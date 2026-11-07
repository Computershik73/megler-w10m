// The Google APKs Megler's package brings (its bitgapps folder: Google Play services, Google Play Store, Google Services
// Framework, microG's Google Play services), and Megler's copies of them: in the library (LocalState\apks), for the apps'
// own packages (<shared>\apks\<identity>\<token>\app.apk) and for the services apps host (<shared>\services). Each copy is
// compared byte for byte with the package's and mended where it differs (docs/PER-APP-PACKAGES.md, "The Google APKs
// Megler brings").
#pragma once

#include <string>

namespace megler::app {
struct ApkEntry;
}

namespace megler::app::bundledApks {

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
/// what had to be written again said in the log; false, and no .part left, when it could not be.
bool copy(const std::wstring& source, const std::wstring& target);

/// Megler's copies compared with the package's APKs and mended (files::mend), on a thread of their own at background
/// priority: once for each build of Megler (LocalState\bundled-apks-checked.txt), and at every start with the flag
/// check-bundled-apks, which also has the log say each file's SHA-1 (as Google Play Store says its own) and SHA-256, and
/// each byte of a copy that differed. In an app's own package, which has no bitgapps, that flag has the log say the size,
/// SHA-1 and SHA-256 of the app's own APK as that app reads it.
void checkLater();

}  // namespace megler::app::bundledApks
