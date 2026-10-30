/*
 * The Dalvik runtime's tests (src/dvm), run with the core's own by
 * mg_tests_run: on the PC, under qemu-arm and in the phone's Self-test.
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

#include "../src/dvm/mg_dvm_internal.h"
#include "../src/ld/mg_ld.h"
#include "mg_dvm_ops_cases.h"
#include "mg_dvm_tests.h"

int g_checks, g_failed;
bool g_libcore;
static mg_test_sink g_sink;
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

mg_jvalue A[16];

mg_jvalue call(const char *cls, const char *name, const char *sig)
{
    mg_jvalue r;
    r.j = 0;
    mg_dvm_call_static(cls, name, sig, A, &r);
    memset(A, 0, sizeof(A));
    return r;
}

mg_jvalue vcall(mg_jobj *self, const char *name, const char *sig)
{
    mg_jvalue r;
    r.j = 0;
    mg_dvm_call_virtual(self, name, sig, A, &r);
    memset(A, 0, sizeof(A));
    return r;
}

const char *thrown(void)
{
    static char text[4][600];
    static int next;
    char *buf = text[next++ & 3];
    if (!mg_dvm_describe_pending(buf, sizeof(text[0]))) buf[0] = 0;
    mg_dvm_clear_pending();
    return buf;
}

const char *text_of(mg_jobj *s)
{
    const char *t = mg_dvm_string_utf8(s);
    return t ? t : "(null)";
}

/* ------------------------------------------------------- generated: t.Ops */

static void test_ops(void)
{
    size_t i;
    int before = g_failed;
    say("every arithmetic instruction (%u cases worked out by tools/dvm-ops.py)", (unsigned)MG_COUNT(k_ops_cases));
    for (i = 0; i < MG_COUNT(k_ops_cases); i++) {
        const ops_case *c = &k_ops_cases[i];
        mg_jvalue args[2], r;
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
        ok = mg_dvm_call_static("t/Ops", c->name, c->sig, args, &r);
        g_checks++;
        if (c->throws) {
            good = !ok && mg_dvm_pending() && !strcmp(mg_dvm_class_name(mg_dvm_pending()), c->throws);
        } else if (!ok) {
            good = false;
        } else if (c->want_nan) {
            good = ret == 'F' ? mg_f((u32)r.i) != mg_f((u32)r.i) : mg_d((u64)r.j) != mg_d((u64)r.j);
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
        mg_dvm_clear_pending();
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
    CHECK(!mg_dvm_call_static("t/Arith", "nothing", "()V", NULL, NULL));
    CHECK_STR(thrown(), "java.lang.NoSuchMethodError: t/Arith.nothing()V");
    CHECK(!mg_dvm_call_static("t/Nowhere", "nothing", "()V", NULL, NULL));
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: t/Nowhere");
}

/* ---------------------------------------------------------------- t.Jit */

/* What t.Jit.mix computes, in C with Java's wrapping arithmetic: the answer its compiled code must give. */
static i32 jit_mix(i32 n, i32 k)
{
    u32 v0 = (u32)k, v2, v3, v4, v5, v6;
    i32 v1;
    for (v1 = 0; v1 < n; v1++) {
        u32 s = (u32)v1 & 31;
        v2 = v0 + (u32)v1;
        v3 = v2 - (u32)k;
        v2 = v3 * v2;
        v2 ^= v0;
        v3 = (u32)v1 | 1;
        v4 = (u32)((i32)v2 / (i32)v3);
        v5 = (u32)((i32)v2 % (i32)v3);
        v4 += v5;
        v5 = v4 << s;
        v3 = (u32)((i32)v2 >> s);
        v6 = v2 >> s;
        v5 ^= v3;
        v5 += v6;
        v3 = v5 & 0x7f;
        v6 = ~v5;
        v6 = 0u - v6;
        v6 -= v3;
        v3 = 0x1234u - v6;
        v3 = 0x11u - v3;
        v3 = v3 * (u32)-3;
        v4 = (u32)((i32)v3 / 7);
        v5 = (u32)((i32)v3 % 0x65);
        v4 += v5;
        v5 = v4 << 1;
        v6 = (u32)((i32)v4 >> 3);
        v3 = v4 >> 31;
        v5 += v6;
        v5 += v3;
        v0 = v5 & v2;
        v0 |= v5;
    }
    return (i32)v0;
}

static mg_jobj *jit_array(const char *cls, i32 len) { return mg_dvm_new_array(cls, len); }

/* What t.Jit's call, long and switch loops return, computed here with Java's arithmetic. */
static i32 jit_call_kinds(i32 n)
{
    u32 v2 = 0;
    i32 i;
    for (i = 0; i < n; i++) {
        v2 += 3;                       /* area() */
        v2 += 3;                       /* hashCode() */
        v2 += (u32)i + v2;             /* sum(i, v2) */
        v2 ^= (u32)i + v2;             /* sum/range */
        v2 += 4;                       /* "abcd".length() */
        v2 += (u32)i;                  /* (int) wide(i) */
    }
    return (i32)v2;
}

static i32 jit_catch_in_loop(i32 n)
{
    i32 s = 0, i;
    for (i = 0; i < n; i++) s += i == 2 ? 1000 : 100 / (i - 2);
    return s;
}

static i64 jit_longs(i32 n, i64 seed)
{
    u64 v0 = (u64)seed, v2 = 0, v6, v8;
    i32 i, c;
    for (i = 0; i < n; i++) {
        u32 s = (u32)i & 63;
        v0 = v0 * 0x5851f42d4c957f2dull + 0x14057b7ef767814full;
        v2 ^= v0 >> s;
        v2 += v0 << s;
        v2 -= (u64)((i64)v0 >> s);
        v8 = v0 | v2;
        v6 = v8 & v0;
        v2 ^= v6;
        v6 = (u64)0 - v2;
        v8 = ~v6;
        v2 += v8;
        v2 += (u64)(i64)i;
        v2 ^= (u64)(i64)(i32)(u32)v2;
        c = (i64)v0 < (i64)v2 ? -1 : (i64)v0 > (i64)v2 ? 1 : 0;
        v2 += (u64)(i64)c;
        c = (i64)v2 < (i64)0x4000000000000000ll ? -1 : (i64)v2 > (i64)0x4000000000000000ll ? 1 : 0;
        v2 += (u64)(i64)c;
    }
    return (i64)(v0 + v2);
}

static i32 jit_switches(i32 n)
{
    i32 s = 0, i;
    for (i = 0; i < n; i++) {
        i32 k;
        switch (i % 6 - 1) {
        case -1: s += 10; break;
        case 0: s += 20 + 30; break;
        case 1: s += 30; break;
        case 2: s += 40; break;
        default: s += 1; break;
        }
        k = i * 7 % 50;
        s += k == 0 ? 100 : k == 7 ? 200 : k == 49 ? 300 : 2;
    }
    return s;
}

/*
 * The JIT (mg_dvm_jit.c, docs/JIT.md) with its threshold at 1: each of t.Jit's loops compiled at its first jump back,
 * and what they return as Java says. Exits too: a divisor of zero, an index out of bounds and a null array inside a
 * compiled loop throw as interpreted; calls made from compiled code, their exceptions and deep recursion through them; long
 * arithmetic and switches. Where the host runs no Thumb-2
 * (x64) nothing is compiled and the same answers are the interpreter's.
 */
static void test_jit(void)
{
    u32 was = g_dvm_jit_threshold;
    mg_jobj *ints, *bytes, *chars, *shorts, *flags;
    mg_dvm_class *c;
    i32 i, want;
    say("the JIT: loops compiled at their first jump back");
    mg_dvm_jit_start(true, 1, false);
    ints = jit_array("[I", 40);
    bytes = jit_array("[B", 40);
    chars = jit_array("[C", 40);
    shorts = jit_array("[S", 40);
    flags = jit_array("[Z", 7);
    CHECK(ints && bytes && chars && shorts && flags);
    if (!ints || !bytes || !chars || !shorts || !flags) return;
    /* Filled by the compiled loop at k = 1000: ints i*1000, bytes, chars and shorts their truncations. */
    A[0].l = ints;
    A[1].l = bytes;
    A[2].l = chars;
    A[3].l = shorts;
    A[4].i = 1000;
    want = 0;
    for (i = 0; i < 40; i++) want += i * 1000 + (i8)(i * 1000) + (u16)(i * 1000) + (i16)(i * 1000);
    CHECK_INT(call("t/Jit", "fill", "([I[B[C[SI)I").i, want);
    CHECK_INT(((i32 *)ints->u.arr.data)[39], 39000);
    CHECK_INT(((i8 *)bytes->u.arr.data)[39], (i8)39000);
    CHECK_INT(((u16 *)chars->u.arr.data)[39], (u16)39000);
    CHECK_INT(((i16 *)shorts->u.arr.data)[39], (i16)39000);
    for (i = 0; i < 7; i++) ((u8 *)flags->u.arr.data)[i] = (u8)(i % 3 == 0);
    A[0].l = flags;
    CHECK_INT(call("t/Jit", "countTrue", "([Z)I").i, 3);
    A[0].i = 300;
    A[1].i = 12345;
    CHECK_INT(call("t/Jit", "mix", "(II)I").i, jit_mix(300, 12345));
    A[0].i = 77;
    A[1].i = -9;
    CHECK_INT(call("t/Jit", "mix", "(II)I").i, jit_mix(77, -9));
    A[0].i = 3;
    CHECK_INT(call("t/Jit", "minOverMinusOne", "(I)I").i, (i32)(3u * 0x80000000u));
    for (want = 0, i = -5; i <= 5; i++)
        want += (i < 0 ? 1 : 0) + (i > 0 ? 2 : 0) + (i == 0 ? 4 : 0) + (i >= 0 ? 8 : 0) + (i <= 0 ? 0x10 : 0) + (i != 0 ? 0x20 : 0) +
                (i >= 0 ? 0x40 : 0) + (i > 0 ? 0x80 : 0) + (i != 0 ? 0x100 : 0) + (i == 0 ? 0x200 : 0) + (i < 0 ? 0x400 : 0);
    A[0].i = 5;
    CHECK_INT(call("t/Jit", "conditions", "(I)I").i, want);
    /* A divisor of zero among them, the index past the end, a null array: thrown from inside the compiled loops. */
    for (i = 0; i < 40; i++) ((i32 *)ints->u.arr.data)[i] = i < 30 ? i + 1 : 0;
    A[0].i = 100000;
    A[1].l = ints;
    CHECK_INT(call("t/Jit", "divideAll", "(I[I)I").i, 0);
    CHECK_STR(thrown(), "java.lang.ArithmeticException: divide by zero");
    A[0].l = ints;
    A[1].i = 41;
    CHECK_INT(call("t/Jit", "readTo", "([II)I").i, 0);
    CHECK_STR(thrown(), "java.lang.ArrayIndexOutOfBoundsException: length=40; index=40");
    A[0].l = NULL;
    A[1].i = 3;
    call("t/Jit", "readTo", "([II)I");
    CHECK_STR(thrown(), "java.lang.NullPointerException: Attempt to read from a null array");
    A[0].l = ints;
    A[1].i = 30;
    CHECK_INT(call("t/Jit", "readTo", "([II)I").i, 465);
    A[0].i = 50;
    CHECK_INT(call("t/Jit", "callsInLoop", "(I)I").i, 1225);
    /* Fields of every width, an object one and static ones, read and written by a compiled loop. */
    {
        i32 n = 300, k, ii = 0, total = 0;
        i8 b = 0;
        u16 ch = 0;
        i16 sh = 0;
        u8 z = 0;
        for (k = 0; k < n; k++) {
            ii += k;
            b = (i8)(b + 7);
            ch = (u16)(ch + 1000);
            sh = (i16)(sh + 1000);
            z ^= 1;
            total += k;
        }
        A[0].i = n;
        CHECK_INT(call("t/Jit", "fields", "(I)I").i, ii + b + ch + sh + z + total);
    }
    /* Calls of each kind from a compiled loop, a callee's exception caught by the loop's own handler, recursion through
       compiled calls deeper than the host's stack takes loops for, long arithmetic, and both switches. */
    A[0].i = 200;
    CHECK_INT(call("t/Jit", "callKinds", "(I)I").i, jit_call_kinds(200));
    A[0].i = 9;
    CHECK_INT(call("t/Jit", "catchInLoop", "(I)I").i, jit_catch_in_loop(9));
    CHECK_STR(thrown(), "");
    A[0].i = 3000;
    CHECK_INT(call("t/Jit", "deepCalls", "(I)I").i, 3000);
    CHECK_STR(thrown(), "");
    A[0].i = 500;
    A[1].j = (i64)0x0123456789abcdefll;
    CHECK(call("t/Jit", "longs", "(IJ)J").j == jit_longs(500, (i64)0x0123456789abcdefll));
    A[0].i = 777;
    CHECK_INT(call("t/Jit", "switches", "(I)I").i, jit_switches(777));
    /* Objects made, cast, locked and stored from a compiled loop; a store and a cast that fail thrown from it. */
    for (want = 0, i = 0; i < 100; i++) want += 3 + i + (i & 7);
    A[0].i = 100;
    CHECK_INT(call("t/Jit", "objects", "(I)I").i, want);
    A[0].i = 10;
    CHECK_INT(call("t/Jit", "storeFails", "(I)I").i, 1003);
    A[0].i = 10;
    CHECK_INT(call("t/Jit", "castFails", "(I)I").i, 2005);
    CHECK_STR(thrown(), "");
    A[0].i = 10;
    A[1].j = (i64)0x123456789abcdef0ll;
    CHECK(call("t/Jit", "wideMoves", "(IJ)J").j == (i64)0x123456789abcdef0ll);
    CHECK_STR(thrown(), "");
    /* Compiled, where the host runs Thumb-2. */
    c = mg_dvm_find_class("t/Jit");
#if defined(__arm__) || defined(_M_ARM)
    {
        static const struct { const char *name, *sig; } compiled[] = {
            { "sum", "([I)I" }, { "fill", "([I[B[C[SI)I" }, { "mix", "(II)I" }, { "conditions", "(I)I" }, { "divideAll", "(I[I)I" },
            { "readTo", "([II)I" }, { "wideMoves", "(IJ)J" }, { "fields", "(I)I" }, { "callsInLoop", "(I)I" },
            { "callKinds", "(I)I" }, { "catchInLoop", "(I)I" }, { "deepCalls", "(I)I" }, { "longs", "(IJ)J" }, { "switches", "(I)I" },
            { "objects", "(I)I" }, { "storeFails", "(I)I" }, { "castFails", "(I)I" },
        };
        size_t k;
        for (k = 0; k < MG_COUNT(compiled); k++) {
            mg_dvm_method *m = c ? mg_dvm_find_method(c, compiled[k].name, compiled[k].sig, true) : NULL;
            CHECK(m && m->jit_state == 1 && m->jit);
            if (!m || !m->jit) say("  t.Jit.%s%s was not compiled", compiled[k].name, compiled[k].sig);
        }
    }
#else
    (void)c;
#endif
    g_dvm_jit_threshold = was;
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
        path = mg_sprintf("%s/%c.dex", fixtures, 'a' + i);
        if (!mg_dvm_add_dex_file(path)) ok = false;
        free(path);
    }
    added = true;
    return ok;
}

void mg_dvm_tests_bench(const char *fixtures, const char *android_root, int ms)
{
    static const int rounds = 20;
    u64 t0, t1;
    int i;
    i64 units;
    /* On libcore: the benchmark of mg_dvm_bench.c, whose items need java.util. */
    if (android_root) {
        mg_log_set_level(MG_LOG_ERROR);
        if (!mg_dvm_start_android(android_root)) {
            say("the runtime did not start on the Android root %s", android_root);
            return;
        }
        say("benchmark on libcore (%s, about %d ms an item)", mg_host_name(), ms);
        mg_dvm_bench_run(fixtures, ms);
        return;
    }
    if (!mg_dvm_start(NULL, 0) || !add_fixtures(fixtures)) {
        say("the runtime did not start, or its fixtures are not in %s", fixtures);
        return;
    }
    say("benchmark (%s)", mg_host_name());
    /* sumTo(n): four instructions a turn (if-lez, add-int/2addr, add-int/lit8, goto). */
    t0 = mg_time_monotonic_ns();
    for (i = 0; i < rounds; i++) {
        A[0].i = 5000000;
        call("t/Arith", "sumTo", "(I)I");
    }
    t1 = mg_time_monotonic_ns();
    units = (i64)rounds * 5000000 * 4;
    say("  a counting loop: %.0f million instructions a second", (double)units * 1000.0 / (double)(t1 - t0));
    /* fib(27): 832040 calls returning 0 or 1 and 832039 that add; about 13 instructions and one call each. */
    t0 = mg_time_monotonic_ns();
    A[0].i = 27;
    call("t/Arith", "fib", "(I)I");
    t1 = mg_time_monotonic_ns();
    say("  recursive calls: %.1f million calls a second", 317811.0 * 2 * 1000.0 / (double)(t1 - t0));
}

int mg_dvm_tests_run(const mg_dvm_test_config *config, mg_test_sink sink, void *user, int *checks)
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
    /*
     * The collector runs all through the tests, often (a budget of 1 MB), with its poison on: an object freed while something
     * still used it would read as 0xDB.
     */
    mg_dvm_gc_set_poison(true);
    mg_dvm_gc_set_min_budget(1u << 20);
    /* Much of what is tested is refusal, and each refusal is a warning in the log: only errors are shown unless tracing is asked for. */
    mg_log_set_level(config->trace ? MG_LOG_INFO : MG_LOG_ERROR);
    say("dalvik runtime (%s, %u-bit registers, %u bytes of header to an object, %u registers to a frame)", mg_host_name(), (unsigned)(sizeof(mg_dvm_reg) * 8),
        (unsigned)MG_DVM_HEADER, (unsigned)((sizeof(mg_dvm_frame) + sizeof(mg_dvm_reg) - 1) / sizeof(mg_dvm_reg)));
    if (config->android_root) {
        /* On Android's own libcore: started, asked what only libcore can answer, and then given the same fixtures as the built-in java.lang. */
        u64 t0 = mg_time_monotonic_ns();
        size_t unresolved = mg_ld_unresolved_count();
        mg_dvm_stats at_start;
        CHECK(mg_dvm_start_android(config->android_root));
        /* libcore's own libraries, where they were loaded, found every function they import from the system's. */
        CHECK_INT(mg_ld_unresolved_count() - unresolved, 0);
        if (!g_failed) {
            g_libcore = true;
            mg_dvm_get_stats(&at_start);
            mg_dvm_set_trace(config->trace);
            test_libcore(config, mg_time_monotonic_ns() - t0, &at_start);
        }
    } else {
        CHECK(mg_dvm_start(NULL, 0));
    }
    CHECK(add_fixtures(config->fixtures));
    if (!g_failed) {
        mg_dvm_set_trace(config->trace);
        test_ops();
        test_arith();
        test_jit();
        test_two_files();
        test_language();
        test_runtime(config->fixtures);
        if (config->scratch) test_lazy_dex(config->fixtures, config->scratch);
        test_gc();
        test_jni(config->guest_dir, config->fixtures);
        if (!g_libcore) test_d8(config->fixtures);            /* on libcore that program has been run already, with the other */
        if (config->apk) test_real_apk(config->apk);
        if (g_libcore) test_libcore_nio();
        if (g_libcore) test_libcore_link_all();
        mg_dvm_set_trace(0);
    } else {
        say("  (the fixtures are made by tools/build-dvm-fixtures.sh and looked for in %s)", config->fixtures);
    }
    mg_log_set_level(MG_LOG_INFO);
    *checks = g_checks;
    return g_failed;
}
