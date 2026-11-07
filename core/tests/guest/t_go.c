/*
 * A guest library that stands for one of Go's (tools/build-guest.sh: every ABI but x86, whose `int 0x80` of a library's
 * own is not led out to the core): it has Go's build ID note, which the loader takes a library of Go's by
 * (mg_ld_lib_is_go), and does what Go's runtime does with a goroutine's stack. A goroutine's stack is a piece of Go's
 * heap; Go's scavenger gives the heap's free pages back with madvise, by number from its own code, and soon puts a
 * goroutine on such pages again, whose first frame then touches them first on its own stack -- main.GoWork.func1 of a
 * c-shared library's 1000 goroutines, `sub $0x10038,%rsp; movb $0,0x17(%rsp)`. Here: a heap of 512 KB mapped, a
 * goroutine's 256 KB stack at its top, a 64 KB frame on it (t_go_heap_new), the stack's pages or the rest of the heap's
 * given back as Go's runtime gives them back or as any other code does (t_go_give_back), the frame run on the stack again
 * (t_go_run), and what reads zero (t_go_nonzero). See core/tests/mg_tests.c, test_go_stack, for what each must come to.
 */
#include "t_abi.h"

void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);
int madvise(void *, size_t, int);
int mprotect(void *, size_t, int);
void *memset(void *, int, size_t);
int getpid(void);
typedef unsigned long t_pthread;
int pthread_create(t_pthread *, const void *, void *(*)(void *), void *);
int pthread_join(t_pthread, void **);

/* Go's build ID note: owner "Go" (namesz 4, as Go's linker writes it), type 4, a made-up ID. */
__asm__(".section .note.go.buildid,\"a\",%note\n"
        ".p2align 2\n"
        ".long 4\n"
        ".long 8\n"
        ".long 4\n"
        ".byte 0x47, 0x6f, 0, 0\n"
        ".byte 0x74, 0x5f, 0x67, 0x6f, 0x2d, 0x69, 0x64, 0\n"
        ".text\n");

/*
 * t_go_on_stack(top, fn, arg): fn(arg) with the stack pointer at `top` (16 bytes aligned), then back -- a goroutine run on
 * a stack of the caller's. t_go_syscall3(nr, a, b, c): a system call of the library's own by its number, as Go's runtime
 * makes it (`syscall`, `svc 0`), the kernel's result (-errno on failure).
 */
#if defined(__x86_64__)
__asm__(".text\n"
        ".globl t_go_on_stack\n.type t_go_on_stack, @function\n"
        "t_go_on_stack:\n"
        "  pushq %rbx\n"
        "  movq %rsp, %rbx\n"
        "  movq %rdi, %rsp\n"
        "  movq %rdx, %rdi\n"
        "  callq *%rsi\n"
        "  movq %rbx, %rsp\n"
        "  popq %rbx\n"
        "  ret\n"
        ".size t_go_on_stack, .-t_go_on_stack\n"
        ".globl t_go_syscall3\n.type t_go_syscall3, @function\n"
        "t_go_syscall3:\n"
        "  movq %rdi, %rax\n"
        "  movq %rsi, %rdi\n"
        "  movq %rdx, %rsi\n"
        "  movq %rcx, %rdx\n"
        "  syscall\n"
        "  ret\n"
        ".size t_go_syscall3, .-t_go_syscall3\n");
#elif defined(__arm__)
__asm__(".text\n"
        ".syntax unified\n"
        ".thumb\n"
        ".globl t_go_on_stack\n.type t_go_on_stack, %function\n.thumb_func\n"
        "t_go_on_stack:\n"
        "  push {r4, lr}\n"
        "  mov r4, sp\n"
        "  mov sp, r0\n"
        "  mov r0, r2\n"
        "  blx r1\n"
        "  mov sp, r4\n"
        "  pop {r4, pc}\n"
        ".size t_go_on_stack, .-t_go_on_stack\n"
        ".globl t_go_syscall3\n.type t_go_syscall3, %function\n.thumb_func\n"
        "t_go_syscall3:\n"
        "  push {r7, lr}\n"
        "  mov r7, r0\n"
        "  mov r0, r1\n"
        "  mov r1, r2\n"
        "  mov r2, r3\n"
        "  svc #0\n"
        "  pop {r7, pc}\n"
        ".size t_go_syscall3, .-t_go_syscall3\n"

        /* Go's runtime's own way, in ARM state (t_go_arm_svc, below): the number in r7, `svc 0`. */
        ".arm\n"
        ".globl t_go_arm_getpid\n.type t_go_arm_getpid, %function\n"
        "t_go_arm_getpid:\n"
        "  push {r7}\n"
        "  mov r7, #20\n"
        "  svc #0\n"
        "  pop {r7}\n"
        "  bx lr\n"
        ".size t_go_arm_getpid, .-t_go_arm_getpid\n"

        ".globl t_go_arm_keep\n.type t_go_arm_keep, %function\n"
        "t_go_arm_keep:\n"
        "  push {r4-r11, lr}\n"
        "  vpush {d8}\n"
        "  push {r0}\n"
        "  str sp, [r0, #100]\n"
        "  movw r1, #0x1111\n  movt r1, #0x1111\n  movw r2, #0x2222\n  movt r2, #0x2222\n"
        "  movw r3, #0x3333\n  movt r3, #0x3333\n  movw r4, #0x4444\n  movt r4, #0x4444\n"
        "  movw r5, #0x5555\n  movt r5, #0x5555\n  movw r6, #0x6666\n  movt r6, #0x6666\n"
        "  movw r8, #0x8888\n  movt r8, #0x8888\n  movw r9, #0x9999\n  movt r9, #0x9999\n"
        "  movw r10, #0xaaaa\n  movt r10, #0xaaaa\n  movw r11, #0xbbbb\n  movt r11, #0xbbbb\n"
        "  movw r12, #0xcccc\n  movt r12, #0xcccc\n  movw lr, #0xeeee\n  movt lr, #0xeeee\n"
        "  vmov d0, r1, r2\n  vmov d7, r3, r4\n  vmov d8, r5, r6\n  vmov d16, r8, r9\n  vmov d31, r10, r11\n"
        "  movw r0, #0\n  movt r0, #0xb00f\n"
        "  msr APSR_nzcvqg, r0\n"
        "  mov r7, #20\n"
        "  svc #0\n"
        "  push {r0-r12, lr}\n"
        "  mrs r0, apsr\n"
        "  ldr r1, [sp, #56]\n"
        "  mov r2, sp\n"
        "  ldmia r2!, {r3-r10}\n  stmia r1!, {r3-r10}\n"
        "  ldmia r2!, {r3-r8}\n  stmia r1!, {r3-r8}\n"
        "  str r0, [r1], #4\n"
        "  vmov r3, r4, d0\n  vmov r5, r6, d7\n  vmov r7, r8, d8\n  stmia r1!, {r3-r8}\n"
        "  vmov r3, r4, d16\n  vmov r5, r6, d31\n  stmia r1!, {r3-r6}\n"
        "  add r2, sp, #56\n"
        "  str r2, [r1, #4]\n"
        "  add sp, sp, #60\n"
        "  vpop {d8}\n"
        "  pop {r4-r11, pc}\n"
        ".size t_go_arm_keep, .-t_go_arm_keep\n"

        ".globl t_go_arm_sc6\n.type t_go_arm_sc6, %function\n"
        "t_go_arm_sc6:\n"
        "  push {r4, r5, r7, lr}\n"
        "  mov r7, r0\n"
        "  mov r0, r1\n"
        "  mov r1, r2\n"
        "  mov r2, r3\n"
        "  ldr r3, [sp, #16]\n"
        "  ldr r4, [sp, #20]\n"
        "  ldr r5, [sp, #24]\n"
        "  svc #0\n"
        "  pop {r4, r5, r7, pc}\n"
        ".size t_go_arm_sc6, .-t_go_arm_sc6\n"

        ".globl t_go_arm_on_stack\n.type t_go_arm_on_stack, %function\n"
        "t_go_arm_on_stack:\n"
        "  push {r4, r7, lr}\n"
        "  mov r4, sp\n"
        "  mov sp, r0\n"
        "  mov r7, #20\n"
        "  svc #0\n"
        "  mov sp, r4\n"
        "  pop {r4, r7, pc}\n"
        ".size t_go_arm_on_stack, .-t_go_arm_on_stack\n"
        ".thumb\n");
#elif defined(__aarch64__)
__asm__(".text\n"
        ".globl t_go_on_stack\n.type t_go_on_stack, %function\n"
        "t_go_on_stack:\n"
        "  stp x19, x30, [sp, #-16]!\n"
        "  mov x19, sp\n"
        "  mov sp, x0\n"
        "  mov x0, x2\n"
        "  blr x1\n"
        "  mov sp, x19\n"
        "  ldp x19, x30, [sp], #16\n"
        "  ret\n"
        ".size t_go_on_stack, .-t_go_on_stack\n"
        ".globl t_go_syscall3\n.type t_go_syscall3, %function\n"
        "t_go_syscall3:\n"
        "  mov x8, x0\n"
        "  mov x0, x1\n"
        "  mov x1, x2\n"
        "  mov x2, x3\n"
        "  svc #0\n"
        "  ret\n"
        ".size t_go_syscall3, .-t_go_syscall3\n");
#else
#error "t_go.c: x86_64, armeabi-v7a and arm64-v8a only"
#endif

long t_go_on_stack(void *top, long (*fn)(long), long arg);
long t_go_syscall3(long nr, long a, long b, long c);
#if defined(__arm__)
long t_go_arm_getpid(void);
void t_go_arm_keep(unsigned *out);
long t_go_arm_sc6(long nr, long a, long b, long c, long d, long e, long f);
long t_go_arm_on_stack(void *top);
#endif

#if defined(__arm__)
enum { NR_madvise = 220, NR_prctl = 172 };
#elif defined(__x86_64__)
enum { NR_madvise = 28, NR_prctl = 157 };
#else
enum { NR_madvise = 233, NR_prctl = 167 };
#endif

enum { HEAP = 512 * 1024, STACK = 256 * 1024 };
static unsigned char *g_heap;

/*
 * A goroutine's first frame on its stack: 64 KB, written at its far end first, then its near end and its middle: 2 * seed
 * + 8. (Its address given to an empty asm: the array is then the frame's, whole, rather than three bytes that clang keeps
 * below the stack pointer.)
 */
__attribute__((noinline)) static long go_frame(long seed)
{
    volatile unsigned char buf[64 * 1024 + 56];
    __asm__ volatile("" : : "r"((const void *)buf) : "memory");
    buf[0] = (unsigned char)seed;
    buf[sizeof(buf) - 1] = (unsigned char)(seed + 1);
    buf[40000] = 7;
    return buf[0] + buf[sizeof(buf) - 1] + buf[40000];
}

/* The heap mapped and written whole, and a goroutine run on the stack at its top: the frame's sum, or -1. */
long t_go_heap_new(long seed)
{
    g_heap = (unsigned char *)mmap(0, HEAP, 3, 0x22, -1, 0);
    if (g_heap == (unsigned char *)-1) {
        g_heap = 0;
        return -1;
    }
    memset(g_heap, 0x5A, HEAP);
    return t_go_on_stack(g_heap + HEAP, go_frame, seed);
}

/*
 * Pages of the heap given back with madvise(MADV_DONTNEED), `how`: 0 the goroutine's stack (the heap's top half) by number
 * from this library's code, as Go's scavenger gives its heap's pages back; 1 the rest of the heap so; 2 the stack through
 * libc's madvise, as any other code does. 0, or -1 when madvise failed. Nothing is read: a read of a page given back
 * takes it back.
 */
long t_go_give_back(long how)
{
    unsigned char *at = g_heap + (how == 1 ? 0 : HEAP - STACK);
    return (how == 2 ? madvise(at, STACK, 4) : t_go_syscall3(NR_madvise, (long)at, STACK, 4)) == 0 ? 0 : -1;
}

/* How many pages of the stack (`part` 0) or of the rest of the heap (1) do not read zero, at their first and last bytes. */
long t_go_nonzero(long part)
{
    unsigned char *at = g_heap + (part ? 0 : HEAP - STACK);
    long nonzero = 0, i;
    for (i = 0; i < STACK; i += 4096)
        if (at[i] || at[i + 4095]) nonzero++;
    return nonzero;
}

/* A goroutine run on the stack again, its frame's first touch on the pages given back: the frame's sum. */
long t_go_run(long seed) { return t_go_on_stack(g_heap + HEAP, go_frame, seed); }

void t_go_heap_free(void)
{
    if (g_heap) munmap(g_heap, HEAP);
    g_heap = 0;
}

#if defined(__arm__)
/*
 * ARM's svc 0 of a library of Go's, Go's runtime's way to the kernel, led out to the core (arm/mg_patch.c): the call made
 * on the thread's own stack where the caller is on it, and on the thread's stack for system calls where it is on another,
 * a goroutine's of two kilobytes here with no memory below it. What must come of it, 0 or the step that failed:
 *
 *   1    getpid by svc, with the stack 4 mod 8 (push {r7}), is libc's getpid
 *   2..  t_go_arm_keep: every register but r0, the flags (N, C, V and the four GE bits set) and d0, d7, d8, d16, d31 as
 *        they were before it, and sp
 *   30.. a C function of six arguments around it (Go's Syscall6): mmap2, munmap, close of no descriptor (-EBADF)
 *   40.. on a goroutine's stack, a hundred times: nothing below its sp written but the 16 bytes the way out puts by, and
 *        nothing above it; its stack's lowest page has no access, as Go's heap below a stack may have none
 *   50.. the same on four threads of their own, each on its own stack and on one of its own of two kilobytes
 */
static long g_go_pid;

static long svc_stack_round(unsigned char *page)
{
    unsigned char *stack = page + 4096, *top = stack + 2048;
    long bad = 0, i;
    memset(stack, 0x5A, 4096);
    for (i = 0; i < 100; i++)
        if (t_go_arm_on_stack(top) != g_go_pid) bad++;
    for (i = 0; i < 4096; i++)
        if ((stack + i < top - 16 || stack + i >= top) && stack[i] != 0x5A) return 1000 + i;
    return bad;
}

static void *svc_thread(void *arg)
{
    unsigned char *page = (unsigned char *)arg;
    long bad = 0, i;
    for (i = 0; i < 200; i++)
        if (t_go_arm_getpid() != g_go_pid) bad++;
    bad += svc_stack_round(page);
    return (void *)bad;
}

long t_go_arm_svc(void)
{
    unsigned out[27];
    unsigned char *map, *pages;
    t_pthread t[4];
    long i;
    g_go_pid = getpid();
    if (t_go_arm_getpid() != g_go_pid) return 1;
    memset(out, 0, sizeof(out));
    t_go_arm_keep(out);
    if (out[0] != (unsigned)g_go_pid) return 2;
    {
        static const unsigned kept[14] = { 0, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u, 0x66666666u, 20,
                                           0x88888888u, 0x99999999u, 0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0xeeeeeeeeu };
        for (i = 1; i < 14; i++)
            if (out[i] != kept[i]) return 2 + i;
    }
    if ((out[14] & 0xF80F0000u) != 0xB00F0000u) return 16;
    if (out[15] != 0x11111111u || out[16] != 0x22222222u || out[17] != 0x33333333u || out[18] != 0x44444444u) return 17;
    if (out[19] != 0x55555555u || out[20] != 0x66666666u) return 18;
    if (out[21] != 0x88888888u || out[22] != 0x99999999u || out[23] != 0xaaaaaaaau || out[24] != 0xbbbbbbbbu) return 19;
    if (out[25] != out[26]) return 20;
    map = (unsigned char *)t_go_arm_sc6(192 /* mmap2 */, 0, 8192, 3, 0x22, -1, 0);
    if ((long)map < 0 && (long)map > -4096) return 30;
    map[0] = 1;
    map[8191] = 2;
    if (t_go_arm_sc6(91 /* munmap */, (long)map, 8192, 0, 0, 0, 0) != 0) return 31;
    if (t_go_arm_sc6(6 /* close */, -1, 0, 0, 0, 0, 0) != -9) return 32;
    /* Five pairs of pages: a page with no access, and a goroutine's stack in the next. */
    pages = (unsigned char *)mmap(0, 5 * 8192, 3, 0x22, -1, 0);
    if (pages == (unsigned char *)-1) return 40;
    for (i = 0; i < 5; i++)
        if (mprotect(pages + i * 8192, 4096, 0) != 0) return 41;
    i = svc_stack_round(pages);
    if (i) return i >= 1000 ? 42 : 43;
    for (i = 0; i < 4; i++)
        if (pthread_create(&t[i], 0, svc_thread, pages + (i + 1) * 8192) != 0) return 50;
    for (i = 0; i < 4; i++) {
        void *r = (void *)-1L;
        pthread_join(t[i], &r);
        if (r) return 51 + i;
    }
    munmap(pages, 5 * 8192);
    return 0;
}
#endif

/*
 * prctl by number, as Go's runtime (1.25 on) names each mapping it makes, PR_SET_VMA's PR_SET_VMA_ANON_NAME: refused with
 * EINVAL, as a kernel without names for anonymous memory refuses it, which is what makes Go stop asking. PR_SET_NAME and
 * PR_GET_NAME as libc's prctl takes them, the thread's name put back after. 0, or which of them failed.
 */
long t_go_prctl(void)
{
    static const char named[] = "t_go_named";
    char was[16], now[16];
    unsigned i;
    if (t_go_syscall3(NR_prctl, 0x53564d41, 0, (long)g_heap) != -22) return 1;
    if (t_go_syscall3(NR_prctl, 16, (long)was, 0) != 0) return 2;
    if (t_go_syscall3(NR_prctl, 15, (long)named, 0) != 0) return 3;
    memset(now, 0, sizeof(now));
    if (t_go_syscall3(NR_prctl, 16, (long)now, 0) != 0) return 4;
    for (i = 0; i < sizeof(named); i++)
        if (now[i] != named[i]) return 5;
    t_go_syscall3(NR_prctl, 15, (long)was, 0);
    return 0;
}
