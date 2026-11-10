/* The tests' stand-in for the shell's Wi-Fi: mg_wifi_standin.h. */
#include "mg_wifi_standin.h"

#include <string.h>

#include "../src/mg_core.h"
#include "../src/host/mg_host.h"

static volatile i32 g_state = MEGLER_WIFI_ON, g_scans;

static const struct {
    unsigned long long bssid;
    const char *ssid;
    int rssi, mhz, security, phy, adhoc;
} k_networks[] = {
    { 0x001122334455ull, "Megler Test", -45, 2437, MEGLER_WIFI_SECURITY(MEGLER_WIFI_AUTH_RSNA_PSK, MEGLER_WIFI_CIPHER_CCMP), MEGLER_WIFI_PHY_HT, 0 },
    { 0x0ABBCCDDEEFFull, "\xD0\x9A\xD0\xB0\xD1\x84\xD0\xB5", -67, 5180, MEGLER_WIFI_SECURITY(MEGLER_WIFI_AUTH_OPEN, MEGLER_WIFI_CIPHER_NONE), MEGLER_WIFI_PHY_VHT, 0 },
    { 0x0212345678ABull, "", -80, 2412, MEGLER_WIFI_SECURITY(MEGLER_WIFI_AUTH_WPA3_SAE, MEGLER_WIFI_CIPHER_CCMP), MEGLER_WIFI_PHY_HE, 0 },
    { 0x665544332211ull, "Printer_nomap", -70, 2462, MEGLER_WIFI_SECURITY(MEGLER_WIFI_AUTH_WPA_PSK, MEGLER_WIFI_CIPHER_TKIP), MEGLER_WIFI_PHY_ERP, 1 },
};

static void fill(megler_wifi_network *n, int i, int scan)
{
    memset(n, 0, sizeof(*n));
    n->bssid = k_networks[i].bssid;
    n->ssid_size = (int)strlen(k_networks[i].ssid);
    memcpy(n->ssid, k_networks[i].ssid, (size_t)n->ssid_size);
    /* (The first network a little weaker at each scan: one scan's results told from another's.) */
    n->rssi_dbm = k_networks[i].rssi - (i == 0 ? scan % 8 : 0);
    n->frequency_mhz = k_networks[i].mhz;
    n->security = k_networks[i].security;
    n->phy = k_networks[i].phy;
    n->adhoc = k_networks[i].adhoc;
}

static MEGLER_CB int state_fn(void *user, bool ask)
{
    (void)user;
    (void)ask;
    return (int)g_state;
}

static MEGLER_CB int scan_fn(void *user, megler_wifi_network *out, int max, long long *time_ns)
{
    int i, n = 0, scan;
    (void)user;
    if (g_state != MEGLER_WIFI_ON) return -1;
    scan = (int)mg_atomic_add(&g_scans, 1);
    /* A scan takes a moment, as Windows' takes a few seconds. */
    mg_sleep_ns(100000000ull);
    for (i = 0; i < (int)MG_COUNT(k_networks) && n < max; i++) fill(&out[n++], i, scan);
    /* (Windows' report is made as its scan ends: here 50 ms before the call returns, after it began.) */
    *time_ns = (long long)mg_time_realtime_ns() - 50000000ll;
    return n;
}

static MEGLER_CB bool connection_fn(void *user, megler_wifi_network *out)
{
    (void)user;
    if (g_state != MEGLER_WIFI_ON) return false;
    fill(out, 0, 0);
    return true;
}

static const megler_wifi_host k_host = { NULL, state_fn, scan_fn, connection_fn };

const megler_wifi_host *mg_wifi_standin(void) { return &k_host; }

void mg_wifi_standin_set_state(int state) { mg_atomic_xchg(&g_state, state); }

int mg_wifi_standin_scans(void) { return (int)g_scans; }

void mg_wifi_standin_reset(void)
{
    mg_atomic_xchg(&g_state, MEGLER_WIFI_ON);
    mg_atomic_xchg(&g_scans, 0);
}
