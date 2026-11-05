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
 * x86's `int 0x80` is counted and said, not rewritten.
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

/* x86_64's own system calls led out to the core: where guest code runs (not MSVC's build of the tests, which runs none). */
#define SYSCALLS (MG_ARCH_X86_64 && MG_GUEST_CODE)

static bool g_trap_svc;

void mg_patch_set_trap_svc(bool on) { g_trap_svc = on; }
bool mg_patch_svc_target(mg_addr pc, mg_addr *target) { (void)pc; (void)target; return false; }
bool mg_patch_tp_target(mg_addr pc, int *reg, mg_addr *target) { (void)pc; (void)reg; (void)target; return false; }

/*
 * A system call's piece of island at most: four instructions of fifteen bytes taken with it, the 30 bytes of the way to
 * the entry and the five of the way back.
 */
enum { SC_PIECE_MAX = 96 };

/*
 * Island space the loader keeps for a library (ld/mg_ld.c, island_estimate, 16 bytes for each one counted): the thread
 * pointer's reads need none, each keeping its place; a system call its piece, for every two bytes that may be one.
 */
u32 mg_patch_count_tp_reads(const u8 *bytes, size_t size)
{
#if SYSCALLS
    size_t i;
    u32 n = 0;
    for (i = 0; i + 1 < size; i++)
        if (bytes[i] == 0x0F && bytes[i + 1] == 0x05) n += SC_PIECE_MAX / 16;
    return n;
#else
    (void)bytes;
    (void)size;
    return 0;
#endif
}

/* A syscall the walk found: where, and the instructions it found before it. */
typedef struct sc_site {
    u8 *at;
    u8 len;                            /* (prefixes and all) */
    u8 *before[3];                     /* the instructions before it, the nearest first; NULL where the walk had none */
    u8 *lo, *hi;                       /* the stretch of code it is in */
    u8 targets[12];                    /* jumps of the library's to [at - 48, at + 48): a bit for each byte */
} sc_site;

/* One walk of a segment's code. */
typedef struct walk {
    mg_lib *lib;
    u8 *seg;
    size_t size;
    u32 tp_seg, tp_at, guard_seg, guard_at;
    bool have_slots;
    u32 odd, calls;
    sc_site *sites;
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

#if SYSCALLS
static void add_site(walk *w, u8 *p, const mg_x86_insn *in, u8 *const before[3], u8 *lo, u8 *hi)
{
    sc_site *s;
    if (w->nsites == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 16;
        w->sites = (sc_site *)mg_realloc(w->sites, sizeof(sc_site) * w->cap);
    }
    s = &w->sites[w->nsites++];
    memset(s, 0, sizeof(*s));
    s->at = p;
    s->len = in->len;
    memcpy(s->before, before, sizeof(s->before));
    s->lo = lo;
    s->hi = hi;
}
#endif

/* Walks [lo, hi) of the library's code, an instruction at a time: the thread pointer's reads rewritten, system calls found. */
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
#if SYSCALLS
            add_site(w, p, &in, before, lo, hi);
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
            } else {
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

#if SYSCALLS
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
#if defined(MG_CORE_ELF)
    void **tp = on_block ? (void **)mg_guest_tp_get() : NULL;
    bool made = tp && !tp[1];
    long r = mg_bionic_syscall(nr, args);
    if (made && tp[1]) {
        mg_gthread *g = (mg_gthread *)tp[1];
        g->stack_lo = (void *)block[SC_SAVED_DEALLOC];
        g->stack_hi = (void *)block[SC_SAVED_BASE];
    }
    return r;
#else
    (void)block;
    (void)on_block;
    return mg_bionic_syscall(nr, args);
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

static int site_order(const void *a, const void *b)
{
    const u8 *x = ((const sc_site *)a)->at, *y = ((const sc_site *)b)->at;
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
 * eip-relative address is cut to 32 bits.
 */
static bool movable(const mg_x86_insn *in)
{
    u8 op = in->op;
    if (rip_relative(in) && in->adsize) return false;
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

static bool jumped_into(const sc_site *s, const u8 *start, const u8 *end)
{
    const u8 *p;
    for (p = start + 1; p < end; p++) {
        mg_addr bit = (mg_addr)p + 48 - (mg_addr)s->at;
        if (bit < 96 && ((s->targets[bit / 8] >> (bit % 8)) & 1)) return true;
    }
    return false;
}

/*
 * The place the jump takes, [start, end): the syscall with whole instructions around it, five bytes or more, every one
 * movable, none before `free_from` (a site before it already took that), nothing jumping into it but at its start. The
 * fewest instructions, those before it first. False when there is none.
 */
static bool window(const sc_site *s, const u8 *free_from, u8 **start, u8 **end)
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
    for (n = 1; n <= nbefore + nafter && n <= 4; n++) {
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
static bool lead_out(walk *w, const sc_site *s, u8 *start, u8 *end)
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

/* Every system call the walk found led out, where it can be; said where it cannot. */
static void lead_out_all(walk *w)
{
    u32 i, done = 0, left = 0;
    u8 *free_from = NULL;
    qsort(w->sites, w->nsites, sizeof(sc_site), site_order);
    each_range(w, find_targets);
    xsave_setup();
    for (i = 0; i < w->nsites; i++) {
        sc_site *s = &w->sites[i];
        u8 *start, *end;
        if (w->have_slots && window(s, free_from, &start, &end) && lead_out(w, s, start, end)) {
            free_from = end;
            done++;
            continue;
        }
        if (left++ < 4)
            mg_warn("ld: %s+0x%x: a system call of its own is left as it is: %s", w->lib->name, (unsigned)(s->at - w->lib->base),
                    !w->have_slots ? "no slot for the thread pointer" : "no whole instructions around it to take its place");
    }
    if (done) mg_tls_syscall_stacks();
    w->lib->stats.svc_sites += done;
    mg_info("ld: %s: %u system calls of its own go to the core%s", w->lib->name, (unsigned)done, left ? "" : ", all of them");
    if (left) mg_warn("ld: %s: %u system calls of its own are left as they are: they would be the host's", w->lib->name, (unsigned)left);
}
#endif /* SYSCALLS */

void mg_patch_segment(mg_lib *lib, u8 *seg, size_t size)
{
    walk w;
    memset(&w, 0, sizeof(w));
    w.lib = lib;
    w.seg = seg;
    w.size = size;
    w.have_slots = mg_guest_tp_where(&w.tp_seg, &w.tp_at) && mg_guest_guard_where(&w.guard_seg, &w.guard_at);
    if (!w.have_slots) mg_err("ld: %s: the host has no place for the guest's thread pointer; its reads are left as they are", lib->name);
    each_range(&w, patch_range);
#if SYSCALLS
    if (w.nsites) lead_out_all(&w);
    free(w.sites);
#else
    if (w.calls) {
        lib->stats.svc_sites += w.calls;
        mg_warn("ld: %s: %u system calls of its own (%s); they are not intercepted on this processor yet", lib->name, (unsigned)w.calls,
                IS64 ? "syscall" : "int 0x80");
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
