/* The tests of the phone's Bluetooth LE radio between the shell and the framework (mg_bluetooth_tests.c). */
#ifndef MG_BLUETOOTH_TESTS_H
#define MG_BLUETOOTH_TESTS_H

#include "mg_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * core/src/run/mg_bluetooth.c over the stand-in for the shell's radio (mg_bluetooth_standin.c): the radio's state and name,
 * the devices paired before, an advertisement's device (its kind of address, its name, its RSSI), a connection's events in
 * order and none after it is closed, the RSSI answered from the last advertisement, advertisements nobody takes dropped
 * past the queue's bound while every answer waits, what is pushed copied, and nothing after the app has ended. Returns the
 * number of failed checks and adds the number made to *checks.
 */
int mg_bluetooth_tests_run(mg_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
