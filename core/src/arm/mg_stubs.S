@ The handful of routines that cannot be written in C.
@
@ They are not linked into the core. tools/gen-stubs.sh assembles this file and
@ writes its bytes into mg_stubs_blob.h; at startup mg_stubs.c copies them into
@ executable memory and fills in the literal slots (mg_slot_*) with the
@ addresses of the C functions they call. So the same bytes run on the phone
@ and under qemu-arm, whatever compiler built the core, and no second copy in
@ another assembler's syntax has to be kept in step.
@
@ Everything here is position independent and Thumb-2.

    .syntax unified
    .arch armv7-a
    .fpu neon
    .thumb
    .text

@ int setjmp(jmp_buf env)   (and _setjmp, sigsetjmp: the signal mask is not saved)
@ bionic's jmp_buf is at least 32 words; this uses 26: r4-r11, sp, lr, d8-d15.
    .align 2
    .global mg_stub_setjmp
    .thumb_func
mg_stub_setjmp:
    mov     ip, sp
    stmia   r0!, {r4-r11, ip, lr}
    vstmia  r0, {d8-d15}
    movs    r0, #0
    bx      lr

@ void longjmp(jmp_buf env, int value)
    .align 2
    .global mg_stub_longjmp
    .thumb_func
mg_stub_longjmp:
    ldmia   r0!, {r4-r11, ip, lr}
    vldmia  r0, {d8-d15}
    mov     sp, ip
    movs    r0, r1
    it      eq
    moveq   r0, #1
    bx      lr

@ __aeabi_uldivmod: r0:r1 / r2:r3 -> quotient in r0:r1, remainder in r2:r3.
@ Calls u64 mg_aeabi_uldivmod(u64 n, u64 d, u64 *rem).
    .align 2
    .global mg_stub_uldivmod
    .thumb_func
mg_stub_uldivmod:
    push    {r4, lr}
    sub     sp, sp, #16
    add     r4, sp, #8
    str     r4, [sp]
    ldr     r4, mg_slot_uldivmod
    blx     r4
    ldrd    r2, r3, [sp, #8]
    add     sp, sp, #16
    pop     {r4, pc}
    .align 2
    .global mg_slot_uldivmod
mg_slot_uldivmod:
    .word   0

@ __aeabi_ldivmod, the same over i64 mg_aeabi_ldivmod(i64 n, i64 d, i64 *rem).
    .align 2
    .global mg_stub_ldivmod
    .thumb_func
mg_stub_ldivmod:
    push    {r4, lr}
    sub     sp, sp, #16
    add     r4, sp, #8
    str     r4, [sp]
    ldr     r4, mg_slot_ldivmod
    blx     r4
    ldrd    r2, r3, [sp, #8]
    add     sp, sp, #16
    pop     {r4, pc}
    .align 2
    .global mg_slot_ldivmod
mg_slot_ldivmod:
    .word   0

@ Device probes (tools/probe, and the app's self-test).
@
@ mg_stub_arm_spin(u32 count): `count` iterations of a loop in ARM (not Thumb)
@ state, then the value 0x600DC0DE. Entered and left through interworking
@ branches. On a host that cannot keep a thread in ARM state it does not come
@ back, or comes back with something else.
    .align 2
    .global mg_stub_arm_spin
    .thumb_func
mg_stub_arm_spin:
    bx      pc
    nop
    .arm
1:  subs    r0, r0, #1
    bne     1b
    ldr     r0, 2f
    bx      lr
2:  .word   0x600DC0DE
    .thumb

@ mg_stub_arm_once(): a single ARM-state instruction and back, as a rewritten
@ PLT entry is (arm/mg_patch.c): returns its argument plus one.
    .align 2
    .global mg_stub_arm_once
    .thumb_func
mg_stub_arm_once:
    adds    r0, r0, #1
    adr     r3, mg_stub_arm_once_entry
    bx      r3
    .align 2
    .global mg_stub_arm_once_entry
mg_stub_arm_once_entry:
    .arm
    ldr     pc, [pc, #-4]
    .global mg_slot_arm_once
mg_slot_arm_once:
    .word   0
    .thumb

@ Where mg_slot_arm_once points: back in Thumb state, just return.
    .align 2
    .global mg_stub_arm_once_landing
    .thumb_func
mg_stub_arm_once_landing:
    bx      lr

@ mg_stub_read_tpidruro / mg_stub_read_tpidrurw: the two user-readable thread
@ registers, for the report.
    .align 2
    .global mg_stub_read_tpidruro
    .thumb_func
mg_stub_read_tpidruro:
    mrc     p15, 0, r0, c13, c0, 3
    bx      lr

    .align 2
    .global mg_stub_read_tpidrurw
    .thumb_func
mg_stub_read_tpidrurw:
    mrc     p15, 0, r0, c13, c0, 2
    bx      lr

@ mg_stub_udf(): an instruction that is undefined on purpose, then a return.
@ The probe uses it to check that a fault in generated code reaches the handler
@ and can be resumed from, before relying on that for anything else.
    .align 2
    .global mg_stub_udf
    .thumb_func
mg_stub_udf:
    udf     #0
    bx      lr

@ mg_stub_arm_udf(): an undefined instruction taken in ARM state. The probe's
@ handler notes what the fault looked like and resumes at the next instruction,
@ still in ARM state; the value 0x600DC0DE comes back if that worked.
    .align 2
    .global mg_stub_arm_udf
    .thumb_func
mg_stub_arm_udf:
    bx      pc
    nop
    .arm
    .global mg_stub_arm_udf_at
mg_stub_arm_udf_at:
    @ (Not #16 or its neighbours: those are the breakpoints Linux hands to debuggers as SIGTRAP.)
    udf     #0x1234
    ldr     r0, 3f
    bx      lr
3:  .word   0x600DC0DE
    .thumb

@ mg_stub_arm_count(volatile u32 *words): in ARM state, adds one to words[0]
@ over and over until words[1] is not zero, then returns words[0]. For the test
@ of a thread stopped and resumed in ARM state (mg_thread_suspend, which the
@ collector uses): each turn depends on the one before, so a thread that came
@ back in Thumb state could not go on counting.
    .align 2
    .global mg_stub_arm_count
    .thumb_func
mg_stub_arm_count:
    bx      pc
    nop
    .arm
4:  ldr     r1, [r0]
    add     r1, r1, #1
    str     r1, [r0]
    ldr     r2, [r0, #4]
    cmp     r2, #0
    beq     4b
    mov     r0, r1
    bx      lr
    .thumb

@ void on_stack(void *arg, void (*fn)(void *), void *top)
@ Calls fn(arg) with the stack pointer at top (8-byte aligned), and returns on
@ the stack it was called on, kept in r4 meanwhile. For Java called from native
@ code when the thread's own stack has run out (mg_dvm_invoke): the calls go on
@ in a stack of their own.
    .align 2
    .global mg_stub_on_stack
    .thumb_func
mg_stub_on_stack:
    push    {r4, lr}
    mov     r4, sp
    mov     sp, r2
    blx     r1
    mov     sp, r4
    pop     {r4, pc}

@ mg_stub_svc_entry: the way out of an ARM `svc 0` of a library's own, which the loader leads out of its code
@ (arm/mg_patch.c, a library of Go's), from the site's piece of island:
@
@     push {r12, lr}; ldr r12, =mg_stub_svc_entry+1; blx r12; b 1f; udf (a trap); 1: pop {r12, lr}; b <site + 4>
@
@ lr the piece's way back, sp the guest's less the piece's two words, every other register, the flags and the VFP
@ registers as at the svc. The call is made by mg_arm_svc_call(regs, block, on_block) (mg_slot_svc_call: mg_core.c) on a
@ stack with room for it, and the piece goes on with r0 its result (-errno on failure) and nothing else changed.
@
@ The block is the thread's (ld/mg_tls.c: slot 15 of the guest's thread pointer, MG_TCB_SYSCALL), its words (MG_SC_*,
@ mg_stubs.h, by their offsets here) at the top of a stack of its own for system calls. On the thread's own stack the
@ call is made below the guest's sp, as the trap's handler made it. On any other -- a goroutine's, of two to eight
@ kilobytes, below which Windows laid the trap's records and the handler its 5.6 KB frame -- it is made on the block's,
@ the TEB told so while it is there (mg_slot_svc_teb: Windows), as a fiber's switch tells it. A thread without a block
@ goes back to the piece's trap, which the fault handler answers as before.
    .align 2
    .global mg_stub_svc_entry
    .thumb_func
mg_stub_svc_entry:
    push    {r0, r1}
    mrs     r0, apsr                        @ the guest's flags, before anything here sets them
    .global mg_stub_svc_tp
mg_stub_svc_tp:
    mrc     p15, 0, r1, c13, c0, 2          @ the host's thread register and the guest's thread pointer's offset from it:
    ldr.w   r1, [r1, #0]                    @ (both written in at the start as the host keeps them, mg_stubs.c)
    cbz     r1, 9f
    ldr     r1, [r1, #60]                   @ its slot MG_TCB_SYSCALL: the block
    cbnz    r1, 8f
9:  msr     APSR_nzcvqg, r0                 @ no block: the piece's trap, everything as at the svc
    pop     {r0, r1}
    add.w   lr, lr, #4
    bx      lr
8:  str     r0, [r1, #36]                   @ MG_SC_FLAGS
    str     lr, [r1, #4]                    @ MG_SC_BACK
    mov     r12, r1
    pop     {r0, r1}
    mov     lr, sp
    str     lr, [r12, #0]                   @ MG_SC_GUEST_SP: the guest's, less the piece's two words
    str     r0, [r12, #40]                  @ MG_SC_R0: r0 put by, for a register to work with
    @ Which stack. The block's already (nothing under the call runs the guest's code: not expected): it stays there.
    ldr     r0, [r12, #20]                  @ MG_SC_BASE
    cmp     lr, r0
    blo     1f
    cmp     lr, r12
    blo     2f
1:  ldr     r0, mg_slot_svc_teb             @ Windows: the thread's own stack is the one its TEB says
    cbz     r0, 3f
    mrc     p15, 0, r0, c13, c0, 2
    ldr.w   r0, [r0, #0xe0c]                @ DeallocationStack
    str     r0, [r12, #8]                   @ MG_SC_OWN_LO
    mrc     p15, 0, r0, c13, c0, 2
    ldr     r0, [r0, #4]                    @ NtTib.StackBase
    str     r0, [r12, #12]                  @ MG_SC_OWN_HI
3:  ldr     r0, [r12, #8]
    cmp     lr, r0
    blo     4f
    ldr     r0, [r12, #12]
    cmp     lr, r0
    bhs     4f
2:  movs    r0, #0                          @ the thread's own: the call below the guest's sp
    b       5f
4:  mov     sp, r12                         @ another (a goroutine's): the block's, below its words
    ldr     r0, mg_slot_svc_teb
    cbz     r0, 6f
    push    {r1}
    mrc     p15, 0, r0, c13, c0, 2          @ the TEB's three of its stack kept in the block, the block's in their place
    ldr     r1, [r0, #4]
    str     r1, [r12, #24]                  @ MG_SC_SAVED_BASE
    ldr     r1, [r0, #8]
    str     r1, [r12, #28]                  @ MG_SC_SAVED_LIMIT
    ldr.w   r1, [r0, #0xe0c]
    str     r1, [r12, #32]                  @ MG_SC_SAVED_DEALLOC
    str     r12, [r0, #4]                   @ StackBase: the block
    ldr     r1, [r12, #16]                  @ MG_SC_LIMIT
    str     r1, [r0, #8]                    @ StackLimit
    ldr     r1, [r12, #20]
    str.w   r1, [r0, #0xe0c]                @ DeallocationStack: its reservation
    pop     {r1}
6:  movs    r0, #1
5:  str     r0, [r12, #44]                  @ MG_SC_ON_BLOCK
    ldr     r0, [r12, #40]
    @ The guest's registers r0..r15 as an array -- sp the one at the svc, pc the piece's way back -- which the call reads,
    @ and which the collector finds on the thread's own stack as it found the trap's.
    sub     sp, sp, #64
    stm     sp, {r0-r11}
    mov     r4, sp
    mov     r5, r12
    ldr     r0, [r5, #0]
    ldr     r1, [r0]                        @ r12 and lr, where the piece put them
    ldr     r2, [r0, #4]
    adds    r0, r0, #8
    str     r1, [r4, #48]
    str     r0, [r4, #52]
    str     r2, [r4, #56]
    ldr     r0, [r5, #4]
    str     r0, [r4, #60]
    @ The VFP registers a call may change and the status register, kept; the stack 8-aligned for the call.
    vmrs    r0, fpscr
    mov     r1, sp
    bic     r1, r1, #7
    mov     sp, r1
    push    {r0, r1}
    vpush   {d0-d7}
    vpush   {d16-d31}
    mov     r0, r4
    mov     r1, r5
    ldr     r2, [r5, #44]
    ldr     r3, mg_slot_svc_call
    blx     r3
    vpop    {d16-d31}
    vpop    {d0-d7}
    pop     {r1, r2}
    vmsr    fpscr, r1
    str     r0, [r4]                        @ the result, r0's
    ldr     r0, [r5, #44]
    cbz     r0, 7f
    ldr     r0, mg_slot_svc_teb
    cbz     r0, 7f
    mrc     p15, 0, r0, c13, c0, 2          @ the TEB's own three back
    ldr     r1, [r5, #24]
    str     r1, [r0, #4]
    ldr     r1, [r5, #28]
    str     r1, [r0, #8]
    ldr     r1, [r5, #32]
    str.w   r1, [r0, #0xe0c]
7:  @ Back to the piece: the guest's registers, flags and sp (r12 and lr the piece takes back itself).
    ldr     lr, [r5, #4]
    ldr     r12, [r5, #0]
    ldr     r0, [r5, #36]
    msr     APSR_nzcvqg, r0
    ldm     r4, {r0-r11}
    mov     sp, r12
    bx      lr
    .align 2
    .global mg_slot_svc_call
mg_slot_svc_call:
    .word   0
    .global mg_slot_svc_teb
mg_slot_svc_teb:
    .word   0

    .align 2
    .global mg_stub_end
mg_stub_end:
