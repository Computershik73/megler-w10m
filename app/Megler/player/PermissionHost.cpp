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

const char* stateName(int state) {
    switch (state) {
    case MEGLER_PERMISSION_GRANTED: return "granted";
    case MEGLER_PERMISSION_DENIED: return "denied";
    case MEGLER_PERMISSION_DENIED_FIXED: return "denied-fixed";
    default: return "undecided";
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

bool allowedByFlag() { return files::exists(platform::localStateDirectory() + L"\\flags\\allow-permissions"); }

/// Megler's question about a group: for a group Megler asks about by default (or one the user said no to once), what is kept
/// from then on, a second no for good; for one asked every time, this run's answer alone (`state` being this run's so far).
/// Under the flag allow-permissions (LocalState\flags) every such question is answered Allow with no one asked, for a run no
/// one watches. Back puts the question away and changes nothing.
int askUser(const Group& group, const std::string& package, int state, bool everyTime) {
    int answer = MEGLER_PERMISSION_GRANTED;
    if (allowedByFlag()) {
        shellLog::write("permissions: " + group.name + " allowed under the flag allow-permissions");
    } else {
        const std::string app = systemHost::runningLabel();
        std::string message = explanationOf(group.name);
        if (everyTime)
            message += " Asked each time " + app + " starts and wants it: its App Permissions in Megler change that.";
        else if (state == MEGLER_PERMISSION_DENIED)
            message += " " + app + " asks again after a no: a second no is for good (its App Permissions in Megler change it).";
        if (group.decider == Decider::windows) message += " Windows asks too, the first time.";
        const int button = systemHost::askUser(questionOf(group.name, app), message, {"Don't Allow", "Allow"});
        if (button < 0) {
            shellLog::write("permissions: the question of " + group.name + " was put away unanswered");
            return state;
        }
        answer = button == 1 ? MEGLER_PERMISSION_GRANTED : state == MEGLER_PERMISSION_DENIED ? MEGLER_PERMISSION_DENIED_FIXED : MEGLER_PERMISSION_DENIED;
    }
    if (everyTime)
        rememberThisRun(package, group.name, answer);
    else
        keep(package, group.name, answer);
    return answer;
}

} // namespace

int MEGLER_CB thunk(void*, const char* name, bool ask) {
    const std::string group = name ? name : "";
    const Group* g = groupNamed(group);
    if (!g) return MEGLER_PERMISSION_GRANTED;
    const std::string package = systemHost::runningPackage();
    const int said = kept(package, group);
    int run = said == EVERY_TIME ? thisRun(package, group) : MEGLER_PERMISSION_UNDECIDED;
    // Windows is asked what it says only where that counts: a group it decides, the user's own no not standing before it.
    auto windowsCounts = [&] {
        return g->decider == Decider::windows && said != MEGLER_PERMISSION_DENIED && said != MEGLER_PERMISSION_DENIED_FIXED &&
               (said != EVERY_TIME || run == MEGLER_PERMISSION_GRANTED);
    };
    int windowsState = MEGLER_PERMISSION_UNDECIDED;
    if (windowsCounts()) inMta([&] { windowsState = windowsNow(group); });
    int state = decide(g->decider, said, windowsState, run);
    if (ask && !package.empty()) {
        if (said == EVERY_TIME && run != MEGLER_PERMISSION_GRANTED && run != MEGLER_PERMISSION_DENIED_FIXED) {
            // Asked every time: Megler's question, this run's; then Windows' own for a group it decides.
            run = askUser(*g, package, run, true);
            if (windowsCounts()) windowsState = askWindows(group);
        } else if (g->decider == Decider::windows) {
            if (windowsCounts() && windowsState == MEGLER_PERMISSION_UNDECIDED) windowsState = askWindows(group);
        } else if (state == MEGLER_PERMISSION_UNDECIDED || state == MEGLER_PERMISSION_DENIED) {
            askUser(*g, package, state, false);
        }
        state = decide(g->decider, kept(package, group), windowsState, run);
    }
    if (group == "location" && windowsCounts()) sayWindows(group, package);
    if (ask)
        shellLog::write("permissions: " + package + " asked for " + group + ": " + stateName(state) +
                        (said == MEGLER_PERMISSION_DENIED_FIXED && g->decider != Decider::megler ? " (Don't allow, in its App Permissions)"
                         : said == EVERY_TIME                                                    ? " (asked every time)"
                         : g->decider == Decider::windows                                        ? " (Windows')"
                                                                                                 : ""));
    return state;
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
