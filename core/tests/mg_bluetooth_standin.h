/*
 * A stand-in for the shell's Bluetooth LE radio (megler_bluetooth_host) in the tests: a radio that is on, named "Test Radio",
 * paired before with one device ("Old Radio", 11:22:33:44:55:66), and two devices in range while it scans:
 *
 *   MeshCore-Test  C0:FF:EE:00:00:01, a random address; it takes connections. Its advertisement: flags 06, the Nordic UART
 *                  service's UUID (6e400001-b5a3-f393-e0a9-e50e24dcca9e), its name, the manufacturer 0xFFFF's 01 02 03, and
 *                  a transmit power of -4 dBm; heard at -50 dBm. Its database (handles):
 *                    0x0001  Generic Access (1800): 0x0003 the device's name (2a00), read
 *                    0x0010  the Nordic UART service: 0x0012 RX (6e400002), write and write without response; 0x0014 TX
 *                            (6e400003), notify, its Client Characteristic Configuration descriptor at 0x0015
 *                    0x0020  a service of its own (fff0): 0x0022 (fff1), read, which it answers only once it is bonded
 *                            (before, GATT_INSUFFICIENT_AUTHENTICATION), "s3cret"
 *                  What is written to RX comes back on TX as a notification, while they are on; "drop" written there drops
 *                  the connection (Android's 19, the device ended it) after the write is answered. Pairing asks for a PIN,
 *                  123456; the user, asked, gives it.
 *   Beacon         00:11:22:33:44:55, public, takes no connections: flags 04, the battery service (180f) and its data 0x64,
 *                  its name; heard at -70 dBm.
 *
 * Its answers come a few milliseconds after it is asked, from a thread of its own, as a radio's do; advertisements every
 * 40 ms while it scans. A connection to any other device is never made: without `auto` it fails after 100 ms with Android's
 * 133. The radio can be turned off and on by the tests (mg_bluetooth_standin_radio); settings() turns it on, as a user would
 * in the phone's settings. The counts say what was asked of it.
 */
#ifndef MG_BLUETOOTH_STANDIN_H
#define MG_BLUETOOTH_STANDIN_H

#include "../include/megler.h"

#define MG_BT_STANDIN_MESHCORE 0xC0FFEE000001ull
#define MG_BT_STANDIN_BEACON 0x001122334455ull
#define MG_BT_STANDIN_OLD 0x112233445566ull

typedef struct mg_bluetooth_standin_counts {
    int scans, scans_active, scans_stopped;  /* scans started (of them active), and stops */
    bool scanning;
    int connects, connect_type, disconnects; /* connections asked for, the address's kind the last was asked with, ends */
    int connected;                           /* connections made and not ended */
    int written;                             /* bytes written to RX */
    int cccd;                                /* TX's subscription as written: 1 notify, 2 indicate, 0 none */
    int mtu_asked;
    int bonds, unbonds, pairing_answers, asked_user, settings;
    char pin[16];                            /* the PIN the last answer gave */
} mg_bluetooth_standin_counts;

const megler_bluetooth_host *mg_bluetooth_standin(void);
/* Everything as it was at the start: the radio on, nothing bonded but Old Radio, the counts zero. */
void mg_bluetooth_standin_reset(void);
/* The radio turned on or off, as a user would; STATE is pushed. */
void mg_bluetooth_standin_radio(bool on);
void mg_bluetooth_standin_get(mg_bluetooth_standin_counts *out);

#endif
