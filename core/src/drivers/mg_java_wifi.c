/*
 * megler.WifiNative: the phone's Wi-Fi behind a Java app's android.net.wifi.WifiManager, over the shell's (megler_wifi_host,
 * core/include/megler.h). The Java side (framework-overlay/megler/Wifi.java) keeps the scans' results, decides when the
 * phone is scanned, makes Android's ScanResults and sends the broadcast; this asks the host: whether the phone can scan, a
 * scan, and the network it is connected to.
 *
 * The log says what the host answers as it changes, and what scans find: each of the first eight -- how many networks, how
 * many of them on 5 GHz, the strongest signal, how long Windows took -- and then once a minute while scans are made, how
 * many and what they found, so that a phone's log shows whether the Wi-Fi is heard without a line for every scan.
 */
#include "mg_java.h"

#include <stdio.h>
#include <string.h>

#include "../dvm/mg_dvm.h"
#include "../host/mg_host.h"

#define N(name) static void name(mg_jcall *c)

/* At most this many networks a scan: Android keeps a few hundred; a city street hears dozens. */
#define MAX_NETWORKS 128
#define SAID_SCANS 8

static megler_wifi_host g_host;
static mg_lock g_lock = MG_LOCK_INIT;
static int g_said_state = -1;
/* The scans said one by one, and the minute's summary after them. */
static int g_scans;
static u64 g_minute_from;
static int g_minute_scans, g_minute_failed, g_minute_most;

void mg_java_wifi_set_host(const megler_wifi_host *host)
{
    mg_lock_acquire(&g_lock);
    if (host) g_host = *host;
    else memset(&g_host, 0, sizeof(g_host));
    g_said_state = -1;
    g_scans = 0;
    g_minute_from = 0;
    g_minute_scans = g_minute_failed = g_minute_most = 0;
    mg_lock_release(&g_lock);
}

static const char *state_name(int s)
{
    switch (s) {
    case MEGLER_WIFI_ON: return "on: the phone's Wi-Fi can be scanned";
    case MEGLER_WIFI_OFF: return "off: the phone's Wi-Fi is switched off";
    case MEGLER_WIFI_DENIED: return "denied: Windows does not let Megler have the Wi-Fi (the package's wiFiControl, or a policy)";
    case MEGLER_WIFI_UNKNOWN: return "not asked yet";
    default: return "none: the phone has no Wi-Fi adapter";
    }
}

int mg_jwifi_state(bool ask)
{
    int s = g_host.state ? g_host.state(g_host.user, ask) : MEGLER_WIFI_NONE;
    bool say;
    mg_lock_acquire(&g_lock);
    say = s != g_said_state;
    g_said_state = s;
    mg_lock_release(&g_lock);
    if (say) mg_info("wifi: %s%s", state_name(s), ask ? " (asked)" : "");
    return s;
}

int mg_jwifi_scan(megler_wifi_network *out, int max, long long *time_ns)
{
    u64 t0 = mg_time_monotonic_ns(), now;
    int n = -1, i, five = 0, strongest = -1000, said, summary_scans = 0, summary_failed = 0, summary_most = 0;
    long long summary_s = 0;
    *time_ns = 0;
    if (max > 0 && g_host.scan) n = g_host.scan(g_host.user, out, max, time_ns);
    if (n > max) n = max;
    now = mg_time_monotonic_ns();
    for (i = 0; i < n; i++) {
        if (out[i].frequency_mhz >= 4900 && out[i].frequency_mhz < 5950) five++;
        if (out[i].rssi_dbm > strongest) strongest = out[i].rssi_dbm;
        if (out[i].ssid_size < 0) out[i].ssid_size = 0;
        if (out[i].ssid_size > 32) out[i].ssid_size = 32;
    }
    mg_lock_acquire(&g_lock);
    said = g_scans++;
    if (!g_minute_from) g_minute_from = now;
    g_minute_scans++;
    if (n < 0) g_minute_failed++;
    if (n > g_minute_most) g_minute_most = n;
    if (said >= SAID_SCANS && now - g_minute_from >= 60000000000ull) {
        summary_scans = g_minute_scans;
        summary_failed = g_minute_failed;
        summary_most = g_minute_most;
        summary_s = (long long)((now - g_minute_from) / 1000000000ull);
        g_minute_from = now;
        g_minute_scans = g_minute_failed = g_minute_most = 0;
    }
    if (said < SAID_SCANS) {
        /* (The scans said one by one count for no minute.) */
        g_minute_from = now;
        g_minute_scans = g_minute_failed = g_minute_most = 0;
    }
    mg_lock_release(&g_lock);
    if (said < SAID_SCANS) {
        char best[40] = "";
        if (n > 0) snprintf(best, sizeof(best), ", the strongest at %d dBm", strongest);
        if (n < 0) mg_info("wifi: scan %d could not be made (%s)", said + 1, g_host.scan ? "the shell's scan failed or the Wi-Fi is not to be had" : "no Wi-Fi here");
        else
            mg_info("wifi: scan %d found %d network%s (%d on 5 GHz)%s, in %llu ms", said + 1, n, n == 1 ? "" : "s", five, best,
                    (unsigned long long)((now - t0) / 1000000ull));
        if (said + 1 == SAID_SCANS) mg_info("wifi: the scans after these are said once a minute");
    } else if (summary_scans) {
        mg_info("wifi: %d scan%s in the last %lld s, %d of them failed; %d networks at most", summary_scans, summary_scans == 1 ? "" : "s", summary_s, summary_failed,
                summary_most);
    }
    return n;
}

bool mg_jwifi_connection(megler_wifi_network *out)
{
    memset(out, 0, sizeof(*out));
    return g_host.connection && g_host.connection(g_host.user, out);
}

/* What scan() and connection() hand Java, by index in meta (megler.WifiNative's M_*). */
enum { M_COUNT, M_TIME, M_COUNT_OF };
/* Each network's numbers in the int[] (megler.WifiNative's I_*). */
enum { I_SSID_SIZE, I_RSSI, I_FREQUENCY, I_SECURITY, I_PHY, I_ADHOC, I_STRIDE };

/* The networks as {long[] bssids, byte[] ssids (32 bytes each), int[] numbers (I_STRIDE each)}. */
static mg_jobj *networks_of(const megler_wifi_network *v, int n)
{
    mg_jobj *out = mg_dvm_new_array("[Ljava/lang/Object;", 3), *b, *s, *ints;
    int i;
    if (!out) return NULL;
    b = mg_dvm_new_array("[J", n);
    s = mg_dvm_new_array("[B", n * 32);
    ints = mg_dvm_new_array("[I", n * I_STRIDE);
    if (!b || !s || !ints) return NULL;
    for (i = 0; i < n; i++) {
        i32 *p = (i32 *)ints->u.arr.data + i * I_STRIDE;
        ((i64 *)b->u.arr.data)[i] = (i64)(v[i].bssid & 0xFFFFFFFFFFFFull);
        memcpy((u8 *)s->u.arr.data + i * 32, v[i].ssid, 32);
        p[I_SSID_SIZE] = v[i].ssid_size;
        p[I_RSSI] = v[i].rssi_dbm;
        p[I_FREQUENCY] = v[i].frequency_mhz;
        p[I_SECURITY] = v[i].security;
        p[I_PHY] = v[i].phy;
        p[I_ADHOC] = v[i].adhoc;
    }
    out->u.oarr.v[0] = b;
    out->u.oarr.v[1] = s;
    out->u.oarr.v[2] = ints;
    return out;
}

/* state(boolean ask): MEGLER_WIFI_* */
N(Wifi_state) { c->ret.i = mg_jwifi_state(c->args[0].z != 0); }

/*
 * scan(long[] meta): a scan, waited for; meta[M_COUNT] the networks' number (-1 for no scan), meta[M_TIME] when Windows' report
 * was made (ns since 1970); the networks as networks_of has them, null for no scan.
 */
N(Wifi_scan)
{
    mg_jobj *meta = c->args[0].l;
    static megler_wifi_network nets[MAX_NETWORKS];
    static mg_lock scanning = MG_LOCK_INIT;
    long long time_ns = 0;
    int n;
    c->ret.l = NULL;
    if (!meta || meta->u.arr.len < M_COUNT_OF) return;
    /* (One scan at a time: there is one thread of the framework's that asks, and the array is the core's.) */
    mg_lock_acquire(&scanning);
    memset(nets, 0, sizeof(nets));
    n = mg_jwifi_scan(nets, MAX_NETWORKS, &time_ns);
    ((i64 *)meta->u.arr.data)[M_COUNT] = n;
    ((i64 *)meta->u.arr.data)[M_TIME] = time_ns;
    if (n >= 0) c->ret.l = networks_of(nets, n);
    mg_lock_release(&scanning);
}

/* connection(long[] meta): the network the phone is connected to by Wi-Fi as networks_of has one, null when it is not on Wi-Fi. */
N(Wifi_connection)
{
    mg_jobj *meta = c->args[0].l;
    megler_wifi_network net;
    c->ret.l = NULL;
    if (!meta || meta->u.arr.len < M_COUNT_OF) return;
    if (!mg_jwifi_connection(&net)) return;
    ((i64 *)meta->u.arr.data)[M_COUNT] = 1;
    ((i64 *)meta->u.arr.data)[M_TIME] = 0;
    c->ret.l = networks_of(&net, 1);
}

static const mg_jhle k_wifi[] = {
    { "megler/WifiNative", "state", NULL, Wifi_state },
    { "megler/WifiNative", "scan", NULL, Wifi_scan },
    { "megler/WifiNative", "connection", NULL, Wifi_connection },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_wifi_natives(void) { return k_wifi; }
