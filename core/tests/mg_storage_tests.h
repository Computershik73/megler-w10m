/* The tests of the phone's own folders in an app's storage (mg_storage_tests.c). */
#ifndef MG_STORAGE_TESTS_H
#define MG_STORAGE_TESTS_H

#include "mg_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Foreign paths in the host layer (host/mg_host.h) and the shared storage an
 * app sees through the C library (bionic/mg_libc_io.c, run/mg_run.c), with a
 * stand-in for the shell's storage host: folders under `scratch`, opened with
 * the operating system's plain calls. Needs no guest code and no phone. It
 * sets where an app's files are, to folders of its own under `scratch`, and
 * leaves the shared storage there with no links: run it before an app
 * starts, not while one runs. Returns the number of failed checks and adds
 * the number made to *checks.
 */
int mg_storage_tests_run(const char *scratch, mg_test_sink sink, void *user, int *checks);

/*
 * The same tests with a storage host of the caller's in the stand-in's place:
 * `make` is handed three folders under `scratch` and the names to link them
 * by, and returns the host whose links 0, 1 and 2 they are (NULL: it could
 * not). That is how the shell's own host, which goes through Windows.Storage,
 * is run on a PC (mg_tests --storage-windows, core/tests/mg_storage_windows.cpp).
 */
struct megler_storage_host;
typedef const struct megler_storage_host *(*mg_storage_host_maker)(const char *const *names, const char *const *folders, int count);
int mg_storage_tests_run_shell(const char *scratch, mg_storage_host_maker make, mg_test_sink sink, void *user, int *checks);

/*
 * The check a phone runs (megler_storage_check in megler.h), for each of `count`
 * folders of the shared storage, /sdcard/<name>: what an app does to files,
 * through the C library, a line for every step -- listed; a folder made; a
 * file written, read back through the storage's other name, stat'ed,
 * appended to, emptied; renamed, replaced, moved into a folder; a folder
 * that holds something refused removal; folders renamed and moved; all of it
 * removed again. And a file renamed from the first folder to the second. It
 * works in a folder of its own, megler-storage-check, and touches nothing else
 * that is there. The shared storage has to be set (mg_run_storage) and no
 * app running. Returns the number of steps that did not go as they must and
 * adds the number of steps to *checks.
 */
int mg_storage_check_run(const char *const *names, int count, mg_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
