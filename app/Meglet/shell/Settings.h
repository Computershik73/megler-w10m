// The shell's few settings: "key=value" lines in files of the shared folder, which every package of Meglet's reads
// (shell/AppPackage.h, docs/PER-APP-PACKAGES.md):
//   <shared>\settings.ini                   Meglet's: what every app is given unless it has its own
//   <shared>\apps\<package>\settings.ini    one app's own, over Meglet's; a key not there is Meglet's
// Meglet's Settings change Meglet's; an app's settings, changed in Meglet on its page or in its own package from the player's
// menu, change that app's. Each package reads the files when it starts; the app's own package takes Meglet's and its own.
// Where Windows gives no shared folder, Meglet's are LocalState\settings.ini.
#pragma once

#include <optional>
#include <string>

namespace meglet::app::settings {

/// Reads the files. Once, at start-up. `ownPackage`: the Android package of an app's own package, empty in Meglet. A
/// LocalState\settings.ini of before (when they were kept there) is copied to its place in the shared folder the first time.
void load(const std::wstring& localStateDirectory, const std::wstring& sharedFolder, const std::string& ownPackage);

/// What this process goes by: its app's own, else Meglet's, else `fallback`.
std::string get(const std::string& key, const std::string& fallback);
int getInt(const std::string& key, int fallback);
/// Sets and writes this process's own: Meglet's in Meglet, its app's in an app's own package.
void set(const std::string& key, const std::string& value);

/// Meglet's, as its file has it; nothing when it has none.
std::optional<std::string> meglet(const std::string& key);
void setMeglet(const std::string& key, const std::string& value);
/// The app `package`'s own, as its file has it; nothing when it goes by Meglet's.
std::optional<std::string> app(const std::string& package, const std::string& key);
/// Sets the app's own, or, with nothing, has it go by Meglet's again.
void setApp(const std::string& package, const std::string& key, const std::optional<std::string>& value);

/// The longer side of the surface an app draws to, in pixels: 960, 1280,
/// 1600, or 0 for the panel's own size. A phone's 2560x1440 is four times the
/// pixels of 1280x720 for a GPU that was sized for games of its day.
constexpr const char* renderCap = "meglet.renderCap";
constexpr int renderCapDefault = 1280;

/// Where an app is told the phone is (player/LocationHost.h): "phone", where
/// Windows finds it, or "set", the place in locationPlace instead -- for
/// testing an app somewhere else than where the phone is.
constexpr const char* locationSource = "meglet.location";
/// The place set by hand: "latitude,longitude" in degrees, as 55.751244,37.618423.
constexpr const char* locationPlace = "meglet.location.place";
constexpr const char* locationPlaceDefault = "55.751244,37.618423";

/// The language and region apps are told (player/PhoneState.h), a BCP 47 tag
/// as ru-RU or en-US; empty: the phone's, from Windows' settings.
constexpr const char* localeTag = "meglet.locale";

/// A separate app (ui/SharedFolders.h): the names of Meglet's folders the user said "Not Now" to, "a|b".
constexpr const char* foldersNotNow = "meglet.folders.notNow";

} // namespace meglet::app::settings

namespace meglet::app::settings::location {

/// The place set by hand, when one is to be used: true with its latitude and longitude.
bool setPlace(double& latitude, double& longitude);
/// "latitude,longitude" read back: false when it is not two numbers on Earth.
bool parsePlace(const std::string& text, double& latitude, double& longitude);

} // namespace meglet::app::settings::location
