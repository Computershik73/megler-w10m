/*
 * A stand-in for EGL and OpenGL ES that draws (hk_gl_standin.c), for the tests of OpenGL from an app's Java: given to the
 * core with hk_egl_set_provider, it is what the Java natives and the GL threads call where ANGLE would be.
 *
 * It is no renderer to look at, and it is the same on every machine. A surface is memory, R, G, B, A, rows bottom first, as
 * OpenGL has it. glClear fills the surface with the clear colour; glDrawArrays fills triangles (pixel centres inside) with
 * one colour: the vec4 uniform of the program whose name has "olor" in it, and the positions are the attribute whose name
 * has "osition" in it, two to four floats in clip space, from client memory or a buffer object. Shaders are not compiled:
 * their attributes and uniforms are read out of their source ("attribute vec4 aPosition;", "uniform vec4 uColor;"), every
 * compile and link succeeds and has a log saying so. glReadPixels gives R, G, B, A, or B, G, R, A with GL_BGRA_EXT, which
 * its extensions list. Contexts of OpenGL ES 2 and 3 are made; one of version 1 is refused unless asked for, as an ANGLE
 * without OpenGL ES 1 refuses it. All contexts share their objects. Anything it does not have is "no such function".
 */
#ifndef HK_GL_STANDIN_H
#define HK_GL_STANDIN_H

#include "../src/hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* For hk_egl_set_provider. */
void *hk_gl_standin(const char *name);

typedef struct hk_gl_standin_stats {
    i32 contexts, contexts_alive, surfaces, surfaces_alive;
    i32 clears, draws, reads, swaps;
    i32 refused_es1;                   /* version-1 contexts refused */
    float clear[4];                    /* the last clear colour */
    i32 largest_es;                    /* the newest version a context was made for */
} hk_gl_standin_stats;
void hk_gl_standin_stats_get(hk_gl_standin_stats *out);
/* Back to nothing made and nothing counted; whether GL_EXT_read_format_bgra is listed, and whether version 1 is made. */
void hk_gl_standin_reset(bool bgra, bool es1);
/* The colour of a pixel of the calling thread's current surface as 0xRRGGBBAA, (0, 0) its bottom left; 0 with none. */
u32 hk_gl_standin_pixel(int x, int y);

#ifdef __cplusplus
}
#endif

#endif
