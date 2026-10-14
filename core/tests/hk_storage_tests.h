/* The tests of the phone's own folders in an app's storage (hk_storage_tests.c). */
#ifndef HK_STORAGE_TESTS_H
#define HK_STORAGE_TESTS_H

#include "hk_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Foreign paths in the host layer (host/hk_host.h) and the shared storage an
 * app sees through the C library (bionic/hk_libc_io.c, run/hk_run.c), with a
 * stand-in for the shell's storage host: folders under `scratch`, opened with
 * the operating system's plain calls. Needs no guest code and no phone. It
 * sets where an app's files are, to folders of its own under `scratch`, and
 * leaves the shared storage there with no links: run it before an app
 * starts, not while one runs. Returns the number of failed checks and adds
 * the number made to *checks.
 */
int hk_storage_tests_run(const char *scratch, hk_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
