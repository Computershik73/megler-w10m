/* The shape of an x86 or x86_64 instruction (mg_x86_decode.h). Compiled everywhere; used by the x86 processors' loader. */
#include "mg_x86_decode.h"

/*
 * The one-byte map: whether an opcode has a ModRM byte (M), and its immediate: 1 a byte (B), 2 a word (W, 2 bytes),
 * Z a word or a doubleword by the operand size (2 or 4), V the same and eight with REX.W (B8-BF), X an opcode that is
 * no instruction in 64-bit mode, P a prefix (handled before the table), E an escape (0F).
 */
enum { M = 0x10, B = 1, W = 2, Z = 3, V = 4, X = 0x20, P = 0x40, E = 0x80 };

static const u8 one_byte[256] = {
    /* 00 */ M, M, M, M, B, Z, X, X, M, M, M, M, B, Z, X, E,
    /* 10 */ M, M, M, M, B, Z, X, X, M, M, M, M, B, Z, X, X,
    /* 20 */ M, M, M, M, B, Z, P, X, M, M, M, M, B, Z, P, X,
    /* 30 */ M, M, M, M, B, Z, P, X, M, M, M, M, B, Z, P, X,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ X, X, M | X, M, P, P, P, P, Z, M | Z, B, M | B, 0, 0, 0, 0,
    /* 70 */ B, B, B, B, B, B, B, B, B, B, B, B, B, B, B, B,
    /* 80 */ M | B, M | Z, M | B | X, M | B, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, X, 0, 0, 0, 0, 0,
    /* A0 */ 0, 0, 0, 0, 0, 0, 0, 0, B, Z, 0, 0, 0, 0, 0, 0,
    /* B0 */ B, B, B, B, B, B, B, B, V, V, V, V, V, V, V, V,
    /* C0 */ M | B, M | B, W, 0, M, M, M | B, M | Z, 0, 0, W, 0, 0, B, X, 0,
    /* D0 */ M, M, M, M, B | X, B | X, X, 0, M, M, M, M, M, M, M, M,
    /* E0 */ B, B, B, B, B, B, B, B, Z, Z, X, B, 0, 0, 0, 0,
    /* F0 */ P, 0, P, P, 0, 0, M, M, 0, 0, 0, 0, 0, 0, M, M,
};

/* The 0F map: which opcodes have no ModRM, and which an immediate byte; ff for none of these (an undefined one). */
static bool two_byte_modrm(u8 op)
{
    switch (op) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x37:
    case 0x77: case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
        return false;
    default:
        if (op >= 0x80 && op <= 0x8F) return false;   /* jcc rel32 */
        if (op >= 0xC8 && op <= 0xCF) return false;   /* bswap */
        return true;
    }
}

static u8 two_byte_imm(u8 op, bool opsize)
{
    if (op >= 0x80 && op <= 0x8F) return opsize ? 2 : 4;
    switch (op) {
    case 0x70: case 0x71: case 0x72: case 0x73: case 0xA4: case 0xAC: case 0xBA: case 0xC2: case 0xC4: case 0xC5: case 0xC6:
    case 0x0F:                                         /* 3DNow!: its opcode is a byte after the operand */
        return 1;
    default:
        return 0;
    }
}

/* What follows the opcode: ModRM, SIB and the displacement, when the instruction has ModRM. */
static bool modrm_tail(const u8 *p, size_t avail, size_t *at, bool is64, mg_x86_insn *in)
{
    u8 modrm, mod, rm;
    if (*at >= avail) return false;
    in->has_modrm = true;
    in->modrm_at = (u8)*at;
    modrm = in->modrm = p[(*at)++];
    mod = modrm >> 6;
    rm = modrm & 7;
    if (mod == 3) return true;
    if (!is64 && in->adsize) {
        /* 16-bit addressing: no SIB; [disp16] alone, or a displacement of a byte or two. */
        if (mod == 0 && rm == 6) in->disp_size = 2;
        else if (mod == 1) in->disp_size = 1;
        else if (mod == 2) in->disp_size = 2;
    } else {
        if (rm == 4) {
            if (*at >= avail) return false;
            in->has_sib = true;
            in->sib = p[(*at)++];
            if (mod == 0 && (in->sib & 7) == 5) in->disp_size = 4;
        }
        if (mod == 0 && rm == 5) in->disp_size = 4;    /* [disp32], or rip-relative on x86_64 */
        else if (mod == 1) in->disp_size = 1;
        else if (mod == 2) in->disp_size = 4;
    }
    in->disp_at = (u8)*at;
    *at += in->disp_size;
    return *at <= avail;
}

bool mg_x86_decode(const u8 *p, size_t avail, bool is64, mg_x86_insn *in)
{
    size_t at = 0;
    u8 op, flags, imm = 0;
    bool rexw = false;
    memset(in, 0, sizeof(*in));
    in->is64 = is64;
    if (avail > 15) avail = 15;
    /* Prefixes, in any order, and REX last (x86_64). */
    for (;;) {
        u8 b;
        if (at >= avail) return false;
        b = p[at];
        if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
            in->seg = b;
            in->seg_at = (u8)at;
        } else if (b == 0x66) {
            in->opsize = true;
        } else if (b == 0x67) {
            in->adsize = true;
        } else if (b != 0xF0 && b != 0xF2 && b != 0xF3) {
            break;
        }
        at++;
    }
    if (is64 && (p[at] & 0xF0) == 0x40) {
        in->rex = p[at++];
        rexw = (in->rex & 8) != 0;
        if (at >= avail) return false;
    }
    op = p[at];
    /* VEX and EVEX: in x86_64 always; in 32-bit mode only where the next byte's top bits are set (else LES, LDS, BOUND). */
    if ((op == 0xC4 || op == 0xC5 || op == 0x62) && at + 1 < avail && (is64 || (p[at + 1] & 0xC0) == 0xC0)) {
        u8 map;
        in->vex = true;
        if (op == 0xC5) {
            map = 1;
            at += 2;
        } else if (op == 0xC4) {
            if (at + 2 >= avail) return false;
            map = p[at + 1] & 0x1F;
            at += 3;
        } else {
            if (at + 3 >= avail) return false;
            map = p[at + 1] & 0x07;
            at += 4;
        }
        if (map < 1 || map > 3 || at >= avail) return false;
        in->map = map;
        in->op_at = (u8)at;
        in->op = p[at++];
        if (!(map == 1 && in->op == 0x77)) {
            if (!modrm_tail(p, avail, &at, is64, in)) return false;
        }
        if (map == 3 || (map == 1 && two_byte_imm(in->op, false) == 1 && in->op != 0x0F)) imm = 1;
        in->imm_at = (u8)at;
        in->imm_size = imm;
        at += imm;
        if (at > avail) return false;
        in->len = (u8)at;
        return true;
    }
    in->op_at = (u8)at;
    in->op = op;
    at++;
    if (op == 0x0F) {
        if (at >= avail) return false;
        op = in->op = p[at++];
        in->op_at = (u8)(at - 1);
        if (op == 0x38 || op == 0x3A) {
            /* The three-byte maps: ModRM always; an immediate byte in 0F3A's. */
            if (at >= avail) return false;
            in->map = op == 0x38 ? 2 : 3;
            in->op = p[at++];
            in->op_at = (u8)(at - 1);
            if (!modrm_tail(p, avail, &at, is64, in)) return false;
            imm = in->map == 3 ? 1 : 0;
        } else {
            in->map = 1;
            if (op == 0x04 || op == 0x0A || op == 0x0C || op == 0x24 || op == 0x25 || op == 0x26 || op == 0x27 || op == 0x36 || op == 0x39
                || (op >= 0x3B && op <= 0x3F))
                return false;
            if (two_byte_modrm(op) && !modrm_tail(p, avail, &at, is64, in)) return false;
            imm = two_byte_imm(op, in->opsize && !is64);
        }
    } else {
        flags = one_byte[op];
        if ((flags & X) && is64) return false;
        if (flags & P) return false;                   /* (a prefix where an opcode belongs: none) */
        if ((flags & M) && !modrm_tail(p, avail, &at, is64, in)) return false;
        switch (flags & 0x0F) {
        case B: imm = 1; break;
        case W: imm = 2; break;
        case Z: imm = in->opsize ? 2 : 4; break;
        case V: imm = rexw ? 8 : in->opsize ? 2 : 4; break;
        default: break;
        }
        /* The few whose immediate the table cannot say. */
        if (op == 0xF6 && ((in->modrm >> 3) & 7) <= 1) imm = 1;                     /* test r/m8, imm8 */
        else if (op == 0xF7 && ((in->modrm >> 3) & 7) <= 1) imm = in->opsize ? 2 : 4; /* test r/m, imm */
        else if (op == 0xC8) imm = 3;                                               /* enter imm16, imm8 */
        else if (op == 0x9A || op == 0xEA) imm = in->opsize ? 4 : 6;                /* far pointer, 32-bit mode */
        else if ((op == 0xE8 || op == 0xE9) && is64) imm = 4;
        else if (op >= 0xA0 && op <= 0xA3) {
            /* mov to and from an absolute address: as wide as an address. */
            imm = is64 ? (in->adsize ? 4 : 8) : (in->adsize ? 2 : 4);
            in->moffs = true;
        }
    }
    in->imm_at = (u8)at;
    in->imm_size = imm;
    at += imm;
    if (at > avail) return false;
    in->len = (u8)at;
    return true;
}

bool mg_x86_absolute(const mg_x86_insn *in, const u8 *p, i64 *addr)
{
    if (in->moffs) {
        if (in->imm_size == 8) { u64 v; memcpy(&v, p + in->imm_at, 8); *addr = (i64)v; }
        else if (in->imm_size == 4) { u32 v; memcpy(&v, p + in->imm_at, 4); *addr = (i64)v; }
        else return false;
        return true;
    }
    if (!in->has_modrm || (in->modrm >> 6) != 0 || in->disp_size != 4) return false;
    /* [disp32] with no base and no index: through a SIB of base 5 and index 4 -- or ModRM's rm 5, but that is rip-relative on x86_64. */
    if (in->has_sib) {
        if ((in->sib & 7) != 5 || ((in->sib >> 3) & 7) != 4 || (in->rex & 2)) return false;
    } else if ((in->modrm & 7) != 5 || in->is64 || in->adsize) {
        return false;
    }
    {
        i32 d;
        memcpy(&d, p + in->disp_at, 4);
        *addr = d;
    }
    return true;
}
