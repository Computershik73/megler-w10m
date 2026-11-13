/*
 * The kernel's user helpers (Linux's "kuser helpers"): a page of code the ARM kernel maps at 0xffff0000 into every process,
 * which old ARM code calls for what ARMv5 had no instructions for -- __kuser_cmpxchg64 at 0xffff0f60, __kuser_memory_barrier
 * at 0xffff0fa0, __kuser_cmpxchg at 0xffff0fc0, __kuser_get_tls at 0xffff0fe0 -- and whose version it reads at 0xffff0ffc.
 * On Windows the kernel is there: a call faults on executing the helper's address, and the fault is the call; a read of the
 * version faults on reading it, and the instruction that read it is done here. The Weather Channel's libsqliteJni.so called
 * the barrier (package 501).
 */
#ifndef MG_KUSER_H
#define MG_KUSER_H

#include "../mg_base.h"

/* What the version word says: every helper above is here (cmpxchg64 needs 5). */
#define MG_KUSER_VERSION 5u

/*
 * A call of the helper at `pc`, done: the registers as the helper leaves them (r0 its result; for the two cmpxchg, C set
 * when the value was swapped), and where to go on (the caller's lr). `tls` is what __kuser_get_tls returns. False when `pc`
 * is not one of the helpers.
 */
bool mg_kuser_call(mg_addr pc, u32 *regs, u32 *cpsr, u32 tls, mg_addr *resume);

/*
 * A load of the version word at `address`, done: `insn` is the instruction that faulted (Thumb when `thumb`), which is
 * decoded -- LDR with an immediate offset or a register, 16 or 32 bits wide, or ARM's -- its register given the version and
 * its base written back as the instruction says. Where to go on, the Thumb bit set as the code was. False when the
 * instruction is not such a load or does not read `address`.
 */
bool mg_kuser_read(mg_addr address, const u8 *insn, bool thumb, u32 *regs, mg_addr *next);

/*
 * SWP and SWPB (`swp Rt, Rt2, [Rn]`, ARM state only), with which code exchanged a register and memory in one step before
 * ARMv6 had LDREX and STREX. An ARMv7 processor of several cores runs them only where the system turns them on, an ARMv8
 * one not at all: the instruction is undefined there, and Linux does it in the kernel (Android's init asks for that:
 * abi.swp), which Windows does not. `insn` is the instruction the processor refused: when it is one of the two it is done
 * here, the exchange in one step for every other thread -- or left undone where its condition does not hold with the flags
 * of `cpsr`, as the processor leaves an instruction whose condition fails -- and the thread goes on after it. False for any
 * other instruction, for one with pc among its registers, and for SWP of a word that is not aligned, which Linux refuses
 * too. Boost's spinlock for ARM (spinlock_gcc_arm.hpp, which every shared_ptr's count is kept under) locks so: Plague Inc
 * ended at its first exception_ptr (0.1.0.695).
 */
bool mg_kuser_swp(const u8 *insn, u32 *regs, u32 cpsr);

#endif
