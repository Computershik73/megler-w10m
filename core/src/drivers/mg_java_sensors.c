/*
 * megler.Sensors: the phone's sensors behind a Java app's android.hardware.SensorManager (framework-overlay/megler/Sensors.java),
 * over the list and the queues of ndk/mg_sensors.c. The Java side keeps the app's listeners and hands each its events on its
 * Handler's thread; this says what sensors there are, turns each on and off at the shortest period the app's listeners want
 * of it, in the framework's one queue, and hands that queue's events to the thread of the framework's that waits for them.
 */
#include "mg_java.h"

#include <string.h>

#include "../dvm/mg_dvm.h"
#include "../host/mg_host.h"
#include "../ndk/mg_sensors.h"

#define N(name) static void name(mg_jcall *c)

/* The framework's one queue, made when the app first wants a sensor or waits for one. */
static mg_sensor_queue *g_queue;
static mg_lock g_queue_lock = MG_LOCK_INIT;

static mg_sensor_queue *java_queue(void)
{
    mg_lock_acquire(&g_queue_lock);
    if (!g_queue) g_queue = mg_sensor_queue_create("Java", false);
    mg_lock_release(&g_queue_lock);
    return g_queue;
}

/* What describe() fills in, by index (megler.Sensors's constants). */
enum { D_TYPE, D_HANDLE, D_MAX_RANGE, D_RESOLUTION, D_POWER, D_MIN_DELAY, D_MAX_DELAY, D_REPORTING_MODE, D_WAKE_UP, D_VALUES, D_VERSION, D_NUMBERS };

N(Sensors_count) { c->ret.i = mg_sensors_count(); }

/* describe(int index, float[] numbers): false for no such sensor */
N(Sensors_describe)
{
    const mg_sensor *s = mg_sensors_at(c->args[0].i);
    mg_jobj *a = c->args[1].l;
    float *v;
    c->ret.z = 0;
    if (!s || !a || a->u.arr.len < D_NUMBERS) return;
    v = (float *)a->u.arr.data;
    v[D_TYPE] = (float)s->type;
    v[D_HANDLE] = (float)s->handle;
    v[D_MAX_RANGE] = s->max_range;
    v[D_RESOLUTION] = s->resolution;
    v[D_POWER] = s->power_ma;
    v[D_MIN_DELAY] = (float)s->min_delay_us;
    v[D_MAX_DELAY] = (float)s->max_delay_us;
    v[D_REPORTING_MODE] = (float)s->reporting_mode;
    v[D_WAKE_UP] = s->wake_up ? 1.0f : 0.0f;
    v[D_VALUES] = (float)s->values;
    v[D_VERSION] = (float)s->version;
    c->ret.z = 1;
}

/* text(int index, int what): its name (0), vendor (1) or string type (2) */
N(Sensors_text)
{
    const mg_sensor *s = mg_sensors_at(c->args[0].i);
    int what = c->args[1].i;
    c->ret.l = s ? mg_dvm_new_string(what == 0 ? s->name : what == 1 ? s->vendor : s->string_type) : NULL;
}

/* enable(int handle, int periodUs), disable(int handle), flush(int handle, int token) */
N(Sensors_enable) { c->ret.z = mg_sensor_queue_enable(java_queue(), c->args[0].i, c->args[1].i) == 0; }
N(Sensors_disable) { mg_sensor_queue_disable(java_queue(), c->args[0].i); }
N(Sensors_flush) { c->ret.z = mg_sensor_queue_flush(java_queue(), c->args[0].i, c->args[1].i) == 0; }

/*
 * next(float[] values, long[] meta, int timeoutMs): the next event's handle, its values into `values` and into meta its
 * timestamp, accuracy, flush token (0 for a reading) and how many values it has; 0 when none came within the time.
 */
N(Sensors_next)
{
    mg_jobj *values = c->args[0].l, *meta = c->args[1].l;
    mg_sensor_event e;
    c->ret.i = 0;
    if (!values || !meta || values->u.arr.len < 16 || meta->u.arr.len < 4) return;
    if (mg_sensor_queue_read(java_queue(), &e, 1, c->args[2].i) != 1) return;
    memcpy(values->u.arr.data, e.values, sizeof(e.values));
    ((i64 *)meta->u.arr.data)[0] = e.timestamp;
    ((i64 *)meta->u.arr.data)[1] = e.accuracy;
    ((i64 *)meta->u.arr.data)[2] = e.flush;
    ((i64 *)meta->u.arr.data)[3] = e.count;
    c->ret.i = e.handle;
}

static const mg_jhle k_sensors[] = {
    { "megler/Sensors", "count", NULL, Sensors_count },
    { "megler/Sensors", "describe", NULL, Sensors_describe },
    { "megler/Sensors", "text", NULL, Sensors_text },
    { "megler/Sensors", "enable", NULL, Sensors_enable },
    { "megler/Sensors", "disable", NULL, Sensors_disable },
    { "megler/Sensors", "flush", NULL, Sensors_flush },
    { "megler/Sensors", "next", NULL, Sensors_next },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_sensors_natives(void) { return k_sensors; }
