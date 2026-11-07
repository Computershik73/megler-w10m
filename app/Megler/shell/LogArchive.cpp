#include "pch.h"

#include "shell/LogArchive.h"

#include "megler.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/LogMirror.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "shell/ZipWriter.h"

#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Security.ExchangeActiveSyncProvisioning.h>

#include <algorithm>
#include <map>

namespace megler::app::logArchive {

using namespace winrt;
using namespace winrt::Windows::Storage;

namespace {

std::mutex gMutex;
State gState;
StorageFile gFile{nullptr};

/// A file of the ZIP: one read where it is (`path`), or a text made here.
struct Item {
    std::string name;   // in the ZIP: "<identity>/megler.log"
    std::wstring path;  // empty for `text`
    std::string text;
    uint64_t size = 0;  // what is to be read of it
};

void setStep(const std::string& step) {
    std::lock_guard<std::mutex> lock(gMutex);
    gState.step = step;
    gState.generation++;
}

void setDone(uint64_t done) {
    std::lock_guard<std::mutex> lock(gMutex);
    gState.done = done;
}

bool endsWith(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

/// What is read of a file of `size` bytes: at most its last capBytes.
uint64_t readable(uint64_t size) { return std::min(size, logMirror::capBytes); }

/// The files: this package's from its own LocalState (the newest there are, its log flushed first), every other package's
/// copies from the shared folder, and the settings all of them read.
std::vector<Item> gather() {
    std::vector<Item> items;
    const std::wstring local = platform::localStateDirectory();
    const std::string own = narrow(logMirror::identity());
    for (const std::wstring& name : logMirror::names()) {
        const std::wstring path = local + L"\\" + name;
        if (files::exists(path)) items.push_back({own + "/" + narrow(name), path, {}, readable(files::size(path))});
    }
    if (const std::string flags = logMirror::flagsText(); !flags.empty()) items.push_back({own + "/flags.txt", {}, flags, flags.size()});
    const std::string about = logMirror::packageText();
    items.push_back({own + "/package.txt", {}, about, about.size()});
    // (This package's own copies there are older than its LocalState: left out.)
    const std::wstring logs = logMirror::root();
    if (!logs.empty()) {
        auto folders = files::listFolders(logs, L"*");
        std::sort(folders.begin(), folders.end());
        for (const auto& folder : folders) {
            if (_wcsicmp(folder.c_str(), logMirror::identity().c_str()) == 0) continue;
            auto names = files::list(logs + L"\\" + folder, L"*");
            std::sort(names.begin(), names.end());
            for (const auto& name : names) {
                if (endsWith(name, L".part")) continue;
                const std::wstring path = logs + L"\\" + folder + L"\\" + name;
                items.push_back({narrow(folder) + "/" + narrow(name), path, {}, readable(files::size(path))});
            }
        }
    }
    const std::wstring shared = appPackage::sharedFolder();
    if (!shared.empty()) {
        if (files::exists(shared + L"\\settings.ini"))
            items.push_back({"shared/settings.ini", shared + L"\\settings.ini", {}, readable(files::size(shared + L"\\settings.ini"))});
        auto apps = files::listFolders(shared + L"\\apps", L"*");
        std::sort(apps.begin(), apps.end());
        for (const auto& app : apps) {
            const std::wstring path = shared + L"\\apps\\" + app + L"\\settings.ini";
            if (files::exists(path)) items.push_back({"shared/apps/" + narrow(app) + "/settings.ini", path, {}, readable(files::size(path))});
        }
    }
    return items;
}

/// A FILETIME (UTC) in local time, "2026-11-08 02:14".
std::string localTime(uint64_t filetime) {
    FILETIME utc{DWORD(filetime & 0xffffffff), DWORD(filetime >> 32)}, local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return "?";
    return format("%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

/// The local time's offset from UTC, "UTC+04:00".
std::string utcOffset() {
    SYSTEMTIME utc{}, local{};
    GetSystemTime(&utc);
    GetLocalTime(&local);
    FILETIME a{}, b{};
    SystemTimeToFileTime(&utc, &a);
    SystemTimeToFileTime(&local, &b);
    const int64_t ticks = int64_t((uint64_t(b.dwHighDateTime) << 32) | b.dwLowDateTime) - int64_t((uint64_t(a.dwHighDateTime) << 32) | a.dwLowDateTime);
    // (To the minute: the two were read a moment apart.)
    const int64_t minutes = (ticks + (ticks >= 0 ? 300000000 : -300000000)) / 600000000;
    const int64_t whole = minutes < 0 ? -minutes : minutes;
    return format("UTC%c%02lld:%02lld", minutes < 0 ? '-' : '+', (long long)(whole / 60), (long long)(whole % 60));
}

/// The device as its maker names it, "Microsoft Corporation RM-1154_1003".
std::string model() {
    try {
        winrt::Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation info;
        std::string text = narrow(std::wstring(info.SystemManufacturer())) + " " + narrow(std::wstring(info.SystemProductName()));
        const std::string sku = narrow(std::wstring(info.SystemSku()));
        if (!sku.empty() && text.find(sku) == std::string::npos) text += " (" + sku + ")";
        return text;
    } catch (...) {
        return "unknown";
    }
}

std::string freeSpace(const std::wstring& path) {
    ULARGE_INTEGER free{}, total{};
    if (path.empty() || !GetDiskFreeSpaceExW(path.c_str(), &free, &total, nullptr)) return "unknown";
    return formattedByteCount(free.QuadPart) + " free of " + formattedByteCount(total.QuadPart);
}

const char* processor(winrt::Windows::System::ProcessorArchitecture a) {
    using winrt::Windows::System::ProcessorArchitecture;
    switch (a) {
    case ProcessorArchitecture::Arm: return "ARM";
    case ProcessorArchitecture::Arm64: return "ARM64";
    case ProcessorArchitecture::X64: return "x64";
    case ProcessorArchitecture::X86: return "x86";
    case ProcessorArchitecture::Neutral: return "neutral";
    default: return "?";
    }
}

/// The packages of Megler's publisher, as Windows's package manager lists them (packageQuery, which an app's own package
/// keeps): Megler, its test identities, the apps' own.
std::string packages() {
    try {
        const std::wstring publisher(winrt::Windows::ApplicationModel::Package::Current().Id().PublisherId());
        std::vector<std::string> rows;
        for (auto const& package : winrt::Windows::Management::Deployment::PackageManager().FindPackagesForUser(L"")) {
            const auto id = package.Id();
            if (std::wstring(id.PublisherId()) != publisher) continue;
            const auto v = id.Version();
            std::string row = format("  %s %u.%u.%u.%u %s", narrow(std::wstring(id.Name())).c_str(), v.Major, v.Minor, v.Build, v.Revision,
                                     processor(id.Architecture()));
            try {
                if (package.IsDevelopmentMode()) row += ", a developer's";
            } catch (...) {
            }
            rows.push_back(row + "\r\n");
        }
        std::sort(rows.begin(), rows.end());
        std::string text;
        for (const auto& row : rows) text += row;
        return text.empty() ? "  none listed\r\n" : text;
    } catch (hresult_error const& e) {
        return format("  not listed: 0x%08x ", unsigned(e.code())) + narrow(e.message()) + "\r\n";
    }
}

/// The last install refused (appPackage::installWithPackageManager writes it): Megler's own LocalState's, or, in an app's
/// own package, which installs nothing, Megler's copy of it in the shared folder.
std::string lastRefusal() {
    std::string text;
    std::string from;
    if (!files::read(platform::localStateDirectory() + L"\\install-refusal.txt", text) && appPackage::isSingleApp()) {
        const std::wstring family = appPackage::meglerFamilyName();
        const std::wstring megler = family.substr(0, family.rfind(L'_'));
        if (files::read(logMirror::root() + L"\\" + megler + L"\\install-refusal.txt", text)) from = "  (Megler's, from its logs in the shared folder)\r\n";
    }
    if (text.empty()) return "  none\r\n";
    std::string indented;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) indented += "  " + line + "\r\n";
        at = end + 1;
    }
    return indented + from;
}

std::string summary(const std::vector<Item>& items) {
    std::string s = "Megler's logs\r\n\r\n";
    const auto line = [&s](const char* key, const std::string& value) { s += std::string(key) + ": " + value + "\r\n"; };
    const std::string own = narrow(logMirror::identity());
    line("Saved", localDateTime() + " (" + utcOffset() + "), by " + own +
                      (appPackage::isSingleApp() ? ", " + appPackage::single().label + "'s own package" : std::string()));
    line("Megler", platform::packageVersion());
    // (On desktop Windows the core is an ELF that may not have loaded: then it says nothing.)
    const char* core = megler_version();
    line("Core", core ? core : "not loaded");
    line("Architecture", platform::architecture());
    line("Device family", platform::deviceFamily());
    line("Device", model());
    line("Windows", platform::osVersion());
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    line("Memory", platform::memorySummary() +
                       (GlobalMemoryStatusEx(&memory) ? "; the device has " + formattedByteCount(memory.ullTotalPhys) : std::string()));
    line("Android Code", platform::canRunAndroidCode() ? "Can run" : "Cannot run");
    line("ANGLE", platform::angle().summary);
    line("Free space", freeSpace(platform::localStateDirectory()) + " (LocalState's drive); " + freeSpace(appPackage::sharedFolder()) +
                           " (the shared folder's)");
    s += "\r\nMegler's packages, as Windows lists them:\r\n" + packages();
    s += "\r\nThe last install refused:\r\n" + lastRefusal();
    // What the ZIP holds, a folder a line, with when the newest of its files was written (whether a package's copies are of
    // today): "MeglerApp.x, newest 2026-11-08 02:14: megler.log 1.2 MB, megler.prev.log 300 KB".
    s += "\r\nIn this ZIP (a file over " + formattedByteCount(logMirror::capBytes) + " as its last part):\r\n";
    std::map<std::string, std::pair<uint64_t, std::string>> folders;
    for (const Item& item : items) {
        const size_t slash = item.name.rfind('/');
        auto& [newest, row] = folders[item.name.substr(0, slash)];
        if (!item.path.empty()) newest = std::max(newest, files::lastWrite(item.path));
        row += (row.empty() ? "" : ", ") + item.name.substr(slash + 1) + " " +
               formattedByteCount(item.path.empty() ? item.text.size() : files::size(item.path));
    }
    for (const auto& [folder, about] : folders)
        s += "  " + folder + (folder == own ? " (this package, from its LocalState)" : std::string()) +
             (about.first ? ", newest " + localTime(about.first) : std::string()) + ": " + about.second + "\r\n";
    return s;
}

/// One file into the ZIP, read a MB at a time: as it is now (a log another package appends to meanwhile, as far as it
/// was), over capBytes from tailStart after the line that says so. False with `why` where it could not all be read.
bool addFile(ZipWriter& zip, const Item& item, uint64_t& done, std::string& why) {
    const uint64_t size = files::size(item.path);
    uint64_t at = 0;
    std::string note;
    if (size > logMirror::capBytes) {
        at = logMirror::tailStart(item.path, size);
        note = logMirror::tailNote(at, size);
    }
    if (!zip.begin(item.name, files::lastWrite(item.path)) || (!note.empty() && !zip.write(note.data(), note.size()))) return false;
    std::string piece;
    bool whole = true;
    while (at < size) {
        if (!files::readRange(item.path, at, std::min<uint64_t>(1u << 20, size - at), piece) || piece.empty()) {
            why = format("read as far as byte %llu of %llu (error %lu)", (unsigned long long)at, (unsigned long long)size, GetLastError());
            whole = false;
            break;
        }
        if (!zip.write(piece.data(), piece.size())) return false;
        at += piece.size();
        done += piece.size();
        setDone(done);
    }
    return zip.end() && whole;
}

/// The newest three ZIPs kept in LocalState\logs, where they stay when the Pictures folder cannot be had; what an earlier
/// saving left half made goes.
void prune(const std::wstring& folder) {
    for (const auto& name : files::list(folder, L"*.part")) files::remove(folder + L"\\" + name);
    auto zips = files::list(folder, L"megler-logs-*.zip");
    std::sort(zips.begin(), zips.end());
    for (size_t i = 0; i + 3 < zips.size(); i++) files::remove(folder + L"\\" + zips[i]);
}

void save() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string name = format("megler-logs-%04u%02u%02u-%02u%02u%02u.zip", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring folder = platform::localStateDirectory() + L"\\logs", path = folder + L"\\" + widen(name), part = path + L".part";
    State result;
    result.name = name;
    files::makeDirectories(folder);
    prune(folder);

    setStep("Looking for the logs…");
    shellLog::write("logs: saving all logs as " + name);
    shellLog::flush();
    const std::vector<Item> items = gather();
    uint64_t total = 0;
    for (const Item& item : items) total += item.size;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gState.total = total;
        gState.done = 0;
    }
    ZipWriter zip(part);
    zip.add("summary.txt", summary(items));
    uint64_t done = 0;
    std::string missed;
    for (size_t i = 0; i < items.size() && zip.ok(); i++) {
        setStep(format("Saving the logs: %u of %u files…", unsigned(i + 1), unsigned(items.size())));
        const Item& item = items[i];
        std::string why;
        if (item.path.empty()) {
            zip.add(item.name, item.text);
            done += item.text.size();
            setDone(done);
        } else if (!addFile(zip, item, done, why) && zip.ok()) {
            missed += item.name + ": " + why + "\r\n";
        }
    }
    if (!missed.empty()) zip.add("not-whole.txt", "Files that could not all be read (another package may have been rotating its logs):\r\n" + missed);
    const unsigned count = unsigned(zip.entries());
    if (!zip.finish("Megler's logs, saved " + localDateTime() + " by " + narrow(logMirror::identity())) || !files::move(part, path)) {
        const std::string why = zip.ok() ? format("the ZIP could not be put in place (error %lu)", GetLastError()) : zip.error();
        files::remove(part);
        shellLog::write("logs: not saved: " + why);
        std::lock_guard<std::mutex> lock(gMutex);
        gState.running = false;
        gState.saved = false;
        gState.error = "The logs were not saved: " + why + ".";
        gState.generation++;
        return;
    }
    result.size = files::size(path);
    result.files = count;

    // Where the user finds it: Pictures\Megler\logs, which the phone's file manager shows and a PC reaches over USB.
    StorageFile saved{nullptr};
    if (platform::isXbox()) {
        result.note = "An Xbox gives apps no Pictures folder: the ZIP is in Megler's LocalState\\logs, which the console's Device Portal "
                      "shows (File explorer, LocalAppData).";
    } else {
        try {
            setStep("Copying the ZIP to Pictures\\Megler\\logs…");
            auto megler = KnownFolders::PicturesLibrary().CreateFolderAsync(L"Megler", CreationCollisionOption::OpenIfExists).get();
            auto logs = megler.CreateFolderAsync(L"logs", CreationCollisionOption::OpenIfExists).get();
            saved = StorageFile::GetFileFromPathAsync(path).get().CopyAsync(logs, widen(name), NameCollisionOption::ReplaceExisting).get();
            files::remove(path);
            result.where = "Pictures\\Megler\\logs";
            shellLog::write("logs: " + narrow(std::wstring(saved.Path())) + format(" saved, %u files, ", count) + formattedByteCount(result.size));
        } catch (hresult_error const& e) {
            saved = nullptr;
            // (An app's own package declares the Pictures library only where its app's permissions want it.)
            result.note = appPackage::isSingleApp() && e.code() == E_ACCESSDENIED
                              ? "This app's own package may not write to the Pictures folder, so the ZIP is in its LocalState\\logs: Share "
                                "sends it. Megler's own Log screen saves all logs to Pictures\\Megler\\logs."
                              : format("The ZIP could not be copied to Pictures\\Megler\\logs (0x%08x ", unsigned(e.code())) +
                                    narrow(e.message()) + "), so it is in LocalState\\logs: Share sends it.";
            shellLog::write("logs: " + narrow(path) + " saved, not copied to Pictures\\Megler\\logs: " + format("0x%08x ", unsigned(e.code())) +
                            narrow(e.message()));
        }
    }
    if (!saved) {
        result.where = "LocalState\\logs";
        try {
            saved = StorageFile::GetFileFromPathAsync(path).get();
        } catch (...) {
        }
    }
    std::lock_guard<std::mutex> lock(gMutex);
    gFile = saved;
    result.saved = true;
    result.done = gState.done;
    result.total = gState.total;
    result.generation = gState.generation + 1;
    gState = result;
}

} // namespace

State state() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gState;
}

StorageFile file() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gFile;
}

bool start() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gState.running) return false;
        gState.running = true;
        gState.saved = false;
        gState.error.clear();
        gState.note.clear();
        gState.done = gState.total = 0;
        gState.step = "Looking for the logs…";
        gState.generation++;
    }
    try {
        std::thread([] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            } catch (...) {
            }
            try {
                save();
                return;
            } catch (hresult_error const& e) {
                shellLog::write("logs: saving failed: " + narrow(e.message()));
            } catch (const std::exception& e) {
                shellLog::write(std::string("logs: saving failed: ") + e.what());
            } catch (...) {
                shellLog::write("logs: saving failed");
            }
            std::lock_guard<std::mutex> lock(gMutex);
            gState.running = false;
            gState.error = "The logs were not saved: something failed on the way (the log says what).";
            gState.generation++;
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(gMutex);
        gState.running = false;
        gState.error = "The logs were not saved: no thread could be started for it.";
        gState.generation++;
        return false;
    }
    return true;
}

} // namespace megler::app::logArchive
