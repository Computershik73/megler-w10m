#include "pch.h"

#include "shell/Startup.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <chrono>
#include <condition_variable>

namespace megler::app::startup {

namespace {

std::mutex gMutex;
std::condition_variable gChanged;
std::string gStep = "launching";
bool gLaunched = false, gShown = false;
std::chrono::steady_clock::time_point gLaunch = std::chrono::steady_clock::now();

std::string seconds(uint64_t ms) { return format("%.1f s", double(ms) / 1000.0); }

/// "N s after the launch (M s after the process began)".
std::string since(uint64_t ms) {
    const uint64_t age = processAge();
    return seconds(ms) + " after the launch" + (age ? " (" + seconds(age) + " after the process began)" : std::string());
}

} // namespace

uint64_t processAge() {
    FILETIME created = {}, exited = {}, kernel = {}, user = {}, now = {};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimeAsFileTime(&now);
    const uint64_t from = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const uint64_t to = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return to > from ? (to - from) / 10000 : 0;
}

uint64_t sinceLaunch() {
    std::lock_guard<std::mutex> lock(gMutex);
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gLaunch).count());
}

void launched(std::function<std::string(unsigned threadId)> where, std::vector<unsigned> marks) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gLaunched) return;
        gLaunched = true;
        gLaunch = std::chrono::steady_clock::now();
    }
    const unsigned ui = GetCurrentThreadId();
    std::thread([where = std::move(where), marks = std::move(marks), ui] {
        for (const unsigned mark : marks) {
            std::string step;
            {
                std::unique_lock<std::mutex> lock(gMutex);
                if (gChanged.wait_until(lock, gLaunch + std::chrono::milliseconds(mark), [] { return gShown; })) return;
                step = gStep;
            }
            std::string at;
            try {
                if (where) at = where(ui);
            } catch (...) {
            }
            shellLog::writeNow("start: " + since(mark) + ", the window is not shown yet: still " + step +
                               (at.empty() ? std::string() : "; the UI thread: " + at));
        }
    }).detach();
}

void step(const std::string& what) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gStep = what;
    }
    shellLog::writeNow("start: " + what);
}

std::string current() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gStep;
}

void shown() {
    uint64_t ms = 0;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gShown) return;
        gShown = true;
        gStep = "the window shown";
        ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - gLaunch).count());
    }
    gChanged.notify_all();
    shellLog::writeNow("start: the window is shown, " + since(ms));
}

bool isShown() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gShown;
}

} // namespace megler::app::startup
