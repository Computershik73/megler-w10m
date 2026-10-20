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
 *     finished frame to the shell, which owns the swap chain (meglet.h). On a
 *     SurfaceView's window (ANativeWindow_fromSurface in a Java app) the
 *     surface is the window's own, composed under the app's views
 *     (drivers/mg_java_ndk.c, mg_java_gl.c), and the app's EGLSurface stands
 *     for whichever pbuffer the window has at the time;
 *   - every GLES function that takes a float gets an adapter: the guest passes
 *     floats in integer registers, ANGLE expects them in VFP registers.
 */
#ifndef MG_EGL_H
#define MG_EGL_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The address to bind a guest import of an EGL or GLES function to (an adapter, or ANGLE's own), or NULL. Loads ANGLE on first use. */
void *mg_egl_resolve(const char *name);
/* Whether a name is one this module answers for, without loading anything (for reports). */
bool mg_egl_is_gl_name(const char *name);
/*
 * The same EGL and OpenGL ES for the core's own callers (an app's Java, drivers/mg_java_gl.c): what a guest is bound to,
 * except that a GLES function with floats among its parameters is ANGLE's own, not its adapter. A caller compiled for
 * this host passes floats where the host's convention puts them, which is what ANGLE takes; the adapters are for guest
 * code, which passes them in integer registers. The texture policy (mg_gltex.c) is the same for both. No tracing thunk
 * ("trace-gl"): those are for guest calls. NULL when there is no such function.
 */
void *mg_egl_host(const char *name);

/*
 * Where EGL and GLES come from. By default ANGLE's two DLLs in the package;
 * tests give a function that returns their own stand-ins. NULL from it means
 * "no such function".
 */
void mg_egl_set_provider(void *(*resolve)(const char *name));

/*
 * A pbuffer's DXGI keyed mutex, which ANGLE shares its pbuffers' textures with (NULL when it has none), and that mutex acquired
 * or released by ANGLE's device. Every pbuffer made through this layer is acquired when it is made (Direct3D drops what a
 * device does with such a texture without it); mg_java_gl.c lets go of one while the shell copies a frame out of it.
 */
void *mg_egl_surface_mutex(void *display, void *surface);
bool mg_egl_keyed_mutex(void *mutex, bool acquire);

/*
 * The display eglGetDisplay gives code in the library of that name: a display of its own for the WebView's Chromium
 * (libwebviewchromium.so), whose GPU thread draws while the app's threads do, and ANGLE's Direct3D 11 renderer does not
 * take two contexts of one display drawing at once; the app's for any other (NULL included). And the WebView's display
 * itself, made on first use; NULL when ANGLE cannot make one apart from the app's.
 */
void *mg_egl_display_for_library(const char *name);
void *mg_egl_webview_display(void);

/* What ANGLE gives on this device -- display, Direct3D feature level, GL version and limits -- a line at a time. */
/* Why ANGLE's Direct3D device is gone (an HRESULT), or 0 while it is there or there is none. */
u32 mg_egl_device_removed(void);
/* The graphics memory ANGLE's device (the app's display) has used, and its budget, in MB; false while there is no device. */
bool mg_egl_gpu_memory(u32 *used_mb, u32 *budget_mb);
void mg_egl_probe(void (*line)(const char *fmt, ...));

/*
 * A picture for an "external" texture of the app's (a SurfaceTexture's): on
 * the app's drawing thread, with its context current. The picture goes into
 * an ordinary texture of this layer's, and the app's texture is made an EGL
 * image of that one (the first time, and whenever the size changes). `helper`,
 * `image` and the sizes are the caller's to keep between calls, zero at first.
 * What the app had bound is as it was afterwards.
 */
bool mg_egl_external_upload(u32 external, u32 *helper, void **image, int *helper_w, int *helper_h, const u8 *rgba, int w, int h);

/*
 * Hardware buffers' textures (ndk/mg_ahb.c): an EGL image of a buffer's texture, made with the buffer's pixels the first
 * time (NULL when none can be); the texture read back into the buffer's pixels; the pixels uploaded to it; the texture
 * deleted. Each on a thread of this layer's that keeps a context of its own current.
 */
struct mg_ahb;
void *mg_egl_ahb_image(struct mg_ahb *b, void *display);
bool mg_egl_ahb_read(struct mg_ahb *b);
bool mg_egl_ahb_upload(struct mg_ahb *b);
void mg_egl_ahb_forget(struct mg_ahb *b);

/* mg_gltex.c: the wrapper a texture-making function goes through (or `host` itself for any other), and what has been asked for so far. */
void *mg_gltex_wrap(const char *name, void *host);
void mg_gltex_report(char *out, size_t size);
/* What the app's textures held now come to as ANGLE keeps them (the levels made, less the textures deleted). */
u64 mg_gltex_held_bytes(void);
/* Where mg_gltex.c finds the OpenGL functions it calls for itself (ANGLE's own, by name). */
void mg_gltex_set_host(void *(*resolve)(const char *name));
/* RGBA8 pixels (rows `stride` bytes apart) to DXT5 blocks; a width or height that is not a multiple of four is padded with its edge. */
size_t mg_gltex_dxt5_size(u32 w, u32 h);
void mg_gltex_dxt5(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out);
/* An RGBA8 ETC2 EAC block (sixteen bytes) decoded into sixteen pixels, rows of four, top first; and `blocks` of them packed again as as many DXT5 blocks. */
void mg_gltex_etc2_eac_decode(const u8 block[16], u8 px[16][4]);
void mg_gltex_etc2_eac_to_dxt5(const u8 *in, size_t blocks, u8 *out);
/* RGBA8 pixels to BC1 blocks (opaque, four colours), as mg_gltex_dxt5 does DXT5. */
void mg_gltex_bc1(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out);
/* One block of an ETC format (ETC1, ETC2 RGB8, its punch-through kind, RGBA8 EAC, and their sRGB twins) decoded; false for another format. */
bool mg_gltex_etc_decode(u32 format, const u8 *block, u8 px[16][4]);
/*
 * A level of an ETC format, w x h, at half its size (max(1, w/2) x max(1, h/2)): decoded, each 2x2 pixels averaged, packed
 * again as DXT5 (the formats with alpha) or BC1; the bytes written (mg_gltex_etc_halved_size), 0 when it cannot be done.
 */
size_t mg_gltex_etc_halved_size(u32 format, u32 w, u32 h);
size_t mg_gltex_etc_halve(u32 format, const u8 *in, u32 w, u32 h, u8 *out);
/* Back to nothing known (tests): the functions it calls, the formats this OpenGL has, what was counted, the textures held, this thread's bindings. */
void mg_gltex_reset(void);

#ifdef __cplusplus
}
#endif

#endif
