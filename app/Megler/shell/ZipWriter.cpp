#include "pch.h"

#include "shell/ZipWriter.h"

// The core's zlib, compiled into Megler.exe already (tools/build-appx.sh; on desktop Windows tools/build-desktop.sh's half of
// the core for Megler.exe): its deflate and crc32. (By its path: the shell's own include folders do not name the core's
// third-party code, and zlib.h finds its zconf.h beside it.)
#include "../../../core/third_party/zlib/zlib.h"

#include <algorithm>
#include <cstring>

namespace megler::app {

namespace {

constexpr uint32_t kLocalHeader = 0x04034b50, kCentralHeader = 0x02014b50, kEnd = 0x06054b50;
constexpr uint16_t kVersion = 20;      // 2.0: deflate, folders in names
constexpr uint16_t kUtf8 = 0x0800;     // the general purpose flag's bit 11: the name is UTF-8
constexpr uint16_t kDeflated = 8;
constexpr size_t kBuffer = 256 * 1024, kChunk = 64 * 1024;

void le16(std::string& out, uint16_t v) {
    out += char(v & 0xff);
    out += char(v >> 8);
}

void le32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out += char((v >> (8 * i)) & 0xff);
}

/// A FILETIME (UTC; 0: now) as DOS's time and date, in local time, as Explorer and Python's zipfile show them. Before 1980
/// (DOS's first year) is 1980-01-01.
void dosTime(uint64_t modified, uint16_t& time, uint16_t& date) {
    FILETIME utc{DWORD(modified & 0xffffffff), DWORD(modified >> 32)}, local{};
    if (!modified) GetSystemTimeAsFileTime(&utc);
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st) || st.wYear < 1980 || st.wYear > 2107) {
        time = 0;
        date = uint16_t((1 << 5) | 1);
        return;
    }
    time = uint16_t((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    date = uint16_t(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
}

} // namespace

ZipWriter::ZipWriter(const std::wstring& path) : file_(INVALID_HANDLE_VALUE), chunk_(kChunk) {
    CREATEFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    params.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
    file_ = CreateFile2(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, &params);
    if (file_ == INVALID_HANDLE_VALUE) error_ = "the file could not be made (error " + std::to_string(GetLastError()) + ")";
    buffer_.reserve(kBuffer + kChunk);
}

ZipWriter::~ZipWriter() {
    if (stream_) {
        deflateEnd(static_cast<z_stream*>(stream_));
        delete static_cast<z_stream*>(stream_);
    }
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool ZipWriter::fail(std::string why) {
    if (error_.empty()) error_ = std::move(why);
    return false;
}

bool ZipWriter::flush() {
    size_t at = 0;
    while (at < buffer_.size()) {
        DWORD done = 0;
        const DWORD n = DWORD(std::min<size_t>(buffer_.size() - at, 1u << 20));
        if (!WriteFile(file_, buffer_.data() + at, n, &done, nullptr) || done == 0)
            return fail("the file could not be written (error " + std::to_string(GetLastError()) + "): is the storage full?");
        at += done;
        written_ += done;
    }
    buffer_.clear();
    return true;
}

bool ZipWriter::put(const void* data, size_t size) {
    // (A ZIP without ZIP64 ends at 4 GB: every offset and size in it is 32 bits.)
    if (this->size() + size > 0xffffffffull) return fail("the ZIP would pass 4 GB");
    buffer_.append(static_cast<const char*>(data), size);
    return buffer_.size() < kBuffer || flush();
}

bool ZipWriter::deflateOut(int mode) {
    z_stream* z = static_cast<z_stream*>(stream_);
    for (;;) {
        z->next_out = chunk_.data();
        z->avail_out = uInt(chunk_.size());
        const int status = deflate(z, mode);
        if (status == Z_STREAM_ERROR) return fail("deflate failed");
        const size_t made = chunk_.size() - z->avail_out;
        if (made && !put(chunk_.data(), made)) return false;
        out_ += made;
        // Done when deflate has taken all it was given and had room left (Z_NO_FLUSH), or has said the stream ended.
        if (mode == Z_FINISH ? status == Z_STREAM_END : z->avail_in == 0 && z->avail_out != 0) return true;
        if (status == Z_BUF_ERROR && made == 0) return fail("deflate made no progress");
    }
}

bool ZipWriter::begin(const std::string& name, uint64_t modified) {
    if (!ok()) return false;
    if (stream_ && !end()) return false;
    if (name.empty() || name.size() > 0xffff) return fail("an entry's name is empty or too long");
    if (entries_.size() >= 0xffff) return fail("more than 65535 entries");
    if (size() > 0xffffffffull) return fail("the ZIP would pass 4 GB");
    Entry e;
    e.name = name;
    e.offset = uint32_t(size());
    dosTime(modified, e.time, e.date);
    // Its CRC-32 and sizes as zeros for now: end() puts them in.
    std::string header;
    le32(header, kLocalHeader);
    le16(header, kVersion);
    le16(header, kUtf8);
    le16(header, kDeflated);
    le16(header, e.time);
    le16(header, e.date);
    le32(header, 0);
    le32(header, 0);
    le32(header, 0);
    le16(header, uint16_t(name.size()));
    le16(header, 0);
    header += name;
    if (!put(header.data(), header.size())) return false;
    auto* z = new z_stream{};
    // Raw deflate (no zlib header: windowBits -15), as a ZIP has it; zlib's default level.
    if (deflateInit2(z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        delete z;
        return fail("deflate could not be started");
    }
    stream_ = z;
    crc_ = uint32_t(crc32(0, nullptr, 0));
    in_ = out_ = 0;
    entries_.push_back(std::move(e));
    return true;
}

bool ZipWriter::write(const void* data, size_t size) {
    if (!ok()) return false;
    if (!stream_) return fail("write without an entry begun");
    z_stream* z = static_cast<z_stream*>(stream_);
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size) {
        // (zlib counts in uInt: a piece of at most 1 GB at a time.)
        const uInt n = uInt(std::min<size_t>(size, 1u << 30));
        crc_ = uint32_t(crc32(crc_, bytes, n));
        z->next_in = const_cast<unsigned char*>(bytes);
        z->avail_in = n;
        if (!deflateOut(Z_NO_FLUSH)) return false;
        in_ += n;
        bytes += n;
        size -= n;
    }
    if (in_ > 0xffffffffull) return fail("an entry would pass 4 GB");
    return true;
}

bool ZipWriter::end() {
    if (!ok()) return false;
    if (!stream_) return true;
    z_stream* z = static_cast<z_stream*>(stream_);
    z->next_in = nullptr;
    z->avail_in = 0;
    const bool finished = deflateOut(Z_FINISH);
    deflateEnd(z);
    delete z;
    stream_ = nullptr;
    if (!finished) return false;
    if (in_ > 0xffffffffull || out_ > 0xffffffffull) return fail("an entry would pass 4 GB");
    Entry& e = entries_.back();
    e.crc = crc_;
    e.compressed = uint32_t(out_);
    e.size = uint32_t(in_);
    // Into the local header, 14 bytes on: the CRC-32, the compressed size, the size.
    std::string sums;
    le32(sums, e.crc);
    le32(sums, e.compressed);
    le32(sums, e.size);
    if (!flush()) return false;
    LARGE_INTEGER at{}, back{};
    at.QuadPart = LONGLONG(e.offset) + 14;
    back.QuadPart = LONGLONG(written_);
    DWORD done = 0;
    if (!SetFilePointerEx(file_, at, nullptr, FILE_BEGIN) || !WriteFile(file_, sums.data(), DWORD(sums.size()), &done, nullptr) ||
        done != sums.size() || !SetFilePointerEx(file_, back, nullptr, FILE_BEGIN))
        return fail("an entry's header could not be completed (error " + std::to_string(GetLastError()) + ")");
    return true;
}

bool ZipWriter::add(const std::string& name, std::string_view data, uint64_t modified) {
    return begin(name, modified) && write(data.data(), data.size()) && end();
}

bool ZipWriter::finish(const std::string& comment) {
    if (!ok() || !end()) return false;
    const uint64_t start = size();
    std::string directory;
    for (const Entry& e : entries_) {
        le32(directory, kCentralHeader);
        le16(directory, kVersion);  // made by: MS-DOS (0) and 2.0, so the attributes below are DOS's
        le16(directory, kVersion);
        le16(directory, kUtf8);
        le16(directory, kDeflated);
        le16(directory, e.time);
        le16(directory, e.date);
        le32(directory, e.crc);
        le32(directory, e.compressed);
        le32(directory, e.size);
        le16(directory, uint16_t(e.name.size()));
        le16(directory, 0);  // extra field
        le16(directory, 0);  // comment
        le16(directory, 0);  // disk
        le16(directory, 0);  // internal attributes
        le32(directory, FILE_ATTRIBUTE_ARCHIVE);
        le32(directory, e.offset);
        directory += e.name;
    }
    if (!put(directory.data(), directory.size())) return false;
    if (start > 0xffffffffull || directory.size() > 0xffffffffull) return fail("the ZIP would pass 4 GB");
    const std::string note = comment.substr(0, 0xffff);
    std::string tail;
    le32(tail, kEnd);
    le16(tail, 0);
    le16(tail, 0);
    le16(tail, uint16_t(entries_.size()));
    le16(tail, uint16_t(entries_.size()));
    le32(tail, uint32_t(directory.size()));
    le32(tail, uint32_t(start));
    le16(tail, uint16_t(note.size()));
    tail += note;
    if (!put(tail.data(), tail.size()) || !flush()) return false;
    const bool closed = CloseHandle(file_) != 0;
    file_ = INVALID_HANDLE_VALUE;
    return closed || fail("the file could not be closed");
}

} // namespace megler::app
