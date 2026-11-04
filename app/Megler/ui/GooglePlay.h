// Google Play or not, asked at Megler's first start (the owner's wish, 2026-11-04: some users' Play Store could no longer
// sign in once Google Play services had gone from their library).
//
// Wanted: Megler installs the Google Play services and the Google Play Store it brings (tools/build-appx.sh --gms
// --play-store) as apps of their own, again whenever they are not installed, putting them back in the library from its
// own package when they have gone from it. Nothing else waits for them: apps are installed and started meanwhile, and
// run with microG until Google Play services is installed (player/Session.cpp takes microG's where Google's own is not).
//
// Not wanted: every app runs with microG, and Google's own Google Play services can't be chosen for an app (Settings)
// until it is installed as an app of its own, from the Library, by hand.
#pragma once

#include <string>

namespace megler::app::ui {
class AppShell;
}

namespace megler::app::ui::googlePlay {

enum class Choice { unasked, wanted, notWanted };
/// Megler's answer, from its settings (which every package of Megler's reads).
Choice choice();
/// Whether this Megler brings Google Play services and Google Play Store.
bool bundled();
/// Megler, at every poll (UI thread): the question at the first start; once Google Play is wanted, its Google Play
/// services and Google Play Store installed when the portal says they are not, one after the other.
void tick(AppShell& shell);
/// What the Library says while Google Play is wanted and not installed yet; empty otherwise.
std::string pending();
/// Whether Google's own Google Play services may be chosen for apps (Settings): it is installed as an app of its own.
bool googleServicesChoosable();

} // namespace megler::app::ui::googlePlay
