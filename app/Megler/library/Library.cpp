#include "pch.h"

#include "library/Library.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

#include <algorithm>

namespace megler::app {

using namespace winrt;
using namespace winrt::Windows::Data::Json;

namespace {

// Windows.Data.Json throws on a value of another type than asked for; a
// report from a newer or older core must not take the list down, so every
// field is read through these.
std::string stringOf(JsonObject const& o, const wchar_t* name) {
    if (!o.HasKey(name)) return std::string();
    IJsonValue v = o.GetNamedValue(name);
    return v.ValueType() == JsonValueType::String ? narrow(v.GetString()) : std::string();
}

double numberOf(JsonObject const& o, const wchar_t* name) {
    if (!o.HasKey(name)) return 0;
    IJsonValue v = o.GetNamedValue(name);
    return v.ValueType() == JsonValueType::Number ? v.GetNumber() : 0;
}

bool boolOf(JsonObject const& o, const wchar_t* name) {
    if (!o.HasKey(name)) return false;
    IJsonValue v = o.GetNamedValue(name);
    return v.ValueType() == JsonValueType::Boolean && v.GetBoolean();
}

std::vector<std::string> stringsOf(JsonObject const& o, const wchar_t* name) {
    std::vector<std::string> out;
    if (!o.HasKey(name)) return out;
    IJsonValue v = o.GetNamedValue(name);
    if (v.ValueType() != JsonValueType::Array) return out;
    for (IJsonValue const& item : v.GetArray())
        if (item.ValueType() == JsonValueType::String) out.push_back(narrow(item.GetString()));
    return out;
}

std::string versionStamp() { return platform::packageVersion() + " core " + megler_version(); }

} // namespace

ApkReport ApkReport::parse(const std::string& json) {
    ApkReport r;
    try {
        JsonObject o{nullptr};
        if (!JsonObject::TryParse(hs(json), o) || !o) return r;
        r.valid = true;
        r.ok = boolOf(o, L"ok");
        r.error = stringOf(o, L"error");
        r.package = stringOf(o, L"package");
        r.label = stringOf(o, L"label");
        r.versionName = stringOf(o, L"versionName");
        r.versionCode = int64_t(numberOf(o, L"versionCode"));
        r.minSdk = int(numberOf(o, L"minSdk"));
        r.targetSdk = int(numberOf(o, L"targetSdk"));
        r.abis = stringsOf(o, L"abis");
        r.abi = stringOf(o, L"abi");
        r.runnable = boolOf(o, L"runnable");
        r.reason = stringOf(o, L"reason");
        r.engine = stringOf(o, L"engine");
        r.orientation = stringOf(o, L"orientation");
        r.supported = boolOf(o, L"supported");
        r.activity = stringOf(o, L"activity");
        r.nativeLibrary = stringOf(o, L"nativeLibrary");
        r.notes = stringsOf(o, L"notes");
        r.splits = stringsOf(o, L"splits");
        r.splitsLeftOut = stringsOf(o, L"splitsLeftOut");
        if (o.HasKey(L"libraries") && o.GetNamedValue(L"libraries").ValueType() == JsonValueType::Array) {
            for (IJsonValue const& item : o.GetNamedArray(L"libraries")) {
                if (item.ValueType() != JsonValueType::Object) continue;
                JsonObject l = item.GetObject();
                ApkLibrary library;
                library.name = stringOf(l, L"name");
                library.size = uint64_t(numberOf(l, L"size"));
                library.armFunctions = int(numberOf(l, L"armFunctions"));
                library.thumbFunctions = int(numberOf(l, L"thumbFunctions"));
                // (megler.h calls it missingImports; the core writes importsNotInSystem.)
                library.missingImports = int(numberOf(l, l.HasKey(L"missingImports") ? L"missingImports" : L"importsNotInSystem"));
                library.needs = stringsOf(l, L"needs");
                r.libraries.push_back(std::move(library));
            }
        }
    } catch (const hresult_error&) {
        r = ApkReport();
    }
    return r;
}

std::string ApkReport::status() const {
    // The core's reasons are log lines, begun in lower case; here one stands
    // alone as a status.
    auto sentence = [](std::string text) {
        if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = char(text[0] - 'a' + 'A');
        return text;
    };
    if (!valid) return "Couldn't be read";
    if (!ok) return error.empty() ? "Couldn't be read" : sentence(error);
    if (!runnable) return reason.empty() ? "Can't run here" : sentence(reason);
    if (!supported) return reason.empty() ? engineName() + " apps aren't supported yet" : sentence(reason);
    return "Ready";
}

std::string ApkReport::engineName() const {
    if (engine == "native-activity") return "NativeActivity";
    if (engine == "sdl") return "SDL";
    if (engine == "unity") return "Unity";
    if (engine == "cocos2d-x") return "Cocos2d-x";
    if (engine == "unreal") return "Unreal Engine";
    if (engine == "godot") return "Godot";
    if (engine == "java") return "Java";
    return engine.empty() || engine == "unknown" ? "Unknown" : engine;
}

std::string ApkEntry::title() const {
    if (!report.label.empty()) return report.label;
    if (!report.package.empty()) return report.package;
    return id;
}

Library& Library::instance() {
    static Library library;
    return library;
}

void Library::load() {
    std::vector<std::string> toRead;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        directory_ = platform::localStateDirectory() + L"\\apks";
        files::makeDirectory(directory_);
        // A copy that never finished (the app was ended under the picker's stream), and a bundle's unpacking likewise.
        for (const auto& part : files::list(directory_, L"*.part")) files::remove(directory_ + L"\\" + part);
        for (const auto& part : files::listFolders(directory_, L"*.splits.part")) files::removeTree(directory_ + L"\\" + part);
        // Splits whose base is gone.
        for (const auto& name : files::listFolders(directory_, L"*.apk.splits")) {
            const std::wstring folder = directory_ + L"\\" + name;
            if (!files::exists(folder.substr(0, folder.size() - 7))) files::removeTree(folder);
        }
        // A cache whose APK is gone (removed while it was still being read).
        for (const wchar_t* pattern : {L"*.apk.json", L"*.apk.icon"}) {
            for (const auto& name : files::list(directory_, pattern)) {
                const std::wstring cached = directory_ + L"\\" + name;
                if (!files::exists(cached.substr(0, cached.size() - 5))) files::remove(cached);
            }
        }
        // The demo, once added, is a copy like any other, and an update of
        // Megler may carry a newer one: the copy follows the package's.
        {
            const std::wstring source = platform::installDirectory() + L"\\demo\\megler-demo.apk";
            const std::wstring copy = directory_ + L"\\megler-demo.apk";
            std::string ours, theirs;
            if (files::read(copy, theirs) && files::read(source, ours) && ours != theirs && files::copy(source, copy)) {
                files::remove(copy + L".json");
                files::remove(copy + L".icon");
                shellLog::write("library: the demo was replaced with this build's");
            }
        }
        // The Java test apps likewise, where the package has them (tools/build-appx.sh --java).
        for (const wchar_t* name : {L"hello.apk", L"demos.apk", L"gl.apk"}) {
            const std::wstring source = platform::installDirectory() + L"\\demo\\" + name;
            const std::wstring copy = directory_ + L"\\" + name;
            std::string ours, theirs;
            if (files::read(copy, theirs) && files::read(source, ours) && ours != theirs && files::copy(source, copy)) {
                files::remove(copy + L".json");
                files::remove(copy + L".icon");
                shellLog::write("library: " + narrow(name) + " was replaced with this build's");
            }
        }

        std::string stamp;
        files::read(directory_ + L"\\reports.version", stamp);
        const bool stale = stamp != versionStamp();
        entries_.clear();
        for (const auto& name : files::list(directory_, L"*.apk")) {
            ApkEntry entry;
            entry.id = narrow(name);
            entry.path = directory_ + L"\\" + name;
            entry.size = files::size(entry.path);
            if (files::exists(splitsDirFor(entry.path))) {
                entry.splitsDir = splitsDirFor(entry.path);
                entry.size += files::folderSize(entry.splitsDir);
            }
            std::string json;
            if (files::read(entry.path + L".json", json) && !json.empty()) {
                entry.report = ApkReport::parse(json);
                entry.analysed = entry.report.valid;
            }
            if (files::exists(entry.path + L".icon")) entry.iconPath = entry.path + L".icon";
            if (stale || !entry.analysed) toRead.push_back(entry.id);
            entries_.push_back(std::move(entry));
        }
        refreshingAll_ = stale && !toRead.empty();
        if (stale && toRead.empty()) files::write(directory_ + L"\\reports.version", versionStamp());
        ++generation_;
        shellLog::write(format("library: %u APKs in LocalState\\apks, %u to read%s", unsigned(entries_.size()),
                               unsigned(toRead.size()), stale ? " (the reports are another build's)" : ""));
    }
    for (const auto& id : toRead) queue(id);
}

std::vector<ApkEntry> Library::entries() const {
    std::vector<ApkEntry> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = entries_;
    }
    std::sort(copy.begin(), copy.end(), [](const ApkEntry& a, const ApkEntry& b) {
        return _stricmp(a.title().c_str(), b.title().c_str()) < 0;
    });
    return copy;
}

std::optional<ApkEntry> Library::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_)
        if (entry.id == id) return entry;
    return std::nullopt;
}

unsigned Library::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

bool Library::isBundle(const std::string& pickedName) {
    auto endsWith = [&](const char* tail) {
        const size_t n = strlen(tail);
        return pickedName.size() > n && _stricmp(pickedName.c_str() + pickedName.size() - n, tail) == 0;
    };
    return endsWith(".apks") || endsWith(".xapk");
}

std::string Library::idFor(const std::string& pickedName) {
    std::string id;
    std::string name = pickedName;
    // A bundle is kept as its base, an .apk.
    if (isBundle(name)) name.resize(name.size() - 5);
    for (unsigned char ch : name) {
        // Bytes above ASCII are UTF-8 of a name in another script: kept.
        const bool safe = isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch == '+' || ch >= 0x80;
        id += safe ? char(ch) : '_';
    }
    while (!id.empty() && id.front() == '.') id.erase(id.begin());
    if (id.size() < 4 || _stricmp(id.c_str() + id.size() - 4, ".apk") != 0) id += ".apk";
    if (id == ".apk") id = "app.apk";
    return id;
}

void Library::added(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::wstring path = directory_ + L"\\" + widen(id);
        // The cache of a copy this one replaces describes another file.
        files::remove(path + L".json");
        files::remove(path + L".icon");
        ApkEntry entry;
        entry.id = id;
        entry.path = path;
        entry.size = files::size(path);
        if (files::exists(splitsDirFor(path))) {
            entry.splitsDir = splitsDirFor(path);
            entry.size += files::folderSize(entry.splitsDir);
        }
        auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; });
        if (existing != entries_.end())
            *existing = std::move(entry);
        else
            entries_.push_back(std::move(entry));
        ++generation_;
    }
    shellLog::write("library: added " + id);
    queue(id);
}

namespace {
const char* const demoId = "megler-demo.apk";
std::wstring demoSource() { return platform::installDirectory() + L"\\demo\\megler-demo.apk"; }
} // namespace

bool Library::hasDemo() { return files::exists(demoSource()); }

bool Library::hasPackaged(const std::string& name) { return files::exists(platform::installDirectory() + L"\\demo\\" + widen(name)); }

bool Library::addPackaged(const std::string& name) {
    std::wstring target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = directory_ + L"\\" + widen(name);
    }
    if (!files::copy(platform::installDirectory() + L"\\demo\\" + widen(name), target)) {
        shellLog::write(format("library: %s of the package could not be copied (error %lu)", name.c_str(), GetLastError()));
        return false;
    }
    added(name);
    return true;
}

bool Library::addDemo() {
    std::wstring target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        target = directory_ + L"\\" + widen(demoId);
    }
    if (!files::copy(demoSource(), target)) {
        shellLog::write(format("library: the demo could not be copied (error %lu)", GetLastError()));
        return false;
    }
    added(demoId);
    return true;
}

void Library::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; });
    if (it == entries_.end()) return;
    files::remove(it->path);
    files::remove(it->path + L".json");
    files::remove(it->path + L".icon");
    if (!it->splitsDir.empty()) files::removeTree(it->splitsDir);
    entries_.erase(it);
    ++generation_;
    shellLog::write("library: removed " + id);
}

void Library::queue(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(id);
    for (auto& entry : entries_)
        if (entry.id == id) entry.analysing = true;
    ++generation_;
    if (working_) return;
    working_ = true;
    std::thread([this] { work(); }).detach();
}

void Library::work() {
    for (;;) {
        std::string id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                // Every report is this build's now.
                if (refreshingAll_) files::write(directory_ + L"\\reports.version", versionStamp());
                refreshingAll_ = false;
                working_ = false;
                return;
            }
            id = queue_.front();
            queue_.pop_front();
        }
        try {
            analyse(id);
        } catch (const hresult_error& e) {
            shellLog::write("library: reading " + id + " failed: " + narrow(e.message()));
        } catch (const std::exception& e) {
            shellLog::write("library: reading " + id + " failed: " + e.what());
        }
    }
}

void Library::analyse(const std::string& id) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = directory_ + L"\\" + widen(id);
    }
    const std::string utf8Path = narrow(path);
    // An app in several APKs is reported on, and its icon looked for, as one: the base and its splits' folder.
    const std::string utf8Splits = files::exists(splitsDirFor(path)) ? narrow(splitsDirFor(path)) : std::string();
    const char* splits = utf8Splits.empty() ? nullptr : utf8Splits.c_str();
    std::string json;
    if (char* report = megler_apk_report_splits(utf8Path.c_str(), splits)) {
        json = report;
        megler_free(report);
    }
    ApkReport parsed = ApkReport::parse(json);
    if (!json.empty()) files::write(path + L".json", json);

    std::wstring iconPath;
    uint8_t* icon = nullptr;
    size_t iconSize = 0;
    if (megler_apk_icon_splits(utf8Path.c_str(), splits, &icon, &iconSize) && icon && iconSize > 0) {
        if (files::write(path + L".icon", std::string_view(reinterpret_cast<const char*>(icon), iconSize))) iconPath = path + L".icon";
    } else {
        files::remove(path + L".icon");
    }
    if (icon) megler_free(icon);

    shellLog::write(format("library: %s is %s (%s%s) -- %s", id.c_str(), parsed.package.empty() ? "?" : parsed.package.c_str(),
                           parsed.engine.empty() ? "?" : parsed.engine.c_str(),
                           parsed.splits.empty() ? "" : format(", with %u splits", unsigned(parsed.splits.size())).c_str(), parsed.status().c_str()));
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
        if (entry.id != id) continue;
        entry.report = std::move(parsed);
        entry.analysed = true;
        entry.analysing = false;
        entry.iconPath = iconPath;
        break;
    }
    ++generation_;
}

} // namespace megler::app
