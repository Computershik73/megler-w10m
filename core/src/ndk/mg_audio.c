#include "mg_audio.h"

#include "../mg_core.h"
#include "../host/mg_host.h"
#include "../run/mg_run.h"

const char *megler_data_dir(void);

#define MAX_QUEUE 64

typedef struct queued {
    const u8 *data;
    u32 frames;
} queued;

struct mg_voice {
    int rate, channels, bits;
    bool is_float, playing, dead;
    u32 frame_bytes;
    queued queue[MAX_QUEUE];
    u32 head, count;
    u32 pos;                           /* frame within the buffer being played */
    u32 frac;                          /* 16.16 fraction between that frame and the next */
    u32 step;                          /* source frames per output frame, 16.16 */
    float gain, gain_l, gain_r;
    u32 played;
    u64 position;
    mg_voice_done_fn done;
    void *done_user;
    struct mg_voice *next;
};

static mg_lock g_lock = MG_LOCK_INIT;
static mg_voice *g_voices;
static mg_thread *g_mixer;
static bool g_host_open;

/* One sample of a voice's own format as a float in -1..1. */
static float sample_at(const mg_voice *v, const u8 *frame, int channel)
{
    const u8 *p = frame + (size_t)channel * (size_t)(v->bits / 8);
    if (v->bits == 16) return (float)(i16)mg_rd16(p) / 32768.0f;
    if (v->bits == 8) return ((float)*p - 128.0f) / 128.0f;
    if (v->is_float) return mg_f(mg_rd32(p));
    return (float)(i32)mg_rd32(p) / 2147483648.0f;
}

/* Adds one chunk of a voice to the mix. Returns how many of its buffers ended. Called with the lock held. */
static u32 mix_voice(mg_voice *v, float *mix, int frames)
{
    u32 ended = 0;
    int i;
    for (i = 0; i < frames && v->count; i++) {
        const queued *q = &v->queue[v->head];
        const u8 *frame = q->data + (size_t)v->pos * v->frame_bytes;
        float l = sample_at(v, frame, 0), r = v->channels > 1 ? sample_at(v, frame, 1) : l;
        mix[i * 2] += l * v->gain * v->gain_l;
        mix[i * 2 + 1] += r * v->gain * v->gain_r;
        v->frac += v->step;
        v->pos += v->frac >> 16;
        v->position += v->frac >> 16;
        v->frac &= 0xFFFF;
        while (v->count && v->pos >= v->queue[v->head].frames) {
            v->pos -= v->queue[v->head].frames;
            v->head = (v->head + 1) % MAX_QUEUE;
            v->count--;
            v->played++;
            ended++;
        }
        if (!v->count) v->pos = v->frac = 0;
    }
    return ended;
}

/*
 * The flag "record-audio": what the mixer puts out, from the first sound on
 * and for half a minute, written to megler-audio.wav in the data folder. With
 * "record-film" instead, the half minute starts as a film's sound does. Nobody
 * at the PC hears a phone; a file can be fetched and looked at (is it music
 * or noise, is it clipped, are there gaps).
 */
enum { RECORD_SECONDS = 30 };
static i16 *g_record;
static u32 g_record_frames;
static int g_recording = -1;           /* -1 not asked yet, 0 no (or done), 1 waiting for sound or recording, 2 waiting for a film */
static bool g_record_silence;          /* a film's half minute starts at once, quiet or not */

static void record_asked(void)
{
    if (g_recording < 0) g_recording = mg_flag("record-audio") ? 1 : mg_flag("record-film") ? 2 : 0;
}

void mg_audio_film_begins(void)
{
    record_asked();
    if (g_recording == 2) {
        g_record_silence = true;
        g_recording = 1;
    }
}

static void record(const i16 *out, u32 frames)
{
    const u32 want = RECORD_SECONDS * MG_AUDIO_RATE;
    u32 i, n;
    bool loud = false;
    record_asked();
    if (g_recording != 1) return;
    for (i = 0; i < frames * 2 && !loud; i++) loud = out[i] > 64 || out[i] < -64;
    if (!g_record_frames && !loud && !g_record_silence) return;
    if (!g_record) g_record = (i16 *)mg_alloc((size_t)want * 4);
    n = frames < want - g_record_frames ? frames : want - g_record_frames;
    memcpy(g_record + (size_t)g_record_frames * 2, out, (size_t)n * 4);
    g_record_frames += n;
    if (g_record_frames == want) {
        const char *dir = megler_data_dir();
        char *path = mg_sprintf("%s/megler-audio.wav", dir && dir[0] ? dir : ".");
        u32 bytes = want * 4;
        u8 *wav = (u8 *)mg_alloc(44 + (size_t)bytes);
        static const u8 head[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                                     0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
        u32 v;
        memcpy(wav, head, 44);
        v = 36 + bytes;
        memcpy(wav + 4, &v, 4);
        v = MG_AUDIO_RATE;
        memcpy(wav + 24, &v, 4);
        v = MG_AUDIO_RATE * 4;
        memcpy(wav + 28, &v, 4);
        memcpy(wav + 40, &bytes, 4);
        memcpy(wav + 44, g_record, bytes);
        mg_info("audio: %d seconds of the mixer's output %s %s", RECORD_SECONDS, mg_file_write_all(path, wav, 44 + (size_t)bytes) ? "written to" : "could NOT be written to",
                path);
        free(wav);
        free(path);
        free(g_record);
        g_record = NULL;
        g_recording = 0;
    }
}

static MG_HOSTCB void mixer_thread(void *arg)
{
    static float mix[MG_AUDIO_CHUNK * 2];
    static i16 out[MG_AUDIO_CHUNK * 2];
    u64 next = mg_time_monotonic_ns();
    (void)arg;
    for (;;) {
        const megler_run_config *run = mg_run_config();
        struct { mg_voice *v; u32 ended; mg_voice_done_fn fn; void *user; } done[32];
        int ndone = 0, i;
        mg_voice *v, **link;
        memset(mix, 0, sizeof(mix));
        mg_lock_acquire(&g_lock);
        for (link = &g_voices; (v = *link) != NULL;) {
            if (v->dead) {
                *link = v->next;
                free(v);
                continue;
            }
            if (v->playing && v->count) {
                u32 ended = mix_voice(v, mix, MG_AUDIO_CHUNK);
                if (ended && v->done && ndone < (int)MG_COUNT(done)) {
                    done[ndone].v = v;
                    done[ndone].ended = ended;
                    done[ndone].fn = v->done;
                    done[ndone].user = v->done_user;
                    ndone++;
                }
            }
            link = &v->next;
        }
        mg_lock_release(&g_lock);
        /* The app refills its queue from here, as it does from Android's audio thread. */
        for (i = 0; i < ndone; i++) {
            u32 k;
            for (k = 0; k < done[i].ended; k++) done[i].fn(done[i].v, done[i].user);
        }
        if (g_host_open && run->audio.write) {
            for (i = 0; i < MG_AUDIO_CHUNK * 2; i++) {
                float s = mix[i] * 32767.0f;
                out[i] = (i16)(s > 32767.0f ? 32767 : s < -32768.0f ? -32768 : (i32)s);
            }
            record(out, MG_AUDIO_CHUNK);
            /* Silence is written too: the output paces the mixer, and write returns when there is room for more. */
            run->audio.write(run->audio.user, out, MG_AUDIO_CHUNK);
        } else {
            u64 now;
            next += 10000000ull;
            now = mg_time_monotonic_ns();
            if (next > now) mg_sleep_ns(next - now);
            else if (now - next > 200000000ull) next = now;
        }
    }
}

/*
 * The mixer's thread, started with the first voice, and the run's output, opened then. An output the run names later
 * (another run in the same process: the tests) is opened when the next voice is made; the one tried is not tried again.
 */
static bool g_open_asked;
static bool (MEGLER_CB *g_open_tried)(void *user, int rate);

static void ensure_mixer(void)
{
    const megler_run_config *run = mg_run_config();
    if (g_mixer && g_open_asked && run->audio.open == g_open_tried) return;
    mg_lock_acquire(&g_lock);
    if (!g_open_asked || run->audio.open != g_open_tried) {
        g_open_asked = true;
        g_open_tried = run->audio.open;
        g_host_open = run->audio.open && run->audio.open(run->audio.user, MG_AUDIO_RATE);
        if (!g_host_open) mg_warn("audio: no output; the app's sound is consumed silently");
    }
    /*
     * The apps' own code runs on this thread: OpenSL ES buffer queues' callbacks and AAudio's data callbacks, where a
     * game mixes its sound. Android has them on a thread with pthread's default stack, a megabyte, and a game's frames
     * there are what it likes: Smash Hit's QiAudio::fillBuffer and QiAudioChannel::fillBuffer take 128 KB each, the
     * second inside the first once a channel plays. On the 256 KB this thread had, the second's first store was below
     * the stack -- a fault Windows has no stack to dispatch, so the process ended with nothing said, when the game's
     * first channel played (0.1.0.690: closed after the splash screen).
     */
    if (!g_mixer) g_mixer = mg_thread_start(mixer_thread, NULL, ((size_t)1 << 20) + MG_STACK_FAULT_ROOM, "audio-mixer");
    mg_lock_release(&g_lock);
}

mg_voice *mg_voice_create(int rate, int channels, int bits, bool is_float)
{
    mg_voice *v;
    if (rate < 4000 || rate > 192000 || channels < 1 || channels > 8 || (bits != 8 && bits != 16 && bits != 32)) return NULL;
    v = (mg_voice *)mg_alloc(sizeof(*v));
    v->rate = rate;
    v->channels = channels;
    v->bits = bits;
    v->is_float = is_float;
    v->frame_bytes = (u32)(channels * bits / 8);
    v->step = (u32)(((u64)rate << 16) / MG_AUDIO_RATE);
    v->gain = v->gain_l = v->gain_r = 1.0f;
    ensure_mixer();
    mg_lock_acquire(&g_lock);
    v->next = g_voices;
    g_voices = v;
    mg_lock_release(&g_lock);
    return v;
}

void mg_voice_destroy(mg_voice *v)
{
    if (!v) return;
    mg_lock_acquire(&g_lock);
    v->playing = false;
    v->done = NULL;
    v->dead = true;                    /* the mixer frees it: it may be in the middle of this voice's callbacks */
    mg_lock_release(&g_lock);
}

bool mg_voice_enqueue(mg_voice *v, const void *data, u32 bytes)
{
    bool ok = false;
    mg_lock_acquire(&g_lock);
    if (v->count < MAX_QUEUE && bytes >= v->frame_bytes) {
        queued *q = &v->queue[(v->head + v->count) % MAX_QUEUE];
        q->data = (const u8 *)data;
        q->frames = bytes / v->frame_bytes;
        v->count++;
        ok = true;
    }
    mg_lock_release(&g_lock);
    return ok;
}

void mg_voice_clear(mg_voice *v)
{
    mg_lock_acquire(&g_lock);
    v->head = v->count = v->pos = v->frac = 0;
    v->played = 0;
    mg_lock_release(&g_lock);
}

void mg_voice_set_playing(mg_voice *v, bool playing)
{
    mg_lock_acquire(&g_lock);
    v->playing = playing;
    mg_lock_release(&g_lock);
}

void mg_voice_set_gain(mg_voice *v, float gain) { v->gain = gain < 0 ? 0 : gain; }

void mg_voice_set_gains(mg_voice *v, float left, float right)
{
    v->gain_l = left < 0 ? 0 : left;
    v->gain_r = right < 0 ? 0 : right;
}

void mg_voice_set_rate(mg_voice *v, int rate)
{
    if (rate < 1000 || rate > 400000) return;
    mg_lock_acquire(&g_lock);
    v->rate = rate;
    v->step = (u32)(((u64)rate << 16) / MG_AUDIO_RATE);
    mg_lock_release(&g_lock);
}

void mg_voice_set_done(mg_voice *v, mg_voice_done_fn fn, void *user)
{
    mg_lock_acquire(&g_lock);
    v->done = fn;
    v->done_user = user;
    mg_lock_release(&g_lock);
}

u32 mg_voice_queued(mg_voice *v) { return v->count; }
u32 mg_voice_played(mg_voice *v) { return v->played; }
u64 mg_voice_position(mg_voice *v) { return v->position; }
