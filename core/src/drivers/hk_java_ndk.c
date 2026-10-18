/*
 * The NDK's functions that are about the framework's Java objects, for the
 * native libraries of a Java app. The rest of the NDK is in ndk/ and needs no
 * Java; these need the Java driver's framework under them, and answer "bad
 * parameter" for anything that is not one of its objects.
 *
 * libjnigraphics: AndroidBitmap_getInfo, AndroidBitmap_lockPixels,
 * AndroidBitmap_unlockPixels on an android.graphics.Bitmap. The Bitmap's
 * pixels are the core's own memory (hk_java_gfx.c), kept as B, G, R, A,
 * premultiplied; native code is promised ANDROID_BITMAP_FORMAT_RGBA_8888, so
 * a lock turns the memory to that order where it lies and the unlock turns
 * it back and takes the bitmap as changed (hk_java_gfx_lock_rgba). Every
 * Bitmap is 32 bits a pixel here whatever its Config says, and that is the
 * format reported: code that asks for the format first, as the NDK's
 * documentation has it do, is told the truth.
 *
 * libandroid: ANativeWindow_fromSurface on the framework's Surface of a
 * SurfaceView (its meglerView): a window of the view's surface, below. A
 * Surface a driver made carries its window as it always did.
 */
#include "../bionic/hk_bionic.h"
#include "../dvm/hk_dvm_internal.h"
#include "../ndk/hk_ndk.h"
#include "hk_java_gfx.h"
#include "hk_java_gl.h"

enum { G_BITMAP_OK = 0, G_BITMAP_BAD_PARAMETER = -1, G_BITMAP_JNI_EXCEPTION = -2 };
enum { G_BITMAP_FORMAT_RGBA_8888 = 1 };

/* AndroidBitmapInfo. */
typedef struct g_bitmap_info {
    u32 width, height, stride;
    i32 format;
    u32 flags;                         /* 0: premultiplied alpha, no hardware buffer */
} g_bitmap_info;

/* The core's bitmap of an android.graphics.Bitmap: its field mNative; 0 for anything else, and for a recycled one. */
static i64 bitmap_of(hk_jobj *bitmap)
{
    hk_jvalue v;
    v.j = 0;
    if (!bitmap || !bitmap->dvm || !hk_dvm_instance_of(bitmap, "android/graphics/Bitmap")) return 0;
    if (!hk_dvm_get_field(bitmap, "mNative", &v)) hk_dvm_clear_pending();
    return v.j;
}

static int g_bitmap_get_info(void *env, hk_jobj *bitmap, g_bitmap_info *info)
{
    hk_java_gfx_bitmap b;
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (!bm || !hk_java_gfx_bitmap_info(bm, &b)) return G_BITMAP_BAD_PARAMETER;
    if (info) {
        info->width = (u32)b.width;
        info->height = (u32)b.height;
        info->stride = (u32)b.stride;
        info->format = G_BITMAP_FORMAT_RGBA_8888;
        info->flags = 0;
    }
    return G_BITMAP_OK;
}

static int g_bitmap_lock(void *env, hk_jobj *bitmap, void **address)
{
    hk_java_gfx_bitmap b;
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (address) *address = NULL;
    if (!bm) return G_BITMAP_BAD_PARAMETER;
    if (!hk_java_gfx_lock_rgba(bm, &b)) return G_BITMAP_JNI_EXCEPTION;
    if (address) *address = b.pixels;
    return G_BITMAP_OK;
}

static int g_bitmap_unlock(void *env, hk_jobj *bitmap)
{
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (!bm) return G_BITMAP_BAD_PARAMETER;
    hk_java_gfx_unlock_rgba(bm);
    return G_BITMAP_OK;
}

/* ------------------------------------------- the window of a SurfaceView's surface
 *
 * What ANativeWindow_fromSurface gives for a SurfaceView's Surface: a window of the view's surface as it is now, as on
 * Android, where the window is the Surface's native object while the surface lasts. The same window for every call while the
 * surface is there (each call a reference of the caller's); its size the surface's, following surfaceChanged (the view's
 * mSurfaceW and mSurfaceH, set before the callbacks), unless ANativeWindow_setBuffersGeometry asked for another; its format the
 * holder's (OPAQUE, the overlay's default, is RGBX_8888). When the surface is destroyed -- told by EGLNative.forget after the
 * surfaceDestroyed callbacks -- the window is no longer valid, for good: what it is asked answers NO_INIT, a swap of an EGL
 * surface on it fails with EGL_BAD_NATIVE_WINDOW, a lock is refused; it is freed when native code lets go of its last reference.
 * A surface that comes back is a new window.
 *
 * Drawn on with EGL (ndk/hk_egl.c's eglCreateWindowSurface), the window's EGL surface is hk_java_gl.c's: a pbuffer under the
 * view's hole, composed into the window as the frames of an EGL surface made from Java on the view are. Drawn on in software
 * (ANativeWindow_lock, unlockAndPost), the buffer is memory of the window's own, kept from one frame to the next as Android's
 * is in effect (it copies the last frame back); a post turns it into one of two Bitmaps in turn, B, G, R, A, opaque, and hands
 * that to the view as a whole frame (SurfaceView.meglerPostBitmap, which the camera's preview uses too), which the view draws in
 * its place scaled to it. Posts are paced at sixty a second, as a swap is.
 */

#define MAX_WINDOWS 16
#define NW_GONE (-19)                  /* status_t NO_INIT */
#define NW_BAD_VALUE (-22)
#define NW_INVALID_OPERATION (-38)
enum { FORMAT_RGBA_8888 = 1, FORMAT_RGBX_8888 = 2, FORMAT_RGB_565 = 4 };

typedef struct view_window {
    hk_nwindow base;                   /* first: what native code holds */
    hk_jobj *view;                     /* the SurfaceView (a root of the collector's); NULL: a free slot */
    volatile i32 valid;                /* until the view's surface is destroyed */
    volatile i32 egl;                  /* EGL window surfaces on it now */
    volatile i32 locked;
    bool drawn_said;
    u8 *pixels;                        /* the software buffer, rows top first */
    size_t pixels_size;
    i32 bw, bh, bformat;               /* as it was last locked */
    hk_jobj *bitmaps[2];               /* roots: the posted buffer goes to the view in these, in turn */
    int next;
    u64 due;                           /* when the next post may return */
} view_window;

/* An EGL window surface on such a window: hk_java_gl.c's, and the window it is on. */
typedef struct view_surface {
    view_window *w;
    void *gl;
} view_surface;

static view_window g_windows[MAX_WINDOWS];
static hk_lock g_windows_lock = HK_LOCK_INIT;
static bool g_windows_rooted;

/* The fields of the overlay's SurfaceView the windows read, by their slots (any thread may ask a window its size). */
static struct {
    volatile i32 state;                /* 0 not looked for, 1 found, 2 not there */
    u32 created, width, height, format;
} SV;

static bool surface_view_fields(void)
{
    if (!SV.state) {
        hk_dvm_class *c = hk_dvm_find_class("android/view/SurfaceView");
        hk_dvm_field *created = c ? hk_dvm_find_field(c, "mCreated", false) : NULL, *width = c ? hk_dvm_find_field(c, "mSurfaceW", false) : NULL;
        hk_dvm_field *height = c ? hk_dvm_find_field(c, "mSurfaceH", false) : NULL, *format = c ? hk_dvm_find_field(c, "mFormat", false) : NULL;
        bool ok = created && width && height && format;
        if (!c) hk_dvm_clear_pending();
        if (ok) {
            SV.created = created->slot;
            SV.width = width->slot;
            SV.height = height->slot;
            SV.format = format->slot;
        } else {
            hk_warn("java: the framework's SurfaceView has no mCreated, mSurfaceW, mSurfaceH or mFormat: native code is given no window of one");
        }
        hk_atomic_xchg(&SV.state, ok ? 1 : 2);
    }
    return SV.state == 1;
}

static void vw_size(const hk_nwindow *b, int *width, int *height)
{
    const view_window *w = (const view_window *)b;
    hk_jvalue *slots = hk_dvm_slots(w->view);
    i32 sw = slots[SV.width].i, sh = slots[SV.height].i;
    if (sw > 0 && sh > 0) {
        *width = sw;
        *height = sh;
    }
}

static bool vw_valid(const hk_nwindow *b) { return ((const view_window *)b)->valid != 0; }

static void *vw_egl_create(hk_nwindow *b, void *const *configs, int count)
{
    view_window *w = (view_window *)b;
    view_surface *s;
    void *gl;
    int width, height;
    if (w->locked) {
        hk_note_once("java: an EGL surface was asked for on a window locked for drawing in software: refused, as on Android");
        return NULL;
    }
    hk_nwindow_buffer_size(b, &width, &height);
    gl = hk_java_gl_window_surface(w->view, width, height, configs, count);
    if (!gl) return NULL;
    s = (view_surface *)hk_alloc(sizeof(*s));
    s->w = w;
    s->gl = gl;
    hk_atomic_add(&w->egl, 1);
    return s;
}

static void *vw_egl_handle(void *p, int *width, int *height) { return hk_java_gl_window_handle(((view_surface *)p)->gl, width, height); }

static bool vw_egl_swap(void *p)
{
    view_surface *s = (view_surface *)p;
    int width, height;
    if (!s->w->valid) {
        hk_note_once("java: native code swapped an EGL surface whose SurfaceView's surface has gone: EGL_BAD_NATIVE_WINDOW, as on Android");
        return false;
    }
    hk_nwindow_buffer_size(&s->w->base, &width, &height);
    hk_java_gl_window_swap(s->gl, width, height);
    return true;
}

static void vw_egl_destroy(void *p)
{
    view_surface *s = (view_surface *)p;
    hk_java_gl_window_destroy(s->gl);
    hk_atomic_add(&s->w->egl, -1);
    free(s);
}

/* ANativeWindow_Buffer, as the NDK lays it out. */
typedef struct anw_buffer {
    i32 width, height, stride, format;
    void *bits;
    u32 reserved[6];
} anw_buffer;

static int vw_lock(hk_nwindow *b, void *buffer, i32 *dirty)
{
    view_window *w = (view_window *)b;
    anw_buffer *out = (anw_buffer *)buffer;
    int width, height, format, bpp;
    size_t need;
    bool kept;
    if (!out) return NW_BAD_VALUE;
    if (!w->valid) return NW_GONE;
    if (w->egl) {
        hk_note_once("java: ANativeWindow_lock on a window with an EGL surface on it: refused, as on Android");
        return NW_BAD_VALUE;
    }
    if (hk_atomic_xchg(&w->locked, 1)) return NW_INVALID_OPERATION;
    hk_nwindow_buffer_size(b, &width, &height);
    format = b->format == FORMAT_RGB_565 ? FORMAT_RGB_565 : b->format == FORMAT_RGBX_8888 ? FORMAT_RGBX_8888 : FORMAT_RGBA_8888;
    bpp = format == FORMAT_RGB_565 ? 2 : 4;
    need = (size_t)width * (size_t)height * (size_t)bpp;
    kept = w->pixels && w->bw == width && w->bh == height && w->bformat == format;
    if (!kept) {
        if (w->pixels_size < need) {
            free(w->pixels);
            w->pixels = (u8 *)hk_alloc(need);
            w->pixels_size = need;
        }
        memset(w->pixels, 0, need);
        w->bw = width;
        w->bh = height;
        w->bformat = format;
    }
    out->width = width;
    out->height = height;
    out->stride = width;
    out->format = format;
    out->bits = w->pixels;
    memset(out->reserved, 0, sizeof(out->reserved));
    /* What is to be drawn again: what was asked, where the last frame is still in the buffer; all of it where it is new. */
    if (dirty && !kept) {
        dirty[0] = dirty[1] = 0;
        dirty[2] = width;
        dirty[3] = height;
    }
    if (!w->drawn_said) {
        w->drawn_said = true;
        hk_info("java: native code draws a window of %s in software (ANativeWindow_lock): %dx%d, format %d", hk_dvm_class_name(w->view), width, height, format);
    }
    return 0;
}

/* A Bitmap of the framework's, ARGB_8888; NULL when none could be made. */
static hk_jobj *new_bitmap(int width, int height)
{
    hk_jvalue config, args[3], ret;
    config.j = 0;
    if (!hk_dvm_get_static("android/graphics/Bitmap$Config", "ARGB_8888", &config) || !config.l) {
        hk_dvm_clear_pending();
        return NULL;
    }
    memset(args, 0, sizeof(args));
    args[0].i = width;
    args[1].i = height;
    args[2].l = config.l;
    ret.j = 0;
    if (!hk_dvm_call_static("android/graphics/Bitmap", "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", args, &ret)) {
        hk_dvm_clear_pending();
        return NULL;
    }
    return ret.l;
}

/* The core's bitmap of a Bitmap of this width and height in the slot, the slot given a new one when it has none of that size. */
static i64 post_bitmap(hk_jobj **slot, int width, int height, hk_java_gfx_bitmap *out)
{
    int round;
    for (round = 0; round < 2; round++) {
        hk_jvalue v;
        v.j = 0;
        if (*slot && !hk_dvm_get_field(*slot, "mNative", &v)) hk_dvm_clear_pending();
        if (v.j && hk_java_gfx_bitmap_info(v.j, out) && out->width == width && out->height == height) return v.j;
        if (!round) *slot = new_bitmap(width, height);
    }
    return 0;
}

/* The buffer, as a whole frame of the view's: into the next Bitmap, B, G, R, A and opaque, which the view is handed. */
static void post(view_window *w)
{
    hk_java_gfx_bitmap b;
    hk_jvalue args[1], ret;
    i64 bm = post_bitmap(&w->bitmaps[w->next], w->bw, w->bh, &b);
    i32 x, y;
    if (!bm) {
        hk_note_once("java: no Bitmap for a window's software buffer: its frames are not shown");
        return;
    }
    for (y = 0; y < w->bh; y++) {
        u8 *d = (u8 *)b.pixels + (size_t)y * (size_t)b.stride;
        if (w->bformat == FORMAT_RGB_565) {
            const u16 *s = (const u16 *)(const void *)(w->pixels + (size_t)y * (size_t)w->bw * 2);
            for (x = 0; x < w->bw; x++, d += 4) {
                u32 v = s[x], r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31;
                d[0] = (u8)((bl << 3) | (bl >> 2));
                d[1] = (u8)((g << 2) | (g >> 4));
                d[2] = (u8)((r << 3) | (r >> 2));
                d[3] = 255;
            }
        } else {
            const u8 *s = w->pixels + (size_t)y * (size_t)w->bw * 4;
            for (x = 0; x < w->bw; x++, d += 4, s += 4) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = 255;
            }
        }
    }
    hk_java_gfx_bitmap_changed(bm);
    memset(args, 0, sizeof(args));
    args[0].l = w->bitmaps[w->next];
    ret.j = 0;
    if (!hk_dvm_call_virtual(w->view, "meglerPostBitmap", "(Landroid/graphics/Bitmap;)Z", args, &ret)) hk_dvm_clear_pending();
    /* The view has this one as its front buffer now: the next post goes into the other. */
    else if (ret.z) w->next ^= 1;
}

static int vw_unlock(hk_nwindow *b)
{
    view_window *w = (view_window *)b;
    if (!w->locked) return NW_BAD_VALUE;
    if (w->valid) {
        const u64 period = 16666667ull;
        u64 now;
        post(w);
        /* No sooner than a sixtieth of a second after the post before, as a queued buffer is taken on Android. */
        now = hk_time_monotonic_ns();
        if (!w->due || now > w->due + 100000000ull) w->due = now;
        w->due += period;
        if (w->due > now) hk_sleep_ns(w->due - now);
    }
    hk_atomic_xchg(&w->locked, 0);
    return w->valid ? 0 : NW_GONE;
}

static void vw_gone(hk_nwindow *b)
{
    view_window *w = (view_window *)b;
    hk_lock_acquire(&g_windows_lock);
    free(w->pixels);
    /* (The slot is free with its view gone; the collector's roots in it now point at nothing.) */
    memset(w, 0, sizeof(*w));
    hk_lock_release(&g_windows_lock);
}

static const hk_nwindow_ops k_view_window = { vw_size, vw_valid, vw_lock, vw_unlock, vw_egl_create, vw_egl_handle, vw_egl_swap, vw_egl_destroy, vw_gone };

/* The window of a SurfaceView's surface now, a reference of the caller's; NULL while it has none (Android's Surface is not valid then). */
static hk_nwindow *view_window_of(hk_jobj *view)
{
    view_window *w = NULL, *free_slot = NULL;
    hk_jvalue *slots;
    i32 format;
    int i;
    if (!surface_view_fields()) return NULL;
    slots = hk_dvm_slots(view);
    if (!slots[SV.created].z) {
        hk_info("java: ANativeWindow_fromSurface: %s has no surface now: NULL", hk_dvm_class_name(view));
        return NULL;
    }
    hk_lock_acquire(&g_windows_lock);
    if (!g_windows_rooted) {
        for (i = 0; i < MAX_WINDOWS; i++) {
            hk_dvm_add_root(&g_windows[i].view);
            hk_dvm_add_root(&g_windows[i].bitmaps[0]);
            hk_dvm_add_root(&g_windows[i].bitmaps[1]);
        }
        g_windows_rooted = true;
    }
    for (i = 0; i < MAX_WINDOWS && !w; i++) {
        if (g_windows[i].view == view && g_windows[i].valid) w = &g_windows[i];
        else if (!g_windows[i].view && !free_slot) free_slot = &g_windows[i];
    }
    if (w) {
        hk_nwindow_acquire(&w->base);
        hk_lock_release(&g_windows_lock);
        return &w->base;
    }
    if (!free_slot) {
        hk_lock_release(&g_windows_lock);
        hk_warn("java: ANativeWindow_fromSurface: native code holds %d windows already: NULL", MAX_WINDOWS);
        return NULL;
    }
    w = free_slot;
    memset(w, 0, sizeof(*w));
    w->base.refs = 1;
    w->base.ops = &k_view_window;
    /* PixelFormat: OPAQUE (-1) is RGBX_8888 as Android's compositor makes it, TRANSLUCENT and TRANSPARENT RGBA_8888, the rest themselves. */
    format = slots[SV.format].i;
    w->base.format = format > 0 ? format : format == -1 ? FORMAT_RGBX_8888 : FORMAT_RGBA_8888;
    w->base.width = slots[SV.width].i;
    w->base.height = slots[SV.height].i;
    w->view = view;
    w->valid = 1;
    hk_lock_release(&g_windows_lock);
    hk_info("java: ANativeWindow_fromSurface: a window of %s's surface, %dx%d, format %d", hk_dvm_class_name(view), w->base.width, w->base.height, w->base.format);
    return &w->base;
}

void hk_java_ndk_view_gone(hk_jobj *view)
{
    int i, gone = 0;
    hk_lock_acquire(&g_windows_lock);
    for (i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].view != view || !g_windows[i].valid) continue;
        hk_atomic_xchg(&g_windows[i].valid, 0);
        gone++;
    }
    hk_lock_release(&g_windows_lock);
    if (gone) hk_info("java: the surface of %s has gone: native code's window of it is no longer valid", hk_dvm_class_name(view));
}

static hk_nwindow *g_window_from_surface(void *env, hk_jobj *surface)
{
    hk_jvalue v;
    (void)env;
    if (!surface) return NULL;
    if (!surface->dvm) {
        /* A Surface a driver made carries its window (drivers/hk_unity.c); the caller owns a reference, as on Android. */
        hk_nwindow *w = (hk_nwindow *)surface->native;
        if (w) hk_nwindow_acquire(w);
        return w;
    }
    v.j = 0;
    if (!hk_dvm_get_field(surface, "meglerView", &v)) hk_dvm_clear_pending();
    if (v.l) return view_window_of(v.l);
    v.j = 0;
    if (!hk_dvm_get_field(surface, "meglerTexture", &v)) hk_dvm_clear_pending();
    hk_note_once("java: ANativeWindow_fromSurface on a Surface %s: no window of it here, NULL", v.l ? "of a SurfaceTexture" : "of neither a SurfaceView nor a SurfaceTexture");
    return NULL;
}

static const hk_sym k_java_ndk[] = {
    HK_WRAP("AndroidBitmap_getInfo", g_bitmap_get_info), HK_WRAP("AndroidBitmap_lockPixels", g_bitmap_lock),
    HK_WRAP("AndroidBitmap_unlockPixels", g_bitmap_unlock),
    HK_WRAP("ANativeWindow_fromSurface", g_window_from_surface),
    HK_END
};

const hk_sym *hk_java_ndk_table(void) { return k_java_ndk; }
