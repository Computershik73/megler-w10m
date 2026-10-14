// The test program's way to the shell's storage host: megler_storage_host as
// app/Megler/player/StorageHost.cpp implements it over Windows.Storage, for
// hk_tests --storage-windows (hk_tests_main.c), which runs the storage's
// tests (hk_storage_tests.c) with it in the stand-in's place.
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
// uses of the shell is stood in for below.
#include "pch.h"

#include "player/StorageHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <cstdio>

namespace megler::app {

// The shell's log: said as the tests say things.
namespace shellLog {
void write(const std::string& line) {
    printf("  %s\n", line.c_str());
    fflush(stdout);
}
} // namespace shellLog

// The flags folder of LocalState, which a test program has none of: no flag is on.
namespace files {
bool exists(const std::wstring&) { return false; }
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
const megler_storage_host* hk_storage_windows_host(const char* const* names, const char* const* folders, int count) {
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
void hk_storage_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", megler::app::StorageHost::shared().summary().c_str()); }

} // extern "C"
