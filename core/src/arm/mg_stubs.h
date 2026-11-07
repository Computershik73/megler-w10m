#ifndef MG_STUBS_H
#define MG_STUBS_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copies the stubs into executable memory and registers the ones guest code imports (setjmp, 64-bit division). False when executable memory cannot be had. */
bool mg_stubs_init(void);

enum { MG_STUB_ARM_SPIN = 1, MG_STUB_ARM_ONCE, MG_STUB_ARM_ONCE_ENTRY, MG_STUB_READ_TPIDRURO, MG_STUB_READ_TPIDRURW, MG_STUB_UDF, MG_STUB_ARM_UDF, MG_STUB_ARM_UDF_AT,
       MG_STUB_ARM_COUNT, MG_STUB_SETJMP, MG_STUB_LONGJMP, MG_STUB_ON_STACK, MG_STUB_SVC_ENTRY };
/* A probe routine's address (Thumb bit set where it is a function); NULL before mg_stubs_init, and MG_STUB_SVC_ENTRY NULL
   where the host's thread pointer could not be written into it. */
void *mg_stub(int which);
bool mg_stubs_once_target(mg_addr pc, mg_addr *target);

/*
 * The words of a thread's block for system calls (MG_TCB_SYSCALL, ld/mg_tls.c), at the top of its stack for them, which
 * mg_stub_svc_entry (mg_stubs.S) reads and writes by these offsets times four: the guest's sp at the call less the
 * piece's two words, the piece's way back, the thread's own stack (Windows: its TEB's, at each call; elsewhere what the
 * block was made with, 0 where that was not known), the lowest address of the block's stack and its reservation, the
 * TEB's three while it says the block's, the guest's flags, r0 put by, and whether the call is on the block's stack.
 */
enum {
    MG_SC_GUEST_SP, MG_SC_BACK, MG_SC_OWN_LO, MG_SC_OWN_HI, MG_SC_LIMIT, MG_SC_BASE,
    MG_SC_SAVED_BASE, MG_SC_SAVED_LIMIT, MG_SC_SAVED_DEALLOC, MG_SC_FLAGS, MG_SC_R0, MG_SC_ON_BLOCK,
    MG_SC_WORDS = 16
};
/* The call mg_stub_svc_entry makes (mg_core.c): the guest's r0..r15, r15 the piece's way back; r0's result. */
u32 mg_arm_svc_call(u32 *regs, mg_addr *block, u32 on_block);

#ifdef __cplusplus
}
#endif

#endif
