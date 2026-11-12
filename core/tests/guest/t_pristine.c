/* The ARM syscall and PLT call of a library whose instructions must stay as they are. */
extern int getpid(void);

/* Match DroidGuard's two ELF exports. The fixture has a name of its own. */
void Java_com_google_ccc_abuse_droidguard_DroidGuard_initNative(void) {}
void Java_com_google_ccc_abuse_droidguard_DroidGuard_closeNative(void) {}

__attribute__((naked)) int t_pristine_pid(void)
{
    __asm__("push {r7, lr}\n"
            "mov r7, #20\n"
            "svc #0\n"
            "pop {r7, pc}\n");
}

int t_pristine_libc_pid(void)
{
    return getpid();
}
