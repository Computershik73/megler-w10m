/*
 * The phone's sensors as Android's (core/src/ndk/mg_sensors.c), over the stand-in for the shell's (mg_sensor_standin.c) with
 * its own thread silent: every reading here is handed over by the tests, at times they choose, so that what comes out --
 * values, units, signs, accuracy, times, how many -- is known exactly. libandroid's functions are called by the names native
 * code binds against (mg_bionic_find), as native code calls them.
 */
#include "mg_sensor_tests.h"

#include <math.h>
#include <stdio.h>

#include "../src/bionic/mg_bionic.h"
#include "../src/bionic/mg_fd.h"
#include "../src/host/mg_host.h"
#include "../src/ndk/mg_sensors.h"
#include "mg_sensor_standin.h"

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

static bool near(float a, double b, double within) { return fabs((double)a - b) <= within; }

/* The values an event of the sensor made of `kind` has, against what Android's sensor reads (3 or more of them). */
static bool values_are(const float *v, int n, const double *want, double within)
{
    int i;
    for (i = 0; i < n; i++)
        if (!near(v[i], want[i], within)) {
            say("  value %d is %.7g, expected %.7g", i, v[i], want[i]);
            return false;
        }
    return true;
}

/* Hands the stand-in's reading of `kind` over, read `ago_ms` before now (below 0: after; 0: no time, now). */
static void push(int kind, double ago_ms)
{
    megler_sensor_sample s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.accuracy = mg_sensor_standin_reading(kind, s.values);
    if (ago_ms != 0) s.time_ns = (long long)mg_time_realtime_ns() - (long long)(ago_ms * 1e6);
    megler_sensor_push(&s);
}

static void push_at(int kind, long long time_ns, const double *values, int accuracy)
{
    megler_sensor_sample s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.accuracy = accuracy;
    memcpy(s.values, values, sizeof(s.values));
    s.time_ns = time_ns;
    megler_sensor_push(&s);
}

/* What the sensors' list says: the stand-in's eleven, as Android has them. */
static void test_list(void)
{
    const mg_sensor *s;
    int i;
    say("the sensors' list, of the stand-in's device");
    mg_sensors_set_host(NULL);
    CHECK_INT(mg_sensors_count(), 0);
    CHECK(mg_sensors_default(MG_SENSOR_ACCELEROMETER) == NULL);
    mg_sensors_set_host(mg_sensor_standin());
    CHECK_INT(mg_sensors_count(), 11);
    /* In Android's order: by type. */
    for (i = 1; i < mg_sensors_count(); i++) CHECK(mg_sensors_at(i - 1)->type < mg_sensors_at(i)->type);
    s = mg_sensors_default(MG_SENSOR_ACCELEROMETER);
    CHECK(s && s->handle == 1 && s->kind == MEGLER_SENSOR_ACCELEROMETER && !strcmp(s->name, "Test Accelerometer") && !strcmp(s->vendor, "Megler Tests")
          && !strcmp(s->string_type, "android.sensor.accelerometer") && s->reporting_mode == MG_SENSOR_CONTINUOUS && !s->wake_up && s->values == 3);
    /* Its range and its least period are Windows', in Android's units: 4 g, 10 ms. */
    CHECK(s && near(s->max_range, 39.2266, 0.0001) && s->min_delay_us == 10000 && s->max_delay_us == 1000000);
    s = mg_sensors_default(MG_SENSOR_GYROSCOPE);
    CHECK(s && near(s->max_range, 34.906586, 0.0001) && !strcmp(s->name, "Test Gyrometer"));
    /* What Windows does not say is a typical part's. */
    s = mg_sensors_default(MG_SENSOR_MAGNETIC_FIELD);
    CHECK(s && near(s->max_range, 4912, 0.01) && s->min_delay_us == 20000);
    /* A sensor Windows makes of others is named as Android names its own such, and is Windows' when Windows names no maker. */
    s = mg_sensors_default(MG_SENSOR_GRAVITY);
    CHECK(s && !strcmp(s->name, "Gravity Sensor") && !strcmp(s->vendor, "Windows") && s->values == 3);
    s = mg_sensors_default(MG_SENSOR_ROTATION_VECTOR);
    CHECK(s && !strcmp(s->name, "Rotation Vector Sensor") && s->values == 5 && near(s->max_range, 1, 0));
    s = mg_sensors_default(MG_SENSOR_GAME_ROTATION_VECTOR);
    CHECK(s && !strcmp(s->string_type, "android.sensor.game_rotation_vector") && s->values == 4);
    s = mg_sensors_default(MG_SENSOR_ORIENTATION);
    CHECK(s && !strcmp(s->name, "Orientation Sensor") && near(s->max_range, 360, 0));
    /* The light and the proximity report changes: no least period. The proximity wakes the device, as Android's default does,
       and reads to the millimetre up to the 5 cm the stand-in says. */
    s = mg_sensors_default(MG_SENSOR_LIGHT);
    CHECK(s && s->reporting_mode == MG_SENSOR_ON_CHANGE && s->min_delay_us == 0 && !s->wake_up && s->values == 1);
    s = mg_sensors_default(MG_SENSOR_PROXIMITY);
    CHECK(s && s->reporting_mode == MG_SENSOR_ON_CHANGE && s->wake_up && near(s->max_range, 5, 0.0001) && near(s->resolution, 0.1, 0.0001));
    /* A part Windows does not name is named for its kind. */
    s = mg_sensors_default(MG_SENSOR_PRESSURE);
    CHECK(s && !strcmp(s->name, "Barometer") && !strcmp(s->vendor, "Windows") && s->min_delay_us == 100000);
    CHECK(mg_sensors_by_handle(15) == mg_sensors_default(MG_SENSOR_GAME_ROTATION_VECTOR) && mg_sensors_by_handle(7) == NULL);
}

/* Every reading in Android's units and signs. */
static void test_conversions(void)
{
    static const double accel[] = { -2.4516625, 4.903325, 7.3549875 }, gravity[] = { 0, 5.88399, 7.84532 }, linear[] = { -0.980665, -1.96133, 2.941995 },
                        gyro[] = { 1.5707963, -0.7853982, 3.1415927 }, magnetic[] = { 20, -30, -40 }, rotation[] = { -0.1, -0.2, -0.3, 0.92736185, -1 },
                        game[] = { 0, 0, 0.6, 0.8 }, orientation[] = { 70.3141, -31.5667, 17.2294 };
    double w[4];
    float v[16];
    say("the readings in Android's units and signs");
    /* Lying face up Windows reads -1 g on z, Android +9.81 m/s^2: the same axes, every sign turned. */
    mg_sensor_standin_reading(MEGLER_SENSOR_ACCELEROMETER, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_ACCELEROMETER, w, 39.2f, v) == 3 && values_are(v, 3, accel, 1e-5));
    w[0] = 0, w[1] = 0, w[2] = -1;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_ACCELEROMETER, w, 39.2f, v) == 3 && near(v[2], 9.80665, 1e-5) && near(v[0], 0, 0) && near(v[1], 0, 0));
    mg_sensor_standin_reading(MEGLER_SENSOR_GRAVITY, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_GRAVITY, w, 19.6f, v) == 3 && values_are(v, 3, gravity, 1e-5));
    mg_sensor_standin_reading(MEGLER_SENSOR_LINEAR_ACCELERATION, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_LINEAR_ACCELERATION, w, 39.2f, v) == 3 && values_are(v, 3, linear, 1e-5));
    /* Degrees a second as radians, counterclockwise both. */
    mg_sensor_standin_reading(MEGLER_SENSOR_GYROMETER, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_GYROMETER, w, 34.9f, v) == 3 && values_are(v, 3, gyro, 1e-6));
    mg_sensor_standin_reading(MEGLER_SENSOR_MAGNETOMETER, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_MAGNETOMETER, w, 4912, v) == 3 && values_are(v, 3, magnetic, 1e-6));
    /* The quaternion as it is, w made positive (the same rotation), and the heading's accuracy unknown. */
    mg_sensor_standin_reading(MEGLER_SENSOR_ORIENTATION, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_ORIENTATION, w, 1, v) == 5 && values_are(v, 5, rotation, 1e-6));
    mg_sensor_standin_reading(MEGLER_SENSOR_RELATIVE_ORIENTATION, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_RELATIVE_ORIENTATION, w, 1, v) == 4 && values_are(v, 4, game, 1e-6));
    /* The inclinometer's pitch 30, roll -20, yaw 300 as Android's old orientation sensor reads that rotation. */
    mg_sensor_standin_reading(MEGLER_SENSOR_INCLINOMETER, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_INCLINOMETER, w, 360, v) == 3 && values_are(v, 3, orientation, 0.001));
    /* Flat and level, the top to the north: 0, 0, 0; the top to the east (yaw a quarter clockwise, -90 or 270): azimuth 90;
       stood upright: pitch -90; the right edge raised 30 degrees (roll -30 as Windows turns it): roll 30. */
    w[0] = 0, w[1] = 0, w[2] = 0;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_INCLINOMETER, w, 360, v) == 3 && near(v[0], 0, 0.001) && near(v[1], 0, 0.001) && near(v[2], 0, 0.001));
    w[2] = 270;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_INCLINOMETER, w, 360, v) == 3 && near(v[0], 90, 0.001));
    w[0] = 90, w[2] = 0;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_INCLINOMETER, w, 360, v) == 3 && near(v[1], -90, 0.001) && near(v[2], 0, 0.001));
    w[0] = 0, w[1] = -30;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_INCLINOMETER, w, 360, v) == 3 && near(v[1], 0, 0.001) && near(v[2], 30, 0.001));
    /* Near 30 mm away: 3 cm; near with no distance said: 0; far: the maximum range, whatever the distance. */
    w[0] = 1, w[1] = 30;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_PROXIMITY, w, 5, v) == 1 && near(v[0], 3, 1e-6));
    w[1] = -1;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_PROXIMITY, w, 5, v) == 1 && near(v[0], 0, 0));
    w[0] = 0, w[1] = 80;
    CHECK(mg_sensors_convert(MEGLER_SENSOR_PROXIMITY, w, 5, v) == 1 && near(v[0], 5, 0));
    mg_sensor_standin_reading(MEGLER_SENSOR_LIGHT, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_LIGHT, w, 10000, v) == 1 && near(v[0], 321.5, 1e-4));
    mg_sensor_standin_reading(MEGLER_SENSOR_BAROMETER, w);
    CHECK(mg_sensors_convert(MEGLER_SENSOR_BAROMETER, w, 1100, v) == 1 && near(v[0], 1013.25, 1e-3));
}

/*
 * A queue: the shell's sensor run at the shortest period wanted, events at the queue's own, their times, pause, flush. (A
 * sensor's events never go back in time, so each sensor's readings here go forward, all of them within the last second.)
 */
static void test_queues(void)
{
    static const double accel[] = { -2.4516625, 4.903325, 7.3549875 };
    mg_sensor_queue *q, *fast;
    mg_sensor_event e[300];
    double w[4];
    i64 before, after, base;
    int starts, stops, ms, n, i;
    say("sensor queues over the stand-in");
    mg_sensors_set_host(mg_sensor_standin());
    mg_sensor_standin_feed(false);
    q = mg_sensor_queue_create("the tests", true);
    CHECK(mg_sensor_queue_fd(q) >= 0);
    CHECK_INT(mg_sensor_queue_enable(q, 7, 20000), -22);
    CHECK_INT(mg_sensor_queue_enable(q, MG_SENSOR_ACCELEROMETER, -1), -22);
    /* Wanted every 20 ms: the shell's accelerometer started at 20 ms. */
    CHECK_INT(mg_sensor_queue_enable(q, MG_SENSOR_ACCELEROMETER, 20000), 0);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, &starts, &stops, &ms);
    CHECK(starts == 1 && stops == 0 && ms == 20 && mg_sensor_standin_running(MEGLER_SENSOR_ACCELEROMETER));
    /* Faster than the queue wants: four milliseconds apart, the queue takes those 20 ms apart (four fifths of it, 16, will do). */
    mg_sensor_standin_reading(MEGLER_SENSOR_ACCELEROMETER, w);
    base = (i64)mg_time_realtime_ns() - 600000000;
    push_at(MEGLER_SENSOR_ACCELEROMETER, base, w, 0);
    push_at(MEGLER_SENSOR_ACCELEROMETER, base + 4000000, w, 0);
    push_at(MEGLER_SENSOR_ACCELEROMETER, base + 8000000, w, 0);
    push_at(MEGLER_SENSOR_ACCELEROMETER, base + 12000000, w, 0);
    push_at(MEGLER_SENSOR_ACCELEROMETER, base + 25000000, w, 0);
    /* The same reading twice (its time the same) is one. */
    push_at(MEGLER_SENSOR_ACCELEROMETER, base + 25000000, w, 0);
    n = mg_sensor_queue_read(q, e, 10, 0);
    CHECK_INT(n, 2);
    if (n == 2) CHECK(e[1].timestamp - e[0].timestamp >= 24000000 && e[1].timestamp - e[0].timestamp <= 27000000);
    /* A second queue wanting it faster than the sensor can: the shell's at its least, 10 ms; that one gets every reading
       10 ms apart, the first every other. */
    fast = mg_sensor_queue_create("the tests' second", false);
    CHECK_INT(mg_sensor_queue_enable(fast, MG_SENSOR_ACCELEROMETER, 5000), 0);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, &starts, NULL, &ms);
    CHECK(starts == 2 && ms == 10);
    for (i = 0; i < 4; i++) push_at(MEGLER_SENSOR_ACCELEROMETER, base + 50000000 + i * 10000000ll, w, 0);
    CHECK_INT(mg_sensor_queue_read(fast, e, 10, 0), 4);
    CHECK_INT(mg_sensor_queue_read(q, e, 10, 0), 2);
    /* Gone, the first's period again. */
    mg_sensor_queue_destroy(fast);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, &starts, &stops, &ms);
    CHECK(starts == 3 && stops == 0 && ms == 20);
    /* A reading three milliseconds old: Android's values, accuracy, and a time three milliseconds before now on its clock. */
    before = (i64)mg_time_monotonic_ns();
    push(MEGLER_SENSOR_ACCELEROMETER, 3);
    after = (i64)mg_time_monotonic_ns();
    n = mg_sensor_queue_read(q, e, 10, 0);
    CHECK_INT(n, 1);
    if (n == 1) {
        CHECK(e[0].handle == 1 && e[0].type == 1 && e[0].count == 3 && e[0].flush == 0 && e[0].accuracy == MG_SENSOR_STATUS_HIGH && values_are(e[0].values, 3, accel, 1e-5));
        CHECK(e[0].timestamp >= before - 3000000 - 2000000 && e[0].timestamp <= after - 3000000 + 2000000);
    }
    /* Paused: the shell's sensor stopped, nothing comes; resumed: started again as it was. */
    mg_sensors_set_paused(true);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, &starts, &stops, &ms);
    CHECK(stops == 1 && !mg_sensor_standin_running(MEGLER_SENSOR_ACCELEROMETER));
    mg_sleep_ns(20000000);
    push(MEGLER_SENSOR_ACCELEROMETER, 0);
    CHECK_INT(mg_sensor_queue_read(q, e, 10, 0), 0);
    mg_sensors_set_paused(false);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, &starts, &stops, &ms);
    CHECK(starts == 4 && ms == 20 && mg_sensor_standin_running(MEGLER_SENSOR_ACCELEROMETER));
    /* The magnetometer's accuracy is Windows' (approximate: Android's medium); the light's every change, however soon. */
    CHECK_INT(mg_sensor_queue_enable(q, MG_SENSOR_MAGNETIC_FIELD, 200000), 0);
    CHECK_INT(mg_sensor_queue_enable(q, MG_SENSOR_LIGHT, 200000), 0);
    push(MEGLER_SENSOR_MAGNETOMETER, 2);
    push(MEGLER_SENSOR_LIGHT, 2);
    push(MEGLER_SENSOR_LIGHT, 1);
    n = mg_sensor_queue_read(q, e, 10, 0);
    CHECK(n == 3 && e[0].type == MG_SENSOR_MAGNETIC_FIELD && e[0].accuracy == MG_SENSOR_STATUS_MEDIUM && e[1].type == MG_SENSOR_LIGHT && e[2].type == MG_SENSOR_LIGHT
          && near(e[2].values[0], 321.5, 1e-4));
    mg_sensor_queue_disable(q, MG_SENSOR_MAGNETIC_FIELD);
    CHECK(!mg_sensor_standin_running(MEGLER_SENSOR_MAGNETOMETER) && mg_sensor_standin_running(MEGLER_SENSOR_LIGHT));
    /* A reading from the future of this clock is now. */
    before = (i64)mg_time_monotonic_ns();
    push(MEGLER_SENSOR_LIGHT, -5);
    n = mg_sensor_queue_read(q, e, 10, 0);
    CHECK(n == 1 && e[0].timestamp >= before && e[0].timestamp <= (i64)mg_time_monotonic_ns());
    /* A flush ends after what waits. */
    push(MEGLER_SENSOR_LIGHT, 0);
    CHECK_INT(mg_sensor_queue_flush(q, MG_SENSOR_LIGHT, 77), 0);
    n = mg_sensor_queue_read(q, e, 10, 0);
    CHECK(n == 2 && e[0].flush == 0 && e[1].flush == 77 && e[1].handle == MG_SENSOR_LIGHT);
    /* The descriptor: readable while events wait, and not once they are taken. */
    {
        mg_pollfd p;
        p.fd = mg_sensor_queue_fd(q);
        p.events = MG_POLLIN;
        p.revents = 0;
        CHECK_INT(mg_fd_poll(&p, 1, 0), 0);
        push(MEGLER_SENSOR_LIGHT, 0);
        p.revents = 0;
        CHECK(mg_fd_poll(&p, 1, 0) == 1 && (p.revents & MG_POLLIN));
        CHECK_INT(mg_sensor_queue_read(q, e, 10, 0), 1);
        p.revents = 0;
        CHECK_INT(mg_fd_poll(&p, 1, 0), 0);
    }
    /* A queue nobody reads keeps the newest 256. */
    base = (i64)mg_time_realtime_ns();
    for (i = 0; i < 300; i++) {
        w[0] = i;
        push_at(MEGLER_SENSOR_LIGHT, base - 400000000 + i * 1000000ll, w, 0);
    }
    n = mg_sensor_queue_read(q, e, 300, 0);
    CHECK(n == 256 && near(e[0].values[0], 44, 1e-3) && near(e[255].values[0], 299, 1e-3));
    /* As fast as it can: the sensor's least period. */
    CHECK_INT(mg_sensor_queue_enable(q, MG_SENSOR_ACCELEROMETER, 0), 0);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, NULL, NULL, &ms);
    CHECK_INT(ms, 10);
    /* A reader waits for the next. */
    before = (i64)mg_time_monotonic_ns();
    CHECK_INT(mg_sensor_queue_read(q, e, 1, 30), 0);
    CHECK((i64)mg_time_monotonic_ns() - before >= 25000000);
    /* Stopped when the app ends. */
    mg_sensors_end();
    CHECK(!mg_sensor_standin_running(MEGLER_SENSOR_ACCELEROMETER) && !mg_sensor_standin_running(MEGLER_SENSOR_LIGHT));
    mg_sensor_queue_destroy(q);
}

/* libandroid's ASensorManager, called by the names native code binds against, with its structures. */
typedef struct t_asensor_event {
    i32 version, sensor, type, reserved0;
    i64 timestamp;
    float data[16];
    u32 flags;
    i32 reserved1[3];
} t_asensor_event;

static void test_ndk(void)
{
    void *(*get_instance)(void) = (void *(*)(void))mg_bionic_find("ASensorManager_getInstance");
    void *(*get_instance_for)(const char *) = (void *(*)(const char *))mg_bionic_find("ASensorManager_getInstanceForPackage");
    int (*get_list)(void *, const void *const **) = (int (*)(void *, const void *const **))mg_bionic_find("ASensorManager_getSensorList");
    const void *(*get_default)(void *, int) = (const void *(*)(void *, int))mg_bionic_find("ASensorManager_getDefaultSensor");
    const void *(*get_default_ex)(void *, int, bool) = (const void *(*)(void *, int, bool))mg_bionic_find("ASensorManager_getDefaultSensorEx");
    void *(*create_queue)(void *, void *, int, mg_addr, void *) = (void *(*)(void *, void *, int, mg_addr, void *))mg_bionic_find("ASensorManager_createEventQueue");
    int (*destroy_queue)(void *, void *) = (int (*)(void *, void *))mg_bionic_find("ASensorManager_destroyEventQueue");
    int (*enable)(void *, const void *) = (int (*)(void *, const void *))mg_bionic_find("ASensorEventQueue_enableSensor");
    int (*disable)(void *, const void *) = (int (*)(void *, const void *))mg_bionic_find("ASensorEventQueue_disableSensor");
    int (*set_rate)(void *, const void *, i32) = (int (*)(void *, const void *, i32))mg_bionic_find("ASensorEventQueue_setEventRate");
    int (*has_events)(void *) = (int (*)(void *))mg_bionic_find("ASensorEventQueue_hasEvents");
    gssize (*get_events)(void *, t_asensor_event *, size_t) = (gssize (*)(void *, t_asensor_event *, size_t))mg_bionic_find("ASensorEventQueue_getEvents");
    const char *(*name)(const void *) = (const char *(*)(const void *))mg_bionic_find("ASensor_getName");
    const char *(*string_type)(const void *) = (const char *(*)(const void *))mg_bionic_find("ASensor_getStringType");
    int (*type)(const void *) = (int (*)(const void *))mg_bionic_find("ASensor_getType");
    int (*min_delay)(const void *) = (int (*)(const void *))mg_bionic_find("ASensor_getMinDelay");
    gfloat (*resolution)(const void *) = (gfloat (*)(const void *))mg_bionic_find("ASensor_getResolution");
    bool (*wake_up)(const void *) = (bool (*)(const void *))mg_bionic_find("ASensor_isWakeUpSensor");
    int (*reporting)(const void *) = (int (*)(const void *))mg_bionic_find("ASensor_getReportingMode");
    static const double accel[] = { -2.4516625, 4.903325, 7.3549875 };
    const void *const *list = NULL;
    const void *acc, *prox;
    void *m, *queue;
    t_asensor_event ev[4];
    int starts, ms, n;
    say("libandroid's ASensorManager over the stand-in");
    CHECK(get_instance && get_instance_for && get_list && get_default && get_default_ex && create_queue && destroy_queue && enable && disable && set_rate
          && has_events && get_events && name && string_type && type && min_delay && resolution && wake_up && reporting);
    CHECK(mg_bionic_find("ASensor_getHandle") && mg_bionic_find("ASensorEventQueue_registerSensor") && mg_bionic_find("ASensor_getFifoMaxEventCount"));
    if (!get_instance || !get_list || !get_default || !get_default_ex || !create_queue || !destroy_queue || !enable || !disable || !set_rate || !has_events
        || !get_events || !name || !string_type || !type || !min_delay || !resolution || !wake_up || !reporting || !get_instance_for)
        return;
    mg_sensors_set_host(mg_sensor_standin());
    mg_sensor_standin_feed(false);
    m = get_instance();
    CHECK(m && get_instance_for("megler.test") == m);
    n = get_list(m, &list);
    CHECK(n == 11 && list && list[0] && type(list[0]) == 1 && list[11] == NULL);
    acc = get_default(m, 1);
    CHECK(acc && !strcmp(name(acc), "Test Accelerometer") && !strcmp(string_type(acc), "android.sensor.accelerometer") && min_delay(acc) == 10000
          && near(mg_gf(resolution(acc)), 0.0023942, 1e-7) && !wake_up(acc) && reporting(acc) == 0);
    /* The proximity's default is the one that wakes the device; asked for one that does not, there is none. */
    prox = get_default(m, 8);
    CHECK(prox && wake_up(prox) && reporting(prox) == 1 && get_default_ex(m, 8, true) == prox && get_default_ex(m, 8, false) == NULL
          && get_default_ex(m, 1, false) == acc && get_default_ex(m, 1, true) == NULL);
    CHECK(get_default(m, 7) == NULL && type(NULL) == 0 && !strcmp(name(NULL), ""));
    /* (No looper: this thread has none, and the descriptor is the queue's own test above.) */
    queue = create_queue(m, NULL, 3, 0, NULL);
    CHECK(queue != NULL);
    /* Enabled, Android's SENSOR_DELAY_NORMAL until a rate is set. */
    CHECK_INT(enable(queue, acc), 0);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, &starts, NULL, &ms);
    CHECK_INT(ms, 200);
    CHECK_INT(set_rate(queue, acc, 20000), 0);
    mg_sensor_standin_counts(MEGLER_SENSOR_ACCELEROMETER, NULL, NULL, &ms);
    CHECK_INT(ms, 20);
    CHECK_INT(set_rate(queue, acc, -1), -22);
    CHECK_INT(has_events(queue), 0);
    CHECK_INT(get_events(queue, ev, 4), 0);
    push(MEGLER_SENSOR_ACCELEROMETER, 2);
    CHECK_INT(has_events(queue), 1);
    n = (int)get_events(queue, ev, 4);
    CHECK_INT(n, 1);
    if (n == 1) {
        CHECK(ev[0].version == 104 && ev[0].sensor == 1 && ev[0].type == 1 && ev[0].timestamp > 0 && values_are(ev[0].data, 3, accel, 1e-5));
        /* ASensorVector.status, the byte after the three values: SENSOR_STATUS_ACCURACY_HIGH. */
        CHECK(((const i8 *)&ev[0].data[3])[0] == 3);
    }
    CHECK_INT(has_events(queue), 0);
    CHECK_INT(disable(queue, acc), 0);
    CHECK(!mg_sensor_standin_running(MEGLER_SENSOR_ACCELEROMETER));
    CHECK_INT(destroy_queue(m, queue), 0);
}

int mg_sensor_tests_run(mg_test_sink sink, void *user, int *checks)
{
    t_checks = t_failed = 0;
    t_sink = sink;
    t_sink_user = user;
    test_list();
    test_conversions();
    test_queues();
    test_ndk();
    /* As it was: no sensors, the stand-in's thread handing its readings over again for the Java apps' runs. */
    mg_sensors_set_host(NULL);
    mg_sensor_standin_feed(true);
    *checks += t_checks;
    return t_failed;
}
