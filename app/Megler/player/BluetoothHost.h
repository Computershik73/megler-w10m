// The phone's Bluetooth LE radio, the central's side, for an Android app's android.bluetooth: megler_bluetooth_host
// (core/include/megler.h) over Windows.Devices.Bluetooth. The core and the framework keep Android's side (the devices, the
// GATT objects, their callbacks and broadcasts, core/src/run/mg_bluetooth.c); this does what the radio does and hands each
// answer over as an event (megler_bluetooth_push).
//
// What the phone has is looked for once, on a thread of the multithreaded apartment of this host's own, as soon as the host
// is first asked for (when an app is started): Windows' default Bluetooth adapter, whether it can be an LE central, its
// radio (on or off, and every change after as STATE), the phone's name, and the LE devices it is paired with (PAIRED, as
// each app starts). Every call returns at once: the work is queued to that thread, and what waits for the radio (a
// connection, a discovery, a read, a write, a pairing) goes on in coroutines on Windows' threads, none of it on the UI
// thread. In detail:
//
//   scan          a BluetoothLEAdvertisementWatcher, active or passive. An advertisement and the scan response that follows
//                 it are two of Windows' events, and one record for Android: each is handed over with the device's last
//                 advertisement and its last scan response, their AD structures as Windows got them. A watcher Windows
//                 stops with an error is SCAN_FAILED.
//   connect       the device by its address (and kind, where it was heard), a GattSession that maintains the connection,
//                 and its services asked for afresh, which is what makes Windows connect: CONNECTION 1 once it has them,
//                 or 0 with 133 when it could not reach the device -- or when anything else went wrong: whatever Windows
//                 throws or lacks, the app hears of a connection not made. The session is asked for by the device's id
//                 where Windows has one (Windows 10 1709); Windows 10 Mobile has none, and there the session is the one
//                 of the device's first service, once the services are there (a connection made `auto` is then tried
//                 every few seconds until the device answers, there being no session to wait with). Then Windows'
//                 ConnectionStatus: a connection that drops is CONNECTION 0 with Android's 8 (Windows says not why); with
//                 `auto` Windows makes it again whenever it can (MaintainConnection) and it comes as CONNECTION 1,
//                 without, it is let go.
//   discover      every service's characteristics and descriptors, afresh, by their attribute handles. A characteristic
//                 that notifies or indicates without a Client Characteristic Configuration descriptor among those Windows
//                 lists has one made up for it, which stands for Windows' own call.
//   read, write   the characteristic's or the descriptor's value, Windows' status made Android's (the ATT error as it is,
//                 133 unreachable, 257 refused). A write of a Client Characteristic Configuration descriptor is Windows'
//                 WriteClientCharacteristicConfigurationDescriptor, its ValueChanged subscribed to first: each notification
//                 and indication is CHANGED.
//   mtu           the session's MaxPduSize (waited for a moment while it is still 23), never more than asked.
//   bond          Windows' pairing with a ceremony of the app's (DeviceInformationCustomPairing): each request of the
//                 device's -- a PIN, a confirmation of the number it shows, a number to enter on it, a consent -- is a
//                 PAIRING the app may answer (its setPin); one it leaves to the user (ASK_USER) is Megler's question
//                 for a confirmation or a consent, and Windows' own pairing (its dialog asks for the PIN) for a PIN.
//                 Unpairing is Windows' UnpairAsync.
//   settings      the phone's Bluetooth settings (ms-settings:bluetooth), through the system host.
//
// One BluetoothHost serves the process: shared().
#pragma once

#include "megler.h"

#include <string>

namespace megler::app {

class BluetoothHost {
public:
    static BluetoothHost& shared();
    BluetoothHost(const BluetoothHost&) = delete;
    BluetoothHost& operator=(const BluetoothHost&) = delete;

    /// The callbacks for megler_run_config.bluetooth, with this as their user; the phone is looked at from now on, and the
    /// devices it is paired with are said again to the app that starts now.
    megler_bluetooth_host callbacks();
    /// What was found, in a line (waits for the looking to end).
    std::string summary();

private:
    BluetoothHost() = default;
};

} // namespace megler::app
