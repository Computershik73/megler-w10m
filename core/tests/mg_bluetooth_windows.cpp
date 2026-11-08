// The test program's way to the shell's Bluetooth: megler_bluetooth_host as app/Megler/player/BluetoothHost.cpp implements
// it over Windows.Devices.Bluetooth, for mg_tests --bluetooth-windows (mg_tests_main.c), which says what radio this PC has
// through it and the core's, and listens passively a moment: nothing is sent, nothing connected to, nothing paired with.
//
// What that shows: the host's own work -- Windows' adapter and radio found, the devices it is paired with, a watcher
// started and stopped, the advertisements handed over -- against this PC's Windows. A PC without Bluetooth says so.
//
// Only tools/build-tests.sh win builds this file, with BluetoothHost.cpp itself; the rest of the shell it uses is stood in
// for here (the system host: its question to the user and its links) and by mg_storage_windows.cpp (the log).
#include "pch.h"

#include "player/BluetoothHost.h"
#include "player/SystemHost.h"

#include <cstdio>

namespace megler::app::systemHost {

// (No one to ask in the tests, and no settings to show.)
int askUser(const std::string&, const std::string&, const std::vector<std::string>&) { return -1; }
megler_system_host callbacks() { return megler_system_host{}; }

} // namespace megler::app::systemHost

extern "C" {

const megler_bluetooth_host* mg_bluetooth_windows_host(void) {
    static megler_bluetooth_host host;
    host = megler::app::BluetoothHost::shared().callbacks();
    return &host;
}

/// What Windows has, in a line.
void mg_bluetooth_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", megler::app::BluetoothHost::shared().summary().c_str()); }

} // extern "C"
