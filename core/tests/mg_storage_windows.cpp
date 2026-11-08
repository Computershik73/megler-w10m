// The test program's way to the shell's storage host: megler_storage_host as
// app/Megler/player/StorageHost.cpp implements it over Windows.Storage, for
// mg_tests --storage-windows (mg_tests_main.c), which runs the storage's
// tests (mg_storage_tests.c) with it in the stand-in's place.
//
// What that shows: the host's own work -- a path walked folder by folder,
// handles asked of Windows.Storage's objects, folders made, things removed
// and renamed, errors put into megler.h's words, several callers at once --
// against the real classes, on folders of the tests' own. What it cannot
// show: a PC's test program is no app container, so no broker stands between
// it and the files, and the Windows is not a phone's. Whether the broker
// refuses what a desktop process is let do is found out on a phone.
//
// Only tools/build-tests.sh win builds this file, with StorageHost.cpp itself
// and the shell's Strings.cpp beside it. The rest of what StorageHost.cpp
// uses of the shell is stood in for below, for the other hosts built in the
// same way too (the window's, mg_window_windows.cpp).
#include "pch.h"

#include "player/StorageHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <cstdio>
#include <set>

namespace {
// What the shell's log said, for a test to look for (mg_shell_said), and the flags a test has put on (mg_shell_flag).
std::mutex gMutex;
std::vector<std::string> gSaid;
std::set<std::wstring> gFlags;
} // namespace

namespace megler::app {

// The shell's log: said as the tests say things, and kept.
namespace shellLog {
void write(const std::string& line) {
    printf("  %s\n", line.c_str());
    fflush(stdout);
    std::lock_guard<std::mutex> lock(gMutex);
    gSaid.push_back(line);
}
} // namespace shellLog

// LocalState, which a test program has none of: no flag is on but those a test puts on (mg_shell_flag: a path that ends
// in flags\<name>), and the codec host's list of decoders that hung (codec-hung.txt) is not there and is not written, nor
// is what the camera host learnt of the cameras (camera.txt).
namespace files {
bool exists(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(gMutex);
    for (const std::wstring& name : gFlags)
        for (const wchar_t* folder : {L"/flags/", L"\\flags\\"}) {
            const std::wstring end = folder + name;
            if (path.size() >= end.size() && path.compare(path.size() - end.size(), end.size(), end) == 0) return true;
        }
    return false;
}
bool read(const std::wstring&, std::string&) { return false; }
bool write(const std::wstring&, std::string_view) { return false; }
bool append(const std::wstring&, std::string_view) { return false; }
} // namespace files

namespace platform {
const std::wstring& localStateDirectory() {
    static const std::wstring none;
    return none;
}
} // namespace platform

} // namespace megler::app

extern "C" {

/// The shell's host with `count` folders of the tests' own as its links,
/// under `names`; null when Windows.Storage will not open one of them.
const megler_storage_host* mg_storage_windows_host(const char* const* names, const char* const* folders, int count) {
    using winrt::Windows::Storage::StorageFolder;
    static megler_storage_host host;
    try {
        // (This thread asks and waits, which a thread of the multithreaded apartment may.)
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::vector<std::pair<std::string, StorageFolder>> links;
        for (int i = 0; i < count; i++) {
            std::wstring path = megler::app::widen(folders[i]);
            for (wchar_t& c : path)
                if (c == L'/') c = L'\\';
            links.emplace_back(names[i], StorageFolder::GetFolderFromPathAsync(path).get());
        }
        megler::app::StorageHost::shared().startWith(std::move(links));
        host = megler::app::StorageHost::shared().callbacks();
        return &host;
    } catch (const winrt::hresult_error& e) {
        printf("  Windows.Storage would not open the tests' folders: 0x%08X\n", unsigned(e.code()));
        return nullptr;
    }
}

/// What the host was asked and which way Windows answered, in a line.
void mg_storage_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", megler::app::StorageHost::shared().summary().c_str()); }

/// The shell's flag `name` (a file of LocalState\flags on a phone) on or off for the hosts built into the tests.
void mg_shell_flag(const char* name, int on) {
    std::lock_guard<std::mutex> lock(gMutex);
    const std::wstring wide = megler::app::widen(name);
    if (on)
        gFlags.insert(wide);
    else
        gFlags.erase(wide);
}

/// How many lines of the shell's log since the program started have `fragment` in them.
int mg_shell_said(const char* fragment) {
    std::lock_guard<std::mutex> lock(gMutex);
    int n = 0;
    for (const std::string& line : gSaid)
        if (line.find(fragment) != std::string::npos) n++;
    return n;
}

} // extern "C"
