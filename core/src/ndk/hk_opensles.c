/*
 * libOpenSLES.so: PCM buffer-queue players over the mixer (hk_audio.c).
 *
 * That is the part of OpenSL ES games use: an engine, an output mix, and audio
 * players fed from the app's own buffers through a buffer queue, each with a
 * callback that asks for the next buffer. FMOD, OpenAL Soft, SDL and the
 * engines' own mixers all sit on exactly this. Players that take a file or a
 * URL -- which would need this layer to decode MP3 or Ogg itself -- are
 * refused, and the app carries on without that sound.
 *
 * An OpenSL ES object is reached through interfaces; an interface is a
 * pointer to a pointer to a table of functions, each taking the interface
 * itself as its first argument.
 */
#include <math.h>

#include "../bionic/hk_bionic.h"
#include "hk_audio.h"

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
#define SL_PARAMETER_INVALID 2u
#define SL_BUFFER_INSUFFICIENT 7u
#define SL_CONTENT_UNSUPPORTED 10u
#define SL_FEATURE_UNSUPPORTED 12u

enum { OBJ_ENGINE = 1, OBJ_MIX, OBJ_PLAYER };

struct sl_object;
typedef struct sl_itf {
    const void *const *table;
    struct sl_object *obj;
} sl_itf;

typedef struct sl_object {
    int kind;
    sl_itf object, engine, play, queue, volume, config;
    hk_voice *voice;
    int rate;
    u32 play_state;
    hk_addr queue_cb;
    void *queue_ctx;
    i32 volume_mb;
    bool mute;
} sl_object;

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
        if (hk_atomic_add(&g_said, 1) <= 120) hk_info("opensl: " __VA_ARGS__); \
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
    }
    *out = itf;
    SL_SAY("GetInterface(%s) on %s -> %s", iid_name(iid), o->kind == OBJ_ENGINE ? "the engine" : o->kind == OBJ_MIX ? "the output mix" : "a player",
           itf ? "ok" : "not supported");
    return itf ? SL_OK : SL_FEATURE_UNSUPPORTED;
}

static void obj_Destroy(sl_itf *self)
{
    sl_object *o = self->obj;
    if (o->voice) hk_voice_destroy(o->voice);
    /* The object itself stays: the mixer may be inside this player's callback, which holds its queue interface. */
    o->voice = NULL;
}

static const void *const g_object_table[] = {
    (void *)ok0,                /* Realize */
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

typedef struct g_data_source {
    u32 locator, format;
} g_data_source;

static void voice_done(hk_voice *v, void *user)
{
    sl_object *o = (sl_object *)user;
    (void)v;
    if (o->queue_cb) HK_GFN(void (*)(sl_itf *, void *), o->queue_cb)(&o->queue, o->queue_ctx);
}

static u32 eng_CreateAudioPlayer(sl_itf *self, sl_itf **out, const g_data_source *src, const void *sink, u32 n, const void *ids, const void *req)
{
    const u32 *locator, *format;
    sl_object *o;
    bool is_float;
    (void)self; (void)sink; (void)n; (void)ids; (void)req;
    if (!out || !src || !src->locator || !src->format) return SL_PARAMETER_INVALID;
    *out = NULL;
    locator = (const u32 *)(hk_addr)src->locator;
    format = (const u32 *)(hk_addr)src->format;
    SL_SAY("CreateAudioPlayer: locator 0x%x, format type %u, %u channels, %u mHz, %u bits in %u, channel mask 0x%x, %u interfaces asked for",
           (unsigned)locator[0], (unsigned)format[0], (unsigned)format[1], (unsigned)format[2], (unsigned)format[3], (unsigned)format[4],
           (unsigned)format[5], (unsigned)n);
    {
        u32 i;
        const sl_iid *const *wanted = (const sl_iid *const *)ids;
        for (i = 0; wanted && i < n && i < 8; i++) SL_SAY("  interface %u: %s%s", (unsigned)i, iid_name(wanted[i]), req && ((const u32 *)req)[i] ? " (required)" : "");
    }
    /* SL_DATALOCATOR_BUFFERQUEUE, SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE; SL_DATAFORMAT_PCM, SL_ANDROID_DATAFORMAT_PCM_EX */
    if ((locator[0] != 6 && locator[0] != 0x800007BDu) || (format[0] != 2 && format[0] != 4)) {
        hk_note_once("audio: a player that reads a file or a URL was asked for; only PCM buffer queues are played");
        return SL_CONTENT_UNSUPPORTED;
    }
    /* SLDataFormat_PCM: formatType, numChannels, samplesPerSec (in millihertz), bitsPerSample, containerSize, channelMask, endianness[, representation] */
    is_float = format[0] == 4 && format[7] == 3;
    o = new_object(OBJ_PLAYER);
    o->rate = (int)(format[2] / 1000);
    o->voice = hk_voice_create(o->rate, (int)format[1], (int)format[4], is_float);
    if (!o->voice) {
        hk_note_once("audio: a PCM format that cannot be mixed (%u Hz, %u channels, %u bits)", format[2] / 1000, format[1], format[4]);
        free(o);
        return SL_CONTENT_UNSUPPORTED;
    }
    hk_voice_set_done(o->voice, voice_done, o);
    hk_info("audio: player %d Hz, %u channels, %u bits%s", o->rate, format[1], format[4], is_float ? " float" : "");
    *out = &o->object;
    return SL_OK;
}

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

static const void *const g_engine_table[] = {
    (void *)unsupported,        /* CreateLEDDevice */
    (void *)unsupported,        /* CreateVibraDevice */
    (void *)eng_CreateAudioPlayer,
    (void *)unsupported,        /* CreateAudioRecorder */
    (void *)unsupported,        /* CreateMidiPlayer */
    (void *)unsupported,        /* CreateListener */
    (void *)unsupported,        /* Create3DGroup */
    (void *)eng_CreateOutputMix,
    (void *)unsupported,        /* CreateMetadataExtractor */
    (void *)unsupported,        /* CreateExtensionObject */
    (void *)eng_QueryNum,       /* QueryNumSupportedInterfaces */
    (void *)unsupported,        /* QuerySupportedInterfaces */
    (void *)eng_QueryNum,       /* QueryNumSupportedExtensions */
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
    if (o->voice) {
        hk_voice_set_playing(o->voice, state == 3);       /* SL_PLAYSTATE_PLAYING */
        if (state == 1) hk_voice_clear(o->voice);         /* stopping empties the queue, as Android's does */
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
    (void)self;
    if (ms) *ms = 0xFFFFFFFFu;         /* SL_TIME_UNKNOWN */
    return SL_OK;
}

static u32 play_GetPosition(sl_itf *self, u32 *ms)
{
    sl_object *o = self->obj;
    if (ms) *ms = o->voice && o->rate ? (u32)(hk_voice_position(o->voice) * 1000ull / (u64)o->rate) : 0;
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
    (void *)ok0,                /* RegisterCallback */
    (void *)ok0,                /* SetCallbackEventsMask */
    (void *)play_GetU32,        /* GetCallbackEventsMask */
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
    return hk_voice_enqueue(o->voice, buffer, size) ? SL_OK : SL_BUFFER_INSUFFICIENT;
}

static u32 queue_Clear(sl_itf *self)
{
    if (self->obj->voice) hk_voice_clear(self->obj->voice);
    return SL_OK;
}

typedef struct g_queue_state {
    u32 count, index;
} g_queue_state;

static u32 queue_GetState(sl_itf *self, g_queue_state *state)
{
    sl_object *o = self->obj;
    if (!state) return SL_PARAMETER_INVALID;
    state->count = o->voice ? hk_voice_queued(o->voice) : 0;
    state->index = o->voice ? hk_voice_played(o->voice) : 0;
    return SL_OK;
}

static u32 queue_RegisterCallback(sl_itf *self, hk_addr callback, void *context)
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

/* ----------------------------------------------------------------- volume */

static void apply_volume(sl_object *o)
{
    /* Millibels: hundredths of a decibel, 0 the loudest. */
    if (o->voice) hk_voice_set_gain(o->voice, o->mute ? 0.0f : (float)pow(10.0, (double)o->volume_mb / 2000.0));
}

static u32 vol_SetLevel(sl_itf *self, int millibels)
{
    self->obj->volume_mb = (i16)millibels > 0 ? 0 : (i16)millibels;
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

static sl_object *new_object(int kind)
{
    sl_object *o = (sl_object *)hk_alloc(sizeof(*o));
    o->kind = kind;
    o->play_state = 1;                 /* SL_PLAYSTATE_STOPPED */
    o->object.table = g_object_table;
    o->engine.table = g_engine_table;
    o->play.table = g_play_table;
    o->queue.table = g_queue_table;
    o->volume.table = g_volume_table;
    o->config.table = g_config_table;
    o->object.obj = o->engine.obj = o->play.obj = o->queue.obj = o->volume.obj = o->config.obj = o;
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

static hk_sym g_tab[IID_COUNT + 3];

const hk_sym *hk_opensles_table(void)
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
