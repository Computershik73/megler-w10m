#include "pch.h"

#include "shell/Storage.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

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

/// A library's folders as Windows names them (the capability must be there, or this throws).
std::vector<std::wstring> libraryFolders(winrt::Windows::Storage::KnownLibraryId id, std::string& said) {
    std::vector<std::wstring> paths;
    try {
        const auto library = winrt::Windows::Storage::StorageLibrary::GetLibraryAsync(id).get();
        const auto save = library.SaveFolder();
        if (save && !save.Path().empty()) paths.push_back(std::wstring(save.Path()));
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
