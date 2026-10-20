/*
 * libmediandk.so: a game's films.
 *
 * Android gives native code a demuxer (AMediaExtractor), which hands out a
 * file's compressed samples one at a time, and decoders (AMediaCodec), which
 * take those and give back pictures or sound; an engine wires the one to the
 * other. Unity plays every VideoClip this way, the picture decoded onto a
 * Surface made from a SurfaceTexture, which it then draws from as an OpenGL
 * "external" texture.
 *
 * Here the file is decoded whole by the shell (meglet_media_host: on a phone,
 * Windows' own H.264 and AAC decoders), and the two halves are a show put on
 * for the engine: the "compressed sample" the extractor hands out is a
 * ticket saying which file and which sample, and the codec that is given a
 * ticket asks the shell for the next decoded picture or the next block of
 * sound. That is all an engine can tell: it never looks inside a sample.
 * What it does notice is timing and order, so those are kept as Android has
 * them: a format-changed notice before the first output, a time on every
 * buffer, an end-of-stream flag that travels from the extractor through the
 * codec's input to its output.
 *
 * A file is copied out of wherever the app keeps it (a range of the APK, by
 * descriptor; a data source that calls back into the app) to a file of its
 * own first: the shell's decoders want a path, and a game's films are small.
 *
 * The picture's way to the screen: releaseOutputBuffer(render) leaves the
 * frame with the SurfaceTexture and its listener is told (on the Handler's
 * thread, as Android does it); the engine then calls updateTexImage on its
 * drawing thread, where the frame goes into an ordinary texture that the
 * external texture is an EGL image of (mg_egl.c).
 */
#include "mg_media.h"

#include <stdio.h>
#include <stdlib.h>

#include "../bionic/mg_bionic.h"
#include "../bionic/mg_fd.h"
#include "../host/mg_host.h"
#include "../jni/mg_jni.h"
#include "../jni/mg_jni_loop.h"
#include "../run/mg_run.h"
#include "mg_audio.h"
#include "mg_egl.h"
#include "mg_ndk.h"

const char *meglet_data_dir(void);

#define AMEDIA_OK 0
#define AMEDIA_ERROR_UNKNOWN (-10000)
#define AMEDIA_ERROR_UNSUPPORTED (-10002)
#define AMEDIA_ERROR_INVALID_OBJECT (-10003)
#define AMEDIA_ERROR_INVALID_PARAMETER (-10004)

#define BUFFER_FLAG_END_OF_STREAM 4u
#define INFO_TRY_AGAIN_LATER (-1)
#define INFO_OUTPUT_FORMAT_CHANGED (-2)

#define AUDIO_BLOCK 1024               /* frames of sound to a sample: an AAC frame's worth */
#define TICKET_MAGIC 0x314D474Du       /* "MGM1" */

/* What the calls say, for the first of them: which call an engine gave up after is the first thing to know. */
static volatile i32 g_said;
#define SAY(...)                                               \
    do {                                                       \
        if (mg_atomic_add(&g_said, 1) <= 160) mg_info("media: " __VA_ARGS__); \
    } while (0)

/* ------------------------------------------------------------ AMediaFormat */

typedef struct fmt_entry {
    char *key;
    char type;                         /* i, l, f, s, b */
    i32 i;
    i64 l;
    float f;
    char *s;
    void *data;
    u32 size;
    struct fmt_entry *next;
} fmt_entry;

typedef struct g_format {
    fmt_entry *first;
    char *text;
} g_format;

static fmt_entry *fmt_find(const g_format *f, const char *key, char type)
{
    fmt_entry *e;
    if (!f || !key) return NULL;
    for (e = f->first; e; e = e->next)
        if (!strcmp(e->key, key) && (!type || e->type == type)) return e;
    return NULL;
}

static fmt_entry *fmt_entry_for(g_format *f, const char *key, char type)
{
    fmt_entry *e = fmt_find(f, key, 0);
    if (!e) {
        e = (fmt_entry *)mg_alloc(sizeof(*e));
        e->key = mg_strdup(key);
        e->next = f->first;
        f->first = e;
    }
    free(e->s);
    free(e->data);
    e->s = NULL;
    e->data = NULL;
    e->type = type;
    return e;
}

static g_format *g_AMediaFormat_new(void) { return (g_format *)mg_alloc(sizeof(g_format)); }

static int g_AMediaFormat_delete(g_format *f)
{
    if (!f) return AMEDIA_OK;
    while (f->first) {
        fmt_entry *e = f->first;
        f->first = e->next;
        free(e->key);
        free(e->s);
        free(e->data);
        free(e);
    }
    free(f->text);
    free(f);
    return AMEDIA_OK;
}

static void fmt_set_i32(g_format *f, const char *key, i32 v) { if (f && key) fmt_entry_for(f, key, 'i')->i = v; }
static void fmt_set_i64(g_format *f, const char *key, i64 v) { if (f && key) fmt_entry_for(f, key, 'l')->l = v; }
static void fmt_set_str(g_format *f, const char *key, const char *v) { if (f && key) fmt_entry_for(f, key, 's')->s = mg_strdup(v ? v : ""); }

static void g_AMediaFormat_setInt32(g_format *f, const char *key, i32 v) { fmt_set_i32(f, key, v); }
static void g_AMediaFormat_setInt64(g_format *f, const char *key, i64 v) { fmt_set_i64(f, key, v); }
/* (A float comes from Android code in an integer register: its bits.) */
static void g_AMediaFormat_setFloat(g_format *f, const char *key, u32 bits)
{
    if (f && key) memcpy(&fmt_entry_for(f, key, 'f')->f, &bits, 4);
}
static void g_AMediaFormat_setString(g_format *f, const char *key, const char *v) { fmt_set_str(f, key, v); }
static void g_AMediaFormat_setBuffer(g_format *f, const char *key, const void *data, u32 size)
{
    fmt_entry *e;
    if (!f || !key) return;
    e = fmt_entry_for(f, key, 'b');
    e->data = mg_alloc(size ? size : 1);
    if (data && size) memcpy(e->data, data, size);
    e->size = size;
}

/* A number is given as whichever kind is asked for: Android's own keys are an int in one version and a float in the next (frame-rate). */
static bool fmt_number(const g_format *f, const char *key, double *out)
{
    const fmt_entry *e = fmt_find(f, key, 0);
    if (!e || (e->type != 'i' && e->type != 'l' && e->type != 'f')) return false;
    *out = e->type == 'i' ? (double)e->i : e->type == 'l' ? (double)e->l : (double)e->f;
    return true;
}
static u8 g_AMediaFormat_getInt32(g_format *f, const char *key, i32 *out)
{
    double v;
    if (!fmt_number(f, key, &v)) return 0;
    if (out) *out = (i32)v;
    return 1;
}
static u8 g_AMediaFormat_getInt64(g_format *f, const char *key, i64 *out)
{
    const fmt_entry *e = fmt_find(f, key, 'l');
    double v;
    if (e) {
        if (out) *out = e->l;
        return 1;
    }
    if (!fmt_number(f, key, &v)) return 0;
    if (out) *out = (i64)v;
    return 1;
}
static u8 g_AMediaFormat_getFloat(g_format *f, const char *key, float *out)
{
    double v;
    float as_float;
    if (!fmt_number(f, key, &v)) return 0;
    as_float = (float)v;
    if (out) memcpy(out, &as_float, 4);
    return 1;
}
static u8 g_AMediaFormat_getString(g_format *f, const char *key, const char **out)
{
    fmt_entry *e = fmt_find(f, key, 's');
    if (e && out) *out = e->s;
    return e != NULL;
}
static u8 g_AMediaFormat_getBuffer(g_format *f, const char *key, void **data, u32 *size)
{
    fmt_entry *e = fmt_find(f, key, 'b');
    if (e && data) *data = e->data;
    if (e && size) *size = e->size;
    return e != NULL;
}

static const char *g_AMediaFormat_toString(g_format *f)
{
    char line[1024];
    size_t n = 0;
    fmt_entry *e;
    if (!f) return "null";
    line[0] = 0;
    for (e = f->first; e && n + 80 < sizeof(line); e = e->next) {
        if (e->type == 'i') n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s: int32(%d)", n ? ", " : "", e->key, (int)e->i);
        else if (e->type == 'l') n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s: int64(%lld)", n ? ", " : "", e->key, (long long)e->l);
        else if (e->type == 'f') n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s: float(%g)", n ? ", " : "", e->key, (double)e->f);
        else if (e->type == 's') n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s: string(%.60s)", n ? ", " : "", e->key, e->s);
        else n += (size_t)snprintf(line + n, sizeof(line) - n, "%s%s: data", n ? ", " : "", e->key);
    }
    free(f->text);
    f->text = mg_strdup(line);
    return f->text;
}

/* ------------------------------------------------------------ the file itself */

/* One opened file: what the shell decodes it with, shared by the extractor that opened it and the codecs it feeds. */
typedef struct g_media {
    volatile i32 refs;
    void *handle;
    meglet_media_info info;
    char *path;                        /* the copy of the file, deleted with the last reference */
    mg_lock lock;                      /* one read of the shell's decoder at a time */
} g_media;

static const meglet_media_host *g_test_host;
void mg_media_set_host(const struct meglet_media_host *h) { g_test_host = h; }

static const meglet_media_host *host(void)
{
    const meglet_run_config *run = mg_run_config();
    if (g_test_host) return g_test_host;
    return run ? &run->media : NULL;
}

static void media_release(g_media *m)
{
    const meglet_media_host *h = host();
    int err = 0;
    if (!m || mg_atomic_add(&m->refs, -1) > 0) return;
    if (m->handle && h && h->close) h->close(h->user, m->handle);
    if (m->path) mg_path_remove(m->path, &err);
    free(m->path);
    free(m);
}

typedef i32 (*source_read)(void *user, i64 offset, void *buffer, u32 size);

/* Copies `size` bytes of a source to a file of its own and has the shell open that. */
static g_media *media_open(source_read read, void *user, i64 size)
{
    static volatile i32 serial;
    const meglet_media_host *h = host();
    const char *dir = meglet_data_dir();
    char *folder, *path;
    mg_file *out;
    u8 *chunk;
    i64 at = 0;
    int err = 0;
    g_media *m;
    if (!h || !h->open) {
        mg_note_once("media: the shell has no decoder for films; the app is told the file cannot be opened");
        return NULL;
    }
    if (size <= 0 || size > ((i64)1 << 31)) {
        mg_warn("media: a file of %lld bytes cannot be played", (long long)size);
        return NULL;
    }
    folder = mg_sprintf("%s/media", dir && dir[0] ? dir : ".");
    mg_path_mkdirs(folder);
    path = mg_sprintf("%s/film-%u-%d.mp4", folder, (unsigned)mg_thread_self_id(), (int)mg_atomic_add(&serial, 1));
    free(folder);
    out = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    if (!out) {
        mg_warn("media: %s cannot be written", path);
        free(path);
        return NULL;
    }
    chunk = (u8 *)mg_alloc(256 * 1024);
    while (at < size) {
        u32 want = (u32)(size - at > 256 * 1024 ? 256 * 1024 : size - at);
        i32 got = read(user, at, chunk, want);
        if (got <= 0 || mg_file_write(out, chunk, (size_t)got, &err) != got) break;
        at += got;
    }
    free(chunk);
    mg_file_close(out);
    if (at != size) {
        mg_warn("media: only %lld of the file's %lld bytes could be read", (long long)at, (long long)size);
        mg_path_remove(path, &err);
        free(path);
        return NULL;
    }
    m = (g_media *)mg_alloc(sizeof(*m));
    m->refs = 1;
    m->path = path;
    mg_lock_init(&m->lock);
    m->handle = h->open(h->user, path, &m->info);
    if (!m->handle) {
        mg_warn("media: the shell's decoder could not open the film (%lld bytes)", (long long)size);
        media_release(m);
        return NULL;
    }
    mg_info("media: a film of %dx%d at %d/%d frames a second, sound %d Hz in %d channels, %lld ms long", m->info.width, m->info.height, m->info.frame_rate_num,
            m->info.frame_rate_den, m->info.sample_rate, m->info.channels, (long long)(m->info.duration_us / 1000));
    return m;
}

/* ---------------------------------------------------------- AMediaDataSource */

typedef struct g_data_source {
    void *userdata;
    mg_addr read_at, get_size, close;
} g_data_source;

static g_data_source *g_AMediaDataSource_new(void) { return (g_data_source *)mg_alloc(sizeof(g_data_source)); }
static void g_AMediaDataSource_delete(g_data_source *s) { free(s); }
static void g_AMediaDataSource_setUserdata(g_data_source *s, void *userdata) { if (s) s->userdata = userdata; }
static void g_AMediaDataSource_setReadAt(g_data_source *s, mg_addr fn) { if (s) s->read_at = fn; }
static void g_AMediaDataSource_setGetSize(g_data_source *s, mg_addr fn) { if (s) s->get_size = fn; }
static void g_AMediaDataSource_setClose(g_data_source *s, mg_addr fn) { if (s) s->close = fn; }

static i32 read_data_source(void *user, i64 offset, void *buffer, u32 size)
{
    g_data_source *s = (g_data_source *)user;
    return MG_GFN(i32 (*)(void *, i64, void *, u32), s->read_at)(s->userdata, offset, buffer, size);
}

/* ----------------------------------------------------------- AMediaExtractor */

typedef struct g_track {
    bool video, selected;
    u64 index, count;                  /* the next sample, and how many there are */
    g_format *format;
} g_track;

typedef struct g_extractor {
    g_media *media;
    g_track tracks[2];
    int ntracks;
} g_extractor;

/* What a sample's bytes are here: which file, which track, which sample. */
typedef struct ticket {
    u32 magic, track;
    u64 index;
    g_media *media;
} ticket;

static i64 sample_time(const g_extractor *ex, const g_track *t, u64 index)
{
    const meglet_media_info *in = &ex->media->info;
    if (t->video) return in->frame_rate_num > 0 ? (i64)(index * 1000000ull * (u64)in->frame_rate_den / (u64)in->frame_rate_num) : (i64)(index * 33333ull);
    return in->sample_rate > 0 ? (i64)(index * AUDIO_BLOCK * 1000000ull / (u64)in->sample_rate) : 0;
}

/* The selected track whose next sample comes first, or -1 when every one of them has run out. */
static int current_track(const g_extractor *ex)
{
    int i, best = -1;
    i64 best_time = 0;
    if (!ex || !ex->media) return -1;
    for (i = 0; i < ex->ntracks; i++) {
        const g_track *t = &ex->tracks[i];
        i64 time;
        if (!t->selected || t->index >= t->count) continue;
        time = sample_time(ex, t, t->index);
        if (best < 0 || time < best_time) {
            best = i;
            best_time = time;
        }
    }
    return best;
}

static g_extractor *g_AMediaExtractor_new(void) { return (g_extractor *)mg_alloc(sizeof(g_extractor)); }

static int g_AMediaExtractor_delete(g_extractor *ex)
{
    int i;
    if (!ex) return AMEDIA_OK;
    for (i = 0; i < ex->ntracks; i++) g_AMediaFormat_delete(ex->tracks[i].format);
    media_release(ex->media);
    free(ex);
    return AMEDIA_OK;
}

static int extractor_opened(g_extractor *ex, g_media *m)
{
    const meglet_media_info *in;
    if (!m) return AMEDIA_ERROR_UNKNOWN;
    ex->media = m;
    in = &m->info;
    if (in->width > 0 && in->height > 0) {
        g_track *t = &ex->tracks[ex->ntracks++];
        double fps = in->frame_rate_num > 0 && in->frame_rate_den > 0 ? (double)in->frame_rate_num / in->frame_rate_den : 30.0;
        t->video = true;
        t->count = (u64)((double)in->duration_us * fps / 1000000.0 + 0.5);
        if (!t->count) t->count = 1;
        t->format = g_AMediaFormat_new();
        fmt_set_str(t->format, "mime", "video/avc");
        fmt_set_i32(t->format, "width", in->width);
        fmt_set_i32(t->format, "height", in->height);
        fmt_set_i64(t->format, "durationUs", in->duration_us);
        fmt_set_i32(t->format, "frame-rate", (i32)(fps + 0.5));
        fmt_set_i32(t->format, "max-input-size", 65536);
    }
    if (in->sample_rate > 0 && in->channels > 0) {
        g_track *t = &ex->tracks[ex->ntracks++];
        t->video = false;
        t->count = (u64)((double)in->duration_us * in->sample_rate / 1000000.0 / AUDIO_BLOCK + 0.5);
        if (!t->count) t->count = 1;
        t->format = g_AMediaFormat_new();
        fmt_set_str(t->format, "mime", "audio/mp4a-latm");
        fmt_set_i32(t->format, "sample-rate", in->sample_rate);
        fmt_set_i32(t->format, "channel-count", in->channels);
        fmt_set_i64(t->format, "durationUs", in->duration_us);
        fmt_set_i32(t->format, "max-input-size", 8192);
    }
    SAY("extractor: %d tracks", ex->ntracks);
    return ex->ntracks ? AMEDIA_OK : AMEDIA_ERROR_UNSUPPORTED;
}

typedef struct fd_range {
    int fd;
    i64 base;
} fd_range;

static i32 read_fd_range(void *user, i64 offset, void *buffer, u32 size)
{
    const fd_range *r = (const fd_range *)user;
    return mg_fd_pread(r->fd, buffer, size, (u64)(r->base + offset));
}

static int g_AMediaExtractor_setDataSourceFd(g_extractor *ex, int fd, i64 offset, i64 length)
{
    fd_range r;
    if (!ex || ex->media) return AMEDIA_ERROR_INVALID_OBJECT;
    SAY("extractor: a file by descriptor %d, %lld bytes from %lld", fd, (long long)length, (long long)offset);
    r.fd = fd;
    r.base = offset;
    return extractor_opened(ex, media_open(read_fd_range, &r, length));
}

static int g_AMediaExtractor_setDataSource(g_extractor *ex, const char *location)
{
    fd_range r;
    i64 size;
    int rc;
    char *host_path;
    if (!ex || ex->media || !location) return AMEDIA_ERROR_INVALID_OBJECT;
    SAY("extractor: a file by name, %s", location);
    host_path = mg_fs_host_path(location);
    r.fd = host_path ? mg_fd_open_host(host_path, MG_O_READ) : -1;
    free(host_path);
    if (r.fd < 0) return AMEDIA_ERROR_UNKNOWN;
    r.base = 0;
    size = mg_fd_seek(r.fd, 0, 2);
    rc = extractor_opened(ex, media_open(read_fd_range, &r, size));
    mg_fd_close(r.fd);
    return rc;
}

static int g_AMediaExtractor_setDataSourceCustom(g_extractor *ex, g_data_source *src)
{
    i64 size;
    if (!ex || ex->media || !src || !src->read_at || !src->get_size) return AMEDIA_ERROR_INVALID_PARAMETER;
    size = MG_GFN(i32 (*)(void *), src->get_size)(src->userdata);
    SAY("extractor: a file through the app's own reader, %lld bytes", (long long)size);
    return extractor_opened(ex, media_open(read_data_source, src, size));
}

static u32 g_AMediaExtractor_getTrackCount(g_extractor *ex) { return ex ? (u32)ex->ntracks : 0; }

/* A copy: the caller deletes what it is given. */
static g_format *g_AMediaExtractor_getTrackFormat(g_extractor *ex, u32 index)
{
    g_format *copy;
    const fmt_entry *e;
    if (!ex || index >= (u32)ex->ntracks) return NULL;
    copy = g_AMediaFormat_new();
    for (e = ex->tracks[index].format->first; e; e = e->next) {
        if (e->type == 'i') fmt_set_i32(copy, e->key, e->i);
        else if (e->type == 'l') fmt_set_i64(copy, e->key, e->l);
        else if (e->type == 's') fmt_set_str(copy, e->key, e->s);
    }
    SAY("extractor: track %u is %s", (unsigned)index, g_AMediaFormat_toString(copy));
    return copy;
}

static int g_AMediaExtractor_selectTrack(g_extractor *ex, u32 index)
{
    if (!ex || index >= (u32)ex->ntracks) return AMEDIA_ERROR_INVALID_PARAMETER;
    ex->tracks[index].selected = true;
    SAY("extractor: track %u selected", (unsigned)index);
    return AMEDIA_OK;
}

static int g_AMediaExtractor_unselectTrack(g_extractor *ex, u32 index)
{
    if (!ex || index >= (u32)ex->ntracks) return AMEDIA_ERROR_INVALID_PARAMETER;
    ex->tracks[index].selected = false;
    return AMEDIA_OK;
}

static i32 g_AMediaExtractor_readSampleData(g_extractor *ex, u8 *buffer, u32 capacity)
{
    int k = current_track(ex);
    ticket t;
    if (k < 0 || !buffer || capacity < sizeof(t)) return -1;
    memset(&t, 0, sizeof(t));
    t.magic = TICKET_MAGIC;
    t.track = (u32)k;
    t.index = ex->tracks[k].index;
    t.media = ex->media;
    memcpy(buffer, &t, sizeof(t));
    return (i32)sizeof(t);
}

static u32 g_AMediaExtractor_getSampleFlags(g_extractor *ex) { return current_track(ex) >= 0 ? 1u : 0u; }   /* every sample a sync sample */
static int g_AMediaExtractor_getSampleTrackIndex(g_extractor *ex) { return current_track(ex); }
static i64 g_AMediaExtractor_getSampleTime(g_extractor *ex)
{
    int k = current_track(ex);
    return k < 0 ? -1 : sample_time(ex, &ex->tracks[k], ex->tracks[k].index);
}

static u8 g_AMediaExtractor_advance(g_extractor *ex)
{
    int k = current_track(ex);
    if (k < 0) return 0;
    ex->tracks[k].index++;
    return current_track(ex) >= 0;
}

static int g_AMediaExtractor_seekTo(g_extractor *ex, i64 time_us, int mode)
{
    const meglet_media_host *h = host();
    int i;
    (void)mode;
    if (!ex || !ex->media) return AMEDIA_ERROR_INVALID_OBJECT;
    if (time_us < 0) time_us = 0;
    SAY("extractor: seek to %lld ms", (long long)(time_us / 1000));
    mg_lock_acquire(&ex->media->lock);
    if (h && h->seek) h->seek(h->user, ex->media->handle, time_us);
    mg_lock_release(&ex->media->lock);
    for (i = 0; i < ex->ntracks; i++) {
        g_track *t = &ex->tracks[i];
        const meglet_media_info *in = &ex->media->info;
        if (t->video) t->index = in->frame_rate_den > 0 ? (u64)time_us * (u64)in->frame_rate_num / (u64)in->frame_rate_den / 1000000ull : 0;
        else t->index = (u64)time_us * (u64)in->sample_rate / 1000000ull / AUDIO_BLOCK;
    }
    return AMEDIA_OK;
}

/* ----------------------------------------------------------- surface textures */

#define SURFACE_TEXTURE "android/graphics/SurfaceTexture"

typedef struct surface_tex {
    mg_jobj *object;
    u32 external;                      /* the app's texture name */
    u32 helper;                        /* the ordinary texture the frames go into */
    void *image;                       /* the EGL image that makes the one the other */
    int helper_w, helper_h;
    mg_lock lock;
    u8 *pending;                       /* the newest frame, RGBA, top row first */
    int w, h;
    i64 pending_us, shown_us;
    u32 pending_seq, shown_seq;
    volatile i32 notices;              /* frame-available calls on their way */
    mg_jobj *listener, *handler;
    mg_nwindow *window;
    bool released;
    struct surface_tex *next;
} surface_tex;

static surface_tex *g_surfaces;
static mg_lock g_surfaces_lock = MG_LOCK_INIT;

static surface_tex *surface_of_window(const void *window)
{
    surface_tex *s;
    mg_lock_acquire(&g_surfaces_lock);
    for (s = g_surfaces; s; s = s->next)
        if (s->window == window && !s->released) break;
    mg_lock_release(&g_surfaces_lock);
    return window ? s : NULL;
}

static void frame_notice(void *arg)
{
    surface_tex *s = (surface_tex *)arg;
    mg_atomic_add(&s->notices, -1);
    if (s->listener && !s->released)
        mg_jni_unref(mg_jni_proxy_call(s->listener, SURFACE_TEXTURE "$OnFrameAvailableListener", "onFrameAvailable", "(L" SURFACE_TEXTURE ";)V", &s->object, 1));
}

/* A decoded frame for a surface: kept until the app's drawing thread takes it (updateTexImage), and the app told. */
static void surface_push(surface_tex *s, const u8 *rgba, int w, int h, i64 time_us)
{
    mg_lock_acquire(&s->lock);
    if (s->w != w || s->h != h || !s->pending) {
        free(s->pending);
        s->pending = (u8 *)mg_alloc((size_t)w * (size_t)h * 4);
        s->w = w;
        s->h = h;
    }
    memcpy(s->pending, rgba, (size_t)w * (size_t)h * 4);
    s->pending_us = time_us;
    s->pending_seq++;
    mg_lock_release(&s->lock);
    /* (One notice on its way is enough: the app takes whatever frame is newest when it looks.) */
    if (s->listener && mg_atomic_add(&s->notices, 1) <= 2) mg_jni_loop_call(s->handler, frame_notice, s);
    else mg_atomic_add(&s->notices, -1);
}

static surface_tex *st_of(const mg_jobj *o) { return o && o->native ? (surface_tex *)o->native : NULL; }

/* new SurfaceTexture(texName), (texName, singleBufferMode), (singleBufferMode). */
static void st_init(mg_jcall *c)
{
    surface_tex *s = (surface_tex *)mg_alloc(sizeof(*s));
    mg_lock_init(&s->lock);
    s->object = c->self;
    s->external = c->sig[1] == 'I' ? (u32)c->args[0].i : 0;
    c->self->native = s;
    c->self->permanent = true;
    mg_lock_acquire(&g_surfaces_lock);
    s->next = g_surfaces;
    g_surfaces = s;
    mg_lock_release(&g_surfaces_lock);
    SAY("a SurfaceTexture on texture %u", (unsigned)s->external);
}

static void st_set_listener(mg_jcall *c)
{
    surface_tex *s = st_of(c->self);
    if (!s) return;
    s->listener = mg_jni_ref(c->args[0].l);
    s->handler = strstr(c->sig, "Landroid/os/Handler;") ? mg_jni_ref(c->args[1].l) : NULL;
    if (!s->listener) SAY("SurfaceTexture: its listener is taken away");
    else SAY("SurfaceTexture: frames are announced to a listener%s", s->handler ? " on a Handler's thread" : "");
}

static void st_attach(mg_jcall *c)
{
    surface_tex *s = st_of(c->self);
    if (s) {
        s->external = (u32)c->args[0].i;
        s->helper_w = s->helper_h = 0;         /* the image is made again for the new name */
    }
}

/* On the app's drawing thread, with its context current: the newest frame becomes what the external texture shows. */
static void st_update(mg_jcall *c)
{
    surface_tex *s = st_of(c->self);
    static volatile i32 said;
    if (!s) return;
    mg_lock_acquire(&s->lock);
    if (s->pending && s->pending_seq != s->shown_seq) {
        bool ok = mg_egl_external_upload(s->external, &s->helper, &s->image, &s->helper_w, &s->helper_h, s->pending, s->w, s->h);
        s->shown_seq = s->pending_seq;
        s->shown_us = s->pending_us;
        if (mg_atomic_add(&said, 1) <= 3) mg_info("media: a frame of %dx%d %s texture %u (at %lld ms)", s->w, s->h, ok ? "is on" : "could NOT be put on", (unsigned)s->external,
                                                 (long long)(s->shown_us / 1000));
    }
    mg_lock_release(&s->lock);
}

/* The matrix that turns texture coordinates into the frame's: its rows are top first, OpenGL's texture rows bottom first. */
static void st_matrix(mg_jcall *c)
{
    static const float flip[16] = { 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1 };
    mg_jobj *a = c->args[0].l;
    if (a && a->kind == MG_J_PRIM_ARRAY && a->u.arr.len >= 16) memcpy(a->u.arr.data, flip, sizeof(flip));
}

static void st_timestamp(mg_jcall *c)
{
    surface_tex *s = st_of(c->self);
    c->ret.j = s ? s->shown_us * 1000 : 0;
}

static void st_release(mg_jcall *c)
{
    surface_tex *s = st_of(c->self);
    if (s) s->released = true;
}

static void st_is_released(mg_jcall *c)
{
    surface_tex *s = st_of(c->self);
    c->ret.z = !s || s->released;
}

static void ret_void(mg_jcall *c) { (void)c; }
static void ret_true(mg_jcall *c) { c->ret.z = 1; }

/* new Surface(SurfaceTexture): the window a decoder is given. */
static void surface_init(mg_jcall *c)
{
    surface_tex *s = strstr(c->sig, "L" SURFACE_TEXTURE ";") ? st_of(c->args[0].l) : NULL;
    mg_nwindow *w = mg_nwindow_create(1, 1);
    c->self->native = w;
    if (s) s->window = w;
}

static const mg_jhle k_java[] = {
    { SURFACE_TEXTURE, "<init>", NULL, st_init },
    { SURFACE_TEXTURE, "setOnFrameAvailableListener", NULL, st_set_listener },
    { SURFACE_TEXTURE, "setDefaultBufferSize", NULL, ret_void },
    { SURFACE_TEXTURE, "attachToGLContext", NULL, st_attach },
    { SURFACE_TEXTURE, "detachFromGLContext", NULL, ret_void },
    { SURFACE_TEXTURE, "updateTexImage", NULL, st_update },
    { SURFACE_TEXTURE, "getTransformMatrix", NULL, st_matrix },
    { SURFACE_TEXTURE, "getTimestamp", NULL, st_timestamp },
    { SURFACE_TEXTURE, "release", NULL, st_release },
    { SURFACE_TEXTURE, "releaseTexImage", NULL, ret_void },
    { SURFACE_TEXTURE, "isReleased", NULL, st_is_released },
    { "android/view/Surface", "<init>", NULL, surface_init },
    { "android/view/Surface", "release", NULL, ret_void },
    { "android/view/Surface", "isValid", NULL, ret_true },
    { NULL, NULL, NULL, NULL }
};

/* The copies of films a run that was ended in the middle of one left behind. */
static void remove_left_over(const char *name, bool is_dir, void *user)
{
    int err = 0;
    if (!is_dir && !strncmp(name, "film-", 5)) {
        char *path = mg_sprintf("%s/%s", (const char *)user, name);
        mg_path_remove(path, &err);
        free(path);
    }
}

void mg_media_install(void)
{
    static bool done;
    const char *dir = meglet_data_dir();
    char *folder;
    int err = 0;
    if (done) return;
    done = true;
    folder = mg_sprintf("%s/media", dir && dir[0] ? dir : ".");
    mg_path_list(folder, remove_left_over, folder, &err);
    free(folder);
    mg_jni_class(SURFACE_TEXTURE, "java/lang/Object");
    mg_jni_class(SURFACE_TEXTURE "$OnFrameAvailableListener", "java/lang/Object");
    mg_jni_register(k_java);
}

/* -------------------------------------------------------------- AMediaCodec */

enum { OUT_SLOTS = 4 };

typedef struct g_buffer_info {
    i32 offset, size;
    i64 time_us;
    u32 flags;
} g_buffer_info;

typedef struct out_slot {
    u8 *data;
    u32 capacity, size;
    i64 time_us;
    bool held;                         /* handed to the app and not yet released */
} out_slot;

typedef struct g_codec {
    bool video;
    mg_lock lock;
    g_media *media;                    /* known from the first ticket */
    surface_tex *surface;
    volatile i32 wake;                 /* goes up when input arrives: what an output wait waits on */
    volatile i32 room;                 /* goes up when a queued sample is decoded: what an input wait waits on */
    u32 pending;                       /* samples queued and not yet decoded */
    u32 decoded;                       /* how many have been, for the log */
    bool input_ended, output_ended, format_said, started;
    out_slot out[OUT_SLOTS];
    u8 input[64];
    g_format *format;
} g_codec;

static g_codec *g_AMediaCodec_createDecoderByType(const char *mime)
{
    g_codec *c;
    bool video = mime && !strncmp(mime, "video/", 6), audio = mime && !strncmp(mime, "audio/", 6);
    SAY("codec: a decoder for %s", mime ? mime : "null");
    if (!video && !audio) return NULL;
    c = (g_codec *)mg_alloc(sizeof(*c));
    c->video = video;
    mg_lock_init(&c->lock);
    return c;
}

static g_codec *g_AMediaCodec_createCodecByName(const char *name)
{
    /* Android's names: "OMX.qcom.video.decoder.avc", "c2.android.aac.decoder"... */
    bool video = name && (strstr(name, "video") || strstr(name, "avc") || strstr(name, "h264") || strstr(name, "hevc") || strstr(name, "vp"));
    if (name && strstr(name, "encoder")) return NULL;
    return g_AMediaCodec_createDecoderByType(video ? "video/avc" : "audio/mp4a-latm");
}

static g_codec *g_AMediaCodec_createEncoderByType(const char *mime)
{
    mg_note_once("media: an encoder was asked for (%s); there is none", mime ? mime : "null");
    return NULL;
}

static int g_AMediaCodec_delete(g_codec *c)
{
    int i;
    if (!c) return AMEDIA_OK;
    for (i = 0; i < OUT_SLOTS; i++) free(c->out[i].data);
    g_AMediaFormat_delete(c->format);
    media_release(c->media);
    free(c);
    return AMEDIA_OK;
}

static int g_AMediaCodec_configure(g_codec *c, const g_format *format, void *window, void *crypto, u32 flags)
{
    (void)format; (void)crypto;
    if (!c) return AMEDIA_ERROR_INVALID_OBJECT;
    if (flags & 1) return AMEDIA_ERROR_UNSUPPORTED;          /* an encoder */
    c->surface = surface_of_window(window);
    SAY("codec: configured, %s", !c->video ? "sound" : c->surface ? "pictures onto a SurfaceTexture" : window ? "pictures onto a window that is no SurfaceTexture" : "pictures into buffers");
    return AMEDIA_OK;
}

static int g_AMediaCodec_start(g_codec *c)
{
    if (!c) return AMEDIA_ERROR_INVALID_OBJECT;
    c->started = true;
    return AMEDIA_OK;
}

static void codec_reset(g_codec *c)
{
    int i;
    mg_lock_acquire(&c->lock);
    c->pending = 0;
    c->input_ended = c->output_ended = false;
    for (i = 0; i < OUT_SLOTS; i++) c->out[i].held = false;
    c->room++;
    mg_lock_release(&c->lock);
    mg_futex_wake(&c->room, true);
}

static int g_AMediaCodec_stop(g_codec *c)
{
    if (!c) return AMEDIA_ERROR_INVALID_OBJECT;
    codec_reset(c);
    c->started = false;
    return AMEDIA_OK;
}

static int g_AMediaCodec_flush(g_codec *c)
{
    if (!c) return AMEDIA_ERROR_INVALID_OBJECT;
    codec_reset(c);
    return AMEDIA_OK;
}

/*
 * How many samples a decoder takes ahead of what has been asked of its
 * output. A real one has a handful of input buffers and is full when they
 * are, and an engine counts on that: Unity feeds a film's sound and pictures
 * in the order the file has them, takes the sound as fast as it is decoded
 * and the pictures as their times come -- it is the picture decoder refusing
 * more that holds the sound back. With no limit all of a film's sound was
 * played in its first seconds, and the pictures skipped ahead after it.
 */
enum { VIDEO_AHEAD = 8, AUDIO_AHEAD = 16 };

/* There is one input buffer, free again the moment it has been queued (nothing is kept of what it held) unless the decoder is full. */
static i32 g_AMediaCodec_dequeueInputBuffer(g_codec *c, i64 timeout_us)
{
    u32 limit;
    i32 seen;
    bool full;
    if (!c || !c->started) return INFO_TRY_AGAIN_LATER;
    limit = c->video ? VIDEO_AHEAD : AUDIO_AHEAD;
    mg_lock_acquire(&c->lock);
    seen = c->room;
    if (c->pending >= limit && timeout_us != 0) {
        u64 wait = timeout_us < 0 || timeout_us > 20000 ? 20000000ull : (u64)timeout_us * 1000ull;
        mg_lock_release(&c->lock);
        mg_gwait(&c->room, seen, wait);
        mg_lock_acquire(&c->lock);
    }
    full = c->pending >= limit;
    mg_lock_release(&c->lock);
    return full ? INFO_TRY_AGAIN_LATER : 0;
}

static u8 *g_AMediaCodec_getInputBuffer(g_codec *c, u32 index, u32 *size)
{
    if (!c || index != 0) return NULL;
    if (size) *size = sizeof(c->input);
    return c->input;
}

static int g_AMediaCodec_queueInputBuffer(g_codec *c, u32 index, i32 offset, u32 size, u64 time_us, u32 flags)
{
    ticket t;
    (void)time_us;
    if (!c || index != 0) return AMEDIA_ERROR_INVALID_PARAMETER;
    mg_lock_acquire(&c->lock);
    if (size >= sizeof(t) && offset >= 0 && (u32)offset + sizeof(t) <= sizeof(c->input)) {
        memcpy(&t, c->input + offset, sizeof(t));
        if (t.magic == TICKET_MAGIC && t.media) {
            if (!c->media) {
                c->media = t.media;
                mg_atomic_add(&t.media->refs, 1);
            }
            c->pending++;
        }
    }
    if (flags & BUFFER_FLAG_END_OF_STREAM) c->input_ended = true;
    c->wake++;
    mg_lock_release(&c->lock);
    mg_futex_wake(&c->wake, true);
    return AMEDIA_OK;
}

static g_format *output_format(g_codec *c)
{
    g_format *f = g_AMediaFormat_new();
    const meglet_media_info *in = c->media ? &c->media->info : NULL;
    if (c->video) {
        fmt_set_str(f, "mime", "video/raw");
        fmt_set_i32(f, "width", in ? in->width : 0);
        fmt_set_i32(f, "height", in ? in->height : 0);
        fmt_set_i32(f, "stride", in ? in->width : 0);
        fmt_set_i32(f, "slice-height", in ? in->height : 0);
        fmt_set_i32(f, "color-format", 0x7F000789);           /* COLOR_FormatSurface */
    } else {
        fmt_set_str(f, "mime", "audio/raw");
        fmt_set_i32(f, "sample-rate", in ? in->sample_rate : 44100);
        fmt_set_i32(f, "channel-count", in ? in->channels : 2);
        fmt_set_i32(f, "pcm-encoding", 2);                    /* 16-bit */
    }
    return f;
}

static g_format *g_AMediaCodec_getOutputFormat(g_codec *c) { return c ? output_format(c) : NULL; }

static i32 g_AMediaCodec_dequeueOutputBuffer(g_codec *c, g_buffer_info *info, i64 timeout_us)
{
    const meglet_media_host *h = host();
    out_slot *slot = NULL;
    i32 index = INFO_TRY_AGAIN_LATER, seen;
    int i, got;
    if (!c || !info) return AMEDIA_ERROR_INVALID_PARAMETER;
    memset(info, 0, sizeof(*info));
    mg_lock_acquire(&c->lock);
    seen = c->wake;
    if (!c->pending && !c->input_ended && timeout_us != 0) {
        /* Nothing to decode yet: wait for input, as long as asked (and never for good: the caller polls). */
        u64 wait = timeout_us < 0 || timeout_us > 20000 ? 20000000ull : (u64)timeout_us * 1000ull;
        mg_lock_release(&c->lock);
        mg_gwait(&c->wake, seen, wait);
        mg_lock_acquire(&c->lock);
    }
    if (c->output_ended || !c->media || (!c->pending && !c->input_ended)) {
        mg_lock_release(&c->lock);
        return INFO_TRY_AGAIN_LATER;
    }
    if (!c->format_said) {
        c->format_said = true;
        mg_lock_release(&c->lock);
        SAY("codec: %s output begins", c->video ? "picture" : "sound");
        if (!c->video) mg_audio_film_begins();
        return INFO_OUTPUT_FORMAT_CHANGED;
    }
    for (i = 0; i < OUT_SLOTS && !slot; i++)
        if (!c->out[i].held) {
            slot = &c->out[i];
            index = i;
        }
    if (!slot) {
        mg_lock_release(&c->lock);
        return INFO_TRY_AGAIN_LATER;           /* the app holds every buffer */
    }
    if (!c->pending) {
        /* The input has ended and everything queued has been decoded. */
        c->output_ended = true;
        slot->held = true;
        slot->size = 0;
        info->flags = BUFFER_FLAG_END_OF_STREAM;
        mg_lock_release(&c->lock);
        SAY("codec: %s output has ended after %u samples", c->video ? "picture" : "sound", (unsigned)c->decoded);
        return index;
    }
    c->pending--;
    c->decoded++;
    c->room++;
    mg_futex_wake(&c->room, true);
    {
        const meglet_media_info *in = &c->media->info;
        u32 need = c->video ? (u32)in->width * (u32)in->height * 4 : (u32)AUDIO_BLOCK * (u32)in->channels * 2;
        i64 time_us = 0;
        if (slot->capacity < need) {
            free(slot->data);
            slot->data = (u8 *)mg_alloc(need);
            slot->capacity = need;
        }
        mg_lock_acquire(&c->media->lock);
        if (c->video) got = h && h->read_video ? h->read_video(h->user, c->media->handle, slot->data, &time_us) : -1;
        else got = h && h->read_audio ? h->read_audio(h->user, c->media->handle, (int16_t *)slot->data, AUDIO_BLOCK, &time_us) : -1;
        mg_lock_release(&c->media->lock);
        if (got <= 0) {
            /* The file ran out before the samples the extractor promised did (their count is an estimate), or could not be decoded. */
            c->output_ended = true;
            c->pending = 0;
            slot->held = true;
            slot->size = 0;
            info->flags = BUFFER_FLAG_END_OF_STREAM;
            mg_lock_release(&c->lock);
            SAY("codec: %s output has ended after %u samples (the decoder %s)", c->video ? "picture" : "sound", (unsigned)c->decoded - 1,
                got < 0 ? "failed" : "reached the end");
            return index;
        }
        slot->size = c->video ? need : (u32)got * (u32)in->channels * 2;
        slot->time_us = time_us;
        slot->held = true;
        info->size = (i32)slot->size;
        info->time_us = time_us;
    }
    mg_lock_release(&c->lock);
    return index;
}

static u8 *g_AMediaCodec_getOutputBuffer(g_codec *c, u32 index, u32 *size)
{
    if (!c || index >= OUT_SLOTS || !c->out[index].held) return NULL;
    if (size) *size = c->out[index].size;
    return c->out[index].data;
}

static int g_AMediaCodec_releaseOutputBuffer(g_codec *c, u32 index, u8 render)
{
    out_slot *slot;
    if (!c || index >= OUT_SLOTS) return AMEDIA_ERROR_INVALID_PARAMETER;
    slot = &c->out[index];
    if (slot->held && render && c->video && c->surface && slot->size && c->media) surface_push(c->surface, slot->data, c->media->info.width, c->media->info.height, slot->time_us);
    slot->held = false;
    return AMEDIA_OK;
}

static int g_AMediaCodec_releaseOutputBufferAtTime(g_codec *c, u32 index, i64 time_ns)
{
    (void)time_ns;
    return g_AMediaCodec_releaseOutputBuffer(c, index, 1);
}

/* ------------------------------------------------------------------ symbols */

#define KEYS(X)                                                                                                                       \
    X(AAC_PROFILE, "aac-profile") X(BIT_RATE, "bitrate") X(CHANNEL_COUNT, "channel-count") X(CHANNEL_MASK, "channel-mask")             \
    X(COLOR_FORMAT, "color-format") X(DURATION, "durationUs") X(FLAC_COMPRESSION_LEVEL, "flac-compression-level")                      \
    X(FRAME_RATE, "frame-rate") X(HEIGHT, "height") X(IS_ADTS, "is-adts") X(IS_AUTOSELECT, "is-autoselect") X(IS_DEFAULT, "is-default") \
    X(IS_FORCED_SUBTITLE, "is-forced-subtitle") X(I_FRAME_INTERVAL, "i-frame-interval") X(LANGUAGE, "language")                         \
    X(MAX_HEIGHT, "max-height") X(MAX_INPUT_SIZE, "max-input-size") X(MAX_WIDTH, "max-width") X(MIME, "mime")                          \
    X(PUSH_BLANK_BUFFERS_ON_STOP, "push-blank-buffers-on-shutdown") X(REPEAT_PREVIOUS_FRAME_AFTER, "repeat-previous-frame-after")     \
    X(ROTATION, "rotation-degrees") X(SAMPLE_RATE, "sample-rate") X(SLICE_HEIGHT, "slice-height") X(STRIDE, "stride") X(WIDTH, "width")  \
    X(BITRATE_MODE, "bitrate-mode") X(COLOR_RANGE, "color-range") X(COLOR_STANDARD, "color-standard") X(COLOR_TRANSFER, "color-transfer") \
    X(LATENCY, "latency") X(LEVEL, "level") X(PRIORITY, "priority") X(PROFILE, "profile") X(TEMPORAL_LAYERING, "ts-schema")             \
    X(TEMPORAL_LAYER_ID, "temporal-layer-id") X(OPERATING_RATE, "operating-rate") X(INTRA_REFRESH_PERIOD, "intra-refresh-period")       \
    X(CSD_0, "csd-0") X(CSD_1, "csd-1") X(CSD_2, "csd-2") X(HDR_STATIC_INFO, "hdr-static-info") X(PCM_ENCODING, "pcm-encoding")

/* Each AMEDIAFORMAT_KEY_ symbol is a variable holding the key's text. */
#define X(name, text) static const char *k_key_##name = text;
KEYS(X)
#undef X

#define N(name, fn) { name, (void *)(fn) }
static const mg_sym g_tab[] = {
#define X(name, text) { "AMEDIAFORMAT_KEY_" #name, (void *)&k_key_##name },
    KEYS(X)
#undef X
    N("AMediaFormat_new", g_AMediaFormat_new), N("AMediaFormat_delete", g_AMediaFormat_delete), N("AMediaFormat_toString", g_AMediaFormat_toString),
    N("AMediaFormat_getInt32", g_AMediaFormat_getInt32), N("AMediaFormat_getInt64", g_AMediaFormat_getInt64), N("AMediaFormat_getFloat", g_AMediaFormat_getFloat),
    N("AMediaFormat_getString", g_AMediaFormat_getString), N("AMediaFormat_getBuffer", g_AMediaFormat_getBuffer),
    N("AMediaFormat_setInt32", g_AMediaFormat_setInt32), N("AMediaFormat_setInt64", g_AMediaFormat_setInt64), N("AMediaFormat_setFloat", g_AMediaFormat_setFloat),
    N("AMediaFormat_setString", g_AMediaFormat_setString), N("AMediaFormat_setBuffer", g_AMediaFormat_setBuffer),
    N("AMediaDataSource_new", g_AMediaDataSource_new), N("AMediaDataSource_delete", g_AMediaDataSource_delete),
    N("AMediaDataSource_setUserdata", g_AMediaDataSource_setUserdata), N("AMediaDataSource_setReadAt", g_AMediaDataSource_setReadAt),
    N("AMediaDataSource_setGetSize", g_AMediaDataSource_setGetSize), N("AMediaDataSource_setClose", g_AMediaDataSource_setClose),
    N("AMediaExtractor_new", g_AMediaExtractor_new), N("AMediaExtractor_delete", g_AMediaExtractor_delete),
    N("AMediaExtractor_setDataSource", g_AMediaExtractor_setDataSource), N("AMediaExtractor_setDataSourceFd", g_AMediaExtractor_setDataSourceFd),
    N("AMediaExtractor_setDataSourceCustom", g_AMediaExtractor_setDataSourceCustom), N("AMediaExtractor_getTrackCount", g_AMediaExtractor_getTrackCount),
    N("AMediaExtractor_getTrackFormat", g_AMediaExtractor_getTrackFormat), N("AMediaExtractor_selectTrack", g_AMediaExtractor_selectTrack),
    N("AMediaExtractor_unselectTrack", g_AMediaExtractor_unselectTrack), N("AMediaExtractor_readSampleData", g_AMediaExtractor_readSampleData),
    N("AMediaExtractor_getSampleFlags", g_AMediaExtractor_getSampleFlags), N("AMediaExtractor_getSampleTrackIndex", g_AMediaExtractor_getSampleTrackIndex),
    N("AMediaExtractor_getSampleTime", g_AMediaExtractor_getSampleTime), N("AMediaExtractor_advance", g_AMediaExtractor_advance),
    N("AMediaExtractor_seekTo", g_AMediaExtractor_seekTo),
    N("AMediaCodec_createDecoderByType", g_AMediaCodec_createDecoderByType), N("AMediaCodec_createCodecByName", g_AMediaCodec_createCodecByName),
    N("AMediaCodec_createEncoderByType", g_AMediaCodec_createEncoderByType), N("AMediaCodec_delete", g_AMediaCodec_delete),
    N("AMediaCodec_configure", g_AMediaCodec_configure), N("AMediaCodec_start", g_AMediaCodec_start), N("AMediaCodec_stop", g_AMediaCodec_stop),
    N("AMediaCodec_flush", g_AMediaCodec_flush), N("AMediaCodec_getInputBuffer", g_AMediaCodec_getInputBuffer),
    N("AMediaCodec_getOutputBuffer", g_AMediaCodec_getOutputBuffer), N("AMediaCodec_dequeueInputBuffer", g_AMediaCodec_dequeueInputBuffer),
    N("AMediaCodec_queueInputBuffer", g_AMediaCodec_queueInputBuffer), N("AMediaCodec_dequeueOutputBuffer", g_AMediaCodec_dequeueOutputBuffer),
    N("AMediaCodec_getOutputFormat", g_AMediaCodec_getOutputFormat), N("AMediaCodec_releaseOutputBuffer", g_AMediaCodec_releaseOutputBuffer),
    N("AMediaCodec_releaseOutputBufferAtTime", g_AMediaCodec_releaseOutputBufferAtTime),
    MG_END
};

const mg_sym *mg_media_table(void) { return g_tab; }
