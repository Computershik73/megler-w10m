/* The assembly stubs (hk_stubs.S), copied into executable memory and wired to the C functions they call. */
#include "hk_stubs.h"

#include "../bionic/hk_bionic.h"
#include "hk_stubs_blob.h"

u64 hk_aeabi_uldivmod(u64 n, u64 d, u64 *rem);
i64 hk_aeabi_ldivmod(i64 n, i64 d, i64 *rem);

static u8 *g_code;

#define THUMB(off) ((void *)((hk_addr)(g_code + (off)) | 1u))

bool hk_stubs_init(void)
{
#if HK_HOST_ARM32
    static const hk_sym empty[] = { HK_END };
    hk_sym table[8];
    int n = 0;
    if (g_code) return true;
    g_code = (u8 *)hk_mem_alloc(65536);
    if (!g_code) return false;
    memcpy(g_code, hk_stubs_blob, sizeof(hk_stubs_blob));
    hk_wr32(g_code + HK_SLOT_ULDIVMOD_OFFSET, (u32)(hk_addr)hk_aeabi_uldivmod);
    hk_wr32(g_code + HK_SLOT_LDIVMOD_OFFSET, (u32)(hk_addr)hk_aeabi_ldivmod);
    hk_wr32(g_code + HK_SLOT_ARM_ONCE_OFFSET, (u32)(hk_addr)(g_code + HK_STUB_ARM_ONCE_LANDING_OFFSET) | 1u);
    if (!hk_mem_protect(g_code, 65536, HK_PROT_RX)) {
        hk_err("stubs: executable memory was refused -- is the codeGeneration capability granted?");
        hk_mem_release(g_code, 65536);
        g_code = NULL;
        return false;
    }
    hk_icache_flush(g_code, sizeof(hk_stubs_blob));
    (void)empty;
    table[n].name = "setjmp"; table[n++].addr = THUMB(HK_STUB_SETJMP_OFFSET);
    table[n].name = "_setjmp"; table[n++].addr = THUMB(HK_STUB_SETJMP_OFFSET);
    table[n].name = "sigsetjmp"; table[n++].addr = THUMB(HK_STUB_SETJMP_OFFSET);
    table[n].name = "longjmp"; table[n++].addr = THUMB(HK_STUB_LONGJMP_OFFSET);
    table[n].name = "_longjmp"; table[n++].addr = THUMB(HK_STUB_LONGJMP_OFFSET);
    table[n].name = "siglongjmp"; table[n++].addr = THUMB(HK_STUB_LONGJMP_OFFSET);
    table[n].name = NULL; table[n].addr = NULL;
    hk_bionic_register(table);
    n = 0;
    table[n].name = "__aeabi_uldivmod"; table[n++].addr = THUMB(HK_STUB_ULDIVMOD_OFFSET);
    table[n].name = "__aeabi_ldivmod"; table[n++].addr = THUMB(HK_STUB_LDIVMOD_OFFSET);
    table[n].name = NULL; table[n].addr = NULL;
    hk_bionic_register(table);
    return true;
#else
    return false;
#endif
}

void *hk_stub(int which)
{
    if (!g_code) return NULL;
    switch (which) {
    case HK_STUB_ARM_SPIN: return THUMB(HK_STUB_ARM_SPIN_OFFSET);
    case HK_STUB_ARM_ONCE: return THUMB(HK_STUB_ARM_ONCE_OFFSET);
    case HK_STUB_ARM_ONCE_ENTRY: return g_code + HK_STUB_ARM_ONCE_ENTRY_OFFSET;
    case HK_STUB_READ_TPIDRURO: return THUMB(HK_STUB_READ_TPIDRURO_OFFSET);
    case HK_STUB_READ_TPIDRURW: return THUMB(HK_STUB_READ_TPIDRURW_OFFSET);
    case HK_STUB_UDF: return THUMB(HK_STUB_UDF_OFFSET);
    case HK_STUB_ARM_UDF: return THUMB(HK_STUB_ARM_UDF_OFFSET);
    case HK_STUB_ARM_UDF_AT: return g_code + HK_STUB_ARM_UDF_AT_OFFSET;
    case HK_STUB_ARM_COUNT: return THUMB(HK_STUB_ARM_COUNT_OFFSET);
    default: return NULL;
    }
}

/* The same rescue as for a rewritten PLT entry (hk_ld_plt_target), for the probe's own entry. */
bool hk_stubs_once_target(hk_addr pc, hk_addr *target)
{
    if (!g_code || pc != (hk_addr)(g_code + HK_STUB_ARM_ONCE_ENTRY_OFFSET)) return false;
    *target = hk_rd32(g_code + HK_SLOT_ARM_ONCE_OFFSET);
    return true;
}
