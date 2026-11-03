/*
 * What Android's platform gives the WebView besides the framework's Java: the natives of android.webkit.WebViewLibraryLoader,
 * which are libwebviewchromium_loader.so's on Android, and those of libwebviewchromium_plat_support.so, which the WebView's
 * glue loads by name and whose natives its classes declare (docs/JAVA.md, "The WebView").
 *
 * The loader. On Android the zygote reserves address space for the WebView's library, an isolated process loads it once to
 * write its relocated RELRO, and each app loads it into its reservation with that RELRO, by android_dlopen_ext with
 * ANDROID_DLEXT_RESERVED_ADDRESS and USE_RELRO, in the linker namespace of the WebView's class loader. Here there is one
 * process, with no RELRO to share, and so no range to share it in: nativeReserveAddressSpace takes nothing and says it
 * did, and nativeLoadWithRelroFile loads the library wherever its own size fits, as dlopen would, and runs its
 * constructors. (It was loaded into the 130 MB the WebView asks for, as android_dlopen_ext does with
 * RESERVED_ADDRESS_HINT; the library and the patcher's code take 72 MB, and the other 58 stayed reserved for nothing,
 * out of a 2 GB address space where Spotify's WebView ended out of memory wanting 126 MB in one piece.) The
 * library is found as the class loader's namespace finds it: the loader's findLibrary names "<apk>!/lib/<abi>/<name>", an
 * APK whose libraries are used from it as they are (the WebView's are stored, not extracted), and that APK is given to the
 * linker to read libraries from. JNI_OnLoad is not run here: as on Android it runs when the WebView's own LibraryLoader calls
 * System.loadLibrary("webviewchromium"), which finds the library loaded (dvm/mg_dvm_jvm.c, load_native).
 *
 * The platform support. libwebviewchromium_plat_support.so gives the WebView the tables of functions it draws through on a
 * canvas: GraphicsUtils.nativeGetDrawSWFunctionTable (Skia's canvas under an android.graphics.Canvas, which the WebView
 * draws into directly when it can), nativeGetDrawGLFunctionTable and DrawFunctor.nativeGetFunctionTable (the GL functor and
 * draw functor of a hardware-accelerated canvas), and DrawGLFunctor's functor. The draw functor's table is there where the
 * shell keeps pictures on the graphics processor (the draw functor, below): a WebView's canvas in a view list then says it is
 * hardware-accelerated, and Chromium draws its page with OpenGL ES on the WebView's render thread here. The GL functor's
 * table (Android 9's and before) is null: Chromium asks for it only where the draw functor's is not there. The software
 * table is there too (sw_access_pixels, below), for a canvas that is not hardware-accelerated: a canvas of a Bitmap of the
 * core's hands Chromium its pixels, which it draws into directly; any other canvas has none, and Chromium draws a page into a
 * Bitmap of its own and draws that bitmap into the view's canvas (JavaBrowserViewRendererHelper, its "auxiliary bitmap").
 * The library itself is a name the linker knows as a system library (bionic/mg_bionic.c), so that System.loadLibrary finds
 * and "loads" it.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bionic/mg_bionic.h"
#include "../dvm/mg_dvm_internal.h"
#include "../ld/mg_ld.h"
#include "../mg_core.h"
#include "../ndk/mg_egl.h"
#include "mg_java.h"
#include "mg_java_gfx.h"
#include "mg_java_gl.h"

#define N(name) static void name(mg_jcall *c)

/* What nativeReserveAddressSpace was asked for, which is not taken (see the top). */
static i64 g_asked;

/* nativeReserveAddressSpace(long size): true, nothing reserved. */
N(Loader_reserve)
{
    g_asked = c->args[0].j;
    c->ret.z = g_asked > 0;
}

/*
 * "<apk>!/lib/<abi>/<name>": the APK's guest path into apk, and true. A plain path (a library that is a file of its own) or
 * anything else: false.
 */
static bool apk_of(const char *found, char *apk, size_t n)
{
    const char *bang = strstr(found, "!/");
    size_t k;
    if (!bang) return false;
    k = (size_t)(bang - found);
    if (k + 1 > n) return false;
    memcpy(apk, found, k);
    apk[k] = 0;
    return true;
}

/*
 * nativeLoadWithRelroFile(String lib, String relro, ClassLoader loader): WebViewFactory's LIBLOAD_* codes. 0 loaded; 6 the
 * library could not be found or loaded (LIBLOAD_FAILED_TO_LOAD_LIBRARY); 10 the class loader names no library of that name
 * (LIBLOAD_FAILED_TO_FIND_NAMESPACE).
 */
N(Loader_load)
{
    const char *lib = mg_dvm_string_utf8(c->args[0].l);
    mg_jobj *loader = c->args[2].l;
    char name[200], apk[400];
    mg_jvalue arg, found;
    const char *path;
    char *host;
    mg_lib *l;
    size_t n;
    int scope, previous;
    bool ok;
    c->ret.i = 6;
    if (!lib || !loader) return;
    /* findLibrary takes the name System.loadLibrary would: "webviewchromium" for libwebviewchromium.so. */
    n = strlen(lib);
    if (!strncmp(lib, "lib", 3) && n > 6 && !strcmp(lib + n - 3, ".so")) snprintf(name, sizeof(name), "%.*s", (int)(n - 6), lib + 3);
    else mg_strlcpy(name, lib, sizeof(name));
    arg.j = 0;
    arg.l = mg_dvm_new_string(name);
    found.j = 0;
    if (!mg_dvm_call_virtual(loader, "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", &arg, &found) || !found.l) {
        mg_dvm_clear_pending();
        mg_warn("webview: the WebView's class loader finds no library %s", lib);
        c->ret.i = 10;
        return;
    }
    path = mg_dvm_string_utf8(found.l);
    if (!path || !apk_of(path, apk, sizeof(apk))) {
        mg_warn("webview: the WebView's library is %s, which is not in an APK; only an APK's are loaded so", path ? path : "nothing");
        return;
    }
    /*
     * In the ELF scope of the WebView's class loader (a name space of its own, android/webkit/WebViewFactory.java), whose APK
     * that scope reads its libraries from: Chromium's System.loadLibrary("webviewchromium") from the WebView's classes then
     * finds this library there, and its JNI_OnLoad finds the WebView's classes, not an app's of the same names.
     */
    scope = mg_dvm_loader_scope(loader);
    previous = mg_ld_set_scope(scope);
    l = mg_ld_find_lib(lib);
    if (!l) {
        host = scope ? NULL : mg_fs_host_path(apk);
        if (!scope && (!host || !mg_ld_add_apk(host))) {
            mg_warn("webview: %s cannot be read for its libraries", apk);
            free(host);
            mg_ld_set_scope(previous);
            return;
        }
        free(host);
        mg_info("webview: loading %s from %s%s", lib, apk, scope ? ", in the WebView's own scope" : "");
        l = mg_ld_load(lib);
        if (!l) {
            mg_warn("webview: %s did not load", lib);
            mg_ld_set_scope(previous);
            return;
        }
    }
    ok = mg_ld_init(l);
    mg_ld_set_scope(previous);
    if (!ok) {
        mg_warn("webview: the constructors of %s did not run", lib);
        return;
    }
    mg_info("webview: %s loaded at %p, %u MB of address space with the patcher's code (of the %u MB the WebView asked to reserve, none is)",
            lib, mg_ld_lib_base(l), (unsigned)(mg_ld_lib_extent(l) >> 20), (unsigned)(g_asked >> 20));
    c->ret.i = 0;
}

/*
 * The software one: AwDrawSWFunctionTable (Chromium's android_webview/public/browser/draw_sw.h), whose access_pixels hands
 * the WebView the pixels under a Java Canvas as Skia's SkCanvasState (version 1, a stable ABI of Skia's own:
 * SkCanvasStateUtils, which Chromium makes an SkCanvas of), inside an AwPixelInfo of version 3. Here a canvas of a Bitmap of
 * the core's has its pixels (mg_java_gfx_canvas_raster): one raster layer, R, G, B, A premultiplied (Skia's N32 on Android),
 * with the canvas's matrix and its clip's box. Any other canvas (one that only records) gets null, and Chromium goes on as
 * before, drawing into a Bitmap of its own.
 *
 * Why. With no table Chromium drew each frame into a new Bitmap of the WebView's whole size (its "auxiliary bitmap": 12 to 15
 * MB at 1440 pixels wide, ten a second), which only the collector took back. Google's sign-in page in Play Store filled the
 * process's two gigabytes with them: Bitmaps could no longer be made ("Error allocating bitmap"), frames went undrawn, and the
 * app ended. android/webkit/WebView.java draws every frame through one Bitmap of what the window shows of the view, which
 * Chromium now draws into.
 */
typedef struct sk_clip_rect { i32 left, top, right, bottom; } sk_clip_rect;
typedef struct sk_mc_state {
    float matrix[9];
    i32 clip_rect_count;
    sk_clip_rect *clip_rects;
} sk_mc_state;
typedef struct sk_layer_state {
    i32 type;                          /* 1: raster */
    i32 x, y, width, height;
    sk_mc_state mc;
    union {
        struct {
            i32 config;                /* 2: ARGB_8888, Skia's N32 */
            u64 row_bytes;
            void *pixels;
        } raster;
        struct { i32 texture_id; } gpu;
    } u;
} sk_layer_state;
typedef struct sk_canvas_state_v1 {
    i32 version;                       /* 1 */
    i32 width, height, alignment_padding;
    sk_mc_state mc;
    i32 layer_count;
    sk_layer_state *layers;
    void *original_canvas;             /* Skia's own pointer: none here */
} sk_canvas_state_v1;
typedef struct aw_pixel_info {
    int version;                       /* 3 */
    sk_canvas_state_v1 *state;
} aw_pixel_info;
#if MG_HOST_ARM32
/* The layout Chromium's Skia reads, on 32-bit ARM. */
_Static_assert(sizeof(sk_layer_state) == 88 && offsetof(sk_layer_state, u) == 64, "SkCanvasLayerState's layout");
_Static_assert(sizeof(sk_canvas_state_v1) == 72 && offsetof(sk_canvas_state_v1, layers) == 64, "SkCanvasState_v1's layout");
#endif

/* One access_pixels's: what it handed over, kept together, and the canvas whose bitmap stays locked until release_pixels. */
typedef struct sw_pixels {
    aw_pixel_info info;                /* first: what Chromium gives back is a pointer to it */
    sk_canvas_state_v1 state;
    sk_layer_state layer;
    sk_clip_rect clip[2];
    i64 canvas;
} sw_pixels;

static aw_pixel_info *sw_access_pixels(void *env, mg_jobj *java_canvas)
{
    static volatile i32 said, said_none;
    mg_java_gfx_raster r;
    sw_pixels *p;
    mg_jvalue v;
    (void)env;
    if (!java_canvas || !java_canvas->dvm || !mg_dvm_instance_of(java_canvas, "android/graphics/Canvas")) return NULL;
    v.j = 0;
    if (!mg_dvm_get_field(java_canvas, "mNative", &v)) {
        mg_dvm_clear_pending();
        return NULL;
    }
    if (!v.j || !mg_java_gfx_canvas_raster(v.j, &r)) {
        if (mg_atomic_add(&said_none, 1) == 1) mg_info("webview: a canvas with no pixels of the core's: Chromium draws into a bitmap of its own");
        return NULL;
    }
    p = (sw_pixels *)calloc(1, sizeof(*p));
    if (!p) {
        mg_java_gfx_canvas_raster_end(v.j);
        return NULL;
    }
    p->canvas = v.j;
    p->clip[0].left = r.clip[0];
    p->clip[0].top = r.clip[1];
    p->clip[0].right = r.clip[2];
    p->clip[0].bottom = r.clip[3];
    p->clip[1] = p->clip[0];
    p->layer.type = 1;
    p->layer.width = r.width;
    p->layer.height = r.height;
    memcpy(p->layer.mc.matrix, r.matrix, sizeof(r.matrix));
    p->layer.mc.clip_rect_count = 1;
    p->layer.mc.clip_rects = &p->clip[1];
    p->layer.u.raster.config = 2;
    p->layer.u.raster.row_bytes = (u64)r.stride;
    p->layer.u.raster.pixels = r.pixels;
    p->state.version = 1;
    p->state.width = r.width;
    p->state.height = r.height;
    memcpy(p->state.mc.matrix, r.matrix, sizeof(r.matrix));
    p->state.mc.clip_rect_count = 1;
    p->state.mc.clip_rects = &p->clip[0];
    p->state.layer_count = 1;
    p->state.layers = &p->layer;
    p->info.version = 3;
    p->info.state = &p->state;
    if (mg_atomic_add(&said, 1) == 1)
        mg_info("webview: Chromium draws straight into a canvas's pixels (%dx%d, clip %d,%d to %d,%d)", (int)r.width, (int)r.height,
                (int)r.clip[0], (int)r.clip[1], (int)r.clip[2], (int)r.clip[3]);
    return &p->info;
}

static void sw_release_pixels(aw_pixel_info *info)
{
    sw_pixels *p = (sw_pixels *)info;
    if (!p) return;
    mg_java_gfx_canvas_raster_end(p->canvas);
    free(p);
}

/*
 * AwDrawSWFunctionTable (android_webview/public/browser/draw_sw.h): its version (kAwDrawSWFunctionTableVersion, 1) first,
 * then the two functions. Without the version Chromium took release_pixels for access_pixels and handed it the JNIEnv to
 * free: the heap of Play Store's process was corrupted at the first frame of Google's sign-in page.
 */
typedef struct aw_draw_sw_table {
    i32 version;
    void *access_pixels, *release_pixels;
} aw_draw_sw_table;
_Static_assert(offsetof(aw_draw_sw_table, access_pixels) == sizeof(void *) && offsetof(aw_draw_sw_table, release_pixels) == 2 * sizeof(void *),
               "AwDrawSWFunctionTable's layout");
static const aw_draw_sw_table k_sw_table = { 1, (void *)sw_access_pixels, (void *)sw_release_pixels };

N(SW_table)
{
    mg_info("webview: the platform's software table is handed over");
    c->ret.j = (i64)(mg_addr)&k_sw_table;
}

/*
 * The draw functor: AwDrawFn (Chromium's android_webview/public/browser/draw_fn.h, version 4), which Chromium draws a page
 * through on a canvas that is hardware-accelerated, as HWUI's WebViewFunctor has it drawn on Android. The table below is
 * Chromium's (DrawFunctor.nativeGetFunctionTable) where pictures can be kept on the graphics processor (the shell's
 * megler_gfx_host.texture_frame and draw_texture, EGL's shared pbuffer textures) and the flag no-webview-gpu is not set;
 * Chromium makes a functor of it for each WebView (create_functor_v3: its callbacks), and on a hardware-accelerated canvas
 * has the platform draw it (WebViewDelegate.drawWebViewFunctor). A WebView's canvas says it is hardware-accelerated where the
 * table was given (megler.WebFunctors.on; android/webkit/WebView.java): a view list's, the shell's on the graphics processor.
 *
 * The functor is drawn at once, on a thread of its own here (the WebView's "render thread"), as HWUI's render thread draws it,
 * the UI thread waiting as HWUI's does for a frame it cannot let go of before it is drawn: on_sync, then draw_gl, with a context
 * of OpenGL ES 3 current on a pbuffer of the part of the view the window shows, rounded up, of the WebView's own display
 * (ndk/mg_egl.c, webview_display: the one Chromium's own EGL calls are given), cleared, its viewport the pbuffer, its scissor
 * the part. The parameters are HWUI's for a functor drawn on a surface of that size: the clip the part, in the surface's pixels
 * from its top left; the transform the view's coordinates to those pixels (a translation by the part's corner); sRGB.
 * Chromium draws its frame into the pbuffer as into HWUI's surface, the right way up for OpenGL; its texture is then the
 * shell's picture of the functor (mg_java_gl_share_frame, the keyed mutex let go for the copy), which the view's list draws
 * where the part is (DL_TEXTURE, turned as a TextureView's frame is), nothing read back. Before, every frame of a page was
 * drawn by Chromium's software compositor into a bitmap and uploaded again to be drawn (sw_access_pixels above).
 *
 * A functor released (release_functor, Chromium's AwDrawFnImpl going) has its last callbacks on the render thread:
 * on_context_destroyed with the context current, if it was ever drawn, then on_destroyed; its pbuffer and picture go with it.
 * One context serves every functor, as HWUI's one context does. Every callback takes and returns only integers and pointers,
 * so Chromium's code (softfp) and this (the host's convention) call each other as they are.
 */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef i32 EGLint;

enum {
    AW_DRAW_FN_VERSION = 4,
    WV_FUNCTORS = 32,
    WV_ROUND = 64,                     /* a pbuffer's size, rounded up to this: a part that changes a little keeps its pbuffer */
    WV_PICTURES = 0x40000000,          /* the shell's ids of functors' pictures: above any a TextureView's texture is given */
    WV_WAIT_MS = 4000,                 /* the most the UI thread waits for a functor drawn */
};

typedef struct aw_sync_params {
    i32 version;
    bool apply_force_dark;
} aw_sync_params;

typedef struct aw_draw_gl_params {
    i32 version;
    i32 clip_left, clip_top, clip_right, clip_bottom;
    i32 width, height;
    bool deprecated_0;                 /* was is_layer */
    float transform[16];               /* column by column, as OpenGL's */
    float transfer_g, transfer_a, transfer_b, transfer_c, transfer_d, transfer_e, transfer_f;
    float to_xyz_d50[9];               /* row by row */
    i32 overlays_mode;                 /* 0: none */
    void *get_surface_control;
    void *merge_transaction;
} aw_draw_gl_params;

typedef struct aw_callbacks {
    void *on_sync, *on_context_destroyed, *on_destroyed, *draw_gl;
    void *init_vk, *draw_vk, *post_draw_vk;
    void *remove_overlays;             /* version 3 on */
} aw_callbacks;

typedef struct aw_draw_fn_table {
    i32 version;
    void *query_render_mode, *create_functor, *release_functor, *create_functor_v3, *report_rendering_threads;
} aw_draw_fn_table;

#if MG_HOST_ARM32
/* The layouts Chromium's code reads, on 32-bit ARM. */
_Static_assert(offsetof(aw_draw_gl_params, transform) == 32 && offsetof(aw_draw_gl_params, to_xyz_d50) == 124 &&
               offsetof(aw_draw_gl_params, overlays_mode) == 160 && sizeof(aw_draw_gl_params) == 172, "AwDrawFn_DrawGLParams's layout");
_Static_assert(sizeof(aw_callbacks) == 32 && sizeof(aw_draw_fn_table) == 24, "AwDrawFnFunctorCallbacks', AwDrawFnFunctionTable's layouts");
#endif

typedef struct wv_functor {
    i32 id;                            /* 0: a free slot */
    void *data;
    aw_callbacks cb;
    bool released;                     /* release_functor: its last callbacks are the render thread's to make */
    bool drawn;                        /* draw_gl has run: on_context_destroyed is owed at the end */
    EGLSurface surface;                /* its pbuffer, pw x ph */
    int pw, ph;
    mg_java_gl_share share;
    u32 draws;
    u64 draw_ns;
} wv_functor;

/* The functors, the render thread and what is asked of it, under g_wv_lock; one draw is asked for at a time (g_wv_serial). */
static mg_lock g_wv_lock = MG_LOCK_INIT, g_wv_serial = MG_LOCK_INIT;
static mg_cond g_wv_wake = MG_COND_INIT, g_wv_answered = MG_COND_INIT;
static struct {
    i32 started;                       /* 0 not yet, 1 the render thread runs, 2 it could not be started */
    i32 next_id;
    i32 releases;                      /* functors released whose last callbacks are owed */
    wv_functor f[WV_FUNCTORS];
    /* the draw asked for, and its answer */
    bool asked, done;
    i32 ask_id, ask_l, ask_t, ask_w, ask_h;
    i64 answer;
    volatile i32 stuck;                /* a draw outlasted WV_WAIT_MS: no more are asked for */
    /* the render thread's own */
    i32 egl;                           /* 0 not tried, 1 ready, 2 none */
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    int version;
    u32 (*choose)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLSurface (*create_pbuffer)(EGLDisplay, EGLConfig, const EGLint *);
    u32 (*destroy_surface)(EGLDisplay, EGLSurface);
    u32 (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLint (*get_error)(void);
    EGLContext (*current_context)(void);
    void (*viewport)(i32, i32, i32, i32), (*scissor)(i32, i32, i32, i32);
    void (*enable)(u32), (*disable)(u32), (*clear)(u32), (*bind_framebuffer)(u32, u32), (*color_mask)(u8, u8, u8, u8);
    void (*clear_color)(float, float, float, float);
} W;

/* Whether Chromium is given the table: pictures can be kept on the graphics processor, and no flag says otherwise. */
static bool wv_wanted(void)
{
    static i32 wanted = -1;
    if (wanted < 0) {
        bool on = !mg_flag("no-webview-gpu") && mg_java_gfx_textures() && mg_java_gl_shares_frames();
        wanted = on ? 1 : 0;
        mg_info("webview: %s", on ? "pages are drawn on the graphics processor, through the platform's draw functor"
                                  : "pages are drawn in software: no draw functor (the flag no-webview-gpu, or no pictures kept on the graphics processor)");
    }
    return wanted > 0;
}

static wv_functor *wv_find(i32 id)
{
    int i;
    for (i = 0; i < WV_FUNCTORS; i++)
        if (W.f[i].id == id && id) return &W.f[i];
    return NULL;
}

static bool wv_start(void);

static i32 wv_query_render_mode(void) { return 0; }   /* AW_DRAW_FN_RENDER_MODE_OPENGL_ES */

static i32 wv_create_functor_v3(void *data, i32 version, const aw_callbacks *cb)
{
    wv_functor *f = NULL;
    i32 id = 0;
    int i;
    if (!cb) return 0;
    mg_lock_acquire(&g_wv_lock);
    for (i = 0; i < WV_FUNCTORS && !f; i++)
        if (!W.f[i].id) f = &W.f[i];
    if (f) {
        memset(f, 0, sizeof(*f));
        id = ++W.next_id;
        if (id >= WV_PICTURES) id = W.next_id = 1;
        f->id = id;
        f->data = data;
        /* remove_overlays is only there from version 3 */
        memcpy(&f->cb, cb, version >= 3 ? sizeof(aw_callbacks) : offsetof(aw_callbacks, remove_overlays));
    }
    mg_lock_release(&g_wv_lock);
    if (!f) mg_warn("webview: more than %d draw functors at once: Chromium's next WebView gets none", WV_FUNCTORS);
    else mg_info("webview: draw functor %d made (callbacks of version %d)", (int)id, (int)version);
    return id;
}

static i32 wv_create_functor(void *data, const aw_callbacks *cb) { return wv_create_functor_v3(data, 2, cb); }

static void wv_release_functor(i32 id)
{
    wv_functor *f;
    void *data = NULL, *on_destroyed = NULL;
    bool thread = wv_start();
    mg_lock_acquire(&g_wv_lock);
    f = wv_find(id);
    if (f && !f->released) {
        if (thread) {
            f->released = true;
            W.releases++;
            mg_cond_signal(&g_wv_wake);
        } else {
            /* No render thread to make its last callback: made here (it was never drawn). */
            data = f->data;
            on_destroyed = f->cb.on_destroyed;
            memset(f, 0, sizeof(*f));
        }
    }
    mg_lock_release(&g_wv_lock);
    if (on_destroyed) MG_GFN(void (*)(i32, void *), on_destroyed)(id, data);
}

/* report_rendering_threads: for the performance hints Android gives the threads; none here. */
static void wv_report_threads(i32 id, const i32 *threads, u32 count) { (void)id; (void)threads; (void)count; }

static const aw_draw_fn_table k_draw_fn_table = {
    AW_DRAW_FN_VERSION, (void *)wv_query_render_mode, (void *)wv_create_functor, (void *)wv_release_functor, (void *)wv_create_functor_v3,
    (void *)wv_report_threads,
};

/* draw_gl's get_surface_control and merge_transaction, for overlays, which are never asked for (overlays_mode 0). */
static void *wv_no_surface_control(void) { return NULL; }
static void wv_no_transaction(void *transaction) { (void)transaction; }

/* The render thread's EGL: a context of the WebView's display, OpenGL ES 3 where it has it, and the configuration of its pbuffers. */
static bool wv_egl(void)
{
    enum {
        EGL_NONE = 0x3038, EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 1, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES2_BIT = 4,
        EGL_OPENGL_ES3_BIT = 0x40, EGL_RED_SIZE = 0x3024, EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022, EGL_ALPHA_SIZE = 0x3021,
        EGL_CONTEXT_CLIENT_VERSION = 0x3098
    };
    /* No depth, no stencil: Chromium's frame needs neither (its render target has no stencil). */
    EGLint config[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLint context[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLint n = 0;
    if (W.egl) return W.egl == 1;
    W.egl = 2;
    W.choose = (u32(*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))mg_egl_host("eglChooseConfig");
    W.create_context = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))mg_egl_host("eglCreateContext");
    W.create_pbuffer = (EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint *))mg_egl_host("eglCreatePbufferSurface");
    W.destroy_surface = (u32(*)(EGLDisplay, EGLSurface))mg_egl_host("eglDestroySurface");
    W.make_current = (u32(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))mg_egl_host("eglMakeCurrent");
    W.get_error = (EGLint(*)(void))mg_egl_host("eglGetError");
    W.current_context = (EGLContext(*)(void))mg_egl_host("eglGetCurrentContext");
    W.viewport = (void (*)(i32, i32, i32, i32))mg_egl_host("glViewport");
    W.scissor = (void (*)(i32, i32, i32, i32))mg_egl_host("glScissor");
    W.enable = (void (*)(u32))mg_egl_host("glEnable");
    W.disable = (void (*)(u32))mg_egl_host("glDisable");
    W.clear = (void (*)(u32))mg_egl_host("glClear");
    W.bind_framebuffer = (void (*)(u32, u32))mg_egl_host("glBindFramebuffer");
    W.color_mask = (void (*)(u8, u8, u8, u8))mg_egl_host("glColorMask");
    W.clear_color = (void (*)(float, float, float, float))mg_egl_host("glClearColor");
    if (!W.choose || !W.create_context || !W.create_pbuffer || !W.destroy_surface || !W.make_current || !W.get_error || !W.current_context || !W.viewport || !W.scissor ||
        !W.enable || !W.disable || !W.clear || !W.bind_framebuffer || !W.clear_color) {
        mg_warn("webview: EGL or OpenGL ES lacks what the render thread draws with: pages are drawn in software");
        return false;
    }
    /* Chromium's display: what its own eglGetDisplay is given (ndk/mg_egl.c). */
    W.display = mg_egl_display_for_library("libwebviewchromium.so");
    if (!W.display) return false;
    W.version = 3;
    if (!W.choose(W.display, config, &W.config, 1, &n) || n < 1) {
        config[3] = EGL_OPENGL_ES2_BIT;
        W.version = 2;
        n = 0;
        if (!W.choose(W.display, config, &W.config, 1, &n) || n < 1) {
            mg_warn("webview: no EGL configuration for the render thread's pbuffers (EGL error 0x%x): pages are drawn in software", (unsigned)W.get_error());
            return false;
        }
    }
    context[1] = W.version;
    W.context = W.create_context(W.display, W.config, NULL, context);
    if (!W.context) {
        mg_warn("webview: no OpenGL ES %d context for the render thread (EGL error 0x%x): pages are drawn in software", W.version, (unsigned)W.get_error());
        return false;
    }
    mg_info("webview: the render thread draws Chromium's functors with OpenGL ES %d on the WebView's display", W.version);
    W.egl = 1;
    return true;
}

static void wv_surface_free(wv_functor *f)
{
    if (!f->surface) return;
    W.make_current(W.display, NULL, NULL, NULL);
    W.destroy_surface(W.display, f->surface);
    f->surface = NULL;
    f->pw = f->ph = 0;
    memset(&f->share, 0, sizeof(f->share));
}

static int wv_round(int n) { return (n + WV_ROUND - 1) / WV_ROUND * WV_ROUND; }

/*
 * The pbuffer as HWUI leaves its surface for a functor (Chromium leaves the state it drew with, as HWUI then sets its own):
 * the surface's framebuffer, cleared, its viewport the surface, its scissor the part (w x h, at the top left), no stencil test
 * (with one Chromium draws into a texture of its own and that through the stencil).
 */
static void wv_prepare(wv_functor *f, int w, int h)
{
    enum { GL_FRAMEBUFFER = 0x8D40, GL_SCISSOR_TEST = 0x0C11, GL_STENCIL_TEST = 0x0B90, GL_COLOR_BUFFER_BIT = 0x4000 };
    W.bind_framebuffer(GL_FRAMEBUFFER, 0);
    W.disable(GL_SCISSOR_TEST);
    W.disable(GL_STENCIL_TEST);
    if (W.color_mask) W.color_mask(1, 1, 1, 1);
    W.clear_color(0, 0, 0, 0);
    W.clear(GL_COLOR_BUFFER_BIT);
    W.viewport(0, 0, f->pw, f->ph);
    W.enable(GL_SCISSOR_TEST);
    W.scissor(0, f->ph - h, w, h);
}

/*
 * The flag trace-webview: what a functor's draw left in its pbuffer, said for its first 30 draws and every 100th after -- the
 * OpenGL error, and the colour (R, G, B, A) at the middle of the part and of its four quarters, read back -- to tell a page
 * Chromium drew from one it did not.
 */
static void wv_trace(wv_functor *f, int l, int t, int w, int h)
{
    static int wanted = -1;
    typedef void (*fn_read)(i32, i32, i32, i32, u32, u32, void *);
    typedef u32 (*fn_error)(void);
    static fn_read read_pixels;
    static fn_error gl_error;
    static const int at[5][2] = { { 1, 1 }, { 1, 3 }, { 3, 1 }, { 3, 3 }, { 2, 2 } };
    char line[300];
    size_t used;
    u32 err;
    int i, k = (int)f->draws + 1;
    if (wanted < 0) wanted = mg_flag("trace-webview") ? 1 : 0;
    if (!wanted || (k > 30 && k % 100)) return;
    if (!read_pixels) read_pixels = (fn_read)mg_egl_host("glReadPixels");
    if (!gl_error) gl_error = (fn_error)mg_egl_host("glGetError");
    if (!read_pixels || !gl_error) return;
    err = gl_error();
    used = (size_t)snprintf(line, sizeof(line), "webview: functor %d draw %d, part %d,%d %dx%d of %dx%d: GL error 0x%x; colours", (int)f->id, k, l, t, w, h, f->pw, f->ph,
                            (unsigned)err);
    for (i = 0; i < 5 && used + 16 < sizeof(line); i++) {
        u8 px[4] = { 0, 0, 0, 0 };
        int x = w * at[i][0] / 4, y = f->ph - h * at[i][1] / 4;   /* from the part's top left, in OpenGL's rows */
        read_pixels(x, y, 1, 1, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, px);
        used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x%02x%02x%02x", px[0], px[1], px[2], px[3]);
    }
    mg_info("%s", line);
}

/*
 * The functor drawn, the part of its view at (l, t), w x h in the view's coordinates (with its scroll): on the render thread.
 * The shell's id of its picture and the pbuffer's size (id | width << 32 | height << 48); 0 when it was not drawn.
 */
static i64 wv_draw(wv_functor *f, int l, int t, int w, int h)
{
    enum { EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056, EGL_NONE = 0x3038 };
    /* sRGB, as skcms names it: the transfer function and the primaries to XYZ D50. */
    static const float to_xyz[9] = { 0.436065674f, 0.385147095f, 0.143066406f, 0.222488403f, 0.716873169f,
                                     0.060607910f, 0.013916016f, 0.097076416f, 0.714096069f };
    aw_sync_params sync;
    aw_draw_gl_params p;
    int pw, ph;
    u64 t0, ready, rendered, finished;
    t0 = mg_time_monotonic_ns();
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !f->cb.draw_gl || !wv_egl()) return 0;
    pw = wv_round(w);
    ph = wv_round(h);
    /* A new pbuffer where the part has outgrown this one, or shrunk to under half of it either way. */
    if (!f->surface || pw > f->pw || ph > f->ph || pw * 2 < f->pw || ph * 2 < f->ph) {
        EGLint attribs[] = { EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE };
        wv_surface_free(f);
        attribs[1] = pw;
        attribs[3] = ph;
        f->surface = W.create_pbuffer(W.display, W.config, attribs);
        if (!f->surface) {
            mg_warn("webview: no %dx%d pbuffer for draw functor %d (EGL error 0x%x)", pw, ph, (int)f->id, (unsigned)W.get_error());
            return 0;
        }
        f->pw = pw;
        f->ph = ph;
    }
    if (!W.make_current(W.display, f->surface, f->surface, W.context)) {
        mg_warn("webview: the render thread's context could not be made current (EGL error 0x%x)", (unsigned)W.get_error());
        return 0;
    }
    ready = mg_time_monotonic_ns();
    memset(&sync, 0, sizeof(sync));
    sync.version = AW_DRAW_FN_VERSION;
    if (f->cb.on_sync) MG_GFN(void (*)(i32, void *, aw_sync_params *), f->cb.on_sync)(f->id, f->data, &sync);
    memset(&p, 0, sizeof(p));
    p.version = AW_DRAW_FN_VERSION;
    p.clip_left = 0;
    p.clip_top = 0;
    p.clip_right = w;
    p.clip_bottom = h;
    p.width = f->pw;
    p.height = f->ph;
    p.transform[0] = p.transform[5] = p.transform[10] = p.transform[15] = 1;
    p.transform[12] = (float)-l;
    p.transform[13] = (float)-t;
    p.transfer_g = 2.4f;
    p.transfer_a = (float)(1 / 1.055);
    p.transfer_b = (float)(0.055 / 1.055);
    p.transfer_c = (float)(1 / 12.92);
    p.transfer_d = 0.04045f;
    memcpy(p.to_xyz_d50, to_xyz, sizeof(to_xyz));
    p.overlays_mode = 0;
    p.get_surface_control = (void *)wv_no_surface_control;
    p.merge_transaction = (void *)wv_no_transaction;
    wv_prepare(f, w, h);
    MG_GFN(void (*)(i32, void *, aw_draw_gl_params *), f->cb.draw_gl)(f->id, f->data, &p);
    f->drawn = true;
    /*
     * Chromium's first draw sets up its GL (GpuServiceWebView) before it looks at the context current, and that leaves the
     * context Chromium made to look at the driver current, drawing into it instead: then the frame is drawn again, into ours.
     */
    if (W.current_context() != W.context) {
        static volatile i32 said;
        if (!mg_atomic_xchg(&said, 1)) mg_info("webview: draw functor %d left a context of Chromium's current; drawn again, in the render thread's", (int)f->id);
        if (!W.make_current(W.display, f->surface, f->surface, W.context)) return 0;
        wv_prepare(f, w, h);
        MG_GFN(void (*)(i32, void *, aw_draw_gl_params *), f->cb.draw_gl)(f->id, f->data, &p);
        if (W.current_context() != W.context && !W.make_current(W.display, f->surface, f->surface, W.context)) return 0;
    }
    rendered = mg_time_monotonic_ns();
    wv_trace(f, l, t, w, h);
    if (!mg_java_gl_share_frame(W.display, f->surface, &f->share, WV_PICTURES + f->id, f->pw, f->ph)) {
        mg_note_once("webview: the shell did not take a functor's picture: pages are drawn in software");
        return 0;
    }
    f->draws++;
    finished = mg_time_monotonic_ns();
    f->draw_ns += finished - t0;
    if (finished - t0 >= 100000000ull && (f->draws < 20 || f->draws % 120 == 0))
        mg_info("webview: slow frame %d: setup %.1f ms, Chromium %.1f ms, shared picture %.1f ms",
                (int)f->id, (double)(ready - t0) / 1e6, (double)(rendered - ready) / 1e6,
                (double)(finished - rendered) / 1e6);
    if (f->draws == 1 || f->draws % 600 == 0)
        mg_info("webview: draw functor %d drawn %u times, %u us each (the last %dx%d of a %dx%d pbuffer)", (int)f->id, (unsigned)f->draws,
                (unsigned)(f->draw_ns / f->draws / 1000), w, h, f->pw, f->ph);
    return (i64)(u32)(WV_PICTURES + f->id) | ((i64)f->pw << 32) | ((i64)f->ph << 48);
}

/* Released functors' last callbacks, on the render thread; called with g_wv_lock held, which is let go around them. */
static void wv_releases(void)
{
    int i;
    for (i = 0; i < WV_FUNCTORS; i++) {
        wv_functor *f = &W.f[i];
        if (!f->id || !f->released) continue;
        mg_lock_release(&g_wv_lock);
        if (f->drawn && f->cb.on_context_destroyed && W.egl == 1 && f->surface && W.make_current(W.display, f->surface, f->surface, W.context))
            MG_GFN(void (*)(i32, void *), f->cb.on_context_destroyed)(f->id, f->data);
        if (f->cb.on_destroyed) MG_GFN(void (*)(i32, void *), f->cb.on_destroyed)(f->id, f->data);
        if (f->surface) wv_surface_free(f);
        mg_java_gl_share_gone(WV_PICTURES + f->id);
        mg_info("webview: draw functor %d gone, drawn %u times", (int)f->id, (unsigned)f->draws);
        mg_lock_acquire(&g_wv_lock);
        memset(f, 0, sizeof(*f));
        W.releases--;
    }
}

static void wv_main(void *arg)
{
    (void)arg;
    /* A thread of the app's: Chromium's code runs on it, and may call Java. */
    mg_gthread_self();
    mg_dvm_self();
    mg_dvm_thread_attach("WebView RenderThread", NULL, true);
    mg_thread_runs_app_code();
    mg_lock_acquire(&g_wv_lock);
    for (;;) {
        while (!W.asked && W.releases <= 0) mg_cond_wait(&g_wv_wake, &g_wv_lock);
        if (W.releases > 0) wv_releases();
        if (W.asked) {
            wv_functor *f = wv_find(W.ask_id);
            i32 l = W.ask_l, t = W.ask_t, w = W.ask_w, h = W.ask_h;
            i64 answer = 0;
            W.asked = false;
            mg_lock_release(&g_wv_lock);
            /* (Only this thread frees a slot: f stays this functor's while it draws.) */
            if (f && !f->released) answer = wv_draw(f, l, t, w, h);
            if (mg_dvm_pending()) {
                char what[300];
                mg_dvm_describe_pending(what, sizeof(what));
                mg_warn("webview: draw functor %d threw %s", (int)(f ? f->id : 0), what);
                mg_dvm_clear_pending();
            }
            mg_lock_acquire(&g_wv_lock);
            W.answer = answer;
            W.done = true;
            mg_cond_broadcast(&g_wv_answered);
        }
    }
}

static bool wv_start(void)
{
    if (W.started) return W.started == 1;
    mg_lock_acquire(&g_wv_lock);
    if (!W.started) {
        mg_thread *t = mg_thread_start(wv_main, NULL, 2u << 20, "WebView RenderThread");
        if (t) mg_thread_detach(t);
        else mg_warn("webview: the render thread could not be started: pages are drawn in software");
        W.started = t ? 1 : 2;
    }
    mg_lock_release(&g_wv_lock);
    return W.started == 1;
}

/* megler.WebFunctors.nativeOn(): whether Chromium is given the table, and the render thread may be asked to draw. */
N(WF_on) { c->ret.z = wv_wanted() && !W.stuck; }

/*
 * The functor drawn on the render thread, the part of the view at (l, t), w x h; the calling thread waits. The shell's id of
 * its picture and the pbuffer's size (id | width << 32 | height << 48), 0 when it was not drawn.
 */
static i64 wv_ask(i32 id, i32 l, i32 t, i32 w, i32 h)
{
    u64 until;
    i64 answer = 0;
    if (W.stuck || !wv_start()) return 0;
    mg_lock_acquire(&g_wv_serial);
    mg_lock_acquire(&g_wv_lock);
    W.ask_id = id;
    W.ask_l = l;
    W.ask_t = t;
    W.ask_w = w;
    W.ask_h = h;
    W.done = false;
    W.asked = true;
    mg_cond_signal(&g_wv_wake);
    until = mg_time_monotonic_ns() + (u64)WV_WAIT_MS * 1000000u;
    while (!W.done) {
        u64 now = mg_time_monotonic_ns();
        if (now >= until) break;
        mg_cond_wait_ns(&g_wv_answered, &g_wv_lock, until - now);
    }
    if (W.done) {
        answer = W.answer;
    } else {
        mg_atomic_xchg(&W.stuck, 1);
        mg_warn("webview: draw functor %d was not drawn in %d ms: pages are drawn in software from now on", (int)id, WV_WAIT_MS);
    }
    mg_lock_release(&g_wv_lock);
    mg_lock_release(&g_wv_serial);
    return answer;
}

/* megler.WebFunctors.draw(int functor, int left, int top, int width, int height): wv_ask. */
N(WF_draw) { c->ret.j = wv_wanted() ? wv_ask(c->args[0].i, c->args[1].i, c->args[2].i, c->args[3].i, c->args[4].i) : 0; }

const void *mg_java_webview_draw_fn_table(void) { return &k_draw_fn_table; }

i64 mg_java_webview_draw_functor(i32 functor, int left, int top, int width, int height) { return wv_ask(functor, left, top, width, height); }

void mg_java_webview_forget_egl(void)
{
    mg_lock_acquire(&g_wv_serial);
    W.egl = 0;
    W.display = W.config = W.context = NULL;
    mg_lock_release(&g_wv_serial);
}

/* DrawFunctor.nativeGetFunctionTable: the draw functor's table where pages are drawn on the graphics processor (above). */
N(DrawFn_table)
{
    c->ret.j = wv_wanted() ? (i64)(mg_addr)&k_draw_fn_table : 0;
    if (c->ret.j) mg_info("webview: the platform's draw functor table is handed over (version %d)", AW_DRAW_FN_VERSION);
}

/* The platform support's other tables: none (see the top). */
N(Null_table) { c->ret.j = 0; }
N(Nothing) { (void)c; }

static const mg_jhle k_webview[] = {
    { "android/webkit/WebViewLibraryLoader", "nativeReserveAddressSpace", NULL, Loader_reserve },
    { "android/webkit/WebViewLibraryLoader", "nativeLoadWithRelroFile", NULL, Loader_load },
    { "com/android/webview/chromium/GraphicsUtils", "nativeGetDrawSWFunctionTable", NULL, SW_table },
    { "com/android/webview/chromium/GraphicsUtils", "nativeGetDrawGLFunctionTable", NULL, Null_table },
    { "com/android/webview/chromium/DrawFunctor", "nativeGetFunctionTable", NULL, DrawFn_table },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeCreateGLFunctor", NULL, Null_table },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeDestroyGLFunctor", NULL, Nothing },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeSetChromiumAwDrawGLFunction", NULL, Nothing },
    { "megler/WebFunctors", "nativeOn", NULL, WF_on },
    { "megler/WebFunctors", "draw", NULL, WF_draw },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_webview_natives(void) { return k_webview; }
