#include "pch.h"

#include "player/PermissionHost.h"

#include "library/Library.h"
#include "player/LocationHost.h"
#include "player/SystemHost.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.UI.Core.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace megler::app::permissionHost {

using winrt::Windows::Devices::Enumeration::DeviceAccessInformation;
using winrt::Windows::Devices::Enumeration::DeviceAccessStatus;
using winrt::Windows::Devices::Enumeration::DeviceClass;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncAction;

namespace {

/* Android 14's dangerous permissions by group, as the framework's megler.Permissions has them. */
const struct {
    const char* permission;
    const char* group;
} kPermissions[] = {
    {"READ_CALENDAR", "calendar"},           {"WRITE_CALENDAR", "calendar"},
    {"READ_CALL_LOG", "call_log"},           {"WRITE_CALL_LOG", "call_log"},
    {"PROCESS_OUTGOING_CALLS", "call_log"},  {"CAMERA", "camera"},
    {"READ_CONTACTS", "contacts"},           {"WRITE_CONTACTS", "contacts"},
    {"GET_ACCOUNTS", "contacts"},            {"ACCESS_FINE_LOCATION", "location"},
    {"ACCESS_COARSE_LOCATION", "location"},  {"ACCESS_BACKGROUND_LOCATION", "location"},
    {"RECORD_AUDIO", "microphone"},          {"READ_PHONE_STATE", "phone"},
    {"READ_PHONE_NUMBERS", "phone"},         {"CALL_PHONE", "phone"},
    {"ANSWER_PHONE_CALLS", "phone"},         {"ADD_VOICEMAIL", "phone"},
    {"USE_SIP", "phone"},                    {"ACCEPT_HANDOVER", "phone"},
    {"BODY_SENSORS", "sensors"},             {"BODY_SENSORS_BACKGROUND", "sensors"},
    {"SEND_SMS", "sms"},                     {"RECEIVE_SMS", "sms"},
    {"READ_SMS", "sms"},                     {"RECEIVE_WAP_PUSH", "sms"},
    {"RECEIVE_MMS", "sms"},                  {"READ_CELL_BROADCASTS", "sms"},
    {"READ_EXTERNAL_STORAGE", "storage"},    {"WRITE_EXTERNAL_STORAGE", "storage"},
    {"READ_MEDIA_IMAGES", "read_media_visual"}, {"READ_MEDIA_VIDEO", "read_media_visual"},
    {"READ_MEDIA_VISUAL_USER_SELECTED", "read_media_visual"}, {"ACCESS_MEDIA_LOCATION", "read_media_visual"},
    {"READ_MEDIA_AUDIO", "read_media_aural"}, {"BLUETOOTH_SCAN", "nearby_devices"},
    {"BLUETOOTH_CONNECT", "nearby_devices"}, {"BLUETOOTH_ADVERTISE", "nearby_devices"},
    {"NEARBY_WIFI_DEVICES", "nearby_devices"}, {"UWB_RANGING", "nearby_devices"},
    {"POST_NOTIFICATIONS", "notifications"}, {"ACTIVITY_RECOGNITION", "activity_recognition"},
};

/* Google Location Accuracy's group, Megler's own: asked of an app that holds the location, as its Google Play services want it. */
constexpr const char* kLocationAccuracy = "location_accuracy";

/* The groups in Android's order, what each lets an app have, and who decides: Windows, Megler's question, or no one. */
struct Kind {
    const char* group;
    const char* what;
    Decider decider;
    const char* question;    // Megler's, after "Allow <app> to "
    const char* explanation; // what it amounts to here
};
const Kind kKinds[] = {
    {"location", "the location", Decider::windows, nullptr, nullptr},
    // (No permission of Android's: the setting Google Play services' network location goes by, asked of an app that holds the
    // location -- megler.LocationAccuracy, which writes the answer into Google's settings, every app's.)
    {kLocationAccuracy, "Google Location Accuracy", Decider::megler, "use Google Location Accuracy?",
     "Google Play services in the app then finds the phone from the Wi-Fi networks around it, as on Android: it sends the networks "
     "it hears, their addresses and signals, to Google, which answers where they are -- within tens of metres, where Windows finds "
     "the phone by the cell network alone. It is one setting for every app, as Android's is; an app's Settings in Megler change it."},
    {"camera", "the camera", Decider::windows, nullptr, nullptr},
    {"microphone", "the microphone", Decider::windows, nullptr, nullptr},
    {"storage", "photos, media and files", Decider::megler, "have the photos, media and files on this phone?",
     "The phone's pictures, music and videos, and the files Android apps keep in their shared storage (/sdcard)."},
    {"read_media_visual", "photos and videos", Decider::megler, "have the photos and videos on this phone?",
     "The phone's Pictures, Videos and the camera's pictures."},
    {"read_media_aural", "music and audio", Decider::megler, "have the music and audio on this phone?", "The phone's Music."},
    {"sms", "text messages", Decider::megler, "read text messages?",
     "The text messages that come in while it runs: a code it waits for comes so. None that came before."},
    {"phone", "the phone's calls", Decider::megler, "know of the phone's calls?",
     "That a call comes in or ends while it runs, and the phone's SIM."},
    {"call_log", "the call history", Decider::megler, "read the call history?",
     "The calls of the phone's history that end while it runs, with their numbers."},
    {"contacts", "contacts", Decider::none, nullptr, nullptr},
    {"calendar", "the calendar", Decider::none, nullptr, nullptr},
    {"sensors", "body sensors", Decider::none, nullptr, nullptr},
    {"nearby_devices", "nearby devices", Decider::none, nullptr, nullptr},
    {"notifications", "notifications", Decider::none, nullptr, nullptr},
    {"activity_recognition", "physical activity", Decider::none, nullptr, nullptr},
};

const Kind* kindOf(const std::string& group) {
    for (const Kind& k : kKinds)
        if (group == k.group) return &k;
    return nullptr;
}

const char* stateName(int state) {
    switch (state) {
    case MEGLER_PERMISSION_GRANTED: return "granted";
    case MEGLER_PERMISSION_DENIED: return "denied";
    case MEGLER_PERMISSION_DENIED_FIXED: return "denied-fixed";
    default: return "";
    }
}

/// `f` on a thread of its own in the multithreaded apartment, waited for: Windows' objects are asked from the app's threads,
/// which have none (as player/LocationHost.cpp does it).
template <class F>
void inMta(F&& f) {
    std::thread t([&f] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            f();
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("permissions: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
        } catch (...) {
            shellLog::write("permissions: an exception");
        }
        winrt::uninit_apartment();
    });
    t.join();
}

int ofAccess(DeviceAccessStatus s) {
    switch (s) {
    case DeviceAccessStatus::Allowed: return MEGLER_PERMISSION_GRANTED;
    case DeviceAccessStatus::DeniedByUser:
    case DeviceAccessStatus::DeniedBySystem: return MEGLER_PERMISSION_DENIED_FIXED;
    default: return MEGLER_PERMISSION_UNDECIDED;
    }
}

/// Windows' location consent of this package, nothing asked: by the class of its consent, as Windows' own documentation
/// checks it without asking (Geolocator's RequestAccessAsync would ask).
DeviceAccessStatus locationConsent() {
    const winrt::guid consentClass{0xBFA794E4, 0xF964, 0x4FDB, {0x90, 0xF6, 0x51, 0x05, 0x6B, 0xFE, 0x4B, 0x44}};
    return DeviceAccessInformation::CreateFromDeviceClassId(consentClass).CurrentStatus();
}

/// Windows' consent of this package for a group it decides, nothing asked. The location refused by the system -- the phone's
/// location switched off, or every app's -- is no refusal of the app's: Android grants or refuses an app's location
/// permission whether the location is on or off, and an app told the permission is its own then asks the user to switch the
/// location on (megler.LocationService tells it the location is off). Taken for a refusal for good, it left the app nothing
/// to ask and nothing to say but that the location was not to be had (VTB, 0.1.0.671: "no one asked").
int windowsNow(const std::string& group) {
    if (group == "location") {
        double latitude = 0, longitude = 0;
        if (settings::location::setPlace(latitude, longitude)) return MEGLER_PERMISSION_GRANTED;
        const DeviceAccessStatus s = locationConsent();
        return s == DeviceAccessStatus::DeniedBySystem ? MEGLER_PERMISSION_GRANTED : ofAccess(s);
    }
    return ofAccess(DeviceAccessInformation::CreateFromDeviceClass(group == "camera" ? DeviceClass::VideoCapture : DeviceClass::AudioCapture).CurrentStatus());
}

/// What Windows says of a group it decides, in words, for the log and the app's settings.
std::string windowsSaidNow(const std::string& group) {
    if (group == "location") {
        double latitude = 0, longitude = 0;
        if (settings::location::setPlace(latitude, longitude)) return "Allowed: a place set in Megler's Settings stands in for the phone's";
        switch (locationConsent()) {
        case DeviceAccessStatus::Allowed: return "Allowed by Windows";
        case DeviceAccessStatus::DeniedByUser: return "Not allowed by Windows (Settings > Privacy > Location)";
        case DeviceAccessStatus::DeniedBySystem: return "The phone's location is switched off: the app may have it once it is on";
        default: return "Windows asks when the app first wants it";
        }
    }
    const int state = windowsNow(group);
    return state == MEGLER_PERMISSION_GRANTED ? "Allowed by Windows" : state == MEGLER_PERMISSION_DENIED_FIXED ? "Not allowed by Windows" : "Windows asks when the app first wants it";
}

/// Each answer of Windows' said once in the log, as it is first given and as it changes: why an app's location permission is
/// what it is, even when nothing asks (a request of the app's answered from what Windows already said).
void sayWindows(const std::string& group, const std::string& package) {
    static std::mutex mutex;
    static std::string said;
    std::string now;
    inMta([&] { now = windowsSaidNow(group); });
    const std::string line = group + ": " + now;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (said.find("|" + line + "|") != std::string::npos) return;
        said += "|" + line + "|";
    }
    shellLog::write("permissions: " + package + "'s " + group + ", as Windows has it now: " + now);
}

/// Whether the phone has a camera (or a microphone): one it has not, Windows has nothing to ask about.
bool hasDevice(const std::string& group) {
    return DeviceInformation::FindAllAsync(group == "camera" ? DeviceClass::VideoCapture : DeviceClass::AudioCapture).get().Size() > 0;
}

/// MediaCapture's InitializeAsync for the camera or the microphone alone, on the UI thread, where Windows asks the user the
/// first time (as player/CameraHost.cpp and MicrophoneHost.cpp open them); closed again at once. S_OK when it opened.
HRESULT openOnce(const std::string& group) {
    using namespace winrt::Windows::Media::Capture;
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    try {
        dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().Dispatcher();
    } catch (const winrt::hresult_error&) {
    }
    if (!dispatcher) return E_FAIL;
    // The MediaCapture is the state's, closed on the UI thread once the question is answered -- not inside its own
    // InitializeAsync's Completed: Sberbank's process ended there (0.1.0.671, an access violation in the handler's Close,
    // called from Windows.Media through combase as Windows' own question was answered), the capture it held null by then.
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        HRESULT hr = E_FAIL;
        MediaCapture capture{nullptr};
    };
    auto state = std::make_shared<State>();
    const bool camera = group == "camera";
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [state, camera] {
        auto finish = [state](HRESULT hr) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->hr = hr;
            state->done = true;
            state->cv.notify_all();
        };
        try {
            state->capture = MediaCapture();
            MediaCaptureInitializationSettings settings;
            settings.StreamingCaptureMode(camera ? StreamingCaptureMode::Video : StreamingCaptureMode::Audio);
            IAsyncAction op = state->capture.InitializeAsync(settings);
            op.Completed([finish](const IAsyncAction& done, AsyncStatus status) {
                HRESULT hr = S_OK;
                if (status != AsyncStatus::Completed) {
                    hr = E_FAIL;
                    try {
                        if (done) hr = done.ErrorCode();
                    } catch (...) {
                    }
                    if (hr == S_OK) hr = E_FAIL;
                }
                finish(hr);
            });
        } catch (const winrt::hresult_error& e) {
            finish(e.code());
        }
    });
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->cv.wait_for(lock, std::chrono::seconds(170), [&state] { return state->done; })) hr = state->hr;
    }
    // (Closed where it was made; a question still open after the wait is left to Windows, the capture closed with it.)
    if (hr != HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        try {
            dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [state] {
                try {
                    if (state->capture) state->capture.Close();
                } catch (...) {
                }
                state->capture = nullptr;
            });
        } catch (const winrt::hresult_error&) {
        }
    }
    return hr;
}

/// Windows' own question for a group it decides, when it has not been answered; then what Windows says.
int askWindows(const std::string& group) {
    int now = MEGLER_PERMISSION_UNDECIDED;
    inMta([&] { now = windowsNow(group); });
    if (now != MEGLER_PERMISSION_UNDECIDED) return now;
    if (group == "location") {
        // The location host's own question (Geolocator.RequestAccessAsync on the UI thread), which it remembers for the app.
        megler_location_host host = LocationHost::shared().callbacks();
        const int access = host.access(host.user, true);
        inMta([&] { now = windowsNow(group); });
        if (now != MEGLER_PERMISSION_UNDECIDED) return now;
        // (The location off is the app's to have once it is on, as windowsNow says.)
        return access == MEGLER_LOCATION_ON || access == MEGLER_LOCATION_NONE || access == MEGLER_LOCATION_OFF ? MEGLER_PERMISSION_GRANTED
               : access == MEGLER_LOCATION_DENIED                                                            ? MEGLER_PERMISSION_DENIED_FIXED
                                                                                                             : MEGLER_PERMISSION_UNDECIDED;
    }
    bool device = true;
    inMta([&] { device = hasDevice(group); });
    if (!device) return MEGLER_PERMISSION_GRANTED;
    HRESULT hr = E_FAIL;
    inMta([&] { hr = openOnce(group); });
    inMta([&] { now = windowsNow(group); });
    if (now != MEGLER_PERMISSION_UNDECIDED) return now;
    // A phone that asks nothing (or has asked already) and opened it: allowed; one that refused it: Windows' no.
    if (hr == S_OK) return MEGLER_PERMISSION_GRANTED;
    if (hr == E_ACCESSDENIED) return MEGLER_PERMISSION_DENIED_FIXED;
    shellLog::write(format("permissions: %s could not be asked of Windows (0x%08X)", group.c_str(), unsigned(hr)));
    return MEGLER_PERMISSION_UNDECIDED;
}

/// Megler's question for a group it asks about itself; what is kept from then on. Under the flag allow-permissions
/// (LocalState\flags) every such question is answered Allow with no one asked, for a run no one watches.
int askUser(const Kind& kind, const std::string& package, int state) {
    if (files::exists(platform::localStateDirectory() + L"\\flags\\allow-permissions")) {
        shellLog::write(std::string("permissions: ") + kind.group + " allowed under the flag allow-permissions");
        keep(package, kind.group, MEGLER_PERMISSION_GRANTED);
        return MEGLER_PERMISSION_GRANTED;
    }
    const std::string app = systemHost::runningLabel();
    std::string message = kind.explanation;
    if (state == MEGLER_PERMISSION_DENIED)
        message += " " + app + " asks again after a no: a second no is for good (its App Settings in Megler change it).";
    const int button = systemHost::askUser("Allow " + app + " to " + kind.question, message, {"Don't Allow", "Allow"});
    if (button < 0) {
        shellLog::write(std::string("permissions: the question of ") + kind.group + " was put away unanswered");
        return state;
    }
    const int answer = button == 1 ? MEGLER_PERMISSION_GRANTED : state == MEGLER_PERMISSION_DENIED ? MEGLER_PERMISSION_DENIED_FIXED : MEGLER_PERMISSION_DENIED;
    keep(package, kind.group, answer);
    return answer;
}

} // namespace

int MEGLER_CB thunk(void*, const char* name, bool ask) {
    const std::string group = name ? name : "";
    const Kind* kind = kindOf(group);
    if (!kind || kind->decider == Decider::none) return MEGLER_PERMISSION_GRANTED;
    const std::string package = systemHost::runningPackage();
    int state = MEGLER_PERMISSION_UNDECIDED;
    if (kind->decider == Decider::windows) {
        if (ask) {
            state = askWindows(group);
        } else {
            inMta([&] { state = windowsNow(group); });
        }
        if (group == "location") sayWindows(group, package);
    } else {
        state = kept(package, group);
        if (ask && !package.empty() && state != MEGLER_PERMISSION_GRANTED && state != MEGLER_PERMISSION_DENIED_FIXED) state = askUser(*kind, package, state);
    }
    if (ask)
        shellLog::write("permissions: " + package + " asked for " + group + ": " +
                        (state == MEGLER_PERMISSION_UNDECIDED ? std::string("undecided") : stateName(state)) +
                        (kind->decider == Decider::windows ? " (Windows')" : ""));
    return state;
}

std::vector<Group> groupsOf(const ApkReport& report) {
    std::vector<Group> out;
    for (const Kind& kind : kKinds) {
        bool asked = false;
        // (Google Location Accuracy is the app's to decide where it holds the location: its Google Play services find the phone so.)
        const bool accuracy = !strcmp(kind.group, kLocationAccuracy);
        for (const auto& p : kPermissions)
            if (!strcmp(p.group, accuracy ? "location" : kind.group) && !(accuracy && !strcmp(p.permission, "ACCESS_BACKGROUND_LOCATION")) &&
                report.permissionsKnown && report.asks(std::string("android.permission.") + p.permission))
                asked = true;
        if (asked) out.push_back({kind.group, kind.what, kind.decider});
    }
    return out;
}

int kept(const std::string& package, const std::string& group) {
    if (package.empty()) return MEGLER_PERMISSION_UNDECIDED;
    const std::string value = settings::app(package, "megler.permission." + group).value_or("");
    if (value == "granted") return MEGLER_PERMISSION_GRANTED;
    if (value == "denied") return MEGLER_PERMISSION_DENIED;
    if (value == "denied-fixed") return MEGLER_PERMISSION_DENIED_FIXED;
    return MEGLER_PERMISSION_UNDECIDED;
}

void keep(const std::string& package, const std::string& group, int state) {
    if (package.empty()) return;
    const char* name = stateName(state);
    settings::setApp(package, "megler.permission." + group, name[0] ? std::optional<std::string>(name) : std::nullopt);
}

void forgetAll(const std::string& package) {
    if (package.empty()) return;
    const int forgotten = settings::eraseApp(package, "megler.permission.");
    shellLog::write("permissions: " + package + "'s data is cleared: " +
                    (forgotten ? std::to_string(forgotten) + " answer(s) forgotten, the app asks again" : std::string("no answers kept to forget")));
}

int windows(const std::string& group) {
    int state = MEGLER_PERMISSION_UNDECIDED;
    inMta([&] { state = windowsNow(group); });
    return state;
}

std::string windowsSaid(const std::string& group) {
    std::string said;
    inMta([&] { said = windowsSaidNow(group); });
    return said;
}

std::wstring windowsSettingsPage(const std::string& group) {
    if (group == "location") return L"ms-settings:privacy-location";
    if (group == "camera") return L"ms-settings:privacy-webcam";
    if (group == "microphone") return L"ms-settings:privacy-microphone";
    return L"";
}

} // namespace megler::app::permissionHost
