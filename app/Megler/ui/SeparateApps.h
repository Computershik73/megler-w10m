// The library's apps as apps of the phone's own (shell/AppPackage.h, docs/PER-APP-PACKAGES.md): which are installed and
// which Megler made them, as the phone's Device Portal lists them, and installing, updating and uninstalling them -- one
// at a time, on a worker thread, whichever page is shown. Megler is where they are managed; it runs an app itself only in
// an auto run (auto/AutoRun.h).
//
// A package carries the Megler that made it: an update of Megler updates none of them, so those made by another Megler are
// "outdated", and Update All makes and installs each of them again.
#pragma once

#include "shell/AppPackage.h"

#include <string>
#include <vector>

namespace megler::app::ui {
class AppShell;
}

namespace megler::app::ui::separateApps {

enum class Installed { unknown, no, yes };

struct Status {
    Installed installed = Installed::unknown; // unknown until the portal has answered, or when it could not be asked
    std::string version;                      // the installed package's: the Megler that made it
    bool outdated = false;                    // made by another Megler than this one
    bool queued = false;                      // an action of it is waiting or going on
    std::string step;                         // what is being done, or how the last action ended
    appPackage::Made made;                    // the last package made of it here
};

/// Asks the portal which are installed, on a worker thread (once at a time). UI thread.
void refresh(AppShell& shell);
/// Goes up with every change of what this module knows.
unsigned generation();
/// Why the portal could not be asked the last time; empty when it answered (or has not been asked).
std::string portalProblem();
Status status(const std::string& androidPackage);

/// Makes the app's package and installs it through the portal. When the portal cannot install it, the package stays
/// made, for Windows's installer. UI thread.
void install(AppShell& shell, const std::string& apkId);
void uninstall(AppShell& shell, const std::string& androidPackage);
/// The library's apps installed from another Megler's packages: each installed again.
std::vector<std::string> outdatedIds();
void updateAll(AppShell& shell);
/// What is going on, "Updating Instagram (2 of 3): Packing…"; empty when nothing is.
std::string working();

} // namespace megler::app::ui::separateApps
