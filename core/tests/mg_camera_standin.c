/* The stand-in for the shell's cameras: mg_camera_standin.h says what it does. */
#include "mg_camera_standin.h"

#include <stdlib.h>
#include <string.h>

#include "../src/mg_base.h"
#include "../src/host/mg_host.h"

typedef struct camera {
    int index;
    mg_lock lock;                      /* the focus below, and calls of `done` */
    mg_thread *thread;
    volatile i32 quit;
    int width, height, fps;
    megler_camera_frame_fn frame;
    megler_camera_focus_fn done;
    void *core;
    int focus_mode;                    /* what is under way: MEGLER_CAMERA_FOCUS_ONCE or _MACRO until due, _CONTINUOUS until said */
    u64 focus_due;
    bool streaming;
} camera;

static mg_lock g_lock = MG_LOCK_INIT;
static mg_camera_standin_counts g_counts;
static bool g_refuse;
static bool g_open[2];
static i32 g_number;                  /* pictures made since the start, the number in each */

void mg_camera_standin_refuse(bool refuse)
{
    mg_lock_acquire(&g_lock);
    g_refuse = refuse;
    mg_lock_release(&g_lock);
}

void mg_camera_standin_counts_get(mg_camera_standin_counts *out)
{
    mg_lock_acquire(&g_lock);
    *out = g_counts;
    mg_lock_release(&g_lock);
}

static int count(void *user)
{
    (void)user;
    return 2;
}

static bool info(void *user, int index, megler_camera_info *out)
{
    (void)user;
    if (index < 0 || index > 1) return false;
    memset(out, 0, sizeof(*out));
    out->facing = index == 0 ? MEGLER_CAMERA_BACK : MEGLER_CAMERA_FRONT;
    out->orientation = index == 0 ? 90 : 270;
    out->focus = index == 0;
    out->light = index == 0;
    mg_strlcpy(out->name, index == 0 ? "stand-in back camera" : "stand-in front camera", sizeof(out->name));
    return true;
}

static void *open_camera(void *user, int index, megler_camera_caps *caps, int *error)
{
    static const megler_camera_size back[] = { { 1280, 720, 15, 30 }, { 640, 480, 15, 30 }, { 320, 240, 15, 30 } };
    static const megler_camera_size front[] = { { 640, 480, 15, 30 }, { 320, 240, 15, 30 } };
    camera *c;
    (void)user;
    memset(caps, 0, sizeof(*caps));
    if (index < 0 || index > 1) {
        *error = MEGLER_CAMERA_FAILED;
        mg_strlcpy(caps->why, "the stand-in has two cameras", sizeof(caps->why));
        return NULL;
    }
    mg_lock_acquire(&g_lock);
    if (g_refuse || g_open[index]) {
        *error = g_refuse ? MEGLER_CAMERA_DENIED : MEGLER_CAMERA_BUSY;
        mg_strlcpy(caps->why, g_refuse ? "the stand-in refuses, as Windows does when the user has said no" : "the stand-in's camera is open already",
                   sizeof(caps->why));
        mg_lock_release(&g_lock);
        return NULL;
    }
    g_open[index] = true;
    g_counts.opened++;
    mg_lock_release(&g_lock);
    if (index == 0) {
        caps->size_count = (int)MG_COUNT(back);
        memcpy(caps->sizes, back, sizeof(back));
        caps->focus = caps->macro = caps->torch = true;
    } else {
        caps->size_count = (int)MG_COUNT(front);
        memcpy(caps->sizes, front, sizeof(front));
    }
    c = (camera *)mg_alloc(sizeof(*c));
    c->index = index;
    mg_lock_init(&c->lock);
    *error = MEGLER_CAMERA_OK;
    return c;
}

static void fill(u8 *p, int n, u8 v) { memset(p, v, (size_t)n); }

/* One picture of the four quarters, numbered; rows `ys` and `cs` bytes long, what is past the picture 0xEE. */
static void make(u8 *y, u8 *uv, int w, int h, int ys, int cs, i32 number)
{
    static const u8 tl[3] = { MG_CAMERA_STANDIN_RED }, tr[3] = { MG_CAMERA_STANDIN_GREEN }, bl[3] = { MG_CAMERA_STANDIN_BLUE }, br[3] = { MG_CAMERA_STANDIN_WHITE };
    int r, x;
    for (r = 0; r < h; r++) {
        const u8 *left = r < h / 2 ? tl : bl, *right = r < h / 2 ? tr : br;
        u8 *row = y + (size_t)r * (size_t)ys;
        fill(row, w / 2, left[0]);
        fill(row + w / 2, w - w / 2, right[0]);
        fill(row + w, ys - w, 0xEE);
    }
    for (r = 0; r < h / 2; r++) {
        const u8 *left = r < h / 4 ? tl : bl, *right = r < h / 4 ? tr : br;
        u8 *row = uv + (size_t)r * (size_t)cs;
        for (x = 0; x < w / 2; x++) {
            const u8 *q = x < w / 4 ? left : right;
            row[2 * x] = q[1];
            row[2 * x + 1] = q[2];
        }
        fill(row + w, cs - w, 0xEE);
    }
    y[0] = (u8)number;
    y[1] = (u8)(number >> 8);
    y[2] = (u8)(number >> 16);
    y[3] = (u8)(number >> 24);
}

/* The camera's thread: a picture every 1/fps of a second, and what focusing has come to. */
static void stream(void *arg)
{
    camera *c = (camera *)arg;
    int ys = c->width + 16, cs = c->width + 32;
    u8 *y = (u8 *)mg_alloc((size_t)ys * (size_t)c->height), *uv = (u8 *)mg_alloc((size_t)cs * (size_t)(c->height / 2));
    u64 start = mg_time_monotonic_ns(), period = 1000000000ull / (u64)(c->fps > 0 ? c->fps : 30);
    i64 n = 0;
    while (!c->quit) {
        megler_camera_frame f;
        u64 now = mg_time_monotonic_ns(), due = start + (u64)n * period;
        i32 number;
        if (now < due) {
            mg_sleep_ns(due - now > 5000000ull ? 5000000ull : due - now);
            continue;
        }
        n++;
        mg_lock_acquire(&g_lock);
        number = ++g_number;
        g_counts.frames++;
        mg_lock_release(&g_lock);
        make(y, uv, c->width, c->height, ys, cs, number);
        memset(&f, 0, sizeof(f));
        f.width = c->width;
        f.height = c->height;
        f.y = y;
        f.uv = uv;
        f.y_stride = ys;
        f.uv_stride = cs;
        f.time_us = (i64)(mg_time_monotonic_ns() / 1000);
        c->frame(c->core, &f);
        mg_lock_acquire(&c->lock);
        if (c->focus_mode && mg_time_monotonic_ns() >= c->focus_due) {
            c->focus_mode = 0;
            if (c->done) c->done(c->core, true);
            mg_lock_acquire(&g_lock);
            g_counts.focused++;
            mg_lock_release(&g_lock);
        }
        mg_lock_release(&c->lock);
    }
    free(y);
    free(uv);
}

static bool start(void *user, void *cam, int width, int height, int fps, megler_camera_frame_fn frame, void *core)
{
    camera *c = (camera *)cam;
    (void)user;
    if (c->streaming || width <= 0 || height <= 0 || (width & 1) || (height & 1)) return false;
    c->width = width;
    c->height = height;
    c->fps = fps < 15 ? 15 : fps > 30 ? 30 : fps;
    c->frame = frame;
    c->core = core;
    c->quit = 0;
    c->streaming = true;
    mg_lock_acquire(&g_lock);
    g_counts.started++;
    g_counts.width = width;
    g_counts.height = height;
    g_counts.fps = c->fps;
    mg_lock_release(&g_lock);
    c->thread = mg_thread_start(stream, c, 256 * 1024, "camera stand-in");
    return c->thread != NULL;
}

static bool focus(void *user, void *cam, int mode, megler_camera_focus_fn done, void *core);

static void stop(void *user, void *cam)
{
    camera *c = (camera *)cam;
    (void)user;
    if (!c->streaming) return;
    focus(user, cam, MEGLER_CAMERA_FOCUS_NONE, NULL, NULL);
    mg_atomic_xchg(&c->quit, 1);
    mg_thread_join(c->thread);
    c->thread = NULL;
    c->streaming = false;
    mg_lock_acquire(&g_lock);
    g_counts.stopped++;
    g_counts.torch = false;
    mg_lock_release(&g_lock);
}

static bool focus(void *user, void *cam, int mode, megler_camera_focus_fn done, void *core)
{
    camera *c = (camera *)cam;
    (void)user;
    if (!c->streaming) return false;
    if (mode != MEGLER_CAMERA_FOCUS_NONE && c->index != 0) return false;
    mg_lock_acquire(&c->lock);
    if (mode == MEGLER_CAMERA_FOCUS_NONE) {
        if (c->focus_mode) {
            mg_lock_acquire(&g_lock);
            g_counts.cancelled++;
            mg_lock_release(&g_lock);
        }
        c->focus_mode = 0;
        c->done = NULL;
    } else {
        c->focus_mode = mode;
        c->focus_due = mg_time_monotonic_ns() + (mode == MEGLER_CAMERA_FOCUS_CONTINUOUS ? 0 : 30000000ull);
        c->done = done;
        c->core = core;
    }
    mg_lock_release(&c->lock);
    return true;
}

static void torch(void *user, void *cam, bool on)
{
    camera *c = (camera *)cam;
    (void)user;
    if (!c->streaming || c->index != 0) return;
    mg_lock_acquire(&g_lock);
    g_counts.torch = on;
    mg_lock_release(&g_lock);
    mg_info("camera stand-in: the torch is %s", on ? "on" : "off");
}

static void close_camera(void *user, void *cam)
{
    camera *c = (camera *)cam;
    stop(user, cam);
    mg_lock_acquire(&g_lock);
    g_open[c->index] = false;
    g_counts.closed++;
    mg_lock_release(&g_lock);
    free(c);
}

const megler_camera_host *mg_camera_standin(void)
{
    static const megler_camera_host host = { NULL, count, info, open_camera, start, stop, focus, torch, close_camera };
    return &host;
}
