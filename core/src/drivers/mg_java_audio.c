/*
 * Sound for Java apps: megler.Audio's natives, which the framework's AudioTrack, MediaPlayer and SoundPool play through.
 *
 * - A track (AudioTrack; the music MediaPlayer decodes and writes) is a written stream of the mixer (mg_pcm_*,
 *   ndk/mg_aaudio.c): 16-bit PCM at the track's own rate and channels, 100 ms of it waiting at most, a blocking
 *   write waiting for room as AudioTrack's does, paused or not.
 * - A sound (SoundPool) is decoded whole when it is loaded; each play of it is a voice of the mixer at the sound's
 *   rate times the play's, its two volumes, and its loops (the decoded sound queued again while loops are left;
 *   -1 for ever).
 * - A decoder (MediaPlayer, SoundPool's loading) reads a file the app has in memory or behind a descriptor:
 *   Ogg Vorbis through stb_vorbis (Windows has no Vorbis decoder), WAV (PCM, 8 to 32 bits, or float) read here, and
 *   anything else (MP3, AAC and M4A, FLAC, WMA) through the shell's decoder (megler_media_host: Media Foundation's
 *   source reader on a phone), which wants a file: the bytes are written to one of its own under the data folder,
 *   named for what they are, and it is deleted when the decoder is closed.
 *
 * Every decoder gives 16-bit samples, at most two channels (a file with more gives its first two).
 */
#include "mg_java.h"

#include <stdlib.h>
#include <string.h>

#include "../bionic/mg_fd.h"
#include "../host/mg_host.h"
#include "../jni/mg_jni.h"
#include "../ndk/mg_audio.h"
#include "../run/mg_run.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../../third_party/stb_vorbis/stb_vorbis.inc"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

const char *megler_data_dir(void);

#define N(name) static void name(mg_jcall *c)

#define MAX_TRACKS 32
#define MAX_SOUNDS 1024
#define MAX_PLAYS 64
#define MAX_FILE (256 << 20)

static mg_lock g_lock = MG_LOCK_INIT;

/* ------------------------------------------------------------------ tracks */

typedef struct track {
    mg_pcm *pcm;
    int channels;
} track;

static track g_tracks[MAX_TRACKS];

/* A track by its id, NULL when there is none. Tracks are made and closed under the lock; a track in use is not closed
 * under its writer by the framework (AudioTrack.release and MediaPlayer stop their writing first). */
static track *track_of(i32 id) { return id >= 0 && id < MAX_TRACKS && g_tracks[id].pcm ? &g_tracks[id] : NULL; }

N(Audio_trackOpen)
{
    i32 rate = c->args[0].i, channels = c->args[1].i == 1 ? 1 : 2, id = -1, i;
    mg_pcm *p = mg_pcm_open(rate > 0 ? rate : MG_AUDIO_RATE, channels, 1, 100);
    if (p) {
        mg_lock_acquire(&g_lock);
        for (i = 0; i < MAX_TRACKS && id < 0; i++)
            if (!g_tracks[i].pcm) id = i;
        if (id >= 0) {
            g_tracks[id].pcm = p;
            g_tracks[id].channels = channels;
        }
        mg_lock_release(&g_lock);
        if (id < 0) mg_pcm_close(p);
    }
    c->ret.i = id;
}

static i16 clamp16(float f)
{
    f *= 32767.0f;
    return (i16)(f > 32767.0f ? 32767 : f < -32768.0f ? -32768 : (i32)f);
}

/* The array's elements off..off+n of `esz` bytes, if they are there. */
static const u8 *elements(mg_jobj *arr, i32 off, i32 n, u32 esz)
{
    if (!arr || off < 0 || n <= 0 || (u64)off + (u64)n > arr->u.arr.len) return NULL;
    return (const u8 *)arr->u.arr.data + (size_t)off * esz;
}

/* trackWrite(int id, short[] data, int off, int n, boolean blocking): samples taken. */
N(Audio_trackWrite)
{
    track *t = track_of(c->args[0].i);
    const u8 *p = elements(c->args[1].l, c->args[2].i, c->args[3].i, 2);
    c->ret.i = 0;
    if (!t || !p) return;
    c->ret.i = mg_pcm_write(t->pcm, p, c->args[3].i / t->channels, c->args[4].z != 0) * t->channels;
}

/* Floats to 16-bit, `n` samples, into a buffer of the caller's to free. */
static i16 *from_floats(const float *f, i32 n)
{
    i16 *out = (i16 *)mg_alloc((size_t)n * 2);
    i32 i;
    for (i = 0; i < n; i++) out[i] = clamp16(f[i]);
    return out;
}

/* trackWriteBytes(int id, byte[] data, int off, int n, int encoding, boolean blocking): bytes taken. Encoding 3 is 8-bit, 4 float, else 16-bit. */
N(Audio_trackWriteBytes)
{
    track *t = track_of(c->args[0].i);
    const u8 *p = elements(c->args[1].l, c->args[2].i, c->args[3].i, 1);
    i32 n = c->args[3].i, enc = c->args[4].i;
    bool wait = c->args[5].z != 0;
    c->ret.i = 0;
    if (!t || !p) return;
    if (enc == 3) {
        i16 *tmp = (i16 *)mg_alloc((size_t)n * 2);
        i32 i;
        for (i = 0; i < n; i++) tmp[i] = (i16)((p[i] - 128) << 8);
        c->ret.i = mg_pcm_write(t->pcm, tmp, n / t->channels, wait) * t->channels;
        free(tmp);
    } else if (enc == 4) {
        i16 *tmp = from_floats((const float *)p, n / 4);
        c->ret.i = mg_pcm_write(t->pcm, tmp, n / 4 / t->channels, wait) * t->channels * 4;
        free(tmp);
    } else {
        c->ret.i = mg_pcm_write(t->pcm, p, n / 2 / t->channels, wait) * t->channels * 2;
    }
}

/* trackWriteFloat(int id, float[] data, int off, int n, boolean blocking): samples taken. */
N(Audio_trackWriteFloat)
{
    track *t = track_of(c->args[0].i);
    const u8 *p = elements(c->args[1].l, c->args[2].i, c->args[3].i, 4);
    i16 *tmp;
    c->ret.i = 0;
    if (!t || !p) return;
    tmp = from_floats((const float *)p, c->args[3].i);
    c->ret.i = mg_pcm_write(t->pcm, tmp, c->args[3].i / t->channels, c->args[4].z != 0) * t->channels;
    free(tmp);
}

N(Audio_trackPlay)
{
    track *t = track_of(c->args[0].i);
    if (t) mg_pcm_play(t->pcm, c->args[1].z != 0);
}

N(Audio_trackFlush)
{
    track *t = track_of(c->args[0].i);
    if (t) mg_pcm_flush(t->pcm);
}

N(Audio_trackVolume)
{
    track *t = track_of(c->args[0].i);
    if (t) mg_pcm_volume(t->pcm, c->args[1].f, c->args[2].f);
}

N(Audio_trackPosition)
{
    track *t = track_of(c->args[0].i);
    c->ret.j = t ? (i64)mg_pcm_position(t->pcm) : 0;
}

N(Audio_trackPending)
{
    track *t = track_of(c->args[0].i);
    c->ret.i = t ? mg_pcm_pending(t->pcm) : 0;
}

N(Audio_trackClose)
{
    i32 id = c->args[0].i;
    mg_pcm *p = NULL;
    mg_lock_acquire(&g_lock);
    if (track_of(id)) {
        p = g_tracks[id].pcm;
        g_tracks[id].pcm = NULL;
    }
    mg_lock_release(&g_lock);
    if (p) mg_pcm_close(p);
}

/* ---------------------------------------------------------------- decoders */

typedef struct decoder {
    stb_vorbis *ogg;
    const u8 *wav;                     /* the WAV's samples, in `data` */
    u32 wav_frames, wav_pos, wav_bytes; /* frames in all, the next, bytes a sample */
    bool wav_float;
    void *media;                       /* the shell's decoder, of the file at `path` */
    char *path;
    i32 file_channels;                 /* what the shell's decoder gives, which may be more than 2 */
    u8 *data;                          /* the file, kept while Vorbis or WAV reads from it */
    i32 rate, channels;
    i64 frames;
} decoder;

static const megler_media_host *media_host(void)
{
    const megler_run_config *run = mg_run_config();
    return run && run->media.open ? &run->media : NULL;
}

static u32 rd32le(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16le(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* A RIFF WAVE file of PCM (8, 16, 24 or 32 bits) or 32-bit float. */
static bool open_wav(decoder *d, const u8 *data, size_t len)
{
    size_t at = 12;
    u16 format = 0, channels = 0, bits = 0;
    u32 rate = 0;
    if (len < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) return false;
    while (at + 8 <= len) {
        u32 size = rd32le(data + at + 4);
        const u8 *body = data + at + 8;
        if (!memcmp(data + at, "fmt ", 4) && size >= 16 && at + 8 + 16 <= len) {
            format = rd16le(body);
            channels = rd16le(body + 2);
            rate = rd32le(body + 4);
            bits = rd16le(body + 14);
            if (format == 0xFFFE && size >= 26) format = rd16le(body + 24);   /* WAVE_FORMAT_EXTENSIBLE: its subformat */
        } else if (!memcmp(data + at, "data", 4)) {
            u32 avail = (u32)(len - (at + 8) < size ? len - (at + 8) : size);
            if (!channels || !rate || (format != 1 && format != 3) || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) return false;
            if (format == 3 && bits != 32) return false;
            d->wav = body;
            d->wav_bytes = bits / 8u;
            d->wav_float = format == 3;
            d->file_channels = channels;
            d->wav_frames = avail / (d->wav_bytes * channels);
            d->rate = (i32)rate;
            d->channels = channels > 1 ? 2 : 1;
            d->frames = d->wav_frames;
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

static i16 wav_sample(const decoder *d, const u8 *p)
{
    switch (d->wav_bytes) {
    case 1: return (i16)((p[0] - 128) << 8);
    case 2: return (i16)rd16le(p);
    case 3: return (i16)(p[1] | p[2] << 8);
    default: return d->wav_float ? clamp16(mg_f(rd32le(p))) : (i16)(rd32le(p) >> 16);
    }
}

/* The extension the shell's decoder knows the bytes by. */
static const char *kind_of(const u8 *d, size_t len)
{
    if (len >= 12 && !memcmp(d + 4, "ftyp", 4)) return "m4a";
    if (len >= 4 && !memcmp(d, "fLaC", 4)) return "flac";
    if (len >= 4 && !memcmp(d, "\x30\x26\xB2\x75", 4)) return "wma";
    if (len >= 3 && !memcmp(d, "ID3", 3)) return "mp3";
    if (len >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) return "aac";     /* ADTS */
    if (len >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return "mp3";
    return "mp3";
}

/* The bytes to a file of their own, which the shell's decoder opens. */
static bool open_media(decoder *d, const u8 *data, size_t len)
{
    static volatile i32 serial;
    const megler_media_host *h = media_host();
    const char *dir = megler_data_dir();
    megler_media_info info;
    char *folder;
    mg_file *out;
    int err = 0;
    bool written;
    if (!h) {
        mg_note_once("audio: the shell has no decoder for compressed sound; the app is told such a file cannot be played");
        return false;
    }
    folder = mg_sprintf("%s/media", dir && dir[0] ? dir : ".");
    mg_path_mkdirs(folder);
    d->path = mg_sprintf("%s/sound-%u-%d.%s", folder, (unsigned)mg_thread_self_id(), (int)mg_atomic_add(&serial, 1), kind_of(data, len));
    free(folder);
    out = mg_file_open(d->path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    if (!out) return false;
    written = mg_file_write(out, data, len, &err) == (i64)len;
    mg_file_close(out);
    if (!written) return false;
    memset(&info, 0, sizeof(info));
    d->media = h->open(h->user, d->path, &info);
    if (d->media && (info.sample_rate <= 0 || info.channels <= 0)) {
        h->close(h->user, d->media);
        d->media = NULL;
    }
    if (!d->media) {
        mg_warn("audio: the shell cannot decode %s", d->path);
        return false;
    }
    d->rate = info.sample_rate;
    d->file_channels = info.channels;
    d->channels = info.channels > 1 ? 2 : 1;
    d->frames = info.duration_us * info.sample_rate / 1000000;
    return true;
}

static void dec_close(decoder *d)
{
    int err = 0;
    const megler_media_host *h = media_host();
    if (!d) return;
    if (d->ogg) stb_vorbis_close(d->ogg);
    if (d->media && h && h->close) h->close(h->user, d->media);
    if (d->path) {
        mg_path_remove(d->path, &err);
        free(d->path);
    }
    free(d->data);
    free(d);
}

/* A decoder of the file `data` (taken: freed with the decoder), or NULL. */
static decoder *dec_open(u8 *data, size_t len)
{
    decoder *d = (decoder *)mg_alloc(sizeof(*d));
    d->data = data;
    if (len > 4 && !memcmp(data, "OggS", 4)) {
        int err = 0;
        d->ogg = stb_vorbis_open_memory(data, (int)len, &err, NULL);
        if (d->ogg) {
            stb_vorbis_info i = stb_vorbis_get_info(d->ogg);
            d->rate = (i32)i.sample_rate;
            d->file_channels = i.channels;
            d->channels = i.channels > 1 ? 2 : 1;
            d->frames = stb_vorbis_stream_length_in_samples(d->ogg);
            return d;
        }
        mg_warn("audio: an Ogg file stb_vorbis cannot read (error %d): not Vorbis, or damaged", err);
        dec_close(d);
        return NULL;
    }
    if (open_wav(d, data, len)) return d;
    if (open_media(d, data, len)) {
        /* The shell's decoder has its own copy now. */
        free(d->data);
        d->data = NULL;
        return d;
    }
    dec_close(d);
    return NULL;
}

/* Up to `frames` frames of 16-bit sound, `channels` to a frame. How many; 0 at the end. */
static i32 dec_read(decoder *d, i16 *out, i32 frames)
{
    if (d->ogg) return stb_vorbis_get_samples_short_interleaved(d->ogg, d->channels, out, frames * d->channels);
    if (d->wav) {
        i32 n = (i32)(d->wav_frames - d->wav_pos < (u32)frames ? d->wav_frames - d->wav_pos : (u32)frames), i;
        for (i = 0; i < n; i++) {
            const u8 *f = d->wav + (size_t)(d->wav_pos + (u32)i) * d->wav_bytes * (u32)d->file_channels;
            out[i * d->channels] = wav_sample(d, f);
            if (d->channels == 2) out[i * 2 + 1] = wav_sample(d, f + d->wav_bytes);
        }
        d->wav_pos += (u32)n;
        return n;
    }
    if (d->media) {
        const megler_media_host *h = media_host();
        i64 when = 0;
        i32 n, i;
        if (!h || !h->read_audio) return 0;
        if (d->file_channels <= 2) {
            n = h->read_audio(h->user, d->media, out, frames, &when);
            return n > 0 ? n : 0;
        }
        {
            i16 *wide = (i16 *)mg_alloc((size_t)frames * (size_t)d->file_channels * 2);
            n = h->read_audio(h->user, d->media, wide, frames, &when);
            for (i = 0; i < n; i++) {
                out[i * 2] = wide[i * d->file_channels];
                out[i * 2 + 1] = wide[i * d->file_channels + 1];
            }
            free(wide);
            return n > 0 ? n : 0;
        }
    }
    return 0;
}

static void dec_seek(decoder *d, i64 frame)
{
    if (frame < 0) frame = 0;
    if (d->ogg) stb_vorbis_seek(d->ogg, (unsigned)frame);
    else if (d->wav) d->wav_pos = (u32)(frame < (i64)d->wav_frames ? frame : (i64)d->wav_frames);
    else if (d->media) {
        const megler_media_host *h = media_host();
        if (h && h->seek && d->rate) h->seek(h->user, d->media, frame * 1000000 / d->rate);
    }
}

/* A copy of a byte[]'s contents, or NULL. */
static u8 *bytes_of(mg_jobj *arr, size_t *len)
{
    u8 *d;
    if (!arr || !arr->u.arr.len) return NULL;
    d = (u8 *)mg_alloc(arr->u.arr.len);
    memcpy(d, arr->u.arr.data, arr->u.arr.len);
    *len = arr->u.arr.len;
    return d;
}

/* `len` bytes from `off` of the app's descriptor (all that is there, when `len` is not above 0), or NULL. */
static u8 *bytes_at(i32 fd, i64 off, i64 len, size_t *out)
{
    size_t cap = len > 0 ? (size_t)len : 1 << 20, got = 0;
    u8 *d;
    if (fd < 0 || off < 0 || len > MAX_FILE) return NULL;
    d = (u8 *)malloc(cap);
    if (!d) return NULL;
    for (;;) {
        i32 n;
        if (got == cap) {
            u8 *more;
            if (len > 0 || cap >= MAX_FILE) break;
            more = (u8 *)realloc(d, cap * 2);
            if (!more) break;
            d = more;
            cap *= 2;
        }
        n = mg_fd_pread(fd, d + got, (u32)(cap - got), (u64)off + got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    if (!got) {
        free(d);
        return NULL;
    }
    *out = got;
    return d;
}

N(Audio_decOpen)
{
    size_t len = 0;
    u8 *d = bytes_of(c->args[0].l, &len);
    c->ret.j = d ? (i64)(mg_addr)dec_open(d, len) : 0;
}

N(Audio_decOpenFd)
{
    size_t len = 0;
    u8 *d = bytes_at(c->args[0].i, c->args[1].j, c->args[2].j, &len);
    c->ret.j = d ? (i64)(mg_addr)dec_open(d, len) : 0;
}

/* decInfo(long dec, int[] out): the rate, the channels, the length in milliseconds. */
N(Audio_decInfo)
{
    decoder *d = (decoder *)(mg_addr)c->args[0].j;
    mg_jobj *o = c->args[1].l;
    if (d && o && o->u.arr.len >= 3) {
        i32 *v = (i32 *)o->u.arr.data;
        v[0] = d->rate;
        v[1] = d->channels;
        v[2] = d->rate ? (i32)(d->frames * 1000 / d->rate) : 0;
    }
}

/* decRead(long dec, short[] buf): samples read into it, 0 at the end. */
N(Audio_decRead)
{
    decoder *d = (decoder *)(mg_addr)c->args[0].j;
    mg_jobj *o = c->args[1].l;
    c->ret.i = 0;
    if (!d || !o || !d->channels) return;
    c->ret.i = dec_read(d, (i16 *)o->u.arr.data, (i32)(o->u.arr.len / (u32)d->channels)) * d->channels;
}

N(Audio_decSeek)
{
    decoder *d = (decoder *)(mg_addr)c->args[0].j;
    if (d) dec_seek(d, (i64)c->args[1].i * d->rate / 1000);
}

N(Audio_decClose) { dec_close((decoder *)(mg_addr)c->args[0].j); }

/* ------------------------------------------------------------------ sounds */

typedef struct sound {
    i16 *pcm;
    i32 frames, channels, rate;
} sound;

typedef struct play {
    i32 id, sound, loops;
    mg_voice *voice;
    bool paused;
} play;

static sound g_sounds[MAX_SOUNDS];
static play g_plays[MAX_PLAYS];
static i32 g_next_play = 1;

/* A sound decoded whole: its id (above 0), or -1. */
static i32 sound_load(u8 *data, size_t len)
{
    decoder *d = dec_open(data, len);
    size_t cap, n = 0;
    i16 *pcm;
    i32 id = -1, i;
    if (!d) return -1;
    cap = d->frames > 0 ? (size_t)d->frames + 1 : 44100;
    pcm = (i16 *)malloc(cap * (size_t)d->channels * 2);
    while (pcm) {
        i32 r;
        if (n + 4096 > cap) {
            i16 *more = (i16 *)realloc(pcm, (cap * 2 + 4096) * (size_t)d->channels * 2);
            if (!more) break;
            pcm = more;
            cap = cap * 2 + 4096;
        }
        r = dec_read(d, pcm + n * (size_t)d->channels, 4096);
        if (r <= 0) break;
        n += (size_t)r;
    }
    if (pcm && n) {
        mg_lock_acquire(&g_lock);
        for (i = 1; i < MAX_SOUNDS && id < 0; i++)
            if (!g_sounds[i].pcm) id = i;
        if (id > 0) {
            g_sounds[id].pcm = pcm;
            g_sounds[id].frames = (i32)n;
            g_sounds[id].channels = d->channels;
            g_sounds[id].rate = d->rate;
        }
        mg_lock_release(&g_lock);
    }
    if (id < 0) free(pcm);
    dec_close(d);
    return id;
}

N(Audio_soundLoad)
{
    size_t len = 0;
    u8 *d = bytes_of(c->args[0].l, &len);
    c->ret.i = d ? sound_load(d, len) : -1;
}

N(Audio_soundLoadFd)
{
    size_t len = 0;
    u8 *d = bytes_at(c->args[0].i, c->args[1].j, c->args[2].j, &len);
    c->ret.i = d ? sound_load(d, len) : -1;
}

/* A play's voice goes. With the lock held. */
static void end_play(play *p)
{
    if (p->voice) mg_voice_destroy(p->voice);
    memset(p, 0, sizeof(*p));
}

N(Audio_soundUnload)
{
    i32 id = c->args[0].i, i;
    mg_lock_acquire(&g_lock);
    if (id > 0 && id < MAX_SOUNDS && g_sounds[id].pcm) {
        /* A voice is let go under the mixer's lock, which a chunk is mixed under: none of them reads the samples after this. */
        for (i = 0; i < MAX_PLAYS; i++)
            if (g_plays[i].id && g_plays[i].sound == id) end_play(&g_plays[i]);
        free(g_sounds[id].pcm);
        memset(&g_sounds[id], 0, sizeof(g_sounds[id]));
    }
    mg_lock_release(&g_lock);
}

N(Audio_soundDuration)
{
    i32 id = c->args[0].i;
    mg_lock_acquire(&g_lock);
    c->ret.i = id > 0 && id < MAX_SOUNDS && g_sounds[id].pcm && g_sounds[id].rate ? (i32)((i64)g_sounds[id].frames * 1000 / g_sounds[id].rate) : 0;
    mg_lock_release(&g_lock);
}

/* A play's sound has been played to its end: again while loops are left. On the mixer's thread. */
static void play_done(mg_voice *v, void *user)
{
    i32 id = (i32)(mg_addr)user, i;
    mg_lock_acquire(&g_lock);
    for (i = 0; i < MAX_PLAYS; i++) {
        play *p = &g_plays[i];
        sound *s;
        if (p->id != id || p->voice != v) continue;
        s = &g_sounds[p->sound];
        if (p->loops != 0 && s->pcm) {
            if (p->loops > 0) p->loops--;
            mg_voice_enqueue(v, s->pcm, (u32)s->frames * (u32)s->channels * 2);
        } else {
            end_play(p);
        }
        break;
    }
    mg_lock_release(&g_lock);
}

/* soundPlay(int sound, float left, float right, int loops, float rate): a play's id (above 0), or 0. */
N(Audio_soundPlay)
{
    i32 id = c->args[0].i, slot = -1, oldest = -1, i;
    float rate = c->args[4].f > 0 ? c->args[4].f : 1.0f;
    sound *s;
    c->ret.i = 0;
    mg_lock_acquire(&g_lock);
    if (id <= 0 || id >= MAX_SOUNDS || !g_sounds[id].pcm) {
        mg_lock_release(&g_lock);
        return;
    }
    s = &g_sounds[id];
    for (i = 0; i < MAX_PLAYS && slot < 0; i++) {
        if (!g_plays[i].id) slot = i;
        else if (oldest < 0 || g_plays[i].id < g_plays[oldest].id) oldest = i;
    }
    if (slot < 0) {                    /* all playing: the oldest goes */
        slot = oldest;
        end_play(&g_plays[slot]);
    }
    g_plays[slot].voice = mg_voice_create((int)((float)s->rate * rate), s->channels, 16, false);
    if (g_plays[slot].voice) {
        g_plays[slot].id = g_next_play++;
        g_plays[slot].sound = id;
        g_plays[slot].loops = c->args[3].i;
        mg_voice_set_gains(g_plays[slot].voice, c->args[1].f, c->args[2].f);
        mg_voice_set_done(g_plays[slot].voice, play_done, (void *)(mg_addr)g_plays[slot].id);
        mg_voice_enqueue(g_plays[slot].voice, s->pcm, (u32)s->frames * (u32)s->channels * 2);
        mg_voice_set_playing(g_plays[slot].voice, true);
        c->ret.i = g_plays[slot].id;
    }
    mg_lock_release(&g_lock);
}

static play *play_of(i32 id)
{
    i32 i;
    for (i = 0; id > 0 && i < MAX_PLAYS; i++)
        if (g_plays[i].id == id) return &g_plays[i];
    return NULL;
}

N(Audio_voiceStop)
{
    play *p;
    mg_lock_acquire(&g_lock);
    if ((p = play_of(c->args[0].i)) != NULL) end_play(p);
    mg_lock_release(&g_lock);
}

N(Audio_voicePause)
{
    play *p;
    mg_lock_acquire(&g_lock);
    if ((p = play_of(c->args[0].i)) != NULL) {
        p->paused = c->args[1].z != 0;
        mg_voice_set_playing(p->voice, !p->paused);
    }
    mg_lock_release(&g_lock);
}

N(Audio_voiceVolume)
{
    play *p;
    mg_lock_acquire(&g_lock);
    if ((p = play_of(c->args[0].i)) != NULL) mg_voice_set_gains(p->voice, c->args[1].f, c->args[2].f);
    mg_lock_release(&g_lock);
}

N(Audio_voiceRate)
{
    play *p;
    mg_lock_acquire(&g_lock);
    if ((p = play_of(c->args[0].i)) != NULL) mg_voice_set_rate(p->voice, (int)((float)g_sounds[p->sound].rate * (c->args[1].f > 0 ? c->args[1].f : 1.0f)));
    mg_lock_release(&g_lock);
}

N(Audio_voiceLoop)
{
    play *p;
    mg_lock_acquire(&g_lock);
    if ((p = play_of(c->args[0].i)) != NULL) p->loops = c->args[1].i;
    mg_lock_release(&g_lock);
}

N(Audio_pauseAll)
{
    i32 i;
    mg_lock_acquire(&g_lock);
    for (i = 0; i < MAX_PLAYS; i++)
        if (g_plays[i].id) {
            g_plays[i].paused = c->args[0].z != 0;
            mg_voice_set_playing(g_plays[i].voice, !g_plays[i].paused);
        }
    mg_lock_release(&g_lock);
}

static const mg_jhle k_audio[] = {
    { "megler/Audio", "trackOpen", NULL, Audio_trackOpen },
    { "megler/Audio", "trackWrite", NULL, Audio_trackWrite },
    { "megler/Audio", "trackWriteBytes", NULL, Audio_trackWriteBytes },
    { "megler/Audio", "trackWriteFloat", NULL, Audio_trackWriteFloat },
    { "megler/Audio", "trackPlay", NULL, Audio_trackPlay },
    { "megler/Audio", "trackFlush", NULL, Audio_trackFlush },
    { "megler/Audio", "trackVolume", NULL, Audio_trackVolume },
    { "megler/Audio", "trackPosition", NULL, Audio_trackPosition },
    { "megler/Audio", "trackPending", NULL, Audio_trackPending },
    { "megler/Audio", "trackClose", NULL, Audio_trackClose },
    { "megler/Audio", "soundLoad", NULL, Audio_soundLoad },
    { "megler/Audio", "soundLoadFd", NULL, Audio_soundLoadFd },
    { "megler/Audio", "soundUnload", NULL, Audio_soundUnload },
    { "megler/Audio", "soundDuration", NULL, Audio_soundDuration },
    { "megler/Audio", "soundPlay", NULL, Audio_soundPlay },
    { "megler/Audio", "voiceStop", NULL, Audio_voiceStop },
    { "megler/Audio", "voicePause", NULL, Audio_voicePause },
    { "megler/Audio", "voiceVolume", NULL, Audio_voiceVolume },
    { "megler/Audio", "voiceRate", NULL, Audio_voiceRate },
    { "megler/Audio", "voiceLoop", NULL, Audio_voiceLoop },
    { "megler/Audio", "pauseAll", NULL, Audio_pauseAll },
    { "megler/Audio", "decOpen", NULL, Audio_decOpen },
    { "megler/Audio", "decOpenFd", NULL, Audio_decOpenFd },
    { "megler/Audio", "decInfo", NULL, Audio_decInfo },
    { "megler/Audio", "decRead", NULL, Audio_decRead },
    { "megler/Audio", "decSeek", NULL, Audio_decSeek },
    { "megler/Audio", "decClose", NULL, Audio_decClose },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_audio_natives(void) { return k_audio; }
