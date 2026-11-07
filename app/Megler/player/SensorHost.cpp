#include "pch.h"

#include "player/SensorHost.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Sensors.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace megler::app {
namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationKind;
using winrt::Windows::Devices::Sensors::Accelerometer;
using winrt::Windows::Devices::Sensors::AccelerometerReading;
using winrt::Windows::Devices::Sensors::AccelerometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::AccelerometerReadingType;
using winrt::Windows::Devices::Sensors::Barometer;
using winrt::Windows::Devices::Sensors::BarometerReading;
using winrt::Windows::Devices::Sensors::BarometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::Gyrometer;
using winrt::Windows::Devices::Sensors::GyrometerReading;
using winrt::Windows::Devices::Sensors::GyrometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::Inclinometer;
using winrt::Windows::Devices::Sensors::InclinometerReading;
using winrt::Windows::Devices::Sensors::InclinometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::LightSensor;
using winrt::Windows::Devices::Sensors::LightSensorReading;
using winrt::Windows::Devices::Sensors::LightSensorReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::Magnetometer;
using winrt::Windows::Devices::Sensors::MagnetometerAccuracy;
using winrt::Windows::Devices::Sensors::MagnetometerReading;
using winrt::Windows::Devices::Sensors::MagnetometerReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::OrientationSensor;
using winrt::Windows::Devices::Sensors::OrientationSensorReading;
using winrt::Windows::Devices::Sensors::OrientationSensorReadingChangedEventArgs;
using winrt::Windows::Devices::Sensors::ProximitySensor;
using winrt::Windows::Devices::Sensors::ProximitySensorReading;
using winrt::Windows::Devices::Sensors::ProximitySensorReadingChangedEventArgs;
using winrt::Windows::Foundation::DateTime;
using winrt::Windows::Foundation::Metadata::ApiInformation;

constexpr int kinds = MEGLER_SENSOR_BAROMETER;
/// How long list waits for the looking: far longer than it takes (a phone's sensors answer in a fraction of a second).
constexpr std::chrono::seconds lookLimit{15};

const char* const kindNames[kinds + 1] = {"",
                                          "accelerometer",
                                          "gravity",
                                          "linear acceleration",
                                          "gyrometer",
                                          "magnetometer",
                                          "orientation sensor",
                                          "relative orientation sensor",
                                          "inclinometer",
                                          "light sensor",
                                          "proximity sensor",
                                          "barometer"};

/// One of Windows' sensors as found, and its state; touched on the worker alone, but for `info` once the looking is done.
struct Found {
    bool there = false;
    megler_sensor_info info{};
    uint32_t minimum = 0;
    bool running = false, saidStart = false;
    winrt::event_token token{};
    Accelerometer accelerometer{nullptr};
    Gyrometer gyrometer{nullptr};
    Magnetometer magnetometer{nullptr};
    OrientationSensor orientation{nullptr};
    Inclinometer inclinometer{nullptr};
    LightSensor light{nullptr};
    ProximitySensor proximity{nullptr};
    Barometer barometer{nullptr};
};

Found gFound[kinds + 1];
std::mutex gMutex; // gLooked
std::condition_variable gLookedChanged;
bool gLooking = false, gLooked = false;
// Whether the readings say how accurate their heading is (Windows 10 1511 and later).
bool gOrientationYaw = false, gInclinometerYaw = false;

/// A thread of the multithreaded apartment that runs what is posted to it, in order, nobody waiting.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void post(std::function<void()> work) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(work));
        }
        wake_.notify_one();
    }

private:
    void loop() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !queue_.empty(); });
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                work();
            } catch (const winrt::hresult_error& e) {
                shellLog::write(format("sensors: an error of Windows' (0x%08X): %s", unsigned(e.code()), narrow(e.message()).c_str()));
            } catch (...) {
                shellLog::write("sensors: an unexpected exception");
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
};

Worker& worker() {
    // Never destroyed: Windows' threads may hand a reading over as the process ends.
    static Worker* const w = new Worker();
    return *w;
}

void copyText(char* to, size_t size, const std::string& from) {
    const size_t n = std::min(size - 1, from.size());
    memcpy(to, from.data(), n);
    to[n] = 0;
}

int accuracyOf(MagnetometerAccuracy a) {
    switch (a) {
    case MagnetometerAccuracy::Unreliable: return MEGLER_SENSOR_ACCURACY_UNRELIABLE;
    case MagnetometerAccuracy::Approximate: return MEGLER_SENSOR_ACCURACY_APPROXIMATE;
    case MagnetometerAccuracy::High: return MEGLER_SENSOR_ACCURACY_HIGH;
    default: return MEGLER_SENSOR_ACCURACY_UNKNOWN;
    }
}

// ------------------------------------------------------------------ readings

/// A reading to the core: Windows' values as they are, and when Windows read it (a DateTime counts 100 ns from 1601).
void hand(int kind, double a, double b, double c, double d, int accuracy, DateTime when) {
    megler_sensor_sample s{};
    s.kind = kind;
    s.accuracy = accuracy;
    s.values[0] = a;
    s.values[1] = b;
    s.values[2] = c;
    s.values[3] = d;
    s.time_ns = (when.time_since_epoch().count() - 116444736000000000LL) * 100;
    megler_sensor_push(&s);
}

void handAccelerometer(int kind, const AccelerometerReading& r) { hand(kind, r.AccelerationX(), r.AccelerationY(), r.AccelerationZ(), 0, 0, r.Timestamp()); }
void handGyrometer(const GyrometerReading& r) {
    hand(MEGLER_SENSOR_GYROMETER, r.AngularVelocityX(), r.AngularVelocityY(), r.AngularVelocityZ(), 0, 0, r.Timestamp());
}
void handMagnetometer(const MagnetometerReading& r) {
    hand(MEGLER_SENSOR_MAGNETOMETER, r.MagneticFieldX(), r.MagneticFieldY(), r.MagneticFieldZ(), 0, accuracyOf(r.DirectionalAccuracy()), r.Timestamp());
}
void handOrientation(int kind, const OrientationSensorReading& r) {
    const auto q = r.Quaternion();
    hand(kind, q.X(), q.Y(), q.Z(), q.W(), gOrientationYaw ? accuracyOf(r.YawAccuracy()) : MEGLER_SENSOR_ACCURACY_UNKNOWN, r.Timestamp());
}
void handInclinometer(const InclinometerReading& r) {
    hand(MEGLER_SENSOR_INCLINOMETER, r.PitchDegrees(), r.RollDegrees(), r.YawDegrees(), 0, gInclinometerYaw ? accuracyOf(r.YawAccuracy()) : MEGLER_SENSOR_ACCURACY_UNKNOWN,
         r.Timestamp());
}
void handLight(const LightSensorReading& r) { hand(MEGLER_SENSOR_LIGHT, r.IlluminanceInLux(), 0, 0, 0, 0, r.Timestamp()); }
void handProximity(const ProximitySensorReading& r) {
    const auto distance = r.DistanceInMillimeters();
    hand(MEGLER_SENSOR_PROXIMITY, r.IsDetected() ? 1 : 0, distance ? double(distance.Value()) : -1, 0, 0, 0, r.Timestamp());
}
void handBarometer(const BarometerReading& r) { hand(MEGLER_SENSOR_BAROMETER, r.StationPressureInHectopascals(), 0, 0, 0, 0, r.Timestamp()); }

/// `f` on a reading from Windows' thread: what it throws stays here.
template <class F>
void guarded(F&& f) {
    try {
        f();
    } catch (...) {
    }
}

// ------------------------------------------------------------------ looking

/// The name of the sensor's device, and its maker where Windows says (the device node's DEVPKEY_Device_Manufacturer).
void describe(Found& f, const winrt::hstring& deviceId) {
    static const winrt::hstring instanceKey = L"System.Devices.DeviceInstanceId", makerKey = L"{a45c254e-df1c-4efd-8020-67d146a850e0} 13";
    try {
        const DeviceInformation d = DeviceInformation::CreateFromIdAsync(deviceId, {instanceKey}).get();
        copyText(f.info.name, sizeof(f.info.name), narrow(d.Name()));
        const auto instance = d.Properties().TryLookup(instanceKey);
        const winrt::hstring instanceId = instance ? winrt::unbox_value_or<winrt::hstring>(instance, L"") : winrt::hstring();
        if (!instanceId.empty()) {
            const DeviceInformation device = DeviceInformation::CreateFromIdAsync(instanceId, {makerKey}, DeviceInformationKind::Device).get();
            const auto maker = device.Properties().TryLookup(makerKey);
            if (maker) copyText(f.info.vendor, sizeof(f.info.vendor), narrow(winrt::unbox_value_or<winrt::hstring>(maker, L"")));
        }
    } catch (const winrt::hresult_error&) {
    }
}

template <class S>
void found(int kind, const S& sensor, uint32_t minimum) {
    Found& f = gFound[kind];
    f.there = true;
    f.info.kind = kind;
    f.minimum = minimum;
    f.info.min_interval_ms = int(minimum);
    // (A sensor's DeviceId came with Windows 10 1511: without it the sensor is there, unnamed.)
    winrt::hstring id;
    try {
        id = sensor.DeviceId();
    } catch (const winrt::hresult_error&) {
    }
    if (!id.empty()) describe(f, id);
}

/// Each sensor megler.h names, asked of Windows once; one Windows does not have, or a Windows too old to name it, is not there.
void look() {
    const auto tryIt = [](const char* what, auto&& f) {
        try {
            f();
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("sensors: the %s could not be asked for (0x%08X)", what, unsigned(e.code())));
        }
    };
    tryIt("accelerometer", [] {
        if (auto a = Accelerometer::GetDefault()) {
            gFound[MEGLER_SENSOR_ACCELEROMETER].accelerometer = a;
            found(MEGLER_SENSOR_ACCELEROMETER, a, a.MinimumReportInterval());
        }
    });
    // (Windows 10 1703 and later: the reading types its sensor fusion makes of the accelerometer and the gyrometer.)
    tryIt("gravity", [] {
        if (!ApiInformation::IsMethodPresent(L"Windows.Devices.Sensors.Accelerometer", L"GetDefault", 1)) return;
        if (auto a = Accelerometer::GetDefault(AccelerometerReadingType::Gravity)) {
            gFound[MEGLER_SENSOR_GRAVITY].accelerometer = a;
            found(MEGLER_SENSOR_GRAVITY, a, a.MinimumReportInterval());
        }
    });
    tryIt("linear acceleration", [] {
        if (!ApiInformation::IsMethodPresent(L"Windows.Devices.Sensors.Accelerometer", L"GetDefault", 1)) return;
        if (auto a = Accelerometer::GetDefault(AccelerometerReadingType::Linear)) {
            gFound[MEGLER_SENSOR_LINEAR_ACCELERATION].accelerometer = a;
            found(MEGLER_SENSOR_LINEAR_ACCELERATION, a, a.MinimumReportInterval());
        }
    });
    tryIt("gyrometer", [] {
        if (auto g = Gyrometer::GetDefault()) {
            gFound[MEGLER_SENSOR_GYROMETER].gyrometer = g;
            found(MEGLER_SENSOR_GYROMETER, g, g.MinimumReportInterval());
        }
    });
    tryIt("magnetometer", [] {
        if (auto m = Magnetometer::GetDefault()) {
            gFound[MEGLER_SENSOR_MAGNETOMETER].magnetometer = m;
            found(MEGLER_SENSOR_MAGNETOMETER, m, m.MinimumReportInterval());
        }
    });
    tryIt("orientation sensor", [] {
        if (auto o = OrientationSensor::GetDefault()) {
            gFound[MEGLER_SENSOR_ORIENTATION].orientation = o;
            found(MEGLER_SENSOR_ORIENTATION, o, o.MinimumReportInterval());
        }
    });
    tryIt("relative orientation sensor", [] {
        if (auto o = OrientationSensor::GetDefaultForRelativeReadings()) {
            gFound[MEGLER_SENSOR_RELATIVE_ORIENTATION].orientation = o;
            found(MEGLER_SENSOR_RELATIVE_ORIENTATION, o, o.MinimumReportInterval());
        }
    });
    tryIt("inclinometer", [] {
        if (auto i = Inclinometer::GetDefault()) {
            gFound[MEGLER_SENSOR_INCLINOMETER].inclinometer = i;
            found(MEGLER_SENSOR_INCLINOMETER, i, i.MinimumReportInterval());
        }
    });
    tryIt("light sensor", [] {
        if (auto l = LightSensor::GetDefault()) {
            gFound[MEGLER_SENSOR_LIGHT].light = l;
            found(MEGLER_SENSOR_LIGHT, l, l.MinimumReportInterval());
        }
    });
    tryIt("proximity sensor", [] {
        const auto all = DeviceInformation::FindAllAsync(ProximitySensor::GetDeviceSelector()).get();
        if (all.Size() == 0) return;
        if (auto p = ProximitySensor::FromId(all.GetAt(0).Id())) {
            Found& f = gFound[MEGLER_SENSOR_PROXIMITY];
            f.proximity = p;
            found(MEGLER_SENSOR_PROXIMITY, p, 0);
            if (const auto most = p.MaxDistanceInMillimeters()) f.info.max_range = double(most.Value());
        }
    });
    tryIt("barometer", [] {
        if (auto b = Barometer::GetDefault()) {
            gFound[MEGLER_SENSOR_BAROMETER].barometer = b;
            found(MEGLER_SENSOR_BAROMETER, b, b.MinimumReportInterval());
        }
    });
    gOrientationYaw = ApiInformation::IsPropertyPresent(L"Windows.Devices.Sensors.OrientationSensorReading", L"YawAccuracy");
    gInclinometerYaw = ApiInformation::IsPropertyPresent(L"Windows.Devices.Sensors.InclinometerReading", L"YawAccuracy");
}

std::string summarize() {
    std::string line;
    for (int k = 1; k <= kinds; k++) {
        const Found& f = gFound[k];
        if (!f.there) continue;
        line += format("%s%s \"%s\"%s%s, %u ms at least", line.empty() ? "" : "; ", kindNames[k], f.info.name, f.info.vendor[0] ? " by " : "", f.info.vendor,
                       unsigned(f.minimum));
        if (k == MEGLER_SENSOR_PROXIMITY && f.info.max_range > 0) line += format(", up to %.0f mm", f.info.max_range);
    }
    return line.empty() ? "none" : line;
}

/// Starts the looking once, on the worker.
void lookOnce() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gLooking) return;
        gLooking = true;
    }
    worker().post([] {
        try {
            look();
        } catch (...) {
        }
        shellLog::write("sensors: Windows has " + summarize());
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gLooked = true;
        }
        gLookedChanged.notify_all();
    });
}

bool waitLooked() {
    lookOnce();
    std::unique_lock<std::mutex> lock(gMutex);
    return gLookedChanged.wait_for(lock, lookLimit, [] { return gLooked; });
}

// ------------------------------------------------------------- start, stop

/// On the worker: the sensor's readings at `intervalMs` (0: as often as it can), never below its minimum.
void startOn(int kind, int intervalMs) {
    Found& f = gFound[kind];
    if (!f.there) return;
    const uint32_t interval = intervalMs <= 0 ? f.minimum : std::max(uint32_t(intervalMs), f.minimum);
    switch (kind) {
    case MEGLER_SENSOR_ACCELEROMETER:
    case MEGLER_SENSOR_GRAVITY:
    case MEGLER_SENSOR_LINEAR_ACCELERATION:
        f.accelerometer.ReportInterval(interval);
        if (!f.running)
            f.token = f.accelerometer.ReadingChanged([kind](const Accelerometer&, const AccelerometerReadingChangedEventArgs& e) {
                guarded([&] { handAccelerometer(kind, e.Reading()); });
            });
        if (auto r = f.accelerometer.GetCurrentReading()) handAccelerometer(kind, r);
        break;
    case MEGLER_SENSOR_GYROMETER:
        f.gyrometer.ReportInterval(interval);
        if (!f.running)
            f.token = f.gyrometer.ReadingChanged([](const Gyrometer&, const GyrometerReadingChangedEventArgs& e) { guarded([&] { handGyrometer(e.Reading()); }); });
        if (auto r = f.gyrometer.GetCurrentReading()) handGyrometer(r);
        break;
    case MEGLER_SENSOR_MAGNETOMETER:
        f.magnetometer.ReportInterval(interval);
        if (!f.running)
            f.token = f.magnetometer.ReadingChanged([](const Magnetometer&, const MagnetometerReadingChangedEventArgs& e) { guarded([&] { handMagnetometer(e.Reading()); }); });
        if (auto r = f.magnetometer.GetCurrentReading()) handMagnetometer(r);
        break;
    case MEGLER_SENSOR_ORIENTATION:
    case MEGLER_SENSOR_RELATIVE_ORIENTATION:
        f.orientation.ReportInterval(interval);
        if (!f.running)
            f.token = f.orientation.ReadingChanged([kind](const OrientationSensor&, const OrientationSensorReadingChangedEventArgs& e) {
                guarded([&] { handOrientation(kind, e.Reading()); });
            });
        if (auto r = f.orientation.GetCurrentReading()) handOrientation(kind, r);
        break;
    case MEGLER_SENSOR_INCLINOMETER:
        f.inclinometer.ReportInterval(interval);
        if (!f.running)
            f.token = f.inclinometer.ReadingChanged([](const Inclinometer&, const InclinometerReadingChangedEventArgs& e) { guarded([&] { handInclinometer(e.Reading()); }); });
        if (auto r = f.inclinometer.GetCurrentReading()) handInclinometer(r);
        break;
    case MEGLER_SENSOR_LIGHT:
        f.light.ReportInterval(interval);
        if (!f.running)
            f.token = f.light.ReadingChanged([](const LightSensor&, const LightSensorReadingChangedEventArgs& e) { guarded([&] { handLight(e.Reading()); }); });
        if (auto r = f.light.GetCurrentReading()) handLight(r);
        break;
    case MEGLER_SENSOR_PROXIMITY:
        // (A proximity sensor reports changes, at no interval of anyone's.)
        if (!f.running)
            f.token = f.proximity.ReadingChanged([](const ProximitySensor&, const ProximitySensorReadingChangedEventArgs& e) { guarded([&] { handProximity(e.Reading()); }); });
        if (auto r = f.proximity.GetCurrentReading()) handProximity(r);
        break;
    case MEGLER_SENSOR_BAROMETER:
        f.barometer.ReportInterval(interval);
        if (!f.running)
            f.token = f.barometer.ReadingChanged([](const Barometer&, const BarometerReadingChangedEventArgs& e) { guarded([&] { handBarometer(e.Reading()); }); });
        if (auto r = f.barometer.GetCurrentReading()) handBarometer(r);
        break;
    default: return;
    }
    if (!f.running && !f.saidStart) {
        f.saidStart = true;
        shellLog::write(format("sensors: the %s reports every %u ms (asked %d)", kindNames[kind], unsigned(interval), intervalMs));
    }
    f.running = true;
}

/// On the worker: no more readings, and the interval back to Windows' default.
void stopOn(int kind) {
    Found& f = gFound[kind];
    if (!f.there || !f.running) return;
    f.running = false;
    switch (kind) {
    case MEGLER_SENSOR_ACCELEROMETER:
    case MEGLER_SENSOR_GRAVITY:
    case MEGLER_SENSOR_LINEAR_ACCELERATION:
        f.accelerometer.ReadingChanged(f.token);
        f.accelerometer.ReportInterval(0);
        break;
    case MEGLER_SENSOR_GYROMETER:
        f.gyrometer.ReadingChanged(f.token);
        f.gyrometer.ReportInterval(0);
        break;
    case MEGLER_SENSOR_MAGNETOMETER:
        f.magnetometer.ReadingChanged(f.token);
        f.magnetometer.ReportInterval(0);
        break;
    case MEGLER_SENSOR_ORIENTATION:
    case MEGLER_SENSOR_RELATIVE_ORIENTATION:
        f.orientation.ReadingChanged(f.token);
        f.orientation.ReportInterval(0);
        break;
    case MEGLER_SENSOR_INCLINOMETER:
        f.inclinometer.ReadingChanged(f.token);
        f.inclinometer.ReportInterval(0);
        break;
    case MEGLER_SENSOR_LIGHT:
        f.light.ReadingChanged(f.token);
        f.light.ReportInterval(0);
        break;
    case MEGLER_SENSOR_PROXIMITY: f.proximity.ReadingChanged(f.token); break;
    case MEGLER_SENSOR_BAROMETER:
        f.barometer.ReadingChanged(f.token);
        f.barometer.ReportInterval(0);
        break;
    default: break;
    }
}

} // namespace

SensorHost& SensorHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static SensorHost* const host = new SensorHost();
    return *host;
}

megler_sensor_host SensorHost::callbacks() {
    lookOnce();
    megler_sensor_host host = {};
    host.user = this;
    host.list = &listThunk;
    host.start = &startThunk;
    host.stop = &stopThunk;
    return host;
}

std::string SensorHost::summary() {
    if (!waitLooked()) return "not known: Windows did not answer in time";
    return summarize();
}

int SensorHost::listThunk(void*, megler_sensor_info* info, int max) {
    if (!waitLooked()) {
        shellLog::write("sensors: Windows did not say within 15 seconds what sensors there are: the app is told of none");
        return 0;
    }
    int n = 0;
    for (int k = 1; k <= kinds && n < max; k++)
        if (gFound[k].there) info[n++] = gFound[k].info;
    return n;
}

void SensorHost::startThunk(void*, int kind, int intervalMs) {
    if (kind <= 0 || kind > kinds) return;
    worker().post([kind, intervalMs] { startOn(kind, intervalMs); });
}

void SensorHost::stopThunk(void*, int kind) {
    if (kind <= 0 || kind > kinds) return;
    worker().post([kind] { stopOn(kind); });
}

} // namespace megler::app
