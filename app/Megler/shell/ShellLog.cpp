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

// The ring, the lines not written out yet and the file's name: only ever held
// while they change, never across the file's opening or writing.
std::mutex gMutex;
// The file's: whoever writes lines out to it holds it from taking them out of
// gBuffer to the end of the write, so that what one took goes in before what
// the next takes. Nothing but a writer waits for it -- the writer thread, a
// line that must be in the file before its thread goes on, flush().
std::mutex gFileMutex;
// The thread under gMutex, and under gFileMutex: a fault there that the core
// reports comes back to emit() on the same thread, which must not wait for
// itself.
std::atomic<DWORD> gOwner{0}, gFileOwner{0};
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

// The tests' (setWriteHook): called as lines are written out.
std::atomic<void (*)()> gWriteHook{nullptr};

struct Owner {
    std::atomic<DWORD>& who;
    explicit Owner(std::atomic<DWORD>& w) : who(w) { who.store(GetCurrentThreadId()); }
    ~Owner() { who.store(0); }
};

// The lines said so far, out to the file: taken out of gBuffer under gMutex,
// written with gFileMutex alone held. On a phone a write is a millisecond or
// more, and far longer while another thread puts a file on the disk (SQLite's
// fsync); it was made under gMutex -- and, the core's lines coming through
// its log's lock, under that too -- so every thread that logged meanwhile
// waited for the disk: a fault's report wrote its trace lines one by one so
// while Tuya Smart's main thread waited in the core's mg_logv (0.1.0.671).
// Called with neither lock held. A thread that comes while another writes
// waits for that write, which has its lines in it or leaves them for it.
void writeOut() {
    std::lock_guard<std::mutex> file(gFileMutex);
    Owner fileOwner(gFileOwner);
    std::string lines;
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        Owner owner(gOwner);
        if (gBuffer.empty() || gPathW.empty()) return;
        lines.swap(gBuffer);
        path = gPathW;
    }
    if (const auto hook = gWriteHook.load()) hook();
    files::append(path, lines);
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
    const DWORD self = GetCurrentThreadId();
    if (gOwner.load() == self || gFileOwner.load() == self) {
        // Entered again from a fault on this thread while it held one of the
        // log's locks: the file only, past them.
        if (!gPathW.empty()) files::append(gPathW, line + "\r\n");
        return;
    }
    bool now;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        Owner owner(gOwner);
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
        now = gBuffer.size() >= 64 * 1024 || urgent(level, message);
    }
    // (Past gMutex: the other threads' lines go into gBuffer meanwhile.)
    if (now) writeOut();
}

void writer() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        writeOut();
    }
}

} // namespace

void initialize(const std::wstring& localStateDirectory) {
    // The files moved and begun under the file's lock alone: a line said
    // meanwhile waits in gPending, or in gBuffer behind the early ones, and
    // never for the disk.
    std::lock_guard<std::mutex> fileLock(gFileMutex);
    Owner fileOwner(gFileOwner);
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gPathW.empty()) return;
    }
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
    {
        std::lock_guard<std::mutex> lock(gMutex);
        Owner owner(gOwner);
        for (const auto& line : gPending) early += line + "\r\n";
        gPending.clear();
        gPathW = file;
    }
    files::write(file, early);
    std::thread(writer).detach();
}

void flush() { writeOut(); }

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

void setWriteHook(void (*hook)()) { gWriteHook.store(hook); }

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
