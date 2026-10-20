// A file the user picked, read where it is. An app container may not open it by its path; Windows' broker hands out
// handles to it (IStorageItemHandleAccess), and the core is given it under a path of Megler's choosing, which it opens by
// asking here for a new handle each time (megler_file_by_handle): megler_apk_report, megler_apk_icon, megler_bundle_unpack and
// the package's making read it so, and no copy is made unless one is asked for.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace megler::app {

class PickedFile {
public:
    /// The file, as the core's `path` until this goes. Off the UI thread (its size is asked for).
    PickedFile(winrt::Windows::Storage::StorageFile file, std::wstring path);
    ~PickedFile();
    PickedFile(const PickedFile&) = delete;
    PickedFile& operator=(const PickedFile&) = delete;

    /// Whether the core has it (a phone too old for the broker's handles has not).
    bool ok() const { return ok_; }
    const std::wstring& path() const { return path_; }
    const std::string& name() const { return name_; }
    uint64_t size() const { return size_; }

    /// A copy of it at `target`, written beside and moved into place; `progress` is told the bytes copied. Empty, or why
    /// not. Off the UI thread.
    std::string copyTo(const std::wstring& target, const std::function<void(uint64_t)>& progress = nullptr) const;

private:
    static void* open(void* user);

    winrt::Windows::Storage::StorageFile file_;
    std::wstring path_;
    std::string name_;
    uint64_t size_ = 0;
    bool ok_ = false;
};

} // namespace megler::app
