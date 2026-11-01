/*
 * OpenGL ES and EGL for an app's Java (docs/ARCHITECTURE.md, "OpenGL ES from an app's Java"): the GLES classes' native
 * methods (mg_java_gles.c, generated, and mg_java_gl.c, by hand), megler.EGLNative, the GL thread of a GLSurfaceView
 * (megler.Native.startGL), and the window composed of the views' picture over the frames of GL surfaces.
 */
#ifndef MG_JAVA_GL_H
#define MG_JAVA_GL_H

#include "../../include/megler.h"
#include "../mg_base.h"
#include "../jni/mg_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------- for the drivers */

/*
 * How a picture reaches the shell (mg_java.c): the whole window, of which `part` (left, top, right, bottom) is what
 * changed since the last one, or all of it when part[2] is 0. On whichever thread presents, one call at a time.
 */
typedef void (*mg_java_show_fn)(const void *pixels, int width, int height, int stride, const i32 part[4]);
/* At the driver's start, before the app runs: the window's size, and the way to the shell. */
void mg_java_gl_start(int width, int height, mg_java_show_fn show);
/*
 * The views' picture is about to be shown (the app's main thread; `part` as above). True when an EGL window surface has
 * been made in this app: the picture was then composed with the GL frames and shown from here, and must not be shown
 * again. False: no GL, nothing done, show it as it is.
 */
bool mg_java_gl_views(const void *pixels, int width, int height, int stride, const i32 part[4]);
/*
 * Whether the window is composed here (an EGL window surface has been made, and the shell does not compose the window):
 * its views' picture is then needed in memory.
 */
bool mg_java_gl_composing(void);
/* The shell's drawing, which composes the window itself where it can (megler_gfx_host.gl_frame); before the app runs. */
void mg_java_gl_set_shell(const megler_gfx_host *gfx);
/* The shell's drawing it was given; NULL before. */
const megler_gfx_host *mg_java_gl_shell(void);
/*
 * The views' picture is about to be shown by the shell from its own (megler_gfx_host.present; the app's main thread): where the
 * shell composes the window, the places the views said for their surfaces are told it first.
 */
void mg_java_gl_views_shown(void);
/* megler.EGLNative, megler.Native's startGL and requestRender, and the GLES natives written by hand. */
const mg_jhle *mg_java_gl_natives(void);

/* ------------------------------------------------- for the NDK's windows */

/*
 * EGL window surfaces native code makes on a SurfaceView's window (ANativeWindow_fromSurface, mg_java_ndk.c; eglCreateWindowSurface,
 * ndk/mg_egl.c): a pbuffer width x height on the view's layer, its frames shown where the view is and paced as those of a window
 * surface made from Java on the view are; the view is told it is drawn with EGL (its hole). With no view (a SurfaceTexture's
 * window) a pbuffer whose frames are read back for its consumer. Made with the first of `configs` (EGL configurations) that EGL
 * makes a pbuffer with; NULL when none.
 */
void *mg_java_gl_window_surface(mg_jobj *view, int width, int height, void *const *configs, int count);
/* The pbuffer it is now (EGL's surface, which the app's stands for) and its size; either pointer may be NULL. */
void *mg_java_gl_window_handle(void *surface, int *width, int *height);
/*
 * A swap of it on the calling thread, where it is current in whatever context the app made: the frame shown under the view, or
 * on no view read back (mg_java_gl_window_pixels); paced. Then, if the window's buffers are now width x height and the pbuffer
 * is not, it is a new pbuffer of that size from here on (the old one destroyed): the caller makes the new one current in the
 * old one's place.
 */
void mg_java_gl_window_swap(void *surface, int width, int height);
/*
 * The same, on no view, for a texture a TextureView shows under `texture_id` (1 and up; 0: none): the frame handed to the shell
 * as that id's picture on the graphics processor (megler_gfx_host.texture_frame), nothing read back. False: it was read back as
 * by mg_java_gl_window_swap (the shell does not keep pictures so, or refused this one).
 */
bool mg_java_gl_window_swap_texture(void *surface, int width, int height, int texture_id);
/*
 * A pbuffer's frame handed to the shell as the picture of `id` (megler_gfx_host.texture_frame), on the thread that drew it, in
 * whatever context: finished (flushed, where a keyed mutex orders the two devices' work), the texture's keyed mutex let go of
 * for the call and taken back, as a layer's frame is handed over. `display` is the pbuffer's (the app's, or the WebView's:
 * drivers/mg_java_webview.c); `share` what was asked of the pbuffer before, all zero for a new one, its texture's shared handle
 * and keyed mutex asked for the first time. False: the shell does not keep pictures so, or refused this one.
 */
typedef struct mg_java_gl_share {
    bool asked;
    void *handle, *mutex;
} mg_java_gl_share;
bool mg_java_gl_share_frame(void *display, void *pbuffer, mg_java_gl_share *share, int id, int width, int height);
/* Whether the shell keeps pictures so (megler_gfx_host.texture_frame and draw_texture) and EGL shares pbuffers' textures. */
bool mg_java_gl_shares_frames(void);
/* The shell's picture of `id` let go. */
void mg_java_gl_share_gone(int id);
/* On no view: the frame the last swap read back, R, G, B, A, rows bottom first, and its size; NULL before the first. */
const void *mg_java_gl_window_pixels(void *surface, int *width, int *height);
void mg_java_gl_window_destroy(void *surface);
/*
 * The flag trace-surfaces, where the shell shows the views from its own picture (megler_gfx_host.present): whether the views'
 * picture is due to be looked at over the OpenGL layers (every three seconds), and then the picture, brought up to date in
 * memory for it (mg_java_gfx.c's present).
 */
bool mg_java_gl_views_probe_due(void);
void mg_java_gl_views_probe(const void *pixels, int width, int height, int stride);
/* mg_java_ndk.c: the view's surface has gone (EGLNative.forget): the windows native code has of it are no longer valid. */
void mg_java_ndk_view_gone(mg_jobj *view);

/* mg_java_gles.c (generated by tools/gles-natives.py): the GLES natives that are a plain call of their function. */
const mg_jhle *mg_java_gles_natives(void);
/* Every native method the framework's GLES classes declare (fn NULL): for the test that counts what is bound. */
const mg_jhle *mg_java_gles_declared(void);
/* Every OpenGL ES function is looked up again at its next call: for a test that gives the core another OpenGL ES. */
void mg_java_gles_forget(void);

/* What OpenGL frames have cost so far, for the tests and the log: all times in nanoseconds, summed. */
typedef struct mg_java_gl_stats {
    u32 swaps, shown;                  /* swaps of window surfaces; frames of them shown composed */
    u64 read_ns, compose_ns, show_ns;  /* reading back; composing on the CPU; the shell's present */
    u64 longest_ns;                    /* the longest swap's own work */
    u32 threads;                       /* GL threads started (GLSurfaceView) */
} mg_java_gl_stats;
void mg_java_gl_get_stats(mg_java_gl_stats *out);
/* For a test that gives the core another OpenGL ES (mg_egl_set_provider): EGL and every function are looked up again. */
void mg_java_gl_reset(void);

/* ------------------------------------------- for mg_java_gles.c's natives */

/*
 * The function to call for an OpenGL ES name: ANGLE's own (mg_egl_host), looked up the first time and kept in *slot.
 * NULL when this OpenGL ES has no such function, which is said once in the log; the call then does nothing.
 */
void *mg_java_gl_fn(void **slot, const char *name);
/*
 * Parameter k is an array and k + 1 the offset into it: the memory from that element on, elements `esize` bytes. False,
 * with IllegalArgumentException pending, for a null array or an offset outside it.
 */
bool mg_java_gl_array(mg_jcall *c, int k, int esize, void **out);
/*
 * Parameter k is a java.nio buffer: its memory from its position on (NIOAccess's answer); NULL for null, unless the memory
 * is `needed` (C writes it). False, with IllegalArgumentException pending, for a buffer that is neither direct nor over
 * an array, or null where it is needed.
 */
bool mg_java_gl_buffer(mg_jcall *c, int k, bool needed, void **out);
/* Parameter k is a String: its UTF-8. False, with IllegalArgumentException pending, for null. */
bool mg_java_gl_string(mg_jcall *c, int k, const char **out);

#ifdef __cplusplus
}
#endif

#endif
