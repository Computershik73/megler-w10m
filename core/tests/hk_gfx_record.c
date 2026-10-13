/*
 * A Java app's drawing kept in a file, and drawn again from it by another
 * host (hk_gfx_record.h).
 *
 * The record is the calls as the core made them, in the order it made them:
 * a number for the call, the size of what follows, and the arguments. A
 * canvas, a shader and a typeface are known by the pointer the recording
 * host gave back for them. Memory cannot be known by its address alone -- the
 * core writes into bitmaps itself, and frees and reuses them -- so the record
 * carries the pixels wherever a host would read them: a bitmap's as its
 * canvas is made and whenever the core says it has touched them, and an
 * image's with every call that draws from it. Measuring calls are not kept:
 * their answers were the recording host's, and the layout that came of them
 * is in the coordinates of what was drawn.
 *
 * Both machines are little-endian and megler_gfx_paint is nineteen words of
 * four bytes on each, so the file is read as it was written.
 */
#include <stdio.h>

#include "../src/hk_base.h"
#include "../src/host/hk_host.h"
#include "hk_gfx_record.h"
#include "hk_gfx_standin.h"

enum {
    R_CANVAS_NEW = 1, R_CANVAS_FREE, R_FLUSH, R_TOUCHED, R_SAVE, R_SAVE_LAYER, R_RESTORE, R_MATRIX, R_CLIP_RECT, R_CLIP_PATH,
    R_COLOR, R_RECT, R_ROUND_RECT, R_OVAL, R_PATH, R_BITMAP, R_TEXT, R_FACE_CREATE, R_FACE_DERIVE, R_FACE_DATA,
    R_LINEAR, R_RADIAL, R_SWEEP, R_PATTERN, R_SHADER_MATRIX, R_SHADER_FREE, R_DECODE, R_PRESENT, R_COUNT
};

static const char k_magic[12] = "HKGFXREC 1\n";

/* ---------------------------------------------------------------- recording */

typedef struct rcanvas {
    void *cv;
    const u8 *px;
    int w, h, stride;
} rcanvas;

static hk_lock r_lock = HK_LOCK_INIT;
static struct {
    FILE *file;
    const megler_gfx_host *inner;
    u8 *buf;
    size_t n, cap;
    rcanvas *canvases;
    int ncanvases, cap_canvases;
    u32 records, by_kind[R_COUNT];
    u64 bytes;
} R;

static void put(const void *p, size_t n)
{
    if (R.n + n > R.cap) {
        R.cap = (R.n + n) * 2 + 4096;
        R.buf = (u8 *)hk_realloc(R.buf, R.cap);
    }
    if (n) memcpy(R.buf + R.n, p, n);
    R.n += n;
}
static void put32(u32 v) { put(&v, 4); }
static void puti(int v) { put32((u32)v); }
static void putf(float v) { put(&v, 4); }
static void put_id(const void *p)
{
    u64 v = (u64)(hk_addr)p;
    put(&v, 8);
}
static void put_floats(const float *f, int n)
{
    if (n > 0) put(f, sizeof(float) * (size_t)n);
}
/* The pixels, row after row with nothing between the rows. */
static void put_pixels(const void *px, int w, int h, int stride)
{
    int y;
    for (y = 0; y < h; y++) put((const u8 *)px + (size_t)y * (size_t)stride, (size_t)w * 4);
}
static void put_image(const megler_gfx_image *image)
{
    puti(image->width);
    puti(image->height);
    put32(image->id);
    put32(image->version);
    put_pixels(image->pixels, image->width, image->height, image->stride);
}

/* A record is put together under the lock (calls come from several threads) and written whole. */
static void record_open(void)
{
    hk_lock_acquire(&r_lock);
    R.n = 0;
}
static void record_close(u32 kind)
{
    u32 head[2];
    head[0] = kind;
    head[1] = (u32)R.n;
    if (R.file) {
        fwrite(head, 4, 2, R.file);
        fwrite(R.buf, 1, R.n, R.file);
        R.records++;
        R.by_kind[kind]++;
        R.bytes += 8 + R.n;
    }
    hk_lock_release(&r_lock);
}
static void record_id(u32 kind, const void *id)
{
    record_open();
    put_id(id);
    record_close(kind);
}

#define INNER (R.inner)

static void *r_canvas_new(void *user, void *pixels, int w, int h, int stride)
{
    void *cv = INNER->canvas_new(INNER->user, pixels, w, h, stride);
    (void)user;
    if (!cv) return NULL;
    record_open();
    put_id(cv);
    put_id(pixels);
    puti(w);
    puti(h);
    put_pixels(pixels, w, h, stride);
    if (R.ncanvases == R.cap_canvases) {
        R.cap_canvases = R.cap_canvases * 2 + 32;
        R.canvases = (rcanvas *)hk_realloc(R.canvases, sizeof(rcanvas) * (size_t)R.cap_canvases);
    }
    R.canvases[R.ncanvases].cv = cv;
    R.canvases[R.ncanvases].px = (const u8 *)pixels;
    R.canvases[R.ncanvases].w = w;
    R.canvases[R.ncanvases].h = h;
    R.canvases[R.ncanvases].stride = stride;
    R.ncanvases++;
    record_close(R_CANVAS_NEW);
    return cv;
}

static void r_canvas_free(void *user, void *cv)
{
    int i;
    (void)user;
    record_open();
    put_id(cv);
    for (i = 0; i < R.ncanvases; i++)
        if (R.canvases[i].cv == cv) {
            R.canvases[i] = R.canvases[--R.ncanvases];
            break;
        }
    record_close(R_CANVAS_FREE);
    INNER->canvas_free(INNER->user, cv);
}

static void r_flush(void *user, void *cv)
{
    (void)user;
    INNER->flush(INNER->user, cv);
    record_id(R_FLUSH, cv);
}

/* The memory was written by someone else: what it holds now goes into the record, for a host that will not have seen the writing. */
static void r_touched(void *user, void *cv)
{
    int i;
    (void)user;
    record_open();
    put_id(cv);
    for (i = 0; i < R.ncanvases; i++)
        if (R.canvases[i].cv == cv) {
            put_pixels(R.canvases[i].px, R.canvases[i].w, R.canvases[i].h, R.canvases[i].stride);
            break;
        }
    record_close(R_TOUCHED);
    INNER->touched(INNER->user, cv);
}

static void r_save(void *user, void *cv)
{
    (void)user;
    record_id(R_SAVE, cv);
    INNER->save(INNER->user, cv);
}

static void r_save_layer(void *user, void *cv, const float *bounds, int alpha, int blend)
{
    (void)user;
    record_open();
    put_id(cv);
    puti(bounds != NULL);
    if (bounds) put_floats(bounds, 4);
    puti(alpha);
    puti(blend);
    record_close(R_SAVE_LAYER);
    INNER->save_layer(INNER->user, cv, bounds, alpha, blend);
}

static void r_restore(void *user, void *cv)
{
    (void)user;
    record_id(R_RESTORE, cv);
    INNER->restore(INNER->user, cv);
}

static void r_set_matrix(void *user, void *cv, const float *m)
{
    (void)user;
    record_open();
    put_id(cv);
    put_floats(m, 6);
    record_close(R_MATRIX);
    INNER->set_matrix(INNER->user, cv, m);
}

static void r_clip_rect(void *user, void *cv, float l, float t, float r, float b, int op)
{
    (void)user;
    record_open();
    put_id(cv);
    putf(l);
    putf(t);
    putf(r);
    putf(b);
    puti(op);
    record_close(R_CLIP_RECT);
    INNER->clip_rect(INNER->user, cv, l, t, r, b, op);
}

static void r_clip_path(void *user, void *cv, const float *path, int count, int fill, int op)
{
    (void)user;
    record_open();
    put_id(cv);
    puti(count);
    puti(fill);
    puti(op);
    put_floats(path, count);
    record_close(R_CLIP_PATH);
    INNER->clip_path(INNER->user, cv, path, count, fill, op);
}

static void r_draw_color(void *user, void *cv, uint32_t color, int blend)
{
    (void)user;
    record_open();
    put_id(cv);
    put32(color);
    puti(blend);
    record_close(R_COLOR);
    INNER->draw_color(INNER->user, cv, color, blend);
}

/* What the three calls with a rectangle have in common. */
static void put_shape(void *cv, float l, float t, float r, float b, const megler_gfx_paint *paint, void *shader)
{
    put_id(cv);
    putf(l);
    putf(t);
    putf(r);
    putf(b);
    put(paint, sizeof(*paint));
    put_id(shader);
}

static void r_draw_rect(void *user, void *cv, float l, float t, float r, float b, const megler_gfx_paint *paint, void *shader)
{
    (void)user;
    record_open();
    put_shape(cv, l, t, r, b, paint, shader);
    record_close(R_RECT);
    INNER->draw_rect(INNER->user, cv, l, t, r, b, paint, shader);
}

static void r_draw_round_rect(void *user, void *cv, float l, float t, float r, float b, float rx, float ry, const megler_gfx_paint *paint, void *shader)
{
    (void)user;
    record_open();
    put_shape(cv, l, t, r, b, paint, shader);
    putf(rx);
    putf(ry);
    record_close(R_ROUND_RECT);
    INNER->draw_round_rect(INNER->user, cv, l, t, r, b, rx, ry, paint, shader);
}

static void r_draw_oval(void *user, void *cv, float l, float t, float r, float b, const megler_gfx_paint *paint, void *shader)
{
    (void)user;
    record_open();
    put_shape(cv, l, t, r, b, paint, shader);
    record_close(R_OVAL);
    INNER->draw_oval(INNER->user, cv, l, t, r, b, paint, shader);
}

static void r_draw_path(void *user, void *cv, const float *path, int count, int fill, const megler_gfx_paint *paint, void *shader)
{
    (void)user;
    record_open();
    put_id(cv);
    puti(count);
    puti(fill);
    put(paint, sizeof(*paint));
    put_id(shader);
    put_floats(path, count);
    record_close(R_PATH);
    INNER->draw_path(INNER->user, cv, path, count, fill, paint, shader);
}

static void r_draw_bitmap(void *user, void *cv, const megler_gfx_image *image, const float *src, const float *dst, const megler_gfx_paint *paint)
{
    (void)user;
    record_open();
    put_id(cv);
    put_floats(src, 4);
    put_floats(dst, 4);
    put(paint, sizeof(*paint));
    put_image(image);
    record_close(R_BITMAP);
    INNER->draw_bitmap(INNER->user, cv, image, src, dst, paint);
}

static void r_draw_text(void *user, void *cv, const uint16_t *text, int count, float x, float y, const megler_gfx_paint *paint, void *typeface, void *shader)
{
    (void)user;
    record_open();
    put_id(cv);
    puti(count);
    putf(x);
    putf(y);
    put(paint, sizeof(*paint));
    put_id(typeface);
    put_id(shader);
    if (count > 0) put(text, 2 * (size_t)count);
    record_close(R_TEXT);
    INNER->draw_text(INNER->user, cv, text, count, x, y, paint, typeface, shader);
}

static void *r_typeface_create(void *user, const char *family, int generic, int weight, bool italic)
{
    void *face = INNER->typeface_create(INNER->user, family, generic, weight, italic);
    u32 length = (u32)strlen(family ? family : "");
    (void)user;
    record_open();
    put_id(face);
    puti(generic);
    puti(weight);
    puti(italic);
    put32(length);
    put(family ? family : "", length);
    record_close(R_FACE_CREATE);
    return face;
}

static void *r_typeface_derive(void *user, void *typeface, int weight, bool italic)
{
    void *face = INNER->typeface_derive(INNER->user, typeface, weight, italic);
    (void)user;
    record_open();
    put_id(face);
    put_id(typeface);
    puti(weight);
    puti(italic);
    record_close(R_FACE_DERIVE);
    return face;
}

static void *r_typeface_from_data(void *user, const void *data, size_t size)
{
    void *face = INNER->typeface_from_data(INNER->user, data, size);
    (void)user;
    record_open();
    put_id(face);
    put32((u32)size);
    put(data, size);
    record_close(R_FACE_DATA);
    return face;
}

static void put_stops(const uint32_t *colors, const float *positions, int count, int tile)
{
    puti(count);
    puti(tile);
    puti(positions != NULL);
    put(colors, 4 * (size_t)count);
    if (positions) put_floats(positions, count);
}

static void *r_shader_linear(void *user, float x0, float y0, float x1, float y1, const uint32_t *colors, const float *positions, int count, int tile)
{
    void *shader = INNER->shader_linear(INNER->user, x0, y0, x1, y1, colors, positions, count, tile);
    (void)user;
    record_open();
    put_id(shader);
    putf(x0);
    putf(y0);
    putf(x1);
    putf(y1);
    put_stops(colors, positions, count, tile);
    record_close(R_LINEAR);
    return shader;
}

static void *r_shader_radial(void *user, float cx, float cy, float radius, const uint32_t *colors, const float *positions, int count, int tile)
{
    void *shader = INNER->shader_radial(INNER->user, cx, cy, radius, colors, positions, count, tile);
    (void)user;
    record_open();
    put_id(shader);
    putf(cx);
    putf(cy);
    putf(radius);
    put_stops(colors, positions, count, tile);
    record_close(R_RADIAL);
    return shader;
}

static void *r_shader_sweep(void *user, float cx, float cy, const uint32_t *colors, const float *positions, int count)
{
    void *shader = INNER->shader_sweep(INNER->user, cx, cy, colors, positions, count);
    (void)user;
    record_open();
    put_id(shader);
    putf(cx);
    putf(cy);
    put_stops(colors, positions, count, 0);
    record_close(R_SWEEP);
    return shader;
}

static void *r_shader_bitmap(void *user, const megler_gfx_image *image, int tile)
{
    void *shader = INNER->shader_bitmap(INNER->user, image, tile);
    (void)user;
    record_open();
    put_id(shader);
    puti(tile);
    put_image(image);
    record_close(R_PATTERN);
    return shader;
}

static void r_shader_matrix(void *user, void *shader, const float *m)
{
    (void)user;
    record_open();
    put_id(shader);
    puti(m != NULL);
    if (m) put_floats(m, 6);
    record_close(R_SHADER_MATRIX);
    INNER->shader_matrix(INNER->user, shader, m);
}

static void r_shader_free(void *user, void *shader)
{
    (void)user;
    record_id(R_SHADER_FREE, shader);
    INNER->shader_free(INNER->user, shader);
}

/* An image file the app decoded: its bytes, and what the recording host made of them, for another host's decoder to be held against. */
static bool r_image_decode(void *user, const void *data, size_t size, void *pixels, int w, int h, int stride)
{
    bool ok = INNER->image_decode(INNER->user, data, size, pixels, w, h, stride);
    (void)user;
    record_open();
    put32((u32)size);
    puti(w);
    puti(h);
    puti(ok);
    put(data, size);
    if (ok) put_pixels(pixels, w, h, stride);
    record_close(R_DECODE);
    return ok;
}

/* ------------------------------------------------------- the app, recorded */

static volatile i32 g_pictures, g_exited;
static int g_button[4];
static volatile i32 g_have_button;
static char g_reason[400];

void hk_gfx_record_log(int level, const char *line, void *user)
{
    const char *at = strstr(line, "layout focus=true");
    (void)level;
    (void)user;
    at = at ? strstr(at, " button=") : NULL;
    if (at && sscanf(at, " button=%d,%d,%dx%d", &g_button[0], &g_button[1], &g_button[2], &g_button[3]) == 4) hk_atomic_xchg(&g_have_button, 1);
    if (strstr(line, "HelloTest") || strstr(line, "DemosTest") || level >= MEGLER_LOG_WARN) printf("    %s\n", line);
}

static void rec_present(void *user, const void *pixels, int w, int h, int stride)
{
    (void)user;
    record_open();
    put_id(pixels);
    puti(w);
    puti(h);
    put_pixels(pixels, w, h, stride);
    record_close(R_PRESENT);
    hk_atomic_add(&g_pictures, 1);
}

static void rec_exit(void *user, int status, const char *reason)
{
    (void)user;
    (void)status;
    hk_strlcpy(g_reason, reason ? reason : "", sizeof(g_reason));
    hk_atomic_xchg(&g_exited, 1);
}

/* Until the app has shown no new picture for a third of a second. */
static void quiet(void)
{
    int still = 0, tries;
    for (tries = 0; tries < 2000 && still < 30 && !g_exited; tries++) {
        i32 before = g_pictures;
        hk_sleep_ns(10000000ull);
        still = g_pictures == before ? still + 1 : 0;
    }
}

int hk_gfx_record_run(const char *which, const char *apk, const char *android_root, const char *record_file)
{
    static megler_run_config cfg;
    int tries;
    i32 before;
    memset(&R, 0, sizeof(R));
    R.inner = hk_gfx_standin();
    R.file = fopen(record_file, "wb");
    if (!R.file) {
        printf("the record %s cannot be written\n", record_file);
        return 1;
    }
    fwrite(k_magic, 1, sizeof(k_magic), R.file);
    memset(&cfg, 0, sizeof(cfg));
    cfg.apk_path = apk;
    cfg.android_root = android_root;
    /* The size the Java test itself gives the app: a small phone at Android's base density. */
    cfg.width = 480;
    cfg.height = 800;
    cfg.density_dpi = 160;
    cfg.on_exit = rec_exit;
    cfg.window.present_pixels = rec_present;
    /* The stand-in draws and measures; every call that draws passes through here on its way. */
    cfg.gfx = *R.inner;
    cfg.gfx.canvas_new = r_canvas_new;
    cfg.gfx.canvas_free = r_canvas_free;
    cfg.gfx.flush = r_flush;
    cfg.gfx.touched = r_touched;
    cfg.gfx.save = r_save;
    cfg.gfx.save_layer = r_save_layer;
    cfg.gfx.restore = r_restore;
    cfg.gfx.set_matrix = r_set_matrix;
    cfg.gfx.clip_rect = r_clip_rect;
    cfg.gfx.clip_path = r_clip_path;
    cfg.gfx.draw_color = r_draw_color;
    cfg.gfx.draw_rect = r_draw_rect;
    cfg.gfx.draw_round_rect = r_draw_round_rect;
    cfg.gfx.draw_oval = r_draw_oval;
    cfg.gfx.draw_path = r_draw_path;
    cfg.gfx.draw_bitmap = r_draw_bitmap;
    cfg.gfx.draw_text = r_draw_text;
    cfg.gfx.typeface_create = r_typeface_create;
    cfg.gfx.typeface_derive = r_typeface_derive;
    cfg.gfx.typeface_from_data = r_typeface_from_data;
    cfg.gfx.shader_linear = r_shader_linear;
    cfg.gfx.shader_radial = r_shader_radial;
    cfg.gfx.shader_sweep = r_shader_sweep;
    cfg.gfx.shader_bitmap = r_shader_bitmap;
    cfg.gfx.shader_matrix = r_shader_matrix;
    cfg.gfx.shader_free = r_shader_free;
    cfg.gfx.image_decode = r_image_decode;
    printf("recording what the Java test app %s asks of the shell's drawing (%s on the Android root %s)\n", which, apk, android_root);
    if (!megler_run(&cfg)) {
        printf("  the app did not start: %s\n", megler_last_error());
        fclose(R.file);
        return 1;
    }
    for (tries = 0; tries < 18000 && !g_pictures && !g_exited; tries++) hk_sleep_ns(10000000ull);
    quiet();
    printf("  started: %d pictures so far\n", (int)g_pictures);
    if (!strcmp(which, "hello") && g_have_button && !g_exited) {
        /* The button pressed, where the app's own log says it is; the next picture has the new text and the wider green rectangle. */
        before = g_pictures;
        megler_touch(0, 1, (float)(g_button[0] + g_button[2] / 2), (float)(g_button[1] + g_button[3] / 2));
        hk_sleep_ns(60000000ull);
        megler_touch(2, 1, (float)(g_button[0] + g_button[2] / 2), (float)(g_button[1] + g_button[3] / 2));
        for (tries = 0; tries < 500 && g_pictures == before && !g_exited; tries++) hk_sleep_ns(10000000ull);
        quiet();
        printf("  the button at %d,%d pressed: %d pictures so far\n", g_button[0] + g_button[2] / 2, g_button[1] + g_button[3] / 2, (int)g_pictures);
    }
    if (!g_exited) {
        megler_key(4, true);
        megler_key(4, false);
    }
    for (tries = 0; tries < 3000 && !g_exited; tries++) hk_sleep_ns(10000000ull);
    /* The app's last calls (its canvases freed) may still be coming: the file is closed under the lock, and what comes after is not kept. */
    hk_sleep_ns(200000000ull);
    hk_lock_acquire(&r_lock);
    fclose(R.file);
    R.file = NULL;
    hk_lock_release(&r_lock);
    printf("  %s: %u calls recorded in %u KB (%u canvases made, %u drawings of text, %u of bitmaps, %u image files decoded), %d pictures shown; the app %s%s%s\n",
           record_file, (unsigned)R.records, (unsigned)(R.bytes >> 10), (unsigned)R.by_kind[R_CANVAS_NEW], (unsigned)R.by_kind[R_TEXT], (unsigned)R.by_kind[R_BITMAP],
           (unsigned)R.by_kind[R_DECODE], (int)g_pictures, g_exited ? "ended" : "did NOT end", g_reason[0] ? ": " : "", g_reason);
    return g_pictures > 0 && g_exited && !g_reason[0] ? 0 : 1;
}

/* ------------------------------------------------------------------ replaying */

typedef struct reader {
    const u8 *at, *end;
    bool bad;
} reader;

static const void *get(reader *r, size_t n)
{
    const void *p = r->at;
    if (r->bad || (size_t)(r->end - r->at) < n) {
        r->bad = true;
        return NULL;
    }
    r->at += n;
    return p;
}
static u32 get32(reader *r)
{
    const void *p = get(r, 4);
    return p ? hk_rd32(p) : 0;
}
static int geti(reader *r) { return (int)get32(r); }
static float getf(reader *r) { return hk_f(get32(r)); }
static u64 get_id(reader *r)
{
    const void *p = get(r, 8);
    u64 v = 0;
    if (p) memcpy(&v, p, 8);
    return v;
}
/* `n` floats, copied to where they are aligned. NULL when the record is short. */
static float *get_floats(reader *r, int n, float *into)
{
    const void *p = n > 0 ? get(r, sizeof(float) * (size_t)n) : NULL;
    if (!p) return NULL;
    memcpy(into, p, sizeof(float) * (size_t)n);
    return into;
}
static bool get_paint(reader *r, megler_gfx_paint *paint)
{
    const void *p = get(r, sizeof(*paint));
    if (!p) return false;
    memcpy(paint, p, sizeof(*paint));
    return true;
}

typedef struct pmem {
    u64 addr;
    int w, h, refs;
    u32 *px;
} pmem;

typedef struct pcanvas {
    u64 id;
    void *cv;
    int mem;
} pcanvas;

typedef struct pthing {
    u64 id;
    void *host;
} pthing;

static struct {
    const megler_gfx_host *h;
    pmem *mems;
    pcanvas *canvases;
    pthing *shaders, *faces;
    int nmems, ncanvases, nshaders, nfaces;
} P;

static int p_checks, p_failed;
static hk_test_sink p_sink;
static void *p_sink_user;

static void p_say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (p_sink) p_sink(line, p_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define P_CHECK(cond)                                                  \
    do {                                                               \
        p_checks++;                                                    \
        if (!(cond)) {                                                 \
            p_failed++;                                                \
            p_say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);      \
        }                                                              \
    } while (0)

static void *canvas_of(u64 id)
{
    int i;
    for (i = P.ncanvases - 1; i >= 0; i--)
        if (P.canvases[i].id == id) return P.canvases[i].cv;
    return NULL;
}

static void *thing_of(const pthing *list, int n, u64 id)
{
    int i;
    for (i = n - 1; i >= 0 && id; i--)
        if (list[i].id == id) return list[i].host;
    return NULL;
}

static void thing_add(pthing **list, int *n, u64 id, void *host)
{
    *list = (pthing *)hk_realloc(*list, sizeof(pthing) * (size_t)(*n + 1));
    (*list)[*n].id = id;
    (*list)[*n].host = host;
    (*n)++;
}

/* An image of a record as a host is handed one: its pixels copied to where they are aligned. The caller frees them. */
static u32 *get_image(reader *r, megler_gfx_image *image)
{
    u32 *px;
    const void *p;
    memset(image, 0, sizeof(*image));
    image->width = geti(r);
    image->height = geti(r);
    image->id = get32(r);
    image->version = get32(r);
    if (r->bad || image->width <= 0 || image->height <= 0 || image->width > 16384 || image->height > 16384) {
        r->bad = true;
        return NULL;
    }
    p = get(r, (size_t)image->width * (size_t)image->height * 4);
    if (!p) return NULL;
    px = (u32 *)hk_alloc((size_t)image->width * (size_t)image->height * 4);
    memcpy(px, p, (size_t)image->width * (size_t)image->height * 4);
    image->pixels = px;
    image->stride = image->width * 4;
    return px;
}

static int far_apart(u32 a, u32 b)
{
    int shift, worst = 0;
    for (shift = 0; shift < 32; shift += 8) {
        int d = abs((int)((a >> shift) & 255) - (int)((b >> shift) & 255));
        if (d > worst) worst = d;
    }
    return worst;
}

typedef struct pbytes {
    u8 *data;
    size_t size;
} pbytes;

static bool p_bytes_write(void *sink, const void *data, size_t size)
{
    pbytes *o = (pbytes *)sink;
    o->data = (u8 *)hk_realloc(o->data, o->size + size + 1);
    memcpy(o->data + o->size, data, size);
    o->size += size;
    return true;
}

int hk_gfx_replay_run(const char *record_file, const megler_gfx_host *host, bool same, const char *out_dir, const char *name, hk_test_sink sink, void *user, int *checks)
{
    const megler_gfx_host *standin = hk_gfx_standin();
    size_t size = 0;
    u8 *file = hk_file_read_all(record_file, &size);
    reader all;
    int calls = 0, frames = 0, images = 0, images_wrong = 0, images_more = 0, unknown = 0, i;
    p_sink = sink;
    p_sink_user = user;
    p_checks = p_failed = 0;
    memset(&P, 0, sizeof(P));
    P.h = host;
    p_say("a Java app's drawing, recorded under qemu, drawn again from %s (%u KB)%s", record_file, (unsigned)(size >> 10),
          same ? " by the host that recorded it: every picture must come out as it was" : "");
    P_CHECK(file != NULL && size > sizeof(k_magic) && memcmp(file, k_magic, sizeof(k_magic)) == 0);
    if (p_failed) {
        free(file);
        if (checks) *checks += p_checks;
        return p_failed;
    }
    if (out_dir) hk_path_mkdirs(out_dir);
    all.at = file + sizeof(k_magic);
    all.end = file + size;
    all.bad = false;
    while (!all.bad && all.end - all.at >= 8) {
        u32 kind = get32(&all), length = get32(&all);
        const u8 *body = (const u8 *)get(&all, length);
        reader r;
        megler_gfx_paint paint;
        megler_gfx_image image;
        float f[8], l, t, rr, b;
        u64 id;
        void *cv;
        if (!body) break;
        r.at = body;
        r.end = body + length;
        r.bad = false;
        calls++;
        id = kind == R_DECODE || kind == R_PRESENT ? 0 : get_id(&r);
        cv = kind <= R_TEXT ? canvas_of(id) : NULL;
        switch (kind) {
        case R_CANVAS_NEW: {
            u64 addr = get_id(&r);
            int w = geti(&r), h = geti(&r), mem = -1;
            const void *px = r.bad || w <= 0 || h <= 0 || w > 16384 || h > 16384 ? NULL : get(&r, (size_t)w * (size_t)h * 4);
            if (!px) break;
            /* Another canvas on memory a canvas is drawing into shares it; memory no canvas has is another bitmap's by now, or the same one's, written since. */
            for (i = P.nmems - 1; i >= 0 && mem < 0; i--)
                if (P.mems[i].addr == addr && P.mems[i].refs > 0 && P.mems[i].w == w && P.mems[i].h == h) mem = i;
            if (mem < 0) {
                P.mems = (pmem *)hk_realloc(P.mems, sizeof(pmem) * (size_t)(P.nmems + 1));
                mem = P.nmems++;
                P.mems[mem].addr = addr;
                P.mems[mem].w = w;
                P.mems[mem].h = h;
                P.mems[mem].refs = 0;
                P.mems[mem].px = (u32 *)hk_alloc((size_t)w * (size_t)h * 4);
                memcpy(P.mems[mem].px, px, (size_t)w * (size_t)h * 4);
            }
            P.mems[mem].refs++;
            P.canvases = (pcanvas *)hk_realloc(P.canvases, sizeof(pcanvas) * (size_t)(P.ncanvases + 1));
            P.canvases[P.ncanvases].id = id;
            P.canvases[P.ncanvases].mem = mem;
            P.canvases[P.ncanvases].cv = host->canvas_new(host->user, P.mems[mem].px, w, h, w * 4);
            P.ncanvases++;
            break;
        }
        case R_CANVAS_FREE:
            for (i = P.ncanvases - 1; i >= 0; i--)
                if (P.canvases[i].id == id) {
                    pmem *m = &P.mems[P.canvases[i].mem];
                    if (P.canvases[i].cv) host->canvas_free(host->user, P.canvases[i].cv);
                    if (--m->refs == 0) {
                        free(m->px);
                        m->px = NULL;
                    }
                    P.canvases[i] = P.canvases[--P.ncanvases];
                    break;
                }
            break;
        case R_FLUSH:
            if (cv) host->flush(host->user, cv);
            break;
        case R_TOUCHED:
            for (i = P.ncanvases - 1; i >= 0; i--)
                if (P.canvases[i].id == id) {
                    pmem *m = &P.mems[P.canvases[i].mem];
                    const void *px = get(&r, (size_t)m->w * (size_t)m->h * 4);
                    if (px) memcpy(m->px, px, (size_t)m->w * (size_t)m->h * 4);
                    break;
                }
            if (cv) host->touched(host->user, cv);
            break;
        case R_SAVE:
            if (cv) host->save(host->user, cv);
            break;
        case R_SAVE_LAYER: {
            bool bounded = geti(&r) != 0;
            float *bounds = bounded ? get_floats(&r, 4, f) : NULL;
            int alpha = geti(&r), blend = geti(&r);
            if (cv && !r.bad) host->save_layer(host->user, cv, bounds, alpha, blend);
            break;
        }
        case R_RESTORE:
            if (cv) host->restore(host->user, cv);
            break;
        case R_MATRIX:
            if (cv && get_floats(&r, 6, f)) host->set_matrix(host->user, cv, f);
            break;
        case R_CLIP_RECT:
            l = getf(&r);
            t = getf(&r);
            rr = getf(&r);
            b = getf(&r);
            i = geti(&r);
            if (cv && !r.bad) host->clip_rect(host->user, cv, l, t, rr, b, i);
            break;
        case R_CLIP_PATH:
        case R_PATH: {
            int count = geti(&r), fill = geti(&r), op = 0;
            void *shader = NULL;
            float *path;
            if (kind == R_CLIP_PATH) op = geti(&r);
            else if (get_paint(&r, &paint)) shader = thing_of(P.shaders, P.nshaders, get_id(&r));
            if (r.bad || count < 0 || count > 1000000) break;
            path = (float *)hk_alloc(sizeof(float) * (size_t)(count + 1));
            if (cv && (count == 0 || get_floats(&r, count, path))) {
                if (kind == R_CLIP_PATH) host->clip_path(host->user, cv, path, count, fill, op);
                else host->draw_path(host->user, cv, path, count, fill, &paint, shader);
            }
            free(path);
            break;
        }
        case R_COLOR: {
            u32 color = get32(&r);
            int blend = geti(&r);
            if (cv && !r.bad) host->draw_color(host->user, cv, color, blend);
            break;
        }
        case R_RECT:
        case R_ROUND_RECT:
        case R_OVAL: {
            void *shader;
            l = getf(&r);
            t = getf(&r);
            rr = getf(&r);
            b = getf(&r);
            if (!get_paint(&r, &paint)) break;
            shader = thing_of(P.shaders, P.nshaders, get_id(&r));
            if (kind == R_ROUND_RECT) {
                f[0] = getf(&r);
                f[1] = getf(&r);
            }
            if (!cv || r.bad) break;
            if (kind == R_RECT) host->draw_rect(host->user, cv, l, t, rr, b, &paint, shader);
            else if (kind == R_OVAL) host->draw_oval(host->user, cv, l, t, rr, b, &paint, shader);
            else host->draw_round_rect(host->user, cv, l, t, rr, b, f[0], f[1], &paint, shader);
            break;
        }
        case R_BITMAP: {
            u32 *px;
            if (!get_floats(&r, 4, f) || !get_floats(&r, 4, f + 4) || !get_paint(&r, &paint)) break;
            px = get_image(&r, &image);
            if (px && cv) host->draw_bitmap(host->user, cv, &image, f, f + 4, &paint);
            free(px);
            break;
        }
        case R_TEXT: {
            int count = geti(&r);
            float x = getf(&r), y = getf(&r);
            void *face, *shader;
            const void *units;
            u16 *text;
            if (!get_paint(&r, &paint)) break;
            face = thing_of(P.faces, P.nfaces, get_id(&r));
            shader = thing_of(P.shaders, P.nshaders, get_id(&r));
            units = count > 0 && count < 1000000 ? get(&r, 2 * (size_t)count) : NULL;
            if (!units || !cv) break;
            text = (u16 *)hk_alloc(2 * (size_t)count);
            memcpy(text, units, 2 * (size_t)count);
            host->draw_text(host->user, cv, text, count, x, y, &paint, face, shader);
            free(text);
            break;
        }
        case R_FACE_CREATE: {
            int generic = geti(&r), weight = geti(&r), italic = geti(&r);
            u32 length = get32(&r);
            const void *family = get(&r, length);
            char text[100];
            if (!family) break;
            memset(text, 0, sizeof(text));
            memcpy(text, family, length < sizeof(text) - 1 ? length : sizeof(text) - 1);
            thing_add(&P.faces, &P.nfaces, id, host->typeface_create(host->user, text, generic, weight, italic != 0));
            break;
        }
        case R_FACE_DERIVE: {
            void *from = thing_of(P.faces, P.nfaces, get_id(&r));
            int weight = geti(&r), italic = geti(&r);
            if (!r.bad) thing_add(&P.faces, &P.nfaces, id, host->typeface_derive(host->user, from, weight, italic != 0));
            break;
        }
        case R_FACE_DATA: {
            u32 length = get32(&r);
            const void *data = get(&r, length);
            if (data) thing_add(&P.faces, &P.nfaces, id, host->typeface_from_data(host->user, data, length));
            break;
        }
        case R_LINEAR:
        case R_RADIAL:
        case R_SWEEP: {
            int coords = kind == R_LINEAR ? 4 : kind == R_RADIAL ? 3 : 2, count, tile, has;
            u32 *colors;
            float *positions = NULL;
            const void *p;
            void *made;
            if (!get_floats(&r, coords, f)) break;
            count = geti(&r);
            tile = geti(&r);
            has = geti(&r);
            if (r.bad || count < 1 || count > 100000) break;
            p = get(&r, 4 * (size_t)count);
            if (!p) break;
            colors = (u32 *)hk_alloc(4 * (size_t)count);
            memcpy(colors, p, 4 * (size_t)count);
            if (has) {
                positions = (float *)hk_alloc(sizeof(float) * (size_t)count);
                if (!get_floats(&r, count, positions)) {
                    free(positions);
                    positions = NULL;
                }
            }
            if (kind == R_LINEAR) made = host->shader_linear(host->user, f[0], f[1], f[2], f[3], colors, positions, count, tile);
            else if (kind == R_RADIAL) made = host->shader_radial(host->user, f[0], f[1], f[2], colors, positions, count, tile);
            else made = host->shader_sweep(host->user, f[0], f[1], colors, positions, count);
            thing_add(&P.shaders, &P.nshaders, id, made);
            free(colors);
            free(positions);
            break;
        }
        case R_PATTERN: {
            int tile = geti(&r);
            u32 *px = get_image(&r, &image);
            if (px) thing_add(&P.shaders, &P.nshaders, id, host->shader_bitmap(host->user, &image, tile));
            free(px);
            break;
        }
        case R_SHADER_MATRIX: {
            void *shader = thing_of(P.shaders, P.nshaders, id);
            bool has = geti(&r) != 0;
            float *m = has ? get_floats(&r, 6, f) : NULL;
            if (shader && !r.bad) host->shader_matrix(host->user, shader, m);
            break;
        }
        case R_SHADER_FREE:
            for (i = P.nshaders - 1; i >= 0; i--)
                if (P.shaders[i].id == id) {
                    if (P.shaders[i].host) host->shader_free(host->user, P.shaders[i].host);
                    P.shaders[i] = P.shaders[--P.nshaders];
                    break;
                }
            break;
        case R_DECODE: {
            u32 length = get32(&r);
            int w = geti(&r), h = geti(&r), ok = geti(&r), wrong = 0, k;
            const void *data = get(&r, length), *want;
            u32 *made;
            if (!data || w <= 0 || h <= 0 || w > 16384 || h > 16384) break;
            made = (u32 *)hk_alloc((size_t)w * (size_t)h * 4);
            if (!ok) {
                /* (The stand-in reads PNG alone: what it could not read, a real host may.) */
                images_more += host->image_decode(host->user, data, length, made, w, h, w * 4);
            } else if ((want = get(&r, (size_t)w * (size_t)h * 4)) != NULL) {
                images++;
                if (!host->image_decode(host->user, data, length, made, w, h, w * 4)) wrong = 1;
                for (k = 0; k < w * h && !wrong; k++) wrong = far_apart(made[k], hk_rd32((const u8 *)want + 4 * (size_t)k)) > 1;
                if (wrong && !images_wrong) p_say("    an image file of %u bytes, %d x %d, decodes to other pixels than the stand-in's", (unsigned)length, w, h);
                images_wrong += wrong;
            }
            free(made);
            break;
        }
        case R_PRESENT: {
            u64 addr = get_id(&r);
            int w = geti(&r), h = geti(&r), differ = 0, worst = 0, k;
            const void *want = r.bad || w <= 0 || h <= 0 ? NULL : get(&r, (size_t)w * (size_t)h * 4);
            const pmem *m = NULL;
            if (!want) break;
            for (i = P.nmems - 1; i >= 0 && !m; i--)
                if (P.mems[i].addr == addr && P.mems[i].px && P.mems[i].w == w && P.mems[i].h == h) m = &P.mems[i];
            frames++;
            P_CHECK(m != NULL);
            if (!m) break;
            for (k = 0; k < w * h; k++) {
                int d = far_apart(m->px[k], hk_rd32((const u8 *)want + 4 * (size_t)k));
                differ += d > 1;
                if (d > worst) worst = d;
            }
            if (same) {
                P_CHECK(differ == 0);
            } else {
                p_say("  picture %d, %d x %d: %d of its %d pixels are another colour than in the stand-in's picture (text is letters here and boxes there; edges are smoothed)", frames,
                      w, h, differ, w * h);
            }
            if (out_dir) {
                pbytes png = { NULL, 0 };
                char *path = hk_sprintf("%s/%s-%d.png", out_dir, name, frames);
                if (!standin->image_encode(standin->user, m->px, w, h, w * 4, MEGLER_GFX_PNG, 100, p_bytes_write, &png) || !hk_file_write_all(path, png.data, png.size))
                    p_say("  (could not write %s)", path);
                free(png.data);
                free(path);
            }
            break;
        }
        default:
            unknown++;
            break;
        }
        if (r.bad) unknown++;
    }
    /* What the app had not freed when the record ended. */
    for (i = P.ncanvases - 1; i >= 0; i--)
        if (P.canvases[i].cv) host->canvas_free(host->user, P.canvases[i].cv);
    for (i = 0; i < P.nshaders; i++)
        if (P.shaders[i].host) host->shader_free(host->user, P.shaders[i].host);
    for (i = 0; i < P.nmems; i++) free(P.mems[i].px);
    free(P.mems);
    free(P.canvases);
    free(P.shaders);
    free(P.faces);
    free(file);
    p_say("  %d calls drawn again, %d pictures%s%s; %d image files decoded again, %d of them to other pixels than the stand-in's%s", calls, frames,
          out_dir ? " written to " : "", out_dir ? out_dir : "", images, images_wrong,
          images_more ? hk_sprintf("; %d more that the stand-in could not read were read", images_more) : "");
    P_CHECK(frames > 0);
    P_CHECK(unknown == 0 && !all.bad);
    P_CHECK(images_wrong == 0);
    if (checks) *checks += p_checks;
    return p_failed;
}
