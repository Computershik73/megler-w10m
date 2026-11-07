/*
 * The phone's sensors as Android has them, over the shell's (megler.h, megler_sensor_host): one list of Android's sensors made
 * of what the device has, and queues of their events -- the Java framework's one (drivers/mg_java_sensors.c, behind
 * android.hardware.SensorManager) and one for each ASensorEventQueue of native code (libandroid's, mg_sensors.c).
 *
 * Each Android sensor is made of one of the shell's, in Android's units and with Android's signs on the same axes (the
 * device's own, its natural orientation's); an event's time is SystemClock.elapsedRealtimeNanos's clock. The shell's sensor
 * runs at the shortest period any queue wants of it, and each queue is handed its sensors' events at about its own period;
 * nothing runs while the app is paused, as Android hands an app in the background no events.
 */
#ifndef MG_SENSORS_H
#define MG_SENSORS_H

#include "../../include/megler.h"
#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Android's sensor types that are made here (Sensor.TYPE_*, ASENSOR_TYPE_*): a sensor's handle is its type, there being one of each. */
enum {
    MG_SENSOR_ACCELEROMETER = 1, MG_SENSOR_MAGNETIC_FIELD = 2, MG_SENSOR_ORIENTATION = 3, MG_SENSOR_GYROSCOPE = 4, MG_SENSOR_LIGHT = 5,
    MG_SENSOR_PRESSURE = 6, MG_SENSOR_PROXIMITY = 8, MG_SENSOR_GRAVITY = 9, MG_SENSOR_LINEAR_ACCELERATION = 10,
    MG_SENSOR_ROTATION_VECTOR = 11, MG_SENSOR_GAME_ROTATION_VECTOR = 15, MG_SENSOR_TYPES = 16
};
/* Sensor.REPORTING_MODE_* */
enum { MG_SENSOR_CONTINUOUS = 0, MG_SENSOR_ON_CHANGE = 1, MG_SENSOR_ONE_SHOT = 2, MG_SENSOR_SPECIAL_TRIGGER = 3 };
/* SensorManager.SENSOR_STATUS_* */
enum { MG_SENSOR_STATUS_UNRELIABLE = 0, MG_SENSOR_STATUS_LOW = 1, MG_SENSOR_STATUS_MEDIUM = 2, MG_SENSOR_STATUS_HIGH = 3 };

/* A sensor as Android describes it (android.hardware.Sensor, ASensor). */
typedef struct mg_sensor {
    int handle;                /* its type */
    int type;
    int kind;                  /* the shell's sensor it is made of (MEGLER_SENSOR_*) */
    char name[64], vendor[64];
    const char *string_type;   /* "android.sensor.accelerometer" */
    int version;
    float max_range, resolution, power_ma;
    int min_delay_us;          /* 0 for a sensor that reports changes only */
    int max_delay_us;
    int reporting_mode;
    bool wake_up;
    int values;                /* how many values its events have */
} mg_sensor;

/* The shell's sensors; NULL or one left zero: none. Copied. The list is made again when it is next asked for. */
void mg_sensors_set_host(const megler_sensor_host *host);
int mg_sensors_count(void);
const mg_sensor *mg_sensors_at(int index);
const mg_sensor *mg_sensors_by_handle(int handle);
/* Android's default sensor of a type: of the types Android has waking the device by default (the proximity), the waking one. */
const mg_sensor *mg_sensors_default(int type);
/* While the app is paused, the shell's sensors are stopped; they run again as they were when it is resumed. */
void mg_sensors_set_paused(bool paused);
/* The app has ended: every sensor stopped for good. */
void mg_sensors_end(void);

typedef struct mg_sensor_event {
    int handle, type;
    i64 timestamp;             /* nanoseconds, SystemClock.elapsedRealtimeNanos's clock */
    float values[16];
    int count;                 /* how many of the values are the sensor's */
    int accuracy;              /* MG_SENSOR_STATUS_* */
    i32 flush;                 /* not 0: no reading but the end of a flush, which was asked with this token */
} mg_sensor_event;

typedef struct mg_sensor_queue mg_sensor_queue;
/* `who` is said in the log ("Java", "native code"). With `with_fd`, a descriptor that is readable while events wait. */
mg_sensor_queue *mg_sensor_queue_create(const char *who, bool with_fd);
void mg_sensor_queue_destroy(mg_sensor_queue *q);
int mg_sensor_queue_fd(const mg_sensor_queue *q);
/* The sensor's events at about every period_us (0: as often as it reports), or another period for one already on. 0, or
   Android's negative error (-EINVAL: no such sensor). */
int mg_sensor_queue_enable(mg_sensor_queue *q, int handle, i32 period_us);
/* Another period for a sensor that is on; for one that is off nothing, as Android's SensorDevice keeps it for nobody. */
int mg_sensor_queue_set_period(mg_sensor_queue *q, int handle, i32 period_us);
int mg_sensor_queue_disable(mg_sensor_queue *q, int handle);
/* After the events waiting, an event with `flush` set to token for that sensor. 0, or -EINVAL. */
int mg_sensor_queue_flush(mg_sensor_queue *q, int handle, i32 token);
/* Up to max events taken, oldest first; waits up to wait_ms for one when there is none (0: no wait, -1: for ever). */
int mg_sensor_queue_read(mg_sensor_queue *q, mg_sensor_event *out, int max, int wait_ms);
int mg_sensor_queue_waiting(mg_sensor_queue *q);

/*
 * The shell's reading of `kind` as Android's values (a sensor of the list made of it, `max_range` its maximum range in
 * Android's unit): their count. For the tests; mg_sensors_push uses it.
 */
int mg_sensors_convert(int kind, const double *in, float max_range, float *out);

#ifdef __cplusplus
}
#endif

#endif
