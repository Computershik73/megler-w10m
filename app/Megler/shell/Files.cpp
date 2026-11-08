#include "pch.h"

#include "shell/Files.h"

#include "shell/Strings.h"

namespace megler::app::files {
namespace {

HANDLE open(const std::wstring& path, DWORD access, DWORD disposition) {
    CREATEFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    return CreateFile2(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, &params);
}

bool writeAll(HANDLE h, std::string_view data) {
    while (!data.empty()) {
        DWORD written = 0;
        const DWORD chunk = DWORD(std::min<size_t>(data.size(), 1u << 20));
        if (!WriteFile(h, data.data(), chunk, &written, nullptr) || written == 0) return false;
        data.remove_prefix(written);
    }
    return true;
}

} // namespace

bool exists(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != 0;
}

uint64_t size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool makeDirectory(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool makeDirectories(const std::wstring& path) {
    // From the folder up, only as far as folders are missing: the ones above an app's own folders (C:\Data, ...) are
    // not the app's to make, and asking for them may be refused rather than answered "already there".
    if (makeDirectory(path)) return true;
    if (GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash == 0) return false;
    return makeDirectories(path.substr(0, slash)) && makeDirectory(path);
}

bool read(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE h = open(path, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buffer[16384];
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buffer, sizeof(buffer), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        out.append(buffer, got);
    }
    CloseHandle(h);
    return ok;
}

bool readRange(const std::wstring& path, uint64_t at, uint64_t n, std::string& out) {
    out.clear();
    HANDLE h = open(path, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos = {};
    pos.QuadPart = LONGLONG(at);
    bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
    char buffer[65536];
    while (ok && out.size() < n) {
        DWORD got = 0;
        const DWORD want = DWORD(std::min<uint64_t>(sizeof(buffer), n - out.size()));
        if (!ReadFile(h, buffer, want, &got, nullptr)) ok = false;
        else if (got == 0) break;
        else out.append(buffer, got);
    }
    CloseHandle(h);
    return ok;
}

uint64_t lastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

bool write(const std::wstring& path, std::string_view data) {
    HANDLE h = open(path, GENERIC_WRITE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = writeAll(h, data);
    CloseHandle(h);
    return ok;
}

bool append(const std::wstring& path, std::string_view data) {
    HANDLE h = open(path, FILE_APPEND_DATA, OPEN_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = writeAll(h, data);
    CloseHandle(h);
    return ok;
}

// A file an app made read-only (chmod, File.setReadOnly: the core keeps it as Windows' read-only attribute) goes all the same,
// as POSIX's unlink takes one: an app's folders are removed whole (its data moved or cleared, a bundle unpacked again).
bool remove(const std::wstring& path) {
    if (DeleteFileW(path.c_str())) return true;
    if (GetLastError() != ERROR_ACCESS_DENIED) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) || !(attributes & FILE_ATTRIBUTE_READONLY)) return false;
    const DWORD rest = attributes & ~DWORD(FILE_ATTRIBUTE_READONLY);
    return SetFileAttributesW(path.c_str(), rest ? rest : FILE_ATTRIBUTE_NORMAL) && DeleteFileW(path.c_str());
}

bool move(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool copy(const std::wstring& from, const std::wstring& to) {
    return SUCCEEDED(CopyFile2(from.c_str(), to.c_str(), nullptr));
}

bool flush(const std::wstring& path) {
    HANDLE h = open(path, GENERIC_WRITE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

namespace {

bool readEnd(const std::wstring& path, uint32_t bytes, uint64_t& size, std::string& out) {
    HANDLE h = open(path, GENERIC_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER whole = {};
    bool ok = GetFileSizeEx(h, &whole) != 0;
    if (ok) {
        size = uint64_t(whole.QuadPart);
        const uint64_t n = std::min<uint64_t>(bytes, size);
        LARGE_INTEGER at = {};
        at.QuadPart = LONGLONG(size - n);
        out.assign(size_t(n), '\0');
        DWORD got = 0;
        ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) != 0 &&
             (n == 0 || (ReadFile(h, out.data(), DWORD(n), &got, nullptr) && got == DWORD(n)));
    }
    CloseHandle(h);
    return ok;
}

} // namespace

bool sameEnd(const std::wstring& a, const std::wstring& b, uint32_t bytes) {
    uint64_t sizeA = 0, sizeB = 0;
    std::string endA, endB;
    return readEnd(a, bytes, sizeA, endA) && readEnd(b, bytes, sizeB, endB) && sizeA == sizeB && endA == endB;
}

bool same(const std::wstring& a, const std::wstring& b) {
    HANDLE x = open(a, GENERIC_READ, OPEN_EXISTING);
    if (x == INVALID_HANDLE_VALUE) return false;
    HANDLE y = open(b, GENERIC_READ, OPEN_EXISTING);
    if (y == INVALID_HANDLE_VALUE) {
        CloseHandle(x);
        return false;
    }
    LARGE_INTEGER sizeX = {}, sizeY = {};
    bool alike = GetFileSizeEx(x, &sizeX) && GetFileSizeEx(y, &sizeY) && sizeX.QuadPart == sizeY.QuadPart;
    std::vector<char> p(65536), q(65536);
    for (int64_t left = sizeX.QuadPart; alike && left > 0;) {
        const DWORD n = DWORD(std::min<int64_t>(left, int64_t(p.size())));
        DWORD gotX = 0, gotY = 0;
        alike = ReadFile(x, p.data(), n, &gotX, nullptr) && ReadFile(y, q.data(), n, &gotY, nullptr) && gotX == n && gotY == n &&
                !memcmp(p.data(), q.data(), n);
        left -= n;
    }
    CloseHandle(x);
    CloseHandle(y);
    return alike;
}

namespace {

bool writeAt(HANDLE h, uint64_t at, const char* data, DWORD n) {
    LARGE_INTEGER pos = {};
    pos.QuadPart = LONGLONG(at);
    DWORD written = 0;
    return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && WriteFile(h, data, n, &written, nullptr) && written == n;
}

} // namespace

DiskFile::DiskFile(const std::wstring& path) : handle_(INVALID_HANDLE_VALUE) {
    CREATEFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    params.dwFileFlags = FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN;
    handle_ = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, &params);
    uncached_ = handle_ != INVALID_HANDLE_VALUE;
    if (!uncached_) handle_ = open(path, GENERIC_READ, OPEN_EXISTING);
    LARGE_INTEGER size = {};
    if (handle_ != INVALID_HANDLE_VALUE && GetFileSizeEx(handle_, &size)) size_ = uint64_t(size.QuadPart);
    // (A page is a whole number of any sector a phone's or a PC's disk has.)
    buffer_ = static_cast<char*>(VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

DiskFile::~DiskFile() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    if (buffer_) VirtualFree(buffer_, 0, MEM_RELEASE);
}

bool DiskFile::isOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

bool DiskFile::read(uint64_t at, char* out, uint32_t n) {
    if (handle_ == INVALID_HANDLE_VALUE || !buffer_ || n > chunk || at > size_ || n > size_ - at) return false;
    // Uncached, a read asks for whole sectors and is given what there is of them up to the file's end.
    const DWORD asked = uncached_ ? DWORD((n + 4095u) & ~4095u) : DWORD(n);
    LARGE_INTEGER pos = {};
    pos.QuadPart = LONGLONG(at);
    DWORD got = 0;
    if (!SetFilePointerEx(handle_, pos, nullptr, FILE_BEGIN) || !ReadFile(handle_, buffer_, asked, &got, nullptr) || got < n) return false;
    memcpy(out, buffer_, n);
    return true;
}

bool mend(const std::wstring& source, const std::wstring& copy, uint64_t& mended, std::vector<uint64_t>* chunks, const Progress& progress) {
    const DWORD chunk = DiskFile::chunk;
    mended = 0;
    // (The source past the cache as well: what is compared is what the disk has, and a phone's memory is not filled with it.)
    DiskFile from(source);
    if (!from.isOpen()) return false;
    HANDLE to = open(copy, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    if (to == INVALID_HANDLE_VALUE) return false;
    DiskFile disk(copy);
    LARGE_INTEGER sizeTo = {};
    const uint64_t total = from.size();
    bool ok = disk.isOpen() && GetFileSizeEx(to, &sizeTo) && uint64_t(sizeTo.QuadPart) == total;
    std::vector<char> a(chunk), b(chunk), again(chunk);
    for (uint64_t at = 0; ok && at < total; at += chunk) {
        const DWORD n = DWORD(std::min<uint64_t>(chunk, total - at));
        ok = from.read(at, a.data(), n) && disk.read(at, b.data(), n);
        if (ok && memcmp(a.data(), b.data(), n)) {
            // The source read twice alike first, so that what is written is the file's and not a reading's mistake; then the
            // chunk written, put on the disk and read back from there.
            bool fixed = false;
            for (int tries = 0; tries < 3 && !fixed; tries++) {
                if (!from.read(at, a.data(), n) || !from.read(at, again.data(), n) || memcmp(a.data(), again.data(), n)) continue;
                fixed = writeAt(to, at, a.data(), n) && FlushFileBuffers(to) && disk.read(at, b.data(), n) && !memcmp(a.data(), b.data(), n);
            }
            ok = fixed;
            if (fixed) {
                mended += n;
                if (chunks) chunks->push_back(at);
            }
        }
        if (ok && progress) progress(Stage::comparing, at + n, total);
    }
    CloseHandle(to);
    return ok;
}

namespace {

COPYFILE2_MESSAGE_ACTION CALLBACK copyProgress(const COPYFILE2_MESSAGE* message, PVOID context) {
    if (message->Type == COPYFILE2_CALLBACK_CHUNK_FINISHED)
        (*static_cast<const Progress*>(context))(Stage::copying, uint64_t(message->Info.ChunkFinished.uliTotalBytesTransferred.QuadPart),
                                                 uint64_t(message->Info.ChunkFinished.uliTotalFileSize.QuadPart));
    return COPYFILE2_PROGRESS_CONTINUE;
}

/// CopyFile2 with `flags`, `progress` told of each chunk; whether it copied, and when not, its Win32 error for GetLastError.
bool copyFile(const std::wstring& from, const std::wstring& to, DWORD flags, const Progress& progress) {
    COPYFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwCopyFlags = flags;
    if (progress) {
        params.pProgressRoutine = &copyProgress;
        params.pvCallbackContext = const_cast<Progress*>(&progress);
    }
    const HRESULT hr = CopyFile2(from.c_str(), to.c_str(), &params);
    if (FAILED(hr)) SetLastError(HRESULT_FACILITY(hr) == FACILITY_WIN32 ? DWORD(HRESULT_CODE(hr)) : DWORD(hr));
    return SUCCEEDED(hr);
}

} // namespace

bool copyWhole(const std::wstring& from, const std::wstring& to, uint64_t& mended, const Progress& progress) {
    mended = 0;
    const uint64_t total = size(from);
    bool copied = false;
    if (total >= unbufferedFrom) {
        copied = copyFile(from, to, COPY_FILE_NO_BUFFERING, progress);
        // (A full disk is full through the cache too.)
        const DWORD error = copied ? 0 : GetLastError();
        if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) return false;
    }
    if (!copied && !copyFile(from, to, 0, progress)) return false;
    if (progress) progress(Stage::flushing, 0, total);
    if (!flush(to)) return false;
    if (progress) progress(Stage::comparing, 0, total);
    return mend(from, to, mended, nullptr, progress);
}

std::vector<std::wstring> list(const std::wstring& directory, const std::wstring& pattern) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found = {};
    HANDLE h = FindFirstFileExW((directory + L"\\" + pattern).c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.emplace_back(found.cFileName);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return names;
}

std::vector<std::wstring> listFolders(const std::wstring& directory, const std::wstring& pattern) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found = {};
    HANDLE h = FindFirstFileExW((directory + L"\\" + pattern).c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        const std::wstring name = found.cFileName;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") names.push_back(name);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return names;
}

bool removeTree(const std::wstring& path) {
    bool ok = true;
    for (const auto& name : list(path, L"*")) ok = remove(path + L"\\" + name) && ok;
    for (const auto& name : listFolders(path, L"*")) ok = removeTree(path + L"\\" + name) && ok;
    return (RemoveDirectoryW(path.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) && ok;
}

uint64_t folderSize(const std::wstring& path) {
    uint64_t total = 0;
    for (const auto& name : list(path, L"*")) total += size(path + L"\\" + name);
    return total;
}

LineLog::LineLog(std::wstring path) : path_(std::move(path)) { write(path_, std::string_view()); }

void LineLog::line(const std::string& text) { append(path_, timeOfDay() + " " + text + "\r\n"); }

} // namespace megler::app::files
