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
