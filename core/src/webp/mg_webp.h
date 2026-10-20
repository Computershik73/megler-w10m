/*
 * WebP pictures, decoded in the core.
 *
 * An Android app's pictures are WebP as often as PNG (the build tools convert
 * them), and the phone's own picture decoder (WIC on Windows 10 Mobile) has
 * no WebP in it. So a WebP file is not handed to the shell's image_size and
 * image_decode at all: the core sees "RIFF....WEBP" and decodes it itself,
 * with libwebp's decoder, into the same memory the shell would have filled:
 * B, G, R, A, premultiplied, top row first.
 *
 * libwebp is not part of this repository. Where its decoder's sources have
 * been put (build/webp/src, tools/webp-fetch.sh) the build scripts define
 * MG_HAVE_WEBP and name build/webp as an include path, and the files
 * mg_webp_*.c of this folder each compile one of libwebp's (it has no
 * amalgamation, and its files do not bear being compiled as one). Plain C:
 * the NEON and SSE paths are switched off (mg_webp_config.h), so nothing
 * asks at run time what the processor can do; and no threads. Where the
 * sources are not there, everything compiles to nothing, mg_webp_available
 * is false, and a WebP picture goes to the shell like any other, which the
 * log says once.
 *
 * Decoded: lossy (VP8), lossless (VP8L), either with alpha, and of an
 * animated file its first frame, placed on a transparent canvas. A picture
 * asked for smaller than it is (BitmapFactory's inSampleSize) is scaled
 * while it is decoded.
 */
#ifndef MG_WEBP_H
#define MG_WEBP_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether this build has the decoder in it. */
bool mg_webp_available(void);
/* Whether the bytes begin as a WebP file does. True in every build. */
bool mg_webp_is(const void *data, size_t size);
/* The picture's size (an animation's canvas). False when it is no WebP file this decoder reads. */
bool mg_webp_size(const void *data, size_t size, int *width, int *height);
/*
 * The picture into width x height pixels of `stride` bytes a row, scaled to
 * that size when it is another: B, G, R, A premultiplied. False, and the
 * memory undefined, when it cannot be decoded.
 */
bool mg_webp_decode(const void *data, size_t size, void *pixels, int width, int height, int stride);

#ifdef __cplusplus
}
#endif

#endif
