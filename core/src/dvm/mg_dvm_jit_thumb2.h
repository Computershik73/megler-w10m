/*
 * The JIT's Thumb-2 back end (docs/JIT.md, "the back ends"): what mg_dvm_jit.c's front end asks for, in 32-bit ARM's
 * Thumb-2 under the AAPCS's hard-float convention -- the phone, Windows RT's tablets, and the ARM tests under qemu-arm.
 * Included by mg_dvm_jit.c alone, after its emitter and before its compiler.
 *
 * Registers: r4 the frame's registers (ctx->regs), r5 the context, r7 the slow flag's address, all three kept by the calls
 * the code makes; r0-r3 and r12 for values, s0-s3 (d0, d1) for floating point, nothing kept in them from one instruction
 * to the next. A register of the frame is a word: an int, a float's bits, a reference, half of a long or a double.
 */

#define JIT_ISA "Thumb-2"
/* Bit 0 of a pointer to the code says Thumb. */
#define JIT_CODE_BIT 1

typedef char jit_register_word[sizeof(mg_dvm_reg) == 4 ? 1 : -1];
/* Where the code finds what mg_dvm_jit_ctx holds. */
enum { JIT_CTX_REGS = 0, JIT_CTX_SLOW = 4, JIT_CTX_PC = 8, JIT_CTX_T = 12, JIT_CTX_F = 16, JIT_CTX_RESULT = 24 };
typedef char jit_ctx_layout[offsetof(mg_dvm_jit_ctx, regs) == JIT_CTX_REGS && offsetof(mg_dvm_jit_ctx, slow) == JIT_CTX_SLOW &&
                                    offsetof(mg_dvm_jit_ctx, pc) == JIT_CTX_PC && offsetof(mg_dvm_jit_ctx, t) == JIT_CTX_T &&
                                    offsetof(mg_dvm_jit_ctx, f) == JIT_CTX_F && offsetof(mg_dvm_jit_ctx, result) == JIT_CTX_RESULT
                                ? 1
                                : -1];

/* Whether the processor has SDIV (mg_cpu_arm_divide): not a Surface RT's Cortex-A9, where Windows traps each one and
   emulates it (some 80 cycles more than the runtime's division by a call) and qemu-arm faults on it. */
static bool g_jit_sdiv = true;

/* At mg_dvm_jit_start: the processor looked at, and what the log's line says of it. */
static const char *jit_processor(void)
{
    g_jit_sdiv = mg_cpu_arm_divide();
    return g_jit_sdiv ? "" : "; the processor has no SDIV (a Cortex-A9): an int's division is a call";
}

/* ------------------------------------------------------------------ branches */

/* Every branch the front end places is a 32-bit B<cond>.W or B.W. */
static u32 branch_units(u8 cond)
{
    (void)cond;
    return 2;
}

/* A branch at halfword `at` to halfword `to`: B<cond>.W (T3, +-1 MB) or B.W (T4). False when it does not reach. */
static bool put_branch(jit_unit *b, u32 at, u32 to, u8 cond)
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

/* ------------------------------------------------------------------ the instructions */

static void e16(emitter *e, u32 h) { put_unit(e, h); }
static void e32(emitter *e, u32 hi, u32 lo) { e16(e, hi); e16(e, lo); }

/* Registers: r0 to r3 and r12 for values, r4 the frame's registers, r5 the context, r7 the slow flag. */
enum { R0, R1, R2, R3, R4, R5, R6, R7, R12 = 12 };

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

/*
 * An int's division and remainder where the processor has no SDIV (g_jit_sdiv): calls of these, with Java's answers for
 * MIN_VALUE / -1 (the divisor is never 0 there: checked before, or a literal that is not).
 */
static i32 jit_div_int(i32 a, i32 b) { return a == INT32_MIN && b == -1 ? a : a / b; }
static i32 jit_rem_int(i32 a, i32 b) { return a == INT32_MIN && b == -1 ? 0 : a % b; }
static void call_c(emitter *e, mg_addr fn);

/* mov rd, rm (MOV register, T1: any two of r0-r15). */
static void mov_reg(emitter *e, int rd, int rm) { e16(e, 0x4600 | (((u32)rd & 8) << 4) | ((u32)rm << 3) | ((u32)rd & 7)); }

static void data_op(emitter *e, int op, int rd, int rn, int rm)
{
    if ((op == OP_DIV || op == OP_REM) && !g_jit_sdiv) {
        /* rn into r0 and rm into r1 (rm kept in r12 first where it is r0), the call, its result into rd. */
        if (rm == R0) {
            mov_reg(e, R12, R0);
            rm = R12;
        }
        if (rn != R0) mov_reg(e, R0, rn);
        if (rm != R1) mov_reg(e, R1, rm);
        call_c(e, op == OP_DIV ? (mg_addr)jit_div_int : (mg_addr)jit_rem_int);
        if (rd != R0) mov_reg(e, rd, R0);
        return;
    }
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

/* binop and binop/2addr: vD = vA op vB. */
static void emit_arith(emitter *e, u32 pc, int op, u32 vd, u32 va, u32 vb)
{
    ldr_v(e, R1, va);
    ldr_v(e, R2, vb);
    arith(e, pc, op, vd, R1, R2);
}

/* binop/lit16 and binop/lit8: vD = vA op lit (a divisor of zero never comes here: the front end hands it back). */
static void emit_arith_lit(emitter *e, int op, u32 vd, u32 va, i32 lit)
{
    ldr_v(e, R1, va);
    mov_imm(e, R2, (u32)lit);
    data_op(e, op, R0, R1, R2);
    str_v(e, R0, vd);
}

/* ------------------------------------------------------------------ moves and constants */

static void emit_move(emitter *e, u32 vd, u32 vs)
{
    ldr_v(e, R0, vs);
    str_v(e, R0, vd);
}

static void emit_move_wide(emitter *e, u32 vd, u32 vs)
{
    ldr_v(e, R0, vs);
    ldr_v(e, R1, vs + 1);
    str_v(e, R0, vd);
    str_v(e, R1, vd + 1);
}

/* move-result, -wide, -object: a compiled call's result (ctx->result). An int and a reference are both a word here. */
static void emit_move_result(emitter *e, int type, u32 vd)
{
    ldr_imm(e, R0, R5, JIT_CTX_RESULT);
    if (type == T_WIDE) {
        ldr_imm(e, R1, R5, JIT_CTX_RESULT + 4);
        str_v(e, R0, vd);
        str_v(e, R1, vd + 1);
    } else {
        str_v(e, R0, vd);
    }
}

/* The value of a return for ctx->result, whole (`type` -1 for return-void); the front end then branches to the way out. */
static void emit_return(emitter *e, int type, u32 va)
{
    if (type < 0) {
        mov_imm(e, R0, 0);
        mov_imm(e, R1, 0);
    } else if (type == T_WIDE) {
        ldr_v(e, R0, va);
        ldr_v(e, R1, va + 1);
    } else {
        ldr_v(e, R0, va);
        mov_imm(e, R1, 0);
    }
    str_imm(e, R0, R5, JIT_CTX_RESULT);
    str_imm(e, R1, R5, JIT_CTX_RESULT + 4);
}

static void emit_const(emitter *e, u32 vd, u32 v)
{
    mov_imm(e, R0, v);
    str_v(e, R0, vd);
}

static void emit_const_wide(emitter *e, u32 vd, u64 v)
{
    mov_imm(e, R0, (u32)v);
    mov_imm(e, R1, (u32)(v >> 32));
    str_v(e, R0, vd);
    str_v(e, R1, vd + 1);
}

/* A reference the code holds as a constant: a string already made (interned, never freed). */
static void emit_const_object(emitter *e, u32 vd, const void *p)
{
    mov_imm(e, R0, (u32)(mg_addr)p);
    str_v(e, R0, vd);
}

/* ------------------------------------------------------------------ branches on values */

/* if-test vA, vB: the flags set; the condition the branch is taken on. */
static u8 emit_compare(emitter *e, u32 op, u32 va, u32 vb)
{
    ldr_v(e, R0, va);
    ldr_v(e, R1, vb);
    cmp_reg(e, R0, R1);
    return if_cond(op - 0x32);
}

/* if-testz vAA. */
static u8 emit_compare_zero(emitter *e, u32 op, u32 va)
{
    ldr_v(e, R0, va);
    cmp_imm(e, R0, 0);
    return if_cond(op - 0x38);
}

/* A switch's value, in r0 (a packed switch's less its first key), then each key compared with it: equal is its case. */
static void emit_switch_value(emitter *e, u32 va, bool packed, i32 first)
{
    ldr_v(e, R0, va);
    if (packed) {
        mov_imm(e, R1, (u32)first);
        data_op(e, OP_SUB, R0, R0, R1);
    }
}

static void emit_switch_key(emitter *e, i32 key)
{
    if (key >= 0 && key < 256) {
        cmp_imm(e, R0, (u32)key);
    } else {
        mov_imm(e, R1, (u32)key);
        cmp_reg(e, R0, R1);
    }
}

/* ------------------------------------------------------------------ arrays and fields */

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

/* A word, byte or halfword's kind as T_* numbers them, a reference being a word here. */
static int word_kind(int type) { return type == T_OBJECT ? T_INT : type; }

/* An element at r3 + r1 << shift: loaded into r2 (kind: 0 word, 1 byte, 2 signed byte, 3 halfword, 4 signed halfword) or stored from it. */
static void element(emitter *e, bool store, int kind)
{
    static const u16 loads[] = { 0xF850, 0xF810, 0xF910, 0xF830, 0xF930 }, stores[] = { 0xF840, 0xF800, 0xF800, 0xF820, 0xF820 };
    static const u8 shifts[] = { 2, 0, 0, 1, 1 };
    e32(e, (u32)(store ? stores[kind] : loads[kind]) | R3, (R2 << 12) | ((u32)shifts[kind] << 4) | R1);
}

/* aget and aput of every kind but aput-object (the runtime's: the store check): vR = vA[vI], or vA[vI] = vR. */
static void emit_array(emitter *e, u32 pc, bool store, int type, u32 vr, u32 va, u32 vi)
{
    static const u8 sizes[] = { 4, 1, 1, 2, 2, 8, 0 };
    array_checks(e, pc, va, vi, sizes[type]);
    if (type == T_WIDE) {
        /* the element at r3 + r1 * 8 */
        e32(e, 0xEB03, (R3 << 8) | (3u << 6) | R1);              /* add r3, r3, r1, lsl #3 */
        if (store) {
            ldr_v(e, R0, vr);
            ldr_v(e, R1, vr + 1);
            str_imm(e, R0, R3, 0);
            str_imm(e, R1, R3, 4);
        } else {
            ldr_imm(e, R0, R3, 0);
            ldr_imm(e, R1, R3, 4);
            str_v(e, R0, vr);
            str_v(e, R1, vr + 1);
        }
    } else if (store) {
        ldr_v(e, R2, vr);
        element(e, true, word_kind(type));
    } else {
        element(e, false, word_kind(type));
        str_v(e, R2, vr);
    }
}

static void emit_array_length(emitter *e, u32 pc, u32 vd, u32 va)
{
    ldr_v(e, R0, va);
    cmp_imm(e, R0, 0);
    fix(e, FIX_EXIT, pc, C_EQ);
    ldr_imm(e, R1, R0, (u32)offsetof(mg_jobj, u.arr.len));
    str_v(e, R1, vd);
}

/* A field's value at [rn + off]: loaded into rt (kind: 0 word, 1 byte, 2 signed byte, 3 halfword, 4 signed halfword) or stored from it. */
static void field_access(emitter *e, bool store, int kind, int rt, int rn, u32 off)
{
    static const u16 loads[] = { 0xF8D0, 0xF890, 0xF990, 0xF8B0, 0xF9B0 }, stores[] = { 0xF8C0, 0xF880, 0xF880, 0xF8A0, 0xF8A0 };
    e32(e, (u32)(store ? stores[kind] : loads[kind]) | (u32)rn, ((u32)rt << 12) | off);
}

/* The field at [r0 + off] read into vR, or written from it. */
static void field_value(emitter *e, bool store, int type, u32 vr, u32 off)
{
    if (type == T_WIDE) {
        if (store) {
            ldr_v(e, R2, vr);
            ldr_v(e, R3, vr + 1);
            field_access(e, true, 0, R2, R0, off);
            field_access(e, true, 0, R3, R0, off + 4);
        } else {
            field_access(e, false, 0, R2, R0, off);
            field_access(e, false, 0, R3, R0, off + 4);
            str_v(e, R2, vr);
            str_v(e, R3, vr + 1);
        }
    } else if (store) {
        ldr_v(e, R2, vr);
        field_access(e, true, word_kind(type), R2, R0, off);
    } else {
        field_access(e, false, word_kind(type), R2, R0, off);
        str_v(e, R2, vr);
    }
}

/*
 * iget and iput of a field resolved by now, at `off` in the object: the object checked for null and for having the slot
 * (an object the runtime keeps its fields for by name does not: the interpreter's slotless_field), each failure an exit.
 */
static void emit_instance_field(emitter *e, u32 pc, bool store, int type, u32 vr, u32 vo, u32 slot, u32 off)
{
    ldr_v(e, R0, vo);
    cmp_imm(e, R0, 0);
    fix(e, FIX_EXIT, pc, C_EQ);
    ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
    ldr_imm(e, R1, R1, (u32)offsetof(mg_dvm_class, nslots));
    mov_imm(e, R2, slot);
    cmp_reg(e, R2, R1);
    fix(e, FIX_EXIT, pc, C_HS);
    field_value(e, store, type, vr, off);
}

/* sget and sput of a class initialised by now: its slot's address is the code's own constant. */
static void emit_static_field(emitter *e, bool store, int type, u32 vr, mg_jvalue *slot)
{
    mov_imm(e, R0, (u32)(mg_addr)slot);
    field_value(e, store, type, vr, 0);
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
 * shl, shr, ushr; the shifts by the int vB). False for division and remainder: they are emit_long_divide's.
 */
static bool emit_long(emitter *e, u32 op, u32 vd, u32 va, u32 vb)
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
static void emit_cmp_long(emitter *e, u32 vd, u32 va, u32 vb)
{
    u32 lt, gt, lo, hi, zero_done, minus_done;
    ldr_v(e, R0, va);
    ldr_v(e, R1, va + 1);
    ldr_v(e, R2, vb);
    ldr_v(e, R3, vb + 1);
    cmp_reg(e, R1, R3);
    lt = local_branch(e, C_LT);
    gt = local_branch(e, C_GT);
    cmp_reg(e, R0, R2);
    lo = local_branch(e, C_LO);
    hi = local_branch(e, C_HI);
    mov_imm(e, R0, 0);
    zero_done = local_branch(e, C_AL);
    local_to(e, lt, C_LT);
    local_to(e, lo, C_LO);
    mov_imm(e, R0, 0xFFFFFFFFu);
    minus_done = local_branch(e, C_AL);
    local_to(e, gt, C_GT);
    local_to(e, hi, C_HI);
    mov_imm(e, R0, 1);
    local_to(e, zero_done, C_AL);
    local_to(e, minus_done, C_AL);
    str_v(e, R0, vd);
}

/* neg-int, not-int, neg-long, not-long, int-to-long, long-to-int, int-to-byte, -char, -short: vA = op vB. */
static void emit_unary(emitter *e, u32 op, u32 va, u32 vb)
{
    switch (op) {
    case 0x7b: ldr_v(e, R1, vb); e32(e, 0xF1C0 | R1, (R0 << 8)); str_v(e, R0, va); break;          /* rsb r0, r1, #0 */
    case 0x7c: ldr_v(e, R1, vb); e32(e, 0xEA6F, (R0 << 8) | R1); str_v(e, R0, va); break;          /* mvn r0, r1 */
    case 0x7d:                                                   /* neg-long: 0 - (hi:lo) */
        ldr_v(e, R0, vb);
        ldr_v(e, R1, vb + 1);
        e32(e, 0xF1D0 | R0, (R0 << 8));                          /* rsbs r0, r0, #0 */
        mov_imm(e, R2, 0);
        e32(e, 0xEB60 | R2, (R1 << 8) | R1);                     /* sbc r1, r2, r1 */
        str_v(e, R0, va);
        str_v(e, R1, va + 1);
        break;
    case 0x7e:                                                   /* not-long */
        ldr_v(e, R0, vb);
        ldr_v(e, R1, vb + 1);
        e32(e, 0xEA6F, (R0 << 8) | R0);
        e32(e, 0xEA6F, (R1 << 8) | R1);
        str_v(e, R0, va);
        str_v(e, R1, va + 1);
        break;
    case 0x81:                                                   /* int-to-long: the sign into the high word */
        ldr_v(e, R0, vb);
        e32(e, 0xEA4F, (7u << 12) | (R1 << 8) | (3u << 6) | (2u << 4) | R0);     /* asr r1, r0, #31 */
        str_v(e, R0, va);
        str_v(e, R1, va + 1);
        break;
    case 0x84: ldr_v(e, R0, vb); str_v(e, R0, va); break;       /* long-to-int: the low word */
    case 0x8d: ldr_v(e, R1, vb); e32(e, 0xFA4F, 0xF080 | (R0 << 8) | R1); str_v(e, R0, va); break;  /* sxtb */
    case 0x8e: ldr_v(e, R1, vb); e32(e, 0xFA1F, 0xF080 | (R0 << 8) | R1); str_v(e, R0, va); break;  /* uxth */
    default: ldr_v(e, R1, vb); e32(e, 0xFA0F, 0xF080 | (R0 << 8) | R1); str_v(e, R0, va); break;    /* sxth */
    }
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
static void emit_fp_arith(emitter *e, u32 op, bool dbl, u32 vd, u32 va, u32 vb)
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
static void emit_fp_compare(emitter *e, bool dbl, bool greater, u32 vd, u32 va, u32 vb)
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
static bool emit_fp_convert(emitter *e, u32 op, u32 va, u32 vb)
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
static void emit_long_divide(emitter *e, u32 pc, bool rem, u32 vd, u32 va, u32 vb)
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
static void emit_fp_remainder(emitter *e, bool dbl, u32 vd, u32 va, u32 vb)
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
static void emit_long_convert(emitter *e, u32 op, u32 va, u32 vb)
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
static void emit_invoke(emitter *e, u32 pc)
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
 * The quick way (the front end's fast_invoke): mg_dvm_jit_fast(ctx, m, desc, pc); 1 back is an exception pending, 3 a call
 * it did not make, which goes the general way. `known`: the method is the one called, and whether its code has a start
 * for such calls is asked in the code first, with no call when not.
 */
static void emit_fast_invoke(emitter *e, u32 pc, const mg_dvm_method *ref, u32 desc, bool known)
{
    u32 done, slow[2], nslow = 0, i;
    mov_imm(e, R1, (u32)(mg_addr)ref);
    if (known) {
        ldr_imm(e, R0, R1, (u32)offsetof(mg_dvm_method, jit));
        cmp_imm(e, R0, 0);
        slow[nslow++] = local_branch(e, C_EQ);
        ldr_imm(e, R0, R0, (u32)offsetof(mg_dvm_jit_code, fast));
        cmp_imm(e, R0, 0);
        slow[nslow++] = local_branch(e, C_EQ);
    }
    e16(e, 0x4628);                                             /* mov r0, r5 */
    mov_imm(e, R2, desc);
    mov_imm(e, R3, pc);
    mov_imm(e, R12, (u32)(mg_addr)mg_dvm_jit_fast);
    e16(e, 0x47E0);                                             /* blx r12 */
    cmp_imm(e, R0, 1);
    fix(e, FIX_THROW, pc, C_EQ);
    cmp_imm(e, R0, 3);
    done = local_branch(e, C_NE);
    for (i = 0; i < nslow; i++) local_to(e, slow[i], C_EQ);
    emit_invoke(e, pc);
    local_to(e, done, C_NE);
}

/*
 * The intrinsics that are a few instructions, as inline_call allows them (its receiver in r0, checked for null, when the
 * method is not static). A null string or an index out of its bounds goes by the call (mg_dvm_jit_invoke), which throws
 * as the interpreter would; the others cannot fail.
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
        fails[(*nfails)++] = local_branch(e, C_HS);
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

/* A call of a trivial method or an intrinsic done in the code, as the front end's inline_call found it may be. */
static void emit_inline_call(emitter *e, u32 pc, const inline_plan *p)
{
    u32 fails[6], i, nfails = 0, done;
    u8 conds[6];
    if (p->hooks) {
        /* The method hooked since (mg_dvm_hook.c): by mg_dvm_jit_invoke, which calls the hook. */
        mov_imm(e, R1, (u32)(mg_addr)p->target);
        ldrb_imm(e, R1, R1, (u32)offsetof(mg_dvm_method, hooked));
        cmp_imm(e, R1, 0);
        conds[nfails] = C_NE;
        fails[nfails++] = local_branch(e, C_NE);
    }
    if (p->kind != DVM_INVOKE_STATIC) {
        ldr_v(e, R0, p->self);
        cmp_imm(e, R0, 0);
        conds[nfails] = C_EQ;
        fails[nfails++] = local_branch(e, C_EQ);
        if (p->vtab) {
            ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
            ldr_imm(e, R2, R1, (u32)offsetof(mg_dvm_class, nvtab));
            mov_imm(e, R3, (u32)p->target->vidx);
            cmp_reg(e, R3, R2);
            conds[nfails] = C_HS;
            fails[nfails++] = local_branch(e, C_HS);
            ldr_imm(e, R1, R1, (u32)offsetof(mg_dvm_class, vtab));
            e32(e, 0xF850 | R1, (R1 << 12) | (2u << 4) | R3);    /* ldr r1, [r1, r3, lsl #2] */
            mov_imm(e, R2, (u32)(mg_addr)p->target);
            cmp_reg(e, R1, R2);
            conds[nfails] = C_NE;
            fails[nfails++] = local_branch(e, C_NE);
        }
        if (p->slots) {
            ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
            ldr_imm(e, R1, R1, (u32)offsetof(mg_dvm_class, nslots));
            mov_imm(e, R2, p->fld->slot);
            cmp_reg(e, R2, R1);
            conds[nfails] = C_HS;
            fails[nfails++] = local_branch(e, C_HS);
        }
    }
    if (p->intrinsic) {
        /* (The front end checked that it is one of those: this emits it.) */
        intrinsic_in_code(e, p->intrinsic, p->count, p->self, p->value, fails, conds, &nfails);
    } else switch (SHAPE_OF(p->shape)) {
    case SHAPE_CONST:
        mov_imm(e, R0, (u32)SHAPE_VALUE(p->shape));
        str_imm(e, R0, R5, JIT_CTX_RESULT);
        break;
    case SHAPE_STATIC_GETTER:
        mov_imm(e, R0, (u32)(mg_addr)&p->fld->cls->statics[p->fld->slot]);
        /* fall through - read as a getter's field, at offset 0 */
    case SHAPE_GETTER:
        if (p->type == T_WIDE) {
            field_access(e, false, 0, R2, R0, p->off);
            field_access(e, false, 0, R3, R0, p->off + 4);
            str_imm(e, R2, R5, JIT_CTX_RESULT);
            str_imm(e, R3, R5, JIT_CTX_RESULT + 4);
        } else {
            field_access(e, false, word_kind(p->type), R2, R0, p->off);
            str_imm(e, R2, R5, JIT_CTX_RESULT);
        }
        break;
    case SHAPE_SETTER:
        ldr_v(e, R2, p->value);
        field_access(e, true, word_kind(p->type), R2, R0, p->off);
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

/* check-cast and instance-of of class `c`, resolved: the code's own answer for null and for the very class, the runtime's for the rest. */
static void emit_type_test(emitter *e, u32 pc, bool instance_of, u32 vo, u32 vd, const mg_dvm_class *c)
{
    u32 null_done, same_done, slow_done;
    ldr_v(e, R0, vo);
    cmp_imm(e, R0, 0);
    null_done = local_branch(e, C_EQ);
    ldr_imm(e, R1, R0, (u32)offsetof(mg_dvm_obj, cls));
    mov_imm(e, R2, (u32)(mg_addr)c);
    cmp_reg(e, R1, R2);
    same_done = local_branch(e, C_EQ);
    emit_runtime(e, pc);
    slow_done = local_branch(e, C_AL);
    if (instance_of) {
        /* 0 for null, 1 for the very class */
        u32 one_done;
        local_to(e, null_done, C_EQ);
        mov_imm(e, R0, 0);
        one_done = local_branch(e, C_AL);
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

/* ------------------------------------------------------------------ the way in and out */

/* push {r3-r7, lr} (six words: the stack stays 8-byte aligned for the calls); r5 = ctx, r4 = ctx->regs, r7 = ctx->slow; bx r1 (the entry). */
static void emit_prologue(emitter *e)
{
    e16(e, 0xB5F8);
    e16(e, 0x4605);
    e16(e, 0x682C);
    e16(e, 0x686F);
    e16(e, 0x4708);
}

/* Out at `pc`: r0 = pc, to the way out (FIX_EPILOGUE); with the exception pending, to its way out (FIX_THROW_EPILOGUE). */
static void emit_exit(emitter *e, u32 pc)
{
    mov_imm(e, R0, pc);
    fix(e, FIX_EPILOGUE, 0, C_AL);
}

static void emit_throw_exit(emitter *e, u32 pc)
{
    mov_imm(e, R0, pc);
    fix(e, FIX_THROW_EPILOGUE, 0, C_AL);
}

/* A jump back to `pc`: on to its code, unless the instructions are watched (g_dvm.slow): then out at `pc`. */
static void emit_back_check(emitter *e, u32 pc)
{
    e16(e, 0x6838);                                              /* ldr r0, [r7] */
    cmp_imm(e, R0, 0);
    fix(e, FIX_EXIT, pc, C_NE);
    fix(e, FIX_CODE, pc, C_AL);
}

/* The ways out, each's first unit: ctx->pc = r0, return 0; the same, return 1 (an exception pending); return 2 (a return, its value in ctx->result). */
static void emit_epilogues(emitter *e, u32 *epilogue, u32 *thrown, u32 *returned)
{
    *epilogue = e->n;
    e16(e, 0x60A8);                                              /* str r0, [r5, #8] */
    e16(e, 0x2000);                                              /* movs r0, #0 */
    e16(e, 0xBDF8);                                              /* pop {r3-r7, pc} */
    *thrown = e->n;
    e16(e, 0x60A8);                                              /* str r0, [r5, #8] */
    e16(e, 0x2001);                                              /* movs r0, #1 */
    e16(e, 0xBDF8);                                              /* pop {r3-r7, pc} */
    *returned = e->n;
    e16(e, 0x2002);                                              /* movs r0, #2 */
    e16(e, 0xBDF8);                                              /* pop {r3-r7, pc} */
}
