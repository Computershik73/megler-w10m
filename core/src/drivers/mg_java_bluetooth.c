/*
 * megler.Bluetooth: the phone's Bluetooth LE radio behind a Java app's android.bluetooth (framework-overlay/megler/
 * Bluetooth.java), over run/mg_bluetooth.c. The Java side keeps Android's objects -- the adapter, the devices, the scans'
 * callbacks, each BluetoothGatt -- and hands each event to them on their threads; this asks the radio and hands its events
 * over one at a time.
 */
#include "mg_java.h"

#include <string.h>

#include "../dvm/mg_dvm.h"
#include "../host/mg_host.h"
#include "../run/mg_bluetooth.h"

#define N(name) static void name(mg_jcall *c)

static mg_jobj *bytes_of(const u8 *data, int size)
{
    mg_jobj *a = mg_dvm_new_array("[B", size);
    if (a && size > 0) memcpy(a->u.arr.data, data, (size_t)size);
    return a;
}

static mg_jobj *addresses_of(const u64 *v, int n)
{
    mg_jobj *a = mg_dvm_new_array("[J", n);
    if (a && n > 0) memcpy(a->u.arr.data, v, (size_t)n * sizeof(u64));
    return a;
}

/* state(): megler.h's MEGLER_BT_RADIO_* */
N(Bt_state) { c->ret.i = mg_bluetooth_state(NULL, 0); }
/* name(): the radio's name, the phone's */
N(Bt_name)
{
    char name[128];
    mg_bluetooth_state(name, sizeof(name));
    c->ret.l = mg_dvm_new_string(name);
}
/* scan(int mode) */
N(Bt_scan) { c->ret.z = mg_bluetooth_scan(c->args[0].i); }
/* connect(long address, int type, boolean auto, int conn): its number, 0 for none */
N(Bt_connect) { c->ret.i = mg_bluetooth_connect((u64)c->args[0].j, c->args[1].i, c->args[2].z != 0, c->args[3].i); }
/* disconnect(int conn, boolean close) */
N(Bt_disconnect) { mg_bluetooth_disconnect(c->args[0].i, c->args[1].z != 0); }
N(Bt_discover) { c->ret.z = mg_bluetooth_discover(c->args[0].i); }
/* read(int conn, int handle) */
N(Bt_read) { c->ret.z = mg_bluetooth_read(c->args[0].i, c->args[1].i); }
/* write(int conn, int handle, byte[] value, int type) */
N(Bt_write)
{
    mg_jobj *v = c->args[2].l;
    c->ret.z = mg_bluetooth_write(c->args[0].i, c->args[1].i, v ? (const u8 *)v->u.arr.data : NULL, v ? (int)v->u.arr.len : 0, c->args[3].i);
}
/* mtu(int conn, int mtu), rssi(int conn) */
N(Bt_mtu) { c->ret.z = mg_bluetooth_mtu(c->args[0].i, c->args[1].i); }
N(Bt_rssi) { c->ret.z = mg_bluetooth_rssi(c->args[0].i); }
/* bond(long address, int type, boolean create) */
N(Bt_bond) { c->ret.z = mg_bluetooth_bond((u64)c->args[0].j, c->args[1].i, c->args[2].z != 0); }
/* pairingReply(long address, int answer, String pin) */
N(Bt_pairingReply)
{
    const char *pin = c->args[2].l ? mg_dvm_string_utf8(c->args[2].l) : NULL;
    mg_bluetooth_pairing_reply((u64)c->args[0].j, c->args[1].i, pin);
}
N(Bt_settings) { c->ret.z = mg_bluetooth_settings(); }
/* bondState(long address), addressType(long address), deviceName(long address) */
N(Bt_bondState) { c->ret.i = mg_bluetooth_bond_state((u64)c->args[0].j); }
N(Bt_addressType) { c->ret.i = mg_bluetooth_address_type((u64)c->args[0].j); }
N(Bt_deviceName)
{
    char name[64];
    c->ret.l = mg_bluetooth_name((u64)c->args[0].j, name, sizeof(name)) ? mg_dvm_new_string(name) : NULL;
}
/* bonded(), connected(): the devices' addresses */
N(Bt_bonded)
{
    u64 v[64];
    c->ret.l = addresses_of(v, mg_bluetooth_bonded(v, 64));
}
N(Bt_connected)
{
    u64 v[64];
    c->ret.l = addresses_of(v, mg_bluetooth_connected(v, 64));
}

/* What next() fills in of its meta, by index (megler.Bluetooth's constants). */
enum { M_KIND, M_CONN, M_ADDRESS, M_ADDRESS_TYPE, M_STATUS, M_HANDLE, M_VALUE, M_EXTRA, M_TIME, M_COUNT };

/*
 * next(long[] meta, int timeoutMs): the next event, its numbers into meta and as {byte[] data, String name, int[] attributes
 * (kind, handle, properties for each), byte[] their UUIDs (16 bytes each, as written)}; null when none came within the time.
 */
N(Bt_next)
{
    mg_jobj *meta = c->args[0].l, *out;
    megler_bluetooth_event *e;
    i64 *m;
    c->ret.l = NULL;
    if (!meta || meta->u.arr.len < M_COUNT) return;
    e = mg_bluetooth_next(c->args[1].i);
    if (!e) return;
    m = (i64 *)meta->u.arr.data;
    m[M_KIND] = e->kind;
    m[M_CONN] = e->conn;
    m[M_ADDRESS] = (i64)e->address;
    m[M_ADDRESS_TYPE] = e->address_type;
    m[M_STATUS] = e->status;
    m[M_HANDLE] = e->handle;
    m[M_VALUE] = e->value;
    m[M_EXTRA] = e->extra;
    /* (An advertisement's time is when it came here, on the clock of elapsedRealtimeNanos, as Android's ScanResult has it.) */
    m[M_TIME] = (i64)mg_time_monotonic_ns();
    out = mg_dvm_new_array("[Ljava/lang/Object;", 4);
    if (out) {
        if (e->data) out->u.oarr.v[0] = bytes_of(e->data, e->size);
        if (e->name) out->u.oarr.v[1] = mg_dvm_new_string(e->name);
        if (e->attributes && e->count > 0) {
            mg_jobj *attrs = mg_dvm_new_array("[I", e->count * 3), *uuids = mg_dvm_new_array("[B", e->count * 16);
            int i;
            for (i = 0; attrs && uuids && i < e->count; i++) {
                ((i32 *)attrs->u.arr.data)[i * 3] = e->attributes[i].kind;
                ((i32 *)attrs->u.arr.data)[i * 3 + 1] = e->attributes[i].handle;
                ((i32 *)attrs->u.arr.data)[i * 3 + 2] = e->attributes[i].properties;
                memcpy((u8 *)uuids->u.arr.data + i * 16, e->attributes[i].uuid, 16);
            }
            out->u.oarr.v[2] = attrs;
            out->u.oarr.v[3] = uuids;
        }
    }
    mg_bluetooth_free(e);
    c->ret.l = out;
}

static const mg_jhle k_bluetooth[] = {
    { "megler/Bluetooth", "state", NULL, Bt_state },
    { "megler/Bluetooth", "name", NULL, Bt_name },
    { "megler/Bluetooth", "scan", NULL, Bt_scan },
    { "megler/Bluetooth", "connect", NULL, Bt_connect },
    { "megler/Bluetooth", "disconnect", NULL, Bt_disconnect },
    { "megler/Bluetooth", "discover", NULL, Bt_discover },
    { "megler/Bluetooth", "read", NULL, Bt_read },
    { "megler/Bluetooth", "write", NULL, Bt_write },
    { "megler/Bluetooth", "mtu", NULL, Bt_mtu },
    { "megler/Bluetooth", "rssi", NULL, Bt_rssi },
    { "megler/Bluetooth", "bond", NULL, Bt_bond },
    { "megler/Bluetooth", "pairingReply", NULL, Bt_pairingReply },
    { "megler/Bluetooth", "settings", NULL, Bt_settings },
    { "megler/Bluetooth", "bondState", NULL, Bt_bondState },
    { "megler/Bluetooth", "addressType", NULL, Bt_addressType },
    { "megler/Bluetooth", "deviceName", NULL, Bt_deviceName },
    { "megler/Bluetooth", "bonded", NULL, Bt_bonded },
    { "megler/Bluetooth", "connected", NULL, Bt_connected },
    { "megler/Bluetooth", "next", NULL, Bt_next },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_bluetooth_natives(void) { return k_bluetooth; }
