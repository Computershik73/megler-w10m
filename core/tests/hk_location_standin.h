/*
 * A stand-in for the shell's location (meglet_location_host) in the tests' runs of a Java app: the location is on, and the
 * phone is in Moscow (55.751244, 37.618423), a little further north at each second, 12 metres accurate, at the time asked;
 * there is always a fix. The counts say how often it was asked to follow the location and to stop.
 */
#ifndef HK_LOCATION_STANDIN_H
#define HK_LOCATION_STANDIN_H

#include "../include/meglet.h"

const meglet_location_host *hk_location_standin(void);
void hk_location_standin_counts(int *starts, int *stops);

#endif
