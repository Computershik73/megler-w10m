#include "pch.h"

#include "shell/Files.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

namespace megler::app::files {
namespace {

std::atomic<bool> gSaidRefused{false}, gSaidStreamCopy{false}, gSaidEncrypted{false}, gSaidUncached{false};
// The tests' refusals (testing::), 0 for none.
std::atomic<uint32_t> gRefuseCopyFile2{0}, gRefuseUncached{0};

/// `line` in the log the first time only: a way round the file system said once a run, not once a file.
void sayOnce(std::atomic<bool>& said, const std::string& line) {
    if (!said.exchange(true)) shellLog::write(line + "; said once");
}

/*
 * Whether `path` is one to be read, written or listed: absolute, on a drive ("C:\...") or by a volume's or a share's name
 * ("\\?\...", "\\server\..."). A path made from a folder Windows did not give -- the shared folder, which a package on a memory
 * card may have none of ("" + L"\\apks\\...") -- is relative to the current drive's root, which is no package's: it is refused
 * with ERROR_BAD_PATHNAME, and the first such path is said in the log (an empty one, which stands for no file, is refused
 * without a word). Nothing is then listed, written, made or removed at a drive's root.
 */
bool usable(const std::wstring& path) {
    const auto separator = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if ((path.size() >= 3 && iswalpha(path[0]) && path[1] == L':' && separator(path[2])) ||
        (path.size() >= 3 && separator(path[0]) && separator(path[1]) && !separator(path[2])))
        return true;
    if (!path.empty() && !gSaidRefused.exchange(true))
        shellLog::write("files: a path that is not absolute was refused, not used at the drive's root: \"" + narrow(path) +
                        "\" (as one made from a folder Windows gave none of: a package on a memory card may have no shared folder); said once");
    SetLastError(ERROR_BAD_PATHNAME);
    return false;
}

/// Whether `path` is a drive's or a volume's root ("C:\", "\\?\C:\", "\\?\Volume{...}\", "\\server\share"), which no folder of
/// the shell's is: removeTree refuses one.
bool isRoot(std::wstring path) {
    for (wchar_t& c : path)
        if (c == L'/') c = L'\\';
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    if (path.size() >= 4 && (path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0)) {
        path.erase(0, 4);
        if (_wcsnicmp(path.c_str(), L"UNC\\", 4) == 0) path = L"\\\\" + path.substr(4);
    }
    if (path.size() <= 2) return true;  // "C:", or nothing
    if (path[0] == L'\\' && path[1] == L'\\') {
        // "\\server\share": a component more is a folder.
        const size_t server = path.find(L'\\', 2);
        return server == std::wstring::npos || path.find(L'\\', server + 1) == std::wstring::npos;
    }
    // "Volume{...}" (from "\\?\Volume{...}\"): no separator in it.
    return path.find(L'\\') == std::wstring::npos;
}

HANDLE open(const std::wstring& path, DWORD access, DWORD disposition) {
    if (!usable(path)) return INVALID_HANDLE_VALUE;
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
    return usable(path) && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != 0;
}

uint64_t size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!usable(path) || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool makeDirectory(const std::wstring& path) {
    if (!usable(path)) return false;
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool makeDirectories(const std::wstring& path) {
    // From the folder up, only as far as folders are missing: the ones above an app's own folders (C:\Data, ...) are
    // not the app's to make, and asking for them may be refused rather than answered "already there". (Never up to a drive's
    // root, "C:", nor from a path that is not absolute: usable.)
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
    if (!usable(path) || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
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
    if (!usable(path)) return false;
    if (DeleteFileW(path.c_str())) return true;
    if (GetLastError() != ERROR_ACCESS_DENIED) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) || !(attributes & FILE_ATTRIBUTE_READONLY)) return false;
    const DWORD rest = attributes & ~DWORD(FILE_ATTRIBUTE_READONLY);
    return SetFileAttributesW(path.c_str(), rest ? rest : FILE_ATTRIBUTE_NORMAL) && DeleteFileW(path.c_str());
}

bool move(const std::wstring& from, const std::wstring& to) {
    return usable(from) && usable(to) && MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
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

DiskFile::DiskFile(const std::wstring& path) : handle_(INVALID_HANDLE_VALUE), path_(path) {
    if (!usable(path)) return;
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
    for (;;) {
        // Uncached, a read asks for whole sectors and is given what there is of them up to the file's end.
        const DWORD asked = uncached_ ? DWORD((n + 4095u) & ~4095u) : DWORD(n);
        LARGE_INTEGER pos = {};
        pos.QuadPart = LONGLONG(at);
        DWORD got = 0;
        bool ok = SetFilePointerEx(handle_, pos, nullptr, FILE_BEGIN) != 0;
        if (ok && uncached_ && gRefuseUncached) {
            SetLastError(gRefuseUncached);
            ok = false;
        }
        ok = ok && ReadFile(handle_, buffer_, asked, &got, nullptr);
        if (ok && got >= n) {
            memcpy(out, buffer_, n);
            return true;
        }
        /*
         * A file system that opens a file past the cache and then refuses its reads so -- a memory card's whose sectors are
         * larger than a page, a driver that has no uncached reads -- has it read through the cache from then on: compared
         * so, a copy is checked against what Windows's cache holds of it, which is less, but no copy fails for it.
         */
        const DWORD error = ok ? ERROR_HANDLE_EOF : GetLastError();
        if (!uncached_ || !(error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION || error == ERROR_NOACCESS))
            return false;
        HANDLE cached = open(path_, GENERIC_READ, OPEN_EXISTING);
        if (cached == INVALID_HANDLE_VALUE) return false;
        CloseHandle(handle_);
        handle_ = cached;
        uncached_ = false;
        sayOnce(gSaidUncached, format("files: %s could not be read past Windows's cache (error %lu): read through it instead, as every file whose "
                                      "file system refuses that",
                                      narrow(path_).c_str(), error));
    }
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

/// CopyFile2 with `flags`, `progress` told of each chunk: 0, or its Win32 error.
DWORD copyFile2(const std::wstring& from, const std::wstring& to, DWORD flags, const Progress& progress) {
    if (gRefuseCopyFile2) return gRefuseCopyFile2;
    COPYFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwCopyFlags = flags;
    if (progress) {
        params.pProgressRoutine = &copyProgress;
        params.pvCallbackContext = const_cast<Progress*>(&progress);
    }
    const HRESULT hr = CopyFile2(from.c_str(), to.c_str(), &params);
    return SUCCEEDED(hr) ? 0 : HRESULT_FACILITY(hr) == FACILITY_WIN32 ? DWORD(HRESULT_CODE(hr)) : DWORD(hr);
}

/// Whether CopyFile2 was refused for what it copies besides the bytes, which a plain read and write do without: a memory card's
/// FAT32 or exFAT has no encryption of Windows's (ERROR_ENCRYPTION_FAILED), no extended attributes and no streams but the file's
/// own (ERROR_EAS_NOT_SUPPORTED, ERROR_NOT_SUPPORTED, ERROR_INVALID_PARAMETER, ERROR_INVALID_FUNCTION), and an app container may
/// be refused what goes with a file (ERROR_ACCESS_DENIED, ERROR_PRIVILEGE_NOT_HELD).
bool refusedForExtras(DWORD error) {
    switch (error) {
    case ERROR_ENCRYPTION_FAILED: case ERROR_EAS_NOT_SUPPORTED: case ERROR_NOT_SUPPORTED: case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_FUNCTION: case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD:
        return true;
    default:
        return false;
    }
}

/// `from` copied by reading it and writing `to` (made anew), a megabyte at a time, and given the time `from` was last written,
/// as CopyFile2 gives a copy: the bytes and nothing else, so a plain file whatever `from` is. On failure `to` is removed and
/// GetLastError() says why.
bool streamCopy(const std::wstring& from, const std::wstring& to, const Progress& progress) {
    HANDLE in = open(from, GENERIC_READ, OPEN_EXISTING);
    if (in == INVALID_HANDLE_VALUE) return false;
    HANDLE out = open(to, GENERIC_WRITE, CREATE_ALWAYS);
    if (out == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        CloseHandle(in);
        SetLastError(error);
        return false;
    }
    LARGE_INTEGER total = {};
    GetFileSizeEx(in, &total);
    std::vector<char> buffer(1u << 20);
    uint64_t done = 0;
    DWORD error = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buffer.data(), DWORD(buffer.size()), &got, nullptr)) {
            error = GetLastError();
            break;
        }
        if (!got) break;
        SetLastError(0);
        if (!writeAll(out, std::string_view(buffer.data(), got))) {
            error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
            break;
        }
        done += got;
        if (progress) progress(Stage::copying, done, uint64_t(total.QuadPart));
    }
    FILE_BASIC_INFO basic = {};
    if (!error && GetFileInformationByHandleEx(in, FileBasicInfo, &basic, sizeof(basic))) {
        FILE_BASIC_INFO times = {};  // (Its other times and its attributes 0: left as they are.)
        times.LastWriteTime = basic.LastWriteTime;
        SetFileInformationByHandle(out, FileBasicInfo, &times, sizeof(times));
    }
    CloseHandle(in);
    CloseHandle(out);
    if (error) {
        DeleteFileW(to.c_str());
        SetLastError(error);
        return false;
    }
    return true;
}

/*
 * `from` copied to `to`, `progress` told of each chunk; whether it was, and when not, its Win32 error for GetLastError. CopyFile2
 * with `flags`, but where a memory card's file system or an app container refuses it what it copies besides the bytes
 * (refusedForExtras: encryption first, then the rest), the bytes read and written (streamCopy) -- not where it is asked past
 * the cache (COPY_FILE_NO_BUFFERING), which copyWhole asks again through it. A file Windows encrypted -- an app's on a memory
 * card is, for that app -- is copied so at once: the copy is plain, and the publisher's other packages can read it in the
 * shared folder, where CopyFile2 made it encrypted again. Each way round said once.
 */
bool copyFile(const std::wstring& from, const std::wstring& to, DWORD flags, const Progress& progress) {
    if (!usable(from) || !usable(to)) return false;
    const DWORD attributes = GetFileAttributesW(from.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_ENCRYPTED) && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        sayOnce(gSaidEncrypted, "files: " + narrow(from) + " is encrypted, as Windows keeps an app's files on a memory card: copied by reading and "
                                                             "writing it, so that the copy is plain and the publisher's other packages read it");
        return streamCopy(from, to, progress);
    }
    DWORD error = copyFile2(from, to, flags, progress);
    if (error == ERROR_ENCRYPTION_FAILED) error = copyFile2(from, to, flags | COPY_FILE_ALLOW_DECRYPTED_DESTINATION, progress);
    if (!error) return true;
    if ((flags & COPY_FILE_NO_BUFFERING) || !refusedForExtras(error)) {
        SetLastError(error);
        return false;
    }
    // (Where reading and writing cannot copy it either, GetLastError says why they could not.)
    if (!streamCopy(from, to, progress)) return false;
    sayOnce(gSaidStreamCopy, format("files: CopyFile2 refused to copy %s (error %lu), and reading and writing it copied it: so is every copy it "
                                    "refuses so -- a memory card's file system has not all a copy of Windows's takes along",
                                    narrow(from).c_str(), error));
    return true;
}

} // namespace

bool copy(const std::wstring& from, const std::wstring& to) { return copyFile(from, to, 0, nullptr); }

namespace testing {
void refuseCopyFile2(uint32_t error) { gRefuseCopyFile2 = error; }
void refuseUncachedReads(uint32_t error) { gRefuseUncached = error; }
} // namespace testing

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
    if (!usable(directory)) return names;
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
    if (!usable(directory)) return names;
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
    if (!usable(path)) return false;
    if (isRoot(path)) {
        SetLastError(ERROR_BAD_PATHNAME);
        return false;
    }
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

uint64_t treeSize(const std::wstring& path) {
    uint64_t total = 0;
    WIN32_FIND_DATAW found = {};
    if (!usable(path)) return 0;
    // (The listing's own sizes: one call a folder, none a file.)
    HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring name = found.cFileName;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            total += (uint64_t(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
        else if (name != L"." && name != L".." && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            total += treeSize(path + L"\\" + name);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return total;
}

LineLog::LineLog(std::wstring path) : path_(std::move(path)) { write(path_, std::string_view()); }

void LineLog::line(const std::string& text) { append(path_, timeOfDay() + " " + text + "\r\n"); }

} // namespace megler::app::files
