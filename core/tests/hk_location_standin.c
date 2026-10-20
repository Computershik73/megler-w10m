/* The tests' stand-in for the shell's location: hk_location_standin.h. */
#include "hk_location_standin.h"

#include "../src/hk_core.h"
#include "../src/host/hk_host.h"

static volatile i32 g_starts, g_stops;

static int access_fn(void *user, bool ask)
{
    (void)user;
    (void)ask;
    return MEGLET_LOCATION_ON;
}

static bool start_fn(void *user, int interval_ms)
{
    (void)user;
    (void)interval_ms;
    hk_atomic_add(&g_starts, 1);
    return true;
}

static void stop_fn(void *user)
{
    (void)user;
    hk_atomic_add(&g_stops, 1);
}

static bool last_fn(void *user, meglet_location *fix)
{
    long long now_ms = (long long)(hk_time_realtime_ns() / 1000000ull);
    (void)user;
    fix->latitude = 55.751244 + (double)(now_ms / 1000 % 100) * 0.00001;
    fix->longitude = 37.618423;
    fix->altitude = 144.0;
    fix->accuracy_m = 12.0f;
    fix->speed_mps = 1.5f;
    fix->bearing_deg = 0.0f;
    fix->has = MEGLET_LOCATION_HAS_ALTITUDE | MEGLET_LOCATION_HAS_SPEED | MEGLET_LOCATION_HAS_BEARING;
    /* A new fix each second, as a phone's satellites give one. */
    fix->time_ms = now_ms / 1000 * 1000;
    fix->source = MEGLET_LOCATION_SATELLITES;
    return true;
}

static const meglet_location_host k_host = { NULL, access_fn, start_fn, stop_fn, last_fn };

const meglet_location_host *hk_location_standin(void) { return &k_host; }

void hk_location_standin_counts(int *starts, int *stops)
{
    *starts = (int)g_starts;
    *stops = (int)g_stops;
}
