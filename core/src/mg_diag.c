/*
 * megler_selftest and megler_probe: what this device does with the mechanisms the
 * layer rests on, measured rather than assumed.
 */
#include "../include/megler.h"

#include <stdio.h>

#include "../tests/mg_net_tests.h"
#include "../tests/mg_storage_tests.h"
#include "../tests/mg_tests.h"
#include "arm/mg_stubs.h"
#include "bionic/mg_bionic.h"
#include "mg_core.h"
#include "host/mg_host.h"
#include "ld/mg_ld.h"
#include "ndk/mg_egl.h"
#include "run/mg_run.h"

/* Appends a line to a file and has it on disk before returning: a step that takes the process down has still said how far things got. */
static void append_line(const char *path, const char *text)
{
    mg_file *f = path ? mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_APPEND, NULL) : NULL;
    if (!f) return;
    mg_file_write(f, text, strlen(text), NULL);
    mg_file_write(f, "\n", 1, NULL);
    mg_file_sync(f);
    mg_file_close(f);
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
    char *libs = mg_sprintf("%s/selftest", megler_package_dir()), *scratch = mg_sprintf("%s/selftest-scratch", megler_data_dir());
    selftest_out s;
    int failed;
    s.out = out;
    s.user = user;
    s.steps = megler_data_dir()[0] ? mg_sprintf("%s/megler-selftest-steps.log", megler_data_dir()) : NULL;
    if (s.steps) mg_path_remove(s.steps, NULL);
    failed = mg_tests_run(libs, scratch, selftest_line, &s);
    free(s.steps);
    free(libs);
    free(scratch);
    return failed;
}

/*
 * megler_storage_check: the steps are the tests' own (tests/mg_storage_tests.c,
 * mg_storage_check_run), which the tests run on a stand-in for the shell.
 * Here they run on what the shell really hands over.
 */
int megler_storage_check(const megler_storage_host *storage, megler_line_fn out, void *user, int *checks)
{
    const char *names[MEGLER_STORAGE_MAX_LINKS];
    char line[160];
    int i, count = 0, skipped = 0;
    if (mg_run_active()) {
        if (out) out("FAIL an app is running: the storage's links are not set under it", user);
        if (checks) (*checks)++;
        return 1;
    }
    mg_run_storage(megler_storage_dir(), storage);
    for (i = 0; storage && storage->links && i < storage->link_count && i < MEGLER_STORAGE_MAX_LINKS; i++) {
        /* (A link that is only to be read has nothing here that could be tried without leaving something behind.) */
        if (storage->links[i].writable) names[count++] = storage->links[i].name;
        else skipped++;
    }
    snprintf(line, sizeof(line), "the phone's folders in an app's storage: %d to go through%s", count, skipped ? ", and some that are only to be read are left alone" : "");
    if (out) out(line, user);
    return mg_storage_check_run(names, count, out, user, checks);
}

/*
 * megler_net_check: the steps are the tests' own (tests/mg_net_tests.c,
 * mg_net_check_run), which the tests run without the internet. Here the
 * internet is asked too: the name and the path Android's own connectivity
 * check asks for, which answers 204 and nothing else.
 */
int megler_net_check(megler_line_fn out, void *user, int *checks, int *refused)
{
    if (out) out("the network as an app finds it", user);
    return mg_net_check_run("connectivitycheck.gstatic.com", "/generate_204", out, user, checks, refused);
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
    mg_info("probe: %s", text);
    append_line(g_probe_file, text);
    if (g_out) g_out(text, g_out_user);
}

#if MG_HOST_ARM32

/*
 * A probe may leave the thread somewhere it cannot continue from: ARM code
 * decoded as Thumb runs off anywhere. So each one is run from a saved point
 * (the setjmp stub's register block), and any fault on the probing thread
 * while a probe is armed goes back to it.
 */
static u32 g_env[32];
static volatile i32 g_armed;
static u32 g_probe_thread, g_fault_code;
static mg_addr g_fault_pc;
static u32 g_fault_cpsr;
static bool g_arm_fault_seen;

bool mg_probe_fix_fault(mg_fault *f)
{
    if (!g_armed || mg_thread_self_id() != g_probe_thread || !f->regs) return false;
    g_fault_code = f->code;
    g_fault_pc = f->regs[15];
    g_fault_cpsr = f->cpsr ? *f->cpsr : 0;
    if ((f->regs[15] & ~1u) == (u32)(mg_addr)mg_stub(MG_STUB_ARM_UDF_AT) && !g_arm_fault_seen) {
        /* The undefined instruction taken in ARM state on purpose: step over it and stay in ARM state. */
        g_arm_fault_seen = true;
        mg_fault_resume_at(f, (mg_addr)mg_stub(MG_STUB_ARM_UDF_AT) + 4);
        return true;
    }
    mg_atomic_xchg(&g_armed, 0);
    /* What mg_stub_setjmp saved: r4-r11, sp, lr. Resume as if it returned 1. */
    memcpy(&f->regs[4], &g_env[0], 8 * sizeof(u32));
    f->regs[13] = g_env[8];
    mg_fault_resume_at(f, g_env[9]);
    f->regs[0] = 1;
    return true;
}

typedef int (*setjmp_fn)(u32 *);

static void probe_arm_state(void)
{
    static const u32 steps[] = { 1000u, 1000000u, 30000000u, 300000000u, 1500000000u };
    setjmp_fn sj = (setjmp_fn)mg_bionic_find("setjmp");
    u32 (*spin)(u32) = (u32 (*)(u32))mg_stub(MG_STUB_ARM_SPIN);
    int (*once)(int) = (int (*)(int))mg_stub(MG_STUB_ARM_ONCE);
    void *entry = mg_stub(MG_STUB_ARM_ONCE_ENTRY);
    volatile size_t i;
    volatile u64 t0;
    volatile u32 before;
    if (!sj || !spin || !once) {
        line("arm-state: not measured (no stubs)");
        return;
    }
    g_probe_thread = mg_thread_self_id();

    /* First, the safety net itself: a deliberate undefined instruction has to arrive here and be resumed from. */
    line("fault-recovery: trying (if this is the last line, a fault in generated code is not reaching the handler)");
    mg_atomic_xchg(&g_armed, 1);
    if (sj(g_env) == 0) {
        ((void (*)(void))mg_stub(MG_STUB_UDF))();
        mg_atomic_xchg(&g_armed, 0);
        line("fault-recovery: FAILED -- an undefined instruction ran without a fault");
        return;
    }
    /* The raw pc: whether the host keeps the Thumb bit there (host/mg_host.h, mg_fault_resume_at). */
    line("fault-recovery: ok (code 0x%x, pc 0x%08x, cpsr 0x%08x)", g_fault_code, (unsigned)g_fault_pc, g_fault_cpsr);

    /* One ARM instruction and back: what every call through a rewritten PLT entry does. */
    line("arm-one-instruction: trying");
    mg_atomic_xchg(&g_armed, 1);
    if (sj(g_env) == 0) {
        int r = once(41);
        mg_atomic_xchg(&g_armed, 0);
        line("arm-one-instruction: %s", r == 42 ? "ok" : "wrong result");
    } else {
        line("arm-one-instruction: FAULT code 0x%x at pc 0x%08x cpsr 0x%08x", g_fault_code, (unsigned)g_fault_pc, g_fault_cpsr);
    }

    /* The same bytes entered in Thumb state: the illegal instruction and its rescue, timed. */
    before = mg_core_plt_rescues();
    t0 = mg_time_monotonic_ns();
    for (i = 0; i < 200; i++)
        if (((int (*)(int))((mg_addr)entry | 1u))((int)i) != (int)i) break;
    if (i == 200 && mg_core_plt_rescues() - before == 200)
        line("plt-rescue: ok, %u us each", (unsigned)((mg_time_monotonic_ns() - t0) / 200 / 1000));
    else
        line("plt-rescue: FAILED after %u (rescued %u)", (unsigned)i, (unsigned)(mg_core_plt_rescues() - before));

    /* A thread kept in ARM state for longer and longer: does it come back? */
    for (i = 0; i < MG_COUNT(steps); i++) {
        line("arm-state: trying %u iterations", steps[i]);
        mg_atomic_xchg(&g_armed, 1);
        t0 = mg_time_monotonic_ns();
        if (sj(g_env) == 0) {
            u32 r = spin(steps[i]);
            u64 ms = (mg_time_monotonic_ns() - t0) / 1000000ull;
            mg_atomic_xchg(&g_armed, 0);
            if (r != 0x600DC0DEu) {
                line("arm-state: after %u iterations (%u ms) returned 0x%08x -- ARM state was lost without a fault", steps[i], (unsigned)ms, r);
                mg_core_set_arm_state(-1);
                return;
            }
            line("arm-state: survived %u iterations, %u ms", steps[i], (unsigned)ms);
        } else {
            line("arm-state: LOST within %u ms of %u iterations (fault 0x%x at pc 0x%08x, cpsr 0x%08x)",
                 (unsigned)((mg_time_monotonic_ns() - t0) / 1000000ull), steps[i], g_fault_code, (unsigned)g_fault_pc, g_fault_cpsr);
            line("arm-state: verdict: ARM-mode code cannot run as it is on this device; it has to be translated to Thumb-2");
            mg_core_set_arm_state(-1);
            return;
        }
    }
    line("arm-state: verdict: a thread stays in ARM state across interrupts on this device");
    mg_core_set_arm_state(1);
}

/*
 * A fault taken in ARM state: is it reported as one, and can the thread go on
 * in ARM state after it? Run last of all, since nothing else depends on it.
 */
static void probe_arm_fault(void)
{
    setjmp_fn sj = (setjmp_fn)mg_bionic_find("setjmp");
    if (!sj || !mg_stub(MG_STUB_ARM_UDF)) return;
    g_probe_thread = mg_thread_self_id();
    line("arm-fault: trying");
    g_arm_fault_seen = false;
    mg_atomic_xchg(&g_armed, 1);
    if (sj(g_env) == 0) {
        u32 r = ((u32 (*)(void))mg_stub(MG_STUB_ARM_UDF))();
        mg_atomic_xchg(&g_armed, 0);
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
    u32 (*ro)(void) = (u32 (*)(void))mg_stub(MG_STUB_READ_TPIDRURO), (*rw)(void) = (u32 (*)(void))mg_stub(MG_STUB_READ_TPIDRURW);
    u32 opc2 = 0, offset = 0;
    if (ro) line("tpidruro: 0x%08x (the register Android code reads)", ro());
    if (rw) line("tpidrurw: 0x%08x", rw());
    if (mg_guest_tp_where(&opc2, &offset)) line("guest-thread-pointer: c13,c0,%u + 0x%x", opc2, offset);
    else line("guest-thread-pointer: NOT AVAILABLE -- thread-pointer reads cannot be redirected");
}

static void probe_memory(void)
{
    u8 *p = (u8 *)mg_mem_alloc(65536);
    u64 t0;
    int i;
    if (!p) {
        line("exec-memory: allocation failed");
        return;
    }
    t0 = mg_time_monotonic_ns();
    for (i = 0; i < 200; i++)
        if (!mg_mem_protect(p, 4096, MG_PROT_RX) || !mg_mem_protect(p, 4096, MG_PROT_RW)) break;
    if (i == 200) line("exec-memory: ok, a write/execute flip pair of one page takes %u us", (unsigned)((mg_time_monotonic_ns() - t0) / 200 / 1000));
    else line("exec-memory: REFUSED (flip %d) -- is the codeGeneration capability granted?", i);
    t0 = mg_time_monotonic_ns();
    for (i = 0; i < 200; i++) mg_icache_flush(p, 4096);
    line("icache-flush: %u us per page", (unsigned)((mg_time_monotonic_ns() - t0) / 200 / 1000));
    mg_mem_release(p, 65536);
}

#endif /* MG_HOST_ARM32 */

void megler_probe(megler_line_fn out, void *user)
{
    g_out = out;
    g_out_user = user;
    free(g_probe_file);
    g_probe_file = megler_data_dir()[0] ? mg_sprintf("%s/megler-probe-steps.log", megler_data_dir()) : NULL;
    if (g_probe_file) mg_path_remove(g_probe_file, NULL);
    line("host: %s, %d processors", mg_host_name(), mg_cpu_count());
    line("memory: limit %u MB, in use %u MB", (unsigned)(mg_mem_limit() >> 20), (unsigned)(mg_mem_usage() >> 20));
    line("can-run-guest-code: %s", mg_core_can_run() ? "yes" : "no");
#if MG_HOST_ARM32
    if (mg_core_can_run()) {
        probe_memory();
        probe_thread_pointer();
        probe_arm_state();
    }
#endif
    mg_egl_probe(line);
#if MG_HOST_ARM32
    if (mg_core_can_run()) probe_arm_fault();
#endif
    line("done");
    g_out = NULL;
}
