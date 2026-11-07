/*
 * The phone's sensors as Android has them (mg_sensors.h), and libandroid's ASensorManager over them.
 *
 * What Windows reads and what Android reads, sensor by sensor (the shell hands over Windows' readings as they are, megler.h):
 *
 *   accelerometer, gravity, linear acceleration  Windows' are in g and point along gravity: a device lying face up reads -1
 *       on z, one standing upright -1 on y, and turned a quarter clockwise +1 on x (the hardware lab kit's accelerometer
 *       tests; "the accelerometer would read -1 G on the Z axis" face up, Windows' Sensors overview). Android's are in
 *       m/s^2 and point against it: +9.81 on z face up, +9.81 on y upright (SensorEvent). The axes are the same --
 *       x right, y up, z out of the screen, the device's natural orientation's -- so every value is -9.80665 times
 *       Windows'. Chromium's Windows sensors do the same ("Change sign of values, to report 'reaction force'").
 *   gyroscope       Windows' are degrees a second, positive counterclockwise looking from the axis's end (the lab kit's
 *       gyro tests: flat, screen up, turned clockwise reads z < -40); Android's rad/s the same way: times pi/180.
 *   magnetic field  microteslas both, the same axes: as they are.
 *   rotation vector, game rotation vector  Windows' OrientationSensor's quaternion (absolute; and for relative readings,
 *       without the magnetometer) is the device's rotation from the earth's frame, which is Android's rotation vector:
 *       x, y, z, w as they are (Chromium hands Windows' quaternion to the web's AbsoluteOrientationSensor as it is, and
 *       Android's TYPE_ROTATION_VECTOR as it is), w kept positive -- the same rotation -- as Android computes w for an
 *       app that reads three values; heading accuracy -1, unknown.
 *   orientation (Android's deprecated TYPE_ORIENTATION)  made of Windows' Inclinometer, whose yaw, pitch and roll are the
 *       web's DeviceOrientation alpha (about z, counterclockwise), beta (about x) and gamma (about y), applied z, then x,
 *       then y (Chromium hands them to DeviceOrientation as they are): the rotation they make is taken apart again as
 *       Android's old orientation sensor's, azimuth clockwise from north 0..360, pitch about x -180..180 positive when z
 *       turns towards y, roll about y -90..90 positive when x turns towards z (AOSP's OrientationSensor).
 *   light           lux both.
 *   proximity       Windows' says whether something is near and maybe how far in millimetres; Android's is centimetres,
 *       and a sensor that only knows near and far says 0 and its maximum range.
 *   pressure        hectopascals both.
 *
 * An event's time is when Windows read it: the reading's Timestamp, Windows' clock, moved onto the monotonic clock
 * Android's elapsedRealtimeNanos is by how long ago it was (up to a second; a reading from further back, or from a clock
 * ahead, is now), and never before the sensor's previous event.
 */
#include "mg_sensors.h"

#include <math.h>
#include <stdio.h>

#include "../bionic/mg_bionic.h"
#include "../bionic/mg_fd.h"
#include "../host/mg_host.h"
#include "mg_ndk.h"

#define KINDS MEGLER_SENSOR_BAROMETER
#define QUEUE_EVENTS 256
#define STANDARD_GRAVITY 9.80665
#define PI_ 3.14159265358979323846
#define GE_INVAL_ 22

/* What each of the shell's sensors becomes, in Android's order (by type), with what Windows does not say: a typical part's. */
static const struct kind_def {
    int kind, type;
    const char *string_type, *name, *fallback; /* name: Android's for a sensor Windows makes of others; fallback: for a part Windows did not name */
    int mode, values;
    float range, resolution, power_ma;
} k_kinds[] = {
    { MEGLER_SENSOR_ACCELEROMETER, MG_SENSOR_ACCELEROMETER, "android.sensor.accelerometer", NULL, "Accelerometer", MG_SENSOR_CONTINUOUS, 3, 78.4532f, 0.0023942f, 0.18f },
    { MEGLER_SENSOR_MAGNETOMETER, MG_SENSOR_MAGNETIC_FIELD, "android.sensor.magnetic_field", NULL, "Magnetometer", MG_SENSOR_CONTINUOUS, 3, 4912.0f, 0.15f, 0.6f },
    { MEGLER_SENSOR_INCLINOMETER, MG_SENSOR_ORIENTATION, "android.sensor.orientation", "Orientation Sensor", NULL, MG_SENSOR_CONTINUOUS, 3, 360.0f, 0.00390625f, 6.88f },
    { MEGLER_SENSOR_GYROMETER, MG_SENSOR_GYROSCOPE, "android.sensor.gyroscope", NULL, "Gyroscope", MG_SENSOR_CONTINUOUS, 3, 34.906586f, 0.0010652f, 6.1f },
    { MEGLER_SENSOR_LIGHT, MG_SENSOR_LIGHT, "android.sensor.light", NULL, "Light Sensor", MG_SENSOR_ON_CHANGE, 1, 10000.0f, 1.0f, 0.1f },
    { MEGLER_SENSOR_BAROMETER, MG_SENSOR_PRESSURE, "android.sensor.pressure", NULL, "Barometer", MG_SENSOR_CONTINUOUS, 1, 1100.0f, 0.01f, 0.004f },
    { MEGLER_SENSOR_PROXIMITY, MG_SENSOR_PROXIMITY, "android.sensor.proximity", NULL, "Proximity Sensor", MG_SENSOR_ON_CHANGE, 1, 5.0f, 5.0f, 0.1f },
    { MEGLER_SENSOR_GRAVITY, MG_SENSOR_GRAVITY, "android.sensor.gravity", "Gravity Sensor", NULL, MG_SENSOR_CONTINUOUS, 3, 19.6133f, 0.0023942f, 6.28f },
    { MEGLER_SENSOR_LINEAR_ACCELERATION, MG_SENSOR_LINEAR_ACCELERATION, "android.sensor.linear_acceleration", "Linear Acceleration Sensor", NULL,
      MG_SENSOR_CONTINUOUS, 3, 78.4532f, 0.0023942f, 6.28f },
    { MEGLER_SENSOR_ORIENTATION, MG_SENSOR_ROTATION_VECTOR, "android.sensor.rotation_vector", "Rotation Vector Sensor", NULL, MG_SENSOR_CONTINUOUS, 5, 1.0f,
      5.96e-8f, 6.88f },
    { MEGLER_SENSOR_RELATIVE_ORIENTATION, MG_SENSOR_GAME_ROTATION_VECTOR, "android.sensor.game_rotation_vector", "Game Rotation Vector Sensor", NULL,
      MG_SENSOR_CONTINUOUS, 4, 1.0f, 5.96e-8f, 6.28f },
};

/* What the log calls each of the shell's sensors. */
static const char *const k_said[KINDS + 1] = { "", "accelerometer", "gravity", "linear acceleration", "gyrometer", "magnetometer", "orientation sensor",
                                               "relative orientation sensor", "inclinometer", "light sensor", "proximity sensor", "barometer" };

struct mg_sensor_queue {
    char who[24];
    int fd;
    mg_cond cond;
    struct {
        bool on;
        i64 period_ns, last_ts;
    } sub[MG_SENSOR_TYPES];
    mg_sensor_event ring[QUEUE_EVENTS];
    int head, count;
    u32 dropped;
    mg_sensor_queue *next;
};

static megler_sensor_host g_host;
/* The list, the queues, each sensor's state. Never held across a call of the host's: the host may hand a reading over at once. */
static mg_lock g_lock = MG_LOCK_INIT;
/* The host's starts and stops, one at a time and in order. */
static mg_lock g_host_lock = MG_LOCK_INIT;
/*
 * The list's making, once. Apart from the starts and stops: the shell may take its time to say what the device has (it
 * waits for Windows), and a pause told meanwhile on the shell's UI thread must not wait for that.
 */
static mg_lock g_list_lock = MG_LOCK_INIT;
static mg_sensor *g_list;
static int g_count;
static volatile i32 g_listed;
static int g_index_of_kind[KINDS + 1] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static const mg_sensor *g_refs[MG_SENSOR_TYPES + 1];
static mg_sensor_queue *g_queues;
static bool g_paused, g_ended;
static struct {
    bool running, read_once;
    int interval_ms;
    i64 last_ts, last_sample_ns;
} g_kind[KINDS + 1];
static volatile i32 g_said;

/* The log, at most 80 lines a run: the sensors found, what is asked for, each sensor's first reading. */
static void say(const char *fmt, ...) MG_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    if (mg_atomic_add(&g_said, 1) > 80) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    mg_info("sensors: %s", line);
}

/* ------------------------------------------------------------ conversion */

/* A Windows Inclinometer reading as Android's old orientation sensor reads it: azimuth, pitch, roll in degrees. */
static void inclination_as_orientation(double pitch, double roll, double yaw, float *out)
{
    /* The rotation from the device's axes to the earth's (east, north, up), Rz(yaw) Rx(pitch) Ry(roll): its elements as
       Android's getRotationMatrixFromVector lays them out, of which these five are needed. */
    double a = yaw * PI_ / 180, b = pitch * PI_ / 180, g = roll * PI_ / 180;
    double ca = cos(a), sa = sin(a), cb = cos(b), sb = sin(b), cg = cos(g), sg = sin(g);
    double r0 = ca * cg - sa * sb * sg, r3 = cg * sa + ca * sb * sg, r6 = -cb * sg, r7 = sb, r8 = cb * cg;
    double azimuth = atan2(-r3, r0) * 180 / PI_;
    if (r6 > 1) r6 = 1;
    if (r6 < -1) r6 = -1;
    if (azimuth < 0) azimuth += 360;
    if (azimuth >= 360) azimuth -= 360;
    out[0] = (float)azimuth;
    out[1] = (float)(atan2(-r7, r8) * 180 / PI_);
    out[2] = (float)(asin(r6) * 180 / PI_);
}

int mg_sensors_convert(int kind, const double *w, float max_range, float *v)
{
    int i;
    switch (kind) {
    case MEGLER_SENSOR_ACCELEROMETER:
    case MEGLER_SENSOR_GRAVITY:
    case MEGLER_SENSOR_LINEAR_ACCELERATION:
        for (i = 0; i < 3; i++) v[i] = (float)(-w[i] * STANDARD_GRAVITY);
        return 3;
    case MEGLER_SENSOR_GYROMETER:
        for (i = 0; i < 3; i++) v[i] = (float)(w[i] * PI_ / 180);
        return 3;
    case MEGLER_SENSOR_MAGNETOMETER:
        for (i = 0; i < 3; i++) v[i] = (float)w[i];
        return 3;
    case MEGLER_SENSOR_ORIENTATION:
    case MEGLER_SENSOR_RELATIVE_ORIENTATION: {
        /* (q and -q turn alike; an app that reads three values computes w as the positive root.) */
        double sign = w[3] < 0 ? -1 : 1;
        for (i = 0; i < 4; i++) v[i] = (float)(w[i] * sign);
        if (kind == MEGLER_SENSOR_RELATIVE_ORIENTATION) return 4;
        v[4] = -1;
        return 5;
    }
    case MEGLER_SENSOR_INCLINOMETER:
        inclination_as_orientation(w[0], w[1], w[2], v);
        return 3;
    case MEGLER_SENSOR_PROXIMITY:
        /* Near at a distance it says, near at no distance it says, or far. */
        v[0] = w[0] != 0 ? (w[1] >= 0 ? (float)(w[1] / 10) : 0.0f) : max_range;
        if (v[0] > max_range) v[0] = max_range;
        return 1;
    case MEGLER_SENSOR_LIGHT:
    case MEGLER_SENSOR_BAROMETER:
        v[0] = (float)w[0];
        return 1;
    default:
        return 0;
    }
}

/* Windows' accuracy as Android's: where Windows says nothing, a sensor that needs no calibration is as good as it gets. */
static int accuracy_of(int kind, int windows)
{
    switch (windows) {
    case MEGLER_SENSOR_ACCURACY_UNRELIABLE: return MG_SENSOR_STATUS_UNRELIABLE;
    case MEGLER_SENSOR_ACCURACY_APPROXIMATE: return MG_SENSOR_STATUS_MEDIUM;
    case MEGLER_SENSOR_ACCURACY_HIGH: return MG_SENSOR_STATUS_HIGH;
    default:
        return kind == MEGLER_SENSOR_MAGNETOMETER || kind == MEGLER_SENSOR_ORIENTATION || kind == MEGLER_SENSOR_INCLINOMETER ? MG_SENSOR_STATUS_MEDIUM
                                                                                                                        : MG_SENSOR_STATUS_HIGH;
    }
}

/* A reading's time on Android's clock (see the top); with the lock held. */
static i64 stamp_locked(int kind, i64 sample_unix_ns)
{
    i64 now = (i64)mg_time_monotonic_ns(), age = 0, ts;
    if (sample_unix_ns > 0) {
        age = (i64)mg_time_realtime_ns() - sample_unix_ns;
        if (age < 0 || age > 1000000000) age = 0;
    }
    ts = now - age;
    if (ts <= g_kind[kind].last_ts) ts = g_kind[kind].last_ts + 1;
    g_kind[kind].last_ts = ts;
    return ts;
}

/* ------------------------------------------------------------- the list */

/* The shell's sensor's own range or step, in Android's unit; 0 when it said none. */
static float in_android_units(int kind, double v)
{
    if (v <= 0) return 0;
    switch (kind) {
    case MEGLER_SENSOR_ACCELEROMETER:
    case MEGLER_SENSOR_GRAVITY:
    case MEGLER_SENSOR_LINEAR_ACCELERATION: return (float)(v * STANDARD_GRAVITY);
    case MEGLER_SENSOR_GYROMETER: return (float)(v * PI_ / 180);
    case MEGLER_SENSOR_PROXIMITY: return (float)(v / 10);
    case MEGLER_SENSOR_ORIENTATION:
    case MEGLER_SENSOR_RELATIVE_ORIENTATION:
    case MEGLER_SENSOR_INCLINOMETER: return 0;
    default: return (float)v;
    }
}

static void make_sensor(mg_sensor *s, const struct kind_def *d, const megler_sensor_info *info)
{
    float range = in_android_units(d->kind, info->max_range), step = in_android_units(d->kind, info->resolution);
    memset(s, 0, sizeof(*s));
    s->handle = s->type = d->type;
    s->kind = d->kind;
    if (d->name) mg_strlcpy(s->name, d->name, sizeof(s->name));
    else mg_strlcpy(s->name, info->name[0] ? info->name : d->fallback, sizeof(s->name));
    mg_strlcpy(s->vendor, info->vendor[0] ? info->vendor : "Windows", sizeof(s->vendor));
    s->string_type = d->string_type;
    s->version = 1;
    s->max_range = range > 0 ? range : d->range;
    /* A proximity sensor that says how far is read to the millimetre; one that does not, near or far. */
    s->resolution = step > 0 ? step : d->kind == MEGLER_SENSOR_PROXIMITY && range > 0 ? 0.1f : d->resolution;
    s->power_ma = d->power_ma;
    s->reporting_mode = d->mode;
    s->min_delay_us = d->mode == MG_SENSOR_CONTINUOUS ? (info->min_interval_ms > 0 ? info->min_interval_ms * 1000 : 10000) : 0;
    s->max_delay_us = d->mode == MG_SENSOR_CONTINUOUS ? 1000000 : 0;
    /* What Android's getDefaultSensor gives for the proximity is the sensor that wakes the device. */
    s->wake_up = d->type == MG_SENSOR_PROXIMITY;
    s->values = d->values;
}

/* The list, asked of the host the first time anyone wants it. */
static void ensure_list(void)
{
    megler_sensor_info info[24];
    mg_sensor *list;
    char names[900];
    size_t at = 0;
    int n, i, k, count = 0;
    if (g_listed) return;
    mg_lock_acquire(&g_list_lock);
    if (g_listed) {
        mg_lock_release(&g_list_lock);
        return;
    }
    memset(info, 0, sizeof(info));
    n = g_host.list ? g_host.list(g_host.user, info, (int)MG_COUNT(info)) : 0;
    if (n > (int)MG_COUNT(info)) n = (int)MG_COUNT(info);
    list = (mg_sensor *)mg_alloc(sizeof(mg_sensor) * MG_COUNT(k_kinds));
    names[0] = 0;
    for (k = 0; k < (int)MG_COUNT(k_kinds); k++) {
        for (i = 0; i < n && info[i].kind != k_kinds[k].kind; i++) { }
        if (i == n) continue;
        info[i].name[sizeof(info[i].name) - 1] = 0;
        info[i].vendor[sizeof(info[i].vendor) - 1] = 0;
        make_sensor(&list[count], &k_kinds[k], &info[i]);
        if (at + 40 < sizeof(names)) at += (size_t)snprintf(names + at, sizeof(names) - at, "%s%s", count ? ", " : "", list[count].string_type + 15);
        count++;
    }
    mg_lock_acquire(&g_lock);
    /* (A list made before is not freed: native code may still hold its sensors. Only the tests make a second.) */
    g_list = list;
    g_count = count;
    for (k = 0; k <= KINDS; k++) g_index_of_kind[k] = -1;
    for (i = 0; i < count; i++) {
        g_index_of_kind[list[i].kind] = i;
        g_refs[i] = &list[i];
    }
    g_refs[count] = NULL;
    mg_lock_release(&g_lock);
    mg_atomic_xchg(&g_listed, 1);
    mg_lock_release(&g_list_lock);
    if (count) say("the device has %d: %s", count, names);
    else say("the device has none%s", g_host.list ? "" : " (the shell has no sensors to give)");
    for (i = 0; i < count; i++)
        say("%s: %s by %s, of the %s; %.4g at most, in steps of %.4g, %d us apart at least%s", list[i].string_type + 15, list[i].name, list[i].vendor,
            k_said[list[i].kind], list[i].max_range, list[i].resolution, list[i].min_delay_us, list[i].wake_up ? ", wakes the device" : "");
}

void mg_sensors_set_host(const megler_sensor_host *host)
{
    int k;
    mg_lock_acquire(&g_list_lock);
    mg_lock_acquire(&g_host_lock);
    mg_lock_acquire(&g_lock);
    if (host) g_host = *host;
    else memset(&g_host, 0, sizeof(g_host));
    for (k = 0; k <= KINDS; k++) {
        memset(&g_kind[k], 0, sizeof(g_kind[k]));
        g_index_of_kind[k] = -1;
    }
    g_count = 0;
    g_refs[0] = NULL;
    g_paused = g_ended = false;
    mg_atomic_xchg(&g_listed, 0);
    mg_lock_release(&g_lock);
    mg_lock_release(&g_host_lock);
    mg_lock_release(&g_list_lock);
}

int mg_sensors_count(void)
{
    ensure_list();
    return g_count;
}

const mg_sensor *mg_sensors_at(int index)
{
    ensure_list();
    return index >= 0 && index < g_count ? &g_list[index] : NULL;
}

const mg_sensor *mg_sensors_by_handle(int handle)
{
    int i;
    ensure_list();
    for (i = 0; i < g_count; i++)
        if (g_list[i].handle == handle) return &g_list[i];
    return NULL;
}

const mg_sensor *mg_sensors_default(int type)
{
    int i;
    ensure_list();
    for (i = 0; i < g_count; i++)
        if (g_list[i].type == type) return &g_list[i];
    return NULL;
}

/* ------------------------------------------------- running the shell's */

/*
 * Each of the shell's sensors started at the shortest period a queue wants of it, stopped when none wants it, the app is
 * paused or it has ended: what changed is told to the host, in order, outside the list's lock.
 */
static void apply(void)
{
    int k;
    mg_lock_acquire(&g_host_lock);
    for (k = 1; k <= KINDS; k++) {
        bool want = false, had;
        int ms = 0, had_ms, i;
        mg_sensor_queue *q;
        mg_lock_acquire(&g_lock);
        i = g_index_of_kind[k];
        /* (Before any list there is none: nothing to run, and nothing told the host.) */
        if (i >= 0 && i < g_count && !g_paused && !g_ended) {
            int handle = g_list[i].handle;
            for (q = g_queues; q; q = q->next) {
                int this_ms;
                if (!q->sub[handle].on) continue;
                this_ms = (int)(q->sub[handle].period_ns / 1000000);
                if (!want || this_ms < ms) ms = this_ms;
                want = true;
            }
        }
        had = g_kind[k].running;
        had_ms = g_kind[k].interval_ms;
        g_kind[k].running = want;
        g_kind[k].interval_ms = ms;
        if (want && !had) {
            g_kind[k].last_sample_ns = 0;
            g_kind[k].read_once = false;
        }
        mg_lock_release(&g_lock);
        if (want == had && (!want || ms == had_ms)) continue;
        if (want) {
            if (g_host.start) g_host.start(g_host.user, k, ms);
            say("the %s %s every %d ms", k_said[k], had ? "now reported" : "started, reported", ms);
        } else {
            if (g_host.stop) g_host.stop(g_host.user, k);
            say("the %s stopped%s", k_said[k], g_ended ? ": the app has ended" : g_paused ? " while the app is paused" : "");
        }
    }
    mg_lock_release(&g_host_lock);
}

void mg_sensors_set_paused(bool paused)
{
    mg_lock_acquire(&g_lock);
    g_paused = paused;
    mg_lock_release(&g_lock);
    apply();
}

void mg_sensors_end(void)
{
    mg_lock_acquire(&g_lock);
    g_ended = true;
    mg_lock_release(&g_lock);
    apply();
}

/* --------------------------------------------------------------- queues */

/* With the lock held: an event in, the oldest out when there is no room (the newest are what an app wants). */
static void put_locked(mg_sensor_queue *q, const mg_sensor_event *e)
{
    if (q->count == QUEUE_EVENTS) {
        q->head = (q->head + 1) % QUEUE_EVENTS;
        q->count--;
        if (q->dropped++ == 0) mg_warn("sensors: %s takes no events: the oldest are dropped", q->who);
    }
    q->ring[(q->head + q->count) % QUEUE_EVENTS] = *e;
    q->count++;
    /* The descriptor is readable exactly while events wait: it is set as the first comes and read empty as the last goes. */
    if (q->count == 1 && q->fd >= 0) {
        u64 one = 1;
        mg_fd_write(q->fd, &one, 8);
    }
    mg_cond_broadcast(&q->cond);
}

void megler_sensor_push(const megler_sensor_sample *in)
{
    mg_sensor_event e;
    const mg_sensor *s;
    mg_sensor_queue *q;
    i64 age_ms = -1;
    bool first;
    int k = in ? in->kind : 0, i;
    if (k <= 0 || k > KINDS || !g_listed) return;
    mg_lock_acquire(&g_lock);
    i = g_index_of_kind[k];
    /* (Nothing for a sensor stopped since: the shell's last readings may come after its stop.) */
    if (i < 0 || i >= g_count || !g_kind[k].running) {
        mg_lock_release(&g_lock);
        return;
    }
    /* The same reading twice -- the one the sensor had when it was started, and the same again as its first change -- once. */
    if (in->time_ns && in->time_ns == g_kind[k].last_sample_ns) {
        mg_lock_release(&g_lock);
        return;
    }
    g_kind[k].last_sample_ns = in->time_ns;
    s = &g_list[i];
    memset(&e, 0, sizeof(e));
    e.handle = s->handle;
    e.type = s->type;
    e.count = mg_sensors_convert(k, in->values, s->max_range, e.values);
    e.accuracy = accuracy_of(k, in->accuracy);
    e.timestamp = stamp_locked(k, in->time_ns);
    for (q = g_queues; q; q = q->next) {
        if (!q->sub[s->handle].on) continue;
        /* Each queue at about its own period: what comes sooner than four fifths of it after the last is not its. */
        if (s->reporting_mode == MG_SENSOR_CONTINUOUS && q->sub[s->handle].last_ts && e.timestamp - q->sub[s->handle].last_ts < q->sub[s->handle].period_ns * 4 / 5)
            continue;
        q->sub[s->handle].last_ts = e.timestamp;
        put_locked(q, &e);
    }
    first = !g_kind[k].read_once;
    g_kind[k].read_once = true;
    mg_lock_release(&g_lock);
    if (first) {
        if (in->time_ns > 0) age_ms = ((i64)mg_time_realtime_ns() - in->time_ns) / 1000000;
        say("the %s's first reading: %.4g %.4g %.4g %.4g as Android's %.4g %.4g %.4g %.4g, %lld ms old", k_said[k], in->values[0], in->values[1], in->values[2],
            in->values[3], e.values[0], e.values[1], e.values[2], e.values[3], (long long)age_ms);
    }
}

mg_sensor_queue *mg_sensor_queue_create(const char *who, bool with_fd)
{
    mg_sensor_queue *q = (mg_sensor_queue *)mg_alloc(sizeof(*q));
    mg_strlcpy(q->who, who, sizeof(q->who));
    mg_cond_init(&q->cond);
    q->fd = with_fd ? mg_fd_eventfd(0, 04000) : -1;   /* non-blocking */
    mg_lock_acquire(&g_lock);
    q->next = g_queues;
    g_queues = q;
    mg_lock_release(&g_lock);
    return q;
}

void mg_sensor_queue_destroy(mg_sensor_queue *q)
{
    mg_sensor_queue **p;
    bool had = false;
    int h;
    if (!q) return;
    mg_lock_acquire(&g_lock);
    for (p = &g_queues; *p; p = &(*p)->next) {
        if (*p == q) {
            *p = q->next;
            break;
        }
    }
    for (h = 0; h < MG_SENSOR_TYPES; h++) had |= q->sub[h].on;
    mg_lock_release(&g_lock);
    if (had) apply();
    if (q->fd >= 0) mg_fd_close(q->fd);
    /* (The condition is not destroyed: no reader may be left waiting on a queue that is destroyed.) */
    free(q);
}

int mg_sensor_queue_fd(const mg_sensor_queue *q) { return q ? q->fd : -1; }

/* A period as a queue keeps it: within the sensor's least and most (Android's SensorService clamps it so). */
static i64 period_ns_of(const mg_sensor *s, i32 period_us)
{
    i64 us = period_us;
    if (s->reporting_mode == MG_SENSOR_CONTINUOUS) {
        if (us < s->min_delay_us) us = s->min_delay_us;
        if (s->max_delay_us > 0 && us > s->max_delay_us) us = s->max_delay_us;
    }
    return us * 1000;
}

int mg_sensor_queue_enable(mg_sensor_queue *q, int handle, i32 period_us)
{
    const mg_sensor *s = mg_sensors_by_handle(handle);
    bool was;
    if (!q || !s || period_us < 0) return -GE_INVAL_;
    mg_lock_acquire(&g_lock);
    was = q->sub[handle].on;
    q->sub[handle].on = true;
    q->sub[handle].period_ns = period_ns_of(s, period_us);
    if (!was) q->sub[handle].last_ts = 0;
    mg_lock_release(&g_lock);
    say("%s wants the %s (%s) every %d us", q->who, k_said[s->kind], s->string_type + 15, (int)period_us);
    apply();
    return 0;
}

int mg_sensor_queue_set_period(mg_sensor_queue *q, int handle, i32 period_us)
{
    const mg_sensor *s = mg_sensors_by_handle(handle);
    bool on;
    if (!q || !s || period_us < 0) return -GE_INVAL_;
    mg_lock_acquire(&g_lock);
    on = q->sub[handle].on;
    if (on) q->sub[handle].period_ns = period_ns_of(s, period_us);
    mg_lock_release(&g_lock);
    if (on) {
        say("%s wants the %s every %d us", q->who, k_said[s->kind], (int)period_us);
        apply();
    }
    return 0;
}

int mg_sensor_queue_disable(mg_sensor_queue *q, int handle)
{
    const mg_sensor *s = mg_sensors_by_handle(handle);
    bool was;
    if (!q || !s) return -GE_INVAL_;
    mg_lock_acquire(&g_lock);
    was = q->sub[handle].on;
    q->sub[handle].on = false;
    mg_lock_release(&g_lock);
    if (was) {
        say("%s no longer wants the %s", q->who, k_said[s->kind]);
        apply();
    }
    return 0;
}

int mg_sensor_queue_flush(mg_sensor_queue *q, int handle, i32 token)
{
    const mg_sensor *s = mg_sensors_by_handle(handle);
    mg_sensor_event e;
    if (!q || !s) return -GE_INVAL_;
    memset(&e, 0, sizeof(e));
    e.handle = s->handle;
    e.type = s->type;
    e.flush = token ? token : 1;
    e.timestamp = (i64)mg_time_monotonic_ns();
    mg_lock_acquire(&g_lock);
    put_locked(q, &e);
    mg_lock_release(&g_lock);
    return 0;
}

int mg_sensor_queue_read(mg_sensor_queue *q, mg_sensor_event *out, int max, int wait_ms)
{
    int n = 0;
    u64 until = wait_ms > 0 ? mg_time_monotonic_ns() + (u64)wait_ms * 1000000ull : 0;
    if (!q || max <= 0) return 0;
    mg_lock_acquire(&g_lock);
    while (q->count == 0 && wait_ms != 0) {
        if (wait_ms < 0) {
            mg_cond_wait(&q->cond, &g_lock);
        } else {
            u64 now = mg_time_monotonic_ns();
            if (now >= until) break;
            mg_cond_wait_ns(&q->cond, &g_lock, until - now);
        }
    }
    while (n < max && q->count) {
        out[n++] = q->ring[q->head];
        q->head = (q->head + 1) % QUEUE_EVENTS;
        q->count--;
    }
    if (n && !q->count && q->fd >= 0) {
        u64 drained;
        mg_fd_read(q->fd, &drained, 8);
    }
    mg_lock_release(&g_lock);
    return n;
}

int mg_sensor_queue_waiting(mg_sensor_queue *q)
{
    int n;
    if (!q) return 0;
    mg_lock_acquire(&g_lock);
    n = q->count;
    mg_lock_release(&g_lock);
    return n;
}

/* ============================================= libandroid's ASensorManager */

/* The NDK's ASensorEvent, as native code reads it: 104 bytes on every ABI. */
typedef struct g_asensor_event {
    i32 version, sensor, type, reserved0;
    i64 timestamp;
    float data[16];
    u32 flags;
    i32 reserved1[3];
} g_asensor_event;
_Static_assert(sizeof(g_asensor_event) == 104 && offsetof(g_asensor_event, timestamp) == 16 && offsetof(g_asensor_event, flags) == 88,
               "ASensorEvent's layout");

/* An ASensorEventQueue: a queue of the list's, and the looper its descriptor was added to. */
typedef struct g_ndk_queue {
    mg_sensor_queue *q;
    void *looper;
} g_ndk_queue;

static int g_manager_tag;
static void *g_sm_instance(void) { return &g_manager_tag; }
static void *g_sm_instance_for(const char *package) { (void)package; return &g_manager_tag; }

/* ASensorList: an array of ASensorRef, the list's own. */
static int g_sm_list(void *m, const mg_sensor *const **list)
{
    int n = mg_sensors_count();
    (void)m;
    if (list) *list = (const mg_sensor *const *)g_refs;
    return n;
}

/* (Android 13's dynamic sensors: there are none.) */
static int g_sm_dynamic_list(void *m, const mg_sensor *const **list)
{
    static const mg_sensor *const none[1] = { NULL };
    (void)m;
    if (list) *list = none;
    return 0;
}

static const mg_sensor *g_sm_default(void *m, int type)
{
    (void)m;
    return mg_sensors_default(type);
}

static const mg_sensor *g_sm_default_ex(void *m, int type, bool wake_up)
{
    const mg_sensor *s = mg_sensors_default(type);
    (void)m;
    return s && s->wake_up == wake_up ? s : NULL;
}

/* As Android's: the queue's descriptor added to the looper, with the app's identifier or callback, for input. */
static g_ndk_queue *g_sm_create_queue(void *m, void *looper, int ident, mg_addr callback, void *data)
{
    g_ndk_queue *nq = (g_ndk_queue *)mg_alloc(sizeof(*nq));
    (void)m;
    nq->q = mg_sensor_queue_create("native code", true);
    nq->looper = looper;
    if (looper) mg_looper_add_fd(looper, mg_sensor_queue_fd(nq->q), ident, 1, callback, data);
    else mg_note_once("sensors: native code made an event queue on no looper: it can read its events, but nothing tells it of them");
    return nq;
}

static int g_sm_destroy_queue(void *m, g_ndk_queue *nq)
{
    (void)m;
    if (!nq) return -GE_INVAL_;
    if (nq->looper) mg_looper_remove_fd(nq->looper, mg_sensor_queue_fd(nq->q));
    mg_sensor_queue_destroy(nq->q);
    free(nq);
    return 0;
}

/* Direct channels (shared memory the sensors write into): no sensor here has one. */
static int g_sm_direct_channel(void *m, int fd, size_t size) { (void)m; (void)fd; (void)size; return -GE_INVAL_; }
static void g_sm_destroy_channel(void *m, int channel) { (void)m; (void)channel; }
static int g_sm_configure_report(void *m, const mg_sensor *s, int channel, int rate) { (void)m; (void)s; (void)channel; (void)rate; return -GE_INVAL_; }

/* enableSensor is Android's SensorEventQueue::enableSensor: the sensor at SENSOR_DELAY_NORMAL, 200 ms, until setEventRate. */
static int g_q_enable(g_ndk_queue *nq, const mg_sensor *s) { return nq && s ? mg_sensor_queue_enable(nq->q, s->handle, 200000) : -GE_INVAL_; }
static int g_q_register(g_ndk_queue *nq, const mg_sensor *s, i32 period_us, i64 latency_us)
{
    (void)latency_us;
    return nq && s ? mg_sensor_queue_enable(nq->q, s->handle, period_us) : -GE_INVAL_;
}
static int g_q_disable(g_ndk_queue *nq, const mg_sensor *s) { return nq && s ? mg_sensor_queue_disable(nq->q, s->handle) : -GE_INVAL_; }
static int g_q_set_rate(g_ndk_queue *nq, const mg_sensor *s, i32 usec) { return nq && s ? mg_sensor_queue_set_period(nq->q, s->handle, usec) : -GE_INVAL_; }
static int g_q_has_events(g_ndk_queue *nq) { return nq ? mg_sensor_queue_waiting(nq->q) > 0 : -GE_INVAL_; }
static int g_q_additional_info(g_ndk_queue *nq, bool on) { (void)on; return nq ? 0 : -GE_INVAL_; }

/* Up to `count` events, none waited for: 0 when there are none, as Android's (its socket's EAGAIN is no error). */
static gssize g_q_get_events(g_ndk_queue *nq, g_asensor_event *events, size_t count)
{
    mg_sensor_event e[16];
    size_t n = 0;
    if (!nq || !events) return -GE_INVAL_;
    while (n < count) {
        int got = mg_sensor_queue_read(nq->q, e, count - n < MG_COUNT(e) ? (int)(count - n) : (int)MG_COUNT(e), 0), i;
        for (i = 0; i < got; i++, n++) {
            g_asensor_event *a = &events[n];
            memset(a, 0, sizeof(*a));
            a->version = (i32)sizeof(*a);
            a->sensor = e[i].handle;
            a->type = e[i].type;
            a->timestamp = e[i].timestamp;
            memcpy(a->data, e[i].values, sizeof(float) * (size_t)e[i].count);
            /* A vector's accuracy is the byte after its three values (ASensorVector.status). */
            if (e[i].count == 3) ((i8 *)&a->data[3])[0] = (i8)e[i].accuracy;
        }
        if (got < (int)MG_COUNT(e)) break;
    }
    return (gssize)n;
}

/* An ASensor's description; a NULL one answers nothing rather than ending the app. */
static const char *g_s_name(const mg_sensor *s) { return s ? s->name : ""; }
static const char *g_s_vendor(const mg_sensor *s) { return s ? s->vendor : ""; }
static const char *g_s_string_type(const mg_sensor *s) { return s ? s->string_type : ""; }
static int g_s_type(const mg_sensor *s) { return s ? s->type : 0; }
static int g_s_handle(const mg_sensor *s) { return s ? s->handle : 0; }
static gfloat g_s_resolution(const mg_sensor *s) { return mg_gfb(s ? s->resolution : 0.0f); }
static int g_s_min_delay(const mg_sensor *s) { return s ? s->min_delay_us : 0; }
static int g_s_zero(const mg_sensor *s) { (void)s; return 0; }
static int g_s_reporting_mode(const mg_sensor *s) { return s ? s->reporting_mode : 0; }
static bool g_s_wake_up(const mg_sensor *s) { return s && s->wake_up; }
static bool g_s_channel_type(const mg_sensor *s, int type) { (void)s; (void)type; return false; }

#define N(n, f) MG_WRAP(n, f)

static const mg_sym k_ndk[] = {
    N("ASensorManager_getInstance", g_sm_instance), N("ASensorManager_getInstanceForPackage", g_sm_instance_for),
    N("ASensorManager_getSensorList", g_sm_list), N("ASensorManager_getDynamicSensorList", g_sm_dynamic_list),
    N("ASensorManager_getDefaultSensor", g_sm_default), N("ASensorManager_getDefaultSensorEx", g_sm_default_ex),
    N("ASensorManager_createEventQueue", g_sm_create_queue), N("ASensorManager_destroyEventQueue", g_sm_destroy_queue),
    N("ASensorManager_createSharedMemoryDirectChannel", g_sm_direct_channel), N("ASensorManager_createHardwareBufferDirectChannel", g_sm_direct_channel),
    N("ASensorManager_destroyDirectChannel", g_sm_destroy_channel), N("ASensorManager_configureDirectReport", g_sm_configure_report),
    N("ASensorEventQueue_registerSensor", g_q_register), N("ASensorEventQueue_enableSensor", g_q_enable),
    N("ASensorEventQueue_disableSensor", g_q_disable), N("ASensorEventQueue_setEventRate", g_q_set_rate),
    N("ASensorEventQueue_hasEvents", g_q_has_events), N("ASensorEventQueue_getEvents", g_q_get_events),
    N("ASensorEventQueue_requestAdditionalInfoEvents", g_q_additional_info),
    N("ASensor_getName", g_s_name), N("ASensor_getVendor", g_s_vendor), N("ASensor_getType", g_s_type), N("ASensor_getResolution", g_s_resolution),
    N("ASensor_getMinDelay", g_s_min_delay), N("ASensor_getFifoMaxEventCount", g_s_zero), N("ASensor_getFifoReservedEventCount", g_s_zero),
    N("ASensor_getStringType", g_s_string_type), N("ASensor_getReportingMode", g_s_reporting_mode), N("ASensor_isWakeUpSensor", g_s_wake_up),
    N("ASensor_isDirectChannelTypeSupported", g_s_channel_type), N("ASensor_getHighestDirectReportRateLevel", g_s_zero),
    N("ASensor_getHandle", g_s_handle),
    MG_END
};

const mg_sym *mg_sensors_table(void) { return k_ndk; }
