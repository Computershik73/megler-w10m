/*
 * The JIT: a method's bytecode compiled to Thumb-2 once its loops are hot (docs/JIT.md).
 *
 * The code works on the frame's own registers, in memory, as the interpreter does: r4 holds them throughout, every
 * instruction loads what it reads from them and stores what it writes, so that at any instruction the code can stop and
 * hand the frame back -- "go on at this pc" -- and the interpreter goes on with nothing to rebuild. Whatever the compiler
 * does not do (calls, returns, allocation, exceptions, wide and floating-point values, fields for now) is such a hand-back,
 * an exit, and so is anything that would throw: a null or an index out of bounds sends the frame back to the instruction
 * that throws, and the interpreter throws it. No safepoints: the collector stops threads from outside and reads their
 * registers and stacks conservatively, and moves nothing (mg_dvm_gc.c).
 *
 * Entered from the interpreter's plain loop at a jump back (mg_dvm_jit_back), at the jump's target: every branch target is
 * an entry, and pc 0. A jump back in the code looks at g_dvm.slow first and hands the frame back at the target when
 * anything watches the instructions (the watched loop runs then).
 *
 *   int code(mg_dvm_reg *regs, const void *entry, u32 *pc_out, const volatile i32 *slow)
 *     r4 regs, r6 pc_out, r7 slow; returns 0 with *pc_out where the interpreter goes on.
 *
 * Only where the host runs Thumb-2: the phone, and the ARM tests under qemu-arm. Elsewhere nothing is compiled.
 */
#include "mg_dvm_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 g_dvm_jit_threshold;
static bool g_jit_trace;
static volatile i32 g_jit_methods, g_jit_refused;

#if defined(__arm__) || defined(_M_ARM)

/* An array's elements and length are where they are whichever kind it is: one load each in the code. */
typedef char jit_array_data_same[offsetof(mg_jobj, u.arr.data) == offsetof(mg_jobj, u.oarr.v) ? 1 : -1];
typedef char jit_array_len_same[offsetof(mg_jobj, u.arr.len) == offsetof(mg_jobj, u.oarr.len) ? 1 : -1];
typedef char jit_register_word[sizeof(mg_dvm_reg) == 4 ? 1 : -1];

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

enum { FIX_CODE, FIX_EXIT, FIX_BACK, FIX_EPILOGUE };
enum { C_EQ = 0, C_NE = 1, C_HS = 2, C_LO = 3, C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13, C_AL = 14 };

typedef struct emitter {
    u16 *b;
    u32 n, cap;
    fixup *fx;
    u32 nfx, capfx;
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

/* Registers: r0 to r3 for values, r4 the frame's registers, r6 pc_out, r7 the slow flag. */
enum { R0, R1, R2, R3, R4, R5, R6, R7 };

static void ldr_v(emitter *e, int rt, u32 v) { e32(e, 0xF8D0 | R4, ((u32)rt << 12) | (v * 4)); }
static void str_v(emitter *e, int rt, u32 v) { e32(e, 0xF8C0 | R4, ((u32)rt << 12) | (v * 4)); }
static void ldr_imm(emitter *e, int rt, int rn, u32 off) { e32(e, 0xF8D0 | (u32)rn, ((u32)rt << 12) | off); }
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
 * Memory for code: pages of an arena of the process's, reserved a megabyte at a time and committed as code comes, each
 * method's code on pages of its own (written, then made executable once: the phone never has a page both). A method on
 * an allocation of its own took 64 KB of addresses (Windows' granularity) for a few hundred bytes of code, and Yandex
 * Maps has addresses to spare for nothing. Under g_jit_lock.
 */
#define JIT_ARENA (1u << 20)
static u8 *g_arena;
static size_t g_arena_used;

static u8 *code_pages(size_t bytes)
{
    size_t need = (bytes + MG_PAGE - 1) & ~(size_t)(MG_PAGE - 1);
    u8 *p;
    if (need > JIT_ARENA / 4) return (u8 *)mg_mem_alloc(need);
    if (!g_arena || g_arena_used + need > JIT_ARENA) {
        u8 *a = (u8 *)mg_mem_reserve(JIT_ARENA);
        if (!a) return NULL;
        g_arena = a;
        g_arena_used = 0;
    }
    p = g_arena + g_arena_used;
    if (!mg_mem_commit(p, need, MG_PROT_RW)) return NULL;
    g_arena_used += need;
    return p;
}

static mg_dvm_jit_code *compile(mg_dvm_method *m)
{
    emitter e;
    u32 *code_at = NULL, *exit_at = NULL, *back_at = NULL, pc, i, epilogue, compiled = 0, exits = 0, loops = 0;
    u8 *target = NULL, *handback = NULL;
    mg_dvm_jit_code *c = NULL;
    size_t bytes;
    u8 *pages;
    if (!m->insns || m->regs > 1000 || m->ninsns == 0 || m->ninsns > 65535) return NULL;
    memset(&e, 0, sizeof(e));
    code_at = (u32 *)calloc(m->ninsns, sizeof(u32));
    exit_at = (u32 *)calloc(m->ninsns, sizeof(u32));
    back_at = (u32 *)calloc(m->ninsns, sizeof(u32));
    target = (u8 *)calloc(m->ninsns, 1);
    handback = (u8 *)calloc(m->ninsns, 1);
    if (!code_at || !exit_at || !back_at || !target || !handback) goto out;
    mark_targets(m, target);
    /* The way in: push {r4-r7, lr}; r4 = regs, r6 = pc_out, r7 = slow; bx r1 (the entry). */
    e16(&e, 0xB5F0);
    e16(&e, 0x4604);
    e16(&e, 0x4616);
    e16(&e, 0x461F);
    e16(&e, 0x4708);
    for (pc = 0; pc < m->ninsns;) {
        u32 n = mg_dvm_insn_units(m->insns, m->ninsns, pc);
        if (!n) break;
        code_at[pc] = e.n + 1;
        if (compile_insn(&e, m, pc)) {
            compiled++;
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
    if (!loops) goto out;
    /* The exits of the checks and the jump-back stubs, after the code: r0 = the instruction's pc, to the way out. (Each
       instruction's code goes on into the next one's; the last never ends that way, which bytecode cannot.) */
    for (i = 0; i < e.nfx; i++) {
        u32 at = e.fx[i].pc;
        if (e.fx[i].kind == FIX_EXIT && !exit_at[at]) {
            exit_at[at] = e.n + 1;
            mov_imm(&e, R0, at);
            fix(&e, FIX_EPILOGUE, 0, C_AL);
        } else if (e.fx[i].kind == FIX_BACK && !back_at[at] && at < m->ninsns) {
            /* A jump back: on to the target, unless the instructions are watched (g_dvm.slow): then out at the target. */
            back_at[at] = e.n + 1;
            e16(&e, 0x6838);           /* ldr r0, [r7] */
            cmp_imm(&e, R0, 0);
            fix(&e, FIX_EXIT, at, C_NE);
            fix(&e, FIX_CODE, at, C_AL);
        }
    }
    /* The way out: *pc_out = r0; return 0. */
    epilogue = e.n;
    e16(&e, 0x6030);                   /* str r0, [r6] */
    e16(&e, 0x2000);                   /* movs r0, #0 */
    e16(&e, 0xBDF0);                   /* pop {r4-r7, pc} */
    if (e.failed) goto out;
    for (i = 0; i < e.nfx; i++) {
        fixup *x = &e.fx[i];
        u32 to;
        switch (x->kind) {
        case FIX_CODE: to = x->pc < m->ninsns ? code_at[x->pc] : 0; break;
        case FIX_EXIT: to = exit_at[x->pc]; break;
        case FIX_BACK: to = x->pc < m->ninsns ? back_at[x->pc] : 0; break;
        default: to = epilogue + 1; break;
        }
        /* A branch to the middle of an instruction or outside the method: not compiled. */
        if (!to || !put_branch(e.b, x->at, to - 1, x->cond)) goto out;
    }
    bytes = (size_t)e.n * 2;
    pages = code_pages(bytes);
    if (!pages) goto out;
    memcpy(pages, e.b, bytes);
    /* (Pages a failure leaves behind stay in the arena, unused: they are never given out again.) */
    if (!mg_mem_protect(pages, (bytes + MG_PAGE - 1) & ~(size_t)(MG_PAGE - 1), MG_PROT_RX)) goto out;
    mg_icache_flush(pages, bytes);
    c = (mg_dvm_jit_code *)calloc(1, sizeof(*c));
    if (!c) goto out;
    c->pages = pages;
    c->size = bytes;
    c->run = (int (*)(mg_dvm_reg *, const void *, u32 *, const volatile i32 *))(void *)((mg_addr)pages | 1);
    c->ninsns = m->ninsns;
    c->entry = code_at;
    code_at = NULL;
    for (pc = 0; pc < m->ninsns; pc++)
        if (!target[pc]) c->entry[pc] = 0;
    if (g_jit_trace)
        mg_info("jit: %s.%s%s compiled: %u instructions, %u of them handed back, %u loops entered, %u bytes of Thumb-2", m->cls->name,
                m->name, m->sig, (unsigned)(compiled + exits), (unsigned)exits, (unsigned)loops, (unsigned)bytes);
out:
    free(code_at);
    free(exit_at);
    free(back_at);
    free(target);
    free(handback);
    free(e.b);
    free(e.fx);
    return c;
}

static mg_lock g_jit_lock = MG_LOCK_INIT;

bool mg_dvm_jit_enter(mg_dvm_method *m, mg_dvm_reg *regs, u32 pc, u32 *next)
{
    mg_dvm_jit_code *c = m->jit;
    u32 at;
    if (!c) {
        if (m->jit_state) return false;
        mg_lock_acquire(&g_jit_lock);
        if (!m->jit_state) {
            c = compile(m);
            if (c) {
                /* Published whole: the code and its tables before the pointer (an interlocked exchange is a barrier). */
                mg_atomic_cas_ptr((void *volatile *)&m->jit, NULL, c);
                m->jit_state = 1;
                mg_atomic_add(&g_jit_methods, 1);
            } else {
                m->jit_state = 2;
                mg_atomic_add(&g_jit_refused, 1);
            }
        }
        mg_lock_release(&g_jit_lock);
        c = m->jit;
        if (!c) return false;
    }
    if (pc >= c->ninsns || !(at = c->entry[pc])) return false;
    c->run(regs, c->pages + (size_t)(at - 1) * 2 + 1, next, &g_dvm.slow);
    return true;
}

#else

void mg_dvm_jit_start(bool on, u32 threshold, bool trace)
{
    (void)on;
    (void)threshold;
    g_jit_trace = trace;
    g_dvm_jit_threshold = 0;
}

bool mg_dvm_jit_enter(mg_dvm_method *m, mg_dvm_reg *regs, u32 pc, u32 *next)
{
    (void)regs;
    (void)pc;
    (void)next;
    m->jit_state = 2;
    return false;
}

#endif
