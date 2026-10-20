#include "pch.h"

#include "shell/Files.h"

#include "shell/Strings.h"

namespace meglet::app::files {
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

} // namespace meglet::app::files
