/* The tests of the drawing under a Java app: the stand-in for the shell's side, and megler.Gfx's native methods on top of it. */
#ifndef HK_GFX_TESTS_H
#define HK_GFX_TESTS_H

#include "hk_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The stand-in (hk_gfx_standin.c), through the megler_gfx_host struct and
 * nothing else: what it draws for the simple cases of megler.h's contract, its
 * text of boxes, PNG files read and written. Needs no runtime. Returns the
 * number of failed checks and adds the number made to *checks.
 */
int hk_gfx_tests_run(hk_test_sink sink, void *user, int *checks);
/*
 * A folder the tests may write in: a BMP written and read back, removed again,
 * and hk_gfx_scene.bmp, the picture of one test, left there to be looked at.
 * Without a folder the writing of a BMP is not tested, and that is said.
 */
void hk_gfx_tests_set_scratch(const char *dir);

/*
 * megler.Gfx's native methods (core/src/drivers/hk_java_gfx.c), called as the
 * runtime calls them, with the stand-in as the shell: bitmaps and their
 * pixels both ways, the canvas's bookkeeping, paints and paths unpacked,
 * what reaches the shell and what comes back. It makes its arrays and
 * strings with the Dalvik runtime's own functions, so the runtime has to be
 * running; where it is not, nothing is checked and that is said. It sets the
 * natives' host and their present function, and leaves both unset: run it
 * before an app starts or after it has ended, not while one runs.
 */
int hk_gfx_natives_tests_run(hk_test_sink sink, void *user, int *checks);

/*
 * OpenGL ES and EGL from an app's Java (core/src/drivers/hk_java_gl.c, hk_java_gles.c) the same way, with the stand-in
 * that draws (hk_gl_standin.c) in ANGLE's place: that every GLES native the framework declares is bound, GLES20's natives
 * with arrays, buffers and strings, megler.EGLNative's contexts and surfaces, the window composed of the views and the
 * frames of a surface under them, and what a swap costs at a phone's size. Needs the runtime running; the buffers need
 * libcore. It gives the core the stand-in and gives the core back no OpenGL ES at all at its end (hk_egl_set_provider
 * NULL): not while an app runs.
 */
int hk_gl_natives_tests_run(hk_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
