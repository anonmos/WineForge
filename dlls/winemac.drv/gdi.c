/*
 * Mac graphics driver initialisation functions
 *
 * Copyright 1996 Alexandre Julliard
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "macdrv.h"
#include "winreg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/file.h>
#include <unistd.h>

WINE_DEFAULT_DEBUG_CHANNEL(macdrv);


typedef struct
{
    struct gdi_physdev  dev;
} MACDRV_PDEVICE;

static inline MACDRV_PDEVICE *get_macdrv_dev(PHYSDEV dev)
{
    return (MACDRV_PDEVICE*)dev;
}


/* a few dynamic device caps */
static CGRect desktop_rect;     /* virtual desktop rectangle */
static int horz_size;           /* horz. size of screen in millimeters */
static int vert_size;           /* vert. size of screen in millimeters */
static bool device_data_valid;  /* do the above variables have up-to-date values? */

bool retina_on = false;

static pthread_mutex_t device_data_mutex = PTHREAD_MUTEX_INITIALIZER;


#define WSCF_HEADER_SIZE 32
#define WSCF_MAX_FRAME_BYTES (64 * 1024 * 1024)

static UINT32 read_wscf_le32( const unsigned char *p )
{
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static UINT64 read_wscf_le64( const unsigned char *p )
{
    return (UINT64)read_wscf_le32( p ) | ((UINT64)read_wscf_le32( p + 4 ) << 32);
}

static void macdrv_close_capture_file( FILE *file )
{
    flock( fileno(file), LOCK_UN );
    fclose( file );
}

static void macdrv_free_image_bits( struct gdi_image_bits *bits )
{
    free( bits->ptr );
    bits->ptr = NULL;
}

static void macdrv_log_getimage( unsigned int call, const char *stage,
                                 const struct bitblt_coords *src, UINT32 width,
                                 UINT32 height, DWORD status )
{
    const char *dbg = getenv( "WINE_SCREEN_CAPTURE_DEBUG" );
    FILE *log;
    if (!dbg || !*dbg || *dbg == '0') return;
    if (call > 8 && call % 120) return;
    if (!(log = fopen( "/private/tmp/wine-sck-gdi-pgetimage.log", "a" ))) return;
    if (!flock( fileno(log), LOCK_EX ))
    {
        fprintf( log, "%lld pid=%d call=%u %s src=%d,%d %dx%d vis=%d,%d,%d,%d image=%ux%u status=%lu\n",
                 (long long)time(NULL), (int)getpid(), call, stage,
                 src ? src->x : 0, src ? src->y : 0,
                 src ? src->width : 0, src ? src->height : 0,
                 src ? src->visrect.left : 0, src ? src->visrect.top : 0,
                 src ? src->visrect.right : 0, src ? src->visrect.bottom : 0,
                 width, height, (unsigned long)status );
        fflush( log );
        flock( fileno(log), LOCK_UN );
    }
    fclose( log );
}

/* Optionally save the first successful frame returned to a GDI caller. */
static void macdrv_dump_capture_frame( UINT32 width, UINT32 height,
                                       const unsigned char *pixels, size_t stride )
{
    const char *dir = getenv( "WINE_SCREEN_CAPTURE_DEBUG_DUMP_DIR" );
    static int dumped;
    char path[1024];
    unsigned char *row;
    FILE *file;
    UINT y, x;
    UINT64 nonblack = 0, total = (UINT64)width * height;
    UINT64 sums[3] = {0, 0, 0};

    if (!dir || !*dir || __atomic_exchange_n( &dumped, 1, __ATOMIC_RELAXED )) return;
    if (snprintf( path, sizeof(path), "%s/wine-gdi-frame-%d.ppm", dir, (int)getpid() ) >= sizeof(path)) return;
    if (!(file = fopen( path, "wb" ))) return;
    if (!(row = malloc( (size_t)width * 3 ))) { fclose( file ); return; }
    fprintf( file, "P6\n%u %u\n255\n", width, height );
    for (y = 0; y < height; y++)
    {
        for (x = 0; x < width; x++)
        {
            const unsigned char *src = pixels + (size_t)y * stride + x * 4;
            unsigned char r = src[2], g = src[1], b = src[0];
            row[x * 3] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
            if (r || g || b) nonblack++;
            sums[0] += r;
            sums[1] += g;
            sums[2] += b;
        }
        fwrite( row, 3, width, file );
    }
    free( row );
    fclose( file );
    {
        FILE *log = fopen( "/private/tmp/wine-sck-gdi-pixels.log", "a" );
        if (log)
        {
            if (!flock( fileno(log), LOCK_EX ))
            {
                fprintf( log, "pid=%d file=%s size=%ux%u nonblack=%llu/%llu mean_rgb=%.1f,%.1f,%.1f\n",
                         (int)getpid(), path, width, height,
                         (unsigned long long)nonblack, (unsigned long long)total,
                         total ? (double)sums[0] / total : 0.0,
                         total ? (double)sums[1] / total : 0.0,
                         total ? (double)sums[2] / total : 0.0 );
                fflush( log );
                flock( fileno(log), LOCK_UN );
            }
            fclose( log );
        }
    }
}

/* Read the latest top-down BGRA frame published by the ScreenCaptureKit helper. */
static DWORD macdrv_GetImage( PHYSDEV dev, BITMAPINFO *info,
                              struct gdi_image_bits *bits, struct bitblt_coords *src )
{
    const char *path = getenv( "WINE_SCREEN_CAPTURE_FRAME" );
    unsigned char header[WSCF_HEADER_SIZE];
    unsigned char *captured = NULL, *pixels = NULL;
    UINT32 width, height, stride;
    UINT64 timestamp_ns, now_ns;
    size_t capture_size, output_stride, output_size;
    struct timespec now;
    FILE *file = NULL;
    UINT y;
    static unsigned int call_counter;
    unsigned int call = __atomic_add_fetch( &call_counter, 1, __ATOMIC_RELAXED );

    (void)dev;
    macdrv_log_getimage( call, "enter", src, 0, 0, ERROR_SUCCESS );

    if (!path || !*path) path = "/private/tmp/wine-sck-probe/latest-frame.wscf";
    if (!(file = fopen( path, "rb" )))
    {
        macdrv_log_getimage( call, "open-failed", src, 0, 0, ERROR_NOT_SUPPORTED );
        return ERROR_NOT_SUPPORTED;
    }
    if (flock( fileno(file), LOCK_SH )) { fclose( file ); return ERROR_INVALID_DATA; }
    if (fread( header, 1, sizeof(header), file ) != sizeof(header) ||
        memcmp( header, "WSCF", 4 ) || read_wscf_le32( header + 4 ) != 1)
    {
        macdrv_close_capture_file( file );
        macdrv_log_getimage( call, "bad-header", src, 0, 0, ERROR_INVALID_DATA );
        return ERROR_INVALID_DATA;
    }

    width = read_wscf_le32( header + 8 );
    height = read_wscf_le32( header + 12 );
    stride = read_wscf_le32( header + 16 );
    timestamp_ns = read_wscf_le64( header + 24 );
    if (!width || !height || width > 16384 || height > 16384 ||
        stride < width * 4 || (UINT64)stride * height > WSCF_MAX_FRAME_BYTES)
    {
        macdrv_close_capture_file( file );
        macdrv_log_getimage( call, "bad-dimensions", src, width, height, ERROR_INVALID_DATA );
        return ERROR_INVALID_DATA;
    }

    clock_gettime( CLOCK_REALTIME, &now );
    now_ns = (UINT64)now.tv_sec * 1000000000ULL + now.tv_nsec;
    if (now_ns < timestamp_ns || now_ns - timestamp_ns > 1500000000ULL)
    {
        macdrv_close_capture_file( file );
        macdrv_log_getimage( call, "stale", src, width, height, ERROR_NOT_SUPPORTED );
        return ERROR_NOT_SUPPORTED;
    }

    capture_size = (size_t)stride * height;
    output_stride = (size_t)width * 4;
    output_size = output_stride * height;
    if (!(captured = malloc( capture_size )) || !(pixels = malloc( output_size )))
    {
        free( captured );
        free( pixels );
        macdrv_close_capture_file( file );
        macdrv_log_getimage( call, "out-of-memory", src, width, height, ERROR_OUTOFMEMORY );
        return ERROR_OUTOFMEMORY;
    }
    if (fread( captured, 1, capture_size, file ) != capture_size)
    {
        free( captured );
        free( pixels );
        macdrv_close_capture_file( file );
        macdrv_log_getimage( call, "short-read", src, width, height, ERROR_INVALID_DATA );
        return ERROR_INVALID_DATA;
    }
    macdrv_close_capture_file( file );

    for (y = 0; y < height; y++)
        memcpy( pixels + y * output_stride, captured + y * stride, output_stride );
    free( captured );
    macdrv_dump_capture_frame( width, height, pixels, output_stride );

    memset( &info->bmiHeader, 0, sizeof(info->bmiHeader) );
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = width;
    info->bmiHeader.biHeight = -(LONG)height;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = output_size;

    bits->ptr = pixels;
    bits->is_copy = TRUE;
    bits->free = macdrv_free_image_bits;
    bits->param = NULL;
    macdrv_log_getimage( call, "success", src, width, height, ERROR_SUCCESS );
    return ERROR_SUCCESS;
}

static const struct user_driver_funcs macdrv_funcs;

/***********************************************************************
 *              compute_desktop_rect
 */
static void compute_desktop_rect(void)
{
    CGDirectDisplayID displayIDs[32];
    uint32_t count, i;

    desktop_rect = CGRectNull;
    if (CGGetOnlineDisplayList(ARRAY_SIZE(displayIDs), displayIDs, &count) != kCGErrorSuccess ||
        !count)
    {
        displayIDs[0] = CGMainDisplayID();
        count = 1;
    }

    for (i = 0; i < count; i++)
        desktop_rect = CGRectUnion(desktop_rect, CGDisplayBounds(displayIDs[i]));
    desktop_rect = cgrect_win_from_mac(desktop_rect);
}


/***********************************************************************
 *              macdrv_get_desktop_rect
 *
 * Returns the rectangle encompassing all the screens.
 */
CGRect macdrv_get_desktop_rect(void)
{
    CGRect ret;

    pthread_mutex_lock(&device_data_mutex);

    if (!device_data_valid)
    {
        check_retina_status();
        compute_desktop_rect();
    }
    ret = desktop_rect;

    pthread_mutex_unlock(&device_data_mutex);

    TRACE("%s\n", wine_dbgstr_cgrect(ret));

    return ret;
}


/**********************************************************************
 *              device_init
 *
 * Perform initializations needed upon creation of the first device.
 */
static void device_init(void)
{
    CGDirectDisplayID mainDisplay = CGMainDisplayID();
    CGSize size_mm = CGDisplayScreenSize(mainDisplay);

    check_retina_status();

    /* Initialize device caps */
    horz_size = size_mm.width;
    vert_size = size_mm.height;

    compute_desktop_rect();

    device_data_valid = true;
}


void macdrv_reset_device_metrics(void)
{
    pthread_mutex_lock(&device_data_mutex);
    device_data_valid = false;
    pthread_mutex_unlock(&device_data_mutex);
}


static MACDRV_PDEVICE *create_mac_physdev(void)
{
    MACDRV_PDEVICE *physDev;

    pthread_mutex_lock(&device_data_mutex);
    if (!device_data_valid) device_init();
    pthread_mutex_unlock(&device_data_mutex);

    if (!(physDev = calloc(1, sizeof(*physDev)))) return NULL;

    return physDev;
}


/**********************************************************************
 *              CreateDC (MACDRV.@)
 */
static BOOL macdrv_CreateDC(PHYSDEV *pdev, LPCWSTR device, LPCWSTR output, const DEVMODEW* initData)
{
    MACDRV_PDEVICE *physDev = create_mac_physdev();

    TRACE("pdev %p hdc %p device %s output %s initData %p\n", pdev,
          (*pdev)->hdc, debugstr_w(device), debugstr_w(output), initData);

    if (!physDev) return FALSE;

    push_dc_driver(pdev, &physDev->dev, &macdrv_funcs.dc_funcs);
    return TRUE;
}


/**********************************************************************
 *              CreateCompatibleDC (MACDRV.@)
 */
static BOOL macdrv_CreateCompatibleDC(PHYSDEV orig, PHYSDEV *pdev)
{
    MACDRV_PDEVICE *physDev = create_mac_physdev();

    TRACE("orig %p orig->hdc %p pdev %p pdev->hdc %p\n", orig, (orig ? orig->hdc : NULL), pdev,
          ((pdev && *pdev) ? (*pdev)->hdc : NULL));

    if (!physDev) return FALSE;

    push_dc_driver(pdev, &physDev->dev, &macdrv_funcs.dc_funcs);
    return TRUE;
}


/**********************************************************************
 *              DeleteDC (MACDRV.@)
 */
static BOOL macdrv_DeleteDC(PHYSDEV dev)
{
    MACDRV_PDEVICE *physDev = get_macdrv_dev(dev);

    TRACE("hdc %p\n", dev->hdc);

    free(physDev);
    return TRUE;
}


/***********************************************************************
 *              GetDeviceCaps (MACDRV.@)
 */
static INT macdrv_GetDeviceCaps(PHYSDEV dev, INT cap)
{
    INT ret;

    pthread_mutex_lock(&device_data_mutex);

    if (!device_data_valid) device_init();

    switch(cap)
    {
    case HORZSIZE:
        ret = horz_size;
        break;
    case VERTSIZE:
        ret = vert_size;
        break;
    case HORZRES:
    case VERTRES:
    default:
        pthread_mutex_unlock(&device_data_mutex);
        dev = GET_NEXT_PHYSDEV( dev, pGetDeviceCaps );
        ret = dev->funcs->pGetDeviceCaps( dev, cap );
        if ((cap == HORZRES || cap == VERTRES) && retina_on)
            ret *= 2;
        return ret;
    }

    TRACE("cap %d -> %d\n", cap, ret);

    pthread_mutex_unlock(&device_data_mutex);
    return ret;
}


static const struct user_driver_funcs macdrv_funcs =
{
    .dc_funcs.pCreateCompatibleDC = macdrv_CreateCompatibleDC,
    .dc_funcs.pCreateDC = macdrv_CreateDC,
    .dc_funcs.pDeleteDC = macdrv_DeleteDC,
    .dc_funcs.pGetDeviceCaps = macdrv_GetDeviceCaps,
    .dc_funcs.pGetDeviceGammaRamp = macdrv_GetDeviceGammaRamp,
    .dc_funcs.pGetImage = macdrv_GetImage,
    .dc_funcs.pSetDeviceGammaRamp = macdrv_SetDeviceGammaRamp,
    .dc_funcs.priority = GDI_PRIORITY_GRAPHICS_DRV,

    .pActivateKeyboardLayout = macdrv_ActivateKeyboardLayout,
    .pBeep = macdrv_Beep,
    .pChangeDisplaySettings = macdrv_ChangeDisplaySettings,
    .pClipCursor = macdrv_ClipCursor,
    .pNotifyIcon = macdrv_NotifyIcon,
    .pCleanupIcons = macdrv_CleanupIcons,
    .pClipboardWindowProc = macdrv_ClipboardWindowProc,
    .pDesktopWindowProc = macdrv_DesktopWindowProc,
    .pDestroyCursorIcon = macdrv_DestroyCursorIcon,
    .pDestroyWindow = macdrv_DestroyWindow,
    .pUpdateDisplayDevices = macdrv_UpdateDisplayDevices,
    .pGetCursorPos = macdrv_GetCursorPos,
    .pGetKeyboardLayoutList = macdrv_GetKeyboardLayoutList,
    .pGetKeyNameText = macdrv_GetKeyNameText,
    .pMapVirtualKeyEx = macdrv_MapVirtualKeyEx,
    .pProcessEvents = macdrv_ProcessEvents,
    .pRegisterHotKey = macdrv_RegisterHotKey,
    .pSetCapture = macdrv_SetCapture,
    .pSetCursor = macdrv_SetCursor,
    .pSetCursorPos = macdrv_SetCursorPos,
    .pSetDesktopWindow = macdrv_SetDesktopWindow,
    .pActivateWindow = macdrv_ActivateWindow,
    .pSetLayeredWindowAttributes = macdrv_SetLayeredWindowAttributes,
    .pSetParent = macdrv_SetParent,
    .pSetWindowRgn = macdrv_SetWindowRgn,
    .pSetWindowStyle = macdrv_SetWindowStyle,
    .pSetWindowText = macdrv_SetWindowText,
    .pShowWindow = macdrv_ShowWindow,
    .pSysCommand =macdrv_SysCommand,
    .pSystemParametersInfo = macdrv_SystemParametersInfo,
    .pThreadDetach = macdrv_ThreadDetach,
    .pToUnicodeEx = macdrv_ToUnicodeEx,
    .pUnregisterHotKey = macdrv_UnregisterHotKey,
    .pUpdateClipboard = macdrv_UpdateClipboard,
    .pUpdateLayeredWindow = macdrv_UpdateLayeredWindow,
    .pVkKeyScanEx = macdrv_VkKeyScanEx,
    .pImeToAsciiEx = macdrv_ImeToAsciiEx,
    .pNotifyIMEStatus = macdrv_NotifyIMEStatus,
    .pSetIMECompositionRect = macdrv_SetIMECompositionRect,
    .pWindowMessage = macdrv_WindowMessage,
    .pWindowPosChanged = macdrv_WindowPosChanged,
    .pWindowPosChanging = macdrv_WindowPosChanging,
    .pGetWindowStyleMasks = macdrv_GetWindowStyleMasks,
    .pCreateClientSurface = macdrv_CreateClientSurface,
    .pCreateWindowSurface = macdrv_CreateWindowSurface,
    .pVulkanInit = macdrv_VulkanInit,
    .pOpenGLInit = macdrv_OpenGLInit,
};


void init_user_driver(void)
{
    __wine_set_user_driver( &macdrv_funcs, WINE_GDI_DRIVER_VERSION );
}
