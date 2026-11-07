/* The assembly stubs (mg_stubs.S), copied into executable memory and wired to the C functions they call. */
#include "mg_stubs.h"

#include "../bionic/mg_bionic.h"
#include "../host/mg_host.h"
#include "mg_stubs_blob.h"

u64 mg_aeabi_uldivmod(u64 n, u64 d, u64 *rem);
i64 mg_aeabi_ldivmod(i64 n, i64 d, i64 *rem);

static u8 *g_code;
static bool g_svc_ready;

#define THUMB(off) ((void *)((mg_addr)(g_code + (off)) | 1u))

#if MG_HOST_ARM32
/*
 * The way out of a library's ARM svc (mg_stub_svc_entry), before the stubs are made executable: its C function, whether
 * the thread's stack is the one its TEB says (Windows), and the guest's thread pointer read as the host keeps it -- the
 * opc2 of the thread register its place is reached from and its offset there (mg_guest_tp_where) -- written into the
 * entry's mrc and ldr.w. False where the thread pointer has no such place: a library's svc then stays a trap.
 */
static bool svc_entry_write(void)
{
    u8 *at = g_code + MG_STUB_SVC_TP_OFFSET;
    u32 opc2 = 0, offset = 0;
    mg_wr32(g_code + MG_SLOT_SVC_CALL_OFFSET, (u32)(mg_addr)mg_arm_svc_call);
    mg_wr32(g_code + MG_SLOT_SVC_TEB_OFFSET, MG_HOST_WINDOWS ? 1u : 0u);
    if (!mg_guest_tp_where(&opc2, &offset) || (opc2 != 2 && opc2 != 3) || offset > 4095) return false;
    /* mrc p15, 0, r1, c13, c0, 2 ; ldr.w r1, [r1, #0], as assembled. */
    if (mg_rd16(at) != 0xEE1D || mg_rd16(at + 2) != 0x1F50 || mg_rd16(at + 4) != 0xF8D1 || mg_rd16(at + 6) != 0x1000) return false;
    mg_wr16(at + 2, (u16)(0x1F10 | (opc2 << 5)));
    mg_wr16(at + 6, (u16)(0x1000 | offset));
    return true;
}

/*
 * And on Windows, once the stubs run: the TEB's words of the stack where the entry reads them (NtTib.StackBase at 4,
 * DeallocationStack at 0xE0C), checked against what Windows says of this thread's stack before they are trusted.
 */
static bool svc_entry_teb_checked(void)
{
#if MG_HOST_WINDOWS
    void *lo = NULL, *hi = NULL;
    u32 teb = ((u32(*)(void))THUMB(MG_STUB_READ_TPIDRURW_OFFSET))();
    mg_thread_stack_bounds(&lo, &hi);
    if (!teb || !hi || mg_rd32((const void *)(mg_addr)(teb + 4)) != (u32)(mg_addr)hi || mg_rd32((const void *)(mg_addr)(teb + 0xE0C)) != (u32)(mg_addr)lo) {
        mg_warn("stubs: the TEB does not say the thread's stack where the system calls' way out reads it: a library's svc stays a trap");
        return false;
    }
#endif
    return true;
}
#endif

bool mg_stubs_init(void)
{
#if MG_HOST_ARM32
    static const mg_sym empty[] = { MG_END };
    mg_sym table[8];
    int n = 0;
    bool svc;
    if (g_code) return true;
    g_code = (u8 *)mg_mem_alloc(65536);
    if (!g_code) return false;
    memcpy(g_code, mg_stubs_blob, sizeof(mg_stubs_blob));
    mg_wr32(g_code + MG_SLOT_ULDIVMOD_OFFSET, (u32)(mg_addr)mg_aeabi_uldivmod);
    mg_wr32(g_code + MG_SLOT_LDIVMOD_OFFSET, (u32)(mg_addr)mg_aeabi_ldivmod);
    mg_wr32(g_code + MG_SLOT_ARM_ONCE_OFFSET, (u32)(mg_addr)(g_code + MG_STUB_ARM_ONCE_LANDING_OFFSET) | 1u);
    svc = svc_entry_write();
    if (!mg_mem_protect(g_code, 65536, MG_PROT_RX)) {
        mg_err("stubs: executable memory was refused -- is the codeGeneration capability granted?");
        mg_mem_release(g_code, 65536);
        g_code = NULL;
        return false;
    }
    mg_icache_flush(g_code, sizeof(mg_stubs_blob));
    g_svc_ready = svc && svc_entry_teb_checked();
    (void)empty;
    table[n].name = "setjmp"; table[n++].addr = THUMB(MG_STUB_SETJMP_OFFSET);
    table[n].name = "_setjmp"; table[n++].addr = THUMB(MG_STUB_SETJMP_OFFSET);
    table[n].name = "sigsetjmp"; table[n++].addr = THUMB(MG_STUB_SETJMP_OFFSET);
    table[n].name = "longjmp"; table[n++].addr = THUMB(MG_STUB_LONGJMP_OFFSET);
    table[n].name = "_longjmp"; table[n++].addr = THUMB(MG_STUB_LONGJMP_OFFSET);
    table[n].name = "siglongjmp"; table[n++].addr = THUMB(MG_STUB_LONGJMP_OFFSET);
    table[n].name = NULL; table[n].addr = NULL;
    mg_bionic_register(table);
    n = 0;
    table[n].name = "__aeabi_uldivmod"; table[n++].addr = THUMB(MG_STUB_ULDIVMOD_OFFSET);
    table[n].name = "__aeabi_ldivmod"; table[n++].addr = THUMB(MG_STUB_LDIVMOD_OFFSET);
    table[n].name = NULL; table[n].addr = NULL;
    mg_bionic_register(table);
    return true;
#else
    return false;
#endif
}

void *mg_stub(int which)
{
    if (!g_code) return NULL;
    switch (which) {
    case MG_STUB_ARM_SPIN: return THUMB(MG_STUB_ARM_SPIN_OFFSET);
    case MG_STUB_ARM_ONCE: return THUMB(MG_STUB_ARM_ONCE_OFFSET);
    case MG_STUB_ARM_ONCE_ENTRY: return g_code + MG_STUB_ARM_ONCE_ENTRY_OFFSET;
    case MG_STUB_READ_TPIDRURO: return THUMB(MG_STUB_READ_TPIDRURO_OFFSET);
    case MG_STUB_READ_TPIDRURW: return THUMB(MG_STUB_READ_TPIDRURW_OFFSET);
    case MG_STUB_UDF: return THUMB(MG_STUB_UDF_OFFSET);
    case MG_STUB_ARM_UDF: return THUMB(MG_STUB_ARM_UDF_OFFSET);
    case MG_STUB_ARM_UDF_AT: return g_code + MG_STUB_ARM_UDF_AT_OFFSET;
    case MG_STUB_ARM_COUNT: return THUMB(MG_STUB_ARM_COUNT_OFFSET);
    /* (The guest's setjmp and longjmp, for the core's own guarded calls: mg_jni.c, mg_core.c.) */
    case MG_STUB_SETJMP: return THUMB(MG_STUB_SETJMP_OFFSET);
    case MG_STUB_LONGJMP: return THUMB(MG_STUB_LONGJMP_OFFSET);
    /* (on_stack(arg, fn, top): fn(arg) on another stack, for Java called deep in native code: mg_dvm_interp.c.) */
    case MG_STUB_ON_STACK: return THUMB(MG_STUB_ON_STACK_OFFSET);
    /* (The way out of a library's ARM svc, where it could be made: arm/mg_patch.c.) */
    case MG_STUB_SVC_ENTRY: return g_svc_ready ? THUMB(MG_STUB_SVC_ENTRY_OFFSET) : NULL;
    default: return NULL;
    }
}

/* The same rescue as for a rewritten PLT entry (mg_ld_plt_target), for the probe's own entry. */
bool mg_stubs_once_target(mg_addr pc, mg_addr *target)
{
    if (!g_code || pc != (mg_addr)(g_code + MG_STUB_ARM_ONCE_ENTRY_OFFSET)) return false;
    *target = mg_rd32(g_code + MG_SLOT_ARM_ONCE_OFFSET);
    return true;
}
