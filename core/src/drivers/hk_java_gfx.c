/*
 * megler.Gfx: the native methods under the Java framework's android.graphics.
 *
 * The framework (the original Megler's, src/java-framework) has Bitmap, Canvas,
 * Paint, Path, Typeface and the shaders in Java, on the 54 native methods of
 * megler.Gfx. There they are CoreGraphics and CoreText
 * (megler-tl-dvm-graphics.c). Here a bitmap's pixels and everything that is
 * bookkeeping are this file's, and what has to be drawn, measured or decoded
 * goes to the shell through megler_gfx_host (megler.h, which is the contract for
 * that side and says what each call means). This comment is the other side:
 * what the Java passes.
 *
 * Handles are Java longs. A bitmap, a canvas and a typeface are objects of
 * this file; a shader is the shell's own pointer. 0 is "none" everywhere.
 *
 * A bitmap is 4 * width * height bytes from calloc, B, G, R, A premultiplied,
 * top row first: what Direct2D, WIC and the phone's swap chain take as it
 * is. Android's own order is R, G, B, A, and it shows in two places, which
 * convert: bmRaw (copyPixelsToBuffer, copyPixelsFromBuffer, texture uploads,
 * frames from native producers) and hk_java_gfx_lock_rgba (libjnigraphics).
 * Colours that cross as ints are 0xAARRGGBB and not premultiplied: getPixel
 * and setPixel convert, as Android's do.
 *
 * A Paint arrives as its int[20] (android.graphics.Paint.mP), floats as their
 * bits; a null or short array is the default paint:
 *
 *    0  colour, 0xAARRGGBB not premultiplied; setAlpha changes its top byte
 *    1  style: 0 FILL, 1 STROKE, 2 FILL_AND_STROKE
 *    2  stroke width (float); 0 is a hairline
 *    3  flags, the framework's own bits and not Android's: 1 anti-alias,
 *       2 filter bitmaps, 4 fake bold, 8 underline, 16 strike through
 *    4  cap: 0 BUTT, 1 ROUND, 2 SQUARE
 *    5  join: 0 MITER, 1 ROUND, 2 BEVEL
 *    6  text size (float), 12 to start with
 *    7  text align: 0 LEFT, 1 CENTER, 2 RIGHT
 *    8  miter limit (float), 4 to start with
 *    9  shadow radius (float; 0: none), 10 and 11 its offset (floats),
 *   12  its colour
 *   13  text scale x (float), 1 to start with
 *   14  text skew x (float)
 *   15  letter spacing in ems (float)
 *   16  transfer mode, a PorterDuff.Mode number (3, SRC_OVER, to start with)
 *   17  the colour filter's colour, 18 its mode (-1: no filter)
 *   19  255, and never anything else: nothing reads it
 *
 * The PorterDuff numbers are 0 CLEAR, 1 SRC, 2 DST, 3 SRC_OVER, 4 DST_OVER,
 * 5 SRC_IN, 6 DST_IN, 7 SRC_OUT, 8 DST_OUT, 9 SRC_ATOP, 10 DST_ATOP, 11 XOR,
 * 12 ADD, 13 MULTIPLY, 14 SCREEN, 15 OVERLAY, 16 DARKEN, 17 LIGHTEN; the
 * framework turns every BlendMode it has no number for into 3.
 *
 * A Path arrives as its float[] and how many of them count: an operation and
 * its operands, over and over (android.graphics.Path.ops):
 *
 *    0 MOVE x y            10 RMOVE dx dy       (from the last point)
 *    1 LINE x y            11 RLINE dx dy
 *    2 QUAD cx cy x y      12 RQUAD, both points from the last point
 *    3 CUBIC 3 points      13 RCUBIC, all three from the last point
 *    4 CLOSE
 *    5 ARC l t r b start sweep move   an arc of the oval in that rectangle,
 *                          degrees, 0 at three o'clock, positive clockwise;
 *                          joined to what is there by a line unless `move`
 *    6 RECT l t r b ccw    7 OVAL l t r b ccw
 *    8 RRECT l t r b rx ry ccw
 *   14 RRECT8 l t r b and (rx, ry) for the top left, top right, bottom right
 *                          and bottom left corner, then ccw
 *    9 TRANSFORM m0..m5    everything before it goes through the matrix
 *
 * with a fill rule beside it, 0 winding or 1 even-odd (the inverse rules
 * arrive as these two). The shell is not shown this: path_build turns it into
 * figures of lines and Beziers with every coordinate absolute, and the bounds,
 * the hit test and the length are worked out here from the same figures.
 *
 * A matrix arrives as six floats, Android's first two rows: x' = m0 x + m1 y
 * + m2, y' = m3 x + m4 y + m5. cvMatrix(set = false) is Canvas.concat, which
 * is a pre-concatenation: what is drawn afterwards goes through the new
 * matrix first and then through what the canvas had. The canvas's matrix is
 * kept here and the shell is given the product whole.
 *
 * Also kept here, per canvas: the save count, and the clip's bounding box in
 * pixels, which is what cvClipBounds and the booleans of cvClipRect and
 * cvClipPath answer from. It is the box of the boxes: exact while clips are
 * rectangles under a matrix that does not rotate, and never too small.
 */
#include <math.h>
#include <stdio.h>

#include "../dvm/hk_dvm.h"
#include "../host/hk_host.h"
#include "../webp/hk_webp.h"
#include "hk_java_gfx.h"

#define N(name) static void name(hk_jcall *c)

#define BM_MAGIC 0x31504d42u
#define CV_MAGIC 0x31564e43u
#define TF_MAGIC 0x31434654u

typedef struct gcanvas gcanvas;

typedef struct gbitmap {
    u32 magic;
    i32 w, h, stride;
    u8 *px;
    gcanvas *canvases;
    u32 id, version;                   /* for a shell that keeps textures: megler_gfx_image */
    i32 rgba_locks;                    /* above zero: the memory is in Android's order (hk_java_gfx_lock_rgba) */
} gbitmap;

typedef struct gstate {
    float m[6];
    float clip[4];                     /* the clip's bounding box, in pixels */
    bool empty;
    bool clipped;                      /* something has been taken from the clip: it is no longer the whole bitmap for certain */
    bool layer;                        /* the save that made this state began a layer */
} gstate;

struct gcanvas {
    u32 magic;
    gbitmap *bm;                       /* NULL once the bitmap is freed: nothing is drawn through the canvas any more */
    void *host;
    gcanvas *next;
    bool dirty;                        /* drawn through since its last flush */
    bool stale;                        /* the memory was written behind its back */
    gstate *st;                        /* st[depth] is the current one */
    i32 depth, cap;
    float sent[6];                     /* the matrix the shell has */
};

typedef struct gface {
    u32 magic;
    void *host;                        /* NULL: the shell's default face */
    i32 weight;
    bool italic;
} gface;

static megler_gfx_host G;
/*
 * The time spent in the shell's drawing, summed for each thread: GFX(statement) runs a statement that calls G and adds what
 * it took, by the clock. Two readings of the clock a call; the Java driver's frame meter reads the main thread's sum
 * (hk_java_gfx_host_ns).
 */
static HK_THREAD_LOCAL u64 t_host_ns;
#define GFX(statement)                                    \
    do {                                                  \
        u64 gfx_started_ = hk_time_monotonic_ns();        \
        statement;                                        \
        t_host_ns += hk_time_monotonic_ns() - gfx_started_; \
    } while (0)
static struct {
    void (*fn)(void *user, const void *pixels, int width, int height, int stride);
    void *user;
    bool (*canvas_fn)(void *user, void *canvas, int width, int height);
    void *canvas_user;
} g_present;
/* The bitmaps' counts, each bitmap's list of canvases and their two flags, and the typefaces made. */
static hk_lock g_lock = HK_LOCK_INIT;
static u64 g_bytes;
static u32 g_count, g_next_id;
static gface g_default_face = { TF_MAGIC, NULL, 400, false };
static struct derived {
    gface *from, *made;
    i32 weight;
    bool italic;
} *g_derived;
static i32 g_nderived, g_cap_derived;

static bool missing(const char *fn)
{
    hk_note_once("gfx: the shell has no %s: what it would draw or answer is missing", fn);
    return false;
}
#define HAS(fn) (G.fn != NULL || missing(#fn))

static i64 handle(const void *p) { return (i64)(hk_addr)p; }
static void *pointer(i64 h) { return (void *)(hk_addr)h; }

/* The elements of a primitive array of that type with at least that many of them; NULL for anything else. */
static void *elements(hk_jobj *a, char etype, u32 at_least)
{
    if (!a || a->kind != HK_J_PRIM_ARRAY || a->u.arr.etype != etype || a->u.arr.len < at_least) return NULL;
    return a->u.arr.data;
}
static u32 length_of(hk_jobj *a) { return a && a->kind == HK_J_PRIM_ARRAY ? a->u.arr.len : 0; }

/* A float that is a number, or the fallback. */
static float number(float f, float fallback) { return isfinite(f) ? f : fallback; }
/* A double as an int without leaving its range, which C leaves undefined. */
static i32 whole(double v)
{
    if (!(v == v)) return 0;
    if (v > 2000000000.0) return 2000000000;
    if (v < -2000000000.0) return -2000000000;
    return (i32)v;
}

/* ---------------------------------------------------------------- colours */

/* A colour as Android passes it (0xAARRGGBB, not premultiplied) to the word a pixel is, and back, with the original's rounding. */
static u32 premultiplied(u32 c)
{
    u32 a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (a == 255) return c;
    r = (r * a + 127) / 255;
    g = (g * a + 127) / 255;
    b = (b * a + 127) / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

static u32 unpremultiplied(u32 p)
{
    u32 a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    if (!a) return 0;
    if (a == 255) return p;
    r = (r * 255 + a / 2) / a;
    g = (g * 255 + a / 2) / a;
    b = (b * 255 + a / 2) / a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return a << 24 | r << 16 | g << 8 | b;
}

/*
 * One channel of S put on D by a PorterDuff number, both premultiplied and in
 * 0..1 (megler.h has the table). The same expressions give the alpha when the
 * channel passed is the alpha itself.
 */
static float blend_channel(int mode, float s, float d, float sa, float da)
{
    float both = s * (1 - da) + d * (1 - sa);
    switch (mode) {
    case MEGLER_GFX_BLEND_CLEAR: return 0;
    case MEGLER_GFX_BLEND_SRC: return s;
    case MEGLER_GFX_BLEND_DST: return d;
    case MEGLER_GFX_BLEND_DST_OVER: return d + s * (1 - da);
    case MEGLER_GFX_BLEND_SRC_IN: return s * da;
    case MEGLER_GFX_BLEND_DST_IN: return d * sa;
    case MEGLER_GFX_BLEND_SRC_OUT: return s * (1 - da);
    case MEGLER_GFX_BLEND_DST_OUT: return d * (1 - sa);
    case MEGLER_GFX_BLEND_SRC_ATOP: return s * da + d * (1 - sa);
    case MEGLER_GFX_BLEND_DST_ATOP: return d * sa + s * (1 - da);
    case MEGLER_GFX_BLEND_XOR: return both;
    case MEGLER_GFX_BLEND_PLUS: return s + d > 1 ? 1 : s + d;
    case MEGLER_GFX_BLEND_MODULATE: return s * d;
    case MEGLER_GFX_BLEND_SCREEN: return s + d - s * d;
    case MEGLER_GFX_BLEND_OVERLAY: return both + (2 * d <= da ? 2 * s * d : sa * da - 2 * (da - d) * (sa - s));
    case MEGLER_GFX_BLEND_DARKEN: return both + (s * da < d * sa ? s * da : d * sa);
    case MEGLER_GFX_BLEND_LIGHTEN: return both + (s * da > d * sa ? s * da : d * sa);
    default: return s + d * (1 - sa);
    }
}

/*
 * What a colour filter makes of a plain colour: the filter's colour as the
 * source on the paint's as the destination. Android filters a paint's colour
 * before the shape's coverage comes into it, so for a plain colour the
 * filter can be done once here and the shell be spared it.
 */
static u32 filtered(int mode, u32 filter, u32 color)
{
    float s[4], d[4], o[4];
    u32 out = 0;
    int i;
    for (i = 0; i < 4; i++) {
        s[i] = (float)((filter >> (24 - 8 * i)) & 255) / 255.0f;
        d[i] = (float)((color >> (24 - 8 * i)) & 255) / 255.0f;
    }
    for (i = 1; i < 4; i++) {
        s[i] *= s[0];
        d[i] *= d[0];
    }
    for (i = 0; i < 4; i++) o[i] = blend_channel(mode, s[i], d[i], s[0], d[0]);
    if (!(o[0] > 0)) return 0;
    if (o[0] > 1) o[0] = 1;
    for (i = 0; i < 4; i++) {
        float v = i ? o[i] / o[0] : o[0];
        if (!(v > 0)) v = 0;
        if (v > 1) v = 1;
        out |= (u32)(v * 255.0f + 0.5f) << (24 - 8 * i);
    }
    return out;
}

/* ---------------------------------------------------------------- bitmaps */

static gbitmap *bm_new(i64 w, i64 h)
{
    gbitmap *b;
    /* The original's limit: 2^28 pixels, a gigabyte. calloc refuses long before that on a phone. */
    if (w <= 0 || h <= 0 || w > 0x7fffffff / 4 || w * h > ((i64)1 << 28)) return NULL;
    b = (gbitmap *)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->px = (u8 *)calloc((size_t)h, (size_t)w * 4);
    if (!b->px) {
        free(b);
        return NULL;
    }
    b->magic = BM_MAGIC;
    b->w = (i32)w;
    b->h = (i32)h;
    b->stride = (i32)w * 4;
    hk_lock_acquire(&g_lock);
    if (!++g_next_id) g_next_id = 1;
    b->id = g_next_id;
    g_bytes += (u64)b->stride * (u64)b->h;
    g_count++;
    hk_lock_release(&g_lock);
    /* Pixels a Bitmap object keeps alive: the collector counts them toward its next collection, as Android counts a bitmap's. */
    hk_dvm_gc_native((i64)b->stride * (i64)b->h);
    return b;
}

static gbitmap *bm_of(i64 h)
{
    gbitmap *b = (gbitmap *)pointer(h);
    return b && b->magic == BM_MAGIC ? b : NULL;
}

static u32 *bm_row(const gbitmap *b, i32 y) { return (u32 *)(void *)(b->px + (size_t)y * (size_t)b->stride); }

/* Before the pixels are read: what was drawn through the bitmap's canvases goes into the memory. */
static void bm_settle(gbitmap *b)
{
    gcanvas *v;
    hk_lock_acquire(&g_lock);
    for (v = b->canvases; v; v = v->next)
        if (v->dirty) {
            if (v->host && G.flush) GFX(G.flush(G.user, v->host));
            v->dirty = false;
        }
    hk_lock_release(&g_lock);
}

/* After the pixels were written by anything but a canvas: the canvases are told before they next draw, and a kept copy is no good. */
static void bm_changed(gbitmap *b)
{
    gcanvas *v;
    hk_lock_acquire(&g_lock);
    b->version++;
    for (v = b->canvases; v; v = v->next) v->stale = true;
    hk_lock_release(&g_lock);
}

static void bm_free(gbitmap *b)
{
    gcanvas *v;
    i32 i;
    hk_lock_acquire(&g_lock);
    /*
     * A Canvas can outlive its Bitmap (recycle() while a Canvas still holds
     * it). Its shell half goes now, as the shell is promised it: every save
     * undone, flushed, while the memory is still there. The canvas itself
     * stays, counts its saves as before, and draws nothing from here on.
     */
    for (v = b->canvases; v; v = v->next) {
        if (v->host) {
            for (i = v->depth; i > 0 && G.restore; i--) GFX(G.restore(G.user, v->host));
            if (v->dirty && G.flush) GFX(G.flush(G.user, v->host));
            if (G.canvas_free) GFX(G.canvas_free(G.user, v->host));
        }
        v->host = NULL;
        v->bm = NULL;
        v->dirty = false;
    }
    g_bytes -= (u64)b->stride * (u64)b->h;
    g_count--;
    hk_lock_release(&g_lock);
    if (G.bitmap_gone) GFX(G.bitmap_gone(G.user, b->id));
    b->magic = 0;
    free(b->px);
    free(b);
}

static void image_of(const gbitmap *b, megler_gfx_image *out)
{
    out->pixels = b->px;
    out->width = b->w;
    out->height = b->h;
    out->stride = b->stride;
    out->id = b->id;
    out->version = b->version;
}

/* Red and blue change places: between this file's order and Android's, either way. */
static void swap_red_blue(u8 *dst, const u8 *src, size_t pixels)
{
    size_t i;
    for (i = 0; i < pixels; i++) {
        u8 b = src[4 * i], r = src[4 * i + 2];
        dst[4 * i] = r;
        dst[4 * i + 1] = src[4 * i + 1];
        dst[4 * i + 2] = b;
        dst[4 * i + 3] = src[4 * i + 3];
    }
}

/* ---- scaling a bitmap without the shell: createScaledBitmap, and every picture a resource scales to the screen's density */

/* For each pixel of a row or column of the result: the first source pixel that makes it, how many do, and how much of each. */
typedef struct gtaps {
    i32 *first, *count, max;
    float *weight;
} gtaps;

static void taps_free(gtaps *t)
{
    free(t->first);
    free(t->count);
    free(t->weight);
}

/*
 * `from` pixels onto `to`. Not smooth: the nearest one. Smooth and growing:
 * the two around the place, by distance. Smooth and shrinking: every pixel
 * the result's pixel covers, by how much of it, which is what keeps a picture
 * reduced several times from breaking up.
 */
static bool taps_make(gtaps *t, i32 from, i32 to, bool mirrored, bool smooth)
{
    double step = (double)from / (double)to;
    i32 i, k;
    t->max = !smooth ? 1 : to >= from ? 2 : (i32)ceil(step) + 1;
    t->first = (i32 *)malloc(sizeof(i32) * (size_t)to);
    t->count = (i32 *)malloc(sizeof(i32) * (size_t)to);
    t->weight = (float *)malloc(sizeof(float) * (size_t)to * (size_t)t->max);
    if (!t->first || !t->count || !t->weight) {
        taps_free(t);
        return false;
    }
    for (i = 0; i < to; i++) {
        i32 q = mirrored ? to - 1 - i : i, first, count = 1;
        float *w = t->weight + (size_t)i * (size_t)t->max;
        w[0] = 1;
        if (!smooth) {
            first = (i32)floor(((double)q + 0.5) * step);
        } else if (to >= from) {
            double at = ((double)q + 0.5) * step - 0.5, low = floor(at);
            first = (i32)low;
            if (first >= 0 && first + 1 < from && at > low) {
                count = 2;
                w[0] = (float)(1 - (at - low));
                w[1] = (float)(at - low);
            }
        } else {
            double a = (double)q * step, b = (double)(q + 1) * step;
            i32 last = (i32)ceil(b) - 1;
            first = (i32)floor(a);
            if (last > from - 1) last = from - 1;
            if (last < first) last = first;
            count = last - first + 1;
            if (count > t->max) count = t->max;
            for (k = 0; k < count; k++) {
                double lo = a > first + k ? a : first + k, hi = b < first + k + 1 ? b : first + k + 1;
                w[k] = (float)((hi > lo ? hi - lo : 0) / step);
            }
        }
        if (first < 0) first = 0;
        if (first > from - 1) first = from - 1;
        if (first + count > from) count = from - first;
        t->first[i] = first;
        t->count[i] = count;
    }
    return true;
}

/* The part (x, y, w, h) of `s` onto all of `d`. False when out of memory. */
static bool bm_scale(const gbitmap *s, i32 x, i32 y, i32 w, i32 h, gbitmap *d, bool mirror_x, bool mirror_y, bool smooth)
{
    gtaps tx, ty;
    i32 i, j, a, b, ch;
    memset(&tx, 0, sizeof(tx));
    memset(&ty, 0, sizeof(ty));
    if (!taps_make(&tx, w, d->w, mirror_x, smooth)) return false;
    if (!taps_make(&ty, h, d->h, mirror_y, smooth)) {
        taps_free(&tx);
        return false;
    }
    for (j = 0; j < d->h; j++) {
        u8 *out = d->px + (size_t)j * (size_t)d->stride;
        const float *wy = ty.weight + (size_t)j * (size_t)ty.max;
        for (i = 0; i < d->w; i++, out += 4) {
            const float *wx = tx.weight + (size_t)i * (size_t)tx.max;
            float sum[4] = { 0, 0, 0, 0 };
            if (tx.count[i] == 1 && ty.count[j] == 1) {
                memcpy(out, s->px + (size_t)(y + ty.first[j]) * (size_t)s->stride + 4 * (size_t)(x + tx.first[i]), 4);
                continue;
            }
            for (b = 0; b < ty.count[j]; b++) {
                const u8 *in = s->px + (size_t)(y + ty.first[j] + b) * (size_t)s->stride + 4 * (size_t)(x + tx.first[i]);
                for (a = 0; a < tx.count[i]; a++, in += 4) {
                    float k = wx[a] * wy[b];
                    for (ch = 0; ch < 4; ch++) sum[ch] += k * (float)in[ch];
                }
            }
            for (ch = 0; ch < 4; ch++) out[ch] = (u8)(sum[ch] >= 254.5f ? 255 : sum[ch] <= 0 ? 0 : (int)(sum[ch] + 0.5f));
        }
    }
    taps_free(&tx);
    taps_free(&ty);
    return true;
}

/* ----------------------------------------------------------------- paints */

enum {
    P_COLOR, P_STYLE, P_STROKE, P_FLAGS, P_CAP, P_JOIN, P_TEXTSIZE, P_ALIGN, P_MITER, P_SHRAD, P_SHDX, P_SHDY, P_SHCOL,
    P_SCALEX, P_SKEWX, P_LSPACE, P_XFER, P_CFCOLOR, P_CFMODE, P_ALPHA, P_COUNT
};
enum { PF_AA = 1, PF_FILTER = 2, PF_FAKEBOLD = 4, PF_UNDERLINE = 8, PF_STRIKE = 16 };

typedef struct gpaint {
    megler_gfx_paint p;
    bool underline, strike;            /* drawn here, as rectangles */
} gpaint;

/* A PorterDuff number the shell can be given: what the framework has no number for is SRC_OVER there already. */
static int blend_number(i32 mode) { return mode >= 0 && mode <= MEGLER_GFX_BLEND_LIGHTEN ? (int)mode : MEGLER_GFX_BLEND_SRC_OVER; }

/*
 * The paint of a drawing call, unpacked and made safe to hand on. No array
 * is the default paint: what drawBitmap(bitmap, ..., null) draws with.
 */
static void paint_of(hk_jobj *array, gpaint *out)
{
    const i32 *a = (const i32 *)elements(array, 'I', P_COUNT);
    megler_gfx_paint *p = &out->p;
    memset(out, 0, sizeof(*out));
    p->color = 0xFF000000u;
    p->flags = MEGLER_GFX_ANTI_ALIAS;
    p->miter_limit = 4;
    p->text_size = 12;
    p->text_scale_x = 1;
    p->blend = MEGLER_GFX_BLEND_SRC_OVER;
    p->filter_mode = -1;
    if (!a) return;
    p->color = (u32)a[P_COLOR];
    p->style = a[P_STYLE] == 1 || a[P_STYLE] == 2 ? (int)a[P_STYLE] : MEGLER_GFX_FILL;
    p->stroke_width = number(hk_f((u32)a[P_STROKE]), 0);
    if (p->stroke_width < 0) p->stroke_width = 0;
    p->flags = (a[P_FLAGS] & PF_AA ? MEGLER_GFX_ANTI_ALIAS : 0) | (a[P_FLAGS] & PF_FILTER ? MEGLER_GFX_FILTER_BITMAP : 0) |
               (a[P_FLAGS] & PF_FAKEBOLD ? MEGLER_GFX_FAKE_BOLD : 0);
    out->underline = (a[P_FLAGS] & PF_UNDERLINE) != 0;
    out->strike = (a[P_FLAGS] & PF_STRIKE) != 0;
    p->cap = a[P_CAP] == 1 || a[P_CAP] == 2 ? (int)a[P_CAP] : MEGLER_GFX_CAP_BUTT;
    p->join = a[P_JOIN] == 1 || a[P_JOIN] == 2 ? (int)a[P_JOIN] : MEGLER_GFX_JOIN_MITER;
    p->miter_limit = number(hk_f((u32)a[P_MITER]), 4);
    if (!(p->miter_limit > 0)) p->miter_limit = 4;
    p->text_size = number(hk_f((u32)a[P_TEXTSIZE]), 12);
    if (!(p->text_size > 0)) p->text_size = 12;
    p->text_align = a[P_ALIGN] == 1 || a[P_ALIGN] == 2 ? (int)a[P_ALIGN] : MEGLER_GFX_ALIGN_LEFT;
    p->text_scale_x = number(hk_f((u32)a[P_SCALEX]), 1);
    if (!(p->text_scale_x > 0)) p->text_scale_x = 1;
    p->text_skew_x = number(hk_f((u32)a[P_SKEWX]), 0);
    p->letter_spacing = number(hk_f((u32)a[P_LSPACE]), 0);
    p->blend = blend_number(a[P_XFER]);
    p->filter_mode = a[P_CFMODE] < 0 ? -1 : blend_number(a[P_CFMODE]);
    p->filter_color = (u32)a[P_CFCOLOR];
    p->shadow_radius = number(hk_f((u32)a[P_SHRAD]), 0);
    if (!(p->shadow_radius > 0)) p->shadow_radius = 0;
    p->shadow_dx = number(hk_f((u32)a[P_SHDX]), 0);
    p->shadow_dy = number(hk_f((u32)a[P_SHDY]), 0);
    p->shadow_color = (u32)a[P_SHCOL];
}

/*
 * The paint for one drawing call. False when the call draws nothing: DST
 * leaves the picture as it is. `plain` says the paint's own colour is what
 * is drawn (no shader, no bitmap), and then a colour filter is done here.
 */
static bool paint_for(hk_jobj *array, bool plain, gpaint *out)
{
    paint_of(array, out);
    if (out->p.blend == MEGLER_GFX_BLEND_DST) return false;
    if (plain && out->p.filter_mode >= 0) {
        out->p.color = filtered(out->p.filter_mode, out->p.filter_color, out->p.color);
        out->p.filter_mode = -1;
    }
    return true;
}

/* ------------------------------------------------------------------ paths */

/* What the shell is given (megler.h, draw_path), and what the questions about a path are answered from. */
typedef struct gpath {
    float *v;
    i32 n, cap;
    float sx, sy, cx, cy;              /* where the figure began, and the current point */
    bool any;                          /* something has been put in it */
    bool open;                         /* a figure is begun and not closed */
    i32 segments;                      /* in that figure */
    float fixed[192];
} gpath;

static void path_init(gpath *p)
{
    memset(p, 0, offsetof(gpath, fixed));
    p->v = p->fixed;
    p->cap = (i32)HK_COUNT(p->fixed);
}

static void path_free(gpath *p)
{
    if (p->v != p->fixed) free(p->v);
}

static void path_put(gpath *p, int verb, const float *xy, int count)
{
    if (p->n + 1 + count > p->cap) {
        i32 cap = p->cap * 2 + 64;
        float *v = (float *)hk_alloc(sizeof(float) * (size_t)cap);
        memcpy(v, p->v, sizeof(float) * (size_t)p->n);
        path_free(p);
        p->v = v;
        p->cap = cap;
    }
    p->v[p->n++] = (float)verb;
    if (count) memcpy(p->v + p->n, xy, sizeof(float) * (size_t)count);
    p->n += count;
}

static void path_move(gpath *p, float x, float y)
{
    float xy[2];
    xy[0] = x;
    xy[1] = y;
    path_put(p, MEGLER_GFX_PATH_MOVE, xy, 2);
    p->sx = p->cx = x;
    p->sy = p->cy = y;
    p->any = p->open = true;
    p->segments = 0;
}

/*
 * A segment is about to be added, and needs a figure to be added to. A path
 * that starts with a line starts at (0, 0), as Android's does; after a close
 * the next figure starts where the closed one did.
 */
static void path_segment(gpath *p)
{
    if (!p->any) path_move(p, 0, 0);
    else if (!p->open) path_move(p, p->cx, p->cy);
    p->segments++;
}

static void path_line(gpath *p, float x, float y)
{
    float xy[2];
    path_segment(p);
    xy[0] = x;
    xy[1] = y;
    path_put(p, MEGLER_GFX_PATH_LINE, xy, 2);
    p->cx = x;
    p->cy = y;
}

static void path_quad(gpath *p, float x1, float y1, float x, float y)
{
    float xy[4];
    path_segment(p);
    xy[0] = x1;
    xy[1] = y1;
    xy[2] = x;
    xy[3] = y;
    path_put(p, MEGLER_GFX_PATH_QUAD, xy, 4);
    p->cx = x;
    p->cy = y;
}

static void path_cubic(gpath *p, float x1, float y1, float x2, float y2, float x, float y)
{
    float xy[6];
    path_segment(p);
    xy[0] = x1;
    xy[1] = y1;
    xy[2] = x2;
    xy[3] = y2;
    xy[4] = x;
    xy[5] = y;
    path_put(p, MEGLER_GFX_PATH_CUBIC, xy, 6);
    p->cx = x;
    p->cy = y;
}

static void path_close(gpath *p)
{
    if (!p->open) return;
    /* A figure of one point has nothing to close; the next segment still starts a figure of its own from there. */
    if (p->segments) path_put(p, MEGLER_GFX_PATH_CLOSE, NULL, 0);
    p->cx = p->sx;
    p->cy = p->sy;
    p->open = false;
}

static void path_rect(gpath *p, float l, float t, float r, float b)
{
    path_move(p, l, t);
    path_line(p, r, t);
    path_line(p, r, b);
    path_line(p, l, b);
    path_close(p);
}

/* How far along its tangents a cubic's control points lie to make a quarter of a circle of radius one. */
#define QUARTER 0.5522847f

/* The ellipse in a rectangle, clockwise as seen with y down, from three o'clock. */
static void path_oval(gpath *p, float l, float t, float r, float b)
{
    float cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2, kx = rx * QUARTER, ky = ry * QUARTER;
    path_move(p, cx + rx, cy);
    path_cubic(p, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    path_cubic(p, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    path_cubic(p, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    path_cubic(p, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    path_close(p);
}

/* A rectangle with a quarter ellipse at each corner: (rx, ry) for the top left, top right, bottom right, bottom left, as Android's addRoundRect(float[8]). */
static void path_round_rect(gpath *p, float l, float t, float r, float b, const float *rad)
{
    float rx[4], ry[4], w, h;
    int i;
    if (l > r) {
        w = l;
        l = r;
        r = w;
    }
    if (t > b) {
        h = t;
        t = b;
        b = h;
    }
    w = r - l;
    h = b - t;
    for (i = 0; i < 4; i++) {
        rx[i] = rad[2 * i] > 0 ? rad[2 * i] : 0;
        ry[i] = rad[2 * i + 1] > 0 ? rad[2 * i + 1] : 0;
        if (rx[i] > w / 2) rx[i] = w / 2;
        if (ry[i] > h / 2) ry[i] = h / 2;
    }
    path_move(p, l + rx[0], t);
    path_line(p, r - rx[1], t);
    path_cubic(p, r - rx[1] + rx[1] * QUARTER, t, r, t + ry[1] - ry[1] * QUARTER, r, t + ry[1]);
    path_line(p, r, b - ry[2]);
    path_cubic(p, r, b - ry[2] + ry[2] * QUARTER, r - rx[2] + rx[2] * QUARTER, b, r - rx[2], b);
    path_line(p, l + rx[3], b);
    path_cubic(p, l + rx[3] - rx[3] * QUARTER, b, l, b - ry[3] + ry[3] * QUARTER, l, b - ry[3]);
    path_line(p, l, t + ry[0]);
    path_cubic(p, l, t + ry[0] - ry[0] * QUARTER, l + rx[0] - rx[0] * QUARTER, t, l + rx[0], t);
    path_close(p);
}

/*
 * An arc of the ellipse in a rectangle: from `start` degrees through `sweep`
 * degrees, 0 at three o'clock and positive clockwise as seen with y down.
 * It begins a figure when `move` says so or nothing is there yet, and is
 * otherwise joined to the current point by a line. A whole turn or more is
 * the whole ellipse, after which the current point is the arc's start, as
 * the original leaves it.
 */
static void path_arc(gpath *p, float l, float t, float r, float b, float start, float sweep, bool move)
{
    double cx = ((double)l + r) / 2, cy = ((double)t + b) / 2, rx = ((double)r - l) / 2, ry = ((double)b - t) / 2;
    double a = (double)start * 3.14159265358979323846 / 180, whole_sweep = (double)sweep * 3.14159265358979323846 / 180, step, k;
    float x0, y0;
    int pieces, i;
    if (!(rx > 0) || !(ry > 0) || !isfinite(start) || !isfinite(sweep)) return;
    x0 = (float)(cx + rx * cos(a));
    y0 = (float)(cy + ry * sin(a));
    if (move || !p->any) path_move(p, x0, y0);
    else path_line(p, x0, y0);
    if (fabs((double)sweep) >= 360) {
        path_oval(p, l, t, r, b);
        path_move(p, x0, y0);
        return;
    }
    if (sweep == 0) return;
    /* Pieces of a quarter turn at most, each one cubic: off the true arc by less than three parts in ten thousand of the radius. */
    pieces = (int)ceil(fabs((double)sweep) / 90);
    step = whole_sweep / pieces;
    k = 4.0 / 3.0 * tan(step / 4);
    for (i = 0; i < pieces; i++) {
        double a1 = a + step, c0 = cos(a), s0 = sin(a), c1 = cos(a1), s1 = sin(a1);
        path_cubic(p, (float)(cx + rx * (c0 - k * s0)), (float)(cy + ry * (s0 + k * c0)), (float)(cx + rx * (c1 + k * s1)),
                   (float)(cy + ry * (s1 - k * c1)), (float)(cx + rx * c1), (float)(cy + ry * s1));
        a = a1;
    }
}

static void map_point(const float *m, float x, float y, float *ox, float *oy)
{
    *ox = m[0] * x + m[1] * y + m[2];
    *oy = m[3] * x + m[4] * y + m[5];
}

/* How many coordinates follow each verb of a built path. */
static int verb_floats(int verb) { return verb == MEGLER_GFX_PATH_QUAD ? 4 : verb == MEGLER_GFX_PATH_CUBIC ? 6 : verb == MEGLER_GFX_PATH_CLOSE ? 0 : 2; }

/* Everything in the path so far through a matrix: Path.transform, which the framework records as an operation. */
static void path_transform(gpath *p, const float *m)
{
    i32 i = 0, k;
    while (i < p->n) {
        int count = verb_floats((int)p->v[i++]);
        for (k = 0; k < count; k += 2) map_point(m, p->v[i + k], p->v[i + k + 1], &p->v[i + k], &p->v[i + k + 1]);
        i += count;
    }
    map_point(m, p->sx, p->sy, &p->sx, &p->sy);
    map_point(m, p->cx, p->cy, &p->cx, &p->cy);
}

enum {
    OP_MOVE, OP_LINE, OP_QUAD, OP_CUBIC, OP_CLOSE, OP_ARC, OP_RECT, OP_OVAL, OP_RRECT, OP_TRANSFORM, OP_RMOVE, OP_RLINE, OP_RQUAD, OP_RCUBIC, OP_RRECT8,
    OP_COUNT
};

/*
 * The framework's stream of operations into figures. It follows the original
 * step by step, its oddities included, since the Java keeps its own idea of
 * the last point beside it: a rectangle leaves that point at its (l, t), an
 * oval leaves it alone, and the direction flags of the oval and the round
 * rectangles are not used. A stream that ends in the middle of an operation,
 * or holds a number that is no operation, ends there.
 */
static void path_build(gpath *p, const float *f, i32 n)
{
    static const int operands[OP_COUNT] = { 2, 2, 4, 6, 0, 7, 5, 5, 7, 6, 2, 2, 4, 6, 13 };
    float lx = 0, ly = 0, ox, oy, rad[8];
    i32 i = 0;
    int op, k;
    path_init(p);
    while (i < n) {
        if (!(f[i] >= 0 && f[i] < OP_COUNT)) break;
        op = (int)f[i++];
        if (i + operands[op] > n) break;
        ox = op >= OP_RMOVE && op <= OP_RCUBIC ? lx : 0;
        oy = op >= OP_RMOVE && op <= OP_RCUBIC ? ly : 0;
        switch (op) {
        case OP_MOVE:
        case OP_RMOVE:
            lx = f[i] + ox;
            ly = f[i + 1] + oy;
            path_move(p, lx, ly);
            break;
        case OP_LINE:
        case OP_RLINE:
            lx = f[i] + ox;
            ly = f[i + 1] + oy;
            path_line(p, lx, ly);
            break;
        case OP_QUAD:
        case OP_RQUAD:
            path_quad(p, f[i] + ox, f[i + 1] + oy, f[i + 2] + ox, f[i + 3] + oy);
            lx = p->cx;
            ly = p->cy;
            break;
        case OP_CUBIC:
        case OP_RCUBIC:
            path_cubic(p, f[i] + ox, f[i + 1] + oy, f[i + 2] + ox, f[i + 3] + oy, f[i + 4] + ox, f[i + 5] + oy);
            lx = p->cx;
            ly = p->cy;
            break;
        case OP_CLOSE:
            if (p->any) {
                path_close(p);
                lx = p->cx;
                ly = p->cy;
            }
            break;
        case OP_ARC:
            path_arc(p, f[i], f[i + 1], f[i + 2], f[i + 3], f[i + 4], f[i + 5], f[i + 6] != 0);
            lx = p->cx;
            ly = p->cy;
            break;
        case OP_RECT: {
            float l = f[i], t = f[i + 1], r = f[i + 2], b = f[i + 3];
            if (f[i + 4] != 0) {
                path_move(p, l, t);
                path_line(p, l, b);
                path_line(p, r, b);
                path_line(p, r, t);
                path_close(p);
            } else {
                path_rect(p, l < r ? l : r, t < b ? t : b, l < r ? r : l, t < b ? b : t);
            }
            lx = l;
            ly = t;
            break;
        }
        case OP_OVAL:
            path_oval(p, f[i] < f[i + 2] ? f[i] : f[i + 2], f[i + 1] < f[i + 3] ? f[i + 1] : f[i + 3], f[i] < f[i + 2] ? f[i + 2] : f[i],
                      f[i + 1] < f[i + 3] ? f[i + 3] : f[i + 1]);
            break;
        case OP_RRECT:
            for (k = 0; k < 8; k++) rad[k] = f[i + 4 + (k & 1)];
            path_round_rect(p, f[i], f[i + 1], f[i + 2], f[i + 3], rad);
            break;
        case OP_RRECT8:
            path_round_rect(p, f[i], f[i + 1], f[i + 2], f[i + 3], f + i + 4);
            break;
        default: /* OP_TRANSFORM */
            path_transform(p, f + i);
            map_point(f + i, lx, ly, &lx, &ly);
            break;
        }
        i += operands[op];
    }
}

/* Whether anything of it would be drawn: at least one line or curve. */
static bool path_draws(const gpath *p)
{
    i32 i = 0;
    while (i < p->n) {
        int verb = (int)p->v[i++];
        if (verb != MEGLER_GFX_PATH_MOVE) return true;
        i += 2;
    }
    return false;
}

typedef struct gbox {
    float l, t, r, b;
    bool any;
} gbox;

static void box_x(gbox *o, float x)
{
    if (x < o->l) o->l = x;
    if (x > o->r) o->r = x;
}

static void box_y(gbox *o, float y)
{
    if (y < o->t) o->t = y;
    if (y > o->b) o->b = y;
}

static void box_point(gbox *o, float x, float y)
{
    if (!o->any) {
        o->l = o->r = x;
        o->t = o->b = y;
        o->any = true;
        return;
    }
    box_x(o, x);
    box_y(o, y);
}

/* Where one coordinate of a Bezier turns round between its ends, if it does: the points that can lie outside the end points' box. */
static int bezier_turns(int order, const float *v, double *at)
{
    int count = 0;
    if (order == 2) {
        double den = (double)v[0] - 2.0 * v[1] + v[2];
        if (den != 0) at[count++] = ((double)v[0] - v[1]) / den;
    } else {
        double a = -(double)v[0] + 3.0 * v[1] - 3.0 * v[2] + v[3], b = 2.0 * ((double)v[0] - 2.0 * v[1] + v[2]), k = (double)v[1] - v[0], disc;
        if (fabs(a) < 1e-12) {
            if (b != 0) at[count++] = -k / b;
        } else {
            disc = b * b - 4 * a * k;
            if (disc >= 0) {
                disc = sqrt(disc);
                at[count++] = (-b + disc) / (2 * a);
                at[count++] = (-b - disc) / (2 * a);
            }
        }
    }
    return count;
}

static float bezier_at(int order, const float *v, double t)
{
    double u = 1 - t;
    if (order == 2) return (float)(u * u * v[0] + 2 * u * t * v[1] + t * t * v[2]);
    return (float)(u * u * u * v[0] + 3 * u * u * t * v[1] + 3 * u * t * t * v[2] + t * t * t * v[3]);
}

/* The box round the path's lines and curves themselves, not round the curves' control points. A point nothing is drawn from does not count. */
static void path_bounds(const gpath *p, gbox *o)
{
    float cx = 0, cy = 0, sx = 0, sy = 0, xs[4], ys[4];
    double at[2];
    i32 i = 0;
    int k, turns, count, order;
    memset(o, 0, sizeof(*o));
    while (i < p->n) {
        int verb = (int)p->v[i++];
        const float *q = p->v + i;
        count = verb_floats(verb);
        i += count;
        if (verb == MEGLER_GFX_PATH_MOVE) {
            sx = cx = q[0];
            sy = cy = q[1];
            continue;
        }
        if (verb == MEGLER_GFX_PATH_CLOSE) {
            cx = sx;
            cy = sy;
            continue;
        }
        box_point(o, cx, cy);
        box_point(o, q[count - 2], q[count - 1]);
        if (verb != MEGLER_GFX_PATH_LINE) {
            order = count / 2;
            xs[0] = cx;
            ys[0] = cy;
            for (k = 0; k < order; k++) {
                xs[k + 1] = q[2 * k];
                ys[k + 1] = q[2 * k + 1];
            }
            turns = bezier_turns(order, xs, at);
            for (k = 0; k < turns; k++)
                if (at[k] > 0 && at[k] < 1) box_x(o, bezier_at(order, xs, at[k]));
            turns = bezier_turns(order, ys, at);
            for (k = 0; k < turns; k++)
                if (at[k] > 0 && at[k] < 1) box_y(o, bezier_at(order, ys, at[k]));
        }
        cx = q[count - 2];
        cy = q[count - 1];
    }
}

typedef void (*edge_fn)(void *ctx, float x0, float y0, float x1, float y1);

/*
 * The path as straight edges: a curve is sixteen chords, which is what the
 * original measures a path's length by. `closed` adds the edge that closes
 * every figure left open, for a question about the inside.
 */
static void path_edges(const gpath *p, bool closed, edge_fn fn, void *ctx)
{
    float cx = 0, cy = 0, sx = 0, sy = 0, xs[4], ys[4], px, py, x, y;
    bool open = false;
    i32 i = 0;
    int k, count, order;
    while (i < p->n) {
        int verb = (int)p->v[i++];
        const float *q = p->v + i;
        count = verb_floats(verb);
        i += count;
        if (verb == MEGLER_GFX_PATH_MOVE || verb == MEGLER_GFX_PATH_CLOSE) {
            if (open && (closed || verb == MEGLER_GFX_PATH_CLOSE)) fn(ctx, cx, cy, sx, sy);
            open = false;
            if (verb == MEGLER_GFX_PATH_MOVE) {
                sx = q[0];
                sy = q[1];
            }
            cx = sx;
            cy = sy;
            continue;
        }
        open = true;
        if (verb == MEGLER_GFX_PATH_LINE) {
            fn(ctx, cx, cy, q[0], q[1]);
        } else {
            order = count / 2;
            xs[0] = cx;
            ys[0] = cy;
            for (k = 0; k < order; k++) {
                xs[k + 1] = q[2 * k];
                ys[k + 1] = q[2 * k + 1];
            }
            px = cx;
            py = cy;
            for (k = 1; k <= 16; k++) {
                x = k == 16 ? q[count - 2] : bezier_at(order, xs, k / 16.0);
                y = k == 16 ? q[count - 1] : bezier_at(order, ys, k / 16.0);
                fn(ctx, px, py, x, y);
                px = x;
                py = y;
            }
        }
        cx = q[count - 2];
        cy = q[count - 1];
    }
    if (open && closed) fn(ctx, cx, cy, sx, sy);
}

typedef struct ghit {
    float x, y;
    i32 winding, crossings;
} ghit;

/* A ray from the point to the right: which edges it crosses, and which way they go. */
static void hit_edge(void *ctx, float x0, float y0, float x1, float y1)
{
    ghit *h = (ghit *)ctx;
    if ((y0 <= h->y) == (y1 <= h->y)) return;
    if ((double)x0 + ((double)h->y - y0) * ((double)x1 - x0) / ((double)y1 - y0) <= h->x) return;
    h->winding += y1 > y0 ? 1 : -1;
    h->crossings++;
}

static void length_edge(void *ctx, float x0, float y0, float x1, float y1)
{
    *(double *)ctx += hypot((double)x1 - x0, (double)y1 - y0);
}

/* The path of a native's arguments: the float[] and how much of it counts. False when there is no array. */
static bool path_of(hk_jobj *array, i32 n, gpath *p)
{
    const float *f = (const float *)elements(array, 'F', 0);
    if (!f) return false;
    if (n < 0) n = 0;
    if ((u32)n > length_of(array)) n = (i32)length_of(array);
    path_build(p, f, n);
    return true;
}

/* --------------------------------------------------------------- canvases */

static gcanvas *cv_of(i64 h)
{
    gcanvas *v = (gcanvas *)pointer(h);
    return v && v->magic == CV_MAGIC ? v : NULL;
}

static gstate *cv_state(gcanvas *v) { return &v->st[v->depth]; }

static void matrix_identity(float *m)
{
    m[0] = m[4] = 1;
    m[1] = m[2] = m[3] = m[5] = 0;
}

/* a then b as one matrix: a point goes through b first. That is Android's preConcat(b) on a. */
static void matrix_concat(const float *a, const float *b, float *out)
{
    float r[6];
    r[0] = a[0] * b[0] + a[1] * b[3];
    r[1] = a[0] * b[1] + a[1] * b[4];
    r[2] = a[0] * b[2] + a[1] * b[5] + a[2];
    r[3] = a[3] * b[0] + a[4] * b[3];
    r[4] = a[3] * b[1] + a[4] * b[4];
    r[5] = a[3] * b[2] + a[4] * b[5] + a[5];
    memcpy(out, r, sizeof(r));
}

static bool matrix_invert(const float *m, float *out)
{
    double det = (double)m[0] * m[4] - (double)m[1] * m[3];
    if (!(fabs(det) > 1e-30)) return false;
    out[0] = (float)(m[4] / det);
    out[1] = (float)(-m[1] / det);
    out[3] = (float)(-m[3] / det);
    out[4] = (float)(m[0] / det);
    out[2] = (float)(((double)m[1] * m[5] - (double)m[4] * m[2]) / det);
    out[5] = (float)(((double)m[3] * m[2] - (double)m[0] * m[5]) / det);
    return true;
}

/* The bounding box of a rectangle through a matrix. */
static void matrix_box(const float *m, float l, float t, float r, float b, float *out)
{
    gbox o;
    float x, y;
    memset(&o, 0, sizeof(o));
    map_point(m, l, t, &x, &y);
    box_point(&o, x, y);
    map_point(m, r, t, &x, &y);
    box_point(&o, x, y);
    map_point(m, r, b, &x, &y);
    box_point(&o, x, y);
    map_point(m, l, b, &x, &y);
    box_point(&o, x, y);
    out[0] = o.l;
    out[1] = o.t;
    out[2] = o.r;
    out[3] = o.b;
}

static void clip_intersect(gstate *s, const float *box)
{
    if (box[0] > s->clip[0]) s->clip[0] = box[0];
    if (box[1] > s->clip[1]) s->clip[1] = box[1];
    if (box[2] < s->clip[2]) s->clip[2] = box[2];
    if (box[3] < s->clip[3]) s->clip[3] = box[3];
    if (!(s->clip[0] < s->clip[2]) || !(s->clip[1] < s->clip[3])) s->empty = true;
}

/* A rectangle cut out of the clip changes the clip's box only when it takes a whole side of it away. */
static void clip_difference(gstate *s, const float *box)
{
    bool wide = box[0] <= s->clip[0] && box[2] >= s->clip[2], tall = box[1] <= s->clip[1] && box[3] >= s->clip[3];
    if (wide && tall) s->empty = true;
    else if (wide && box[1] <= s->clip[1] && box[3] > s->clip[1]) s->clip[1] = box[3];
    else if (wide && box[3] >= s->clip[3] && box[1] < s->clip[3]) s->clip[3] = box[1];
    else if (tall && box[0] <= s->clip[0] && box[2] > s->clip[0]) s->clip[0] = box[2];
    else if (tall && box[2] >= s->clip[2] && box[0] < s->clip[2]) s->clip[2] = box[0];
}

/* The shell's canvas is given the matrix when it is not the one it has. */
static void cv_send_matrix(gcanvas *v, const float *m)
{
    if (!v->host || memcmp(v->sent, m, sizeof(v->sent)) == 0) return;
    memcpy(v->sent, m, sizeof(v->sent));
    if (HAS(set_matrix)) GFX(G.set_matrix(G.user, v->host, m));
}

/* Whether a drawing call through the canvas now reaches every pixel of the bitmap: nothing saved, so no layer, and nothing taken from the clip. */
static bool cv_whole(gcanvas *v) { return v->depth == 0 && !v->st[0].clipped && !v->st[0].empty; }

/*
 * The shell's canvas is about to change the picture. What other canvases of
 * the same bitmap have drawn is put into the memory first, this one is told
 * when the memory changed behind its back, and the others will be told
 * before they draw again. `replaces`: the call puts a colour of its own in
 * every pixel, so what the memory holds does not matter and the shell is not
 * made to take it back for nothing.
 */
static void cv_changes(gcanvas *v, bool replaces)
{
    gcanvas *o;
    hk_lock_acquire(&g_lock);
    for (o = v->bm->canvases; o; o = o->next) {
        if (o == v) continue;
        if (o->dirty) {
            if (o->host && G.flush) GFX(G.flush(G.user, o->host));
            o->dirty = false;
            v->stale = true;
        }
        o->stale = true;
    }
    if (v->stale) {
        if (G.touched && !replaces) GFX(G.touched(G.user, v->host));
        v->stale = false;
    }
    v->dirty = true;
    v->bm->version++;
    hk_lock_release(&g_lock);
}

/* Before a drawing call: whether anything can be drawn through the canvas now, and if so the shell's canvas made ready for it. */
static bool cv_draw(gcanvas *v, bool replaces)
{
    if (!v || !v->bm || !v->host || cv_state(v)->empty) return false;
    cv_changes(v, replaces);
    return true;
}

static i32 cv_save(gcanvas *v, const float *bounds, bool layer, int alpha, int blend)
{
    gstate *s;
    if (v->depth + 1 >= v->cap) {
        v->cap = v->cap * 2 + 8;
        v->st = (gstate *)hk_realloc(v->st, sizeof(gstate) * (size_t)v->cap);
    }
    v->st[v->depth + 1] = v->st[v->depth];
    v->depth++;
    s = cv_state(v);
    s->layer = layer;
    if (bounds) {
        float box[4];
        matrix_box(s->m, bounds[0], bounds[1], bounds[2], bounds[3], box);
        clip_intersect(s, box);
        s->clipped = true;
    }
    if (v->host) {
        /* A layer the shell cannot make is still a save: the restores must go on matching. */
        if (layer && HAS(save_layer)) GFX(G.save_layer(G.user, v->host, bounds, alpha, blend));
        else if (HAS(save)) GFX(G.save(G.user, v->host));
    }
    /* What Canvas.save returns: the count before it, the number restoreToCount takes to undo it. */
    return v->depth;
}

static void cv_restore(gcanvas *v)
{
    bool layer;
    if (v->depth <= 0) return;
    layer = cv_state(v)->layer;
    v->depth--;
    if (v->host) {
        /* A layer ending is drawing: it is put onto the picture now. */
        if (layer && v->bm) cv_changes(v, false);
        if (HAS(restore)) GFX(G.restore(G.user, v->host));
        cv_send_matrix(v, cv_state(v)->m);
    }
}

static void cv_free(gcanvas *v)
{
    gcanvas **link;
    while (v->depth > 0) cv_restore(v);
    hk_lock_acquire(&g_lock);
    if (v->bm) {
        for (link = &v->bm->canvases; *link; link = &(*link)->next)
            if (*link == v) {
                *link = v->next;
                break;
            }
    }
    if (v->host) {
        if (v->dirty && G.flush) GFX(G.flush(G.user, v->host));
        if (G.canvas_free) GFX(G.canvas_free(G.user, v->host));
    }
    hk_lock_release(&g_lock);
    v->magic = 0;
    free(v->st);
    free(v);
}

/* ------------------------------------------------------------ Gfx: bitmaps */

N(G_bmNew) { c->ret.j = handle(bm_new(c->args[0].i, c->args[1].i)); }

N(G_bmFree)
{
    gbitmap *b = bm_of(c->args[0].j);
    if (b) bm_free(b);
}

/*
 * (long bm, int argb): Bitmap.eraseColor. The framework erases the screen's
 * bitmap before every frame and then draws the views into it through its
 * canvas. Where a canvas of the bitmap can do the erasing, it is drawing
 * like the rest; otherwise a shell with a surface of its own would be made
 * to take the whole memory back once a frame, only to draw all over it.
 */
N(G_bmErase)
{
    gbitmap *b = bm_of(c->args[0].j);
    u32 v = premultiplied((u32)c->args[1].i), *row;
    gcanvas *through;
    i32 x, y;
    if (!b) return;
    hk_lock_acquire(&g_lock);
    for (through = b->canvases; through; through = through->next)
        if (through->host && cv_whole(through)) break;
    hk_lock_release(&g_lock);
    if (through && G.draw_color && cv_draw(through, true)) {
        GFX(G.draw_color(G.user, through->host, (u32)c->args[1].i, MEGLER_GFX_BLEND_SRC));
        return;
    }
    bm_settle(b);
    if (!v) {
        memset(b->px, 0, (size_t)b->stride * (size_t)b->h);
    } else {
        for (y = 0; y < b->h; y++) {
            row = bm_row(b, y);
            for (x = 0; x < b->w; x++) row[x] = v;
        }
    }
    bm_changed(b);
}

N(G_bmGetPixel)
{
    gbitmap *b = bm_of(c->args[0].j);
    i32 x = c->args[1].i, y = c->args[2].i;
    if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    bm_settle(b);
    c->ret.i = (i32)unpremultiplied(bm_row(b, y)[x]);
}

N(G_bmSetPixel)
{
    gbitmap *b = bm_of(c->args[0].j);
    i32 x = c->args[1].i, y = c->args[2].i;
    if (!b || x < 0 || y < 0 || x >= b->w || y >= b->h) return;
    bm_settle(b);
    bm_row(b, y)[x] = premultiplied((u32)c->args[3].i);
    bm_changed(b);
}

/* (long bm, int[] px, int offset, int stride, int x, int y, int w, int h, boolean set): Bitmap.getPixels and setPixels. The stride may be negative. */
N(G_bmPixels)
{
    gbitmap *b = bm_of(c->args[0].j);
    i32 *px = (i32 *)elements(c->args[1].l, 'I', 0);
    i64 len = length_of(c->args[1].l), off = c->args[2].i, stride = c->args[3].i, x = c->args[4].i, y = c->args[5].i, w = c->args[6].i, h = c->args[7].i;
    i64 first, last, r, k;
    bool set = c->args[8].z != 0;
    if (!b || !px || x < 0 || y < 0 || w < 0 || h < 0 || x + w > b->w || y + h > b->h) {
        hk_dvm_throw("java/lang/IllegalArgumentException", "pixels out of bounds");
        return;
    }
    if (!w || !h) return;
    /* The rows' places in the array run evenly from the first to the last, so those two say whether every one is inside it. */
    first = off;
    last = off + (h - 1) * stride;
    if (first < 0 || last < 0 || first + w > len || last + w > len) {
        hk_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "pixels");
        return;
    }
    bm_settle(b);
    for (r = 0; r < h; r++) {
        u32 *row = bm_row(b, (i32)(y + r)) + (size_t)x;
        i32 *at = px + (size_t)(off + r * stride);
        if (set) {
            for (k = 0; k < w; k++) row[k] = premultiplied((u32)at[k]);
        } else {
            for (k = 0; k < w; k++) at[k] = (i32)unpremultiplied(row[k]);
        }
    }
    if (set) bm_changed(b);
}

/*
 * (long bm, long address, boolean set): all the pixels to or from native
 * memory, as copyPixelsToBuffer and copyPixelsFromBuffer mean them and as GL
 * takes a texture: premultiplied and in Android's order, R, G, B, A. The
 * address is a direct buffer's or a native producer's, with room for all of
 * it; that is the Java's to see to.
 */
N(G_bmRaw)
{
    gbitmap *b = bm_of(c->args[0].j);
    u8 *at = (u8 *)pointer(c->args[1].j);
    if (!b || !at) return;
    bm_settle(b);
    if (c->args[2].z) {
        swap_red_blue(b->px, at, (size_t)b->w * (size_t)b->h);
        bm_changed(b);
    } else {
        swap_red_blue(at, b->px, (size_t)b->w * (size_t)b->h);
    }
}

/* Nothing in the framework's Java calls it. What it gives is the memory as it is here, B, G, R, A. */
N(G_bmAddress)
{
    gbitmap *b = bm_of(c->args[0].j);
    if (!b) return;
    bm_settle(b);
    c->ret.j = handle(b->px);
}

/*
 * (byte[] data, int offset, int length, int sample, int[] outWH, boolean boundsOnly) -> long
 *
 * An image file into a new bitmap. `sample` is BitmapFactory's inSampleSize,
 * a power of two the framework has made of what the app asked: the result is
 * the picture's own size divided by it, rounded down, at least one pixel.
 * outWH gets that size. boundsOnly: the size and no bitmap. 0 when the bytes
 * are no image the shell can read, and then outWH is left alone.
 */
N(G_bmDecode)
{
    const u8 *data = (const u8 *)elements(c->args[0].l, 'B', 1);
    i64 len = length_of(c->args[0].l), off = c->args[1].i, n = c->args[2].i;
    i32 sample = c->args[3].i, *wh = (i32 *)elements(c->args[4].l, 'I', 2);
    int w = 0, h = 0;
    gbitmap *b;
    bool webp, decoded = false;
    if (!data || off < 0 || n <= 0 || off + n > len) return;
    if (sample < 1) sample = 1;
    data += (size_t)off;
    /*
     * A WebP picture is the core's own to decode (webp/hk_webp.h): the phone's decoder has no WebP. In a build without
     * the decoder it goes to the shell like any other picture, for whatever the shell can make of it.
     */
    webp = hk_webp_is(data, (size_t)n);
    if (webp && !hk_webp_available()) {
        hk_note_once("java: the app has WebP pictures, and this build of the layer has no WebP decoder in it (build/webp: tools/webp-fetch.sh): they are left to the shell's");
        webp = false;
    }
    if (webp) {
        if (!hk_webp_size(data, (size_t)n, &w, &h)) return;
    } else {
        bool sized = false;
        if (HAS(image_size)) GFX(sized = G.image_size(G.user, data, (size_t)n, &w, &h));
        if (!sized || w <= 0 || h <= 0) return;
    }
    w = w / sample > 0 ? w / sample : 1;
    h = h / sample > 0 ? h / sample : 1;
    if (c->args[5].z) {
        if (wh) {
            wh[0] = w;
            wh[1] = h;
        }
        return;
    }
    if (!webp && !HAS(image_decode)) return;
    b = bm_new(w, h);
    if (!b) return;
    if (webp) decoded = hk_webp_decode(data, (size_t)n, b->px, b->w, b->h, b->stride);
    else GFX(decoded = G.image_decode(G.user, data, (size_t)n, b->px, b->w, b->h, b->stride));
    if (!decoded) {
        bm_free(b);
        return;
    }
    if (wh) {
        wh[0] = w;
        wh[1] = h;
    }
    c->ret.j = handle(b);
}

typedef struct gbytes {
    u8 *data;
    size_t size, cap;
} gbytes;

static bool bytes_write(void *sink, const void *data, size_t size)
{
    gbytes *o = (gbytes *)sink;
    u8 *grown;
    if (size > 0x7fffffffu - o->size) return false;
    if (o->size + size > o->cap) {
        size_t cap = (o->size + size) * 2 + 4096;
        grown = (u8 *)realloc(o->data, cap);
        if (!grown) return false;
        o->data = grown;
        o->cap = cap;
    }
    if (size) memcpy(o->data + o->size, data, size);
    o->size += size;
    return true;
}

/* (long bm, int format, int quality) -> byte[]: Bitmap.compress. The framework passes 0 for JPEG and 1 for everything else, which is written as PNG. */
N(G_bmCompress)
{
    gbitmap *b = bm_of(c->args[0].j);
    i32 quality = c->args[2].i;
    gbytes out;
    hk_jobj *array;
    bool encoded = false;
    if (!b || !HAS(image_encode)) return;
    memset(&out, 0, sizeof(out));
    bm_settle(b);
    if (quality < 0) quality = 0;
    if (quality > 100) quality = 100;
    GFX(encoded = G.image_encode(G.user, b->px, b->w, b->h, b->stride, c->args[1].i == 0 ? MEGLER_GFX_JPEG : MEGLER_GFX_PNG, quality, bytes_write, &out));
    if (encoded && out.size) {
        array = hk_dvm_new_array("[B", (i32)out.size);
        if (array) memcpy(array->u.arr.data, out.data, out.size);
        c->ret.l = array;
    }
    free(out.data);
}

/* A turned or slanted copy is drawing, which is the shell's: through a canvas made for the one call. */
static void bm_copy_drawn(gbitmap *s, i32 x, i32 y, i32 w, i32 h, const float *m, bool filter, gbitmap *d)
{
    megler_gfx_image image;
    megler_gfx_paint p;
    gpaint def;
    float src[4], dst[4];
    void *cv;
    if (!HAS(canvas_new) || !HAS(draw_bitmap)) return;
    GFX(cv = G.canvas_new(G.user, d->px, d->w, d->h, d->stride));
    if (!cv) return;
    paint_of(NULL, &def);
    p = def.p;
    p.flags = filter ? MEGLER_GFX_ANTI_ALIAS | MEGLER_GFX_FILTER_BITMAP : 0;
    image_of(s, &image);
    src[0] = (float)x;
    src[1] = (float)y;
    src[2] = (float)(x + w);
    src[3] = (float)(y + h);
    dst[0] = dst[1] = 0;
    dst[2] = (float)w;
    dst[3] = (float)h;
    if (G.set_matrix) GFX(G.set_matrix(G.user, cv, m));
    GFX(G.draw_bitmap(G.user, cv, &image, src, dst, &p));
    if (G.flush) GFX(G.flush(G.user, cv));
    if (G.canvas_free) GFX(G.canvas_free(G.user, cv));
}

/*
 * (long src, int x, int y, int w, int h, float[] matrix or null, boolean filter) -> long
 *
 * Bitmap.createBitmap(source, x, y, w, h, matrix, filter): a new bitmap of
 * that part of the source, through the matrix. Its size is the box of the
 * part through the matrix, rounded, as the Java works it out for itself.
 * Without a matrix it is a copy; with one that only scales and mirrors,
 * which is createScaledBitmap and every picture a resource fits to the
 * screen's density, it is scaled here; anything else is drawn by the shell.
 */
N(G_bmCopy)
{
    gbitmap *s = bm_of(c->args[0].j), *d;
    i64 x = c->args[1].i, y = c->args[2].i, w = c->args[3].i, h = c->args[4].i;
    const float *m = (const float *)elements(c->args[5].l, 'F', 6);
    float box[4], moved[6];
    i32 r, nw, nh;
    bool filter = c->args[6].z != 0;
    if (!s || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > s->w || y + h > s->h) return;
    bm_settle(s);
    if (!m) {
        d = bm_new(w, h);
        if (!d) return;
        for (r = 0; r < (i32)h; r++) memcpy(bm_row(d, r), bm_row(s, (i32)(y + r)) + (size_t)x, (size_t)w * 4);
        c->ret.j = handle(d);
        return;
    }
    matrix_box(m, 0, 0, (float)w, (float)h, box);
    nw = whole(floor((double)box[2] - box[0] + 0.5));
    nh = whole(floor((double)box[3] - box[1] + 0.5));
    d = bm_new(nw > 0 ? nw : 1, nh > 0 ? nh : 1);
    if (!d) return;
    if (m[1] == 0 && m[3] == 0 && m[0] != 0 && m[4] != 0 && isfinite(m[0]) && isfinite(m[4])) {
        if (!bm_scale(s, (i32)x, (i32)y, (i32)w, (i32)h, d, m[0] < 0, m[4] < 0, filter)) {
            bm_free(d);
            return;
        }
    } else {
        memcpy(moved, m, sizeof(moved));
        moved[2] -= box[0];
        moved[5] -= box[1];
        bm_copy_drawn(s, (i32)x, (i32)y, (i32)w, (i32)h, moved, filter, d);
    }
    c->ret.j = handle(d);
}

/* ----------------------------------------------------------- Gfx: canvases */

N(G_cvNew)
{
    gbitmap *b = bm_of(c->args[0].j);
    gcanvas *v;
    gstate *s;
    if (!b) return;
    v = (gcanvas *)hk_alloc(sizeof(*v));
    v->magic = CV_MAGIC;
    v->bm = b;
    v->cap = 8;
    v->st = (gstate *)hk_alloc(sizeof(gstate) * (size_t)v->cap);
    s = cv_state(v);
    matrix_identity(s->m);
    matrix_identity(v->sent);
    s->clip[2] = (float)b->w;
    s->clip[3] = (float)b->h;
    /* What other canvases have drawn is in the memory before the shell reads it as this one's picture. */
    bm_settle(b);
    /* Without the shell's half the canvas still counts saves and keeps its matrix and clip, so that the Java's bookkeeping holds; it draws nothing. */
    if (HAS(canvas_new)) GFX(v->host = G.canvas_new(G.user, b->px, b->w, b->h, b->stride));
    hk_lock_acquire(&g_lock);
    v->next = b->canvases;
    b->canvases = v;
    hk_lock_release(&g_lock);
    c->ret.j = handle(v);
}

N(G_cvFree)
{
    gcanvas *v = cv_of(c->args[0].j);
    if (v) cv_free(v);
}

N(G_cvSave)
{
    gcanvas *v = cv_of(c->args[0].j);
    c->ret.i = v ? cv_save(v, NULL, false, 255, MEGLER_GFX_BLEND_SRC_OVER) : 1;
}

/* (long, l, t, r, b, int alpha[, int mode]): saveLayerAlpha, and saveLayer with a Paint's alpha and transfer mode. A rectangle with no area is "no bounds". */
static void save_layer(hk_jcall *c, int mode)
{
    gcanvas *v = cv_of(c->args[0].j);
    float bounds[4];
    i32 alpha = c->args[5].i;
    bool bounded;
    if (!v) {
        c->ret.i = 1;
        return;
    }
    bounds[0] = c->args[1].f;
    bounds[1] = c->args[2].f;
    bounds[2] = c->args[3].f;
    bounds[3] = c->args[4].f;
    bounded = bounds[2] - bounds[0] > 0 && bounds[3] - bounds[1] > 0;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    c->ret.i = cv_save(v, bounded ? bounds : NULL, true, alpha, mode);
}

N(G_cvSaveLayer) { save_layer(c, MEGLER_GFX_BLEND_SRC_OVER); }
N(G_cvSaveLayerMode) { save_layer(c, blend_number(c->args[6].i)); }

N(G_cvRestore)
{
    gcanvas *v = cv_of(c->args[0].j);
    if (v) cv_restore(v);
}

N(G_cvRestoreTo)
{
    gcanvas *v = cv_of(c->args[0].j);
    i32 count = c->args[1].i;
    if (!v) return;
    while (v->depth + 1 > count && v->depth > 0) cv_restore(v);
}

N(G_cvSaveCount)
{
    gcanvas *v = cv_of(c->args[0].j);
    c->ret.i = v ? v->depth + 1 : 1;
}

/* (long, float[6], boolean set): Canvas.setMatrix, or concat and with it translate, scale, rotate and skew. */
N(G_cvMatrix)
{
    gcanvas *v = cv_of(c->args[0].j);
    const float *m = (const float *)elements(c->args[1].l, 'F', 6);
    gstate *s;
    int i;
    if (!v || !m) return;
    for (i = 0; i < 6; i++)
        if (!isfinite(m[i])) return;
    s = cv_state(v);
    if (c->args[2].z) memcpy(s->m, m, sizeof(s->m));
    else matrix_concat(s->m, m, s->m);
    cv_send_matrix(v, s->m);
}

N(G_cvGetMatrix)
{
    gcanvas *v = cv_of(c->args[0].j);
    float *m = (float *)elements(c->args[1].l, 'F', 6);
    if (v && m) memcpy(m, cv_state(v)->m, sizeof(float) * 6);
}

/* (long, l, t, r, b, int op: 1 intersect, 0 difference) -> whether anything is left of the clip. The Java passes no other op. */
N(G_cvClipRect)
{
    gcanvas *v = cv_of(c->args[0].j);
    float l = c->args[1].f, t = c->args[2].f, r = c->args[3].f, b = c->args[4].f, box[4], swap;
    int op = c->args[5].i == 0 ? MEGLER_GFX_CLIP_DIFFERENCE : MEGLER_GFX_CLIP_INTERSECT;
    gstate *s;
    c->ret.z = 1;
    if (!v) return;
    s = cv_state(v);
    if (l > r) {
        swap = l;
        l = r;
        r = swap;
    }
    if (t > b) {
        swap = t;
        t = b;
        b = swap;
    }
    matrix_box(s->m, l, t, r, b, box);
    s->clipped = true;
    if (op == MEGLER_GFX_CLIP_INTERSECT) clip_intersect(s, box);
    else if (s->m[1] == 0 && s->m[3] == 0) clip_difference(s, box);
    if (v->host && HAS(clip_rect)) GFX(G.clip_rect(G.user, v->host, l, t, r, b, op));
    c->ret.z = !s->empty;
}

/* (long, float[] path, int n, int fill, int op) -> whether anything is left of the clip */
N(G_cvClipPath)
{
    gcanvas *v = cv_of(c->args[0].j);
    int fill = c->args[3].i == 1 ? MEGLER_GFX_EVEN_ODD : MEGLER_GFX_WINDING, op = c->args[4].i == 0 ? MEGLER_GFX_CLIP_DIFFERENCE : MEGLER_GFX_CLIP_INTERSECT;
    gstate *s;
    gpath p;
    gbox o;
    float box[4];
    c->ret.z = 1;
    if (!v || !path_of(c->args[1].l, c->args[2].i, &p)) return;
    s = cv_state(v);
    s->clipped = true;
    if (op == MEGLER_GFX_CLIP_INTERSECT) {
        path_bounds(&p, &o);
        /* Nothing is inside a path with nothing in it. */
        if (!o.any) {
            s->empty = true;
        } else {
            matrix_box(s->m, o.l, o.t, o.r, o.b, box);
            clip_intersect(s, box);
        }
    }
    if (v->host && HAS(clip_path)) GFX(G.clip_path(G.user, v->host, p.v, p.n, fill, op));
    path_free(&p);
    c->ret.z = !s->empty;
}

/*
 * (long, int[4] out) -> boolean: the clip's box in the drawing's own
 * coordinates, whole numbers that take all of it in; false and zeros when
 * the clip is empty. Under a matrix that rotates it is the box of the box.
 */
N(G_cvClipBounds)
{
    gcanvas *v = cv_of(c->args[0].j);
    i32 *out = (i32 *)elements(c->args[1].l, 'I', 4);
    float inverse[6], box[4];
    gstate *s;
    if (!out) return;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!v) return;
    s = cv_state(v);
    if (s->empty || !matrix_invert(s->m, inverse)) return;
    matrix_box(inverse, s->clip[0], s->clip[1], s->clip[2], s->clip[3], box);
    out[0] = whole(floor(box[0]));
    out[1] = whole(floor(box[1]));
    out[2] = whole(ceil(box[2]));
    out[3] = whole(ceil(box[3]));
    c->ret.z = 1;
}

/* (long, int color, int mode): Canvas.drawColor. */
N(G_cvDrawColor)
{
    gcanvas *v = cv_of(c->args[0].j);
    int blend = blend_number(c->args[2].i);
    bool replaces = blend == MEGLER_GFX_BLEND_SRC || blend == MEGLER_GFX_BLEND_CLEAR;
    if (blend == MEGLER_GFX_BLEND_DST || !v || !v->host || !HAS(draw_color) || !cv_draw(v, replaces && cv_whole(v))) return;
    GFX(G.draw_color(G.user, v->host, (u32)c->args[1].i, blend));
}

static void draw_rect(gcanvas *v, float l, float t, float r, float b, const gpaint *p, void *shader)
{
    float swap;
    if (l > r) {
        swap = l;
        l = r;
        r = swap;
    }
    if (t > b) {
        swap = t;
        t = b;
        b = swap;
    }
    if (!v || !v->host || !HAS(draw_rect) || !cv_draw(v, false)) return;
    GFX(G.draw_rect(G.user, v->host, l, t, r, b, &p->p, shader));
}

static void draw_path(gcanvas *v, const gpath *path, int fill, const gpaint *p, void *shader)
{
    if (!v || !v->host || !path_draws(path) || !HAS(draw_path) || !cv_draw(v, false)) return;
    GFX(G.draw_path(G.user, v->host, path->v, path->n, fill, &p->p, shader));
}

/* (long, l, t, r, b, int[] paint, long shader) */
N(G_cvDrawRect)
{
    void *shader = pointer(c->args[6].j);
    gpaint p;
    if (paint_for(c->args[5].l, !shader, &p)) draw_rect(cv_of(c->args[0].j), c->args[1].f, c->args[2].f, c->args[3].f, c->args[4].f, &p, shader);
}

/* (long, l, t, r, b, rx, ry, int[] paint, long shader) */
N(G_cvDrawRRect)
{
    gcanvas *v = cv_of(c->args[0].j);
    float l = c->args[1].f, t = c->args[2].f, r = c->args[3].f, b = c->args[4].f, rx = c->args[5].f, ry = c->args[6].f, swap;
    void *shader = pointer(c->args[8].j);
    gpaint p;
    if (!paint_for(c->args[7].l, !shader, &p)) return;
    if (l > r) {
        swap = l;
        l = r;
        r = swap;
    }
    if (t > b) {
        swap = t;
        t = b;
        b = swap;
    }
    if (rx > (r - l) / 2) rx = (r - l) / 2;
    if (ry > (b - t) / 2) ry = (b - t) / 2;
    /* Without both radii the corners are square. */
    if (!(rx > 0) || !(ry > 0)) {
        draw_rect(v, l, t, r, b, &p, shader);
        return;
    }
    if (!v || !v->host || !HAS(draw_round_rect) || !cv_draw(v, false)) return;
    GFX(G.draw_round_rect(G.user, v->host, l, t, r, b, rx, ry, &p.p, shader));
}

/* (long, l, t, r, b, int[] paint, long shader) */
N(G_cvDrawOval)
{
    gcanvas *v = cv_of(c->args[0].j);
    float l = c->args[1].f, t = c->args[2].f, r = c->args[3].f, b = c->args[4].f, swap;
    void *shader = pointer(c->args[6].j);
    gpaint p;
    if (!paint_for(c->args[5].l, !shader, &p)) return;
    if (l > r) {
        swap = l;
        l = r;
        r = swap;
    }
    if (t > b) {
        swap = t;
        t = b;
        b = swap;
    }
    if (!v || !v->host || !HAS(draw_oval) || !cv_draw(v, false)) return;
    GFX(G.draw_oval(G.user, v->host, l, t, r, b, &p.p, shader));
}

/* (long, l, t, r, b, float start, float sweep, boolean useCenter, int[] paint, long shader): the arc alone, or the wedge to the oval's centre. */
N(G_cvDrawArc)
{
    float l = c->args[1].f, t = c->args[2].f, r = c->args[3].f, b = c->args[4].f;
    void *shader = pointer(c->args[9].j);
    gpaint p;
    gpath path;
    if (!paint_for(c->args[8].l, !shader, &p)) return;
    path_init(&path);
    if (c->args[7].z) {
        path_move(&path, (l + r) / 2, (t + b) / 2);
        path_arc(&path, l, t, r, b, c->args[5].f, c->args[6].f, false);
        path_close(&path);
    } else {
        path_arc(&path, l, t, r, b, c->args[5].f, c->args[6].f, true);
    }
    draw_path(cv_of(c->args[0].j), &path, MEGLER_GFX_WINDING, &p, shader);
    path_free(&path);
}

/* (long, float[] path, int n, int fill, int[] paint, long shader) */
N(G_cvDrawPath)
{
    void *shader = pointer(c->args[5].j);
    gpaint p;
    gpath path;
    if (!paint_for(c->args[4].l, !shader, &p) || !path_of(c->args[1].l, c->args[2].i, &path)) return;
    draw_path(cv_of(c->args[0].j), &path, c->args[3].i == 1 ? MEGLER_GFX_EVEN_ODD : MEGLER_GFX_WINDING, &p, shader);
    path_free(&path);
}

/*
 * (long, float[] pts, int offset, int count, int[] paint, long shader, boolean points)
 *
 * drawLines: every four floats a line, stroked whatever the paint's style.
 * drawPoints: every two floats a point as wide as the stroke -- a square, or
 * a circle when the cap is round --, filled. A hairline point is a pixel.
 */
N(G_cvDrawLines)
{
    gcanvas *v = cv_of(c->args[0].j);
    const float *f = (const float *)elements(c->args[1].l, 'F', 0);
    i64 off = c->args[2].i, n = c->args[3].i, i;
    void *shader = pointer(c->args[5].j);
    gpaint p;
    gpath path;
    float half;
    if (!v || !f || off < 0 || n < 0 || off + n > length_of(c->args[1].l) || !paint_for(c->args[4].l, !shader, &p)) return;
    path_init(&path);
    if (c->args[6].z) {
        const float *m = cv_state(v)->m;
        double det = fabs((double)m[0] * m[4] - (double)m[1] * m[3]);
        half = p.p.stroke_width > 0 ? p.p.stroke_width / 2 : det > 0 ? (float)(0.5 / sqrt(det)) : 0.5f;
        for (i = off; i + 1 < off + n; i += 2) {
            if (p.p.cap == MEGLER_GFX_CAP_ROUND) path_oval(&path, f[i] - half, f[i + 1] - half, f[i] + half, f[i + 1] + half);
            else path_rect(&path, f[i] - half, f[i + 1] - half, f[i] + half, f[i + 1] + half);
        }
        p.p.style = MEGLER_GFX_FILL;
    } else {
        for (i = off; i + 3 < off + n; i += 4) {
            path_move(&path, f[i], f[i + 1]);
            path_line(&path, f[i + 2], f[i + 3]);
        }
        p.p.style = MEGLER_GFX_STROKE;
    }
    draw_path(v, &path, MEGLER_GFX_WINDING, &p, shader);
    path_free(&path);
}

/*
 * A part of one bitmap onto a rectangle of a canvas. As Android has it: a
 * source or a destination with no area draws nothing, and a source that
 * reaches outside the bitmap is cut to it, the destination with it.
 */
static void draw_bitmap(gcanvas *v, gbitmap *b, float *src, float *dst, hk_jobj *paint)
{
    megler_gfx_image image;
    gpaint p;
    float sw = src[2] - src[0], sh = src[3] - src[1], dw = dst[2] - dst[0], dh = dst[3] - dst[1], cut;
    if (!v || !v->host || !b || !(sw > 0) || !(sh > 0) || !(dw > 0) || !(dh > 0) || !paint_for(paint, false, &p)) return;
    /* drawBitmap without a Paint samples smoothly, as the original has it. */
    if (!paint) p.p.flags |= MEGLER_GFX_FILTER_BITMAP;
    if (src[0] < 0) {
        cut = -src[0];
        src[0] = 0;
        dst[0] += cut * dw / sw;
    }
    if (src[1] < 0) {
        cut = -src[1];
        src[1] = 0;
        dst[1] += cut * dh / sh;
    }
    if (src[2] > (float)b->w) {
        cut = src[2] - (float)b->w;
        src[2] = (float)b->w;
        dst[2] -= cut * dw / sw;
    }
    if (src[3] > (float)b->h) {
        cut = src[3] - (float)b->h;
        src[3] = (float)b->h;
        dst[3] -= cut * dh / sh;
    }
    if (!(src[2] > src[0]) || !(src[3] > src[1]) || !(dst[2] > dst[0]) || !(dst[3] > dst[1]) || !HAS(draw_bitmap)) return;
    /* What is drawn from must be in the memory: also when it is the canvas's own bitmap. */
    bm_settle(b);
    if (!cv_draw(v, false)) return;
    image_of(b, &image);
    GFX(G.draw_bitmap(G.user, v->host, &image, src, dst, &p.p));
}

/* (long cv, long bm, sl, st, sr, sb, dl, dt, dr, db, int[] paint or null) */
N(G_cvDrawBitmap)
{
    float src[4], dst[4];
    int i;
    for (i = 0; i < 4; i++) {
        src[i] = c->args[2 + i].f;
        dst[i] = c->args[6 + i].f;
    }
    draw_bitmap(cv_of(c->args[0].j), bm_of(c->args[1].j), src, dst, c->args[10].l);
}

/* (long cv, long bm, float[6] matrix, int[] paint or null): the whole bitmap through a matrix of its own, which is the canvas's for the one call. */
N(G_cvDrawBitmapM)
{
    gcanvas *v = cv_of(c->args[0].j);
    gbitmap *b = bm_of(c->args[1].j);
    const float *m = (const float *)elements(c->args[2].l, 'F', 6);
    float src[4], dst[4], through[6];
    int i;
    if (!v || !v->host || !b || !m) return;
    for (i = 0; i < 6; i++)
        if (!isfinite(m[i])) return;
    src[0] = src[1] = dst[0] = dst[1] = 0;
    src[2] = dst[2] = (float)b->w;
    src[3] = dst[3] = (float)b->h;
    matrix_concat(cv_state(v)->m, m, through);
    cv_send_matrix(v, through);
    draw_bitmap(v, b, src, dst, c->args[3].l);
    cv_send_matrix(v, cv_state(v)->m);
}

/* -------------------------------------------------------------- Gfx: text */

static gface *tf_of(i64 h)
{
    gface *f = (gface *)pointer(h);
    return f && f->magic == TF_MAGIC ? f : NULL;
}

static void *tf_host(i64 h)
{
    gface *f = tf_of(h);
    return f ? f->host : NULL;
}

/* The part of a string a native is to work on, with the original's forgiveness for a range that is not one. NULL for no string. */
static const u16 *text_of(hk_jobj *s, i32 start, i32 end, i32 *count)
{
    i32 n = 0;
    const u16 *u = s ? hk_dvm_string_utf16(s, &n) : NULL;
    *count = 0;
    if (!u) return NULL;
    if (start < 0) start = 0;
    if (end > n || end < 0) end = n;
    if (end < start) end = start;
    *count = end - start;
    return u + start;
}

static float text_width(const u16 *text, i32 count, const gpaint *p, void *face)
{
    float width = 0;
    if (count <= 0 || !HAS(text_measure)) return 0;
    GFX(width = G.text_measure(G.user, text, count, &p->p, face));
    return number(width, 0);
}

/*
 * (long cv, String, int start, int end, float x, float y, int[] paint, long typeface, long shader)
 *
 * Canvas.drawText: the part of the string as one run, its baseline at y and
 * x by the paint's alignment. An underline and a strike-through are bars
 * drawn here, where the original puts them: a ninth of the text's size below
 * the baseline and three tenths above it, an eighteenth thick.
 */
N(G_cvDrawText)
{
    gcanvas *v = cv_of(c->args[0].j);
    void *face = tf_host(c->args[7].j), *shader = pointer(c->args[8].j);
    float x = c->args[4].f, y = c->args[5].f, width, size, thick;
    const u16 *text;
    i32 count;
    gpaint p;
    text = text_of(c->args[1].l, c->args[2].i, c->args[3].i, &count);
    if (!v || !v->host || !text || count <= 0 || !paint_for(c->args[6].l, !shader, &p)) return;
    if (HAS(draw_text) && cv_draw(v, false)) GFX(G.draw_text(G.user, v->host, text, count, x, y, &p.p, face, shader));
    if (!p.underline && !p.strike) return;
    width = text_width(text, count, &p, face);
    size = p.p.text_size;
    thick = size / 18 > 1 ? size / 18 : 1;
    if (p.p.text_align == MEGLER_GFX_ALIGN_CENTER) x -= width / 2;
    else if (p.p.text_align == MEGLER_GFX_ALIGN_RIGHT) x -= width;
    p.p.style = MEGLER_GFX_FILL;
    if (p.underline) draw_rect(v, x, y + size / 9, x + width, y + size / 9 + thick, &p, shader);
    if (p.strike) draw_rect(v, x, y - size * 0.3f, x + width, y - size * 0.3f + thick, &p, shader);
}

/* (String, int start, int end, int[] paint, long typeface) -> float: the run's advance width */
N(G_txMeasure)
{
    gpaint p;
    i32 count;
    const u16 *text = text_of(c->args[0].l, c->args[1].i, c->args[2].i, &count);
    paint_of(c->args[3].l, &p);
    c->ret.f = text ? text_width(text, count, &p, tf_host(c->args[4].j)) : 0;
}

/*
 * (int[] paint, long typeface, float[5] out) -> float
 *
 * The face's measures at the paint's text size, y down from the baseline:
 * out = top, ascent (both negative), descent, bottom (both positive),
 * leading; returns the line spacing, -ascent + descent + leading. The Java
 * reads the five by those places (Paint.getFontMetrics).
 */
N(G_txMetrics)
{
    float *out = (float *)elements(c->args[2].l, 'F', 5), m[5] = { 0, 0, 0, 0, 0 }, spacing;
    gpaint p;
    int i;
    paint_of(c->args[0].l, &p);
    if (!HAS(text_metrics)) return;
    GFX(spacing = G.text_metrics(G.user, &p.p, tf_host(c->args[1].j), m));
    if (out)
        for (i = 0; i < 5; i++) out[i] = number(m[i], 0);
    c->ret.f = number(spacing, 0);
}

/* (String, int start, int end, int[] paint, long typeface, int[4] out): the ink's box from the run's start on the baseline, whole numbers that take it in. */
N(G_txBounds)
{
    i32 *out = (i32 *)elements(c->args[5].l, 'I', 4), count;
    const u16 *text = text_of(c->args[0].l, c->args[1].i, c->args[2].i, &count);
    float box[4] = { 0, 0, 0, 0 };
    gpaint p;
    if (!out) return;
    out[0] = out[1] = out[2] = out[3] = 0;
    paint_of(c->args[3].l, &p);
    if (!text || count <= 0 || !HAS(text_bounds)) return;
    GFX(G.text_bounds(G.user, text, count, &p.p, tf_host(c->args[4].j), box));
    if (!(box[2] > box[0]) || !(box[3] > box[1])) return;
    out[0] = whole(floor(box[0]));
    out[1] = whole(floor(box[1]));
    out[2] = whole(ceil(box[2]));
    out[3] = whole(ceil(box[3]));
}

/*
 * (String, int start, int end, float maxWidth, int[] paint, long typeface, boolean words) -> int
 *
 * How many chars from `start` fit in the width: by characters for
 * Paint.breakText, by words for StaticLayout's lines, which asks again by
 * characters when the answer is 0.
 */
N(G_txBreak)
{
    i32 count, fit;
    const u16 *text = text_of(c->args[0].l, c->args[1].i, c->args[2].i, &count);
    float max_width = c->args[3].f;
    gpaint p;
    paint_of(c->args[4].l, &p);
    if (!text || count <= 0 || !HAS(text_break)) return;
    if (!(max_width > 0)) return;
    GFX(fit = G.text_break(G.user, text, count, max_width, &p.p, tf_host(c->args[5].j), c->args[6].z != 0));
    c->ret.i = fit < 0 ? 0 : fit > count ? count : fit;
}

/* (String, int start, int end, int[] paint, long typeface, float[] out): each char's advance. An array too short for them is left alone. */
N(G_txWidths)
{
    i32 count;
    const u16 *text = text_of(c->args[0].l, c->args[1].i, c->args[2].i, &count);
    float *out;
    gpaint p;
    paint_of(c->args[3].l, &p);
    if (!text || count <= 0) return;
    out = (float *)elements(c->args[5].l, 'F', (u32)count);
    if (out && HAS(text_widths)) GFX(G.text_widths(G.user, text, count, &p.p, tf_host(c->args[4].j), out));
}

/* --------------------------------------------------------- Gfx: typefaces */

static gface *tf_new(void *host, i32 weight, bool italic)
{
    gface *f = (gface *)hk_alloc(sizeof(*f));
    f->magic = TF_MAGIC;
    f->host = host;
    f->weight = weight < 1 ? 1 : weight > 999 ? 999 : weight;
    f->italic = italic;
    return f;
}

/*
 * (String family, int style) -> long: Typeface.create(family, style). The
 * style is Typeface's: 1 bold, 2 italic. What the name says beyond the
 * family is read here as the original reads it: "medium", "light", "thin"
 * and "black" in it are weights, "mono" is the monospaced face, and "serif"
 * without "sans" the one with serifs.
 */
N(G_tfCreate)
{
    const char *name = c->args[0].l ? hk_dvm_string_utf8(c->args[0].l) : NULL;
    i32 style = c->args[1].i, weight = (style & 1) ? 700 : 400;
    int generic = MEGLER_GFX_SANS;
    char family[96], *at;
    void *host = NULL;
    hk_strlcpy(family, name && name[0] ? name : "sans-serif", sizeof(family));
    for (at = family; *at; at++)
        if (*at >= 'A' && *at <= 'Z') *at = (char)(*at + 32);
    if (strstr(family, "medium")) weight = (style & 1) ? 800 : 500;
    else if (strstr(family, "light")) weight = 300;
    else if (strstr(family, "thin")) weight = 200;
    else if (strstr(family, "black")) weight = 900;
    if (strstr(family, "mono")) generic = MEGLER_GFX_MONOSPACE;
    else if (strstr(family, "serif") && !strstr(family, "sans")) generic = MEGLER_GFX_SERIF;
    if (HAS(typeface_create)) GFX(host = G.typeface_create(G.user, family, generic, weight, (style & 2) != 0));
    c->ret.j = handle(tf_new(host, weight, (style & 2) != 0));
}

/* () -> long: Typeface.DEFAULT, which is what a Paint without a typeface draws with as well. */
N(G_tfDefault) { c->ret.j = handle(&g_default_face); }

/*
 * (long tf, int style, int weight) -> long: the same font at another weight
 * and slant; weight 0 is "by the style". The Java makes a new Typeface for
 * every such call and typefaces are never freed, so each is made once here.
 */
N(G_tfDerive)
{
    gface *from = tf_of(c->args[0].j), *made = NULL;
    i32 style = c->args[1].i, weight = c->args[2].i > 0 ? c->args[2].i : (style & 1) ? 700 : 400, i;
    bool italic = (style & 2) != 0;
    void *host = NULL;
    if (!from) from = &g_default_face;
    if (weight > 999) weight = 999;
    hk_lock_acquire(&g_lock);
    for (i = 0; i < g_nderived && !made; i++)
        if (g_derived[i].from == from && g_derived[i].weight == weight && g_derived[i].italic == italic) made = g_derived[i].made;
    hk_lock_release(&g_lock);
    if (!made) {
        if (HAS(typeface_derive)) GFX(host = G.typeface_derive(G.user, from->host, weight, italic));
        made = tf_new(host, weight, italic);
        hk_lock_acquire(&g_lock);
        if (g_nderived == g_cap_derived) {
            g_cap_derived = g_cap_derived * 2 + 16;
            g_derived = (struct derived *)hk_realloc(g_derived, sizeof(*g_derived) * (size_t)g_cap_derived);
        }
        g_derived[g_nderived].from = from;
        g_derived[g_nderived].made = made;
        g_derived[g_nderived].weight = weight;
        g_derived[g_nderived].italic = italic;
        g_nderived++;
        hk_lock_release(&g_lock);
    }
    c->ret.j = handle(made);
}

/* (byte[] data) -> long, 0 when it is not a font: fonts out of the app's assets and resources */
N(G_tfFromData)
{
    const u8 *data = (const u8 *)elements(c->args[0].l, 'B', 1);
    void *host;
    if (!data || !HAS(typeface_from_data)) return;
    GFX(host = G.typeface_from_data(G.user, data, length_of(c->args[0].l)));
    if (host) c->ret.j = handle(tf_new(host, 400, false));
}

N(G_tfWeight)
{
    gface *f = tf_of(c->args[0].j);
    c->ret.i = f ? f->weight : 400;
}

/* ----------------------------------------------------------- Gfx: shaders */

typedef struct gstops {
    u32 *colors;
    float *positions;
    i32 count;
} gstops;

/*
 * A gradient's stops as the shell is promised them: two or more, positions
 * in 0..1 that never go down. The Java passes colours, and positions or
 * null for "evenly"; the original takes positions fewer than the colours as
 * none, and no colours as black.
 */
static void stops_of(hk_jobj *colors, hk_jobj *positions, gstops *out)
{
    const u32 *cs = (const u32 *)elements(colors, 'I', 1);
    i32 n = cs ? (i32)length_of(colors) : 1, i;
    const float *ps = (const float *)elements(positions, 'F', (u32)n);
    out->count = n < 2 ? 2 : n;
    out->colors = (u32 *)hk_alloc(sizeof(u32) * (size_t)out->count);
    out->positions = (float *)hk_alloc(sizeof(float) * (size_t)out->count);
    for (i = 0; i < n; i++) {
        float at = ps ? number(ps[i], 0) : n == 1 ? 0 : (float)i / (float)(n - 1);
        if (at < 0) at = 0;
        if (at > 1) at = 1;
        if (i && at < out->positions[i - 1]) at = out->positions[i - 1];
        out->colors[i] = cs ? cs[i] : 0xFF000000u;
        out->positions[i] = at;
    }
    if (n < 2) {
        out->colors[1] = out->colors[0];
        out->positions[1] = 1;
    }
}

static void stops_free(gstops *s)
{
    free(s->colors);
    free(s->positions);
}

/* Shader.TileMode as the Java numbers it: 0 CLAMP (and DECAL), 1 REPEAT, 2 MIRROR. */
static int tile_number(i32 tile) { return tile == 1 || tile == 2 ? (int)tile : MEGLER_GFX_TILE_CLAMP; }

/* (x0, y0, x1, y1, int[] colors, float[] positions, int tile) -> long */
N(G_shLinear)
{
    gstops s;
    if (!HAS(shader_linear)) return;
    stops_of(c->args[4].l, c->args[5].l, &s);
    GFX(c->ret.j = handle(G.shader_linear(G.user, c->args[0].f, c->args[1].f, c->args[2].f, c->args[3].f, s.colors, s.positions, s.count,
                                          tile_number(c->args[6].i))));
    stops_free(&s);
}

/* (cx, cy, r, int[] colors, float[] positions, int tile) -> long */
N(G_shRadial)
{
    gstops s;
    if (!HAS(shader_radial)) return;
    stops_of(c->args[3].l, c->args[4].l, &s);
    GFX(c->ret.j = handle(G.shader_radial(G.user, c->args[0].f, c->args[1].f, c->args[2].f, s.colors, s.positions, s.count, tile_number(c->args[5].i))));
    stops_free(&s);
}

/* (cx, cy, int[] colors, float[] positions) -> long */
N(G_shSweep)
{
    gstops s;
    if (!HAS(shader_sweep)) return;
    stops_of(c->args[2].l, c->args[3].l, &s);
    GFX(c->ret.j = handle(G.shader_sweep(G.user, c->args[0].f, c->args[1].f, s.colors, s.positions, s.count)));
    stops_free(&s);
}

/* (long bm, int tile) -> long: the bitmap as it is now. The Java passes the x tile mode for both directions. */
N(G_shBitmap)
{
    gbitmap *b = bm_of(c->args[0].j);
    megler_gfx_image image;
    if (!b || !HAS(shader_bitmap)) return;
    bm_settle(b);
    image_of(b, &image);
    GFX(c->ret.j = handle(G.shader_bitmap(G.user, &image, tile_number(c->args[1].i))));
}

/* (long shader, float[6] or null): Shader.setLocalMatrix */
N(G_shMatrix)
{
    void *shader = pointer(c->args[0].j);
    const float *m = (const float *)elements(c->args[1].l, 'F', 6);
    int i;
    if (!shader || !HAS(shader_matrix)) return;
    for (i = 0; m && i < 6; i++)
        if (!isfinite(m[i])) return;
    GFX(G.shader_matrix(G.user, shader, m));
}

N(G_shFree)
{
    void *shader = pointer(c->args[0].j);
    if (shader && HAS(shader_free)) GFX(G.shader_free(G.user, shader));
}

/* ------------------------------------------------------------- Gfx: paths */

/* (float[] path, int n, float[4] out): Path.computeBounds. Zeros for a path with nothing in it. */
N(G_pathBounds)
{
    float *out = (float *)elements(c->args[2].l, 'F', 4);
    gpath p;
    gbox o;
    if (!out || !path_of(c->args[0].l, c->args[1].i, &p)) return;
    path_bounds(&p, &o);
    out[0] = o.l;
    out[1] = o.t;
    out[2] = o.r;
    out[3] = o.b;
    path_free(&p);
}

/* (float[] path, int n, int fill, float x, float y) -> boolean: whether the point is inside the path as it would be filled */
N(G_pathContains)
{
    gpath p;
    ghit h;
    if (!path_of(c->args[0].l, c->args[1].i, &p)) return;
    memset(&h, 0, sizeof(h));
    h.x = c->args[3].f;
    h.y = c->args[4].f;
    path_edges(&p, true, hit_edge, &h);
    c->ret.z = c->args[2].i == 1 ? (h.crossings & 1) != 0 : h.winding != 0;
    path_free(&p);
}

/*
 * (float[] path, int n, float[2] out) -> float: PathMeasure's length. As in
 * the original it is the length of everything in the path, every figure and
 * the closing lines of the closed ones, where Android measures the first
 * figure. out gets the path's first point, which the Java does not read.
 */
N(G_pathLength)
{
    float *out = (float *)elements(c->args[2].l, 'F', 2);
    double length = 0;
    gpath p;
    if (!path_of(c->args[0].l, c->args[1].i, &p)) return;
    path_edges(&p, false, length_edge, &length);
    if (out && p.n >= 3) {
        out[0] = p.v[1];
        out[1] = p.v[2];
    }
    c->ret.f = (float)length;
    path_free(&p);
}

/* ------------------------------------------------------------ Gfx: present */

/* (long bm): the window's views are drawn, in the screen's bitmap; 0 when there is nothing to show. */
/*
 * Gfx.bmDiffer(a, b, box): how many pixels differ between two bitmaps of one size outside the rectangles box[0..3] and
 * box[4..7] (left, top, right, bottom) as they are given, and then the box round them in box[0..3]. For the tests' check of a
 * window drawn in part (megler.ViewRoot): the screen outside the part drawn against the whole drawn again.
 */
N(G_bmDiffer)
{
    gbitmap *a = bm_of(c->args[0].j), *b = bm_of(c->args[1].j);
    i32 *box = (i32 *)elements(c->args[2].l, 'I', 8);
    i32 x, y, n = 0, skip[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    c->ret.j = 0;
    if (!a || !b || a->w != b->w || a->h != b->h) {
        c->ret.i = -1;
        return;
    }
    bm_settle(a);
    bm_settle(b);
    if (box) {
        memcpy(skip, box, sizeof(skip));
        box[0] = box[1] = box[2] = box[3] = 0;
    }
    for (y = 0; y < a->h; y++) {
        const u32 *ra = bm_row(a, y), *rb = bm_row(b, y);
        if (!memcmp(ra, rb, (size_t)a->w * 4)) continue;
        for (x = 0; x < a->w; x++) {
            if (ra[x] == rb[x] || (x >= skip[0] && x < skip[2] && y >= skip[1] && y < skip[3]) || (x >= skip[4] && x < skip[6] && y >= skip[5] && y < skip[7]))
                continue;
            if (box) {
                if (n == 0 || x < box[0]) box[0] = x;
                if (n == 0 || y < box[1]) box[1] = y;
                if (n == 0 || x + 1 > box[2]) box[2] = x + 1;
                if (n == 0 || y + 1 > box[3]) box[3] = y + 1;
            }
            n++;
        }
    }
    c->ret.i = n;
}

N(G_present)
{
    gbitmap *b = bm_of(c->args[0].j);
    if (!g_present.fn) return;
    if (!b) {
        g_present.fn(g_present.user, NULL, 0, 0, 0);
        return;
    }
    /*
     * Shown from the shell's own picture of the bitmap where it can (megler_gfx_host.present): when one canvas draws the bitmap
     * and nothing was written into the memory behind it. The canvas is then not flushed, and stays dirty: whatever reads
     * the memory later flushes it first (bm_settle), as it always has.
     */
    if (g_present.canvas_fn) {
        gcanvas *only;
        bool shown = false;
        hk_lock_acquire(&g_lock);
        only = b->canvases;
        if (only && !only->next && only->host && !only->stale && !b->rgba_locks) shown = g_present.canvas_fn(g_present.canvas_user, only->host, b->w, b->h);
        hk_lock_release(&g_lock);
        if (shown) return;
    }
    bm_settle(b);
    g_present.fn(g_present.user, b->px, b->w, b->h, b->stride);
}

static const hk_jhle k_gfx[] = {
    { "megler/Gfx", "bmNew", "(II)J", G_bmNew },
    { "megler/Gfx", "bmFree", "(J)V", G_bmFree },
    { "megler/Gfx", "bmErase", "(JI)V", G_bmErase },
    { "megler/Gfx", "bmDiffer", NULL, G_bmDiffer },
    { "megler/Gfx", "bmGetPixel", "(JII)I", G_bmGetPixel },
    { "megler/Gfx", "bmSetPixel", "(JIII)V", G_bmSetPixel },
    { "megler/Gfx", "bmPixels", "(J[IIIIIIIZ)V", G_bmPixels },
    { "megler/Gfx", "bmRaw", "(JJZ)V", G_bmRaw },
    { "megler/Gfx", "bmAddress", "(J)J", G_bmAddress },
    { "megler/Gfx", "bmDecode", "([BIII[IZ)J", G_bmDecode },
    { "megler/Gfx", "bmCompress", "(JII)[B", G_bmCompress },
    { "megler/Gfx", "bmCopy", "(JIIII[FZ)J", G_bmCopy },
    { "megler/Gfx", "cvNew", "(J)J", G_cvNew },
    { "megler/Gfx", "cvFree", "(J)V", G_cvFree },
    { "megler/Gfx", "cvSave", "(J)I", G_cvSave },
    { "megler/Gfx", "cvSaveLayer", "(JFFFFI)I", G_cvSaveLayer },
    { "megler/Gfx", "cvSaveLayerMode", "(JFFFFII)I", G_cvSaveLayerMode },
    { "megler/Gfx", "cvRestore", "(J)V", G_cvRestore },
    { "megler/Gfx", "cvRestoreTo", "(JI)V", G_cvRestoreTo },
    { "megler/Gfx", "cvSaveCount", "(J)I", G_cvSaveCount },
    { "megler/Gfx", "cvMatrix", "(J[FZ)V", G_cvMatrix },
    { "megler/Gfx", "cvGetMatrix", "(J[F)V", G_cvGetMatrix },
    { "megler/Gfx", "cvClipRect", "(JFFFFI)Z", G_cvClipRect },
    { "megler/Gfx", "cvClipPath", "(J[FIII)Z", G_cvClipPath },
    { "megler/Gfx", "cvClipBounds", "(J[I)Z", G_cvClipBounds },
    { "megler/Gfx", "cvDrawColor", "(JII)V", G_cvDrawColor },
    { "megler/Gfx", "cvDrawRect", "(JFFFF[IJ)V", G_cvDrawRect },
    { "megler/Gfx", "cvDrawRRect", "(JFFFFFF[IJ)V", G_cvDrawRRect },
    { "megler/Gfx", "cvDrawOval", "(JFFFF[IJ)V", G_cvDrawOval },
    { "megler/Gfx", "cvDrawArc", "(JFFFFFFZ[IJ)V", G_cvDrawArc },
    { "megler/Gfx", "cvDrawPath", "(J[FII[IJ)V", G_cvDrawPath },
    { "megler/Gfx", "cvDrawLines", "(J[FII[IJZ)V", G_cvDrawLines },
    { "megler/Gfx", "cvDrawBitmap", "(JJFFFFFFFF[I)V", G_cvDrawBitmap },
    { "megler/Gfx", "cvDrawBitmapM", "(JJ[F[I)V", G_cvDrawBitmapM },
    { "megler/Gfx", "cvDrawText", "(JLjava/lang/String;IIFF[IJJ)V", G_cvDrawText },
    { "megler/Gfx", "txMeasure", "(Ljava/lang/String;II[IJ)F", G_txMeasure },
    { "megler/Gfx", "txMetrics", "([IJ[F)F", G_txMetrics },
    { "megler/Gfx", "txBounds", "(Ljava/lang/String;II[IJ[I)V", G_txBounds },
    { "megler/Gfx", "txBreak", "(Ljava/lang/String;IIF[IJZ)I", G_txBreak },
    { "megler/Gfx", "txWidths", "(Ljava/lang/String;II[IJ[F)V", G_txWidths },
    { "megler/Gfx", "tfCreate", "(Ljava/lang/String;I)J", G_tfCreate },
    { "megler/Gfx", "tfDefault", "()J", G_tfDefault },
    { "megler/Gfx", "tfDerive", "(JII)J", G_tfDerive },
    { "megler/Gfx", "tfFromData", "([B)J", G_tfFromData },
    { "megler/Gfx", "tfWeight", "(J)I", G_tfWeight },
    { "megler/Gfx", "shLinear", "(FFFF[I[FI)J", G_shLinear },
    { "megler/Gfx", "shRadial", "(FFF[I[FI)J", G_shRadial },
    { "megler/Gfx", "shSweep", "(FF[I[F)J", G_shSweep },
    { "megler/Gfx", "shBitmap", "(JI)J", G_shBitmap },
    { "megler/Gfx", "shMatrix", "(J[F)V", G_shMatrix },
    { "megler/Gfx", "shFree", "(J)V", G_shFree },
    { "megler/Gfx", "pathBounds", "([FI[F)V", G_pathBounds },
    { "megler/Gfx", "pathContains", "([FIIFF)Z", G_pathContains },
    { "megler/Gfx", "pathLength", "([FI[F)F", G_pathLength },
    { "megler/Gfx", "present", "(J)V", G_present },
    { NULL, NULL, NULL, NULL },
};

/* --------------------------------------------------------- for the driver */

const hk_jhle *hk_java_gfx_natives(void) { return k_gfx; }

void hk_java_gfx_set_host(const megler_gfx_host *host)
{
    if (host) G = *host;
    else memset(&G, 0, sizeof(G));
}

u64 hk_java_gfx_host_ns(void) { return t_host_ns; }

void hk_java_gfx_set_present(void (*fn)(void *user, const void *pixels, int width, int height, int stride), void *user)
{
    g_present.fn = fn;
    g_present.user = user;
}

void hk_java_gfx_set_present_canvas(bool (*fn)(void *user, void *canvas, int width, int height), void *user)
{
    g_present.canvas_fn = fn;
    g_present.canvas_user = user;
}

void hk_java_gfx_stats(uint64_t *bitmap_bytes, uint32_t *bitmaps)
{
    hk_lock_acquire(&g_lock);
    if (bitmap_bytes) *bitmap_bytes = g_bytes;
    if (bitmaps) *bitmaps = g_count;
    hk_lock_release(&g_lock);
}

static void info_of(const gbitmap *b, hk_java_gfx_bitmap *out)
{
    if (!out) return;
    out->pixels = b->px;
    out->width = b->w;
    out->height = b->h;
    out->stride = b->stride;
}

bool hk_java_gfx_bitmap_info(i64 bm, hk_java_gfx_bitmap *out)
{
    gbitmap *b = bm_of(bm);
    if (!b) return false;
    bm_settle(b);
    info_of(b, out);
    return true;
}

void hk_java_gfx_bitmap_changed(i64 bm)
{
    gbitmap *b = bm_of(bm);
    if (b) bm_changed(b);
}

bool hk_java_gfx_lock_rgba(i64 bm, hk_java_gfx_bitmap *out)
{
    gbitmap *b = bm_of(bm);
    bool first;
    if (!b) return false;
    bm_settle(b);
    hk_lock_acquire(&g_lock);
    first = b->rgba_locks++ == 0;
    hk_lock_release(&g_lock);
    if (first) swap_red_blue(b->px, b->px, (size_t)b->w * (size_t)b->h);
    info_of(b, out);
    return true;
}

void hk_java_gfx_unlock_rgba(i64 bm)
{
    gbitmap *b = bm_of(bm);
    bool last;
    if (!b) return;
    hk_lock_acquire(&g_lock);
    last = b->rgba_locks > 0 && --b->rgba_locks == 0;
    hk_lock_release(&g_lock);
    if (!last) return;
    swap_red_blue(b->px, b->px, (size_t)b->w * (size_t)b->h);
    bm_changed(b);
}
