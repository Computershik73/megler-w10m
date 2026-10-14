/*
 * The runtime on Android's own libcore: what hk_dvm_tests_run does when it is
 * given an Android root, a folder laid out like Android's /system
 * (tools/dvm-android-root.sh makes one from build/android9): the boot class
 * path as DEX files in framework/, and, where they have been built, libcore's
 * native libraries in lib/ with ICU's data and the time zones beside them.
 *
 * A process has one runtime and one boot class path: a run is on libcore or
 * on the built-in java.lang, and the fixtures are run on whichever it is.
 *
 * What is asked here, before the fixtures: that the runtime starts on those
 * files, and what that costs before anything is linked that is not needed;
 * and that programs javac and d8 compiled give what the JVM gave for the same
 * class files (d8t.dex, which uses java.lang alone; lib.dex, which uses
 * java.util, java.io and java.text: the parts of it that need libcore's
 * native libraries run only where those are loaded). After the fixtures:
 * that every class of the boot class path can be linked.
 */
#include <stdio.h>

#include "../src/bionic/hk_bionic.h"
#include "../src/bionic/hk_fd.h"
#include "../src/dvm/hk_dvm_internal.h"
#include "hk_dvm_tests.h"

const char *hk_dvm_art_host_output(void);

/* The line "name=..." of a file of such lines, or NULL: where Android's answer to a test is not the JVM's (lib-android.txt). */
static const char *android_answer(const char *overrides, const char *name, char *out, size_t n)
{
    size_t len = strlen(name);
    const char *p;
    for (p = overrides; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, name, len) && p[len] == '=') {
            const char *end = strchr(p, '\n');
            size_t k = end ? (size_t)(end - (p + len + 1)) : strlen(p + len + 1);
            if (k && p[len + k] == '\r') k--;
            if (k >= n) k = n - 1;
            memcpy(out, p + len + 1, k);
            out[k] = 0;
            return out;
        }
    }
    return NULL;
}

/*
 * Each "name=result" line of a file the JVM wrote, against the same method
 * run here. Lines whose name starts with `skip` are left out and counted.
 * `android_name` is a file of the answers that are Android's own.
 */
static void run_expected(const char *fixtures, const char *expected_name, const char *android_name, const char *cls, const char *skip, int *ran,
                         int *skipped)
{
    char *path = hk_sprintf("%s/%s", fixtures, expected_name);
    char *apath = android_name ? hk_sprintf("%s/%s", fixtures, android_name) : NULL;
    size_t size = 0;
    char *expected = (char *)hk_file_read_all(path, &size), *line, *next;
    char *overrides = apath ? (char *)hk_file_read_all(apath, &size) : NULL;
    static char theirs[4096];
    free(apath);
    *ran = *skipped = 0;
    CHECK(expected != NULL);
    free(path);
    if (!expected) return;
    for (line = expected; *line; line = next) {
        char *eq, *end = strchr(line, '\n'), stack[1600];
        hk_jvalue r;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        if (end && end > line && end[-1] == '\r') end[-1] = 0;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (skip && !strncmp(line, skip, strlen(skip))) {
            (*skipped)++;
            continue;
        }
        (*ran)++;
        r.j = 0;
        if (!hk_dvm_call_static(cls, line, "()Ljava/lang/String;", NULL, &r)) {
            hk_dvm_pending_stack(stack, sizeof(stack));
            say("  %s threw %s\n%s", line, thrown(), stack);
            r.l = NULL;
        }
        {
            const char *want = android_answer(overrides, line, theirs, sizeof(theirs));
            g_checks++;
            if (strcmp(text_of(r.l), want ? want : eq + 1) != 0) {
                g_failed++;
                say("  FAIL %s.%s():\n    got      %s\n    expected %s%s", cls, line, text_of(r.l), want ? want : eq + 1, want ? "\n    (Android's own answer, from the file of those)" : "");
            } else if (want) {
                say("  %s: Android's own answer, not the JVM's (%s says why)", line, android_name);
            }
        }
    }
    free(expected);
    free(overrides);
}

void test_libcore_link_all(void)
{
    u32 total = hk_dvm_defined_count(), i, linked = 0, failed = 0;
    hk_dvm_stats before, after;
    char first[300] = "";
    hk_dvm_get_stats(&before);
    for (i = 0; i < total; i++) {
        hk_dvm_dex *d = NULL;
        const char *name = hk_dvm_defined_name(i, &d);
        if (!d || !d->boot) continue;
        if (hk_dvm_find_class(name)) linked++;
        else {
            if (!failed) hk_dvm_describe_pending(first, sizeof(first));
            failed++;
            hk_dvm_clear_pending();
        }
    }
    hk_dvm_get_stats(&after);
    say("every class of the boot class path: %u linked, %u not%s%s (%u were linked before this, in %u KB of tables and classes; now %u, in %u KB)", (unsigned)linked,
        (unsigned)failed, failed ? ", the first: " : "", first, (unsigned)before.classes_linked, (unsigned)(before.meta_bytes >> 10), (unsigned)after.classes_linked,
        (unsigned)(after.meta_bytes >> 10));
    CHECK(linked > 5000);
    CHECK_INT(failed, 0);
}

/* The default zone's name and its offset from Greenwich, after libcore has been made to forget the one it had worked out (TimeZone.setDefault(null)), so that it asks again. */
static const char *default_zone(i32 *offset_ms)
{
    hk_jvalue zone;
    memset(A, 0, sizeof(A));
    call("java/util/TimeZone", "setDefault", "(Ljava/util/TimeZone;)V");
    zone = call("java/util/TimeZone", "getDefault", "()Ljava/util/TimeZone;");
    *offset_ms = -1;
    if (!zone.l) return "(none)";
    *offset_ms = vcall(zone.l, "getRawOffset", "()I").i;
    return text_of(vcall(zone.l, "getID", "()Ljava/lang/String;").l);
}

/*
 * The default time zone is the phone's, which the shell names (megler_set_timezone; hk_timezone_set underneath): with none
 * named it is GMT, as libcore says by itself; with a zone of tzdata's, that zone; with a name tzdata does not have, GMT
 * again and nothing thrown. The property persist.sys.timezone says the name, "UTC" while there is none.
 */
static void test_timezone(void)
{
    i32 offset;
    say("the default time zone");
    CHECK_STR(default_zone(&offset), "GMT");
    CHECK_INT(offset, 0);
    hk_timezone_set("Asia/Kolkata");
    CHECK_STR(default_zone(&offset), "Asia/Kolkata");
    CHECK_INT(offset, 19800000);
    CHECK_STR(hk_sysprop("persist.sys.timezone"), "Asia/Kolkata");
    hk_timezone_set("Nowhere/At_All");
    CHECK_STR(default_zone(&offset), "GMT");
    CHECK_INT(offset, 0);
    hk_timezone_set("");
    CHECK_STR(default_zone(&offset), "GMT");
    CHECK_STR(hk_sysprop("persist.sys.timezone"), "UTC");
    CHECK_STR(thrown(), "");
}

/*
 * A DEX file handed over in memory, as a library that carries Java code inside its native one hands it over
 * (dalvik.system.InMemoryDexClassLoader; JUCE, which comScore in MX Player is built with, does): m.dex, which nothing has
 * put on the path, read into a byte array, wrapped in a ByteBuffer and given to the loader, whose class then runs.
 */
static void test_in_memory_dex(const char *fixtures)
{
    char *path = hk_sprintf("%s/m.dex", fixtures);
    size_t size = 0;
    u8 *bytes = (u8 *)hk_file_read_all(path, &size);
    hk_jobj *array, *buffer, *parent, *loader = NULL, *cls = NULL;
    say("a DEX file handed over in memory (InMemoryDexClassLoader)");
    free(path);
    CHECK(bytes != NULL && !hk_dvm_class_exists("t/FromMemory"));
    if (!bytes) return;
    array = hk_dvm_new_array("[B", (i32)size);
    if (array) memcpy(array->u.arr.data, bytes, size);
    free(bytes);
    memset(A, 0, sizeof(A));
    A[0].l = array;
    buffer = call("java/nio/ByteBuffer", "wrap", "([B)Ljava/nio/ByteBuffer;").l;
    parent = call("java/lang/ClassLoader", "getSystemClassLoader", "()Ljava/lang/ClassLoader;").l;
    memset(A, 0, sizeof(A));
    A[0].l = buffer;
    A[1].l = parent;
    if (buffer) loader = hk_dvm_new("dalvik/system/InMemoryDexClassLoader", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V", A);
    CHECK_STR(thrown(), "");
    memset(A, 0, sizeof(A));
    A[0].l = hk_dvm_new_string("t.FromMemory");
    if (loader) cls = vcall(loader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;").l;
    CHECK(cls != NULL);
    CHECK_STR(thrown(), "");
    CHECK_INT(call("t/FromMemory", "answer", "()I").i, 42);
}

void test_libcore(const hk_dvm_test_config *config, u64 started_ns, const hk_dvm_stats *at_start)
{
    const char *fixtures = config->fixtures, *scratch = config->scratch;
    hk_dvm_stats s;
    hk_jvalue v, dir;
    u64 t0;
    int ran = 0, skipped = 0, trace = config->trace;
    i32 registered = 0, unknown = 0, by_name = 0;
    bool natives = hk_dvm_libcore_natives();
    say("libcore (Android's boot class path in %s; its native libraries %s)", config->android_root, natives ? "loaded" : "not loaded");
    say("  started in %u ms: %u DEX files, %u classes defined, %u linked, %u initialised, %u objects (%u KB), %u KB of tables and classes",
        (unsigned)(started_ns / 1000000ull), (unsigned)at_start->dex_files, (unsigned)at_start->classes_defined, (unsigned)at_start->classes_linked,
        (unsigned)at_start->classes_initialised, (unsigned)at_start->objects, (unsigned)(at_start->object_bytes >> 10), (unsigned)(at_start->meta_bytes >> 10));
    if (natives) {
        /*
         * libjavacore's and libopenjdk's JNI_OnLoad have run: 330 and 239 native methods registered, each of them one the
         * boot class path declares.
         */
        hk_jni_native_counts(&registered, &unknown, &by_name);
        say("  native methods registered by libcore's libraries: %d; table entries that named none: %d", (int)registered, (int)unknown);
        CHECK_INT(registered, 569);
        CHECK_INT(unknown, 0);
    }
    /* The two programs' own classes, after the boot class path. */
    for (ran = 0; ran < 2; ran++) {
        char *dex = hk_sprintf("%s/%s", fixtures, ran ? "lib.dex" : "d8t.dex");
        CHECK(hk_dvm_add_dex_file(dex));
        free(dex);
    }

    /* java.lang.System's own initialiser: properties, the three streams. */
    t0 = hk_time_monotonic_ns();
    v.j = 0;
    CHECK(hk_dvm_get_static("java/lang/System", "out", &v) && v.l != NULL);
    CHECK_STR(thrown(), "");
    if (natives && v.l) {
        /*
         * A line to System.out: through libcore's PrintStream, ICU's converter and libopenjdk's write to descriptor 1, which
         * in this layer is its log, at the level of things said in passing: shown for this.
         */
        hk_log_set_level(HK_LOG_INFO);
        A[0].l = hk_dvm_new_string("hello from libcore");
        vcall(v.l, "println", "(Ljava/lang/String;)V");
        CHECK_STR(thrown(), "");
        hk_log_set_level(trace ? HK_LOG_INFO : HK_LOG_ERROR);
        if (scratch) {
            /*
             * And the same line with System.out's descriptor turned to a file for the moment, by the guest's own dup2:
             * what arrives there is the line and its newline, byte for byte. (The log shows the line; this checks it.)
             * The descriptor is not 1 itself: libcore's FileDescriptor.out is a duplicate of 1 made when the class starts.
             */
            char *host = hk_sprintf("%s/system-out.txt", scratch), *text;
            size_t size = 0;
            int saved, file, out_fd;
            hk_jvalue fdo, number;
            fdo.j = number.j = 0;
            CHECK(hk_dvm_get_static("java/io/FileDescriptor", "out", &fdo) && fdo.l && hk_dvm_get_field(fdo.l, "descriptor", &number));
            out_fd = number.i;
            CHECK(out_fd > 2);
            hk_path_mkdirs(scratch);
            hk_fs_mount("/scratch-java", scratch);
            saved = ((int (*)(int))hk_bionic_find("dup"))(out_fd);
            file = ((int (*)(const char *, int, ...))hk_bionic_find("open"))("/scratch-java/system-out.txt", 01 | 0100 | 01000, 0600);
            CHECK(saved > 2 && file > 2);
            ((int (*)(int, int))hk_bionic_find("dup2"))(file, out_fd);
            A[0].l = hk_dvm_new_string("hello from libcore");
            vcall(v.l, "println", "(Ljava/lang/String;)V");
            CHECK_STR(thrown(), "");
            ((int (*)(int, int))hk_bionic_find("dup2"))(saved, out_fd);
            ((int (*)(int))hk_bionic_find("close"))(saved);
            ((int (*)(int))hk_bionic_find("close"))(file);
            text = (char *)hk_file_read_all(host, &size);
            CHECK(text && size == 19 && !memcmp(text, "hello from libcore\n", 19));
            say("  System.out.println wrote %u bytes to descriptor %d, libcore's duplicate of 1", (unsigned)size, out_fd);
            free(text);
            free(host);
        }
    } else {
        say("  (libcore's native libraries are not loaded: nothing is printed through System.out, which needs ICU's converters)");
    }
    hk_dvm_get_stats(&s);
    say("  System and a line printed, %u ms: %u classes linked, %u initialised, %u objects (%u KB), %u KB of tables and classes",
        (unsigned)((hk_time_monotonic_ns() - t0) / 1000000ull), (unsigned)s.classes_linked, (unsigned)s.classes_initialised, (unsigned)s.objects,
        (unsigned)(s.object_bytes >> 10), (unsigned)(s.meta_bytes >> 10));

    /*
     * A class that asks who is calling it: BufferedInputStream makes an atomic updater for its own protected field, the
     * updater notes its caller's class (three frames up from the native method that says so), and closing the stream
     * goes through the updater, which refuses anyone but that class.
     */
    memset(A, 0, sizeof(A));
    v.l = hk_dvm_new("java/io/BufferedInputStream", "(Ljava/io/InputStream;)V", A);
    CHECK(v.l != NULL);
    if (v.l) vcall(v.l, "close", "()V");
    CHECK_STR(thrown(), "");

    say("the java.lang program (d8t.dex) on libcore");
    /* One of its tests turns a string into bytes, which is ICU's work. */
    run_expected(fixtures, "expected.txt", NULL, "d8t/Main", natives ? NULL : "numbers", &ran, &skipped);
    CHECK_INT(ran + skipped, 10);
    if (skipped) say("  (%d of its 10 tests left out: it needs libcore's native libraries)", skipped);

    say("the java.util, java.io and java.text program (lib.dex)");
    if (natives && scratch) {
        /* Where its file tests write: a folder of the host's, under a name of the guest's, since libcore's libraries see Android's paths. */
        hk_path_mkdirs(scratch);
        hk_fs_mount("/scratch-java", scratch);
        dir.j = 0;
        dir.l = hk_dvm_new_string("/scratch-java");
        CHECK(hk_dvm_set_static("d8t/Lib", "dir", dir));
        CHECK_STR(thrown(), "");
    }
    t0 = hk_time_monotonic_ns();
    run_expected(fixtures, "lib-expected.txt", "lib-android.txt", "d8t/Lib", natives ? NULL : "lib_", &ran, &skipped);
    CHECK_INT(ran + skipped, 18);
    if (skipped) say("  (%d of its 18 tests left out: they need libcore's native libraries)", skipped);
    if (natives) {
        /* java.nio.file's native half is in no table: libopenjdk exports it as Java_sun_nio_fs_... and it is found by those names. */
        hk_jni_native_counts(&registered, &unknown, &by_name);
        say("  native methods found by their JNI names so far: %d", (int)by_name);
        CHECK(by_name > 0);
        CHECK_INT(unknown, 0);
    }
    hk_dvm_get_stats(&s);
    say("  the two programs, %u ms for the second: %u classes linked, %u initialised, %u objects (%u KB), %u KB of tables and classes",
        (unsigned)((hk_time_monotonic_ns() - t0) / 1000000ull), (unsigned)s.classes_linked, (unsigned)s.classes_initialised, (unsigned)s.objects,
        (unsigned)(s.object_bytes >> 10), (unsigned)(s.meta_bytes >> 10));
    /* The time zones are a file read through libcore's own libraries. */
    if (natives) test_timezone();
    /* (InMemoryDexClassLoader goes through java.io, whose native half is libcore's.) */
    if (natives) test_in_memory_dex(fixtures);
    /* The interpreter's speed on this machine, said and not checked: so the phone's Self-test brings the phone's own numbers. */
    say("the interpreter's benchmark (printed, not checked)");
    /* With the collector as an app has it (the tests' own is poisoned and runs every megabyte), and as the tests have it after. */
    hk_dvm_gc_set_poison(false);
    hk_dvm_gc_set_min_budget(8u << 20);
    hk_dvm_bench_run(fixtures, 40);
    hk_dvm_gc_set_poison(true);
    hk_dvm_gc_set_min_budget(1u << 20);
}
