// Where Megler's packages and their folders are: on which volume a path is, which package volume Megler itself is installed
// on, and which one its apps' packages are kept on -- one for them all, which the user chooses once where there is more than
// one (shell/AppPackage.cpp, appsStorage and installWithPackageManager; docs/PER-APP-PACKAGES.md, "A memory card").
//
// Windows 10 keeps a package installed on another volume than the system's with its app data on that volume. Its publisher
// cache folder, Megler's shared folder where Android's files and the apps' APKs are, was feared to be that volume's own, or
// none; what is known since says it is the user's, one wherever a package is (a Lumia 950's Google Play Store in the phone's
// memory read there what its Megler had put, 0.1.0.696, and Windows makes the folder's path of the user's profile folder and
// the publisher's id alone). An app's own package still says what it lacks where it finds neither (missingFor).
#pragma once

#include <string>
#include <vector>

namespace megler::app::placement {

/// The root of the volume `path` is on, as the path names it: "C:" for "C:\Data\...", "D:" for "\\?\D:\...",
/// "\\?\Volume{...}" for a volume's own name, "\\server\share" for a share; empty for a path that is not absolute.
std::wstring driveOf(const std::wstring& path);

/// A package volume as Windows's package manager lists it (PackageManager.FindPackageVolumes).
struct Volume {
    std::wstring name;        // its volume's name: "\\?\Volume{...}"
    std::wstring storePath;   // where its packages are installed: "C:\Program Files\WindowsApps", "D:\WindowsApps"
    std::wstring mountPoint;  // where it is mounted: "C:\", "D:\"
    bool system = false;      // Windows's own: a phone's memory
    bool offline = false;     // not there now: a memory card taken out
};

/// Of `volumes`, the index of the one `installFolder` (Package.InstalledLocation: "D:\WindowsApps\Megler_...") is on: the
/// longest store path the folder is in; else the one whose name is `installVolume`, the volume the folder's own handle says it
/// is on ("\\?\Volume{...}": where Windows names the store path by another drive than the folder's path has, as a phone's
/// data volume may be "U:\" and "C:\Data\" both); else the longest mount point the folder is under; -1 for none. Names are
/// compared without regard to case, "\\?\" before a drive and a separator at the end.
int volumeHolding(const std::vector<Volume>& volumes, const std::wstring& installFolder, const std::wstring& installVolume = {});

/// Of `volumes`, the index of the one named `name` ("\\?\Volume{...}", compared as volumeHolding compares); -1 for none, or
/// for no name.
int volumeNamed(const std::vector<Volume>& volumes, const std::wstring& name);

/// Where Megler's apps' packages are kept, one volume for them all.
struct AppsPlace {
    int volume = -1;      // of the volumes, the one a new package is installed on; -1: Windows is told none
    bool chosen = false;  // it is the one the user chose
    bool ask = false;     // the user is to be asked before the next install: more than one to choose of, and none chosen
};
/// Where the apps are kept: of `volumes`, the one the user chose (`chosenName`, a volume's name, as Megler's settings keep
/// it) while it is there; else Megler's own (`meglers`, an index or -1), and the user is asked where two or more volumes are
/// there to choose of -- also when the one chosen is gone (a card taken out, another put in) -- and not where there is one.
AppsPlace appsPlace(const std::vector<Volume>& volumes, const std::wstring& chosenName, int meglers);

/// What a package volume is called on Megler's screens: "The phone's memory" and "The memory card (D:)" on a phone, "Drive
/// C: (Windows's)" and "Drive D:" on desktop Windows; its store path where it has no drive.
std::string volumeTitle(const Volume& volume, bool mobile);

/// The volume `folder` is on, as the handle of the folder says it ("\\?\Volume{...}"); empty where Windows does not say.
std::wstring volumeOf(const std::wstring& folder);

/// Whether `folder` is on the drive Windows itself is on (GetSystemDirectory's): a phone's own memory ("C:\Data\..."), not a
/// memory card ("D:\..."). True where it cannot be told.
bool onSystemDrive(const std::wstring& folder);

/// The folders a package has, and their drives, for the log: "the install folder on D: (\\?\Volume{...}), LocalState on D:,
/// the shared folder on C: (C:\Data\...\shared); Windows on C:".
std::string summary(const std::wstring& install, const std::wstring& local, const std::wstring& shared);

/// What an app's own package lacks of what Megler put in the shared folder for it, by what it finds there.
enum class Missing {
    nothing,
    sharedFolder,  // Windows gave the package no shared folder
    apk,           // the app's APK is not in it, or cannot be read there
    androidFiles,  // Android's libcore and framework of the Megler that made it are not in it (no file `complete`)
};
/// What is missing for an app's own package whose shared folder is `shared`, its APK `apk` (MeglerApp.ini's apk=shared:..., in
/// it; before 0.1.0.645 in the package's own install folder) and its Android files `androidRoot` (empty where MeglerApp.ini
/// names none: not looked for). The APK is read, not only looked for: one of another storage's that Windows encrypted for
/// another package is there and cannot be read.
Missing missingFor(const std::wstring& shared, const std::wstring& apk, const std::wstring& androidRoot);
/// What an app's own package says on its screen for `missing` (not nothing): what it lacks and why, and what to do -- keep the
/// app where Megler is, by Megler's Settings (Apps' Storage, which moves the apps) or Windows's (the app moved), or the app
/// installed again from Megler.
std::string missingText(Missing missing, const std::string& label, const std::wstring& shared, bool mobile);

/// What Megler says once at its start, and above its Library, where Windows gave it no shared folder (`noSharedFolder`): no
/// app can be installed as an app of its own, and where Megler should be then -- a phone's memory where it is on the memory
/// card (`onCard`: not on the drive Windows is on; a phone's only, `mobile`). Empty with a shared folder, wherever Megler
/// is: until 0.1.0.699 Megler on the card said that the phone's memory is where it should be, which nothing bore out.
std::string meglerOnCardText(bool onCard, bool noSharedFolder, bool mobile);

} // namespace megler::app::placement
