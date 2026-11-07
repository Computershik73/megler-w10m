// A ZIP file written as it goes: the one file of every package's logs that Save All Logs makes (shell/LogArchive.h).
//
// Each entry is deflated by the zlib Megler.exe carries anyway (the core's, core/third_party/zlib; on desktop Windows in
// Megler.exe's half of the core), its local header written first and its CRC-32 and sizes put into it once the entry has
// ended; then the central directory and its end. Names are UTF-8 (the general purpose flag's bit 11) with '/' between
// folders, times DOS's in local time. No ZIP64: an entry and the whole file stay under 4 GB, which logs of at most 16 MB a
// file are far from, and a call that would pass it fails. The file is written by the plain file functions, so it is one of
// the app's own folders (LocalState); a phone's Pictures library is not theirs, and the finished ZIP is copied there.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace megler::app {

class ZipWriter {
public:
    /// The file made at `path`, replacing one there; ok() says whether it could be.
    explicit ZipWriter(const std::wstring& path);
    /// Closes the file; one not finished is no ZIP.
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool ok() const { return error_.empty(); }
    /// Why it failed; every call after a failure fails at once.
    const std::string& error() const { return error_; }

    /// An entry begun (the one before ended): `name` as "Megler/megler.log", `modified` its time as a FILETIME is, 100 ns
    /// since 1601 in UTC (0: now).
    bool begin(const std::string& name, uint64_t modified = 0);
    /// Bytes of the entry begun, in as many pieces as it takes.
    bool write(const void* data, size_t size);
    /// The entry ended: deflate's last bytes written, its CRC-32 and sizes put into its local header.
    bool end();
    /// A whole entry at once.
    bool add(const std::string& name, std::string_view data, uint64_t modified = 0);
    /// The central directory and its end written, with `comment` as the ZIP's own, and the file closed: a ZIP.
    bool finish(const std::string& comment = {});

    /// What the file holds so far, in bytes.
    uint64_t size() const { return written_ + buffer_.size(); }
    /// The entries begun so far.
    size_t entries() const { return entries_.size(); }

private:
    struct Entry {
        std::string name;
        uint32_t offset = 0;      // of its local header
        uint32_t crc = 0, compressed = 0, size = 0;
        uint16_t time = 0, date = 0;
    };
    bool fail(std::string why);
    /// To the file through a buffer of 256 KB.
    bool put(const void* data, size_t size);
    bool flush();
    /// Runs deflate over what it has been given, with `mode` (Z_NO_FLUSH, Z_FINISH), and puts out what it makes.
    bool deflateOut(int mode);

    void* file_;                  // the HANDLE
    std::string error_;
    std::vector<Entry> entries_;
    std::string buffer_;
    uint64_t written_ = 0;        // bytes in the file, the buffer's not counted
    void* stream_ = nullptr;      // zlib's z_stream, while an entry is open
    uint32_t crc_ = 0;
    uint64_t in_ = 0, out_ = 0;   // the open entry's bytes, as given and as deflated
    std::vector<unsigned char> chunk_;
};

} // namespace megler::app
