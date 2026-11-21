#include "pch.h"

#include "library/DefaultApps.h"

#include "library/PhoneAppsList.h"
#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

namespace megler::app::defaultApps {

using namespace winrt;
using namespace winrt::Windows::Data::Json;

namespace {

std::wstring file() {
    const std::wstring dir = phoneAppsList::folder();
    return dir.empty() ? std::wstring() : dir + L"\\defaults.ini";
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

bool write(const std::map<std::string, std::string>& values) {
    const std::wstring path = file();
    if (path.empty()) return false;
    std::string text;
    for (const auto& [key, value] : values) text += key + "=" + value + "\n";
    // Beside it first, then in its place: an app reading it meanwhile finds the one before or this one.
    const std::wstring temp = path + L".shell.new";
    return files::write(temp, text) && files::move(temp, path);
}

/// Windows.Data.Json throws on a value of another type than asked for: a description of another form is read as far as it goes.
std::vector<std::string> strings(JsonObject const& o, const wchar_t* name) {
    std::vector<std::string> out;
    if (!o.HasKey(name)) return out;
    IJsonValue v = o.GetNamedValue(name);
    if (v.ValueType() != JsonValueType::Array) return out;
    for (IJsonValue const& item : v.GetArray())
        if (item.ValueType() == JsonValueType::String) out.push_back(narrow(item.GetString()));
    return out;
}

std::string stringOf(JsonObject const& o, const wchar_t* name) {
    if (!o.HasKey(name)) return std::string();
    IJsonValue v = o.GetNamedValue(name);
    return v.ValueType() == JsonValueType::String ? narrow(v.GetString()) : std::string();
}

bool has(const std::vector<std::string>& list, const char* x) { return std::find(list.begin(), list.end(), x) != list.end(); }

/// The apps of apps.txt: package and label.
std::vector<std::pair<std::string, std::string>> apps() {
    std::vector<std::pair<std::string, std::string>> out;
    const std::wstring dir = phoneAppsList::folder();
    std::string text;
    if (dir.empty() || !files::read(dir + L"\\apps.txt", text)) return out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1),
                     t3 = t2 == std::string::npos ? t2 : line.find('\t', t2 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        out.emplace_back(line.substr(0, t1), trim(line.substr(t2 + 1, t3 == std::string::npos ? std::string::npos : t3 - t2 - 1)));
    }
    return out;
}

/// Each filter of the app's activities as the shell described them, with the activity's name and label: f(name, label, filter).
template <typename F> void eachFilter(const std::string& package, F f) {
    const std::wstring dir = phoneAppsList::folder();
    std::string json;
    if (dir.empty() || !files::read(dir + L"\\" + widen(package) + L"\\activities.json", json)) return;
    try {
        JsonObject root;
        if (!JsonObject::TryParse(widen(json), root) || !root.HasKey(L"activities")) return;
        IJsonValue acts = root.GetNamedValue(L"activities");
        if (acts.ValueType() != JsonValueType::Array) return;
        for (IJsonValue const& a : acts.GetArray()) {
            if (a.ValueType() != JsonValueType::Object) continue;
            JsonObject act = a.GetObject();
            if (act.HasKey(L"exported") && act.GetNamedValue(L"exported").ValueType() == JsonValueType::Boolean && !act.GetNamedBoolean(L"exported")) continue;
            if (act.HasKey(L"enabled") && act.GetNamedValue(L"enabled").ValueType() == JsonValueType::Boolean && !act.GetNamedBoolean(L"enabled")) continue;
            if (!act.HasKey(L"filters") || act.GetNamedValue(L"filters").ValueType() != JsonValueType::Array) continue;
            const std::string name = stringOf(act, L"name"), label = stringOf(act, L"label");
            for (IJsonValue const& x : act.GetNamedArray(L"filters"))
                if (x.ValueType() == JsonValueType::Object) f(name, label, x.GetObject());
        }
    } catch (const hresult_error&) {
        shellLog::write("default apps: the activities of " + package + " could not be read");
    }
}

/// IntentFilter.handlesWebUris: VIEW, BROWSABLE, and http or https among its schemes.
bool webLinks(JsonObject const& filter) {
    const auto schemes = strings(filter, L"schemes");
    return has(strings(filter, L"actions"), "android.intent.action.VIEW") && has(strings(filter, L"categories"), "android.intent.category.BROWSABLE") &&
           (has(schemes, "http") || has(schemes, "https"));
}

/// Whether the filter makes its activity a browser, as megler.Resolver.browsers() has it for the apps' own lists (and
/// Android's Settings for its own): it takes the probe that asks -- VIEW of https://example.com/, BROWSABLE, CATEGORY_DEFAULT
/// as startActivity asks, no type -- and takes it as a browser does (IntentFilter.handleAllWebDataURI: no host at all, or
/// CATEGORY_APP_BROWSER where a host "*" takes every one). Not a filter of web links of some MIME type (a player's streams, a
/// downloader's files), one without DEFAULT, or APP_BROWSER on a filter that takes no web links: the looser test before this
/// made the Settings' list of browsers long with apps that are none.
bool browserFilter(JsonObject const& filter) {
    const auto categories = strings(filter, L"categories");
    if (!has(strings(filter, L"actions"), "android.intent.action.VIEW") || !has(categories, "android.intent.category.BROWSABLE") ||
        !has(categories, "android.intent.category.DEFAULT") || !has(strings(filter, L"schemes"), "https"))
        return false;
    // (A type, even "*/*", is not matched by an intent without one; nor is a scheme-specific part without hosts.)
    if (!strings(filter, L"types").empty()) return false;
    const auto hosts = strings(filter, L"hosts");
    if (hosts.empty())
        return !(filter.HasKey(L"ssps") && filter.GetNamedValue(L"ssps").ValueType() == JsonValueType::Array &&
                 filter.GetNamedArray(L"ssps").Size() > 0);
    return has(categories, "android.intent.category.APP_BROWSER") && has(hosts, "*");
}

bool autoVerify(JsonObject const& filter) {
    return filter.HasKey(L"autoVerify") && filter.GetNamedValue(L"autoVerify").ValueType() == JsonValueType::Boolean && filter.GetNamedBoolean(L"autoVerify");
}

} // namespace

std::map<std::string, std::string> read() {
    std::map<std::string, std::string> out;
    const std::wstring path = file();
    std::string text;
    if (path.empty() || !files::read(path, text)) return out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(at, end - at));
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0 || line[0] == '#' || line[0] == ';') continue;
        out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return out;
}

bool set(const std::string& key, const std::optional<std::string>& value) {
    auto values = read();
    if (value)
        values[key] = *value;
    else
        values.erase(key);
    const bool ok = write(values);
    shellLog::write("default apps: " + key + (value ? " = " + *value : std::string(" taken out")) + (ok ? "" : " (not written)"));
    return ok;
}

int clearAlways(const std::string& package) {
    auto values = read();
    int n = 0;
    for (auto it = values.begin(); it != values.end();) {
        if (it->first.rfind("always.", 0) == 0 && it->second.rfind(package + "/", 0) == 0) {
            it = values.erase(it);
            n++;
        } else {
            ++it;
        }
    }
    if (n > 0) write(values);
    shellLog::write(format("default apps: %d \"Always\" answer(s) for %s cleared", n, package.c_str()));
    return n;
}

std::vector<Browser> browsers() {
    std::vector<Browser> out;
    for (const auto& [package, appLabel] : apps()) {
        eachFilter(package, [&, package = package, appLabel = appLabel](const std::string& name, const std::string& label, JsonObject const& filter) {
            if (!browserFilter(filter)) return;
            const std::string id = package + "/" + name;
            for (const Browser& b : out)
                if (b.id == id) return;
            out.push_back({id, label.empty() ? appLabel : label});
        });
    }
    return out;
}

std::vector<std::pair<std::string, bool>> linkDomains(const std::string& package) {
    std::vector<std::pair<std::string, bool>> out;
    eachFilter(package, [&](const std::string&, const std::string&, JsonObject const& filter) {
        if (!webLinks(filter)) return;
        const bool verify = autoVerify(filter);
        for (const std::string& host : strings(filter, L"hosts")) {
            auto it = std::find_if(out.begin(), out.end(), [&](const auto& d) { return d.first == host; });
            if (it == out.end())
                out.emplace_back(host, verify);
            else
                it->second = it->second || verify;
        }
    });
    return out;
}

std::string kindInWords(const std::string& kind) {
    std::vector<std::string> f;
    size_t at = 0;
    while (true) {
        const size_t bar = kind.find('|', at);
        f.push_back(kind.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    while (f.size() < 4) f.emplace_back();
    std::string action = f[0];
    const std::string prefix = "android.intent.action.";
    if (action.rfind(prefix, 0) == 0) action = action.substr(prefix.size());
    std::string what = !f[3].empty()   ? "Links to " + f[3]
                       : !f[1].empty() ? f[1] + ": addresses" + (f[2].empty() ? std::string() : " (" + f[2] + ")")
                       : !f[2].empty() ? f[2] + " files"
                                       : std::string("Others");
    return action == "VIEW" ? what : what + ", " + action;
}

std::string labelOf(const std::string& id) {
    if (id.empty() || id == "windows") return "Windows";
    const std::string package = id.substr(0, id.find('/'));
    for (const auto& [p, label] : apps())
        if (p == package) return label.empty() ? p : label;
    return package;
}

} // namespace megler::app::defaultApps
