// The phone's own folders in an Android app's storage: megler_storage_host
// (core/include/megler.h) over Windows.Storage.
//
// An app's files live under LocalState, where the layer opens them by path.
// The user's music, pictures and videos do not, and an app container may not
// open those by path at all: on the phone tried (HP Elite x3, 10.0.15254)
// FindFirstFileExW and CreateFile2 are refused in every one of them (error
// 5) with every library capability declared. What Windows gives an app there
// is objects of its broker -- and, since 1703, real handles out of those
// objects (IStorageFolderHandleAccess, IStorageItemHandleAccess). The same
// phone was seen to hand over a file handle that writes, seeks, reads back,
// says its size and times and deletes through itself, and a folder handle
// that lists. So the layer asks here for a handle and goes on with it as with
// any file's; making, removing and renaming go through the broker's objects.
//
// The links:
//   Music, Pictures, Movies   the three libraries' folders to save to (the
//                             capabilities musicLibrary, picturesLibrary,
//                             videosLibrary)
//   DCIM                      the pictures' "Camera Roll", where there is one
//   any name                  a folder the user chose in Settings (the folder
//                             picker), which Windows remembers for the app
//                             from then on (FutureAccessList). The documents
//                             library, the downloads and a memory card have
//                             no other way in: the first wants file types
//                             declared and was refused on that phone, the
//                             second has no capability, the third was not in
//                             it.
// A link that does not answer when Megler starts is left out, and the folder
// of the layer's own stands where it would have been; the log says which.
//
// The calls come on the app's own threads, several at once, and those have
// stacks of the app's choosing and no COM apartment. Each is done on one of
// a few threads of this class's own, which live in the multithreaded
// apartment, while the caller waits: the same arrangement as MediaHost's.
// Looking a path up costs a call to the broker for every folder on the way,
// so the folders found are kept for a while.
//
// And Windows' own pictures of the files there (thumbnail): asked for by the
// layer without waiting, made on threads of their own -- a video's frame may
// take Windows a moment -- and answered through the layer's callback.
//
// One StorageHost serves the process: shared().
#pragma once

#include "megler.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace megler::app {

class StorageHost {
public:
    static StorageHost& shared();
    StorageHost(const StorageHost&) = delete;
    StorageHost& operator=(const StorageHost&) = delete;

    /// Looks for the links on a thread of its own and returns at once; one
    /// line of the log for each. Once, at start-up.
    void start();
    /// In place of start(): nothing is looked for, and these folders are the
    /// links, each under its name. For the tests' program on a PC, which has
    /// folders of its own for it (core/tests/mg_storage_windows.cpp); there
    /// the user's real libraries are the last thing to be written into. On a
    /// thread that may wait for Windows.Storage (not a UI thread).
    void startWith(std::vector<std::pair<std::string, winrt::Windows::Storage::StorageFolder>> folders);
    /// True once start()'s search is over.
    bool searched();

    /// The callbacks for megler_run_config.storage, with this as their user,
    /// and the links as they are at this moment: the ones that answer. Waits
    /// for start()'s search. Not on the UI thread, and not while an app runs:
    /// the numbers the layer knows the links by are those of the last call.
    megler_storage_host callbacks();

    /// A link as the Settings screen shows it.
    struct Link {
        std::string name;      // the folder's name in Android's storage: /sdcard/<name>
        std::string path;      // the phone's folder as Windows names it; may be empty (a folder of a provider has none)
        std::string what;      // "the music library", "a folder you added"
        std::string token;     // what FutureAccessList knows an added folder by; empty for the phone's own
        bool answers = false;  // it could be listed when it was found
        std::string note;      // "12 entries", or why it does not answer
    };
    /// Every link found or added, answering or not.
    std::vector<Link> links();

    /// Whether `name` can be a link's name and no link has it (letters compared without their case).
    bool nameFree(const std::string& name);
    /// The names to offer for a folder the user picked: "Documents" and
    /// "Download" while no link has them, then the folder's own, changed
    /// until it is free.
    std::vector<std::string> namesFor(const std::string& folderName);
    /// Adds a folder the user picked as /sdcard/<name> and has Windows
    /// remember it. Empty, or why not. On the UI thread.
    std::string add(winrt::Windows::Storage::StorageFolder const& folder, const std::string& name);
    /// Forgets an added folder. On the UI thread.
    void remove(const std::string& token);

    /// megler_storage_check (megler.h) on the links as they are now: the layer
    /// does in each of them what an app does to files, in a folder of its
    /// own that it removes again, and `line` is told every step. Before
    /// that, three steps of its own: that the music, the pictures and the
    /// videos are among the links, as they are expected to be on a phone.
    /// Returns the last line, "DONE storage: N checks, M failed". Not on the
    /// UI thread, and not while an app runs.
    std::string check(const std::function<void(const std::string&)>& line);

    /// What has been asked so far and which way Windows answered, in a line:
    /// how many calls of each kind, how many folders had to be looked up and
    /// how many were known, and each of the ways round a refusal that was
    /// taken at least once, with the first refusal's code. For the log of a
    /// run on a phone, where that is how the broker is learnt.
    std::string summary();

private:
    StorageHost() = default;

    struct Root {
        Link link;
        bool writable = true;
        winrt::Windows::Storage::StorageFolder folder{nullptr};
    };
    using Roots = std::vector<Root>;

    static void* openThunk(void* user, int link, const char* path, int flags, int kind, int* error);
    static bool makeFolderThunk(void* user, int link, const char* path, int* error);
    static bool removeThunk(void* user, int link, const char* path, bool folder, int* error);
    static bool renameThunk(void* user, int fromLink, const char* from, int toLink, const char* to, int* error);
    static bool thumbnailThunk(void* user, int link, const char* path, int size, int mode, megler_storage_thumbnail_done done, void* core);

    /// Runs `work` on one of the worker threads and returns when it has run.
    void run(const std::function<void()>& work);
    void workerLoop();
    /// Runs `work` on one of the thumbnails' threads, later; returns at once.
    void later(std::function<void()> work);
    void thumbnailLoop();
    void search();
    /// A folder as a link, asked whether it answers, with its line in the log.
    static Root found(const std::string& name, const std::string& what, winrt::Windows::Storage::StorageFolder const& folder, const std::string& token);
    std::shared_ptr<const Roots> given();

    // The workers' own, each called on one of them; a Windows.Storage call that fails throws out of these.
    winrt::Windows::Storage::StorageFolder folderAt(const Roots& roots, int root, const std::wstring& path, int& error);
    HANDLE openOnWorker(const Roots& roots, int root, const std::wstring& path, int flags, int kind, int& error);
    bool makeFolderOnWorker(const Roots& roots, int root, const std::wstring& path, int& error);
    bool removeOnWorker(const Roots& roots, int root, const std::wstring& path, bool folder, int& error);
    bool renameOnWorker(const Roots& roots, int fromRoot, const std::wstring& from, int toRoot, const std::wstring& to, int& error);
    bool removeEmptyFolder(winrt::Windows::Storage::StorageFolder const& folder, int& error);
    std::vector<uint8_t> thumbnailOnWorker(const Roots& roots, int root, const std::wstring& path, int size, int mode, int& error);

    winrt::Windows::Storage::StorageFolder cached(int root, const std::wstring& path);
    void keep(int root, const std::wstring& path, winrt::Windows::Storage::StorageFolder const& folder);
    void forget(int root);

    std::mutex mutex_; // found_, searched_, started_, given_
    std::condition_variable searchDone_;
    Roots found_;
    bool started_ = false, searched_ = false;
    std::shared_ptr<const Roots> given_;
    // What megler_storage_host points at, alive until the next callbacks().
    std::vector<std::string> givenNames_;
    std::vector<megler_storage_link> givenLinks_;

    struct Job {
        const std::function<void()>* work = nullptr;
        bool finished = false;
    };
    std::mutex poolMutex_; // queue_, workers_, idle_, and every Job's finished
    std::condition_variable wake_, done_;
    std::deque<Job*> queue_;
    int workers_ = 0, idle_ = 0;

    struct Kept {
        winrt::Windows::Storage::StorageFolder folder{nullptr};
        std::chrono::steady_clock::time_point at;
    };
    std::mutex cacheMutex_;
    std::unordered_map<std::wstring, Kept> cache_;

    // The thumbnails' threads and what they have to do.
    std::mutex thumbMutex_; // thumbQueue_, thumbWorkers_, thumbIdle_
    std::condition_variable thumbWake_;
    std::deque<std::function<void()>> thumbQueue_;
    int thumbWorkers_ = 0, thumbIdle_ = 0;
};

} // namespace megler::app
