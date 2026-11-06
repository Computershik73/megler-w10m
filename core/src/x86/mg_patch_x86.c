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
 * and only the two slots: what is read at another absolute place of the segment (a thread-local variable at a negative
 * offset, the local-exec model's) is counted and said, and left. An access through a register is the subject of a
 * paragraph of its own, below.
 *
 * The code is walked an instruction at a time (x86/mg_x86_decode.c) through the library's executable sections only
 * (mg_lib's code, read from its section headers), so that a byte of data, or a byte inside another instruction, is never
 * taken for one of these.
 *
 * System calls of the library's own, x86_64's `syscall` (Linux's number in rax, its arguments in rdi, rsi, rdx, r10, r8
 * and r9), would be Windows' system calls here, and the host's own under the tests. Each is led out of the library to
 * the core instead: the instruction and whole instructions next to it -- before it, after it, or both, five bytes or
 * more -- become a jump to a piece of the library's island,
 *
 *         the instructions that were before it, as they were
 *         mov   %gs:<the slot of the guest's thread pointer>, %r11
 *         lea   on(%rip), %rcx
 *         jmp   *mg_x86_syscall_entry
 *     on: the instructions that were after it, as they were
 *         jmp   <where the last of them ended>
 *
 * and traps in the rest of their place, which nothing jumps to. rcx and r11 are what `syscall` itself overwrites (with
 * the return address and the flags), so no code keeps anything in them across one. mg_x86_syscall_entry, below, makes
 * the call by the ABI's number (mg_bionic_syscall, bionic/mg_syscall_abi.c) and puts its result in rax; every other
 * register, the flags and the extended state (XSAVE's: the vector registers, AVX's and AVX-512's halves included, which
 * Windows' own code under the call may change) are as they were.
 *
 * Not a trap, as ARM64's svc is: Windows lays an exception's records on the stack right below rsp, over the 128 bytes
 * System V code may keep there without moving rsp (the red zone). The entry writes nothing in those 128 bytes: on the
 * thread's own stack (where its TEB says it is) its frame goes below them; on any other -- a goroutine's, a few
 * kilobytes, on which Go's runtime expects a system call to write nothing -- it takes the thread's own stack for system
 * calls (ld/mg_tls.c, slot MG_TCB_SYSCALL of its guest block) and tells the TEB that one while it is there, as a fiber's
 * switch does, so that Windows' code under the call (its stack probes, its unwinding) finds its stack where the TEB says.
 * The entry is the core's code, not the library's: a thread in it is not interrupted for a signal (mg_thread_interrupt
 * waits until it is back in guest code), which would find it on that stack and could make a second call there.
 *
 * Only where it can be done safely: the instructions taken with the syscall whole, as the walk decoded them, none a jump,
 * call, return or another system call, and one with an operand relative to rip given the displacement that reaches the
 * same place from the piece; nothing of the library's jumping into the middle of them (its direct jumps and calls are
 * looked at); a place in the island within a jump's reach. Where that cannot be had, the syscall is left as it is and
 * said.
 *
 * An access of the segment through a register -- an instruction whose memory operand is the segment's with a base or an
 * index, `mov %fs:(%rcx), %rax`, `mov %rdx, %fs:-0x10(%rcx)` -- reaches a place at an offset from the thread pointer that
 * a register holds. Go's runtime on android/amd64 reaches its g so, at the offset its variable runtime.tls_g holds
 * (TLS_SLOT_APP's, 16): `mov runtime.tls_g(%rip), %r14; mov %fs:(%r14), %r14` after each call to its assembly, 437 of
 * the 468 such accesses of a small c-shared library of Go 1.27.1's, the rest loads and stores through other registers
 * (setg's `mov %rdi, %fs:(%rax)`, rt0_go's `mov %fs:-0x10(%rcx), %rcx`); and C code of the initial-exec model each of its
 * variables, `mov x@gottpoff(%rip), %rax; mov %fs:(%rax), %eax`. Each such instruction is led out as a system call is,
 * with whole instructions next to it (or alone, where it has five bytes), to a piece that makes the access on the guest's
 * thread pointer:
 *
 *         the instructions that were before it, as they were
 *         mov   %rS, %gs:<the patcher's word>
 *         mov   %gs:<the slot of the guest's thread pointer>, %rS
 *         lea   (%rS,<its base>,1), %rS
 *         the instruction, its segment's prefix gone and %rS its base, its index, scale and displacement as they were
 *         mov   %gs:<the patcher's word>, %rS
 *         the instructions that were after it, as they were
 *         jmp   <where the last of them ended>
 *
 * %rS is one of rcx, rbx, rsi and rdi that the instruction does not name, nor use without naming it: no register but
 * those the instruction writes, and no flag, changes. Its value waits in a word of the host's thread block, beside the
 * thread pointer's (mg_guest_scratch_where), not on the stack: a goroutine's has nothing to spare below its stack
 * pointer. A thread interrupted in the middle of a piece finds the word as it was when it goes back (mg_thread_interrupt
 * keeps it across what it makes the thread run), and nothing else runs on a thread there: a fault in a piece is not
 * given to the app's handler on this processor.
 *
 * Only in a library that says it reaches the segment so: one the linker marked for the initial-exec model (DF_STATIC_TLS,
 * for its TPOFF relocations), or one of Go's (its build ID note, owner "Go"). Elsewhere such bytes have been data among
 * the code -- ChaCha's constant "expand 32-byte k" in libcrypto reads as `and %dh, %fs:(%rbx)` -- which a jump in their
 * place would wreck: they are counted and said, and left, as before. lea (the segment is no part of the address it
 * makes), a hint (prefetch, a long nop) and a jump or call through memory are left too, and an access of 32-bit
 * addresses.
 *
 * x86's `int 0x80` is counted and said, not rewritten: Windows answers it with an access violation, which the core makes the
 * system call of in its place (mg_core.c); and its accesses of %gs through a register are left (Go's android/386 makes
 * them so).
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

/*
 * x86_64's code of an app's own led out to pieces of the library's island -- its system calls, its accesses of the
 * segment through a register -- where guest code runs (not MSVC's build of the tests, which runs none).
 */
#define PIECES (MG_ARCH_X86_64 && MG_GUEST_CODE)

static bool g_trap_svc;

void mg_patch_set_trap_svc(bool on) { g_trap_svc = on; }
bool mg_patch_svc_target(mg_addr pc, mg_addr *target) { (void)pc; (void)target; return false; }
bool mg_patch_tp_target(mg_addr pc, int *reg, mg_addr *target) { (void)pc; (void)reg; (void)target; return false; }

/*
 * A piece of island at most: four instructions of fifteen bytes taken with the site, and the five bytes of the way back;
 * a system call's 30 bytes of the way to the entry, or an access's 9 + 9 + 4 of its address, itself (fourteen bytes,
 * its segment's prefix gone) and 9 of its register given back.
 */
enum { SC_PIECE_MAX = 96, ACCESS_PIECE_MAX = 112 };

#if PIECES
/*
 * What an access of the segment through a register mostly looks like, for the island's estimate: a mov to or from a
 * register or of an immediate (8B, 89, C7), after the segment's prefix and maybe a REX, whose ModRM's memory operand has
 * a base or an index. (Go's are all of them; C's initial-exec loads and stores mostly.)
 */
static bool access_shape(const u8 *p, size_t avail)
{
    size_t at = 1;
    u8 mod, rm;
    if (at < avail && (p[at] & 0xF0) == 0x40) at++;
    if (at + 1 >= avail || (p[at] != 0x8B && p[at] != 0x89 && p[at] != 0xC7)) return false;
    mod = p[at + 1] >> 6;
    rm = p[at + 1] & 7;
    if (mod == 3 || (mod == 0 && rm == 5)) return false;
    /* ([disp32] through a SIB of base 5 and index 4: an absolute place, rewritten where it is.) */
    return !(mod == 0 && rm == 4 && at + 2 < avail && (p[at + 2] & 7) == 5 && ((p[at + 2] >> 3) & 7) == 4);
}
#endif

/*
 * Island space the loader keeps for a library (ld/mg_ld.c, island_estimate, 16 bytes for each one counted): the thread
 * pointer's reads need none, each keeping its place; a system call its piece, for every two bytes that may be one; an
 * access of the segment through a register its piece, for every shape of one (whether the library's are led out is not
 * known yet: see reaches_through_registers).
 */
u32 mg_patch_count_tp_reads(const u8 *bytes, size_t size)
{
#if PIECES
    size_t i;
    u32 n = 0;
    for (i = 0; i + 1 < size; i++) {
        if (bytes[i] == 0x0F && bytes[i + 1] == 0x05) n += SC_PIECE_MAX / 16;
        else if (bytes[i] == GUEST_SEG && access_shape(bytes + i, size - i)) n += ACCESS_PIECE_MAX / 16;
    }
    return n;
#else
    (void)bytes;
    (void)size;
    return 0;
#endif
}

/* A site the walk found -- a system call, or an access of the segment through a register: where, and the instructions it found before it. */
typedef struct site {
    u8 *at;
    u8 len;                            /* (prefixes and all) */
    bool access;                       /* an access of the segment through a register; else a system call */
    u8 *before[3];                     /* the instructions before it, the nearest first; NULL where the walk had none */
    u8 *lo, *hi;                       /* the stretch of code it is in */
    u8 targets[12];                    /* jumps of the library's to [at - 48, at + 48): a bit for each byte */
} site;

/* One walk of a segment's code. */
typedef struct walk {
    mg_lib *lib;
    u8 *seg;
    size_t size;
    u32 tp_seg, tp_at, guard_seg, guard_at, scratch_seg, scratch_at;
    bool have_slots;
    bool through_registers;            /* the library's accesses of the segment through a register are led out */
    u32 odd, calls;
    site *sites;
    u32 nsites, cap;
} walk;

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

#if PIECES
static void add_site(walk *w, u8 *p, const mg_x86_insn *in, u8 *const before[3], u8 *lo, u8 *hi, bool access)
{
    site *s;
    if (w->nsites == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 16;
        w->sites = (site *)mg_realloc(w->sites, sizeof(site) * w->cap);
    }
    s = &w->sites[w->nsites++];
    memset(s, 0, sizeof(*s));
    s->at = p;
    s->len = in->len;
    s->access = access;
    memcpy(s->before, before, sizeof(s->before));
    s->lo = lo;
    s->hi = hi;
}

/*
 * An access of the segment through a register that a piece can make: a memory operand with a base or an index, and not
 * relative to rip or of 32-bit addresses; in the legacy encodings (none such has been seen in VEX's or EVEX's); its base
 * not rsp. Not lea (the segment is no part of the address it makes), not a hint (prefetch, a long nop: no access), not
 * a jump or call through memory (the piece would go on from its own place, a call leave that as the return address).
 */
static bool through_register(const mg_x86_insn *in)
{
    u8 mod = in->modrm >> 6, ext = (in->modrm >> 3) & 7;
    if (in->vex || !in->has_modrm || mod == 3 || in->adsize) return false;
    if (in->has_sib) {
        bool base = !(mod == 0 && (in->sib & 7) == 5), index = ((in->sib >> 3) & 7) != 4 || (in->rex & 2);
        if (!base && !index) return false;
        if (base && (in->sib & 7) == 4 && !(in->rex & 1)) return false;
    } else if (mod == 0 && (in->modrm & 7) == 5) {
        return false;
    }
    if (in->map == 0) return !(in->op == 0x8D || (in->op == 0xFF && ext >= 2 && ext <= 5));
    if (in->map == 1) return !((in->op >= 0x18 && in->op <= 0x1F) || in->op == 0x0D);
    return true;
}

/*
 * Whether the library says that its code reaches the segment through registers: the linker's DF_STATIC_TLS (the
 * initial-exec model's TPOFF relocations), or it is one of Go's (its build ID note, which the loader looked for:
 * mg_ld_lib_is_go). Elsewhere such bytes have been data among the code.
 */
static bool reaches_through_registers(const mg_lib *lib)
{
    const ElfW(Dyn) *d;
    for (d = lib->dyn; d && d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_FLAGS && (d->d_val & DF_STATIC_TLS)) return true;
    return mg_ld_lib_is_go(lib);
}
#endif

/*
 * Walks [lo, hi) of the library's code, an instruction at a time: the thread pointer's reads rewritten; system calls and
 * accesses of the segment through a register found.
 */
static void patch_range(walk *w, u8 *lo, u8 *hi)
{
    u8 *p = lo, *before[3] = { NULL, NULL, NULL };
    while (p < hi) {
        mg_x86_insn in;
        i64 at;
        if (!mg_x86_decode(p, (size_t)(hi - p), IS64, &in)) {
            p++;
            before[0] = before[1] = before[2] = NULL;
            continue;
        }
#if MG_ARCH_X86_64
        if (in.map == 1 && in.op == 0x05 && !in.vex) {                         /* syscall */
            w->calls++;
#if PIECES
            add_site(w, p, &in, before, lo, hi, false);
#endif
        }
#else
        if (in.map == 0 && in.op == 0xCD && p[in.imm_at] == 0x80) w->calls++;  /* int 0x80 */
#endif
        if (in.seg == GUEST_SEG) {
            if (w->have_slots && mg_x86_absolute(&in, p, &at) && (at == 0 || at == GUARD_AT)) {
                if (at == 0) retarget(p, &in, w->tp_seg, w->tp_at);
                else retarget(p, &in, w->guard_seg, w->guard_at);
                w->lib->stats.tp_reads++;
            }
#if PIECES
            else if (w->through_registers && through_register(&in)) {
                add_site(w, p, &in, before, lo, hi, true);
            }
#endif
            else {
                if (w->odd < 4)
                    mg_warn("ld: %s+0x%x: a read of the thread pointer's segment that is not one of its two slots is left as it is",
                            w->lib->name, (unsigned)(p - w->lib->base));
                w->odd++;
                w->lib->stats.tp_reads_failed++;
            }
        }
        before[2] = before[1];
        before[1] = before[0];
        before[0] = p;
        p += in.len;
    }
}

/* Each stretch of the segment's code: its executable sections, or all of it where they are not known. */
static void each_range(walk *w, void (*fn)(walk *, u8 *, u8 *))
{
    mg_lib *lib = w->lib;
    int k;
    if (!lib->ncode) {
        fn(w, w->seg, w->seg + w->size);
        return;
    }
    for (k = 0; k < lib->ncode; k++) {
        u8 *lo = lib->base + lib->code[k][0], *hi = lib->base + lib->code[k][1];
        if (lo < w->seg) lo = w->seg;
        if (hi > w->seg + w->size) hi = w->seg + w->size;
        if (lo < hi) fn(w, lo, hi);
    }
}

#if PIECES
/* ------------------------------------------------ system calls of the library's own */

/* The way out (below), and what it reads of the extended state it keeps. */
void mg_x86_syscall_entry(void);
u64 mg_x86_xsave_mask;
u64 mg_x86_xsave_size = 512;
u8 mg_x86_xsave_use;

/*
 * The thread's stack for system calls (MG_TCB_SYSCALL): a reservation whose lowest page is left out, a guard, and whose
 * top holds the block the entry keeps its words in; the stack is below the block.
 */
enum { SC_STACK = 256 * 1024 };
enum {
    SC_GUEST_SP,                       /* the guest's stack pointer at the call */
    SC_GO_ON,                          /* where it goes on */
    SC_OWN_LO, SC_OWN_HI,              /* the thread's own stack, where the TEB does not say it (the tests' Linux) */
    SC_LIMIT,                          /* the lowest address of the stack for system calls, below it the guard */
    SC_BASE,                           /* its reservation */
    SC_SAVED_BASE, SC_SAVED_LIMIT, SC_SAVED_DEALLOC,  /* the TEB's while it says the block's stack */
    SC_WORDS = 16
};

void *mg_patch_syscall_stack_new(void *own_lo, void *own_hi)
{
    u8 *base = (u8 *)mg_mem_reserve(SC_STACK);
    mg_addr *block;
    if (!base || !mg_mem_commit(base + MG_PAGE, SC_STACK - MG_PAGE, MG_PROT_RW)) {
        if (base) mg_mem_release(base, SC_STACK);
        mg_err("ld: no stack for a thread's system calls: one of them there ends the process");
        return NULL;
    }
    block = (mg_addr *)(base + SC_STACK - SC_WORDS * sizeof(mg_addr));
    block[SC_OWN_LO] = (mg_addr)own_lo;
    block[SC_OWN_HI] = (mg_addr)own_hi;
    block[SC_LIMIT] = (mg_addr)(base + MG_PAGE);
    block[SC_BASE] = (mg_addr)base;
    return block;
}

void mg_patch_syscall_stack_free(void *block) { mg_mem_release((u8 *)block + SC_WORDS * sizeof(mg_addr) - SC_STACK, SC_STACK); }

/*
 * The call the entry makes, in System V's convention on the stack it chose (`on_block`: the block's). The thread's record
 * (mg_gthread) may be made by this call, with the bounds Windows says of the thread's stack, which while the TEB says
 * the block's stack are that one's: it is given the thread's own instead.
 */
long mg_x86_syscall_call(long nr, const long args[6], const mg_addr *block, long on_block)
{
    /* (The site: the piece of island it goes on from, which is the library's.) */
#if defined(MG_CORE_ELF)
    void **tp = on_block ? (void **)mg_guest_tp_get() : NULL;
    bool made = tp && !tp[1];
    long r = mg_bionic_syscall(nr, args, block[SC_GO_ON]);
    if (made && tp[1]) {
        mg_gthread *g = (mg_gthread *)tp[1];
        g->stack_lo = (void *)block[SC_SAVED_DEALLOC];
        g->stack_hi = (void *)block[SC_SAVED_BASE];
    }
    return r;
#else
    (void)on_block;
    return mg_bionic_syscall(nr, args, block[SC_GO_ON]);
#endif
}

/*
 * The entry: from a site's piece of island, r11 the guest's thread pointer, rcx where to go on, everything else as at the
 * syscall. The block's words are at their SC_* places, 8 bytes each; the registers pushed below the flags and rax, rbp
 * pointing at them: r15 0, r14 8, r13 16, r12 24, r10 32, r9 40, r8 48, rdi 56, rsi 64, rbp 72, rbx 80, rdx 88, rax 96,
 * the flags 104. On Windows the thread's own stack is [DeallocationStack, StackBase) of its TEB (gs:0x1478, gs:8), and
 * the TEB is told the block's while it is used: StackBase the block, StackLimit (gs:0x10) its limit, DeallocationStack
 * its reservation.
 */
_Static_assert(MG_TCB_SYSCALL * 8 == 120, "the entry reads the block from the thread pointer + 120");
#if defined(MG_CORE_ELF)
#define SC_OWN_STACK "cmpq %gs:0x1478, %rax\n jb 1f\n cmpq %gs:8, %rax\n jae 1f\n"
#define SC_TEB_IN                                                                                                    \
    "movq %gs:8, %rax\n movq %rax, 48(%rbx)\n movq %gs:16, %rax\n movq %rax, 56(%rbx)\n"                            \
    "movq %gs:0x1478, %rax\n movq %rax, 64(%rbx)\n movq %rbx, %gs:8\n movq 32(%rbx), %rax\n movq %rax, %gs:16\n"    \
    "movq 40(%rbx), %rax\n movq %rax, %gs:0x1478\n"
#define SC_TEB_OUT "movq 48(%rbx), %rax\n movq %rax, %gs:8\n movq 56(%rbx), %rax\n movq %rax, %gs:16\n movq 64(%rbx), %rax\n movq %rax, %gs:0x1478\n"
#else
#define SC_OWN_STACK "cmpq 16(%r11), %rax\n jb 1f\n cmpq 24(%r11), %rax\n jae 1f\n"
#define SC_TEB_IN ""
#define SC_TEB_OUT ""
#endif
__asm__(".text\n"
        ".p2align 4\n"
        ".globl mg_x86_syscall_entry\n"
        ".hidden mg_x86_syscall_entry\n"
        ".type mg_x86_syscall_entry, @function\n"
        "mg_x86_syscall_entry:\n"
        "movq 120(%r11), %r11\n"                    /* the thread's block, its slot MG_TCB_SYSCALL */
        "movq %rsp, 0(%r11)\n"
        "movq %rcx, 8(%r11)\n"
        "movq %r11, %rsp\n"                         /* the flags and rax below the block while the stack is chosen */
        "pushfq\n"
        "pushq %rax\n"
        "movq 0(%r11), %rax\n"
        SC_OWN_STACK
        "leaq -128(%rax), %rsp\n"                   /* the thread's own: below its red zone, the two moved there */
        "pushq -8(%r11)\n"
        "pushq -16(%r11)\n"
        "xorl %ecx, %ecx\n"
        "jmp 2f\n"
        "1:\n"
        "movl $1, %ecx\n"                           /* another (a goroutine's): the block's */
        "2:\n"
        "pushq %rdx\n pushq %rbx\n pushq %rbp\n pushq %rsi\n pushq %rdi\n pushq %r8\n"
        "pushq %r9\n pushq %r10\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n"
        "cld\n"
        "movq %rsp, %rbp\n"
        "movq %r11, %rbx\n"
        "movq %rcx, %r12\n"
        "testq %r12, %r12\n"
        "jz 3f\n"
        SC_TEB_IN
        "3:\n"
        "andq $-64, %rsp\n"                         /* the extended state */
        "subq mg_x86_xsave_size(%rip), %rsp\n"
        "cmpb $0, mg_x86_xsave_use(%rip)\n"
        "je 4f\n"
        /* The header zero first: XSAVE writes only XSTATE_BV's bits of the mask, and XRSTOR faults on any other bit set
           there or on anything in the rest of the header (the stack's old bytes, "4.4.0-" of uname's, did so). */
        "xorl %eax, %eax\n"
        "movq %rax, 512(%rsp)\n movq %rax, 520(%rsp)\n movq %rax, 528(%rsp)\n movq %rax, 536(%rsp)\n"
        "movq %rax, 544(%rsp)\n movq %rax, 552(%rsp)\n movq %rax, 560(%rsp)\n movq %rax, 568(%rsp)\n"
        "movl mg_x86_xsave_mask(%rip), %eax\n"
        "movl 4+mg_x86_xsave_mask(%rip), %edx\n"
        "xsave64 (%rsp)\n"
        "jmp 5f\n"
        "4:\n"
        "fxsave64 (%rsp)\n"
        "5:\n"
        "subq $48, %rsp\n"                          /* Linux's arguments: rdi, rsi, rdx, r10, r8, r9 */
        "movq 56(%rbp), %rax\n movq %rax, 0(%rsp)\n"
        "movq 64(%rbp), %rax\n movq %rax, 8(%rsp)\n"
        "movq 88(%rbp), %rax\n movq %rax, 16(%rsp)\n"
        "movq 32(%rbp), %rax\n movq %rax, 24(%rsp)\n"
        "movq 48(%rbp), %rax\n movq %rax, 32(%rsp)\n"
        "movq 40(%rbp), %rax\n movq %rax, 40(%rsp)\n"
        "movq 96(%rbp), %rdi\n"                     /* the number */
        "movq %rsp, %rsi\n"
        "movq %rbx, %rdx\n"
        "movq %r12, %rcx\n"
        "call mg_x86_syscall_call\n"
        "movq %rax, 96(%rbp)\n"                     /* the result, rax's */
        "addq $48, %rsp\n"
        "cmpb $0, mg_x86_xsave_use(%rip)\n"
        "je 6f\n"
        "movl mg_x86_xsave_mask(%rip), %eax\n"
        "movl 4+mg_x86_xsave_mask(%rip), %edx\n"
        "xrstor64 (%rsp)\n"
        "jmp 7f\n"
        "6:\n"
        "fxrstor64 (%rsp)\n"
        "7:\n"
        "testq %r12, %r12\n"
        "jz 8f\n"
        SC_TEB_OUT
        "8:\n"
        "movq %rbx, %r11\n"
        "movq %rbp, %rsp\n"
        "popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %r10\n popq %r9\n"
        "popq %r8\n popq %rdi\n popq %rsi\n popq %rbp\n popq %rbx\n popq %rdx\n"
        "popq %rax\n"
        "popfq\n"
        "movq 8(%r11), %rcx\n"
        "movq 0(%r11), %rsp\n"
        "jmp *%rcx\n"
        ".size mg_x86_syscall_entry, .-mg_x86_syscall_entry\n");

static void cpuid(u32 leaf, u32 sub, u32 r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

/*
 * What the entry keeps of the extended state: XSAVE's x87, SSE, AVX and AVX-512 parts as far as the system has turned
 * them on (XCR0), in the standard form's size for them; FXSAVE's 512 bytes where it has no XSAVE. (AMX's tiles, which
 * a thread asks for, and the protection keys are left.)
 */
static void xsave_setup(void)
{
    static bool done;
    u32 r[4], lo, hi, k;
    u64 mask, size = 576;
    if (done) return;
    done = true;
    cpuid(1, 0, r);
    if (!(r[2] & (1u << 27))) return;                  /* no OSXSAVE */
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    mask = (((u64)hi << 32) | lo) & 0xE7;
    for (k = 2; k < 8; k++) {
        if (!(mask & (1u << k))) continue;
        cpuid(0xD, k, r);
        if ((u64)r[1] + r[0] > size) size = (u64)r[1] + r[0];
    }
    mg_x86_xsave_mask = mask;
    mg_x86_xsave_size = MG_ALIGN_UP(size, 64);
    mg_x86_xsave_use = 1;
}

/* ------------------------------------------------ the places that become jumps to pieces of island */

static int site_order(const void *a, const void *b)
{
    const u8 *x = ((const site *)a)->at, *y = ((const site *)b)->at;
    return x < y ? -1 : x > y;
}

/* The target of `in` at `p`, if it is a direct jump or call. */
static bool branch_target(const mg_x86_insn *in, const u8 *p, mg_addr *target)
{
    i32 rel;
    if (in->vex) return false;
    if (in->map == 0 && ((in->op >= 0x70 && in->op <= 0x7F) || (in->op >= 0xE0 && in->op <= 0xE3) || in->op == 0xEB)) {
        rel = (i8)p[in->imm_at];
    } else if (((in->map == 0 && (in->op == 0xE8 || in->op == 0xE9)) || (in->map == 1 && in->op >= 0x80 && in->op <= 0x8F)) &&
               in->imm_size == 4) {
        memcpy(&rel, p + in->imm_at, 4);
    } else {
        return false;
    }
    *target = (mg_addr)p + in->len + (mg_addr)(i64)rel;
    return true;
}

/* Every direct jump and call of [lo, hi) that lands within 48 bytes of a site: marked there. */
static void find_targets(walk *w, u8 *lo, u8 *hi)
{
    u8 *p = lo;
    while (p < hi) {
        mg_x86_insn in;
        mg_addr t;
        u32 a, b;
        if (!mg_x86_decode(p, (size_t)(hi - p), IS64, &in)) {
            p++;
            continue;
        }
        p += in.len;
        if (!branch_target(&in, p - in.len, &t)) continue;
        /* The first site at or past t - 48, then each up to t + 48. */
        for (a = 0, b = w->nsites; a < b;) {
            u32 mid = (a + b) / 2;
            if ((mg_addr)w->sites[mid].at + 48 <= t) a = mid + 1;
            else b = mid;
        }
        for (; a < w->nsites && (mg_addr)w->sites[a].at <= t + 48; a++) {
            mg_addr bit = t + 48 - (mg_addr)w->sites[a].at;
            if (bit < 96) w->sites[a].targets[bit / 8] |= (u8)(1u << (bit % 8));
        }
    }
}

/* An operand relative to rip: ModRM's mod 0 and rm 5, which on x86_64 is rip plus the displacement. */
static bool rip_relative(const mg_x86_insn *in) { return in->has_modrm && !in->has_sib && (in->modrm >> 6) == 0 && (in->modrm & 7) == 5; }

/*
 * An instruction that runs elsewhere as it does here: no jump, call, return or system call. One with an operand relative
 * to rip is moved with its displacement made right for its new place (copy_moved) -- not one of 32-bit addresses, whose
 * eip-relative address is cut to 32 bits. Not an access of the segment through a register either: a site of its own,
 * or one left, which a piece would make as it is.
 */
static bool movable(const mg_x86_insn *in)
{
    u8 op = in->op;
    if (rip_relative(in) && in->adsize) return false;
    if (in->seg == GUEST_SEG && through_register(in)) return false;
    if (in->vex) return true;
    if (in->map == 0)
        return !((op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) || op == 0xE8 || op == 0xE9 || op == 0xEB || op == 0xC2 ||
                 op == 0xC3 || op == 0xCA || op == 0xCB || op == 0xCC || op == 0xCD || op == 0xCF || op == 0xF4 ||
                 (op == 0xFF && ((in->modrm >> 3) & 7) >= 2 && ((in->modrm >> 3) & 7) <= 5));
    if (in->map == 1)
        return !((op >= 0x80 && op <= 0x8F) || op == 0x05 || op == 0x07 || op == 0x0B || op == 0x34 || op == 0x35 || op == 0xB9 ||
                 op == 0xFF);
    return true;
}

static bool jumped_into(const site *s, const u8 *start, const u8 *end)
{
    const u8 *p;
    for (p = start + 1; p < end; p++) {
        mg_addr bit = (mg_addr)p + 48 - (mg_addr)s->at;
        if (bit < 96 && ((s->targets[bit / 8] >> (bit % 8)) & 1)) return true;
    }
    return false;
}

/*
 * The place the jump takes, [start, end): the site with whole instructions around it, five bytes or more (or the site
 * alone, where it has five), every one movable, none before `free_from` (a site before it already took that), nothing
 * jumping into it but at its start. The fewest instructions, those before it first. False when there is none.
 */
static bool window(const site *s, const u8 *free_from, u8 **start, u8 **end)
{
    u8 *from[4], *to[4];
    int nbefore = 0, nafter = 0, n, k;
    from[0] = s->at;
    while (nbefore < 3 && s->before[nbefore]) {
        mg_x86_insn in;
        u8 *p = s->before[nbefore];
        if (p < s->lo || p < free_from || !mg_x86_decode(p, (size_t)(s->hi - p), IS64, &in) || p + in.len != from[nbefore] || !movable(&in))
            break;
        from[++nbefore] = p;
    }
    to[0] = s->at + s->len;
    while (nafter < 3 && to[nafter] < s->hi) {
        mg_x86_insn in;
        if (!mg_x86_decode(to[nafter], (size_t)(s->hi - to[nafter]), IS64, &in) || !movable(&in)) break;
        to[nafter + 1] = to[nafter] + in.len;
        nafter++;
    }
    for (n = 0; n <= nbefore + nafter && n <= 4; n++) {
        for (k = n < nbefore ? n : nbefore; k >= 0 && n - k <= nafter; k--) {
            if (to[n - k] - from[k] < 5 || jumped_into(s, from[k], to[n - k])) continue;
            *start = from[k];
            *end = to[n - k];
            return true;
        }
    }
    return false;
}

/*
 * Whole instructions [from, to) copied to `dst`, each with an operand relative to rip given the displacement that reaches
 * the same place from its new one. False when one cannot (the place more than 2 GB away from there).
 */
static bool copy_moved(u8 *dst, const u8 *from, const u8 *to)
{
    while (from < to) {
        mg_x86_insn in;
        if (!mg_x86_decode(from, (size_t)(to - from), IS64, &in)) return false;
        memcpy(dst, from, in.len);
        if (rip_relative(&in)) {
            i32 d;
            i64 moved;
            memcpy(&d, from + in.disp_at, 4);
            moved = (i64)d + (i64)((mg_addr)from - (mg_addr)dst);
            if (moved != (i32)moved) return false;
            d = (i32)moved;
            memcpy(dst + in.disp_at, &d, 4);
        }
        dst += in.len;
        from += in.len;
    }
    return true;
}

/* The site's piece of island, and the jump to it in [start, end): false when the island has no room within reach. */
static bool lead_out(walk *w, const site *s, u8 *start, u8 *end)
{
    size_t before = (size_t)(s->at - start), after = (size_t)(end - (s->at + s->len));
    u8 *piece = mg_island_alloc(w->lib, (mg_addr)start, before + 9 + 7 + 14 + after + 5, 0x7FFF0000u), *q, *on;
    u64 entry = (u64)(mg_addr)mg_x86_syscall_entry;
    i32 rel;
    if (!piece) return false;
    q = piece;
    if (!copy_moved(q, start, s->at)) return false;
    q += before;
    *q++ = (u8)w->tp_seg;                              /* mov %seg:tp_at, %r11 */
    *q++ = 0x4C;
    *q++ = 0x8B;
    *q++ = 0x1C;
    *q++ = 0x25;
    memcpy(q, &w->tp_at, 4);
    q += 4;
    on = q + 7 + 14;
    *q++ = 0x48;                                       /* lea on(%rip), %rcx */
    *q++ = 0x8D;
    *q++ = 0x0D;
    rel = (i32)(on - (q + 4));
    memcpy(q, &rel, 4);
    q += 4;
    *q++ = 0xFF;                                       /* jmp *0(%rip), the entry's address after it */
    *q++ = 0x25;
    memset(q, 0, 4);
    q += 4;
    memcpy(q, &entry, 8);
    q += 8;
    if (!copy_moved(q, s->at + s->len, end)) return false;   /* on: what was after it */
    q += after;
    *q++ = 0xE9;                                       /* jmp end */
    rel = (i32)(end - (q + 4));
    memcpy(q, &rel, 4);
    start[0] = 0xE9;                                   /* the jump to the piece; traps in the rest of the place */
    rel = (i32)(piece - (start + 5));
    memcpy(start + 1, &rel, 4);
    memset(start + 5, 0xCC, (size_t)(end - start - 5));
    return true;
}

/* ------------------------------------------------ accesses of the segment through a register */

/*
 * The register a piece keeps the address in (S above): the first of rcx, rbx, rsi and rdi -- none needs a REX, none is
 * a ModRM's or a SIB's special case -- that the instruction does not name, as its ModRM's reg or the operand's base or
 * index (each taken as the register of its low three bits, which costs only a choice; a reg of 4 to 7 with no REX as
 * the one whose byte ah, ch, dh or bh it may be too), nor uses without naming it: rax and rdx never (mul, div, cmpxchg,
 * xsave), rcx where a shift counts by cl and in shld, shrd and cmpxchg16b, rbx in cmpxchg16b. -1 when all four are.
 */
static int scratch_reg(const mg_x86_insn *in)
{
    static const int order[4] = { 1, 3, 6, 7 };
    int reg = (in->modrm >> 3) & 7, k;
    u32 used = 1u << 0 | 1u << 2 | 1u << reg;
    if (!in->rex && reg >= 4) used |= 1u << (reg - 4);
    if (in->has_sib) used |= 1u << (in->sib & 7) | 1u << ((in->sib >> 3) & 7);
    else used |= 1u << (in->modrm & 7);
    if ((in->map == 0 && (in->op == 0xD2 || in->op == 0xD3)) || (in->map == 1 && (in->op == 0xA5 || in->op == 0xAD))) used |= 1u << 1;
    if (in->map == 1 && in->op == 0xC7) used |= 1u << 1 | 1u << 3;
    for (k = 0; k < 4; k++)
        if (!(used & (1u << order[k]))) return order[k];
    return -1;
}

/* The memory operand's base register (REX.B its fourth bit), or -1 where it has none ([disp32 + index * scale]). */
static int base_reg(const mg_x86_insn *in)
{
    if (in->has_sib) {
        if ((in->modrm >> 6) == 0 && (in->sib & 7) == 5) return -1;
        return (in->sib & 7) | (in->rex & 1) << 3;
    }
    return (in->modrm & 7) | (in->rex & 1) << 3;
}

/*
 * The instruction `in` at `p` written at `out` with its segments' prefixes left out (the others are no part of an address
 * on x86_64) and `r` its base, every other byte as it was: one where it had none takes ModRM's mod 2, whose 32-bit
 * displacement it had already. Its length.
 */
static size_t rebase(u8 *out, const u8 *p, const mg_x86_insn *in, int r)
{
    size_t n = 0, i, opcode = (size_t)in->op_at - (in->map == 0 ? 0 : in->map == 1 ? 1 : 2), cut;
    u8 *modrm;
    for (i = 0; i < in->len; i++) {
        u8 b = p[i];
        if (i < opcode && (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65)) continue;
        out[n++] = b;
    }
    cut = in->len - n;
    /* (The prefixes all come before the opcode, REX right before it.) */
    if (in->rex) out[opcode - 1 - cut] &= (u8)~1u;
    modrm = out + in->modrm_at - cut;
    if (in->has_sib) {
        if ((*modrm >> 6) == 0 && (modrm[1] & 7) == 5) *modrm = (u8)((*modrm & 0x3F) | 0x80);
        modrm[1] = (u8)((modrm[1] & 0xF8) | r);
    } else {
        *modrm = (u8)((*modrm & 0xF8) | r);
    }
    return n;
}

/* `mov %r, %seg:at` (op 0x89) or `mov %seg:at, %r` (0x8B), r one of the first eight registers: nine bytes. */
static u8 *seg_word(u8 *q, u32 seg, u8 op, int r, u32 at)
{
    *q++ = (u8)seg;
    *q++ = 0x48;
    *q++ = op;
    *q++ = (u8)(r << 3 | 4);
    *q++ = 0x25;
    memcpy(q, &at, 4);
    return q + 4;
}

/* An access's piece of island (at the top), and the jump to it in [start, end): false when it cannot be made. */
static bool lead_out_access(walk *w, const site *s, u8 *start, u8 *end)
{
    size_t before = (size_t)(s->at - start), after = (size_t)(end - (s->at + s->len));
    mg_x86_insn in;
    int r, base;
    u8 *piece, *q;
    i32 rel;
    if (!mg_x86_decode(s->at, s->len, IS64, &in) || (r = scratch_reg(&in)) < 0) return false;
    base = base_reg(&in);
    piece = mg_island_alloc(w->lib, (mg_addr)start, before + 9 + 9 + (base >= 0 ? 4 : 0) + in.len + 9 + after + 5, 0x7FFF0000u);
    if (!piece) return false;
    q = piece;
    if (!copy_moved(q, start, s->at)) return false;
    q += before;
    q = seg_word(q, w->scratch_seg, 0x89, r, w->scratch_at);          /* mov %r, %seg:scratch */
    q = seg_word(q, w->tp_seg, 0x8B, r, w->tp_at);                     /* mov %seg:tp, %r */
    if (base >= 0) {
        *q++ = (u8)(0x48 | (base >= 8 ? 2 : 0));                       /* lea (%r,%base,1), %r */
        *q++ = 0x8D;
        *q++ = (u8)(r << 3 | 4);
        *q++ = (u8)((base & 7) << 3 | r);
    }
    q += rebase(q, s->at, &in, r);
    q = seg_word(q, w->scratch_seg, 0x8B, r, w->scratch_at);          /* mov %seg:scratch, %r */
    if (!copy_moved(q, s->at + s->len, end)) return false;
    q += after;
    *q++ = 0xE9;                                                       /* jmp end */
    rel = (i32)(end - (q + 4));
    memcpy(q, &rel, 4);
    start[0] = 0xE9;                                   /* the jump to the piece; traps in the rest of the place */
    rel = (i32)(piece - (start + 5));
    memcpy(start + 1, &rel, 4);
    memset(start + 5, 0xCC, (size_t)(end - start - 5));
    return true;
}

/* Every site the walk found led out, where it can be; said where it cannot. */
static void lead_out_all(walk *w)
{
    u32 i, calls = 0, calls_left = 0, accesses = 0, accesses_left = 0;
    u8 *free_from = NULL;
    qsort(w->sites, w->nsites, sizeof(site), site_order);
    each_range(w, find_targets);
    xsave_setup();
    for (i = 0; i < w->nsites; i++) {
        site *s = &w->sites[i];
        u8 *start, *end;
        if (w->have_slots && window(s, free_from, &start, &end) &&
            (s->access ? lead_out_access(w, s, start, end) : lead_out(w, s, start, end))) {
            free_from = end;
            if (s->access) accesses++;
            else calls++;
            continue;
        }
        if (s->access ? accesses_left++ < 4 : calls_left++ < 4)
            mg_warn("ld: %s+0x%x: %s is left as it is: %s", w->lib->name, (unsigned)(s->at - w->lib->base),
                    s->access ? "an access of the thread pointer's segment through a register" : "a system call of its own",
                    !w->have_slots ? "no slot for the thread pointer" : "no whole instructions around it to take its place");
    }
    if (calls) mg_tls_syscall_stacks();
    w->lib->stats.svc_sites += calls;
    w->lib->stats.tp_led_out += accesses;
    w->lib->stats.tp_reads_failed += accesses_left;
    if (calls || calls_left) {
        mg_info("ld: %s: %u system calls of its own go to the core%s", w->lib->name, (unsigned)calls, calls_left ? "" : ", all of them");
        if (calls_left)
            mg_warn("ld: %s: %u system calls of its own are left as they are: they would be the host's", w->lib->name, (unsigned)calls_left);
    }
    if (accesses || accesses_left) {
        mg_info("ld: %s: %u accesses of the thread pointer's segment through a register go to pieces of island%s", w->lib->name,
                (unsigned)accesses, accesses_left ? "" : ", all of them");
        if (accesses_left)
            mg_warn("ld: %s: %u accesses of the thread pointer's segment through a register are left as they are: they would reach the host's",
                    w->lib->name, (unsigned)accesses_left);
    }
}
#endif /* PIECES */

void mg_patch_segment(mg_lib *lib, u8 *seg, size_t size)
{
    walk w;
    memset(&w, 0, sizeof(w));
    w.lib = lib;
    w.seg = seg;
    w.size = size;
    w.have_slots = mg_guest_tp_where(&w.tp_seg, &w.tp_at) && mg_guest_guard_where(&w.guard_seg, &w.guard_at);
    if (!w.have_slots) mg_err("ld: %s: the host has no place for the guest's thread pointer; its reads are left as they are", lib->name);
#if PIECES
    w.through_registers = w.have_slots && reaches_through_registers(lib) && mg_guest_scratch_where(&w.scratch_seg, &w.scratch_at);
#endif
    each_range(&w, patch_range);
#if PIECES
    if (w.nsites) lead_out_all(&w);
    free(w.sites);
#else
    if (w.calls) {
        lib->stats.svc_sites += w.calls;
        if (IS64)
            mg_warn("ld: %s: %u system calls of its own (syscall); they are not intercepted on this processor yet", lib->name, (unsigned)w.calls);
        else
            mg_info("ld: %s: %u system calls of its own (int 0x80), each made by the core at the fault it gives", lib->name, (unsigned)w.calls);
    }
#endif
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
