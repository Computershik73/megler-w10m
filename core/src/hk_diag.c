/*
 * megler_selftest and megler_probe: what this device does with the mechanisms the
 * layer rests on, measured rather than assumed.
 */
#include "../include/megler.h"

#include <stdio.h>

#include "../tests/hk_tests.h"
#include "arm/hk_stubs.h"
#include "bionic/hk_bionic.h"
#include "hk_core.h"
#include "host/hk_host.h"
#include "ld/hk_ld.h"
#include "ndk/hk_egl.h"
#include "run/hk_run.h"

/* Appends a line to a file and has it on disk before returning: a step that takes the process down has still said how far things got. */
static void append_line(const char *path, const char *text)
{
    hk_file *f = path ? hk_file_open(path, HK_O_WRITE | HK_O_CREATE | HK_O_APPEND, NULL) : NULL;
    if (!f) return;
    hk_file_write(f, text, strlen(text), NULL);
    hk_file_write(f, "\n", 1, NULL);
    hk_file_sync(f);
    hk_file_close(f);
}

typedef struct selftest_out {
    megler_line_fn out;
    void *user;
    char *steps;
} selftest_out;

static void selftest_line(const char *text, void *user)
{
    selftest_out *s = (selftest_out *)user;
    append_line(s->steps, text);
    if (s->out) s->out(text, s->user);
}

int megler_selftest(megler_line_fn out, void *user)
{
    char *libs = hk_sprintf("%s/selftest", megler_package_dir()), *scratch = hk_sprintf("%s/selftest-scratch", megler_data_dir());
    selftest_out s;
    int failed;
    s.out = out;
    s.user = user;
    s.steps = megler_data_dir()[0] ? hk_sprintf("%s/megler-selftest-steps.log", megler_data_dir()) : NULL;
    if (s.steps) hk_path_remove(s.steps, NULL);
    failed = hk_tests_run(libs, scratch, selftest_line, &s);
    free(s.steps);
    free(libs);
    free(scratch);
    return failed;
}

static megler_line_fn g_out;
static void *g_out_user;

static char *g_probe_file;

static void line(const char *fmt, ...)
{
    char text[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    hk_info("probe: %s", text);
    append_line(g_probe_file, text);
    if (g_out) g_out(text, g_out_user);
}

#if HK_HOST_ARM32

/*
 * A probe may leave the thread somewhere it cannot continue from: ARM code
 * decoded as Thumb runs off anywhere. So each one is run from a saved point
 * (the setjmp stub's register block), and any fault on the probing thread
 * while a probe is armed goes back to it.
 */
static u32 g_env[32];
static volatile i32 g_armed;
static u32 g_probe_thread, g_fault_code;
static hk_addr g_fault_pc;
static u32 g_fault_cpsr;
static bool g_arm_fault_seen;

bool hk_probe_fix_fault(hk_fault *f)
{
    if (!g_armed || hk_thread_self_id() != g_probe_thread || !f->regs) return false;
    g_fault_code = f->code;
    g_fault_pc = f->regs[15];
    g_fault_cpsr = f->cpsr ? *f->cpsr : 0;
    if ((f->regs[15] & ~1u) == (u32)(hk_addr)hk_stub(HK_STUB_ARM_UDF_AT) && !g_arm_fault_seen) {
        /* The undefined instruction taken in ARM state on purpose: step over it and stay in ARM state. */
        g_arm_fault_seen = true;
        hk_fault_resume_at(f, (hk_addr)hk_stub(HK_STUB_ARM_UDF_AT) + 4);
        return true;
    }
    hk_atomic_xchg(&g_armed, 0);
    /* What hk_stub_setjmp saved: r4-r11, sp, lr. Resume as if it returned 1. */
    memcpy(&f->regs[4], &g_env[0], 8 * sizeof(u32));
    f->regs[13] = g_env[8];
    hk_fault_resume_at(f, g_env[9]);
    f->regs[0] = 1;
    return true;
}

typedef int (*setjmp_fn)(u32 *);

static void probe_arm_state(void)
{
    static const u32 steps[] = { 1000u, 1000000u, 30000000u, 300000000u, 1500000000u };
    setjmp_fn sj = (setjmp_fn)hk_bionic_find("setjmp");
    u32 (*spin)(u32) = (u32 (*)(u32))hk_stub(HK_STUB_ARM_SPIN);
    int (*once)(int) = (int (*)(int))hk_stub(HK_STUB_ARM_ONCE);
    void *entry = hk_stub(HK_STUB_ARM_ONCE_ENTRY);
    volatile size_t i;
    volatile u64 t0;
    volatile u32 before;
    if (!sj || !spin || !once) {
        line("arm-state: not measured (no stubs)");
        return;
    }
    g_probe_thread = hk_thread_self_id();

    /* First, the safety net itself: a deliberate undefined instruction has to arrive here and be resumed from. */
    line("fault-recovery: trying (if this is the last line, a fault in generated code is not reaching the handler)");
    hk_atomic_xchg(&g_armed, 1);
    if (sj(g_env) == 0) {
        ((void (*)(void))hk_stub(HK_STUB_UDF))();
        hk_atomic_xchg(&g_armed, 0);
        line("fault-recovery: FAILED -- an undefined instruction ran without a fault");
        return;
    }
    /* The raw pc: whether the host keeps the Thumb bit there (host/hk_host.h, hk_fault_resume_at). */
    line("fault-recovery: ok (code 0x%x, pc 0x%08x, cpsr 0x%08x)", g_fault_code, (unsigned)g_fault_pc, g_fault_cpsr);

    /* One ARM instruction and back: what every call through a rewritten PLT entry does. */
    line("arm-one-instruction: trying");
    hk_atomic_xchg(&g_armed, 1);
    if (sj(g_env) == 0) {
        int r = once(41);
        hk_atomic_xchg(&g_armed, 0);
        line("arm-one-instruction: %s", r == 42 ? "ok" : "wrong result");
    } else {
        line("arm-one-instruction: FAULT code 0x%x at pc 0x%08x cpsr 0x%08x", g_fault_code, (unsigned)g_fault_pc, g_fault_cpsr);
    }

    /* The same bytes entered in Thumb state: the illegal instruction and its rescue, timed. */
    before = hk_core_plt_rescues();
    t0 = hk_time_monotonic_ns();
    for (i = 0; i < 200; i++)
        if (((int (*)(int))((hk_addr)entry | 1u))((int)i) != (int)i) break;
    if (i == 200 && hk_core_plt_rescues() - before == 200)
        line("plt-rescue: ok, %u us each", (unsigned)((hk_time_monotonic_ns() - t0) / 200 / 1000));
    else
        line("plt-rescue: FAILED after %u (rescued %u)", (unsigned)i, (unsigned)(hk_core_plt_rescues() - before));

    /* A thread kept in ARM state for longer and longer: does it come back? */
    for (i = 0; i < HK_COUNT(steps); i++) {
        line("arm-state: trying %u iterations", steps[i]);
        hk_atomic_xchg(&g_armed, 1);
        t0 = hk_time_monotonic_ns();
        if (sj(g_env) == 0) {
            u32 r = spin(steps[i]);
            u64 ms = (hk_time_monotonic_ns() - t0) / 1000000ull;
            hk_atomic_xchg(&g_armed, 0);
            if (r != 0x600DC0DEu) {
                line("arm-state: after %u iterations (%u ms) returned 0x%08x -- ARM state was lost without a fault", steps[i], (unsigned)ms, r);
                hk_core_set_arm_state(-1);
                return;
            }
            line("arm-state: survived %u iterations, %u ms", steps[i], (unsigned)ms);
        } else {
            line("arm-state: LOST within %u ms of %u iterations (fault 0x%x at pc 0x%08x, cpsr 0x%08x)",
                 (unsigned)((hk_time_monotonic_ns() - t0) / 1000000ull), steps[i], g_fault_code, (unsigned)g_fault_pc, g_fault_cpsr);
            line("arm-state: verdict: ARM-mode code cannot run as it is on this device; it has to be translated to Thumb-2");
            hk_core_set_arm_state(-1);
            return;
        }
    }
    line("arm-state: verdict: a thread stays in ARM state across interrupts on this device");
    hk_core_set_arm_state(1);
}

/*
 * A fault taken in ARM state: is it reported as one, and can the thread go on
 * in ARM state after it? Run last of all, since nothing else depends on it.
 */
static void probe_arm_fault(void)
{
    setjmp_fn sj = (setjmp_fn)hk_bionic_find("setjmp");
    if (!sj || !hk_stub(HK_STUB_ARM_UDF)) return;
    g_probe_thread = hk_thread_self_id();
    line("arm-fault: trying");
    g_arm_fault_seen = false;
    hk_atomic_xchg(&g_armed, 1);
    if (sj(g_env) == 0) {
        u32 r = ((u32 (*)(void))hk_stub(HK_STUB_ARM_UDF))();
        hk_atomic_xchg(&g_armed, 0);
        if (g_arm_fault_seen && r == 0x600DC0DEu)
            line("arm-fault: ok, reported with pc 0x%08x cpsr 0x%08x and resumed in ARM state", (unsigned)g_fault_pc, g_fault_cpsr);
        else
            line("arm-fault: WRONG (fault %s, returned 0x%08x)", g_arm_fault_seen ? "seen" : "not seen", r);
    } else {
        line("arm-fault: FAILED: after the fault at pc 0x%08x cpsr 0x%08x, a second one (0x%x) instead of going on", (unsigned)g_fault_pc,
             g_fault_cpsr, g_fault_code);
    }
}

static void probe_thread_pointer(void)
{
    u32 (*ro)(void) = (u32 (*)(void))hk_stub(HK_STUB_READ_TPIDRURO), (*rw)(void) = (u32 (*)(void))hk_stub(HK_STUB_READ_TPIDRURW);
    u32 opc2 = 0, offset = 0;
    if (ro) line("tpidruro: 0x%08x (the register Android code reads)", ro());
    if (rw) line("tpidrurw: 0x%08x", rw());
    if (hk_guest_tp_where(&opc2, &offset)) line("guest-thread-pointer: c13,c0,%u + 0x%x", opc2, offset);
    else line("guest-thread-pointer: NOT AVAILABLE -- thread-pointer reads cannot be redirected");
}

static void probe_memory(void)
{
    u8 *p = (u8 *)hk_mem_alloc(65536);
    u64 t0;
    int i;
    if (!p) {
        line("exec-memory: allocation failed");
        return;
    }
    t0 = hk_time_monotonic_ns();
    for (i = 0; i < 200; i++)
        if (!hk_mem_protect(p, 4096, HK_PROT_RX) || !hk_mem_protect(p, 4096, HK_PROT_RW)) break;
    if (i == 200) line("exec-memory: ok, a write/execute flip pair of one page takes %u us", (unsigned)((hk_time_monotonic_ns() - t0) / 200 / 1000));
    else line("exec-memory: REFUSED (flip %d) -- is the codeGeneration capability granted?", i);
    t0 = hk_time_monotonic_ns();
    for (i = 0; i < 200; i++) hk_icache_flush(p, 4096);
    line("icache-flush: %u us per page", (unsigned)((hk_time_monotonic_ns() - t0) / 200 / 1000));
    hk_mem_release(p, 65536);
}

#endif /* HK_HOST_ARM32 */

void megler_probe(megler_line_fn out, void *user)
{
    g_out = out;
    g_out_user = user;
    free(g_probe_file);
    g_probe_file = megler_data_dir()[0] ? hk_sprintf("%s/megler-probe-steps.log", megler_data_dir()) : NULL;
    if (g_probe_file) hk_path_remove(g_probe_file, NULL);
    line("host: %s, %d processors", hk_host_name(), hk_cpu_count());
    line("memory: limit %u MB, in use %u MB", (unsigned)(hk_mem_limit() >> 20), (unsigned)(hk_mem_usage() >> 20));
    line("can-run-guest-code: %s", hk_core_can_run() ? "yes" : "no");
#if HK_HOST_ARM32
    if (hk_core_can_run()) {
        probe_memory();
        probe_thread_pointer();
        probe_arm_state();
    }
#endif
    hk_egl_probe(line);
#if HK_HOST_ARM32
    if (hk_core_can_run()) probe_arm_fault();
#endif
    line("done");
    g_out = NULL;
}
