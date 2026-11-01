// The log: every line the core reports through megler_config.log and the
// shell's own, in LocalState\megler.log -- one file, so a phone run reads as
// one story over the Device Portal (tools/phone.py logs) -- and in a ring the
// Log screen shows. The file starts afresh at each launch; the run before is
// kept beside it as megler.prev.log.
//
// megler-boot.txt is the earliest mark, written with nothing but Win32 before
// XAML starts: a phone that has it but no megler.log died in XAML; one with
// neither died in the loader (the diagnosis gecko-w10m's and podium-w10m's
// shells make the same way).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace megler::app::shellLog {

/// How many lines the ring keeps for the Log screen.
constexpr size_t ringCapacity = 2000;

/// Moves LocalState\megler.log to megler.prev.log and starts a new one. Lines
/// written earlier are kept in memory and written out then.
void initialize(const std::wstring& localStateDirectory);

/// A line of the shell's own.
void write(const std::string& line);
/// The lines not in the file yet written out now (they go every 200 ms, and at
/// once for a fault's report): before suspending and exiting.
void flush();
/// megler_config.log's sink: a line of the core's, from any thread.
void core(int level, const char* line, void* user);

std::string path();

/// Appends to `out` the lines numbered `cursor` and later that the ring still
/// holds, and returns the number after the last one: a reader that passes it
/// back next time sees every line once.
uint64_t take(uint64_t cursor, std::vector<std::string>& out);

/// A line in LocalState\megler-boot.txt, from Win32 alone.
void bootMark(const char* what);

} // namespace megler::app::shellLog
