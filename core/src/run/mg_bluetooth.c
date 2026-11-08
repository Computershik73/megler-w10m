/*
 * The phone's Bluetooth LE radio between the shell and the framework (mg_bluetooth.h, megler.h's megler_bluetooth_host).
 *
 * The shell's calls are made outside the lock: a shell may push an event from inside one (the core's own answer for the
 * RSSI does). What it pushes is copied whole into one block and queued for the framework's thread for Bluetooth, which
 * reads one at a time (mg_bluetooth_next) and hands each to Android's objects on their threads. Advertisements come by the
 * dozen a second: no more than MAX_ADS_WAITING of them wait for an app that does not take them, the newest dropped; the
 * other events always wait, being answers the app asked for.
 *
 * Besides the queue the core keeps what Android answers without asking the radio: the radio's state and name, the devices
 * heard or paired (an address's kind, its name, its bond, the last advertisement's RSSI), and the connections the app has.
 */
#include "mg_bluetooth.h"

#include <stdio.h>
#include <string.h>

#include "../host/mg_host.h"

#define MAX_DEVICES 512
#define MAX_CONNS 64
#define MAX_ADS_WAITING 256
/* The devices heard that the log names, the first of them. */
#define DEVICES_SAID 32

typedef struct queued {
    megler_bluetooth_event e; /* first: an event handed out is its block */
    struct queued *next;
} queued;

typedef struct device {
    u64 address;
    int type, bond, rssi;
    u64 rssi_ns;
    bool heard;
    char name[64];
} device;

typedef struct connection {
    int conn;
    u64 address;
    int type;
    bool open, connected, auto_connect;
} connection;

static megler_bluetooth_host g_host;
static bool g_has_host, g_ended = true;
static mg_lock g_lock = MG_LOCK_INIT;
static mg_cond g_cond;
static bool g_cond_ready;
static queued *g_head, *g_tail;
static int g_ads_waiting;
static u64 g_ads_heard, g_ads_dropped, g_changed, g_written;
static device g_devices[MAX_DEVICES];
static int g_ndevices, g_devices_said;
static connection g_conns[MAX_CONNS];
static int g_nconns, g_next_conn = 1;
static int g_state = -1, g_scan;
static char g_name[128];

void mg_bluetooth_address_text(u64 address, char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(address >> 40) & 0xff, (unsigned)(address >> 32) & 0xff, (unsigned)(address >> 24) & 0xff,
             (unsigned)(address >> 16) & 0xff, (unsigned)(address >> 8) & 0xff, (unsigned)address & 0xff);
}

static const char *radio_name(int s)
{
    return s == MEGLER_BT_RADIO_ON ? "on" : s == MEGLER_BT_RADIO_OFF ? "off" : "not there (no radio that can be a Bluetooth LE central)";
}

static const char *bond_name(int b)
{
    return b == MEGLER_BT_BOND_BONDED ? "bonded" : b == MEGLER_BT_BOND_BONDING ? "bonding" : "not bonded";
}

static const char *pairing_name(int v)
{
    switch (v) {
    case MEGLER_BT_PAIRING_PIN: return "a PIN";
    case MEGLER_BT_PAIRING_PASSKEY: return "a passkey";
    case MEGLER_BT_PAIRING_CONFIRMATION: return "the passkey it shows confirmed";
    case MEGLER_BT_PAIRING_CONSENT: return "a consent";
    case MEGLER_BT_PAIRING_DISPLAY_PASSKEY: return "its passkey entered on it";
    case MEGLER_BT_PAIRING_DISPLAY_PIN: return "its PIN entered on it";
    default: return "something";
    }
}

/* ------------------------------------------------------------- the tables */

static device *device_locked(u64 address, bool make)
{
    int i;
    for (i = 0; i < g_ndevices; i++)
        if (g_devices[i].address == address) return &g_devices[i];
    if (!make) return NULL;
    if (g_ndevices == MAX_DEVICES) {
        /* (The oldest that is not paired makes room: a crowded street has more beacons than this.) */
        for (i = 0; i < g_ndevices && g_devices[i].bond == MEGLER_BT_BOND_BONDED; i++) {
        }
        if (i == g_ndevices) return NULL;
        memmove(&g_devices[i], &g_devices[i + 1], (size_t)(g_ndevices - i - 1) * sizeof(device));
        g_ndevices--;
    }
    memset(&g_devices[g_ndevices], 0, sizeof(device));
    g_devices[g_ndevices].address = address;
    g_devices[g_ndevices].type = MEGLER_BT_ADDRESS_UNKNOWN;
    g_devices[g_ndevices].bond = MEGLER_BT_BOND_NONE;
    return &g_devices[g_ndevices++];
}

static connection *conn_locked(int conn)
{
    int i;
    for (i = 0; i < g_nconns; i++)
        if (g_conns[i].conn == conn) return &g_conns[i];
    return NULL;
}

/* The device's name an advertisement gives: its complete local name, else its shortened one; false for neither. */
static bool name_of(const u8 *ad, int size, char *name, size_t cap)
{
    int at = 0, found = 0;
    while (at + 1 < size) {
        int len = ad[at], type;
        if (len == 0 || at + 1 + len > size) break;
        type = ad[at + 1];
        if ((type == 0x09 || (type == 0x08 && found != 0x09)) && cap) {
            size_t n = (size_t)len - 1 < cap - 1 ? (size_t)len - 1 : cap - 1;
            memcpy(name, ad + at + 2, n);
            name[n] = 0;
            found = type;
        }
        at += 1 + len;
    }
    return found != 0;
}

/* ------------------------------------------------------------- the queue */

static void queue_locked(queued *q)
{
    q->next = NULL;
    if (g_tail) g_tail->next = q;
    else g_head = q;
    g_tail = q;
    if (q->e.kind == MEGLER_BT_EVENT_ADVERTISEMENT) g_ads_waiting++;
    mg_cond_broadcast(&g_cond);
}

/* A copy of the event in one block: its bytes, its name and its attributes after it. */
static queued *copy_of(const megler_bluetooth_event *in)
{
    size_t data = in->data && in->size > 0 ? (size_t)in->size : 0, name = in->name ? strlen(in->name) + 1 : 0;
    size_t attrs = in->attributes && in->count > 0 ? (size_t)in->count * sizeof(megler_bluetooth_attribute) : 0;
    queued *q = (queued *)mg_alloc(sizeof(queued) + attrs + data + name);
    u8 *at = (u8 *)(q + 1);
    q->e = *in;
    q->e.attributes = NULL;
    q->e.count = 0;
    q->e.data = NULL;
    q->e.size = 0;
    q->e.name = NULL;
    if (attrs) {
        memcpy(at, in->attributes, attrs);
        q->e.attributes = (const megler_bluetooth_attribute *)at;
        q->e.count = in->count;
        at += attrs;
    }
    if (data) {
        memcpy(at, in->data, data);
        q->e.data = at;
        q->e.size = (int)data;
        at += data;
    }
    if (name) {
        memcpy(at, in->name, name);
        q->e.name = (const char *)at;
    }
    return q;
}

void megler_bluetooth_push(const megler_bluetooth_event *in)
{
    char said[300], addr[18];
    connection *c;
    device *d;
    bool take = true;
    said[0] = 0;
    if (!in) return;
    mg_bluetooth_address_text(in->address, addr);
    mg_lock_acquire(&g_lock);
    if (!g_has_host || g_ended) {
        mg_lock_release(&g_lock);
        return;
    }
    switch (in->kind) {
    case MEGLER_BT_EVENT_STATE:
        if (in->name && in->name[0]) mg_strlcpy(g_name, in->name, sizeof(g_name));
        if (in->value != g_state) snprintf(said, sizeof(said), "bluetooth: the radio is %s", radio_name(in->value));
        g_state = in->value;
        /* With the radio off nothing scans: the app's scan is the shell's to start again when it asks. */
        if (in->value != MEGLER_BT_RADIO_ON) g_scan = MEGLER_BT_SCAN_OFF;
        break;
    case MEGLER_BT_EVENT_ADVERTISEMENT: {
        char name[64];
        bool named = in->data && name_of(in->data, in->size, name, sizeof(name));
        g_ads_heard++;
        d = device_locked(in->address, true);
        if (d) {
            if (!d->heard && g_devices_said < DEVICES_SAID) {
                g_devices_said++;
                snprintf(said, sizeof(said), "bluetooth: heard %s (%s)%s%s%s, %d dBm%s", addr, in->address_type == MEGLER_BT_ADDRESS_RANDOM ? "random" : "public",
                         named ? " \"" : "", named ? name : "", named ? "\"" : "", in->value, in->extra ? ", takes connections" : "");
            }
            d->heard = true;
            d->type = in->address_type;
            d->rssi = in->value;
            d->rssi_ns = mg_time_monotonic_ns();
            if (named) mg_strlcpy(d->name, name, sizeof(d->name));
        }
        if (g_ads_waiting >= MAX_ADS_WAITING) {
            g_ads_dropped++;
            take = false;
        }
        break;
    }
    case MEGLER_BT_EVENT_SCAN_FAILED:
        snprintf(said, sizeof(said), "bluetooth: the scan failed (ScanCallback's code %d)", in->status);
        g_scan = MEGLER_BT_SCAN_OFF;
        break;
    case MEGLER_BT_EVENT_PAIRED:
        d = device_locked(in->address, true);
        if (d) {
            d->bond = MEGLER_BT_BOND_BONDED;
            if (d->type == MEGLER_BT_ADDRESS_UNKNOWN) d->type = in->address_type;
            if (in->name && in->name[0]) mg_strlcpy(d->name, in->name, sizeof(d->name));
        }
        snprintf(said, sizeof(said), "bluetooth: paired before: %s%s%s%s", addr, in->name ? " \"" : "", in->name ? in->name : "", in->name ? "\"" : "");
        /* (The table's alone: Android tells an app of the devices bonded before it ran by none of its broadcasts.) */
        take = false;
        break;
    case MEGLER_BT_EVENT_BOND:
        d = device_locked(in->address, true);
        if (d) {
            d->bond = in->value;
            if (d->type == MEGLER_BT_ADDRESS_UNKNOWN) d->type = in->address_type;
            if (in->name && in->name[0]) mg_strlcpy(d->name, in->name, sizeof(d->name));
        }
        if (in->value == MEGLER_BT_BOND_NONE && in->status)
            snprintf(said, sizeof(said), "bluetooth: %s is %s (reason %d)", addr, bond_name(in->value), in->status);
        else
            snprintf(said, sizeof(said), "bluetooth: %s is %s", addr, bond_name(in->value));
        break;
    case MEGLER_BT_EVENT_PAIRING:
        snprintf(said, sizeof(said), "bluetooth: %s asks for %s to pair", addr, pairing_name(in->value));
        break;
    default:
        /* The connection's events: none for one the app has closed. */
        c = conn_locked(in->conn);
        if (!c || !c->open) {
            take = false;
            break;
        }
        mg_bluetooth_address_text(c->address, addr);
        if (in->kind == MEGLER_BT_EVENT_CONNECTION) {
            if (in->value && !c->connected) snprintf(said, sizeof(said), "bluetooth: connection %d to %s made", c->conn, addr);
            else if (!in->value && c->connected)
                snprintf(said, sizeof(said), "bluetooth: connection %d to %s %s (status %d)", c->conn, addr, in->status ? "dropped" : "ended", in->status);
            else if (!in->value)
                snprintf(said, sizeof(said), "bluetooth: connection %d to %s not made (status %d)", c->conn, addr, in->status);
            c->connected = in->value != 0;
        } else if (in->kind == MEGLER_BT_EVENT_SERVICES) {
            int i, services = 0, chars = 0, descs = 0;
            for (i = 0; in->attributes && i < in->count; i++) {
                int k = in->attributes[i].kind;
                services += k == MEGLER_BT_ATTR_SERVICE || k == MEGLER_BT_ATTR_SECONDARY;
                chars += k == MEGLER_BT_ATTR_CHARACTERISTIC;
                descs += k == MEGLER_BT_ATTR_DESCRIPTOR;
            }
            snprintf(said, sizeof(said), "bluetooth: connection %d: %d services, %d characteristics, %d descriptors (status %d)", c->conn, services, chars, descs,
                     in->status);
        } else if (in->kind == MEGLER_BT_EVENT_MTU) {
            snprintf(said, sizeof(said), "bluetooth: connection %d: MTU %d (status %d)", c->conn, in->value, in->status);
        } else if ((in->kind == MEGLER_BT_EVENT_READ || in->kind == MEGLER_BT_EVENT_WRITTEN) && in->status) {
            snprintf(said, sizeof(said), "bluetooth: connection %d: %s of handle 0x%04x failed (status %d)", c->conn, in->kind == MEGLER_BT_EVENT_READ ? "a read" : "a write",
                     in->handle, in->status);
        }
        if (in->kind == MEGLER_BT_EVENT_CHANGED) g_changed++;
        if (in->kind == MEGLER_BT_EVENT_WRITTEN && !in->status) g_written++;
        break;
    }
    if (take) queue_locked(copy_of(in));
    mg_lock_release(&g_lock);
    if (said[0]) mg_info("%s", said);
}

megler_bluetooth_event *mg_bluetooth_next(int wait_ms)
{
    queued *q = NULL;
    u64 until = wait_ms > 0 ? mg_time_monotonic_ns() + (u64)wait_ms * 1000000ull : 0;
    mg_lock_acquire(&g_lock);
    if (!g_cond_ready) {
        mg_cond_init(&g_cond);
        g_cond_ready = true;
    }
    for (;;) {
        while (g_head && !q) {
            connection *c;
            q = g_head;
            g_head = q->next;
            if (!g_head) g_tail = NULL;
            if (q->e.kind == MEGLER_BT_EVENT_ADVERTISEMENT) g_ads_waiting--;
            /* (A connection closed after its event was queued: dropped here.) */
            if (q->e.kind >= MEGLER_BT_EVENT_CONNECTION && q->e.kind <= MEGLER_BT_EVENT_RSSI && (!(c = conn_locked(q->e.conn)) || !c->open)) {
                free(q);
                q = NULL;
            }
        }
        if (q || wait_ms == 0) break;
        if (wait_ms < 0) {
            mg_cond_wait(&g_cond, &g_lock);
        } else {
            u64 now = mg_time_monotonic_ns();
            if (now >= until) break;
            mg_cond_wait_ns(&g_cond, &g_lock, until - now);
        }
    }
    mg_lock_release(&g_lock);
    return q ? &q->e : NULL;
}

void mg_bluetooth_free(megler_bluetooth_event *e) { free(e); }

/* ------------------------------------------------------------- the run */

void mg_bluetooth_set_host(const megler_bluetooth_host *host)
{
    queued *q;
    mg_lock_acquire(&g_lock);
    if (!g_cond_ready) {
        mg_cond_init(&g_cond);
        g_cond_ready = true;
    }
    if (host) g_host = *host;
    else memset(&g_host, 0, sizeof(g_host));
    g_has_host = host && host->state;
    g_ended = false;
    while ((q = g_head) != NULL) {
        g_head = q->next;
        free(q);
    }
    g_tail = NULL;
    g_ads_waiting = 0;
    g_ads_heard = g_ads_dropped = g_changed = g_written = 0;
    g_ndevices = g_devices_said = g_nconns = 0;
    g_next_conn = 1;
    g_state = -1;
    g_scan = MEGLER_BT_SCAN_OFF;
    g_name[0] = 0;
    mg_lock_release(&g_lock);
}

void mg_bluetooth_end(void)
{
    megler_bluetooth_host host;
    int conns[MAX_CONNS], n = 0, i;
    bool scanning, had;
    u64 heard, dropped, changed, written;
    queued *q;
    mg_lock_acquire(&g_lock);
    had = g_has_host && !g_ended;
    host = g_host;
    scanning = g_scan != MEGLER_BT_SCAN_OFF;
    for (i = 0; i < g_nconns; i++)
        if (g_conns[i].open) conns[n++] = g_conns[i].conn;
    heard = g_ads_heard;
    dropped = g_ads_dropped;
    changed = g_changed;
    written = g_written;
    g_ended = true;
    g_scan = MEGLER_BT_SCAN_OFF;
    while ((q = g_head) != NULL) {
        g_head = q->next;
        free(q);
    }
    g_tail = NULL;
    g_ads_waiting = 0;
    g_nconns = 0;
    if (g_cond_ready) mg_cond_broadcast(&g_cond);
    mg_lock_release(&g_lock);
    if (!had) return;
    if (scanning && host.scan) host.scan(host.user, MEGLER_BT_SCAN_OFF);
    for (i = 0; i < n; i++)
        if (host.disconnect) host.disconnect(host.user, conns[i]);
    if (heard || n)
        mg_info("bluetooth: the app has ended: %llu advertisements heard (%llu of them dropped, the app not taking them), %llu notifications, %llu writes; "
                "%d connection(s) closed", (unsigned long long)heard, (unsigned long long)dropped, (unsigned long long)changed, (unsigned long long)written, n);
}

/* ------------------------------------------------------------- the app's asking */

int mg_bluetooth_state(char *name, size_t size)
{
    megler_bluetooth_host host;
    int state;
    char got[128];
    mg_lock_acquire(&g_lock);
    host = g_host;
    state = g_has_host ? g_state : MEGLER_BT_RADIO_NONE;
    mg_lock_release(&g_lock);
    if (state < 0) {
        /* The shell's first answer, outside the lock: it may wait until the shell has looked. */
        got[0] = 0;
        state = host.state(host.user, got, (int)sizeof(got));
        got[sizeof(got) - 1] = 0;
        if (state < MEGLER_BT_RADIO_NONE || state > MEGLER_BT_RADIO_ON) state = MEGLER_BT_RADIO_NONE;
        mg_lock_acquire(&g_lock);
        /* (A STATE event that came meanwhile is newer.) */
        if (g_state < 0) {
            g_state = state;
            if (got[0]) mg_strlcpy(g_name, got, sizeof(g_name));
            mg_lock_release(&g_lock);
            mg_info("bluetooth: the radio is %s%s%s%s", radio_name(state), got[0] ? " (\"" : "", got, got[0] ? "\")" : "");
        } else {
            state = g_state;
            mg_lock_release(&g_lock);
        }
    }
    if (name && size) {
        mg_lock_acquire(&g_lock);
        mg_strlcpy(name, g_name, size);
        mg_lock_release(&g_lock);
    }
    return state;
}

bool mg_bluetooth_scan(int mode)
{
    megler_bluetooth_host host;
    bool change;
    mg_lock_acquire(&g_lock);
    host = g_host;
    change = g_has_host && !g_ended && host.scan && mode != g_scan;
    if (change) g_scan = mode;
    mg_lock_release(&g_lock);
    if (!host.scan || !g_has_host) return false;
    if (change) {
        mg_info("bluetooth: %s", mode == MEGLER_BT_SCAN_ACTIVE ? "scanning (active: with scan responses)" : mode == MEGLER_BT_SCAN_PASSIVE ? "scanning (passive)" : "no longer scanning");
        host.scan(host.user, mode);
    }
    return true;
}

int mg_bluetooth_connect(u64 address, int address_type, bool auto_connect, int conn)
{
    megler_bluetooth_host host;
    connection *c = NULL;
    device *d;
    char addr[18];
    mg_lock_acquire(&g_lock);
    host = g_host;
    if (!g_has_host || g_ended || !host.connect) {
        mg_lock_release(&g_lock);
        return 0;
    }
    if (conn) c = conn_locked(conn);
    if (!c) {
        if (g_nconns == MAX_CONNS) {
            /* (A slot of a connection closed long ago.) */
            int i;
            for (i = 0; i < g_nconns && g_conns[i].open; i++) {
            }
            if (i == g_nconns) {
                mg_lock_release(&g_lock);
                mg_warn("bluetooth: %d connections open: no more", MAX_CONNS);
                return 0;
            }
            g_conns[i] = g_conns[--g_nconns];
        }
        c = &g_conns[g_nconns++];
        memset(c, 0, sizeof(*c));
        c->conn = g_next_conn++;
    }
    /* A device heard says what its address is; one the app named by its address alone is taken as it was heard. */
    d = device_locked(address, false);
    if (address_type == MEGLER_BT_ADDRESS_UNKNOWN && d) address_type = d->type;
    c->address = address;
    c->type = address_type;
    c->auto_connect = auto_connect;
    c->open = true;
    conn = c->conn;
    mg_lock_release(&g_lock);
    mg_bluetooth_address_text(address, addr);
    mg_info("bluetooth: connection %d to %s (%s)%s", conn, addr,
            address_type == MEGLER_BT_ADDRESS_RANDOM ? "random" : address_type == MEGLER_BT_ADDRESS_PUBLIC ? "public" : "of a kind not known",
            auto_connect ? ", made whenever it is in range" : "");
    host.connect(host.user, conn, address, address_type, auto_connect);
    return conn;
}

void mg_bluetooth_disconnect(int conn, bool close)
{
    megler_bluetooth_host host;
    connection *c;
    bool tell;
    mg_lock_acquire(&g_lock);
    host = g_host;
    c = conn_locked(conn);
    tell = c && c->open && g_has_host && !g_ended && host.disconnect;
    if (c && close) {
        c->open = false;
        c->connected = false;
    }
    mg_lock_release(&g_lock);
    if (tell) host.disconnect(host.user, conn);
}

/* The host, when the connection is the app's and open. */
static bool conn_host(int conn, megler_bluetooth_host *host)
{
    connection *c;
    bool ok;
    mg_lock_acquire(&g_lock);
    *host = g_host;
    c = conn_locked(conn);
    ok = c && c->open && g_has_host && !g_ended;
    mg_lock_release(&g_lock);
    return ok;
}

bool mg_bluetooth_discover(int conn)
{
    megler_bluetooth_host host;
    if (!conn_host(conn, &host) || !host.discover) return false;
    host.discover(host.user, conn);
    return true;
}

bool mg_bluetooth_read(int conn, int handle)
{
    megler_bluetooth_host host;
    if (!conn_host(conn, &host) || !host.read) return false;
    host.read(host.user, conn, handle);
    return true;
}

bool mg_bluetooth_write(int conn, int handle, const u8 *data, int size, int type)
{
    megler_bluetooth_host host;
    if (!conn_host(conn, &host) || !host.write) return false;
    host.write(host.user, conn, handle, data, size, type);
    return true;
}

bool mg_bluetooth_mtu(int conn, int mtu)
{
    megler_bluetooth_host host;
    if (!conn_host(conn, &host) || !host.mtu) return false;
    host.mtu(host.user, conn, mtu);
    return true;
}

bool mg_bluetooth_rssi(int conn)
{
    megler_bluetooth_host host;
    megler_bluetooth_event e;
    connection *c;
    device *d;
    if (!conn_host(conn, &host)) return false;
    if (host.rssi) {
        host.rssi(host.user, conn);
        return true;
    }
    /* Windows tells no connection's signal: the last advertisement heard from the device, or GATT_FAILURE when none was. */
    memset(&e, 0, sizeof(e));
    e.kind = MEGLER_BT_EVENT_RSSI;
    e.conn = conn;
    e.status = 257;
    e.value = -127;
    mg_lock_acquire(&g_lock);
    c = conn_locked(conn);
    d = c ? device_locked(c->address, false) : NULL;
    if (d && d->rssi_ns) {
        e.status = 0;
        e.value = d->rssi;
    }
    mg_lock_release(&g_lock);
    megler_bluetooth_push(&e);
    return true;
}

bool mg_bluetooth_bond(u64 address, int address_type, bool create)
{
    megler_bluetooth_host host;
    device *d;
    char addr[18];
    mg_lock_acquire(&g_lock);
    host = g_host;
    d = device_locked(address, false);
    if (address_type == MEGLER_BT_ADDRESS_UNKNOWN && d) address_type = d->type;
    mg_lock_release(&g_lock);
    if (!g_has_host || g_ended || !host.bond) return false;
    mg_bluetooth_address_text(address, addr);
    mg_info("bluetooth: %s %s", create ? "pairing with" : "the pairing removed with", addr);
    host.bond(host.user, address, address_type, create);
    return true;
}

void mg_bluetooth_pairing_reply(u64 address, int answer, const char *pin)
{
    megler_bluetooth_host host;
    char addr[18];
    mg_lock_acquire(&g_lock);
    host = g_host;
    mg_lock_release(&g_lock);
    if (!g_has_host || g_ended || !host.pairing_reply) return;
    mg_bluetooth_address_text(address, addr);
    mg_info("bluetooth: %s's pairing %s", addr,
            answer == MEGLER_BT_PAIR_ACCEPT ? (pin ? "answered with the app's PIN" : "accepted by the app")
            : answer == MEGLER_BT_PAIR_ASK_USER ? "not answered by the app: the user is asked"
                                                 : "refused by the app");
    host.pairing_reply(host.user, address, answer, pin);
}

bool mg_bluetooth_settings(void)
{
    megler_bluetooth_host host;
    mg_lock_acquire(&g_lock);
    host = g_host;
    mg_lock_release(&g_lock);
    if (!g_has_host || g_ended || !host.settings) return false;
    mg_info("bluetooth: the phone's Bluetooth settings shown, for the user to turn the radio on");
    host.settings(host.user);
    return true;
}

/* ------------------------------------------------------------- what is known */

int mg_bluetooth_bond_state(u64 address)
{
    device *d;
    int b;
    mg_lock_acquire(&g_lock);
    d = device_locked(address, false);
    b = d ? d->bond : MEGLER_BT_BOND_NONE;
    mg_lock_release(&g_lock);
    return b;
}

int mg_bluetooth_address_type(u64 address)
{
    device *d;
    int t;
    mg_lock_acquire(&g_lock);
    d = device_locked(address, false);
    t = d ? d->type : MEGLER_BT_ADDRESS_UNKNOWN;
    mg_lock_release(&g_lock);
    return t;
}

bool mg_bluetooth_name(u64 address, char *name, size_t size)
{
    device *d;
    bool has;
    mg_lock_acquire(&g_lock);
    d = device_locked(address, false);
    has = d && d->name[0];
    if (has && size) mg_strlcpy(name, d->name, size);
    mg_lock_release(&g_lock);
    return has;
}

int mg_bluetooth_bonded(u64 *out, int max)
{
    int i, n = 0;
    mg_lock_acquire(&g_lock);
    for (i = 0; i < g_ndevices && n < max; i++)
        if (g_devices[i].bond == MEGLER_BT_BOND_BONDED) out[n++] = g_devices[i].address;
    mg_lock_release(&g_lock);
    return n;
}

int mg_bluetooth_connected(u64 *out, int max)
{
    int i, j, n = 0;
    mg_lock_acquire(&g_lock);
    for (i = 0; i < g_nconns && n < max; i++) {
        if (!g_conns[i].open || !g_conns[i].connected) continue;
        /* (A device once, however many of the app's BluetoothGatt objects are connected to it.) */
        for (j = 0; j < n && out[j] != g_conns[i].address; j++) {
        }
        if (j == n) out[n++] = g_conns[i].address;
    }
    mg_lock_release(&g_lock);
    return n;
}
