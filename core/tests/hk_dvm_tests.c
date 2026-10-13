/*
 * The Dalvik runtime's tests (src/dvm), run with the core's own by
 * hk_tests_run: on the PC, under qemu-arm and in the phone's Self-test.
 *
 * The fixtures are DEX files, in the folder `dvm` beside the guest test
 * libraries. tools/build-dvm-fixtures.sh makes them: a.dex to d.dex are
 * assembled by tools/dexasm.py from the .smali files under
 * core/tests/dvm/fixtures (a.dex holds nearly everything, b.dex what needs a
 * second file, c.dex the instructions that are refused, d.dex classes that
 * cannot be linked); the arithmetic part is generated, with its expected
 * values, by tools/dvm-ops.py; d8t.dex is a program javac and d8 compiled. An
 * APK, when one is given, is a real app's: its classes.dex is read, linked and
 * partly run.
 *
 * The runtime is started once in a process and cannot be stopped, and the
 * fixtures count on finding their classes untouched: a second run in the same
 * process says so and checks nothing.
 */
#include <math.h>
#include <stdio.h>

#include "../src/dvm/hk_dvm_internal.h"
#include "../src/ld/hk_ld.h"
#include "hk_dvm_ops_cases.h"
#include "hk_dvm_tests.h"

int g_checks, g_failed;
bool g_libcore;
static hk_test_sink g_sink;
static void *g_sink_user;

void say(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_sink) g_sink(line, g_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

hk_jvalue A[16];

hk_jvalue call(const char *cls, const char *name, const char *sig)
{
    hk_jvalue r;
    r.j = 0;
    hk_dvm_call_static(cls, name, sig, A, &r);
    memset(A, 0, sizeof(A));
    return r;
}

hk_jvalue vcall(hk_jobj *self, const char *name, const char *sig)
{
    hk_jvalue r;
    r.j = 0;
    hk_dvm_call_virtual(self, name, sig, A, &r);
    memset(A, 0, sizeof(A));
    return r;
}

const char *thrown(void)
{
    static char text[4][600];
    static int next;
    char *buf = text[next++ & 3];
    if (!hk_dvm_describe_pending(buf, sizeof(text[0]))) buf[0] = 0;
    hk_dvm_clear_pending();
    return buf;
}

const char *text_of(hk_jobj *s)
{
    const char *t = hk_dvm_string_utf8(s);
    return t ? t : "(null)";
}

/* ------------------------------------------------------- generated: t.Ops */

static void test_ops(void)
{
    size_t i;
    int before = g_failed;
    say("every arithmetic instruction (%u cases worked out by tools/dvm-ops.py)", (unsigned)HK_COUNT(k_ops_cases));
    for (i = 0; i < HK_COUNT(k_ops_cases); i++) {
        const ops_case *c = &k_ops_cases[i];
        hk_jvalue args[2], r;
        const char *p = c->sig + 1;
        char ret = c->sig[strlen(c->sig) - 1];
        bool ok, good;
        int k;
        for (k = 0; *p != ')'; p++, k++) {
            args[k].j = 0;
            if (*p == 'J' || *p == 'D') args[k].j = c->a[k];
            else args[k].i = (i32)c->a[k];
        }
        r.j = 0;
        ok = hk_dvm_call_static("t/Ops", c->name, c->sig, args, &r);
        g_checks++;
        if (c->throws) {
            good = !ok && hk_dvm_pending() && !strcmp(hk_dvm_class_name(hk_dvm_pending()), c->throws);
        } else if (!ok) {
            good = false;
        } else if (c->want_nan) {
            good = ret == 'F' ? hk_f((u32)r.i) != hk_f((u32)r.i) : hk_d((u64)r.j) != hk_d((u64)r.j);
        } else if (ret == 'J' || ret == 'D') {
            good = r.j == c->want;
        } else {
            good = r.i == (i32)c->want;
        }
        if (!good) {
            g_failed++;
            if (g_failed - before <= 20)
                say("  FAIL t.Ops.%s%s (%#llx, %#llx): got %#llx%s, expected %#llx%s%s", c->name, c->sig, (unsigned long long)c->a[0], (unsigned long long)c->a[1],
                    (unsigned long long)r.j, ok ? "" : " and an exception", (unsigned long long)c->want, c->want_nan ? " (a NaN)" : "", c->throws ? c->throws : "");
        }
        hk_dvm_clear_pending();
    }
}

/* ---------------------------------------------------------------- t.Arith */

static void test_arith(void)
{
    say("constants, loops, recursion");
    A[0].i = 2;
    A[1].i = 3;
    CHECK_INT(call("t/Arith", "sum", "(II)I").i, 5);
    CHECK_INT(call("t/Arith", "consts", "()I").i, 0x11373bb4);
    CHECK(call("t/Arith", "wideConsts", "()J").j == (i64)0xadcba98708880885ull);
    CHECK(call("t/Arith", "floatConsts", "()F").f == 40.0f);
    CHECK(call("t/Arith", "doubleConsts", "()D").d == 2.5 + 0.1);
    A[0].i = 20;
    CHECK(call("t/Arith", "factorial", "(I)J").j == 2432902008176640000ll);
    A[0].i = 1000;
    CHECK_INT(call("t/Arith", "sumTo", "(I)I").i, 500500);
    A[0].i = 1071;
    A[1].i = 462;
    CHECK_INT(call("t/Arith", "gcd", "(II)I").i, 21);
    A[0].i = 20;
    CHECK_INT(call("t/Arith", "fib", "(I)I").i, 6765);
    CHECK_STR(thrown(), "");
    /* What is not there says so. */
    CHECK(!hk_dvm_call_static("t/Arith", "nothing", "()V", NULL, NULL));
    CHECK_STR(thrown(), "java.lang.NoSuchMethodError: t/Arith.nothing()V");
    CHECK(!hk_dvm_call_static("t/Nowhere", "nothing", "()V", NULL, NULL));
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: t/Nowhere");
}

/* ------------------------------------------------------------- two files */

static void test_two_files(void)
{
    say("a second DEX file");
    A[0].i = 40;
    A[1].i = 2;
    CHECK_INT(call("u/Other", "viaArith", "(II)I").i, 142);
    CHECK_INT(call("u/Other", "dup", "()I").i, 1);
    CHECK_STR(thrown(), "");
}

/* ------------------------------------------------------------------ main */

static bool add_fixtures(const char *fixtures)
{
    static bool added;
    char *path;
    bool ok = true;
    int i;
    if (added) return true;
    for (i = 0; i < 4; i++) {
        path = hk_sprintf("%s/%c.dex", fixtures, 'a' + i);
        if (!hk_dvm_add_dex_file(path)) ok = false;
        free(path);
    }
    added = true;
    return ok;
}

void hk_dvm_tests_bench(const char *fixtures)
{
    static const int rounds = 20;
    u64 t0, t1;
    int i;
    i64 units;
    if (!hk_dvm_start(NULL, 0) || !add_fixtures(fixtures)) {
        say("the runtime did not start, or its fixtures are not in %s", fixtures);
        return;
    }
    say("benchmark (%s)", hk_host_name());
    /* sumTo(n): four instructions a turn (if-lez, add-int/2addr, add-int/lit8, goto). */
    t0 = hk_time_monotonic_ns();
    for (i = 0; i < rounds; i++) {
        A[0].i = 5000000;
        call("t/Arith", "sumTo", "(I)I");
    }
    t1 = hk_time_monotonic_ns();
    units = (i64)rounds * 5000000 * 4;
    say("  a counting loop: %.0f million instructions a second", (double)units * 1000.0 / (double)(t1 - t0));
    /* fib(27): 832040 calls returning 0 or 1 and 832039 that add; about 13 instructions and one call each. */
    t0 = hk_time_monotonic_ns();
    A[0].i = 27;
    call("t/Arith", "fib", "(I)I");
    t1 = hk_time_monotonic_ns();
    say("  recursive calls: %.1f million calls a second", 317811.0 * 2 * 1000.0 / (double)(t1 - t0));
}

int hk_dvm_tests_run(const hk_dvm_test_config *config, hk_test_sink sink, void *user, int *checks)
{
    static bool ran;
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    if (ran) {
        say("dalvik runtime: already tested in this process (it cannot be started twice)");
        *checks = 0;
        return 0;
    }
    ran = true;
    /* Much of what is tested is refusal, and each refusal is a warning in the log: only errors are shown unless tracing is asked for. */
    hk_log_set_level(config->trace ? HK_LOG_INFO : HK_LOG_ERROR);
    say("dalvik runtime (%s, %u-bit registers, %u bytes of header to an object, %u registers to a frame)", hk_host_name(), (unsigned)(sizeof(hk_dvm_reg) * 8),
        (unsigned)HK_DVM_HEADER, (unsigned)((sizeof(hk_dvm_frame) + sizeof(hk_dvm_reg) - 1) / sizeof(hk_dvm_reg)));
    if (config->android_root) {
        /* On Android's own libcore: started, asked what only libcore can answer, and then given the same fixtures as the built-in java.lang. */
        u64 t0 = hk_time_monotonic_ns();
        size_t unresolved = hk_ld_unresolved_count();
        hk_dvm_stats at_start;
        CHECK(hk_dvm_start_android(config->android_root));
        /* libcore's own libraries, where they were loaded, found every function they import from the system's. */
        CHECK_INT(hk_ld_unresolved_count() - unresolved, 0);
        if (!g_failed) {
            g_libcore = true;
            hk_dvm_get_stats(&at_start);
            hk_dvm_set_trace(config->trace);
            test_libcore(config, hk_time_monotonic_ns() - t0, &at_start);
        }
    } else {
        CHECK(hk_dvm_start(NULL, 0));
    }
    CHECK(add_fixtures(config->fixtures));
    if (!g_failed) {
        hk_dvm_set_trace(config->trace);
        test_ops();
        test_arith();
        test_two_files();
        test_language();
        test_runtime(config->fixtures);
        test_jni(config->guest_dir);
        if (!g_libcore) test_d8(config->fixtures);            /* on libcore that program has been run already, with the other */
        if (config->apk) test_real_apk(config->apk);
        if (g_libcore) test_libcore_link_all();
        hk_dvm_set_trace(0);
    } else {
        say("  (the fixtures are made by tools/build-dvm-fixtures.sh and looked for in %s)", config->fixtures);
    }
    hk_log_set_level(HK_LOG_INFO);
    *checks = g_checks;
    return g_failed;
}
