// The package of one Android app (docs/PER-APP-PACKAGES.md): made here from Megler's own files and the app's APK, signed
// (core/src/appx, megler.h), and installed through the phone's Device Portal or by the system's installer; and, inside such
// a package, what it is.
//
// Such a package is Megler with a file MeglerApp.ini in its install folder, which says the app (its Android package and
// label), where its APK is (app\app.apk, with the library's report and icon of it beside), and where Android's libcore and
// framework are: a folder of the publisher cache folder "shared", copied there by Megler the first time it makes a package
// with that build. Started, it goes straight to the app, and keeps its shared storage (/sdcard and the apps' own parts of
// it) in that folder too, where every such package finds it; the app's private files stay in its own LocalState.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace megler::app {
struct ApkEntry;
}

namespace megler::app::appPackage {

/// What MeglerApp.ini says; an empty package when this is Megler itself.
struct SingleApp {
    std::string package;
    std::string label;
    std::wstring apkFolder;    // the folder with app.apk and its report (in the install folder)
    std::wstring androidRoot;  // Android's libcore and framework, in the shared folder
};
const SingleApp& single();
inline bool isSingleApp() { return !single().package.empty(); }
/// The id the library gives the packaged APK.
inline const char* singleAppId() { return "app.apk"; }

/// The publisher cache folder "shared", which every package of Megler's publisher has; empty when Windows gives none.
std::wstring sharedFolder();
/// Remember an installed microG APK for clients of the same publisher; no service APK is bundled into each client.
bool registerServiceApk(const ApkEntry& entry, const std::string& installedFullName, bool replace = false);
void unregisterServiceApk(const std::string& installedFullName);
/// The copies of Google Play services' APK in <shared>\services that are not the registered one, removed: one was made for
/// each install of its package (every Update All), some 266 MB each, and none ever went. A copy a running app still has
/// open, which Windows does not let go, stays until the next time. Slow: off the UI thread.
void removeUnusedServiceCopies();
/// The APKs apps' own packages handed over for Megler to install (<shared>\incoming) that are over an hour old: what was
/// left there before Megler removed each once its request was answered (ui/AppShell.cpp). Slow: off the UI thread.
void removeOldHandedApks();
/// What the shared folder holds, said in the log once a run: each of its folders' size (an app's own storage the largest).
/// The Device Portal cannot look there. Slow: off the UI thread.
void logSharedSpace();
/// Google Play services' APK an app starts with: microG's when `microg` and it is registered, else Google's own, else
/// microG's; nothing when neither is. Each kind is registered apart (registerServiceApk), the last installed of each.
std::wstring serviceApk(bool microg = false);
/// Whether a microG's Google Play services is registered.
bool hasMicroG();
/// microG's GmsCore.apk: the one registered by microG's own package, else the one published in the shared folder; empty when none.
std::wstring microgApk();
/// Whether Google's own Google Play services is installed as an app of its own (registered for the apps to start with).
bool hasGoogleServices();
std::wstring gsfApk();

/// An app's own package, before its app starts: the app's private files that Megler handed over when it made the package
/// (the shared folder's handover\<package>, see make) copied into its LocalState in place of its own, which are kept
/// aside (data.before-megler). Nothing when there is no handover. Off the UI thread.
void takeHandover();

struct Made {
    std::wstring appx;         // the package, in LocalState\packages
    std::wstring certificate;  // the certificate it is signed with (the phone has to trust it), beside it
    std::string identity;      // its identity name
    std::string fullName;      // its full name once installed: the name of its install folder
    std::string error;         // set when it was not made
};
/// Makes the package of `entry`. Slow (copies Android's files once per build, reads the whole APK): off the UI thread.
/// The first package made of an app takes the app's data in Megler along: its part of Android's storage to the shared one,
/// its private files to the shared folder's handover\<package>, for takeHandover. `progress` is told each step.
Made make(const ApkEntry& entry, const std::function<void(const std::string&)>& progress);

/// What the Device Portal did: whether it did it, and what it said, or why it did not.
struct PortalResult {
    bool ok = false;
    std::string said;
};

/// Installs `made` through the phone's own Device Portal, https://127.0.0.1 (its certificate, which the phone made for
/// itself, taken as it is), with the certificate. Off the UI thread.
PortalResult installThroughPortal(const Made& made, const std::function<void(const std::string&)>& progress);

/// An app's own package installed on the phone.
struct InstalledApp {
    std::string identity;  // its identity name, "MeglerApp...." (megler_appx_app_identity)
    std::string version;   // its version, which is that of the Megler that made it: "0.1.0.358"
    std::string fullName;  // its full name, what the Device Portal uninstalls it by
};
/// The apps' own packages of Megler's publisher installed on the phone, as the Device Portal lists them; false, with why,
/// when it could not be asked. Off the UI thread.
bool installedApps(std::vector<InstalledApp>& out, std::string& error, const std::function<void()>& reachable = {});
/// Uninstalls one through the Device Portal: the package goes, with the app's own LocalState; what it has in the shared
/// folder stays. Off the UI thread.
PortalResult uninstallThroughPortal(const InstalledApp& app);

/// This Megler's version, "0.1.0.358": the version of every package it makes.
std::string meglerVersion();

/// Megler's own update, copied to the phone over USB (MTP) into Pictures\Megler: the newest Megler_<version>_ARM.appx there,
/// when it is newer than this Megler, installed with the Megler.cer beside it through the phone's own Device Portal, which
/// closes this Megler to replace it. Tried once for each version (LocalState\update-tried-<version>). Megler's own package
/// only, not an app's. Off the UI thread: started on a thread of its own.
void installCopiedUpdate();
/// Whether installCopiedUpdate is sending the newer Megler to the portal now (the screen is kept on meanwhile: ui/AppShell).
bool selfUpdating();
/// The version installCopiedUpdate is installing ("0.1.0.610"), empty when it is not: the Library says so.
std::string selfUpdateVersion();
/// Megler: the Android files of older builds in the shared folder (<shared>\android\<version>, some 170 MB each) that no
/// package in `installed` (the Device Portal's whole list) is of any more -- after Update All, every older build's. Slow:
/// off the UI thread.
void removeUnusedAndroidRoots(const std::vector<InstalledApp>& installed);

/// An installed app's own package's APK, app\app.apk in its install folder, beside Megler's (the folder of the packages
/// installed on the phone's storage), and whether Megler can read it there. When it can, Megler keeps no copy of the APK: an
/// update is made from the installed one (Library, ui/SeparateApps.cpp).
std::wstring installedApk(const std::string& fullName);
bool canRead(const std::wstring& apk);

/// Megler's own storage (/sdcard, and each app's part of it) is the shared folder's, as the apps' own packages' is: true
/// once shareMeglersStorage has copied Megler's of before there (LocalState\storage-in-shared.txt). From the start after.
bool meglersStorageShared();
/// Megler, once: its storage of before (LocalState\sdcard, LocalState\apps\<package>\sdcard) copied to the shared folder, a
/// file already there as large left, and an app's part left out where its own package has had it handed over (that app's
/// is the one in the shared folder). What it copied stays in LocalState. Slow: off the UI thread.
void shareMeglersStorage();

/// Hands a file to Windows to open as it opens one in the file manager: a package to its installer, a certificate to its
/// own. On the UI thread; `done` is told whether Windows took it.
void openWithSystem(const std::wstring& path, std::function<void(bool)> done);

}  // namespace megler::app::appPackage
