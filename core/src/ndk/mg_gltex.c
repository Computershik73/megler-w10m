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
 *
 * And whatever the app has asked for, while the process is short of addresses
 * (less than 768 MB of them free), a texture of 1024x1024 pixels or more is
 * packed so too, made with glTexStorage2D or glTexImage2D: as BC1, half a byte
 * a pixel, when the first level the app gives is opaque throughout, else as
 * DXT5; and its mipmaps are made here from that level, packed the same way, as
 * Direct3D makes none for a compressed texture (glGenerateMipmap then has
 * nothing left to do). Yandex Maps, with 444 MB of addresses free, ended in
 * GL_OUT_OF_MEMORY as soon as a panorama of 2048x2048 textures was zoomed:
 * graphics memory went from 214 MB to 292 in five seconds, and Direct3D
 * found no 32 MB stretch of addresses for more (packages 540, 543). A
 * texture made with glTexImage2D stays packed while the app gives it whole
 * levels; a level of another size or format makes it as the app asked again.
 *
 * And ETC. A game made for OpenGL ES 3 ships its textures as ETC2, which
 * Direct3D does not have: ANGLE decodes them to RGBA8, four bytes a pixel for
 * what was half a byte or one, and keeps them so. Asphalt 9 asked for 112 MB
 * of them, which came to 507 MB, and the addresses ran out in its first race
 * (package 278). So they go to ANGLE in a format Direct3D has, of the same
 * size: the RGB ones (ETC1, ETC2 RGB8, its punch-through and sRGB kinds) as
 * ANGLE's own GL_ANGLE_lossy_etc_decode formats, which ANGLE turns into BC1
 * block by block as it loads them; RGBA8 ETC2 EAC, which that extension does
 * not offer over Direct3D, as DXT5, each block decoded and packed again here.
 * Direct3D takes a block-compressed level only where the base level it
 * implies is whole blocks: a texture whose first level is not stays as the
 * app gave it, all its levels (one texture, one format). The texture bound is
 * followed for 2D, cube map and 2D array targets. Flag "etc-by-angle": every
 * ETC texture goes to ANGLE as the app gave it.
 *
 * And a large ETC texture -- 1024 pixels a side or more at its base -- goes at
 * half its size, a quarter of the memory: every level decoded, each 2x2 pixels
 * averaged, packed again as BC1 (the opaque ones) or DXT5 (with alpha), the
 * level numbers as the app has them. All of it is in the same 2 GB of
 * addresses as the game, and Direct3D's resources too: Asphalt 9 with its
 * textures the same size as in its APK still ran out of addresses in its first
 * race, 442 MB free in pieces of 31 MB at most, a vertex buffer's mapping
 * failed and the road went untextured (package 281). It costs detail on what
 * is seen up close, against the race. A part of a level uploaded later is
 * halved too where it starts and ends on whole blocks of the halved level.
 * Flag "full-textures": no texture is halved.
 */
#include "mg_egl.h"

#include <stdio.h>
#include <stdlib.h>

#include "../mg_core.h"
#include "../host/mg_host.h"

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA8 0x8058
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_DXT5 0x83F3
#define GL_DXT5_SRGB 0x8C4F
#define GL_DXT1 0x83F0
#define GL_DXT1_SRGB 0x8C4C
#define GL_EXTENSIONS 0x1F03
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_MAP_READ_BIT 0x0001
#define GL_RGBA8_ETC2_EAC 0x9278
#define GL_SRGB8_ALPHA8_ETC2_EAC 0x9279

/* An ETC texture is halved from this many pixels a side at its base (unless "full-textures"). */
#define HALVE_FROM 1024u

/* Packing starts when this much plain texture has been asked for, with textures at least this large. */
#define PACK_AFTER_BYTES (160u << 20)
#define PACK_MIN_PIXELS (256u * 256u)
/* And while the process has less than this many MB of addresses free, for textures at least this large (4 MB as RGBA8). */
#define SHORT_OF_ADDRESSES_MB 768u
#define PRESSED_MIN_PIXELS (1024u * 1024u)

typedef void (*fn_tex_storage_2d)(u32 target, i32 levels, u32 internal, i32 w, i32 h);
typedef void (*fn_tex_storage_3d)(u32 target, i32 levels, u32 internal, i32 w, i32 h, i32 d);
typedef void (*fn_tex_image_2d)(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 border, u32 format, u32 type, const void *pixels);
typedef void (*fn_tex_image_3d)(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 d, i32 border, u32 format, u32 type, const void *pixels);
typedef void (*fn_compressed_2d)(u32 target, i32 level, u32 internal, i32 w, i32 h, i32 border, i32 size, const void *data);
typedef void (*fn_tex_sub_image_2d)(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, const void *pixels);
typedef void (*fn_compressed_sub_2d)(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, i32 size, const void *data);
typedef void (*fn_compressed_3d)(u32 target, i32 level, u32 internal, i32 w, i32 h, i32 d, i32 border, i32 size, const void *data);
typedef void (*fn_compressed_sub_3d)(u32 target, i32 level, i32 x, i32 y, i32 z, i32 w, i32 h, i32 d, u32 format, i32 size, const void *data);
typedef void *(*fn_map_range)(u32 target, intptr_t offset, intptr_t length, u32 access);
typedef u8 (*fn_unmap)(u32 target);
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
static fn_compressed_3d h_compressed_3d;
static fn_compressed_sub_3d h_compressed_sub_3d;
static fn_map_range h_map_range;
static fn_unmap h_unmap;
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
    u64 etc_expanded;                  /* what the ETC2/EAC ones ANGLE decodes come to at four bytes a pixel */
    u64 etc_as_bc;                     /* the ETC2/EAC ones given to ANGLE as BC1 or DXT5 instead, their own size */
    u32 etc_as_bc_count;
    u64 halved_from, halved_to;        /* the levels and storage given at half their size: what they were, what they became */
    u32 halved_count;
    u64 packed_from;                   /* what the packed ones would have been */
    u32 count[K_KINDS];
    u32 largest_w, largest_h, largest_format;
    u64 largest_bytes;
    u32 dropped;                       /* uploads to a packed texture that could not be done */
} G;
static mg_lock g_lock = MG_LOCK_INIT;

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

size_t mg_gltex_dxt5_size(u32 w, u32 h) { return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16; }

static void encode_blocks(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out, bool bc1);

/* ------------------------------------------------------------- ETC2 EAC to DXT5 */

/* ETC1 and ETC2's intensity modifiers, by table and by a pixel's two-bit index. */
static const i16 k_etc_modifier[8][4] = {
    { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
    { 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 }, { 47, 183, -47, -183 },
};
/* The distances of ETC2's T and H modes. */
static const u8 k_etc_distance[8] = { 3, 6, 11, 16, 23, 32, 41, 64 };
/* EAC's modifiers, by table and by a pixel's three-bit index. */
static const i8 k_eac_modifier[16][8] = {
    { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 }, { -2, -4, -6, -13, 1, 3, 5, 12 },
    { -3, -6, -8, -12, 2, 5, 7, 11 }, { -3, -7, -9, -11, 2, 6, 8, 10 }, { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },
    { -2, -6, -8, -10, 1, 5, 7, 9 }, { -2, -5, -8, -10, 1, 4, 7, 9 }, { -2, -4, -8, -10, 1, 3, 7, 9 }, { -2, -5, -7, -10, 1, 4, 6, 9 },
    { -3, -4, -7, -10, 2, 3, 6, 9 }, { -1, -2, -3, -10, 0, 1, 2, 9 }, { -4, -6, -8, -9, 3, 5, 7, 8 }, { -3, -5, -7, -9, 2, 4, 6, 8 },
};

static u8 clamp_byte(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }
static int extend4(int v) { return v << 4 | v; }
static int extend5(int v) { return v << 3 | v >> 2; }
static int extend6(int v) { return v << 2 | v >> 4; }
static int extend7(int v) { return v << 1 | v >> 6; }
static int signed3(int v) { return v & 4 ? v - 8 : v; }

static void set3(int *c, int r, int g, int b)
{
    c[0] = r;
    c[1] = g;
    c[2] = b;
}

/*
 * The colours of an ETC2 RGB block (eight bytes, as the format stores them) into sixteen pixels, rows of four, top first;
 * their alpha is left as it was. A pixel's index is two bits, its high bit in bytes 4-5 and its low bit in bytes 6-7, pixel
 * x, y at bit x * 4 + y. The differential bit, and which channel's difference carries its base colour out of range, tell
 * the mode: none, individual; red, T; green, H; blue, planar; else differential.
 */
static void etc2_colour(const u8 *c, u8 px[16][4], bool punch)
{
    u32 msb = (u32)c[4] << 8 | c[5], lsb = (u32)c[6] << 8 | c[7];
    int pal[4][3], base[2][3], x, y, k;
    bool halves = false, clear = punch && !(c[3] & 2);       /* punch-through: the bit is "opaque", and there is no individual mode */
    if ((c[3] & 2) || punch) {
        int r = c[0] >> 3, g = c[1] >> 3, b = c[2] >> 3;
        int r2 = r + signed3(c[0] & 7), g2 = g + signed3(c[1] & 7), b2 = b + signed3(c[2] & 7);
        if (r2 < 0 || r2 > 31) {
            /* T: one colour, and three about a second one, a distance either side. */
            int r1 = extend4((c[0] >> 3 & 3) << 2 | (c[0] & 3)), g1 = extend4(c[1] >> 4), b1 = extend4(c[1] & 15);
            int rr = extend4(c[2] >> 4), gg = extend4(c[2] & 15), bb = extend4(c[3] >> 4);
            int d = k_etc_distance[(c[3] >> 2 & 3) << 1 | (c[3] & 1)];
            set3(pal[0], r1, g1, b1);
            set3(pal[1], rr + d, gg + d, bb + d);
            set3(pal[2], rr, gg, bb);
            set3(pal[3], rr - d, gg - d, bb - d);
        } else if (g2 < 0 || g2 > 31) {
            /* H: two colours, each a distance either side; which of them is the larger gives the distance's last bit. */
            int r1 = extend4(c[0] >> 3 & 15), g1 = extend4((c[0] & 7) << 1 | (c[1] >> 4 & 1));
            int b1 = extend4((c[1] >> 3 & 1) << 3 | (c[1] & 3) << 1 | c[2] >> 7);
            int rr = extend4(c[2] >> 3 & 15), gg = extend4((c[2] & 7) << 1 | c[3] >> 7), bb = extend4(c[3] >> 3 & 15);
            int order = (r1 << 16 | g1 << 8 | b1) >= (rr << 16 | gg << 8 | bb) ? 1 : 0;
            int d = k_etc_distance[(c[3] >> 2 & 1) << 2 | (c[3] & 1) << 1 | order];
            set3(pal[0], r1 + d, g1 + d, b1 + d);
            set3(pal[1], r1 - d, g1 - d, b1 - d);
            set3(pal[2], rr + d, gg + d, bb + d);
            set3(pal[3], rr - d, gg - d, bb - d);
        } else if (b2 < 0 || b2 > 31) {
            /* Planar: a colour at the origin, one a block to the right and one a block down, and the plane through them. */
            int ro = extend6(c[0] >> 1 & 63), go = extend7((c[0] & 1) << 6 | (c[1] >> 1 & 63));
            int bo = extend6((c[1] & 1) << 5 | (c[2] >> 3 & 3) << 3 | (c[2] & 3) << 1 | c[3] >> 7);
            int rh = extend6((c[3] >> 2 & 31) << 1 | (c[3] & 1)), gh = extend7(c[4] >> 1), bh = extend6((c[4] & 1) << 5 | c[5] >> 3);
            int rv = extend6((c[5] & 7) << 3 | c[6] >> 5), gv = extend7((c[6] & 31) << 2 | c[7] >> 6), bv = extend6(c[7] & 63);
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++) {
                    u8 *p = px[y * 4 + x];
                    p[0] = clamp_byte(((x * (rh - ro) + y * (rv - ro) + 2) >> 2) + ro);
                    p[1] = clamp_byte(((x * (gh - go) + y * (gv - go) + 2) >> 2) + go);
                    p[2] = clamp_byte(((x * (bh - bo) + y * (bv - bo) + 2) >> 2) + bo);
                }
            return;
        } else {
            set3(base[0], extend5(r), extend5(g), extend5(b));
            set3(base[1], extend5(r2), extend5(g2), extend5(b2));
            halves = true;
        }
    } else {
        set3(base[0], extend4(c[0] >> 4), extend4(c[1] >> 4), extend4(c[2] >> 4));
        set3(base[1], extend4(c[0] & 15), extend4(c[1] & 15), extend4(c[2] & 15));
        halves = true;
    }
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
            int bit = x * 4 + y, idx = (int)((msb >> bit & 1) << 1 | (lsb >> bit & 1));
            u8 *p = px[y * 4 + x];
            if (halves) {
                /* Two halves, side by side or (the flip bit) one above the other, each with its colour and its table. */
                int half = (c[3] & 1) ? y >= 2 : x >= 2, m = k_etc_modifier[half ? (c[3] >> 2 & 7) : (c[3] >> 5 & 7)][idx];
                if (clear && !(idx & 1)) m = 0;                 /* the table without its small steps */
                for (k = 0; k < 3; k++) p[k] = clamp_byte(base[half][k] + m);
            } else {
                for (k = 0; k < 3; k++) p[k] = clamp_byte(pal[idx][k]);
            }
            /* Punch-through's index 2, where the block is not opaque: a pixel that is not there. */
            if (clear && idx == 2) p[0] = p[1] = p[2] = p[3] = 0;
        }
}

/* The alpha of an EAC block (eight bytes) into sixteen pixels: a base, a multiplier, a table, and three bits a pixel, pixel x, y the (x * 4 + y)th from the top. */
static void eac_alpha(const u8 *a, u8 px[16][4])
{
    const i8 *mod = k_eac_modifier[a[1] & 15];
    int mult = a[1] >> 4, i;
    u64 bits = 0;
    for (i = 2; i < 8; i++) bits = bits << 8 | a[i];
    for (i = 0; i < 16; i++) px[(i & 3) * 4 + (i >> 2)][3] = clamp_byte(a[0] + mod[(bits >> (45 - 3 * i)) & 7] * mult);
}

void mg_gltex_etc2_eac_decode(const u8 block[16], u8 px[16][4])
{
    eac_alpha(block, px);
    etc2_colour(block + 8, px, false);
}

/* The bytes of one 4x4 block of an ETC format. */
static u32 etc_block_bytes(u32 format) { return format == 0x9278 || format == 0x9279 || format == 0x9272 || format == 0x9273 ? 16 : 8; }

/* Whether the halved form of an ETC format has alpha (DXT5) or not (BC1). */
static bool etc_alpha(u32 format) { return format >= 0x9276 && format <= 0x9279; }

bool mg_gltex_etc_decode(u32 format, const u8 *block, u8 px[16][4])
{
    int i;
    switch (format) {
    case 0x9278: case 0x9279:
        mg_gltex_etc2_eac_decode(block, px);
        return true;
    case 0x8D64: case 0x9274: case 0x9275: case 0x9276: case 0x9277:
        for (i = 0; i < 16; i++) px[i][3] = 255;
        etc2_colour(block, px, format == 0x9276 || format == 0x9277);
        return true;
    default:
        return false;
    }
}

size_t mg_gltex_etc_halved_size(u32 format, u32 w, u32 h)
{
    u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    return (size_t)((nw + 3) / 4) * ((nh + 3) / 4) * (etc_alpha(format) ? 16 : 8);
}

size_t mg_gltex_etc_halve(u32 format, const u8 *in, u32 w, u32 h, u8 *out)
{
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4, nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, bx, by, x, y, k, row = bw * 16;
    size_t bytes = etc_block_bytes(format);
    u8 *rgba, *half, px[16][4];
    if (!w || !h || !mg_gltex_etc_decode(format, in, px)) return 0;
    rgba = (u8 *)malloc((size_t)row * bh * 4);
    half = (u8 *)malloc((size_t)nw * nh * 4);
    if (!rgba || !half) {
        free(rgba);
        free(half);
        return 0;
    }
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            mg_gltex_etc_decode(format, in + (by * bw + bx) * bytes, px);
            for (y = 0; y < 4; y++) memcpy(rgba + (size_t)(by * 4 + y) * row + bx * 16, px[y * 4], 16);
        }
    /* Each pixel of the half the mean of its four, the last row or column taken twice where the size is odd or one. */
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++) {
            u32 x0 = 2 * x < w ? 2 * x : w - 1, y0 = 2 * y < h ? 2 * y : h - 1;
            u32 x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
            for (k = 0; k < 4; k++)
                half[(y * nw + x) * 4 + k] = (u8)((rgba[(size_t)y0 * row + x0 * 4 + k] + rgba[(size_t)y0 * row + x1 * 4 + k] + rgba[(size_t)y1 * row + x0 * 4 + k] +
                                                   rgba[(size_t)y1 * row + x1 * 4 + k] + 2) / 4);
        }
    encode_blocks(half, nw, nh, (size_t)nw * 4, out, !etc_alpha(format));
    free(rgba);
    free(half);
    return mg_gltex_etc_halved_size(format, w, h);
}

void mg_gltex_etc2_eac_to_dxt5(const u8 *in, size_t blocks, u8 *out)
{
    u8 px[16][4];
    for (; blocks; blocks--, in += 16, out += 16) {
        mg_gltex_etc2_eac_decode(in, px);
        dxt5_block((const u8 (*)[4])px, out);
    }
}

static void encode_blocks(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out, bool bc1)
{
    u32 bx, by, x, y;
    u8 px[16][4], whole[16];
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
            if (bc1) {
                /* BC1 is DXT5's colour half: its ends in order, larger first, so that it reads as four colours and no transparency. */
                dxt5_block((const u8 (*)[4])px, whole);
                memcpy(out, whole + 8, 8);
                out += 8;
            } else {
                dxt5_block((const u8 (*)[4])px, out);
                out += 16;
            }
        }
}

void mg_gltex_dxt5(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out) { encode_blocks(rgba, w, h, stride, out, false); }
void mg_gltex_bc1(const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out) { encode_blocks(rgba, w, h, stride, out, true); }

/* --------------------------------------------------- what is bound, per thread */

/* A thread draws with one context; what it has bound there. (An app that moves a context between threads is not followed.) */
static MG_THREAD_LOCAL struct {
    u32 unit;
    u32 bound[32];                     /* the 2D texture on each unit */
    u32 cube[32], array[32];           /* the cube map and the 2D array texture on each unit */
    u32 unpack_buffer;
    i32 row_length, skip_rows, skip_pixels;
} T;

static u32 bound_now(void) { return T.bound[T.unit]; }

/* ------------------------------------------------------ the textures held */

enum { S_NONE, S_PENDING, S_PACKED };

typedef struct tex_info {
    u8 state;
    u8 levels;
    u8 flags;                          /* TF_*: how a packed one is kept */
    u8 spare;
    u16 w, h;
    u32 internal;
} tex_info;

enum {
    TF_BC1 = 1,                        /* packed as BC1, opaque, rather than DXT5 */
    TF_MIPS = 2,                       /* its mipmaps made here from its first level */
    TF_MUTABLE = 4                     /* made with glTexImage2D, not glTexStorage2D */
};

/* By the texture's name: ANGLE hands names out from 1 upwards. One past the table is simply never held back. */
static tex_info g_info[16384];
static int g_can_pack = -1, g_can_pack_srgb, g_has_dxt5, g_etc_lossy;
static int g_policy = -1;              /* 0 never, 1 when the app has asked for much, 2 from the start */
static int g_etc_policy = -1;          /* 0: ETC as the app gave it (flag etc-by-angle); 1: in a format Direct3D has */
static int g_halve = -1;               /* 0: no texture halved (flag full-textures) */
static u8 g_etc[16384];                /* by texture name: 0 not known, 1 its ETC levels go as BC1 or DXT5, 2 as the app gave them, 3 halved */
static u16 g_etc_w[16384], g_etc_h[16384];    /* a halved texture's base size, as the app has it */
static u8 g_etc_levels[16384];         /* a halved texture's levels where storage fixed them (0: not fixed) */

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
        mg_info("%s: %.*s", what, (int)take, list + at);
        at += take;
        while (at < n && list[at] == ' ') at++;
    }
}

/* What this OpenGL has of DXT5 and of ANGLE's lossy ETC decoding (with a context current: asked at the first texture that could use them). */
static void learn_formats(void)
{
    const char *ext;
    if (g_can_pack >= 0) return;
    ext = h_get_string ? (const char *)h_get_string(GL_EXTENSIONS) : NULL;
    say_list("gl extensions", ext);
    g_has_dxt5 = ext && (has_word(ext, "GL_ANGLE_texture_compression_dxt5") || has_word(ext, "GL_EXT_texture_compression_s3tc")) ? 1 : 0;
    g_can_pack_srgb = g_has_dxt5 && has_word(ext, "GL_EXT_texture_compression_s3tc_srgb");
    g_etc_lossy = ext && has_word(ext, "GL_ANGLE_lossy_etc_decode");
    g_can_pack = g_has_dxt5 && h_compressed_sub_2d && h_bind_texture;
    mg_info("textures: large ones can%s be kept as DXT5 here%s; ETC2 RGBA8 ones can%s be given to ANGLE as DXT5, the RGB ones%s as BC1", g_can_pack ? "" : "not",
            g_can_pack && !g_can_pack_srgb ? " (not the sRGB ones)" : "", g_has_dxt5 ? "" : "not", g_etc_lossy ? "" : " not");
}

static bool can_pack(u32 internal)
{
    learn_formats();
    return g_can_pack && (internal == GL_RGBA8 || g_can_pack_srgb);
}

static int policy(void)
{
    if (g_policy < 0) g_policy = mg_flag("plain-textures") ? 0 : mg_flag("pack-textures") ? 2 : 1;
    return g_policy;
}

/* ------------------------------------------------------------- ETC kept small */

static bool is_etc(u32 internal) { return internal == 0x8D64 || (internal >= 0x9270 && internal <= 0x9279); }

/* The texture bound for a target on this thread's unit: 2D, a cube map (or one of its faces), a 2D array; 0 for any other. */
static u32 bound_for(u32 target)
{
    if (target == GL_TEXTURE_2D) return T.bound[T.unit];
    if (target == GL_TEXTURE_CUBE_MAP || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) return T.cube[T.unit];
    if (target == GL_TEXTURE_2D_ARRAY) return T.array[T.unit];
    return 0;
}

/* What an ETC format goes to ANGLE as, and whether its blocks are transcoded here on the way (to DXT5); 0: as it is. */
static u32 etc_as(u32 internal, bool *transcode)
{
    *transcode = false;
    if (g_etc_policy < 0) g_etc_policy = mg_flag("etc-by-angle") ? 0 : 1;
    if (!g_etc_policy) return 0;
    learn_formats();
    switch (internal) {
    case 0x8D64: return g_etc_lossy ? 0x9690 : 0;              /* ETC1 */
    case 0x9274: return g_etc_lossy ? 0x9695 : 0;              /* RGB8, its sRGB twin, the punch-through pair */
    case 0x9275: return g_etc_lossy ? 0x9696 : 0;
    case 0x9276: return g_etc_lossy ? 0x9697 : 0;
    case 0x9277: return g_etc_lossy ? 0x9698 : 0;
    case GL_RGBA8_ETC2_EAC: *transcode = g_has_dxt5 != 0; return g_has_dxt5 ? GL_DXT5 : 0;
    case GL_SRGB8_ALPHA8_ETC2_EAC: *transcode = g_can_pack_srgb != 0; return g_can_pack_srgb ? GL_DXT5_SRGB : 0;
    default: return 0;                                         /* R11 and RG11: as ANGLE has them */
    }
}

/* What an ETC format goes to ANGLE as when it is halved: DXT5 with alpha, BC1 without (sRGB where this OpenGL has it); 0: not halved. */
static u32 etc_half_as(u32 internal)
{
    bool srgb = internal == 0x9275 || internal == 0x9277 || internal == 0x9279;
    if (g_halve < 0) g_halve = mg_flag("full-textures") ? 0 : 1;
    if (g_etc_policy < 0) g_etc_policy = mg_flag("etc-by-angle") ? 0 : 1;
    if (!g_halve || !g_etc_policy || !(internal == 0x8D64 || (internal >= 0x9274 && internal <= 0x9279))) return 0;
    learn_formats();
    if (!g_has_dxt5 || (srgb && !g_can_pack_srgb)) return 0;
    return etc_alpha(internal) ? (srgb ? GL_DXT5_SRGB : GL_DXT5) : (srgb ? GL_DXT1_SRGB : GL_DXT1);
}

enum { ETC_ASIS, ETC_SAME, ETC_HALF };

/* How an ETC upload goes to ANGLE: as the app gave it; the same size (`to`, its blocks transcoded here or not); or halved (`to`). */
typedef struct etc_way {
    int mode;
    u32 to;
    bool transcode;
} etc_way;

/*
 * A level of the texture bound for `target`, in an ETC format: how it goes to ANGLE. Its first level decides for the whole
 * texture, which must be of one format. The base level that level implies must be whole blocks, as ANGLE over Direct3D asks
 * of a block-compressed level, and so must its half to be halved; a texture halved is one 1024 pixels a side or more at
 * its base. `anew`: the texture's base level or its storage is being made, which decides again.
 */
static etc_way etc_plan(u32 target, u32 internal, i32 level, i32 w, i32 h, bool anew)
{
    static volatile i32 said, said_half;
    etc_way way = { ETC_ASIS, 0, false };
    bool transcode;
    u32 name = bound_for(target), same = etc_as(internal, &transcode), half = etc_half_as(internal), bw, bh;
    u8 *known = name && name < MG_COUNT(g_etc) ? &g_etc[name] : NULL;
    if (!same && !half) return way;
    if (known && *known && !anew) {
        if (*known == 1 && same) {
            way.mode = ETC_SAME;
            way.to = same;
            way.transcode = transcode;
        } else if (*known == 3 && half) {
            way.mode = ETC_HALF;
            way.to = half;
        }
        return way;
    }
    if (level < 0 || level > 15 || w <= 0 || h <= 0) {
        if (known) *known = 2;
        return way;
    }
    bw = (u32)w << level;
    bh = (u32)h << level;
    if (half && (bw >= HALVE_FROM || bh >= HALVE_FROM) && !(bw / 2 & 3) && !(bh / 2 & 3)) {
        way.mode = ETC_HALF;
        way.to = half;
        if (known) {
            *known = 3;
            g_etc_w[name] = (u16)(bw > 65535 ? 65535 : bw);
            g_etc_h[name] = (u16)(bh > 65535 ? 65535 : bh);
            g_etc_levels[name] = 0;
        }
        if (!mg_atomic_xchg(&said_half, 1))
            mg_info("textures: ETC ones from %u pixels a side go to ANGLE at half their size, a quarter of the memory (BC1 or DXT5; flag full-textures keeps them whole)",
                    HALVE_FROM);
        return way;
    }
    if (same && !(bw & 3) && !(bh & 3)) {
        way.mode = ETC_SAME;
        way.to = same;
        way.transcode = transcode;
        if (known) *known = 1;
        if (!mg_atomic_xchg(&said, 1))
            mg_info("textures: ETC ones go to ANGLE in formats Direct3D has, the same size: the RGB ones as BC1 by ANGLE's lossy decoding, RGBA8 ETC2 EAC as DXT5 "
                    "transcoded here (ANGLE keeps ETC as RGBA8, four to eight times the size)");
        return way;
    }
    if (known) *known = 2;
    return way;
}

/* How an ETC upload into part of a level goes: as the texture's levels went. */
static etc_way etc_part_plan(u32 target, u32 format)
{
    etc_way way = { ETC_ASIS, 0, false };
    u32 name = bound_for(target);
    if (!is_etc(format) || !name || name >= MG_COUNT(g_etc)) return way;
    if (g_etc[name] == 1 && (way.to = etc_as(format, &way.transcode)) != 0) way.mode = ETC_SAME;
    else if (g_etc[name] == 3 && (way.to = etc_half_as(format)) != 0) way.mode = ETC_HALF;
    if (way.mode == ETC_ASIS) way.transcode = false;
    return way;
}

/*
 * The ETC blocks of an upload (`internal`, w x h, d slices) as ANGLE is to have them, in memory of their own (the caller
 * frees it), and their size: RGBA8 ETC2 EAC as DXT5 (ETC_SAME), or every slice halved (ETC_HALF). From the app's memory, or
 * from the unpack buffer bound, where `data` is an offset. NULL when they cannot be read, or the size is not the blocks'.
 */
static u8 *etc_converted(const etc_way *way, u32 internal, const void *data, i32 size, u32 w, u32 h, u32 d, size_t *out_size)
{
    size_t slice = (size_t)((w + 3) / 4) * ((h + 3) / 4) * etc_block_bytes(internal), out_slice, k;
    const u8 *in = (const u8 *)data;
    void *mapped = NULL;
    u8 *out;
    out_slice = way->mode == ETC_HALF ? mg_gltex_etc_halved_size(internal, w, h) : slice;
    *out_size = out_slice * d;
    if (size < 0 || (size_t)size != slice * d) return NULL;
    if (T.unpack_buffer) {
        if (!h_map_range || !h_unmap || !h_bind_buffer) return NULL;
        mapped = h_map_range(GL_PIXEL_UNPACK_BUFFER, (intptr_t)data, (intptr_t)size, GL_MAP_READ_BIT);
        if (!mapped) return NULL;
        in = (const u8 *)mapped;
    } else if (!in) {
        return NULL;
    }
    out = (u8 *)malloc(*out_size ? *out_size : 1);
    for (k = 0; out && k < d; k++) {
        if (way->mode == ETC_HALF) {
            if (!mg_gltex_etc_halve(internal, in + k * slice, w, h, out + k * out_slice)) {
                free(out);
                out = NULL;
            }
        } else {
            mg_gltex_etc2_eac_to_dxt5(in + k * slice, slice / 16, out + k * out_slice);
        }
    }
    if (mapped) h_unmap(GL_PIXEL_UNPACK_BUFFER);
    return out;
}

/* Around an upload from the layer's own memory: the app's unpack buffer is let go of, and bound again after. */
static void own_memory(bool begin)
{
    if (T.unpack_buffer && h_bind_buffer) h_bind_buffer(GL_PIXEL_UNPACK_BUFFER, begin ? 0 : T.unpack_buffer);
}

/* An ETC upload that was to be transcoded and could not be read: its level is made all the same, its pixels not given. */
static void etc_unread(const char *what, i32 w, i32 h)
{
    static volatile i32 said;
    mg_atomic_add((volatile i32 *)&G.dropped, 1);
    if (mg_atomic_add(&said, 1) <= 6) mg_warn("textures: an ETC2 upload (%s, %dx%d) to be given to ANGLE as BC1 or DXT5 could not be read; its level is left blank", what, w, h);
}

static u64 count(u32 target, u32 internal, u32 format, u32 type, i32 w, i32 h, i32 d, i32 levels, u64 given, bool as_bc);
static u64 storage_bytes(u32 internal, u32 format, u32 type, i32 w, i32 h, i32 d, i32 levels, int *kind, u64 *pixels_out);
static void held_set(u32 name, u64 bytes);
static void held_level(u32 target, i32 level, u64 bytes);

/* The levels a texture of this base size can have. */
static i32 levels_for(i32 w, i32 h)
{
    i32 n = 1, m = w > h ? w : h;
    while (m > 1) {
        m >>= 1;
        n++;
    }
    return n;
}

/* Storage of an ETC format halved: the half size, the levels it can have, and what the texture holds. */
static void etc_storage_halved(u32 target, i32 levels, u32 internal, u32 to, i32 w, i32 h, i32 d, bool three)
{
    u32 name = bound_for(target);
    i32 nw = w / 2, nh = h / 2, lv = levels < levels_for(nw, nh) ? levels : levels_for(nw, nh);
    int kind;
    u64 was, now;
    if (name && name < MG_COUNT(g_etc_levels)) g_etc_levels[name] = (u8)lv;
    was = count(target, internal, 0, 0, w, h, d, levels, 0, true);
    now = storage_bytes(to, 0, 0, nw, nh, d, lv, &kind, NULL) * (target == GL_TEXTURE_CUBE_MAP ? 6 : 1);
    held_set(name, now);
    mg_lock_acquire(&g_lock);
    G.halved_count++;
    G.halved_from += was;
    G.halved_to += now;
    mg_lock_release(&g_lock);
    if (three) h_storage_3d(target, lv, to, nw, nh, d);
    else h_storage_2d(target, lv, to, nw, nh);
}

/*
 * An ETC upload transcoded or halved on its way to ANGLE: a level (glCompressedTexImage2D/3D) or a part of one (`part`:
 * glCompressedTexSubImage2D/3D, at x, y, z). A halved part must start and end on whole blocks of the halved level; levels
 * past those a halved texture's storage has are not given.
 */
static void etc_send(u32 target, i32 level, u32 internal, const etc_way *way, bool part, bool three, i32 x, i32 y, i32 z, i32 w, i32 h, i32 d, i32 border,
                     i32 size, const void *data)
{
    static volatile i32 said;
    u32 name = bound_for(target);
    i32 nw = w, nh = h, nx = x, ny = y;
    size_t out_size = 0;
    u8 *out = NULL;
    if (way->mode == ETC_HALF) {
        nw = w > 1 ? w / 2 : 1;
        nh = h > 1 ? h / 2 : 1;
        nx = x / 2;
        ny = y / 2;
        if (name && name < MG_COUNT(g_etc_levels) && g_etc_levels[name] && level >= g_etc_levels[name]) return;
        if (part && name && name < MG_COUNT(g_etc_w)) {
            i32 lw = g_etc_w[name] >> level ? g_etc_w[name] >> level : 1, lh = g_etc_h[name] >> level ? g_etc_h[name] >> level : 1;
            if ((x & 7) || (y & 7) || ((w & 7) && x + w != lw) || ((h & 7) && y + h != lh)) {
                mg_atomic_add((volatile i32 *)&G.dropped, 1);
                if (mg_atomic_add(&said, 1) <= 6)
                    mg_warn("textures: a part %dx%d at %d,%d of level %d of a halved texture is not whole blocks once halved: not given", w, h, x, y, level);
                return;
            }
        }
    }
    if (data || T.unpack_buffer) {
        out = etc_converted(way, internal, data, size, (u32)w, (u32)h, (u32)(d > 0 ? d : 1), &out_size);
        if (!out) {
            etc_unread(part ? "a part of a level" : "a level", w, h);
            if (part) return;
        }
    } else {
        out_size = way->mode == ETC_HALF ? mg_gltex_etc_halved_size(internal, (u32)w, (u32)h) * (size_t)(d > 0 ? d : 1) : (size_t)(size > 0 ? size : 0);
    }
    if (!part) {
        u64 was = count(0, internal, 0, 0, w, h, d, 1, size > 0 ? (u64)size : 0, true);
        held_level(target, level, out_size);
        if (way->mode == ETC_HALF) {
            mg_lock_acquire(&g_lock);
            G.halved_count++;
            G.halved_from += was;
            G.halved_to += out_size;
            mg_lock_release(&g_lock);
        }
    }
    own_memory(true);
    if (part && three) h_compressed_sub_3d(target, level, nx, ny, z, nw, nh, d, way->to, (i32)out_size, out);
    else if (part) h_compressed_sub_2d(target, level, nx, ny, nw, nh, way->to, (i32)out_size, out);
    else if (three) h_compressed_3d(target, level, way->to, nw, nh, d, border, (i32)out_size, out);
    else h_compressed_2d(target, level, way->to, nw, nh, border, (i32)out_size, out);
    own_memory(false);
    free(out);
}

/* ---------------------------------------------------------------- counting */

static u32 bound_for(u32 target);

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

/* Counted, and what ANGLE keeps of it: its own size, or four bytes a pixel for ETC that ANGLE decodes. */
static u64 count(u32 target, u32 internal, u32 format, u32 type, i32 w, i32 h, i32 d, i32 levels, u64 given, bool as_bc)
{
    int kind;
    u64 bytes, pixels = 0;
    if (w <= 0 || h <= 0 || d <= 0 || levels <= 0) return 0;
    bytes = storage_bytes(internal, format, type, w, h, d, levels, &kind, &pixels);
    if (given) bytes = given;
    if (target == GL_TEXTURE_CUBE_MAP) {
        bytes *= 6;
        pixels *= 6;
    }
    mg_lock_acquire(&g_lock);
    G.bytes[kind] += bytes;
    G.count[kind]++;
    if (kind == K_ETC && as_bc) {
        G.etc_as_bc += bytes;
        G.etc_as_bc_count++;
    } else if (kind == K_ETC) {
        G.etc_expanded += pixels * 4;
    }
    if (bytes > G.largest_bytes) {
        G.largest_bytes = bytes;
        G.largest_w = (u32)w;
        G.largest_h = (u32)h;
        G.largest_format = internal;
    }
    mg_lock_release(&g_lock);
    return kind == K_ETC && !as_bc ? pixels * 4 : bytes;
}

/*
 * The textures held now, by name: what each one's levels came to as ANGLE keeps them, and their sum. A texture's storage
 * sets its figure; a level made adds to it, its base level (a cube map's first face) starting it again; a texture
 * deleted takes its figure away. Names past the table are not followed. A diagnosis's figure (trace-cpu, crash reports).
 */
static u32 g_held[16384];
static u64 g_held_sum;

static void held_set(u32 name, u64 bytes)
{
    if (!name || name >= MG_COUNT(g_held)) return;
    mg_lock_acquire(&g_lock);
    g_held_sum -= g_held[name];
    g_held[name] = (u32)(bytes > 0xFFFFFFFFu ? 0xFFFFFFFFu : bytes);
    g_held_sum += g_held[name];
    mg_lock_release(&g_lock);
}

static void held_level(u32 target, i32 level, u64 bytes)
{
    u32 name = bound_for(target);
    if (!name || name >= MG_COUNT(g_held)) return;
    mg_lock_acquire(&g_lock);
    if (level == 0 && (target < GL_TEXTURE_CUBE_MAP_POSITIVE_X + 1 || target > GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) {
        g_held_sum -= g_held[name];
        g_held[name] = 0;
    }
    g_held[name] += (u32)bytes;
    g_held_sum += (u32)bytes;
    mg_lock_release(&g_lock);
}

u64 mg_gltex_held_bytes(void)
{
    u64 sum;
    mg_lock_acquire(&g_lock);
    sum = g_held_sum;
    mg_lock_release(&g_lock);
    return sum;
}

/* ------------------------------------------------- holding back, and making */

static tex_info *info_of(u32 name) { return name && name < MG_COUNT(g_info) ? &g_info[name] : NULL; }

/*
 * Whether the process is short of addresses now (less than SHORT_OF_ADDRESSES_MB free), asked of the host at most once a
 * second: a walk through the whole address space. Said in the log the first time it is.
 */
static u64 g_short_at;                 /* when the host was last asked (addresses_short), and what it said */
static bool g_short_was, g_short_said;
static int g_short_forced = -1;        /* the tests' answer (mg_gltex_short_of_addresses); -1: the host's */

void mg_gltex_short_of_addresses(int forced) { g_short_forced = forced; }

static bool addresses_short(void)
{
    u64 now = mg_time_monotonic_ns();
    u32 free_mb = 0, largest = 0, reserved = 0, committed = 0;
    bool short_now, say = false;
    if (g_short_forced >= 0) return g_short_forced != 0;
    mg_lock_acquire(&g_lock);
    if (g_short_at && now - g_short_at < 1000000000ull) {
        short_now = g_short_was;
        mg_lock_release(&g_lock);
        return short_now;
    }
    g_short_at = now;
    mg_lock_release(&g_lock);
    short_now = mg_host_address_space(&free_mb, &largest, &reserved, &committed) && free_mb < SHORT_OF_ADDRESSES_MB;
    mg_lock_acquire(&g_lock);
    g_short_was = short_now;
    if (short_now && !g_short_said) say = g_short_said = true;
    mg_lock_release(&g_lock);
    if (say)
        mg_info("textures: the process has %u MB of addresses free (the largest stretch %u): textures of 1024x1024 and more are kept as BC1 or "
                "DXT5 from now on, their mipmaps made here", (unsigned)free_mb, (unsigned)largest);
    return short_now;
}

/* Whether every pixel of a level the app gives is opaque: it may be kept as BC1. */
static bool all_opaque(const u8 *rgba, u32 w, u32 h, size_t stride)
{
    u32 x, y;
    for (y = 0; y < h; y++) {
        const u8 *row = rgba + (size_t)y * stride;
        for (x = 0; x < w; x++)
            if (row[x * 4 + 3] != 255) return false;
    }
    return true;
}

/* The format a packed texture is given to ANGLE in, and the size of a level of it. */
static u32 packed_format(const tex_info *t)
{
    bool srgb = t->internal == GL_SRGB8_ALPHA8;
    return t->flags & TF_BC1 ? (srgb ? GL_DXT1_SRGB : GL_DXT1) : (srgb ? GL_DXT5_SRGB : GL_DXT5);
}

static size_t packed_size(const tex_info *t, u32 w, u32 h) { return mg_gltex_dxt5_size(w, h) / (t->flags & TF_BC1 ? 2 : 1); }

static void pack_level(const tex_info *t, const u8 *rgba, u32 w, u32 h, size_t stride, u8 *out)
{
    if (t->flags & TF_BC1) mg_gltex_bc1(rgba, w, h, stride, out);
    else mg_gltex_dxt5(rgba, w, h, stride, out);
}

/* A level of a packed texture given to ANGLE: the whole of it, made (glTexImage2D's) or filled (glTexStorage2D's). */
static void put_level(u32 target, const tex_info *t, i32 level, u32 w, u32 h, const u8 *packed, size_t size)
{
    if (t->flags & TF_MUTABLE) h_compressed_2d(target, level, packed_format(t), (i32)w, (i32)h, 0, (i32)size, packed);
    else h_compressed_sub_2d(target, level, 0, 0, (i32)w, (i32)h, packed_format(t), (i32)size, packed);
}

/*
 * The mipmaps of a packed texture, made from its first level (`rgba`, w x h) down to its last: each level's 2x2 pixels
 * averaged into the next one's, and packed as the texture is. Direct3D makes none for a compressed texture.
 */
static void make_mips(u32 target, tex_info *t, const u8 *rgba, u32 w, u32 h, size_t stride)
{
    u8 *from = NULL, *to = NULL, *packed = NULL;
    const u8 *src = rgba;
    size_t src_stride = stride;
    i32 level;
    t->flags |= TF_MIPS;
    for (level = 1; level < t->levels && (w > 1 || h > 1); level++) {
        u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, x, y, c;
        size_t size = packed_size(t, nw, nh);
        to = (u8 *)malloc((size_t)nw * nh * 4);
        packed = (u8 *)malloc(size);
        if (!to || !packed) break;
        for (y = 0; y < nh; y++) {
            const u8 *r0 = src + (size_t)(y * 2 < h ? y * 2 : h - 1) * src_stride;
            const u8 *r1 = src + (size_t)(y * 2 + 1 < h ? y * 2 + 1 : h - 1) * src_stride;
            u8 *o = to + (size_t)y * nw * 4;
            for (x = 0; x < nw; x++) {
                u32 x0 = (x * 2 < w ? x * 2 : w - 1) * 4, x1 = (x * 2 + 1 < w ? x * 2 + 1 : w - 1) * 4;
                for (c = 0; c < 4; c++) o[x * 4 + c] = (u8)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) >> 2);
            }
        }
        pack_level(t, to, nw, nh, (size_t)nw * 4, packed);
        put_level(target, t, level, nw, nh, packed, size);
        free(packed);
        packed = NULL;
        free(from);
        from = to;
        to = NULL;
        src = from;
        src_stride = (size_t)nw * 4;
        w = nw;
        h = nh;
    }
    free(to);
    free(packed);
    free(from);
}

/* The storage a held-back texture was asked with, made now. `name` is bound for it and what was bound is put back. */
static void make_plain(u32 name, tex_info *t)
{
    bool rebind = bound_now() != name;
    t->state = S_NONE;
    if (rebind) h_bind_texture(GL_TEXTURE_2D, name);
    held_set(name, count(GL_TEXTURE_2D, t->internal, 0, 0, t->w, t->h, 1, t->levels, 0, false));
    h_storage_2d(GL_TEXTURE_2D, t->levels, t->internal, t->w, t->h);
    if (rebind) h_bind_texture(GL_TEXTURE_2D, bound_now());
}

static void make_packed(tex_info *t, bool bc1)
{
    int kind;
    u64 plain = storage_bytes(t->internal, 0, 0, t->w, t->h, 1, t->levels, &kind, NULL), packed;
    t->state = S_PACKED;
    t->flags = bc1 ? TF_BC1 : 0;
    packed = plain / (bc1 ? 8 : 4);
    held_set((u32)(t - g_info), packed);
    h_storage_2d(GL_TEXTURE_2D, t->levels, packed_format(t), t->w, t->h);
    mg_lock_acquire(&g_lock);
    G.count[K_PACKED]++;
    G.bytes[K_PACKED] += packed;
    G.packed_from += plain;
    mg_lock_release(&g_lock);
}

/* A level 0 of glTexImage2D's that is large and plain RGBA8, while addresses are short: made packed, its mipmaps too. */
static bool image_packed(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 border, u32 format, u32 type, const void *pixels)
{
    u32 name = bound_now();
    tex_info *t = target == GL_TEXTURE_2D ? info_of(name) : NULL;
    size_t stride, size;
    u8 *packed;
    u64 plain, kept = 0;
    u32 lw, lh;
    i32 levels;
    if (!t || level != 0 || border || (internal != GL_RGBA && internal != GL_RGBA8) || format != GL_RGBA || type != GL_UNSIGNED_BYTE || !pixels ||
        T.unpack_buffer || T.skip_rows || T.skip_pixels || w <= 0 || h <= 0 || w > 16384 || h > 16384 || (w & 3) || (h & 3) ||
        (u32)w * (u32)h < PRESSED_MIN_PIXELS || !policy() || !can_pack(GL_RGBA8) || !addresses_short())
        return false;
    for (levels = 1, lw = (u32)w, lh = (u32)h; lw > 1 || lh > 1; levels++) {
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    stride = (size_t)(T.row_length > 0 ? T.row_length : w) * 4;
    t->state = S_PACKED;
    t->flags = TF_MUTABLE | (all_opaque((const u8 *)pixels, (u32)w, (u32)h, stride) ? TF_BC1 : 0);
    t->levels = (u8)levels;
    t->w = (u16)w;
    t->h = (u16)h;
    t->internal = GL_RGBA8;
    size = packed_size(t, (u32)w, (u32)h);
    packed = (u8 *)malloc(size);
    if (!packed) {
        t->state = S_NONE;
        t->flags = 0;
        return false;
    }
    pack_level(t, (const u8 *)pixels, (u32)w, (u32)h, stride, packed);
    put_level(target, t, 0, (u32)w, (u32)h, packed, size);
    free(packed);
    make_mips(target, t, (const u8 *)pixels, (u32)w, (u32)h, stride);
    for (level = 0, lw = (u32)w, lh = (u32)h; level < levels; level++) {
        kept += packed_size(t, lw, lh);
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    plain = kept * (t->flags & TF_BC1 ? 8 : 4);
    held_set(name, kept);
    mg_lock_acquire(&g_lock);
    G.count[K_PACKED]++;
    G.bytes[K_PACKED] += kept;
    G.packed_from += plain;
    mg_lock_release(&g_lock);
    return true;
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
    if (mg_atomic_add(&said, 1) <= 6) mg_warn("textures: %s on texture %u, which is kept as %s and cannot take it", what, (unsigned)name, t->flags & TF_BC1 ? "BC1" : "DXT5");
}

static void w_storage_2d(u32 target, i32 levels, u32 internal, i32 w, i32 h)
{
    u32 name = bound_now();
    tex_info *t = target == GL_TEXTURE_2D ? info_of(name) : NULL;
    bool much, pressed, transcode;
    if (t && t->state != S_NONE) t->state = S_NONE;            /* a name used again */
    if (t) t->flags = 0;
    if (is_etc(internal)) {
        etc_way way = etc_plan(target, internal, 0, w, h, true);
        if (way.mode == ETC_HALF) {
            etc_storage_halved(target, levels, internal, way.to, w, h, 1, false);
            return;
        }
        held_set(bound_for(target), count(target, internal, 0, 0, w, h, 1, levels, 0, way.mode == ETC_SAME));
        h_storage_2d(target, levels, way.mode == ETC_SAME ? way.to : internal, w, h);
        return;
    }
    (void)transcode;
    mg_lock_acquire(&g_lock);
    much = G.bytes[K_PLAIN] + G.packed_from >= PACK_AFTER_BYTES;
    mg_lock_release(&g_lock);
    pressed = w > 0 && h > 0 && (u32)w * (u32)h >= PRESSED_MIN_PIXELS && (internal == GL_RGBA8 || internal == GL_SRGB8_ALPHA8) && addresses_short();
    if (t && (internal == GL_RGBA8 || internal == GL_SRGB8_ALPHA8) && levels >= 1 && levels < 32 && w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
        !(w & 3) && !(h & 3) && (u32)w * (u32)h >= PACK_MIN_PIXELS && policy() && (much || pressed || policy() == 2) && can_pack(internal)) {
        t->state = S_PENDING;
        t->levels = (u8)levels;
        t->w = (u16)w;
        t->h = (u16)h;
        t->internal = internal;
        return;
    }
    held_set(bound_for(target), count(target, internal, 0, 0, w, h, 1, levels, 0, false));
    h_storage_2d(target, levels, internal, w, h);
}

static void w_sub_image_2d(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, const void *pixels)
{
    static volatile i32 said;
    tex_info *t = target == GL_TEXTURE_2D ? info_of(bound_now()) : NULL;
    bool ours = format == GL_RGBA && type == GL_UNSIGNED_BYTE && !T.unpack_buffer && pixels && !T.skip_rows && !T.skip_pixels && w > 0 && h > 0;
    if (t && t->state == S_PENDING) {
        i32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
        /* (BC1 for a large one whose first level is opaque throughout: what it is filled with later is taken to be so too.) */
        if (ours && level >= 0 && level < t->levels && x == 0 && y == 0 && w == lw && h == lh)
            make_packed(t, level == 0 && (u32)w * (u32)h >= PRESSED_MIN_PIXELS &&
                               all_opaque((const u8 *)pixels, (u32)w, (u32)h, (size_t)(T.row_length > 0 ? T.row_length : w) * 4));
        else make_plain(bound_now(), t);
    }
    if (t && t->state == S_PACKED) {
        i32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
        /* Whole blocks only: a piece must start on one and end on one, or at the level's edge. */
        bool fits = ours && level >= 0 && level < t->levels && !(x & 3) && !(y & 3) && x >= 0 && y >= 0 && x + w <= lw && y + h <= lh &&
                    (!(w & 3) || x + w == lw) && (!(h & 3) || y + h == lh);
        if (fits) {
            size_t stride = (size_t)(T.row_length > 0 ? T.row_length : w) * 4, size = packed_size(t, (u32)w, (u32)h);
            u8 *packed = (u8 *)malloc(size);
            if (packed) {
                pack_level(t, (const u8 *)pixels, (u32)w, (u32)h, stride, packed);
                h_compressed_sub_2d(target, level, x, y, w, h, packed_format(t), (i32)size, packed);
                free(packed);
                /* The first level given whole, of one with more levels: its mipmaps, made here from it (again, when it is given again). */
                if (level == 0 && x == 0 && y == 0 && w == lw && h == lh && t->levels > 1 && (u32)w * (u32)h >= PRESSED_MIN_PIXELS)
                    make_mips(target, t, (const u8 *)pixels, (u32)w, (u32)h, stride);
                return;
            }
        }
        mg_atomic_add((volatile i32 *)&G.dropped, 1);
        if (mg_atomic_add(&said, 1) <= 6)
            mg_warn("textures: an upload of %dx%d at %d,%d (level %d, format 0x%x type 0x%x) to a texture kept as DXT5 could not be done", w, h, x, y,
                    level, (unsigned)format, (unsigned)type);
        return;
    }
    h_sub_image_2d(target, level, x, y, w, h, format, type, pixels);
}

static void w_storage_3d(u32 target, i32 levels, u32 internal, i32 w, i32 h, i32 d)
{
    etc_way way = { ETC_ASIS, 0, false };
    if (is_etc(internal)) way = etc_plan(target, internal, 0, w, h, true);
    if (way.mode == ETC_HALF) {
        etc_storage_halved(target, levels, internal, way.to, w, h, d, true);
        return;
    }
    held_set(bound_for(target), count(target, internal, 0, 0, w, h, d, levels, 0, way.mode == ETC_SAME));
    h_storage_3d(target, levels, way.mode == ETC_SAME ? way.to : internal, w, h, d);
}

static void w_image_2d(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 border, u32 format, u32 type, const void *pixels)
{
    tex_info *t = target == GL_TEXTURE_2D ? info_of(bound_now()) : NULL;
    if (t && t->state == S_PACKED && (t->flags & TF_MUTABLE)) {
        i32 lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1;
        if (level > 0 && level < t->levels && w == lw && h == lh && format == GL_RGBA && type == GL_UNSIGNED_BYTE && pixels && !border &&
            (internal == GL_RGBA || internal == GL_RGBA8) && !T.unpack_buffer && !T.skip_rows && !T.skip_pixels) {
            /* A level of its own the app gives, in place of the one made here: packed the same way. */
            size_t stride = (size_t)(T.row_length > 0 ? T.row_length : w) * 4, size = packed_size(t, (u32)w, (u32)h);
            u8 *packed = (u8 *)malloc(size);
            if (packed) {
                pack_level(t, (const u8 *)pixels, (u32)w, (u32)h, stride, packed);
                put_level(target, t, level, (u32)w, (u32)h, packed, size);
                free(packed);
                return;
            }
        }
        /* Made again from its first level, or a level of another kind: as the app asks from now on. */
        t->state = S_NONE;
        t->flags = 0;
    }
    if (image_packed(target, level, internal, w, h, border, format, type, pixels)) return;
    if (target == GL_TEXTURE_2D) needs_plain(bound_now(), "glTexImage2D");
    held_level(target, level, count(0, (u32)internal, format, type, w, h, 1, 1, 0, false));
    h_image_2d(target, level, internal, w, h, border, format, type, pixels);
}

static void w_image_3d(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 d, i32 border, u32 format, u32 type, const void *pixels)
{
    held_level(target, level, count(0, (u32)internal, format, type, w, h, d, 1, 0, false));
    h_image_3d(target, level, internal, w, h, d, border, format, type, pixels);
}

static void w_compressed_2d(u32 target, i32 level, u32 internal, i32 w, i32 h, i32 border, i32 size, const void *data)
{
    etc_way way = { ETC_ASIS, 0, false };
    if (target == GL_TEXTURE_2D) needs_plain(bound_now(), "glCompressedTexImage2D");
    if (is_etc(internal)) way = etc_plan(target, internal, level, w, h, level == 0);
    if (way.mode == ETC_HALF || way.transcode) {
        etc_send(target, level, internal, &way, false, false, 0, 0, 0, w, h, 1, border, size, data);
        return;
    }
    held_level(target, level, count(0, internal, 0, 0, w, h, 1, 1, size > 0 ? (u64)size : 0, way.mode == ETC_SAME));
    h_compressed_2d(target, level, way.mode == ETC_SAME ? way.to : internal, w, h, border, size, data);
}

static void w_compressed_sub_2d(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, i32 size, const void *data)
{
    etc_way way = etc_part_plan(target, format);
    if (way.mode == ETC_HALF || way.transcode) {
        etc_send(target, level, format, &way, true, false, x, y, 0, w, h, 1, 0, size, data);
        return;
    }
    h_compressed_sub_2d(target, level, x, y, w, h, way.mode == ETC_SAME ? way.to : format, size, data);
}

static void w_compressed_3d(u32 target, i32 level, u32 internal, i32 w, i32 h, i32 d, i32 border, i32 size, const void *data)
{
    etc_way way = { ETC_ASIS, 0, false };
    if (is_etc(internal)) way = etc_plan(target, internal, level, w, h, level == 0);
    if ((way.mode == ETC_HALF || way.transcode) && d > 0) {
        etc_send(target, level, internal, &way, false, true, 0, 0, 0, w, h, d, border, size, data);
        return;
    }
    held_level(target, level, count(0, internal, 0, 0, w, h, d, 1, size > 0 ? (u64)size : 0, way.mode == ETC_SAME));
    h_compressed_3d(target, level, way.mode == ETC_SAME ? way.to : internal, w, h, d, border, size, data);
}

static void w_compressed_sub_3d(u32 target, i32 level, i32 x, i32 y, i32 z, i32 w, i32 h, i32 d, u32 format, i32 size, const void *data)
{
    etc_way way = etc_part_plan(target, format);
    if ((way.mode == ETC_HALF || way.transcode) && d > 0) {
        etc_send(target, level, format, &way, true, true, x, y, z, w, h, d, 0, size, data);
        return;
    }
    h_compressed_sub_3d(target, level, x, y, z, w, h, d, way.mode == ETC_SAME ? way.to : format, size, data);
}

static void w_active_texture(u32 unit)
{
    if (unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + MG_COUNT(T.bound)) T.unit = unit - GL_TEXTURE0;
    h_active_texture(unit);
}

static void w_bind_texture(u32 target, u32 name)
{
    if (target == GL_TEXTURE_2D) T.bound[T.unit] = name;
    else if (target == GL_TEXTURE_CUBE_MAP) T.cube[T.unit] = name;
    else if (target == GL_TEXTURE_2D_ARRAY) T.array[T.unit] = name;
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
        if (t) {
            t->state = S_NONE;
            t->flags = 0;
        }
        if (names[i] < MG_COUNT(g_etc)) {
            g_etc[names[i]] = 0;
            g_etc_levels[names[i]] = 0;
        }
        held_set(names[i], 0);
        for (u = 0; u < MG_COUNT(T.bound); u++) {
            if (T.bound[u] == names[i]) T.bound[u] = 0;
            if (T.cube[u] == names[i]) T.cube[u] = 0;
            if (T.array[u] == names[i]) T.array[u] = 0;
        }
    }
    h_delete_textures(n, names);
}

static void w_generate_mipmap(u32 target)
{
    tex_info *t = target == GL_TEXTURE_2D ? info_of(bound_now()) : NULL;
    static volatile i32 said;
    if (t && t->state == S_PENDING) make_plain(bound_now(), t);
    if (t && t->state == S_PACKED) {
        /* Direct3D makes no mipmaps for a compressed texture: they were made here when its first level came (TF_MIPS), else
           the levels the app did not upload stay as they are. */
        if (!(t->flags & TF_MIPS) && mg_atomic_add(&said, 1) <= 3) mg_warn("textures: mipmaps asked for a texture kept as DXT5; not made");
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

void *mg_gltex_wrap(const char *name, void *host)
{
    static const struct { const char *name; void *wrapper; void **host; } k[] = {
        { "glTexStorage2D", (void *)w_storage_2d, (void **)&h_storage_2d },
        { "glTexStorage2DEXT", (void *)w_storage_2d, (void **)&h_storage_2d },
        { "glTexStorage3D", (void *)w_storage_3d, (void **)&h_storage_3d },
        { "glTexImage2D", (void *)w_image_2d, (void **)&h_image_2d },
        { "glTexImage3D", (void *)w_image_3d, (void **)&h_image_3d },
        { "glCompressedTexImage2D", (void *)w_compressed_2d, (void **)&h_compressed_2d },
        { "glCompressedTexSubImage2D", (void *)w_compressed_sub_2d, (void **)&h_compressed_sub_2d },
        { "glCompressedTexImage3D", (void *)w_compressed_3d, (void **)&h_compressed_3d },
        { "glCompressedTexImage3DOES", (void *)w_compressed_3d, (void **)&h_compressed_3d },
        { "glCompressedTexSubImage3D", (void *)w_compressed_sub_3d, (void **)&h_compressed_sub_3d },
        { "glCompressedTexSubImage3DOES", (void *)w_compressed_sub_3d, (void **)&h_compressed_sub_3d },
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
    for (i = 0; i < MG_COUNT(k); i++)
        if (!strcmp(k[i].name, name)) {
            if (!*k[i].host) *k[i].host = host;
            /* (An EXT name and the core one are the same function in ANGLE; if they ever were not, the second is left alone.) */
            return *k[i].host == host ? k[i].wrapper : host;
        }
    return host;
}

void mg_gltex_set_host(void *(*resolve)(const char *name))
{
    /* What packing itself calls, whether or not the app ever asked for them by name. */
    if (!h_compressed_sub_2d) h_compressed_sub_2d = (fn_compressed_sub_2d)resolve("glCompressedTexSubImage2D");
    if (!h_bind_texture) h_bind_texture = (fn_enum_uint)resolve("glBindTexture");
    if (!h_get_string) h_get_string = (fn_get_string)resolve("glGetString");
    if (!h_storage_2d) h_storage_2d = (fn_tex_storage_2d)resolve("glTexStorage2D");
    /* And what reading an ETC upload out of the app's unpack buffer takes. */
    if (!h_map_range) h_map_range = (fn_map_range)resolve("glMapBufferRange");
    if (!h_unmap) h_unmap = (fn_unmap)resolve("glUnmapBuffer");
    if (!h_bind_buffer) h_bind_buffer = (fn_enum_uint)resolve("glBindBuffer");
}

void mg_gltex_reset(void)
{
    h_storage_2d = NULL;
    h_storage_3d = NULL;
    h_image_2d = NULL;
    h_image_3d = NULL;
    h_compressed_2d = NULL;
    h_sub_image_2d = NULL;
    h_compressed_sub_2d = NULL;
    h_compressed_3d = NULL;
    h_compressed_sub_3d = NULL;
    h_map_range = NULL;
    h_unmap = NULL;
    h_active_texture = h_generate_mipmap = NULL;
    h_bind_texture = h_bind_buffer = NULL;
    h_pixel_storei = NULL;
    h_delete_textures = NULL;
    h_fb_texture_2d = NULL;
    h_fb_texture_2d_ms = NULL;
    h_fb_texture_layer = NULL;
    h_copy_sub_2d = NULL;
    h_copy_image = NULL;
    h_get_string = NULL;
    mg_lock_acquire(&g_lock);
    memset(&G, 0, sizeof(G));
    mg_lock_release(&g_lock);
    memset(g_info, 0, sizeof(g_info));
    memset(g_etc, 0, sizeof(g_etc));
    memset(g_etc_w, 0, sizeof(g_etc_w));
    memset(g_etc_h, 0, sizeof(g_etc_h));
    memset(g_etc_levels, 0, sizeof(g_etc_levels));
    g_halve = -1;
    memset(g_held, 0, sizeof(g_held));
    g_held_sum = 0;
    memset(&T, 0, sizeof(T));
    g_can_pack = g_policy = g_etc_policy = -1;
    g_can_pack_srgb = g_has_dxt5 = g_etc_lossy = 0;
    g_short_at = 0;
    g_short_was = g_short_said = false;
    g_short_forced = -1;
}

void mg_gltex_report(char *out, size_t size)
{
    mg_lock_acquire(&g_lock);
    snprintf(out, size,
             "textures asked for so far: %u MB plain in %u, %u MB ETC2/EAC in %u (%u MB of it in %u given to ANGLE as BC1 or DXT5, the same size; "
             "the rest %u MB as ANGLE decodes it), %u MB other compressed in %u; "
             "the largest %ux%u of format 0x%x, %u MB; kept as DXT5 or BC1: %u, %u MB for what would have been %u MB (%u uploads dropped); halved: %u, %u MB "
             "for %u MB; held now %u MB",
             (unsigned)(G.bytes[K_PLAIN] >> 20), (unsigned)G.count[K_PLAIN], (unsigned)(G.bytes[K_ETC] >> 20), (unsigned)G.count[K_ETC],
             (unsigned)(G.etc_as_bc >> 20), (unsigned)G.etc_as_bc_count, (unsigned)(G.etc_expanded >> 20), (unsigned)(G.bytes[K_BLOCK] >> 20),
             (unsigned)G.count[K_BLOCK], (unsigned)G.largest_w,
             (unsigned)G.largest_h, (unsigned)G.largest_format, (unsigned)(G.largest_bytes >> 20), (unsigned)G.count[K_PACKED],
             (unsigned)(G.bytes[K_PACKED] >> 20), (unsigned)(G.packed_from >> 20), (unsigned)G.dropped, (unsigned)G.halved_count,
             (unsigned)(G.halved_to >> 20), (unsigned)(G.halved_from >> 20), (unsigned)(g_held_sum >> 20));
    mg_lock_release(&g_lock);
}
