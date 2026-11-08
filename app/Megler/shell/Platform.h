// What the shell tells the core about the phone at start-up (megler_init), and
// the facts the Settings screen shows: where the data lives, the memory
// ceiling, whether this process may run Android code, whether the package
// carries ANGLE.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace megler::app::platform {

bool isMobile();
/// An Xbox (in Developer Mode, the only way Megler is on one): a television, a gamepad, the console's Device Portal. Or a
/// PC's Megler under the flag as-xbox (asXbox).
bool isXbox();
/// The flag "as-xbox" (a file of that name in LocalState\flags, read once a start) on a PC: Megler takes an Xbox's ways
/// where the shell has ways of its own for one (isXbox) -- its window opened at 960 by 540, an Xbox's television in an
/// app's pixels, and never resized or turned for an app's wish; the log's ZIP kept in LocalState -- to try them without a
/// console. What Windows does otherwise on an Xbox (200 percent, the television's safe area) a PC does not. The package
/// Megler updates itself with is still the PC's (ui/Ota.cpp goes by the device family).
bool asXbox();
std::string deviceFamily();
std::string osVersion();
std::string packageVersion();
std::string architecture();
/// LocalState, where everything the app keeps lives.
const std::wstring& localStateDirectory();
/// The installed package's folder.
const std::wstring& installDirectory();
/// "390 MB limit, 85 MB in use" from MemoryManager, or why not.
std::string memorySummary();
/// "NOKIA RM-976_1018, 512 MB of memory, 96.3 MB of it free": the device as Windows names it (EasClientDeviceInformation),
/// and its memory (GlobalMemoryStatusEx).
std::string deviceSummary();
/// "LocalState's 1.20 GB free of 3.56 GB", and the package's and the shared folder's where they are on other disks.
std::string diskSummary();
/// Reads the app's memory use and limit for the core to hand to the app
/// (Platform.cpp). On the shell's own thread: at start-up and on its tick.
void sampleMemory();

/// megler_init: the log's sink, LocalState as the data folder, the install
/// folder as the package's, MemoryManager behind the memory callbacks. Once,
/// on the UI thread, after shellLog::initialize.
void initializeCore();
/// What megler_init returned: false when the phone did not grant
/// codeGeneration, so no Android code can run (APKs can still be read).
bool canRunAndroidCode();

struct AngleStatus {
    bool present = false;
    /// "In the package", or "ANGLE missing: libEGL.dll, ...".
    std::string summary;
};
/// Whether the package carries ANGLE and what it loads with
/// (tools/build-appx.sh, "ANGLE"): looked for as files, nothing is loaded.
const AngleStatus& angle();

/// Loads ANGLE's DLLs one by one with LoadPackagedLibrary, in the order they
/// depend on each other, and says for each whether the loader took it and
/// with which error if not -- 126 is a DLL (or one it imports) not found, 127
/// a function missing from one, 193 a wrong architecture. The first phone
/// run's question about the packaging, answered in the probe's own log: each
/// answer goes to `line` before the next DLL is tried, so a load that ends
/// the process still leaves the ones before it on record.
void angleLoadCheck(const std::function<void(const std::string&)>& line);

/// Runs `work` on a new thread with a stack of `stackBytes`; std::thread has
/// no say in the size. The thread is never joined.
bool startThread(std::function<void()> work, size_t stackBytes);

} // namespace megler::app::platform
