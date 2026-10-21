// What a Java app asks of the phone beyond its window: megler_system_host (core/include/megler.h). Its calls arrive on the
// app's threads and return at once; what they ask is done on the UI thread.
//   open_url     Windows opens the link (Launcher.LaunchUriAsync): the browser, the mail app, the phone app.
//   share_text   Windows' share sheet with the text (DataTransferManager).
//   install_apk  Megler's installer (ui/AppShell.h, installApkPath), which asks the user. An app's own package hands the
//                APK to Megler: copied into the shared folder's incoming\, then Megler opened with megler:install
//                (App.cpp, OnActivated).
//   launch_app   The app's own package, if it is installed (megler:run, with that package as the target); else Megler with
//                megler:run, which plays it when nothing has run yet in its process.
#pragma once

#include "megler.h"

#include <functional>
#include <string>

namespace megler::app::systemHost {

struct Hooks {
    /// UI thread: an APK at a path of Megler's own that the app (`from`, its name) wants installed.
    std::function<void(const std::wstring& path, const std::string& name, const std::string& from)> install;
    /// UI thread: another app could not be started; `why` says so in a sentence.
    std::function<void(const std::string& why)> cannotLaunch;
};

/// UI thread, once: where the app's requests go from now on.
void attach(Hooks hooks);
/// The callbacks for megler_run_config.system, with nothing as their user.
megler_system_host callbacks();
/// The app that runs, for saying who asks (session::start).
void setRunningApp(const std::string& label, const std::string& package);
/// UI thread: another app the phone has started -- its own package if it has one, else Megler with it (megler:run). With an
/// activity's class, that activity of it (megler:run's activity; its own package starts it, ui/AppShell.cpp).
void launch(const std::string& package, const std::string& activity = std::string());
/// The family name of Megler's own package, and of an app's own package.
std::wstring meglerFamilyName();
std::wstring appFamilyName(const std::string& package);

} // namespace megler::app::systemHost
