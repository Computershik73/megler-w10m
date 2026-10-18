/*
 * megler.LocationNative: the phone's location behind a Java app's android.location.LocationManager, over the shell's
 * (megler_location_host, core/include/megler.h). The Java side (framework-overlay/megler/LocationService.java) keeps the app's
 * requests and delivers to its listeners; this only asks the host: whether the app may have the location, to start and
 * stop finding it, and the newest fix.
 *
 * What the host answers is said in the log as it changes: whether the app may have the location, and each start and stop.
 */
#include "hk_java.h"

#include <string.h>

#include "../dvm/hk_dvm.h"
#include "../host/hk_host.h"

#define N(name) static void name(hk_jcall *c)

static megler_location_host g_host;
static hk_lock g_lock = HK_LOCK_INIT;
static int g_said_access = -1;

void hk_java_location_set_host(const megler_location_host *host)
{
    if (host) g_host = *host;
    else memset(&g_host, 0, sizeof(g_host));
    g_said_access = -1;
}

static const char *access_name(int a)
{
    switch (a) {
    case MEGLER_LOCATION_ON: return "on: the app may have it";
    case MEGLER_LOCATION_OFF: return "off: the phone's location is switched off in its settings";
    case MEGLER_LOCATION_DENIED: return "denied: the user has not let Megler have the location";
    case MEGLER_LOCATION_UNKNOWN: return "not asked yet";
    default: return "none: nothing finds where the phone is";
    }
}

int hk_jlocation_access(bool ask)
{
    int a = g_host.access ? g_host.access(g_host.user, ask) : MEGLER_LOCATION_NONE;
    bool say;
    hk_lock_acquire(&g_lock);
    say = a != g_said_access;
    g_said_access = a;
    hk_lock_release(&g_lock);
    if (say) hk_info("location: %s%s", access_name(a), ask ? " (asked)" : "");
    return a;
}

bool hk_jlocation_start(int interval_ms)
{
    bool ok = g_host.start && g_host.start(g_host.user, interval_ms);
    hk_info("location: the phone's location %s, every %d ms", ok ? "is followed" : "could not be followed", interval_ms);
    return ok;
}

void hk_jlocation_stop(void)
{
    if (g_host.stop) g_host.stop(g_host.user);
    hk_info("location: no longer followed");
}

bool hk_jlocation_last(megler_location *fix)
{
    memset(fix, 0, sizeof(*fix));
    return g_host.last && g_host.last(g_host.user, fix);
}

/* access(boolean ask) */
N(Location_access) { c->ret.i = hk_jlocation_access(c->args[0].i != 0); }
/* start(int intervalMs) */
N(Location_start) { c->ret.i = hk_jlocation_start(c->args[0].i) ? 1 : 0; }
N(Location_stop) { hk_jlocation_stop(); }

/* last(): the newest fix as {latitude, longitude, altitude, accuracy, altitude accuracy, speed, bearing, time ms, has, source}; null for none */
N(Location_last)
{
    megler_location fix;
    hk_jobj *a;
    c->ret.l = NULL;
    if (!hk_jlocation_last(&fix)) return;
    a = hk_dvm_new_array("[D", 10);
    if (a) {
        double *v = (double *)a->u.arr.data;
        v[0] = fix.latitude;
        v[1] = fix.longitude;
        v[2] = fix.altitude;
        v[3] = fix.accuracy_m;
        v[4] = fix.altitude_accuracy_m;
        v[5] = fix.speed_mps;
        v[6] = fix.bearing_deg;
        v[7] = (double)fix.time_ms;
        v[8] = fix.has;
        v[9] = fix.source;
    }
    c->ret.l = a;
}

static const hk_jhle k_location[] = {
    { "megler/LocationNative", "access", NULL, Location_access },
    { "megler/LocationNative", "start", NULL, Location_start },
    { "megler/LocationNative", "stop", NULL, Location_stop },
    { "megler/LocationNative", "last", NULL, Location_last },
    { NULL, NULL, NULL, NULL },
};

const hk_jhle *hk_java_location_natives(void) { return k_location; }
