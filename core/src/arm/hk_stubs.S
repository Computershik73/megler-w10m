@ The handful of routines that cannot be written in C.
@
@ They are not linked into the core. tools/gen-stubs.sh assembles this file and
@ writes its bytes into hk_stubs_blob.h; at startup hk_stubs.c copies them into
@ executable memory and fills in the literal slots (hk_slot_*) with the
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
    .global hk_stub_setjmp
    .thumb_func
hk_stub_setjmp:
    mov     ip, sp
    stmia   r0!, {r4-r11, ip, lr}
    vstmia  r0, {d8-d15}
    movs    r0, #0
    bx      lr

@ void longjmp(jmp_buf env, int value)
    .align 2
    .global hk_stub_longjmp
    .thumb_func
hk_stub_longjmp:
    ldmia   r0!, {r4-r11, ip, lr}
    vldmia  r0, {d8-d15}
    mov     sp, ip
    movs    r0, r1
    it      eq
    moveq   r0, #1
    bx      lr

@ __aeabi_uldivmod: r0:r1 / r2:r3 -> quotient in r0:r1, remainder in r2:r3.
@ Calls u64 hk_aeabi_uldivmod(u64 n, u64 d, u64 *rem).
    .align 2
    .global hk_stub_uldivmod
    .thumb_func
hk_stub_uldivmod:
    push    {r4, lr}
    sub     sp, sp, #16
    add     r4, sp, #8
    str     r4, [sp]
    ldr     r4, hk_slot_uldivmod
    blx     r4
    ldrd    r2, r3, [sp, #8]
    add     sp, sp, #16
    pop     {r4, pc}
    .align 2
    .global hk_slot_uldivmod
hk_slot_uldivmod:
    .word   0

@ __aeabi_ldivmod, the same over i64 hk_aeabi_ldivmod(i64 n, i64 d, i64 *rem).
    .align 2
    .global hk_stub_ldivmod
    .thumb_func
hk_stub_ldivmod:
    push    {r4, lr}
    sub     sp, sp, #16
    add     r4, sp, #8
    str     r4, [sp]
    ldr     r4, hk_slot_ldivmod
    blx     r4
    ldrd    r2, r3, [sp, #8]
    add     sp, sp, #16
    pop     {r4, pc}
    .align 2
    .global hk_slot_ldivmod
hk_slot_ldivmod:
    .word   0

@ Device probes (tools/probe, and the app's self-test).
@
@ hk_stub_arm_spin(u32 count): `count` iterations of a loop in ARM (not Thumb)
@ state, then the value 0x600DC0DE. Entered and left through interworking
@ branches. On a host that cannot keep a thread in ARM state it does not come
@ back, or comes back with something else.
    .align 2
    .global hk_stub_arm_spin
    .thumb_func
hk_stub_arm_spin:
    bx      pc
    nop
    .arm
1:  subs    r0, r0, #1
    bne     1b
    ldr     r0, 2f
    bx      lr
2:  .word   0x600DC0DE
    .thumb

@ hk_stub_arm_once(): a single ARM-state instruction and back, as a rewritten
@ PLT entry is (arm/hk_patch.c): returns its argument plus one.
    .align 2
    .global hk_stub_arm_once
    .thumb_func
hk_stub_arm_once:
    adds    r0, r0, #1
    adr     r3, hk_stub_arm_once_entry
    bx      r3
    .align 2
    .global hk_stub_arm_once_entry
hk_stub_arm_once_entry:
    .arm
    ldr     pc, [pc, #-4]
    .global hk_slot_arm_once
hk_slot_arm_once:
    .word   0
    .thumb

@ Where hk_slot_arm_once points: back in Thumb state, just return.
    .align 2
    .global hk_stub_arm_once_landing
    .thumb_func
hk_stub_arm_once_landing:
    bx      lr

@ hk_stub_read_tpidruro / hk_stub_read_tpidrurw: the two user-readable thread
@ registers, for the report.
    .align 2
    .global hk_stub_read_tpidruro
    .thumb_func
hk_stub_read_tpidruro:
    mrc     p15, 0, r0, c13, c0, 3
    bx      lr

    .align 2
    .global hk_stub_read_tpidrurw
    .thumb_func
hk_stub_read_tpidrurw:
    mrc     p15, 0, r0, c13, c0, 2
    bx      lr

@ hk_stub_udf(): an instruction that is undefined on purpose, then a return.
@ The probe uses it to check that a fault in generated code reaches the handler
@ and can be resumed from, before relying on that for anything else.
    .align 2
    .global hk_stub_udf
    .thumb_func
hk_stub_udf:
    udf     #0
    bx      lr

@ hk_stub_arm_udf(): an undefined instruction taken in ARM state. The probe's
@ handler notes what the fault looked like and resumes at the next instruction,
@ still in ARM state; the value 0x600DC0DE comes back if that worked.
    .align 2
    .global hk_stub_arm_udf
    .thumb_func
hk_stub_arm_udf:
    bx      pc
    nop
    .arm
    .global hk_stub_arm_udf_at
hk_stub_arm_udf_at:
    @ (Not #16 or its neighbours: those are the breakpoints Linux hands to debuggers as SIGTRAP.)
    udf     #0x1234
    ldr     r0, 3f
    bx      lr
3:  .word   0x600DC0DE
    .thumb

@ hk_stub_arm_count(volatile u32 *words): in ARM state, adds one to words[0]
@ over and over until words[1] is not zero, then returns words[0]. For the test
@ of a thread stopped and resumed in ARM state (hk_thread_suspend, which the
@ collector uses): each turn depends on the one before, so a thread that came
@ back in Thumb state could not go on counting.
    .align 2
    .global hk_stub_arm_count
    .thumb_func
hk_stub_arm_count:
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

    .align 2
    .global hk_stub_end
hk_stub_end:
