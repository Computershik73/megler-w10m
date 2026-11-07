/*
 * A shell's drawing, looked at and checked (mg_gfx_scenes.h).
 *
 * Everything goes through the megler_gfx_host struct as the core's side of
 * megler.Gfx (src/drivers/mg_java_gfx.c) uses it: the whole matrix is set for
 * every change, flush comes before the pixels are read and touched after
 * they were written, arcs and lines arrive as paths, every save is restored
 * before a canvas is freed.
 *
 * Two kinds of scene. The first kind is drawn twice, by the host and by the
 * stand-in (mg_gfx_standin.c), with only what the stand-in does as megler.h
 * says, and the two pictures are compared: a pixel whose eight neighbours
 * in the stand-in's picture are all the same colour is inside a shape (or
 * outside all of them) and has to be that colour in the host's picture too,
 * to within a step; any other pixel is on an edge, where a host that smooths
 * may differ, but only to a colour between those of its neighbours. The
 * second kind is what the stand-in cannot draw -- joins, clips by shapes,
 * layers, gradients, text -- and is there to be looked at; a few things in
 * them are checked on pixels where the right answer is known.
 *
 * The pictures are written as PNG files by the stand-in's encoder, which is
 * not the one under test.
 */
#include <math.h>
#include <stdio.h>

#include "../src/mg_base.h"
#include "../src/host/mg_host.h"
#include "mg_gfx_scenes.h"
#include "mg_gfx_standin.h"

static int t_checks, t_failed;
static mg_test_sink t_sink;
static void *t_sink_user;
static const megler_gfx_host *H, *S;
static const mg_gfx_scenes_config *t_config;
static bool t_smooth = true;           /* whether the paints of a scene ask for smoothed edges */

static void say(const char *fmt, ...)
{
    char line[700];
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

#define CHECK_HEX(expr, want)                                                                        \
    do {                                                                                             \
        u32 got_ = (u32)(expr), want_ = (u32)(want);                                                 \
        t_checks++;                                                                                  \
        if (got_ != want_) {                                                                         \
            t_failed++;                                                                              \
            say("  FAIL %s:%d: %s = %08x, expected %08x", __FILE__, __LINE__, #expr, got_, want_);   \
        }                                                                                            \
    } while (0)

/* Two numbers that should be the same but for rounding: within `slack`. */
#define CHECK_NEAR(expr, want, slack)                                                                           \
    do {                                                                                                        \
        double got_ = (double)(expr), want_ = (double)(want), slack_ = (double)(slack);                         \
        t_checks++;                                                                                             \
        if (!(fabs(got_ - want_) <= slack_)) {                                                                  \
            t_failed++;                                                                                         \
            say("  FAIL %s:%d: %s = %g, expected %g (within %g)", __FILE__, __LINE__, #expr, got_, want_, slack_); \
        }                                                                                                       \
    } while (0)

/* ---------------------------------------------------- a bitmap and its canvas */

typedef struct pic {
    u32 *px;                           /* a pixel is the word 0xAARRGGBB, premultiplied; in memory B, G, R, A */
    int w, h;
    void *cv;
    const megler_gfx_host *g;
    float m[6];                        /* the matrix, kept here as the core keeps it */
} pic;

static void pic_make(pic *p, const megler_gfx_host *g, int w, int h)
{
    memset(p, 0, sizeof(*p));
    p->g = g;
    p->w = w;
    p->h = h;
    p->px = (u32 *)mg_alloc(sizeof(u32) * (size_t)w * (size_t)h);
    p->cv = g->canvas_new(g->user, p->px, w, h, w * 4);
    p->m[0] = p->m[4] = 1;
}

/* Before the pixels are read. */
static void settle(pic *p)
{
    if (p->cv) p->g->flush(p->g->user, p->cv);
}

/* After the pixels were written by other means than the canvas. */
static void wrote(pic *p)
{
    if (p->cv) p->g->touched(p->g->user, p->cv);
}

static void pic_done(pic *p)
{
    if (p->cv) {
        settle(p);
        p->g->canvas_free(p->g->user, p->cv);
    }
    free(p->px);
    memset(p, 0, sizeof(*p));
}

static u32 at(const pic *p, int x, int y) { return x >= 0 && y >= 0 && x < p->w && y < p->h ? p->px[y * p->w + x] : 0; }

static void image_of(const pic *p, megler_gfx_image *out)
{
    static u32 next_id;
    out->pixels = p->px;
    out->width = p->w;
    out->height = p->h;
    out->stride = p->w * 4;
    out->id = ++next_id ? next_id : ++next_id;
    out->version = 1;
}

/* ---- the matrix: the core keeps it and hands the host the whole of it each time */

static void matrix_set(pic *p, float a, float b, float c, float d, float e, float f)
{
    p->m[0] = a;
    p->m[1] = b;
    p->m[2] = c;
    p->m[3] = d;
    p->m[4] = e;
    p->m[5] = f;
    p->g->set_matrix(p->g->user, p->cv, p->m);
}

static void matrix_reset(pic *p) { matrix_set(p, 1, 0, 0, 0, 1, 0); }

/* Canvas.concat: what is drawn afterwards goes through (a..f) first and then through what the canvas had. */
static void matrix_concat(pic *p, float a, float b, float c, float d, float e, float f)
{
    const float *m = p->m;
    matrix_set(p, m[0] * a + m[1] * d, m[0] * b + m[1] * e, m[0] * c + m[1] * f + m[2], m[3] * a + m[4] * d, m[3] * b + m[4] * e, m[3] * c + m[4] * f + m[5]);
}

static void translate(pic *p, float x, float y) { matrix_concat(p, 1, 0, x, 0, 1, y); }
static void scale(pic *p, float x, float y) { matrix_concat(p, x, 0, 0, 0, y, 0); }
static void rotate(pic *p, float degrees)
{
    float r = degrees * 3.14159265f / 180.0f, c = (float)cos(r), s = (float)sin(r);
    matrix_concat(p, c, -s, 0, s, c, 0);
}
static void skew(pic *p, float kx, float ky) { matrix_concat(p, 1, kx, 0, ky, 1, 0); }

/* Canvas.save and restore: the clip is the host's to keep, the matrix the caller's. */
typedef struct saved {
    float m[6];
} saved;

static saved save(pic *p)
{
    saved s;
    memcpy(s.m, p->m, sizeof(s.m));
    p->g->save(p->g->user, p->cv);
    return s;
}

static saved save_layer(pic *p, const float *bounds, int alpha, int blend)
{
    saved s;
    memcpy(s.m, p->m, sizeof(s.m));
    p->g->save_layer(p->g->user, p->cv, bounds, alpha, blend);
    return s;
}

/* A layer blurred as it is put on (save_layer_blur), by Android's radius both ways; a plain layer where the host has none. */
static saved save_layer_blur(pic *p, const float *bounds, float radius, bool clamp)
{
    saved s;
    memcpy(s.m, p->m, sizeof(s.m));
    if (p->g->save_layer_blur) p->g->save_layer_blur(p->g->user, p->cv, bounds, 255, radius, radius, clamp);
    else p->g->save_layer(p->g->user, p->cv, bounds, 255, MEGLER_GFX_BLEND_SRC_OVER);
    return s;
}

static void restore(pic *p, saved s)
{
    p->g->restore(p->g->user, p->cv);
    matrix_set(p, s.m[0], s.m[1], s.m[2], s.m[3], s.m[4], s.m[5]);
}

/* ---- drawing, as short as the scenes want it */

static megler_gfx_paint paint_of(u32 color)
{
    megler_gfx_paint p;
    memset(&p, 0, sizeof(p));
    p.color = color;
    p.flags = t_smooth ? MEGLER_GFX_ANTI_ALIAS : 0;
    p.miter_limit = 4;
    p.text_size = 12;
    p.text_scale_x = 1;
    p.blend = MEGLER_GFX_BLEND_SRC_OVER;
    p.filter_mode = -1;
    return p;
}

static megler_gfx_paint stroke_of(u32 color, float width)
{
    megler_gfx_paint p = paint_of(color);
    p.style = MEGLER_GFX_STROKE;
    p.stroke_width = width;
    return p;
}

static void color(pic *p, u32 c, int blend) { p->g->draw_color(p->g->user, p->cv, c, blend); }
static void rect(pic *p, float l, float t, float r, float b, const megler_gfx_paint *paint, void *shader)
{
    p->g->draw_rect(p->g->user, p->cv, l, t, r, b, paint, shader);
}
static void fill(pic *p, float l, float t, float r, float b, u32 c)
{
    megler_gfx_paint paint = paint_of(c);
    rect(p, l, t, r, b, &paint, NULL);
}
static void round_rect(pic *p, float l, float t, float r, float b, float rx, float ry, const megler_gfx_paint *paint, void *shader)
{
    p->g->draw_round_rect(p->g->user, p->cv, l, t, r, b, rx, ry, paint, shader);
}
static void oval(pic *p, float l, float t, float r, float b, const megler_gfx_paint *paint, void *shader)
{
    p->g->draw_oval(p->g->user, p->cv, l, t, r, b, paint, shader);
}
static void path(pic *p, const float *v, int n, int rule, const megler_gfx_paint *paint, void *shader)
{
    p->g->draw_path(p->g->user, p->cv, v, n, rule, paint, shader);
}
static void line(pic *p, float x0, float y0, float x1, float y1, const megler_gfx_paint *paint)
{
    float v[6];
    megler_gfx_paint stroked = *paint;
    v[0] = MEGLER_GFX_PATH_MOVE;
    v[1] = x0;
    v[2] = y0;
    v[3] = MEGLER_GFX_PATH_LINE;
    v[4] = x1;
    v[5] = y1;
    stroked.style = MEGLER_GFX_STROKE;
    path(p, v, 6, MEGLER_GFX_WINDING, &stroked, NULL);
}
static void clip_rect(pic *p, float l, float t, float r, float b, int op) { p->g->clip_rect(p->g->user, p->cv, l, t, r, b, op); }
static void clip_path(pic *p, const float *v, int n, int rule, int op) { p->g->clip_path(p->g->user, p->cv, v, n, rule, op); }

/* A part of one picture onto a rectangle of another. The source has been settled by the caller, as the core settles it. */
static void bitmap(pic *p, const pic *from, float sl, float st, float sr, float sb, float dl, float dt, float dr, float db, const megler_gfx_paint *paint)
{
    megler_gfx_image image;
    float src[4], dst[4];
    image_of(from, &image);
    src[0] = sl;
    src[1] = st;
    src[2] = sr;
    src[3] = sb;
    dst[0] = dl;
    dst[1] = dt;
    dst[2] = dr;
    dst[3] = db;
    p->g->draw_bitmap(p->g->user, p->cv, &image, src, dst, paint);
}

/* ASCII as UTF-16. */
static int units(const char *ascii, u16 *out, int cap)
{
    int n = 0;
    while (ascii[n] && n < cap) {
        out[n] = (u8)ascii[n];
        n++;
    }
    return n;
}

static void text16(pic *p, const u16 *t, int n, float x, float y, const megler_gfx_paint *paint, void *face, void *shader)
{
    p->g->draw_text(p->g->user, p->cv, t, n, x, y, paint, face, shader);
}

static void text(pic *p, const char *ascii, float x, float y, const megler_gfx_paint *paint, void *face)
{
    u16 t[256];
    int n = units(ascii, t, 256);
    text16(p, t, n, x, y, paint, face, NULL);
}

/* A small black label, in the default face: what a part of a scene is. */
static void label(pic *p, const char *ascii, float x, float y)
{
    megler_gfx_paint paint = paint_of(0xFF202020u);
    paint.flags = MEGLER_GFX_ANTI_ALIAS;
    paint.text_size = 11;
    text(p, ascii, x, y, &paint, NULL);
}

/* ---- paths the core would have made: an arc of an oval as cubics, a polygon, a star */

typedef struct pathbuf {
    float v[512];
    int n;
} pathbuf;

static void pb_verb(pathbuf *b, int verb, const float *xy, int count)
{
    int i;
    if (b->n + 1 + count > (int)MG_COUNT(b->v)) return;
    b->v[b->n++] = (float)verb;
    for (i = 0; i < count; i++) b->v[b->n++] = xy[i];
}
static void pb_move(pathbuf *b, float x, float y)
{
    float xy[2];
    xy[0] = x;
    xy[1] = y;
    pb_verb(b, MEGLER_GFX_PATH_MOVE, xy, 2);
}
static void pb_line(pathbuf *b, float x, float y)
{
    float xy[2];
    xy[0] = x;
    xy[1] = y;
    pb_verb(b, MEGLER_GFX_PATH_LINE, xy, 2);
}
static void pb_quad(pathbuf *b, float cx, float cy, float x, float y)
{
    float xy[4];
    xy[0] = cx;
    xy[1] = cy;
    xy[2] = x;
    xy[3] = y;
    pb_verb(b, MEGLER_GFX_PATH_QUAD, xy, 4);
}
static void pb_cubic(pathbuf *b, float ax, float ay, float bx, float by, float x, float y)
{
    float xy[6];
    xy[0] = ax;
    xy[1] = ay;
    xy[2] = bx;
    xy[3] = by;
    xy[4] = x;
    xy[5] = y;
    pb_verb(b, MEGLER_GFX_PATH_CUBIC, xy, 6);
}
static void pb_close(pathbuf *b) { pb_verb(b, MEGLER_GFX_PATH_CLOSE, NULL, 0); }

/*
 * An arc of the oval in a rectangle, as Android's Path.arcTo means it:
 * degrees, 0 at three o'clock, positive clockwise on screen. Pieces of a
 * quarter turn at most, each a cubic; `move` starts a figure at its start,
 * otherwise a line leads there.
 */
static void pb_arc(pathbuf *b, float l, float t, float r, float bt, float start, float sweep, bool move)
{
    float cx = (l + r) / 2, cy = (t + bt) / 2, rx = (r - l) / 2, ry = (bt - t) / 2;
    int pieces = (int)ceil(fabs(sweep) / 90.0), i;
    double a0 = start * 3.14159265358979 / 180, step;
    if (pieces < 1) pieces = 1;
    step = sweep * 3.14159265358979 / 180 / pieces;
    if (move) pb_move(b, cx + rx * (float)cos(a0), cy + ry * (float)sin(a0));
    else pb_line(b, cx + rx * (float)cos(a0), cy + ry * (float)sin(a0));
    for (i = 0; i < pieces; i++) {
        double a1 = a0 + step, k = 4.0 / 3.0 * tan(step / 4);
        pb_cubic(b, cx + rx * (float)(cos(a0) - k * sin(a0)), cy + ry * (float)(sin(a0) + k * cos(a0)), cx + rx * (float)(cos(a1) + k * sin(a1)),
                 cy + ry * (float)(sin(a1) - k * cos(a1)), cx + rx * (float)cos(a1), cy + ry * (float)sin(a1));
        a0 = a1;
    }
}

/* A star of five points drawn in one stroke, which crosses itself: its middle is inside by winding and outside by even-odd. */
static void pb_star(pathbuf *b, float cx, float cy, float radius)
{
    int i;
    for (i = 0; i < 5; i++) {
        double a = (-90.0 + 144.0 * i) * 3.14159265358979 / 180;
        if (i) pb_line(b, cx + radius * (float)cos(a), cy + radius * (float)sin(a));
        else pb_move(b, cx + radius * (float)cos(a), cy + radius * (float)sin(a));
    }
    pb_close(b);
}

/* ------------------------------------------------------------ files, the sheet */

typedef struct bytes {
    u8 *data;
    size_t size, cap;
} bytes;

static MEGLER_CB bool bytes_write(void *sink, const void *data, size_t size)
{
    bytes *o = (bytes *)sink;
    if (o->size + size > o->cap) {
        o->cap = (o->size + size) * 2 + 4096;
        o->data = (u8 *)mg_realloc(o->data, o->cap);
    }
    if (size) memcpy(o->data + o->size, data, size);
    o->size += size;
    return true;
}

#define MAX_SHEET 40
static struct {
    char name[48];
    u32 *px;
    int w, h;
} t_sheet[MAX_SHEET];
static int t_nsheet;

/* The picture as <dir>/<name>.png, written by the stand-in's encoder (or as a BMP, when the run is asked for those); and kept for the sheet. */
static void keep(const char *name, pic *p)
{
    settle(p);
    if (t_nsheet < MAX_SHEET) {
        mg_strlcpy(t_sheet[t_nsheet].name, name, sizeof(t_sheet[t_nsheet].name));
        t_sheet[t_nsheet].px = (u32 *)mg_alloc(sizeof(u32) * (size_t)p->w * (size_t)p->h);
        memcpy(t_sheet[t_nsheet].px, p->px, sizeof(u32) * (size_t)p->w * (size_t)p->h);
        t_sheet[t_nsheet].w = p->w;
        t_sheet[t_nsheet].h = p->h;
        t_nsheet++;
    }
    if (t_config->out_dir) {
        bytes png;
        char *file = mg_sprintf("%s/%s%s.%s", t_config->out_dir, t_config->file_prefix ? t_config->file_prefix : "", name, t_config->bmp ? "bmp" : "png");
        memset(&png, 0, sizeof(png));
        if (t_config->bmp) {
            if (!mg_gfx_write_bmp(file, p->px, p->w, p->h, p->w * 4)) say("  (could not write %s)", file);
        } else if (!S->image_encode(S->user, p->px, p->w, p->h, p->w * 4, MEGLER_GFX_PNG, 100, bytes_write, &png) || !mg_file_write_all(file, png.data, png.size)) {
            say("  (could not write %s)", file);
        }
        free(png.data);
        free(file);
    }
}

/* ------------------------------------------------- one picture against another */

typedef struct difference {
    int inside;                        /* pixels well inside a shape of the stand-in's (or well outside all) that are another colour in the host's */
    int points;                        /* pixels two away from an edge of the stand-in's that differ: the tips of sharp corners, which only a host that smooths draws */
    int edges;                         /* pixels on an edge of the stand-in's that are another colour in the host's */
    int beyond;                        /* of the last two, the ones whose colour is not between those of the stand-in's pixels around them */
    int worst;                         /* the largest step by which a pixel inside differs */
    int x, y;                          /* where the first pixel inside differs */
} difference;

static int channel(u32 p, int shift) { return (int)((p >> shift) & 255); }

static int apart(u32 a, u32 b)
{
    int shift, worst = 0;
    for (shift = 0; shift < 32; shift += 8) {
        int d = abs(channel(a, shift) - channel(b, shift));
        if (d > worst) worst = d;
    }
    return worst;
}

/*
 * A pixel of the stand-in's picture with the same colour all around it is
 * inside a shape or outside all of them; one with another colour next to it
 * is on an edge. Around means two pixels each way: a smoothed corner of
 * thirty-six degrees is still a tenth of a pixel wide a pixel beyond the
 * last pixel whose centre it covers.
 */
static void compare(const pic *host, const pic *standin, int slack, difference *out)
{
    int x, y, dx, dy, shift;
    memset(out, 0, sizeof(*out));
    for (y = 0; y < host->h; y++)
        for (x = 0; x < host->w; x++) {
            u32 h = at(host, x, y), s = at(standin, x, y);
            int lo[4], hi[4], d = apart(h, s);
            bool next = false, near = false, between = true;
            if (d <= slack) continue;
            for (shift = 0; shift < 32; shift += 8) lo[shift / 8] = hi[shift / 8] = channel(s, shift);
            for (dy = -2; dy <= 2; dy++)
                for (dx = -2; dx <= 2; dx++) {
                    u32 n;
                    if (x + dx < 0 || y + dy < 0 || x + dx >= host->w || y + dy >= host->h) continue;
                    n = at(standin, x + dx, y + dy);
                    if (n == s) continue;
                    near = true;
                    if (abs(dx) <= 1 && abs(dy) <= 1) next = true;
                    for (shift = 0; shift < 32; shift += 8) {
                        if (channel(n, shift) < lo[shift / 8]) lo[shift / 8] = channel(n, shift);
                        if (channel(n, shift) > hi[shift / 8]) hi[shift / 8] = channel(n, shift);
                    }
                }
            if (!near) {
                if (!out->inside) {
                    out->x = x;
                    out->y = y;
                }
                out->inside++;
                if (d > out->worst) out->worst = d;
                continue;
            }
            if (next) out->edges++;
            else out->points++;
            for (shift = 0; shift < 32; shift += 8)
                if (channel(h, shift) < lo[shift / 8] - slack || channel(h, shift) > hi[shift / 8] + slack) between = false;
            if (!between) out->beyond++;
        }
}

/*
 * A scene drawn by the host and by the stand-in, and the two compared.
 * `exact`: the scene has nothing in it that the two may draw differently
 * (upright rectangles, edges not smoothed), so no pixel may differ at all.
 */
typedef void (*scene_fn)(pic *p);

static void both(const char *name, scene_fn scene, int w, int h, bool smooth, bool exact)
{
    pic host, standin;
    difference d;
    t_smooth = smooth;
    pic_make(&host, H, w, h);
    pic_make(&standin, S, w, h);
    CHECK(host.cv != NULL);
    if (host.cv) {
        scene(&host);
        scene(&standin);
        settle(&host);
        compare(&host, &standin, 1, &d);
        say("  %-18s %dx%d: inside shapes %d pixels differ from the stand-in's; on edges %d differ and at sharp points %d, %d of them beyond the colours around them", name, w,
            h, d.inside, d.edges, d.points, d.beyond);
        if (d.inside) say("    (by up to %d steps; the first at %d,%d: %08x for the stand-in's %08x)", d.worst, d.x, d.y, at(&host, d.x, d.y), at(&standin, d.x, d.y));
        CHECK(d.inside == 0);
        CHECK(d.beyond == 0);
        CHECK(d.points <= 24);
        if (exact) CHECK(d.edges == 0 && d.points == 0);
        keep(name, &host);
        if (t_config->out_dir && (d.inside || d.beyond || (exact && d.edges))) {
            char *other = mg_sprintf("%s-standin", name);
            keep(other, &standin);
            free(other);
        }
    }
    pic_done(&host);
    pic_done(&standin);
    t_smooth = true;
}

/* ------------------------------------- scenes the stand-in draws as megler.h says */

static void scene_fills(pic *p)
{
    megler_gfx_paint clear = paint_of(0xFF000000u);
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    fill(p, 20, 20, 180, 120, 0xFFE02020u);
    fill(p, 100, 60, 260, 160, 0x800000FFu);
    fill(p, 60.3f, 140.7f, 200.6f, 220.2f, 0xC000A040u);
    /* Partly outside the bitmap. */
    fill(p, -30, 300, 90, 400, 0xFF8040C0u);
    fill(p, 440, -20, 520, 60, 0xFFC0C000u);
    /* Half transparent red on white. */
    fill(p, 300, 20, 380, 100, 0x80FF0000u);
    /* A hole to nothing, and half transparent colours on nothing and on each other. */
    clear.blend = MEGLER_GFX_BLEND_CLEAR;
    rect(p, 290, 130, 470, 310, &clear, NULL);
    fill(p, 300, 140, 400, 240, 0x80FF0000u);
    fill(p, 350, 190, 460, 300, 0x8000FF00u);
    fill(p, 20, 240, 21, 241, 0xFF000000u);
    fill(p, 30.5f, 240.5f, 35.5f, 245.5f, 0xFF000000u);
}

static void scene_corners(pic *p)
{
    megler_gfx_paint half = paint_of(0x80FFFFFFu);
    color(p, 0xFF808080u, MEGLER_GFX_BLEND_SRC);
    fill(p, 0, 0, 8, 8, 0xFFFF0000u);
    fill(p, (float)p->w - 8, 0, (float)p->w, 8, 0xFF00FF00u);
    fill(p, 0, (float)p->h - 8, 8, (float)p->h, 0xFF0000FFu);
    half.blend = MEGLER_GFX_BLEND_SRC;
    rect(p, (float)p->w - 8, (float)p->h - 8, (float)p->w, (float)p->h, &half, NULL);
}

static void scene_shapes(pic *p)
{
    megler_gfx_paint a = paint_of(0xFF2060C0u), b = paint_of(0xB0E08020u), c = paint_of(0xFF208040u), s = stroke_of(0xFF000000u, 6);
    pathbuf star, dome, tri;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    round_rect(p, 20, 20, 180, 110, 24, 16, &a, NULL);
    round_rect(p, 110.5f, 60.25f, 250, 140, 40, 40, &b, NULL);
    oval(p, 270, 20, 460, 120, &c, NULL);
    oval(p, 300, 40, 380, 150, &b, NULL);
    memset(&tri, 0, sizeof(tri));
    pb_move(&tri, 30, 150);
    pb_line(&tri, 150, 170);
    pb_line(&tri, 60, 250);
    pb_close(&tri);
    path(p, tri.v, tri.n, MEGLER_GFX_WINDING, &a, NULL);
    /* The same star by both rules. */
    memset(&star, 0, sizeof(star));
    pb_star(&star, 230, 220, 70);
    path(p, star.v, star.n, MEGLER_GFX_WINDING, &c, NULL);
    memset(&star, 0, sizeof(star));
    pb_star(&star, 390, 220, 70);
    path(p, star.v, star.n, MEGLER_GFX_EVEN_ODD, &c, NULL);
    /* Curves: a quadratic dome and a cubic wave, each closed by a line. */
    memset(&dome, 0, sizeof(dome));
    pb_move(&dome, 20, 340);
    pb_quad(&dome, 90, 230, 160, 340);
    pb_close(&dome);
    pb_move(&dome, 180, 320);
    pb_cubic(&dome, 240, 250, 300, 390, 360, 320);
    pb_line(&dome, 360, 345);
    pb_line(&dome, 180, 345);
    pb_close(&dome);
    path(p, dome.v, dome.n, MEGLER_GFX_WINDING, &b, NULL);
    /* Outlines of curved shapes: the stand-in's are within a pixel. */
    oval(p, 380, 280, 460, 340, &s, NULL);
    s.stroke_width = 4;
    s.color = 0xC0C02020u;
    round_rect(p, 30, 270, 130, 300, 10, 10, &s, NULL);
}

static void scene_strokes_square(pic *p)
{
    megler_gfx_paint s = stroke_of(0xFF000000u, 1), both_ways = paint_of(0x80C02020u), hair = stroke_of(0xFF0000C0u, 0), cap = stroke_of(0xFF207020u, 8);
    int i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    /* Frames of several widths, on whole and on half coordinates. */
    for (i = 0; i < 5; i++) {
        s.stroke_width = (float)(i + 1);
        rect(p, 20.0f + 70 * (float)i, 20, 70.0f + 70 * (float)i, 70, &s, NULL);
        rect(p, 20.5f + 70 * (float)i, 90.5f, 70.5f + 70 * (float)i, 140.5f, &s, NULL);
    }
    s.stroke_width = 9;
    s.color = 0x802020C0u;
    rect(p, 390, 30, 450, 130, &s, NULL);
    /* Filled and stroked in a half transparent colour: every pixel of it once. */
    both_ways.style = MEGLER_GFX_FILL_AND_STROKE;
    both_ways.stroke_width = 10;
    rect(p, 30, 170, 130, 250, &both_ways, NULL);
    /* Lines: flat ends, and square ones half a width longer. */
    line(p, 170, 170, 300, 170, &cap);
    line(p, 170, 190, 300, 190, &cap);
    cap.cap = MEGLER_GFX_CAP_SQUARE;
    line(p, 170, 210, 300, 210, &cap);
    line(p, 330, 160, 330, 250, &cap);
    cap.stroke_width = 3;
    line(p, 360, 160, 450, 250, &cap);
    /* Hairlines: a pixel wide, also under a matrix that makes everything three times as large. */
    line(p, 20, 280.5f, 200, 280.5f, &hair);
    line(p, 210.5f, 270, 210.5f, 340, &hair);
    matrix_set(p, 3, 0, 240, 0, 3, 270);
    line(p, 0, 5.5f, 60, 5.5f, &hair);
    rect(p, 10, 10, 30, 20, &hair, NULL);
    s.stroke_width = 1;
    s.color = 0xFF000000u;
    rect(p, 40, 10, 60, 20, &s, NULL);
    matrix_reset(p);
}

static void scene_clips_rect(pic *p)
{
    saved a, b;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    /* A clip under a matrix stays where it was when the matrix changes. */
    a = save(p);
    translate(p, 30, 20);
    scale(p, 2, 2);
    clip_rect(p, 0, 0, 60, 40, MEGLER_GFX_CLIP_INTERSECT);
    matrix_reset(p);
    color(p, 0xFFE02020u, MEGLER_GFX_BLEND_SRC_OVER);
    /* Inside it, a second one that can only take away, and a colour by another mode. */
    b = save(p);
    clip_rect(p, 0, 0, 90.4f, 60.6f, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0x800000FFu, MEGLER_GFX_BLEND_SRC_OVER);
    restore(p, b);
    fill(p, 100, 70, 400, 400, 0xFF20A020u);
    restore(p, a);
    /* A rectangle cut out of the clip, and another out of what is left. */
    a = save(p);
    clip_rect(p, 200, 20, 440, 140, MEGLER_GFX_CLIP_INTERSECT);
    clip_rect(p, 240, 40, 300, 100, MEGLER_GFX_CLIP_DIFFERENCE);
    clip_rect(p, 280, 80, 400, 120, MEGLER_GFX_CLIP_DIFFERENCE);
    color(p, 0xC02060C0u, MEGLER_GFX_BLEND_SRC_OVER);
    fill(p, 0, 0, 480, 60, 0x80F0C020u);
    restore(p, a);
    /* A quarter turn keeps rectangles upright: x' = 300 - y, y' = 180 + x. */
    a = save(p);
    matrix_set(p, 0, -1, 300, 1, 0, 180);
    clip_rect(p, 10, 20, 150, 100, MEGLER_GFX_CLIP_INTERSECT);
    fill(p, 0, 0, 80, 300, 0xFF8040C0u);
    fill(p, 60, 40, 200, 60, 0x8000C0C0u);
    restore(p, a);
    /* Saves nest; a clip to nothing draws nothing. */
    a = save(p);
    clip_rect(p, 320, 180, 460, 340, MEGLER_GFX_CLIP_INTERSECT);
    b = save(p);
    clip_rect(p, 340, 200, 380, 240, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0xFF000000u, MEGLER_GFX_BLEND_SRC);
    restore(p, b);
    b = save(p);
    clip_rect(p, 0, 0, 10, 10, MEGLER_GFX_CLIP_INTERSECT);
    restore(p, b);
    color(p, 0x60FF8000u, MEGLER_GFX_BLEND_SRC_OVER);
    restore(p, a);
    fill(p, 10, 300, 300, 350, 0x40000000u);
}

/* Every transfer mode but DST (which draws nothing) and OVERLAY (which the stand-in does not do): one colour laid on another, each partly on nothing. */
static void scene_blends(pic *p)
{
    static const int modes[16] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17 };
    int i;
    for (i = 0; i < 16; i++) {
        float x = 10.0f + 118 * (float)(i % 4), y = 8.0f + 88 * (float)(i / 4);
        megler_gfx_paint src = paint_of(0xA0F08020u);
        fill(p, x, y, x + 70, y + 55, 0xC02060E0u);
        fill(p, x + 5, y + 5, x + 30, y + 20, 0xFF10C040u);
        src.blend = modes[i];
        rect(p, x + 35, y + 25, x + 105, y + 78, &src, NULL);
    }
}

/* A picture of four by four pixels, every one another colour, some of them partly transparent. */
static void small_picture(pic *s, const megler_gfx_host *g)
{
    static const u32 colours[16] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFFFF00FFu, 0xFF000000u, 0xFFFFFFFFu,
                                     0x80FF0000u, 0x8000FF00u, 0x800000FFu, 0x40FFFFFFu, 0xC0804020u, 0x00000000u, 0xFF808080u, 0xFF204060u };
    int i;
    pic_make(s, g, 4, 4);
    for (i = 0; i < 16; i++) {
        u32 c = colours[i], a = c >> 24;
        s->px[i] = a << 24 | ((((c >> 16) & 255) * a + 127) / 255) << 16 | ((((c >> 8) & 255) * a + 127) / 255) << 8 | ((c & 255) * a + 127) / 255;
    }
    wrote(s);
}

static void scene_bitmaps_nearest(pic *p)
{
    pic s;
    megler_gfx_paint plain = paint_of(0xFF000000u), faint = paint_of(0x80000000u), tint = paint_of(0xFF000000u), out = paint_of(0xFF000000u), copy = paint_of(0xC0000000u);
    saved a;
    small_picture(&s, p->g);
    plain.flags = faint.flags = tint.flags = out.flags = copy.flags = 0;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    fill(p, 0, 180, 480, 360, 0xFF406080u);
    /* Eight times the size; a part of it sixteen times; its own size. */
    bitmap(p, &s, 0, 0, 4, 4, 16, 16, 48, 48, &plain);
    bitmap(p, &s, 1, 1, 3, 3, 64, 16, 96, 48, &plain);
    bitmap(p, &s, 0, 0, 4, 4, 112, 16, 116, 20, &plain);
    bitmap(p, &s, 0, 0, 4, 4, 130, 10, 250, 130, &plain);
    /* With the paint's alpha; tinted (SRC_IN: the picture's shape in the filter's colour). */
    bitmap(p, &s, 0, 0, 4, 4, 260, 10, 340, 90, &faint);
    tint.filter_mode = MEGLER_GFX_BLEND_SRC_IN;
    tint.filter_color = 0xFFC02080u;
    bitmap(p, &s, 0, 0, 4, 4, 350, 10, 430, 90, &tint);
    tint.filter_mode = MEGLER_GFX_BLEND_MODULATE;
    tint.filter_color = 0xFF80FF80u;
    bitmap(p, &s, 0, 0, 4, 4, 260, 96, 340, 176, &tint);
    /* Through a matrix: larger and moved; turned a quarter. */
    a = save(p);
    translate(p, 20, 200);
    scale(p, 10, 5);
    bitmap(p, &s, 0, 0, 4, 4, 0, 0, 8, 16, &plain);
    restore(p, a);
    a = save(p);
    matrix_set(p, 0, -1, 220, 1, 0, 200);
    bitmap(p, &s, 0, 0, 4, 4, 0, 0, 120, 100, &plain);
    restore(p, a);
    /* By other transfer modes: what is there is cut away by the picture's alpha; the picture replaces what is there, its alpha too. */
    out.blend = MEGLER_GFX_BLEND_DST_OUT;
    bitmap(p, &s, 0, 0, 4, 4, 240, 200, 340, 300, &out);
    copy.blend = MEGLER_GFX_BLEND_SRC;
    bitmap(p, &s, 0, 0, 4, 4, 360, 200, 460, 300, &copy);
    copy.blend = MEGLER_GFX_BLEND_DST_IN;
    copy.color = 0xFF000000u;
    bitmap(p, &s, 2, 0, 4, 2, 240, 310, 340, 350, &copy);
    pic_done(&s);
}

/* A rounded rectangle as the core's path_round_rect makes a view's outline: straight sides and a cubic in each corner. */
static void pb_round_rect(pathbuf *b, float l, float t, float r, float bt, float rad)
{
    const float q = 0.5522847f * rad;
    pb_move(b, l + rad, t);
    pb_line(b, r - rad, t);
    pb_cubic(b, r - rad + q, t, r, t + rad - q, r, t + rad);
    pb_line(b, r, bt - rad);
    pb_cubic(b, r, bt - rad + q, r - rad + q, bt, r - rad, bt);
    pb_line(b, l + rad, bt);
    pb_cubic(b, l + rad - q, bt, l, bt - rad + q, l, bt - rad);
    pb_line(b, l, t + rad);
    pb_cubic(b, l, t + rad - q, l + rad - q, t, l + rad, t);
    pb_close(b);
}

/*
 * Clips that are one rounded rectangle, as views' outlines make them (Compose's tiles): the host draws what keeps off the
 * corners with the box alone, a colour or a rectangle or a bitmap that reaches them filled through the rounded rectangle,
 * and a rounded rectangle that is the clip's own with no clip (GfxHost.cpp, Clip::round), the rest by a layer. (The stand-in
 * clips by a path's box: checked by pixels, check_clips_round.)
 */
static void scene_clips_round(pic *p)
{
    pic s;
    saved a;
    pathbuf v;
    megler_gfx_paint green = paint_of(0xFF20A040u), red = paint_of(0xFFC02040u), plain = paint_of(0xFF000000u), blue = paint_of(0xFF2040C0u);
    small_picture(&s, p->g);
    plain.flags = 0;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    /* A tile: a colour over all of it, a picture at its left reaching two corners, a bar inside off them. */
    a = save(p);
    memset(&v, 0, sizeof(v));
    pb_round_rect(&v, 20, 20, 220, 100, 12);
    clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0xFF303030u, MEGLER_GFX_BLEND_SRC_OVER);
    bitmap(p, &s, 0, 0, 4, 4, 20, 20, 100, 100, &plain);
    rect(p, 120, 50, 200, 70, &green, NULL);
    restore(p, a);
    /* Under a matrix that scales and moves: a rounded rectangle that is the clip's own, a rectangle reaching the lower corners. */
    a = save(p);
    translate(p, 260, 20);
    scale(p, 2, 2);
    memset(&v, 0, sizeof(v));
    pb_round_rect(&v, 0, 0, 100, 50, 10);
    clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
    round_rect(p, 0, 0, 100, 50, 10, 10, &red, NULL);
    rect(p, -10, 25, 110, 60, &green, NULL);
    restore(p, a);
    /* What still goes by a layer: an oval reaching the corners; and a rectangle off whole pixels, inside a clip narrowed by a box. */
    a = save(p);
    memset(&v, 0, sizeof(v));
    pb_round_rect(&v, 20, 130, 220, 220, 20);
    clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
    clip_rect(p, 20, 130, 180, 220, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0xFF606060u, MEGLER_GFX_BLEND_SRC_OVER);
    p->g->draw_oval(p->g->user, p->cv, 10, 120, 120, 230, &blue, NULL);
    rect(p, 100.5f, 140.25f, 230, 230, &red, NULL);
    restore(p, a);
    pic_done(&s);
}

static void check_clips_round(pic *p)
{
    /* The tile: its corners outside the rounded rectangle white, the picture's first cell, the colour and the bar inside. */
    CHECK(at(p, 21, 21) == 0xFFFFFFFFu && at(p, 219, 21) == 0xFFFFFFFFu && at(p, 21, 99) == 0xFFFFFFFFu && at(p, 219, 99) == 0xFFFFFFFFu);
    CHECK(at(p, 30, 30) == 0xFFFF0000u && at(p, 150, 30) == 0xFF303030u && at(p, 160, 60) == 0xFF20A040u && at(p, 210, 90) == 0xFF303030u);
    /* Scaled twice and moved: the corners outside white, the clip's own rounded rectangle red above, the rectangle green below. */
    CHECK(at(p, 261, 21) == 0xFFFFFFFFu && at(p, 459, 21) == 0xFFFFFFFFu && at(p, 261, 119) == 0xFFFFFFFFu && at(p, 459, 119) == 0xFFFFFFFFu);
    CHECK(at(p, 360, 40) == 0xFFC02040u && at(p, 360, 110) == 0xFF20A040u && at(p, 275, 110) == 0xFF20A040u);
    /* By a layer: the corners outside white, the oval blue, the rectangle red, the box's cut at x = 180 white beyond it. */
    CHECK(at(p, 21, 131) == 0xFFFFFFFFu && at(p, 21, 219) == 0xFFFFFFFFu && at(p, 60, 175) == 0xFF2040C0u && at(p, 150, 180) == 0xFFC02040u &&
          at(p, 170, 135) == 0xFF606060u && at(p, 190, 175) == 0xFFFFFFFFu);
}

static void scene_layer_alpha(pic *p)
{
    float bounds[4] = { 40, 30, 200, 150 };
    saved a, b;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    fill(p, 0, 90, 480, 120, 0xFF000000u);
    /* What is drawn in a layer comes out fainter by the layer's alpha; the bounds clip. */
    a = save_layer(p, bounds, 128, MEGLER_GFX_BLEND_SRC_OVER);
    fill(p, 0, 0, 120, 400, 0xFFE02020u);
    fill(p, 120, 0, 480, 100, 0xFF2020E0u);
    restore(p, a);
    /* Without bounds: the whole clip. */
    a = save(p);
    clip_rect(p, 220, 30, 460, 150, MEGLER_GFX_CLIP_INTERSECT);
    b = save_layer(p, NULL, 64, MEGLER_GFX_BLEND_SRC_OVER);
    fill(p, 240, 0, 330, 400, 0xFF20A020u);
    fill(p, 330, 60, 480, 400, 0xFF000000u);
    restore(p, b);
    restore(p, a);
    /* A layer in a layer: the alphas multiply. */
    a = save_layer(p, NULL, 128, MEGLER_GFX_BLEND_SRC_OVER);
    fill(p, 40, 180, 140, 330, 0xFFC08020u);
    bounds[0] = 160;
    bounds[1] = 180;
    bounds[2] = 300;
    bounds[3] = 330;
    b = save_layer(p, bounds, 128, MEGLER_GFX_BLEND_SRC_OVER);
    fill(p, 0, 0, 480, 250, 0xFF8020C0u);
    restore(p, b);
    fill(p, 320, 180, 440, 330, 0x80202020u);
    restore(p, a);
}

/* ------------------------------------------- scenes of what only a real host draws */

/* A pixel that has been drawn on: not the white the scenes begin with, nor nearly so (the faint rim of a smoothed edge does not count). */
static bool inked(const pic *p, int x, int y) { return apart(at(p, x, y), 0xFFFFFFFFu) > 48; }

/* A colour a part of the way from one to another, each channel by itself: what a gradient is between two stops. */
static u32 between(u32 a, u32 b, double part)
{
    u32 out = 0;
    int shift;
    for (shift = 0; shift < 32; shift += 8) out |= (u32)(channel(a, shift) + (channel(b, shift) - channel(a, shift)) * part + 0.5) << shift;
    return out;
}

/* A chequered ground, for what is partly transparent to show against. */
static void chequer(pic *p, float l, float t, float r, float b)
{
    saved s = save(p);
    int ix, iy;
    bool smooth = t_smooth;
    t_smooth = false;
    clip_rect(p, l, t, r, b, MEGLER_GFX_CLIP_INTERSECT);
    for (iy = 0; (float)iy * 10 + t < b; iy++)
        for (ix = 0; (float)ix * 10 + l < r; ix++)
            fill(p, l + (float)ix * 10, t + (float)iy * 10, l + (float)ix * 10 + 10, t + (float)iy * 10 + 10, (ix + iy) & 1 ? 0xFFB8B8B8u : 0xFFE8E8E8u);
    t_smooth = smooth;
    restore(p, s);
}

static void scene_strokes(pic *p)
{
    static const char *const caps[3] = { "butt", "round", "square" }, *const joins[3] = { "miter", "round", "bevel" };
    megler_gfx_paint guide = stroke_of(0xFFE00000u, 0), s;
    pathbuf v;
    int i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    /* The three caps on a line from x = 50 to x = 170: the guides are the line's two ends. */
    for (i = 0; i < 3; i++) {
        float y = 30.0f + 34 * (float)i;
        s = stroke_of(0xFF303030u, 16);
        s.cap = i;
        line(p, 50, y, 170, y, &s);
        label(p, caps[i], 200, y + 4);
    }
    line(p, 50.5f, 10, 50.5f, 120, &guide);
    line(p, 170.5f, 10, 170.5f, 120, &guide);
    /* The three joins on a corner of about a right angle and one of thirty-seven degrees (whose miter, 3.2 widths long, is within the limit). */
    for (i = 0; i < 3; i++) {
        float x = 40.0f + 150 * (float)i;
        s = stroke_of(0xC02060C0u, 14);
        s.join = i;
        memset(&v, 0, sizeof(v));
        pb_move(&v, x, 200);
        pb_line(&v, x + 40, 140);
        pb_line(&v, x + 100, 200);
        pb_move(&v, x, 296);
        pb_line(&v, x + 90, 266);
        pb_line(&v, x, 236);
        path(p, v.v, v.n, MEGLER_GFX_WINDING, &s, NULL);
        label(p, joins[i], x + 30, 222);
    }
    /* The miter limit, 4: a corner of 30 degrees is still a miter (its tip is 3.86 widths long), one of 28 degrees is cut off (4.13). */
    for (i = 0; i < 2; i++) {
        double half = (i ? 28.0 : 30.0) * 3.14159265358979 / 360;
        float x = 380, y = 30.0f + 60 * (float)i;
        s = stroke_of(0xFF207020u, 6);
        memset(&v, 0, sizeof(v));
        pb_move(&v, x - 90 * (float)cos(half), y - 90 * (float)sin(half));
        pb_line(&v, x, y);
        pb_line(&v, x - 90 * (float)cos(half), y + 90 * (float)sin(half));
        path(p, v.v, v.n, MEGLER_GFX_WINDING, &s, NULL);
        label(p, i ? "28 deg: bevel" : "30 deg: miter", 400, y + 22);
    }
    /* A curve with round ends; a closed figure, whose every corner is a join; a hairline through a matrix. */
    s = stroke_of(0xFFC04020u, 9);
    s.cap = MEGLER_GFX_CAP_ROUND;
    memset(&v, 0, sizeof(v));
    pb_move(&v, 30, 330);
    pb_cubic(&v, 90, 270, 130, 390, 200, 320);
    path(p, v.v, v.n, MEGLER_GFX_WINDING, &s, NULL);
    s = stroke_of(0xFF000000u, 5);
    s.join = MEGLER_GFX_JOIN_ROUND;
    memset(&v, 0, sizeof(v));
    pb_move(&v, 250, 340);
    pb_line(&v, 300, 290);
    pb_line(&v, 350, 340);
    pb_close(&v);
    path(p, v.v, v.n, MEGLER_GFX_WINDING, &s, NULL);
    matrix_set(p, 4, 0.5f, 380, -0.5f, 4, 300);
    s = stroke_of(0xFF0000C0u, 0);
    oval(p, 0, 0, 20, 10, &s, NULL);
    matrix_reset(p);
    label(p, "hairline, x4", 380, 352);
}

static void check_strokes(pic *p)
{
    /* A butt cap ends at the line's end; a square one goes half a width (8) on; a round one too, but not in its corners. */
    CHECK(!inked(p, 47, 30) && inked(p, 53, 30) && inked(p, 167, 30) && !inked(p, 173, 30));
    CHECK(inked(p, 44, 64) && !inked(p, 40, 64) && !inked(p, 43, 57) && inked(p, 55, 57));
    CHECK(inked(p, 44, 98) && inked(p, 43, 91) && !inked(p, 40, 98) && inked(p, 176, 104) && !inked(p, 180, 98));
    /* A miter join comes to a point above the corner at (x + 40, 140); a bevel is cut off there, a round one is in between. */
    CHECK(inked(p, 80, 132) && !inked(p, 80 + 300, 132) && inked(p, 80 + 300, 139) && inked(p, 80 + 150, 134) && !inked(p, 80 + 150, 131));
    /* And sideways, at the sharper corner at (x + 90, 266): the miter's point is 22 beyond it, the bevel ends 2 beyond, the round one 7. */
    CHECK(inked(p, 130 + 14, 266) && !inked(p, 430 + 6, 266) && inked(p, 280 + 5, 266) && !inked(p, 280 + 10, 266));
    /* The miter limit: the 30 degree corner's point reaches past the corner at x = 380, the 28 degree one's does not. */
    CHECK(inked(p, 388, 30) && !inked(p, 388, 90) && inked(p, 378, 90));
}

static void scene_arcs(pic *p)
{
    megler_gfx_paint a = paint_of(0xFF2060C0u), s = stroke_of(0xFF000000u, 3), b = paint_of(0xA0E08020u);
    pathbuf v;
    int i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    /* drawArc with the centre: wedges of 60, 135 and 300 degrees. */
    for (i = 0; i < 3; i++) {
        static const float sweeps[3] = { 60, 135, 300 };
        float x = 20.0f + 150 * (float)i;
        memset(&v, 0, sizeof(v));
        pb_move(&v, x + 60, 70);
        pb_arc(&v, x, 10, x + 120, 130, -30, sweeps[i], false);
        pb_close(&v);
        path(p, v.v, v.n, MEGLER_GFX_WINDING, i == 1 ? &b : &a, NULL);
        path(p, v.v, v.n, MEGLER_GFX_WINDING, &s, NULL);
    }
    /* drawArc without it: the arc alone, stroked, with each cap; counter-clockwise too. */
    for (i = 0; i < 3; i++) {
        megler_gfx_paint arc = stroke_of(0xFFC02040u, 12);
        float x = 20.0f + 150 * (float)i;
        arc.cap = i;
        memset(&v, 0, sizeof(v));
        pb_arc(&v, x, 150, x + 120, 230, 200, i == 2 ? -220.0f : 140.0f, true);
        path(p, v.v, v.n, MEGLER_GFX_WINDING, &arc, NULL);
    }
    /* A whole turn; ovals and round rectangles, filled and outlined. */
    memset(&v, 0, sizeof(v));
    pb_arc(&v, 30, 250, 130, 340, 0, 360, true);
    pb_close(&v);
    path(p, v.v, v.n, MEGLER_GFX_WINDING, &b, NULL);
    oval(p, 160, 250, 300, 340, &a, NULL);
    oval(p, 160, 250, 300, 340, &s, NULL);
    round_rect(p, 320, 250, 460, 340, 45, 20, &b, NULL);
    s.stroke_width = 8;
    s.color = 0x80000000u;
    round_rect(p, 330, 260, 450, 330, 12, 12, &s, NULL);
}

static void scene_paths(pic *p)
{
    megler_gfx_paint a = paint_of(0xFF208040u), s = stroke_of(0xFF000000u, 2), both_ways = paint_of(0x90C020A0u);
    pathbuf v;
    int i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    for (i = 0; i < 2; i++) {
        float x = 20.0f + 230 * (float)i;
        int rule = i ? MEGLER_GFX_EVEN_ODD : MEGLER_GFX_WINDING;
        /*
         * Three squares, one in another: two the same way round and the innermost the other way. The outline winds round the
         * rings once and twice and round the innermost once, so winding fills all of it; a ray crosses one, two and three sides,
         * so even-odd leaves the middle ring out.
         */
        memset(&v, 0, sizeof(v));
        pb_move(&v, x, 20);
        pb_line(&v, x + 140, 20);
        pb_line(&v, x + 140, 160);
        pb_line(&v, x, 160);
        pb_close(&v);
        pb_move(&v, x + 25, 45);
        pb_line(&v, x + 115, 45);
        pb_line(&v, x + 115, 135);
        pb_line(&v, x + 25, 135);
        pb_close(&v);
        pb_move(&v, x + 50, 70);
        pb_line(&v, x + 50, 110);
        pb_line(&v, x + 90, 110);
        pb_line(&v, x + 90, 70);
        pb_close(&v);
        path(p, v.v, v.n, rule, &a, NULL);
        label(p, i ? "even-odd" : "winding", x + 150, 30);
        /* A star, and a figure of curves that crosses itself. */
        memset(&v, 0, sizeof(v));
        pb_star(&v, x + 60, 250, 70);
        pb_move(&v, x + 130, 190);
        pb_cubic(&v, x + 260, 190, x + 100, 330, x + 215, 330);
        pb_quad(&v, x + 250, 260, x + 130, 300);
        pb_close(&v);
        path(p, v.v, v.n, rule, &both_ways, NULL);
        path(p, v.v, v.n, rule, &s, NULL);
    }
    /* An open figure: filled as if closed, stroked open. */
    memset(&v, 0, sizeof(v));
    pb_move(&v, 400, 40);
    pb_line(&v, 460, 60);
    pb_line(&v, 410, 120);
    path(p, v.v, v.n, MEGLER_GFX_WINDING, &both_ways, NULL);
    s.stroke_width = 4;
    path(p, v.v, v.n, MEGLER_GFX_WINDING, &s, NULL);
}

static void check_paths(pic *p)
{
    /* By winding the three squares are one filled square; by even-odd the ring between the outer two is left out. */
    CHECK(inked(p, 30, 30) && inked(p, 60, 60) && inked(p, 90, 90));
    CHECK(inked(p, 260, 30) && !inked(p, 290, 60) && inked(p, 320, 90));
    /* The star's middle likewise: inside by winding, outside by even-odd. */
    CHECK(inked(p, 80, 250) && !inked(p, 310, 250) && inked(p, 310, 200));
    /* The open figure's third side is filled up to but not drawn: just outside it, by the middle of that side, nothing. */
    CHECK(inked(p, 425, 70) && !inked(p, 398, 80));
}

static void circle_path(pathbuf *v, float cx, float cy, float r)
{
    memset(v, 0, sizeof(*v));
    pb_arc(v, cx - r, cy - r, cx + r, cy + r, 0, 360, true);
    pb_close(v);
}

static void scene_clips(pic *p)
{
    saved a, b;
    pathbuf v;
    megler_gfx_paint band = paint_of(0xFFE0A020u);
    int i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    /* A rectangle clip under a turned matrix is the turned rectangle, not its box. */
    a = save(p);
    translate(p, 110, 90);
    rotate(p, 25);
    clip_rect(p, -80, -50, 80, 50, MEGLER_GFX_CLIP_INTERSECT);
    matrix_reset(p);
    color(p, 0xFF2060C0u, MEGLER_GFX_BLEND_SRC_OVER);
    for (i = 0; i < 12; i++) fill(p, 0, 12.0f + 20 * (float)i, 480, 18.0f + 20 * (float)i, 0xFFFFFFFFu);
    restore(p, a);
    label(p, "clipRect, turned 25 deg", 20, 190);
    /* A clip by a path: a circle. */
    a = save(p);
    circle_path(&v, 340, 90, 75);
    clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0xFF208040u, MEGLER_GFX_BLEND_SRC_OVER);
    for (i = 0; i < 12; i++) rect(p, 250.0f + 20 * (float)i, 0, 258.0f + 20 * (float)i, 200, &band, NULL);
    restore(p, a);
    label(p, "clipPath", 320, 190);
    /* A circle cut out of a rectangle; then a rectangle inside that, and back out of it. */
    a = save(p);
    clip_rect(p, 30, 210, 210, 340, MEGLER_GFX_CLIP_INTERSECT);
    circle_path(&v, 120, 275, 50);
    clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_DIFFERENCE);
    color(p, 0xFFC02040u, MEGLER_GFX_BLEND_SRC_OVER);
    b = save(p);
    clip_rect(p, 30, 210, 90, 340, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0xFF000000u, MEGLER_GFX_BLEND_SRC_OVER);
    restore(p, b);
    fill(p, 150, 300, 480, 320, 0xFF2020E0u);
    restore(p, a);
    label(p, "difference", 90, 355);
    /* A star by even-odd, inside a circle; and a drawing by another transfer mode through such a clip. */
    a = save(p);
    circle_path(&v, 350, 275, 70);
    clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
    memset(&v, 0, sizeof(v));
    pb_star(&v, 350, 275, 90);
    clip_path(p, v.v, v.n, MEGLER_GFX_EVEN_ODD, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0xFF8040C0u, MEGLER_GFX_BLEND_SRC_OVER);
    band.blend = MEGLER_GFX_BLEND_CLEAR;
    rect(p, 0, 268, 480, 282, &band, NULL);
    restore(p, a);
    fill(p, 440, 330, 470, 350, 0xFF000000u);
}

static void check_clips(pic *p)
{
    /* The turned rectangle's middle is drawn; the corners of its box (about 41..179 by 11..169) are not. */
    CHECK(at(p, 110, 107) == 0xFF2060C0u && at(p, 45, 20) == 0xFFFFFFFFu && at(p, 175, 160) == 0xFFFFFFFFu && at(p, 175, 25) == 0xFFFFFFFFu);
    /* The circle's middle, and its box's corner. */
    CHECK(at(p, 345, 90) == 0xFF208040u && at(p, 270, 20) == 0xFFFFFFFFu && at(p, 410, 160) == 0xFFFFFFFFu);
    /* The rectangle with the circle cut out: black left of x = 90, red right of it, nothing in the circle; and the blue bar only in what is left of the clip. */
    CHECK(at(p, 40, 220) == 0xFF000000u && at(p, 200, 220) == 0xFFC02040u && at(p, 120, 275) == 0xFFFFFFFFu && at(p, 200, 310) == 0xFF2020E0u &&
          at(p, 230, 310) == 0xFFFFFFFFu);
    /* The star by even-odd inside the circle: its arm is drawn, its middle is not; the last rectangle: no clip is left over. */
    CHECK(at(p, 350, 213) == 0xFF8040C0u && at(p, 350, 262) == 0xFFFFFFFFu && at(p, 455, 340) == 0xFF000000u);
    /* CLEAR through a clip by a shape empties what is inside the shape and nothing else: emptied pixels are in the band's rows, inside the circle, and there are some. */
    {
        int x, y, emptied = 0, astray = 0;
        for (y = 0; y < p->h; y++)
            for (x = 0; x < p->w; x++) {
                if (at(p, x, y) != 0) continue;
                emptied++;
                if (y < 268 || y >= 282 || x < 280 || x >= 420) astray++;
            }
        CHECK(emptied > 200 && astray == 0);
    }
}

static void scene_layers(pic *p)
{
    float bounds[4];
    megler_gfx_paint red = paint_of(0xFFE02020u), blue = paint_of(0xFF2040E0u), mask = paint_of(0xFF000000u), faint;
    saved a;
    pathbuf v;
    int i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    chequer(p, 0, 0, 480, 360);
    /* Two discs at half strength: drawn one by one each lets the other show through; in a layer the pair is one picture, faded once. */
    faint = red;
    faint.color = 0x80E02020u;
    oval(p, 20, 20, 110, 110, &faint, NULL);
    faint.color = 0x802040E0u;
    oval(p, 70, 20, 160, 110, &faint, NULL);
    a = save_layer(p, NULL, 128, MEGLER_GFX_BLEND_SRC_OVER);
    oval(p, 190, 20, 280, 110, &red, NULL);
    oval(p, 240, 20, 330, 110, &blue, NULL);
    restore(p, a);
    label(p, "alpha 128: each, and as a layer", 20, 128);
    /* A picture cut to a disc, the way apps round a picture: in a layer, the disc first, then the picture by SRC_IN, which keeps of it what falls on the disc. */
    bounds[0] = 350;
    bounds[1] = 15;
    bounds[2] = 465;
    bounds[3] = 115;
    a = save_layer(p, bounds, 255, MEGLER_GFX_BLEND_SRC_OVER);
    circle_path(&v, 407, 65, 45);
    path(p, v.v, v.n, MEGLER_GFX_WINDING, &mask, NULL);
    for (i = 0; i < 6; i++) {
        megler_gfx_paint stripe = paint_of(i & 1 ? 0xFF20A060u : 0xFFE0C020u);
        stripe.blend = MEGLER_GFX_BLEND_SRC_IN;
        rect(p, 350.0f + 20 * (float)i, 15, 370.0f + 20 * (float)i, 115, &stripe, NULL);
    }
    restore(p, a);
    label(p, "cut to a disc", 375, 128);
    /* A layer put on by DST_OUT: what is drawn in it is cut out of what was there. */
    fill(p, 20, 150, 160, 250, 0xFF303030u);
    bounds[0] = 20;
    bounds[1] = 150;
    bounds[2] = 160;
    bounds[3] = 250;
    a = save_layer(p, bounds, 255, MEGLER_GFX_BLEND_DST_OUT);
    oval(p, 50, 165, 130, 235, &red, NULL);
    restore(p, a);
    label(p, "layer by DST_OUT", 30, 268);
    /* A layer put on by DST_IN: also where nothing was drawn in it, so only the disc is left of what was there. */
    fill(p, 180, 150, 320, 250, 0xFF303030u);
    bounds[0] = 180;
    bounds[2] = 320;
    a = save_layer(p, bounds, 255, MEGLER_GFX_BLEND_DST_IN);
    oval(p, 210, 165, 290, 235, &red, NULL);
    restore(p, a);
    label(p, "layer by DST_IN", 195, 268);
    /* A layer at 40 per cent with a layer in it, under a turned matrix, with a clip by a shape inside. */
    a = save(p);
    translate(p, 400, 200);
    rotate(p, -20);
    bounds[0] = -55;
    bounds[1] = -45;
    bounds[2] = 55;
    bounds[3] = 45;
    {
        saved outer = save_layer(p, bounds, 102, MEGLER_GFX_BLEND_SRC_OVER), inner;
        fill(p, -55, -45, 55, 45, 0xFF2040E0u);
        circle_path(&v, 0, 0, 35);
        clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
        inner = save_layer(p, NULL, 255, MEGLER_GFX_BLEND_SRC_OVER);
        fill(p, -55, -45, 55, 0, 0xFFFFFFFFu);
        fill(p, -55, 0, 55, 45, 0xFFE02020u);
        restore(p, inner);
        restore(p, outer);
    }
    restore(p, a);
    label(p, "nested, turned, 40%", 345, 268);
    /* A DST layer is thrown away. */
    a = save_layer(p, NULL, 255, MEGLER_GFX_BLEND_DST);
    color(p, 0xFF00FF00u, MEGLER_GFX_BLEND_SRC);
    restore(p, a);
}

static u32 mul255(u32 a, u32 b) { return (a * b + 127) / 255; }
static u32 premultiplied(u32 c)
{
    u32 a = c >> 24;
    return a << 24 | mul255((c >> 16) & 255, a) << 16 | mul255((c >> 8) & 255, a) << 8 | mul255(c & 255, a);
}
static u32 faded(u32 p, u32 alpha) { return mul255(p >> 24, alpha) << 24 | mul255((p >> 16) & 255, alpha) << 16 | mul255((p >> 8) & 255, alpha) << 8 | mul255(p & 255, alpha); }
static u32 over(u32 s, u32 d)
{
    u32 k = 255 - (s >> 24), out = 0;
    int shift;
    for (shift = 0; shift < 32; shift += 8) out |= (((s >> shift) & 255) + mul255((d >> shift) & 255, k)) << shift;
    return out;
}

static void check_layers(pic *p)
{
    /*
     * In the layer the blue disc hides the red one where they overlap, and the pair is faded once: the overlap at (260, 60) and the
     * blue disc's own part at (300, 60) are both blue at half strength over the ground, which is the same square of the chequered
     * ground's colour at both places. Drawn one by one, the overlap at (90, 60) has the red disc under the blue: it is darker.
     */
    CHECK_HEX(at(p, 260, 65), over(faded(0xFF2040E0u, 128), 0xFFE8E8E8u));
    CHECK_HEX(at(p, 300, 65), over(faded(0xFF2040E0u, 128), 0xFFE8E8E8u));
    CHECK(channel(at(p, 90, 65), 8) + 20 < channel(at(p, 260, 65), 8));
    /* The picture cut to a disc: stripes in the middle, the ground in the layer's corners. */
    CHECK((at(p, 405, 65) == 0xFF20A060u || at(p, 405, 65) == 0xFFE0C020u) && channel(at(p, 353, 18), 0) >= 0xB8 && channel(at(p, 462, 112), 0) >= 0xB8);
    /* DST_OUT: nothing at all is left where the disc was (the bitmap itself is cut, to no alpha); the rest of the dark rectangle stays. */
    CHECK(at(p, 90, 200) == 0 && at(p, 25, 155) == 0xFF303030u);
    /* DST_IN: the disc's place keeps the dark rectangle, the rest of the layer's bounds is cut, and beside the bounds nothing changed. */
    CHECK(at(p, 250, 200) == 0xFF303030u && at(p, 185, 155) == 0 && at(p, 315, 245) == 0 && channel(at(p, 325, 200), 0) >= 0xB8);
    /* The DST layer changed nothing. */
    CHECK(at(p, 5, 5) != 0xFF00FF00u && at(p, 240, 300) != 0xFF00FF00u);
}

static void scene_matrices(pic *p)
{
    static const char *const names[6] = { "as it is", "moved", "scaled 1.5 x 0.75", "turned 30 deg", "slanted", "mirrored" };
    pic s;
    int i;
    small_picture(&s, p->g);
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    for (i = 0; i < 6; i++) {
        megler_gfx_paint a = paint_of(0xFF2060C0u), o = stroke_of(0xFF000000u, 2), t = paint_of(0xFFC02020u), b = paint_of(0xFF000000u);
        float x = 30.0f + 160 * (float)(i % 3), y = 40.0f + 170 * (float)(i / 3);
        saved sv = save(p);
        translate(p, x, y);
        if (i == 1) translate(p, 20, 30);
        if (i == 2) scale(p, 1.5f, 0.75f);
        if (i == 3) {
            translate(p, 40, 30);
            rotate(p, 30);
            translate(p, -40, -30);
        }
        if (i == 4) skew(p, 0.5f, 0);
        if (i == 5) {
            translate(p, 100, 0);
            scale(p, -1, 1);
        }
        rect(p, 0, 0, 80, 60, &a, NULL);
        rect(p, 0, 0, 80, 60, &o, NULL);
        oval(p, 50, 10, 76, 50, &t, NULL);
        b.flags = 0;
        bitmap(p, &s, 0, 0, 4, 4, 6, 26, 34, 54, &b);
        t.color = 0xFFFFFFFFu;
        t.text_size = 20;
        text(p, "Aa1", 6, 22, &t, NULL);
        restore(p, sv);
        label(p, names[i], x, y + 115);
    }
    pic_done(&s);
}

/* A picture like the ones Android stretches: corners, sides and a middle, each its own colour, eight pixels to a piece. */
static void nine_picture(pic *s, const megler_gfx_host *g)
{
    static const u32 colours[9] = { 0xFFC02020u, 0xFF20A020u, 0xFF2020C0u, 0xFFC0A020u, 0xFFF0F0F0u, 0xFF20A0C0u, 0xFFA020C0u, 0xFF606060u, 0xFF000000u };
    int x, y;
    pic_make(s, g, 24, 24);
    for (y = 0; y < 24; y++)
        for (x = 0; x < 24; x++) s->px[y * 24 + x] = colours[(y / 8) * 3 + x / 8];
    wrote(s);
}

/* The nine pieces of it onto a rectangle, each stretched its own way, as NinePatch.draw does it: a dozen drawBitmap calls of one bitmap. */
static void nine_draw(pic *p, const pic *s, float l, float t, float r, float b, const megler_gfx_paint *paint)
{
    float sx[4] = { 0, 8, 16, 24 }, sy[4] = { 0, 8, 16, 24 }, dx[4], dy[4];
    int i, j;
    dx[0] = l;
    dx[1] = l + 8;
    dx[2] = r - 8;
    dx[3] = r;
    dy[0] = t;
    dy[1] = t + 8;
    dy[2] = b - 8;
    dy[3] = b;
    for (j = 0; j < 3; j++)
        for (i = 0; i < 3; i++) bitmap(p, s, sx[i], sy[j], sx[i + 1], sy[j + 1], dx[i], dy[j], dx[i + 1], dy[j + 1], paint);
}

/* A picture with something of everything in it, to be stretched and turned: 64 by 64. */
static void photo_picture(pic *s, const megler_gfx_host *g)
{
    megler_gfx_paint a;
    int x, y;
    pic_make(s, g, 64, 64);
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++) s->px[y * 64 + x] = 0xFF000000u | (u32)(x * 4) << 16 | (u32)(y * 4) << 8 | (u32)(255 - x * 2 - y * 2);
    wrote(s);
    a = paint_of(0xFFFFFFFFu);
    oval(s, 8, 8, 30, 30, &a, NULL);
    a = stroke_of(0xFF000000u, 1);
    line(s, 0, 63.5f, 64, 63.5f, &a);
    line(s, 0.5f, 0, 0.5f, 64, &a);
    line(s, 40, 10, 60, 30, &a);
    a = paint_of(0xFF000000u);
    a.text_size = 18;
    text(s, "Hk", 34, 56, &a, NULL);
    settle(s);
}

static void scene_bitmaps(pic *p)
{
    pic photo, nine, small;
    megler_gfx_paint smooth = paint_of(0xFF000000u), rough = paint_of(0xFF000000u), faint, tint;
    saved a;
    int i;
    photo_picture(&photo, p->g);
    nine_picture(&nine, p->g);
    small_picture(&small, p->g);
    smooth.flags = MEGLER_GFX_ANTI_ALIAS | MEGLER_GFX_FILTER_BITMAP;
    rough.flags = 0;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    chequer(p, 300, 0, 480, 130);
    /* Its own size; half as large again, smooth and not. */
    bitmap(p, &photo, 0, 0, 64, 64, 10, 10, 74, 74, &rough);
    bitmap(p, &photo, 0, 0, 64, 64, 84, 10, 180, 106, &smooth);
    bitmap(p, &photo, 0, 0, 64, 64, 190, 10, 286, 106, &rough);
    label(p, "1:1", 10, 88);
    label(p, "smooth", 110, 120);
    label(p, "nearest", 215, 120);
    /* Smaller; a part; partly transparent, and a picture with alpha of its own, over a chequered ground. */
    bitmap(p, &photo, 0, 0, 64, 64, 10, 96, 42, 128, &smooth);
    bitmap(p, &photo, 32, 32, 64, 64, 46, 96, 78, 128, &smooth);
    faint = smooth;
    faint.color = 0x66000000u;
    bitmap(p, &photo, 0, 0, 64, 64, 310, 10, 374, 74, &faint);
    bitmap(p, &small, 0, 0, 4, 4, 390, 10, 470, 90, &smooth);
    bitmap(p, &small, 0, 0, 4, 4, 390, 96, 422, 128, &rough);
    /* Stretchable pictures: nine pieces each, three sizes; edges must stay their own colours. */
    nine_draw(p, &nine, 10, 150, 160, 240, &smooth);
    nine_draw(p, &nine, 170, 150, 200, 240, &smooth);
    nine_draw(p, &nine, 210, 150, 290, 170, &smooth);
    label(p, "nine pieces", 210, 200);
    /* Turned, with smoothed edges; a part that is not whole pixels. */
    a = save(p);
    translate(p, 380, 200);
    rotate(p, 20);
    bitmap(p, &photo, 0, 0, 64, 64, -48, -48, 48, 48, &smooth);
    restore(p, a);
    bitmap(p, &photo, 8.5f, 8.5f, 30.5f, 30.5f, 300, 270, 366, 336, &smooth);
    /* Tinted, as icons are: the picture's shape in a colour (SRC_IN), its colours darkened by one (MODULATE), a colour laid over it (SRC_ATOP), under it (DST_OVER). */
    for (i = 0; i < 4; i++) {
        static const int modes[4] = { MEGLER_GFX_BLEND_SRC_IN, MEGLER_GFX_BLEND_MODULATE, MEGLER_GFX_BLEND_SRC_ATOP, MEGLER_GFX_BLEND_DST_OVER };
        static const u32 colours[4] = { 0xFF2060C0u, 0xFFFFA040u, 0x80FF0000u, 0xFF208040u };
        tint = rough;
        tint.filter_mode = modes[i];
        tint.filter_color = colours[i];
        bitmap(p, &small, 0, 0, 4, 4, 10.0f + 68 * (float)i, 260, 70.0f + 68 * (float)i, 320, &tint);
    }
    label(p, "filters: SRC_IN, MODULATE, SRC_ATOP, DST_OVER", 10, 340);
    pic_done(&photo);
    pic_done(&nine);
    pic_done(&small);
}

static void check_bitmaps(pic *p)
{
    static const u32 colours[9] = { 0xFFC02020u, 0xFF20A020u, 0xFF2020C0u, 0xFFC0A020u, 0xFFF0F0F0u, 0xFF20A0C0u, 0xFFA020C0u, 0xFF606060u, 0xFF000000u };
    int x, y, k, mixed = 0, pieces[9];
    /* The stretched picture, smooth sampling and all: every pixel is one of the nine colours. A sample taken across a piece's edge would be a mixture. */
    memset(pieces, 0, sizeof(pieces));
    for (y = 150; y < 240; y++)
        for (x = 10; x < 160; x++) {
            for (k = 0; k < 9; k++)
                if (at(p, x, y) == colours[k]) break;
            if (k == 9) mixed++;
            else pieces[k]++;
        }
    say("    the nine pieces stretched to 150x90: %d pixels are none of the nine colours; the middle has %d, the corners %d %d %d %d", mixed, pieces[4], pieces[0], pieces[2],
        pieces[6], pieces[8]);
    CHECK(mixed == 0);
    CHECK(pieces[4] == 134 * 74 && pieces[0] == 64 && pieces[2] == 64 && pieces[6] == 64 && pieces[8] == 64 && pieces[1] == 134 * 8 && pieces[3] == 74 * 8);
    /* Its own size: pixel for pixel. Half as large again by the nearest pixel: every second pixel of the photo twice, none of them changed. */
    CHECK(at(p, 10 + 20, 10 + 40) == (0xFF000000u | (u32)(20 * 4) << 16 | (u32)(40 * 4) << 8 | (u32)(255 - 40 - 80)));
    CHECK(at(p, 190 + 31, 10 + 61) == at(p, 190 + 32, 10 + 62) && at(p, 190 + 31, 10 + 61) == (0xFF000000u | (u32)(21 * 4) << 16 | (u32)(41 * 4) << 8 | (u32)(255 - 42 - 82)));
    /* The tint by SRC_IN: opaque pixels are the filter's colour itself. */
    CHECK(at(p, 15, 265) == 0xFF2060C0u && at(p, 62, 280) == 0xFF2060C0u);
}

static void *linear(const megler_gfx_host *g, float x0, float y0, float x1, float y1, const u32 *colours, const float *positions, int n, int tile)
{
    return g->shader_linear(g->user, x0, y0, x1, y1, colours, positions, n, tile);
}

static void scene_shaders(pic *p)
{
    static const u32 two[2] = { 0xFFE02020u, 0xFF2040E0u }, three[3] = { 0xFFFFE000u, 0xFF20A040u, 0xFF200060u }, glass[3] = { 0xFF2060C0u, 0x002060C0u, 0xFFC02020u },
                     wheel[5] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFFFF0000u }, hard[4] = { 0xFF000000u, 0xFF000000u, 0xFFFFFFFFu, 0xFFFFFFFFu };
    static const float ends[2] = { 0, 1 }, thirds[3] = { 0, 0.3f, 1 }, quarters[5] = { 0, 0.25f, 0.5f, 0.75f, 1 }, steps[4] = { 0, 0.5f, 0.5f, 1 };
    static const char *const tiles[3] = { "clamp", "repeat", "mirror" };
    const megler_gfx_host *g = p->g;
    megler_gfx_paint paint = paint_of(0xFF000000u), s;
    megler_gfx_image image;
    void *sh;
    pic small;
    int i;
    float turn[6] = { 0.8f, -0.6f, 0, 0.6f, 0.8f, 0 };
    small_picture(&small, g);
    image_of(&small, &image);
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    for (i = 0; i < 3; i++) {
        float x = 10.0f + 118 * (float)i;
        /* Along a line from x + 30 to x + 70: what is before and after it is the tile mode's. */
        sh = linear(g, x + 30, 0, x + 70, 0, two, ends, 2, i);
        rect(p, x, 10, x + 108, 50, &paint, sh);
        g->shader_free(g->user, sh);
        /* By the distance from a centre: position 1 at 25. */
        sh = g->shader_radial(g->user, x + 54, 100, 25, three, thirds, 3, i);
        rect(p, x, 60, x + 108, 140, &paint, sh);
        g->shader_free(g->user, sh);
        /* A picture as the pattern, a pixel made six: its own matrix. */
        sh = g->shader_bitmap(g->user, &image, i);
        {
            float m[6] = { 6, 0, 0, 0, 6, 0 };
            m[2] = x + 42;
            m[5] = 180;
            g->shader_matrix(g->user, sh, m);
        }
        rect(p, x, 150, x + 108, 230, &paint, sh);
        g->shader_free(g->user, sh);
        label(p, tiles[i], x + 36, 244);
    }
    /* By the angle round a centre: red along +x, then green, blue, yellow, clockwise. */
    sh = g->shader_sweep(g->user, 420, 70, wheel, quarters, 5);
    oval(p, 365, 15, 475, 125, &paint, sh);
    g->shader_free(g->user, sh);
    label(p, "sweep", 405, 140);
    /* The same, squashed and turned by its own matrix; as a stroke. */
    sh = g->shader_sweep(g->user, 0, 0, wheel, quarters, 5);
    {
        float m[6] = { 1.2f, -0.9f, 420, 0.4f, 0.6f, 190 };
        g->shader_matrix(g->user, sh, m);
    }
    s = stroke_of(0xFF000000u, 14);
    oval(p, 380, 160, 460, 220, &s, sh);
    g->shader_free(g->user, sh);
    /* A gradient through nothing, over a chequered ground; a sudden step; one turned by its own matrix; one that moves with the canvas's. */
    chequer(p, 10, 260, 150, 300);
    sh = linear(g, 10, 0, 150, 0, glass, NULL, 3, MEGLER_GFX_TILE_CLAMP);
    rect(p, 10, 260, 150, 300, &paint, sh);
    g->shader_free(g->user, sh);
    sh = linear(g, 160, 0, 240, 0, hard, steps, 4, MEGLER_GFX_TILE_CLAMP);
    rect(p, 160, 260, 240, 300, &paint, sh);
    g->shader_free(g->user, sh);
    sh = linear(g, 0, 0, 20, 0, two, ends, 2, MEGLER_GFX_TILE_MIRROR);
    turn[2] = 250;
    turn[5] = 260;
    g->shader_matrix(g->user, sh, turn);
    round_rect(p, 250, 260, 350, 340, 16, 16, &paint, sh);
    {
        saved a = save(p);
        translate(p, 420, 300);
        rotate(p, 30);
        g->shader_matrix(g->user, sh, NULL);
        rect(p, -40, -30, 40, 30, &paint, sh);
        restore(p, a);
    }
    g->shader_free(g->user, sh);
    /* The paint's alpha multiplies a shader; and text is filled with one like any shape. */
    sh = linear(g, 10, 0, 150, 0, three, thirds, 3, MEGLER_GFX_TILE_CLAMP);
    paint.color = 0x60000000u;
    rect(p, 10, 310, 150, 322, &paint, sh);
    paint.color = 0xFF000000u;
    paint.text_size = 30;
    text(p, "Gradient", 10, 352, &paint, NULL);
    {
        u16 t[16];
        int n = units("Gradient", t, 16);
        text16(p, t, n, 10, 352, &paint, NULL, sh);
    }
    g->shader_free(g->user, sh);
    pic_done(&small);
}

static void check_shaders(pic *p)
{
    const u32 red = 0xFFE02020u, blue = 0xFF2040E0u;
    pic q;
    /*
     * Along the line from x + 30 (red) to x + 70 (blue), a pixel's place is that of its centre. Clamp: red before, blue after.
     * Repeat: it begins again after the end, and ends before the beginning. Mirror: it runs back from the end, and from the beginning.
     */
    CHECK(apart(at(p, 15, 30), red) <= 2 && apart(at(p, 110, 30), blue) <= 2 && apart(at(p, 60, 30), between(red, blue, 20.5 / 40)) <= 3);
    CHECK(apart(at(p, 128 + 72, 30), between(red, blue, 2.5 / 40)) <= 3 && apart(at(p, 128 + 68, 30), between(red, blue, 38.5 / 40)) <= 3 &&
          apart(at(p, 128 + 28, 30), between(red, blue, 38.5 / 40)) <= 3);
    CHECK(apart(at(p, 246 + 72, 30), between(red, blue, 37.5 / 40)) <= 3 && apart(at(p, 246 + 28, 30), between(red, blue, 1.5 / 40)) <= 3 &&
          apart(at(p, 246 + 104, 30), between(red, blue, 5.5 / 40)) <= 3);
    /* By the distance from (x + 54, 100): the first colour at the centre, the second at three tenths of 25, the last at 25 and (clamp) beyond. */
    CHECK(apart(at(p, 64, 100), between(0xFFFFE000u, 0xFF20A040u, 0.7071 / 7.5)) <= 4 && apart(at(p, 64 + 30, 100), 0xFF200060u) <= 2 && apart(at(p, 15, 65), 0xFF200060u) <= 2);
    CHECK(apart(at(p, 64 + 7, 100), between(0xFFFFE000u, 0xFF20A040u, 7.5 / 7.5)) <= 6 && apart(at(p, 64 + 16, 100), between(0xFF20A040u, 0xFF200060u, 9.0 / 17.5)) <= 6);
    /* By the angle: the colours' places are at three, six, nine and twelve o'clock, in that order clockwise on the screen. */
    CHECK(apart(at(p, 420, 70 + 40), 0xFF00FF00u) <= 8 && apart(at(p, 420 - 40, 70), 0xFF0000FFu) <= 8 && apart(at(p, 420, 70 - 40), 0xFFFFFF00u) <= 8);
    CHECK(apart(at(p, 420 + 40, 70 + 1), 0xFFFF0000u) <= 8 && apart(at(p, 420 + 40, 70 - 2), 0xFFFF0000u) <= 12);
    CHECK(apart(at(p, 420 + 28, 70 + 28), between(0xFFFF0000u, 0xFF00FF00u, 0.5)) <= 12);
    /* The pattern: the picture's top left pixel (red) is a block of six at its matrix's place; clamped, its edge pixels go on to the left and right. */
    CHECK(at(p, 10 + 42 + 2, 182) == 0xFFFF0000u && at(p, 10 + 42 + 8, 182) == 0xFF00FF00u && at(p, 12, 182) == 0xFFFF0000u && at(p, 10 + 100, 182) == 0xFFFFFF00u);
    /* Repeated: the same block again 24 pixels on. Mirrored: the picture turned round beside itself, so its fourth column comes again after its fourth. */
    CHECK(at(p, 128 + 42 + 2, 182) == 0xFFFF0000u && at(p, 128 + 42 + 26, 182) == 0xFFFF0000u && at(p, 128 + 42 - 22, 182) == 0xFFFF0000u);
    CHECK(at(p, 246 + 42 + 20, 182) == 0xFFFFFF00u && at(p, 246 + 42 + 26, 182) == 0xFFFFFF00u && at(p, 246 + 42 + 44, 182) == 0xFFFF0000u);
    /* The sudden step. */
    CHECK(at(p, 170, 280) == 0xFF000000u && at(p, 230, 280) == 0xFFFFFFFFu);
    /*
     * Between two stops the colour is interpolated as it stands and multiplied by its alpha afterwards: half way from opaque
     * red to green with no alpha is half red, half green, at half strength -- not red at half strength, which interpolating
     * the multiplied colours would give.
     */
    pic_make(&q, H, 64, 4);
    {
        static const u32 two[2] = { 0xFFFF0000u, 0x0000FF00u };
        static const float ends[2] = { 0, 1 };
        megler_gfx_paint paint = paint_of(0xFF000000u);
        void *sh = linear(H, 0, 0, 64, 0, two, ends, 2, MEGLER_GFX_TILE_CLAMP);
        rect(&q, 0, 0, 64, 4, &paint, sh);
        H->shader_free(H->user, sh);
        settle(&q);
        CHECK(apart(at(&q, 32, 2), premultiplied(between(two[0], two[1], 32.5 / 64))) <= 3);
        CHECK(apart(at(&q, 0, 2), premultiplied(between(two[0], two[1], 0.5 / 64))) <= 3 && apart(at(&q, 63, 2), premultiplied(between(two[0], two[1], 63.5 / 64))) <= 3);
        /* The paint's alpha multiplies a shader's colours. */
        color(&q, 0, MEGLER_GFX_BLEND_CLEAR);
        paint.color = 0x80000000u;
        sh = linear(H, 0, 0, 64, 0, two, ends, 2, MEGLER_GFX_TILE_CLAMP);
        rect(&q, 0, 0, 64, 4, &paint, sh);
        H->shader_free(H->user, sh);
        settle(&q);
        CHECK(apart(at(&q, 0, 2), faded(premultiplied(between(two[0], two[1], 0.5 / 64)), 128)) <= 3);
    }
    pic_done(&q);
}

/* Every transfer mode as a layer's content: one disc, and a square put on it by the mode, over a chequered ground. */
static void scene_blend_modes(pic *p)
{
    static const char *const names[18] = { "CLEAR", "SRC", "DST", "SRC_OVER", "DST_OVER", "SRC_IN", "DST_IN", "SRC_OUT", "DST_OUT",
                                           "SRC_ATOP", "DST_ATOP", "XOR", "PLUS", "MODULATE", "SCREEN", "OVERLAY", "DARKEN", "LIGHTEN" };
    static const u32 fade[2] = { 0xFFF0C020u, 0x40F04020u };
    static const float ends[2] = { 0, 1 };
    int i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    chequer(p, 0, 0, (float)p->w, (float)p->h);
    for (i = 0; i < 18; i++) {
        float x = 8.0f + 106 * (float)(i % 6), y = 8.0f + 116 * (float)(i / 6), bounds[4];
        megler_gfx_paint dst = paint_of(0xE02060C0u), src = paint_of(0xFF000000u);
        saved layer;
        void *sh;
        bounds[0] = x;
        bounds[1] = y;
        bounds[2] = x + 98;
        bounds[3] = y + 92;
        layer = save_layer(p, bounds, 255, MEGLER_GFX_BLEND_SRC_OVER);
        oval(p, x + 4, y + 4, x + 68, y + 68, &dst, NULL);
        sh = linear(p->g, x + 30, 0, x + 94, 0, fade, ends, 2, MEGLER_GFX_TILE_CLAMP);
        src.blend = i;
        /* (DST is never a paint's: the core does not make the call.) */
        if (i != MEGLER_GFX_BLEND_DST) rect(p, x + 30, y + 28, x + 94, y + 88, &src, sh);
        p->g->shader_free(p->g->user, sh);
        restore(p, layer);
        label(p, names[i], x + 4, y + 106);
    }
}

/* One channel of S on D by megler.h's table, in numbers between 0 and 1: written from the table, not from any implementation. */
static double table_channel(int mode, double s, double d, double sa, double da)
{
    double both = s * (1 - da) + d * (1 - sa);
    switch (mode) {
    case 0: return 0;
    case 1: return s;
    case 2: return d;
    case 3: return s + d * (1 - sa);
    case 4: return d + s * (1 - da);
    case 5: return s * da;
    case 6: return d * sa;
    case 7: return s * (1 - da);
    case 8: return d * (1 - sa);
    case 9: return s * da + d * (1 - sa);
    case 10: return d * sa + s * (1 - da);
    case 11: return both;
    case 12: return s + d > 1 ? 1 : s + d;
    case 13: return s * d;
    case 14: return s + d - s * d;
    case 15: return both + (2 * d <= da ? 2 * s * d : sa * da - 2 * (da - d) * (sa - s));
    case 16: return both + (s * da < d * sa ? s * da : d * sa);
    default: return both + (s * da > d * sa ? s * da : d * sa);
    }
}

static u32 table_pixel(int mode, u32 s, u32 d)
{
    double sa = (s >> 24) / 255.0, da = (d >> 24) / 255.0;
    u32 out = 0;
    int shift;
    for (shift = 0; shift < 32; shift += 8) {
        double v = table_channel(mode, ((s >> shift) & 255) / 255.0, ((d >> shift) & 255) / 255.0, sa, da);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        out |= (u32)(v * 255.0 + 0.5) << shift;
    }
    return out;
}

/* The eighteen modes on known colours, against megler.h's table: a shape in a colour, a bitmap, a shader, a layer, drawColor. */
static void check_blend_table(void)
{
    static const u32 pairs[4][2] = { { 0xA0F08020u, 0xC02060E0u }, { 0xFF3090F0u, 0x80C04010u }, { 0x40FFFFFFu, 0xFF102030u }, { 0xC0406080u, 0x00000000u } };
    static const float ends[2] = { 0, 1 };
    pic p, s;
    int mode, k, how, worst = 0, wrong = 0;
    say("  the eighteen transfer modes against megler.h's table, drawn seven ways each on four pairs of colours, and as colour filters");
    pic_make(&p, H, 8, 8);
    pic_make(&s, H, 2, 2);
    for (mode = 0; mode < 18; mode++)
        for (k = 0; k < 4; k++)
            for (how = 0; how < 8; how++) {
                u32 src = pairs[k][0], dst = pairs[k][1], want = table_pixel(mode, premultiplied(src), premultiplied(dst)), got;
                megler_gfx_paint paint = paint_of(src);
                int d;
                if (mode == MEGLER_GFX_BLEND_DST && how != 3 && how != 5 && how != 7) continue;
                if (how == 5) {
                    /* A colour filter by the mode, of the source's colour, on a shader that is the destination's colour all
                       along, drawn by SRC: each colour C of the shader becomes the mode of S = the filter's colour and D = C
                       (megler.h, THE PAINT), which is the table's pixel again. */
                    u32 two[2];
                    void *sh;
                    two[0] = two[1] = dst;
                    sh = linear(H, 0, 0, 8, 0, two, ends, 2, MEGLER_GFX_TILE_CLAMP);
                    color(&p, 0xFF808080u, MEGLER_GFX_BLEND_SRC);
                    paint = paint_of(0xFF000000u);
                    paint.blend = MEGLER_GFX_BLEND_SRC;
                    paint.filter_mode = mode;
                    paint.filter_color = src;
                    rect(&p, 2, 2, 6, 6, &paint, sh);
                    H->shader_free(H->user, sh);
                    settle(&p);
                    got = at(&p, 3, 3);
                    d = apart(got, want);
                    if (d > worst) worst = d;
                    if (d > 2) {
                        wrong++;
                        if (wrong <= 6) say("    filter by mode %d, %08x on %08x: %08x, the table says %08x", mode, src, dst, got, want);
                    }
                    if (at(&p, 0, 0) != 0xFF808080u || at(&p, 7, 7) != 0xFF808080u) wrong++;
                    continue;
                }
                color(&p, dst, MEGLER_GFX_BLEND_SRC);
                paint.blend = mode;
                if (how == 0) {
                    rect(&p, 2, 2, 6, 6, &paint, NULL);
                } else if (how == 1) {
                    /* A bitmap of the colour, the paint opaque. */
                    s.px[0] = s.px[1] = s.px[2] = s.px[3] = premultiplied(src);
                    wrote(&s);
                    paint.color = 0xFF000000u;
                    paint.flags = 0;
                    bitmap(&p, &s, 0, 0, 2, 2, 2, 2, 6, 6, &paint);
                } else if (how == 2) {
                    /* A gradient that is the colour all along, without its alpha, which is the paint's. */
                    u32 two[2];
                    void *sh;
                    two[0] = two[1] = src | 0xFF000000u;
                    sh = linear(H, 0, 0, 8, 0, two, ends, 2, MEGLER_GFX_TILE_CLAMP);
                    rect(&p, 2, 2, 6, 6, &paint, sh);
                    H->shader_free(H->user, sh);
                } else if (how == 3) {
                    /* A layer that holds the colour opaque and is put on at the colour's alpha by the mode. */
                    float bounds[4] = { 2, 2, 6, 6 };
                    saved layer = save_layer(&p, bounds, (int)(src >> 24), mode);
                    color(&p, src | 0xFF000000u, MEGLER_GFX_BLEND_SRC);
                    restore(&p, layer);
                    if (mode == MEGLER_GFX_BLEND_DST) want = premultiplied(dst);
                } else if (how == 4) {
                    saved clip = save(&p);
                    clip_rect(&p, 2, 2, 6, 6, MEGLER_GFX_CLIP_INTERSECT);
                    color(&p, src, mode);
                    restore(&p, clip);
                } else {
                    /* The last two again under a clip that is a path, whose edge is smoothed: a pixel well inside it is
                       covered whole, and gets the table's colour as well. */
                    static const float square[] = { MEGLER_GFX_PATH_MOVE, 2, 2, MEGLER_GFX_PATH_LINE, 6, 2, MEGLER_GFX_PATH_LINE, 6, 6, MEGLER_GFX_PATH_LINE, 2, 6,
                                                    MEGLER_GFX_PATH_CLOSE };
                    saved clip = save(&p);
                    clip_path(&p, square, (int)MG_COUNT(square), MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
                    if (how == 6) {
                        color(&p, src, mode);
                    } else {
                        saved layer = save_layer(&p, NULL, (int)(src >> 24), mode);
                        color(&p, src | 0xFF000000u, MEGLER_GFX_BLEND_SRC);
                        restore(&p, layer);
                        if (mode == MEGLER_GFX_BLEND_DST) want = premultiplied(dst);
                    }
                    restore(&p, clip);
                }
                settle(&p);
                got = at(&p, 3, 3);
                d = apart(got, want);
                if (d > worst) worst = d;
                if (d > 2) {
                    wrong++;
                    if (wrong <= 6) say("    mode %d, way %d, %08x on %08x: %08x, the table says %08x", mode, how, src, dst, got, want);
                }
                /* Outside the shape nothing changed, whatever the mode. */
                if (at(&p, 0, 0) != premultiplied(dst) || at(&p, 7, 7) != premultiplied(dst)) wrong++;
            }
    say("    the furthest any result is from the table: %d steps of 255", worst);
    CHECK(wrong == 0);
    CHECK(worst <= 2);
    pic_done(&s);
    pic_done(&p);
}

/* ---------------------------------------------------------------------- text */

static const u16 k_latin[] = { 'H', 'a', 'm', 'b', 'u', 'r', 'g', 'e', 'f', 'o', 'n', 's', 't', 'i', 'v', ' ', '0', '1', '2', '3' };
static const u16 k_cyrillic[] = { 0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442, ',', ' ', 0x043C, 0x0438, 0x0440, '!', ' ', 0x0416, 0x0449, 0x044F };
static const u16 k_cjk[] = { 0x65E5, 0x672C, 0x8A9E, ' ', 0x4E2D, 0x6587, ' ', 0xD55C, 0xAD6D, 0xC5B4 };
static const u16 k_emoji[] = { 'o', 'k', ' ', 0xD83D, 0xDE00, ' ', 0x2764, ' ', 0xD83D, 0xDC4D, ' ', 0xD83C, 0xDF0D };

static float measure(const u16 *t, int n, const megler_gfx_paint *paint, void *face) { return H->text_measure(H->user, t, n, paint, face); }

/* A run with what the measuring functions say about it drawn in: the baseline, the advance width as a bracket under it, the ink's box. */
static void text_marked(pic *p, const u16 *t, int n, float x, float y, const megler_gfx_paint *paint, void *face)
{
    megler_gfx_paint base = stroke_of(0xFFE04040u, 0), bracket = stroke_of(0xFF2040E0u, 0), ink = stroke_of(0x8020A040u, 0);
    float width = measure(t, n, paint, face), box[4] = { 0, 0, 0, 0 }, drop = paint->text_size * 0.3f + 3;
    base.flags = bracket.flags = ink.flags = 0;
    p->g->text_bounds(p->g->user, t, n, paint, face, box);
    line(p, x - 6, y + 0.5f, x + width + 6, y + 0.5f, &base);
    if (box[2] > box[0]) rect(p, x + box[0] - 0.5f, y + box[1] - 0.5f, x + box[2] + 0.5f, y + box[3] + 0.5f, &ink, NULL);
    text16(p, t, n, x, y, paint, face, NULL);
    line(p, x + 0.5f, y + drop - 3, x + 0.5f, y + drop, &bracket);
    line(p, x, y + drop + 0.5f, x + width, y + drop + 0.5f, &bracket);
    line(p, x + width - 0.5f, y + drop - 3, x + width - 0.5f, y + drop, &bracket);
}

static void *t_face_data;              /* a typeface out of a font file's bytes, when one could be read */
static char t_face_file[260];

static void scene_text(pic *p)
{
    static const float sizes[4] = { 12, 18, 28, 44 };
    const megler_gfx_host *g = p->g;
    megler_gfx_paint paint = paint_of(0xFF101010u);
    void *bold = g->typeface_derive(g->user, NULL, 700, false), *italic = g->typeface_derive(g->user, NULL, 400, true), *both_ways = g->typeface_derive(g->user, NULL, 700, true);
    void *serif = g->typeface_create(g->user, "serif", MEGLER_GFX_SERIF, 400, false), *mono = g->typeface_create(g->user, "monospace", MEGLER_GFX_MONOSPACE, 400, false);
    void *light = g->typeface_create(g->user, "sans-serif-light", MEGLER_GFX_SANS, 300, false);
    float y = 8;
    u16 t[96];
    int i, n;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    for (i = 0; i < 4; i++) {
        paint.text_size = sizes[i];
        y += sizes[i] * 1.5f + 4;
        text_marked(p, k_latin, (int)MG_COUNT(k_latin), 12, y, &paint, NULL);
    }
    paint.text_size = 24;
    y += 44;
    text_marked(p, k_cyrillic, (int)MG_COUNT(k_cyrillic), 12, y, &paint, NULL);
    y += 44;
    text_marked(p, k_cjk, (int)MG_COUNT(k_cjk), 12, y, &paint, NULL);
    text_marked(p, k_emoji, (int)MG_COUNT(k_emoji), 330, y, &paint, NULL);
    paint.text_size = 20;
    y += 40;
    n = units("Bold 700", t, 96);
    text_marked(p, t, n, 12, y, &paint, bold);
    n = units("Italic", t, 96);
    text_marked(p, t, n, 130, y, &paint, italic);
    n = units("Both", t, 96);
    text_marked(p, t, n, 215, y, &paint, both_ways);
    n = units("Light 300", t, 96);
    text_marked(p, t, n, 290, y, &paint, light);
    y += 38;
    n = units("Serif face", t, 96);
    text_marked(p, t, n, 12, y, &paint, serif);
    n = units("Monospace", t, 96);
    text_marked(p, t, n, 150, y, &paint, mono);
    if (t_face_data) {
        n = units("From a file's bytes", t, 96);
        text_marked(p, t, n, 330, y, &paint, t_face_data);
        y += 38;
        n = units("... made bold, slanted", t, 96);
        text_marked(p, t, n, 330, y, &paint, g->typeface_derive(g->user, t_face_data, 700, true));
    }
    /* Breaking: a sentence, the width it has to fit in (the grey bar), and where the two ways of breaking end the first line. */
    {
        megler_gfx_paint bar = paint_of(0xFFD0D0D0u), by_words = stroke_of(0xFF20A040u, 0), by_chars = stroke_of(0xFFE04040u, 0);
        float width = 250, x = 12;
        int w, c;
        y += 40;
        n = units("The quick brown fox jumps over the lazy dog", t, 96);
        paint.text_size = 18;
        bar.flags = by_words.flags = by_chars.flags = 0;
        rect(p, x, y + 8, x + width, y + 12, &bar, NULL);
        text16(p, t, n, x, y, &paint, NULL, NULL);
        w = g->text_break(g->user, t, n, width, &paint, NULL, true);
        c = g->text_break(g->user, t, n, width, &paint, NULL, false);
        line(p, x + measure(t, w, &paint, NULL) + 0.5f, y - 18, x + measure(t, w, &paint, NULL) + 0.5f, y + 6, &by_words);
        line(p, x + measure(t, c, &paint, NULL) + 0.5f, y - 18, x + measure(t, c, &paint, NULL) + 0.5f, y + 6, &by_chars);
        label(p, "green: where the line ends by words; red: by characters; grey: the width it has", x, y + 30);
    }
}

static void scene_text_styles(pic *p)
{
    static const u32 fade[3] = { 0xFFE02020u, 0xFFE0C020u, 0xFF2040E0u };
    const megler_gfx_host *g = p->g;
    megler_gfx_paint paint = paint_of(0xFF101010u), guide = stroke_of(0xFFE04040u, 0), other;
    void *sh;
    u16 t[64];
    int n, i;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    paint.text_size = 22;
    guide.flags = 0;
    /* The paint's three changes to a run: wider and narrower, slanted, spaced. */
    n = units("Scale 1.6", t, 64);
    other = paint;
    other.text_scale_x = 1.6f;
    text_marked(p, t, n, 12, 30, &other, NULL);
    n = units("Scale 0.6", t, 64);
    other.text_scale_x = 0.6f;
    text_marked(p, t, n, 190, 30, &other, NULL);
    n = units("Skew -0.25", t, 64);
    other = paint;
    other.text_skew_x = -0.25f;
    text_marked(p, t, n, 290, 30, &other, NULL);
    n = units("Spacing 0.2", t, 64);
    other = paint;
    other.letter_spacing = 0.2f;
    text_marked(p, t, n, 12, 72, &other, NULL);
    /* Thickened, outlined, both; filled by a shader. */
    n = units("Fake bold", t, 64);
    other = paint;
    other.flags |= MEGLER_GFX_FAKE_BOLD;
    text_marked(p, t, n, 200, 72, &other, NULL);
    n = units("plain", t, 64);
    text_marked(p, t, n, 330, 72, &paint, NULL);
    other = paint;
    other.text_size = 40;
    other.style = MEGLER_GFX_STROKE;
    other.stroke_width = 1.5f;
    n = units("Stroke", t, 64);
    text16(p, t, n, 12, 124, &other, NULL, NULL);
    other.style = MEGLER_GFX_FILL_AND_STROKE;
    other.stroke_width = 3;
    other.color = 0x80C02060u;
    n = units("Both", t, 64);
    text16(p, t, n, 150, 124, &other, NULL, NULL);
    other = paint;
    other.text_size = 40;
    sh = linear(g, 260, 0, 460, 0, fade, NULL, 3, MEGLER_GFX_TILE_CLAMP);
    n = units("Shader", t, 64);
    text16(p, t, n, 260, 124, &other, NULL, sh);
    g->shader_free(g->user, sh);
    /* Shadows: sharp and near, soft and far. */
    other = paint;
    other.text_size = 30;
    other.shadow_radius = 1;
    other.shadow_dx = 2;
    other.shadow_dy = 2;
    other.shadow_color = 0xFF808080u;
    n = units("Shadow 1", t, 64);
    text16(p, t, n, 12, 172, &other, NULL, NULL);
    other.shadow_radius = 6;
    other.shadow_dx = 0;
    other.shadow_dy = 4;
    other.shadow_color = 0xC02040E0u;
    other.color = 0xFFFFFFFFu;
    n = units("Shadow 6", t, 64);
    text16(p, t, n, 160, 172, &other, NULL, NULL);
    other = paint_of(0xFFE0A020u);
    other.shadow_radius = 5;
    other.shadow_dx = 5;
    other.shadow_dy = 5;
    other.shadow_color = 0xA0000000u;
    round_rect(p, 320, 145, 380, 180, 8, 8, &other, NULL);
    oval(p, 400, 140, 460, 185, &other, NULL);
    /* x is the run's start, its middle or its end. */
    line(p, 240.5f, 196, 240.5f, 262, &guide);
    for (i = 0; i < 3; i++) {
        static const char *const names[3] = { "left of x", "centred on x", "right of x" };
        other = paint;
        other.text_size = 16;
        other.text_align = i;
        n = units(names[i], t, 64);
        text16(p, t, n, 240, 212.0f + 20 * (float)i, &other, NULL, NULL);
    }
    /* Not smoothed; very small; turned; mirrored; one large letter. */
    other = paint;
    other.flags = 0;
    other.text_size = 16;
    n = units("Not smoothed 0123", t, 64);
    text16(p, t, n, 12, 290, &other, NULL, NULL);
    other = paint;
    other.text_size = 7;
    n = units("Seven pixels high: the quick brown fox", t, 64);
    text16(p, t, n, 12, 304, &other, NULL, NULL);
    {
        saved a = save(p);
        translate(p, 60, 340);
        rotate(p, -12);
        other = paint;
        other.text_size = 20;
        n = units("Turned", t, 64);
        text16(p, t, n, 0, 0, &other, NULL, NULL);
        restore(p, a);
        a = save(p);
        translate(p, 260, 340);
        scale(p, -1, 1);
        n = units("Mirrored", t, 64);
        text16(p, t, n, 0, 0, &other, NULL, NULL);
        restore(p, a);
    }
    other = paint_of(0xFF2060C0u);
    other.text_size = 150;
    n = units("g", t, 64);
    text_marked(p, t, n, 370, 320, &other, NULL);
}

/* The box of the pixels of a picture that are not white. False when all are. */
static bool ink_box(const pic *p, int *l, int *t, int *r, int *b)
{
    int x, y;
    bool any = false;
    for (y = 0; y < p->h; y++)
        for (x = 0; x < p->w; x++) {
            if (at(p, x, y) == 0xFFFFFFFFu) continue;
            if (!any || x < *l) *l = x;
            if (!any || x + 1 > *r) *r = x + 1;
            if (!any || y < *t) *t = y;
            if (!any || y + 1 > *b) *b = y + 1;
            any = true;
        }
    return any;
}

/* What the text functions answer, against each other and against what is drawn. */
static void check_text(void)
{
    static const u16 words[] = { 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', ' ', 'f', 'o', 'o' }, lines[] = { 'a', 'b', '\n', 'c', 'd' };
    static const u16 pair[] = { 'a', 0xD83D, 0xDE00, 'b' }, blank[] = { ' ', ' ', ' ' }, odd[] = { 'a', 0xD83D, 'b', '\t', 0x0007, 'c', 0xDE00 };
    static const u16 many[] = { 'T', 'h', 'e', ' ', 'q', 'u', 'i', 'c', 'k', ' ', 'b', 'r', 'o', 'w', 'n', ' ', 'f', 'o', 'x', ' ', 'j', 'u', 'm', 'p', 's' };
    megler_gfx_paint paint = paint_of(0xFF000000u), other;
    void *bold = H->typeface_derive(H->user, NULL, 700, false), *mono = H->typeface_create(H->user, "monospace", MEGLER_GFX_MONOSPACE, 400, false);
    void *faces[4];
    float m[5], widths[32], box[4], sum, w, spacing;
    int i, k, f, n, sizes_bad = 0, sums_bad = 0, breaks_bad = 0;
    say("  text: what the five measuring functions answer, against each other and against the pixels");
    faces[0] = NULL;
    faces[1] = bold;
    faces[2] = mono;
    faces[3] = t_face_data;
    CHECK(bold != NULL && mono != NULL);
    for (f = 0; f < 4; f++) {
        if (f == 3 && !t_face_data) continue;
        /* The face's measures: top and ascent above the baseline (negative), descent and bottom below, in that order; the line spacing is their sum. */
        paint.text_size = 20;
        memset(m, 0, sizeof(m));
        spacing = H->text_metrics(H->user, &paint, faces[f], m);
        CHECK(m[0] <= m[1] && m[1] < 0 && m[2] > 0 && m[3] >= m[2] && m[4] >= 0);
        CHECK_NEAR(spacing, -m[1] + m[2] + m[4], 1e-3);
        CHECK(-m[1] > 10 && -m[1] < 30 && m[2] > 2 && m[2] < 12);
        if (f == 0) say("    the default face at 20: top %.2f, ascent %.2f, descent %.2f, bottom %.2f, leading %.2f; a line is %.2f", m[0], m[1], m[2], m[3], m[4], spacing);
        {
            float twice[5];
            other = paint;
            other.text_size = 40;
            H->text_metrics(H->user, &other, faces[f], twice);
            CHECK_NEAR(twice[1], 2 * m[1], 1e-3);
            CHECK_NEAR(twice[3], 2 * m[3], 1e-3);
        }
        /* The advance width is the sum of the code units' advances, and grows with the size in proportion. */
        for (k = 0; k < 4; k++) {
            const u16 *t = k == 0 ? k_latin : k == 1 ? k_cyrillic : k == 2 ? k_cjk : k_emoji;
            n = k == 0 ? (int)MG_COUNT(k_latin) : k == 1 ? (int)MG_COUNT(k_cyrillic) : k == 2 ? (int)MG_COUNT(k_cjk) : (int)MG_COUNT(k_emoji);
            paint.text_size = 17;
            w = measure(t, n, &paint, faces[f]);
            H->text_widths(H->user, t, n, &paint, faces[f], widths);
            sum = 0;
            for (i = 0; i < n; i++) sum += widths[i];
            if (!(fabs(sum - w) <= 1e-3 * w) || !(w > 0)) sums_bad++;
            other = paint;
            other.text_size = 51;
            if (!(fabs(measure(t, n, &other, faces[f]) - 3 * w) <= 2e-3 * w)) sizes_bad++;
            /* A part of the run measures no more than the whole. */
            if (measure(t, n / 2, &paint, faces[f]) > w) sums_bad++;
        }
    }
    CHECK(sums_bad == 0);
    CHECK(sizes_bad == 0);
    /* Nothing measures nothing; the second half of a pair has no advance of its own, and the pair is one character. */
    paint.text_size = 20;
    CHECK(measure(k_latin, 0, &paint, NULL) == 0);
    H->text_widths(H->user, pair, 4, &paint, NULL, widths);
    CHECK(widths[0] > 0 && widths[1] > 0 && widths[2] == 0 && widths[3] > 0);
    CHECK(H->text_break(H->user, pair, 4, widths[0] + widths[1] * 0.5f, &paint, NULL, false) == 1);
    CHECK(H->text_break(H->user, pair, 4, widths[0] + widths[1] + 0.01f, &paint, NULL, false) == 3);
    /* A monospaced face gives every letter the same advance. */
    H->text_widths(H->user, k_latin, 15, &paint, mono, widths);
    for (i = 1, k = 0; i < 15; i++) k += fabs(widths[i] - widths[0]) > 1e-3;
    CHECK(k == 0);
    /* A bold face is wider than the regular one (Roboto's by little more than a hundredth); scale and spacing are in the answer. */
    w = measure(k_latin, 15, &paint, NULL);
    CHECK(measure(k_latin, 15, &paint, bold) > w * 1.005f);
    other = paint;
    other.text_scale_x = 2;
    CHECK_NEAR(measure(k_latin, 15, &other, NULL), 2 * w, 1e-3 * w);
    other = paint;
    other.letter_spacing = 0.25f;
    CHECK_NEAR(measure(k_latin, 15, &other, NULL), w + 15 * 0.25f * 20, 2e-3 * w);
    other.text_scale_x = 2;
    CHECK_NEAR(measure(k_latin, 15, &other, NULL), 2 * (w + 15 * 0.25f * 20), 4e-3 * w);
    /* Control characters, a tab and halves of pairs on their own: an answer, and no line break. */
    w = measure(odd, 7, &paint, NULL);
    CHECK(w > 0 && w < 20 * 7);
    CHECK(H->text_break(H->user, odd, 7, 1000, &paint, NULL, true) == 7);
    H->text_widths(H->user, odd, 7, &paint, NULL, widths);
    CHECK(widths[4] == 0 && widths[3] > 0);
    /* The ink's box: inside the advance but for the letters' bearings, above and a little below the baseline; nothing for spaces. */
    for (k = 0; k < 3; k++) {
        const u16 *t = k == 0 ? k_latin : k == 1 ? k_cyrillic : k_cjk;
        n = k == 0 ? 15 : k == 1 ? 11 : 3;
        w = measure(t, n, &paint, NULL);
        box[0] = box[1] = box[2] = box[3] = 9;
        H->text_bounds(H->user, t, n, &paint, NULL, box);
        CHECK(box[0] > -4 && box[0] < 5 && box[2] > w - 5 && box[2] < w + 4 && box[1] < -8 && box[1] > -24 && box[3] >= 0 && box[3] < 8);
    }
    H->text_bounds(H->user, blank, 3, &paint, NULL, box);
    CHECK(box[0] == 0 && box[1] == 0 && box[2] == 0 && box[3] == 0 && measure(blank, 3, &paint, NULL) > 0);
    /* ... and it is where the pixels are: the run drawn, smoothed, and the box of what is not white, to a pixel and a half. */
    {
        pic p;
        int l = 0, t = 0, r = 0, b = 0, way;
        pic_make(&p, H, 320, 120);
        for (way = 0; way < 5; way++) {
            float x = 60, y = 70, width;
            other = paint;
            other.text_size = 32;
            if (way == 1) other.text_scale_x = 1.5f;
            if (way == 2) other.text_skew_x = -0.25f;
            if (way == 3) other.text_align = MEGLER_GFX_ALIGN_CENTER;
            if (way == 4) other.text_align = MEGLER_GFX_ALIGN_RIGHT;
            if (way >= 3) x = 160;
            color(&p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
            text16(&p, k_latin, 9, x, y, &other, NULL, NULL);
            settle(&p);
            H->text_bounds(H->user, k_latin, 9, &other, NULL, box);
            width = measure(k_latin, 9, &other, NULL);
            if (way == 3) x -= width / 2;
            if (way == 4) x -= width;
            CHECK(ink_box(&p, &l, &t, &r, &b));
            if (fabs(l - (x + box[0])) > 1.5 || fabs(r - (x + box[2])) > 1.5 || fabs(t - (y + box[1])) > 1.5 || fabs(b - (y + box[3])) > 1.5)
                say("    way %d: the ink is at %d..%d by %d..%d, text_bounds says %.1f..%.1f by %.1f..%.1f", way, l, r, t, b, x + box[0], x + box[2], y + box[1], y + box[3]);
            CHECK(fabs(l - (x + box[0])) <= 1.5 && fabs(r - (x + box[2])) <= 1.5 && fabs(t - (y + box[1])) <= 1.5 && fabs(b - (y + box[3])) <= 1.5);
        }
        /* Not smoothed: every pixel is the colour or the ground. */
        other = paint;
        other.flags = 0;
        other.text_size = 24;
        color(&p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        text16(&p, k_latin, 9, 10, 60, &other, NULL, NULL);
        settle(&p);
        for (i = 0, k = 0, n = 0; i < p.w * p.h; i++) {
            k += p.px[i] != 0xFFFFFFFFu && p.px[i] != 0xFF000000u;
            n += p.px[i] == 0xFF000000u;
        }
        CHECK(k == 0 && n > 200);
        pic_done(&p);
    }
    /*
     * A glyph drawn large under a small scale is there too, where text_bounds says, scaled: a letter 224 px high under a
     * scale of 0.46, as Spotify's play button on its now-playing screen draws the glyph of its icon font (64 dp at 560 dpi,
     * in a drawable fitted to the button), in the default face and in the one from a file's bytes. On the phone that glyph
     * was not drawn, where the same font's glyphs of up to 32 dp, unscaled, were.
     */
    {
        static const u16 letter[] = { 'H' };
        pic p;
        int l = 0, t = 0, r = 0, b = 0;
        pic_make(&p, H, 160, 160);
        for (f = 0; f < 2; f++) {
            void *face = f ? t_face_data : NULL;
            const float s = 0.46f, x = 40, y = 280;
            saved a;
            if (f && !face) continue;
            other = paint;
            other.text_size = 224;
            color(&p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
            a = save(&p);
            scale(&p, s, s);
            text16(&p, letter, 1, x, y, &other, face, NULL);
            restore(&p, a);
            settle(&p);
            H->text_bounds(H->user, letter, 1, &other, face, box);
            k = ink_box(&p, &l, &t, &r, &b);
            if (!k || fabs(l - s * (x + box[0])) > 2 || fabs(r - s * (x + box[2])) > 2 || fabs(t - s * (y + box[1])) > 2 || fabs(b - s * (y + box[3])) > 2)
                say("    a letter of 224 px under a scale of 0.46 (%s): %s; text_bounds says %.1f..%.1f by %.1f..%.1f", f ? "the face from bytes" : "the default face",
                    k ? "its ink is elsewhere" : "no ink at all", s * (x + box[0]), s * (x + box[2]), s * (y + box[1]), s * (y + box[3]));
            CHECK(k && fabs(l - s * (x + box[0])) <= 2 && fabs(r - s * (x + box[2])) <= 2 && fabs(t - s * (y + box[1])) <= 2 && fabs(b - s * (y + box[3])) <= 2);
        }
        pic_done(&p);
    }
    /* Breaking never gives more than fits, and gives all there is room for. */
    paint.text_size = 15;
    for (i = 0; i < 60; i++) {
        float room = 4.0f * (float)i, used;
        int by_chars = H->text_break(H->user, many, (int)MG_COUNT(many), room, &paint, NULL, false);
        int by_words = H->text_break(H->user, many, (int)MG_COUNT(many), room, &paint, NULL, true), kept;
        n = (int)MG_COUNT(many);
        used = measure(many, by_chars, &paint, NULL);
        if (by_chars < 0 || by_chars > n || used > room + 1e-3f) breaks_bad++;
        if (by_chars < n && measure(many, by_chars + 1, &paint, NULL) <= room) breaks_bad++;
        /* By words: no more than by characters but for spaces at the end, which need no room; and it ends where a word does. */
        kept = by_words;
        while (kept > 0 && many[kept - 1] == ' ') kept--;
        if (by_words < 0 || by_words > n || measure(many, kept, &paint, NULL) > room + 1e-3f) breaks_bad++;
        if (by_words < n && by_words > 0 && by_words != by_chars && many[by_words - 1] != ' ') breaks_bad++;
    }
    CHECK(breaks_bad == 0);
    paint.text_size = 10;
    w = measure(words, 6, &paint, NULL);
    /* "hello " fits where "hello" does; the next word needs its own room; a word too long for the line is cut where the line is full. */
    CHECK(H->text_break(H->user, words, 15, measure(words, 5, &paint, NULL) + 0.01f, &paint, NULL, true) == 6);
    CHECK(H->text_break(H->user, words, 15, measure(words, 11, &paint, NULL) - 0.5f, &paint, NULL, true) == 6);
    CHECK(H->text_break(H->user, words, 15, measure(words, 11, &paint, NULL) + 0.01f, &paint, NULL, true) == 12);
    CHECK(H->text_break(H->user, words, 15, 1000, &paint, NULL, true) == 15 && H->text_break(H->user, words, 15, 1000, &paint, NULL, false) == 15);
    CHECK(H->text_break(H->user, words, 15, measure(words, 3, &paint, NULL) + 0.01f, &paint, NULL, true) == 3);
    CHECK(H->text_break(H->user, words, 15, 0.5f, &paint, NULL, false) == 0);
    /* A line feed ends the line and belongs to it. */
    CHECK(H->text_break(H->user, lines, 5, 1000, &paint, NULL, true) == 3 && H->text_break(H->user, lines, 5, 1000, &paint, NULL, false) == 3);
    (void)w;
    /* Bytes that are no font give no face. */
    CHECK(H->typeface_from_data(H->user, "this is not a font, whatever it says", 36) == NULL);
}

/* --------------------------------------------------------------- image files */

/*
 * A GIF of 8 by 8 pixels in four colours, the third of them transparent,
 * written out here: a header, a palette, a graphic control block that names
 * the transparent colour, and the pixels as the LZW codes of 3 bits that
 * need no table -- a clear code, then two pixels, over and over -- so that
 * what a decoder must give back is known exactly. Pixel (x, y) is colour
 * (x / 2 + y / 2) % 4. Returns the file's size.
 */
static const u32 k_gif_colours[4] = { 0xFFE02020u, 0xFF20C040u, 0x00000000u, 0xFF2040E0u };

static size_t gif_make(u8 *out)
{
    static const u8 head[] = { 'G', 'I', 'F', '8', '9', 'a', 8, 0, 8, 0, 0x91, 0, 0 };
    u8 codes[128], packed[64];
    u32 bits = 0;
    int ncodes = 0, nbits = 0, npacked = 0, x, y, i;
    size_t n = 0;
    memcpy(out, head, sizeof(head));
    n += sizeof(head);
    for (i = 0; i < 4; i++) {
        out[n++] = (u8)(k_gif_colours[i] >> 16);
        out[n++] = (u8)(k_gif_colours[i] >> 8);
        out[n++] = (u8)k_gif_colours[i];
    }
    /* The graphic control extension: colour 2 is transparent. */
    out[n++] = 0x21;
    out[n++] = 0xF9;
    out[n++] = 4;
    out[n++] = 1;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 2;
    out[n++] = 0;
    /* The image: at 0, 0, 8 by 8, no palette of its own, codes of two bits to begin with. */
    out[n++] = 0x2C;
    for (i = 0; i < 4; i++) out[n++] = 0;
    out[n++] = 8;
    out[n++] = 0;
    out[n++] = 8;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 2;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++) {
            if (((y * 8 + x) & 1) == 0) codes[ncodes++] = 4;
            codes[ncodes++] = (u8)((x / 2 + y / 2) % 4);
        }
    codes[ncodes++] = 5;
    for (i = 0; i < ncodes; i++) {
        bits |= (u32)codes[i] << nbits;
        nbits += 3;
        while (nbits >= 8) {
            packed[npacked++] = (u8)bits;
            bits >>= 8;
            nbits -= 8;
        }
    }
    if (nbits) packed[npacked++] = (u8)bits;
    out[n++] = (u8)npacked;
    memcpy(out + n, packed, (size_t)npacked);
    n += (size_t)npacked;
    out[n++] = 0;
    out[n++] = 0x3B;
    return n;
}

/* A picture decoded from bytes, as BitmapFactory has the core do it: its size asked first, then all of it at that size over `sample`. */
static bool decode(const void *data, size_t size, int sample, pic *out)
{
    int w = 0, h = 0;
    memset(out, 0, sizeof(*out));
    if (!H->image_size(H->user, data, size, &w, &h) || w <= 0 || h <= 0) return false;
    w = w / sample > 0 ? w / sample : 1;
    h = h / sample > 0 ? h / sample : 1;
    pic_make(out, H, w, h);
    if (!H->image_decode(H->user, data, size, out->px, w, h, w * 4)) {
        pic_done(out);
        return false;
    }
    wrote(out);
    return true;
}

static bytes t_png, t_jpeg_good, t_jpeg_poor;
static u8 t_gif[160];
static size_t t_gif_size;
static u8 *t_jpeg_file;
static size_t t_jpeg_file_size;
static char t_jpeg_name[260];

/* The pictures the image scene shows, made first: a scene as a PNG (by the stand-in's encoder) and as two JPEGs (by the host's), the GIF, and a JPEG of this machine's. */
static void images_make(void)
{
    static const char *const known[] = { "C:/Windows/Web/Wallpaper/Windows/img0.jpg", "C:/Windows/Web/Screen/img100.jpg", "C:/Windows/Web/Wallpaper/Theme1/img1.jpg" };
    pic p;
    size_t i;
    photo_picture(&p, H);
    memset(&t_png, 0, sizeof(t_png));
    memset(&t_jpeg_good, 0, sizeof(t_jpeg_good));
    memset(&t_jpeg_poor, 0, sizeof(t_jpeg_poor));
    S->image_encode(S->user, p.px, p.w, p.h, p.w * 4, MEGLER_GFX_PNG, 100, bytes_write, &t_png);
    H->image_encode(H->user, p.px, p.w, p.h, p.w * 4, MEGLER_GFX_JPEG, 95, bytes_write, &t_jpeg_good);
    H->image_encode(H->user, p.px, p.w, p.h, p.w * 4, MEGLER_GFX_JPEG, 5, bytes_write, &t_jpeg_poor);
    pic_done(&p);
    t_gif_size = gif_make(t_gif);
    t_jpeg_file = NULL;
    if (t_config->jpeg_file) {
        t_jpeg_file = mg_file_read_all(t_config->jpeg_file, &t_jpeg_file_size);
        mg_strlcpy(t_jpeg_name, t_config->jpeg_file, sizeof(t_jpeg_name));
    }
    for (i = 0; !t_jpeg_file && !t_config->jpeg_file && i < MG_COUNT(known); i++) {
        t_jpeg_file = mg_file_read_all(known[i], &t_jpeg_file_size);
        mg_strlcpy(t_jpeg_name, known[i], sizeof(t_jpeg_name));
    }
}

static void scene_images(pic *p)
{
    megler_gfx_paint smooth = paint_of(0xFF000000u), rough = paint_of(0xFF000000u);
    pic d;
    smooth.flags = MEGLER_GFX_ANTI_ALIAS | MEGLER_GFX_FILTER_BITMAP;
    rough.flags = 0;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    chequer(p, 330, 10, 460, 140);
    if (decode(t_png.data, t_png.size, 1, &d)) {
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 10, 10, 138, 138, &rough);
        pic_done(&d);
    }
    label(p, "a PNG", 10, 152);
    if (decode(t_jpeg_good.data, t_jpeg_good.size, 1, &d)) {
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 148, 10, 276, 138, &rough);
        pic_done(&d);
    }
    label(p, "as a JPEG, quality 95", 148, 152);
    if (decode(t_jpeg_poor.data, t_jpeg_poor.size, 1, &d)) {
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 10, 170, 138, 298, &rough);
        pic_done(&d);
    }
    label(p, "quality 5", 10, 312);
    if (decode(t_gif, t_gif_size, 1, &d)) {
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 340, 20, 450, 130, &rough);
        pic_done(&d);
    }
    label(p, "a GIF with a transparent colour", 300, 152);
    /* The PNG at half and at a quarter of its size, which is what inSampleSize asks for. */
    if (decode(t_png.data, t_png.size, 2, &d)) {
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 148, 170, 148 + (float)d.w, 170 + (float)d.h, &rough);
        pic_done(&d);
    }
    if (decode(t_png.data, t_png.size, 4, &d)) {
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 190, 170, 190 + (float)d.w, 170 + (float)d.h, &rough);
        pic_done(&d);
    }
    label(p, "1/2, 1/4", 148, 218);
    if (t_jpeg_file && decode(t_jpeg_file, t_jpeg_file_size, 8, &d)) {
        float w = 200, h = 200.0f * (float)d.h / (float)d.w;
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 270, 170, 270 + w, 170 + h, &smooth);
        label(p, "a JPEG of this machine, decoded at 1/8", 270, 170 + h + 14);
        pic_done(&d);
    } else {
        label(p, "(no JPEG of this machine's was found)", 270, 190);
    }
}

static void check_images(void)
{
    pic p, back;
    bytes file;
    int w = 0, h = 0, i, worst = 0, sample;
    say("  image files: PNG, JPEG and GIF read; PNG and JPEG written");
    /* A picture with every kind of pixel in it: opaque, partly transparent, empty. */
    pic_make(&p, H, 37, 23);
    for (i = 0; i < p.w * p.h; i++) {
        u32 a = (u32)(i * 7) % 256, c = (u32)i * 2654435761u;
        if (i % 5 == 0) a = 255;
        if (i % 11 == 0) a = 0;
        p.px[i] = premultiplied(a << 24 | (c & 0xFFFFFF));
    }
    wrote(&p);
    /* Written by the host and read by the host: what it was. */
    memset(&file, 0, sizeof(file));
    CHECK(H->image_encode(H->user, p.px, p.w, p.h, p.w * 4, MEGLER_GFX_PNG, 100, bytes_write, &file));
    CHECK(file.size > 8 && memcmp(file.data, "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(H->image_size(H->user, file.data, file.size, &w, &h) && w == 37 && h == 23);
    CHECK(decode(file.data, file.size, 1, &back));
    if (back.px) {
        CHECK(memcmp(back.px, p.px, sizeof(u32) * (size_t)(p.w * p.h)) == 0);
        pic_done(&back);
    }
    /* Written by the host and read by the stand-in, whose reader is another one. */
    {
        u32 *other = (u32 *)mg_alloc(sizeof(u32) * (size_t)(p.w * p.h));
        CHECK(S->image_decode(S->user, file.data, file.size, other, p.w, p.h, p.w * 4));
        CHECK(memcmp(other, p.px, sizeof(u32) * (size_t)(p.w * p.h)) == 0);
        free(other);
    }
    free(file.data);
    /* Written by the stand-in and read by the host. */
    memset(&file, 0, sizeof(file));
    CHECK(S->image_encode(S->user, p.px, p.w, p.h, p.w * 4, MEGLER_GFX_PNG, 100, bytes_write, &file));
    CHECK(decode(file.data, file.size, 1, &back));
    if (back.px) {
        CHECK(memcmp(back.px, p.px, sizeof(u32) * (size_t)(p.w * p.h)) == 0);
        pic_done(&back);
    }
    /* A few bytes of it are no picture; neither is nothing, nor bytes that are something else. */
    CHECK(!H->image_size(H->user, file.data, 6, &w, &h));
    CHECK(!H->image_size(H->user, "this is not a picture at all, however long it is", 48, &w, &h));
    CHECK(!H->image_size(H->user, NULL, 0, &w, &h));
    free(file.data);
    pic_done(&p);
    /* A JPEG has no alpha: written from an opaque picture and read back it is that picture, nearly; and smaller by far at low quality. */
    photo_picture(&p, H);
    CHECK(t_jpeg_good.size > 4 && t_jpeg_good.data[0] == 0xFF && t_jpeg_good.data[1] == 0xD8);
    CHECK(H->image_size(H->user, t_jpeg_good.data, t_jpeg_good.size, &w, &h) && w == 64 && h == 64);
    CHECK(decode(t_jpeg_good.data, t_jpeg_good.size, 1, &back));
    if (back.px) {
        long total = 0;
        for (i = 0; i < 64 * 64; i++) {
            total += apart(back.px[i], p.px[i]);
            if ((back.px[i] >> 24) != 255) worst = 256;
        }
        say("    a JPEG at quality 95: %u bytes; a pixel is %.1f steps from the original on average. At quality 5: %u bytes", (unsigned)t_jpeg_good.size,
            (double)total / (64 * 64), (unsigned)t_jpeg_poor.size);
        CHECK(worst < 256 && total / (64 * 64) < 12);
        pic_done(&back);
    }
    CHECK(t_jpeg_poor.size > 0 && t_jpeg_poor.size * 2 < t_jpeg_good.size);
    /* A partly transparent picture as a JPEG: the alpha is dropped and the colours go as they are in memory, which is the picture over black. */
    {
        u32 half[64];
        for (i = 0; i < 64; i++) half[i] = premultiplied(0x80FF8040u);
        memset(&file, 0, sizeof(file));
        CHECK(H->image_encode(H->user, half, 8, 8, 32, MEGLER_GFX_JPEG, 100, bytes_write, &file));
        if (decode(file.data, file.size, 1, &back)) {
            CHECK(apart(back.px[27], 0xFF804020u) <= 4);
            pic_done(&back);
        }
        free(file.data);
    }
    pic_done(&p);
    /* The GIF: its first frame, the transparent colour as no pixel at all. */
    CHECK(H->image_size(H->user, t_gif, t_gif_size, &w, &h) && w == 8 && h == 8);
    CHECK(decode(t_gif, t_gif_size, 1, &back));
    if (back.px) {
        int wrong = 0, x, y;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) wrong += back.px[y * 8 + x] != k_gif_colours[(x / 2 + y / 2) % 4];
        CHECK(wrong == 0);
        pic_done(&back);
    }
    /* At a half, a quarter, an eighth of its size (inSampleSize): the right size, and still the picture -- a place in it is the colour of that place in the original. */
    for (sample = 2; sample <= 8; sample *= 2) {
        CHECK(decode(t_png.data, t_png.size, sample, &back));
        if (back.px) {
            CHECK(back.w == 64 / sample && back.h == 64 / sample);
            CHECK(apart(at(&back, back.w / 4, back.h * 3 / 4), 0xFF000000u | (u32)(16 * 4) << 16 | (u32)(48 * 4) << 8 | (u32)(255 - 32 - 96)) <= 6 * sample);
            pic_done(&back);
        }
    }
    /* Reduced with alpha: the colours of pixels that do not show must not come into those that do. Half red, half nothing: red stays red. */
    {
        u32 striped[64], small[16];
        for (i = 0; i < 64; i++) striped[i] = (i % 8) < 4 ? 0xFFFF0000u : 0x00000000u;
        memset(&file, 0, sizeof(file));
        S->image_encode(S->user, striped, 8, 8, 32, MEGLER_GFX_PNG, 100, bytes_write, &file);
        CHECK(H->image_decode(H->user, file.data, file.size, small, 4, 4, 16));
        CHECK(small[0] == 0xFFFF0000u && small[3] == 0 && small[13] == 0xFFFF0000u);
        /* An odd reduction mixes the two: what is left of the red is red at less strength, never a darker red (the colour stays multiplied by its alpha). */
        CHECK(H->image_decode(H->user, file.data, file.size, small, 3, 3, 12));
        for (i = 0; i < 9; i++) CHECK(channel(small[i], 16) == channel(small[i], 24) && channel(small[i], 8) == 0 && channel(small[i], 0) == 0);
        free(file.data);
    }
    if (t_jpeg_file) {
        CHECK(H->image_size(H->user, t_jpeg_file, t_jpeg_file_size, &w, &h));
        say("    %s: %d x %d, %u KB; decoded at an eighth of that", t_jpeg_name, w, h, (unsigned)(t_jpeg_file_size >> 10));
        CHECK(decode(t_jpeg_file, t_jpeg_file_size, 8, &back));
        if (back.px) {
            CHECK(back.w == w / 8 && back.h == h / 8 && (at(&back, back.w / 2, back.h / 2) >> 24) == 255);
            pic_done(&back);
        }
    } else {
        say("    (no JPEG of this machine's was found: only the JPEGs written here were read)");
    }
}

/* ----------------------------------------------------- shadows and colour filters */

static void scene_shadows_filters(pic *p)
{
    static const u32 fade[2] = { 0xFFFFFFFFu, 0xFF000000u };
    const megler_gfx_host *g = p->g;
    megler_gfx_paint shape = paint_of(0xFFF0F0F0u), paint;
    pic small, photo;
    void *sh;
    int i;
    small_picture(&small, g);
    photo_picture(&photo, g);
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    fill(p, 0, 0, 480, 150, 0xFFD8E0E8u);
    /* A card: a white rounded rectangle with a soft shadow under it, as buttons and dialogs have. */
    for (i = 0; i < 3; i++) {
        static const float radii[3] = { 2, 6, 14 };
        shape.shadow_radius = radii[i];
        shape.shadow_dx = 0;
        shape.shadow_dy = radii[i] / 2;
        shape.shadow_color = 0x80000000u;
        round_rect(p, 20.0f + 150 * (float)i, 25, 140.0f + 150 * (float)i, 105, 8, 8, &shape, NULL);
    }
    label(p, "shadows of radius 2, 6, 14", 20, 138);
    /* A shadow keeps to the clip, and a stroke throws one too. */
    {
        saved a = save(p);
        clip_rect(p, 20, 165, 150, 235, MEGLER_GFX_CLIP_INTERSECT);
        paint = paint_of(0xFF2060C0u);
        paint.shadow_radius = 8;
        paint.shadow_dx = 10;
        paint.shadow_dy = 10;
        paint.shadow_color = 0xFFC02020u;
        oval(p, 40, 170, 120, 220, &paint, NULL);
        restore(p, a);
        paint = stroke_of(0xFF000000u, 4);
        paint.shadow_radius = 3;
        paint.shadow_dx = 4;
        paint.shadow_dy = 4;
        paint.shadow_color = 0xA0208040u;
        oval(p, 170, 170, 250, 230, &paint, NULL);
    }
    /* Colour filters on a shader: a gradient from white to black as the strength of a colour (SRC_IN keeps the filter's colour at the gradient's alpha -- all of it; MODULATE darkens it). */
    sh = linear(g, 280, 0, 460, 0, fade, NULL, 2, MEGLER_GFX_TILE_CLAMP);
    paint = paint_of(0xFF000000u);
    rect(p, 280, 165, 460, 185, &paint, sh);
    paint.filter_mode = MEGLER_GFX_BLEND_MODULATE;
    paint.filter_color = 0xFFE08020u;
    rect(p, 280, 190, 460, 210, &paint, sh);
    paint.filter_mode = MEGLER_GFX_BLEND_SCREEN;
    paint.filter_color = 0xFF2040C0u;
    rect(p, 280, 215, 460, 235, &paint, sh);
    g->shader_free(g->user, sh);
    label(p, "a gradient; MODULATE, SCREEN filters", 280, 250);
    /* A photo with each of several filters. */
    for (i = 0; i < 6; i++) {
        static const int modes[6] = { -1, MEGLER_GFX_BLEND_MODULATE, MEGLER_GFX_BLEND_SCREEN, MEGLER_GFX_BLEND_SRC_ATOP, MEGLER_GFX_BLEND_DARKEN, MEGLER_GFX_BLEND_OVERLAY };
        static const u32 colours[6] = { 0, 0xFFFFC080u, 0xFF203060u, 0x8020C040u, 0xFF808080u, 0xFFC04040u };
        paint = paint_of(0xFF000000u);
        paint.flags |= MEGLER_GFX_FILTER_BITMAP;
        paint.filter_mode = modes[i];
        paint.filter_color = colours[i];
        bitmap(p, &photo, 0, 0, 64, 64, 14.0f + 78 * (float)i, 265, 78.0f + 78 * (float)i, 329, &paint);
    }
    label(p, "none, MODULATE, SCREEN, SRC_ATOP, DARKEN, OVERLAY", 14, 346);
    pic_done(&small);
    pic_done(&photo);
}

/*
 * What a view with RenderEffect.createBlurEffect draws (save_layer_blur, radius 20: a standard deviation of 12 pixels). With
 * the edge DECAL a red square inside the layer comes out soft on every side, its colour reaching a little past its edges and
 * not far, nothing beyond the layer's bounds; a square filling the layer stays solid to the layer's edge with the edge
 * CLAMP, and fades there with DECAL.
 */
static void scene_layer_blur(pic *p)
{
    float bounds[4] = { 20, 20, 160, 160 };
    saved a;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    a = save_layer_blur(p, bounds, 20, false);
    fill(p, 60, 60, 120, 120, 0xFFE02020u);
    restore(p, a);
    bounds[0] = 200;
    bounds[2] = 300;
    a = save_layer_blur(p, bounds, 20, true);
    fill(p, 200, 20, 300, 160, 0xFFE02020u);
    restore(p, a);
    bounds[0] = 340;
    bounds[2] = 440;
    a = save_layer_blur(p, bounds, 20, false);
    fill(p, 340, 20, 440, 160, 0xFFE02020u);
    restore(p, a);
    label(p, "blurred layers: a square (decal); filled, clamped and decal", 20, 200);
}

static void check_layer_blur(pic *p)
{
    const u32 centre = at(p, 90, 90), inside = at(p, 61, 90), out = at(p, 126, 90), far = at(p, 152, 90), beyond = at(p, 175, 90);
    if (!p->g->save_layer_blur) return;
    /* (Green rises from the red's 0x20 to the ground's 0xFF as less of the red is there.) */
    CHECK(channel(centre, 16) >= 0xC8 && channel(centre, 8) <= 0x50);
    CHECK(channel(inside, 8) > channel(centre, 8) + 0x30);
    CHECK(channel(out, 8) < 0xF0 && channel(out, 16) >= 0xE0);
    CHECK(apart(far, 0xFFFFFFFFu) < 24);
    CHECK(beyond == 0xFFFFFFFFu);
    /* The filled layers' left edges: solid where the edge is clamped, faded where it is not. */
    CHECK(channel(at(p, 201, 90), 8) <= 0x50 && channel(at(p, 341, 90), 8) >= 0x60);
}

static void check_shadows(pic *p)
{
    /* Under the cards the ground is darker than beside them, and darker close to the card than further down. */
    CHECK(channel(at(p, 380, 109), 8) < channel(at(p, 380, 145), 8) && channel(at(p, 380, 145), 8) <= 0xE0);
    CHECK(at(p, 5, 5) == 0xFFD8E0E8u && at(p, 80, 60) == 0xFFF0F0F0u);
    /* The red shadow, ten pixels down and right of the blue oval, stops at the clip. */
    CHECK(channel(at(p, 110, 224), 16) > channel(at(p, 110, 224), 0) + 40 && at(p, 152, 228) == 0xFFFFFFFFu && at(p, 125, 238) == 0xFFFFFFFFu);
    /* The filtered gradient: MODULATE of white is the filter's colour, of black is black; SCREEN of black is the filter's colour, of white white. */
    CHECK(apart(at(p, 282, 200), 0xFFE08020u) <= 6 && apart(at(p, 458, 200), 0xFF000000u) <= 6);
    CHECK(apart(at(p, 458, 225), 0xFF2040C0u) <= 6 && apart(at(p, 282, 225), 0xFFFFFFFFu) <= 6);
}

/* ------------------------------------------- the memory, and several canvases */

static void check_memory(void)
{
    pic a, b;
    megler_gfx_paint half = paint_of(0x80FF0000u);
    void *second;
    int i;
    say("  the memory: row and channel order, flush and touched, two canvases on one bitmap, a canvas on memory with a picture in it");
    /* Rows and channels: red in the top left, green top right, blue bottom left; in memory a pixel is blue, green, red, alpha. */
    pic_make(&a, H, 64, 48);
    scene_corners(&a);
    settle(&a);
    CHECK_HEX(a.px[0], 0xFFFF0000u);
    CHECK_HEX(a.px[63], 0xFF00FF00u);
    CHECK_HEX(a.px[47 * 64], 0xFF0000FFu);
    CHECK_HEX(a.px[47 * 64 + 63], 0x80808080u);
    CHECK(((const u8 *)a.px)[0] == 0 && ((const u8 *)a.px)[1] == 0 && ((const u8 *)a.px)[2] == 255 && ((const u8 *)a.px)[3] == 255);
    CHECK_HEX(a.px[24 * 64 + 32], 0xFF808080u);
    /* Half transparent red on white: the colour is multiplied by its alpha once, and the result is opaque. */
    color(&a, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    rect(&a, 8, 8, 40, 40, &half, NULL);
    settle(&a);
    CHECK_HEX(at(&a, 20, 20), 0xFFFF7F7Fu);
    /* ... on nothing, and on itself. */
    color(&a, 0, MEGLER_GFX_BLEND_CLEAR);
    rect(&a, 8, 8, 40, 40, &half, NULL);
    settle(&a);
    CHECK_HEX(at(&a, 20, 20), 0x80800000u);
    rect(&a, 8, 8, 20, 40, &half, NULL);
    settle(&a);
    CHECK_HEX(at(&a, 10, 20), 0xC0C00000u);
    CHECK_HEX(at(&a, 30, 20), 0x80800000u);
    CHECK_HEX(at(&a, 50, 20), 0u);
    /* The memory written behind the canvas's back, as the core writes single pixels: told, the canvas draws on what is there. */
    for (i = 0; i < 64 * 48; i++) a.px[i] = 0xFF0000FFu;
    wrote(&a);
    rect(&a, 0, 0, 32, 48, &half, NULL);
    settle(&a);
    CHECK_HEX(at(&a, 10, 10), 0xFF80007Fu);
    CHECK_HEX(at(&a, 40, 10), 0xFF0000FFu);
    /* A second canvas on the same memory, as the core does it: the first is flushed before the second is made or draws, and each is told of the other's drawing. */
    second = H->canvas_new(H->user, a.px, a.w, a.h, a.w * 4);
    CHECK(second != NULL);
    if (second) {
        megler_gfx_paint green = paint_of(0xFF00FF00u);
        H->draw_rect(H->user, second, 40, 0, 64, 20, &green, NULL);
        H->flush(H->user, second);
        wrote(&a);
        rect(&a, 36, 10, 50, 30, &half, NULL);
        settle(&a);
        CHECK_HEX(at(&a, 45, 15), over(0x80800000u, 0xFF00FF00u));
        CHECK_HEX(at(&a, 60, 5), 0xFF00FF00u);
        CHECK_HEX(at(&a, 45, 25), 0xFF80007Fu);
        H->touched(H->user, second);
        H->draw_rect(H->user, second, 0, 40, 64, 48, &green, NULL);
        H->flush(H->user, second);
        H->canvas_free(H->user, second);
        CHECK_HEX(at(&a, 5, 44), 0xFF00FF00u);
    }
    /* A bitmap drawn onto itself: its memory is the source and the target at once. */
    {
        megler_gfx_paint plain = paint_of(0xFF000000u);
        plain.flags = 0;
        color(&a, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        fill(&a, 0, 0, 16, 16, 0xFFC02020u);
        settle(&a);
        bitmap(&a, &a, 0, 0, 16, 16, 32, 16, 64, 48, &plain);
        settle(&a);
        CHECK_HEX(at(&a, 40, 30), 0xFFC02020u);
        CHECK_HEX(at(&a, 20, 30), 0xFFFFFFFFu);
    }
    pic_done(&a);
    /* A canvas over memory that already holds a picture begins with that picture. */
    memset(&b, 0, sizeof(b));
    b.g = H;
    b.w = b.h = 8;
    b.px = (u32 *)mg_alloc(sizeof(u32) * 64);
    for (i = 0; i < 64; i++) b.px[i] = 0xFFFFFFFFu;
    b.cv = H->canvas_new(H->user, b.px, 8, 8, 32);
    b.m[0] = b.m[4] = 1;
    rect(&b, 0, 0, 4, 8, &half, NULL);
    settle(&b);
    CHECK_HEX(at(&b, 1, 1), 0xFFFF7F7Fu);
    CHECK_HEX(at(&b, 6, 1), 0xFFFFFFFFu);
    pic_done(&b);
    /* What cannot be a canvas is refused, and calls with no canvas do no harm. */
    CHECK(H->canvas_new(H->user, NULL, 8, 8, 32) == NULL);
    H->flush(H->user, NULL);
    H->touched(H->user, NULL);
    H->canvas_free(H->user, NULL);
}

/* ------------------------------------------ values no caller should pass */

/*
 * The core checks what it hands on (megler.h, "THE PAINT"), so a host should
 * never see these. One that did must still not fall over, nor leave a canvas
 * unable to draw: numbers that are none, an enumeration out of range, a path
 * that ends in the middle, a bitmap that is not there.
 */
static void check_odd_values(void)
{
    static const float broken[] = { MEGLER_GFX_PATH_MOVE, 2, 2, MEGLER_GFX_PATH_LINE, 9, 2, MEGLER_GFX_PATH_CUBIC, 3, 3, 4 };
    static const float unknown[] = { MEGLER_GFX_PATH_MOVE, 2, 2, 17, 5, 5, MEGLER_GFX_PATH_LINE, 9, 9 };
    static const u32 one_colour[1] = { 0xFF00FF00u };
    static const u16 letters[] = { 'o', 'd', 'd' };
    float nan = (float)sqrt(-1.0), huge = 1.0e30f, bounds[4], m[6];
    megler_gfx_paint paint = paint_of(0xFFFF0000u), odd;
    megler_gfx_image image;
    void *sh;
    pic p, s;
    say("  values no caller should pass: numbers that are none, enumerations out of range, paths cut short");
    pic_make(&p, H, 32, 32);
    small_picture(&s, H);
    color(&p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    rect(&p, nan, 0, 10, 10, &paint, NULL);
    rect(&p, 0, 0, huge, huge * 10, &paint, NULL);
    oval(&p, 0, nan, 10, 10, &paint, NULL);
    round_rect(&p, 0, 0, 10, 10, nan, 2, &paint, NULL);
    clip_rect(&p, nan, 0, 10, 10, MEGLER_GFX_CLIP_INTERSECT);
    path(&p, NULL, 12, MEGLER_GFX_WINDING, &paint, NULL);
    path(&p, broken, -3, MEGLER_GFX_WINDING, &paint, NULL);
    m[0] = nan;
    m[1] = m[2] = m[3] = m[5] = 0;
    m[4] = 1;
    H->set_matrix(H->user, p.cv, m);
    bitmap(&p, &s, 0, 0, 4, 4, nan, 0, 8, 8, &paint);
    bitmap(&p, &s, 0, 0, 4, 4, 0, 0, huge, 8, &paint);
    image_of(&s, &image);
    image.pixels = NULL;
    bounds[0] = bounds[1] = 0;
    bounds[2] = bounds[3] = 8;
    H->draw_bitmap(H->user, p.cv, &image, bounds, bounds, &paint);
    image_of(&s, &image);
    image.stride = 3;
    H->draw_bitmap(H->user, p.cv, &image, bounds, bounds, &paint);
    text16(&p, letters, 3, nan, 10, &paint, NULL, NULL);
    text16(&p, NULL, 3, 4, 10, &paint, NULL, NULL);
    text16(&p, letters, -1, 4, 10, &paint, NULL, NULL);
    H->restore(H->user, p.cv);
    H->restore(H->user, p.cv);
    settle(&p);
    /* None of that drew anything, and none of it broke the canvas. */
    CHECK(at(&p, 5, 5) == 0xFFFFFFFFu && at(&p, 20, 20) == 0xFFFFFFFFu);
    /* A path that goes wrong is its good part: here one line, which filled is nothing and stroked is a line. */
    odd = stroke_of(0xFF0000FFu, 2);
    odd.flags = 0;
    path(&p, broken, (int)MG_COUNT(broken), MEGLER_GFX_WINDING, &odd, NULL);
    path(&p, unknown, (int)MG_COUNT(unknown), MEGLER_GFX_WINDING, &odd, NULL);
    settle(&p);
    CHECK(at(&p, 5, 2) == 0xFF0000FFu && at(&p, 5, 5) == 0xFFFFFFFFu);
    /* Enumerations out of range are taken as the plainest thing: a filled shape, laid over, with no filter. */
    odd = paint_of(0x80FF0000u);
    odd.style = 7;
    odd.cap = -4;
    odd.join = 99;
    odd.blend = 99;
    odd.filter_mode = 40;
    odd.filter_color = 0xFF00FF00u;
    odd.text_align = 12;
    odd.miter_limit = nan;
    odd.stroke_width = nan;
    odd.text_size = nan;
    odd.text_scale_x = 0;
    odd.shadow_radius = nan;
    odd.flags = 0x7FFFFFF0;
    rect(&p, 10, 10, 20, 20, &odd, NULL);
    text16(&p, letters, 3, 2, 30, &odd, NULL, NULL);
    settle(&p);
    CHECK_HEX(at(&p, 15, 15), 0xFFFF7F7Fu);
    color(&p, 0xFF000000u, 99);
    settle(&p);
    CHECK_HEX(at(&p, 15, 15), 0xFF000000u);
    bounds[0] = nan;
    {
        saved layer = save_layer(&p, bounds, 300, 99);
        color(&p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        restore(&p, layer);
    }
    settle(&p);
    CHECK_HEX(at(&p, 15, 15), 0xFFFFFFFFu);
    /* The measuring functions answer something for such a paint, and nothing for no text. */
    CHECK(H->text_measure(H->user, letters, 3, &odd, NULL) > 0 && H->text_measure(H->user, NULL, 3, &odd, NULL) == 0 && H->text_measure(H->user, letters, -2, &odd, NULL) == 0);
    CHECK(H->text_break(H->user, letters, 3, nan, &odd, NULL, true) >= 0 && H->text_break(H->user, letters, 3, -5, &odd, NULL, false) == 0);
    /* Shaders of one colour, of numbers that are none, and none at all. */
    sh = H->shader_linear(H->user, 0, 0, 10, 0, one_colour, NULL, 1, 9);
    rect(&p, 0, 0, 8, 8, &paint, sh);
    H->shader_matrix(H->user, sh, m);
    H->shader_free(H->user, sh);
    sh = H->shader_radial(H->user, 4, 4, -1, one_colour, NULL, 1, MEGLER_GFX_TILE_CLAMP);
    rect(&p, 8, 0, 16, 8, &paint, sh);
    H->shader_free(H->user, sh);
    CHECK(H->shader_linear(H->user, nan, 0, 10, 0, one_colour, NULL, 1, 0) == NULL);
    CHECK(H->shader_sweep(H->user, 0, 0, NULL, NULL, 2) == NULL);
    H->shader_free(H->user, NULL);
    H->shader_matrix(H->user, NULL, NULL);
    settle(&p);
    CHECK_HEX(at(&p, 4, 4), 0xFF00FF00u);
    CHECK_HEX(at(&p, 12, 4), 0xFF00FF00u);
    /* Images and fonts that are nothing. */
    CHECK(!H->image_decode(H->user, "x", 1, p.px, 1, 1, 4) && !H->image_decode(H->user, NULL, 100, p.px, 1, 1, 4));
    CHECK(!H->image_encode(H->user, NULL, 4, 4, 16, MEGLER_GFX_PNG, 100, bytes_write, NULL) && !H->image_encode(H->user, p.px, 0, 4, 16, MEGLER_GFX_PNG, 100, bytes_write, NULL));
    CHECK(H->typeface_from_data(H->user, NULL, 100) == NULL && H->typeface_from_data(H->user, "tiny", 4) == NULL);
    H->bitmap_gone(H->user, 0);
    /* After all of it the canvas draws as before. */
    matrix_reset(&p);
    paint = paint_of(0xFF123456u);
    paint.flags = 0;
    rect(&p, 0, 0, 32, 32, &paint, NULL);
    settle(&p);
    CHECK_HEX(at(&p, 31, 31), 0xFF123456u);
    pic_done(&s);
    pic_done(&p);
}

/* ------------------------------------------------- several threads at once */

typedef struct worker {
    mg_thread *thread;
    pic p;
    u32 sum;
    int number;
} worker;

static u32 picture_sum(const pic *p)
{
    u32 sum = 2166136261u;
    int i;
    for (i = 0; i < p->w * p->h; i++) sum = (sum ^ p->px[i]) * 16777619u;
    return sum;
}

/* What each thread does, over and over: shapes, a gradient, a layer, text, a picture decoded -- on a canvas of its own. */
static void worker_draw(pic *p, int round)
{
    static const u32 two[2] = { 0xFFE02020u, 0xFF2040E0u };
    static const float ends[2] = { 0, 1 };
    megler_gfx_paint a = paint_of(0xC02060C0u), t = paint_of(0xFF000000u);
    void *sh = linear(p->g, 0, 0, 100, 0, two, ends, 2, MEGLER_GFX_TILE_MIRROR);
    pathbuf v;
    saved s;
    u16 line_of[40];
    char words[40];
    pic d;
    color(p, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    rect(p, 10, 10, 150, 60, &a, sh);
    memset(&v, 0, sizeof(v));
    pb_star(&v, 80, 110, 60);
    path(p, v.v, v.n, MEGLER_GFX_EVEN_ODD, &a, NULL);
    s = save_layer(p, NULL, 128, MEGLER_GFX_BLEND_SRC_OVER);
    circle_path(&v, 100, 100, 50);
    clip_path(p, v.v, v.n, MEGLER_GFX_WINDING, MEGLER_GFX_CLIP_INTERSECT);
    color(p, 0xFF208040u, MEGLER_GFX_BLEND_SRC_OVER);
    restore(p, s);
    snprintf(words, sizeof(words), "Thread text %d", round % 7);
    t.text_size = 14.0f + (float)(round % 5);
    text16(p, line_of, units(words, line_of, 40), 8, 150, &t, NULL, sh);
    p->g->text_measure(p->g->user, line_of, units(words, line_of, 40), &t, NULL);
    p->g->shader_free(p->g->user, sh);
    if (decode(t_png.data, t_png.size, 2, &d)) {
        t.flags = 0;
        bitmap(p, &d, 0, 0, (float)d.w, (float)d.h, 150, 100, 150 + (float)d.w, 100 + (float)d.h, &t);
        pic_done(&d);
    }
    settle(p);
}

static MG_HOSTCB void worker_main(void *arg)
{
    worker *w = (worker *)arg;
    int round;
    for (round = 0; round < 40; round++) worker_draw(&w->p, round);
    worker_draw(&w->p, 3);
    w->sum = picture_sum(&w->p);
}

static void check_threads(void)
{
    worker workers[4];
    pic alone;
    u32 want;
    int i, started = 0, same = 0;
    say("  four threads drawing at once, each on a canvas of its own");
    pic_make(&alone, H, 200, 170);
    worker_draw(&alone, 3);
    want = picture_sum(&alone);
    memset(workers, 0, sizeof(workers));
    for (i = 0; i < 4; i++) {
        workers[i].number = i;
        pic_make(&workers[i].p, H, 200, 170);
        workers[i].thread = mg_thread_start(worker_main, &workers[i], 1u << 20, "gfx-test");
        started += workers[i].thread != NULL;
    }
    for (i = 0; i < 4; i++) {
        if (workers[i].thread) mg_thread_join(workers[i].thread);
        same += workers[i].sum == want;
        pic_done(&workers[i].p);
    }
    CHECK(started == 4);
    /* Each thread's last picture is the picture one thread draws alone. */
    CHECK(same == 4);
    pic_done(&alone);
}

/* ------------------------------------------------------- what things cost */

static double microseconds(u64 from, int times) { return (double)(mg_time_monotonic_ns() - from) / 1000.0 / (double)times; }

/* Not checks: how long the calls an app makes most take on this machine, a line each. The phone will say its own. */
static void measure_costs(void)
{
    static u32 small_memory[64 * 64];
    pic screen, photo, large;
    megler_gfx_paint opaque = paint_of(0xFF2060C0u), faint = paint_of(0x802060C0u), words = paint_of(0xFF000000u), plain = paint_of(0xFF000000u);
    u16 t[40];
    char line_of[40];
    float bounds[4] = { 100, 100, 300, 300 };
    double made, erased, rects, rounded, flushed, flushedPart, first, again, drawn, copied, shared, kept, layered, composed, strip, written;
    megler_gfx_image same;
    u64 from;
    int i, n;
    say("  what things cost here (a phone's processor is several times slower):");
    from = mg_time_monotonic_ns();
    for (i = 0; i < 2000; i++) {
        void *cv = H->canvas_new(H->user, small_memory, 64, 64, 256);
        H->canvas_free(H->user, cv);
    }
    made = microseconds(from, 2000);
    pic_make(&screen, H, 720, 1280);
    photo_picture(&photo, H);
    pic_make(&large, H, 512, 512);
    color(&large, 0xFF8040C0u, MEGLER_GFX_BLEND_SRC);
    settle(&large);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 50; i++) {
        color(&screen, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        settle(&screen);
    }
    erased = microseconds(from, 50);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 2000; i++) rect(&screen, (float)(i % 17) * 40, (float)(i % 31) * 40, (float)(i % 17) * 40 + 40, (float)(i % 31) * 40 + 40, &opaque, NULL);
    settle(&screen);
    rects = microseconds(from, 2000);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 2000; i++)
        round_rect(&screen, (float)(i % 17) * 40 + 0.5f, (float)(i % 31) * 40, (float)(i % 17) * 40 + 40, (float)(i % 31) * 40 + 40, 8, 8, &faint, NULL);
    settle(&screen);
    rounded = microseconds(from, 2000);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 500; i++) {
        rect(&screen, 10, 10, 50, 50, &opaque, NULL);
        settle(&screen);
    }
    flushed = microseconds(from, 500);
    /* The same in a small part of the picture, as a window that redraws a little of itself draws: the clip leaves 200 x 100
       of the 720 x 1280 open. */
    {
        saved part = save(&screen);
        clip_rect(&screen, 10, 10, 210, 110, MEGLER_GFX_CLIP_INTERSECT);
        from = mg_time_monotonic_ns();
        for (i = 0; i < 500; i++) {
            rect(&screen, 10, 10, 50, 50, &opaque, NULL);
            settle(&screen);
        }
        flushedPart = microseconds(from, 500);
        restore(&screen, part);
    }
    words.text_size = 16;
    from = mg_time_monotonic_ns();
    for (i = 0; i < 500; i++) {
        snprintf(line_of, sizeof(line_of), "A line of text, number %d", i);
        measure(t, units(line_of, t, 40), &words, NULL);
    }
    first = microseconds(from, 500);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 20000; i++) measure(t, 24, &words, NULL);
    again = microseconds(from, 20000);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 500; i++) {
        snprintf(line_of, sizeof(line_of), "A line of text, number %d", i);
        n = units(line_of, t, 40);
        text16(&screen, t, n, 20, 20.0f + (float)(i % 60) * 20, &words, NULL, NULL);
    }
    settle(&screen);
    drawn = microseconds(from, 500);
    plain.flags = MEGLER_GFX_FILTER_BITMAP;
    from = mg_time_monotonic_ns();
    for (i = 0; i < 1000; i++) bitmap(&screen, &photo, 0, 0, 64, 64, (float)(i % 10) * 70, (float)(i % 17) * 70, (float)(i % 10) * 70 + 64, (float)(i % 17) * 70 + 64, &plain);
    settle(&screen);
    copied = microseconds(from, 1000);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 100; i++) bitmap(&screen, &large, 0, 0, 512, 512, 100, 100, 612, 612, &plain);
    settle(&screen);
    shared = microseconds(from, 100);
    /* The same bitmap under one id and version every time, as the core hands a picture that has not changed: a host that
       keeps bitmaps draws it from what it kept. (bitmap() above gives each call an id of its own.) */
    {
        float src[4] = { 0, 0, 512, 512 }, dst[4] = { 100, 100, 612, 612 };
        image_of(&large, &same);
        from = mg_time_monotonic_ns();
        for (i = 0; i < 100; i++) H->draw_bitmap(H->user, screen.cv, &same, src, dst, &plain);
        settle(&screen);
        kept = microseconds(from, 100);
        H->bitmap_gone(H->user, same.id);
    }
    /* A frame as a window draws one when a little of it changed, a caret blinking in a text field: a strip of 720 x 64 of the
       720 x 1280 picture redrawn -- its background, eight rectangles, two lines of text -- and flushed, which is when a host
       that draws elsewhere than in the memory puts the strip there. */
    {
        megler_gfx_paint bar = paint_of(0xFFE0E0E0u), caret = paint_of(0xFF2060C0u);
        from = mg_time_monotonic_ns();
        for (i = 0; i < 100; i++) {
            saved part = save(&screen);
            int k;
            clip_rect(&screen, 0, 600, 720, 664, MEGLER_GFX_CLIP_INTERSECT);
            color(&screen, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
            for (k = 0; k < 7; k++) rect(&screen, (float)(k * 100 + 10), 610, (float)(k * 100 + 90), 612, &bar, NULL);
            if (i & 1) rect(&screen, 300, 614, 302, 650, &caret, NULL);
            text16(&screen, t, units("A line of text in a field", t, 40), 20, 640, &words, NULL, NULL);
            text16(&screen, t, units("and its hint", t, 40), 420, 640, &words, NULL, NULL);
            restore(&screen, part);
            settle(&screen);
        }
        strip = microseconds(from, 100);
    }
    /* The whole picture written behind the canvas's back (touched), then a rectangle drawn on it and flushed: a host that
       keeps the picture elsewhere takes all of it again. */
    from = mg_time_monotonic_ns();
    for (i = 0; i < 50; i++) {
        screen.px[i] ^= 0x00010101u;
        wrote(&screen);
        rect(&screen, 10, 10, 50, 50, &opaque, NULL);
        settle(&screen);
    }
    written = microseconds(from, 50);
    from = mg_time_monotonic_ns();
    for (i = 0; i < 200; i++) {
        saved layer = save_layer(&screen, bounds, 128, MEGLER_GFX_BLEND_SRC_OVER);
        rect(&screen, 120, 120, 280, 280, &opaque, NULL);
        restore(&screen, layer);
    }
    settle(&screen);
    layered = microseconds(from, 200);
    faint.blend = MEGLER_GFX_BLEND_XOR;
    from = mg_time_monotonic_ns();
    for (i = 0; i < 500; i++) oval(&screen, 300, 300, 400, 400, &faint, NULL);
    settle(&screen);
    composed = microseconds(from, 500);
    say("    a canvas made and freed %.1f us; a 720x1280 picture erased %.0f us; a 40x40 rectangle %.1f us, a smoothed half transparent rounded one %.1f us;", made, erased, rects,
        rounded);
    say("    a rectangle and a flush %.0f us, in a clip of 200x100 of the 720x1280 picture %.0f us; a line of 24 letters measured for the first time %.0f us, and again %.2f us;",
        flushed, flushedPart, first, again);
    say("    a line of 24 letters drawn %.0f us;", drawn);
    say("    a 64x64 bitmap %.0f us, a 512x512 one %.0f us, the 512x512 one again under the same id and version %.0f us; a 200x200 layer at half strength with a "
        "rectangle in it %.0f us; a 100x100 oval by XOR %.0f us;", copied, shared, kept, layered, composed);
    say("    a frame that redraws a strip of 720x64 of the 720x1280 picture (a colour, 8 rectangles, 2 lines of text) and is flushed %.0f us; the whole picture "
        "written behind the canvas's back, then a rectangle drawn and flushed %.0f us", strip, written);
    pic_done(&large);
    pic_done(&photo);
    pic_done(&screen);
}

/* ------------------------------------------------------------------ the run */

/* A scene of the host's alone, kept as a picture; `check` looks at pixels of it where the right answer is known. */
static void alone(const char *name, scene_fn scene, void (*check)(pic *p), int w, int h)
{
    pic p;
    pic_make(&p, H, w, h);
    CHECK(p.cv != NULL);
    if (p.cv) {
        t_smooth = true;
        scene(&p);
        settle(&p);
        if (check) check(&p);
        keep(name, &p);
    }
    pic_done(&p);
}

/* Every picture kept, at half its size, in rows of three with its name under it: one file to look at. */
static void write_sheet(void)
{
    const int cell_w = 330, cell_h = 280, columns = 3;
    int rows = (t_nsheet + columns - 1) / columns, i, x, y;
    pic sheet;
    if (!t_config->out_dir || !t_nsheet) return;
    pic_make(&sheet, H, columns * cell_w, rows * cell_h);
    if (!sheet.cv) {
        pic_done(&sheet);
        return;
    }
    color(&sheet, 0xFF404040u, MEGLER_GFX_BLEND_SRC);
    settle(&sheet);
    for (i = 0; i < t_nsheet; i++) {
        int ox = (i % columns) * cell_w + 5, oy = (i / columns) * cell_h + 5, w = t_sheet[i].w / 2, h = t_sheet[i].h / 2;
        megler_gfx_paint white = paint_of(0xFFFFFFFFu);
        if (w > cell_w - 10) w = cell_w - 10;
        if (h > cell_h - 30) h = cell_h - 30;
        /* Each pixel of the sheet is the mean of four of the picture, over a mid grey where the picture is transparent. */
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                u32 sum[4] = { 0, 0, 0, 0 }, out = 0;
                int k, shift;
                for (k = 0; k < 4; k++) {
                    u32 v = over(t_sheet[i].px[(2 * y + k / 2) * t_sheet[i].w + 2 * x + (k & 1)], 0xFF909090u);
                    for (shift = 0; shift < 32; shift += 8) sum[shift / 8] += (v >> shift) & 255;
                }
                for (shift = 0; shift < 32; shift += 8) out |= ((sum[shift / 8] + 2) / 4) << shift;
                sheet.px[(oy + y) * sheet.w + ox + x] = out;
            }
        wrote(&sheet);
        white.text_size = 14;
        text(&sheet, t_sheet[i].name, (float)ox, (float)(oy + h + 18), &white, NULL);
        settle(&sheet);
    }
    {
        int kept = t_nsheet;
        keep("sheet", &sheet);
        t_nsheet = kept;
    }
    pic_done(&sheet);
}

int mg_gfx_scenes_run(const mg_gfx_scenes_config *config, mg_test_sink sink, void *user, int *checks)
{
    static const char *const fonts[] = { "C:/Windows/Fonts/comic.ttf", "C:/Windows/Fonts/georgia.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf" };
    size_t i;
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    t_config = config;
    t_nsheet = 0;
    H = config->host;
    S = mg_gfx_standin();
    say("a shell's drawing, through megler_gfx_host: scenes drawn, compared with the stand-in's where it can draw them, and checked");
    CHECK(H->canvas_new && H->canvas_free && H->flush && H->touched && H->save && H->save_layer && H->restore && H->set_matrix && H->clip_rect && H->clip_path);
    CHECK(H->draw_color && H->draw_rect && H->draw_round_rect && H->draw_oval && H->draw_path && H->draw_bitmap && H->draw_text);
    CHECK(H->text_measure && H->text_metrics && H->text_bounds && H->text_break && H->text_widths && H->typeface_create && H->typeface_derive && H->typeface_from_data);
    CHECK(H->shader_linear && H->shader_radial && H->shader_sweep && H->shader_bitmap && H->shader_matrix && H->shader_free);
    CHECK(H->image_size && H->image_decode && H->image_encode && H->bitmap_gone);
    if (t_failed) {
        say("  (the host lacks functions: nothing more is tried)");
        if (checks) *checks += t_checks;
        return t_failed;
    }
    {
        /* Whether the host can draw at all here: without Direct2D it makes no canvas, and the rest would only say so sixty times. */
        pic probe;
        pic_make(&probe, H, 4, 4);
        CHECK(probe.cv != NULL);
        if (!probe.cv) {
            say("  (the host makes no canvas: nothing more is tried)");
            pic_done(&probe);
            if (checks) *checks += t_checks;
            return t_failed;
        }
        pic_done(&probe);
    }
    if (config->out_dir) mg_path_mkdirs(config->out_dir);

    /* A typeface out of a font file's bytes, for the scenes and the checks of text. */
    t_face_data = NULL;
    t_face_file[0] = 0;
    for (i = 0; !t_face_data && i < (config->font_file ? 1 : MG_COUNT(fonts)); i++) {
        const char *file = config->font_file ? config->font_file : fonts[i];
        size_t size = 0;
        u8 *data = mg_file_read_all(file, &size);
        if (!data) continue;
        t_face_data = H->typeface_from_data(H->user, data, size);
        /* (The bytes are the caller's: the host has copied what it keeps.) */
        memset(data, 0x55, size);
        free(data);
        if (t_face_data) mg_strlcpy(t_face_file, file, sizeof(t_face_file));
    }
    if (t_face_data) say("  a typeface from the bytes of %s", t_face_file);
    else say("  (no font file could be read: typeface_from_data is only tried with bytes that are no font)");
    CHECK(t_face_data != NULL || !config->font_file);

    say("  scenes the stand-in draws too (the host's paints smooth edges unless it says otherwise):");
    both("01-fills", scene_fills, 480, 360, true, false);
    both("02-fills-rough", scene_fills, 480, 360, false, true);
    both("03-corners", scene_corners, 64, 48, false, true);
    both("04-shapes", scene_shapes, 480, 360, true, false);
    both("05-shapes-rough", scene_shapes, 480, 360, false, false);
    both("06-strokes-square", scene_strokes_square, 480, 360, true, false);
    both("07-strokes-rough", scene_strokes_square, 480, 360, false, true);
    both("08-clips-rect", scene_clips_rect, 480, 360, true, false);
    both("09-blends", scene_blends, 480, 360, true, false);
    both("10-bitmaps-nearest", scene_bitmaps_nearest, 480, 360, false, true);
    both("11-layer-alpha", scene_layer_alpha, 480, 360, true, false);
    check_memory();
    check_blend_table();

    say("  scenes of the host's alone, with pixels checked where the answer is known:");
    alone("12-strokes", scene_strokes, check_strokes, 480, 360);
    alone("13-arcs", scene_arcs, NULL, 480, 360);
    alone("14-paths", scene_paths, check_paths, 480, 360);
    alone("15-clips", scene_clips, check_clips, 480, 360);
    alone("16-layers", scene_layers, check_layers, 480, 360);
    alone("17-matrices", scene_matrices, NULL, 480, 360);
    alone("18-bitmaps", scene_bitmaps, check_bitmaps, 480, 360);
    alone("19-shaders", scene_shaders, check_shaders, 480, 360);
    alone("20-blend-modes", scene_blend_modes, NULL, 644, 360);
    alone("21-text", scene_text, NULL, 660, 520);
    alone("22-text-styles", scene_text_styles, NULL, 480, 360);
    check_text();
    images_make();
    alone("23-images", scene_images, NULL, 480, 360);
    check_images();
    alone("24-shadows-filters", scene_shadows_filters, check_shadows, 480, 360);
    alone("25-layer-blur", scene_layer_blur, check_layer_blur, 480, 240);
    alone("26-clips-round", scene_clips_round, check_clips_round, 480, 240);
    check_odd_values();
    check_threads();
    measure_costs();
    write_sheet();
    if (config->out_dir)
        say("  the pictures are in %s: %d scenes as %s files, and %ssheet with all of them at half size", config->out_dir, t_nsheet, config->bmp ? "BMP" : "PNG",
            config->file_prefix ? config->file_prefix : "");
    for (i = 0; i < (size_t)t_nsheet; i++) free(t_sheet[i].px);
    t_nsheet = 0;
    free(t_png.data);
    free(t_jpeg_good.data);
    free(t_jpeg_poor.data);
    free(t_jpeg_file);
    memset(&t_png, 0, sizeof(t_png));
    memset(&t_jpeg_good, 0, sizeof(t_jpeg_good));
    memset(&t_jpeg_poor, 0, sizeof(t_jpeg_poor));
    t_jpeg_file = NULL;
    if (checks) *checks += t_checks;
    return t_failed;
}
