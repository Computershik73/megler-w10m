#ifndef MG_CORE_H
#define MG_CORE_H

#include <stdio.h>

#include "mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brings the core up: the system libraries' symbol tables, the assembly stubs,
 * the fault handler. Safe to call more than once. False when this process
 * cannot run guest code -- a host that is not 32-bit ARM, or one that refuses
 * executable memory.
 */
bool mg_core_init(void);
bool mg_core_can_run(void);

/* How often a thread was interrupted on a rewritten PLT entry and rescued (arm/mg_patch.c), and how often a write-and-execute page changed sides. */
u32 mg_core_plt_rescues(void);
u32 mg_core_wx_flips(void);
/*
 * Whether this device keeps a thread in ARM (not Thumb) state, as the probe
 * last found it: 1 it does, -1 it does not, 0 not measured yet. Kept in a file
 * under the data directory, so a later start knows without probing again.
 */
int mg_core_arm_state(void);
/* A switch set by a file's presence in <data_dir>/flags (mg_api.c): for trying things on a device. Says so in the log when on. */
bool mg_flag(const char *name);
/* Every flag whose name starts with `prefix`, the rest of its name given to `fn` (mg_api.c). */
void mg_flags_each(const char *prefix, void (*fn)(const char *rest, void *user), void *user);
void mg_core_set_arm_state(int state);

/* Called with a description of a fault in guest code that nothing could repair, before the process ends. */
void mg_core_set_crash_hook(void (*hook)(const char *report));
/*
 * What a fault's report adds of the thread that faulted, written into `out` (the Java runtime's: the thread's Java frames,
 * mg_dvm_object.c). Run in the fault's handler, on that thread: it may make nothing and take no lock.
 */
void mg_core_set_fault_detail(void (*detail)(char *out, size_t cap));
/* The same detail for the thread that asks, outside any fault (empty when none was set): its Java frames, for a log line. */
void mg_core_thread_detail(char *out, size_t cap);
/*
 * Chromium's end out of memory: when the Thumb code at `pc` is the trap that ends its OnNoMemoryInternal, the address of
 * the global that function put the size it was asked for in; 0 when it is not. The crash report reads it. The code from
 * 18 bytes before pc to 16 after it must be readable.
 */
mg_addr mg_chromium_oom_global(mg_addr pc);

#ifdef __cplusplus
}
#endif

#endif
