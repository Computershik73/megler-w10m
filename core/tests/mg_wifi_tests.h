/* The tests of the phone's Wi-Fi between the shell and the framework (mg_wifi_tests.c). */
#ifndef MG_WIFI_TESTS_H
#define MG_WIFI_TESTS_H

#include "mg_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * core/src/drivers/mg_java_wifi.c over the stand-in for the shell's Wi-Fi (mg_wifi_standin.c), and with no Wi-Fi at all: the
 * state, a scan's networks as the shell found them (their addresses, names as octets, signals, channels, security), a scan
 * bounded by the room given, the time of the report, a scan refused while the Wi-Fi is off, and the network the phone is
 * connected to. Returns the number of failed checks and adds the number made to *checks.
 */
int mg_wifi_tests_run(mg_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
