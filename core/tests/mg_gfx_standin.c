/*
 * A stand-in for the shell's drawing, for tests (mg_gfx_standin.h).
 *
 * It draws straight into the memory it is given, so flush and touched have
 * nothing to do. Nothing is smoothed, whatever the paint says: a pixel is
 * drawn when its centre is inside the shape (on a left or top edge: inside),
 * and every number is worked out the same way on every machine, so a test
 * can name the pixels it expects.
 *
 * What it does as the contract says: filled and stroked rectangles, round
 * rectangles, ovals and paths under any matrix, both fill rules; the blend
 * modes but one; clips by rectangle; save and restore; bitmaps stretched,
 * with the paint's alpha, its blend and its colour filter; PNG files read
 * and written.
 *
 * What it does in its own way, each of which a test of real drawing would
 * have to know:
 *
 *   - Curves are polygons: each Bezier is 24 straight pieces.
 *   - A stroke is a rectangle along each straight piece, lengthened by half
 *     the width at every corner. That is exact for lines and for shapes with
 *     right angles; other corners are neither mitered, rounded nor bevelled,
 *     and round caps are square.
 *   - A clip is a rectangle of whole pixels: the box of what was asked for
 *     when the matrix rotates, and for a path the box of the path. A
 *     rectangle cut out of the clip is kept (eight of them at most) when the
 *     matrix does not rotate, and not otherwise; a path cut out is kept the
 *     same way, as its box (exact for a rectangle's path, which is what an
 *     inverse path clipped to arrives as: the core turns that into cutting
 *     the path out).
 *   - A layer is not a picture of its own: save_layer is a save, and its
 *     alpha multiplies what is drawn until the restore. That is the same
 *     only while what is drawn in the layer does not overlap itself, and the
 *     layer's blend mode is not done.
 *   - OVERLAY is drawn as SRC_OVER. Shadows are not drawn.
 *   - Bitmaps are sampled at the nearest pixel, with or without the filter
 *     flag.
 *   - A gradient is its first colour, a bitmap shader its bitmap's top left
 *     pixel.
 *   - Text is boxes. Every UTF-16 unit advances (0.6 + letter_spacing) *
 *     text_size * text_scale_x, and every unit above U+0020 other than
 *     U+00A0 is a filled box 0.5 * text_size * text_scale_x wide and 0.7 *
 *     text_size high standing on the baseline, at the start of its advance.
 *     The ascent is 0.8 and the descent 0.2 of the text size, top and bottom
 *     the same, no leading. All typefaces are that one; skew, fake bold and
 *     the paint's style do nothing to it. A line breaks after spaces. A
 *     glyph's outline (text_path) is its box, four lines clockwise.
 *   - Image files: PNG only, not interlaced (every colour type and bit
 *     depth, tRNS included; the chunks' checksums are not looked at). An
 *     image asked for at another size than its own is sampled at the nearest
 *     pixel. Everything is written as PNG, JPEG too.
 */
#include <math.h>
#include <stdio.h>

#include "zlib.h"

#include "../src/mg_base.h"
#include "../src/host/mg_host.h"
#include "mg_gfx_standin.h"

#define MAX_CUTS 8
#define CURVE_PIECES 24

typedef struct sstate {
    float m[6];
    i32 clip[4];                       /* left, top, right, bottom, in pixels, inside the bitmap */
    i32 cut[MAX_CUTS][4];              /* rectangles taken out of it */
    i32 cuts;
    u32 alpha;                         /* the layers' alphas multiplied, 0..255 */
} sstate;

typedef struct scanvas {
    u8 *px;
    i32 w, h, stride;
    sstate *st;                        /* st[depth] is the current one */
    i32 depth, cap;
    u8 *mask;                          /* w * h: which pixels the shape being drawn covers; all zero between calls */
    i32 box[4];                        /* the part of the mask that may be set */
} scanvas;

typedef struct sshader {
    u32 color;                         /* premultiplied */
} sshader;

/* The corners of figures, each figure closed or open. */
typedef struct spoints {
    double *xy;
    i32 n, cap;
    i32 *end;                          /* one past each figure's last point */
    bool *closed;
    i32 figures, figure_cap, begun;    /* begun: where the figure being added to starts */
} spoints;

/* ---------------------------------------------------------------- colours */

static u32 mul255(u32 a, u32 b) { return (a * b + 127) / 255; }

static u32 premultiplied(u32 c)
{
    u32 a = c >> 24;
    if (a == 255) return c;
    return a << 24 | mul255((c >> 16) & 255, a) << 16 | mul255((c >> 8) & 255, a) << 8 | mul255(c & 255, a);
}

static u32 unpremultiplied(u32 p)
{
    u32 a = p >> 24, r, g, b;
    if (!a) return 0;
    if (a == 255) return p;
    r = (((p >> 16) & 255) * 255 + a / 2) / a;
    g = (((p >> 8) & 255) * 255 + a / 2) / a;
    b = ((p & 255) * 255 + a / 2) / a;
    return a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

/* Every channel of a premultiplied pixel by an alpha. */
static u32 faded(u32 p, u32 alpha)
{
    if (alpha >= 255) return p;
    return mul255(p >> 24, alpha) << 24 | mul255((p >> 16) & 255, alpha) << 16 | mul255((p >> 8) & 255, alpha) << 8 | mul255(p & 255, alpha);
}

/* One channel of S on D by megler.h's table, in whole numbers of 255ths. */
static u32 blend_channel(int mode, u32 s, u32 d, u32 sa, u32 da)
{
    u32 v, both = mul255(s, 255 - da) + mul255(d, 255 - sa), x = mul255(s, da), y = mul255(d, sa);
    switch (mode) {
    case MEGLER_GFX_BLEND_CLEAR: v = 0; break;
    case MEGLER_GFX_BLEND_SRC: v = s; break;
    case MEGLER_GFX_BLEND_DST: v = d; break;
    case MEGLER_GFX_BLEND_DST_OVER: v = d + mul255(s, 255 - da); break;
    case MEGLER_GFX_BLEND_SRC_IN: v = x; break;
    case MEGLER_GFX_BLEND_DST_IN: v = y; break;
    case MEGLER_GFX_BLEND_SRC_OUT: v = mul255(s, 255 - da); break;
    case MEGLER_GFX_BLEND_DST_OUT: v = mul255(d, 255 - sa); break;
    case MEGLER_GFX_BLEND_SRC_ATOP: v = x + mul255(d, 255 - sa); break;
    case MEGLER_GFX_BLEND_DST_ATOP: v = y + mul255(s, 255 - da); break;
    case MEGLER_GFX_BLEND_XOR: v = both; break;
    case MEGLER_GFX_BLEND_PLUS: v = s + d; break;
    case MEGLER_GFX_BLEND_MODULATE: v = mul255(s, d); break;
    case MEGLER_GFX_BLEND_SCREEN: v = s + d - mul255(s, d); break;
    case MEGLER_GFX_BLEND_DARKEN: v = both + (x < y ? x : y); break;
    case MEGLER_GFX_BLEND_LIGHTEN: v = both + (x > y ? x : y); break;
    default: v = s + mul255(d, 255 - sa); break;
    }
    return v > 255 ? 255 : v;
}

static u32 blended(int mode, u32 s, u32 d)
{
    u32 sa = s >> 24, da = d >> 24;
    if (mode == MEGLER_GFX_BLEND_SRC_OVER && sa == 255) return s;
    return blend_channel(mode, sa, da, sa, da) << 24 | blend_channel(mode, (s >> 16) & 255, (d >> 16) & 255, sa, da) << 16 |
           blend_channel(mode, (s >> 8) & 255, (d >> 8) & 255, sa, da) << 8 | blend_channel(mode, s & 255, d & 255, sa, da);
}

/* --------------------------------------------------------------- canvases */

static sstate *state(scanvas *v) { return &v->st[v->depth]; }

/* A coordinate as a pixel number that stays a number: far outside any bitmap is as good as infinitely far. */
static i32 pixel_at(double v)
{
    if (!(v == v)) return 0;
    if (v > 1000000000.0) return 1000000000;
    if (v < -1000000000.0) return -1000000000;
    return (i32)v;
}

static void map(const float *m, double x, double y, double *ox, double *oy)
{
    *ox = (double)m[0] * x + (double)m[1] * y + (double)m[2];
    *oy = (double)m[3] * x + (double)m[4] * y + (double)m[5];
}

/* The pixels whose centres are in the box of a rectangle through the matrix. */
static void pixel_box(const float *m, double l, double t, double r, double b, i32 *out)
{
    double x[4], y[4], x0, y0, x1, y1;
    int i;
    map(m, l, t, &x[0], &y[0]);
    map(m, r, t, &x[1], &y[1]);
    map(m, r, b, &x[2], &y[2]);
    map(m, l, b, &x[3], &y[3]);
    x0 = x1 = x[0];
    y0 = y1 = y[0];
    for (i = 1; i < 4; i++) {
        if (x[i] < x0) x0 = x[i];
        if (x[i] > x1) x1 = x[i];
        if (y[i] < y0) y0 = y[i];
        if (y[i] > y1) y1 = y[i];
    }
    out[0] = pixel_at(ceil(x0 - 0.5));
    out[1] = pixel_at(ceil(y0 - 0.5));
    out[2] = pixel_at(ceil(x1 - 0.5));
    out[3] = pixel_at(ceil(y1 - 0.5));
}

static void clip_to(sstate *s, const i32 *box)
{
    if (box[0] > s->clip[0]) s->clip[0] = box[0];
    if (box[1] > s->clip[1]) s->clip[1] = box[1];
    if (box[2] < s->clip[2]) s->clip[2] = box[2];
    if (box[3] < s->clip[3]) s->clip[3] = box[3];
    if (s->clip[2] < s->clip[0]) s->clip[2] = s->clip[0];
    if (s->clip[3] < s->clip[1]) s->clip[3] = s->clip[1];
}

static bool cut_out(const sstate *s, i32 x, i32 y)
{
    i32 i;
    for (i = 0; i < s->cuts; i++)
        if (x >= s->cut[i][0] && x < s->cut[i][2] && y >= s->cut[i][1] && y < s->cut[i][3]) return true;
    return false;
}

static MEGLER_CB void *s_canvas_new(void *user, void *pixels, int width, int height, int stride)
{
    scanvas *v;
    (void)user;
    if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return NULL;
    v = (scanvas *)mg_alloc(sizeof(*v));
    v->px = (u8 *)pixels;
    v->w = width;
    v->h = height;
    v->stride = stride;
    v->cap = 8;
    v->st = (sstate *)mg_alloc(sizeof(sstate) * (size_t)v->cap);
    v->st[0].m[0] = v->st[0].m[4] = 1;
    v->st[0].clip[2] = width;
    v->st[0].clip[3] = height;
    v->st[0].alpha = 255;
    return v;
}

static MEGLER_CB void s_canvas_free(void *user, void *canvas)
{
    scanvas *v = (scanvas *)canvas;
    (void)user;
    if (!v) return;
    free(v->mask);
    free(v->st);
    free(v);
}

static MEGLER_CB void s_nothing(void *user, void *canvas)
{
    (void)user;
    (void)canvas;
}

static MEGLER_CB void s_save(void *user, void *canvas)
{
    scanvas *v = (scanvas *)canvas;
    (void)user;
    if (v->depth + 1 >= v->cap) {
        v->cap *= 2;
        v->st = (sstate *)mg_realloc(v->st, sizeof(sstate) * (size_t)v->cap);
    }
    v->st[v->depth + 1] = v->st[v->depth];
    v->depth++;
}

static MEGLER_CB void s_save_layer(void *user, void *canvas, const float *bounds, int alpha, int blend)
{
    scanvas *v = (scanvas *)canvas;
    i32 box[4];
    (void)blend;
    s_save(user, canvas);
    if (bounds) {
        pixel_box(state(v)->m, bounds[0], bounds[1], bounds[2], bounds[3], box);
        clip_to(state(v), box);
    }
    state(v)->alpha = mul255(state(v)->alpha, (u32)(alpha < 0 ? 0 : alpha > 255 ? 255 : alpha));
}

static MEGLER_CB void s_restore(void *user, void *canvas)
{
    scanvas *v = (scanvas *)canvas;
    float m[6];
    (void)user;
    if (v->depth <= 0) return;
    /* The matrix is not the save's to keep (megler.h): the one in force stays. */
    memcpy(m, state(v)->m, sizeof(m));
    v->depth--;
    memcpy(state(v)->m, m, sizeof(m));
}

static MEGLER_CB void s_set_matrix(void *user, void *canvas, const float *matrix)
{
    (void)user;
    memcpy(state((scanvas *)canvas)->m, matrix, sizeof(float) * 6);
}

static MEGLER_CB void s_clip_rect(void *user, void *canvas, float left, float top, float right, float bottom, int op)
{
    scanvas *v = (scanvas *)canvas;
    sstate *s = state(v);
    i32 box[4];
    (void)user;
    pixel_box(s->m, left, top, right, bottom, box);
    if (op == MEGLER_GFX_CLIP_INTERSECT) {
        clip_to(s, box);
    } else if (s->m[1] == 0 && s->m[3] == 0 && s->cuts < MAX_CUTS) {
        memcpy(s->cut[s->cuts++], box, sizeof(box));
    }
}

/* ------------------------------------------------------------------ shapes */

static void points_free(spoints *p)
{
    free(p->xy);
    free(p->end);
    free(p->closed);
}

static void points_add(spoints *p, double x, double y)
{
    if (p->n == p->cap) {
        p->cap = p->cap * 2 + 64;
        p->xy = (double *)mg_realloc(p->xy, sizeof(double) * 2 * (size_t)p->cap);
    }
    p->xy[2 * p->n] = x;
    p->xy[2 * p->n + 1] = y;
    p->n++;
}

/* The figure being added to is complete. */
static void points_end(spoints *p, bool closed)
{
    if (p->n == p->begun) return;
    if (p->figures == p->figure_cap) {
        p->figure_cap = p->figure_cap * 2 + 16;
        p->end = (i32 *)mg_realloc(p->end, sizeof(i32) * (size_t)p->figure_cap);
        p->closed = (bool *)mg_realloc(p->closed, sizeof(bool) * (size_t)p->figure_cap);
    }
    p->end[p->figures] = p->n;
    p->closed[p->figures] = closed;
    p->figures++;
    p->begun = p->n;
}

static void points_rect(spoints *p, double l, double t, double r, double b)
{
    points_add(p, l, t);
    points_add(p, r, t);
    points_add(p, r, b);
    points_add(p, l, b);
    points_end(p, true);
}

/* A path as megler.h's draw_path has it, into corners. A stream that is not well formed ends where it goes wrong. */
static void points_path(spoints *p, const float *path, int count)
{
    double cx = 0, cy = 0, t, u, x, y;
    int i = 0, k, verb, need;
    while (path && i < count) {
        if (!(path[i] >= 0 && path[i] <= 4)) break;
        verb = (int)path[i++];
        need = verb == MEGLER_GFX_PATH_QUAD ? 4 : verb == MEGLER_GFX_PATH_CUBIC ? 6 : verb == MEGLER_GFX_PATH_CLOSE ? 0 : 2;
        if (i + need > count) break;
        if (verb == MEGLER_GFX_PATH_CLOSE) {
            points_end(p, true);
            continue;
        }
        if (verb == MEGLER_GFX_PATH_MOVE) {
            points_end(p, false);
            points_add(p, path[i], path[i + 1]);
        } else if (verb == MEGLER_GFX_PATH_LINE) {
            points_add(p, path[i], path[i + 1]);
        } else {
            for (k = 1; k <= CURVE_PIECES; k++) {
                t = (double)k / CURVE_PIECES;
                u = 1 - t;
                if (k == CURVE_PIECES) {
                    x = path[i + need - 2];
                    y = path[i + need - 1];
                } else if (verb == MEGLER_GFX_PATH_QUAD) {
                    x = u * u * cx + 2 * u * t * path[i] + t * t * path[i + 2];
                    y = u * u * cy + 2 * u * t * path[i + 1] + t * t * path[i + 3];
                } else {
                    x = u * u * u * cx + 3 * u * u * t * path[i] + 3 * u * t * t * path[i + 2] + t * t * t * path[i + 4];
                    y = u * u * u * cy + 3 * u * u * t * path[i + 1] + 3 * u * t * t * path[i + 3] + t * t * t * path[i + 5];
                }
                points_add(p, x, y);
            }
        }
        cx = path[i + need - 2];
        cy = path[i + need - 1];
        i += need;
    }
    points_end(p, false);
}

/* A quarter of an ellipse as a cubic: how far along the tangents the control points lie, for a radius of one. */
#define QUARTER 0.5522847f

static int path_cubic(float *o, float x1, float y1, float x2, float y2, float x, float y)
{
    o[0] = MEGLER_GFX_PATH_CUBIC;
    o[1] = x1;
    o[2] = y1;
    o[3] = x2;
    o[4] = y2;
    o[5] = x;
    o[6] = y;
    return 7;
}

static int path_to(float *o, int verb, float x, float y)
{
    o[0] = (float)verb;
    o[1] = x;
    o[2] = y;
    return 3;
}

/* A rectangle with quarter ellipses of radii rx, ry for corners, as a path; rx = half the width and ry = half the height is the oval. */
static int path_round(float *o, float l, float t, float r, float b, float rx, float ry)
{
    float kx = rx * QUARTER, ky = ry * QUARTER;
    int n = 0;
    n += path_to(o + n, MEGLER_GFX_PATH_MOVE, l + rx, t);
    n += path_to(o + n, MEGLER_GFX_PATH_LINE, r - rx, t);
    n += path_cubic(o + n, r - rx + kx, t, r, t + ry - ky, r, t + ry);
    n += path_to(o + n, MEGLER_GFX_PATH_LINE, r, b - ry);
    n += path_cubic(o + n, r, b - ry + ky, r - rx + kx, b, r - rx, b);
    n += path_to(o + n, MEGLER_GFX_PATH_LINE, l + rx, b);
    n += path_cubic(o + n, l + rx - kx, b, l, b - ry + ky, l, b - ry);
    n += path_to(o + n, MEGLER_GFX_PATH_LINE, l, t + ry);
    n += path_cubic(o + n, l, t + ry - ky, l + rx - kx, t, l + rx, t);
    o[n++] = MEGLER_GFX_PATH_CLOSE;
    return n;
}

/*
 * The outline of the figures as figures to fill: a rectangle along every
 * side, half the width to each side of it, and half the width longer at an
 * end where another side follows (so that right angles come out whole) or
 * where the cap is not butt. Every rectangle goes round the same way, so
 * that filling them all by the winding rule is their union.
 */
static void points_stroke(const spoints *in, spoints *out, double half, int cap)
{
    i32 f, s, first = 0;
    for (f = 0; f < in->figures; first = in->end[f], f++) {
        i32 n = in->end[f] - first, sides = in->closed[f] ? n : n - 1;
        if (n < 2) continue;
        for (s = 0; s < sides; s++) {
            const double *a = in->xy + 2 * (size_t)(first + s), *b = in->xy + 2 * (size_t)(first + (s + 1) % n);
            double dx = b[0] - a[0], dy = b[1] - a[1], len = sqrt(dx * dx + dy * dy), ux, uy, nx, ny, before, after;
            if (!(len > 0)) continue;
            ux = dx / len;
            uy = dy / len;
            nx = -uy * half;
            ny = ux * half;
            before = in->closed[f] || s > 0 || cap != MEGLER_GFX_CAP_BUTT ? half : 0;
            after = in->closed[f] || s < sides - 1 || cap != MEGLER_GFX_CAP_BUTT ? half : 0;
            points_add(out, a[0] - ux * before + nx, a[1] - uy * before + ny);
            points_add(out, b[0] + ux * after + nx, b[1] + uy * after + ny);
            points_add(out, b[0] + ux * after - nx, b[1] + uy * after - ny);
            points_add(out, a[0] - ux * before - nx, a[1] - uy * before - ny);
            points_end(out, true);
        }
    }
}

typedef struct sedge {
    double x0, y0, x1, y1;
} sedge;

typedef struct scross {
    double x;
    int way;
} scross;

static bool mask_ready(scanvas *v)
{
    if (!v->mask) {
        v->mask = (u8 *)calloc((size_t)v->w, (size_t)v->h);
        v->box[0] = v->w;
        v->box[1] = v->h;
        v->box[2] = v->box[3] = 0;
    }
    return v->mask != NULL;
}

/* The pixels x0..x1 of row y are covered, as far as the clip lets them be. */
static void mask_span(scanvas *v, i32 y, i32 x0, i32 x1)
{
    const sstate *s = state(v);
    if (x0 < s->clip[0]) x0 = s->clip[0];
    if (x1 > s->clip[2]) x1 = s->clip[2];
    if (x0 >= x1 || y < s->clip[1] || y >= s->clip[3]) return;
    memset(v->mask + (size_t)y * (size_t)v->w + (size_t)x0, 1, (size_t)(x1 - x0));
    if (x0 < v->box[0]) v->box[0] = x0;
    if (x1 > v->box[2]) v->box[2] = x1;
    if (y < v->box[1]) v->box[1] = y;
    if (y + 1 > v->box[3]) v->box[3] = y + 1;
}

/*
 * The figures, every one taken as closed, through the canvas's matrix and
 * onto the mask: row by row, the places where the row through the pixels'
 * centres crosses a side, and the stretches between crossings that are
 * inside by the rule.
 */
static void mask_fill(scanvas *v, const spoints *p, int rule)
{
    const sstate *s = state(v);
    sedge *edges;
    scross *cross;
    double ymin = 0, ymax = 0;
    i32 f, k, first = 0, count = 0, y, y0, y1, i, j, n, winding;
    bool any = false;
    if (!p->n || !mask_ready(v)) return;
    edges = (sedge *)mg_alloc(sizeof(sedge) * (size_t)p->n);
    cross = (scross *)mg_alloc(sizeof(scross) * (size_t)p->n);
    for (f = 0; f < p->figures; first = p->end[f], f++) {
        n = p->end[f] - first;
        for (k = 0; k < n; k++) {
            sedge *e = &edges[count];
            map(s->m, p->xy[2 * (size_t)(first + k)], p->xy[2 * (size_t)(first + k) + 1], &e->x0, &e->y0);
            map(s->m, p->xy[2 * (size_t)(first + (k + 1) % n)], p->xy[2 * (size_t)(first + (k + 1) % n) + 1], &e->x1, &e->y1);
            if (!any || e->y0 < ymin) ymin = e->y0;
            if (!any || e->y0 > ymax) ymax = e->y0;
            any = true;
            /* A side along a row crosses none. */
            if (e->y0 != e->y1 && e->y0 == e->y0 && e->y1 == e->y1) count++;
        }
    }
    y0 = pixel_at(ceil(ymin - 0.5));
    y1 = pixel_at(ceil(ymax - 0.5));
    if (y0 < s->clip[1]) y0 = s->clip[1];
    if (y1 > s->clip[3]) y1 = s->clip[3];
    for (y = y0; y < y1; y++) {
        double at = (double)y + 0.5;
        n = 0;
        for (k = 0; k < count; k++) {
            const sedge *e = &edges[k];
            scross c;
            if ((e->y0 <= at) == (e->y1 <= at)) continue;
            c.x = e->x0 + (at - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
            c.way = e->y1 > e->y0 ? 1 : -1;
            for (j = n; j > 0 && cross[j - 1].x > c.x; j--) cross[j] = cross[j - 1];
            cross[j] = c;
            n++;
        }
        winding = 0;
        for (i = 0; i + 1 < n; i++) {
            winding += cross[i].way;
            if (rule == MEGLER_GFX_EVEN_ODD ? (i & 1) != 0 : winding == 0) continue;
            mask_span(v, y, pixel_at(ceil(cross[i].x - 0.5)), pixel_at(ceil(cross[i + 1].x - 0.5)));
        }
    }
    free(edges);
    free(cross);
}

/* What the mask covers gets the colour (premultiplied), by the blend mode; the mask is clear again afterwards. */
static void mask_paint(scanvas *v, u32 color, int blend)
{
    const sstate *s = state(v);
    i32 x, y;
    if (!v->mask) return;
    for (y = v->box[1]; y < v->box[3]; y++) {
        u8 *m = v->mask + (size_t)y * (size_t)v->w, *row = v->px + (size_t)y * (size_t)v->stride;
        for (x = v->box[0]; x < v->box[2]; x++) {
            if (!m[x]) continue;
            m[x] = 0;
            if (s->cuts && cut_out(s, x, y)) continue;
            mg_wr32(row + 4 * (size_t)x, blended(blend, color, mg_rd32(row + 4 * (size_t)x)));
        }
    }
    v->box[0] = v->w;
    v->box[1] = v->h;
    v->box[2] = v->box[3] = 0;
}

/* The colour a call draws with: the paint's, or the shader's with the paint's alpha and filter, and the layers' alpha over all of it. */
static u32 source_of(scanvas *v, const megler_gfx_paint *p, const sshader *shader)
{
    u32 c = premultiplied(p->color);
    if (shader) c = faded(shader->color, p->color >> 24);
    if (p->filter_mode >= 0) c = blended(p->filter_mode, premultiplied(p->filter_color), c);
    return faded(c, state(v)->alpha);
}

/* How wide a stroke is in the drawing's coordinates: the paint's width, or for a hairline what comes to a pixel. */
static double stroke_half(scanvas *v, const megler_gfx_paint *p)
{
    const float *m = state(v)->m;
    double det = fabs((double)m[0] * m[4] - (double)m[1] * m[3]);
    if (p->stroke_width > 0) return p->stroke_width / 2.0;
    return det > 0 ? 0.5 / sqrt(det) : 0.5;
}

static void draw_points(scanvas *v, spoints *p, int rule, const megler_gfx_paint *paint, void *shader)
{
    spoints outline;
    if (paint->style != MEGLER_GFX_STROKE) mask_fill(v, p, rule);
    if (paint->style != MEGLER_GFX_FILL) {
        memset(&outline, 0, sizeof(outline));
        points_stroke(p, &outline, stroke_half(v, paint), paint->cap);
        mask_fill(v, &outline, MEGLER_GFX_WINDING);
        points_free(&outline);
    }
    mask_paint(v, source_of(v, paint, (const sshader *)shader), paint->blend);
    points_free(p);
}

static MEGLER_CB void s_draw_rect(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader)
{
    spoints p;
    (void)user;
    memset(&p, 0, sizeof(p));
    points_rect(&p, left, top, right, bottom);
    draw_points((scanvas *)canvas, &p, MEGLER_GFX_WINDING, paint, shader);
}

static MEGLER_CB void s_draw_round_rect(void *user, void *canvas, float left, float top, float right, float bottom, float rx, float ry, const megler_gfx_paint *paint,
                              void *shader)
{
    float path[64];
    spoints p;
    (void)user;
    memset(&p, 0, sizeof(p));
    points_path(&p, path, path_round(path, left, top, right, bottom, rx, ry));
    draw_points((scanvas *)canvas, &p, MEGLER_GFX_WINDING, paint, shader);
}

static MEGLER_CB void s_draw_oval(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader)
{
    s_draw_round_rect(user, canvas, left, top, right, bottom, (right - left) / 2, (bottom - top) / 2, paint, shader);
}

static MEGLER_CB void s_draw_path(void *user, void *canvas, const float *path, int count, int fill, const megler_gfx_paint *paint, void *shader)
{
    spoints p;
    (void)user;
    memset(&p, 0, sizeof(p));
    points_path(&p, path, count);
    draw_points((scanvas *)canvas, &p, fill, paint, shader);
}

/* The clip becomes what of it is in the box of the path, or that box is cut out of it: see the top of the file. */
static MEGLER_CB void s_clip_path(void *user, void *canvas, const float *path, int count, int fill, int op)
{
    scanvas *v = (scanvas *)canvas;
    sstate *s = state(v);
    spoints p;
    double x, y, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    i32 i, box[4];
    (void)user;
    (void)fill;
    if (op != MEGLER_GFX_CLIP_INTERSECT && (s->m[1] != 0 || s->m[3] != 0 || s->cuts >= MAX_CUTS)) return;
    memset(&p, 0, sizeof(p));
    points_path(&p, path, count);
    for (i = 0; i < p.n; i++) {
        map(s->m, p.xy[2 * (size_t)i], p.xy[2 * (size_t)i + 1], &x, &y);
        if (!i || x < x0) x0 = x;
        if (!i || x > x1) x1 = x;
        if (!i || y < y0) y0 = y;
        if (!i || y > y1) y1 = y;
    }
    box[0] = pixel_at(ceil(x0 - 0.5));
    box[1] = pixel_at(ceil(y0 - 0.5));
    box[2] = pixel_at(ceil(x1 - 0.5));
    box[3] = pixel_at(ceil(y1 - 0.5));
    if (op == MEGLER_GFX_CLIP_INTERSECT) clip_to(s, box);
    else if (p.n) memcpy(s->cut[s->cuts++], box, sizeof(box));
    points_free(&p);
}

static MEGLER_CB void s_draw_color(void *user, void *canvas, uint32_t color, int blend)
{
    scanvas *v = (scanvas *)canvas;
    const sstate *s = state(v);
    u32 c = faded(premultiplied(color), s->alpha);
    i32 x, y;
    (void)user;
    for (y = s->clip[1]; y < s->clip[3]; y++) {
        u8 *row = v->px + (size_t)y * (size_t)v->stride;
        for (x = s->clip[0]; x < s->clip[2]; x++) {
            if (s->cuts && cut_out(s, x, y)) continue;
            mg_wr32(row + 4 * (size_t)x, blended(blend, c, mg_rd32(row + 4 * (size_t)x)));
        }
    }
}

/*
 * Every pixel whose centre the matrix takes from inside `dst` gets the pixel
 * of the image that the same place in `src` falls in: the nearest one, never
 * one outside `src`.
 */
static MEGLER_CB void s_draw_bitmap(void *user, void *canvas, const megler_gfx_image *image, const float *src, const float *dst, const megler_gfx_paint *paint)
{
    scanvas *v = (scanvas *)canvas;
    const sstate *s = state(v);
    const float *m = s->m;
    double det = (double)m[0] * m[4] - (double)m[1] * m[3], lx, ly, dx, dy;
    i32 box[4], x, y, sx, sy, sx0, sy0, sx1, sy1;
    u32 alpha = paint->color >> 24, filter = premultiplied(paint->filter_color), c;
    (void)user;
    if (!image || !image->pixels || !(fabs(det) > 1e-30) || !(dst[2] > dst[0]) || !(dst[3] > dst[1])) return;
    sx0 = pixel_at(floor(src[0]));
    sy0 = pixel_at(floor(src[1]));
    sx1 = pixel_at(ceil(src[2])) - 1;
    sy1 = pixel_at(ceil(src[3])) - 1;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > image->width - 1) sx1 = image->width - 1;
    if (sy1 > image->height - 1) sy1 = image->height - 1;
    if (sx1 < sx0 || sy1 < sy0) return;
    pixel_box(m, dst[0], dst[1], dst[2], dst[3], box);
    if (box[0] < s->clip[0]) box[0] = s->clip[0];
    if (box[1] < s->clip[1]) box[1] = s->clip[1];
    if (box[2] > s->clip[2]) box[2] = s->clip[2];
    if (box[3] > s->clip[3]) box[3] = s->clip[3];
    for (y = box[1]; y < box[3]; y++) {
        u8 *row = v->px + (size_t)y * (size_t)v->stride;
        for (x = box[0]; x < box[2]; x++) {
            dx = (double)x + 0.5 - m[2];
            dy = (double)y + 0.5 - m[5];
            lx = (m[4] * dx - m[1] * dy) / det;
            ly = (m[0] * dy - m[3] * dx) / det;
            if (!(lx >= dst[0] && lx < dst[2] && ly >= dst[1] && ly < dst[3])) continue;
            if (s->cuts && cut_out(s, x, y)) continue;
            sx = pixel_at(floor(src[0] + (lx - dst[0]) * ((double)src[2] - src[0]) / ((double)dst[2] - dst[0])));
            sy = pixel_at(floor(src[1] + (ly - dst[1]) * ((double)src[3] - src[1]) / ((double)dst[3] - dst[1])));
            if (sx < sx0) sx = sx0;
            if (sx > sx1) sx = sx1;
            if (sy < sy0) sy = sy0;
            if (sy > sy1) sy = sy1;
            c = faded(mg_rd32((const u8 *)image->pixels + (size_t)sy * (size_t)image->stride + 4 * (size_t)sx), alpha);
            if (paint->filter_mode >= 0) c = blended(paint->filter_mode, filter, c);
            mg_wr32(row + 4 * (size_t)x, blended(paint->blend, faded(c, s->alpha), mg_rd32(row + 4 * (size_t)x)));
        }
    }
}

/* ------------------------------------------------------------------- text */

static float advance_of(const megler_gfx_paint *p) { return (0.6f + p->letter_spacing) * p->text_size * p->text_scale_x; }
static float box_width(const megler_gfx_paint *p) { return 0.5f * p->text_size * p->text_scale_x; }
static bool inked(u16 unit) { return unit > 0x20 && unit != 0xA0; }

static MEGLER_CB float s_text_measure(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface)
{
    (void)user;
    (void)text;
    (void)typeface;
    return count > 0 ? (float)count * advance_of(paint) : 0;
}

static MEGLER_CB float s_text_metrics(void *user, const megler_gfx_paint *paint, void *typeface, float *out)
{
    float up = 0.8f * paint->text_size, down = 0.2f * paint->text_size;
    (void)user;
    (void)typeface;
    if (out) {
        out[0] = out[1] = -up;
        out[2] = out[3] = down;
        out[4] = 0;
    }
    return up + down;
}

static MEGLER_CB void s_text_bounds(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface, float *out)
{
    int i, first = -1, last = -1;
    (void)user;
    (void)typeface;
    for (i = 0; i < count; i++)
        if (inked(text[i])) {
            if (first < 0) first = i;
            last = i;
        }
    out[0] = out[1] = out[2] = out[3] = 0;
    if (first < 0) return;
    out[0] = (float)first * advance_of(paint);
    out[1] = -0.7f * paint->text_size;
    out[2] = (float)last * advance_of(paint) + box_width(paint);
}

static bool spacing(u16 unit) { return unit == ' ' || unit == '\t' || unit == '\n'; }

static MEGLER_CB int s_text_break(void *user, const uint16_t *text, int count, float max_width, const megler_gfx_paint *paint, void *typeface, bool words)
{
    float advance = advance_of(paint);
    int line = count, fit = 0, i, kept, best = -1;
    (void)user;
    (void)typeface;
    /* The line is over at a line feed, which belongs to it. */
    for (i = 0; i < count; i++)
        if (text[i] == '\n') {
            line = i + 1;
            break;
        }
    while (fit < line && (float)(fit + 1) * advance <= max_width) fit++;
    if (!words) return fit;
    /* A line may end after spaces or at the end; the spaces it ends with do not count against the width. */
    for (i = 1; i <= line; i++) {
        if (i < line && !(spacing(text[i - 1]) && !spacing(text[i]))) continue;
        kept = i;
        while (kept > 0 && spacing(text[kept - 1])) kept--;
        if ((float)kept * advance <= max_width) best = i;
    }
    return best >= 0 ? best : fit;
}

static MEGLER_CB void s_text_widths(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface, float *out)
{
    int i;
    (void)user;
    (void)text;
    (void)typeface;
    for (i = 0; i < count; i++) out[i] = advance_of(paint);
}

static MEGLER_CB void s_draw_text(void *user, void *canvas, const uint16_t *text, int count, float x, float y, const megler_gfx_paint *paint, void *typeface,
                        void *shader)
{
    scanvas *v = (scanvas *)canvas;
    float advance = advance_of(paint), width = (float)count * advance;
    spoints p;
    int i;
    (void)user;
    (void)typeface;
    if (paint->text_align == MEGLER_GFX_ALIGN_CENTER) x -= width / 2;
    else if (paint->text_align == MEGLER_GFX_ALIGN_RIGHT) x -= width;
    memset(&p, 0, sizeof(p));
    for (i = 0; i < count; i++)
        if (inked(text[i])) points_rect(&p, x + (float)i * advance, y - 0.7f * paint->text_size, x + (float)i * advance + box_width(paint), y);
    mask_fill(v, &p, MEGLER_GFX_WINDING);
    mask_paint(v, source_of(v, paint, (const sshader *)shader), paint->blend);
    points_free(&p);
}

/* The glyphs' outlines: each box that s_draw_text fills, as a closed figure of four lines, clockwise on the screen. */
static MEGLER_CB bool s_text_path(void *user, const uint16_t *text, int count, float x, float y, const megler_gfx_paint *paint, void *typeface,
                                  megler_gfx_write write, void *sink)
{
    float advance = advance_of(paint), width = (float)count * advance;
    int i;
    (void)user;
    (void)typeface;
    if (paint->text_align == MEGLER_GFX_ALIGN_CENTER) x -= width / 2;
    else if (paint->text_align == MEGLER_GFX_ALIGN_RIGHT) x -= width;
    for (i = 0; i < count; i++) {
        float l = x + (float)i * advance, r = l + box_width(paint), t = y - 0.7f * paint->text_size;
        float figure[] = { MEGLER_GFX_PATH_MOVE, l, t, MEGLER_GFX_PATH_LINE, r, t, MEGLER_GFX_PATH_LINE, r, y, MEGLER_GFX_PATH_LINE, l, y, MEGLER_GFX_PATH_CLOSE };
        if (inked(text[i]) && !write(sink, figure, sizeof(figure))) return false;
    }
    return true;
}

/* ---------------------------------------------------- typefaces, shaders */

typedef struct sface {
    int weight;
    bool italic;
} sface;

static void *face_new(int weight, bool italic)
{
    sface *f = (sface *)mg_alloc(sizeof(*f));
    f->weight = weight;
    f->italic = italic;
    return f;
}

static MEGLER_CB void *s_typeface_create(void *user, const char *family, int generic, int weight, bool italic)
{
    (void)user;
    (void)family;
    (void)generic;
    return face_new(weight, italic);
}

static MEGLER_CB void *s_typeface_derive(void *user, void *typeface, int weight, bool italic)
{
    (void)user;
    (void)typeface;
    return face_new(weight, italic);
}

/* A font file is known by how it begins: TrueType, OpenType with CFF, a collection. */
static MEGLER_CB void *s_typeface_from_data(void *user, const void *data, size_t size)
{
    static const char *const magic[] = { "\0\1\0\0", "OTTO", "true", "ttcf" };
    size_t i;
    (void)user;
    if (!data || size < 12) return NULL;
    for (i = 0; i < MG_COUNT(magic); i++)
        if (memcmp(data, magic[i], 4) == 0) return face_new(400, false);
    return NULL;
}

static void *shader_new(u32 premultiplied_color)
{
    sshader *s = (sshader *)mg_alloc(sizeof(*s));
    s->color = premultiplied_color;
    return s;
}

static MEGLER_CB void *s_shader_linear(void *user, float x0, float y0, float x1, float y1, const uint32_t *colors, const float *positions, int count, int tile)
{
    (void)user;
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)positions;
    (void)tile;
    return count > 0 ? shader_new(premultiplied(colors[0])) : NULL;
}

static MEGLER_CB void *s_shader_radial(void *user, float cx, float cy, float radius, const uint32_t *colors, const float *positions, int count, int tile)
{
    (void)radius;
    return s_shader_linear(user, cx, cy, 0, 0, colors, positions, count, tile);
}

static MEGLER_CB void *s_shader_sweep(void *user, float cx, float cy, const uint32_t *colors, const float *positions, int count)
{
    return s_shader_linear(user, cx, cy, 0, 0, colors, positions, count, 0);
}

static MEGLER_CB void *s_shader_bitmap(void *user, const megler_gfx_image *image, int tile)
{
    (void)user;
    (void)tile;
    return image && image->pixels && image->width > 0 && image->height > 0 ? shader_new(mg_rd32(image->pixels)) : NULL;
}

static MEGLER_CB void s_shader_matrix(void *user, void *shader, const float *matrix)
{
    (void)user;
    (void)shader;
    (void)matrix;
}

static MEGLER_CB void s_shader_free(void *user, void *shader)
{
    (void)user;
    free(shader);
}

/* -------------------------------------------------------------------- PNG */

static const u8 k_png[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };

typedef struct spng {
    u32 w, h, depth, type, channels;
    const u8 *palette, *transparent;
    u32 colors, ntransparent;
} spng;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3]; }

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* The file's first chunk: what kind of picture it is, and whether it is one this reads. */
static bool png_header(const u8 *d, size_t n, spng *p)
{
    static const u8 channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
    memset(p, 0, sizeof(*p));
    if (!d || n < 33 || memcmp(d, k_png, 8) != 0 || be32(d + 8) != 13 || memcmp(d + 12, "IHDR", 4) != 0) return false;
    p->w = be32(d + 16);
    p->h = be32(d + 20);
    p->depth = d[24];
    p->type = d[25];
    /* d[26], d[27]: the only compression and filter methods there are; d[28]: interlaced, which is not read. */
    if (d[26] || d[27] || d[28] || p->type > 6 || !channels[p->type]) return false;
    if (p->depth != 8 && p->depth != 16 && !((p->type == 0 || p->type == 3) && (p->depth == 1 || p->depth == 2 || p->depth == 4))) return false;
    if (p->type == 3 && p->depth == 16) return false;
    /* A limit on the size, so that no count of bytes below can outgrow a 32-bit size. */
    if (!p->w || !p->h || p->w > 32767 || p->h > 32767 || (u64)p->w * p->h > ((u64)1 << 26)) return false;
    p->channels = channels[p->type];
    return true;
}

/* One sample of a row: `index` counts samples, of `depth` bits each, the first in the highest bits. */
static u32 png_sample(const u8 *row, u32 index, u32 depth)
{
    u32 bit;
    if (depth == 8) return row[index];
    if (depth == 16) return (u32)row[2 * index] << 8 | row[2 * index + 1];
    bit = index * depth;
    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
}

/* The picture as R, G, B, A bytes, not premultiplied, top row first; NULL when the file is damaged or not one this reads. */
static u8 *png_read(const u8 *d, size_t n, spng *p)
{
    u8 *packed = NULL, *raw = NULL, *out = NULL, *o;
    size_t at = 8, npacked = 0, line, total;
    uLongf got;
    u32 x, y, i, bpp, key[3] = { 0, 0, 0 }, top;
    bool keyed = false, ok = false;
    if (!png_header(d, n, p)) return NULL;
    while (at + 12 <= n) {
        u32 len = be32(d + at);
        const u8 *type = d + at + 4, *data = d + at + 8;
        if (len > n - at - 12) goto done;
        if (memcmp(type, "IDAT", 4) == 0) {
            packed = (u8 *)mg_realloc(packed, npacked + len + 1);
            memcpy(packed + npacked, data, len);
            npacked += len;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            p->palette = data;
            p->colors = len / 3;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            p->transparent = data;
            p->ntransparent = len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        at += 12 + (size_t)len;
    }
    if (!packed || (p->type == 3 && !p->colors)) goto done;
    line = ((size_t)p->w * p->channels * p->depth + 7) / 8;
    total = (line + 1) * p->h;
    raw = (u8 *)malloc(total);
    out = (u8 *)malloc((size_t)p->w * p->h * 4);
    if (!raw || !out) goto done;
    got = (uLongf)total;
    if (uncompress(raw, &got, packed, (uLong)npacked) != Z_OK || got != total) goto done;
    /* Each row was stored as its difference from what is to its left and above, in one of five ways. */
    bpp = p->channels * p->depth / 8;
    if (!bpp) bpp = 1;
    for (y = 0; y < p->h; y++) {
        u8 *row = raw + (size_t)y * (line + 1) + 1, how = row[-1];
        const u8 *up = y ? row - (line + 1) : NULL;
        if (how > 4) goto done;
        for (i = 0; i < line; i++) {
            int a = i >= bpp ? row[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= bpp ? up[i - bpp] : 0, pa, pb, pc, guess = 0;
            if (how == 1) guess = a;
            else if (how == 2) guess = b;
            else if (how == 3) guess = (a + b) / 2;
            else if (how == 4) {
                pa = abs(b - c);
                pb = abs(a - c);
                pc = abs(a + b - 2 * c);
                guess = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            row[i] = (u8)(row[i] + guess);
        }
    }
    /* One colour can be named as transparent for pictures without an alpha channel or a palette. */
    if (p->transparent && p->type == 0 && p->ntransparent >= 2) {
        key[0] = (u32)p->transparent[0] << 8 | p->transparent[1];
        keyed = true;
    } else if (p->transparent && p->type == 2 && p->ntransparent >= 6) {
        for (i = 0; i < 3; i++) key[i] = (u32)p->transparent[2 * i] << 8 | p->transparent[2 * i + 1];
        keyed = true;
    }
    top = p->depth == 16 ? 8 : 0;
    for (y = 0; y < p->h; y++) {
        const u8 *row = raw + (size_t)y * (line + 1) + 1;
        o = out + (size_t)y * p->w * 4;
        for (x = 0; x < p->w; x++, o += 4) {
            u32 v[4] = { 0, 0, 0, 255 };
            if (p->type == 3) {
                u32 index = png_sample(row, x, p->depth);
                if (index < p->colors) {
                    v[0] = p->palette[3 * index];
                    v[1] = p->palette[3 * index + 1];
                    v[2] = p->palette[3 * index + 2];
                }
                if (p->transparent && index < p->ntransparent) v[3] = p->transparent[index];
            } else if (p->type == 0 || p->type == 4) {
                u32 grey = png_sample(row, x * p->channels, p->depth);
                if (keyed && grey == key[0]) v[3] = 0;
                v[0] = v[1] = v[2] = p->depth < 8 ? grey * 255 / ((1u << p->depth) - 1) : grey >> top;
                if (p->type == 4) v[3] = png_sample(row, x * 2 + 1, p->depth) >> top;
            } else {
                for (i = 0; i < p->channels; i++) v[i] = png_sample(row, x * p->channels + i, p->depth);
                if (keyed && v[0] == key[0] && v[1] == key[1] && v[2] == key[2]) v[3] = 0;
                for (i = 0; i < p->channels; i++) v[i] >>= top;
            }
            for (i = 0; i < 4; i++) o[i] = (u8)v[i];
        }
    }
    ok = true;
done:
    free(packed);
    free(raw);
    if (!ok) {
        free(out);
        out = NULL;
    }
    return out;
}

static MEGLER_CB bool s_image_size(void *user, const void *data, size_t size, int *width, int *height)
{
    spng p;
    (void)user;
    if (!png_header((const u8 *)data, size, &p)) return false;
    *width = (int)p.w;
    *height = (int)p.h;
    return true;
}

static MEGLER_CB bool s_image_decode(void *user, const void *data, size_t size, void *pixels, int width, int height, int stride)
{
    spng p;
    u8 *rgba;
    int x, y;
    (void)user;
    if (!pixels || width <= 0 || height <= 0) return false;
    rgba = png_read((const u8 *)data, size, &p);
    if (!rgba) return false;
    for (y = 0; y < height; y++) {
        const u8 *row = rgba + (size_t)(((i64)(2 * y + 1) * p.h) / (2 * (i64)height)) * p.w * 4;
        u8 *out = (u8 *)pixels + (size_t)y * (size_t)stride;
        for (x = 0; x < width; x++) {
            const u8 *in = row + (size_t)(((i64)(2 * x + 1) * p.w) / (2 * (i64)width)) * 4;
            mg_wr32(out + 4 * (size_t)x, premultiplied((u32)in[3] << 24 | (u32)in[0] << 16 | (u32)in[1] << 8 | in[2]));
        }
    }
    free(rgba);
    return true;
}

/* A chunk into a file being put together: its length, its name, its bytes, and the checksum of the last two. */
static size_t png_chunk(u8 *out, const char *type, const u8 *data, u32 len)
{
    put32(out, len);
    memcpy(out + 4, type, 4);
    if (len) memcpy(out + 8, data, len);
    /* (zlib's crc32 takes a null pointer as "start again", so a chunk without bytes passes one that is not.) */
    put32(out + 8 + len, (u32)crc32(crc32(0, (const Bytef *)type, 4), data ? data : (const Bytef *)type, len));
    return 12 + (size_t)len;
}

static MEGLER_CB bool s_image_encode(void *user, const void *pixels, int width, int height, int stride, int format, int quality, megler_gfx_write write, void *sink)
{
    size_t line = (size_t)width * 4 + 1, nraw = line * (size_t)height, at;
    u8 *raw, *packed = NULL, *out = NULL, head[13];
    uLongf npacked;
    bool ok = false;
    int x, y;
    (void)user;
    (void)format;
    (void)quality;
    if (!pixels || width <= 0 || height <= 0 || !write) return false;
    raw = (u8 *)malloc(nraw);
    if (!raw) return false;
    for (y = 0; y < height; y++) {
        u8 *row = raw + (size_t)y * line;
        *row++ = 0;
        for (x = 0; x < width; x++, row += 4) {
            u32 c = unpremultiplied(mg_rd32((const u8 *)pixels + (size_t)y * (size_t)stride + 4 * (size_t)x));
            row[0] = (u8)(c >> 16);
            row[1] = (u8)(c >> 8);
            row[2] = (u8)c;
            row[3] = (u8)(c >> 24);
        }
    }
    npacked = compressBound((uLong)nraw);
    packed = (u8 *)malloc(npacked);
    out = packed ? (u8 *)malloc(8 + 25 + 12 + npacked + 12) : NULL;
    if (out && compress(packed, &npacked, raw, (uLong)nraw) == Z_OK) {
        memcpy(out, k_png, 8);
        put32(head, (u32)width);
        put32(head + 4, (u32)height);
        head[8] = 8;                   /* bits a sample */
        head[9] = 6;                   /* colour type: red, green, blue, alpha */
        head[10] = head[11] = head[12] = 0;
        at = 8;
        at += png_chunk(out + at, "IHDR", head, 13);
        at += png_chunk(out + at, "IDAT", packed, (u32)npacked);
        at += png_chunk(out + at, "IEND", NULL, 0);
        ok = write(sink, out, at);
    }
    free(raw);
    free(packed);
    free(out);
    return ok;
}

static MEGLER_CB void s_bitmap_gone(void *user, uint32_t id)
{
    (void)user;
    (void)id;
}

/* ------------------------------------------------------------------- BMP */

static void put16le(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32le(u8 *p, u32 v)
{
    put16le(p, v);
    put16le(p + 2, v >> 16);
}

bool mg_gfx_write_bmp(const char *host_path, const void *pixels, int width, int height, int stride)
{
    size_t line, size;
    u8 *out, *row;
    bool ok;
    int x, y;
    if (!host_path || !pixels || width <= 0 || height <= 0) return false;
    /* Rows are padded to four bytes and stored bottom row first, which is the form everything reads. */
    line = ((size_t)width * 3 + 3) & ~(size_t)3;
    size = 54 + line * (size_t)height;
    out = (u8 *)calloc(1, size);
    if (!out) return false;
    out[0] = 'B';
    out[1] = 'M';
    put32le(out + 2, (u32)size);
    put32le(out + 10, 54);
    put32le(out + 14, 40);
    put32le(out + 18, (u32)width);
    put32le(out + 22, (u32)height);
    put16le(out + 26, 1);
    put16le(out + 28, 24);
    put32le(out + 34, (u32)(line * (size_t)height));
    put32le(out + 38, 2835);           /* 72 dots an inch, in dots a metre */
    put32le(out + 42, 2835);
    for (y = 0; y < height; y++) {
        row = out + 54 + line * (size_t)(height - 1 - y);
        for (x = 0; x < width; x++) {
            u32 c = unpremultiplied(mg_rd32((const u8 *)pixels + (size_t)y * (size_t)stride + 4 * (size_t)x));
            row[3 * x] = (u8)c;
            row[3 * x + 1] = (u8)(c >> 8);
            row[3 * x + 2] = (u8)(c >> 16);
        }
    }
    ok = mg_file_write_all(host_path, out, size);
    free(out);
    return ok;
}

bool mg_gfx_standin_pixels(void *canvas, const void **pixels, int *width, int *height, int *stride)
{
    scanvas *v = (scanvas *)canvas;
    if (!v) return false;
    *pixels = v->px;
    *width = v->w;
    *height = v->h;
    *stride = v->stride;
    return true;
}

/* ---------------------------------------------------------------- the host */

const megler_gfx_host *mg_gfx_standin(void)
{
    static const megler_gfx_host host = {
        .canvas_new = s_canvas_new,
        .canvas_free = s_canvas_free,
        .flush = s_nothing,
        .touched = s_nothing,
        .save = s_save,
        .save_layer = s_save_layer,
        .restore = s_restore,
        .set_matrix = s_set_matrix,
        .clip_rect = s_clip_rect,
        .clip_path = s_clip_path,
        .draw_color = s_draw_color,
        .draw_rect = s_draw_rect,
        .draw_round_rect = s_draw_round_rect,
        .draw_oval = s_draw_oval,
        .draw_path = s_draw_path,
        .draw_bitmap = s_draw_bitmap,
        .draw_text = s_draw_text,
        .text_measure = s_text_measure,
        .text_metrics = s_text_metrics,
        .text_bounds = s_text_bounds,
        .text_break = s_text_break,
        .text_widths = s_text_widths,
        .text_path = s_text_path,
        .typeface_create = s_typeface_create,
        .typeface_derive = s_typeface_derive,
        .typeface_from_data = s_typeface_from_data,
        .shader_linear = s_shader_linear,
        .shader_radial = s_shader_radial,
        .shader_sweep = s_shader_sweep,
        .shader_bitmap = s_shader_bitmap,
        .shader_matrix = s_shader_matrix,
        .shader_free = s_shader_free,
        .image_size = s_image_size,
        .image_decode = s_image_decode,
        .image_encode = s_image_encode,
        .bitmap_gone = s_bitmap_gone,
    };
    return &host;
}
