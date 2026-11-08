// The Google APKs Megler's package brings (its bitgapps folder: Google Play services, Google Play Store, Google Services
// Framework, microG's Google Play services), and Megler's copies of them: in the library (LocalState\apks), for the apps'
// own packages (<shared>\apks\<identity>\<token>\app.apk) and for the services apps host (<shared>\services). Each copy is
// compared byte for byte with the package's and mended where it differs (docs/PER-APP-PACKAGES.md, "The Google APKs
// Megler brings").
#pragma once

#include "shell/Files.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace megler::app {
struct ApkEntry;
}

namespace megler::app::bundledApks {

/// A copy of one of the package's APKs that Megler makes after its window is shown (Library::load plans them, start
/// makes them). A first start's are hundreds of megabytes -- the better part of a minute on a slow phone -- and Windows
/// ends an app whose window it has not been shown in time, with no word to the app: a Lumia 630's Megler 0.1.0.671 made
/// them on its UI thread before its window was shown, was ended at every start, and began them again at the next.
struct Task {
    enum class Kind {
        service, // Google Services Framework or microG's Google Play services, for the apps: <shared>\services
        offer,   // Google Play services or Google Play Store offered in the library, once (LocalState\apks\bitgapps.added)
        again,   // the library's copy, which is not the package's (a copy that never finished, an older one)
        recover, // the library's entry of an app whose package Windows took away: its APK put back from the package's
    };
    Kind kind = Kind::service;
    std::wstring source, target;
    std::string id;   // the library's entry (bitgapps-*.apk), for offer, again and recover
    std::string what; // in words: "Google Play services"
};

/// The copy being made, for the library's card: what, which of how many (start's; 0 for another's), and how far.
struct Progress {
    bool active = false;
    std::string what;
    int index = 0, count = 0;
    files::Stage stage = files::Stage::copying;
    uint64_t done = 0, total = 0;
};
Progress progress();
/// Whether start()'s copies are being made: Google Play's install waits for them (ui/GooglePlay.cpp).
bool copying();

/// `tasks` made in turn on a thread of their own at background priority, each with copy() (said in the log before it
/// begins, with its times after); `done` told of each on that thread, with whether it was made. Then checkLater().
void start(std::vector<Task> tasks, std::function<void(const Task& task, bool copied)> done);

/// The package's APK that `path` is a copy of, damaged or not, else empty: one of its size whose last 64 KB, the ZIP's
/// central directory (every entry's name, size and CRC), differ from the copy's in 64 bytes at most. Another APK's
/// directory differs in thousands.
std::wstring originalOf(const std::wstring& path);

/// The package's APK of Google's app that `entry` is -- Google Play services (Google's own, not microG's) or Google Play
/// Store -- when it is to take the place of the entry's APK in the app's next package: of a later version, or of the same
/// release (Google's version codes of one release differ in their last three digits, which name the variant) with more of
/// this processor's libraries, the variant of this processor where the entry's is another's. An earlier Megler installed
/// the app from its own package's APK, or the user from another; a later Megler's then never reached it, as Update All
/// makes the next package from the APK the app has (a phone's package carried the arm64-v8a variant of Google Play
/// services, 26.41.30, whose armeabi-v7a code is 8 of its 40 libraries). An older one is not put over a newer, as Android
/// keeps an update of a system app over an older system image. `path` empty: none; `report` its megler_apk_report JSON,
/// `why` in words. Reads the package's APK when it differs from the entry's (seconds, for Google Play services on a phone):
/// off the UI thread.
struct Replacement {
    std::wstring path;
    std::string report;
    std::string why;
};
Replacement replacementFor(const ApkEntry& entry);

/// The package's APK `source` (bitgapps\...) copied to `target`, through `target`.part, byte for byte (files::copyWhole),
/// what had to be written again said in the log; false, and no .part left, when it could not be. Not where the disk has
/// not the room for it. Said in the log before it begins (written out at once: Windows may end the app meanwhile), every
/// 10 s while it goes, and with its times when it ends; `what` names it there (else the file's name). Off the UI thread.
bool copy(const std::wstring& source, const std::wstring& target, const std::string& what = std::string());

/// Megler's copies compared with the package's APKs and mended (files::mend), on a thread of their own at background
/// priority: once for each build of Megler (LocalState\bundled-apks-checked.txt), and at every start with the flag
/// check-bundled-apks, which also has the log say each file's SHA-1 (as Google Play Store says its own) and SHA-256, and
/// each byte of a copy that differed. Copies copy() made whole in this run are left out (but with that flag). In an
/// app's own package, which has no bitgapps, that flag has the log say the size, SHA-1 and SHA-256 of the app's own APK as
/// that app reads it.
void checkLater();

}  // namespace megler::app::bundledApks
