/*
 * megler.Gfx's native methods (core/src/drivers/hk_java_gfx.c), called as the
 * runtime calls them: an hk_jcall with one value for each parameter of the
 * Java declaration, arrays and strings made by the runtime. The shell is the
 * stand-in, so what a call draws can be read back out of the bitmap.
 *
 * What is checked is this file's side of the work: a bitmap's pixels both
 * ways and in both byte orders, the canvas's own bookkeeping (save counts,
 * the matrix, the clip's box), the paint and the path as the Java packs
 * them, what is handed to the shell and when it is told to flush.
 */
#include <math.h>
#include <stdio.h>

#include "../src/drivers/hk_java_gfx.h"
#include "../src/dvm/hk_dvm.h"
#include "../src/webp/hk_webp.h"
#include "hk_gfx_standin.h"
#include "hk_gfx_tests.h"
#include "hk_webp_fixtures.inc"

static int t_checks, t_failed;
static hk_test_sink t_sink;
static void *t_sink_user;

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (t_sink) t_sink(line, t_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define CHECK(cond)                                                    \
    do {                                                               \
        t_checks++;                                                    \
        if (!(cond)) {                                                 \
            t_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

#define CHECK_INT(expr, want)                                                                      \
    do {                                                                                           \
        long long got_ = (long long)(expr), want_ = (long long)(want);                             \
        t_checks++;                                                                                \
        if (got_ != want_) {                                                                       \
            t_failed++;                                                                            \
            say("  FAIL %s:%d: %s = %lld, expected %lld", __FILE__, __LINE__, #expr, got_, want_); \
        }                                                                                          \
    } while (0)

#define CHECK_HEX(expr, want)                                                                        \
    do {                                                                                             \
        u32 got_ = (u32)(expr), want_ = (u32)(want);                                                 \
        t_checks++;                                                                                  \
        if (got_ != want_) {                                                                         \
            t_failed++;                                                                              \
            say("  FAIL %s:%d: %s = %08x, expected %08x", __FILE__, __LINE__, #expr, got_, want_);   \
        }                                                                                            \
    } while (0)

#define CHECK_FLOAT(expr, want)                                                                      \
    do {                                                                                             \
        double got_ = (double)(expr), want_ = (double)(want);                                        \
        t_checks++;                                                                                  \
        if (!(fabs(got_ - want_) <= 1e-3)) {                                                         \
            t_failed++;                                                                              \
            say("  FAIL %s:%d: %s = %g, expected %g", __FILE__, __LINE__, #expr, got_, want_);       \
        }                                                                                            \
    } while (0)

/* --------------------------------------------------------- calling a native */

static hk_jvalue J(i64 v)
{
    hk_jvalue x;
    x.j = v;
    return x;
}

static hk_jvalue I(i32 v)
{
    hk_jvalue x;
    x.j = 0;
    x.i = v;
    return x;
}

static hk_jvalue F(float v)
{
    hk_jvalue x;
    x.j = 0;
    x.f = v;
    return x;
}

static hk_jvalue Z(bool v)
{
    hk_jvalue x;
    x.j = 0;
    x.z = v;
    return x;
}

static hk_jvalue L(hk_jobj *v)
{
    hk_jvalue x;
    x.j = 0;
    x.l = v;
    return x;
}

/* A native of megler.Gfx by its name, with `count` values for its parameters, in the order of the Java declaration. */
static hk_jvalue call(const char *name, int count, ...)
{
    const hk_jhle *e;
    hk_jvalue args[16];
    hk_jcall c;
    va_list ap;
    int i;
    memset(&c, 0, sizeof(c));
    memset(args, 0, sizeof(args));
    va_start(ap, count);
    for (i = 0; i < count && i < 16; i++) args[i] = va_arg(ap, hk_jvalue);
    va_end(ap);
    for (e = hk_java_gfx_natives(); e->cls; e++)
        if (strcmp(e->name, name) == 0) break;
    if (!e->cls) {
        t_failed++;
        say("  FAIL: megler.Gfx has no native %s", name);
        return c.ret;
    }
    c.name = e->name;
    c.sig = e->sig;
    c.args = args;
    e->fn(&c);
    return c.ret;
}

static hk_jobj *ints(const i32 *v, int n)
{
    hk_jobj *a = hk_dvm_new_array("[I", n);
    if (a && v && n) memcpy(a->u.arr.data, v, sizeof(i32) * (size_t)n);
    return a;
}

static hk_jobj *floats(const float *v, int n)
{
    hk_jobj *a = hk_dvm_new_array("[F", n);
    if (a && v && n) memcpy(a->u.arr.data, v, sizeof(float) * (size_t)n);
    return a;
}

static hk_jobj *bytes(const void *v, int n)
{
    hk_jobj *a = hk_dvm_new_array("[B", n);
    if (a && v && n) memcpy(a->u.arr.data, v, (size_t)n);
    return a;
}

static i32 *ints_of(hk_jobj *a) { return (i32 *)a->u.arr.data; }
static float *floats_of(hk_jobj *a) { return (float *)a->u.arr.data; }

/* The exception a native left, as text, cleared; "" when there is none. */
static const char *thrown(void)
{
    static char text[300];
    text[0] = 0;
    if (hk_dvm_pending()) {
        hk_dvm_describe_pending(text, sizeof(text));
        hk_dvm_clear_pending();
    }
    return text;
}

/* android.graphics.Paint.mP as `new Paint()` leaves it: the slots are in hk_java_gfx.c's first comment. */
enum {
    P_COLOR, P_STYLE, P_STROKE, P_FLAGS, P_CAP, P_JOIN, P_TEXTSIZE, P_ALIGN, P_MITER, P_SHRAD, P_SHDX, P_SHDY, P_SHCOL,
    P_SCALEX, P_SKEWX, P_LSPACE, P_XFER, P_CFCOLOR, P_CFMODE, P_ALPHA, P_COUNT
};

static hk_jobj *paint(u32 color)
{
    hk_jobj *a = hk_dvm_new_array("[I", P_COUNT);
    i32 *p = ints_of(a);
    p[P_COLOR] = (i32)color;
    p[P_TEXTSIZE] = (i32)hk_fb(12);
    p[P_MITER] = (i32)hk_fb(4);
    p[P_SCALEX] = (i32)hk_fb(1);
    p[P_XFER] = 3;
    p[P_CFMODE] = -1;
    p[P_ALPHA] = 255;
    return a;
}

/* ---------------------------------------------------------- reading a bitmap */

static u32 word(i64 bm, int x, int y)
{
    hk_java_gfx_bitmap b;
    if (!hk_java_gfx_bitmap_info(bm, &b) || x < 0 || y < 0 || x >= b.width || y >= b.height) return 0xDEADBEEFu;
    return hk_rd32((const u8 *)b.pixels + (size_t)y * (size_t)b.stride + 4 * (size_t)x);
}

static int covered(i64 bm)
{
    hk_java_gfx_bitmap b;
    int x, y, n = 0;
    if (!hk_java_gfx_bitmap_info(bm, &b)) return -1;
    for (y = 0; y < b.height; y++)
        for (x = 0; x < b.width; x++) n += word(bm, x, y) != 0;
    return n;
}

/* The pixels that are not transparent are exactly x0..x1 by y0..y1. */
static bool exactly(i64 bm, int x0, int y0, int x1, int y1)
{
    hk_java_gfx_bitmap b;
    int x, y;
    if (!hk_java_gfx_bitmap_info(bm, &b)) return false;
    for (y = 0; y < b.height; y++)
        for (x = 0; x < b.width; x++)
            if ((word(bm, x, y) != 0) != (x >= x0 && x < x1 && y >= y0 && y < y1)) return false;
    return true;
}

static void erase(i64 bm) { call("bmErase", 2, J(bm), I(0)); }

static const float k_identity[6] = { 1, 0, 0, 0, 1, 0 };

/* -------------------------------------------------------------------- bitmaps */

static void test_bitmaps(void)
{
    uint64_t bytes0 = 0, bytes1 = 0;
    uint32_t count0 = 0, count1 = 0;
    hk_java_gfx_bitmap info;
    hk_jobj *a;
    i64 bm, copy;
    u8 raw[48];
    int i;
    say("  bitmaps: pixels as Java reads and writes them, in memory, and in Android's byte order");
    hk_java_gfx_stats(&bytes0, &count0);
    bm = call("bmNew", 2, I(4), I(3)).j;
    CHECK(bm != 0);
    hk_java_gfx_stats(&bytes1, &count1);
    CHECK_INT(bytes1 - bytes0, 48);
    CHECK_INT(count1 - count0, 1);
    CHECK(call("bmNew", 2, I(0), I(5)).j == 0);
    CHECK(call("bmNew", 2, I(5), I(-1)).j == 0);
    CHECK(call("bmNew", 2, I(100000), I(100000)).j == 0);
    CHECK(hk_java_gfx_bitmap_info(bm, &info));
    CHECK_INT(info.width, 4);
    CHECK_INT(info.height, 3);
    CHECK_INT(info.stride, 16);
    CHECK(call("bmAddress", 1, J(bm)).j == (i64)(hk_addr)info.pixels);
    CHECK(!hk_java_gfx_bitmap_info(0, &info));
    /* New, it is transparent. A colour set is premultiplied in memory, B, G, R, A, and comes back as it was set. */
    CHECK_INT(covered(bm), 0);
    call("bmSetPixel", 4, J(bm), I(1), I(2), I((i32)0x80FF0000u));
    CHECK_HEX(word(bm, 1, 2), 0x80800000u);
    CHECK_INT(((const u8 *)info.pixels)[2 * 16 + 4 + 0], 0x00);
    CHECK_INT(((const u8 *)info.pixels)[2 * 16 + 4 + 2], 0x80);
    CHECK_INT(((const u8 *)info.pixels)[2 * 16 + 4 + 3], 0x80);
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(1), I(2)).i, 0x80FF0000u);
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(0), I(0)).i, 0u);
    /* Outside the bitmap, and for no bitmap, nothing happens. */
    call("bmSetPixel", 4, J(bm), I(4), I(0), I(-1));
    call("bmSetPixel", 4, J(bm), I(0), I(-1), I(-1));
    call("bmSetPixel", 4, J(0), I(0), I(0), I(-1));
    CHECK_INT(call("bmGetPixel", 3, J(bm), I(9), I(9)).i, 0);
    CHECK_INT(call("bmGetPixel", 3, J(0), I(0), I(0)).i, 0);
    CHECK_INT(covered(bm), 1);
    call("bmErase", 2, J(bm), I((i32)0xFF112233u));
    CHECK_HEX(word(bm, 0, 0), 0xFF112233u);
    CHECK_HEX(word(bm, 3, 2), 0xFF112233u);
    call("bmErase", 2, J(bm), I(0x40FFFFFF));
    CHECK_HEX(word(bm, 2, 1), 0x40404040u);
    /* getPixels and setPixels: a part of the bitmap, at a place in the array, a stride apart. */
    {
        static const i32 in[] = { 0, (i32)0xFFFF0000u, (i32)0xFF00FF00u, 7, (i32)0xFF0000FFu, (i32)0x80FFFFFFu };
        a = ints(in, 6);
        call("bmPixels", 9, J(bm), L(a), I(1), I(3), I(2), I(1), I(2), I(2), Z(true));
        CHECK_HEX(word(bm, 2, 1), 0xFFFF0000u);
        CHECK_HEX(word(bm, 3, 1), 0xFF00FF00u);
        CHECK_HEX(word(bm, 2, 2), 0xFF0000FFu);
        CHECK_HEX(word(bm, 3, 2), 0x80808080u);
        CHECK_HEX(word(bm, 1, 1), 0x40404040u);
        a = ints(NULL, 8);
        call("bmPixels", 9, J(bm), L(a), I(0), I(4), I(0), I(1), I(4), I(2), Z(false));
        CHECK_HEX(ints_of(a)[0], 0x40FFFFFFu);
        CHECK_HEX(ints_of(a)[2], 0xFFFF0000u);
        CHECK_HEX(ints_of(a)[3], 0xFF00FF00u);
        CHECK_HEX(ints_of(a)[6], 0xFF0000FFu);
        CHECK_HEX(ints_of(a)[7], 0x80FFFFFFu);
        /* A negative stride reads the rows upwards. */
        memset(ints_of(a), 0, 32);
        call("bmPixels", 9, J(bm), L(a), I(4), I(-4), I(0), I(1), I(4), I(2), Z(false));
        CHECK_HEX(ints_of(a)[6], 0xFFFF0000u);
        CHECK_HEX(ints_of(a)[2], 0xFF0000FFu);
        CHECK(thrown()[0] == 0);
        /* What does not fit is an exception, and nothing is written. */
        call("bmPixels", 9, J(bm), L(a), I(0), I(4), I(2), I(1), I(3), I(2), Z(false));
        CHECK(strstr(thrown(), "IllegalArgumentException") != NULL);
        call("bmPixels", 9, J(bm), L(a), I(1), I(4), I(0), I(1), I(4), I(2), Z(true));
        CHECK(strstr(thrown(), "ArrayIndexOutOfBoundsException") != NULL);
        call("bmPixels", 9, J(bm), L(a), I(4), I(-8), I(0), I(1), I(4), I(2), Z(true));
        CHECK(strstr(thrown(), "ArrayIndexOutOfBoundsException") != NULL);
        call("bmPixels", 9, J(bm), L(NULL), I(0), I(4), I(0), I(0), I(1), I(1), Z(false));
        CHECK(strstr(thrown(), "IllegalArgumentException") != NULL);
        CHECK_HEX(word(bm, 2, 1), 0xFFFF0000u);
    }
    /* copyPixelsToBuffer and copyPixelsFromBuffer: the same pixels, premultiplied, as Android orders them, R, G, B, A. */
    memset(raw, 0xEE, sizeof(raw));
    call("bmRaw", 3, J(bm), J((i64)(hk_addr)raw), Z(false));
    CHECK(raw[16 + 8 + 0] == 0xFF && raw[16 + 8 + 1] == 0 && raw[16 + 8 + 2] == 0 && raw[16 + 8 + 3] == 0xFF);
    CHECK(raw[32 + 8 + 0] == 0 && raw[32 + 8 + 1] == 0 && raw[32 + 8 + 2] == 0xFF && raw[32 + 8 + 3] == 0xFF);
    CHECK(raw[0] == 0x40 && raw[3] == 0x40);
    for (i = 0; i < 12; i++) {
        raw[4 * i] = 1;
        raw[4 * i + 1] = 2;
        raw[4 * i + 2] = 3;
        raw[4 * i + 3] = 255;
    }
    call("bmRaw", 3, J(bm), J((i64)(hk_addr)raw), Z(true));
    CHECK_HEX(word(bm, 3, 2), 0xFF010203u);
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(0), I(0)).i, 0xFF010203u);
    call("bmRaw", 3, J(bm), J(0), Z(true));
    /* For the app's native code (libjnigraphics) the memory itself is turned to Android's order, and back. */
    call("bmSetPixel", 4, J(bm), I(0), I(0), I((i32)0xFF0A0B0Cu));
    CHECK(hk_java_gfx_lock_rgba(bm, &info));
    CHECK(((const u8 *)info.pixels)[0] == 0x0A && ((const u8 *)info.pixels)[1] == 0x0B && ((const u8 *)info.pixels)[2] == 0x0C);
    CHECK(hk_java_gfx_lock_rgba(bm, &info));
    CHECK(((const u8 *)info.pixels)[0] == 0x0A);
    ((u8 *)info.pixels)[1] = 0x77;
    hk_java_gfx_unlock_rgba(bm);
    CHECK(((const u8 *)info.pixels)[0] == 0x0A);
    hk_java_gfx_unlock_rgba(bm);
    CHECK_HEX(word(bm, 0, 0), 0xFF0A770Cu);
    CHECK(!hk_java_gfx_lock_rgba(0, &info));
    hk_java_gfx_unlock_rgba(0);
    /* createBitmap(source, x, y, w, h): a part, copied. */
    call("bmSetPixel", 4, J(bm), I(2), I(1), I((i32)0xFFFF0000u));
    call("bmSetPixel", 4, J(bm), I(3), I(1), I((i32)0xFF0000FFu));
    call("bmSetPixel", 4, J(bm), I(2), I(2), I((i32)0xFF00FF00u));
    call("bmSetPixel", 4, J(bm), I(3), I(2), I((i32)0x80FFFFFFu));
    copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(2), L(NULL), Z(false)).j;
    CHECK(copy != 0 && hk_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 2);
    CHECK_HEX(word(copy, 0, 0), 0xFFFF0000u);
    CHECK_HEX(word(copy, 1, 1), 0x80808080u);
    call("bmFree", 1, J(copy));
    CHECK(call("bmCopy", 7, J(bm), I(2), I(1), I(3), I(2), L(NULL), Z(false)).j == 0);
    CHECK(call("bmCopy", 7, J(bm), I(-1), I(1), I(2), I(2), L(NULL), Z(false)).j == 0);
    CHECK(call("bmCopy", 7, J(0), I(0), I(0), I(1), I(1), L(NULL), Z(false)).j == 0);
    /* createScaledBitmap: twice the size without the filter is every pixel four times. */
    {
        static const float twice[6] = { 2, 0, 0, 0, 2, 0 }, half[6] = { 0.5f, 0, 0, 0, 0.5f, 0 }, mirror[6] = { -1, 0, 0, 0, 1, 0 };
        static const float turn[6] = { 0, -1, 0, 1, 0, 0 };
        copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(2), L(floats(twice, 6)), Z(false)).j;
        CHECK(hk_java_gfx_bitmap_info(copy, &info) && info.width == 4 && info.height == 4);
        CHECK_HEX(word(copy, 0, 0), 0xFFFF0000u);
        CHECK_HEX(word(copy, 1, 1), 0xFFFF0000u);
        CHECK_HEX(word(copy, 2, 1), 0xFF0000FFu);
        CHECK_HEX(word(copy, 1, 2), 0xFF00FF00u);
        CHECK_HEX(word(copy, 3, 3), 0x80808080u);
        call("bmFree", 1, J(copy));
        /* With the filter, growing: the pixels in between are mixed, the corners are not. */
        copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(2), L(floats(twice, 6)), Z(true)).j;
        CHECK_HEX(word(copy, 0, 0), 0xFFFF0000u);
        CHECK_HEX(word(copy, 3, 0), 0xFF0000FFu);
        CHECK_HEX(word(copy, 1, 0), 0xFFBF0040u);
        CHECK_HEX(word(copy, 2, 0), 0xFF4000BFu);
        call("bmFree", 1, J(copy));
        /* With the filter, shrinking: every pixel that the new one covers, by how much. Here all four, a quarter each. */
        copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(2), L(floats(half, 6)), Z(true)).j;
        CHECK(hk_java_gfx_bitmap_info(copy, &info) && info.width == 1 && info.height == 1);
        CHECK_HEX(word(copy, 0, 0), 0xDF606060u);
        call("bmFree", 1, J(copy));
        /* A matrix that mirrors. */
        copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(2), L(floats(mirror, 6)), Z(false)).j;
        CHECK(hk_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 2);
        CHECK_HEX(word(copy, 0, 0), 0xFF0000FFu);
        CHECK_HEX(word(copy, 1, 0), 0xFFFF0000u);
        CHECK_HEX(word(copy, 0, 1), 0x80808080u);
        call("bmFree", 1, J(copy));
        /* A quarter turn is drawing, which the shell does: the row of two becomes a column of two. */
        copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(1), L(floats(turn, 6)), Z(false)).j;
        CHECK(hk_java_gfx_bitmap_info(copy, &info) && info.width == 1 && info.height == 2);
        CHECK_HEX(word(copy, 0, 0), 0xFFFF0000u);
        CHECK_HEX(word(copy, 0, 1), 0xFF0000FFu);
        call("bmFree", 1, J(copy));
    }
    /* compress and decode: a PNG and back. */
    {
        hk_jobj *png = call("bmCompress", 3, J(bm), I(1), I(90)).l, *wh = ints(NULL, 2);
        CHECK(png != NULL && png->u.arr.len > 8 && memcmp(png->u.arr.data, "\x89PNG", 4) == 0);
        if (png) {
            i32 n = (i32)png->u.arr.len;
            CHECK(call("bmDecode", 6, L(png), I(0), I(n), I(1), L(wh), Z(true)).j == 0);
            CHECK_INT(ints_of(wh)[0], 4);
            CHECK_INT(ints_of(wh)[1], 3);
            ints_of(wh)[0] = ints_of(wh)[1] = 0;
            copy = call("bmDecode", 6, L(png), I(0), I(n), I(1), L(wh), Z(false)).j;
            CHECK(copy != 0);
            CHECK_INT(ints_of(wh)[0], 4);
            CHECK_INT(ints_of(wh)[1], 3);
            CHECK_HEX(word(copy, 0, 0), 0xFF0A770Cu);
            CHECK_HEX(word(copy, 3, 2), 0x80808080u);
            CHECK_HEX(word(copy, 2, 1), 0xFFFF0000u);
            call("bmFree", 1, J(copy));
            /* inSampleSize 2: half the size, rounded down, never less than a pixel. */
            copy = call("bmDecode", 6, L(png), I(0), I(n), I(2), L(wh), Z(false)).j;
            CHECK(hk_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 1);
            CHECK_INT(ints_of(wh)[0], 2);
            CHECK_INT(ints_of(wh)[1], 1);
            call("bmFree", 1, J(copy));
            call("bmDecode", 6, L(png), I(0), I(n), I(8), L(wh), Z(true));
            CHECK_INT(ints_of(wh)[0], 1);
            CHECK_INT(ints_of(wh)[1], 1);
            /* The file can lie anywhere in the array; a range outside the array, or bytes that are no image, give 0. */
            {
                hk_jobj *padded = bytes(NULL, n + 10);
                memcpy((u8 *)padded->u.arr.data + 7, png->u.arr.data, (size_t)n);
                copy = call("bmDecode", 6, L(padded), I(7), I(n), I(1), L(NULL), Z(false)).j;
                CHECK(copy != 0);
                CHECK_HEX(word(copy, 0, 0), 0xFF0A770Cu);
                call("bmFree", 1, J(copy));
                CHECK(call("bmDecode", 6, L(padded), I(7), I(n + 4), I(1), L(wh), Z(false)).j == 0);
                CHECK(call("bmDecode", 6, L(padded), I(-1), I(n), I(1), L(wh), Z(false)).j == 0);
                CHECK(call("bmDecode", 6, L(padded), I(0), I(n), I(1), L(wh), Z(false)).j == 0);
                CHECK(call("bmDecode", 6, L(NULL), I(0), I(n), I(1), L(wh), Z(false)).j == 0);
            }
        }
        CHECK(call("bmCompress", 3, J(0), I(1), I(90)).l == NULL);
    }
    call("bmFree", 1, J(bm));
    call("bmFree", 1, J(0));
    hk_java_gfx_stats(&bytes1, &count1);
    CHECK_INT(bytes1, bytes0);
    CHECK_INT(count1, count0);
    CHECK(thrown()[0] == 0);
}

/* ------------------------------------------------------------------- canvases */

/* Whether each of a pixel's four parts is within `slack` of what it should be: a lossy picture's colours come back nearly. */
static bool near_word(u32 got, u32 want, int slack)
{
    int k;
    for (k = 0; k < 32; k += 8) {
        int d = (int)((got >> k) & 0xFF) - (int)((want >> k) & 0xFF);
        if (d < -slack || d > slack) return false;
    }
    return true;
}

/*
 * WebP pictures are decoded by the core itself, before the shell is asked (webp/hk_webp.h): the stand-in here reads PNG
 * and nothing else, as the phone's decoder reads no WebP. Five small files (tools/webp-fixtures.py), through
 * BitmapFactory's native as an app's pictures come: 32 x 32, in quadrants of 16.
 */
static void test_webp(void)
{
    hk_jobj *wh = ints(NULL, 2), *file;
    hk_java_gfx_bitmap info;
    i64 bm;
    if (!hk_webp_available()) {
        say("drawing natives: WebP -- this build has no WebP decoder in it (build/webp/src was not there)");
        /* The file is known for what it is all the same, and goes to the shell's decoder, which here says no. */
        CHECK(hk_webp_is(k_webp_lossless, sizeof(k_webp_lossless)) && !hk_webp_is("RIFF....WAVE", 12));
        CHECK(call("bmDecode", 6, L(bytes(k_webp_lossless, (int)sizeof(k_webp_lossless))), I(0), I((int)sizeof(k_webp_lossless)), I(1), L(wh), Z(false)).j == 0);
        return;
    }
    say("drawing natives: WebP pictures, decoded in the core");
    CHECK(hk_webp_is(k_webp_lossless, sizeof(k_webp_lossless)) && !hk_webp_is("RIFF....WAVE", 12) && !hk_webp_is(k_webp_lossless, 11));

    /* Lossless, with alpha: every pixel exactly, premultiplied -- opaque red, nothing, opaque blue, half-transparent white. */
    file = bytes(k_webp_lossless, (int)sizeof(k_webp_lossless));
    CHECK(call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossless)), I(1), L(wh), Z(true)).j == 0);
    CHECK(ints_of(wh)[0] == 32 && ints_of(wh)[1] == 32);
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossless)), I(1), L(wh), Z(false)).j;
    CHECK(bm != 0 && hk_java_gfx_bitmap_info(bm, &info) && info.width == 32 && info.height == 32);
    CHECK_HEX(word(bm, 3, 3), 0xFFFF0000u);
    CHECK_HEX(word(bm, 15, 15), 0xFFFF0000u);
    CHECK_HEX(word(bm, 16, 15), 0x00000000u);
    CHECK_HEX(word(bm, 3, 28), 0xFF0000FFu);
    CHECK_HEX(word(bm, 28, 28), 0x80808080u);
    /* And as the app's Java reads a pixel: not premultiplied. */
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(28), I(28)).i, 0x80FFFFFFu);
    call("bmFree", 1, J(bm));
    /* inSampleSize 2 and 4: the picture scaled while it is decoded, a quadrant now 8 and 4 pixels. */
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossless)), I(2), L(wh), Z(false)).j;
    CHECK(bm != 0 && hk_java_gfx_bitmap_info(bm, &info) && info.width == 16 && info.height == 16 && ints_of(wh)[0] == 16);
    CHECK_HEX(word(bm, 2, 2), 0xFFFF0000u);
    CHECK_HEX(word(bm, 13, 2), 0x00000000u);
    CHECK_HEX(word(bm, 2, 13), 0xFF0000FFu);
    CHECK_HEX(word(bm, 13, 13), 0x80808080u);
    call("bmFree", 1, J(bm));
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossless)), I(4), L(wh), Z(false)).j;
    CHECK(bm != 0 && hk_java_gfx_bitmap_info(bm, &info) && info.width == 8 && info.height == 8);
    CHECK_HEX(word(bm, 1, 1), 0xFFFF0000u);
    CHECK_HEX(word(bm, 6, 6), 0x80808080u);
    call("bmFree", 1, J(bm));

    /* Lossy: red, green, blue, white, each nearly. */
    file = bytes(k_webp_lossy, (int)sizeof(k_webp_lossy));
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossy)), I(1), L(wh), Z(false)).j;
    CHECK(bm != 0 && ints_of(wh)[0] == 32 && ints_of(wh)[1] == 32);
    say("  lossy: the four quadrants are %08x %08x %08x %08x", (unsigned)word(bm, 7, 7), (unsigned)word(bm, 24, 7), (unsigned)word(bm, 7, 24), (unsigned)word(bm, 24, 24));
    CHECK(near_word(word(bm, 7, 7), 0xFFFF0000u, 12) && near_word(word(bm, 24, 7), 0xFF00FF00u, 12) && near_word(word(bm, 7, 24), 0xFF0000FFu, 12)
          && near_word(word(bm, 24, 24), 0xFFFFFFFFu, 12));
    CHECK((word(bm, 0, 0) >> 24) == 0xFF && (word(bm, 31, 31) >> 24) == 0xFF);
    call("bmFree", 1, J(bm));

    /* Lossy with an alpha plane: opaque and transparent exactly, the half-transparent quadrant nearly (the encoder made 129 of 128), its colours premultiplied. */
    file = bytes(k_webp_lossy_alpha, (int)sizeof(k_webp_lossy_alpha));
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossy_alpha)), I(1), L(wh), Z(false)).j;
    CHECK(bm != 0 && ints_of(wh)[0] == 32);
    CHECK(near_word(word(bm, 7, 7), 0xFFFF0000u, 12) && near_word(word(bm, 7, 24), 0xFF0000FFu, 12));
    CHECK_HEX(word(bm, 24, 7), 0x00000000u);
    say("  lossy with alpha: the half-transparent white quadrant is %08x", (unsigned)word(bm, 24, 24));
    CHECK(near_word(word(bm, 24, 24), 0x80808080u, 8));
    call("bmFree", 1, J(bm));

    /* An animation: its first frame, which is all orange; the second, teal, is not looked at. */
    file = bytes(k_webp_anim, (int)sizeof(k_webp_anim));
    CHECK(call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_anim)), I(1), L(wh), Z(true)).j == 0 && ints_of(wh)[0] == 32 && ints_of(wh)[1] == 32);
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_anim)), I(1), L(wh), Z(false)).j;
    CHECK(bm != 0);
    CHECK_HEX(word(bm, 0, 0), 0xFFFF8000u);
    CHECK_HEX(word(bm, 31, 31), 0xFFFF8000u);
    call("bmFree", 1, J(bm));
    /* A first frame that does not fill its canvas: 16 x 16 of magenta at (8, 8), the canvas around it transparent; and the same at half the size. */
    file = bytes(k_webp_anim_part, (int)sizeof(k_webp_anim_part));
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_anim_part)), I(1), L(wh), Z(false)).j;
    CHECK(bm != 0 && ints_of(wh)[0] == 32 && ints_of(wh)[1] == 32);
    CHECK_HEX(word(bm, 8, 8), 0xFFFF00FFu);
    CHECK_HEX(word(bm, 23, 23), 0xFFFF00FFu);
    CHECK_HEX(word(bm, 7, 8), 0x00000000u);
    CHECK_HEX(word(bm, 24, 23), 0x00000000u);
    CHECK_HEX(word(bm, 0, 0), 0x00000000u);
    call("bmFree", 1, J(bm));
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_anim_part)), I(2), L(wh), Z(false)).j;
    CHECK(bm != 0 && ints_of(wh)[0] == 16);
    CHECK_HEX(word(bm, 4, 4), 0xFFFF00FFu);
    CHECK_HEX(word(bm, 3, 4), 0x00000000u);
    CHECK_HEX(word(bm, 12, 11), 0x00000000u);
    call("bmFree", 1, J(bm));

    /* What only begins like a WebP file is no picture: cut short, and with its inside overwritten. */
    {
        u8 broken[sizeof(k_webp_lossless)];
        memcpy(broken, k_webp_lossless, sizeof(broken));
        memset(broken + 20, 0xEE, sizeof(broken) - 20);
        CHECK(call("bmDecode", 6, L(bytes(broken, (int)sizeof(broken))), I(0), I((int)sizeof(broken)), I(1), L(wh), Z(false)).j == 0);
        CHECK(call("bmDecode", 6, L(bytes(k_webp_lossy, 40)), I(0), I(40), I(1), L(wh), Z(false)).j == 0);
        CHECK(call("bmDecode", 6, L(bytes(k_webp_lossless, 12)), I(0), I(12), I(1), L(wh), Z(true)).j == 0);
    }
}

static void test_canvas(void)
{
    static const float move[6] = { 1, 0, 5, 0, 1, 7 }, twice[6] = { 2, 0, 0, 0, 2, 0 }, turn[6] = { 0, -1, 10, 1, 0, 0 };
    hk_jobj *m = floats(NULL, 6), *box = ints(NULL, 4), *red = paint(0xFFFF0000u);
    i64 bm = call("bmNew", 2, I(16), I(16)).j, cv = call("cvNew", 1, J(bm)).j;
    say("  the canvas: save counts, the matrix, the clip's box");
    CHECK(cv != 0);
    CHECK(call("cvNew", 1, J(0)).j == 0);
    /* Canvas.save returns the count before it, which is what restoreToCount takes. */
    CHECK_INT(call("cvSaveCount", 1, J(cv)).i, 1);
    CHECK_INT(call("cvSave", 1, J(cv)).i, 1);
    CHECK_INT(call("cvSaveCount", 1, J(cv)).i, 2);
    CHECK_INT(call("cvSaveLayer", 6, J(cv), F(0), F(0), F(0), F(0), I(255)).i, 2);
    CHECK_INT(call("cvSaveLayerMode", 7, J(cv), F(0), F(0), F(8), F(8), I(128), I(6)).i, 3);
    CHECK_INT(call("cvSaveCount", 1, J(cv)).i, 4);
    call("cvRestore", 1, J(cv));
    CHECK_INT(call("cvSaveCount", 1, J(cv)).i, 3);
    call("cvRestoreTo", 2, J(cv), I(1));
    CHECK_INT(call("cvSaveCount", 1, J(cv)).i, 1);
    call("cvRestore", 1, J(cv));
    CHECK_INT(call("cvSaveCount", 1, J(cv)).i, 1);
    /* For no canvas the answers are a new canvas's, as the original gives them. */
    CHECK_INT(call("cvSave", 1, J(0)).i, 1);
    CHECK_INT(call("cvSaveCount", 1, J(0)).i, 1);
    /* translate(5, 7) and then scale(2, 2): what is drawn next is doubled first and moved second. */
    call("cvSave", 1, J(cv));
    call("cvMatrix", 3, J(cv), L(floats(move, 6)), Z(false));
    call("cvMatrix", 3, J(cv), L(floats(twice, 6)), Z(false));
    call("cvGetMatrix", 2, J(cv), L(m));
    CHECK(floats_of(m)[0] == 2 && floats_of(m)[1] == 0 && floats_of(m)[2] == 5 && floats_of(m)[3] == 0 && floats_of(m)[4] == 2 && floats_of(m)[5] == 7);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(2), L(red), J(0));
    CHECK(exactly(bm, 5, 7, 7, 11));
    /* The clip's box is asked for in the drawing's own coordinates: here the bitmap seen through the matrix, in whole numbers that take it in. */
    CHECK(call("cvClipBounds", 2, J(cv), L(box)).z);
    CHECK(ints_of(box)[0] == -3 && ints_of(box)[1] == -4 && ints_of(box)[2] == 6 && ints_of(box)[3] == 5);
    /* restore brings the matrix back as well, and the shell is told. */
    call("cvRestore", 1, J(cv));
    call("cvGetMatrix", 2, J(cv), L(m));
    CHECK(floats_of(m)[0] == 1 && floats_of(m)[2] == 0 && floats_of(m)[4] == 1 && floats_of(m)[5] == 0);
    erase(bm);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(2), L(red), J(0));
    CHECK(exactly(bm, 0, 0, 1, 2));
    /* setMatrix replaces. */
    call("cvMatrix", 3, J(cv), L(floats(twice, 6)), Z(false));
    call("cvMatrix", 3, J(cv), L(floats(move, 6)), Z(true));
    call("cvGetMatrix", 2, J(cv), L(m));
    CHECK(floats_of(m)[0] == 1 && floats_of(m)[2] == 5 && floats_of(m)[5] == 7);
    call("cvMatrix", 3, J(cv), L(floats(NULL, 3)), Z(true));
    call("cvMatrix", 3, J(cv), L(NULL), Z(true));
    call("cvGetMatrix", 2, J(cv), L(m));
    CHECK(floats_of(m)[2] == 5);
    /* The clip: what a clipRect answers, what the box becomes, what is drawn. */
    erase(bm);
    call("cvSave", 1, J(cv));
    CHECK(call("cvClipRect", 6, J(cv), F(0), F(0), F(4), F(3), I(1)).z);
    CHECK(call("cvClipBounds", 2, J(cv), L(box)).z);
    CHECK(ints_of(box)[0] == 0 && ints_of(box)[1] == 0 && ints_of(box)[2] == 4 && ints_of(box)[3] == 3);
    call("cvDrawColor", 3, J(cv), I((i32)0xFFFF0000u), I(3));
    CHECK(exactly(bm, 5, 7, 9, 10));
    /* A rectangle given the wrong way round is the same rectangle. */
    CHECK(call("cvClipRect", 6, J(cv), F(4), F(3), F(2), F(1), I(1)).z);
    CHECK(call("cvClipBounds", 2, J(cv), L(box)).z);
    CHECK(ints_of(box)[0] == 2 && ints_of(box)[1] == 1 && ints_of(box)[2] == 4 && ints_of(box)[3] == 3);
    /* A rectangle cut out that takes a whole side moves that side; one that takes it all leaves nothing, and says so. */
    CHECK(call("cvClipRect", 6, J(cv), F(3), F(0), F(9), F(9), I(0)).z);
    CHECK(call("cvClipBounds", 2, J(cv), L(box)).z);
    CHECK(ints_of(box)[0] == 2 && ints_of(box)[2] == 3);
    CHECK(!call("cvClipRect", 6, J(cv), F(0), F(0), F(9), F(9), I(0)).z);
    CHECK(!call("cvClipBounds", 2, J(cv), L(box)).z);
    CHECK(ints_of(box)[0] == 0 && ints_of(box)[1] == 0 && ints_of(box)[2] == 0 && ints_of(box)[3] == 0);
    erase(bm);
    call("cvDrawColor", 3, J(cv), I((i32)0xFFFF0000u), I(3));
    call("cvDrawRect", 7, J(cv), F(-50), F(-50), F(50), F(50), L(red), J(0));
    CHECK_INT(covered(bm), 0);
    call("cvRestore", 1, J(cv));
    CHECK(call("cvClipBounds", 2, J(cv), L(box)).z);
    CHECK(ints_of(box)[0] == -5 && ints_of(box)[1] == -7 && ints_of(box)[2] == 11 && ints_of(box)[3] == 9);
    /* Two clips that do not meet. */
    call("cvSave", 1, J(cv));
    CHECK(call("cvClipRect", 6, J(cv), F(0), F(0), F(2), F(2), I(1)).z);
    CHECK(!call("cvClipRect", 6, J(cv), F(3), F(3), F(5), F(5), I(1)).z);
    call("cvRestore", 1, J(cv));
    /* A path as the clip: the box of the path; a path with nothing in it leaves nothing. */
    {
        static const float tri[] = { 0, 1, 1, 1, 4, 1, 1, 1, 3, 4 }, nothing[] = { 0, 1, 1 };
        call("cvSave", 1, J(cv));
        CHECK(call("cvClipPath", 5, J(cv), L(floats(tri, 10)), I(10), I(0), I(1)).z);
        CHECK(call("cvClipBounds", 2, J(cv), L(box)).z);
        CHECK(ints_of(box)[0] == 1 && ints_of(box)[1] == 1 && ints_of(box)[2] == 4 && ints_of(box)[3] == 3);
        CHECK(call("cvClipPath", 5, J(cv), L(floats(tri, 10)), I(10), I(0), I(0)).z);
        CHECK(!call("cvClipPath", 5, J(cv), L(floats(nothing, 3)), I(3), I(0), I(1)).z);
        call("cvRestore", 1, J(cv));
        CHECK(call("cvClipPath", 5, J(cv), L(NULL), I(3), I(0), I(1)).z);
    }
    /* Under a quarter turn (x' = 10 - y, y' = x) the bitmap's box is on its side. */
    call("cvMatrix", 3, J(cv), L(floats(turn, 6)), Z(true));
    CHECK(call("cvClipBounds", 2, J(cv), L(box)).z);
    CHECK(ints_of(box)[0] == 0 && ints_of(box)[1] == -6 && ints_of(box)[2] == 16 && ints_of(box)[3] == 10);
    erase(bm);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(4), F(2), L(red), J(0));
    CHECK(exactly(bm, 8, 0, 10, 4));
    /* A layer's bounds clip, and its alpha fades what is drawn in it (as far as the stand-in does layers). */
    call("cvMatrix", 3, J(cv), L(floats(k_identity, 6)), Z(true));
    erase(bm);
    CHECK_INT(call("cvSaveLayer", 6, J(cv), F(0), F(0), F(8), F(16), I(128)).i, 1);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(16), F(16), L(red), J(0));
    call("cvRestoreTo", 2, J(cv), I(1));
    CHECK(exactly(bm, 0, 0, 8, 16));
    CHECK_HEX(word(bm, 3, 3), 0x80800000u);
    /* A canvas whose bitmap is recycled under it draws nothing and breaks nothing. */
    call("bmFree", 1, J(bm));
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(16), F(16), L(red), J(0));
    call("cvDrawColor", 3, J(cv), I(-1), I(3));
    CHECK_INT(call("cvSave", 1, J(cv)).i, 1);
    call("cvRestore", 1, J(cv));
    call("cvFree", 1, J(cv));
    call("cvFree", 1, J(0));
    CHECK(thrown()[0] == 0);
}

/* -------------------------------------------------------------------- drawing */

static void test_drawing(void)
{
    hk_jobj *red = paint(0xFFFF0000u), *p;
    i64 bm = call("bmNew", 2, I(20), I(20)).j, cv = call("cvNew", 1, J(bm)).j, src;
    say("  drawing: the paint and the path as the Java packs them");
    /* A rectangle the wrong way round is still that rectangle. */
    call("cvDrawRect", 7, J(cv), F(6), F(8), F(2), F(3), L(red), J(0));
    CHECK(exactly(bm, 2, 3, 6, 8));
    CHECK_HEX(word(bm, 2, 3), 0xFFFF0000u);
    /* No paint array, or one that is too short, is the default paint: opaque black. */
    erase(bm);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(2), F(2), L(NULL), J(0));
    call("cvDrawRect", 7, J(cv), F(4), F(0), F(6), F(2), L(ints(NULL, 5)), J(0));
    CHECK_HEX(word(bm, 1, 1), 0xFF000000u);
    CHECK_HEX(word(bm, 5, 1), 0xFF000000u);
    /* Slot 1 is the style and slot 2 the stroke's width, a float's bits: a frame 2 wide round 5..15. */
    erase(bm);
    p = paint(0xFFFF0000u);
    ints_of(p)[P_STYLE] = 1;
    ints_of(p)[P_STROKE] = (i32)hk_fb(2);
    call("cvDrawRect", 7, J(cv), F(5), F(5), F(15), F(15), L(p), J(0));
    CHECK_INT(covered(bm), 12 * 12 - 8 * 8);
    CHECK_HEX(word(bm, 10, 10), 0u);
    /* Slot 16 is the transfer mode: SRC replaces what is there, DST draws nothing at all. */
    call("cvDrawColor", 3, J(cv), I((i32)0xFF0000FFu), I(1));
    p = paint(0x80FF0000u);
    ints_of(p)[P_XFER] = 1;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(2), F(2), L(p), J(0));
    CHECK_HEX(word(bm, 1, 1), 0x80800000u);
    ints_of(p)[P_XFER] = 2;
    call("cvDrawRect", 7, J(cv), F(4), F(4), F(6), F(6), L(p), J(0));
    call("cvDrawColor", 3, J(cv), I(-1), I(2));
    CHECK_HEX(word(bm, 5, 5), 0xFF0000FFu);
    /* A mode the framework has no number for is SRC_OVER. */
    ints_of(p)[P_XFER] = 99;
    call("cvDrawRect", 7, J(cv), F(4), F(4), F(6), F(6), L(p), J(0));
    CHECK_HEX(word(bm, 5, 5), 0xFF80007Fu);
    /* drawColor's modes. */
    call("cvDrawColor", 3, J(cv), I((i32)0x40FF0000u), I(1));
    CHECK_HEX(word(bm, 9, 9), 0x40400000u);
    call("cvDrawColor", 3, J(cv), I(-1), I(0));
    CHECK_INT(covered(bm), 0);
    /* Slots 17 and 18 are a colour filter. For a plain colour it is worked out before the shell sees the paint: white through SRC_IN red is red ... */
    p = paint(0xFFFFFFFFu);
    ints_of(p)[P_CFCOLOR] = (i32)0xFFFF0000u;
    ints_of(p)[P_CFMODE] = 5;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(2), F(2), L(p), J(0));
    CHECK_HEX(word(bm, 1, 1), 0xFFFF0000u);
    /* ... half-transparent white through it is half-transparent red, through MULTIPLY by a grey it is darker and as transparent, */
    ints_of(p)[P_COLOR] = (i32)0x80FFFFFFu;
    call("cvDrawRect", 7, J(cv), F(4), F(0), F(6), F(2), L(p), J(0));
    CHECK_HEX(word(bm, 5, 1), 0x80800000u);
    ints_of(p)[P_COLOR] = (i32)0xFFFFFFFFu;
    ints_of(p)[P_CFCOLOR] = (i32)0xFF808080u;
    ints_of(p)[P_CFMODE] = 13;
    call("cvDrawRect", 7, J(cv), F(8), F(0), F(10), F(2), L(p), J(0));
    CHECK_HEX(word(bm, 9, 1), 0xFF808080u);
    /* ... and through SRC_OVER an opaque colour it is that colour. */
    ints_of(p)[P_CFCOLOR] = (i32)0xFF00FF00u;
    ints_of(p)[P_CFMODE] = 3;
    call("cvDrawRect", 7, J(cv), F(12), F(0), F(14), F(2), L(p), J(0));
    CHECK_HEX(word(bm, 13, 1), 0xFF00FF00u);
    /* Round rectangles and ovals; a round rectangle without radii is a rectangle. */
    erase(bm);
    call("cvDrawRRect", 9, J(cv), F(0), F(0), F(20), F(20), F(8), F(8), L(red), J(0));
    CHECK_HEX(word(bm, 0, 0), 0u);
    CHECK_HEX(word(bm, 2, 2), 0xFFFF0000u);
    CHECK_HEX(word(bm, 19, 19), 0u);
    CHECK_HEX(word(bm, 10, 10), 0xFFFF0000u);
    erase(bm);
    call("cvDrawRRect", 9, J(cv), F(0), F(0), F(20), F(20), F(0), F(8), L(red), J(0));
    CHECK_INT(covered(bm), 400);
    /* Radii larger than half the size are half the size: here the circle. */
    erase(bm);
    call("cvDrawRRect", 9, J(cv), F(0), F(0), F(20), F(20), F(50), F(50), L(red), J(0));
    CHECK(covered(bm) >= 304 && covered(bm) <= 324);
    {
        int round_rect = covered(bm);
        erase(bm);
        call("cvDrawOval", 7, J(cv), F(0), F(0), F(20), F(20), L(red), J(0));
        CHECK_INT(covered(bm), round_rect);
        /* drawArc: a whole turn is the oval; */
        erase(bm);
        call("cvDrawArc", 10, J(cv), F(0), F(0), F(20), F(20), F(0), F(360), Z(false), L(red), J(0));
        CHECK(abs(covered(bm) - round_rect) <= 4);
    }
    /* a quarter from three o'clock on, to the centre, is the wedge below and to the right: angles grow clockwise, y being down. */
    erase(bm);
    call("cvDrawArc", 10, J(cv), F(0), F(0), F(20), F(20), F(0), F(90), Z(true), L(red), J(0));
    CHECK_HEX(word(bm, 15, 15), 0xFFFF0000u);
    CHECK_HEX(word(bm, 11, 18), 0xFFFF0000u);
    CHECK_HEX(word(bm, 18, 11), 0xFFFF0000u);
    CHECK_HEX(word(bm, 5, 5), 0u);
    CHECK_HEX(word(bm, 15, 5), 0u);
    CHECK_HEX(word(bm, 5, 15), 0u);
    CHECK_HEX(word(bm, 18, 18), 0u);
    CHECK(covered(bm) >= 72 && covered(bm) <= 84);
    /* Without the centre it is the piece between the arc and its chord; a sweep backwards goes anticlockwise. */
    erase(bm);
    call("cvDrawArc", 10, J(cv), F(0), F(0), F(20), F(20), F(0), F(-90), Z(false), L(red), J(0));
    CHECK_HEX(word(bm, 17, 2), 0u);
    CHECK_HEX(word(bm, 16, 4), 0xFFFF0000u);
    CHECK_HEX(word(bm, 12, 8), 0u);
    CHECK_HEX(word(bm, 15, 15), 0u);
    /* Paths, by the framework's operations (hk_java_gfx.c's first comment). A triangle by relative lines: */
    erase(bm);
    {
        static const float tri[] = { 0, 0, 0, 11, 10, 0, 11, -10, 10, 4 };
        call("cvDrawPath", 6, J(cv), L(floats(tri, 10)), I(10), I(0), L(red), J(0));
        CHECK_INT(covered(bm), 45);
        CHECK_HEX(word(bm, 4, 4), 0xFFFF0000u);
        CHECK_HEX(word(bm, 5, 5), 0u);
        /* Only the count given is the path: the rest of the array is not read. */
        erase(bm);
        call("cvDrawPath", 6, J(cv), L(floats(tri, 10)), I(3), I(0), L(red), J(0));
        CHECK_INT(covered(bm), 0);
    }
    /* A rectangle and a round rectangle as operations, then everything so far moved by a matrix, then an oval that is not moved. */
    {
        static const float ops[] = { 6, 0, 0, 4, 3, 0, 8, 6, 0, 14, 8, 4, 4, 0, 9, 1, 0, 2, 0, 1, 5, 7, 0, 12, 4, 16, 0 };
        call("cvDrawPath", 6, J(cv), L(floats(ops, 27)), I(27), I(0), L(red), J(0));
        CHECK_HEX(word(bm, 2, 5), 0xFFFF0000u);
        CHECK_HEX(word(bm, 5, 7), 0xFFFF0000u);
        CHECK_HEX(word(bm, 1, 5), 0u);
        CHECK_HEX(word(bm, 2, 4), 0u);
        CHECK_HEX(word(bm, 6, 5), 0u);
        CHECK_HEX(word(bm, 9, 6), 0xFFFF0000u);
        CHECK_HEX(word(bm, 12, 9), 0xFFFF0000u);
        CHECK_HEX(word(bm, 8, 5), 0u);
        CHECK_HEX(word(bm, 15, 12), 0u);
        CHECK_HEX(word(bm, 2, 14), 0xFFFF0000u);
        CHECK_HEX(word(bm, 0, 12), 0u);
        CHECK_HEX(word(bm, 2, 2), 0u);
    }
    /* Two squares one inside the other, both the same way round: the fill rule beside the path decides about the inner one. */
    {
        static const float two[] = { 6, 0, 0, 10, 10, 0, 6, 3, 3, 7, 7, 0 };
        erase(bm);
        call("cvDrawPath", 6, J(cv), L(floats(two, 12)), I(12), I(0), L(red), J(0));
        CHECK_INT(covered(bm), 100);
        erase(bm);
        call("cvDrawPath", 6, J(cv), L(floats(two, 12)), I(12), I(1), L(red), J(0));
        CHECK_INT(covered(bm), 84);
    }
    /* An arc as an operation, joined to what is there by a line: a line down the middle, then the right half of the circle, closed. */
    {
        static const float half[] = { 0, 10, 0, 5, 0, 0, 20, 20, -90, 180, 0, 4 };
        erase(bm);
        call("cvDrawPath", 6, J(cv), L(floats(half, 12)), I(12), I(0), L(red), J(0));
        CHECK_HEX(word(bm, 15, 10), 0xFFFF0000u);
        CHECK_HEX(word(bm, 11, 2), 0xFFFF0000u);
        CHECK_HEX(word(bm, 5, 10), 0u);
        CHECK_HEX(word(bm, 9, 10), 0u);
        CHECK(covered(bm) >= 148 && covered(bm) <= 164);
    }
    /* A stream cut off in the middle of an operation, or with a number that is none, ends there without harm. */
    {
        static const float bad[] = { 6, 0, 0, 4, 4, 0, 3, 1, 2, 3 }, worse[] = { 6, 0, 0, 4, 4, 0, 77, 1, 2, 3 };
        erase(bm);
        call("cvDrawPath", 6, J(cv), L(floats(bad, 10)), I(10), I(0), L(red), J(0));
        CHECK(exactly(bm, 0, 0, 4, 4));
        call("cvDrawPath", 6, J(cv), L(floats(worse, 10)), I(10), I(0), L(red), J(0));
        call("cvDrawPath", 6, J(cv), L(floats(worse, 10)), I(1000), I(0), L(red), J(0));
        call("cvDrawPath", 6, J(cv), L(floats(worse, 10)), I(-5), I(0), L(red), J(0));
        call("cvDrawPath", 6, J(cv), L(NULL), I(10), I(0), L(red), J(0));
        CHECK(exactly(bm, 0, 0, 4, 4));
    }
    /* drawLines: every four floats a line, stroked whatever the style says. Slot 2 is the width. */
    {
        static const float pts[] = { 99, 2, 10.5f, 12, 10.5f, 3.5f, 0.5f, 3.5f, 4.5f, 77 };
        erase(bm);
        p = paint(0xFFFF0000u);
        ints_of(p)[P_STROKE] = (i32)hk_fb(1);
        call("cvDrawLines", 7, J(cv), L(floats(pts, 10)), I(1), I(8), L(p), J(0), Z(false));
        CHECK_INT(covered(bm), 10 + 4);
        CHECK_HEX(word(bm, 2, 10), 0xFFFF0000u);
        CHECK_HEX(word(bm, 11, 10), 0xFFFF0000u);
        CHECK_HEX(word(bm, 3, 3), 0xFFFF0000u);
        /* A hairline (width 0) is a pixel wide. */
        erase(bm);
        call("cvDrawLines", 7, J(cv), L(floats(pts, 10)), I(1), I(4), L(red), J(0), Z(false));
        CHECK(exactly(bm, 2, 10, 12, 11));
        /* What does not fit in the array is not drawn. */
        erase(bm);
        call("cvDrawLines", 7, J(cv), L(floats(pts, 10)), I(4), I(8), L(red), J(0), Z(false));
        call("cvDrawLines", 7, J(cv), L(floats(pts, 10)), I(-1), I(4), L(red), J(0), Z(false));
        CHECK_INT(covered(bm), 0);
        /* drawPoints: a square as wide as the stroke round each point; for a hairline, a pixel. */
        ints_of(p)[P_STROKE] = (i32)hk_fb(4);
        call("cvDrawLines", 7, J(cv), L(floats(pts, 10)), I(1), I(2), L(p), J(0), Z(true));
        CHECK(exactly(bm, 0, 9, 4, 13) || exactly(bm, 0, 8, 4, 12));
        erase(bm);
        call("cvDrawLines", 7, J(cv), L(floats(pts, 10)), I(5), I(4), L(red), J(0), Z(true));
        CHECK_INT(covered(bm), 2);
        CHECK_HEX(word(bm, 3, 0), 0xFFFF0000u);
        CHECK_HEX(word(bm, 3, 4), 0xFFFF0000u);
    }
    /* Bitmaps. A source rectangle; no paint; the paint's alpha (the top byte of slot 0). */
    src = call("bmNew", 2, I(2), I(2)).j;
    call("bmSetPixel", 4, J(src), I(0), I(0), I((i32)0xFFFF0000u));
    call("bmSetPixel", 4, J(src), I(1), I(0), I((i32)0xFF00FF00u));
    call("bmSetPixel", 4, J(src), I(0), I(1), I((i32)0xFF0000FFu));
    call("bmSetPixel", 4, J(src), I(1), I(1), I((i32)0xFFFFFFFFu));
    erase(bm);
    call("cvDrawBitmap", 11, J(cv), J(src), F(0), F(0), F(2), F(2), F(4), F(4), F(12), F(12), L(NULL));
    CHECK(exactly(bm, 4, 4, 12, 12));
    CHECK_HEX(word(bm, 4, 4), 0xFFFF0000u);
    CHECK_HEX(word(bm, 8, 4), 0xFF00FF00u);
    CHECK_HEX(word(bm, 7, 8), 0xFF0000FFu);
    CHECK_HEX(word(bm, 11, 11), 0xFFFFFFFFu);
    erase(bm);
    call("cvDrawBitmap", 11, J(cv), J(src), F(1), F(0), F(2), F(1), F(0), F(0), F(3), F(3), L(paint(0x80000000u)));
    CHECK(exactly(bm, 0, 0, 3, 3));
    CHECK_HEX(word(bm, 1, 1), 0x80008000u);
    /* A source that reaches outside the bitmap is cut to it, and the destination by as much: here the right half of the source is not there. */
    erase(bm);
    call("cvDrawBitmap", 11, J(cv), J(src), F(0), F(0), F(4), F(2), F(0), F(0), F(8), F(4), L(NULL));
    CHECK(exactly(bm, 0, 0, 4, 4));
    /* Nothing for a source or a destination without area, a bitmap that is none, a canvas that is none. */
    erase(bm);
    call("cvDrawBitmap", 11, J(cv), J(src), F(1), F(0), F(1), F(2), F(0), F(0), F(8), F(8), L(NULL));
    call("cvDrawBitmap", 11, J(cv), J(src), F(0), F(0), F(2), F(2), F(8), F(0), F(0), F(8), L(NULL));
    call("cvDrawBitmap", 11, J(cv), J(0), F(0), F(0), F(2), F(2), F(0), F(0), F(8), F(8), L(NULL));
    call("cvDrawBitmap", 11, J(0), J(src), F(0), F(0), F(2), F(2), F(0), F(0), F(8), F(8), L(NULL));
    CHECK_INT(covered(bm), 0);
    /* A colour filter on a bitmap is the shell's to do: the paint goes to it with the filter in it. */
    p = paint(0xFF000000u);
    ints_of(p)[P_CFCOLOR] = (i32)0xFF112233u;
    ints_of(p)[P_CFMODE] = 5;
    call("cvDrawBitmap", 11, J(cv), J(src), F(0), F(0), F(2), F(2), F(0), F(0), F(2), F(2), L(p));
    CHECK_HEX(word(bm, 0, 0), 0xFF112233u);
    CHECK_HEX(word(bm, 1, 1), 0xFF112233u);
    /* drawBitmap(bitmap, matrix, paint): the matrix is the canvas's for the one call, under the canvas's own. */
    {
        static const float m[6] = { 2, 0, 1, 0, 3, 2 }, move[6] = { 1, 0, 4, 0, 1, 4 };
        hk_jobj *got = floats(NULL, 6);
        erase(bm);
        call("cvMatrix", 3, J(cv), L(floats(move, 6)), Z(true));
        call("cvDrawBitmapM", 4, J(cv), J(src), L(floats(m, 6)), L(NULL));
        CHECK(exactly(bm, 5, 6, 9, 12));
        CHECK_HEX(word(bm, 5, 6), 0xFFFF0000u);
        CHECK_HEX(word(bm, 8, 11), 0xFFFFFFFFu);
        call("cvGetMatrix", 2, J(cv), L(got));
        CHECK(floats_of(got)[0] == 1 && floats_of(got)[2] == 4 && floats_of(got)[4] == 1 && floats_of(got)[5] == 4);
        erase(bm);
        call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(red), J(0));
        CHECK(exactly(bm, 4, 4, 5, 5));
        call("cvMatrix", 3, J(cv), L(floats(k_identity, 6)), Z(true));
    }
    /* A bitmap drawn into itself, and one drawn from while a canvas on it has drawn. */
    {
        i64 cv2 = call("cvNew", 1, J(src)).j;
        erase(bm);
        call("cvDrawRect", 7, J(cv2), F(0), F(0), F(2), F(1), L(red), J(0));
        call("cvDrawBitmap", 11, J(cv), J(src), F(0), F(0), F(2), F(2), F(0), F(0), F(2), F(2), L(NULL));
        CHECK_HEX(word(bm, 1, 0), 0xFFFF0000u);
        CHECK_HEX(word(bm, 1, 1), 0xFFFFFFFFu);
        call("cvDrawBitmap", 11, J(cv), J(bm), F(0), F(0), F(2), F(2), F(10), F(10), F(12), F(12), L(NULL));
        CHECK_HEX(word(bm, 11, 10), 0xFFFF0000u);
        CHECK_HEX(word(bm, 11, 11), 0xFFFFFFFFu);
        call("cvFree", 1, J(cv2));
    }
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(src));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

/* ------------------------------------------------------------------ the paint */

static megler_gfx_paint g_seen;
static void *g_seen_shader;
static int g_seen_calls;

/* A shell that only notes the paint it is handed. */
static void spy_rect(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader)
{
    (void)user;
    (void)canvas;
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
    g_seen = *paint;
    g_seen_shader = shader;
    g_seen_calls++;
}

static void test_paint(void)
{
    megler_gfx_host spy = *hk_gfx_standin();
    hk_jobj *p = paint(0x80123456u);
    i32 *a = ints_of(p);
    i64 bm = call("bmNew", 2, I(8), I(8)).j, cv = call("cvNew", 1, J(bm)).j, sh;
    say("  the paint the shell is handed, slot by slot");
    spy.draw_rect = spy_rect;
    hk_java_gfx_set_host(&spy);
    /* Every slot with a value of its own. */
    a[P_STYLE] = 2;
    a[P_STROKE] = (i32)hk_fb(3.5f);
    a[P_FLAGS] = 1 | 2 | 4 | 8 | 16;
    a[P_CAP] = 1;
    a[P_JOIN] = 2;
    a[P_TEXTSIZE] = (i32)hk_fb(17);
    a[P_ALIGN] = 2;
    a[P_MITER] = (i32)hk_fb(7);
    a[P_SHRAD] = (i32)hk_fb(2.5f);
    a[P_SHDX] = (i32)hk_fb(1.5f);
    a[P_SHDY] = (i32)hk_fb(-0.5f);
    a[P_SHCOL] = 0x40112233;
    a[P_SCALEX] = (i32)hk_fb(1.25f);
    a[P_SKEWX] = (i32)hk_fb(-0.25f);
    a[P_LSPACE] = (i32)hk_fb(0.125f);
    a[P_XFER] = 9;
    a[P_CFCOLOR] = (i32)0xFF445566u;
    a[P_ALPHA] = 3;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(p), J(0));
    CHECK_INT(g_seen_calls, 1);
    CHECK_HEX(g_seen.color, 0x80123456u);
    CHECK_INT(g_seen.style, MEGLER_GFX_FILL_AND_STROKE);
    CHECK(g_seen.stroke_width == 3.5f);
    /* The framework's flag bits are not Android's; the two for the bars under and through text are not the shell's business. */
    CHECK_INT(g_seen.flags, MEGLER_GFX_ANTI_ALIAS | MEGLER_GFX_FILTER_BITMAP | MEGLER_GFX_FAKE_BOLD);
    CHECK_INT(g_seen.cap, MEGLER_GFX_CAP_ROUND);
    CHECK_INT(g_seen.join, MEGLER_GFX_JOIN_BEVEL);
    CHECK(g_seen.miter_limit == 7);
    CHECK(g_seen.text_size == 17);
    CHECK_INT(g_seen.text_align, MEGLER_GFX_ALIGN_RIGHT);
    CHECK(g_seen.text_scale_x == 1.25f && g_seen.text_skew_x == -0.25f && g_seen.letter_spacing == 0.125f);
    CHECK_INT(g_seen.blend, MEGLER_GFX_BLEND_SRC_ATOP);
    CHECK_INT(g_seen.filter_mode, -1);
    CHECK(g_seen.shadow_radius == 2.5f && g_seen.shadow_dx == 1.5f && g_seen.shadow_dy == -0.5f);
    CHECK_HEX(g_seen.shadow_color, 0x40112233u);
    CHECK(g_seen_shader == NULL);
    /* A colour filter on a plain colour is worked out here: the shell gets the colour that results and no filter. */
    a[P_CFMODE] = 5;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(p), J(0));
    CHECK_HEX(g_seen.color, 0x80445566u);
    CHECK_INT(g_seen.filter_mode, -1);
    /* With a shader it is the shell's: the paint is as the app set it, and the shader's handle is the shell's own. */
    sh = call("shLinear", 7, F(0), F(0), F(8), F(0), L(NULL), L(NULL), I(0)).j;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(p), J(sh));
    CHECK_HEX(g_seen.color, 0x80123456u);
    CHECK_INT(g_seen.filter_mode, MEGLER_GFX_BLEND_SRC_IN);
    CHECK_HEX(g_seen.filter_color, 0xFF445566u);
    CHECK(g_seen_shader != NULL && g_seen_shader == (void *)(hk_addr)sh);
    call("shFree", 1, J(sh));
    /* What makes no sense is put right before the shell sees it. */
    a[P_STYLE] = 7;
    a[P_STROKE] = (i32)hk_fb(-2);
    a[P_CAP] = -1;
    a[P_JOIN] = 9;
    a[P_TEXTSIZE] = (i32)hk_fb(0);
    a[P_ALIGN] = 5;
    a[P_MITER] = (i32)hk_fb(-1);
    a[P_SHRAD] = (i32)hk_fb(-4);
    a[P_SCALEX] = (i32)hk_fb(0);
    a[P_SKEWX] = (i32)0x7FC00000;
    a[P_XFER] = 77;
    a[P_CFMODE] = -1;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(p), J(0));
    CHECK(g_seen.style == MEGLER_GFX_FILL && g_seen.cap == MEGLER_GFX_CAP_BUTT && g_seen.join == MEGLER_GFX_JOIN_MITER && g_seen.text_align == MEGLER_GFX_ALIGN_LEFT);
    CHECK(g_seen.stroke_width == 0 && g_seen.text_size == 12 && g_seen.miter_limit == 4 && g_seen.text_scale_x == 1 && g_seen.text_skew_x == 0);
    CHECK(g_seen.shadow_radius == 0);
    CHECK_INT(g_seen.blend, MEGLER_GFX_BLEND_SRC_OVER);
    a[P_STROKE] = (i32)0x7F800000;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(p), J(0));
    CHECK(g_seen.stroke_width == 0);
    /* DST is never handed on: nothing is drawn. */
    g_seen_calls = 0;
    a[P_XFER] = 2;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(p), J(0));
    CHECK_INT(g_seen_calls, 0);
    /* No Paint at all. */
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(NULL), J(0));
    CHECK_INT(g_seen_calls, 1);
    CHECK_HEX(g_seen.color, 0xFF000000u);
    CHECK(g_seen.style == 0 && g_seen.stroke_width == 0 && g_seen.flags == MEGLER_GFX_ANTI_ALIAS && g_seen.miter_limit == 4 && g_seen.text_size == 12);
    CHECK(g_seen.text_scale_x == 1 && g_seen.blend == MEGLER_GFX_BLEND_SRC_OVER && g_seen.filter_mode == -1 && g_seen.shadow_radius == 0);
    /* A rectangle is handed on with its sides in order, and not at all while the clip is empty. */
    g_seen_calls = 0;
    call("cvClipRect", 6, J(cv), F(9), F(9), F(10), F(10), I(1));
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(NULL), J(0));
    CHECK_INT(g_seen_calls, 0);
    hk_java_gfx_set_host(hk_gfx_standin());
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

/* ----------------------------------------------------------------------- text */

static void test_text(void)
{
    hk_jobj *s = hk_dvm_new_string("xA B"), *p = paint(0xFF0000FFu), *out5 = floats(NULL, 5), *box = ints(NULL, 4), *widths = floats(NULL, 4);
    hk_jobj *words = hk_dvm_new_string("hello world foo");
    i64 bm = call("bmNew", 2, I(64), I(40)).j, cv = call("cvNew", 1, J(bm)).j, face, bold, again;
    int i;
    say("  text and typefaces");
    /* Slot 6 is the text size. The string's part from start to end is the run; y is the baseline. The stand-in's boxes at size 20 are 10 by 14, 12 apart. */
    ints_of(p)[P_TEXTSIZE] = (i32)hk_fb(20);
    call("cvDrawText", 9, J(cv), L(s), I(1), I(4), F(10), F(30), L(p), J(0), J(0));
    CHECK_INT(covered(bm), 2 * 10 * 14);
    CHECK_HEX(word(bm, 10, 16), 0xFF0000FFu);
    CHECK_HEX(word(bm, 19, 29), 0xFF0000FFu);
    CHECK_HEX(word(bm, 25, 20), 0u);
    CHECK_HEX(word(bm, 34, 20), 0xFF0000FFu);
    /* A range that is none draws nothing; one that reaches past the string ends with it. */
    erase(bm);
    call("cvDrawText", 9, J(cv), L(s), I(2), I(2), F(10), F(30), L(p), J(0), J(0));
    call("cvDrawText", 9, J(cv), L(NULL), I(0), I(2), F(10), F(30), L(p), J(0), J(0));
    CHECK_INT(covered(bm), 0);
    call("cvDrawText", 9, J(cv), L(s), I(3), I(99), F(10), F(30), L(p), J(0), J(0));
    CHECK(exactly(bm, 10, 16, 20, 30));
    /* Slot 7 is the alignment: 1 puts the run's middle at x. */
    erase(bm);
    ints_of(p)[P_ALIGN] = 1;
    call("cvDrawText", 9, J(cv), L(s), I(3), I(4), F(30), F(30), L(p), J(0), J(0));
    CHECK(exactly(bm, 24, 16, 34, 30));
    ints_of(p)[P_ALIGN] = 0;
    /* Flags 8 and 16 of slot 3 are an underline and a strike-through: bars drawn here, a ninth of the size under the baseline and three tenths above it. */
    erase(bm);
    ints_of(p)[P_TEXTSIZE] = (i32)hk_fb(36);
    ints_of(p)[P_FLAGS] = 8;
    call("cvDrawText", 9, J(cv), L(s), I(1), I(2), F(10), F(30), L(p), J(0), J(0));
    CHECK_HEX(word(bm, 10, 33), 0u);
    CHECK_HEX(word(bm, 10, 34), 0xFF0000FFu);
    CHECK_HEX(word(bm, 30, 35), 0xFF0000FFu);
    CHECK_HEX(word(bm, 10, 36), 0u);
    CHECK_HEX(word(bm, 32, 34), 0u);
    erase(bm);
    ints_of(p)[P_FLAGS] = 16;
    ints_of(p)[P_COLOR] = (i32)0xFFFF0000u;
    call("cvDrawText", 9, J(cv), L(hk_dvm_new_string(" ")), I(0), I(1), F(10), F(30), L(p), J(0), J(0));
    CHECK(covered(bm) == 22 * 2);
    CHECK_HEX(word(bm, 10, 19), 0xFFFF0000u);
    CHECK_HEX(word(bm, 31, 20), 0xFFFF0000u);
    ints_of(p)[P_FLAGS] = 0;
    ints_of(p)[P_TEXTSIZE] = (i32)hk_fb(20);
    /* Measuring: the same part of the string, the same paint. */
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(1), I(4), L(p), J(0)).f, 36);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(0), I(4), L(p), J(0)).f, 48);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(2), I(2), L(p), J(0)).f, 0);
    CHECK_FLOAT(call("txMeasure", 5, L(NULL), I(0), I(4), L(p), J(0)).f, 0);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(0), I(4), L(NULL), J(0)).f, 4 * 0.6f * 12);
    /* Slots 13 and 15, the scale and the letter spacing, are in the paint the shell is given. */
    ints_of(p)[P_SCALEX] = (i32)hk_fb(2);
    ints_of(p)[P_LSPACE] = (i32)hk_fb(0.4f);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(0), I(2), L(p), J(0)).f, 80);
    ints_of(p)[P_SCALEX] = (i32)hk_fb(1);
    ints_of(p)[P_LSPACE] = 0;
    /* The metrics, where Paint.getFontMetrics reads them: top, ascent, descent, bottom, leading; and the spacing returned. */
    CHECK_FLOAT(call("txMetrics", 3, L(p), J(0), L(out5)).f, 20);
    CHECK_FLOAT(floats_of(out5)[0], -16);
    CHECK_FLOAT(floats_of(out5)[1], -16);
    CHECK_FLOAT(floats_of(out5)[2], 4);
    CHECK_FLOAT(floats_of(out5)[3], 4);
    CHECK_FLOAT(floats_of(out5)[4], 0);
    CHECK_FLOAT(call("txMetrics", 3, L(p), J(0), L(floats(NULL, 4))).f, 20);
    CHECK_FLOAT(call("txMetrics", 3, L(p), J(0), L(NULL)).f, 20);
    /* The ink's box in whole numbers. */
    call("txBounds", 6, L(s), I(1), I(4), L(p), J(0), L(box));
    CHECK(ints_of(box)[0] == 0 && ints_of(box)[1] == -14 && ints_of(box)[2] == 34 && ints_of(box)[3] == 0);
    call("txBounds", 6, L(s), I(2), I(3), L(p), J(0), L(box));
    CHECK(ints_of(box)[0] == 0 && ints_of(box)[1] == 0 && ints_of(box)[2] == 0 && ints_of(box)[3] == 0);
    call("txBounds", 6, L(s), I(1), I(4), L(p), J(0), L(ints(NULL, 3)));
    /* Breaking: how many chars from `start` fit, by characters or by words. */
    ints_of(p)[P_TEXTSIZE] = (i32)hk_fb(10);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(50), L(p), J(0), Z(false)).i, 8);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(50), L(p), J(0), Z(true)).i, 6);
    CHECK_INT(call("txBreak", 7, L(words), I(6), I(15), F(50), L(p), J(0), Z(true)).i, 6);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(1000), L(p), J(0), Z(true)).i, 15);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(0), L(p), J(0), Z(true)).i, 0);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(-3), L(p), J(0), Z(false)).i, 0);
    /* Each char's advance; an array too short for them is left alone. */
    ints_of(p)[P_TEXTSIZE] = (i32)hk_fb(20);
    call("txWidths", 6, L(s), I(0), I(4), L(p), J(0), L(widths));
    for (i = 0; i < 4; i++) CHECK_FLOAT(floats_of(widths)[i], 12);
    floats_of(widths)[0] = 99;
    call("txWidths", 6, L(words), I(0), I(15), L(p), J(0), L(widths));
    CHECK_FLOAT(floats_of(widths)[0], 99);
    /* Typefaces. The default is always there; the name's words are weights; a derived face is made once. */
    face = call("tfDefault", 0).j;
    CHECK(face != 0);
    CHECK(call("tfDefault", 0).j == face);
    CHECK_INT(call("tfWeight", 1, J(face)).i, 400);
    CHECK_INT(call("tfWeight", 1, J(0)).i, 400);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("sans-serif")), I(0)).j)).i, 400);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("sans-serif")), I(1)).j)).i, 700);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("sans-serif-medium")), I(0)).j)).i, 500);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("Sans-Serif-MEDIUM")), I(3)).j)).i, 800);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("sans-serif-light")), I(0)).j)).i, 300);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("sans-serif-thin")), I(0)).j)).i, 200);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("sans-serif-black")), I(0)).j)).i, 900);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(hk_dvm_new_string("monospace")), I(0)).j)).i, 400);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(NULL), I(2)).j)).i, 400);
    bold = call("tfDerive", 3, J(face), I(1), I(0)).j;
    CHECK(bold != 0 && bold != face);
    CHECK_INT(call("tfWeight", 1, J(bold)).i, 700);
    again = call("tfDerive", 3, J(face), I(1), I(0)).j;
    CHECK(again == bold);
    CHECK(call("tfDerive", 3, J(0), I(1), I(700)).j == bold);
    CHECK(call("tfDerive", 3, J(face), I(3), I(0)).j != bold);
    CHECK_INT(call("tfWeight", 1, J(call("tfDerive", 3, J(bold), I(2), I(300)).j)).i, 300);
    CHECK_INT(call("tfWeight", 1, J(call("tfDerive", 3, J(bold), I(0), I(5000)).j)).i, 999);
    CHECK(call("tfFromData", 1, L(bytes("this is not a font", 18))).j == 0);
    CHECK(call("tfFromData", 1, L(NULL)).j == 0);
    again = call("tfFromData", 1, L(bytes("OTTO\0\1\0\0\0\0\0\0\0\0\0\0", 16))).j;
    CHECK(again != 0);
    CHECK_INT(call("tfWeight", 1, J(again)).i, 400);
    /* A typeface's handle in a text call is this file's object, and the shell is given its own. */
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(0), I(4), L(p), J(bold)).f, 48);
    call("cvDrawText", 9, J(cv), L(s), I(0), I(1), F(0), F(30), L(p), J(again), J(0));
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

/* -------------------------------------------------------------------- shaders */

static u32 g_colors[8];
static float g_positions[8];
static int g_stops, g_tile;

/* A shell that only notes the stops it is handed. */
static void *spy_linear(void *user, float x0, float y0, float x1, float y1, const uint32_t *colors, const float *positions, int count, int tile)
{
    int i;
    (void)user;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    g_stops = count;
    g_tile = tile;
    for (i = 0; i < count && i < 8; i++) {
        g_colors[i] = colors[i];
        g_positions[i] = positions[i];
    }
    return g_colors;
}

static void test_shaders(void)
{
    static const i32 three[] = { (i32)0xFF0000FFu, (i32)0xFFFF0000u, (i32)0xFF00FF00u };
    static const float untidy[] = { 0.5f, 0.25f, 7 };
    megler_gfx_host spy;
    hk_jobj *p = paint(0x80FFFFFFu);
    i64 bm = call("bmNew", 2, I(8), I(8)).j, cv = call("cvNew", 1, J(bm)).j, sh;
    say("  shaders");
    /* A gradient is the shell's object, and its handle is what a drawing call passes back. The stand-in's is its first colour, here at the paint's alpha. */
    sh = call("shLinear", 7, F(0), F(0), F(8), F(0), L(ints(three, 3)), L(NULL), I(0)).j;
    CHECK(sh != 0);
    call("shMatrix", 2, J(sh), L(floats(k_identity, 6)));
    call("shMatrix", 2, J(sh), L(NULL));
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(2), F(2), L(p), J(sh));
    CHECK_HEX(word(bm, 1, 1), 0x80000080u);
    /* A colour filter with a shader is the shell's to do, on the shader's colour: SRC_IN red leaves the shape in red. */
    ints_of(p)[P_CFCOLOR] = (i32)0xFFFF0000u;
    ints_of(p)[P_CFMODE] = 5;
    call("cvDrawRect", 7, J(cv), F(4), F(0), F(6), F(2), L(p), J(sh));
    CHECK_HEX(word(bm, 5, 1), 0x80800000u);
    call("shFree", 1, J(sh));
    call("shFree", 1, J(0));
    sh = call("shRadial", 6, F(4), F(4), F(4), L(ints(three + 1, 2)), L(NULL), I(1)).j;
    CHECK(sh != 0);
    call("shFree", 1, J(sh));
    sh = call("shSweep", 4, F(4), F(4), L(ints(three + 2, 1)), L(NULL)).j;
    CHECK(sh != 0);
    call("shFree", 1, J(sh));
    /* A bitmap as a shader is the bitmap as it is at that moment. */
    call("bmErase", 2, J(bm), I((i32)0xFF00FF00u));
    sh = call("shBitmap", 2, J(bm), I(1)).j;
    CHECK(sh != 0);
    erase(bm);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(2), F(2), L(paint(0xFF000000u)), J(sh));
    CHECK_HEX(word(bm, 1, 1), 0xFF00FF00u);
    call("shFree", 1, J(sh));
    CHECK(call("shBitmap", 2, J(0), I(0)).j == 0);
    /* What the shell is promised about the stops: two at least, positions in 0..1 that never go down, spread evenly when the app gives none. */
    spy = *hk_gfx_standin();
    spy.shader_linear = spy_linear;
    hk_java_gfx_set_host(&spy);
    call("shLinear", 7, F(0), F(0), F(8), F(0), L(ints(three, 3)), L(NULL), I(2));
    CHECK_INT(g_stops, 3);
    CHECK_INT(g_tile, 2);
    CHECK(g_colors[0] == 0xFF0000FFu && g_colors[2] == 0xFF00FF00u);
    CHECK(g_positions[0] == 0 && g_positions[1] == 0.5f && g_positions[2] == 1);
    call("shLinear", 7, F(0), F(0), F(8), F(0), L(ints(three, 3)), L(floats(untidy, 3)), I(7));
    CHECK_INT(g_tile, 0);
    CHECK(g_positions[0] == 0.5f && g_positions[1] == 0.5f && g_positions[2] == 1);
    /* Fewer positions than colours are none, as in the original. One colour is that colour at both ends; no colours, black. */
    call("shLinear", 7, F(0), F(0), F(8), F(0), L(ints(three, 3)), L(floats(untidy, 2)), I(1));
    CHECK(g_positions[0] == 0 && g_positions[1] == 0.5f && g_positions[2] == 1);
    call("shLinear", 7, F(0), F(0), F(8), F(0), L(ints(three + 1, 1)), L(NULL), I(0));
    CHECK_INT(g_stops, 2);
    CHECK(g_colors[0] == 0xFFFF0000u && g_colors[1] == 0xFFFF0000u && g_positions[0] == 0 && g_positions[1] == 1);
    call("shLinear", 7, F(0), F(0), F(8), F(0), L(NULL), L(NULL), I(0));
    CHECK_INT(g_stops, 2);
    CHECK(g_colors[0] == 0xFF000000u && g_colors[1] == 0xFF000000u);
    hk_java_gfx_set_host(hk_gfx_standin());
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

/* ---------------------------------------------------------------------- paths */

static void test_paths(void)
{
    hk_jobj *out = floats(NULL, 4), *first = floats(NULL, 2);
    say("  questions about a path, answered here");
    /* The box round the path itself: a dome whose control point is at -10 only reaches 0. */
    {
        static const float dome[] = { 0, 0, 10, 2, 10, -10, 20, 10 }, wave[] = { 0, 0, 0, 3, 0, 30, 30, -30, 30, 0 }, rect[] = { 6, 4, 3, 1, 9, 0 };
        call("pathBounds", 3, L(floats(dome, 8)), I(8), L(out));
        CHECK_FLOAT(floats_of(out)[0], 0);
        CHECK_FLOAT(floats_of(out)[1], 0);
        CHECK_FLOAT(floats_of(out)[2], 20);
        CHECK_FLOAT(floats_of(out)[3], 10);
        /* A cubic that swings both ways: 30 * 3 * t * (1 - t) * (1 - 2t) is at most 30 * 3 * 0.0962 either way. */
        call("pathBounds", 3, L(floats(wave, 10)), I(10), L(out));
        CHECK_FLOAT(floats_of(out)[0], 0);
        CHECK_FLOAT(floats_of(out)[2], 30);
        CHECK(fabs(floats_of(out)[1] + 8.6603) < 0.01 && fabs(floats_of(out)[3] - 8.6603) < 0.01);
        /* A rectangle given the wrong way round, and nothing at all. */
        call("pathBounds", 3, L(floats(rect, 6)), I(6), L(out));
        CHECK(floats_of(out)[0] == 1 && floats_of(out)[1] == 3 && floats_of(out)[2] == 4 && floats_of(out)[3] == 9);
        call("pathBounds", 3, L(floats(rect, 6)), I(0), L(out));
        CHECK(floats_of(out)[0] == 0 && floats_of(out)[1] == 0 && floats_of(out)[2] == 0 && floats_of(out)[3] == 0);
        /* The length: three sides and the closing one; a curve by its chords. */
        CHECK_FLOAT(call("pathLength", 3, L(floats(rect, 6)), I(6), L(first)).f, 18);
        CHECK(floats_of(first)[0] == 1 && floats_of(first)[1] == 3);
    }
    {
        static const float line[] = { 0, 1, 1, 11, 3, 4 }, circle[] = { 7, 0, 0, 20, 20, 0 }, moved[] = { 0, 1, 1, 1, 4, 5, 9, 2, 0, 0, 0, 2, 0 };
        static const float two[] = { 6, 0, 0, 10, 10, 0, 6, 3, 3, 7, 7, 0 }, turned[] = { 6, 0, 0, 10, 10, 0, 6, 3, 3, 7, 7, 1 };
        CHECK_FLOAT(call("pathLength", 3, L(floats(line, 6)), I(6), L(NULL)).f, 5);
        CHECK(fabs(call("pathLength", 3, L(floats(circle, 6)), I(6), L(first)).f - 62.83) < 0.3);
        /* Path.transform is an operation that goes through everything before it: the line of 5 is 10 long after it. */
        CHECK_FLOAT(call("pathLength", 3, L(floats(moved, 13)), I(13), L(NULL)).f, 10);
        call("pathBounds", 3, L(floats(moved, 13)), I(13), L(out));
        CHECK(floats_of(out)[0] == 2 && floats_of(out)[1] == 2 && floats_of(out)[2] == 8 && floats_of(out)[3] == 10);
        /* Inside or not, by the fill rule: two squares the same way round, and the inner one turned the other way. */
        CHECK(call("pathContains", 5, L(floats(two, 12)), I(12), I(0), F(5), F(5)).z);
        CHECK(!call("pathContains", 5, L(floats(two, 12)), I(12), I(1), F(5), F(5)).z);
        CHECK(call("pathContains", 5, L(floats(two, 12)), I(12), I(1), F(1), F(5)).z);
        CHECK(!call("pathContains", 5, L(floats(two, 12)), I(12), I(0), F(11), F(5)).z);
        CHECK(!call("pathContains", 5, L(floats(turned, 12)), I(12), I(0), F(5), F(5)).z);
        CHECK(call("pathContains", 5, L(floats(turned, 12)), I(12), I(0), F(1), F(5)).z);
        CHECK(call("pathContains", 5, L(floats(circle, 6)), I(6), I(0), F(10), F(10)).z);
        CHECK(call("pathContains", 5, L(floats(circle, 6)), I(6), I(0), F(3.5f), F(3.5f)).z);
        CHECK(!call("pathContains", 5, L(floats(circle, 6)), I(6), I(0), F(2), F(2)).z);
        CHECK(!call("pathContains", 5, L(NULL), I(6), I(0), F(2), F(2)).z);
    }
    CHECK(thrown()[0] == 0);
}

/* ----------------------------------------------- flush, touched, present, no shell */

static int g_flushes, g_touches, g_gone;
static void *g_flushed, *g_touched;

static void count_flush(void *user, void *canvas)
{
    (void)user;
    g_flushes++;
    g_flushed = canvas;
}

static void count_touched(void *user, void *canvas)
{
    (void)user;
    g_touches++;
    g_touched = canvas;
}

static void count_gone(void *user, uint32_t id)
{
    (void)user;
    g_gone += id != 0;
}

static const void *g_presented;
static int g_presented_w, g_presented_h, g_presented_stride, g_presents;

static void on_present(void *user, const void *pixels, int width, int height, int stride)
{
    *(int *)user += 1;
    g_presented = pixels;
    g_presented_w = width;
    g_presented_h = height;
    g_presented_stride = stride;
}

static void test_protocol(void)
{
    megler_gfx_host counting = *hk_gfx_standin();
    hk_java_gfx_bitmap info;
    hk_jobj *red = paint(0xFFFF0000u);
    i64 bm, cv, cv2, other, cv3;
    say("  when the shell is told to flush and that the memory changed; present; no shell at all");
    counting.flush = count_flush;
    counting.touched = count_touched;
    counting.bitmap_gone = count_gone;
    hk_java_gfx_set_host(&counting);
    bm = call("bmNew", 2, I(8), I(8)).j;
    cv = call("cvNew", 1, J(bm)).j;
    /* Nothing drawn: reading needs no flush. */
    call("bmGetPixel", 3, J(bm), I(0), I(0));
    CHECK_INT(g_flushes, 0);
    /* Drawn, then read: one flush, however much is read. */
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(4), F(4), L(red), J(0));
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(2), F(2), L(red), J(0));
    CHECK_INT(g_flushes, 0);
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(1), I(1)).i, 0xFFFF0000u);
    call("bmGetPixel", 3, J(bm), I(2), I(2));
    hk_java_gfx_bitmap_info(bm, &info);
    CHECK_INT(g_flushes, 1);
    CHECK_INT(g_touches, 0);
    /* Written behind the canvas, many times, then drawn: one touched, before the drawing. */
    call("bmSetPixel", 4, J(bm), I(7), I(7), I(-1));
    call("bmSetPixel", 4, J(bm), I(6), I(7), I(-1));
    {
        static const i32 four[4] = { 1, 2, 3, 4 };
        call("bmPixels", 9, J(bm), L(ints(four, 4)), I(0), I(2), I(0), I(6), I(2), I(2), Z(true));
    }
    CHECK_INT(g_touches, 0);
    call("cvSave", 1, J(cv));
    call("cvClipRect", 6, J(cv), F(0), F(0), F(8), F(8), I(1));
    call("cvRestore", 1, J(cv));
    CHECK_INT(g_touches, 0);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(4), F(4), L(red), J(0));
    CHECK_INT(g_touches, 1);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(4), F(4), L(red), J(0));
    CHECK_INT(g_touches, 1);
    CHECK_INT(g_flushes, 1);
    /* A write flushes first, so that what was drawn before it is under it and not over it. */
    call("bmSetPixel", 4, J(bm), I(0), I(0), I(-1));
    CHECK_INT(g_flushes, 2);
    /*
     * Erasing the whole bitmap, which the framework does before every frame,
     * is drawing when a canvas can do it: the shell is not told to take the
     * memory back (touched) only to have every pixel replaced, and the
     * memory has the colour when it is next read.
     */
    call("bmErase", 2, J(bm), I((i32)0x80FF0000u));
    CHECK_INT(g_touches, 1);
    CHECK_INT(g_flushes, 2);
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(5), I(5)).i, 0x80FF0000u);
    CHECK_INT(g_flushes, 3);
    CHECK_INT(covered(bm), 64);
    /* The same for a drawColor that replaces everything; one that lays a colour over what is there needs what is there. */
    call("bmSetPixel", 4, J(bm), I(0), I(0), I(-1));
    call("cvDrawColor", 3, J(cv), I(0), I(0));
    CHECK_INT(g_touches, 1);
    CHECK_INT(covered(bm), 0);
    call("bmSetPixel", 4, J(bm), I(0), I(0), I(-1));
    call("cvDrawColor", 3, J(cv), I((i32)0x80FF0000u), I(3));
    CHECK_INT(g_touches, 2);
    CHECK_HEX(word(bm, 0, 0), 0xFFFF7F7Fu);
    CHECK_HEX(word(bm, 1, 0), 0x80800000u);
    /* With something saved or clipped a canvas cannot reach every pixel, and the erasing is done in the memory. */
    call("cvSave", 1, J(cv));
    call("cvClipRect", 6, J(cv), F(0), F(0), F(2), F(2), I(1));
    g_flushes = g_touches = 0;
    call("bmErase", 2, J(bm), I((i32)0xFF00FF00u));
    CHECK_INT(g_touches, 0);
    call("cvDrawColor", 3, J(cv), I(0), I(1));
    CHECK_INT(g_touches, 1);
    call("cvRestore", 1, J(cv));
    CHECK_HEX(word(bm, 1, 1), 0u);
    CHECK_HEX(word(bm, 2, 2), 0xFF00FF00u);
    CHECK_INT(covered(bm), 60);
    call("bmSetPixel", 4, J(bm), I(0), I(0), I(-1));
    /* Two canvases on one bitmap: before the second draws, the first's drawing is put into the memory and the second is told; and the other way round. */
    cv2 = call("cvNew", 1, J(bm)).j;
    g_flushes = g_touches = 0;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(4), F(4), L(red), J(0));
    CHECK_INT(g_touches, 1);
    CHECK_INT(g_flushes, 0);
    call("cvDrawRect", 7, J(cv2), F(0), F(0), F(4), F(4), L(red), J(0));
    CHECK_INT(g_flushes, 1);
    CHECK_INT(g_touches, 2);
    CHECK(g_flushed != g_touched);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(4), F(4), L(red), J(0));
    CHECK_INT(g_flushes, 2);
    CHECK_INT(g_touches, 3);
    CHECK(g_flushed != g_touched);
    /* A bitmap drawn from is flushed first; and so is one handed to the app's native code or written through its address. */
    other = call("bmNew", 2, I(8), I(8)).j;
    cv3 = call("cvNew", 1, J(other)).j;
    g_flushes = g_touches = 0;
    call("cvDrawBitmap", 11, J(cv3), J(bm), F(0), F(0), F(8), F(8), F(0), F(0), F(8), F(8), L(NULL));
    CHECK_INT(g_flushes, 1);
    CHECK_HEX(word(other, 1, 1), 0xFFFF0000u);
    CHECK_INT(g_flushes, 2);
    CHECK(hk_java_gfx_lock_rgba(other, &info));
    hk_java_gfx_unlock_rgba(other);
    CHECK_INT(g_touches, 0);
    call("cvDrawRect", 7, J(cv3), F(0), F(0), F(1), F(1), L(red), J(0));
    CHECK_INT(g_touches, 1);
    hk_java_gfx_bitmap_changed(other);
    call("cvDrawRect", 7, J(cv3), F(0), F(0), F(1), F(1), L(red), J(0));
    CHECK_INT(g_touches, 2);
    /* A layer put back at its restore is drawing too. */
    g_flushes = 0;
    call("bmGetPixel", 3, J(other), I(0), I(0));
    CHECK_INT(g_flushes, 1);
    call("cvSaveLayer", 6, J(cv3), F(0), F(0), F(0), F(0), I(255));
    call("cvRestore", 1, J(cv3));
    call("bmGetPixel", 3, J(other), I(0), I(0));
    CHECK_INT(g_flushes, 2);
    call("cvSave", 1, J(cv3));
    call("cvRestore", 1, J(cv3));
    call("bmGetPixel", 3, J(other), I(0), I(0));
    CHECK_INT(g_flushes, 2);
    /* present: the bitmap's memory with everything drawn in it; 0 is "nothing to show". */
    hk_java_gfx_set_present(on_present, &g_presents);
    call("cvDrawRect", 7, J(cv3), F(0), F(0), F(1), F(1), L(red), J(0));
    g_flushes = 0;
    call("present", 1, J(other));
    CHECK_INT(g_presents, 1);
    CHECK_INT(g_flushes, 1);
    hk_java_gfx_bitmap_info(other, &info);
    CHECK(g_presented == info.pixels && g_presented_w == 8 && g_presented_h == 8 && g_presented_stride == 32);
    call("present", 1, J(0));
    CHECK_INT(g_presents, 2);
    CHECK(g_presented == NULL && g_presented_w == 0);
    hk_java_gfx_set_present(NULL, NULL);
    call("present", 1, J(other));
    CHECK_INT(g_presents, 2);
    /* The shell hears of every bitmap that goes. */
    call("cvFree", 1, J(cv3));
    call("cvFree", 1, J(cv2));
    call("cvFree", 1, J(cv));
    g_gone = 0;
    call("bmFree", 1, J(other));
    call("bmFree", 1, J(bm));
    CHECK_INT(g_gone, 2);
    /* No shell: bitmaps and the canvas's bookkeeping still work, nothing is drawn, every answer is zero, nothing breaks. */
    hk_java_gfx_set_host(NULL);
    bm = call("bmNew", 2, I(8), I(8)).j;
    CHECK(bm != 0);
    cv = call("cvNew", 1, J(bm)).j;
    CHECK(cv != 0);
    CHECK_INT(call("cvSave", 1, J(cv)).i, 1);
    CHECK_INT(call("cvSaveLayer", 6, J(cv), F(0), F(0), F(4), F(4), I(100)).i, 2);
    CHECK(call("cvClipRect", 6, J(cv), F(0), F(0), F(4), F(4), I(1)).z);
    CHECK(!call("cvClipRect", 6, J(cv), F(5), F(5), F(6), F(6), I(1)).z);
    call("cvRestoreTo", 2, J(cv), I(1));
    CHECK_INT(call("cvSaveCount", 1, J(cv)).i, 1);
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(4), F(4), L(red), J(0));
    call("cvDrawColor", 3, J(cv), I(-1), I(3));
    call("cvDrawText", 9, J(cv), L(hk_dvm_new_string("abc")), I(0), I(3), F(0), F(6), L(red), J(0), J(0));
    CHECK_INT(covered(bm), 0);
    call("bmSetPixel", 4, J(bm), I(1), I(1), I(-1));
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(1), I(1)).i, 0xFFFFFFFFu);
    CHECK_FLOAT(call("txMeasure", 5, L(hk_dvm_new_string("abc")), I(0), I(3), L(red), J(0)).f, 0);
    CHECK_FLOAT(call("txMetrics", 3, L(red), J(0), L(floats(NULL, 5))).f, 0);
    CHECK_INT(call("txBreak", 7, L(hk_dvm_new_string("abc")), I(0), I(3), F(100), L(red), J(0), Z(true)).i, 0);
    CHECK(call("shLinear", 7, F(0), F(0), F(8), F(0), L(NULL), L(NULL), I(0)).j == 0);
    CHECK(call("bmCompress", 3, J(bm), I(1), I(90)).l == NULL);
    CHECK(call("bmDecode", 6, L(bytes("\x89PNG\r\n\x1a\n....", 12)), I(0), I(12), I(1), L(NULL), Z(false)).j == 0);
    CHECK(call("tfFromData", 1, L(bytes("OTTO\0\1\0\0\0\0\0\0\0\0\0\0", 16))).j == 0);
    CHECK(call("tfCreate", 2, L(hk_dvm_new_string("serif")), I(0)).j != 0);
    /* What needs no drawing is still done: a plain copy, a scaled one. */
    {
        static const float twice[6] = { 2, 0, 0, 0, 2, 0 }, turn[6] = { 0, -1, 0, 1, 0, 0 };
        i64 copy = call("bmCopy", 7, J(bm), I(0), I(0), I(4), I(4), L(floats(twice, 6)), Z(false)).j;
        CHECK(copy != 0);
        CHECK_HEX(word(copy, 2, 2), 0xFFFFFFFFu);
        CHECK_HEX(word(copy, 3, 3), 0xFFFFFFFFu);
        CHECK_INT(covered(copy), 4);
        call("bmFree", 1, J(copy));
        copy = call("bmCopy", 7, J(bm), I(0), I(0), I(4), I(2), L(floats(turn, 6)), Z(true)).j;
        CHECK(hk_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 4);
        CHECK_INT(covered(copy), 0);
        call("bmFree", 1, J(copy));
    }
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

int hk_gfx_natives_tests_run(hk_test_sink sink, void *user, int *checks)
{
    uint64_t bytes0 = 0, bytes1 = 1;
    uint32_t count0 = 0, count1 = 1;
    int n = 0;
    const hk_jhle *e;
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    if (!hk_dvm_running()) {
        say("megler.Gfx's native methods: not tested, the Dalvik runtime is not running");
        return 0;
    }
    say("megler.Gfx's native methods, over the stand-in");
    /* One entry for each native method megler/Gfx.java declares. */
    for (e = hk_java_gfx_natives(); e->cls; e++) n += strcmp(e->cls, "megler/Gfx") == 0 && e->sig && e->fn;
    CHECK_INT(n, 54);
    hk_java_gfx_set_host(hk_gfx_standin());
    hk_java_gfx_stats(&bytes0, &count0);
    test_bitmaps();
    test_webp();
    test_canvas();
    test_drawing();
    test_paint();
    test_text();
    test_shaders();
    test_paths();
    test_protocol();
    /* Every bitmap the tests made is freed again. */
    hk_java_gfx_stats(&bytes1, &count1);
    CHECK_INT(bytes1, bytes0);
    CHECK_INT(count1, count0);
    hk_java_gfx_set_host(NULL);
    hk_java_gfx_set_present(NULL, NULL);
    if (checks) *checks += t_checks;
    return t_failed;
}
