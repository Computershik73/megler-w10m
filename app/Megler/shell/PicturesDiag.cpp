#include "pch.h"

#include "shell/PicturesDiag.h"

#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

using namespace winrt;
using namespace winrt::Windows::Storage;

namespace megler::app::picturesDiag {
namespace {

std::mutex gMutex;
std::condition_variable gReady;
bool gStarted = false, gFlagsDone = false;
StorageFolder gFolder{nullptr}; // the diagnosis's folder, where one asked for this package
std::mutex gCopyMutex;

void apartment() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
}

/// A name a flag may have (core: mg_flag): letters, digits, '-', '_' and '.', as trace-calls-X.0h3 has.
bool flagName(const std::wstring& name) {
    if (name.empty() || name.size() > 120 || name[0] == L'.') return false;
    for (wchar_t c : name)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.'))
            return false;
    return true;
}

StorageFolder child(StorageFolder const& folder, const std::wstring& name) {
    if (!folder) return nullptr;
    auto item = folder.TryGetItemAsync(name).get();
    return item ? item.try_as<StorageFolder>() : nullptr;
}

std::set<std::wstring> lines(const std::string& text) {
    std::set<std::wstring> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::wstring name = widen(text.substr(at, end - at));
        if (flagName(name)) out.insert(name);
        at = end + 1;
    }
    return out;
}

/// The flags the folder's "flags" names, in LocalState\flags; those put there so before and not named now, taken away. A flag
/// that was there already (the portal's) is not written down as the folder's, and stays when the folder goes.
void placeFlags(StorageFolder const& folder) {
    const std::wstring local = platform::localStateDirectory();
    const std::wstring dir = local + L"\\flags", record = local + L"\\flags-from-pictures.txt";
    std::set<std::wstring> wanted;
    if (auto flags = child(folder, L"flags"))
        for (auto const& file : flags.GetFilesAsync().get())
            if (flagName(std::wstring(file.Name()))) wanted.insert(std::wstring(file.Name()));
    std::string text;
    const std::set<std::wstring> before = files::read(record, text) ? lines(text) : std::set<std::wstring>();
    for (const auto& name : before)
        if (!wanted.count(name) && files::remove(dir + L"\\" + name))
            shellLog::write("diag: the flag " + narrow(name) + " is off: Pictures\\Megler\\diag names it no longer");
    std::string ours, said;
    for (const auto& name : wanted) {
        const std::wstring path = dir + L"\\" + name;
        if (!before.count(name) && files::exists(path)) continue;
        files::makeDirectories(dir);
        if (!files::exists(path)) files::write(path, "x");
        ours += narrow(name) + "\n";
        said += (said.empty() ? "" : ", ") + narrow(name);
    }
    if (ours.empty()) files::remove(record);
    else files::write(record, ours);
    if (!said.empty()) shellLog::write("diag: flags from Pictures\\Megler\\diag: " + said);
}

/// A file of LocalState copied into the folder, replacing the one there.
void copyOut(StorageFolder const& folder, const wchar_t* name) {
    static std::atomic<int> failures{0};
    std::lock_guard<std::mutex> lock(gCopyMutex);
    try {
        const std::wstring path = platform::localStateDirectory() + L"\\" + name;
        if (!files::exists(path)) return;
        auto file = StorageFile::GetFileFromPathAsync(path).get();
        file.CopyAsync(folder, name, NameCollisionOption::ReplaceExisting).get();
    } catch (hresult_error const& e) {
        if (failures++ < 3) shellLog::write("diag: " + narrow(name) + " could not be copied to Pictures\\Megler\\diag: " + narrow(e.message()));
    } catch (...) {
        if (failures++ < 3) shellLog::write("diag: " + narrow(name) + " could not be copied to Pictures\\Megler\\diag");
    }
}

/// The folder looked for and the flags put in place. Whatever goes wrong only costs the diagnosis: this runs at every start of
/// every package of Megler's, and a throw out of a thread would end the app.
StorageFolder lookAndPlace(const std::string& name) {
    StorageFolder folder{nullptr};
    try {
        folder = child(child(child(KnownFolders::PicturesLibrary(), L"Megler"), L"diag"), widen(name));
    } catch (hresult_error const& e) {
        shellLog::write("diag: Pictures\\Megler\\diag could not be looked at: " + narrow(e.message()));
    } catch (...) {
        shellLog::write("diag: Pictures\\Megler\\diag could not be looked at");
    }
    try {
        placeFlags(folder);
    } catch (hresult_error const& e) {
        shellLog::write("diag: the flags in Pictures\\Megler\\diag could not be read: " + narrow(e.message()));
    } catch (...) {
        shellLog::write("diag: the flags in Pictures\\Megler\\diag could not be read");
    }
    return folder;
}

} // namespace

void start() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gStarted) return;
        gStarted = true;
    }
    auto work = [] {
        apartment();
        StorageFolder folder{nullptr};
        std::string name = "megler";
        try {
            if (appPackage::isSingleApp()) name = appPackage::single().package;
            folder = lookAndPlace(name);
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gFolder = folder;
            gFlagsDone = true;
        }
        gReady.notify_all();
        if (!folder) return;
        shellLog::write("diag: Pictures\\Megler\\diag\\" + name + " asks for this package's log: it is copied there every 10 s");
        shellLog::flush();
        copyOut(folder, L"megler.prev.log");
        for (;;) {
            shellLog::flush();
            copyOut(folder, L"megler.log");
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    };
    try {
        std::thread(work).detach();
    } catch (...) {
        // (No thread for it: no diagnosis, and nobody waits for one.)
        std::lock_guard<std::mutex> lock(gMutex);
        gFlagsDone = true;
    }
}

void waitForFlags() {
    start();
    std::unique_lock<std::mutex> lock(gMutex);
    gReady.wait_for(lock, std::chrono::seconds(5), [] { return gFlagsDone; });
}

void exportNow() {
    StorageFolder folder{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        folder = gFolder;
    }
    if (!folder) return;
    try {
        auto done = std::make_shared<std::promise<void>>();
        auto finished = done->get_future();
        std::thread([folder, done] {
            apartment();
            shellLog::flush();
            copyOut(folder, L"megler.log");
            done->set_value();
        }).detach();
        finished.wait_for(std::chrono::seconds(2));
    } catch (...) {
    }
}

} // namespace megler::app::picturesDiag
