/* The tests of the network (mg_net_tests.c), and the check a phone runs. */
#ifndef MG_NET_TESTS_H
#define MG_NET_TESTS_H

#include "mg_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sockets and names through the C library as an app's code calls it (by the
 * names it binds against, with bionic's structures), over this machine's
 * loopback: TCP both ways, blocking and not, UDP, AF_UNIX pairs, options,
 * waiting on sockets with pipes and eventfds, getaddrinfo's answers field by
 * field, the interfaces. Nothing leaves the machine. Returns the number of
 * failed checks and adds the number made to *checks.
 */
int mg_net_tests_run(mg_test_sink sink, void *user, int *checks);

/*
 * The check a phone runs (meglet_net_check in meglet.h): `public_host` is
 * resolved, a TCP connection made to its port 80, a HEAD request sent for
 * `path` and the status line read; then the same against a server of the
 * check's own on loopback, and a UDP round trip on loopback; and what
 * interfaces there are, without their addresses. NULL `public_host`: the
 * part that needs the internet is left out (the tests run it so). A line for
 * every step. Returns the number of steps that failed and adds the number of
 * steps to *checks; *refused (may be NULL) is how many were refused by the
 * phone itself rather than failed -- loopback to an app's own server, which
 * an app container may not be allowed.
 */
int mg_net_check_run(const char *public_host, const char *path, mg_test_sink sink, void *user, int *checks, int *refused);

#ifdef __cplusplus
}
#endif

#endif
