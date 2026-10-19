/* Start-up of the core, and the one fault handler everything else hangs off. */
#include "hk_core.h"
#include "ndk/hk_egl.h"

#include "arm/hk_stubs.h"
#include "bionic/hk_bionic.h"
#include "host/hk_host.h"
#include "ld/hk_ld.h"

void hk_ctype_init(void);
void hk_time_init(void);
void hk_misc_init(void);
void hk_env_init(void);
bool hk_mmap_fix_fault(hk_addr address, bool write, bool exec);
#if HK_HOST_ARM32
bool hk_probe_fix_fault(hk_fault *f);
#endif

static volatile i32 g_plt_rescues, g_wx_flips;
static bool g_inited, g_can_run;
static void (*g_crash_hook)(const char *report);

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
hk_addr hk_chromium_oom_global(hk_addr pc)
{
    const u16 *h = (const u16 *)pc;
    hk_addr f, literal;
    u32 offset;
    if (pc < 18 || (pc & 1)) return 0;
    if (h[0] == 0xde00 && h[-1] == 0xbe00) f = pc - 18;
    else if (h[0] == 0xbe00 && h[1] == 0xde00) f = pc - 16;
    else return 0;
    h = (const u16 *)f;
    if (h[0] != 0xb082 || (h[1] & 0xff00) != 0x4900 || h[2] != 0x9001 || h[3] != 0x4479 || h[4] != 0x6008 || h[5] != 0xa801
        || (h[6] & 0xf800) != 0xf000 || (h[7] & 0xd000) != 0xd000)
        return 0;
    literal = ((f + 2 + 4) & ~(hk_addr)3) + (hk_addr)(h[1] & 0xff) * 4;
    /* (It follows the function's end, as the compiler puts it: no further than f + 32, which the caller can check.) */
    if (literal + 4 > f + 32) return 0;
    memcpy(&offset, (const void *)literal, 4);
    /* (A 32-bit offset: added as a signed one, it lands where the Thumb add lands on any width of address.) */
    return f + 6 + 4 + (hk_addr)(intptr_t)(i32)offset;
}

static bool on_fault(hk_fault *f)
{
    const char *lib = NULL, *sym;
    const void *sym_addr = NULL, *base = NULL;
    char report[4000];
    hk_addr pc = f->regs ? (f->regs[15] & ~1u) : 0;
    if (f->kind == HK_FAULT_ILLEGAL && f->regs) {
        hk_addr target = 0;
        if (hk_patch_svc_target(pc, &target)) {
            f->regs[0] = hk_bionic_syscall_raw(f->regs);
            hk_fault_resume_at(f, target);
            return true;
        }
        if (hk_ld_plt_target(pc, &target) || hk_stubs_once_target(pc, &target)) {
            hk_fault_resume_at(f, target);
            hk_atomic_add(&g_plt_rescues, 1);
            return true;
        }
    }
#if HK_HOST_ARM32
    /* A device probe that went wrong on purpose (hk_diag.c). */
    if (hk_probe_fix_fault(f)) return true;
#endif
    if (f->kind == HK_FAULT_ACCESS && hk_mmap_fix_fault(f->address, f->write, f->exec)) {
        hk_atomic_add(&g_wx_flips, 1);
        return true;
    }
    /* Not one of ours to repair. Say where it was, in the guest's terms, before the process goes. */
    sym = pc ? hk_ld_symbol_at((const void *)pc, &lib, &sym_addr, &base) : NULL;
    /* (A pc of zero is a fault like any other: a call through a null pointer, and lr says whose.) */
    if (f->regs) {
        snprintf(report, sizeof(report),
                 "fault: %s (code 0x%x)%s%s at pc %08x%s%s%s%s, address %08x\n"
                 "  r0 %08x r1 %08x r2 %08x r3 %08x r4 %08x r5 %08x r6 %08x r7 %08x\n"
                 "  r8 %08x r9 %08x r10 %08x r11 %08x r12 %08x sp %08x lr %08x cpsr %08x",
                 f->kind == HK_FAULT_ILLEGAL ? "illegal instruction" : f->kind == HK_FAULT_ACCESS ? "access violation" : fault_name(f->code), f->code,
                 f->write ? " writing" : "", f->exec ? " executing" : "", (unsigned)pc, lib ? " in " : "", lib ? lib : "", sym ? " " : "",
                 sym ? sym : "", (unsigned)f->address, f->regs[0], f->regs[1], f->regs[2], f->regs[3], f->regs[4], f->regs[5], f->regs[6],
                 f->regs[7], f->regs[8], f->regs[9], f->regs[10], f->regs[11], f->regs[12], f->regs[13], f->regs[14], f->cpsr ? *f->cpsr : 0);
        if (lib && base) {
            size_t n = strlen(report);
            snprintf(report + n, sizeof(report) - n, "\n  %s+0x%x", lib, (unsigned)(pc - (hk_addr)base));
        }
        if (f->kind == HK_FAULT_ACCESS && f->address < 0x100) {
            /*
             * A Gecko app's crash on purpose: MOZ_CRASH puts its reason in mozglue's gMozCrashReason and then writes to
             * address 0 (a Rust panic in Gecko ends so too, its message the reason).
             */
            const char *const *reason = (const char *const *)hk_ld_sym(NULL, "gMozCrashReason");
            if (reason && *reason) {
                size_t n = strlen(report);
                snprintf(report + n, sizeof(report) - n, "\n  MOZ_CRASH: %.300s", *reason);
            }
        }
        if (lib && hk_ld_lib_of((const void *)(pc - 18)) == hk_ld_lib_of((const void *)pc)
            && hk_ld_lib_of((const void *)(pc + 16)) == hk_ld_lib_of((const void *)pc)) {
            /* Chromium's end out of memory: how much it was asked for, which its library's global keeps (see above). */
            hk_addr global = hk_chromium_oom_global(pc);
            if (global && hk_ld_lib_of((const void *)global) == hk_ld_lib_of((const void *)pc)
                && hk_ld_lib_of((const void *)(global + 3)) == hk_ld_lib_of((const void *)pc)) {
                u32 asked;
                size_t n = strlen(report);
                memcpy(&asked, (const void *)global, 4);
                snprintf(report + n, sizeof(report) - n, "\n  Chromium ran out of memory asking for %u bytes (%u MB)", (unsigned)asked,
                         (unsigned)(asked >> 20));
            }
        }
        if (f->kind == HK_FAULT_ACCESS && pc < 0x10000) {
            size_t n = strlen(report);
            snprintf(report + n, sizeof(report) - n, "\n  a call through a null pointer: the caller (lr) had asked for a function and was given none");
        }
        {
            /* Who called: a fault inside this layer (a shim handed a bad pointer) is the caller's doing, and the caller is what can be looked up. */
            const char *caller_lib = NULL;
            const void *caller_sym_addr = NULL, *caller_base = NULL;
            hk_addr lr = f->regs[14] & ~(hk_addr)1;
            const char *caller = hk_ld_symbol_at((const void *)lr, &caller_lib, &caller_sym_addr, &caller_base);
            if (caller_lib && caller_base) {
                size_t n = strlen(report);
                snprintf(report + n, sizeof(report) - n, "\n  called from %s+0x%x%s%s", caller_lib, (unsigned)(lr - (hk_addr)caller_base), caller ? " " : "",
                         caller ? caller : "");
            }
        }
        if (!lib) {
            /* Not Android code: a module of the app's own or the system's, by name. */
            char module[96];
            hk_addr module_base = 0;
            size_t n = strlen(report);
            if (hk_host_module_at(pc, module, sizeof(module), &module_base))
                snprintf(report + n, sizeof(report) - n, "\n  in %s+0x%x", module, (unsigned)(pc - module_base));
            n = strlen(report);
            if (hk_host_module_at(f->regs[14] & ~(hk_addr)1, module, sizeof(module), &module_base))
                snprintf(report + n, sizeof(report) - n, ", called from %s+0x%x", module, (unsigned)((f->regs[14] & ~(hk_addr)1) - module_base));
        }
        {
            /*
             * What the stack holds that looks like a way back into Android
             * code or into a module: not a real unwinding (there are no
             * tables for most of it), but it usually names who asked.
             */
            void *lo = NULL, *hi = NULL;
            hk_addr sp = f->regs[13] & ~(hk_addr)3, last = 0;
            int found = 0;
            hk_thread_stack_bounds(&lo, &hi);
            if (sp >= (hk_addr)lo && sp < (hk_addr)hi) {
                size_t n = strlen(report);
                hk_addr at;
                for (at = sp; at + 4 <= (hk_addr)hi && at < sp + 65536 && found < 40 && n + 80 < sizeof(report); at += 4) {
                    hk_addr word = *(const u32 *)at & ~(hk_addr)1, where_base = 0;
                    const char *where_lib = NULL;
                    const void *where_sym = NULL, *where_lib_base = NULL;
                    char module[96];
                    if (word < 0x10000 || word == last) continue;
                    if (hk_ld_is_guest_code(word)) {
                        hk_ld_symbol_at((const void *)word, &where_lib, &where_sym, &where_lib_base);
                        if (!where_lib || !where_lib_base) continue;
                        snprintf(report + n, sizeof(report) - n, "%s%s+0x%x", found ? ", " : "\n  on the stack: ", where_lib,
                                 (unsigned)(word - (hk_addr)where_lib_base));
                    } else if (hk_host_module_at(word, module, sizeof(module), &where_base) && word != where_base) {
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
            snprintf(report + n, sizeof(report) - n, "\n  thread %u%s", (unsigned)hk_thread_self_id(),
                     hk_guest_tp_get() ? ", which has run Android code" : ", which has never run Android code");
            n = strlen(report);
            snprintf(report + n, sizeof(report) - n, "; %u MB in use of %u", (unsigned)(hk_mem_usage() >> 20), (unsigned)(hk_mem_limit() >> 20));
            if (hk_egl_device_removed()) {
                n = strlen(report);
                snprintf(report + n, sizeof(report) - n, "\n  the Direct3D device had been removed: 0x%x", (unsigned)hk_egl_device_removed());
            }
            {
                u32 free_mb = 0, largest = 0, reserved = 0, committed = 0;
                if (hk_host_address_space(&free_mb, &largest, &reserved, &committed)) {
                    n = strlen(report);
                    u32 stretch[4];
                    snprintf(report + n, sizeof(report) - n,
                             "\n  address space: %u MB free (the largest stretch %u MB), %u MB reserved, %u MB committed; %u MB of it views of shared memory and files",
                             (unsigned)free_mb, (unsigned)largest, (unsigned)reserved, (unsigned)committed, (unsigned)(hk_views_kb() >> 10));
                    if (hk_host_free_stretches(stretch, 4)) {
                        n = strlen(report);
                        snprintf(report + n, sizeof(report) - n, "\n  the largest free stretches: %u, %u, %u and %u MB", (unsigned)stretch[0],
                                 (unsigned)stretch[1], (unsigned)stretch[2], (unsigned)stretch[3]);
                    }
                }
            }
        }
        hk_err("%s", report);
        {
            char textures[320];
            hk_gltex_report(textures, sizeof(textures));
            hk_err("  %s", textures);
        }
        hk_trace_dump(HK_LOG_ERROR, 500);
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
            if (hk_guest_tp_get()) {
                for (;;) hk_sleep_ns(1000000000ull);
            }
        }
    }
    return false;
}

bool hk_core_init(void)
{
    if (g_inited) return g_can_run;
    g_inited = true;
    hk_bionic_init();
    hk_ctype_init();
    hk_time_init();
    hk_misc_init();
    hk_env_init();
    g_can_run = hk_stubs_init();
    hk_fault_set_handler(on_fault);
#if HK_HOST_ARM32
    {
        /* Asked for now, while few thread-local slots are taken: on Windows the slot has to be one of the first 64, which live in the TEB itself. */
        u32 opc2 = 0, offset = 0;
        if (!hk_guest_tp_where(&opc2, &offset)) hk_err("core: the guest thread pointer has no place on this host; libraries that read it will not work");
    }
#endif
    hk_info("megler core on %s%s", hk_host_name(), g_can_run ? "" : " (guest code cannot run here)");
    return g_can_run;
}

const char *megler_data_dir(void);

static int g_arm_state;
static bool g_arm_state_read;

static char *arm_state_path(void)
{
    const char *dir = megler_data_dir();
    return dir && dir[0] ? hk_sprintf("%s/arm-state.txt", dir) : NULL;
}

int hk_core_arm_state(void)
{
    if (!g_arm_state_read) {
        char *path = arm_state_path();
        u8 *text = path ? hk_file_read_all(path, NULL) : NULL;
        if (text) g_arm_state = !strncmp((const char *)text, "survives", 8) ? 1 : !strncmp((const char *)text, "lost", 4) ? -1 : 0;
        free(text);
        free(path);
        g_arm_state_read = true;
    }
    return g_arm_state;
}

void hk_core_set_arm_state(int state)
{
    char *path = arm_state_path();
    const char *text = state > 0 ? "survives\n" : state < 0 ? "lost\n" : "";
    g_arm_state = state;
    g_arm_state_read = true;
    if (path) hk_file_write_all(path, text, strlen(text));
    free(path);
}

bool hk_core_can_run(void) { return g_can_run; }
u32 hk_core_plt_rescues(void) { return (u32)g_plt_rescues; }
u32 hk_core_wx_flips(void) { return (u32)g_wx_flips; }
void hk_core_set_crash_hook(void (*hook)(const char *report)) { g_crash_hook = hook; }
