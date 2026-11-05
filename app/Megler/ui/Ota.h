// Megler's own updates over the air, from its releases on GitHub (Computershik73/megler-w10m; the owner's wish,
// 2026-11-05).
//
// Megler asks GitHub's API for the releases when it starts, at most once in six hours (Settings: at start, or only when
// asked), and takes the newest that has a package for this Megler: a release's assets are named by the Windows and the
// processor they are for (docs/RELEASES.md), and only that name is taken:
//   Megler_<version>_ARM.appx     Windows 10 Mobile, 32-bit ARM (every release so far)
//   Megler_<version>_ARM64.appx   desktop Windows 10 and 11 on 64-bit ARM, and on x64 and x86 likewise:
//   Megler_<version>_x64.appx, Megler_<version>_x86.appx
// A 32-bit ARM package of another Windows (Windows RT's, should there be one) is named otherwise (_RT_ARM), so a phone
// never takes it, nor a PC the phone's; a 32-bit ARM Megler on desktop Windows takes the ARM64 one, there being no
// 32-bit ARM package for the desktop. A newer one is offered in a dialog, with its size and what the release says is
// new; taken, it is downloaded into LocalState\update, its size and its SHA-256 checked against what GitHub says of the
// asset, and installed as Megler's own update from Pictures\Megler is (shell/AppPackage.h, installOwnUpdate): Windows's
// deployment engine closes this Megler, and Megler is opened again once Windows has installed it. Not in an app's own
// package. Off the UI thread, all of it but the dialog.
#pragma once

#include <cstdint>
#include <string>

namespace megler::app::ui {
class AppShell;
}

namespace megler::app::ui::ota {

/// The part of a package's name that says which Windows and processor it is for, as this Megler needs: "ARM" on a phone,
/// "ARM64", "x64" or "x86" on desktop Windows; empty where GitHub has none for it (and nothing is asked).
std::string packageSuffix();
/// At every poll (UI thread): GitHub asked when it is time, and a newer Megler offered; once taken, downloaded, checked
/// and installed.
void tick(AppShell& shell);
/// Settings' Check Now: GitHub asked at the next poll, and a newer Megler offered even if "Later" was said to it.
void checkNow();
/// Whether GitHub is asked at start (Settings): yes unless turned off.
bool atStart();
void setAtStart(bool on);
/// Where it stands, in a line, for Settings.
std::string summary();
/// While a newer Megler is downloaded: true, with its version, what has come and its size (the Library's card).
bool downloading(std::string& version, uint64_t& got, uint64_t& total);

} // namespace megler::app::ui::ota
