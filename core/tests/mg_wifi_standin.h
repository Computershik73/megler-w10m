/*
 * A stand-in for the shell's Wi-Fi (megler_wifi_host) in the tests: a phone whose Wi-Fi is on, that hears four networks and
 * is connected to the first of them. Each scan takes 100 ms and finds them as a report made 50 ms before it ends,
 * the signals a little different each time (the scan's number off the first network's), so that one scan's results are
 * told from another's:
 *   00:11:22:33:44:55  "Megler Test"     -45 dBm  2437 MHz (channel 6)    WPA2-Personal, CCMP  802.11n
 *   0a:bb:cc:dd:ee:ff  "Кафе" (UTF-8)    -67 dBm  5180 MHz (channel 36)   open                 802.11ac
 *   02:12:34:56:78:ab  (hidden)          -80 dBm  2412 MHz (channel 1)    WPA3-Personal (SAE)  802.11ax
 *   66:55:44:33:22:11  "Printer_nomap"   -70 dBm  2462 MHz (channel 11)   WPA-Personal, TKIP   802.11g, ad hoc
 * The state can be set (mg_wifi_standin_set_state) to stand for a phone whose Wi-Fi is off or refused; a scan then fails.
 */
#ifndef MG_WIFI_STANDIN_H
#define MG_WIFI_STANDIN_H

#include "../include/megler.h"

#ifdef __cplusplus
extern "C" {
#endif

const megler_wifi_host *mg_wifi_standin(void);
/* The state it answers (MEGLER_WIFI_*), ON as it starts; and the scans made since the last reset. */
void mg_wifi_standin_set_state(int state);
int mg_wifi_standin_scans(void);
void mg_wifi_standin_reset(void);

#ifdef __cplusplus
}
#endif

#endif
