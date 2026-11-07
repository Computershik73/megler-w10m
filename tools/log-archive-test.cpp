// The check of Save All Logs on a PC (tools/log-archive-test.sh builds and runs it): app/Megler/shell/LogArchive.cpp, the file
// Megler.exe is built from, with the logs' mirror, the ZIP writer, the shell's Files.cpp and Strings.cpp and the core's zlib,
// and stand-ins for the little else (the shared folder, LocalState, what the platform says, the shell's log), run on
// folders of its own as Megler's would have them: its own logs in LocalState (one of them over 16 MB, a flag set, the last
// install refused), two apps' copies in the shared folder (one with a file half written, .part), Megler's and an app's
// settings. The stand-in says it is an Xbox, so the ZIP stays in LocalState\logs and nothing is put in this PC's Pictures.
// Then tools/log-archive-check.py reads the ZIP with Python's zipfile.
//
//   log-archive-test <folder>     the ZIP's path on its last line; exits 0 when saving said it saved
#include "pch.h"

#include "megler.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/LogArchive.h"
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

extern "C" const char* megler_version(void) { return "0.1.0 (the test's)"; }

namespace megler::app::appPackage {
std::wstring sharedFolder() { return gRoot + L"\\shared"; }
const SingleApp& single() {
    static const SingleApp none;
    return none;
}
std::wstring meglerFamilyName() { return L"Megler_7p434m16tbgc6"; }
} // namespace megler::app::appPackage
namespace megler::app::platform {
const std::wstring& localStateDirectory() {
    static const std::wstring path = gRoot + L"\\LocalState";
    return path;
}
std::string packageVersion() { return "0.1.0.999"; }
std::string architecture() { return "x64"; }
std::string deviceFamily() { return "Windows.Xbox"; }
std::string osVersion() { return "10.0.0.0"; }
std::string memorySummary() { return "1.00 GB limit, 100 MB in use"; }
bool canRunAndroidCode() { return true; }
const AngleStatus& angle() {
    static const AngleStatus status{true, "In the package"};
    return status;
}
bool isXbox() { return true; }
bool isMobile() { return false; }
} // namespace megler::app::platform
namespace megler::app::shellLog {
void write(const std::string& line) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gLog += line + "\n";
}
void flush() {}
} // namespace megler::app::shellLog

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: log-archive-test <folder>\n");
        return 2;
    }
    gRoot = argv[1];
    files::removeTree(gRoot);
    const std::wstring local = gRoot + L"\\LocalState", shared = gRoot + L"\\shared";
    files::makeDirectories(local + L"\\flags");
    files::write(local + L"\\megler.log", "02:00:00.000 I shell: Megler's running log\r\n");
    files::write(local + L"\\megler.prev.log", "02:00:00.000 I shell: the run before\r\n");
    // Over 16 MB, as a long run of a chatty app's: its last 16 MB go into the ZIP.
    std::string big;
    for (int i = 0; big.size() < logMirror::capBytes + 1024 * 1024; i++) big += format("02:10:00.000 I app: line %d of a long run\r\n", i);
    files::write(local + L"\\megler.prev2.log", big);
    files::write(local + L"\\install-refusal.txt", "2026-11-08 02:10:11, Megler 0.1.0.999: MeglerApp.a.appx was not installed. refused\r\n");
    files::write(local + L"\\flags\\keep-screen-on", "x");
    for (const wchar_t* app : {L"MeglerApp.one.app", L"MeglerApp.two.app"}) {
        const std::wstring dir = shared + L"\\logs\\" + app;
        files::makeDirectories(dir);
        files::write(dir + L"\\megler.log", narrow(app) + " running\r\n");
        files::write(dir + L"\\megler.prev.log", narrow(app) + " the run before\r\n");
        files::write(dir + L"\\package.txt", narrow(app) + "_0.1.0.999_x64__7p434m16tbgc6\r\n");
    }
    files::write(shared + L"\\logs\\MeglerApp.two.app\\megler.log.part", "half written");
    // (Megler's own copies, which the ZIP leaves out: its LocalState has the newer.)
    files::makeDirectories(shared + L"\\logs\\Megler");
    files::write(shared + L"\\logs\\Megler\\megler.log", "an older copy of Megler's\r\n");
    files::write(shared + L"\\settings.ini", "megler.googlePlay=yes\r\n");
    files::makeDirectories(shared + L"\\apps\\one.app");
    files::write(shared + L"\\apps\\one.app\\settings.ini", "megler.renderCap=960\r\n");
    files::makeDirectories(shared + L"\\apps\\three.app");  // (no settings of its own)

    if (!logArchive::start()) {
        printf("FAILED: saving did not start\n");
        return 1;
    }
    for (int i = 0; i < 600 && logArchive::state().running; i++) Sleep(100);
    const logArchive::State s = logArchive::state();
    printf("%s", gLog.c_str());
    if (s.running || !s.saved) {
        printf("FAILED: not saved: %s\n", s.error.c_str());
        return 1;
    }
    printf("saved in %s: %s, %llu bytes, %u files; %s\n", s.where.c_str(), s.name.c_str(), (unsigned long long)s.size, s.files, s.note.c_str());
    const std::wstring zip = local + L"\\logs\\" + widen(s.name);
    if (s.where != "LocalState\\logs" || !files::exists(zip) || !logArchive::file()) {
        printf("FAILED: the ZIP is not in LocalState\\logs, or there is no file for Share\n");
        return 1;
    }
    printf("%s\n", narrow(zip).c_str());
    return 0;
}
