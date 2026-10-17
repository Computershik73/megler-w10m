/*
 * libaaudio.so: AAudio's output streams over the mixer (hk_audio.c).
 *
 * AAudio (Android 8.0) is the NDK's own way to play PCM, which apps that
 * decode their sound themselves use where it is there: Spotify's native core
 * opens libaaudio.so by name and plays every track through it. A stream is
 * a voice of the mixer fed from a ring of the stream's own buffers, each
 * "a burst" long (10 ms, or the frames per callback the app asked for). The
 * sound reaches them in one of AAudio's two ways:
 *
 *   - a data callback: when a buffer has been played, the mixer's thread asks
 *     the app for the next one, as Android's audio thread does; at the start
 *     the ring is filled with silence, so the first callbacks come at once;
 *   - AAudioStream_write: the app's frames are copied into the ring, and the
 *     call waits, up to its timeout, while every buffer is waiting to be
 *     played. What a write leaves in a buffer it did not fill goes to the
 *     mixer as it is, a shorter buffer, so that nothing written waits for
 *     the next write to be heard.
 *
 * Java's AudioTrack, and the music MediaPlayer decodes, are written streams
 * too (hk_pcm_*, for drivers/hk_java_audio.c): a blocking write there also
 * waits while a paused stream is full, as AudioTrack's does.
 *
 * Output only: there is no microphone here, and an input stream is refused
 * as unavailable. Formats are those the mixer takes (16-bit, 32-bit, float);
 * 24-bit packed is refused as an invalid format, which apps take as a reason
 * to ask for another. A stream's state changes when it is asked to, never
 * later: there is no STARTING between OPEN and STARTED.
 *
 * A closed stream's record is kept, not freed: the mixer may be about to
 * call back into it from a chunk it mixed before the stream was closed, and
 * the record says it is closed. Its buffers are freed.
 */
#include "../bionic/hk_bionic.h"
#include "../host/hk_host.h"
#include "hk_audio.h"

#define AA_OK 0
#define AA_ERROR_ILLEGAL_ARGUMENT (-898)
#define AA_ERROR_INVALID_STATE (-895)
#define AA_ERROR_INVALID_HANDLE (-892)
#define AA_ERROR_UNIMPLEMENTED (-890)
#define AA_ERROR_UNAVAILABLE (-889)
#define AA_ERROR_NULL (-886)
#define AA_ERROR_TIMEOUT (-885)
#define AA_ERROR_INVALID_FORMAT (-883)
#define AA_ERROR_OUT_OF_RANGE (-882)
#define AA_ERROR_INVALID_RATE (-880)

enum {
    ST_UNINITIALIZED, ST_UNKNOWN, ST_OPEN, ST_STARTING, ST_STARTED, ST_PAUSING, ST_PAUSED, ST_FLUSHING, ST_FLUSHED,
    ST_STOPPING, ST_STOPPED, ST_CLOSING, ST_CLOSED, ST_DISCONNECTED
};
enum { FMT_UNSPECIFIED = 0, FMT_I16 = 1, FMT_FLOAT = 2, FMT_I24_PACKED = 3, FMT_I32 = 4 };
enum { DIR_OUTPUT = 0, DIR_INPUT = 1 };
enum { CALLBACK_CONTINUE = 0, CALLBACK_STOP = 1 };

/* Buffers in the ring; at most one fewer may wait to be played, so the one being filled is never one of them. */
#define AA_BUFFERS 16
#define AA_DEFAULT_IN_USE 4

typedef struct aa_builder {
    i32 device, rate, channels, format, sharing, direction, capacity, performance, usage, content, preset, session, policy;
    i32 frames_per_callback, channel_mask, spatialization;
    bool privacy, spatialized;
    hk_addr data_cb, error_cb;
    void *data_user, *error_user;
} aa_builder;

typedef struct aa_stream {
    aa_builder b;                      /* as asked, with what was unspecified filled in */
    hk_voice *voice;
    hk_lock lock;
    hk_cond cond;                      /* a buffer played, or the state changed */
    volatile i32 state;
    u32 frame_bytes, burst, in_use;    /* bytes per frame; frames per buffer; buffers that may wait at once */
    u8 *mem;                           /* AA_BUFFERS * burst frames */
    u32 next, fill;                    /* the buffer being filled, and how many of its frames are */
    u32 waiting;                       /* buffers handed to the mixer and not yet played */
    u32 frames_in[AA_BUFFERS];         /* how many frames each buffer handed over holds */
    u32 waiting_frames;                /* and those of the buffers waiting, together */
    u32 generation;                    /* one more at each flush or stop: a waiting write gives up */
    u64 written;
    u32 xruns;
    i32 busy;                          /* the mixer's thread is in this stream's callback */
    bool closed, released, callback_stopped;
    bool java;                         /* a Java track's (hk_pcm_open): not said as AAudio's */
} aa_stream;

/* What an app asks of AAudio, said for its first calls: a stream it gives up on is the last line here. */
static volatile i32 g_said;
#define AA_SAY(...)                                                                                     \
    do {                                                                                                \
        if (hk_atomic_add(&g_said, 1) <= 60) hk_info("aaudio: " __VA_ARGS__);                           \
    } while (0)

static const char *format_name(i32 f)
{
    switch (f) {
    case FMT_I16: return "16-bit";
    case FMT_FLOAT: return "float";
    case FMT_I24_PACKED: return "24-bit packed";
    case FMT_I32: return "32-bit";
    default: return "unspecified";
    }
}

static const char *state_name(i32 s)
{
    static const char *const names[] = { "AAUDIO_STREAM_STATE_UNINITIALIZED", "AAUDIO_STREAM_STATE_UNKNOWN", "AAUDIO_STREAM_STATE_OPEN",
                                         "AAUDIO_STREAM_STATE_STARTING", "AAUDIO_STREAM_STATE_STARTED", "AAUDIO_STREAM_STATE_PAUSING",
                                         "AAUDIO_STREAM_STATE_PAUSED", "AAUDIO_STREAM_STATE_FLUSHING", "AAUDIO_STREAM_STATE_FLUSHED",
                                         "AAUDIO_STREAM_STATE_STOPPING", "AAUDIO_STREAM_STATE_STOPPED", "AAUDIO_STREAM_STATE_CLOSING",
                                         "AAUDIO_STREAM_STATE_CLOSED", "AAUDIO_STREAM_STATE_DISCONNECTED" };
    return s >= 0 && s < (i32)HK_COUNT(names) ? names[s] : "Unrecognized AAudio state.";
}

static void set_state(aa_stream *s, i32 state)
{
    s->state = state;
    hk_cond_broadcast(&s->cond);
}

/* ---------------------------------------------------------------- builder */

static i32 AAudio_createStreamBuilder(aa_builder **out)
{
    aa_builder *b;
    if (!out) return AA_ERROR_NULL;
    b = (aa_builder *)hk_alloc(sizeof(*b));
    b->sharing = 1;                    /* AAUDIO_SHARING_MODE_SHARED */
    b->performance = 10;               /* AAUDIO_PERFORMANCE_MODE_NONE */
    b->usage = 1;                      /* AAUDIO_USAGE_MEDIA */
    b->content = 2;                    /* AAUDIO_CONTENT_TYPE_MUSIC */
    b->preset = 1;                     /* AAUDIO_INPUT_PRESET_GENERIC */
    b->session = -1;                   /* AAUDIO_SESSION_ID_NONE */
    b->policy = 1;                     /* AAUDIO_ALLOW_CAPTURE_BY_ALL */
    b->spatialization = 1;             /* AAUDIO_SPATIALIZATION_BEHAVIOR_AUTO */
    *out = b;
    return AA_OK;
}

#define SETTER(name, field)                                         \
    static void AAudioStreamBuilder_##name(aa_builder *b, i32 v)  \
    {                                                               \
        if (b) b->field = v;                                        \
    }
SETTER(setDeviceId, device)
SETTER(setSampleRate, rate)
SETTER(setChannelCount, channels)
SETTER(setSamplesPerFrame, channels)
SETTER(setFormat, format)
SETTER(setSharingMode, sharing)
SETTER(setDirection, direction)
SETTER(setBufferCapacityInFrames, capacity)
SETTER(setPerformanceMode, performance)
SETTER(setUsage, usage)
SETTER(setContentType, content)
SETTER(setInputPreset, preset)
SETTER(setSessionId, session)
SETTER(setAllowedCapturePolicy, policy)
SETTER(setFramesPerDataCallback, frames_per_callback)
SETTER(setChannelMask, channel_mask)
SETTER(setSpatializationBehavior, spatialization)
#undef SETTER

static void AAudioStreamBuilder_setPrivacySensitive(aa_builder *b, bool v)
{
    if (b) b->privacy = v;
}

static void AAudioStreamBuilder_setIsContentSpatialized(aa_builder *b, bool v)
{
    if (b) b->spatialized = v;
}

static void AAudioStreamBuilder_setDataCallback(aa_builder *b, hk_addr cb, void *user)
{
    if (!b) return;
    b->data_cb = cb;
    b->data_user = user;
}

static void AAudioStreamBuilder_setErrorCallback(aa_builder *b, hk_addr cb, void *user)
{
    if (!b) return;
    b->error_cb = cb;
    b->error_user = user;
}

/* The package name and attribution tag (Android 12) say who plays; nothing here asks. */
static i32 AAudioStreamBuilder_setPackageName(aa_builder *b, const char *name)
{
    (void)b; (void)name;
    return AA_OK;
}

static i32 AAudioStreamBuilder_delete(aa_builder *b)
{
    free(b);
    return AA_OK;
}

/* ----------------------------------------------------------------- stream */

static void refill(aa_stream *s);

/* A buffer has been played: on the mixer's thread, outside its lock. */
static void voice_done(hk_voice *v, void *user)
{
    aa_stream *s = (aa_stream *)user;
    (void)v;
    hk_lock_acquire(&s->lock);
    if (s->closed || s->released) {
        hk_lock_release(&s->lock);
        return;
    }
    if (s->waiting) {
        u32 oldest = (s->next + AA_BUFFERS - s->waiting) % AA_BUFFERS;
        s->waiting_frames -= s->frames_in[oldest] < s->waiting_frames ? s->frames_in[oldest] : s->waiting_frames;
        s->waiting--;
    }
    if (!s->waiting && s->state == ST_STARTED) s->xruns++;
    hk_cond_broadcast(&s->cond);
    s->busy++;
    if (s->b.data_cb) refill(s);
    s->busy--;
    hk_cond_broadcast(&s->cond);
    hk_lock_release(&s->lock);
}

static u8 *buffer_at(aa_stream *s, u32 i) { return s->mem + (size_t)i * s->burst * s->frame_bytes; }

/* The buffer being filled goes to the mixer: full, or as much of it as a write filled. With the lock held. */
static void hand_over(aa_stream *s)
{
    u32 frames = s->fill ? s->fill : s->burst;
    if (!hk_voice_enqueue(s->voice, buffer_at(s, s->next), frames * s->frame_bytes)) return;
    s->frames_in[s->next] = frames;
    s->waiting++;
    s->waiting_frames += frames;
    s->written += frames;
    s->next = (s->next + 1) % AA_BUFFERS;
    s->fill = 0;
}

/*
 * Callback streams: the app fills buffers until as many wait as may. Called with the lock held, which is let go
 * while the app's callback runs: the callback may ask the stream things.
 */
static void refill(aa_stream *s)
{
    while (s->state == ST_STARTED && !s->callback_stopped && s->waiting < s->in_use) {
        u8 *buf = buffer_at(s, s->next);
        i32 result;
        hk_lock_release(&s->lock);
        result = HK_GFN(i32 (*)(aa_stream *, void *, void *, i32), s->b.data_cb)(s, s->b.data_user, buf, (i32)s->burst);
        hk_lock_acquire(&s->lock);
        if (s->closed || s->released || s->state != ST_STARTED) return;
        hand_over(s);
        if (result == CALLBACK_STOP) {
            /* What it gave last is still played; no more is asked for. */
            s->callback_stopped = true;
            set_state(s, ST_STOPPED);
        }
    }
}

static i32 open_stream(aa_builder *b, aa_stream **out, bool java)
{
    aa_stream *s;
    i32 channels, format, rate;
    if (!b || !out) return AA_ERROR_NULL;
    *out = NULL;
    channels = b->channels;
    if (!channels && b->channel_mask) {
        u32 m = (u32)b->channel_mask;
        for (channels = 0; m; m &= m - 1) channels++;
    }
    if (!channels) channels = 2;
    format = b->format ? b->format : FMT_I16;
    rate = b->rate ? b->rate : HK_AUDIO_RATE;
    if (!java)
        AA_SAY("openStream: %s, %d Hz, %d channels, %s, %s, %d frames per callback, capacity %d frames, performance mode %d",
               b->direction == DIR_INPUT ? "input" : "output", (int)rate, (int)channels, format_name(format), b->data_cb ? "a data callback" : "written",
               (int)b->frames_per_callback, (int)b->capacity, (int)b->performance);
    if (b->direction == DIR_INPUT) {
        hk_note_once("aaudio: an input stream was asked for; there is no microphone here, and the app is told it is unavailable");
        return AA_ERROR_UNAVAILABLE;
    }
    if (b->direction != DIR_OUTPUT) return AA_ERROR_ILLEGAL_ARGUMENT;
    if (format != FMT_I16 && format != FMT_FLOAT && format != FMT_I32) {
        hk_note_once("aaudio: a stream in %s samples was asked for; only 16-bit, 32-bit and float are mixed", format_name(format));
        return AA_ERROR_INVALID_FORMAT;
    }
    if (rate < 8000 || rate > 192000) return AA_ERROR_INVALID_RATE;
    if (channels > 8) return AA_ERROR_OUT_OF_RANGE;
    s = (aa_stream *)hk_alloc(sizeof(*s));
    s->b = *b;
    s->b.channels = channels;
    s->b.format = format;
    s->b.rate = rate;
    s->frame_bytes = (u32)channels * (format == FMT_I16 ? 2u : 4u);
    s->voice = hk_voice_create(rate, channels, format == FMT_I16 ? 16 : 32, format == FMT_FLOAT);
    if (!s->voice) {
        free(s);
        return AA_ERROR_INVALID_FORMAT;
    }
    s->burst = b->frames_per_callback > 0 ? (u32)b->frames_per_callback : (u32)rate / 100;
    if (s->burst < 16) s->burst = 16;
    if (s->burst > (u32)rate) s->burst = (u32)rate;
    s->in_use = AA_DEFAULT_IN_USE;
    if (b->capacity > 0) {
        u32 n = ((u32)b->capacity + s->burst - 1) / s->burst;
        s->in_use = n < 2 ? 2 : n > AA_BUFFERS - 1 ? AA_BUFFERS - 1 : n;
    }
    s->mem = (u8 *)hk_alloc((size_t)AA_BUFFERS * s->burst * s->frame_bytes);
    hk_lock_init(&s->lock);
    hk_cond_init(&s->cond);
    s->state = ST_OPEN;
    s->java = java;
    hk_voice_set_done(s->voice, voice_done, s);
    if (java) hk_info("audio: a Java track, %d Hz, %d channels", (int)rate, (int)channels);
    else
        hk_info("aaudio: stream %d Hz, %d channels, %s, %s, %u frames a burst", (int)rate, (int)channels, format_name(format),
                b->data_cb ? "fed by its callback" : "written to", (unsigned)s->burst);
    *out = s;
    return AA_OK;
}

static i32 AAudioStreamBuilder_openStream(aa_builder *b, aa_stream **out) { return open_stream(b, out, false); }

static i32 AAudioStream_requestStart(aa_stream *s)
{
    if (!s) return AA_ERROR_NULL;
    hk_lock_acquire(&s->lock);
    if (s->closed || s->released) {
        hk_lock_release(&s->lock);
        return AA_ERROR_INVALID_STATE;
    }
    if (!s->java) AA_SAY("requestStart (from %s)", state_name(s->state));
    s->callback_stopped = false;
    set_state(s, ST_STARTED);
    /* A callback stream starts on silence, as many buffers as may wait: the mixer asks for the first real one when the first of them has played. */
    if (s->b.data_cb)
        while (s->waiting < s->in_use) {
            memset(buffer_at(s, s->next), 0, (size_t)s->burst * s->frame_bytes);
            hand_over(s);
        }
    hk_lock_release(&s->lock);
    hk_voice_set_playing(s->voice, true);
    return AA_OK;
}

static i32 AAudioStream_requestPause(aa_stream *s)
{
    if (!s) return AA_ERROR_NULL;
    hk_voice_set_playing(s->voice, false);
    hk_lock_acquire(&s->lock);
    if (!s->closed && !s->released) set_state(s, ST_PAUSED);
    hk_lock_release(&s->lock);
    return AA_OK;
}

/* What waits to be played, and what is half filled, is gone. With the lock held. */
static void drop_queued(aa_stream *s)
{
    hk_voice_clear(s->voice);
    s->waiting = 0;
    s->waiting_frames = 0;
    s->fill = 0;
    s->generation++;
    hk_cond_broadcast(&s->cond);
}

static i32 AAudioStream_requestFlush(aa_stream *s)
{
    if (!s) return AA_ERROR_NULL;
    hk_lock_acquire(&s->lock);
    if (s->state == ST_STARTED || s->closed || s->released) {
        hk_lock_release(&s->lock);
        return AA_ERROR_INVALID_STATE;
    }
    drop_queued(s);
    set_state(s, ST_FLUSHED);
    hk_lock_release(&s->lock);
    return AA_OK;
}

static i32 AAudioStream_requestStop(aa_stream *s)
{
    if (!s) return AA_ERROR_NULL;
    hk_voice_set_playing(s->voice, false);
    hk_lock_acquire(&s->lock);
    if (!s->closed && !s->released) {
        drop_queued(s);
        set_state(s, ST_STOPPED);
    }
    hk_lock_release(&s->lock);
    return AA_OK;
}

/* The mixer lets the stream go and its buffers are freed; the record stays (see the top). */
static void let_go(aa_stream *s, i32 state)
{
    bool first;
    hk_lock_acquire(&s->lock);
    first = !s->released;
    s->released = true;
    set_state(s, state);
    hk_lock_release(&s->lock);
    if (!first) return;
    hk_voice_destroy(s->voice);
    hk_lock_acquire(&s->lock);
    while (s->busy) hk_cond_wait(&s->cond, &s->lock);
    hk_lock_release(&s->lock);
    free(s->mem);
    s->mem = NULL;
}

/* Android 11: the stream's resources go, the handle stays until close. */
static i32 AAudioStream_release(aa_stream *s)
{
    if (!s) return AA_ERROR_NULL;
    let_go(s, ST_CLOSING);
    return AA_OK;
}

static i32 AAudioStream_close(aa_stream *s)
{
    if (!s) return AA_ERROR_NULL;
    if (!s->java) AA_SAY("close");
    let_go(s, ST_CLOSED);
    hk_lock_acquire(&s->lock);
    s->closed = true;
    hk_lock_release(&s->lock);
    return AA_OK;
}

static i32 AAudioStream_getState(aa_stream *s) { return s ? s->state : ST_UNINITIALIZED; }

static i32 AAudioStream_waitForStateChange(aa_stream *s, i32 input, i32 *next, i64 timeout_ns)
{
    u64 end;
    i32 result = AA_OK;
    if (!s) return AA_ERROR_NULL;
    end = hk_time_monotonic_ns() + (timeout_ns > 0 ? (u64)timeout_ns : 0);
    hk_lock_acquire(&s->lock);
    while (s->state == input) {
        u64 now = hk_time_monotonic_ns();
        if (now >= end) {
            result = AA_ERROR_TIMEOUT;
            break;
        }
        hk_cond_wait_ns(&s->cond, &s->lock, end - now);
    }
    if (next) *next = s->state;
    hk_lock_release(&s->lock);
    return result;
}

/*
 * Frames into the ring: as many as there is room for, waiting while every buffer waits to be played -- up to the
 * timeout, or with `forever` until there is room. A buffer goes to the mixer when it is full, and what is left in one
 * at the end goes as it is. Only a playing stream makes room: before the start (priming) or when paused, an AAudio
 * write takes what fits and returns; AudioTrack's (`any_state`) waits for the play. A flush or a stop ends the wait.
 * How many frames were taken.
 */
static i32 stream_write(aa_stream *s, const void *data, i32 frames, i64 timeout_ns, bool forever, bool any_state)
{
    const u8 *from = (const u8 *)data;
    u64 end = hk_time_monotonic_ns() + (timeout_ns > 0 ? (u64)timeout_ns : 0);
    u32 generation;
    i32 done = 0;
    hk_lock_acquire(&s->lock);
    generation = s->generation;
    while (done < frames && !s->closed && !s->released && s->generation == generation) {
        u32 n;
        if (!s->fill && s->waiting >= s->in_use) {
            u64 now = hk_time_monotonic_ns();
            if (forever) {
                if (s->state != ST_STARTED && !any_state) break;
                hk_cond_wait_ns(&s->cond, &s->lock, 100000000ull);
                continue;
            }
            if ((s->state != ST_STARTED && !any_state) || now >= end) break;
            hk_cond_wait_ns(&s->cond, &s->lock, end - now);
            continue;
        }
        n = s->burst - s->fill;
        if (n > (u32)(frames - done)) n = (u32)(frames - done);
        memcpy(buffer_at(s, s->next) + (size_t)s->fill * s->frame_bytes, from + (size_t)done * s->frame_bytes, (size_t)n * s->frame_bytes);
        s->fill += n;
        done += (i32)n;
        if (s->fill == s->burst) hand_over(s);
    }
    if (s->fill && !s->closed && !s->released) hand_over(s);
    hk_lock_release(&s->lock);
    return done;
}

static i32 AAudioStream_write(aa_stream *s, const void *data, i32 frames, i64 timeout_ns)
{
    if (!s || !data) return AA_ERROR_NULL;
    if (frames < 0) return AA_ERROR_ILLEGAL_ARGUMENT;
    if (s->b.data_cb || s->closed || s->released) return AA_ERROR_INVALID_STATE;     /* a callback stream is not written to */
    return stream_write(s, data, frames, timeout_ns, false, false);
}

static i32 AAudioStream_read(aa_stream *s, void *data, i32 frames, i64 timeout_ns)
{
    (void)data; (void)frames; (void)timeout_ns;
    return s ? AA_ERROR_UNAVAILABLE : AA_ERROR_NULL;
}

static i32 AAudioStream_setBufferSizeInFrames(aa_stream *s, i32 frames)
{
    u32 n;
    if (!s) return AA_ERROR_NULL;
    if (frames < 0) return AA_ERROR_ILLEGAL_ARGUMENT;
    n = ((u32)frames + s->burst - 1) / s->burst;
    hk_lock_acquire(&s->lock);
    s->in_use = n < 2 ? 2 : n > AA_BUFFERS - 1 ? AA_BUFFERS - 1 : n;
    hk_lock_release(&s->lock);
    return (i32)(s->in_use * s->burst);
}

static i32 AAudioStream_getBufferSizeInFrames(aa_stream *s) { return s ? (i32)(s->in_use * s->burst) : AA_ERROR_NULL; }
static i32 AAudioStream_getFramesPerBurst(aa_stream *s) { return s ? (i32)s->burst : AA_ERROR_NULL; }
static i32 AAudioStream_getBufferCapacityInFrames(aa_stream *s) { return s ? (i32)((AA_BUFFERS - 1) * s->burst) : AA_ERROR_NULL; }
static i32 AAudioStream_getFramesPerDataCallback(aa_stream *s) { return s ? s->b.frames_per_callback : AA_ERROR_NULL; }
static i32 AAudioStream_getXRunCount(aa_stream *s) { return s ? (i32)s->xruns : AA_ERROR_NULL; }
static i32 AAudioStream_getSampleRate(aa_stream *s) { return s ? s->b.rate : AA_ERROR_NULL; }
static i32 AAudioStream_getChannelCount(aa_stream *s) { return s ? s->b.channels : AA_ERROR_NULL; }
static i32 AAudioStream_getDeviceId(aa_stream *s) { return s ? 1 : AA_ERROR_NULL; }
static i32 AAudioStream_getFormat(aa_stream *s) { return s ? s->b.format : AA_ERROR_NULL; }
static i32 AAudioStream_getSharingMode(aa_stream *s) { return s ? 1 : AA_ERROR_NULL; }
static i32 AAudioStream_getPerformanceMode(aa_stream *s) { return s ? s->b.performance : AA_ERROR_NULL; }
static i32 AAudioStream_getDirection(aa_stream *s) { return s ? DIR_OUTPUT : AA_ERROR_NULL; }
static i32 AAudioStream_getSessionId(aa_stream *s) { return s ? s->b.session : AA_ERROR_NULL; }
static i32 AAudioStream_getUsage(aa_stream *s) { return s ? s->b.usage : AA_ERROR_NULL; }
static i32 AAudioStream_getContentType(aa_stream *s) { return s ? s->b.content : AA_ERROR_NULL; }
static i32 AAudioStream_getInputPreset(aa_stream *s) { return s ? s->b.preset : AA_ERROR_NULL; }
static i32 AAudioStream_getAllowedCapturePolicy(aa_stream *s) { return s ? s->b.policy : AA_ERROR_NULL; }
static bool AAudioStream_isPrivacySensitive(aa_stream *s) { return s && s->b.privacy; }
static u32 AAudioStream_getChannelMask(aa_stream *s) { return s && s->b.channels == 1 ? 1u : 3u; }  /* MONO, STEREO */
static i32 AAudioStream_getSpatializationBehavior(aa_stream *s) { return s ? s->b.spatialization : AA_ERROR_NULL; }
static bool AAudioStream_isContentSpatialized(aa_stream *s) { return s && s->b.spatialized; }
static bool AAudioStream_isMMapUsed(aa_stream *s) { (void)s; return false; }
/* Android 14: what the output itself is, which is the mixer's. */
static i32 AAudioStream_getHardwareChannelCount(aa_stream *s) { return s ? 2 : AA_ERROR_NULL; }
static i32 AAudioStream_getHardwareSampleRate(aa_stream *s) { return s ? HK_AUDIO_RATE : AA_ERROR_NULL; }
static i32 AAudioStream_getHardwareFormat(aa_stream *s) { return s ? FMT_I16 : AA_ERROR_NULL; }

static i64 AAudioStream_getFramesWritten(aa_stream *s) { return s ? (i64)(s->written + s->fill) : AA_ERROR_NULL; }
static i64 AAudioStream_getFramesRead(aa_stream *s) { return s && s->voice && !s->released ? (i64)hk_voice_position(s->voice) : 0; }

/* Where playing has got to and when: the frames the mixer has taken, now. Only while started, as on Android. */
static i32 AAudioStream_getTimestamp(aa_stream *s, i32 clock, i64 *frame, i64 *time_ns)
{
    (void)clock;
    if (!s || !frame || !time_ns) return AA_ERROR_NULL;
    if (s->state != ST_STARTED || s->released) return AA_ERROR_INVALID_STATE;
    *frame = (i64)hk_voice_position(s->voice);
    *time_ns = (i64)hk_time_monotonic_ns();
    return AA_OK;
}

static const char *AAudio_convertStreamStateToText(i32 state) { return state_name(state); }

static const char *AAudio_convertResultToText(i32 r)
{
    switch (r) {
    case AA_OK: return "AAUDIO_OK";
    case -899: return "AAUDIO_ERROR_DISCONNECTED";
    case AA_ERROR_ILLEGAL_ARGUMENT: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case -896: return "AAUDIO_ERROR_INTERNAL";
    case AA_ERROR_INVALID_STATE: return "AAUDIO_ERROR_INVALID_STATE";
    case AA_ERROR_INVALID_HANDLE: return "AAUDIO_ERROR_INVALID_HANDLE";
    case AA_ERROR_UNIMPLEMENTED: return "AAUDIO_ERROR_UNIMPLEMENTED";
    case AA_ERROR_UNAVAILABLE: return "AAUDIO_ERROR_UNAVAILABLE";
    case -888: return "AAUDIO_ERROR_NO_FREE_HANDLES";
    case -887: return "AAUDIO_ERROR_NO_MEMORY";
    case AA_ERROR_NULL: return "AAUDIO_ERROR_NULL";
    case AA_ERROR_TIMEOUT: return "AAUDIO_ERROR_TIMEOUT";
    case -884: return "AAUDIO_ERROR_WOULD_BLOCK";
    case AA_ERROR_INVALID_FORMAT: return "AAUDIO_ERROR_INVALID_FORMAT";
    case AA_ERROR_OUT_OF_RANGE: return "AAUDIO_ERROR_OUT_OF_RANGE";
    case -881: return "AAUDIO_ERROR_NO_SERVICE";
    case AA_ERROR_INVALID_RATE: return "AAUDIO_ERROR_INVALID_RATE";
    default: return "Unrecognized AAudio error.";
    }
}

/* Hidden, but looked up by name by some apps: never the memory-mapped path, which there is not. */
static i32 AAudio_getMMapPolicy(void) { return 1; }  /* AAUDIO_POLICY_NEVER */
static i32 AAudio_setMMapPolicy(i32 policy)
{
    (void)policy;
    return AA_OK;
}

/* ------------------------------------------------- written streams for Java */

hk_pcm *hk_pcm_open(int rate, int channels, int format, int buffer_ms)
{
    aa_builder *b = NULL;
    aa_stream *s = NULL;
    AAudio_createStreamBuilder(&b);
    b->rate = rate;
    b->channels = channels;
    b->format = format == 2 ? FMT_FLOAT : FMT_I16;
    open_stream(b, &s, true);
    AAudioStreamBuilder_delete(b);
    if (s) AAudioStream_setBufferSizeInFrames(s, rate * buffer_ms / 1000);
    return s;
}

int hk_pcm_write(hk_pcm *p, const void *frames, int count, bool wait)
{
    if (!p || !frames || count <= 0 || p->closed || p->released) return 0;
    return stream_write(p, frames, count, 0, wait, true);
}

void hk_pcm_play(hk_pcm *p, bool play)
{
    if (!p) return;
    if (play) AAudioStream_requestStart(p);
    else AAudioStream_requestPause(p);
}

void hk_pcm_flush(hk_pcm *p)
{
    if (!p) return;
    hk_lock_acquire(&p->lock);
    if (!p->closed && !p->released) drop_queued(p);
    hk_lock_release(&p->lock);
}

void hk_pcm_volume(hk_pcm *p, float left, float right)
{
    if (p && p->voice && !p->released) hk_voice_set_gains(p->voice, left, right);
}

u64 hk_pcm_position(hk_pcm *p) { return p && p->voice && !p->released ? hk_voice_position(p->voice) : 0; }
int hk_pcm_pending(hk_pcm *p) { return p && !p->released ? (int)(p->waiting_frames + p->fill) : 0; }

void hk_pcm_close(hk_pcm *p)
{
    if (p) AAudioStream_close(p);
}

#define D(fn) { #fn, (void *)(fn) }
static const hk_sym g_tab[] = {
    D(AAudio_createStreamBuilder), D(AAudio_convertResultToText), D(AAudio_convertStreamStateToText), D(AAudio_getMMapPolicy),
    D(AAudio_setMMapPolicy),
    D(AAudioStreamBuilder_setDeviceId), D(AAudioStreamBuilder_setSampleRate), D(AAudioStreamBuilder_setChannelCount),
    D(AAudioStreamBuilder_setSamplesPerFrame), D(AAudioStreamBuilder_setFormat), D(AAudioStreamBuilder_setSharingMode),
    D(AAudioStreamBuilder_setDirection), D(AAudioStreamBuilder_setBufferCapacityInFrames), D(AAudioStreamBuilder_setPerformanceMode),
    D(AAudioStreamBuilder_setUsage), D(AAudioStreamBuilder_setContentType), D(AAudioStreamBuilder_setInputPreset),
    D(AAudioStreamBuilder_setSessionId), D(AAudioStreamBuilder_setAllowedCapturePolicy), D(AAudioStreamBuilder_setFramesPerDataCallback),
    D(AAudioStreamBuilder_setChannelMask), D(AAudioStreamBuilder_setSpatializationBehavior), D(AAudioStreamBuilder_setPrivacySensitive),
    D(AAudioStreamBuilder_setIsContentSpatialized), D(AAudioStreamBuilder_setDataCallback), D(AAudioStreamBuilder_setErrorCallback),
    D(AAudioStreamBuilder_setPackageName), { "AAudioStreamBuilder_setAttributionTag", (void *)AAudioStreamBuilder_setPackageName },
    D(AAudioStreamBuilder_openStream), D(AAudioStreamBuilder_delete),
    D(AAudioStream_release), D(AAudioStream_close), D(AAudioStream_requestStart), D(AAudioStream_requestPause), D(AAudioStream_requestFlush),
    D(AAudioStream_requestStop), D(AAudioStream_getState), D(AAudioStream_waitForStateChange), D(AAudioStream_read), D(AAudioStream_write),
    D(AAudioStream_setBufferSizeInFrames), D(AAudioStream_getBufferSizeInFrames), D(AAudioStream_getFramesPerBurst),
    D(AAudioStream_getBufferCapacityInFrames), D(AAudioStream_getFramesPerDataCallback), D(AAudioStream_getXRunCount),
    D(AAudioStream_getSampleRate), D(AAudioStream_getChannelCount), { "AAudioStream_getSamplesPerFrame", (void *)AAudioStream_getChannelCount },
    D(AAudioStream_getDeviceId), D(AAudioStream_getFormat), D(AAudioStream_getSharingMode), D(AAudioStream_getPerformanceMode),
    D(AAudioStream_getDirection), D(AAudioStream_getFramesWritten), D(AAudioStream_getFramesRead), D(AAudioStream_getSessionId),
    D(AAudioStream_getTimestamp), D(AAudioStream_getUsage), D(AAudioStream_getContentType), D(AAudioStream_getInputPreset),
    D(AAudioStream_getAllowedCapturePolicy), D(AAudioStream_isPrivacySensitive), D(AAudioStream_getChannelMask),
    D(AAudioStream_getSpatializationBehavior), D(AAudioStream_isContentSpatialized), D(AAudioStream_isMMapUsed),
    D(AAudioStream_getHardwareChannelCount), D(AAudioStream_getHardwareSampleRate), D(AAudioStream_getHardwareFormat),
    HK_END
};
#undef D

const hk_sym *hk_aaudio_table(void) { return g_tab; }
