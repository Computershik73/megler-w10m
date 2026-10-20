#ifndef MG_STUBS_H
#define MG_STUBS_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copies the stubs into executable memory and registers the ones guest code imports (setjmp, 64-bit division). False when executable memory cannot be had. */
bool mg_stubs_init(void);

enum { MG_STUB_ARM_SPIN = 1, MG_STUB_ARM_ONCE, MG_STUB_ARM_ONCE_ENTRY, MG_STUB_READ_TPIDRURO, MG_STUB_READ_TPIDRURW, MG_STUB_UDF, MG_STUB_ARM_UDF, MG_STUB_ARM_UDF_AT,
       MG_STUB_ARM_COUNT };
/* A probe routine's address (Thumb bit set where it is a function); NULL before mg_stubs_init. */
void *mg_stub(int which);
bool mg_stubs_once_target(mg_addr pc, mg_addr *target);

#ifdef __cplusplus
}
#endif

#endif
