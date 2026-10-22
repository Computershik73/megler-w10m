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
 * draw functor of a hardware-accelerated canvas), and DrawGLFunctor's functor. None of those exist here: a canvas is
 * Megler's, drawn by the shell, and none is hardware-accelerated. So every table is null. With no software table Chromium
 * draws a page into a Bitmap of its own and draws that bitmap into the view's canvas (JavaBrowserViewRendererHelper, its
 * "auxiliary bitmap"); with no GL table it never asks for a functor. The library itself is a name the linker knows as a
 * system library (bionic/mg_bionic.c), so that System.loadLibrary finds and "loads" it.
 */
#include <stdio.h>
#include <string.h>

#include "../bionic/mg_bionic.h"
#include "../dvm/mg_dvm_internal.h"
#include "../ld/mg_ld.h"
#include "mg_java.h"

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

/* The platform support's tables: none (see the top). */
N(Null_table) { c->ret.j = 0; }
N(Nothing) { (void)c; }

static const mg_jhle k_webview[] = {
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

const mg_jhle *mg_java_webview_natives(void) { return k_webview; }
