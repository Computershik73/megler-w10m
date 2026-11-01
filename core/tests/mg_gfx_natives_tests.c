/*
 * megler.Gfx's native methods (core/src/drivers/mg_java_gfx.c), called as the
 * runtime calls them: an mg_jcall with one value for each parameter of the
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
#include <stdlib.h>

#include "../src/drivers/mg_java_gfx.h"
#include "../src/dvm/mg_dvm.h"
#include "../src/host/mg_host.h"
#include "../src/webp/mg_webp.h"
#include "mg_gfx_standin.h"
#include "mg_gfx_tests.h"
#include "mg_webp_fixtures.inc"

static int t_checks, t_failed;
static mg_test_sink t_sink;
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

static mg_jvalue J(i64 v)
{
    mg_jvalue x;
    x.j = v;
    return x;
}

static mg_jvalue I(i32 v)
{
    mg_jvalue x;
    x.j = 0;
    x.i = v;
    return x;
}

static mg_jvalue F(float v)
{
    mg_jvalue x;
    x.j = 0;
    x.f = v;
    return x;
}

static mg_jvalue Z(bool v)
{
    mg_jvalue x;
    x.j = 0;
    x.z = v;
    return x;
}

static mg_jvalue L(mg_jobj *v)
{
    mg_jvalue x;
    x.j = 0;
    x.l = v;
    return x;
}

/* A native of megler.Gfx by its name, with `count` values for its parameters, in the order of the Java declaration. */
static mg_jvalue call(const char *name, int count, ...)
{
    const mg_jhle *e;
    mg_jvalue args[16];
    mg_jcall c;
    va_list ap;
    int i;
    memset(&c, 0, sizeof(c));
    memset(args, 0, sizeof(args));
    va_start(ap, count);
    for (i = 0; i < count && i < 16; i++) args[i] = va_arg(ap, mg_jvalue);
    va_end(ap);
    for (e = mg_java_gfx_natives(); e->cls; e++)
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

static mg_jobj *ints(const i32 *v, int n)
{
    mg_jobj *a = mg_dvm_new_array("[I", n);
    if (a && v && n) memcpy(a->u.arr.data, v, sizeof(i32) * (size_t)n);
    return a;
}

static mg_jobj *floats(const float *v, int n)
{
    mg_jobj *a = mg_dvm_new_array("[F", n);
    if (a && v && n) memcpy(a->u.arr.data, v, sizeof(float) * (size_t)n);
    return a;
}

static mg_jobj *bytes(const void *v, int n)
{
    mg_jobj *a = mg_dvm_new_array("[B", n);
    if (a && v && n) memcpy(a->u.arr.data, v, (size_t)n);
    return a;
}

static i32 *ints_of(mg_jobj *a) { return (i32 *)a->u.arr.data; }
static float *floats_of(mg_jobj *a) { return (float *)a->u.arr.data; }

/* The exception a native left, as text, cleared; "" when there is none. */
static const char *thrown(void)
{
    static char text[300];
    text[0] = 0;
    if (mg_dvm_pending()) {
        mg_dvm_describe_pending(text, sizeof(text));
        mg_dvm_clear_pending();
    }
    return text;
}

/* android.graphics.Paint.mP as `new Paint()` leaves it: the slots are in mg_java_gfx.c's first comment. */
enum {
    P_COLOR, P_STYLE, P_STROKE, P_FLAGS, P_CAP, P_JOIN, P_TEXTSIZE, P_ALIGN, P_MITER, P_SHRAD, P_SHDX, P_SHDY, P_SHCOL,
    P_SCALEX, P_SKEWX, P_LSPACE, P_XFER, P_CFCOLOR, P_CFMODE, P_ALPHA, P_COUNT
};

static mg_jobj *paint(u32 color)
{
    mg_jobj *a = mg_dvm_new_array("[I", P_COUNT);
    i32 *p = ints_of(a);
    p[P_COLOR] = (i32)color;
    p[P_TEXTSIZE] = (i32)mg_fb(12);
    p[P_MITER] = (i32)mg_fb(4);
    p[P_SCALEX] = (i32)mg_fb(1);
    p[P_XFER] = 3;
    p[P_CFMODE] = -1;
    p[P_ALPHA] = 255;
    return a;
}

/* ---------------------------------------------------------- reading a bitmap */

static u32 word(i64 bm, int x, int y)
{
    mg_java_gfx_bitmap b;
    if (!mg_java_gfx_bitmap_info(bm, &b) || x < 0 || y < 0 || x >= b.width || y >= b.height) return 0xDEADBEEFu;
    return mg_rd32((const u8 *)b.pixels + (size_t)y * (size_t)b.stride + 4 * (size_t)x);
}

static int covered(i64 bm)
{
    mg_java_gfx_bitmap b;
    int x, y, n = 0;
    if (!mg_java_gfx_bitmap_info(bm, &b)) return -1;
    for (y = 0; y < b.height; y++)
        for (x = 0; x < b.width; x++) n += word(bm, x, y) != 0;
    return n;
}

/* The pixels that are not transparent are exactly x0..x1 by y0..y1. */
static bool exactly(i64 bm, int x0, int y0, int x1, int y1)
{
    mg_java_gfx_bitmap b;
    int x, y;
    if (!mg_java_gfx_bitmap_info(bm, &b)) return false;
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
    mg_java_gfx_bitmap info;
    mg_jobj *a;
    i64 bm, copy;
    u8 raw[48];
    int i;
    say("  bitmaps: pixels as Java reads and writes them, in memory, and in Android's byte order");
    mg_java_gfx_stats(&bytes0, &count0);
    bm = call("bmNew", 2, I(4), I(3)).j;
    CHECK(bm != 0);
    mg_java_gfx_stats(&bytes1, &count1);
    CHECK_INT(bytes1 - bytes0, 48);
    CHECK_INT(count1 - count0, 1);
    CHECK(call("bmNew", 2, I(0), I(5)).j == 0);
    CHECK(call("bmNew", 2, I(5), I(-1)).j == 0);
    CHECK(call("bmNew", 2, I(100000), I(100000)).j == 0);
    CHECK(mg_java_gfx_bitmap_info(bm, &info));
    CHECK_INT(info.width, 4);
    CHECK_INT(info.height, 3);
    CHECK_INT(info.stride, 16);
    CHECK(call("bmAddress", 1, J(bm)).j == (i64)(mg_addr)info.pixels);
    CHECK(!mg_java_gfx_bitmap_info(0, &info));
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
    call("bmRaw", 3, J(bm), J((i64)(mg_addr)raw), Z(false));
    CHECK(raw[16 + 8 + 0] == 0xFF && raw[16 + 8 + 1] == 0 && raw[16 + 8 + 2] == 0 && raw[16 + 8 + 3] == 0xFF);
    CHECK(raw[32 + 8 + 0] == 0 && raw[32 + 8 + 1] == 0 && raw[32 + 8 + 2] == 0xFF && raw[32 + 8 + 3] == 0xFF);
    CHECK(raw[0] == 0x40 && raw[3] == 0x40);
    for (i = 0; i < 12; i++) {
        raw[4 * i] = 1;
        raw[4 * i + 1] = 2;
        raw[4 * i + 2] = 3;
        raw[4 * i + 3] = 255;
    }
    call("bmRaw", 3, J(bm), J((i64)(mg_addr)raw), Z(true));
    CHECK_HEX(word(bm, 3, 2), 0xFF010203u);
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(0), I(0)).i, 0xFF010203u);
    call("bmRaw", 3, J(bm), J(0), Z(true));
    /* For the app's native code (libjnigraphics) the memory itself is turned to Android's order, and back. */
    call("bmSetPixel", 4, J(bm), I(0), I(0), I((i32)0xFF0A0B0Cu));
    CHECK(mg_java_gfx_lock_rgba(bm, &info));
    CHECK(((const u8 *)info.pixels)[0] == 0x0A && ((const u8 *)info.pixels)[1] == 0x0B && ((const u8 *)info.pixels)[2] == 0x0C);
    CHECK(mg_java_gfx_lock_rgba(bm, &info));
    CHECK(((const u8 *)info.pixels)[0] == 0x0A);
    ((u8 *)info.pixels)[1] = 0x77;
    mg_java_gfx_unlock_rgba(bm);
    CHECK(((const u8 *)info.pixels)[0] == 0x0A);
    mg_java_gfx_unlock_rgba(bm);
    CHECK_HEX(word(bm, 0, 0), 0xFF0A770Cu);
    CHECK(!mg_java_gfx_lock_rgba(0, &info));
    mg_java_gfx_unlock_rgba(0);
    /* createBitmap(source, x, y, w, h): a part, copied. */
    call("bmSetPixel", 4, J(bm), I(2), I(1), I((i32)0xFFFF0000u));
    call("bmSetPixel", 4, J(bm), I(3), I(1), I((i32)0xFF0000FFu));
    call("bmSetPixel", 4, J(bm), I(2), I(2), I((i32)0xFF00FF00u));
    call("bmSetPixel", 4, J(bm), I(3), I(2), I((i32)0x80FFFFFFu));
    copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(2), L(NULL), Z(false)).j;
    CHECK(copy != 0 && mg_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 2);
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
        CHECK(mg_java_gfx_bitmap_info(copy, &info) && info.width == 4 && info.height == 4);
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
        CHECK(mg_java_gfx_bitmap_info(copy, &info) && info.width == 1 && info.height == 1);
        CHECK_HEX(word(copy, 0, 0), 0xDF606060u);
        call("bmFree", 1, J(copy));
        /* A matrix that mirrors. */
        copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(2), L(floats(mirror, 6)), Z(false)).j;
        CHECK(mg_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 2);
        CHECK_HEX(word(copy, 0, 0), 0xFF0000FFu);
        CHECK_HEX(word(copy, 1, 0), 0xFFFF0000u);
        CHECK_HEX(word(copy, 0, 1), 0x80808080u);
        call("bmFree", 1, J(copy));
        /* A quarter turn is drawing, which the shell does: the row of two becomes a column of two. */
        copy = call("bmCopy", 7, J(bm), I(2), I(1), I(2), I(1), L(floats(turn, 6)), Z(false)).j;
        CHECK(mg_java_gfx_bitmap_info(copy, &info) && info.width == 1 && info.height == 2);
        CHECK_HEX(word(copy, 0, 0), 0xFFFF0000u);
        CHECK_HEX(word(copy, 0, 1), 0xFF0000FFu);
        call("bmFree", 1, J(copy));
    }
    /* compress and decode: a PNG and back. */
    {
        mg_jobj *png = call("bmCompress", 3, J(bm), I(1), I(90)).l, *wh = ints(NULL, 2);
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
            CHECK(mg_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 1);
            CHECK_INT(ints_of(wh)[0], 2);
            CHECK_INT(ints_of(wh)[1], 1);
            call("bmFree", 1, J(copy));
            call("bmDecode", 6, L(png), I(0), I(n), I(8), L(wh), Z(true));
            CHECK_INT(ints_of(wh)[0], 1);
            CHECK_INT(ints_of(wh)[1], 1);
            /* The file can lie anywhere in the array; a range outside the array, or bytes that are no image, give 0. */
            {
                mg_jobj *padded = bytes(NULL, n + 10);
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
    mg_java_gfx_stats(&bytes1, &count1);
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
 * WebP pictures are decoded by the core itself, before the shell is asked (webp/mg_webp.h): the stand-in here reads PNG
 * and nothing else, as the phone's decoder reads no WebP. Five small files (tools/webp-fixtures.py), through
 * BitmapFactory's native as an app's pictures come: 32 x 32, in quadrants of 16.
 */
static void test_webp(void)
{
    mg_jobj *wh = ints(NULL, 2), *file;
    mg_java_gfx_bitmap info;
    i64 bm;
    if (!mg_webp_available()) {
        say("drawing natives: WebP -- this build has no WebP decoder in it (build/webp/src was not there)");
        /* The file is known for what it is all the same, and goes to the shell's decoder, which here says no. */
        CHECK(mg_webp_is(k_webp_lossless, sizeof(k_webp_lossless)) && !mg_webp_is("RIFF....WAVE", 12));
        CHECK(call("bmDecode", 6, L(bytes(k_webp_lossless, (int)sizeof(k_webp_lossless))), I(0), I((int)sizeof(k_webp_lossless)), I(1), L(wh), Z(false)).j == 0);
        return;
    }
    say("drawing natives: WebP pictures, decoded in the core");
    CHECK(mg_webp_is(k_webp_lossless, sizeof(k_webp_lossless)) && !mg_webp_is("RIFF....WAVE", 12) && !mg_webp_is(k_webp_lossless, 11));

    /* Lossless, with alpha: every pixel exactly, premultiplied -- opaque red, nothing, opaque blue, half-transparent white. */
    file = bytes(k_webp_lossless, (int)sizeof(k_webp_lossless));
    CHECK(call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossless)), I(1), L(wh), Z(true)).j == 0);
    CHECK(ints_of(wh)[0] == 32 && ints_of(wh)[1] == 32);
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossless)), I(1), L(wh), Z(false)).j;
    CHECK(bm != 0 && mg_java_gfx_bitmap_info(bm, &info) && info.width == 32 && info.height == 32);
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
    CHECK(bm != 0 && mg_java_gfx_bitmap_info(bm, &info) && info.width == 16 && info.height == 16 && ints_of(wh)[0] == 16);
    CHECK_HEX(word(bm, 2, 2), 0xFFFF0000u);
    CHECK_HEX(word(bm, 13, 2), 0x00000000u);
    CHECK_HEX(word(bm, 2, 13), 0xFF0000FFu);
    CHECK_HEX(word(bm, 13, 13), 0x80808080u);
    call("bmFree", 1, J(bm));
    bm = call("bmDecode", 6, L(file), I(0), I((int)sizeof(k_webp_lossless)), I(4), L(wh), Z(false)).j;
    CHECK(bm != 0 && mg_java_gfx_bitmap_info(bm, &info) && info.width == 8 && info.height == 8);
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

/*
 * A canvas's pixels lent out to be drawn into directly (the WebView's software table, drivers/mg_java_webview.c): the
 * bitmap's size and rows, the canvas's matrix as Skia's nine numbers, its clip's box in the bitmap's pixels; R, G, B, A in
 * memory while they are lent; and what was written there is the bitmap's afterwards, read back as the colour it is.
 */
static void test_canvas_raster(void)
{
    static const float move[6] = { 1, 0, 3, 0, 1, 2 };
    mg_java_gfx_raster r;
    i64 bm = call("bmNew", 2, I(8), I(6)).j, cv = call("cvNew", 1, J(bm)).j;
    u8 *px;
    say("  a canvas's pixels lent out (the WebView's software table): Android's byte order, the matrix, the clip");
    CHECK(!mg_java_gfx_canvas_raster(0, &r));
    call("cvMatrix", 3, J(cv), L(floats(move, 6)), Z(false));
    call("cvClipRect", 6, J(cv), F(0), F(0), F(4), F(3), I(1));
    CHECK(mg_java_gfx_canvas_raster(cv, &r));
    CHECK(r.pixels != NULL && r.width == 8 && r.height == 6 && r.stride == 32);
    CHECK(r.matrix[0] == 1 && r.matrix[1] == 0 && r.matrix[2] == 3 && r.matrix[3] == 0 && r.matrix[4] == 1 && r.matrix[5] == 2);
    CHECK(r.matrix[6] == 0 && r.matrix[7] == 0 && r.matrix[8] == 1);
    CHECK(!r.empty && r.clip[0] == 3 && r.clip[1] == 2 && r.clip[2] == 7 && r.clip[3] == 5);
    if (r.pixels) {
        px = (u8 *)r.pixels + 2 * 32 + 3 * 4;
        px[0] = 0xFF;
        px[1] = 0x00;
        px[2] = 0x00;
        px[3] = 0xFF;
        mg_java_gfx_canvas_raster_end(cv);
    }
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(3), I(2)).i, 0xFFFF0000u);
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(0), I(0)).i, 0u);
    /* The memory itself erased (bmEraseMemory), whatever canvas the bitmap has. */
    call("bmEraseMemory", 2, J(bm), I((i32)0xFF0000FFu));
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(5), I(4)).i, 0xFF0000FFu);
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
}

static void test_canvas(void)
{
    static const float move[6] = { 1, 0, 5, 0, 1, 7 }, twice[6] = { 2, 0, 0, 0, 2, 0 }, turn[6] = { 0, -1, 10, 1, 0, 0 };
    mg_jobj *m = floats(NULL, 6), *box = ints(NULL, 4), *red = paint(0xFFFF0000u);
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
    mg_jobj *red = paint(0xFFFF0000u), *p;
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
    ints_of(p)[P_STROKE] = (i32)mg_fb(2);
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
    /* Paths, by the framework's operations (mg_java_gfx.c's first comment). A triangle by relative lines: */
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
        ints_of(p)[P_STROKE] = (i32)mg_fb(1);
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
        ints_of(p)[P_STROKE] = (i32)mg_fb(4);
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
        mg_jobj *got = floats(NULL, 6);
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
    megler_gfx_host spy = *mg_gfx_standin();
    mg_jobj *p = paint(0x80123456u);
    i32 *a = ints_of(p);
    i64 bm = call("bmNew", 2, I(8), I(8)).j, cv = call("cvNew", 1, J(bm)).j, sh;
    say("  the paint the shell is handed, slot by slot");
    spy.draw_rect = spy_rect;
    mg_java_gfx_set_host(&spy);
    /* Every slot with a value of its own. */
    a[P_STYLE] = 2;
    a[P_STROKE] = (i32)mg_fb(3.5f);
    a[P_FLAGS] = 1 | 2 | 4 | 8 | 16;
    a[P_CAP] = 1;
    a[P_JOIN] = 2;
    a[P_TEXTSIZE] = (i32)mg_fb(17);
    a[P_ALIGN] = 2;
    a[P_MITER] = (i32)mg_fb(7);
    a[P_SHRAD] = (i32)mg_fb(2.5f);
    a[P_SHDX] = (i32)mg_fb(1.5f);
    a[P_SHDY] = (i32)mg_fb(-0.5f);
    a[P_SHCOL] = 0x40112233;
    a[P_SCALEX] = (i32)mg_fb(1.25f);
    a[P_SKEWX] = (i32)mg_fb(-0.25f);
    a[P_LSPACE] = (i32)mg_fb(0.125f);
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
    /*
     * With a shader it is the shell's: the paint is as the app set it, and the shell is handed its own shader, which the
     * Java's handle holds (the core keeps it in an object of its own, for the display lists that draw with it).
     */
    sh = call("shLinear", 7, F(0), F(0), F(8), F(0), L(NULL), L(NULL), I(0)).j;
    call("cvDrawRect", 7, J(cv), F(0), F(0), F(1), F(1), L(p), J(sh));
    CHECK_HEX(g_seen.color, 0x80123456u);
    CHECK_INT(g_seen.filter_mode, MEGLER_GFX_BLEND_SRC_IN);
    CHECK_HEX(g_seen.filter_color, 0xFF445566u);
    CHECK(g_seen_shader != NULL && g_seen_shader != (void *)(mg_addr)sh);
    call("shFree", 1, J(sh));
    /* What makes no sense is put right before the shell sees it. */
    a[P_STYLE] = 7;
    a[P_STROKE] = (i32)mg_fb(-2);
    a[P_CAP] = -1;
    a[P_JOIN] = 9;
    a[P_TEXTSIZE] = (i32)mg_fb(0);
    a[P_ALIGN] = 5;
    a[P_MITER] = (i32)mg_fb(-1);
    a[P_SHRAD] = (i32)mg_fb(-4);
    a[P_SCALEX] = (i32)mg_fb(0);
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
    mg_java_gfx_set_host(mg_gfx_standin());
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

/* ----------------------------------------------------------------------- text */

static void test_text(void)
{
    mg_jobj *s = mg_dvm_new_string("xA B"), *p = paint(0xFF0000FFu), *out5 = floats(NULL, 5), *box = ints(NULL, 4), *widths = floats(NULL, 4);
    mg_jobj *words = mg_dvm_new_string("hello world foo");
    i64 bm = call("bmNew", 2, I(64), I(40)).j, cv = call("cvNew", 1, J(bm)).j, face, bold, again;
    int i;
    say("  text and typefaces");
    /* Slot 6 is the text size. The string's part from start to end is the run; y is the baseline. The stand-in's boxes at size 20 are 10 by 14, 12 apart. */
    ints_of(p)[P_TEXTSIZE] = (i32)mg_fb(20);
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
    ints_of(p)[P_TEXTSIZE] = (i32)mg_fb(36);
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
    call("cvDrawText", 9, J(cv), L(mg_dvm_new_string(" ")), I(0), I(1), F(10), F(30), L(p), J(0), J(0));
    CHECK(covered(bm) == 22 * 2);
    CHECK_HEX(word(bm, 10, 19), 0xFFFF0000u);
    CHECK_HEX(word(bm, 31, 20), 0xFFFF0000u);
    ints_of(p)[P_FLAGS] = 0;
    ints_of(p)[P_TEXTSIZE] = (i32)mg_fb(20);
    /* Measuring: the same part of the string, the same paint. */
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(1), I(4), L(p), J(0)).f, 36);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(0), I(4), L(p), J(0)).f, 48);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(2), I(2), L(p), J(0)).f, 0);
    CHECK_FLOAT(call("txMeasure", 5, L(NULL), I(0), I(4), L(p), J(0)).f, 0);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(0), I(4), L(NULL), J(0)).f, 4 * 0.6f * 12);
    /* Slots 13 and 15, the scale and the letter spacing, are in the paint the shell is given. */
    ints_of(p)[P_SCALEX] = (i32)mg_fb(2);
    ints_of(p)[P_LSPACE] = (i32)mg_fb(0.4f);
    CHECK_FLOAT(call("txMeasure", 5, L(s), I(0), I(2), L(p), J(0)).f, 80);
    ints_of(p)[P_SCALEX] = (i32)mg_fb(1);
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
    ints_of(p)[P_TEXTSIZE] = (i32)mg_fb(10);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(50), L(p), J(0), Z(false)).i, 8);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(50), L(p), J(0), Z(true)).i, 6);
    CHECK_INT(call("txBreak", 7, L(words), I(6), I(15), F(50), L(p), J(0), Z(true)).i, 6);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(1000), L(p), J(0), Z(true)).i, 15);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(0), L(p), J(0), Z(true)).i, 0);
    CHECK_INT(call("txBreak", 7, L(words), I(0), I(15), F(-3), L(p), J(0), Z(false)).i, 0);
    /* Each char's advance; an array too short for them is left alone. */
    ints_of(p)[P_TEXTSIZE] = (i32)mg_fb(20);
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
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("sans-serif")), I(0)).j)).i, 400);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("sans-serif")), I(1)).j)).i, 700);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("sans-serif-medium")), I(0)).j)).i, 500);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("Sans-Serif-MEDIUM")), I(3)).j)).i, 800);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("sans-serif-light")), I(0)).j)).i, 300);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("sans-serif-thin")), I(0)).j)).i, 200);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("sans-serif-black")), I(0)).j)).i, 900);
    CHECK_INT(call("tfWeight", 1, J(call("tfCreate", 2, L(mg_dvm_new_string("monospace")), I(0)).j)).i, 400);
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
    mg_jobj *p = paint(0x80FFFFFFu);
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
    spy = *mg_gfx_standin();
    spy.shader_linear = spy_linear;
    mg_java_gfx_set_host(&spy);
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
    mg_java_gfx_set_host(mg_gfx_standin());
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

/* ---------------------------------------------------------------------- paths */

static void test_paths(void)
{
    mg_jobj *out = floats(NULL, 4), *first = floats(NULL, 2);
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
        /* The path as it is drawn, for Path.getPathIterator: the transform gone into the points before it, a rectangle in lines. */
        {
            static const float drawn[] = { 0, 2, 2, 1, 8, 10 }, rect[] = { 6, 4, 3, 1, 9, 0 }, box[] = { 0, 1, 3, 1, 4, 3, 1, 4, 9, 1, 1, 9, 4 };
            mg_jobj *segments = call("pathSegments", 2, L(floats(moved, 13)), I(13)).l;
            CHECK(segments && segments->u.arr.len == 6 && memcmp(floats_of(segments), drawn, sizeof(drawn)) == 0);
            segments = call("pathSegments", 2, L(floats(rect, 6)), I(6)).l;
            CHECK(segments && segments->u.arr.len == 13 && memcmp(floats_of(segments), box, sizeof(box)) == 0);
            segments = call("pathSegments", 2, L(floats(rect, 6)), I(0)).l;
            CHECK(segments && segments->u.arr.len == 0);
        }
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
    megler_gfx_host counting = *mg_gfx_standin();
    mg_java_gfx_bitmap info;
    mg_jobj *red = paint(0xFFFF0000u);
    i64 bm, cv, cv2, other, cv3;
    say("  when the shell is told to flush and that the memory changed; present; no shell at all");
    counting.flush = count_flush;
    counting.touched = count_touched;
    counting.bitmap_gone = count_gone;
    mg_java_gfx_set_host(&counting);
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
    mg_java_gfx_bitmap_info(bm, &info);
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
    CHECK(mg_java_gfx_lock_rgba(other, &info));
    mg_java_gfx_unlock_rgba(other);
    CHECK_INT(g_touches, 0);
    call("cvDrawRect", 7, J(cv3), F(0), F(0), F(1), F(1), L(red), J(0));
    CHECK_INT(g_touches, 1);
    mg_java_gfx_bitmap_changed(other);
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
    mg_java_gfx_set_present(on_present, &g_presents);
    call("cvDrawRect", 7, J(cv3), F(0), F(0), F(1), F(1), L(red), J(0));
    g_flushes = 0;
    call("present", 1, J(other));
    CHECK_INT(g_presents, 1);
    CHECK_INT(g_flushes, 1);
    mg_java_gfx_bitmap_info(other, &info);
    CHECK(g_presented == info.pixels && g_presented_w == 8 && g_presented_h == 8 && g_presented_stride == 32);
    call("present", 1, J(0));
    CHECK_INT(g_presents, 2);
    CHECK(g_presented == NULL && g_presented_w == 0);
    mg_java_gfx_set_present(NULL, NULL);
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
    mg_java_gfx_set_host(NULL);
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
    call("cvDrawText", 9, J(cv), L(mg_dvm_new_string("abc")), I(0), I(3), F(0), F(6), L(red), J(0), J(0));
    CHECK_INT(covered(bm), 0);
    call("bmSetPixel", 4, J(bm), I(1), I(1), I(-1));
    CHECK_HEX(call("bmGetPixel", 3, J(bm), I(1), I(1)).i, 0xFFFFFFFFu);
    CHECK_FLOAT(call("txMeasure", 5, L(mg_dvm_new_string("abc")), I(0), I(3), L(red), J(0)).f, 0);
    CHECK_FLOAT(call("txMetrics", 3, L(red), J(0), L(floats(NULL, 5))).f, 0);
    CHECK_INT(call("txBreak", 7, L(mg_dvm_new_string("abc")), I(0), I(3), F(100), L(red), J(0), Z(true)).i, 0);
    CHECK(call("shLinear", 7, F(0), F(0), F(8), F(0), L(NULL), L(NULL), I(0)).j == 0);
    CHECK(call("bmCompress", 3, J(bm), I(1), I(90)).l == NULL);
    CHECK(call("bmDecode", 6, L(bytes("\x89PNG\r\n\x1a\n....", 12)), I(0), I(12), I(1), L(NULL), Z(false)).j == 0);
    CHECK(call("tfFromData", 1, L(bytes("OTTO\0\1\0\0\0\0\0\0\0\0\0\0", 16))).j == 0);
    CHECK(call("tfCreate", 2, L(mg_dvm_new_string("serif")), I(0)).j != 0);
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
        CHECK(mg_java_gfx_bitmap_info(copy, &info) && info.width == 2 && info.height == 4);
        CHECK_INT(covered(copy), 0);
        call("bmFree", 1, J(copy));
    }
    call("cvFree", 1, J(cv));
    call("bmFree", 1, J(bm));
    CHECK(thrown()[0] == 0);
}

/* -------------------------------------------------------------- display lists */

/* gnode's flags as android.graphics.RenderNode packs them for rnSet (mg_java_gfx.c). */
enum { RN_IDENTITY = 1, RN_CLIP_BOUNDS = 2, RN_CLIP_RECT = 4, RN_CLIP_OUTLINE = 8, RN_LAYER = 16, RN_LAYER_PAINT = 32 };

/* How many pixels differ between two bitmaps of one size. */
static int differ(i64 a, i64 b)
{
    static const i32 none[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    return call("bmDiffer", 3, J(a), J(b), L(ints(none, 8))).i;
}

/* One of nearly everything a canvas draws, through the natives as the Java calls them, on whatever canvas it is given. */
static void scene(i64 cv, i64 pic)
{
    static const float move[6] = { 1, 0, 3, 0, 1, 2 }, slant[6] = { 1, 0.25f, 0, 0, 1, 0 }, place[6] = { 2, 0, 34, 0, 2, 32 };
    static const float tri[10] = { 0, 2, 20, 1, 12, 20, 1, 2, 30, 4 }, clip[6] = { 6, 30, 30, 48, 44, 0 };
    static const float lines[8] = { 30, 2, 46, 2, 30, 4, 46, 8 }, points[6] = { 33, 12, 36, 12, 39, 13 };
    mg_jobj *red = paint(0xFFFF0000u), *blue = paint(0xFF0000FFu), *green = paint(0x8000FF00u), *under = paint(0xFFFFFF00u);
    ints_of(under)[P_FLAGS] = 8;
    call("cvDrawColor", 3, J(cv), I((i32)0xFF202020u), I(3));
    call("cvDrawRect", 7, J(cv), F(1), F(1), F(9), F(5), L(red), J(0));
    call("cvSave", 1, J(cv));
    call("cvMatrix", 3, J(cv), L(floats(move, 6)), Z(false));
    call("cvClipRect", 6, J(cv), F(0), F(0), F(24), F(14), I(1));
    call("cvDrawOval", 7, J(cv), F(10), F(0), F(30), F(10), L(blue), J(0));
    call("cvDrawRRect", 9, J(cv), F(0), F(6), F(10), F(14), F(2), F(2), L(red), J(0));
    call("cvRestore", 1, J(cv));
    call("cvSaveLayer", 6, J(cv), F(14), F(14), F(30), F(30), I(128));
    call("cvDrawRect", 7, J(cv), F(16), F(16), F(26), F(24), L(blue), J(0));
    call("cvRestore", 1, J(cv));
    call("cvDrawPath", 6, J(cv), L(floats(tri, 10)), I(10), I(0), L(green), J(0));
    call("cvDrawLines", 7, J(cv), L(floats(lines, 8)), I(0), I(8), L(red), J(0), Z(false));
    call("cvDrawLines", 7, J(cv), L(floats(points, 6)), I(0), I(6), L(blue), J(0), Z(true));
    call("cvDrawArc", 10, J(cv), F(30), F(16), F(46), F(28), F(0), F(135), Z(true), L(red), J(0));
    call("cvDrawText", 9, J(cv), L(mg_dvm_new_string("Hi!")), I(0), I(3), F(2), F(44), L(under), J(0), J(0));
    call("cvDrawBitmap", 11, J(cv), J(pic), F(0), F(0), F(4), F(4), F(14), F(32), F(22), F(40), L(NULL));
    call("cvSave", 1, J(cv));
    call("cvClipPath", 5, J(cv), L(floats(clip, 6)), I(6), I(0), I(1));
    call("cvMatrix", 3, J(cv), L(floats(slant, 6)), Z(false));
    call("cvDrawBitmapM", 4, J(cv), J(pic), L(floats(place, 6)), L(red));
    call("cvRestoreTo", 2, J(cv), I(1));
}

/*
 * What a recorded call keeps of its Paint, every part of it: a grid of cells over a striped background, each cell one call
 * with one kind of paint -- a bitmap drawn with no paint, with the filter flag, at an alpha, through colour filters of
 * several modes (PorterDuff's and BlendMode's numbers are the same here), by transfer modes; a rectangle, a path and a run of
 * text likewise, and with a shader; layers put on at an alpha and by a mode. Drawn on whatever canvas it is given.
 */
static void paint_cases(i64 cv, i64 pic, i64 shader)
{
    static const i32 filters[] = { 5, 9, 13, 6, 14, 1 }, blends[] = { 0, 1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17 };
    static const float tri[10] = { 0, 0, 0, 1, 6, 0, 1, 0, 6, 4 };
    mg_jobj *stripe = paint(0xFF3060A0u);
    float at[6] = { 1, 0, 0, 0, 1, 0 }, x, y;
    int i, n = 0;
    call("cvDrawColor", 3, J(cv), I((i32)0xFFE0E0E0u), I(3));
    for (i = 0; i < 12; i++) call("cvDrawRect", 7, J(cv), F(0), F((float)(8 * i)), F(96), F((float)(8 * i + 4)), L(stripe), J(0));
#define CELL() (x = (float)(8 * (n % 12)), y = (float)(8 * (n / 12)), n++)
    /* Bitmaps: no paint, a paint with the filter flag and without, an alpha. */
    CELL();
    call("cvDrawBitmap", 11, J(cv), J(pic), F(0), F(0), F(4), F(4), F(x), F(y), F(x + 7), F(y + 7), L(NULL));
    {
        mg_jobj *p = paint(0xFF000000u);
        ints_of(p)[P_FLAGS] = 2;
        CELL();
        call("cvDrawBitmap", 11, J(cv), J(pic), F(0), F(0), F(4), F(4), F(x), F(y), F(x + 7), F(y + 7), L(p));
        CELL();
        call("cvDrawBitmap", 11, J(cv), J(pic), F(0), F(0), F(4), F(4), F(x), F(y), F(x + 7), F(y + 7), L(paint(0x80000000u)));
    }
    for (i = 0; i < (int)MG_COUNT(filters); i++) {
        mg_jobj *p = paint(0xC0FFFFFFu);
        ints_of(p)[P_CFCOLOR] = (i32)0xFF20C040u;
        ints_of(p)[P_CFMODE] = filters[i];
        CELL();
        call("cvDrawBitmap", 11, J(cv), J(pic), F(0), F(0), F(4), F(4), F(x), F(y), F(x + 7), F(y + 7), L(p));
        CELL();
        call("cvDrawRect", 7, J(cv), F(x), F(y), F(x + 7), F(y + 7), L(p), J(0));
        CELL();
        call("cvDrawRect", 7, J(cv), F(x), F(y), F(x + 7), F(y + 7), L(p), J(shader));
        CELL();
        ints_of(p)[P_TEXTSIZE] = (i32)mg_fb(8);
        call("cvDrawText", 9, J(cv), L(mg_dvm_new_string("W")), I(0), I(1), F(x), F(y + 7), L(p), J(0), J(0));
    }
    for (i = 0; i < (int)MG_COUNT(blends); i++) {
        mg_jobj *p = paint(0xA0C02020u);
        ints_of(p)[P_XFER] = blends[i];
        CELL();
        call("cvDrawBitmap", 11, J(cv), J(pic), F(0), F(0), F(4), F(4), F(x), F(y), F(x + 7), F(y + 7), L(p));
        CELL();
        call("cvDrawOval", 7, J(cv), F(x), F(y), F(x + 7), F(y + 7), L(p), J(0));
        CELL();
        at[2] = x;
        at[5] = y;
        call("cvSave", 1, J(cv));
        call("cvMatrix", 3, J(cv), L(floats(at, 6)), Z(false));
        call("cvDrawPath", 6, J(cv), L(floats(tri, 10)), I(10), I(0), L(p), J(shader));
        call("cvRestore", 1, J(cv));
        /* A layer by that mode, with something of its own in it. */
        CELL();
        call("cvSaveLayerMode", 7, J(cv), F(x), F(y), F(x + 7), F(y + 7), I(0xC0), I(blends[i]));
        call("cvDrawRect", 7, J(cv), F(x + 1), F(y + 1), F(x + 6), F(y + 6), L(paint(0xFF2080F0u)), J(0));
        call("cvRestore", 1, J(cv));
    }
#undef CELL
}

/*
 * paint_cases drawn directly and through a list, both inside a node's save and move as a node's list is drawn, on the host
 * set now: how many pixels differ.
 */
static int paint_cases_differ(void)
{
    static const float move[6] = { 1, 0, 3, 0, 1, 2 };
    static const i32 two[2] = { (i32)0xFFF0A000u, (i32)0xFF00A0F0u };
    i64 pic = call("bmNew", 2, I(4), I(4)).j, a = call("bmNew", 2, I(104), I(104)).j, b = call("bmNew", 2, I(104), I(104)).j;
    i64 ca = call("cvNew", 1, J(a)).j, cb = call("cvNew", 1, J(b)).j, sh, rc, list;
    int d;
    call("bmErase", 2, J(pic), I((i32)0xFFFF8000u));
    call("bmSetPixel", 4, J(pic), I(1), I(1), I((i32)0x80FFFFFFu));
    call("bmSetPixel", 4, J(pic), I(2), I(2), I(0));
    sh = call("shLinear", 7, F(0), F(0), F(96), F(0), L(ints(two, 2)), L(NULL), I(0)).j;
    call("cvSave", 1, J(ca));
    call("cvMatrix", 3, J(ca), L(floats(move, 6)), Z(false));
    paint_cases(ca, pic, sh);
    call("cvRestore", 1, J(ca));
    rc = call("cvRecord", 2, I(100), I(100)).j;
    paint_cases(rc, pic, sh);
    list = call("cvEndRecord", 1, J(rc)).j;
    call("cvFree", 1, J(rc));
    call("cvMatrix", 3, J(cb), L(floats(move, 6)), Z(false));
    call("cvDrawList", 2, J(cb), J(list));
    d = differ(a, b);
    call("dlFree", 1, J(list));
    call("shFree", 1, J(sh));
    call("cvFree", 1, J(ca));
    call("cvFree", 1, J(cb));
    call("bmFree", 1, J(a));
    call("bmFree", 1, J(b));
    call("bmFree", 1, J(pic));
    return d;
}

/* A node's properties as RenderNode hands them over: its place, a matrix (none: the identity), alpha, flags, a layer's alpha, a clip rectangle, an outline. */
static void node_set(i64 node, float l, float t, float r, float b, const float *matrix, float alpha, i32 flags, i32 layer_alpha, const float *clip,
                     i32 outline_kind, const float *outline, float radius)
{
    float f[20];
    i32 k[5] = { 0, 0, 0, 0, 0 };
    memset(f, 0, sizeof(f));
    f[0] = l;
    f[1] = t;
    f[2] = r;
    f[3] = b;
    memcpy(f + 4, matrix ? matrix : k_identity, sizeof(float) * 6);
    f[10] = alpha;
    if (clip) memcpy(f + 11, clip, sizeof(float) * 4);
    if (outline) memcpy(f + 15, outline, sizeof(float) * 4);
    f[19] = radius;
    k[0] = flags | (matrix ? 0 : RN_IDENTITY);
    k[1] = outline_kind;
    k[2] = layer_alpha;
    call("rnSet", 5, J(node), L(floats(f, 20)), L(ints(k, 5)), L(NULL), L(NULL));
}

static void node_place(i64 node, float l, float t, float r, float b, i32 flags) { node_set(node, l, t, r, b, NULL, 1, flags, 255, NULL, 0, NULL, 0); }

/* A node's list made of one rectangle in its own coordinates. */
static void node_rect(i64 node, int w, int h, float l, float t, float r, float b, u32 color)
{
    i64 rc = call("cvRecord", 2, I(w), I(h)).j;
    call("cvDrawRect", 7, J(rc), F(l), F(t), F(r), F(b), L(paint(color)), J(0));
    call("rnEnd", 2, J(node), J(rc));
    call("cvFree", 1, J(rc));
}

/* A node's list made of a drawing of other nodes. */
static void node_of(i64 node, i64 child, i64 other)
{
    i64 rc = call("cvRecord", 2, I(64), I(64)).j;
    call("cvDrawNode", 2, J(rc), J(child));
    if (other) call("cvDrawNode", 2, J(rc), J(other));
    call("rnEnd", 2, J(node), J(rc));
    call("cvFree", 1, J(rc));
}

/* One of a Canvas's moves, as the framework hands translate and scale over: pre-concatenated. */
static void move_by(i64 cv, float sx, float sy, float tx, float ty)
{
    const float m[6] = { sx, 0, tx, 0, sy, ty };
    call("cvMatrix", 3, J(cv), L(floats(m, 6)), Z(false));
}

/*
 * Spotify's play/pause button on its now-playing screen as the phone showed it: the View form, p.daf drawing p.zg71 (the
 * drawable of one glyph of Spotify's icon font). A white disc, then the glyph: black, a run of text in a face from a font
 * file's bytes at 64 dp (224 px at 560 dpi), placed by zg71 from its measures (an advance of 224, a height of 1.0233 of
 * its size, the baseline 1.00645 of it below its middle), each drawable between save and restoreToCount, all under the
 * scale the button's ImageView gives its 511 px drawable to fit 236 px. Two bars stand in for the pause glyph. On
 * whatever canvas it is given, 236 px square.
 */
static void player_button(i64 cv, i64 face)
{
    mg_jobj *disc = paint(0xFFFFFFFFu), *glyph = paint(0xFF000000u);
    i32 outer, inner;
    ints_of(glyph)[P_TEXTSIZE] = (i32)mg_fb(224);
    outer = call("cvSave", 1, J(cv)).i;
    move_by(cv, 236.0f / 511, 236.0f / 511, 0, 0);
    /* daf: the disc about its middle, then the glyph's box (224 x 230) scaled to the disc and by 0.45, about the middle too. */
    move_by(cv, 1, 1, 255.5f, 255.5f);
    call("cvDrawOval", 7, J(cv), F(-255.5f), F(-255.5f), F(255.5f), F(255.5f), L(disc), J(0));
    move_by(cv, 511.0f / 230, 511.0f / 230, 0, 0);
    move_by(cv, 0.45f, 0.45f, 0, 0);
    move_by(cv, 1, 1, -112, -115);
    /* zg71: to its box's middle, down to the baseline, back by half the advance and half the height. */
    inner = call("cvSave", 1, J(cv)).i;
    move_by(cv, 1, 1, 112, 115);
    move_by(cv, 1, 1, 0, 224 * 1.00645f);
    move_by(cv, 1, 1, -112, -224 * 1.0233f / 2);
    call("cvDrawText", 9, J(cv), L(mg_dvm_new_string("II")), I(0), I(2), F(0), F(0), L(glyph), J(face), J(0));
    call("cvRestoreTo", 2, J(cv), I(inner));
    call("cvRestoreTo", 2, J(cv), I(outer));
}

/* How many pixels of a part of a bitmap are dark (each of red, green and blue under 0x60) and opaque. */
static int dark(i64 bm, int x0, int y0, int x1, int y1)
{
    int x, y, n = 0;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            u32 w = word(bm, x, y);
            n += (w >> 24) > 0xC0 && ((w >> 16) & 255) < 0x60 && ((w >> 8) & 255) < 0x60 && (w & 255) < 0x60;
        }
    return n;
}

/*
 * player_button drawn on the host set now three ways, into bitmaps of its size over a dark ground: directly, as it is
 * drawn on the window; through a list; and in a node a parent node draws, as a graphics layer of Compose's would hold it.
 * How many pixels the list and the node give that the direct drawing does not; *ink, how many dark pixels each of the
 * three has where the glyph is (the least of them).
 */
static int player_button_differ(i64 face, int *ink)
{
    i64 direct = call("bmNew", 2, I(236), I(236)).j, listed = call("bmNew", 2, I(236), I(236)).j, noded = call("bmNew", 2, I(236), I(236)).j;
    i64 cd = call("cvNew", 1, J(direct)).j, cl = call("cvNew", 1, J(listed)).j, cn = call("cvNew", 1, J(noded)).j;
    i64 rc, list, node = call("rnNew", 0).j, parent = call("rnNew", 0).j;
    int d, a, b, c;
    call("cvDrawColor", 3, J(cd), I((i32)0xFF202020u), I(1));
    call("cvDrawColor", 3, J(cl), I((i32)0xFF202020u), I(1));
    call("cvDrawColor", 3, J(cn), I((i32)0xFF202020u), I(1));
    player_button(cd, face);
    rc = call("cvRecord", 2, I(236), I(236)).j;
    player_button(rc, face);
    list = call("cvEndRecord", 1, J(rc)).j;
    call("cvFree", 1, J(rc));
    call("cvDrawList", 2, J(cl), J(list));
    rc = call("cvRecord", 2, I(236), I(236)).j;
    player_button(rc, face);
    call("rnEnd", 2, J(node), J(rc));
    call("cvFree", 1, J(rc));
    node_place(node, 0, 0, 236, 236, RN_CLIP_BOUNDS);
    rc = call("cvRecord", 2, I(236), I(236)).j;
    call("cvDrawNode", 2, J(rc), J(node));
    call("rnEnd", 2, J(parent), J(rc));
    call("cvFree", 1, J(rc));
    node_place(parent, 0, 0, 236, 236, RN_CLIP_BOUNDS);
    call("cvDrawNode", 2, J(cn), J(parent));
    d = differ(direct, listed) + differ(direct, noded);
    a = dark(direct, 60, 80, 130, 180);
    b = dark(listed, 60, 80, 130, 180);
    c = dark(noded, 60, 80, 130, 180);
    *ink = a < b ? (a < c ? a : c) : (b < c ? b : c);
    call("dlFree", 1, J(list));
    call("rnFree", 1, J(parent));
    call("rnFree", 1, J(node));
    call("cvFree", 1, J(cd));
    call("cvFree", 1, J(cl));
    call("cvFree", 1, J(cn));
    call("bmFree", 1, J(direct));
    call("bmFree", 1, J(listed));
    call("bmFree", 1, J(noded));
    return d;
}

static int g_shader_matrices, g_shader_frees;
static bool g_shader_had_matrix;
static void spy_shader_matrix(void *user, void *shader, const float *matrix)
{
    g_shader_matrices++;
    g_shader_had_matrix = matrix != NULL;
    mg_gfx_standin()->shader_matrix(user, shader, matrix);
}
static void spy_shader_free(void *user, void *shader)
{
    g_shader_frees++;
    mg_gfx_standin()->shader_free(user, shader);
}

static void test_lists(void)
{
    static const float move[6] = { 1, 0, 5, 0, 1, 7 }, twice[6] = { 2, 0, 0, 0, 2, 0 }, by2[6] = { 1, 0, 2, 0, 1, 2 };
    static const float tri[10] = { 0, 2, 2, 1, 10, 2, 1, 2, 10, 4 };
    uint64_t bytes0, bytes1, lbytes;
    uint32_t count0, count1, lists0, lists, nodes0, nodes;
    mg_jobj *m = floats(NULL, 6), *box = ints(NULL, 4);
    i64 a = call("bmNew", 2, I(64), I(64)).j, b = call("bmNew", 2, I(64), I(64)).j, pic = call("bmNew", 2, I(4), I(4)).j;
    i64 ca = call("cvNew", 1, J(a)).j, cb = call("cvNew", 1, J(b)).j, rc, list;
    int i;
    say("  display lists: a recording canvas, its list drawn again, what it keeps, nodes drawn with what they have when drawn");
    mg_java_gfx_list_stats(NULL, &lists0, &nodes0);
    call("bmErase", 2, J(pic), I((i32)0xFF00FFFFu));
    call("bmSetPixel", 4, J(pic), I(0), I(0), I((i32)0xFFFF00FFu));
    call("bmSetPixel", 4, J(pic), I(3), I(2), I((i32)0x80FFFFFFu));

    /* A recording canvas: a canvas's bookkeeping from (0, 0, width, height), and nothing drawn anywhere, no bitmap made. */
    rc = call("cvRecord", 2, I(48), I(40)).j;
    CHECK(rc != 0);
    CHECK_INT(call("cvSaveCount", 1, J(rc)).i, 1);
    CHECK(call("cvClipBounds", 2, J(rc), L(box)).z);
    CHECK(ints_of(box)[0] == 0 && ints_of(box)[1] == 0 && ints_of(box)[2] == 48 && ints_of(box)[3] == 40);
    mg_java_gfx_stats(&bytes0, &count0);
    scene(rc, pic);
    mg_java_gfx_stats(&bytes1, &count1);
    CHECK_INT(count1, count0);
    CHECK_INT(bytes1, bytes0);
    CHECK_INT(call("cvSaveCount", 1, J(rc)).i, 1);
    CHECK_INT(covered(b), 0);
    list = call("cvEndRecord", 1, J(rc)).j;
    CHECK(list != 0);
    /* Ended, it records no more and draws nothing. */
    CHECK(call("cvEndRecord", 1, J(rc)).j == 0);
    call("cvDrawRect", 7, J(rc), F(0), F(0), F(8), F(8), L(paint(0xFFFF0000u)), J(0));
    call("cvFree", 1, J(rc));
    mg_java_gfx_list_stats(&lbytes, &lists, NULL);
    CHECK_INT(lists, lists0 + 1);
    CHECK(lbytes > 0 && lbytes < 8192);
    /* Drawn again on a canvas moved by (5, 7), it is what the same calls draw there, pixel for pixel. */
    call("cvMatrix", 3, J(ca), L(floats(move, 6)), Z(false));
    scene(ca, pic);
    call("cvMatrix", 3, J(cb), L(floats(move, 6)), Z(false));
    call("cvDrawList", 2, J(cb), J(list));
    CHECK_INT(covered(a), 64 * 64);
    CHECK_INT(differ(a, b), 0);
    /* The canvas is left as it was found: one save, its own matrix. */
    CHECK_INT(call("cvSaveCount", 1, J(cb)).i, 1);
    call("cvGetMatrix", 2, J(cb), L(m));
    CHECK(memcmp(floats_of(m), move, sizeof(move)) == 0);
    /* A list is drawn as often as it is asked to be. */
    erase(b);
    call("cvDrawList", 2, J(cb), J(list));
    CHECK_INT(differ(a, b), 0);
    /* A list recorded into another is the list as it was then, whatever its Picture does afterwards. */
    rc = call("cvRecord", 2, I(64), I(64)).j;
    call("cvDrawList", 2, J(rc), J(list));
    call("dlFree", 1, J(list));
    list = call("cvEndRecord", 1, J(rc)).j;
    call("cvFree", 1, J(rc));
    erase(b);
    call("cvDrawList", 2, J(cb), J(list));
    CHECK_INT(differ(a, b), 0);
    call("dlFree", 1, J(list));
    call("cvFree", 1, J(ca));
    call("cvFree", 1, J(cb));
    ca = call("cvNew", 1, J(a)).j;
    cb = call("cvNew", 1, J(b)).j;

    /* What a call was given is what the list keeps: the paint, the path and the matrix as they were then, whatever is done to the arrays afterwards. */
    {
        mg_jobj *p = paint(0xFFFF0000u), *path = floats(tri, 10), *mx = floats(by2, 6);
        rc = call("cvRecord", 2, I(16), I(16)).j;
        call("cvMatrix", 3, J(rc), L(mx), Z(false));
        call("cvDrawPath", 6, J(rc), L(path), I(10), I(0), L(p), J(0));
        list = call("cvEndRecord", 1, J(rc)).j;
        call("cvFree", 1, J(rc));
        ints_of(p)[P_COLOR] = (i32)0xFF0000FFu;
        floats_of(path)[1] = 40;
        floats_of(mx)[2] = 30;
        erase(a);
        call("cvDrawList", 2, J(ca), J(list));
        CHECK_HEX(word(a, 4, 4), 0xFFFF0000u);
        CHECK_HEX(word(a, 5, 9), 0xFFFF0000u);
        CHECK_HEX(word(a, 3, 3), 0u);
        CHECK_HEX(word(a, 20, 5), 0u);
        CHECK_HEX(word(a, 34, 5), 0u);
        call("dlFree", 1, J(list));
    }

    /* A bitmap is drawn as it is when the list is drawn, as Android draws it; it stays while a list draws it, whatever the Java does. */
    {
        i64 src = call("bmNew", 2, I(2), I(2)).j;
        call("bmErase", 2, J(src), I((i32)0xFFFF0000u));
        rc = call("cvRecord", 2, I(8), I(8)).j;
        call("cvDrawBitmap", 11, J(rc), J(src), F(0), F(0), F(2), F(2), F(0), F(0), F(4), F(4), L(NULL));
        list = call("cvEndRecord", 1, J(rc)).j;
        call("cvFree", 1, J(rc));
        call("bmErase", 2, J(src), I((i32)0xFF0000FFu));
        erase(a);
        call("cvDrawList", 2, J(ca), J(list));
        CHECK_HEX(word(a, 1, 1), 0xFF0000FFu);
        CHECK(exactly(a, 0, 0, 4, 4));
        mg_java_gfx_stats(&bytes0, &count0);
        call("bmFree", 1, J(src));
        mg_java_gfx_stats(&bytes1, &count1);
        CHECK_INT(count1, count0);
        erase(a);
        call("cvDrawList", 2, J(ca), J(list));
        CHECK_HEX(word(a, 3, 3), 0xFF0000FFu);
        call("dlFree", 1, J(list));
        mg_java_gfx_stats(&bytes1, &count1);
        CHECK_INT(count1, count0 - 1);
        CHECK_INT(bytes1, bytes0 - 16);
    }

    /* A shader: drawn with the local matrix it had when recorded, its own given back afterwards; freed when the Java and the last list are done with it. */
    {
        static const i32 two[2] = { (i32)0xFF00FF00u, (i32)0xFF0000FFu };
        megler_gfx_host spy = *mg_gfx_standin();
        i64 sh;
        spy.shader_matrix = spy_shader_matrix;
        spy.shader_free = spy_shader_free;
        mg_java_gfx_set_host(&spy);
        sh = call("shLinear", 7, F(0), F(0), F(8), F(0), L(ints(two, 2)), L(NULL), I(0)).j;
        rc = call("cvRecord", 2, I(8), I(8)).j;
        call("cvDrawRect", 7, J(rc), F(0), F(0), F(4), F(4), L(paint(0xFF000000u)), J(sh));
        list = call("cvEndRecord", 1, J(rc)).j;
        call("cvFree", 1, J(rc));
        call("shMatrix", 2, J(sh), L(floats(twice, 6)));
        g_shader_matrices = g_shader_frees = 0;
        erase(a);
        call("cvDrawList", 2, J(ca), J(list));
        CHECK_HEX(word(a, 1, 1), 0xFF00FF00u);
        CHECK_INT(g_shader_matrices, 2);
        CHECK(g_shader_had_matrix);
        /* Drawn with the matrix it has, nothing is changed round the call. */
        call("shMatrix", 2, J(sh), L(NULL));
        g_shader_matrices = 0;
        call("cvDrawList", 2, J(ca), J(list));
        CHECK_INT(g_shader_matrices, 0);
        call("shFree", 1, J(sh));
        CHECK_INT(g_shader_frees, 0);
        call("dlFree", 1, J(list));
        CHECK_INT(g_shader_frees, 1);
        mg_java_gfx_set_host(mg_gfx_standin());
    }

    /* Every part of a Paint is what the list keeps: a grid of bitmaps, shapes, text and layers by every filter and mode, the same both ways. */
    CHECK_INT(paint_cases_differ(), 0);
    /* Spotify's player button, a glyph in a face from bytes under several moves between save and restoreToCount: the same three ways. */
    {
        static const u8 font[16] = { 0, 1, 0, 0 };
        i64 face = call("tfFromData", 1, L(bytes(font, 16))).j;
        int ink = 0;
        CHECK(face != 0);
        CHECK_INT(player_button_differ(face, &ink), 0);
    }

    /* Nodes. A node's list is drawn where the node is, clipped to its bounds when it clips to them. */
    {
        static const float clip[4] = { 2, 2, 6, 6 }, outline[4] = { 0, 0, 5, 5 };
        i64 child = call("rnNew", 0).j, parent = call("rnNew", 0).j, grand = call("rnNew", 0).j, middle = call("rnNew", 0).j, top = call("rnNew", 0).j;
        i64 e = call("bmNew", 2, I(10), I(10)).j, ce = call("cvNew", 1, J(e)).j;
        mg_java_gfx_list_stats(NULL, NULL, &nodes);
        CHECK_INT(nodes, nodes0 + 5);
        /* Red in its bounds, blue just outside them. */
        rc = call("cvRecord", 2, I(10), I(10)).j;
        call("cvDrawRect", 7, J(rc), F(0), F(0), F(10), F(10), L(paint(0xFFFF0000u)), J(0));
        call("cvDrawRect", 7, J(rc), F(10), F(0), F(14), F(10), L(paint(0xFF0000FFu)), J(0));
        call("rnEnd", 2, J(child), J(rc));
        call("cvFree", 1, J(rc));
        node_place(child, 4, 4, 14, 14, RN_CLIP_BOUNDS);
        node_of(parent, child, 0);
        node_place(parent, 0, 0, 64, 64, RN_CLIP_BOUNDS);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK(exactly(a, 4, 4, 14, 14));
        CHECK_HEX(word(a, 13, 13), 0xFFFF0000u);
        /* Its properties changed and the parent not recorded again: the parent draws it as it is now, here not clipping to its bounds. */
        node_place(child, 16, 2, 26, 12, 0);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK(exactly(a, 16, 2, 30, 12));
        CHECK_HEX(word(a, 27, 5), 0xFF0000FFu);
        /* A matrix of its own: twice the size. */
        node_set(child, 0, 0, 10, 10, twice, 1, RN_CLIP_BOUNDS, 255, NULL, 0, NULL, 0);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK(exactly(a, 0, 0, 20, 20));
        /* Translucent: a layer of its bounds put on at its alpha, which is what its bitmap drawn at that alpha gave, pixel for pixel. */
        node_set(child, 4, 4, 14, 14, NULL, 0.5f, RN_CLIP_BOUNDS | RN_LAYER, 128, NULL, 0, NULL, 0);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        erase(b);
        call("cvDrawRect", 7, J(ce), F(0), F(0), F(10), F(10), L(paint(0xFFFF0000u)), J(0));
        call("cvDrawBitmap", 11, J(cb), J(e), F(0), F(0), F(10), F(10), F(4), F(4), F(14), F(14), L(paint(0x80000000u)));
        CHECK_INT(differ(a, b), 0);
        CHECK_HEX(word(a, 5, 5), 0x80800000u);
        CHECK(exactly(a, 4, 4, 14, 14));
        /* A colour filter on the layer's paint: drawn through a bitmap of the node with that paint, as before, and the bitmap gone afterwards. */
        {
            mg_jobj *lp = paint(0xFF000000u), *f = floats(NULL, 20), *k = ints(NULL, 5);
            ints_of(lp)[P_CFCOLOR] = (i32)0xFF00FF00u;
            ints_of(lp)[P_CFMODE] = 5;
            floats_of(f)[0] = floats_of(f)[1] = 4;
            floats_of(f)[2] = floats_of(f)[3] = 14;
            floats_of(f)[10] = 1;
            ints_of(k)[0] = RN_IDENTITY | RN_CLIP_BOUNDS | RN_LAYER | RN_LAYER_PAINT;
            ints_of(k)[2] = 200;
            call("rnSet", 5, J(child), L(f), L(k), L(NULL), L(lp));
            mg_java_gfx_stats(&bytes0, &count0);
            erase(a);
            call("cvDrawNode", 2, J(ca), J(parent));
            mg_java_gfx_stats(&bytes1, &count1);
            CHECK_INT(count1, count0);
            ints_of(lp)[P_COLOR] = (i32)0xC8000000u;
            erase(b);
            call("cvDrawBitmap", 11, J(cb), J(e), F(0), F(0), F(10), F(10), F(4), F(4), F(14), F(14), L(lp));
            CHECK_INT(differ(a, b), 0);
            CHECK(exactly(a, 4, 4, 14, 14));
            CHECK((word(a, 5, 5) & 0x00FFFF00u) != 0 && (word(a, 5, 5) & 0x00FF0000u) == 0);
        }
        /* Its clip rectangle, and then its outline. */
        node_set(child, 4, 4, 14, 14, NULL, 1, RN_CLIP_BOUNDS | RN_CLIP_RECT, 255, clip, 0, NULL, 0);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK(exactly(a, 6, 6, 10, 10));
        node_set(child, 4, 4, 14, 14, NULL, 1, RN_CLIP_OUTLINE, 255, NULL, 1, outline, 0);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK(exactly(a, 4, 4, 9, 9));
        /* With round corners it is a path, which the stand-in clips to as its box. */
        node_set(child, 4, 4, 14, 14, NULL, 1, RN_CLIP_OUTLINE, 255, NULL, 1, outline, 2);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK(exactly(a, 4, 4, 9, 9));
        /* An outline to clip to that is empty, or no alpha: nothing at all, as on Android. */
        node_set(child, 4, 4, 14, 14, NULL, 1, RN_CLIP_OUTLINE, 255, NULL, 0, NULL, 0);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK_INT(covered(a), 0);
        node_set(child, 4, 4, 14, 14, NULL, 0, 0, 255, NULL, 0, NULL, 0);
        call("cvDrawNode", 2, J(ca), J(parent));
        CHECK_INT(covered(a), 0);

        /* Nested: a node in a node in a node, each moved by its own place; the innermost moved, nothing recorded again. */
        node_rect(grand, 2, 2, 0, 0, 2, 2, 0xFF00FF00u);
        node_place(grand, 1, 1, 3, 3, RN_CLIP_BOUNDS);
        node_of(middle, grand, 0);
        node_place(middle, 10, 10, 20, 20, RN_CLIP_BOUNDS);
        node_of(top, middle, 0);
        node_place(top, 0, 0, 64, 64, RN_CLIP_BOUNDS);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(top));
        CHECK(exactly(a, 11, 11, 13, 13));
        node_place(grand, 5, 5, 7, 7, RN_CLIP_BOUNDS);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(top));
        CHECK(exactly(a, 15, 15, 17, 17));
        /* Discarded: drawn as nothing. Recorded again with something else: the outer lists draw the new list. */
        call("rnDiscard", 1, J(grand));
        erase(a);
        call("cvDrawNode", 2, J(ca), J(top));
        CHECK_INT(covered(a), 0);
        node_rect(grand, 2, 2, 0, 0, 2, 2, 0xFF0000FFu);
        call("cvDrawNode", 2, J(ca), J(top));
        CHECK(exactly(a, 15, 15, 17, 17));
        CHECK_HEX(word(a, 16, 16), 0xFF0000FFu);
        /* A node drawn on a recording canvas is drawn by the list it went into, not where it was recorded. */
        rc = call("cvRecord", 2, I(64), I(64)).j;
        call("cvDrawNode", 2, J(rc), J(top));
        CHECK_INT(covered(a), 4);
        call("cvFree", 1, J(rc));
        /* The Java's nodes gone, the lists that draw them keep them; the last list gone, the nodes go. */
        call("rnFree", 1, J(grand));
        call("rnFree", 1, J(middle));
        erase(a);
        call("cvDrawNode", 2, J(ca), J(top));
        CHECK(exactly(a, 15, 15, 17, 17));
        mg_java_gfx_list_stats(NULL, NULL, &nodes);
        CHECK_INT(nodes, nodes0 + 5);
        call("rnFree", 1, J(top));
        mg_java_gfx_list_stats(NULL, NULL, &nodes);
        CHECK_INT(nodes, nodes0 + 2);
        call("rnFree", 1, J(parent));
        call("rnFree", 1, J(child));
        call("cvFree", 1, J(ce));
        call("bmFree", 1, J(e));
    }

    /* A node whose list draws the node itself is drawn so deep and no deeper: nothing hangs, nothing overflows. */
    {
        i64 self = call("rnNew", 0).j;
        rc = call("cvRecord", 2, I(8), I(8)).j;
        call("cvDrawRect", 7, J(rc), F(0), F(0), F(2), F(2), L(paint(0xFFFF0000u)), J(0));
        call("cvDrawNode", 2, J(rc), J(self));
        call("rnEnd", 2, J(self), J(rc));
        call("cvFree", 1, J(rc));
        node_place(self, 1, 0, 9, 8, 0);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(self));
        CHECK(word(a, 1, 1) == 0xFFFF0000u && word(a, 63, 1) == 0xFFFF0000u);
        call("rnDiscard", 1, J(self));
        call("rnFree", 1, J(self));
    }

    /* Two hundred nodes the size of a phone's screen, recorded and drawn, as a Compose screen has them: no bitmap for any of them. */
    {
        i64 nodes200[200], all = call("rnNew", 0).j;
        mg_java_gfx_stats(&bytes0, &count0);
        for (i = 0; i < 200; i++) {
            nodes200[i] = call("rnNew", 0).j;
            rc = call("cvRecord", 2, I(1440), I(2560)).j;
            call("cvDrawColor", 3, J(rc), I((i32)0x10FFFFFFu), I(3));
            call("cvDrawRect", 7, J(rc), F((float)i), F(0), F((float)i + 1), F(2560), L(paint(0xFF000000u | (u32)i)), J(0));
            call("rnEnd", 2, J(nodes200[i]), J(rc));
            call("cvFree", 1, J(rc));
            node_place(nodes200[i], 0, 0, 1440, 2560, RN_CLIP_BOUNDS);
        }
        rc = call("cvRecord", 2, I(1440), I(2560)).j;
        for (i = 0; i < 200; i++) call("cvDrawNode", 2, J(rc), J(nodes200[i]));
        call("rnEnd", 2, J(all), J(rc));
        call("cvFree", 1, J(rc));
        node_place(all, 0, 0, 1440, 2560, RN_CLIP_BOUNDS);
        erase(a);
        call("cvDrawNode", 2, J(ca), J(all));
        CHECK_INT(covered(a), 64 * 64);
        mg_java_gfx_stats(&bytes1, &count1);
        mg_java_gfx_list_stats(&lbytes, &lists, &nodes);
        CHECK_INT(count1, count0);
        CHECK_INT(bytes1, bytes0);
        CHECK_INT(nodes, nodes0 + 201);
        CHECK(lbytes < 200 * 512);
        say("    200 nodes of 1440 x 2560 recorded and drawn: %u bytes of lists, no bitmap (a bitmap each was %u MB in all)", (unsigned)lbytes,
            (unsigned)(200ull * 1440 * 2560 * 4 >> 20));
        CHECK_INT(call("rnBytes", 1, J(nodes200[7])).j > 0, 1);
        for (i = 0; i < 200; i++) call("rnFree", 1, J(nodes200[i]));
        call("rnFree", 1, J(all));
    }
    mg_java_gfx_list_stats(&lbytes, &lists, &nodes);
    CHECK_INT(lists, lists0);
    CHECK_INT(nodes, nodes0);
    call("cvFree", 1, J(ca));
    call("cvFree", 1, J(cb));
    call("bmFree", 1, J(a));
    call("bmFree", 1, J(b));
    call("bmFree", 1, J(pic));
    CHECK(thrown()[0] == 0);
}

/* ---------------------------------------------------------------- inverse fills */

enum { INV_RED = (i32)0xFFFF0000u, INV_GREEN = (i32)0xFF00FF00u, INV_BLUE = (i32)0xFF0000FFu, INV_YELLOW = (i32)0xFFFFFF00u, INV_WHITE = (i32)0xFFFFFFFFu };

static i32 pixel_of(i64 bm, int x, int y) { return call("bmGetPixel", 3, J(bm), I(x), I(y)).i; }

/*
 * A camera's picture framed as VK mp3 mod frames its QR scanner's (BarcodeViewfinderLayout): a picture of four colours, 40
 * by 40 pixels, drawn over the whole of a canvas `w` x `h` (one picture's pixel to one of the canvas's, under whatever
 * matrix the canvas has), and then a rounded rectangle the canvas's size filled in white by the path's fill rule `fill`
 * (Path.meglerFill: 2 is INVERSE_WINDING), which with an inverse rule is the four corners outside it.
 */
static void viewfinder(i64 cv, i64 pic, int w, int h, int fill)
{
    float round[8] = { 8, 0, 0, (float)w, (float)h, 8, 8, 0 };
    call("cvDrawColor", 3, J(cv), I((i32)0xFF000000u), I(3));
    call("cvDrawBitmap", 11, J(cv), J(pic), F(0), F(0), F(40), F(40), F(0), F(0), F((float)w), F((float)h), L(NULL));
    call("cvDrawPath", 6, J(cv), L(floats(round, 8)), I(8), I(fill), L(paint(0xFFFFFFFFu)), J(0));
}

/* The middles of the picture's four quarters are its colours, and the corner pixels white; what they are, when not. */
static bool framed(i64 bm, int w, int h)
{
    static const i32 want[8] = { INV_RED, INV_GREEN, INV_BLUE, INV_YELLOW, INV_WHITE, INV_WHITE, INV_WHITE, INV_WHITE };
    const int at[8][2] = { { w / 4, h / 4 }, { 3 * w / 4, h / 4 }, { w / 4, 3 * h / 4 }, { 3 * w / 4, 3 * h / 4 }, { 0, 0 }, { w - 1, 0 }, { 0, h - 1 }, { w - 1, h - 1 } };
    bool all = true;
    int i;
    for (i = 0; i < 8; i++) {
        i32 got = pixel_of(bm, at[i][0], at[i][1]);
        if (got != want[i]) {
            say("    at %d,%d: %08x, not %08x", at[i][0], at[i][1], (unsigned)got, (unsigned)want[i]);
            all = false;
        }
    }
    return all;
}

/*
 * Android's inverse fill rules (Path.toggleInverseFillType, INVERSE_WINDING, INVERSE_EVEN_ODD), on whatever host is set:
 * filled, a path so is everything within the clip that it does not cover; clipped by, the clip is what it does not cover.
 * The viewfinder above, which on the phone showed white where the camera's picture was when the rule was taken as plain
 * winding: by INVERSE_WINDING its picture shows and only its corners are white, also under a matrix and through a display
 * list; by plain winding the white covers it. Two squares one in the other by INVERSE_EVEN_ODD: the ring between them left
 * alone, the rest filled. A rectangle by INVERSE_WINDING clipped to (the outside kept) and clipped out (the inside kept).
 */
static void test_inverse(const char *on)
{
    static const float squares[12] = { 6, 8, 8, 32, 32, 0, 6, 14, 14, 26, 26, 0 }, box[6] = { 6, 10, 10, 30, 30, 0 }, twice[6] = { 2, 0, 0, 0, 2, 0 };
    i64 pic = call("bmNew", 2, I(40), I(40)).j, a = call("bmNew", 2, I(40), I(40)).j, b = call("bmNew", 2, I(40), I(40)).j;
    i64 ca = call("cvNew", 1, J(a)).j, cb = call("cvNew", 1, J(b)).j, cp = call("cvNew", 1, J(pic)).j, rc, list;
    say("  inverse fill rules, %s", on);
    call("cvDrawRect", 7, J(cp), F(0), F(0), F(20), F(20), L(paint((u32)INV_RED)), J(0));
    call("cvDrawRect", 7, J(cp), F(20), F(0), F(40), F(20), L(paint((u32)INV_GREEN)), J(0));
    call("cvDrawRect", 7, J(cp), F(0), F(20), F(20), F(40), L(paint((u32)INV_BLUE)), J(0));
    call("cvDrawRect", 7, J(cp), F(20), F(20), F(40), F(40), L(paint((u32)INV_YELLOW)), J(0));
    call("cvFree", 1, J(cp));
    viewfinder(ca, pic, 40, 40, 2);
    CHECK(framed(a, 40, 40));
    /* Inside the rounded rectangle along its straight edges, the picture; just outside a rounded corner, white. */
    CHECK(pixel_of(a, 12, 1) == INV_RED && pixel_of(a, 38, 30) == INV_YELLOW && pixel_of(a, 1, 1) == INV_WHITE);
    /* The same rectangle by plain winding is white all over: what the phone showed. */
    viewfinder(cb, pic, 40, 40, 0);
    CHECK(pixel_of(b, 10, 10) == INV_WHITE && pixel_of(b, 30, 30) == INV_WHITE);
    /* Through a display list, drawn on another canvas whose clip is its own: the same pixels as drawn straight. */
    rc = call("cvRecord", 2, I(40), I(40)).j;
    viewfinder(rc, pic, 40, 40, 2);
    list = call("cvEndRecord", 1, J(rc)).j;
    call("cvFree", 1, J(rc));
    call("cvDrawColor", 3, J(cb), I(0), I(3));
    call("cvDrawList", 2, J(cb), J(list));
    CHECK_INT(differ(a, b), 0);
    call("dlFree", 1, J(list));
    /* Under a matrix that doubles everything: a canvas 20 x 20 in the drawing's own coordinates, framed the same way. */
    call("cvDrawColor", 3, J(cb), I(0), I(3));
    call("cvSave", 1, J(cb));
    call("cvMatrix", 3, J(cb), L(floats(twice, 6)), Z(false));
    viewfinder(cb, pic, 20, 20, 2);
    call("cvRestore", 1, J(cb));
    CHECK(framed(b, 40, 40));
    /* INVERSE_EVEN_ODD of two squares one in the other: outside both and inside the inner one filled, the ring left. */
    call("cvDrawColor", 3, J(cb), I(INV_RED), I(3));
    call("cvDrawPath", 6, J(cb), L(floats(squares, 12)), I(12), I(3), L(paint(0xFFFFFFFFu)), J(0));
    CHECK(pixel_of(b, 4, 4) == INV_WHITE && pixel_of(b, 10, 10) == INV_RED && pixel_of(b, 20, 20) == INV_WHITE && pixel_of(b, 36, 20) == INV_WHITE);
    /* Clipped to a rectangle by INVERSE_WINDING, what is drawn lands outside it; clipped out of it, inside it. */
    call("cvDrawColor", 3, J(cb), I(INV_RED), I(3));
    call("cvSave", 1, J(cb));
    call("cvClipPath", 5, J(cb), L(floats(box, 6)), I(6), I(2), I(1));
    call("cvDrawColor", 3, J(cb), I(INV_BLUE), I(3));
    call("cvRestore", 1, J(cb));
    CHECK(pixel_of(b, 5, 5) == INV_BLUE && pixel_of(b, 20, 20) == INV_RED);
    call("cvSave", 1, J(cb));
    call("cvClipPath", 5, J(cb), L(floats(box, 6)), I(6), I(2), I(0));
    call("cvDrawColor", 3, J(cb), I(INV_GREEN), I(3));
    call("cvRestore", 1, J(cb));
    CHECK(pixel_of(b, 5, 5) == INV_BLUE && pixel_of(b, 20, 20) == INV_GREEN);
    /* Inside an inverse path is outside the path. */
    CHECK(!call("pathContains", 5, L(floats(box, 6)), I(6), I(2), F(20), F(20)).z && call("pathContains", 5, L(floats(box, 6)), I(6), I(2), F(5), F(5)).z);
    call("cvFree", 1, J(ca));
    call("cvFree", 1, J(cb));
    call("bmFree", 1, J(a));
    call("bmFree", 1, J(b));
    call("bmFree", 1, J(pic));
    CHECK(thrown()[0] == 0);
}

#ifdef MG_GFX_WINDOWS
/* mg_gfx_windows.cpp: the shell's own megler_gfx_host (app/Megler/player/GfxHost.cpp), in the Windows build of the tests. */
const megler_gfx_host *mg_gfx_windows_host(void (*line)(const char *text, void *user), void *user, int way);

static void shell_line(const char *text, void *user)
{
    (void)user;
    say("    (shell) %s", text);
}

/*
 * The same on the shell's own drawing (Direct2D on this machine's graphics processor): every part of a Paint drawn both
 * ways gives the same pixels, and so does Spotify's player button, in a face of this machine's (Windows' fonts), its glyph
 * dark on the white disc all three ways. (On the phone that glyph was missing; what this shows is that neither a list nor
 * a node loses it.)
 */
static void test_lists_on_shell(void)
{
    static const char *const fonts[] = { "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf" };
    const megler_gfx_host *shell = mg_gfx_windows_host(shell_line, NULL, 0);
    i64 face = 0;
    int ink = 0;
    size_t i;
    say("  display lists on the shell's own drawing (Direct2D): a grid of paints, and Spotify's player button");
    mg_java_gfx_set_host(shell);
    CHECK_INT(paint_cases_differ(), 0);
    for (i = 0; !face && i < MG_COUNT(fonts); i++) {
        size_t size = 0;
        u8 *data = mg_file_read_all(fonts[i], &size);
        if (!data) continue;
        face = call("tfFromData", 1, L(bytes(data, (int)size))).j;
        free(data);
    }
    if (!face) say("    (no font file of Windows' could be read: the button's glyph is in the default face)");
    CHECK_INT(player_button_differ(face, &ink), 0);
    say("    the button's glyph: %d dark pixels, the least of the three ways", ink);
    CHECK(ink > 400);
    /* And the inverse fill rules, as the phone's Direct2D draws them: VK mp3 mod's scanner frame over its camera's picture. */
    test_inverse("on the shell's own drawing (Direct2D)");
    mg_java_gfx_set_host(mg_gfx_standin());
}
#endif

int mg_gfx_natives_tests_run(mg_test_sink sink, void *user, int *checks)
{
    uint64_t bytes0 = 0, bytes1 = 1;
    uint32_t count0 = 0, count1 = 1;
    int n = 0;
    const mg_jhle *e;
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    if (!mg_dvm_running()) {
        say("megler.Gfx's native methods: not tested, the Dalvik runtime is not running");
        return 0;
    }
    say("megler.Gfx's native methods, over the stand-in");
    /* One entry for each native method megler/Gfx.java declares. */
    for (e = mg_java_gfx_natives(); e->cls; e++) n += strcmp(e->cls, "megler/Gfx") == 0 && e->sig && e->fn;
    CHECK_INT(n, 72);
    mg_java_gfx_set_host(mg_gfx_standin());
    mg_java_gfx_stats(&bytes0, &count0);
    test_bitmaps();
    test_webp();
    test_canvas();
    test_canvas_raster();
    test_drawing();
    test_paint();
    test_text();
    test_shaders();
    test_paths();
    test_lists();
    test_inverse("over the stand-in");
#ifdef MG_GFX_WINDOWS
    test_lists_on_shell();
#endif
    test_protocol();
    /* Every bitmap the tests made is freed again. */
    mg_java_gfx_stats(&bytes1, &count1);
    CHECK_INT(bytes1, bytes0);
    CHECK_INT(count1, count0);
    mg_java_gfx_set_host(NULL);
    mg_java_gfx_set_present(NULL, NULL);
    if (checks) *checks += t_checks;
    return t_failed;
}
