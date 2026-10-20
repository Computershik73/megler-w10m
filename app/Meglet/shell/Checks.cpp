#include "pch.h"

#include "shell/Checks.h"

#include "player/AudioHost.h"
#include "player/Session.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "meglet.h"

namespace meglet::app::checks {
namespace {

struct Output {
    std::vector<std::string> lines;
    unsigned run = 0;
};

std::mutex gMutex;         // the two outputs and the flags
std::mutex gRunMutex;      // held for the length of a check: one at a time
Output gOutputs[2];
bool gRunning = false;
bool gSelfTestStarted = false;
Kind gCurrent = Kind::probe;

struct Sink {
    Kind kind;
    files::LineLog* log;
    size_t count = 0;
};

void emit(Sink& sink, const std::string& text) {
    sink.log->line(text);
    ++sink.count;
    std::lock_guard<std::mutex> lock(gMutex);
    gOutputs[int(sink.kind)].lines.push_back(text);
}

// meglet_line_fn: called by the core on the checking thread.
void coreLine(const char* line, void* user) { emit(*static_cast<Sink*>(user), line ? line : ""); }

// A stack the size Android gives an app's main thread: the self-test calls
// real Android code on the thread it runs on.
constexpr size_t checkStack = size_t(8) << 20;

} // namespace

const char* name(Kind kind) { return kind == Kind::probe ? "probe" : "selftest"; }

const wchar_t* logName(Kind kind) { return kind == Kind::probe ? L"meglet-probe.log" : L"meglet-selftest.log"; }

std::string run(Kind kind) {
    std::lock_guard<std::mutex> one(gRunMutex);
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gRunning = true;
        gCurrent = kind;
        if (kind == Kind::selftest) gSelfTestStarted = true;
        gOutputs[int(kind)].lines.clear();
        ++gOutputs[int(kind)].run;
    }
    files::LineLog log(platform::localStateDirectory() + L"\\" + logName(kind));
    Sink sink{kind, &log};
    shellLog::write(std::string("check: ") + name(kind) + " started");
    emit(sink, format("%s: Meglet %s, core %s (%s, %s %s)", name(kind), platform::packageVersion().c_str(), meglet_version(),
                      platform::architecture().c_str(), platform::deviceFamily().c_str(), platform::osVersion().c_str()));
    std::string done;
    if (kind == Kind::probe) {
        // The shell's own part first: whether the loader takes ANGLE as it
        // was packaged. Then the core's.
        platform::angleLoadCheck([&sink](const std::string& line) { emit(sink, line); });
        emit(sink, std::string("code-generation: ") + (platform::canRunAndroidCode() ? "granted (meglet_init succeeded)" : "NOT granted (meglet_init returned false)"));
        emit(sink, "memory: " + platform::memorySummary());
        meglet_probe(&coreLine, &sink);
        // And the one answer that takes an ear: a second of a tone through
        // the output an app's sound goes to.
        AudioHost::testTone([&sink](const std::string& line) { emit(sink, line); });
        done = format("DONE probe: %u lines", unsigned(sink.count));
    } else {
        const int failed = meglet_selftest(&coreLine, &sink);
        done = format("DONE selftest: %d failed", failed);
    }
    emit(sink, done);
    shellLog::write("check: " + done);
    std::lock_guard<std::mutex> lock(gMutex);
    gRunning = false;
    return done;
}

bool start(Kind kind) {
    if (kind == Kind::selftest && session::spent()) return false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRunning || (kind == Kind::selftest && gSelfTestStarted)) return false;
        // Taken now, not when the thread gets going: two taps must not start two.
        gRunning = true;
        gCurrent = kind;
    }
    if (platform::startThread([kind] { run(kind); }, checkStack)) return true;
    std::lock_guard<std::mutex> lock(gMutex);
    gRunning = false;
    return false;
}

bool running() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gRunning;
}

Kind current() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gCurrent;
}

bool selfTestStarted() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gSelfTestStarted;
}

uint64_t take(Kind kind, uint64_t cursor, std::vector<std::string>& out, unsigned& run) {
    std::lock_guard<std::mutex> lock(gMutex);
    const Output& output = gOutputs[int(kind)];
    if (run != output.run) {
        run = output.run;
        cursor = 0;
    }
    for (uint64_t i = cursor; i < output.lines.size(); ++i) out.push_back(output.lines[size_t(i)]);
    return output.lines.size();
}

} // namespace meglet::app::checks
