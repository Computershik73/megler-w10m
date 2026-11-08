#include "pch.h"

#include "shell/LogMirror.h"

#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>

namespace megler::app::logMirror {

namespace {

constexpr auto kPeriod = std::chrono::seconds(30), kFirstPass = std::chrono::seconds(10);
constexpr uint64_t kPiece = 1u << 20;  // what is read and appended at a time

std::mutex gMutex;
std::condition_variable gWake;
bool gStarted = false;
uint64_t gAsked = 0, gAnswered = 0;  // now()'s asks, and those a pass has answered

// The mirror's thread's own.
std::wstring gLocal, gDir;
uint64_t gCopied = 0;  // how much of LocalState's megler.log the folder's megler.log holds
bool gCapped = false;  // ... which stopped at capBytes
int gFailures = 0;

void fail(const std::string& what) {
    if (gFailures++ < 3) shellLog::write("logs: " + what + format(" (error %lu); the mirror goes on", GetLastError()));
}

std::wstring local(const std::wstring& name) { return gLocal + L"\\" + name; }
std::wstring mirrored(const std::wstring& name) { return gDir + L"\\" + name; }

/// `from`'s bytes [at, to) appended to `out`, a piece at a time.
bool appendRange(const std::wstring& from, const std::wstring& out, uint64_t at, uint64_t to) {
    std::string piece;
    while (at < to) {
        if (!files::readRange(from, at, std::min<uint64_t>(kPiece, to - at), piece) || piece.empty() || !files::append(out, piece)) return false;
        at += piece.size();
    }
    return true;
}

/// Whether `copy`'s `have` bytes begin as `log` does: its first 4 KB of them compared, which a log of another run, its
/// first line's time and thread among them, never shares.
bool sameStart(const std::wstring& log, const std::wstring& copy, uint64_t have) {
    if (!have) return true;
    std::string a, b;
    const uint64_t n = std::min<uint64_t>(have, 4096);
    return files::readRange(log, 0, n, a) && files::readRange(copy, 0, n, b) && a.size() == n && a == b;
}

/// `to` made `from`'s whole copy: written beside it, then put in place, so that a reader finds the old one or the new.
bool copyWhole(const std::wstring& from, const std::wstring& to) {
    const std::wstring part = to + L".part";
    if (files::copy(from, part) && files::move(part, to)) return true;
    files::remove(part);
    return false;
}

/// `to` made the end of `from`, a log of `size` bytes over capBytes: the note, then its bytes from tailStart.
bool copyTail(const std::wstring& from, const std::wstring& to, uint64_t size) {
    const uint64_t start = tailStart(from, size);
    const std::string note = tailNote(start, size);
    std::string head;
    // (Made so before, from this very log: the copy of the run before, rotated.)
    if (files::readRange(to, 0, note.size(), head) && head == note) return true;
    const std::wstring part = to + L".part";
    if (files::write(part, note) && appendRange(from, part, start, size) && files::move(part, to)) return true;
    files::remove(part);
    return false;
}

/// `to` made a copy of the finished log `from`: only what it lacks appended where it is a beginning of it (the mirror's copy
/// of a run, rotated, lacks at most its last 30 s), else whole; over capBytes, its end.
bool copyLog(const std::wstring& from, const std::wstring& to) {
    if (!files::exists(from)) return true;
    const uint64_t size = files::size(from);
    if (size > capBytes) return copyTail(from, to, size);
    const bool there = files::exists(to);
    const uint64_t have = there ? files::size(to) : 0;
    if (there && have <= size && sameStart(from, to, have)) return have == size || appendRange(from, to, have, size);
    return copyWhole(from, to);
}

/// The folder made, and filled as it is at a start: the runs before, the running log begun. False where it could not be made.
bool begin() {
    if (!files::makeDirectories(gDir)) {
        fail("the folder " + narrow(gDir) + " could not be made");
        return false;
    }
    // As shellLog::initialize turned LocalState's: the copy of the run before is megler.prev.log now.
    if (files::exists(mirrored(L"megler.log"))) {
        if (files::exists(mirrored(L"megler.prev.log"))) files::move(mirrored(L"megler.prev.log"), mirrored(L"megler.prev2.log"));
        files::move(mirrored(L"megler.log"), mirrored(L"megler.prev.log"));
    }
    for (const wchar_t* name : {L"megler.prev.log", L"megler.prev2.log"})
        if (!copyLog(local(name), mirrored(name))) fail(narrow(name) + " could not be copied");
    if (!files::write(mirrored(L"megler.log"), std::string_view())) fail("megler.log could not be begun");
    gCopied = 0;
    gCapped = false;
    files::write(mirrored(L"package.txt"), packageText());
    return true;
}

/// megler.log's lines that are new since the last pass, appended: up to capBytes, and there a line that says the rest comes
/// at the next start.
void followLog() {
    if (gCapped) return;
    const std::wstring from = local(L"megler.log"), to = mirrored(L"megler.log");
    const uint64_t size = files::size(from);
    if (size < gCopied) {
        // (Begun again under the mirror: never in a run so far.)
        files::write(to, std::string_view());
        gCopied = 0;
    }
    const uint64_t upTo = std::min(size, capBytes);
    if (upTo > gCopied) {
        if (!appendRange(from, to, gCopied, upTo)) {
            fail("megler.log's new lines could not be copied");
            return;
        }
        gCopied = upTo;
    }
    if (size > capBytes) {
        gCapped = true;
        files::append(to, format("\r\n-- megler: this log passed %llu MB here, and its copy stops: the rest is in the package's LocalState, "
                                 "and the log's last %llu MB come here at the package's next start --\r\n",
                                 (unsigned long long)(capBytes >> 20), (unsigned long long)(capBytes >> 20)));
    }
}

/// Whether a copy's time of writing, `copy`, is its source's, `source`: the same, or, where the copy is on a memory card's FAT,
/// which keeps a time to two seconds, the source's rounded so -- such a copy was made again at every pass.
bool sameTime(uint64_t source, uint64_t copy) {
    const uint64_t twoSeconds = 20000000;  // (FILETIME's 100 ns)
    return source == copy || (copy % twoSeconds == 0 && (source > copy ? source - copy : copy - source) < twoSeconds);
}

/// The other files, each whenever it has changed (its size or its time: a copy keeps the time of what it copies); gone
/// from LocalState, gone here too.
void copyOthers() {
    for (const std::wstring& name : names()) {
        if (name == L"megler.log" || name == L"megler.prev.log" || name == L"megler.prev2.log") continue;
        const std::wstring from = local(name), to = mirrored(name);
        if (!files::exists(from)) {
            if (files::exists(to)) files::remove(to);
            continue;
        }
        const uint64_t size = files::size(from);
        if (files::exists(to) && size == files::size(to) && sameTime(files::lastWrite(from), files::lastWrite(to))) continue;
        if (!(size > capBytes ? copyTail(from, to, size) : copyWhole(from, to))) fail(narrow(name) + " could not be copied");
    }
    const std::string flags = flagsText();
    const std::wstring list = mirrored(L"flags.txt");
    std::string had;
    if (flags.empty()) {
        if (files::exists(list)) files::remove(list);
    } else if (!files::read(list, had) || had != flags) {
        files::write(list, flags);
    }
}

void run() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    bool made = false;  // the folder was made and filled, at the last begin()
    uint64_t asked = 0;
    {
        // Out of the way of the start, the busiest time of a phone's storage (an app's libraries, its classes, Google Play
        // services' APK): the first pass ten seconds in, or as soon as now() asks (suspended at once).
        std::unique_lock<std::mutex> lock(gMutex);
        gWake.wait_for(lock, kFirstPass, [] { return gAsked != 0; });
        asked = gAsked;
    }
    for (;;) {
        try {
            // (Again where the folder went meanwhile: Megler removes a package's folder when it is not in the list of the
            // installed ones, and one installed after the list was made may be running. Where it cannot be made at all, the
            // pass is left out: fail() has said why, three times at most.)
            if (!made || !files::exists(gDir)) {
                if (made) shellLog::write("logs: " + narrow(gDir) + " was gone: begun again");
                made = begin();
            }
            if (made) {
                shellLog::flush();
                followLog();
                copyOthers();
            }
        } catch (...) {
            fail("the mirror's pass failed");
        }
        std::unique_lock<std::mutex> lock(gMutex);
        gAnswered = asked;
        gWake.notify_all();
        gWake.wait_for(lock, kPeriod, [&] { return gAsked != asked; });
        asked = gAsked;
    }
}

} // namespace

const std::vector<std::wstring>& names() {
    static const std::vector<std::wstring> list = {L"megler.log",          L"megler.prev.log",           L"megler.prev2.log",
                                                   L"megler-boot.txt",     L"megler-selftest.log",       L"megler-selftest-steps.log",
                                                   L"megler-probe.log",    L"settings.ini",              L"flags-from-pictures.txt",
                                                   L"storage-in-shared.txt", L"install-refusal.txt"};
    return list;
}

std::wstring root() {
    const std::wstring shared = appPackage::sharedFolder();
    return shared.empty() ? std::wstring() : shared + L"\\logs";
}

std::wstring identity() {
    try {
        return std::wstring(winrt::Windows::ApplicationModel::Package::Current().Id().Name());
    } catch (...) {
        return L"Megler";
    }
}

std::string flagsText() {
    auto flags = files::list(platform::localStateDirectory() + L"\\flags", L"*");
    std::sort(flags.begin(), flags.end());
    std::string text;
    for (const auto& name : flags) text += narrow(name) + "\r\n";
    return text;
}

std::string packageText() {
    std::string text;
    try {
        text = narrow(std::wstring(winrt::Windows::ApplicationModel::Package::Current().Id().FullName())) + "\r\n";
    } catch (...) {
        text = narrow(identity()) + "\r\n";
    }
    text += "Megler " + platform::packageVersion() + ", " + platform::architecture() + ", " + platform::deviceFamily() + " " +
            platform::osVersion() + "\r\n";
    if (appPackage::isSingleApp()) text += "the app: " + appPackage::single().package + ", " + appPackage::single().label + "\r\n";
    return text;
}

uint64_t tailStart(const std::wstring& path, uint64_t size) {
    const uint64_t at = size > capBytes ? size - capBytes : 0;
    std::string window;
    if (!at || !files::readRange(path, at, 64 * 1024, window)) return at;
    const size_t line = window.find('\n');
    return line == std::string::npos ? at : at + line + 1;
}

std::string tailNote(uint64_t start, uint64_t size) {
    return format("-- megler: the first %llu of this file's %llu bytes are left out here: a copy keeps its last %llu MB; the whole "
                  "file is in the package's LocalState --\r\n",
                  (unsigned long long)start, (unsigned long long)size, (unsigned long long)(capBytes >> 20));
}

void start() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gStarted) return;
        gStarted = true;
    }
    try {
        std::thread([] {
            try {
                gLocal = platform::localStateDirectory();
                const std::wstring logs = root();
                if (logs.empty()) {
                    shellLog::write("logs: Windows gives no shared folder: this package's logs are not mirrored");
                    std::lock_guard<std::mutex> lock(gMutex);
                    gStarted = false;
                    return;
                }
                gDir = logs + L"\\" + identity();
                shellLog::write("logs: mirrored to " + narrow(gDir) + ", for Save All Logs");
            } catch (...) {
                std::lock_guard<std::mutex> lock(gMutex);
                gStarted = false;
                return;
            }
            run();
        }).detach();
    } catch (...) {
        // (No thread for it: no mirror, and now() waits for none.)
        std::lock_guard<std::mutex> lock(gMutex);
        gStarted = false;
    }
}

void now() {
    std::unique_lock<std::mutex> lock(gMutex);
    if (!gStarted) return;
    const uint64_t ask = ++gAsked;
    gWake.notify_all();
    gWake.wait_for(lock, std::chrono::seconds(1), [&] { return gAnswered >= ask || !gStarted; });
}

void removeUninstalled(const std::vector<appPackage::InstalledApp>& installed) {
    const std::wstring logs = root();
    if (logs.empty() || appPackage::isSingleApp()) return;
    int removed = 0, left = 0;
    for (const auto& name : files::listFolders(logs, L"MeglerApp.*")) {
        const std::string identityName = narrow(name);
        if (std::any_of(installed.begin(), installed.end(), [&](const appPackage::InstalledApp& a) { return a.identity == identityName; }))
            continue;
        if (files::removeTree(logs + L"\\" + name))
            removed++;
        else
            left++;
    }
    if (removed || left)
        shellLog::write(format("logs: the logs of %d packages no longer installed removed from the shared folder", removed) +
                        (left ? format("; %d could not all be removed", left) : std::string()));
}

} // namespace megler::app::logMirror
