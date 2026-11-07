/* The tests of the phone's sensors as Android's (mg_sensor_tests.c). */
#ifndef MG_SENSOR_TESTS_H
#define MG_SENSOR_TESTS_H

#include "mg_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * core/src/ndk/mg_sensors.c over the stand-in for the shell's sensors (mg_sensor_standin.c), its readings handed over by the
 * tests themselves: the list Android's side is given, every sensor's reading in Android's units and signs, the times,
 * the shell's sensor run at the shortest period wanted and stopped while the app is paused, each queue at its own rate,
 * flushes, a queue that is not read, its descriptor; and libandroid's ASensorManager, called as native code calls it.
 * Returns the number of failed checks and adds the number made to *checks.
 */
int mg_sensor_tests_run(mg_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
