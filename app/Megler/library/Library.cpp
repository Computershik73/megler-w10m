#include "pch.h"

#include "library/Library.h"

#include "shell/AppPackage.h"
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
        // An app's own package has its one APK, with its report, in the install folder (shell/AppPackage.h): read as it is.
        const bool single = appPackage::isSingleApp();
        directory_ = single ? appPackage::single().apkFolder : platform::localStateDirectory() + L"\\apks";
        if (!single) {
            files::makeDirectory(directory_);
            // A copy that never finished (the app was ended under the picker's stream), and a bundle's unpacking likewise.
            for (const auto& part : files::list(directory_, L"*.part")) files::remove(directory_ + L"\\" + part);
            for (const auto& part : files::listFolders(directory_, L"*.splits.part")) files::removeTree(directory_ + L"\\" + part);
            // Splits whose base is gone.
            for (const auto& name : files::listFolders(directory_, L"*.apk.splits")) {
                const std::wstring folder = directory_ + L"\\" + name;
                if (!files::exists(folder.substr(0, folder.size() - 7))) files::removeTree(folder);
            }
            // A cache whose APK is gone (removed while it was still being read); an installed app's is its entry's.
            for (const wchar_t* pattern : {L"*.apk.json", L"*.apk.icon"}) {
                for (const auto& name : files::list(directory_, pattern)) {
                    const std::wstring cached = directory_ + L"\\" + name, apk = cached.substr(0, cached.size() - 5);
                    if (!files::exists(apk) && !files::exists(apk + L".installed")) files::remove(cached);
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
        // The apps whose APK is an installed package's (Megler has no copy of it).
        for (const auto& name : files::list(directory_, L"*.apk.installed")) {
            const std::string id = narrow(name.substr(0, name.size() - 10));
            std::string fullName;
            if (std::any_of(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; }) ||
                !files::read(directory_ + L"\\" + name, fullName))
                continue;
            ApkEntry entry = installedEntry(id, fullName);
            if ((stale || !entry.analysed) && files::exists(entry.path)) toRead.push_back(entry.id);
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
        entry.replaces = true;
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

void Library::eraseLocked(std::vector<ApkEntry>::iterator it) {
    // The cache is here for either kind; the APK and its splits only for a copy.
    const std::wstring own = directory_ + L"\\" + widen(it->id);
    if (it->installedPackage.empty()) {
        files::remove(it->path);
        if (!it->splitsDir.empty()) files::removeTree(it->splitsDir);
    }
    files::remove(own + L".installed");
    files::remove(own + L".json");
    files::remove(own + L".icon");
    // Not read after it has gone: its report would describe a file that is not there.
    queue_.erase(std::remove(queue_.begin(), queue_.end(), it->id), queue_.end());
    entries_.erase(it);
    ++generation_;
}

void Library::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; });
    if (it == entries_.end()) return;
    eraseLocked(it);
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
    // The APK where it is (a copy here, or an installed package's); its cache here.
    std::wstring path, apk;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = directory_ + L"\\" + widen(id);
        apk = path;
        for (const auto& entry : entries_)
            if (entry.id == id) apk = entry.path;
    }
    const std::string utf8Path = narrow(apk);
    // An app in several APKs is reported on, and its icon looked for, as one: the base and its splits' folder.
    const std::string utf8Splits = files::exists(splitsDirFor(apk)) ? narrow(splitsDirFor(apk)) : std::string();
    const char* splits = utf8Splits.empty() ? nullptr : utf8Splits.c_str();
    std::string json;
    if (char* report = megler_apk_report_splits(utf8Path.c_str(), splits)) {
        json = report;
        megler_free(report);
    }
    ApkReport parsed = ApkReport::parse(json);
    // An APK that cannot be read now -- an installed package's, whose folder Megler may not read, when a new build reads
    // every APK again -- keeps what was found before: its report and its icon. (Its icon was removed here, and its report
    // made empty: the library showed no icon for the apps of the phone's own after each new build.)
    const bool read = !json.empty() && parsed.valid;
    if (read) files::write(path + L".json", json);

    std::wstring iconPath;
    uint8_t* icon = nullptr;
    size_t iconSize = 0;
    if (megler_apk_icon_splits(utf8Path.c_str(), splits, &icon, &iconSize) && icon && iconSize > 0) {
        // The same picture as before (every APK is read again once a new build is installed): the file is left as it
        // is, and a list showing it sees nothing change. A new one is written beside it and moved into its place, so
        // that it is never read half written.
        const std::string_view bytes(reinterpret_cast<const char*>(icon), iconSize);
        const std::wstring target = path + L".icon";
        std::string before;
        if (files::read(target, before) && before == bytes) {
            iconPath = target;
        } else if (files::write(target + L".new", bytes) && files::move(target + L".new", target)) {
            iconPath = target;
        } else {
            files::remove(target + L".new");
        }
    } else {
        // None found now. The one found before is kept -- the core drew an adaptive icon then and may not now -- and an app
        // of the phone's own has the one its package was made with beside its APK (apppp.apk.icon), which its tiles
        // were drawn from: taken from there. (It was removed here, and the library showed none for such apps.)
        if (!files::exists(path + L".icon") && apk != path && files::exists(apk + L".icon")) files::copy(apk + L".icon", path + L".icon");
        if (files::exists(path + L".icon")) iconPath = path + L".icon";
        if (read) shellLog::write("library: " + id + ": the core found no icon it could draw" + (iconPath.empty() ? "" : "; the one kept is shown"));
    }
    if (icon) megler_free(icon);
    if (!read) shellLog::write("library: " + id + " could not be read now (" + narrow(apk) + "): what was found before is kept");

    shellLog::write(format("library: %s is %s (%s%s) -- %s", id.c_str(), parsed.package.empty() ? "?" : parsed.package.c_str(),
                           parsed.engine.empty() ? "?" : parsed.engine.c_str(),
                           parsed.splits.empty() ? "" : format(", with %u splits", unsigned(parsed.splits.size())).c_str(), parsed.status().c_str()));
    std::lock_guard<std::mutex> lock(mutex_);
    std::string package;
    bool replaces = false;
    for (auto& entry : entries_) {
        if (entry.id != id) continue;
        replaces = entry.replaces;
        entry.replaces = false;
        entry.analysing = false;
        if (!read && entry.analysed) {
            if (entry.iconPath.empty()) entry.iconPath = iconPath;
            break;
        }
        package = parsed.package;
        entry.report = std::move(parsed);
        entry.analysed = true;
        entry.iconPath = iconPath;
        break;
    }
    // Added just now, for a package already in the library: it replaces the entries that were there, whatever their file
    // names, versions or signatures (none is compared); the app's own files stay. Not when the report names no package.
    if (replaces && !package.empty()) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->id != id && it->report.package == package) {
                shellLog::write(format("library: %s replaces %s, the same package %s (its version and signature not compared; the app's files kept)",
                                       id.c_str(), it->id.c_str(), package.c_str()));
                eraseLocked(it);
                it = entries_.begin();
            } else {
                ++it;
            }
        }
    }
    ++generation_;
}

ApkEntry Library::installedEntry(const std::string& id, const std::string& fullName) const {
    const std::wstring own = directory_ + L"\\" + widen(id);
    ApkEntry entry;
    entry.id = id;
    entry.installedPackage = fullName;
    entry.path = appPackage::installedApk(fullName);
    entry.size = files::size(entry.path);
    if (files::exists(splitsDirFor(entry.path))) {
        entry.splitsDir = splitsDirFor(entry.path);
        entry.size += files::folderSize(entry.splitsDir);
    }
    std::string json;
    if (files::read(own + L".json", json) && !json.empty()) {
        entry.report = ApkReport::parse(json);
        entry.analysed = entry.report.valid;
    }
    // The icon: the library's, else the one the app's package was made with (beside its APK), copied here, where the list
    // shows icons from.
    if (!files::exists(own + L".icon") && files::exists(entry.path + L".icon")) files::copy(entry.path + L".icon", own + L".icon");
    if (files::exists(own + L".icon")) entry.iconPath = own + L".icon";
    return entry;
}

void Library::adopt(const std::string& id, const std::string& fullName) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::wstring own = directory_ + L"\\" + widen(id);
    if (!files::write(own + L".installed", fullName)) {
        shellLog::write("library: " + id + " keeps its copy: " + narrow(own) + ".installed could not be written");
        return;
    }
    const bool hadCopy = files::exists(own);
    files::remove(own);
    files::removeTree(splitsDirFor(own));
    ApkEntry entry = installedEntry(id, fullName);
    const std::string package = entry.report.package;
    auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; });
    if (existing != entries_.end())
        *existing = std::move(entry);
    else
        entries_.push_back(std::move(entry));
    // The other entries of its package, as an added APK replaces them.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->id != id && !package.empty() && it->report.package == package) {
            shellLog::write("library: " + id + " replaces " + it->id + ", the same package " + package);
            eraseLocked(it);
            it = entries_.begin();
        } else {
            ++it;
        }
    }
    ++generation_;
    shellLog::write("library: " + id + " is " + fullName + "'s APK now" + (hadCopy ? ", Megler's copy removed" : ""));
}

std::string Library::restore(const std::string& id) {
    ApkEntry entry;
    std::wstring own;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; });
        if (it == entries_.end()) return "it is not in the library";
        if (it->installedPackage.empty()) return std::string();
        entry = *it;
        own = directory_ + L"\\" + widen(id);
    }
    // Copied beside and moved into place, the splits likewise, so that nothing half copied is taken for the app.
    if (!files::copy(entry.path, own + L".part") || !files::move(own + L".part", own)) {
        files::remove(own + L".part");
        return format("its APK could not be copied back from the installed app (error %lu)", GetLastError());
    }
    if (!entry.splitsDir.empty()) {
        const std::wstring splits = splitsDirFor(own);
        files::removeTree(splits + L".part");
        files::makeDirectory(splits + L".part");
        for (const auto& name : files::list(entry.splitsDir, L"*")) {
            if (!files::copy(entry.splitsDir + L"\\" + name, splits + L".part\\" + name)) {
                files::removeTree(splits + L".part");
                files::remove(own);
                return format("its split APKs could not be copied back from the installed app (error %lu)", GetLastError());
            }
        }
        files::removeTree(splits);
        files::move(splits + L".part", splits);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    files::remove(own + L".installed");
    for (auto& e : entries_) {
        if (e.id != id) continue;
        e.installedPackage.clear();
        e.path = own;
        e.splitsDir = files::exists(splitsDirFor(own)) ? splitsDirFor(own) : std::wstring();
    }
    ++generation_;
    shellLog::write("library: " + id + "'s APK copied back from the installed app");
    return std::string();
}

} // namespace megler::app
