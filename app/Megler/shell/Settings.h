// The shell's few settings: LocalState\settings.ini, "key=value" lines, so a
// phone's can be read and changed over the Device Portal.
#pragma once

#include <string>

namespace megler::app::settings {

/// Reads LocalState\settings.ini. Once, at start-up.
void load(const std::wstring& localStateDirectory);

std::string get(const std::string& key, const std::string& fallback);
int getInt(const std::string& key, int fallback);
/// Sets and writes the file.
void set(const std::string& key, const std::string& value);

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

} // namespace megler::app::settings

namespace megler::app::settings::location {

/// The place set by hand, when one is to be used: true with its latitude and longitude.
bool setPlace(double& latitude, double& longitude);
/// "latitude,longitude" read back: false when it is not two numbers on Earth.
bool parsePlace(const std::string& text, double& latitude, double& longitude);

} // namespace megler::app::settings::location
