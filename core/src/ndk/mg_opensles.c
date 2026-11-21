/*
 * libOpenSLES.so: PCM buffer-queue players over the mixer (mg_audio.c), and
 * PCM buffer-queue recorders over the microphone (mg_mic.c).
 *
 * That is the part of OpenSL ES games use: an engine, an output mix, and audio
 * players fed from the app's own buffers through a buffer queue, each with a
 * callback that asks for the next buffer. FMOD, OpenAL Soft, SDL and the
 * engines' own mixers all sit on exactly this. Players of a file (an Android
 * file descriptor or a file:// URI: cocos2d-x's music) are decoded here and
 * fed to a voice the same way; a URL's are refused.
 *
 * And the part a call uses: an audio recorder that fills the app's buffers
 * with what the microphone hears and hands each back through its queue's
 * callback, as WebRTC's opensles_recorder.cc and the messengers' own voice
 * engines have it (imo Lite's calls ended at CreateAudioRecorder, 0.1.0.699).
 *
 * An OpenSL ES object is reached through interfaces; an interface is a
 * pointer to a pointer to a table of functions, each taking the interface
 * itself as its first argument.
 */
#include <math.h>

#include "../bionic/mg_bionic.h"
#include "../host/mg_host.h"
#include "../run/mg_run.h"
#include "mg_audio.h"
#include "mg_mic.h"

typedef struct sl_iid {
    u32 time_low;
    u16 time_mid, time_hi_and_version, clock_seq;
    u8 node[6];
} sl_iid;

#define SL_NAMES(X)                                                                                                             \
    X(NULL) X(OBJECT) X(AUDIOIODEVICECAPABILITIES) X(LED) X(VIBRA) X(METADATAEXTRACTION) X(METADATATRAVERSAL) X(DYNAMICSOURCE)   \
    X(OUTPUTMIX) X(PLAY) X(PREFETCHSTATUS) X(PLAYBACKRATE) X(SEEK) X(RECORD) X(EQUALIZER) X(VOLUME) X(DEVICEVOLUME)              \
    X(BUFFERQUEUE) X(PRESETREVERB) X(ENVIRONMENTALREVERB) X(EFFECTSEND) X(3DGROUPING) X(3DCOMMIT) X(3DLOCATION) X(3DDOPPLER)      \
    X(3DSOURCE) X(3DMACROSCOPIC) X(MUTESOLO) X(DYNAMICINTERFACEMANAGEMENT) X(MIDIMESSAGE) X(MIDIMUTESOLO) X(MIDITEMPO)           \
    X(MIDITIME) X(AUDIODECODERCAPABILITIES) X(AUDIOENCODERCAPABILITIES) X(AUDIOENCODER) X(BASSBOOST) X(PITCH) X(RATEPITCH)        \
    X(VIRTUALIZER) X(VISUALIZATION) X(ENGINE) X(ENGINECAPABILITIES) X(THREADSYNC) X(ANDROIDEFFECT) X(ANDROIDEFFECTSEND)          \
    X(ANDROIDEFFECTCAPABILITIES) X(ANDROIDCONFIGURATION) X(ANDROIDSIMPLEBUFFERQUEUE) X(ANDROIDBUFFERQUEUESOURCE)                 \
    X(ANDROIDACOUSTICECHOCANCELLATION) X(ANDROIDAUTOMATICGAINCONTROL) X(ANDROIDNOISESUPPRESSION)

enum {
#define X(n) IID_##n,
    SL_NAMES(X)
#undef X
    IID_COUNT
};

static sl_iid g_iids[IID_COUNT];
/* What the guest's SL_IID_ENGINE and friends are: pointers to the ID objects. */
static const sl_iid *g_iid_ptrs[IID_COUNT];

#define SL_OK 0u
#define SL_PRECONDITIONS_VIOLATED 1u
#define SL_PARAMETER_INVALID 2u
#define SL_RESOURCE_ERROR 4u
#define SL_BUFFER_INSUFFICIENT 7u
#define SL_CONTENT_UNSUPPORTED 9u
#define SL_FEATURE_UNSUPPORTED 12u

enum { OBJ_ENGINE = 1, OBJ_MIX, OBJ_PLAYER, OBJ_RECORDER };

struct sl_object;
struct sl_recorder;
typedef struct sl_itf {
    const void *const *table;
    struct sl_object *obj;
} sl_itf;

typedef struct sl_object {
    int kind;
    sl_itf object, engine, play, queue, volume, config, record;
    mg_voice *voice;
    int rate;
    u32 play_state;
    mg_addr queue_cb;
    void *queue_ctx;
    i32 volume_mb;
    bool mute;
    struct sl_recorder *rec;           /* a recorder's own (OBJ_RECORDER) */
    /*
     * A player of a file (an Android file descriptor or a URI, of MP3, Ogg, WAV...): decoded here (mg_audio_dec_*) into
     * FILE_BUFS buffers of PCM that go round the voice's queue, each filled again as it is played out -- by a thread of the
     * player's own, ahead of the mixer: an MP3 decoded on the mixer's thread (the shell's decoder) made it late, and Kingdom
     * Rush Vengeance's music crackled. Under `file_lock`: the decoder, which that thread reads and Destroy closes.
     */
    sl_itf seek, prefetch;
    void *dec;
    mg_lock file_lock;
    mg_cond file_cond;                 /* a buffer has been played out, the state changed, or the player has gone */
    bool file_dead;
    i16 *file_buf[8];
    int file_channels, file_next;
    u32 file_ms;
    bool file_loop, file_ended, file_primed;
    mg_addr play_cb, prefetch_cb;
    void *play_ctx, *prefetch_ctx;
    u32 play_mask;
    bool prefetch_told;
} sl_object;

#define FILE_BUFS 8
#define FILE_FRAMES 4096

/* Defined after the tables it fills in. */
static sl_object *new_object(int kind);

/*
 * What an app asks of OpenSL ES, said for the first calls it makes: when a
 * sound library gives up on its output ("error initializing output device"
 * is all FMOD says), the call it gave up after is the last line here.
 */
static volatile i32 g_said;
#define SL_SAY(...)                                           \
    do {                                                      \
        if (mg_atomic_add(&g_said, 1) <= 120) mg_info("opensl: " __VA_ARGS__); \
    } while (0)
/* A recorder's calls have a count of their own: a call's log has them whatever the app's players said before it. */
static volatile i32 g_rec_said;
#define REC_SAY(...)                                          \
    do {                                                      \
        if (mg_atomic_add(&g_rec_said, 1) <= 120) mg_info("opensl: recorder: " __VA_ARGS__); \
    } while (0)

static const char *iid_name(const void *iid);

static u32 ok0(void) { return SL_OK; }
static u32 unsupported(void)
{
    SL_SAY("a function this layer does not have was called; the app is told it is not supported");
    return SL_FEATURE_UNSUPPORTED;
}

/* ----------------------------------------------------------------- object */

static u32 obj_GetState(sl_itf *self, u32 *state)
{
    (void)self;
    if (state) *state = 2;             /* SL_OBJECT_STATE_REALIZED */
    return SL_OK;
}

static u32 obj_GetInterface(sl_itf *self, const sl_iid *iid, void **out)
{
    sl_object *o = self->obj;
    sl_itf *itf = NULL;
    if (!out) return SL_PARAMETER_INVALID;
    if (iid == g_iid_ptrs[IID_OBJECT]) itf = &o->object;
    else if (iid == g_iid_ptrs[IID_ENGINE] && o->kind == OBJ_ENGINE) itf = &o->engine;
    else if (o->kind == OBJ_PLAYER) {
        if (iid == g_iid_ptrs[IID_PLAY]) itf = &o->play;
        else if (iid == g_iid_ptrs[IID_BUFFERQUEUE] || iid == g_iid_ptrs[IID_ANDROIDSIMPLEBUFFERQUEUE]) itf = &o->queue;
        else if (iid == g_iid_ptrs[IID_VOLUME]) itf = &o->volume;
        else if (iid == g_iid_ptrs[IID_ANDROIDCONFIGURATION]) itf = &o->config;
        else if (o->dec && iid == g_iid_ptrs[IID_SEEK]) itf = &o->seek;
        else if (o->dec && iid == g_iid_ptrs[IID_PREFETCHSTATUS]) itf = &o->prefetch;
    } else if (o->kind == OBJ_RECORDER) {
        /* Before Realize as after it: WebRTC sets the recording preset between CreateAudioRecorder and Realize. */
        if (iid == g_iid_ptrs[IID_RECORD]) itf = &o->record;
        else if (iid == g_iid_ptrs[IID_BUFFERQUEUE] || iid == g_iid_ptrs[IID_ANDROIDSIMPLEBUFFERQUEUE]) itf = &o->queue;
        else if (iid == g_iid_ptrs[IID_ANDROIDCONFIGURATION]) itf = &o->config;
    }
    *out = itf;
    if (o->kind == OBJ_RECORDER) REC_SAY("GetInterface(%s) -> %s", iid_name(iid), itf ? "ok" : "not supported");
    else SL_SAY("GetInterface(%s) on %s -> %s", iid_name(iid), o->kind == OBJ_ENGINE ? "the engine" : o->kind == OBJ_MIX ? "the output mix" : "a player",
                itf ? "ok" : "not supported");
    return itf ? SL_OK : SL_FEATURE_UNSUPPORTED;
}

static u32 obj_Realize(sl_itf *self, u32 async)
{
    (void)async;
    if (self->obj->kind == OBJ_RECORDER) REC_SAY("Realize");
    return SL_OK;
}

static void rec_destroy(sl_object *o);

static void obj_Destroy(sl_itf *self)
{
    sl_object *o = self->obj;
    if (o->kind == OBJ_RECORDER) rec_destroy(o);
    if (o->dec) {
        mg_lock_acquire(&o->file_lock);
        o->file_dead = true;
        mg_audio_dec_close(o->dec);
        o->dec = NULL;
        mg_cond_broadcast(&o->file_cond);
        mg_lock_release(&o->file_lock);
    }
    if (o->voice) mg_voice_destroy(o->voice);
    /* The object itself stays: the mixer may be inside this player's callback, which holds its queue interface. */
    o->voice = NULL;
}

static const void *const g_object_table[] = {
    (void *)obj_Realize,
    (void *)ok0,                /* Resume */
    (void *)obj_GetState,
    (void *)obj_GetInterface,
    (void *)ok0,                /* RegisterCallback */
    (void *)ok0,                /* AbortAsyncOperation */
    (void *)obj_Destroy,
    (void *)ok0,                /* SetPriority */
    (void *)ok0,                /* GetPriority */
    (void *)ok0,                /* SetLossOfControlInterfaces */
};

/* ----------------------------------------------------------------- engine */

/* SLDataSource: where the sound comes from and its format, both SLuint32 structures of their own. */
typedef struct g_data_source {
    const void *locator, *format;
} g_data_source;

/* With file_lock held: the next of a file player's buffers decoded and queued; false at the end (with no loop). */
static bool file_fill(sl_object *o)
{
    i16 *b = o->file_buf[o->file_next];
    int got = 0, n;
    if (!o->dec || !o->voice || o->file_ended) return false;
    while (got < FILE_FRAMES) {
        n = mg_audio_dec_read(o->dec, b + (size_t)got * (size_t)o->file_channels, FILE_FRAMES - got);
        if (n > 0) {
            got += n;
            continue;
        }
        if (!o->file_loop) break;
        mg_audio_dec_seek(o->dec, 0);
        n = mg_audio_dec_read(o->dec, b + (size_t)got * (size_t)o->file_channels, FILE_FRAMES - got);
        if (n <= 0) break;
        got += n;
    }
    if (got <= 0) {
        o->file_ended = true;
        return false;
    }
    if (!mg_voice_enqueue(o->voice, b, (u32)got * (u32)o->file_channels * 2u)) return false;
    o->file_next = (o->file_next + 1) % FILE_BUFS;
    return true;
}

/* A file player started (again): its buffers filled, from where its decoder is. */
static void file_prime(sl_object *o)
{
    int i;
    mg_lock_acquire(&o->file_lock);
    if (!o->file_primed && o->voice && mg_voice_queued(o->voice) == 0) {
        /* (Two here, at once; the decoding thread the rest.) */
        for (i = 0; i < 2; i++)
            if (!file_fill(o)) break;
        o->file_primed = true;
    }
    mg_cond_broadcast(&o->file_cond);
    mg_lock_release(&o->file_lock);
}

/* A file player's decoding thread: its queue kept full while it is not stopped, until it is destroyed. */
static MG_HOSTCB void file_thread(void *arg)
{
    sl_object *o = (sl_object *)arg;
    mg_lock_acquire(&o->file_lock);
    while (!o->file_dead && o->dec) {
        if (o->voice && o->play_state != 1 && o->file_primed && !o->file_ended && mg_voice_queued(o->voice) < FILE_BUFS) {
            file_fill(o);
            continue;
        }
        mg_cond_wait_ns(&o->file_cond, &o->file_lock, 100000000ull);
    }
    mg_lock_release(&o->file_lock);
}

static void voice_done(mg_voice *v, void *user)
{
    sl_object *o = (sl_object *)user;
    if (o->dec) {
        bool at_end;
        mg_lock_acquire(&o->file_lock);
        at_end = o->file_ended && mg_voice_queued(v) == 0;
        mg_cond_broadcast(&o->file_cond);
        mg_lock_release(&o->file_lock);
        /* SL_PLAYEVENT_HEADATEND, when the app asked for it: cocos2d-x's UrlAudioPlayer ends the sound there. */
        if (at_end && o->play_cb && (o->play_mask & 1u)) MG_GFN(void (*)(sl_itf *, void *, u32), o->play_cb)(&o->play, o->play_ctx, 1u);
        return;
    }
    if (o->queue_cb) MG_GFN(void (*)(sl_itf *, void *), o->queue_cb)(&o->queue, o->queue_ctx);
}

/* A file player's prefetch is done at once (the file is read whole): its callback told so, once, when it has one. */
static void prefetch_tell(sl_object *o)
{
    if (o->prefetch_told || !o->prefetch_cb) return;
    o->prefetch_told = true;
    /* SL_PREFETCHEVENT_STATUSCHANGE | SL_PREFETCHEVENT_FILLLEVELCHANGE */
    MG_GFN(void (*)(sl_itf *, void *, u32), o->prefetch_cb)(&o->prefetch, o->prefetch_ctx, 3u);
}

/*
 * The data locators of a file, as the app's code lays them out (OpenSLES.h, OpenSLES_Android.h): the URI's pointer where its
 * ABI puts a pointer after a 32-bit field -- at 8 on a 64-bit one, not at 4 -- and the descriptor's 64-bit fields at 8 and 16.
 */
typedef struct sl_locator_fd {
    u32 type;                          /* SL_DATALOCATOR_ANDROIDFD */
    i32 fd;
    i64 offset, length;                /* length all ones: the file's size */
} sl_locator_fd;
typedef struct sl_locator_uri {
    u32 type;                          /* SL_DATALOCATOR_URI */
    const char *uri;
} sl_locator_uri;

/* The file a player's data locator names: SL_DATALOCATOR_ANDROIDFD (fd, offset, length) or SL_DATALOCATOR_URI. */
static void *file_decoder(const u32 *locator)
{
    if (locator[0] == 0x800007BCu) {
        const sl_locator_fd *l = (const sl_locator_fd *)locator;
        return mg_audio_dec_open_fd(l->fd, l->offset, l->length < 0 ? -1 : l->length);
    }
    if (locator[0] == 1u) {
        /* A file's, as a path or a file:// URI. */
        const char *uri = ((const sl_locator_uri *)locator)->uri;
        if (!uri) return NULL;
        if (!strncmp(uri, "file://", 7)) uri += 7;
        if (uri[0] != '/') return NULL;
        return mg_audio_dec_open_path(uri);
    }
    return NULL;
}

static u32 eng_CreateAudioPlayer(sl_itf *self, sl_itf **out, const g_data_source *src, const void *sink, u32 n, const void *ids, const void *req)
{
    const u32 *locator, *format;
    sl_object *o;
    bool is_float;
    (void)self; (void)sink; (void)n; (void)ids; (void)req;
    if (!out || !src || !src->locator || !src->format) return SL_PARAMETER_INVALID;
    *out = NULL;
    locator = (const u32 *)src->locator;
    format = (const u32 *)src->format;
    SL_SAY("CreateAudioPlayer: locator 0x%x, format type %u, %u channels, %u mHz, %u bits in %u, channel mask 0x%x, %u interfaces asked for",
           (unsigned)locator[0], (unsigned)format[0], (unsigned)format[1], (unsigned)format[2], (unsigned)format[3], (unsigned)format[4],
           (unsigned)format[5], (unsigned)n);
    {
        u32 i;
        const sl_iid *const *wanted = (const sl_iid *const *)ids;
        for (i = 0; wanted && i < n && i < 8; i++) SL_SAY("  interface %u: %s%s", (unsigned)i, iid_name(wanted[i]), req && ((const u32 *)req)[i] ? " (required)" : "");
    }
    /* SL_DATALOCATOR_BUFFERQUEUE, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE; SL_DATAFORMAT_PCM, SL_ANDROID_DATAFORMAT_PCM_EX */
    if ((locator[0] == 0x800007BCu || locator[0] == 1u) && format[0] == 1u) {
        /* SL_DATAFORMAT_MIME: the file says what it is. */
        void *dec = file_decoder(locator);
        int rate = 0, channels = 0, i;
        if (!dec) {
            mg_note_once("audio: a player of a file was asked for whose sound cannot be read or decoded here");
            return SL_CONTENT_UNSUPPORTED;
        }
        o = new_object(OBJ_PLAYER);
        o->dec = dec;
        mg_audio_dec_info(dec, &rate, &channels, &o->file_ms);
        o->rate = rate;
        o->file_channels = channels > 1 ? 2 : 1;
        o->voice = rate > 0 ? mg_voice_create(rate, o->file_channels, 16, false) : NULL;
        if (!o->voice) {
            mg_audio_dec_close(dec);
            free(o);
            return SL_CONTENT_UNSUPPORTED;
        }
        for (i = 0; i < FILE_BUFS; i++) o->file_buf[i] = (i16 *)mg_alloc((size_t)FILE_FRAMES * (size_t)o->file_channels * 2);
        mg_voice_set_done(o->voice, voice_done, o);
        {
            mg_thread *t = mg_thread_start(file_thread, o, ((size_t)256 << 10) + MG_STACK_FAULT_ROOM, "audio-file");
            if (t) mg_thread_detach(t);
            else mg_warn("audio: no thread to decode a file player's sound: it is decoded as it is played");
        }
        mg_info("audio: a player of a file, decoded here: %d Hz, %d channels, %u ms", rate, o->file_channels, (unsigned)o->file_ms);
        *out = &o->object;
        return SL_OK;
    }
    if ((locator[0] != 6 && locator[0] != 0x800007BDu) || (format[0] != 2 && format[0] != 4)) {
        mg_note_once("audio: a player of a URL or a format other than PCM was asked for; only PCM buffer queues and files are played");
        return SL_CONTENT_UNSUPPORTED;
    }
    /* SLDataFormat_PCM: formatType, numChannels, samplesPerSec (in millihertz), bitsPerSample, containerSize, channelMask, endianness[, representation] */
    is_float = format[0] == 4 && format[7] == 3;
    o = new_object(OBJ_PLAYER);
    o->rate = (int)(format[2] / 1000);
    o->voice = mg_voice_create(o->rate, (int)format[1], (int)format[4], is_float);
    if (!o->voice) {
        mg_note_once("audio: a PCM format that cannot be mixed (%u Hz, %u channels, %u bits)", format[2] / 1000, format[1], format[4]);
        free(o);
        return SL_CONTENT_UNSUPPORTED;
    }
    mg_voice_set_done(o->voice, voice_done, o);
    mg_info("audio: player %d Hz, %u channels, %u bits%s", o->rate, format[1], format[4], is_float ? " float" : "");
    *out = &o->object;
    return SL_OK;
}

/* Defined with the recorder, below. */
static u32 eng_CreateAudioRecorder(sl_itf *self, sl_itf **out, const g_data_source *src, const g_data_source *sink, u32 n, const void *ids,
                                   const void *req);

static u32 eng_CreateOutputMix(sl_itf *self, sl_itf **out, u32 n, const void *ids, const void *req)
{
    (void)self; (void)n; (void)ids; (void)req;
    if (!out) return SL_PARAMETER_INVALID;
    *out = &new_object(OBJ_MIX)->object;
    SL_SAY("CreateOutputMix (%u interfaces asked for)", (unsigned)n);
    return SL_OK;
}

static u32 eng_QueryNum(sl_itf *self, u32 id, u32 *count)
{
    (void)self; (void)id;
    if (count) *count = 0;
    return SL_OK;
}

/* QueryNumSupportedExtensions has no object id: the count is the second argument. */
static u32 eng_QueryNumExtensions(sl_itf *self, u32 *count)
{
    (void)self;
    if (count) *count = 0;
    return SL_OK;
}

static const void *const g_engine_table[] = {
    (void *)unsupported,        /* CreateLEDDevice */
    (void *)unsupported,        /* CreateVibraDevice */
    (void *)eng_CreateAudioPlayer,
    (void *)eng_CreateAudioRecorder,
    (void *)unsupported,        /* CreateMidiPlayer */
    (void *)unsupported,        /* CreateListener */
    (void *)unsupported,        /* Create3DGroup */
    (void *)eng_CreateOutputMix,
    (void *)unsupported,        /* CreateMetadataExtractor */
    (void *)unsupported,        /* CreateExtensionObject */
    (void *)eng_QueryNum,       /* QueryNumSupportedInterfaces */
    (void *)unsupported,        /* QuerySupportedInterfaces */
    (void *)eng_QueryNumExtensions,
    (void *)unsupported,        /* QuerySupportedExtension */
    (void *)unsupported,        /* IsExtensionSupported */
};

/* ------------------------------------------------------------------- play */

static u32 play_SetPlayState(sl_itf *self, u32 state)
{
    sl_object *o = self->obj;
    if (state < 1 || state > 3) return SL_PARAMETER_INVALID;
    SL_SAY("SetPlayState(%s)", state == 3 ? "playing" : state == 2 ? "paused" : "stopped");
    o->play_state = state;
    if (o->dec && o->voice) {
        /* A file: stopping goes back to its start; paused or playing, its buffers are filled (a prefetch, at once). */
        if (state == 1) {
            mg_voice_set_playing(o->voice, false);
            mg_voice_clear(o->voice);
            mg_lock_acquire(&o->file_lock);
            if (o->dec) mg_audio_dec_seek(o->dec, 0);
            o->file_ended = o->file_primed = false;
            mg_lock_release(&o->file_lock);
            return SL_OK;
        }
        file_prime(o);
        prefetch_tell(o);
        mg_voice_set_playing(o->voice, state == 3);
        return SL_OK;
    }
    if (o->voice) {
        mg_voice_set_playing(o->voice, state == 3);       /* SL_PLAYSTATE_PLAYING */
        if (state == 1) mg_voice_clear(o->voice);         /* stopping empties the queue, as Android's does */
    }
    return SL_OK;
}

static u32 play_GetPlayState(sl_itf *self, u32 *state)
{
    if (state) *state = self->obj->play_state;
    return SL_OK;
}

static u32 play_GetDuration(sl_itf *self, u32 *ms)
{
    sl_object *o = self->obj;
    if (ms) *ms = o->dec && o->file_ms ? o->file_ms : 0xFFFFFFFFu;         /* else SL_TIME_UNKNOWN */
    return SL_OK;
}

static u32 play_RegisterCallback(sl_itf *self, mg_addr callback, void *context)
{
    self->obj->play_cb = callback;
    self->obj->play_ctx = context;
    return SL_OK;
}

static u32 play_SetEventsMask(sl_itf *self, u32 mask)
{
    self->obj->play_mask = mask;
    return SL_OK;
}

static u32 play_GetEventsMask(sl_itf *self, u32 *mask)
{
    if (mask) *mask = self->obj->play_mask;
    return SL_OK;
}

static u32 play_GetPosition(sl_itf *self, u32 *ms)
{
    sl_object *o = self->obj;
    if (ms) *ms = o->voice && o->rate ? (u32)(mg_voice_position(o->voice) * 1000ull / (u64)o->rate) : 0;
    return SL_OK;
}

static u32 play_GetU32(sl_itf *self, u32 *out)
{
    (void)self;
    if (out) *out = 0;
    return SL_OK;
}

static const void *const g_play_table[] = {
    (void *)play_SetPlayState,
    (void *)play_GetPlayState,
    (void *)play_GetDuration,
    (void *)play_GetPosition,
    (void *)play_RegisterCallback,
    (void *)play_SetEventsMask,
    (void *)play_GetEventsMask,
    (void *)ok0,                /* SetMarkerPosition */
    (void *)ok0,                /* ClearMarkerPosition */
    (void *)play_GetU32,        /* GetMarkerPosition */
    (void *)ok0,                /* SetPositionUpdatePeriod */
    (void *)play_GetU32,        /* GetPositionUpdatePeriod */
};

/* ------------------------------------------------------- the buffer queue */

static u32 queue_Enqueue(sl_itf *self, const void *buffer, u32 size)
{
    sl_object *o = self->obj;
    if (!buffer || !o->voice) return SL_PARAMETER_INVALID;
    return mg_voice_enqueue(o->voice, buffer, size) ? SL_OK : SL_BUFFER_INSUFFICIENT;
}

static u32 queue_Clear(sl_itf *self)
{
    if (self->obj->voice) mg_voice_clear(self->obj->voice);
    return SL_OK;
}

typedef struct g_queue_state {
    u32 count, index;
} g_queue_state;

static u32 queue_GetState(sl_itf *self, g_queue_state *state)
{
    sl_object *o = self->obj;
    if (!state) return SL_PARAMETER_INVALID;
    state->count = o->voice ? mg_voice_queued(o->voice) : 0;
    state->index = o->voice ? mg_voice_played(o->voice) : 0;
    return SL_OK;
}

static u32 queue_RegisterCallback(sl_itf *self, mg_addr callback, void *context)
{
    self->obj->queue_cb = callback;
    self->obj->queue_ctx = context;
    SL_SAY("buffer queue: callback registered");
    return SL_OK;
}

static const void *const g_queue_table[] = {
    (void *)queue_Enqueue,
    (void *)queue_Clear,
    (void *)queue_GetState,
    (void *)queue_RegisterCallback,
};

/* ------------------------------------------------- seek and prefetch, of a file player */

/* SetPosition(SLmillisecond pos, SLuint32 seekMode) */
static u32 seek_SetPosition(sl_itf *self, u32 ms, u32 mode)
{
    sl_object *o = self->obj;
    bool playing = o->play_state == 3;
    (void)mode;
    if (!o->dec || !o->voice) return SL_OK;
    mg_voice_set_playing(o->voice, false);
    mg_voice_clear(o->voice);
    mg_lock_acquire(&o->file_lock);
    if (o->dec) mg_audio_dec_seek(o->dec, (i64)ms * o->rate / 1000);
    o->file_ended = o->file_primed = false;
    mg_lock_release(&o->file_lock);
    if (o->play_state != 1) file_prime(o);
    mg_voice_set_playing(o->voice, playing);
    return SL_OK;
}

/* SetLoop(SLboolean loopEnable, SLmillisecond startPos, SLmillisecond endPos): the whole file, whatever is asked. */
static u32 seek_SetLoop(sl_itf *self, u32 loop, u32 start, u32 end)
{
    (void)start; (void)end;
    mg_lock_acquire(&self->obj->file_lock);
    self->obj->file_loop = loop != 0;
    mg_lock_release(&self->obj->file_lock);
    return SL_OK;
}

static u32 seek_GetLoop(sl_itf *self, u32 *loop, u32 *start, u32 *end)
{
    if (loop) *loop = self->obj->file_loop;
    if (start) *start = 0;
    if (end) *end = 0xFFFFFFFFu;
    return SL_OK;
}

static const void *const g_seek_table[] = {
    (void *)seek_SetPosition,
    (void *)seek_SetLoop,
    (void *)seek_GetLoop,
};

static u32 pre_GetStatus(sl_itf *self, u32 *status)
{
    (void)self;
    if (status) *status = 4;           /* SL_PREFETCHSTATUS_SUFFICIENTDATA */
    return SL_OK;
}

static u32 pre_GetFillLevel(sl_itf *self, u16 *permille)
{
    (void)self;
    if (permille) *permille = 1000;
    return SL_OK;
}

static u32 pre_RegisterCallback(sl_itf *self, mg_addr callback, void *context)
{
    self->obj->prefetch_cb = callback;
    self->obj->prefetch_ctx = context;
    self->obj->prefetch_told = false;
    return SL_OK;
}

static const void *const g_prefetch_table[] = {
    (void *)pre_GetStatus,
    (void *)pre_GetFillLevel,
    (void *)pre_RegisterCallback,
    (void *)ok0,                /* SetCallbackEventsMask */
    (void *)play_GetU32,        /* GetCallbackEventsMask */
    (void *)ok0,                /* SetFillUpdatePeriod */
    (void *)play_GetU32,        /* GetFillUpdatePeriod */
};

/* ----------------------------------------------------------------- volume */

static void apply_volume(sl_object *o)
{
    /* Millibels: hundredths of a decibel, 0 the loudest. */
    if (o->voice) mg_voice_set_gain(o->voice, o->mute ? 0.0f : (float)pow(10.0, (double)o->volume_mb / 2000.0));
}

/* (An SLmillibel is a 16-bit integer, which an arm64 caller leaves the rest of the register beside.) */
static u32 vol_SetLevel(sl_itf *self, i16 millibels)
{
    self->obj->volume_mb = millibels > 0 ? 0 : millibels;
    apply_volume(self->obj);
    return SL_OK;
}

static u32 vol_GetLevel(sl_itf *self, i16 *millibels)
{
    if (millibels) *millibels = (i16)self->obj->volume_mb;
    return SL_OK;
}

static u32 vol_GetMax(sl_itf *self, i16 *millibels)
{
    (void)self;
    if (millibels) *millibels = 0;
    return SL_OK;
}

static u32 vol_SetMute(sl_itf *self, u32 mute)
{
    self->obj->mute = mute != 0;
    apply_volume(self->obj);
    return SL_OK;
}

static u32 vol_GetMute(sl_itf *self, u32 *mute)
{
    if (mute) *mute = self->obj->mute;
    return SL_OK;
}

static u32 vol_GetZero(sl_itf *self, void *out)
{
    (void)self;
    if (out) *(u16 *)out = 0;
    return SL_OK;
}

static const void *const g_volume_table[] = {
    (void *)vol_SetLevel,
    (void *)vol_GetLevel,
    (void *)vol_GetMax,
    (void *)vol_SetMute,
    (void *)vol_GetMute,
    (void *)ok0,                /* EnableStereoPosition */
    (void *)vol_GetZero,        /* IsEnabledStereoPosition */
    (void *)ok0,                /* SetStereoPosition */
    (void *)vol_GetZero,        /* GetStereoPosition */
};

static const void *const g_config_table[] = {
    (void *)ok0,                /* SetConfiguration */
    (void *)unsupported,        /* GetConfiguration */
    (void *)unsupported,        /* AcquireJavaProxy */
    (void *)unsupported,        /* ReleaseJavaProxy */
};

/* --------------------------------------------------------------- recorder
 *
 * An audio recorder: the microphone's sound into the app's own buffers. The app queues buffers (Enqueue), sets the
 * recorder recording, and is called back once for each buffer filled, where it takes the sound and queues the buffer
 * again (WebRTC: two buffers of ten milliseconds queued before SetRecordState(RECORDING), then one more from each
 * callback). As on Android, what is heard while no buffer is queued is dropped; stopping leaves the queue as it is and
 * Clear empties it; a callback may still be running when SetRecordState(STOPPED) returns, and none is once Destroy has.
 *
 * A thread of the recorder's own does it, started the first time the recorder records. It asks the shell for the
 * microphone as an AudioRecord does (mg_mic.c: at the rate and in the channels the app's format names, so nothing is
 * converted but 16 bits to float for a format that asks for float; Windows asks the user the first time, which the app's
 * SetRecordState does not wait for), takes what is heard ten milliseconds at a time and calls the app's callback, as
 * Android calls it from its record thread. Where there is no microphone, where the user said no, and once another
 * recording (a Java AudioRecord) has taken the microphone over, the app is given silence at a microphone's pace, as an
 * AudioRecord is.
 */
typedef struct rec_buffer {
    u8 *data;
    u32 size;
} rec_buffer;

typedef struct sl_recorder {
    mg_lock lock;
    mg_cond changed;                   /* `state`, `dead` or `in_callback` has */
    int channels;
    bool is_float;
    u32 state;                         /* SL_RECORDSTATE_STOPPED 1, PAUSED 2, RECORDING 3 */
    u32 starts;                        /* times it was set recording: the thread hears the microphone for one of them */
    rec_buffer *buffers;               /* a ring of `cap`: the app's buffers, in the order queued */
    u32 cap, head, count;
    u32 filled;                        /* bytes written of the first */
    u32 index;                         /* buffers filled and handed back since the last Clear */
    u64 frames;                        /* heard while recording: GetPosition */
    u32 mic;                           /* the recording's token while the thread hears the microphone (mg_mic_start) */
    bool has_thread, dead, in_callback;
    u32 thread;                        /* the recorder's thread, as mg_thread_self_id says it there */
    u32 preset, performance;           /* SL_ANDROID_KEY_RECORDING_PRESET, SL_ANDROID_KEY_PERFORMANCE_MODE */
    u32 enqueues;                      /* Enqueue calls said */
} sl_recorder;

#define REC_RECORDING 3u

/*
 * What was heard, into the app's buffers in the order they were queued; each one filled is handed back through the
 * callback, which usually queues it again. With none queued the rest is dropped. Called with the lock held; the app's
 * callback is called without it.
 */
static void rec_deliver(sl_object *o, u32 starts, const u8 *data, u32 bytes)
{
    sl_recorder *r = o->rec;
    while (bytes && r->count && !r->dead && r->state == REC_RECORDING && r->starts == starts) {
        rec_buffer *b = &r->buffers[r->head];
        u32 n = b->size - r->filled;
        if (n > bytes) n = bytes;
        memcpy(b->data + r->filled, data, n);
        r->filled += n;
        data += n;
        bytes -= n;
        if (r->filled < b->size) break;
        r->head = (r->head + 1) % r->cap;
        r->count--;
        r->index++;
        r->filled = 0;
        if (o->queue_cb) {
            const mg_addr cb = o->queue_cb;
            void *const ctx = o->queue_ctx;
            r->in_callback = true;
            mg_lock_release(&r->lock);
            MG_GFN(void (*)(sl_itf *, void *), cb)(&o->queue, ctx);
            mg_lock_acquire(&r->lock);
            r->in_callback = false;
            mg_cond_broadcast(&r->changed);
        }
    }
}

static MG_HOSTCB void recorder_thread(void *arg)
{
    sl_object *o = (sl_object *)arg;
    sl_recorder *r = o->rec;
    const int chunk = o->rate / 100;                       /* frames taken at a time: 10 ms */
    const int samples = chunk * r->channels;
    i16 *heard = (i16 *)mg_alloc(sizeof(i16) * (size_t)samples);
    float *as_float = r->is_float ? (float *)mg_alloc(sizeof(float) * (size_t)samples) : NULL;
    mg_lock_acquire(&r->lock);
    r->thread = mg_thread_self_id();
    for (;;) {
        const megler_microphone_host *h = &mg_run_config()->microphone;
        u32 starts, mic;
        u64 next, now;
        while (!r->dead && r->state != REC_RECORDING) mg_cond_wait(&r->changed, &r->lock);
        if (r->dead) break;
        starts = r->starts;
        mg_lock_release(&r->lock);
        mic = mg_mic_start(h, o->rate, r->channels);
        mg_info("audio: the app records through OpenSL ES at %d Hz in %d channel%s: %s", o->rate, r->channels, r->channels == 2 ? "s" : "",
                mic ? "the microphone hears" : h->start ? "the microphone was refused; the app records silence" : "no microphone here; the app records silence");
        next = mg_time_monotonic_ns();
        mg_lock_acquire(&r->lock);
        r->mic = mic;
        while (!r->dead && r->state == REC_RECORDING && r->starts == starts) {
            int got, i;
            mg_lock_release(&r->lock);
            got = mg_mic_read(mic, heard, samples, true);
            mg_lock_acquire(&r->lock);
            if (got < samples) {
                /* Nothing to hear (no microphone, refused, taken over by another recording): silence, at its pace. */
                memset(heard + got, 0, sizeof(i16) * (size_t)(samples - got));
                next += 10000000ull;
                while (!r->dead && r->state == REC_RECORDING && r->starts == starts && (now = mg_time_monotonic_ns()) < next)
                    mg_cond_wait_ns(&r->changed, &r->lock, next - now);
                now = mg_time_monotonic_ns();
                if (now > next && now - next > 200000000ull) next = now;
            } else next = mg_time_monotonic_ns();
            if (r->dead || r->state != REC_RECORDING || r->starts != starts) break;
            r->frames += (u64)chunk;
            if (as_float) {
                for (i = 0; i < samples; i++) as_float[i] = (float)heard[i] / 32768.0f;
                rec_deliver(o, starts, (const u8 *)as_float, (u32)samples * 4u);
            } else rec_deliver(o, starts, (const u8 *)heard, (u32)samples * 2u);
        }
        r->mic = 0;
        mg_lock_release(&r->lock);
        mg_mic_stop(mic);              /* (nothing, when SetRecordState or Destroy already let it go) */
        mg_lock_acquire(&r->lock);
    }
    mg_lock_release(&r->lock);
    free(heard);
    free(as_float);
}

/* SLDataSink is laid out as SLDataSource is: a locator and a format. */
static u32 eng_CreateAudioRecorder(sl_itf *self, sl_itf **out, const g_data_source *src, const g_data_source *sink, u32 n, const void *ids,
                                   const void *req)
{
    const sl_iid *const *wanted = (const sl_iid *const *)ids;
    const u32 *from, *to, *format;
    sl_object *o;
    sl_recorder *r;
    bool is_float;
    u32 i, rate;
    (void)self;
    if (!out) return SL_PARAMETER_INVALID;
    *out = NULL;
    if (!src || !src->locator || !sink || !sink->locator || !sink->format) {
        REC_SAY("CreateAudioRecorder with no source, no sink or no format: refused");
        return SL_PARAMETER_INVALID;
    }
    from = (const u32 *)src->locator;
    to = (const u32 *)sink->locator;
    format = (const u32 *)sink->format;
    REC_SAY("CreateAudioRecorder: from locator 0x%x (device type %u), into locator 0x%x of %u buffers, format type %u, %u channels, %u mHz, "
            "%u bits in %u, %u interfaces asked for",
            (unsigned)from[0], (unsigned)from[1], (unsigned)to[0], (unsigned)to[1], (unsigned)format[0], (unsigned)format[1], (unsigned)format[2],
            (unsigned)format[3], (unsigned)format[4], (unsigned)n);
    for (i = 0; wanted && i < n && i < 8; i++)
        REC_SAY("  interface %u: %s%s", (unsigned)i, iid_name(wanted[i]), req && ((const u32 *)req)[i] ? " (required)" : "");
    /*
     * From SL_DATALOCATOR_IODEVICE of SL_IODEVICE_AUDIOINPUT, into SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE (or the plain
     * buffer queue), as SL_DATAFORMAT_PCM or SL_ANDROID_DATAFORMAT_PCM_EX: the one recorder Android has.
     */
    if (from[0] != 3 || from[1] != 1 || (to[0] != 0x800007BDu && to[0] != 6) || (format[0] != 2 && format[0] != 4)) {
        mg_note_once("audio: a recorder that does not record the microphone into a PCM buffer queue was asked for (from 0x%x of type %u, into 0x%x, "
                     "format type %u)", (unsigned)from[0], (unsigned)from[1], (unsigned)to[0], (unsigned)format[0]);
        return SL_CONTENT_UNSUPPORTED;
    }
    /* 16-bit integers, or (PCM_EX's representation 3) 32-bit float; one or two channels. */
    is_float = format[0] == 4 && format[7] == 3 && format[3] == 32 && format[4] == 32;
    rate = format[2] / 1000;
    if (format[1] < 1 || format[1] > 2 || rate < 4000 || rate > 192000 ||
        (!is_float && (format[3] != 16 || format[4] != 16 || (format[0] == 4 && format[7] != 1)))) {
        mg_note_once("audio: a recording format the microphone is not heard in (%u Hz, %u channels, %u bits in %u)", (unsigned)rate, (unsigned)format[1],
                     (unsigned)format[3], (unsigned)format[4]);
        return SL_CONTENT_UNSUPPORTED;
    }
    o = new_object(OBJ_RECORDER);
    r = (sl_recorder *)mg_alloc(sizeof(*r));
    mg_lock_init(&r->lock);
    mg_cond_init(&r->changed);
    r->channels = (int)format[1];
    r->is_float = is_float;
    r->state = 1;                      /* SL_RECORDSTATE_STOPPED */
    r->cap = to[1] < 1 ? 1 : to[1] > 255 ? 255 : to[1];
    r->buffers = (rec_buffer *)mg_alloc(sizeof(rec_buffer) * r->cap);
    r->performance = 1;                /* SL_ANDROID_PERFORMANCE_LATENCY, what Android starts a recorder with */
    o->rec = r;
    o->rate = (int)rate;
    mg_info("audio: recorder %d Hz, %d channels, %s", o->rate, r->channels, is_float ? "32 bits float" : "16 bits");
    *out = &o->object;
    return SL_OK;
}

static void rec_destroy(sl_object *o)
{
    sl_recorder *r = o->rec;
    u32 mic;
    mg_lock_acquire(&r->lock);
    if (r->dead) {
        mg_lock_release(&r->lock);
        return;
    }
    r->dead = true;
    r->state = 1;
    mic = r->mic;
    mg_cond_broadcast(&r->changed);
    mg_lock_release(&r->lock);
    REC_SAY("Destroy");
    mg_mic_stop(mic);
    /*
     * The app frees what its callback uses right after this: it returns when the callback is not running (and the
     * thread, which ends by itself, calls it no more), as Android's Destroy does -- unless this is the callback.
     * The object itself stays, as a player's does.
     */
    mg_lock_acquire(&r->lock);
    while (r->in_callback && r->thread != mg_thread_self_id()) mg_cond_wait(&r->changed, &r->lock);
    mg_lock_release(&r->lock);
}

static u32 rec_SetRecordState(sl_itf *self, u32 state)
{
    sl_object *o = self->obj;
    sl_recorder *r = o->rec;
    u32 mic = 0, result = SL_OK;
    if (state < 1 || state > 3) return SL_PARAMETER_INVALID;
    REC_SAY("SetRecordState(%s)", state == REC_RECORDING ? "recording" : state == 2 ? "paused" : "stopped");
    mg_lock_acquire(&r->lock);
    if (r->dead) result = SL_PRECONDITIONS_VIOLATED;
    else if (state == REC_RECORDING && !r->has_thread) {
        /* The app's callback runs on it: a megabyte of stack, as the mixer's thread has for the players' callbacks. */
        mg_thread *t = mg_thread_start(recorder_thread, o, ((size_t)1 << 20) + MG_STACK_FAULT_ROOM, "audio-recorder");
        if (t) {
            mg_thread_detach(t);
            r->has_thread = true;
        } else result = SL_RESOURCE_ERROR;
    }
    if (result == SL_OK) {
        if (state == REC_RECORDING && r->state != REC_RECORDING) r->starts++;
        /* Stopped or paused, the microphone is let go here and now, which also ends the thread's wait for its sound. */
        if (state != REC_RECORDING) mic = r->mic;
        r->state = state;
        mg_cond_broadcast(&r->changed);
    }
    mg_lock_release(&r->lock);
    mg_mic_stop(mic);
    return result;
}

static u32 rec_GetRecordState(sl_itf *self, u32 *state)
{
    sl_recorder *r = self->obj->rec;
    if (!state) return SL_PARAMETER_INVALID;
    mg_lock_acquire(&r->lock);
    *state = r->state;
    mg_lock_release(&r->lock);
    return SL_OK;
}

static u32 rec_GetPosition(sl_itf *self, u32 *ms)
{
    sl_object *o = self->obj;
    sl_recorder *r = o->rec;
    if (!ms) return SL_PARAMETER_INVALID;
    mg_lock_acquire(&r->lock);
    *ms = (u32)(r->frames * 1000ull / (u64)o->rate);
    mg_lock_release(&r->lock);
    return SL_OK;
}

static const void *const g_record_table[] = {
    (void *)rec_SetRecordState,
    (void *)rec_GetRecordState,
    (void *)ok0,                /* SetDurationLimit */
    (void *)rec_GetPosition,
    (void *)ok0,                /* RegisterCallback (of the record events, which are not sent) */
    (void *)ok0,                /* SetCallbackEventsMask */
    (void *)play_GetU32,        /* GetCallbackEventsMask */
    (void *)ok0,                /* SetMarkerPosition */
    (void *)ok0,                /* ClearMarkerPosition */
    (void *)play_GetU32,        /* GetMarkerPosition */
    (void *)ok0,                /* SetPositionUpdatePeriod */
    (void *)play_GetU32,        /* GetPositionUpdatePeriod */
};

static u32 recq_Enqueue(sl_itf *self, void *buffer, u32 size)
{
    sl_recorder *r = self->obj->rec;
    u32 result = SL_OK;
    bool say;
    if (!buffer || !size) return SL_PARAMETER_INVALID;
    mg_lock_acquire(&r->lock);
    if (r->count >= r->cap) result = SL_BUFFER_INSUFFICIENT;
    else {
        rec_buffer *b = &r->buffers[(r->head + r->count) % r->cap];
        b->data = (u8 *)buffer;
        b->size = size;
        r->count++;
    }
    say = r->enqueues < 4 || (result != SL_OK && r->enqueues < 16);
    if (say) r->enqueues++;
    mg_lock_release(&r->lock);
    if (say) REC_SAY("Enqueue(%u bytes) -> %s", (unsigned)size, result == SL_OK ? "ok" : "the queue is full");
    return result;
}

static u32 recq_Clear(sl_itf *self)
{
    sl_recorder *r = self->obj->rec;
    mg_lock_acquire(&r->lock);
    r->head = r->count = r->filled = r->index = 0;
    mg_lock_release(&r->lock);
    REC_SAY("buffer queue: cleared");
    return SL_OK;
}

static u32 recq_GetState(sl_itf *self, g_queue_state *state)
{
    sl_recorder *r = self->obj->rec;
    if (!state) return SL_PARAMETER_INVALID;
    mg_lock_acquire(&r->lock);
    state->count = r->count;
    state->index = r->index;
    mg_lock_release(&r->lock);
    return SL_OK;
}

static u32 recq_RegisterCallback(sl_itf *self, mg_addr callback, void *context)
{
    sl_object *o = self->obj;
    mg_lock_acquire(&o->rec->lock);
    o->queue_cb = callback;
    o->queue_ctx = context;
    mg_lock_release(&o->rec->lock);
    REC_SAY("buffer queue: callback %s", callback ? "registered" : "taken away");
    return SL_OK;
}

static const void *const g_rec_queue_table[] = {
    (void *)recq_Enqueue,
    (void *)recq_Clear,
    (void *)recq_GetState,
    (void *)recq_RegisterCallback,
};

/* SLAndroidConfigurationItf of a recorder: the recording preset and the performance mode, kept and told back. */
static u32 *rec_config_value(sl_recorder *r, const char *key, u32 *max)
{
    if (!strcmp(key, "androidRecordingPreset")) {
        *max = 5;                      /* SL_ANDROID_RECORDING_PRESET_UNPROCESSED */
        return &r->preset;
    }
    if (!strcmp(key, "androidPerformanceMode")) {
        *max = 3;                      /* SL_ANDROID_PERFORMANCE_POWER_SAVING */
        return &r->performance;
    }
    return NULL;
}

static u32 reccfg_SetConfiguration(sl_itf *self, const char *key, const void *value, u32 size)
{
    u32 v, max = 0, *kept;
    if (!key || !value || !size) return SL_PARAMETER_INVALID;
    kept = rec_config_value(self->obj->rec, key, &max);
    if (!kept) {
        REC_SAY("SetConfiguration(%.40s): a key a recorder does not have", key);
        return SL_PARAMETER_INVALID;
    }
    if (size < 4) return SL_BUFFER_INSUFFICIENT;
    memcpy(&v, value, 4);
    REC_SAY("SetConfiguration(%s, %u)%s", key, (unsigned)v, v > max ? ": no such value" : kept == &self->obj->rec->preset && v == 4 ? ": voice communication" : "");
    if (v > max) return SL_PARAMETER_INVALID;
    *kept = v;
    return SL_OK;
}

static u32 reccfg_GetConfiguration(sl_itf *self, const char *key, u32 *size, void *value)
{
    u32 max = 0, *kept;
    if (!key || !size) return SL_PARAMETER_INVALID;
    kept = rec_config_value(self->obj->rec, key, &max);
    if (!kept) return SL_PARAMETER_INVALID;
    if (value) {
        if (*size < 4) {
            *size = 4;
            return SL_BUFFER_INSUFFICIENT;
        }
        memcpy(value, kept, 4);
    }
    *size = 4;
    return SL_OK;
}

static const void *const g_rec_config_table[] = {
    (void *)reccfg_SetConfiguration,
    (void *)reccfg_GetConfiguration,
    (void *)unsupported,        /* AcquireJavaProxy */
    (void *)unsupported,        /* ReleaseJavaProxy */
};

static sl_object *new_object(int kind)
{
    sl_object *o = (sl_object *)mg_alloc(sizeof(*o));
    o->kind = kind;
    o->play_state = 1;                 /* SL_PLAYSTATE_STOPPED */
    o->object.table = g_object_table;
    o->engine.table = g_engine_table;
    o->play.table = g_play_table;
    o->queue.table = g_queue_table;
    o->volume.table = g_volume_table;
    o->config.table = g_config_table;
    o->record.table = g_record_table;
    o->seek.table = g_seek_table;
    o->prefetch.table = g_prefetch_table;
    o->object.obj = o->engine.obj = o->play.obj = o->queue.obj = o->volume.obj = o->config.obj = o->record.obj = o;
    o->seek.obj = o->prefetch.obj = o;
    mg_lock_init(&o->file_lock);
    mg_cond_init(&o->file_cond);
    if (kind == OBJ_RECORDER) {
        o->queue.table = g_rec_queue_table;
        o->config.table = g_rec_config_table;
    }
    return o;
}

/* ------------------------------------------------------------ entry points */

static u32 g_slCreateEngine(sl_itf **engine, u32 nopts, const void *opts, u32 nifaces, const void *ids, const void *required)
{
    (void)opts; (void)ids; (void)required;
    if (!engine) return SL_PARAMETER_INVALID;
    *engine = &new_object(OBJ_ENGINE)->object;
    SL_SAY("slCreateEngine (%u options, %u interfaces asked for)", (unsigned)nopts, (unsigned)nifaces);
    return SL_OK;
}

static u32 g_slQueryNumSupportedEngineInterfaces(u32 *count)
{
    if (count) *count = 1;
    return SL_OK;
}

static const char *const g_iid_names[] = {
#define X(n) #n,
    SL_NAMES(X)
#undef X
};

/* An interface ID's name: by which of this library's IDs it points at, or by what it holds (an app may keep copies of its own). */
static const char *iid_name(const void *iid)
{
    int i;
    for (i = 0; i < IID_COUNT; i++)
        if (iid == (const void *)g_iid_ptrs[i]) return g_iid_names[i];
    for (i = 0; iid && i < IID_COUNT; i++)
        if (!memcmp(iid, &g_iids[i], sizeof(sl_iid))) return g_iid_names[i];
    return iid ? "an ID this library did not hand out" : "null";
}

static mg_sym g_tab[IID_COUNT + 3];

const mg_sym *mg_opensles_table(void)
{
    static const char *const names[] = {
#define X(n) "SL_IID_" #n,
        SL_NAMES(X)
#undef X
    };
    int i;
    for (i = 0; i < IID_COUNT; i++) {
        g_iids[i].time_low = 0x79216360u + (u32)i;
        g_iids[i].time_mid = 0xDDD7;
        g_iids[i].time_hi_and_version = 0x11DB;
        g_iids[i].clock_seq = 0xAC16;
        g_iid_ptrs[i] = &g_iids[i];
        g_tab[i].name = names[i];
        g_tab[i].addr = (void *)&g_iid_ptrs[i];
    }
    g_tab[IID_COUNT].name = "slCreateEngine";
    g_tab[IID_COUNT].addr = (void *)g_slCreateEngine;
    g_tab[IID_COUNT + 1].name = "slQueryNumSupportedEngineInterfaces";
    g_tab[IID_COUNT + 1].addr = (void *)g_slQueryNumSupportedEngineInterfaces;
    g_tab[IID_COUNT + 2].name = NULL;
    g_tab[IID_COUNT + 2].addr = NULL;
    return g_tab;
}
