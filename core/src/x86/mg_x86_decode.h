/*
 * The length and the shape of an x86 or x86_64 instruction: enough of a decoder for the loader to walk a library's code
 * an instruction at a time and find the few it has to rewrite (x86/mg_patch_x86.c) -- which bytes are prefixes, opcode,
 * ModRM, SIB, displacement and immediate. It does not say what an instruction does.
 *
 * Every encoding a compiler for Android emits is known: the legacy prefixes, REX, the one-byte map and the 0F, 0F38 and
 * 0F3A maps, VEX (C4, C5) and EVEX (62), x87, the 3DNow! suffix. What is not an instruction at all (an opcode that does
 * not exist in the mode) is said so, and the walker steps over a byte.
 */
#ifndef MG_X86_DECODE_H
#define MG_X86_DECODE_H

#include "../mg_base.h"

typedef struct mg_x86_insn {
    u8 len;                            /* bytes in all */
    u8 seg;                            /* a segment override's prefix byte (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65), or 0 */
    u8 seg_at;                         /* where it is */
    u8 rex;                            /* the REX byte (x86_64), or 0 */
    bool opsize, adsize;               /* 0x66, 0x67 */
    u8 map;                            /* 0 the one-byte map, 1 0F, 2 0F38, 3 0F3A */
    u8 op, op_at;
    bool vex;                          /* VEX or EVEX */
    bool has_modrm;
    u8 modrm, modrm_at;
    bool has_sib;
    u8 sib;
    u8 disp_at, disp_size;             /* 0, 1, 2 or 4 bytes */
    u8 imm_at, imm_size;               /* 0 to 8 bytes, every immediate of the instruction */
    bool moffs;                        /* A0-A3: the "displacement" is a whole address, imm_at/imm_size say where */
    bool is64;                         /* decoded as x86_64's */
} mg_x86_insn;

/* Decodes the instruction at `p`, with `avail` bytes readable there; `is64` for x86_64's mode. False when it is none. */
bool mg_x86_decode(const u8 *p, size_t avail, bool is64, mg_x86_insn *out);

/* An absolute address in the instruction's memory operand, [disp32] or moffs (no base, no index): its value. */
bool mg_x86_absolute(const mg_x86_insn *in, const u8 *p, i64 *addr);

#endif
