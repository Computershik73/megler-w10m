#include "pch.h"

#include "shell/Graphics.h"

#include "shell/Files.h"
#include "shell/Strings.h"

namespace megler::app::graphics {

const wchar_t* const mesaFiles[3] = {L"libgallium_wgl.dll", L"libGLESv2_mesa.dll", L"libEGL_mesa.dll"};

bool isMesaFile(const std::wstring& name) {
    for (const wchar_t* file : mesaFiles)
        if (_wcsicmp(name.c_str(), file) == 0) return true;
    return false;
}

const wchar_t* const mesaLicenses = L"mesa-licenses";

bool isMesaFolder(const std::wstring& name) { return _wcsicmp(name.c_str(), mesaLicenses) == 0; }

bool hasMesaFiles(const std::wstring& folder, std::string* missing) {
    std::string lacking;
    for (const wchar_t* file : mesaFiles)
        if (folder.empty() || !files::exists(folder + L"\\" + file)) lacking += (lacking.empty() ? "" : ", ") + narrow(file);
    if (missing) *missing = lacking;
    return lacking.empty();
}

std::string packageDefault(const std::wstring& installFolder) {
    std::string text;
    if (installFolder.empty() || !files::read(installFolder + L"\\gles-default.txt", text)) return "angle";
    return text.rfind("mesa", 0) == 0 ? "mesa" : "angle";
}

std::string resolve(const std::optional<std::string>& own, const std::optional<std::string>& megler, const std::string& fallback,
                    bool mesaChoosable) {
    // (An empty value is none: a setting written back to nothing goes by the next.)
    const std::string chosen = own && !own->empty() ? *own : megler && !megler->empty() ? *megler : fallback;
    return mesaChoosable && chosen == "mesa" ? "mesa" : "angle";
}

Start atStart(const std::string& chosen, bool packageHasMesa, const std::string& missing, bool ownPackage) {
    Start s;
    s.gles = chosen == "mesa" && packageHasMesa ? "mesa" : "angle";
    if (chosen == "mesa" && !packageHasMesa)
        s.why = ownPackage ? "the app is set to Mesa's freedreno, but its package was made while it went by ANGLE and has none of Mesa's "
                             "DLLs (" + missing + "): its package is to be made again with them -- the app's Update in Megler's Library, "
                             "or Update All -- and until then it draws with ANGLE"
                           : "Mesa's freedreno is chosen, but this package has none of its DLLs (" + missing + "): ANGLE";
    return s;
}

namespace {

std::wstring recordPath(const std::wstring& shared, const std::string& identity) {
    return shared.empty() || identity.empty() ? std::wstring() : shared + L"\\apks\\" + widen(identity) + L"\\graphics.txt";
}

/// The record's lines, "<full name> <mesa|angle>", as they are.
std::vector<std::string> recordLines(const std::wstring& path) {
    std::vector<std::string> out;
    std::string text;
    if (path.empty() || !files::read(path, text)) return out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

}  // namespace

bool record(const std::wstring& shared, const std::string& identity, const std::string& fullName, bool mesa) {
    const std::wstring path = recordPath(shared, identity);
    if (path.empty() || fullName.empty()) return false;
    // (The full name's earlier line, of a package made again with that name and not installed, goes: the last one counts.)
    std::vector<std::string> kept;
    for (const std::string& line : recordLines(path))
        if (line.rfind(fullName + " ", 0) != 0) kept.push_back(line);
    kept.push_back(fullName + (mesa ? " mesa" : " angle"));
    if (kept.size() > 16) kept.erase(kept.begin(), kept.end() - 16);
    std::string text;
    for (const std::string& line : kept) text += line + "\r\n";
    files::makeDirectories(path.substr(0, path.find_last_of(L'\\')));
    return files::write(path + L".new", text) && files::move(path + L".new", path);
}

bool recordedMesa(const std::wstring& shared, const std::string& identity, const std::string& fullName) {
    bool mesa = false;
    for (const std::string& line : recordLines(recordPath(shared, identity)))
        if (line.rfind(fullName + " ", 0) == 0) mesa = line.substr(fullName.size() + 1) == "mesa";
    return mesa;
}

}  // namespace megler::app::graphics
