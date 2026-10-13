#include "pch.h"

#include "shell/Strings.h"

#include <cstdarg>

namespace megler::app {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), &out[0], n);
    return out;
}

std::string narrow(std::wstring_view utf16) {
    if (utf16.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), int(utf16.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.data(), int(utf16.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::string format(const char* fmt, ...) {
    // (Not "small": the Windows headers define that away.)
    char fixed[512];
    va_list args;
    va_start(args, fmt);
    va_list again;
    va_copy(again, args);
    const int n = vsnprintf(fixed, sizeof(fixed), fmt, args);
    va_end(args);
    std::string out;
    if (n < 0) {
        out = fmt;
    } else if (size_t(n) < sizeof(fixed)) {
        out.assign(fixed, size_t(n));
    } else {
        out.resize(size_t(n) + 1);
        vsnprintf(&out[0], out.size(), fmt, again);
        out.resize(size_t(n));
    }
    va_end(again);
    return out;
}

std::string formattedByteCount(uint64_t bytes) {
    if (bytes < 1024) return format("%u bytes", unsigned(bytes));
    static const char* units[] = {"KB", "MB", "GB", "TB"};
    double value = double(bytes) / 1024.0;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    return format(value >= 100 || unit == 0 ? "%.0f %s" : value >= 10 ? "%.1f %s" : "%.2f %s", value, units[unit]);
}

std::string timeOfDay() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

} // namespace megler::app
