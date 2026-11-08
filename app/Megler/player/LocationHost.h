// The phone's location for an Android app's android.location.LocationManager:
// megler_location_host (core/include/megler.h) over
// Windows.Devices.Geolocation's Geolocator.
//
// Whether an app may have the location is Windows' question to the user,
// asked the first time an app wants it (Geolocator::RequestAccessAsync, on
// the UI thread, where Windows can ask; the package declares the location
// capability). Before anyone asks, what Windows already says of the package
// is read (its consent, DeviceAccessInformation of the location's consent
// class) and followed while the process runs (AccessChanged): allowed, the
// user's no for this app (Settings > Privacy > Location), the system's no --
// the phone's location switched off, which an app is told as the location
// off, not as a refusal -- or not decided yet, and the log says each. The
// answer is kept for the process and follows Windows' later changes too
// (StatusChanged: the location switched off in the phone's settings, or on
// again). While the core follows the location, the
// Geolocator reports at the interval asked for, at high accuracy, and each
// fix is kept for last: where, how accurately, the altitude, speed and
// heading where Windows has them, when, and whether the satellites found it
// or Wi-Fi and the cell network. When it starts, the newest fix Windows has
// (up to two minutes old) is taken at once.
//
// A place set in Megler's settings (Settings > Location, shell/Settings.h)
// stands in for the phone's: every app is told the phone is there, the
// location on, without asking Windows -- a fix a second while it is
// followed, as the satellites give.
//
// The core's calls come on the app's own threads, which have no COM
// apartment; what needs WinRT is done on a thread of the multithreaded
// apartment while the caller waits. Fixes come on Windows' own threads.
//
// One LocationHost serves the process: shared().
#pragma once

#include "megler.h"

namespace megler::app {

class LocationHost {
public:
    static LocationHost& shared();
    LocationHost(const LocationHost&) = delete;
    LocationHost& operator=(const LocationHost&) = delete;

    /// The callbacks for megler_run_config.location, with this as their user.
    megler_location_host callbacks();

private:
    LocationHost() = default;

    static int accessThunk(void* user, bool ask);
    static bool startThunk(void* user, int intervalMs);
    static void stopThunk(void* user);
    static bool lastThunk(void* user, megler_location* fix);
};

} // namespace megler::app
