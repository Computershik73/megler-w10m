/*
 * What is rewritten in an x86_64 or x86 library's code before it is made executable (arm/mg_patch.c is 32-bit ARM's).
 *
 * bionic keeps its thread pointer in a segment: %fs on x86_64, %gs on x86, whose base is the thread's slot array (bionic's
 * TLS_SLOT_SELF, slot 0, holds the array's own address). Compiled code reads two of its slots inline, everywhere:
 *
 *   - the thread pointer itself, `mov %fs:0, %rax` (x86: `mov %gs:0, %eax`), for every thread-local variable of the
 *     initial-exec kind and for bionic's own inline __get_tls();
 *   - the stack guard, slot 5: `mov %fs:0x28, %rax` in the prologue of every function with an array on its stack, and a
 *     `sub` or `xor` of the same in its epilogue (x86: %gs:0x14). Thousands of them in a library: 3612 in libicui18n.
 *
 * Neither segment is Android's here. On Windows gs is the TEB on x86_64 and fs on x86, and the other has no base user
 * code can set; on Linux (the tests) fs is glibc's. So each such instruction is given the host's segment and the place
 * of a slot of the host's own thread block that holds the same value on every guest thread (mg_guest_tp_where,
 * mg_guest_guard_where): on Windows x86_64 `mov %gs:0x1480+8*slot, %rax`, the TEB's TlsSlots. The instruction keeps its
 * length and every other byte -- the segment is a prefix, the address a 32-bit displacement (or an absolute moffs) --
 * so nothing moves, no island is needed, and no register or flag changes that the original did not change.
 *
 * Only the instructions whose memory operand is an absolute address in the segment, [disp32] or moffs, are of this kind,
 * and only the two slots: what is read at another place of the segment (a thread-local variable at a negative offset,
 * the initial-exec model's other form; a segment-relative access through a register) is counted and said, and left.
 *
 * The code is walked an instruction at a time (x86/mg_x86_decode.c) through the library's executable sections only
 * (mg_lib's code, read from its section headers), so that a byte of data, or a byte inside another instruction, is never
 * taken for one of these.
 *
 * System calls (`syscall`, `int 0x80`) are counted and said, not yet rewritten.
 */
#include "../mg_core.h"
#include "../bionic/mg_bionic.h"
#include "../host/mg_host.h"
#include "../ld/mg_ld_internal.h"
#include "mg_x86_decode.h"

#if MG_ARCH_X86_64 || MG_ARCH_X86

#if MG_ARCH_X86_64
#define GUEST_SEG 0x64                 /* %fs */
#define GUARD_AT 0x28                  /* bionic's slot 5, eight bytes each */
#define IS64 true
#else
#define GUEST_SEG 0x65                 /* %gs */
#define GUARD_AT 0x14                  /* slot 5, four bytes each */
#define IS64 false
#endif

static bool g_trap_svc;

void mg_patch_set_trap_svc(bool on) { g_trap_svc = on; }
bool mg_patch_svc_target(mg_addr pc, mg_addr *target) { (void)pc; (void)target; return false; }
bool mg_patch_tp_target(mg_addr pc, int *reg, mg_addr *target) { (void)pc; (void)reg; (void)target; return false; }

/* Nothing of these needs an island: every rewrite keeps its place. */
u32 mg_patch_count_tp_reads(const u8 *bytes, size_t size) { (void)bytes; (void)size; return 0; }

/* The segment-relative operand of `in` at `p` made `seg`:`offset` (two's complement where it is below the segment's base). */
static void retarget(u8 *p, const mg_x86_insn *in, u32 seg, u32 offset)
{
    p[in->seg_at] = (u8)seg;
    if (in->moffs) {
        if (in->imm_size == 8) {
            u64 v = (u64)(i64)(i32)offset;
            memcpy(p + in->imm_at, &v, 8);
        } else {
            memcpy(p + in->imm_at, &offset, 4);
        }
    } else {
        memcpy(p + in->disp_at, &offset, 4);
    }
}

/* Walks [lo, hi) of the library's code, an instruction at a time. */
static void patch_range(mg_lib *lib, u8 *lo, u8 *hi, u32 tp_seg, u32 tp_at, u32 guard_seg, u32 guard_at, bool have_slots, u32 *odd, u32 *calls)
{
    u8 *p = lo;
    while (p < hi) {
        mg_x86_insn in;
        i64 at;
        if (!mg_x86_decode(p, (size_t)(hi - p), IS64, &in)) {
            p++;
            continue;
        }
#if MG_ARCH_X86_64
        if (in.map == 1 && in.op == 0x05 && !in.vex) (*calls)++;              /* syscall */
#else
        if (in.map == 0 && in.op == 0xCD && p[in.imm_at] == 0x80) (*calls)++; /* int 0x80 */
#endif
        if (in.seg == GUEST_SEG) {
            if (have_slots && mg_x86_absolute(&in, p, &at) && (at == 0 || at == GUARD_AT)) {
                if (at == 0) retarget(p, &in, tp_seg, tp_at);
                else retarget(p, &in, guard_seg, guard_at);
                lib->stats.tp_reads++;
            } else {
                if (*odd < 4)
                    mg_warn("ld: %s+0x%x: a read of the thread pointer's segment that is not one of its two slots is left as it is",
                            lib->name, (unsigned)(p - lib->base));
                (*odd)++;
                lib->stats.tp_reads_failed++;
            }
        }
        p += in.len;
    }
}

void mg_patch_segment(mg_lib *lib, u8 *seg, size_t size)
{
    u32 tp_seg = 0, tp_at = 0, guard_seg = 0, guard_at = 0, odd = 0, calls = 0;
    bool have = mg_guest_tp_where(&tp_seg, &tp_at) && mg_guest_guard_where(&guard_seg, &guard_at);
    int k;
    if (!have) mg_err("ld: %s: the host has no place for the guest's thread pointer; its reads are left as they are", lib->name);
    if (!lib->ncode) {
        patch_range(lib, seg, seg + size, tp_seg, tp_at, guard_seg, guard_at, have, &odd, &calls);
    } else {
        for (k = 0; k < lib->ncode; k++) {
            u8 *lo = lib->base + lib->code[k][0], *hi = lib->base + lib->code[k][1];
            if (lo < seg) lo = seg;
            if (hi > seg + size) hi = seg + size;
            if (lo < hi) patch_range(lib, lo, hi, tp_seg, tp_at, guard_seg, guard_at, have, &odd, &calls);
        }
    }
    if (calls) {
        lib->stats.svc_sites += calls;
        mg_warn("ld: %s: %u system calls of its own (%s); they are not intercepted on this processor yet", lib->name, (unsigned)calls,
                IS64 ? "syscall" : "int 0x80");
    }
}

/* ------------------------------------------------------ unresolved imports */

static u64 unresolved_called(const char *name)
{
    mg_note_once("guest code called %s, which nothing provides", name);
    return 0;
}

/*
 * A stub that says the name and returns zero, as on ARM: on x86_64 `movabs rdi, name; movabs rax, fn; jmp rax`, which
 * goes into the function as if the caller had called it; on x86 the name pushed on a stack kept at Android's 16 bytes,
 * the call, and back.
 */
mg_addr mg_patch_unresolved_stub(mg_lib *lib, const char *name)
{
#if MG_ARCH_X86_64
    u8 *st = mg_island_alloc(lib, 0, 24, 0);
    u64 a = (u64)(mg_addr)name, f = (u64)(mg_addr)unresolved_called;
    if (!st) return 0;
    st[0] = 0x48; st[1] = 0xBF; memcpy(st + 2, &a, 8);       /* movabs rdi, name */
    st[10] = 0x48; st[11] = 0xB8; memcpy(st + 12, &f, 8);    /* movabs rax, unresolved_called */
    st[20] = 0xFF; st[21] = 0xE0;                            /* jmp rax */
    return (mg_addr)st;
#else
    u8 *st = mg_island_alloc(lib, 0, 20, 0);
    u32 a = (u32)(mg_addr)name, f = (u32)(mg_addr)unresolved_called;
    if (!st) return 0;
    st[0] = 0x83; st[1] = 0xEC; st[2] = 0x08;                /* sub esp, 8: with the name, 16 below the return address */
    st[3] = 0x68; memcpy(st + 4, &a, 4);                     /* push name */
    st[8] = 0xB8; memcpy(st + 9, &f, 4);                     /* mov eax, unresolved_called */
    st[13] = 0xFF; st[14] = 0xD0;                            /* call eax */
    st[15] = 0x83; st[16] = 0xC4; st[17] = 0x0C;             /* add esp, 12 */
    st[18] = 0xC3;                                           /* ret */
    return (mg_addr)st;
#endif
}

#endif /* MG_ARCH_X86_64 || MG_ARCH_X86 */
