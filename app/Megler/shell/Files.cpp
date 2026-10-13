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

LineLog::LineLog(std::wstring path) : path_(std::move(path)) { write(path_, std::string_view()); }

void LineLog::line(const std::string& text) { append(path_, timeOfDay() + " " + text + "\r\n"); }

} // namespace megler::app::files
