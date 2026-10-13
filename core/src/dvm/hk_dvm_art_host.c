/*
 * A few native methods of libcore's own libraries, done here.
 *
 * On Android these are in libjavacore.so and libopenjdk.so, and where those
 * libraries are loaded (hk_dvm_load_libcore_natives: a 32-bit ARM host) they
 * are theirs: everything in this file is registered as a fallback, which a
 * method gets only when no library has it. Where the libraries cannot run --
 * the PC's build, which has no way to execute ARM code -- these are what lets
 * java.lang.System initialise, and with it everything that only needs
 * libcore's bytecode: collections, strings, text that is not formatted by
 * ICU. They are the least that does it, not an implementation of the
 * libraries: no files beyond the three standard streams, no locale data, no
 * time zones, no network.
 */
#include "hk_dvm_internal.h"

#include <math.h>
#include <stdio.h>

#define N(name) static void name(hk_jcall *c)

static hk_jobj *strings(const char *const *items, int n)
{
    hk_jobj *a = hk_dvm_new_array("[Ljava/lang/String;", n);
    int i;
    for (i = 0; a && i < n; i++) a->u.oarr.v[i] = hk_dvm_new_string(items[i]);
    return a;
}

N(System_specialProperties)
{
    static const char *const props[] = { "user.dir=/", "android.zlib.version=1.2.11", "android.openssl.version=none", "java.library.path=/system/lib" };
    c->ret.l = strings(props, (int)HK_COUNT(props));
}

N(System_currentTimeMillis) { c->ret.j = (i64)(hk_time_realtime_ns() / 1000000ull); }
N(System_nanoTime) { c->ret.j = (i64)hk_time_monotonic_ns(); }

N(System_log)
{
    const char *text = hk_dvm_string_utf8(c->args[1].l);
    hk_warn("java: %c %s", (char)c->args[0].c, text ? text : "");
}

N(System_mapLibraryName)
{
    const char *name = hk_dvm_string_utf8(c->args[0].l);
    char *lib;
    if (!name) {
        hk_dvm_throw("java/lang/NullPointerException", "libname == null");
        return;
    }
    lib = hk_sprintf("lib%s.so", name);
    c->ret.l = hk_dvm_new_string(lib);
    free(lib);
}

static void set_stream(hk_jcall *c, const char *field)
{
    hk_jvalue v;
    v.j = 0;
    v.l = c->args[0].l;
    hk_dvm_set_static("java/lang/System", field, v);
}
N(System_setIn0) { set_stream(c, "in"); }
N(System_setOut0) { set_stream(c, "out"); }
N(System_setErr0) { set_stream(c, "err"); }

/* libcore.io.Linux: who and where the process is, and descriptors that are only numbers. */
N(Linux_getuid) { c->ret.i = 10000; }

N(Linux_getpwuid)
{
    hk_jvalue a[5];
    memset(a, 0, sizeof(a));
    a[0].l = hk_dvm_new_string("u0_a0");
    a[1].i = c->args[0].i;
    a[2].i = c->args[0].i;
    a[3].l = hk_dvm_new_string("/");
    a[4].l = hk_dvm_new_string("/system/bin/sh");
    c->ret.l = hk_dvm_new("android/system/StructPasswd", "(Ljava/lang/String;IILjava/lang/String;Ljava/lang/String;)V", a);
}

N(Linux_uname)
{
    hk_jvalue a[5];
    memset(a, 0, sizeof(a));
    a[0].l = hk_dvm_new_string("Linux");
    a[1].l = hk_dvm_new_string("localhost");
    a[2].l = hk_dvm_new_string("4.4.0");
    a[3].l = hk_dvm_new_string("#1");
    a[4].l = hk_dvm_new_string("armv7l");
    c->ret.l = hk_dvm_new("android/system/StructUtsname", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", a);
}

/* fcntlInt(fd, F_DUPFD_CLOEXEC, 0) is how FileDescriptor makes its in, out and err: the same number back is a descriptor that works as well. */
N(Linux_fcntlInt)
{
    hk_jvalue fd;
    fd.j = 0;
    hk_dvm_get_field(c->args[0].l, "descriptor", &fd);
    c->ret.i = fd.i;
}

/* sysconf: the one thing asked is how many processors there are (Runtime.availableProcessors); anything else is "no such limit". */
N(Linux_sysconf) { c->ret.j = hk_cpu_count(); }

N(Linux_getenv)
{
    const char *name = hk_dvm_string_utf8(c->args[0].l);
    if (name && !strcmp(name, "ANDROID_ROOT")) c->ret.l = hk_dvm_new_string("/system");
    else if (name && !strcmp(name, "ANDROID_DATA")) c->ret.l = hk_dvm_new_string("/data");
}

N(ICU_getIcuVersion) { c->ret.l = hk_dvm_new_string("60.2"); }
N(ICU_getUnicodeVersion) { c->ret.l = hk_dvm_new_string("10.0"); }
N(ICU_getCldrVersion) { c->ret.l = hk_dvm_new_string("32.0.1"); }

/*
 * What System.out and System.err are written with: the bytes go to the log,
 * a line at a time, and into a buffer the tests read (hk_dvm_art_host_output).
 */
static hk_lock g_out_lock = HK_LOCK_INIT;
static char *g_out;
static size_t g_out_len, g_out_cap, g_out_line;

/* writeBytes(FileDescriptor fd, Object buffer, int offset, int count): only to the standard output and error. */
N(Linux_writeBytes)
{
    hk_jobj *bytes = c->args[1].l;
    i32 off = c->args[2].i, n = c->args[3].i;
    hk_jvalue fd;
    fd.j = 0;
    hk_dvm_get_field(c->args[0].l, "descriptor", &fd);
    if (!bytes || bytes->kind != HK_J_PRIM_ARRAY || off < 0 || n < 0 || off > (i32)bytes->u.arr.len - n || (fd.i != 1 && fd.i != 2)) {
        hk_dvm_throw("java/lang/UnsupportedOperationException", "a write to descriptor %d, with libcore's libraries not loaded", (int)fd.i);
        return;
    }
    c->ret.i = n;
    hk_lock_acquire(&g_out_lock);
    if (g_out_len + (size_t)n + 1 > g_out_cap) {
        g_out_cap = (g_out_len + (size_t)n + 1) * 2;
        g_out = (char *)hk_realloc(g_out, g_out_cap);
    }
    memcpy(g_out + g_out_len, (const u8 *)bytes->u.arr.data + off, (size_t)n);
    g_out_len += (size_t)n;
    g_out[g_out_len] = 0;
    for (;;) {
        char *nl = (char *)memchr(g_out + g_out_line, '\n', g_out_len - g_out_line);
        if (!nl) break;
        hk_info("java: %.*s", (int)(nl - (g_out + g_out_line)), g_out + g_out_line);
        g_out_line = (size_t)(nl - g_out) + 1;
    }
    hk_lock_release(&g_out_lock);
}

const char *hk_dvm_art_host_output(void) { return g_out ? g_out : ""; }

N(nothing) { (void)c; }
N(zero) { (void)c; }

N(Float_floatToRawIntBits) { c->ret.i = (i32)hk_fb(c->args[0].f); }
N(Float_intBitsToFloat) { c->ret.f = hk_f((u32)c->args[0].i); }
N(Double_doubleToRawLongBits) { c->ret.j = (i64)hk_db(c->args[0].d); }
N(Double_longBitsToDouble) { c->ret.d = hk_d((u64)c->args[0].j); }

/* java.lang.Math and StrictMath, which are the C library's functions of the same names on Android too. */
N(math)
{
    static const struct { const char *name; double (*fn)(double); } one[] = {
        { "acos", acos }, { "asin", asin }, { "atan", atan }, { "cbrt", cbrt }, { "ceil", ceil }, { "cos", cos }, { "cosh", cosh }, { "exp", exp },
        { "expm1", expm1 }, { "floor", floor }, { "log", log }, { "log10", log10 }, { "log1p", log1p }, { "rint", rint }, { "sin", sin }, { "sinh", sinh },
        { "sqrt", sqrt }, { "tan", tan }, { "tanh", tanh },
    };
    static const struct { const char *name; double (*fn)(double, double); } two[] = { { "IEEEremainder", remainder }, { "atan2", atan2 }, { "hypot", hypot }, { "pow", pow } };
    size_t i;
    for (i = 0; i < HK_COUNT(one); i++)
        if (!strcmp(one[i].name, c->name)) {
            c->ret.d = one[i].fn(c->args[0].d);
            return;
        }
    for (i = 0; i < HK_COUNT(two); i++)
        if (!strcmp(two[i].name, c->name)) {
            c->ret.d = two[i].fn(c->args[0].d, c->args[1].d);
            return;
        }
    hk_dvm_throw("java/lang/UnsatisfiedLinkError", "java.lang.Math.%s", c->name);
}

static const hk_jhle k_host[] = {
    { "java/lang/System", "specialProperties", NULL, System_specialProperties },
    { "java/lang/System", "currentTimeMillis", NULL, System_currentTimeMillis },
    { "java/lang/System", "nanoTime", NULL, System_nanoTime },
    { "java/lang/System", "log", NULL, System_log },
    { "java/lang/System", "mapLibraryName", NULL, System_mapLibraryName },
    { "java/lang/System", "setIn0", NULL, System_setIn0 },
    { "java/lang/System", "setOut0", NULL, System_setOut0 },
    { "java/lang/System", "setErr0", NULL, System_setErr0 },
    { "libcore/io/Linux", "getuid", NULL, Linux_getuid },
    { "libcore/io/Linux", "getpwuid", NULL, Linux_getpwuid },
    { "libcore/io/Linux", "uname", NULL, Linux_uname },
    { "libcore/io/Linux", "fcntlInt", NULL, Linux_fcntlInt },
    { "libcore/io/Linux", "getenv", NULL, Linux_getenv },
    { "libcore/io/Linux", "sysconf", NULL, Linux_sysconf },
    { "libcore/icu/ICU", "getIcuVersion", NULL, ICU_getIcuVersion },
    { "libcore/icu/ICU", "getUnicodeVersion", NULL, ICU_getUnicodeVersion },
    { "libcore/icu/ICU", "getCldrVersion", NULL, ICU_getCldrVersion },
    { "libcore/io/Linux", "writeBytes", NULL, Linux_writeBytes },
    { "java/lang/Float", "floatToRawIntBits", NULL, Float_floatToRawIntBits },
    { "java/lang/Float", "intBitsToFloat", NULL, Float_intBitsToFloat },
    { "java/lang/Double", "doubleToRawLongBits", NULL, Double_doubleToRawLongBits },
    { "java/lang/Double", "longBitsToDouble", NULL, Double_longBitsToDouble },
    { "java/lang/Math", NULL, NULL, math },
    { "java/lang/StrictMath", NULL, NULL, math },
    /* android.system.OsConstants keeps Linux's numbers, which the library fills in: here they stay zero, and nothing above asks. */
    { "android/system/OsConstants", "initConstants", NULL, nothing },
    { "java/io/FileDescriptor", "isSocket", NULL, zero },
    { "java/io/FileDescriptor", "sync", NULL, nothing },
    { NULL, NULL, NULL, NULL },
};

void hk_dvm_art_host_start(void) { hk_dvm_register_fallback(k_host); }
