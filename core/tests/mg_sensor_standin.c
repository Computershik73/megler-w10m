/* The tests' stand-in for the shell's sensors: mg_sensor_standin.h. */
#include "mg_sensor_standin.h"

#include <string.h>

#include "../src/mg_core.h"
#include "../src/host/mg_host.h"

#define KINDS MEGLER_SENSOR_BAROMETER

/* What each reads, as Windows reads it; the orientation's w below zero, which Android's rotation vector has above. */
static const struct {
    double v[4];
    int accuracy;
} k_readings[KINDS + 1] = {
    { { 0, 0, 0, 0 }, 0 },
    { { 0.25, -0.5, -0.75, 0 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },          /* accelerometer, g */
    { { 0, -0.6, -0.8, 0 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },              /* gravity */
    { { 0.1, 0.2, -0.3, 0 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },             /* linear acceleration */
    { { 90, -45, 180, 0 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },               /* gyrometer, degrees a second */
    { { 20, -30, -40, 0 }, MEGLER_SENSOR_ACCURACY_APPROXIMATE },           /* magnetometer, microteslas */
    { { 0.1, 0.2, 0.3, -0.92736185 }, MEGLER_SENSOR_ACCURACY_HIGH },       /* orientation: x, y, z, w */
    { { 0, 0, 0.6, 0.8 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },                /* relative orientation */
    { { 30, -20, 300, 0 }, MEGLER_SENSOR_ACCURACY_HIGH },                  /* inclinometer: pitch, roll, yaw */
    { { 321.5, 0, 0, 0 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },                /* light, lux */
    { { 1, 30, 0, 0 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },                   /* proximity: near, 30 mm */
    { { 1013.25, 0, 0, 0 }, MEGLER_SENSOR_ACCURACY_UNKNOWN },              /* barometer, hPa */
};

static const megler_sensor_info k_info[] = {
    { MEGLER_SENSOR_ACCELEROMETER, "Test Accelerometer", "Megler Tests", 10, 4, 0 },
    { MEGLER_SENSOR_GRAVITY, "Test Sensor Fusion", "", 10, 0, 0 },
    { MEGLER_SENSOR_LINEAR_ACCELERATION, "Test Sensor Fusion", "", 10, 0, 0 },
    { MEGLER_SENSOR_GYROMETER, "Test Gyrometer", "Megler Tests", 10, 2000, 0 },
    { MEGLER_SENSOR_MAGNETOMETER, "Test Magnetometer", "Megler Tests", 20, 0, 0 },
    { MEGLER_SENSOR_ORIENTATION, "Test Sensor Fusion", "", 16, 0, 0 },
    { MEGLER_SENSOR_RELATIVE_ORIENTATION, "Test Sensor Fusion", "", 16, 0, 0 },
    { MEGLER_SENSOR_INCLINOMETER, "Test Sensor Fusion", "", 16, 0, 0 },
    { MEGLER_SENSOR_LIGHT, "Test Light Sensor", "Megler Tests", 0, 0, 0 },
    { MEGLER_SENSOR_PROXIMITY, "Test Proximity Sensor", "Megler Tests", 0, 50, 0 },
    { MEGLER_SENSOR_BAROMETER, "", "", 100, 0, 0 },
};

static mg_lock g_lock = MG_LOCK_INIT;
static struct {
    bool running, fresh;
    int starts, stops, interval_ms;
} g_kind[KINDS + 1];
static bool g_feed = true, g_thread;

int mg_sensor_standin_reading(int kind, double values[4])
{
    if (kind <= 0 || kind > KINDS) return 0;
    memcpy(values, k_readings[kind].v, sizeof(k_readings[kind].v));
    return k_readings[kind].accuracy;
}

static void push(int kind)
{
    megler_sensor_sample s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.accuracy = mg_sensor_standin_reading(kind, s.values);
    s.time_ns = (long long)mg_time_realtime_ns() - 3000000;
    megler_sensor_push(&s);
}

static MG_HOSTCB void feed(void *arg)
{
    (void)arg;
    for (;;) {
        bool run[KINDS + 1];
        int k;
        mg_lock_acquire(&g_lock);
        for (k = 1; k <= KINDS; k++) {
            bool on_change = k == MEGLER_SENSOR_LIGHT || k == MEGLER_SENSOR_PROXIMITY;
            run[k] = g_feed && g_kind[k].running && (!on_change || g_kind[k].fresh);
            if (run[k]) g_kind[k].fresh = false;
        }
        mg_lock_release(&g_lock);
        for (k = 1; k <= KINDS; k++)
            if (run[k]) push(k);
        mg_sleep_ns(5000000);
    }
}

static MEGLER_CB int list_fn(void *user, megler_sensor_info *info, int max)
{
    int n = (int)(sizeof(k_info) / sizeof(k_info[0]));
    (void)user;
    if (n > max) n = max;
    memcpy(info, k_info, sizeof(k_info[0]) * (size_t)n);
    return n;
}

static MEGLER_CB void start_fn(void *user, int kind, int interval_ms)
{
    (void)user;
    if (kind <= 0 || kind > KINDS) return;
    mg_lock_acquire(&g_lock);
    g_kind[kind].starts++;
    g_kind[kind].running = true;
    g_kind[kind].fresh = true;
    g_kind[kind].interval_ms = interval_ms;
    if (!g_thread) {
        g_thread = true;
        mg_thread_start(feed, NULL, 128 * 1024, "sensor stand-in");
    }
    mg_lock_release(&g_lock);
}

static MEGLER_CB void stop_fn(void *user, int kind)
{
    (void)user;
    if (kind <= 0 || kind > KINDS) return;
    mg_lock_acquire(&g_lock);
    g_kind[kind].stops++;
    g_kind[kind].running = false;
    mg_lock_release(&g_lock);
}

static const megler_sensor_host k_host = { NULL, list_fn, start_fn, stop_fn };

const megler_sensor_host *mg_sensor_standin(void) { return &k_host; }

void mg_sensor_standin_feed(bool on)
{
    mg_lock_acquire(&g_lock);
    g_feed = on;
    mg_lock_release(&g_lock);
}

void mg_sensor_standin_counts(int kind, int *starts, int *stops, int *interval_ms)
{
    mg_lock_acquire(&g_lock);
    if (starts) *starts = kind > 0 && kind <= KINDS ? g_kind[kind].starts : 0;
    if (stops) *stops = kind > 0 && kind <= KINDS ? g_kind[kind].stops : 0;
    if (interval_ms) *interval_ms = kind > 0 && kind <= KINDS ? g_kind[kind].interval_ms : 0;
    mg_lock_release(&g_lock);
}

bool mg_sensor_standin_running(int kind)
{
    bool on;
    mg_lock_acquire(&g_lock);
    on = kind > 0 && kind <= KINDS && g_kind[kind].running;
    mg_lock_release(&g_lock);
    return on;
}
