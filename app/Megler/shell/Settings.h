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

} // namespace megler::app::settings
