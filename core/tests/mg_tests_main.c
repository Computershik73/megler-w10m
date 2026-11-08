/*
 * The tests as a program.
 *
 *   mg_tests <guest library dir | -> <scratch dir> [--dvm <dir>] [--dvm-apk <apk>] [--dvm-trace <n>] [--dvm-android <root>]
 *       the core's tests. --dvm names the folder of the Dalvik runtime's DEX
 *       fixtures where they are not in `dvm` beside the guest libraries;
 *       --dvm-apk a real APK whose DEX is read and partly run; --dvm-trace
 *       has the runtime say what it does (1 classes, 2 calls, 3 instructions);
 *       --dvm-android runs the runtime's tests on Android's own libcore from
 *       that folder, not on the built-in java.lang
 *   mg_tests --dvm-only <fixtures dir> <scratch dir> <android root | -> [trace] [guest library dir]
 *       only the Dalvik runtime's tests: on Android's own libcore from that root
 *       (tools/dvm-android-root.sh makes one), or with "-" on the built-in java.lang
 *   mg_tests --dvm-link <android root> <dex file>...
 *       what those DEX files need of Android's libcore and do not find: the
 *       runtime is started on the boot class path of that root, the files are
 *       added, every class they define is linked, and every class, method and
 *       field their bytecode names is looked up. Nothing is run.
 *   mg_tests --dvm-init <android root> [name prefix]
 *       runs the static initialiser of every class of that root's boot class
 *       path (or of those whose names start with the prefix, "java/"), each
 *       under a limit of instructions, and says which failed and for what
 *   mg_tests --dvm-natives <android root>
 *       every native method that root's boot class path declares, and who
 *       answers for it: the runtime in C, a library of libcore's, or nobody
 *   mg_tests --dvm-bench <dir> [<android root> [milliseconds]]
 *       how fast the interpreter is on this machine: a counting loop and calls on the built-in java.lang, or with an
 *       Android root the benchmark of mg_dvm_bench.c on its libcore, each item run for about that long (300 ms)
 *   mg_tests --apk <file>...
 *       what the layer makes of real APKs: the report the shell shows, and the icon
 *   mg_tests --java <hello | demos | natives | crash | data | gc | gl | webview | screen> <apk> <data dir> <android root> [picture dir | -] [verbose] [WIDTHxHEIGHT@DPI]
 *       start one of the Java test apps (core/tests/app) through megler_run on that Android root, with a stand-in
 *       for the shell's drawing; check what it logs and draws, touch it, press Back; write each picture as a BMP
 *   mg_tests --java app <apk> <data dir> <android root> <picture dir | -> <verbose> <WIDTHxHEIGHT@DPI> [tap:X,Y | drag:X,Y,X2,Y2 | wait:MS | back | key:CODE]...
 *       the same for any APK: started, its pictures written (app-1, and one more after each step of the script), nothing checked
 *   mg_tests --gfx-windows <picture dir | -> [gpu | warp | memory | copy]
 *       the shell's drawing (app/Megler/player/GfxHost.cpp: Direct2D, DirectWrite, WIC) through megler_gfx_host: scenes drawn,
 *       written as PNG files, compared with the stand-in's and checked (mg_gfx_scenes.c). On this machine's graphics processor
 *       (the default, `gpu`; and then what a lost device leaves, last), on WARP (Direct3D's software device), in memory by
 *       Direct2D's software rasterizer, or `copy`: in memory with every canvas copied to a WIC bitmap and back. Only in the
 *       build tools/build-tests.sh win makes; any other says so and does nothing
 *   mg_tests --gfx-record <hello | demos> <apk> <data dir> <android root> <record file>
 *       start one of the Java test apps through megler_run with the stand-in as the shell's drawing, and write every call it
 *       makes of that drawing to a file (mg_gfx_record.c); only where guest code can run
 *   mg_tests --gfx-replay <record file> <picture dir | -> <name> [standin | memory]
 *       draw such a record again -- by the shell's own drawing in the build tools/build-tests.sh win makes (on the graphics
 *       processor, or in memory with `memory`), by the stand-in with `standin` or in any other build -- and write each picture
 *       the app showed as <picture dir>/<name>-N.png
 *   mg_tests --storage-windows <scratch dir>
 *       the shell's storage host (app/Megler/player/StorageHost.cpp: Windows.Storage) through megler_storage_host: the tests of
 *       mg_storage_tests.c with it in the stand-in's place, on folders under the scratch folder. Only in the build
 *       tools/build-tests.sh win makes; any other says so and does nothing
 *   mg_tests --codec-windows <streams dir> <scratch dir> [speed streams dir]
 *       the shell's decoders (app/Megler/player/CodecHost.cpp: Media Foundation's decoder transforms) through
 *       megler_codec_host, under the core's codec for MediaCodec: the tests of mg_codec_tests.c on the streams in that folder
 *       (core/tests/app/data/assets), and, with a third folder, how fast they decode the streams there
 *       (tools/media-test-streams.py --speed). Only in the build tools/build-tests.sh win makes; any other says so and does
 *       nothing
 *   mg_tests --camera-windows <scratch dir>
 *       the shell's cameras (app/Megler/player/CameraHost.cpp: Windows.Media.Capture) through megler_camera_host, under the
 *       core's camera for android.hardware.Camera: the PC's first camera opened, a few pictures taken at its smallest size and
 *       checked, focus and the light where it has them, stop and close. A PC without a camera says so and has nothing to
 *       check. Only in the build tools/build-tests.sh win makes; any other says so and does nothing
 *   mg_tests --sensors-windows <scratch dir>
 *       the shell's sensors (app/Megler/player/SensorHost.cpp: Windows.Devices.Sensors) through megler_sensor_host, under the
 *       core's sensors for SensorManager and ASensorManager: what this PC has, each read for a moment, its first event said as
 *       Android's. A PC without sensors says so and has nothing to check. Only in the build tools/build-tests.sh win makes;
 *       any other says so and does nothing
 *   mg_tests --bluetooth-windows <scratch dir>
 *       the shell's Bluetooth (app/Megler/player/BluetoothHost.cpp: Windows.Devices.Bluetooth) through megler_bluetooth_host,
 *       under the core's for android.bluetooth: this PC's radio, the devices it is paired with, and a passive scan of three
 *       seconds -- nothing sent, connected to or paired with -- its advertisements counted, each device's first said. A PC
 *       without Bluetooth, or with the radio off, says so and has nothing to check. Only in the build tools/build-tests.sh win
 *       makes; any other says so and does nothing
 *   mg_tests --portal-windows <address> <user> <password> <scratch dir> <received dir> <mock's log>
 *       the Xbox build's client of the console's Device Portal (app/Megler/shell/DevicePortal.cpp) against a portal of the
 *       tests' own at that address (tools/device-portal-mock.py, which tools/build-tests.sh win starts): its handshake, user
 *       name and password, CSRF token, an install, a refused one, the list of the packages, an uninstall. Only in the build
 *       tools/build-tests.sh win makes; any other says so and does nothing
 *   mg_tests --bundles <test apps dir> <scratch dir> [real bundles dir]
 *       bundles of split APKs (.apks, .xapk, .apkm) unpacked as the shell unpacks them, and the APK report of what comes out
 *       (mg_bundle_tests.c): the test app `splits` in the folder of the test apps (build/guest/java), and the real bundles
 *       in the third folder where one is named
 *   mg_tests --icon <apk> <out file> [drawable]
 *       the APK's icon as megler_apk_icon gives it, written to the file: a picture of the APK's, or one drawn from its
 *       drawables (core/src/apk/mg_icon.c: an adaptive icon, a vector), to look at; with a drawable's path in the APK
 *       (res/mipmap-anydpi-v26/ic_launcher.xml), that one drawn as an icon instead
 *   mg_tests --run <apk> <data dir> [milliseconds]
 *       start the APK with a stand-in for OpenGL ES, touch it, press Back, and say what happened
 *   mg_tests --load <dir> <library>...
 *       load real Android libraries from a folder, say what each needed and what was done to it,
 *       run their constructors, and try the few functions known by name (OpenSSL's)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/megler.h"
#include "../src/apk/mg_apk.h"
#include "../src/drivers/mg_java.h"
#include "../src/dvm/mg_dvm_internal.h"
#include "../src/host/mg_host.h"
#include "../src/mg_core.h"
#include "../src/jni/mg_jni.h"
#include "../src/ld/mg_ld.h"
#include "../src/ndk/mg_sensors.h"
#include "../src/run/mg_bluetooth.h"
#include "mg_codec_tests.h"
#include "mg_gfx_record.h"
#include "mg_gfx_scenes.h"
#include "mg_gfx_standin.h"
#include "mg_storage_tests.h"
#include "mg_tests.h"

#if MG_GUEST_CODE
static int load_mode(int argc, char **argv)
{
    int i, bad = 0;
    mg_lib *last = NULL;
    if (!mg_core_init()) return 1;
    mg_ld_set_verbosity(2);
    mg_ld_add_search_dir(argv[2]);
    for (i = 3; i < argc; i++) {
        mg_lib *lib = mg_ld_load(argv[i]);
        mg_ld_stats st;
        void *fn;
        if (!lib) {
            bad++;
            continue;
        }
        mg_ld_init(lib);
        last = lib;
        mg_ld_get_stats(lib, &st);
        printf("%s: %u Thumb, %u ARM functions; %u unresolved; %u thread-pointer reads (%u failed), %u accesses through a register "
               "led out, %u PLT entries, %u direct system calls (%u led out), %u writes of x18\n", argv[i], st.thumb_functions, st.arm_functions,
               st.unresolved, st.tp_reads, st.tp_reads_failed, st.tp_led_out, st.plt_entries, st.svc_sites, st.svc_led_out, st.x18_writes);
        fn = mg_ld_sym(lib, "OpenSSL_version");
        if (fn) printf("  OpenSSL_version(0): %s\n", MG_GFN(const char *(*)(int), fn)(0));
        fn = mg_ld_sym(lib, "SHA256");
        if (fn) {
            unsigned char md[32];
            int k;
            MG_GFN(unsigned char *(*)(const unsigned char *, unsigned, unsigned char *), fn)((const unsigned char *)"abc", 3, md);
            printf("  SHA256(\"abc\"): ");
            for (k = 0; k < 32; k++) printf("%02x", md[k]);
            printf("\n");
        }
        fn = mg_ld_sym(lib, "AES_set_encrypt_key");
        if (fn && mg_ld_sym(lib, "AES_encrypt")) {
            unsigned char key[16] = { 0 }, in[16] = { 0 }, out[16], sched[256];
            int k;
            MG_GFN(int (*)(const unsigned char *, int, void *), fn)(key, 128, sched);
            MG_GFN(void (*)(const unsigned char *, unsigned char *, const void *), mg_ld_sym(lib, "AES_encrypt"))(in, out, sched);
            printf("  AES-128 of zeros with a zero key: ");
            for (k = 0; k < 16; k++) printf("%02x", out[k]);
            printf("\n");
        }
        fn = mg_ld_sym(lib, "BN_new");
        if (fn && mg_ld_sym(lib, "BN_dec2bn") && mg_ld_sym(lib, "BN_bn2dec") && mg_ld_sym(lib, "BN_mul") && mg_ld_sym(lib, "BN_CTX_new")) {
            void *a = NULL, *b = NULL, *r = MG_GFN(void *(*)(void), fn)(), *ctx = MG_GFN(void *(*)(void), mg_ld_sym(lib, "BN_CTX_new"))();
            MG_GFN(int (*)(void **, const char *), mg_ld_sym(lib, "BN_dec2bn"))(&a, "123456789012345678901234567890");
            MG_GFN(int (*)(void **, const char *), mg_ld_sym(lib, "BN_dec2bn"))(&b, "987654321098765432109876543210");
            MG_GFN(int (*)(void *, void *, void *, void *), mg_ld_sym(lib, "BN_mul"))(r, a, b, ctx);
            printf("  BN_mul: %s\n", MG_GFN(char *(*)(void *), mg_ld_sym(lib, "BN_bn2dec"))(r));
        }
    }
    printf("unresolved imports in all: %u\n", (unsigned)mg_ld_unresolved_count());
    /* MG_TESTS_LOAD_WAIT_MS: a while longer for what the libraries' init functions started on threads of their own. Go's
       runtime, loaded as a library, starts itself on one (_rt0_arm_lib), and the process ended before it ran. */
    if (getenv("MG_TESTS_LOAD_WAIT_MS")) mg_sleep_ns((u64)atoi(getenv("MG_TESTS_LOAD_WAIT_MS")) * 1000000u);
    {
        /* (ARM svc of a library of Go's, led out to the core: how many were made, and how many of them on goroutines' stacks.) */
        u32 made = 0, on_block = 0;
        mg_core_svc_calls(&made, &on_block);
        if (made) printf("system calls led out: %u made, %u of them on a stack for them (not the thread's own)\n", (unsigned)made, (unsigned)on_block);
    }
    /* MG_TESTS_LOAD_CALL=NAME[:ARG][,NAME[:ARG]...]: functions of the last library loaded, as `long NAME(long ARG)`, called on
       this thread after the wait, and what each returned -- a Go library's exports, which come into its runtime from a
       thread it did not make (cgocallback, needm). */
    if (getenv("MG_TESTS_LOAD_CALL") && last) {
        const char *at = getenv("MG_TESTS_LOAD_CALL");
        while (*at) {
            char name[128], *end;
            size_t n = strcspn(at, ":,");
            long arg = 0;
            void *fn;
            if (!n || n >= sizeof(name)) break;
            memcpy(name, at, n);
            name[n] = 0;
            at += n;
            if (*at == ':') {
                arg = strtol(at + 1, &end, 0);
                at = end;
            }
            fn = mg_ld_sym(last, name);
            if (fn) printf("%s(%ld): %ld\n", name, arg, MG_GFN(long (*)(long), fn)(arg));
            else printf("%s: not in the library\n", name);
            if (*at == ',') at++;
        }
    }
    /* MG_TESTS_LOAD_PEEK=OFFSET[,OFFSET...] (hex): the 32-bit words at those offsets of the last library loaded, as they are
       after its init functions and the wait -- variables of its, such as Go's runtime.vdsoClockgettimeSym, which says whether
       Go took the vDSO. */
    if (getenv("MG_TESTS_LOAD_PEEK") && last) {
        const char *at = getenv("MG_TESTS_LOAD_PEEK");
        while (*at) {
            char *end;
            unsigned long off = strtoul(at, &end, 16);
            u32 word;
            if (end == at) break;
            memcpy(&word, (u8 *)mg_ld_lib_base(last) + off, 4);
            printf("word at +%lx: %08x (base %p)\n", off, (unsigned)word, mg_ld_lib_base(last));
            at = *end == ',' ? end + 1 : end;
        }
    }
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
    const char *key = mg_dvm_intern(what);
    int i;
    l->total++;
    for (i = 0; i < l->n; i++)
        if (l->v[i].what == key) {
            l->v[i].count++;
            return;
        }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = (ref_count *)mg_realloc(l->v, sizeof(*l->v) * (size_t)l->cap);
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
    if (*desc != 'L' || !mg_dvm_descriptor_name(desc, out, n)) return NULL;
    return out;
}

static int link_mode(int argc, char **argv)
{
    ref_list want = { 0 }, packages = { 0 }, failed_packages = { 0 }, no_class = { 0 }, no_method = { 0 }, no_field = { 0 }, own_method = { 0 }, own_field = { 0 };
    mg_dvm_dex *files[16];
    u32 first, total, i, linked = 0, failed = 0, types = 0, methods = 0, fields = 0, unlinked_refs = 0;
    int nfiles = 0, k;
    mg_dvm_class *object;
    mg_core_init();
    mg_log_set_level(MG_LOG_ERROR);
    if (!mg_dvm_start_android(argv[2])) return 1;
    first = mg_dvm_defined_count();
    for (k = 3; k < argc; k++)
        if (!mg_dvm_add_dex_file(argv[k])) {
            printf("%s could not be read\n", argv[k]);
            return 1;
        }
    total = mg_dvm_defined_count();
    object = mg_dvm_find_class("java/lang/Object");
    printf("on the boot class path of %s (%u classes): %d DEX file(s) defining %u classes\n", argv[2], (unsigned)first, argc - 3, (unsigned)(total - first));

    /* Every class they define, linked: its superclass and interfaces found and linked, its fields laid out, its methods tabled. */
    for (i = first; i < total; i++) {
        mg_dvm_dex *d = NULL;
        const char *name = mg_dvm_defined_name(i, &d);
        char why[400], pkg[200], *cut;
        for (k = 0; k < nfiles && files[k] != d; k++) {}
        if (k == nfiles && nfiles < 16) files[nfiles++] = d;
        ref_add(&packages, package_of(name, pkg, sizeof(pkg)));
        if (mg_dvm_find_class(name)) {
            linked++;
            continue;
        }
        failed++;
        ref_add(&failed_packages, pkg);
        mg_dvm_describe_pending(why, sizeof(why));
        mg_dvm_clear_pending();
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
        mg_dvm_dex *d = files[k];
        char name[512];
        bool ready;
        /* (A file is viewed once a class of it is needed: here, all of it, now.) */
        mg_dvm_lock();
        ready = mg_dvm_dex_ready(d);
        mg_dvm_unlock();
        if (!ready || !d->b) continue;
        for (i = 0; i < d->ntype; i++) {
            const char *cls = class_of_descriptor(mg_dvm_dex_type(d, i), name, sizeof(name));
            if (!cls) continue;
            types++;
            if (!mg_dvm_class_exists(cls)) ref_add(&no_class, cls);
        }
        for (i = 0; i < d->nmeth; i++) {
            const u8 *id = d->b + d->meth_off + 8 * (size_t)i;
            const char *desc = mg_dvm_dex_type(d, mg_dvm_rd16(id)), *cls = desc[0] == '[' ? "java/lang/Object" : class_of_descriptor(desc, name, sizeof(name));
            const char *mname = mg_dvm_dex_string(d, mg_dvm_rd32(id + 4), NULL), *sig = mg_dvm_dex_proto_sig(d, mg_dvm_rd16(id + 2));
            mg_dvm_class *c;
            char text[900];
            if (!cls) continue;
            methods++;
            if (!mg_dvm_class_exists(cls)) continue;                 /* counted with the classes */
            c = mg_dvm_find_class(cls);
            if (!c) {
                mg_dvm_clear_pending();
                unlinked_refs++;
                continue;
            }
            if (mg_dvm_find_method(c, mname, sig, false) || mg_dvm_find_method(c, mname, sig, true) || mg_dvm_find_virtual(c, mg_dvm_intern(mname), mg_dvm_intern(sig)) ||
                mg_dvm_find_method(object, mname, sig, false))
                continue;
            snprintf(text, sizeof(text), "%s.%s%s", cls, mname, sig);
            ref_add(c->dex && c->dex->boot ? &no_method : &own_method, text);
        }
        for (i = 0; i < d->nfield; i++) {
            const u8 *id = d->b + d->field_off + 8 * (size_t)i;
            const char *cls = class_of_descriptor(mg_dvm_dex_type(d, mg_dvm_rd16(id)), name, sizeof(name)), *type = mg_dvm_dex_type(d, mg_dvm_rd16(id + 2));
            const char *fname = mg_dvm_dex_string(d, mg_dvm_rd32(id + 4), NULL);
            mg_dvm_class *c;
            char text[900];
            if (!cls) continue;
            fields++;
            if (!mg_dvm_class_exists(cls)) continue;
            c = mg_dvm_find_class(cls);
            if (!c) {
                mg_dvm_clear_pending();
                unlinked_refs++;
                continue;
            }
            if (mg_dvm_find_field_typed(c, fname, type, false) || mg_dvm_find_field_typed(c, fname, type, true)) continue;
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
    mg_jobj *e = mg_dvm_pending();
    mg_jvalue v;
    int depth;
    for (depth = 0; e && depth < 8; depth++) {
        v.j = 0;
        if (mg_dvm_instance_of(e, "java/lang/ExceptionInInitializerError") && mg_dvm_get_field(e, "exception", &v) && v.l) e = v.l;
        else if (mg_dvm_get_field(e, "cause", &v) && v.l && v.l != e) e = v.l;
        else break;
    }
    if (!e) {
        mg_strlcpy(out, "(nothing pending)", n);
        return;
    }
    v.j = 0;
    mg_dvm_get_field(e, "detailMessage", &v);
    snprintf(out, n, "%s: %s", mg_dvm_class_name(e), v.l ? mg_dvm_string_utf8(v.l) : "(no message)");
    if (!strncmp(out, "java/lang/UnsatisfiedLinkError: No implementation found for ", 60)) {
        /* "...found for libcore/icu/NativeConverter.openConverter(...)": the class is what tells whose library is missing. */
        char *dot = strchr(out + 60, '.');
        if (dot) *dot = 0;
        memmove(out + 32, out + 60, strlen(out + 60) + 1);
        memcpy(out, "no native code for a method of: ", 32);
    } else if (!strncmp(out, "java/lang/NoClassDefFoundError: Could not initialize class ", 59)) {
        mg_strlcpy(out, "(it needs a class whose own initialiser failed)", n);
    }
}

static int init_mode(int argc, char **argv)
{
    ref_list why = { 0 };
    const char *prefix = argc >= 4 ? argv[3] : "";
    u32 total, i, tried = 0, done = 0, failed = 0, unlinked = 0;
    mg_dvm_stats st;
    mg_core_init();
    mg_log_set_level(MG_LOG_ERROR);
    if (!mg_dvm_start_android(argv[2])) return 1;
    total = mg_dvm_defined_count();
    mg_dvm_set_step_limit(50000000);
    for (i = 0; i < total; i++) {
        mg_dvm_dex *d = NULL;
        const char *name = mg_dvm_defined_name(i, &d);
        mg_dvm_class *c;
        char text[600];
        if (!d || !d->boot || strncmp(name, prefix, strlen(prefix)) != 0) continue;
        tried++;
        c = mg_dvm_find_class(name);
        if (!c) {
            mg_dvm_clear_pending();
            unlinked++;
            continue;
        }
        if (mg_dvm_ensure_init(c)) {
            done++;
            continue;
        }
        failed++;
        root_cause(text, sizeof(text));
        mg_dvm_clear_pending();
        ref_add(&why, text);
        if (argc >= 5) printf("  %s: %s\n", name, text);
    }
    mg_dvm_set_step_limit(0);
    mg_dvm_get_stats(&st);
    printf("the boot class path of %s, libcore's native libraries %s: of %u classes%s%s, %u initialised, %u did not, %u did not link\n", argv[2],
           mg_dvm_libcore_natives() ? "loaded" : "not loaded", (unsigned)tried, prefix[0] ? " named " : "", prefix, (unsigned)done, (unsigned)failed, (unsigned)unlinked);
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
    mg_core_init();
    mg_log_set_level(MG_LOG_ERROR);
    if (!mg_dvm_start_android(argv[2])) return 1;
    /*
     * conscrypt's library is loaded when NativeCrypto first starts (System.loadLibrary in its static initialiser), not with
     * libcore's: started here, so that where the library is in the root its natives count as answered by it. Where it is not,
     * the start fails and they are nobody's.
     */
    {
        mg_dvm_class *crypto = mg_dvm_find_class("com/android/org/conscrypt/NativeCrypto");
        if (!crypto || !mg_dvm_ensure_init(crypto)) mg_dvm_clear_pending();
    }
    total = mg_dvm_defined_count();
    for (i = 0; i < total; i++) {
        mg_dvm_dex *d = NULL;
        const char *name = mg_dvm_defined_name(i, &d);
        mg_dvm_class *c = d && d->boot ? mg_dvm_find_class(name) : NULL;
        int pass, k, had = 0;
        if (!c) {
            mg_dvm_clear_pending();
            continue;
        }
        for (pass = 0; pass < 2; pass++) {
            mg_dvm_method *list = pass ? c->virt : c->direct;
            int n = pass ? c->nvirt : c->ndirect;
            for (k = 0; k < n; k++) {
                mg_dvm_method *m = &list[k];
                if (!(m->flags & DVM_ACC_NATIVE)) continue;
                had = 1;
                if (!mg_dvm_bind_native(m)) {
                    mg_dvm_clear_pending();
                    none++;
                    ref_add(&nobody, name);
                    /* Each by its name, but for conscrypt's 280, which are one fact: its library is not there, or did not load. */
                    if (strcmp(name, "com/android/org/conscrypt/NativeCrypto")) printf("  nobody answers %s.%s%s\n", name, m->name, m->sig);
                } else if (m->nat && m->nat->native) {
                    in_c++;
                } else {
                    in_library++;
                }
            }
        }
        classes += (u32)had;
    }
    mg_jni_native_counts(&registered, &unknown, &by_name);
    printf("the boot class path of %s, libcore's native libraries %s: %u native methods in %u classes\n", argv[2], mg_dvm_libcore_natives() ? "loaded" : "not loaded",
           (unsigned)(in_c + in_library + none), (unsigned)classes);
    printf("answered in C by the runtime (or what stands in for a library): %u; by a library: %u (%d of them found by their JNI names, the rest registered); by nobody: %u\n",
           (unsigned)in_c, (unsigned)in_library, (int)by_name, (unsigned)none);
    printf("the libraries registered %d methods; table entries that named no native method: %d\n", (int)registered, (int)unknown);
    ref_print("classes with native methods nobody answers for (and how many)", &nobody, 80);
    return 0;
}

static MEGLER_CB void print_line(const char *line, void *user)
{
    (void)user;
    printf("%s\n", line);
}

/* ---------------------------------------------------------- --gfx-windows */

#ifdef MG_GFX_WINDOWS
/* mg_gfx_windows.cpp: the shell's own megler_gfx_host, and what it counted. */
MG_HOSTAPI const megler_gfx_host *mg_gfx_windows_host(void (MEGLER_CB *line)(const char *text, void *user), void *user, int way);
MG_HOSTAPI void mg_gfx_windows_counts(char *out, size_t size);
MG_HOSTAPI int mg_gfx_windows_on_gpu(void);
MG_HOSTAPI void mg_gfx_windows_lose_device(void);
MG_HOSTAPI int mg_gfx_windows_check_present(const megler_gfx_host *h, void (MEGLER_CB *line)(const char *text, void *user), int *checks);
MG_HOSTAPI int mg_gfx_windows_check_gl(const megler_gfx_host *h, void (MEGLER_CB *line)(const char *text, void *user), int *checks);
MG_HOSTAPI int mg_gfx_windows_check_canvas(const megler_gfx_host *h, void (MEGLER_CB *line)(const char *text, void *user), int *checks);
MG_HOSTAPI int mg_gfx_windows_check_video(const megler_gfx_host *h, void (MEGLER_CB *line)(const char *text, void *user), int *checks);

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
    mg_gfx_windows_lose_device();
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
#ifdef MG_GFX_WINDOWS
    static const char *const ways[] = { "on this machine's graphics processor", "in memory, every canvas copied to a WIC bitmap and back", "in memory",
                                        "on WARP, Direct3D's software device" };
    mg_gfx_scenes_config config;
    char counts[1200];
    int checks = 0, failed, way = 0;
    if (argc >= 4) way = !strcmp(argv[3], "copy") ? 1 : !strcmp(argv[3], "memory") ? 2 : !strcmp(argv[3], "warp") ? 3 : 0;
    mg_core_init();
    memset(&config, 0, sizeof(config));
    config.host = mg_gfx_windows_host(print_line, NULL, way);
    config.out_dir = strcmp(argv[2], "-") ? argv[2] : NULL;
    printf("(drawing %s)\n", ways[way]);
    failed = mg_gfx_scenes_run(&config, NULL, NULL, &checks);
    if ((way == 0 || way == 3) && mg_gfx_windows_on_gpu()) {
        failed += mg_gfx_windows_check_present(config.host, print_line, &checks);
        failed += mg_gfx_windows_check_canvas(config.host, print_line, &checks);
        failed += mg_gfx_windows_check_gl(config.host, print_line, &checks);
        failed += mg_gfx_windows_check_video(config.host, print_line, &checks);
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
    mg_gfx_windows_counts(counts, sizeof(counts));
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
    const megler_gfx_host *host = mg_gfx_standin();
    const char *dir = strcmp(argv[3], "-") ? argv[3] : NULL;
    bool same = true;
    int checks = 0, failed;
    mg_core_init();
#ifdef MG_GFX_WINDOWS
    if (argc < 6 || strcmp(argv[5], "standin")) {
        host = mg_gfx_windows_host(print_line, NULL, argc >= 6 && !strcmp(argv[5], "memory") ? 2 : 0);
        same = false;
    }
#else
    (void)argc;
#endif
    failed = mg_gfx_replay_run(argv[2], host, same, dir, argv[4], NULL, NULL, &checks);
#ifdef MG_GFX_WINDOWS
    if (!same) {
        char counts[1200];
        mg_gfx_windows_counts(counts, sizeof(counts));
        printf("  %s\n", counts);
    }
#endif
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}

/* ------------------------------------------------------ --storage-windows */

#ifdef MG_STORAGE_WINDOWS
/* mg_storage_windows.cpp: the shell's own megler_storage_host, with these folders as its links. */
MG_HOSTAPI const megler_storage_host *mg_storage_windows_host(const char *const *names, const char *const *folders, int count);
MG_HOSTAPI void mg_storage_windows_summary(char *out, size_t size);
#endif

static int storage_windows_mode(char **argv)
{
#ifdef MG_STORAGE_WINDOWS
    megler_config config;
    char summary[1200];
    int checks = 0, failed;
    memset(&config, 0, sizeof(config));
    config.data_dir = argv[2];
    config.package_dir = argv[2];
    megler_init(&config);
    failed = mg_storage_tests_run_shell(argv[2], mg_storage_windows_host, NULL, NULL, &checks);
    mg_storage_windows_summary(summary, sizeof(summary));
    printf("  %s\n%d checks, %d failed\n", summary, checks, failed);
    return failed ? 1 : 0;
#else
    (void)argv;
    printf("(the shell's storage host -- Windows.Storage -- is not in this build of the tests: --storage-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* -------------------------------------------------------- --codec-windows */

#ifdef MG_CODEC_WINDOWS
/* mg_codec_windows.cpp: the shell's own megler_codec_host. */
MG_HOSTAPI const megler_codec_host *mg_codec_windows_host(void);
MG_HOSTAPI void mg_codec_windows_summary(char *out, size_t size);
MG_HOSTAPI void mg_codec_windows_ten_bits(int prefer);
MG_HOSTAPI int mg_codec_windows_call_limit(void);
MG_HOSTAPI const megler_media_host *mg_media_windows_host(void);

/*
 * A file of sound alone through the shell's reader of whole files (MediaHost), as drivers/mg_java_audio.c hands it what
 * MediaPlayer and SoundPool are given that is not Ogg or WAV: tone.mp3 and tone.m4a, half a second of 440 Hz at 44100 Hz
 * in one channel. Opened with no picture (width 0), the rate, the channels and the length said, and about half a second
 * of sound read, not silent.
 */
static void media_sound_check(const char *dir, const char *file, int *checks, int *failed)
{
    const megler_media_host *h = mg_media_windows_host();
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
#ifdef MG_CODEC_WINDOWS
    megler_config config;
    char summary[600];
    int checks = 0, failed;
    memset(&config, 0, sizeof(config));
    config.data_dir = argv[3];
    config.package_dir = argv[3];
    megler_init(&config);
    mg_codec_windows_summary(summary, sizeof(summary));
    printf("  Windows' decoders: %s\n", summary);
    failed = mg_codec_tests_run(argv[2], mg_codec_windows_host(), NULL, NULL, &checks);
    /* H.265 once more with P010 taken where a decoder offers it beside NV12: the way of a phone's decoder that offers P010 alone. */
    mg_codec_windows_ten_bits(1);
    failed += mg_codec_tests_h265_run(argv[2], mg_codec_windows_host(), "P010 taken where the decoder offers it", NULL, NULL, &checks);
    mg_codec_windows_ten_bits(0);
    if (argc >= 5) failed += mg_codec_speed_run(argv[4], mg_codec_windows_host(), NULL, NULL, &checks);
#ifdef MG_GFX_WINDOWS
    {
        /* And on the graphics processor: the shell's drawing started there first, as an app's first drawing starts it, its
           device then the decoders' too (CodecHost decodes on GfxHost's device, DXVA), the pictures shown from there. */
        const megler_gfx_host *gfx = mg_gfx_windows_host(print_line, NULL, 0);
        u32 px[16];
        void *cv = gfx->canvas_new(gfx->user, px, 4, 4, 16);
        if (cv) {
            gfx->draw_color(gfx->user, cv, 0xFF000000u, MEGLER_GFX_BLEND_SRC);
            gfx->flush(gfx->user, cv);
            gfx->canvas_free(gfx->user, cv);
        }
        if (mg_gfx_windows_on_gpu()) failed += mg_codec_gpu_run(argv[2], mg_codec_windows_host(), gfx, NULL, NULL, &checks);
        else printf("  (no graphics processor to draw on here: pictures decoded on it are not tried)\n");
        if (argc >= 5 && mg_gfx_windows_on_gpu()) failed += mg_codec_speed_run(argv[4], mg_codec_windows_host(), NULL, NULL, &checks);
    }
#endif
    /* A decoder that does not come back from a call (the phone's HEVCVideoExtension did not, from its first sample): the
       call is given up on after its limit, and the next one answers at once, so that the app's thread is not held. */
    /* Files of sound alone, which the reader of films refused ("no picture in the file") until it took them for their sound. */
    media_sound_check(argv[2], "tone.mp3", &checks, &failed);
    media_sound_check(argv[2], "tone.m4a", &checks, &failed);
    checks++;
    if (mg_codec_windows_call_limit()) {
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

#ifdef MG_CAMERA_WINDOWS
/* mg_camera_windows.cpp: the shell's own megler_camera_host. */
MG_HOSTAPI const megler_camera_host *mg_camera_windows_host(void);
MG_HOSTAPI void mg_camera_windows_summary(char *out, size_t size);

static int g_cam_checks, g_cam_failed;

static void cam_check(bool ok, const char *what)
{
    g_cam_checks++;
    if (!ok) g_cam_failed++;
    printf("  %s %s\n", ok ? "ok" : "FAIL", what);
}

/* News from the camera, gathered until one of `want` comes or `ms` pass. */
static int cam_news(mg_jcamera *c, int want, int ms)
{
    int news = 0, i;
    for (i = 0; i < ms / 50 && !(news & want); i++) news |= mg_jcamera_wait(c, 50);
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
#ifdef MG_CAMERA_WINDOWS
    megler_config config;
    char summary[1200], what[200];
    const megler_camera_caps *caps;
    mg_jcamera *c;
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
    mg_java_camera_set_host(mg_camera_windows_host());
    mg_camera_windows_summary(summary, sizeof(summary));
    printf("  Windows' cameras: %s\n", summary);
    if (mg_jcamera_count() == 0) {
        printf("(no camera on this PC: --camera-windows has nothing to open, and nothing is checked)\n0 checks, 0 failed\n");
        return 0;
    }
    cam_check(mg_jcamera_info(0, &info), "camera 0 is described");
    c = mg_jcamera_open(0, &err);
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
    caps = mg_jcamera_caps(c);
    cam_check(caps->size_count > 0, "it offers sizes");
    if (caps->size_count > 0) {
        const megler_camera_size *s = &caps->sizes[caps->size_count - 1];
        w = s->width;
        h = s->height;
        cam_check(mg_jcamera_start(c, w, h, s->max_fps), "it streams at its smallest size");
        nv21 = (u8 *)malloc(mg_jcamera_frame_bytes(w, h, 17));
        bgra = (u8 *)malloc((size_t)w * (size_t)h * 4);
        for (i = 0; i < 100 && frames < 10; i++) {
            news = cam_news(c, MG_JCAMERA_FRAME | MG_JCAMERA_GONE, 1000);
            if (news & MG_JCAMERA_GONE) break;
            if (!(news & MG_JCAMERA_FRAME)) continue;
            frames++;
            if (mg_jcamera_size(c) != (w << 16 | h)) copied = false;
            if (mg_jcamera_time(c) <= last) ordered = false;
            last = mg_jcamera_time(c);
            if (mg_jcamera_copy(c, nv21, mg_jcamera_frame_bytes(w, h, 17), 17) != (i32)mg_jcamera_frame_bytes(w, h, 17)) copied = false;
            if (!mg_jcamera_render(c, bgra, h, w, h * 4, 90, info.facing == MEGLER_CAMERA_FRONT)) drawn = false;
        }
        if (frames > 0 && mg_jcamera_render(c, bgra, w, h, w * 4, 0, false)) {
            /* The last picture as it came, to be looked at: <scratch dir>/camera-0.bmp. */
            char path[1024];
            mg_path_mkdirs(argv[2]);
            snprintf(path, sizeof(path), "%s/camera-0.bmp", argv[2]);
            if (mg_gfx_write_bmp(path, bgra, w, h, w * 4)) printf("  the last picture: %s\n", path);
        }
        snprintf(what, sizeof(what), "ten pictures of %dx%d came (%d)", w, h, frames);
        cam_check(frames == 10, what);
        cam_check(ordered, "each taken after the one before");
        cam_check(copied, "each copied as NV21 at its size");
        cam_check(drawn, "each drawn turned for a view");
        if (caps->focus) {
            cam_check(mg_jcamera_focus(c, MEGLER_CAMERA_FOCUS_ONCE), "it is asked to focus");
            news = cam_news(c, MG_JCAMERA_FOCUSED | MG_JCAMERA_NOT_FOCUSED, 10000);
            printf("  focusing ended %s\n", news & MG_JCAMERA_FOCUSED ? "in focus" : news & MG_JCAMERA_NOT_FOCUSED ? "out of focus" : "not at all");
            cam_check((news & (MG_JCAMERA_FOCUSED | MG_JCAMERA_NOT_FOCUSED)) != 0, "focusing ends");
        } else {
            printf("  (it does not focus)\n");
        }
        if (caps->torch) {
            mg_jcamera_torch(c, true);
            mg_jcamera_torch(c, false);
        } else {
            printf("  (it has no light)\n");
        }
        mg_jcamera_stop(c);
        cam_check(!(cam_news(c, MG_JCAMERA_FRAME, 500) & MG_JCAMERA_FRAME), "stopped, nothing comes");
        free(nv21);
        free(bgra);
    }
    mg_jcamera_close(c);
    printf("%d checks, %d failed\n", g_cam_checks, g_cam_failed);
    return g_cam_failed ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    printf("(the shell's cameras -- Windows.Media.Capture -- are not in this build of the tests: --camera-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* ------------------------------------------------------ --sensors-windows */

#ifdef MG_SENSORS_WINDOWS
/* mg_sensors_windows.cpp: the shell's own megler_sensor_host. */
MG_HOSTAPI const megler_sensor_host *mg_sensors_windows_host(void);
MG_HOSTAPI void mg_sensors_windows_summary(char *out, size_t size);
#endif

/*
 * This PC's sensors through the shell's host and the core's, as an app's SensorManager uses them: listed, each started at
 * 100 ms and read for up to two seconds -- its first event said as Android's, the events' times going forward -- and
 * stopped. A PC without sensors says so and has nothing to check.
 */
static int sensors_windows_mode(int argc, char **argv)
{
#ifdef MG_SENSORS_WINDOWS
    megler_config config;
    char summary[1500];
    mg_sensor_queue *q;
    int checks = 0, failed = 0, i, n;
    (void)argc;
    memset(&config, 0, sizeof(config));
    config.data_dir = argv[2];
    config.package_dir = argv[2];
    megler_init(&config);
    mg_sensors_set_host(mg_sensors_windows_host());
    mg_sensors_windows_summary(summary, sizeof(summary));
    printf("  Windows' sensors: %s\n", summary);
    n = mg_sensors_count();
    if (n == 0) {
        printf("(no sensors on this PC: --sensors-windows has nothing to read, and nothing is checked)\n0 checks, 0 failed\n");
        return 0;
    }
    q = mg_sensor_queue_create("the tests", false);
    for (i = 0; i < n; i++) {
        const mg_sensor *s = mg_sensors_at(i);
        mg_sensor_event e;
        u64 until = mg_time_monotonic_ns() + 2000000000ull;
        i64 last = 0;
        int events = 0;
        bool forward = true;
        mg_sensor_queue_enable(q, s->handle, 100000);
        while (mg_time_monotonic_ns() < until) {
            if (mg_sensor_queue_read(q, &e, 1, 100) != 1) continue;
            if (e.handle != s->handle) continue;
            if (events++ == 0)
                printf("  %s (%s by %s): %.4g %.4g %.4g %.4g %.4g, accuracy %d\n", s->string_type, s->name, s->vendor, e.values[0], e.values[1], e.values[2],
                       e.values[3], e.values[4], e.accuracy);
            if (e.timestamp <= last) forward = false;
            last = e.timestamp;
        }
        mg_sensor_queue_disable(q, s->handle);
        printf("  %s %s: %d events in two seconds%s\n", events ? "ok" : "FAIL", s->string_type, events, forward ? "" : ", NOT in order");
        checks++;
        if (!events || !forward) failed++;
    }
    mg_sensor_queue_destroy(q);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    printf("(the shell's sensors -- Windows.Devices.Sensors -- are not in this build of the tests: --sensors-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* ---------------------------------------------------- --bluetooth-windows */

#ifdef MG_BLUETOOTH_WINDOWS
/* mg_bluetooth_windows.cpp: the shell's own megler_bluetooth_host. */
MG_HOSTAPI const megler_bluetooth_host *mg_bluetooth_windows_host(void);
MG_HOSTAPI void mg_bluetooth_windows_summary(char *out, size_t size);
#endif

/*
 * This PC's Bluetooth through the shell's host and the core's, as an app's android.bluetooth uses them: the radio, then a
 * passive scan of three seconds (nothing is sent: no scan request, no connection, no pairing) whose advertisements come
 * through the core's queue, each device's first said. A PC without a radio, or with it off, says so and checks nothing.
 */
static int bluetooth_windows_mode(int argc, char **argv)
{
#ifdef MG_BLUETOOTH_WINDOWS
    megler_config config;
    char summary[1500], name[128];
    int checks = 0, failed = 0, ads = 0, devices = 0, state, i;
    u64 seen[64];
    u64 until;
    (void)argc;
    memset(&config, 0, sizeof(config));
    config.data_dir = argv[2];
    config.package_dir = argv[2];
    megler_init(&config);
    mg_bluetooth_set_host(mg_bluetooth_windows_host());
    mg_bluetooth_windows_summary(summary, sizeof(summary));
    printf("  Windows' Bluetooth: %s\n", summary);
    state = mg_bluetooth_state(name, sizeof(name));
    if (state != MEGLER_BT_RADIO_ON) {
        printf("(%s: --bluetooth-windows has nothing to listen to, and nothing is checked)\n0 checks, 0 failed\n",
               state == MEGLER_BT_RADIO_OFF ? "this PC's Bluetooth radio is off" : "no Bluetooth LE radio on this PC");
        mg_bluetooth_end();
        return 0;
    }
    printf("  the radio is on, named \"%s\"; listening (passive) for three seconds\n", name);
    mg_bluetooth_scan(MEGLER_BT_SCAN_PASSIVE);
    until = mg_time_monotonic_ns() + 3000000000ull;
    while (mg_time_monotonic_ns() < until) {
        megler_bluetooth_event *e = mg_bluetooth_next(100);
        char text[18];
        if (!e) continue;
        if (e->kind == MEGLER_BT_EVENT_ADVERTISEMENT) {
            ads++;
            for (i = 0; i < devices && seen[i] != e->address; i++) {
            }
            if (i == devices && devices < 64) {
                seen[devices++] = e->address;
                mg_bluetooth_address_text(e->address, text);
                if (devices <= 8)
                    printf("  heard %s (%s), %d dBm, %d bytes of advertisement%s\n", text, e->address_type == MEGLER_BT_ADDRESS_RANDOM ? "random" : "public",
                           e->value, e->size, e->extra ? ", takes connections" : "");
            }
        } else if (e->kind == MEGLER_BT_EVENT_SCAN_FAILED) {
            printf("  the scan failed: ScanCallback's code %d\n", e->status);
            failed++;
        }
        mg_bluetooth_free(e);
    }
    mg_bluetooth_scan(MEGLER_BT_SCAN_OFF);
    mg_bluetooth_end();
    checks++;
    printf("  %s %d advertisements from %d devices in three seconds (there may be none in range)\n", failed ? "FAIL" : "ok", ads, devices);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
#else
    (void)argc;
    (void)argv;
    printf("(the shell's Bluetooth -- Windows.Devices.Bluetooth -- is not in this build of the tests: --bluetooth-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* ------------------------------------------------------- --portal-windows */

#ifdef MG_PORTAL_WINDOWS
/* mg_portal_windows.cpp: the Xbox build's Device Portal client against the mock portal at `address`. */
MG_HOSTAPI int mg_portal_windows_run(const char *address, const char *user, const char *password, const char *scratch, const char *received,
                                     const char *mock_log, int *checks);
#endif

static int portal_windows_mode(char **argv)
{
#ifdef MG_PORTAL_WINDOWS
    int checks = 0;
    const int failed = mg_portal_windows_run(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7], &checks);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
#else
    (void)argv;
    printf("(the Xbox build's Device Portal client is not in this build of the tests: --portal-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

/* ------------------------------------------------------- --window-windows */

#ifdef MG_WINDOW_WINDOWS
/* mg_window_windows.cpp: the shell's window (player/WindowHost.cpp) with no panel, its pictures as they come and the first. */
MG_HOSTAPI int mg_window_windows_run(void (MEGLER_CB *line)(const char *text, void *user), int *checks);
#endif

static int window_windows_mode(void)
{
#ifdef MG_WINDOW_WINDOWS
    int checks = 0, failed;
    mg_core_init();
    failed = mg_window_windows_run(print_line, &checks);
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
#else
    printf("(the shell's window -- player/WindowHost.cpp -- is not in this build of the tests: --window-windows does nothing here; tools/build-tests.sh win runs it)\n");
    return 0;
#endif
}

#if defined(MG_CORE_ELF)
/* On desktop Windows the tests are part of the core's ELF, and Megler's test program calls this (host/win32/mg_desktop_tests.c). */
MEGLER_API int mg_tests_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    if (argc >= 3 && !strcmp(argv[1], "--gfx-windows")) return gfx_windows_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--storage-windows")) return storage_windows_mode(argv);
    if (argc >= 4 && !strcmp(argv[1], "--codec-windows")) return codec_windows_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--camera-windows")) return camera_windows_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--sensors-windows")) return sensors_windows_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--bluetooth-windows")) return bluetooth_windows_mode(argc, argv);
    if (argc >= 8 && !strcmp(argv[1], "--portal-windows")) return portal_windows_mode(argv);
    if (argc >= 2 && !strcmp(argv[1], "--window-windows")) return window_windows_mode();
    if (argc >= 5 && !strcmp(argv[1], "--gfx-replay")) return gfx_replay_mode(argc, argv);
    if (argc >= 7 && !strcmp(argv[1], "--gfx-record")) {
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[4];
        config.package_dir = argv[4];
        config.log = mg_gfx_record_log;
        megler_init(&config);
        return mg_gfx_record_run(argv[2], argv[3], argv[5], argv[6]);
    }
    if (argc >= 6 && !strcmp(argv[1], "--java")) {
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[4];
        config.package_dir = argv[4];
        config.log = mg_java_tests_log;
        megler_init(&config);
        if (argc >= 9) {
            int w = 0, h = 0, dpi = 0;
            if (sscanf(argv[8], "%dx%d@%d", &w, &h, &dpi) == 3 && w > 0 && h > 0 && dpi >= 160) mg_tests_java_set_screen(w, h, dpi);
        }
        if (argc > 9) mg_tests_java_set_script(argc - 9, argv + 9);
        return mg_tests_java_run(argv[2], argv[3], argv[5], argc >= 7 && strcmp(argv[6], "-") ? argv[6] : NULL, argc >= 8 ? atoi(argv[7]) : 0) ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--icon")) {
        megler_config config;
        uint8_t *bytes = NULL;
        size_t size = 0;
        FILE *f;
        memset(&config, 0, sizeof(config));
        config.data_dir = ".";
        config.package_dir = ".";
        megler_init(&config);
        if (argc >= 5) bytes = mg_apk_icon_draw(argv[2], argv[4], &size);
        if (argc >= 5 ? !bytes : !megler_apk_icon(argv[2], &bytes, &size)) {
            printf("%s: no icon\n", argv[2]);
            return 1;
        }
        f = fopen(argv[3], "wb");
        if (!f || fwrite(bytes, 1, size, f) != size) {
            printf("%s could not be written\n", argv[3]);
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
        printf("%s: %u bytes\n", argv[3], (unsigned)size);
        megler_free(bytes);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--bundles")) {
        /* The scratch folder is the data folder too: a bundle's expansion files go into the app's storage under it. */
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[3];
        config.package_dir = argv[3];
        megler_init(&config);
        return mg_bundle_tests_run(argv[2], argv[3], argc >= 5 ? argv[4] : NULL) ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--sqlite-hold")) return mg_tests_sqlite_hold(argv[2], argv[3]);
#if MG_GUEST_CODE
    if (argc >= 4 && !strcmp(argv[1], "--load")) return load_mode(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "--run")) {
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[3];
        config.package_dir = argv[3];
        megler_init(&config);
        return mg_tests_smoke_run(argv[2], argc >= 5 ? atoi(argv[4]) : 1500);
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
        mg_core_init();
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
        mg_dvm_test_config config;
        int checks = 0, failed;
        mg_core_init();
        memset(&config, 0, sizeof(config));
        config.fixtures = argv[2];
        config.scratch = argv[3];
        config.android_root = argv[4][0] == '-' && !argv[4][1] ? NULL : argv[4];
        config.trace = argc >= 6 ? atoi(argv[5]) : 0;
        config.guest_dir = argc >= 7 ? argv[6] : NULL;
        failed = mg_dvm_tests_run(&config, NULL, NULL, &checks);
        printf("%d checks, %d failed\n", checks, failed);
        return failed ? 1 : 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--dvm-link")) return link_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--dvm-init")) return init_mode(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "--dvm-natives")) return natives_mode(argv);
    if (argc >= 3 && !strcmp(argv[1], "--dvm-bench")) {
        mg_core_init();
        mg_dvm_tests_bench(argv[2], argc >= 4 ? argv[3] : NULL, argc >= 5 ? atoi(argv[4]) : 300);
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
            else if (!strcmp(argv[i], "--dvm-android")) mg_tests_set_dvm_android(argv[i + 1]);
        }
        mg_tests_set_dvm(fixtures, apk, trace);
    }
    {
        /* The app's data lives under the scratch directory, as it lives under LocalState on the phone. */
        megler_config config;
        memset(&config, 0, sizeof(config));
        config.data_dir = argv[2];
        config.package_dir = argv[1];
        megler_init(&config);
    }
    return mg_tests_run(argv[1][0] == '-' && !argv[1][1] ? NULL : argv[1], argv[2], NULL, NULL) ? 1 : 0;
}
