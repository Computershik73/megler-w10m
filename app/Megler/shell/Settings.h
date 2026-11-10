// The shell's few settings: "key=value" lines in files of the shared folder, which every package of Megler's reads
// (shell/AppPackage.h, docs/PER-APP-PACKAGES.md):
//   <shared>\settings.ini                   Megler's: what every app is given unless it has its own
//   <shared>\apps\<package>\settings.ini    one app's own, over Megler's; a key not there is Megler's
// Megler's Settings change Megler's; an app's settings, changed in Megler on its page or in its own package from the player's
// menu, change that app's. Each package reads the files when it starts; the app's own package takes Megler's and its own.
// Where Windows gives no shared folder, Megler's are LocalState\settings.ini.
#pragma once

#include <optional>
#include <string>

namespace megler::app::settings {

/// Reads the files. Once, at start-up. `ownPackage`: the Android package of an app's own package, empty in Megler. A
/// LocalState\settings.ini of before (when they were kept there) is copied to its place in the shared folder the first time.
void load(const std::wstring& localStateDirectory, const std::wstring& sharedFolder, const std::string& ownPackage);

/// What this process goes by: its app's own, else Megler's, else `fallback`.
std::string get(const std::string& key, const std::string& fallback);
int getInt(const std::string& key, int fallback);
/// Sets and writes this process's own: Megler's in Megler, its app's in an app's own package.
void set(const std::string& key, const std::string& value);

/// Megler's, as its file had it when this process started (or Megler's Settings last changed it here); nothing when it has
/// none.
std::optional<std::string> megler(const std::string& key);
/// Megler's, as its file has it now: another package may have written it since (an answer given in an app's own package that
/// counts for every app, as gmsLocation's).
std::optional<std::string> meglerNow(const std::string& key);
void setMegler(const std::string& key, const std::string& value);
/// The app `package`'s own, as its file has it; nothing when it goes by Megler's.
std::optional<std::string> app(const std::string& package, const std::string& key);
/// Sets the app's own, or, with nothing, has it go by Megler's again.
void setApp(const std::string& package, const std::string& key, const std::optional<std::string>& value);
/// Takes out every key of the app's own that begins with `prefix`, in one write: its permission answers, when its data is
/// cleared (player/PermissionHost.h). How many went.
int eraseApp(const std::string& package, const std::string& prefix);

/// The longer side of the surface an app draws to, in pixels: 960, 1280,
/// 1600, or 0 for the panel's own size. A phone's 2560x1440 is four times the
/// pixels of 1280x720 for a GPU that was sized for games of its day.
constexpr const char* renderCap = "megler.renderCap";
constexpr int renderCapDefault = 1280;

/// Where an app is told the phone is (player/LocationHost.h): "phone", where
/// Windows finds it, or "set", the place in locationPlace instead -- for
/// testing an app somewhere else than where the phone is.
constexpr const char* locationSource = "megler.location";
/// The place set by hand: "latitude,longitude" in degrees, as 55.751244,37.618423.
constexpr const char* locationPlace = "megler.location.place";
constexpr const char* locationPlaceDefault = "55.751244,37.618423";

/// The language and region apps are told (player/PhoneState.h), a BCP 47 tag
/// as ru-RU or en-US; empty: the phone's, from Windows' settings.
constexpr const char* localeTag = "megler.locale";

/// Whether apps are told the phone is dark (Configuration.uiMode's night, player/PhoneState.h): "light", "dark", or
/// empty: as Windows' theme is.
constexpr const char* appTheme = "megler.appTheme";

/// Which Google Play services an app starts with (shell/AppPackage.h, serviceApk): "google", Google's own, or "microg",
/// microG's, which takes far less memory and has fewer of Google's APIs. Empty: Google's own.
constexpr const char* googleServices = "megler.googleServices";

/// Megler's question at its first start (ui/GooglePlay.h): "yes", Google Play wanted -- Megler installs its Google Play
/// services and Google Play Store, apps running with microG until then; "no", apps run with microG; empty: not asked.
constexpr const char* googlePlay = "megler.googlePlay";

/// Whether Google Play services' own features may use the phone's location in every app's process (player/PermissionHost.h,
/// gmsLocation): "granted", "denied"; empty: not asked yet. Megler's alone, one answer for every app; asked in whichever
/// package Google Play services first want it, and changed in Megler's Settings and in any app's App Permissions.
constexpr const char* gmsLocation = "megler.gmsLocation";

/// A separate app (ui/SharedFolders.h): the names of Megler's folders the user said "Not Now" to, "a|b".
constexpr const char* foldersNotNow = "megler.folders.notNow";

/// Whether Megler asks GitHub for a newer Megler when it starts (ui/Ota.h): "off", only when asked (Settings' Check Now);
/// anything else, at start, at most once in six hours.
constexpr const char* otaAtStart = "megler.ota";

} // namespace megler::app::settings

namespace megler::app::settings::location {

/// The place set by hand, when one is to be used: true with its latitude and longitude.
bool setPlace(double& latitude, double& longitude);
/// "latitude,longitude" read back: false when it is not two numbers on Earth.
bool parsePlace(const std::string& text, double& latitude, double& longitude);

} // namespace megler::app::settings::location
