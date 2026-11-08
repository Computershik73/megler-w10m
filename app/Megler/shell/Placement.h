// Where Megler's packages and their folders are: on which volume a path is, and which package volume Megler itself is
// installed on -- every package it installs goes there (shell/AppPackage.cpp, installWithPackageManager; docs/PER-APP-PACKAGES.md,
// "A memory card").
//
// Windows 10 keeps a package installed on another volume than the system's with its app data on that volume, and gives it the
// publisher cache folder of that volume, or none: Megler in the phone's memory and an app's package on the memory card (or the
// other way round) do not see each other's shared folder, where Android's files and the app's APK are.
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
};

/// Of `volumes`, the index of the one `installFolder` (Package.InstalledLocation: "D:\WindowsApps\Megler_...") is on: the
/// longest store path the folder is in; else the one whose name is `installVolume`, the volume the folder's own handle says it
/// is on ("\\?\Volume{...}": where Windows names the store path by another drive than the folder's path has, as a phone's
/// data volume may be "U:\" and "C:\Data\" both); else the longest mount point the folder is under; -1 for none. Names are
/// compared without regard to case, "\\?\" before a drive and a separator at the end.
int volumeHolding(const std::vector<Volume>& volumes, const std::wstring& installFolder, const std::wstring& installVolume = {});

/// The volume `folder` is on, as the handle of the folder says it ("\\?\Volume{...}"); empty where Windows does not say.
std::wstring volumeOf(const std::wstring& folder);

/// Whether `folder` is on the drive Windows itself is on (GetSystemDirectory's): a phone's own memory ("C:\Data\..."), not a
/// memory card ("D:\..."). True where it cannot be told.
bool onSystemDrive(const std::wstring& folder);

/// The folders a package has, and their drives, for the log: "the install folder on D: (\\?\Volume{...}), LocalState on D:,
/// the shared folder on C: (C:\Data\...\shared); Windows on C:".
std::string summary(const std::wstring& install, const std::wstring& local, const std::wstring& shared);

} // namespace megler::app::placement
