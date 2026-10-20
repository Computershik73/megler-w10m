/* What the files of the Dalvik runtime's tests share: counting checks, and calling into the fixtures with few words. */
#ifndef MG_DVM_TESTS_H
#define MG_DVM_TESTS_H

#include "../src/dvm/mg_dvm.h"
#include "mg_tests.h"

/* These names are short for the tests' sake; to the linker, which has the whole app to keep apart, they are longer. */
#define g_checks mg_dvmt_checks
#define g_failed mg_dvmt_failed
#define say mg_dvmt_say
#define A mg_dvmt_args
#define call mg_dvmt_call
#define vcall mg_dvmt_vcall
#define thrown mg_dvmt_thrown
#define text_of mg_dvmt_text_of
#define test_language mg_dvmt_language
#define test_runtime mg_dvmt_runtime
#define test_jni mg_dvmt_jni
#define test_d8 mg_dvmt_d8
#define test_real_apk mg_dvmt_real_apk
#define test_libcore mg_dvmt_libcore
#define test_libcore_link_all mg_dvmt_libcore_link_all
#define test_libcore_nio mg_dvmt_libcore_nio
#define g_libcore mg_dvmt_on_libcore

extern int g_checks, g_failed;
/* Whether the run is on Android's libcore, not the built-in java.lang: the few checks whose right answer depends on which. */
extern bool g_libcore;
void say(const char *fmt, ...) MG_PRINTF(1, 2);

#define CHECK(cond)                                                    \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

#define CHECK_INT(expr, want)                                                                      \
    do {                                                                                           \
        long long got_ = (long long)(expr), want_ = (long long)(want);                             \
        g_checks++;                                                                                \
        if (got_ != want_) {                                                                       \
            g_failed++;                                                                            \
            say("  FAIL %s:%d: %s = %lld, expected %lld", __FILE__, __LINE__, #expr, got_, want_); \
        }                                                                                          \
    } while (0)

#define CHECK_STR(expr, want)                                                                          \
    do {                                                                                               \
        const char *got_ = (expr), *want_ = (want);                                                    \
        g_checks++;                                                                                    \
        if (!got_ || strcmp(got_, want_) != 0) {                                                       \
            g_failed++;                                                                                \
            say("  FAIL %s:%d: %s = \"%s\", expected \"%s\"", __FILE__, __LINE__, #expr, got_ ? got_ : "(null)", want_); \
        }                                                                                              \
    } while (0)

/* The arguments of the next call(); cleared by it. */
extern mg_jvalue A[16];
/* A static method of a fixture, with A as its arguments. When it throws, the result is zero and thrown() says what. */
mg_jvalue call(const char *cls, const char *name, const char *sig);
mg_jvalue vcall(mg_jobj *self, const char *name, const char *sig);
/* The pending exception as text, cleared; "" when there is none. */
const char *thrown(void);
/* A string object's text; "(null)" for null. */
const char *text_of(mg_jobj *s);

/* mg_dvm_tests_language.c: what the fixtures say about the bytecode: wide values, switches, arrays, exceptions, dispatch, initialisation, strings. */
void test_language(void);
/* mg_dvm_tests_runtime.c: natives, monitors and threads, limits, what is refused. */
void test_runtime(const char *fixtures);
/* mg_dvm_tests_gc.c: the collector, from the heap up. */
void test_gc(void);
/* mg_dvm_tests_jni.c: the runtime and the JNI layer as one Java world; with a guest library where guest code can run. */
void test_jni(const char *guest_dir);
/* mg_dvm_tests_d8.c: a program compiled by javac and d8, against what the JVM printed for it. */
void test_d8(const char *fixtures);
/*
 * mg_dvm_bench.c: the interpreter's benchmark on libcore (bench.dex among the fixtures), each item run for about `ms`
 * milliseconds; its table through say(). Printed, not checked.
 */
#define mg_dvm_bench_run mg_dvmt_bench_run
#define mg_dvm_bench_add mg_dvmt_bench_add
void mg_dvm_bench_run(const char *fixtures, int ms);
/* bench.dex added to the class path, once; false when it is not there. */
bool mg_dvm_bench_add(const char *fixtures);
/* mg_dvm_tests_libcore.c: what only a run on Android's libcore is asked. */
struct mg_dvm_stats;
void test_libcore(const mg_dvm_test_config *config, u64 started_ns, const struct mg_dvm_stats *at_start);
void test_libcore_link_all(void);
void test_libcore_nio(void);
/* mg_dvm_tests_real.c: a real app's DEX. */
void test_real_apk(const char *apk);

#endif
