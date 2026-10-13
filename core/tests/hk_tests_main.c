/*
 * The tests as a program.
 *
 *   hk_tests <guest library dir | -> <scratch dir> [--dvm <dir>] [--dvm-apk <apk>] [--dvm-trace <n>] [--dvm-android <root>]
 *       the core's tests. --dvm names the folder of the Dalvik runtime's DEX
 *       fixtures where they are not in `dvm` beside the guest libraries;
 *       --dvm-apk a real APK whose DEX is read and partly run; --dvm-trace
 *       has the runtime say what it does (1 classes, 2 calls, 3 instructions);
 *       --dvm-android runs the runtime's tests on Android's own libcore from
 *       that folder, not on the built-in java.lang
 *   hk_tests --dvm-only <fixtures dir> <scratch dir> <android root | -> [trace] [guest library dir]
 *       only the Dalvik runtime's tests: on Android's own libcore from that root
 *       (tools/dvm-android-root.sh makes one), or with "-" on the built-in java.lang
 *   hk_tests --dvm-link <android root> <dex file>...
 *       what those DEX files need of Android's libcore and do not find: the
 *       runtime is started on the boot class path of that root, the files are
 *       added, every class they define is linked, and every class, method and
 *       field their bytecode names is looked up. Nothing is run.
 *   hk_tests --dvm-init <android root> [name prefix]
 *       runs the static initialiser of every class of that root's boot class
 *       path (or of those whose names start with the prefix, "java/"), each
 *       under a limit of instructions, and says which failed and for what
 *   hk_tests --dvm-natives <android root>
 *       every native method that root's boot class path declares, and who
 *       answers for it: the runtime in C, a library of libcore's, or nobody
 *   hk_tests --dvm-bench <dir>
 *       how fast the interpreter is on this machine
 *   hk_tests --apk <file>...
 *       what the layer makes of real APKs: the report the shell shows, and the icon
 *   hk_tests --java <hello | demos> <apk> <data dir> <android root> [picture dir | -] [verbose] [WIDTHxHEIGHT@DPI]
 *       start one of the Java test apps (core/tests/app) through megler_run on that Android root, with a stand-in
 *       for the shell's drawing; check what it logs and draws, touch it, press Back; write each picture as a BMP
 *   hk_tests --gfx-windows <picture dir | -> [copy]
 *       the shell's drawing (app/Megler/player/GfxHost.cpp: Direct2D, DirectWrite, WIC) through megler_gfx_host: scenes drawn,
 *       written as PNG files, compared with the stand-in's and checked (hk_gfx_scenes.c). `copy`: with every canvas copied to a
 *       WIC bitmap and back. Only in the build tools/build-tests.sh win makes; any other says so and does nothing
 *   hk_tests --gfx-record <hello | demos> <apk> <data dir> <android root> <record file>
 *       start one of the Java test apps through megler_run with the stand-in as the shell's drawing, and write every call it
 *       makes of that drawing to a file (hk_gfx_record.c); only where guest code can run
 *   hk_tests --gfx-replay <record file> <picture dir | -> <name> [standin]
 *       draw such a record again -- by the shell's own drawing in the build tools/build-tests.sh win makes, by the stand-in with
 *       `standin` or in any other build -- and write each picture the app showed as <picture dir>/<name>-N.png
 *   hk_tests --run <apk> <data dir> [milliseconds]
 *       start the APK with a stand-in for OpenGL ES, touch it, press Back, and say what happened
 *   hk_tests --load <dir> <library>...
 *       load real Android libraries from a folder, say what each needed and what was done to it,
 *       run their constructors, and try the few functions known by name (OpenSSL's)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/megler.h"
#include "../src/dvm/hk_dvm_internal.h"
#include "../src/hk_core.h"
#include "../src/jni/hk_jni.h"
#include "../src/ld/hk_ld.h"
#include "hk_gfx_record.h"
#include "hk_gfx_scenes.h"
#include "hk_gfx_standin.h"
#include "hk_tests.h"

#if HK_HOST_ARM32
static int load_mode(int argc, char **argv)
{
    int i, bad = 0;
    if (!hk_core_init()) return 1;
    hk_ld_set_verbosity(2);
    hk_ld_add_search_dir(argv[2]);
    for (i = 3; i < argc; i++) {
        hk_lib *lib = hk_ld_load(argv[i]);
        hk_ld_stats st;
        void *fn;
        if (!lib) {
            bad++;
            continue;
        }
        hk_ld_init(lib);
        hk_ld_get_stats(lib, &st);
        printf("%s: %u Thumb, %u ARM functions; %u unresolved; %u thread-pointer reads (%u failed), %u PLT entries, %u direct system calls\n", argv[i],
               st.thumb_functions, st.arm_functions, st.unresolved, st.tp_reads, st.tp_reads_failed, st.plt_entries, st.svc_sites);
        fn = hk_ld_sym(lib, "OpenSSL_version");
        if (fn) printf("  OpenSSL_version(0): %s\n", HK_GFN(const char *(*)(int), fn)(0));
        fn = hk_ld_sym(lib, "SHA256");
        if (fn) {
            unsigned char md[32];
            int k;
            HK_GFN(unsigned char *(*)(const unsigned char *, unsigned, unsigned char *), fn)((const unsigned char *)"abc", 3, md);
            printf("  SHA256(\"abc\"): ");
            for (k = 0; k < 32; k++) printf("%02x", md[k]);
            printf("\n");
        }
        fn = hk_ld_sym(lib, "AES_set_encrypt_key");
        if (fn && hk_ld_sym(lib, "AES_encrypt")) {
            unsigned char key[16] = { 0 }, in[16] = { 0 }, out[16], sched[256];
            int k;
            HK_GFN(int (*)(const unsigned char *, int, void *), fn)(key, 128, sched);
            HK_GFN(void (*)(const unsigned char *, unsigned char *, const void *), hk_ld_sym(lib, "AES_encrypt"))(in, out, sched);
            printf("  AES-128 of zeros with a zero key: ");
            for (k = 0; k < 16; k++) printf("%02x", out[k]);
            printf("\n");
        }
        fn = hk_ld_sym(lib, "BN_new");
        if (fn && hk_ld_sym(lib, "BN_dec2bn") && hk_ld_sym(lib, "BN_bn2dec") && hk_ld_sym(lib, "BN_mul") && hk_ld_sym(lib, "BN_CTX_new")) {
            void *a = NULL, *b = NULL, *r = HK_GFN(void *(*)(void), fn)(), *ctx = HK_GFN(void *(*)(void), hk_ld_sym(lib, "BN_CTX_new"))();
            HK_GFN(int (*)(void **, const char *), hk_ld_sym(lib, "BN_dec2bn"))(&a, "123456789012345678901234567890");
            HK_GFN(int (*)(void **, const char *), hk_ld_sym(lib, "BN_dec2bn"))(&b, "987654321098765432109876543210");
            HK_GFN(int (*)(void *, void *, void *, void *), hk_ld_sym(lib, "BN_mul"))(r, a, b, ctx);
            printf("  BN_mul: %s\n", HK_GFN(char *(*)(void *), hk_ld_sym(lib, "BN_bn2dec"))(r));
        }
    }
    printf("unresolved imports in all: %u\n", (unsigned)hk_ld_unresolved_count());
    return bad ? 1 : 0;
}
#endif

/* ------------------------------------------------------------ --dvm-link */

/* Names and how often each came up; a name is the runtime's one copy of it, so two are the same when their pointers are. */
typedef struct ref_count {
    const char *what;
    int count;
} ref_count;
typedef struct ref_list {
    ref_count *v;
    int n, cap, total;
} ref_list;

static void ref_add(ref_list *l, const char *what)
{
    const char *key = hk_dvm_intern(what);
    int i;
    l->total++;
    for (i = 0; i < l->n; i++)
        if (l->v[i].what == key) {
            l->v[i].count++;
            return;
        }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = (ref_count *)hk_realloc(l->v, sizeof(*l->v) * (size_t)l->cap);
    }
    l->v[l->n].what = key;
    l->v[l->n++].count = 1;
}

static int ref_by_count(const void *a, const void *b)
{
    const ref_count *x = (const ref_count *)a, *y = (const ref_count *)b;
    return x->count != y->count ? y->count - x->count : strcmp(x->what, y->what);
}

static void ref_print(const char *title, ref_list *l, int most)
{
    int i;
    if (l->n) qsort(l->v, (size_t)l->n, sizeof(*l->v), ref_by_count);
    printf("%s: %d different, named %d times%s\n", title, l->n, l->total, l->n > most ? "; the most named:" : l->n ? ":" : "");
    for (i = 0; i < l->n && i < most; i++) printf("  %6d  %s\n", l->v[i].count, l->v[i].what);
}

/* "android/view/View$OnClickListener" -> "android/view": the package, to two levels, for a picture of where the failures are. */
static const char *package_of(const char *name, char *out, size_t n)
{
    const char *a = strchr(name, '/'), *b = a ? strchr(a + 1, '/') : NULL;
    size_t k = b ? (size_t)(b - name) : a ? (size_t)(a - name) : 0;
    if (k >= n) k = n - 1;
    memcpy(out, name, k);
    out[k] = 0;
    return k ? out : "(no package)";
}

/* A descriptor's class: arrays by what they hold; NULL for a primitive. */
static const char *class_of_descriptor(const char *desc, char *out, size_t n)
{
    while (*desc == '[') desc++;
    if (*desc != 'L' || !hk_dvm_descriptor_name(desc, out, n)) return NULL;
    return out;
}

static int link_mode(int argc, char **argv)
{
    ref_list want = { 0 }, packages = { 0 }, failed_packages = { 0 }, no_class = { 0 }, no_method = { 0 }, no_field = { 0 }, own_method = { 0 }, own_field = { 0 };
    hk_dvm_dex *files[16];
    u32 first, total, i, linked = 0, failed = 0, types = 0, methods = 0, fields = 0, unlinked_refs = 0;
    int nfiles = 0, k;
    hk_dvm_class *object;
    hk_core_init();
    hk_log_set_level(HK_LOG_ERROR);
    if (!hk_dvm_start_android(argv[2])) return 1;
    first = hk_dvm_defined_count();
    for (k = 3; k < argc; k++)
        if (!hk_dvm_add_dex_file(argv[k])) {
            printf("%s could not be read\n", argv[k]);
            return 1;
        }
    total = hk_dvm_defined_count();
    object = hk_dvm_find_class("java/lang/Object");
    printf("on the boot class path of %s (%u classes): %d DEX file(s) defining %u classes\n", argv[2], (unsigned)first, argc - 3, (unsigned)(total - first));

    /* Every class they define, linked: its superclass and interfaces found and linked, its fields laid out, its methods tabled. */
    for (i = first; i < total; i++) {
        hk_dvm_dex *d = NULL;
        const char *name = hk_dvm_defined_name(i, &d);
        char why[400], pkg[200], *cut;
        for (k = 0; k < nfiles && files[k] != d; k++) {}
        if (k == nfiles && nfiles < 16) files[nfiles++] = d;
        ref_add(&packages, package_of(name, pkg, sizeof(pkg)));
        if (hk_dvm_find_class(name)) {
            linked++;
            continue;
        }
        failed++;
        ref_add(&failed_packages, pkg);
        hk_dvm_describe_pending(why, sizeof(why));
        hk_dvm_clear_pending();
        cut = strstr(why, " (needed by");
        if (cut) *cut = 0;
        ref_add(&want, why);
    }
    printf("linked: %u; not: %u\n", (unsigned)linked, (unsigned)failed);
    ref_print("what the classes that did not link wanted", &want, 40);
    if (failed_packages.n) {
        qsort(failed_packages.v, (size_t)failed_packages.n, sizeof(*failed_packages.v), ref_by_count);
        printf("where they are (classes not linked, of how many in the package):\n");
        for (k = 0; k < failed_packages.n && k < 40; k++) {
            int j, of = 0;
            for (j = 0; j < packages.n; j++)
                if (packages.v[j].what == failed_packages.v[k].what) of = packages.v[j].count;
            printf("  %6d of %5d  %s\n", failed_packages.v[k].count, of, failed_packages.v[k].what);
        }
    }

    /* Everything their bytecode can name: each type, method and field of their tables, looked up as an instruction using it would. */
    for (k = 0; k < nfiles; k++) {
        hk_dvm_dex *d = files[k];
        char name[512];
        for (i = 0; i < d->ntype; i++) {
            const char *cls = class_of_descriptor(hk_dvm_dex_type(d, i), name, sizeof(name));
            if (!cls) continue;
            types++;
            if (!hk_dvm_class_exists(cls)) ref_add(&no_class, cls);
        }
        for (i = 0; i < d->nmeth; i++) {
            const u8 *id = d->b + d->meth_off + 8 * (size_t)i;
            const char *desc = hk_dvm_dex_type(d, hk_dvm_rd16(id)), *cls = desc[0] == '[' ? "java/lang/Object" : class_of_descriptor(desc, name, sizeof(name));
            const char *mname = hk_dvm_dex_string(d, hk_dvm_rd32(id + 4), NULL), *sig = hk_dvm_dex_proto_sig(d, hk_dvm_rd16(id + 2));
            hk_dvm_class *c;
            char text[900];
            if (!cls) continue;
            methods++;
            if (!hk_dvm_class_exists(cls)) continue;                 /* counted with the classes */
            c = hk_dvm_find_class(cls);
            if (!c) {
                hk_dvm_clear_pending();
                unlinked_refs++;
                continue;
            }
            if (hk_dvm_find_method(c, mname, sig, false) || hk_dvm_find_method(c, mname, sig, true) || hk_dvm_find_virtual(c, hk_dvm_intern(mname), hk_dvm_intern(sig)) ||
                hk_dvm_find_method(object, mname, sig, false))
                continue;
            snprintf(text, sizeof(text), "%s.%s%s", cls, mname, sig);
            ref_add(c->dex && c->dex->boot ? &no_method : &own_method, text);
        }
        for (i = 0; i < d->nfield; i++) {
            const u8 *id = d->b + d->field_off + 8 * (size_t)i;
            const char *cls = class_of_descriptor(hk_dvm_dex_type(d, hk_dvm_rd16(id)), name, sizeof(name)), *type = hk_dvm_dex_type(d, hk_dvm_rd16(id + 2));
            const char *fname = hk_dvm_dex_string(d, hk_dvm_rd32(id + 4), NULL);
            hk_dvm_class *c;
            char text[900];
            if (!cls) continue;
            fields++;
            if (!hk_dvm_class_exists(cls)) continue;
            c = hk_dvm_find_class(cls);
            if (!c) {
                hk_dvm_clear_pending();
                unlinked_refs++;
                continue;
            }
            if (hk_dvm_find_field_typed(c, fname, type, false) || hk_dvm_find_field_typed(c, fname, type, true)) continue;
            snprintf(text, sizeof(text), "%s.%s:%s", cls, fname, type);
            ref_add(c->dex && c->dex->boot ? &no_field : &own_field, text);
        }
    }
    printf("their tables name %u classes, %u methods and %u fields (%u of the methods and fields are of classes that did not link)\n", (unsigned)types, (unsigned)methods,
           (unsigned)fields, (unsigned)unlinked_refs);
    ref_print("classes named and defined nowhere", &no_class, 60);
    ref_print("methods named in a class of the boot class path that has none such", &no_method, 80);
    ref_print("fields named in a class of the boot class path that has none such", &no_field, 40);
    ref_print("methods named in one of their own classes that has none such", &own_method, 15);
    ref_print("fields named in one of their own classes that has none such", &own_field, 15);
    return 0;
}

/* ------------------------------------------------------------ --dvm-init */

/* What an initialiser's failure comes down to: the exception under the wrappers, and for a native method that nobody answers, its class alone. */
static void root_cause(char *out, size_t n)
{
    hk_jobj *e = hk_dvm_pending();
    hk_jvalue v;
    int depth;
    for (depth = 0; e && depth < 8; depth++) {
        v.j = 0;
        if (hk_dvm_instance_of(e, "java/lang/ExceptionInInitializerError") && hk_dvm_get_field(e, "exception", &v) && v.l) e = v.l;
        else if (hk_dvm_get_field(e, "cause", &v) && v.l && v.l != e) e = v.l;
        else break;
    }
    if (!e) {
        hk_strlcpy(out, "(nothing pending)", n);
        return;
    }
    v.j = 0;
    hk_dvm_get_field(e, "detailMessage", &v);
    snprintf(out, n, "%s: %s", hk_dvm_class_name(e), v.l ? hk_dvm_string_utf8(v.l) : "(no message)");
    if (!strncmp(out, "java/lang/UnsatisfiedLinkError: No implementation found for ", 60)) {
        /* "...found for libcore/icu/NativeConverter.openConverter(...)": the class is what tells whose library is missing. */
        char *dot = strchr(out + 60, '.');
        if (dot) *dot = 0;
        memmove(out + 32, out + 60, strlen(out + 60) + 1);
        memcpy(out, "no native code for a method of: ", 32);
    } else if (!strncmp(out, "java/lang/NoClassDefFoundError: Could not initialize class ", 59)) {
        hk_strlcpy(out, "(it needs a class whose own initialiser failed)", n);
    }
}

static int init_mode(int argc, char **argv)
{
    ref_list why = { 0 };
    const char *prefix = argc >= 4 ? argv[3] : "";
    u32 total, i, tried = 0, done = 0, failed = 0, unlinked = 0;
    hk_dvm_stats st;
    hk_core_init();
    hk_log_set_level(HK_LOG_ERROR);
    if (!hk_dvm_start_android(argv[2])) return 1;
    total = hk_dvm_defined_count();
    hk_dvm_set_step_limit(50000000);
    for (i = 0; i < total; i++) {
        hk_dvm_dex *d = NULL;
        const char *name = hk_dvm_defined_name(i, &d);
        hk_dvm_class *c;
        char text[600];
        if (!d || !d->boot || strncmp(name, prefix, strlen(prefix)) != 0) continue;
        tried++;
        c = hk_dvm_find_class(name);
        if (!c) {
            hk_dvm_clear_pending();
            unlinked++;
            continue;
        }
        if (hk_dvm_ensure_init(c)) {
            done++;
            continue;
        }
        failed++;
        root_cause(text, sizeof(text));
        hk_dvm_clear_pending();
        ref_add(&why, text);
        if (argc >= 5) printf("  %s: %s\n", name, text);
    }
    hk_dvm_set_step_limit(0);
    hk_dvm_get_stats(&st);
    printf("the boot class path of %s, libcore's native libraries %s: of %u classes%s%s, %u initialised, %u did not, %u did not link\n", argv[2],
           hk_dvm_libcore_natives() ? "loaded" : "not loaded", (unsigned)tried, prefix[0] ? " named " : "", prefix, (unsigned)done, (unsigned)failed, (unsigned)unlinked);
    printf("(%u classes initialised in all, %u objects, %u KB)\n", (unsigned)st.classes_initialised, (unsigned)st.objects, (unsigned)(st.object_bytes >> 10));
    ref_print("what the failures come down to", &why, 60);
    return 0;
}

/* --------------------------------------------------------- --dvm-natives */

static int natives_mode(char **argv)
{
    ref_list nobody = { 0 };
    u32 total, i, classes = 0, in_c = 0, in_library = 0, none = 0;
    i32 registered = 0, unknown = 0, by_name = 0;
    hk_core_init();
    hk_log_set_level(HK_LOG_ERROR);
    if (!hk_dvm_start_android(argv[2])) return 1;
    total = hk_dvm_defined_count();
    for (i = 0; i < total; i++) {
        hk_dvm_dex *d = NULL;
        const char *name = hk_dvm_defined_name(i, &d);
        hk_dvm_class *c = d && d->boot ? hk_dvm_find_class(name) : NULL;
        int pass, k, had = 0;
        if (!c) {
            hk_dvm_clear_pending();
            continue;
        }
        for (pass = 0; pass < 2; pass++) {
            hk_dvm_method *list = pass ? c->virt : c->direct;
            int n = pass ? c->nvirt : c->ndirect;
            for (k = 0; k < n; k++) {
                hk_dvm_method *m = &list[k];
                if (!(m->flags & DVM_ACC_NATIVE)) continue;
                had = 1;
                if (!hk_dvm_bind_native(m)) {
                    hk_dvm_clear_pending();
                    none++;
                    ref_add(&nobody, name);
                } else if (m->native) {
                    in_c++;
                } else {
                    in_library++;
                }
            }
        }
        classes += (u32)had;
    }
    hk_jni_native_counts(&registered, &unknown, &by_name);
    printf("the boot class path of %s, libcore's native libraries %s: %u native methods in %u classes\n", argv[2], hk_dvm_libcore_natives() ? "loaded" : "not loaded",
           (unsigned)(in_c + in_library + none), (unsigned)classes);
    printf("answered in C by the runtime (or what stands in for a library): %u; by a library: %u (%d of them found by their JNI names, the rest registered); by nobody: %u\n",
           (unsigned)in_c, (unsigned)in_library, (int)by_name, (unsigned)none);
    printf("the libraries registered %d methods; table entries that named no native method: %d\n", (int)registered, (int)unknown);
    ref_print("classes with native methods nobody answers for (and how many)", &nobody, 80);
    return 0;
}

static void print_line(const char *line, void *user)
{
    (void)user;
    printf("%s\n", line);
}

/* ---------------------------------------------------------- --gfx-windows */

#ifdef HK_GFX_WINDOWS
/* hk_gfx_windows.cpp: the shell's own megler_gfx_host, and what it counted. */
const megler_gfx_host *hk_gfx_windows_host(void (*line)(const char *text, void *user), void *user, int copying);
void hk_gfx_windows_counts(char *out, size_t size);
#endif

static int gfx_windows_mode(int argc, char **argv)
{
#ifdef HK_GFX_WINDOWS
    hk_gfx_scenes_config config;
    char counts[600];
    int checks = 0, failed, copying = argc >= 4 && !strcmp(argv[3], "copy");
    hk_core_init();
    memset(&config, 0, sizeof(config));
    config.host = hk_gfx_windows_host(print_line, NULL, copying);
    config.out_dir = strcmp(argv[2], "-") ? argv[2] : NULL;
    if (copying) printf("(every canvas copied to a WIC bitmap and back)\n");
    failed = hk_gfx_scenes_run(&config, NULL, NULL, &checks);
    hk_gfx_windows_counts(counts, sizeof(counts));
    printf("  %s\n%d checks, %d failed\n", counts, checks, failed);
    return failed ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    printf("(the shell's drawing -- Direct2D, DirectWrite, WIC -- is not in this build of the tests: --gfx-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* A recorded app's drawing again: by the shell's own drawing where this build has it and it is not told otherwise, else by the stand-in that recorded it. */
static int gfx_replay_mode(int argc, char **argv)
{
    const megler_gfx_host *host = hk_gfx_standin();
    const char *dir = strcmp(argv[3], "-") ? argv[3] : NULL;
    bool same = true;
    int checks = 0, failed;
    hk_core_init();
#ifdef HK_GFX_WINDOWS
    if (argc < 6 || strcmp(argv[5], "standin")) {
        host = hk_gfx_windows_host(print_line, NULL, 0);
        same = false;
    }
#else
    (void)argc;
#endif
    failed = hk_gfx_replay_run(argv[2], host, same, dir, argv[4], NULL, NULL, &checks);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--gfx-windows")) return gfx_windows_mode(argc, argv);
    if (argc >= 5 && !strcmp(argv[1], "--gfx-replay")) return gfx_replay_mode(argc, argv);
    if (argc >= 7 && !strcmp(argv[1], "--gfx-record")) {
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[4];
        config.package_dir = argv[4];
        config.log = hk_gfx_record_log;
        megler_init(&config);
        return hk_gfx_record_run(argv[2], argv[3], argv[5], argv[6]);
    }
    if (argc >= 6 && !strcmp(argv[1], "--java")) {
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[4];
        config.package_dir = argv[4];
        config.log = hk_java_tests_log;
        megler_init(&config);
        if (argc >= 9) {
            int w = 0, h = 0, dpi = 0;
            if (sscanf(argv[8], "%dx%d@%d", &w, &h, &dpi) == 3 && w > 0 && h > 0 && dpi >= 160) hk_tests_java_set_screen(w, h, dpi);
        }
        return hk_tests_java_run(argv[2], argv[3], argv[5], argc >= 7 && strcmp(argv[6], "-") ? argv[6] : NULL, argc >= 8 ? atoi(argv[7]) : 0) ? 1 : 0;
    }
#if HK_HOST_ARM32
    if (argc >= 4 && !strcmp(argv[1], "--load")) return load_mode(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "--run")) {
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[3];
        config.package_dir = argv[3];
        megler_init(&config);
        return hk_tests_smoke_run(argv[2], argc >= 5 ? atoi(argv[4]) : 1500);
    }
#endif
    if (argc >= 3 && !strcmp(argv[1], "--probe")) {
        /* The device probe, as the app's Device checks runs it; argv[2] is where its step log goes. */
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[2];
        config.package_dir = argv[2];
        megler_init(&config);
        megler_probe(print_line, NULL);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--apk")) {
        /* What the layer makes of a real APK: its report, and whether an icon was found. */
        int i;
        hk_core_init();
        for (i = 2; i < argc; i++) {
            char *report = megler_apk_report(argv[i]);
            uint8_t *icon = NULL;
            size_t icon_size = 0;
            printf("%s\n%s\n", argv[i], report ? report : "(no report)");
            if (megler_apk_icon(argv[i], &icon, &icon_size)) printf("icon: %u bytes, starts %02x %02x %02x %02x\n", (unsigned)icon_size, icon[0], icon[1], icon[2], icon[3]);
            else printf("icon: none found\n");
            megler_free(icon);
            megler_free(report);
        }
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "--dvm-only")) {
        /* Only the Dalvik runtime's tests: <fixtures> <scratch> <android root | -> [trace] [guest library dir]. */
        hk_dvm_test_config config;
        int checks = 0, failed;
        hk_core_init();
        memset(&config, 0, sizeof(config));
        config.fixtures = argv[2];
        config.scratch = argv[3];
        config.android_root = argv[4][0] == '-' && !argv[4][1] ? NULL : argv[4];
        config.trace = argc >= 6 ? atoi(argv[5]) : 0;
        config.guest_dir = argc >= 7 ? argv[6] : NULL;
        failed = hk_dvm_tests_run(&config, NULL, NULL, &checks);
        printf("%d checks, %d failed\n", checks, failed);
        return failed ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--dvm-link")) return link_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--dvm-init")) return init_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--dvm-natives")) return natives_mode(argv);
    if (argc >= 3 && !strcmp(argv[1], "--dvm-bench")) {
        hk_core_init();
        hk_dvm_tests_bench(argv[2]);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: %s <guest library dir | -> <scratch dir>\n       %s --load <dir> <library>...\n", argv[0], argv[0]);
        return 2;
    }
    {
        const char *fixtures = NULL, *apk = NULL;
        int trace = 0, i;
        for (i = 3; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--dvm")) fixtures = argv[i + 1];
            else if (!strcmp(argv[i], "--dvm-apk")) apk = argv[i + 1];
            else if (!strcmp(argv[i], "--dvm-trace")) trace = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "--dvm-android")) hk_tests_set_dvm_android(argv[i + 1]);
        }
        hk_tests_set_dvm(fixtures, apk, trace);
    }
    {
        /* The app's data lives under the scratch directory, as it lives under LocalState on the phone. */
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[2];
        config.package_dir = argv[1];
        megler_init(&config);
    }
    return hk_tests_run(argv[1][0] == '-' && !argv[1][1] ? NULL : argv[1], argv[2], NULL, NULL) ? 1 : 0;
}
