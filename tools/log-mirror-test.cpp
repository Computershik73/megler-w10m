// The check of the logs' mirror on a PC (tools/log-mirror-test.sh builds and runs it): app/Megler/shell/LogMirror.cpp, the
// file Megler.exe is built from, with the shell's Files.cpp and Strings.cpp, and stand-ins for the little else it calls (the
// shared folder, LocalState, the shell's log), run on folders of its own as a package's start and run would have it:
//
//   1. a start after a run that ended in a crash: LocalState's megler.prev.log whole, the mirror's copy of that run a
//      beginning of it (its last lines never copied), an older run's copy as megler.prev.log there -- rotated and completed,
//      only the missing lines appended; the run before that copied again from nothing where the mirror had none;
//   2. the running megler.log's new lines copied at each now(), and nothing else of it written again;
//   3. the other files copied when they change and taken away when they go, flags.txt following LocalState\flags;
//   4. a log over 16 MB: its copy stops there with a line saying so; at the next start the run's last 16 MB, from a line's
//      beginning, after a line saying what was left out;
//   5. the folder taken away under the running mirror: made again at its next pass;
//   6. removeUninstalled: the folders of packages not in the list go, Megler's and the listed ones stay.
//
//   log-mirror-test <folder>     exits 0 when every check passes, and says each that does not
#include "pch.h"

#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/LogMirror.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <cstdio>

using namespace megler::app;

namespace {
std::wstring gRoot;
std::mutex gLogMutex;
std::string gLog;
} // namespace

// The stand-ins: where the test's folders are, and a log in memory.
namespace megler::app::appPackage {
std::wstring sharedFolder() { return gRoot + L"\\shared"; }
const SingleApp& single() {
    static const SingleApp none;
    return none;
}
} // namespace megler::app::appPackage
namespace megler::app::platform {
const std::wstring& localStateDirectory() {
    static const std::wstring path = gRoot + L"\\LocalState";
    return path;
}
std::string packageVersion() { return "0.1.0.999"; }
std::string architecture() { return "x64"; }
std::string deviceFamily() { return "Windows.Desktop"; }
std::string osVersion() { return "10.0.0.0"; }
} // namespace megler::app::platform
namespace megler::app::shellLog {
void write(const std::string& line) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gLog += line + "\n";
}
void flush() {}
} // namespace megler::app::shellLog

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    gFailures++;
    printf("FAILED: %s\n", what.c_str());
}

std::string readAll(const std::wstring& path) {
    std::string text;
    files::read(path, text);
    return text;
}

std::string lines(const char* tag, int from, int to) {
    std::string text;
    for (int i = from; i < to; i++) text += format("02:00:%02d.%03d I %s: line %d\r\n", (i / 1000) % 60, i % 1000, tag, i);
    return text;
}

/// The file's id on its volume: a copy put in its place is another file, though it keeps the time of what it copied.
uint64_t fileId(const std::wstring& path) {
    HANDLE h = CreateFile2(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILE_ID_INFO info{};
    const bool ok = GetFileInformationByHandleEx(h, FileIdInfo, &info, sizeof(info)) != 0;
    CloseHandle(h);
    uint64_t id = 0;
    if (ok) memcpy(&id, info.FileId.Identifier, sizeof(id));
    return id;
}

/// The mirror's passes asked for until one has run after everything before: now() waits a second at most, and a pass
/// that copies 16 MB may take longer.
void settle() {
    for (int i = 0; i < 3; i++) logMirror::now();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: log-mirror-test <folder>\n");
        return 2;
    }
    gRoot = argv[1];
    files::removeTree(gRoot);
    const std::wstring local = gRoot + L"\\LocalState", mirror = gRoot + L"\\shared\\logs\\Megler";
    files::makeDirectories(local + L"\\flags");
    files::makeDirectories(mirror);

    // 1. As a start after a crash finds them: LocalState's logs rotated by shellLog::initialize, the mirror's not yet.
    const std::string run1 = lines("run one", 0, 3000), run2 = lines("run two", 0, 5000), run3 = lines("run three", 0, 100);
    files::write(local + L"\\megler.prev2.log", run1);
    files::write(local + L"\\megler.prev.log", run2);
    files::write(local + L"\\megler.log", run3);
    files::write(mirror + L"\\megler.log", run2.substr(0, run2.size() / 2));  // the crash: its copy stopped halfway
    files::write(mirror + L"\\megler.prev.log", "a run before both, which goes to megler.prev2.log and is made run one's\r\n");
    files::write(local + L"\\megler-boot.txt", "02:00:00.000 wWinMain reached\r\n");
    files::write(local + L"\\install-refusal.txt", "a refusal\r\n");
    files::write(local + L"\\flags\\trace-views", "x");
    files::write(mirror + L"\\megler-probe.log", "a probe's log LocalState no longer has\r\n");

    const uint64_t crashedCopy = fileId(mirror + L"\\megler.log");
    logMirror::start();
    settle();  // (the first pass, as a suspend asks it before the ten seconds are up)
    check(readAll(mirror + L"\\megler.prev.log") == run2, "the crashed run's copy is completed: megler.prev.log is run two's");
    check(crashedCopy && fileId(mirror + L"\\megler.prev.log") == crashedCopy, "... in place: only the lines it lacked appended");
    check(readAll(mirror + L"\\megler.prev2.log") == run1, "megler.prev2.log is run one's");
    check(readAll(mirror + L"\\megler.log") == run3, "the running log is copied");
    check(readAll(mirror + L"\\megler-boot.txt") == readAll(local + L"\\megler-boot.txt"), "megler-boot.txt is copied");
    check(readAll(mirror + L"\\install-refusal.txt") == "a refusal\r\n", "install-refusal.txt is copied");
    check(readAll(mirror + L"\\flags.txt") == "trace-views\r\n", "flags.txt names the flags");
    check(!files::exists(mirror + L"\\megler-probe.log"), "a file LocalState no longer has goes from the mirror");
    check(files::exists(mirror + L"\\package.txt"), "package.txt is written");

    // 2. The running log's new lines, at each now(): appended to the same file; an unchanged file left as it is.
    const uint64_t bootCopy = fileId(mirror + L"\\megler-boot.txt"), logCopy = fileId(mirror + L"\\megler.log");
    std::string running = run3;
    for (int round = 0; round < 3; round++) {
        const std::string more = lines("run three", 100 + round * 50, 150 + round * 50);
        files::append(local + L"\\megler.log", more);
        running += more;
        settle();
        check(readAll(mirror + L"\\megler.log") == running, format("the running log's new lines are copied (round %d)", round));
    }
    check(bootCopy && fileId(mirror + L"\\megler-boot.txt") == bootCopy, "an unchanged file is not copied again");
    check(logCopy && fileId(mirror + L"\\megler.log") == logCopy, "the running log's copy is appended to, not made again");

    // 3. A file changed, one gone, a flag gone.
    files::append(local + L"\\megler-boot.txt", "02:00:05.000 XAML started\r\n");
    files::remove(local + L"\\install-refusal.txt");
    files::remove(local + L"\\flags\\trace-views");
    settle();
    check(readAll(mirror + L"\\megler-boot.txt") == readAll(local + L"\\megler-boot.txt"), "a changed file is copied again");
    check(!files::exists(mirror + L"\\install-refusal.txt"), "a file gone from LocalState goes from the mirror");
    check(!files::exists(mirror + L"\\flags.txt"), "flags.txt goes with the last flag");

    // 4. Past 16 MB: the copy stops, with a line; at the next start (begun again below), the run's last 16 MB.
    std::string big;
    for (int i = 0; big.size() < logMirror::capBytes + 3 * 1024 * 1024; i++) big += format("02:10:00.000 I app: a long log's line %d\r\n", i);
    files::append(local + L"\\megler.log", big);
    running += big;
    settle();
    const std::string capped = readAll(mirror + L"\\megler.log");
    check(capped.size() > logMirror::capBytes && capped.size() < logMirror::capBytes + 1024, "the running log's copy stops at 16 MB");
    check(capped.compare(0, logMirror::capBytes, running, 0, logMirror::capBytes) == 0, "what it holds is the log's beginning");
    check(capped.find("-- megler: this log passed 16 MB here") != std::string::npos, "a line says where it stopped");

    // 5. The folder taken away under the mirror (as Megler does for a package not in its list): made again at the next pass,
    // the running log copied from its beginning (as far as 16 MB).
    files::removeTree(gRoot + L"\\shared\\logs\\Megler");
    settle();
    check(files::exists(mirror + L"\\megler.log") && files::exists(mirror + L"\\megler.prev.log"), "the folder is made again");
    check(readAll(mirror + L"\\megler.prev.log") == run2, "... with the runs before");
    check(gLog.find("was gone: begun again") != std::string::npos, "... and the log says so");

    // 6. removeUninstalled: an uninstalled app's folder goes, the listed one's and Megler's stay.
    files::makeDirectories(gRoot + L"\\shared\\logs\\MeglerApp.gone.app");
    files::write(gRoot + L"\\shared\\logs\\MeglerApp.gone.app\\megler.log", "x");
    files::makeDirectories(gRoot + L"\\shared\\logs\\MeglerApp.kept.app");
    files::write(gRoot + L"\\shared\\logs\\MeglerApp.kept.app\\megler.log", "x");
    logMirror::removeUninstalled({{"MeglerApp.kept.app", "0.1.0.999", "MeglerApp.kept.app_0.1.0.999_x64__7p434m16tbgc6"}});
    check(!files::exists(gRoot + L"\\shared\\logs\\MeglerApp.gone.app"), "an uninstalled app's folder goes");
    check(files::exists(gRoot + L"\\shared\\logs\\MeglerApp.kept.app\\megler.log"), "an installed app's folder stays");
    check(files::exists(mirror + L"\\megler.log"), "Megler's own folder stays");

    // 4, the next start: the big run's last 16 MB, from a line's beginning, after a line saying what was left out. (A start
    // as shellLog::initialize makes it, and the mirror's begin(), as the folder's going made it run once more above: this
    // checks copyLog's tail on the finished log by tailStart and tailNote, which both the mirror and the ZIP use.)
    const uint64_t size = files::size(local + L"\\megler.log");
    const uint64_t start = logMirror::tailStart(local + L"\\megler.log", size);
    check(start > size - logMirror::capBytes && start < size - logMirror::capBytes + 200, "the tail begins within a line of 16 MB from the end");
    check(running[size_t(start) - 1] == '\n', "the tail begins at a line's beginning");
    const std::string note = logMirror::tailNote(start, size);
    check(note.find(format("the first %llu of this file's %llu bytes are left out", (unsigned long long)start, (unsigned long long)size)) !=
              std::string::npos,
          "the note says what was left out");

    printf("%s", gLog.c_str());
    printf("log-mirror-test: %s\n", gFailures ? format("%d failures", gFailures).c_str() : "passed");
    return gFailures ? 1 : 0;
}
