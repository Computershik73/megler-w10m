/*
 * What is rewritten in a library's code before it is made executable.
 *
 * Android's 32-bit ARM code runs on the phone's CPU as it is, with two
 * exceptions that every library has:
 *
 * 1. Reads of the thread pointer. bionic keeps its thread pointer in TPIDRURO
 *    and compiled code fetches it inline: `mrc p15, 0, rX, c13, c0, 3`. On the
 *    host that register is the operating system's. Each such instruction is
 *    replaced by a branch to a few instructions in the library's island that
 *    load the guest's thread pointer from host thread-local storage into the
 *    same register, and branch back. No other register or flag changes, which
 *    is what the original instruction promised.
 *
 * 2. PLT entries. A linker writes them in ARM mode even in a library that is
 *    otherwise Thumb, and Thumb callers reach them with `blx`. Windows on ARM
 *    runs Thumb code only: a thread that is interrupted in ARM state comes
 *    back in Thumb state (the kernel sets the T bit when it rebuilds a
 *    thread's context), and whatever it was executing is then decoded as
 *    Thumb. So an entry is rewritten into a single ARM instruction and a
 *    literal:
 *
 *        ldr pc, [pc, #-4]      ; E51FF004
 *        .word thunk | 1        ; Thumb code in the island: jump through the GOT slot
 *
 *    One instruction leaves no place to be interrupted "in the middle" of ARM
 *    code -- only before it. And these four bytes read as Thumb are
 *    `F004 E51F`, a BLX with its low bit set, which the architecture defines
 *    as UNDEFINED: a thread resumed there in Thumb state takes an
 *    illegal-instruction exception at once, deterministically, and the fault
 *    handler (mg_ld_plt_target) finishes the jump. The entry works in either
 *    state.
 *
 * ARM-mode function bodies are not rewritten here; docs/ARCHITECTURE.md, "ARM
 * mode", says what happens to them.
 */
#include "../mg_core.h"
#include "../bionic/mg_bionic.h"
#include "../host/mg_host.h"
#include "../ld/mg_ld_internal.h"
#include "mg_stubs.h"

/* (32-bit ARM's alone: x86/mg_patch_x86.c is the x86 processors'.) */
#if MG_ARCH_ARM32

/* ---------------------------------------------------------------- encoders */

/* Thumb-2 B.W (encoding T4): 16 MB either way. `from` is where the instruction is, `to` a Thumb address without its low bit. */
static bool t32_b_w(u8 *at, mg_addr from, mg_addr to)
{
    i32 off = (i32)(to - (from + 4));
    u32 s, i1, i2, j1, j2;
    if (off < -(1 << 24) || off >= (1 << 24) || (off & 1)) return false;
    s = ((u32)off >> 24) & 1;
    i1 = ((u32)off >> 23) & 1;
    i2 = ((u32)off >> 22) & 1;
    j1 = (~i1 ^ s) & 1;
    j2 = (~i2 ^ s) & 1;
    mg_wr16(at, (u16)(0xF000 | (s << 10) | (((u32)off >> 12) & 0x3FF)));
    mg_wr16(at + 2, (u16)(0x9000 | (j1 << 13) | (j2 << 11) | (((u32)off >> 1) & 0x7FF)));
    return true;
}

/* ARM B: 32 MB either way. */
static bool a32_b(u8 *at, mg_addr from, mg_addr to)
{
    i32 off = (i32)(to - (from + 8));
    if (off < -(1 << 25) || off >= (1 << 25) || (off & 3)) return false;
    mg_wr32(at, 0xEA000000u | (((u32)off >> 2) & 0x00FFFFFFu));
    return true;
}

/* -------------------------------------------------- thread-pointer reads */

static bool is_t32_tp_read(const u8 *p, int *reg)
{
    u16 hw2;
    if (mg_rd16(p) != 0xEE1D) return false;
    hw2 = mg_rd16(p + 2);
    if ((hw2 & 0x0FFF) != 0x0F70) return false;
    *reg = hw2 >> 12;
    return *reg != 13 && *reg != 15;
}

static bool is_a32_tp_read(const u8 *p, int *reg)
{
    u32 w = mg_rd32(p);
    if ((w & 0xFFFF0FFFu) != 0xEE1D0F70u) return false;
    *reg = (int)((w >> 12) & 15);
    return *reg != 13 && *reg != 15;
}

/*
 * Island space the loader keeps for a library (ld/mg_ld.c, island_estimate, 16 bytes for each one counted): a thread
 * pointer's read its 12 bytes, and an ARM svc 0 the 32 of its way out, whether or not the library is one whose calls are
 * led out (that is not known yet).
 */
u32 mg_patch_count_tp_reads(const u8 *bytes, size_t size)
{
    u32 n = 0;
    size_t i;
    int reg;
    for (i = 0; i + 4 <= size; i += 2) {
        if (is_t32_tp_read(bytes + i, &reg)) n++;
        else if (!(i & 3) && is_a32_tp_read(bytes + i, &reg)) n++;
        else if (!(i & 3) && mg_rd32(bytes + i) == 0xEF000000u) n += 2;
    }
    return n;
}

/* Whether [at, at + n) lies in one of the library's executable sections (mg_lib's code); anywhere, when none are known. */
static bool in_code(const mg_lib *lib, const u8 *at, size_t n)
{
    u32 v = (u32)(at - lib->base);
    int k;
    if (!lib->ncode) return true;
    for (k = 0; k < lib->ncode; k++)
        if (v >= lib->code[k][0] && v + n <= lib->code[k][1]) return true;
    return false;
}

/* One read made a branch to the island, which reads the guest's thread pointer: false when it could not be. */
static bool patch_tp_read(mg_lib *lib, u8 *site, bool thumb, int reg)
{
    u32 opc2 = 0, offset = 0;
    u8 *tr;
    if (!mg_guest_tp_where(&opc2, &offset)) return false;
    tr = mg_island_alloc(lib, (mg_addr)site, 12, thumb ? (1u << 24) - 16 : (1u << 25) - 16);
    if (!tr) return false;
    if (thumb) {
        /* mrc p15, 0, reg, c13, c0, opc2 ; ldr.w reg, [reg, #offset] ; b.w back */
        mg_wr16(tr + 0, 0xEE1D);
        mg_wr16(tr + 2, (u16)((reg << 12) | 0x0F10 | (opc2 << 5)));
        mg_wr16(tr + 4, (u16)(0xF8D0 | reg));
        mg_wr16(tr + 6, (u16)((reg << 12) | offset));
        /* The site is untouched when its own branch could not be encoded (it is written last). */
        return t32_b_w(tr + 8, (mg_addr)tr + 8, (mg_addr)site + 4) && t32_b_w(site, (mg_addr)site, (mg_addr)tr);
    }
    mg_wr32(tr + 0, 0xEE1D0F10u | ((u32)reg << 12) | (opc2 << 5));
    mg_wr32(tr + 4, 0xE5900000u | ((u32)reg << 16) | ((u32)reg << 12) | offset);
    return a32_b(tr + 8, (mg_addr)tr + 8, (mg_addr)site + 4) && a32_b(site, (mg_addr)site, (mg_addr)tr);
}

static void patch_tp_reads(mg_lib *lib, u8 *seg, size_t size)
{
    size_t i;
    int reg;
    for (i = 0; i + 4 <= size; i += 2) {
        u8 *site = seg + i;
        bool thumb = is_t32_tp_read(site, &reg);
        if (!thumb && ((i & 3) || !is_a32_tp_read(site, &reg))) continue;
        if (!in_code(lib, site, 4)) continue;
        if (!patch_tp_read(lib, site, thumb, reg)) {
            lib->stats.tp_reads_failed++;
            continue;
        }
        lib->stats.tp_reads++;
        i += 2;
    }
}

/* ------------------------------------------------------------ PLT entries */

static int by_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

static bool is_jump_slot(const u32 *slots, u32 n, u32 vaddr)
{
    u32 lo = 0, hi = n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (slots[mid] == vaddr) return true;
        if (slots[mid] < vaddr) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

static u32 arm_imm(u32 w)
{
    u32 v = w & 0xFF, r = ((w >> 8) & 15) * 2;
    return r ? (v >> r) | (v << (32 - r)) : v;
}

/* The GOT slot an ARM PLT entry at `p` jumps through, by the shapes linkers write; false when `p` is not one. */
static bool plt_entry_slot(const u8 *p, size_t left, mg_addr *slot)
{
    u32 w0, w1, w2;
    if (left < 12) return false;
    w0 = mg_rd32(p);
    w1 = mg_rd32(p + 4);
    w2 = mg_rd32(p + 8);
    /* add ip, pc, #a ; add ip, ip, #b ; ldr pc, [ip, #c]! */
    if ((w0 & 0xFFFFF000u) == 0xE28FC000u && (w1 & 0xFFFFF000u) == 0xE28CC000u && (w2 & 0xFFFFF000u) == 0xE5BCF000u) {
        *slot = (mg_addr)p + 8 + arm_imm(w0) + arm_imm(w1) + (w2 & 0xFFF);
        return true;
    }
    /* ldr ip, [pc, #4] ; add ip, ip, pc (or add ip, pc, ip) ; ldr pc, [ip] ; .word offset */
    if (left >= 16 && w0 == 0xE59FC004u && (w1 == 0xE08CC00Fu || w1 == 0xE08FC00Cu) && w2 == 0xE59CF000u) {
        *slot = (mg_addr)p + 12 + mg_rd32(p + 12);
        return true;
    }
    return false;
}

static void patch_plt(mg_lib *lib, u8 *seg, size_t size)
{
    u32 n = lib->pltrelsz / sizeof(Elf32_Rel), i, count = 0;
    u32 *slots;
    size_t off;
    char flag[96];
    if (!n) return;
    /* The flags no-plt-rewrite and no-plt-rewrite-<library>: its PLT left as the linker wrote it (ARM), for finding out
       whether a library minds its entries rewritten. (One left as the file has it, below, has none rewritten.) */
    snprintf(flag, sizeof(flag), "no-plt-rewrite-%s", lib->name);
    if (mg_flag("no-plt-rewrite") || mg_flag(flag)) return;
    slots = (u32 *)mg_alloc(sizeof(u32) * n);
    for (i = 0; i < n; i++)
        if (ELF_R_TYPE(lib->jmprel[i].r_info) == R_ARM_JUMP_SLOT) slots[count++] = lib->jmprel[i].r_offset;
    qsort(slots, count, sizeof(u32), by_u32);
    off = (size_t)(MG_ALIGN_UP((mg_addr)seg, 4) - (mg_addr)seg);
    for (; off + 12 <= size; off += 4) {
        u8 *p = seg + off;
        mg_addr slot;
        u8 *th;
        if (!plt_entry_slot(p, size - off, &slot)) continue;
        /* Only an entry whose slot a JUMP_SLOT relocation fills: three words that merely look like one are left alone. */
        if (slot < (mg_addr)lib->base || !is_jump_slot(slots, count, (u32)(slot - (mg_addr)lib->base))) continue;
        th = mg_island_alloc(lib, 0, 12, 0);
        if (!th) {
            mg_err("ld: %s: island full; a PLT entry stays in ARM mode", lib->name);
            break;
        }
        mg_wr16(th + 0, 0xF8DF);       /* ldr.w ip, [pc, #4] */
        mg_wr16(th + 2, 0xC004);
        mg_wr16(th + 4, 0xF8DC);       /* ldr.w pc, [ip] */
        mg_wr16(th + 6, 0xF000);
        mg_wr32(th + 8, (u32)slot);
        mg_wr32(p, 0xE51FF004u);       /* ldr pc, [pc, #-4] */
        mg_wr32(p + 4, (u32)(mg_addr)th | 1u);
        lib->stats.plt_entries++;
        off += 8;
    }
    free(slots);
}

/* ---------------------------------------------------------- system calls */

/*
 * Never execute a recognised Linux SVC on the host. Replace it with UDF and
 * record its exact address so the fault handler can dispatch r7/r0..r5 and
 * resume after it. Windows lays the trap's records below the guest's sp,
 * aligned even when the guest used push {r7}, and the handler runs below them:
 * room enough on a thread's own stack, none on a goroutine's, whence a library
 * of Go's has its ARM svc led out to the core instead (lead_out_svc, below).
 *
 * Executable segments also contain literal pools and Thumb-2 second halves.
 * Keep the Thumb r7-producer check instead of replacing every DF00 halfword.
 * In particular Chromium's cacheflush uses a literal load, not movw, and
 * compiler-rt's __clear_cache (Facebook's libdexload.so) ends with movt:
 * movs r7,#2; movs r2,#0; movt r7,#15; svc 0. Left unpatched, that svc went
 * to Windows, failed, and __clear_cache's assertion aborted the app.
 *
 * The number may be set a few instructions back: Chromium's own syscall()
 * (mov r7, r0; ldr r0, [r6]; svc 0; pop {r7, pc}), bionic's (mov r7, r0, three
 * moves, ldmia ip, {r3-r6}; svc 0). Up to eight halfwords before the svc are
 * looked at, back to an instruction after which straight-line code ends
 * (mov pc, rN; bx rN; b; pop {..., pc}): an svc right behind one is not reached
 * by falling through to it. TikTok Lite's libmetasec_ov.so has 554 such in its
 * anti-disassembly stubs (mov pc, r1; mov r0, pc; svc 0; pop {r0-r3, r7}),
 * which jump over them, and it sums code that holds some of them: one of its
 * init functions sends the thread to 0x4000 with sp 0x2b, and the process
 * ends, when a byte of that code has changed. They stay as the file has them.
 *
 * Only the library's executable sections are looked at, when its section
 * headers say which they are (mg_lib's code): read-only data shares the
 * segment, and a table in libmetasec_ov.so's .rodata holds 0xEF000000, the
 * word of an ARM svc 0, which was patched into a UDF before.
 * trap-svc retains its diagnostic meaning: stop at the site without dispatch.
 */
static volatile i32 g_trap_svc = -1;

void mg_patch_set_trap_svc(bool on) { g_trap_svc = on ? 1 : 0; }

/* A registered svc trap at `at` (its UDF written by the caller): mg_patch_svc_target answers it. NULL on allocation failure. */
static mg_svc_site *record_svc_site(mg_lib *lib, u8 *at, bool thumb)
{
    mg_svc_site *site = (mg_svc_site *)malloc(sizeof(*site));
    if (!site) return NULL;
    site->pc = (mg_addr)at | (thumb ? 1u : 0u);
    site->watched = false;
    site->tp_reg = -1;
    site->next = lib->svc_sites;
    lib->svc_sites = site;
    return site;
}

static void patch_svc_site(mg_lib *lib, u8 *at, bool thumb, bool trap)
{
    mg_svc_site *site = trap ? NULL : record_svc_site(lib, at, thumb);
    lib->stats.svc_sites++;
    /* Fail closed: never leave a host system call behind on allocation failure. */
    if (!site && !trap) mg_err("ld: %s: cannot record direct system call; it will fault", lib->name);
    if (thumb) mg_wr16(at, site ? 0xDEA6 : 0xDEA5);
    else mg_wr32(at, site ? 0xE7F00AF6u : 0xE7F00AF5u);
}

/*
 * An ARM svc 0 of a library of Go's, led out of its code to the core rather than made a trap. A trap's handler runs on
 * the stack the svc was made on, below its sp: Windows lays the exception's records there (it has no other stack for
 * them) and the core's fault handler its frame of 5.6 KB, then the call's own. Go's runtime makes its calls by number on
 * its goroutines' stacks, of two to eight kilobytes, cut from its heap: the trap's handler wrote far below such a stack,
 * into the goroutine stacks and heap beneath it, or ran into memory that is not there, and the process ended with no word
 * of the core's. MCO Advanced's libgojni.so (Go 1.27.2, its Tailscale) died so about a tenth of a second into its runtime's
 * start, twice (package 669): under qemu-arm its first seconds make 53 calls on such stacks, with 1448 to 7164 bytes
 * below sp.
 *
 * The site becomes a branch to a piece of the library's island,
 *
 *         push  {r12, lr}
 *         ldr   r12, [pc, #16]          ; mg_stub_svc_entry (arm/mg_stubs.S), Thumb
 *         blx   r12
 *         b     1f                      ; its way back
 *         udf                           ; the trap, where a thread with no stack for the call is sent back to
 *     1:  pop   {r12, lr}
 *         b     <the site + 4>
 *         .word mg_stub_svc_entry + 1
 *
 * which writes eight bytes below the guest's sp, as the kernel would write none; the entry makes the call on the thread's
 * stack for system calls (ld/mg_tls.c, MG_TCB_SYSCALL), or on its own stack where the guest's sp already is. Every register
 * but r0, the flags and the VFP registers are as they were after it, as the kernel keeps them. Only Go's libraries'
 * (mg_ld_lib_is_go), whose code is all ARM: elsewhere an svc runs on the thread's own stack, and stays the trap it was.
 * False where it cannot be done (no island within a branch's reach, no entry): the site is then made a trap.
 */
static bool lead_out_svc(mg_lib *lib, u8 *at)
{
    void *entry = mg_stub(MG_STUB_SVC_ENTRY);
    u8 *piece;
    if (!entry) return false;
    piece = mg_island_alloc(lib, (mg_addr)at, 32, (1u << 25) - 64);
    if (!piece) return false;
    mg_wr32(piece + 0, 0xE92D5000u);           /* push {r12, lr} */
    mg_wr32(piece + 4, 0xE59FC010u);           /* ldr r12, [pc, #16] */
    mg_wr32(piece + 8, 0xE12FFF3Cu);           /* blx r12 */
    mg_wr32(piece + 12, 0xEA000000u);          /* b piece + 20 */
    mg_wr32(piece + 16, 0xE7F00AF6u);          /* the registered trap */
    mg_wr32(piece + 20, 0xE8BD5000u);          /* pop {r12, lr} */
    mg_wr32(piece + 28, (u32)(mg_addr)entry);
    if (!a32_b(piece + 24, (mg_addr)piece + 24, (mg_addr)at + 4) || !record_svc_site(lib, piece + 16, false)) return false;
    /* (The site last: untouched until the piece is whole, made a trap by the caller otherwise.) */
    if (!a32_b(at, (mg_addr)at, (mg_addr)piece)) {
        mg_svc_site *site = lib->svc_sites;
        lib->svc_sites = site->next;
        free(site);
        return false;
    }
    lib->stats.svc_sites++;
    lib->stats.svc_led_out++;
    return true;
}

/*
 * A thread's stack for system calls led out (MG_TCB_SYSCALL, mg_stubs.h's MG_SC_* words at its top, the stack below
 * them): 128 KB reserved and committed, its lowest page left out, as a guard.
 */
enum { SC_STACK = 128 * 1024 };

void *mg_patch_syscall_stack_new(void *own_lo, void *own_hi)
{
    u8 *base = (u8 *)mg_mem_reserve(SC_STACK);
    mg_addr *block;
    if (!base || !mg_mem_commit(base + MG_PAGE, SC_STACK - MG_PAGE, MG_PROT_RW)) {
        if (base) mg_mem_release(base, SC_STACK);
        mg_err("ld: no stack for a thread's system calls: its calls of a library's own are traps on the stack they are made on");
        return NULL;
    }
    block = (mg_addr *)(base + SC_STACK - MG_SC_WORDS * sizeof(mg_addr));
    block[MG_SC_OWN_LO] = (mg_addr)own_lo;
    block[MG_SC_OWN_HI] = (mg_addr)own_hi;
    block[MG_SC_LIMIT] = (mg_addr)(base + MG_PAGE);
    block[MG_SC_BASE] = (mg_addr)base;
    return block;
}

void mg_patch_syscall_stack_free(void *block) { mg_mem_release((u8 *)block + MG_SC_WORDS * sizeof(mg_addr) - SC_STACK, SC_STACK); }

/* The first halfword of a 32-bit Thumb instruction. */
static bool is_t32_first(u16 h) { return (h >> 11) >= 0x1D; }
/* 16-bit instructions that set r7: movs r7,#n; ldr r7,[pc,#n]; mov r7,rN. */
static bool t16_sets_r7(u16 h) { return (h & 0xFF00) == 0x2700 || (h & 0xFF00) == 0x4F00 || (h & 0xFF87) == 0x4607; }
/* 32-bit ones, f the first halfword and s the second: mov.w/movw/movt r7,#n; ldr.w r7,[pc,+/-n]. */
static bool t32_sets_r7(u16 f, u16 s)
{
    return (((f & 0xFBF0) == 0xF040 || (f & 0xFBF0) == 0xF240 || (f & 0xFBF0) == 0xF2C0) && (s & 0x8F00) == 0x0700)
        || ((f == 0xF8DF || f == 0xF85F) && (s & 0xF000) == 0x7000);
}
/* 16-bit instructions after which straight-line code ends: mov pc, rN; bx rN; b; pop {..., pc}. */
static bool t16_ends_flow(u16 h)
{
    return (h & 0xFF87) == 0x4687 || (h & 0xFF87) == 0x4700 || (h & 0xF800) == 0xE000 || (h & 0xFF00) == 0xBD00;
}

/*
 * Whether the Thumb svc at seg + i makes a system call: r7 set within the eight halfwords before it, and no instruction
 * between after which straight-line code ends. A first half of a 32-bit instruction is never taken for one that ends
 * it (they are 0xE800 and up); a second half may be, which leaves such an svc as it is.
 */
static bool t_svc_numbered(const u8 *seg, size_t i)
{
    size_t k;
    if (is_t32_first(mg_rd16(seg + i - 2))) return false;   /* DF00 the second half of a 32-bit instruction */
    for (k = 2; k <= 16 && k <= i; k += 2) {
        u16 s = mg_rd16(seg + i - k), f = k + 2 <= i ? mg_rd16(seg + i - k - 2) : 0;
        if (t16_ends_flow(s)) return false;
        if (t16_sets_r7(s) || t32_sets_r7(f, s)) return true;
    }
    return false;
}

static void patch_svc(mg_lib *lib, u8 *seg, size_t size)
{
    size_t i;
    bool trap;
    u32 led_out = lib->stats.svc_led_out;
    if (g_trap_svc < 0) g_trap_svc = mg_flag("trap-svc") ? 1 : 0;
    trap = g_trap_svc > 0;
    for (i = 2; i + 2 <= size; i += 2)
        if (mg_rd16(seg + i) == 0xDF00 && t_svc_numbered(seg, i) && in_code(lib, seg + i, 2)) patch_svc_site(lib, seg + i, true, trap);
    for (i = (size_t)(MG_ALIGN_UP((mg_addr)seg, 4) - (mg_addr)seg); i + 4 <= size; i += 4)
        if (mg_rd32(seg + i) == 0xEF000000u && in_code(lib, seg + i, 4) && !(!trap && lib->go && lead_out_svc(lib, seg + i)))
            patch_svc_site(lib, seg + i, false, trap);
    /* Every thread a stack for them from now on: those there already, and each one made later (ld/mg_tls.c). */
    if (lib->stats.svc_led_out != led_out) mg_tls_syscall_stacks();
}

bool mg_patch_svc_target(mg_addr pc, mg_addr *target)
{
    mg_lib *lib = mg_ld_lib_of((const void *)pc);
    mg_svc_site *site;
    if (!lib) return false;
    for (site = lib->svc_sites; site; site = site->next) {
        bool thumb = (site->pc & 1) != 0;
        if ((site->pc & ~(mg_addr)1) != pc || site->tp_reg >= 0) continue;
        /* A watched one is as the file has it, a patched one the registered UDF. */
        if (site->watched ? (thumb ? mg_rd16((const void *)pc) != 0xDF00 : mg_rd32((const void *)pc) != 0xEF000000u)
                          : (thumb ? mg_rd16((const void *)pc) != 0xDEA6 : mg_rd32((const void *)pc) != 0xE7F00AF6u))
            return false;
        *target = (pc + (thumb ? 2 : 4)) | (thumb ? 1u : 0u);
        return true;
    }
    return false;
}

bool mg_patch_tp_target(mg_addr pc, int *reg, mg_addr *target)
{
    mg_lib *lib = mg_ld_lib_of((const void *)pc);
    mg_svc_site *site;
    if (!lib) return false;
    for (site = lib->svc_sites; site; site = site->next) {
        if ((site->pc & ~(mg_addr)1) != pc || site->tp_reg < 0) continue;
        *reg = site->tp_reg;
        *target = (pc + 4) | (site->pc & 1);
        return true;
    }
    return false;
}

/* ------------------------------------------------ libraries left as the file has them */

/*
 * A library that sums its own code and ends the process when a byte of it has changed. TikTok Lite's libmetasec_ov.so,
 * ByteDance's protection, sums its syscall() stub (+0x86e7c, 13 words, by the function at +0x86d54) and code around its
 * anti-disassembly stubs (an init function, +0x6e9a4 to +0x6ea56), among others: a sum that differs sends the thread to
 * 0x1000, 0x4000 or 0x8000 with sp 0x2b, and the process ends with no report. Nothing in such a library is written. Its
 * PLT stays the linker's, ARM code, which threads keep on the phones tried ("ARM mode" in docs/ARCHITECTURE.md), and
 * its system calls and thread-pointer reads stop at hardware breakpoints (mg_breakpoint_add), where the fault handler does
 * them as it does a patched one's. A phone has few of those (a Snapdragon 820, six), shared by the process: a site past
 * them is patched as in any other library, and said. A sigreturn's svc is neither: a handler returns to this layer here,
 * never through its restorer. Where the host has no breakpoints (qemu's Linux), such a library is patched as any other.
 * The flag pristine-<library> makes another library so.
 */
static bool lib_pristine(const mg_lib *lib)
{
    static const char *const names[] = {
        "libmetasec_ov.so",
        /* DroidGuard returns a shorter result if its code changes, including when its ARM syscall stub becomes a UDF. */
        "libdB21738DAE92C.so",
    };
    char flag[96];
    size_t i;
    for (i = 0; i < MG_COUNT(names); i++)
        if (!strcmp(lib->name, names[i])) return true;
    snprintf(flag, sizeof(flag), "pristine-%s", lib->name);
    return mg_flag(flag);
}

/* A site recorded and a breakpoint put on it: tp_reg the register a thread-pointer read sets, -1 for an svc. */
static bool watch_site(mg_lib *lib, u8 *at, bool thumb, int tp_reg)
{
    mg_svc_site *site = (mg_svc_site *)mg_alloc(sizeof(*site));
    site->pc = (mg_addr)at | (thumb ? 1u : 0u);
    site->watched = true;
    site->tp_reg = (i8)tp_reg;
    site->next = lib->svc_sites;
    /* Recorded before the breakpoint is there: a thread that stops at it finds it. */
    lib->svc_sites = site;
    if (mg_breakpoint_add(site->pc)) return true;
    lib->svc_sites = site->next;
    free(site);
    return false;
}

/* A sigreturn's svc: 119 or 173 put in r7 just before it, a signal restorer's. */
static bool t_svc_sigreturn(const u8 *seg, size_t i)
{
    u16 a = mg_rd16(seg + i - 2);
    return a == 0x2777 || a == 0x27AD;
}
static bool a_svc_sigreturn(const u8 *seg, size_t i)
{
    u32 a = i >= 4 ? mg_rd32(seg + i - 4) : 0;
    return a == 0xE3A07077u || a == 0xE3A070ADu;
}

static void watch_segment(mg_lib *lib, u8 *seg, size_t size)
{
    u32 svc = 0, tp = 0, patched = 0;
    bool trap;
    size_t i;
    int reg;
    if (g_trap_svc < 0) g_trap_svc = mg_flag("trap-svc") ? 1 : 0;
    trap = g_trap_svc > 0;
    for (i = 0; i + 4 <= size; i += 2) {
        u8 *at = seg + i;
        bool thumb = is_t32_tp_read(at, &reg);
        if (!thumb && ((i & 3) || !is_a32_tp_read(at, &reg))) continue;
        if (!in_code(lib, at, 4)) continue;
        if (watch_site(lib, at, thumb, reg)) {
            tp++;
        } else if (patch_tp_read(lib, at, thumb, reg)) {
            lib->stats.tp_reads++;
            patched++;
        } else {
            lib->stats.tp_reads_failed++;
        }
        i += 2;
    }
    for (i = 2; i + 2 <= size; i += 2) {
        if (mg_rd16(seg + i) != 0xDF00 || !t_svc_numbered(seg, i) || !in_code(lib, seg + i, 2) || t_svc_sigreturn(seg, i)) continue;
        if (watch_site(lib, seg + i, true, -1)) {
            svc++;
        } else {
            patch_svc_site(lib, seg + i, true, trap);
            patched++;
        }
    }
    for (i = (size_t)(MG_ALIGN_UP((mg_addr)seg, 4) - (mg_addr)seg); i + 4 <= size; i += 4) {
        if (mg_rd32(seg + i) != 0xEF000000u || !in_code(lib, seg + i, 4) || a_svc_sigreturn(seg, i)) continue;
        if (watch_site(lib, seg + i, false, -1)) {
            svc++;
        } else {
            patch_svc_site(lib, seg + i, false, trap);
            patched++;
        }
    }
    mg_info("ld: %s is left as the file has it, as it sums its own code: %u system calls and %u thread-pointer reads stop at "
            "hardware breakpoints (%d of the process's)", lib->name, (unsigned)svc, (unsigned)tp, mg_breakpoint_count());
    if (patched)
        mg_warn("ld: %s: %u more patched as in any other library, past the hardware breakpoints the phone has: if it sums them, "
                "it ends the process", lib->name, (unsigned)patched);
    /* Every guest thread, this one first, which is about to run the library's init functions. */
    if (svc || tp) mg_gthreads_arm_breakpoints();
}

void mg_patch_segment(mg_lib *lib, u8 *seg, size_t size)
{
    if (lib_pristine(lib) && mg_breakpoints_available()) {
        watch_segment(lib, seg, size);
        return;
    }
    patch_plt(lib, seg, size);
    patch_tp_reads(lib, seg, size);
    patch_svc(lib, seg, size);
}

/* ------------------------------------------------------ unresolved imports */

static u32 unresolved_called(const char *name)
{
    mg_note_once("guest code called %s, which nothing provides", name);
    return 0;
}

mg_addr mg_patch_unresolved_stub(mg_lib *lib, const char *name)
{
#if MG_HOST_ARM32
    u8 *st = mg_island_alloc(lib, 0, 16, 0);
    if (!st) return 0;
    mg_wr16(st + 0, 0xF8DF);           /* ldr.w r0, [pc, #4] */
    mg_wr16(st + 2, 0x0004);
    mg_wr16(st + 4, 0xF8DF);           /* ldr.w pc, [pc, #4] */
    mg_wr16(st + 6, 0xF004);
    mg_wr32(st + 8, (u32)(mg_addr)name);
    mg_wr32(st + 12, (u32)(mg_addr)unresolved_called);
    return (mg_addr)st | 1u;
#else
    (void)lib; (void)name; (void)unresolved_called;
    return 0;
#endif
}

#endif /* MG_ARCH_ARM32 */
