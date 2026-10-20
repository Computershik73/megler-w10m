/*
 * A stand-in for the shell's drawing (megler_gfx_host), for tests: portable C
 * that draws straight into the bitmaps' memory and gives the same pixels on
 * every machine. It is not a renderer to look at: mg_gfx_standin.c says what
 * it leaves out and what it draws in place of the real thing (text is boxes).
 */
#ifndef MG_GFX_STANDIN_H
#define MG_GFX_STANDIN_H

#include "../include/megler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The host, every function of it filled in but `present`; `user` is not used. */
const megler_gfx_host *mg_gfx_standin(void);

/*
 * The memory a canvas of the stand-in's draws into, which always holds its picture: for a test that shows a canvas's
 * picture as a shell with megler_gfx_host.present would, from the canvas and not from a flushed bitmap.
 */
bool mg_gfx_standin_pixels(void *canvas, const void **pixels, int *width, int *height, int *stride);

/*
 * A bitmap of the layer's (B, G, R, A premultiplied, top row first) as a
 * 24-bit BMP file any viewer opens, the colours un-premultiplied and the
 * alpha left out. False when the file cannot be written.
 */
bool mg_gfx_write_bmp(const char *host_path, const void *pixels, int width, int height, int stride);

#ifdef __cplusplus
}
#endif

#endif
