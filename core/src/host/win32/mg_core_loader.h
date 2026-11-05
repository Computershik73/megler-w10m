/*
 * Megler.exe's half of the core on desktop Windows (docs/DESKTOP.md).
 *
 * There the core is not linked into the program: it is an ELF shared library of Android's ABI for the processor
 * (megler-core.so, built by tools/build-desktop.sh), so that its C has the guest's calling convention, `long` and
 * `va_list` throughout. This loads it into the process, binds what it imports -- the host layer's functions (mg_host.h,
 * here in Megler.exe), the C library functions it calls (mg_crt_bridge.c) -- and hands out what it exports, which the
 * stubs tools/desktop-stubs.py generates call: Megler's API (megler.h) for the shell, and the few functions the host layer
 * itself calls (mg_log_line).
 *
 * Compiled only into the desktop builds (MG_ELF_HOST); empty elsewhere.
 */
#ifndef MG_CORE_LOADER_H
#define MG_CORE_LOADER_H

#include "../../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A name the core's ELF may import, and what it is bound to. A table ends with a NULL name. */
typedef struct mg_core_import {
    const char *name;
    void *addr;
} mg_core_import;

/* Where an export the shell calls through a stub is kept: the stub jumps through *slot. A table ends with a NULL name. */
typedef struct mg_core_export {
    const char *name;
    void **slot;
} mg_core_export;

/* The tables the build makes (tools/desktop-stubs.py): the host layer's functions the core imports, and the stubs' slots. */
extern const mg_core_import mg_core_host_imports[];
extern const mg_core_export mg_core_exports[];
/* The C library functions of Windows' that the core imports, under their Android names (mg_crt_bridge.c). */
extern const mg_core_import mg_crt_bridge_imports[];
/* What the table's objects hold (stdout, stderr), set before the core is relocated. */
void mg_crt_bridge_init(void);
/* The host layer's own log lines go to the core's log from now on (mg_host_base.c). */
void mg_host_base_use_core_log(void);

/*
 * Loads the core from `path` (UTF-8), binds its imports and every stub, and runs its initialisers. False with the reason
 * in `err` when the file is not there or not the core of this processor, or something it imports is not had here (each
 * such name is said). Once per process.
 */
bool mg_core_load(const char *path, char *err, size_t errlen);
/* A function or object the core exports, or NULL. */
void *mg_core_sym(const char *name);
/* Where the core lies in memory, for a crash report; false before it is loaded. */
MG_HOSTAPI bool mg_core_image(mg_addr *base, size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* MG_CORE_LOADER_H */
