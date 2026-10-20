// The device checks: megler_probe and megler_selftest, each run on a thread of
// its own, their lines kept for the Device screen to show as they arrive and
// written to LocalState\megler-probe.log / megler-selftest.log (a line at a time,
// so the Device Portal can copy the file while the check still runs).
//
// One check runs at a time: both drive the same core. The unattended run
// (auto/AutoRun.h) goes through here too, so what it does shows on the screen
// like a check the user started.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace megler::app::checks {

enum class Kind { probe, selftest };

const char* name(Kind kind);
/// "megler-probe.log" / "megler-selftest.log".
const wchar_t* logName(Kind kind);

/// Starts the check on a new thread. False when one is already running, and
/// for a self-test when this process has already had its one app
/// (selfTestStarted(), or session::spent()).
bool start(Kind kind);
/// Runs the check on the calling thread (a background one) and returns its
/// last line ("DONE probe: 23 lines", "DONE selftest: 0 failed"). Waits for a
/// check already running to finish first.
std::string run(Kind kind);

bool running();
/// The check running now; meaningful while running() is true.
Kind current();
/// Whether a self-test has been started since the app was opened. It starts
/// and stops a small app of its own, and that is the one app a process may
/// run (core/include/megler.h): after it, nothing can be played, and it cannot
/// run a second time, until Megler has been closed and opened again.
bool selfTestStarted();

/// Appends to `out` the lines of the newest run of `kind` numbered `cursor`
/// and later, and returns the number after the last. `run` counts that kind's
/// runs: when it changes, the reader's lines are an older run's and its
/// cursor starts again at 0.
uint64_t take(Kind kind, uint64_t cursor, std::vector<std::string>& out, unsigned& run);

} // namespace megler::app::checks
