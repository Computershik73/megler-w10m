// Android's own Settings app (AOSP 9's, com.android.settings), which Megler's package brings (platform-apps\AndroidSettings.apk,
// tools/build-appx.sh) at the owner's wish (2026-11-19: "сделай настоящий apk настроек, такой же как в aosp").
//
// Megler puts it in the library and installs it as an app of its own, once: the phone's Android Settings, which the user
// opens from the Library and apps open for their system screens. It runs as an app of the platform's own (sharedUserId
// android.uid.system): Megler's framework answers what it asks of the system (megler.PlatformInterfaces, Android 9's
// framework after Megler's, docs/JAVA.md), and what it changes -- the default browser, an app's links -- is Megler's own
// (defaults.ini). Removed by the user, it is not installed again by itself; the library keeps its APK to install it from.
#pragma once

namespace megler::app::ui {
class AppShell;
}

namespace megler::app::ui::androidSettings {

/// Megler, at every poll (UI thread): the package's Android Settings put in the library when it is not there, and installed
/// once, when Windows has listed the phone's apps and nothing else is being installed.
void tick(AppShell& shell);

} // namespace megler::app::ui::androidSettings
