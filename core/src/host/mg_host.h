/*
 * Everything the core asks of the operating system, in one place.
 *
 * Two implementations: mg_host_win32.c (the Windows 10 Mobile app container, and
 * desktop Windows for the x64 build of the tests) and mg_host_posix.c (Linux,
 * where the ARM32 build runs under qemu-arm and the tests execute guest code).
 * Nothing else in the core includes an OS header.
 *
 * Paths are UTF-8 with forward slashes.
 */
#ifndef MG_HOST_H
#define MG_HOST_H

#include "../mg_base.h"

#if !MG_HOST_WINDOWS
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * x86 (desktop Windows): this header's structures are laid out alike by the core's ELF, which aligns a 64-bit field at
 * 4 bytes as Android's x86 ABI does, and by Megler.exe's half, which MSVC's x86 aligns at 8 (mg_stat's size was read
 * from its padding). Both sides take 4.
 */
#if MG_HOST_WINDOWS && MG_ARCH_X86
#pragma pack(push, 4)
#endif

/* ---------------------------------------------------------------- memory
 *
 * Windows 10 Mobile lets a package with the codeGeneration capability turn
 * committed read-write pages into execute-read ones and back
 * (VirtualProtectFromApp). The FromApp functions never allow an executable
 * commit or a page that is writable and executable at once. So everything here
 * is written with a page writable and run with it executable: a library's text
 * is copied in, patched, and then flipped, once. The plain VirtualProtect, in
 * the phone's kernelbase like the others, does allow the latter where the
 * phone does (mg_mem_rwx): MG_PROT_RWX.
 */
enum { MG_PROT_NONE = 0, MG_PROT_R = 1, MG_PROT_RW = 3, MG_PROT_RX = 5, MG_PROT_RWX = 7 };

/*
 * Whether pages may be writable and executable at once (MG_PROT_RWX for mg_mem_protect): found out once, by making a page
 * so, running code written to it, writing it again and running it again (Windows); always (POSIX).
 */
MG_HOSTAPI bool mg_mem_rwx(void);

#define MG_PAGE 4096u

/* Address space only, a multiple of 64 KB (Windows' allocation granularity). NULL on failure. */
MG_HOSTAPI void *mg_mem_reserve(size_t size);
/* The same at exactly `p` (64 KB aligned), or NULL when any of it is taken. */
MG_HOSTAPI void *mg_mem_reserve_at(void *p, size_t size);
/* Back reserved pages with zeroed memory: MG_PROT_R or MG_PROT_RW. */
MG_HOSTAPI bool mg_mem_commit(void *p, size_t size, int prot);
MG_HOSTAPI bool mg_mem_protect(void *p, size_t size, int prot);
MG_HOSTAPI bool mg_mem_decommit(void *p, size_t size);
/* mg_mem_protect for the committed pages of [p, p + size) alone, the others left reserved (Windows); mg_mem_protect elsewhere. */
MG_HOSTAPI bool mg_mem_protect_committed(void *p, size_t size, int prot);
/* How many bytes of [p, p + size) are the process's own memory committed (not views of files or sections): Windows says; -1 where the host does not (POSIX, which commits as pages are touched). */
MG_HOSTAPI i64 mg_mem_committed(const void *p, size_t size);
/*
 * What an allocator done with memory gives back (madvise's MADV_DONTNEED): the committed pages of [p, p + size) that are
 * readable and writable are decommitted, each run told to `fn` first; pages of any other access are left. The bytes given
 * back. Windows says each page's access; POSIX does not, and takes all of it for readable and writable, as an allocator's
 * memory is when it gives it back.
 */
MG_HOSTAPI size_t mg_mem_discard(void *p, size_t size, void (MG_HOSTCB *fn)(u8 *at, size_t n, void *user), void *user);
/*
 * Called with memory the system itself is about to write into (a file's or a socket's bytes read into a buffer the app
 * gave): the layer above commits pages of it that it gave back (bionic/mg_libc_mem.c, madvise), which the system would
 * refuse to write into -- the call failing -- where the app's own code faults and has them committed again.
 */
MG_HOSTAPI void mg_mem_set_before_system_write(void (MG_HOSTCB *fn)(void *p, size_t size));
/* Whether the page at `p` is committed, and its access (MG_PROT_*, -1 for any other): false where the host cannot say (POSIX). */
MG_HOSTAPI bool mg_mem_page_state(const void *p, bool *committed, int *prot);
MG_HOSTAPI void mg_mem_release(void *p, size_t size);
/* After writing code and before running it. */
MG_HOSTAPI void mg_icache_flush(void *p, size_t size);
/* Reserve and commit read-write in one step (anonymous mmap). */
MG_HOSTAPI void *mg_mem_alloc(size_t size);
/*
 * The same, with the host noting which of its pages are written (Windows' write watch, MEM_WRITE_WATCH): NULL where it
 * cannot, and mg_mem_alloc's memory is had instead. Released with mg_mem_release.
 */
MG_HOSTAPI void *mg_mem_alloc_watched(size_t size);
/* Why the host's last call on this thread failed, as the host numbers it (GetLastError, errno): for the log. */
MG_HOSTAPI u32 mg_host_last_error(void);
/*
 * The pages of watched [p, p + size) written since the memory was made or the watch was last started again, as page numbers
 * from `p` into `out` (at most `max`): how many; -1 where the host cannot say (and every page is to be taken for written).
 * `reset`: the watch started again for them. Pages of MG_PAGE bytes.
 */
MG_HOSTAPI int mg_mem_written_pages(void *p, size_t size, u32 *out, int max, bool reset);

/*
 * Memory that several mappings show at once: a section (a pagefile-backed file mapping on Windows, a memfd on Linux), the
 * host's half of shared memory as Android's ashmem and memfd give it. Created zero-filled; each view is the same pages at an
 * address of its own, writable or read-only; a view stays valid after the section is closed, until it is unmapped. A view's
 * offset is a multiple of a page: `base` is what mg_section_unmap takes, with the size mapped from there (the view may start
 * below the address returned, on the host's 64 KB granularity: that much more).
 */
typedef struct mg_section mg_section;
MG_HOSTAPI mg_section *mg_section_create(u64 size);
MG_HOSTAPI void *mg_section_map(mg_section *s, u64 offset, size_t size, bool writable, void **base);
/*
 * A view that may be read and run, not written: code written through a writable view of the same section (or the
 * descriptor) and run here, as a hook's or a JIT's two mappings of one memfd are. NULL where the host refuses it.
 */
MG_HOSTAPI void *mg_section_map_code(mg_section *s, u64 offset, size_t size, void **base);
MG_HOSTAPI void mg_section_unmap(void *base, size_t size);
MG_HOSTAPI void mg_section_close(mg_section *s);
/*
 * The address space that views hold now, in KB: those of sections and those of files (mg_file_map), mapped and not yet
 * given to mg_section_unmap (which unmaps both kinds). The crash report says it; the shared memory tests count by it.
 */
MG_HOSTAPI u32 mg_views_kb(void);

/* What the app may use before the OS ends it, and what it uses now; 0 when unknown. */
MG_HOSTAPI u64 mg_mem_limit(void);
MG_HOSTAPI u64 mg_mem_usage(void);
MG_HOSTAPI void mg_mem_set_probe(u64 (MG_HOSTCB *limit)(void), u64 (MG_HOSTCB *usage)(void));
/*
 * The machine's physical memory that is free or can be made so, which the app may still take; 0 when unknown. Windows 10
 * Mobile does not end a foreground app at its limit (mg_mem_limit): a Lumia 650 said 390 MB and let TikTok Lite use 500,
 * and ended Play Store at 877 MB, when the phone's 927 MB were gone. A test may set what it says (mg_mem_set_available_probe).
 */
MG_HOSTAPI u64 mg_mem_available(void);
MG_HOSTAPI void mg_mem_set_available_probe(u64 (MG_HOSTCB *available)(void));
/* The page faults the process has taken so far, the soft and the hard together; 0 where the host does not say. */
MG_HOSTAPI u64 mg_mem_page_faults(void);

/* --------------------------------------------------------------- threads */

typedef struct mg_thread mg_thread;
typedef void (MG_HOSTCB *mg_thread_fn)(void *arg);

/*
 * What a thread of the app's (a guest pthread, a Java thread) gets beyond the stack it asks for: 64 KB on Windows ARM64,
 * nothing elsewhere. There a fault, or a system call of an app's own left a trap (arm64/mg_patch_arm64.c), is an exception
 * dispatched on the thread's own stack below its sp -- KiUserExceptionDispatcher's CONTEXT and record (0x3b0 bytes),
 * RtlDispatchException's 0x2a0, the vectored handlers' frames and the core's fault handler's 5.6 KB -- where Android's
 * kernel takes nothing of the thread's stack: an app that sizes a stack for what its own code needs leaves none of that
 * at the bottom of it, and the process ended in RtlDispatchException's first store into its frame (GitHub issue 4).
 */
#if MG_ARCH_ARM64 && MG_HOST_WINDOWS
#define MG_STACK_FAULT_ROOM ((size_t)64 << 10)
#else
#define MG_STACK_FAULT_ROOM ((size_t)0)
#endif

MG_HOSTAPI mg_thread *mg_thread_start(mg_thread_fn fn, void *arg, size_t stack_size, const char *name);
MG_HOSTAPI void mg_thread_join(mg_thread *t);     /* frees t */
MG_HOSTAPI void mg_thread_detach(mg_thread *t);   /* frees t */
/* Ends the calling thread where it stands. */
MG_HOSTAPI MG_NORETURN void mg_thread_exit(void);
MG_HOSTAPI u32 mg_thread_self_id(void);
/* Another thread's id as mg_thread_self_id says it on that thread; 0 where the host does not keep it (posix). */
MG_HOSTAPI u32 mg_thread_id(const mg_thread *t);
/*
 * The calling thread runs the app's code from here on: it gives way to the
 * threads of the shell (its UI thread above all) whenever they have something
 * to do, and otherwise runs as it would.
 */
MG_HOSTAPI void mg_thread_runs_app_code(void);
/* A thread's priority among the process's: -2 lowest, -1 below normal (an app's own, giving way to the shell's), 0 normal,
   1 above normal, 2 highest; t NULL for the caller. False where the host has no such priorities. */
MG_HOSTAPI bool mg_thread_set_priority(mg_thread *t, int level);
/* The calling thread, however it was started; lives as long as the process. */
MG_HOSTAPI mg_thread *mg_thread_current(void);
/* The processor time a thread has used, user and kernel, in nanoseconds; the calling thread's when `t` is NULL. 0 when unknown. */
MG_HOSTAPI u64 mg_thread_cpu_ns(mg_thread *t);
/* The processor time the process has used, all its threads, in nanoseconds; 0 when unknown. */
MG_HOSTAPI u64 mg_process_cpu_ns(void);
/*
 * Interrupting a thread in the middle of guest code, which is what a signal
 * sent to it does on Android. `fn` is then called on that thread, and the
 * thread goes on where it was with every register as it was.
 * mg_thread_interrupt only does this to a thread that is executing code for
 * which `in_guest_code` says yes at that instant: a thread inside this layer
 * or the system may hold a lock that `fn` needs. It returns false when the
 * thread was elsewhere, and the caller asks again a little later.
 * `in_guest_code` is called while the other thread is stopped: it takes no
 * lock.
 */
MG_HOSTAPI void mg_thread_set_interrupt(void (MG_HOSTCB *fn)(void), bool (MG_HOSTCB *in_guest_code)(mg_addr pc));
MG_HOSTAPI bool mg_thread_interrupt(mg_thread *t);
/* What has become of the interruptions asked for so far, in a line: for the tests and the log. */
MG_HOSTAPI void mg_thread_interrupt_report(char *out, size_t size);
/*
 * Hardware breakpoints on guest code: the processor's breakpoint registers, which Windows on ARM keeps in each thread's
 * context, for instructions of a library that must stay as the file has them (arm/mg_patch.c, "left as the file has
 * it"). The list is the process's, and every guest thread is given it; a thread that reaches one stops before the
 * instruction, a fault of kind MG_FAULT_BREAKPOINT at that address.
 *   mg_breakpoints_available  whether the host has them at all (not qemu's Linux, nor the PC)
 *   mg_breakpoint_add         an address put in the list, bit 0 set for Thumb: false when the list is full
 *   mg_breakpoints_arm        the list given to a thread: another one, stopped for the moment and read back; the caller
 *                             (t NULL or its own) through an exception's context. True at once with an empty list.
 */
MG_HOSTAPI bool mg_breakpoints_available(void);
MG_HOSTAPI bool mg_breakpoint_add(mg_addr at);
MG_HOSTAPI int mg_breakpoint_count(void);
MG_HOSTAPI bool mg_breakpoints_arm(mg_thread *t);
/* Why the last mg_breakpoints_arm that failed did, with the thread's own id: for the log. */
MG_HOSTAPI const char *mg_breakpoints_why(void);
/*
 * Stopping another thread where it is, for the collector (dvm/): mg_thread_suspend stops `t`, whatever it is executing, and
 * reads its registers; it stays stopped until mg_thread_resume. Never the calling thread. The registers are only read: the
 * context is never written back (no SetThreadContext), so the thread goes on exactly where it was, in the state it was in.
 * On Windows SuspendThread then GetThreadContext, which is how Windows says to wait until the thread has stopped; elsewhere
 * a signal of the host's own whose handler saves the registers and waits. False when the thread could not be stopped (it
 * has ended); it is then not stopped.
 */
typedef struct mg_thread_regs {
    mg_addr sp, pc;
    u32 flags;                         /* CPSR on ARM: bit 5 is the Thumb state */
    int n;
    /*
     * The general registers that may hold anything, sp and pc not among them: on 32-bit ARM r0..r12 and lr (r[13]); on
     * x86_64 rax, rcx, rdx, rbx, rbp, rsi, rdi, r8..r15; on x86 eax, ecx, edx, ebx, ebp, esi, edi; on ARM64 x0..x28, fp, lr.
     */
    mg_addr r[32];
    /*
     * Of a thread stopped in an interruption (mg_thread_interrupt: running the function it was sent to, a guest signal's
     * handler, on the thread itself), the general registers of the code it interrupted, `n_was` of them in r's order. On
     * Windows they are kept on the host's heap until the thread goes back: nothing else of the thread holds them meanwhile,
     * neither its registers nor its stack, and a collector takes them as it takes r. 0 for a thread in no interruption,
     * and on the POSIX host, whose interruption is a signal: the kernel's frame holds them, on the stack the thread was on.
     */
    int n_was;
    mg_addr was[32];
} mg_thread_regs;
MG_HOSTAPI bool mg_thread_suspend(mg_thread *t, mg_thread_regs *out);
MG_HOSTAPI void mg_thread_resume(mg_thread *t);
/* For the tests' log: how many stops found a thread whose registers Windows would not give right and stopped it again. */
MG_HOSTAPI void mg_thread_suspend_report(char *out, size_t size);
/*
 * Another thread of the process by its id (as mg_thread_self_id says it on that thread), one the layer may not have
 * started: stopped while its registers and up to `max_words` words of its stack from sp up are copied (no further than
 * the memory there is committed), then let go. For the hang report. The words copied, or -1 when it could not be stopped
 * (posix: always).
 */
MG_HOSTAPI int mg_thread_snapshot_id(u32 id, mg_thread_regs *regs, mg_addr *words, int max_words);
/*
 * Windows ARM64 alone: each time a thread is stopped and its registers read (mg_thread_suspend, mg_thread_interrupt), x18
 * is compared with the thread's TEB, recorded as it started; a pc where they differed is given to `fn` once, with x18,
 * the TEB, the thread's id and how many stops have found them different so far -- not while a thread is stopped (the
 * stopped one may hold the log's lock), but once none is: after the last mg_thread_resume, at the end of an interruption.
 * Nothing elsewhere.
 */
MG_HOSTAPI void mg_thread_set_x18_report(void (MG_HOSTCB *fn)(mg_addr pc, mg_addr x18, mg_addr teb, u32 thread, u32 stops));
/* Called on a thread started by mg_thread_start as it ends (its function returned, or mg_thread_exit): the collector forgets it. */
MG_HOSTAPI void mg_thread_set_end_hook(void (MG_HOSTCB *fn)(void));
MG_HOSTAPI void mg_thread_yield(void);
MG_HOSTAPI void mg_sleep_ns(u64 ns);
MG_HOSTAPI int mg_cpu_count(void);
#if MG_ARCH_ARM32
/*
 * Whether the processor divides in hardware in Thumb state (SDIV, UDIV). The ARMv7 cores of Windows's phones and a
 * Surface 2's Cortex-A15 do; a Surface RT's Cortex-A9 (Tegra 3, under desktop Windows 10 build 15035: docs/RT.md) does
 * not: Windows traps either there and emulates it, some 80 cycles a time (Microsoft's ARM32 ABI conventions), and Linux
 * -- qemu-arm playing a Cortex-A9 -- takes it for an undefined instruction. The JIT's int division and what /proc/cpuinfo
 * and AT_HWCAP tell an app follow it.
 */
MG_HOSTAPI bool mg_cpu_arm_divide(void);
#endif

#if MG_HOST_WINDOWS
typedef struct mg_lock { void *srw; } mg_lock;        /* SRWLOCK */
typedef struct mg_cond { void *cv; } mg_cond;         /* CONDITION_VARIABLE */
#define MG_LOCK_INIT { 0 }
#define MG_COND_INIT { 0 }
#else
typedef struct mg_lock { pthread_mutex_t m; } mg_lock;
typedef struct mg_cond { pthread_cond_t c; bool monotonic; } mg_cond;
#define MG_LOCK_INIT { PTHREAD_MUTEX_INITIALIZER }
#define MG_COND_INIT { PTHREAD_COND_INITIALIZER, false }
#endif

MG_HOSTAPI void mg_lock_init(mg_lock *l);
MG_HOSTAPI void mg_lock_acquire(mg_lock *l);
MG_HOSTAPI bool mg_lock_try(mg_lock *l);
MG_HOSTAPI void mg_lock_release(mg_lock *l);
MG_HOSTAPI void mg_cond_init(mg_cond *c);
MG_HOSTAPI void mg_cond_wait(mg_cond *c, mg_lock *l);
/* Returns false when the time ran out. */
MG_HOSTAPI bool mg_cond_wait_ns(mg_cond *c, mg_lock *l, u64 ns);
MG_HOSTAPI void mg_cond_signal(mg_cond *c);
MG_HOSTAPI void mg_cond_broadcast(mg_cond *c);

MG_HOSTAPI i32 mg_atomic_cas(volatile i32 *p, i32 expect, i32 value);   /* returns what was there */
MG_HOSTAPI i32 mg_atomic_add(volatile i32 *p, i32 delta);                /* returns the new value */
MG_HOSTAPI i32 mg_atomic_xchg(volatile i32 *p, i32 value);
MG_HOSTAPI void *mg_atomic_cas_ptr(void *volatile *p, void *expect, void *value);
/* The same for 64 bits, which a 32-bit processor does with a pair of exclusive instructions; `p` is aligned to 8 bytes. */
MG_HOSTAPI i64 mg_atomic_cas64(volatile i64 *p, i64 expect, i64 value);

/*
 * Waiting on a word of memory (a futex; WaitOnAddress on Windows): what the
 * guest's mutexes, conditions and semaphores are built from, since bionic's
 * are four bytes each with no room for a host object.
 * mg_futex_wait returns when *addr != expect, on a wake, or spuriously; false
 * only when `timeout_ns` (0: none) ran out.
 */
MG_HOSTAPI bool mg_futex_wait(volatile i32 *addr, i32 expect, u64 timeout_ns);
MG_HOSTAPI void mg_futex_wake(volatile i32 *addr, bool all);
/* The calling thread's stack: [lo, hi). */
MG_HOSTAPI void mg_thread_stack_bounds(void **lo, void **hi);
/*
 * The limits of the stack the calling thread now runs on, as the host's own code goes by them (Windows' stack probes and
 * unwinder read the thread's NT_TIB), set to [lo, hi); what they were is put in *was_lo and *was_hi when not NULL. For a
 * thread that goes on in a stack of another's memory (mg_dvm_invoke's on_more_stack) and back. Nothing on POSIX.
 */
MG_HOSTAPI void mg_thread_stack_limits(void *lo, void *hi, void **was_lo, void **was_hi);
/*
 * Makes all of the calling thread's stack usable at once, for a thread about
 * to run Android code. Windows hands a stack out a page at a time, to code
 * that feels its way down (every frame bigger than a page probes each page
 * in turn, and touching the guard page below the used part is what brings
 * the next one). Android code does not: a function with a 300 KB array just
 * moves sp and writes. That lands beyond the guard page, in memory not yet
 * given, and the fault cannot even be delivered, since the stack it would be
 * delivered on is the one that is missing: Windows ends the process with no
 * handler run and nothing said. (Seen with Unity on a phone: a texture
 * decoder on the graphics thread, 0.7 s into the first scene.)
 */
MG_HOSTAPI void mg_thread_whole_stack(void);

/* Thread-local slots of the host's own. */
MG_HOSTAPI u32 mg_tls_alloc(void);
MG_HOSTAPI void *mg_tls_get(u32 key);
MG_HOSTAPI void mg_tls_set(u32 key, void *value);

/* ------------------------------------------------- the guest thread pointer
 *
 * Android code reads its thread pointer straight from the CPU:
 * `mrc p15, 0, rX, c13, c0, 3` (TPIDRURO). That register belongs to the host
 * here (Windows keeps its own data there, glibc its thread pointer), so every
 * such instruction is redirected to a few instructions that load the guest's
 * thread pointer from host thread-local storage instead (arm/mg_patch.c).
 * These are the host's half: where the value lives, and how two instructions
 * can fetch it into a register without touching anything else -- read
 * coprocessor register c13, c0, <opc2> (the host's own thread block), then
 * load the word at <offset> from it.
 */
MG_HOSTAPI void mg_guest_tp_set(void *tp);
MG_HOSTAPI void *mg_guest_tp_get(void);
/*
 * False when this host cannot say (and thread-pointer reads then cannot be redirected). On the other processors `opc2` says
 * where the host's own thread block is instead, and `offset` the slot's place in it (two's complement, below the block
 * where that is negative): x86_64 and x86 the segment's prefix byte, 0x65 for gs (Windows' TEB on x86_64), 0x64 for fs
 * (Windows' TEB on x86, glibc's thread pointer on x86_64); ARM64 the register, 18 for x18 (Windows' TEB), 0 for TPIDR_EL0
 * (glibc's thread pointer).
 */
MG_HOSTAPI bool mg_guest_tp_where(u32 *opc2, u32 *offset);
/*
 * The stack guard beside it, for the processors whose code reads its canary through the thread pointer (x86_64 at
 * %fs:0x28, x86 at %gs:0x14, ARM64 at TPIDR_EL0+0x28: bionic's slot 5): a slot of the host's thread block that holds the
 * guard on every thread that runs guest code, where the patched reads load it from (x86/mg_patch_x86.c). Its place as
 * mg_guest_tp_where says one; false where there is none. mg_guest_guard_set gives the calling thread its value.
 */
MG_HOSTAPI bool mg_guest_guard_where(u32 *opc2, u32 *offset);
MG_HOSTAPI void mg_guest_guard_set(mg_addr guard);
/*
 * A word of the host's thread block that holds nothing of its own, for x86_64's pieces of island that do an access of the
 * thread pointer's segment through a register (x86/mg_patch_x86.c): such a piece keeps a register of the guest's there
 * while it computes the address in that register. Its place as mg_guest_tp_where says one; false where there is none.
 * A thread interrupted in the middle of a piece (mg_thread_interrupt) finds it as it was when it goes back: what it was
 * made to run meanwhile may have used it too.
 */
MG_HOSTAPI bool mg_guest_scratch_where(u32 *opc2, u32 *offset);

/* ----------------------------------------------------------------- faults
 *
 * Illegal instructions and access violations in guest code come here first
 * (a vectored exception handler; SIGILL/SIGSEGV/SIGBUS handlers under Linux).
 * The handler may change the registers and return true to resume.
 */
/* MG_FAULT_BREAKPOINT: a thread stopped at one of the process's hardware breakpoints (mg_breakpoint_add), before the instruction. */
enum { MG_FAULT_ILLEGAL = 1, MG_FAULT_ACCESS, MG_FAULT_OTHER, MG_FAULT_BREAKPOINT };

/*
 * A faulting thread's registers, in the order of mg_fault's regs: 32-bit ARM r0..r15; x86_64 rax, rcx, rdx, rbx, rsp,
 * rbp, rsi, rdi, r8..r15 (the instructions' own numbering), then rip; x86 eax, ecx, edx, ebx, esp, ebp, esi, edi, then
 * eip; ARM64 x0..x30, sp, pc. MG_REG_LR is -1 where the return address is not in a register.
 */
#if MG_ARCH_ARM32
enum { MG_REG_SP = 13, MG_REG_LR = 14, MG_REG_PC = 15, MG_REG_COUNT = 16 };
#elif MG_ARCH_X86_64
enum { MG_REG_SP = 4, MG_REG_LR = -1, MG_REG_PC = 16, MG_REG_COUNT = 17 };
#elif MG_ARCH_X86
enum { MG_REG_SP = 4, MG_REG_LR = -1, MG_REG_PC = 8, MG_REG_COUNT = 9 };
#else
enum { MG_REG_SP = 31, MG_REG_LR = 30, MG_REG_PC = 32, MG_REG_COUNT = 33 };
#endif

typedef struct mg_fault {
    int kind;
    u32 code;              /* the OS's own number: an NTSTATUS, or a signal */
    mg_addr address;       /* for MG_FAULT_ACCESS: what was touched */
    bool write, exec;
    mg_addr *regs;         /* MG_REG_COUNT of them, writable; NULL where the host cannot say */
    u32 *cpsr;             /* the flags (CPSR, EFLAGS, PSTATE), writable */
} mg_fault;

typedef bool (MG_HOSTCB *mg_fault_handler)(mg_fault *f);
/*
 * A fault of the system's own that is a known assertion of it, made harmless where it is (mg_host_win32.c): true when
 * the thread is to go on at f's registers as changed. Windows 10 Mobile's CoreUIComponents.dll ends the process over
 * an invalid change of its view's state, which TikTok Lite's start brings about.
 */
MG_HOSTAPI bool mg_host_repair_known_fault(mg_fault *f);
MG_HOSTAPI void mg_fault_set_handler(mg_fault_handler h);
/*
 * Where the faulting thread goes on when the handler returns true. `target`
 * is a code address as a function pointer holds it: bit 0 set for Thumb.
 *
 * Not something to do to regs[15] by hand. Where the Thumb bit of a context
 * lives is the host's own business: Linux wants an even pc and the T bit in
 * cpsr; Windows on ARM resumes in whatever state bit 0 of Pc says and ignores
 * the T bit it is handed (seen on a phone: a context resumed at an even Pc
 * with T set ran on in ARM state).
 */
MG_HOSTAPI void mg_fault_resume_at(mg_fault *f, mg_addr target);

/* ------------------------------------------------------------------- time */

MG_HOSTAPI u64 mg_time_monotonic_ns(void);
/*
 * A count that only grows, for timing what is short and frequent (the Java runtime's native calls and allocations): as
 * cheap to read as the host allows, with no arithmetic. mg_ticks_ns turns a count, or a difference of two, into
 * nanoseconds. On Windows it is QueryPerformanceCounter's own count, which mg_time_monotonic_ns divides three times; on
 * 32-bit ARM those are calls of the C library's 64-bit division. Elsewhere it is nanoseconds.
 */
MG_HOSTAPI u64 mg_time_ticks(void);
MG_HOSTAPI u64 mg_ticks_ns(u64 ticks);
MG_HOSTAPI u64 mg_time_realtime_ns(void);         /* since the Unix epoch */
/* Offset of local time from UTC in seconds at `unix_seconds`, and whether DST is on. */
MG_HOSTAPI i32 mg_time_utc_offset(i64 unix_seconds, bool *dst);

/* ------------------------------------------------------------------ files */

typedef struct mg_file mg_file;

enum {
    MG_O_READ = 1, MG_O_WRITE = 2, MG_O_CREATE = 4, MG_O_TRUNC = 8, MG_O_APPEND = 16, MG_O_EXCL = 32,
};
enum { MG_E_OK = 0, MG_E_NOENT, MG_E_ACCESS, MG_E_EXIST, MG_E_NOTDIR, MG_E_ISDIR, MG_E_NOSPC, MG_E_IO, MG_E_INVAL, MG_E_NOTEMPTY, MG_E_BUSY,
       MG_E_XDEV };                        /* a rename from one file system to another */

typedef struct mg_stat {
    bool is_dir;
    bool read_only;                    /* a file that may not be written (Windows' read-only attribute; no owner write bit) */
    u64 size;
    i64 mtime_ns, atime_ns, ctime_ns;  /* since the Unix epoch */
    u64 id;                            /* something distinct per file */
} mg_stat;

MG_HOSTAPI mg_file *mg_file_open(const char *path, int flags, int *err);
MG_HOSTAPI void mg_file_close(mg_file *f);
/*
 * A file reached only through handles someone else hands out (on Windows, a file the user picked, which an app container
 * may not open by its path: Windows' broker gives handles to it), under a path of the caller's: from now on mg_file_open
 * opens `path` for reading by calling `open(user)` for a new handle of its own (a HANDLE), and refuses it for writing.
 * The path is compared without regard to case or to '/' and '\'. NULL `open` takes the path back. A few at a time.
 * Windows only: false elsewhere.
 */
MG_HOSTAPI bool mg_file_by_handle(const char *path, void *(MG_HOSTCB *open)(void *user), void *user);
/* Bytes moved, or -1 with *err set. Positioned variants leave the file's own position alone. */
MG_HOSTAPI i64 mg_file_read(mg_file *f, void *buf, size_t n, int *err);
MG_HOSTAPI i64 mg_file_write(mg_file *f, const void *buf, size_t n, int *err);
MG_HOSTAPI i64 mg_file_pread(mg_file *f, void *buf, size_t n, u64 offset, int *err);
MG_HOSTAPI i64 mg_file_pwrite(mg_file *f, const void *buf, size_t n, u64 offset, int *err);
MG_HOSTAPI i64 mg_file_seek(mg_file *f, i64 offset, int whence, int *err);   /* whence: 0 set, 1 cur, 2 end */
MG_HOSTAPI bool mg_file_truncate(mg_file *f, u64 size, int *err);
MG_HOSTAPI bool mg_file_sync(mg_file *f);
MG_HOSTAPI bool mg_file_stat(mg_file *f, mg_stat *st);
/*
 * A range of a file's bytes locked for this handle, shared or `exclusive`, as SQLite locks a database: at once or not at all
 * (false with *err MG_E_BUSY when another handle holds a lock there that conflicts -- of another process or of this one, each
 * handle its own as Windows' are). A file system that has no such locks grants every one, and says nothing. Unlocking what
 * is not locked does nothing.
 */
MG_HOSTAPI bool mg_file_lock(mg_file *f, u64 offset, u64 length, bool exclusive, int *err);
MG_HOSTAPI void mg_file_unlock(mg_file *f, u64 offset, u64 length);
/* Whether another handle holds a lock on part of the range: a test, which takes nothing (SQLite's xCheckReservedLock). */
MG_HOSTAPI bool mg_file_locked_elsewhere(mg_file *f, u64 offset, u64 length);
/*
 * A view of [offset, offset + size) of a file, read-only, backed by the file (not memory of the process's): NULL when the
 * host cannot make one. `base` is what mg_section_unmap takes, with the size mapped from it (the view begins at the host's
 * granularity below `offset`).
 */
MG_HOSTAPI void *mg_file_map(mg_file *f, u64 offset, size_t size, void **base, size_t *mapped);

MG_HOSTAPI bool mg_path_stat(const char *path, mg_stat *st, int *err);
MG_HOSTAPI bool mg_path_mkdir(const char *path, int *err);
MG_HOSTAPI bool mg_path_mkdirs(const char *path);
MG_HOSTAPI bool mg_path_remove(const char *path, int *err);        /* a file */
/*
 * A folder on the same disk as the app's files where mg_path_remove puts a file someone still has open before deleting it,
 * so that its name goes at once, as POSIX's unlink takes it (mg_host_win32.c, remove_open); what an earlier run left there
 * is deleted. Up to four, tried in order (the app's data, the shared storage). Nothing on POSIX hosts.
 */
MG_HOSTAPI void mg_path_add_trash(const char *dir);
MG_HOSTAPI bool mg_path_rmdir(const char *path, int *err);
MG_HOSTAPI bool mg_path_rename(const char *from, const char *to, int *err);
/*
 * A file made read-only or writable again, as chmod takes the write bits away and gives them back: Windows' read-only
 * attribute, or the owner's write bit. A directory is left as it is (true). The file stays removable and replaceable by
 * mg_path_remove and mg_path_rename, as POSIX's unlink and rename look at the folder, not at the file.
 */
MG_HOSTAPI bool mg_path_set_writable(const char *path, bool writable, int *err);
/* Calls cb for every entry but . and ..; false with *err set when the directory cannot be listed. */
typedef void (MG_HOSTCB *mg_dir_cb)(const char *name, bool is_dir, void *user);
MG_HOSTAPI bool mg_path_list(const char *path, mg_dir_cb cb, void *user, int *err);

/*
 * Names Windows refuses. Android allows every character in a name but '/' and NUL; Windows refuses < > : " \ | ? * and
 * the characters 1 to 31, a name that ends in a dot or a space, and the names of its devices (CON, PRN, AUX, NUL, COM0 to
 * 9, LPT0 to 9, with or without an extension). Such a name is given to Windows with each of those characters as the one
 * U+F000 above it -- a refused character, the dot or space at the end, the first letter of a device's name -- which is
 * what Cygwin and WSL do, and a name read back from Windows has every character from U+F001 to U+F07F as the one below
 * it again. A name that needed nothing is passed as it is. (One that holds a character of that private-use range of its
 * own is read back as the ASCII character; nothing on Android names files so.)
 *
 * mg_name_to_host makes every name in a path one Windows takes, malloc'd. MG_NAME_HOST: a host path, where '\' is a
 * separator as well as '/', and a leading \\?\ or \\.\ and a drive's "C:" are the place, not names. MG_NAME_FOREIGN: a
 * foreign path's part below its root, where '/' alone separates and a '\' is part of a name. mg_name_from_host undoes it
 * in a name read back, in place. The Win32 host layer does the first for every path it gives Windows and the second for
 * every name it lists; foreign paths are mapped before their owner is asked (below), on every host.
 */
#define MG_NAME_FOREIGN 0
#define MG_NAME_HOST 1
MG_HOSTAPI char *mg_name_to_host(const char *path, int how);
MG_HOSTAPI void mg_name_from_host(char *name);
/*
 * Whether this host's own file system needs the names mapped: Windows' does. Where a guest's path is joined to a host
 * folder (bionic/mg_libc_io.c) the guest's part is then mapped as a foreign path's is, so that a backslash in a name
 * stays a name's and does not become a separator; the host layer's own mapping finds nothing more to do in it.
 */
#if MG_HOST_WINDOWS
#define MG_HOST_MAPS_NAMES 1
#else
#define MG_HOST_MAPS_NAMES 0
#endif
/* The whole file, malloc'd (a NUL follows the bytes, not counted in *size); NULL when it cannot be read. */
MG_HOSTAPI u8 *mg_file_read_all(const char *path, size_t *size);
MG_HOSTAPI bool mg_file_write_all(const char *path, const void *data, size_t size);

/* A read-only view of a whole file (for APKs: nothing is copied until it is read). */
typedef struct mg_map {
    const u8 *data;
    u64 size;
    void *h1, *h2;
} mg_map;
MG_HOSTAPI bool mg_map_open(mg_map *m, const char *path);
MG_HOSTAPI void mg_map_close(mg_map *m);

/* How much the volume a folder is on holds and how much of that this process may still fill, in bytes; false where it cannot be asked (of a foreign path: never). */
MG_HOSTAPI bool mg_path_space(const char *path, u64 *total, u64 *available);

/* ---------------------------------------------------------- foreign paths
 *
 * Files the operating system will not open by path for this process but
 * hands over as handles some other way. On a phone that is the user's music,
 * pictures and videos: an app container may not name them to CreateFile2, and
 * Windows.Storage's broker gives out real file handles for them.
 *
 * Such a file has a host path all the same: MG_FOREIGN_PREFIX, the number of
 * a root, and a path below it. "megler-link:/2/Camera Roll/a.jpg" is a file of
 * root 2, and "megler-link:/2" that root itself. Every function above that
 * takes a path takes one of these. Instead of opening by path it asks
 * whoever was registered here for the operating system's own handle and goes
 * on with that, so reading, writing, mapping and listing are the same code
 * as for any other file. Making, removing and renaming have no handle to go
 * through, and are asked for as such.
 *
 * What follows the root's number reaches the callbacks with forward slashes
 * and none at either end; "" is the root itself. Its names are mapped as
 * Windows needs them (mg_name_to_host, MG_NAME_FOREIGN): a backslash or a
 * colon in a name, say, reaches them as U+F05C or U+F03A, and the names
 * listed through their handles are mapped back by the host layer. A path
 * with an empty part or a "." or ".." part is refused here (MG_E_INVAL) and
 * never reaches them. A root itself cannot be
 * made (MG_E_EXIST), removed or renamed (MG_E_BUSY), and that too is answered
 * here.
 *
 * A rename between a foreign path and an ordinary one, either way, is
 * MG_E_XDEV: two file systems, as far as anything above can tell.
 */
#define MG_FOREIGN_PREFIX "megler-link:/"

enum {
    MG_FOREIGN_FILE = 0,               /* a file, to read and write: MG_E_ISDIR for a directory */
    MG_FOREIGN_DIR,                    /* a directory, to list it: MG_E_NOTDIR for a file */
    MG_FOREIGN_ANY,                    /* whichever it is, for its size, kind and times only */
};

typedef struct mg_foreign {
    void *user;
    /*
     * The operating system's own handle, the caller's to close: a HANDLE on
     * Win32, the file descriptor plus one on POSIX. NULL with *err one of
     * MG_E_*. `flags` are MG_O_* (MG_O_APPEND may be ignored on Win32, where
     * the host layer seeks before each write itself); `kind` MG_FOREIGN_*.
     */
    void *(MG_HOSTCB *open)(void *user, int root, const char *rel, int flags, int kind, int *err);
    bool (MG_HOSTCB *mkdir)(void *user, int root, const char *rel, int *err);
    /* `dir`: a directory, which must be empty (MG_E_NOTEMPTY); else a file (MG_E_ISDIR for a directory). */
    bool (MG_HOSTCB *remove)(void *user, int root, const char *rel, bool dir, int *err);
    /* As rename(2): onto an existing file it replaces it. Roots may differ. MG_E_XDEV when it cannot be done as a rename. */
    bool (MG_HOSTCB *rename)(void *user, int from_root, const char *from, int to_root, const char *to, int *err);
} mg_foreign;

/* Who answers for foreign paths from now on; the structure is copied. NULL: nobody, and every foreign path is MG_E_NOENT. */
MG_HOSTAPI void mg_foreign_set(const mg_foreign *f);
MG_HOSTAPI bool mg_path_is_foreign(const char *path);

/*
 * For the two host files, which are otherwise the same for a foreign path as
 * for any other: the handle of one (NULL with *err set), and the three
 * things done by name.
 */
MG_HOSTAPI void *mg_foreign_open(const char *path, int flags, int kind, int *err);
MG_HOSTAPI bool mg_foreign_mkdir(const char *path, int *err);
MG_HOSTAPI bool mg_foreign_remove(const char *path, bool dir, int *err);
MG_HOSTAPI bool mg_foreign_rename(const char *from, const char *to, int *err);
/*
 * The operating system's own handle for a path of the host's, as a foreign path's owner hands one out (a HANDLE on Win32,
 * the descriptor plus one on POSIX; `flags` MG_O_*, `kind` MG_FOREIGN_*): NULL with *err set. For the tests' stand-in of
 * the shell's storage (core/tests/mg_storage_tests.c), which on desktop Windows is part of the core's ELF and calls no
 * operating system of its own.
 */
MG_HOSTAPI void *mg_host_open_handle(const char *path, int flags, int kind, int *err);

/* ---------------------------------------------------------------- sockets
 *
 * The operating system's own sockets: Winsock 2 on Windows, BSD sockets on
 * POSIX. Everything here is in this header's neutral terms -- families,
 * options, flags and errors are its own numbers, addresses a structure of its
 * own -- and bionic/mg_net.c turns Android's ABI into them and back.
 *
 * A socket here never blocks. What would wait answers MG_NE_AGAIN (and a
 * connection under way MG_NE_INPROGRESS), and the caller waits the way it
 * waits for every other descriptor of the layer: on its one condition
 * (bionic/mg_libc_io.c). For that the host says when the state of a socket
 * may have changed -- a connection arrived, made or failed, data arrived,
 * there is room to send again, the other side closed -- by calling the
 * function mg_sock_set_notify names, on a thread of its own, and
 * mg_sock_ready says what holds now. A change is announced after it has
 * happened, so a caller that looks with mg_sock_ready and then waits for the
 * announcement, both under one lock that the announcement also takes, misses
 * nothing.
 */
typedef struct mg_sock mg_sock;

enum { MG_AF_INET = 1, MG_AF_INET6 = 2 };
enum { MG_SOCK_STREAM = 1, MG_SOCK_DGRAM = 2, MG_SOCK_RAW = 3 };

typedef struct mg_sockaddr {
    int family;                        /* MG_AF_INET, MG_AF_INET6; 0: no address */
    u16 port;                          /* in the host's byte order */
    u8 addr[16];                       /* in the network's: an IPv4 address in the first four */
    u32 flowinfo, scope_id;            /* IPv6 */
} mg_sockaddr;

/* What went wrong. */
enum {
    MG_NE_OK = 0, MG_NE_AGAIN, MG_NE_INPROGRESS, MG_NE_ALREADY, MG_NE_ISCONN, MG_NE_NOTCONN, MG_NE_CONNREFUSED, MG_NE_CONNRESET,
    MG_NE_CONNABORTED, MG_NE_TIMEDOUT, MG_NE_NETUNREACH, MG_NE_HOSTUNREACH, MG_NE_NETDOWN, MG_NE_NETRESET, MG_NE_ADDRINUSE,
    MG_NE_ADDRNOTAVAIL, MG_NE_AFNOSUPPORT, MG_NE_PROTONOSUPPORT, MG_NE_SOCKTNOSUPPORT, MG_NE_OPNOTSUPP, MG_NE_NOPROTOOPT, MG_NE_INVAL,
    MG_NE_ACCES, MG_NE_MSGSIZE, MG_NE_NOBUFS, MG_NE_MFILE, MG_NE_SHUTDOWN, MG_NE_PIPE, MG_NE_DESTADDRREQ, MG_NE_FAULT, MG_NE_IO
};

/* What mg_sock_ready says. MG_SOCK_RDHUP: the other side will send no more (its FIN, or a reset). */
enum { MG_SOCK_IN = 1, MG_SOCK_OUT = 2, MG_SOCK_ERR = 4, MG_SOCK_HUP = 8, MG_SOCK_RDHUP = 16 };
/* Flags of mg_sock_send / mg_sock_recv, and what mg_sock_recv says of what it read. */
enum { MG_MSG_PEEK = 1, MG_MSG_OOB = 2, MG_MSG_DONTROUTE = 4 };
enum { MG_MSG_TRUNCATED = 1 };

/* Options, set and read as one integer each. */
enum {
    MG_SO_REUSEADDR = 1, MG_SO_KEEPALIVE, MG_SO_BROADCAST, MG_SO_OOBINLINE, MG_SO_RCVBUF, MG_SO_SNDBUF,
    MG_SO_LINGER,                      /* seconds; -1: off */
    MG_SO_DONTROUTE, MG_TCP_NODELAY, MG_TCP_KEEPIDLE, MG_TCP_KEEPINTVL, MG_TCP_KEEPCNT, MG_TCP_MAXSEG,
    MG_IP_TOS, MG_IP_TTL, MG_IP_MULTICAST_TTL, MG_IP_MULTICAST_LOOP,
    MG_IP_MULTICAST_IF,                /* an IPv4 address as its four bytes in the network's order, read as a u32 */
    MG_IPV6_V6ONLY, MG_IPV6_UNICAST_HOPS, MG_IPV6_MULTICAST_HOPS, MG_IPV6_MULTICAST_LOOP,
    MG_IPV6_MULTICAST_IF,              /* an interface's index */
    MG_IPV6_TCLASS
};

/*
 * A new socket: `type` MG_SOCK_*, `protocol` IANA's number (6 TCP, 17 UDP, 0
 * the type's own). An IPv6 socket takes IPv4 addresses too (written as
 * ::ffff:a.b.c.d) until MG_IPV6_V6ONLY says otherwise, which is Linux's
 * default and not Windows'. NULL with *err set.
 */
MG_HOSTAPI mg_sock *mg_sock_open(int family, int type, int protocol, int *err);
MG_HOSTAPI void mg_sock_close(mg_sock *s);
MG_HOSTAPI bool mg_sock_bind(mg_sock *s, const mg_sockaddr *a, int *err);
MG_HOSTAPI bool mg_sock_listen(mg_sock *s, int backlog, int *err);
/* A connection that has arrived, or NULL with *err MG_NE_AGAIN when none has. */
MG_HOSTAPI mg_sock *mg_sock_accept(mg_sock *s, mg_sockaddr *peer, int *err);
/* false with *err MG_NE_INPROGRESS while a connection is being made; mg_sock_error says how it ended. A datagram socket's family 0: no peer any more. */
MG_HOSTAPI bool mg_sock_connect(mg_sock *s, const mg_sockaddr *a, int *err);
/* Bytes sent (as many as there was room for), or -1 with *err set. `to` NULL: to the peer. */
MG_HOSTAPI i32 mg_sock_send(mg_sock *s, const void *buf, u32 n, int flags, const mg_sockaddr *to, int *err);
/* Bytes read, 0 at the end of a stream, or -1 with *err set. `from` and `said` (MG_MSG_TRUNCATED) may be NULL. */
MG_HOSTAPI i32 mg_sock_recv(mg_sock *s, void *buf, u32 n, int flags, mg_sockaddr *from, int *said, int *err);
MG_HOSTAPI bool mg_sock_shutdown(mg_sock *s, int how, int *err);   /* how: 0 reading, 1 writing, 2 both */
MG_HOSTAPI bool mg_sock_name(mg_sock *s, bool peer, mg_sockaddr *a, int *err);
/* What holds now, MG_SOCK_*; never waits. */
MG_HOSTAPI int mg_sock_ready(mg_sock *s);
/* The error a connection ended with (SO_ERROR), MG_NE_*; asking clears it. */
MG_HOSTAPI int mg_sock_error(mg_sock *s);
/* Bytes that can be read without waiting (FIONREAD); whether the next byte is urgent data's mark (SIOCATMARK). */
MG_HOSTAPI i32 mg_sock_pending(mg_sock *s);
MG_HOSTAPI bool mg_sock_at_mark(mg_sock *s);
/* MG_NE_NOPROTOOPT: the option means nothing to this host. */
MG_HOSTAPI bool mg_sock_set(mg_sock *s, int option, int value, int *err);
MG_HOSTAPI bool mg_sock_get(mg_sock *s, int option, int *value, int *err);
/* Joins or leaves a multicast group: by an IPv4 interface's address (`iface`, family 0 for any) or an interface's index. */
MG_HOSTAPI bool mg_sock_multicast(mg_sock *s, bool join, const mg_sockaddr *group, const mg_sockaddr *iface, u32 index, int *err);
/* Who is told that some socket's state may have changed; called on a thread of the host's, never with a lock of the caller's held. */
MG_HOSTAPI void mg_sock_set_notify(void (MG_HOSTCB *changed)(void));

/* ---------------------------------------------------- names and interfaces */

enum { MG_EAI_OK = 0, MG_EAI_NONAME, MG_EAI_AGAIN, MG_EAI_FAIL, MG_EAI_NODATA, MG_EAI_FAMILY, MG_EAI_MEMORY, MG_EAI_SYSTEM };
enum { MG_RESOLVE_CANONICAL = 1, MG_RESOLVE_ADDRCONFIG = 2 };
/*
 * The addresses of a name, as the system's resolver has them (DNS, the hosts
 * file): a malloc'd array of *count, port 0, in the order the system prefers
 * them. `family` MG_AF_* or 0 for both. *canonical, when asked for with
 * MG_RESOLVE_CANONICAL, is the canonical name, malloc'd, or stays NULL.
 * Returns MG_EAI_*. A name only: numbers are the caller's to parse.
 */
MG_HOSTAPI int mg_net_resolve(const char *name, int family, int flags, mg_sockaddr **addrs, int *count, char **canonical);
/* The name an address has (a reverse lookup), MG_EAI_NONAME when it has none. */
MG_HOSTAPI int mg_net_name_of(const mg_sockaddr *a, char *name, size_t size);

/* The host's network interfaces. */
enum { MG_IF_LOOPBACK = 1, MG_IF_WIFI, MG_IF_ETHERNET, MG_IF_CELLULAR, MG_IF_TUNNEL, MG_IF_OTHER };
enum { MG_IFF_UP = 1, MG_IFF_RUNNING = 2, MG_IFF_MULTICAST = 4, MG_IFF_BROADCAST = 8, MG_IFF_POINTOPOINT = 16 };
#define MG_NETIF_ADDRS 12
typedef struct mg_netif {
    u32 index;
    int kind;                          /* MG_IF_* */
    u32 flags;                         /* MG_IFF_* */
    u32 mtu;
    u8 hw[8];
    int hw_len;                        /* 0: no hardware address said */
    int naddrs;
    struct {
        mg_sockaddr addr;
        int prefix;                    /* bits of the network part */
    } addrs[MG_NETIF_ADDRS];
} mg_netif;
/* Fills up to `max` and returns how many there are; -1 when they cannot be listed. */
MG_HOSTAPI int mg_net_interfaces(mg_netif *out, int max);
/*
 * The name servers the phone uses now, as text ("192.168.31.1", "2001:db8::1"): those of the interfaces that are up, the
 * ones with a default gateway first, each once. Up to `max`; how many were written. What Android's LinkProperties.
 * getDnsServers says, which a resolver of an app's own (c-ares, inside 2GIS's map library) asks to know where to send its
 * queries.
 */
MG_HOSTAPI int mg_net_dns_servers(char (*out)[48], int max);

/* ------------------------------------------------------ host shared libraries */

/* A DLL of the app package / a shared object, by file name. NULL when it does not load. */
MG_HOSTAPI void *mg_dl_open(const char *name);
MG_HOSTAPI void *mg_dl_sym(void *lib, const char *name);
/* Which of the host's own modules (the app, a DLL of the package or of the system) an address is in: its file name and where it is loaded. For a crash report. */
MG_HOSTAPI bool mg_host_module_at(mg_addr address, char *name, size_t size, mg_addr *base);
/*
 * The linker's time stamp of the module an address is in (a PE file's TimeDateStamp), which its build's map says at its top
 * ("Timestamp is ..."): which build a report's module+offset addresses are of. False where there is none (an ELF, POSIX).
 */
MG_HOSTAPI bool mg_host_module_stamp(mg_addr address, u32 *stamp);
/* A module of the process's, by file name, as it lies in memory into a file (megler_dump_module); its base and image size. */
MG_HOSTAPI bool mg_host_module_dump(const char *name, const char *path, mg_addr *base, size_t *size);
/* Whether the system stops a thread at a hardware breakpoint set through its context: a probe, said in `out` (megler_probe_breakpoints). */
MG_HOSTAPI bool mg_host_probe_breakpoints(char *out, size_t size);

/* ------------------------------------------------------------------- misc */

/* The feature level of a Direct3D 11 device (an ID3D11Device *), as D3D_FEATURE_LEVEL numbers it (0x9300, 0xb100); 0 where there is no Direct3D. */
MG_HOSTAPI int mg_host_d3d11_feature_level(void *device);
/*
 * The highest feature level the graphics processor's own Direct3D 11 offers (D3D_DRIVER_TYPE_HARDWARE), numbered so; asked
 * of Direct3D with no device made. 0 where there is no Direct3D (posix) or no such device. 9_1 on a Surface RT's and a
 * Surface 2's Tegra (docs/RT.md), below the 9_3 ANGLE needs; 9_3 and up on every phone's.
 */
MG_HOSTAPI int mg_host_d3d11_hardware_level(void);
/* Why a Direct3D 11 device is gone (an HRESULT such as DXGI_ERROR_DEVICE_HUNG), or 0 while it is still there. */
MG_HOSTAPI u32 mg_host_d3d11_removed_reason(void *device);
/*
 * What Direct3D 11.1 a device's driver offers that ANGLE's speed depends on (D3D11_FEATURE_D3D11_OPTIONS): constant buffers
 * bound at an offset (without them ANGLE makes a buffer of its own for each range of a uniform buffer an app binds), and
 * constant buffers updated in part. False where it cannot be asked.
 */
MG_HOSTAPI bool mg_host_d3d11_options(void *device, bool *constant_buffer_offsets, bool *constant_buffer_partial_updates);
/* How much graphics memory the process may use and does (megabytes), as the adapter of a Direct3D 11 device says; false where it cannot be asked. */
MG_HOSTAPI bool mg_host_gpu_memory(void *device, u32 *budget_mb, u32 *used_mb);
/*
 * A DXGI keyed mutex (an IDXGIKeyedMutex *: what ANGLE shares its pbuffers' textures with) acquired, waiting up to `ms`, or
 * released, with key 0. Its device is made safe for use from several threads first (from then on its Direct3D calls take a
 * lock, which these take too: the app's other threads may be drawing on that device). False when it could not be acquired in
 * time, and where there is no DXGI.
 */
MG_HOSTAPI bool mg_host_keyed_mutex(void *mutex, bool acquire, u32 ms);
/*
 * The process's address space, in megabytes: how much is free, the largest
 * free stretch, and how much is reserved without being committed. A 32-bit
 * process has two gigabytes, and it is the largest free stretch, not the
 * total, that decides whether the next big allocation -- anyone's, the
 * system's included -- can be made. False where it cannot be walked.
 */
MG_HOSTAPI bool mg_host_address_space(u32 *free_mb, u32 *largest_free_mb, u32 *reserved_mb, u32 *committed_mb);
/*
 * What holds the process's addresses, in megabytes: the images (the program and its DLLs), the views of files and shared
 * memory, and the private memory, committed and reserved; and the sizes of private allocation that hold the most of it,
 * with how many of each. Yandex Maps ran out of addresses with 210 MB of the last 340 MB in nothing the core could name
 * (package 490). False where it cannot be walked. (The heaps are not asked: HeapSummary walks a heap, and of the process's
 * several some are not serialized -- walked while their owner used them, one was broken and the app ended, package 493 --
 * and it held the process heap's lock for most of a second.)
 *
 * And whose each size's allocations are, where it can be said: `whose` (the core's) names the owner of the allocation at
 * `base` -- the Java heap's arenas, a library's mappings, Go's heap -- or gives NULL, and then one with a guard page in it
 * is a thread's stack; a name is told apart from another by its pointer. WhatsApp's log said "1 of 528384 KB" and "599 of
 * 1024 KB", and whose they were took a disassembly and a guess (HP Elite x3, 0.1.0.671).
 */
#define MG_ADDRESS_OWNERS 3
typedef struct mg_host_address_kinds {
    u32 images_mb, views_mb, private_mb, private_reserved_mb;
    int nsizes;
    struct {
        u32 kb, count;
        struct { const char *whose; u32 count; } by[MG_ADDRESS_OWNERS];   /* the owners with the most of them, the most first; NULL past the last */
    } sizes[4];
} mg_host_address_kinds;
/* The owner of the private allocation at `base` of `size` bytes, as the core knows it, or NULL (mg_host_address_census). */
typedef const char *(MG_HOSTCB *mg_address_whose_fn)(const void *base, size_t size);
/* The threads mg_thread_start started that run now: each has the stack it was given, 1 MB of addresses or more. */
MG_HOSTAPI u32 mg_thread_live_count(void);
MG_HOSTAPI bool mg_host_address_census(mg_host_address_kinds *k, mg_address_whose_fn whose);
/* The bytes of [base, base + size) reserved and not committed; 0 where the host cannot say. */
MG_HOSTAPI u64 mg_host_reserved_in(const void *base, size_t size);
/*
 * The `n` largest free stretches of the address space, in megabytes, largest first, 0 past the last. Two allocations of
 * one size need two stretches of it: Chromium in a single process maps each image it decodes twice. False where the
 * address space cannot be walked.
 */
MG_HOSTAPI bool mg_host_free_stretches(u32 *mb, int n);

/* Bytes from the OS's random source. */
MG_HOSTAPI void mg_random(void *buf, size_t n);
/* A line to the debugger / stderr; the log's last resort. */
MG_HOSTAPI void mg_host_print(const char *line);
MG_HOSTAPI const char *mg_host_name(void);        /* "win32-arm", "win32-x64", "linux-arm" */

#if MG_HOST_WINDOWS && MG_ARCH_X86
#pragma pack(pop)
#endif

#ifdef __cplusplus
}
#endif

#endif /* MG_HOST_H */
