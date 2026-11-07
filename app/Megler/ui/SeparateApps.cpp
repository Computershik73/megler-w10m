#include "pch.h"

#include "ui/SeparateApps.h"

#include "megler.h"
#include "library/Library.h"
#if defined(MEGLER_XBOX)
#include "shell/DevicePortal.h"
#endif
#include "shell/BundledApks.h"
#include "shell/Files.h"
#include "shell/LogMirror.h"
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
bool gReachable = false;             // its lister answered, whether or not its list came
std::string gProblem;                // why it could not be had last
std::vector<appPackage::InstalledApp> gInstalled;
std::map<std::string, Status> gStatus; // the actions' part (queued, step, made), by Android package
std::deque<Job> gJobs;
bool gWorking = false;
std::string gWorkingLine;
std::vector<std::function<void()>> gListWaiters; // what waits for the list (askList)
int gDone = 0, gTotal = 0;           // of the jobs since the worker started
bool gRefused = false;               // the last install or uninstall done or refused was refused for access (installsRefused)

/// How an install or an uninstall of `package` ended, as far as access goes: refused for it (Megler has not got
/// packageManagement), or not.
void setRefused(const std::string& package, bool denied) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!package.empty()) gStatus[package].accessDenied = denied;
    gRefused = denied;
    gGeneration++;
}

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

/// The list of the apps' packages, asked for again: Windows's package manager's, on an Xbox its Device Portal's
/// (appPackage::installedApps). Off the UI thread.
void listInstalled() {
    std::vector<appPackage::InstalledApp> apps;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gReachable = false;
    }
    const bool ok = appPackage::installedApps(apps, error, [] {
        std::lock_guard<std::mutex> lock(gMutex);
        gReachable = true;
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
    // And the logs' copies of the packages that are gone (shell/LogMirror.h), which would grow in number for ever.
    if (ok) logMirror::removeUninstalled(apps);
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
        // Windows's deployment engine reads the package where it lies; on an Xbox, the console's portal is sent it (shell/AppPackage.h).
        const auto step = [&](const std::string& said) { setStep(package, stepOf(said)); };
        const appPackage::InstallResult result = appPackage::install(made, step);
        setRefused(package, !result.ok && result.accessDenied);
        if (!result.ok) {
            const std::string kept = job.picked ? keepCopy(job, package) : std::string();
#if defined(MEGLER_XBOX)
            // (An Xbox has no installer of its own for a package Megler made: what the console's portal said is all there is.)
            setStep(package, result.said + (kept.empty() ? std::string() : " (" + kept + ")"));
#else
            /*
             * Refused for access at every try: Megler has not got packageManagement, which a phone grants only with the capability
             * part of its interop unlock. Windows's own words, "Access is denied." four times, said nothing of why nor of what to
             * do (a user's phone, 0.1.0.671); the package is made all the same, for Windows's own installer.
             */
            if (result.accessDenied) {
                setStep(package, appPackage::accessDeniedText("Open the Package (below): Windows's own installer installs it.") +
                                     (kept.empty() ? std::string() : " (" + kept + ")"));
                return;
            }
            /*
             * CERT_E_UNTRUSTEDROOT: Windows trusts no certificate of Megler's making (its key is this phone's own, in
             * LocalState) unless it is installed for the machine, which an app may not do. Until 2026-11-07 the phone's Device
             * Portal, asked where the engine refused, installed it with the first package; now the user installs it once,
             * through Windows's own installer of certificates, which the app's page hands the file to.
             */
            const bool untrusted = appPackage::untrustedCertificate(result.said);
            // (Desktop Windows opens no certificate an app hands it: the command that makes the machine trust it, or the
            // certificate in File Explorer.)
            const std::string trust =
                platform::isMobile()
                    ? " Windows does not trust the certificate Megler signs apps' packages with: open the certificate (below), let Windows "
                      "install it, and install the app again."
                    : " Windows does not trust the certificate Megler signs apps' packages with. Trust it once, as administrator: "
                      "Copy the Command (below) and run it in PowerShell run as administrator -- " + appPackage::trustCommand() +
                          " -- or Show the Certificate, open it, Install Certificate, Local Machine, Trusted People. Then install the "
                          "app again.";
            setStep(package, result.said + (untrusted ? trust : " The package is made: Windows's installer can install it (below).") +
                                 (kept.empty() ? std::string() : " (" + kept + ")"));
#endif
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
        const appPackage::InstallResult result = appPackage::uninstall(app);
        setRefused(job.package, !result.ok && result.accessDenied);
        if (result.ok) {
            std::lock_guard<std::mutex> lock(gMutex);
            gInstalled.erase(std::remove_if(gInstalled.begin(), gInstalled.end(),
                                            [&](const appPackage::InstalledApp& a) { return a.identity == app.identity; }),
                             gInstalled.end());
        }
        // (Refused for access, as an install is where Megler has not got packageManagement: Windows uninstalls it itself.)
        const std::string elsewhere = platform::isMobile() ? "uninstall it from the phone's list of apps: press and hold it, then Uninstall."
                                                           : "uninstall it in Windows's Settings, Apps.";
        setStep(job.package, result.ok             ? std::string("Uninstalled. What it kept in the shared folder, its part of Android's storage, stays.")
                             : result.accessDenied ? appPackage::accessDeniedText(elsewhere)
                                                   : result.said);
    }
}

/// A listing ended: what waited for it goes on. UI thread.
void listAnswered() {
    std::vector<std::function<void()>> waiters;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        waiters.swap(gListWaiters);
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
            listInstalled();
        },
        [shellPointer] {
            {
                std::lock_guard<std::mutex> lock(gMutex);
                gWorking = false;
                gWorkingLine.clear();
                gGeneration++;
            }
            listAnswered();
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
    shell.runInBackground([] { listInstalled(); }, [] { listAnswered(); });
}

void askList(AppShell& shell, std::function<void()> then) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gListWaiters.push_back(std::move(then));
    }
    // (A listing or the worker under way runs them as it ends.)
    refresh(shell);
}

unsigned generation() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gGeneration;
}

std::string listProblem() {
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

#if defined(MEGLER_XBOX)
namespace {

void askPortalSignIn(AppShell& shell) {
    /*
     * The user name and password of the console's Device Portal (Dev Home, Remote access settings, where its authentication
     * is on), one field at a time: a dialog has one. Kept in Windows's credential locker (shell/DevicePortal.h), forgotten
     * when the portal refuses them, and the list asked for again with them at once.
     */
    AppShell::DialogSpec name;
    name.title = "The Xbox's Device Portal";
    name.message = "The console's Device Portal, through which Megler installs its apps on an Xbox, asks for its user name and password: "
                   "those set in Dev Home (Remote access settings). Its user name:";
    name.field = true;
    name.fieldText = devicePortal::savedCredentials().user;
    name.fieldPlaceholder = "User name";
    name.buttons = {"Cancel", "Next"};
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    shell.dialog(std::move(name), [weak](int button, std::string user) {
        auto self = weak.lock();
        if (!self || button != 1 || user.empty()) return;
        AppShell::DialogSpec password;
        password.title = "The Xbox's Device Portal";
        password.message = "The password of the Device Portal's user " + user + ":";
        password.field = true;
        password.secret = true;
        password.fieldPlaceholder = "Password";
        password.buttons = {"Cancel", "Sign In"};
        self->dialog(std::move(password), [weak, user](int button, std::string word) {
            auto self = weak.lock();
            if (!self || button != 1) return;
            devicePortal::saveCredentials({user, word});
            shellLog::write("separate apps: the Xbox's Device Portal's user name and password given, for " + user);
            refresh(*self);
        });
    });
}

} // namespace
#endif

List list() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gReachable) return List::ready;
    if (!gProblem.empty()) return List::unavailable;
    return gListed ? List::ready : List::unknown;
}

bool requireList(AppShell& shell, std::function<void()> retry) {
    const List state = list();
    if (state == List::ready) return true;
    /*
     * Asked for before the first list came: done once it has. The flag ui-update-all is acted on a second after the start,
     * and a Lumia 650's Megler 648 acted on it 156 ms before Windows listed the apps' packages: it said "Asking the Device
     * Portal", and Update All waited for the owner's tap, 18 s later.
     */
    if (state == List::unknown && retry) {
        shellLog::write("separate apps: waiting for the list of the installed apps");
        std::weak_ptr<AppShell> weak = shell.weak_from_this();
        askList(shell, [weak, retry = std::move(retry)] {
            auto self = weak.lock();
            if (!self) return;
            // (Once: a list still not known has the shell say so.)
            if (list() == List::unknown)
                requireList(*self);
            else
                retry();
        });
        return false;
    }
    refresh(shell);  // asked again, for the next try
    if (state == List::unknown) {
        shell.alert("Looking at the Installed Apps", "Megler is asking Windows which of its apps are installed, which it needs to know before it "
                                                     "installs one. Try again in a moment.");
        return false;
    }
    const std::string why = listProblem();
    shellLog::write("separate apps: nothing installed while no list of the installed apps can be had: " + why);
#if defined(MEGLER_XBOX)
    // The Xbox's Device Portal, which lists, installs and uninstalls the apps' packages on a console: its user name and
    // password asked for where it wants them, else what it said and the way to it (appPackage::portalHelp, in `why` where
    // it was not reached at all).
    if (appPackage::portalWantsSignIn()) {
        askPortalSignIn(shell);
        return false;
    }
    shell.alert("The Xbox's Device Portal", "The console's Device Portal did not tell Megler which of its apps are installed: " + why +
                                                " Megler installs nothing until it answers. Then try again.");
#else
    /*
     * Windows's package manager, which lists, installs and uninstalls the apps' packages, did not answer. Until 2026-11-07
     * the phone's Device Portal was asked then, and this said to turn the portal on; nothing is asked of the portal any more,
     * and there is nothing to turn on: the restricted capabilities packageQuery and packageManagement are in Megler's
     * manifest, and Windows grants them when it installs Megler.
     */
    shell.alert("Windows Did Not List the Apps",
                "Windows's package manager did not tell Megler which of its apps are installed (" + why + "). Megler lists, installs and "
                "uninstalls its apps through it, and installs nothing until it answers. Try again in a moment.");
#endif
    return false;
}

void install(AppShell& shell, const std::string& apkId, bool whenRead) {
    if (!requireList(shell, [&shell, apkId, whenRead] { install(shell, apkId, whenRead); })) return;
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
    if (!requireList(shell, [&shell, picked, apkId] { installPicked(shell, picked, apkId); })) return;
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
    if (!requireList(shell, [&shell] { updateAll(shell); })) return;
    const auto ids = outdatedIds();
    shellLog::write(format("separate apps: update all, %u", unsigned(ids.size())));
    for (const auto& id : ids) install(shell, id);
}

std::string working() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gWorking || !gJobs.empty() ? gWorkingLine : std::string();
}

bool installsRefused() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gRefused;
}

} // namespace megler::app::ui::separateApps
