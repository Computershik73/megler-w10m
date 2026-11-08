#include "pch.h"

#include "shell/SharedApks.h"

#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace megler::app::sharedApks {

namespace {

/// An hour in a FILETIME's 100 ns.
constexpr uint64_t kHour = 36000000000ull;

bool sameName(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool sameFullName(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

/// A folder being copied (keepInShared copies into <token>.part and renames it), or left so by a copy that never finished:
/// no package reads it.
bool partial(const Token& t) { return t.name.size() > 5 && _wcsicmp(t.name.c_str() + t.name.size() - 5, L".part") == 0; }

/// Whether made.txt names `fullName`.
bool madeAs(const Token& t, const std::string& fullName) {
    return std::any_of(t.made.begin(), t.made.end(), [&](const std::string& m) { return sameFullName(m, fullName); });
}

/// The non-empty lines of `text`, each without the spaces round it.
std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t first = 0;
        while (first < line.size() && (line[first] == ' ' || line[first] == '\t')) first++;
        if (first < line.size()) out.push_back(line.substr(first));
        at = end + 1;
    }
    return out;
}

/// A "key=value" line's value, or empty.
std::string valueOf(const std::string& text, const char* key) {
    const std::string k = std::string(key) + "=";
    for (const auto& line : lines(text))
        if (line.compare(0, k.size(), k) == 0) return line.substr(k.size());
    return {};
}

std::string where(const std::wstring& identity, const std::wstring& token) { return "apks\\" + narrow(identity) + "\\" + narrow(token); }

/// Said once a run: the same line again (a decision that has not changed, at each listing) is not said.
void sayOnce(const std::string& line) {
    static std::mutex mutex;
    static std::set<std::string> said;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!said.insert(line).second) return;
    }
    shellLog::write(line);
}

/// Written beside and moved into place: whoever reads it finds the whole of it, or what was there before.
bool writeWhole(const std::wstring& path, const std::string& text) {
    if (files::write(path + L".part", text) && files::move(path + L".part", path)) return true;
    const DWORD error = GetLastError();
    files::remove(path + L".part");
    SetLastError(error);
    return false;
}

} // namespace

uint64_t begunAt(const std::wstring& token) {
    if (token.size() < 16) return 0;
    uint64_t value = 0;
    for (size_t i = 0; i < 16; i++) {
        const wchar_t c = token[i];
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (digit < 0) return 0;
        value = value * 16 + uint64_t(digit);
    }
    return value;
}

Plan plan(const std::vector<App>& apps, const std::vector<Installed>& installed, uint64_t now) {
    Plan out;
    for (const App& app : apps) {
        const std::string identity = narrow(app.identity);
        // This app's packages installed now: one as a rule (Windows keeps one package of a family for a user).
        std::vector<std::string> names;
        for (const Installed& i : installed)
            if (sameName(widen(i.identity), app.identity)) names.push_back(i.fullName);
        if (names.empty() && (!app.installedToken.empty() || app.installedUnreadable)) out.forget.push_back(app.identity);

        // installed.txt's folder, while the app is installed and that folder is there: what Megler itself last installed it
        // from, after which installed.txt was written.
        const Token* byMegler = nullptr;
        if (!names.empty() && !app.installedToken.empty())
            for (const Token& t : app.tokens)
                if (!partial(t) && sameName(t.name, app.installedToken)) byMegler = &t;

        // For each package installed, the folders made for it -- those made before Megler itself installed the same package
        // from another folder set apart: that install replaced the package of the name that was there, and the file of a
        // package made before it is gone (each make writes LocalState\packages\<identity>.appx again, and Megler's install
        // removes it), so that nothing installed since was made with them.
        std::vector<std::vector<const Token*>> madeFor(names.size()), supersededFor(names.size());
        bool recognised = false;
        for (size_t n = 0; n < names.size(); n++) {
            for (const Token& t : app.tokens) {
                if (partial(t) || !madeAs(t, names[n])) continue;
                recognised = true;
                // (Where either time is not known, it is not.)
                const bool superseded = byMegler && &t != byMegler && madeAs(*byMegler, names[n]) && t.madeTime && app.installedTime &&
                                        t.madeTime < app.installedTime;
                (superseded ? supersededFor[n] : madeFor[n]).push_back(&t);
            }
        }
        // Where nothing names which folder is read -- an installed package made before made.txt was written, a waiting one
        // made before its record was -- the folders it may read: installed.txt's, and any begun (or made, where it says when)
        // after installed.txt was written, as one Windows's own installer installed after Megler's install would be; not one
        // from before that install, whose package it replaced. Every one where there is no installed.txt.
        const auto mayBeRead = [&](const Token& t) {
            if (partial(t)) return false;
            if (!byMegler || &t == byMegler) return true;
            return (t.recorded && t.madeTime ? t.madeTime : begunAt(t.name)) >= app.installedTime;
        };

        for (const Token& t : app.tokens) {
            Verdict v;
            v.identity = app.identity;
            v.token = t.name;
            std::string why;
            for (size_t n = 0; n < names.size() && why.empty(); n++)
                if (std::find(madeFor[n].begin(), madeFor[n].end(), &t) != madeFor[n].end())
                    why = "the installed " + names[n] + " was made with it" +
                          (madeFor[n].size() > 1 ? format(", as were %u other folders, and which of them it reads cannot be told: all are kept",
                                                          unsigned(madeFor[n].size() - 1))
                                                 : std::string());
            const uint64_t begun = begunAt(t.name);
            if (!why.empty()) {
            } else if (t.read) {
                why = "the library reads it";
            } else if (app.waiting && !app.waitingToken.empty() && sameName(app.waitingToken, t.name) && !partial(t)) {
                why = "the package made with it, " + app.waitingFullName + ", waits in LocalState\\packages for Windows's own installer";
            } else if (t.unreadable) {
                why = "its made.txt could not be read: what it was made for is not known";
            } else if (app.installedUnreadable && !names.empty()) {
                why = "installed.txt could not be read: which folder Megler installed " + identity + " from is not known";
            } else if (!names.empty() && !recognised && mayBeRead(t)) {
                const std::string before = "the installed " + names[0] + " was made before Megler wrote down which folder a package reads";
                if (&t == byMegler)
                    why = "installed.txt names it: Megler installed " + identity + " from it, and no package since says it reads another";
                else if (byMegler)
                    why = before + ", and it may read this one, begun after Megler's own install of it from " + narrow(byMegler->name);
                else
                    why = before + ", and was not installed by Megler (installed.txt names none here): it may read this one";
            } else if (app.waiting && app.waitingToken.empty() && mayBeRead(t)) {
                why = "a package made before Megler wrote down which folder a package reads waits in LocalState\\packages for Windows's own "
                      "installer, and it may read this one";
            } else if (begun && (begun > now || now - begun < kHour)) {
                why = "begun within the hour: a package may be being made with it";
            }
            if (!why.empty()) {
                v.keep = true;
                v.why = why;
                out.verdicts.push_back(std::move(v));
                continue;
            }
            v.keep = false;
            for (size_t n = 0; n < names.size() && why.empty(); n++)
                if (std::find(supersededFor[n].begin(), supersededFor[n].end(), &t) != supersededFor[n].end())
                    why = "made for " + names[n] + " before Megler installed that package from " + narrow(byMegler->name);
            if (!why.empty()) {
            } else if (partial(t)) {
                why = "a copy that never finished";
            } else if (names.empty()) {
                why = identity + " is not installed, no package made of it waits, and the library does not read it";
            } else if (recognised) {
                why = "the installed " + names[0] + " was not made with it, no package made with it waits, and the library does not read it";
            } else {
                why = "begun before Megler installed " + identity + " from " + (byMegler ? narrow(byMegler->name) : std::string("another folder")) +
                      ", which replaced its package, and the library does not read it";
            }
            v.why = why;
            out.verdicts.push_back(std::move(v));
        }

        // The installed package whose folder is known for sure: the one left made for it.
        if (names.size() == 1 && madeFor[0].size() == 1) {
            Known k;
            k.identity = app.identity;
            k.token = madeFor[0][0]->name;
            k.fullName = names[0];
            k.waitingDone = app.waiting && sameName(app.waitingToken, k.token) && sameFullName(app.waitingFullName, k.fullName);
            out.known.push_back(std::move(k));
        }
    }
    return out;
}

std::vector<App> look(const std::wstring& shared, const std::wstring& packages, const std::vector<std::wstring>& read) {
    std::vector<App> apps;
    if (shared.empty()) return apps;
    const std::wstring apks = shared + L"\\apks";
    for (const auto& identity : files::listFolders(apks, L"*")) {
        App app;
        app.identity = identity;
        const std::wstring dir = apks + L"\\" + identity;
        std::string text;
        const std::wstring installedTxt = dir + L"\\installed.txt";
        if (files::read(installedTxt, text)) {
            const auto said = lines(text);
            if (!said.empty()) app.installedToken = widen(said.front());
            app.installedTime = files::lastWrite(installedTxt);
        } else if (files::exists(installedTxt)) {
            app.installedUnreadable = true;
        }
        for (const auto& name : files::listFolders(dir, L"*")) {
            Token t;
            t.name = name;
            const std::wstring folder = dir + L"\\" + name, record = folder + L"\\made.txt";
            if (files::read(record, text)) {
                t.recorded = true;
                t.made = lines(text);
                t.madeTime = files::lastWrite(record);
            } else if (files::exists(record)) {
                t.unreadable = true;
            }
            t.read = std::any_of(read.begin(), read.end(), [&](const std::wstring& r) { return sameName(r, folder); });
            app.tokens.push_back(std::move(t));
        }
        if (!packages.empty()) {
            const std::wstring appx = packages + L"\\" + identity + L".appx";
            if (files::exists(appx)) {
                app.waiting = true;
                // Its record names a folder of this app's: apks\<identity>\<token>.
                if (files::read(appx + L".txt", text)) {
                    const std::wstring apk = widen(valueOf(text, "apk")), prefix = L"apks\\" + identity + L"\\";
                    if (apk.size() > prefix.size() && _wcsnicmp(apk.c_str(), prefix.c_str(), prefix.size()) == 0 &&
                        apk.find(L'\\', prefix.size()) == std::wstring::npos) {
                        app.waitingToken = apk.substr(prefix.size());
                        app.waitingFullName = valueOf(text, "package");
                    }
                }
            }
        }
        apps.push_back(std::move(app));
    }
    return apps;
}

void apply(const Plan& plan, const std::wstring& shared, const std::wstring& packages) {
    if (shared.empty()) return;
    const std::wstring apks = shared + L"\\apks";
    for (const auto& identity : plan.forget) files::remove(apks + L"\\" + identity + L"\\installed.txt");
    for (const Known& k : plan.known) {
        const std::wstring record = apks + L"\\" + k.identity + L"\\installed.txt";
        std::string text;
        const auto said = files::read(record, text) ? lines(text) : std::vector<std::string>();
        if (said.empty() || !sameName(widen(said.front()), k.token)) {
            if (writeWhole(record, narrow(k.token)))
                shellLog::write("app package: apks\\" + narrow(k.identity) + "\\installed.txt names " + narrow(k.token) + " now: the installed " +
                                k.fullName + " was made with it, and installed by another than Megler (Windows's own installer, a file "
                                "manager, or Windows after Megler had given up on it)" +
                                (said.empty() ? std::string() : "; it named " + said.front()));
            else
                sayOnce(format("app package: apks\\%s\\installed.txt could not be written (error %lu): the folder is kept all the same, "
                               "its package's full name being in its made.txt",
                               narrow(k.identity).c_str(), GetLastError()));
        }
        if (k.waitingDone && !packages.empty()) {
            const std::wstring appx = packages + L"\\" + k.identity + L".appx";
            removeWaiting(appx);
            if (!files::exists(appx))
                shellLog::write("app package: " + narrow(k.identity) +
                                ".appx, made and not installed by Megler, is the package installed now: removed from LocalState\\packages");
        }
    }
    uint64_t freed = 0;
    unsigned removed = 0, open = 0;
    for (const Verdict& v : plan.verdicts) {
        const std::wstring folder = apks + L"\\" + v.identity + L"\\" + v.token;
        if (v.keep) {
            sayOnce("app package: " + where(v.identity, v.token) + " kept: " + v.why);
            continue;
        }
        // The APK first: one an app has open stays, and so does the rest of its folder, made.txt with it, for the next time.
        const uint64_t size = files::size(folder + L"\\app.apk") + files::folderSize(folder + L"\\app.apk.splits");
        if (files::exists(folder + L"\\app.apk") && !files::remove(folder + L"\\app.apk") && files::exists(folder + L"\\app.apk")) {
            open++;
            sayOnce(format("app package: %s could not be removed, its APK being open in an app (error %lu): left for the next time; ",
                           where(v.identity, v.token).c_str(), GetLastError()) +
                    v.why);
            continue;
        }
        if (files::removeTree(folder)) {
            freed += size;
            removed++;
            shellLog::write("app package: " + where(v.identity, v.token) + " removed (" + formattedByteCount(size) + "): " + v.why);
        } else {
            open++;
            sayOnce(format("app package: %s could not all be removed (error %lu): left for the next time; ", where(v.identity, v.token).c_str(),
                           GetLastError()) +
                    v.why);
        }
    }
    // An app's folder with nothing left in it goes too (RemoveDirectory takes only an empty one).
    for (const auto& identity : files::listFolders(apks, L"*")) RemoveDirectoryW((apks + L"\\" + identity).c_str());
    // (Folders still open alone are said once: a running app may hold one through many listings.)
    const std::string summary = format("app package: %u folders of apps' APKs that nothing reads removed from the shared folder, %s", removed,
                                       formattedByteCount(freed).c_str()) +
                                (open ? format("; %u still open in an app, left for the next time", open) : std::string());
    if (removed)
        shellLog::write(summary);
    else if (open)
        sayOnce(summary);
}

bool recordMade(const std::wstring& folder, const std::string& fullName) {
    const std::wstring record = folder + L"\\made.txt";
    std::string text;
    // (What cannot be read is not written over: the full names in it would be lost.)
    if (files::exists(record) && !files::read(record, text)) return false;
    std::string all;
    bool there = false;
    for (const auto& line : lines(text)) {
        there = there || sameFullName(line, fullName);
        all += line + "\r\n";
    }
    if (!there) all += fullName + "\r\n";
    return writeWhole(record, all);
}

bool recordWaiting(const std::wstring& appx, const std::wstring& apkFolder, const std::string& fullName) {
    return writeWhole(appx + L".txt", "apk=" + narrow(apkFolder) + "\r\npackage=" + fullName + "\r\n");
}

void removeWaiting(const std::wstring& appx) {
    files::remove(appx);
    if (!files::exists(appx)) files::remove(appx + L".txt");
}

} // namespace megler::app::sharedApks
