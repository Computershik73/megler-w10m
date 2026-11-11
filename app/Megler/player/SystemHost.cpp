#include "pch.h"

#include "player/SystemHost.h"

#include "library/PhoneAppsList.h"
#include "player/PermissionHost.h"
#include "player/PickerHost.h"

#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.ApplicationModel.DataTransfer.h>

#include <chrono>
#include <condition_variable>
#include <memory>

namespace megler::app::systemHost {

using namespace winrt;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::System::Launcher;
using winrt::Windows::System::LauncherOptions;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

namespace {

std::mutex gMutex; // everything below
CoreDispatcher gDispatcher{nullptr};
Hooks gHooks;
std::string gLabel = "The app", gPackage;
std::string gShared; // what share_text was last given, for the share sheet's request

void onUi(std::function<void()> f) {
    CoreDispatcher d{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        d = gDispatcher;
    }
    if (d) d.RunAsync(CoreDispatcherPriority::Normal, [f = std::move(f)] { f(); });
}

fire_and_forget openUri(std::wstring uri) {
    try {
        const bool ok = co_await Launcher::LaunchUriAsync(Uri(uri));
        shellLog::write("system: " + narrow(uri) + (ok ? " opened" : " was not opened: Windows has nothing for it"));
    } catch (const hresult_error& e) {
        shellLog::write(format("system: %s could not be opened (0x%08X)", narrow(uri).c_str(), unsigned(e.code().value)));
    }
}

/// A megler: link opened in the package of that family name; `then` gets whether Windows did.
fire_and_forget openIn(std::wstring family, std::wstring uri, std::function<void(bool)> then) {
    bool ok = false;
    try {
        LauncherOptions options;
        options.TargetApplicationPackageFamilyName(family);
        ok = co_await Launcher::LaunchUriAsync(Uri(uri), options);
    } catch (const hresult_error& e) {
        shellLog::write(format("system: %s could not be opened in %s (0x%08X)", narrow(uri).c_str(), narrow(family).c_str(), unsigned(e.code().value)));
    }
    shellLog::write("system: " + narrow(uri) + (ok ? " opened in " : " not opened in ") + narrow(family));
    if (then) then(ok);
}

std::wstring escaped(const std::string& s) { return std::wstring(Uri::EscapeComponent(hs(s))); }

void openUrlThunk(void*, const char* uri) {
    const std::wstring u = widen(uri ? uri : "");
    onUi([u] { openUri(u); });
}

void shareTextThunk(void*, const char* text, const char*) {
    const std::string t = text ? text : "";
    onUi([t] {
        using namespace winrt::Windows::ApplicationModel::DataTransfer;
        static bool hooked = false;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gShared = t;
        }
        try {
            auto manager = DataTransferManager::GetForCurrentView();
            if (!hooked) {
                hooked = true;
                manager.DataRequested([](auto&&, DataRequestedEventArgs const& e) {
                    std::string shared, label;
                    {
                        std::lock_guard<std::mutex> lock(gMutex);
                        shared = gShared;
                        label = gLabel;
                    }
                    e.Request().Data().Properties().Title(hs(label));
                    e.Request().Data().SetText(hs(shared));
                });
            }
            DataTransferManager::ShowShareUI();
            shellLog::write("system: the share sheet, with what the app shares");
        } catch (const hresult_error& e) {
            shellLog::write(format("system: no share sheet (0x%08X)", unsigned(e.code().value)));
        }
    });
}

/// The hooks as they are now, for calling outside the lock.
Hooks hooks() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gHooks;
}

void cannotLaunch(const std::string& why) {
    if (auto h = hooks(); h.cannotLaunch) h.cannotLaunch(why);
}

bool installApkThunk(void*, const char* hostPath, const char* name) {
    std::wstring path = widen(hostPath ? hostPath : "");
    std::string file = name && name[0] ? name : "app.apk";
    std::string from;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        from = gLabel;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        // A session of several files: one app's base and its splits. Megler's installer takes one APK or a bundle file.
        const auto inside = files::list(path, L"*.apk");
        if (inside.size() != 1) {
            onUi([from, file] {
                cannotLaunch(from + " wants to install " + file + " in several APKs at once, which Megler cannot take that way yet: "
                                                                 "install its .apks, .xapk or .apkm, or the APK, from the file manager.");
            });
            return false;
        }
        file = narrow(inside[0]);
        path += L"\\" + inside[0];
    }
    if (!appPackage::isSingleApp()) {
        onUi([path, file, from] {
            if (auto h = hooks(); h.install) h.install(path, file, from);
        });
        return true;
    }
    // An app's own package: the APK to the shared folder's incoming\, then Megler opened on it.
    std::thread([path, file] {
        const std::wstring shared = appPackage::sharedFolder();
        if (shared.empty()) {
            shellLog::write("system: no shared folder to hand " + file + " to Megler through");
            return;
        }
        const std::wstring incoming = shared + L"\\incoming";
        files::makeDirectories(incoming);
        if (!files::copy(path, incoming + L"\\" + widen(file))) {
            shellLog::write("system: " + file + " could not be handed to Megler (the copy into the shared folder failed)");
            phoneAppsList::installOutcome(file, "failed", "The APK could not be handed to Megler: the copy into the shared folder failed.");
            return;
        }
        const std::wstring uri = appPackage::linkScheme() + L":install?name=" + escaped(file);
        onUi([uri] { openIn(meglerFamilyName(), uri, nullptr); });
    }).detach();
    return true;
}

bool uninstallAppThunk(void*, const char* package) {
    const std::string p = package ? package : "";
    if (p.empty() || p.find_first_of("\\/:?&=%") != std::string::npos) return false;
    std::string from;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        from = gLabel;
    }
    if (!appPackage::isSingleApp()) {
        onUi([p, from] {
            if (auto h = hooks(); h.uninstall) h.uninstall(p, from);
        });
        return true;
    }
    // An app's own package: Megler, which has the library and the packages, is opened on it.
    const std::wstring uri = appPackage::linkScheme() + L":uninstall?package=" + escaped(p);
    onUi([uri] { openIn(meglerFamilyName(), uri, nullptr); });
    return true;
}

bool launchAppThunk(void*, const char* package, const char* activity) {
    const std::string p = package ? package : "", a = activity ? activity : "";
    if (p.empty()) return false;
    onUi([p, a] { launch(p, a); });
    return true;
}

} // namespace

void attach(Hooks hooks) {
    std::lock_guard<std::mutex> lock(gMutex);
    gDispatcher = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
    gHooks = std::move(hooks);
    pickerHost::attach(gDispatcher);
}

megler_system_host callbacks() {
    megler_system_host host = {};
    host.open_url = &openUrlThunk;
    host.share_text = &shareTextThunk;
    host.install_apk = &installApkThunk;
    host.launch_app = &launchAppThunk;
    host.uninstall_app = &uninstallAppThunk;
    host.permission = &permissionHost::thunk;
    host.pick = &pickerHost::thunk;
    return host;
}

void setRunningApp(const std::string& label, const std::string& package) {
    std::lock_guard<std::mutex> lock(gMutex);
    gLabel = label.empty() ? std::string("The app") : label;
    gPackage = package;
}

std::string runningLabel() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gLabel;
}

std::string runningPackage() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gPackage;
}

int askUser(const std::string& title, const std::string& message, const std::vector<std::string>& buttons) {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        int button = -1;
    };
    auto state = std::make_shared<State>();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gDispatcher || !gHooks.ask) return -1;
    }
    onUi([state, title, message, buttons] {
        auto finish = [state](int button) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->button = button;
            state->done = true;
            state->cv.notify_all();
        };
        if (auto h = hooks(); h.ask) h.ask(title, message, buttons, finish);
        else finish(-1);
    });
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, std::chrono::minutes(10), [&state] { return state->done; })) {
        shellLog::write("system: no answer to \"" + title + "\" within ten minutes: taken as put away");
        return -1;
    }
    return state->button;
}

std::wstring meglerFamilyName() { return appPackage::meglerFamilyName(); }

void openMegler(const std::wstring& what) { openIn(meglerFamilyName(), appPackage::linkScheme() + L":" + what, nullptr); }

std::wstring appFamilyName(const std::string& package) {
    char identity[64];
    if (!megler_appx_app_identity(package.c_str(), identity, sizeof(identity))) return L"";
    return widen(identity) + L"_" + std::wstring(winrt::Windows::ApplicationModel::Package::Current().Id().PublisherId());
}

void launch(const std::string& package, const std::string& activity) {
    // The activity's class goes with it: microG's sign-in, which Play Store's AccountManager asks for, is not microG's launcher.
    const std::wstring uri = appPackage::linkScheme() + L":run?package=" + escaped(package) + (activity.empty() ? L"" : L"&activity=" + escaped(activity));
    const std::wstring own = appFamilyName(package);
    shellLog::write("system: starting " + package + (activity.empty() ? "" : "/" + activity));
    // Its own package first; without one, Megler, which plays it from its library.
    openIn(own, uri, [package, uri](bool ok) {
        if (ok) return;
        if (appPackage::isSingleApp()) {
            openIn(meglerFamilyName(), uri, [package](bool started) {
                if (!started) cannotLaunch(package + " is not an app of the phone's own, and Megler could not be opened.");
            });
            return;
        }
        cannotLaunch(package + " runs in Megler only, which runs one app at a time. Install it as an app of the phone's own (its page "
                               "in the Library: Install), and a launcher can start it.");
    });
}

} // namespace megler::app::systemHost
