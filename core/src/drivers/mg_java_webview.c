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
 * draw functor of a hardware-accelerated canvas), and DrawGLFunctor's functor. A canvas is Megler's, drawn by the shell, and
 * none is hardware-accelerated: the GL tables are null, and Chromium never asks for a functor. The software table is there
 * (sw_access_pixels, below): a canvas of a Bitmap of the core's hands Chromium its pixels, which it draws into directly; any
 * other canvas has none, and Chromium draws a page into a Bitmap of its own and draws that bitmap into the view's canvas
 * (JavaBrowserViewRendererHelper, its "auxiliary bitmap"). The library itself is a name the linker knows as a system
 * library (bionic/mg_bionic.c), so that System.loadLibrary finds and "loads" it.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bionic/mg_bionic.h"
#include "../dvm/mg_dvm_internal.h"
#include "../ld/mg_ld.h"
#include "mg_java.h"
#include "mg_java_gfx.h"

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

/* The platform support's other tables: none (see the top). */
N(Null_table) { c->ret.j = 0; }
N(Nothing) { (void)c; }

static const mg_jhle k_webview[] = {
    { "android/webkit/WebViewLibraryLoader", "nativeReserveAddressSpace", NULL, Loader_reserve },
    { "android/webkit/WebViewLibraryLoader", "nativeLoadWithRelroFile", NULL, Loader_load },
    { "com/android/webview/chromium/GraphicsUtils", "nativeGetDrawSWFunctionTable", NULL, SW_table },
    { "com/android/webview/chromium/GraphicsUtils", "nativeGetDrawGLFunctionTable", NULL, Null_table },
    { "com/android/webview/chromium/DrawFunctor", "nativeGetFunctionTable", NULL, Null_table },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeCreateGLFunctor", NULL, Null_table },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeDestroyGLFunctor", NULL, Nothing },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeSetChromiumAwDrawGLFunction", NULL, Nothing },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_webview_natives(void) { return k_webview; }
