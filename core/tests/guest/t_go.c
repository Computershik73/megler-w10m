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
void *memset(void *, int, size_t);

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
        ".size t_go_syscall3, .-t_go_syscall3\n");
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
