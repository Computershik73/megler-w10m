/* Start-up of the core, and the one fault handler everything else hangs off. */
#include "../include/megler.h"
#include "mg_core.h"
#include "ndk/mg_egl.h"

#include "arm/mg_kuser.h"
#include "arm/mg_stubs.h"
#include "bionic/mg_bionic.h"
#include "host/mg_host.h"
#include "ld/mg_ld.h"

void mg_ctype_init(void);
void mg_time_init(void);
void mg_misc_init(void);
void mg_env_init(void);
bool mg_mmap_fix_fault(mg_addr address, bool write, bool exec, mg_addr pc);
bool mg_mmap_app_trap(mg_addr pc);
#if MG_HOST_ARM32
bool mg_probe_fix_fault(mg_fault *f);
#endif

static volatile i32 g_plt_rescues, g_wx_flips, g_svc_calls, g_svc_on_block;
static bool g_inited, g_can_run;
static void (*g_crash_hook)(const char *report);
static void (*g_fault_detail)(char *out, size_t cap);

/* The exceptions that are not memory or an instruction, by what Windows numbers them (a signal number says nothing here). */
static const char *fault_name(u32 code)
{
    switch (code) {
    case 0x80000003u: return "breakpoint (a trap the code was built with: an assertion, an unreachable place, abort)";
    case 0xC00000FDu: return "stack overflow";
    case 0xC0000094u: return "integer division by zero";
    case 0xC0000420u: return "assertion";
    case 0xC0000008u: return "invalid handle";
    default: return "exception";
    }
}

/*
 * Chromium ends an app out of memory in OnNoMemoryInternal (PartitionAlloc's, which base's TerminateBecauseOutOfMemory
 * calls): the size it was asked for goes into a global of its library, g_oom_size, and then it traps -- bkpt, then udf,
 * the second being where the phone's report finds the pc. The registers do not have the size by then, the global does.
 * The WebView's build (128.0.6613.88) compiles the function to
 *
 *     sub sp, #8; ldr r1, [pc, #n]; str r0, [sp, #4]; add r1, pc; str r0, [r1]; add r0, sp, #4; bl Alias; bkpt; udf
 *
 * and the global's address is the literal plus where `add r1, pc` reads pc. Another build that compiles it otherwise is
 * not recognised, and its report has no size.
 */
mg_addr mg_chromium_oom_global(mg_addr pc)
{
    const u16 *h = (const u16 *)pc;
    mg_addr f, literal;
    u32 offset;
    if (pc < 18 || (pc & 1)) return 0;
    if (h[0] == 0xde00 && h[-1] == 0xbe00) f = pc - 18;
    else if (h[0] == 0xbe00 && h[1] == 0xde00) f = pc - 16;
    else return 0;
    h = (const u16 *)f;
    if (h[0] != 0xb082 || (h[1] & 0xff00) != 0x4900 || h[2] != 0x9001 || h[3] != 0x4479 || h[4] != 0x6008 || h[5] != 0xa801
        || (h[6] & 0xf800) != 0xf000 || (h[7] & 0xd000) != 0xd000)
        return 0;
    literal = ((f + 2 + 4) & ~(mg_addr)3) + (mg_addr)(h[1] & 0xff) * 4;
    /* (It follows the function's end, as the compiler puts it: no further than f + 32, which the caller can check.) */
    if (literal + 4 > f + 32) return 0;
    memcpy(&offset, (const void *)literal, 4);
    /* (A 32-bit offset: added as a signed one, it lands where the Thumb add lands on any width of address.) */
    return f + 6 + 4 + (mg_addr)(intptr_t)(i32)offset;
}

const char *megler_data_dir(void);

#if !MG_ARCH_ARM32 && MG_GUEST_CODE
/* core/src/crt/mg_crt_setjmp.c: the core's own, of the processor's convention (docs/DESKTOP.md). */
int mg_setjmp(mg_addr *jb) __attribute__((returns_twice));
MG_NORETURN void mg_longjmp(mg_addr *jb, int value);
#if MG_ARCH_X86
/* compiler-rt's, which the core is linked with: x86's 64-bit division, which its libc gives guest code (libgcc's names). */
long long __divdi3(long long a, long long b);
long long __moddi3(long long a, long long b);
unsigned long long __udivdi3(unsigned long long a, unsigned long long b);
unsigned long long __umoddi3(unsigned long long a, unsigned long long b);
#endif

/*
 * What bionic gives guest code that is the processor's own code here, as 32-bit ARM's assembly stubs give it there
 * (arm/mg_stubs.c): setjmp and longjmp, which save and restore the registers the convention has the callee keep (the
 * core's own, which fit in bionic's jmp_buf of each processor; sigsetjmp's mask is not kept, as on ARM); and on x86 the
 * 64-bit division libgcc gave Android's libc. Without setjmp a guest's call of it went to the stub of a missing import,
 * which returned, and its function's frame was then torn ("stack corruption detected").
 */
static const mg_sym g_own_code[] = {
    MG_WRAP("setjmp", mg_setjmp), MG_WRAP("_setjmp", mg_setjmp), MG_WRAP("sigsetjmp", mg_setjmp),
    MG_WRAP("longjmp", mg_longjmp), MG_WRAP("_longjmp", mg_longjmp), MG_WRAP("siglongjmp", mg_longjmp),
#if MG_ARCH_X86
    MG_WRAP("__divdi3", __divdi3), MG_WRAP("__moddi3", __moddi3), MG_WRAP("__udivdi3", __udivdi3), MG_WRAP("__umoddi3", __umoddi3),
#endif
    MG_END
};
#endif

/* The fault's registers as a report says them, in the processor's own names. */
static void registers_line(const mg_fault *f, char *out, size_t cap)
{
    static const char *const names[MG_REG_COUNT] = {
#if MG_ARCH_ARM32
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
#elif MG_ARCH_X86_64
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"
#elif MG_ARCH_X86
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip"
#else
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
        "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "fp", "lr", "sp", "pc"
#endif
    };
    size_t n = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < MG_REG_COUNT && n + 32 < cap; i++) {
        if (i == MG_REG_PC) continue;
        n += (size_t)snprintf(out + n, cap - n, "%s%s %0*llx", i % 8 ? " " : "\n  ", names[i], (int)(sizeof(mg_addr) * 2),
                              (unsigned long long)f->regs[i]);
    }
    if (n + 24 < cap) snprintf(out + n, cap - n, " %s %08x", MG_ARCH_ARM32 ? "cpsr" : MG_ARCH_ARM64 ? "pstate" : "flags", f->cpsr ? *f->cpsr : 0);
}

#if MG_ARCH_ARM32
/*
 * The path a traced call by number names (open, openat, stat64, fstatat64, access, faccessat, readlink, readlinkat and the
 * rest), as said after its arguments: in quotes, its first 120 characters. Read after the call, which read it already: not
 * where the call said EFAULT. Google Play services' DroidGuard checks files by number, and the line said only an address.
 */
static void syscall_path(u32 nr, const u32 args[4], u32 result, char *out, size_t size)
{
    const char *p;
    size_t k = 0;
    int which;
    out[0] = 0;
    switch (nr) {
    case 5: case 10: case 33: case 38: case 39: case 40: case 85: case 195: case 196: case 266: which = 0; break;
    case 322: case 323: case 325: case 327: case 328: case 329: case 332: case 333: case 334: case 348: which = 1; break;
    default: return;
    }
    p = (const char *)(mg_addr)args[which];
    if (!p || (i32)result == -14 /* EFAULT */) return;
    out[k++] = ' ';
    out[k++] = '"';
    while (*p && k + 5 < size && k < 122) {
        out[k++] = *p >= 0x20 && *p < 0x7F ? *p : '?';
        p++;
    }
    if (*p) { out[k++] = '.'; out[k++] = '.'; }
    out[k++] = '"';
    out[k] = 0;
}

/* The flag trace-syscalls: each system call of a library's own said, with its number, its first four arguments, the path it
   names, what it gives and whose. */
static void trace_syscall(u32 nr, const u32 args[4], u32 result, mg_addr pc)
{
    static volatile i32 traced = -1, lines;
    if (traced < 0 && megler_data_dir()[0]) traced = mg_flag("trace-syscalls") ? 1 : 0;
    if (traced > 0 && mg_atomic_add(&lines, 1) <= 30000) {
        const char *where = NULL;
        const void *s = NULL, *b = NULL;
        char path[140];
        mg_ld_symbol_at((const void *)pc, &where, &s, &b);
        syscall_path(nr, args, result, path, sizeof(path));
        mg_info("syscall %u(0x%x, 0x%x, 0x%x, 0x%x)%s = %d on thread %u at %s+0x%x", (unsigned)nr, (unsigned)args[0], (unsigned)args[1],
                (unsigned)args[2], (unsigned)args[3], path, (int)result, (unsigned)mg_thread_self_id(), where ? where : "?", (unsigned)(pc - (mg_addr)b));
    }
}

/*
 * A system call of an ARM svc 0 led out of its library (arm/mg_patch.c, arm/mg_stubs.S's mg_stub_svc_entry), on the
 * thread's stack for system calls (`on_block`) or its own: regs r0..r15 as at the svc, r15 the piece's way back, whose
 * branch three words on goes to the instruction after the site. The site is what the call is said to be made at
 * (mg_bionic_syscall_site: madvise asks whose it is). On Windows the thread's record (mg_gthread) may be made by the
 * call, with the bounds Windows says of the thread's stack, which while the TEB says the block's are that one's: it is
 * given the thread's own. Elsewhere the block learns the thread's own stack, where it was not told it when it was made.
 */
u32 mg_arm_svc_call(u32 *regs, mg_addr *block, u32 on_block)
{
    u32 b = mg_rd32((const void *)(mg_addr)(regs[15] + 12)), args[4], result;
#if MG_HOST_WINDOWS
    void **tp = on_block ? (void **)mg_guest_tp_get() : NULL;
    bool made = tp && !tp[1];
#endif
    regs[15] = regs[15] + 16 + (u32)((i32)(b << 8) >> 6);
    memcpy(args, regs, sizeof(args));
    mg_atomic_add(&g_svc_calls, 1);
    if (on_block) mg_atomic_add(&g_svc_on_block, 1);
    result = mg_bionic_syscall_raw(regs);
#if MG_HOST_WINDOWS
    if (made && tp[1]) {
        mg_gthread *g = (mg_gthread *)tp[1];
        g->stack_lo = (void *)block[MG_SC_SAVED_DEALLOC];
        g->stack_hi = (void *)block[MG_SC_SAVED_BASE];
    }
#else
    if (on_block && !block[MG_SC_OWN_HI]) {
        void *lo = NULL, *hi = NULL;
        mg_thread_stack_bounds(&lo, &hi);
        block[MG_SC_OWN_LO] = (mg_addr)lo;
        block[MG_SC_OWN_HI] = (mg_addr)hi;
    }
#endif
    trace_syscall(regs[7], args, result, regs[15]);
    return result;
}
#endif

static void where_name(mg_addr a, char *out, size_t size);

#if MG_ARCH_ARM64 && MG_HOST_WINDOWS
/*
 * What Windows ARM64 is asked, for GitHub issue 4 (apps with ARM64 code ended in ntdll's RtlDispatchException as they
 * started, on a Snapdragon X Elite): two things said in the log, each once. Both cheap: they are looked at on every fault,
 * and at every stop of a thread.
 *
 * A fault of guest code -- a system call left a trap among them -- with less than 64 KB between its sp and the bottom of
 * its thread's stack, once a thread: the dispatch of an exception takes some 8 KB of the stack below sp before any handler
 * runs (mg_host.h, MG_STACK_FAULT_ROOM), and the process ended where that was not there.
 */
static void stack_room_said(const mg_fault *f, mg_addr pc)
{
    static volatile i32 said[64];
    void *lo = NULL, *hi = NULL;
    mg_addr sp = f->regs[MG_REG_SP];
    char where[160];
    u32 tid;
    int i;
    mg_thread_stack_bounds(&lo, &hi);
    if (sp < (mg_addr)lo || sp >= (mg_addr)hi || sp - (mg_addr)lo >= 64 * 1024) return;
    tid = mg_thread_self_id();
    for (i = 0; i < (int)MG_COUNT(said); i++) {
        i32 was = mg_atomic_cas(&said[i], 0, (i32)tid);
        if (was == (i32)tid) return;
        if (!was) break;
    }
    if (i == (int)MG_COUNT(said)) return;
    where_name(pc, where, sizeof(where));
    mg_warn("fault: thread %u faulted at %s with %u KB of its stack left below sp (%u KB in all)", (unsigned)tid, where,
            (unsigned)((sp - (mg_addr)lo) >> 10), (unsigned)(((mg_addr)hi - (mg_addr)lo) >> 10));
}

/*
 * A thread found, as the core stopped it (the collector's mg_thread_suspend, a signal's mg_thread_interrupt), with x18 other
 * than its TEB, once a pc (host/mg_host_win32.c, which says it once no thread is stopped): every function of Windows' reads
 * the TEB through x18, KiUserExceptionDispatcher does not put it back, and the thread's next exception ends the process in
 * RtlDispatchException, before any handler of the core's runs. The pc is the code that wrote it, or code after it.
 */
static MG_HOSTCB void x18_said(mg_addr pc, mg_addr x18, mg_addr teb, u32 thread, u32 stops)
{
    char where[160];
    where_name(pc, where, sizeof(where));
    mg_err("x18: thread %u was stopped at %s with x18 %016llx, not its TEB %016llx (%u such stops so far)", (unsigned)thread, where,
           (unsigned long long)x18, (unsigned long long)teb, (unsigned)stops);
}
#endif

static MG_HOSTCB bool on_fault(mg_fault *f)
{
    const char *lib = NULL, *sym;
    const void *sym_addr = NULL, *base = NULL;
    char report[4000];
    mg_addr pc = f->regs ? MG_CODE_ADDR(f->regs[MG_REG_PC]) : 0;
#if MG_ARCH_ARM64 && MG_HOST_WINDOWS
    if (f->regs && pc && mg_ld_is_guest_code(pc)) stack_room_said(f, pc);
#endif
#if MG_HOST_ARM32
    /* The kernel's user helpers, which Windows has no page of (arm/mg_kuser.h): a call of one, or a read of their version. */
    if (f->kind == MG_FAULT_ACCESS && f->regs && (f->address & 0xfffff000u) == 0xffff0000u) {
        mg_addr resume = 0;
        bool thumb = (f->regs[15] & 1u) || (f->cpsr && (*f->cpsr & 0x20u));
        bool done = f->exec ? mg_kuser_call(f->address, f->regs, f->cpsr, (u32)(mg_addr)mg_guest_tp_get(), &resume)
                            : !f->write && mg_kuser_read(f->address, (const u8 *)pc, thumb, f->regs, &resume);
        if (done) {
            static volatile i32 said;
            if (!said && !mg_atomic_xchg(&said, 1))
                mg_info("the kernel's user helpers at 0xffff0000 are called (old ARM code's atomics, barrier, TLS): each call is a fault, done in its place");
            mg_fault_resume_at(f, resume);
            return true;
        }
    }
#endif
    if ((f->kind == MG_FAULT_ILLEGAL || f->kind == MG_FAULT_BREAKPOINT) && f->regs) {
        mg_addr target = 0;
#if MG_ARCH_ARM32
        int reg;
        /* A patched svc's UDF, or an svc left as the file has it, stopped at its hardware breakpoint. */
        if (mg_patch_svc_target(pc, &target)) {
            u32 nr = f->regs[7], args[4];
            args[0] = f->regs[0];
            args[1] = f->regs[1];
            args[2] = f->regs[2];
            args[3] = f->regs[3];
            f->regs[0] = mg_bionic_syscall_raw(f->regs);
            trace_syscall(nr, args, f->regs[0], pc);
            mg_fault_resume_at(f, target);
            return true;
        }
        /* A thread-pointer read left as the file has it, stopped at its breakpoint: the guest's thread pointer. */
        if (f->kind == MG_FAULT_BREAKPOINT && mg_patch_tp_target(pc, &reg, &target)) {
            f->regs[reg] = (u32)(mg_addr)mg_guest_tp_get();
            mg_fault_resume_at(f, target);
            return true;
        }
#elif MG_ARCH_ARM64
        /*
         * A patched svc's UDF (arm64/mg_patch_arm64.c): one with no piece of island in reach, or a piece's own trap, where
         * a thread with no stack for system calls is sent; mg_arm64_syscall_call makes the call as this does. The call's
         * number in x8, its arguments in x0..x5, what it gives in x0.
         */
        if (mg_patch_svc_target(pc, &target)) {
            long args[6];
            int i;
            for (i = 0; i < 6; i++) args[i] = (long)f->regs[i];
            f->regs[0] = (mg_addr)mg_bionic_syscall((long)f->regs[8], args, pc);
            mg_fault_resume_at(f, target);
            return true;
        }
        /*
         * A read of CTR_EL0, the cache's type, or cache maintenance by address (DC CVAU, CVAC, CIVAC; IC IVAU), which Linux
         * lets user code run and Windows refuses in user mode: done here, and the thread on after it. The loader makes each
         * read of CTR_EL0 it finds a constant's (arm64/mg_patch_arm64.c); this answers code made at run time, and the
         * maintenance, should Windows refuse that too.
         */
        if (f->kind == MG_FAULT_ILLEGAL && mg_patch_sys_fault(pc, f->regs)) {
            mg_fault_resume_at(f, pc + 4);
            return true;
        }
#endif
        if (f->kind == MG_FAULT_BREAKPOINT) return false;
        if (mg_ld_plt_target(pc, &target) || mg_stubs_once_target(pc, &target)) {
            mg_fault_resume_at(f, target);
            mg_atomic_add(&g_plt_rescues, 1);
            return true;
        }
        /* A trap the app wrote into its code for its SIGILL handler: an inline hook's. */
        if (mg_mmap_app_trap(pc) && mg_signal_fault(4, f)) return true;
    }
#if MG_HOST_ARM32
    /* A device probe that went wrong on purpose (mg_diag.c). */
    if (mg_probe_fix_fault(f)) return true;
#endif
#if MG_ARCH_X86
    /*
     * x86's `int 0x80` in an app's own code (its patcher counts them and leaves them, x86/mg_patch_x86.c), which Windows
     * answers with an access violation at it: the system call made here with Linux's i386 number and arguments -- eax;
     * ebx, ecx, edx, esi, edi, ebp -- its result (-errno on failure) in eax, and the thread on after the two bytes, as the
     * kernel goes on. Asphalt 9's breakpad (libcrashlytics) asks so for its sigaltstack, and the game ended at its start.
     */
    if (f->kind == MG_FAULT_ACCESS && f->regs && pc && mg_ld_is_guest_code(pc) && ((const u8 *)pc)[0] == 0xCD && ((const u8 *)pc)[1] == 0x80) {
        static volatile i32 said;
        long args[6];
        u32 nr = (u32)f->regs[0];
        args[0] = (long)f->regs[3];
        args[1] = (long)f->regs[1];
        args[2] = (long)f->regs[2];
        args[3] = (long)f->regs[6];
        args[4] = (long)f->regs[7];
        args[5] = (long)f->regs[5];
        f->regs[0] = (mg_addr)mg_bionic_syscall((long)nr, args, pc);
        if (!mg_atomic_xchg(&said, 1)) mg_info("int 0x80 in an app's code: each one is a fault, made a system call in its place (the first, %u)", (unsigned)nr);
        mg_fault_resume_at(f, pc + 2);
        return true;
    }
#endif
    if (f->kind == MG_FAULT_ACCESS && mg_mmap_fix_fault(f->address, f->write, f->exec, pc)) {
        mg_atomic_add(&g_wx_flips, 1);
        return true;
    }
    /* An assertion of the system's own that is known to end the process for nothing (mg_host_win32.c). */
    if (mg_host_repair_known_fault(f)) return true;
    /* Not one of ours to repair. Say where it was, in the guest's terms, before the process goes. */
    sym = pc ? mg_ld_symbol_at((const void *)pc, &lib, &sym_addr, &base) : NULL;
    /* (A pc of zero is a fault like any other: a call through a null pointer, and lr says whose.) */
    if (f->regs) {
        char regs[900];
        registers_line(f, regs, sizeof(regs));
        snprintf(report, sizeof(report), "fault: %s (code 0x%x)%s%s at pc %0*llx%s%s%s%s, address %0*llx%s",
                 f->kind == MG_FAULT_ILLEGAL ? "illegal instruction" : f->kind == MG_FAULT_ACCESS ? "access violation" : fault_name(f->code), f->code,
                 f->write ? " writing" : "", f->exec ? " executing" : "", (int)(sizeof(mg_addr) * 2), (unsigned long long)pc, lib ? " in " : "",
                 lib ? lib : "", sym ? " " : "", sym ? sym : "", (int)(sizeof(mg_addr) * 2), (unsigned long long)f->address, regs);
        if (lib && base) {
            size_t n = strlen(report);
            snprintf(report + n, sizeof(report) - n, "\n  %s+0x%x", lib, (unsigned)(pc - (mg_addr)base));
        }
        if (f->kind == MG_FAULT_ACCESS && f->address < 0x100) {
            /*
             * A Gecko app's crash on purpose: MOZ_CRASH puts its reason in mozglue's gMozCrashReason and then writes to
             * address 0 (a Rust panic in Gecko ends so too, its message the reason).
             */
            const char *const *reason = (const char *const *)mg_ld_sym(NULL, "gMozCrashReason");
            if (reason && *reason) {
                size_t n = strlen(report);
                snprintf(report + n, sizeof(report) - n, "\n  MOZ_CRASH: %.300s", *reason);
            }
        }
        if (lib && mg_ld_lib_of((const void *)(pc - 18)) == mg_ld_lib_of((const void *)pc)
            && mg_ld_lib_of((const void *)(pc + 16)) == mg_ld_lib_of((const void *)pc)) {
            /* Chromium's end out of memory: how much it was asked for, which its library's global keeps (see above). */
            mg_addr global = mg_chromium_oom_global(pc);
            if (global && mg_ld_lib_of((const void *)global) == mg_ld_lib_of((const void *)pc)
                && mg_ld_lib_of((const void *)(global + 3)) == mg_ld_lib_of((const void *)pc)) {
                u32 asked;
                size_t n = strlen(report);
                memcpy(&asked, (const void *)global, 4);
                snprintf(report + n, sizeof(report) - n, "\n  Chromium ran out of memory asking for %u bytes (%u MB)", (unsigned)asked,
                         (unsigned)(asked >> 20));
            }
        }
        /*
         * A small pc, reached by a call when lr is where a call returns to: a call through a null pointer, or a little past
         * one, through a table of them. Not when lr is no return address -- in a library's data, after an instruction that
         * is no call, on the stack: the thread jumped or returned there, as a function returns whose frame's saved lr was
         * written over. A library that ends its process on purpose does that: DANA's libjabe.so (DexGuard) sums its own
         * code, and where a sum differs it puts 20 in its frame's saved lr and 24 in the saved r11 and returns (+0xeed18) --
         * and its sums differ here, the loader having made its system calls traps. The report said a call through a null
         * pointer (0.1.0.678).
         */
        int called = 1;
        if (f->kind == MG_FAULT_ACCESS && pc < 0x10000) {
            size_t n = strlen(report);
#if MG_ARCH_ARM32
            mg_addr lr_at = f->regs[MG_REG_LR];
            called = mg_ld_after_call(lr_at);
            if (called < 0) {
                void *lo = NULL, *hi = NULL;
                mg_thread_stack_bounds(&lo, &hi);
                if (lr_at >= (mg_addr)lo && lr_at < (mg_addr)hi) called = 0;
            }
#endif
            if (called) {
                snprintf(report + n, sizeof(report) - n, "\n  a call through a null pointer: the caller (%s) had asked for a function and was given none",
                         MG_REG_LR >= 0 ? "lr" : "the return address on the stack");
            } else {
                mg_lib *changed = mg_ld_lib_of((const void *)f->regs[MG_REG_LR >= 0 ? MG_REG_LR : 0]);
                mg_ld_stats st;
                snprintf(report + n, sizeof(report) - n, "\n  not a call: lr is no return address, so the thread jumped or returned to this pc, as a function "
                         "returns whose saved lr was written over -- a library that ends its process on purpose does that");
                if (changed) {
                    mg_ld_get_stats(changed, &st);
                    n = strlen(report);
                    if (st.svc_sites + st.tp_reads + st.plt_entries)
                        snprintf(report + n, sizeof(report) - n, "; the loader changed %s's code in %u places (%u system calls by number made traps, "
                                 "%u thread-pointer reads, %u PLT entries), which a library that sums its own code takes for tampering",
                                 mg_ld_lib_name(changed), (unsigned)(st.svc_sites + st.tp_reads + st.plt_entries), (unsigned)st.svc_sites,
                                 (unsigned)st.tp_reads, (unsigned)st.plt_entries);
                }
            }
        }
        {
            /*
             * Who called: a fault inside this layer (a shim handed a bad pointer) is the caller's doing, and the caller is what can be
             * looked up. In lr on ARM; on x86 the return address is in a register only at a call's first instruction, and at sp then.
             */
            const char *caller_lib = NULL;
            const void *caller_sym_addr = NULL, *caller_base = NULL;
            mg_addr lr = MG_REG_LR >= 0 ? MG_CODE_ADDR(f->regs[MG_REG_LR >= 0 ? MG_REG_LR : 0]) : (pc < 0x10000 ? *(const mg_addr *)f->regs[MG_REG_SP] : 0);
            const char *caller = mg_ld_symbol_at((const void *)lr, &caller_lib, &caller_sym_addr, &caller_base);
            if (caller_lib && caller_base) {
                size_t n = strlen(report);
                snprintf(report + n, sizeof(report) - n, "\n  %s %s+0x%x%s%s", called ? "called from" : "lr:", caller_lib, (unsigned)(lr - (mg_addr)caller_base),
                         caller ? " " : "", caller ? caller : "");
            }
        }
        if (!lib) {
            /* Not Android code: a module of the app's own or the system's, by name. */
            char module[96];
            mg_addr module_base = 0;
            size_t n = strlen(report);
            if (mg_host_module_at(pc, module, sizeof(module), &module_base))
                snprintf(report + n, sizeof(report) - n, "\n  in %s+0x%x", module, (unsigned)(pc - module_base));
            n = strlen(report);
            if (MG_REG_LR >= 0 && mg_host_module_at(MG_CODE_ADDR(f->regs[MG_REG_LR >= 0 ? MG_REG_LR : 0]), module, sizeof(module), &module_base))
                snprintf(report + n, sizeof(report) - n, ", called from %s+0x%x", module,
                         (unsigned)(MG_CODE_ADDR(f->regs[MG_REG_LR >= 0 ? MG_REG_LR : 0]) - module_base));
        }
        {
            /*
             * What the stack holds that looks like a way back into Android
             * code or into a module: not a real unwinding (there are no
             * tables for most of it), but it usually names who asked.
             */
            void *lo = NULL, *hi = NULL;
            mg_addr sp = f->regs[MG_REG_SP] & ~(mg_addr)(sizeof(mg_addr) - 1), last = 0;
            int found = 0;
            mg_thread_stack_bounds(&lo, &hi);
            if (sp >= (mg_addr)lo && sp < (mg_addr)hi) {
                size_t n = strlen(report);
                mg_addr at;
                for (at = sp; at + sizeof(mg_addr) <= (mg_addr)hi && at < sp + 65536 && found < 40 && n + 80 < sizeof(report); at += sizeof(mg_addr)) {
                    mg_addr word = MG_CODE_ADDR(*(const mg_addr *)at), where_base = 0;
                    const char *where_lib = NULL;
                    const void *where_sym = NULL, *where_lib_base = NULL;
                    char module[96];
                    if (word < 0x10000 || word == last) continue;
                    if (mg_ld_is_guest_code(word)) {
                        mg_ld_symbol_at((const void *)word, &where_lib, &where_sym, &where_lib_base);
                        if (!where_lib || !where_lib_base) continue;
                        snprintf(report + n, sizeof(report) - n, "%s%s+0x%x", found ? ", " : "\n  on the stack: ", where_lib,
                                 (unsigned)(word - (mg_addr)where_lib_base));
                    } else if (mg_host_module_at(word, module, sizeof(module), &where_base) && word != where_base) {
                        snprintf(report + n, sizeof(report) - n, "%s%s+0x%x", found ? ", " : "\n  on the stack: ", module, (unsigned)(word - where_base));
                    } else {
                        continue;
                    }
                    n = strlen(report);
                    last = word;
                    found++;
                }
            }
        }
        {
            size_t n = strlen(report);
            snprintf(report + n, sizeof(report) - n, "\n  thread %u%s", (unsigned)mg_thread_self_id(),
                     mg_guest_tp_get() ? ", which has run Android code" : ", which has never run Android code");
            n = strlen(report);
            snprintf(report + n, sizeof(report) - n, "; %u MB in use of %u", (unsigned)(mg_mem_usage() >> 20), (unsigned)(mg_mem_limit() >> 20));
            if (g_fault_detail && mg_guest_tp_get()) {
                n = strlen(report);
                g_fault_detail(report + n, sizeof(report) - n);
            }
            if (mg_egl_device_removed()) {
                n = strlen(report);
                snprintf(report + n, sizeof(report) - n, "\n  the Direct3D device had been removed: 0x%x", (unsigned)mg_egl_device_removed());
            }
            {
                u32 free_mb = 0, largest = 0, reserved = 0, committed = 0;
                if (mg_host_address_space(&free_mb, &largest, &reserved, &committed)) {
                    n = strlen(report);
                    u32 stretch[4];
                    snprintf(report + n, sizeof(report) - n,
                             "\n  address space: %u MB free (the largest stretch %u MB), %u MB reserved, %u MB committed; %u MB of it views of shared memory and files",
                             (unsigned)free_mb, (unsigned)largest, (unsigned)reserved, (unsigned)committed, (unsigned)(mg_views_kb() >> 10));
                    if (mg_host_free_stretches(stretch, 4)) {
                        n = strlen(report);
                        snprintf(report + n, sizeof(report) - n, "\n  the largest free stretches: %u, %u, %u and %u MB", (unsigned)stretch[0],
                                 (unsigned)stretch[1], (unsigned)stretch[2], (unsigned)stretch[3]);
                    }
                }
            }
        }
        mg_err("%s", report);
        {
            char textures[320];
            mg_gltex_report(textures, sizeof(textures));
            mg_err("  %s", textures);
        }
        mg_trace_dump(MG_LOG_ERROR, 500);
        {
            /*
             * Guest code run under a guard (mg_jni_call_native_guarded: a native method of code Android runs in a process
             * of its own, not the app's -- Google Play services' DroidGuard): on Android that process ends, and the app
             * that asked it is told so. Here the thread goes back to the call, which ends with an exception, and the app
             * runs on. Not when the fault is in the layer's own code (a module of the host's), which may hold its locks.
             */
            mg_gthread *gt = mg_guest_tp_get() ? mg_gthread_self() : NULL;
            char module[96];
            mg_addr module_base = 0;
#if MG_ARCH_ARM32
            void *longjmp_stub = mg_stub(MG_STUB_LONGJMP);
#elif MG_GUEST_CODE
            void *longjmp_stub = (void *)mg_longjmp;
#else
            void *longjmp_stub = NULL;
#endif
            if (gt && gt->fault_guard && longjmp_stub && !mg_host_module_at(pc, module, sizeof(module), &module_base)) {
                mg_addr *jb = gt->fault_guard;
                gt->fault_guard = NULL;
                mg_warn("fault: contained: the thread goes back to the native method's call, which ends with an exception; the app runs on");
                /* longjmp(jb, 1), as the processor's convention passes them: in registers, or on x86 on the stack. */
#if MG_ARCH_X86_64
                f->regs[7] = (mg_addr)jb;                /* rdi */
                f->regs[6] = 1;                          /* rsi */
                f->regs[MG_REG_SP] = (f->regs[MG_REG_SP] - 256) & ~(mg_addr)15;
                f->regs[MG_REG_SP] -= 8;                 /* as if called */
#elif MG_ARCH_X86
                {
                    mg_addr *at = (mg_addr *)((f->regs[MG_REG_SP] - 256) & ~(mg_addr)15);
                    at[-3] = 0;                          /* the return address it never uses */
                    at[-2] = (mg_addr)jb;
                    at[-1] = 1;
                    f->regs[MG_REG_SP] = (mg_addr)(at - 3);
                }
#else
                f->regs[0] = (mg_addr)jb;
                f->regs[1] = 1;
#endif
                mg_fault_resume_at(f, (mg_addr)longjmp_stub);
                return true;
            }
        }
        if (g_crash_hook) {
            g_crash_hook(report);
            /*
             * An app is running and one of its threads cannot go on. Letting
             * the exception through would end the process, and the report
             * with it; the thread is parked instead, for good. The shell
             * shows what happened, and the app's other threads stay as they
             * are until Megler is closed (nothing else can run after an app
             * anyway).
             */
            if (mg_guest_tp_get()) {
                for (;;) mg_sleep_ns(1000000000ull);
            }
        }
    }
    return false;
}

/* An address as the reports say it: <library>+0x.. of Android code, <module>+0x.. of the host's, else the address. */
static void where_name(mg_addr a, char *out, size_t size)
{
    const char *lib = NULL;
    const void *sym = NULL, *lib_base = NULL;
    char module[96];
    mg_addr base = 0;
    a &= ~(mg_addr)1;
    if (mg_ld_is_guest_code(a)) mg_ld_symbol_at((const void *)a, &lib, &sym, &lib_base);
    if (lib && lib_base)
        snprintf(out, size, "%s+0x%x", lib, (unsigned)(a - (mg_addr)lib_base));
    else if (mg_host_module_at(a, module, sizeof(module), &base) && a != base)
        snprintf(out, size, "%s+0x%x", module, (unsigned)(a - base));
    else
        snprintf(out, size, "%0*llx", (int)(sizeof(mg_addr) * 2), (unsigned long long)a);
}

bool megler_dump_module(const char *module, const char *path)
{
    mg_addr base = 0;
    size_t size = 0;
    bool ok = module && path && mg_host_module_dump(module, path, &base, &size);
    if (ok) mg_info("%s at %08x, %u bytes of image, is in %s", module, (unsigned)base, (unsigned)size, path);
    else mg_warn("%s: not dumped (no such module in this process, or %s could not be written)", module ? module : "?", path ? path : "?");
    return ok;
}

bool megler_probe_breakpoints(char *out, size_t size)
{
    bool ok = mg_host_probe_breakpoints(out, size);
    mg_info("breakpoints: %s", out);
    return ok;
}

bool megler_thread_where(unsigned thread_id, char *out, size_t size)
{
    enum { WORDS = 65536 / sizeof(mg_addr) };     /* 64 KB of its stack from sp up, as a fault's report looks */
    static mg_addr words[WORDS];
    static mg_lock lock = MG_LOCK_INIT;
    mg_thread_regs regs;
    char name[128];
    size_t n;
    int got, i, found = 0;
    mg_addr last = 0;
    if (!out || !size) return false;
    out[0] = 0;
    mg_lock_acquire(&lock);
    got = mg_thread_snapshot_id(thread_id, &regs, words, WORDS);
    if (got < 0) {
        mg_lock_release(&lock);
        return false;
    }
    where_name(regs.pc, name, sizeof(name));
    snprintf(out, size, "pc %s", name);
#if MG_HOST_ARM32
    where_name(regs.r[13], name, sizeof(name));
    n = strlen(out);
    snprintf(out + n, size - n, ", lr %s", name);
#endif
    for (i = 0; i < got && found < 40; i++) {
        mg_addr word = MG_CODE_ADDR(words[i]), base = 0;
        char module[96];
        const char *lib = NULL;
        const void *sym = NULL, *lib_base = NULL;
        if (word < 0x10000 || word == last) continue;
        if (mg_ld_is_guest_code(word)) {
            mg_ld_symbol_at((const void *)word, &lib, &sym, &lib_base);
            if (!lib || !lib_base) continue;
        } else if (!mg_host_module_at(word, module, sizeof(module), &base) || word == base) {
            continue;
        }
        where_name(word, name, sizeof(name));
        n = strlen(out);
        if (n + strlen(name) + 20 >= size) break;
        snprintf(out + n, size - n, "%s%s", found ? ", " : "; on the stack: ", name);
        last = word;
        found++;
    }
    mg_lock_release(&lock);
    return true;
}

bool mg_core_init(void)
{
    if (g_inited) return g_can_run;
    g_inited = true;
    mg_bionic_init();
    mg_ctype_init();
    mg_time_init();
    mg_misc_init();
    mg_env_init();
#if MG_ARCH_ARM32
    g_can_run = mg_stubs_init();
#elif MG_GUEST_CODE
    {
        /* Where the core is of the guest's own convention (docs/DESKTOP.md): whether memory can be made executable at all. */
        u8 *page = (u8 *)mg_mem_alloc(MG_PAGE);
        g_can_run = page && mg_mem_protect(page, MG_PAGE, MG_PROT_RX);
        if (page) mg_mem_release(page, MG_PAGE);
        if (!g_can_run) mg_err("core: executable memory was refused -- is the codeGeneration capability granted?");
        mg_bionic_register(g_own_code);
    }
#endif
    mg_fault_set_handler(on_fault);
#if MG_ARCH_ARM64 && MG_HOST_WINDOWS
    mg_thread_set_x18_report(x18_said);
#endif
#if MG_GUEST_CODE
    {
        /* Asked for now, while few thread-local slots are taken: on Windows the slot has to be one of the first 64, which live in the TEB itself. */
        u32 opc2 = 0, offset = 0;
        if (!mg_guest_tp_where(&opc2, &offset)) mg_err("core: the guest thread pointer has no place on this host; libraries that read it will not work");
    }
#endif
    mg_info("megler core on %s%s", mg_host_name(), g_can_run ? "" : " (guest code cannot run here)");
    return g_can_run;
}

const char *megler_data_dir(void);

static int g_arm_state;
static bool g_arm_state_read;

static char *arm_state_path(void)
{
    const char *dir = megler_data_dir();
    return dir && dir[0] ? mg_sprintf("%s/arm-state.txt", dir) : NULL;
}

int mg_core_arm_state(void)
{
    if (!g_arm_state_read) {
        char *path = arm_state_path();
        u8 *text = path ? mg_file_read_all(path, NULL) : NULL;
        if (text) g_arm_state = !strncmp((const char *)text, "survives", 8) ? 1 : !strncmp((const char *)text, "lost", 4) ? -1 : 0;
        free(text);
        free(path);
        g_arm_state_read = true;
    }
    return g_arm_state;
}

void mg_core_set_arm_state(int state)
{
    char *path = arm_state_path();
    const char *text = state > 0 ? "survives\n" : state < 0 ? "lost\n" : "";
    g_arm_state = state;
    g_arm_state_read = true;
    if (path) mg_file_write_all(path, text, strlen(text));
    free(path);
}

bool mg_core_can_run(void) { return g_can_run; }
u32 mg_core_plt_rescues(void) { return (u32)g_plt_rescues; }
u32 mg_core_wx_flips(void) { return (u32)g_wx_flips; }

void mg_core_svc_calls(u32 *made, u32 *on_block)
{
    *made = (u32)g_svc_calls;
    *on_block = (u32)g_svc_on_block;
}

void mg_core_count_svc(bool on_block)
{
    mg_atomic_add(&g_svc_calls, 1);
    if (on_block) mg_atomic_add(&g_svc_on_block, 1);
}

void mg_core_set_crash_hook(void (*hook)(const char *report)) { g_crash_hook = hook; }
void mg_core_set_fault_detail(void (*detail)(char *out, size_t cap)) { g_fault_detail = detail; }
void mg_core_thread_detail(char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (g_fault_detail) g_fault_detail(out, cap);
}
