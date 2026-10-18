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
 *   hk_tests --dvm-bench <dir> [<android root> [milliseconds]]
 *       how fast the interpreter is on this machine: a counting loop and calls on the built-in java.lang, or with an
 *       Android root the benchmark of hk_dvm_bench.c on its libcore, each item run for about that long (300 ms)
 *   hk_tests --apk <file>...
 *       what the layer makes of real APKs: the report the shell shows, and the icon
 *   hk_tests --java <hello | demos | natives | crash | data | gc | gl | webview> <apk> <data dir> <android root> [picture dir | -] [verbose] [WIDTHxHEIGHT@DPI]
 *       start one of the Java test apps (core/tests/app) through megler_run on that Android root, with a stand-in
 *       for the shell's drawing; check what it logs and draws, touch it, press Back; write each picture as a BMP
 *   hk_tests --java app <apk> <data dir> <android root> <picture dir | -> <verbose> <WIDTHxHEIGHT@DPI> [tap:X,Y | drag:X,Y,X2,Y2 | wait:MS | back | key:CODE]...
 *       the same for any APK: started, its pictures written (app-1, and one more after each step of the script), nothing checked
 *   hk_tests --gfx-windows <picture dir | -> [gpu | warp | memory | copy]
 *       the shell's drawing (app/Megler/player/GfxHost.cpp: Direct2D, DirectWrite, WIC) through megler_gfx_host: scenes drawn,
 *       written as PNG files, compared with the stand-in's and checked (hk_gfx_scenes.c). On this machine's graphics processor
 *       (the default, `gpu`; and then what a lost device leaves, last), on WARP (Direct3D's software device), in memory by
 *       Direct2D's software rasterizer, or `copy`: in memory with every canvas copied to a WIC bitmap and back. Only in the
 *       build tools/build-tests.sh win makes; any other says so and does nothing
 *   hk_tests --gfx-record <hello | demos> <apk> <data dir> <android root> <record file>
 *       start one of the Java test apps through megler_run with the stand-in as the shell's drawing, and write every call it
 *       makes of that drawing to a file (hk_gfx_record.c); only where guest code can run
 *   hk_tests --gfx-replay <record file> <picture dir | -> <name> [standin | memory]
 *       draw such a record again -- by the shell's own drawing in the build tools/build-tests.sh win makes (on the graphics
 *       processor, or in memory with `memory`), by the stand-in with `standin` or in any other build -- and write each picture
 *       the app showed as <picture dir>/<name>-N.png
 *   hk_tests --storage-windows <scratch dir>
 *       the shell's storage host (app/Megler/player/StorageHost.cpp: Windows.Storage) through megler_storage_host: the tests of
 *       hk_storage_tests.c with it in the stand-in's place, on folders under the scratch folder. Only in the build
 *       tools/build-tests.sh win makes; any other says so and does nothing
 *   hk_tests --codec-windows <streams dir> <scratch dir> [speed streams dir]
 *       the shell's decoders (app/Megler/player/CodecHost.cpp: Media Foundation's decoder transforms) through
 *       megler_codec_host, under the core's codec for MediaCodec: the tests of hk_codec_tests.c on the streams in that folder
 *       (core/tests/app/data/assets), and, with a third folder, how fast they decode the streams there
 *       (tools/media-test-streams.py --speed). Only in the build tools/build-tests.sh win makes; any other says so and does
 *       nothing
 *   hk_tests --camera-windows <scratch dir>
 *       the shell's cameras (app/Megler/player/CameraHost.cpp: Windows.Media.Capture) through megler_camera_host, under the
 *       core's camera for android.hardware.Camera: the PC's first camera opened, a few pictures taken at its smallest size and
 *       checked, focus and the light where it has them, stop and close. A PC without a camera says so and has nothing to
 *       check. Only in the build tools/build-tests.sh win makes; any other says so and does nothing
 *   hk_tests --bundles <test apps dir> <scratch dir> [real bundles dir]
 *       bundles of split APKs (.apks, .xapk) unpacked as the shell unpacks them, and the APK report of what comes out
 *       (hk_bundle_tests.c): the test app `splits` in the folder of the test apps (build/guest/java), and the real bundles
 *       in the third folder where one is named
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
#include "../src/drivers/hk_java.h"
#include "../src/dvm/hk_dvm_internal.h"
#include "../src/host/hk_host.h"
#include "../src/hk_core.h"
#include "../src/jni/hk_jni.h"
#include "../src/ld/hk_ld.h"
#include "hk_codec_tests.h"
#include "hk_gfx_record.h"
#include "hk_gfx_scenes.h"
#include "hk_gfx_standin.h"
#include "hk_storage_tests.h"
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
    ref_print("methods named in one of their own classes that has none such", &own_method, 300);
    ref_print("fields named in one of their own classes that has none such", &own_field, 300);
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
    /*
     * conscrypt's library is loaded when NativeCrypto first starts (System.loadLibrary in its static initialiser), not with
     * libcore's: started here, so that where the library is in the root its natives count as answered by it. Where it is not,
     * the start fails and they are nobody's.
     */
    {
        hk_dvm_class *crypto = hk_dvm_find_class("com/android/org/conscrypt/NativeCrypto");
        if (!crypto || !hk_dvm_ensure_init(crypto)) hk_dvm_clear_pending();
    }
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
                    /* Each by its name, but for conscrypt's 280, which are one fact: its library is not there, or did not load. */
                    if (strcmp(name, "com/android/org/conscrypt/NativeCrypto")) printf("  nobody answers %s.%s%s\n", name, m->name, m->sig);
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
const megler_gfx_host *hk_gfx_windows_host(void (*line)(const char *text, void *user), void *user, int way);
void hk_gfx_windows_counts(char *out, size_t size);
int hk_gfx_windows_on_gpu(void);
void hk_gfx_windows_lose_device(void);
int hk_gfx_windows_check_present(const megler_gfx_host *h, void (*line)(const char *text, void *user), int *checks);
int hk_gfx_windows_check_gl(const megler_gfx_host *h, void (*line)(const char *text, void *user), int *checks);

/* A pixel of a 32 x 32 picture, as a check: a line when it is not what it should be. */
static int gfx_loss_pixel(const u32 *px, int x, int y, u32 want, const char *what)
{
    if (px[y * 32 + x] == want) return 0;
    printf("  FAIL after the device was lost: %s at %d,%d is %08x, not %08x\n", what, x, y, px[y * 32 + x], want);
    return 1;
}

/*
 * The graphics processor's device lost under canvases that are in use, which
 * a phone does when its driver resets: the memory keeps what the last flush
 * put there, a canvas draws on in memory, a layer that was open is put on at
 * its restore with what was drawn into it after the loss, and a shader and a
 * bitmap made before the loss still draw. Last in a run: the loss is for good.
 */
static int gfx_windows_loss(const megler_gfx_host *h, int *checks)
{
    static const u32 two[2] = { 0xFF00FF00u, 0xFF00FF00u };
    static const float ends[2] = { 0, 1 };
    static u32 px[32 * 32], picture[4 * 4], later[32 * 32];
    megler_gfx_paint p;
    megler_gfx_image image;
    void *cv, *after, *shader;
    float src[4] = { 0, 0, 4, 4 }, dst[4] = { 24, 24, 28, 28 }, corner[4] = { 28, 28, 32, 32 };
    int failed = 0, i;
    memset(&p, 0, sizeof(p));
    p.color = 0xFFFF0000u;
    p.miter_limit = 4;
    p.text_size = 12;
    p.text_scale_x = 1;
    p.blend = MEGLER_GFX_BLEND_SRC_OVER;
    p.filter_mode = -1;
    for (i = 0; i < 16; i++) picture[i] = 0xFF00FFFFu;
    memset(&image, 0, sizeof(image));
    image.pixels = picture;
    image.width = image.height = 4;
    image.stride = 16;
    image.id = 0x7FFFFFF0u;
    image.version = 1;
    printf("  the graphics processor's device lost under a canvas in use\n");
    cv = h->canvas_new(h->user, px, 32, 32, 128);
    if (!cv) {
        printf("  FAIL: no canvas\n");
        *checks += 1;
        return 1;
    }
    shader = h->shader_linear(h->user, 0, 0, 32, 0, two, ends, 2, MEGLER_GFX_TILE_CLAMP);
    h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    h->draw_rect(h->user, cv, 0, 0, 16, 32, &p, NULL);
    h->draw_bitmap(h->user, cv, &image, src, dst, &p);
    h->flush(h->user, cv);
    h->save_layer(h->user, cv, NULL, 255, MEGLER_GFX_BLEND_SRC_OVER);
    h->draw_rect(h->user, cv, 16, 16, 24, 24, &p, NULL);
    hk_gfx_windows_lose_device();
    p.color = 0xFF0000FFu;
    h->draw_rect(h->user, cv, 16, 0, 32, 8, &p, NULL);
    h->restore(h->user, cv);
    h->draw_rect(h->user, cv, 16, 8, 32, 12, &p, shader);
    h->draw_bitmap(h->user, cv, &image, src, corner, &p);
    h->flush(h->user, cv);
    failed += gfx_loss_pixel(px, 2, 2, 0xFFFF0000u, "what the last flush put into the memory");
    failed += gfx_loss_pixel(px, 25, 25, 0xFF00FFFFu, "a bitmap drawn before the loss");
    failed += gfx_loss_pixel(px, 31, 31, 0xFF00FFFFu, "a bitmap drawn after it");
    failed += gfx_loss_pixel(px, 20, 4, 0xFF0000FFu, "what was drawn into the open layer after the loss");
    failed += gfx_loss_pixel(px, 20, 10, 0xFF00FF00u, "a shader made before the loss");
    failed += gfx_loss_pixel(px, 20, 30, 0xFFFFFFFFu, "the background");
    h->canvas_free(h->user, cv);
    h->shader_free(h->user, shader);
    /* A canvas made after the loss draws in memory from the start. */
    after = h->canvas_new(h->user, later, 32, 32, 128);
    if (after) {
        h->draw_color(h->user, after, 0xFF102030u, MEGLER_GFX_BLEND_SRC);
        h->draw_rect(h->user, after, 8, 8, 16, 16, &p, NULL);
        h->flush(h->user, after);
        failed += gfx_loss_pixel(later, 10, 10, 0xFF0000FFu, "a canvas made after the loss");
        failed += gfx_loss_pixel(later, 2, 2, 0xFF102030u, "its background");
        h->canvas_free(h->user, after);
    } else {
        printf("  FAIL: no canvas after the device was lost\n");
        failed++;
    }
    h->bitmap_gone(h->user, image.id);
    *checks += 9;
    return failed;
}
#endif

static int gfx_windows_mode(int argc, char **argv)
{
#ifdef HK_GFX_WINDOWS
    static const char *const ways[] = { "on this machine's graphics processor", "in memory, every canvas copied to a WIC bitmap and back", "in memory",
                                        "on WARP, Direct3D's software device" };
    hk_gfx_scenes_config config;
    char counts[1200];
    int checks = 0, failed, way = 0;
    if (argc >= 4) way = !strcmp(argv[3], "copy") ? 1 : !strcmp(argv[3], "memory") ? 2 : !strcmp(argv[3], "warp") ? 3 : 0;
    hk_core_init();
    memset(&config, 0, sizeof(config));
    config.host = hk_gfx_windows_host(print_line, NULL, way);
    config.out_dir = strcmp(argv[2], "-") ? argv[2] : NULL;
    printf("(drawing %s)\n", ways[way]);
    failed = hk_gfx_scenes_run(&config, NULL, NULL, &checks);
    if ((way == 0 || way == 3) && hk_gfx_windows_on_gpu()) {
        failed += hk_gfx_windows_check_present(config.host, print_line, &checks);
        failed += hk_gfx_windows_check_gl(config.host, print_line, &checks);
        failed += gfx_windows_loss(config.host, &checks);
    } else if (config.host->present) {
        /* Drawn in memory there is no picture of the shell's own to show: the layer is told no, and flushes as before. */
        u32 px[16];
        void *cv = config.host->canvas_new(config.host->user, px, 4, 4, 16);
        checks++;
        if (cv && config.host->present(config.host->user, cv, 0, 0, 4, 4)) {
            printf("  FAIL: present said yes for a canvas drawn in memory\n");
            failed++;
        }
        if (cv) config.host->canvas_free(config.host->user, cv);
    }
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

/* A recorded app's drawing again: by the shell's own drawing where this build has it and it is not told otherwise (on the
   graphics processor, or `memory`), else by the stand-in that recorded it. */
static int gfx_replay_mode(int argc, char **argv)
{
    const megler_gfx_host *host = hk_gfx_standin();
    const char *dir = strcmp(argv[3], "-") ? argv[3] : NULL;
    bool same = true;
    int checks = 0, failed;
    hk_core_init();
#ifdef HK_GFX_WINDOWS
    if (argc < 6 || strcmp(argv[5], "standin")) {
        host = hk_gfx_windows_host(print_line, NULL, argc >= 6 && !strcmp(argv[5], "memory") ? 2 : 0);
        same = false;
    }
#else
    (void)argc;
#endif
    failed = hk_gfx_replay_run(argv[2], host, same, dir, argv[4], NULL, NULL, &checks);
#ifdef HK_GFX_WINDOWS
    if (!same) {
        char counts[1200];
        hk_gfx_windows_counts(counts, sizeof(counts));
        printf("  %s\n", counts);
    }
#endif
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------ --storage-windows */

#ifdef HK_STORAGE_WINDOWS
/* hk_storage_windows.cpp: the shell's own megler_storage_host, with these folders as its links. */
const megler_storage_host *hk_storage_windows_host(const char *const *names, const char *const *folders, int count);
void hk_storage_windows_summary(char *out, size_t size);
#endif

static int storage_windows_mode(char **argv)
{
#ifdef HK_STORAGE_WINDOWS
    megler_config config;
    char summary[1200];
    int checks = 0, failed;
    memset(&config, 0, sizeof(config));
    config.data_dir = argv[2];
    config.package_dir = argv[2];
    megler_init(&config);
    failed = hk_storage_tests_run_shell(argv[2], hk_storage_windows_host, NULL, NULL, &checks);
    hk_storage_windows_summary(summary, sizeof(summary));
    printf("  %s\n%d checks, %d failed\n", summary, checks, failed);
    return failed ? 1 : 0;
#else
    (void)argv;
    printf("(the shell's storage host -- Windows.Storage -- is not in this build of the tests: --storage-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* -------------------------------------------------------- --codec-windows */

#ifdef HK_CODEC_WINDOWS
/* hk_codec_windows.cpp: the shell's own megler_codec_host. */
const megler_codec_host *hk_codec_windows_host(void);
void hk_codec_windows_summary(char *out, size_t size);
void hk_codec_windows_ten_bits(int prefer);
int hk_codec_windows_call_limit(void);
const megler_media_host *hk_media_windows_host(void);

/*
 * A file of sound alone through the shell's reader of whole files (MediaHost), as drivers/hk_java_audio.c hands it what
 * MediaPlayer and SoundPool are given that is not Ogg or WAV: tone.mp3 and tone.m4a, half a second of 440 Hz at 44100 Hz
 * in one channel. Opened with no picture (width 0), the rate, the channels and the length said, and about half a second
 * of sound read, not silent.
 */
static void media_sound_check(const char *dir, const char *file, int *checks, int *failed)
{
    const megler_media_host *h = hk_media_windows_host();
    char path[1024];
    megler_media_info info;
    int16_t buf[4096];
    int64_t when = 0, frames = 0, loud = 0;
    void *media;
    int n, i;
    bool held;
    memset(&info, 0, sizeof(info));
    snprintf(path, sizeof(path), "%s\\%s", dir, file);
    media = h->open(h->user, path, &info);
    if (media) {
        while ((n = h->read_audio(h->user, media, buf, (int)(sizeof(buf) / sizeof(buf[0])) / (info.channels > 0 ? info.channels : 1), &when)) > 0) {
            for (i = 0; i < n * info.channels; i++)
                if (buf[i] > 2000 || buf[i] < -2000) loud++;
            frames += n;
        }
        h->close(h->user, media);
    }
    held = media && info.width == 0 && info.height == 0 && info.sample_rate == 44100 && info.channels == 1 && info.duration_us > 400000 &&
           info.duration_us < 700000 && frames > 44100 * 4 / 10 && frames < 44100 * 7 / 10 && loud > frames / 4;
    (*checks)++;
    if (held) {
        printf("  %s: sound alone, %d Hz in %d channels, %lld ms said, %lld frames read\n", file, info.sample_rate, info.channels,
               (long long)(info.duration_us / 1000), (long long)frames);
    } else {
        (*failed)++;
        printf("  FAIL %s: %s, %dx%d, %d Hz in %d channels, %lld ms said, %lld frames read, %lld loud samples\n", file, media ? "opened" : "not opened",
               info.width, info.height, info.sample_rate, info.channels, (long long)(info.duration_us / 1000), (long long)frames, (long long)loud);
    }
}
#endif

static int codec_windows_mode(int argc, char **argv)
{
#ifdef HK_CODEC_WINDOWS
    megler_config config;
    char summary[600];
    int checks = 0, failed;
    memset(&config, 0, sizeof(config));
    config.data_dir = argv[3];
    config.package_dir = argv[3];
    megler_init(&config);
    hk_codec_windows_summary(summary, sizeof(summary));
    printf("  Windows' decoders: %s\n", summary);
    failed = hk_codec_tests_run(argv[2], hk_codec_windows_host(), NULL, NULL, &checks);
    /* H.265 once more with P010 taken where a decoder offers it beside NV12: the way of a phone's decoder that offers P010 alone. */
    hk_codec_windows_ten_bits(1);
    failed += hk_codec_tests_h265_run(argv[2], hk_codec_windows_host(), "P010 taken where the decoder offers it", NULL, NULL, &checks);
    hk_codec_windows_ten_bits(0);
    if (argc >= 5) failed += hk_codec_speed_run(argv[4], hk_codec_windows_host(), NULL, NULL, &checks);
    /* A decoder that does not come back from a call (the phone's HEVCVideoExtension did not, from its first sample): the
       call is given up on after its limit, and the next one answers at once, so that the app's thread is not held. */
    /* Files of sound alone, which the reader of films refused ("no picture in the file") until it took them for their sound. */
    media_sound_check(argv[2], "tone.mp3", &checks, &failed);
    media_sound_check(argv[2], "tone.m4a", &checks, &failed);
    checks++;
    if (hk_codec_windows_call_limit()) {
        printf("  a call that does not return in time is given up on, and so is the next, at once\n");
    } else {
        failed++;
        printf("  FAIL: a call that does not return in time was not given up on as it should be\n");
    }
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    printf("(the shell's decoders -- Media Foundation's transforms -- are not in this build of the tests: --codec-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* ------------------------------------------------------- --camera-windows */

#ifdef HK_CAMERA_WINDOWS
/* hk_camera_windows.cpp: the shell's own megler_camera_host. */
const megler_camera_host *hk_camera_windows_host(void);
void hk_camera_windows_summary(char *out, size_t size);

static int g_cam_checks, g_cam_failed;

static void cam_check(bool ok, const char *what)
{
    g_cam_checks++;
    if (!ok) g_cam_failed++;
    printf("  %s %s\n", ok ? "ok" : "FAIL", what);
}

/* News from the camera, gathered until one of `want` comes or `ms` pass. */
static int cam_news(hk_jcamera *c, int want, int ms)
{
    int news = 0, i;
    for (i = 0; i < ms / 50 && !(news & want); i++) news |= hk_jcamera_wait(c, 50);
    return news;
}
#endif

/*
 * The PC's first camera through the shell's host and the core's camera code, as an app's Camera uses them: opened, started
 * at its smallest size, ten pictures of that size taken in order of their times, each copied as NV21 and drawn turned for a
 * view; focused once where it focuses, its light on and off where it has one; stopped, after which nothing comes; closed.
 */
static int camera_windows_mode(int argc, char **argv)
{
#ifdef HK_CAMERA_WINDOWS
    megler_config config;
    char summary[1200], what[200];
    const megler_camera_caps *caps;
    hk_jcamera *c;
    megler_camera_info info;
    int err = 0, i, w, h, frames = 0, news;
    i64 last = -1;
    bool ordered = true, copied = true, drawn = true;
    u8 *nv21, *bgra;
    (void)argc;
    memset(&config, 0, sizeof(config));
    config.data_dir = argv[2];
    config.package_dir = argv[2];
    megler_init(&config);
    hk_java_camera_set_host(hk_camera_windows_host());
    hk_camera_windows_summary(summary, sizeof(summary));
    printf("  Windows' cameras: %s\n", summary);
    if (hk_jcamera_count() == 0) {
        printf("(no camera on this PC: --camera-windows has nothing to open, and nothing is checked)\n0 checks, 0 failed\n");
        return 0;
    }
    cam_check(hk_jcamera_info(0, &info), "camera 0 is described");
    c = hk_jcamera_open(0, &err);
    if (!c && err == MEGLER_CAMERA_DENIED) {
        printf("(Windows refused this program the camera: the privacy setting for desktop apps' cameras says no; nothing more is checked)\n");
        printf("%d checks, %d failed\n", g_cam_checks, g_cam_failed);
        return g_cam_failed ? 1 : 0;
    }
    cam_check(c != NULL, "camera 0 opens");
    if (!c) {
        printf("%d checks, %d failed\n", g_cam_checks, g_cam_failed);
        return 1;
    }
    caps = hk_jcamera_caps(c);
    cam_check(caps->size_count > 0, "it offers sizes");
    if (caps->size_count > 0) {
        const megler_camera_size *s = &caps->sizes[caps->size_count - 1];
        w = s->width;
        h = s->height;
        cam_check(hk_jcamera_start(c, w, h, s->max_fps), "it streams at its smallest size");
        nv21 = (u8 *)malloc(hk_jcamera_frame_bytes(w, h, 17));
        bgra = (u8 *)malloc((size_t)w * (size_t)h * 4);
        for (i = 0; i < 100 && frames < 10; i++) {
            news = cam_news(c, HK_JCAMERA_FRAME | HK_JCAMERA_GONE, 1000);
            if (news & HK_JCAMERA_GONE) break;
            if (!(news & HK_JCAMERA_FRAME)) continue;
            frames++;
            if (hk_jcamera_size(c) != (w << 16 | h)) copied = false;
            if (hk_jcamera_time(c) <= last) ordered = false;
            last = hk_jcamera_time(c);
            if (hk_jcamera_copy(c, nv21, hk_jcamera_frame_bytes(w, h, 17), 17) != (i32)hk_jcamera_frame_bytes(w, h, 17)) copied = false;
            if (!hk_jcamera_render(c, bgra, h, w, h * 4, 90, info.facing == MEGLER_CAMERA_FRONT)) drawn = false;
        }
        if (frames > 0 && hk_jcamera_render(c, bgra, w, h, w * 4, 0, false)) {
            /* The last picture as it came, to be looked at: <scratch dir>/camera-0.bmp. */
            char path[1024];
            hk_path_mkdirs(argv[2]);
            snprintf(path, sizeof(path), "%s/camera-0.bmp", argv[2]);
            if (hk_gfx_write_bmp(path, bgra, w, h, w * 4)) printf("  the last picture: %s\n", path);
        }
        snprintf(what, sizeof(what), "ten pictures of %dx%d came (%d)", w, h, frames);
        cam_check(frames == 10, what);
        cam_check(ordered, "each taken after the one before");
        cam_check(copied, "each copied as NV21 at its size");
        cam_check(drawn, "each drawn turned for a view");
        if (caps->focus) {
            cam_check(hk_jcamera_focus(c, MEGLER_CAMERA_FOCUS_ONCE), "it is asked to focus");
            news = cam_news(c, HK_JCAMERA_FOCUSED | HK_JCAMERA_NOT_FOCUSED, 10000);
            printf("  focusing ended %s\n", news & HK_JCAMERA_FOCUSED ? "in focus" : news & HK_JCAMERA_NOT_FOCUSED ? "out of focus" : "not at all");
            cam_check((news & (HK_JCAMERA_FOCUSED | HK_JCAMERA_NOT_FOCUSED)) != 0, "focusing ends");
        } else {
            printf("  (it does not focus)\n");
        }
        if (caps->torch) {
            hk_jcamera_torch(c, true);
            hk_jcamera_torch(c, false);
        } else {
            printf("  (it has no light)\n");
        }
        hk_jcamera_stop(c);
        cam_check(!(cam_news(c, HK_JCAMERA_FRAME, 500) & HK_JCAMERA_FRAME), "stopped, nothing comes");
        free(nv21);
        free(bgra);
    }
    hk_jcamera_close(c);
    printf("%d checks, %d failed\n", g_cam_checks, g_cam_failed);
    return g_cam_failed ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    printf("(the shell's cameras -- Windows.Media.Capture -- are not in this build of the tests: --camera-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--gfx-windows")) return gfx_windows_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--storage-windows")) return storage_windows_mode(argv);
    if (argc >= 4 && !strcmp(argv[1], "--codec-windows")) return codec_windows_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--camera-windows")) return camera_windows_mode(argc, argv);
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
        if (argc > 9) hk_tests_java_set_script(argc - 9, argv + 9);
        return hk_tests_java_run(argv[2], argv[3], argv[5], argc >= 7 && strcmp(argv[6], "-") ? argv[6] : NULL, argc >= 8 ? atoi(argv[7]) : 0) ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--bundles")) {
        /* The scratch folder is the data folder too: a bundle's expansion files go into the app's storage under it. */
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[3];
        config.package_dir = argv[3];
        megler_init(&config);
        return hk_bundle_tests_run(argv[2], argv[3], argc >= 5 ? argv[4] : NULL) ? 1 : 0;
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
        hk_dvm_tests_bench(argv[2], argc >= 4 ? argv[3] : NULL, argc >= 5 ? atoi(argv[4]) : 300);
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
