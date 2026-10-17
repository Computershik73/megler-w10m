/*
 * What Android's platform gives the WebView besides the framework's Java: the natives of android.webkit.WebViewLibraryLoader,
 * which are libwebviewchromium_loader.so's on Android, and those of libwebviewchromium_plat_support.so, which the WebView's
 * glue loads by name and whose natives its classes declare (docs/JAVA.md, "The WebView").
 *
 * The loader. On Android the zygote reserves address space for the WebView's library, an isolated process loads it once to
 * write its relocated RELRO, and each app loads it into its reservation with that RELRO, by android_dlopen_ext with
 * ANDROID_DLEXT_RESERVED_ADDRESS and USE_RELRO, in the linker namespace of the WebView's class loader. Here there is one
 * process: nativeReserveAddressSpace maps the range as Chromium's own linker does (mmap, PROT_NONE), and
 * nativeLoadWithRelroFile loads the library into it the way android_dlopen_ext does with RESERVED_ADDRESS_HINT
 * (bionic/hk_libdl.c, hk_ld_load_placed), and runs its constructors; no RELRO is shared, there being no other process. The
 * library is found as the class loader's namespace finds it: the loader's findLibrary names "<apk>!/lib/<abi>/<name>", an
 * APK whose libraries are used from it as they are (the WebView's are stored, not extracted), and that APK is given to the
 * linker to read libraries from. JNI_OnLoad is not run here: as on Android it runs when the WebView's own LibraryLoader calls
 * System.loadLibrary("webviewchromium"), which finds the library loaded (dvm/hk_dvm_jvm.c, load_native).
 *
 * The platform support. libwebviewchromium_plat_support.so gives the WebView the tables of functions it draws through on a
 * canvas: GraphicsUtils.nativeGetDrawSWFunctionTable (Skia's canvas under an android.graphics.Canvas, which the WebView
 * draws into directly when it can), nativeGetDrawGLFunctionTable and DrawFunctor.nativeGetFunctionTable (the GL functor and
 * draw functor of a hardware-accelerated canvas), and DrawGLFunctor's functor. None of those exist here: a canvas is
 * Megler's, drawn by the shell, and none is hardware-accelerated. So every table is null. With no software table Chromium
 * draws a page into a Bitmap of its own and draws that bitmap into the view's canvas (JavaBrowserViewRendererHelper, its
 * "auxiliary bitmap"); with no GL table it never asks for a functor. The library itself is a name the linker knows as a
 * system library (bionic/hk_bionic.c), so that System.loadLibrary finds and "loads" it.
 */
#include <stdio.h>
#include <string.h>

#include "../bionic/hk_bionic.h"
#include "../dvm/hk_dvm_internal.h"
#include "../ld/hk_ld.h"
#include "hk_java.h"

#define N(name) static void name(hk_jcall *c)

/* The range nativeReserveAddressSpace reserved, which the library is loaded into. */
static void *g_reserved;
static size_t g_reserved_size;

/* nativeReserveAddressSpace(long size): true when the range is reserved. Once: a second call answers what the first did. */
N(Loader_reserve)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, i32);
    mmap_fn guest_mmap = (mmap_fn)hk_bionic_find("mmap");
    i64 size = c->args[0].j;
    void *p;
    if (g_reserved) {
        c->ret.z = 1;
        return;
    }
    if (!guest_mmap || size <= 0 || size > (512 << 20)) {
        c->ret.z = 0;
        return;
    }
    /* PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS: what the zygote's loader and Chromium's linker reserve with. */
    p = guest_mmap(NULL, (size_t)size, 0, 0x02 | 0x20, -1, 0);
    if (!p || p == (void *)(hk_addr)-1) {
        hk_warn("webview: %u MB of address space for the WebView's library could not be reserved", (unsigned)(size >> 20));
        c->ret.z = 0;
        return;
    }
    g_reserved = p;
    g_reserved_size = (size_t)size;
    hk_info("webview: %u MB of address space reserved for the WebView's library at %p", (unsigned)(size >> 20), p);
    c->ret.z = 1;
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
    const char *lib = hk_dvm_string_utf8(c->args[0].l);
    hk_jobj *loader = c->args[2].l;
    char name[200], apk[400], why[300];
    hk_jvalue arg, found;
    const char *path;
    char *host;
    hk_lib *l;
    size_t n;
    c->ret.i = 6;
    if (!lib || !loader) return;
    /* findLibrary takes the name System.loadLibrary would: "webviewchromium" for libwebviewchromium.so. */
    n = strlen(lib);
    if (!strncmp(lib, "lib", 3) && n > 6 && !strcmp(lib + n - 3, ".so")) snprintf(name, sizeof(name), "%.*s", (int)(n - 6), lib + 3);
    else hk_strlcpy(name, lib, sizeof(name));
    arg.j = 0;
    arg.l = hk_dvm_new_string(name);
    found.j = 0;
    if (!hk_dvm_call_virtual(loader, "findLibrary", "(Ljava/lang/String;)Ljava/lang/String;", &arg, &found) || !found.l) {
        hk_dvm_clear_pending();
        hk_warn("webview: the WebView's class loader finds no library %s", lib);
        c->ret.i = 10;
        return;
    }
    path = hk_dvm_string_utf8(found.l);
    if (!path || !apk_of(path, apk, sizeof(apk))) {
        hk_warn("webview: the WebView's library is %s, which is not in an APK; only an APK's are loaded so", path ? path : "nothing");
        return;
    }
    l = hk_ld_find_lib(lib);
    if (!l) {
        host = hk_fs_host_path(apk);
        if (!host || !hk_ld_add_apk(host)) {
            hk_warn("webview: %s cannot be read for its libraries", apk);
            free(host);
            return;
        }
        free(host);
        why[0] = 0;
        hk_info("webview: loading %s from %s", lib, apk);
        l = g_reserved ? hk_ld_load_placed(lib, g_reserved, g_reserved_size, true, false, why, sizeof(why)) : hk_ld_load(lib);
        if (!l) {
            hk_warn("webview: %s did not load%s%s", lib, why[0] ? ": " : "", why);
            return;
        }
    }
    if (!hk_ld_init(l)) {
        hk_warn("webview: the constructors of %s did not run", lib);
        return;
    }
    hk_info("webview: %s loaded at %p", lib, hk_ld_lib_base(l));
    c->ret.i = 0;
}

/* The platform support's tables: none (see the top). */
N(Null_table) { c->ret.j = 0; }
N(Nothing) { (void)c; }

static const hk_jhle k_webview[] = {
    { "android/webkit/WebViewLibraryLoader", "nativeReserveAddressSpace", NULL, Loader_reserve },
    { "android/webkit/WebViewLibraryLoader", "nativeLoadWithRelroFile", NULL, Loader_load },
    { "com/android/webview/chromium/GraphicsUtils", "nativeGetDrawSWFunctionTable", NULL, Null_table },
    { "com/android/webview/chromium/GraphicsUtils", "nativeGetDrawGLFunctionTable", NULL, Null_table },
    { "com/android/webview/chromium/DrawFunctor", "nativeGetFunctionTable", NULL, Null_table },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeCreateGLFunctor", NULL, Null_table },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeDestroyGLFunctor", NULL, Nothing },
    { "com/android/webview/chromium/DrawGLFunctor", "nativeSetChromiumAwDrawGLFunction", NULL, Nothing },
    { NULL, NULL, NULL, NULL },
};

const hk_jhle *hk_java_webview_natives(void) { return k_webview; }
