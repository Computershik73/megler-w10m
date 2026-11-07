// The check of the shell's ZIP writer on a PC (tools/zip-writer-test.sh builds and runs it): app/Megler/shell/ZipWriter.cpp,
// the very file Megler.exe is built from, with the core's zlib, writes a ZIP the way Save All Logs does -- an entry from
// memory, a log written in pieces of every size, an empty file, a name in UTF-8, 20 MB that do not compress (the ZIP passes
// 16 MB), a file of a given time -- and each entry's bytes are written beside it, for tools/zip-writer-check.py to have
// Python's zipfile and Explorer's ZIP folders read the ZIP and compare.
//
//   zip-writer-test <folder>    <folder>\test.zip; <folder>\expected\<n>.bin, the n-th entry's bytes; <folder>\expected.txt,
//                               a line for each: its name, size, CRC-32 and time (local, as a ZIP keeps it), by tabs
#include "pch.h"

#include "shell/ZipWriter.h"

#include "../core/third_party/zlib/zlib.h"

#include <cstdio>
#include <string>
#include <vector>

using megler::app::ZipWriter;

namespace {

std::string utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool writeFile(const std::wstring& path, const std::string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return fclose(f) == 0 && ok;
}

/// A FILETIME (UTC) as the local time a ZIP keeps, "YYYY-MM-DD HH:MM:SS".
std::string localTime(uint64_t filetime) {
    FILETIME utc{DWORD(filetime), DWORD(filetime >> 32)}, local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &st);
    char text[32];
    snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return text;
}

uint64_t now() {
    FILETIME t{};
    GetSystemTimeAsFileTime(&t);
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

/// 2026-11-08 02:15:30 local time, as a FILETIME in UTC.
uint64_t fixedTime() {
    SYSTEMTIME st{2026, 11, 0, 8, 2, 15, 30, 0};
    FILETIME local{}, utc{};
    SystemTimeToFileTime(&st, &local);
    LocalFileTimeToFileTime(&local, &utc);
    return (uint64_t(utc.dwHighDateTime) << 32) | utc.dwLowDateTime;
}

struct Expected {
    std::string name, data;
    uint64_t modified;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: zip-writer-test <folder>\n");
        return 2;
    }
    const std::wstring folder = argv[1];
    CreateDirectoryW(folder.c_str(), nullptr);
    CreateDirectoryW((folder + L"\\expected").c_str(), nullptr);

    std::vector<Expected> entries;
    entries.push_back({"summary.txt", "Megler's logs\r\nSaved: a test of the shell's ZIP writer\r\n", 0});
    std::string log;
    for (int i = 0; log.size() < 3u * 1024 * 1024; i++) {
        char line[160];
        snprintf(line, sizeof(line), "02:%02d:%02d.%03d I shell: line %d of a log that compresses well, as a log does\r\n", (i / 600) % 60,
                 (i / 10) % 60, i % 1000, i);
        log += line;
    }
    entries.push_back({"Megler/megler.log", log, 0});
    entries.push_back({"Megler/empty.txt", "", 0});
    entries.push_back({utf8(L"MeglerApp.test/имя-ñ.txt"), utf8(L"Текст in UTF-8\r\n"), 0});
    // 20 MB that deflate cannot make smaller (xorshift), which it keeps in stored blocks of its own: the ZIP passes 16 MB.
    std::string noise(20u * 1024 * 1024, '\0');
    uint32_t x = 2463534242u;
    for (char& c : noise) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = char(x & 0xff);
    }
    entries.push_back({"MeglerApp.test/noise.bin", noise, 0});
    entries.push_back({"MeglerApp.test/dated.txt", "A file of 2026-11-08 02:15:30, local time.\r\n", fixedTime()});

    const uint64_t started = now();
    ZipWriter zip(folder + L"\\test.zip");
    std::string list;
    uint32_t seed = 1;
    for (size_t i = 0; i < entries.size(); i++) {
        const Expected& e = entries[i];
        if (i == 0) {
            if (!zip.add(e.name, e.data, e.modified)) break;
        } else {
            // In pieces of every size, as a file is read: 1 byte up to 300 KB.
            if (!zip.begin(e.name, e.modified)) break;
            size_t at = 0;
            while (at < e.data.size()) {
                seed = seed * 1103515245u + 12345u;
                const size_t n = std::min<size_t>(e.data.size() - at, 1 + (seed >> 8) % (300 * 1024));
                if (!zip.write(e.data.data() + at, n)) break;
                at += n;
            }
            if (!zip.end()) break;
        }
        const uint32_t crc = uint32_t(crc32(0, reinterpret_cast<const Bytef*>(e.data.data()), uInt(e.data.size())));
        list += e.name + "\t" + std::to_string(e.data.size()) + "\t" + std::to_string(crc) + "\t" +
                localTime(e.modified ? e.modified : started) + "\n";
        if (!writeFile(folder + L"\\expected\\" + std::to_wstring(i) + L".bin", e.data)) {
            fprintf(stderr, "could not write the expected bytes of %s\n", e.name.c_str());
            return 1;
        }
    }
    if (!zip.finish("Megler's logs: a test of the shell's ZIP writer")) {
        fprintf(stderr, "the ZIP writer failed: %s\n", zip.error().c_str());
        return 1;
    }
    if (!writeFile(folder + L"\\expected.txt", list)) {
        fprintf(stderr, "could not write expected.txt\n");
        return 1;
    }
    printf("wrote %zu entries, %llu bytes\n", zip.entries(), (unsigned long long)zip.size());
    return 0;
}
