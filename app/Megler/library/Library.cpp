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
        r.microg = boolOf(o, L"microg");
        r.orientation = stringOf(o, L"orientation");
        r.supported = boolOf(o, L"supported");
        r.activity = stringOf(o, L"activity");
        r.nativeLibrary = stringOf(o, L"nativeLibrary");
        r.notes = stringsOf(o, L"notes");
        r.splits = stringsOf(o, L"splits");
        r.splitsLeftOut = stringsOf(o, L"splitsLeftOut");
        // (A report an older core made says nothing of them: not known, which is not none.)
        r.permissionsKnown = o.HasKey(L"permissions");
        r.permissions = stringsOf(o, L"permissions");
        if (o.HasKey(L"permissionMaxSdk") && o.GetNamedValue(L"permissionMaxSdk").ValueType() == JsonValueType::Array)
            for (IJsonValue const& item : o.GetNamedArray(L"permissionMaxSdk"))
                r.permissionMaxSdk.push_back(item.ValueType() == JsonValueType::Number ? int(item.GetNumber()) : 0);
        r.permissionMaxSdk.resize(r.permissions.size(), 0);
        r.smsRetriever = boolOf(o, L"smsRetriever");
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

bool ApkReport::asks(const std::string& permission) const {
    if (!permissionsKnown) return true;
    for (size_t i = 0; i < permissions.size(); ++i)
        if (permissions[i] == permission) return i >= permissionMaxSdk.size() || permissionMaxSdk[i] == 0 || permissionMaxSdk[i] >= 34;
    return false;
}

int ApkReport::capabilities() const {
    if (!permissionsKnown) return -1;
    std::vector<const char*> names;
    for (const auto& p : permissions) names.push_back(p.c_str());
    return int(megler_appx_capabilities(names.empty() ? nullptr : names.data(), int(names.size()), smsRetriever));
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

namespace {

/// What an id.apk.installed says (Library::adopt): the package's full name, then, for a package made since 645, the folder of
/// the shared folder its app's APK is in; and so where that APK is.
struct Installed {
    std::string fullName;
    std::wstring apk;
};

Installed installedOf(const std::string& marker) {
    Installed installed;
    installed.fullName = marker.substr(0, marker.find_first_of("\r\n"));
    std::string folder;
    if (const size_t at = marker.find("\nshared="); at != std::string::npos) folder = marker.substr(at + 8, marker.find_first_of("\r\n", at + 8) - (at + 8));
    installed.apk = folder.empty() ? appPackage::installedApk(installed.fullName) : appPackage::sharedFolder() + L"\\" + widen(folder) + L"\\app.apk";
    return installed;
}

/// Copies an installed app's APK `apk` (in the shared folder, or in its package), and its splits, to `own` and own +
/// ".splits": each beside its place and moved into it, so that nothing half copied is taken for the app. The splits are
/// named by their folder's list, else by the app's report beside the APK: a package installed as the Store installs an app
/// shows another package none of its folders (appPackage::reportedSplits). Empty when copied, else why not.
std::string copyFromInstalled(const std::wstring& apk, const std::wstring& own) {
    const std::wstring splitsDir = Library::splitsDirFor(apk);
    std::vector<std::wstring> splits = files::list(splitsDir, L"*");
    if (splits.empty()) splits = appPackage::reportedSplits(apk);
    if (!files::copy(apk, own + L".part") || !files::move(own + L".part", own)) {
        const DWORD error = GetLastError();
        files::remove(own + L".part");
        return format("its APK could not be copied back from the installed app (error %lu)", error);
    }
    if (splits.empty()) return std::string();
    const std::wstring mine = Library::splitsDirFor(own);
    files::removeTree(mine + L".part");
    files::makeDirectory(mine + L".part");
    for (const auto& name : splits) {
        if (!files::copy(splitsDir + L"\\" + name, mine + L".part\\" + name)) {
            const DWORD error = GetLastError();
            files::removeTree(mine + L".part");
            files::remove(own);
            return format("its split APKs could not be copied back from the installed app (error %lu)", error);
        }
    }
    files::removeTree(mine);
    files::move(mine + L".part", mine);
    return std::string();
}

} // namespace

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
            // A locally bundled BiTGApps archive is offered once in the library.
            // Removing one of these entries must not silently put it back on
            // the next launch or replace an APK the user already installed.
            const std::wstring bitgapps = platform::installDirectory() + L"\\bitgapps";
            const std::wstring bitgappsStamp = directory_ + L"\\bitgapps.added";
            // GSF has no launcher activity, so it is a shared service APK rather than a Windows app.
            // Publish it once for every client APPX of this publisher to mount from the same path.
            const std::wstring gsfSource = bitgapps + L"\\GoogleServicesFramework.apk";
            const std::wstring shared = appPackage::sharedFolder();
            if (!shared.empty() && files::exists(gsfSource)) {
                const std::wstring gsfFolder = shared + L"\\services\\gsf";
                const std::wstring gsfTarget = gsfFolder + L"\\GoogleServicesFramework.apk";
                if (!files::exists(gsfTarget) && files::makeDirectories(gsfFolder)) {
                    if (files::copy(gsfSource, gsfTarget + L".part") && files::move(gsfTarget + L".part", gsfTarget))
                        shellLog::write("library: published Google Services Framework in the shared service cache");
                    else shellLog::write("library: could not publish Google Services Framework");
                }
            }
            // microG's Google Play services, when the package brings them: published for apps set to them (shell/Settings.h,
            // googleServices) beside Google's own; again when this build's differs in size from the one published.
            const std::wstring microgSource = bitgapps + L"\\MicroGGmsCore.apk";
            if (!shared.empty() && files::exists(microgSource)) {
                const std::wstring microgFolder = shared + L"\\services\\microg";
                const std::wstring microgTarget = microgFolder + L"\\GmsCore.apk";
                if ((!files::exists(microgTarget) || files::size(microgTarget) != files::size(microgSource)) && files::makeDirectories(microgFolder)) {
                    if (files::copy(microgSource, microgTarget + L".part") && files::move(microgTarget + L".part", microgTarget))
                        shellLog::write("library: published microG's Google Play services in the shared service cache");
                    else shellLog::write("library: could not publish microG's Google Play services");
                }
            }
            // GSF is no app to start or to install as one: Google Play services and the Play Store use it from the shared
            // folder (above), so the library does not list it, nor keep the copy an older Megler offered there.
            {
                const std::wstring gsfCopy = directory_ + L"\\bitgapps-GoogleServicesFramework.apk";
                if (files::exists(gsfCopy)) {
                    files::remove(gsfCopy);
                    files::remove(gsfCopy + L".json");
                    files::remove(gsfCopy + L".icon");
                    shellLog::write("library: Google Services Framework is used from the shared service cache, not listed");
                }
            }
            if (files::exists(bitgapps + L"\\PrebuiltGmsCore.apk") && !files::exists(bitgappsStamp)) {
                bool copied = true;
                for (const wchar_t* name : {L"PrebuiltGmsCore.apk", L"Phonesky.apk"}) {
                    const std::wstring destination = directory_ + L"\\bitgapps-" + name;
                    // (Through a .part, as every other copy here: one that does not finish -- Megler ended or suspended under
                    // it, the phone full -- is not taken for the APK.)
                    if (!files::exists(destination) &&
                        !(files::copy(bitgapps + L"\\" + name, destination + L".part") && files::move(destination + L".part", destination))) {
                        files::remove(destination + L".part");
                        copied = false;
                        shellLog::write("library: could not offer BiTGApps " + narrow(name));
                    }
                }
                if (copied) {
                    files::write(bitgappsStamp, "offered");
                    shellLog::write("library: offered locally bundled BiTGApps APKs");
                }
            }
            /*
             * A copy of the bundle's that never finished, which the first start made where it was to be, not through a
             * .part (Megler before 576): the whole size, its end zeros. Its report was "Not a ZIP archive (no end
             * record)", and Google Play services could not be installed from the library. Copied again when it is not the
             * bundle's (its size and its end), which also brings the bundle's after an update of Megler.
             */
            for (const wchar_t* name : {L"PrebuiltGmsCore.apk", L"Phonesky.apk"}) {
                const std::wstring own = directory_ + L"\\bitgapps-" + name;
                const std::wstring source = bitgapps + L"\\" + name;
                if (!files::exists(own) || files::exists(own + L".installed") || !files::exists(source) ||
                    files::sameEnd(own, source, 65536))
                    continue;
                if (files::copy(source, own + L".part") && files::move(own + L".part", own)) {
                    files::remove(own + L".json");
                    files::remove(own + L".icon");
                    shellLog::write("library: bitgapps-" + narrow(name) + " was not this Megler's bundle's (a copy that never finished, or an older one): copied again");
                } else {
                    files::remove(own + L".part");
                    shellLog::write("library: bitgapps-" + narrow(name) + " is not the bundle's and could not be copied again");
                }
            }
            // An older update could leave an .installed marker pointing at an APPX that Windows
            // removed during replacement. Recover the bundled Google APK before listing entries;
            // otherwise Update tries to pack app.apk from the vanished APPX again.
            for (const wchar_t* name : {L"PrebuiltGmsCore.apk", L"Phonesky.apk"}) {
                const std::wstring own = directory_ + L"\\bitgapps-" + name;
                const std::wstring marker = own + L".installed";
                std::string text;
                if (!files::read(marker, text) || appPackage::canRead(installedOf(text).apk)) continue;
                const std::wstring source = bitgapps + L"\\" + name;
                if (!files::exists(source)) continue;
                if (files::copy(source, own + L".part") && files::move(own + L".part", own)) {
                    files::remove(marker);
                    files::remove(own + L".json");
                    files::remove(own + L".icon");
                    shellLog::write("library: recovered " + narrow(name) + " from this Megler's BiTGApps bundle");
                } else {
                    files::remove(own + L".part");
                    shellLog::write("library: could not recover " + narrow(name) + " from the BiTGApps bundle");
                }
            }
        }

        std::string stamp;
        files::read(directory_ + L"\\reports.version", stamp);
        const bool stale = stamp != versionStamp();
        entries_.clear();
        const auto copied = [this](const std::wstring& name) {
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
            return entry;
        };
        for (const auto& name : files::list(directory_, L"*.apk")) {
            ApkEntry entry = copied(name);
            if (stale || !entry.analysed) toRead.push_back(entry.id);
            entries_.push_back(std::move(entry));
        }
        // The apps whose APK is an installed package's (Megler has no copy of it).
        for (const auto& name : files::list(directory_, L"*.apk.installed")) {
            const std::string id = narrow(name.substr(0, name.size() - 10));
            std::string marker;
            if (std::any_of(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; }) ||
                !files::read(directory_ + L"\\" + name, marker))
                continue;
            ApkEntry entry = installedEntry(id, marker);
            const std::string fullName = entry.installedPackage;
            // An app in several APKs whose package (one made by 640 to 644, with the APK in it) shows Megler none of its
            // folders, as one installed as the Store installs an app does, which those let their copy go for: the core reads
            // an app's splits as a folder, so the library had it without them, as an APK that cannot run, and Update All left
            // it out. Its APKs are copied back from the package by the names its report gives, and the library has its own
            // copy again, for its next package to take to the shared folder. (That report is read only for an app read with
            // splits, or that cannot run.)
            if (!appPackage::inSharedApks(entry.path) && entry.splitsDir.empty() && (!entry.report.splits.empty() || !entry.report.runnable) &&
                !appPackage::reportedSplits(entry.path).empty()) {
                const std::string error = copyFromInstalled(entry.path, directory_ + L"\\" + widen(id));
                if (error.empty()) {
                    files::remove(directory_ + L"\\" + name);
                    shellLog::write("library: " + id + "'s APK and its splits copied back from " + fullName + ", whose folders Megler is not shown");
                    entry = copied(widen(id));
                    toRead.push_back(entry.id);
                    entries_.push_back(std::move(entry));
                    continue;
                }
                shellLog::write("library: " + id + "'s splits, in " + fullName + ", whose folders Megler is not shown, could not be copied back: " + error);
            }
            if ((stale || !entry.analysed) && files::exists(entry.path)) toRead.push_back(entry.id);
            entries_.push_back(std::move(entry));
        }
        if (appPackage::single().package == "com.google.android.gms")
            for (const auto& entry : entries_) appPackage::registerServiceApk(entry, narrow(winrt::Windows::ApplicationModel::Package::Current().Id().FullName()));
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
    // (.apkm: APKMirror's, base.apk and split_*.apk with its info.json, which the core unpacks as it unpacks the others.)
    return endsWith(".apks") || endsWith(".xapk") || endsWith(".apkm");
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
        if (appPackage::single().package == "com.google.android.gms")
            appPackage::registerServiceApk(entry, narrow(winrt::Windows::ApplicationModel::Package::Current().Id().FullName()));
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

ApkEntry Library::installedEntry(const std::string& id, const std::string& marker) const {
    const std::wstring own = directory_ + L"\\" + widen(id);
    const Installed installed = installedOf(marker);
    ApkEntry entry;
    entry.id = id;
    entry.installedPackage = installed.fullName;
    entry.path = installed.apk;
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

void Library::adopt(const std::string& id, const std::string& fullName, const std::wstring& sharedApk) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::wstring own = directory_ + L"\\" + widen(id);
    const std::string marker = sharedApk.empty() ? fullName : fullName + "\r\nshared=" + narrow(sharedApk) + "\r\n";
    if (!files::write(own + L".installed", marker)) {
        shellLog::write("library: " + id + " keeps its copy: " + narrow(own) + ".installed could not be written");
        return;
    }
    const bool hadCopy = files::exists(own);
    files::remove(own);
    files::removeTree(splitsDirFor(own));
    ApkEntry entry = installedEntry(id, marker);
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
    std::wstring apk, own;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find_if(entries_.begin(), entries_.end(), [&](const ApkEntry& e) { return e.id == id; });
        if (it == entries_.end()) return "it is not in the library";
        if (it->installedPackage.empty()) return std::string();
        apk = it->path;
        own = directory_ + L"\\" + widen(id);
    }
    if (const std::string error = copyFromInstalled(apk, own); !error.empty()) return error;
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
