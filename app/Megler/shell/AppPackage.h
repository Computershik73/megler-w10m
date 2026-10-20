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

struct Made {
    std::wstring appx;         // the package, in LocalState\packages
    std::wstring certificate;  // the certificate it is signed with (the phone has to trust it), beside it
    std::string identity;      // its identity name
    std::string error;         // set when it was not made
};
/// Makes the package of `entry`. Slow (copies Android's files once per build, reads the whole APK): off the UI thread.
/// `progress` is told each step.
Made make(const ApkEntry& entry, const std::function<void(const std::string&)>& progress);

/// Installs `made` through the phone's own Device Portal, https://127.0.0.1 (its certificate, which the phone made for
/// itself, taken as it is), with the certificate. What the portal said in the end, or why it did not. Off the UI thread.
std::string installThroughPortal(const Made& made, const std::function<void(const std::string&)>& progress);

/// Hands a file to Windows to open as it opens one in the file manager: a package to its installer, a certificate to its
/// own. On the UI thread; `done` is told whether Windows took it.
void openWithSystem(const std::wstring& path, std::function<void(bool)> done);

}  // namespace megler::app::appPackage
