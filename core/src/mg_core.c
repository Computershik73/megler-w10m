/* Start-up of the core, and the one fault handler everything else hangs off. */
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
bool mg_mmap_fix_fault(mg_addr address, bool write, bool exec);
bool mg_mmap_app_trap(mg_addr pc);
#if MG_HOST_ARM32
bool mg_probe_fix_fault(mg_fault *f);
#endif

static volatile i32 g_plt_rescues, g_wx_flips;
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

static MG_HOSTCB bool on_fault(mg_fault *f)
{
    const char *lib = NULL, *sym;
    const void *sym_addr = NULL, *base = NULL;
    char report[4000];
    mg_addr pc = f->regs ? MG_CODE_ADDR(f->regs[MG_REG_PC]) : 0;
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
            /* The flag trace-syscalls: each one said, with its number, its first four arguments, what it gives and whose. */
            static volatile i32 traced = -1, lines;
            u32 nr = f->regs[7], a0 = f->regs[0], a1 = f->regs[1], a2 = f->regs[2], a3 = f->regs[3];
            f->regs[0] = mg_bionic_syscall_raw(f->regs);
            if (traced < 0 && megler_data_dir()[0]) traced = mg_flag("trace-syscalls") ? 1 : 0;
            if (traced > 0 && mg_atomic_add(&lines, 1) <= 30000) {
                const char *where = NULL;
                const void *s = NULL, *b = NULL;
                mg_ld_symbol_at((const void *)pc, &where, &s, &b);
                mg_info("syscall %u(0x%x, 0x%x, 0x%x, 0x%x) = %d on thread %u at %s+0x%x", (unsigned)nr, (unsigned)a0, (unsigned)a1,
                        (unsigned)a2, (unsigned)a3, (int)f->regs[0], (unsigned)mg_thread_self_id(), where ? where : "?",
                        (unsigned)(pc - (mg_addr)b));
            }
            mg_fault_resume_at(f, target);
            return true;
        }
        /* A thread-pointer read left as the file has it, stopped at its breakpoint: the guest's thread pointer. */
        if (f->kind == MG_FAULT_BREAKPOINT && mg_patch_tp_target(pc, &reg, &target)) {
            f->regs[reg] = (u32)(mg_addr)mg_guest_tp_get();
            mg_fault_resume_at(f, target);
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
    if (f->kind == MG_FAULT_ACCESS && mg_mmap_fix_fault(f->address, f->write, f->exec)) {
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
        if (f->kind == MG_FAULT_ACCESS && pc < 0x10000) {
            size_t n = strlen(report);
            snprintf(report + n, sizeof(report) - n, "\n  a call through a null pointer: the caller (%s) had asked for a function and was given none",
                     MG_REG_LR >= 0 ? "lr" : "the return address on the stack");
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
                snprintf(report + n, sizeof(report) - n, "\n  called from %s+0x%x%s%s", caller_lib, (unsigned)(lr - (mg_addr)caller_base), caller ? " " : "",
                         caller ? caller : "");
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
        snprintf(out, size, "%08x", (unsigned)a);
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
    }
#endif
    mg_fault_set_handler(on_fault);
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
void mg_core_set_crash_hook(void (*hook)(const char *report)) { g_crash_hook = hook; }
void mg_core_set_fault_detail(void (*detail)(char *out, size_t cap)) { g_fault_detail = detail; }
void mg_core_thread_detail(char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (g_fault_detail) g_fault_detail(out, cap);
}
