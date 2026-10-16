/*
 * A direct system call, as some code makes one past libc: getpid by its number, with `svc 0`. For the loader's flag
 * trap-svc (core/src/arm/hk_patch.c), which makes such a site a fault: the tests load this library with the trap on and look
 * at what became of the site. Nothing here is ever run.
 */
__attribute__((naked)) int t_svc_getpid(void)
{
    __asm__ volatile("push {r7}\n"
                     "movw r7, #20\n"
                     "svc 0\n"
                     "pop {r7}\n"
                     "bx lr\n");
}
