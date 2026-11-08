// Files in the app's own folders (LocalState, the install folder), through the
// Win32 calls an app container has: CreateFile2 and friends. Paths are UTF-16
// with backslashes, as WinRT hands them out.
//
// Every function here refuses a path that is not absolute (ERROR_BAD_PATHNAME;
// the first such path said in the log): one made from a folder Windows gave
// none of -- the shared folder, which a package on a memory card may have none
// of -- would be the current drive's root's ("" + L"\\apks\\..."). removeTree
// refuses a drive's or a volume's root itself.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace megler::app::files {

/// What a long copy (copyWhole) is doing: copying, putting the copy on the disk, comparing it with what it was made from.
enum class Stage { copying, flushing, comparing };
/// Told how far a long copy is, as it goes: `done` of `total` bytes of the stage. From the copying thread.
using Progress = std::function<void(Stage stage, uint64_t done, uint64_t total)>;

bool exists(const std::wstring& path);
uint64_t size(const std::wstring& path);
bool makeDirectory(const std::wstring& path);
/// The folder and those it is in that are not there yet.
bool makeDirectories(const std::wstring& path);

/// The whole file; false when it cannot be opened.
bool read(const std::wstring& path, std::string& out);
/// At most `n` bytes of the file from `at` (fewer at its end); false when it cannot be opened or read. Another process may be
/// writing it meanwhile: a log being appended to.
bool readRange(const std::wstring& path, uint64_t at, uint64_t n, std::string& out);
/// When the file was last written, as a FILETIME's 100 ns since 1601 (UTC); 0 for none.
uint64_t lastWrite(const std::wstring& path);
/// Creates or replaces the file.
bool write(const std::wstring& path, std::string_view data);
/// Opens, appends and closes: the Device Portal refuses to copy a file some
/// process holds open for writing, so nothing a phone run is read back
/// through (the logs) is ever kept open.
bool append(const std::wstring& path, std::string_view data);
bool remove(const std::wstring& path);
/// Renames, replacing what is there.
bool move(const std::wstring& from, const std::wstring& to);
/// Copies, replacing what is there: CopyFile2, or, where it is refused for what it copies besides the bytes (a memory card's
/// FAT32 or exFAT has no encryption, extended attributes or other streams), the bytes read and written, the time of writing
/// kept; a file Windows encrypted (an app's on a memory card) so at once, so that the copy is plain. Each said once.
bool copy(const std::wstring& from, const std::wstring& to);
/// What is written of the file put on the disk now (FlushFileBuffers), not when Windows gets to it: a phone that restarts by
/// itself keeps a file's name and size and may lose what was not on the disk yet.
bool flush(const std::wstring& path);
/// Whether two files are of one size and end with the same `bytes`: a copy that never finished is not (Windows gives it
/// the whole size first, and what was never written of it reads as zeros).
bool sameEnd(const std::wstring& a, const std::wstring& b, uint32_t bytes);
/// Whether two files hold the same bytes: of one size, then compared a piece at a time, neither read whole into memory.
/// False when either cannot be read.
bool same(const std::wstring& a, const std::wstring& b);
/// Whether `copy` now holds `source`'s bytes, every 1 MB chunk of it compared and those that differ written again from
/// `source` (read twice alike first), put on the disk and read back; `mended`: how many bytes were written so, and
/// `chunks`, when given, the offset of each chunk that was. A Lumia 650's copies of Google Play services' 277 MB APK came
/// out twice with a byte wrong in their middle, each time in a DEX file of it that no app could then use (packages 625 and
/// 632). Both are read as the disk holds them (DiskFile): a page Windows still has in memory from the writing reads right
/// whatever went to the disk, and hundreds of megabytes read through Windows's cache push out of a phone's memory what
/// everything else had in it (a Lumia 630 has 512 MB). `progress`, when given, is told each chunk compared.
bool mend(const std::wstring& source, const std::wstring& copy, uint64_t& mended, std::vector<uint64_t>* chunks = nullptr,
          const Progress& progress = nullptr);
/// `from` copied to `to` (replacing it), put on the disk, and made byte for byte `from`'s (mend; `mended` as there): how
/// every copy of an APK is made that apps run from. A file of unbufferedFrom bytes or more is copied past Windows's cache
/// (COPY_FILE_NO_BUFFERING), as a copy of hundreds of megabytes should be on a phone of 512 MB, and again through it should
/// that be refused (and read and written where CopyFile2 is refused, as copy). `progress`, when given, is told how far each
/// stage is. On failure GetLastError() says why.
bool copyWhole(const std::wstring& from, const std::wstring& to, uint64_t& mended, const Progress& progress = nullptr);
/// The size from which copyWhole copies past Windows's cache.
constexpr uint64_t unbufferedFrom = 8u << 20;

/// A file read as the disk holds it, past Windows's cache of it (FILE_FLAG_NO_BUFFERING: whole sectors, into memory of
/// its own aligned to a page); as any file where the file system will not open it so, or opens it so and then refuses its
/// reads (said once).
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
    std::wstring path_;  // (opened again through the cache where the file system refuses the uncached reads)
};

/// The names (not paths) of the files in `directory` matching `pattern` ("*.apk").
std::vector<std::wstring> list(const std::wstring& directory, const std::wstring& pattern);
/// The same for its folders.
std::vector<std::wstring> listFolders(const std::wstring& directory, const std::wstring& pattern);
/// A folder with everything in it, its own folders too; false when something of it could not be removed.
bool removeTree(const std::wstring& path);
/// The sizes of the files in a folder (not in its folders), added up; 0 for no folder.
uint64_t folderSize(const std::wstring& path);

/// What a PC's NTFS never refuses and a memory card's file system may, for the tests of the ways round it
/// (core/tests/mg_shell_windows.cpp): every CopyFile2 refused with `error`, every read past the cache (DiskFile) with
/// `error`; 0 for none, as they are.
namespace testing {
void refuseCopyFile2(uint32_t error);
void refuseUncachedReads(uint32_t error);
} // namespace testing

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
