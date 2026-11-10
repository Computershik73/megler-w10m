// The phone's Wi-Fi for an Android app's android.net.wifi.WifiManager: megler_wifi_host (core/include/megler.h) over
// Windows.Devices.WiFi. The framework keeps Android's side (its scans' results as ScanResults, the broadcast, the rules of
// who may have them: megler/Wifi.java); this does what the Wi-Fi does: whether the phone can scan, a scan, and the network it
// is connected to. Google Play services in the app finds the phone's location from a scan's networks, as on Android.
//
//   state      Windows' access to the Wi-Fi (WiFiAdapter.RequestAccessAsync, asked when the core says so: nothing is shown
//              to the user for it; the package declares wiFiControl), its first Wi-Fi adapter (FindAllAdaptersAsync), and
//              its radio's state (Windows.Devices.Radios, where Windows tells it): ON, OFF, DENIED, NONE or UNKNOWN. Looked
//              at again after a scan fails, and at most every ten seconds while it is not ON.
//   scan       WiFiAdapter.ScanAsync, waited for ten seconds at most, then its NetworkReport: each network's BSSID (Windows
//              writes "aa:bb:cc:dd:ee:ff"), its SSID (Windows gives text, which is handed over as UTF-8: a name of other bytes
//              comes as Windows decoded it), NetworkRssiInDecibelMilliwatts, ChannelCenterFrequencyInKilohertz, its
//              SecuritySettings' authentication and cipher, PhyKind and NetworkKind, and the report's Timestamp.
//   connection the network adapter's connected profile (GetConnectedProfileAsync) where it is a Wi-Fi one: its SSID
//              (WlanConnectionProfileDetails.GetConnectedSsid); its BSSID, signal, channel and security those of the
//              strongest access point of that SSID in the last scan (Windows says not which one the phone is associated
//              with), else the signal from the profile's bars. Kept ten seconds: the core asks for it from an app's
//              getConnectionInfo, which some apps call over and over.
//
// The core's calls come on the framework's own thread for the Wi-Fi ("megler-wifi") and on the app's threads for the
// connection; what needs WinRT runs on this host's own thread of the multithreaded apartment while the caller waits. The log
// says what Windows answers of the access, the adapter and the radio as it changes, and a scan or a connection that fails;
// the core says what scans find (core/src/drivers/mg_java_wifi.c).
//
// One WifiHost serves the process: shared().
#pragma once

#include "megler.h"

#include <string>

namespace megler::app {

class WifiHost {
public:
    static WifiHost& shared();
    WifiHost(const WifiHost&) = delete;
    WifiHost& operator=(const WifiHost&) = delete;

    /// The callbacks for megler_run_config.wifi, with this as their user.
    megler_wifi_host callbacks();
    /// What Windows says of the Wi-Fi, in a line (Windows asked for access first if it has not been).
    std::string summary();

private:
    WifiHost() = default;
};

} // namespace megler::app
