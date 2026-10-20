/*
 * The stand-in for the shell's drawing, tested through meglet_gfx_host alone.
 *
 * These are also the simple cases of meglet.h's contract written out as
 * checks: which pixels a shape covers, what a matrix and a clip do to it,
 * what a colour laid over another comes to, where text sits on its baseline,
 * what an image file decodes to. A real shell smooths edges and draws
 * letters, so it would differ from the stand-in at the edges and in every
 * number about text; in the middle of a shape and for whole pixels it should
 * agree to within one step of a colour.
 */
#include <math.h>
#include <stdio.h>

#include "zlib.h"

#include "../src/mg_base.h"
#include "../src/host/mg_host.h"
#include "mg_gfx_standin.h"
#include "mg_gfx_tests.h"

static int t_checks, t_failed;
static mg_test_sink t_sink;
static void *t_sink_user;
static char *t_scratch;
static const meglet_gfx_host *H;

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
        if (!(fabs(got_ - want_) <= 1e-4)) {                                                         \
            t_failed++;                                                                              \
            say("  FAIL %s:%d: %s = %g, expected %g", __FILE__, __LINE__, #expr, got_, want_);       \
        }                                                                                            \
    } while (0)

/* ------------------------------------------------------- a bitmap to draw in */

typedef struct tbitmap {
    u32 *px;                           /* a pixel is the word 0xAARRGGBB, premultiplied */
    int w, h;
    void *cv;
} tbitmap;

static void bitmap_make(tbitmap *b, int w, int h)
{
    b->w = w;
    b->h = h;
    b->px = (u32 *)mg_alloc(sizeof(u32) * (size_t)w * (size_t)h);
    b->cv = H->canvas_new(H->user, b->px, w, h, w * 4);
}

static void bitmap_done(tbitmap *b)
{
    if (b->cv) H->canvas_free(H->user, b->cv);
    free(b->px);
}

static u32 at(const tbitmap *b, int x, int y) { return b->px[y * b->w + x]; }

/* How many pixels are not transparent, and how many hold exactly this. */
static int covered(const tbitmap *b)
{
    int i, n = 0;
    for (i = 0; i < b->w * b->h; i++) n += b->px[i] != 0;
    return n;
}

static int count_of(const tbitmap *b, u32 value)
{
    int i, n = 0;
    for (i = 0; i < b->w * b->h; i++) n += b->px[i] == value;
    return n;
}

/* The pixels that are not transparent are exactly the rectangle x0..x1 by y0..y1 (x1 and y1 not in it). */
static bool exactly(const tbitmap *b, int x0, int y0, int x1, int y1)
{
    int x, y;
    for (y = 0; y < b->h; y++)
        for (x = 0; x < b->w; x++)
            if ((at(b, x, y) != 0) != (x >= x0 && x < x1 && y >= y0 && y < y1)) return false;
    return true;
}

static void erase(tbitmap *b) { memset(b->px, 0, sizeof(u32) * (size_t)b->w * (size_t)b->h); }

static const float k_identity[6] = { 1, 0, 0, 0, 1, 0 };

static meglet_gfx_paint paint_of(u32 color)
{
    meglet_gfx_paint p;
    memset(&p, 0, sizeof(p));
    p.color = color;
    p.miter_limit = 4;
    p.text_size = 12;
    p.text_scale_x = 1;
    p.blend = MEGLET_GFX_BLEND_SRC_OVER;
    p.filter_mode = -1;
    return p;
}

/* What the contract's table says of SRC_OVER, in 255ths rounded to the nearest, on premultiplied words. */
static u32 mul255(u32 a, u32 b) { return (a * b + 127) / 255; }
static u32 premultiplied(u32 c)
{
    u32 a = c >> 24;
    return a << 24 | mul255((c >> 16) & 255, a) << 16 | mul255((c >> 8) & 255, a) << 8 | mul255(c & 255, a);
}
static u32 over(u32 s, u32 d)
{
    u32 k = 255 - (s >> 24), out = 0;
    int shift;
    for (shift = 0; shift < 32; shift += 8) out |= (((s >> shift) & 255) + mul255((d >> shift) & 255, k)) << shift;
    return out;
}
static u32 faded(u32 p, u32 alpha)
{
    u32 out = 0;
    int shift;
    for (shift = 0; shift < 32; shift += 8) out |= mul255((p >> shift) & 255, alpha) << shift;
    return out;
}

/* ----------------------------------------------------------------- shapes */

static void test_fills(void)
{
    tbitmap b;
    meglet_gfx_paint red = paint_of(0xFFFF0000u), half = paint_of(0x80FF0000u);
    say("  rectangles, and one colour over another");
    bitmap_make(&b, 16, 16);
    CHECK(b.cv != NULL);
    H->draw_rect(H->user, b.cv, 2, 3, 6, 8, &red, NULL);
    CHECK(exactly(&b, 2, 3, 6, 8));
    CHECK_HEX(at(&b, 2, 3), 0xFFFF0000u);
    CHECK_HEX(at(&b, 5, 7), 0xFFFF0000u);
    /* A pixel is drawn when its centre is inside: a centre on the left or top edge is, one on the right or bottom edge is not. */
    erase(&b);
    H->draw_rect(H->user, b.cv, 2.5f, 2.5f, 4.5f, 4.5f, &red, NULL);
    CHECK(exactly(&b, 2, 2, 4, 4));
    erase(&b);
    H->draw_rect(H->user, b.cv, 2.4f, 2.6f, 4.6f, 4.4f, &red, NULL);
    CHECK(exactly(&b, 2, 3, 5, 4));
    /* What is outside the bitmap is not drawn and does no harm. */
    erase(&b);
    H->draw_rect(H->user, b.cv, -5, -5, 3, 2, &red, NULL);
    H->draw_rect(H->user, b.cv, 14, 15, 99, 99, &red, NULL);
    CHECK_INT(covered(&b), 3 * 2 + 2 * 1);
    /* A colour is not premultiplied; a pixel is. Half-transparent red on nothing: */
    erase(&b);
    H->draw_rect(H->user, b.cv, 0, 0, 4, 4, &half, NULL);
    CHECK_HEX(at(&b, 1, 1), 0x80800000u);
    /* ... on itself: */
    H->draw_rect(H->user, b.cv, 0, 0, 2, 4, &half, NULL);
    CHECK_HEX(at(&b, 1, 1), 0xC0C00000u);
    CHECK_HEX(at(&b, 1, 1), over(premultiplied(0x80FF0000u), 0x80800000u));
    CHECK_HEX(at(&b, 3, 1), 0x80800000u);
    /* ... and on opaque white, which stays opaque: */
    H->draw_color(H->user, b.cv, 0xFFFFFFFFu, MEGLET_GFX_BLEND_SRC);
    CHECK_INT(count_of(&b, 0xFFFFFFFFu), 256);
    H->draw_rect(H->user, b.cv, 0, 0, 4, 4, &half, NULL);
    CHECK_HEX(at(&b, 1, 1), 0xFFFF7F7Fu);
    CHECK_HEX(at(&b, 4, 4), 0xFFFFFFFFu);
    /* drawColor: SRC replaces, alpha and all; SRC_OVER lays over; CLEAR empties. */
    H->draw_color(H->user, b.cv, 0x40FF0000u, MEGLET_GFX_BLEND_SRC);
    CHECK_INT(count_of(&b, 0x40400000u), 256);
    H->draw_color(H->user, b.cv, 0x8000FF00u, MEGLET_GFX_BLEND_SRC_OVER);
    CHECK_HEX(at(&b, 9, 9), over(0x80008000u, 0x40400000u));
    CHECK_HEX(at(&b, 9, 9), 0xA0208000u);
    H->draw_color(H->user, b.cv, 0xFF123456u, MEGLET_GFX_BLEND_CLEAR);
    CHECK_INT(covered(&b), 0);
    /* The other modes work inside the shape only: SRC puts the colour there whatever was, DST_OUT takes away by the colour's alpha. */
    H->draw_color(H->user, b.cv, 0xFF0000FFu, MEGLET_GFX_BLEND_SRC);
    half.blend = MEGLET_GFX_BLEND_SRC;
    H->draw_rect(H->user, b.cv, 0, 0, 2, 2, &half, NULL);
    CHECK_HEX(at(&b, 1, 1), 0x80800000u);
    CHECK_HEX(at(&b, 2, 2), 0xFF0000FFu);
    half.blend = MEGLET_GFX_BLEND_DST_OUT;
    H->draw_rect(H->user, b.cv, 4, 4, 6, 6, &half, NULL);
    CHECK_HEX(at(&b, 5, 5), 0x7F00007Fu);
    red.blend = MEGLET_GFX_BLEND_CLEAR;
    H->draw_rect(H->user, b.cv, 8, 8, 10, 10, &red, NULL);
    CHECK_HEX(at(&b, 9, 9), 0u);
    CHECK_HEX(at(&b, 10, 10), 0xFF0000FFu);
    /* flush and touched may be called at any time. */
    H->flush(H->user, b.cv);
    H->touched(H->user, b.cv);
    bitmap_done(&b);
}

static void test_matrix_and_clip(void)
{
    tbitmap b;
    meglet_gfx_paint red = paint_of(0xFFFF0000u), blue = paint_of(0xFF0000FFu);
    float m[6] = { 2, 0, 3, 0, 2, 1 }, turn[6] = { 0, -1, 10, 1, 0, 0 };
    say("  the matrix, the clip, save and restore");
    bitmap_make(&b, 16, 16);
    /* x' = 2x + 3, y' = 2y + 1 */
    H->set_matrix(H->user, b.cv, m);
    H->draw_rect(H->user, b.cv, 1, 1, 3, 2, &red, NULL);
    CHECK(exactly(&b, 5, 3, 9, 5));
    /* A quarter turn: x' = 10 - y, y' = x. The rectangle 0..4 by 0..2 stands on its end. */
    erase(&b);
    H->set_matrix(H->user, b.cv, turn);
    H->draw_rect(H->user, b.cv, 0, 0, 4, 2, &red, NULL);
    CHECK(exactly(&b, 8, 0, 10, 4));
    /* A clip is in the matrix of its moment and stays where it is when the matrix changes. */
    erase(&b);
    H->set_matrix(H->user, b.cv, m);
    H->save(H->user, b.cv);
    H->clip_rect(H->user, b.cv, 0, 0, 3, 3, MEGLET_GFX_CLIP_INTERSECT);
    H->set_matrix(H->user, b.cv, k_identity);
    H->draw_color(H->user, b.cv, 0xFFFF0000u, MEGLET_GFX_BLEND_SRC_OVER);
    CHECK(exactly(&b, 3, 1, 9, 7));
    /* A second clip can only take away. */
    H->clip_rect(H->user, b.cv, 0, 0, 5, 16, MEGLET_GFX_CLIP_INTERSECT);
    H->draw_color(H->user, b.cv, 0xFF0000FFu, MEGLET_GFX_BLEND_SRC);
    CHECK_INT(count_of(&b, 0xFF0000FFu), 2 * 6);
    CHECK_INT(count_of(&b, 0xFFFF0000u), 4 * 6);
    CHECK_HEX(at(&b, 4, 3), 0xFF0000FFu);
    CHECK_HEX(at(&b, 5, 3), 0xFFFF0000u);
    /* restore brings the clip of the save back; the matrix is whatever was set last. */
    H->restore(H->user, b.cv);
    H->draw_rect(H->user, b.cv, 0, 0, 16, 16, &blue, NULL);
    CHECK_INT(count_of(&b, 0xFF0000FFu), 256);
    /* A rectangle cut out of the clip. */
    H->save(H->user, b.cv);
    H->clip_rect(H->user, b.cv, 4, 4, 12, 12, MEGLET_GFX_CLIP_INTERSECT);
    H->clip_rect(H->user, b.cv, 6, 6, 8, 8, MEGLET_GFX_CLIP_DIFFERENCE);
    H->draw_color(H->user, b.cv, 0xFFFF0000u, MEGLET_GFX_BLEND_SRC);
    CHECK_INT(count_of(&b, 0xFFFF0000u), 64 - 4);
    CHECK_HEX(at(&b, 7, 7), 0xFF0000FFu);
    CHECK_HEX(at(&b, 8, 8), 0xFFFF0000u);
    H->draw_rect(H->user, b.cv, 0, 0, 16, 16, &blue, NULL);
    CHECK_INT(count_of(&b, 0xFF0000FFu), 256);
    H->restore(H->user, b.cv);
    /* Saves nest. */
    erase(&b);
    H->save(H->user, b.cv);
    H->clip_rect(H->user, b.cv, 0, 0, 8, 8, MEGLET_GFX_CLIP_INTERSECT);
    H->save(H->user, b.cv);
    H->clip_rect(H->user, b.cv, 0, 0, 2, 2, MEGLET_GFX_CLIP_INTERSECT);
    H->restore(H->user, b.cv);
    H->draw_color(H->user, b.cv, 0xFFFF0000u, MEGLET_GFX_BLEND_SRC);
    CHECK(exactly(&b, 0, 0, 8, 8));
    H->restore(H->user, b.cv);
    H->draw_color(H->user, b.cv, 0xFFFF0000u, MEGLET_GFX_BLEND_SRC);
    CHECK_INT(covered(&b), 256);
    /* A layer with an alpha: what is drawn in it comes out that much fainter, and its bounds clip. */
    erase(&b);
    {
        float bounds[4] = { 0, 0, 8, 16 };
        H->save_layer(H->user, b.cv, bounds, 128, MEGLET_GFX_BLEND_SRC_OVER);
    }
    H->draw_rect(H->user, b.cv, 0, 0, 16, 16, &red, NULL);
    H->restore(H->user, b.cv);
    CHECK(exactly(&b, 0, 0, 8, 16));
    CHECK_HEX(at(&b, 3, 3), 0x80800000u);
    H->draw_rect(H->user, b.cv, 0, 0, 16, 16, &red, NULL);
    CHECK_INT(count_of(&b, 0xFFFF0000u), 256);
    /* The stand-in clips to a path's box. */
    erase(&b);
    {
        float tri[] = { MEGLET_GFX_PATH_MOVE, 2, 2, MEGLET_GFX_PATH_LINE, 10, 2, MEGLET_GFX_PATH_LINE, 2, 6, MEGLET_GFX_PATH_CLOSE };
        H->save(H->user, b.cv);
        H->clip_path(H->user, b.cv, tri, (int)MG_COUNT(tri), MEGLET_GFX_WINDING, MEGLET_GFX_CLIP_INTERSECT);
        H->draw_color(H->user, b.cv, 0xFFFF0000u, MEGLET_GFX_BLEND_SRC);
        H->restore(H->user, b.cv);
    }
    CHECK(exactly(&b, 2, 2, 10, 6));
    bitmap_done(&b);
}

static void test_shapes(void)
{
    tbitmap b;
    meglet_gfx_paint red = paint_of(0xFFFF0000u);
    int x, y, n;
    say("  round rectangles, ovals, paths, strokes");
    bitmap_make(&b, 20, 20);
    /* A round rectangle: its corners' pixels are outside, the middles of its sides and its centre inside. */
    H->draw_round_rect(H->user, b.cv, 0, 0, 20, 20, 8, 8, &red, NULL);
    CHECK_HEX(at(&b, 0, 0), 0u);
    CHECK_HEX(at(&b, 1, 1), 0u);
    CHECK_HEX(at(&b, 19, 0), 0u);
    CHECK_HEX(at(&b, 0, 19), 0u);
    CHECK_HEX(at(&b, 19, 19), 0u);
    CHECK_HEX(at(&b, 2, 2), 0xFFFF0000u);
    CHECK_HEX(at(&b, 10, 10), 0xFFFF0000u);
    CHECK_HEX(at(&b, 0, 10), 0xFFFF0000u);
    CHECK_HEX(at(&b, 10, 0), 0xFFFF0000u);
    CHECK_HEX(at(&b, 19, 10), 0xFFFF0000u);
    CHECK_HEX(at(&b, 10, 19), 0xFFFF0000u);
    /* It is the same in all four corners, and what is missing of the square is what four quarter circles leave: 400 - 8 * 8 * (4 - pi) is 345. */
    n = 0;
    for (y = 0; y < 20; y++)
        for (x = 0; x < 20; x++) {
            n += at(&b, x, y) != at(&b, 19 - x, y);
            n += at(&b, x, y) != at(&b, x, 19 - y);
        }
    CHECK_INT(n, 0);
    CHECK(covered(&b) >= 338 && covered(&b) <= 352);
    /* An oval touches the middles of its rectangle's sides and leaves the corners. */
    erase(&b);
    H->draw_oval(H->user, b.cv, 0, 4, 20, 14, &red, NULL);
    CHECK_HEX(at(&b, 10, 9), 0xFFFF0000u);
    CHECK_HEX(at(&b, 0, 9), 0xFFFF0000u);
    CHECK_HEX(at(&b, 19, 9), 0xFFFF0000u);
    CHECK_HEX(at(&b, 10, 4), 0xFFFF0000u);
    CHECK_HEX(at(&b, 10, 13), 0xFFFF0000u);
    CHECK_HEX(at(&b, 0, 4), 0u);
    CHECK_HEX(at(&b, 19, 13), 0u);
    CHECK_HEX(at(&b, 10, 3), 0u);
    CHECK_HEX(at(&b, 10, 14), 0u);
    /* pi * 10 * 5 is 157. */
    CHECK(covered(&b) >= 150 && covered(&b) <= 164);
    /* A path: a triangle. A pixel is in when its centre is below the slanted side. */
    erase(&b);
    {
        float tri[] = { MEGLET_GFX_PATH_MOVE, 0, 0, MEGLET_GFX_PATH_LINE, 10, 0, MEGLET_GFX_PATH_LINE, 0, 10, MEGLET_GFX_PATH_CLOSE };
        H->draw_path(H->user, b.cv, tri, (int)MG_COUNT(tri), MEGLET_GFX_WINDING, &red, NULL);
    }
    CHECK_HEX(at(&b, 1, 1), 0xFFFF0000u);
    CHECK_HEX(at(&b, 4, 4), 0xFFFF0000u);
    CHECK_HEX(at(&b, 5, 5), 0u);
    CHECK_HEX(at(&b, 8, 0), 0xFFFF0000u);
    CHECK_HEX(at(&b, 0, 8), 0xFFFF0000u);
    CHECK_INT(covered(&b), 45);
    /* The two fill rules: a square inside a square, both drawn the same way round. Winding fills both; even-odd leaves the inner one out. */
    {
        float two[] = { MEGLET_GFX_PATH_MOVE, 0, 0, MEGLET_GFX_PATH_LINE, 10, 0, MEGLET_GFX_PATH_LINE, 10, 10, MEGLET_GFX_PATH_LINE, 0, 10, MEGLET_GFX_PATH_CLOSE,
                        MEGLET_GFX_PATH_MOVE, 3, 3, MEGLET_GFX_PATH_LINE, 7, 3, MEGLET_GFX_PATH_LINE, 7, 7, MEGLET_GFX_PATH_LINE, 3, 7, MEGLET_GFX_PATH_CLOSE };
        erase(&b);
        H->draw_path(H->user, b.cv, two, (int)MG_COUNT(two), MEGLET_GFX_WINDING, &red, NULL);
        CHECK_INT(covered(&b), 100);
        erase(&b);
        H->draw_path(H->user, b.cv, two, (int)MG_COUNT(two), MEGLET_GFX_EVEN_ODD, &red, NULL);
        CHECK_INT(covered(&b), 100 - 16);
        CHECK_HEX(at(&b, 5, 5), 0u);
        CHECK_HEX(at(&b, 2, 5), 0xFFFF0000u);
    }
    /* Curves: a quadratic from (0, 10) to (20, 10) pulled up to (10, -10) peaks at (10, 0); with the line back it is a dome. */
    erase(&b);
    {
        float dome[] = { MEGLET_GFX_PATH_MOVE, 0, 10, MEGLET_GFX_PATH_QUAD, 10, -10, 20, 10, MEGLET_GFX_PATH_CLOSE };
        H->draw_path(H->user, b.cv, dome, (int)MG_COUNT(dome), MEGLET_GFX_WINDING, &red, NULL);
    }
    CHECK_HEX(at(&b, 10, 5), 0xFFFF0000u);
    CHECK_HEX(at(&b, 10, 1), 0xFFFF0000u);
    CHECK_HEX(at(&b, 2, 2), 0u);
    CHECK_HEX(at(&b, 17, 2), 0u);
    CHECK_HEX(at(&b, 10, 10), 0u);
    /* The same dome as a cubic (the quadratic's control point two thirds of the way from each end). */
    {
        tbitmap c;
        float dome[] = { MEGLET_GFX_PATH_MOVE, 0, 10, MEGLET_GFX_PATH_CUBIC, 20.0f / 3, -10.0f / 3, 40.0f / 3, -10.0f / 3, 20, 10, MEGLET_GFX_PATH_CLOSE };
        bitmap_make(&c, 20, 20);
        H->draw_path(H->user, c.cv, dome, (int)MG_COUNT(dome), MEGLET_GFX_WINDING, &red, NULL);
        n = 0;
        for (x = 0; x < 400; x++) n += b.px[x] != c.px[x];
        CHECK(n <= 4);
        bitmap_done(&c);
    }
    /* A stroke is centred on the outline: a square 5..15 stroked 2 wide is a frame from 4 to 16 with a hole from 6 to 14. */
    erase(&b);
    red.style = MEGLET_GFX_STROKE;
    red.stroke_width = 2;
    H->draw_rect(H->user, b.cv, 5, 5, 15, 15, &red, NULL);
    CHECK_INT(covered(&b), 12 * 12 - 8 * 8);
    CHECK_HEX(at(&b, 4, 4), 0xFFFF0000u);
    CHECK_HEX(at(&b, 15, 15), 0xFFFF0000u);
    CHECK_HEX(at(&b, 3, 10), 0u);
    CHECK_HEX(at(&b, 4, 10), 0xFFFF0000u);
    CHECK_HEX(at(&b, 5, 10), 0xFFFF0000u);
    CHECK_HEX(at(&b, 6, 10), 0u);
    CHECK_HEX(at(&b, 16, 10), 0u);
    /* Filled and stroked: the frame and what is in it, once. */
    erase(&b);
    red.style = MEGLET_GFX_FILL_AND_STROKE;
    red.color = 0x80FF0000u;
    H->draw_rect(H->user, b.cv, 5, 5, 15, 15, &red, NULL);
    CHECK_INT(count_of(&b, 0x80800000u), 12 * 12);
    CHECK_INT(covered(&b), 12 * 12);
    /* A line is a path of two points, stroked: one pixel wide along y = 10.5 it is row 10. */
    erase(&b);
    red = paint_of(0xFFFF0000u);
    red.style = MEGLET_GFX_STROKE;
    red.stroke_width = 1;
    {
        float line[] = { MEGLET_GFX_PATH_MOVE, 2, 10.5f, MEGLET_GFX_PATH_LINE, 12, 10.5f };
        H->draw_path(H->user, b.cv, line, (int)MG_COUNT(line), MEGLET_GFX_WINDING, &red, NULL);
        CHECK(exactly(&b, 2, 10, 12, 11));
        /* A square cap adds half the width at each end; a butt cap adds nothing. */
        erase(&b);
        red.cap = MEGLET_GFX_CAP_SQUARE;
        red.stroke_width = 2;
        H->draw_path(H->user, b.cv, line, (int)MG_COUNT(line), MEGLET_GFX_WINDING, &red, NULL);
        CHECK(exactly(&b, 1, 10, 13, 12) || exactly(&b, 1, 9, 13, 11));
        /* Width 0 is a hairline: a pixel wide whatever the matrix. Under a matrix that doubles, the line is twice as long and no thicker. */
        erase(&b);
        red.cap = MEGLET_GFX_CAP_BUTT;
        red.stroke_width = 0;
        {
            float twice[6] = { 2, 0, 0, 0, 2, 0 }, hair[] = { MEGLET_GFX_PATH_MOVE, 1, 3.25f, MEGLET_GFX_PATH_LINE, 6, 3.25f };
            H->set_matrix(H->user, b.cv, twice);
            H->draw_path(H->user, b.cv, hair, (int)MG_COUNT(hair), MEGLET_GFX_WINDING, &red, NULL);
            CHECK(exactly(&b, 2, 6, 12, 7));
            /* A width that is not 0 goes through the matrix: 1 becomes 2. */
            erase(&b);
            red.stroke_width = 1;
            H->draw_path(H->user, b.cv, hair, (int)MG_COUNT(hair), MEGLET_GFX_WINDING, &red, NULL);
            CHECK(exactly(&b, 2, 5, 12, 8) || exactly(&b, 2, 6, 12, 8) || exactly(&b, 2, 5, 12, 7));
            CHECK_INT(covered(&b), 20);
            H->set_matrix(H->user, b.cv, k_identity);
        }
    }
    bitmap_done(&b);
}

/* ---------------------------------------------------------------- bitmaps */

static void image_of(const tbitmap *b, meglet_gfx_image *out)
{
    out->pixels = b->px;
    out->width = b->w;
    out->height = b->h;
    out->stride = b->w * 4;
    out->id = 1;
    out->version = 1;
}

static void test_bitmaps(void)
{
    tbitmap s, d;
    meglet_gfx_image image;
    meglet_gfx_paint p = paint_of(0xFF000000u);
    float whole[4] = { 0, 0, 2, 2 }, all[4] = { 0, 0, 8, 8 }, one[4] = { 1, 0, 2, 1 }, four[4] = { 0, 0, 4, 4 }, same[4] = { 3, 3, 5, 5 };
    void *shader;
    say("  one bitmap drawn into another, shaders");
    bitmap_make(&s, 2, 2);
    bitmap_make(&d, 8, 8);
    s.px[0] = 0xFFFF0000u;
    s.px[1] = 0xFF00FF00u;
    s.px[2] = 0xFF0000FFu;
    s.px[3] = 0x80404040u;
    image_of(&s, &image);
    /* Stretched four times: each pixel becomes a 4 by 4 block. */
    H->draw_bitmap(H->user, d.cv, &image, whole, all, &p);
    CHECK_INT(count_of(&d, 0xFFFF0000u), 16);
    CHECK_INT(count_of(&d, 0xFF00FF00u), 16);
    CHECK_INT(count_of(&d, 0xFF0000FFu), 16);
    CHECK_INT(count_of(&d, 0x80404040u), 16);
    CHECK_HEX(at(&d, 3, 3), 0xFFFF0000u);
    CHECK_HEX(at(&d, 4, 3), 0xFF00FF00u);
    CHECK_HEX(at(&d, 3, 4), 0xFF0000FFu);
    CHECK_HEX(at(&d, 4, 4), 0x80404040u);
    /* A part of it: nothing from outside the part shows. */
    erase(&d);
    H->draw_bitmap(H->user, d.cv, &image, one, four, &p);
    CHECK_INT(count_of(&d, 0xFF00FF00u), 16);
    CHECK(exactly(&d, 0, 0, 4, 4));
    /* Its own size, somewhere: pixel for pixel. */
    erase(&d);
    H->draw_bitmap(H->user, d.cv, &image, whole, same, &p);
    CHECK(exactly(&d, 3, 3, 5, 5));
    CHECK_HEX(at(&d, 4, 4), 0x80404040u);
    /* The paint's alpha multiplies every pixel, and the result is laid over what is there. */
    H->draw_color(H->user, d.cv, 0xFFFFFFFFu, MEGLET_GFX_BLEND_SRC);
    p.color = 0x80000000u;
    H->draw_bitmap(H->user, d.cv, &image, whole, all, &p);
    CHECK_HEX(at(&d, 0, 0), over(faded(0xFFFF0000u, 128), 0xFFFFFFFFu));
    CHECK_HEX(at(&d, 0, 0), 0xFFFF7F7Fu);
    CHECK_HEX(at(&d, 7, 7), over(faded(0x80404040u, 128), 0xFFFFFFFFu));
    /* A colour filter, SRC_IN: the bitmap's shape in the filter's colour. That is how Android tints an icon. */
    erase(&d);
    p = paint_of(0xFF000000u);
    p.filter_mode = MEGLET_GFX_BLEND_SRC_IN;
    p.filter_color = 0xFF112233u;
    H->draw_bitmap(H->user, d.cv, &image, whole, all, &p);
    CHECK_HEX(at(&d, 0, 0), 0xFF112233u);
    CHECK_HEX(at(&d, 7, 0), 0xFF112233u);
    CHECK_HEX(at(&d, 7, 7), premultiplied(0x80112233u));
    /* Through a matrix: twice the size from (1, 1) is the whole of an 8 by 8 less a border. */
    erase(&d);
    p = paint_of(0xFF000000u);
    {
        float m[6] = { 2, 0, 2, 0, 2, 2 }, small[4] = { 0, 0, 2, 2 };
        H->set_matrix(H->user, d.cv, m);
        H->draw_bitmap(H->user, d.cv, &image, whole, small, &p);
        H->set_matrix(H->user, d.cv, k_identity);
    }
    CHECK(exactly(&d, 2, 2, 6, 6));
    CHECK_HEX(at(&d, 2, 2), 0xFFFF0000u);
    CHECK_HEX(at(&d, 5, 5), 0x80404040u);
    /* The stand-in's shaders: a gradient is its first colour, a bitmap its top left pixel; the paint's alpha multiplies them. */
    erase(&d);
    {
        u32 colors[2] = { 0xFF0000FFu, 0xFFFF0000u };
        float positions[2] = { 0, 1 };
        shader = H->shader_linear(H->user, 0, 0, 8, 0, colors, positions, 2, MEGLET_GFX_TILE_CLAMP);
    }
    CHECK(shader != NULL);
    p = paint_of(0x80FFFFFFu);
    H->shader_matrix(H->user, shader, k_identity);
    H->draw_rect(H->user, d.cv, 0, 0, 2, 2, &p, shader);
    CHECK_HEX(at(&d, 1, 1), 0x80000080u);
    H->shader_free(H->user, shader);
    shader = H->shader_bitmap(H->user, &image, MEGLET_GFX_TILE_REPEAT);
    CHECK(shader != NULL);
    p = paint_of(0xFF000000u);
    H->draw_rect(H->user, d.cv, 4, 4, 6, 6, &p, shader);
    CHECK_HEX(at(&d, 5, 5), 0xFFFF0000u);
    H->shader_free(H->user, shader);
    bitmap_done(&s);
    bitmap_done(&d);
}

/* ------------------------------------------------------------------- text */

static void test_text(void)
{
    static const u16 a_b[] = { 'A', ' ', 'B' }, aa[] = { 'A', 'A' }, words[] = { 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', ' ', 'f', 'o', 'o' },
                     longword[] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j' }, lines[] = { 'a', 'b', '\n', 'c', 'd' }, blank[] = { ' ', ' ' };
    tbitmap b;
    meglet_gfx_paint p = paint_of(0xFF0000FFu);
    float m[5] = { 9, 9, 9, 9, 9 }, box[4] = { 9, 9, 9, 9 }, widths[15];
    void *face;
    int i;
    say("  text: the stand-in's boxes, and its answers about them");
    bitmap_make(&b, 64, 40);
    /* At size 20 a character advances 12 and is a box 10 wide and 14 high standing on the baseline. */
    p.text_size = 20;
    H->draw_text(H->user, b.cv, a_b, 3, 10, 30, &p, NULL, NULL);
    CHECK_INT(covered(&b), 2 * 10 * 14);
    CHECK_HEX(at(&b, 10, 16), 0xFF0000FFu);
    CHECK_HEX(at(&b, 19, 29), 0xFF0000FFu);
    CHECK_HEX(at(&b, 9, 20), 0u);
    CHECK_HEX(at(&b, 20, 20), 0u);
    CHECK_HEX(at(&b, 10, 15), 0u);
    CHECK_HEX(at(&b, 10, 30), 0u);
    CHECK_HEX(at(&b, 25, 20), 0u);
    CHECK_HEX(at(&b, 34, 20), 0xFF0000FFu);
    CHECK_HEX(at(&b, 43, 16), 0xFF0000FFu);
    CHECK_HEX(at(&b, 44, 20), 0u);
    /* x is the run's middle or its end when the paint says so. */
    erase(&b);
    p.text_align = MEGLET_GFX_ALIGN_CENTER;
    H->draw_text(H->user, b.cv, aa, 2, 40, 30, &p, NULL, NULL);
    CHECK_HEX(at(&b, 27, 20), 0u);
    CHECK_HEX(at(&b, 28, 20), 0xFF0000FFu);
    CHECK_HEX(at(&b, 38, 20), 0u);
    CHECK_HEX(at(&b, 40, 20), 0xFF0000FFu);
    CHECK_HEX(at(&b, 49, 20), 0xFF0000FFu);
    CHECK_HEX(at(&b, 50, 20), 0u);
    erase(&b);
    p.text_align = MEGLET_GFX_ALIGN_RIGHT;
    H->draw_text(H->user, b.cv, aa, 2, 40, 30, &p, NULL, NULL);
    CHECK_HEX(at(&b, 15, 20), 0u);
    CHECK_HEX(at(&b, 16, 20), 0xFF0000FFu);
    CHECK_HEX(at(&b, 37, 20), 0xFF0000FFu);
    CHECK_HEX(at(&b, 38, 20), 0u);
    p.text_align = MEGLET_GFX_ALIGN_LEFT;
    /* Measuring agrees with drawing. */
    CHECK_FLOAT(H->text_measure(H->user, a_b, 3, &p, NULL), 36);
    CHECK_FLOAT(H->text_measure(H->user, a_b, 0, &p, NULL), 0);
    /* The face's measures, y down from the baseline: top and ascent negative, descent and bottom positive, then the leading. */
    CHECK_FLOAT(H->text_metrics(H->user, &p, NULL, m), 20);
    CHECK_FLOAT(m[0], -16);
    CHECK_FLOAT(m[1], -16);
    CHECK_FLOAT(m[2], 4);
    CHECK_FLOAT(m[3], 4);
    CHECK_FLOAT(m[4], 0);
    CHECK(m[0] <= m[1] && m[1] < 0 && m[2] > 0 && m[3] >= m[2] && m[4] >= 0);
    /* The ink's box, from the run's start on the baseline: here two boxes with a space between. */
    H->text_bounds(H->user, a_b, 3, &p, NULL, box);
    CHECK_FLOAT(box[0], 0);
    CHECK_FLOAT(box[1], -14);
    CHECK_FLOAT(box[2], 34);
    CHECK_FLOAT(box[3], 0);
    H->text_bounds(H->user, blank, 2, &p, NULL, box);
    CHECK(box[0] == 0 && box[1] == 0 && box[2] == 0 && box[3] == 0);
    /* Each code unit's advance; they add up to the measure. */
    H->text_widths(H->user, words, 15, &p, NULL, widths);
    for (i = 0; i < 15; i++) CHECK_FLOAT(widths[i], 12);
    /* Breaking, at size 10 (6 a character). By characters: as many as fit. */
    p.text_size = 10;
    CHECK_INT(H->text_break(H->user, words, 15, 50, &p, NULL, false), 8);
    CHECK_INT(H->text_break(H->user, words, 15, 5, &p, NULL, false), 0);
    CHECK_INT(H->text_break(H->user, words, 15, 1000, &p, NULL, false), 15);
    /* By words: "hello " fits in 50 and "hello world" does not. The space the line ends with is counted in and does not need room. */
    CHECK_INT(H->text_break(H->user, words, 15, 50, &p, NULL, true), 6);
    CHECK_INT(H->text_break(H->user, words, 15, 30, &p, NULL, true), 6);
    CHECK_INT(H->text_break(H->user, words, 15, 66, &p, NULL, true), 12);
    CHECK_INT(H->text_break(H->user, words, 15, 1000, &p, NULL, true), 15);
    /* A word too long for the line is broken where the line is full. */
    CHECK_INT(H->text_break(H->user, longword, 10, 20, &p, NULL, true), 3);
    CHECK_INT(H->text_break(H->user, words, 15, 29, &p, NULL, true), 4);
    /* A line feed ends the line and belongs to it. */
    CHECK_INT(H->text_break(H->user, lines, 5, 1000, &p, NULL, true), 3);
    CHECK_INT(H->text_break(H->user, lines, 5, 1000, &p, NULL, false), 3);
    /* The scale and the spacing are in every answer. */
    p.text_scale_x = 2;
    p.letter_spacing = 0.4f;
    CHECK_FLOAT(H->text_measure(H->user, aa, 2, &p, NULL), 40);
    /* Typefaces: any name gives one; bytes that are no font give none. */
    face = H->typeface_create(H->user, "sans-serif", MEGLET_GFX_SANS, 400, false);
    CHECK(face != NULL);
    CHECK(H->typeface_derive(H->user, face, 700, true) != NULL);
    CHECK(H->typeface_derive(H->user, NULL, 700, false) != NULL);
    CHECK(H->typeface_from_data(H->user, "OTTO\0\1\0\0\0\0\0\0\0\0\0\0", 16) != NULL);
    CHECK(H->typeface_from_data(H->user, "\0\1\0\0\0\14\0\200\0\3\0\100", 12) != NULL);
    CHECK(H->typeface_from_data(H->user, "this is not a font", 18) == NULL);
    bitmap_done(&b);
}

/* ------------------------------------------------------------ image files */

typedef struct tbytes {
    u8 *data;
    size_t size;
} tbytes;

static void bytes_add(tbytes *b, const void *data, size_t size)
{
    b->data = (u8 *)mg_realloc(b->data, b->size + size + 1);
    if (size) memcpy(b->data + b->size, data, size);
    b->size += size;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void chunk(tbytes *out, const char *type, const u8 *data, u32 len)
{
    u8 word[4];
    u32 crc = (u32)crc32(0, (const Bytef *)type, 4);
    if (len) crc = (u32)crc32(crc, data, len);
    put32(word, len);
    bytes_add(out, word, 4);
    bytes_add(out, type, 4);
    bytes_add(out, data, len);
    put32(word, crc);
    bytes_add(out, word, 4);
}

/*
 * A PNG file of the rows given as they are stored: each a filter byte and
 * then the row's bytes, filtered by it. `idats`: into how many IDAT chunks
 * the packed picture is cut, which a reader has to put together again.
 */
static void png_make(tbytes *out, u32 w, u32 h, u8 depth, u8 type, const u8 *palette, u32 npalette, const u8 *trns, u32 ntrns, const u8 *rows, size_t nrows,
                     int idats)
{
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    u8 head[13], packed[4096];
    uLongf npacked = sizeof(packed);
    size_t cut;
    memset(out, 0, sizeof(*out));
    bytes_add(out, sig, 8);
    put32(head, w);
    put32(head + 4, h);
    head[8] = depth;
    head[9] = type;
    head[10] = head[11] = head[12] = 0;
    chunk(out, "IHDR", head, 13);
    if (palette) chunk(out, "PLTE", palette, npalette);
    if (trns) chunk(out, "tRNS", trns, ntrns);
    if (compress(packed, &npacked, rows, (uLong)nrows) != Z_OK) npacked = 0;
    cut = idats > 1 ? npacked / 2 : npacked;
    chunk(out, "IDAT", packed, (u32)cut);
    if (cut < npacked) chunk(out, "IDAT", packed + cut, (u32)(npacked - cut));
    chunk(out, "IEND", NULL, 0);
}

/* One row as a PNG stores it under filter `how`: each byte less what the filter guesses from the bytes to its left and above. */
static void png_filter(u8 *out, int how, const u8 *row, const u8 *up, size_t n, size_t bpp)
{
    size_t i;
    out[0] = (u8)how;
    for (i = 0; i < n; i++) {
        int a = i >= bpp ? row[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= bpp ? up[i - bpp] : 0, guess = 0, pa, pb, pc;
        if (how == 1) guess = a;
        else if (how == 2) guess = b;
        else if (how == 3) guess = (a + b) / 2;
        else if (how == 4) {
            pa = abs(b - c);
            pb = abs(a - c);
            pc = abs(a + b - 2 * c);
            guess = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        }
        out[1 + i] = (u8)(row[i] - guess);
    }
}

static bool sink_write(void *sink, const void *data, size_t size)
{
    bytes_add((tbytes *)sink, data, size);
    return true;
}

static void test_images(void)
{
    tbytes png, again;
    u32 px[64];
    int w = 0, h = 0, x, y, i;
    say("  image files: PNG read and written");
    /* Red, green, blue, alpha, 8 bits each; the second row stored as its difference from the first (filter 2). */
    {
        static const u8 rows[] = { 0, 255, 0, 0, 255, 0, 255, 0, 128, 2, 1, 0, 255, 0, 10, 21, 30, 128 };
        png_make(&png, 2, 2, 8, 6, NULL, 0, NULL, 0, rows, sizeof(rows), 1);
        CHECK(H->image_size(H->user, png.data, png.size, &w, &h));
        CHECK_INT(w, 2);
        CHECK_INT(h, 2);
        memset(px, 0x55, sizeof(px));
        CHECK(H->image_decode(H->user, png.data, png.size, px, 2, 2, 8));
        /* In memory a pixel is B, G, R, A, premultiplied: the word 0xAARRGGBB. */
        CHECK_HEX(px[0], 0xFFFF0000u);
        CHECK_HEX(px[1], 0x80008000u);
        CHECK_HEX(px[2], 0xFF0000FFu);
        CHECK_HEX(px[3], 0x000000u);
        CHECK_INT(((const u8 *)px)[0], 0);
        CHECK_INT(((const u8 *)px)[2], 255);
        CHECK_INT(((const u8 *)px)[3], 255);
        /* Half the size, as inSampleSize 2 asks: one pixel, one of the four. */
        CHECK(H->image_decode(H->user, png.data, png.size, px, 1, 1, 4));
        CHECK(px[0] == 0xFFFF0000u || px[0] == 0x80008000u || px[0] == 0xFF0000FFu || px[0] == 0);
        /* Twice the size: every pixel four times. */
        CHECK(H->image_decode(H->user, png.data, png.size, px, 4, 4, 16));
        CHECK_HEX(px[0], 0xFFFF0000u);
        CHECK_HEX(px[5], 0xFFFF0000u);
        CHECK_HEX(px[2], 0x80008000u);
        CHECK_HEX(px[4 * 3 + 1], 0xFF0000FFu);
        /* Cut short, it is no image; the header alone still says its size. */
        CHECK(!H->image_decode(H->user, png.data, png.size - 20, px, 2, 2, 8));
        CHECK(H->image_size(H->user, png.data, 33, &w, &h));
        CHECK(!H->image_size(H->user, png.data, 20, &w, &h));
        free(png.data);
    }
    /* A palette of three colours at two bits a pixel, the second of them half transparent (tRNS), in two IDAT chunks. */
    {
        static const u8 palette[] = { 255, 0, 0, 0, 255, 0, 0, 0, 255 }, trns[] = { 255, 128 }, rows[] = { 0, 0x19 };
        png_make(&png, 4, 1, 2, 3, palette, sizeof(palette), trns, sizeof(trns), rows, sizeof(rows), 2);
        CHECK(H->image_size(H->user, png.data, png.size, &w, &h));
        CHECK_INT(w, 4);
        CHECK_INT(h, 1);
        CHECK(H->image_decode(H->user, png.data, png.size, px, 4, 1, 16));
        CHECK_HEX(px[0], 0xFFFF0000u);
        CHECK_HEX(px[1], 0x80008000u);
        CHECK_HEX(px[2], 0xFF0000FFu);
        CHECK_HEX(px[3], 0x80008000u);
        free(png.data);
    }
    /* Grey, 8 bits, stored as differences from the left (filter 1); then grey with alpha; then red, green, blue with one colour named transparent. */
    {
        static const u8 grey[] = { 1, 10, 10, 10 }, grey_alpha[] = { 0, 200, 255, 100, 51 }, rgb[] = { 0, 1, 2, 3, 9, 9, 9 }, key[] = { 0, 9, 0, 9, 0, 9 };
        png_make(&png, 3, 1, 8, 0, NULL, 0, NULL, 0, grey, sizeof(grey), 1);
        CHECK(H->image_decode(H->user, png.data, png.size, px, 3, 1, 12));
        CHECK_HEX(px[0], 0xFF0A0A0Au);
        CHECK_HEX(px[1], 0xFF141414u);
        CHECK_HEX(px[2], 0xFF1E1E1Eu);
        free(png.data);
        png_make(&png, 2, 1, 8, 4, NULL, 0, NULL, 0, grey_alpha, sizeof(grey_alpha), 1);
        CHECK(H->image_decode(H->user, png.data, png.size, px, 2, 1, 8));
        CHECK_HEX(px[0], 0xFFC8C8C8u);
        CHECK_HEX(px[1], 0x33141414u);
        free(png.data);
        png_make(&png, 2, 1, 8, 2, NULL, 0, key, sizeof(key), rgb, sizeof(rgb), 1);
        CHECK(H->image_decode(H->user, png.data, png.size, px, 2, 1, 8));
        CHECK_HEX(px[0], 0xFF010203u);
        CHECK_HEX(px[1], 0u);
        free(png.data);
    }
    /* All five filters, a row each, over a picture with no pattern to it. */
    {
        u8 plain[5][20], rows[5 * 21];
        u32 seed = 12345;
        for (y = 0; y < 5; y++)
            for (x = 0; x < 20; x++) {
                seed = seed * 1103515245u + 12345u;
                plain[y][x] = (u8)(x % 4 == 3 ? 255 : seed >> 16);
            }
        for (y = 0; y < 5; y++) png_filter(rows + y * 21, y, plain[y], y ? plain[y - 1] : NULL, 20, 4);
        png_make(&png, 5, 5, 8, 6, NULL, 0, NULL, 0, rows, sizeof(rows), 1);
        CHECK(H->image_decode(H->user, png.data, png.size, px, 5, 5, 20));
        i = 0;
        for (y = 0; y < 5; y++)
            for (x = 0; x < 5; x++) i += px[y * 5 + x] != (0xFF000000u | (u32)plain[y][4 * x] << 16 | (u32)plain[y][4 * x + 1] << 8 | plain[y][4 * x + 2]);
        CHECK_INT(i, 0);
        free(png.data);
    }
    /* What is not a PNG is refused: a JPEG's first bytes, nothing at all, an interlaced PNG. */
    {
        static const u8 jpeg[40] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0 }, rows[] = { 0, 1, 2, 3, 4 };
        CHECK(!H->image_size(H->user, jpeg, sizeof(jpeg), &w, &h));
        CHECK(!H->image_decode(H->user, jpeg, sizeof(jpeg), px, 1, 1, 4));
        CHECK(!H->image_size(H->user, jpeg, 0, &w, &h));
        png_make(&png, 1, 1, 8, 6, NULL, 0, NULL, 0, rows, sizeof(rows), 1);
        CHECK(H->image_size(H->user, png.data, png.size, &w, &h));
        png.data[28] = 1;
        CHECK(!H->image_size(H->user, png.data, png.size, &w, &h));
        free(png.data);
    }
    /* Written and read back, a bitmap is what it was: opaque, half transparent and empty pixels alike. */
    {
        u32 back[15], picture[15];
        for (i = 0; i < 15; i++)
            picture[i] = premultiplied(i == 7 ? 0 : (u32)(i * 17) << 24 | (u32)(i * 40 % 256) << 16 | (u32)(255 - i * 9) << 8 | (u32)(i * i));
        picture[14] = 0xFF010203u;
        memset(&again, 0, sizeof(again));
        CHECK(H->image_encode(H->user, picture, 5, 3, 20, MEGLET_GFX_PNG, 100, sink_write, &again));
        CHECK(again.size > 8 && memcmp(again.data, "\x89PNG\r\n\x1a\n", 8) == 0);
        CHECK(H->image_size(H->user, again.data, again.size, &w, &h));
        CHECK_INT(w, 5);
        CHECK_INT(h, 3);
        memset(back, 0x55, sizeof(back));
        CHECK(H->image_decode(H->user, again.data, again.size, back, 5, 3, 20));
        CHECK(memcmp(back, picture, sizeof(picture)) == 0);
        free(again.data);
    }
}

static void test_bmp(void)
{
    static const u32 picture[6] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0x80800000u, 0xFFFFFFFFu, 0 };
    char *path;
    u8 *file;
    size_t size = 0;
    int err = 0;
    if (!t_scratch) {
        say("  (no scratch folder: writing a BMP is not tested)");
        return;
    }
    say("  a bitmap written as a BMP file");
    path = mg_sprintf("%s/mg_gfx_test.bmp", t_scratch);
    mg_path_mkdirs(t_scratch);
    CHECK(mg_gfx_write_bmp(path, picture, 3, 2, 12));
    file = mg_file_read_all(path, &size);
    CHECK(file != NULL);
    /* 54 bytes of headers, then two rows of three pixels of three bytes, each row padded to a multiple of four. */
    CHECK_INT(size, 54 + 2 * 12);
    if (file && size == 54 + 2 * 12) {
        CHECK(file[0] == 'B' && file[1] == 'M');
        CHECK_INT(mg_rd32(file + 2), 54 + 2 * 12);
        CHECK_INT(mg_rd32(file + 10), 54);
        CHECK_INT(mg_rd32(file + 18), 3);
        CHECK_INT(mg_rd32(file + 22), 2);
        CHECK_INT(mg_rd16(file + 28), 24);
        /* The bottom row is first in the file, and a pixel is blue, green, red. The half transparent red is written as full red. */
        CHECK(file[54] == 0 && file[55] == 0 && file[56] == 255);
        CHECK(file[57] == 255 && file[58] == 255 && file[59] == 255);
        CHECK(file[54 + 12] == 0 && file[54 + 13] == 0 && file[54 + 14] == 255);
        CHECK(file[54 + 15] == 0 && file[54 + 16] == 255 && file[54 + 17] == 0);
        CHECK(file[54 + 18] == 255 && file[54 + 19] == 0 && file[54 + 20] == 0);
    }
    free(file);
    mg_path_remove(path, &err);
    CHECK(!mg_gfx_write_bmp(path, picture, 0, 2, 12));
    free(path);
}

/*
 * A picture with a bit of everything in it, slanted and curved, and one
 * number made of all its pixels. The number is the same on every machine:
 * nothing in the stand-in depends on how a compiler or a processor rounds.
 */
static void test_same_everywhere(void)
{
    static const u16 text[] = { 'M', 'e', 'g', 'l', 'e', 't', ' ', '1', '2' };
    static const float slant[6] = { 0.8f, -0.6f, 20, 0.6f, 0.8f, 5 }, small[6] = { 0.3f, 0.1f, 2.25f, -0.2f, 0.7f, 30.5f };
    static const float star[] = { MEGLET_GFX_PATH_MOVE,  32, 2,  MEGLET_GFX_PATH_LINE, 44, 60, MEGLET_GFX_PATH_LINE, 3, 22,
                                  MEGLET_GFX_PATH_LINE,  61, 22, MEGLET_GFX_PATH_LINE, 20, 60, MEGLET_GFX_PATH_CLOSE,
                                  MEGLET_GFX_PATH_MOVE,  5, 50,  MEGLET_GFX_PATH_CUBIC, 20, 20.5f, 40, 80.25f, 60, 45,
                                  MEGLET_GFX_PATH_QUAD,  31.5f, 70, 5, 50 };
    static const float lines[] = { MEGLET_GFX_PATH_MOVE, 3, 4, MEGLET_GFX_PATH_LINE, 60.5f, 17.25f, MEGLET_GFX_PATH_MOVE, 50, 3, MEGLET_GFX_PATH_LINE, 7.75f, 61 };
    tbitmap b, s;
    meglet_gfx_image image;
    meglet_gfx_paint p;
    float whole[4] = { 0, 0, 3, 3 }, onto[4] = { 0, 0, 40, 30 };
    u32 sum = 2166136261u;
    int i;
    say("  a picture that comes out the same everywhere");
    bitmap_make(&b, 64, 64);
    bitmap_make(&s, 3, 3);
    for (i = 0; i < 9; i++) s.px[i] = premultiplied(0x60000000u + (u32)i * 0x13579Bu + ((u32)i << 28));
    image_of(&s, &image);
    H->draw_color(H->user, b.cv, 0x30808080u, MEGLET_GFX_BLEND_SRC);
    p = paint_of(0xC03060F0u);
    H->draw_oval(H->user, b.cv, 3.3f, 5.7f, 58.1f, 40.2f, &p, NULL);
    p = paint_of(0x80F0A020u);
    p.style = MEGLET_GFX_STROKE;
    p.stroke_width = 3.5f;
    H->draw_round_rect(H->user, b.cv, 8.5f, 9.25f, 50, 55.5f, 11, 7.5f, &p, NULL);
    p = paint_of(0x9020C040u);
    H->draw_path(H->user, b.cv, star, (int)MG_COUNT(star), MEGLET_GFX_EVEN_ODD, &p, NULL);
    p = paint_of(0xFF102030u);
    p.style = MEGLET_GFX_STROKE;
    p.stroke_width = 2.5f;
    p.cap = MEGLET_GFX_CAP_SQUARE;
    p.blend = MEGLET_GFX_BLEND_XOR;
    H->draw_path(H->user, b.cv, lines, (int)MG_COUNT(lines), MEGLET_GFX_WINDING, &p, NULL);
    H->save(H->user, b.cv);
    H->set_matrix(H->user, b.cv, slant);
    H->clip_rect(H->user, b.cv, 2, 2, 50, 45, MEGLET_GFX_CLIP_INTERSECT);
    p = paint_of(0x70FF00FFu);
    p.blend = MEGLET_GFX_BLEND_SCREEN;
    H->draw_rect(H->user, b.cv, 5, 5, 30, 20.5f, &p, NULL);
    p = paint_of(0xB0000000u);
    H->draw_bitmap(H->user, b.cv, &image, whole, onto, &p);
    p = paint_of(0xE0FFFF00u);
    p.text_size = 9.5f;
    p.letter_spacing = 0.05f;
    H->draw_text(H->user, b.cv, text, (int)MG_COUNT(text), 4.5f, 28.25f, &p, NULL, NULL);
    H->set_matrix(H->user, b.cv, small);
    p = paint_of(0x60FFFFFFu);
    p.style = MEGLET_GFX_FILL_AND_STROKE;
    p.blend = MEGLET_GFX_BLEND_DARKEN;
    H->draw_oval(H->user, b.cv, 10, 10, 90, 70, &p, NULL);
    H->restore(H->user, b.cv);
    for (i = 0; i < 64 * 64; i++) sum = (sum ^ b.px[i]) * 16777619u;
    CHECK(covered(&b) == 64 * 64);
    CHECK_HEX(sum, 0xC789983Fu);
    /* For whoever wants to see it. */
    if (t_scratch) {
        char *path = mg_sprintf("%s/mg_gfx_scene.bmp", t_scratch);
        mg_path_mkdirs(t_scratch);
        if (mg_gfx_write_bmp(path, b.px, 64, 64, 64 * 4)) say("    (the picture is in %s)", path);
        free(path);
    }
    bitmap_done(&s);
    bitmap_done(&b);
}

void mg_gfx_tests_set_scratch(const char *dir)
{
    free(t_scratch);
    t_scratch = dir && dir[0] ? mg_strdup(dir) : NULL;
}

int mg_gfx_tests_run(mg_test_sink sink, void *user, int *checks)
{
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    H = mg_gfx_standin();
    say("the stand-in for the shell's drawing, through meglet_gfx_host");
    /* Every function of the host is there: a test below that calls one need not ask. */
    CHECK(H->canvas_new && H->canvas_free && H->flush && H->touched && H->save && H->save_layer && H->restore && H->set_matrix && H->clip_rect && H->clip_path);
    CHECK(H->draw_color && H->draw_rect && H->draw_round_rect && H->draw_oval && H->draw_path && H->draw_bitmap && H->draw_text);
    CHECK(H->text_measure && H->text_metrics && H->text_bounds && H->text_break && H->text_widths);
    CHECK(H->typeface_create && H->typeface_derive && H->typeface_from_data);
    CHECK(H->shader_linear && H->shader_radial && H->shader_sweep && H->shader_bitmap && H->shader_matrix && H->shader_free);
    CHECK(H->image_size && H->image_decode && H->image_encode && H->bitmap_gone);
    test_fills();
    test_matrix_and_clip();
    test_shapes();
    test_bitmaps();
    test_text();
    test_images();
    test_same_everywhere();
    test_bmp();
    if (checks) *checks += t_checks;
    return t_failed;
}
