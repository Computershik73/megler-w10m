/*
 * setjmp and longjmp (and the emulated thread-local variables' lookup) for the core's ELF on desktop Windows (docs/DESKTOP.md): the collector spills the registers it
 * may hold references in with setjmp before it scans its own stack (dvm/mg_dvm_gc.c), and Windows' setjmp is no use to
 * code of Android's convention. Each saves the registers its processor's convention has the callee keep, the stack
 * pointer and where to return, which fits in bionic's jmp_buf of the same processor (11 words on x86_64, 32 on ARM64,
 * 10 on x86). Compiled only into the core's ELF (MG_CORE_ELF).
 */
#include "../mg_base.h"

#if defined(MG_CORE_ELF)

/*
 * The core's thread-local variables are emulated (-femulated-tls): each is found through this, which the compiler calls.
 * Defined here so that compiler-rt's own (over pthread keys, which Windows has not) is not linked in; the bridge keeps
 * the variables in fiber-local storage (host/win32/mg_crt_bridge.c).
 */
void *mg_crt_emutls_get_address(void *control);
void *__emutls_get_address(void *control) { return mg_crt_emutls_get_address(control); }

#if MG_ARCH_X86_64
__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".hidden setjmp\n"
    ".type setjmp,@function\n"
    "setjmp:\n"
    "    movq %rbx, 0(%rdi)\n"
    "    movq %rbp, 8(%rdi)\n"
    "    movq %r12, 16(%rdi)\n"
    "    movq %r13, 24(%rdi)\n"
    "    movq %r14, 32(%rdi)\n"
    "    movq %r15, 40(%rdi)\n"
    "    leaq 8(%rsp), %rax\n"
    "    movq %rax, 48(%rdi)\n"
    "    movq (%rsp), %rax\n"
    "    movq %rax, 56(%rdi)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".size setjmp, .-setjmp\n"
    ".globl longjmp\n"
    ".hidden longjmp\n"
    ".type longjmp,@function\n"
    "longjmp:\n"
    "    movl %esi, %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    incl %eax\n"
    "1:\n"
    "    movq 0(%rdi), %rbx\n"
    "    movq 8(%rdi), %rbp\n"
    "    movq 16(%rdi), %r12\n"
    "    movq 24(%rdi), %r13\n"
    "    movq 32(%rdi), %r14\n"
    "    movq 40(%rdi), %r15\n"
    "    movq 48(%rdi), %rsp\n"
    "    jmpq *56(%rdi)\n"
    ".size longjmp, .-longjmp\n");
#elif MG_ARCH_ARM64
__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".hidden setjmp\n"
    ".type setjmp,%function\n"
    "setjmp:\n"
    "    stp x19, x20, [x0, #0]\n"
    "    stp x21, x22, [x0, #16]\n"
    "    stp x23, x24, [x0, #32]\n"
    "    stp x25, x26, [x0, #48]\n"
    "    stp x27, x28, [x0, #64]\n"
    "    stp x29, x30, [x0, #80]\n"
    "    mov x2, sp\n"
    "    str x2, [x0, #96]\n"
    "    stp d8, d9, [x0, #104]\n"
    "    stp d10, d11, [x0, #120]\n"
    "    stp d12, d13, [x0, #136]\n"
    "    stp d14, d15, [x0, #152]\n"
    "    mov w0, #0\n"
    "    ret\n"
    ".size setjmp, .-setjmp\n"
    ".globl longjmp\n"
    ".hidden longjmp\n"
    ".type longjmp,%function\n"
    "longjmp:\n"
    "    ldp x19, x20, [x0, #0]\n"
    "    ldp x21, x22, [x0, #16]\n"
    "    ldp x23, x24, [x0, #32]\n"
    "    ldp x25, x26, [x0, #48]\n"
    "    ldp x27, x28, [x0, #64]\n"
    "    ldp x29, x30, [x0, #80]\n"
    "    ldr x2, [x0, #96]\n"
    "    mov sp, x2\n"
    "    ldp d8, d9, [x0, #104]\n"
    "    ldp d10, d11, [x0, #120]\n"
    "    ldp d12, d13, [x0, #136]\n"
    "    ldp d14, d15, [x0, #152]\n"
    "    cmp w1, #0\n"
    "    csinc w0, w1, wzr, ne\n"
    "    ret\n"
    ".size longjmp, .-longjmp\n");
#elif MG_ARCH_X86
__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".hidden setjmp\n"
    ".type setjmp,@function\n"
    "setjmp:\n"
    "    movl 4(%esp), %eax\n"
    "    movl %ebx, 0(%eax)\n"
    "    movl %esi, 4(%eax)\n"
    "    movl %edi, 8(%eax)\n"
    "    movl %ebp, 12(%eax)\n"
    "    leal 4(%esp), %ecx\n"
    "    movl %ecx, 16(%eax)\n"
    "    movl (%esp), %ecx\n"
    "    movl %ecx, 20(%eax)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".size setjmp, .-setjmp\n"
    ".globl longjmp\n"
    ".hidden longjmp\n"
    ".type longjmp,@function\n"
    "longjmp:\n"
    "    movl 4(%esp), %edx\n"
    "    movl 8(%esp), %eax\n"
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    incl %eax\n"
    "1:\n"
    "    movl 0(%edx), %ebx\n"
    "    movl 4(%edx), %esi\n"
    "    movl 8(%edx), %edi\n"
    "    movl 12(%edx), %ebp\n"
    "    movl 16(%edx), %esp\n"
    "    jmp *20(%edx)\n"
    ".size longjmp, .-longjmp\n");
#endif

#endif /* MG_CORE_ELF */
