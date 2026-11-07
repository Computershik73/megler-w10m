// Every package of Megler's -- Megler itself and each app's own package (shell/AppPackage.h) -- keeps copies of its logs in
// the publisher cache folder every one of them shares, <shared>\logs\<its package identity name>\, where Save All Logs
// (shell/LogArchive.h) finds them all: a package cannot read another's LocalState, and a phone without the Device Portal
// has no other way to them.
//
//   at start (ten seconds in, out of the way of the start's own reading, or at once when it is suspended), after
//     shellLog::initialize has put the runs before aside: those runs' logs, which are whole -- the mirror's copy of the run
//     before, rotated as LocalState's was, gets the lines it lacked (those after its last copy, a crash's among them), and
//     is copied anew only where it is no beginning of LocalState's;
//   while the package runs: megler.log's new lines every 30 s, and at once as it is suspended and as it exits; the other
//     files (names()) whenever they have changed, and flags.txt, the names of its flags (LocalState\flags);
// so a run that ended in a crash is there whole from the package's next start, and the part before the crash at once.
//
// Cheap, and never in the app's way: the files names() has that are there, only what is new of megler.log, on a thread of
// its own below normal priority, never on the UI thread; whatever fails is said at most three times in the log and the
// mirror goes on. A log over capBytes is kept as its last capBytes (copyTail's note says what was left out); the running
// log's copy stops there, and its end comes whole, as such a tail, at the next start. Megler removes the folder of a
// package that is no longer installed when it lists the apps' packages.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace megler::app::appPackage {
struct InstalledApp;
}

namespace megler::app::logMirror {

/// The most of a log that is copied, into a package's folder and into Save All Logs' ZIP: its last 16 MB.
constexpr uint64_t capBytes = 16ull << 20;

/// The files of a package's LocalState its logs are: megler.log and the two runs before it, megler-boot.txt, the checks'
/// logs, the old settings.ini, flags-from-pictures.txt, storage-in-shared.txt, install-refusal.txt.
const std::vector<std::wstring>& names();
/// <shared>\logs, which has a folder for each package; empty where Windows gives no shared folder.
std::wstring root();
/// This package's identity name, its folder's: "Megler", "MeglerApp.org.telegram.messenger.web".
std::wstring identity();
/// The names of this package's flags (LocalState\flags), a line each; empty when it has none.
std::string flagsText();
/// What this package is, for its folder and the ZIP: its full name, the Megler it is, its app.
std::string packageText();

/// Where a log of `size` bytes, over capBytes, is copied from: the first line that begins in its last capBytes.
uint64_t tailStart(const std::wstring& path, uint64_t size);
/// The line such a copy begins with, which says what was left out of it.
std::string tailNote(uint64_t start, uint64_t size);

/// The mirror started, on a thread of its own. Once a run, after shellLog::initialize.
void start();
/// megler.log's new lines copied now, waited for 1 s at most: as the package is suspended, and as it exits.
void now();
/// Megler, with the list of the apps' packages: the folders of those no longer installed removed. Off the UI thread.
void removeUninstalled(const std::vector<appPackage::InstalledApp>& installed);

} // namespace megler::app::logMirror
