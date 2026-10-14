/* WebP pictures decoded in the core: see hk_webp.h. This file is the core's side; libwebp's own files are compiled by hk_webp_*.c. */
#include "hk_webp.h"

bool hk_webp_is(const void *data, size_t size)
{
    const u8 *p = (const u8 *)data;
    return p && size >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4);
}

#if defined(HK_HAVE_WEBP) && HK_HAVE_WEBP

#include "src/webp/decode.h"

bool hk_webp_available(void) { return true; }

bool hk_webp_size(const void *data, size_t size, int *width, int *height)
{
    return hk_webp_is(data, size) && WebPGetInfo((const uint8_t *)data, size, width, height) && *width > 0 && *height > 0;
}

/* One still picture, straight into the caller's memory, scaled by the decoder when the size asked for is not its own. */
static bool decode_still(const u8 *data, size_t size, void *pixels, int width, int height, int stride)
{
    WebPDecoderConfig config;
    bool ok;
    if (!WebPInitDecoderConfig(&config) || WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) return false;
    config.output.colorspace = MODE_bgrA;          /* B, G, R, A in memory, premultiplied */
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = (uint8_t *)pixels;
    config.output.u.RGBA.stride = stride;
    config.output.u.RGBA.size = (size_t)stride * (size_t)height;
    if (width != config.input.width || height != config.input.height) {
        config.options.use_scaling = 1;
        config.options.scaled_width = width;
        config.options.scaled_height = height;
    }
    ok = WebPDecode(data, size, &config) == VP8_STATUS_OK;
    WebPFreeDecBuffer(&config.output);
    return ok;
}

static u32 le24(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16; }
static u32 le32(const u8 *p) { return le24(p) | (u32)p[3] << 24; }

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/*
 * An animated file: its first frame. The decoder proper does not read
 * animations (that is libwebp's demux library, which is not here), but a
 * frame is an ordinary picture's chunks inside an ANMF chunk, after sixteen
 * bytes that say where on the canvas it goes and how large it is. They are
 * copied out behind a file header of their own -- with a VP8X chunk in front
 * when the frame has an alpha plane, which is what says so -- and decoded as
 * a still. The canvas around a frame that does not fill it stays transparent.
 */
static bool decode_first_frame(const u8 *data, size_t size, void *pixels, int width, int height, int stride, int canvas_w, int canvas_h)
{
    size_t at = 12;
    while (at + 8 <= size) {
        u32 n = le32(data + at + 4);
        const u8 *payload = data + at + 8;
        if (n > size - at - 8) return false;
        if (!memcmp(data + at, "ANMF", 4) && n > 16) {
            int fx = (int)le24(payload) * 2, fy = (int)le24(payload + 3) * 2, fw = (int)le24(payload + 6) + 1, fh = (int)le24(payload + 9) + 1;
            const u8 *chunks = payload + 16;
            size_t chunks_size = n - 16, total;
            bool alpha = chunks_size >= 4 && !memcmp(chunks, "ALPH", 4), ok;
            u8 *file, *frame, *p;
            int x, y;
            total = 12 + (alpha ? 18 : 0) + chunks_size;
            file = (u8 *)hk_alloc(total);
            p = file;
            memcpy(p, "RIFF", 4);
            put32(p + 4, (u32)(total - 8));
            memcpy(p + 8, "WEBP", 4);
            p += 12;
            if (alpha) {
                memcpy(p, "VP8X", 4);
                put32(p + 4, 10);
                put32(p + 8, 0x10);                               /* the alpha flag, and three reserved bytes */
                p[12] = (u8)(fw - 1); p[13] = (u8)((fw - 1) >> 8); p[14] = (u8)((fw - 1) >> 16);
                p[15] = (u8)(fh - 1); p[16] = (u8)((fh - 1) >> 8); p[17] = (u8)((fh - 1) >> 16);
                p += 18;
            }
            memcpy(p, chunks, chunks_size);
            if (fx == 0 && fy == 0 && fw == canvas_w && fh == canvas_h) {
                /* The usual case: the first frame is the whole canvas. */
                ok = decode_still(file, total, pixels, width, height, stride);
                free(file);
                return ok;
            }
            if (fx < 0 || fy < 0 || fw > canvas_w - fx || fh > canvas_h - fy) {
                free(file);
                return false;
            }
            /* A frame smaller than the canvas: decoded at its own size, then the canvas is sampled down to what was asked for. */
            frame = (u8 *)hk_alloc((size_t)fw * 4 * (size_t)fh);
            ok = decode_still(file, total, frame, fw, fh, fw * 4);
            free(file);
            for (y = 0; ok && y < height; y++) {
                u8 *row = (u8 *)pixels + (size_t)y * (size_t)stride;
                int sy = (int)((i64)y * canvas_h / height) - fy;
                for (x = 0; x < width; x++) {
                    int sx = (int)((i64)x * canvas_w / width) - fx;
                    if (sx >= 0 && sy >= 0 && sx < fw && sy < fh) memcpy(row + 4 * x, frame + ((size_t)sy * (size_t)fw + (size_t)sx) * 4, 4);
                    else memset(row + 4 * x, 0, 4);
                }
            }
            free(frame);
            return ok;
        }
        at += 8 + (size_t)n + (n & 1);
    }
    return false;
}

bool hk_webp_decode(const void *data, size_t size, void *pixels, int width, int height, int stride)
{
    WebPBitstreamFeatures features;
    const u8 *bytes = (const u8 *)data;
    if (!hk_webp_is(data, size) || !pixels || width <= 0 || height <= 0 || stride < width * 4) return false;
    if (WebPGetFeatures(bytes, size, &features) != VP8_STATUS_OK) return false;
    if (features.has_animation) return decode_first_frame(bytes, size, pixels, width, height, stride, features.width, features.height);
    return decode_still(bytes, size, pixels, width, height, stride);
}

#else

bool hk_webp_available(void) { return false; }

bool hk_webp_size(const void *data, size_t size, int *width, int *height)
{
    (void)data; (void)size; (void)width; (void)height;
    return false;
}

bool hk_webp_decode(const void *data, size_t size, void *pixels, int width, int height, int stride)
{
    (void)data; (void)size; (void)pixels; (void)width; (void)height; (void)stride;
    return false;
}

#endif
