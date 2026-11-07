// Save All Logs (the Log screen, in Megler and in an app's own package): every package's logs in one ZIP, for a problem
// report sent from the phone itself -- what the PC's kit of 2026-11-07 (megler-logs.ps1) took off a phone through its
// Device Portal, which a phone need not have on any more.
//
// The ZIP (shell/ZipWriter.h), megler-logs-<yyyyMMdd-HHmmss>.zip:
//   summary.txt          Megler's version, the processor, the device and Windows, memory, whether Android code can run,
//                        ANGLE, Megler's packages as Windows lists them, free space, the time, the last install refusal,
//                        and what the ZIP holds
//   <identity>/...       this package's files (logMirror::names() that are there), from its own LocalState, the newest;
//                        flags.txt, its flags; package.txt, what it is
//   <identity>/...       every other package's copies, <shared>\logs\<identity> (shell/LogMirror.h), as they are
//   shared/settings.ini, shared/apps/<package>/settings.ini
//                        the settings every package reads (shell/Settings.h)
// A file over logMirror::capBytes is taken as its last part, a line first saying what was left out. Made in
// LocalState\logs, then copied to Pictures\Megler\logs, which a phone's file manager and a PC over USB both reach (on a PC,
// the user's Pictures), and taken out of LocalState. Where the Pictures folder cannot be had -- an Xbox, an app's own
// package that does not declare it -- it stays in LocalState\logs (the newest three are kept there), and the Log screen
// says so; Share sends it from either place.
#pragma once

#include <cstdint>
#include <string>

namespace megler::app::logArchive {

/// Where saving stands, for the Log screen's poll.
struct State {
    bool running = false;
    std::string step;              // what is being done, while it runs
    uint64_t done = 0, total = 0;  // bytes of the files read, and to be read
    bool saved = false;            // the last saving made its ZIP
    std::string where;             // the folder it is in, as the user finds it: "Pictures\Megler\logs"
    std::string name;              // its file's name
    uint64_t size = 0;             // its size
    unsigned files = 0;            // the files in it
    std::string note;              // why it is not in Pictures\Megler\logs, when it is not
    std::string error;             // why the last saving made none
    unsigned generation = 0;       // goes up with every change but the bytes read
};
State state();
/// The ZIP saved last, for Share; null before one is.
winrt::Windows::Storage::StorageFile file();
/// Saving begun, on a thread of its own; false while one is under way.
bool start();

} // namespace megler::app::logArchive
