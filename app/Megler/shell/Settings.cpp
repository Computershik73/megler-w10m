#include "pch.h"

#include "shell/Settings.h"

#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <map>

namespace megler::app::settings {
namespace {

using Values = std::map<std::string, std::string>;

std::mutex gMutex;
std::wstring gMeglerPath;     // Megler's file
std::wstring gShared;       // the shared folder, where the apps' files are; empty when Windows gives none
std::string gOwnPackage;    // an app's own package: its app; empty in Megler
Values gMegler, gOwn;

std::string trimmed(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

Values readFile(const std::wstring& path) {
    Values values;
    std::string text;
    if (path.empty() || !files::read(path, text)) return values;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trimmed(line.substr(0, equals));
        if (!key.empty() && key[0] != '#') values[key] = trimmed(line.substr(equals + 1));
    }
    return values;
}

/// Written whole beside and then put in place, so that another package reading it meanwhile finds the old or the new.
void writeFile(const std::wstring& path, const Values& values) {
    if (path.empty()) return;
    std::string text;
    for (const auto& [k, v] : values) text += k + "=" + v + "\r\n";
    const size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) files::makeDirectories(path.substr(0, slash));
    if (!files::write(path + L".new", text) || !files::move(path + L".new", path))
        shellLog::write("settings: " + narrow(path) + " could not be written");
}

std::wstring appPath(const std::string& package) {
    return gShared.empty() || package.empty() ? std::wstring() : gShared + L"\\apps\\" + widen(package) + L"\\settings.ini";
}

} // namespace

void load(const std::wstring& localStateDirectory, const std::wstring& sharedFolder, const std::string& ownPackage) {
    std::lock_guard<std::mutex> lock(gMutex);
    const std::wstring old = localStateDirectory + L"\\settings.ini";
    gShared = sharedFolder;
    gOwnPackage = ownPackage;
    gMeglerPath = gShared.empty() ? old : gShared + L"\\settings.ini";
    // Kept in LocalState until 0.1.0.358: Megler's become Megler's in the shared folder, an app's own package's its app's.
    const std::wstring mine = gOwnPackage.empty() ? gMeglerPath : appPath(gOwnPackage);
    if (mine != old && !files::exists(mine) && files::exists(old)) {
        writeFile(mine, readFile(old));
        shellLog::write("settings: " + narrow(old) + " copied to " + narrow(mine));
    }
    gMegler = readFile(gMeglerPath);
    gOwn = gOwnPackage.empty() ? Values() : readFile(appPath(gOwnPackage));
    std::string said;
    for (const auto& [k, v] : gMegler) said += " " + k + "=" + v;
    for (const auto& [k, v] : gOwn) said += " " + k + "=" + v + " (the app's)";
    shellLog::write("settings:" + (said.empty() ? std::string(" none set") : said));
}

std::string get(const std::string& key, const std::string& fallback) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto own = gOwn.find(key);
    if (own != gOwn.end()) return own->second;
    auto megler = gMegler.find(key);
    return megler == gMegler.end() ? fallback : megler->second;
}

int getInt(const std::string& key, int fallback) {
    const std::string value = get(key, std::string());
    if (value.empty()) return fallback;
    char* end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    return end && *end == 0 ? int(parsed) : fallback;
}

void set(const std::string& key, const std::string& value) {
    std::string own;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        own = gOwnPackage;
    }
    if (own.empty())
        setMegler(key, value);
    else
        setApp(own, key, value);
}

std::optional<std::string> megler(const std::string& key) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gMegler.find(key);
    if (it == gMegler.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> meglerNow(const std::string& key) {
    std::lock_guard<std::mutex> lock(gMutex);
    // (Read whole and not kept: what this process goes by, get(), stays as it was when it started.)
    const Values values = readFile(gMeglerPath);
    auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
}

void setMegler(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(gMutex);
    // Read again first, as an app's are: the file is what counts.
    gMegler = readFile(gMeglerPath);
    gMegler[key] = value;
    writeFile(gMeglerPath, gMegler);
}

std::optional<std::string> app(const std::string& package, const std::string& key) {
    std::lock_guard<std::mutex> lock(gMutex);
    // Another app's, or this package's own as its file has it now (Megler may have changed it since the start).
    const Values values = readFile(appPath(package));
    auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
}

void setApp(const std::string& package, const std::string& key, const std::optional<std::string>& value) {
    std::lock_guard<std::mutex> lock(gMutex);
    const std::wstring path = appPath(package);
    if (path.empty()) return;
    // Read again: Megler and the app's own package may both change it.
    Values values = readFile(path);
    if (value)
        values[key] = *value;
    else
        values.erase(key);
    writeFile(path, values);
    if (package == gOwnPackage) gOwn = values;
}

int eraseApp(const std::string& package, const std::string& prefix) {
    std::lock_guard<std::mutex> lock(gMutex);
    const std::wstring path = appPath(package);
    if (path.empty()) return 0;
    Values values = readFile(path);
    int erased = 0;
    for (auto it = values.begin(); it != values.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = values.erase(it);
            erased++;
        } else {
            ++it;
        }
    }
    if (!erased) return 0;
    writeFile(path, values);
    if (package == gOwnPackage) gOwn = values;
    return erased;
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
