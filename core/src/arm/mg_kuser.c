/* The kernel's user helpers, done where Windows has no such page: see mg_kuser.h. */
#include "mg_kuser.h"

#include "../host/mg_host.h"

#define KUSER_CMPXCHG64 0xffff0f60u
#define KUSER_BARRIER 0xffff0fa0u
#define KUSER_CMPXCHG 0xffff0fc0u
#define KUSER_GET_TLS 0xffff0fe0u
#define KUSER_VERSION 0xffff0ffcu
#define CPSR_C 0x20000000u

static volatile i32 g_fence;

static void set_carry(u32 *cpsr, bool on)
{
    if (cpsr) *cpsr = on ? (*cpsr | CPSR_C) : (*cpsr & ~CPSR_C);
}

bool mg_kuser_call(mg_addr pc, u32 *regs, u32 *cpsr, u32 tls, mg_addr *resume)
{
    switch ((u32)pc) {
    case KUSER_GET_TLS:
        regs[0] = tls;
        break;
    case KUSER_BARRIER:
        mg_atomic_add(&g_fence, 0);    /* an interlocked operation: a full barrier */
        break;
    case KUSER_CMPXCHG: {
        /* int __kuser_cmpxchg(int oldval, int newval, volatile int *ptr): 0 and C set when *ptr was oldval and is now newval. */
        bool swapped = (u32)mg_atomic_cas((volatile i32 *)(mg_addr)regs[2], (i32)regs[0], (i32)regs[1]) == regs[0];
        regs[0] = swapped ? 0u : 1u;
        set_carry(cpsr, swapped);
        break;
    }
    case KUSER_CMPXCHG64: {
        /* int __kuser_cmpxchg64(const int64_t *oldval, const int64_t *newval, volatile int64_t *ptr), as the other. */
        i64 expect = *(const i64 *)(mg_addr)regs[0];
        bool swapped = mg_atomic_cas64((volatile i64 *)(mg_addr)regs[2], expect, *(const i64 *)(mg_addr)regs[1]) == expect;
        regs[0] = swapped ? 0u : 1u;
        set_carry(cpsr, swapped);
        break;
    }
    default:
        return false;
    }
    *resume = regs[14];
    return true;
}

static u16 half(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

bool mg_kuser_read(mg_addr address, const u8 *insn, bool thumb, u32 *regs, mg_addr *next)
{
    u32 rt, rn, base, at, len;
    bool wb = false;
    if ((u32)address != KUSER_VERSION) return false;
    if (thumb) {
        u16 h = half(insn);
        if ((h & 0xf800) == 0x6800) {                          /* LDR Rt, [Rn, #imm5 * 4] */
            rt = h & 7;
            rn = (h >> 3) & 7;
            base = regs[rn];
            at = base + ((h >> 6) & 31) * 4;
            len = 2;
        } else if ((h & 0xfe00) == 0x5800) {                   /* LDR Rt, [Rn, Rm] */
            rt = h & 7;
            rn = (h >> 3) & 7;
            base = regs[rn];
            at = base + regs[(h >> 6) & 7];
            len = 2;
        } else if ((h & 0xfff0) == 0xf8d0) {                   /* LDR.W Rt, [Rn, #imm12] */
            u16 h2 = half(insn + 2);
            rn = h & 15;
            rt = h2 >> 12;
            base = regs[rn];
            at = base + (h2 & 0xfff);
            len = 4;
        } else if ((h & 0xfff0) == 0xf850 && (half(insn + 2) & 0x0800)) {   /* LDR Rt, [Rn, #+/-imm8] with P, U and W */
            u16 h2 = half(insn + 2);
            u32 imm = h2 & 0xff, off;
            bool p = (h2 >> 10) & 1, u = (h2 >> 9) & 1, w = (h2 >> 8) & 1;
            rn = h & 15;
            rt = h2 >> 12;
            base = regs[rn];
            off = u ? base + imm : base - imm;
            at = p ? off : base;
            wb = w || !p;
            if (wb) base = off;
            len = 4;
        } else {
            return false;
        }
    } else {
        u32 w32 = (u32)insn[0] | ((u32)insn[1] << 8) | ((u32)insn[2] << 16) | ((u32)insn[3] << 24);
        u32 imm = w32 & 0xfff, off;
        bool p, u, w;
        if ((w32 & 0x0e500000u) != 0x04100000u) return false;   /* LDR (immediate), not byte, not a register offset */
        p = (w32 >> 24) & 1;
        u = (w32 >> 23) & 1;
        w = (w32 >> 21) & 1;
        rn = (w32 >> 16) & 15;
        rt = (w32 >> 12) & 15;
        base = regs[rn];
        off = u ? base + imm : base - imm;
        at = p ? off : base;
        wb = w || !p;
        if (wb) base = off;
        len = 4;
    }
    if (rt == 15 || rn == 15 || at != KUSER_VERSION) return false;
    if (wb) regs[rn] = base;
    regs[rt] = MG_KUSER_VERSION;
    *next = ((mg_addr)insn + len) | (thumb ? 1u : 0u);
    return true;
}
