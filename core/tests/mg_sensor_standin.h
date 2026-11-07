/*
 * A stand-in for the shell's sensors (megler_sensor_host) in the tests: a device with one of each sensor megler.h names,
 * each reading a fixed value in Windows' units and signs (mg_sensor_standin_reading), stamped three milliseconds before it is
 * handed over, so that what an app reads can be checked to the last digit. While a sensor runs, a thread of the stand-in's
 * hands its reading over every 5 ms -- faster than an app asks, as a sensor may report -- and a light's or a proximity's,
 * which report changes only, once as it starts. The counts say how often each was started and stopped, and at what interval.
 */
#ifndef MG_SENSOR_STANDIN_H
#define MG_SENSOR_STANDIN_H

#include "../include/megler.h"

const megler_sensor_host *mg_sensor_standin(void);
/* What the stand-in's sensor of `kind` reads, as Windows reads it (megler.h), and its accuracy. */
int mg_sensor_standin_reading(int kind, double values[4]);
/* Whether its thread hands readings over (the default), or only the counts are kept, for a test that hands its own. */
void mg_sensor_standin_feed(bool on);
void mg_sensor_standin_counts(int kind, int *starts, int *stops, int *interval_ms);
bool mg_sensor_standin_running(int kind);

#endif
