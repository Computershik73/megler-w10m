// The shell's start and its copies, tested on a PC: tools/build-tests.sh win builds this program (mg_shell_tests.exe)
// from the very files the phone's package is built from -- shell/Files.cpp, ShellLog.cpp, Startup.cpp, BundledApks.cpp and
// Strings.cpp -- with the few stand-ins below for the rest of the shell, and runs it on a scratch folder of its own:
//
//   - files::same; files::copyWhole through Windows's cache and past it (COPY_FILE_NO_BUFFERING, from 8 MB), with what
//     its progress is told; files::mend of a copy with a byte wrong;
//   - the log: the core's word on a process about to end ("host: ..." at the error level) and writeNow's line are in the
//     file the moment they are said, not at the writer's next turn;
//   - the start's steps and its watch (shell/Startup.h): a step said at once, the watch's lines while the window is not
//     shown, and none after it is;
//   - bundledApks::plan on a library and a shared folder in each state a phone's can be in (a first start's, all copied,
//     the stamp missing, a copy that is not the package's, microG's of another size, an installed app whose package
//     is gone), and bundledApks::start's copies made on their thread, each said in the log before it begins and after,
//     then the once-a-build check, which leaves out the copies just made.
//
// A Lumia 630's Megler 0.1.0.671 was ended at every start while it made those copies on its UI thread before its window
// was shown (library/Library.cpp); what is tested here is the way they are made since.
//
// Usage: mg_shell_tests <scratch folder>. Each check is a line; the last line counts them, and the exit code is 1 when one
// failed. Not an app container: what Windows's broker refuses an app is not found out here.
#include "pch.h"

#include "library/Library.h"
#include "shell/AppPackage.h"
#include "shell/BundledApks.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Startup.h"
#include "shell/Strings.h"

#include "megler.h"

#include <cstdio>

namespace megler::app {

namespace {
std::wstring gLocal, gInstall, gShared;
} // namespace

// What the copies' code asks of the rest of the shell: the folders, the tests' own; no app's own package; no report of an
// APK (the core is not in this program).
ApkReport ApkReport::parse(const std::string&) { return ApkReport(); }

namespace platform {
const std::wstring& localStateDirectory() { return gLocal; }
const std::wstring& installDirectory() { return gInstall; }
std::string packageVersion() { return "0.1.0.0"; }
} // namespace platform

namespace appPackage {
std::wstring sharedFolder() { return gShared; }
const SingleApp& single() {
    static const SingleApp none;
    return none;
}
} // namespace appPackage

} // namespace megler::app

extern "C" char* megler_apk_report(const char*) { return nullptr; }
extern "C" void megler_free(void*) {}

using namespace megler::app;

namespace {

int gChecks = 0, gFailed = 0;

void check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) gFailed++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
}

/// `n` bytes of a sequence of its own (`seed`), into `path`.
bool makeFile(const std::wstring& path, uint64_t n, uint32_t seed) {
    std::string data(size_t(n), '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (char& c : data) {
        x = x * 1664525u + 1013904223u;
        c = char(x >> 24);
    }
    return files::write(path, data);
}

/// The byte at `at` of `path` made another.
bool spoil(const std::wstring& path, uint64_t at) {
    HANDLE h = CreateFile2(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos = {};
    pos.QuadPart = LONGLONG(at);
    char c = 0;
    DWORD n = 0;
    bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, &c, 1, &n, nullptr) && n == 1;
    c = char(c ^ 0x5A);
    ok = ok && SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && WriteFile(h, &c, 1, &n, nullptr) && n == 1 && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

/// The log's file as it is: what is in it without the writer's next turn.
std::string logText() {
    std::string text;
    files::read(gLocal + L"\\megler.log", text);
    return text;
}

/// The log's file once every line said is written out.
std::string writtenLog() {
    shellLog::flush();
    return logText();
}

size_t count(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) n++;
    return n;
}

/// What a copy's progress was told: the stages in the order they came, the last of each.
struct Told {
    std::vector<files::Stage> stages;
    uint64_t lastDone = 0, lastTotal = 0;
    files::Progress progress() {
        return [this](files::Stage stage, uint64_t done, uint64_t total) {
            if (stages.empty() || stages.back() != stage) stages.push_back(stage);
            lastDone = done;
            lastTotal = total;
        };
    }
};

void testFiles(const std::wstring& root) {
    printf("files\n");
    const std::wstring dir = root + L"\\files";
    files::makeDirectories(dir);
    const std::wstring a = dir + L"\\a.bin", b = dir + L"\\b.bin";
    check(makeFile(a, 300 * 1024 + 7, 1) && makeFile(b, 300 * 1024 + 7, 1), "two files of one sequence made");
    check(files::same(a, b), "same: alike files are the same");
    spoil(b, 200 * 1024);
    check(!files::same(a, b), "same: a byte changed in the middle is seen");
    makeFile(b, 300 * 1024 + 8, 1);
    check(!files::same(a, b), "same: files of two sizes are not the same");
    check(!files::same(a, dir + L"\\none.bin"), "same: a file that is not there is not the same");

    // Through Windows's cache: under unbufferedFrom.
    {
        Told told;
        uint64_t mended = 99;
        const bool ok = files::copyWhole(a, dir + L"\\small.copy", mended, told.progress());
        check(ok && mended == 0 && files::same(a, dir + L"\\small.copy"), "copyWhole: a small file copied whole, nothing mended");
        check(told.stages == std::vector<files::Stage>{files::Stage::copying, files::Stage::flushing, files::Stage::comparing},
              format("copyWhole: its progress told copying, putting on the disk, comparing, in that order (%u stages)", unsigned(told.stages.size())));
        check(told.lastDone == files::size(a) && told.lastTotal == files::size(a), "copyWhole: the comparing told of every byte");
    }
    // Past the cache: unbufferedFrom and more, of a size no whole number of sectors.
    const std::wstring big = dir + L"\\big.bin", bigCopy = dir + L"\\big.copy";
    const uint64_t bigSize = files::unbufferedFrom + 5 * 1024 * 1024 + 123;
    check(makeFile(big, bigSize, 2), format("a file of %llu bytes made", (unsigned long long)bigSize));
    {
        Told told;
        uint64_t mended = 99;
        const ULONGLONG began = GetTickCount64();
        const bool ok = files::copyWhole(big, bigCopy, mended, told.progress());
        check(ok && mended == 0 && files::same(big, bigCopy),
              format("copyWhole: a large file copied past the cache, whole, in %llu ms", (unsigned long long)(GetTickCount64() - began)));
        check(!told.stages.empty() && told.stages.front() == files::Stage::copying && told.stages.back() == files::Stage::comparing &&
                  told.lastDone == bigSize,
              "copyWhole: its progress told of the copy and of every byte compared");
    }
    // A byte wrong in the copy: its chunk written again from the source.
    {
        const uint64_t at = 5 * 1024 * 1024 + 4097;
        check(spoil(bigCopy, at) && !files::same(big, bigCopy), "a byte of the large copy changed");
        uint64_t mended = 0;
        std::vector<uint64_t> chunks;
        Told told;
        const bool ok = files::mend(big, bigCopy, mended, &chunks, told.progress());
        check(ok && mended == files::DiskFile::chunk && chunks.size() == 1 && chunks[0] == (at & ~uint64_t(files::DiskFile::chunk - 1)),
              format("mend: the chunk with the byte written again (%llu bytes mended, %u chunks)", (unsigned long long)mended, unsigned(chunks.size())));
        check(files::same(big, bigCopy), "mend: the copy is the source's byte for byte again");
        check(told.lastDone == bigSize, "mend: its progress told of every byte compared");
    }
    uint64_t none = 0;
    check(!files::copyWhole(dir + L"\\none.bin", dir + L"\\none.copy", none) && !files::exists(dir + L"\\none.copy"),
          "copyWhole: a file that is not there is not copied, and nothing is made");
}

void testLog() {
    printf("log\n");
    shellLog::core(3, "host: abort() was called on thread 1 (the shell's tests say it); Windows ends the process now", nullptr);
    check(logText().find("host: abort() was called on thread 1 (the shell's tests say it)") != std::string::npos,
          "the core's word on a process about to end is in the file at once");
    shellLog::writeNow("a line the shell's tests write out at once");
    check(logText().find("shell: a line the shell's tests write out at once") != std::string::npos, "writeNow's line is in the file at once");
    shellLog::core(1, "an ordinary line of the shell's tests", nullptr);
    Sleep(600);
    check(logText().find("an ordinary line of the shell's tests") != std::string::npos, "an ordinary line is in the file at the writer's turn");
}

void testStartup() {
    printf("start\n");
    const char* const notShown = "the window is not shown yet: still testing the start; the UI thread: a place of the tests'";
    startup::launched([](unsigned) { return std::string("a place of the tests'"); }, {150, 300, 900});
    startup::step("testing the start");
    check(logText().find("start: testing the start") != std::string::npos, "a step is in the file the moment it is said");
    check(startup::current() == "testing the start", "the step is the one said");
    Sleep(600);
    check(count(logText(), notShown) == 2, format("the watch spoke at its two marks before the window was shown (%u times)", unsigned(count(logText(), notShown))));
    startup::shown();
    check(startup::isShown() && logText().find("start: the window is shown, ") != std::string::npos, "the window shown is said");
    Sleep(700);
    check(count(logText(), notShown) == 2, "the watch says nothing once the window is shown");
    check(startup::processAge() > 0, format("the process's age is known (%llu ms)", (unsigned long long)startup::processAge()));
}

using Kind = bundledApks::Task::Kind;

std::string describe(const std::vector<bundledApks::Task>& tasks) {
    std::string text;
    for (const auto& t : tasks) {
        const char* kind = t.kind == Kind::service ? "service" : t.kind == Kind::offer ? "offer" : t.kind == Kind::again ? "again" : "recover";
        text += (text.empty() ? "" : ", ") + std::string(kind) + " " + (t.id.empty() ? narrow(t.target.substr(t.target.find_last_of(L'\\') + 1)) : t.id);
    }
    return text.empty() ? "none" : text;
}

void testBundled() {
    printf("bundled apks\n");
    const std::wstring bitgapps = gInstall + L"\\bitgapps", library = gLocal + L"\\apks";
    files::makeDirectories(bitgapps);
    files::makeDirectories(library);
    files::makeDirectories(gShared);
    const struct {
        const wchar_t* name;
        uint64_t size;
    } apks[] = {{L"GoogleServicesFramework.apk", 200 * 1024 + 3},
                {L"PrebuiltGmsCore.apk", files::unbufferedFrom + 2 * 1024 * 1024 + 4567},
                {L"Phonesky.apk", 3 * 1024 * 1024 + 89},
                {L"MicroGGmsCore.apk", files::unbufferedFrom + 123}};
    bool made = true;
    uint32_t seed = 10;
    for (const auto& apk : apks) made = makeFile(bitgapps + L"\\" + apk.name, apk.size, seed++) && made;
    check(made, "the package's four Google APKs made");
    const auto never = [](const std::string&) { return false; };

    // A first start: everything to copy, Google Services Framework first and microG's last.
    std::vector<bundledApks::Task> tasks = bundledApks::plan(library, gShared, never);
    check(describe(tasks) == "service GoogleServicesFramework.apk, offer bitgapps-PrebuiltGmsCore.apk, offer bitgapps-Phonesky.apk, service GmsCore.apk",
          "plan at a first start: " + describe(tasks));
    check(!files::exists(library + L"\\bitgapps.added"), "plan at a first start: the offers' stamp is not written before they are made");

    // Made on their own thread, each said before it begins and after it ends.
    std::vector<std::pair<std::string, bool>> done;
    std::mutex doneMutex;
    bundledApks::start(tasks, [&](const bundledApks::Task& task, bool copied) {
        std::lock_guard<std::mutex> lock(doneMutex);
        done.emplace_back(task.what, copied);
    });
    check(bundledApks::copying(), "start: the copies are being made when it returns");
    bool sawProgress = false;
    for (int i = 0; i < 1200 && bundledApks::copying(); i++) {
        sawProgress = sawProgress || bundledApks::progress().active;
        Sleep(50);
    }
    check(!bundledApks::copying(), "start: the copies are made within a minute");
    {
        std::lock_guard<std::mutex> lock(doneMutex);
        bool all = done.size() == 4;
        for (const auto& d : done) all = all && d.second;
        check(all && done.front().first == "Google Services Framework" && done.back().first == "microG's Google Play services",
              format("start: each copy told, in order, made (%u told)", unsigned(done.size())));
    }
    bool alike = true;
    for (const auto& task : tasks) alike = alike && files::same(task.source, task.target) && !files::exists(task.target + L".part");
    check(alike, "start: each copy is its source byte for byte, and no .part is left");
    check(!bundledApks::progress().active, "start: no copy is in progress once they are made");
    const std::string log = writtenLog();
    const size_t before = log.find("bundled apks: copying Google Play services, package\\bitgapps\\PrebuiltGmsCore.apk"),
                 after = log.find("bundled apks: Google Play services copied to LocalState\\apks\\bitgapps-PrebuiltGmsCore.apk in ");
    check(before != std::string::npos && after != std::string::npos && before < after,
          "the log says a copy before it begins and after it ends, with its times");
    check(log.find("bundled apks: 4 of 4 copies made in ") != std::string::npos, "the log says all four were made");
    // Then the once-a-build check, which leaves out the copies just made.
    bool checked = false;
    for (int i = 0; i < 600 && !checked; i++) {
        std::string version;
        checked = files::read(gLocal + L"\\bundled-apks-checked.txt", version) && version == "0.1.0.0";
        if (!checked) Sleep(50);
    }
    check(checked, "the once-a-build check ran after the copies, and wrote its stamp");
    check(writtenLog().find("(4 made at this start, and compared then, left out)") != std::string::npos, "the check left out the four copies just made");

    // All there, the stamp not: nothing to copy, and the stamp written.
    tasks = bundledApks::plan(library, gShared, never);
    check(tasks.empty() && files::exists(library + L"\\bitgapps.added"), "plan with every copy there: " + describe(tasks) + ", and the offers' stamp written");
    tasks = bundledApks::plan(library, gShared, never);
    check(tasks.empty(), "plan again: " + describe(tasks));

    // A library's copy that is not the package's (its end differs), and microG's of another size.
    spoil(library + L"\\bitgapps-Phonesky.apk", files::size(library + L"\\bitgapps-Phonesky.apk") - 10);
    files::append(gShared + L"\\services\\microg\\GmsCore.apk", "x");
    tasks = bundledApks::plan(library, gShared, never);
    check(describe(tasks) == "again bitgapps-Phonesky.apk, service GmsCore.apk", "plan with a copy not the package's and microG's of another size: " + describe(tasks));

    // An app installed from the library's copy whose package is gone, and one whose package is there.
    files::remove(library + L"\\bitgapps-PrebuiltGmsCore.apk");
    files::write(library + L"\\bitgapps-PrebuiltGmsCore.apk.installed", "MeglerApp.com.google.android.gms_1.0.0.0_arm__x\r\n");
    tasks = bundledApks::plan(library, gShared, [](const std::string& marker) { return marker.find("MeglerApp.com.google.android.gms") == 0; });
    check(describe(tasks) == "again bitgapps-Phonesky.apk, recover bitgapps-PrebuiltGmsCore.apk, service GmsCore.apk",
          "plan with an installed app's package gone: " + describe(tasks));
    tasks = bundledApks::plan(library, gShared, never);
    check(describe(tasks) == "again bitgapps-Phonesky.apk, service GmsCore.apk", "plan with that package there: " + describe(tasks));

    // A copy of what is not there: refused, said, and nothing left.
    check(!bundledApks::copy(bitgapps + L"\\None.apk", library + L"\\bitgapps-None.apk", "an APK the tests have not") &&
              !files::exists(library + L"\\bitgapps-None.apk") && !files::exists(library + L"\\bitgapps-None.apk.part"),
          "copy: an APK that is not there is not copied, and nothing is left");
    check(writtenLog().find("bundled apks: an APK the tests have not could not be copied to") != std::string::npos, "copy: the log says why not");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: mg_shell_tests <scratch folder>\n");
        return 2;
    }
    const std::wstring root = argv[1];
    files::removeTree(root);
    gLocal = root + L"\\LocalState";
    gInstall = root + L"\\package";
    gShared = root + L"\\shared";
    if (!files::makeDirectories(gLocal) || !files::makeDirectories(gInstall)) {
        printf("the scratch folder could not be made (error %lu)\n", GetLastError());
        return 2;
    }
    shellLog::initialize(gLocal);
    printf("the shell's start and copies (shell/Files.cpp, ShellLog.cpp, Startup.cpp, BundledApks.cpp)\n");
    testFiles(root);
    testLog();
    testStartup();
    testBundled();
    shellLog::flush();
    printf("shell: %d checks, %d failed\n", gChecks, gFailed);
    return gFailed ? 1 : 0;
}
