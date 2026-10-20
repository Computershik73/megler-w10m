#include "pch.h"

#include "shell/PickedFile.h"

#include "megler.h"
#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

// The interface of <windowsstoragecom.h> that hands over a file's handle, declared here by hand as player/StorageHost.cpp
// declares it (the SDK's header brings in the shell's, which does not compile for an app container).
namespace {

enum HANDLE_OPTIONS : DWORD { HO_NONE = 0 };
enum HANDLE_ACCESS_OPTIONS : DWORD { HAO_READ = 0x120089 };
enum HANDLE_SHARING_OPTIONS : DWORD { HSO_SHARE_READ = 1, HSO_SHARE_DELETE = 4 };

struct __declspec(uuid("5CA296B2-2C25-4D22-B785-B885C8201E6A")) __declspec(novtable) IStorageItemHandleAccess : ::IUnknown {
    virtual HRESULT __stdcall Create(HANDLE_ACCESS_OPTIONS access, HANDLE_SHARING_OPTIONS sharing, HANDLE_OPTIONS options, ::IUnknown* oplockBreakingHandler,
                                     HANDLE* handle) = 0;
};

} // namespace

namespace megler::app {

PickedFile::PickedFile(winrt::Windows::Storage::StorageFile file, std::wstring path) : file_(std::move(file)), path_(std::move(path)) {
    name_ = narrow(std::wstring(file_.Name()));
    try {
        size_ = file_.GetBasicPropertiesAsync().get().Size();
    } catch (const winrt::hresult_error&) {
    }
    // Asked once here: a broker that hands out no handle (before 1703) is said now, not at the first read.
    if (HANDLE h = static_cast<HANDLE>(open(this)); h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        ok_ = megler_file_by_handle(narrow(path_).c_str(), &PickedFile::open, this);
    }
    shellLog::write("picked: " + name_ + (ok_ ? " read where it is, as " + narrow(path_) : std::string(" can't be read where it is")));
}

PickedFile::~PickedFile() {
    if (ok_) megler_file_by_handle(narrow(path_).c_str(), nullptr, nullptr);
}

void* PickedFile::open(void* user) {
    auto* self = static_cast<PickedFile*>(user);
    try {
        winrt::com_ptr<IStorageItemHandleAccess> access;
        if (!self->file_.try_as(access)) return nullptr;
        HANDLE h = INVALID_HANDLE_VALUE;
        const HRESULT hr = access->Create(HAO_READ, HANDLE_SHARING_OPTIONS(HSO_SHARE_READ | HSO_SHARE_DELETE), HO_NONE, nullptr, &h);
        if (FAILED(hr) || h == INVALID_HANDLE_VALUE) {
            shellLog::write(format("picked: no handle to %s (0x%08X)", self->name_.c_str(), unsigned(hr)));
            return nullptr;
        }
        return h;
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("picked: no handle to %s (0x%08X)", self->name_.c_str(), unsigned(e.code())));
        return nullptr;
    }
}

std::string PickedFile::copyTo(const std::wstring& target, const std::function<void(uint64_t)>& progress) const {
    winrt::file_handle in{static_cast<HANDLE>(open(const_cast<PickedFile*>(this)))};
    if (!in || in.get() == INVALID_HANDLE_VALUE) return "the picked file can't be read";
    const std::wstring part = target + L".part";
    CREATEFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    std::string error;
    {
        winrt::file_handle out{CreateFile2(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, &params)};
        if (!out) return format("the copy could not be created (error %lu)", GetLastError());
        std::vector<char> buffer(1 << 20);
        uint64_t copied = 0;
        for (;;) {
            DWORD got = 0, written = 0;
            if (!ReadFile(in.get(), buffer.data(), DWORD(buffer.size()), &got, nullptr)) {
                error = format("reading the picked file failed (error %lu)", GetLastError());
                break;
            }
            if (got == 0) break;
            if (!WriteFile(out.get(), buffer.data(), got, &written, nullptr) || written != got) {
                error = format("writing the copy failed (error %lu) -- is the phone's storage full?", GetLastError());
                break;
            }
            copied += got;
            if (progress) progress(copied);
        }
        if (error.empty() && size_ && copied != size_)
            error = format("the copy ended after %llu of %llu bytes", (unsigned long long)copied, (unsigned long long)size_);
    }
    if (error.empty() && !files::move(part, target)) error = format("the copy could not be put in place (error %lu)", GetLastError());
    if (!error.empty()) files::remove(part);
    return error;
}

} // namespace megler::app
