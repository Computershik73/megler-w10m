/*
 * The Java driver, seen from outside:
 *
 *   hk_tests --java <hello | demos> <apk> <data dir> <android root> [picture dir | -] [verbose]
 *
 * The APKs are the test apps of core/tests/app (tools/build-test-app.sh):
 * ordinary Android apps with no native code. Each is started through
 * megler_run as the shell would start it, with a stand-in for the shell's
 * drawing (hk_gfx_standin.c) and a window host that keeps each picture it is
 * handed.
 *
 * `hello` is an Application, one Activity, and a layout from its resources
 * with a TextView, a Button, a view that draws for itself and a picture.
 * Checked: what the app says in the log, in the order Android would have it
 * say it; pixels of the pictures where the layout puts known colours; that a
 * touch on the button runs its listener and the next picture shows the
 * change; pause and resume; that Back takes the activity down in order and
 * ends the app. And measured: what a picture and a touch cost in objects and
 * instructions, since nothing is collected yet.
 *
 * `demos` is the less tame one: a ListView with an adapter, an AlertDialog,
 * SharedPreferences, delayed messages, an AsyncTask, a second Activity
 * started for a result, a Toast. The test touches the first row; the app goes
 * down its list by itself and says in the log how each went.
 *
 * `natives` is the one with native code: a library of its own, built by the
 * NDK, which a second library of the APK's is needed by. The app goes through
 * JNI and the NDK step by step (core/tests/app/natives) and says how each
 * went; the test reads that, and looks at the bitmap the library painted
 * where the app drew it. Only where guest code can run.
 *
 * `gl` is OpenGL ES from Java: a GLSurfaceView's renderer on the layer's GL thread, EGL14 on a pbuffer, EGL10 and GL10 on a
 * SurfaceView from a thread of the app's, and the window composed of the views and the frames under them. OpenGL ES is the
 * stand-in that draws (hk_gl_standin.c), for every app here.
 *
 * `app` is any APK at all: started, left to itself until it is still, its
 * picture written, then whatever the script on the command line says -- a
 * touch, a wait, Back, each followed by another picture -- and a summary of
 * how it stands. Nothing is checked but that it started: it is for looking
 * at what a real app does here (docs/JAVA.md, "Real apps").
 *
 * A process runs one app, so each is a run of its own. Each picture is also
 * written out as a BMP when a folder is named, to be looked at.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../include/megler.h"
#include "../src/apk/hk_apk.h"
#include "../src/bionic/hk_bionic.h"
#include "../src/drivers/hk_driver.h"
#include "../src/drivers/hk_java.h"
#include "../src/drivers/hk_java_gfx.h"
#include "../src/drivers/hk_java_gl.h"
#include "../src/dvm/hk_dvm.h"
#include "../src/hk_base.h"
#include "../src/host/hk_host.h"
#include "../src/ndk/hk_egl.h"
#include "../src/run/hk_run.h"
#include "hk_gfx_standin.h"
#include "hk_gl_standin.h"
#include "hk_tests.h"

const char *megler_data_dir(void);

static int g_checks, g_failed;
static bool running(void);

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

#define CHECK(cond)                                                    \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

/* ---------------------------------------------------------------- the log */

#define MAX_LINES 8000
static hk_lock g_log_lock = HK_LOCK_INIT;
static char *g_lines[MAX_LINES];
static volatile i32 g_nlines;
static int g_verbose;

/* Every line of the layer's log, kept; the app's own (android.util.Log) are among them as "I/Tag: text". */
void hk_java_tests_log(int level, const char *line, void *user)
{
    (void)user;
    hk_lock_acquire(&g_log_lock);
    if (g_nlines < MAX_LINES) g_lines[g_nlines++] = hk_strdup(line);
    hk_lock_release(&g_log_lock);
    if (g_verbose || level >= MEGLER_LOG_WARN) {
        printf("%s%s\n", level >= MEGLER_LOG_ERROR ? "E " : level == MEGLER_LOG_WARN ? "W " : "  ", line);
        fflush(stdout);
    }
}

/* The first line at or after `from` that contains the text; -1 when there is none. */
static int find_line(const char *text, int from)
{
    int i, found = -1;
    hk_lock_acquire(&g_log_lock);
    for (i = from < 0 ? 0 : from; i < g_nlines && found < 0; i++)
        if (strstr(g_lines[i], text)) found = i;
    hk_lock_release(&g_log_lock);
    return found;
}

static const char *line_at(int i) { return i >= 0 && i < g_nlines ? g_lines[i] : ""; }

/* Waits for a line of the log, up to `seconds`, while the app runs; its number, or -1. */
static int wait_line(const char *text, int from, int seconds)
{
    int tries, at = -1;
    for (tries = 0; tries < seconds * 100 && (at = find_line(text, from)) < 0 && running(); tries++) hk_sleep_ns(10000000ull);
    return at >= 0 ? at : find_line(text, from);
}

/* A line the app must have said after line `*at`; `*at` moves to it, so that a row of these checks the order too. */
static void expect_line(const char *text, int *at, int seconds)
{
    int found = wait_line(text, *at + 1, seconds);
    g_checks++;
    if (found < 0) {
        g_failed++;
        say("  FAIL: the log has no \"%s\" after line %d", text, *at);
    } else {
        *at = found;
    }
}

/* "…name=x,y,WxH…" of a line: the four numbers. */
static bool rect_in(const char *line, const char *name, int r[4])
{
    char key[40];
    const char *p;
    snprintf(key, sizeof(key), "%s=", name);
    p = strstr(line, key);
    return p && sscanf(p + strlen(key), "%d,%d,%dx%d", &r[0], &r[1], &r[2], &r[3]) == 4;
}

static void say_app_lines(const char *tag)
{
    int i;
    say("  what the app said:");
    for (i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], tag)) say("    %s", g_lines[i]);
}

/* ------------------------------------------------------------ the window */

static struct {
    hk_lock lock;
    u8 *pixels;                        /* the last picture, copied: the memory handed over is the app's and is drawn into again */
    int width, height, stride;
    volatile i32 pictures;
    const char *dir;
    /* Pictures that came as a part (present_pixels_part), and pixels outside the part they said had changed that had changed. */
    i32 parts;
    long stale;
    /* Pictures shown from a canvas, unflushed (megler_gfx_host.present), as a shell that draws elsewhere than in memory shows them. */
    volatile i32 from_canvas;
} W = { HK_LOCK_INIT, NULL, 0, 0, 0, 0, NULL, 0, 0, 0 };

static void present_pixels(void *user, const void *pixels, int width, int height, int stride)
{
    (void)user;
    hk_lock_acquire(&W.lock);
    if (W.width != width || W.height != height || W.stride != stride) {
        free(W.pixels);
        W.pixels = (u8 *)hk_alloc((size_t)stride * (size_t)height);
        W.width = width;
        W.height = height;
        W.stride = stride;
    }
    memcpy(W.pixels, pixels, (size_t)stride * (size_t)height);
    hk_lock_release(&W.lock);
    hk_atomic_add(&W.pictures, 1);
}

/* Pixels that differ between two runs of `n` pixels. */
static long pixels_differing(const u8 *a, const u8 *b, int n)
{
    long d = 0;
    int i;
    if (n <= 0 || !memcmp(a, b, (size_t)n * 4)) return 0;
    for (i = 0; i < n; i++) d += memcmp(a + 4 * i, b + 4 * i, 4) != 0;
    return d;
}

/*
 * A picture of which the layer says only a rectangle changed: that rectangle is copied, as a shell that keeps its last picture
 * would, and the rest is compared with what was kept. A pixel that changed outside the rectangle would be stale on such a
 * shell: the window's dirty region missed it.
 */
static void present_pixels_part(void *user, const void *pixels, int width, int height, int stride, int x, int y, int part_width, int part_height)
{
    const u8 *src = (const u8 *)pixels;
    int row;
    if (!W.pixels || W.width != width || W.height != height || W.stride != stride || x < 0 || y < 0 || x + part_width > width || y + part_height > height) {
        present_pixels(user, pixels, width, height, stride);
        return;
    }
    hk_lock_acquire(&W.lock);
    for (row = 0; row < height; row++) {
        const u8 *s = src + (size_t)row * (size_t)stride;
        u8 *d = W.pixels + (size_t)row * (size_t)stride;
        if (row < y || row >= y + part_height) {
            W.stale += pixels_differing(s, d, width);
            continue;
        }
        W.stale += pixels_differing(s, d, x) + pixels_differing(s + 4 * (x + part_width), d + 4 * (x + part_width), width - x - part_width);
        memcpy(d + 4 * x, s + 4 * x, (size_t)part_width * 4);
    }
    W.parts++;
    hk_lock_release(&W.lock);
    hk_atomic_add(&W.pictures, 1);
}

/*
 * megler_gfx_host.present, as a shell that draws on a graphics processor has it: the window's picture shown from the canvas that
 * drew it, which the layer has not flushed. The stand-in's canvas draws straight into the memory, which is its picture, so that
 * is shown, the same way as a picture handed over by present_pixels(_part): the same checks hold for both.
 */
/*
 * megler_gfx_host.gl_frame and gl_place, as a shell that composes the window on its graphics processor has them: each layer's
 * place and last frame kept, the views' picture as `present` last showed it kept, and the window composed of the two -- the
 * frames at their places, scaled to them, opaque, the views' premultiplied colours over them -- whenever either changes,
 * then shown as a picture handed to present_pixels is. The frames come out of the GL stand-in's surfaces by their "shared
 * handles" (hk_gl_standin_shared). With the script's "readback" gl_frame says no, and the core composes the window itself.
 */
#define GL_LAYERS 8
static struct {
    hk_lock lock;
    bool refuse, used;
    struct {
        i32 x, y, w, h;
        u8 *frame;                     /* R, G, B, A, rows bottom first */
        int fw, fh;
    } layers[GL_LAYERS];
    u8 *views, *out;                   /* B, G, R, A premultiplied, rows top first */
    int vw, vh;
    volatile i32 frames, places, composed;
} GL = { HK_LOCK_INIT };

/* With GL.lock held: the window composed and shown. */
static void gl_compose(void)
{
    int i, r, col;
    size_t stride = (size_t)GL.vw * 4;
    if (!GL.views) return;
    memcpy(GL.out, GL.views, stride * (size_t)GL.vh);
    for (i = 0; i < GL_LAYERS; i++) {
        i32 x0, y0, x1, y1;
        if (!GL.layers[i].frame || GL.layers[i].w <= 0 || GL.layers[i].h <= 0) continue;
        x0 = GL.layers[i].x < 0 ? 0 : GL.layers[i].x;
        y0 = GL.layers[i].y < 0 ? 0 : GL.layers[i].y;
        x1 = GL.layers[i].x + GL.layers[i].w > GL.vw ? GL.vw : GL.layers[i].x + GL.layers[i].w;
        y1 = GL.layers[i].y + GL.layers[i].h > GL.vh ? GL.vh : GL.layers[i].y + GL.layers[i].h;
        for (r = y0; r < y1; r++) {
            i32 lr = r - GL.layers[i].y, fr = GL.layers[i].fh - 1 - (i32)((i64)lr * GL.layers[i].fh / GL.layers[i].h);
            for (col = x0; col < x1; col++) {
                i32 fc = (i32)((i64)(col - GL.layers[i].x) * GL.layers[i].fw / GL.layers[i].w);
                const u8 *g = GL.layers[i].frame + ((size_t)fr * (size_t)GL.layers[i].fw + (size_t)fc) * 4;
                const u8 *v = GL.views + (size_t)r * stride + (size_t)col * 4;
                u8 *o = GL.out + (size_t)r * stride + (size_t)col * 4;
                u32 left = 255u - v[3];
                o[0] = (u8)(v[0] + (g[2] * left + 127) / 255);
                o[1] = (u8)(v[1] + (g[1] * left + 127) / 255);
                o[2] = (u8)(v[2] + (g[0] * left + 127) / 255);
                o[3] = 255;
            }
        }
    }
    present_pixels(NULL, GL.out, GL.vw, GL.vh, GL.vw * 4);
    hk_atomic_add(&GL.composed, 1);
}

static bool gl_frame(void *user, int layer, void *share_handle, int width, int height)
{
    const u8 *rgba;
    int w, h;
    (void)user;
    if (GL.refuse || layer < 0 || layer >= GL_LAYERS || !hk_gl_standin_shared(share_handle, &rgba, &w, &h) || w != width || h != height) return false;
    hk_lock_acquire(&GL.lock);
    GL.used = true;
    if (GL.layers[layer].fw != w || GL.layers[layer].fh != h) {
        free(GL.layers[layer].frame);
        GL.layers[layer].frame = (u8 *)hk_alloc((size_t)w * (size_t)h * 4);
        GL.layers[layer].fw = w;
        GL.layers[layer].fh = h;
    }
    memcpy(GL.layers[layer].frame, rgba, (size_t)w * (size_t)h * 4);
    if (GL.layers[layer].w > 0) gl_compose();
    hk_lock_release(&GL.lock);
    hk_atomic_add(&GL.frames, 1);
    return true;
}

static void gl_place(void *user, int layer, int x, int y, int width, int height)
{
    (void)user;
    if (layer < 0 || layer >= GL_LAYERS) return;
    hk_lock_acquire(&GL.lock);
    GL.used = true;
    GL.layers[layer].x = x;
    GL.layers[layer].y = y;
    GL.layers[layer].w = width > 0 ? width : 0;
    GL.layers[layer].h = width > 0 ? height : 0;
    if (width <= 0) {
        free(GL.layers[layer].frame);
        GL.layers[layer].frame = NULL;
        GL.layers[layer].fw = GL.layers[layer].fh = 0;
    }
    hk_lock_release(&GL.lock);
    hk_atomic_add(&GL.places, 1);
}

static bool present_canvas(void *user, void *canvas, int x, int y, int width, int height)
{
    const void *pixels;
    int w, h, stride, r;
    if (!hk_gfx_standin_pixels(canvas, &pixels, &w, &h, &stride)) return false;
    hk_lock_acquire(&GL.lock);
    if (GL.used) {
        /* The shell composes the window: the views' picture is kept, as the shell's snapshot of it, and the window composed. */
        if (GL.vw != w || GL.vh != h) {
            free(GL.views);
            free(GL.out);
            GL.views = (u8 *)hk_alloc((size_t)w * (size_t)h * 4);
            GL.out = (u8 *)hk_alloc((size_t)w * (size_t)h * 4);
            GL.vw = w;
            GL.vh = h;
        }
        for (r = 0; r < h; r++) memcpy(GL.views + (size_t)r * (size_t)w * 4, (const u8 *)pixels + (size_t)r * (size_t)stride, (size_t)w * 4);
        gl_compose();
        hk_lock_release(&GL.lock);
        hk_atomic_add(&W.from_canvas, 1);
        return true;
    }
    hk_lock_release(&GL.lock);
    if (x == 0 && y == 0 && width == w && height == h) present_pixels(user, pixels, w, h, stride);
    else present_pixels_part(user, pixels, w, h, stride, x, y, width, height);
    hk_atomic_add(&W.from_canvas, 1);
    return true;
}

/* A pixel of the last picture as 0xRRGGBB (the picture is opaque). */
static u32 pixel(int x, int y)
{
    u32 v = 0;
    hk_lock_acquire(&W.lock);
    if (W.pixels && x >= 0 && y >= 0 && x < W.width && y < W.height) memcpy(&v, W.pixels + (size_t)y * (size_t)W.stride + 4 * (size_t)x, 4);
    hk_lock_release(&W.lock);
    return v & 0xFFFFFFu;
}

/* How many pixels of a rectangle of the last picture are of one colour. */
static int count_colour(const int r[4], u32 rgb)
{
    int x, y, n = 0;
    for (y = r[1]; y < r[1] + r[3]; y++)
        for (x = r[0]; x < r[0] + r[2]; x++)
            if (pixel(x, y) == rgb) n++;
    return n;
}

static void write_picture(const char *name)
{
    char *path;
    if (!W.dir || !W.pixels) return;
    path = hk_sprintf("%s/%s.bmp", W.dir, name);
    hk_lock_acquire(&W.lock);
    if (hk_gfx_write_bmp(path, W.pixels, W.width, W.height, W.stride)) say("  picture written: %s (%dx%d)", path, W.width, W.height);
    else say("  (could not write %s)", path);
    hk_lock_release(&W.lock);
    free(path);
}

/*
 * Waits until the app is still: no new picture for a moment, during which its main loop has come round twenty times
 * with nothing to do. The second half matters: under qemu a single picture can take the main thread longer than any
 * fixed moment, and a main loop that is not turning is one that is busy drawing it (or has not drawn its first yet).
 * Twenty turns of an idle loop are 160 ms, which outlasts what the framework delays by itself after a touch (a pressed
 * state held for 64 ms, a tap recognised after 100).
 */
/* When megler_run was called, and whether settle() has said what the start cost. */
static u64 g_start_ns;
static bool g_start_said;

static void settle(void)
{
    hk_java_stats st;
    i32 pictures, polls;
    u64 since = hk_time_monotonic_ns();
    int tries;
    hk_java_get_stats(&st);
    /* (The views' pictures: an OpenGL surface drawn continuously is a new picture every frame and never still.) */
    pictures = st.frames;
    polls = st.polls;
    for (tries = 0; tries < 6000 && running(); tries++) {
        hk_sleep_ns(10000000ull);
        hk_java_get_stats(&st);
        if (st.frames != pictures) {
            pictures = st.frames;
            polls = st.polls;
            since = hk_time_monotonic_ns();
        } else if (st.polls - polls >= 20 && hk_time_monotonic_ns() - since >= 200000000ull) {
            break;
        }
    }
    /*
     * The first time after the start: what the start cost, as a measure of the interpreter on real code -- the instructions
     * every thread executed, and the time from megler_run to the last picture before the app came to rest.
     */
    if (g_start_ns && !g_start_said) {
        g_start_said = true;
        say("  the start: %.1f M instructions on all threads, %.2f s to the last picture before the app rested", (double)hk_dvm_executed_all() / 1e6,
            (double)(since - g_start_ns) / 1e9);
    }
}

static void tap(int x, int y)
{
    megler_touch(0, 1, (float)x, (float)y);
    hk_sleep_ns(60000000ull);
    megler_touch(2, 1, (float)x, (float)y);
}

/*
 * The shell's keyboard, played by the test: what the app asked of it is counted and kept, and the test types by calling
 * what a shell calls (megler_text_commit, megler_text_key, megler_keyboard_closed, megler_keyboard_height). Both of the host's
 * functions are called on the app's main thread.
 */
static struct {
    volatile i32 shown, hidden;        /* how many times each was asked */
    volatile i32 kind, flags, enter;   /* of the last show */
    u32 thread;                        /* the thread the last request came on */
} K;

static void keyboard_show(void *user, int kind, int flags, int enter)
{
    (void)user;
    K.kind = kind;
    K.flags = flags;
    K.enter = enter;
    K.thread = hk_thread_self_id();
    hk_atomic_add(&K.shown, 1);
}

static void keyboard_hide(void *user)
{
    (void)user;
    K.thread = hk_thread_self_id();
    hk_atomic_add(&K.hidden, 1);
}

static int g_screen_w = 480, g_screen_h = 800, g_screen_dpi = 160;

void hk_tests_java_set_screen(int width, int height, int dpi)
{
    g_screen_w = width;
    g_screen_h = height;
    g_screen_dpi = dpi;
}

/* Whether the app's manifest asks for portrait (demos does; hello does not say). */
static bool g_portrait;
/* Not one of the test apps: nothing is known of what its manifest says. */
static bool g_any_app;
static bool g_crash_app;
static volatile i32 g_exited;
static int g_status;
static char g_reason[1600];
/* Started by the test itself, past megler_run: see start(). */
static bool g_direct;

/* Whether the app is still there to wait for. */
static bool running(void)
{
    const char *reason = NULL;
    if (!g_direct) {
        int tries;
        if (megler_running()) return true;
        /*
         * The core says "not running" a moment before it tells the shell why (hk_run_ended: the flag, a line in the log, then
         * on_exit), so the end is there to read only when on_exit has been through. The crash test once read the status and the
         * reason in that moment and found neither.
         */
        for (tries = 0; tries < 500 && !g_exited; tries++) hk_sleep_ns(10000000ull);
        return false;
    }
    if (!g_exited && hk_java_ended(&g_status, &reason)) {
        hk_strlcpy(g_reason, reason ? reason : "", sizeof(g_reason));
        hk_atomic_xchg(&g_exited, 1);
    }
    return !g_exited;
}

static void on_exit_cb(void *user, int status, const char *reason)
{
    (void)user;
    g_status = status;
    hk_strlcpy(g_reason, reason ? reason : "", sizeof(g_reason));
    hk_atomic_xchg(&g_exited, 1);
}

static bool start(const char *apk, const char *android_root)
{
    static megler_run_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.apk_path = apk;
    cfg.android_root = android_root;
    /* Unless another is asked for, a small phone at Android's base density: a dp is a pixel, and the layout's numbers can be read off the picture. */
    cfg.width = g_screen_w;
    cfg.height = g_screen_h;
    cfg.density_dpi = g_screen_dpi;
    cfg.on_exit = on_exit_cb;
    cfg.window.present_pixels = present_pixels;
    cfg.window.present_pixels_part = present_pixels_part;
    cfg.gfx = *hk_gfx_standin();
    cfg.gfx.present = present_canvas;
    cfg.gfx.gl_frame = gl_frame;
    cfg.gfx.gl_place = gl_place;
    cfg.keyboard.show = keyboard_show;
    cfg.keyboard.hide = keyboard_hide;
    /* The framework says which window each touch went to when it finds this in the environment: asked for with verbose 2. */
    if (g_verbose >= 2 && hk_bionic_find("setenv")) ((int (*)(const char *, const char *, int))hk_bionic_find("setenv"))("TL_TOUCH_LOG", "1", 1);
    /* And every window's views with their frames, every three seconds while it lays out: verbose 3. */
    if (g_verbose >= 3 && hk_bionic_find("setenv")) ((int (*)(const char *, const char *, int))hk_bionic_find("setenv"))("TL_VIEW_DUMP", "1", 1);
    say("a Java app through megler_run (%s on the Android root %s)", apk, android_root);
    {
        /* What the shell reads before it starts an app: this is a Java app it can run, and how it asks to be held. */
        char *report = megler_apk_report(apk);
        CHECK(report && strstr(report, "\"engine\":\"java\"") && strstr(report, "\"runnable\":true"));
        /* The app whose launcher entries are aliases, the first one disabled: what is started is what the enabled one stands for. */
        if (g_crash_app) CHECK(report && strstr(report, "\"activity\":\"megler.test.crash.First\""));
        if (!g_any_app) CHECK(report && strstr(report, g_portrait ? "\"orientation\":\"portrait\"" : "\"orientation\":\"unspecified\""));
        megler_free(report);
    }
    g_start_ns = hk_time_monotonic_ns();
    if (megler_run(&cfg)) return true;
#if !HK_HOST_ARM32
    {
        /*
         * megler_run refuses every app on a host that cannot run guest code. A Java app with no native library needs none, so
         * here the driver is started as megler_run would have started it: the APK read, the app's folders named, the driver's start.
         */
        static hk_apk_info info;
        say("  (megler_run: %s -- the Java driver is started directly)", megler_last_error());
        if (hk_apk_read_info(apk, &info) && !strcmp(info.engine, "java")) {
            hk_run_files(megler_data_dir(), info.package, &cfg.storage);
            g_direct = hk_driver_java.start(&cfg, &info);
            if (g_direct) return true;
        }
    }
#endif
    g_checks++;
    g_failed++;
    say("  FAIL: not started: %s", megler_last_error());
    return false;
}

/* Back, and the app's end: the activity taken down in order, the main loop ended, status 0. */
static void back_and_end(const char *destroyed_line, int *at)
{
    int tries;
    megler_key(4, true);
    megler_key(4, false);
    expect_line(destroyed_line, at, 30);
    for (tries = 0; tries < 3000 && running(); tries++) hk_sleep_ns(10000000ull);
    CHECK(g_exited);
    CHECK(g_exited && g_status == 0);
    if (g_exited) say("  the app ended with status %d%s%s", g_status, g_reason[0] ? ": " : "", g_reason);
    else say("  the app did not end after Back");
}

/* --------------------------------------------------------------- measuring */

/*
 * HK_JAVA_PROFILE in the environment names a step to put under the runtime's profile (hk_dvm_profile): "start" (from
 * before the app starts to its first pictures), and for `demos` "dialog" (from the finger on the first row to the dialog
 * on the screen) or "scroll" (one drag of the list). The report is printed when the step is over. Without it nothing is
 * profiled and the steps are only timed.
 */
static const char *g_profile_step;

static bool profiled(const char *step) { return g_profile_step && !strcmp(g_profile_step, step); }

static void profile_line(const char *text, void *user)
{
    (void)user;
    say("  %s", text);
}

static void profile_begin(const char *step)
{
    if (profiled(step)) hk_dvm_profile(true);
}

static void profile_end(const char *step)
{
    if (!profiled(step)) return;
    hk_dvm_profile(false);
    say("  the profile of \"%s\":", step);
    /* The twenty methods that cost most; HK_JAVA_PROFILE_TOP asks for more (every method that ran, with a large number). */
    hk_dvm_profile_report(getenv("HK_JAVA_PROFILE_TOP") && atoi(getenv("HK_JAVA_PROFILE_TOP")) > 0 ? atoi(getenv("HK_JAVA_PROFILE_TOP")) : 20, profile_line, NULL);
}

/* ------------------------------------------------------------------ hello */

typedef struct cost {
    hk_dvm_stats dvm;
    hk_java_stats java;
    u64 bitmap_bytes, ns;
    u32 bitmaps;
} cost;

static void cost_now(cost *c)
{
    hk_dvm_get_stats(&c->dvm);
    hk_java_get_stats(&c->java);
    hk_java_gfx_stats(&c->bitmap_bytes, &c->bitmaps);
    c->ns = hk_time_monotonic_ns();
}

static void say_cost(const char *what, const cost *a, const cost *b)
{
    double seconds = (double)(b->ns - a->ns) / 1e9;
    i32 pictures = b->java.frames - a->java.frames, turns = b->java.polls - a->java.polls;
    u64 objects = b->dvm.objects - a->dvm.objects, bytes = b->dvm.object_bytes - a->dvm.object_bytes, steps = b->dvm.instructions - a->dvm.instructions;
    say("  %s, %.1f s: %d pictures, %d turns of the main loop; %u objects (%u KB) made; %u thousand instructions; bitmaps now %u (%u KB)", what, seconds, (int)pictures,
        (int)turns, (unsigned)objects, (unsigned)(bytes >> 10), (unsigned)(steps / 1000), (unsigned)b->bitmaps, (unsigned)(b->bitmap_bytes >> 10));
    if (pictures > 0)
        say("    for each picture: %u objects, %u bytes, %u thousand instructions", (unsigned)(objects / (u64)pictures), (unsigned)(bytes / (u64)pictures),
            (unsigned)(steps / 1000 / (u64)pictures));
}

static int run_hello(void)
{
    int at = -1, layout, button[4], swatch[4], mark[4], text[4], root[4], i, black_before, black_after, dp = g_screen_dpi / 160;
    i32 pictures;
    cost c0, c1;
    memset(button, 0, sizeof(button));
    memset(swatch, 0, sizeof(swatch));
    memset(mark, 0, sizeof(mark));
    memset(text, 0, sizeof(text));

    /* The start, in Android's order: the Application first, then the activity's three steps, the views found by their ids in between. */
    expect_line("I/HelloTest: App.onCreate package=megler.test.hello", &at, 120);
    /* Its class loader: a PathClassLoader over the APK, the one loader of all its classes, which finds the APK's other files too; the system's classes name Android's boot loader. */
    expect_line("I/HelloTest: loader=dalvik.system.PathClassLoader same=true manifest=true loads=true missing=true system=java.lang.BootClassLoader apk=true", &at, 60);
    expect_line("I/HelloTest: onCreate app=1 state=none", &at, 60);
    expect_line("I/HelloTest: views text=\"not pressed\" button=\"Press\"", &at, 60);
    /* A copy of a message is a message (Message.obtain(Message) answered null, and no button of a support-library dialog did anything)... */
    expect_line("I/HelloTest: message copy=true", &at, 30);
    expect_line("I/HelloTest: onStart", &at, 30);
    expect_line("I/HelloTest: onResume", &at, 30);
    layout = wait_line("I/HelloTest: layout focus=true", 0, 30);
    CHECK(layout >= 0);
    /* ... and the copy, sent, arrives at the first one's handler saying what the first said. */
    CHECK(wait_line("I/HelloTest: message what=7 arg=1,2 obj=word when=true", 0, 30) >= 0);
    {
        /* A launcher's questions (AppWidgetHostView, LauncherApps): Android 9's padding of a widget's frame, 8 dp each way; this app's one launcher activity, with its label and icon. */
        char want[200];
        snprintf(want, sizeof(want), "I/HelloTest: launcher padding=Rect(%d, %d - %d, %d)", 8 * dp, 8 * dp, 8 * dp, 8 * dp);
        CHECK(find_line(want, 0) >= 0);
        CHECK(find_line("I/HelloTest: launcher activities=1 first=megler.test.hello.MainActivity label=Hello icon=true resolved=true", 0) >= 0);
    }
    /* A row of no fixed width shares what its weighted, widthless children asked for equally among them, as Android does for an app written for Android 7 or later. */
    CHECK(find_line("I/HelloTest: row unlimited=140:70+70 exact=200:100+100", 0) >= 0);
    {
        /* A switch is two thumbs and its track's padding wide, as Android measures it, and wider than high; the track's picture is a few pixels that are stretched. */
        int sw = 0, sh = 0, thumb = 0, trackpad = 0, track = 0, line = find_line("I/HelloTest: switch width=", 0);
        CHECK(line >= 0 && sscanf(strstr(line_at(line), "switch width="), "switch width=%d height=%d thumb=%d trackpad=%d track=%d", &sw, &sh, &thumb, &trackpad, &track) == 5);
        CHECK(thumb > 0 && sw == 2 * thumb + trackpad && sw > sh && sw > track);
        say("  a switch: %d x %d, its thumb %d, its track's padding %d, the track's picture %d wide", sw, sh, thumb, trackpad, track);
    }
    wait_line("I/HelloTest: layout text=", layout, 10);
    CHECK(rect_in(line_at(layout), "button", button) && rect_in(line_at(find_line("layout swatch=", layout)), "swatch", swatch));
    CHECK(rect_in(line_at(find_line("layout mark=", layout)), "mark", mark) && rect_in(line_at(find_line("layout text=", layout)), "text", text));
    CHECK(sscanf(strstr(line_at(layout), "root=") ? strstr(line_at(layout), "root=") + 5 : "", "%dx%d", &root[2], &root[3]) == 2);
    say("  laid out: text %d,%d %dx%d; button %d,%d %dx%d; the drawn view %d,%d %dx%d; the picture %d,%d %dx%d", text[0], text[1], text[2], text[3], button[0], button[1], button[2],
        button[3], swatch[0], swatch[1], swatch[2], swatch[3], mark[0], mark[1], mark[2], mark[3]);
    /* What the layout file says, in pixels at this density: full width, the button 64 dp high, the drawn view 100, the picture 64 square, one under the other. */
    CHECK(button[2] == g_screen_w && button[3] == 64 * dp && swatch[2] == g_screen_w && swatch[3] == 100 * dp && mark[2] == 64 * dp && mark[3] == 64 * dp && text[2] == g_screen_w);
    CHECK(button[1] == text[1] + text[3] && swatch[1] == button[1] + button[3] && mark[1] == swatch[1] + swatch[3]);

    /* The first picture. */
    settle();
    profile_end("start");
    CHECK(W.pictures >= 1 && W.width == g_screen_w && W.height == g_screen_h);
    write_picture("hello-1-started");
    /* The view that draws for itself: its field, the red rectangle at (10,10)-(50,30), the green one moved to (60,10), 20 wide until pressed. */
    CHECK(pixel(swatch[0] + 300, swatch[1] + 60 * dp) == 0x2060C0);
    CHECK(pixel(swatch[0] + 20, swatch[1] + 20) == 0xFF0000 && pixel(swatch[0] + 9, swatch[1] + 20) == 0x2060C0 && pixel(swatch[0] + 50, swatch[1] + 20) == 0x2060C0);
    CHECK(pixel(swatch[0] + 70, swatch[1] + 20) == 0x00C000 && pixel(swatch[0] + 90, swatch[1] + 20) == 0x2060C0);
    {
        /* Two letters made twice the paint's size by a span are drawn as the same letters by a paint of twice the size: the same count of pixels in the colour only they have. */
        int by_span[4], by_paint[4], spanned, plain;
        by_span[0] = swatch[0] + 200;
        by_paint[0] = swatch[0] + 340;
        by_span[1] = by_paint[1] = swatch[1] + 35;
        by_span[2] = by_paint[2] = 140;
        by_span[3] = by_paint[3] = 65;
        spanned = count_colour(by_span, 0x800080);
        plain = count_colour(by_paint, 0x800080);
        say("  two letters at twice the size: %d pixels when a span says so, %d when the paint does", spanned, plain);
        CHECK(plain > 0 && spanned == plain);
    }
    /* A shape with no colour of its own, tinted, is filled with the tint, and its rounded corner leaves the field showing: Material's tab indicator. */
    CHECK(pixel(swatch[0] + 40, swatch[1] + 65) == 0xC06020 && pixel(swatch[0] + 10, swatch[1] + 50) == 0x2060C0 && pixel(swatch[0] + 75, swatch[1] + 65) == 0x2060C0);
    /* The PNG from the resources, 16 pixels stretched to 64 dp: orange left, green right, a transparent border through which the layout's grey shows. */
    CHECK(pixel(mark[0] + mark[2] / 4, mark[1] + mark[3] / 2) == 0xFF8000 && pixel(mark[0] + 3 * mark[2] / 4, mark[1] + mark[3] / 2) == 0x00A040);
    CHECK(pixel(mark[0] + 1, mark[1] + 1) == 0xE0E0E0 && pixel(mark[0] + mark[2] + 100, mark[1] + mark[3] / 2) == 0xE0E0E0);
    /* A surface nothing has drawn on leaves the view behind it to be seen; one with a frame shows the frame (the bottom right corner). */
    CHECK(wait_line("I/HelloTest: surface frame posted", 0, 30) >= 0);
    settle();
    CHECK(pixel(g_screen_w - 60, g_screen_h - 20) == 0x00A000 && pixel(g_screen_w - 20, g_screen_h - 20) == 0xC000C0);
    /* The text: the stand-in draws a box for each letter, in the text's colour. */
    black_before = count_colour(text, 0x000000);
    CHECK(black_before > 0);

    /* A touch on the button: its listener runs, the text changes, the view is asked to draw again, and the next picture shows both. */
    pictures = W.pictures;
    tap(button[0] + button[2] / 2, button[1] + button[3] / 2);
    expect_line("I/HelloTest: onClick presses=1 text=\"pressed 1\"", &at, 30);
    for (i = 0; i < 500 && W.pictures == pictures; i++) hk_sleep_ns(10000000ull);
    settle();
    CHECK(W.pictures > pictures);
    write_picture("hello-2-pressed");
    CHECK(pixel(swatch[0] + 90, swatch[1] + 20) == 0x00C000 && pixel(swatch[0] + 110, swatch[1] + 20) == 0x2060C0);
    black_after = count_colour(text, 0x000000);
    say("  the text's pixels: %d black before the press (\"not pressed\"), %d after (\"pressed 1\")", black_before, black_after);
    /* Ten letters became eight. */
    CHECK(black_after > 0 && black_after * 10 == black_before * 8);
    hk_sleep_ns(150000000ull);
    tap(button[0] + 10, button[1] + 10);
    expect_line("I/HelloTest: onClick presses=2 text=\"pressed 2\"", &at, 30);
    settle();
    write_picture("hello-3-pressed-twice");
    CHECK(pixel(swatch[0] + 110, swatch[1] + 20) == 0x00C000);
    /* A touch beside the button is not a press. */
    tap(swatch[0] + 200, swatch[1] + 50);
    hk_sleep_ns(300000000ull);
    CHECK(find_line("onClick presses=3", 0) < 0);

    /* What it costs, since nothing is collected: standing still, and being pressed ten times. */
    hk_dvm_count_instructions(true);
    settle();
    cost_now(&c0);
    hk_sleep_ns(3000000000ull);
    cost_now(&c1);
    say_cost("idle", &c0, &c1);
    cost_now(&c0);
    for (i = 0; i < 10; i++) {
        char want[64];
        /*
         * One press at a time, each waited for until the app has drawn what came of it and is still: the framework shows
         * a pressed button for a moment after the finger leaves and then un-presses it by a delayed message, and when
         * that message runs between the next press's down and up, that press is no click. On a phone no finger is that
         * quick; here a picture can take the main thread longer than the gap between two presses.
         */
        snprintf(want, sizeof(want), "onClick presses=%d ", 3 + i);
        tap(button[0] + 40, button[1] + 30);
        wait_line(want, 0, 30);
        settle();
    }
    settle();
    cost_now(&c1);
    say_cost("ten presses", &c0, &c1);
    hk_dvm_count_instructions(false);
    expect_line("I/HelloTest: onClick presses=12", &at, 30);
    say("  in all so far: %u classes linked, %u objects (%u KB), %u KB of tables and classes", (unsigned)c1.dvm.classes_linked, (unsigned)c1.dvm.objects,
        (unsigned)(c1.dvm.object_bytes >> 10), (unsigned)(c1.dvm.meta_bytes >> 10));

    /* The shell hides the app and shows it again. */
    megler_set_paused(true);
    expect_line("I/HelloTest: onPause draws=true", &at, 30);
    megler_set_paused(false);
    expect_line("I/HelloTest: onResume", &at, 30);

    /* Back: down in order, and out. */
    megler_key(4, true);
    megler_key(4, false);
    expect_line("I/HelloTest: onPause", &at, 30);
    expect_line("I/HelloTest: onStop", &at, 30);
    at--;
    back_and_end("I/HelloTest: onDestroy presses=12", &at);
    say_app_lines("/HelloTest: ");
    return g_failed;
}

/* ------------------------------------------------------------- the keyboard */

/* Waits until the app has asked for the keyboard `count` times in all, a few seconds at most. */
static bool shown_times(i32 count)
{
    int tries;
    for (tries = 0; tries < 1000 && K.shown < count && running(); tries++) hk_sleep_ns(10000000ull);
    return K.shown == count;
}

/*
 * The row "typing" of the demos list opens a screen with four text fields (core/tests/app/demos, Typing). The test is
 * the keyboard: a touch of a field must ask for it, with the field's kind; what it then sends must arrive in the field,
 * in order, text and editing keys alike; and the field's own pixels must show it.
 */
static void type_into_fields(const int row[4], int *at)
{
    int name[4], number[4], secret[4], note[4], layout, typed, fewer;
    i32 shown, hidden;
    memset(name, 0, sizeof(name));
    say("the keyboard: text typed into the demos app's fields");
    /* Typed while no field has the focus: it goes nowhere, and the framework's log says so (megler.InputMethods in the overlay). */
    megler_text_commit("x");
    expect_line("java: the first thing the keyboard sent (text) reached the core and waits for the app's main thread", at, 30);
    expect_line("I/Megler: keyboard: the first thing the keyboard sent, typed text (1 character), finds no view with the focus: it goes nowhere", at, 30);
    tap(row[0] + row[2] / 2, row[1] + row[3] * 7 + row[3] / 2);
    expect_line("I/DemoTest: list: clicked row 7 \"typing\"", at, 30);
    expect_line("I/DemoTest: typing: created", at, 30);
    layout = wait_line("I/DemoTest: typing: layout name=", *at, 30);
    CHECK(layout >= 0 && rect_in(line_at(layout), "name", name) && rect_in(line_at(layout), "number", number) && rect_in(line_at(layout), "secret", secret)
          && rect_in(line_at(layout), "note", note));
    settle();
    if (layout < 0) return;

    /* A touch of the first field: the keyboard is asked for, for a line of text whose Enter is Done, on the app's main thread and not on this one. */
    shown = K.shown;
    tap(name[0] + 60, name[1] + name[3] / 2);
    CHECK(shown_times(shown + 1) && K.kind == MEGLER_KEYBOARD_TEXT && K.flags == 0 && K.enter == MEGLER_ENTER_DONE);
    CHECK(K.thread != 0 && K.thread != hk_thread_self_id());
    settle();
    CHECK(count_colour(name, 0x102030) == 0);
    /* The keyboard comes up and says how much it covers before anything is typed, as a phone's shell does. */
    megler_keyboard_height(g_screen_h * 2 / 5);
    settle();
    /* Text, as the keyboard commits it. */
    megler_text_commit("Hello");
    expect_line("I/DemoTest: typing: name=\"Hello\" cursor=5", at, 30);
    /* The log's next word on it: once it reaches a field, which one and what the text's length did; the text itself is not there. */
    expect_line("I/Megler: keyboard: typed text (5 characters) now goes to android.widget.EditText, which has the focus and an input connection; "
                "its text was 0 characters long and is 5 now", at, 30);
    settle();
    write_picture("demos-8-typed");
    typed = count_colour(name, 0x102030);
    CHECK(typed > 0);
    /* Backspace: one letter fewer, in the text and in the picture (the stand-in draws a box a letter). */
    megler_text_key(MEGLER_TEXT_BACKSPACE);
    expect_line("I/DemoTest: typing: name=\"Hell\" cursor=4", at, 30);
    settle();
    fewer = count_colour(name, 0x102030);
    say("  the field's own pixels: %d of the text's colour for \"Hello\", %d for \"Hell\"", typed, fewer);
    CHECK(fewer > 0 && fewer * 5 == typed * 4);
    /* The cursor keys, which go as keys, between two pieces of text: everything arrives in the order it was sent. */
    megler_text_key(MEGLER_TEXT_LEFT);
    megler_text_key(MEGLER_TEXT_LEFT);
    megler_text_commit("X");
    expect_line("I/DemoTest: typing: name=\"HeXll\" cursor=3", at, 30);
    megler_text_key(MEGLER_TEXT_HOME);
    megler_text_commit("\xD0\x81\xD0\xB6");                     /* two Cyrillic letters, UTF-8 */
    expect_line("I/DemoTest: typing: name=\"\xD0\x81\xD0\xB6HeXll\" cursor=2", at, 30);
    megler_text_key(MEGLER_TEXT_END);
    megler_text_key(MEGLER_TEXT_DELETE);                              /* nothing after the end to delete */
    megler_text_commit("!");
    expect_line("I/DemoTest: typing: name=\"\xD0\x81\xD0\xB6HeXll!\" cursor=8", at, 30);
    megler_text_key(MEGLER_TEXT_HOME);
    megler_text_key(MEGLER_TEXT_DELETE);
    expect_line("I/DemoTest: typing: name=\"\xD0\xB6HeXll!\" cursor=0", at, 30);
    /* Enter in a field of one line is the field's action: IME_ACTION_DONE, 6. */
    megler_text_key(MEGLER_TEXT_ENTER);
    expect_line("I/DemoTest: typing: action name 6", at, 30);
    /* Done put the keyboard away: the shell says it covers nothing now. */
    megler_keyboard_height(0);
    settle();

    /* Another field: asked again, for a number with a decimal point, whose Enter is Next. */
    shown = K.shown;
    tap(number[0] + 60, number[1] + number[3] / 2);
    CHECK(shown_times(shown + 1) && K.kind == MEGLER_KEYBOARD_NUMBER && K.flags == MEGLER_KEYBOARD_F_DECIMAL && K.enter == MEGLER_ENTER_NEXT);
    megler_text_commit("4");
    megler_text_commit("2.5");
    expect_line("I/DemoTest: typing: number=\"42.5\"", at, 30);
    /* A password. */
    shown = K.shown;
    tap(secret[0] + 60, secret[1] + secret[3] / 2);
    CHECK(shown_times(shown + 1) && K.kind == MEGLER_KEYBOARD_PASSWORD);
    megler_text_commit("pw");
    expect_line("I/DemoTest: typing: secret=\"pw\"", at, 30);
    /* A note of several lines: Enter is a new line there, and the keyboard is told so. */
    shown = K.shown;
    tap(note[0] + 60, note[1] + 30);
    CHECK(shown_times(shown + 1) && K.kind == MEGLER_KEYBOARD_TEXT && K.flags == (MEGLER_KEYBOARD_F_MULTILINE | MEGLER_KEYBOARD_F_CAP_SENTENCES) && K.enter == MEGLER_ENTER_NEWLINE);
    megler_text_commit("one");
    megler_text_key(MEGLER_TEXT_ENTER);
    megler_text_commit("two");
    expect_line("I/DemoTest: typing: note=\"one|two\" cursor=7", at, 30);
    settle();
    write_picture("demos-9-fields");

    /* The keyboard says how much of the surface it covers, then that the user put it away: the app goes on, and the next touch of a field asks again. */
    hidden = K.hidden;
    megler_keyboard_height(g_screen_h * 2 / 5);
    hk_sleep_ns(300000000ull);
    megler_keyboard_closed();
    megler_keyboard_height(0);
    hk_sleep_ns(300000000ull);
    settle();
    CHECK(running() && K.hidden == hidden);
    shown = K.shown;
    tap(name[0] + 60, name[1] + name[3] / 2);
    CHECK(shown_times(shown + 1) && K.kind == MEGLER_KEYBOARD_TEXT);
    settle();
    /* Back: the screen goes, and with it the field that wanted the keyboard. */
    megler_key(4, true);
    megler_key(4, false);
    expect_line("I/DemoTest: typing: destroyed name=\"\xD0\xB6HeXll!\"", at, 30);
    settle();
    CHECK(K.hidden > hidden);
    say("  the keyboard was asked for %d times and sent away %d times", (int)K.shown, (int)K.hidden);
}

/* The last line from `from` on that holds `text`, or -1. */
static int last_line(const char *text, int from)
{
    int found = -1, i;
    while ((i = find_line(text, found >= 0 ? found + 1 : from)) >= 0) found = i;
    return found;
}

/*
 * Dialogs with a text field, over which the keyboard comes up (core/tests/app/demos, KeyboardDialogs): what each window's
 * softInputMode asks is done, as Android does it. A keyboard that covers 500 of the 800 rows leaves 300.
 *  - The focused field must lie wholly between the top of the screen and the top of the keyboard.
 *  - A dialog that is resized ("top", which asks so; "scroll", which leaves it unspecified and holds a ScrollView) must be
 *    centred in those 300 rows.
 *  - One that is panned ("bottom", which asks so; "theme", which says nothing and has its theme's adjustPan) must keep its
 *    size and be moved up exactly as far as brings its field's bottom edge to the keyboard's top.
 *  - Once the keyboard has gone, each is where it was before.
 * On the phone the shell said the keyboard covered 513 of 1280 rows, and Apktool M's search dialog had its field above the
 * screen's top edge.
 */
static void keyboard_and_dialogs(const int row[4], int *at)
{
    static const char *const tags[] = { "top", "bottom", "scroll", "theme" };
    static const bool panned[] = { false, true, false, true };
    const int kbd = 500, space = g_screen_h - kbd;
    int k;
    say("the keyboard over dialogs: resized, panned, and as the contents or the theme decide");
    tap(row[0] + row[2] / 2, row[1] + row[3] * 8 + row[3] / 2);
    expect_line("I/DemoTest: list: clicked row 8 \"dialogs and the keyboard\"", at, 30);
    expect_line("I/DemoTest: keyboard: created", at, 30);
    for (k = 0; k < 4; k++) {
        char want[80];
        int field0[4] = { 0 }, window0[4] = { 0 }, field[4] = { 0 }, window[4] = { 0 }, line;
        i32 shown;
        snprintf(want, sizeof(want), "I/DemoTest: keyboard: %s field=", tags[k]);
        line = wait_line(want, *at + 1, 30);
        CHECK(line >= 0);
        if (line < 0) return;
        *at = line;
        settle();
        line = last_line(want, *at);
        CHECK(rect_in(line_at(line), "field", field0) && rect_in(line_at(line), "window", window0));
        /* A touch of the field asks for the keyboard; the keyboard says how much it covers. */
        shown = K.shown;
        tap(field0[0] + 30, field0[1] + field0[3] / 2);
        CHECK(shown_times(shown + 1));
        megler_keyboard_height(kbd);
        settle();
        line = last_line(want, *at);
        CHECK(rect_in(line_at(line), "field", field) && rect_in(line_at(line), "window", window));
        say("  %s: the field at rows %d to %d and the window at %d to %d, under a keyboard from row %d; without it %d to %d and %d to %d", tags[k], field[1],
            field[1] + field[3], window[1], window[1] + window[3], space, field0[1], field0[1] + field0[3], window0[1], window0[1] + window0[3]);
        CHECK(field[1] >= 0 && field[1] + field[3] <= space);
        if (panned[k]) {
            CHECK(field[1] + field[3] == space && window[3] == window0[3]);
        } else {
            CHECK(window[3] <= space && abs(window[1] - (space - window[3]) / 2) <= 1);
        }
        /* The keyboard put away by the user: the dialog goes back where it was. Back then closes it, and the next one comes. */
        megler_keyboard_closed();
        megler_keyboard_height(0);
        settle();
        line = last_line(want, *at);
        CHECK(rect_in(line_at(line), "field", field) && rect_in(line_at(line), "window", window));
        CHECK(field[1] == field0[1] && window[1] == window0[1] && window[3] == window0[3]);
        megler_key(4, true);
        megler_key(4, false);
        snprintf(want, sizeof(want), "I/DemoTest: keyboard: %s closed", tags[k]);
        expect_line(want, at, 30);
    }
    expect_line("I/DemoTest: keyboard: all shown", at, 30);
    settle();
    megler_key(4, true);
    megler_key(4, false);
    settle();
}

/* ------------------------------------------------------------------ demos */

/* The last picture, kept: to be compared with a later one. */
static u8 *snapshot(void)
{
    u8 *copy = NULL;
    hk_lock_acquire(&W.lock);
    if (W.pixels) {
        copy = (u8 *)hk_alloc((size_t)W.stride * (size_t)W.height);
        memcpy(copy, W.pixels, (size_t)W.stride * (size_t)W.height);
    }
    hk_lock_release(&W.lock);
    return copy;
}

/* How many pixels of a rectangle differ between a kept picture and the last one. */
static int count_changed(const u8 *before, const int r[4])
{
    int x, y, n = 0;
    if (!before) return 0;
    hk_lock_acquire(&W.lock);
    for (y = r[1]; y < r[1] + r[3]; y++)
        for (x = r[0]; x < r[0] + r[2]; x++)
            if (x >= 0 && y >= 0 && x < W.width && y < W.height
                && memcmp(before + (size_t)y * (size_t)W.stride + 4 * (size_t)x, W.pixels + (size_t)y * (size_t)W.stride + 4 * (size_t)x, 3) != 0)
                n++;
    hk_lock_release(&W.lock);
    return n;
}

/* The smallest rectangle that holds every pixel that differs between a kept picture and the last one; false when none does. */
static bool changed_box(const u8 *before, int r[4])
{
    int x, y, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    if (!before) return false;
    hk_lock_acquire(&W.lock);
    for (y = 0; y < W.height; y++)
        for (x = 0; x < W.width; x++)
            if (memcmp(before + (size_t)y * (size_t)W.stride + 4 * (size_t)x, W.pixels + (size_t)y * (size_t)W.stride + 4 * (size_t)x, 3) != 0) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    hk_lock_release(&W.lock);
    r[0] = x0;
    r[1] = y0;
    r[2] = x1 - x0 + 1;
    r[3] = y1 - y0 + 1;
    return x1 >= 0;
}

/*
 * The dialog, once the app has said where it is: its panel drawn over a list that is dimmed around it, and a finger on its
 * "Yes" button, which is a window of its own above the list's. `list` is the picture of the list alone.
 */
static void answer_dialog(const u8 *list, int *at, const char *picture)
{
    int showing = wait_line("I/DemoTest: dialog: showing=true", *at + 1, 60), window[4], yes[4], outside[4];
    memset(window, 0, sizeof(window));
    memset(yes, 0, sizeof(yes));
    CHECK(showing >= 0 && rect_in(line_at(showing), "window", window) && rect_in(line_at(showing), "yes", yes));
    if (showing < 0 || yes[2] <= 0) return;
    *at = showing;
    settle();
    write_picture(picture);
    say("  the dialog is at %d,%d %dx%d, its Yes at %d,%d %dx%d", window[0], window[1], window[2], window[3], yes[0], yes[1], yes[2], yes[3]);
    /* Inside the window, away from its edges, the picture is no longer the list's; and the list around it is dimmed, so it changed too. */
    {
        int inner[4];
        inner[0] = window[0] + window[2] / 4;
        inner[1] = window[1] + window[3] / 4;
        inner[2] = window[2] / 2;
        inner[3] = window[3] / 2;
        CHECK(count_changed(list, inner) > inner[2] * inner[3] / 2);
    }
    outside[0] = 0;
    outside[1] = 0;
    outside[2] = W.width;
    outside[3] = 40;
    CHECK(count_changed(list, outside) > outside[2] * outside[3] / 2);
    /* A finger beside the button does not answer; one on it does, and the dialog is gone. */
    tap(yes[0] + yes[2] / 2, yes[1] + yes[3] / 2);
    expect_line("I/DemoTest: dialog: answered yes", at, 30);
}

/*
 * What the app's first dialog may take from the finger to its own report of itself: thirty million bytecode instructions
 * (it takes three; it took seven hundred million, thirty seconds under qemu and fourteen on a phone, when the framework
 * answered one question about one resource's name by reading the names of all of them in every language: docs/JAVA.md,
 * "What an AlertDialog cost"), and five seconds under qemu, where the whole app starts in about one.
 */
#define DIALOG_BUDGET_INSTRUCTIONS 30000000ull
#define DIALOG_BUDGET_NS 5000000000ull

static int run_demos(void)
{
    static const char *const chain[] = {
        "I/DemoTest: preferences: runs=1 name=h\xc3\xa9llo flag=true big=1099511627776 ratio=0.25 missing=default contains=true committed=true keys=6",
        "I/DemoTest: delayed: order=abc after the delay on the main thread",
        "I/DemoTest: task: background sum=500500 progress=pre;500;1000; delivered on the main thread",
        "I/DemoTest: second: asked for",
        "I/DemoTest: second: created with number=21 text=from the list",
    };
    int at = -1, shown, row[4], back[4], toast[4], place, changed;
    u8 *list, *with_toast;
    u64 dialog_started;
    hk_dvm_stats dialog_before;
    size_t i;
    memset(row, 0, sizeof(row));
    memset(back, 0, sizeof(back));
    memset(toast, 0, sizeof(toast));
    expect_line("I/DemoTest: list: created with 40 rows", &at, 120);
    shown = wait_line("I/DemoTest: list: shown rows=", 0, 60);
    CHECK(shown >= 0 && rect_in(line_at(shown), "row0", row));
    /* A layout of the list whose data has not changed puts its rows back without asking the adapter for any of them again. */
    CHECK(wait_line("I/DemoTest: list: laid out again, rows bound again 0 rows=", 0, 30) >= 0);
    settle();
    write_picture("demos-1-list");
    profile_end("start");
    list = snapshot();
    say("  the list's first row is at %d,%d, %dx%d", row[0], row[1], row[2], row[3]);
    if (row[2] <= 0) return g_failed;
    {
        /*
         * One drag of the list by half the screen's height, up and back down to where it began: forty rows do not fit, so
         * the list scrolls, rows leave at one end and are bound again at the other. What a picture of that costs, since
         * nothing is collected; and the list is where it was afterwards, pixel for pixel.
         */
        cost c0, c1;
        int k, x = row[0] + row[2] / 2, y0 = row[1] + row[3] * 10, travel = g_screen_h / 2, all[4], moved;
        profile_begin("scroll");
        hk_dvm_count_instructions(true);
        cost_now(&c0);
        megler_touch(0, 1, (float)x, (float)y0);
        for (k = 1; k <= 20; k++) {
            hk_sleep_ns(16000000ull);
            megler_touch(1, 1, (float)x, (float)(y0 - travel * k / 20));
        }
        for (k = 19; k >= 0; k--) {
            hk_sleep_ns(16000000ull);
            megler_touch(1, 1, (float)x, (float)(y0 - travel * k / 20));
        }
        /* (Held still before it is let go, so that it is not flung.) */
        hk_sleep_ns(150000000ull);
        megler_touch(1, 1, (float)x, (float)y0);
        hk_sleep_ns(150000000ull);
        megler_touch(2, 1, (float)x, (float)y0);
        settle();
        cost_now(&c1);
        hk_dvm_count_instructions(false);
        say_cost("a drag of the list by half the screen and back", &c0, &c1);
        profile_end("scroll");
        CHECK(c1.java.frames - c0.java.frames >= 4);
        all[0] = all[1] = 0;
        all[2] = W.width;
        all[3] = W.height;
        moved = count_changed(list, all);
        CHECK(moved == 0);
        if (moved) say("  (%d pixels are not where they were before the drag)", moved);
        CHECK(find_line("list: clicked row", 0) < 0);
    }

    /* A finger on the first row: the list runs down from there. The dialog first, which waits for its own button. */
    profile_begin("dialog");
    hk_dvm_count_instructions(true);
    hk_dvm_get_stats(&dialog_before);
    dialog_started = hk_time_monotonic_ns();
    tap(row[0] + row[2] / 2, row[1] + row[3] / 2);
    expect_line("I/DemoTest: list: clicked row 0 \"dialog\"", &at, 30);
    {
        /* How long the first dialog of an app takes to come up, from the finger to its report of itself (which the app makes 300 ms after showing it). */
        int showing = wait_line("I/DemoTest: dialog: showing=true", at + 1, 120);
        u64 took = hk_time_monotonic_ns() - dialog_started;
        hk_dvm_stats after;
        hk_dvm_get_stats(&after);
        if (!profiled("dialog")) hk_dvm_count_instructions(false);
        profile_end("dialog");
        say("  the first dialog was up %u ms after the finger (300 of them the app's own wait before it says so): %u thousand instructions, %u objects (%u KB)",
            (unsigned)(took / 1000000ull), (unsigned)((after.instructions - dialog_before.instructions) / 1000), (unsigned)(after.objects - dialog_before.objects),
            (unsigned)((after.object_bytes - dialog_before.object_bytes) >> 10));
        CHECK(showing >= 0);
        /* The budget in instructions holds on any machine; the one in time only says that nothing else has become as slow. */
        CHECK(after.instructions - dialog_before.instructions < DIALOG_BUDGET_INSTRUCTIONS);
        if (!g_profile_step) CHECK(took < DIALOG_BUDGET_NS);
    }
    answer_dialog(list, &at, "demos-2-dialog");
    for (i = 0; i < HK_COUNT(chain); i++) expect_line(chain[i], &at, 60);

    /* The second activity is on top now and waits for its button. A finger where the list's seventh row was is the second activity's, not the list's. */
    place = wait_line("I/DemoTest: second: shown back=", at, 30);
    CHECK(place >= 0 && rect_in(line_at(place), "back", back));
    settle();
    write_picture("demos-3-second");
    CHECK(pixel(10, row[1] + row[3] * 6 + row[3] / 2) == 0x204020);                 /* its own background, where the list was */
    tap(10, row[1] + row[3] * 6 + row[3] / 2);
    hk_sleep_ns(600000000ull);
    CHECK(find_line("list: clicked row 6", 0) < 0);
    if (back[2] > 0) tap(back[0] + back[2] / 2, back[1] + back[3] / 2);
    expect_line("I/DemoTest: second: result request=7 ok=true answer=42", &at, 30);
    expect_line("I/DemoTest: list: resumed", &at, 30);

    /*
     * The toast: a window of its own over the list for two seconds, which the app cannot ask the place of. So it is looked
     * for in the picture: while it is shown, what differs from the list alone is one patch, in the lower half of the screen
     * and across its middle, most of it not the list's any more; afterwards the list is back.
     */
    place = wait_line("I/DemoTest: toast: shown", at, 30);
    CHECK(place >= 0);
    expect_line("I/DemoTest: all done", &at, 30);
    settle();
    write_picture("demos-4-toast");
    with_toast = snapshot();
    CHECK(changed_box(list, toast));
    changed = count_changed(list, toast);
    say("  the toast is at %d,%d %dx%d; %d of its %d pixels are not the list's while it is shown", toast[0], toast[1], toast[2], toast[3], changed, toast[2] * toast[3]);
    CHECK(toast[2] >= 60 && toast[3] >= 20 && toast[1] > W.height / 2 && toast[0] < W.width / 2 && toast[0] + toast[2] > W.width / 2);
    CHECK(toast[2] > 0 && changed > toast[2] * toast[3] / 2);
    hk_sleep_ns(2600000000ull);
    settle();
    write_picture("demos-5-after");
    changed = count_changed(with_toast, toast);
    CHECK(toast[2] > 0 && changed > toast[2] * toast[3] / 2);
    {
        /* And what is left is the list as it was before anything was touched. */
        int all[4];
        all[0] = all[1] = 0;
        all[2] = W.width;
        all[3] = W.height;
        changed = count_changed(list, all);
        say("  after the toast %d pixels differ from the list's first picture", changed);
        CHECK(changed == 0);
    }
    free(with_toast);

    /*
     * The list still answers a finger, after a second activity has come and gone over it. (On the phone it did not: the
     * list's window had been taken away and put back, and the list took no touch for a row any more.) A row touched now
     * does its own thing alone: the dialog again, by its picture and its button; then the toast's row.
     */
    tap(row[0] + row[2] / 2, row[1] + row[3] / 2);
    expect_line("I/DemoTest: list: clicked row 0 \"dialog\"", &at, 30);
    answer_dialog(list, &at, "demos-6-dialog-again");
    expect_line("I/DemoTest: row 0 done", &at, 30);
    hk_sleep_ns(300000000ull);
    tap(row[0] + row[2] / 2, row[1] + row[3] * 5 + row[3] / 2);
    expect_line("I/DemoTest: list: clicked row 5 \"toast\"", &at, 30);
    expect_line("I/DemoTest: row 5 done", &at, 30);
    /* A touch that moves down the list and comes up elsewhere is a scroll that has nowhere to go, not a click. */
    megler_touch(0, 1, (float)(row[0] + 100), (float)(row[1] + row[3] * 2 + 10));
    for (i = 1; i <= 8; i++) {
        hk_sleep_ns(20000000ull);
        megler_touch(1, 1, (float)(row[0] + 100), (float)(row[1] + row[3] * 2 + 10 + 12 * (int)i));
    }
    megler_touch(2, 1, (float)(row[0] + 100), (float)(row[1] + row[3] * 4 + 10));
    hk_sleep_ns(500000000ull);
    CHECK(find_line("list: clicked row", at + 1) < 0);
    {
        /*
         * A row held down shows that it is pressed inside its own rectangle and nowhere else: the row's background is a
         * ripple whose mask is the row. (The framework drew it as a disc as wide as the screen, over the rows above and
         * below: its inflater dropped the mask.) Then the finger is lifted, which is a click on a row that does nothing.
         */
        int held[4], above[4], below[4], in, out_above, out_below;
        held[0] = above[0] = below[0] = row[0];
        held[2] = above[2] = below[2] = row[2];
        held[3] = above[3] = below[3] = row[3] - 2;
        held[1] = row[1] + row[3] * 6 + 1;
        above[1] = row[1] + row[3] * 4 + 1;
        below[1] = row[1] + row[3] * 8 + 1;
        settle();
        free(list);
        list = snapshot();
        megler_touch(0, 1, (float)(row[0] + 60), (float)(row[1] + row[3] * 6 + row[3] / 2));
        hk_sleep_ns(400000000ull);
        settle();
        write_picture("demos-7-row-held");
        in = count_changed(list, held);
        out_above = count_changed(list, above);
        out_below = count_changed(list, below);
        say("  a row held down: %d of its %d pixels show it; %d and %d pixels of the rows two above and two below changed", in, held[2] * held[3], out_above, out_below);
        CHECK(in > held[2] * held[3] / 2);
        CHECK(out_above == 0 && out_below == 0);
        megler_touch(2, 1, (float)(row[0] + 60), (float)(row[1] + row[3] * 6 + row[3] / 2));
        expect_line("I/DemoTest: list: clicked row 6 \"seven\"", &at, 30);
        settle();
    }
    free(list);
    type_into_fields(row, &at);
    keyboard_and_dialogs(row, &at);
    back_and_end("I/DemoTest: list: destroyed", &at);
    {
        /*
         * The frame meter (hk_java.c): a line every five seconds while the app draws, and at its end one for the whole run,
         * with the frames drawn and the share of the screen drawn again in each: less than all of it, since the windows draw
         * their dirty regions (the typing screen's cursor and fields, a row pressed).
         */
        int meter = find_line("java: frames in all, ", 0);
        const char *counts = strstr(line_at(meter), " s: "), *share = strstr(line_at(meter), "redrawn ");
        unsigned drawn = 0, percent = 100;
        CHECK(meter >= 0 && counts && sscanf(counts + 4, "%u drawn", &drawn) == 1 && drawn >= 10);
        CHECK(share && sscanf(share, "redrawn %u%%", &percent) == 1 && percent < 100);
        CHECK(find_line("java: frames ", 0) >= 0 && find_line("java: frames ", 0) < meter);
        if (meter >= 0) say("  %s", line_at(meter));
    }
    say_app_lines("/DemoTest: ");
    return g_failed;
}

/* ---------------------------------------------------------------- natives */

static int run_natives(void)
{
    static const char *const steps[] = { "load", "registered", "primitives", "stack-mix", "overloads", "arrays", "bounds", "strings", "callback", "throw-native",
                                         "throw-java", "find-class", "references", "reflection", "monitor", "thread", "thread-daemon", "needed-library", "asset",
                                         "bitmap", "direct-buffer", "configuration", "library-folder", "loaded-once", "own-file", "missing-library",
                                         "missing-method", "looper-watched", "gc-held", "system-icu" };
    int at = -1, layout, pic[4], i;
    char want[120];
    memset(pic, 0, sizeof(pic));
    expect_line("I/NativesTest: onCreate", &at, 120);
    /* The library's own line (__android_log_print), from its JNI_OnLoad: run when the class that loads it is first used. */
    expect_line("I/NativesLib: JNI_OnLoad call 1", &at, 60);
    for (i = 0; i < (int)HK_COUNT(steps); i++) {
        snprintf(want, sizeof(want), "I/NativesTest: %s ok", steps[i]);
        expect_line(want, &at, 60);
    }
    snprintf(want, sizeof(want), "I/NativesTest: done ok=%d failed=0", (int)HK_COUNT(steps));
    expect_line(want, &at, 60);
    /* The descriptor the library watches on the main thread's looper: its callback comes with a later turn of the message loop. */
    expect_line("I/NativesTest: looper ok", &at, 30);
    /* Loaded three times over by the app, loaded once. */
    CHECK(find_line("JNI_OnLoad call 2", 0) < 0);
    layout = wait_line("I/NativesTest: layout focus=true", 0, 30);
    CHECK(layout >= 0 && rect_in(line_at(layout), "picture", pic));
    settle();
    write_picture("natives-1");
    /* The bitmap the library painted through libjnigraphics, drawn pixel for pixel 16 in from its view's corner: green corner, red left, blue right, the dark field around. */
    CHECK(pixel(pic[0] + 16 + 4, pic[1] + 16 + 4) == 0x00FF00 && pixel(pic[0] + 16 + 20, pic[1] + 16 + 40) == 0xFF0000 && pixel(pic[0] + 16 + 40, pic[1] + 16 + 4) == 0x0000FF);
    CHECK(pixel(pic[0] + 16 + 15, pic[1] + 16 + 15) == 0x00FF00 && pixel(pic[0] + 16 + 16, pic[1] + 16 + 16) == 0xFF0000 && pixel(pic[0] + 16 + 62, pic[1] + 16 + 30) == 0x0000FF);
    CHECK(pixel(pic[0] + 8, pic[1] + 8) == 0x202020 && pixel(pic[0] + 16 + 70, pic[1] + 40) == 0x202020);
    for (i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], "/NativesTest: ") && strstr(g_lines[i], " FAILED")) say("  %s", g_lines[i]);
    back_and_end("I/NativesTest: onDestroy", &at);
    say_app_lines("/NativesTest: ");
    return g_failed;
}

/* What the command line says after the screen's size: for `app` the steps of its script, for `data` the run it is to be. */
static int g_script_count;
static char **g_script;

void hk_tests_java_set_script(int count, char **steps)
{
    g_script_count = count;
    g_script = steps;
}

/* ------------------------------------------------------------------- data */

/*
 * `data`: a database through the framework's android.database.sqlite, on the SQLite compiled into the core. The app
 * says how each step went. It is run twice by the script, the second time on the data folder the first left: the
 * script names the run it expects ("run:2") and the app says which run its own database makes it.
 */
/* A battery nobody would take for the default: 37 of 100 and going down, off the charger, 3.712 V, 30.1 degrees. */
static const hk_battery k_battery_run2 = { 37, 100, 3, 0, 2, 3712, 301, true, "Li-poly" };

static int count_lines(const char *text)
{
    int i, n = 0;
    hk_lock_acquire(&g_log_lock);
    for (i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], text)) n++;
    hk_lock_release(&g_log_lock);
    return n;
}

/*
 * The battery changing under the app, as the shell reports it (megler_set_battery). The app has a receiver registered for the
 * battery's broadcasts, which has heard the battery once already, when it was registered. It must hear each change once, on the
 * main thread: not the same battery reported again, not a voltage or a temperature that changed alone; and that the charger was
 * connected or taken away when it was, before the battery's own news, which is Android's order. `on_battery`: the app was
 * started on its battery (the second run), so that the first change here is also a charger connected.
 */
static void battery_changes(bool on_battery)
{
    static const megler_battery charging = { 80, 2, 2, 0, INT_MIN, true }, discharging = { 79, 3, 0, 3900, 312, true }, warmer = { 79, 3, 0, 3890, 320, true },
                              lower = { 78, 3, 0, 3890, 320, true };
    int heard, power;
    CHECK(wait_line("I/DataTest: battery-heard 1 ", 0, 30) >= 0);
    /* On a USB charger at 80: the voltage is not said and is made of the level, the temperature is not said and is 25.0. */
    megler_set_battery(&charging);
    heard = wait_line("I/DataTest: battery-heard 2 level=80/100 status=2 plugged=2 health=2 voltage=4040 temperature=250 present=true technology=Li-ion", 0, 30);
    power = find_line("I/DataTest: power-heard android.intent.action.ACTION_POWER_CONNECTED", 0);
    CHECK(heard >= 0 && (on_battery ? power >= 0 && power < heard : power < 0));
    /* The same again is no news; off the charger and one lower is, and so is the charger's going. */
    megler_set_battery(&charging);
    megler_set_battery(&discharging);
    heard = wait_line("I/DataTest: battery-heard 3 level=79/100 status=3 plugged=0 health=2 voltage=3900 temperature=312 present=true technology=Li-ion", 0, 30);
    power = find_line("I/DataTest: power-heard android.intent.action.ACTION_POWER_DISCONNECTED", 0);
    CHECK(heard >= 0 && power >= 0 && power < heard);
    /* Warmer and a little less voltage at the same level is not broadcast; the next change carries both. */
    megler_set_battery(&warmer);
    megler_set_battery(&lower);
    CHECK(wait_line("I/DataTest: battery-heard 4 level=78/100 status=3 plugged=0 health=2 voltage=3890 temperature=320 present=true technology=Li-ion", 0, 30) >= 0);
    /* Four in all, the first being the one every receiver gets when it registers, and nothing came anywhere but on the main thread. */
    CHECK(count_lines("I/DataTest: battery-heard ") == 4 && count_lines("I/DataTest: power-heard ") == (on_battery ? 2 : 1) && count_lines("OFF THE MAIN THREAD") == 0);
}

static int run_data(void)
{
    static const char *const steps[] = { "open", "kept", "insert", "query", "types", "update", "delete", "rollback", "commit", "constraint", "statement", "text",
                                         "bad-sql", "memory", "upgrade", "left",
                                         /* And the storage as an app sees it. */
                                         "environment", "own-folders", "volumes", "space", "permissions", "download", "list-root",
                                         /* And the battery, the time zone, TLS that fails politely, Android's key store's key pairs, and
                                            the list of decoders, which is empty and says nothing about it. */
                                         "battery", "timezone", "tls", "keystore", "codecs",
                                         /* And XML through android.util.Xml: a serializer and a pull parser, kXML's. And a stored file of
                                            the APK as a descriptor whose number is taken from its ParcelFileDescriptor. And the app's task as
                                            ActivityManager describes it. And registerReceiver's other forms through a ContextWrapper. And a
                                            ViewGroup made without its constructor calling an overridable setWillNotDraw. And an EditText's
                                            cursor and selection handles. And ParcelFileDescriptor's dup, detachFd and adoptFd. And a theme
                                            attribute that names itself. */
                                         "xml", "asset-fd", "tasks", "wrapper-receiver", "viewgroup-flag", "text-drawables", "pfd-dup",
                                         "theme-selfref",
                                         /* And the network from Java, on loopback: names, an HTTP server of the app's own and HttpURLConnection, UDP, a server
                                            socket closed under the thread waiting in accept, a java.nio selector, the interfaces, ConnectivityManager. */
                                         "net-localhost", "net-http", "net-udp", "net-close", "net-nio", "net-interfaces", "net-connectivity" };
    int at = -1, want = 0, run = 0, line, i;
    char text[120];
    if (g_script_count) sscanf(g_script[0], "run:%d", &want);
    expect_line("I/DataTest: onCreate", &at, 120);
    line = wait_line("I/DataTest: run ", at, 60);
    CHECK(line >= 0 && sscanf(strstr(line_at(line), ": run ") ? strstr(line_at(line), ": run ") : "", ": run %d", &run) == 1 && run >= 1 && (!want || run == want));
    say("  this is run %d of the app on its database", run);
    for (i = 0; i < (int)HK_COUNT(steps); i++) {
        snprintf(text, sizeof(text), "I/DataTest: %s ok", steps[i]);
        expect_line(text, &at, 60);
    }
    snprintf(text, sizeof(text), "I/DataTest: done ok=%d failed=0", (int)HK_COUNT(steps));
    expect_line(text, &at, 60);
    /* What requestPermissions asked comes back by a message of its own, after the method that asked has returned. */
    expect_line("I/DataTest: permission-result ok after done=true", &at, 30);
    /* So does the battery's sticky broadcast to a receiver registered for it, once. */
    expect_line("I/DataTest: battery-received ok", &at, 30);
    /* The window's insets, when its views are first laid out, say the frame they are relative to: the screen, which the window fills. */
    {
        int frame = wait_line("I/DataTest: insets-frame ", 0, 30), fw = 0, fh = 0;
        CHECK(frame >= 0 && sscanf(strstr(line_at(frame), "insets-frame ") + 13, "%dx%d", &fw, &fh) == 2 && fw > 0 && fh > 0);
        if (frame >= 0) say("  %s", strstr(line_at(frame), "insets-frame "));
    }
    /*
     * The time zone the app has is the one the shell named before the start (megler_set_timezone, below): in the first run a zone with
     * summer time, whose winter's noon by Greenwich is 13:00 and whose summer's is 14:00; in the second a name no tzdata has, which is
     * GMT and a line in the log, and breaks nothing.
     */
    CHECK(find_line(run == 2 && want == 2 ? "I/DataTest: timezone is GMT winter=2023-01-15 12:00 +0000 summer=2023-07-15 12:00 +0000"
                                           : "I/DataTest: timezone is Europe/Berlin winter=2023-01-15 13:00 +0100 summer=2023-07-15 14:00 +0200", 0) >= 0);
    if (run == 2 && want == 2) CHECK(find_line("java: the phone's time zone is given as \"Nowhere/At_All\", which the Android root's tzdata does not have", 0) >= 0);
    /* A popup list that wraps its content has room for all its rows, inside what the list and the popup's background keep clear. */
    expect_line("I/DataTest: popup ok rows=3", &at, 30);
    /* And the battery an app reads is the core's (hk_battery_read): a full one on mains when nothing was set, in the second run the one set before it. */
    CHECK(find_line(run == 2 && want == 2 ? "I/DataTest: battery is level=37/100 status=3 plugged=0 health=2 voltage=3712 temperature=301 present=true technology=Li-poly capacity=37 charging=false"
                                           : "I/DataTest: battery is level=100/100 status=5 plugged=1 health=2 voltage=4200 temperature=250 present=true technology=Li-ion capacity=100 charging=true",
                    0) >= 0);
    battery_changes(run == 2 && want == 2);
    /* The core's own word that it is its SQLite, on its files. */
    CHECK(find_line("sqlite: SQLite 3.", 0) >= 0);
    /* An empty list of decoders is the answer, not a gap: the codecs step asked, and nothing said the class was missing. */
    CHECK(find_line("megler/MediaCodecNative is not there yet", 0) < 0);
    for (i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], "/DataTest: ") && strstr(g_lines[i], " FAILED")) say("  %s", g_lines[i]);
    settle();
    write_picture(run == 1 ? "data-1" : "data-2");
    back_and_end("I/DataTest: onDestroy", &at);
    say_app_lines("/DataTest: ");
    return g_failed;
}

/* --------------------------------------------------------------------- gc */

/*
 * `gc`: the collector as an app meets it (core/tests/app/gc). References cleared and enqueued, finalizers run once and an
 * object they bring back kept, far more made than the heap holds with what is kept checked, four threads allocating at once,
 * bitmaps freed through their finalizers. The app runs with the collector's poison on (hk_tests_java_run): an object freed
 * while still in use would read as 0xDB.
 */
static int run_gc(void)
{
    static const char *const steps[] = { "weak", "soft", "phantom", "finalize", "resurrect", "churn", "threads", "bitmaps" };
    int at = -1, i, tries;
    char text[120];
    u64 collections = 0, pause_ns = 0, live;
    uint64_t bitmap_bytes = 0;
    uint32_t bitmaps = 0;
    expect_line("I/GcTest: onCreate", &at, 120);
    for (i = 0; i < (int)HK_COUNT(steps); i++) {
        snprintf(text, sizeof(text), "I/GcTest: %s ok", steps[i]);
        expect_line(text, &at, 600);
    }
    snprintf(text, sizeof(text), "I/GcTest: done ok=%d failed=0", (int)HK_COUNT(steps));
    expect_line(text, &at, 120);
    for (i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], "/GcTest: ") && strstr(g_lines[i], " FAILED")) say("  %s", g_lines[i]);
    /* The app finishes itself. */
    for (tries = 0; tries < 3000 && running(); tries++) hk_sleep_ns(10000000ull);
    CHECK(g_exited && g_status == 0);
    /* The collector ran, the heap is a fraction of what was made, and the dropped bitmaps' pixels went with their finalizers. */
    hk_dvm_gc_totals(&collections, &pause_ns);
    live = hk_dvm_live_bytes();
    hk_java_gfx_stats(&bitmap_bytes, &bitmaps);
    say("  %u collections, %.0f ms in all; %u MB live at the end; %u bitmaps (%u KB) left of the 40 dropped and the app's own", (unsigned)collections,
        (double)pause_ns / 1e6, (unsigned)(live >> 20), (unsigned)bitmaps, (unsigned)(bitmap_bytes >> 10));
    CHECK(collections >= 5);
    CHECK(live < (64u << 20));
    CHECK(bitmaps < 20);
    say_app_lines("/GcTest: ");
    return g_failed;
}

/* ------------------------------------------------------------------ crash */

/*
 * `crash`: an activity whose onCreate throws and nothing catches it. The app ends, as it would on Android; what is
 * checked is that the shell is told why in one sentence, the same one in on_exit's reason and in megler_last_error.
 */
static int run_crash(void)
{
    int at = -1, tries;
    expect_line("I/CrashTest: first onCreate", &at, 120);
    /* The app is written for Android 6: a row of no fixed width leaves its weighted, widthless children what they asked for, the rule Android keeps for apps that old. */
    expect_line("I/CrashTest: row unlimited=140:40+100 exact=200:100+100", &at, 30);
    expect_line("I/CrashTest: second onCreate, about to throw", &at, 60);
    for (tries = 0; tries < 3000 && running(); tries++) hk_sleep_ns(10000000ull);
    CHECK(g_exited && g_status == -1);
    say("  the app's end, as the shell is told it: %s", g_reason);
    CHECK(strstr(g_reason, "java.lang.IllegalStateException: thrown in onCreate") && strstr(g_reason, "(in megler.test.crash.Second.onCreate)"));
    /* Of its causes the innermost is named, cut short, and the one in between is not: the sentence fits a dialog whatever the app threw. */
    CHECK(strstr(g_reason, "thrown in onCreate, caused by java.io.IOException: the innermost cause, which says what went wrong, at length: xxx") && strstr(g_reason, "xxx... (in megler.test"));
    CHECK(!strstr(g_reason, "a cause in between") && !strchr(g_reason, '\n') && strlen(g_reason) < 440);
    CHECK(!strcmp(megler_last_error(), g_reason));
    /* The framework's own word for it, as Android's log has it, and the stack. */
    CHECK(find_line("E/Looper: uncaught exception on main", 0) >= 0 && find_line("\tat megler.test.crash.Second.onCreate(Second.java:", 0) >= 0);
    CHECK(find_line("java: the main thread ended with java.lang.IllegalStateException: thrown in onCreate", 0) >= 0);
    return g_failed;
}

/* --------------------------------------------------------------------- gl */

/*
 * `gl`: OpenGL ES from Java (core/tests/app/gl), with the stand-in that draws (hk_gl_standin.c) where ANGLE would be. A
 * GLSurfaceView's renderer on the layer's GL thread -- its calls in Android's order, sixty frames a second at most, none
 * when not asked for in RENDERMODE_WHEN_DIRTY and one when asked, an event queued to it, a fixed size, a pause and a
 * resume that bring a new context; EGL14 on a pbuffer; EGL10 and GL10 on a SurfaceView of the app's own thread. And the
 * picture: the frames under the views, where the views left holes, the label over the GL view.
 */
static int run_gl(void)
{
    int at = -1, line, gl[4], label[4], egl[4], tries;
    long ms = -1;
    char text[200];
    hk_java_gl_stats st0, st1;
    memset(gl, 0, sizeof(gl));
    memset(label, 0, sizeof(label));
    memset(egl, 0, sizeof(egl));
    expect_line("I/GlTest: onCreate", &at, 120);
    /* The version the device says it offers is what a context can be made of (the stand-in has OpenGL ES 3). */
    expect_line("I/GlTest: device gles=3.0 req=0x30000", &at, 30);
    /* EGL14 on a pbuffer, on a thread of the app's: how an app asks the GPU's name. */
    CHECK(wait_line("I/GlTest: egl14 pbuffer made=true egl=1.4 pixel=255,0,128 width=16 version=2 current=true released=true max-texture=4096 error=0", 0, 120) >= 0);
    CHECK(find_line("I/GlTest: egl14 strings vendor=Megler renderer=stand-in for OpenGL ES version=OpenGL ES 2.0 (stand-in) extensions=3", 0) >= 0);
    /* The renderer, on the GL thread with its context current: created, changed, drawn, in that order. */
    expect_line("I/GlTest: created 1 thread=GLThread ", &at, 120);
    CHECK(strstr(line_at(at), "current=true version=2 config=8/16") != NULL);
    expect_line("I/GlTest: strings vendor=Megler renderer=stand-in for OpenGL ES version=OpenGL ES 2.0 (stand-in) extensions=3 gl10=Megler", &at, 30);
    expect_line("I/GlTest: program linked=1 position=0 color=0 log=stand-in: program", &at, 30);
    expect_line("I/GlTest: changed ", &at, 30);
    expect_line("I/GlTest: first frame on GLThread", &at, 30);
    CHECK(strstr(line_at(at), "error=0") != NULL);
    line = wait_line("I/GlTest: layout ", 0, 60);
    CHECK(line >= 0 && rect_in(line_at(line), "gl", gl) && rect_in(line_at(line), "label", label) && rect_in(line_at(line), "egl", egl));
    snprintf(text, sizeof(text), "I/GlTest: changed %dx%d on GLThread", gl[2], gl[3]);
    CHECK(find_line(text, 0) >= 0);
    /* RENDERMODE_CONTINUOUSLY, paced as a swap interval of 1 paces it: thirty frames take half a second at least. */
    line = wait_line("I/GlTest: continuous 30 frames in ", 0, 120);
    CHECK(line >= 0 && sscanf(strstr(line_at(line), " in ") + 4, "%ld", &ms) == 1 && ms >= 480);
    say("  thirty frames drawn continuously took %ld ms", ms);
    /* RENDERMODE_WHEN_DIRTY: nothing drawn while nothing asks, one frame for one requestRender. An event on the GL thread. */
    expect_line("I/GlTest: when-dirty idle frames=0", &at, 60);
    expect_line("I/GlTest: request-render frames=1", &at, 60);
    expect_line("I/GlTest: event on GLThread ", &at, 60);
    CHECK(strstr(line_at(at), "current=true vendor=Megler") != NULL);
    /* EGL10 and GL10 from the app's own thread on the bottom SurfaceView: a 565 configuration found by exact match, a version-1 context. */
    line = wait_line("I/GlTest: egl10 configs=6 exact565=true made=true surface=", 0, 60);
    snprintf(text, sizeof(text), "surface=%dx%d egl=1.4", egl[2], egl[3]);
    CHECK(line >= 0 && strstr(line_at(line), text));
    CHECK(wait_line("I/GlTest: egl10 swapped 5 ok=true error=12288", 0, 60) >= 0);
    CHECK(find_line("java: this OpenGL ES has no version 1", 0) >= 0);
    settle();
    write_picture("gl-1");
    /*
     * Who composed the window: the shell, from the surfaces' frames by their shared handles and the views as `present` showed
     * them, nothing read back -- or, when it said no ("readback"), the core, from frames read back and the views' memory.
     */
    say("  the window composed by %s: %d frames handed to the shell, %d places told, %d pictures composed there", GL.refuse ? "the core" : "the shell",
        (int)GL.frames, (int)GL.places, (int)GL.composed);
    if (GL.refuse) CHECK(find_line("java: the shell did not take an OpenGL ES frame", 0) >= 0 && GL.composed == 0);
    else CHECK(find_line("java: the app draws with EGL on a view: from now on its window is composed by the shell", 0) >= 0 && GL.frames > 0 && GL.places > 0 && GL.composed > 0);
    /* The views over the frames: the GL view's blue, the triangle's yellow at its centroid, the label over it, the EGL10 view's green. */
    CHECK(pixel(gl[0] + 4, gl[1] + 4) == 0x0080FF);
    CHECK(pixel(gl[0] + gl[2] / 2, gl[1] + gl[3] * 7 / 12) == 0xFFFF00);
    CHECK(pixel(gl[0] + gl[2] / 2, gl[1] + gl[3] / 8) == 0x0080FF);
    CHECK(pixel(label[0] + 2, label[1] + 2) == 0xFF00FF);
    CHECK(pixel(egl[0] + egl[2] / 2, egl[1] + egl[3] / 2) == 0x00FF00);
    /* A fixed size half the view's: a new surface, onSurfaceChanged with that size, the frame scaled into the view. */
    expect_line("I/GlTest: fixed size asked ", &at, 60);
    snprintf(text, sizeof(text), "I/GlTest: changed %dx%d on GLThread", gl[2] / 2, gl[3] / 2);
    expect_line(text, &at, 60);
    expect_line("I/GlTest: ready frames=", &at, 60);
    settle();
    write_picture("gl-2");
    CHECK(pixel(gl[0] + 4, gl[1] + 4) == 0x0080FF);
    CHECK(pixel(gl[0] + gl[2] / 2, gl[1] + gl[3] * 7 / 12) == 0xFFFF00);
    CHECK(pixel(label[0] + 2, label[1] + 2) == 0xFF00FF);
    /* Paused: no frames. Resumed: the context made again (it was not to be kept), onSurfaceCreated and onSurfaceChanged again, a frame. */
    megler_set_paused(true);
    expect_line("I/GlTest: onPause", &at, 30);
    hk_sleep_ns(300000000ull);
    hk_java_gl_get_stats(&st0);
    hk_sleep_ns(500000000ull);
    hk_java_gl_get_stats(&st1);
    CHECK(st1.swaps == st0.swaps);
    megler_set_paused(false);
    expect_line("I/GlTest: onResume", &at, 30);
    expect_line("I/GlTest: created 2 thread=GLThread ", &at, 60);
    expect_line("I/GlTest: changed ", &at, 30);
    for (tries = 0; tries < 1000 && st1.swaps == st0.swaps && running(); tries++) {
        hk_sleep_ns(10000000ull);
        hk_java_gl_get_stats(&st1);
    }
    CHECK(st1.swaps > st0.swaps);
    say("  %u swaps of window surfaces, %u of them shown, on %u GL threads; %d pictures in all, %d of them as a part", (unsigned)st1.swaps, (unsigned)st1.shown,
        (unsigned)st1.threads, (int)W.pictures, (int)W.parts);
    /* Composed here, a new GL frame under unchanged views is handed over as the part it changed; the test's shell hands whole pictures. */
    if (GL.refuse) CHECK(W.parts > 0);
    line = find_line("java: OpenGL frames ", 0);
    if (line >= 0) say("  %s", line_at(line));
    back_and_end("I/GlTest: onDestroy", &at);
    CHECK(wait_line("java: GLThread 1 ended", 0, 30) >= 0);
    CHECK(wait_line("I/GlTest: egl10 ended", 0, 30) >= 0);
    CHECK(find_line("FAILED", 0) < 0 && find_line("FATAL EXCEPTION", 0) < 0);
    say_app_lines("/GlTest: ");
    return g_failed;
}

/* -------------------------------------------------------------- any app */

/*
 * The script: "tap:X,Y", "drag:X,Y,X2,Y2", "wait:MILLISECONDS", "back", "key:CODE", in the order given. After each the app is waited for
 * until it is still (a minute at most) and its picture written as app-N. And the keyboard's part, as a phone's shell plays it:
 * "type:TEXT" (megler_text_commit), "textkey:N" (megler_text_key, MEGLER_TEXT_*), "kbd:PIXELS" (megler_keyboard_height).
 */
static int run_app(void)
{
    char name[40];
    int i, n = 1, x, y, x2, y2, k, ms, code, tries;
    /* HK_JAVA_PROFILE=after:N: the profile from the end of the script's Nth step (0: from the start of the script) to its end. */
    int after = g_profile_step && !strncmp(g_profile_step, "after:", 6) ? atoi(g_profile_step + 6) : -1;
    i32 pictures = W.pictures;
    /* The first picture can be long in coming: a real app's start under qemu is minutes of class linking. */
    for (tries = 0; tries < 60000 && W.pictures == pictures && running(); tries++) hk_sleep_ns(10000000ull);
    settle();
    say("  started: %d pictures so far, the app %s", (int)W.pictures, running() ? "runs" : "has ended");
    write_picture("app-1");
    for (i = 0; i < g_script_count && running(); i++) {
        const char *s = g_script[i];
        if (i == after) hk_dvm_profile(true);
        if (sscanf(s, "tap:%d,%d", &x, &y) == 2) {
            tap(x, y);
            hk_sleep_ns(300000000ull);
        } else if (sscanf(s, "drag:%d,%d,%d,%d", &x, &y, &x2, &y2) == 4) {
            /* A finger put down, moved in twelve steps and held a moment before it is lifted, so that what it dragged is not flung on. */
            megler_touch(0, 1, (float)x, (float)y);
            for (k = 1; k <= 12; k++) {
                hk_sleep_ns(30000000ull);
                megler_touch(1, 1, (float)(x + (x2 - x) * k / 12), (float)(y + (y2 - y) * k / 12));
            }
            hk_sleep_ns(400000000ull);
            megler_touch(2, 1, (float)x2, (float)y2);
            hk_sleep_ns(300000000ull);
        } else if (sscanf(s, "wait:%d", &ms) == 1) {
            hk_sleep_ns((u64)ms * 1000000ull);
        } else if (!strcmp(s, "back")) {
            megler_key(4, true);
            megler_key(4, false);
            hk_sleep_ns(300000000ull);
        } else if (sscanf(s, "key:%d", &code) == 1) {
            megler_key(code, true);
            megler_key(code, false);
            hk_sleep_ns(300000000ull);
        } else if (!strncmp(s, "type:", 5)) {
            megler_text_commit(s + 5);
            hk_sleep_ns(300000000ull);
        } else if (sscanf(s, "textkey:%d", &code) == 1) {
            megler_text_key(code);
            hk_sleep_ns(300000000ull);
        } else if (sscanf(s, "kbd:%d", &code) == 1) {
            megler_keyboard_height(code);
            hk_sleep_ns(300000000ull);
        } else {
            say("  (the script's \"%s\" is not understood)", s);
            continue;
        }
        settle();
        snprintf(name, sizeof(name), "app-%d", ++n);
        say("  after %s: %d pictures, the app %s", s, (int)W.pictures, running() ? "runs" : "has ended");
        write_picture(name);
    }
    if (!running()) say("  the app ended with status %d%s%s", g_status, g_reason[0] ? ": " : "", g_reason);
    /* With HK_JAVA_PROFILE=start: what ran from the app's start to the end of the script. */
    profile_end("start");
    if (after >= 0 && after < g_script_count) {
        hk_dvm_profile(false);
        say("  the profile of the script from its step %d:", after + 1);
        hk_dvm_profile_report(getenv("HK_JAVA_PROFILE_TOP") && atoi(getenv("HK_JAVA_PROFILE_TOP")) > 0 ? atoi(getenv("HK_JAVA_PROFILE_TOP")) : 20, profile_line, NULL);
    }
    return g_failed;
}

int hk_tests_java_run(const char *which, const char *apk, const char *android_root, const char *picture_dir, int verbose)
{
    g_verbose = verbose;
    g_profile_step = getenv("HK_JAVA_PROFILE");
    /* HK_JAVA_TRACE=1: the runtime's trace, which names every class it links and every exception thrown, caught or not. */
    if (getenv("HK_JAVA_TRACE") && atoi(getenv("HK_JAVA_TRACE")) > 0) hk_dvm_set_trace(atoi(getenv("HK_JAVA_TRACE")));
    profile_begin("start");
    W.dir = picture_dir;
    if (picture_dir) hk_path_mkdirs(picture_dir);
    g_portrait = !strcmp(which, "demos");
    g_any_app = !strcmp(which, "app");
    g_crash_app = !strcmp(which, "crash");
    /* The shell does not compose the window ("readback"): the core reads the GL frames back and composes it, as before. */
    GL.refuse = g_script_count && !strcmp(g_script[0], "readback");
    /* The data app's second run is on another battery than the one the core has when nobody has said: what the app then reads is what was set here. */
    if (!strcmp(which, "data") && g_script_count && !strcmp(g_script[0], "run:2")) hk_battery_set(&k_battery_run2);
    /* And in a time zone the shell names, as it must, before the app starts: one with summer time, and in the second run a name that is no zone's. */
    if (!strcmp(which, "data")) megler_set_timezone(g_script_count && !strcmp(g_script[0], "run:2") ? "Nowhere/At_All" : "Europe/Berlin");
    /* For an app that is not a test's, HK_JAVA_TIMEZONE names the zone it is started in: a real app as on a phone that has one. */
    if (!strcmp(which, "app") && getenv("HK_JAVA_TIMEZONE")) megler_set_timezone(getenv("HK_JAVA_TIMEZONE"));
    /* The test apps' frames drawn in part are each checked against the whole screen drawn again (megler.ViewRoot.checkPart). */
    hk_java_check_partial(!g_any_app);
    /*
     * And the collector runs often under the test apps (every 2 MB made) with its poison on: an object freed while something
     * still used it would read as 0xDB. An app that is not a test's runs as on the phone.
     */
    if (!g_any_app) {
        hk_dvm_gc_set_poison(true);
        hk_dvm_gc_set_min_budget(2u << 20);
    }
    /*
     * OpenGL ES is the stand-in that draws, as for any app here: there is no ANGLE under qemu or on this PC. A real app that
     * asks the GPU's name is told "stand-in for OpenGL ES".
     */
    hk_egl_set_provider(hk_gl_standin);
    hk_gl_standin_reset(true, false);
    if (start(apk, android_root)) {
        if (!strcmp(which, "demos")) run_demos();
        else if (!strcmp(which, "natives")) run_natives();
        else if (!strcmp(which, "crash")) run_crash();
        else if (!strcmp(which, "data")) run_data();
        else if (!strcmp(which, "gc")) run_gc();
        else if (!strcmp(which, "gl")) run_gl();
        else if (g_any_app) run_app();
        else run_hello();
        if (running()) {
            if (g_direct) hk_driver_java.stop();
            else megler_stop();
            hk_sleep_ns(500000000ull);
        }
    }
    /*
     * Pictures drawn in part: none may differ from the screen drawn whole (the window's dirty region missed nothing), and none
     * may have changed outside the part it said (the clip held).
     */
    say("  %d pictures, %d of them presented as a part, %d shown from the window's canvas without a flush; %ld pixels changed outside the part said",
        (int)W.pictures, (int)W.parts, (int)W.from_canvas, W.stale);
    CHECK(W.stale == 0);
    /* An app that shows pictures and composes no OpenGL frames has them shown from its canvas (megler_gfx_host.present). */
    if (W.pictures > 0 && count_lines("java: the app draws with EGL on a view") == 0) CHECK(W.from_canvas > 0);
    if (!g_any_app) CHECK(count_lines("a frame drawn in part missed") == 0);
    say("%d checks, %d failed", g_checks, g_failed);
    return g_failed;
}
