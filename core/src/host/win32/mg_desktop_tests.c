/*
 * The test program of the desktop builds (tools/build-desktop.sh tests): Megler.exe's half of the core, a main, and the
 * core's ELF beside it, megler-core.so, in which the tests are (core/tests). The arguments are mg_tests_main.c's.
 *
 * Compiled only into the desktop builds (MG_ELF_HOST).
 */
#if defined(MG_ELF_HOST)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdio.h>

#include "mg_core_loader.h"

int main(int argc, char **argv)
{
    char path[MAX_PATH + 32], err[2048];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;
    int (*entry)(int, char **);
    if (!n || n >= MAX_PATH) return 2;
    slash = strrchr(path, '\\');
    strcpy(slash ? slash + 1 : path, "megler-core.so");
    if (!mg_core_load(path, err, sizeof(err))) {
        fprintf(stderr, "mg_tests: %s\n", err);
        return 2;
    }
    entry = (int (*)(int, char **))mg_core_sym("mg_tests_main");
    if (!entry) {
        fprintf(stderr, "mg_tests: the core has no mg_tests_main (was it built with the tests?)\n");
        return 2;
    }
    return entry(argc, argv);
}

#endif /* MG_ELF_HOST */
