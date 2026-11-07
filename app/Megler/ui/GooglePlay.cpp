#include "pch.h"

#include "ui/GooglePlay.h"

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

namespace megler::app::ui::googlePlay {

namespace {

constexpr const char* gmsPackage = "com.google.android.gms";
constexpr const char* storePackage = "com.android.vending";

struct Bundled {
    const char* package;
    const wchar_t* file;      // in the package's bitgapps folder, and the library's copy is bitgapps-<file>
    ULONGLONG installAsked;   // when an install of it was last asked for (GetTickCount64), 0 never
    ULONGLONG restoreAsked;   // when its copy was last put back in the library
};
Bundled gBundled[] = {{gmsPackage, L"PrebuiltGmsCore.apk", 0, 0}, {storePackage, L"Phonesky.apk", 0, 0}};

/// A failed install, or a copy that could not be made, is tried again after this long.
constexpr ULONGLONG retryMs = 60000;

bool gAsking = false;   // the question is up
bool gCopying = false;  // a copy of the bundle's is being put in the library

std::wstring bundleFile(const wchar_t* name) { return platform::installDirectory() + L"\\bitgapps\\" + name; }

/// Google's own (the registered app, else the portal's list says its package is installed), or Google Play Store.
bool installed(const std::string& package) {
    if (package == gmsPackage && appPackage::hasGoogleServices()) return true;
    return separateApps::status(package).installed == separateApps::Installed::yes;
}

/// The library's entry of Google's own Google Play services (not microG's) or of Google Play Store.
std::optional<ApkEntry> entryOf(const std::string& package) {
    for (const ApkEntry& e : Library::instance().entries())
        if (e.report.package == package && !(package == gmsPackage && e.report.microg)) return e;
    return std::nullopt;
}

/// The phone's memory, for the question's word on Google Play Store's speed; 0 when Windows does not say.
uint64_t phoneMemory() {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
}

void answer(bool wanted) {
    settings::setMegler(settings::googlePlay, wanted ? "yes" : "no");
    // Not wanted: every app with microG (Settings can't choose Google's own until it is installed). Wanted: Google's own.
    settings::setMegler(settings::googleServices, wanted ? "google" : "microg");
    shellLog::write(wanted ? "google play: wanted at the first start: Megler installs its Google Play services and Google Play Store"
                           : "google play: not wanted at the first start: apps run with microG");
}

void ask(AppShell& shell) {
    gAsking = true;
    const uint64_t memory = phoneMemory();
    // (Windows keeps some of a phone's memory for itself: a phone of 1 GB says a little less.)
    const bool littleMemory = memory && memory <= (uint64_t(1280) << 20);
    AppShell::DialogSpec spec;
    spec.title = "Google Play?";
    spec.message =
        "Megler brings Google Play services and Google Play Store. With them, apps sign in with Google and install from Google "
        "Play. Megler installs both now, and apps run with microG until Google Play services is installed.\n\n"
        "Without them, apps run with microG, which takes far less memory. Google Play services can still be installed from "
        "the Library later.\n\n"
        "On phones with 1 GB of memory or less, Google Play Store can be slow. A store such as Aurora Store installs apps "
        "from Google Play without it." +
        std::string(littleMemory ? format(" This phone has %s.", formattedByteCount(memory).c_str()) : "");
    spec.buttons = {"Use microG", "Install Google Play"};
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    shell.dialog(std::move(spec), [weak](int button, std::string) {
        gAsking = false;
        // Dismissed (Back, a tap beside it): asked again at the next poll.
        if (button < 0) return;
        answer(button == 1);
        if (auto self = weak.lock()) self->refreshChrome();
    });
}

/// The bundle's copy put in the library again, on a worker thread: it had gone from there (removed, or lost to an update).
void restore(AppShell& shell, Bundled& b) {
    if (gCopying || !files::exists(bundleFile(b.file))) return;
    const ULONGLONG now = GetTickCount64();
    if (b.restoreAsked && now - b.restoreAsked < retryMs) return;
    b.restoreAsked = now;
    gCopying = true;
    const std::string id = "bitgapps-" + narrow(b.file);
    const std::wstring source = bundleFile(b.file), target = Library::instance().directory() + L"\\" + widen(id);
    auto copied = std::make_shared<bool>(false);
    shell.runInBackground(
        [source, target, copied] { *copied = bundledApks::copy(source, target); },
        [id, copied] {
            gCopying = false;
            if (!*copied) {
                shellLog::write("google play: " + id + " could not be put back in the library from Megler's package");
                return;
            }
            shellLog::write("google play: " + id + " was not in the library: put back from Megler's package");
            Library::instance().added(id);
        });
}

/// Once Google Play is wanted: what of it is not installed, installed (Google Play services first), when the portal has
/// listed the phone's apps and nothing else is being installed.
void ensureInstalled(AppShell& shell) {
    const bool listed = separateApps::portal() == separateApps::Portal::on && separateApps::portalProblem().empty();
    if ((installed(gmsPackage) && installed(storePackage)) || !listed) return;
    if (!separateApps::working().empty() || gCopying) return;
    for (Bundled& b : gBundled) {
        if (installed(b.package)) continue;
        const auto entry = entryOf(b.package);
        if (!entry) {
            restore(shell, b);
            return;
        }
        if (!entry->analysed || !entry->report.ready()) return;
        const separateApps::Status s = separateApps::status(b.package);
        if (s.queued || s.installed != separateApps::Installed::no) return;
        const ULONGLONG now = GetTickCount64();
        if (b.installAsked && now - b.installAsked < retryMs) return;
        b.installAsked = now;
        shellLog::write("google play: installing " + entry->title() + ", Google Play being wanted");
        separateApps::install(shell, entry->id);
        return;
    }
}

} // namespace

Choice choice() {
    const std::string answered = settings::megler(settings::googlePlay).value_or("");
    return answered == "yes" ? Choice::wanted : answered == "no" ? Choice::notWanted : Choice::unasked;
}

bool bundled() {
    static const bool has = files::exists(bundleFile(gBundled[0].file)) && files::exists(bundleFile(gBundled[1].file));
    return has;
}

void tick(AppShell& shell) {
    // (Not while Megler sends its own update to the portal: it closes in a moment, and the next one asks.)
    if (appPackage::isSingleApp() || !bundled() || appPackage::selfUpdating()) return;
    switch (choice()) {
    case Choice::unasked:
        if (!gAsking && !shell.dialogShown()) ask(shell);
        break;
    case Choice::wanted:
        ensureInstalled(shell);
        break;
    case Choice::notWanted:
        break;
    }
}

std::string pending() {
    if (appPackage::isSingleApp() || !bundled() || choice() != Choice::wanted) return {};
    if (installed(gmsPackage) && installed(storePackage)) return {};
    return "Google Play was chosen: Megler installs its Google Play services, then Google Play Store. Apps run with microG "
           "until Google Play services is installed.";
}

bool googleServicesChoosable() { return appPackage::hasGoogleServices(); }

} // namespace megler::app::ui::googlePlay
