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

/* Chromium's cacheflush wrapper: start + length, flags 0, literal syscall nr. */
__attribute__((naked)) int t_svc_cacheflush(void *start, unsigned length)
{
    __asm__ volatile("add r1, r0\nmovs r2, #0\npush {r7}\nldr r7, 1f\nsvc 0\npop {r7}\nbx lr\n.balign 4\n1: .word 0xf0002\n");
}
