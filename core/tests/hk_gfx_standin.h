/*
 * A stand-in for the shell's drawing (meglet_gfx_host), for tests: portable C
 * that draws straight into the bitmaps' memory and gives the same pixels on
 * every machine. It is not a renderer to look at: hk_gfx_standin.c says what
 * it leaves out and what it draws in place of the real thing (text is boxes).
 */
#ifndef HK_GFX_STANDIN_H
#define HK_GFX_STANDIN_H

#include "../include/meglet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The host, every function of it filled in but `present`; `user` is not used. */
const meglet_gfx_host *hk_gfx_standin(void);

/*
 * The memory a canvas of the stand-in's draws into, which always holds its picture: for a test that shows a canvas's
 * picture as a shell with meglet_gfx_host.present would, from the canvas and not from a flushed bitmap.
 */
bool hk_gfx_standin_pixels(void *canvas, const void **pixels, int *width, int *height, int *stride);

/*
 * A bitmap of the layer's (B, G, R, A premultiplied, top row first) as a
 * 24-bit BMP file any viewer opens, the colours un-premultiplied and the
 * alpha left out. False when the file cannot be written.
 */
bool hk_gfx_write_bmp(const char *host_path, const void *pixels, int width, int height, int stride);

#ifdef __cplusplus
}
#endif

#endif
