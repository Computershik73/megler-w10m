#include "pch.h"

#include "ui/SeparateApps.h"

#include "megler.h"
#include "library/Library.h"
#include "shell/Files.h"
#include "shell/PickedFile.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/GooglePlay.h"

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
bool gListing = false;               // the portal is being asked
bool gListed = false;                // it answered at least once
bool gPortalReachable = false;       // info answered, independently of its package list
std::string gProblem;                // why it could not be asked last
std::vector<appPackage::InstalledApp> gInstalled;
std::map<std::string, Status> gStatus; // the actions' part (queued, step, made), by Android package
std::deque<Job> gJobs;
bool gWorking = false;
std::string gWorkingLine;
std::vector<std::function<void()>> gPortalWaiters; // what waits for the portal's answer (askPortal)
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

/// The portal's list, asked again. Off the UI thread.
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
        // Windows removes the old APPX during an update. Keep its APK in Megler first,
        // so a new package that Megler cannot read cannot leave a marker to a vanished file.
        if (!job.picked && !entry.installedPackage.empty()) {
            setStep(package, stepOf("Keeping the APK for this update…"));
            const std::string error = Library::instance().restore(job.id);
            if (!error.empty()) {
                setStep(package, "The installed APK could not be kept for the update: " + error);
                return;
            }
            const auto restored = Library::instance().find(job.id);
            if (!restored) {
                setStep(package, "The APK kept for the update is missing from the library.");
                return;
            }
            entry = *restored;
        }
        const std::wstring apkPath = entry.path;
        const uint64_t apkSize = job.picked ? job.picked->size() : files::size(entry.path);
        setStep(package, stepOf("Making the package…"));
        const appPackage::Made made = appPackage::make(entry, [&](const std::string& step) { setStep(package, stepOf(step)); });
        if (!made.error.empty()) {
            const std::string kept = job.picked ? keepCopy(job, package) : std::string();
            setStep(package, made.error + (kept.empty() ? std::string() : " (" + kept + ")"));
            return;
        }
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
            result = appPackage::installThroughPortal(made, step);
        }
        if (!result.ok) {
            const std::string kept = job.picked ? keepCopy(job, package) : std::string();
            setStep(package, result.said + " The package is made: Windows's installer can install it (below)." +
                                 (kept.empty() ? std::string() : " (" + kept + ")"));
            return;
        }
        const bool servicePublished = appPackage::registerServiceApk(entry, made.fullName, true);
        if (!servicePublished) shellLog::write("microG: installed, but its shared service APK could not be published; launch microG to retry");
        // Installed: the package made is the installed one now, and goes (Windows's installer is not wanted for it). The
        // app is listed as installed at once; the portal's list is asked again once the queue is done.
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
        // Megler keeps no copy of the APK when it can read the installed one in its place; else it keeps (or makes) one.
        const std::wstring installed = appPackage::installedApk(made.fullName);
        if (appPackage::canRead(installed) && files::size(installed) == apkSize) {
            Library::instance().adopt(job.id, made.fullName);
            setStep(package, "Installed. Megler reads its APK in the installed app and keeps no copy.");
        } else {
            shellLog::write("separate apps: " + narrow(installed) + " can't be read by Megler (or is not as large as " + narrow(apkPath) +
                            "): Megler keeps a copy of the APK");
            const std::string kept = job.picked ? keepCopy(job, package) : std::string();
            setStep(package, "Installed." + (kept.empty() ? std::string() : " Megler could not keep a copy of the APK for updates: " + kept));
        }
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

/// A listing of the portal's ended: what waited for its answer goes on. UI thread.
void portalAnswered() {
    std::vector<std::function<void()>> waiters;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        waiters.swap(gPortalWaiters);
    }
    for (auto& then : waiters) then();
}

/// The worker: the jobs one after another, then the portal's list again. UI thread.
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

bool requirePortal(AppShell& shell) {
    const Portal state = portal();
    if (state == Portal::on) return true;
    refresh(shell);  // asked again, for the next try
    if (state == Portal::unknown) {
        shell.alert("Asking the Device Portal", "Megler installs apps through the phone's Device Portal and is asking it whether it is on. "
                                                "Try again in a moment.");
        return false;
    }
    const std::string why = portalProblem();
    shellLog::write("separate apps: nothing installed while the Device Portal is off: " + why);
    shell.confirm("Turn On the Device Portal",
                  "Megler installs apps through the phone's Device Portal, which did not answer (" + why +
                      "). In Settings › Update & security › For developers, turn on Developer mode, then Device Portal, and turn the "
                      "portal's Authentication off: Megler asks the portal on this phone itself, without a user name and password. "
                      "Then try again.",
                  "Open Settings", false,
                  [] { winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(L"ms-settings:developers")); });
    return false;
}

void install(AppShell& shell, const std::string& apkId, bool whenRead) {
    if (!requirePortal(shell)) return;
    const auto entry = Library::instance().find(apkId);
    if (!entry || (!whenRead && entry->report.package.empty())) return;
    if (!googlePlay::appsMayGo(shell, entry->report.package, false)) return;
    shellLog::write("separate apps: install " + apkId);
    Job job;
    job.entry = *entry;
    job.package = entry->report.package;
    job.id = apkId;
    job.whenRead = whenRead;
    enqueue(shell, std::move(job));
}

void installPicked(AppShell& shell, std::shared_ptr<PickedFile> picked, const std::string& apkId) {
    if (!requirePortal(shell)) return;
    // (Which app it is is not known until it is read: none but Google Play's own two while those are awaited.)
    if (!googlePlay::appsMayGo(shell, std::string(), false)) return;
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
    if (!requirePortal(shell) || !googlePlay::appsMayGo(shell, std::string(), false)) return;
    const auto ids = outdatedIds();
    shellLog::write(format("separate apps: update all, %u", unsigned(ids.size())));
    for (const auto& id : ids) install(shell, id);
}

std::string working() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gWorking || !gJobs.empty() ? gWorkingLine : std::string();
}

} // namespace megler::app::ui::separateApps
