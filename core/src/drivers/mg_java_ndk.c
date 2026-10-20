/*
 * The NDK's functions that are about the framework's Java objects, for the
 * native libraries of a Java app. The rest of the NDK is in ndk/ and needs no
 * Java; these need the Java driver's framework under them, and answer "bad
 * parameter" for anything that is not one of its objects.
 *
 * libjnigraphics: AndroidBitmap_getInfo, AndroidBitmap_lockPixels,
 * AndroidBitmap_unlockPixels on an android.graphics.Bitmap. The Bitmap's
 * pixels are the core's own memory (mg_java_gfx.c), kept as B, G, R, A,
 * premultiplied; native code is promised ANDROID_BITMAP_FORMAT_RGBA_8888, so
 * a lock turns the memory to that order where it lies and the unlock turns
 * it back and takes the bitmap as changed (mg_java_gfx_lock_rgba). Every
 * Bitmap is 32 bits a pixel here whatever its Config says, and that is the
 * format reported: code that asks for the format first, as the NDK's
 * documentation has it do, is told the truth.
 *
 * libandroid: ANativeWindow_fromSurface on the framework's Surface of a
 * SurfaceView (its megletView) or of a SurfaceTexture (its megletTexture): a
 * window of the view's surface or of the texture, below. A Surface a driver
 * made carries its window as it always did.
 */
#include "../bionic/mg_bionic.h"
#include "../dvm/mg_dvm_internal.h"
#include "../ndk/mg_ndk.h"
#include "mg_java_gfx.h"
#include "mg_java_gl.h"

enum { G_BITMAP_OK = 0, G_BITMAP_BAD_PARAMETER = -1, G_BITMAP_JNI_EXCEPTION = -2 };
enum { G_BITMAP_FORMAT_RGBA_8888 = 1 };

/* AndroidBitmapInfo. */
typedef struct g_bitmap_info {
    u32 width, height, stride;
    i32 format;
    u32 flags;                         /* 0: premultiplied alpha, no hardware buffer */
} g_bitmap_info;

/* The core's bitmap of an android.graphics.Bitmap: its field mNative; 0 for anything else, and for a recycled one. */
static i64 bitmap_of(mg_jobj *bitmap)
{
    mg_jvalue v;
    v.j = 0;
    if (!bitmap || !bitmap->dvm || !mg_dvm_instance_of(bitmap, "android/graphics/Bitmap")) return 0;
    if (!mg_dvm_get_field(bitmap, "mNative", &v)) mg_dvm_clear_pending();
    return v.j;
}

static int g_bitmap_get_info(void *env, mg_jobj *bitmap, g_bitmap_info *info)
{
    mg_java_gfx_bitmap b;
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (!bm || !mg_java_gfx_bitmap_info(bm, &b)) return G_BITMAP_BAD_PARAMETER;
    if (info) {
        info->width = (u32)b.width;
        info->height = (u32)b.height;
        info->stride = (u32)b.stride;
        info->format = G_BITMAP_FORMAT_RGBA_8888;
        info->flags = 0;
    }
    return G_BITMAP_OK;
}

static int g_bitmap_lock(void *env, mg_jobj *bitmap, void **address)
{
    mg_java_gfx_bitmap b;
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (address) *address = NULL;
    if (!bm) return G_BITMAP_BAD_PARAMETER;
    if (!mg_java_gfx_lock_rgba(bm, &b)) return G_BITMAP_JNI_EXCEPTION;
    if (address) *address = b.pixels;
    return G_BITMAP_OK;
}

static int g_bitmap_unlock(void *env, mg_jobj *bitmap)
{
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (!bm) return G_BITMAP_BAD_PARAMETER;
    mg_java_gfx_unlock_rgba(bm);
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
 * Drawn on with EGL (ndk/mg_egl.c's eglCreateWindowSurface), the window's EGL surface is mg_java_gl.c's: a pbuffer under the
 * view's hole, composed into the window as the frames of an EGL surface made from Java on the view are. Drawn on in software
 * (ANativeWindow_lock, unlockAndPost), the buffer is memory of the window's own, kept from one frame to the next as Android's
 * is in effect (it copies the last frame back); a post turns it into one of two Bitmaps in turn, B, G, R, A, opaque, and hands
 * that to the view as a whole frame (SurfaceView.megletPostBitmap, which the camera's preview uses too), which the view draws in
 * its place scaled to it. Posts are paced at sixty a second, as a swap is.
 *
 * A SurfaceTexture's Surface (a TextureView's, or one an app made) has a window of the texture: its size the texture's default
 * buffer size, its format RGBA_8888, valid until the texture is released. Its frames go to the texture through the framework's
 * way in for a native producer's frame (SurfaceTexture.megletPostRgba): an EGL surface on it is a pbuffer on no view whose frame
 * is read back at each swap, R, G, B, A, and handed over the right way up; a software buffer is handed over as R, G, B, A.
 */

#define MAX_WINDOWS 16
#define NW_GONE (-19)                  /* status_t NO_INIT */
#define NW_BAD_VALUE (-22)
#define NW_INVALID_OPERATION (-38)
enum { FORMAT_RGBA_8888 = 1, FORMAT_RGBX_8888 = 2, FORMAT_RGB_565 = 4 };

typedef struct view_window {
    mg_nwindow base;                   /* first: what native code holds */
    mg_jobj *view;                     /* the SurfaceView (a root of the collector's), or NULL for a SurfaceTexture's window */
    mg_jobj *texture;                  /* the SurfaceTexture (a root); NULL for a SurfaceView's; both NULL: a free slot */
    volatile i32 valid;                /* until the view's surface is destroyed (a texture's: released) */
    volatile i32 egl;                  /* EGL window surfaces on it now */
    volatile i32 locked;
    bool drawn_said;
    u8 *pixels;                        /* the software buffer, rows top first */
    size_t pixels_size;
    i32 bw, bh, bformat;               /* as it was last locked */
    mg_jobj *bitmaps[2];               /* roots: the posted buffer goes to the view in these, in turn */
    int next;
    u8 *rgba;                          /* a texture's buffer turned to R, G, B, A for it, when it is not that already */
    size_t rgba_size;
    u64 due;                           /* when the next post may return */
} view_window;

/* An EGL window surface on such a window: mg_java_gl.c's, and the window it is on. */
typedef struct view_surface {
    view_window *w;
    void *gl;
} view_surface;

static view_window g_windows[MAX_WINDOWS];
static mg_lock g_windows_lock = MG_LOCK_INIT;
static bool g_windows_rooted;

/* The fields of the overlay's SurfaceView the windows read, by their slots (any thread may ask a window its size). */
static struct {
    volatile i32 state;                /* 0 not looked for, 1 found, 2 not there */
    u32 created, width, height, format;
} SV;

/* And the framework's SurfaceTexture's: its default buffer size and whether it is released. */
static struct {
    volatile i32 state;
    u32 width, height, released;
} ST;

static bool surface_texture_fields(void)
{
    if (!ST.state) {
        mg_dvm_class *c = mg_dvm_find_class("android/graphics/SurfaceTexture");
        mg_dvm_field *width = c ? mg_dvm_find_field(c, "mW", false) : NULL, *height = c ? mg_dvm_find_field(c, "mH", false) : NULL;
        mg_dvm_field *released = c ? mg_dvm_find_field(c, "mReleased", false) : NULL;
        bool ok = width && height && released;
        if (!c) mg_dvm_clear_pending();
        if (ok) {
            ST.width = width->slot;
            ST.height = height->slot;
            ST.released = released->slot;
        } else {
            mg_warn("java: the framework's SurfaceTexture has no mW, mH or mReleased: native code is given no window of one");
        }
        mg_atomic_xchg(&ST.state, ok ? 1 : 2);
    }
    return ST.state == 1;
}

/* What a window is of: its SurfaceView, or its SurfaceTexture. */
static mg_jobj *owner_of(const view_window *w) { return w->view ? w->view : w->texture; }

static bool surface_view_fields(void)
{
    if (!SV.state) {
        mg_dvm_class *c = mg_dvm_find_class("android/view/SurfaceView");
        mg_dvm_field *created = c ? mg_dvm_find_field(c, "mCreated", false) : NULL, *width = c ? mg_dvm_find_field(c, "mSurfaceW", false) : NULL;
        mg_dvm_field *height = c ? mg_dvm_find_field(c, "mSurfaceH", false) : NULL, *format = c ? mg_dvm_find_field(c, "mFormat", false) : NULL;
        bool ok = created && width && height && format;
        if (!c) mg_dvm_clear_pending();
        if (ok) {
            SV.created = created->slot;
            SV.width = width->slot;
            SV.height = height->slot;
            SV.format = format->slot;
        } else {
            mg_warn("java: the framework's SurfaceView has no mCreated, mSurfaceW, mSurfaceH or mFormat: native code is given no window of one");
        }
        mg_atomic_xchg(&SV.state, ok ? 1 : 2);
    }
    return SV.state == 1;
}

static void vw_size(const mg_nwindow *b, int *width, int *height)
{
    const view_window *w = (const view_window *)b;
    mg_jvalue *slots = mg_dvm_slots(owner_of(w));
    i32 sw = w->view ? slots[SV.width].i : slots[ST.width].i, sh = w->view ? slots[SV.height].i : slots[ST.height].i;
    if (sw > 0 && sh > 0) {
        *width = sw;
        *height = sh;
    }
}

/* (A texture's window goes with the texture's release, which is for good.) */
static bool vw_valid(const mg_nwindow *b)
{
    const view_window *w = (const view_window *)b;
    return w->valid != 0 && (!w->texture || !mg_dvm_slots(w->texture)[ST.released].z);
}

/* A frame for a texture: w x h, R, G, B, A, rows bottom first when `flip` (as OpenGL reads them), handed to it. */
static void post_rgba(mg_jobj *texture, const void *rgba, int width, int height, bool flip)
{
    mg_jvalue args[4];
    memset(args, 0, sizeof(args));
    args[0].j = (i64)(mg_addr)rgba;
    args[1].i = width;
    args[2].i = height;
    args[3].z = flip;
    if (!mg_dvm_call_virtual(texture, "megletPostRgba", "(JIIZ)V", args, NULL)) mg_dvm_clear_pending();
}

static void *vw_egl_create(mg_nwindow *b, void *const *configs, int count)
{
    view_window *w = (view_window *)b;
    view_surface *s;
    void *gl;
    int width, height;
    if (w->locked) {
        mg_note_once("java: an EGL surface was asked for on a window locked for drawing in software: refused, as on Android");
        return NULL;
    }
    mg_nwindow_buffer_size(b, &width, &height);
    gl = mg_java_gl_window_surface(w->view, width, height, configs, count);
    if (!gl) return NULL;
    s = (view_surface *)mg_alloc(sizeof(*s));
    s->w = w;
    s->gl = gl;
    mg_atomic_add(&w->egl, 1);
    return s;
}

static void *vw_egl_handle(void *p, int *width, int *height) { return mg_java_gl_window_handle(((view_surface *)p)->gl, width, height); }

static bool vw_egl_swap(void *p)
{
    view_surface *s = (view_surface *)p;
    int width, height;
    if (!vw_valid(&s->w->base)) {
        mg_note_once("java: native code swapped an EGL surface whose window has gone (its SurfaceView's surface destroyed, or its SurfaceTexture released): "
                     "EGL_BAD_NATIVE_WINDOW, as on Android");
        return false;
    }
    mg_nwindow_buffer_size(&s->w->base, &width, &height);
    mg_java_gl_window_swap(s->gl, width, height);
    if (s->w->texture) {
        /* (The frame read back is kept apart from the surface, which the swap may have replaced by one of a new size.) */
        int fw, fh;
        const void *rgba = mg_java_gl_window_pixels(s->gl, &fw, &fh);
        if (rgba) post_rgba(s->w->texture, rgba, fw, fh, true);
    }
    return true;
}

static void vw_egl_destroy(void *p)
{
    view_surface *s = (view_surface *)p;
    mg_java_gl_window_destroy(s->gl);
    mg_atomic_add(&s->w->egl, -1);
    free(s);
}

/* ANativeWindow_Buffer, as the NDK lays it out. */
typedef struct anw_buffer {
    i32 width, height, stride, format;
    void *bits;
    u32 reserved[6];
} anw_buffer;

static int vw_lock(mg_nwindow *b, void *buffer, i32 *dirty)
{
    view_window *w = (view_window *)b;
    anw_buffer *out = (anw_buffer *)buffer;
    int width, height, format, bpp;
    size_t need;
    bool kept;
    if (!out) return NW_BAD_VALUE;
    if (!vw_valid(b)) return NW_GONE;
    if (w->egl) {
        mg_note_once("java: ANativeWindow_lock on a window with an EGL surface on it: refused, as on Android");
        return NW_BAD_VALUE;
    }
    if (mg_atomic_xchg(&w->locked, 1)) return NW_INVALID_OPERATION;
    mg_nwindow_buffer_size(b, &width, &height);
    format = b->format == FORMAT_RGB_565 ? FORMAT_RGB_565 : b->format == FORMAT_RGBX_8888 ? FORMAT_RGBX_8888 : FORMAT_RGBA_8888;
    bpp = format == FORMAT_RGB_565 ? 2 : 4;
    need = (size_t)width * (size_t)height * (size_t)bpp;
    kept = w->pixels && w->bw == width && w->bh == height && w->bformat == format;
    if (!kept) {
        if (w->pixels_size < need) {
            free(w->pixels);
            w->pixels = (u8 *)mg_alloc(need);
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
        mg_info("java: native code draws a window of %s in software (ANativeWindow_lock): %dx%d, format %d", mg_dvm_class_name(owner_of(w)), width, height, format);
    }
    return 0;
}

/* A Bitmap of the framework's, ARGB_8888; NULL when none could be made. */
static mg_jobj *new_bitmap(int width, int height)
{
    mg_jvalue config, args[3], ret;
    config.j = 0;
    if (!mg_dvm_get_static("android/graphics/Bitmap$Config", "ARGB_8888", &config) || !config.l) {
        mg_dvm_clear_pending();
        return NULL;
    }
    memset(args, 0, sizeof(args));
    args[0].i = width;
    args[1].i = height;
    args[2].l = config.l;
    ret.j = 0;
    if (!mg_dvm_call_static("android/graphics/Bitmap", "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;", args, &ret)) {
        mg_dvm_clear_pending();
        return NULL;
    }
    return ret.l;
}

/* The core's bitmap of a Bitmap of this width and height in the slot, the slot given a new one when it has none of that size. */
static i64 post_bitmap(mg_jobj **slot, int width, int height, mg_java_gfx_bitmap *out)
{
    int round;
    for (round = 0; round < 2; round++) {
        mg_jvalue v;
        v.j = 0;
        if (*slot && !mg_dvm_get_field(*slot, "mNative", &v)) mg_dvm_clear_pending();
        if (v.j && mg_java_gfx_bitmap_info(v.j, out) && out->width == width && out->height == height) return v.j;
        if (!round) *slot = new_bitmap(width, height);
    }
    return 0;
}

/* A texture's buffer, as R, G, B, A (opaque but for an RGBA_8888 one), handed to the texture. */
static void post_texture(view_window *w)
{
    size_t n = (size_t)w->bw * (size_t)w->bh, i;
    const u8 *rgba = w->pixels;
    if (w->bformat != FORMAT_RGBA_8888) {
        if (w->rgba_size < n * 4) {
            free(w->rgba);
            w->rgba = (u8 *)mg_alloc(n * 4);
            w->rgba_size = n * 4;
        }
        for (i = 0; i < n; i++) {
            u8 *d = w->rgba + i * 4;
            if (w->bformat == FORMAT_RGB_565) {
                u32 v = ((const u16 *)(const void *)w->pixels)[i], r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31;
                d[0] = (u8)((r << 3) | (r >> 2));
                d[1] = (u8)((g << 2) | (g >> 4));
                d[2] = (u8)((bl << 3) | (bl >> 2));
            } else {
                memcpy(d, w->pixels + i * 4, 3);
            }
            d[3] = 255;
        }
        rgba = w->rgba;
    }
    post_rgba(w->texture, rgba, w->bw, w->bh, false);
}

/* The buffer, as a whole frame of the view's: into the next Bitmap, B, G, R, A and opaque, which the view is handed. */
static void post(view_window *w)
{
    mg_java_gfx_bitmap b;
    mg_jvalue args[1], ret;
    i64 bm = post_bitmap(&w->bitmaps[w->next], w->bw, w->bh, &b);
    i32 x, y;
    if (!bm) {
        mg_note_once("java: no Bitmap for a window's software buffer: its frames are not shown");
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
    mg_java_gfx_bitmap_changed(bm);
    memset(args, 0, sizeof(args));
    args[0].l = w->bitmaps[w->next];
    ret.j = 0;
    if (!mg_dvm_call_virtual(w->view, "megletPostBitmap", "(Landroid/graphics/Bitmap;)Z", args, &ret)) mg_dvm_clear_pending();
    /* The view has this one as its front buffer now: the next post goes into the other. */
    else if (ret.z) w->next ^= 1;
}

static int vw_unlock(mg_nwindow *b)
{
    view_window *w = (view_window *)b;
    if (!w->locked) return NW_BAD_VALUE;
    if (vw_valid(b)) {
        const u64 period = 16666667ull;
        u64 now;
        if (w->texture) post_texture(w);
        else post(w);
        /* No sooner than a sixtieth of a second after the post before, as a queued buffer is taken on Android. */
        now = mg_time_monotonic_ns();
        if (!w->due || now > w->due + 100000000ull) w->due = now;
        w->due += period;
        if (w->due > now) mg_sleep_ns(w->due - now);
    }
    mg_atomic_xchg(&w->locked, 0);
    return vw_valid(b) ? 0 : NW_GONE;
}

static void vw_gone(mg_nwindow *b)
{
    view_window *w = (view_window *)b;
    mg_lock_acquire(&g_windows_lock);
    free(w->pixels);
    free(w->rgba);
    /* (The slot is free with its view gone; the collector's roots in it now point at nothing.) */
    memset(w, 0, sizeof(*w));
    mg_lock_release(&g_windows_lock);
}

static const mg_nwindow_ops k_view_window = { vw_size, vw_valid, vw_lock, vw_unlock, vw_egl_create, vw_egl_handle, vw_egl_swap, vw_egl_destroy, vw_gone };

/*
 * The window of a SurfaceView's surface now, or of a SurfaceTexture (one of the two is given), a reference of the caller's; NULL
 * while the view has no surface or the texture is released (Android's Surface is not valid then).
 */
static mg_nwindow *window_of(mg_jobj *view, mg_jobj *texture)
{
    view_window *w = NULL, *free_slot = NULL;
    mg_jobj *owner = view ? view : texture;
    mg_jvalue *slots;
    i32 format;
    int i;
    if (view ? !surface_view_fields() : !surface_texture_fields()) return NULL;
    slots = mg_dvm_slots(owner);
    if (view ? !slots[SV.created].z : slots[ST.released].z) {
        mg_info("java: ANativeWindow_fromSurface: %s %s: NULL", mg_dvm_class_name(owner), view ? "has no surface now" : "is released");
        return NULL;
    }
    mg_lock_acquire(&g_windows_lock);
    if (!g_windows_rooted) {
        for (i = 0; i < MAX_WINDOWS; i++) {
            mg_dvm_add_root(&g_windows[i].view);
            mg_dvm_add_root(&g_windows[i].texture);
            mg_dvm_add_root(&g_windows[i].bitmaps[0]);
            mg_dvm_add_root(&g_windows[i].bitmaps[1]);
        }
        g_windows_rooted = true;
    }
    for (i = 0; i < MAX_WINDOWS && !w; i++) {
        if (owner_of(&g_windows[i]) == owner && g_windows[i].valid) w = &g_windows[i];
        else if (!owner_of(&g_windows[i]) && !free_slot) free_slot = &g_windows[i];
    }
    if (w) {
        mg_nwindow_acquire(&w->base);
        mg_lock_release(&g_windows_lock);
        return &w->base;
    }
    if (!free_slot) {
        mg_lock_release(&g_windows_lock);
        mg_warn("java: ANativeWindow_fromSurface: native code holds %d windows already: NULL", MAX_WINDOWS);
        return NULL;
    }
    w = free_slot;
    memset(w, 0, sizeof(*w));
    w->base.refs = 1;
    w->base.ops = &k_view_window;
    if (view) {
        /* PixelFormat: OPAQUE (-1) is RGBX_8888 as Android's compositor makes it, TRANSLUCENT and TRANSPARENT RGBA_8888, the rest themselves. */
        format = slots[SV.format].i;
        w->base.format = format > 0 ? format : format == -1 ? FORMAT_RGBX_8888 : FORMAT_RGBA_8888;
        w->base.width = slots[SV.width].i;
        w->base.height = slots[SV.height].i;
    } else {
        w->base.format = FORMAT_RGBA_8888;
        w->base.width = slots[ST.width].i > 0 ? slots[ST.width].i : 1;
        w->base.height = slots[ST.height].i > 0 ? slots[ST.height].i : 1;
    }
    w->view = view;
    w->texture = texture;
    w->valid = 1;
    mg_lock_release(&g_windows_lock);
    mg_info("java: ANativeWindow_fromSurface: a window of %s%s, %dx%d, format %d", mg_dvm_class_name(owner), view ? "'s surface" : "", w->base.width,
            w->base.height, w->base.format);
    return &w->base;
}

void mg_java_ndk_view_gone(mg_jobj *view)
{
    int i, gone = 0;
    mg_lock_acquire(&g_windows_lock);
    for (i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].view != view || !g_windows[i].valid) continue;
        mg_atomic_xchg(&g_windows[i].valid, 0);
        gone++;
    }
    mg_lock_release(&g_windows_lock);
    if (gone) mg_info("java: the surface of %s has gone: native code's window of it is no longer valid", mg_dvm_class_name(view));
}

static mg_nwindow *g_window_from_surface(void *env, mg_jobj *surface)
{
    mg_jvalue v;
    (void)env;
    if (!surface) return NULL;
    if (!surface->dvm) {
        /* A Surface a driver made carries its window (drivers/mg_unity.c); the caller owns a reference, as on Android. */
        mg_nwindow *w = (mg_nwindow *)surface->native;
        if (w) mg_nwindow_acquire(w);
        return w;
    }
    v.j = 0;
    if (!mg_dvm_get_field(surface, "megletView", &v)) mg_dvm_clear_pending();
    if (v.l) return window_of(v.l, NULL);
    v.j = 0;
    if (!mg_dvm_get_field(surface, "megletTexture", &v)) mg_dvm_clear_pending();
    if (v.l) return window_of(NULL, v.l);
    mg_note_once("java: ANativeWindow_fromSurface on a Surface of neither a SurfaceView nor a SurfaceTexture: no window of it here, NULL");
    return NULL;
}

static const mg_sym k_java_ndk[] = {
    MG_WRAP("AndroidBitmap_getInfo", g_bitmap_get_info), MG_WRAP("AndroidBitmap_lockPixels", g_bitmap_lock),
    MG_WRAP("AndroidBitmap_unlockPixels", g_bitmap_unlock),
    MG_WRAP("ANativeWindow_fromSurface", g_window_from_surface),
    MG_END
};

const mg_sym *mg_java_ndk_table(void) { return k_java_ndk; }
