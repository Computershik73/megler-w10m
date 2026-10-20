/*
 * Drawing an app's icon from its drawables (hk_icon.h).
 *
 * The drawables are the APK's compiled XML (binary XML, as the manifest is), read into a small tree here; their values
 * are resolved through the resource tables (hk_icon_src). Everything is drawn onto one canvas of premultiplied floats:
 * a path's inside (non-zero or even-odd) is found for each pixel by eight lines sampled within it, each line's
 * crossings to the fraction of a pixel; a stroke is drawn as the inside of quadrilaterals along it, with its joins and
 * caps; a clip path's inside is a mask the rest of its group is drawn through. Colours are straight RGBA until they are
 * blended. The canvas ends as straight RGBA bytes, and a PNG of them.
 *
 * What is not drawn: a vector's tint, trimmed paths, a ring shape, nine-patch stretching, and anything themed
 * (?attr/...): an icon is drawn without the app's theme.
 */
#include "hk_icon.h"

#include "../webp/hk_webp.h"
#include "zlib.h"

#include <math.h>
#include <stdlib.h>

#define PI_F 3.14159265358979f

/* The attributes read, by their resource ids (the SDK's public.xml). */
#define A_NAME 0x01010003u
#define A_SRC 0x01010119u
#define A_TINT 0x01010121u
#define A_HEIGHT 0x01010155u
#define A_WIDTH 0x01010159u
#define A_DRAWABLE 0x01010199u
#define A_SHAPE 0x0101019au
#define A_START_COLOR 0x0101019du
#define A_END_COLOR 0x0101019eu
#define A_ANGLE 0x010101a0u
#define A_TYPE 0x010101a1u
#define A_CENTER_X 0x010101a2u
#define A_CENTER_Y 0x010101a3u
#define A_GRADIENT_RADIUS 0x010101a4u
#define A_COLOR 0x010101a5u
#define A_RADIUS 0x010101a8u
#define A_TOP_LEFT_RADIUS 0x010101a9u
#define A_TOP_RIGHT_RADIUS 0x010101aau
#define A_BOTTOM_LEFT_RADIUS 0x010101abu
#define A_BOTTOM_RIGHT_RADIUS 0x010101acu
#define A_LEFT 0x010101adu
#define A_TOP 0x010101aeu
#define A_RIGHT 0x010101afu
#define A_BOTTOM 0x010101b0u
#define A_PIVOT_X 0x010101b5u
#define A_PIVOT_Y 0x010101b6u
#define A_INSET_LEFT 0x010101b7u
#define A_INSET_RIGHT 0x010101b8u
#define A_INSET_TOP 0x010101b9u
#define A_INSET_BOTTOM 0x010101bau
#define A_CENTER_COLOR 0x0101020bu
#define A_ALPHA 0x0101031fu
#define A_SCALE_X 0x01010324u
#define A_SCALE_Y 0x01010325u
#define A_ROTATION 0x01010326u
#define A_VIEWPORT_WIDTH 0x01010402u
#define A_VIEWPORT_HEIGHT 0x01010403u
#define A_FILL_COLOR 0x01010404u
#define A_PATH_DATA 0x01010405u
#define A_STROKE_COLOR 0x01010406u
#define A_STROKE_WIDTH 0x01010407u
#define A_STROKE_LINE_CAP 0x0101040bu
#define A_STROKE_LINE_JOIN 0x0101040cu
#define A_STROKE_MITER_LIMIT 0x0101040du
#define A_TRANSLATE_X 0x0101045au
#define A_TRANSLATE_Y 0x0101045bu
#define A_INSET 0x010104b5u
#define A_STROKE_ALPHA 0x010104cbu
#define A_FILL_ALPHA 0x010104ccu
#define A_START_X 0x01010510u
#define A_START_Y 0x01010511u
#define A_END_X 0x01010512u
#define A_END_Y 0x01010513u
#define A_OFFSET 0x01010514u
#define A_FILL_TYPE 0x0101051eu
#define A_TILE_MODE 0x01010201u

/* ------------------------------------------------------------------ binary XML */

typedef struct xattr {
    u32 id;           /* the attribute's resource id; 0 when it has none */
    char name[40];
    u8 type;          /* Res_value's */
    u32 data;
    char *text;       /* a string's (path data can be long); NULL for anything else */
} xattr;

typedef struct xnode {
    char tag[48];
    xattr *attrs;
    int nattrs;
    struct xnode *child, *next, *parent;
} xnode;

/* String `index` of a string pool chunk as UTF-8, malloc'd; NULL when there is no such string. */
static char *pool_string(const u8 *p, u32 psize, u32 index)
{
    u32 count, start, off, hsize, flags;
    const u8 *s, *end;
    char *out;
    if (!p || psize < 28) return NULL;
    hsize = hk_rd16(p + 2);
    count = hk_rd32(p + 8);
    flags = hk_rd32(p + 16);
    start = hk_rd32(p + 20);
    if (index >= count || hsize + (u64)count * 4 > psize || start > psize) return NULL;
    off = hk_rd32(p + hsize + index * 4);
    if (off >= psize - start) return NULL;
    s = p + start + off;
    end = p + psize;
    if (flags & 0x100) {
        /* UTF-8: its length in UTF-16 units, then in bytes, each in one or two bytes. */
        u32 len;
        if (s >= end) return NULL;
        if (*s++ & 0x80) s++;
        if (s >= end) return NULL;
        len = *s++;
        if (len & 0x80) {
            if (s >= end) return NULL;
            len = ((len & 0x7F) << 8) | *s++;
        }
        if (len > (u32)(end - s)) return NULL;
        out = (char *)hk_alloc(len + 1);
        memcpy(out, s, len);
        return out;
    } else {
        u32 len, i, n = 0;
        if (s + 2 > end) return NULL;
        len = hk_rd16(s);
        s += 2;
        if (len & 0x8000) {
            if (s + 2 > end) return NULL;
            len = ((len & 0x7FFF) << 16) | hk_rd16(s);
            s += 2;
        }
        if (len > (u32)(end - s) / 2) return NULL;
        out = (char *)hk_alloc((size_t)len * 3 + 1);
        for (i = 0; i < len; i++) {
            u32 c = hk_rd16(s + i * 2);
            /* (Names and path data are ASCII; anything else is kept a unit at a time.) */
            if (c < 0x80) {
                out[n++] = (char)c;
            } else if (c < 0x800) {
                out[n++] = (char)(0xC0 | (c >> 6));
                out[n++] = (char)(0x80 | (c & 0x3F));
            } else {
                out[n++] = (char)(0xE0 | (c >> 12));
                out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
        }
        return out;
    }
}

static void xml_free(xnode *n)
{
    while (n) {
        xnode *next = n->next;
        int i;
        xml_free(n->child);
        for (i = 0; i < n->nattrs; i++) free(n->attrs[i].text);
        free(n->attrs);
        free(n);
        n = next;
    }
}

/* A compiled XML file as a tree of its elements; NULL when it is none. */
static xnode *xml_parse(const u8 *xml, size_t size)
{
    const u8 *pool = NULL, *resmap = NULL;
    u32 pool_size = 0, resmap_count = 0, off;
    xnode *root = NULL, *cur = NULL;
    if (!xml || size < 8 || hk_rd16(xml) != 0x0003) return NULL;
    off = hk_rd16(xml + 2);
    while (off + 8 <= size) {
        const u8 *ch = xml + off;
        u32 type = hk_rd16(ch), head = hk_rd16(ch + 2), csize = hk_rd32(ch + 4);
        if (csize < 8 || csize > size - off || head > csize) break;
        if (type == 0x0001) {
            pool = ch;
            pool_size = csize;
        } else if (type == 0x0180) {
            resmap = ch + head;
            resmap_count = (csize - head) / 4;
        } else if (type == 0x0102 && csize >= 36) {
            u32 astart = hk_rd16(ch + 24), asize = hk_rd16(ch + 26), acount = hk_rd16(ch + 28), i;
            xnode *n = (xnode *)hk_alloc(sizeof(*n));
            char *tag = pool_string(pool, pool_size, hk_rd32(ch + 20));
            if (tag) hk_strlcpy(n->tag, tag, sizeof(n->tag));
            free(tag);
            n->attrs = (xattr *)hk_alloc(sizeof(xattr) * (acount ? acount : 1));
            for (i = 0; i < acount && asize >= 20; i++) {
                const u8 *a = ch + 16 + astart + i * asize;
                xattr *out = &n->attrs[n->nattrs];
                u32 name_index;
                char *name;
                if (a + 20 > ch + csize) break;
                name_index = hk_rd32(a + 4);
                name = pool_string(pool, pool_size, name_index);
                if (name) hk_strlcpy(out->name, name, sizeof(out->name));
                free(name);
                if (name_index < resmap_count) out->id = hk_rd32(resmap + name_index * 4);
                out->type = a[15];
                out->data = hk_rd32(a + 16);
                if (out->type == 0x03) out->text = pool_string(pool, pool_size, out->data);
                n->nattrs++;
            }
            n->parent = cur;
            if (cur) {
                xnode **at = &cur->child;
                while (*at) at = &(*at)->next;
                *at = n;
            } else if (!root) {
                root = n;
            } else {
                xml_free(n); /* (a second root: nothing) */
                n = NULL;
            }
            if (n) cur = n;
        } else if (type == 0x0103) {
            if (cur) cur = cur->parent;
        }
        off += csize;
    }
    return root;
}

static const xattr *attr_of(const xnode *n, u32 id)
{
    int i;
    for (i = 0; n && i < n->nattrs; i++)
        if (n->attrs[i].id == id) return &n->attrs[i];
    return NULL;
}

static const xnode *child_named(const xnode *n, const char *tag)
{
    const xnode *c;
    for (c = n ? n->child : NULL; c; c = c->next)
        if (!strcmp(c->tag, tag)) return c;
    return NULL;
}

/* ------------------------------------------------------------------ values */

typedef struct ctx {
    const hk_icon_src *src;
    int w, h;
    float *px;        /* premultiplied R, G, B, A for each pixel */
    float *clip;      /* the current clip's coverage for each pixel; NULL: none */
    int depth;        /* drawables within drawables (a loop of references is cut short) */
} ctx;

typedef struct val {
    u8 type;
    u32 data;
    char text[256];
} val;

static bool ends_with(const char *s, const char *tail)
{
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && !strcmp(s + n - k, tail);
}

/* A value with its references followed to what they name. False for none, a reference to nothing or a theme's. */
static bool resolve(const ctx *c, u8 type, u32 data, const char *text, val *v)
{
    int i;
    v->type = type;
    v->data = data;
    v->text[0] = 0;
    if (text) hk_strlcpy(v->text, text, sizeof(v->text));
    for (i = 0; i < 8 && v->type == 0x01; i++) {
        u32 id = v->data;
        v->text[0] = 0;
        if (!id || !c->src->value(c->src->user, id, &v->type, &v->data, v->text, sizeof(v->text))) return false;
    }
    return v->type != 0x00 && v->type != 0x01 && v->type != 0x02;
}

static bool attr_val(const ctx *c, const xnode *n, u32 id, val *v)
{
    const xattr *a = attr_of(n, id);
    return a && resolve(c, a->type, a->data, a->text, v);
}

static float complex_float(u32 data)
{
    static const float mults[4] = { 1.0f / (1 << 8), 1.0f / (1 << 15), 1.0f / (1 << 23), 1.0f / 2147483648.0f };
    return (float)(i32)(data & 0xFFFFFF00u) * mults[(data >> 4) & 3];
}

static float float_of(u32 data)
{
    float f;
    memcpy(&f, &data, 4);
    return f;
}

/* A number: a float or an integer as it is, a dimension at `dp` pixels a dp (one in px as it is), a fraction of
   `whole`. */
static float number(const ctx *c, const xnode *n, u32 id, float dflt, float dp, float whole)
{
    val v;
    if (!attr_val(c, n, id, &v)) return dflt;
    switch (v.type) {
    case 0x04: return float_of(v.data);
    case 0x05: return (v.data & 0xF) == 0 ? complex_float(v.data) : complex_float(v.data) * dp;
    case 0x06: return complex_float(v.data) * whole;
    case 0x10: case 0x11: return (float)(i32)v.data;
    case 0x12: return v.data ? 1.0f : 0.0f;
    default: return dflt;
    }
}

static bool argb_of(const val *v, u32 *argb)
{
    u32 d = v->data;
    switch (v->type) {
    case 0x1c: *argb = d; return true;
    case 0x1d: *argb = 0xFF000000u | d; return true;
    case 0x1e: *argb = ((d >> 12 & 15) * 0x11u) << 24 | ((d >> 8 & 15) * 0x11u) << 16 | ((d >> 4 & 15) * 0x11u) << 8 | (d & 15) * 0x11u; return true;
    case 0x1f: *argb = 0xFF000000u | ((d >> 8 & 15) * 0x11u) << 16 | ((d >> 4 & 15) * 0x11u) << 8 | (d & 15) * 0x11u; return true;
    }
    return false;
}

static void rgba_of(u32 argb, float out[4])
{
    out[0] = (float)((argb >> 16) & 255) / 255.0f;
    out[1] = (float)((argb >> 8) & 255) / 255.0f;
    out[2] = (float)(argb & 255) / 255.0f;
    out[3] = (float)(argb >> 24) / 255.0f;
}

static xnode *read_xml(const ctx *c, const char *path)
{
    size_t size = 0;
    u8 *file = c->src->read(c->src->user, path, &size);
    xnode *x = file ? xml_parse(file, size) : NULL;
    free(file);
    return x;
}

/* A colour: one given as such, or a colour state list's (its last item, the one for no state, as a rule). */
static bool color_of(const ctx *c, const val *v, u32 *argb, int depth)
{
    xnode *x;
    const xnode *item, *last = NULL;
    bool ok = false;
    if (argb_of(v, argb)) return true;
    if (v->type != 0x03 || !ends_with(v->text, ".xml") || depth > 4) return false;
    x = read_xml(c, v->text);
    if (x && !strcmp(x->tag, "selector")) {
        for (item = x->child; item; item = item->next)
            if (!strcmp(item->tag, "item")) last = item;
        if (last) {
            val cv;
            if (attr_val(c, last, A_COLOR, &cv) && color_of(c, &cv, argb, depth + 1)) {
                float alpha = number(c, last, A_ALPHA, 1.0f, 1.0f, 1.0f);
                u32 a = (u32)((float)(*argb >> 24) * (alpha < 0 ? 0 : alpha > 1 ? 1 : alpha) + 0.5f);
                *argb = (*argb & 0x00FFFFFFu) | a << 24;
                ok = true;
            }
        }
    }
    xml_free(x);
    return ok;
}

/* ------------------------------------------------------------------ geometry */

typedef struct mat {
    float a, b, c, d, e, f;   /* x' = a x + c y + e, y' = b x + d y + f */
} mat;

static mat mat_mul(mat p, mat q)   /* q, then p */
{
    mat r;
    r.a = p.a * q.a + p.c * q.b;
    r.b = p.b * q.a + p.d * q.b;
    r.c = p.a * q.c + p.c * q.d;
    r.d = p.b * q.c + p.d * q.d;
    r.e = p.a * q.e + p.c * q.f + p.e;
    r.f = p.b * q.e + p.d * q.f + p.f;
    return r;
}

static mat mat_make(float a, float b, float c, float d, float e, float f)
{
    mat m;
    m.a = a; m.b = b; m.c = c; m.d = d; m.e = e; m.f = f;
    return m;
}

static float mat_scale_of(mat m) { return sqrtf(fabsf(m.a * m.d - m.b * m.c)); }

typedef struct pt {
    float x, y;
} pt;

static pt apply(mat m, float x, float y)
{
    pt p;
    p.x = m.a * x + m.c * y + m.e;
    p.y = m.b * x + m.d * y + m.f;
    return p;
}

/* Contours of points on the canvas. */
typedef struct shape {
    pt *pts;
    int npts, cpts;
    int *start;       /* each contour's first point */
    u8 *closed;
    int nc, cc;
} shape;

static void sh_free(shape *s)
{
    free(s->pts);
    free(s->start);
    free(s->closed);
    memset(s, 0, sizeof(*s));
}

static void sh_move(shape *s, pt p)
{
    if (s->nc == s->cc) {
        s->cc = s->cc ? s->cc * 2 : 8;
        s->start = (int *)hk_realloc(s->start, sizeof(int) * (size_t)s->cc);
        s->closed = (u8 *)hk_realloc(s->closed, (size_t)s->cc);
    }
    s->start[s->nc] = s->npts;
    s->closed[s->nc] = 0;
    s->nc++;
    if (s->npts == s->cpts) {
        s->cpts = s->cpts ? s->cpts * 2 : 64;
        s->pts = (pt *)hk_realloc(s->pts, sizeof(pt) * (size_t)s->cpts);
    }
    s->pts[s->npts++] = p;
}

static void sh_line(shape *s, pt p)
{
    if (!s->nc) {
        sh_move(s, p);
        return;
    }
    if (s->npts == s->cpts) {
        s->cpts = s->cpts * 2;
        s->pts = (pt *)hk_realloc(s->pts, sizeof(pt) * (size_t)s->cpts);
    }
    s->pts[s->npts++] = p;
}

static int contour_end(const shape *s, int k) { return k + 1 < s->nc ? s->start[k + 1] : s->npts; }

/* A cubic from the last point, as lines: enough of them that each is a few pixels at most. */
static void sh_cubic(shape *s, pt p1, pt p2, pt p3)
{
    pt p0 = s->npts ? s->pts[s->npts - 1] : p1;
    float len = hypotf(p1.x - p0.x, p1.y - p0.y) + hypotf(p2.x - p1.x, p2.y - p1.y) + hypotf(p3.x - p2.x, p3.y - p2.y);
    int n = (int)ceilf(len / 2.0f), i;
    if (n < 2) n = 2;
    if (n > 96) n = 96;
    for (i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1 - t;
        pt q;
        q.x = u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x;
        q.y = u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y;
        sh_line(s, q);
    }
}

/* SVG's elliptical arc (its implementation notes, F.6.5) from (x1, y1) to (x2, y2), in the path's own units. */
static void sh_arc(shape *s, mat m, float x1, float y1, float rx, float ry, float rotation, int large, int sweep, float x2, float y2)
{
    float phi = rotation * PI_F / 180.0f, cs = cosf(phi), sn = sinf(phi);
    float dx = (x1 - x2) / 2, dy = (y1 - y2) / 2, x1p = cs * dx + sn * dy, y1p = -sn * dx + cs * dy;
    float lambda, num, den, coef, cxp, cyp, cx, cy, t1, dt, ux, uy, vx, vy;
    int n, i;
    rx = fabsf(rx);
    ry = fabsf(ry);
    if (rx < 1e-6f || ry < 1e-6f || (x1 == x2 && y1 == y2)) {
        sh_line(s, apply(m, x2, y2));
        return;
    }
    lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lambda > 1) {
        rx *= sqrtf(lambda);
        ry *= sqrtf(lambda);
    }
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    coef = den > 0 && num > 0 ? sqrtf(num / den) : 0;
    if (large == sweep) coef = -coef;
    cxp = coef * rx * y1p / ry;
    cyp = -coef * ry * x1p / rx;
    cx = cs * cxp - sn * cyp + (x1 + x2) / 2;
    cy = sn * cxp + cs * cyp + (y1 + y2) / 2;
    ux = (x1p - cxp) / rx;
    uy = (y1p - cyp) / ry;
    vx = (-x1p - cxp) / rx;
    vy = (-y1p - cyp) / ry;
    t1 = atan2f(uy, ux);
    dt = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && dt > 0) dt -= 2 * PI_F;
    else if (sweep && dt < 0) dt += 2 * PI_F;
    n = (int)ceilf(fabsf(dt) * (rx > ry ? rx : ry) * mat_scale_of(m) / 2.0f);
    if (n < 4) n = 4;
    if (n > 128) n = 128;
    for (i = 1; i <= n; i++) {
        float t = t1 + dt * (float)i / (float)n;
        float x = cx + rx * cs * cosf(t) - ry * sn * sinf(t), y = cy + rx * sn * cosf(t) + ry * cs * sinf(t);
        sh_line(s, i == n ? apply(m, x2, y2) : apply(m, x, y));
    }
}

static const char *skip(const char *p)
{
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* A number of path data, as SVG writes them ("-1.5e-2", ".5.5" being two); without the C library's locale. */
static bool read_num(const char **ps, float *out)
{
    const char *p = skip(*ps);
    double v = 0, frac = 0.1;
    int sign = 1, digits = 0;
    if (*p == '-' || *p == '+') sign = *p++ == '-' ? -1 : 1;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        digits++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            v += (*p++ - '0') * frac;
            frac /= 10;
            digits++;
        }
    }
    if (!digits) return false;
    if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
        int esign = 1, e = 0;
        p++;
        if (*p == '-' || *p == '+') esign = *p++ == '-' ? -1 : 1;
        while (*p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
        v *= pow(10.0, esign * e);
    }
    *out = (float)(sign * v);
    *ps = p;
    return true;
}

static bool read_flag(const char **ps, int *f)
{
    const char *p = skip(*ps);
    if (*p != '0' && *p != '1') return false;
    *f = *p - '0';
    *ps = p + 1;
    return true;
}

/* Path data (SVG's syntax, as VectorDrawable takes it) into contours on the canvas through `m`. */
static void path_data(const char *d, mat m, shape *s)
{
    const char *p = d;
    char cmd = 0, prev = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0, kx = 0, ky = 0;   /* the current point, the contour's start, the last control point */
    bool open = false;                                        /* a contour is begun */
    if (!d) return;
    for (;;) {
        float a[7];
        int f1 = 0, f2 = 0;
        bool rel;
        p = skip(p);
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;
            if (cmd == 'z' || cmd == 'Z') {
                if (open) s->closed[s->nc - 1] = 1;
                cx = sx;
                cy = sy;
                open = false;
                prev = 'z';
                continue;
            }
        } else if (!cmd || cmd == 'z' || cmd == 'Z') {
            break;
        }
        rel = cmd >= 'a';
        /* After a close, a drawing command begins a contour where the last one began. */
        if (!open && cmd != 'm' && cmd != 'M') {
            sh_move(s, apply(m, cx, cy));
            open = true;
        }
        switch (cmd) {
        case 'M': case 'm':
            if (!read_num(&p, &a[0]) || !read_num(&p, &a[1])) return;
            cx = (rel ? cx : 0) + a[0];
            cy = (rel ? cy : 0) + a[1];
            sx = cx;
            sy = cy;
            sh_move(s, apply(m, cx, cy));
            open = true;
            cmd = rel ? 'l' : 'L';
            break;
        case 'L': case 'l':
            if (!read_num(&p, &a[0]) || !read_num(&p, &a[1])) return;
            cx = (rel ? cx : 0) + a[0];
            cy = (rel ? cy : 0) + a[1];
            sh_line(s, apply(m, cx, cy));
            break;
        case 'H': case 'h':
            if (!read_num(&p, &a[0])) return;
            cx = (rel ? cx : 0) + a[0];
            sh_line(s, apply(m, cx, cy));
            break;
        case 'V': case 'v':
            if (!read_num(&p, &a[0])) return;
            cy = (rel ? cy : 0) + a[0];
            sh_line(s, apply(m, cx, cy));
            break;
        case 'C': case 'c': case 'S': case 's': {
            float x1, y1, x2, y2, x, y;
            bool smooth = cmd == 'S' || cmd == 's';
            int i, need = smooth ? 4 : 6;
            for (i = 0; i < need; i++)
                if (!read_num(&p, &a[i])) return;
            if (smooth) {
                bool after = prev == 'C' || prev == 'c' || prev == 'S' || prev == 's';
                x1 = after ? 2 * cx - kx : cx;
                y1 = after ? 2 * cy - ky : cy;
                x2 = (rel ? cx : 0) + a[0];
                y2 = (rel ? cy : 0) + a[1];
                x = (rel ? cx : 0) + a[2];
                y = (rel ? cy : 0) + a[3];
            } else {
                x1 = (rel ? cx : 0) + a[0];
                y1 = (rel ? cy : 0) + a[1];
                x2 = (rel ? cx : 0) + a[2];
                y2 = (rel ? cy : 0) + a[3];
                x = (rel ? cx : 0) + a[4];
                y = (rel ? cy : 0) + a[5];
            }
            sh_cubic(s, apply(m, x1, y1), apply(m, x2, y2), apply(m, x, y));
            kx = x2;
            ky = y2;
            cx = x;
            cy = y;
            break;
        }
        case 'Q': case 'q': case 'T': case 't': {
            float qx, qy, x, y;
            bool smooth = cmd == 'T' || cmd == 't';
            int i, need = smooth ? 2 : 4;
            for (i = 0; i < need; i++)
                if (!read_num(&p, &a[i])) return;
            if (smooth) {
                bool after = prev == 'Q' || prev == 'q' || prev == 'T' || prev == 't';
                qx = after ? 2 * cx - kx : cx;
                qy = after ? 2 * cy - ky : cy;
                x = (rel ? cx : 0) + a[0];
                y = (rel ? cy : 0) + a[1];
            } else {
                qx = (rel ? cx : 0) + a[0];
                qy = (rel ? cy : 0) + a[1];
                x = (rel ? cx : 0) + a[2];
                y = (rel ? cy : 0) + a[3];
            }
            /* As a cubic: its control points two thirds of the way to the quadratic's. */
            sh_cubic(s, apply(m, cx + 2.0f / 3 * (qx - cx), cy + 2.0f / 3 * (qy - cy)), apply(m, x + 2.0f / 3 * (qx - x), y + 2.0f / 3 * (qy - y)),
                     apply(m, x, y));
            kx = qx;
            ky = qy;
            cx = x;
            cy = y;
            break;
        }
        case 'A': case 'a': {
            float x, y;
            if (!read_num(&p, &a[0]) || !read_num(&p, &a[1]) || !read_num(&p, &a[2]) || !read_flag(&p, &f1) || !read_flag(&p, &f2) ||
                !read_num(&p, &a[3]) || !read_num(&p, &a[4]))
                return;
            x = (rel ? cx : 0) + a[3];
            y = (rel ? cy : 0) + a[4];
            sh_arc(s, m, cx, cy, a[0], a[1], a[2], f1, f2, x, y);
            cx = x;
            cy = y;
            break;
        }
        default:
            return;
        }
        prev = cmd;
    }
}

/* A rectangle with rounded corners (radii top left, top right, bottom right, bottom left), as one closed contour. */
static void sh_round_rect(shape *s, float x, float y, float w, float h, const float r[4])
{
    static const int steps = 12;
    float cx[4], cy[4], rr[4], start[4];
    int k, i;
    for (k = 0; k < 4; k++) {
        rr[k] = r[k] < 0 ? 0 : r[k];
        if (rr[k] > w / 2) rr[k] = w / 2;
        if (rr[k] > h / 2) rr[k] = h / 2;
    }
    cx[0] = x + rr[0]; cy[0] = y + rr[0]; start[0] = PI_F;
    cx[1] = x + w - rr[1]; cy[1] = y + rr[1]; start[1] = 1.5f * PI_F;
    cx[2] = x + w - rr[2]; cy[2] = y + h - rr[2]; start[2] = 0;
    cx[3] = x + rr[3]; cy[3] = y + h - rr[3]; start[3] = 0.5f * PI_F;
    for (k = 0; k < 4; k++)
        for (i = 0; i <= steps; i++) {
            float t = start[k] + 0.5f * PI_F * (float)i / steps;
            pt p;
            p.x = cx[k] + rr[k] * cosf(t);
            p.y = cy[k] + rr[k] * sinf(t);
            if (k == 0 && i == 0) sh_move(s, p);
            else sh_line(s, p);
        }
    s->closed[s->nc - 1] = 1;
}

static void sh_oval(shape *s, float x, float y, float w, float h)
{
    int n = 96, i;
    for (i = 0; i < n; i++) {
        float t = 2 * PI_F * (float)i / (float)n;
        pt p;
        p.x = x + w / 2 + w / 2 * cosf(t);
        p.y = y + h / 2 + h / 2 * sinf(t);
        if (!i) sh_move(s, p);
        else sh_line(s, p);
    }
    s->closed[s->nc - 1] = 1;
}

/* ------------------------------------------------------------------ the inside of a shape */

typedef struct edge {
    float x0, y0, x1, y1;   /* y0 < y1 */
    int dir;
} edge;

typedef struct cross {
    float x;
    int dir;
} cross;

static int by_x(const void *a, const void *b)
{
    float d = ((const cross *)a)->x - ((const cross *)b)->x;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

#define SAMPLES 8

/* How much of each pixel is inside the shape, 0 to 1, by the non-zero rule or the even-odd one. */
static float *coverage(const ctx *c, const shape *s, bool even_odd)
{
    float *cov = (float *)hk_alloc(sizeof(float) * (size_t)c->w * (size_t)c->h);
    edge *edges = (edge *)hk_alloc(sizeof(edge) * (size_t)(s->npts + 1));
    cross *xs = (cross *)hk_alloc(sizeof(cross) * (size_t)(s->npts + 1));
    int ne = 0, k, y, i;
    float ymin = 1e30f, ymax = -1e30f;
    for (k = 0; k < s->nc; k++) {
        int a = s->start[k], b = contour_end(s, k), j;
        for (j = a; j < b; j++) {
            pt p = s->pts[j], q = s->pts[j + 1 < b ? j + 1 : a];
            edge *e = &edges[ne];
            if (p.y == q.y) continue;
            if (p.y < q.y) {
                e->x0 = p.x; e->y0 = p.y; e->x1 = q.x; e->y1 = q.y; e->dir = 1;
            } else {
                e->x0 = q.x; e->y0 = q.y; e->x1 = p.x; e->y1 = p.y; e->dir = -1;
            }
            if (e->y0 < ymin) ymin = e->y0;
            if (e->y1 > ymax) ymax = e->y1;
            ne++;
        }
    }
    for (y = ymin < 0 ? 0 : (int)ymin; ne && y < c->h && y <= (int)ymax; y++) {
        float *row = cov + (size_t)y * c->w;
        int sub;
        for (sub = 0; sub < SAMPLES; sub++) {
            float sy = (float)y + ((float)sub + 0.5f) / SAMPLES;
            int n = 0, wind = 0;
            for (i = 0; i < ne; i++) {
                const edge *e = &edges[i];
                if (sy < e->y0 || sy >= e->y1) continue;
                xs[n].x = e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                xs[n].dir = e->dir;
                n++;
            }
            if (n < 2) continue;
            qsort(xs, (size_t)n, sizeof(cross), by_x);
            for (i = 0; i + 1 < n; i++) {
                bool inside;
                float xa, xb;
                int ia, ib, x;
                wind += xs[i].dir;
                inside = even_odd ? (wind & 1) != 0 : wind != 0;
                if (!inside) continue;
                xa = xs[i].x < 0 ? 0 : xs[i].x;
                xb = xs[i + 1].x > (float)c->w ? (float)c->w : xs[i + 1].x;
                if (xb <= xa) continue;
                ia = (int)xa;
                ib = (int)xb;
                if (ia == ib) {
                    row[ia] += (xb - xa) / SAMPLES;
                    continue;
                }
                row[ia] += ((float)(ia + 1) - xa) / SAMPLES;
                for (x = ia + 1; x < ib; x++) row[x] += 1.0f / SAMPLES;
                if (ib < c->w) row[ib] += (xb - (float)ib) / SAMPLES;
            }
        }
    }
    for (i = 0; i < c->w * c->h; i++)
        if (cov[i] > 1) cov[i] = 1;
    free(edges);
    free(xs);
    return cov;
}

/* ------------------------------------------------------------------ paints */

typedef struct stop {
    float at;
    float c[4];   /* straight */
} stop;

typedef struct paint {
    int kind;                    /* 0 nothing, 1 a colour, 2 linear, 3 radial, 4 sweep */
    float c[4];                  /* a colour's, straight */
    float x0, y0, x1, y1, r;     /* a gradient's, on the canvas */
    stop stops[16];
    int nstops;
    int tile;                    /* 0 clamp, 1 repeat, 2 mirror */
} paint;

static void add_stop(paint *p, float at, u32 argb)
{
    if (p->nstops >= (int)HK_COUNT(p->stops)) return;
    p->stops[p->nstops].at = at;
    rgba_of(argb, p->stops[p->nstops].c);
    p->nstops++;
}

static void paint_at(const paint *p, float x, float y, float out[4])
{
    float t;
    int i;
    if (p->kind == 1) {
        memcpy(out, p->c, sizeof(float) * 4);
        return;
    }
    if (p->kind == 2) {
        float dx = p->x1 - p->x0, dy = p->y1 - p->y0, len2 = dx * dx + dy * dy;
        t = len2 > 0 ? ((x - p->x0) * dx + (y - p->y0) * dy) / len2 : 0;
    } else if (p->kind == 3) {
        t = p->r > 0 ? hypotf(x - p->x0, y - p->y0) / p->r : 0;
    } else {
        t = atan2f(y - p->y0, x - p->x0) / (2 * PI_F);
        if (t < 0) t += 1;
    }
    if (p->tile == 1) t -= floorf(t);
    else if (p->tile == 2) {
        t = fmodf(fabsf(t), 2.0f);
        if (t > 1) t = 2 - t;
    }
    if (t <= p->stops[0].at) {
        memcpy(out, p->stops[0].c, sizeof(float) * 4);
        return;
    }
    for (i = 0; i + 1 < p->nstops; i++) {
        const stop *a = &p->stops[i], *b = &p->stops[i + 1];
        if (t <= b->at) {
            float u = b->at > a->at ? (t - a->at) / (b->at - a->at) : 0;
            int k;
            for (k = 0; k < 4; k++) out[k] = a->c[k] + (b->c[k] - a->c[k]) * u;
            return;
        }
    }
    memcpy(out, p->stops[p->nstops - 1].c, sizeof(float) * 4);
}

/* A vector's gradient (an <aapt:attr> of a fill or stroke, compiled into a file of its own): its points in the
   vector's units, through `m`. */
static bool vector_gradient(const ctx *c, const xnode *g, mat m, paint *p)
{
    const xnode *item;
    int type = (int)number(c, g, A_TYPE, 0, 1, 1);
    p->kind = type == 1 ? 3 : type == 2 ? 4 : 2;
    p->tile = (int)number(c, g, A_TILE_MODE, 0, 1, 1);
    if (p->tile < 0 || p->tile > 2) p->tile = 0;
    if (p->kind == 2) {
        pt a = apply(m, number(c, g, A_START_X, 0, 1, 1), number(c, g, A_START_Y, 0, 1, 1));
        pt b = apply(m, number(c, g, A_END_X, 0, 1, 1), number(c, g, A_END_Y, 0, 1, 1));
        p->x0 = a.x; p->y0 = a.y; p->x1 = b.x; p->y1 = b.y;
    } else {
        pt o = apply(m, number(c, g, A_CENTER_X, 0, 1, 1), number(c, g, A_CENTER_Y, 0, 1, 1));
        p->x0 = o.x;
        p->y0 = o.y;
        p->r = number(c, g, A_GRADIENT_RADIUS, 0, 1, 1) * mat_scale_of(m);
    }
    for (item = g->child; item; item = item->next) {
        val v;
        u32 argb;
        if (strcmp(item->tag, "item") || !attr_val(c, item, A_COLOR, &v) || !color_of(c, &v, &argb, 0)) continue;
        add_stop(p, number(c, item, A_OFFSET, 0, 1, 1), argb);
    }
    if (!p->nstops) {
        val v;
        u32 argb;
        if (attr_val(c, g, A_START_COLOR, &v) && color_of(c, &v, &argb, 0)) add_stop(p, 0, argb);
        if (attr_val(c, g, A_CENTER_COLOR, &v) && color_of(c, &v, &argb, 0)) add_stop(p, 0.5f, argb);
        if (attr_val(c, g, A_END_COLOR, &v) && color_of(c, &v, &argb, 0)) add_stop(p, 1, argb);
    }
    return p->nstops > 0;
}

/* A fill or stroke: a colour, a colour state list, or a gradient. */
static bool paint_of(const ctx *c, const val *v, mat m, paint *p)
{
    u32 argb;
    memset(p, 0, sizeof(*p));
    if (color_of(c, v, &argb, 0)) {
        p->kind = 1;
        rgba_of(argb, p->c);
        return p->c[3] > 0;
    }
    if (v->type == 0x03 && ends_with(v->text, ".xml")) {
        xnode *x = read_xml(c, v->text);
        bool ok = x && !strcmp(x->tag, "gradient") && vector_gradient(c, x, m, p);
        xml_free(x);
        return ok;
    }
    return false;
}

/* The paint over every pixel the coverage has, at `alpha`, through the clip. */
static void blend(ctx *c, const float *cov, const paint *p, float alpha)
{
    int x, y;
    for (y = 0; y < c->h; y++)
        for (x = 0; x < c->w; x++) {
            size_t i = (size_t)y * c->w + x;
            float col[4], a, *d;
            if (cov[i] <= 0) continue;
            a = cov[i] * alpha * (c->clip ? c->clip[i] : 1.0f);
            if (a <= 0) continue;
            paint_at(p, (float)x + 0.5f, (float)y + 0.5f, col);
            a *= col[3];
            d = c->px + i * 4;
            d[0] = col[0] * a + d[0] * (1 - a);
            d[1] = col[1] * a + d[1] * (1 - a);
            d[2] = col[2] * a + d[2] * (1 - a);
            d[3] = a + d[3] * (1 - a);
        }
}

static void fill_shape(ctx *c, const shape *s, bool even_odd, const paint *p, float alpha)
{
    float *cov;
    if (!s->nc || p->kind == 0 || alpha <= 0) return;
    cov = coverage(c, s, even_odd);
    blend(c, cov, p, alpha);
    free(cov);
}

/* ------------------------------------------------------------------ strokes */

static float area_of(const pt *p, int n)
{
    float a = 0;
    int i;
    for (i = 0; i < n; i++) a += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
    return a / 2;
}

/* A polygon added as a closed contour, turned the same way as every other (so that the non-zero rule joins them). */
static void add_polygon(shape *out, pt *p, int n)
{
    int i;
    if (n < 3) return;
    if (area_of(p, n) < 0)
        for (i = 0; i < n / 2; i++) {
            pt t = p[i];
            p[i] = p[n - 1 - i];
            p[n - 1 - i] = t;
        }
    sh_move(out, p[0]);
    for (i = 1; i < n; i++) sh_line(out, p[i]);
    out->closed[out->nc - 1] = 1;
}

static void add_disc(shape *out, pt o, float r)
{
    pt p[16];
    int i;
    for (i = 0; i < 16; i++) {
        p[i].x = o.x + r * cosf(2 * PI_F * (float)i / 16);
        p[i].y = o.y + r * sinf(2 * PI_F * (float)i / 16);
    }
    add_polygon(out, p, 16);
}

/* The outline of a stroke of `width` along the shape's contours, as polygons to fill by the non-zero rule: a
   quadrilateral for each line, and at each corner its join (round, miter within the limit, else bevel), at each open
   end its cap. */
static void stroke_outline(const shape *s, float width, int cap, int join, float miter, shape *out)
{
    float h = width / 2;
    int k;
    for (k = 0; k < s->nc; k++) {
        int a = s->start[k], b = contour_end(s, k), n = b - a, j, segs;
        bool closed = s->closed[k] != 0;
        if (n < 2) {
            if (n == 1 && cap == 1) add_disc(out, s->pts[a], h);
            continue;
        }
        segs = closed ? n : n - 1;
        for (j = 0; j < segs; j++) {
            pt p = s->pts[a + j], q = s->pts[a + (j + 1) % n], quad[4];
            float dx = q.x - p.x, dy = q.y - p.y, len = hypotf(dx, dy), nx, ny;
            if (len <= 0) continue;
            nx = -dy / len * h;
            ny = dx / len * h;
            quad[0].x = p.x + nx; quad[0].y = p.y + ny;
            quad[1].x = q.x + nx; quad[1].y = q.y + ny;
            quad[2].x = q.x - nx; quad[2].y = q.y - ny;
            quad[3].x = p.x - nx; quad[3].y = p.y - ny;
            add_polygon(out, quad, 4);
        }
        /* The joins: at every point but an open contour's two ends. */
        for (j = closed ? 0 : 1; j < (closed ? n : n - 1); j++) {
            pt o = s->pts[a + j], pin = s->pts[a + (j + n - 1) % n], pout = s->pts[a + (j + 1) % n];
            float ax = o.x - pin.x, ay = o.y - pin.y, bx = pout.x - o.x, by = pout.y - o.y;
            float la = hypotf(ax, ay), lb = hypotf(bx, by), turn;
            pt tri[4];
            if (la <= 0 || lb <= 0) continue;
            ax /= la; ay /= la; bx /= lb; by /= lb;
            if (join == 1) {
                add_disc(out, o, h);
                continue;
            }
            /* The outer side is the one the path turns away from. */
            turn = ax * by - ay * bx;
            if (fabsf(turn) < 1e-4f) continue;
            {
                float side = turn > 0 ? -1.0f : 1.0f;
                pt e1, e2;
                e1.x = o.x - ay * h * side; e1.y = o.y + ax * h * side;
                e2.x = o.x - by * h * side; e2.y = o.y + bx * h * side;
                tri[0] = o;
                tri[1] = e1;
                if (join == 0) {
                    /* The miter's tip, where the two outer edges meet, when it is within the limit. */
                    float mx = -(ay + by), my = ax + bx, ml = hypotf(mx, my), cosh;
                    cosh = ml / 2;
                    if (ml > 1e-4f && cosh > 0 && 1.0f / cosh <= miter) {
                        pt tip;
                        tip.x = o.x + mx / ml * h / cosh * side;
                        tip.y = o.y + my / ml * h / cosh * side;
                        tri[2] = tip;
                        tri[3] = e2;
                        add_polygon(out, tri, 4);
                        continue;
                    }
                }
                tri[2] = e2;
                add_polygon(out, tri, 3);
            }
        }
        if (!closed) {
            int e;
            for (e = 0; e < 2; e++) {
                pt o = s->pts[e ? b - 1 : a], other = s->pts[e ? b - 2 : a + 1];
                float dx = o.x - other.x, dy = o.y - other.y, len = hypotf(dx, dy);
                if (cap == 1) {
                    add_disc(out, o, h);
                } else if (cap == 2 && len > 0) {
                    pt sq[4];
                    float ux = dx / len * h, uy = dy / len * h;
                    sq[0].x = o.x - uy; sq[0].y = o.y + ux;
                    sq[1].x = o.x - uy + ux; sq[1].y = o.y + ux + uy;
                    sq[2].x = o.x + uy + ux; sq[2].y = o.y - ux + uy;
                    sq[3].x = o.x + uy; sq[3].y = o.y - ux;
                    add_polygon(out, sq, 4);
                }
            }
        }
    }
}

static void stroke_shape(ctx *c, const shape *s, float width, int cap, int join, float miter, const paint *p, float alpha)
{
    shape outline;
    if (width <= 0 || p->kind == 0 || alpha <= 0) return;
    memset(&outline, 0, sizeof(outline));
    stroke_outline(s, width, cap, join, miter, &outline);
    fill_shape(c, &outline, false, p, alpha);
    sh_free(&outline);
}

/* ------------------------------------------------------------------ pictures */

typedef struct image {
    int w, h;
    u8 *rgba;   /* straight */
} image;

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* A PNG, not interlaced, of any colour type and depth, as straight RGBA. */
static bool png_decode(const u8 *d, size_t n, image *img)
{
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    u32 w = 0, h = 0;
    int depth = 0, type = 0, interlace = 0, channels, y;
    u8 palette[256 * 4];
    int npal = 0;
    u8 *idat = NULL, *raw = NULL;
    size_t nidat = 0, off = 8, stride, bpp;
    uLongf rawlen;
    bool ok = false;
    memset(palette, 255, sizeof(palette));
    if (n < 8 || memcmp(d, sig, 8)) return false;
    while (off + 12 <= n) {
        u32 len = (u32)d[off] << 24 | (u32)d[off + 1] << 16 | (u32)d[off + 2] << 8 | d[off + 3];
        const u8 *t = d + off + 4, *data = d + off + 8;
        if (len > n - off - 12) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = (u32)data[0] << 24 | (u32)data[1] << 16 | (u32)data[2] << 8 | data[3];
            h = (u32)data[4] << 24 | (u32)data[5] << 16 | (u32)data[6] << 8 | data[7];
            depth = data[8];
            type = data[9];
            interlace = data[12];
        } else if (!memcmp(t, "PLTE", 4)) {
            u32 i;
            npal = (int)(len / 3 > 256 ? 256 : len / 3);
            for (i = 0; i < (u32)npal; i++) {
                palette[i * 4] = data[i * 3];
                palette[i * 4 + 1] = data[i * 3 + 1];
                palette[i * 4 + 2] = data[i * 3 + 2];
            }
        } else if (!memcmp(t, "tRNS", 4) && type == 3) {
            u32 i;
            for (i = 0; i < len && i < 256; i++) palette[i * 4 + 3] = data[i];
        } else if (!memcmp(t, "IDAT", 4)) {
            idat = (u8 *)hk_realloc(idat, nidat + len + 1);
            memcpy(idat + nidat, data, len);
            nidat += len;
        } else if (!memcmp(t, "IEND", 4)) {
            break;
        }
        off += (size_t)len + 12;
    }
    channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
    if (!w || !h || w > 4096 || h > 4096 || interlace || !channels || !idat || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16))
        goto done;
    stride = ((size_t)w * channels * depth + 7) / 8;
    bpp = ((size_t)channels * depth + 7) / 8;
    rawlen = (uLongf)((stride + 1) * h);
    raw = (u8 *)hk_alloc(rawlen);
    if (uncompress(raw, &rawlen, idat, (uLong)nidat) != Z_OK || rawlen != (stride + 1) * h) goto done;
    for (y = 0; y < (int)h; y++) {
        u8 *row = raw + (size_t)y * (stride + 1) + 1, *up = y ? row - stride - 1 : NULL;
        int f = row[-1];
        size_t i;
        for (i = 0; i < stride; i++) {
            int a = i >= bpp ? row[i - bpp] : 0, b = up ? up[i] : 0, cc = up && i >= bpp ? up[i - bpp] : 0;
            switch (f) {
            case 1: row[i] = (u8)(row[i] + a); break;
            case 2: row[i] = (u8)(row[i] + b); break;
            case 3: row[i] = (u8)(row[i] + ((a + b) >> 1)); break;
            case 4: row[i] = (u8)(row[i] + paeth(a, b, cc)); break;
            default: break;
            }
        }
    }
    img->w = (int)w;
    img->h = (int)h;
    img->rgba = (u8 *)hk_alloc((size_t)w * h * 4);
    for (y = 0; y < (int)h; y++) {
        const u8 *row = raw + (size_t)y * (stride + 1) + 1;
        u32 x;
        for (x = 0; x < w; x++) {
            u8 *o = img->rgba + ((size_t)y * w + x) * 4;
            int s[4], k;
            for (k = 0; k < channels; k++) {
                if (depth == 16) s[k] = row[(x * channels + k) * 2];
                else if (depth == 8) s[k] = row[x * channels + k];
                else {
                    size_t bit = (size_t)(x * channels + k) * depth;
                    int v = (row[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1);
                    s[k] = type == 3 ? v : v * 255 / ((1 << depth) - 1);
                }
            }
            switch (type) {
            case 0: o[0] = o[1] = o[2] = (u8)s[0]; o[3] = 255; break;
            case 2: o[0] = (u8)s[0]; o[1] = (u8)s[1]; o[2] = (u8)s[2]; o[3] = 255; break;
            case 3: memcpy(o, palette + (s[0] < npal ? s[0] : 0) * 4, 4); break;
            case 4: o[0] = o[1] = o[2] = (u8)s[0]; o[3] = (u8)s[1]; break;
            default: o[0] = (u8)s[0]; o[1] = (u8)s[1]; o[2] = (u8)s[2]; o[3] = (u8)s[3]; break;
            }
        }
    }
    ok = true;
done:
    free(idat);
    free(raw);
    return ok;
}

static bool image_read(const ctx *c, const char *path, image *img)
{
    size_t size = 0;
    u8 *file = c->src->read(c->src->user, path, &size);
    bool ok = false;
    int w = 0, h = 0;
    memset(img, 0, sizeof(*img));
    if (!file) return false;
    if (png_decode(file, size, img)) {
        ok = true;
    } else if (hk_webp_size(file, size, &w, &h) && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        /* The core's WebP decoder gives B, G, R, A premultiplied: made straight R, G, B, A. */
        size_t i;
        img->w = w;
        img->h = h;
        img->rgba = (u8 *)hk_alloc((size_t)w * h * 4);
        if (hk_webp_decode(file, size, img->rgba, w, h, w * 4)) {
            for (i = 0; i < (size_t)w * h; i++) {
                u8 *p = img->rgba + i * 4, b = p[0], a = p[3];
                p[0] = a ? (u8)(p[2] * 255 / a) : 0;
                p[1] = a ? (u8)(p[1] * 255 / a) : 0;
                p[2] = a ? (u8)(b * 255 / a) : 0;
            }
            ok = true;
        } else {
            free(img->rgba);
            img->rgba = NULL;
        }
    }
    free(file);
    return ok;
}

/* Halved, a box of four for each pixel, until it is at most twice the size it is drawn at: no picture loses its finer
   parts to sampling. */
static void image_shrink(image *img, float w, float h)
{
    while (img->w > 2 * w + 1 && img->h > 2 * h + 1 && img->w > 1 && img->h > 1) {
        int nw = img->w / 2, nh = img->h / 2, x, y, k;
        u8 *out = (u8 *)hk_alloc((size_t)nw * nh * 4);
        for (y = 0; y < nh; y++)
            for (x = 0; x < nw; x++) {
                const u8 *p[4] = { img->rgba + ((size_t)(2 * y) * img->w + 2 * x) * 4, img->rgba + ((size_t)(2 * y) * img->w + 2 * x + 1) * 4,
                                   img->rgba + ((size_t)(2 * y + 1) * img->w + 2 * x) * 4, img->rgba + ((size_t)(2 * y + 1) * img->w + 2 * x + 1) * 4 };
                int a = p[0][3] + p[1][3] + p[2][3] + p[3][3];
                u8 *o = out + ((size_t)y * nw + x) * 4;
                for (k = 0; k < 3; k++)
                    o[k] = a ? (u8)((p[0][k] * p[0][3] + p[1][k] * p[1][3] + p[2][k] * p[2][3] + p[3][k] * p[3][3]) / a) : 0;
                o[3] = (u8)(a / 4);
            }
        free(img->rgba);
        img->rgba = out;
        img->w = nw;
        img->h = nh;
    }
}

/* A picture into the rectangle, sampled between its pixels, through the clip. */
static void draw_image(ctx *c, image *img, float x, float y, float w, float h)
{
    int px, py;
    if (w <= 0 || h <= 0) return;
    image_shrink(img, w, h);
    for (py = y < 0 ? 0 : (int)y; py < c->h && py < y + h; py++)
        for (px = x < 0 ? 0 : (int)x; px < c->w && px < x + w; px++) {
            float u = ((float)px + 0.5f - x) / w * (float)img->w - 0.5f, v = ((float)py + 0.5f - y) / h * (float)img->h - 0.5f;
            int u0 = (int)floorf(u), v0 = (int)floorf(v), k, q;
            float fu = u - (float)u0, fv = v - (float)v0, col[4] = { 0, 0, 0, 0 }, a, *d;
            size_t i = (size_t)py * c->w + px;
            for (q = 0; q < 4; q++) {
                int sx = u0 + (q & 1), sy = v0 + (q >> 1);
                float wgt = ((q & 1) ? fu : 1 - fu) * ((q >> 1) ? fv : 1 - fv);
                const u8 *s;
                float sa;
                if (sx < 0) sx = 0;
                if (sy < 0) sy = 0;
                if (sx >= img->w) sx = img->w - 1;
                if (sy >= img->h) sy = img->h - 1;
                s = img->rgba + ((size_t)sy * img->w + sx) * 4;
                sa = (float)s[3] / 255.0f;
                for (k = 0; k < 3; k++) col[k] += (float)s[k] / 255.0f * sa * wgt;
                col[3] += sa * wgt;
            }
            a = c->clip ? c->clip[i] : 1.0f;
            d = c->px + i * 4;
            for (k = 0; k < 4; k++) col[k] *= a;
            d[0] = col[0] + d[0] * (1 - col[3]);
            d[1] = col[1] + d[1] * (1 - col[3]);
            d[2] = col[2] + d[2] * (1 - col[3]);
            d[3] = col[3] + d[3] * (1 - col[3]);
        }
}

/* ------------------------------------------------------------------ drawables */

static void draw_node(ctx *c, const xnode *n, float x, float y, float w, float h, float dp);

static void fill_rect(ctx *c, float x, float y, float w, float h, u32 argb)
{
    shape s;
    paint p;
    static const float none[4] = { 0, 0, 0, 0 };
    memset(&s, 0, sizeof(s));
    memset(&p, 0, sizeof(p));
    p.kind = 1;
    rgba_of(argb, p.c);
    sh_round_rect(&s, x, y, w, h, none);
    fill_shape(c, &s, false, &p, 1.0f);
    sh_free(&s);
}

/* A drawable given as a value: a colour, or a file (an XML, a picture). */
static void draw_value(ctx *c, const val *v, float x, float y, float w, float h, float dp)
{
    u32 argb;
    if (c->depth > 12) return;
    if (argb_of(v, &argb)) {
        fill_rect(c, x, y, w, h, argb);
        return;
    }
    if (v->type != 0x03 || !v->text[0]) return;
    c->depth++;
    if (ends_with(v->text, ".xml")) {
        xnode *x2 = read_xml(c, v->text);
        if (x2) draw_node(c, x2, x, y, w, h, dp);
        xml_free(x2);
    } else {
        image img;
        if (image_read(c, v->text, &img)) {
            draw_image(c, &img, x, y, w, h);
            free(img.rgba);
        }
    }
    c->depth--;
}

/* An element's drawable: its android:drawable, else the drawable written inside it. */
static void draw_inner(ctx *c, const xnode *n, float x, float y, float w, float h, float dp)
{
    val v;
    if (attr_val(c, n, A_DRAWABLE, &v)) draw_value(c, &v, x, y, w, h, dp);
    else if (n->child) draw_node(c, n->child, x, y, w, h, dp);
}

/* A vector's groups and paths, through `m` from its units to the canvas. A clip path is the mask of what follows it in
   its group. */
static void vector_walk(ctx *c, const xnode *parent, mat m, float alpha)
{
    const xnode *n;
    float *saved = c->clip, *own = NULL;
    for (n = parent->child; n; n = n->next) {
        if (!strcmp(n->tag, "group")) {
            float px = number(c, n, A_PIVOT_X, 0, 1, 1), py = number(c, n, A_PIVOT_Y, 0, 1, 1);
            float rot = number(c, n, A_ROTATION, 0, 1, 1) * PI_F / 180.0f;
            float sx = number(c, n, A_SCALE_X, 1, 1, 1), sy = number(c, n, A_SCALE_Y, 1, 1, 1);
            float tx = number(c, n, A_TRANSLATE_X, 0, 1, 1), ty = number(c, n, A_TRANSLATE_Y, 0, 1, 1);
            /* As Android's VGroup: to the pivot, scaled, rotated, back from it and moved. */
            mat local = mat_make(1, 0, 0, 1, -px, -py);
            local = mat_mul(mat_make(sx, 0, 0, sy, 0, 0), local);
            local = mat_mul(mat_make(cosf(rot), sinf(rot), -sinf(rot), cosf(rot), 0, 0), local);
            local = mat_mul(mat_make(1, 0, 0, 1, tx + px, ty + py), local);
            vector_walk(c, n, mat_mul(m, local), alpha);
        } else if (!strcmp(n->tag, "clip-path")) {
            const xattr *d = attr_of(n, A_PATH_DATA);
            shape s;
            float *cov;
            int i;
            memset(&s, 0, sizeof(s));
            path_data(d ? d->text : NULL, m, &s);
            cov = coverage(c, &s, false);
            sh_free(&s);
            if (!own) {
                own = cov;
                if (saved)
                    for (i = 0; i < c->w * c->h; i++) own[i] *= saved[i];
            } else {
                for (i = 0; i < c->w * c->h; i++) own[i] *= cov[i];
                free(cov);
            }
            c->clip = own;
        } else if (!strcmp(n->tag, "path")) {
            const xattr *d = attr_of(n, A_PATH_DATA);
            shape s;
            paint p;
            val v;
            memset(&s, 0, sizeof(s));
            path_data(d ? d->text : NULL, m, &s);
            if (attr_val(c, n, A_FILL_COLOR, &v) && paint_of(c, &v, m, &p))
                fill_shape(c, &s, number(c, n, A_FILL_TYPE, 0, 1, 1) == 1, &p, number(c, n, A_FILL_ALPHA, 1, 1, 1) * alpha);
            if (attr_val(c, n, A_STROKE_COLOR, &v) && paint_of(c, &v, m, &p))
                stroke_shape(c, &s, number(c, n, A_STROKE_WIDTH, 0, 1, 1) * mat_scale_of(m), (int)number(c, n, A_STROKE_LINE_CAP, 0, 1, 1),
                             (int)number(c, n, A_STROKE_LINE_JOIN, 0, 1, 1), number(c, n, A_STROKE_MITER_LIMIT, 4, 1, 1), &p,
                             number(c, n, A_STROKE_ALPHA, 1, 1, 1) * alpha);
            sh_free(&s);
        }
    }
    c->clip = saved;
    free(own);
}

/* A GradientDrawable's gradient over its bounds: its angle in steps of 45 (0 from the left, 90 from the bottom), its
   centre and radius as fractions or dimensions. */
static bool shape_gradient(const ctx *c, const xnode *g, float x, float y, float w, float h, float dp, paint *p)
{
    val v;
    u32 argb;
    int type = (int)number(c, g, A_TYPE, 0, 1, 1);
    float cx = x + w * number(c, g, A_CENTER_X, 0.5f, 1, 1), cy = y + h * number(c, g, A_CENTER_Y, 0.5f, 1, 1);
    memset(p, 0, sizeof(*p));
    p->kind = type == 1 ? 3 : type == 2 ? 4 : 2;
    if (p->kind == 2) {
        int angle = (((int)number(c, g, A_ANGLE, 0, 1, 1) % 360) + 360) % 360;
        float dx = angle == 90 || angle == 270 ? 0 : angle > 90 && angle < 270 ? -1.0f : 1.0f;
        float dy = angle == 0 || angle == 180 ? 0 : angle < 180 ? -1.0f : 1.0f;
        p->x0 = x + w / 2 - dx * w / 2;
        p->y0 = y + h / 2 - dy * h / 2;
        p->x1 = x + w / 2 + dx * w / 2;
        p->y1 = y + h / 2 + dy * h / 2;
    } else {
        p->x0 = cx;
        p->y0 = cy;
        p->r = number(c, g, A_GRADIENT_RADIUS, 0.5f * (w < h ? w : h), dp, w < h ? w : h);
    }
    if (attr_val(c, g, A_START_COLOR, &v) && color_of(c, &v, &argb, 0)) add_stop(p, 0, argb);
    if (attr_val(c, g, A_CENTER_COLOR, &v) && color_of(c, &v, &argb, 0)) add_stop(p, 0.5f, argb);
    if (attr_val(c, g, A_END_COLOR, &v) && color_of(c, &v, &argb, 0)) add_stop(p, 1, argb);
    return p->nstops > 0;
}

/* A GradientDrawable: a rectangle (its corners rounded or not), an oval or a line, filled with a colour or a gradient
   and stroked. */
static void draw_shape(ctx *c, const xnode *n, float x, float y, float w, float h, float dp)
{
    int kind = (int)number(c, n, A_SHAPE, 0, 1, 1);
    const xnode *solid = child_named(n, "solid"), *gradient = child_named(n, "gradient"), *corners = child_named(n, "corners"),
                *stroke = child_named(n, "stroke");
    shape s;
    paint p;
    val v;
    memset(&s, 0, sizeof(s));
    if (kind == 1) {
        sh_oval(&s, x, y, w, h);
    } else if (kind == 2) {
        pt a, b;
        a.x = x;
        a.y = y + h / 2;
        b.x = x + w;
        b.y = a.y;
        sh_move(&s, a);
        sh_line(&s, b);
    } else {
        float r = corners ? number(c, corners, A_RADIUS, 0, dp, 1) : 0, radii[4];
        radii[0] = corners ? number(c, corners, A_TOP_LEFT_RADIUS, r, dp, 1) : 0;
        radii[1] = corners ? number(c, corners, A_TOP_RIGHT_RADIUS, r, dp, 1) : 0;
        radii[2] = corners ? number(c, corners, A_BOTTOM_RIGHT_RADIUS, r, dp, 1) : 0;
        radii[3] = corners ? number(c, corners, A_BOTTOM_LEFT_RADIUS, r, dp, 1) : 0;
        sh_round_rect(&s, x, y, w, h, radii);
    }
    if (kind != 2) {
        if (gradient && shape_gradient(c, gradient, x, y, w, h, dp, &p)) fill_shape(c, &s, false, &p, 1.0f);
        else if (solid && attr_val(c, solid, A_COLOR, &v) && paint_of(c, &v, mat_make(1, 0, 0, 1, 0, 0), &p)) fill_shape(c, &s, false, &p, 1.0f);
    }
    if (stroke && attr_val(c, stroke, A_COLOR, &v) && paint_of(c, &v, mat_make(1, 0, 0, 1, 0, 0), &p))
        stroke_shape(c, &s, number(c, stroke, A_WIDTH, 0, dp, 1), 0, 0, 4, &p, 1.0f);
    sh_free(&s);
}

static void draw_node(ctx *c, const xnode *n, float x, float y, float w, float h, float dp)
{
    if (!n || c->depth > 12) return;
    c->depth++;
    if (!strcmp(n->tag, "vector")) {
        float vw = number(c, n, A_VIEWPORT_WIDTH, 0, 1, 1), vh = number(c, n, A_VIEWPORT_HEIGHT, 0, 1, 1);
        if (vw > 0 && vh > 0) vector_walk(c, n, mat_make(w / vw, 0, 0, h / vh, x, y), number(c, n, A_ALPHA, 1, 1, 1));
    } else if (!strcmp(n->tag, "adaptive-icon")) {
        /* Within another drawable: its layers over the bounds, as they are. */
        const xnode *b = child_named(n, "background"), *f = child_named(n, "foreground");
        if (b) draw_inner(c, b, x, y, w, h, dp);
        if (f) draw_inner(c, f, x, y, w, h, dp);
    } else if (!strcmp(n->tag, "layer-list")) {
        const xnode *item;
        for (item = n->child; item; item = item->next) {
            float l, t, r, bo, iw, ih, ix, iy, ww, hh;
            if (strcmp(item->tag, "item")) continue;
            l = number(c, item, A_LEFT, 0, dp, w);
            t = number(c, item, A_TOP, 0, dp, h);
            r = number(c, item, A_RIGHT, 0, dp, w);
            bo = number(c, item, A_BOTTOM, 0, dp, h);
            ix = x + l;
            iy = y + t;
            ww = w - l - r;
            hh = h - t - bo;
            /* A size of its own (API 23): in the middle of what the insets leave. */
            iw = number(c, item, A_WIDTH, -1, dp, w);
            ih = number(c, item, A_HEIGHT, -1, dp, h);
            if (iw > 0) {
                ix += (ww - iw) / 2;
                ww = iw;
            }
            if (ih > 0) {
                iy += (hh - ih) / 2;
                hh = ih;
            }
            draw_inner(c, item, ix, iy, ww, hh, dp);
        }
    } else if (!strcmp(n->tag, "inset")) {
        float all = number(c, n, A_INSET, 0, dp, w);
        float l = number(c, n, A_INSET_LEFT, all, dp, w), r = number(c, n, A_INSET_RIGHT, all, dp, w);
        float t = number(c, n, A_INSET_TOP, all, dp, h), b = number(c, n, A_INSET_BOTTOM, all, dp, h);
        draw_inner(c, n, x + l, y + t, w - l - r, h - t - b, dp);
    } else if (!strcmp(n->tag, "shape")) {
        draw_shape(c, n, x, y, w, h, dp);
    } else if (!strcmp(n->tag, "bitmap") || !strcmp(n->tag, "nine-patch")) {
        val v;
        if (attr_val(c, n, A_SRC, &v)) draw_value(c, &v, x, y, w, h, dp);
    } else if (!strcmp(n->tag, "color")) {
        val v;
        u32 argb;
        if (attr_val(c, n, A_COLOR, &v) && color_of(c, &v, &argb, 0)) fill_rect(c, x, y, w, h, argb);
    } else if (!strcmp(n->tag, "selector") || !strcmp(n->tag, "level-list") || !strcmp(n->tag, "animated-selector") ||
               !strcmp(n->tag, "ripple")) {
        /* The first of its drawables: an icon has no state. */
        const xnode *item;
        for (item = n->child; item; item = item->next)
            if (!strcmp(item->tag, "item")) {
                draw_inner(c, item, x, y, w, h, dp);
                break;
            }
    } else if (!strcmp(n->tag, "animated-vector") || !strcmp(n->tag, "rotate") || !strcmp(n->tag, "scale") || !strcmp(n->tag, "clip") ||
               !strcmp(n->tag, "transition")) {
        draw_inner(c, n, x, y, w, h, dp);
    }
    c->depth--;
}

/* ------------------------------------------------------------------ the icon */

u8 *hk_icon_rgba(const hk_icon_src *src, const char *path, int size)
{
    ctx c;
    u8 *out = NULL;
    bool any = false;
    int i;
    if (!src || !path || size < 16 || size > 1024) return NULL;
    memset(&c, 0, sizeof(c));
    c.src = src;
    c.w = c.h = size;
    c.px = (float *)hk_alloc(sizeof(float) * 4 * (size_t)size * size);
    if (ends_with(path, ".xml")) {
        xnode *x = read_xml(&c, path);
        if (x && !strcmp(x->tag, "adaptive-icon")) {
            /* Both layers are 108 dp; a launcher shows the 72 in their middle, rounded by its mask. */
            const xnode *b = child_named(x, "background"), *f = child_named(x, "foreground");
            float dp = (float)size / 72.0f, off = -18.0f * dp, whole = 108.0f * dp;
            if (b) draw_inner(&c, b, off, off, whole, whole, dp);
            if (f) draw_inner(&c, f, off, off, whole, whole, dp);
            {
                shape mask;
                float radii[4], *cov;
                for (i = 0; i < 4; i++) radii[i] = (float)size * 0.25f;
                memset(&mask, 0, sizeof(mask));
                sh_round_rect(&mask, 0, 0, (float)size, (float)size, radii);
                cov = coverage(&c, &mask, false);
                for (i = 0; i < size * size; i++) {
                    int k;
                    for (k = 0; k < 4; k++) c.px[i * 4 + k] *= cov[i];
                }
                free(cov);
                sh_free(&mask);
            }
        } else if (x) {
            /* A drawable of its own: its intrinsic size in dp, where it gives one, for its dimensions. */
            float width = number(&c, x, A_WIDTH, 48.0f, 1, 1);
            draw_node(&c, x, 0, 0, (float)size, (float)size, (float)size / (width > 0 ? width : 48.0f));
        }
        xml_free(x);
    } else {
        image img;
        if (image_read(&c, path, &img)) {
            draw_image(&c, &img, 0, 0, (float)size, (float)size);
            free(img.rgba);
        }
    }
    out = (u8 *)hk_alloc((size_t)size * size * 4);
    for (i = 0; i < size * size; i++) {
        const float *p = c.px + (size_t)i * 4;
        float a = p[3] > 1 ? 1 : p[3];
        int k;
        for (k = 0; k < 3; k++) {
            float v = a > 0 ? p[k] / a : 0;
            out[i * 4 + k] = (u8)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
        }
        out[i * 4 + 3] = (u8)(a <= 0 ? 0 : a * 255.0f + 0.5f);
        if (out[i * 4 + 3]) any = true;
    }
    free(c.px);
    if (!any) {
        free(out);
        return NULL;
    }
    return out;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* A chunk at `p`: its length, type, data and CRC; the bytes it takes. */
static size_t chunk(u8 *p, const char *type, const u8 *data, u32 len)
{
    put32(p, len);
    memcpy(p + 4, type, 4);
    if (len) memcpy(p + 8, data, len);
    put32(p + 8 + len, (u32)crc32(0, p + 4, len + 4));
    return (size_t)len + 12;
}

u8 *hk_png_encode(const u8 *rgba, int width, int height, size_t *png_size)
{
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    size_t stride = (size_t)width * 4 + 1, raw_size = stride * (size_t)height, at = 0;
    u8 *raw = (u8 *)hk_alloc(raw_size), *z, *png, ihdr[13];
    uLongf zsize;
    int y;
    for (y = 0; y < height; y++) memcpy(raw + (size_t)y * stride + 1, rgba + (size_t)y * width * 4, (size_t)width * 4);
    zsize = compressBound((uLong)raw_size);
    z = (u8 *)hk_alloc(zsize);
    if (compress2(z, &zsize, raw, (uLong)raw_size, 6) != Z_OK) {
        free(raw);
        free(z);
        return NULL;
    }
    free(raw);
    put32(ihdr, (u32)width);
    put32(ihdr + 4, (u32)height);
    ihdr[8] = 8;    /* bits a channel */
    ihdr[9] = 6;    /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png = (u8 *)hk_alloc(8 + 25 + 12 + zsize + 12);
    memcpy(png, sig, 8);
    at = 8;
    at += chunk(png + at, "IHDR", ihdr, 13);
    at += chunk(png + at, "IDAT", z, (u32)zsize);
    at += chunk(png + at, "IEND", NULL, 0);
    free(z);
    *png_size = at;
    return png;
}

u8 *hk_icon_png(const hk_icon_src *src, const char *path, int size, size_t *png_size)
{
    u8 *rgba = hk_icon_rgba(src, path, size), *png;
    if (!rgba) return NULL;
    png = hk_png_encode(rgba, size, size, png_size);
    free(rgba);
    return png;
}
