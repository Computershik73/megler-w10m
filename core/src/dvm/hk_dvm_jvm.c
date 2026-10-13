/*
 * libcore's own native libraries, and the one of them that is not a file.
 *
 * libjavacore.so and libopenjdk.so are Android code like an app's: the linker
 * (ld/) loads them, their JNI_OnLoad registers their native methods through
 * the JNIEnv (jni/), and from then on libcore's files, character sets,
 * regular expressions, time zones and big numbers are theirs. That is
 * hk_dvm_load_libcore_natives, in the order Android's runtime does it
 * (art/runtime/runtime.cc, InitNativeMethods).
 *
 * libopenjdk.so imports some two dozen functions from libopenjdkjvm.so, which
 * on Android is part of the runtime (art/runtime/openjdkjvm/OpenjdkJvm.cc):
 * the files it opens and reads go through them, and a few questions about the
 * virtual machine. They are here, registered with the linker as a system
 * library's.
 *
 * Guest code calls these, so nothing that crosses has a float in its
 * signature (docs/ARCHITECTURE.md): JVM_IsNaN takes its double as bits.
 */
#include "hk_dvm_internal.h"

#include <stdio.h>

#include "../bionic/hk_bionic.h"
#include "../bionic/hk_fd.h"
#include "../ld/hk_ld.h"

/* The layer's own C library, by name: these functions are its open, read and the rest, as the guest would call them. */
static void *libc(const char *name)
{
    void *fn = hk_bionic_find(name);
    if (!fn) hk_fatal("dvm: the layer's C library has no %s", name);
    return fn;
}

/* As art/openjdkjvm/OpenjdkJvm.cc has them: the C library's own calls, with the few differences that file says. */
static i32 JVM_Open(const char *path, i32 flags, i32 mode)
{
    i32 fd;
    /* JVM_O_DELETE (delete on close) is fatal on Android; here it is refused out loud and the file opened without it. */
    if (flags & 0x10000) hk_warn("java: JVM_Open of %s asked for delete-on-close, which Android does not have", path);
    fd = ((i32 (*)(const char *, i32, ...))libc("open"))(path, flags & ~0x10000, mode);
    /* -1 for a failure, but JVM_EEXIST (-100) when it is that the file is there and was asked not to be. */
    if (fd < 0 && *hk_errno_ptr() == GE_EXIST) return -100;
    return fd;
}
static i32 JVM_Close(i32 fd) { return ((i32 (*)(i32))libc("close"))(fd); }
static i32 JVM_Read(i32 fd, char *buf, i32 n) { return ((i32 (*)(i32, void *, u32))libc("read"))(fd, buf, (u32)n); }
static i32 JVM_Write(i32 fd, char *buf, i32 n) { return ((i32 (*)(i32, const void *, u32))libc("write"))(fd, buf, (u32)n); }
static i64 JVM_Lseek(i32 fd, i64 offset, i32 whence) { return ((i64 (*)(i32, i64, i32))libc("lseek64"))(fd, offset, whence); }
static i32 JVM_Sync(i32 fd) { return ((i32 (*)(i32))libc("fsync"))(fd); }
static char *JVM_NativePath(char *path) { return path; }
static i32 JVM_GetLastErrorString(char *buf, i32 len)
{
    /* What went wrong last, for an exception's message: the guest's errno, as its own C library words it. */
    if (len <= 0) return 0;
    snprintf(buf, (size_t)len, "%s", ((const char *(*)(i32))libc("strerror"))(*((i32 *(*)(void))libc("__errno"))()));
    return (i32)strlen(buf);
}

static i64 JVM_CurrentTimeMillis(void *env, void *cls)
{
    (void)env; (void)cls;
    return (i64)(hk_time_realtime_ns() / 1000000ull);
}
/* Nothing is collected and nothing is counted: the figures say "there is room", which is all a caller can use them for. */
static i64 JVM_FreeMemory(void) { return (i64)128 << 20; }
static i64 JVM_TotalMemory(void) { return (i64)256 << 20; }
static i64 JVM_MaxMemory(void) { return (i64)256 << 20; }
static void JVM_GC(void) {}
static i32 JVM_InitializeSocketLibrary(void) { return 0; }
static u32 JVM_IsNaN(u64 bits)
{
    double d = hk_d(bits);
    return d != d;
}
static void JVM_Exit(i32 status)
{
    hk_warn("java: Runtime.exit(%d)", (int)status);
    ((void (*)(i32))libc("exit"))(status);
}
static void *JVM_FindLibraryEntry(void *handle, const char *name) { return ((void *(*)(void *, const char *))libc("dlsym"))(handle, name); }

/* A "raw monitor" is a plain lock of the virtual machine's. */
static void *JVM_RawMonitorCreate(void)
{
    hk_lock *l = (hk_lock *)hk_alloc(sizeof(*l));
    hk_lock_init(l);
    return l;
}
static void JVM_RawMonitorDestroy(void *mon) { free(mon); }
static i32 JVM_RawMonitorEnter(void *mon)
{
    hk_lock_acquire((hk_lock *)mon);
    return 0;
}
static void JVM_RawMonitorExit(void *mon) { hk_lock_release((hk_lock *)mon); }

static i32 jio_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ((int (*)(char *, size_t, const char *, va_list))libc("vsnprintf"))(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
static i32 jio_fprintf(void *file, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ((int (*)(void *, const char *, va_list))libc("vfprintf"))(file, fmt, ap);
    va_end(ap);
    return r;
}

/* Where an app's own libraries are looked for by name, as the linker's own search path would say. */
static void android_get_LD_LIBRARY_PATH(char *buf, size_t n)
{
    if (n) hk_strlcpy(buf, "/system/lib", n);
}

static bool load_native(const char *name_or_path, bool runtime, char *why, size_t why_size);

/*
 * Runtime.nativeLoad: System.load and System.loadLibrary. The library is
 * loaded by the layer's linker and its JNI_OnLoad run; what is handed back is
 * null, or the reason it could not be done, as a string for the
 * UnsatisfiedLinkError, in ART's words (JavaVMExt::LoadNativeLibrary).
 *
 * Whose library it is follows from the class loader libcore passes, which is
 * that of the class that called System.loadLibrary: none for the runtime's own
 * classes, whose libraries are the Android root's, the app's for the app's,
 * whose libraries are the APK's. libcore has found the path before it comes
 * here (BaseDexClassLoader.findLibrary: the app's library folder, where the
 * Java driver has put each of the APK's libraries), so a library the APK does
 * not have never gets this far: that is libcore's own UnsatisfiedLinkError,
 * "... couldn't find "libfoo.so"".
 */
static hk_jobj *JVM_NativeLoad(void *env, hk_jobj *path, hk_jobj *loader)
{
    const char *name = hk_dvm_string_utf8(path);
    char why[700];
    (void)env;
    if (!name) return NULL;
    if (load_native(name, loader == NULL, why, sizeof(why))) return NULL;
    /* An exception JNI_OnLoad left is not the answer: the reason is. */
    hk_dvm_clear_pending();
    return hk_dvm_new_string(why);
}

static const hk_sym k_jvm[] = {
    HK_WRAP("JVM_Open", JVM_Open), HK_WRAP("JVM_Close", JVM_Close), HK_WRAP("JVM_Read", JVM_Read), HK_WRAP("JVM_Write", JVM_Write),
    HK_WRAP("JVM_Lseek", JVM_Lseek), HK_WRAP("JVM_Sync", JVM_Sync), HK_WRAP("JVM_NativePath", JVM_NativePath),
    HK_WRAP("JVM_GetLastErrorString", JVM_GetLastErrorString), HK_WRAP("JVM_CurrentTimeMillis", JVM_CurrentTimeMillis),
    HK_WRAP("JVM_FreeMemory", JVM_FreeMemory), HK_WRAP("JVM_TotalMemory", JVM_TotalMemory), HK_WRAP("JVM_MaxMemory", JVM_MaxMemory),
    HK_WRAP("JVM_GC", JVM_GC), HK_WRAP("JVM_InitializeSocketLibrary", JVM_InitializeSocketLibrary), HK_WRAP("JVM_IsNaN", JVM_IsNaN),
    HK_WRAP("JVM_Exit", JVM_Exit), HK_WRAP("JVM_FindLibraryEntry", JVM_FindLibraryEntry), HK_WRAP("JVM_NativeLoad", JVM_NativeLoad),
    HK_WRAP("JVM_RawMonitorCreate", JVM_RawMonitorCreate), HK_WRAP("JVM_RawMonitorDestroy", JVM_RawMonitorDestroy),
    HK_WRAP("JVM_RawMonitorEnter", JVM_RawMonitorEnter), HK_WRAP("JVM_RawMonitorExit", JVM_RawMonitorExit),
    HK_WRAP("jio_snprintf", jio_snprintf), HK_WRAP("jio_fprintf", jio_fprintf),
    HK_WRAP("android_get_LD_LIBRARY_PATH", android_get_LD_LIBRARY_PATH),
    HK_END
};

/*
 * One library, of the runtime's or of the app's, loaded once: a second call
 * for it answers true and does nothing, as ART does for a library its class
 * loader has loaded (the library's JNI_OnLoad is not run again).
 *
 * An app's library is the APK's of that file name. Only a path that is not in
 * the APK and names a file with something in it -- a library the app unpacked
 * or downloaded for itself and loads with System.load -- is read from that
 * file; what it needs still comes from the APK.
 *
 * While JNI_OnLoad runs, the thread says so (onload_*): FindClass in there is
 * for the loader the library is loaded for. An exception JNI_OnLoad leaves
 * stays pending when the library is otherwise loaded, and is what
 * System.loadLibrary then throws; that too is ART's.
 */
static bool load_native(const char *name_or_path, bool runtime, char *why, size_t why_size)
{
#if HK_HOST_ARM32
    const char *slash = strrchr(name_or_path, '/'), *base = slash ? slash + 1 : name_or_path;
    hk_dvm_thread *t = hk_dvm_self();
    hk_lib *lib = runtime ? hk_ld_find_runtime_lib(base) : hk_ld_find_lib(base);
    hk_dvm_frame *was_frame;
    bool was_running, was_app;
    void *onload;
    i32 version;
    why[0] = 0;
    if (lib) return true;
    if (runtime) {
        lib = hk_ld_load_runtime(base);
    } else if (hk_ld_has_lib(base)) {
        lib = hk_ld_load(base);
    } else {
        char *host = slash ? hk_fs_host_path(name_or_path) : NULL;
        hk_stat st;
        if (host && hk_path_stat(host, &st, NULL) && !st.is_dir && st.size > 0) {
            lib = hk_ld_load_file(host);
        } else {
            snprintf(why, why_size, "dlopen failed: library \"%s\" not found", name_or_path);
            free(host);
            return false;
        }
        free(host);
    }
    if (!lib || !hk_ld_init(lib)) {
        snprintf(why, why_size, "dlopen failed: \"%s\" could not be loaded (the layer's log says what its linker made of it)", name_or_path);
        return false;
    }
    onload = hk_ld_sym(lib, "JNI_OnLoad");
    if (!onload || hk_ld_lib_of(onload) != lib) return true;    /* a library need not have one */
    was_frame = t->onload_frame;
    was_running = t->onload_running;
    was_app = t->onload_app;
    t->onload_frame = t->top;
    t->onload_running = true;
    t->onload_app = !runtime;
    version = HK_GFN(i32 (*)(void *, void *), onload)(hk_jni_vm(), NULL);
    t->onload_frame = was_frame;
    t->onload_running = was_running;
    t->onload_app = was_app;
    if (version == -1) {
        snprintf(why, why_size, "JNI_ERR returned from JNI_OnLoad in \"%s\"", name_or_path);
        return false;
    }
    if (version != 0x10002 && version != 0x10004 && version != 0x10006) {
        snprintf(why, why_size, "Bad JNI version returned from JNI_OnLoad in \"%s\": %d", name_or_path, (int)version);
        return false;
    }
    return true;
#else
    (void)runtime;
    snprintf(why, why_size, "dlopen failed: \"%s\" is 32-bit ARM code, which this host does not run", name_or_path);
    hk_note_once("dvm: %s cannot be loaded on this host, which does not run ARM code", name_or_path);
    return false;
#endif
}

bool hk_dvm_load_library(const char *name_or_path)
{
    char why[700];
    if (load_native(name_or_path, false, why, sizeof(why))) return true;
    hk_warn("dvm: %s", why);
    return false;
}

/* One of the runtime's own: the reason it did not load, or the exception its JNI_OnLoad left, is for the log. */
static bool load_runtime_library(const char *name)
{
    char why[700], text[400];
    bool ok = load_native(name, true, why, sizeof(why));
    if (!ok) hk_warn("dvm: %s", why);
    if (hk_dvm_pending()) {
        hk_warn("dvm: JNI_OnLoad of %s left %s", name, hk_dvm_describe_pending(text, sizeof(text)) ? text : "an exception");
        hk_dvm_clear_pending();
        ok = false;
    }
    return ok;
}

bool hk_dvm_load_libcore_natives(const char *root)
{
#if HK_HOST_ARM32
    static bool registered;
    char *dir = hk_sprintf("%s/lib", root), *probe = hk_sprintf("%s/lib/libjavacore.so", root);
    hk_stat st;
    hk_lib *helper;
    void *init;
    bool ok = false;
    if (hk_path_stat(probe, &st, NULL)) {
        if (!registered) {
            registered = true;
            hk_bionic_register(k_jvm);
        }
        /* Android's own layout to the libraries: ICU maps /system/usr/icu/icudt60l.dat, libcore reads /system/usr/share/zoneinfo. */
        hk_fs_mount("/system", root);
        /* These are the runtime's libraries, not the app's: the linker keeps the two apart (ld/hk_ld.h). */
        hk_ld_add_runtime_dir(dir);
        /* libnativehelper keeps the classes and fields the two libraries name; Android's runtime has it look them up before either is loaded. */
        helper = hk_ld_load_runtime("libnativehelper.so");
        init = helper && hk_ld_init(helper) ? hk_ld_sym(helper, "_ZN12JniConstants4initEP7_JNIEnv") : NULL;
        if (init) {
            HK_GFN(void (*)(void *), init)(hk_jni_env());
            if (hk_dvm_pending()) {
                char text[400];
                hk_warn("dvm: libnativehelper's JniConstants::init left %s", hk_dvm_describe_pending(text, sizeof(text)) ? text : "an exception");
                hk_dvm_clear_pending();
            }
            ok = load_runtime_library("libjavacore.so") && load_runtime_library("libopenjdk.so");
        } else {
            hk_warn("dvm: libnativehelper.so did not load from %s", dir);
        }
    }
    free(dir);
    free(probe);
    return ok;
#else
    (void)root;
    (void)k_jvm;
    return false;
#endif
}
