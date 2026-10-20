#include "pch.h"

#include "ui/SeparateApps.h"

#include "megler.h"
#include "library/Library.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"

#include <deque>
#include <map>

namespace megler::app::ui::separateApps {

namespace {

struct Job {
    bool install = true;   // else uninstall
    ApkEntry entry;        // the app, as the library had it when asked
    std::string package;   // its Android package
};

std::mutex gMutex;
unsigned gGeneration = 1;
bool gListing = false;               // the portal is being asked
bool gListed = false;                // it answered at least once
std::string gProblem;                // why it could not be asked last
std::vector<appPackage::InstalledApp> gInstalled;
std::map<std::string, Status> gStatus; // the actions' part (queued, step, made), by Android package
std::deque<Job> gJobs;
bool gWorking = false;
std::string gWorkingLine;
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
    gStatus[package].step = step;
    gWorkingLine = step;
    gGeneration++;
}

/// The portal's list, asked again. Off the UI thread.
void list() {
    std::vector<appPackage::InstalledApp> apps;
    std::string error;
    const bool ok = appPackage::installedApps(apps, error);
    std::lock_guard<std::mutex> lock(gMutex);
    if (ok) {
        gInstalled = std::move(apps);
        gListed = true;
        gProblem.clear();
    } else {
        gProblem = error;
    }
    gListing = false;
    gGeneration++;
}

/// One job. Off the UI thread.
void run(const Job& job, int number, int total) {
    const std::string title = job.entry.title();
    auto stepOf = [&](const std::string& what) {
        return total > 1 ? format("%s (%d of %d): ", title.c_str(), number, total) + what : what;
    };
    if (job.install) {
        setStep(job.package, stepOf("Making the package…"));
        const appPackage::Made made = appPackage::make(job.entry, [&](const std::string& step) { setStep(job.package, stepOf(step)); });
        if (!made.error.empty()) {
            setStep(job.package, made.error);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gStatus[job.package].made = made;
        }
        const appPackage::PortalResult result =
            appPackage::installThroughPortal(made, [&](const std::string& step) { setStep(job.package, stepOf(step)); });
        setStep(job.package, result.ok ? std::string("Installed.")
                                       : result.said + " The package is made: Windows's installer can install it (below).");
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
        setStep(job.package, stepOf("Uninstalling…"));
        const appPackage::PortalResult result = appPackage::uninstallThroughPortal(app);
        setStep(job.package, result.ok ? std::string("Uninstalled. What it kept in the shared folder, its part of Android's storage, stays.")
                                       : result.said);
    }
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
                gStatus[job.package].queued = false;
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
            // Asked for while the last was going on.
            startWorker(*shellPointer);
        });
}

void enqueue(AppShell& shell, Job job) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        Status& s = gStatus[job.package];
        if (s.queued) return;
        s.queued = true;
        s.step = job.install ? "Waiting…" : "Waiting to uninstall…";
        gTotal++;
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
    shell.runInBackground([] { list(); });
}

unsigned generation() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gGeneration;
}

std::string portalProblem() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gProblem;
}

Status status(const std::string& androidPackage) {
    std::lock_guard<std::mutex> lock(gMutex);
    return statusLocked(androidPackage);
}

void install(AppShell& shell, const std::string& apkId) {
    const auto entry = Library::instance().find(apkId);
    if (!entry || entry->report.package.empty()) return;
    shellLog::write("separate apps: install " + entry->report.package);
    enqueue(shell, Job{true, *entry, entry->report.package});
}

void uninstall(AppShell& shell, const std::string& androidPackage) {
    shellLog::write("separate apps: uninstall " + androidPackage);
    Job job;
    job.install = false;
    job.package = androidPackage;
    for (const auto& e : Library::instance().entries())
        if (e.report.package == androidPackage) job.entry = e;
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
    const auto ids = outdatedIds();
    shellLog::write(format("separate apps: update all, %u", unsigned(ids.size())));
    for (const auto& id : ids) install(shell, id);
}

std::string working() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gWorking ? gWorkingLine : std::string();
}

} // namespace megler::app::ui::separateApps
