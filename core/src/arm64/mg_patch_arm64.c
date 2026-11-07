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
 * System calls of the library's own, `svc #0` with the number in x8 and the arguments in x0..x5, are led out of it to the
 * core: each becomes a branch to a piece of the library's island,
 *
 *         stp   x16, x30, [sp, #-16]!       ; the two the piece uses, below the guest's sp
 *         ldr   x16, [x18, #<the guest's thread pointer's slot>]
 *                                           ; (Linux: mrs x16, tpidr_el0; ldr x16, [x16, #<its offset>])
 *         ldr   x30, 1f
 *         blr   x30                         ; mg_arm64_syscall_entry, below
 *         udf   #0xa6                       ; the trap, where the entry sends a thread it cannot make the call for
 *         ldp   x16, x30, [sp], #16         ; where it comes back otherwise
 *         b     <the svc + 4>
 *     1:  .quad mg_arm64_syscall_entry
 *
 * and the entry makes the call (mg_bionic_syscall, by the guest's number) with every register but x0 as the kernel
 * keeps them: x1..x30, the flags, q0..q31, FPSR and FPCR -- and x18, which nothing on the way writes. Not a trap, as it
 * was: on Windows a trap is an exception, laid on the guest's stack below its sp -- a CONTEXT of 0x390 bytes and the
 * record, RtlDispatchException's 0x2a0, the vectored handlers' frames and the core's fault handler's 5.6 KB, some 8 KB in
 * all for every system call of an app's own -- and dispatched by ntdll's code, which reads the TEB through x18 before any
 * handler of the core's runs (GitHub issue 4: Telegram's libtmessages made 119 such calls, and on a Snapdragon X Elite the
 * process ended in RtlDispatchException 20 ms into its first activity). The piece writes 16 bytes below the guest's sp,
 * the entry its frame of 848 below that on the thread's own stack (registers, flags, the vector registers whole: a
 * function of the core's keeps only the lower halves of v8..v15), or, where the guest's sp is on a stack of its own (a
 * goroutine's, of a few kilobytes), on the thread's stack for system calls (ld/mg_tls.c, MG_TCB_SYSCALL), which the TEB
 * is told while the call is made, as a fiber's switch tells it, so that Windows' code under the call finds its stack
 * where the TEB says. Its words the entry keeps on the frame of the call it makes, not in the thread's block: a signal's
 * handler that runs inside the call (a signal sent to the thread itself, one let through by the call) may make another
 * on the same thread. The entry is the core's code, so a thread in it is not interrupted for a signal
 * (mg_thread_interrupt waits until it is back in guest code); in the piece it may be, which keeps nothing below its sp.
 *
 * The trap stays where the piece cannot be: no island within a branch's reach (128 MB either way), no place for the
 * guest's thread pointer, or the flag trap-svc, which makes every site a trap that is not answered (UDF 0xa5, a fault
 * where the svc was). A site that is a trap is a UDF and recorded, as on 32-bit ARM: the fault handler (mg_core.c) does
 * the call and resumes after it. A signal restorer's svc (rt_sigreturn, 139 put in x8 just before it) is left alone.
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

/* A system call's piece of island (lead_out): 40 bytes, and 4 for the literal's alignment. */
enum { SC_PIECE = 48 };

/*
 * Island space the loader keeps for a library (ld/mg_ld.c, island_estimate, 16 bytes for each one counted): one thunk per
 * access of the thread pointer, where one is needed; a system call's piece for every svc 0.
 */
u32 mg_patch_count_tp_reads(const u8 *bytes, size_t size)
{
    size_t i;
    u32 n = 0;
    for (i = 0; i + 4 <= size; i += 4) {
        u32 w = mg_rd32(bytes + i);
        if ((w & ~31u) == MRS_TPIDR_EL0 || (w & ~31u) == MSR_TPIDR_EL0) n++;
        else if (w == SVC_0) n += SC_PIECE / 16;
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

/* A registered trap at `at` (its UDF written by the caller): mg_patch_svc_target answers it. NULL on allocation failure. */
static mg_svc_site *record_svc_site(mg_lib *lib, u8 *at)
{
    mg_svc_site *site = (mg_svc_site *)malloc(sizeof(*site));
    if (!site) return NULL;
    site->pc = (mg_addr)at;
    site->watched = false;
    site->tp_reg = -1;
    site->next = lib->svc_sites;
    lib->svc_sites = site;
    return site;
}

static void patch_svc_site(mg_lib *lib, u8 *at, bool trap)
{
    mg_svc_site *site = trap ? NULL : record_svc_site(lib, at);
    lib->stats.svc_sites++;
    /* Fail closed: never leave a host system call behind on allocation failure. */
    if (!site && !trap) mg_err("ld: %s: cannot record direct system call; it will fault", lib->name);
    mg_wr32(at, site ? SVC_UDF : SVC_UDF_TRAP);
}

#if MG_GUEST_CODE
/* ------------------------------------------------ system calls led out to the core */

/* The way out (below), the call it makes, and the thread's stack for system calls. */
void mg_arm64_syscall_entry(void);

/*
 * The thread's stack for system calls (MG_TCB_SYSCALL): a reservation whose lowest page is left out, a guard, and whose
 * top holds the block of words below; the stack is below the block. The first four are the entry's while it chooses a
 * stack, before it has a frame -- the core's code is not interrupted for a signal there, and makes no call -- and are
 * copied to the frame at once: a call made from a signal's handler that runs inside the call would write them again.
 */
enum { SC_STACK = 256 * 1024 };
enum {
    SC_X17, SC_BACK, SC_SP, SC_NZCV,   /* the guest's x17, the way back (the piece's trap), the piece's sp, the flags */
    SC_OWN_LO, SC_OWN_HI,              /* the thread's own stack, where the TEB does not say it (the tests' Linux) */
    SC_LIMIT,                          /* the lowest address of the stack for system calls, below it the guard */
    SC_BASE,                           /* its reservation */
    SC_WORDS = 16
};

/*
 * The entry's frame, in words: x0..x30 at their numbers (x18 read, never written), the guest's sp at the svc, its flags,
 * FPSR and FPCR, the way back, the block, whether the call is on the block's stack, the TEB's three words of its stack
 * while it says the block's (Windows), and q0..q31 at 16-byte alignment. 848 bytes.
 */
enum {
    F_SP = 31, F_NZCV, F_FPSR, F_FPCR, F_BACK, F_BLOCK, F_ON_BLOCK, F_TEB_BASE, F_TEB_LIMIT, F_TEB_DEALLOC,
    F_Q = 42, F_WORDS = F_Q + 64
};
_Static_assert(MG_TCB_SYSCALL * 8 == 120, "the entry reads the block from the guest's thread pointer + 120");
_Static_assert(F_SP * 8 == 248 && F_NZCV * 8 == 256 && F_FPSR * 8 == 264 && F_FPCR * 8 == 272 && F_BACK * 8 == 280 &&
               F_BLOCK * 8 == 288 && F_ON_BLOCK * 8 == 296 && F_TEB_BASE * 8 == 304 && F_TEB_DEALLOC * 8 == 320 && F_Q * 8 == 336 &&
               F_WORDS * 8 == 848, "the entry's frame, by its offsets");
_Static_assert(SC_X17 == 0 && SC_BACK == 1 && SC_SP == 2 && SC_NZCV == 3 && SC_OWN_LO == 4 && SC_OWN_HI == 5 && SC_LIMIT == 6 &&
               SC_BASE == 7, "the block's words, by the entry's offsets");

void *mg_patch_syscall_stack_new(void *own_lo, void *own_hi)
{
    u8 *base = (u8 *)mg_mem_reserve(SC_STACK);
    mg_addr *block;
    if (!base || !mg_mem_commit(base + MG_PAGE, SC_STACK - MG_PAGE, MG_PROT_RW)) {
        if (base) mg_mem_release(base, SC_STACK);
        mg_err("ld: no stack for a thread's system calls: its calls of a library's own are traps on the stack they are made on");
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
 * The call the entry makes, on the stack it chose (`on_block`: the block's): `frame` x0..x30 as at the svc and its sp
 * (F_SP), `back` the piece's trap, two instructions before its branch back to after the svc. Made as the trap's fault
 * handler makes it (mg_core.c, on_fault): mg_bionic_syscall with the number in x8, the arguments in x0..x5 and the svc's
 * own place as the site (madvise asks whose it is), what it gives -- -errno on failure, the guest's errno as it was -- for
 * x0; a signal's handler that a call runs on the thread (one sent to itself, one let through by its mask) runs inside it
 * as inside the trap's. On Windows the thread's record (mg_gthread) may be made by this call, with the bounds Windows says
 * of the thread's stack, which while the TEB says the block's stack are that one's: it is given the thread's own. Elsewhere
 * the block learns the thread's own stack, where it was not told it when it was made.
 */
long mg_arm64_syscall_call(const mg_addr *frame, mg_addr back, mg_addr *block, long on_block)
{
    u32 b = mg_rd32((const void *)(back + 8));
    mg_addr site = back + 8 + (mg_addr)((i64)((i32)(b << 6) >> 6) * 4) - 4;
    long args[6], result;
    int i;
#if defined(MG_CORE_ELF)
    void **tp = on_block ? (void **)mg_guest_tp_get() : NULL;
    bool made = tp && !tp[1];
#endif
    for (i = 0; i < 6; i++) args[i] = (long)frame[i];
    mg_core_count_svc(on_block != 0);
    result = mg_bionic_syscall((long)frame[8], args, site);
#if defined(MG_CORE_ELF)
    if (made && tp[1]) {
        mg_gthread *g = (mg_gthread *)tp[1];
        g->stack_lo = (void *)frame[F_TEB_DEALLOC];
        g->stack_hi = (void *)frame[F_TEB_BASE];
    }
#else
    if (on_block && !block[SC_OWN_HI]) {
        void *lo = NULL, *hi = NULL;
        mg_thread_stack_bounds(&lo, &hi);
        block[SC_OWN_LO] = (mg_addr)lo;
        block[SC_OWN_HI] = (mg_addr)hi;
    }
#endif
    return result;
}

/*
 * The entry: from a site's piece, x16 the guest's thread pointer, x30 the piece's trap (the way back is the instruction
 * after it), sp the guest's less the 16 bytes where the piece put x16 and x30, every other register as at the svc. On
 * Windows the thread's own stack is [DeallocationStack, StackBase) of its TEB (x18 + 0x1478, x18 + 8), and the TEB is
 * told the block's while the call is made there: StackBase the block, StackLimit (x18 + 0x10) its limit,
 * DeallocationStack its reservation. A thread whose sp is on the block's stack already -- a handler's call inside a call
 * made there -- stays on it, as on its own. A thread with no block (it could not be made) goes back to the piece's trap
 * with every register but x16 as at the svc.
 */
#if defined(MG_CORE_ELF)
#define A64_OWN_STACK "ldr x30, [x18, #0x1478]\n cmp x17, x30\n b.lo 1f\n ldr x30, [x18, #8]\n cmp x17, x30\n b.hs 1f\n"
#define A64_TEB_IN                                                                                                     \
    "cbz x30, 5f\n"                                                                                                    \
    "ldr x0, [x18, #8]\n ldr x1, [x18, #16]\n stp x0, x1, [sp, #304]\n ldr x0, [x18, #0x1478]\n str x0, [sp, #320]\n"     \
    "str x16, [x18, #8]\n ldr x0, [x16, #48]\n str x0, [x18, #16]\n ldr x0, [x16, #56]\n str x0, [x18, #0x1478]\n"       \
    "5:\n"
#define A64_TEB_OUT                                                                                                    \
    "ldr x1, [sp, #296]\n cbz x1, 6f\n"                                                                                \
    "ldp x2, x3, [sp, #304]\n str x2, [x18, #8]\n str x3, [x18, #16]\n ldr x2, [sp, #320]\n str x2, [x18, #0x1478]\n"    \
    "6:\n"
#else
#define A64_OWN_STACK "ldr x30, [x16, #32]\n cmp x17, x30\n b.lo 1f\n ldr x30, [x16, #40]\n cmp x17, x30\n b.hs 1f\n"
#define A64_TEB_IN ""
#define A64_TEB_OUT ""
#endif
__asm__(".text\n"
        ".p2align 4\n"
        ".globl mg_arm64_syscall_entry\n"
        ".hidden mg_arm64_syscall_entry\n"
        ".type mg_arm64_syscall_entry, %function\n"
        "mg_arm64_syscall_entry:\n"
        "cbz x16, 9f\n"
        "ldr x16, [x16, #120]\n"                    /* the thread's block, its slot MG_TCB_SYSCALL */
        "cbz x16, 9f\n"
        "stp x17, x30, [x16, #0]\n"                 /* SC_X17, SC_BACK: a second register to work with, and the way back */
        "mrs x17, nzcv\n"
        "str x17, [x16, #24]\n"                     /* SC_NZCV: the flags, before anything here sets them */
        "mov x17, sp\n"
        "str x17, [x16, #16]\n"                     /* SC_SP */
        A64_OWN_STACK                               /* the thread's own: the frame below the guest's sp */
        "b 2f\n"
        "1:\n"
        "ldr x30, [x16, #56]\n"                     /* SC_BASE: on the block's stack already, it stays there */
        "cmp x17, x30\n"
        "b.lo 3f\n"
        "cmp x17, x16\n"
        "b.lo 2f\n"
        "3:\n"
        "mov sp, x16\n"                             /* another (a goroutine's): the block's, below its words */
        "mov x30, #1\n"
        "b 4f\n"
        "2:\n"
        "mov x30, #0\n"
        "4:\n"
        "sub sp, sp, #848\n"
        "stp x0, x1, [sp, #0]\n"
        "stp x2, x3, [sp, #16]\n"
        "stp x4, x5, [sp, #32]\n"
        "stp x6, x7, [sp, #48]\n"
        "stp x8, x9, [sp, #64]\n"
        "stp x10, x11, [sp, #80]\n"
        "stp x12, x13, [sp, #96]\n"
        "stp x14, x15, [sp, #112]\n"
        "ldr x0, [x17]\n"                           /* x16, where the piece put it */
        "ldr x1, [x16, #0]\n"                       /* x17 */
        "stp x0, x1, [sp, #128]\n"
        "mov x0, x18\n"                             /* (read, for the frame to be whole: x18 is never written) */
        "stp x0, x19, [sp, #144]\n"
        "stp x20, x21, [sp, #160]\n"
        "stp x22, x23, [sp, #176]\n"
        "stp x24, x25, [sp, #192]\n"
        "stp x26, x27, [sp, #208]\n"
        "stp x28, x29, [sp, #224]\n"
        "ldr x0, [x17, #8]\n"                       /* x30, where the piece put it */
        "add x1, x17, #16\n"                        /* the guest's sp at the svc */
        "stp x0, x1, [sp, #240]\n"
        "ldr x0, [x16, #24]\n"
        "mrs x1, fpsr\n"
        "stp x0, x1, [sp, #256]\n"                  /* F_NZCV, F_FPSR */
        "mrs x0, fpcr\n"
        "ldr x1, [x16, #8]\n"
        "stp x0, x1, [sp, #272]\n"                  /* F_FPCR, F_BACK */
        "stp x16, x30, [sp, #288]\n"                /* F_BLOCK, F_ON_BLOCK */
        "stp q0, q1, [sp, #336]\n"
        "stp q2, q3, [sp, #368]\n"
        "stp q4, q5, [sp, #400]\n"
        "stp q6, q7, [sp, #432]\n"
        "stp q8, q9, [sp, #464]\n"
        "stp q10, q11, [sp, #496]\n"
        "stp q12, q13, [sp, #528]\n"
        "stp q14, q15, [sp, #560]\n"
        "stp q16, q17, [sp, #592]\n"
        "stp q18, q19, [sp, #624]\n"
        "stp q20, q21, [sp, #656]\n"
        "stp q22, q23, [sp, #688]\n"
        "stp q24, q25, [sp, #720]\n"
        "stp q26, q27, [sp, #752]\n"
        "stp q28, q29, [sp, #784]\n"
        "stp q30, q31, [sp, #816]\n"
        A64_TEB_IN
        "mov x0, sp\n"
        "ldr x1, [sp, #280]\n"
        "mov x2, x16\n"
        "mov x3, x30\n"
        "bl mg_arm64_syscall_call\n"
        "str x0, [sp, #0]\n"                        /* the result, x0's */
        A64_TEB_OUT
        "ldp q0, q1, [sp, #336]\n"
        "ldp q2, q3, [sp, #368]\n"
        "ldp q4, q5, [sp, #400]\n"
        "ldp q6, q7, [sp, #432]\n"
        "ldp q8, q9, [sp, #464]\n"
        "ldp q10, q11, [sp, #496]\n"
        "ldp q12, q13, [sp, #528]\n"
        "ldp q14, q15, [sp, #560]\n"
        "ldp q16, q17, [sp, #592]\n"
        "ldp q18, q19, [sp, #624]\n"
        "ldp q20, q21, [sp, #656]\n"
        "ldp q22, q23, [sp, #688]\n"
        "ldp q24, q25, [sp, #720]\n"
        "ldp q26, q27, [sp, #752]\n"
        "ldp q28, q29, [sp, #784]\n"
        "ldp q30, q31, [sp, #816]\n"
        "ldp x0, x1, [sp, #264]\n"
        "msr fpsr, x0\n"
        "msr fpcr, x1\n"
        "ldr x0, [sp, #256]\n"
        "msr nzcv, x0\n"                            /* nothing below sets the flags */
        "ldp x2, x3, [sp, #16]\n"
        "ldp x4, x5, [sp, #32]\n"
        "ldp x6, x7, [sp, #48]\n"
        "ldp x8, x9, [sp, #64]\n"
        "ldp x10, x11, [sp, #80]\n"
        "ldp x12, x13, [sp, #96]\n"
        "ldp x14, x15, [sp, #112]\n"
        "ldr x17, [sp, #136]\n"
        "ldr x19, [sp, #152]\n"
        "ldp x20, x21, [sp, #160]\n"
        "ldp x22, x23, [sp, #176]\n"
        "ldp x24, x25, [sp, #192]\n"
        "ldp x26, x27, [sp, #208]\n"
        "ldp x28, x29, [sp, #224]\n"
        "ldr x30, [sp, #280]\n"
        "add x30, x30, #4\n"                        /* the way back: past the piece's trap */
        "ldr x16, [sp, #248]\n"
        "sub x16, x16, #16\n"                       /* the piece's sp; x16 and x30 the piece takes back itself */
        "ldp x0, x1, [sp, #0]\n"
        "mov sp, x16\n"
        "ret\n"
        "9:\n"
        "ret\n"                                     /* no block: the piece's trap */
        ".size mg_arm64_syscall_entry, .-mg_arm64_syscall_entry\n");

/*
 * The svc at `at` led out to the core (above): its piece of island, the piece's trap recorded, and the branch to it in its
 * place, written last. False when it cannot be (no island within a branch's reach): the site is then made a trap.
 */
static bool lead_out(mg_lib *lib, u8 *at, u32 where, u32 offset)
{
    u8 *mem = mg_island_alloc(lib, (mg_addr)at, SC_PIECE, (1u << 27) - SC_PIECE), *piece, *q, *trap, *literal;
    u64 entry = (u64)(mg_addr)mg_arm64_syscall_entry;
    mg_svc_site *site;
    if (!mem) return false;
    /* (The literal on 8 bytes' alignment, at the piece's 32nd byte.) */
    piece = (u8 *)MG_ALIGN_UP((mg_addr)mem, (mg_addr)8);
    literal = piece + 32;
    q = piece;
    mg_wr32(q, 0xA9BF7BF0u);                                         /* stp x16, x30, [sp, #-16]! */
    q += 4;
    if (where == 18) {
        mg_wr32(q, ldr_x(16, 18, offset));                           /* ldr x16, [x18, #offset] */
        q += 4;
    } else {
        mg_wr32(q, MRS_TPIDR_EL0 | 16u);                             /* mrs x16, tpidr_el0 */
        mg_wr32(q + 4, ldr_x(16, 16, offset));                       /* ldr x16, [x16, #offset] */
        q += 8;
    }
    mg_wr32(q, 0x58000000u | ((u32)((literal - q) / 4) << 5) | 30u); /* ldr x30, <the literal> */
    mg_wr32(q + 4, 0xD63F03C0u);                                     /* blr x30 */
    trap = q + 8;
    mg_wr32(trap, SVC_UDF);
    mg_wr32(trap + 4, 0xA8C17BF0u);                                  /* ldp x16, x30, [sp], #16 */
    if (!a64_b(trap + 8, (mg_addr)(trap + 8), (mg_addr)(at + 4))) return false;
    for (q = trap + 12; q < literal; q += 4) mg_wr32(q, 0xD503201Fu);   /* nop */
    memcpy(literal, &entry, 8);
    site = record_svc_site(lib, trap);
    if (!site) return false;
    if (!a64_b(at, (mg_addr)at, (mg_addr)piece)) {
        lib->svc_sites = site->next;
        free(site);
        return false;
    }
    lib->stats.svc_sites++;
    lib->stats.svc_led_out++;
    return true;
}
#endif /* MG_GUEST_CODE */

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
            (*svc)++;
#if MG_GUEST_CODE
            if (!trap && have && lead_out(lib, p, where, offset)) continue;
#endif
            patch_svc_site(lib, p, trap);
        }
    }
}

void mg_patch_segment(mg_lib *lib, u8 *seg, size_t size)
{
    u32 where = 0, offset = 0, svc = 0, failed = 0, led = lib->stats.svc_led_out;
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
    led = lib->stats.svc_led_out - led;
#if MG_GUEST_CODE
    /* Every thread a stack for them from now on: those there already, and each one made later (ld/mg_tls.c). */
    if (led) mg_tls_syscall_stacks();
#endif
    if (trap && svc) mg_info("ld: %s: %u system calls of its own made traps (trap-svc: not done)", lib->name, (unsigned)svc);
    else if (svc && led == svc) mg_info("ld: %s: %u system calls of its own led out to the core, 0 left traps", lib->name, (unsigned)svc);
    else if (svc)
        mg_warn("ld: %s: %u system calls of its own led out to the core, %u left traps (%s)", lib->name, (unsigned)led, (unsigned)(svc - led),
                have ? "no island within a branch's reach" : "no place for the guest's thread pointer");
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
