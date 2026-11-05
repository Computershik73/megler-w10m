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

bool remove(const std::wstring& path) { return DeleteFileW(path.c_str()) != 0; }

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

namespace {

bool readAt(HANDLE h, uint64_t at, char* out, DWORD n) {
    LARGE_INTEGER pos = {};
    pos.QuadPart = LONGLONG(at);
    DWORD got = 0;
    return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, out, n, &got, nullptr) && got == n;
}

bool writeAt(HANDLE h, uint64_t at, const char* data, DWORD n) {
    LARGE_INTEGER pos = {};
    pos.QuadPart = LONGLONG(at);
    DWORD written = 0;
    return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && WriteFile(h, data, n, &written, nullptr) && written == n;
}

} // namespace

bool mend(const std::wstring& source, const std::wstring& copy, uint64_t& mended) {
    const DWORD chunk = 1u << 20;
    mended = 0;
    HANDLE from = open(source, GENERIC_READ, OPEN_EXISTING);
    if (from == INVALID_HANDLE_VALUE) return false;
    HANDLE to = open(copy, GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING);
    if (to == INVALID_HANDLE_VALUE) {
        CloseHandle(from);
        return false;
    }
    LARGE_INTEGER sizeFrom = {}, sizeTo = {};
    bool ok = GetFileSizeEx(from, &sizeFrom) && GetFileSizeEx(to, &sizeTo) && sizeFrom.QuadPart == sizeTo.QuadPart;
    std::vector<char> a(chunk), b(chunk), again(chunk);
    for (uint64_t at = 0; ok && at < uint64_t(sizeFrom.QuadPart); at += chunk) {
        const DWORD n = DWORD(std::min<uint64_t>(chunk, uint64_t(sizeFrom.QuadPart) - at));
        ok = readAt(from, at, a.data(), n) && readAt(to, at, b.data(), n);
        if (!ok || !memcmp(a.data(), b.data(), n)) continue;
        // The source read twice alike first, so that what is written is the file's and not a reading's mistake; then the
        // chunk written, put on the disk and read back.
        bool fixed = false;
        for (int tries = 0; tries < 3 && !fixed; tries++) {
            if (!readAt(from, at, a.data(), n) || !readAt(from, at, again.data(), n) || memcmp(a.data(), again.data(), n)) continue;
            fixed = writeAt(to, at, a.data(), n) && FlushFileBuffers(to) && readAt(to, at, b.data(), n) && !memcmp(a.data(), b.data(), n);
        }
        ok = fixed;
        if (fixed) mended += n;
    }
    CloseHandle(from);
    CloseHandle(to);
    return ok;
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
