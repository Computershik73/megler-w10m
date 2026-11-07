// The phone's sensors for an Android app's SensorManager and its native code's ASensorManager: megler_sensor_host
// (core/include/megler.h) over Windows.Devices.Sensors. The core makes Android's sensors of what this hands it --
// Android's types, units, signs and times (core/src/ndk/mg_sensors.c) -- so this hands Windows' readings over as they are.
//
// What the device has is looked for once, on a thread of the multithreaded apartment of this host's own, as soon as the
// host is first asked for (when an app is started), so that it is known by the time the app asks: each of Windows'
// sensors megler.h names -- the accelerometer of each reading type (standard; gravity and linear acceleration, which
// Windows' sensor fusion makes where the device has a gyrometer), the gyrometer, the magnetometer, the orientation sensor
// for absolute and for relative readings, the inclinometer, the light sensor, the proximity sensor, the barometer -- with
// its device's name and maker where Windows has them, its MinimumReportInterval and the proximity's
// MaxDistanceInMillimeters. A sensor Windows has not, or a Windows too old for it (an exception), is not there.
//
// start subscribes to the sensor's ReadingChanged at the interval asked for, never below its minimum, and hands over the
// reading it has at once (GetCurrentReading: a light or a proximity reports changes only); stop unsubscribes and gives the
// interval back to Windows' default. Both are queued to that thread and return at once: the core calls them on the app's
// threads, its main thread among them, which must not wait for Windows. Readings come on Windows' threads and go to the
// core as they are (megler_sensor_push) with their own Timestamp.
//
// One SensorHost serves the process: shared().
#pragma once

#include "megler.h"

#include <string>

namespace megler::app {

class SensorHost {
public:
    static SensorHost& shared();
    SensorHost(const SensorHost&) = delete;
    SensorHost& operator=(const SensorHost&) = delete;

    /// The callbacks for megler_run_config.sensors, with this as their user; the device is looked at from now on.
    megler_sensor_host callbacks();
    /// What was found, in a line (waits for the looking to end).
    std::string summary();

private:
    SensorHost() = default;

    static int listThunk(void* user, megler_sensor_info* info, int max);
    static void startThunk(void* user, int kind, int intervalMs);
    static void stopThunk(void* user, int kind);
};

} // namespace megler::app
