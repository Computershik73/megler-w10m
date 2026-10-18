#include "pch.h"

#include "shell/Settings.h"

#include "shell/Files.h"

#include <map>

namespace megler::app::settings {
namespace {

std::mutex gMutex;
std::wstring gPath;
std::map<std::string, std::string> gValues;

std::string trimmed(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace

void load(const std::wstring& localStateDirectory) {
    std::lock_guard<std::mutex> lock(gMutex);
    gPath = localStateDirectory + L"\\settings.ini";
    gValues.clear();
    std::string text;
    if (!files::read(gPath, text)) return;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trimmed(line.substr(0, equals));
        if (!key.empty() && key[0] != '#') gValues[key] = trimmed(line.substr(equals + 1));
    }
}

std::string get(const std::string& key, const std::string& fallback) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gValues.find(key);
    return it == gValues.end() ? fallback : it->second;
}

int getInt(const std::string& key, int fallback) {
    const std::string value = get(key, std::string());
    if (value.empty()) return fallback;
    char* end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    return end && *end == 0 ? int(parsed) : fallback;
}

void set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(gMutex);
    gValues[key] = value;
    if (gPath.empty()) return;
    std::string text;
    for (const auto& [k, v] : gValues) text += k + "=" + v + "\r\n";
    files::write(gPath, text);
}

} // namespace megler::app::settings

namespace megler::app::settings::location {

bool parsePlace(const std::string& text, double& latitude, double& longitude) {
    const size_t comma = text.find(',');
    if (comma == std::string::npos) return false;
    const std::string a = text.substr(0, comma), b = text.substr(comma + 1);
    char* endA = nullptr;
    char* endB = nullptr;
    const double lat = strtod(a.c_str(), &endA), lon = strtod(b.c_str(), &endB);
    auto rest = [](const char* p) {
        while (p && (*p == ' ' || *p == '\t')) p++;
        return p && *p == 0;
    };
    if (endA == a.c_str() || endB == b.c_str() || !rest(endA) || !rest(endB)) return false;
    if (!(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0)) return false;
    latitude = lat;
    longitude = lon;
    return true;
}

bool setPlace(double& latitude, double& longitude) {
    if (get(locationSource, "phone") != "set") return false;
    return parsePlace(get(locationPlace, locationPlaceDefault), latitude, longitude);
}

} // namespace megler::app::settings::location
