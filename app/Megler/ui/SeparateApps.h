// The library's apps as apps of the phone's own (shell/AppPackage.h, docs/PER-APP-PACKAGES.md): which are installed and
// which Megler made them, as Windows's package manager lists them (on an Xbox, the console's Device Portal), and installing,
// updating and uninstalling them -- one at a time, on a worker thread, whichever page is shown. Megler is where they are
// managed; it runs an app itself only in an auto run (auto/AutoRun.h).
//
// A package carries the Megler that made it: an update of Megler updates none of them, so those made by another Megler are
// "outdated", and Update All makes and installs each of them again.
//
// After an install Megler keeps no copy of the APK when it can read the installed package's (Library::adopt): the update
// is made from that one. An APK picked to be installed is read where it is (shell/PickedFile.h) and copied only when
// Megler has to keep it (it can't run here, it was not installed, or the installed one can't be read).
#pragma once

#include "shell/AppPackage.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace megler::app {
class PickedFile;
}

namespace megler::app::ui {
class AppShell;
}

namespace megler::app::ui::separateApps {

enum class Installed { unknown, no, yes };

struct Status {
    Installed installed = Installed::unknown; // unknown until the list of the apps' packages has come, or when it could not be had
    std::string version;                      // the installed package's: the Megler that made it
    bool outdated = false;                    // made by another Megler than this one
    bool queued = false;                      // an action of it is waiting or going on
    std::string step;                         // what is being done, or how the last action ended
    appPackage::Made made;                    // the last package made of it here
    bool accessDenied = false;                // its last install or uninstall was refused for access (InstallResult::accessDenied)
};

/// Asks which are installed (appPackage::installedApps), on a worker thread (once at a time). UI thread.
void refresh(AppShell& shell);
/// Goes up with every change of what this module knows.
unsigned generation();
/// Why the list of the apps' packages could not be had the last time; empty when it came (or has not been asked for).
std::string listProblem();
/// Whether the first list since Megler started is still awaited: which apps are installed, and which Megler made them, is
/// not known yet (Windows's package manager answers within a second or two).
bool checking();
Status status(const std::string& androidPackage);

/// Makes the app's package and installs it by Windows's deployment engine (on an Xbox, through the console's Device Portal).
/// When Windows does not install it, the package stays made, for Windows's installer. With `whenRead`, once the library has
/// read the APK (just added). UI thread.
void install(AppShell& shell, const std::string& apkId, bool whenRead = false);
/// An APK the user picked, installed straight from where it is, as the library's `apkId`. UI thread.
void installPicked(AppShell& shell, std::shared_ptr<PickedFile> picked, const std::string& apkId);
void uninstall(AppShell& shell, const std::string& androidPackage);
/// The library's apps installed from another Megler's packages: each installed again.
std::vector<std::string> outdatedIds();
void updateAll(AppShell& shell);
/// What is going on, "Updating Instagram (2 of 3): Packing…"; empty when nothing is.
std::string working();
/// Whether Windows refused an install or an uninstall for access since Megler started, and has done none since: Megler has
/// not got packageManagement (appPackage::accessDeniedText), and the Library says so once for all its apps.
bool installsRefused();

/// The list of the apps' packages, which Windows's package manager gives (on an Xbox, the console's Device Portal:
/// appPackage::installedApps): ready once it came (or once its lister has answered at all), unavailable when it could not be
/// had, unknown before it has been asked for.
enum class List { unknown, ready, unavailable };
List list();
/// The list asked for again (or the asking under way waited for), and `then` called on the UI thread once it has come or
/// failed to: list() says which. UI thread.
void askList(AppShell& shell, std::function<void()> then);
/// Whether an install may begin: only once Megler knows which of its apps are installed (the owner's wish, 2026-11-02 --
/// without the list an install went as far as making the package and then failed). Otherwise false: asked for before the
/// first list came, `retry` is called once it has (with none, the shell says to try again in a moment); when no list can
/// be had, the shell says why (on an Xbox whose Device Portal wants a user name and password, asks for them), and the list
/// is asked for again for the next try. UI thread.
bool requireList(AppShell& shell, std::function<void()> retry = {});

/*
 * Where the apps' packages are kept: one package volume for all of Megler's apps, Google Play services and Google Play
 * Store too, and none of an app's own (appPackage::AppsStorage, docs/PER-APP-PACKAGES.md "A memory card"). Where Windows
 * has more than one -- a phone with a memory card -- and none is chosen, the first install of anything asks ("Where to
 * Keep the Apps") and goes on once it is answered; the answer is remembered (settings::appsVolume), and Megler's Settings,
 * Apps' Storage, change it since, which moves every app there.
 */
/// Whether an install may begin as far as that goes; else false, and the question is up (or the list that looks at the
/// volumes still coming): `retry` is called once it is answered with a volume, and dropped when it is put off. UI thread.
bool requirePlace(AppShell& shell, std::function<void()> retry = {});
/// Whether the question was put off ("Not Now") and is still to be answered: what Megler installs by itself (Google Play:
/// ui/GooglePlay.cpp) waits, and an install asked for by hand asks again.
bool placePutOff();
struct Storage {
    bool known = false;               // a list of the apps' packages has come since Megler started, which looks at the volumes
    appPackage::AppsStorage storage;  // the volumes, and which the apps are kept on
    std::vector<int> apps;            // how many of Megler's apps' packages are on each
    int elsewhere = 0;                // ... on another than the one they are kept on
};
Storage storage();
/// The apps are kept on the volume named `volumeName` from now on (Megler's Settings): remembered, and every package of
/// Megler's apps on another volume moved there, one after another on the worker thread. UI thread.
void keepAppsOn(AppShell& shell, const std::string& volumeName);
/// Those still elsewhere (a move Windows refused) moved again. UI thread.
void moveAllToKept(AppShell& shell);

} // namespace megler::app::ui::separateApps
