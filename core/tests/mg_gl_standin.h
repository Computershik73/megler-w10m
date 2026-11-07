/*
 * A stand-in for EGL and OpenGL ES that draws (mg_gl_standin.c), for the tests of OpenGL from an app's Java: given to the
 * core with mg_egl_set_provider, it is what the Java natives and the GL threads call where ANGLE would be.
 *
 * It is no renderer to look at, and it is the same on every machine. A surface is memory, R, G, B, A, rows bottom first, as
 * OpenGL has it. glClear fills the surface with the clear colour; glDrawArrays fills triangles (pixel centres inside) with
 * one colour: the vec4 uniform of the program whose name has "olor" in it, and the positions are the attribute whose name
 * has "osition" in it, two to four floats in clip space, from client memory or a buffer object. Shaders are not compiled:
 * their attributes and uniforms are read out of their source ("attribute vec4 aPosition;", "uniform vec4 uColor;"), every
 * compile and link succeeds and has a log saying so. glReadPixels gives R, G, B, A, or B, G, R, A with GL_BGRA_EXT, which
 * its extensions list. Contexts of OpenGL ES 2 and 3 are made; one of version 1 is refused unless asked for, as an ANGLE
 * without OpenGL ES 1 refuses it. All contexts share their objects. A texture keeps its pixels (glTexImage2D and
 * glTexSubImage2D of RGBA, RGB and RGB 5-6-5); an EGL image of a texture (EGL_KHR_gl_texture_2D_image, which its EGL lists)
 * shares them with a texture made of it (glEGLImageTargetTexture2DOES); a framebuffer object with a texture for its colour
 * is what glClear fills and glReadPixels reads. Anything it does not have is "no such function".
 */
#ifndef MG_GL_STANDIN_H
#define MG_GL_STANDIN_H

#include "../src/mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* For mg_egl_set_provider. */
void *mg_gl_standin(const char *name);
/*
 * EGL_ANDROID_blob_cache, which the stand-in lists: whether the layer gave it its functions, and those functions tried --
 * a value kept and found again, its size alone when asked with no room, nothing for another key, a value kept again
 * replacing it. 0 when they did all that (or none was given); otherwise which of them failed.
 */
bool mg_gl_standin_blob_given(void);
int mg_gl_standin_blob_check(void);

typedef struct mg_gl_standin_stats {
    i32 contexts, contexts_alive, surfaces, surfaces_alive;
    i32 clears, draws, reads, swaps;
    i32 refused_es1;                   /* version-1 contexts refused */
    float clear[4];                    /* the last clear colour */
    i32 largest_es;                    /* the newest version a context was made for */
    i32 dropped;                       /* clears, draws and reads of a surface whose keyed mutex was not held: dropped */
    i32 makes;                         /* eglMakeCurrent calls */
    i32 overlaps;                      /* glClear calls that came while another thread's was still in the stand-in */
    i32 stale;                         /* glClear calls of a context other than the one any thread made current last */
} mg_gl_standin_stats;
void mg_gl_standin_stats_get(mg_gl_standin_stats *out);
/*
 * How long each glClear stays in the stand-in (0: no time): two threads that clear at once then overlap, unless their calls
 * take turns. Like ANGLE's renderer, the stand-in has one notion of the context whose state is set, the one made current
 * last on any thread, and a clear of another context's counts as stale.
 */
void mg_gl_standin_set_dwell(u64 ns);
/* Back to nothing made and nothing counted; whether GL_EXT_read_format_bgra is listed, and whether version 1 is made. */
void mg_gl_standin_reset(bool bgra, bool es1);
/*
 * The graphics processor the stand-in plays, as a Direct3D feature level (0x9100): below 0x9300 -- a Surface RT's or a
 * Surface 2's 9_1 -- eglInitialize makes a display only where it was asked for on WARP (EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE
 * of EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE), as ANGLE there; 0 (as after mg_gl_standin_reset), any display. And the
 * displays asked for since the last time this was asked, and how many of them on WARP.
 */
void mg_gl_standin_set_level(int level);
void mg_gl_standin_displays(int *asked, int *warp);
/*
 * The complete mode: a driver of OpenGL ES 2.0 alone in which every function of EGL 1.4 and OpenGL ES 2.0 is there, those it
 * does not draw with doing nothing (mg_gl_standin.c, "complete mode"), for an app with a GL stack of its own over the
 * system's: the WebView's Chromium. Off unless asked for, and left as it is by mg_gl_standin_reset.
 */
void mg_gl_standin_set_complete(bool on);
/* The colour of a pixel of the calling thread's current surface as 0xRRGGBBAA, (0, 0) its bottom left; 0 with none. */
u32 mg_gl_standin_pixel(int x, int y);
/*
 * A surface by what eglQuerySurfacePointerANGLE gave as its texture's shared handle (EGL_ANGLE_surface_d3d_texture_2d_share_handle,
 * which the stand-in lists): its memory, R, G, B, A, rows bottom first, and its size. For a test's shell that composes the
 * window from the surfaces' frames (megler_gfx_host.gl_frame).
 *
 * Every surface has a keyed mutex, as every pbuffer of gecko-w10m's ANGLE does (EGL_DXGI_KEYED_MUTEX_ANGLE through
 * eglQuerySurfacePointerANGLE), released with key 0 when it is made. The stand-in clears, draws into and reads a surface only
 * while its own "device" holds that mutex (the core's mg_egl_keyed_mutex, through the provider's name meglerKeyedMutexANGLE),
 * and otherwise drops what is asked, as Direct3D does: a core that never acquires it gets no frames. A test's shell takes it
 * with mg_gl_standin_keyed before it copies a frame and gives it back after, as the shell's AcquireSync and ReleaseSync do;
 * false when it is held by the other side.
 */
bool mg_gl_standin_keyed(void *share_handle, bool acquire);
/* The display the last EGL image was asked for on. Displays: 0xD15, and 0xD16 for the native display -3 (EGL_D3D11_ONLY_DISPLAY_ANGLE). */
void *mg_gl_standin_image_display(void);
/* The features of ANGLE's the app's display was asked to be made without, by name, separated by spaces ("" for none). */
const char *mg_gl_standin_app_features_off(void);
bool mg_gl_standin_shared(void *share_handle, const u8 **rgba, int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif
