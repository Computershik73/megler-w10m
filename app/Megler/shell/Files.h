// Files in the app's own folders (LocalState, the install folder), through the
// Win32 calls an app container has: CreateFile2 and friends. Paths are UTF-16
// with backslashes, as WinRT hands them out.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace megler::app::files {

bool exists(const std::wstring& path);
uint64_t size(const std::wstring& path);
bool makeDirectory(const std::wstring& path);
/// The folder and those it is in that are not there yet.
bool makeDirectories(const std::wstring& path);

/// The whole file; false when it cannot be opened.
bool read(const std::wstring& path, std::string& out);
/// Creates or replaces the file.
bool write(const std::wstring& path, std::string_view data);
/// Opens, appends and closes: the Device Portal refuses to copy a file some
/// process holds open for writing, so nothing a phone run is read back
/// through (the logs) is ever kept open.
bool append(const std::wstring& path, std::string_view data);
bool remove(const std::wstring& path);
/// Renames, replacing what is there.
bool move(const std::wstring& from, const std::wstring& to);
/// Copies, replacing what is there.
bool copy(const std::wstring& from, const std::wstring& to);
/// What is written of the file put on the disk now (FlushFileBuffers), not when Windows gets to it: a phone that restarts by
/// itself keeps a file's name and size and may lose what was not on the disk yet.
bool flush(const std::wstring& path);
/// Whether two files are of one size and end with the same `bytes`: a copy that never finished is not (Windows gives it
/// the whole size first, and what was never written of it reads as zeros).
bool sameEnd(const std::wstring& a, const std::wstring& b, uint32_t bytes);
/// Whether `copy` now holds `source`'s bytes, every 1 MB chunk of it compared and those that differ written again from
/// `source` (read twice alike first), put on the disk and read back; `mended`: how many bytes were written so, and
/// `chunks`, when given, the offset of each chunk that was. A Lumia 650's copies of Google Play services' 277 MB APK came
/// out twice with a byte wrong in their middle, each time in a DEX file of it that no app could then use (packages 625 and
/// 632). The copy is read as the disk holds it (DiskFile): a page Windows still has in memory from the writing reads right
/// whatever went to the disk.
bool mend(const std::wstring& source, const std::wstring& copy, uint64_t& mended, std::vector<uint64_t>* chunks = nullptr);
/// `from` copied to `to` (replacing it), put on the disk, and made byte for byte `from`'s (mend; `mended` as there): how
/// every copy of an APK is made that apps run from.
bool copyWhole(const std::wstring& from, const std::wstring& to, uint64_t& mended);

/// A file read as the disk holds it, past Windows's cache of it (FILE_FLAG_NO_BUFFERING: whole sectors, into memory of
/// its own aligned to a page); as any file where the file system will not open it so.
class DiskFile {
public:
    explicit DiskFile(const std::wstring& path);
    ~DiskFile();
    DiskFile(const DiskFile&) = delete;
    DiskFile& operator=(const DiskFile&) = delete;
    bool isOpen() const;
    /// Whether the disk itself is read (else Windows's cache may answer).
    bool uncached() const { return uncached_; }
    uint64_t size() const { return size_; }
    /// `n` bytes at `at` (a multiple of 4096), at most a chunk and within the file.
    bool read(uint64_t at, char* out, uint32_t n);
    static constexpr uint32_t chunk = 1u << 20;

private:
    void* handle_;
    bool uncached_ = false;
    uint64_t size_ = 0;
    char* buffer_ = nullptr;
};

/// The names (not paths) of the files in `directory` matching `pattern` ("*.apk").
std::vector<std::wstring> list(const std::wstring& directory, const std::wstring& pattern);
/// The same for its folders.
std::vector<std::wstring> listFolders(const std::wstring& directory, const std::wstring& pattern);
/// A folder with everything in it, its own folders too; false when something of it could not be removed.
bool removeTree(const std::wstring& path);
/// The sizes of the files in a folder (not in its folders), added up; 0 for no folder.
uint64_t folderSize(const std::wstring& path);

/// One line per call into a file started afresh, each stamped with the time
/// of day: the probe's, the self-test's and the unattended run's logs.
class LineLog {
public:
    explicit LineLog(std::wstring path);
    void line(const std::string& text);
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

} // namespace megler::app::files
