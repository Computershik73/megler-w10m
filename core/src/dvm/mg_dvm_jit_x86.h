/*
 * The JIT's x86 back end (docs/JIT.md, "the back ends"): what mg_dvm_jit.c's front end asks for, in x86-64's instructions
 * under the System V convention, or in 32-bit x86's under i386's cdecl -- the conventions of the core's ELF on desktop
 * Windows, which are Android's (docs/DESKTOP.md), not Windows' -- with SSE2 for floating point, never x87. Included by
 * mg_dvm_jit.c alone, after its emitter and before its compiler.
 *
 * Registers: rbx (ebx) the frame's registers, r12 (esi) the context, r13 (edi) the slow flag's address, rbp the frame
 * pointer: callee-saved, and so kept by the calls the code makes. rax, rcx, rdx (and r11, and the arguments' registers
 * of a call) for values, xmm0 and xmm1 for floating point. Nothing is kept in them from one instruction to the next, and a
 * reference only ever in a general register, which the collector reads of a thread it stops (rax..r15, eax..edi), never in
 * an xmm. Neither fs nor gs is ever named: they are Windows' TEB (gs on x64, fs on x86); the guest's own reads of fs are
 * the patcher's.
 *
 * A register of the frame is 64 bits on x86-64: an int zero-extended (a 32-bit operation's write clears the upper half by
 * itself) and stored whole, a reference whole, a long or a double halved into its two registers, each zero-extended, as on
 * A64. On x86 it is 32 bits, and a long or a double is the 8 bytes of its two registers, as on the phone.
 *
 * Java's answers where x86's differ: CVTTSS2SI and CVTTSD2SI give the "integer indefinite" (MIN_VALUE) for NaN and for
 * what is out of range, which the code turns into 0 and MAX_VALUE as Java has them; IDIV faults on a divisor of zero (the
 * code hands the frame back first, and the interpreter throws) and on MIN_VALUE / -1, for which the code negates instead
 * (MIN_VALUE again, with a remainder of 0). The shifts mask their counts as Java does (31, or 63 for a long's).
 */

#if MG_ARCH_X86_64
#define JIT_ISA "x86-64"
#define X64 1
#define SLOT 8
enum { JIT_CTX_REGS = 0, JIT_CTX_SLOW = 8, JIT_CTX_PC = 16, JIT_CTX_T = 24, JIT_CTX_F = 32, JIT_CTX_RESULT = 48 };
#else
#define JIT_ISA "x86"
#define X64 0
#define SLOT 4
enum { JIT_CTX_REGS = 0, JIT_CTX_SLOW = 4, JIT_CTX_PC = 8, JIT_CTX_T = 12, JIT_CTX_F = 16, JIT_CTX_RESULT = 24 };
#endif
#define JIT_CODE_BIT 0

typedef char jit_register_size[sizeof(mg_dvm_reg) == SLOT ? 1 : -1];
typedef char jit_ctx_layout[offsetof(mg_dvm_jit_ctx, regs) == JIT_CTX_REGS && offsetof(mg_dvm_jit_ctx, slow) == JIT_CTX_SLOW &&
                                    offsetof(mg_dvm_jit_ctx, pc) == JIT_CTX_PC && offsetof(mg_dvm_jit_ctx, t) == JIT_CTX_T &&
                                    offsetof(mg_dvm_jit_ctx, f) == JIT_CTX_F && offsetof(mg_dvm_jit_ctx, result) == JIT_CTX_RESULT
                                ? 1
                                : -1];

/* (SSE2 and CMOV are every x86-64's and every processor's Windows 10 runs on: nothing to look at.) */
static const char *jit_processor(void) { return ""; }

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { XMM0, XMM1, XMM2, XMM3 };
#if X64
enum { XREGS = RBX, XCTX = R12, XSLOW = R13, XTMP = R11 };
#else
enum { XREGS = RBX, XCTX = RSI, XSLOW = RDI };
#endif

/*
 * x86's condition codes (the low nibble of Jcc's and SETcc's and CMOVcc's opcodes); ARM's conditions are turned into
 * them, and an x86 condition the front end has no name for is X86_CC(cc).
 */
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
#define X86_CC(cc) ((u8)(0x40 | (cc)))

static u32 x86_cc(u8 cond)
{
    static const u8 arm[] = { CC_E, CC_NE, CC_AE, CC_B, CC_S, CC_NS, CC_O, CC_NO, CC_A, CC_BE, CC_GE, CC_L, CC_G, CC_LE };
    return (cond & 0x40) ? cond & 0xFu : arm[cond];
}

/* ------------------------------------------------------------------ branches */

/* JMP rel32 (5 bytes) or Jcc rel32 (6). */
static u32 branch_units(u8 cond) { return cond == C_AL ? 5 : 6; }

static bool put_branch(jit_unit *b, u32 at, u32 to, u8 cond)
{
    u32 len = branch_units(cond), rel;
    if (cond == C_AL) {
        b[at] = 0xE9;
    } else {
        b[at] = 0x0F;
        b[at + 1] = (u8)(0x80 | x86_cc(cond));
    }
    rel = to - (at + len);
    b[at + len - 4] = (u8)rel;
    b[at + len - 3] = (u8)(rel >> 8);
    b[at + len - 2] = (u8)(rel >> 16);
    b[at + len - 1] = (u8)(rel >> 24);
    return true;
}

/* ------------------------------------------------------------------ the instructions */

static void b1(emitter *e, u32 v) { put_unit(e, v & 0xFF); }
static void b4(emitter *e, u32 v)
{
    b1(e, v);
    b1(e, v >> 8);
    b1(e, v >> 16);
    b1(e, v >> 24);
}

/*
 * One instruction with a ModRM operand: a legacy prefix `pre` (SSE's 0x66, 0xF2, 0xF3) or 0; REX on x86-64 where it is
 * needed (W: 64 bits; and the high bit of each register named); the opcode, its bytes from the lowest one (0x0F first);
 * `reg`, a register or the opcode's extension; then the register `base` (mem false), or the memory at [base + disp], or
 * at [base + index << scale] when `index` is not -1.
 */
static void x86_op(emitter *e, u32 pre, bool w, u32 op, int reg, bool mem, int base, int index, u32 scale, i32 disp)
{
    u32 rex = 0x40 | (w ? 8u : 0u) | ((reg & 8) ? 4u : 0u) | (mem && index >= 0 && (index & 8) ? 2u : 0u) | ((base & 8) ? 1u : 0u);
    if (pre) b1(e, pre);
    if (X64 && rex != 0x40) b1(e, rex);
    do {
        b1(e, op);
        op >>= 8;
    } while (op);
    if (!mem) {
        b1(e, 0xC0 | (u32)(reg & 7) << 3 | (u32)(base & 7));
        return;
    }
    {
        /* [rbp] and [r13] have no form without a displacement: 0 as a byte. */
        u32 mod = !disp && (base & 7) != RBP ? 0 : disp >= -128 && disp <= 127 ? 1 : 2;
        if (index >= 0) {
            b1(e, mod << 6 | (u32)(reg & 7) << 3 | 4);
            b1(e, scale << 6 | (u32)(index & 7) << 3 | (u32)(base & 7));
        } else if ((base & 7) == RSP) {
            b1(e, mod << 6 | (u32)(reg & 7) << 3 | 4);
            b1(e, 0x24);
        } else {
            b1(e, mod << 6 | (u32)(reg & 7) << 3 | (u32)(base & 7));
        }
        if (mod == 1) b1(e, (u32)disp);
        else if (mod == 2) b4(e, (u32)disp);
    }
}
static void op_rr(emitter *e, u32 pre, bool w, u32 op, int reg, int rm) { x86_op(e, pre, w, op, reg, false, rm, -1, 0, 0); }
static void op_rm(emitter *e, u32 pre, bool w, u32 op, int reg, int base, i32 disp) { x86_op(e, pre, w, op, reg, true, base, -1, 0, disp); }
static void op_rx(emitter *e, u32 pre, bool w, u32 op, int reg, int base, int index, u32 scale) { x86_op(e, pre, w, op, reg, true, base, index, scale, 0); }

/* The frame's register v: its int (the low 32 bits), or whole; stored whole. */
static i32 at_v(u32 v) { return (i32)(v * SLOT); }
static void ld_int(emitter *e, int r, u32 v) { op_rm(e, 0, false, 0x8B, r, XREGS, at_v(v)); }
static void ld_word(emitter *e, int r, u32 v) { op_rm(e, 0, X64, 0x8B, r, XREGS, at_v(v)); }
static void st_word(emitter *e, int r, u32 v) { op_rm(e, 0, X64, 0x89, r, XREGS, at_v(v)); }

/* r = v, an int: MOV r32, imm32, which leaves the flags (and zero-extends on x86-64). */
static void mov_i32(emitter *e, int r, u32 v)
{
    if (X64 && r >= R8) b1(e, 0x41);
    b1(e, 0xB8 | (u32)(r & 7));
    b4(e, v);
}

/* r = a pointer: MOV r64, imm64 on x86-64 where it is past 32 bits. */
static void mov_ptr(emitter *e, int r, const void *p)
{
#if X64
    u64 v = (u64)(mg_addr)p;
    if (v <= 0xFFFFFFFFu) {
        mov_i32(e, r, (u32)v);
        return;
    }
    b1(e, 0x48 | (r >= R8 ? 1u : 0u));
    b1(e, 0xB8 | (u32)(r & 7));
    b4(e, (u32)v);
    b4(e, (u32)(v >> 32));
#else
    mov_i32(e, r, (u32)(mg_addr)p);
#endif
}

/* The ALU's group-1 operations with a register (op as the opcode of "r/m, r") or an immediate (`ext` its extension). */
enum { G_ADD = 0, G_OR = 1, G_AND = 4, G_SUB = 5, G_XOR = 6, G_CMP = 7 };
static void alu_rr(emitter *e, bool w, int ext, int rd, int rs) { op_rr(e, 0, w, (u32)(ext << 3 | 1), rs, rd); }
static void alu_ri(emitter *e, bool w, int ext, int rd, i32 imm)
{
    if (imm >= -128 && imm <= 127) {
        op_rr(e, 0, w, 0x83, ext, rd);
        b1(e, (u32)imm);
    } else {
        op_rr(e, 0, w, 0x81, ext, rd);
        b4(e, (u32)imm);
    }
}
/* the same on memory: [base + disp] op= imm (cmp of a byte, a dword or a qword) */
static void cmp_mi(emitter *e, int size, int base, i32 disp, i32 imm)
{
    if (size == 1) {
        op_rm(e, 0, false, 0x80, G_CMP, base, disp);
        b1(e, (u32)imm);
    } else {
        op_rm(e, 0, size == 8, 0x83, G_CMP, base, disp);
        b1(e, (u32)imm);
    }
}
static void test_rr(emitter *e, bool w, int a, int b) { op_rr(e, 0, w, 0x85, b, a); }
static void mov_rr(emitter *e, bool w, int rd, int rs) { op_rr(e, 0, w, 0x89, rs, rd); }

static void call_c(emitter *e, mg_addr fn)
{
    mov_ptr(e, RAX, (const void *)fn);
    op_rr(e, 0, false, 0xFF, 2, RAX);                            /* call rax */
}

/* ------------------------------------------------------------------ longs and doubles in a 64-bit frame (x86-64) */

#if X64
/* The long of the pair v, v + 1 into r (r11 used): the two registers' low halves joined. */
static void ld_wide(emitter *e, int r, u32 v)
{
    ld_int(e, r, v);
    ld_int(e, XTMP, v + 1);
    op_rr(e, 0, true, 0xC1, 4, XTMP);                            /* shl r11, 32 */
    b1(e, 32);
    alu_rr(e, true, G_OR, r, XTMP);
}
/* r stored as the pair v, v + 1, each half zero-extended (r11 used; r kept). */
static void st_wide(emitter *e, int r, u32 v)
{
    mov_rr(e, false, XTMP, r);                                   /* mov r11d, r32: the low half */
    st_word(e, XTMP, v);
    mov_rr(e, true, XTMP, r);
    op_rr(e, 0, true, 0xC1, 5, XTMP);                            /* shr r11, 32 */
    b1(e, 32);
    st_word(e, XTMP, v + 1);
}
#endif

/* xmm<x> = the float of register v, or the double of the pair v, v + 1; and stored back so. */
static void ld_float(emitter *e, int x, u32 v)
{
#if X64
    op_rm(e, 0x66, false, 0x6E0F, x, XREGS, at_v(v));            /* movd xmm, [v] */
#else
    op_rm(e, 0xF3, false, 0x100F, x, XREGS, at_v(v));            /* movss xmm, [v] */
#endif
}
static void st_float(emitter *e, int x, u32 v)
{
#if X64
    op_rr(e, 0x66, false, 0x7E0F, x, RAX);                       /* movd eax, xmm: the float alone, zero-extended */
    st_word(e, RAX, v);
#else
    op_rm(e, 0xF3, false, 0x110F, x, XREGS, at_v(v));            /* movss [v], xmm */
#endif
}
static void ld_double(emitter *e, int x, u32 v)
{
#if X64
    ld_wide(e, RAX, v);
    op_rr(e, 0x66, true, 0x6E0F, x, RAX);                        /* movq xmm, rax */
#else
    op_rm(e, 0xF2, false, 0x100F, x, XREGS, at_v(v));            /* movsd xmm, [v]: the two registers' 8 bytes */
#endif
}
static void st_double(emitter *e, int x, u32 v)
{
#if X64
    op_rr(e, 0x66, true, 0x7E0F, x, RAX);                        /* movq rax, xmm */
    st_wide(e, RAX, v);
#else
    op_rm(e, 0xF2, false, 0x110F, x, XREGS, at_v(v));            /* movsd [v], xmm */
#endif
}

/* ------------------------------------------------------------------ int arithmetic */

/*
 * eax = eax / ecx or eax % ecx (ecx not 0: checked before, or a literal that is not): IDIV, but for a divisor of -1,
 * where IDIV faults on MIN_VALUE, the negation (MIN_VALUE again) or 0. `w`: of longs, rax and rcx.
 */
static void divide(emitter *e, bool w, bool rem)
{
    u32 normal, done;
    alu_ri(e, w, G_CMP, RCX, -1);
    normal = local_branch(e, C_NE);
    if (rem) alu_rr(e, false, G_XOR, RAX, RAX);
    else op_rr(e, 0, w, 0xF7, 3, RAX);                           /* neg */
    done = local_branch(e, C_AL);
    local_to(e, normal, C_NE);
    if (w) b1(e, 0x48);
    b1(e, 0x99);                                                 /* cdq, cqo */
    op_rr(e, 0, w, 0xF7, 7, RCX);                                /* idiv rcx */
    if (rem) mov_rr(e, w, RAX, RDX);
    local_to(e, done, C_AL);
}

/* eax = eax op ecx: the shifts by cl, masked by the processor as Java masks them. `w`: of longs (x86-64). */
static void int_op(emitter *e, bool w, int op)
{
    switch (op) {
    case OP_ADD: alu_rr(e, w, G_ADD, RAX, RCX); break;
    case OP_SUB: alu_rr(e, w, G_SUB, RAX, RCX); break;
    case OP_RSB: op_rr(e, 0, w, 0xF7, 3, RAX); alu_rr(e, w, G_ADD, RAX, RCX); break;   /* -eax + ecx */
    case OP_MUL: op_rr(e, 0, w, 0xAF0F, RAX, RCX); break;      /* imul eax, ecx */
    case OP_DIV: divide(e, w, false); break;
    case OP_REM: divide(e, w, true); break;
    case OP_AND: alu_rr(e, w, G_AND, RAX, RCX); break;
    case OP_OR: alu_rr(e, w, G_OR, RAX, RCX); break;
    case OP_XOR: alu_rr(e, w, G_XOR, RAX, RCX); break;
    case OP_SHL: op_rr(e, 0, w, 0xD3, 4, RAX); break;
    case OP_SHR: op_rr(e, 0, w, 0xD3, 7, RAX); break;
    default: op_rr(e, 0, w, 0xD3, 5, RAX); break;
    }
}

/* binop and binop/2addr: vD = vA op vB, a divisor of zero an exit (the interpreter throws ArithmeticException). */
static void emit_arith(emitter *e, u32 pc, int op, u32 vd, u32 va, u32 vb)
{
    ld_int(e, RAX, va);
    ld_int(e, RCX, vb);
    if (op == OP_DIV || op == OP_REM) {
        test_rr(e, false, RCX, RCX);
        fix(e, FIX_EXIT, pc, C_EQ);
    }
    int_op(e, false, op);
    st_word(e, RAX, vd);
}

/* binop/lit16 and binop/lit8: vD = vA op lit (never a divisor of zero: the front end hands that back). */
static void emit_arith_lit(emitter *e, int op, u32 vd, u32 va, i32 lit)
{
    ld_int(e, RAX, va);
    switch (op) {
    case OP_ADD: alu_ri(e, false, G_ADD, RAX, lit); break;
    case OP_AND: alu_ri(e, false, G_AND, RAX, lit); break;
    case OP_OR: alu_ri(e, false, G_OR, RAX, lit); break;
    case OP_XOR: alu_ri(e, false, G_XOR, RAX, lit); break;
    case OP_MUL:
        op_rr(e, 0, false, 0x69, RAX, RAX);                      /* imul eax, eax, imm32 */
        b4(e, (u32)lit);
        break;
    case OP_SHL: case OP_SHR: case OP_USHR:
        op_rr(e, 0, false, 0xC1, op == OP_SHL ? 4 : op == OP_SHR ? 7 : 5, RAX);
        b1(e, (u32)lit & 31);                                    /* Java's count, its low five bits */
        break;
    default:
        mov_i32(e, RCX, (u32)lit);
        int_op(e, false, op);
        break;
    }
    st_word(e, RAX, vd);
}

/* ------------------------------------------------------------------ moves and constants */

static void emit_move(emitter *e, u32 vd, u32 vs)
{
    ld_word(e, RAX, vs);
    st_word(e, RAX, vd);
}

/* (Both read before either is written: the pairs may overlap.) */
static void emit_move_wide(emitter *e, u32 vd, u32 vs)
{
    ld_word(e, RAX, vs);
    ld_word(e, RCX, vs + 1);
    st_word(e, RAX, vd);
    st_word(e, RCX, vd + 1);
}

/* move-result, -wide, -object: a compiled call's result (ctx->result), an int's low half alone, as the interpreter reads result.i. */
static void emit_move_result(emitter *e, int type, u32 vd)
{
    if (type == T_WIDE) {
        op_rm(e, 0, false, 0x8B, RAX, XCTX, JIT_CTX_RESULT);
        op_rm(e, 0, false, 0x8B, RCX, XCTX, JIT_CTX_RESULT + 4);
        st_word(e, RAX, vd);
        st_word(e, RCX, vd + 1);
        return;
    }
    op_rm(e, 0, type == T_OBJECT && X64, 0x8B, RAX, XCTX, JIT_CTX_RESULT);
    st_word(e, RAX, vd);
}

/* The value of a return for ctx->result, whole (`type` -1 for return-void); the front end then branches to the way out. */
static void emit_return(emitter *e, int type, u32 va)
{
    if (type < 0) {
        alu_rr(e, false, G_XOR, RAX, RAX);
        alu_rr(e, false, G_XOR, RCX, RCX);
    } else if (type == T_WIDE) {
        ld_int(e, RAX, va);
        ld_int(e, RCX, va + 1);
    } else {
        if (type == T_OBJECT) ld_word(e, RAX, va);
        else ld_int(e, RAX, va);
        if (!X64 || type != T_OBJECT) alu_rr(e, false, G_XOR, RCX, RCX);
    }
#if X64
    if (type == T_OBJECT) {
        op_rm(e, 0, true, 0x89, RAX, XCTX, JIT_CTX_RESULT);
        return;
    }
#endif
    op_rm(e, 0, false, 0x89, RAX, XCTX, JIT_CTX_RESULT);
    op_rm(e, 0, false, 0x89, RCX, XCTX, JIT_CTX_RESULT + 4);
}

static void emit_const(emitter *e, u32 vd, u32 v)
{
    mov_i32(e, RAX, v);
    st_word(e, RAX, vd);
}

static void emit_const_wide(emitter *e, u32 vd, u64 v)
{
    mov_i32(e, RAX, (u32)v);
    mov_i32(e, RCX, (u32)(v >> 32));
    st_word(e, RAX, vd);
    st_word(e, RCX, vd + 1);
}

/* A reference the code holds as a constant: a string already made (interned, never freed). */
static void emit_const_object(emitter *e, u32 vd, const void *p)
{
    mov_ptr(e, RAX, p);
    st_word(e, RAX, vd);
}

/* ------------------------------------------------------------------ branches on values */

/* if-test vA, vB: if-eq and if-ne on whole registers (a reference is a whole one), the others on ints. */
static u8 emit_compare(emitter *e, u32 op, u32 va, u32 vb)
{
    bool whole = op == 0x32 || op == 0x33;
    if (whole) ld_word(e, RAX, va);
    else ld_int(e, RAX, va);
    op_rm(e, 0, whole && X64, 0x3B, RAX, XREGS, at_v(vb));       /* cmp rax, [vb] */
    return if_cond(op - 0x32);
}

/* if-testz vAA. */
static u8 emit_compare_zero(emitter *e, u32 op, u32 va)
{
    cmp_mi(e, (op == 0x38 || op == 0x39) ? SLOT : 4, XREGS, at_v(va), 0);
    return if_cond(op - 0x38);
}

/* A switch's value, in eax (a packed switch's less its first key), then each key compared with it: equal is its case. */
static void emit_switch_value(emitter *e, u32 va, bool packed, i32 first)
{
    ld_int(e, RAX, va);
    if (packed && first) alu_ri(e, false, G_SUB, RAX, first);
}

static void emit_switch_key(emitter *e, i32 key) { alu_ri(e, false, G_CMP, RAX, key); }

/* ------------------------------------------------------------------ arrays and fields */

/* What each T_* is loaded with into eax/rax (MOV, MOVZX, MOVSX) and stored with (MOV of a dword, byte, word, qword). */
static void load_kind(emitter *e, int type, int r, int base, int index, i32 disp, bool indexed)
{
    static const u32 loads[] = { 0x8B, 0xB60F, 0xBE0F, 0xB70F, 0xBF0F, 0x8B, 0x8B };
    static const u8 scales[] = { 2, 0, 0, 1, 1, 3, X64 ? 3 : 2 };
    bool w = X64 && (type == T_WIDE || type == T_OBJECT);
    if (indexed) op_rx(e, 0, w, loads[type], r, base, index, scales[type]);
    else op_rm(e, 0, w, loads[type], r, base, disp);
}
static void store_kind(emitter *e, int type, int r, int base, int index, i32 disp, bool indexed)
{
    static const u8 scales[] = { 2, 0, 0, 1, 1, 3, X64 ? 3 : 2 };
    u32 pre = type == T_CHAR || type == T_SHORT ? 0x66 : 0, op = type == T_BOOLEAN || type == T_BYTE ? 0x88 : 0x89;
    bool w = X64 && (type == T_WIDE || type == T_OBJECT);
    if (indexed) x86_op(e, pre, w, op, r, true, base, index, scales[type], 0);
    else x86_op(e, pre, w, op, r, true, base, -1, 0, disp);
}

/* aget and aput of every kind but aput-object (the runtime's: the store check): vR = vA[vI], or vA[vI] = vR. */
static void emit_array(emitter *e, u32 pc, bool store, int type, u32 vr, u32 va, u32 vi)
{
    static const u8 sizes[] = { 4, 1, 1, 2, 2, 8, 0 };
    /* The array and its index checked as the interpreter checks them -- null, the kind and element size it is, the bounds
       (unsigned: a negative index is past them) -- each failure an exit; then rdx the elements, rcx the index. */
    ld_word(e, RAX, va);
    test_rr(e, X64, RAX, RAX);
    fix(e, FIX_EXIT, pc, C_EQ);
    cmp_mi(e, 1, RAX, (i32)offsetof(mg_jobj, kind), sizes[type] ? MG_J_PRIM_ARRAY : MG_J_OBJ_ARRAY);
    fix(e, FIX_EXIT, pc, C_NE);
    if (sizes[type]) {
        cmp_mi(e, 1, RAX, (i32)offsetof(mg_jobj, u.arr.esz), sizes[type]);
        fix(e, FIX_EXIT, pc, C_NE);
    }
    ld_int(e, RCX, vi);
    op_rm(e, 0, false, 0x3B, RCX, RAX, (i32)offsetof(mg_jobj, u.arr.len));     /* cmp ecx, [rax + len] */
    fix(e, FIX_EXIT, pc, C_HS);
    op_rm(e, 0, X64, 0x8B, RDX, RAX, (i32)offsetof(mg_jobj, u.arr.data));
#if !X64
    if (type == T_WIDE) {
        /* the 8 bytes at edx + ecx * 8, a word at a time, as the two registers hold them */
        op_rx(e, 0, false, 0x8D, RDX, RDX, RCX, 3);              /* lea edx, [edx + ecx * 8] */
        if (store) {
            ld_int(e, RAX, vr);
            op_rm(e, 0, false, 0x89, RAX, RDX, 0);
            ld_int(e, RAX, vr + 1);
            op_rm(e, 0, false, 0x89, RAX, RDX, 4);
        } else {
            op_rm(e, 0, false, 0x8B, RAX, RDX, 0);
            op_rm(e, 0, false, 0x8B, RCX, RDX, 4);
            st_word(e, RAX, vr);
            st_word(e, RCX, vr + 1);
        }
        return;
    }
#endif
    if (store) {
#if X64
        if (type == T_WIDE) ld_wide(e, RAX, vr);
        else
#endif
        if (type == T_OBJECT) ld_word(e, RAX, vr);
        else ld_int(e, RAX, vr);
        store_kind(e, type, RAX, RDX, RCX, 0, true);
    } else {
        load_kind(e, type, RAX, RDX, RCX, 0, true);
#if X64
        if (type == T_WIDE) {
            st_wide(e, RAX, vr);
            return;
        }
#endif
        st_word(e, RAX, vr);
    }
}

static void emit_array_length(emitter *e, u32 pc, u32 vd, u32 va)
{
    ld_word(e, RAX, va);
    test_rr(e, X64, RAX, RAX);
    fix(e, FIX_EXIT, pc, C_EQ);
    op_rm(e, 0, false, 0x8B, RCX, RAX, (i32)offsetof(mg_jobj, u.arr.len));
    st_word(e, RCX, vd);
}

/* The field at [rax + off] read into vR, or written from it (rcx, rdx used). */
static void field_value(emitter *e, bool store, int type, u32 vr, u32 off)
{
#if X64
    if (type == T_WIDE) {
        if (store) {
            ld_wide(e, RCX, vr);
            op_rm(e, 0, true, 0x89, RCX, RAX, (i32)off);
        } else {
            op_rm(e, 0, true, 0x8B, RCX, RAX, (i32)off);
            st_wide(e, RCX, vr);
        }
        return;
    }
#else
    if (type == T_WIDE) {
        if (store) {
            ld_int(e, RCX, vr);
            ld_int(e, RDX, vr + 1);
            op_rm(e, 0, false, 0x89, RCX, RAX, (i32)off);
            op_rm(e, 0, false, 0x89, RDX, RAX, (i32)off + 4);
        } else {
            op_rm(e, 0, false, 0x8B, RCX, RAX, (i32)off);
            op_rm(e, 0, false, 0x8B, RDX, RAX, (i32)off + 4);
            st_word(e, RCX, vr);
            st_word(e, RDX, vr + 1);
        }
        return;
    }
#endif
    if (store) {
        if (type == T_OBJECT) ld_word(e, RCX, vr);
        else ld_int(e, RCX, vr);
        store_kind(e, type, RCX, RAX, -1, (i32)off, false);
    } else {
        load_kind(e, type, RCX, RAX, -1, (i32)off, false);
        st_word(e, RCX, vr);
    }
}

/*
 * iget and iput of a field resolved by now, at `off` in the object: the object checked for null and for having the slot
 * (an object the runtime keeps its fields for by name does not: the interpreter's slotless_field), each failure an exit.
 */
static void emit_instance_field(emitter *e, u32 pc, bool store, int type, u32 vr, u32 vo, u32 slot, u32 off)
{
    ld_word(e, RAX, vo);
    test_rr(e, X64, RAX, RAX);
    fix(e, FIX_EXIT, pc, C_EQ);
    op_rm(e, 0, X64, 0x8B, RCX, RAX, (i32)offsetof(mg_dvm_obj, cls));
    op_rm(e, 0, false, 0x81, G_CMP, RCX, (i32)offsetof(mg_dvm_class, nslots));       /* cmp dword [rcx + nslots], slot */
    b4(e, slot);
    fix(e, FIX_EXIT, pc, X86_CC(CC_BE));                                         /* nslots <= slot: none */
    field_value(e, store, type, vr, off);
}

/* sget and sput of a class initialised by now: its slot's address is the code's own constant. */
static void emit_static_field(emitter *e, bool store, int type, u32 vr, mg_jvalue *slot)
{
    mov_ptr(e, RAX, slot);
    field_value(e, store, type, vr, 0);
}

/* ------------------------------------------------------------------ longs */

#if X64
/* A long operation of the pairs vA and vB into vD, in rax and rcx (the shifts by the int vB, whose count the processor masks by 63). */
static bool emit_long(emitter *e, u32 op, u32 vd, u32 va, u32 vb)
{
    static const signed char ops[] = { OP_ADD, OP_SUB, OP_MUL, -1, -1, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_USHR };
    if (op > 10 || ops[op] < 0) return false;
    ld_wide(e, RAX, va);
    if (op >= 8) ld_int(e, RCX, vb);
    else ld_wide(e, RCX, vb);
    int_op(e, true, ops[op]);
    st_wide(e, RAX, vd);
    return true;
}

/* div-long and rem-long, a divisor of zero an exit. */
static void emit_long_divide(emitter *e, u32 pc, bool rem, u32 vd, u32 va, u32 vb)
{
    ld_wide(e, RCX, vb);
    test_rr(e, true, RCX, RCX);
    fix(e, FIX_EXIT, pc, C_EQ);
    ld_wide(e, RAX, va);
    divide(e, true, rem);
    st_wide(e, RAX, vd);
}

/* cmp-long: (a > b) - (a < b). */
static void emit_cmp_long(emitter *e, u32 vd, u32 va, u32 vb)
{
    ld_wide(e, RAX, va);
    ld_wide(e, RCX, vb);
    alu_rr(e, true, G_CMP, RAX, RCX);
    op_rr(e, 0, false, 0x9F0F, 0, RDX);                          /* setg dl */
    op_rr(e, 0, false, 0x9C0F, 0, RCX);                          /* setl cl */
    op_rr(e, 0, false, 0x28, RCX, RDX);                          /* sub dl, cl */
    op_rr(e, 0, false, 0xBE0F, RAX, RDX);                        /* movsx eax, dl */
    st_word(e, RAX, vd);
}
#else
/* The pair vA in edx:eax. */
static void ld_pair(emitter *e, u32 v)
{
    ld_int(e, RAX, v);
    ld_int(e, RDX, v + 1);
}
static void st_pair(emitter *e, u32 v)
{
    st_word(e, RAX, v);
    st_word(e, RDX, v + 1);
}

/*
 * A long operation of the pairs vA and vB into vD, a word at a time in edx:eax (op as numbered from add-long; the shifts by
 * the int vB, its count masked by 63 here, as SHLD and SHL take only five bits of it). False for division and remainder.
 */
static bool emit_long(emitter *e, u32 op, u32 vd, u32 va, u32 vb)
{
    u32 small;
    if (op == 3 || op == 4 || op > 10) return false;
    if (op == 2) {
        /* (hi:lo) = lo * lo' (unsigned, 64 bits) + (hi * lo' + lo * hi') << 32 */
        ld_int(e, RAX, va + 1);
        op_rm(e, 0, false, 0xAF0F, RAX, XREGS, at_v(vb));       /* imul eax, [b.lo] */
        ld_int(e, RCX, vb + 1);
        op_rm(e, 0, false, 0xAF0F, RCX, XREGS, at_v(va));       /* imul ecx, [a.lo] */
        alu_rr(e, false, G_ADD, RCX, RAX);
        ld_int(e, RAX, va);
        op_rm(e, 0, false, 0xF7, 4, XREGS, at_v(vb));           /* mul dword [b.lo]: edx:eax */
        alu_rr(e, false, G_ADD, RDX, RCX);
        st_pair(e, vd);
        return true;
    }
    ld_pair(e, va);
    if (op >= 8) {
        ld_int(e, RCX, vb);
        alu_ri(e, false, G_AND, RCX, 63);
        if (op == 8) {
            op_rr(e, 0, false, 0xA50F, RAX, RDX);                /* shld edx, eax, cl */
            op_rr(e, 0, false, 0xD3, 4, RAX);                    /* shl eax, cl */
        } else {
            op_rr(e, 0, false, 0xAD0F, RDX, RAX);                /* shrd eax, edx, cl */
            op_rr(e, 0, false, 0xD3, op == 9 ? 7 : 5, RDX);      /* sar, shr edx, cl */
        }
        /* a count of 32 or more: the word shifted into the other's place, and the one it leaves 0 or the sign */
        op_rr(e, 0, false, 0xF6, 0, RCX);                        /* test cl, 32 */
        b1(e, 32);
        small = local_branch(e, C_EQ);
        if (op == 8) {
            mov_rr(e, false, RDX, RAX);
            alu_rr(e, false, G_XOR, RAX, RAX);
        } else {
            mov_rr(e, false, RAX, RDX);
            if (op == 9) {
                op_rr(e, 0, false, 0xC1, 7, RDX);                /* sar edx, 31 */
                b1(e, 31);
            } else {
                alu_rr(e, false, G_XOR, RDX, RDX);
            }
        }
        local_to(e, small, C_EQ);
    } else {
        static const u8 lo_ops[] = { G_ADD, G_SUB, 0, 0, 0, G_AND, G_OR, G_XOR }, hi_ops[] = { 2, 3, 0, 0, 0, G_AND, G_OR, G_XOR };
        op_rm(e, 0, false, (u32)(lo_ops[op] << 3 | 3), RAX, XREGS, at_v(vb));      /* op eax, [b.lo] */
        op_rm(e, 0, false, (u32)(hi_ops[op] << 3 | 3), RDX, XREGS, at_v(vb + 1));  /* op edx, [b.hi]: adc, sbb */
    }
    st_pair(e, vd);
    return true;
}

/* div-long and rem-long by a call (cdecl: the two longs on the stack, the result in edx:eax), a divisor of zero an exit. */
static i64 jit_div_long(i64 a, i64 b) { return a == INT64_MIN && b == -1 ? a : a / b; }
static i64 jit_rem_long(i64 a, i64 b) { return a == INT64_MIN && b == -1 ? 0 : a % b; }

static void emit_long_divide(emitter *e, u32 pc, bool rem, u32 vd, u32 va, u32 vb)
{
    ld_int(e, RAX, vb);
    op_rm(e, 0, false, 0x0B, RAX, XREGS, at_v(vb + 1));          /* or eax, [b.hi] */
    fix(e, FIX_EXIT, pc, C_EQ);
    ld_pair(e, va);
    op_rm(e, 0, false, 0x89, RAX, RSP, 0);
    op_rm(e, 0, false, 0x89, RDX, RSP, 4);
    ld_pair(e, vb);
    op_rm(e, 0, false, 0x89, RAX, RSP, 8);
    op_rm(e, 0, false, 0x89, RDX, RSP, 12);
    call_c(e, rem ? (mg_addr)jit_rem_long : (mg_addr)jit_div_long);
    st_pair(e, vd);
}

/* cmp-long: the high words compared signed, the low ones unsigned. */
static void emit_cmp_long(emitter *e, u32 vd, u32 va, u32 vb)
{
    u32 lt, gt, lo, hi, zero_done, minus_done;
    ld_int(e, RAX, va + 1);
    op_rm(e, 0, false, 0x3B, RAX, XREGS, at_v(vb + 1));
    lt = local_branch(e, C_LT);
    gt = local_branch(e, C_GT);
    ld_int(e, RAX, va);
    op_rm(e, 0, false, 0x3B, RAX, XREGS, at_v(vb));
    lo = local_branch(e, C_LO);
    hi = local_branch(e, C_HI);
    mov_i32(e, RAX, 0);
    zero_done = local_branch(e, C_AL);
    local_to(e, lt, C_LT);
    local_to(e, lo, C_LO);
    mov_i32(e, RAX, 0xFFFFFFFFu);
    minus_done = local_branch(e, C_AL);
    local_to(e, gt, C_GT);
    local_to(e, hi, C_HI);
    mov_i32(e, RAX, 1);
    local_to(e, zero_done, C_AL);
    local_to(e, minus_done, C_AL);
    st_word(e, RAX, vd);
}
#endif

/* neg-int, not-int, neg-long, not-long, int-to-long, long-to-int, int-to-byte, -char, -short: vA = op vB. */
static void emit_unary(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x7b: case 0x7c:                                        /* neg-int, not-int */
        ld_int(e, RAX, vb);
        op_rr(e, 0, false, 0xF7, op == 0x7b ? 3 : 2, RAX);
        st_word(e, RAX, va);
        break;
#if X64
    case 0x7d: case 0x7e:                                        /* neg-long, not-long */
        ld_wide(e, RAX, vb);
        op_rr(e, 0, true, 0xF7, op == 0x7d ? 3 : 2, RAX);
        st_wide(e, RAX, va);
        break;
    case 0x81:                                                   /* int-to-long */
        op_rm(e, 0, true, 0x63, RAX, XREGS, at_v(vb));           /* movsxd rax, dword [vb] */
        st_wide(e, RAX, va);
        break;
#else
    case 0x7d:                                                   /* neg-long: neg eax; adc edx, 0; neg edx */
        ld_pair(e, vb);
        op_rr(e, 0, false, 0xF7, 3, RAX);
        alu_ri(e, false, 2, RDX, 0);
        op_rr(e, 0, false, 0xF7, 3, RDX);
        st_pair(e, va);
        break;
    case 0x7e:                                                   /* not-long */
        ld_pair(e, vb);
        op_rr(e, 0, false, 0xF7, 2, RAX);
        op_rr(e, 0, false, 0xF7, 2, RDX);
        st_pair(e, va);
        break;
    case 0x81:                                                   /* int-to-long: cdq */
        ld_int(e, RAX, vb);
        b1(e, 0x99);
        st_pair(e, va);
        break;
#endif
    case 0x84:                                                   /* long-to-int: the low half */
        ld_int(e, RAX, vb);
        st_word(e, RAX, va);
        break;
    default:                                                     /* int-to-byte, -char, -short: from the register's low byte or word */
        op_rm(e, 0, false, op == 0x8d ? 0xBE0F : op == 0x8e ? 0xB70F : 0xBF0F, RAX, XREGS, at_v(vb));
        st_word(e, RAX, va);
        break;
    }
}

/* ------------------------------------------------------------------ floating point */

/* SSE's scalar arithmetic, single (F3) or double (F2): add, sub, mul, div. */
static void emit_fp_arith(emitter *e, u32 op, bool dbl, u32 vd, u32 va, u32 vb)
{
    static const u32 ops[] = { 0x580F, 0x5C0F, 0x590F, 0x5E0F };
    if (dbl) {
        ld_double(e, XMM0, va);
        ld_double(e, XMM1, vb);
        op_rr(e, 0xF2, false, ops[op], XMM0, XMM1);
        st_double(e, XMM0, vd);
    } else {
        ld_float(e, XMM0, va);
        ld_float(e, XMM1, vb);
        op_rr(e, 0xF3, false, ops[op], XMM0, XMM1);
        st_float(e, XMM0, vd);
    }
}

/*
 * cmpl and cmpg by UCOMISS or UCOMISD, which say unordered by PF (and set CF and ZF with it): -1, 0 or 1, and for NaN either
 * side -1 (cmpl) or 1 (cmpg). The MOVs between leave the flags.
 */
static void emit_fp_compare(emitter *e, bool dbl, bool greater, u32 vd, u32 va, u32 vb)
{
    u32 nan, first, equal;
    if (dbl) {
        ld_double(e, XMM0, va);
        ld_double(e, XMM1, vb);
    } else {
        ld_float(e, XMM0, va);
        ld_float(e, XMM1, vb);
    }
    op_rr(e, dbl ? 0x66 : 0, false, 0x2E0F, XMM0, XMM1);       /* ucomis xmm0, xmm1 */
    mov_i32(e, RAX, greater ? 1u : 0xFFFFFFFFu);
    nan = local_branch(e, X86_CC(CC_P));
    first = local_branch(e, X86_CC(greater ? CC_A : CC_B));     /* cmpg: a > b; cmpl: a < b -- the same as NaN's */
    mov_i32(e, RAX, 0);
    equal = local_branch(e, X86_CC(CC_E));
    mov_i32(e, RAX, greater ? 0xFFFFFFFFu : 1u);
    local_to(e, nan, X86_CC(CC_P));
    local_to(e, first, X86_CC(greater ? CC_A : CC_B));
    local_to(e, equal, X86_CC(CC_E));
    st_word(e, RAX, vd);
}

/*
 * eax (rax for `w`) as Java converts xmm0 to an int (a long): CVTTSS2SI or CVTTSD2SI, whose MIN_VALUE for NaN and for what
 * is out of range is made 0 for NaN, MAX_VALUE for what is too large, and kept for what is too small (or is MIN_VALUE).
 */
static void java_convert(emitter *e, bool dbl, bool w)
{
    u32 fine, nan, low;
    op_rr(e, dbl ? 0xF2 : 0xF3, w, 0x2C0F, RAX, XMM0);          /* cvtts?2si */
    alu_ri(e, w, G_CMP, RAX, 1);                                 /* overflows only for MIN_VALUE */
    fine = local_branch(e, X86_CC(CC_NO));
    op_rr(e, dbl ? 0x66 : 0, false, 0x2E0F, XMM0, XMM0);        /* ucomis xmm0, xmm0: NaN unordered */
    mov_i32(e, RAX, 0);
    nan = local_branch(e, X86_CC(CC_P));
    op_rr(e, 0, false, 0x570F, XMM1, XMM1);                      /* xorps xmm1, xmm1 */
    op_rr(e, dbl ? 0x66 : 0, false, 0x2E0F, XMM0, XMM1);        /* below 0: MIN_VALUE */
    low = local_branch(e, X86_CC(CC_B));
#if X64
    if (w) {
        b1(e, 0x48);
        b1(e, 0xB8);                                             /* mov rax, MAX_VALUE */
        b4(e, 0xFFFFFFFFu);
        b4(e, 0x7FFFFFFFu);
    } else
#endif
        mov_i32(e, RAX, 0x7FFFFFFFu);
    local_to(e, fine, X86_CC(CC_NO));
    local_to(e, nan, X86_CC(CC_P));
    {
        /* (too small: MIN_VALUE again, eax having been made 0 for NaN's sake) */
        u32 done = local_branch(e, C_AL);
        local_to(e, low, X86_CC(CC_B));
#if X64
        if (w) {
            b1(e, 0x48);
            b1(e, 0xB8);
            b4(e, 0);
            b4(e, 0x80000000u);
        } else
#endif
            mov_i32(e, RAX, 0x80000000u);
        local_to(e, done, C_AL);
    }
}

/* The conversions between int, float and double, and neg: vA = (kind) vB. False for none. */
static bool emit_fp_convert(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x82: case 0x83:                                        /* int-to-float, -double: cvtsi2ss, cvtsi2sd */
        ld_int(e, RAX, vb);
        op_rr(e, op == 0x82 ? 0xF3 : 0xF2, false, 0x2A0F, XMM0, RAX);
        if (op == 0x82) st_float(e, XMM0, va);
        else st_double(e, XMM0, va);
        return true;
    case 0x87: case 0x8a:                                        /* float-to-int, double-to-int */
        if (op == 0x87) ld_float(e, XMM0, vb);
        else ld_double(e, XMM0, vb);
        java_convert(e, op == 0x8a, false);
        st_word(e, RAX, va);
        return true;
    case 0x89:                                                   /* float-to-double: cvtss2sd */
        ld_float(e, XMM0, vb);
        op_rr(e, 0xF3, false, 0x5A0F, XMM0, XMM0);
        st_double(e, XMM0, va);
        return true;
    case 0x8c:                                                   /* double-to-float: cvtsd2ss */
        ld_double(e, XMM0, vb);
        op_rr(e, 0xF2, false, 0x5A0F, XMM0, XMM0);
        st_float(e, XMM0, va);
        return true;
    case 0x7f:                                                   /* neg-float: its sign bit flipped */
        ld_int(e, RAX, vb);
        alu_ri(e, false, G_XOR, RAX, (i32)0x80000000u);
        st_word(e, RAX, va);
        return true;
    case 0x80:                                                   /* neg-double: the high word's sign bit */
        ld_word(e, RAX, vb);
        ld_int(e, RCX, vb + 1);
        alu_ri(e, false, G_XOR, RCX, (i32)0x80000000u);
        st_word(e, RAX, va);
        st_word(e, RCX, va + 1);
        return true;
    default:
        return false;
    }
}

#if X64
/* long-to-float, long-to-double (CVTSI2SS and CVTSI2SD of a 64-bit register), float-to-long, double-to-long. */
static void emit_long_convert(emitter *e, u32 op, u32 va, u32 vb)
{
    if (op == 0x85 || op == 0x86) {
        ld_wide(e, RAX, vb);
        op_rr(e, op == 0x85 ? 0xF3 : 0xF2, true, 0x2A0F, XMM0, RAX);
        if (op == 0x85) st_float(e, XMM0, va);
        else st_double(e, XMM0, va);
    } else {
        if (op == 0x88) ld_float(e, XMM0, vb);
        else ld_double(e, XMM0, vb);
        java_convert(e, op == 0x8b, true);
        st_wide(e, RAX, va);
    }
}

/* A float's and a double's remainder: fmod's, by a call (System V: the operands in xmm0 and xmm1, the result in xmm0). */
static float jit_rem_float(float a, float b) { return fmodf(a, b); }
static double jit_rem_double(double a, double b) { return fmod(a, b); }

static void emit_fp_remainder(emitter *e, bool dbl, u32 vd, u32 va, u32 vb)
{
    if (dbl) {
        ld_double(e, XMM0, va);
        ld_double(e, XMM1, vb);
        call_c(e, (mg_addr)jit_rem_double);
        st_double(e, XMM0, vd);
    } else {
        ld_float(e, XMM0, va);
        ld_float(e, XMM1, vb);
        call_c(e, (mg_addr)jit_rem_float);
        st_float(e, XMM0, vd);
    }
}
#else
/*
 * A float's and a double's remainder, fmod's, by a call of this, which reads the frame's registers and writes the result
 * into them: i386's convention returns a float or a double in x87's st(0), which the code leaves alone.
 */
static void jit_fp_remainder(mg_dvm_reg *regs, u32 dbl, u32 vd, u32 va, u32 vb)
{
    if (dbl) {
        double a, b, r;
        memcpy(&a, &regs[va], 8);
        memcpy(&b, &regs[vb], 8);
        r = fmod(a, b);
        memcpy(&regs[vd], &r, 8);
    } else {
        float a, b, r;
        memcpy(&a, &regs[va], 4);
        memcpy(&b, &regs[vb], 4);
        r = fmodf(a, b);
        memcpy(&regs[vd], &r, 4);
    }
}

/* A call of it: its arguments at [esp], the frame's registers first. */
static void call_regs(emitter *e, mg_addr fn, u32 a, u32 b, u32 c, u32 d)
{
    op_rm(e, 0, false, 0x89, XREGS, RSP, 0);                     /* mov [esp], ebx */
    op_rm(e, 0, false, 0xC7, 0, RSP, 4);
    b4(e, a);
    op_rm(e, 0, false, 0xC7, 0, RSP, 8);
    b4(e, b);
    op_rm(e, 0, false, 0xC7, 0, RSP, 12);
    b4(e, c);
    op_rm(e, 0, false, 0xC7, 0, RSP, 16);
    b4(e, d);
    call_c(e, fn);
}

/*
 * A long's conversions to and from float and double, which 32-bit x86 has no instruction for, in SSE2 from the long's two
 * words. The constants are powers of two, made where they are needed, as no constant lies in the code's memory: xmm<x> =
 * the double whose high word is `hi` and low word 0 (MOVD, then PSLLQ by 32), through the general register `tmp`.
 */
static void double_hi(emitter *e, int x, u32 hi, int tmp)
{
    mov_i32(e, tmp, hi);
    op_rr(e, 0x66, false, 0x6E0F, x, tmp);                       /* movd xmm<x>, tmp */
    op_rr(e, 0x66, false, 0x730F, 6, x);                         /* psllq xmm<x>, 32 */
    b1(e, 32);
}
enum { D_2P31 = 0x41E00000u, D_2P32 = 0x41F00000u, D_2P63 = 0x43E00000u, D_2PM32 = 0x3DF00000u };

/* xmm<x> += the unsigned word in `r` (r changed; xmm<tmp_x> and xmm3 used): r less 2^31 converted signed, 2^31 added back, both exact. */
static void add_unsigned(emitter *e, int x, int r, int tmp_x)
{
    alu_ri(e, false, G_XOR, r, (i32)0x80000000u);
    op_rr(e, 0xF2, false, 0x2A0F, tmp_x, r);                     /* cvtsi2sd */
    double_hi(e, XMM3, D_2P31, r);
    op_rr(e, 0xF2, false, 0x580F, tmp_x, XMM3);                  /* addsd: the word, exact */
    op_rr(e, 0xF2, false, 0x580F, x, tmp_x);
}

/*
 * long-to-double: the high word, signed, times 2^32 and the low word, unsigned -- both exact -- added: the one rounding,
 * which is the conversion's own. The result in xmm0.
 */
static void long_to_double(emitter *e, u32 vb)
{
    op_rm(e, 0xF2, false, 0x2A0F, XMM0, XREGS, at_v(vb + 1));   /* cvtsi2sd xmm0, [the high word] */
    double_hi(e, XMM1, D_2P32, RAX);
    op_rr(e, 0xF2, false, 0x590F, XMM0, XMM1);                   /* mulsd: exact */
    ld_int(e, RAX, vb);
    add_unsigned(e, XMM0, RAX, XMM1);
}

/*
 * long-to-float: an int's CVTSI2SS where the long is one. Otherwise its magnitude made a double exactly, then CVTSD2SS
 * once, and the sign put back: from 2^53 on a double would round the magnitude itself, and that second rounding could
 * differ from the one Java makes, so there the bits under the thirteenth -- below the float's last bit and its rounding
 * bit, where only "any of them set" counts -- are first made the twelfth bit alone when any is set, and the magnitude
 * fits a double's 53 bits. The result in xmm0.
 */
static void long_to_float(emitter *e, u32 vb)
{
    u32 wide, done, positive, exact, same;
    ld_int(e, RAX, vb);
    ld_int(e, RDX, vb + 1);
    mov_rr(e, false, RCX, RAX);
    op_rr(e, 0, false, 0xC1, 7, RCX);                            /* sar ecx, 31 */
    b1(e, 31);
    alu_rr(e, false, G_CMP, RCX, RDX);
    wide = local_branch(e, C_NE);
    op_rr(e, 0xF3, false, 0x2A0F, XMM0, RAX);                    /* an int's: cvtsi2ss xmm0, eax */
    done = local_branch(e, C_AL);
    local_to(e, wide, C_NE);
    mov_rr(e, false, RCX, RDX);                                  /* the sign, for the end */
    test_rr(e, false, RDX, RDX);
    positive = local_branch(e, X86_CC(CC_NS));
    op_rr(e, 0, false, 0xF7, 3, RAX);                            /* the magnitude: neg eax; adc edx, 0; neg edx */
    alu_ri(e, false, 2, RDX, 0);
    op_rr(e, 0, false, 0xF7, 3, RDX);
    local_to(e, positive, X86_CC(CC_NS));
    alu_ri(e, false, G_CMP, RDX, 0x00200000);                    /* under 2^53: exact as it is */
    exact = local_branch(e, C_LO);
    op_rr(e, 0, false, 0xF7, 0, RAX);                            /* test eax, 0xfff */
    b4(e, 0xFFF);
    same = local_branch(e, C_EQ);
    alu_ri(e, false, G_AND, RAX, (i32)0xFFFFF000u);
    alu_ri(e, false, G_OR, RAX, 0x800);
    local_to(e, exact, C_LO);
    local_to(e, same, C_EQ);
    /* both words unsigned: (hi - 2^31 + 2^31) * 2^32 + lo, exact */
    alu_ri(e, false, G_XOR, RDX, (i32)0x80000000u);
    op_rr(e, 0xF2, false, 0x2A0F, XMM0, RDX);
    double_hi(e, XMM3, D_2P31, RDX);
    op_rr(e, 0xF2, false, 0x580F, XMM0, XMM3);
    double_hi(e, XMM1, D_2P32, RDX);
    op_rr(e, 0xF2, false, 0x590F, XMM0, XMM1);
    add_unsigned(e, XMM0, RAX, XMM1);
    op_rr(e, 0xF2, false, 0x5A0F, XMM0, XMM0);                   /* cvtsd2ss: the one rounding */
    test_rr(e, false, RCX, RCX);
    positive = local_branch(e, X86_CC(CC_NS));
    op_rr(e, 0x66, false, 0x7E0F, XMM0, RAX);                    /* movd eax, xmm0 */
    alu_ri(e, false, G_XOR, RAX, (i32)0x80000000u);
    op_rr(e, 0x66, false, 0x6E0F, XMM0, RAX);                    /* movd xmm0, eax */
    local_to(e, positive, X86_CC(CC_NS));
    local_to(e, done, C_AL);
}

/*
 * edx:eax as Java converts the double in xmm0 to a long: 0 for NaN; MAX_VALUE or MIN_VALUE from 2^63 on in magnitude;
 * else toward zero -- CVTTSD2SI where it fits an int, else the magnitude's high word (CVTTSD2SI of it times 2^-32, exact)
 * and its low word (of what is left, exact and under 2^32: less 2^31 first where it is not under that), negated where the
 * double is negative. ecx holds the sign.
 */
static void double_to_long(emitter *e)
{
    u32 nan, big, large, top, low_done, to_done[3], nd = 0, i, keep, keep2;
    op_rr(e, 0x66, false, 0x2E0F, XMM0, XMM0);                   /* ucomisd xmm0, xmm0 */
    nan = local_branch(e, X86_CC(CC_P));
    op_rr(e, 0x66, false, 0x500F, RCX, XMM0);                    /* movmskpd ecx, xmm0: the sign */
    alu_ri(e, false, G_AND, RCX, 1);
    op_rr(e, 0x66, false, 0x760F, XMM1, XMM1);                   /* pcmpeqd xmm1, xmm1 */
    op_rr(e, 0x66, false, 0x730F, 2, XMM1);                      /* psrlq xmm1, 1 */
    b1(e, 1);
    op_rr(e, 0x66, false, 0x540F, XMM1, XMM0);                   /* andpd xmm1, xmm0: the magnitude */
    double_hi(e, XMM2, D_2P63, RAX);
    op_rr(e, 0x66, false, 0x2E0F, XMM1, XMM2);
    big = local_branch(e, X86_CC(CC_AE));
    double_hi(e, XMM2, D_2P31, RAX);
    op_rr(e, 0x66, false, 0x2E0F, XMM1, XMM2);
    large = local_branch(e, X86_CC(CC_AE));
    op_rr(e, 0xF2, false, 0x2C0F, RAX, XMM0);                    /* an int's: cvttsd2si eax, xmm0; cdq */
    b1(e, 0x99);
    to_done[nd++] = local_branch(e, C_AL);
    local_to(e, large, X86_CC(CC_AE));
    double_hi(e, XMM2, D_2PM32, RAX);
    op_rr(e, 0x66, false, 0x280F, XMM3, XMM1);                   /* movapd xmm3, xmm1 */
    op_rr(e, 0xF2, false, 0x590F, XMM3, XMM2);                   /* mulsd: exact */
    op_rr(e, 0xF2, false, 0x2C0F, RDX, XMM3);                    /* cvttsd2si edx: the high word */
    op_rr(e, 0xF2, false, 0x2A0F, XMM3, RDX);
    double_hi(e, XMM2, D_2P32, RAX);
    op_rr(e, 0xF2, false, 0x590F, XMM3, XMM2);
    op_rr(e, 0xF2, false, 0x5C0F, XMM1, XMM3);                   /* subsd: what is left, exact */
    double_hi(e, XMM2, D_2P31, RAX);
    op_rr(e, 0x66, false, 0x2E0F, XMM1, XMM2);
    top = local_branch(e, X86_CC(CC_AE));
    op_rr(e, 0xF2, false, 0x2C0F, RAX, XMM1);
    low_done = local_branch(e, C_AL);
    local_to(e, top, X86_CC(CC_AE));
    op_rr(e, 0xF2, false, 0x5C0F, XMM1, XMM2);
    op_rr(e, 0xF2, false, 0x2C0F, RAX, XMM1);
    alu_ri(e, false, G_XOR, RAX, (i32)0x80000000u);
    local_to(e, low_done, C_AL);
    test_rr(e, false, RCX, RCX);                                 /* negated where the double is negative */
    keep = local_branch(e, C_EQ);
    op_rr(e, 0, false, 0xF7, 3, RAX);
    alu_ri(e, false, 2, RDX, 0);
    op_rr(e, 0, false, 0xF7, 3, RDX);
    local_to(e, keep, C_EQ);
    to_done[nd++] = local_branch(e, C_AL);
    local_to(e, big, X86_CC(CC_AE));                             /* 2^63 or more: MAX_VALUE, or MIN_VALUE where negative */
    mov_i32(e, RAX, 0xFFFFFFFFu);
    mov_i32(e, RDX, 0x7FFFFFFFu);
    test_rr(e, false, RCX, RCX);
    keep2 = local_branch(e, C_EQ);
    mov_i32(e, RAX, 0);
    mov_i32(e, RDX, 0x80000000u);
    local_to(e, keep2, C_EQ);
    to_done[nd++] = local_branch(e, C_AL);
    local_to(e, nan, X86_CC(CC_P));                              /* NaN: 0 */
    mov_i32(e, RAX, 0);
    mov_i32(e, RDX, 0);
    for (i = 0; i < nd; i++) local_to(e, to_done[i], C_AL);
}

/* long-to-float, long-to-double, float-to-long (the float made a double first, exactly), double-to-long. */
static void emit_long_convert(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x85:
        long_to_float(e, vb);
        st_float(e, XMM0, va);
        break;
    case 0x86:
        long_to_double(e, vb);
        st_double(e, XMM0, va);
        break;
    default:
        if (op == 0x88) {
            ld_float(e, XMM0, vb);
            op_rr(e, 0xF3, false, 0x5A0F, XMM0, XMM0);           /* cvtss2sd */
        } else {
            ld_double(e, XMM0, vb);
        }
        double_to_long(e);
        st_pair(e, va);
        break;
    }
}

static void emit_fp_remainder(emitter *e, bool dbl, u32 vd, u32 va, u32 vb)
{
    call_regs(e, (mg_addr)jit_fp_remainder, dbl ? 1u : 0u, vd, va, vb);
}
#endif

/* ------------------------------------------------------------------ calls */

/* The context's t and f as a call's first two arguments, and the pc as its third: x86-64's in rdi, rsi, rdx; x86's at [esp],
   [esp + 4], [esp + 8], in the room the code's frame keeps for them. */
static void args_t_f_pc(emitter *e, u32 pc)
{
#if X64
    op_rm(e, 0, true, 0x8B, RDI, XCTX, JIT_CTX_T);
    op_rm(e, 0, true, 0x8B, RSI, XCTX, JIT_CTX_F);
    mov_i32(e, RDX, pc);
#else
    op_rm(e, 0, false, 0x8B, RAX, XCTX, JIT_CTX_T);
    op_rm(e, 0, false, 0x89, RAX, RSP, 0);
    op_rm(e, 0, false, 0x8B, RAX, XCTX, JIT_CTX_F);
    op_rm(e, 0, false, 0x89, RAX, RSP, 4);
    op_rm(e, 0, false, 0xC7, 0, RSP, 8);
    b4(e, pc);
#endif
}

/*
 * An invoke: mg_dvm_jit_invoke(t, f, pc, &ctx->result), which does the call as the interpreter would; 1 back is an exception
 * pending (out, thrown here), 2 a call it leaves to the interpreter (out at the invoke, nothing done).
 */
static void emit_invoke(emitter *e, u32 pc)
{
    args_t_f_pc(e, pc);
#if X64
    op_rm(e, 0, true, 0x8D, RCX, XCTX, JIT_CTX_RESULT);          /* lea rcx, [r12 + result] */
#else
    op_rm(e, 0, false, 0x8D, RAX, XCTX, JIT_CTX_RESULT);
    op_rm(e, 0, false, 0x89, RAX, RSP, 12);
#endif
    call_c(e, (mg_addr)mg_dvm_jit_invoke);
    alu_ri(e, false, G_CMP, RAX, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    alu_ri(e, false, G_CMP, RAX, 2);
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
#if X64
    mov_ptr(e, RSI, ref);
    if (known) {
        op_rm(e, 0, true, 0x8B, RAX, RSI, (i32)offsetof(mg_dvm_method, jit));
        test_rr(e, true, RAX, RAX);
        slow[nslow++] = local_branch(e, C_EQ);
        op_rm(e, 0, true, 0x8B, RAX, RAX, (i32)offsetof(mg_dvm_jit_code, fast));
        test_rr(e, true, RAX, RAX);
        slow[nslow++] = local_branch(e, C_EQ);
    }
    mov_rr(e, true, RDI, XCTX);
    mov_i32(e, RDX, desc);
    mov_i32(e, RCX, pc);
#else
    mov_ptr(e, RCX, ref);
    if (known) {
        op_rm(e, 0, false, 0x8B, RAX, RCX, (i32)offsetof(mg_dvm_method, jit));
        test_rr(e, false, RAX, RAX);
        slow[nslow++] = local_branch(e, C_EQ);
        op_rm(e, 0, false, 0x8B, RAX, RAX, (i32)offsetof(mg_dvm_jit_code, fast));
        test_rr(e, false, RAX, RAX);
        slow[nslow++] = local_branch(e, C_EQ);
    }
    op_rm(e, 0, false, 0x89, XCTX, RSP, 0);
    op_rm(e, 0, false, 0x89, RCX, RSP, 4);
    op_rm(e, 0, false, 0xC7, 0, RSP, 8);
    b4(e, desc);
    op_rm(e, 0, false, 0xC7, 0, RSP, 12);
    b4(e, pc);
#endif
    call_c(e, (mg_addr)mg_dvm_jit_fast);
    alu_ri(e, false, G_CMP, RAX, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    alu_ri(e, false, G_CMP, RAX, 3);
    done = local_branch(e, C_NE);
    for (i = 0; i < nslow; i++) local_to(e, slow[i], C_EQ);
    emit_invoke(e, pc);
    local_to(e, done, C_NE);
}

/* ctx->result = rcx, an int's or a reference's whole (its other half 0 on x86, where the result's 8 bytes are two words). */
static void result_from_rcx(emitter *e)
{
#if X64
    op_rm(e, 0, true, 0x89, RCX, XCTX, JIT_CTX_RESULT);
#else
    op_rm(e, 0, false, 0x89, RCX, XCTX, JIT_CTX_RESULT);
    op_rm(e, 0, false, 0xC7, 0, XCTX, JIT_CTX_RESULT + 4);
    b4(e, 0);
#endif
}

/*
 * The intrinsics that are a few instructions, as inline_call allows them (its receiver in rax, checked for null, when the
 * method is not static), each result whole in ctx->result. A null string or an index out of its bounds goes by the call
 * (mg_dvm_jit_invoke), which throws as the interpreter would; the others cannot fail.
 */
static void intrinsic_in_code(emitter *e, u8 id, u32 a0, u32 a1, u32 *fails, u8 *conds, u32 *nfails)
{
    switch (id) {
    case INTR_STRING_LENGTH:
        op_rm(e, 0, false, 0x8B, RCX, RAX, (i32)offsetof(mg_jobj, u.str.len16));
        result_from_rcx(e);
        break;
    case INTR_REFERENT:
        /* Reference.getReferent: the referent's slot (a reference object always has it, as a subclass of Reference). */
        op_rm(e, 0, X64, 0x8B, RCX, RAX, (i32)(MG_DVM_HEADER + g_dvm_referent_slot * sizeof(mg_jvalue)));
        result_from_rcx(e);
        break;
    case INTR_STRING_ISEMPTY:
        alu_rr(e, false, G_XOR, RCX, RCX);
        cmp_mi(e, 4, RAX, (i32)offsetof(mg_jobj, u.str.len16), 0);
        op_rr(e, 0, false, 0x940F, 0, RCX);                      /* sete cl */
        result_from_rcx(e);
        break;
    case INTR_STRING_CHARAT:
        ld_int(e, RCX, a1);
        op_rm(e, 0, false, 0x3B, RCX, RAX, (i32)offsetof(mg_jobj, u.str.len16));
        conds[*nfails] = C_HS;
        fails[(*nfails)++] = local_branch(e, C_HS);
        op_rm(e, 0, X64, 0x8B, RDX, RAX, (i32)offsetof(mg_jobj, u.str.u16));
        op_rx(e, 0, false, 0xB70F, RCX, RDX, RCX, 1);           /* movzx ecx, word [rdx + rcx * 2] */
        result_from_rcx(e);
        break;
    case INTR_MIN_I: case INTR_MAX_I:
        ld_int(e, RCX, a0);
        ld_int(e, RDX, a1);
        alu_rr(e, false, G_CMP, RCX, RDX);
        op_rr(e, 0, false, id == INTR_MIN_I ? 0x4F0F : 0x4C0F, RCX, RDX);   /* cmovg, cmovl ecx, edx: the second where the first is greater (min), less (max) */
        result_from_rcx(e);
        break;
    case INTR_ABS_I:
        /* (MIN_VALUE stays itself, as Java has it) */
        ld_int(e, RCX, a0);
        mov_rr(e, false, RDX, RCX);
        op_rr(e, 0, false, 0xC1, 7, RDX);                        /* sar edx, 31 */
        b1(e, 31);
        alu_rr(e, false, G_XOR, RCX, RDX);
        alu_rr(e, false, G_SUB, RCX, RDX);
        result_from_rcx(e);
        break;
    default:                                                     /* sqrt */
        ld_double(e, XMM0, a0);
        op_rr(e, 0xF2, false, 0x510F, XMM0, XMM0);              /* sqrtsd xmm0, xmm0 */
        op_rm(e, 0xF2, false, 0x110F, XMM0, XCTX, JIT_CTX_RESULT);  /* movsd [result], xmm0 */
        break;
    }
}

/* A call of a trivial method or an intrinsic done in the code, as the front end's inline_call found it may be. */
static void emit_inline_call(emitter *e, u32 pc, const inline_plan *p)
{
    u32 fails[6], i, nfails = 0, done;
    u8 conds[6];
    if (p->hooks) {
        /* The method hooked since (mg_dvm_hook.c): by mg_dvm_jit_invoke, which calls the hook. */
        mov_ptr(e, RCX, p->target);
        cmp_mi(e, 1, RCX, (i32)offsetof(mg_dvm_method, hooked), 0);
        conds[nfails] = C_NE;
        fails[nfails++] = local_branch(e, C_NE);
    }
    if (p->kind != DVM_INVOKE_STATIC) {
        ld_word(e, RAX, p->self);
        test_rr(e, X64, RAX, RAX);
        conds[nfails] = C_EQ;
        fails[nfails++] = local_branch(e, C_EQ);
        if (p->vtab) {
            op_rm(e, 0, X64, 0x8B, RCX, RAX, (i32)offsetof(mg_dvm_obj, cls));
            op_rm(e, 0, false, 0x81, G_CMP, RCX, (i32)offsetof(mg_dvm_class, nvtab));      /* nvtab <= vidx: none */
            b4(e, (u32)p->target->vidx);
            conds[nfails] = X86_CC(CC_BE);
            fails[nfails++] = local_branch(e, X86_CC(CC_BE));
            op_rm(e, 0, X64, 0x8B, RCX, RCX, (i32)offsetof(mg_dvm_class, vtab));
            mov_ptr(e, RDX, p->target);
            op_rm(e, 0, X64, 0x3B, RDX, RCX, (i32)((u32)p->target->vidx * sizeof(void *)));   /* cmp rdx, [vtab + vidx] */
            conds[nfails] = C_NE;
            fails[nfails++] = local_branch(e, C_NE);
        }
        if (p->slots) {
            op_rm(e, 0, X64, 0x8B, RCX, RAX, (i32)offsetof(mg_dvm_obj, cls));
            op_rm(e, 0, false, 0x81, G_CMP, RCX, (i32)offsetof(mg_dvm_class, nslots));
            b4(e, p->fld->slot);
            conds[nfails] = X86_CC(CC_BE);
            fails[nfails++] = local_branch(e, X86_CC(CC_BE));
        }
    }
    if (p->intrinsic) {
        intrinsic_in_code(e, p->intrinsic, p->self, p->value, fails, conds, &nfails);
    } else switch (SHAPE_OF(p->shape)) {
    case SHAPE_CONST:
        mov_i32(e, RCX, (u32)SHAPE_VALUE(p->shape));
        result_from_rcx(e);
        break;
    case SHAPE_STATIC_GETTER:
        mov_ptr(e, RAX, &p->fld->cls->statics[p->fld->slot]);
        /* fall through - read as a getter's field, at offset 0 */
    case SHAPE_GETTER:
#if !X64
        if (p->type == T_WIDE) {
            op_rm(e, 0, false, 0x8B, RCX, RAX, (i32)p->off);
            op_rm(e, 0, false, 0x8B, RDX, RAX, (i32)p->off + 4);
            op_rm(e, 0, false, 0x89, RCX, XCTX, JIT_CTX_RESULT);
            op_rm(e, 0, false, 0x89, RDX, XCTX, JIT_CTX_RESULT + 4);
            break;
        }
#endif
        load_kind(e, p->type, RCX, RAX, -1, (i32)p->off, false);
        result_from_rcx(e);
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
    args_t_f_pc(e, pc);
    call_c(e, (mg_addr)mg_dvm_jit_op);
    alu_ri(e, false, G_CMP, RAX, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    alu_ri(e, false, G_CMP, RAX, 2);
    fix(e, FIX_EXIT, pc, C_EQ);
}

/* check-cast and instance-of of class `c`, resolved: the code's own answer for null and for the very class, the runtime's for the rest. */
static void emit_type_test(emitter *e, u32 pc, bool instance_of, u32 vo, u32 vd, const mg_dvm_class *c)
{
    u32 null_done, same_done, slow_done;
    ld_word(e, RAX, vo);
    test_rr(e, X64, RAX, RAX);
    null_done = local_branch(e, C_EQ);
    op_rm(e, 0, X64, 0x8B, RCX, RAX, (i32)offsetof(mg_dvm_obj, cls));
    mov_ptr(e, RDX, c);
    alu_rr(e, X64, G_CMP, RCX, RDX);
    same_done = local_branch(e, C_EQ);
    emit_runtime(e, pc);
    slow_done = local_branch(e, C_AL);
    if (instance_of) {
        /* 1 for the very class; 0 for null, which rax is */
        local_to(e, same_done, C_EQ);
        mov_i32(e, RAX, 1);
        local_to(e, null_done, C_EQ);
        st_word(e, RAX, vd);
    } else {
        local_to(e, null_done, C_EQ);
        local_to(e, same_done, C_EQ);
    }
    local_to(e, slow_done, C_AL);
}

/* ------------------------------------------------------------------ the way in and out */

/*
 * x86-64: push rbp; mov rbp, rsp; push rbx, r12, r13, r14 (r14 for rsp's 16-byte alignment at the calls); r12 = ctx,
 * rbx = ctx->regs, r13 = ctx->slow; jmp rsi (the entry). x86: push ebp; mov ebp, esp; push ebx, esi, edi; sub esp, 28
 * (the calls' arguments, and esp 16-byte aligned at them, as Android's i386 code wants it); esi = ctx, ebx = ctx->regs,
 * edi = ctx->slow; jmp to the entry.
 */
static void emit_prologue(emitter *e)
{
#if X64
    static const u8 code[] = { 0x55, 0x48, 0x89, 0xE5, 0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x49, 0x89, 0xFC, 0x48, 0x8B, 0x1F,
                               0x4C, 0x8B, 0x6F, 0x08, 0xFF, 0xE6 };
#else
    static const u8 code[] = { 0x55, 0x89, 0xE5, 0x53, 0x56, 0x57, 0x83, 0xEC, 0x1C, 0x8B, 0x75, 0x08, 0x8B, 0x45, 0x0C, 0x8B, 0x1E,
                               0x8B, 0x7E, 0x04, 0xFF, 0xE0 };
#endif
    u32 i;
    for (i = 0; i < sizeof(code); i++) b1(e, code[i]);
}

/* The callee-saved registers back as they were, and back to the caller with eax. */
static void restore_and_return(emitter *e)
{
#if X64
    static const u8 code[] = { 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5B, 0x5D, 0xC3 };
#else
    static const u8 code[] = { 0x83, 0xC4, 0x1C, 0x5F, 0x5E, 0x5B, 0x5D, 0xC3 };
#endif
    u32 i;
    for (i = 0; i < sizeof(code); i++) b1(e, code[i]);
}

/* Out at `pc`: eax = pc, to the way out (FIX_EPILOGUE); with the exception pending, to its way out (FIX_THROW_EPILOGUE). */
static void emit_exit(emitter *e, u32 pc)
{
    mov_i32(e, RAX, pc);
    fix(e, FIX_EPILOGUE, 0, C_AL);
}

static void emit_throw_exit(emitter *e, u32 pc)
{
    mov_i32(e, RAX, pc);
    fix(e, FIX_THROW_EPILOGUE, 0, C_AL);
}

/* A jump back to `pc`: on to its code, unless the instructions are watched (g_dvm.slow): then out at `pc`. */
static void emit_back_check(emitter *e, u32 pc)
{
    cmp_mi(e, 4, XSLOW, 0, 0);
    fix(e, FIX_EXIT, pc, C_NE);
    fix(e, FIX_CODE, pc, C_AL);
}

/* The ways out, each's first unit: ctx->pc = eax, return 0; the same, return 1 (an exception pending); return 2 (a return, its value in ctx->result). */
static void emit_epilogues(emitter *e, u32 *epilogue, u32 *thrown, u32 *returned)
{
    *epilogue = e->n;
    op_rm(e, 0, false, 0x89, RAX, XCTX, JIT_CTX_PC);
    mov_i32(e, RAX, 0);
    restore_and_return(e);
    *thrown = e->n;
    op_rm(e, 0, false, 0x89, RAX, XCTX, JIT_CTX_PC);
    mov_i32(e, RAX, 1);
    restore_and_return(e);
    *returned = e->n;
    mov_i32(e, RAX, 2);
    restore_and_return(e);
}
