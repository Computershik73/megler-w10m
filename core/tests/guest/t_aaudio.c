/*
 * AAudio as an app that decodes its own sound uses it: libaaudio.so opened by
 * name and its functions looked up, as Spotify's native core does. Run by
 * core/tests/hk_tests.c, which reads the results t_aaudio_run fills in.
 */
#include <stdint.h>

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
int usleep(unsigned);
int strcmp(const char *, const char *);

typedef struct AAudioStreamBuilder AAudioStreamBuilder;
typedef struct AAudioStream AAudioStream;
typedef int32_t (*data_cb_t)(AAudioStream *, void *, void *, int32_t);

static int32_t (*createStreamBuilder)(AAudioStreamBuilder **);
static void (*setSampleRate)(AAudioStreamBuilder *, int32_t);
static void (*setChannelCount)(AAudioStreamBuilder *, int32_t);
static void (*setFormat)(AAudioStreamBuilder *, int32_t);
static void (*setDirection)(AAudioStreamBuilder *, int32_t);
static void (*setDataCallback)(AAudioStreamBuilder *, data_cb_t, void *);
static int32_t (*openStream)(AAudioStreamBuilder *, AAudioStream **);
static int32_t (*deleteBuilder)(AAudioStreamBuilder *);
static int32_t (*requestStart)(AAudioStream *);
static int32_t (*requestStop)(AAudioStream *);
static int32_t (*requestPause)(AAudioStream *);
static int32_t (*requestFlush)(AAudioStream *);
static int32_t (*closeStream)(AAudioStream *);
static int32_t (*getState)(AAudioStream *);
static int32_t (*waitForStateChange)(AAudioStream *, int32_t, int32_t *, int64_t);
static int32_t (*writeStream)(AAudioStream *, const void *, int32_t, int64_t);
static int32_t (*getFramesPerBurst)(AAudioStream *);
static int32_t (*getSampleRate)(AAudioStream *);
static int32_t (*getFormat)(AAudioStream *);
static int64_t (*getFramesWritten)(AAudioStream *);
static int64_t (*getFramesRead)(AAudioStream *);
static int32_t (*getTimestamp)(AAudioStream *, int32_t, int64_t *, int64_t *);
static const char *(*resultToText)(int32_t);

static volatile int g_calls, g_stop_after;
static volatile int g_wrong_frames;

static int32_t fill(AAudioStream *stream, void *user, void *data, int32_t frames)
{
    float *out = (float *)data;
    int32_t i;
    (void)user;
    if (frames != getFramesPerBurst(stream)) g_wrong_frames++;
    for (i = 0; i < frames * 2; i++) out[i] = 0.25f;
    g_calls++;
    return g_stop_after && g_calls >= g_stop_after ? 1 : 0;   /* AAUDIO_CALLBACK_RESULT_STOP */
}

#define LOOK(var, name) (var = (__typeof__(var))dlsym(lib, name)) != 0

static AAudioStream *open_with(int32_t rate, int32_t channels, int32_t format, int32_t direction, data_cb_t cb, int32_t *result)
{
    AAudioStreamBuilder *b = 0;
    AAudioStream *s = 0;
    createStreamBuilder(&b);
    setSampleRate(b, rate);
    setChannelCount(b, channels);
    setFormat(b, format);
    setDirection(b, direction);
    if (cb) setDataCallback(b, cb, 0);
    *result = openStream(b, &s);
    deleteBuilder(b);
    return s;
}

/*
 * r[0]  every function was found                    r[1]  the callback stream opened (0)
 * r[2]  its burst in frames                         r[3]  its rate and format: 48000 * 10 + 2 (float)
 * r[4]  the state after start and waiting (4)       r[5]  callbacks in 300 ms
 * r[6]  callbacks of another size than the burst    r[7]  frames read, more than 0
 * r[8]  getTimestamp while started (0)              r[9]  the state after stop (10)
 * r[10] callbacks after stop, in 100 ms             r[11] the written stream: frames taken of 22050
 * r[12] milliseconds that took                      r[13] a full stream written with no wait: frames taken
 * r[14] an input stream's open (-889)               r[15] a 24-bit packed one's (-883)
 * r[16] a callback stream that stops itself: its state (10) r[17] its callbacks (3)
 * r[18] pause, flush, start and stop of the written stream, all 0, summed
 * r[19] resultToText(-885) is "AAUDIO_ERROR_TIMEOUT"
 */
int t_aaudio_run(int *r)
{
    void *lib = dlopen("libaaudio.so", 0);
    AAudioStream *s;
    int32_t res, next = 0;
    int64_t frame = 0, when = 0;
    static int16_t pcm[22050];
    int i, before;
    for (i = 0; i < 20; i++) r[i] = -1;
    if (!lib) return 0;
    r[0] = LOOK(createStreamBuilder, "AAudio_createStreamBuilder") && LOOK(setSampleRate, "AAudioStreamBuilder_setSampleRate") &&
           LOOK(setChannelCount, "AAudioStreamBuilder_setChannelCount") && LOOK(setFormat, "AAudioStreamBuilder_setFormat") &&
           LOOK(setDirection, "AAudioStreamBuilder_setDirection") && LOOK(setDataCallback, "AAudioStreamBuilder_setDataCallback") &&
           LOOK(openStream, "AAudioStreamBuilder_openStream") && LOOK(deleteBuilder, "AAudioStreamBuilder_delete") &&
           LOOK(requestStart, "AAudioStream_requestStart") && LOOK(requestStop, "AAudioStream_requestStop") &&
           LOOK(requestPause, "AAudioStream_requestPause") && LOOK(requestFlush, "AAudioStream_requestFlush") &&
           LOOK(closeStream, "AAudioStream_close") && LOOK(getState, "AAudioStream_getState") &&
           LOOK(waitForStateChange, "AAudioStream_waitForStateChange") && LOOK(writeStream, "AAudioStream_write") &&
           LOOK(getFramesPerBurst, "AAudioStream_getFramesPerBurst") && LOOK(getSampleRate, "AAudioStream_getSampleRate") &&
           LOOK(getFormat, "AAudioStream_getFormat") && LOOK(getFramesWritten, "AAudioStream_getFramesWritten") &&
           LOOK(getFramesRead, "AAudioStream_getFramesRead") && LOOK(getTimestamp, "AAudioStream_getTimestamp") &&
           LOOK(resultToText, "AAudio_convertResultToText");
    if (!r[0]) return 0;

    /* A callback stream: stereo float at 48 kHz. */
    s = open_with(48000, 2, 2, 0, fill, &res);
    r[1] = res;
    if (!s) return 0;
    r[2] = getFramesPerBurst(s);
    r[3] = getSampleRate(s) * 10 + getFormat(s);
    requestStart(s);
    waitForStateChange(s, 3, &next, 100000000);      /* STARTING: it is already past it */
    r[4] = next;
    usleep(300000);
    r[5] = g_calls;
    r[6] = g_wrong_frames;
    r[7] = getFramesRead(s) > 0;
    r[8] = getTimestamp(s, 1, &frame, &when);
    requestStop(s);
    r[9] = getState(s);
    before = g_calls;
    usleep(100000);
    r[10] = g_calls - before;
    closeStream(s);

    /* A written stream: a second of mono 16-bit at 22050 Hz, taken as fast as it plays. */
    s = open_with(22050, 1, 1, 0, 0, &res);
    if (s) {
        uint64_t t0, t1;
        struct { long s, ns; } ts;
        extern int clock_gettime(int, void *);
        for (i = 0; i < 22050; i++) pcm[i] = (int16_t)(i * 64);
        requestStart(s);
        clock_gettime(1, &ts);
        t0 = (uint64_t)ts.s * 1000 + (uint64_t)ts.ns / 1000000;
        r[11] = writeStream(s, pcm, 22050, 3000000000LL);
        clock_gettime(1, &ts);
        t1 = (uint64_t)ts.s * 1000 + (uint64_t)ts.ns / 1000000;
        r[12] = (int)(t1 - t0);
        /* Paused, it plays nothing: once what it holds is full, a write that does not wait takes nothing more. */
        r[18] = requestPause(s);
        writeStream(s, pcm, 22050, 0);
        r[13] = writeStream(s, pcm, 22050, 0);
        r[18] += requestFlush(s) + requestStart(s) + requestStop(s);
        closeStream(s);
    }

    open_with(48000, 2, 1, 1, 0, &res);               /* AAUDIO_DIRECTION_INPUT */
    r[14] = res;
    open_with(48000, 2, 3, 0, 0, &res);               /* AAUDIO_FORMAT_PCM_I24_PACKED */
    r[15] = res;

    /* A callback that says stop on its third call. */
    g_calls = 0;
    g_stop_after = 3;
    s = open_with(48000, 2, 2, 0, fill, &res);
    if (s) {
        requestStart(s);
        usleep(200000);
        r[16] = getState(s);
        r[17] = g_calls;
        closeStream(s);
    }
    r[19] = !strcmp(resultToText(-885), "AAUDIO_ERROR_TIMEOUT");
    return 1;
}
