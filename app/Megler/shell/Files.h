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
