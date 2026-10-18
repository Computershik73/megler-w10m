/*
 * EGL and OpenGL ES for guest code, over ANGLE.
 *
 * Android's EGL and GLES are C APIs like any other and ANGLE implements them
 * over Direct3D 11, so most calls go straight through: a guest import of
 * glDrawArrays is bound to ANGLE's glDrawArrays. What this layer adds:
 *
 *   - the display is made as an ANGLE/Direct3D 11 display, whatever the guest
 *     asked for, falling back to what a feature-level-9_3 phone needs;
 *   - the guest's window is an ANativeWindow, and its window surface is a
 *     pbuffer over a texture the shell provides. eglSwapBuffers hands the
 *     finished frame to the shell, which owns the swap chain (megler.h). On a
 *     SurfaceView's window (ANativeWindow_fromSurface in a Java app) the
 *     surface is the window's own, composed under the app's views
 *     (drivers/hk_java_ndk.c, hk_java_gl.c), and the app's EGLSurface stands
 *     for whichever pbuffer the window has at the time;
 *   - every GLES function that takes a float gets an adapter: the guest passes
 *     floats in integer registers, ANGLE expects them in VFP registers.
 */
#ifndef HK_EGL_H
#define HK_EGL_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The address to bind a guest import of an EGL or GLES function to (an adapter, or ANGLE's own), or NULL. Loads ANGLE on first use. */
void *hk_egl_resolve(const char *name);
/* Whether a name is one this module answers for, without loading anything (for reports). */
bool hk_egl_is_gl_name(const char *name);
/*
 * The same EGL and OpenGL ES for the core's own callers (an app's Java, drivers/hk_java_gl.c): what a guest is bound to,
 * except that a GLES function with floats among its parameters is ANGLE's own, not its adapter. A caller compiled for
 * this host passes floats where the host's convention puts them, which is what ANGLE takes; the adapters are for guest
 * code, which passes them in integer registers. The texture policy (hk_gltex.c) is the same for both. No tracing thunk
 * ("trace-gl"): those are for guest calls. NULL when there is no such function.
 */
void *hk_egl_host(const char *name);

/*
 * Where EGL and GLES come from. By default ANGLE's two DLLs in the package;
 * tests give a function that returns their own stand-ins. NULL from it means
 * "no such function".
 */
void hk_egl_set_provider(void *(*resolve)(const char *name));

/*
 * A pbuffer's DXGI keyed mutex, which ANGLE shares its pbuffers' textures with (NULL when it has none), and that mutex acquired
 * or released by ANGLE's device. Every pbuffer made through this layer is acquired when it is made (Direct3D drops what a
 * device does with such a texture without it); hk_java_gl.c lets go of one while the shell copies a frame out of it.
 */
void *hk_egl_surface_mutex(void *display, void *surface);
bool hk_egl_keyed_mutex(void *mutex, bool acquire);

/*
 * The display eglGetDisplay gives code in the library of that name: a display of its own for the WebView's Chromium
 * (libwebviewchromium.so), whose GPU thread draws while the app's threads do, and ANGLE's Direct3D 11 renderer does not
 * take two contexts of one display drawing at once; the app's for any other (NULL included). And the WebView's display
 * itself, made on first use; NULL when ANGLE cannot make one apart from the app's.
 */
void *hk_egl_display_for_library(const char *name);
void *hk_egl_webview_display(void);

/* What ANGLE gives on this device -- display, Direct3D feature level, GL version and limits -- a line at a time. */
/* Why ANGLE's Direct3D device is gone (an HRESULT), or 0 while it is there or there is none. */
u32 hk_egl_device_removed(void);
void hk_egl_probe(void (*line)(const char *fmt, ...));

/*
 * A picture for an "external" texture of the app's (a SurfaceTexture's): on
 * the app's drawing thread, with its context current. The picture goes into
 * an ordinary texture of this layer's, and the app's texture is made an EGL
 * image of that one (the first time, and whenever the size changes). `helper`,
 * `image` and the sizes are the caller's to keep between calls, zero at first.
 * What the app had bound is as it was afterwards.
 */
bool hk_egl_external_upload(u32 external, u32 *helper, void **image, int *helper_w, int *helper_h, const u8 *rgba, int w, int h);

/*
 * Hardware buffers' textures (ndk/hk_ahb.c): an EGL image of a buffer's texture, made with the buffer's pixels the first
 * time (NULL when none can be); the texture read back into the buffer's pixels; the pixels uploaded to it; the texture
 * deleted. Each on a thread of this layer's that keeps a context of its own current.
 */
struct hk_ahb;
void *hk_egl_ahb_image(struct hk_ahb *b, void *display);
bool hk_egl_ahb_read(struct hk_ahb *b);
bool hk_egl_ahb_upload(struct hk_ahb *b);
void hk_egl_ahb_forget(struct hk_ahb *b);

/* hk_gltex.c: the wrapper a texture-making function goes through (or `host` itself for any other), and what has been asked for so far. */
void *hk_gltex_wrap(const char *name, void *host);
void hk_gltex_report(char *out, size_t size);
/* Where hk_gltex.c finds the OpenGL functions it calls for itself (ANGLE's own, by name). */
void hk_gltex_set_host(void *(*resolve)(const char *name));
/* RGBA8 pixels (rows `stride` bytes apart) to DXT5 blocks; a width or height that is not a multiple of four is padded with its edge. */
size_t hk_gltex_dxt5_size(u32 w, u32 h);
void hk_gltex_dxt5(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out);

#ifdef __cplusplus
}
#endif

#endif
