/*
 * Cameras for Java apps' android.hardware.Camera: meglet.CameraNative's natives, which the framework's Camera (the overlay's,
 * core/framework-overlay/android/hardware/Camera.java) works through. The camera proper is the shell's (meglet_camera_host in
 * meglet.h: Windows.Media.Capture on a phone). What is here:
 *
 * - The cameras as the app asks for them: how many, which way each faces and how far its picture is turned, and the
 *   package manager's features (a camera, one at the back, one at the front, autofocus, a flash).
 * - A camera's pictures, as they come from the shell on a thread of its own, copied at once (the shell's memory is its own
 *   for the call alone) and turned from NV12 into NV21, which is what Android hands an app: three buffers, the one the
 *   shell's thread fills, the latest whole one, and the one the app's preview thread works from, so that neither waits
 *   for the other and a picture the app was slow to take is replaced by the next. No more pictures are kept than the rate
 *   the camera was started at, whatever rate the shell's camera streams at.
 * - The news the app's preview thread waits for (mg_jcamera_wait): a new picture, a focus that ended, the camera gone.
 * - A picture for the app: copied into its buffer as NV21 or YV12 (Android's two preview formats), or into a Bitmap for
 *   the view the preview is shown on, mirrored first for a front camera and then turned by the display orientation, as
 *   Android turns a preview (Camera.setDisplayOrientation), BT.601 video range to colour.
 *
 * Which thread the app's callbacks run on, its buffers, its parameters: the Java side's.
 */
#include "mg_java.h"

#include <stdlib.h>
#include <string.h>

#include "../dvm/mg_dvm.h"
#include "../host/mg_host.h"
#include "mg_java_gfx.h"

#define N(name) static void name(mg_jcall *c)

/* The shell's cameras: set when the app starts, before any of its code runs. */
static meglet_camera_host g_host;

void mg_java_camera_set_host(const meglet_camera_host *host)
{
    if (host) g_host = *host;
    else memset(&g_host, 0, sizeof(g_host));
}

static bool host_usable(void) { return g_host.count && g_host.info && g_host.open && g_host.start && g_host.stop && g_host.close; }

/* Android's ImageFormat numbers for the two preview formats every camera has. */
enum { FORMAT_NV21 = 17, FORMAT_YV12 = 0x32315659 };

typedef struct picture {
    u8 *data;                          /* NV21: the luma, packed, then the V, U pairs, packed */
    size_t cap;
    i32 width, height;
    i64 time_us;
} picture;

struct mg_jcamera {
    int index;
    void *cam;                         /* the shell's; NULL once closed */
    meglet_camera_caps caps;
    mg_lock host_lock;                 /* the shell's calls for this camera, one at a time */
    mg_lock lock;                      /* everything below */
    mg_cond news;
    picture spare, ready, held;        /* the shell's thread fills spare; ready is the latest whole one; held is the app's */
    bool fresh;                        /* ready is newer than held */
    int focus_news;                    /* MG_JCAMERA_FOCUSED or _NOT_FOCUSED, not yet taken */
    bool gone, streaming;
    i32 width, height, fps;            /* as started */
    i64 frames, taken, started_ns;
    bool first_said;
    /* The shell's thread's alone: when the last picture kept was taken, and how many came sooner than the rate asked for. */
    i64 kept_us, dropped;
    bool kept_any;
};

/* The cameras open now, so that the app's end closes them (mg_java_camera_close_all). */
#define MAX_OPEN 8
static mg_lock g_open_lock = MG_LOCK_INIT;
static mg_jcamera *g_open[MAX_OPEN];

static const char *facing_name(int facing) { return facing == MEGLET_CAMERA_FRONT ? "front" : "back"; }

int mg_jcamera_count(void)
{
    int n;
    if (!host_usable()) return 0;
    n = g_host.count(g_host.user);
    return n > 0 ? n : 0;
}

bool mg_jcamera_info(int index, meglet_camera_info *out)
{
    memset(out, 0, sizeof(*out));
    if (!host_usable() || index < 0 || index >= mg_jcamera_count() || !g_host.info(g_host.user, index, out)) return false;
    if (out->facing != MEGLET_CAMERA_FRONT) out->facing = MEGLET_CAMERA_BACK;
    out->orientation = ((out->orientation % 360) + 360) % 360 / 90 * 90;
    out->name[sizeof(out->name) - 1] = 0;
    return true;
}

/*
 * What the package manager says about cameras (PackageManager.hasSystemFeature). A back camera's focus and light may not be
 * known before it has been opened once (meglet_camera_info's -1): focus is then taken as there, as nearly every phone's back
 * camera has it, and a light as not, which an app learns otherwise from the camera's parameters once it opens it.
 */
int mg_jcamera_features(void)
{
    int n = mg_jcamera_count(), i, f = 0;
    for (i = 0; i < n; i++) {
        meglet_camera_info info;
        if (!mg_jcamera_info(i, &info)) continue;
        f |= MG_JCAMERA_ANY;
        if (info.facing == MEGLET_CAMERA_FRONT) {
            f |= MG_JCAMERA_HAS_FRONT;
        } else {
            f |= MG_JCAMERA_HAS_BACK;
            if (info.focus != 0) f |= MG_JCAMERA_HAS_AUTOFOCUS;
            if (info.light > 0) f |= MG_JCAMERA_HAS_FLASH;
        }
    }
    return f;
}

static void remember(mg_jcamera *c, bool on)
{
    int i;
    mg_lock_acquire(&g_open_lock);
    for (i = 0; i < MAX_OPEN; i++) {
        if (on && !g_open[i]) {
            g_open[i] = c;
            break;
        }
        if (!on && g_open[i] == c) {
            g_open[i] = NULL;
            break;
        }
    }
    mg_lock_release(&g_open_lock);
}

mg_jcamera *mg_jcamera_open(int index, int *error)
{
    meglet_camera_info info;
    meglet_camera_caps caps;
    mg_jcamera *c;
    void *cam;
    int err = MEGLET_CAMERA_FAILED, i;
    if (error) *error = MEGLET_CAMERA_FAILED;
    if (!mg_jcamera_info(index, &info)) {
        mg_warn("camera: there is no camera %d (%d cameras)", index, mg_jcamera_count());
        return NULL;
    }
    memset(&caps, 0, sizeof(caps));
    mg_info("camera: opening camera %d, at the %s, turned %d degrees: %s", index, facing_name(info.facing), info.orientation, info.name);
    cam = g_host.open(g_host.user, index, &caps, &err);
    caps.why[sizeof(caps.why) - 1] = 0;
    if (!cam) {
        if (error) *error = err;
        mg_warn("camera: camera %d could not be opened: %s%s%s", index,
                err == MEGLET_CAMERA_DENIED ? "the user or the system refused Meglet the camera"
                : err == MEGLET_CAMERA_BUSY ? "something else has the camera"
                                          : "the shell could not open it",
                caps.why[0] ? ": " : "", caps.why);
        return NULL;
    }
    if (caps.size_count < 0) caps.size_count = 0;
    if (caps.size_count > MEGLET_CAMERA_MAX_SIZES) caps.size_count = MEGLET_CAMERA_MAX_SIZES;
    for (i = 0; i < caps.size_count; i++) {
        if (caps.sizes[i].max_fps <= 0) caps.sizes[i].max_fps = 30;
        if (caps.sizes[i].min_fps <= 0 || caps.sizes[i].min_fps > caps.sizes[i].max_fps) caps.sizes[i].min_fps = caps.sizes[i].max_fps;
    }
    c = (mg_jcamera *)mg_alloc(sizeof(*c));
    c->index = index;
    c->cam = cam;
    c->caps = caps;
    mg_lock_init(&c->host_lock);
    mg_lock_init(&c->lock);
    mg_cond_init(&c->news);
    remember(c, true);
    if (error) *error = MEGLET_CAMERA_OK;
    mg_info("camera: camera %d open: %d sizes, the largest %dx%d at %d to %d frames a second%s%s%s", index, caps.size_count,
            caps.size_count ? caps.sizes[0].width : 0, caps.size_count ? caps.sizes[0].height : 0, caps.size_count ? caps.sizes[0].min_fps : 0,
            caps.size_count ? caps.sizes[0].max_fps : 0, caps.focus ? ", focuses" : ", fixed focus", caps.macro ? ", close up too" : "",
            caps.torch ? ", a torch" : "");
    return c;
}

const meglet_camera_caps *mg_jcamera_caps(mg_jcamera *c) { return c ? &c->caps : NULL; }

/* A picture from the shell, on its thread: NV12 into the spare buffer as NV21, which then becomes the latest. */
static void on_frame(void *core, const meglet_camera_frame *f)
{
    mg_jcamera *c = (mg_jcamera *)core;
    picture *s = &c->spare, t;
    i32 w, h, cw, ch, x, y;
    size_t need;
    if (!f) {
        mg_lock_acquire(&c->lock);
        c->gone = true;
        mg_cond_broadcast(&c->news);
        mg_lock_release(&c->lock);
        mg_warn("camera: camera %d stopped for good (the shell says so)", c->index);
        return;
    }
    w = f->width;
    h = f->height;
    if (w <= 0 || h <= 0 || !f->y || !f->uv || f->y_stride < w || f->uv_stride < (w + 1) / 2 * 2) return;
    /*
     * No more pictures than were asked for. The shell takes the nearest rate its camera has, which may be faster (a phone's
     * preview streams at 30 frames a second, and the app is started at 15 where its range allows: Camera.startPreview); a
     * picture that comes sooner than four fifths of the asked period after the last one kept is dropped before it is copied.
     * Times that do not go forward are taken as no measure, and nothing is dropped by them.
     */
    if (c->fps > 0 && c->kept_any && f->time_us > c->kept_us && f->time_us - c->kept_us < (i64)800000 / c->fps) {
        c->dropped++;
        return;
    }
    c->kept_us = f->time_us;
    c->kept_any = true;
    cw = (w + 1) / 2;
    ch = (h + 1) / 2;
    need = (size_t)w * (size_t)h + (size_t)cw * (size_t)ch * 2;
    if (s->cap < need) {
        free(s->data);
        s->data = (u8 *)mg_alloc(need);
        s->cap = need;
    }
    for (y = 0; y < h; y++) memcpy(s->data + (size_t)y * (size_t)w, f->y + (size_t)y * (size_t)f->y_stride, (size_t)w);
    for (y = 0; y < ch; y++) {
        const u8 *uv = f->uv + (size_t)y * (size_t)f->uv_stride;
        u8 *vu = s->data + (size_t)w * (size_t)h + (size_t)y * (size_t)cw * 2;
        for (x = 0; x < cw; x++) {
            vu[2 * x] = uv[2 * x + 1];
            vu[2 * x + 1] = uv[2 * x];
        }
    }
    s->width = w;
    s->height = h;
    s->time_us = f->time_us;
    mg_lock_acquire(&c->lock);
    t = c->ready;
    c->ready = *s;
    *s = t;
    c->fresh = true;
    c->frames++;
    if (!c->first_said) {
        c->first_said = true;
        mg_info("camera: camera %d's first picture, %dx%d, %lld ms after it was started", c->index, w, h,
                (long long)((mg_time_monotonic_ns() - (u64)c->started_ns) / 1000000));
    }
    mg_cond_broadcast(&c->news);
    mg_lock_release(&c->lock);
}

static void on_focus(void *core, bool focused)
{
    mg_jcamera *c = (mg_jcamera *)core;
    mg_lock_acquire(&c->lock);
    c->focus_news = focused ? MG_JCAMERA_FOCUSED : MG_JCAMERA_NOT_FOCUSED;
    mg_cond_broadcast(&c->news);
    mg_lock_release(&c->lock);
}

bool mg_jcamera_start(mg_jcamera *c, int width, int height, int fps)
{
    bool ok;
    if (!c || !c->cam) return false;
    mg_lock_acquire(&c->host_lock);
    if (c->streaming) {
        mg_lock_release(&c->host_lock);
        return true;
    }
    mg_lock_acquire(&c->lock);
    c->fresh = false;
    c->focus_news = 0;
    c->gone = false;
    c->first_said = false;
    c->frames = c->taken = c->dropped = 0;
    c->kept_any = false;
    c->started_ns = (i64)mg_time_monotonic_ns();
    c->width = width;
    c->height = height;
    c->fps = fps;
    mg_lock_release(&c->lock);
    ok = g_host.start(g_host.user, c->cam, width, height, fps, on_frame, c);
    c->streaming = ok;
    mg_lock_release(&c->host_lock);
    if (ok) mg_info("camera: camera %d streams at %dx%d, %d frames a second at most", c->index, width, height, fps);
    else mg_warn("camera: camera %d would not stream at %dx%d, %d frames a second", c->index, width, height, fps);
    return ok;
}

void mg_jcamera_stop(mg_jcamera *c)
{
    i64 frames;
    if (!c || !c->cam) return;
    mg_lock_acquire(&c->host_lock);
    if (!c->streaming) {
        mg_lock_release(&c->host_lock);
        return;
    }
    if (g_host.focus) g_host.focus(g_host.user, c->cam, MEGLET_CAMERA_FOCUS_NONE, NULL, NULL);
    g_host.stop(g_host.user, c->cam);
    c->streaming = false;
    mg_lock_release(&c->host_lock);
    mg_lock_acquire(&c->lock);
    c->fresh = false;
    c->focus_news = 0;
    frames = c->frames;
    mg_cond_broadcast(&c->news);
    mg_lock_release(&c->lock);
    mg_info("camera: camera %d stopped after %lld pictures, %lld of them taken by the app (and %lld that came sooner than asked for, dropped)", c->index,
            (long long)frames, (long long)c->taken, (long long)c->dropped);
}

int mg_jcamera_wait(mg_jcamera *c, int ms)
{
    int news = 0;
    u64 until;
    if (!c) return MG_JCAMERA_GONE;
    until = mg_time_monotonic_ns() + (u64)(ms > 0 ? ms : 0) * 1000000ull;
    mg_lock_acquire(&c->lock);
    for (;;) {
        u64 now;
        if (c->focus_news) {
            news |= c->focus_news;
            c->focus_news = 0;
        }
        if (c->fresh) {
            picture t = c->held;
            c->held = c->ready;
            c->ready = t;
            c->fresh = false;
            c->taken++;
            news |= MG_JCAMERA_FRAME;
        }
        if (c->gone || !c->cam) news |= MG_JCAMERA_GONE;
        now = mg_time_monotonic_ns();
        if (news || now >= until) break;
        mg_cond_wait_ns(&c->news, &c->lock, until - now);
    }
    mg_lock_release(&c->lock);
    return news;
}

void mg_jcamera_counts(mg_jcamera *c, i64 *kept, i64 *dropped)
{
    *kept = *dropped = 0;
    if (!c) return;
    mg_lock_acquire(&c->lock);
    *kept = c->frames;
    *dropped = c->dropped;
    mg_lock_release(&c->lock);
}

i32 mg_jcamera_size(mg_jcamera *c) { return c && c->held.data ? c->held.width << 16 | (c->held.height & 0xffff) : 0; }

i64 mg_jcamera_time(mg_jcamera *c) { return c && c->held.data ? c->held.time_us : 0; }

/* Android's sizes: NV21 is the luma and the chroma at a quarter; YV12's rows are 16-byte aligned, the chroma's as well. */
size_t mg_jcamera_frame_bytes(i32 width, i32 height, int format)
{
    if (width <= 0 || height <= 0) return 0;
    if (format == FORMAT_YV12) {
        size_t ys = ((size_t)width + 15) & ~(size_t)15, cs = (ys / 2 + 15) & ~(size_t)15;
        return ys * (size_t)height + cs * (size_t)((height + 1) / 2) * 2;
    }
    return (size_t)width * (size_t)height + (size_t)((width + 1) / 2) * (size_t)((height + 1) / 2) * 2;
}

i32 mg_jcamera_copy(mg_jcamera *c, u8 *dst, size_t size, int format)
{
    const picture *p;
    size_t need;
    i32 w, h, cw, ch, y, x;
    if (!c || !c->held.data || !dst) return -1;
    p = &c->held;
    w = p->width;
    h = p->height;
    need = mg_jcamera_frame_bytes(w, h, format);
    if (size < need) return -1;
    if (format != FORMAT_YV12) {
        memcpy(dst, p->data, need);
        return (i32)need;
    }
    {
        /* YV12: the luma in rows of the aligned stride, then V's plane, then U's. */
        size_t ys = ((size_t)w + 15) & ~(size_t)15, cs = (ys / 2 + 15) & ~(size_t)15;
        const u8 *vu = p->data + (size_t)w * (size_t)h;
        u8 *vp, *up;
        cw = (w + 1) / 2;
        ch = (h + 1) / 2;
        memset(dst, 0, need);
        for (y = 0; y < h; y++) memcpy(dst + (size_t)y * ys, p->data + (size_t)y * (size_t)w, (size_t)w);
        vp = dst + ys * (size_t)h;
        up = vp + cs * (size_t)ch;
        for (y = 0; y < ch; y++)
            for (x = 0; x < cw; x++) {
                vp[(size_t)y * cs + (size_t)x] = vu[((size_t)y * (size_t)cw + (size_t)x) * 2];
                up[(size_t)y * cs + (size_t)x] = vu[((size_t)y * (size_t)cw + (size_t)x) * 2 + 1];
            }
    }
    return (i32)need;
}

static u8 clamp8(i32 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

/*
 * The picture held into memory of the core's bitmaps (B, G, R, A, top row first): mirrored left to right first when asked
 * (a front camera, as Android shows its preview), then turned `rotation` degrees clockwise, so that the result is `width` x
 * `height` = the picture's height x width for 90 and 270. A pixel (dx, dy) of the result is the picture's (fx, fy), each a
 * sum of dx and dy with a constant: a step along a row of the result is a fixed step in the picture. BT.601 video range,
 * which is what a camera's NV12 is, from tables of 256.
 */
bool mg_jcamera_render(mg_jcamera *c, void *bgra, i32 width, i32 height, i32 stride, int rotation, bool mirror)
{
    const picture *p;
    i32 w, h, cw, dx, dy, fx0, fy0, fxx, fyx, fxy, fyy, i;
    i32 lt[256], rt[256], gut[256], gvt[256], bt[256];
    const u8 *luma, *vu;
    if (!c || !c->held.data || !bgra) return false;
    p = &c->held;
    w = p->width;
    h = p->height;
    rotation = ((rotation % 360) + 360) % 360 / 90 * 90;
    if ((rotation % 180 ? h : w) != width || (rotation % 180 ? w : h) != height) return false;
    cw = (w + 1) / 2;
    luma = p->data;
    vu = p->data + (size_t)w * (size_t)h;
    /* (fx, fy) = (fx0 + fxx dx + fxy dy, fy0 + fyx dx + fyy dy) */
    switch (rotation) {
    case 90: fx0 = 0, fxx = 0, fxy = 1, fy0 = h - 1, fyx = -1, fyy = 0; break;
    case 180: fx0 = w - 1, fxx = -1, fxy = 0, fy0 = h - 1, fyx = 0, fyy = -1; break;
    case 270: fx0 = w - 1, fxx = 0, fxy = -1, fy0 = 0, fyx = 1, fyy = 0; break;
    default: fx0 = 0, fxx = 1, fxy = 0, fy0 = 0, fyx = 0, fyy = 1; break;
    }
    if (mirror) fx0 = w - 1 - fx0, fxx = -fxx, fxy = -fxy;
    for (i = 0; i < 256; i++) {
        lt[i] = 298 * (i - 16) + 128;
        rt[i] = 409 * (i - 128);
        gut[i] = 100 * (i - 128);
        gvt[i] = 208 * (i - 128);
        bt[i] = 516 * (i - 128);
    }
    for (dy = 0; dy < height; dy++) {
        i32 fx = fx0 + fxy * dy, fy = fy0 + fyy * dy;
        u8 *d = (u8 *)bgra + (size_t)dy * (size_t)stride;
        for (dx = 0; dx < width; dx++, fx += fxx, fy += fyx, d += 4) {
            const u8 *pair = vu + ((size_t)(fy >> 1) * (size_t)cw + (size_t)(fx >> 1)) * 2;
            i32 l = lt[luma[(size_t)fy * (size_t)w + (size_t)fx]];
            d[0] = clamp8((l + bt[pair[1]]) >> 8);
            d[1] = clamp8((l - gut[pair[1]] - gvt[pair[0]]) >> 8);
            d[2] = clamp8((l + rt[pair[0]]) >> 8);
            d[3] = 255;
        }
    }
    return true;
}

bool mg_jcamera_focus(mg_jcamera *c, int mode)
{
    bool ok = false;
    if (!c || !c->cam || !g_host.focus) return false;
    mg_lock_acquire(&c->host_lock);
    if (c->streaming) {
        if (mode == MEGLET_CAMERA_FOCUS_NONE) {
            mg_lock_acquire(&c->lock);
            c->focus_news = 0;
            mg_lock_release(&c->lock);
        }
        ok = g_host.focus(g_host.user, c->cam, mode, on_focus, c);
        /* (What a focus asked before this one said, if it came after the cancel, is not this one's answer.) */
        if (mode == MEGLET_CAMERA_FOCUS_NONE) {
            mg_lock_acquire(&c->lock);
            c->focus_news = 0;
            mg_lock_release(&c->lock);
        }
    }
    mg_lock_release(&c->host_lock);
    return ok;
}

void mg_jcamera_torch(mg_jcamera *c, bool on)
{
    if (!c || !c->cam || !g_host.torch || !c->caps.torch) return;
    mg_lock_acquire(&c->host_lock);
    if (c->streaming) g_host.torch(g_host.user, c->cam, on);
    mg_lock_release(&c->host_lock);
    mg_info("camera: camera %d's torch %s", c->index, on ? "on" : "off");
}

/* Stopped and closed with the shell. The memory stays when `keep`: a thread of the app's may still hold the handle. */
static void close_camera(mg_jcamera *c, bool keep)
{
    void *cam;
    mg_jcamera_stop(c);
    mg_lock_acquire(&c->host_lock);
    cam = c->cam;
    c->cam = NULL;
    if (cam) g_host.close(g_host.user, cam);
    mg_lock_release(&c->host_lock);
    if (!cam) return;
    remember(c, false);
    mg_lock_acquire(&c->lock);
    mg_cond_broadcast(&c->news);
    mg_lock_release(&c->lock);
    mg_info("camera: camera %d closed", c->index);
    if (keep) return;
    free(c->spare.data);
    free(c->ready.data);
    free(c->held.data);
    free(c);
}

void mg_jcamera_close(mg_jcamera *c)
{
    if (c) close_camera(c, false);
}

void mg_java_camera_close_all(void)
{
    int i;
    for (i = 0; i < MAX_OPEN; i++) {
        mg_jcamera *c;
        mg_lock_acquire(&g_open_lock);
        c = g_open[i];
        mg_lock_release(&g_open_lock);
        if (c) {
            mg_info("camera: the app ended with camera %d open: it is closed", c->index);
            close_camera(c, true);
        }
    }
}

/* ------------------------------------------------------------ natives */

#define CAMERA ((mg_jcamera *)(mg_addr)c->args[0].j)

N(Camera_count) { c->ret.i = mg_jcamera_count(); }

/* info(int index, int[] out) -> false for no such camera; out = { facing, orientation, focus, light } */
N(Camera_info)
{
    meglet_camera_info info;
    mg_jobj *a = c->args[1].l;
    c->ret.i = 0;
    if (!mg_jcamera_info(c->args[0].i, &info)) return;
    if (a && a->u.arr.len >= 4) {
        i32 *v = (i32 *)a->u.arr.data;
        v[0] = info.facing;
        v[1] = info.orientation;
        v[2] = info.focus;
        v[3] = info.light;
    }
    c->ret.i = 1;
}

N(Camera_features) { c->ret.i = mg_jcamera_features(); }

/* open(int index) -> a handle, 0 when the camera could not be opened (the log says why) */
N(Camera_open) { c->ret.j = (i64)(mg_addr)mg_jcamera_open(c->args[0].i, NULL); }

/* caps(long h) -> { focus, macro, torch, n, then width, height, min fps, max fps for each of the n sizes } */
N(Camera_caps)
{
    const meglet_camera_caps *k = mg_jcamera_caps(CAMERA);
    mg_jobj *a;
    i32 *v;
    int i;
    c->ret.l = NULL;
    if (!k) return;
    a = mg_dvm_new_array("[I", 4 + 4 * k->size_count);
    if (!a) return;
    v = (i32 *)a->u.arr.data;
    v[0] = k->focus;
    v[1] = k->macro;
    v[2] = k->torch;
    v[3] = k->size_count;
    for (i = 0; i < k->size_count; i++) {
        v[4 + 4 * i] = k->sizes[i].width;
        v[5 + 4 * i] = k->sizes[i].height;
        v[6 + 4 * i] = k->sizes[i].min_fps;
        v[7 + 4 * i] = k->sizes[i].max_fps;
    }
    c->ret.l = a;
}

N(Camera_start) { c->ret.i = mg_jcamera_start(CAMERA, c->args[1].i, c->args[2].i, c->args[3].i) ? 1 : 0; }
N(Camera_stop) { mg_jcamera_stop(CAMERA); }
N(Camera_waitNews) { c->ret.i = mg_jcamera_wait(CAMERA, c->args[1].i); }
N(Camera_frameSize) { c->ret.i = mg_jcamera_size(CAMERA); }
N(Camera_frameBytes) { c->ret.i = (i32)mg_jcamera_frame_bytes(c->args[0].i, c->args[1].i, c->args[2].i); }

/* copyFrame(long h, byte[] dst, int format) -> bytes written, -1 when dst is too small or there is no picture */
N(Camera_copyFrame)
{
    mg_jobj *a = c->args[1].l;
    c->ret.i = a ? mg_jcamera_copy(CAMERA, (u8 *)a->u.arr.data, (size_t)a->u.arr.len, c->args[2].i) : -1;
}

/* The core's bitmap of an android.graphics.Bitmap: its field mNative. */
static i64 bitmap_of(mg_jobj *bitmap)
{
    mg_jvalue v;
    v.j = 0;
    if (!bitmap || !mg_dvm_get_field(bitmap, "mNative", &v)) mg_dvm_clear_pending();
    return v.j;
}

/* render(long h, Bitmap dst, int rotation, boolean mirror): the picture into a Bitmap of its turned size */
N(Camera_render)
{
    mg_java_gfx_bitmap b;
    i64 bm = bitmap_of(c->args[1].l);
    c->ret.i = 0;
    if (!bm || !mg_java_gfx_bitmap_info(bm, &b)) return;
    if (!mg_jcamera_render(CAMERA, b.pixels, b.width, b.height, b.stride, c->args[2].i, c->args[3].i != 0)) return;
    mg_java_gfx_bitmap_changed(bm);
    c->ret.i = 1;
}

N(Camera_focus) { c->ret.i = mg_jcamera_focus(CAMERA, c->args[1].i) ? 1 : 0; }
N(Camera_torch) { mg_jcamera_torch(CAMERA, c->args[1].i != 0); }
N(Camera_close) { mg_jcamera_close(CAMERA); }

static const mg_jhle k_camera[] = {
    { "meglet/CameraNative", "count", NULL, Camera_count },
    { "meglet/CameraNative", "info", NULL, Camera_info },
    { "meglet/CameraNative", "features", NULL, Camera_features },
    { "meglet/CameraNative", "open", NULL, Camera_open },
    { "meglet/CameraNative", "caps", NULL, Camera_caps },
    { "meglet/CameraNative", "start", NULL, Camera_start },
    { "meglet/CameraNative", "stop", NULL, Camera_stop },
    { "meglet/CameraNative", "waitNews", NULL, Camera_waitNews },
    { "meglet/CameraNative", "frameSize", NULL, Camera_frameSize },
    { "meglet/CameraNative", "frameBytes", NULL, Camera_frameBytes },
    { "meglet/CameraNative", "copyFrame", NULL, Camera_copyFrame },
    { "meglet/CameraNative", "render", NULL, Camera_render },
    { "meglet/CameraNative", "focus", NULL, Camera_focus },
    { "meglet/CameraNative", "torch", NULL, Camera_torch },
    { "meglet/CameraNative", "close", NULL, Camera_close },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_camera_natives(void) { return k_camera; }
