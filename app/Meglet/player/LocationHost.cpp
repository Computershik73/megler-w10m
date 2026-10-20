#include "pch.h"

#include "player/LocationHost.h"

#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Devices.Geolocation.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace meglet::app {
namespace {

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
int gAccess = MEGLET_LOCATION_UNKNOWN; // Windows' answer, once asked; followed by StatusChanged
bool gAsking = false;
std::condition_variable gAsked;
Geolocator gLocator{nullptr};
winrt::event_token gPositionToken{}, gStatusToken{};
bool gFollowing = false;
bool gHaveFix = false;
meglet_location gFix = {};
bool gSaidFix = false;

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

meglet_location placeFix(double latitude, double longitude) {
    meglet_location f = {};
    f.latitude = latitude;
    f.longitude = longitude;
    f.accuracy_m = 5.0f;
    // (A new fix each second, as the satellites give one.)
    f.time_ms = nowMs() / 1000 * 1000;
    f.source = MEGLET_LOCATION_SET_BY_HAND;
    return f;
}

/// A Geoposition as a fix: altitude where Windows says how accurate it is, speed and heading where it has them.
meglet_location fixOf(const Geoposition& position) {
    const Geocoordinate c = position.Coordinate();
    const auto p = c.Point().Position();
    meglet_location f = {};
    f.latitude = p.Latitude;
    f.longitude = p.Longitude;
    f.accuracy_m = float(c.Accuracy());
    if (auto a = c.AltitudeAccuracy()) {
        f.altitude = p.Altitude;
        f.altitude_accuracy_m = float(a.Value());
        f.has |= MEGLET_LOCATION_HAS_ALTITUDE | MEGLET_LOCATION_HAS_ALTITUDE_ACCURACY;
    }
    if (auto s = c.Speed()) {
        if (!std::isnan(s.Value())) {
            f.speed_mps = float(s.Value());
            f.has |= MEGLET_LOCATION_HAS_SPEED;
        }
    }
    if (auto h = c.Heading()) {
        if (!std::isnan(h.Value())) {
            f.bearing_deg = float(h.Value());
            f.has |= MEGLET_LOCATION_HAS_BEARING;
        }
    }
    // (A DateTime counts 100 ns from 1601.)
    f.time_ms = (c.Timestamp().time_since_epoch().count() - 116444736000000000LL) / 10000;
    f.source = c.PositionSource() == PositionSource::Satellite ? MEGLET_LOCATION_SATELLITES : MEGLET_LOCATION_NETWORK;
    return f;
}

void keep(const Geoposition& position) {
    meglet_location f = fixOf(position);
    bool say;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gFix = f;
        gHaveFix = true;
        say = !gSaidFix;
        gSaidFix = true;
    }
    if (say)
        shellLog::write(format("location: the first fix, from %s, %.0f m accurate, %lld ms old", f.source == MEGLET_LOCATION_SATELLITES ? "the satellites" : "Wi-Fi or the cell network",
                               double(f.accuracy_m), nowMs() - f.time_ms));
}

int accessOf(GeolocationAccessStatus s) {
    switch (s) {
    case GeolocationAccessStatus::Allowed: return MEGLET_LOCATION_ON;
    case GeolocationAccessStatus::Denied: return MEGLET_LOCATION_DENIED;
    default: return MEGLET_LOCATION_OFF;
    }
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
        int access = MEGLET_LOCATION_OFF;
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
                finish(status == AsyncStatus::Completed ? accessOf(done.GetResults()) : MEGLET_LOCATION_OFF);
            });
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("location: Windows could not be asked: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
            finish(MEGLET_LOCATION_OFF);
        }
    });
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, std::chrono::seconds(170), [&state] { return state->done; })) {
        shellLog::write("location: neither the UI thread nor Windows answered within 170 seconds");
        return MEGLET_LOCATION_UNKNOWN;
    }
    return state->access;
}

void onStatus(const Geolocator&, const StatusChangedEventArgs& args) {
    const PositionStatus s = args.Status();
    std::lock_guard<std::mutex> lock(gMutex);
    if (s == PositionStatus::Disabled && gAccess == MEGLET_LOCATION_ON) gAccess = MEGLET_LOCATION_OFF;
    else if ((s == PositionStatus::Ready || s == PositionStatus::Initializing || s == PositionStatus::NoData) && gAccess == MEGLET_LOCATION_OFF) gAccess = MEGLET_LOCATION_ON;
    else if (s == PositionStatus::NotAvailable) gAccess = MEGLET_LOCATION_NONE;
}

} // namespace

LocationHost& LocationHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static LocationHost* const host = new LocationHost();
    return *host;
}

meglet_location_host LocationHost::callbacks() {
    meglet_location_host host = {};
    host.user = this;
    host.access = &accessThunk;
    host.start = &startThunk;
    host.stop = &stopThunk;
    host.last = &lastThunk;
    return host;
}

int LocationHost::accessThunk(void*, bool ask) {
    double latitude = 0, longitude = 0;
    if (placeSet(latitude, longitude)) return MEGLET_LOCATION_ON;
    {
        std::unique_lock<std::mutex> lock(gMutex);
        if (gAccess != MEGLET_LOCATION_UNKNOWN || !ask) return gAccess;
        // (One question at a time: a second asker waits for the first's answer.)
        if (gAsking) {
            gAsked.wait(lock, [] { return !gAsking; });
            return gAccess;
        }
        gAsking = true;
    }
    int access = MEGLET_LOCATION_UNKNOWN;
    inMta([&access] { access = askWindows(); });
    shellLog::write(format("location: Windows says %s", access == MEGLET_LOCATION_ON       ? "Meglet may have the location"
                                                        : access == MEGLET_LOCATION_DENIED ? "Meglet may not have the location (Settings > Privacy > Location)"
                                                        : access == MEGLET_LOCATION_OFF    ? "the location is not to be had"
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
        if (gAccess != MEGLET_LOCATION_ON) return false;
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

bool LocationHost::lastThunk(void*, meglet_location* fix) {
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

} // namespace meglet::app
