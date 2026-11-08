/*
 * The JIT's A64 back end (docs/JIT.md, "the back ends"): what mg_dvm_jit.c's front end asks for, in AArch64's A64 under
 * AAPCS64 -- desktop Windows on ARM64 (a Snapdragon X), and the arm64 tests under qemu-aarch64. Included by mg_dvm_jit.c
 * alone, after its emitter and before its compiler.
 *
 * Registers: x19 the frame's registers (ctx->regs), x20 the context, x21 the slow flag's address, all three callee-saved
 * and so kept by the calls the code makes; x0-x3 and x9-x11 for values, x16 for the address of a function called, d0, d1
 * for floating point. Nothing is kept in them from one instruction to the next, and a reference only ever in x0-x3: the
 * collector, which reads x0-x28, fp and lr of a thread it stops but no vector register, finds each either there or in the
 * frame's registers. x18 is never touched: it is Windows' TEB on ARM64 (and Android's platform register), and code that
 * changes it crashes Windows' exception dispatch (GitHub issue #4). x29 and x30 make the code's frame record, so that a
 * walk of the stack goes through it.
 *
 * A register of the frame is 64 bits (mg_dvm_reg): an int is kept zero-extended -- computed in a W register, whose write
 * clears the upper half, and stored whole -- and a reference whole, so that if-eq and if-eqz compare whole registers, as
 * the interpreter does. A long or a double is its low half in the first register of its pair and its high half in the
 * second, each zero-extended (mg_dvm_put_wide); a float is its bits, zero-extended.
 *
 * A64 gives most of Java's answers by itself: W and X arithmetic wraps; LSLV, LSRV and ASRV take their count modulo 32 (W)
 * or 64 (X), as Java masks it; SDIV gives MIN_VALUE for MIN_VALUE / -1 (and 0 for a divisor of 0, which never comes there:
 * the code hands the frame back first and the interpreter throws), MSUB the remainder, 0 for MIN_VALUE % -1; FCVTZS rounds
 * toward zero, saturates and makes NaN 0. Only a float's and a double's remainder is a call (fmod's).
 */

#define JIT_ISA "A64"
#define JIT_CODE_BIT 0

typedef char jit_register_dword[sizeof(mg_dvm_reg) == 8 ? 1 : -1];
/* Where the code finds what mg_dvm_jit_ctx holds. */
enum { JIT_CTX_REGS = 0, JIT_CTX_SLOW = 8, JIT_CTX_PC = 16, JIT_CTX_T = 24, JIT_CTX_F = 32, JIT_CTX_RESULT = 48 };
typedef char jit_ctx_layout[offsetof(mg_dvm_jit_ctx, regs) == JIT_CTX_REGS && offsetof(mg_dvm_jit_ctx, slow) == JIT_CTX_SLOW &&
                                    offsetof(mg_dvm_jit_ctx, pc) == JIT_CTX_PC && offsetof(mg_dvm_jit_ctx, t) == JIT_CTX_T &&
                                    offsetof(mg_dvm_jit_ctx, f) == JIT_CTX_F && offsetof(mg_dvm_jit_ctx, result) == JIT_CTX_RESULT
                                ? 1
                                : -1];

/* (Every ARMv8 processor has what the code uses: nothing to look at.) */
static const char *jit_processor(void) { return ""; }

/* The registers the code uses (x18 is none of them). */
enum { X0, X1, X2, X3, X9 = 9, X10 = 10, X11 = 11, X16 = 16, XREGS = 19, XCTX = 20, XSLOW = 21, XZR = 31 };

/*
 * Past ARM's conditions: a branch on a register's being zero (CBZ) or not (CBNZ), its W or its X: bit 7 says so, bit 6
 * not zero, bit 5 the X, bits 0-4 the register.
 */
#define C_CBZ_W(r) ((u8)(0x80 | (r)))
#define C_CBNZ_W(r) ((u8)(0xC0 | (r)))
#define C_CBZ_X(r) ((u8)(0xA0 | (r)))
#define C_CBNZ_X(r) ((u8)(0xE0 | (r)))

/* ------------------------------------------------------------------ branches */

static u32 branch_units(u8 cond)
{
    (void)cond;
    return 1;
}

/* A branch at word `at` to word `to`: B (+-128 MB), B.cond, CBZ or CBNZ (+-1 MB). False when it does not reach. */
static bool put_branch(jit_unit *b, u32 at, u32 to, u8 cond)
{
    i32 off = (i32)to - (i32)at;
    if (cond == C_AL) {
        if (off < -(1 << 25) || off >= (1 << 25)) return false;
        b[at] = 0x14000000u | ((u32)off & 0x03FFFFFFu);
        return true;
    }
    if (off < -(1 << 18) || off >= (1 << 18)) return false;
    if (cond & 0x80)
        b[at] = ((cond & 0x20) ? 0xB4000000u : 0x34000000u) | ((cond & 0x40) ? 0x01000000u : 0u) | ((u32)off & 0x7FFFFu) << 5 | (cond & 31u);
    else
        b[at] = 0x54000000u | ((u32)off & 0x7FFFFu) << 5 | cond;
    return true;
}

/* ------------------------------------------------------------------ the instructions */

static void ins(emitter *e, u32 w) { put_unit(e, w); }

/* A load or store at [rn + off], the offset scaled by the access's size (1 << scale): one that does not fit fails the method. */
static void ldst(emitter *e, u32 op, u32 scale, int rt, int rn, u32 off)
{
    if ((off & ((1u << scale) - 1)) || (off >> scale) > 0xFFF) {
        e->failed = true;
        return;
    }
    ins(e, op | (off >> scale) << 10 | (u32)rn << 5 | (u32)rt);
}
/* The same at [rn + rm << scale] (LSL by the access's size), from the unsigned-offset form's opcode. */
static void ldst_reg(emitter *e, u32 op, u32 scale, int rt, int rn, int rm)
{
    ins(e, (op & ~0x01000000u) | 0x00200800u | (u32)rm << 16 | 3u << 13 | (scale ? 1u << 12 : 0u) | (u32)rn << 5 | (u32)rt);
}

/* (Opcodes past an int's range: macros, not an enum's constants.) */
#define LDR_X 0xF9400000u
#define STR_X 0xF9000000u
#define LDR_W 0xB9400000u
#define STR_W 0xB9000000u
#define LDRB 0x39400000u
#define LDRSB_W 0x39C00000u
#define STRB 0x39000000u
#define LDRH 0x79400000u
#define LDRSH_W 0x79C00000u
#define STRH 0x79000000u
#define LDR_S 0xBD400000u
#define STR_D 0xFD000000u
static void ldr_x(emitter *e, int rt, int rn, u32 off) { ldst(e, LDR_X, 3, rt, rn, off); }
static void str_x(emitter *e, int rt, int rn, u32 off) { ldst(e, STR_X, 3, rt, rn, off); }
static void ldr_w(emitter *e, int rt, int rn, u32 off) { ldst(e, LDR_W, 2, rt, rn, off); }
static void str_w(emitter *e, int rt, int rn, u32 off) { ldst(e, STR_W, 2, rt, rn, off); }

/*
 * What each T_* is loaded and stored with, and its size's log2. A W load zero- or sign-extends into 32 bits and clears the
 * upper half; a long, a double and a reference are whole.
 */
static const struct {
    u32 load, store, scale;
} k_access[] = {
    { LDR_W, STR_W, 2 }, { LDRB, STRB, 0 }, { LDRSB_W, STRB, 0 }, { LDRH, STRH, 1 }, { LDRSH_W, STRH, 1 }, { LDR_X, STR_X, 3 }, { LDR_X, STR_X, 3 },
};

/* The frame's register v: its low half (an int, a float's bits) or whole (a reference); stored whole. */
static void ldr_vw(emitter *e, int rt, u32 v) { ldst(e, LDR_W, 2, rt, XREGS, v * 8); }
static void ldr_vx(emitter *e, int rt, u32 v) { ldst(e, LDR_X, 3, rt, XREGS, v * 8); }
static void str_vx(emitter *e, int rt, u32 v) { ldst(e, STR_X, 3, rt, XREGS, v * 8); }
static void ldr_vs(emitter *e, int st, u32 v) { ldst(e, LDR_S, 2, st, XREGS, v * 8); }
/* A float in s<dt> stored whole: d<dt>, whose upper half the instruction that wrote s<dt> cleared. */
static void str_vd(emitter *e, int dt, u32 v) { ldst(e, STR_D, 3, dt, XREGS, v * 8); }

/* Registers v and v + 1 loaded whole into xa and xb (two registers), or stored from them: one LDP or STP where it reaches. */
static void pair(emitter *e, bool store, int xa, int xb, u32 v)
{
    if (v <= 63) {
        ins(e, (store ? 0xA9000000u : 0xA9400000u) | v << 15 | (u32)xb << 10 | (u32)XREGS << 5 | (u32)xa);
    } else if (store) {
        str_vx(e, xa, v);
        str_vx(e, xb, v + 1);
    } else {
        ldr_vx(e, xa, v);
        ldr_vx(e, xb, v + 1);
    }
}

/* The long, or double's bits, of the pair v, v + 1 into x: the two registers' low halves joined (x9 used). */
static void ldr_wide(emitter *e, int x, u32 v)
{
    pair(e, false, x, X9, v);
    ins(e, 0xB3607C00u | (u32)X9 << 5 | (u32)x);                 /* bfi x, x9, #32, #32 */
}

/* x stored as the pair v, v + 1, each half zero-extended into its register (x9 and x10 used; x kept). */
static void str_wide(emitter *e, int x, u32 v)
{
    ins(e, 0x2A0003E0u | (u32)x << 16 | X9);                     /* mov w9, w<x> */
    ins(e, 0xD360FC00u | (u32)x << 5 | X10);                     /* lsr x10, x<x>, #32 */
    pair(e, true, X9, X10, v);
}

/* A double: the pair v, v + 1 into d<d>, or d<d> stored as it (x11 used). */
static void ldr_double(emitter *e, int d, u32 v)
{
    ldr_wide(e, X11, v);
    ins(e, 0x9E670000u | (u32)X11 << 5 | (u32)d);                /* fmov d<d>, x11 */
}
static void str_double(emitter *e, int d, u32 v)
{
    ins(e, 0x9E660000u | (u32)d << 5 | X11);                     /* fmov x11, d<d> */
    str_wide(e, X11, v);
}

/* w<rd> = v: MOVZ or MOVN, and a MOVK where both halves are needed. */
static void mov_w(emitter *e, int rd, u32 v)
{
    if (v <= 0xFFFF) ins(e, 0x52800000u | v << 5 | (u32)rd);
    else if (!(v & 0xFFFF)) ins(e, 0x52A00000u | (v >> 16) << 5 | (u32)rd);
    else if (~v <= 0xFFFF) ins(e, 0x12800000u | (~v & 0xFFFF) << 5 | (u32)rd);
    else if (!(~v & 0xFFFF)) ins(e, 0x12A00000u | (~v >> 16) << 5 | (u32)rd);
    else {
        ins(e, 0x52800000u | (v & 0xFFFF) << 5 | (u32)rd);
        ins(e, 0x72A00000u | (v >> 16) << 5 | (u32)rd);
    }
}

/* x<rd> = v: a MOVZ (or a MOVN, where more of its halfwords are all ones than none) and a MOVK for each other halfword. */
static void mov_x(emitter *e, int rd, u64 v)
{
    u32 k, zeros = 0, ones = 0;
    bool first = true, inverted;
    for (k = 0; k < 4; k++) {
        u32 h = (u32)(v >> (16 * k)) & 0xFFFF;
        zeros += h == 0;
        ones += h == 0xFFFF;
    }
    inverted = ones > zeros;
    for (k = 0; k < 4; k++) {
        u32 h = (u32)(v >> (16 * k)) & 0xFFFF;
        if (h == (inverted ? 0xFFFFu : 0u)) continue;
        if (first) ins(e, (inverted ? 0x92800000u | (~h & 0xFFFF) << 5 : 0xD2800000u | h << 5) | k << 21 | (u32)rd);
        else ins(e, 0xF2800000u | k << 21 | h << 5 | (u32)rd);
        first = false;
    }
    if (first) ins(e, (inverted ? 0x92800000u : 0xD2800000u) | (u32)rd);   /* all ones, or zero */
}

/* Data processing, rd = rn op rm: the W form for an int, the X form for a long or a reference. */
enum {
    A_ADD = 0x0B000000u, A_SUB = 0x4B000000u, A_SUBS = 0x6B000000u, A_AND = 0x0A000000u, A_ORR = 0x2A000000u, A_EOR = 0x4A000000u,
    A_ORN = 0x2A200000u, A_MUL = 0x1B007C00u, A_SDIV = 0x1AC00C00u, A_LSLV = 0x1AC02000u, A_LSRV = 0x1AC02400u, A_ASRV = 0x1AC02800u,
};
static void alu(emitter *e, u32 op, bool x, int rd, int rn, int rm) { ins(e, op | (x ? 0x80000000u : 0u) | (u32)rm << 16 | (u32)rn << 5 | (u32)rd); }
/* rd = ra - rn * rm */
static void msub(emitter *e, bool x, int rd, int rn, int rm, int ra)
{
    ins(e, 0x1B008000u | (x ? 0x80000000u : 0u) | (u32)rm << 16 | (u32)ra << 10 | (u32)rn << 5 | (u32)rd);
}
static void cmp_r(emitter *e, bool x, int rn, int rm) { alu(e, A_SUBS, x, XZR, rn, rm); }
static void mov_r(emitter *e, int xd, int xm) { alu(e, A_ORR, true, xd, XZR, xm); }

/* add, sub, subs (cmp) and adds (cmn) with a 12-bit immediate: rn is never 31 here, which would be sp. */
enum { I_ADD = 0x11000000u, I_SUB = 0x51000000u, I_SUBS = 0x71000000u, I_ADDS = 0x31000000u };
static void alu_imm(emitter *e, u32 op, int rd, int rn, u32 imm) { ins(e, op | imm << 10 | (u32)rn << 5 | (u32)rd); }

/* The flags of w<rn> - k (cmp or cmn when k reaches, else through w<tmp>). */
static void cmp_w_const(emitter *e, int rn, i32 k, int tmp)
{
    if (k >= 0 && k < 4096) {
        alu_imm(e, I_SUBS, XZR, rn, (u32)k);
    } else if (k < 0 && k > -4096) {
        alu_imm(e, I_ADDS, XZR, rn, (u32)-k);
    } else {
        mov_w(e, tmp, (u32)k);
        cmp_r(e, false, rn, tmp);
    }
}

/* The bitfield moves of W registers: the shifts by an immediate, the sign and zero extensions. */
static void ubfm_w(emitter *e, int rd, int rn, u32 immr, u32 imms) { ins(e, 0x53000000u | immr << 16 | imms << 10 | (u32)rn << 5 | (u32)rd); }
static void sbfm_w(emitter *e, int rd, int rn, u32 immr, u32 imms) { ins(e, 0x13000000u | immr << 16 | imms << 10 | (u32)rn << 5 | (u32)rd); }

/* w<rd> = cond ? 1 : 0; w<rd> = cond ? w<rn> : -w<rm>; w<rd> = cond ? w<rn> : w<rm>. */
static void cset_w(emitter *e, int rd, u8 cond) { ins(e, 0x1A9F07E0u | (u32)(cond ^ 1) << 12 | (u32)rd); }
static void csneg_w(emitter *e, int rd, int rn, int rm, u8 cond) { ins(e, 0x5A800400u | (u32)rm << 16 | (u32)cond << 12 | (u32)rn << 5 | (u32)rd); }
static void csel_w(emitter *e, int rd, int rn, int rm, u8 cond) { ins(e, 0x1A800000u | (u32)rm << 16 | (u32)cond << 12 | (u32)rn << 5 | (u32)rd); }

static void call_c(emitter *e, mg_addr fn)
{
    mov_x(e, X16, (u64)fn);
    ins(e, 0xD63F0000u | (u32)X16 << 5);                         /* blr x16 */
}

/* ------------------------------------------------------------------ int arithmetic */

/* w<rd> = w<rn> op w<rm> (rd, rn, rm none of them x3, which the remainder takes for its quotient). */
static void int_op(emitter *e, int op, int rd, int rn, int rm)
{
    switch (op) {
    case OP_ADD: alu(e, A_ADD, false, rd, rn, rm); break;
    case OP_SUB: alu(e, A_SUB, false, rd, rn, rm); break;
    case OP_RSB: alu(e, A_SUB, false, rd, rm, rn); break;
    case OP_MUL: alu(e, A_MUL, false, rd, rn, rm); break;
    case OP_DIV: alu(e, A_SDIV, false, rd, rn, rm); break;
    case OP_REM:
        alu(e, A_SDIV, false, X3, rn, rm);
        msub(e, false, rd, X3, rm, rn);
        break;
    case OP_AND: alu(e, A_AND, false, rd, rn, rm); break;
    case OP_OR: alu(e, A_ORR, false, rd, rn, rm); break;
    case OP_XOR: alu(e, A_EOR, false, rd, rn, rm); break;
    case OP_SHL: alu(e, A_LSLV, false, rd, rn, rm); break;
    case OP_SHR: alu(e, A_ASRV, false, rd, rn, rm); break;
    default: alu(e, A_LSRV, false, rd, rn, rm); break;
    }
}

/* binop and binop/2addr: vD = vA op vB, a divisor of zero an exit (the interpreter throws ArithmeticException). */
static void emit_arith(emitter *e, u32 pc, int op, u32 vd, u32 va, u32 vb)
{
    ldr_vw(e, X1, va);
    ldr_vw(e, X2, vb);
    if (op == OP_DIV || op == OP_REM) fix(e, FIX_EXIT, pc, C_CBZ_W(X2));
    int_op(e, op, X0, X1, X2);
    str_vx(e, X0, vd);
}

/* binop/lit16 and binop/lit8: vD = vA op lit (never a divisor of zero: the front end hands that back). */
static void emit_arith_lit(emitter *e, int op, u32 vd, u32 va, i32 lit)
{
    ldr_vw(e, X1, va);
    if (op == OP_ADD && lit >= 0 && lit < 4096) {
        alu_imm(e, I_ADD, X0, X1, (u32)lit);
    } else if (op == OP_ADD && lit < 0 && lit > -4096) {
        alu_imm(e, I_SUB, X0, X1, (u32)-lit);
    } else if (op == OP_SHL || op == OP_SHR || op == OP_USHR) {
        u32 s = (u32)lit & 31;                                   /* Java's count, its low five bits */
        if (op == OP_SHL) ubfm_w(e, X0, X1, (32 - s) & 31, 31 - s);
        else if (op == OP_SHR) sbfm_w(e, X0, X1, s, 31);
        else ubfm_w(e, X0, X1, s, 31);
    } else {
        mov_w(e, X2, (u32)lit);
        int_op(e, op, X0, X1, X2);
    }
    str_vx(e, X0, vd);
}

/* ------------------------------------------------------------------ moves and constants */

static void emit_move(emitter *e, u32 vd, u32 vs)
{
    ldr_vx(e, X0, vs);
    str_vx(e, X0, vd);
}

/* (Both read before either is written: the pairs may overlap.) */
static void emit_move_wide(emitter *e, u32 vd, u32 vs)
{
    pair(e, false, X0, X1, vs);
    pair(e, true, X0, X1, vd);
}

/* move-result, -wide, -object: a compiled call's result (ctx->result), an int's low half alone, as the interpreter reads result.i. */
static void emit_move_result(emitter *e, int type, u32 vd)
{
    if (type == T_INT) {
        ldr_w(e, X0, XCTX, JIT_CTX_RESULT);
        str_vx(e, X0, vd);
    } else {
        ldr_x(e, X0, XCTX, JIT_CTX_RESULT);
        if (type == T_WIDE) str_wide(e, X0, vd);
        else str_vx(e, X0, vd);
    }
}

/* The value of a return for ctx->result, whole (`type` -1 for return-void); the front end then branches to the way out. */
static void emit_return(emitter *e, int type, u32 va)
{
    if (type < 0) {
        str_x(e, XZR, XCTX, JIT_CTX_RESULT);
        return;
    }
    if (type == T_WIDE) ldr_wide(e, X0, va);
    else if (type == T_OBJECT) ldr_vx(e, X0, va);
    else ldr_vw(e, X0, va);
    str_x(e, X0, XCTX, JIT_CTX_RESULT);
}

static void emit_const(emitter *e, u32 vd, u32 v)
{
    if (!v) {
        str_vx(e, XZR, vd);
        return;
    }
    mov_w(e, X0, v);
    str_vx(e, X0, vd);
}

static void emit_const_wide(emitter *e, u32 vd, u64 v)
{
    u32 lo = (u32)v, hi = (u32)(v >> 32);
    if (lo) mov_w(e, X0, lo);
    if (hi) mov_w(e, X1, hi);
    pair(e, true, lo ? X0 : XZR, hi ? X1 : XZR, vd);
}

/* A reference the code holds as a constant: a string already made (interned, never freed). */
static void emit_const_object(emitter *e, u32 vd, const void *p)
{
    mov_x(e, X0, (u64)(mg_addr)p);
    str_vx(e, X0, vd);
}

/* ------------------------------------------------------------------ branches on values */

/* if-test vA, vB: if-eq and if-ne on whole registers (a reference is 64 bits), the others on ints. */
static u8 emit_compare(emitter *e, u32 op, u32 va, u32 vb)
{
    bool whole = op == 0x32 || op == 0x33;
    if (whole) {
        ldr_vx(e, X0, va);
        ldr_vx(e, X1, vb);
    } else {
        ldr_vw(e, X0, va);
        ldr_vw(e, X1, vb);
    }
    cmp_r(e, whole, X0, X1);
    return if_cond(op - 0x32);
}

/* if-testz vAA: if-eqz and if-nez by CBZ and CBNZ on the whole register, the others on the int. */
static u8 emit_compare_zero(emitter *e, u32 op, u32 va)
{
    if (op == 0x38 || op == 0x39) {
        ldr_vx(e, X0, va);
        return op == 0x38 ? C_CBZ_X(X0) : C_CBNZ_X(X0);
    }
    ldr_vw(e, X0, va);
    alu_imm(e, I_SUBS, XZR, X0, 0);
    return if_cond(op - 0x38);
}

/* A switch's value, in w0 (a packed switch's less its first key), then each key compared with it: equal is its case. */
static void emit_switch_value(emitter *e, u32 va, bool packed, i32 first)
{
    ldr_vw(e, X0, va);
    if (!packed || !first) return;
    if (first > 0 && first < 4096) {
        alu_imm(e, I_SUB, X0, X0, (u32)first);
    } else if (first < 0 && first > -4096) {
        alu_imm(e, I_ADD, X0, X0, (u32)-first);
    } else {
        mov_w(e, X1, (u32)first);
        alu(e, A_SUB, false, X0, X0, X1);
    }
}

static void emit_switch_key(emitter *e, i32 key) { cmp_w_const(e, X0, key, X1); }

/* ------------------------------------------------------------------ arrays and fields */

/* aget and aput of every kind but aput-object (the runtime's: the store check): vR = vA[vI], or vA[vI] = vR. */
static void emit_array(emitter *e, u32 pc, bool store, int type, u32 vr, u32 va, u32 vi)
{
    static const u8 sizes[] = { 4, 1, 1, 2, 2, 8, 0 };
    u32 size = sizes[type], load = k_access[type].load, put = k_access[type].store, scale = k_access[type].scale;
    /* The array and its index checked as the interpreter checks them -- null, the kind and element size it is, the bounds
       (unsigned: a negative index is past them) -- each failure an exit; then x3 the elements, x1 the index. */
    ldr_vx(e, X0, va);
    fix(e, FIX_EXIT, pc, C_CBZ_X(X0));
    ldst(e, LDRB, 0, X1, X0, (u32)offsetof(mg_jobj, kind));
    alu_imm(e, I_SUBS, XZR, X1, size ? MG_J_PRIM_ARRAY : MG_J_OBJ_ARRAY);
    fix(e, FIX_EXIT, pc, C_NE);
    if (size) {
        ldst(e, LDRB, 0, X1, X0, (u32)offsetof(mg_jobj, u.arr.esz));
        alu_imm(e, I_SUBS, XZR, X1, size);
        fix(e, FIX_EXIT, pc, C_NE);
    }
    ldr_vw(e, X1, vi);
    ldr_w(e, X2, X0, (u32)offsetof(mg_jobj, u.arr.len));
    cmp_r(e, false, X1, X2);
    fix(e, FIX_EXIT, pc, C_HS);
    ldr_x(e, X3, X0, (u32)offsetof(mg_jobj, u.arr.data));
    if (store) {
        if (type == T_WIDE) ldr_wide(e, X2, vr);
        else if (type == T_OBJECT) ldr_vx(e, X2, vr);
        else ldr_vw(e, X2, vr);
        ldst_reg(e, put, scale, X2, X3, X1);
    } else {
        ldst_reg(e, load, scale, X2, X3, X1);
        if (type == T_WIDE) str_wide(e, X2, vr);
        else str_vx(e, X2, vr);
    }
}

static void emit_array_length(emitter *e, u32 pc, u32 vd, u32 va)
{
    ldr_vx(e, X0, va);
    fix(e, FIX_EXIT, pc, C_CBZ_X(X0));
    ldr_w(e, X1, X0, (u32)offsetof(mg_jobj, u.arr.len));
    str_vx(e, X1, vd);
}

/* The field at [x0 + off] read into vR, or written from it. */
static void field_value(emitter *e, bool store, int type, u32 vr, u32 off)
{
    if (store) {
        if (type == T_WIDE) ldr_wide(e, X2, vr);
        else if (type == T_OBJECT) ldr_vx(e, X2, vr);
        else ldr_vw(e, X2, vr);
        ldst(e, k_access[type].store, k_access[type].scale, X2, X0, off);
    } else {
        ldst(e, k_access[type].load, k_access[type].scale, X2, X0, off);
        if (type == T_WIDE) str_wide(e, X2, vr);
        else str_vx(e, X2, vr);
    }
}

/*
 * iget and iput of a field resolved by now, at `off` in the object: the object checked for null and for having the slot
 * (an object the runtime keeps its fields for by name does not: the interpreter's slotless_field), each failure an exit.
 */
static void emit_instance_field(emitter *e, u32 pc, bool store, int type, u32 vr, u32 vo, u32 slot, u32 off)
{
    ldr_vx(e, X0, vo);
    fix(e, FIX_EXIT, pc, C_CBZ_X(X0));
    ldr_x(e, X1, X0, (u32)offsetof(mg_dvm_obj, cls));
    ldr_w(e, X1, X1, (u32)offsetof(mg_dvm_class, nslots));
    mov_w(e, X2, slot);
    cmp_r(e, false, X2, X1);
    fix(e, FIX_EXIT, pc, C_HS);
    field_value(e, store, type, vr, off);
}

/* sget and sput of a class initialised by now: its slot's address is the code's own constant. */
static void emit_static_field(emitter *e, bool store, int type, u32 vr, mg_jvalue *slot)
{
    mov_x(e, X0, (u64)(mg_addr)slot);
    field_value(e, store, type, vr, 0);
}

/* ------------------------------------------------------------------ longs */

/*
 * A long operation of the pairs vA and vB into vD (op as numbered from add-long: add, sub, mul, div, rem, and, or, xor,
 * shl, shr, ushr; the shifts by the int vB, whose count the X forms take modulo 64). False for division and remainder:
 * they are emit_long_divide's.
 */
static bool emit_long(emitter *e, u32 op, u32 vd, u32 va, u32 vb)
{
    static const u32 ops[] = { A_ADD, A_SUB, A_MUL, 0, 0, A_AND, A_ORR, A_EOR, A_LSLV, A_ASRV, A_LSRV };
    if (op == 3 || op == 4 || op > 10) return false;
    ldr_wide(e, X1, va);
    if (op >= 8) ldr_vw(e, X2, vb);
    else ldr_wide(e, X2, vb);
    alu(e, ops[op], true, X0, X1, X2);
    str_wide(e, X0, vd);
    return true;
}

/* div-long and rem-long: SDIV and MSUB, a divisor of zero an exit. */
static void emit_long_divide(emitter *e, u32 pc, bool rem, u32 vd, u32 va, u32 vb)
{
    ldr_wide(e, X2, vb);
    fix(e, FIX_EXIT, pc, C_CBZ_X(X2));
    ldr_wide(e, X1, va);
    if (rem) {
        alu(e, A_SDIV, true, X3, X1, X2);
        msub(e, true, X0, X3, X2, X1);
    } else {
        alu(e, A_SDIV, true, X0, X1, X2);
    }
    str_wide(e, X0, vd);
}

/* cmp-long: 1 where they differ, negated where vA is less. */
static void emit_cmp_long(emitter *e, u32 vd, u32 va, u32 vb)
{
    ldr_wide(e, X0, va);
    ldr_wide(e, X1, vb);
    cmp_r(e, true, X0, X1);
    cset_w(e, X0, C_NE);
    csneg_w(e, X0, X0, X0, C_GE);
    str_vx(e, X0, vd);
}

/* neg-int, not-int, neg-long, not-long, int-to-long, long-to-int, int-to-byte, -char, -short: vA = op vB. */
static void emit_unary(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x7b: case 0x7c:                                        /* neg-int, not-int */
        ldr_vw(e, X0, vb);
        alu(e, op == 0x7b ? A_SUB : A_ORN, false, X0, XZR, X0);
        str_vx(e, X0, va);
        break;
    case 0x7d: case 0x7e:                                        /* neg-long, not-long */
        ldr_wide(e, X0, vb);
        alu(e, op == 0x7d ? A_SUB : A_ORN, true, X0, XZR, X0);
        str_wide(e, X0, va);
        break;
    case 0x81:                                                   /* int-to-long: the int, then its sign as the high half */
        ldr_vw(e, X0, vb);
        sbfm_w(e, X1, X0, 31, 31);                               /* asr w1, w0, #31 */
        pair(e, true, X0, X1, va);
        break;
    case 0x84:                                                   /* long-to-int: the low half */
        ldr_vw(e, X0, vb);
        str_vx(e, X0, va);
        break;
    default:                                                     /* int-to-byte, -char, -short */
        ldr_vw(e, X0, vb);
        if (op == 0x8e) ubfm_w(e, X0, X0, 0, 15);                /* uxth */
        else sbfm_w(e, X0, X0, 0, op == 0x8d ? 7 : 15);          /* sxtb, sxth */
        str_vx(e, X0, va);
        break;
    }
}

/* ------------------------------------------------------------------ floating point */

/*
 * d0 and d1 hold an instruction's values, loaded from the frame's registers and stored back, nothing kept in them from one
 * instruction to the next. The arithmetic is the host's, as the interpreter's C does it with the same FPCR; every
 * instruction that writes s0 clears the rest of v0, so that d0 is the float's bits zero-extended, as a register holds them.
 */
enum { F_ADD = 0x1E202800u, F_SUB = 0x1E203800u, F_MUL = 0x1E200800u, F_DIV = 0x1E201800u, F_DOUBLE = 0x00400000u };

/* vd = va op vb, op 0 add, 1 sub, 2 mul, 3 div: s0 = s0 op s1, or d0 = d0 op d1. */
static void emit_fp_arith(emitter *e, u32 op, bool dbl, u32 vd, u32 va, u32 vb)
{
    static const u32 ops[] = { F_ADD, F_SUB, F_MUL, F_DIV };
    if (dbl) {
        ldr_double(e, 0, va);
        ldr_double(e, 1, vb);
        ins(e, ops[op] | F_DOUBLE | 1u << 16);
        str_double(e, 0, vd);
    } else {
        ldr_vs(e, 0, va);
        ldr_vs(e, 1, vb);
        ins(e, ops[op] | 1u << 16);
        str_vd(e, 0, vd);
    }
}

/*
 * cmpl and cmpg: 1 where they differ or are unordered (FCMP's NE), negated where vA is less -- for cmpl, also where they
 * are unordered (LT holds then, and MI does not): NaN either side gives -1 (cmpl) or 1 (cmpg).
 */
static void emit_fp_compare(emitter *e, bool dbl, bool greater, u32 vd, u32 va, u32 vb)
{
    if (dbl) {
        ldr_double(e, 0, va);
        ldr_double(e, 1, vb);
        ins(e, 0x1E602000u | 1u << 16);                          /* fcmp d0, d1 */
    } else {
        ldr_vs(e, 0, va);
        ldr_vs(e, 1, vb);
        ins(e, 0x1E202000u | 1u << 16);                          /* fcmp s0, s1 */
    }
    cset_w(e, X0, C_NE);
    csneg_w(e, X0, X0, X0, greater ? C_PL : C_GE);
    str_vx(e, X0, vd);
}

/* The conversions between int, float and double, and neg: vA = (kind) vB. FCVTZS is Java's conversion to an int. */
static bool emit_fp_convert(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x82: ldr_vw(e, X0, vb); ins(e, 0x1E220000u); str_vd(e, 0, va); return true;                  /* int-to-float: scvtf s0, w0 */
    case 0x83: ldr_vw(e, X0, vb); ins(e, 0x1E620000u); str_double(e, 0, va); return true;              /* int-to-double: scvtf d0, w0 */
    case 0x87: ldr_vs(e, 0, vb); ins(e, 0x1E380000u); str_vx(e, X0, va); return true;                  /* float-to-int: fcvtzs w0, s0 */
    case 0x89: ldr_vs(e, 0, vb); ins(e, 0x1E22C000u); str_double(e, 0, va); return true;              /* float-to-double: fcvt d0, s0 */
    case 0x8a: ldr_double(e, 0, vb); ins(e, 0x1E780000u); str_vx(e, X0, va); return true;             /* double-to-int: fcvtzs w0, d0 */
    case 0x8c: ldr_double(e, 0, vb); ins(e, 0x1E624000u); str_vd(e, 0, va); return true;              /* double-to-float: fcvt s0, d0 */
    case 0x7f: ldr_vs(e, 0, vb); ins(e, 0x1E214000u); str_vd(e, 0, va); return true;                  /* neg-float: fneg s0, s0 */
    case 0x80: ldr_double(e, 0, vb); ins(e, 0x1E614000u); str_double(e, 0, va); return true;          /* neg-double: fneg d0, d0 */
    default: return false;
    }
}

/* long-to-float, long-to-double, float-to-long, double-to-long: SCVTF and FCVTZS of X registers, as Java converts. */
static void emit_long_convert(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x85: ldr_wide(e, X0, vb); ins(e, 0x9E220000u); str_vd(e, 0, va); break;                     /* scvtf s0, x0 */
    case 0x86: ldr_wide(e, X0, vb); ins(e, 0x9E620000u); str_double(e, 0, va); break;                 /* scvtf d0, x0 */
    case 0x88: ldr_vs(e, 0, vb); ins(e, 0x9E380000u); str_wide(e, X0, va); break;                     /* fcvtzs x0, s0 */
    default: ldr_double(e, 0, vb); ins(e, 0x9E780000u); str_wide(e, X0, va); break;                   /* fcvtzs x0, d0 */
    }
}

/* A float's and a double's remainder: fmod's, by a call (its result in s0 or d0, of which nothing else is sure). */
static float jit_rem_float(float a, float b) { return fmodf(a, b); }
static double jit_rem_double(double a, double b) { return fmod(a, b); }

static void emit_fp_remainder(emitter *e, bool dbl, u32 vd, u32 va, u32 vb)
{
    if (dbl) {
        ldr_double(e, 0, va);
        ldr_double(e, 1, vb);
        call_c(e, (mg_addr)jit_rem_double);
        str_double(e, 0, vd);
    } else {
        ldr_vs(e, 0, va);
        ldr_vs(e, 1, vb);
        call_c(e, (mg_addr)jit_rem_float);
        ins(e, 0x1E260000u);                                     /* fmov w0, s0: the float alone, zero-extended */
        str_vx(e, X0, vd);
    }
}

/* ------------------------------------------------------------------ calls */

/*
 * An invoke: mg_dvm_jit_invoke(t, f, pc, &ctx->result), which does the call as the interpreter would; 1 back is an exception
 * pending (out, thrown here), 2 a call it leaves to the interpreter (out at the invoke, nothing done).
 */
static void emit_invoke(emitter *e, u32 pc)
{
    ldr_x(e, X0, XCTX, JIT_CTX_T);
    ldr_x(e, X1, XCTX, JIT_CTX_F);
    mov_w(e, X2, pc);
    alu_imm(e, I_ADD | 0x80000000u, X3, XCTX, JIT_CTX_RESULT);  /* add x3, x20, #result */
    call_c(e, (mg_addr)mg_dvm_jit_invoke);
    alu_imm(e, I_SUBS, XZR, X0, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    alu_imm(e, I_SUBS, XZR, X0, 2);
    fix(e, FIX_EXIT, pc, C_EQ);
}

/*
 * The quick way (the front end's fast_invoke): mg_dvm_jit_fast(ctx, m, desc, pc); 1 back is an exception pending, 3 a call
 * it did not make, which goes the general way. `known`: the method is the one called, and whether its code has a start
 * for such calls is asked in the code first, with no call when not.
 */
static void emit_fast_invoke(emitter *e, u32 pc, const mg_dvm_method *ref, u32 desc, bool known)
{
    u32 done, slow[2], nslow = 0, i;
    mov_x(e, X1, (u64)(mg_addr)ref);
    if (known) {
        ldr_x(e, X0, X1, (u32)offsetof(mg_dvm_method, jit));
        slow[nslow++] = local_branch(e, C_CBZ_X(X0));
        ldr_x(e, X0, X0, (u32)offsetof(mg_dvm_jit_code, fast));
        slow[nslow++] = local_branch(e, C_CBZ_X(X0));
    }
    mov_r(e, X0, XCTX);
    mov_w(e, X2, desc);
    mov_w(e, X3, pc);
    call_c(e, (mg_addr)mg_dvm_jit_fast);
    alu_imm(e, I_SUBS, XZR, X0, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    alu_imm(e, I_SUBS, XZR, X0, 3);
    done = local_branch(e, C_NE);
    for (i = 0; i < nslow; i++) local_to(e, slow[i], C_CBZ_X(X0));
    emit_invoke(e, pc);
    local_to(e, done, C_NE);
}

/*
 * The intrinsics that are a few instructions, as inline_call allows them (its receiver in x0, checked for null, when the
 * method is not static), each result whole in ctx->result. A null string or an index out of its bounds goes by the call
 * (mg_dvm_jit_invoke), which throws as the interpreter would; the others cannot fail.
 */
static void intrinsic_in_code(emitter *e, u8 id, u32 a0, u32 a1, u32 *fails, u8 *conds, u32 *nfails)
{
    switch (id) {
    case INTR_STRING_LENGTH:
        ldr_w(e, X1, X0, (u32)offsetof(mg_jobj, u.str.len16));
        str_x(e, X1, XCTX, JIT_CTX_RESULT);
        break;
    case INTR_REFERENT:
        /* Reference.getReferent: the referent's slot (a reference object always has it, as a subclass of Reference). */
        ldr_x(e, X1, X0, (u32)MG_DVM_HEADER + g_dvm_referent_slot * (u32)sizeof(mg_jvalue));
        str_x(e, X1, XCTX, JIT_CTX_RESULT);
        break;
    case INTR_STRING_ISEMPTY:
        ldr_w(e, X1, X0, (u32)offsetof(mg_jobj, u.str.len16));
        alu_imm(e, I_SUBS, XZR, X1, 0);
        cset_w(e, X2, C_EQ);
        str_x(e, X2, XCTX, JIT_CTX_RESULT);
        break;
    case INTR_STRING_CHARAT:
        ldr_vw(e, X1, a1);
        ldr_w(e, X2, X0, (u32)offsetof(mg_jobj, u.str.len16));
        cmp_r(e, false, X1, X2);
        conds[*nfails] = C_HS;
        fails[(*nfails)++] = local_branch(e, C_HS);
        ldr_x(e, X3, X0, (u32)offsetof(mg_jobj, u.str.u16));
        ldst_reg(e, LDRH, 1, X2, X3, X1);                        /* w2 = x3[x1], a char */
        str_x(e, X2, XCTX, JIT_CTX_RESULT);
        break;
    case INTR_MIN_I: case INTR_MAX_I:
        ldr_vw(e, X0, a0);
        ldr_vw(e, X1, a1);
        cmp_r(e, false, X0, X1);
        csel_w(e, X0, X1, X0, id == INTR_MIN_I ? C_GT : C_LT);  /* the second where the first is greater (min), less (max) */
        str_x(e, X0, XCTX, JIT_CTX_RESULT);
        break;
    case INTR_ABS_I:
        ldr_vw(e, X0, a0);
        alu_imm(e, I_SUBS, XZR, X0, 0);
        csneg_w(e, X0, X0, X0, C_GE);                            /* (MIN_VALUE stays itself, as Java has it) */
        str_x(e, X0, XCTX, JIT_CTX_RESULT);
        break;
    default:                                                     /* sqrt */
        ldr_double(e, 0, a0);
        ins(e, 0x1E61C000u);                                     /* fsqrt d0, d0 */
        ldst(e, STR_D, 3, 0, XCTX, JIT_CTX_RESULT);
        break;
    }
}

/* What a getter done in place leaves in ctx->result: the field at [x0 + off], an int's kind extended as iget does. */
static void getter_result(emitter *e, int type, u32 off)
{
    ldst(e, k_access[type].load, k_access[type].scale, X2, X0, off);
    str_x(e, X2, XCTX, JIT_CTX_RESULT);
}

/* A call of a trivial method or an intrinsic done in the code, as the front end's inline_call found it may be. */
static void emit_inline_call(emitter *e, u32 pc, const inline_plan *p)
{
    u32 fails[6], i, nfails = 0, done;
    u8 conds[6];
    if (p->hooks) {
        /* The method hooked since (mg_dvm_hook.c): by mg_dvm_jit_invoke, which calls the hook. */
        mov_x(e, X1, (u64)(mg_addr)p->target);
        ldst(e, LDRB, 0, X1, X1, (u32)offsetof(mg_dvm_method, hooked));
        conds[nfails] = C_CBNZ_W(X1);
        fails[nfails++] = local_branch(e, C_CBNZ_W(X1));
    }
    if (p->kind != DVM_INVOKE_STATIC) {
        ldr_vx(e, X0, p->self);
        conds[nfails] = C_CBZ_X(X0);
        fails[nfails++] = local_branch(e, C_CBZ_X(X0));
        if (p->vtab) {
            ldr_x(e, X1, X0, (u32)offsetof(mg_dvm_obj, cls));
            ldr_w(e, X2, X1, (u32)offsetof(mg_dvm_class, nvtab));
            mov_w(e, X3, (u32)p->target->vidx);
            cmp_r(e, false, X3, X2);
            conds[nfails] = C_HS;
            fails[nfails++] = local_branch(e, C_HS);
            ldr_x(e, X1, X1, (u32)offsetof(mg_dvm_class, vtab));
            ldst_reg(e, LDR_X, 3, X1, X1, X3);                   /* x1 = vtab[vidx] */
            mov_x(e, X2, (u64)(mg_addr)p->target);
            cmp_r(e, true, X1, X2);
            conds[nfails] = C_NE;
            fails[nfails++] = local_branch(e, C_NE);
        }
        if (p->slots) {
            ldr_x(e, X1, X0, (u32)offsetof(mg_dvm_obj, cls));
            ldr_w(e, X1, X1, (u32)offsetof(mg_dvm_class, nslots));
            mov_w(e, X2, p->fld->slot);
            cmp_r(e, false, X2, X1);
            conds[nfails] = C_HS;
            fails[nfails++] = local_branch(e, C_HS);
        }
    }
    if (p->intrinsic) {
        intrinsic_in_code(e, p->intrinsic, p->self, p->value, fails, conds, &nfails);
    } else switch (SHAPE_OF(p->shape)) {
    case SHAPE_CONST:
        mov_w(e, X0, (u32)SHAPE_VALUE(p->shape));
        str_x(e, X0, XCTX, JIT_CTX_RESULT);
        break;
    case SHAPE_STATIC_GETTER:
        mov_x(e, X0, (u64)(mg_addr)&p->fld->cls->statics[p->fld->slot]);
        getter_result(e, p->type, 0);
        break;
    case SHAPE_GETTER:
        getter_result(e, p->type, p->off);
        break;
    case SHAPE_SETTER:
        field_value(e, true, p->type, p->value, p->off);
        break;
    default:                                                     /* empty */
        break;
    }
    if (nfails) {
        done = local_branch(e, C_AL);
        for (i = 0; i < nfails; i++) local_to(e, fails[i], conds[i]);
        emit_invoke(e, pc);
        local_to(e, done, C_AL);
    }
}

/*
 * const-class, the monitors, check-cast, instance-of, new-instance, new-array, aput-object: mg_dvm_jit_op(t, f, pc), which
 * does it on the frame's registers as the interpreter does; 1 back is an exception pending, 2 one it leaves to the interpreter.
 */
static void emit_runtime(emitter *e, u32 pc)
{
    ldr_x(e, X0, XCTX, JIT_CTX_T);
    ldr_x(e, X1, XCTX, JIT_CTX_F);
    mov_w(e, X2, pc);
    call_c(e, (mg_addr)mg_dvm_jit_op);
    alu_imm(e, I_SUBS, XZR, X0, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    alu_imm(e, I_SUBS, XZR, X0, 2);
    fix(e, FIX_EXIT, pc, C_EQ);
}

/* check-cast and instance-of of class `c`, resolved: the code's own answer for null and for the very class, the runtime's for the rest. */
static void emit_type_test(emitter *e, u32 pc, bool instance_of, u32 vo, u32 vd, const mg_dvm_class *c)
{
    u32 null_done, same_done, slow_done;
    ldr_vx(e, X0, vo);
    null_done = local_branch(e, C_CBZ_X(X0));
    ldr_x(e, X1, X0, (u32)offsetof(mg_dvm_obj, cls));
    mov_x(e, X2, (u64)(mg_addr)c);
    cmp_r(e, true, X1, X2);
    same_done = local_branch(e, C_EQ);
    emit_runtime(e, pc);
    slow_done = local_branch(e, C_AL);
    if (instance_of) {
        /* 1 for the very class; 0 for null, which x0 is */
        local_to(e, same_done, C_EQ);
        mov_w(e, X0, 1);
        local_to(e, null_done, C_CBZ_X(X0));
        str_vx(e, X0, vd);
    } else {
        local_to(e, null_done, C_CBZ_X(X0));
        local_to(e, same_done, C_EQ);
    }
    local_to(e, slow_done, C_AL);
}

/* ------------------------------------------------------------------ the way in and out */

/*
 * stp x29, x30, [sp, #-48]!; mov x29, sp; stp x19, x20, [sp, #16]; str x21, [sp, #32] (a frame of 48 bytes: sp stays
 * 16-byte aligned for the calls); x20 = ctx, x19 = ctx->regs, x21 = ctx->slow; br x1 (the entry).
 */
static void emit_prologue(emitter *e)
{
    ins(e, 0xA9BD7BFDu);
    ins(e, 0x910003FDu);
    ins(e, 0xA90153F3u);
    ins(e, 0xF90013F5u);
    mov_r(e, XCTX, X0);
    ldr_x(e, XREGS, X0, JIT_CTX_REGS);
    ldr_x(e, XSLOW, X0, JIT_CTX_SLOW);
    ins(e, 0xD61F0000u | (u32)X1 << 5);
}

/* x19-x21, x29 and x30 back as they were, and back to the caller. */
static void restore_and_return(emitter *e)
{
    ins(e, 0xA94153F3u);                                         /* ldp x19, x20, [sp, #16] */
    ins(e, 0xF94013F5u);                                         /* ldr x21, [sp, #32] */
    ins(e, 0xA8C37BFDu);                                         /* ldp x29, x30, [sp], #48 */
    ins(e, 0xD65F03C0u);                                         /* ret */
}

/* Out at `pc`: w0 = pc, to the way out (FIX_EPILOGUE); with the exception pending, to its way out (FIX_THROW_EPILOGUE). */
static void emit_exit(emitter *e, u32 pc)
{
    mov_w(e, X0, pc);
    fix(e, FIX_EPILOGUE, 0, C_AL);
}

static void emit_throw_exit(emitter *e, u32 pc)
{
    mov_w(e, X0, pc);
    fix(e, FIX_THROW_EPILOGUE, 0, C_AL);
}

/* A jump back to `pc`: on to its code, unless the instructions are watched (g_dvm.slow): then out at `pc`. */
static void emit_back_check(emitter *e, u32 pc)
{
    ldr_w(e, X0, XSLOW, 0);
    fix(e, FIX_EXIT, pc, C_CBNZ_W(X0));
    fix(e, FIX_CODE, pc, C_AL);
}

/* The ways out, each's first unit: ctx->pc = w0, return 0; the same, return 1 (an exception pending); return 2 (a return, its value in ctx->result). */
static void emit_epilogues(emitter *e, u32 *epilogue, u32 *thrown, u32 *returned)
{
    *epilogue = e->n;
    str_w(e, X0, XCTX, JIT_CTX_PC);
    mov_w(e, X0, 0);
    restore_and_return(e);
    *thrown = e->n;
    str_w(e, X0, XCTX, JIT_CTX_PC);
    mov_w(e, X0, 1);
    restore_and_return(e);
    *returned = e->n;
    mov_w(e, X0, 2);
    restore_and_return(e);
}
