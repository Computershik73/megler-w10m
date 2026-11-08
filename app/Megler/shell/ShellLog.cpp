#include "pch.h"

#include "shell/ShellLog.h"

#include "shell/Files.h"
#include "shell/Strings.h"

#include <appmodel.h>

#include <chrono>
#include <deque>
#include <thread>

namespace megler::app::shellLog {
namespace {

std::mutex gMutex;
// The thread inside emit() under gMutex: a fault there that the core reports
// comes back to emit() on the same thread, which must not wait for itself.
std::atomic<DWORD> gOwner{0};
std::wstring gPathW;
std::deque<std::string> gRing;
uint64_t gFirst = 0;               // the number of the ring's first line
std::vector<std::string> gPending; // before initialize
// The lines not in the file yet: written out every 200 ms by a thread of the
// log's own (writer), at once for a fault's report and when 64 KB wait, and on
// suspending and exiting (flush). Each line was its own open, append and close
// of the file, under gMutex: Native.log was 6% of Yandex Maps' main thread,
// which logs hundreds of lines a second, and every other thread that logged
// waited meanwhile (package 531). The file still is never kept open: the
// Device Portal refuses to copy one a process holds open (Files.h).
std::string gBuffer;

// With gMutex held.
void writeOut() {
    if (gBuffer.empty() || gPathW.empty()) return;
    files::append(gPathW, gBuffer);
    gBuffer.clear();
}

bool urgent(int level, const std::string& message) {
    return message.compare(0, 6, "fault:") == 0 || message.find("FATAL") != std::string::npos ||
           message.compare(0, 6, "trace:") == 0 ||
           // The core's word on a process Windows is about to end -- abort() (std::terminate's end too), an exception nothing
           // handled, the C runtime's refusals, a pure call -- is an error of "host:" (core/src/host/mg_host_win32.c). It
           // waited for the writer like any other line, and abort() ends the process at once: an exception escaping a
           // thread left a log that stopped with no word of it.
           (level >= 3 && message.compare(0, 5, "host:") == 0);
}

char levelLetter(int level) {
    switch (level) {
    case 0: return 'D';
    case 1: return 'I';
    case 2: return 'W';
    case 3: return 'E';
    default: return '?';
    }
}

void emit(int level, const std::string& message) {
    const std::string line = timeOfDay() + " " + levelLetter(level) + " " + message;
    // To a debugger only: with none, OutputDebugStringA's exception reaches
    // nobody (and its dispatch faulted on the phone; see mg_host_print).
    if (IsDebuggerPresent()) OutputDebugStringA(("[megler] " + line + "\n").c_str());
    if (gOwner.load() == GetCurrentThreadId()) {
        // Entered again from a fault on this thread while it was writing:
        // the file only, past the lock it already holds.
        if (!gPathW.empty()) files::append(gPathW, line + "\r\n");
        return;
    }
    std::lock_guard<std::mutex> lock(gMutex);
    struct Owner {
        Owner() { gOwner.store(GetCurrentThreadId()); }
        ~Owner() { gOwner.store(0); }
    } owner;
    gRing.push_back(line);
    if (gRing.size() > ringCapacity) {
        gRing.pop_front();
        ++gFirst;
    }
    if (gPathW.empty()) {
        if (gPending.size() < 500) gPending.push_back(line);
        return;
    }
    gBuffer += line;
    gBuffer += "\r\n";
    if (gBuffer.size() >= 64 * 1024 || urgent(level, message)) writeOut();
}

void writer() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lock(gMutex);
        struct Owner {
            Owner() { gOwner.store(GetCurrentThreadId()); }
            ~Owner() { gOwner.store(0); }
        } owner;
        writeOut();
    }
}

} // namespace

void initialize(const std::wstring& localStateDirectory) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gPathW.empty()) return;
    const std::wstring file = localStateDirectory + L"\\megler.log";
    // The last two runs are kept: after a crash the app is usually opened once
    // more before anyone thinks of fetching the log, and that start must not
    // be what pushes the crash's own log out.
    const std::wstring previous = localStateDirectory + L"\\megler.prev.log";
    if (files::exists(file)) {
        if (files::exists(previous)) files::move(previous, localStateDirectory + L"\\megler.prev2.log");
        files::move(file, previous);
    }
    std::string early;
    for (const auto& line : gPending) early += line + "\r\n";
    gPending.clear();
    files::write(file, early);
    gPathW = file;
    std::thread(writer).detach();
}

void flush() {
    std::lock_guard<std::mutex> lock(gMutex);
    writeOut();
}

void write(const std::string& line) {
    // The first line of a run says which thread the shell's own is: a crash
    // report names the thread it happened on by the same number.
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) emit(1, format("shell: the UI thread is %lu", static_cast<unsigned long>(GetCurrentThreadId())));
    emit(1, "shell: " + line);
}

void writeNow(const std::string& line) {
    write(line);
    flush();
}

void core(int level, const char* line, void*) { emit(level, line ? line : ""); }

std::string path() {
    std::lock_guard<std::mutex> lock(gMutex);
    return narrow(gPathW);
}

uint64_t take(uint64_t cursor, std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lock(gMutex);
    const uint64_t end = gFirst + gRing.size();
    for (uint64_t i = std::max(cursor, gFirst); i < end; ++i) out.push_back(gRing[size_t(i - gFirst)]);
    return end;
}

void bootMark(const char* what) {
    // LocalState is %LOCALAPPDATA%\Packages\<family>\LocalState; inside the
    // container LOCALAPPDATA is the package's own ...\AC directory, whose
    // sibling it is. Both halves come from Win32 alone.
    wchar_t local[MAX_PATH] = {};
    wchar_t family[128] = {};
    UINT32 familyLength = 128;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) ||
        GetCurrentPackageFamilyName(&familyLength, family) != ERROR_SUCCESS)
        return;
    std::wstring base(local);
    if (base.size() > 3 && _wcsicmp(base.c_str() + base.size() - 3, L"\\AC") == 0)
        base.resize(base.size() - 3);
    else
        base += std::wstring(L"\\Packages\\") + family;
    files::append(base + L"\\LocalState\\megler-boot.txt", timeOfDay() + " " + what + "\r\n");
}

} // namespace megler::app::shellLog
