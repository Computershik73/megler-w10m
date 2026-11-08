#include "pch.h"

#include "shell/Placement.h"

#include "shell/Files.h"
#include "shell/Strings.h"

namespace megler::app::placement {

namespace {

/// `path` as names are compared: '/' made '\', lower case, "\\?\" before a drive taken off, no separator at the end but a
/// drive's root's own ("c:\").
std::wstring normal(std::wstring path) {
    for (wchar_t& c : path) c = c == L'/' ? L'\\' : wchar_t(towlower(c));
    if (path.size() >= 6 && path.compare(0, 4, L"\\\\?\\") == 0 && iswalpha(path[4]) && path[5] == L':') path.erase(0, 4);
    while (!path.empty() && path.back() == L'\\' && !(path.size() == 3 && path[1] == L':')) path.pop_back();
    return path;
}

/// Whether `path` is `folder` or in it, both normal.
bool within(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty() || path.size() < folder.size() || path.compare(0, folder.size(), folder) != 0) return false;
    return path.size() == folder.size() || folder.back() == L'\\' || path[folder.size()] == L'\\';
}

/// Of `volumes`, the one whose folder (`storePath` or `mountPoint`) `path` is in, the longest; -1 for none.
int longestHolding(const std::vector<Volume>& volumes, const std::wstring& path, std::wstring Volume::*folder) {
    int best = -1;
    size_t length = 0;
    for (size_t i = 0; i < volumes.size(); i++) {
        const std::wstring& named = volumes[i].*folder;
        if (driveOf(named).empty()) continue;
        const std::wstring holder = normal(named);
        if (within(path, holder) && holder.size() > length) {
            best = int(i);
            length = holder.size();
        }
    }
    return best;
}

std::string shownDrive(const std::wstring& path) {
    const std::wstring drive = driveOf(path);
    return drive.empty() ? std::string("no drive") : narrow(drive);
}

} // namespace

std::wstring driveOf(const std::wstring& path) {
    std::wstring p = path;
    for (wchar_t& c : p)
        if (c == L'/') c = L'\\';
    const auto letter = [](wchar_t c) { return std::wstring(1, wchar_t(towupper(c))) + L":"; };
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\') return letter(p[0]);
    if (p.size() >= 4 && (p.compare(0, 4, L"\\\\?\\") == 0 || p.compare(0, 4, L"\\\\.\\") == 0)) {
        if (p.size() >= 6 && iswalpha(p[4]) && p[5] == L':') return letter(p[4]);
        // "\\?\UNC\server\share\..." is the share's.
        if (p.size() > 8 && _wcsnicmp(p.c_str() + 4, L"UNC\\", 4) == 0) {
            const size_t server = p.find(L'\\', 8), share = server == std::wstring::npos ? server : p.find(L'\\', server + 1);
            return server == std::wstring::npos || server == 8 ? std::wstring() : L"\\\\" + p.substr(8, share == std::wstring::npos ? share : share - 8);
        }
        // "\\?\Volume{...}\...": the volume's own name.
        const size_t end = p.find(L'\\', 4);
        return end == 4 ? std::wstring() : p.substr(0, end);
    }
    if (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'\\') {
        const size_t server = p.find(L'\\', 2);
        if (server == std::wstring::npos || server + 1 >= p.size()) return {};
        return p.substr(0, p.find(L'\\', server + 1));
    }
    return {};
}

int volumeHolding(const std::vector<Volume>& volumes, const std::wstring& installFolder, const std::wstring& installVolume) {
    if (driveOf(installFolder).empty()) return -1;
    const std::wstring folder = normal(installFolder);
    int found = longestHolding(volumes, folder, &Volume::storePath);
    if (found >= 0) return found;
    const std::wstring own = normal(installVolume);
    if (!own.empty())
        for (size_t i = 0; i < volumes.size(); i++)
            if (!volumes[i].name.empty() && normal(volumes[i].name) == own) return int(i);
    return longestHolding(volumes, folder, &Volume::mountPoint);
}

std::wstring volumeOf(const std::wstring& folder) {
    if (driveOf(folder).empty()) return {};
    CREATEFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS;  // (the one way a folder is opened)
    HANDLE h = CreateFile2(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, &params);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring path(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, path.data(), DWORD(path.size()), VOLUME_NAME_GUID);
    if (n >= path.size()) {
        path.assign(size_t(n) + 1, L'\0');
        n = GetFinalPathNameByHandleW(h, path.data(), DWORD(path.size()), VOLUME_NAME_GUID);
    }
    CloseHandle(h);
    if (!n || n >= path.size()) return {};
    path.resize(n);
    // "\\?\Volume{...}\the\folder"
    if (_wcsnicmp(path.c_str(), L"\\\\?\\Volume{", 11) != 0) return {};
    return path.substr(0, path.find(L'\\', 4));
}

bool onSystemDrive(const std::wstring& folder) {
    wchar_t system[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring a = driveOf(folder), b = n && n < MAX_PATH ? driveOf(system) : std::wstring();
    return a.empty() || b.empty() || _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::string summary(const std::wstring& install, const std::wstring& local, const std::wstring& shared) {
    const std::wstring volume = volumeOf(install);
    wchar_t system[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    return "the install folder on " + shownDrive(install) + (volume.empty() ? std::string() : " (" + narrow(volume) + ")") + ", LocalState on " +
           shownDrive(local) +
           (shared.empty() ? std::string(", no shared folder (Windows gave this package none)")
                           : ", the shared folder on " + shownDrive(shared) + " (" + narrow(shared) + ")") +
           "; Windows on " + (n && n < MAX_PATH ? shownDrive(system) : std::string("a drive it does not say"));
}

Missing missingFor(const std::wstring& shared, const std::wstring& apk, const std::wstring& androidRoot) {
    if (shared.empty()) return Missing::sharedFolder;
    std::string head;
    if (apk.empty() || !files::readRange(apk, 0, 4, head) || head != std::string("PK\3\4", 4)) return Missing::apk;
    std::string complete;
    if (!androidRoot.empty() && !files::read(androidRoot + L"\\complete", complete)) return Missing::androidFiles;
    return Missing::nothing;
}

std::string missingText(Missing missing, const std::string& label, const std::wstring& shared, bool mobile) {
    const std::string card = mobile ? "the memory card" : "another drive";
    const std::string why = "As a rule, that is because the app and Megler are on different storage -- one on " + card + ", the other in the " +
                            (mobile ? "phone's memory" : "system drive") + " -- and Windows gives each storage a shared folder of its own. ";
    const std::string what = mobile ? "Keep the app and Megler on the same storage: move the app (or Megler) to the phone's memory in Settings › "
                                      "System › Storage (the memory card, Apps & games, the app, Move), or install the app again from Megler's "
                                      "Library."
                                    : "Keep the app and Megler on the same drive: move the app (or Megler) to the system drive in Settings › Apps "
                                      "(the app, Move), or install the app again from Megler's Library.";
    switch (missing) {
    case Missing::nothing:
        return {};
    case Missing::sharedFolder:
        return label + " can't start: Windows gave it no shared folder, which is where Megler keeps Android's files and the app's APK for "
                       "it. That happens when the app is installed on " + card + ". " + what;
    case Missing::apk:
        return label + " can't start: its APK is not in its shared folder, or can't be read there (" + narrow(shared) + "). " + why + what;
    case Missing::androidFiles:
        return label + " can't start: Android's files, which Megler puts in the shared folder for its apps, are not there (" + narrow(shared) +
               "). " + why + what;
    }
    return {};
}

} // namespace megler::app::placement
