#include "pch.h"

#include "ui/SeparateApps.h"

#include "megler.h"
#include "library/Library.h"
#include "shell/BundledApks.h"
#include "shell/Files.h"
#include "shell/PickedFile.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"

#include <algorithm>
#include <deque>
#include <map>

namespace megler::app::ui::separateApps {

namespace {

struct Job {
    bool install = true;   // else uninstall
    ApkEntry entry;        // the app, as the library had it when asked
    std::string package;   // its Android package (empty for a picked APK until it is read)
    std::string id;        // its library id
    bool whenRead = false; // the library is still to read it
    std::shared_ptr<PickedFile> picked; // an APK picked to be installed, read where it is
};

std::mutex gMutex;
unsigned gGeneration = 1;
bool gListing = false;               // the list of the apps' packages is being asked for
bool gListed = false;                // it came at least once
bool gPortalReachable = false;       // info answered, independently of its package list
std::string gProblem;                // why it could not be asked last
std::vector<appPackage::InstalledApp> gInstalled;
std::map<std::string, Status> gStatus; // the actions' part (queued, step, made), by Android package
std::deque<Job> gJobs;
bool gWorking = false;
std::string gWorkingLine;
std::vector<std::function<void()>> gPortalWaiters; // what waits for the list (askPortal)
int gDone = 0, gTotal = 0;           // of the jobs since the worker started

std::string identityOf(const std::string& package) {
    char identity[64] = {};
    return megler_appx_app_identity(package.c_str(), identity, sizeof(identity)) ? std::string(identity) : std::string();
}

/// "0.1.0.354" before "0.1.0.358".
bool older(const std::string& a, const std::string& b) {
    unsigned x[4] = {}, y[4] = {};
    sscanf_s(a.c_str(), "%u.%u.%u.%u", &x[0], &x[1], &x[2], &x[3]);
    sscanf_s(b.c_str(), "%u.%u.%u.%u", &y[0], &y[1], &y[2], &y[3]);
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i];
    return false;
}

/// Under gMutex.
Status statusLocked(const std::string& package) {
    Status s;
    auto it = gStatus.find(package);
    if (it != gStatus.end()) s = it->second;
    s.installed = Installed::unknown;
    s.version.clear();
    s.outdated = false;
    if (!gListed || !gProblem.empty()) return s;
    const std::string identity = identityOf(package);
    s.installed = Installed::no;
    for (const auto& app : gInstalled) {
        if (app.identity != identity) continue;
        s.installed = Installed::yes;
        s.version = app.version;
        s.outdated = older(app.version, appPackage::meglerVersion());
    }
    return s;
}

void setStep(const std::string& package, const std::string& step) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!package.empty()) gStatus[package].step = step;
    gWorkingLine = step;
    gGeneration++;
}

/// A picked APK kept after all: copied into the library, which reads it. Empty, or why not.
std::string keepCopy(const Job& job, const std::string& package) {
    setStep(package, "Keeping a copy of " + job.picked->name() + "…");
    const std::string error = job.picked->copyTo(Library::instance().directory() + L"\\" + widen(job.id));
    if (error.empty())
        Library::instance().added(job.id);
    else
        shellLog::write("separate apps: " + job.id + " not kept: " + error);
    return error;
}

/// A picked APK read where it is: its report and icon into the library's cache, as the library writes them.
ApkEntry readPicked(const Job& job) {
    ApkEntry entry;
    entry.id = job.id;
    entry.path = job.picked->path();
    entry.size = job.picked->size();
    const std::wstring own = Library::instance().directory() + L"\\" + widen(job.id);
    const std::string path = narrow(entry.path);
    std::string json;
    if (char* report = megler_apk_report(path.c_str())) {
        json = report;
        megler_free(report);
    }
    entry.report = ApkReport::parse(json);
    entry.analysed = entry.report.valid;
    if (!json.empty()) files::write(own + L".json", json);
    uint8_t* icon = nullptr;
    size_t iconSize = 0;
    if (megler_apk_icon(path.c_str(), &icon, &iconSize) && icon && iconSize > 0 &&
        files::write(own + L".icon", std::string_view(reinterpret_cast<const char*>(icon), iconSize)))
        entry.iconPath = own + L".icon";
    if (icon) megler_free(icon);
    shellLog::write("separate apps: " + job.picked->name() + " read where it is: " + (entry.report.package.empty() ? "?" : entry.report.package) +
                    " -- " + entry.report.status());
    return entry;
}

/// The folders of apps' APKs in the shared folder that nothing reads any more (appPackage::removeUnusedSharedApks): those the
/// library's entries are in kept, and only while no job is queued or running. (A package being made has a folder no
/// installed package and no entry names yet; removeUnusedSharedApks leaves a folder of the last hour alone besides.) Off
/// the UI thread.
void removeUnusedApks(const std::vector<appPackage::InstalledApp>& installed) {
    if (appPackage::isSingleApp()) return;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gWorking || !gJobs.empty()) return;
    }
    std::vector<std::wstring> used;
    for (const ApkEntry& entry : Library::instance().entries())
        if (appPackage::inSharedApks(entry.path)) used.push_back(entry.path.substr(0, entry.path.find_last_of(L'\\')));
    appPackage::removeUnusedSharedApks(installed, used);
}

/// Google Play services installed as an app of its own (Google's or microG's) and not the apps' registered service: its
/// registration after the install failed -- one tried while the once-a-build check of the bundled APKs held its APK open
/// failed so after an Update All, and the record of the package it replaced was then forgotten -- and the apps ran with
/// microG, Play Store refusing to start, until Google Play services' own package was opened. Registered from the library's
/// entry, which costs nothing where it is registered already (appPackage::registerServiceApk). Off the UI thread.
void registerInstalledServices(const std::vector<appPackage::InstalledApp>& installed) {
    if (appPackage::isSingleApp()) return;
    for (const ApkEntry& e : Library::instance().entries()) {
        if (e.report.package != "com.google.android.gms" || e.installedPackage.empty()) continue;
        if (std::none_of(installed.begin(), installed.end(), [&](const appPackage::InstalledApp& a) { return a.fullName == e.installedPackage; }))
            continue;
        if (!appPackage::registerServiceApk(e, e.installedPackage, false))
            shellLog::write("separate apps: " + e.installedPackage + " is installed and still not the apps' Google Play services");
    }
}

/// The list of the apps' packages, asked for again: Windows's package manager's, the Device Portal's where Windows will not
/// answer (appPackage::installedApps). Off the UI thread.
void list() {
    std::vector<appPackage::InstalledApp> apps;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gPortalReachable = false;
    }
    const bool ok = appPackage::installedApps(apps, error, [] {
        std::lock_guard<std::mutex> lock(gMutex);
        gPortalReachable = true;
        gGeneration++;
    });
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (ok) {
            gInstalled = apps;
            gListed = true;
            gProblem.clear();
        } else {
            gProblem = error;
        }
        gListing = false;
        gGeneration++;
    }
    // What the packages no longer need: the Android files of builds none of them is of (after Update All, the older ones'),
    // the copies of Google Play services' APK but the one registered, and the APKs apps handed over that were left. Then what
    // the shared folder holds, once a run.
    if (ok) appPackage::removeUnusedAndroidRoots(apps);
    if (ok) registerInstalledServices(apps);
    if (ok) removeUnusedApks(apps);
    appPackage::removeUnusedServiceCopies();
    appPackage::removeOldHandedApks();
    appPackage::logSharedSpace();
}

/// One job. Off the UI thread.
void run(Job& job, int number, int total) {
    const std::string title = job.entry.title();
    auto stepOf = [&](const std::string& what) {
        return total > 1 ? format("%s (%d of %d): ", title.c_str(), number, total) + what : what;
    };
    if (job.install) {
        // The app as it is to be packed: a picked APK read where it is, one the library is reading once it has, or the
        // library's entry.
        ApkEntry entry = job.entry;
        std::string package = job.package;
        if (job.picked) {
            setStep("", stepOf("Reading " + job.picked->name() + "…"));
            entry = readPicked(job);
            package = entry.report.package;
            job.package = package;
            if (!entry.report.ready()) {
                const std::string kept = keepCopy(job, package);
                setStep(package, "It can't run here: " + entry.report.status() + (kept.empty() ? " It is kept in the library." : " (" + kept + ")"));
                return;
            }
            std::lock_guard<std::mutex> lock(gMutex);
            gStatus[package].queued = true;
        } else if (job.whenRead) {
            setStep(package, stepOf("Reading the APK…"));
            for (int i = 0; i < 600; i++) {
                const auto found = Library::instance().find(job.id);
                if (!found) break;
                entry = *found;
                if (entry.analysed && !entry.analysing) break;
                Sleep(500);
            }
            package = entry.report.package;
            job.package = package;
            if (!entry.analysed || !entry.report.ready()) {
                setStep(package, "It can't run here: " + entry.report.status());
                return;
            }
        }
        // The APK is not packed: make keeps it in the shared folder, where the package's app reads it. An update takes it
        // from there as it is, or, from a package made before 645, copies it there while that package is still installed.
        setStep(package, stepOf("Making the package…"));
        // Google Play services or Google Play Store, installed from an older APK than Megler's package brings now: its next
        // package (Update All, after an update of Megler) is made from Megler's (bundledApks::replacementFor), whose report is
        // the library's once it is installed. (An APK of the library's not installed yet is the user's choice, and installed
        // as it is.)
        bundledApks::Replacement replacement;
        if (!job.picked && !entry.installedPackage.empty()) replacement = bundledApks::replacementFor(entry);
        if (!replacement.path.empty()) {
            shellLog::write("separate apps: " + entry.id + ": " + replacement.why + ": its package is made from Megler's " +
                            narrow(replacement.path.substr(replacement.path.find_last_of(L'\\') + 1)));
            entry.path = replacement.path;
            entry.splitsDir.clear();
            entry.size = files::size(replacement.path);
            entry.report = ApkReport::parse(replacement.report);
            entry.analysed = true;
        }
        const appPackage::Made made = appPackage::make(entry, [&](const std::string& step) { setStep(package, stepOf(step)); }, job.picked.get());
        if (!made.error.empty()) {
            const std::string kept = job.picked ? keepCopy(job, package) : std::string();
            setStep(package, made.error + (kept.empty() ? std::string() : " (" + kept + ")"));
            return;
        }
        // (make put the library's report of the APK before beside it, which the package's app reads as its own.)
        if (!replacement.path.empty() &&
            !files::write(appPackage::sharedFolder() + L"\\" + made.apkFolder + L"\\app.apk.json", replacement.report))
            shellLog::write("separate apps: the report of " + entry.id + " could not be put beside its APK in the shared folder");
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gStatus[package].made = made;
        }
        // Windows's deployment engine reads the package where it lies; the Device Portal is sent it from Megler's memory, which
        // a phone of 1 GB allows 390 MB of (shell/AppPackage.h): the portal only where the engine refuses.
        const auto step = [&](const std::string& said) { setStep(package, stepOf(said)); };
        appPackage::PortalResult result = appPackage::installWithPackageManager(made.appx, step);
        if (!result.ok) {
            shellLog::write("separate apps: " + result.said + ": through the Device Portal instead");
            const std::string engine = result.said;
            result = appPackage::installThroughPortal(made, step);
            // The portal installs only a developer's package: why the engine refused the Store's way is said too.
            if (!result.ok) result.said = engine + ". The Device Portal: " + result.said;
        }
        if (!result.ok) {
            const std::string kept = job.picked ? keepCopy(job, package) : std::string();
            // CERT_E_UNTRUSTEDROOT: Windows trusts no certificate of Megler's making unless it is installed for the machine, which
            // an app may not do. On a phone the Device Portal installs it with the first package; desktop Windows has no portal.
            const bool untrusted = result.said.find("0x800b0109") != std::string::npos;
            // (Desktop Windows opens no certificate an app hands it: the command that makes the machine trust it, or the
            // certificate in File Explorer.)
            const std::string trust =
                platform::isMobile() ? " Windows does not trust the certificate Megler signs apps' packages with: open the certificate (below), choose "
                                       "Install Certificate, Local Machine, Place all certificates in the following store, Trusted People, and "
                                       "install the app again."
                                     : " Windows does not trust the certificate Megler signs apps' packages with. Trust it once, as administrator: "
                                       "Copy the Command (below) and run it in PowerShell run as administrator -- " + appPackage::trustCommand() +
                                           " -- or Show the Certificate, open it, Install Certificate, Local Machine, Trusted People. Then install the "
                                           "app again.";
            setStep(package, result.said + (untrusted ? trust : " The package is made: Windows's installer can install it (below).") +
                                 (kept.empty() ? std::string() : " (" + kept + ")"));
            return;
        }
        // The APK as the installed package's app reads it, in the shared folder (that of a package made before is gone now).
        ApkEntry inShared = entry;
        inShared.path = appPackage::sharedFolder() + L"\\" + made.apkFolder + L"\\app.apk";
        inShared.splitsDir = files::exists(Library::splitsDirFor(inShared.path)) ? Library::splitsDirFor(inShared.path) : std::wstring();
        const bool servicePublished = appPackage::registerServiceApk(inShared, made.fullName, true);
        if (!servicePublished) shellLog::write("microG: installed, but its shared service APK could not be published; launch microG to retry");
        // Installed: the package made is the installed one now, and goes (Windows's installer is not wanted for it). The
        // app is listed as installed at once; the list is asked for again once the queue is done.
        files::remove(made.appx);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gStatus[package].made = appPackage::Made();
            gInstalled.erase(std::remove_if(gInstalled.begin(), gInstalled.end(),
                                            [&](const appPackage::InstalledApp& a) { return a.identity == made.identity; }),
                             gInstalled.end());
            gInstalled.push_back({made.identity, appPackage::meglerVersion(), made.fullName});
            gGeneration++;
        }
        // The library's entry is that APK now, and Megler's own copy, if it had one, goes: one copy of it on the phone. (In the
        // package, as before 645, Megler could not read it once Windows installed it as the Store installs an app.)
        if (!replacement.path.empty()) files::write(Library::instance().directory() + L"\\" + widen(job.id) + L".json", replacement.report);
        appPackage::sharedApkInstalled(made);
        Library::instance().adopt(job.id, made.fullName, made.apkFolder);
        setStep(package, "Installed. Its APK is kept once, in the shared folder, where the app and Megler read it.");
    } else {
        appPackage::InstalledApp app;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            const std::string identity = identityOf(job.package);
            for (const auto& a : gInstalled)
                if (a.identity == identity) app = a;
        }
        if (app.fullName.empty()) {
            setStep(job.package, "It is not installed.");
            return;
        }
        // An app whose APK is the installed package's: copied back first, so that the library keeps it.
        if (!job.entry.installedPackage.empty()) {
            setStep(job.package, stepOf("Copying its APK back into Megler…"));
            const std::string error = Library::instance().restore(job.entry.id);
            if (!error.empty()) {
                setStep(job.package, "Not uninstalled: " + error);
                return;
            }
        }
        setStep(job.package, stepOf("Uninstalling…"));
        const appPackage::PortalResult result = appPackage::uninstallThroughPortal(app);
        if (result.ok) {
            std::lock_guard<std::mutex> lock(gMutex);
            gInstalled.erase(std::remove_if(gInstalled.begin(), gInstalled.end(),
                                            [&](const appPackage::InstalledApp& a) { return a.identity == app.identity; }),
                             gInstalled.end());
        }
        setStep(job.package, result.ok ? std::string("Uninstalled. What it kept in the shared folder, its part of Android's storage, stays.")
                                       : result.said);
    }
}

/// A listing ended: what waited for it goes on. UI thread.
void portalAnswered() {
    std::vector<std::function<void()>> waiters;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        waiters.swap(gPortalWaiters);
    }
    for (auto& then : waiters) then();
}

/// The worker: the jobs one after another, then the list again. UI thread.
void startWorker(AppShell& shell) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gWorking || gJobs.empty()) return;
        gWorking = true;
        gDone = 0;
        gTotal = int(gJobs.size());
    }
    AppShell* shellPointer = &shell;
    shell.runInBackground(
        [] {
            for (;;) {
                Job job;
                int number = 0, total = 0;
                {
                    std::lock_guard<std::mutex> lock(gMutex);
                    if (gJobs.empty()) break;
                    job = gJobs.front();
                    gJobs.pop_front();
                    number = ++gDone;
                    total = std::max(gTotal, gDone + int(gJobs.size()));
                    gTotal = total;
                }
                run(job, number, total);
                std::lock_guard<std::mutex> lock(gMutex);
                if (!job.package.empty()) gStatus[job.package].queued = false;
                gGeneration++;
            }
            {
                std::lock_guard<std::mutex> lock(gMutex);
                gListing = true;
            }
            list();
        },
        [shellPointer] {
            {
                std::lock_guard<std::mutex> lock(gMutex);
                gWorking = false;
                gWorkingLine.clear();
                gGeneration++;
            }
            portalAnswered();
            // Asked for while the last was going on.
            startWorker(*shellPointer);
        });
}

void enqueue(AppShell& shell, Job job) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!job.package.empty()) {
            Status& s = gStatus[job.package];
            if (s.queued) return;
            s.queued = true;
            s.step = job.install ? "Waiting…" : "Waiting to uninstall…";
        }
        gTotal++;
        if (!gWorking) gWorkingLine = job.picked ? "Reading " + job.picked->name() + "…" : "Starting…";
        gJobs.push_back(std::move(job));
        gGeneration++;
    }
    startWorker(shell);
}

} // namespace

void refresh(AppShell& shell) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gListing || gWorking) return;
        gListing = true;
    }
    shell.runInBackground([] { list(); }, [] { portalAnswered(); });
}

void askPortal(AppShell& shell, std::function<void()> then) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gPortalWaiters.push_back(std::move(then));
    }
    // (A listing or the worker under way runs them as it ends.)
    refresh(shell);
}

unsigned generation() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gGeneration;
}

std::string portalProblem() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gProblem;
}

bool checking() {
    std::lock_guard<std::mutex> lock(gMutex);
    return !gListed && gProblem.empty();
}

Status status(const std::string& androidPackage) {
    std::lock_guard<std::mutex> lock(gMutex);
    return statusLocked(androidPackage);
}

Portal portal() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gPortalReachable) return Portal::on;
    if (!gProblem.empty()) return Portal::off;
    return gListed ? Portal::on : Portal::unknown;
}

bool requirePortal(AppShell& shell, std::function<void()> retry) {
    const Portal state = portal();
    if (state == Portal::on) return true;
    /*
     * Asked for before the first list came: done once it has. The flag ui-update-all is acted on a second after the start,
     * and a Lumia 650's Megler 648 acted on it 156 ms before Windows listed the apps' packages: it said "Asking the Device
     * Portal", and Update All waited for the owner's tap, 18 s later.
     */
    if (state == Portal::unknown && retry) {
        shellLog::write("separate apps: waiting for the list of the installed apps");
        std::weak_ptr<AppShell> weak = shell.weak_from_this();
        askPortal(shell, [weak, retry = std::move(retry)] {
            auto self = weak.lock();
            if (!self) return;
            // (Once: a list still not known has the shell say so.)
            if (portal() == Portal::unknown)
                requirePortal(*self);
            else
                retry();
        });
        return false;
    }
    refresh(shell);  // asked again, for the next try
    // (The list of the apps' packages comes from Windows's package manager, which installs them too; the phone's Device Portal
    // is asked only where Windows will not answer. Said so: the user saw "Asking the Device Portal" long after Megler had
    // stopped installing through it.)
    if (state == Portal::unknown) {
        shell.alert("Looking at the Installed Apps", "Megler is asking Windows which of its apps are installed, which it needs to know before it "
                                                     "installs one. Try again in a moment.");
        return false;
    }
    const std::string why = portalProblem();
    shellLog::write("separate apps: nothing installed while no list of the installed apps can be had: " + why);
    shell.confirm("Turn On the Device Portal",
                  "Windows did not tell Megler which of its apps are installed, nor did the phone's Device Portal, which Megler asks where "
                  "Windows will not (" + why + "). In Settings › Update & security › For developers, turn on Developer mode, then Device "
                  "Portal, and turn the portal's Authentication off: Megler asks the portal on this phone itself, without a user name and "
                  "password. Then try again.",
                  "Open Settings", false,
                  [] { winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(L"ms-settings:developers")); });
    return false;
}

void install(AppShell& shell, const std::string& apkId, bool whenRead) {
    if (!requirePortal(shell, [&shell, apkId, whenRead] { install(shell, apkId, whenRead); })) return;
    const auto entry = Library::instance().find(apkId);
    if (!entry || (!whenRead && entry->report.package.empty())) return;
    shellLog::write("separate apps: install " + apkId);
    Job job;
    job.entry = *entry;
    job.package = entry->report.package;
    job.id = apkId;
    job.whenRead = whenRead;
    enqueue(shell, std::move(job));
}

void installPicked(AppShell& shell, std::shared_ptr<PickedFile> picked, const std::string& apkId) {
    if (!requirePortal(shell, [&shell, picked, apkId] { installPicked(shell, picked, apkId); })) return;
    shellLog::write("separate apps: install " + picked->name() + " where it is, as " + apkId);
    Job job;
    job.id = apkId;
    job.picked = std::move(picked);
    enqueue(shell, std::move(job));
}

void uninstall(AppShell& shell, const std::string& androidPackage) {
    shellLog::write("separate apps: uninstall " + androidPackage);
    Job job;
    job.install = false;
    job.package = androidPackage;
    for (const auto& e : Library::instance().entries())
        if (e.report.package == androidPackage) {
            job.entry = e;
            job.id = e.id;
        }
    enqueue(shell, std::move(job));
}

std::vector<std::string> outdatedIds() {
    std::vector<std::string> ids;
    const auto entries = Library::instance().entries();
    std::lock_guard<std::mutex> lock(gMutex);
    for (const auto& e : entries) {
        if (e.report.package.empty() || !e.report.ready()) continue;
        const Status s = statusLocked(e.report.package);
        if (s.installed == Installed::yes && s.outdated && !s.queued) ids.push_back(e.id);
    }
    return ids;
}

void updateAll(AppShell& shell) {
    // (Which apps are outdated is known from the list: asked for before it came, as the flag ui-update-all is, it waits.)
    if (!requirePortal(shell, [&shell] { updateAll(shell); })) return;
    const auto ids = outdatedIds();
    shellLog::write(format("separate apps: update all, %u", unsigned(ids.size())));
    for (const auto& id : ids) install(shell, id);
}

std::string working() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gWorking || !gJobs.empty() ? gWorkingLine : std::string();
}

} // namespace megler::app::ui::separateApps
