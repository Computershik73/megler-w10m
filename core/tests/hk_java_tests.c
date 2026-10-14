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
#include "../src/dvm/hk_dvm.h"
#include "../src/hk_base.h"
#include "../src/host/hk_host.h"
#include "../src/run/hk_run.h"
#include "hk_gfx_standin.h"
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
} W = { HK_LOCK_INIT, NULL, 0, 0, 0, 0, NULL };

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
static void settle(void)
{
    hk_java_stats st;
    i32 pictures = W.pictures, polls;
    u64 since = hk_time_monotonic_ns();
    int tries;
    hk_java_get_stats(&st);
    polls = st.polls;
    for (tries = 0; tries < 6000 && running(); tries++) {
        hk_sleep_ns(10000000ull);
        hk_java_get_stats(&st);
        if (W.pictures != pictures) {
            pictures = W.pictures;
            polls = st.polls;
            since = hk_time_monotonic_ns();
        } else if (st.polls - polls >= 20 && hk_time_monotonic_ns() - since >= 200000000ull) {
            break;
        }
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
    cfg.gfx = *hk_gfx_standin();
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
    /* The PNG from the resources, 16 pixels stretched to 64 dp: orange left, green right, a transparent border through which the layout's grey shows. */
    CHECK(pixel(mark[0] + mark[2] / 4, mark[1] + mark[3] / 2) == 0xFF8000 && pixel(mark[0] + 3 * mark[2] / 4, mark[1] + mark[3] / 2) == 0x00A040);
    CHECK(pixel(mark[0] + 1, mark[1] + 1) == 0xE0E0E0 && pixel(mark[0] + mark[2] + 100, mark[1] + mark[3] / 2) == 0xE0E0E0);
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
    /* Text, as the keyboard commits it. */
    megler_text_commit("Hello");
    expect_line("I/DemoTest: typing: name=\"Hello\" cursor=5", at, 30);
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
    back_and_end("I/DemoTest: list: destroyed", &at);
    say_app_lines("/DemoTest: ");
    return g_failed;
}

/* ---------------------------------------------------------------- natives */

static int run_natives(void)
{
    static const char *const steps[] = { "load", "registered", "primitives", "stack-mix", "overloads", "arrays", "bounds", "strings", "callback", "throw-native",
                                         "throw-java", "find-class", "references", "reflection", "monitor", "thread", "thread-daemon", "needed-library", "asset",
                                         "bitmap", "direct-buffer", "configuration", "library-folder", "loaded-once", "own-file", "missing-library",
                                         "missing-method", "looper-watched" };
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
static int run_data(void)
{
    static const char *const steps[] = { "open", "kept", "insert", "query", "types", "update", "delete", "rollback", "commit", "constraint", "statement", "text",
                                         "bad-sql", "memory", "upgrade", "left",
                                         /* And the storage as an app sees it. */
                                         "environment", "own-folders", "volumes", "space", "permissions", "download", "list-root" };
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
    /* The core's own word that it is its SQLite, on its files. */
    CHECK(find_line("sqlite: SQLite 3.", 0) >= 0);
    for (i = 0; i < g_nlines; i++)
        if (strstr(g_lines[i], "/DataTest: ") && strstr(g_lines[i], " FAILED")) say("  %s", g_lines[i]);
    settle();
    write_picture(run == 1 ? "data-1" : "data-2");
    back_and_end("I/DataTest: onDestroy", &at);
    say_app_lines("/DataTest: ");
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

/* -------------------------------------------------------------- any app */

/*
 * The script: "tap:X,Y", "drag:X,Y,X2,Y2", "wait:MILLISECONDS", "back", "key:CODE", in the order given. After each the app is waited for
 * until it is still (a minute at most) and its picture written as app-N.
 */
static int run_app(void)
{
    char name[40];
    int i, n = 1, x, y, x2, y2, k, ms, code, tries;
    i32 pictures = W.pictures;
    /* The first picture can be long in coming: a real app's start under qemu is minutes of class linking. */
    for (tries = 0; tries < 60000 && W.pictures == pictures && running(); tries++) hk_sleep_ns(10000000ull);
    settle();
    say("  started: %d pictures so far, the app %s", (int)W.pictures, running() ? "runs" : "has ended");
    write_picture("app-1");
    for (i = 0; i < g_script_count && running(); i++) {
        const char *s = g_script[i];
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
    if (start(apk, android_root)) {
        if (!strcmp(which, "demos")) run_demos();
        else if (!strcmp(which, "natives")) run_natives();
        else if (!strcmp(which, "crash")) run_crash();
        else if (!strcmp(which, "data")) run_data();
        else if (g_any_app) run_app();
        else run_hello();
        if (running()) {
            if (g_direct) hk_driver_java.stop();
            else megler_stop();
            hk_sleep_ns(500000000ull);
        }
    }
    say("%d checks, %d failed", g_checks, g_failed);
    return g_failed;
}
