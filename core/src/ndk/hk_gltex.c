/*
 * The app's textures: counted as it asks for them, and, when it asks for a
 * great deal, packed.
 *
 * Textures are most of what a game keeps in memory, and all of it is in the
 * process's own 2 GB of addresses here. A game whose textures are in a format
 * this OpenGL does not offer (ASTC above all: ANGLE over Direct3D 11 has
 * none) decodes them itself and hands over plain RGBA8, four bytes a pixel
 * for what was one or less in its APK; a 462 MB game asked for 442 MB of
 * them in twenty seconds and was still loading when the addresses ran out.
 *
 * So, once an app has asked for more plain texture than a small game ever
 * does, a large RGBA8 texture that is only ever filled and sampled is kept as
 * DXT5 instead: one byte a pixel, a format Direct3D has. The pixels are
 * compressed as the app uploads them. It costs some of the picture (DXT5 is
 * coarser than what the artists shipped) and a little loading time, against
 * not running at all.
 *
 * Whether a texture is "only ever filled and sampled" is not known when its
 * storage is asked for, and the storage cannot be changed afterwards. So the
 * request is held back until the app's next move with that texture: pixels
 * for a whole level (it becomes DXT5), or anything else -- attached to a
 * framebuffer, copied into, its mipmaps generated, filled from a buffer or in
 * another format -- and it is made as asked. That needs to know which texture
 * is bound, so glActiveTexture and glBindTexture are watched too.
 *
 * Flags: "plain-textures" turns the packing off, "pack-textures" does it from
 * the first texture.
 */
#include "hk_egl.h"

#include <stdio.h>
#include <stdlib.h>

#include "../hk_core.h"
#include "../host/hk_host.h"

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA8 0x8058
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_DXT5 0x83F3
#define GL_DXT5_SRGB 0x8C4F
#define GL_EXTENSIONS 0x1F03
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4

/* Packing starts when this much plain texture has been asked for, with textures at least this large. */
#define PACK_AFTER_BYTES (160u << 20)
#define PACK_MIN_PIXELS (256u * 256u)

typedef void (*fn_tex_storage_2d)(u32 target, i32 levels, u32 internal, i32 w, i32 h);
typedef void (*fn_tex_storage_3d)(u32 target, i32 levels, u32 internal, i32 w, i32 h, i32 d);
typedef void (*fn_tex_image_2d)(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 border, u32 format, u32 type, const void *pixels);
typedef void (*fn_tex_image_3d)(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 d, i32 border, u32 format, u32 type, const void *pixels);
typedef void (*fn_compressed_2d)(u32 target, i32 level, u32 internal, i32 w, i32 h, i32 border, i32 size, const void *data);
typedef void (*fn_tex_sub_image_2d)(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, const void *pixels);
typedef void (*fn_compressed_sub_2d)(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, i32 size, const void *data);
typedef void (*fn_enum)(u32 v);
typedef void (*fn_enum_uint)(u32 target, u32 name);
typedef void (*fn_enum_int)(u32 pname, i32 v);
typedef void (*fn_delete)(i32 n, const u32 *names);
typedef void (*fn_fb_texture_2d)(u32 target, u32 attachment, u32 textarget, u32 texture, i32 level);
typedef void (*fn_fb_texture_2d_ms)(u32 target, u32 attachment, u32 textarget, u32 texture, i32 level, i32 samples);
typedef void (*fn_fb_texture_layer)(u32 target, u32 attachment, u32 texture, i32 level, i32 layer);
typedef void (*fn_copy_sub_2d)(u32 target, i32 level, i32 xo, i32 yo, i32 x, i32 y, i32 w, i32 h);
typedef void (*fn_copy_image)(u32 src, u32 st, i32 sl, i32 sx, i32 sy, i32 sz, u32 dst, u32 dt, i32 dl, i32 dx, i32 dy, i32 dz, i32 w, i32 h, i32 d);
typedef const u8 *(*fn_get_string)(u32 name);

static fn_tex_storage_2d h_storage_2d;
static fn_tex_storage_3d h_storage_3d;
static fn_tex_image_2d h_image_2d;
static fn_tex_image_3d h_image_3d;
static fn_compressed_2d h_compressed_2d;
static fn_tex_sub_image_2d h_sub_image_2d;
static fn_compressed_sub_2d h_compressed_sub_2d;
static fn_enum h_active_texture, h_generate_mipmap;
static fn_enum_uint h_bind_texture, h_bind_buffer;
static fn_enum_int h_pixel_storei;
static fn_delete h_delete_textures;
static fn_fb_texture_2d h_fb_texture_2d;
static fn_fb_texture_2d_ms h_fb_texture_2d_ms;
static fn_fb_texture_layer h_fb_texture_layer;
static fn_copy_sub_2d h_copy_sub_2d;
static fn_copy_image h_copy_image;
static fn_get_string h_get_string;

enum { K_PLAIN, K_ETC, K_BLOCK, K_PACKED, K_KINDS };

/* Bytes, and how many times asked. 64-bit sums under a lock: a few hundred calls a second at most. */
static struct {
    u64 bytes[K_KINDS];                /* as the format has it; K_PACKED: as DXT5 */
    u64 etc_expanded;                  /* what the ETC2/EAC ones come to at four bytes a pixel */
    u64 packed_from;                   /* what the packed ones would have been */
    u32 count[K_KINDS];
    u32 largest_w, largest_h, largest_format;
    u64 largest_bytes;
    u32 dropped;                       /* uploads to a packed texture that could not be done */
} G;
static hk_lock g_lock = HK_LOCK_INIT;

/* ------------------------------------------------------------------ DXT5 */

static u32 expand565(u32 c, u8 *rgb)
{
    u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (u8)((r << 3) | (r >> 2));
    rgb[1] = (u8)((g << 2) | (g >> 4));
    rgb[2] = (u8)((b << 3) | (b >> 2));
    return c;
}

/* One block from sixteen RGBA pixels (rows of four, top first). */
static void dxt5_block(const u8 px[16][4], u8 out[16])
{
    u32 lo[4] = { 255, 255, 255, 255 }, hi[4] = { 0, 0, 0, 0 };
    u32 c0, c1, bits = 0, inset;
    u8 pal[4][3];
    u64 abits = 0;
    int i, k;
    for (i = 0; i < 16; i++)
        for (k = 0; k < 4; k++) {
            if (px[i][k] < lo[k]) lo[k] = px[i][k];
            if (px[i][k] > hi[k]) hi[k] = px[i][k];
        }

    /* Alpha: its two ends and six steps between, three bits a pixel. */
    out[0] = (u8)hi[3];
    out[1] = (u8)lo[3];
    if (hi[3] != lo[3]) {
        u32 range = hi[3] - lo[3];
        for (i = 0; i < 16; i++) {
            u32 t = ((px[i][3] - lo[3]) * 7 + range / 2) / range;      /* 0 at the low end, 7 at the high */
            u32 idx = t == 7 ? 0 : t == 0 ? 1 : 8 - t;
            abits |= (u64)idx << (3 * i);
        }
    }
    for (i = 0; i < 6; i++) out[2 + i] = (u8)(abits >> (8 * i));

    /*
     * Colour: two ends and two steps between, two bits a pixel. The ends are
     * opposite corners of the box the pixels lie in, pulled in a little --
     * and which pair of corners is told by how red and green each run
     * against blue: where one falls as blue rises, its ends change places.
     * (Red beside blue lies along the other diagonal from black to magenta.)
     */
    {
        i32 mid[3], cov_rb = 0, cov_gb = 0;
        u32 t;
        for (k = 0; k < 3; k++) mid[k] = (i32)(lo[k] + hi[k] + 1) / 2;
        for (i = 0; i < 16; i++) {
            i32 b = (i32)px[i][2] - mid[2];
            cov_rb += ((i32)px[i][0] - mid[0]) * b;
            cov_gb += ((i32)px[i][1] - mid[1]) * b;
        }
        for (k = 0; k < 3; k++) {
            inset = (hi[k] - lo[k]) >> 4;
            lo[k] += inset;
            hi[k] -= inset;
        }
        if (cov_rb < 0) {
            t = lo[0];
            lo[0] = hi[0];
            hi[0] = t;
        }
        if (cov_gb < 0) {
            t = lo[1];
            lo[1] = hi[1];
            hi[1] = t;
        }
    }
    c0 = ((hi[0] >> 3) << 11) | ((hi[1] >> 2) << 5) | (hi[2] >> 3);
    c1 = ((lo[0] >> 3) << 11) | ((lo[1] >> 2) << 5) | (lo[2] >> 3);
    if (c0 < c1) {
        u32 t = c0;
        c0 = c1;
        c1 = t;
    }
    expand565(c0, pal[0]);
    expand565(c1, pal[1]);
    if (c0 != c1) {
        for (k = 0; k < 3; k++) {
            pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        for (i = 0; i < 16; i++) {
            u32 best = 0, best_d = 0xFFFFFFFFu, j;
            for (j = 0; j < 4; j++) {
                i32 dr = (i32)px[i][0] - pal[j][0], dg = (i32)px[i][1] - pal[j][1], db = (i32)px[i][2] - pal[j][2];
                u32 d = (u32)(dr * dr + dg * dg + db * db);
                if (d < best_d) {
                    best_d = d;
                    best = j;
                }
            }
            bits |= best << (2 * i);
        }
    }
    out[8] = (u8)c0;
    out[9] = (u8)(c0 >> 8);
    out[10] = (u8)c1;
    out[11] = (u8)(c1 >> 8);
    out[12] = (u8)bits;
    out[13] = (u8)(bits >> 8);
    out[14] = (u8)(bits >> 16);
    out[15] = (u8)(bits >> 24);
}

size_t hk_gltex_dxt5_size(u32 w, u32 h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16; }

void hk_gltex_dxt5(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out)
{
    u32 bx, by, x, y;
    u8 px[16][4];
    for (by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4) {
            /* A block that hangs over the edge repeats the edge's pixels: they are never shown, and must not pull the block's colours away. */
            for (y = 0; y < 4; y++) {
                u32 sy = by + y < h ? by + y : h - 1;
                const u8 *row = rgba + (size_t)sy * stride;
                for (x = 0; x < 4; x++) {
                    u32 sx = bx + x < w ? bx + x : w - 1;
                    memcpy(px[y * 4 + x], row + (size_t)sx * 4, 4);
                }
            }
            dxt5_block((const u8 (*)[4])px, out);
            out += 16;
        }
}

/* --------------------------------------------------- what is bound, per thread */

/* A thread draws with one context; what it has bound there. (An app that moves a context between threads is not followed.) */
static HK_THREAD_LOCAL struct {
    u32 unit;
    u32 bound[32];                     /* the 2D texture on each unit */
    u32 unpack_buffer;
    i32 row_length, skip_rows, skip_pixels;
} T;

static u32 bound_now(void) { return T.bound[T.unit]; }

/* ------------------------------------------------------ the textures held */

enum { S_NONE, S_PENDING, S_PACKED };

typedef struct tex_info {
    u8 state;
    u8 levels;
    u16 w, h;
    u32 internal;
} tex_info;

/* By the texture's name: ANGLE hands names out from 1 upwards. One past the table is simply never held back. */
static tex_info g_info[16384];
static int g_can_pack = -1, g_can_pack_srgb;
static int g_policy = -1;              /* 0 never, 1 when the app has asked for much, 2 from the start */

static bool has_word(const char *list, const char *word)
{
    size_t n = strlen(word);
    const char *p = list;
    while (p && (p = strstr(p, word)) != NULL) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || !p[n])) return true;
        p += n;
    }
    return false;
}

/* A long list of words into the log, a few hundred characters a line. */
static void say_list(const char *what, const char *list)
{
    size_t n = list ? strlen(list) : 0, at = 0;
    while (at < n) {
        size_t take = n - at > 700 ? 700 : n - at;
        while (take < n - at && take > 0 && list[at + take] != ' ') take--;
        if (!take) take = n - at > 700 ? 700 : n - at;
        hk_info("%s: %.*s", what, (int)take, list + at);
        at += take;
        while (at < n && list[at] == ' ') at++;
    }
}

/* Whether this OpenGL has DXT5 at all (with a context current: asked at the first texture that could use it). */
static bool can_pack(u32 internal)
{
    if (g_can_pack < 0) {
        const char *ext = h_get_string ? (const char *)h_get_string(GL_EXTENSIONS) : NULL;
        say_list("gl extensions", ext);
        g_can_pack = ext && (has_word(ext, "GL_ANGLE_texture_compression_dxt5") || has_word(ext, "GL_EXT_texture_compression_s3tc")) ? 1 : 0;
        g_can_pack_srgb = g_can_pack && has_word(ext, "GL_EXT_texture_compression_s3tc_srgb");
        if (!h_compressed_sub_2d || !h_bind_texture) g_can_pack = 0;
        hk_info("textures: large ones can%s be kept as DXT5 here%s", g_can_pack ? "" : "not",
                g_can_pack && !g_can_pack_srgb ? " (not the sRGB ones)" : "");
    }
    return g_can_pack && (internal == GL_RGBA8 || g_can_pack_srgb);
}

static int policy(void)
{
    if (g_policy < 0) g_policy = hk_flag("plain-textures") ? 0 : hk_flag("pack-textures") ? 2 : 1;
    return g_policy;
}

/* ---------------------------------------------------------------- counting */

/* Bits a pixel and the kind, by internal format (or by format and type, for the unsized ones OpenGL ES 2 code uses). */
static u32 format_bits(u32 internal, u32 format, u32 type, int *kind)
{
    *kind = K_PLAIN;
    switch (internal) {
    case 0x9274: case 0x9275: case 0x9276: case 0x9277: case 0x9270: case 0x9271: case 0x8D64:
        *kind = K_ETC;                 /* RGB8, sRGB8, the punch-through pair, R11 and its signed twin, ETC1 */
        return 4;
    case 0x9278: case 0x9279: case 0x9272: case 0x9273:
        *kind = K_ETC;                 /* RGBA8 and its sRGB twin, RG11 and its signed twin */
        return 8;
    case 0x83F0: case 0x83F1: case 0x8C4C: case 0x8C4D:
        *kind = K_BLOCK;               /* DXT1 */
        return 4;
    case 0x83F2: case 0x83F3: case 0x8C4E: case 0x8C4F:
        *kind = K_BLOCK;               /* DXT3, DXT5 */
        return 8;
    case 0x8229: case 0x8F94: case 0x8232: case 0x8231: case 0x803C: case 0x8040: return 8;      /* R8, ALPHA8, LUMINANCE8 */
    case 0x822B: case 0x8F95: case 0x8D62: case 0x8056: case 0x8057: case 0x81A5: case 0x822D: case 0x8045: return 16;
    case 0x8051: case 0x8C41: case 0x8058: case 0x8C43: case 0x8F97: case 0x81A6: case 0x88F0: case 0x8CAC: case 0x822E: case 0x822F:
    case 0x8C3A: case 0x8C3D: case 0x8059: case 0x906F: case 0x8D7C: case 0x8D8E:
        return 32;                     /* RGB8 is four bytes a pixel in Direct3D too */
    case 0x881A: case 0x881B: case 0x8230: return 64;
    case 0x8814: case 0x8815: return 128;
    default: break;
    }
    if (internal >= 0x93B0 && internal <= 0x93DD) {
        *kind = K_BLOCK;               /* ASTC: 8 bits a pixel at its largest (4x4 blocks) */
        return 8;
    }
    /* Unsized: GL_RGBA, GL_RGB, GL_LUMINANCE_ALPHA, GL_LUMINANCE, GL_ALPHA with a type. */
    if (type == 0x8363 || type == 0x8033 || type == 0x8034) return 16;
    switch (format ? format : internal) {
    case 0x1908: case 0x1907: return 32;
    case 0x190A: return 16;
    case 0x1909: case 0x1906: case 0x1903: return 8;
    default: return 32;
    }
}

/* What a texture of these levels comes to, in bytes and in pixels. */
static u64 storage_bytes(u32 internal, u32 format, u32 type, i32 w, i32 h, i32 d, i32 levels, int *kind, u64 *pixels_out)
{
    u32 bits = format_bits(internal, format, type, kind);
    u64 bytes = 0, pixels = 0;
    int level;
    for (level = 0; level < levels; level++) {
        u64 lw = (u32)w >> level ? (u32)w >> level : 1, lh = (u32)h >> level ? (u32)h >> level : 1;
        u64 n = lw * lh * (u64)d;
        if (*kind != K_PLAIN) n = ((lw + 3) & ~(u64)3) * ((lh + 3) & ~(u64)3) * (u64)d;
        pixels += n;
        bytes += n * bits / 8;
    }
    if (pixels_out) *pixels_out = pixels;
    return bytes;
}

static void count(u32 target, u32 internal, u32 format, u32 type, i32 w, i32 h, i32 d, i32 levels, u64 given)
{
    int kind;
    u64 bytes, pixels = 0;
    if (w <= 0 || h <= 0 || d <= 0 || levels <= 0) return;
    bytes = storage_bytes(internal, format, type, w, h, d, levels, &kind, &pixels);
    if (given) bytes = given;
    if (target == GL_TEXTURE_CUBE_MAP) {
        bytes *= 6;
        pixels *= 6;
    }
    hk_lock_acquire(&g_lock);
    G.bytes[kind] += bytes;
    G.count[kind]++;
    if (kind == K_ETC) G.etc_expanded += pixels * 4;
    if (bytes > G.largest_bytes) {
        G.largest_bytes = bytes;
        G.largest_w = (u32)w;
        G.largest_h = (u32)h;
        G.largest_format = internal;
    }
    hk_lock_release(&g_lock);
}

/* ------------------------------------------------- holding back, and making */

static tex_info *info_of(u32 name) { return name && name < HK_COUNT(g_info) ? &g_info[name] : NULL; }

/* The storage a held-back texture was asked with, made now. `name` is bound for it and what was bound is put back. */
static void make_plain(u32 name, tex_info *t)
{
    bool rebind = bound_now() != name;
    t->state = S_NONE;
    if (rebind) h_bind_texture(GL_TEXTURE_2D, name);
    count(GL_TEXTURE_2D, t->internal, 0, 0, t->w, t->h, 1, t->levels, 0);
    h_storage_2d(GL_TEXTURE_2D, t->levels, t->internal, t->w, t->h);
    if (rebind) h_bind_texture(GL_TEXTURE_2D, bound_now());
}

static void make_packed(tex_info *t)
{
    int kind;
    u64 plain = storage_bytes(t->internal, 0, 0, t->w, t->h, 1, t->levels, &kind, NULL);
    t->state = S_PACKED;
    h_storage_2d(GL_TEXTURE_2D, t->levels, t->internal == GL_SRGB8_ALPHA8 ? GL_DXT5_SRGB : GL_DXT5, t->w, t->h);
    hk_lock_acquire(&g_lock);
    G.count[K_PACKED]++;
    G.bytes[K_PACKED] += plain / 4;
    G.packed_from += plain;
    hk_lock_release(&g_lock);
}

/* The app does something with a texture, by name, that only a plain one takes. */
static void needs_plain(u32 name, const char *what)
{
    static volatile i32 said;
    tex_info *t = info_of(name);
    if (!t || t->state == S_NONE) return;
    if (t->state == S_PENDING) {
        make_plain(name, t);
        return;
    }
    if (hk_atomic_add(&said, 1) <= 6) hk_warn("textures: %s on texture %u, which is kept as DXT5 and cannot take it", what, (unsigned)name);
}

static void w_storage_2d(u32 target, i32 levels, u32 internal, i32 w, i32 h)
{
    u32 name = bound_now();
    tex_info *t = target == GL_TEXTURE_2D ? info_of(name) : NULL;
    bool much;
    if (t && t->state != S_NONE) t->state = S_NONE;            /* a name used again */
    hk_lock_acquire(&g_lock);
    much = G.bytes[K_PLAIN] + G.packed_from >= PACK_AFTER_BYTES;
    hk_lock_release(&g_lock);
    if (t && (internal == GL_RGBA8 || internal == GL_SRGB8_ALPHA8) && levels >= 1 && levels < 32 && w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
        !(w & 3) && !(h & 3) && (u32)w * (u32)h >= PACK_MIN_PIXELS && policy() && (much || policy() == 2) && can_pack(internal)) {
        t->state = S_PENDING;
        t->levels = (u8)levels;
        t->w = (u16)w;
        t->h = (u16)h;
        t->internal = internal;
        return;
    }
    count(target, internal, 0, 0, w, h, 1, levels, 0);
    h_storage_2d(target, levels, internal, w, h);
}

static void w_sub_image_2d(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, const void *pixels)
{
    static volatile i32 said;
    tex_info *t = target == GL_TEXTURE_2D ? info_of(bound_now()) : NULL;
    bool ours = format == GL_RGBA && type == GL_UNSIGNED_BYTE && !T.unpack_buffer && pixels && !T.skip_rows && !T.skip_pixels && w > 0 && h > 0;
    if (t && t->state == S_PENDING) {
        i32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
        if (ours && level >= 0 && level < t->levels && x == 0 && y == 0 && w == lw && h == lh) make_packed(t);
        else make_plain(bound_now(), t);
    }
    if (t && t->state == S_PACKED) {
        i32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
        /* Whole blocks only: a piece must start on one and end on one, or at the level's edge. */
        bool fits = ours && level >= 0 && level < t->levels && !(x & 3) && !(y & 3) && x >= 0 && y >= 0 && x + w <= lw && y + h <= lh &&
                    (!(w & 3) || x + w == lw) && (!(h & 3) || y + h == lh);
        if (fits) {
            size_t stride = (size_t)(T.row_length > 0 ? T.row_length : w) * 4, size = hk_gltex_dxt5_size((u32)w, (u32)h);
            u8 *packed = (u8 *)malloc(size);
            if (packed) {
                hk_gltex_dxt5((const u8 *)pixels, (u32)w, (u32)h, stride, packed);
                h_compressed_sub_2d(target, level, x, y, w, h, t->internal == GL_SRGB8_ALPHA8 ? GL_DXT5_SRGB : GL_DXT5, (i32)size, packed);
                free(packed);
                return;
            }
        }
        hk_atomic_add((volatile i32 *)&G.dropped, 1);
        if (hk_atomic_add(&said, 1) <= 6)
            hk_warn("textures: an upload of %dx%d at %d,%d (level %d, format 0x%x type 0x%x) to a texture kept as DXT5 could not be done", w, h, x, y,
                    level, (unsigned)format, (unsigned)type);
        return;
    }
    h_sub_image_2d(target, level, x, y, w, h, format, type, pixels);
}

static void w_storage_3d(u32 target, i32 levels, u32 internal, i32 w, i32 h, i32 d)
{
    count(target, internal, 0, 0, w, h, d, levels, 0);
    h_storage_3d(target, levels, internal, w, h, d);
}

static void w_image_2d(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 border, u32 format, u32 type, const void *pixels)
{
    if (target == GL_TEXTURE_2D) needs_plain(bound_now(), "glTexImage2D");
    count(0, (u32)internal, format, type, w, h, 1, 1, 0);
    h_image_2d(target, level, internal, w, h, border, format, type, pixels);
}

static void w_image_3d(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 d, i32 border, u32 format, u32 type, const void *pixels)
{
    count(0, (u32)internal, format, type, w, h, d, 1, 0);
    h_image_3d(target, level, internal, w, h, d, border, format, type, pixels);
}

static void w_compressed_2d(u32 target, i32 level, u32 internal, i32 w, i32 h, i32 border, i32 size, const void *data)
{
    if (target == GL_TEXTURE_2D) needs_plain(bound_now(), "glCompressedTexImage2D");
    count(0, internal, 0, 0, w, h, 1, 1, size > 0 ? (u64)size : 0);
    h_compressed_2d(target, level, internal, w, h, border, size, data);
}

static void w_active_texture(u32 unit)
{
    if (unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + HK_COUNT(T.bound)) T.unit = unit - GL_TEXTURE0;
    h_active_texture(unit);
}

static void w_bind_texture(u32 target, u32 name)
{
    if (target == GL_TEXTURE_2D) T.bound[T.unit] = name;
    h_bind_texture(target, name);
}

static void w_bind_buffer(u32 target, u32 name)
{
    if (target == GL_PIXEL_UNPACK_BUFFER) T.unpack_buffer = name;
    h_bind_buffer(target, name);
}

static void w_pixel_storei(u32 pname, i32 v)
{
    if (pname == GL_UNPACK_ROW_LENGTH) T.row_length = v;
    else if (pname == GL_UNPACK_SKIP_ROWS) T.skip_rows = v;
    else if (pname == GL_UNPACK_SKIP_PIXELS) T.skip_pixels = v;
    h_pixel_storei(pname, v);
}

static void w_delete_textures(i32 n, const u32 *names)
{
    i32 i;
    u32 u;
    for (i = 0; names && i < n; i++) {
        tex_info *t = info_of(names[i]);
        if (t) t->state = S_NONE;
        for (u = 0; u < HK_COUNT(T.bound); u++)
            if (T.bound[u] == names[i]) T.bound[u] = 0;
    }
    h_delete_textures(n, names);
}

static void w_generate_mipmap(u32 target)
{
    tex_info *t = target == GL_TEXTURE_2D ? info_of(bound_now()) : NULL;
    static volatile i32 said;
    if (t && t->state == S_PENDING) make_plain(bound_now(), t);
    if (t && t->state == S_PACKED) {
        /* Direct3D makes no mipmaps for a compressed texture. The levels the app did not upload stay as they are. */
        if (hk_atomic_add(&said, 1) <= 3) hk_warn("textures: mipmaps asked for a texture kept as DXT5; not made");
        return;
    }
    h_generate_mipmap(target);
}

static void w_fb_texture_2d(u32 target, u32 attachment, u32 textarget, u32 texture, i32 level)
{
    needs_plain(texture, "drawing into it");
    h_fb_texture_2d(target, attachment, textarget, texture, level);
}

static void w_fb_texture_2d_ms(u32 target, u32 attachment, u32 textarget, u32 texture, i32 level, i32 samples)
{
    needs_plain(texture, "drawing into it");
    h_fb_texture_2d_ms(target, attachment, textarget, texture, level, samples);
}

static void w_fb_texture_layer(u32 target, u32 attachment, u32 texture, i32 level, i32 layer)
{
    needs_plain(texture, "drawing into it");
    h_fb_texture_layer(target, attachment, texture, level, layer);
}

static void w_copy_sub_2d(u32 target, i32 level, i32 xo, i32 yo, i32 x, i32 y, i32 w, i32 h)
{
    if (target == GL_TEXTURE_2D) needs_plain(bound_now(), "copying the screen into it");
    h_copy_sub_2d(target, level, xo, yo, x, y, w, h);
}

static void w_copy_image(u32 src, u32 st, i32 sl, i32 sx, i32 sy, i32 sz, u32 dst, u32 dt, i32 dl, i32 dx, i32 dy, i32 dz, i32 w, i32 h, i32 d)
{
    if (st == GL_TEXTURE_2D) needs_plain(src, "copying from it");
    if (dt == GL_TEXTURE_2D) needs_plain(dst, "copying into it");
    h_copy_image(src, st, sl, sx, sy, sz, dst, dt, dl, dx, dy, dz, w, h, d);
}

void *hk_gltex_wrap(const char *name, void *host)
{
    static const struct { const char *name; void *wrapper; void **host; } k[] = {
        { "glTexStorage2D", (void *)w_storage_2d, (void **)&h_storage_2d },
        { "glTexStorage2DEXT", (void *)w_storage_2d, (void **)&h_storage_2d },
        { "glTexStorage3D", (void *)w_storage_3d, (void **)&h_storage_3d },
        { "glTexImage2D", (void *)w_image_2d, (void **)&h_image_2d },
        { "glTexImage3D", (void *)w_image_3d, (void **)&h_image_3d },
        { "glCompressedTexImage2D", (void *)w_compressed_2d, (void **)&h_compressed_2d },
        { "glTexSubImage2D", (void *)w_sub_image_2d, (void **)&h_sub_image_2d },
        { "glActiveTexture", (void *)w_active_texture, (void **)&h_active_texture },
        { "glBindTexture", (void *)w_bind_texture, (void **)&h_bind_texture },
        { "glBindBuffer", (void *)w_bind_buffer, (void **)&h_bind_buffer },
        { "glPixelStorei", (void *)w_pixel_storei, (void **)&h_pixel_storei },
        { "glDeleteTextures", (void *)w_delete_textures, (void **)&h_delete_textures },
        { "glGenerateMipmap", (void *)w_generate_mipmap, (void **)&h_generate_mipmap },
        { "glFramebufferTexture2D", (void *)w_fb_texture_2d, (void **)&h_fb_texture_2d },
        { "glFramebufferTexture2DMultisampleEXT", (void *)w_fb_texture_2d_ms, (void **)&h_fb_texture_2d_ms },
        { "glFramebufferTextureLayer", (void *)w_fb_texture_layer, (void **)&h_fb_texture_layer },
        { "glCopyTexSubImage2D", (void *)w_copy_sub_2d, (void **)&h_copy_sub_2d },
        { "glCopyImageSubData", (void *)w_copy_image, (void **)&h_copy_image },
        { "glCopyImageSubDataEXT", (void *)w_copy_image, (void **)&h_copy_image },
    };
    size_t i;
    if (!host) return NULL;
    for (i = 0; i < HK_COUNT(k); i++)
        if (!strcmp(k[i].name, name)) {
            if (!*k[i].host) *k[i].host = host;
            /* (An EXT name and the core one are the same function in ANGLE; if they ever were not, the second is left alone.) */
            return *k[i].host == host ? k[i].wrapper : host;
        }
    return host;
}

void hk_gltex_set_host(void *(*resolve)(const char *name))
{
    /* What packing itself calls, whether or not the app ever asked for them by name. */
    if (!h_compressed_sub_2d) h_compressed_sub_2d = (fn_compressed_sub_2d)resolve("glCompressedTexSubImage2D");
    if (!h_bind_texture) h_bind_texture = (fn_enum_uint)resolve("glBindTexture");
    if (!h_get_string) h_get_string = (fn_get_string)resolve("glGetString");
    if (!h_storage_2d) h_storage_2d = (fn_tex_storage_2d)resolve("glTexStorage2D");
}

void hk_gltex_report(char *out, size_t size)
{
    hk_lock_acquire(&g_lock);
    snprintf(out, size,
             "textures asked for so far: %u MB plain in %u, %u MB ETC2/EAC in %u (%u MB as ANGLE keeps them), %u MB other compressed in %u; "
             "the largest %ux%u of format 0x%x, %u MB; kept as DXT5: %u, %u MB for what would have been %u MB (%u uploads dropped)",
             (unsigned)(G.bytes[K_PLAIN] >> 20), (unsigned)G.count[K_PLAIN], (unsigned)(G.bytes[K_ETC] >> 20), (unsigned)G.count[K_ETC],
             (unsigned)(G.etc_expanded >> 20), (unsigned)(G.bytes[K_BLOCK] >> 20), (unsigned)G.count[K_BLOCK], (unsigned)G.largest_w,
             (unsigned)G.largest_h, (unsigned)G.largest_format, (unsigned)(G.largest_bytes >> 20), (unsigned)G.count[K_PACKED],
             (unsigned)(G.bytes[K_PACKED] >> 20), (unsigned)(G.packed_from >> 20), (unsigned)G.dropped);
    hk_lock_release(&g_lock);
}
