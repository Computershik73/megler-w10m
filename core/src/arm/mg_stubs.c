/* The assembly stubs (mg_stubs.S), copied into executable memory and wired to the C functions they call. */
#include "mg_stubs.h"

#include "../bionic/mg_bionic.h"
#include "mg_stubs_blob.h"

u64 mg_aeabi_uldivmod(u64 n, u64 d, u64 *rem);
i64 mg_aeabi_ldivmod(i64 n, i64 d, i64 *rem);

static u8 *g_code;

#define THUMB(off) ((void *)((mg_addr)(g_code + (off)) | 1u))

bool mg_stubs_init(void)
{
#if MG_HOST_ARM32
    static const mg_sym empty[] = { MG_END };
    mg_sym table[8];
    int n = 0;
    if (g_code) return true;
    g_code = (u8 *)mg_mem_alloc(65536);
    if (!g_code) return false;
    memcpy(g_code, mg_stubs_blob, sizeof(mg_stubs_blob));
    mg_wr32(g_code + MG_SLOT_ULDIVMOD_OFFSET, (u32)(mg_addr)mg_aeabi_uldivmod);
    mg_wr32(g_code + MG_SLOT_LDIVMOD_OFFSET, (u32)(mg_addr)mg_aeabi_ldivmod);
    mg_wr32(g_code + MG_SLOT_ARM_ONCE_OFFSET, (u32)(mg_addr)(g_code + MG_STUB_ARM_ONCE_LANDING_OFFSET) | 1u);
    if (!mg_mem_protect(g_code, 65536, MG_PROT_RX)) {
        mg_err("stubs: executable memory was refused -- is the codeGeneration capability granted?");
        mg_mem_release(g_code, 65536);
        g_code = NULL;
        return false;
    }
    mg_icache_flush(g_code, sizeof(mg_stubs_blob));
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
