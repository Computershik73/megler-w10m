/*
 * The phone's Bluetooth LE radio between the shell and the framework (core/src/run/mg_bluetooth.c), over the stand-in for
 * the shell's radio (mg_bluetooth_standin.c): what the framework's thread for Bluetooth reads, event by event, as the app's
 * calls are answered.
 */
#include "mg_bluetooth_tests.h"

#include <stdio.h>
#include <string.h>

#include "../src/host/mg_host.h"
#include "../src/run/mg_bluetooth.h"
#include "mg_bluetooth_standin.h"

static int t_checks, t_failed;
static mg_test_sink t_sink;
static void *t_sink_user;

static void say(const char *fmt, ...) MG_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (t_sink) t_sink(line, t_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define CHECK(cond)                                                \
    do {                                                           \
        t_checks++;                                                \
        if (!(cond)) {                                             \
            t_failed++;                                            \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);    \
        }                                                          \
    } while (0)

#define CHECK_INT(expr, want)                                                                      \
    do {                                                                                           \
        long long got_ = (long long)(expr), want_ = (long long)(want);                             \
        t_checks++;                                                                                \
        if (got_ != want_) {                                                                       \
            t_failed++;                                                                            \
            say("  FAIL %s:%d: %s = %lld, expected %lld", __FILE__, __LINE__, #expr, got_, want_); \
        }                                                                                          \
    } while (0)

/* The next event of that kind within `wait_ms`, the others (advertisements, mostly) passed over; NULL for none. */
static megler_bluetooth_event *next_kind(int kind, int wait_ms)
{
    u64 until = mg_time_monotonic_ns() + (u64)wait_ms * 1000000ull;
    for (;;) {
        u64 now = mg_time_monotonic_ns();
        megler_bluetooth_event *e;
        if (now >= until) return NULL;
        e = mg_bluetooth_next((int)((until - now) / 1000000ull) + 1);
        if (!e) return NULL;
        if (e->kind == kind) return e;
        mg_bluetooth_free(e);
    }
}

static void test_state(void)
{
    char name[64], text[18];
    u64 bonded[4];
    CHECK_INT(mg_bluetooth_state(name, sizeof(name)), MEGLER_BT_RADIO_ON);
    CHECK(!strcmp(name, "Test Radio"));
    /* The device paired before the app ran: bonded, named, and no event of it. */
    CHECK_INT(mg_bluetooth_bond_state(MG_BT_STANDIN_OLD), MEGLER_BT_BOND_BONDED);
    CHECK(mg_bluetooth_name(MG_BT_STANDIN_OLD, name, sizeof(name)) && !strcmp(name, "Old Radio"));
    CHECK_INT(mg_bluetooth_bonded(bonded, 4), 1);
    CHECK(bonded[0] == MG_BT_STANDIN_OLD);
    CHECK_INT(mg_bluetooth_bond_state(MG_BT_STANDIN_MESHCORE), MEGLER_BT_BOND_NONE);
    CHECK_INT(mg_bluetooth_address_type(MG_BT_STANDIN_MESHCORE), MEGLER_BT_ADDRESS_UNKNOWN);
    mg_bluetooth_address_text(MG_BT_STANDIN_MESHCORE, text);
    CHECK(!strcmp(text, "C0:FF:EE:00:00:01"));
    CHECK(mg_bluetooth_next(0) == NULL);
}

static void test_scan(void)
{
    mg_bluetooth_standin_counts n;
    megler_bluetooth_event untold;
    bool meshcore = false, beacon = false;
    char name[64];
    u64 until = mg_time_monotonic_ns() + 3000000000ull;
    CHECK(mg_bluetooth_scan(MEGLER_BT_SCAN_ACTIVE));
    while ((!meshcore || !beacon) && mg_time_monotonic_ns() < until) {
        megler_bluetooth_event *e = next_kind(MEGLER_BT_EVENT_ADVERTISEMENT, 1000);
        if (!e) break;
        if (e->address == MG_BT_STANDIN_MESHCORE && !meshcore) {
            meshcore = true;
            CHECK_INT(e->value, -50);
            CHECK_INT(e->extra, 1);
            CHECK_INT(e->address_type, MEGLER_BT_ADDRESS_RANDOM);
            CHECK_INT(e->size, 46);
            CHECK(e->data && e->data[0] == 2 && e->data[1] == 1 && e->data[2] == 6);
        }
        if (e->address == MG_BT_STANDIN_BEACON && !beacon) {
            beacon = true;
            CHECK_INT(e->extra, 0);
        }
        mg_bluetooth_free(e);
    }
    CHECK(meshcore && beacon);
    /* What each advertisement says of its device is kept: its kind of address, its name. */
    CHECK_INT(mg_bluetooth_address_type(MG_BT_STANDIN_MESHCORE), MEGLER_BT_ADDRESS_RANDOM);
    CHECK(mg_bluetooth_name(MG_BT_STANDIN_MESHCORE, name, sizeof(name)) && !strcmp(name, "MeshCore-Test"));
    CHECK(mg_bluetooth_name(MG_BT_STANDIN_BEACON, name, sizeof(name)) && !strcmp(name, "Beacon"));
    /* An advertisement that tells no kind of address (Windows before 10 2004, the phone's) leaves the kind known. */
    memset(&untold, 0, sizeof(untold));
    untold.kind = MEGLER_BT_EVENT_ADVERTISEMENT;
    untold.address = MG_BT_STANDIN_MESHCORE;
    untold.address_type = MEGLER_BT_ADDRESS_UNKNOWN;
    untold.value = -50;
    untold.extra = 1;
    megler_bluetooth_push(&untold);
    CHECK_INT(mg_bluetooth_address_type(MG_BT_STANDIN_MESHCORE), MEGLER_BT_ADDRESS_RANDOM);
    CHECK(mg_bluetooth_scan(MEGLER_BT_SCAN_OFF));
    mg_bluetooth_standin_get(&n);
    CHECK(n.scans == 1 && n.scans_active == 1 && n.scans_stopped == 1 && !n.scanning);
}

static void test_connection(void)
{
    static const u8 cccd_on[2] = { 1, 0 };
    mg_bluetooth_standin_counts n;
    megler_bluetooth_event *e;
    u64 connected[4];
    int c = mg_bluetooth_connect(MG_BT_STANDIN_MESHCORE, MEGLER_BT_ADDRESS_UNKNOWN, false, 0);
    CHECK(c > 0);
    /* The address's kind as it was heard. */
    mg_bluetooth_standin_get(&n);
    CHECK_INT(n.connect_type, MEGLER_BT_ADDRESS_RANDOM);
    e = next_kind(MEGLER_BT_EVENT_CONNECTION, 2000);
    CHECK(e && e->conn == c && e->value == 1 && e->status == 0);
    mg_bluetooth_free(e);
    CHECK(mg_bluetooth_connected(connected, 4) == 1 && connected[0] == MG_BT_STANDIN_MESHCORE);
    CHECK(mg_bluetooth_discover(c));
    e = next_kind(MEGLER_BT_EVENT_SERVICES, 2000);
    CHECK(e && e->conn == c && e->status == 0 && e->count == 8 && e->attributes && e->attributes[2].handle == 0x10 && e->attributes[2].uuid[0] == 0x6e
          && e->attributes[3].properties == 0x0C);
    mg_bluetooth_free(e);
    /* Notifications on, a write, and what was written back as one. */
    CHECK(mg_bluetooth_write(c, 0x15, cccd_on, 2, MEGLER_BT_WRITE_REQUEST));
    e = next_kind(MEGLER_BT_EVENT_WRITTEN, 2000);
    CHECK(e && e->handle == 0x15 && e->status == 0);
    mg_bluetooth_free(e);
    CHECK(mg_bluetooth_write(c, 0x12, (const u8 *)"hi", 2, MEGLER_BT_WRITE_COMMAND));
    e = next_kind(MEGLER_BT_EVENT_WRITTEN, 2000);
    CHECK(e && e->handle == 0x12 && e->status == 0);
    mg_bluetooth_free(e);
    e = next_kind(MEGLER_BT_EVENT_CHANGED, 2000);
    CHECK(e && e->handle == 0x14 && e->size == 2 && e->data && !memcmp(e->data, "hi", 2));
    mg_bluetooth_free(e);
    /* A read the device refuses unpaired, with its status and no bytes. */
    CHECK(mg_bluetooth_read(c, 0x22));
    e = next_kind(MEGLER_BT_EVENT_READ, 2000);
    CHECK(e && e->handle == 0x22 && e->status == 5 && e->size == 0);
    mg_bluetooth_free(e);
    /* The stand-in tells no connection's RSSI: the last advertisement's. */
    CHECK(mg_bluetooth_rssi(c));
    e = next_kind(MEGLER_BT_EVENT_RSSI, 2000);
    CHECK(e && e->value == -50 && e->status == 0);
    mg_bluetooth_free(e);
    CHECK(mg_bluetooth_mtu(c, 247));
    e = next_kind(MEGLER_BT_EVENT_MTU, 2000);
    CHECK(e && e->value == 185 && e->status == 0);
    mg_bluetooth_free(e);
    /* Closed: what the radio says of it after is not handed over, and it is no device's connection. */
    mg_bluetooth_disconnect(c, true);
    CHECK(next_kind(MEGLER_BT_EVENT_CONNECTION, 300) == NULL);
    CHECK_INT(mg_bluetooth_connected(connected, 4), 0);
    CHECK(!mg_bluetooth_read(c, 0x03));
    mg_bluetooth_standin_get(&n);
    CHECK(n.connected == 0 && n.disconnects == 1 && n.written == 2 && n.mtu_asked == 247);
}

static void test_flood(void)
{
    megler_bluetooth_event ad, mtu, *e;
    u8 bytes[3] = { 2, 1, 6 };
    int ads = 0, i, c = mg_bluetooth_connect(MG_BT_STANDIN_MESHCORE, MEGLER_BT_ADDRESS_RANDOM, false, 0);
    bool answered = false;
    e = next_kind(MEGLER_BT_EVENT_CONNECTION, 2000);
    CHECK(e && e->value == 1);
    mg_bluetooth_free(e);
    /* Three hundred advertisements nobody takes, then an answer: the first 256 wait, the answer too. */
    memset(&ad, 0, sizeof(ad));
    ad.kind = MEGLER_BT_EVENT_ADVERTISEMENT;
    ad.address = 0x020000000001ull;
    ad.value = -80;
    ad.data = bytes;
    ad.size = 3;
    for (i = 0; i < 300; i++) megler_bluetooth_push(&ad);
    memset(&mtu, 0, sizeof(mtu));
    mtu.kind = MEGLER_BT_EVENT_MTU;
    mtu.conn = c;
    mtu.value = 99;
    megler_bluetooth_push(&mtu);
    /* (What was pushed was copied: the bytes changed after do not reach the event.) */
    bytes[2] = 0x55;
    while ((e = mg_bluetooth_next(0)) != NULL) {
        if (e->kind == MEGLER_BT_EVENT_ADVERTISEMENT) {
            ads++;
            CHECK(e->size == 3 && e->data[2] == 6);
        }
        if (e->kind == MEGLER_BT_EVENT_MTU && e->value == 99) answered = true;
        mg_bluetooth_free(e);
    }
    CHECK_INT(ads, 256);
    CHECK(answered);
    mg_bluetooth_disconnect(c, true);
}

static void test_end(void)
{
    mg_bluetooth_standin_counts n;
    megler_bluetooth_event ad;
    CHECK(mg_bluetooth_scan(MEGLER_BT_SCAN_ACTIVE));
    mg_bluetooth_end();
    /* The scan stopped at the shell, and nothing taken after. */
    mg_bluetooth_standin_get(&n);
    CHECK(!n.scanning);
    memset(&ad, 0, sizeof(ad));
    ad.kind = MEGLER_BT_EVENT_ADVERTISEMENT;
    ad.address = MG_BT_STANDIN_BEACON;
    megler_bluetooth_push(&ad);
    CHECK(mg_bluetooth_next(50) == NULL);
}

int mg_bluetooth_tests_run(mg_test_sink sink, void *user, int *checks)
{
    t_checks = t_failed = 0;
    t_sink = sink;
    t_sink_user = user;
    mg_bluetooth_standin_reset();
    mg_bluetooth_set_host(mg_bluetooth_standin());
    test_state();
    test_scan();
    test_connection();
    test_flood();
    test_end();
    /* As it was: no radio, the stand-in as it starts, for the Java apps' runs. */
    mg_bluetooth_set_host(NULL);
    mg_bluetooth_standin_reset();
    say("the Bluetooth LE radio between the shell and the framework: %d checks, %d failed", t_checks, t_failed);
    *checks += t_checks;
    return t_failed;
}
