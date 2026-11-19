#include "pch.h"

#include "ui/AndroidSettings.h"

#include "library/Library.h"
#include "shell/AppPackage.h"
#include "shell/BundledApks.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/SeparateApps.h"

#include <memory>
#include <optional>

namespace megler::app::ui::androidSettings {

namespace {

constexpr const char* package = "com.android.settings";
constexpr const char* libraryId = "platform-AndroidSettings.apk";

/// A failed install, or a copy that could not be made, is tried again after this long.
constexpr ULONGLONG retryMs = 60000;

ULONGLONG gInstallAsked = 0, gCopyAsked = 0;
bool gCopying = false;
bool gLoggedDone = false;

std::wstring bundleFile() { return platform::installDirectory() + L"\\platform-apps\\AndroidSettings.apk"; }

std::optional<ApkEntry> entry() {
    for (const ApkEntry& e : Library::instance().entries())
        if (e.report.package == package) return e;
    return std::nullopt;
}

/// The package's APK put in the library, on a worker thread.
void copyToLibrary(AppShell& shell) {
    if (gCopying || bundledApks::copying()) return;
    const ULONGLONG now = GetTickCount64();
    if (gCopyAsked && now - gCopyAsked < retryMs) return;
    gCopyAsked = now;
    gCopying = true;
    const std::wstring source = bundleFile(), target = Library::instance().directory() + L"\\" + widen(libraryId);
    auto copied = std::make_shared<bool>(false);
    shell.runInBackground([source, target, copied] { *copied = bundledApks::copy(source, target, "Android Settings"); },
                          [copied] {
                              gCopying = false;
                              if (!*copied) {
                                  shellLog::write("android settings: could not be put in the library from Megler's package");
                                  return;
                              }
                              shellLog::write("android settings: put in the library from Megler's package");
                              Library::instance().added(libraryId);
                          });
}

} // namespace

void tick(AppShell& shell) {
    // (An app's own package runs its app alone; and a Megler without it in its package has nothing to install.)
    if (appPackage::isSingleApp() || !files::exists(bundleFile())) return;
    if (settings::megler(settings::androidSettings).value_or("") == "installed") return;
    if (separateApps::status(package).installed == separateApps::Installed::yes) {
        settings::setMegler(settings::androidSettings, "installed");
        if (!gLoggedDone) shellLog::write("android settings: installed as an app of its own");
        gLoggedDone = true;
        return;
    }
    // Once Windows has listed the phone's apps, while nothing else is being installed or copied, and once the question where
    // Megler's apps are kept was answered (as ui/GooglePlay.cpp waits).
    const bool listed = separateApps::list() == separateApps::List::ready && separateApps::listProblem().empty();
    if (!listed || !separateApps::working().empty() || gCopying || bundledApks::copying() || separateApps::placePutOff()) return;
    const auto e = entry();
    if (!e) {
        copyToLibrary(shell);
        return;
    }
    if (!e->analysed || !e->report.ready()) return;
    const separateApps::Status s = separateApps::status(package);
    if (s.queued || s.installed != separateApps::Installed::no) return;
    // Refused for the certificate of Megler's apps' packages (desktop Windows until the user trusts it) or for access (the
    // phone's interop unlock not complete): left to the user, who installs it from the Library once that is done.
    if (s.step.find("0x800b0109") != std::string::npos || s.accessDenied) return;
    const ULONGLONG now = GetTickCount64();
    if (gInstallAsked && now - gInstallAsked < retryMs) return;
    gInstallAsked = now;
    shellLog::write("android settings: installing " + e->title() + " as an app of its own");
    separateApps::install(shell, e->id);
}

} // namespace megler::app::ui::androidSettings
