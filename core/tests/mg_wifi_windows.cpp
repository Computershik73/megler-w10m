// The test program's way to the shell's Wi-Fi: megler_wifi_host as app/Megler/player/WifiHost.cpp implements it over
// Windows.Devices.WiFi, for mg_tests --wifi-windows (mg_tests_main.c), which says what Wi-Fi this PC has through it and the
// core's, scans once and says what the scan heard, and the network the PC is connected to.
//
// What that shows: the host's own work -- Windows' access, the adapter and its radio found, a scan made and its report read,
// each network's numbers, the connected profile -- against this PC's Windows. A PC without Wi-Fi says so.
//
// Only tools/build-tests.sh win builds this file, with WifiHost.cpp itself; the shell's log it writes to is stood in for by
// mg_storage_windows.cpp.
#include "pch.h"

#include "player/WifiHost.h"

#include <cstdio>

extern "C" {

const megler_wifi_host* mg_wifi_windows_host(void) {
    static megler_wifi_host host;
    host = megler::app::WifiHost::shared().callbacks();
    return &host;
}

/// What Windows has, in a line.
void mg_wifi_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", megler::app::WifiHost::shared().summary().c_str()); }

} // extern "C"
