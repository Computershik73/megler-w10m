// The start, step by step: each step said in the log before it begins, the log written out with it
// (shellLog::writeNow), and, until the window is shown, a watch that says now and then which step the start is on and
// where the UI thread stands. Windows ends an app whose window it has not been shown in time without a word to the app,
// and the log then stops where it stops: a Lumia 630's Megler 0.1.0.671 ended so at three starts in a row, each log
// ending with the phone's folders, which another thread had looked up, and nothing of what the UI thread was doing --
// copying the package's Google APKs, hundreds of megabytes, before the window was shown (library/Library.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace megler::app::startup {

/// The launch, on the UI thread, whose id the watch keeps to look at: the watch begins. `where` says in a line where a
/// thread stands (megler_thread_where), or nothing when it cannot; `marks` are when the watch speaks while the window is
/// not shown, in ms after this call. Once.
void launched(std::function<std::string(unsigned threadId)> where,
              std::vector<unsigned> marks = {2000, 5000, 10000, 15000, 20000, 30000, 45000, 60000});
/// The step the start is on from now, said in the log ("start: <what>") and the log written out at once.
void step(const std::string& what);
/// The step the start is on.
std::string current();
/// The window shown (Window::Activate): said with how long the start took, and the watch ends. Once.
void shown();
bool isShown();
/// Milliseconds since launched().
uint64_t sinceLaunch();
/// Milliseconds since the process began (its creation time, as Windows keeps it); 0 when Windows does not say.
uint64_t processAge();

} // namespace megler::app::startup
