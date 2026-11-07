/*
 * The JIT: a method's bytecode compiled to machine code once its loops are hot (docs/JIT.md).
 *
 * The code works on the frame's own registers, in memory, as the interpreter does: one machine register holds their
 * address throughout, every instruction loads what it reads from them and stores what it writes, so that at any
 * instruction the code can stop and hand the frame back -- "go on at this pc" -- and the interpreter goes on with nothing
 * to rebuild. Whatever the compiler does not do (exceptions, the rarer instructions) is such a hand-back, an exit, and so
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
 *     returns 0 with ctx->pc where the interpreter goes on, 1 with ctx->pc where the exception pending was thrown, 2 with
 *     the method's value in ctx->result.
 *
 * This file is the front end, which every processor shares: the walk over the bytecode, what is compiled and what is
 * handed back, branches and their targets, the code's memory and its publishing. What each instruction becomes is a back
 * end's (the emit_* functions, and the branches they write): Thumb-2 on 32-bit ARM, the phone and the ARM tests under
 * qemu-arm (mg_dvm_jit_thumb2.h); A64 on ARM64, desktop Windows on a Snapdragon and the arm64 tests under qemu-aarch64
 * (mg_dvm_jit_a64.h); x86-64 under System V where the core is of Android's ABI, desktop Windows' core and the Linux tests
 * (mg_dvm_jit_x86.h). Where there is no back end (MG_DVM_JIT), nothing is compiled.
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

#if MG_DVM_JIT

/* An array's elements and length are where they are whichever kind it is: one load each in the code. */
typedef char jit_array_data_same[offsetof(mg_jobj, u.arr.data) == offsetof(mg_jobj, u.oarr.v) ? 1 : -1];
typedef char jit_array_len_same[offsetof(mg_jobj, u.arr.len) == offsetof(mg_jobj, u.oarr.len) ? 1 : -1];

/* ------------------------------------------------------------------ the emitter */

/* What the code is written in units of: Thumb-2's halfwords, A64's words, x86's bytes. */
#if MG_ARCH_ARM32
typedef u16 jit_unit;
#elif MG_ARCH_ARM64
typedef u32 jit_unit;
#else
typedef u8 jit_unit;
#endif

typedef struct fixup {
    u32 at;                            /* the branch's first unit */
    u32 pc;                            /* what it goes to: a pc's code, its exit or its jump back */
    u8 kind, cond;                     /* FIX_*; the condition (C_AL: always) */
} fixup;

/*
 * To a pc's code, its exit, its jump back's check, the way out; for a call that threw, its pc's throw and the way out with
 * it; and the way out of a return.
 */
enum { FIX_CODE, FIX_EXIT, FIX_BACK, FIX_EPILOGUE, FIX_THROW, FIX_THROW_EPILOGUE, FIX_RETURN };
/* A branch's condition, as ARM numbers them; a back end for another processor turns them into its own. */
enum { C_EQ = 0, C_NE = 1, C_HS = 2, C_LO = 3, C_MI = 4, C_PL = 5, C_HI = 8, C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13, C_AL = 14 };

typedef struct emitter {
    jit_unit *b;
    u32 n, cap;
    fixup *fx;
    u32 nfx, capfx;
    u32 in_place;                      /* calls of trivial methods done in the code (inline_call) */
    bool failed;
} emitter;

static void put_unit(emitter *e, u32 u)
{
    if (e->n == e->cap) {
        u32 cap = e->cap ? e->cap * 2 : 1024;
        jit_unit *b = (jit_unit *)realloc(e->b, sizeof(jit_unit) * cap);
        if (!b) { e->failed = true; return; }
        e->b = b;
        e->cap = cap;
    }
    e->b[e->n++] = (jit_unit)u;
}

/*
 * The back end's: how many units a branch on that condition takes (its place is kept until the target is known), and the
 * branch at unit `at` to unit `to` written there -- false when it does not reach.
 */
static u32 branch_units(u8 cond);
static bool put_branch(jit_unit *b, u32 at, u32 to, u8 cond);

static void branch_space(emitter *e, u8 cond)
{
    u32 k, n = branch_units(cond);
    for (k = 0; k < n; k++) put_unit(e, 0);
}

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
    branch_space(e, cond);             /* the branch, written once its target is known */
}

/* A branch inside one instruction's code: its place kept, and written with its condition once its label is reached (local_to). */
static u32 local_branch(emitter *e, u8 cond)
{
    u32 at = e->n;
    branch_space(e, cond);
    return at;
}
static void local_to(emitter *e, u32 at, u8 cond)
{
    if (!e->failed && !put_branch(e->b, at, e->n, cond)) e->failed = true;
}

/* ------------------------------------------------------------------ what the front end asks of a back end */

/* The data processing of int arithmetic, rd = rn op rm (RSB: rm - rn). */
enum { OP_ADD, OP_SUB, OP_RSB, OP_MUL, OP_DIV, OP_REM, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR };

/*
 * What an iget, iput, sget, sput, aget or aput moves, and a getter or a setter done in place: an int (or a float's bits),
 * a boolean, a byte, a char, a short (the five as Thumb-2's loads and stores of them are numbered), a long or a double, a
 * reference.
 */
enum { T_INT, T_BOOLEAN, T_BYTE, T_CHAR, T_SHORT, T_WIDE, T_OBJECT };

/* The condition of if-eq, if-ne, if-lt, if-ge, if-gt, if-le (and of their -z forms), from 0 on. */
static u8 if_cond(u32 op)
{
    static const u8 conds[] = { C_EQ, C_NE, C_LT, C_GE, C_GT, C_LE };
    return conds[op];
}

/*
 * A call done in the code (inline_call): the method the invoke names, its kind, its arguments' registers (the object's
 * first) and how many; the intrinsic, or the trivial shape with its field and what that moves (T_*) at `off` in the object
 * (0: a static getter's slot); and the checks before it -- the method's hook mark, the object's class's vtable slot, the
 * object's having the field's slot. Each that fails goes by mg_dvm_jit_invoke.
 */
typedef struct inline_plan {
    const mg_dvm_method *target;
    int kind;
    u32 count, self, value;
    u8 intrinsic;
    u32 shape;
    const mg_dvm_field *fld;
    int type;
    u32 off;
    bool hooks, vtab, slots;
} inline_plan;

#if MG_ARCH_ARM32
#include "mg_dvm_jit_thumb2.h"
#elif MG_ARCH_ARM64
#include "mg_dvm_jit_a64.h"
#else
#include "mg_dvm_jit_x86.h"
#endif

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

/* A branch to pc `to` from the instruction at `pc`: a jump back goes by its target's jump-back stub (the slow flag). */
static void branch_to(emitter *e, u32 pc, u32 to, u8 cond) { fix(e, to <= pc ? FIX_BACK : FIX_CODE, to, cond); }

/* What each of iget..iget-short moves; and iput.., sget.., sput.., aget.., aput.. in the same order. */
static const u8 k_field_type[7] = { T_INT, T_WIDE, T_OBJECT, T_BOOLEAN, T_BYTE, T_CHAR, T_SHORT };

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
    u32 off;
    if (!fld || (fld->flags & DVM_ACC_STATIC)) return false;
    off = (u32)MG_DVM_HEADER + fld->slot * (u32)sizeof(mg_jvalue);
    if (off + 4 >= 4096 || fld->slot > 0xFFFF) return false;
    emit_instance_field(e, pc, store, k_field_type[op - (store ? 0x59 : 0x52)], a4, b4, fld->slot, off);
    return true;
}

/* sget and sput of a field of a class initialised by now: its slot's address is the code's own constant. */
static bool static_field(emitter *e, const mg_dvm_method *m, u32 op, u32 aa, u32 idx)
{
    mg_dvm_dex *d = m->cls->dex;
    mg_dvm_field *fld = idx < d->nfield ? d->fcache[idx] : NULL;
    bool store = op >= 0x67;
    if (!fld || !(fld->flags & DVM_ACC_STATIC) || fld->cls->state != DVM_CS_INITIALIZED || !fld->cls->statics) return false;
    emit_static_field(e, store, k_field_type[op - (store ? 0x67 : 0x60)], aa, &fld->cls->statics[fld->slot]);
    return true;
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
    if (op == 0x2b) {
        emit_switch_value(e, va, true, rd32(p + 2));
        for (i = 0; i < n; i++) {
            i32 to = rd32(p + 4 + 2 * i);
            emit_switch_key(e, (i32)i);
            branch_to(e, pc, (u32)((i32)pc + to), C_EQ);
        }
    } else {
        emit_switch_value(e, va, false, 0);
        for (i = 0; i < n; i++) {
            i32 key = rd32(p + 2 + 2 * i), to = rd32(p + 2 + 2 * n + 2 * i);
            emit_switch_key(e, key);
            branch_to(e, pc, (u32)((i32)pc + to), C_EQ);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ calls */

/*
 * An invoke tried the quick way first: mg_dvm_jit_fast(ctx, m, desc, pc), the method resolved here once (the interpreter's
 * calls resolved it before the loop was compiled) and the arguments' registers packed into `desc` (mg_dvm_internal.h). A
 * static or direct call, or a virtual one of a method nothing overrides (final, or of a final class), names the method
 * itself; another virtual call, or an interface call, the one to look up by the object's class. 1 back is an exception
 * pending (out, thrown here); 3, a call it did not make, goes the general way (emit_invoke: mg_dvm_jit_invoke). Not for
 * invoke-super, a String constructor (new String's result replaces the object), a static method of a class not initialised
 * by now, a call the flag no-fast-calls leaves out.
 */
static int g_fast_calls = -1;          /* (under g_jit_lock, as compile is) */
/* An app hooks methods (mg_dvm_hook.c, mg_dvm_jit_hooks_on): a call done in place looks at its method's mark first (inline_call). */
static volatile bool g_jit_hooks;

static void fast_invoke(emitter *e, const mg_dvm_method *m, u32 pc)
{
    const u16 *insns = m->insns;
    mg_dvm_dex *d = m->cls->dex;
    u16 w = insns[pc];
    u32 op = w & 0xFF, idx = insns[pc + 1], count, desc, kind;
    bool range = op >= 0x74, known;
    int ik = (int)(op - (range ? 0x74u : 0x6eu));
    const mg_dvm_method *ref = idx < d->nmeth ? d->mcache[idx] : NULL;
    if (g_fast_calls < 0) {
        g_fast_calls = !mg_flag("no-fast-calls");
        mg_info("jit: %s", g_fast_calls ? "calls into compiled code go the quick way where they can (mg_dvm_jit_fast)"
                                        : "the flag no-fast-calls: every call goes the general way");
    }
    if (!g_fast_calls || !ref || ((ref->flags & DVM_ACC_STATIC) != 0) != (ik == DVM_INVOKE_STATIC)) {
        emit_invoke(e, pc);
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
        emit_invoke(e, pc);
        return;
    }
    /*
     * A method named here that is known by now never to go the quick way goes the general way at once (String.equals and
     * hashCode, intrinsics, cost half as much again tried first): an intrinsic, a hooked one, a native method, one with
     * handlers or synchronized, one compiled with no entry at its start.
     */
    known = kind == DVM_FAST_STATIC || kind == DVM_FAST_DIRECT;
    if (known && (ref->intrinsic || ref->hooked || !ref->insns || ref->ntries || (ref->flags & DVM_ACC_SYNCHRONIZED) || (ref->jit && !ref->jit->fast))) {
        emit_invoke(e, pc);
        return;
    }
    emit_fast_invoke(e, pc, ref, desc | kind << 28, known);
}

/*
 * A call of a method whose whole body is trivial (mg_dvm_interp.c, "trivial methods": empty, a constant returned, a getter,
 * a setter, a static getter), done in the code as the interpreter's call does it in place, without mg_dvm_jit_invoke and
 * its C call: the method the invoke names, which the interpreter's calls resolved, and its shape, which its first call
 * found (the loop ran interpreted before it was compiled). A virtual call checks that the object's class has that very
 * method in its slot. An object that is null, of a class that overrides the method, or without the getter's or setter's
 * field slot goes by mg_dvm_jit_invoke, which throws or runs the method as the interpreter would. So do the intrinsics that
 * are a few instructions (mg_dvm_interp.c, "intrinsics"): String.length, isEmpty and charAt, Math.min, max and abs of ints,
 * Math.sqrt, Reference.getReferent -- a null string or an index out of its bounds by the call, which throws. False, with
 * nothing emitted: not such a call, which goes by mg_dvm_jit_invoke whole -- a hooked method's too (mg_dvm_hook.c). Once an
 * app hooks methods, a method may be hooked after the call is compiled: the call looks at its mark first, and a hooked one
 * goes by mg_dvm_jit_invoke, which calls its hook (code compiled before, without that look, was forgotten then: forget).
 */
static bool inline_call(emitter *e, const mg_dvm_method *m, u32 pc)
{
    const u16 *insns = m->insns;
    mg_dvm_dex *d = m->cls->dex;
    u16 w = insns[pc];
    u32 op = w & 0xFF, idx = insns[pc + 1], sop;
    bool range = op >= 0x74;
    inline_plan p;
    memset(&p, 0, sizeof(p));
    p.kind = (int)(op - (range ? 0x74u : 0x6eu));
    if (p.kind != DVM_INVOKE_VIRTUAL && p.kind != DVM_INVOKE_DIRECT && p.kind != DVM_INVOKE_STATIC) return false;
    if (range) {
        p.count = (u32)w >> 8;
        p.self = insns[pc + 2];
        p.value = p.self + 1;
    } else {
        p.count = (u32)w >> 12;
        p.self = insns[pc + 2] & 0xFu;
        p.value = ((u32)insns[pc + 2] >> 4) & 0xFu;
    }
    p.target = idx < d->nmeth ? d->mcache[idx] : NULL;
    if (!p.target || ((p.target->flags & DVM_ACC_STATIC) != 0) != (p.kind == DVM_INVOKE_STATIC)) return false;
    if ((p.target->cls->flags & DVM_ACC_INTERFACE) || p.target->stub || p.target->hooked) return false;
    if (p.kind == DVM_INVOKE_STATIC ? p.target->cls->state != DVM_CS_INITIALIZED : !p.count) return false;
    p.intrinsic = p.target->intrinsic;
    p.shape = p.target->shape;
    sop = SHAPE_OP(p.shape);
    if (p.intrinsic) {
        u8 id = p.intrinsic;
        if (id != INTR_STRING_LENGTH && id != INTR_STRING_ISEMPTY && id != INTR_STRING_CHARAT && id != INTR_MIN_I && id != INTR_MAX_I
            && id != INTR_ABS_I && id != INTR_SQRT && id != INTR_REFERENT)
            return false;
        if (id == INTR_REFERENT && (g_dvm_referent_slot == ~0u || (u32)MG_DVM_HEADER + g_dvm_referent_slot * (u32)sizeof(mg_jvalue) + 4 >= 4096))
            return false;
        if ((id == INTR_STRING_CHARAT || id == INTR_MIN_I || id == INTR_MAX_I || id == INTR_SQRT) && p.count < 2) return false;
    } else if (!p.target->insns) {
        return false;
    } else switch (SHAPE_OF(p.shape)) {
    case SHAPE_EMPTY: case SHAPE_CONST: break;
    case SHAPE_STATIC_GETTER:
        p.fld = p.target->shape_field;
        if (!p.fld || p.fld->cls->state != DVM_CS_INITIALIZED || !p.fld->cls->statics) return false;
        p.type = k_field_type[sop - 0x60];
        break;
    case SHAPE_GETTER: case SHAPE_SETTER:
        p.fld = p.target->shape_field;
        if (!p.fld || p.fld->slot > 0xFFFF || (SHAPE_OF(p.shape) == SHAPE_SETTER && p.count < 2)) return false;
        p.off = (u32)MG_DVM_HEADER + p.fld->slot * (u32)sizeof(mg_jvalue);
        if (p.off + 4 >= 4096) return false;
        p.type = k_field_type[sop - (SHAPE_OF(p.shape) == SHAPE_SETTER ? 0x59 : 0x52)];
        break;
    default: return false;
    }
    p.hooks = g_jit_hooks;
    /* (Not for a final method, or one of a final class such as String: nothing can override it.) */
    p.vtab = p.kind == DVM_INVOKE_VIRTUAL && p.target->vidx >= 0 && !((p.target->flags | p.target->cls->flags) & DVM_ACC_FINAL);
    p.slots = !p.intrinsic && p.fld && SHAPE_OF(p.shape) != SHAPE_STATIC_GETTER;
    emit_inline_call(e, pc, &p);
    e->in_place++;
    return true;
}

/*
 * check-cast and instance-of: in the code when the object is null or of the very class named (resolved by now: its pointer a
 * constant), mg_dvm_jit_op for the rest (a subclass, an interface, a class not yet resolved).
 */
static void compile_type_test(emitter *e, const mg_dvm_method *m, u32 pc, u32 op, u32 vo, u32 vd, u32 idx)
{
    mg_dvm_dex *d = m->cls->dex;
    mg_dvm_class *c = idx < d->ntype ? d->tcache[idx] : NULL;
    if (!c) emit_runtime(e, pc);
    else emit_type_test(e, pc, op == 0x20, vo, vd, c);
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
    case 0x01: case 0x07: emit_move(e, a4, b4); return true;
    case 0x02: case 0x08: emit_move(e, aa, F(1)); return true;
    case 0x03: case 0x09: emit_move(e, F(1), F(2)); return true;
    case 0x04: emit_move_wide(e, a4, b4); return true;
    case 0x05: emit_move_wide(e, aa, F(1)); return true;
    case 0x06: emit_move_wide(e, F(1), F(2)); return true;
    case 0x0e: case 0x0f: case 0x10: case 0x11:                  /* the returns: the value for ctx->result */
        emit_return(e, op == 0x0e ? -1 : op == 0x10 ? T_WIDE : op == 0x11 ? T_OBJECT : T_INT, aa);
        fix(e, FIX_RETURN, pc, C_AL);
        return true;
    case 0x0a: emit_move_result(e, T_INT, aa); return true;     /* move-result, -wide, -object: a compiled call's */
    case 0x0b: emit_move_result(e, T_WIDE, aa); return true;
    case 0x0c: emit_move_result(e, T_OBJECT, aa); return true;
    case 0x16: case 0x17: case 0x18: case 0x19: {               /* const-wide/16, /32, const-wide, /high16 */
        u64 v = op == 0x16 ? (u64)(i64)(i16)F(1)
                : op == 0x17 ? (u64)(i64)(i32)(F(1) | F(2) << 16)
                : op == 0x18 ? (u64)F(1) | (u64)F(2) << 16 | (u64)F(3) << 32 | (u64)F(4) << 48
                             : (u64)F(1) << 48;
        emit_const_wide(e, aa, v);
        return true;
    }
    case 0x1a: case 0x1b: {                                      /* const-string, /jumbo: one already made (interned, never freed) */
        mg_dvm_dex *d = m->cls->dex;
        u32 idx = op == 0x1a ? F(1) : F(1) | F(2) << 16;
        mg_jobj *s = idx < d->nstr ? d->scache[idx] : NULL;
        if (!s) return false;
        emit_const_object(e, aa, s);
        return true;
    }
    case 0x1f: compile_type_test(e, m, pc, op, aa, 0, F(1)); return true;      /* check-cast vAA */
    case 0x20: compile_type_test(e, m, pc, op, b4, a4, F(1)); return true;     /* instance-of vA, vB */
    case 0x1c: case 0x1d: case 0x1e: case 0x22: case 0x23: case 0x4d:
        emit_runtime(e, pc);
        return true;
    case 0x2b: case 0x2c: return compile_switch(e, m, pc, op, aa, (i32)(F(1) | F(2) << 16));
    case 0x31: emit_cmp_long(e, aa, F(1) & 0xFF, F(1) >> 8); return true;
    case 0x2d: case 0x2e: case 0x2f: case 0x30:                  /* cmpl-float, cmpg-float, cmpl-double, cmpg-double */
        emit_fp_compare(e, op >= 0x2f, op == 0x2e || op == 0x30, aa, F(1) & 0xFF, F(1) >> 8);
        return true;
    case 0x7f: case 0x80: case 0x82: case 0x83: case 0x87: case 0x89: case 0x8a: case 0x8c:
        return emit_fp_convert(e, op, a4, b4);
    case 0x85: case 0x86: case 0x88: case 0x8b:                  /* long-to-float, -double; float-, double-to-long */
        emit_long_convert(e, op, a4, b4);
        return true;
    case 0x9e: case 0x9f: emit_long_divide(e, pc, op == 0x9f, aa, F(1) & 0xFF, F(1) >> 8); return true;     /* div-long, rem-long */
    case 0xbe: case 0xbf: emit_long_divide(e, pc, op == 0xbf, a4, a4, b4); return true;                       /* and /2addr */
    case 0xaa: case 0xaf: emit_fp_remainder(e, op == 0xaf, aa, F(1) & 0xFF, F(1) >> 8); return true;    /* rem-float, rem-double */
    case 0xca: case 0xcf: emit_fp_remainder(e, op == 0xcf, a4, a4, b4); return true;                     /* and /2addr */
    case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:
        if (!inline_call(e, m, pc)) fast_invoke(e, m, pc);
        return true;
    case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x81: case 0x84: case 0x8d: case 0x8e: case 0x8f:
        emit_unary(e, op, a4, b4);     /* neg and not of ints and longs, int-to-long, long-to-int, int-to-byte, -char, -short */
        return true;
    case 0x12: emit_const(e, a4, (u32)((i32)(i16)w >> 12)); return true;
    case 0x13: emit_const(e, aa, (u32)(i32)(i16)F(1)); return true;
    case 0x14: emit_const(e, aa, F(1) | F(2) << 16); return true;
    case 0x15: emit_const(e, aa, F(1) << 16); return true;
    case 0x21: emit_array_length(e, pc, a4, b4); return true;
    case 0x28: branch_to(e, pc, (u32)((i32)pc + (i8)(w >> 8)), C_AL); return true;
    case 0x29: branch_to(e, pc, (u32)((i32)pc + (i16)F(1)), C_AL); return true;
    case 0x2a: branch_to(e, pc, (u32)((i32)pc + (i32)(F(1) | F(2) << 16)), C_AL); return true;
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
        u8 cond = emit_compare(e, op, a4, b4);
        branch_to(e, pc, (u32)((i32)pc + (i16)F(1)), cond);
        return true;
    }
    case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
        u8 cond = emit_compare_zero(e, op, aa);
        branch_to(e, pc, (u32)((i32)pc + (i16)F(1)), cond);
        return true;
    }
    case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:     /* aget, -wide, -object, -boolean, -byte, -char, -short */
        emit_array(e, pc, false, k_field_type[op - 0x44], aa, F(1) & 0xFF, F(1) >> 8);
        return true;
    case 0x4b: case 0x4c: case 0x4e: case 0x4f: case 0x50: case 0x51:                /* aput of all but references (above, the runtime's) */
        emit_array(e, pc, true, k_field_type[op - 0x4b], aa, F(1) & 0xFF, F(1) >> 8);
        return true;
    case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:     /* iget, -wide, -object, -boolean, -byte, -char, -short */
    case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:     /* iput, and the same */
        return instance_field(e, m, pc, op, a4, b4, F(1));
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:     /* sget, and the same */
    case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d:     /* sput, and the same */
        return static_field(e, m, op, aa, F(1));
    default:
        break;
    }
    if (op >= 0x90 && op <= 0x9a) {                              /* binop vAA, vBB, vCC */
        emit_arith(e, pc, bin_op(op - 0x90), aa, F(1) & 0xFF, F(1) >> 8);
        return true;
    }
    if (op >= 0x9b && op <= 0xa5) return emit_long(e, op - 0x9b, aa, F(1) & 0xFF, F(1) >> 8);    /* long binop vAA, vBB, vCC */
    if ((op >= 0xa6 && op <= 0xa9) || (op >= 0xab && op <= 0xae)) {                             /* float and double binop vAA, vBB, vCC */
        emit_fp_arith(e, (op - 0xa6) % 5, op >= 0xab, aa, F(1) & 0xFF, F(1) >> 8);
        return true;
    }
    if ((op >= 0xc6 && op <= 0xc9) || (op >= 0xcb && op <= 0xce)) {                             /* their /2addr vA, vB */
        emit_fp_arith(e, (op - 0xc6) % 5, op >= 0xcb, a4, a4, b4);
        return true;
    }
    if (op >= 0xbb && op <= 0xc5) return emit_long(e, op - 0xbb, a4, a4, b4);                  /* long binop/2addr vA, vB */
    if (op >= 0xb0 && op <= 0xba) {                              /* binop/2addr vA, vB */
        emit_arith(e, pc, bin_op(op - 0xb0), a4, a4, b4);
        return true;
    }
    if (op >= 0xd0 && op <= 0xd7) {                              /* binop/lit16 vA, vB, #+CCCC */
        int o = lit16_op(op - 0xd0);
        i32 lit = (i16)F(1);
        if ((o == OP_DIV || o == OP_REM) && lit == 0) return false;
        emit_arith_lit(e, o, a4, b4, lit);
        return true;
    }
    if (op >= 0xd8 && op <= 0xe2) {                              /* binop/lit8 vAA, vBB, #+CC */
        int o = lit8_op(op - 0xd8);
        i32 lit = (i8)(F(1) >> 8);
        if ((o == OP_DIV || o == OP_REM) && lit == 0) return false;
        emit_arith_lit(e, o, aa, F(1) & 0xFF, lit);
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
        if (at) c->fast = MG_DVM_JIT_AT(c, at);
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
static bool place(mg_dvm_method *m, mg_dvm_jit_code *c, const jit_unit *code, size_t bytes)
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
        c->run = (int (*)(mg_dvm_jit_ctx *, const void *))(void *)((mg_addr)p | JIT_CODE_BIT);
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
    c->run = (int (*)(mg_dvm_jit_ctx *, const void *))(void *)((mg_addr)p | JIT_CODE_BIT);
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
    /* The way in, which goes on at its second argument: the entry. */
    emit_prologue(&e);
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
            emit_exit(&e, pc);
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
    /* The exits of the checks and the jump-back stubs, after the code: out at the instruction's pc. (Each instruction's
       code goes on into the next one's; the last never ends that way, which bytecode cannot.) */
    for (i = 0; i < e.nfx; i++) {
        u32 at = e.fx[i].pc;
        if (e.fx[i].kind == FIX_EXIT && !exit_at[at]) {
            exit_at[at] = e.n + 1;
            emit_exit(&e, at);
        } else if (e.fx[i].kind == FIX_THROW && !throw_at[at]) {
            /* A call that threw: out with the exception pending, at the invoke. */
            throw_at[at] = e.n + 1;
            emit_throw_exit(&e, at);
        } else if (e.fx[i].kind == FIX_BACK && !back_at[at] && at < m->ninsns) {
            /* A jump back: on to the target, unless the instructions are watched (g_dvm.slow): then out at the target. */
            back_at[at] = e.n + 1;
            emit_back_check(&e, at);
        }
    }
    /* The ways out: ctx->pc set, 0 returned; and with an exception pending, 1; and from a return, its value in ctx->result, 2. */
    emit_epilogues(&e, &epilogue, &thrown, &returned);
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
    bytes = (size_t)e.n * sizeof(jit_unit);
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
        mg_info("jit: %s.%s%s compiled: %u instructions, %u of them handed back, %u calls (%u done in the code), %u loops entered%s, %u bytes of %s",
                m->cls->name, m->name, m->sig, (unsigned)(compiled + exits), (unsigned)exits, (unsigned)calls, (unsigned)e.in_place, (unsigned)loops,
                start ? " and its start" : "", (unsigned)bytes, JIT_ISA);
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

void mg_dvm_jit_start(bool on, u32 threshold, bool trace)
{
    const char *note = jit_processor();
    g_dvm_jit_threshold = on ? (threshold ? threshold : 1) : 0;
    g_jit_trace = trace;
    mg_info("jit: %s%s", on ? "methods whose loops are hot are compiled to " JIT_ISA " (docs/JIT.md)" : "off: everything is interpreted", on ? note : "");
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
