#include "pch.h"

#include "player/LocationHost.h"

#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Geolocation.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace megler::app {
namespace {

using winrt::Windows::Devices::Enumeration::DeviceAccessChangedEventArgs;
using winrt::Windows::Devices::Enumeration::DeviceAccessInformation;
using winrt::Windows::Devices::Enumeration::DeviceAccessStatus;
using winrt::Windows::Devices::Geolocation::Geocoordinate;
using winrt::Windows::Devices::Geolocation::GeolocationAccessStatus;
using winrt::Windows::Devices::Geolocation::Geolocator;
using winrt::Windows::Devices::Geolocation::Geoposition;
using winrt::Windows::Devices::Geolocation::PositionAccuracy;
using winrt::Windows::Devices::Geolocation::PositionChangedEventArgs;
using winrt::Windows::Devices::Geolocation::PositionSource;
using winrt::Windows::Devices::Geolocation::PositionStatus;
using winrt::Windows::Devices::Geolocation::StatusChangedEventArgs;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;

std::mutex gMutex;        // the answer and the fix
std::mutex gLocatorMutex; // the Geolocator and its events (never held while gMutex is: its handlers take that)
int gAccess = MEGLER_LOCATION_UNKNOWN; // Windows' answer, once asked; followed by StatusChanged
bool gAsking = false;
std::condition_variable gAsked;
Geolocator gLocator{nullptr};
winrt::event_token gPositionToken{}, gStatusToken{};
bool gFollowing = false;
bool gHaveFix = false;
megler_location gFix = {};
bool gSaidFix = false;
/// What the fixes were, said once a minute while they come (keep): how many, how many the satellites found, how accurate.
/// Only the first was said before, and a user's "location is still kinda awful" (Transit, 0.1.0.679) could not be told from
/// a phone indoors, where Windows finds it by Wi-Fi and the cell network alone (300 to 600 m), or the satellites never
/// asked for.
struct FixStats {
    long long from = 0;
    int fixes = 0, satellites = 0, lastSource = 0;
    float best = 0, last = 0;
};
FixStats gStats;
// Windows' location consent of this package (the device class Windows' documentation checks it by), read once and followed
// (AccessChanged), kept alive here.
const winrt::guid kLocationConsent{0xBFA794E4, 0xF964, 0x4FDB, {0x90, 0xF6, 0x51, 0x05, 0x6B, 0xFE, 0x4B, 0x44}};
DeviceAccessInformation gConsent{nullptr};
winrt::event_token gConsentToken{};
std::once_flag gConsentOnce;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/// `f` on a thread of the multithreaded apartment, waited for: the core's calls come on threads with no apartment.
template <class F>
void inMta(F&& f) {
    std::thread t([&f] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            f();
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("location: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
        } catch (...) {
            shellLog::write("location: an exception");
        }
        winrt::uninit_apartment();
    });
    t.join();
}

bool placeSet(double& latitude, double& longitude) { return settings::location::setPlace(latitude, longitude); }

megler_location placeFix(double latitude, double longitude) {
    megler_location f = {};
    f.latitude = latitude;
    f.longitude = longitude;
    f.accuracy_m = 5.0f;
    // (A new fix each second, as the satellites give one.)
    f.time_ms = nowMs() / 1000 * 1000;
    f.source = MEGLER_LOCATION_SET_BY_HAND;
    return f;
}

/// A Geoposition as a fix: altitude where Windows says how accurate it is, speed and heading where it has them.
megler_location fixOf(const Geoposition& position) {
    const Geocoordinate c = position.Coordinate();
    const auto p = c.Point().Position();
    megler_location f = {};
    f.latitude = p.Latitude;
    f.longitude = p.Longitude;
    f.accuracy_m = float(c.Accuracy());
    if (auto a = c.AltitudeAccuracy()) {
        f.altitude = p.Altitude;
        f.altitude_accuracy_m = float(a.Value());
        f.has |= MEGLER_LOCATION_HAS_ALTITUDE | MEGLER_LOCATION_HAS_ALTITUDE_ACCURACY;
    }
    if (auto s = c.Speed()) {
        if (!std::isnan(s.Value())) {
            f.speed_mps = float(s.Value());
            f.has |= MEGLER_LOCATION_HAS_SPEED;
        }
    }
    if (auto h = c.Heading()) {
        if (!std::isnan(h.Value())) {
            f.bearing_deg = float(h.Value());
            f.has |= MEGLER_LOCATION_HAS_BEARING;
        }
    }
    // (A DateTime counts 100 ns from 1601.)
    f.time_ms = (c.Timestamp().time_since_epoch().count() - 116444736000000000LL) / 10000;
    f.source = c.PositionSource() == PositionSource::Satellite ? MEGLER_LOCATION_SATELLITES : MEGLER_LOCATION_NETWORK;
    return f;
}

void keep(const Geoposition& position) {
    megler_location f = fixOf(position);
    bool say, sayStats = false;
    FixStats stats;
    const long long now = nowMs();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gFix = f;
        gHaveFix = true;
        say = !gSaidFix;
        gSaidFix = true;
        if (!gStats.from) gStats.from = now;
        gStats.fixes++;
        if (f.source == MEGLER_LOCATION_SATELLITES) gStats.satellites++;
        if (gStats.fixes == 1 || f.accuracy_m < gStats.best) gStats.best = f.accuracy_m;
        gStats.last = f.accuracy_m;
        gStats.lastSource = f.source;
        if (now - gStats.from >= 60000) {
            stats = gStats;
            gStats = FixStats();
            sayStats = true;
        }
    }
    if (say)
        shellLog::write(format("location: the first fix, from %s, %.0f m accurate, %lld ms old", f.source == MEGLER_LOCATION_SATELLITES ? "the satellites" : "Wi-Fi or the cell network",
                               double(f.accuracy_m), now - f.time_ms));
    if (sayStats)
        shellLog::write(format("location: %d fixes in the last %lld s, %d of them from the satellites; accurate to %.0f m at best, %.0f m the last (from %s)",
                               stats.fixes, (now - stats.from) / 1000, stats.satellites, double(stats.best), double(stats.last),
                               stats.lastSource == MEGLER_LOCATION_SATELLITES ? "the satellites" : "Wi-Fi or the cell network"));
}

int accessOf(GeolocationAccessStatus s) {
    switch (s) {
    case GeolocationAccessStatus::Allowed: return MEGLER_LOCATION_ON;
    case GeolocationAccessStatus::Denied: return MEGLER_LOCATION_DENIED;
    default: return MEGLER_LOCATION_OFF;
    }
}

/// Windows' consent as the core's answer: allowed, refused by the user for this app, refused by the system -- the phone's
/// location switched off, or every app's (Settings > Privacy > Location) -- or not decided yet.
int accessOfConsent(DeviceAccessStatus s) {
    switch (s) {
    case DeviceAccessStatus::Allowed: return MEGLER_LOCATION_ON;
    case DeviceAccessStatus::DeniedByUser: return MEGLER_LOCATION_DENIED;
    case DeviceAccessStatus::DeniedBySystem: return MEGLER_LOCATION_OFF;
    default: return MEGLER_LOCATION_UNKNOWN;
    }
}

const char* consentName(DeviceAccessStatus s) {
    switch (s) {
    case DeviceAccessStatus::Allowed: return "allowed";
    case DeviceAccessStatus::DeniedByUser: return "denied by the user for this app (Settings > Privacy > Location)";
    case DeviceAccessStatus::DeniedBySystem: return "denied by the system: the phone's location is switched off, or no app may have it (Settings > Privacy > Location)";
    default: return "not decided yet: Windows asks the first time the app wants it";
    }
}

/// The consent, nothing asked, from now on followed: what Windows says of this package before anyone asks (the phone's
/// location switched off is said so, and an app asking is told so rather than taken for one not asked yet), and each change
/// the user makes in Windows' settings while the app runs. Once for the process.
void followConsent() {
    std::call_once(gConsentOnce, [] {
        inMta([] {
            DeviceAccessInformation info = DeviceAccessInformation::CreateFromDeviceClassId(kLocationConsent);
            const DeviceAccessStatus now = info.CurrentStatus();
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (gAccess == MEGLER_LOCATION_UNKNOWN) gAccess = accessOfConsent(now);
            }
            shellLog::write(format("location: Windows' consent for this app: %s", consentName(now)));
            gConsentToken = info.AccessChanged([](const DeviceAccessInformation&, const DeviceAccessChangedEventArgs& args) {
                const DeviceAccessStatus s = args.Status();
                const int access = accessOfConsent(s);
                bool changed;
                {
                    std::lock_guard<std::mutex> lock(gMutex);
                    changed = gAccess != access;
                    gAccess = access;
                }
                if (changed) shellLog::write(format("location: Windows' consent for this app changed: %s", consentName(s)));
            });
            gConsent = info;
        });
    });
}

/// After Windows' question was answered no: whether that was the system's (the location off) rather than the user's.
int refinedRefusal() {
    int access = MEGLER_LOCATION_DENIED;
    inMta([&access] {
        if (DeviceAccessInformation::CreateFromDeviceClassId(kLocationConsent).CurrentStatus() == DeviceAccessStatus::DeniedBySystem) access = MEGLER_LOCATION_OFF;
    });
    return access;
}

/// Windows' question to the user, on the UI thread where it can be asked (on this one where there is none: the tests'
/// program). Waits for the answer, as long as the user takes, up to 170 seconds.
int askWindows() {
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    try {
        dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().Dispatcher();
    } catch (const winrt::hresult_error&) {
    }
    if (!dispatcher) return accessOf(Geolocator::RequestAccessAsync().get());
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        int access = MEGLER_LOCATION_OFF;
    };
    auto state = std::make_shared<State>();
    auto finish = [state](int access) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->access = access;
        state->done = true;
        state->cv.notify_all();
    };
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [finish] {
        try {
            IAsyncOperation<GeolocationAccessStatus> op = Geolocator::RequestAccessAsync();
            op.Completed([finish](const IAsyncOperation<GeolocationAccessStatus>& done, AsyncStatus status) {
                finish(status == AsyncStatus::Completed ? accessOf(done.GetResults()) : MEGLER_LOCATION_OFF);
            });
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("location: Windows could not be asked: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
            finish(MEGLER_LOCATION_OFF);
        }
    });
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, std::chrono::seconds(170), [&state] { return state->done; })) {
        shellLog::write("location: neither the UI thread nor Windows answered within 170 seconds");
        return MEGLER_LOCATION_UNKNOWN;
    }
    return state->access;
}

void onStatus(const Geolocator&, const StatusChangedEventArgs& args) {
    const PositionStatus s = args.Status();
    std::lock_guard<std::mutex> lock(gMutex);
    if (s == PositionStatus::Disabled && gAccess == MEGLER_LOCATION_ON) gAccess = MEGLER_LOCATION_OFF;
    else if ((s == PositionStatus::Ready || s == PositionStatus::Initializing || s == PositionStatus::NoData) && gAccess == MEGLER_LOCATION_OFF) gAccess = MEGLER_LOCATION_ON;
    else if (s == PositionStatus::NotAvailable) gAccess = MEGLER_LOCATION_NONE;
}

} // namespace

LocationHost& LocationHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static LocationHost* const host = new LocationHost();
    return *host;
}

megler_location_host LocationHost::callbacks() {
    megler_location_host host = {};
    host.user = this;
    host.access = &accessThunk;
    host.start = &startThunk;
    host.stop = &stopThunk;
    host.last = &lastThunk;
    return host;
}

int LocationHost::accessThunk(void*, bool ask) {
    double latitude = 0, longitude = 0;
    if (placeSet(latitude, longitude)) return MEGLER_LOCATION_ON;
    followConsent();
    {
        std::unique_lock<std::mutex> lock(gMutex);
        if (gAccess != MEGLER_LOCATION_UNKNOWN || !ask) return gAccess;
        // (One question at a time: a second asker waits for the first's answer.)
        if (gAsking) {
            gAsked.wait(lock, [] { return !gAsking; });
            return gAccess;
        }
        gAsking = true;
    }
    int access = MEGLER_LOCATION_UNKNOWN;
    inMta([&access] { access = askWindows(); });
    // (Windows' no is the system's when the phone's location is off: no question was put, and the app is told the location
    // is off rather than refused.)
    if (access == MEGLER_LOCATION_DENIED) access = refinedRefusal();
    shellLog::write(format("location: Windows says %s", access == MEGLER_LOCATION_ON       ? "Megler may have the location"
                                                        : access == MEGLER_LOCATION_DENIED ? "Megler may not have the location (Settings > Privacy > Location)"
                                                        : access == MEGLER_LOCATION_OFF    ? "the phone's location is switched off (Settings > Privacy > Location)"
                                                                                         : "nothing yet"));
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gAccess = access;
        gAsking = false;
    }
    gAsked.notify_all();
    return access;
}

bool LocationHost::startThunk(void*, int intervalMs) {
    double latitude = 0, longitude = 0;
    if (placeSet(latitude, longitude)) {
        shellLog::write(format("location: the place set in Settings, %.6f,%.6f, every %d ms", latitude, longitude, intervalMs));
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gAccess != MEGLER_LOCATION_ON) return false;
    }
    bool ok = false;
    inMta([&ok, intervalMs] {
        std::lock_guard<std::mutex> lock(gLocatorMutex);
        if (!gLocator) {
            gLocator = Geolocator();
            gLocator.DesiredAccuracy(PositionAccuracy::High);
        }
        if (gFollowing) {
            gLocator.PositionChanged(gPositionToken);
            gLocator.StatusChanged(gStatusToken);
        }
        gLocator.ReportInterval(uint32_t(intervalMs));
        gPositionToken = gLocator.PositionChanged([](const Geolocator&, const PositionChangedEventArgs& args) { keep(args.Position()); });
        gStatusToken = gLocator.StatusChanged(&onStatus);
        gFollowing = true;
        // A first fix soon: the newest Windows has, up to two minutes old, or a new one within ten seconds.
        auto first = gLocator.GetGeopositionAsync(std::chrono::minutes(2), std::chrono::seconds(10));
        first.Completed([](const IAsyncOperation<Geoposition>& done, AsyncStatus status) {
            if (status != AsyncStatus::Completed) return;
            try {
                keep(done.GetResults());
            } catch (const winrt::hresult_error&) {
            }
        });
        ok = true;
    });
    return ok;
}

void LocationHost::stopThunk(void*) {
    inMta([] {
        std::lock_guard<std::mutex> lock(gLocatorMutex);
        if (!gFollowing || !gLocator) return;
        gLocator.PositionChanged(gPositionToken);
        gLocator.StatusChanged(gStatusToken);
        gFollowing = false;
    });
}

bool LocationHost::lastThunk(void*, megler_location* fix) {
    double latitude = 0, longitude = 0;
    if (placeSet(latitude, longitude)) {
        *fix = placeFix(latitude, longitude);
        return true;
    }
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gHaveFix) return false;
    *fix = gFix;
    return true;
}

} // namespace megler::app
