// UTF-8 (the core's strings) <-> UTF-16 (WinRT's), and the small formatting
// the screens share.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace megler::app {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view utf16);
inline winrt::hstring hs(std::string_view utf8) { return winrt::hstring(widen(utf8)); }

/// printf into a std::string.
std::string format(const char* fmt, ...);

/// "742 KB", "12.3 MB", "1.25 GB": binary units, as file sizes on the phone read.
std::string formattedByteCount(uint64_t bytes);

/// "HH:MM:SS.mmm", local time.
std::string timeOfDay();

} // namespace megler::app
