#if defined(__arm__)
/* Direct ARM EABI calls. The one-word push deliberately leaves sp 4 mod 8. */
__attribute__((naked)) int t_svc_getpid(void)
{
    __asm__ volatile("push {r7}\nmovw r7, #20\nsvc 0\npop {r7}\nbx lr\n");
}

__attribute__((naked)) int t_svc_literal(void)
{
    __asm__ volatile("push {r7}\nldr r7, 1f\nsvc 0\npop {r7}\nbx lr\n.balign 4\n1: .word 20\n");
}

__attribute__((naked)) int t_svc_unknown(void)
{
    __asm__ volatile("push {r7}\nmovw r7, #65535\nsvc 0\npop {r7}\nbx lr\n");
}

/* compiler-rt's __clear_cache (Facebook's libdexload.so): the number's top half set last, by movt, just before the svc. */
__attribute__((naked)) int t_svc_clear_cache(void *start, unsigned length)
{
    __asm__ volatile("add r1, r0\npush {r7}\nmovs r7, #2\nmovs r2, #0\nmovt r7, #15\nsvc 0\npop {r7}\nbx lr\n");
}

/* The number put in r7 further back, the instruction just before the svc not one that sets it (Chromium's syscall()). */
__attribute__((naked)) int t_svc_far_number(void)
{
    __asm__ volatile("push {r4, r7}\nmovs r7, #20\nmov r4, r4\nsvc 0\npop {r4, r7}\nbx lr\n");
}

/* TikTok Lite's libmetasec_ov.so's anti-disassembly stubs: an svc right behind a branch (mov pc, r1), jumped over. The
   loader leaves it as the file has it in Thumb code, which that library sums; the function returns 7. */
__attribute__((naked)) int t_svc_dead_stub(void)
{
    __asm__ volatile("adr r1, 1f\nmov pc, r1\nmov r0, pc\nsvc 0\npop {r0-r3, r7}\n.balign 4\n1: movs r0, #7\nbx lr\n");
}

/* Read-only data, in the executable segment here (--no-rosegment) as in libmetasec_ov.so, one of whose tables holds
   0xEF000000: the word of an ARM svc 0, and a Thumb movs r7, #20 and svc 0. The loader leaves them: they are no code. */
const unsigned t_svc_rodata[2] = { 0xEF000000u, 0xDF002714u };

/* Chromium's cacheflush wrapper: start + length, flags 0, literal syscall nr. */
__attribute__((naked)) int t_svc_cacheflush(void *start, unsigned length)
{
    __asm__ volatile("add r1, r0\nmovs r2, #0\npush {r7}\nldr r7, 1f\nsvc 0\npop {r7}\nbx lr\n.balign 4\n1: .word 0xf0002\n");
}

#elif defined(__aarch64__)
/*
 * ARM64's svc 0 of a library's own, led out to the core through a piece of the library's island (arm64/mg_patch_arm64.c),
 * not a trap: on Windows a trap is an exception laid on the guest's stack and dispatched by ntdll's code, some 8 KB of
 * stack for each call (GitHub issue 4). What each test function must come to, 0 or the step that failed (see
 * core/tests/mg_tests.c, test_guest):
 *
 *   t_svc64_results   getpid by svc is libc's getpid; close of no descriptor -EBADF; a number no kernel has -ENOSYS; mmap
 *                     and munmap; the guest's errno as it was all along
 *   t_svc64_regs      every register but x0 as the call found it (keep_checked: 100 + the register, 131 the flags, 132
 *                     FPSR, 133 sp, 137 FPCR, 140 + the vector register): x1..x30 (x18 as it was, never written), the
 *                     flags, FPSR, FPCR and q0..q31 whole, the upper halves of q8..q15 among them, which a function of the
 *                     core's need not keep
 *   t_svc64_signal    the same around rt_sigprocmask by svc letting through signal 10, which was sent to the thread while
 *                     it held it back: its handler runs inside the call and makes a call of its own by svc on the same
 *                     thread, while the first one's words are in use
 *   t_svc64_stacks    on a goroutine's stack of the caller's, two kilobytes with no memory below its page, a hundred
 *                     calls: nothing written below its sp but the piece's 16 bytes, nothing above it (12; 13 a call's
 *                     result wrong), as the call is made on the thread's stack for system calls; the same on four
 *                     threads of their own, each with 200 calls on its own stack before (21 + the thread); and signal
 *                     10's handler with its call of its own on such a stack (30 + t_svc64_signal's step, 40 a byte
 *                     written)
 *   t_svc64_fallback  the thread's stack for system calls taken away (the guest thread pointer's slot 15,
 *                     MG_TCB_SYSCALL): the entry sends the thread to the piece's trap, and the fault handler makes the
 *                     call as it made every one before, the registers kept all the same
 */
#include "t_abi.h"

typedef unsigned long t_u64;
int getpid(void);
int *__errno(void);
void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
pthread_t pthread_self(void);
int pthread_kill(pthread_t, int);
/* bionic's LP64 struct sigaction: the flags first. */
typedef struct {
    int flags;
    void (*handler)(int);
    t_word mask;
    void (*restorer)(void);
} t_sigaction64;
int sigaction(int, const t_sigaction64 *, t_sigaction64 *);
int sigprocmask(int, const t_word *, t_word *);

enum { NR_close = 57, NR_rt_sigprocmask = 135, NR_getpid = 172, NR_munmap = 215, NR_mmap = 222 };

/*
 * t_svc64_call(nr, a..f): a system call of the library's own, as Go's Syscall6 makes one; what the kernel gives (-errno on
 * failure). t_svc64_on(top, nr, a..d): the same with the stack pointer at `top`, a goroutine's stack of the caller's, x9
 * holding the caller's meanwhile. t_svc64_keep(io): the call io[8] with the arguments io[0..5], every other register given
 * a mark first -- x6, x7, x9..x17 and x19..x30 mark(n), q0..q31 qmark(n), the flags N, C and V, FPSR's QC, IXC, UFC, OFC,
 * DZC and IOC -- and then everything as the call left it written to io: x0..x30 at io[0..30] (the result at io[0]), the
 * flags io[31], FPSR io[32], sp io[33], q0..q31 io[40..103]; and before the call sp io[34], x18 io[35], FPCR io[36], and
 * after it FPCR io[37].
 */
long t_svc64_call(long nr, long a, long b, long c, long d, long e, long f);
long t_svc64_on(void *top, long nr, long a, long b, long c, long d);
void t_svc64_keep(t_u64 *io);
__asm__(".text\n"
        ".globl t_svc64_call\n.type t_svc64_call, %function\n"
        "t_svc64_call:\n"
        "  mov x8, x0\n  mov x0, x1\n  mov x1, x2\n  mov x2, x3\n  mov x3, x4\n  mov x4, x5\n  mov x5, x6\n"
        "  svc #0\n"
        "  ret\n"
        ".size t_svc64_call, .-t_svc64_call\n"

        ".globl t_svc64_on\n.type t_svc64_on, %function\n"
        "t_svc64_on:\n"
        "  mov x9, sp\n"
        "  mov sp, x0\n"
        "  mov x8, x1\n  mov x0, x2\n  mov x1, x3\n  mov x2, x4\n  mov x3, x5\n"
        "  svc #0\n"
        "  mov sp, x9\n"
        "  ret\n"
        ".size t_svc64_on, .-t_svc64_on\n"

        ".globl t_svc64_keep\n.type t_svc64_keep, %function\n"
        "t_svc64_keep:\n"
        "  stp x29, x30, [sp, #-176]!\n"
        "  stp x19, x20, [sp, #16]\n  stp x21, x22, [sp, #32]\n  stp x23, x24, [sp, #48]\n  stp x25, x26, [sp, #64]\n"
        "  stp x27, x28, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
        "  mrs x9, fpsr\n"
        "  stp x0, x9, [sp, #160]\n"                 /* io, and FPSR to give back */
        "  mov x9, sp\n  str x9, [x0, #272]\n"       /* io[34]: sp before */
        "  str x18, [x0, #280]\n"                    /* io[35]: x18 before (read) */
        "  mrs x9, fpcr\n  str x9, [x0, #288]\n"     /* io[36]: FPCR before */
        "  .irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
        "  movz x9, #(0x0101 * \\i)\n  movk x9, #0x5100, lsl #48\n"
        "  movz x10, #(0x0101 * \\i)\n  movk x10, #0x5200, lsl #48\n"
        "  fmov d\\i, x9\n  mov v\\i\\().d[1], x10\n"
        "  .endr\n"
        "  movz x9, #0x001f\n  movk x9, #0x0800, lsl #16\n  msr fpsr, x9\n"
        "  .irp n, 6,7,9,10,11,12,13,14,15,16,17,19,20,21,22,23,24,25,26,27,28,29\n"
        "  movz x\\n, #(0x0101 * \\n)\n  movk x\\n, #(0xA500 + \\n), lsl #48\n"
        "  .endr\n"
        "  movz x30, #0xB000, lsl #16\n  msr nzcv, x30\n"
        "  ldr x8, [x0, #64]\n  ldp x2, x3, [x0, #16]\n  ldp x4, x5, [x0, #32]\n  ldr x1, [x0, #8]\n  ldr x0, [x0, #0]\n"
        "  movz x30, #(0x0101 * 30)\n  movk x30, #(0xA500 + 30), lsl #48\n"
        "  svc #0\n"
        "  stp x0, x1, [sp, #-16]!\n"
        "  ldr x1, [sp, #176]\n"                     /* io */
        "  str x0, [x1, #0]\n  ldr x0, [sp, #8]\n  str x0, [x1, #8]\n"
        "  stp x2, x3, [x1, #16]\n  stp x4, x5, [x1, #32]\n  stp x6, x7, [x1, #48]\n  stp x8, x9, [x1, #64]\n"
        "  stp x10, x11, [x1, #80]\n  stp x12, x13, [x1, #96]\n  stp x14, x15, [x1, #112]\n  stp x16, x17, [x1, #128]\n"
        "  stp x18, x19, [x1, #144]\n  stp x20, x21, [x1, #160]\n  stp x22, x23, [x1, #176]\n  stp x24, x25, [x1, #192]\n"
        "  stp x26, x27, [x1, #208]\n  stp x28, x29, [x1, #224]\n  str x30, [x1, #240]\n"
        "  mrs x0, nzcv\n  str x0, [x1, #248]\n"
        "  mrs x0, fpsr\n  str x0, [x1, #256]\n"
        "  add x0, sp, #16\n  str x0, [x1, #264]\n"
        "  mrs x0, fpcr\n  str x0, [x1, #296]\n"
        "  stp q0, q1, [x1, #320]\n  stp q2, q3, [x1, #352]\n  stp q4, q5, [x1, #384]\n  stp q6, q7, [x1, #416]\n"
        "  stp q8, q9, [x1, #448]\n  stp q10, q11, [x1, #480]\n  stp q12, q13, [x1, #512]\n  stp q14, q15, [x1, #544]\n"
        "  stp q16, q17, [x1, #576]\n  stp q18, q19, [x1, #608]\n  stp q20, q21, [x1, #640]\n  stp q22, q23, [x1, #672]\n"
        "  stp q24, q25, [x1, #704]\n  stp q26, q27, [x1, #736]\n  stp q28, q29, [x1, #768]\n  stp q30, q31, [x1, #800]\n"
        "  add sp, sp, #16\n"
        "  ldr x9, [sp, #168]\n  msr fpsr, x9\n"
        "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
        "  ldp x19, x20, [sp, #16]\n  ldp x21, x22, [sp, #32]\n  ldp x23, x24, [sp, #48]\n  ldp x25, x26, [sp, #64]\n"
        "  ldp x27, x28, [sp, #80]\n"
        "  ldp x29, x30, [sp], #176\n"
        "  ret\n"
        ".size t_svc64_keep, .-t_svc64_keep\n");

static t_u64 mark(int n) { return (t_u64)(0xA500 + n) << 48 | (t_u64)(0x0101 * n); }
static t_u64 qmark(int n, int high) { return (t_u64)(high ? 0x5200 : 0x5100) << 48 | (t_u64)(0x0101 * n); }

/* io as t_svc64_keep left it, `in` what it was given: 0, or the first thing that is not as the call must leave it. */
static long keep_checked(const t_u64 *io, const t_u64 *in)
{
    int n;
    for (n = 1; n <= 30; n++) {
        t_u64 want = n <= 5 || n == 8 ? in[n] : n == 18 ? io[35] : mark(n);
        if (io[n] != want) return 100 + n;
    }
    if ((io[31] >> 28) != 0xB) return 131;
    if (io[32] != 0x0800001Fu) return 132;
    if (io[33] != io[34]) return 133;
    if (io[37] != io[36]) return 137;
    for (n = 0; n < 32; n++)
        if (io[40 + 2 * n] != qmark(n, 0) || io[41 + 2 * n] != qmark(n, 1)) return 140 + n;
    return 0;
}

/* getpid by svc through t_svc64_keep: 0, or what keep_checked said, or 1 when the call's result was not getpid's. */
static long kept_getpid(void)
{
    t_u64 io[104], in[9];
    memset(io, 0, sizeof(io));
    io[1] = 0x1111;
    io[2] = 0x2222;
    io[3] = 0x3333;
    io[4] = 0x4444;
    io[5] = 0x5555;
    io[8] = NR_getpid;
    memcpy(in, io, sizeof(in));
    t_svc64_keep(io);
    if ((long)io[0] != getpid()) return 1;
    return keep_checked(io, in);
}

long t_svc64_results(void)
{
    int *err = __errno(), saved = *err;
    long r = 0;
    char *page;
    *err = 123;
    if (t_svc64_call(NR_getpid, 0, 0, 0, 0, 0, 0) != getpid()) r = 1;
    else if (t_svc64_call(NR_close, -1, 0, 0, 0, 0, 0) != -9) r = 2;
    else if (t_svc64_call(0x7ff, 0, 0, 0, 0, 0, 0) != -38) r = 3;
    else if (*err != 123) r = 4;
    else {
        page = (char *)t_svc64_call(NR_mmap, 0, 8192, 3, 0x22, -1, 0);
        if ((long)page < 0 && (long)page > -4096) {
            r = 5;
        } else {
            page[0] = 1;
            page[8191] = 2;
            if (t_svc64_call(NR_munmap, (long)page, 8192, 0, 0, 0, 0) != 0) r = 6;
        }
    }
    if (!r && *err != 123) r = 7;
    *err = saved;
    return r;
}

long t_svc64_regs(void) { return kept_getpid(); }

static volatile long g_handled, g_nested;
static void on_usr1(int sig)
{
    (void)sig;
    g_handled++;
    g_nested = t_svc64_call(NR_getpid, 0, 0, 0, 0, 0, 0);
}

/*
 * Signal 10 held back, sent to this thread, then let through by rt_sigprocmask by svc: on this thread's stack through
 * t_svc64_keep (`top` NULL), or with sp at `top`. Its handler runs inside the call and makes one of its own by svc. 0, or
 * the step that failed.
 */
static long signalled(void *top)
{
    t_sigaction64 act, old;
    t_word usr1 = (t_word)1 << 9, was = 0;
    long r = 0;
    memset(&act, 0, sizeof(act));
    act.handler = on_usr1;
    if (sigaction(10, &act, &old)) return 1;
    if (sigprocmask(0 /* SIG_BLOCK */, &usr1, &was)) return 2;
    g_handled = 0;
    g_nested = 0;
    if (pthread_kill(pthread_self(), 10)) r = 3;
    else if (g_handled) r = 4;
    else if (top) {
        if (t_svc64_on(top, NR_rt_sigprocmask, 1 /* SIG_UNBLOCK */, (long)&usr1, 0, 8) != 0) r = 5;
    } else {
        t_u64 io[104], in[9];
        memset(io, 0, sizeof(io));
        io[0] = 1;
        io[1] = (t_u64)&usr1;
        io[2] = 0;
        io[3] = 8;
        io[4] = 0x4444;
        io[5] = 0x5555;
        io[8] = NR_rt_sigprocmask;
        memcpy(in, io, sizeof(in));
        t_svc64_keep(io);
        if (io[0] != 0) r = 5;
        else r = keep_checked(io, in);
    }
    if (!r && (g_handled != 1 || g_nested != getpid())) r = 6;
    sigprocmask(2 /* SIG_SETMASK */, &was, 0);
    sigaction(10, &old, 0);
    return r;
}

long t_svc64_signal(void) { return signalled(0); }

/*
 * A goroutine's stack in the second page of `page`'s pair, the first with no access: getpid by svc a hundred times with sp
 * at its middle. Nothing below sp written but the piece's 16 bytes, nothing above it: how many calls were wrong, or 1000 +
 * the first byte written.
 */
static long stack_round(unsigned char *page, long pid)
{
    unsigned char *stack = page + 4096, *top = stack + 2048;
    long bad = 0, i;
    memset(stack, 0x5A, 4096);
    for (i = 0; i < 100; i++)
        if (t_svc64_on(top, NR_getpid, 0, 0, 0, 0) != pid) bad++;
    for (i = 0; i < 4096; i++)
        if ((stack + i < top - 16 || stack + i >= top) && stack[i] != 0x5A) return 1000 + i;
    return bad;
}

static unsigned char *g_pages;
static long g_pid;

static void *thread_round(void *arg)
{
    long bad = 0, i;
    for (i = 0; i < 200; i++)
        if (t_svc64_call(NR_getpid, 0, 0, 0, 0, 0, 0) != g_pid) bad++;
    return (void *)(bad + stack_round(g_pages + ((long)arg + 1) * 8192, g_pid));
}

long t_svc64_stacks(void)
{
    pthread_t t[4];
    unsigned char *stack, *top;
    long i, r = 0;
    g_pid = getpid();
    /* Five pairs of pages: a page with no access, and a goroutine's stack in the next. */
    g_pages = (unsigned char *)mmap(0, 5 * 8192, 3, 0x22, -1, 0);
    if (g_pages == (unsigned char *)-1) return 10;
    for (i = 0; i < 5; i++)
        if (mprotect(g_pages + i * 8192, 4096, 0) != 0) return 11;
    i = stack_round(g_pages, g_pid);
    if (i) return i >= 1000 ? 12 : 13;
    for (i = 0; i < 4; i++)
        if (pthread_create(&t[i], 0, thread_round, (void *)i) != 0) return 20;
    for (i = 0; i < 4; i++) {
        void *ret = (void *)-1L;
        pthread_join(t[i], &ret);
        if (ret && !r) r = 21 + i;
    }
    if (r) return r;
    /* The handler of a signal let through by a call on such a stack, and its own call: on the thread's stack for system
       calls, where the first call is made; nothing below the goroutine's sp but the piece's 16 bytes again. */
    stack = g_pages + 4096;
    top = stack + 2048;
    memset(stack, 0x5A, 4096);
    r = signalled(top);
    if (r) return 30 + r;
    for (i = 0; i < 4096; i++)
        if ((stack + i < top - 16 || stack + i >= top) && stack[i] != 0x5A) return 40;
    munmap(g_pages, 5 * 8192);
    return 0;
}

long t_svc64_fallback(void)
{
    void **tp, *block;
    long r;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    block = tp[15];
    if (!block) return 1;
    tp[15] = 0;
    r = kept_getpid();
    tp[15] = block;
    return r ? 10 + r : 0;
}

#else
#error "t_svc.c: armeabi-v7a and arm64-v8a only"
#endif
