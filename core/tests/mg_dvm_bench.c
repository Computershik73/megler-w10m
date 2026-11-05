/*
 * The interpreter's benchmark, on Android's libcore: the methods of bench.Bench (core/tests/dvm/bench/src/bench/Bench.java,
 * compiled into bench.dex beside the other fixtures), each of which does one kind of work `n` times. Each is run with an `n`
 * that makes it last about as long as asked, then timed, and the instructions the interpreter executed for it are counted
 * (mg_dvm_thread_costs). A line for each: nanoseconds a turn, instructions a turn, and millions of instructions a second over
 * the whole of its time (native methods and allocation included, since that is what a turn costs).
 *
 * Printed, not checked: the numbers are this machine's. `mg_tests --dvm-bench <fixtures> <android root>` runs it on its own
 * with longer runs; the libcore tests run it short (the phone's Self-test then says the phone's own numbers).
 */
#include "mg_dvm_tests.h"

#include <stdio.h>
#include <stdlib.h>

#include "../src/host/mg_host.h"

typedef struct bench_item {
    const char *method;
    const char *what;
} bench_item;

static const bench_item k_items[] = {
    { "arith", "int arithmetic, a loop turn" },
    { "wide", "long and double arithmetic" },
    { "fields", "iget and iput on one object" },
    { "statics", "sget and sput" },
    { "invokeStatic", "invoke-static of a small method" },
    { "invokeVirtual", "invoke-virtual, one class" },
    { "invokeFinal", "invoke-virtual of a final method" },
    { "invokeInterface", "invoke-interface, one class" },
    { "invokeInterface2", "invoke-interface, two classes" },
    { "arrays", "aget and aput, int[256]" },
    { "charAt", "String.charAt and length" },
    { "equalsStr", "String.equals, 23 characters" },
    { "hashCodeStr", "String.hashCode, kept" },
    { "indexOf", "String.indexOf, a char near the end" },
    { "stringBuilder", "StringBuilder: 3 appends, toString" },
    { "hashMap", "HashMap.get of 1000 (put 1 in 16)" },
    { "arrayList", "ArrayList iterator, an element" },
    { "alloc", "a small object made" },
    { "allocArray", "an int[16] made" },
    { "sync", "synchronized block, no contention" },
    { "syncMethod", "synchronized static method" },
    { "exceptions", "exception made, thrown, caught" },
    { "throwPremade", "exception thrown and caught" },
    { "math", "Math.min, max, abs" },
    { "arraycopy", "System.arraycopy of 16 ints" },
    { "nativeSin", "Math.sin, a native method by JNI" },
    { "boxing", "Integer.valueOf and intValue" },
    { "classes", "getClass, instance-of, check-cast" },
};

/*
 * A sampling profile of one item, for finding where its time goes (MG_BENCH_PROFILE=<method>, with the samples written to
 * the file MG_BENCH_PROFILE_OUT): a second thread stops the running one every 20 microseconds or so and notes where it was.
 * The addresses go to the file one a line, after the address of this function, so that a linker map can name them
 * (tools/dvm-bench.sh profile does).
 */
typedef struct sampler {
    mg_thread *target;
    volatile i32 stop;
    mg_addr *pcs;
    u32 n, cap;
} sampler;

static MG_HOSTCB void sampler_main(void *arg)
{
    sampler *s = (sampler *)arg;
    while (!s->stop && s->n < s->cap) {
        mg_thread_regs regs;
        u64 until = mg_time_monotonic_ns() + 20000;
        if (mg_thread_suspend(s->target, &regs)) {
            s->pcs[s->n++] = regs.pc;
            mg_thread_resume(s->target);
        }
        while (mg_time_monotonic_ns() < until) {
        }
    }
}

static void write_samples(const sampler *s, const char *path)
{
    FILE *f = fopen(path, "w");
    u32 i;
    if (!f) return;
    fprintf(f, "anchor %llx\n", (unsigned long long)(mg_addr)(void *)mg_dvm_bench_run);
    for (i = 0; i < s->n; i++) fprintf(f, "%llx\n", (unsigned long long)s->pcs[i]);
    fclose(f);
}

/* One run of an item: false when it threw. */
static bool run_once(const char *method, i32 n, u64 *ns, u64 *executed)
{
    mg_dvm_costs before, after;
    u64 t0;
    mg_jvalue r;
    A[0].i = n;
    mg_dvm_thread_costs(&before);
    t0 = mg_time_monotonic_ns();
    r = call("bench/Bench", method, "(I)I");
    *ns = mg_time_monotonic_ns() - t0;
    mg_dvm_thread_costs(&after);
    *executed = after.executed - before.executed;
    (void)r;
    return thrown()[0] == 0;
}

bool mg_dvm_bench_add(const char *fixtures)
{
    static int added;
    if (!added) {
        char *dex = mg_sprintf("%s/bench.dex", fixtures);
        added = mg_dvm_add_dex_file(dex) ? 1 : -1;
        if (added < 0) say("  (no bench.dex in %s)", fixtures);
        free(dex);
    }
    return added > 0;
}

void mg_dvm_bench_run(const char *fixtures, int ms)
{
    u64 all_ns = 0, all_executed = 0, target = (u64)(ms > 0 ? ms : 50) * 1000000ull;
    size_t i;
    if (!mg_dvm_bench_add(fixtures)) return;
    say("  %-36s %10s %8s %8s", "", "ns a turn", "insns", "M/s");
    for (i = 0; i < MG_COUNT(k_items); i++) {
        u64 ns = 0, executed = 0;
        i32 n = 64;
        bool ok = true;
        /* (Profiling one item: that one alone.) */
        if (getenv("MG_BENCH_PROFILE") && strcmp(getenv("MG_BENCH_PROFILE"), k_items[i].method)) continue;
        /* Once briefly, so that classes are linked and initialised, and then with more turns until a run lasts a tenth of the time. */
        ok = run_once(k_items[i].method, 16, &ns, &executed);
        while (ok) {
            ok = run_once(k_items[i].method, n, &ns, &executed);
            if (!ok || ns >= target / 10 || n >= (1 << 28)) break;
            {
                u64 more = ns < 1000 ? (u64)n * 16 : (u64)n * (target / 10) / ns + 1;
                n = (i32)(more > (u64)n * 16 ? (u64)n * 16 : more < (u64)n * 2 ? (u64)n * 2 : more);
            }
        }
        /* The run that counts: as many turns as fill the time asked for, from what the last one took. */
        if (ok && ns < target) {
            u64 want = ns ? (u64)n * target / ns : (u64)n * 4;
            n = (i32)(want > (u64)(1 << 30) ? (u64)(1 << 30) : want);
            ok = run_once(k_items[i].method, n, &ns, &executed);
        }
        if (!ok) {
            say("  %-36s threw", k_items[i].what);
            continue;
        }
        /* The best of three: what another program on the machine takes from a run is noise, and only ever more time. */
        if (ok) {
            int k;
            for (k = 0; k < 2 && ok; k++) {
                u64 ns2 = 0, executed2 = 0;
                ok = run_once(k_items[i].method, n, &ns2, &executed2);
                if (ok && ns2 < ns) {
                    ns = ns2;
                    executed = executed2;
                }
            }
        }
        if (getenv("MG_BENCH_PROFILE") && !strcmp(getenv("MG_BENCH_PROFILE"), k_items[i].method)) {
            static sampler smp;
            mg_thread *th;
            u64 ns2 = 0, executed2 = 0;
            smp.target = mg_thread_current();
            smp.cap = 1u << 20;
            smp.pcs = (mg_addr *)mg_alloc(sizeof(mg_addr) * smp.cap);
            smp.n = 0;
            smp.stop = 0;
            th = mg_thread_start(sampler_main, &smp, 64 * 1024, "sampler");
            run_once(k_items[i].method, n * 10, &ns2, &executed2);
            mg_atomic_xchg(&smp.stop, 1);
            mg_thread_join(th);
            say("  (%u samples of %s)", (unsigned)smp.n, k_items[i].method);
            if (getenv("MG_BENCH_PROFILE_OUT")) write_samples(&smp, getenv("MG_BENCH_PROFILE_OUT"));
            free(smp.pcs);
        }
        all_ns += ns;
        all_executed += executed;
        say("  %-36s %10.1f %8.1f %8.1f", k_items[i].what, (double)ns / n, (double)executed / n, ns ? (double)executed * 1000.0 / (double)ns : 0.0);
    }
    say("  in all: %.1f M instructions in %.0f ms, %.1f M a second", (double)all_executed / 1e6, (double)all_ns / 1e6,
        all_ns ? (double)all_executed * 1000.0 / (double)all_ns : 0.0);
}
