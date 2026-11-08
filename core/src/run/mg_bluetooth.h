/*
 * The phone's Bluetooth LE radio, the central's side, for an app's android.bluetooth (megler.h, megler_bluetooth_host):
 * what the core keeps between the shell's radio and the framework's Java -- the radio's state, the devices heard and paired,
 * the connections the app has open, and one queue of what the radio did, which the framework's thread for Bluetooth reads
 * (drivers/mg_java_bluetooth.c, megler.Bluetooth). Android's objects, callbacks and broadcasts are the framework's
 * (core/framework-overlay/android/bluetooth).
 */
#ifndef MG_BLUETOOTH_H
#define MG_BLUETOOTH_H

#include "../../include/megler.h"
#include "../mg_base.h"

/* The run's radio (mg_run.c), or NULL for none: what was there before is forgotten. */
void mg_bluetooth_set_host(const megler_bluetooth_host *host);
/* The app has ended: its scan stopped and its connections closed at the shell, what waits dropped, nothing more taken. */
void mg_bluetooth_end(void);

/* MEGLER_BT_RADIO_*, and its name into `name` (may be NULL). The shell is asked once; changes come as events. */
int mg_bluetooth_state(char *name, size_t size);
/* MEGLER_BT_SCAN_*: the radio's scan as the app's scans want it. False when there is no radio. */
bool mg_bluetooth_scan(int mode);
/*
 * A GATT connection to the device: `conn` 0 for a new one (its number is returned), or one that was made before and
 * dropped (BluetoothGatt.connect()). 0 when there is no radio.
 */
int mg_bluetooth_connect(u64 address, int address_type, bool auto_connect, int conn);
/* The connection ended from this side; with `close`, forgotten: nothing more of it is handed to the app (BluetoothGatt.close()). */
void mg_bluetooth_disconnect(int conn, bool close);
bool mg_bluetooth_discover(int conn);
bool mg_bluetooth_read(int conn, int handle);
bool mg_bluetooth_write(int conn, int handle, const u8 *data, int size, int type);
bool mg_bluetooth_mtu(int conn, int mtu);
/* The connection's RSSI: the shell's, or where it has none the last advertisement's of the device (an RSSI event either way). */
bool mg_bluetooth_rssi(int conn);
bool mg_bluetooth_bond(u64 address, int address_type, bool create);
void mg_bluetooth_pairing_reply(u64 address, int answer, const char *pin);
/* The phone's Bluetooth settings, for the user to turn the radio on. False with no shell to show them. */
bool mg_bluetooth_settings(void);

/* What is known of a device: its bond state (MEGLER_BT_BOND_*), its address's kind (MEGLER_BT_ADDRESS_UNKNOWN when it was never heard), its name. */
int mg_bluetooth_bond_state(u64 address);
int mg_bluetooth_address_type(u64 address);
bool mg_bluetooth_name(u64 address, char *name, size_t size);
/* The devices paired with the phone, and those the app has a connection to: up to `max` addresses into `out`, their count returned. */
int mg_bluetooth_bonded(u64 *out, int max);
int mg_bluetooth_connected(u64 *out, int max);

/*
 * The next thing the radio did, for the framework: waited for up to `wait_ms` (0: not at all, -1: for ever). A copy, which
 * mg_bluetooth_free gives back; NULL when nothing came. Events of a connection the app has closed are not handed over.
 */
megler_bluetooth_event *mg_bluetooth_next(int wait_ms);
void mg_bluetooth_free(megler_bluetooth_event *e);

/* "AA:BB:CC:DD:EE:FF", as Android writes an address, into `out` (18 bytes). */
void mg_bluetooth_address_text(u64 address, char *out);

#endif /* MG_BLUETOOTH_H */
