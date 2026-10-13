#include "pch.h"

#include "shell/Storage.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <windowsstoragecom.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace megler::app::storage {

namespace {

std::mutex gMutex;
std::vector<Folder> gFolders;
std::atomic<bool> gProbed{false};
std::atomic<bool> gStarted{false};

std::string forward(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    std::string s = narrow(path);
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

/// Lists a folder with the plain file functions, as the layer will: how many
/// entries (up to a hundred are counted), or the error.
bool list(const std::wstring& path, int& entries, DWORD& error) {
    WIN32_FIND_DATAW found;
    const std::wstring pattern = path + L"\\*";
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    entries = 0;
    error = 0;
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        // An empty folder of a volume's root has not even "." in it.
        return error == ERROR_FILE_NOT_FOUND;
    }
    do {
        if (wcscmp(found.cFileName, L".") && wcscmp(found.cFileName, L"..")) entries++;
    } while (entries < 100 && FindNextFileW(h, &found));
    FindClose(h);
    return true;
}

/// Makes a file and a folder there and takes them away again.
bool write(const std::wstring& path, DWORD& error) {
    const std::wstring file = path + L"\\megler-probe.tmp", folder = path + L"\\megler-probe-folder";
    error = 0;
    HANDLE h = CreateFile2(file.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(h, "megler", 4, &written, nullptr) && written == 4;
    if (!wrote) error = GetLastError();
    CloseHandle(h);
    if (!DeleteFileW(file.c_str()) && !error) error = GetLastError();
    if (wrote && CreateDirectoryW(folder.c_str(), nullptr)) RemoveDirectoryW(folder.c_str());
    else if (wrote && !error) error = GetLastError();
    return wrote && !error;
}

Folder tried(const std::string& android, const std::wstring& path, const std::string& how) {
    Folder f;
    f.android = android;
    f.path = forward(path);
    int entries = 0;
    DWORD listError = 0, writeError = 0;
    f.canList = list(path, entries, listError);
    f.canWrite = f.canList && write(path, writeError);
    f.note = how + ": " + (f.canList ? format("%d%s entries", entries, entries >= 100 ? " or more" : "") : format("cannot be listed (error %lu)", listError)) +
             (f.canList ? (f.canWrite ? ", can be written to" : format(", cannot be written to (error %lu)", writeError)) : std::string());
    return f;
}

/// The other way into a folder: through Windows.Storage's broker, which can
/// hand over a real file handle for a file of a folder it has let the app
/// into (IStorageFolderHandleAccess, Windows 10 1703 on). With handles the
/// layer's own reading, writing and mapping go on as with any file. Tried
/// here on one folder: listing, a file made, written, read back and removed,
/// a folder made and removed, and a handle to the folder itself.
std::string brokered(const winrt::Windows::Storage::StorageFolder& folder) {
    using namespace winrt::Windows::Storage;
    std::string said;
    try {
        const auto items = folder.GetItemsAsync(0, 100).get();
        said += format("the broker lists %u%s items", unsigned(items.Size()), items.Size() >= 100 ? " or more" : "");
    } catch (const winrt::hresult_error& e) {
        return format("the broker will not list it (0x%08X)", unsigned(e.code()));
    }
    winrt::com_ptr<IStorageFolderHandleAccess> access;
    if (!folder.try_as(access)) return said + "; it gives no file handles (no IStorageFolderHandleAccess)";
    HANDLE h = INVALID_HANDLE_VALUE;
    HRESULT hr = access->Create(L"megler-probe.tmp", HCO_CREATE_ALWAYS, HANDLE_ACCESS_OPTIONS(HAO_READ | HAO_WRITE | HAO_DELETE),
                                HANDLE_SHARING_OPTIONS(HSO_SHARE_READ | HSO_SHARE_DELETE), HO_NONE, nullptr, &h);
    if (FAILED(hr) || h == INVALID_HANDLE_VALUE || !h) {
        said += format("; a file handle is refused (0x%08X)", unsigned(hr));
    } else {
        char back[4] = {};
        DWORD done = 0;
        const bool wrote = WriteFile(h, "megler", 4, &done, nullptr) && done == 4;
        LARGE_INTEGER zero = {};
        const bool sought = SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) != 0;
        const bool read = ReadFile(h, back, 4, &done, nullptr) && done == 4 && !memcmp(back, "megler", 4);
        BY_HANDLE_FILE_INFORMATION info = {};
        const bool stat = GetFileInformationByHandle(h, &info) != 0;
        // Removing it through the handle, as unlink() would.
        FILE_DISPOSITION_INFO gone = {};
        gone.DeleteFile = TRUE;
        const bool removed = SetFileInformationByHandle(h, FileDispositionInfo, &gone, sizeof(gone)) != 0;
        const DWORD removeError = removed ? 0 : GetLastError();
        CloseHandle(h);
        said += format("; a file handle: written %s, sought %s, read back %s, its size and times %s, removed through the handle %s", wrote ? "yes" : "NO",
                       sought ? "yes" : "NO", read ? "yes" : "NO", stat ? "yes" : "NO", removed ? "yes" : format("NO (error %lu)", removeError).c_str());
        if (!removed) {
            try {
                folder.GetFileAsync(L"megler-probe.tmp").get().DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                said += ", removed by the broker instead";
            } catch (const winrt::hresult_error& e) {
                said += format(", and the broker will not remove it either (0x%08X)", unsigned(e.code()));
            }
        }
    }
    // A handle to a file that is not there: what error an open() would see.
    h = INVALID_HANDLE_VALUE;
    hr = access->Create(L"megler-probe-missing.tmp", HCO_OPEN_EXISTING, HAO_READ, HSO_SHARE_READ, HO_NONE, nullptr, &h);
    said += format("; a missing file is 0x%08X", unsigned(hr));
    if (SUCCEEDED(hr) && h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    try {
        const auto made = folder.CreateFolderAsync(L"megler-probe-folder", CreationCollisionOption::OpenIfExists).get();
        made.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
        said += "; a folder made and removed";
    } catch (const winrt::hresult_error& e) {
        said += format("; a folder cannot be made (0x%08X)", unsigned(e.code()));
    }
    // The folder's own handle, to read its entries as readdir() would.
    winrt::com_ptr<IStorageItemHandleAccess> self;
    if (folder.try_as(self)) {
        h = INVALID_HANDLE_VALUE;
        hr = self->Create(HAO_READ, HANDLE_SHARING_OPTIONS(HSO_SHARE_READ | HSO_SHARE_WRITE | HSO_SHARE_DELETE), HO_NONE, nullptr, &h);
        if (SUCCEEDED(hr) && h && h != INVALID_HANDLE_VALUE) {
            alignas(8) char buffer[4096];
            int entries = 0;
            bool listed = GetFileInformationByHandleEx(h, FileIdBothDirectoryRestartInfo, buffer, sizeof(buffer)) != 0;
            const DWORD listError = listed ? 0 : GetLastError();
            while (listed) {
                const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer);
                for (;;) {
                    entries++;
                    if (!info->NextEntryOffset) break;
                    info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<const char*>(info) + info->NextEntryOffset);
                }
                listed = entries < 200 && GetFileInformationByHandleEx(h, FileIdBothDirectoryInfo, buffer, sizeof(buffer)) != 0;
            }
            CloseHandle(h);
            said += listError ? format("; the folder's own handle cannot be read (error %lu)", listError) : format("; the folder's own handle lists %d entries", entries);
        } else {
            said += format("; the folder's own handle is refused (0x%08X)", unsigned(hr));
        }
    } else {
        said += "; the folder gives no handle to itself";
    }
    return said;
}

/// A library's folders as Windows names them (the capability must be there, or this throws).
std::vector<std::wstring> libraryFolders(winrt::Windows::Storage::KnownLibraryId id, std::string& said) {
    std::vector<std::wstring> paths;
    try {
        const auto library = winrt::Windows::Storage::StorageLibrary::GetLibraryAsync(id).get();
        const auto save = library.SaveFolder();
        if (save && !save.Path().empty()) paths.push_back(std::wstring(save.Path()));
        if (save) shellLog::write("storage: " + narrow(std::wstring(save.Path())) + " through the broker: " + brokered(save));
        for (const auto& folder : library.Folders()) {
            const std::wstring path(folder.Path());
            if (!path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end()) paths.push_back(path);
        }
        said = format("Windows names %u folder(s) for it", unsigned(paths.size()));
    } catch (const winrt::hresult_error& e) {
        said = format("Windows would not say where it is (0x%08X)", unsigned(e.code()));
    }
    return paths;
}

void run() {
    using winrt::Windows::Storage::KnownLibraryId;
    struct Library {
        const char* android;
        KnownLibraryId id;
        const wchar_t* usual; // where a phone keeps it, tried when Windows does not say
    };
    static const Library libraries[] = {
        {"Documents", KnownLibraryId::Documents, L"C:\\Data\\Users\\Public\\Documents"},
        {"Music", KnownLibraryId::Music, L"C:\\Data\\Users\\Public\\Music"},
        {"Pictures", KnownLibraryId::Pictures, L"C:\\Data\\Users\\Public\\Pictures"},
        {"Movies", KnownLibraryId::Videos, L"C:\\Data\\Users\\Public\\Videos"},
    };
    std::vector<Folder> found;
    for (const Library& l : libraries) {
        std::string said;
        std::vector<std::wstring> paths = libraryFolders(l.id, said);
        bool usualTried = false;
        for (const std::wstring& path : paths) {
            usualTried = usualTried || !_wcsicmp(path.c_str(), l.usual);
            found.push_back(tried(l.android, path, said));
        }
        if (!usualTried) found.push_back(tried(l.android, l.usual, said + "; its usual place on a phone"));
    }
    // No capability names the downloads folder; a phone has one all the same.
    found.push_back(tried("Download", L"C:\\Data\\Users\\Public\\Downloads", "no capability is for it; its usual place on a phone"));
    // The camera's pictures, which Android keeps apart from the others.
    found.push_back(tried("DCIM", L"C:\\Data\\Users\\Public\\Pictures\\Camera Roll", "the camera's folder in the pictures"));
    // A memory card.
    try {
        const auto cards = winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get();
        if (cards.Size() == 0) shellLog::write("storage: no memory card (Windows lists no removable device)");
        for (const auto& card : cards) found.push_back(tried("card", std::wstring(card.Path()), "a removable device, as Windows names it"));
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("storage: removable devices: Windows would not list them (0x%08X)", unsigned(e.code())));
        found.push_back(tried("card", L"D:", "a memory card's usual letter on a phone"));
    }
    for (const Folder& f : found) shellLog::write("storage: " + f.android + " at " + f.path + " -- " + f.note);
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gFolders = std::move(found);
    }
    gProbed.store(true);
}

} // namespace

void probe() {
    if (gStarted.exchange(true)) return;
    std::thread([] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            run();
        } catch (...) {
            shellLog::write("storage: the phone's folders could not be tried");
            gProbed.store(true);
        }
        winrt::uninit_apartment();
    }).detach();
}

std::vector<Folder> folders() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gFolders;
}

bool probed() { return gProbed.load(); }

} // namespace megler::app::storage
