// The test program's way to the shell's sensors: megler_sensor_host as app/Megler/player/SensorHost.cpp implements it over
// Windows.Devices.Sensors, for mg_tests --sensors-windows (mg_tests_main.c), which lists this PC's sensors through it and the
// core's, and reads each a moment.
//
// What that shows: the host's own work -- the sensors Windows has found and described, each started at an interval,
// its readings handed over and made Android's, stopped -- against a PC's sensors and Windows. A PC without sensors (most
// desktops) has none to read, which the mode says; a phone's are another matter.
//
// Only tools/build-tests.sh win builds this file, with SensorHost.cpp itself; the rest of the shell it uses is stood in for
// by mg_storage_windows.cpp.
#include "pch.h"

#include "player/SensorHost.h"

#include <cstdio>

extern "C" {

const megler_sensor_host* mg_sensors_windows_host(void) {
    static megler_sensor_host host;
    host = megler::app::SensorHost::shared().callbacks();
    return &host;
}

/// What Windows has, in a line.
void mg_sensors_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", megler::app::SensorHost::shared().summary().c_str()); }

} // extern "C"
