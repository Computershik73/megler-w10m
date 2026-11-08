/* The tests' stand-in for the shell's Bluetooth LE radio: mg_bluetooth_standin.h. */
#include "mg_bluetooth_standin.h"

#include <string.h>

#include "../src/mg_core.h"
#include "../src/host/mg_host.h"

#define H_NAME 0x0003
#define H_NUS 0x0010
#define H_RX 0x0012
#define H_TX 0x0014
#define H_CCCD 0x0015
#define H_SECRET 0x0022
#define MAX_JOBS 256
#define MAX_CONNS 16
#define DATA_MAX 64

/* UUIDs as written: the Bluetooth base with 16 bits in it, and the Nordic UART service's. */
#define BASE16(hi, lo) { 0x00, 0x00, hi, lo, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb }
#define NUS(b3) { 0x6e, 0x40, 0x00, b3, 0xb5, 0xa3, 0xf3, 0x93, 0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e }

static const megler_bluetooth_attribute k_db[] = {
    { MEGLER_BT_ATTR_SERVICE, 0x0001, BASE16(0x18, 0x00), 0 },
    { MEGLER_BT_ATTR_CHARACTERISTIC, H_NAME, BASE16(0x2a, 0x00), 0x02 },
    { MEGLER_BT_ATTR_SERVICE, H_NUS, NUS(0x01), 0 },
    { MEGLER_BT_ATTR_CHARACTERISTIC, H_RX, NUS(0x02), 0x0C },
    { MEGLER_BT_ATTR_CHARACTERISTIC, H_TX, NUS(0x03), 0x10 },
    { MEGLER_BT_ATTR_DESCRIPTOR, H_CCCD, BASE16(0x29, 0x02), 0 },
    { MEGLER_BT_ATTR_SERVICE, 0x0020, BASE16(0xff, 0xf0), 0 },
    { MEGLER_BT_ATTR_CHARACTERISTIC, H_SECRET, BASE16(0xff, 0xf1), 0x02 },
};

/* MeshCore-Test's advertisement and scan response, AD structure by AD structure; the beacon's. */
static const u8 k_meshcore_ad[] = { 0x02, 0x01, 0x06,
                                    0x11, 0x07, 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e,
                                    0x0e, 0x09, 'M', 'e', 's', 'h', 'C', 'o', 'r', 'e', '-', 'T', 'e', 's', 't',
                                    0x06, 0xff, 0xff, 0xff, 0x01, 0x02, 0x03,
                                    0x02, 0x0a, 0xfc };
static const u8 k_beacon_ad[] = { 0x02, 0x01, 0x04, 0x03, 0x03, 0x0f, 0x18, 0x04, 0x16, 0x0f, 0x18, 0x64, 0x07, 0x09, 'B', 'e', 'a', 'c', 'o', 'n' };

typedef struct job {
    u64 due;
    megler_bluetooth_event e;
    u8 data[DATA_MAX];
    bool with_db;
    bool used;
} job;

static mg_lock g_lock = MG_LOCK_INIT;
static job g_jobs[MAX_JOBS];
static bool g_thread, g_radio = true, g_bonded, g_pairing, g_said_paired;
static int g_scan;
static u64 g_next_ad;
static struct {
    int conn;
    bool connected, auto_connect;
    u64 address;
} g_conns[MAX_CONNS];
static mg_bluetooth_standin_counts g_counts;

static void worker_start_locked(void);

/* An event pushed `ms` from now, its bytes copied. */
static void later_locked(int ms, const megler_bluetooth_event *e, const void *data, int size, bool with_db)
{
    int i;
    for (i = 0; i < MAX_JOBS && g_jobs[i].used; i++) {
    }
    if (i == MAX_JOBS) return;
    memset(&g_jobs[i], 0, sizeof(job));
    g_jobs[i].used = true;
    g_jobs[i].due = mg_time_monotonic_ns() + (u64)ms * 1000000ull;
    g_jobs[i].e = *e;
    if (data && size > 0) {
        if (size > DATA_MAX) size = DATA_MAX;
        memcpy(g_jobs[i].data, data, (size_t)size);
        g_jobs[i].e.data = NULL;
        g_jobs[i].e.size = size;
    } else {
        g_jobs[i].e.size = 0;
    }
    g_jobs[i].with_db = with_db;
    worker_start_locked();
}

static void event_locked(int ms, int kind, int conn, int status, int handle, int value, const void *data, int size)
{
    megler_bluetooth_event e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.conn = conn;
    e.status = status;
    e.handle = handle;
    e.value = value;
    later_locked(ms, &e, data, size, false);
}

static void device_event_locked(int ms, int kind, u64 address, int value, int status, int extra)
{
    megler_bluetooth_event e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.address = address;
    e.address_type = address == MG_BT_STANDIN_MESHCORE ? MEGLER_BT_ADDRESS_RANDOM : MEGLER_BT_ADDRESS_PUBLIC;
    e.value = value;
    e.status = status;
    e.extra = extra;
    later_locked(ms, &e, NULL, 0, false);
}

static void advertise(u64 address, int rssi, bool connectable, const u8 *ad, int size)
{
    megler_bluetooth_event e;
    memset(&e, 0, sizeof(e));
    e.kind = MEGLER_BT_EVENT_ADVERTISEMENT;
    e.address = address;
    e.address_type = address == MG_BT_STANDIN_MESHCORE ? MEGLER_BT_ADDRESS_RANDOM : MEGLER_BT_ADDRESS_PUBLIC;
    e.value = rssi;
    e.extra = connectable;
    e.data = ad;
    e.size = size;
    megler_bluetooth_push(&e);
}

static MG_HOSTCB void worker(void *arg)
{
    (void)arg;
    for (;;) {
        job run;
        bool have = false, ads = false;
        u64 now = mg_time_monotonic_ns();
        int i, pick = -1;
        mg_lock_acquire(&g_lock);
        for (i = 0; i < MAX_JOBS; i++)
            if (g_jobs[i].used && g_jobs[i].due <= now && (pick < 0 || g_jobs[i].due < g_jobs[pick].due)) pick = i;
        if (pick >= 0) {
            run = g_jobs[pick];
            g_jobs[pick].used = false;
            have = true;
        }
        if (g_scan && g_radio && now >= g_next_ad) {
            g_next_ad = now + 40000000ull;
            ads = true;
        }
        mg_lock_release(&g_lock);
        if (have) {
            if (run.e.size) run.e.data = run.data;
            if (run.with_db) {
                run.e.attributes = k_db;
                run.e.count = (int)MG_COUNT(k_db);
            }
            megler_bluetooth_push(&run.e);
            continue;
        }
        if (ads) {
            advertise(MG_BT_STANDIN_MESHCORE, -50, true, k_meshcore_ad, (int)sizeof(k_meshcore_ad));
            advertise(MG_BT_STANDIN_BEACON, -70, false, k_beacon_ad, (int)sizeof(k_beacon_ad));
        }
        mg_sleep_ns(2000000);
    }
}

static void worker_start_locked(void)
{
    if (g_thread) return;
    g_thread = true;
    mg_thread_start(worker, NULL, 128 * 1024, "bluetooth stand-in");
}

static int conn_index_locked(int conn)
{
    int i;
    for (i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].conn == conn) return i;
    return -1;
}

static MEGLER_CB int state_fn(void *user, char *name, int size)
{
    (void)user;
    if (name && size > 0) mg_strlcpy(name, "Test Radio", (size_t)size);
    mg_lock_acquire(&g_lock);
    /* The device paired before the app ran, as the shell says each it finds as it starts. */
    if (!g_said_paired) {
        megler_bluetooth_event e;
        g_said_paired = true;
        memset(&e, 0, sizeof(e));
        e.kind = MEGLER_BT_EVENT_PAIRED;
        e.address = MG_BT_STANDIN_OLD;
        e.address_type = MEGLER_BT_ADDRESS_PUBLIC;
        e.name = "Old Radio";
        megler_bluetooth_push(&e);
    }
    mg_lock_release(&g_lock);
    return g_radio ? MEGLER_BT_RADIO_ON : MEGLER_BT_RADIO_OFF;
}

static MEGLER_CB void scan_fn(void *user, int mode)
{
    (void)user;
    mg_lock_acquire(&g_lock);
    if (mode != MEGLER_BT_SCAN_OFF && !g_scan) {
        g_counts.scans++;
        if (mode == MEGLER_BT_SCAN_ACTIVE) g_counts.scans_active++;
        g_next_ad = 0;
    } else if (mode == MEGLER_BT_SCAN_OFF && g_scan) {
        g_counts.scans_stopped++;
    }
    g_scan = mode;
    g_counts.scanning = mode != MEGLER_BT_SCAN_OFF;
    worker_start_locked();
    mg_lock_release(&g_lock);
}

static MEGLER_CB void connect_fn(void *user, int conn, unsigned long long address, int address_type, bool auto_connect)
{
    int i;
    (void)user;
    mg_lock_acquire(&g_lock);
    g_counts.connects++;
    g_counts.connect_type = address_type;
    i = conn_index_locked(conn);
    if (i < 0) i = conn_index_locked(0);
    if (i >= 0) {
        g_conns[i].conn = conn;
        g_conns[i].address = address;
        g_conns[i].auto_connect = auto_connect;
        g_conns[i].connected = false;
        if (!g_radio) {
            event_locked(20, MEGLER_BT_EVENT_CONNECTION, conn, 133, 0, 0, NULL, 0);
        } else if (address == MG_BT_STANDIN_MESHCORE) {
            g_conns[i].connected = true;
            g_counts.connected++;
            event_locked(30, MEGLER_BT_EVENT_CONNECTION, conn, 0, 0, 1, NULL, 0);
        } else if (!auto_connect) {
            event_locked(100, MEGLER_BT_EVENT_CONNECTION, conn, 133, 0, 0, NULL, 0);
            g_conns[i].conn = 0;
        }
    }
    mg_lock_release(&g_lock);
}

static MEGLER_CB void disconnect_fn(void *user, int conn)
{
    int i;
    (void)user;
    mg_lock_acquire(&g_lock);
    i = conn_index_locked(conn);
    if (i >= 0) {
        g_counts.disconnects++;
        if (g_conns[i].connected) g_counts.connected--;
        g_conns[i].conn = 0;
        g_conns[i].connected = false;
        g_counts.cccd = 0;
        event_locked(10, MEGLER_BT_EVENT_CONNECTION, conn, 0, 0, 0, NULL, 0);
    }
    mg_lock_release(&g_lock);
}

static MEGLER_CB void discover_fn(void *user, int conn)
{
    megler_bluetooth_event e;
    (void)user;
    memset(&e, 0, sizeof(e));
    e.kind = MEGLER_BT_EVENT_SERVICES;
    e.conn = conn;
    mg_lock_acquire(&g_lock);
    if (conn_index_locked(conn) >= 0) later_locked(20, &e, NULL, 0, true);
    mg_lock_release(&g_lock);
}

static MEGLER_CB void read_fn(void *user, int conn, int handle)
{
    u8 cccd[2];
    (void)user;
    mg_lock_acquire(&g_lock);
    if (conn_index_locked(conn) < 0) {
        mg_lock_release(&g_lock);
        return;
    }
    switch (handle) {
    case H_NAME: event_locked(10, MEGLER_BT_EVENT_READ, conn, 0, handle, 0, "MeshCore-Test", 13); break;
    case H_CCCD:
        cccd[0] = (u8)g_counts.cccd;
        cccd[1] = 0;
        event_locked(10, MEGLER_BT_EVENT_READ, conn, 0, handle, 0, cccd, 2);
        break;
    case H_SECRET:
        if (g_bonded) event_locked(10, MEGLER_BT_EVENT_READ, conn, 0, handle, 0, "s3cret", 6);
        else event_locked(10, MEGLER_BT_EVENT_READ, conn, 5, handle, 0, NULL, 0);
        break;
    case H_RX:
    case H_TX: event_locked(10, MEGLER_BT_EVENT_READ, conn, 2, handle, 0, NULL, 0); break;
    default: event_locked(10, MEGLER_BT_EVENT_READ, conn, 1, handle, 0, NULL, 0); break;
    }
    mg_lock_release(&g_lock);
}

static MEGLER_CB void write_fn(void *user, int conn, int handle, const unsigned char *data, int size, int type)
{
    int i;
    (void)user;
    (void)type;
    mg_lock_acquire(&g_lock);
    i = conn_index_locked(conn);
    if (i < 0) {
        mg_lock_release(&g_lock);
        return;
    }
    if (handle == H_RX) {
        g_counts.written += size;
        event_locked(5, MEGLER_BT_EVENT_WRITTEN, conn, 0, handle, 0, NULL, 0);
        if (size == 4 && !memcmp(data, "drop", 4)) {
            if (g_conns[i].connected) g_counts.connected--;
            g_conns[i].connected = false;
            g_counts.cccd = 0;
            event_locked(15, MEGLER_BT_EVENT_CONNECTION, conn, 19, 0, 0, NULL, 0);
        } else if (g_counts.cccd & 1) {
            event_locked(10, MEGLER_BT_EVENT_CHANGED, conn, 0, H_TX, 0, data, size);
        }
    } else if (handle == H_CCCD && size == 2) {
        g_counts.cccd = data[0];
        event_locked(5, MEGLER_BT_EVENT_WRITTEN, conn, 0, handle, 0, NULL, 0);
    } else {
        event_locked(5, MEGLER_BT_EVENT_WRITTEN, conn, handle == H_NAME ? 3 : 1, handle, 0, NULL, 0);
    }
    mg_lock_release(&g_lock);
}

static MEGLER_CB void mtu_fn(void *user, int conn, int mtu)
{
    (void)user;
    mg_lock_acquire(&g_lock);
    g_counts.mtu_asked = mtu;
    if (conn_index_locked(conn) >= 0) event_locked(10, MEGLER_BT_EVENT_MTU, conn, 0, 0, mtu < 185 ? mtu : 185, NULL, 0);
    mg_lock_release(&g_lock);
}

static MEGLER_CB void bond_fn(void *user, unsigned long long address, int address_type, bool create)
{
    (void)user;
    (void)address_type;
    mg_lock_acquire(&g_lock);
    if (address == MG_BT_STANDIN_MESHCORE) {
        if (!create) {
            g_counts.unbonds++;
            g_bonded = false;
            device_event_locked(10, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_NONE, 9, 0);
        } else if (g_bonded) {
            device_event_locked(10, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_BONDED, 0, 0);
        } else {
            g_counts.bonds++;
            g_pairing = true;
            device_event_locked(5, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_BONDING, 0, 0);
            device_event_locked(15, MEGLER_BT_EVENT_PAIRING, address, MEGLER_BT_PAIRING_PIN, 0, 0);
        }
    } else {
        /* (No other device answers: the pairing fails, the device down.) */
        device_event_locked(5, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_BONDING, 0, 0);
        device_event_locked(50, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_NONE, 4, 0);
    }
    mg_lock_release(&g_lock);
}

static MEGLER_CB void pairing_reply_fn(void *user, unsigned long long address, int answer, const char *pin)
{
    (void)user;
    mg_lock_acquire(&g_lock);
    g_counts.pairing_answers++;
    if (pin) mg_strlcpy(g_counts.pin, pin, sizeof(g_counts.pin));
    if (address == MG_BT_STANDIN_MESHCORE && g_pairing) {
        g_pairing = false;
        if (answer == MEGLER_BT_PAIR_ASK_USER) {
            /* The user, asked, types the PIN the radio's screen shows. */
            g_counts.asked_user++;
            mg_strlcpy(g_counts.pin, "123456", sizeof(g_counts.pin));
            g_bonded = true;
            device_event_locked(20, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_BONDED, 0, 0);
        } else if (answer == MEGLER_BT_PAIR_ACCEPT && pin && !strcmp(pin, "123456")) {
            g_bonded = true;
            device_event_locked(10, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_BONDED, 0, 0);
        } else {
            device_event_locked(10, MEGLER_BT_EVENT_BOND, address, MEGLER_BT_BOND_NONE, answer == MEGLER_BT_PAIR_ACCEPT ? 1 : 3, 0);
        }
    }
    mg_lock_release(&g_lock);
}

static MEGLER_CB void settings_fn(void *user)
{
    megler_bluetooth_event e;
    (void)user;
    mg_lock_acquire(&g_lock);
    g_counts.settings++;
    /* The user turns the radio on there. */
    if (!g_radio) {
        g_radio = true;
        memset(&e, 0, sizeof(e));
        e.kind = MEGLER_BT_EVENT_STATE;
        e.value = MEGLER_BT_RADIO_ON;
        later_locked(50, &e, NULL, 0, false);
    }
    mg_lock_release(&g_lock);
}

static const megler_bluetooth_host k_host = { NULL, state_fn, scan_fn, connect_fn, disconnect_fn, discover_fn, read_fn, write_fn, mtu_fn, NULL, bond_fn,
                                              pairing_reply_fn, settings_fn };

const megler_bluetooth_host *mg_bluetooth_standin(void) { return &k_host; }

void mg_bluetooth_standin_reset(void)
{
    mg_lock_acquire(&g_lock);
    memset(g_jobs, 0, sizeof(g_jobs));
    memset(g_conns, 0, sizeof(g_conns));
    memset(&g_counts, 0, sizeof(g_counts));
    g_radio = true;
    g_bonded = g_pairing = g_said_paired = false;
    g_scan = 0;
    mg_lock_release(&g_lock);
}

void mg_bluetooth_standin_radio(bool on)
{
    megler_bluetooth_event e;
    int i;
    mg_lock_acquire(&g_lock);
    g_radio = on;
    memset(&e, 0, sizeof(e));
    e.kind = MEGLER_BT_EVENT_STATE;
    e.value = on ? MEGLER_BT_RADIO_ON : MEGLER_BT_RADIO_OFF;
    later_locked(0, &e, NULL, 0, false);
    /* Off, the connections drop and the scan stops, as Windows' do with the radio. */
    if (!on) {
        for (i = 0; i < MAX_CONNS; i++) {
            if (!g_conns[i].conn || !g_conns[i].connected) continue;
            g_conns[i].connected = false;
            g_counts.connected--;
            event_locked(5, MEGLER_BT_EVENT_CONNECTION, g_conns[i].conn, 8, 0, 0, NULL, 0);
        }
        g_scan = 0;
        g_counts.scanning = false;
    }
    mg_lock_release(&g_lock);
}

void mg_bluetooth_standin_get(mg_bluetooth_standin_counts *out)
{
    mg_lock_acquire(&g_lock);
    *out = g_counts;
    mg_lock_release(&g_lock);
}
