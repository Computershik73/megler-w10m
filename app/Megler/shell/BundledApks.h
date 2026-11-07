// The Google APKs Megler's package brings (its bitgapps folder: Google Play services, Google Play Store, Google Services
// Framework, microG's Google Play services), and Megler's copies of them: in the library (LocalState\apks), for the apps'
// own packages (<shared>\apks\<identity>\<token>\app.apk) and for the services apps host (<shared>\services). Each copy is
// compared byte for byte with the package's and mended where it differs (docs/PER-APP-PACKAGES.md, "The Google APKs
// Megler brings").
#pragma once

#include <string>

namespace megler::app::bundledApks {

/// The package's APK that `path` is a copy of, damaged or not, else empty: one of its size whose last 64 KB, the ZIP's
/// central directory (every entry's name, size and CRC), differ from the copy's in 64 bytes at most. Another APK's
/// directory differs in thousands.
std::wstring originalOf(const std::wstring& path);

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
