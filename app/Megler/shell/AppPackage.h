// The package of one Android app (docs/PER-APP-PACKAGES.md): made here from Megler's own files and the app's APK, signed
// (core/src/appx, megler.h), and installed by Windows's deployment engine or by the system's installer -- on an Xbox, through
// the console's own Device Portal (docs/XBOX.md); and, inside such a package, what it is.
//
// Such a package is Megler with a file MeglerApp.ini in its install folder, which says the app (its Android package and
// label), where its APK is (with the library's report and icon of it beside), and where Android's libcore and framework
// are: a folder of the publisher cache folder "shared", copied there by Megler the first time it makes a package with that
// build. The APK is in the shared folder too since 0.1.0.645 (apks\<identity>\<token>, MeglerApp.ini's apk=shared:...),
// where Megler reads it as well; before, in the package (apk=app: app\app.apk). Started, it goes straight to the app, and
// keeps its shared storage (/sdcard and the apps' own parts of it) in that folder too, where every such package finds it;
// the app's private files stay in its own LocalState.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace megler::app {
struct ApkEntry;
class PickedFile;
}

namespace megler::app::appPackage {

/// What MeglerApp.ini says; an empty package when this is Megler itself.
struct SingleApp {
    std::string package;
    std::string label;
    std::wstring apkFolder;    // the folder with app.apk and its report (in the shared folder; in the install folder before 645)
    std::wstring androidRoot;  // Android's libcore and framework, in the shared folder
    std::wstring megler;       // the package family name of the Megler that made it; empty in a package made before Megler wrote it
};
const SingleApp& single();
inline bool isSingleApp() { return !single().package.empty(); }

/// The scheme of the links by which a package of Megler's asks another (megler:install, megler:run...): the protocol this
/// package declares, which an app's own package has from the Megler that made it -- "megler" for the release's Megler,
/// another for a test identity's (meglertest) -- read from its manifest once.
const std::wstring& linkScheme();
/// The package family name of Megler's own package: this one's in Megler, the one that made it in an app's own package (its
/// MeglerApp.ini; the release's name, Megler_<publisher id>, for a package made before that was written there).
std::wstring meglerFamilyName();
/// The id the library gives the packaged APK.
inline const char* singleAppId() { return "app.apk"; }

/// The publisher cache folder "shared", which every package of Megler's publisher has; empty when Windows gives none.
std::wstring sharedFolder();
/// What a package that declares `capabilities` (megler_appx_capabilities' MEGLER_APPX_CAP_*; -1, all of Megler's) is let
/// have, in words: "the network, the pictures library, the camera".
std::string describeCapabilities(int capabilities);
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
    std::wstring apkFolder;    // the folder of the shared folder its app's APK is in, relative to it: apks\<identity>\<token>
    std::string error;         // set when it was not made
};
/// Makes the package of `entry` (`picked`: the APK picked where it is, which entry's path names for the core). Its APK, splits,
/// report and icon are put in the shared folder, where the package's app reads them: a folder of their own the first time,
/// which the app's next packages take as it is. Slow (copies Android's files once per build and the APK once): off the UI
/// thread. The first package made of an app takes the app's data in Megler along: its part of Android's storage to the
/// shared one, its private files to the shared folder's handover\<package>, for takeHandover. `progress` is told each step.
Made make(const ApkEntry& entry, const std::function<void(const std::string&)>& progress, const PickedFile* picked = nullptr);

/// What an install or an uninstall did: whether it was done, and what Windows (on an Xbox, the console's Device Portal) said,
/// or why it was not done.
struct InstallResult {
    bool ok = false;
    std::string said;
    /// Windows refused every try for access (E_ACCESSDENIED, 0x80070005), the call itself or its result: Megler has not got
    /// the restricted capability packageManagement, which Windows 10 Mobile grants a sideloaded app only with the capability
    /// part of the phone's interop unlock. What is said then is accessDeniedText, not Windows's "Access is denied.".
    bool accessDenied = false;
};

/// What Megler says where Windows refused it for access (InstallResult::accessDenied): that Windows does not let Megler
/// install apps, and on a phone that its interop unlock is not complete and how to complete it (Interop Tools); then
/// `meanwhile`, what the user can do until then: "Open the Package (below): Windows's own installer installs it." Desktop
/// Windows rarely refuses it: there, that Windows refused Megler the packageManagement capability.
std::string accessDeniedText(const std::string& meanwhile);

/// Installs `made` by Windows's deployment engine (installWithPackageManager); on an Xbox, through the console's own Device
/// Portal (shell/DevicePortal.h), the way an app installs packages there (DesktopMode_uwp's). Off the UI thread.
InstallResult install(const Made& made, const std::function<void(const std::string&)>& progress);
/// A package installed by Windows's deployment engine itself (PackageManager.AddPackageAsync, the restricted capability
/// packageManagement), which reads it where it lies: nothing of it goes through Megler's memory. Installed as the Store
/// installs an app, and as a developer's package (of which a phone takes only so many) only where that is refused. An update
/// of Megler's own package closes Megler. Not installed, with what Windows said, when every option was refused; written down
/// then as the last refusal (LocalState\install-refusal.txt, which Save All Logs takes along). Off the UI thread.
InstallResult installWithPackageManager(const std::wstring& path, const std::function<void(const std::string&)>& progress);
/// Whether `said` (an install's refusal) is Windows not trusting the certificate the package is signed with
/// (CERT_E_UNTRUSTEDROOT, CERT_E_CHAINING, TRUST_E_NOSIGNATURE's kin): the certificate is to be installed for the machine.
bool untrustedCertificate(const std::string& said);

/// An app's own package installed on the phone.
struct InstalledApp {
    std::string identity;  // its identity name, "MeglerApp...." (megler_appx_app_identity)
    std::string version;   // its version, which is that of the Megler that made it: "0.1.0.358"
    std::string fullName;  // its full name, which it is uninstalled by
};
/// The apps' own packages of Megler's publisher installed on the phone, as Windows's package manager lists them (the
/// restricted capability packageQuery; on an Xbox, as the console's Device Portal lists them); false, with why, when they
/// could not be listed. `reachable` is told as soon as the lister has answered at all, before a list that takes its time.
/// Off the UI thread.
bool installedApps(std::vector<InstalledApp>& out, std::string& error, const std::function<void()>& reachable = {});
/// What lists, installs and uninstalls the apps' packages here, for a sentence: "Windows's package manager", on an Xbox
/// "the Xbox's Device Portal".
std::string packageLister();
/// Uninstalls one by Windows's package manager (packageManagement; on an Xbox, through its Device Portal): the package goes,
/// with the app's own LocalState; what it has in the shared folder stays. Off the UI thread.
InstallResult uninstall(const InstalledApp& app);

#if defined(MEGLER_XBOX)
/// Whether the Xbox's Device Portal asked, the last time, for a user name and password Megler has not got, or refused those
/// it was given (which are forgotten then): the Library asks for them (ui/SeparateApps.cpp).
bool portalWantsSignIn();
/// The way to the Xbox's Device Portal, in sentences for the user where it could not be reached: the portal turned on in Dev
/// Home, and Megler let reach the console's own address (CheckNetIsolation's loopback exemption, from an SSH session, after
/// every start of the console).
std::string portalHelp();
#endif

/// This Megler's version, "0.1.0.358": the version of every package it makes.
std::string meglerVersion();
/// Whether version `a` ("0.1.0.354") is older than `b` ("0.1.0.358").
bool olderVersion(const std::string& a, const std::string& b);

/// A newer package of Megler's own at `appx`, its certificate at `certificate`, installed by Windows's deployment engine,
/// which reads it where it lies and closes this Megler (on an Xbox, through the console's Device Portal, which closes it
/// likewise). The try and its result are written down (LocalState\update-tried-<version>), the screen kept on meanwhile
/// (selfUpdating), and `from` says in the log where the package came from. Off the UI thread: the Pictures folder's update
/// and GitHub's (ui/Ota.h).
InstallResult installOwnUpdate(const std::wstring& appx, const std::wstring& certificate, const std::string& version, const std::string& from);

/// Megler's own update, copied to the phone over USB (MTP) into Pictures\Megler: the newest Megler_<version>_ARM.appx there,
/// when it is newer than this Megler, installed with the Megler.cer beside it by Windows's deployment engine (installOwnUpdate),
/// which closes this Megler to replace it. Tried once for each file (LocalState\update-tried-<version>): a file copied there
/// again is tried anew. Megler's own package only, not an app's. Off the UI thread: started on a thread of its own.
void installCopiedUpdate();
/// Whether installCopiedUpdate is installing the newer Megler now (the screen is kept on meanwhile: ui/AppShell).
bool selfUpdating();
/// The version installCopiedUpdate is installing ("0.1.0.610"), empty when it is not: the Library says so.
std::string selfUpdateVersion();
/// Why the newer Megler in Pictures\Megler was not installed, as the Library says it: that the copy is damaged and is to be
/// copied again, that Windows does not let Megler install it (ownUpdateRefusedText), or Windows's reason. Empty when
/// nothing failed.
std::string selfUpdateFailure();
/// What is said of Megler's own update `file` (Megler_<version>_ARM.appx) that Windows refused for access: accessDeniedText,
/// with the file to open meanwhile from Pictures\Megler in the file manager, which hands it to Windows's own installer
/// (where `inPictures`; else to download it from GitHub's releases).
std::string ownUpdateRefusedText(const std::string& version, const std::string& file, bool inPictures);
/// Megler's own update downloaded from GitHub (ui/Ota.cpp) that Windows refused for access: moved, with its certificate, from
/// LocalState\update to Pictures\Megler, where the file manager opens it and where Megler tries it again at each start
/// (installCopiedUpdate) -- by itself, once the phone's interop unlock is complete; and what the Library says
/// (selfUpdateFailure) set to that. Whether it is there now. Off the UI thread.
bool keepRefusedUpdate(const std::wstring& appx, const std::wstring& certificate, const std::string& version);
/// Whether Megler's last try of its update to `version` was refused for access (LocalState\update-tried-<version>): GitHub's
/// package of it is not offered again by itself (ui/Ota.cpp), for it waits in Pictures\Megler.
bool updateRefusedForAccess(const std::string& version);
/// Megler: the Android files of older builds in the shared folder (<shared>\android\<version>, on desktop Windows
/// <version>-<ABI>, some 170 MB each) that no
/// package in `installed` (installedApps' whole list) is of any more -- after Update All, every older build's. Slow:
/// off the UI thread.
void removeUnusedAndroidRoots(const std::vector<InstalledApp>& installed);

/// An installed app's own package's APK, app\app.apk in its install folder, beside Megler's (the folder of the packages
/// installed on the phone's storage), and whether Megler can read it there. When it can, Megler keeps no copy of the APK: an
/// update is made from the installed one (Library, ui/SeparateApps.cpp).
std::wstring installedApk(const std::string& fullName);
bool canRead(const std::wstring& apk);
/// An app's split APKs (the folder `apk`.splits), by their files' names, as the report beside `apk` (`apk`.json) names them;
/// none for an app in one APK. A package installed as the Store installs an app (installWithPackageManager) lets another
/// package open some files of its app by their paths, but shows it none of its folders: their attributes and lists are
/// refused (a Lumia 650, 640: TikTok Lite's splits were not seen, though its own package started with them).
std::vector<std::wstring> reportedSplits(const std::wstring& apk);

/// Whether `path` is in the shared folder's apks, where apps' APKs are kept for their packages (make).
bool inSharedApks(const std::wstring& path);
/// After `made` was installed: its folder of the shared folder (Made::apkFolder) written down as its package's
/// (apks\<identity>\installed.txt), which removeUnusedSharedApks keeps while the package is installed.
void sharedApkInstalled(const Made& made);
/// The folders of apps' APKs in the shared folder that no installed package was made with (installed.txt, while its package
/// is in `installed`) and the library does not read (`used`, full paths), removed: a later APK's package was installed, the
/// app uninstalled, a package never installed. One a running app has open stays until the next time. Off the UI thread,
/// while no package is being made.
void removeUnusedSharedApks(const std::vector<InstalledApp>& installed, const std::vector<std::wstring>& used);

/// Megler's own storage (/sdcard, and each app's part of it) is the shared folder's, as the apps' own packages' is: true
/// once shareMeglersStorage has copied Megler's of before there (LocalState\storage-in-shared.txt). From the start after.
bool meglersStorageShared();
/// Megler, once: its storage of before (LocalState\sdcard, LocalState\apps\<package>\sdcard) copied to the shared folder, a
/// file already there as large left, and an app's part left out where its own package has had it handed over (that app's
/// is the one in the shared folder). What it copied stays in LocalState. Slow: off the UI thread.
void shareMeglersStorage();

/// Hands a file to Windows to open as it opens one in the file manager: a package to its installer, a certificate to its
/// own. On the UI thread; `done` is told whether Windows took it. (Desktop Windows opens no certificate an app hands it:
/// Launcher refuses the file type, and Megler said "Windows didn't open it" -- see trustCommand.)
void openWithSystem(const std::wstring& path, std::function<void(bool)> done);

/// The certificate the apps' packages are signed with (LocalState\signing\MeglerApps.cer, made with the first package).
std::wstring signingCertificate();
/// What makes this machine trust it, which only an administrator may: PowerShell's Import-Certificate of it into the
/// machine's Trusted People. Desktop Windows refuses an app's packages until then (0x800b0109); a phone in Developer mode
/// installs them as developer packages without it, and the app's page opens the certificate for Windows to install.
std::string trustCommand();
/// Shows the certificate's folder in File Explorer, the certificate selected (opened there, it has Install Certificate).
/// On the UI thread; `done` is told whether Windows took it.
void showSigningCertificate(std::function<void(bool)> done);

}  // namespace megler::app::appPackage
