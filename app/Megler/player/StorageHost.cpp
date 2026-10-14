#include "pch.h"

#include "player/StorageHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Search.h>

#include <algorithm>
#include <cwctype>
#include <iterator>

// The two interfaces of <windowsstoragecom.h> that hand over file handles,
// declared here by hand: the SDK's header brings in the shell's (shobjidl),
// which does not compile for an app container. Identifiers, methods and
// option values are the header's (Windows 10 SDK 10.0.15063 on).
namespace {

enum HANDLE_OPTIONS : DWORD { HO_NONE = 0 };
enum HANDLE_ACCESS_OPTIONS : DWORD { HAO_NONE = 0, HAO_READ_ATTRIBUTES = 0x80, HAO_READ = 0x120089, HAO_WRITE = 0x120116, HAO_DELETE = 0x10000 };
enum HANDLE_SHARING_OPTIONS : DWORD { HSO_SHARE_NONE = 0, HSO_SHARE_READ = 1, HSO_SHARE_WRITE = 2, HSO_SHARE_DELETE = 4 };
enum HANDLE_CREATION_OPTIONS : DWORD { HCO_CREATE_NEW = 1, HCO_CREATE_ALWAYS = 2, HCO_OPEN_EXISTING = 3, HCO_OPEN_ALWAYS = 4, HCO_TRUNCATE_EXISTING = 5 };

struct __declspec(uuid("5CA296B2-2C25-4D22-B785-B885C8201E6A")) __declspec(novtable) IStorageItemHandleAccess : ::IUnknown {
    virtual HRESULT __stdcall Create(HANDLE_ACCESS_OPTIONS access, HANDLE_SHARING_OPTIONS sharing, HANDLE_OPTIONS options, ::IUnknown* oplockBreakingHandler,
                                     HANDLE* handle) = 0;
};

struct __declspec(uuid("DF19938F-5462-48A0-BE65-D2A3271A08D6")) __declspec(novtable) IStorageFolderHandleAccess : ::IUnknown {
    virtual HRESULT __stdcall Create(LPCWSTR fileName, HANDLE_CREATION_OPTIONS creation, HANDLE_ACCESS_OPTIONS access, HANDLE_SHARING_OPTIONS sharing,
                                     HANDLE_OPTIONS options, ::IUnknown* oplockBreakingHandler, HANDLE* handle) = 0;
};

} // namespace

namespace megler::app {

using winrt::Windows::Storage::CreationCollisionOption;
using winrt::Windows::Storage::IStorageItem;
using winrt::Windows::Storage::NameCollisionOption;
using winrt::Windows::Storage::StorageDeleteOption;
using winrt::Windows::Storage::StorageFile;
using winrt::Windows::Storage::StorageFolder;
using winrt::Windows::Storage::StorageItemTypes;

namespace {

/// Android's files are shared for everything, as files are on Linux.
constexpr HANDLE_SHARING_OPTIONS shareAll = HANDLE_SHARING_OPTIONS(HSO_SHARE_READ | HSO_SHARE_WRITE | HSO_SHARE_DELETE);
constexpr HANDLE_ACCESS_OPTIONS toDelete = HANDLE_ACCESS_OPTIONS(HAO_DELETE | HAO_READ_ATTRIBUTES);
/// What the phone was seen to accept for a file that was then deleted through its handle.
constexpr HANDLE_ACCESS_OPTIONS toDeleteAsSeen = HANDLE_ACCESS_OPTIONS(HAO_READ | HAO_WRITE | HAO_DELETE);

/// How long a folder found on the way to a file is taken to be where it was.
constexpr std::chrono::seconds keptFor{30};
constexpr size_t keptAtMost = 256;
constexpr int workersAtMost = 4;

/// What megler.h calls an error of Windows': the broker's are Win32 errors as
/// HRESULTs (0x80070002 for a file that is not there, on the phone tried).
int errorOf(HRESULT hr) {
    if ((unsigned(hr) & 0xFFFF0000u) != 0x80070000u) return MEGLER_STORAGE_FAILED;
    switch (unsigned(hr) & 0xFFFFu) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return MEGLER_STORAGE_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
        return MEGLER_STORAGE_DENIED;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return MEGLER_STORAGE_EXISTS;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return MEGLER_STORAGE_BUSY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return MEGLER_STORAGE_FULL;
    case ERROR_DIR_NOT_EMPTY:
        return MEGLER_STORAGE_NOT_EMPTY;
    case ERROR_DIRECTORY:
        return MEGLER_STORAGE_NOT_A_FOLDER;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_PARAMETER:
        return MEGLER_STORAGE_INVALID;
    default:
        return MEGLER_STORAGE_FAILED;
    }
}

/// An answer of Windows' that the table above has no word for: the first
/// forty go to the log, which is all there is to learn the broker from.
std::atomic<int> gUnknown{0};
void unknown(const char* doing, const std::wstring& path, HRESULT hr) {
    if (errorOf(hr) != MEGLER_STORAGE_FAILED || gUnknown.fetch_add(1) >= 40) return;
    shellLog::write(format("storage: %s \"%s\": 0x%08X", doing, narrow(path).c_str(), unsigned(hr)));
}

/// What was asked and which way it went (StorageHost::summary).
struct Counts {
    std::atomic<unsigned> files{0}, folders{0}, whichever{0}, made{0}, removed{0}, renamed{0};
    std::atomic<unsigned> lookedUp{0}, known{0};
    // A way round a refusal: how often it was taken, and the code of the first refusal that led to it.
    struct Detour {
        std::atomic<unsigned> times{0};
        std::atomic<unsigned> first{0};
        void taken(HRESULT hr) {
            if (!times.fetch_add(1)) first.store(unsigned(hr));
        }
    };
    Detour folderByName;   // a folder's name asked of its parent as a file's: what the refusal is
    Detour attributesOnly; // a handle for the attributes alone refused, and one to read asked for
    Detour deleteAsSeen;   // a handle to delete through refused with the least rights, asked for with the ones a phone took
    Detour deleteByBroker; // not deleted through a handle at all, and the broker's DeleteAsync asked
    Detour folderNotMoved; // a folder not moved to another folder by renaming
    Detour heldByAnother;  // an open refused because something else had the file, and asked again a moment later
};
Counts gCounts;

bool usable(HRESULT hr, HANDLE h) { return SUCCEEDED(hr) && h && h != INVALID_HANDLE_VALUE; }

/// Asks the broker for a handle, and again for up to a second while the
/// answer is that something else has the file. On the phone (HP Elite x3) a
/// file written and closed in one of the libraries is at times held for a
/// moment by something of Windows' own that looks at new files there: of a
/// hundred steps of the check two were refused that way, a different two
/// each time, and an app would have been told the file was busy.
template <typename Ask>
HRESULT askPatiently(Ask&& ask) {
    HRESULT hr = ask();
    for (DWORD wait = 15; wait <= 480 && (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)); wait *= 2) {
        gCounts.heldByAnother.taken(hr);
        Sleep(wait);
        hr = ask();
    }
    return hr;
}

/// The same patience with a call of Windows.Storage's own, which says it by
/// throwing: a file to be renamed or removed may be held as one to be opened.
template <typename Call>
void callPatiently(Call&& call) {
    for (DWORD wait = 15;; wait *= 2) {
        try {
            call();
            return;
        } catch (const winrt::hresult_error& e) {
            const HRESULT hr = e.code();
            if (wait > 480 || (hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) && hr != HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))) throw;
            gCounts.heldByAnother.taken(hr);
        }
        Sleep(wait);
    }
}

/// A folder's or a file's handle to itself; null with `hr`.
template <typename Item>
HANDLE ownHandle(Item const& item, HANDLE_ACCESS_OPTIONS access, HRESULT& hr) {
    winrt::com_ptr<IStorageItemHandleAccess> self;
    if (!item.try_as(self)) {
        hr = E_NOINTERFACE;
        return nullptr;
    }
    HANDLE h = INVALID_HANDLE_VALUE;
    hr = askPatiently([&] { return self->Create(access, shareAll, HO_NONE, nullptr, &h); });
    return usable(hr, h) ? h : nullptr;
}

/// A handle to a file of a folder, by its name; null with `hr`.
HANDLE fileHandle(StorageFolder const& parent, const std::wstring& name, HANDLE_CREATION_OPTIONS creation, HANDLE_ACCESS_OPTIONS access, HRESULT& hr) {
    winrt::com_ptr<IStorageFolderHandleAccess> files;
    if (!parent.try_as(files)) {
        hr = E_NOINTERFACE;
        return nullptr;
    }
    HANDLE h = INVALID_HANDLE_VALUE;
    hr = askPatiently([&] { return files->Create(name.c_str(), creation, access, shareAll, HO_NONE, nullptr, &h); });
    return usable(hr, h) ? h : nullptr;
}

/// Whether a handle is open on a folder: one asked for by a file's name is
/// not expected to be, and nothing says it cannot.
bool onFolder(HANDLE h) {
    FILE_STANDARD_INFO info = {};
    return GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info)) && info.Directory;
}

/// Marks what a handle is open on to be deleted when it closes, as unlink()
/// and rmdir() would have it. A folder that holds something refuses
/// (ERROR_DIR_NOT_EMPTY), which is the whole point of going this way for
/// folders. Closes the handle.
bool deleteThrough(HANDLE h, DWORD& why) {
    FILE_DISPOSITION_INFO gone = {};
    gone.DeleteFile = TRUE;
    const bool ok = SetFileInformationByHandle(h, FileDispositionInfo, &gone, sizeof(gone)) != 0;
    why = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok;
}

/// Whether a folder can be listed through its own handle, as the layer will
/// list it, and with how many entries (a thousand are counted).
bool listable(StorageFolder const& folder, std::string& note) {
    HRESULT hr = S_OK;
    HANDLE h = ownHandle(folder, HAO_READ, hr);
    if (!h) {
        note = format("its handle is refused (0x%08X)", unsigned(hr));
        return false;
    }
    std::vector<char> buffer(8192 + sizeof(FILE_ID_BOTH_DIR_INFO));
    int entries = 0;
    bool more = GetFileInformationByHandleEx(h, FileIdBothDirectoryRestartInfo, buffer.data(), DWORD(buffer.size())) != 0;
    DWORD why = more ? 0 : GetLastError();
    // A folder with nothing at all in it says so in one of two ways.
    if (why == ERROR_NO_MORE_FILES || why == ERROR_FILE_NOT_FOUND) why = 0;
    while (more) {
        const auto* info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data());
        for (;;) {
            const size_t length = info->FileNameLength / sizeof(wchar_t);
            if (!(length == 1 && info->FileName[0] == L'.') && !(length == 2 && info->FileName[0] == L'.' && info->FileName[1] == L'.')) entries++;
            if (!info->NextEntryOffset) break;
            info = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<const char*>(info) + info->NextEntryOffset);
        }
        more = entries < 1000 && GetFileInformationByHandleEx(h, FileIdBothDirectoryInfo, buffer.data(), DWORD(buffer.size())) != 0;
    }
    CloseHandle(h);
    if (why) {
        note = format("its entries cannot be read (error %lu)", why);
        return false;
    }
    note = format("%d%s entries", entries, entries >= 1000 ? " or more" : "");
    return true;
}

std::wstring parentOf(const std::wstring& path) {
    const size_t slash = path.rfind(L'/');
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring nameOf(const std::wstring& path) {
    const size_t slash = path.rfind(L'/');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring lowered(std::wstring text) {
    for (wchar_t& c : text) c = wchar_t(towlower(c));
    return text;
}

bool sameName(const std::wstring& a, const std::wstring& b) { return lowered(a) == lowered(b); }

std::string loweredAscii(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return text;
}

/// Whether a name can be a folder of /sdcard: one part of a path, and not
/// the folder the apps' own parts are in. (The layer leaves out a link that
/// fails this; here it is what the Settings screen is told beforehand.)
bool nameAllowed(const std::string& name) {
    if (name.empty() || name.size() > 100 || name == "." || name == ".." || loweredAscii(name) == "android") return false;
    for (const char c : name)
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) return false;
    return name.front() != ' ' && name.back() != ' ' && name.back() != '.';
}

/// How a file is to be opened. Never HCO_TRUNCATE_EXISTING: Windows wants
/// GENERIC_WRITE asked for with that, the one bit, and what can be asked for
/// here are the rights it stands for (HAO_WRITE), so it is refused as a wrong
/// parameter (seen on a PC). A file that is to be emptied and not made is
/// opened as it is and cut to nothing through its handle (emptiedOf).
HANDLE_CREATION_OPTIONS creationOf(int flags) {
    const bool create = (flags & MEGLER_STORAGE_CREATE) != 0, truncate = (flags & MEGLER_STORAGE_TRUNCATE) != 0;
    if (create && (flags & MEGLER_STORAGE_EXCLUSIVE)) return HCO_CREATE_NEW;
    if (create && truncate) return HCO_CREATE_ALWAYS;
    if (create) return HCO_OPEN_ALWAYS;
    return HCO_OPEN_EXISTING;
}

bool emptiedOf(int flags) { return (flags & MEGLER_STORAGE_TRUNCATE) && !(flags & MEGLER_STORAGE_CREATE); }

HANDLE_ACCESS_OPTIONS accessOf(int flags) {
    DWORD access = 0;
    if (flags & MEGLER_STORAGE_READ) access |= HAO_READ;
    // (Emptying a file is writing to it, whatever else the app asked for.)
    if (flags & (MEGLER_STORAGE_WRITE | MEGLER_STORAGE_TRUNCATE | MEGLER_STORAGE_APPEND)) access |= HAO_WRITE;
    return HANDLE_ACCESS_OPTIONS(access ? access : DWORD(HAO_READ));
}

/// Runs something that calls Windows.Storage and turns what it throws into
/// one of megler.h's errors.
template <typename Work>
void guarded(const char* doing, const std::wstring& path, int& error, Work&& work) {
    try {
        work();
    } catch (const winrt::hresult_error& e) {
        error = errorOf(e.code());
        unknown(doing, path, e.code());
    } catch (...) {
        error = MEGLER_STORAGE_FAILED;
    }
}

} // namespace

StorageHost& StorageHost::shared() {
    // Never destroyed: its workers are never joined, and the layer may call until the process ends.
    static StorageHost* const host = new StorageHost();
    return *host;
}

// MARK: The links

void StorageHost::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return;
        started_ = true;
    }
    std::thread([this] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
        }
        try {
            search();
        } catch (...) {
            shellLog::write("storage: the search for the phone's folders was cut short");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            searched_ = true;
        }
        searchDone_.notify_all();
    }).detach();
}

bool StorageHost::searched() {
    std::lock_guard<std::mutex> lock(mutex_);
    return searched_;
}

StorageHost::Root StorageHost::found(const std::string& name, const std::string& what, StorageFolder const& folder, const std::string& token) {
    Root root;
    root.link.name = name;
    root.link.what = what;
    root.link.token = token;
    root.folder = folder;
    try {
        root.link.path = narrow(std::wstring(folder.Path()));
    } catch (...) {
    }
    try {
        root.link.answers = listable(folder, root.link.note);
    } catch (const winrt::hresult_error& e) {
        root.link.note = format("it cannot be asked (0x%08X)", unsigned(e.code()));
    }
    shellLog::write("storage: /sdcard/" + name + " is " + (root.link.path.empty() ? std::string("a folder Windows gives no path for") : root.link.path) + ", " + what + ": " +
                    (root.link.answers ? "it answers, " + root.link.note : "it does NOT answer (" + root.link.note + ") and is left out"));
    return root;
}

void StorageHost::startWith(std::vector<std::pair<std::string, StorageFolder>> folders) {
    Roots roots;
    for (const auto& folder : folders) roots.push_back(found(folder.first, "a folder of the tests' own", folder.second, std::string()));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = searched_ = true;
        found_ = std::move(roots);
    }
    searchDone_.notify_all();
}

void StorageHost::search() {
    using winrt::Windows::Storage::KnownLibraryId;
    using winrt::Windows::Storage::StorageLibrary;
    Roots roots;
    auto take = [&roots](const std::string& name, const std::string& what, StorageFolder const& folder, const std::string& token) {
        roots.push_back(found(name, what, folder, token));
    };
    struct Library {
        const char* name;
        KnownLibraryId id;
        const char* what;
    };
    static const Library libraries[] = {
        {"Music", KnownLibraryId::Music, "the music library"},
        {"Pictures", KnownLibraryId::Pictures, "the pictures library"},
        {"Movies", KnownLibraryId::Videos, "the videos library"},
    };
    StorageFolder pictures{nullptr};
    for (const Library& library : libraries) {
        try {
            // Where the library saves, which is one folder; a library can be made of several (the phone's and a memory card's).
            const StorageFolder save = StorageLibrary::GetLibraryAsync(library.id).get().SaveFolder();
            if (!save) {
                shellLog::write(format("storage: no link for /sdcard/%s: %s has no folder to save to", library.name, library.what));
                continue;
            }
            take(library.name, library.what, save, std::string());
            if (library.id == KnownLibraryId::Pictures) pictures = save;
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("storage: no link for /sdcard/%s: Windows would not say where %s is (0x%08X)", library.name, library.what, unsigned(e.code())));
        }
    }
    // The camera's pictures, which Android keeps apart from the others.
    try {
        StorageFolder camera{nullptr};
        if (pictures) {
            const IStorageItem item = pictures.TryGetItemAsync(L"Camera Roll").get();
            if (item && item.IsOfType(StorageItemTypes::Folder)) camera = item.as<StorageFolder>();
        }
        if (!camera) camera = winrt::Windows::Storage::KnownFolders::CameraRoll();
        if (camera) take("DCIM", "the camera's pictures", camera, std::string());
        else shellLog::write("storage: no link for /sdcard/DCIM: the pictures have no Camera Roll");
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("storage: no link for /sdcard/DCIM: the pictures' Camera Roll is not to be had (0x%08X)", unsigned(e.code())));
    }
    // The folders the user added, which Windows has kept the way into.
    try {
        const auto list = winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList();
        for (const auto& entry : list.Entries()) {
            const std::string token = narrow(std::wstring(entry.Token)), name = narrow(std::wstring(entry.Metadata));
            try {
                take(name, "a folder you added", list.GetFolderAsync(entry.Token).get(), token);
            } catch (const winrt::hresult_error& e) {
                // Gone, renamed, or on a card that is not in the phone: kept in the list, so that Settings can show it and forget it.
                Root root;
                root.link.name = name;
                root.link.what = "a folder you added";
                root.link.token = token;
                root.link.note = format("Windows no longer finds it (0x%08X)", unsigned(e.code()));
                shellLog::write("storage: /sdcard/" + name + ", a folder you added: " + root.link.note + ", and it is left out");
                roots.push_back(std::move(root));
            }
        }
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("storage: the folders you added cannot be read back (0x%08X)", unsigned(e.code())));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // (A folder added in Settings while the search ran is in found_ already, and may have been read back here as well.)
    for (Root& root : roots) {
        const bool twice = !root.link.token.empty() &&
                           std::any_of(found_.begin(), found_.end(), [&root](const Root& other) { return other.link.token == root.link.token; });
        if (!twice) found_.push_back(std::move(root));
    }
}

std::vector<StorageHost::Link> StorageHost::links() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Link> all;
    for (const Root& root : found_) all.push_back(root.link);
    return all;
}

bool StorageHost::nameFree(const std::string& name) {
    if (!nameAllowed(name)) return false;
    const std::string wanted = loweredAscii(name);
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Root& root : found_)
        if (loweredAscii(root.link.name) == wanted) return false;
    return true;
}

std::vector<std::string> StorageHost::namesFor(const std::string& folderName) {
    std::vector<std::string> names;
    for (const char* standard : {"Documents", "Download"})
        if (nameFree(standard)) names.push_back(standard);
    // The folder's own name, made into one that can be a link's and that no link has.
    std::string own;
    for (const char c : folderName) own += (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) ? '_' : c;
    while (!own.empty() && (own.back() == ' ' || own.back() == '.')) own.pop_back();
    while (!own.empty() && own.front() == ' ') own.erase(own.begin());
    if (own.empty() || !nameAllowed(own)) own = "Folder";
    std::string candidate = own;
    for (int n = 2; !nameFree(candidate) && n < 100; n++) candidate = own + " " + std::to_string(n);
    const bool offered = std::any_of(names.begin(), names.end(), [&](const std::string& n) { return loweredAscii(n) == loweredAscii(candidate); });
    if (!offered && nameFree(candidate)) names.push_back(candidate);
    return names;
}

std::string StorageHost::add(StorageFolder const& folder, const std::string& name) {
    if (!folder) return "No folder was chosen.";
    if (!nameAllowed(name)) return "\"" + name + "\" cannot be a folder's name in Android's storage.";
    if (!nameFree(name)) return "Android's storage has a folder linked as \"" + name + "\" already.";
    std::string token;
    try {
        // The name is kept with it: FutureAccessList has a line of text for each thing it remembers.
        token = narrow(std::wstring(winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList().Add(folder, hs(name))));
    } catch (const winrt::hresult_error& e) {
        return format("Windows would not keep the folder for Megler (0x%08X).", unsigned(e.code()));
    }
    // Whether it answers is asked on a worker: this is the UI thread, which waits for no call to the broker.
    Root root;
    run([&] { root = found(name, "a folder you added", folder, token); });
    std::lock_guard<std::mutex> lock(mutex_);
    found_.push_back(std::move(root));
    return std::string();
}

void StorageHost::remove(const std::string& token) {
    if (token.empty()) return;
    try {
        const auto list = winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList();
        if (list.ContainsItem(hs(token))) list.Remove(hs(token));
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("storage: Windows would not forget the folder (0x%08X)", unsigned(e.code())));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = found_.begin(); it != found_.end(); ++it) {
        if (it->link.token != token) continue;
        shellLog::write("storage: /sdcard/" + it->link.name + " is no longer linked");
        found_.erase(it);
        break;
    }
}

megler_storage_host StorageHost::callbacks() {
    megler_storage_host host = {};
    // The flag "no-links" (a file of that name in LocalState\flags): the app gets the layer's own storage and nothing of the
    // phone's, for a phone on which the links get in an app's way.
    const bool off = files::exists(platform::localStateDirectory() + L"\\flags\\no-links");
    if (off) shellLog::write("flag: no-links is on: Android's storage has none of the phone's folders in it");
    start();
    std::string names;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!searchDone_.wait_for(lock, std::chrono::seconds(20), [this] { return searched_; }))
            shellLog::write("storage: the search for the phone's folders is not over after 20 s: the app gets the links found so far");
        auto roots = std::make_shared<Roots>();
        for (const Root& root : found_)
            if (!off && root.link.answers && root.folder && roots->size() < size_t(MEGLER_STORAGE_MAX_LINKS)) roots->push_back(root);
        givenNames_.clear();
        givenLinks_.clear();
        for (const Root& root : *roots) givenNames_.push_back(root.link.name);
        for (size_t i = 0; i < roots->size(); i++) {
            megler_storage_link link = {};
            link.name = givenNames_[i].c_str();
            link.writable = (*roots)[i].writable;
            givenLinks_.push_back(link);
            names += (i ? ", " : "") + givenNames_[i];
        }
        given_ = roots;
        if (givenLinks_.empty()) return host;
        host.links = givenLinks_.data();
        host.link_count = int(givenLinks_.size());
    }
    {
        // The numbers are new ones: what was kept under the old ones is some other folder's.
        std::lock_guard<std::mutex> lock(cacheMutex_);
        cache_.clear();
    }
    host.user = this;
    host.open = &openThunk;
    host.make_folder = &makeFolderThunk;
    host.remove = &removeThunk;
    host.rename = &renameThunk;
    shellLog::write(format("storage: %d of the phone's folders are in Android's storage: %s", host.link_count, names.c_str()));
    return host;
}

std::string StorageHost::check(const std::function<void(const std::string&)>& line) {
    const megler_storage_host host = callbacks();
    int checks = 0, failed = 0;
    for (const char* expected : {"Music", "Pictures", "Movies"}) {
        bool there = false;
        for (int i = 0; i < host.link_count; i++) there = there || !strcmp(host.links[i].name, expected);
        checks++;
        if (!there) failed++;
        line(std::string(there ? "  ok   /sdcard/" : "  FAIL /sdcard/") + expected +
             (there ? " is one of the phone's folders" : " is NOT one of the phone's folders: the storage's lines at the start of the log say why"));
    }
    using Line = std::function<void(const std::string&)>;
    failed += megler_storage_check(&host, [](const char* text, void* user) { (*static_cast<const Line*>(user))(text); }, const_cast<Line*>(&line), &checks);
    line("       " + summary());
    const std::string done = format("DONE storage: %d checks, %d failed", checks, failed);
    line(done);
    return done;
}

std::string StorageHost::summary() {
    const Counts& c = gCounts;
    std::string text = format("%u files opened, %u folders to list, %u asked what they are; %u folders made, %u things removed, %u renamed; %u folders looked up on the "
                              "way and %u known from before",
                              c.files.load(), c.folders.load(), c.whichever.load(), c.made.load(), c.removed.load(), c.renamed.load(), c.lookedUp.load(), c.known.load());
    const struct {
        const Counts::Detour& detour;
        const char* what;
    } detours[] = {
        {c.folderByName, "a folder asked for by name as a file was refused"},
        {c.attributesOnly, "a handle for the attributes alone was refused and more asked for"},
        {c.deleteAsSeen, "a handle to delete through was refused with the least rights and asked for with more"},
        {c.deleteByBroker, "not deleted through a handle, and Windows.Storage's own deleting asked for"},
        {c.folderNotMoved, "a folder not moved to another folder by renaming (the app was told EXDEV)"},
        {c.heldByAnother, "an open refused because something else had the file, and asked again a moment later"},
    };
    for (const auto& d : detours)
        if (d.detour.times.load()) text += format("; %s %u times, first with 0x%08X", d.what, d.detour.times.load(), d.detour.first.load());
    return text;
}

std::shared_ptr<const StorageHost::Roots> StorageHost::given() {
    std::lock_guard<std::mutex> lock(mutex_);
    return given_;
}

// MARK: The workers

void StorageHost::run(const std::function<void()>& work) {
    Job job;
    job.work = &work;
    std::unique_lock<std::mutex> lock(poolMutex_);
    queue_.push_back(&job);
    // One worker more while every one there is has something to do.
    if (int(queue_.size()) > idle_ && workers_ < workersAtMost) {
        workers_++;
        std::thread([this] { workerLoop(); }).detach();
    }
    wake_.notify_one();
    done_.wait(lock, [&job] { return job.finished; });
}

void StorageHost::workerLoop() {
    // Windows.Storage's calls are made and waited for here, which a thread in the multithreaded apartment may do.
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    std::unique_lock<std::mutex> lock(poolMutex_);
    for (;;) {
        idle_++;
        wake_.wait(lock, [this] { return !queue_.empty(); });
        idle_--;
        Job* const job = queue_.front();
        queue_.pop_front();
        lock.unlock();
        try {
            (*job->work)();
        } catch (...) {
        }
        lock.lock();
        job->finished = true;
        done_.notify_all();
    }
}

// MARK: Folders on the way

StorageFolder StorageHost::cached(int root, const std::wstring& path) {
    const std::wstring key = std::to_wstring(root) + L"|" + lowered(path);
    std::lock_guard<std::mutex> lock(cacheMutex_);
    const auto it = cache_.find(key);
    if (it == cache_.end()) return nullptr;
    if (std::chrono::steady_clock::now() - it->second.at > keptFor) {
        cache_.erase(it);
        return nullptr;
    }
    return it->second.folder;
}

void StorageHost::keep(int root, const std::wstring& path, StorageFolder const& folder) {
    const std::wstring key = std::to_wstring(root) + L"|" + lowered(path);
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (cache_.size() >= keptAtMost) cache_.clear();
    cache_[key] = Kept{folder, std::chrono::steady_clock::now()};
}

void StorageHost::forget(int root) {
    const std::wstring prefix = std::to_wstring(root) + L"|";
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for (auto it = cache_.begin(); it != cache_.end();) it = it->first.compare(0, prefix.size(), prefix) == 0 ? cache_.erase(it) : std::next(it);
}

StorageFolder StorageHost::folderAt(const Roots& roots, int root, const std::wstring& path, int& error) {
    if (path.empty()) return roots[size_t(root)].folder;
    if (StorageFolder known = cached(root, path)) {
        gCounts.known++;
        return known;
    }
    const StorageFolder parent = folderAt(roots, root, parentOf(path), error);
    if (!parent) return nullptr;
    gCounts.lookedUp++;
    const IStorageItem item = parent.TryGetItemAsync(nameOf(path)).get();
    if (!item) {
        error = MEGLER_STORAGE_NOT_FOUND;
        return nullptr;
    }
    if (!item.IsOfType(StorageItemTypes::Folder)) {
        error = MEGLER_STORAGE_NOT_A_FOLDER;
        return nullptr;
    }
    const StorageFolder folder = item.as<StorageFolder>();
    keep(root, path, folder);
    return folder;
}

// MARK: The four calls

HANDLE StorageHost::openOnWorker(const Roots& roots, int root, const std::wstring& path, int flags, int kind, int& error) {
    HRESULT hr = S_OK;
    (kind == MEGLER_STORAGE_FILE ? gCounts.files : kind == MEGLER_STORAGE_FOLDER ? gCounts.folders : gCounts.whichever)++;
    if (kind == MEGLER_STORAGE_FOLDER || path.empty()) {
        if (kind == MEGLER_STORAGE_FILE) {
            error = MEGLER_STORAGE_IS_A_FOLDER;
            return nullptr;
        }
        const StorageFolder folder = folderAt(roots, root, path, error);
        if (!folder) return nullptr;
        const HANDLE h = ownHandle(folder, HAO_READ, hr);
        if (!h) {
            error = errorOf(hr);
            unknown("the handle of the folder", path, hr);
        }
        return h;
    }
    const std::wstring name = nameOf(path);
    if (kind == MEGLER_STORAGE_ANY) {
        // A folder met before is one still, as far as anyone knows.
        if (StorageFolder known = cached(root, path)) {
            if (const HANDLE h = ownHandle(known, HAO_READ, hr)) return h;
        }
    }
    const StorageFolder parent = folderAt(roots, root, parentOf(path), error);
    if (!parent) return nullptr;
    HANDLE h = nullptr;
    if (kind == MEGLER_STORAGE_FILE) {
        h = fileHandle(parent, name, creationOf(flags), accessOf(flags), hr);
        if (h && onFolder(h)) {
            CloseHandle(h);
            h = nullptr;
            hr = E_ACCESSDENIED;
        }
        if (h && emptiedOf(flags)) {
            FILE_END_OF_FILE_INFO end = {};
            if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &end, sizeof(end))) {
                hr = HRESULT_FROM_WIN32(GetLastError());
                CloseHandle(h);
                error = errorOf(hr);
                unknown("emptying", path, hr);
                return nullptr;
            }
        }
    } else {
        // Whichever it is, for its size and times. A file is the usual case, and its folder hands a handle over in one call.
        h = fileHandle(parent, name, HCO_OPEN_EXISTING, HAO_READ_ATTRIBUTES, hr);
        // (Should the broker want more asked for than the attributes.)
        if (!h && hr == E_INVALIDARG) {
            gCounts.attributesOnly.taken(hr);
            h = fileHandle(parent, name, HCO_OPEN_EXISTING, HAO_READ, hr);
        }
    }
    if (h) return h;
    // Why not. What the broker says of a folder asked for as a file is not known (not found? denied?), so the name is looked at:
    // nothing there, a folder, or a file that Windows will not hand over.
    const HRESULT refused = hr;
    const IStorageItem item = parent.TryGetItemAsync(name).get();
    if (!item) {
        error = errorOf(refused) == MEGLER_STORAGE_NOT_FOUND || !(flags & MEGLER_STORAGE_CREATE) ? MEGLER_STORAGE_NOT_FOUND : errorOf(refused);
        if (error != MEGLER_STORAGE_NOT_FOUND) unknown("making the file", path, refused);
        return nullptr;
    }
    if (item.IsOfType(StorageItemTypes::Folder)) {
        const StorageFolder folder = item.as<StorageFolder>();
        gCounts.folderByName.taken(refused);
        keep(root, path, folder);
        if (kind == MEGLER_STORAGE_FILE) {
            // (To an open that must make the file, what is in the way is in the way, folder or not.)
            const bool mustMake = (flags & MEGLER_STORAGE_CREATE) && (flags & MEGLER_STORAGE_EXCLUSIVE);
            error = mustMake ? MEGLER_STORAGE_EXISTS : MEGLER_STORAGE_IS_A_FOLDER;
            return nullptr;
        }
        h = ownHandle(folder, HAO_READ, hr);
    } else if (kind == MEGLER_STORAGE_ANY) {
        // A file whose folder would not hand it over: asked of the file itself.
        gCounts.attributesOnly.taken(refused);
        h = ownHandle(item, HAO_READ_ATTRIBUTES, hr);
        if (!h) h = ownHandle(item, HAO_READ, hr);
    } else {
        hr = refused;
    }
    if (!h) {
        error = errorOf(hr);
        unknown("the handle of", path, hr);
    }
    return h;
}

bool StorageHost::makeFolderOnWorker(const Roots& roots, int root, const std::wstring& path, int& error) {
    const StorageFolder parent = folderAt(roots, root, parentOf(path), error);
    if (!parent) return false;
    gCounts.made++;
    // (A file of that name is in the way as a folder would be: Windows says "exists" for both.)
    const StorageFolder made = parent.CreateFolderAsync(nameOf(path), CreationCollisionOption::FailIfExists).get();
    keep(root, path, made);
    return true;
}

bool StorageHost::removeEmptyFolder(StorageFolder const& folder, int& error) {
    // Through a handle, where the file system itself refuses a folder with something in it.
    HRESULT hr = S_OK;
    HANDLE h = ownHandle(folder, toDelete, hr);
    if (!h) {
        gCounts.deleteAsSeen.taken(hr);
        h = ownHandle(folder, HANDLE_ACCESS_OPTIONS(HAO_READ | HAO_DELETE), hr);
    }
    if (h) {
        DWORD why = 0;
        if (deleteThrough(h, why)) return true;
        if (why == ERROR_DIR_NOT_EMPTY) {
            error = MEGLER_STORAGE_NOT_EMPTY;
            return false;
        }
        hr = HRESULT_FROM_WIN32(why);
    }
    // The broker instead. Its DeleteAsync takes what a folder holds with it, and rmdir() must not: only a folder seen empty
    // a moment before is given to it.
    gCounts.deleteByBroker.taken(hr);
    if (folder.GetItemsAsync(0, 1).get().Size() != 0) {
        error = MEGLER_STORAGE_NOT_EMPTY;
        return false;
    }
    folder.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
    return true;
}

bool StorageHost::removeOnWorker(const Roots& roots, int root, const std::wstring& path, bool folder, int& error) {
    gCounts.removed++;
    if (folder) {
        const StorageFolder gone = folderAt(roots, root, path, error);
        if (!gone) return false;
        const bool ok = removeEmptyFolder(gone, error);
        // Whatever was kept below it, or as it, is no longer so.
        if (ok) forget(root);
        return ok;
    }
    const StorageFolder parent = folderAt(roots, root, parentOf(path), error);
    if (!parent) return false;
    const std::wstring name = nameOf(path);
    HRESULT hr = S_OK;
    HANDLE h = fileHandle(parent, name, HCO_OPEN_EXISTING, toDelete, hr);
    const HRESULT first = hr;
    if (!h && errorOf(hr) != MEGLER_STORAGE_NOT_FOUND) h = fileHandle(parent, name, HCO_OPEN_EXISTING, toDeleteAsSeen, hr);
    if (h && onFolder(h)) {
        // unlink() does not remove a folder, empty or not.
        CloseHandle(h);
        error = MEGLER_STORAGE_IS_A_FOLDER;
        return false;
    }
    if (h) {
        DWORD why = 0;
        // (Counted here and not above: a folder's name is refused both ways, and that is no detour.)
        if (FAILED(first)) gCounts.deleteAsSeen.taken(first);
        if (deleteThrough(h, why)) return true;
        hr = HRESULT_FROM_WIN32(why);
    }
    // Not through a handle, then: what is there, and the broker's own removing of it.
    const IStorageItem item = parent.TryGetItemAsync(name).get();
    if (!item) {
        error = MEGLER_STORAGE_NOT_FOUND;
        return false;
    }
    if (item.IsOfType(StorageItemTypes::Folder)) {
        error = MEGLER_STORAGE_IS_A_FOLDER;
        return false;
    }
    gCounts.deleteByBroker.taken(hr);
    callPatiently([&] { item.DeleteAsync(StorageDeleteOption::PermanentDelete).get(); });
    return true;
}

bool StorageHost::renameOnWorker(const Roots& roots, int fromRoot, const std::wstring& from, int toRoot, const std::wstring& to, int& error) {
    gCounts.renamed++;
    const StorageFolder fromParent = folderAt(roots, fromRoot, parentOf(from), error);
    if (!fromParent) return false;
    const StorageFolder toParent = folderAt(roots, toRoot, parentOf(to), error);
    if (!toParent) return false;
    const std::wstring fromName = nameOf(from), toName = nameOf(to);
    const IStorageItem item = fromParent.TryGetItemAsync(fromName).get();
    if (!item) {
        error = MEGLER_STORAGE_NOT_FOUND;
        return false;
    }
    const bool isFolder = item.IsOfType(StorageItemTypes::Folder);
    const bool sameParent = fromRoot == toRoot && sameName(parentOf(from), parentOf(to));
    if (sameParent && fromName == toName) return true;
    // The same thing under a name that differs in the case of its letters only: Windows finds it under both.
    const bool itself = sameParent && sameName(fromName, toName);
    if (!itself) {
        // What has the new name: rename(2) puts a file in a file's place and a folder in an empty folder's, and nothing else.
        const IStorageItem target = toParent.TryGetItemAsync(toName).get();
        if (target) {
            const bool targetIsFolder = target.IsOfType(StorageItemTypes::Folder);
            if (isFolder != targetIsFolder) {
                error = isFolder ? MEGLER_STORAGE_NOT_A_FOLDER : MEGLER_STORAGE_IS_A_FOLDER;
                return false;
            }
            if (isFolder && !removeEmptyFolder(target.as<StorageFolder>(), error)) return false;
        }
    }
    if (itself) {
        // By way of a third name: whether "replace what is there" would take the thing for its own obstacle is not known.
        const std::wstring between = L".megler-rename-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(GetCurrentThreadId());
        callPatiently([&] { item.RenameAsync(between, NameCollisionOption::FailIfExists).get(); });
        try {
            item.RenameAsync(toName, NameCollisionOption::FailIfExists).get();
        } catch (...) {
            try {
                item.RenameAsync(fromName, NameCollisionOption::FailIfExists).get();
            } catch (...) {
            }
            throw;
        }
    } else if (sameParent) {
        callPatiently([&] { item.RenameAsync(toName, NameCollisionOption::ReplaceExisting).get(); });
    } else if (!isFolder) {
        callPatiently([&] { item.as<StorageFile>().MoveAsync(toParent, toName, NameCollisionOption::ReplaceExisting).get(); });
    } else {
        // Windows.Storage moves a file to another folder and has nothing that moves a folder. The file system has: a rename
        // through the folder's handle, to the new place by its path. (By its path: SetFileInformationByHandle takes no handle
        // for the place -- one given as the name's root is refused as a wrong parameter, seen on a PC.) So Windows opens the
        // new place by name on the app's behalf, and an app container is not let open the user's folders by name; on a phone
        // this is expected to be refused. Then the app is told what it would be told between two disks, and copies.
        HRESULT hr = E_FAIL;
        std::wstring target;
        try {
            target = std::wstring(toParent.Path());
        } catch (...) {
        }
        bool moved = false;
        if (!target.empty()) {
            target += (target.back() == L'\\' ? L"" : L"\\") + toName;
            if (const HANDLE source = ownHandle(item, toDelete, hr)) {
                std::vector<char> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
                auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
                info->ReplaceIfExists = FALSE;
                info->RootDirectory = nullptr;
                info->FileNameLength = DWORD(target.size() * sizeof(wchar_t));
                memcpy(info->FileName, target.c_str(), target.size() * sizeof(wchar_t));
                moved = SetFileInformationByHandle(source, FileRenameInfo, info, DWORD(buffer.size())) != 0;
                if (!moved) hr = HRESULT_FROM_WIN32(GetLastError());
                CloseHandle(source);
            }
        }
        if (!moved) {
            gCounts.folderNotMoved.taken(hr);
            error = MEGLER_STORAGE_OTHER_DEVICE;
            return false;
        }
    }
    if (isFolder) {
        forget(fromRoot);
        forget(toRoot);
    }
    return true;
}

void* StorageHost::openThunk(void* user, int link, const char* path, int flags, int kind, int* error) {
    StorageHost& self = *static_cast<StorageHost*>(user);
    const std::shared_ptr<const Roots> roots = self.given();
    if (!roots || link < 0 || size_t(link) >= roots->size()) {
        *error = MEGLER_STORAGE_NOT_FOUND;
        return nullptr;
    }
    const std::wstring wide = widen(path);
    HANDLE handle = nullptr;
    int why = MEGLER_STORAGE_FAILED;
    self.run([&] { guarded("opening", wide, why, [&] { handle = self.openOnWorker(*roots, link, wide, flags, kind, why); }); });
    *error = handle ? MEGLER_STORAGE_OK : why;
    return handle;
}

bool StorageHost::makeFolderThunk(void* user, int link, const char* path, int* error) {
    StorageHost& self = *static_cast<StorageHost*>(user);
    const std::shared_ptr<const Roots> roots = self.given();
    if (!roots || link < 0 || size_t(link) >= roots->size()) {
        *error = MEGLER_STORAGE_NOT_FOUND;
        return false;
    }
    const std::wstring wide = widen(path);
    bool ok = false;
    int why = MEGLER_STORAGE_FAILED;
    self.run([&] { guarded("making the folder", wide, why, [&] { ok = self.makeFolderOnWorker(*roots, link, wide, why); }); });
    *error = ok ? MEGLER_STORAGE_OK : why;
    return ok;
}

bool StorageHost::removeThunk(void* user, int link, const char* path, bool folder, int* error) {
    StorageHost& self = *static_cast<StorageHost*>(user);
    const std::shared_ptr<const Roots> roots = self.given();
    if (!roots || link < 0 || size_t(link) >= roots->size()) {
        *error = MEGLER_STORAGE_NOT_FOUND;
        return false;
    }
    const std::wstring wide = widen(path);
    bool ok = false;
    int why = MEGLER_STORAGE_FAILED;
    self.run([&] { guarded("removing", wide, why, [&] { ok = self.removeOnWorker(*roots, link, wide, folder, why); }); });
    *error = ok ? MEGLER_STORAGE_OK : why;
    return ok;
}

bool StorageHost::renameThunk(void* user, int fromLink, const char* from, int toLink, const char* to, int* error) {
    StorageHost& self = *static_cast<StorageHost*>(user);
    const std::shared_ptr<const Roots> roots = self.given();
    if (!roots || fromLink < 0 || size_t(fromLink) >= roots->size() || toLink < 0 || size_t(toLink) >= roots->size()) {
        *error = MEGLER_STORAGE_NOT_FOUND;
        return false;
    }
    const std::wstring wideFrom = widen(from), wideTo = widen(to);
    bool ok = false;
    int why = MEGLER_STORAGE_FAILED;
    self.run([&] { guarded("renaming", wideFrom, why, [&] { ok = self.renameOnWorker(*roots, fromLink, wideFrom, toLink, wideTo, why); }); });
    *error = ok ? MEGLER_STORAGE_OK : why;
    return ok;
}

} // namespace megler::app
