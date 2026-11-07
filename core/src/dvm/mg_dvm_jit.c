/*
 * The JIT: a method's bytecode compiled to Thumb-2 once its loops are hot (docs/JIT.md).
 *
 * The code works on the frame's own registers, in memory, as the interpreter does: r4 holds them throughout, every
 * instruction loads what it reads from them and stores what it writes, so that at any instruction the code can stop and
 * hand the frame back -- "go on at this pc" -- and the interpreter goes on with nothing to rebuild. Whatever the compiler
 * does not do (exceptions, the rarer instructions) is such a hand-back, an exit, and so
 * is anything that would throw: a null or an index out of bounds sends the frame back to the instruction that throws, and
 * the interpreter throws it. A call is made through mg_dvm_jit_invoke (mg_dvm_interp.c), which does it as the interpreter
 * would and keeps its result for the move-result after it; one that throws sends the frame back with the exception pending.
 * No safepoints: the collector stops threads from outside and reads their registers and stacks conservatively, and moves
 * nothing (mg_dvm_gc.c).
 *
 * Entered from the interpreter's plain loop at a jump back (mg_dvm_jit_back), at the head of a loop compiled whole, and at a
 * method's start when it is called, from the interpreter or from compiled code, if the code there does some work before it
 * hands anything back. A jump back in the code looks at g_dvm.slow first and hands the frame back at the target when
 * anything watches the instructions (the watched loop runs then). Returns are compiled: the interpreter, or the compiled
 * call, ends the call with the value the code leaves.
 *
 *   int code(mg_dvm_jit_ctx *ctx, const void *entry)
 *     r5 ctx, r4 ctx->regs, r7 ctx->slow; returns 0 with ctx->pc where the interpreter goes on, 1 with ctx->pc where the
 *     exception pending was thrown, 2 with the method's value in ctx->result.
 *
 * Only where the host runs Thumb-2: the phone, and the ARM tests under qemu-arm. Elsewhere nothing is compiled.
 */
#include "mg_dvm_internal.h"
#include "../mg_core.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 g_dvm_jit_threshold;
u32 g_dvm_jit_in_place;
static bool g_jit_trace;
static volatile i32 g_jit_methods, g_jit_refused;
static u64 g_code_bytes, g_page_bytes;  /* the code compiled, and the pages committed for it (under g_jit_lock) */

#if defined(__arm__) || defined(_M_ARM)

/* An array's elements and length are where they are whichever kind it is: one load each in the code. */
typedef char jit_array_data_same[offsetof(mg_jobj, u.arr.data) == offsetof(mg_jobj, u.oarr.v) ? 1 : -1];
typedef char jit_array_len_same[offsetof(mg_jobj, u.arr.len) == offsetof(mg_jobj, u.oarr.len) ? 1 : -1];
typedef char jit_register_word[sizeof(mg_dvm_reg) == 4 ? 1 : -1];
/* Where the code finds what mg_dvm_jit_ctx holds. */
enum { JIT_CTX_REGS = 0, JIT_CTX_SLOW = 4, JIT_CTX_PC = 8, JIT_CTX_T = 12, JIT_CTX_F = 16, JIT_CTX_RESULT = 24 };
typedef char jit_ctx_layout[offsetof(mg_dvm_jit_ctx, regs) == JIT_CTX_REGS && offsetof(mg_dvm_jit_ctx, slow) == JIT_CTX_SLOW &&
                                    offsetof(mg_dvm_jit_ctx, pc) == JIT_CTX_PC && offsetof(mg_dvm_jit_ctx, t) == JIT_CTX_T &&
                                    offsetof(mg_dvm_jit_ctx, f) == JIT_CTX_F && offsetof(mg_dvm_jit_ctx, result) == JIT_CTX_RESULT
                                ? 1
                                : -1];

void mg_dvm_jit_start(bool on, u32 threshold, bool trace)
{
    g_dvm_jit_threshold = on ? (threshold ? threshold : 1) : 0;
    g_jit_trace = trace;
    mg_info("jit: %s", on ? "methods whose loops are hot are compiled to Thumb-2 (docs/JIT.md)" : "off: everything is interpreted");
}

/* ------------------------------------------------------------------ the emitter */

typedef struct fixup {
    u32 at;                            /* the branch's first halfword */
    u32 pc;                            /* what it goes to: a pc's code, its exit or its jump back */
    u8 kind, cond;                     /* FIX_*; the condition (14: always) */
} fixup;

/*
 * To a pc's code, its exit, its jump back's check, the way out; for a call that threw, its pc's throw and the way out with
 * it; and the way out of a return.
 */
enum { FIX_CODE, FIX_EXIT, FIX_BACK, FIX_EPILOGUE, FIX_THROW, FIX_THROW_EPILOGUE, FIX_RETURN };
enum { C_EQ = 0, C_NE = 1, C_HS = 2, C_LO = 3, C_MI = 4, C_PL = 5, C_HI = 8, C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13, C_AL = 14 };

typedef struct emitter {
    u16 *b;
    u32 n, cap;
    fixup *fx;
    u32 nfx, capfx;
    u32 in_place;                      /* calls of trivial methods done in the code (inline_call) */
    bool failed;
} emitter;

static void e16(emitter *e, u32 h)
{
    if (e->n == e->cap) {
        u32 cap = e->cap ? e->cap * 2 : 1024;
        u16 *b = (u16 *)realloc(e->b, sizeof(u16) * cap);
        if (!b) { e->failed = true; return; }
        e->b = b;
        e->cap = cap;
    }
    e->b[e->n++] = (u16)h;
}
static void e32(emitter *e, u32 hi, u32 lo) { e16(e, hi); e16(e, lo); }

static void fix(emitter *e, u8 kind, u32 pc, u8 cond)
{
    if (e->nfx == e->capfx) {
        u32 cap = e->capfx ? e->capfx * 2 : 256;
        fixup *f = (fixup *)realloc(e->fx, sizeof(fixup) * cap);
        if (!f) { e->failed = true; return; }
        e->fx = f;
        e->capfx = cap;
    }
    e->fx[e->nfx].at = e->n;
    e->fx[e->nfx].pc = pc;
    e->fx[e->nfx].kind = kind;
    e->fx[e->nfx].cond = cond;
    e->nfx++;
    e32(e, 0, 0);                      /* the branch, written once its target is known */
}

/* A branch at halfword `at` to halfword `to`: B<cond>.W (T3, +-1 MB) or B.W (T4). False when it does not reach. */
static bool put_branch(u16 *b, u32 at, u32 to, u8 cond)
{
    i32 off = ((i32)to - (i32)at) * 2 - 4;
    u32 s, imm11 = ((u32)off >> 1) & 0x7FF;
    if (cond == C_AL) {
        u32 imm10 = ((u32)off >> 12) & 0x3FF, i1, i2, j1, j2;
        if (off < -(1 << 24) || off >= (1 << 24)) return false;
        s = ((u32)off >> 24) & 1;
        i1 = ((u32)off >> 23) & 1;
        i2 = ((u32)off >> 22) & 1;
        j1 = (~(i1 ^ s)) & 1;
        j2 = (~(i2 ^ s)) & 1;
        b[at] = (u16)(0xF000 | (s << 10) | imm10);
        b[at + 1] = (u16)(0x9000 | (j1 << 13) | (j2 << 11) | imm11);
    } else {
        u32 imm6 = ((u32)off >> 12) & 0x3F, j1 = ((u32)off >> 18) & 1, j2 = ((u32)off >> 19) & 1;
        if (off < -(1 << 20) || off >= (1 << 20)) return false;
        s = ((u32)off >> 20) & 1;
        b[at] = (u16)(0xF000 | (s << 10) | ((u32)cond << 6) | imm6);
        b[at + 1] = (u16)(0x8000 | (j1 << 13) | (j2 << 11) | imm11);
    }
    return true;
}

/* Registers: r0 to r3 and r12 for values, r4 the frame's registers, r5 the context, r7 the slow flag. */
enum { R0, R1, R2, R3, R4, R5, R6, R7, R12 = 12 };

/* A branch inside one instruction's code: its place kept, and written with its condition once its label is reached (local_to). */
static u32 local_branch(emitter *e)
{
    u32 at = e->n;
    e32(e, 0, 0);
    return at;
}
static void local_to(emitter *e, u32 at, u8 cond)
{
    if (!e->failed && !put_branch(e->b, at, e->n, cond)) e->failed = true;
}

static void ldr_v(emitter *e, int rt, u32 v) { e32(e, 0xF8D0 | R4, ((u32)rt << 12) | (v * 4)); }
static void str_v(emitter *e, int rt, u32 v) { e32(e, 0xF8C0 | R4, ((u32)rt << 12) | (v * 4)); }
static void ldr_imm(emitter *e, int rt, int rn, u32 off) { e32(e, 0xF8D0 | (u32)rn, ((u32)rt << 12) | off); }
static void str_imm(emitter *e, int rt, int rn, u32 off) { e32(e, 0xF8C0 | (u32)rn, ((u32)rt << 12) | off); }
static void ldrb_imm(emitter *e, int rt, int rn, u32 off) { e32(e, 0xF890 | (u32)rn, ((u32)rt << 12) | off); }

static void mov_imm(emitter *e, int rd, u32 v)
{
    u32 lo = v & 0xFFFF, hi = v >> 16;
    e32(e, 0xF240 | (((lo >> 11) & 1) << 10) | (lo >> 12), (((lo >> 8) & 7) << 12) | ((u32)rd << 8) | (lo & 0xFF));
    if (hi) e32(e, 0xF2C0 | (((hi >> 11) & 1) << 10) | (hi >> 12), (((hi >> 8) & 7) << 12) | ((u32)rd << 8) | (hi & 0xFF));
}

static void cmp_reg(emitter *e, int rn, int rm) { e16(e, 0x4280 | ((u32)rm << 3) | (u32)rn); }
static void cmp_imm(emitter *e, int rn, u32 imm8) { e16(e, 0x2800 | ((u32)rn << 8) | imm8); }

/* The three-register data processing of int arithmetic: rd = rn op rm. */
enum { OP_ADD, OP_SUB, OP_RSB, OP_MUL, OP_DIV, OP_REM, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR };

static void data_op(emitter *e, int op, int rd, int rn, int rm)
{
    switch (op) {
    case OP_ADD: e32(e, 0xEB00 | (u32)rn, ((u32)rd << 8) | (u32)rm); break;
    case OP_SUB: e32(e, 0xEBA0 | (u32)rn, ((u32)rd << 8) | (u32)rm); break;
    case OP_RSB: e32(e, 0xEBC0 | (u32)rn, ((u32)rd << 8) | (u32)rm); break;
    case OP_MUL: e32(e, 0xFB00 | (u32)rn, 0xF000 | ((u32)rd << 8) | (u32)rm); break;
    case OP_DIV: e32(e, 0xFB90 | (u32)rn, 0xF0F0 | ((u32)rd << 8) | (u32)rm); break;
    case OP_REM:
        /* rd = rn - (rn / rm) * rm, the quotient in r3 */
        e32(e, 0xFB90 | (u32)rn, 0xF0F0 | (R3 << 8) | (u32)rm);
        e32(e, 0xFB00 | R3, ((u32)rn << 12) | ((u32)rd << 8) | 0x10 | (u32)rm);
        break;
    case OP_AND: e32(e, 0xEA00 | (u32)rn, ((u32)rd << 8) | (u32)rm); break;
    case OP_OR: e32(e, 0xEA40 | (u32)rn, ((u32)rd << 8) | (u32)rm); break;
    case OP_XOR: e32(e, 0xEA80 | (u32)rn, ((u32)rd << 8) | (u32)rm); break;
    case OP_SHL:
    case OP_SHR:
    case OP_USHR:
        /* Java shifts by the low five bits of the count; ARM by its low byte. */
        e32(e, 0xF000 | (u32)rm, (R3 << 8) | 0x1F);
        e32(e, (op == OP_SHL ? 0xFA00 : op == OP_USHR ? 0xFA20 : 0xFA40) | (u32)rn, 0xF000 | ((u32)rd << 8) | R3);
        break;
    }
}

/* ------------------------------------------------------------------ the compiler */

static int bin_op(u32 code)
{
    static const signed char ops[] = { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_REM, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR };
    return code < 11 ? ops[code] : -1;
}
static int lit16_op(u32 code)
{
    static const signed char ops[] = { OP_ADD, OP_RSB, OP_MUL, OP_DIV, OP_REM, OP_AND, OP_OR, OP_XOR };
    return code < 8 ? ops[code] : -1;
}
static int lit8_op(u32 code)
{
    static const signed char ops[] = { OP_ADD, OP_RSB, OP_MUL, OP_DIV, OP_REM, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR };
    return code < 11 ? ops[code] : -1;
}
static u8 if_cond(u32 op)
{
    static const u8 conds[] = { C_EQ, C_NE, C_LT, C_GE, C_GT, C_LE };
    return conds[op];
}

/* A branch to pc `to` from the instruction at `pc`: a jump back goes by its target's jump-back stub (the slow flag). */
static void branch_to(emitter *e, u32 pc, u32 to, u8 cond) { fix(e, to <= pc ? FIX_BACK : FIX_CODE, to, cond); }

/* rd = vB op rm, the divisor checked first: zero is an exit (the interpreter throws ArithmeticException). */
static void arith(emitter *e, u32 pc, int op, u32 vd, int rn, int rm)
{
    if (op == OP_DIV || op == OP_REM) {
        cmp_imm(e, rm, 0);
        fix(e, FIX_EXIT, pc, C_EQ);
    }
    data_op(e, op, R0, rn, rm);
    str_v(e, R0, vd);
}

/*
 * An array and its index checked as the interpreter checks them (null, the kind and element size it is, the bounds), any
 * failure an exit; then r3 the element's address base, r1 the index. `size` 0 for an array of references.
 */
static void array_checks(emitter *e, u32 pc, u32 va, u32 vi, u32 size)
{
    ldr_v(e, R0, va);
    cmp_imm(e, R0, 0);
    fix(e, FIX_EXIT, pc, C_EQ);
    ldrb_imm(e, R1, R0, (u32)offsetof(mg_jobj, kind));
    cmp_imm(e, R1, size ? MG_J_PRIM_ARRAY : MG_J_OBJ_ARRAY);
    fix(e, FIX_EXIT, pc, C_NE);
    if (size) {
        ldrb_imm(e, R1, R0, (u32)offsetof(mg_jobj, u.arr.esz));
        cmp_imm(e, R1, size);
        fix(e, FIX_EXIT, pc, C_NE);
    }
    ldr_v(e, R1, vi);
    ldr_imm(e, R2, R0, (u32)offsetof(mg_jobj, u.arr.len));
    cmp_reg(e, R1, R2);
    fix(e, FIX_EXIT, pc, C_HS);
    ldr_imm(e, R3, R0, (u32)offsetof(mg_jobj, u.arr.data));
}

/* An element at r3 + r1 << shift: loaded into r2 (kind: 0 word, 1 byte, 2 signed byte, 3 halfword, 4 signed halfword) or stored from it. */
static void element(emitter *e, bool store, int kind)
{
    static const u16 loads[] = { 0xF850, 0xF810, 0xF910, 0xF830, 0xF930 }, stores[] = { 0xF840, 0xF800, 0xF800, 0xF820, 0xF820 };
    static const u8 shifts[] = { 2, 0, 0, 1, 1 };
    e32(e, (u32)(store ? stores[kind] : loads[kind]) | R3, (R2 << 12) | ((u32)shifts[kind] << 4) | R1);
}

/* A field's value at [rn + off]: loaded into rt (kind: 0 word, 1 byte, 2 signed byte, 3 halfword, 4 signed halfword) or stored from it. */
static void field_access(emitter *e, bool store, int kind, int rt, int rn, u32 off)
{
    static const u16 loads[] = { 0xF8D0, 0xF890, 0xF990, 0xF8B0, 0xF9B0 }, stores[] = { 0xF8C0, 0xF880, 0xF880, 0xF8A0, 0xF8A0 };
    e32(e, (u32)(store ? stores[kind] : loads[kind]) | (u32)rn, ((u32)rt << 12) | off);
}

/* What each of iget..iget-short and iput..iput-short moves: its kind (field_access), or 5 for a long or double (two words). */
static const u8 k_field_kind[7] = { 0, 5, 0, 1, 2, 3, 4 };

/*
 * iget and iput of a field resolved by now (the loop ran in the interpreter first): the object checked for null and for
 * having the slot (an object the runtime keeps its fields for by name does not: the interpreter's slotless_field), each
 * failure an exit; then the slot itself, as the interpreter's field_slot finds it.
 */
static bool instance_field(emitter *e, const mg_dvm_method *m, u32 pc, u32 op, u32 a4, u32 b4, u32 idx)
{
    mg_dvm_dex *d = m->cls->dex;
    mg_dvm_field *fld = idx < d->nfield ? d->fcache[idx] : NULL;
    bool store = op >= 0x59;
    u8 kind = k_field_kind[op - (store ? 0x59 : 0x52)];
    u32 off;
    if (!fld || (fld->flags & DVM_ACC_STATIC)) return false;
    off = (u32)MG_DVM_HEADER + fld->slot * (u32)sizeof(mg_jvalue);
    if (off + 4 >= 4096 || fld->slot > 0xFFFF) return false;
    ldr_v(e, R0, b4);
    cmp_imm(e, R0, 0);
    fix(e, FIX_EXIT, pc, C_EQ);
    ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
    ldr_imm(e, R1, R1, (u32)offsetof(mg_dvm_class, nslots));
    mov_imm(e, R2, fld->slot);
    cmp_reg(e, R2, R1);
    fix(e, FIX_EXIT, pc, C_HS);
    if (kind == 5) {
        if (store) {
            ldr_v(e, R2, a4);
            ldr_v(e, R3, a4 + 1);
            field_access(e, true, 0, R2, R0, off);
            field_access(e, true, 0, R3, R0, off + 4);
        } else {
            field_access(e, false, 0, R2, R0, off);
            field_access(e, false, 0, R3, R0, off + 4);
            str_v(e, R2, a4);
            str_v(e, R3, a4 + 1);
        }
    } else if (store) {
        ldr_v(e, R2, a4);
        field_access(e, true, kind, R2, R0, off);
    } else {
        field_access(e, false, kind, R2, R0, off);
        str_v(e, R2, a4);
    }
    return true;
}

/* sget and sput of a field of a class initialised by now: its slot's address is the code's own constant. */
static bool static_field(emitter *e, const mg_dvm_method *m, u32 op, u32 aa, u32 idx)
{
    mg_dvm_dex *d = m->cls->dex;
    mg_dvm_field *fld = idx < d->nfield ? d->fcache[idx] : NULL;
    bool store = op >= 0x67;
    u8 kind = k_field_kind[op - (store ? 0x67 : 0x60)];
    mg_jvalue *slot;
    if (!fld || !(fld->flags & DVM_ACC_STATIC) || fld->cls->state != DVM_CS_INITIALIZED || !fld->cls->statics) return false;
    slot = &fld->cls->statics[fld->slot];
    mov_imm(e, R0, (u32)(mg_addr)slot);
    if (kind == 5) {
        if (store) {
            ldr_v(e, R2, aa);
            ldr_v(e, R3, aa + 1);
            field_access(e, true, 0, R2, R0, 0);
            field_access(e, true, 0, R3, R0, 4);
        } else {
            field_access(e, false, 0, R2, R0, 0);
            field_access(e, false, 0, R3, R0, 4);
            str_v(e, R2, aa);
            str_v(e, R3, aa + 1);
        }
    } else if (store) {
        ldr_v(e, R2, aa);
        field_access(e, true, kind, R2, R0, 0);
    } else {
        field_access(e, false, kind, R2, R0, 0);
        str_v(e, R2, aa);
    }
    return true;
}

/* ------------------------------------------------------------------ longs */

/* The register-specified shifts, rd = rn op rm (ARM's: by the low byte of rm, 32 and more shifting everything out, or in, for ASR, the sign). */
static void lsl_r(emitter *e, int rd, int rn, int rm) { e32(e, 0xFA00 | (u32)rn, 0xF000 | ((u32)rd << 8) | (u32)rm); }
static void lsr_r(emitter *e, int rd, int rn, int rm) { e32(e, 0xFA20 | (u32)rn, 0xF000 | ((u32)rd << 8) | (u32)rm); }
static void asr_r(emitter *e, int rd, int rn, int rm) { e32(e, 0xFA40 | (u32)rn, 0xF000 | ((u32)rd << 8) | (u32)rm); }
static void orr_r(emitter *e, int rd, int rn, int rm) { e32(e, 0xEA40 | (u32)rn, ((u32)rd << 8) | (u32)rm); }
/* rd = imm - rn, rd = rn - imm (S: the flags set), for imm under 256 */
static void rsb_i(emitter *e, int rd, int rn, u32 imm) { e32(e, 0xF1C0 | (u32)rn, ((u32)rd << 8) | imm); }
static void sub_i(emitter *e, int rd, int rn, u32 imm, bool s) { e32(e, (s ? 0xF1B0 : 0xF1A0) | (u32)rn, ((u32)rd << 8) | imm); }

/*
 * A long operation of the pairs vA and vB into vD (op as numbered from add-long: add, sub, mul, div, rem, and, or, xor,
 * shl, shr, ushr; the shifts by the int vB). False for division and remainder: they stay the interpreter's.
 */
static bool long_op(emitter *e, u32 op, u32 vd, u32 va, u32 vb)
{
    if (op == 3 || op == 4) return false;
    ldr_v(e, R0, va);
    ldr_v(e, R1, va + 1);
    if (op >= 8) {
        ldr_v(e, R2, vb);
        e32(e, 0xF000 | R2, (R2 << 8) | 0x3F);                  /* and r2, r2, #63 */
        switch (op) {
        case 8:                                                  /* hi = hi << n | lo >> (32 - n) | lo << (n - 32); lo <<= n */
            lsl_r(e, R1, R1, R2);
            rsb_i(e, R3, R2, 32);
            lsr_r(e, R3, R0, R3);
            orr_r(e, R1, R1, R3);
            sub_i(e, R3, R2, 32, false);
            lsl_r(e, R3, R0, R3);
            orr_r(e, R1, R1, R3);
            lsl_r(e, R0, R0, R2);
            break;
        case 10:                                                 /* lo = lo >> n | hi << (32 - n) | hi >> (n - 32); hi >>>= n */
            lsr_r(e, R0, R0, R2);
            rsb_i(e, R3, R2, 32);
            lsl_r(e, R3, R1, R3);
            orr_r(e, R0, R0, R3);
            sub_i(e, R3, R2, 32, false);
            lsr_r(e, R3, R1, R3);
            orr_r(e, R0, R0, R3);
            lsr_r(e, R1, R1, R2);
            break;
        default:                                                 /* shr: as ushr below 32; from 32 on, lo = hi >> (n - 32) with the sign */
            lsr_r(e, R0, R0, R2);
            rsb_i(e, R3, R2, 32);
            lsl_r(e, R3, R1, R3);
            orr_r(e, R0, R0, R3);
            sub_i(e, R3, R2, 32, true);
            e16(e, 0xBF58);                                      /* it pl */
            asr_r(e, R0, R1, R3);
            asr_r(e, R1, R1, R2);
            break;
        }
    } else {
        ldr_v(e, R2, vb);
        ldr_v(e, R3, vb + 1);
        switch (op) {
        case 0:
            e32(e, 0xEB10 | R0, (R0 << 8) | R2);                 /* adds r0, r0, r2 */
            e32(e, 0xEB40 | R1, (R1 << 8) | R3);                 /* adc r1, r1, r3 */
            break;
        case 1:
            e32(e, 0xEBB0 | R0, (R0 << 8) | R2);                 /* subs r0, r0, r2 */
            e32(e, 0xEB60 | R1, (R1 << 8) | R3);                 /* sbc r1, r1, r3 */
            break;
        case 2:
            /* (hi:lo) = lo*lo' + (hi*lo' + lo*hi') << 32 */
            e32(e, 0xFB00 | R1, 0xF000 | (R1 << 8) | R2);        /* mul r1, r1, r2 */
            e32(e, 0xFB00 | R0, (R1 << 12) | (R1 << 8) | R3);    /* mla r1, r0, r3, r1 */
            e32(e, 0xFBA0 | R0, (R0 << 12) | (R12 << 8) | R2);   /* umull r0, r12, r0, r2 */
            e32(e, 0xEB00 | R1, (R1 << 8) | R12);                /* add r1, r1, r12 */
            break;
        case 5: data_op(e, OP_AND, R0, R0, R2); data_op(e, OP_AND, R1, R1, R3); break;
        case 6: data_op(e, OP_OR, R0, R0, R2); data_op(e, OP_OR, R1, R1, R3); break;
        default: data_op(e, OP_XOR, R0, R0, R2); data_op(e, OP_XOR, R1, R1, R3); break;
        }
    }
    str_v(e, R0, vd);
    str_v(e, R1, vd + 1);
    return true;
}

/* cmp-long: -1, 0 or 1 into vD, the high words compared signed, the low ones unsigned. */
static void cmp_long(emitter *e, u32 vd, u32 va, u32 vb)
{
    u32 lt, gt, lo, hi, zero_done, minus_done;
    ldr_v(e, R0, va);
    ldr_v(e, R1, va + 1);
    ldr_v(e, R2, vb);
    ldr_v(e, R3, vb + 1);
    cmp_reg(e, R1, R3);
    lt = local_branch(e);
    gt = local_branch(e);
    cmp_reg(e, R0, R2);
    lo = local_branch(e);
    hi = local_branch(e);
    mov_imm(e, R0, 0);
    zero_done = local_branch(e);
    local_to(e, lt, C_LT);
    local_to(e, lo, C_LO);
    mov_imm(e, R0, 0xFFFFFFFFu);
    minus_done = local_branch(e);
    local_to(e, gt, C_GT);
    local_to(e, hi, C_HI);
    mov_imm(e, R0, 1);
    local_to(e, zero_done, C_AL);
    local_to(e, minus_done, C_AL);
    str_v(e, R0, vd);
}

/* ------------------------------------------------------------------ switches */

/* The payload of a switch at pc + off: its size checked, NULL when it is not the kind asked for or does not lie in the method. */
static const u16 *switch_payload(const mg_dvm_method *m, u32 pc, i32 off, u16 ident, u32 *size)
{
    i64 at = (i64)pc + off;
    const u16 *p;
    u32 n, units;
    if (at < 0 || at + 2 > (i64)m->ninsns || m->insns[at] != ident) return NULL;
    p = m->insns + at;
    n = p[1];
    units = ident == 0x0100 ? 4 + n * 2 : 2 + n * 4;
    if (at + units > (i64)m->ninsns) return NULL;
    *size = n;
    return p;
}
static i32 rd32(const u16 *p) { return (i32)((u32)p[0] | (u32)p[1] << 16); }

/* packed-switch and sparse-switch: the value compared with each key (at most 64 of them), on to the next instruction for none. */
static bool compile_switch(emitter *e, const mg_dvm_method *m, u32 pc, u32 op, u32 va, i32 off)
{
    u32 n, i;
    const u16 *p = switch_payload(m, pc, off, op == 0x2b ? 0x0100 : 0x0200, &n);
    if (!p || n > 64) return false;
    ldr_v(e, R0, va);
    if (op == 0x2b) {
        i32 first = rd32(p + 2);
        mov_imm(e, R1, (u32)first);
        data_op(e, OP_SUB, R0, R0, R1);
        for (i = 0; i < n; i++) {
            i32 to = rd32(p + 4 + 2 * i);
            if (i < 256) cmp_imm(e, R0, i);
            else { mov_imm(e, R1, i); cmp_reg(e, R0, R1); }
            branch_to(e, pc, (u32)((i32)pc + to), C_EQ);
        }
    } else {
        for (i = 0; i < n; i++) {
            i32 key = rd32(p + 2 + 2 * i), to = rd32(p + 2 + 2 * n + 2 * i);
            if (key >= 0 && key < 256) cmp_imm(e, R0, (u32)key);
            else { mov_imm(e, R1, (u32)key); cmp_reg(e, R0, R1); }
            branch_to(e, pc, (u32)((i32)pc + to), C_EQ);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ floating point */

/*
 * VFP: s0 to s3 (d0, d1) hold an instruction's values, loaded from the frame's registers and stored back, nothing kept in
 * them from one instruction to the next (they are the caller's to save: a call made from the code may change them). The
 * frame's registers are within VLDR's reach: an instruction with a double names registers under 256. The arithmetic is
 * the host's, as the interpreter's C does it with the same FPSCR; a conversion to int is VCVT's round toward zero, which
 * saturates and makes NaN 0, as Java says.
 */
static void vldr_s(emitter *e, u32 s, u32 v) { e32(e, 0xED90 | ((s & 1) << 6) | R4, ((s >> 1) << 12) | 0x0A00 | v); }
static void vstr_s(emitter *e, u32 s, u32 v) { e32(e, 0xED80 | ((s & 1) << 6) | R4, ((s >> 1) << 12) | 0x0A00 | v); }
static void vldr_d(emitter *e, u32 d, u32 v) { e32(e, 0xED90 | R4, (d << 12) | 0x0B00 | v); }
static void vstr_d(emitter *e, u32 d, u32 v) { e32(e, 0xED80 | R4, (d << 12) | 0x0B00 | v); }

/* vd = va op vb, op 0 add, 1 sub, 2 mul, 3 div: s0 = s0 op s1, or d0 = d0 op d1. */
static void vfp_arith(emitter *e, u32 op, bool dbl, u32 vd, u32 va, u32 vb)
{
    static const u16 ops[] = { 0xEE30, 0xEE30, 0xEE20, 0xEE80 };
    if (dbl) {
        vldr_d(e, 0, va);
        vldr_d(e, 1, vb);
        e32(e, ops[op], (op == 1 ? 0x40u : 0u) | 0x0B01);
        vstr_d(e, 0, vd);
    } else {
        vldr_s(e, 0, va);
        vldr_s(e, 1, vb);
        e32(e, ops[op], (op == 1 ? 0x40u : 0u) | 0x0A20);
        vstr_s(e, 0, vd);
    }
}

/* cmpl and cmpg: -1, 0 or 1, and for NaN either side -1 (cmpl) or 1 (cmpg). */
static void vfp_compare(emitter *e, bool dbl, bool greater, u32 vd, u32 va, u32 vb)
{
    mov_imm(e, R0, 0);                                           /* movw: the flags left as they are */
    if (dbl) {
        vldr_d(e, 0, va);
        vldr_d(e, 1, vb);
        e32(e, 0xEEB4, 0x0B41);                                  /* vcmp.f64 d0, d1 */
    } else {
        vldr_s(e, 0, va);
        vldr_s(e, 1, vb);
        e32(e, 0xEEB4, 0x0A60);                                  /* vcmp.f32 s0, s1 */
    }
    e32(e, 0xEEF1, 0xFA10);                                      /* vmrs APSR_nzcv, fpscr */
    e16(e, 0xBFC8);                                              /* it gt */
    e16(e, 0x2001);                                              /*   movgt r0, #1 */
    e16(e, 0xBF48);                                              /* it mi */
    e32(e, 0xF06F, 0x0000);                                      /*   mvnmi r0, #0 */
    e16(e, 0xBF68);                                              /* it vs: unordered */
    if (greater) e16(e, 0x2001);                                 /*   movvs r0, #1 */
    else e32(e, 0xF06F, 0x0000);                                 /*   mvnvs r0, #0 */
    str_v(e, R0, vd);
}

/* The conversions VFP has (not those of a long): vA = (kind) vB. False for the others. */
static bool vfp_convert(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x82: vldr_s(e, 1, vb); e32(e, 0xEEB8, 0x0AE0); vstr_s(e, 0, va); return true;    /* int-to-float: vcvt.f32.s32 s0, s1 */
    case 0x83: vldr_s(e, 1, vb); e32(e, 0xEEB8, 0x0BE0); vstr_d(e, 0, va); return true;    /* int-to-double: vcvt.f64.s32 d0, s1 */
    case 0x87: vldr_s(e, 1, vb); e32(e, 0xEEBD, 0x0AE0); vstr_s(e, 0, va); return true;    /* float-to-int: vcvt.s32.f32 s0, s1 */
    case 0x89: vldr_s(e, 2, vb); e32(e, 0xEEB7, 0x0AC1); vstr_d(e, 0, va); return true;    /* float-to-double: vcvt.f64.f32 d0, s2 */
    case 0x8a: vldr_d(e, 1, vb); e32(e, 0xEEBD, 0x0BC1); vstr_s(e, 0, va); return true;    /* double-to-int: vcvt.s32.f64 s0, d1 */
    case 0x8c: vldr_d(e, 1, vb); e32(e, 0xEEB7, 0x0BC1); vstr_s(e, 0, va); return true;    /* double-to-float: vcvt.f32.f64 s0, d1 */
    case 0x7f: vldr_s(e, 0, vb); e32(e, 0xEEB1, 0x0A40); vstr_s(e, 0, va); return true;    /* neg-float: vneg.f32 s0, s0 */
    case 0x80: vldr_d(e, 0, vb); e32(e, 0xEEB1, 0x0B40); vstr_d(e, 0, va); return true;    /* neg-double: vneg.f64 d0, d0 */
    default: return false;
    }
}

/* ------------------------------------------------------------------ by calls */

/*
 * What has no instruction of its own -- a long's division and remainder, a float's and a double's remainder, a long's
 * conversions to and from them -- by a call of a small function of the runtime's, its arguments and result where the
 * hard-float procedure call standard has them (a long in r0:r1 and r2:r3, a float in s0 and s1, a double in d0 and d1),
 * with Java's answers where C's differ: MIN_VALUE / -1, a conversion of NaN or of what is out of range.
 */
static i64 jit_div_long(i64 a, i64 b) { return a == INT64_MIN && b == -1 ? a : a / b; }
static i64 jit_rem_long(i64 a, i64 b) { return a == INT64_MIN && b == -1 ? 0 : a % b; }
static float jit_rem_float(float a, float b) { return fmodf(a, b); }
static double jit_rem_double(double a, double b) { return fmod(a, b); }
static float jit_long_to_float(i64 v) { return (float)v; }
static double jit_long_to_double(i64 v) { return (double)v; }
static i64 jit_double_to_long(double v)
{
    if (v != v) return 0;
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return (i64)v;
}
static i64 jit_float_to_long(float v) { return jit_double_to_long((double)v); }

static void call_c(emitter *e, mg_addr fn)
{
    mov_imm(e, R12, (u32)fn);
    e16(e, 0x47E0);                                              /* blx r12 */
}

/* div-long and rem-long: vd = va op vb, a divisor of zero an exit (the interpreter throws ArithmeticException). */
static void long_divide(emitter *e, u32 pc, bool rem, u32 vd, u32 va, u32 vb)
{
    ldr_v(e, R2, vb);
    ldr_v(e, R3, vb + 1);
    e32(e, 0xEA52, (R12 << 8) | R3);                             /* orrs r12, r2, r3 */
    fix(e, FIX_EXIT, pc, C_EQ);
    ldr_v(e, R0, va);
    ldr_v(e, R1, va + 1);
    call_c(e, rem ? (mg_addr)jit_rem_long : (mg_addr)jit_div_long);
    str_v(e, R0, vd);
    str_v(e, R1, vd + 1);
}

/* rem-float and rem-double: vd = va % vb. */
static void float_remainder(emitter *e, bool dbl, u32 vd, u32 va, u32 vb)
{
    if (dbl) {
        vldr_d(e, 0, va);
        vldr_d(e, 1, vb);
        call_c(e, (mg_addr)jit_rem_double);
        vstr_d(e, 0, vd);
    } else {
        vldr_s(e, 0, va);
        vldr_s(e, 1, vb);
        call_c(e, (mg_addr)jit_rem_float);
        vstr_s(e, 0, vd);
    }
}

/* long-to-float, long-to-double, float-to-long, double-to-long: vA = (kind) vB. */
static void long_convert(emitter *e, u32 op, u32 va, u32 vb)
{
    if (op == 0x85 || op == 0x86) {
        ldr_v(e, R0, vb);
        ldr_v(e, R1, vb + 1);
        call_c(e, op == 0x85 ? (mg_addr)jit_long_to_float : (mg_addr)jit_long_to_double);
        if (op == 0x85) vstr_s(e, 0, va);
        else vstr_d(e, 0, va);
    } else {
        if (op == 0x88) vldr_s(e, 0, vb);
        else vldr_d(e, 0, vb);
        call_c(e, op == 0x88 ? (mg_addr)jit_float_to_long : (mg_addr)jit_double_to_long);
        str_v(e, R0, va);
        str_v(e, R1, va + 1);
    }
}

/* ------------------------------------------------------------------ calls */

/*
 * An invoke: mg_dvm_jit_invoke(t, f, pc, &ctx->result), which does the call as the interpreter would; 1 back is an exception
 * pending (out, thrown here), 2 a call it leaves to the interpreter (out at the invoke, nothing done).
 */
static void compile_invoke(emitter *e, u32 pc)
{
    mov_imm(e, R2, pc);
    ldr_imm(e, R0, R5, JIT_CTX_T);
    ldr_imm(e, R1, R5, JIT_CTX_F);
    e32(e, 0xF200 | R5, (R3 << 8) | JIT_CTX_RESULT);           /* addw r3, r5, #result */
    mov_imm(e, R12, (u32)(mg_addr)mg_dvm_jit_invoke);
    e16(e, 0x47E0);                                             /* blx r12 */
    cmp_imm(e, R0, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    cmp_imm(e, R0, 2);
    fix(e, FIX_EXIT, pc, C_EQ);
}

/*
 * An invoke tried the quick way first: mg_dvm_jit_fast(ctx, m, desc, pc), the method resolved here once (the interpreter's
 * calls resolved it before the loop was compiled) and the arguments' registers packed into `desc` (mg_dvm_internal.h). A
 * static or direct call, or a virtual one of a method nothing overrides (final, or of a final class), names the method
 * itself; another virtual call, or an interface call, the one to look up by the object's class. 1 back is an exception
 * pending (out, thrown here); 3, a call it did not make, goes the general way (compile_invoke). Not for invoke-super, a
 * String constructor (new String's result replaces the object), a static method of a class not initialised by now, a call
 * the flag no-fast-calls leaves out.
 */
static int g_fast_calls = -1;          /* (under g_jit_lock, as compile is) */
/* An app hooks methods (mg_dvm_hook.c, mg_dvm_jit_hooks_on): a call done in place looks at its method's mark first (inline_call). */
static volatile bool g_jit_hooks;

static void fast_invoke(emitter *e, const mg_dvm_method *m, u32 pc)
{
    const u16 *insns = m->insns;
    mg_dvm_dex *d = m->cls->dex;
    u16 w = insns[pc];
    u32 op = w & 0xFF, idx = insns[pc + 1], count, desc, kind, done, slow[2], nslow = 0, i;
    bool range = op >= 0x74, known;
    int ik = (int)(op - (range ? 0x74u : 0x6eu));
    const mg_dvm_method *ref = idx < d->nmeth ? d->mcache[idx] : NULL;
    if (g_fast_calls < 0) {
        g_fast_calls = !mg_flag("no-fast-calls");
        mg_info("jit: %s", g_fast_calls ? "calls into compiled code go the quick way where they can (mg_dvm_jit_fast)"
                                        : "the flag no-fast-calls: every call goes the general way");
    }
    if (!g_fast_calls || !ref || ((ref->flags & DVM_ACC_STATIC) != 0) != (ik == DVM_INVOKE_STATIC)) {
        compile_invoke(e, pc);
        return;
    }
    switch (ik) {
    case DVM_INVOKE_STATIC:
        kind = ref->insns && ref->cls->state == DVM_CS_INITIALIZED ? DVM_FAST_STATIC : 9;
        break;
    case DVM_INVOKE_DIRECT:
        kind = ref->insns && ref->cls != g_dvm.c_string ? DVM_FAST_DIRECT : 9;
        break;
    case DVM_INVOKE_VIRTUAL:
        if (ref->cls->flags & DVM_ACC_INTERFACE) kind = 9;
        else if ((ref->flags | ref->cls->flags) & DVM_ACC_FINAL) kind = ref->insns ? DVM_FAST_DIRECT : 9;
        else kind = ref->vidx >= 0 ? DVM_FAST_VIRTUAL : 9;
        break;
    case DVM_INVOKE_INTERFACE:
        kind = (ref->cls->flags & DVM_ACC_INTERFACE) ? DVM_FAST_INTERFACE : 9;
        break;
    default:                                                     /* super */
        kind = 9;
        break;
    }
    if (range) {
        count = (u32)w >> 8;
        desc = DVM_FAST_RANGE | (count << 16) | insns[pc + 2];
    } else {
        count = (u32)w >> 12;
        desc = insns[pc + 2] | ((((u32)w >> 8) & 0xFu) << 16) | (count << 20);
    }
    if (kind == 9 || count > (range ? 255u : 5u) || (kind != DVM_FAST_STATIC && !count)) {
        compile_invoke(e, pc);
        return;
    }
    /*
     * A method named here that is known by now never to go the quick way goes the general way at once (String.equals and
     * hashCode, intrinsics, cost half as much again tried first): an intrinsic, a hooked one, a native method, one with
     * handlers or synchronized, one compiled with no entry at its start.
     */
    known = kind == DVM_FAST_STATIC || kind == DVM_FAST_DIRECT;
    if (known && (ref->intrinsic || ref->hooked || !ref->insns || ref->ntries || (ref->flags & DVM_ACC_SYNCHRONIZED) || (ref->jit && !ref->jit->fast))) {
        compile_invoke(e, pc);
        return;
    }
    desc |= kind << 28;
    mov_imm(e, R1, (u32)(mg_addr)ref);
    if (known) {
        /* Its code there yet, with its start for the quick way: asked here, with no call when not. */
        ldr_imm(e, R0, R1, (u32)offsetof(mg_dvm_method, jit));
        cmp_imm(e, R0, 0);
        slow[nslow++] = local_branch(e);
        ldr_imm(e, R0, R0, (u32)offsetof(mg_dvm_jit_code, fast));
        cmp_imm(e, R0, 0);
        slow[nslow++] = local_branch(e);
    }
    e16(e, 0x4628);                                             /* mov r0, r5 */
    mov_imm(e, R2, desc);
    mov_imm(e, R3, pc);
    mov_imm(e, R12, (u32)(mg_addr)mg_dvm_jit_fast);
    e16(e, 0x47E0);                                             /* blx r12 */
    cmp_imm(e, R0, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    cmp_imm(e, R0, 3);
    done = local_branch(e);
    for (i = 0; i < nslow; i++) local_to(e, slow[i], C_EQ);
    compile_invoke(e, pc);
    local_to(e, done, C_NE);
}

/*
 * The intrinsics that are a few instructions (mg_dvm_interp.c, "intrinsics"), in the code: String.length, isEmpty and
 * charAt, Math.min, max and abs of ints, Math.sqrt. A null string or an index out of its bounds goes by the call
 * (mg_dvm_jit_invoke), which throws as the interpreter would; the others cannot fail. False, with nothing emitted, for the
 * rest. The string's receiver is in r0, checked for null, when `kind` is not static.
 */
static bool intrinsic_in_code(emitter *e, u8 id, u32 count, u32 a0, u32 a1, u32 *fails, u8 *conds, u32 *nfails)
{
    switch (id) {
    case INTR_STRING_LENGTH:
        ldr_imm(e, R1, R0, (u32)offsetof(mg_jobj, u.str.len16));
        str_imm(e, R1, R5, JIT_CTX_RESULT);
        return true;
    case INTR_REFERENT: {
        /* Reference.getReferent: the referent's slot (a reference object always has it, as a subclass of Reference). */
        u32 off = (u32)MG_DVM_HEADER + g_dvm_referent_slot * (u32)sizeof(mg_jvalue);
        if (g_dvm_referent_slot == ~0u || off + 4 >= 4096) return false;
        ldr_imm(e, R1, R0, off);
        str_imm(e, R1, R5, JIT_CTX_RESULT);
        return true;
    }
    case INTR_STRING_ISEMPTY:
        ldr_imm(e, R1, R0, (u32)offsetof(mg_jobj, u.str.len16));
        mov_imm(e, R2, 0);                                       /* movw: the flags left as they are */
        cmp_imm(e, R1, 0);
        e16(e, 0xBF08);                                          /* it eq */
        e16(e, 0x2201);                                          /*   moveq r2, #1 */
        str_imm(e, R2, R5, JIT_CTX_RESULT);
        return true;
    case INTR_STRING_CHARAT:
        if (count < 2) return false;
        ldr_v(e, R1, a1);
        ldr_imm(e, R2, R0, (u32)offsetof(mg_jobj, u.str.len16));
        cmp_reg(e, R1, R2);
        conds[*nfails] = C_HS;
        fails[(*nfails)++] = local_branch(e);
        ldr_imm(e, R3, R0, (u32)offsetof(mg_jobj, u.str.u16));
        element(e, false, 3);                                    /* r2 = r3[r1], a halfword */
        str_imm(e, R2, R5, JIT_CTX_RESULT);
        return true;
    case INTR_MIN_I: case INTR_MAX_I:
        if (count < 2) return false;
        ldr_v(e, R0, a0);
        ldr_v(e, R1, a1);
        cmp_reg(e, R0, R1);
        e16(e, id == INTR_MIN_I ? 0xBFC8 : 0xBFB8);              /* it gt (min), it lt (max) */
        e16(e, 0x4608);                                          /*   mov r0, r1 */
        str_imm(e, R0, R5, JIT_CTX_RESULT);
        return true;
    case INTR_ABS_I:
        if (count < 1) return false;
        ldr_v(e, R0, a0);
        cmp_imm(e, R0, 0);
        e16(e, 0xBFB8);                                          /* it lt */
        e32(e, 0xF1C0, 0x0000);                                  /*   rsblt r0, r0, #0 */
        str_imm(e, R0, R5, JIT_CTX_RESULT);
        return true;
    case INTR_SQRT:
        if (count < 2) return false;
        vldr_d(e, 0, a0);
        e32(e, 0xEEB1, 0x0BC0);                                  /* vsqrt.f64 d0, d0 */
        e32(e, 0xED80 | R5, 0x0B00 | (JIT_CTX_RESULT / 4));      /* vstr d0, [r5, #result] */
        return true;
    default:
        return false;
    }
}

/*
 * A call of a method whose whole body is trivial (mg_dvm_interp.c, "trivial methods": empty, a constant returned, a getter,
 * a setter, a static getter), done in the code as the interpreter's call does it in place, without mg_dvm_jit_invoke and
 * its C call: the method the invoke names, which the interpreter's calls resolved, and its shape, which its first call
 * found (the loop ran interpreted before it was compiled). A virtual call checks that the object's class has that very
 * method in its slot. An object that is null, of a class that overrides the method, or without the getter's or setter's
 * field slot goes by mg_dvm_jit_invoke, which throws or runs the method as the interpreter would. False, with nothing
 * emitted: not such a call, which goes by mg_dvm_jit_invoke whole -- a hooked method's too (mg_dvm_hook.c). Once an app
 * hooks methods, a method may be hooked after the call is compiled: the call looks at its mark first, and a hooked one goes
 * by mg_dvm_jit_invoke, which calls its hook (code compiled before, without that look, was forgotten then: forget).
 */
static bool inline_call(emitter *e, const mg_dvm_method *m, u32 pc)
{
    const u16 *insns = m->insns;
    mg_dvm_dex *d = m->cls->dex;
    u16 w = insns[pc];
    u32 op = w & 0xFF, idx = insns[pc + 1], count, self, value, shape, sop, off = 0, i, nfails = 0, done;
    bool range = op >= 0x74;
    int kind = (int)(op - (range ? 0x74u : 0x6eu)), fk = 0;
    const mg_dvm_method *target;
    const mg_dvm_field *fld = NULL;
    u32 fails[6];
    u8 conds[6];
    u8 intrinsic;
    if (kind != DVM_INVOKE_VIRTUAL && kind != DVM_INVOKE_DIRECT && kind != DVM_INVOKE_STATIC) return false;
    if (range) {
        count = (u32)w >> 8;
        self = insns[pc + 2];
        value = self + 1;
    } else {
        count = (u32)w >> 12;
        self = insns[pc + 2] & 0xFu;
        value = ((u32)insns[pc + 2] >> 4) & 0xFu;
    }
    target = idx < d->nmeth ? d->mcache[idx] : NULL;
    if (!target || ((target->flags & DVM_ACC_STATIC) != 0) != (kind == DVM_INVOKE_STATIC)) return false;
    if ((target->cls->flags & DVM_ACC_INTERFACE) || target->stub || target->hooked) return false;
    if (kind == DVM_INVOKE_STATIC ? target->cls->state != DVM_CS_INITIALIZED : !count) return false;
    intrinsic = target->intrinsic;
    shape = target->shape;
    sop = SHAPE_OP(shape);
    if (intrinsic) {
        /* (Those in the code: what intrinsic_in_code does.) */
        if (intrinsic != INTR_STRING_LENGTH && intrinsic != INTR_STRING_ISEMPTY && intrinsic != INTR_STRING_CHARAT && intrinsic != INTR_MIN_I
            && intrinsic != INTR_MAX_I && intrinsic != INTR_ABS_I && intrinsic != INTR_SQRT && intrinsic != INTR_REFERENT)
            return false;
        if (intrinsic == INTR_REFERENT && (g_dvm_referent_slot == ~0u || (u32)MG_DVM_HEADER + g_dvm_referent_slot * (u32)sizeof(mg_jvalue) + 4 >= 4096))
            return false;
        if ((intrinsic == INTR_STRING_CHARAT || intrinsic == INTR_MIN_I || intrinsic == INTR_MAX_I || intrinsic == INTR_SQRT) && count < 2) return false;
    } else if (!target->insns) {
        return false;
    } else switch (SHAPE_OF(shape)) {
    case SHAPE_EMPTY: case SHAPE_CONST: break;
    case SHAPE_STATIC_GETTER:
        fld = target->shape_field;
        if (!fld || fld->cls->state != DVM_CS_INITIALIZED || !fld->cls->statics) return false;
        fk = k_field_kind[sop - 0x60];
        break;
    case SHAPE_GETTER: case SHAPE_SETTER:
        fld = target->shape_field;
        if (!fld || fld->slot > 0xFFFF || (SHAPE_OF(shape) == SHAPE_SETTER && count < 2)) return false;
        off = (u32)MG_DVM_HEADER + fld->slot * (u32)sizeof(mg_jvalue);
        if (off + 4 >= 4096) return false;
        fk = k_field_kind[sop - (SHAPE_OF(shape) == SHAPE_SETTER ? 0x59 : 0x52)];
        break;
    default: return false;
    }
    if (g_jit_hooks) {
        /* The method hooked since (mg_dvm_hook.c): by mg_dvm_jit_invoke, which calls the hook. */
        mov_imm(e, R1, (u32)(mg_addr)target);
        ldrb_imm(e, R1, R1, (u32)offsetof(mg_dvm_method, hooked));
        cmp_imm(e, R1, 0);
        conds[nfails] = C_NE;
        fails[nfails++] = local_branch(e);
    }
    if (kind != DVM_INVOKE_STATIC) {
        ldr_v(e, R0, self);
        cmp_imm(e, R0, 0);
        conds[nfails] = C_EQ;
        fails[nfails++] = local_branch(e);
        /* (Not for a final method, or one of a final class such as String: nothing can override it.) */
        if (kind == DVM_INVOKE_VIRTUAL && target->vidx >= 0 && !((target->flags | target->cls->flags) & DVM_ACC_FINAL)) {
            ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
            ldr_imm(e, R2, R1, (u32)offsetof(mg_dvm_class, nvtab));
            mov_imm(e, R3, (u32)target->vidx);
            cmp_reg(e, R3, R2);
            conds[nfails] = C_HS;
            fails[nfails++] = local_branch(e);
            ldr_imm(e, R1, R1, (u32)offsetof(mg_dvm_class, vtab));
            e32(e, 0xF850 | R1, (R1 << 12) | (2u << 4) | R3);    /* ldr r1, [r1, r3, lsl #2] */
            mov_imm(e, R2, (u32)(mg_addr)target);
            cmp_reg(e, R1, R2);
            conds[nfails] = C_NE;
            fails[nfails++] = local_branch(e);
        }
        if (!intrinsic && fld && SHAPE_OF(shape) != SHAPE_STATIC_GETTER) {
            ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
            ldr_imm(e, R1, R1, (u32)offsetof(mg_dvm_class, nslots));
            mov_imm(e, R2, fld->slot);
            cmp_reg(e, R2, R1);
            conds[nfails] = C_HS;
            fails[nfails++] = local_branch(e);
        }
    }
    if (intrinsic) {
        /* (Checked above that it is one of those: this emits it.) */
        intrinsic_in_code(e, intrinsic, count, self, value, fails, conds, &nfails);
    } else switch (SHAPE_OF(shape)) {
    case SHAPE_CONST:
        mov_imm(e, R0, (u32)SHAPE_VALUE(shape));
        str_imm(e, R0, R5, JIT_CTX_RESULT);
        break;
    case SHAPE_STATIC_GETTER:
        mov_imm(e, R0, (u32)(mg_addr)&fld->cls->statics[fld->slot]);
        /* fall through: read as a getter's field, at offset 0 */
    case SHAPE_GETTER:
        if (fk == 5) {
            field_access(e, false, 0, R2, R0, off);
            field_access(e, false, 0, R3, R0, off + 4);
            str_imm(e, R2, R5, JIT_CTX_RESULT);
            str_imm(e, R3, R5, JIT_CTX_RESULT + 4);
        } else {
            field_access(e, false, fk, R2, R0, off);
            str_imm(e, R2, R5, JIT_CTX_RESULT);
        }
        break;
    case SHAPE_SETTER:
        ldr_v(e, R2, value);
        field_access(e, true, fk, R2, R0, off);
        break;
    default:                                                     /* empty */
        break;
    }
    if (nfails) {
        done = local_branch(e);
        for (i = 0; i < nfails; i++) local_to(e, fails[i], conds[i]);
        compile_invoke(e, pc);
        local_to(e, done, C_AL);
    }
    e->in_place++;
    return true;
}

/*
 * const-class, the monitors, check-cast, instance-of, new-instance, new-array, aput-object: mg_dvm_jit_op(t, f, pc), which
 * does it on the frame's registers as the interpreter does; 1 back is an exception pending, 2 one it leaves to the interpreter.
 */
static void compile_runtime_op(emitter *e, u32 pc)
{
    mov_imm(e, R2, pc);
    ldr_imm(e, R0, R5, JIT_CTX_T);
    ldr_imm(e, R1, R5, JIT_CTX_F);
    mov_imm(e, R12, (u32)(mg_addr)mg_dvm_jit_op);
    e16(e, 0x47E0);                                             /* blx r12 */
    cmp_imm(e, R0, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    cmp_imm(e, R0, 2);
    fix(e, FIX_EXIT, pc, C_EQ);
}

/*
 * check-cast and instance-of: in the code when the object is null or of the very class named (resolved by now: its pointer a
 * constant), mg_dvm_jit_op for the rest (a subclass, an interface, a class not yet resolved).
 */
static void compile_type_test(emitter *e, const mg_dvm_method *m, u32 pc, u32 op, u32 vo, u32 vd, u32 idx)
{
    mg_dvm_dex *d = m->cls->dex;
    mg_dvm_class *c = idx < d->ntype ? d->tcache[idx] : NULL;
    u32 null_done, same_done, slow_done;
    if (!c) {
        compile_runtime_op(e, pc);
        return;
    }
    ldr_v(e, R0, vo);
    cmp_imm(e, R0, 0);
    null_done = local_branch(e);
    ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
    mov_imm(e, R2, (u32)(mg_addr)c);
    cmp_reg(e, R1, R2);
    same_done = local_branch(e);
    compile_runtime_op(e, pc);
    slow_done = local_branch(e);
    if (op == 0x20) {
        /* instance-of: 0 for null, 1 for the very class */
        u32 one_done;
        local_to(e, null_done, C_EQ);
        mov_imm(e, R0, 0);
        one_done = local_branch(e);
        local_to(e, same_done, C_EQ);
        mov_imm(e, R0, 1);
        local_to(e, one_done, C_AL);
        str_v(e, R0, vd);
        local_to(e, slow_done, C_AL);
    } else {
        local_to(e, null_done, C_EQ);
        local_to(e, same_done, C_EQ);
        local_to(e, slow_done, C_AL);
    }
}

/* One instruction; false when it is an exit. */
static bool compile_insn(emitter *e, const mg_dvm_method *m, u32 pc)
{
    const u16 *insns = m->insns;
    u16 w = insns[pc];
    u32 op = w & 0xFF, a4 = (w >> 8) & 0xF, b4 = w >> 12, aa = w >> 8;
#define F(n) ((u32)insns[pc + (n)])
    switch (op) {
    case 0x00:
        if (w) return false;           /* the data of a switch or an array: never run */
        return true;
    case 0x01: case 0x07: ldr_v(e, R0, b4); str_v(e, R0, a4); return true;
    case 0x02: case 0x08: ldr_v(e, R0, F(1)); str_v(e, R0, aa); return true;
    case 0x03: case 0x09: ldr_v(e, R0, F(2)); str_v(e, R0, F(1)); return true;
    case 0x04: ldr_v(e, R0, b4); ldr_v(e, R1, b4 + 1); str_v(e, R0, a4); str_v(e, R1, a4 + 1); return true;
    case 0x05: ldr_v(e, R0, F(1)); ldr_v(e, R1, F(1) + 1); str_v(e, R0, aa); str_v(e, R1, aa + 1); return true;
    case 0x06: ldr_v(e, R0, F(2)); ldr_v(e, R1, F(2) + 1); str_v(e, R0, F(1)); str_v(e, R1, F(1) + 1); return true;
    case 0x0e:                                                   /* return-void, and the others: the value for ctx->result */
        mov_imm(e, R0, 0);
        mov_imm(e, R1, 0);
        str_imm(e, R0, R5, JIT_CTX_RESULT);
        str_imm(e, R1, R5, JIT_CTX_RESULT + 4);
        fix(e, FIX_RETURN, pc, C_AL);
        return true;
    case 0x0f: case 0x11:                                        /* return, return-object */
        ldr_v(e, R0, aa);
        mov_imm(e, R1, 0);
        str_imm(e, R0, R5, JIT_CTX_RESULT);
        str_imm(e, R1, R5, JIT_CTX_RESULT + 4);
        fix(e, FIX_RETURN, pc, C_AL);
        return true;
    case 0x10:                                                   /* return-wide */
        ldr_v(e, R0, aa);
        ldr_v(e, R1, aa + 1);
        str_imm(e, R0, R5, JIT_CTX_RESULT);
        str_imm(e, R1, R5, JIT_CTX_RESULT + 4);
        fix(e, FIX_RETURN, pc, C_AL);
        return true;
    case 0x0a: case 0x0c: ldr_imm(e, R0, R5, JIT_CTX_RESULT); str_v(e, R0, aa); return true;   /* move-result, -object: a compiled call's */
    case 0x0b: ldr_imm(e, R0, R5, JIT_CTX_RESULT); ldr_imm(e, R1, R5, JIT_CTX_RESULT + 4); str_v(e, R0, aa); str_v(e, R1, aa + 1); return true;
    case 0x16: case 0x17: case 0x18: case 0x19: {               /* const-wide/16, /32, const-wide, /high16 */
        u64 v = op == 0x16 ? (u64)(i64)(i16)F(1)
                : op == 0x17 ? (u64)(i64)(i32)(F(1) | F(2) << 16)
                : op == 0x18 ? (u64)F(1) | (u64)F(2) << 16 | (u64)F(3) << 32 | (u64)F(4) << 48
                             : (u64)F(1) << 48;
        mov_imm(e, R0, (u32)v);
        mov_imm(e, R1, (u32)(v >> 32));
        str_v(e, R0, aa);
        str_v(e, R1, aa + 1);
        return true;
    }
    case 0x1a: case 0x1b: {                                      /* const-string, /jumbo: one already made (interned, never freed) */
        mg_dvm_dex *d = m->cls->dex;
        u32 idx = op == 0x1a ? F(1) : F(1) | F(2) << 16;
        mg_jobj *s = idx < d->nstr ? d->scache[idx] : NULL;
        if (!s) return false;
        mov_imm(e, R0, (u32)(mg_addr)s);
        str_v(e, R0, aa);
        return true;
    }
    case 0x1f: compile_type_test(e, m, pc, op, aa, 0, F(1)); return true;      /* check-cast vAA */
    case 0x20: compile_type_test(e, m, pc, op, b4, a4, F(1)); return true;     /* instance-of vA, vB */
    case 0x1c: case 0x1d: case 0x1e: case 0x22: case 0x23: case 0x4d:
        compile_runtime_op(e, pc);
        return true;
    case 0x2b: case 0x2c: return compile_switch(e, m, pc, op, aa, (i32)(F(1) | F(2) << 16));
    case 0x31: cmp_long(e, aa, F(1) & 0xFF, F(1) >> 8); return true;
    case 0x2d: case 0x2e: case 0x2f: case 0x30:                  /* cmpl-float, cmpg-float, cmpl-double, cmpg-double */
        vfp_compare(e, op >= 0x2f, op == 0x2e || op == 0x30, aa, F(1) & 0xFF, F(1) >> 8);
        return true;
    case 0x7f: case 0x80: case 0x82: case 0x83: case 0x87: case 0x89: case 0x8a: case 0x8c:
        return vfp_convert(e, op, a4, b4);
    case 0x85: case 0x86: case 0x88: case 0x8b:                  /* long-to-float, -double; float-, double-to-long */
        long_convert(e, op, a4, b4);
        return true;
    case 0x9e: case 0x9f: long_divide(e, pc, op == 0x9f, aa, F(1) & 0xFF, F(1) >> 8); return true;     /* div-long, rem-long */
    case 0xbe: case 0xbf: long_divide(e, pc, op == 0xbf, a4, a4, b4); return true;                       /* and /2addr */
    case 0xaa: case 0xaf: float_remainder(e, op == 0xaf, aa, F(1) & 0xFF, F(1) >> 8); return true;    /* rem-float, rem-double */
    case 0xca: case 0xcf: float_remainder(e, op == 0xcf, a4, a4, b4); return true;                     /* and /2addr */
    case 0x45:                                                   /* aget-wide: the element at r3 + r1 * 8 */
        array_checks(e, pc, F(1) & 0xFF, F(1) >> 8, 8);
        e32(e, 0xEB03, (R3 << 8) | (3u << 6) | R1);              /* add r3, r3, r1, lsl #3 */
        ldr_imm(e, R0, R3, 0);
        ldr_imm(e, R1, R3, 4);
        str_v(e, R0, aa);
        str_v(e, R1, aa + 1);
        return true;
    case 0x4c:                                                   /* aput-wide */
        array_checks(e, pc, F(1) & 0xFF, F(1) >> 8, 8);
        e32(e, 0xEB03, (R3 << 8) | (3u << 6) | R1);
        ldr_v(e, R0, aa);
        ldr_v(e, R1, aa + 1);
        str_imm(e, R0, R3, 0);
        str_imm(e, R1, R3, 4);
        return true;
    case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:
        if (!inline_call(e, m, pc)) fast_invoke(e, m, pc);
        return true;
    case 0x7d:                                                   /* neg-long: 0 - (hi:lo) */
        ldr_v(e, R0, b4);
        ldr_v(e, R1, b4 + 1);
        e32(e, 0xF1D0 | R0, (R0 << 8));                          /* rsbs r0, r0, #0 */
        mov_imm(e, R2, 0);
        e32(e, 0xEB60 | R2, (R1 << 8) | R1);                     /* sbc r1, r2, r1 */
        str_v(e, R0, a4);
        str_v(e, R1, a4 + 1);
        return true;
    case 0x7e:                                                   /* not-long */
        ldr_v(e, R0, b4);
        ldr_v(e, R1, b4 + 1);
        e32(e, 0xEA6F, (R0 << 8) | R0);
        e32(e, 0xEA6F, (R1 << 8) | R1);
        str_v(e, R0, a4);
        str_v(e, R1, a4 + 1);
        return true;
    case 0x81:                                                   /* int-to-long: the sign into the high word */
        ldr_v(e, R0, b4);
        e32(e, 0xEA4F, (7u << 12) | (R1 << 8) | (3u << 6) | (2u << 4) | R0);     /* asr r1, r0, #31 */
        str_v(e, R0, a4);
        str_v(e, R1, a4 + 1);
        return true;
    case 0x84: ldr_v(e, R0, b4); str_v(e, R0, a4); return true;  /* long-to-int: the low word */
    case 0x12: mov_imm(e, R0, (u32)((i32)(i16)w >> 12)); str_v(e, R0, a4); return true;
    case 0x13: mov_imm(e, R0, (u32)(i32)(i16)F(1)); str_v(e, R0, aa); return true;
    case 0x14: mov_imm(e, R0, F(1) | F(2) << 16); str_v(e, R0, aa); return true;
    case 0x15: mov_imm(e, R0, F(1) << 16); str_v(e, R0, aa); return true;
    case 0x21:                                                   /* array-length */
        ldr_v(e, R0, b4);
        cmp_imm(e, R0, 0);
        fix(e, FIX_EXIT, pc, C_EQ);
        ldr_imm(e, R1, R0, (u32)offsetof(mg_jobj, u.arr.len));
        str_v(e, R1, a4);
        return true;
    case 0x28: branch_to(e, pc, (u32)((i32)pc + (i8)(w >> 8)), C_AL); return true;
    case 0x29: branch_to(e, pc, (u32)((i32)pc + (i16)F(1)), C_AL); return true;
    case 0x2a: branch_to(e, pc, (u32)((i32)pc + (i32)(F(1) | F(2) << 16)), C_AL); return true;
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37:
        ldr_v(e, R0, a4);
        ldr_v(e, R1, b4);
        cmp_reg(e, R0, R1);
        branch_to(e, pc, (u32)((i32)pc + (i16)F(1)), if_cond(op - 0x32));
        return true;
    case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d:
        ldr_v(e, R0, aa);
        cmp_imm(e, R0, 0);
        branch_to(e, pc, (u32)((i32)pc + (i16)F(1)), if_cond(op - 0x38));
        return true;
    case 0x44: case 0x47: case 0x48: case 0x49: case 0x4a: {    /* aget, -boolean, -byte, -char, -short */
        static const u8 sizes[] = { 4, 0, 0, 1, 1, 2, 2 }, kinds[] = { 0, 0, 0, 1, 2, 3, 4 };
        array_checks(e, pc, F(1) & 0xFF, F(1) >> 8, sizes[op - 0x44]);
        element(e, false, kinds[op - 0x44]);
        str_v(e, R2, aa);
        return true;
    }
    case 0x46:                                                   /* aget-object */
        array_checks(e, pc, F(1) & 0xFF, F(1) >> 8, 0);
        element(e, false, 0);
        str_v(e, R2, aa);
        return true;
    case 0x4b: case 0x4e: case 0x4f: case 0x50: case 0x51: {    /* aput, -boolean, -byte, -char, -short */
        static const u8 sizes[] = { 4, 0, 0, 1, 1, 2, 2 }, kinds[] = { 0, 0, 0, 1, 1, 3, 3 };
        array_checks(e, pc, F(1) & 0xFF, F(1) >> 8, sizes[op - 0x4b]);
        ldr_v(e, R2, aa);
        element(e, true, kinds[op - 0x4b]);
        return true;
    }
    case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:     /* iget, -wide, -object, -boolean, -byte, -char, -short */
    case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:     /* iput, and the same */
        return instance_field(e, m, pc, op, a4, b4, F(1));
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:     /* sget, and the same */
    case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d:     /* sput, and the same */
        return static_field(e, m, op, aa, F(1));
    case 0x7b: ldr_v(e, R1, b4); e32(e, 0xF1C0 | R1, (R0 << 8)); str_v(e, R0, a4); return true;     /* neg-int */
    case 0x7c: ldr_v(e, R1, b4); e32(e, 0xEA6F, (R0 << 8) | R1); str_v(e, R0, a4); return true;     /* not-int */
    case 0x8d: ldr_v(e, R1, b4); e32(e, 0xFA4F, 0xF080 | (R0 << 8) | R1); str_v(e, R0, a4); return true; /* int-to-byte */
    case 0x8e: ldr_v(e, R1, b4); e32(e, 0xFA1F, 0xF080 | (R0 << 8) | R1); str_v(e, R0, a4); return true; /* int-to-char */
    case 0x8f: ldr_v(e, R1, b4); e32(e, 0xFA0F, 0xF080 | (R0 << 8) | R1); str_v(e, R0, a4); return true; /* int-to-short */
    default:
        break;
    }
    if (op >= 0x90 && op <= 0x9a) {                              /* binop vAA, vBB, vCC */
        ldr_v(e, R1, F(1) & 0xFF);
        ldr_v(e, R2, F(1) >> 8);
        arith(e, pc, bin_op(op - 0x90), aa, R1, R2);
        return true;
    }
    if (op >= 0x9b && op <= 0xa5) return long_op(e, op - 0x9b, aa, F(1) & 0xFF, F(1) >> 8);    /* long binop vAA, vBB, vCC */
    if ((op >= 0xa6 && op <= 0xa9) || (op >= 0xab && op <= 0xae)) {                             /* float and double binop vAA, vBB, vCC */
        vfp_arith(e, (op - 0xa6) % 5, op >= 0xab, aa, F(1) & 0xFF, F(1) >> 8);
        return true;
    }
    if ((op >= 0xc6 && op <= 0xc9) || (op >= 0xcb && op <= 0xce)) {                             /* their /2addr vA, vB */
        vfp_arith(e, (op - 0xc6) % 5, op >= 0xcb, a4, a4, b4);
        return true;
    }
    if (op >= 0xbb && op <= 0xc5) return long_op(e, op - 0xbb, a4, a4, b4);                    /* long binop/2addr vA, vB */
    if (op >= 0xb0 && op <= 0xba) {                              /* binop/2addr vA, vB */
        ldr_v(e, R1, a4);
        ldr_v(e, R2, b4);
        arith(e, pc, bin_op(op - 0xb0), a4, R1, R2);
        return true;
    }
    if (op >= 0xd0 && op <= 0xd7) {                              /* binop/lit16 vA, vB, #+CCCC */
        int o = lit16_op(op - 0xd0);
        i32 lit = (i16)F(1);
        if ((o == OP_DIV || o == OP_REM) && lit == 0) return false;
        ldr_v(e, R1, b4);
        mov_imm(e, R2, (u32)lit);
        data_op(e, o, R0, R1, R2);
        str_v(e, R0, a4);
        return true;
    }
    if (op >= 0xd8 && op <= 0xe2) {                              /* binop/lit8 vAA, vBB, #+CC */
        int o = lit8_op(op - 0xd8);
        i32 lit = (i8)(F(1) >> 8);
        if ((o == OP_DIV || o == OP_REM) && lit == 0) return false;
        ldr_v(e, R1, F(1) & 0xFF);
        mov_imm(e, R2, (u32)lit);
        data_op(e, o, R0, R1, R2);
        str_v(e, R0, aa);
        return true;
    }
#undef F
    return false;
}

/* The branch targets: entries, and what a jump back is checked at. */
static void mark_targets(const mg_dvm_method *m, u8 *target)
{
    const u16 *insns = m->insns;
    u32 pc = 0;
    target[0] = 1;
    while (pc < m->ninsns) {
        u16 w = insns[pc];
        u32 op = w & 0xFF, n = mg_dvm_insn_units(insns, m->ninsns, pc);
        i32 off = 0;
        bool br = true;
        if (!n) break;
        switch (op) {
        case 0x28: off = (i8)(w >> 8); break;
        case 0x29: off = (i16)insns[pc + 1]; break;
        case 0x2a: off = (i32)((u32)insns[pc + 1] | (u32)insns[pc + 2] << 16); break;
        default:
            if (op >= 0x32 && op <= 0x3d) off = (i16)insns[pc + 1];
            else br = false;
        }
        if (br && (u32)((i32)pc + off) < m->ninsns) target[(i32)pc + off] = 1;
        pc += n;
    }
}

/* The loop heads kept as entries: each target of a jump back whose loop, from the head to that jump, hands nothing back. */
static void whole_loops(const mg_dvm_method *m, const u8 *handback, u8 *target)
{
    const u16 *insns = m->insns;
    u8 *whole = (u8 *)calloc(m->ninsns, 1);
    u32 pc = 0, k;
    if (!whole) {
        memset(target, 0, m->ninsns);
        return;
    }
    while (pc < m->ninsns) {
        u16 w = insns[pc];
        u32 op = w & 0xFF, n = mg_dvm_insn_units(insns, m->ninsns, pc);
        i32 off = 1;
        if (!n) break;
        if (op == 0x28) off = (i8)(w >> 8);
        else if (op == 0x29) off = (i16)insns[pc + 1];
        else if (op == 0x2a) off = (i32)((u32)insns[pc + 1] | (u32)insns[pc + 2] << 16);
        else if (op >= 0x32 && op <= 0x3d) off = (i16)insns[pc + 1];
        if (off <= 0 && (u32)((i32)pc + off) < m->ninsns) {
            u32 head = (u32)((i32)pc + off);
            bool ok = true;
            for (k = head; k <= pc && ok; k++) ok = !handback[k];
            if (ok) whole[head] = 1;
        }
        pc += n;
    }
    for (k = 0; k < m->ninsns; k++) target[k] = whole[k];
    free(whole);
}

/*
 * Memory for code: pages of an arena of the process's, reserved a megabyte at a time and committed as code comes (written,
 * then made executable once; or, small methods' shared pages, writable and executable at once where the phone allows it:
 * jit_rwx below). A method on an allocation of its own took 64 KB of
 * addresses (Windows' granularity) for a few hundred bytes of code, and Yandex Maps has addresses to spare for nothing.
 * Under g_jit_lock.
 */
#define JIT_ARENA (1u << 20)
static u8 *g_arena;
static size_t g_arena_used;

static u8 *code_pages(size_t bytes)
{
    size_t need = (bytes + MG_PAGE - 1) & ~(size_t)(MG_PAGE - 1);
    u8 *p;
    if (need > JIT_ARENA / 4) {
        p = (u8 *)mg_mem_alloc(need);
        if (p) g_page_bytes += need;
        return p;
    }
    if (!g_arena || g_arena_used + need > JIT_ARENA) {
        u8 *a = (u8 *)mg_mem_reserve(JIT_ARENA);
        if (!a) return NULL;
        g_arena = a;
        g_arena_used = 0;
    }
    p = g_arena + g_arena_used;
    if (!mg_mem_commit(p, need, MG_PROT_RW)) return NULL;
    g_arena_used += need;
    g_page_bytes += need;
    return p;
}

/*
 * Small methods' code shares pages: most are a few hundred bytes, and the data app's 536 methods came to 377 KB of code
 * on 2.2 MB of pages, each on pages of its own. Code under half a page is written to the page being filled, which is
 * made executable, and its methods given their code, once the next one does not fit or JIT_WAIT_NS after its first --
 * looked at by the next compile and by a waiting method's own calls and jumps back (mg_dvm_jit_back, every threshold's
 * worth of them). A page is never written once it is executable, so a method waits that long interpreted. With the
 * threshold at 1 (the tests: every method compiled at its first call or jump back) nothing waits. Under g_jit_lock.
 */
#define JIT_SHARED (MG_PAGE / 2)
#define JIT_WAIT_NS 200000000ull
static u8 *g_fill;
static size_t g_fill_used;
static u64 g_fill_since;
static struct {
    mg_dvm_method *m;
    mg_dvm_jit_code *c;
} g_waiting[MG_PAGE / 32];
static u32 g_nwaiting;

/* The methods whose code is published, for forgetting it (forget). Under g_jit_lock. */
static mg_dvm_method **g_compiled;
static u32 g_ncompiled, g_capcompiled;

static void publish(mg_dvm_method *m, mg_dvm_jit_code *c)
{
    /* Its start for the calls that go the quick way (mg_dvm_jit_fast), where such a call needs nothing more. */
    if (!m->ntries && !(m->flags & DVM_ACC_SYNCHRONIZED) && !m->intrinsic) {
        u32 at = mg_dvm_jit_entry(c, 0);
        if (at) c->fast = c->pages + (size_t)(at - 1) * 2 + 1;
    }
    if (g_ncompiled == g_capcompiled) {
        g_capcompiled = g_capcompiled ? g_capcompiled * 2 : 256;
        g_compiled = (mg_dvm_method **)mg_realloc(g_compiled, sizeof(*g_compiled) * g_capcompiled);
    }
    g_compiled[g_ncompiled++] = m;
    /* Published whole: the code and its tables before the pointer (an interlocked exchange is a barrier). */
    mg_atomic_cas_ptr((void *volatile *)&m->jit, NULL, c);
    m->jit_state = 1;
    mg_atomic_add(&g_jit_methods, 1);
}

static void refuse(mg_dvm_method *m)
{
    m->jit_state = 2;
    mg_atomic_add(&g_jit_refused, 1);
}

/*
 * Where pages may be writable and executable at once (mg_mem_rwx; the flag no-rwx: not), the page being filled is so from
 * its start, and each method on it is given its code as soon as it is written there: none waits interpreted for the page
 * to be made executable.
 */
static int g_jit_rwx = -1;
static bool g_fill_rwx;

static bool jit_rwx(void)
{
    if (g_jit_rwx < 0) g_jit_rwx = !mg_flag("no-rwx") && mg_mem_rwx() ? 1 : 0;
    return g_jit_rwx > 0;
}

/* The page being filled made executable, and the methods on it given their code. */
static void fill_done(void)
{
    u32 i;
    bool ok;
    if (!g_fill) return;
    if (g_fill_rwx) {
        /* (Its methods have their code already.) */
        g_fill = NULL;
        g_fill_used = 0;
        return;
    }
    ok = mg_mem_protect(g_fill, MG_PAGE, MG_PROT_RX);
    if (ok) mg_icache_flush(g_fill, g_fill_used);
    for (i = 0; i < g_nwaiting; i++) {
        if (ok) {
            publish(g_waiting[i].m, g_waiting[i].c);
        } else {
            refuse(g_waiting[i].m);
            free(g_waiting[i].c);
        }
    }
    g_nwaiting = 0;
    g_fill = NULL;
    g_fill_used = 0;
}

/* A method's code put where it runs from: on pages of its own, published at once, or on the page being filled. False: no memory. */
static bool place(mg_dvm_method *m, mg_dvm_jit_code *c, const u16 *code, size_t bytes)
{
    u8 *p;
    g_code_bytes += bytes;
    if (bytes > JIT_SHARED) {
        size_t span = (bytes + MG_PAGE - 1) & ~(size_t)(MG_PAGE - 1);
        p = code_pages(bytes);
        /* (Pages a failure leaves behind stay in the arena, unused: they are never given out again.) */
        if (!p) return false;
        memcpy(p, code, bytes);
        if (!mg_mem_protect(p, span, MG_PROT_RX)) return false;
        mg_icache_flush(p, bytes);
        c->pages = p;
        c->run = (int (*)(mg_dvm_jit_ctx *, const void *))(void *)((mg_addr)p | 1);
        publish(m, c);
        return true;
    }
    if (g_fill && (g_fill_used + bytes > MG_PAGE || g_nwaiting == MG_COUNT(g_waiting))) fill_done();
    if (!g_fill) {
        g_fill = code_pages(MG_PAGE);
        if (!g_fill) return false;
        g_fill_used = 0;
        g_fill_since = mg_time_monotonic_ns();
        g_fill_rwx = jit_rwx() && mg_mem_protect(g_fill, MG_PAGE, MG_PROT_RWX);
    }
    p = g_fill + g_fill_used;
    memcpy(p, code, bytes);
    g_fill_used = (g_fill_used + bytes + 3) & ~(size_t)3;
    c->pages = p;
    c->run = (int (*)(mg_dvm_jit_ctx *, const void *))(void *)((mg_addr)p | 1);
    if (g_fill_rwx) {
        mg_icache_flush(p, bytes);
        publish(m, c);
        return true;
    }
    g_waiting[g_nwaiting].m = m;
    g_waiting[g_nwaiting].c = c;
    g_nwaiting++;
    m->jit_state = 3;
    if (g_dvm_jit_threshold <= 1) fill_done();
    return true;
}

static mg_dvm_jit_code *compile(mg_dvm_method *m)
{
    emitter e;
    u32 *code_at = NULL, *exit_at = NULL, *back_at = NULL, *throw_at = NULL, pc, i, epilogue, thrown, returned, compiled = 0, exits = 0, loops = 0,
        calls = 0, run = 0;
    bool start;
    u8 *target = NULL, *handback = NULL;
    mg_dvm_jit_code *c = NULL;
    size_t bytes;
    if (!m->insns || m->regs > 1000 || m->ninsns == 0 || m->ninsns > 65535) return NULL;
    memset(&e, 0, sizeof(e));
    code_at = (u32 *)calloc(m->ninsns, sizeof(u32));
    exit_at = (u32 *)calloc(m->ninsns, sizeof(u32));
    back_at = (u32 *)calloc(m->ninsns, sizeof(u32));
    target = (u8 *)calloc(m->ninsns, 1);
    handback = (u8 *)calloc(m->ninsns, 1);
    throw_at = (u32 *)calloc(m->ninsns, sizeof(u32));
    if (!code_at || !exit_at || !back_at || !target || !handback || !throw_at) goto out;
    mark_targets(m, target);
    /* The way in: push {r3-r7, lr} (six words: the stack stays 8-byte aligned for the calls); r5 = ctx, r4 = ctx->regs,
       r7 = ctx->slow; bx r1 (the entry). */
    e16(&e, 0xB5F8);
    e16(&e, 0x4605);
    e16(&e, 0x682C);
    e16(&e, 0x686F);
    e16(&e, 0x4708);
    for (pc = 0; pc < m->ninsns;) {
        u32 n = mg_dvm_insn_units(m->insns, m->ninsns, pc);
        if (!n) break;
        code_at[pc] = e.n + 1;
        if (compile_insn(&e, m, pc)) {
            u32 op = m->insns[pc] & 0xFF;
            compiled++;
            calls += (op >= 0x6e && op <= 0x72) || (op >= 0x74 && op <= 0x78);
        } else {
            exits++;
            handback[pc] = 1;
            mov_imm(&e, R0, pc);
            fix(&e, FIX_EPILOGUE, 0, C_AL);
        }
        pc += n;
    }
    if (!compiled || e.failed) goto out;
    /*
     * Entered only at the head of a loop compiled whole, from its head to its jump back with no instruction handed back
     * in between: a loop whose body calls out (a call, an allocation) would go in and out at every turn, which costs more
     * than what it saves (the benchmark's Math.min, Integer.valueOf: twice as slow). Those stay the interpreter's, and a
     * method with no such loop is not kept: known before any code memory is taken.
     */
    whole_loops(m, handback, target);
    for (pc = 0; pc < m->ninsns; pc++) loops += target[pc];
    /*
     * Its start an entry too, for its calls, when the code there does some work before it hands anything back: four
     * instructions in a row, or a return. (Fewer, and going in and out of the code costs more than it saves.)
     */
    for (pc = 0; pc < m->ninsns && !handback[pc] && run < 4;) {
        u32 n = mg_dvm_insn_units(m->insns, m->ninsns, pc), op = m->insns[pc] & 0xFF;
        if (!n) break;
        run = op >= 0x0e && op <= 0x11 ? 4 : run + 1;
        pc += n;
    }
    start = run >= 4;
    if (start) target[0] = 1;
    if (!loops && !start) goto out;
    /* The exits of the checks and the jump-back stubs, after the code: r0 = the instruction's pc, to the way out. (Each
       instruction's code goes on into the next one's; the last never ends that way, which bytecode cannot.) */
    for (i = 0; i < e.nfx; i++) {
        u32 at = e.fx[i].pc;
        if (e.fx[i].kind == FIX_EXIT && !exit_at[at]) {
            exit_at[at] = e.n + 1;
            mov_imm(&e, R0, at);
            fix(&e, FIX_EPILOGUE, 0, C_AL);
        } else if (e.fx[i].kind == FIX_THROW && !throw_at[at]) {
            /* A call that threw: out with the exception pending, at the invoke. */
            throw_at[at] = e.n + 1;
            mov_imm(&e, R0, at);
            fix(&e, FIX_THROW_EPILOGUE, 0, C_AL);
        } else if (e.fx[i].kind == FIX_BACK && !back_at[at] && at < m->ninsns) {
            /* A jump back: on to the target, unless the instructions are watched (g_dvm.slow): then out at the target. */
            back_at[at] = e.n + 1;
            e16(&e, 0x6838);           /* ldr r0, [r7] */
            cmp_imm(&e, R0, 0);
            fix(&e, FIX_EXIT, at, C_NE);
            fix(&e, FIX_CODE, at, C_AL);
        }
    }
    /* The way out: ctx->pc = r0; return 0. And with an exception pending: return 1. */
    epilogue = e.n;
    e16(&e, 0x60A8);                   /* str r0, [r5, #8] */
    e16(&e, 0x2000);                   /* movs r0, #0 */
    e16(&e, 0xBDF8);                   /* pop {r3-r7, pc} */
    thrown = e.n;
    e16(&e, 0x60A8);                   /* str r0, [r5, #8] */
    e16(&e, 0x2001);                   /* movs r0, #1 */
    e16(&e, 0xBDF8);                   /* pop {r3-r7, pc} */
    /* And from a return, its value in ctx->result: return 2. */
    returned = e.n;
    e16(&e, 0x2002);                   /* movs r0, #2 */
    e16(&e, 0xBDF8);                   /* pop {r3-r7, pc} */
    if (e.failed) goto out;
    for (i = 0; i < e.nfx; i++) {
        fixup *x = &e.fx[i];
        u32 to;
        switch (x->kind) {
        case FIX_CODE: to = x->pc < m->ninsns ? code_at[x->pc] : 0; break;
        case FIX_EXIT: to = exit_at[x->pc]; break;
        case FIX_BACK: to = x->pc < m->ninsns ? back_at[x->pc] : 0; break;
        case FIX_THROW: to = throw_at[x->pc]; break;
        case FIX_THROW_EPILOGUE: to = thrown + 1; break;
        case FIX_RETURN: to = returned + 1; break;
        default: to = epilogue + 1; break;
        }
        /* A branch to the middle of an instruction or outside the method: not compiled. */
        if (!to || !put_branch(e.b, x->at, to - 1, x->cond)) goto out;
    }
    bytes = (size_t)e.n * 2;
    for (pc = 0, i = 0; pc < m->ninsns; pc++) i += target[pc] && code_at[pc];
    c = (mg_dvm_jit_code *)calloc(1, sizeof(*c) + sizeof(c->entries[0]) * i);
    if (!c) goto out;
    c->size = bytes;
    c->in_place = e.in_place;
    for (pc = 0; pc < m->ninsns; pc++)
        if (target[pc] && code_at[pc]) {
            c->entries[c->nentries].pc = pc;
            c->entries[c->nentries++].at = code_at[pc];
        }
    /* Published, or waiting for its page (fill_done), from here on. */
    if (!place(m, c, e.b, bytes)) {
        free(c);
        c = NULL;
        goto out;
    }
    g_dvm_jit_in_place += e.in_place;
    if (g_jit_trace)
        mg_info("jit: %s.%s%s compiled: %u instructions, %u of them handed back, %u calls (%u done in the code), %u loops entered%s, %u bytes of Thumb-2",
                m->cls->name, m->name, m->sig, (unsigned)(compiled + exits), (unsigned)exits, (unsigned)calls, (unsigned)e.in_place, (unsigned)loops,
                start ? " and its start" : "", (unsigned)bytes);
out:
    free(code_at);
    free(exit_at);
    free(back_at);
    free(throw_at);
    free(target);
    free(handback);
    free(e.b);
    free(e.fx);
    return c;
}

static mg_lock g_jit_lock = MG_LOCK_INIT;

void mg_dvm_jit_census(mg_dvm_jit_stats *out)
{
    mg_lock_acquire(&g_jit_lock);
    out->methods = (u32)g_jit_methods + g_nwaiting;
    out->waiting = g_nwaiting;
    out->refused = (u32)g_jit_refused;
    out->code_bytes = g_code_bytes;
    out->page_bytes = g_page_bytes;
    mg_lock_release(&g_jit_lock);
    out->calls_fast = g_dvm_calls_fast;
    out->calls_declined = g_dvm_calls_declined;
    out->calls_general = g_dvm_calls_general;
}

/*
 * Under g_jit_lock: the code of the methods that do calls in place (or of all of them) forgotten, the methods interpreted
 * until they are hot again and compiled anew. The code itself stays where it is, never freed: a thread may be running it
 * (a frame that is, or that called out from it, goes on in it until it hands the frame back or returns). Those waiting for
 * their page are taken off the list of it, unpublished. How many methods' code it was.
 */
static u32 forget(bool all)
{
    u32 i, kept = 0, n = 0, published = 0;
    for (i = 0; i < g_ncompiled; i++) {
        mg_dvm_method *m = g_compiled[i];
        mg_dvm_jit_code *c = m->jit;
        if (!c) continue;
        if (!all && !c->in_place) {
            g_compiled[kept++] = m;
            continue;
        }
        m->jit = NULL;
        m->jit_backs = 0;
        m->jit_state = 0;
        published++;
    }
    g_ncompiled = kept;
    for (i = 0, kept = 0; i < g_nwaiting; i++) {
        if (!all && !g_waiting[i].c->in_place) {
            g_waiting[kept++] = g_waiting[i];
            continue;
        }
        g_waiting[i].m->jit_backs = 0;
        g_waiting[i].m->jit_state = 0;
        free(g_waiting[i].c);
        n++;
    }
    g_nwaiting = kept;
    mg_atomic_add(&g_jit_methods, -(i32)published);
    return n + published;
}

u32 mg_dvm_jit_hooks_on(void)
{
    u32 n = 0;
    bool first = false;
    if (g_jit_hooks) return 0;
    mg_lock_acquire(&g_jit_lock);
    if (!g_jit_hooks) {
        g_jit_hooks = true;
        first = true;
        n = forget(false);
    }
    mg_lock_release(&g_jit_lock);
    if (first)
        mg_info("jit: the app hooks methods: a call compiled to be done in place looks at its method's hook first, and the code of %u methods "
                "compiled before, whose calls do not, is forgotten (compiled again when hot)",
                (unsigned)n);
    return n;
}

u32 mg_dvm_jit_forget_all(void)
{
    u32 n;
    mg_lock_acquire(&g_jit_lock);
    n = forget(true);
    mg_lock_release(&g_jit_lock);
    return n;
}

int mg_dvm_jit_enter(mg_dvm_thread *t, mg_dvm_frame *f, mg_dvm_method *m, mg_dvm_reg *regs, u32 pc, u32 *next, mg_jvalue *res)
{
    mg_dvm_jit_code *c = m->jit;
    u32 at;
    if (!c) {
        if (m->jit_state == 1 || m->jit_state == 2) return 0;
        mg_lock_acquire(&g_jit_lock);
        if (!m->jit_state && !compile(m)) refuse(m);
        /* Its page, and the methods' with it, made executable once they have waited long enough. */
        if (g_nwaiting && mg_time_monotonic_ns() - g_fill_since >= JIT_WAIT_NS) fill_done();
        /* Still waiting: looked at again after as many more calls and jumps back. */
        if (m->jit_state == 3) m->jit_backs = 0;
        mg_lock_release(&g_jit_lock);
        c = m->jit;
        if (!c) return 0;
    }
    if (!(at = mg_dvm_jit_entry(c, pc))) return 0;
    return mg_dvm_jit_run(t, f, c, regs, at, next, res);
}

#else

void mg_dvm_jit_start(bool on, u32 threshold, bool trace)
{
    (void)on;
    (void)threshold;
    g_jit_trace = trace;
    g_dvm_jit_threshold = 0;
}

void mg_dvm_jit_census(mg_dvm_jit_stats *out) { memset(out, 0, sizeof(*out)); }
u32 mg_dvm_jit_hooks_on(void) { return 0; }
u32 mg_dvm_jit_forget_all(void) { return 0; }

int mg_dvm_jit_enter(mg_dvm_thread *t, mg_dvm_frame *f, mg_dvm_method *m, mg_dvm_reg *regs, u32 pc, u32 *next, mg_jvalue *res)
{
    (void)t;
    (void)f;
    (void)regs;
    (void)pc;
    (void)next;
    (void)res;
    m->jit_state = 2;
    return 0;
}

#endif
