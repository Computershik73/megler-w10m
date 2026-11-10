/*
 * The phone's Wi-Fi between the shell and the framework (core/src/drivers/mg_java_wifi.c), over the stand-in for the shell's
 * Wi-Fi (mg_wifi_standin.c), and with none: what the framework's megler.Wifi reads of a scan and of the connection.
 */
#include "mg_wifi_tests.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../src/drivers/mg_java.h"
#include "../src/host/mg_host.h"
#include "mg_wifi_standin.h"

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

/* No Wi-Fi: a shell that gives none (a host left zero) has the app told there is no adapter and no scan. */
static void test_none(void)
{
    megler_wifi_network nets[4], net;
    long long t = 123;
    megler_wifi_host none;
    memset(&none, 0, sizeof(none));
    mg_java_wifi_set_host(&none);
    CHECK_INT(mg_jwifi_state(false), MEGLER_WIFI_NONE);
    CHECK_INT(mg_jwifi_scan(nets, 4, &t), -1);
    CHECK_INT(t, 0);
    CHECK(!mg_jwifi_connection(&net));
    mg_java_wifi_set_host(NULL);
    CHECK_INT(mg_jwifi_state(true), MEGLER_WIFI_NONE);
}

/* A scan of the stand-in's four networks: each as the shell found it, the report's time 50 ms before the scan ended. */
static void test_scan(void)
{
    megler_wifi_network nets[8];
    long long t = 0;
    u64 before = mg_time_realtime_ns(), after;
    int n;
    mg_wifi_standin_reset();
    mg_java_wifi_set_host(mg_wifi_standin());
    CHECK_INT(mg_jwifi_state(true), MEGLER_WIFI_ON);
    memset(nets, 0xA5, sizeof(nets));
    n = mg_jwifi_scan(nets, 8, &t);
    after = mg_time_realtime_ns();
    CHECK_INT(n, 4);
    CHECK_INT(mg_wifi_standin_scans(), 1);
    CHECK(t >= (long long)before && t <= (long long)after - 50000000ll + 10000000ll);
    if (n != 4) return;
    CHECK(nets[0].bssid == 0x001122334455ull && nets[0].ssid_size == 11 && !memcmp(nets[0].ssid, "Megler Test", 11));
    /* (The first network's signal is the scan's: -45, one less at the first scan.) */
    CHECK_INT(nets[0].rssi_dbm, -46);
    CHECK_INT(nets[0].frequency_mhz, 2437);
    CHECK_INT(nets[0].security, MEGLER_WIFI_SECURITY(MEGLER_WIFI_AUTH_RSNA_PSK, MEGLER_WIFI_CIPHER_CCMP));
    CHECK_INT(nets[0].phy, MEGLER_WIFI_PHY_HT);
    CHECK(!nets[0].adhoc);
    CHECK(nets[1].bssid == 0x0ABBCCDDEEFFull && nets[1].ssid_size == 8 && !memcmp(nets[1].ssid, "\xD0\x9A\xD0\xB0\xD1\x84\xD0\xB5", 8));
    CHECK(nets[1].rssi_dbm == -67 && nets[1].frequency_mhz == 5180 && nets[1].phy == MEGLER_WIFI_PHY_VHT);
    /* A hidden network: no octets of its name. */
    CHECK(nets[2].bssid == 0x0212345678ABull && nets[2].ssid_size == 0 && nets[2].rssi_dbm == -80 && nets[2].frequency_mhz == 2412);
    CHECK_INT(nets[2].security, MEGLER_WIFI_SECURITY(MEGLER_WIFI_AUTH_WPA3_SAE, MEGLER_WIFI_CIPHER_CCMP));
    CHECK(nets[3].adhoc == 1 && nets[3].ssid_size == 13 && nets[3].phy == MEGLER_WIFI_PHY_ERP);
    /* A second scan is the shell's again: another report, the first network a little weaker. */
    n = mg_jwifi_scan(nets, 8, &t);
    CHECK_INT(n, 4);
    CHECK_INT(nets[0].rssi_dbm, -47);
    CHECK_INT(mg_wifi_standin_scans(), 2);
    /* No more than the room given. */
    memset(nets, 0, sizeof(nets));
    n = mg_jwifi_scan(nets, 2, &t);
    CHECK_INT(n, 2);
    CHECK(nets[1].bssid == 0x0ABBCCDDEEFFull && nets[2].bssid == 0);
}

/* The network the phone is connected to; and none while the Wi-Fi is off, when a scan cannot be made either. */
static void test_connection(void)
{
    megler_wifi_network net, nets[4];
    long long t = 1;
    mg_wifi_standin_reset();
    mg_java_wifi_set_host(mg_wifi_standin());
    CHECK(mg_jwifi_connection(&net));
    CHECK(net.bssid == 0x001122334455ull && net.ssid_size == 11 && net.rssi_dbm == -45 && net.frequency_mhz == 2437);
    mg_wifi_standin_set_state(MEGLER_WIFI_OFF);
    CHECK_INT(mg_jwifi_state(false), MEGLER_WIFI_OFF);
    CHECK_INT(mg_jwifi_scan(nets, 4, &t), -1);
    CHECK(!mg_jwifi_connection(&net));
    CHECK_INT(mg_wifi_standin_scans(), 0);
}

int mg_wifi_tests_run(mg_test_sink sink, void *user, int *checks)
{
    t_checks = t_failed = 0;
    t_sink = sink;
    t_sink_user = user;
    test_none();
    test_scan();
    test_connection();
    /* As it was: no Wi-Fi, the stand-in as it starts, for the Java apps' runs. */
    mg_java_wifi_set_host(NULL);
    mg_wifi_standin_reset();
    say("the Wi-Fi between the shell and the framework: %d checks, %d failed", t_checks, t_failed);
    *checks += t_checks;
    return t_failed;
}
