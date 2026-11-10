/*
 * megler.Gfx: the native methods under the Java framework's android.graphics.
 *
 * The framework (core/framework) has Bitmap, Canvas, Paint, Path, Typeface
 * and the shaders in Java, on the 54 native methods of megler.Gfx. A bitmap's
 * pixels and everything that is
 * bookkeeping are this file's, and what has to be drawn, measured or decoded
 * goes to the shell through megler_gfx_host (megler.h, which is the contract for
 * that side and says what each call means). This comment is the other side:
 * what the Java passes.
 *
 * Handles are Java longs. A bitmap, a canvas, a typeface, a shader, a
 * display list and a node are objects of this file; a shader holds the
 * shell's own. 0 is "none" everywhere.
 *
 * A bitmap is 4 * width * height bytes from calloc, B, G, R, A premultiplied,
 * top row first: what Direct2D, WIC and the phone's swap chain take as it
 * is. Android's own order is R, G, B, A, and it shows in two places, which
 * convert: bmRaw (copyPixelsToBuffer, copyPixelsFromBuffer, texture uploads,
 * frames from native producers) and mg_java_gfx_lock_rgba (libjnigraphics).
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
 * with a fill rule beside it, 0 winding or 1 even-odd, and 2 more for
 * Android's inverse rules (Path.meglerFill): filled, a path so is everything
 * the path does not cover, within the clip; clipped by, it is the clip taken
 * out (and taken out, the clip kept). The shell sees no inverse rule: a fill
 * is given to it as a rectangle that holds the canvas's clip and the path
 * inside it, by even-odd (draw_path), and a clip as the other op
 * (cv_clip_path). The shell is not shown the stream: path_build turns it into
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
 *
 * DISPLAY LISTS. A RenderNode's canvas and a Picture's record what is drawn
 * instead of drawing it, as Android's do, and the list is drawn again
 * wherever the node or the picture is drawn (cvRecord, cvEndRecord,
 * cvDrawNode, cvDrawList). A recording canvas is a canvas of this file with
 * no bitmap and no shell's half: it keeps its save count, matrix and clip as
 * any canvas does (they start as the identity and (0, 0, width, height)),
 * and each call that would reach the shell is written to the list instead,
 * with its arguments as they are then: the paint unpacked, the path built,
 * the matrix, the text. What a call refers to is referred to, as on Android:
 * a bitmap (its pixels as they are when the list is drawn), a shader (with
 * its local matrix as it was when recorded), a node, another list. Each of
 * those is kept alive by the lists that refer to it, whatever the Java does
 * with its own object.
 *
 * A node (rnNew) is a list and the properties it is drawn with, which the
 * Java keeps and hands over whole (rnSet) before anything is drawn that may
 * draw the node: its place, its matrix, alpha, clips, outline and layer. A
 * node drawn into another's list is that node, not a copy of it: a node
 * drawn later is drawn with its list and properties as they are then. A
 * picture's list is the one it had when drawn into another list.
 */
#include <math.h>
#include <stdio.h>

#include "../dvm/mg_dvm.h"
#include "../host/mg_host.h"
#include "../mg_core.h"
#include "../webp/mg_webp.h"
#include "mg_java_gfx.h"

#define N(name) static void name(mg_jcall *c)

#define BM_MAGIC 0x31504d42u
#define CV_MAGIC 0x31564e43u
#define TF_MAGIC 0x31434654u
#define SH_MAGIC 0x31444853u
#define DL_MAGIC 0x31534c44u
#define RN_MAGIC 0x31444e52u

typedef struct gcanvas gcanvas;
typedef struct gdlist gdlist;

typedef struct gbitmap {
    u32 magic;
    i32 w, h, stride;
    u8 *px;
    gcanvas *canvases;
    u32 id, version;                   /* for a shell that keeps textures: megler_gfx_image */
    i32 rgba_locks;                    /* above zero: the memory is in Android's order (mg_java_gfx_lock_rgba) */
    i32 refs;                          /* display lists that draw it: it outlives the Java's bmFree until the last goes */
    bool released;                     /* the Java has freed it (bmFree), and only lists keep it */
    i32 presenting;                    /* its canvas is being shown, outside g_lock (present_bitmap): g_presented */
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
    gdlist *rec;                       /* a recording canvas's list (no bitmap, no shell's half): what is drawn is written there */
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
 * (mg_java_gfx_host_ns).
 */
static MG_THREAD_LOCAL u64 t_host_ns;
#define GFX(statement)                                    \
    do {                                                  \
        u64 gfx_started_ = mg_time_monotonic_ns();        \
        statement;                                        \
        t_host_ns += mg_time_monotonic_ns() - gfx_started_; \
    } while (0)
static struct {
    void (*fn)(void *user, const void *pixels, int width, int height, int stride);
    void *user;
    bool (*canvas_fn)(void *user, void *canvas, int width, int height);
    void *canvas_user;
    bool (*probe_due)(void);
    void (*probe)(const void *pixels, int width, int height, int stride);
} g_present;
/* The bitmaps' counts, each bitmap's list of canvases and their two flags, and the typefaces made. */
static mg_lock g_lock = MG_LOCK_INIT;
/* A bitmap's canvas shown without g_lock (present_bitmap) is shown whole before anything flushes or frees that canvas. */
static mg_cond g_presented = MG_COND_INIT;
static void bm_wait_presented(gbitmap *b)
{
    while (b->presenting) mg_cond_wait(&g_presented, &g_lock);
}
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
    mg_note_once("gfx: the shell has no %s: what it would draw or answer is missing", fn);
    return false;
}
#define HAS(fn) (G.fn != NULL || missing(#fn))

static i64 handle(const void *p) { return (i64)(mg_addr)p; }
static void *pointer(i64 h) { return (void *)(mg_addr)h; }

/*
 * A shader as the Java holds it: the shell's, and what a display list needs
 * to know of it. Its local matrix can change after a list drew with it
 * (Shader.setLocalMatrix), and the Java can free it while a list still
 * draws with it; a list keeps it alive and remembers the matrix it had.
 */
typedef struct gshader {
    u32 magic;
    void *host;
    float m[6];
    bool has_m;                        /* m is its local matrix (shMatrix); without, it has none */
    bool released;                     /* the Java is done with it (shFree) */
    i32 refs;                          /* display lists that draw with it */
} gshader;

static gshader *sh_new(void *host)
{
    gshader *s;
    if (!host) return NULL;
    s = (gshader *)mg_alloc(sizeof(*s));
    s->magic = SH_MAGIC;
    s->host = host;
    return s;
}

static gshader *sh_of(i64 h)
{
    gshader *s = (gshader *)pointer(h);
    return s && s->magic == SH_MAGIC ? s : NULL;
}

static void *sh_host(const gshader *s) { return s ? s->host : NULL; }

static void sh_destroy(gshader *s)
{
    if (HAS(shader_free)) GFX(G.shader_free(G.user, s->host));
    s->magic = 0;
    free(s);
}

static void sh_release(gshader *s)
{
    bool last;
    mg_lock_acquire(&g_lock);
    s->released = true;
    last = s->refs == 0;
    mg_lock_release(&g_lock);
    if (last) sh_destroy(s);
}

static void sh_ref(gshader *s)
{
    mg_lock_acquire(&g_lock);
    s->refs++;
    mg_lock_release(&g_lock);
}

static void sh_unref(gshader *s)
{
    bool last;
    mg_lock_acquire(&g_lock);
    last = --s->refs == 0 && s->released;
    mg_lock_release(&g_lock);
    if (last) sh_destroy(s);
}

/* The elements of a primitive array of that type with at least that many of them; NULL for anything else. */
static void *elements(mg_jobj *a, char etype, u32 at_least)
{
    if (!a || a->kind != MG_J_PRIM_ARRAY || a->u.arr.etype != etype || a->u.arr.len < at_least) return NULL;
    return a->u.arr.data;
}
static u32 length_of(mg_jobj *a) { return a && a->kind == MG_J_PRIM_ARRAY ? a->u.arr.len : 0; }

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
    mg_lock_acquire(&g_lock);
    if (!++g_next_id) g_next_id = 1;
    b->id = g_next_id;
    g_bytes += (u64)b->stride * (u64)b->h;
    g_count++;
    mg_lock_release(&g_lock);
    /* Pixels a Bitmap object keeps alive: the collector counts them toward its next collection, as Android counts a bitmap's. */
    mg_dvm_gc_native((i64)b->stride * (i64)b->h);
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
    mg_lock_acquire(&g_lock);
    bm_wait_presented(b);
    for (v = b->canvases; v; v = v->next)
        if (v->dirty) {
            if (v->host && G.flush) GFX(G.flush(G.user, v->host));
            v->dirty = false;
        }
    mg_lock_release(&g_lock);
}

/* After the pixels were written by anything but a canvas: the canvases are told before they next draw, and a kept copy is no good. */
static void bm_changed(gbitmap *b)
{
    gcanvas *v;
    mg_lock_acquire(&g_lock);
    b->version++;
    for (v = b->canvases; v; v = v->next) v->stale = true;
    mg_lock_release(&g_lock);
}

/* The memory itself goes: nothing refers to the bitmap any more, neither the Java nor a display list. */
static void bm_destroy(gbitmap *b)
{
    if (G.bitmap_gone) GFX(G.bitmap_gone(G.user, b->id));
    b->magic = 0;
    free(b->px);
    free(b);
}

/*
 * The Java is done with the bitmap (bmFree: recycle() or the collector), or
 * the core with one it made. While a display list still draws it, its
 * pixels stay (it is counted until then) and the list draws them as they
 * are, as on Android; the last list to go frees it (bm_unref).
 */
static void bm_free(gbitmap *b)
{
    gcanvas *v;
    i32 i;
    bool kept;
    mg_lock_acquire(&g_lock);
    bm_wait_presented(b);
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
    b->canvases = NULL;
    kept = b->refs > 0;
    b->released = true;
    if (!kept) {
        g_bytes -= (u64)b->stride * (u64)b->h;
        g_count--;
    }
    mg_lock_release(&g_lock);
    if (!kept) bm_destroy(b);
}

/* A display list draws the bitmap, or no longer does. */
static void bm_ref(gbitmap *b)
{
    mg_lock_acquire(&g_lock);
    b->refs++;
    mg_lock_release(&g_lock);
}

static void bm_unref(gbitmap *b)
{
    bool last;
    mg_lock_acquire(&g_lock);
    last = --b->refs == 0 && b->released;
    if (last) {
        g_bytes -= (u64)b->stride * (u64)b->h;
        g_count--;
    }
    mg_lock_release(&g_lock);
    if (last) bm_destroy(b);
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
static void paint_unpack(const i32 *a, gpaint *out)
{
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
    p->stroke_width = number(mg_f((u32)a[P_STROKE]), 0);
    if (p->stroke_width < 0) p->stroke_width = 0;
    p->flags = (a[P_FLAGS] & PF_AA ? MEGLER_GFX_ANTI_ALIAS : 0) | (a[P_FLAGS] & PF_FILTER ? MEGLER_GFX_FILTER_BITMAP : 0) |
               (a[P_FLAGS] & PF_FAKEBOLD ? MEGLER_GFX_FAKE_BOLD : 0);
    out->underline = (a[P_FLAGS] & PF_UNDERLINE) != 0;
    out->strike = (a[P_FLAGS] & PF_STRIKE) != 0;
    p->cap = a[P_CAP] == 1 || a[P_CAP] == 2 ? (int)a[P_CAP] : MEGLER_GFX_CAP_BUTT;
    p->join = a[P_JOIN] == 1 || a[P_JOIN] == 2 ? (int)a[P_JOIN] : MEGLER_GFX_JOIN_MITER;
    p->miter_limit = number(mg_f((u32)a[P_MITER]), 4);
    if (!(p->miter_limit > 0)) p->miter_limit = 4;
    p->text_size = number(mg_f((u32)a[P_TEXTSIZE]), 12);
    if (!(p->text_size > 0)) p->text_size = 12;
    p->text_align = a[P_ALIGN] == 1 || a[P_ALIGN] == 2 ? (int)a[P_ALIGN] : MEGLER_GFX_ALIGN_LEFT;
    p->text_scale_x = number(mg_f((u32)a[P_SCALEX]), 1);
    if (!(p->text_scale_x > 0)) p->text_scale_x = 1;
    p->text_skew_x = number(mg_f((u32)a[P_SKEWX]), 0);
    p->letter_spacing = number(mg_f((u32)a[P_LSPACE]), 0);
    p->blend = blend_number(a[P_XFER]);
    p->filter_mode = a[P_CFMODE] < 0 ? -1 : blend_number(a[P_CFMODE]);
    p->filter_color = (u32)a[P_CFCOLOR];
    p->shadow_radius = number(mg_f((u32)a[P_SHRAD]), 0);
    if (!(p->shadow_radius > 0)) p->shadow_radius = 0;
    p->shadow_dx = number(mg_f((u32)a[P_SHDX]), 0);
    p->shadow_dy = number(mg_f((u32)a[P_SHDY]), 0);
    p->shadow_color = (u32)a[P_SHCOL];
}

static void paint_of(mg_jobj *array, gpaint *out) { paint_unpack((const i32 *)elements(array, 'I', P_COUNT), out); }

/*
 * The paint for one drawing call. False when the call draws nothing: DST
 * leaves the picture as it is. `plain` says the paint's own colour is what
 * is drawn (no shader, no bitmap), and then a colour filter is done here.
 */
static bool paint_for(mg_jobj *array, bool plain, gpaint *out)
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

/* Path.meglerFill's second bit: the inverse of the fill rule in the first (MEGLER_GFX_WINDING or MEGLER_GFX_EVEN_ODD). The shell never sees it. */
#define FILL_INVERSE 2

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
    p->cap = (i32)MG_COUNT(p->fixed);
}

static void path_free(gpath *p)
{
    if (p->v != p->fixed) free(p->v);
}

static void path_put(gpath *p, int verb, const float *xy, int count)
{
    if (p->n + 1 + count > p->cap) {
        i32 cap = p->cap * 2 + 64;
        float *v = (float *)mg_alloc(sizeof(float) * (size_t)cap);
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

/* Another built path's figures after this one's. A built path is verbs, each with its numbers after it, as path_put writes
   them: the whole of it goes in as its first verb with everything after that as the verb's numbers. */
static void path_append(gpath *p, const gpath *q)
{
    if (q->n) path_put(p, (int)q->v[0], q->v + 1, q->n - 1);
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
static bool path_of(mg_jobj *array, i32 n, gpath *p)
{
    const float *f = (const float *)elements(array, 'F', 0);
    if (!f) return false;
    if (n < 0) n = 0;
    if ((u32)n > length_of(array)) n = (i32)length_of(array);
    path_build(p, f, n);
    return true;
}

/* ---------------------------------------------------------- display lists */

/*
 * A display list: the calls a recording canvas was given, one after
 * another, each a dl_head and what that kind of call carries, a multiple of
 * eight bytes long. It is written once, by its canvas, and read from then on
 * by whoever draws it; it goes when the last of its owners does -- the Java's
 * Picture or the node it was recorded for, and the lists that draw it.
 */
struct gdlist {
    u32 magic;
    i32 refs;
    u8 *ops;
    size_t n, cap;
    i32 width, height;                 /* the size it was recorded at */
    /* What the frame walk (Gfx.damage) worked out once, the list being written no more: the box its own calls draw in, in
       the coordinates it starts in (own_kind 0 not yet, 1 nothing, 2 own, 3 all of the clip), and whether it draws nodes or
       other lists (kids). Read and written by the main thread only. */
    float own[4];
    i32 own_kind;
    bool kids;
};

/* A RenderNode: the list it draws and what it draws it with, as the Java last handed it over (rnSet). */
typedef struct gnode {
    u32 magic;
    i32 refs;                          /* the Java's RenderNode, and each list that draws the node */
    gdlist *list;                      /* NULL: none recorded, or discarded */
    float left, top, right, bottom;
    float m[6];                        /* its transform, applied after the move to (left, top) */
    float alpha;
    float clip[4];                     /* setClipRect's rectangle, in the node's own coordinates */
    float outline[4], radius;          /* the outline's rectangle and corner radius (outline_kind 1) */
    float *outline_path;               /* the outline's path, built (outline_kind 2) */
    i32 outline_n, outline_fill, outline_kind;
    i32 flags;                         /* RN_* */
    i32 layer_alpha;                   /* 0..255: the alpha a layer of the node is put on with */
    i32 layer_paint[P_COUNT];          /* the layer's paint, as Paint.mP (RN_LAYER_PAINT) */
    u32 ver;                           /* one more each time rnSet changes a property, and at rnDiscard */
    /*
     * Where the frame walk (Gfx.damage) last found the node drawn: its list then (held), its properties' ver, the matrix
     * and the clip it was drawn with, the box on the screen it drew in, and the walk's number then (seen_multi: found twice
     * in that walk). All of it the main thread's; the render thread reads the seen box while no walk runs (node_play).
     *
     * `defer`: a View's own node whose whole box was invalidated, what changed in it left to the walk (rnDefer). Once a
     * list is recorded for it after the invalidation -- `ends`, the lists recorded for it, kept or not, beyond defer_min --
     * the walk compares that list with the one it saw. An invalidation made while its list was being recorded (an
     * animation's, from its draw) is for the list recorded after that one: defer_max, the last of them.
     */
    gdlist *seen_list;
    u32 seen_ver, seen_no;
    float seen_m[6], seen_clip[4], seen_box[4];
    bool seen, seen_any, seen_whole, seen_multi, defer;
    u32 ends, defer_min, defer_max;
} gnode;

/* gnode.flags, as the Java packs them (android.graphics.RenderNode, Megler's overlay). RN_VIEW: a View's own node (its list
   the view's drawing), whose changes the View's invalidations say. */
enum { RN_IDENTITY = 1, RN_CLIP_BOUNDS = 2, RN_CLIP_RECT = 4, RN_CLIP_OUTLINE = 8, RN_LAYER = 16, RN_LAYER_PAINT = 32, RN_VIEW = 64 };

typedef struct dl_head {
    u16 kind;
    u16 spare;
    u32 size;                          /* of the whole call, this head included */
} dl_head;

enum {
    DL_SAVE = 1, DL_SAVE_LAYER, DL_RESTORE, DL_MATRIX, DL_CLIP_RECT, DL_CLIP_PATH, DL_COLOR, DL_RECT, DL_RRECT, DL_OVAL, DL_PATH, DL_LINES,
    DL_BITMAP, DL_BITMAP_M, DL_TEXT, DL_NODE, DL_LIST, DL_TEXTURE
};

/* A shader as a call drew with it: the shader, and the local matrix it had then. */
typedef struct gshot {
    gshader *sh;
    float m[6];
    bool has_m;
} gshot;

typedef struct dl_layer {
    dl_head h;
    float bounds[4];
    i32 bounded, alpha, blend;
    float blur[3];                     /* radius x, radius y, clamp: a blurred layer's (cv_save), when blurred */
    i32 blurred;
} dl_layer;

typedef struct dl_matrix {
    dl_head h;
    float m[6];
    i32 set;
} dl_matrix;

typedef struct dl_rect {               /* DL_CLIP_RECT (r[0..3], op), DL_RECT and DL_OVAL (r[0..3]), DL_RRECT (r[0..5]) */
    dl_head h;
    float r[6];
    i32 op;
    gpaint p;
    gshot s;
} dl_rect;

typedef struct dl_path {               /* DL_CLIP_PATH (fill, op), DL_PATH (fill, p, s), DL_LINES (points, p, s); n floats follow */
    dl_head h;
    i32 fill, op, points, n;
    gpaint p;
    gshot s;
} dl_path;

typedef struct dl_color {
    dl_head h;
    u32 color;
    i32 blend;
} dl_color;

typedef struct dl_texture {            /* DL_TEXTURE: a picture the shell keeps on the graphics processor (draw_texture) */
    dl_head h;
    i32 id, alpha;
    float dst[4];
} dl_texture;

typedef struct dl_bitmap {             /* DL_BITMAP (src, dst), DL_BITMAP_M (m) */
    dl_head h;
    gbitmap *b;
    u32 version;                       /* the bitmap's when recorded: a list recorded again over new pixels is not the same list */
    float src[4], dst[4], m[6];
    gpaint p;
} dl_bitmap;

typedef struct dl_text {               /* count UTF-16 units follow */
    dl_head h;
    float x, y;
    void *face;
    i32 count;
    gpaint p;
    gshot s;
} dl_text;

typedef struct dl_node {
    dl_head h;
    gnode *node;
} dl_node;

typedef struct dl_list {
    dl_head h;
    gdlist *list;
} dl_list;

/* How deep nodes and lists may be drawn inside each other: deeper is taken for a list that draws itself. */
#define DL_DEPTH 200

static u64 g_dl_bytes;
static u32 g_dl_count, g_rn_count;

static gdlist *dl_new(i32 width, i32 height)
{
    gdlist *l = (gdlist *)mg_alloc(sizeof(*l));
    l->magic = DL_MAGIC;
    l->refs = 1;
    l->width = width;
    l->height = height;
    mg_lock_acquire(&g_lock);
    g_dl_count++;
    mg_lock_release(&g_lock);
    return l;
}

static gdlist *dl_of(i64 h)
{
    gdlist *l = (gdlist *)pointer(h);
    return l && l->magic == DL_MAGIC ? l : NULL;
}

static gnode *rn_of(i64 h)
{
    gnode *n = (gnode *)pointer(h);
    return n && n->magic == RN_MAGIC ? n : NULL;
}

static void dl_ref(gdlist *l)
{
    mg_lock_acquire(&g_lock);
    l->refs++;
    mg_lock_release(&g_lock);
}

static void rn_ref(gnode *n)
{
    mg_lock_acquire(&g_lock);
    n->refs++;
    mg_lock_release(&g_lock);
}

static void rn_unref(gnode *n);

/* The last owner is gone: what the list refers to is let go of, and the list goes. */
static void dl_unref(gdlist *l)
{
    size_t at;
    bool last;
    if (!l) return;
    mg_lock_acquire(&g_lock);
    last = --l->refs == 0;
    if (last) {
        g_dl_bytes -= l->cap;
        g_dl_count--;
    }
    mg_lock_release(&g_lock);
    if (!last) return;
    for (at = 0; at < l->n; at += ((const dl_head *)(const void *)(l->ops + at))->size) {
        const dl_head *h = (const dl_head *)(const void *)(l->ops + at);
        switch (h->kind) {
        case DL_RECT:
        case DL_RRECT:
        case DL_OVAL:
            if (((const dl_rect *)h)->s.sh) sh_unref(((const dl_rect *)h)->s.sh);
            break;
        case DL_PATH:
        case DL_LINES:
            if (((const dl_path *)h)->s.sh) sh_unref(((const dl_path *)h)->s.sh);
            break;
        case DL_TEXT:
            if (((const dl_text *)h)->s.sh) sh_unref(((const dl_text *)h)->s.sh);
            break;
        case DL_BITMAP:
        case DL_BITMAP_M: bm_unref(((const dl_bitmap *)h)->b); break;
        case DL_NODE: rn_unref(((const dl_node *)h)->node); break;
        case DL_LIST: dl_unref(((const dl_list *)h)->list); break;
        default: break;
        }
    }
    l->magic = 0;
    free(l->ops);
    free(l);
}

static void rn_unref(gnode *n)
{
    bool last;
    mg_lock_acquire(&g_lock);
    last = --n->refs == 0;
    if (last) g_rn_count--;
    mg_lock_release(&g_lock);
    if (!last) return;
    dl_unref(n->list);
    dl_unref(n->seen_list);
    free(n->outline_path);
    n->magic = 0;
    free(n);
}

/* Room for one more call of `size` bytes and `extra` after them, zeroed, its head filled in. Valid until the next. */
static void *dl_put(gdlist *l, int kind, size_t size, size_t extra)
{
    size_t whole = (size + extra + 7) & ~(size_t)7;
    dl_head *h;
    if (l->n + whole > l->cap) {
        size_t cap = l->cap * 2 > l->n + whole ? l->cap * 2 : l->n + whole + 256;
        l->ops = (u8 *)mg_realloc(l->ops, cap);
        mg_lock_acquire(&g_lock);
        g_dl_bytes += cap - l->cap;
        mg_lock_release(&g_lock);
        /* The collector counts it as it counts pixels: a RenderNode or a Picture let go of is what frees it. */
        mg_dvm_gc_native((i64)(cap - l->cap));
        l->cap = cap;
    }
    h = (dl_head *)(void *)(l->ops + l->n);
    memset(h, 0, whole);
    h->kind = (u16)kind;
    h->size = (u32)whole;
    l->n += whole;
    return h;
}

/* The shader of a call being recorded, as it is now. */
static void shot_of(gshader *s, gshot *out)
{
    memset(out, 0, sizeof(*out));
    if (!s) return;
    sh_ref(s);
    out->sh = s;
    out->has_m = s->has_m;
    memcpy(out->m, s->m, sizeof(out->m));
}

/* Before a recorded call is drawn again: its shader given the matrix it had then, when it has another now. True when it was. */
static bool shot_on(const gshot *s)
{
    gshader *sh = s->sh;
    if (!sh || !G.shader_matrix || (sh->has_m == s->has_m && (!s->has_m || !memcmp(sh->m, s->m, sizeof(s->m))))) return false;
    GFX(G.shader_matrix(G.user, sh->host, s->has_m ? s->m : NULL));
    return true;
}

/* And after it: the shader's own matrix back. */
static void shot_off(const gshot *s, bool was)
{
    if (was) GFX(G.shader_matrix(G.user, s->sh->host, s->sh->has_m ? s->sh->m : NULL));
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
    mg_lock_acquire(&g_lock);
    bm_wait_presented(v->bm);
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
    mg_lock_release(&g_lock);
}

/* Before a drawing call: whether anything can be drawn through the canvas now, and if so the shell's canvas made ready for it. */
static bool cv_draw(gcanvas *v, bool replaces)
{
    if (!v || !v->bm || !v->host || cv_state(v)->empty) return false;
    cv_changes(v, replaces);
    return true;
}

/*
 * A save, or a layer's: `layer` with its alpha and blend, and `blur` (radius x, radius y, clamp) for a layer put onto the
 * picture blurred (save_layer_blur, megler.h): source-over, through save_layer, sharp, where the shell has no blur.
 */
static i32 cv_save(gcanvas *v, const float *bounds, bool layer, int alpha, int blend, const float *blur)
{
    gstate *s;
    if (v->depth + 1 >= v->cap) {
        v->cap = v->cap * 2 + 8;
        v->st = (gstate *)mg_realloc(v->st, sizeof(gstate) * (size_t)v->cap);
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
        if (layer && blur && G.save_layer_blur) GFX(G.save_layer_blur(G.user, v->host, bounds, alpha, blur[0], blur[1], blur[2] != 0));
        else if (layer && HAS(save_layer)) GFX(G.save_layer(G.user, v->host, bounds, alpha, blur ? MEGLER_GFX_BLEND_SRC_OVER : blend));
        else if (HAS(save)) GFX(G.save(G.user, v->host));
    }
    if (v->rec) {
        if (layer) {
            dl_layer *o = (dl_layer *)dl_put(v->rec, DL_SAVE_LAYER, sizeof(*o), 0);
            if (bounds) memcpy(o->bounds, bounds, sizeof(o->bounds));
            o->bounded = bounds != NULL;
            o->alpha = alpha;
            o->blend = blend;
            o->blurred = blur != NULL;
            if (blur) memcpy(o->blur, blur, sizeof(o->blur));
        } else {
            dl_put(v->rec, DL_SAVE, sizeof(dl_head), 0);
        }
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
    if (v->rec) dl_put(v->rec, DL_RESTORE, sizeof(dl_head), 0);
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
    /* A recording canvas freed before cvEndRecord: what it recorded goes with it. */
    dl_unref(v->rec);
    v->rec = NULL;
    while (v->depth > 0) cv_restore(v);
    mg_lock_acquire(&g_lock);
    if (v->bm) bm_wait_presented(v->bm);
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
    mg_lock_release(&g_lock);
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
    mg_lock_acquire(&g_lock);
    for (through = b->canvases; through; through = through->next)
        if (through->host && cv_whole(through)) break;
    mg_lock_release(&g_lock);
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

/*
 * (long bm, int argb): the memory itself made one colour, never through a canvas of the bitmap, which would draw it in the
 * shell and have it read back before memory is next written (the WebView's bitmap, which Chromium draws into directly,
 * android/webkit/WebView.java).
 */
N(G_bmEraseMemory)
{
    gbitmap *b = bm_of(c->args[0].j);
    u32 v = premultiplied((u32)c->args[1].i), *row;
    i32 x, y;
    if (!b) return;
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
        mg_dvm_throw("java/lang/IllegalArgumentException", "pixels out of bounds");
        return;
    }
    if (!w || !h) return;
    /* The rows' places in the array run evenly from the first to the last, so those two say whether every one is inside it. */
    first = off;
    last = off + (h - 1) * stride;
    if (first < 0 || last < 0 || first + w > len || last + w > len) {
        mg_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "pixels");
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

/* (long bm): every pixel's alpha made 255 and its colours left as they are -- a frame that an opaque TextureView shows,
   whose producer wrote RGBA with alpha 0 (SurfaceTexture.meglerPostRgba). */
N(G_bmOpaque)
{
    gbitmap *b = bm_of(c->args[0].j);
    i32 x, y;
    if (!b) return;
    bm_settle(b);
    for (y = 0; y < b->h; y++) {
        u32 *row = bm_row(b, y);
        for (x = 0; x < b->w; x++) row[x] |= 0xFF000000u;
    }
    bm_changed(b);
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
     * A WebP picture is the core's own to decode (webp/mg_webp.h): the phone's decoder has no WebP. In a build without
     * the decoder it goes to the shell like any other picture, for whatever the shell can make of it.
     */
    webp = mg_webp_is(data, (size_t)n);
    if (webp && !mg_webp_available()) {
        mg_note_once("java: the app has WebP pictures, and this build of the layer has no WebP decoder in it (build/webp: tools/webp-fetch.sh): they are left to the shell's");
        webp = false;
    }
    if (webp) {
        if (!mg_webp_size(data, (size_t)n, &w, &h)) return;
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
    if (webp) decoded = mg_webp_decode(data, (size_t)n, b->px, b->w, b->h, b->stride);
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

static MEGLER_CB bool bytes_write(void *sink, const void *data, size_t size)
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
    mg_jobj *array;
    bool encoded = false;
    if (!b || !HAS(image_encode)) return;
    memset(&out, 0, sizeof(out));
    bm_settle(b);
    if (quality < 0) quality = 0;
    if (quality > 100) quality = 100;
    GFX(encoded = G.image_encode(G.user, b->px, b->w, b->h, b->stride, c->args[1].i == 0 ? MEGLER_GFX_JPEG : MEGLER_GFX_PNG, quality, bytes_write, &out));
    if (encoded && out.size) {
        array = mg_dvm_new_array("[B", (i32)out.size);
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

/* A canvas's own half: nothing saved, the identity, the clip (0, 0, width, height). */
static gcanvas *cv_alloc(i32 width, i32 height)
{
    gcanvas *v = (gcanvas *)mg_alloc(sizeof(*v));
    gstate *s;
    v->magic = CV_MAGIC;
    v->cap = 8;
    v->st = (gstate *)mg_alloc(sizeof(gstate) * (size_t)v->cap);
    s = cv_state(v);
    matrix_identity(s->m);
    matrix_identity(v->sent);
    s->clip[2] = (float)width;
    s->clip[3] = (float)height;
    if (!(s->clip[2] > 0) || !(s->clip[3] > 0)) s->empty = true;
    return v;
}

static gcanvas *cv_make(gbitmap *b)
{
    gcanvas *v = cv_alloc(b->w, b->h);
    v->bm = b;
    /* What other canvases have drawn is in the memory before the shell reads it as this one's picture. */
    bm_settle(b);
    /* Without the shell's half the canvas still counts saves and keeps its matrix and clip, so that the Java's bookkeeping holds; it draws nothing. */
    if (HAS(canvas_new)) GFX(v->host = G.canvas_new(G.user, b->px, b->w, b->h, b->stride));
    mg_lock_acquire(&g_lock);
    v->next = b->canvases;
    b->canvases = v;
    mg_lock_release(&g_lock);
    return v;
}

N(G_cvNew)
{
    gbitmap *b = bm_of(c->args[0].j);
    if (b) c->ret.j = handle(cv_make(b));
}

/*
 * (int width, int height) -> long: a canvas that records (RenderNode.beginRecording, Picture.beginRecording). Its clip
 * starts as (0, 0, width, height), as a RecordingCanvas's does, for what the Java asks of it; nothing is left out of the
 * list for being outside it, as nothing is on Android.
 */
N(G_cvRecord)
{
    i32 w = c->args[0].i, h = c->args[1].i;
    gcanvas *v = cv_alloc(w, h);
    v->rec = dl_new(w, h);
    c->ret.j = handle(v);
}

/*
 * (long cv) -> long: the list a recording canvas has made, for the caller to keep and free (dlFree); the canvas records no
 * more, and the Java frees it as any other (cvFree). 0 for a canvas that was not recording.
 */
static gdlist *cv_end_record(gcanvas *v)
{
    gdlist *l;
    if (!v || !v->rec) return NULL;
    l = v->rec;
    v->rec = NULL;
    /* The list is not written to again: what was allocated beyond it is given back. */
    if (l->cap > l->n + 64) {
        size_t cap = l->n ? l->n : 8;
        l->ops = (u8 *)mg_realloc(l->ops, cap);
        mg_lock_acquire(&g_lock);
        g_dl_bytes -= l->cap - cap;
        mg_lock_release(&g_lock);
        l->cap = cap;
    }
    return l;
}

N(G_cvEndRecord) { c->ret.j = handle(cv_end_record(cv_of(c->args[0].j))); }

N(G_cvFree)
{
    gcanvas *v = cv_of(c->args[0].j);
    if (v) cv_free(v);
}

N(G_cvSave)
{
    gcanvas *v = cv_of(c->args[0].j);
    c->ret.i = v ? cv_save(v, NULL, false, 255, MEGLER_GFX_BLEND_SRC_OVER, NULL) : 1;
}

/* (long, l, t, r, b, int alpha[, int mode]): saveLayerAlpha, and saveLayer with a Paint's alpha and transfer mode. A rectangle with no area is "no bounds". */
static void save_layer(mg_jcall *c, int mode)
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
    c->ret.i = cv_save(v, bounded ? bounds : NULL, true, alpha, mode, NULL);
}

N(G_cvSaveLayer) { save_layer(c, MEGLER_GFX_BLEND_SRC_OVER); }

/*
 * (long, l, t, r, b, int alpha, float radiusX, float radiusY, boolean clamp): a layer put onto the picture blurred, for a view
 * with Android's RenderEffect.createBlurEffect (android.view.View.drawFromParent, overlaid). Radii in the matrix's units.
 */
N(G_cvSaveLayerBlur)
{
    gcanvas *v = cv_of(c->args[0].j);
    float bounds[4], blur[3];
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
    blur[0] = c->args[6].f > 0 ? c->args[6].f : 0;
    blur[1] = c->args[7].f > 0 ? c->args[7].f : 0;
    blur[2] = c->args[8].z ? 1.0f : 0.0f;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    c->ret.i = cv_save(v, bounded ? bounds : NULL, true, alpha, MEGLER_GFX_BLEND_SRC_OVER, blur[0] > 0 || blur[1] > 0 ? blur : NULL);
}
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

/*
 * The canvas's matrix set to `m`, or `m` concatenated to it. A list drawn again sets its matrices relative to the one the
 * canvas had when the list began (`base`), as Android plays a recorded setMatrix; the Java's own setMatrix has none.
 */
static void cv_matrix(gcanvas *v, const float *m, bool set, const float *base)
{
    gstate *s = cv_state(v);
    if (set && base) matrix_concat(base, m, s->m);
    else if (set) memcpy(s->m, m, sizeof(s->m));
    else matrix_concat(s->m, m, s->m);
    cv_send_matrix(v, s->m);
    if (v->rec) {
        dl_matrix *o = (dl_matrix *)dl_put(v->rec, DL_MATRIX, sizeof(*o), 0);
        memcpy(o->m, m, sizeof(o->m));
        o->set = set;
    }
}

/* (long, float[6], boolean set): Canvas.setMatrix, or concat and with it translate, scale, rotate and skew. */
N(G_cvMatrix)
{
    gcanvas *v = cv_of(c->args[0].j);
    const float *m = (const float *)elements(c->args[1].l, 'F', 6);
    int i;
    if (!v || !m) return;
    for (i = 0; i < 6; i++)
        if (!isfinite(m[i])) return;
    cv_matrix(v, m, c->args[2].z != 0, NULL);
}

N(G_cvGetMatrix)
{
    gcanvas *v = cv_of(c->args[0].j);
    float *m = (float *)elements(c->args[1].l, 'F', 6);
    if (v && m) memcpy(m, cv_state(v)->m, sizeof(float) * 6);
}

/* A rectangle taken as the clip (MEGLER_GFX_CLIP_INTERSECT) or out of it. Whether anything is left of the clip. */
static bool cv_clip_rect(gcanvas *v, float l, float t, float r, float b, int op)
{
    gstate *s = cv_state(v);
    float box[4], swap;
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
    if (v->rec) {
        dl_rect *o = (dl_rect *)dl_put(v->rec, DL_CLIP_RECT, sizeof(*o), 0);
        o->r[0] = l;
        o->r[1] = t;
        o->r[2] = r;
        o->r[3] = b;
        o->op = op;
    }
    return !s->empty;
}

/* (long, l, t, r, b, int op: 1 intersect, 0 difference) -> whether anything is left of the clip. The Java passes no other op. */
N(G_cvClipRect)
{
    gcanvas *v = cv_of(c->args[0].j);
    c->ret.z = 1;
    if (v) c->ret.z = cv_clip_rect(v, c->args[1].f, c->args[2].f, c->args[3].f, c->args[4].f, c->args[5].i == 0 ? MEGLER_GFX_CLIP_DIFFERENCE : MEGLER_GFX_CLIP_INTERSECT);
}

/* A built path taken as the clip or out of it. Whether anything is left of the clip. */
static bool cv_clip_path(gcanvas *v, const gpath *p, int fill, int op)
{
    gstate *s = cv_state(v);
    gbox o;
    float box[4];
    s->clipped = true;
    if (fill & FILL_INVERSE) {
        /* Inside an inverse path is outside the path: clipping to it is taking the path out, and taking it out keeps it. */
        op = op == MEGLER_GFX_CLIP_INTERSECT ? MEGLER_GFX_CLIP_DIFFERENCE : MEGLER_GFX_CLIP_INTERSECT;
        fill &= ~FILL_INVERSE;
    }
    if (op == MEGLER_GFX_CLIP_INTERSECT) {
        path_bounds(p, &o);
        /* Nothing is inside a path with nothing in it. */
        if (!o.any) {
            s->empty = true;
        } else {
            matrix_box(s->m, o.l, o.t, o.r, o.b, box);
            clip_intersect(s, box);
        }
    }
    if (v->host && HAS(clip_path)) GFX(G.clip_path(G.user, v->host, p->v, p->n, fill, op));
    if (v->rec) {
        dl_path *d = (dl_path *)dl_put(v->rec, DL_CLIP_PATH, sizeof(*d), sizeof(float) * (size_t)p->n);
        d->fill = fill;
        d->op = op;
        d->n = p->n;
        if (p->n) memcpy(d + 1, p->v, sizeof(float) * (size_t)p->n);
    }
    return !s->empty;
}

/* (long, float[] path, int n, int fill, int op) -> whether anything is left of the clip */
N(G_cvClipPath)
{
    gcanvas *v = cv_of(c->args[0].j);
    int fill = c->args[3].i & (MEGLER_GFX_EVEN_ODD | FILL_INVERSE), op = c->args[4].i == 0 ? MEGLER_GFX_CLIP_DIFFERENCE : MEGLER_GFX_CLIP_INTERSECT;
    gpath p;
    c->ret.z = 1;
    if (!v || !path_of(c->args[1].l, c->args[2].i, &p)) return;
    c->ret.z = cv_clip_path(v, &p, fill, op);
    path_free(&p);
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

static void draw_color(gcanvas *v, u32 color, int blend)
{
    bool replaces = blend == MEGLER_GFX_BLEND_SRC || blend == MEGLER_GFX_BLEND_CLEAR;
    if (blend == MEGLER_GFX_BLEND_DST || !v) return;
    if (v->rec) {
        dl_color *o = (dl_color *)dl_put(v->rec, DL_COLOR, sizeof(*o), 0);
        o->color = color;
        o->blend = blend;
        return;
    }
    if (!v->host || !HAS(draw_color) || !cv_draw(v, replaces && cv_whole(v))) return;
    GFX(G.draw_color(G.user, v->host, color, blend));
}

/* (long, int color, int mode): Canvas.drawColor. */
N(G_cvDrawColor) { draw_color(cv_of(c->args[0].j), (u32)c->args[1].i, blend_number(c->args[2].i)); }

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

/*
 * A path filled or stroked. Filled by an inverse rule, it is everything within the clip that the path does not cover: what
 * the shell is given is a rectangle a pixel larger than the clip's box, in the drawing's coordinates, and the path's own
 * figures after it, by even-odd -- the path's inside then lies inside the rectangle twice, and is left out. That is exact for
 * INVERSE_EVEN_ODD, and for INVERSE_WINDING wherever the path winds no more than once round a point (a rounded rectangle,
 * a circle, any outline that does not cross itself), which is what an inverse path is used for: corners painted over to
 * round a view. Whatever of the path lies outside the rectangle is outside the clip too. A stroke is drawn by the path's
 * own rule, inverse or not (Android would stroke and then invert the outline of the stroke, which nothing has been seen to
 * ask for).
 */
static void draw_path(gcanvas *v, const gpath *path, int fill, const gpaint *p, void *shader)
{
    if ((fill & FILL_INVERSE) && p->p.style != MEGLER_GFX_STROKE) {
        gstate *s;
        float inverse[6], box[4];
        gpath whole;
        if (!v || !v->host || !HAS(draw_path)) return;
        s = cv_state(v);
        if (s->empty || !matrix_invert(s->m, inverse)) return;
        matrix_box(inverse, s->clip[0] - 1, s->clip[1] - 1, s->clip[2] + 1, s->clip[3] + 1, box);
        path_init(&whole);
        path_rect(&whole, box[0], box[1], box[2], box[3]);
        path_append(&whole, path);
        if (cv_draw(v, false)) GFX(G.draw_path(G.user, v->host, whole.v, whole.n, MEGLER_GFX_EVEN_ODD, &p->p, shader));
        path_free(&whole);
        return;
    }
    if (!v || !v->host || !path_draws(path) || !HAS(draw_path) || !cv_draw(v, false)) return;
    GFX(G.draw_path(G.user, v->host, path->v, path->n, fill & MEGLER_GFX_EVEN_ODD, &p->p, shader));
}

/* A call with a rectangle's numbers, written to a recording canvas's list. */
static void rec_rect(gdlist *l, int kind, const float *r, int count, const gpaint *p, gshader *sh)
{
    dl_rect *o = (dl_rect *)dl_put(l, kind, sizeof(*o), 0);
    memcpy(o->r, r, sizeof(float) * (size_t)count);
    o->p = *p;
    shot_of(sh, &o->s);
}

/* A built path written to a recording canvas's list (DL_PATH), or the points of drawLines and drawPoints (DL_LINES). */
static void rec_path(gdlist *l, int kind, const float *f, i32 n, int fill, bool points, const gpaint *p, gshader *sh)
{
    dl_path *o = (dl_path *)dl_put(l, kind, sizeof(*o), sizeof(float) * (size_t)n);
    o->fill = fill;
    o->points = points;
    o->n = n;
    o->p = *p;
    shot_of(sh, &o->s);
    if (n) memcpy(o + 1, f, sizeof(float) * (size_t)n);
}

/* (long, l, t, r, b, int[] paint, long shader) */
N(G_cvDrawRect)
{
    gcanvas *v = cv_of(c->args[0].j);
    gshader *sh = sh_of(c->args[6].j);
    float r[4];
    gpaint p;
    int i;
    if (!paint_for(c->args[5].l, !sh, &p)) return;
    for (i = 0; i < 4; i++) r[i] = c->args[1 + i].f;
    if (v && v->rec) rec_rect(v->rec, DL_RECT, r, 4, &p, sh);
    else draw_rect(v, r[0], r[1], r[2], r[3], &p, sh_host(sh));
}

static void draw_round_rect(gcanvas *v, float l, float t, float r, float b, float rx, float ry, const gpaint *p, void *shader)
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
    if (rx > (r - l) / 2) rx = (r - l) / 2;
    if (ry > (b - t) / 2) ry = (b - t) / 2;
    /* Without both radii the corners are square. */
    if (!(rx > 0) || !(ry > 0)) {
        draw_rect(v, l, t, r, b, p, shader);
        return;
    }
    if (!v || !v->host || !HAS(draw_round_rect) || !cv_draw(v, false)) return;
    GFX(G.draw_round_rect(G.user, v->host, l, t, r, b, rx, ry, &p->p, shader));
}

/* (long, l, t, r, b, rx, ry, int[] paint, long shader) */
N(G_cvDrawRRect)
{
    gcanvas *v = cv_of(c->args[0].j);
    gshader *sh = sh_of(c->args[8].j);
    float r[6];
    gpaint p;
    int i;
    if (!paint_for(c->args[7].l, !sh, &p)) return;
    for (i = 0; i < 6; i++) r[i] = c->args[1 + i].f;
    if (v && v->rec) rec_rect(v->rec, DL_RRECT, r, 6, &p, sh);
    else draw_round_rect(v, r[0], r[1], r[2], r[3], r[4], r[5], &p, sh_host(sh));
}

static void draw_oval(gcanvas *v, float l, float t, float r, float b, const gpaint *p, void *shader)
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
    if (!v || !v->host || !HAS(draw_oval) || !cv_draw(v, false)) return;
    GFX(G.draw_oval(G.user, v->host, l, t, r, b, &p->p, shader));
}

/* (long, l, t, r, b, int[] paint, long shader) */
N(G_cvDrawOval)
{
    gcanvas *v = cv_of(c->args[0].j);
    gshader *sh = sh_of(c->args[6].j);
    float r[4];
    gpaint p;
    int i;
    if (!paint_for(c->args[5].l, !sh, &p)) return;
    for (i = 0; i < 4; i++) r[i] = c->args[1 + i].f;
    if (v && v->rec) rec_rect(v->rec, DL_OVAL, r, 4, &p, sh);
    else draw_oval(v, r[0], r[1], r[2], r[3], &p, sh_host(sh));
}

/* (long, l, t, r, b, float start, float sweep, boolean useCenter, int[] paint, long shader): the arc alone, or the wedge to the oval's centre. */
N(G_cvDrawArc)
{
    gcanvas *v = cv_of(c->args[0].j);
    float l = c->args[1].f, t = c->args[2].f, r = c->args[3].f, b = c->args[4].f;
    gshader *sh = sh_of(c->args[9].j);
    gpaint p;
    gpath path;
    if (!paint_for(c->args[8].l, !sh, &p)) return;
    path_init(&path);
    if (c->args[7].z) {
        path_move(&path, (l + r) / 2, (t + b) / 2);
        path_arc(&path, l, t, r, b, c->args[5].f, c->args[6].f, false);
        path_close(&path);
    } else {
        path_arc(&path, l, t, r, b, c->args[5].f, c->args[6].f, true);
    }
    if (v && v->rec) rec_path(v->rec, DL_PATH, path.v, path.n, MEGLER_GFX_WINDING, false, &p, sh);
    else draw_path(v, &path, MEGLER_GFX_WINDING, &p, sh_host(sh));
    path_free(&path);
}

/* (long, float[] path, int n, int fill, int[] paint, long shader) */
N(G_cvDrawPath)
{
    gcanvas *v = cv_of(c->args[0].j);
    gshader *sh = sh_of(c->args[5].j);
    int fill = c->args[3].i & (MEGLER_GFX_EVEN_ODD | FILL_INVERSE);
    gpaint p;
    gpath path;
    if (!paint_for(c->args[4].l, !sh, &p) || !path_of(c->args[1].l, c->args[2].i, &path)) return;
    if (v && v->rec) rec_path(v->rec, DL_PATH, path.v, path.n, fill, false, &p, sh);
    else draw_path(v, &path, fill, &p, sh_host(sh));
    path_free(&path);
}

/*
 * drawLines: every four floats a line, stroked whatever the paint's style.
 * drawPoints: every two floats a point as wide as the stroke -- a square, or
 * a circle when the cap is round --, filled. A hairline point is a pixel, by
 * the matrix the canvas has when it is drawn.
 */
static void draw_lines(gcanvas *v, const float *f, i32 n, bool points, const gpaint *paint, void *shader)
{
    gpaint p = *paint;
    gpath path;
    float half;
    i32 i;
    if (!v) return;
    path_init(&path);
    if (points) {
        const float *m = cv_state(v)->m;
        double det = fabs((double)m[0] * m[4] - (double)m[1] * m[3]);
        half = p.p.stroke_width > 0 ? p.p.stroke_width / 2 : det > 0 ? (float)(0.5 / sqrt(det)) : 0.5f;
        for (i = 0; i + 1 < n; i += 2) {
            if (p.p.cap == MEGLER_GFX_CAP_ROUND) path_oval(&path, f[i] - half, f[i + 1] - half, f[i] + half, f[i + 1] + half);
            else path_rect(&path, f[i] - half, f[i + 1] - half, f[i] + half, f[i + 1] + half);
        }
        p.p.style = MEGLER_GFX_FILL;
    } else {
        for (i = 0; i + 3 < n; i += 4) {
            path_move(&path, f[i], f[i + 1]);
            path_line(&path, f[i + 2], f[i + 3]);
        }
        p.p.style = MEGLER_GFX_STROKE;
    }
    draw_path(v, &path, MEGLER_GFX_WINDING, &p, shader);
    path_free(&path);
}

/* (long, float[] pts, int offset, int count, int[] paint, long shader, boolean points) */
N(G_cvDrawLines)
{
    gcanvas *v = cv_of(c->args[0].j);
    const float *f = (const float *)elements(c->args[1].l, 'F', 0);
    i64 off = c->args[2].i, n = c->args[3].i;
    gshader *sh = sh_of(c->args[5].j);
    gpaint p;
    if (!v || !f || off < 0 || n < 0 || off + n > length_of(c->args[1].l) || !paint_for(c->args[4].l, !sh, &p)) return;
    if (v->rec) rec_path(v->rec, DL_LINES, f + off, (i32)n, MEGLER_GFX_WINDING, c->args[6].z != 0, &p, sh);
    else draw_lines(v, f + off, (i32)n, c->args[6].z != 0, &p, sh_host(sh));
}

/*
 * A part of one bitmap onto a rectangle of a canvas. As Android has it: a
 * source or a destination with no area draws nothing, and a source that
 * reaches outside the bitmap is cut to it, the destination with it.
 */
static void draw_bitmap(gcanvas *v, gbitmap *b, float *src, float *dst, const gpaint *p, const float *matrix)
{
    static int canvas_copy = -1;
    megler_gfx_image image;
    bool ready = false;
    float sw = src[2] - src[0], sh = src[3] - src[1], dw = dst[2] - dst[0], dh = dst[3] - dst[1], cut;
    if (!v || !v->host || !b || !(sw > 0) || !(sh > 0) || !(dw > 0) || !(dh > 0)) return;
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
    /* Reject an invisible blit before settling its source. A dirty source
       can force a GPU readback even when the host would draw no pixels. */
    const gstate *state = cv_state(v);
    if (state->empty) return;
    if (!(p->p.shadow_radius > 0 && (p->p.shadow_color >> 24))) {
        float box[4];
        matrix_box(matrix ? matrix : state->m, dst[0], dst[1], dst[2], dst[3], box);
        /* Leave a pixel of slack for antialiasing and fractional coordinates. */
        if (box[2] + 1 < state->clip[0] || box[0] - 1 > state->clip[2] ||
            box[3] + 1 < state->clip[1] || box[1] - 1 > state->clip[3]) return;
    }
    /* A single current canvas can supply its picture without bringing it back
       to memory. Do not use this for self-copies or competing bitmap canvases. */
    if (canvas_copy < 0) canvas_copy = !mg_flag("no-canvas-copy");
    if (G.draw_canvas && canvas_copy && v->bm != b) {
        bool drawn = false;
        if (!(ready = cv_draw(v, false))) return;
        mg_lock_acquire(&g_lock);
        gcanvas *source = b->canvases;
        if (source && !source->next && source->host && !source->stale)
            GFX(drawn = G.draw_canvas(G.user, v->host, source->host, src, dst, &p->p));
        mg_lock_release(&g_lock);
        if (drawn) return;
    }
    /* What is drawn from must otherwise be in memory, including self-copies. */
    bm_settle(b);
    if (!ready && !cv_draw(v, false)) return;
    image_of(b, &image);
    GFX(G.draw_bitmap(G.user, v->host, &image, src, dst, &p->p));
}

/* The paint of a call that draws a bitmap. drawBitmap without a Paint samples smoothly, as the original has it. False: it draws nothing. */
static bool bitmap_paint(mg_jobj *paint, gpaint *p)
{
    if (!paint_for(paint, false, p)) return false;
    if (!paint) p->p.flags |= MEGLER_GFX_FILTER_BITMAP;
    return true;
}

/* A bitmap drawn, written to a recording canvas's list: the bitmap itself, whose pixels are those it has when the list is drawn. */
static dl_bitmap *rec_bitmap(gdlist *l, int kind, gbitmap *b, const gpaint *p)
{
    dl_bitmap *o = (dl_bitmap *)dl_put(l, kind, sizeof(*o), 0);
    bm_ref(b);
    o->b = b;
    o->version = b->version;
    o->p = *p;
    return o;
}

/* (long cv, long bm, sl, st, sr, sb, dl, dt, dr, db, int[] paint or null) */
N(G_cvDrawBitmap)
{
    gcanvas *v = cv_of(c->args[0].j);
    gbitmap *b = bm_of(c->args[1].j);
    float src[4], dst[4];
    gpaint p;
    int i;
    for (i = 0; i < 4; i++) {
        src[i] = c->args[2 + i].f;
        dst[i] = c->args[6 + i].f;
    }
    if (!v || !b || !bitmap_paint(c->args[10].l, &p)) return;
    if (v->rec) {
        dl_bitmap *o = rec_bitmap(v->rec, DL_BITMAP, b, &p);
        memcpy(o->src, src, sizeof(src));
        memcpy(o->dst, dst, sizeof(dst));
        return;
    }
    draw_bitmap(v, b, src, dst, &p, NULL);
}

/* The whole bitmap through a matrix of its own, which is the canvas's for the one call. */
static void draw_bitmap_m(gcanvas *v, gbitmap *b, const float *m, const gpaint *p)
{
    float src[4], dst[4], through[6];
    if (!v || !v->host || !b) return;
    src[0] = src[1] = dst[0] = dst[1] = 0;
    src[2] = dst[2] = (float)b->w;
    src[3] = dst[3] = (float)b->h;
    matrix_concat(cv_state(v)->m, m, through);
    cv_send_matrix(v, through);
    draw_bitmap(v, b, src, dst, p, through);
    cv_send_matrix(v, cv_state(v)->m);
}

/* (long cv, long bm, float[6] matrix, int[] paint or null) */
N(G_cvDrawBitmapM)
{
    gcanvas *v = cv_of(c->args[0].j);
    gbitmap *b = bm_of(c->args[1].j);
    const float *m = (const float *)elements(c->args[2].l, 'F', 6);
    gpaint p;
    int i;
    if (!v || !b || !m) return;
    for (i = 0; i < 6; i++)
        if (!isfinite(m[i])) return;
    if (!bitmap_paint(c->args[3].l, &p)) return;
    if (v->rec) {
        dl_bitmap *o = rec_bitmap(v->rec, DL_BITMAP_M, b, &p);
        memcpy(o->m, m, sizeof(o->m));
        return;
    }
    draw_bitmap_m(v, b, m, &p);
}

/*
 * A picture the shell keeps on the graphics processor (megler_gfx_host.texture_frame: a TextureView's frames that an app made
 * with OpenGL ES), drawn into a rectangle of the canvas: by the shell, nothing read back; recorded by its id, so that a list
 * draws the picture the id has when it is drawn.
 */
static void draw_texture(gcanvas *v, int id, const float *dst, int alpha)
{
    if (!v || !v->host || id <= 0 || !G.draw_texture || !(dst[2] > dst[0]) || !(dst[3] > dst[1])) return;
    if (!cv_draw(v, false)) return;
    GFX(G.draw_texture(G.user, v->host, id, dst[0], dst[1], dst[2], dst[3], alpha < 0 ? 0 : alpha > 255 ? 255 : alpha));
}

/* (long cv, int id, float l, t, r, b, int alpha) */
N(G_cvDrawTexture)
{
    gcanvas *v = cv_of(c->args[0].j);
    float dst[4];
    int i;
    for (i = 0; i < 4; i++) {
        dst[i] = c->args[2 + i].f;
        if (!isfinite(dst[i])) return;
    }
    if (!v || c->args[1].i <= 0) return;
    if (v->rec) {
        dl_texture *o = (dl_texture *)dl_put(v->rec, DL_TEXTURE, sizeof(*o), 0);
        o->id = c->args[1].i;
        o->alpha = c->args[6].i;
        memcpy(o->dst, dst, sizeof(dst));
        return;
    }
    draw_texture(v, c->args[1].i, dst, c->args[6].i);
}

/* textureGone(int id): the shell's copy of that id's picture let go (its SurfaceTexture released). */
N(G_textureGone)
{
    if (c->args[0].i > 0 && G.texture_frame) GFX(G.texture_frame(G.user, c->args[0].i, NULL, 0, 0));
}

/* Whether the shell keeps pictures on the graphics processor (texture_frame and draw_texture). */
bool mg_java_gfx_textures(void) { return G.texture_frame != NULL && G.draw_texture != NULL; }

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
static const u16 *text_of(mg_jobj *s, i32 start, i32 end, i32 *count)
{
    i32 n = 0;
    const u16 *u = s ? mg_dvm_string_utf16(s, &n) : NULL;
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
 * A run of text, its baseline at y and x by the paint's alignment. An
 * underline and a strike-through are bars drawn here, where the original
 * puts them: a ninth of the text's size below the baseline and three tenths
 * above it, an eighteenth thick.
 */
static void draw_text(gcanvas *v, const u16 *text, i32 count, float x, float y, const gpaint *paint, void *face, void *shader)
{
    gpaint p = *paint;
    float width, size, thick;
    if (!v || !v->host || !text || count <= 0) return;
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

/* (long cv, String, int start, int end, float x, float y, int[] paint, long typeface, long shader): Canvas.drawText, the part of the string as one run. */
N(G_cvDrawText)
{
    gcanvas *v = cv_of(c->args[0].j);
    void *face = tf_host(c->args[7].j);
    gshader *sh = sh_of(c->args[8].j);
    const u16 *text;
    i32 count;
    gpaint p;
    text = text_of(c->args[1].l, c->args[2].i, c->args[3].i, &count);
    if (!v || !text || count <= 0 || !paint_for(c->args[6].l, !sh, &p)) return;
    if (v->rec) {
        dl_text *o = (dl_text *)dl_put(v->rec, DL_TEXT, sizeof(*o), sizeof(u16) * (size_t)count);
        o->x = c->args[4].f;
        o->y = c->args[5].f;
        o->face = face;
        o->count = count;
        o->p = p;
        shot_of(sh, &o->s);
        memcpy(o + 1, text, sizeof(u16) * (size_t)count);
        return;
    }
    draw_text(v, text, count, c->args[4].f, c->args[5].f, &p, face, sh_host(sh));
}

/* ------------------------------------------------- Gfx: display lists drawn */

static void node_play(gcanvas *v, gnode *n, int depth);

/* While the render thread draws a frame the frame walk went through (G_damage): the screen's canvas and the walk's number, by
   whose boxes node_play leaves out a node that draws nowhere in the part drawn; and how many it left out, for its log. */
static MG_THREAD_LOCAL gcanvas *t_cull_v;
static MG_THREAD_LOCAL u32 t_cull_no;
static u32 g_culled;

static bool rects_meet(const float *a, const float *b) { return a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3]; }

/* A recorded path as the functions that take a built one take it. */
static void path_view(gpath *p, const float *f, i32 n)
{
    memset(p, 0, offsetof(gpath, fixed));
    p->v = (float *)(mg_addr)f;
    p->n = n;
    p->cap = n;
    p->any = n > 0;
}

/* The calls of a list that draw or clip with a path, each on a frame of its own: a path is large, and lists are drawn inside each other. */
static MG_NOINLINE void play_path(gcanvas *v, const dl_path *o, int kind)
{
    gpath p;
    bool was;
    path_view(&p, (const float *)(o + 1), o->n);
    if (kind == DL_CLIP_PATH) {
        cv_clip_path(v, &p, o->fill, o->op);
        return;
    }
    was = shot_on(&o->s);
    if (kind == DL_PATH) draw_path(v, &p, o->fill, &o->p, sh_host(o->s.sh));
    else draw_lines(v, (const float *)(o + 1), o->n, o->points != 0, &o->p, sh_host(o->s.sh));
    shot_off(&o->s, was);
}

/*
 * A list drawn on a canvas: every call again, on this canvas, from the
 * matrix and clip it has now. It is drawn inside a save of its own, as
 * Android draws a picture or a node's list, so that whatever the list left
 * saved or set is undone after it; a restore it has beyond its own saves is
 * not done.
 */
static void list_play(gcanvas *v, gdlist *l, int depth)
{
    float base[6], src[4], dst[4];
    i32 floor_depth;
    size_t at;
    bool was;
    if (!v || !l) return;
    if (depth > DL_DEPTH) {
        mg_note_once("gfx: display lists drawn inside each other more than %d deep, which is a list that draws itself: what is deeper is left out", DL_DEPTH);
        return;
    }
    dl_ref(l);
    cv_save(v, NULL, false, 255, MEGLER_GFX_BLEND_SRC_OVER, NULL);
    floor_depth = v->depth;
    memcpy(base, cv_state(v)->m, sizeof(base));
    for (at = 0; at < l->n; at += ((const dl_head *)(const void *)(l->ops + at))->size) {
        const dl_head *h = (const dl_head *)(const void *)(l->ops + at);
        switch (h->kind) {
        case DL_SAVE: cv_save(v, NULL, false, 255, MEGLER_GFX_BLEND_SRC_OVER, NULL); break;
        case DL_SAVE_LAYER: {
            const dl_layer *o = (const dl_layer *)h;
            cv_save(v, o->bounded ? o->bounds : NULL, true, o->alpha, o->blend, o->blurred ? o->blur : NULL);
            break;
        }
        case DL_RESTORE:
            if (v->depth > floor_depth) cv_restore(v);
            break;
        case DL_MATRIX: {
            const dl_matrix *o = (const dl_matrix *)h;
            cv_matrix(v, o->m, o->set != 0, base);
            break;
        }
        case DL_CLIP_RECT: {
            const dl_rect *o = (const dl_rect *)h;
            cv_clip_rect(v, o->r[0], o->r[1], o->r[2], o->r[3], o->op);
            break;
        }
        case DL_CLIP_PATH:
        case DL_PATH:
        case DL_LINES: play_path(v, (const dl_path *)h, h->kind); break;
        case DL_COLOR: draw_color(v, ((const dl_color *)h)->color, ((const dl_color *)h)->blend); break;
        case DL_RECT:
        case DL_RRECT:
        case DL_OVAL: {
            const dl_rect *o = (const dl_rect *)h;
            was = shot_on(&o->s);
            if (h->kind == DL_RECT) draw_rect(v, o->r[0], o->r[1], o->r[2], o->r[3], &o->p, sh_host(o->s.sh));
            else if (h->kind == DL_RRECT) draw_round_rect(v, o->r[0], o->r[1], o->r[2], o->r[3], o->r[4], o->r[5], &o->p, sh_host(o->s.sh));
            else draw_oval(v, o->r[0], o->r[1], o->r[2], o->r[3], &o->p, sh_host(o->s.sh));
            shot_off(&o->s, was);
            break;
        }
        case DL_BITMAP: {
            const dl_bitmap *o = (const dl_bitmap *)h;
            memcpy(src, o->src, sizeof(src));
            memcpy(dst, o->dst, sizeof(dst));
            draw_bitmap(v, o->b, src, dst, &o->p, NULL);
            break;
        }
        case DL_BITMAP_M: draw_bitmap_m(v, ((const dl_bitmap *)h)->b, ((const dl_bitmap *)h)->m, &((const dl_bitmap *)h)->p); break;
        case DL_TEXT: {
            const dl_text *o = (const dl_text *)h;
            was = shot_on(&o->s);
            draw_text(v, (const u16 *)(const void *)(o + 1), o->count, o->x, o->y, &o->p, o->face, sh_host(o->s.sh));
            shot_off(&o->s, was);
            break;
        }
        case DL_NODE: node_play(v, ((const dl_node *)h)->node, depth + 1); break;
        case DL_LIST: list_play(v, ((const dl_list *)h)->list, depth + 1); break;
        case DL_TEXTURE: draw_texture(v, ((const dl_texture *)h)->id, ((const dl_texture *)h)->dst, ((const dl_texture *)h)->alpha); break;
        default: break;
        }
    }
    while (v->depth >= floor_depth) cv_restore(v);
    dl_unref(l);
}

/* A node's outline as the clip: its rectangle, with round corners when it has a radius, or its path. */
static MG_NOINLINE void node_clip_outline(gcanvas *v, const gnode *n)
{
    gpath p;
    float rad[8];
    int i;
    if (n->outline_kind == 2) {
        path_view(&p, n->outline_path, n->outline_n);
        cv_clip_path(v, &p, n->outline_fill, MEGLER_GFX_CLIP_INTERSECT);
    } else if (n->radius > 0) {
        for (i = 0; i < 8; i++) rad[i] = n->radius;
        path_init(&p);
        path_round_rect(&p, n->outline[0], n->outline[1], n->outline[2], n->outline[3], rad);
        cv_clip_path(v, &p, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        path_free(&p);
    } else {
        cv_clip_rect(v, n->outline[0], n->outline[1], n->outline[2], n->outline[3], MEGLER_GFX_CLIP_INTERSECT);
    }
}

/*
 * A node whose layer has a colour filter, which a layer of the shell's
 * cannot have (save_layer takes an alpha and a mode): drawn into a bitmap of
 * the node's size and the bitmap put on with the layer's paint, as every
 * node was drawn before nodes kept lists.
 */
static MG_NOINLINE void node_through_bitmap(gcanvas *v, const gnode *n, gdlist *l, float w, float h, int depth)
{
    i32 bw = (i32)ceil(w), bh = (i32)ceil(h);
    float src[4], dst[4];
    gbitmap *b;
    gcanvas *t;
    gpaint p;
    b = bm_new(bw > 0 ? bw : 1, bh > 0 ? bh : 1);
    if (!b) return;
    t = cv_make(b);
    list_play(t, l, depth);
    paint_unpack(n->layer_paint, &p);
    p.p.color = (p.p.color & 0x00FFFFFFu) | (u32)n->layer_alpha << 24;
    src[0] = src[1] = dst[0] = dst[1] = 0;
    src[2] = dst[2] = (float)b->w;
    src[3] = dst[3] = (float)b->h;
    draw_bitmap(v, b, src, dst, &p, NULL);
    /* This temporary has no readers after the composition. Its canvas must
       submit pending commands, but reading its pixels back would be wasted. */
    if (t->host && G.canvas_discard) {
        while (t->depth > 0) cv_restore(t);
        GFX(G.canvas_discard(G.user, t->host));
        t->host = NULL;
        t->dirty = false;
    }
    cv_free(t);
    bm_free(b);
}

/*
 * A node drawn on a canvas as Android's RenderNodeDrawable draws it, with
 * the properties it has now: moved to (left, top), through its matrix; a
 * layer when it has one or is translucent, bounded by its own rectangle (and
 * its clip rectangle), put on with its alpha and the layer paint's mode;
 * otherwise clipped to its bounds when it clips to them, and to its clip
 * rectangle; then to its outline; then its list. Nothing is drawn of a node
 * with no list, no alpha, or an outline to clip to that is empty.
 */
static void node_play(gcanvas *v, gnode *live, int depth)
{
    float w, h, through[6], at[6] = { 1, 0, 0, 0, 1, 0 }, bounds[4];
    i32 d0, flags, blend = MEGLER_GFX_BLEND_SRC_OVER, filter = -1;
    gdlist *l;
    gnode copy, *n = &copy;
    float outline_path[128], *outline_made = NULL;
    if (!v || !live) return;
    /*
     * The node as it is now, read under the lock its setter writes it under (rnSet): the render thread draws a frame while
     * the main thread may set the properties for the next. Its outline's path is copied too, as the setter frees the one
     * it replaces.
     */
    mg_lock_acquire(&g_lock);
    l = live->list;
    if (l) l->refs++;
    copy = *live;
    if (copy.outline_path && copy.outline_n > 0) {
        float *to = copy.outline_n <= (i32)MG_COUNT(outline_path) ? outline_path : (outline_made = (float *)mg_alloc(sizeof(float) * (size_t)copy.outline_n));
        memcpy(to, copy.outline_path, sizeof(float) * (size_t)copy.outline_n);
        copy.outline_path = to;
    }
    mg_lock_release(&g_lock);
    flags = n->flags;
    if (flags & RN_LAYER_PAINT) {
        blend = blend_number(n->layer_paint[P_XFER]);
        filter = n->layer_paint[P_CFMODE];
    }
    if (!l || !(n->alpha > 0) || ((flags & RN_CLIP_OUTLINE) && !n->outline_kind) || ((flags & RN_LAYER) && blend == MEGLER_GFX_BLEND_DST)) {
        dl_unref(l);
        free(outline_made);
        return;
    }
    /*
     * Nothing of it can show where the clip is empty: a view's list under a parent that clips it away while part of the screen
     * is drawn (the views' lists: android.view.View, overlaid), every call of it skipped. Nor where the frame walk found it
     * drawing nowhere in the part of the screen the frame draws (G_damage): a node drawn once in the frame, by its box then.
     */
    if (cv_state(v)->empty ||
        (t_cull_v == v && copy.seen_no == t_cull_no && !copy.seen_multi && (!copy.seen_any || !rects_meet(copy.seen_box, cv_state(v)->clip)))) {
        if (!cv_state(v)->empty) g_culled++;
        dl_unref(l);
        free(outline_made);
        return;
    }
    w = n->right - n->left;
    h = n->bottom - n->top;
    d0 = v->depth;
    cv_save(v, NULL, false, 255, MEGLER_GFX_BLEND_SRC_OVER, NULL);
    at[2] = n->left;
    at[5] = n->top;
    if (flags & RN_IDENTITY) memcpy(through, at, sizeof(through));
    else matrix_concat(at, n->m, through);
    cv_matrix(v, through, false, NULL);
    if (flags & RN_LAYER) {
        bounds[0] = bounds[1] = 0;
        bounds[2] = w;
        bounds[3] = h;
        if (flags & RN_CLIP_RECT) {
            if (n->clip[0] > bounds[0]) bounds[0] = n->clip[0];
            if (n->clip[1] > bounds[1]) bounds[1] = n->clip[1];
            if (n->clip[2] < bounds[2]) bounds[2] = n->clip[2];
            if (n->clip[3] < bounds[3]) bounds[3] = n->clip[3];
        }
        if (filter >= 0) {
            cv_clip_rect(v, bounds[0], bounds[1], bounds[2], bounds[3], MEGLER_GFX_CLIP_INTERSECT);
            if (flags & RN_CLIP_OUTLINE) node_clip_outline(v, n);
            node_through_bitmap(v, n, l, w, h, depth);
            while (v->depth > d0) cv_restore(v);
            dl_unref(l);
            free(outline_made);
            return;
        }
        /* A layer with no area is no layer to cv_save: the node then draws nothing. */
        if (!(bounds[2] > bounds[0]) || !(bounds[3] > bounds[1])) cv_clip_rect(v, 0, 0, 0, 0, MEGLER_GFX_CLIP_INTERSECT);
        else cv_save(v, bounds, true, n->layer_alpha, blend, NULL);
    } else {
        if (flags & RN_CLIP_BOUNDS) cv_clip_rect(v, 0, 0, w, h, MEGLER_GFX_CLIP_INTERSECT);
        if (flags & RN_CLIP_RECT) cv_clip_rect(v, n->clip[0], n->clip[1], n->clip[2], n->clip[3], MEGLER_GFX_CLIP_INTERSECT);
    }
    if (flags & RN_CLIP_OUTLINE) node_clip_outline(v, n);
    list_play(v, l, depth);
    while (v->depth > d0) cv_restore(v);
    dl_unref(l);
    free(outline_made);
}

/* --------------------------------------- Gfx: what a frame changes, node by node */

/*
 * The frame walk (Gfx.damage). Before a frame is handed to the render thread the main thread goes through what the frame
 * draws -- the window's calls and every node and list they draw, with the matrix and clip each is drawn with -- and finds
 * what changed since the frame before, node by node, as Android's renderer does (HWUI's DamageAccumulator), so that only
 * that part of the screen is drawn again. The views' invalidations said rectangles of their boxes, and Compose invalidates
 * its whole view (AndroidComposeView) for a change of any of its layers -- a progress bar's, a scroll -- so that Spotify
 * drew a third of the screen again in every frame (package 662).
 *
 * Each node keeps where the walk last found it (gnode.seen_*), and is compared with that:
 *  - drawn with another matrix or clip, or with properties changed (ver), or not found before: where it was and where it
 *    is now are damaged;
 *  - its list another one, where the walk answers for what is in it -- any node but a View's own, and a View's own whose
 *    whole box was invalidated (rnDefer): the two compared call by call (diff_pass), each with its own matrices and clips.
 *    A call that differs, or is drawn with another matrix or clip, damages where it drew and where it draws; a node in it
 *    is compared as a node. Lists of another shape, or with more than WALK_CHANGED calls changed, damage the whole node;
 *  - otherwise nothing of its own; the nodes it draws are compared.
 * What changed in a View's own list otherwise is the View's to say (the rectangles of View.meglerDamage). The nodes the
 * window's own calls draw are compared likewise. Where a node draws is the box of its own calls (list_own: worked out once
 * for each list, which never changes once recorded) and of its nodes', each as clipped.
 *
 * The render thread then leaves out a node that draws nowhere in the part of the screen being drawn (node_play), by the
 * box this walk found for it.
 */

#define WALK_STATES 4096               /* the matrices and clips saved inside each other, over all the lists being walked */
#define WALK_CHANGED 24                /* the calls of a list compared one by one at most; beyond, the whole node changed */
#define WALK_FAR 1e30f                 /* no clip yet: a list's own calls measured where it starts (list_own) */

enum { W_UPDATE, W_COMPARE };

typedef struct wstate {
    float m[6];
    float clip[4];                     /* the clip's box, in the coordinates the walk started in */
    bool whole;                        /* inside a blurred layer, which spreads what is drawn: all of the clip changes */
} wstate;

static struct {
    wstate *st;                        /* WALK_STATES of them: each list walked uses those above the ones its caller uses */
    i32 used;
    u32 no;                            /* this walk's number; never 0 */
    gbox damage;
    /* For the log: the walks since `since`, the nodes found, those moved or changed, the lists compared call by call and
       those of them taken whole, the share of the screen damaged, the time. */
    u32 walks, nodes, moved, diffed, whole_diffs;
    double area, screen;
    u64 ns, since;
} W;

/* The walk done for the frame about to be handed over (G_damage to G_frame). */
static u32 g_walk_ready;

/* r cut down to clip; whether anything is left. */
static bool rect_and(float *r, const float *clip)
{
    if (clip[0] > r[0]) r[0] = clip[0];
    if (clip[1] > r[1]) r[1] = clip[1];
    if (clip[2] < r[2]) r[2] = clip[2];
    if (clip[3] < r[3]) r[3] = clip[3];
    return r[2] > r[0] && r[3] > r[1];
}

static void box_add(gbox *o, const float *r)
{
    if (!(r[2] > r[0]) || !(r[3] > r[1])) return;
    if (!o->any) {
        o->l = r[0];
        o->t = r[1];
        o->r = r[2];
        o->b = r[3];
        o->any = true;
        return;
    }
    if (r[0] < o->l) o->l = r[0];
    if (r[1] < o->t) o->t = r[1];
    if (r[2] > o->r) o->r = r[2];
    if (r[3] > o->b) o->b = r[3];
}

static void box_add_box(gbox *o, const gbox *b)
{
    float r[4];
    if (!b->any) return;
    r[0] = b->l;
    r[1] = b->t;
    r[2] = b->r;
    r[3] = b->b;
    box_add(o, r);
}

static void walk_damage(const float *r) { box_add(&W.damage, r); }

/* The box round a built path's points, its curves' control points with them, so no smaller than the path. False: no point. */
static bool path_hull(const float *v, i32 n, float *out)
{
    gbox o;
    i32 i = 0;
    int k, count;
    memset(&o, 0, sizeof(o));
    while (i < n) {
        count = verb_floats((int)v[i++]);
        for (k = 0; k + 1 < count && i + k + 1 < n; k += 2) box_point(&o, v[i + k], v[i + k + 1]);
        i += count;
    }
    out[0] = o.l;
    out[1] = o.t;
    out[2] = o.r;
    out[3] = o.b;
    return o.any;
}

/* A box grown by how far a stroke reaches past the line it follows -- half its width, more at a mitred corner or a square
   cap -- when `stroked`, and by the paint's shadow where it has one. */
static void paint_reach(const gpaint *p, bool stroked, float *r)
{
    float s, sh[4];
    if (stroked) {
        s = p->p.stroke_width > 0 ? p->p.stroke_width : 1;
        s *= 0.5f * (p->p.join == MEGLER_GFX_JOIN_MITER && p->p.miter_limit > 1.5f ? p->p.miter_limit : 1.5f);
        r[0] -= s;
        r[1] -= s;
        r[2] += s;
        r[3] += s;
    }
    if (!(p->p.shadow_radius > 0) || !(p->p.shadow_color >> 24)) return;
    s = p->p.shadow_radius * 2 + 1;
    sh[0] = r[0] + p->p.shadow_dx - s;
    sh[1] = r[1] + p->p.shadow_dy - s;
    sh[2] = r[2] + p->p.shadow_dx + s;
    sh[3] = r[3] + p->p.shadow_dy + s;
    if (sh[0] < r[0]) r[0] = sh[0];
    if (sh[1] < r[1]) r[1] = sh[1];
    if (sh[2] > r[2]) r[2] = sh[2];
    if (sh[3] > r[3]) r[3] = sh[3];
}

/* A rectangle as given, its sides in order. */
static void rect_sorted(const float *r, float *out)
{
    out[0] = r[0] < r[2] ? r[0] : r[2];
    out[1] = r[1] < r[3] ? r[1] : r[3];
    out[2] = r[0] < r[2] ? r[2] : r[0];
    out[3] = r[1] < r[3] ? r[3] : r[1];
}

/* A run of text's box: the ink the shell says it has (text_bounds), where the paint's alignment puts it, with the bars of an
   underline and a strike-through (draw_text) and the shadow. */
static int text_local(const dl_text *o, float *out)
{
    const u16 *text = (const u16 *)(const void *)(o + 1);
    const megler_gfx_paint *p = &o->p.p;
    float ink[4] = { 0, 0, 0, 0 }, r[4], x = o->x, width = 0, size = p->text_size, thick;
    gbox b;
    if (o->count <= 0) return 0;
    memset(&b, 0, sizeof(b));
    if (p->text_align != MEGLER_GFX_ALIGN_LEFT || o->p.underline || o->p.strike) width = text_width(text, o->count, &o->p, o->face);
    if (p->text_align == MEGLER_GFX_ALIGN_CENTER) x -= width / 2;
    else if (p->text_align == MEGLER_GFX_ALIGN_RIGHT) x -= width;
    if (HAS(text_bounds)) {
        GFX(G.text_bounds(G.user, text, o->count, p, o->face, ink));
    } else {
        /* (A shell that cannot say: a box no run is likely to leave, two ems a code unit either way and two ems up.) */
        ink[0] = -size * 2 * (float)o->count;
        ink[1] = -size * 2;
        ink[2] = size * 2 * (float)o->count;
        ink[3] = size;
    }
    r[0] = x + ink[0];
    r[1] = o->y + ink[1];
    r[2] = x + ink[2];
    r[3] = o->y + ink[3];
    box_add(&b, r);
    if (o->p.underline || o->p.strike) {
        thick = size / 18 > 1 ? size / 18 : 1;
        r[0] = x;
        r[1] = o->y - size * 0.3f;
        r[2] = x + width;
        r[3] = o->y + size / 9 + thick;
        box_add(&b, r);
    }
    if (!b.any) return 0;
    out[0] = b.l;
    out[1] = b.t;
    out[2] = b.r;
    out[3] = b.b;
    paint_reach(&o->p, false, out);
    return 1;
}

/* A drawing call's box in the coordinates it is drawn in: 0 it draws nothing, 1 the box in out, 2 all of the clip. */
static int op_local(const dl_head *h, float *out)
{
    switch (h->kind) {
    case DL_COLOR:
        return ((const dl_color *)(const void *)h)->blend == MEGLER_GFX_BLEND_DST ? 0 : 2;
    case DL_RECT:
    case DL_RRECT:
    case DL_OVAL: {
        const dl_rect *o = (const dl_rect *)(const void *)h;
        rect_sorted(o->r, out);
        paint_reach(&o->p, o->p.p.style != MEGLER_GFX_FILL, out);
        return 1;
    }
    case DL_PATH: {
        const dl_path *o = (const dl_path *)(const void *)h;
        if ((o->fill & FILL_INVERSE) && o->p.p.style != MEGLER_GFX_STROKE) return 2;
        if (!path_hull((const float *)(const void *)(o + 1), o->n, out)) return 0;
        paint_reach(&o->p, o->p.p.style != MEGLER_GFX_FILL, out);
        return 1;
    }
    case DL_LINES: {
        const dl_path *o = (const dl_path *)(const void *)h;
        const float *f = (const float *)(const void *)(o + 1);
        gbox b;
        i32 i;
        memset(&b, 0, sizeof(b));
        for (i = 0; i + 1 < o->n; i += 2) box_point(&b, f[i], f[i + 1]);
        if (!b.any) return 0;
        out[0] = b.l;
        out[1] = b.t;
        out[2] = b.r;
        out[3] = b.b;
        paint_reach(&o->p, true, out);
        return 1;
    }
    case DL_BITMAP:
        rect_sorted(((const dl_bitmap *)(const void *)h)->dst, out);
        return 1;
    case DL_BITMAP_M: {
        const dl_bitmap *o = (const dl_bitmap *)(const void *)h;
        if (!o->b) return 0;
        matrix_box(o->m, 0, 0, (float)o->b->w, (float)o->b->h, out);
        return 1;
    }
    case DL_TEXT:
        return text_local((const dl_text *)(const void *)h, out);
    case DL_TEXTURE:
        rect_sorted(((const dl_texture *)(const void *)h)->dst, out);
        return 1;
    default:
        return 0;
    }
}

/* A drawing call's box in the walk's coordinates, drawn from state s, a pixel wider all round for antialiasing; false when nothing of it is inside the clip. */
static bool op_box(const dl_head *h, const wstate *s, float *out)
{
    float r[4];
    int k = op_local(h, r);
    if (!k) return false;
    if (k == 2 || s->whole) {
        memcpy(out, s->clip, sizeof(s->clip));
    } else {
        matrix_box(s->m, r[0], r[1], r[2], r[3], out);
        out[0] -= 1;
        out[1] -= 1;
        out[2] += 1;
        out[3] += 1;
    }
    return rect_and(out, s->clip);
}

/*
 * A call that changes the matrix or the clip applied to a walk's state, as list_play applies it to a canvas: `base` the
 * matrix the list began with, which a recorded setMatrix is relative to. A layer's bounds are a clip, and a blurred layer
 * spreads what is drawn into it over all of it. A save is the caller's to make. False: the call is none of these.
 */
static bool walk_state_op(wstate *s, const dl_head *h, const float *base)
{
    float r[4];
    switch (h->kind) {
    case DL_SAVE_LAYER: {
        const dl_layer *o = (const dl_layer *)(const void *)h;
        if (o->bounded) {
            matrix_box(s->m, o->bounds[0], o->bounds[1], o->bounds[2], o->bounds[3], r);
            rect_and(s->clip, r);
        }
        if (o->blurred) s->whole = true;
        return true;
    }
    case DL_MATRIX: {
        const dl_matrix *o = (const dl_matrix *)(const void *)h;
        if (o->set) matrix_concat(base, o->m, s->m);
        else matrix_concat(s->m, o->m, s->m);
        return true;
    }
    case DL_CLIP_RECT: {
        const dl_rect *o = (const dl_rect *)(const void *)h;
        if (o->op == MEGLER_GFX_CLIP_INTERSECT) {
            matrix_box(s->m, o->r[0], o->r[1], o->r[2], o->r[3], r);
            rect_and(s->clip, r);
        }
        return true;
    }
    case DL_CLIP_PATH: {
        const dl_path *o = (const dl_path *)(const void *)h;
        if (o->op != MEGLER_GFX_CLIP_INTERSECT) return true;
        if (path_hull((const float *)(const void *)(o + 1), o->n, r)) {
            matrix_box(s->m, r[0], r[1], r[2], r[3], r);
            rect_and(s->clip, r);
        } else {
            /* (Nothing is inside a path with nothing in it.) */
            s->clip[2] = s->clip[0];
            s->clip[3] = s->clip[1];
        }
        return true;
    }
    default:
        return false;
    }
}

/* Whether what a layer's call draws changes all of its clip: a layer put on by another mode than SRC_OVER, or blurred. */
static bool layer_all(const dl_head *h)
{
    const dl_layer *o = (const dl_layer *)(const void *)h;
    return o->blurred || o->blend != MEGLER_GFX_BLEND_SRC_OVER;
}

static bool far_box(const float *r) { return r[0] <= -WALK_FAR / 2 || r[1] <= -WALK_FAR / 2 || r[2] >= WALK_FAR / 2 || r[3] >= WALK_FAR / 2; }

/*
 * The box a list's own calls draw in -- not its nodes', which the walk goes into, but the pictures' it draws -- in the
 * coordinates the list starts in, and whether it draws nodes or pictures at all (gdlist.own_kind, own, kids). Worked out
 * the first time the walk needs it: a list is not written to again once recorded.
 */
static void list_own(gdlist *l)
{
    wstate st[64];
    i32 d = 0;
    size_t at;
    gbox own;
    float r[4];
    bool all = false;
    int k;
    l->own_kind = 3;                   /* (meanwhile: a list that drew itself would draw everything) */
    l->kids = false;
    memset(&own, 0, sizeof(own));
    matrix_identity(st[0].m);
    st[0].clip[0] = st[0].clip[1] = -WALK_FAR;
    st[0].clip[2] = st[0].clip[3] = WALK_FAR;
    st[0].whole = false;
    for (at = 0; at < l->n; at += ((const dl_head *)(const void *)(l->ops + at))->size) {
        const dl_head *h = (const dl_head *)(const void *)(l->ops + at);
        if (h->kind == DL_NODE || h->kind == DL_LIST) l->kids = true;
        if (all) continue;
        if (h->kind == DL_SAVE || h->kind == DL_SAVE_LAYER) {
            if (d + 1 >= (i32)MG_COUNT(st)) {
                all = true;
                continue;
            }
            st[d + 1] = st[d];
            d++;
            if (h->kind == DL_SAVE) continue;
            walk_state_op(&st[d], h, st[0].m);
            /* (A layer put on by another mode changes what is under all of it, whatever is drawn into it.) */
            if (layer_all(h)) {
                if (far_box(st[d].clip)) all = true;
                else box_add(&own, st[d].clip);
            }
            continue;
        }
        if (h->kind == DL_RESTORE) {
            if (d > 0) d--;
            continue;
        }
        if (walk_state_op(&st[d], h, st[0].m) || h->kind == DL_NODE) continue;
        if (h->kind == DL_LIST) {
            gdlist *sub = ((const dl_list *)(const void *)h)->list;
            if (!sub) continue;
            if (!sub->own_kind) list_own(sub);
            if (sub->own_kind == 3 || (sub->own_kind == 2 && st[d].whole)) {
                memcpy(r, st[d].clip, sizeof(r));
            } else if (sub->own_kind == 2) {
                matrix_box(st[d].m, sub->own[0], sub->own[1], sub->own[2], sub->own[3], r);
                if (!rect_and(r, st[d].clip)) continue;
            } else {
                continue;
            }
        } else {
            k = op_local(h, r);
            if (!k) continue;
            if (k == 2 || st[d].whole) {
                memcpy(r, st[d].clip, sizeof(r));
            } else {
                matrix_box(st[d].m, r[0], r[1], r[2], r[3], r);
                if (!rect_and(r, st[d].clip)) continue;
            }
        }
        if (far_box(r)) all = true;
        else box_add(&own, r);
    }
    l->own[0] = own.l;
    l->own[1] = own.t;
    l->own[2] = own.r;
    l->own[3] = own.b;
    l->own_kind = all ? 3 : own.any ? 2 : 1;
}

/* A list's own box (list_own) drawn from state s into out, a pixel wider all round. */
static void own_box(gdlist *l, const wstate *s, gbox *out)
{
    float r[4];
    if (!l->own_kind) list_own(l);
    if (l->own_kind == 3 || (l->own_kind == 2 && s->whole)) {
        box_add(out, s->clip);
    } else if (l->own_kind == 2) {
        matrix_box(s->m, l->own[0], l->own[1], l->own[2], l->own[3], r);
        r[0] -= 1;
        r[1] -= 1;
        r[2] += 1;
        r[3] += 1;
        if (rect_and(r, s->clip)) box_add(out, r);
    }
}

static void walk_node(gnode *live, const wstate *at, int mode, int depth, gbox *out);

/* A list drawn from state `at`: its own box and its nodes' into out, the nodes compared (W_COMPARE) or only noted (W_UPDATE). */
static void walk_list(gdlist *l, const wstate *at, int mode, int depth, gbox *out)
{
    i32 base = W.used, d;
    size_t pos;
    if (!l || depth > DL_DEPTH) return;
    own_box(l, at, out);
    if (!l->kids) return;
    if (base >= WALK_STATES) {
        /* (Saved too deep to follow: all it may draw is damaged.) */
        walk_damage(at->clip);
        return;
    }
    W.st[base] = *at;
    d = base;
    for (pos = 0; pos < l->n; pos += ((const dl_head *)(const void *)(l->ops + pos))->size) {
        const dl_head *h = (const dl_head *)(const void *)(l->ops + pos);
        if (h->kind == DL_SAVE || h->kind == DL_SAVE_LAYER) {
            if (d + 1 >= WALK_STATES) {
                walk_damage(W.st[d].clip);
                break;
            }
            W.st[d + 1] = W.st[d];
            d++;
            if (h->kind == DL_SAVE_LAYER) walk_state_op(&W.st[d], h, at->m);
            continue;
        }
        if (h->kind == DL_RESTORE) {
            if (d > base) d--;
            continue;
        }
        if (walk_state_op(&W.st[d], h, at->m)) continue;
        if (h->kind != DL_NODE && h->kind != DL_LIST) continue;
        W.used = d + 1;
        if (h->kind == DL_NODE) walk_node(((const dl_node *)(const void *)h)->node, &W.st[d], mode, depth + 1, out);
        else walk_list(((const dl_list *)(const void *)h)->list, &W.st[d], mode, depth + 1, out);
    }
    W.used = base;
}

static bool same_state(const wstate *a, const wstate *b)
{
    return a->whole == b->whole && memcmp(a->m, b->m, sizeof(a->m)) == 0 && memcmp(a->clip, b->clip, sizeof(a->clip)) == 0;
}

/*
 * A node's list against the one the walk last saw it draw (`was`), both from the node's state: call by call, each keeping
 * its own matrices and clips. Without `act` it only counts: -1 when the lists are of another shape -- calls added or taken
 * away, of other kinds, another picture drawn, saved too deep -- else how many drawing calls differ or are drawn
 * with another matrix or clip. With `act`, those damage where they drew and where they draw, a layer whose call or state
 * differs damages all of it, the nodes are compared as nodes (another node in an old one's place damages where the old one
 * was), and the nodes' boxes go into the node's box.
 */
static int diff_pass(const gdlist *was, const gdlist *now, const wstate *ns, int depth, gbox *box, bool act)
{
    i32 base = W.used, half, da, db;
    size_t pa, pb;
    int changed = 0;
    float r[4];
    if (WALK_STATES - base < 4) return -1;
    half = (WALK_STATES - base) / 2;
    da = base;
    db = base + half;
    W.st[da] = *ns;
    W.st[db] = *ns;
    for (pa = pb = 0; pa < was->n && pb < now->n;) {
        const dl_head *ha = (const dl_head *)(const void *)(was->ops + pa), *hb = (const dl_head *)(const void *)(now->ops + pb);
        /* (Calls of one kind may differ in size: a run of text, a path. Only a call of another kind is another shape.) */
        bool same = ha->size == hb->size && memcmp(ha, hb, ha->size) == 0;
        if (ha->kind != hb->kind) return -1;
        pa += ha->size;
        pb += hb->size;
        switch (ha->kind) {
        case DL_SAVE:
        case DL_SAVE_LAYER:
            if (da + 1 >= base + half || db + 1 >= base + 2 * half) {
                W.used = base;
                return -1;
            }
            W.st[da + 1] = W.st[da];
            W.st[db + 1] = W.st[db];
            da++;
            db++;
            if (ha->kind == DL_SAVE) break;
            walk_state_op(&W.st[da], ha, ns->m);
            walk_state_op(&W.st[db], hb, ns->m);
            if (same && same_state(&W.st[da], &W.st[db])) break;
            changed++;
            if (act) {
                walk_damage(W.st[da].clip);
                walk_damage(W.st[db].clip);
            }
            break;
        case DL_RESTORE:
            if (da > base) da--;
            if (db > base + half) db--;
            break;
        case DL_MATRIX:
        case DL_CLIP_RECT:
        case DL_CLIP_PATH:
            walk_state_op(&W.st[da], ha, ns->m);
            walk_state_op(&W.st[db], hb, ns->m);
            break;
        case DL_NODE:
            if (act) {
                gnode *na = ((const dl_node *)(const void *)ha)->node, *nb = ((const dl_node *)(const void *)hb)->node;
                bool any = false;
                if (na != nb && na) {
                    mg_lock_acquire(&g_lock);
                    any = na->seen && na->seen_any;
                    memcpy(r, na->seen_box, sizeof(r));
                    mg_lock_release(&g_lock);
                }
                if (any) walk_damage(r);
                W.used = db + 1;
                walk_node(nb, &W.st[db], W_COMPARE, depth + 1, box);
                W.used = base;
            }
            break;
        case DL_LIST: {
            gdlist *la = ((const dl_list *)(const void *)ha)->list, *lb = ((const dl_list *)(const void *)hb)->list;
            bool moved;
            if (la != lb) {
                W.used = base;
                return -1;
            }
            moved = !same_state(&W.st[da], &W.st[db]);
            if (moved) changed++;
            if (act && lb) {
                if (moved) {
                    gbox ob;
                    memset(&ob, 0, sizeof(ob));
                    own_box(la, &W.st[da], &ob);
                    own_box(lb, &W.st[db], &ob);
                    box_add_box(&W.damage, &ob);
                }
                W.used = db + 1;
                walk_list(lb, &W.st[db], W_COMPARE, depth + 1, box);
                W.used = base;
            }
            break;
        }
        default:
            if (same && same_state(&W.st[da], &W.st[db])) break;
            changed++;
            if (act) {
                if (op_box(ha, &W.st[da], r)) walk_damage(r);
                if (op_box(hb, &W.st[db], r)) walk_damage(r);
            }
            break;
        }
    }
    W.used = base;
    return pa < was->n || pb < now->n ? -1 : changed;
}

/* A node whose list is another than the walk last saw (walk_node): compared call by call, or damaged whole where they differ too much. */
static void walk_diff(const gnode *n, gdlist *was, gdlist *now, const wstate *ns, int depth, gbox *box)
{
    int changed = was && now ? diff_pass(was, now, ns, depth, box, false) : -1;
    W.diffed++;
    if (changed < 0 || changed > WALK_CHANGED) {
        W.whole_diffs++;
        walk_list(now, ns, W_UPDATE, depth + 1, box);
        if (n->seen_any) walk_damage(n->seen_box);
        box_add_box(&W.damage, box);
        return;
    }
    diff_pass(was, now, ns, depth, box, true);
    own_box(now, ns, box);
}

/*
 * A node drawn from state `at` (walk_list): where it draws worked out -- its place, matrix and clips as node_play draws it
 * -- compared with where the walk last found it, when the walk answers for it (see above), and noted for the next walk and
 * for the render thread (node_play). Its box goes into out.
 */
static void walk_node(gnode *live, const wstate *at, int mode, int depth, gbox *out)
{
    gnode n;
    gdlist *l, *was;
    wstate ns;
    gbox box;
    float lc[4], ob[4], r[4], to[6] = { 1, 0, 0, 0, 1, 0 };
    i32 blend = MEGLER_GFX_BLEND_SRC_OVER, filter = -1;
    bool clips = false, draws, own, all, ob_any = false;
    if (!live || depth > DL_DEPTH) return;
    /* The node as it is now, as node_play reads it; its outline path's box worked out under the lock rnSet frees it under. */
    mg_lock_acquire(&g_lock);
    l = live->list;
    if (l) l->refs++;
    n = *live;
    if (n.outline_kind == 2 && n.outline_path && n.outline_n > 0) ob_any = path_hull(n.outline_path, n.outline_n, ob);
    mg_lock_release(&g_lock);
    W.nodes++;
    if (n.flags & RN_LAYER_PAINT) {
        blend = blend_number(n.layer_paint[P_XFER]);
        filter = n.layer_paint[P_CFMODE];
    }
    draws = l && n.alpha > 0 && !((n.flags & RN_CLIP_OUTLINE) && !n.outline_kind) && !((n.flags & RN_LAYER) && blend == MEGLER_GFX_BLEND_DST);
    /* (A layer put on by another mode than SRC_OVER, or through a colour filter, changes all of it whatever is drawn into it.) */
    all = (n.flags & RN_LAYER) && (blend != MEGLER_GFX_BLEND_SRC_OVER || filter >= 0);
    to[2] = n.left;
    to[5] = n.top;
    matrix_concat(at->m, to, ns.m);
    if (!(n.flags & RN_IDENTITY)) matrix_concat(ns.m, n.m, ns.m);
    lc[0] = lc[1] = -WALK_FAR;
    lc[2] = lc[3] = WALK_FAR;
    if (n.flags & (RN_LAYER | RN_CLIP_BOUNDS)) {
        r[0] = r[1] = 0;
        r[2] = n.right - n.left;
        r[3] = n.bottom - n.top;
        rect_and(lc, r);
        clips = true;
    }
    if (n.flags & RN_CLIP_RECT) {
        rect_and(lc, n.clip);
        clips = true;
    }
    if ((n.flags & RN_CLIP_OUTLINE) && n.outline_kind == 1) {
        rect_and(lc, n.outline);
        clips = true;
    }
    if ((n.flags & RN_CLIP_OUTLINE) && n.outline_kind == 2) {
        if (ob_any) rect_and(lc, ob);
        else lc[2] = lc[0];
        clips = true;
    }
    memcpy(ns.clip, at->clip, sizeof(ns.clip));
    if (clips) {
        if (lc[2] > lc[0] && lc[3] > lc[1]) {
            matrix_box(ns.m, lc[0], lc[1], lc[2], lc[3], r);
            if (!rect_and(ns.clip, r)) draws = false;
        } else {
            draws = false;
        }
    }
    if (!(ns.clip[2] > ns.clip[0]) || !(ns.clip[3] > ns.clip[1])) draws = false;
    ns.whole = at->whole;
    /*
     * Any node moved, changed in its properties or new is damaged, a View's too: a View moved by offsetTopAndBottom, as a
     * ListView scrolls its rows, says only where it now is, and the place it left was the list's whole invalidation's
     * before the walk. What changed in a View's list is the View's to say unless it left that to the walk (own).
     */
    own = !(n.flags & RN_VIEW) || (n.defer && n.ends > n.defer_min);
    memset(&box, 0, sizeof(box));
    if (mode == W_COMPARE && n.seen) {
        bool moved = n.seen_ver != n.ver || n.seen_whole != ns.whole || memcmp(n.seen_m, ns.m, sizeof(ns.m)) != 0 ||
                     memcmp(n.seen_clip, ns.clip, sizeof(ns.clip)) != 0;
        if (moved) {
            if (draws) walk_list(l, &ns, W_UPDATE, depth + 1, &box);
            if (draws && all) box_add(&box, ns.clip);
            if (n.seen_any) walk_damage(n.seen_box);
            box_add_box(&W.damage, &box);
            W.moved++;
        } else if (own && l != n.seen_list) {
            if (draws) walk_diff(&n, n.seen_list, l, &ns, depth, &box);
            if (draws && all) box_add(&box, ns.clip);
        } else if (draws) {
            walk_list(l, &ns, W_COMPARE, depth + 1, &box);
            if (all) box_add(&box, ns.clip);
        }
    } else if (mode == W_COMPARE) {
        /* (Not found before: all it draws is new.) */
        if (draws) walk_list(l, &ns, W_UPDATE, depth + 1, &box);
        if (draws && all) box_add(&box, ns.clip);
        box_add_box(&W.damage, &box);
    } else if (draws) {
        walk_list(l, &ns, mode, depth + 1, &box);
        if (all) box_add(&box, ns.clip);
    }
    mg_lock_acquire(&g_lock);
    was = live->seen_list;
    live->seen_list = l;
    live->seen_ver = n.ver;
    memcpy(live->seen_m, ns.m, sizeof(ns.m));
    memcpy(live->seen_clip, ns.clip, sizeof(ns.clip));
    live->seen_whole = ns.whole;
    if (live->seen_no == W.no) {
        /* Drawn twice in one frame: the box round both, and the render thread does not leave it out by it. */
        live->seen_multi = true;
        if (box.any && live->seen_any) {
            gbox both = box;
            box_add(&both, live->seen_box);
            box = both;
        }
    } else {
        live->seen_multi = false;
    }
    if (box.any) {
        live->seen_box[0] = box.l;
        live->seen_box[1] = box.t;
        live->seen_box[2] = box.r;
        live->seen_box[3] = box.b;
    }
    live->seen_any = box.any || (live->seen_multi && live->seen_any);
    live->seen_no = W.no;
    live->seen = true;
    /* (A View's own node: an invalidation answered, unless one made while the list was being recorded waits for the next.) */
    if ((n.flags & RN_VIEW) && own) {
        if (n.ends > n.defer_max) live->defer = false;
        else live->defer_min = n.defer_max;
    }
    mg_lock_release(&g_lock);
    dl_unref(was);
    box_add_box(out, &box);
}

/*
 * (long rec, int width, int height, int[4] out, boolean compare) -> boolean: Gfx.damage. The frame the recording canvas has
 * recorded so far walked (above), from the screen's corner with the screen as the clip, before it is handed over; `compare`
 * false (a frame drawn whole) only notes where every node is. True with the rectangle of the screen that changed in out[0..3],
 * false when nothing did.
 */
N(G_damage)
{
    gcanvas *v = cv_of(c->args[0].j);
    i32 *out = (i32 *)elements(c->args[3].l, 'I', 4), w = c->args[1].i, h = c->args[2].i;
    wstate at;
    gbox box;
    u64 t0 = mg_time_monotonic_ns(), now;
    double area = 0;
    if (!v || !v->rec || !out || w <= 0 || h <= 0) return;
    if (!W.st) W.st = (wstate *)mg_alloc(sizeof(wstate) * WALK_STATES);
    if (++W.no == 0) W.no = 1;
    W.used = 0;
    memset(&W.damage, 0, sizeof(W.damage));
    memset(&box, 0, sizeof(box));
    matrix_identity(at.m);
    at.clip[0] = at.clip[1] = 0;
    at.clip[2] = (float)w;
    at.clip[3] = (float)h;
    at.whole = false;
    walk_list(v->rec, &at, c->args[4].z ? W_COMPARE : W_UPDATE, 0, &box);
    g_walk_ready = W.no;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (W.damage.any) {
        out[0] = whole(floor(W.damage.l > 0 ? W.damage.l : 0));
        out[1] = whole(floor(W.damage.t > 0 ? W.damage.t : 0));
        out[2] = whole(ceil(W.damage.r < (float)w ? W.damage.r : (float)w));
        out[3] = whole(ceil(W.damage.b < (float)h ? W.damage.b : (float)h));
        if (out[2] > out[0] && out[3] > out[1]) {
            c->ret.z = 1;
            area = (double)(out[2] - out[0]) * (out[3] - out[1]);
        }
    }
    now = mg_time_monotonic_ns();
    W.walks++;
    W.area += area;
    W.screen += (double)w * h;
    W.ns += now - t0;
    if (!W.since) W.since = now;
    if (now - W.since >= 5000000000ull && W.walks) {
        mg_info("gfx: the frame walk, %.1f s: %u frames, %.0f nodes and %.2f ms each; a frame %.1f nodes moved or changed, %.1f lists compared "
                "call by call (%.1f of them taken whole); what the nodes changed, %.1f%% of the screen a frame",
                (double)(now - W.since) / 1e9, (unsigned)W.walks, (double)W.nodes / W.walks, (double)W.ns / 1e6 / W.walks,
                (double)W.moved / W.walks, (double)W.diffed / W.walks, (double)W.whole_diffs / W.walks, W.screen > 0 ? 100 * W.area / W.screen : 0);
        W.walks = W.nodes = W.moved = W.diffed = W.whole_diffs = 0;
        W.area = W.screen = 0;
        W.ns = 0;
        W.since = now;
    }
}

/* ---------------------------------------------- Gfx: display lists natives */

/* (long cv, long node): Canvas.drawRenderNode. A recording canvas writes the node itself to its list; any other draws it now. */
N(G_cvDrawNode)
{
    gcanvas *v = cv_of(c->args[0].j);
    gnode *n = rn_of(c->args[1].j);
    if (!v || !n) return;
    if (v->rec) {
        dl_node *o = (dl_node *)dl_put(v->rec, DL_NODE, sizeof(*o), 0);
        rn_ref(n);
        o->node = n;
        return;
    }
    node_play(v, n, 0);
}

/* (long cv, long list): a Picture drawn. A recording canvas writes the list to its own, as it is; any other draws it now. */
N(G_cvDrawList)
{
    gcanvas *v = cv_of(c->args[0].j);
    gdlist *l = dl_of(c->args[1].j);
    if (!v || !l) return;
    if (v->rec) {
        dl_list *o = (dl_list *)dl_put(v->rec, DL_LIST, sizeof(*o), 0);
        dl_ref(l);
        o->list = l;
        return;
    }
    list_play(v, l, 0);
}

/* (long list): the Java's Picture is done with the list it had (a new recording, close(), the collector). */
N(G_dlFree) { dl_unref(dl_of(c->args[0].j)); }

/* () -> long: a RenderNode's native half, with Android's defaults: at (0, 0) with no size, no transform, opaque, clipping to its bounds. */
N(G_rnNew)
{
    gnode *n = (gnode *)mg_alloc(sizeof(*n));
    n->magic = RN_MAGIC;
    n->refs = 1;
    matrix_identity(n->m);
    n->alpha = 1;
    n->flags = RN_IDENTITY | RN_CLIP_BOUNDS;
    n->layer_alpha = 255;
    mg_lock_acquire(&g_lock);
    g_rn_count++;
    mg_lock_release(&g_lock);
    c->ret.j = handle(n);
}

/* (long node): the Java's RenderNode is gone. The node stays while a list draws it. */
N(G_rnFree)
{
    gnode *n = rn_of(c->args[0].j);
    if (n) rn_unref(n);
}

/* Whether two lists are the same calls with the same arguments: the nodes, bitmaps (at the same version) and faces they name the same ones. */
static bool dl_same(const gdlist *a, const gdlist *b)
{
    return a->n == b->n && a->width == b->width && a->height == b->height && (a->n == 0 || memcmp(a->ops, b->ops, a->n) == 0);
}

/*
 * (long node, long cv): endRecording. What the canvas recorded is the node's list from now on; the one it had goes. A list
 * the same as the one the node has is let go of instead, and the node keeps its own: Compose records a layer again
 * whenever its view is drawn, and a View is recorded again whenever it is invalidated, the same calls as often as not; the
 * frame walk (Gfx.damage) then sees at once that nothing in it changed.
 */
N(G_rnEnd)
{
    gnode *n = rn_of(c->args[0].j);
    gdlist *l = cv_end_record(cv_of(c->args[1].j)), *old;
    if (!n) {
        dl_unref(l);
        return;
    }
    mg_lock_acquire(&g_lock);
    old = n->list;
    n->ends++;
    if (old && l && dl_same(old, l)) {
        mg_lock_release(&g_lock);
        dl_unref(l);
        return;
    }
    n->list = l;
    mg_lock_release(&g_lock);
    dl_unref(old);
}

/* (long node): discardDisplayList. The list the frame walk last saw goes with it, and the walk takes the node as changed. */
N(G_rnDiscard)
{
    gnode *n = rn_of(c->args[0].j);
    gdlist *old, *seen;
    if (!n) return;
    mg_lock_acquire(&g_lock);
    old = n->list;
    n->list = NULL;
    seen = n->seen_list;
    n->seen_list = NULL;
    n->ver++;
    mg_lock_release(&g_lock);
    dl_unref(old);
    dl_unref(seen);
}

/*
 * (long node, boolean recording): a View's own node whose whole box was invalidated (android.view.View, overlaid,
 * meglerDefer): what of it changed -- calls of its list, nodes it draws -- is left to the frame walk (Gfx.damage), not the
 * whole box drawn again; `recording`, said while the node's list is being recorded, is for the list recorded after it. A
 * ListView whose overscroll glow invalidates it from its draw had the frame walk compare the list being recorded then, and
 * the glow's next steps went undrawn (gnode.defer).
 */
N(G_rnDefer)
{
    gnode *n = rn_of(c->args[0].j);
    u32 after;
    if (!n) return;
    mg_lock_acquire(&g_lock);
    after = n->ends + (c->args[1].z ? 1u : 0u);
    if (!n->defer) {
        n->defer = true;
        n->defer_min = n->defer_max = after;
    } else if (after > n->defer_max) {
        n->defer_max = after;
    }
    mg_lock_release(&g_lock);
}

/*
 * (long node, float[20] f, int[5] i, float[] outline path or null, int[] layer paint or null): the node's properties, as
 * android.graphics.RenderNode (Megler's overlay) packs them before anything is drawn that may draw the node:
 *
 *   f  0-3 left, top, right, bottom; 4-9 its matrix (RN_IDENTITY: none); 10 alpha; 11-14 setClipRect's rectangle
 *      (RN_CLIP_RECT); 15-18 the outline's rectangle and 19 its corner radius (outline kind 1)
 *   i  0 the RN_* flags; 1 the outline's kind: 0 none or empty, 1 a rectangle, 2 a path; 2 the alpha a layer is put
 *      on with, 0..255; 3 the path's fill rule (1 even-odd); 4 how many floats of the path count
 *
 * The path is the framework's operations (android.graphics.Path.ops), built here. The layer paint is Paint.mP, read for
 * its mode and colour filter (RN_LAYER_PAINT). A property that changes counts in the node's ver, for the frame walk; the
 * same properties handed over again do not.
 */
N(G_rnSet)
{
    gnode *n = rn_of(c->args[0].j);
    const float *f = (const float *)elements(c->args[1].l, 'F', 20);
    const i32 *k = (const i32 *)elements(c->args[2].l, 'I', 5), *paint = (const i32 *)elements(c->args[4].l, 'I', P_COUNT);
    gpath p;
    float *path = NULL, *old_path, m[6], alpha, radius;
    i32 path_n = 0, path_fill = 0, kind, flags, layer_alpha;
    bool changed;
    int i;
    if (!n || !f || !k) return;
    /* The outline's path built first; then everything set at once, under the lock node_play reads it under. */
    kind = k[1] == 1 || k[1] == 2 ? k[1] : 0;
    if (kind == 2) {
        if (!path_of(c->args[3].l, k[4], &p)) {
            kind = 0;
        } else {
            path_fill = k[3] & (MEGLER_GFX_EVEN_ODD | FILL_INVERSE);
            path_n = p.n;
            path = (float *)mg_alloc(sizeof(float) * (size_t)(p.n > 0 ? p.n : 1));
            if (p.n) memcpy(path, p.v, sizeof(float) * (size_t)p.n);
            path_free(&p);
        }
    }
    for (i = 0; i < 6; i++) m[i] = number(f[4 + i], i == 0 || i == 4 ? 1.0f : 0.0f);
    alpha = number(f[10], 1);
    radius = number(f[19], 0);
    flags = paint ? k[0] : k[0] & ~RN_LAYER_PAINT;
    layer_alpha = k[2] < 0 ? 0 : k[2] > 255 ? 255 : k[2];
    mg_lock_acquire(&g_lock);
    /* (What a flag leaves unused is not compared: the Java leaves the matrix's places as they were when the node has none.) */
    changed = n->left != f[0] || n->top != f[1] || n->right != f[2] || n->bottom != f[3] || n->alpha != alpha || n->flags != flags ||
              n->outline_kind != kind || n->layer_alpha != layer_alpha || (!(flags & RN_IDENTITY) && memcmp(n->m, m, sizeof(m)) != 0) ||
              ((flags & RN_CLIP_RECT) && memcmp(n->clip, f + 11, sizeof(n->clip)) != 0) ||
              (kind == 1 && (memcmp(n->outline, f + 15, sizeof(n->outline)) != 0 || n->radius != radius)) ||
              (kind == 2 && (n->outline_n != path_n || n->outline_fill != path_fill ||
                             (path_n > 0 && memcmp(n->outline_path, path, sizeof(float) * (size_t)path_n) != 0))) ||
              ((flags & RN_LAYER_PAINT) && memcmp(n->layer_paint, paint, sizeof(n->layer_paint)) != 0);
    if (changed) n->ver++;
    n->left = f[0];
    n->top = f[1];
    n->right = f[2];
    n->bottom = f[3];
    memcpy(n->m, m, sizeof(m));
    n->alpha = alpha;
    for (i = 0; i < 4; i++) {
        n->clip[i] = f[11 + i];
        n->outline[i] = f[15 + i];
    }
    n->radius = radius;
    n->flags = flags;
    n->outline_kind = kind;
    n->layer_alpha = layer_alpha;
    old_path = n->outline_path;
    n->outline_path = path;
    n->outline_n = path_n;
    if (kind == 2) n->outline_fill = path_fill;
    if (paint) memcpy(n->layer_paint, paint, sizeof(n->layer_paint));
    mg_lock_release(&g_lock);
    free(old_path);
}

/* (long node) -> long: the bytes its list takes (computeApproximateMemoryUsage). */
N(G_rnBytes)
{
    gnode *n = rn_of(c->args[0].j);
    if (n) c->ret.j = (i64)sizeof(*n) + (n->list ? (i64)(sizeof(gdlist) + n->list->cap) : 0);
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

/* Whether the shell's outlines are draw_path's stream as megler.h has it: verbs 0 to 4, each with all its numbers, every number one. */
static bool outline_stream(const float *v, size_t n)
{
    size_t i = 0, k, count;
    while (i < n) {
        if (!(v[i] >= MEGLER_GFX_PATH_MOVE && v[i] <= MEGLER_GFX_PATH_CLOSE) || v[i] != (float)(int)v[i]) return false;
        if (i == 0 && v[i] != MEGLER_GFX_PATH_MOVE) return false;
        count = (size_t)verb_floats((int)v[i++]);
        if (count > n - i) return false;
        for (k = 0; k < count; k++)
            if (!isfinite(v[i + k])) return false;
        i += count;
    }
    return true;
}

/*
 * (String, int start, int end, float x, float y, int[] paint, long typeface) -> float[]: Paint.getTextPath, the outlines of
 * the run's glyphs where drawText would put it, as draw_path's stream, whose verbs are those of the Java Path's own stream
 * (MOVE, LINE, QUAD, CUBIC, CLOSE). Empty for no text; null when the shell gives no outlines, and the Java makes do.
 */
N(G_txPath)
{
    i32 count;
    const u16 *text = text_of(c->args[0].l, c->args[1].i, c->args[2].i, &count);
    gpaint p;
    gbytes out;
    bool given = false;
    mg_jobj *array;
    paint_of(c->args[5].l, &p);
    if (!text) return;
    if (count <= 0) {
        c->ret.l = mg_dvm_new_array("[F", 0);
        return;
    }
    if (!HAS(text_path)) return;
    memset(&out, 0, sizeof(out));
    GFX(given = G.text_path(G.user, text, count, c->args[3].f, c->args[4].f, &p.p, tf_host(c->args[6].j), bytes_write, &out));
    if (given && (out.size % sizeof(float) != 0 || !outline_stream((const float *)out.data, out.size / sizeof(float)))) {
        mg_note_once("gfx: the shell's text_path gave what is not draw_path's stream: Paint.getTextPath makes do with the run's box");
        given = false;
    }
    if (given) {
        array = mg_dvm_new_array("[F", (i32)(out.size / sizeof(float)));
        if (array && out.size) memcpy(array->u.arr.data, out.data, out.size);
        c->ret.l = array;
    }
    free(out.data);
}

/* --------------------------------------------------------- Gfx: typefaces */

static gface *tf_new(void *host, i32 weight, bool italic)
{
    gface *f = (gface *)mg_alloc(sizeof(*f));
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
    const char *name = c->args[0].l ? mg_dvm_string_utf8(c->args[0].l) : NULL;
    i32 style = c->args[1].i, weight = (style & 1) ? 700 : 400;
    int generic = MEGLER_GFX_SANS;
    char family[96], *at;
    void *host = NULL;
    mg_strlcpy(family, name && name[0] ? name : "sans-serif", sizeof(family));
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
    mg_lock_acquire(&g_lock);
    for (i = 0; i < g_nderived && !made; i++)
        if (g_derived[i].from == from && g_derived[i].weight == weight && g_derived[i].italic == italic) made = g_derived[i].made;
    mg_lock_release(&g_lock);
    if (!made) {
        if (HAS(typeface_derive)) GFX(host = G.typeface_derive(G.user, from->host, weight, italic));
        made = tf_new(host, weight, italic);
        mg_lock_acquire(&g_lock);
        if (g_nderived == g_cap_derived) {
            g_cap_derived = g_cap_derived * 2 + 16;
            g_derived = (struct derived *)mg_realloc(g_derived, sizeof(*g_derived) * (size_t)g_cap_derived);
        }
        g_derived[g_nderived].from = from;
        g_derived[g_nderived].made = made;
        g_derived[g_nderived].weight = weight;
        g_derived[g_nderived].italic = italic;
        g_nderived++;
        mg_lock_release(&g_lock);
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
static void stops_of(mg_jobj *colors, mg_jobj *positions, gstops *out)
{
    const u32 *cs = (const u32 *)elements(colors, 'I', 1);
    i32 n = cs ? (i32)length_of(colors) : 1, i;
    const float *ps = (const float *)elements(positions, 'F', (u32)n);
    out->count = n < 2 ? 2 : n;
    out->colors = (u32 *)mg_alloc(sizeof(u32) * (size_t)out->count);
    out->positions = (float *)mg_alloc(sizeof(float) * (size_t)out->count);
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

/* A shader handle as the Java keeps it: the shell's shader inside one of this file's (gshader, by the canvases' section). */
static i64 shader_handle(void *host) { return handle(sh_new(host)); }

/* (x0, y0, x1, y1, int[] colors, float[] positions, int tile) -> long */
N(G_shLinear)
{
    gstops s;
    void *made;
    if (!HAS(shader_linear)) return;
    stops_of(c->args[4].l, c->args[5].l, &s);
    GFX(made = G.shader_linear(G.user, c->args[0].f, c->args[1].f, c->args[2].f, c->args[3].f, s.colors, s.positions, s.count, tile_number(c->args[6].i)));
    c->ret.j = shader_handle(made);
    stops_free(&s);
}

/* (cx, cy, r, int[] colors, float[] positions, int tile) -> long */
N(G_shRadial)
{
    gstops s;
    void *made;
    if (!HAS(shader_radial)) return;
    stops_of(c->args[3].l, c->args[4].l, &s);
    GFX(made = G.shader_radial(G.user, c->args[0].f, c->args[1].f, c->args[2].f, s.colors, s.positions, s.count, tile_number(c->args[5].i)));
    c->ret.j = shader_handle(made);
    stops_free(&s);
}

/* (cx, cy, int[] colors, float[] positions) -> long */
N(G_shSweep)
{
    gstops s;
    void *made;
    if (!HAS(shader_sweep)) return;
    stops_of(c->args[2].l, c->args[3].l, &s);
    GFX(made = G.shader_sweep(G.user, c->args[0].f, c->args[1].f, s.colors, s.positions, s.count));
    c->ret.j = shader_handle(made);
    stops_free(&s);
}

/* (long bm, int tile) -> long: the bitmap as it is now. The Java passes the x tile mode for both directions. */
N(G_shBitmap)
{
    gbitmap *b = bm_of(c->args[0].j);
    megler_gfx_image image;
    void *made;
    if (!b || !HAS(shader_bitmap)) return;
    bm_settle(b);
    image_of(b, &image);
    GFX(made = G.shader_bitmap(G.user, &image, tile_number(c->args[1].i)));
    c->ret.j = shader_handle(made);
}

/* (long shader, float[6] or null): Shader.setLocalMatrix. Kept here as well, for the lists that drew with the matrix before. */
N(G_shMatrix)
{
    gshader *s = sh_of(c->args[0].j);
    const float *m = (const float *)elements(c->args[1].l, 'F', 6);
    int i;
    if (!s || !HAS(shader_matrix)) return;
    for (i = 0; m && i < 6; i++)
        if (!isfinite(m[i])) return;
    s->has_m = m != NULL;
    if (m) memcpy(s->m, m, sizeof(s->m));
    GFX(G.shader_matrix(G.user, s->host, m));
}

N(G_shFree)
{
    gshader *s = sh_of(c->args[0].j);
    if (s) sh_release(s);
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

/* (float[] path, int n) -> float[]: the path as it is drawn, for Path.getPathIterator: verbs MEGLER_GFX_PATH_*, each with its
   numbers after it, the arcs, ovals and round rectangles in cubics and everything through the transforms after it. */
N(G_pathSegments)
{
    mg_jobj *array;
    gpath p;
    if (!path_of(c->args[0].l, c->args[1].i, &p)) return;
    array = mg_dvm_new_array("[F", p.n);
    if (array && p.n) memcpy(array->u.arr.data, p.v, sizeof(float) * (size_t)p.n);
    c->ret.l = array;
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
    c->ret.z = (c->args[2].i & MEGLER_GFX_EVEN_ODD) ? (h.crossings & 1) != 0 : h.winding != 0;
    if (c->args[2].i & FILL_INVERSE) c->ret.z = !c->ret.z;
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

/* ------------------------------------------------------ Gfx: the render thread */

/*
 * The window's frames drawn onto the screen and shown off the main thread, as Android's RenderThread draws what the UI thread
 * recorded. megler.ViewRoot records a frame -- every window, the part of the screen it changes cleared -- into a list
 * (a recording canvas: views draw their own lists by reference, Canvas.meglerRecordFrame) and hands it over (Gfx.frame); the
 * render thread plays that list onto the screen's canvas, which only it draws on, and shows the screen (present_bitmap, the
 * shell's present), while the main thread goes on to its next frame. One frame is handed over at a time: the main thread
 * waits for the one before to be shown, which paces it to the screen as its own present did. What the list draws is kept
 * by it (nodes, lists, bitmaps and shaders are counted references), and a node's properties are read under the lock its
 * setter takes (node_play, rnSet). Before, the main thread drew every view's list onto the screen and waited for the shell's
 * present itself: Spotify's spent 26% of its time drawing the lists and 62% presenting, 27 ms a frame, of which its own
 * Java was 10. The flag no-render-thread draws and shows each frame on the main thread, as before.
 */
static mg_lock g_rt_lock = MG_LOCK_INIT;
static mg_cond g_rt_wake = MG_COND_INIT, g_rt_idle = MG_COND_INIT;
static struct {
    i32 started;                       /* 0 not yet, 1 it runs, 2 none: frames are drawn and shown on the main thread */
    bool busy;                         /* a frame has been handed over and is not shown yet */
    gcanvas *cv;                       /* the frame handed over: the screen's canvas and bitmap, the list, the part it changes */
    gbitmap *bm;
    gdlist *list;
    i32 part[4];
    u32 cull;                          /* the frame walk that went through it (frame_show), or 0 */
    u32 frames;                        /* for the log, since `since` */
    u64 play_ns, show_ns, wait_ns, since;
} R;
static void (*g_present_part)(const i32 part[4]);

void mg_java_gfx_set_present_part(void (*fn)(const i32 part[4])) { g_present_part = fn; }

/* The bitmap shown: from the shell's own picture of it where it can, else its memory brought up to date (Gfx.present). */
static void present_bitmap(gbitmap *b)
{
    /*
     * Shown from the shell's own picture of the bitmap where it can (megler_gfx_host.present): when one canvas draws the bitmap
     * and nothing was written into the memory behind it. The canvas is then not flushed, and stays dirty: whatever reads
     * the memory later flushes it first (bm_settle), as it always has.
     */
    static i32 said;
    const char *why = NULL;
    if (g_present.canvas_fn) {
        gcanvas *only;
        void *host = NULL;
        bool shown = false;
        /*
         * The shell's present (its drawing ended, the window's copy, the swap chain's Present) runs without g_lock: it waits for
         * the swap chain 10 to 26 ms a frame on a Lumia 650, and the main thread's recording (cvRecord, rnEnd, cvDrawNode) takes
         * g_lock for every list it makes -- it waited there, 24 to 28 ms a frame, and nothing above 30 frames a second was
         * drawn. Only the render thread draws the screen's canvas, and the main thread lets it go or reads it only after
         * Gfx.frameWait; anything else that would flush or free it waits for the present (bm_wait_presented). The shell keeps
         * its own state under its own locks, and Direct2D's factory is a multithreaded one.
         */
        mg_lock_acquire(&g_lock);
        only = b->canvases;
        if (only && !only->next && only->host && !only->stale && !b->rgba_locks) {
            host = only->host;
            b->presenting++;
        } else {
            why = !only ? "no canvas draws it" : only->next ? "more than one canvas draws it" : !only->host ? "its canvas is not the shell's"
                : only->stale ? "its memory was written behind its canvas" : "its memory is locked in Android's order";
        }
        mg_lock_release(&g_lock);
        if (host) {
            shown = g_present.canvas_fn(g_present.canvas_user, host, b->w, b->h);
            mg_lock_acquire(&g_lock);
            if (--b->presenting == 0) mg_cond_broadcast(&g_presented);
            mg_lock_release(&g_lock);
            if (!shown) why = "the shell would not show its canvas";
        }
        if (shown) {
            /* (A look at what was shown, for a diagnosis, when one is asked for: the memory brought up to date for it.) */
            if (g_present.probe && g_present.probe_due && g_present.probe_due()) {
                bm_settle(b);
                g_present.probe(b->px, b->w, b->h, b->stride);
            }
            return;
        }
    }
    /* (Why, the first times: AIDA64's every frame went through memory on a Lumia 650, 14 ms of read-back each, and the
       log did not say which of the conditions above failed.) */
    if (why && said < 3) {
        said++;
        mg_info("gfx: the window's picture went through memory, %ux%u: %s", (unsigned)b->w, (unsigned)b->h, why);
    }
    bm_settle(b);
    g_present.fn(g_present.user, b->px, b->w, b->h, b->stride);
}

/*
 * A frame drawn onto the screen's canvas and shown (not when part[2] is -1), on the calling thread; the list is let go. A
 * frame of part of the screen (part[2] above 0) is drawn clipped to the part, cleared first; with `cull` the number of the
 * frame walk that went through it (G_damage), whose boxes leave out the nodes that draw nowhere in the part (node_play).
 */
static void frame_show(gcanvas *v, gbitmap *b, gdlist *l, const i32 part[4], u32 cull)
{
    u64 t0 = mg_time_monotonic_ns(), t1;
    i32 d0 = v->depth;
    cv_save(v, NULL, false, 255, MEGLER_GFX_BLEND_SRC_OVER, NULL);
    if (part[2] > 0) {
        cv_clip_rect(v, (float)part[0], (float)part[1], (float)part[2], (float)part[3], MEGLER_GFX_CLIP_INTERSECT);
        draw_color(v, 0, MEGLER_GFX_BLEND_CLEAR);
    }
    t_cull_v = cull ? v : NULL;
    t_cull_no = cull;
    list_play(v, l, 0);
    t_cull_v = NULL;
    while (v->depth > d0) cv_restore(v);
    dl_unref(l);
    t1 = mg_time_monotonic_ns();
    if (part[2] != -1) {
        if (g_present_part) g_present_part(part);
        if (g_present.fn) present_bitmap(b);
    }
    mg_lock_acquire(&g_rt_lock);
    R.frames++;
    R.play_ns += t1 - t0;
    R.show_ns += mg_time_monotonic_ns() - t1;
    mg_lock_release(&g_rt_lock);
}

static MG_HOSTCB void rt_main(void *arg)
{
    (void)arg;
    mg_lock_acquire(&g_rt_lock);
    for (;;) {
        gcanvas *v;
        gbitmap *b;
        gdlist *l;
        i32 part[4];
        u32 cull;
        u64 now;
        while (!R.busy || !R.list) mg_cond_wait(&g_rt_wake, &g_rt_lock);
        v = R.cv;
        b = R.bm;
        l = R.list;
        memcpy(part, R.part, sizeof(part));
        cull = R.cull;
        R.list = NULL;
        mg_lock_release(&g_rt_lock);
        frame_show(v, b, l, part, cull);
        mg_lock_acquire(&g_rt_lock);
        R.busy = false;
        R.cv = NULL;
        R.bm = NULL;
        mg_cond_broadcast(&g_rt_idle);
        now = mg_time_monotonic_ns();
        if (!R.since) R.since = now;
        if (now - R.since >= 5000000000ull && R.frames) {
            mg_info("gfx: the render thread, %.1f s: %u frames drawn and shown off the main thread, %.1f ms drawing the lists and %.1f ms showing each; "
                    "the main thread waited %.1f ms in all for the frame before to be shown; %.1f nodes a frame left out, drawing nowhere in the part drawn",
                    (double)(now - R.since) / 1e9, (unsigned)R.frames, (double)R.play_ns / 1e6 / R.frames, (double)R.show_ns / 1e6 / R.frames,
                    (double)R.wait_ns / 1e6, (double)g_culled / R.frames);
            g_culled = 0;
            R.frames = 0;
            R.play_ns = R.show_ns = R.wait_ns = 0;
            R.since = now;
        }
    }
}

/* Whether frames go to the render thread, which is started the first time. */
static bool rt_on(void)
{
    if (R.started) return R.started == 1;
    mg_lock_acquire(&g_rt_lock);
    if (!R.started) {
        mg_thread *t = mg_flag("no-render-thread") ? NULL : mg_thread_start(rt_main, NULL, 1u << 20, "RenderThread");
        if (t) mg_thread_detach(t);
        R.started = t ? 1 : 2;
        mg_info("gfx: %s", t ? "the window's frames are drawn and shown by a render thread of their own, off the main thread"
                             : "the window's frames are drawn and shown on the main thread (the flag no-render-thread, or no thread)");
    }
    mg_lock_release(&g_rt_lock);
    return R.started == 1;
}

/* Until the frame handed over last has been shown. */
static void rt_wait(void)
{
    u64 t0;
    if (R.started != 1) return;
    t0 = mg_time_monotonic_ns();
    mg_lock_acquire(&g_rt_lock);
    while (R.busy) mg_cond_wait(&g_rt_idle, &g_rt_lock);
    R.wait_ns += mg_time_monotonic_ns() - t0;
    mg_lock_release(&g_rt_lock);
}

void mg_java_gfx_frames_wait(void) { rt_wait(); }

/*
 * (long screen canvas, long screen bitmap, long recording canvas, int left, int top, int right, int bottom): Gfx.frame. What the
 * recording canvas recorded (its recording ended here) drawn onto the screen's canvas and the screen shown, `part` the part of
 * it the frame changes (right 0: all of it; -1: drawn and not shown); by the render thread, once the frame before has been shown.
 */
N(G_frame)
{
    gcanvas *v = cv_of(c->args[0].j), *rec = cv_of(c->args[2].j);
    gbitmap *b = bm_of(c->args[1].j);
    gdlist *l = cv_end_record(rec);
    i32 part[4];
    u32 cull = g_walk_ready;
    u64 t0;
    int i;
    /* (The frame walk's boxes are this frame's: the walk ran on what was recorded, and no other runs until it is shown.) */
    g_walk_ready = 0;
    for (i = 0; i < 4; i++) part[i] = c->args[3 + i].i;
    if (!v || !b || !l) {
        dl_unref(l);
        return;
    }
    if (!rt_on()) {
        frame_show(v, b, l, part, cull);
        return;
    }
    t0 = mg_time_monotonic_ns();
    mg_lock_acquire(&g_rt_lock);
    while (R.busy) mg_cond_wait(&g_rt_idle, &g_rt_lock);
    R.wait_ns += mg_time_monotonic_ns() - t0;
    R.cv = v;
    R.bm = b;
    R.list = l;
    memcpy(R.part, part, sizeof(part));
    R.cull = cull;
    R.busy = true;
    mg_cond_signal(&g_rt_wake);
    mg_lock_release(&g_rt_lock);
}

/* Gfx.frameWait(): until the frame handed over last has been shown (before the screen's bitmap is let go of, or read). */
N(G_frameWait)
{
    (void)c;
    rt_wait();
}

/* (long bm): the bitmap shown as it is, on the calling thread, after any frame handed to the render thread; 0: nothing to show. */
N(G_present)
{
    gbitmap *b = bm_of(c->args[0].j);
    if (!g_present.fn) return;
    rt_wait();
    if (!b) {
        g_present.fn(g_present.user, NULL, 0, 0, 0);
        return;
    }
    present_bitmap(b);
}

static const mg_jhle k_gfx[] = {
    { "megler/Gfx", "bmNew", "(II)J", G_bmNew },
    { "megler/Gfx", "bmFree", "(J)V", G_bmFree },
    { "megler/Gfx", "bmErase", "(JI)V", G_bmErase },
    { "megler/Gfx", "bmEraseMemory", "(JI)V", G_bmEraseMemory },
    { "megler/Gfx", "bmDiffer", NULL, G_bmDiffer },
    { "megler/Gfx", "bmGetPixel", "(JII)I", G_bmGetPixel },
    { "megler/Gfx", "bmSetPixel", "(JIII)V", G_bmSetPixel },
    { "megler/Gfx", "bmPixels", "(J[IIIIIIIZ)V", G_bmPixels },
    { "megler/Gfx", "bmRaw", "(JJZ)V", G_bmRaw },
    { "megler/Gfx", "bmOpaque", "(J)V", G_bmOpaque },
    { "megler/Gfx", "bmAddress", "(J)J", G_bmAddress },
    { "megler/Gfx", "bmDecode", "([BIII[IZ)J", G_bmDecode },
    { "megler/Gfx", "bmCompress", "(JII)[B", G_bmCompress },
    { "megler/Gfx", "bmCopy", "(JIIII[FZ)J", G_bmCopy },
    { "megler/Gfx", "cvNew", "(J)J", G_cvNew },
    { "megler/Gfx", "cvFree", "(J)V", G_cvFree },
    { "megler/Gfx", "cvSave", "(J)I", G_cvSave },
    { "megler/Gfx", "cvSaveLayer", "(JFFFFI)I", G_cvSaveLayer },
    { "megler/Gfx", "cvSaveLayerBlur", "(JFFFFIFFZ)I", G_cvSaveLayerBlur },
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
    { "megler/Gfx", "cvDrawTexture", "(JIFFFFI)V", G_cvDrawTexture },
    { "megler/Gfx", "textureGone", "(I)V", G_textureGone },
    { "megler/Gfx", "cvDrawText", "(JLjava/lang/String;IIFF[IJJ)V", G_cvDrawText },
    { "megler/Gfx", "txMeasure", "(Ljava/lang/String;II[IJ)F", G_txMeasure },
    { "megler/Gfx", "txMetrics", "([IJ[F)F", G_txMetrics },
    { "megler/Gfx", "txBounds", "(Ljava/lang/String;II[IJ[I)V", G_txBounds },
    { "megler/Gfx", "txBreak", "(Ljava/lang/String;IIF[IJZ)I", G_txBreak },
    { "megler/Gfx", "txWidths", "(Ljava/lang/String;II[IJ[F)V", G_txWidths },
    { "megler/Gfx", "txPath", "(Ljava/lang/String;IIFF[IJ)[F", G_txPath },
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
    { "megler/Gfx", "pathSegments", "([FI)[F", G_pathSegments },
    { "megler/Gfx", "pathContains", "([FIIFF)Z", G_pathContains },
    { "megler/Gfx", "pathLength", "([FI[F)F", G_pathLength },
    { "megler/Gfx", "present", "(J)V", G_present },
    { "megler/Gfx", "frame", "(JJJIIII)V", G_frame },
    { "megler/Gfx", "frameWait", "()V", G_frameWait },
    { "megler/Gfx", "cvRecord", "(II)J", G_cvRecord },
    { "megler/Gfx", "cvEndRecord", "(J)J", G_cvEndRecord },
    { "megler/Gfx", "cvDrawNode", "(JJ)V", G_cvDrawNode },
    { "megler/Gfx", "cvDrawList", "(JJ)V", G_cvDrawList },
    { "megler/Gfx", "dlFree", "(J)V", G_dlFree },
    { "megler/Gfx", "rnNew", "()J", G_rnNew },
    { "megler/Gfx", "rnFree", "(J)V", G_rnFree },
    { "megler/Gfx", "rnEnd", "(JJ)V", G_rnEnd },
    { "megler/Gfx", "rnDiscard", "(J)V", G_rnDiscard },
    { "megler/Gfx", "rnSet", "(J[F[I[F[I)V", G_rnSet },
    { "megler/Gfx", "rnDefer", "(JZ)V", G_rnDefer },
    { "megler/Gfx", "damage", "(JII[IZ)Z", G_damage },
    { "megler/Gfx", "rnBytes", "(J)J", G_rnBytes },
    { NULL, NULL, NULL, NULL },
};

/* --------------------------------------------------------- for the driver */

const mg_jhle *mg_java_gfx_natives(void) { return k_gfx; }

void mg_java_gfx_set_host(const megler_gfx_host *host)
{
    if (host) G = *host;
    else memset(&G, 0, sizeof(G));
}

u64 mg_java_gfx_host_ns(void) { return t_host_ns; }

void mg_java_gfx_list_stats(uint64_t *bytes, uint32_t *lists, uint32_t *nodes)
{
    mg_lock_acquire(&g_lock);
    if (bytes) *bytes = g_dl_bytes;
    if (lists) *lists = g_dl_count;
    if (nodes) *nodes = g_rn_count;
    mg_lock_release(&g_lock);
}

void mg_java_gfx_set_present(void (*fn)(void *user, const void *pixels, int width, int height, int stride), void *user)
{
    g_present.fn = fn;
    g_present.user = user;
}

void mg_java_gfx_set_present_canvas(bool (*fn)(void *user, void *canvas, int width, int height), void *user)
{
    g_present.canvas_fn = fn;
    g_present.canvas_user = user;
}

void mg_java_gfx_set_present_probe(bool (*due)(void), void (*probe)(const void *pixels, int width, int height, int stride))
{
    g_present.probe_due = due;
    g_present.probe = probe;
}

void mg_java_gfx_stats(uint64_t *bitmap_bytes, uint32_t *bitmaps)
{
    mg_lock_acquire(&g_lock);
    if (bitmap_bytes) *bitmap_bytes = g_bytes;
    if (bitmaps) *bitmaps = g_count;
    mg_lock_release(&g_lock);
}

static void info_of(const gbitmap *b, mg_java_gfx_bitmap *out)
{
    if (!out) return;
    out->pixels = b->px;
    out->width = b->w;
    out->height = b->h;
    out->stride = b->stride;
}

bool mg_java_gfx_bitmap_info(i64 bm, mg_java_gfx_bitmap *out)
{
    gbitmap *b = bm_of(bm);
    if (!b) return false;
    bm_settle(b);
    info_of(b, out);
    return true;
}

void mg_java_gfx_bitmap_changed(i64 bm)
{
    gbitmap *b = bm_of(bm);
    if (b) bm_changed(b);
}

bool mg_java_gfx_lock_rgba(i64 bm, mg_java_gfx_bitmap *out)
{
    gbitmap *b = bm_of(bm);
    bool first;
    if (!b) return false;
    bm_settle(b);
    mg_lock_acquire(&g_lock);
    first = b->rgba_locks++ == 0;
    mg_lock_release(&g_lock);
    if (first) swap_red_blue(b->px, b->px, (size_t)b->w * (size_t)b->h);
    info_of(b, out);
    return true;
}

bool mg_java_gfx_canvas_raster(i64 canvas, mg_java_gfx_raster *out)
{
    gcanvas *v = cv_of(canvas);
    mg_java_gfx_bitmap px;
    gstate *s;
    if (!v || v->rec || !v->bm || !mg_java_gfx_lock_rgba(handle(v->bm), &px)) return false;
    s = cv_state(v);
    out->pixels = px.pixels;
    out->width = px.width;
    out->height = px.height;
    out->stride = px.stride;
    out->matrix[0] = s->m[0]; out->matrix[1] = s->m[1]; out->matrix[2] = s->m[2];
    out->matrix[3] = s->m[3]; out->matrix[4] = s->m[4]; out->matrix[5] = s->m[5];
    out->matrix[6] = out->matrix[7] = 0;
    out->matrix[8] = 1;
    out->empty = s->empty;
    out->clip[0] = s->empty ? 0 : whole(floor(s->clip[0]));
    out->clip[1] = s->empty ? 0 : whole(floor(s->clip[1]));
    out->clip[2] = s->empty ? 0 : whole(ceil(s->clip[2]));
    out->clip[3] = s->empty ? 0 : whole(ceil(s->clip[3]));
    /* (the clip within the bitmap: a box past its edges draws nothing there) */
    if (out->clip[0] < 0) out->clip[0] = 0;
    if (out->clip[1] < 0) out->clip[1] = 0;
    if (out->clip[2] > px.width) out->clip[2] = px.width;
    if (out->clip[3] > px.height) out->clip[3] = px.height;
    return true;
}

void mg_java_gfx_canvas_raster_end(i64 canvas)
{
    gcanvas *v = cv_of(canvas);
    if (v && v->bm) mg_java_gfx_unlock_rgba(handle(v->bm));
}

void mg_java_gfx_unlock_rgba(i64 bm)
{
    gbitmap *b = bm_of(bm);
    bool last;
    if (!b) return;
    mg_lock_acquire(&g_lock);
    last = b->rgba_locks > 0 && --b->rgba_locks == 0;
    mg_lock_release(&g_lock);
    if (!last) return;
    swap_red_blue(b->px, b->px, (size_t)b->w * (size_t)b->h);
    bm_changed(b);
}
