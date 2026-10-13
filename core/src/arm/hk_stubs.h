#ifndef HK_STUBS_H
#define HK_STUBS_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copies the stubs into executable memory and registers the ones guest code imports (setjmp, 64-bit division). False when executable memory cannot be had. */
bool hk_stubs_init(void);

enum { HK_STUB_ARM_SPIN = 1, HK_STUB_ARM_ONCE, HK_STUB_ARM_ONCE_ENTRY, HK_STUB_READ_TPIDRURO, HK_STUB_READ_TPIDRURW, HK_STUB_UDF, HK_STUB_ARM_UDF, HK_STUB_ARM_UDF_AT };
/* A probe routine's address (Thumb bit set where it is a function); NULL before hk_stubs_init. */
void *hk_stub(int which);
bool hk_stubs_once_target(hk_addr pc, hk_addr *target);

#ifdef __cplusplus
}
#endif

#endif
