/*
 * What is rewritten in an ARM64 library's code before it is made executable (arm/mg_patch.c is 32-bit ARM's,
 * x86/mg_patch_x86.c the x86 processors').
 *
 * bionic keeps its thread pointer in TPIDR_EL0, and compiled code reads it inline, `mrs xN, tpidr_el0`: for every
 * thread-local variable of the initial-exec kind, for bionic's own __get_tls(), and for the stack guard, which Android's
 * code reads at the thread pointer + 0x28 (bionic's slot 5) in every function with an array on its stack -- so a
 * library has hundreds of them. The register is not the guest's here: on Linux it is glibc's thread pointer, and Windows
 * does not promise to keep what user code writes there. Each read is made a read of the slot of the host's own thread
 * block that holds the guest's thread pointer (mg_guest_tp_where), and the canary then comes from slot 5 of the guest's
 * own block, as on the phone:
 *
 *   - on Windows the slot is in the TEB, which x18 points to in user mode: `ldr xN, [x18, #0x1480+8*slot]`, one
 *     instruction for one, in its place. Android's compiler leaves x18 alone (it is the platform register there, kept
 *     for the shadow call stack), and code that wrote it would break Windows' own functions, patched or not;
 *   - on Linux (the tests) the slot is glibc's initial-exec variable, at an offset from TPIDR_EL0, which needs two
 *     instructions: a branch to the library's island, `mrs xN, tpidr_el0; ldr xN, [xN, #offset]`, and back.
 *
 * A write, `msr tpidr_el0, xN` (bionic's own start of a thread: no app's code has one that runs here), is made the
 * matching store.
 *
 * System calls, `svc #0` with the number in x8, are made a UDF and recorded, as on 32-bit ARM: the fault handler
 * (mg_core.c) does the call and resumes after it. A fault is safe here: the ABI keeps nothing below the stack pointer,
 * so Windows' exception frame overwrites nothing of the guest's (it would on x86_64, below rsp). A signal restorer's
 * svc (rt_sigreturn, 139 put in x8 just before it) is left alone.
 *
 * Only the library's executable sections are walked, when its section headers say which they are (mg_lib's code), a
 * word at a time: ARM64's instructions are all four bytes and aligned, so no word is ever the inside of another.
 */
#include "../mg_core.h"
#include "../bionic/mg_bionic.h"
#include "../host/mg_host.h"
#include "../ld/mg_ld_internal.h"

#if MG_ARCH_ARM64

#define MRS_TPIDR_EL0 0xD53BD040u       /* mrs xN, tpidr_el0: | N */
#define MSR_TPIDR_EL0 0xD51BD040u       /* msr tpidr_el0, xN: | N */
#define SVC_0 0xD4000001u
#define SVC_UDF 0x000000A6u             /* udf #0xa6: a patched svc, dispatched */
#define SVC_UDF_TRAP 0x000000A5u        /* udf #0xa5: the flag trap-svc's, a fault where the svc was */
#define MOV_X8_139 0xD2801168u          /* movz x8, #139: rt_sigreturn's number */
#define MOV_W8_139 0x52801168u

static volatile i32 g_trap_svc = -1;

void mg_patch_set_trap_svc(bool on) { g_trap_svc = on ? 1 : 0; }

/* 64-bit LDR/STR (unsigned offset): Rt, [Rn, #offset], offset a multiple of 8 below 32768. */
static u32 ldr_x(int rt, int rn, u32 offset) { return 0xF9400000u | ((offset / 8) << 10) | ((u32)rn << 5) | (u32)rt; }
static u32 str_x(int rt, int rn, u32 offset) { return 0xF9000000u | ((offset / 8) << 10) | ((u32)rn << 5) | (u32)rt; }

/* B: 128 MB either way. */
static bool a64_b(u8 *at, mg_addr from, mg_addr to)
{
    i64 off = (i64)(to - from);
    if (off < -(1 << 27) || off >= (1 << 27) || (off & 3)) return false;
    mg_wr32(at, 0x14000000u | (((u32)(off >> 2)) & 0x03FFFFFFu));
    return true;
}

/* Island space the loader keeps for a library (ld/mg_ld.c, island_estimate): one thunk per access, where one is needed. */
u32 mg_patch_count_tp_reads(const u8 *bytes, size_t size)
{
    size_t i;
    u32 n = 0;
    for (i = 0; i + 4 <= size; i += 4) {
        u32 w = mg_rd32(bytes + i) & ~31u;
        if (w == MRS_TPIDR_EL0 || w == MSR_TPIDR_EL0) n++;
    }
    return n;
}

/*
 * The read or write at `site` made one of the host's slot, which is at `offset` from x18 (`where` 18) or from TPIDR_EL0
 * (`where` 0): false when it could not be.
 */
static bool patch_tp_access(mg_lib *lib, u8 *site, bool write, int rt, u32 where, u32 offset)
{
    u8 *st;
    if (where == 18) {
        mg_wr32(site, write ? str_x(rt, 18, offset) : ldr_x(rt, 18, offset));
        return true;
    }
    /* (xzr read into is a read of nothing; and xzr is no base register for the load.) */
    if (rt == 31 && !write) {
        mg_wr32(site, 0xD503201Fu);     /* nop */
        return true;
    }
    if (!write) {
        st = mg_island_alloc(lib, (mg_addr)site, 12, 1u << 27);
        if (!st) return false;
        mg_wr32(st, MRS_TPIDR_EL0 | (u32)rt);
        mg_wr32(st + 4, ldr_x(rt, rt, offset));
        if (!a64_b(st + 8, (mg_addr)(st + 8), (mg_addr)(site + 4))) return false;
    } else {
        /* A register to hold glibc's thread pointer while the guest's is stored, kept on the stack (the ABI keeps nothing below it). */
        int tmp = rt == 16 ? 17 : 16;
        st = mg_island_alloc(lib, (mg_addr)site, 24, 1u << 27);
        if (!st) return false;
        mg_wr32(st, 0xA9BF03E0u | 16u | (17u << 10));          /* stp x16, x17, [sp, #-16]! */
        mg_wr32(st + 4, MRS_TPIDR_EL0 | (u32)tmp);
        mg_wr32(st + 8, str_x(rt, tmp, offset));
        mg_wr32(st + 12, 0xA8C103E0u | 16u | (17u << 10));     /* ldp x16, x17, [sp], #16 */
        if (!a64_b(st + 16, (mg_addr)(st + 16), (mg_addr)(site + 4))) return false;
    }
    return a64_b(site, (mg_addr)site, (mg_addr)st);
}

static void patch_svc_site(mg_lib *lib, u8 *at, bool trap)
{
    mg_svc_site *site = trap ? NULL : malloc(sizeof(*site));
    lib->stats.svc_sites++;
    if (site) {
        site->pc = (mg_addr)at;
        site->watched = false;
        site->tp_reg = -1;
        site->next = lib->svc_sites;
        lib->svc_sites = site;
    } else if (!trap) {
        /* Fail closed: never leave a host system call behind on allocation failure. */
        mg_err("ld: %s: cannot record direct system call; it will fault", lib->name);
    }
    mg_wr32(at, site ? SVC_UDF : SVC_UDF_TRAP);
}

/* Walks [lo, hi) of the library's code, a word at a time. */
static void patch_range(mg_lib *lib, u8 *lo, u8 *hi, bool have, u32 where, u32 offset, bool trap, u32 *svc, u32 *failed)
{
    u8 *p = lib->base + MG_ALIGN_UP((mg_addr)(lo - lib->base), 4);
    for (; p + 4 <= hi; p += 4) {
        u32 w = mg_rd32(p);
        if ((w & ~31u) == MRS_TPIDR_EL0 || (w & ~31u) == MSR_TPIDR_EL0) {
            if (!have || !patch_tp_access(lib, p, (w & ~31u) == MSR_TPIDR_EL0, (int)(w & 31u), where, offset)) {
                lib->stats.tp_reads_failed++;
                if ((*failed)++ < 4)
                    mg_warn("ld: %s+0x%x: an access of the thread pointer is left as it is", lib->name, (unsigned)(p - lib->base));
            } else {
                lib->stats.tp_reads++;
            }
        } else if (w == SVC_0) {
            u32 before = p > lib->base ? mg_rd32(p - 4) : 0;
            if (before == MOV_X8_139 || before == MOV_W8_139) continue;
            patch_svc_site(lib, p, trap);
            (*svc)++;
        }
    }
}

void mg_patch_segment(mg_lib *lib, u8 *seg, size_t size)
{
    u32 where = 0, offset = 0, svc = 0, failed = 0;
    bool have = mg_guest_tp_where(&where, &offset) && (where == 18 || where == 0) && offset % 8 == 0 && offset < 32768;
    bool trap;
    int k;
    if (g_trap_svc < 0) g_trap_svc = mg_flag("trap-svc") ? 1 : 0;
    trap = g_trap_svc > 0;
    if (!have) mg_err("ld: %s: the host has no place for the guest's thread pointer; its reads are left as they are", lib->name);
    if (!lib->ncode) {
        patch_range(lib, seg, seg + size, have, where, offset, trap, &svc, &failed);
    } else {
        for (k = 0; k < lib->ncode; k++) {
            u8 *lo = lib->base + lib->code[k][0], *hi = lib->base + lib->code[k][1];
            if (lo < seg) lo = seg;
            if (hi > seg + size) hi = seg + size;
            if (lo < hi) patch_range(lib, lo, hi, have, where, offset, trap, &svc, &failed);
        }
    }
    if (svc) mg_info("ld: %s: %u system calls of its own made traps%s", lib->name, (unsigned)svc, trap ? " (trap-svc: not done)" : "");
}

bool mg_patch_svc_target(mg_addr pc, mg_addr *target)
{
    mg_lib *lib = mg_ld_lib_of((const void *)pc);
    mg_svc_site *site;
    if (!lib) return false;
    for (site = lib->svc_sites; site; site = site->next) {
        if (site->pc != pc || site->tp_reg >= 0) continue;
        if (mg_rd32((const void *)pc) != SVC_UDF) return false;
        *target = pc + 4;
        return true;
    }
    return false;
}

/* (Every access is rewritten where it is: none is left behind a breakpoint, as some are on 32-bit ARM.) */
bool mg_patch_tp_target(mg_addr pc, int *reg, mg_addr *target)
{
    (void)pc; (void)reg; (void)target;
    return false;
}

/* ------------------------------------------------------ unresolved imports */

static u64 unresolved_called(const char *name)
{
    mg_note_once("guest code called %s, which nothing provides", name);
    return 0;
}

/*
 * A stub that says the name and returns zero, as on the other processors: the name into x0 and the function into x16
 * from the literals behind them, and a branch, which goes into the function as if the caller had called it.
 */
mg_addr mg_patch_unresolved_stub(mg_lib *lib, const char *name)
{
    u8 *st = mg_island_alloc(lib, 0, 32, 0);
    u64 a = (u64)(mg_addr)name, f = (u64)(mg_addr)unresolved_called;
    if (!st) return 0;
    mg_wr32(st, 0x58000080u);           /* ldr x0, #16 */
    mg_wr32(st + 4, 0x580000B0u);       /* ldr x16, #20 */
    mg_wr32(st + 8, 0xD61F0200u);       /* br x16 */
    mg_wr32(st + 12, 0xD503201Fu);      /* nop: the literals 8-aligned */
    memcpy(st + 16, &a, 8);
    memcpy(st + 24, &f, 8);
    return (mg_addr)st;
}

#endif /* MG_ARCH_ARM64 */
