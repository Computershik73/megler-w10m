/*
 * Decoders for Java apps' android.media.MediaCodec: megler.MediaCodecNative's natives, which the framework's MediaCodec
 * decodes through. The decoding proper is the shell's (megler_codec_host in megler.h: Media Foundation's decoder transforms on a phone). What
 * is here is everything between what an app may hand MediaCodec and the one form the shell is given:
 *
 * - Pictures (H.264, H.265). MediaCodec is fed access units with start codes, and the parameter sets either in the
 *   stream or apart from it, as csd-0 and csd-1 or in a buffer flagged as codec-specific data. Some apps hand over the
 *   length-prefixed units of an MP4 instead, or its avcC or hvcC record as csd-0. Every unit becomes Annex-B, access
 *   unit delimiters are left out (a parameter set put in front of one would end the unit before it), the parameter
 *   sets are kept, and the first unit after a start or a flush gets them in front of its slices when it does not
 *   carry them itself: a decoder that starts afresh needs them before its first slice.
 * - Display order. MediaCodec hands pictures out in display order. A shell's decoder that gives them in decode order
 *   says how far out they can be (megler_codec_host.reorder): that many are held here, sorted by time, and the earliest
 *   goes on when one more comes; at the end of the stream all of them go. A decoder that said it gives them in order
 *   and then does not is caught at its first picture out of place, and pictures are held back from then on.
 * - Sound (AAC, MP3). One packet per queued buffer. AAC's AudioSpecificConfig comes as csd-0 or in a codec-specific
 *   buffer; ADTS headers (which some apps leave on, saying "is-adts") are taken off, and the first one gives the
 *   configuration when nothing else did; with neither, one is made from the format's rate and channels. MP3's rate
 *   and channels are its first frame's when the format said none or said them wrong.
 *
 * A frame handed to Java (nextFrame) is the shell's picture, kept until freeFrame; renderFrame turns it into the
 * Bitmap that MediaCodec then draws into the app's Surface. Sound is 16-bit, one or two channels.
 */
#include "mg_java.h"

#include <stdlib.h>
#include <string.h>

#include "../dvm/mg_dvm.h"
#include "../host/mg_host.h"
#include "mg_java_gfx.h"

#define N(name) static void name(mg_jcall *c)

enum { FLAG_CONFIG = 2, FLAG_EOS = 4 };

/* The shell's decoders: set when the app starts, before any of its code runs. */
static megler_codec_host g_host;

void mg_java_media_set_host(const megler_codec_host *host)
{
    if (host) g_host = *host;
    else memset(&g_host, 0, sizeof(g_host));
}

static const char *kind_name(int kind)
{
    return kind == MEGLER_CODEC_H264 ? "H.264" : kind == MEGLER_CODEC_H265 ? "H.265" : kind == MEGLER_CODEC_AAC ? "AAC" : kind == MEGLER_CODEC_MP3 ? "MP3" : "?";
}

/* MIME types are compared as Android compares them, without regard to case. */
static bool same_type(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) != *b) return false;
    return !*a && !*b;
}

static int kind_of(const char *mime)
{
    if (!mime) return 0;
    if (same_type(mime, "video/avc")) return MEGLER_CODEC_H264;
    if (same_type(mime, "video/hevc")) return MEGLER_CODEC_H265;
    if (same_type(mime, "audio/mp4a-latm")) return MEGLER_CODEC_AAC;
    if (same_type(mime, "audio/mpeg")) return MEGLER_CODEC_MP3;
    return 0;
}

typedef struct blob {
    u8 *d;
    size_t n, cap;
} blob;

static void blob_add(blob *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->d = (u8 *)mg_realloc(b->d, b->cap);
    }
    if (n) memcpy(b->d + b->n, d, n);
    b->n += n;
}

static void blob_set(blob *b, const void *d, size_t n)
{
    b->n = 0;
    blob_add(b, d, n);
}

static void blob_free(blob *b)
{
    free(b->d);
    memset(b, 0, sizeof(*b));
}

struct mg_mframe {
    void *picture;                     /* the shell's */
    megler_codec_picture p;
    struct mg_mframe *next;
};

struct mg_mcodec {
    int kind;
    mg_lock lock;                      /* the shell's calls for the decoder, one at a time, and everything below */
    void *dec;                         /* the shell's decoder; NULL until it is opened */
    i32 width, height;                 /* as configured, then the latest picture's */
    i32 asked_rate, asked_channels;    /* sound: as configured */
    i32 rate, channels;                /* sound: what the decoder was opened for */
    /* Pictures: the parameter sets, one NAL unit each without its start code; and whether the decoder needs them next. */
    blob vps, sps, pps;
    bool fresh;
    /* AAC: the AudioSpecificConfig the decoder was opened with, and whether it is the app's (else made here or taken from ADTS). */
    blob asc;
    bool asc_given;
    /* Pictures out of the decoder, sorted by time, and those that go to Java in this order. */
    mg_mframe *sorted, *ready, **ready_tail;
    i32 nsorted;
    /*
     * How many are held back besides what the shell says (megler_codec_host.reorder): learnt when a picture comes out earlier
     * than one already gone on, since the last flush, the time of which is `gone`.
     */
    i32 learnt;
    i64 gone;
    bool any_gone;
    blob unit;                         /* the unit being put together for the decoder */
    i64 pushed, pictures, blocks, failed;
};

/* ------------------------------------------------------------- pictures */

static void frame_free(mg_mframe *f)
{
    if (!f) return;
    if (f->picture && g_host.release_picture) g_host.release_picture(g_host.user, f->picture);
    free(f);
}

static void drop_frames(mg_mcodec *c)
{
    mg_mframe *f, *next;
    for (f = c->sorted; f; f = next) {
        next = f->next;
        frame_free(f);
    }
    for (f = c->ready; f; f = next) {
        next = f->next;
        frame_free(f);
    }
    c->sorted = c->ready = NULL;
    c->ready_tail = &c->ready;
    c->nsorted = 0;
}

/*
 * Everything the decoder has ready, sorted in; the earliest go on to Java while more are held than the decoder can be out
 * of order by (all of them, at the end of the stream). How far that is, is the shell's to say; but a decoder that says
 * "in display order" may not be for every stream (a Media Foundation decoder in low-latency mode trusts the stream to
 * say how far its pictures are reordered), so a picture that comes out after a later one has gone on is told in the log,
 * goes on as it is, and from then on four are held back, and twice as many each time it happens again, up to sixteen.
 */
static void pull_pictures(mg_mcodec *c, bool all)
{
    i32 keep;
    for (;;) {
        mg_mframe *f, **p;
        megler_codec_picture pic;
        void *h;
        if (!c->dec || !g_host.pull_picture) break;
        memset(&pic, 0, sizeof(pic));
        h = g_host.pull_picture(g_host.user, c->dec, &pic);
        if (!h) break;
        if (pic.width <= 0 || pic.height <= 0 || !pic.data || (pic.format == MEGLER_CODEC_NV12 && !pic.chroma) ||
            (pic.format != MEGLER_CODEC_NV12 && pic.format != MEGLER_CODEC_RGBA)) {
            if (g_host.release_picture) g_host.release_picture(g_host.user, h);
            c->failed++;
            mg_warn("media codec: the shell's %s decoder gave a picture that cannot be shown (format %d, %dx%d)", kind_name(c->kind), pic.format, pic.width,
                    pic.height);
            continue;
        }
        if (c->any_gone && pic.time_us < c->gone && c->learnt < 16) {
            c->learnt = c->learnt ? c->learnt * 2 : 4;
            mg_warn("media codec: the shell's %s decoder gave a picture of %lld ms after one of %lld ms: %d pictures are held back from now on to put them "
                    "in order", kind_name(c->kind), (long long)(pic.time_us / 1000), (long long)(c->gone / 1000), (int)(g_host.reorder + c->learnt));
            /* (What is out already does not count again: the pictures decoded before this one are as late as it is.) */
            c->any_gone = false;
        }
        f = (mg_mframe *)mg_alloc(sizeof(*f));
        f->picture = h;
        f->p = pic;
        for (p = &c->sorted; *p && (*p)->p.time_us <= pic.time_us; p = &(*p)->next) {}
        f->next = *p;
        *p = f;
        c->nsorted++;
        if (!c->pictures++) mg_info("media codec: the first %s picture, %dx%d %s, at %lld ms", kind_name(c->kind), pic.width, pic.height,
                                   pic.format == MEGLER_CODEC_NV12 ? "NV12" : "RGBA", (long long)(pic.time_us / 1000));
    }
    keep = all ? 0 : g_host.reorder + c->learnt;
    while (c->nsorted > keep) {
        mg_mframe *f = c->sorted;
        c->sorted = f->next;
        c->nsorted--;
        if (!c->any_gone || f->p.time_us > c->gone) c->gone = f->p.time_us;
        c->any_gone = true;
        f->next = NULL;
        *c->ready_tail = f;
        c->ready_tail = &f->next;
    }
}

static int nal_type(const mg_mcodec *c, const u8 *n) { return c->kind == MEGLER_CODEC_H264 ? (n[0] & 0x1f) : ((n[0] >> 1) & 0x3f); }

static bool starts_with_code(const u8 *d, size_t n) { return n >= 3 && !d[0] && !d[1] && (d[2] == 1 || (n >= 4 && !d[2] && d[3] == 1)); }

typedef void (*nal_fn)(mg_mcodec *c, const u8 *nal, size_t n, void *arg);

/* The NAL units of an avcC or hvcC record (MP4's form of the parameter sets), when that is what `d` is. */
static bool each_record_nal(mg_mcodec *c, const u8 *d, size_t n, nal_fn fn, void *arg)
{
    size_t i;
    int arrays, a, k;
    if (n < 7 || d[0] != 1) return false;
    if (c->kind == MEGLER_CODEC_H264) {
        int sps = d[5] & 0x1f;
        for (i = 6, k = 0; k < sps; k++) {
            size_t len;
            if (i + 2 > n || (len = (size_t)d[i] << 8 | d[i + 1]) > n - i - 2) return true;
            fn(c, d + i + 2, len, arg);
            i += 2 + len;
        }
        if (i >= n) return true;
        for (a = d[i++], k = 0; k < a; k++) {
            size_t len;
            if (i + 2 > n || (len = (size_t)d[i] << 8 | d[i + 1]) > n - i - 2) return true;
            fn(c, d + i + 2, len, arg);
            i += 2 + len;
        }
        return true;
    }
    if (n < 23) return false;
    for (arrays = d[22], i = 23, a = 0; a < arrays; a++) {
        int count;
        if (i + 3 > n) return true;
        count = d[i + 1] << 8 | d[i + 2];
        for (i += 3, k = 0; k < count; k++) {
            size_t len;
            if (i + 2 > n || (len = (size_t)d[i] << 8 | d[i + 1]) > n - i - 2) return true;
            fn(c, d + i + 2, len, arg);
            i += 2 + len;
        }
    }
    return true;
}

/* Each NAL unit of `d` to fn: Annex-B (start codes) or the length-prefixed form, four bytes of length before each. */
static void each_nal(mg_mcodec *c, const u8 *d, size_t n, nal_fn fn, void *arg)
{
    size_t i = 0, start = (size_t)-1;
    if (!starts_with_code(d, n)) {
        while (i + 4 <= n) {
            u32 len = (u32)d[i] << 24 | (u32)d[i + 1] << 16 | (u32)d[i + 2] << 8 | d[i + 3];
            i += 4;
            if (len == 0 || len > n - i) break;
            fn(c, d + i, len, arg);
            i += len;
        }
        return;
    }
    while (i + 3 <= n) {
        if (!d[i] && !d[i + 1] && d[i + 2] == 1) {
            if (start != (size_t)-1) {
                size_t end = i;
                while (end > start && !d[end - 1]) end--; /* the zero before a four-byte start code */
                if (end > start) fn(c, d + start, end - start, arg);
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != (size_t)-1 && start < n) fn(c, d + start, n - start, arg);
}

enum { HAS_VPS = 1, HAS_SPS = 2, HAS_PPS = 4 };

typedef struct unit_walk {
    blob *out;
    int params, slices;
} unit_walk;

static void put_nal(blob *out, const u8 *nal, size_t n)
{
    static const u8 code[4] = { 0, 0, 0, 1 };
    blob_add(out, code, 4);
    blob_add(out, nal, n);
}

/* One NAL unit of the app's: a parameter set is kept (and passed on in its place), a delimiter dropped, the rest passed on. */
static void take_nal(mg_mcodec *c, const u8 *nal, size_t n, void *arg)
{
    unit_walk *w = (unit_walk *)arg;
    int t;
    if (!n) return;
    t = nal_type(c, nal);
    if (c->kind == MEGLER_CODEC_H264) {
        if (t == 9) return;
        if (t == 7) {
            blob_set(&c->sps, nal, n);
            w->params |= HAS_SPS;
        } else if (t == 8) {
            blob_set(&c->pps, nal, n);
            w->params |= HAS_PPS;
        } else if (t >= 1 && t <= 5) {
            w->slices++;
        }
    } else {
        if (t == 35) return;
        if (t == 32) {
            blob_set(&c->vps, nal, n);
            w->params |= HAS_VPS;
        } else if (t == 33) {
            blob_set(&c->sps, nal, n);
            w->params |= HAS_SPS;
        } else if (t == 34) {
            blob_set(&c->pps, nal, n);
            w->params |= HAS_PPS;
        } else if (t < 32) {
            w->slices++;
        }
    }
    if (w->out) put_nal(w->out, nal, n);
}

/* The parameter sets known, in Annex-B, after what `out` holds. */
static void put_params(mg_mcodec *c, blob *out)
{
    if (c->kind == MEGLER_CODEC_H265 && c->vps.n) put_nal(out, c->vps.d, c->vps.n);
    if (c->sps.n) put_nal(out, c->sps.d, c->sps.n);
    if (c->pps.n) put_nal(out, c->pps.d, c->pps.n);
}

static int params_needed(const mg_mcodec *c) { return c->kind == MEGLER_CODEC_H265 ? HAS_VPS | HAS_SPS | HAS_PPS : HAS_SPS | HAS_PPS; }

static bool video_open(mg_mcodec *c)
{
    megler_codec_format f;
    blob config;
    if (c->dec) return true;
    if (!g_host.open) return false;
    memset(&config, 0, sizeof(config));
    put_params(c, &config);
    memset(&f, 0, sizeof(f));
    f.width = c->width;
    f.height = c->height;
    f.config = config.d;
    f.config_size = config.n;
    c->dec = g_host.open(g_host.user, c->kind, &f);
    blob_free(&config);
    c->fresh = true;
    if (c->dec) mg_info("media codec: an %s decoder of the shell's, for %dx%d%s", kind_name(c->kind), c->width, c->height, c->sps.n ? "" : " (no parameter sets yet)");
    else mg_warn("media codec: the shell would not make an %s decoder for %dx%d", kind_name(c->kind), c->width, c->height);
    return c->dec != NULL;
}

static void push(mg_mcodec *c, const u8 *d, size_t n, i64 time_us)
{
    if (!c->dec || !g_host.push) return;
    c->pushed++;
    if (!g_host.push(g_host.user, c->dec, d, n, time_us)) {
        if (++c->failed <= 3) mg_warn("media codec: the shell's %s decoder failed on a sample of %u bytes at %lld ms", kind_name(c->kind), (unsigned)n,
                                      (long long)(time_us / 1000));
    }
}

static void video_decode(mg_mcodec *c, const u8 *d, size_t n, i64 time_us, i32 flags)
{
    unit_walk w;
    memset(&w, 0, sizeof(w));
    c->unit.n = 0;
    w.out = &c->unit;
    if (n && !((flags & FLAG_CONFIG) && each_record_nal(c, d, n, take_nal, &w))) each_nal(c, d, n, take_nal, &w);
    /* Parameter sets the app sends as such go in front of the next unit, whatever came before. */
    if ((flags & FLAG_CONFIG) && w.params) c->fresh = true;
    if (w.slices && !(flags & FLAG_CONFIG) && video_open(c)) {
        if (c->fresh && (w.params & params_needed(c)) != params_needed(c) && c->sps.n) {
            /* The parameter sets in front, the unit after them: a fresh decoder sees them before its first slice. */
            blob whole;
            memset(&whole, 0, sizeof(whole));
            put_params(c, &whole);
            blob_add(&whole, c->unit.d, c->unit.n);
            push(c, whole.d, whole.n, time_us);
            blob_free(&whole);
        } else {
            push(c, c->unit.d, c->unit.n, time_us);
        }
        c->fresh = false;
        pull_pictures(c, false);
    }
    if ((flags & FLAG_EOS) && c->dec) {
        if (g_host.drain) g_host.drain(g_host.user, c->dec);
        pull_pictures(c, true);
    }
}

/* ---------------------------------------------------------------- sound */

static const i32 k_aac_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

typedef struct bits {
    const u8 *d;
    size_t n, at;
} bits;

static u32 read_bits(bits *b, int count)
{
    u32 v = 0;
    while (count--) {
        u32 bit = b->at / 8 < b->n ? (b->d[b->at / 8] >> (7 - b->at % 8)) & 1 : 0;
        v = v << 1 | bit;
        b->at++;
    }
    return v;
}

/* The rate and channels an AudioSpecificConfig says (of the core stream: HE-AAC's own rate is the decoder's to find). */
static bool asc_parse(const u8 *d, size_t n, i32 *rate, i32 *channels)
{
    bits b = { d, n, 0 };
    u32 aot, sfi, chan;
    if (n < 2) return false;
    aot = read_bits(&b, 5);
    if (aot == 31) aot = 32 + read_bits(&b, 6);
    sfi = read_bits(&b, 4);
    *rate = sfi == 15 ? (i32)read_bits(&b, 24) : sfi < 13 ? k_aac_rates[sfi] : 0;
    chan = read_bits(&b, 4);
    *channels = chan == 7 ? 8 : (i32)chan;
    return aot != 0 && *rate > 0;
}

/* An AudioSpecificConfig of AAC LC (or of the object type given) at the nearest rate the format has an index for. */
static void asc_make(blob *out, int aot, i32 rate, i32 channels)
{
    int sfi = 4, i;
    u8 b[2];
    for (i = 0; i < 13; i++)
        if (abs(k_aac_rates[i] - rate) < abs(k_aac_rates[sfi] - rate)) sfi = i;
    if (channels < 1 || channels > 7) channels = 2;
    b[0] = (u8)(aot << 3 | sfi >> 1);
    b[1] = (u8)((sfi & 1) << 7 | channels << 3);
    blob_set(out, b, 2);
}

/* An ADTS header's length (7 or 9 bytes) and its frame's, when `d` starts with one. */
static bool adts_header(const u8 *d, size_t n, size_t *header, size_t *frame)
{
    if (n < 7 || d[0] != 0xFF || (d[1] & 0xF6) != 0xF0) return false;
    *header = (d[1] & 1) ? 7 : 9;
    *frame = (size_t)(d[3] & 3) << 11 | (size_t)d[4] << 3 | d[5] >> 5;
    return *frame > *header && *frame <= n;
}

/* An MPEG audio frame header's rate and channels. */
static bool mp3_header(const u8 *d, size_t n, i32 *rate, i32 *channels)
{
    static const i32 base[3] = { 44100, 48000, 32000 };
    int version, layer, sri;
    if (n < 4 || d[0] != 0xFF || (d[1] & 0xE0) != 0xE0) return false;
    version = (d[1] >> 3) & 3; /* 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5 */
    layer = (d[1] >> 1) & 3;
    sri = (d[2] >> 2) & 3;
    if (version == 1 || layer == 0 || sri == 3) return false;
    *rate = base[sri] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    *channels = (d[3] >> 6) == 3 ? 1 : 2;
    return true;
}

/* A decoder for this rate and channels (and AAC's configuration), the old one closed. */
static bool audio_open(mg_mcodec *c, i32 rate, i32 channels)
{
    megler_codec_format f;
    if (c->dec && g_host.close) g_host.close(g_host.user, c->dec);
    c->dec = NULL;
    if (!g_host.open) return false;
    if (rate <= 0) rate = 44100;
    if (channels <= 0) channels = 2;
    if (c->kind == MEGLER_CODEC_AAC) {
        i32 r = 0, ch = 0;
        if (!c->asc.n) asc_make(&c->asc, 2, rate, channels);
        if (asc_parse(c->asc.d, c->asc.n, &r, &ch)) {
            rate = r;
            if (ch > 0) channels = ch;
        }
    }
    memset(&f, 0, sizeof(f));
    f.sample_rate = rate;
    f.channels = channels;
    if (c->kind == MEGLER_CODEC_AAC) {
        f.config = c->asc.d;
        f.config_size = c->asc.n;
    }
    c->dec = g_host.open(g_host.user, c->kind, &f);
    c->rate = rate;
    c->channels = channels;
    if (c->dec) mg_info("media codec: an %s decoder of the shell's, %d Hz, %d channels", kind_name(c->kind), rate, channels);
    else mg_warn("media codec: the shell would not make an %s decoder for %d Hz, %d channels", kind_name(c->kind), rate, channels);
    return c->dec != NULL;
}

static void audio_decode(mg_mcodec *c, const u8 *d, size_t n, i64 time_us, i32 flags)
{
    if (c->kind == MEGLER_CODEC_AAC) {
        size_t header, frame;
        if (flags & FLAG_CONFIG) {
            /* An AudioSpecificConfig of the app's: a decoder for it, unless it is the one there is. */
            if (n >= 2 && (!c->dec || !c->asc_given || c->asc.n != n || memcmp(c->asc.d, d, n))) {
                blob_set(&c->asc, d, n);
                c->asc_given = true;
                audio_open(c, c->asked_rate, c->asked_channels);
            }
        } else if (adts_header(d, n, &header, &frame)) {
            /* ADTS: each frame without its header; the first header says what the stream is when the app did not. */
            if (!c->asc_given) {
                u8 asc[2];
                int profile = d[2] >> 6, sfi = (d[2] >> 2) & 0xF, chan = (d[2] & 1) << 2 | d[3] >> 6;
                asc[0] = (u8)((profile + 1) << 3 | sfi >> 1);
                asc[1] = (u8)((sfi & 1) << 7 | chan << 3);
                if (!c->dec || c->asc.n != 2 || memcmp(c->asc.d, asc, 2)) {
                    blob_set(&c->asc, asc, 2);
                    audio_open(c, 0, 0);
                }
            }
            while (adts_header(d, n, &header, &frame)) {
                push(c, d + header, frame - header, time_us);
                if (c->rate > 0) time_us += (i64)1024 * 1000000 / c->rate;
                d += frame;
                n -= frame;
            }
        } else if (n) {
            if (!c->dec) audio_open(c, c->asked_rate, c->asked_channels);
            push(c, d, n, time_us);
        }
    } else if (n && !(flags & FLAG_CONFIG)) {
        i32 rate, channels;
        if (mp3_header(d, n, &rate, &channels) && (!c->dec || rate != c->rate || channels != c->channels)) audio_open(c, rate, channels);
        else if (!c->dec) audio_open(c, c->asked_rate, c->asked_channels);
        push(c, d, n, time_us);
    }
    if ((flags & FLAG_EOS) && c->dec && g_host.drain) g_host.drain(g_host.user, c->dec);
}

/* ------------------------------------------------------------ the codec */

bool mg_mcodec_supported(const char *mime)
{
    int kind = kind_of(mime);
    return kind && g_host.supports && g_host.open && g_host.push && g_host.supports(g_host.user, kind);
}

mg_mcodec *mg_mcodec_create(const char *mime)
{
    int kind = kind_of(mime);
    mg_mcodec *c;
    if (!kind || !g_host.open) return NULL;
    c = (mg_mcodec *)mg_alloc(sizeof(*c));
    c->kind = kind;
    mg_lock_init(&c->lock);
    c->ready_tail = &c->ready;
    return c;
}

static bool is_video(const mg_mcodec *c) { return c->kind == MEGLER_CODEC_H264 || c->kind == MEGLER_CODEC_H265; }

bool mg_mcodec_configure(mg_mcodec *c, i32 width, i32 height, i32 rate, i32 channels, const u8 *const csd[3], const size_t csd_size[3])
{
    bool ok;
    int i;
    if (!c) return false;
    mg_lock_acquire(&c->lock);
    if (is_video(c)) {
        unit_walk w;
        memset(&w, 0, sizeof(w));
        c->width = width > 0 ? width : 0;
        c->height = height > 0 ? height : 0;
        for (i = 0; i < 3; i++)
            if (csd[i] && csd_size[i] && !each_record_nal(c, csd[i], csd_size[i], take_nal, &w)) each_nal(c, csd[i], csd_size[i], take_nal, &w);
        ok = video_open(c);
    } else {
        c->asked_rate = rate;
        c->asked_channels = channels;
        if (c->kind == MEGLER_CODEC_AAC && csd[0] && csd_size[0] >= 2) {
            blob_set(&c->asc, csd[0], csd_size[0]);
            c->asc_given = true;
        }
        ok = audio_open(c, rate, channels);
    }
    mg_lock_release(&c->lock);
    return ok;
}

void mg_mcodec_decode(mg_mcodec *c, const u8 *data, size_t size, i64 time_us, i32 flags)
{
    if (!c) return;
    mg_lock_acquire(&c->lock);
    if (is_video(c)) video_decode(c, data, data ? size : 0, time_us, flags);
    else audio_decode(c, data, data ? size : 0, time_us, flags);
    mg_lock_release(&c->lock);
}

mg_mframe *mg_mcodec_next_frame(mg_mcodec *c, i64 info[3])
{
    mg_mframe *f;
    if (!c) return NULL;
    mg_lock_acquire(&c->lock);
    pull_pictures(c, false);
    f = c->ready;
    if (f) {
        c->ready = f->next;
        if (!c->ready) c->ready_tail = &c->ready;
        f->next = NULL;
        c->width = f->p.width;
        c->height = f->p.height;
    }
    mg_lock_release(&c->lock);
    if (f && info) {
        info[0] = f->p.time_us;
        info[1] = f->p.width;
        info[2] = f->p.height;
    }
    return f;
}

/* The next block of sound, copied to where `place` says (given its size in bytes); NULL when there is none. */
static void *next_pcm(mg_mcodec *c, i64 info[3], void *(*place)(size_t bytes, void *arg), void *arg)
{
    megler_codec_sound s;
    void *out = NULL;
    mg_lock_acquire(&c->lock);
    memset(&s, 0, sizeof(s));
    if (c->dec && g_host.pull_sound && g_host.pull_sound(g_host.user, c->dec, &s) && s.samples && s.frames > 0 && s.channels > 0) {
        size_t bytes = (size_t)s.frames * (size_t)s.channels * 2;
        out = place(bytes, arg);
        if (out) memcpy(out, s.samples, bytes);
        if (!c->blocks++) mg_info("media codec: the first %s sound, %d frames at %d Hz in %d channels, at %lld ms", kind_name(c->kind), s.frames, s.sample_rate,
                                 s.channels, (long long)(s.time_us / 1000));
        if (info) {
            info[0] = s.time_us;
            info[1] = s.sample_rate;
            info[2] = s.channels;
        }
    }
    mg_lock_release(&c->lock);
    return out;
}

static void *place_malloc(size_t bytes, void *arg)
{
    *(size_t *)arg = bytes;
    return malloc(bytes ? bytes : 1);
}

u8 *mg_mcodec_next_pcm(mg_mcodec *c, i64 info[3], size_t *bytes)
{
    size_t n = 0;
    u8 *out = c ? (u8 *)next_pcm(c, info, place_malloc, &n) : NULL;
    if (bytes) *bytes = out ? n : 0;
    return out;
}

const megler_codec_picture *mg_mcodec_frame_picture(const mg_mframe *f) { return f ? &f->p : NULL; }

void mg_mcodec_free_frame(mg_mframe *f) { frame_free(f); }

void mg_mcodec_flush(mg_mcodec *c)
{
    if (!c) return;
    mg_lock_acquire(&c->lock);
    if (c->dec && g_host.flush) g_host.flush(g_host.user, c->dec);
    drop_frames(c);
    c->fresh = true;
    c->any_gone = false;
    mg_lock_release(&c->lock);
}

void mg_mcodec_destroy(mg_mcodec *c)
{
    if (!c) return;
    mg_lock_acquire(&c->lock);
    drop_frames(c);
    if (c->dec && g_host.close) g_host.close(g_host.user, c->dec);
    c->dec = NULL;
    mg_lock_release(&c->lock);
    mg_info("media codec: an %s decoder closed after %lld samples, %lld pictures, %lld blocks of sound, %lld failures", kind_name(c->kind), (long long)c->pushed,
            (long long)c->pictures, (long long)c->blocks, (long long)c->failed);
    blob_free(&c->vps);
    blob_free(&c->sps);
    blob_free(&c->pps);
    blob_free(&c->asc);
    blob_free(&c->unit);
    free(c);
}

i32 mg_mcodec_size(mg_mcodec *c)
{
    i32 s;
    if (!c) return 0;
    mg_lock_acquire(&c->lock);
    s = c->width << 16 | (c->height & 0xffff);
    mg_lock_release(&c->lock);
    return s;
}

static u8 clamp8(i32 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

/*
 * NV12 to the bitmaps' B, G, R, A: Y'CbCr to R'G'B' with the factors in 256ths, BT.601 or BT.709, video range (luma 16 to
 * 235) or full. What the stream does not say is what Android takes: BT.601 under 720 lines, BT.709 from there. Each
 * luma's term (with the rounding) and each chroma's come from tables of 256, a block of 2 x 2 pixels shares its chroma's,
 * and a sum is brought into 0..255 by a table as well, as (sum >> 8) + 384 can only be 95 to 931. The pixels are the same
 * as with the factors multiplied out for every pixel, which took 13.0 ms for a picture of 1920 x 1080 on the PC this was
 * written on, built as the app is (/O2); this takes 3.8 ms.
 */
static inline void put_bgra(u8 *d, const u8 *clip, i32 l, i32 r, i32 g, i32 b)
{
    u32 px = (u32)clip[(l + b) >> 8] | (u32)clip[(l + g) >> 8] << 8 | (u32)clip[(l + r) >> 8] << 16 | 0xFF000000u;
    memcpy(d, &px, 4);
}

static void nv12_to_bgra(const megler_codec_picture *p, u8 *dst, i32 w, i32 h, i32 stride)
{
    bool b709 = p->matrix == MEGLER_CODEC_BT709 || (p->matrix != MEGLER_CODEC_BT601 && p->height >= 720);
    i32 ky, rv, gu, gv, bu, y0, i, x, y;
    i32 lt[256], rt[256], gut[256], gvt[256], bt[256];
    u8 clip[1024];
    if (p->full_range) {
        ky = 256, y0 = 0;
        rv = b709 ? 403 : 359, gu = b709 ? 48 : 88, gv = b709 ? 120 : 183, bu = b709 ? 475 : 454;
    } else {
        ky = 298, y0 = 16;
        rv = b709 ? 459 : 409, gu = b709 ? 55 : 100, gv = b709 ? 136 : 208, bu = b709 ? 541 : 516;
    }
    for (i = 0; i < 256; i++) {
        lt[i] = ky * (i - y0) + 128 + (384 << 8);
        rt[i] = rv * (i - 128);
        gut[i] = gu * (i - 128);
        gvt[i] = gv * (i - 128);
        bt[i] = bu * (i - 128);
    }
    for (i = 0; i < 1024; i++) clip[i] = clamp8(i - 384);
    for (y = 0; y < h; y += 2) {
        const u8 *row0 = p->data + (size_t)y * (size_t)p->stride, *row1 = row0 + p->stride;
        const u8 *uv = p->chroma + (size_t)(y / 2) * (size_t)p->chroma_stride;
        u8 *d0 = dst + (size_t)y * (size_t)stride, *d1 = d0 + stride;
        bool second = y + 1 < h;
        for (x = 0; x < w; x += 2) {
            i32 r = rt[uv[x + 1]], g = -gut[uv[x]] - gvt[uv[x + 1]], b = bt[uv[x]];
            bool two = x + 1 < w;
            put_bgra(d0 + 4 * x, clip, lt[row0[x]], r, g, b);
            if (two) put_bgra(d0 + 4 * x + 4, clip, lt[row0[x + 1]], r, g, b);
            if (second) {
                put_bgra(d1 + 4 * x, clip, lt[row1[x]], r, g, b);
                if (two) put_bgra(d1 + 4 * x + 4, clip, lt[row1[x + 1]], r, g, b);
            }
        }
    }
}

bool mg_mcodec_render(const megler_codec_picture *p, void *bgra, i32 width, i32 height, i32 stride)
{
    i32 w, h, x, y;
    if (!p || !bgra || width <= 0 || height <= 0) return false;
    w = width < p->width ? width : p->width;
    h = height < p->height ? height : p->height;
    if (p->format == MEGLER_CODEC_RGBA) {
        for (y = 0; y < h; y++) {
            const u8 *s = p->data + (size_t)y * (size_t)p->stride;
            u8 *d = (u8 *)bgra + (size_t)y * (size_t)stride;
            for (x = 0; x < w; x++, s += 4, d += 4) {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
                d[3] = 255;
            }
        }
        return true;
    }
    nv12_to_bgra(p, (u8 *)bgra, w, h, stride);
    return true;
}

/* ------------------------------------------------------------ natives */

#define CODEC ((mg_mcodec *)(mg_addr)c->args[0].j)

N(Media_supported) { c->ret.j = mg_mcodec_supported(mg_dvm_string_utf8(c->args[0].l)) ? 1 : 0; }

N(Media_create) { c->ret.j = (i64)(mg_addr)mg_mcodec_create(mg_dvm_string_utf8(c->args[0].l)); }

static const u8 *array_bytes(mg_jobj *a, size_t *n)
{
    *n = a ? a->u.arr.len : 0;
    return a && a->u.arr.len ? (const u8 *)a->u.arr.data : NULL;
}

/* configure(long h, int width, int height, int rate, int channels, byte[] csd0, byte[] csd1, byte[] csd2) */
N(Media_configure)
{
    const u8 *csd[3];
    size_t size[3];
    int i;
    for (i = 0; i < 3; i++) csd[i] = array_bytes(c->args[5 + i].l, &size[i]);
    c->ret.j = mg_mcodec_configure(CODEC, c->args[1].i, c->args[2].i, c->args[3].i, c->args[4].i, csd, size) ? 1 : 0;
}

/* decode(long h, byte[] data, int len, long ptsUs, int flags): one access unit or packet, or codec-specific data */
N(Media_decode)
{
    size_t n = 0;
    const u8 *d = array_bytes(c->args[1].l, &n);
    if (c->args[2].i >= 0 && (size_t)c->args[2].i < n) n = (size_t)c->args[2].i;
    mg_mcodec_decode(CODEC, d, d ? n : 0, c->args[3].j, c->args[4].i);
    c->ret.i = 0;
}

static i64 *long_array(mg_jobj *a) { return a && a->u.arr.len >= 3 ? (i64 *)a->u.arr.data : NULL; }

/* nextFrame(long h, long[] info) -> a frame (0: none yet); info = { ptsUs, width, height } */
N(Media_nextFrame) { c->ret.j = (i64)(mg_addr)mg_mcodec_next_frame(CODEC, long_array(c->args[1].l)); }

static void *place_array(size_t bytes, void *arg)
{
    mg_jobj *a = mg_dvm_new_array("[B", (i32)bytes);
    *(mg_jobj **)arg = a;
    return a ? a->u.arr.data : NULL;
}

/* nextPcm(long h, long[] info) -> 16-bit PCM (null: none yet); info = { ptsUs, rate, channels } */
N(Media_nextPcm)
{
    mg_jobj *a = NULL;
    mg_mcodec *codec = CODEC;
    c->ret.l = codec && next_pcm(codec, long_array(c->args[1].l), place_array, &a) ? a : NULL;
}

/* The core's bitmap of an android.graphics.Bitmap: its field mNative. */
static i64 bitmap_of(mg_jobj *bitmap)
{
    mg_jvalue v;
    v.j = 0;
    if (!bitmap || !mg_dvm_get_field(bitmap, "mNative", &v)) mg_dvm_clear_pending();
    return v.j;
}

/* renderFrame(long frame, Bitmap dst): the frame's picture into a Bitmap of its size */
N(Media_renderFrame)
{
    mg_java_gfx_bitmap b;
    i64 bm = bitmap_of(c->args[1].l);
    c->ret.j = 0;
    if (!bm || !mg_java_gfx_bitmap_info(bm, &b)) return;
    if (!mg_mcodec_render(mg_mcodec_frame_picture((const mg_mframe *)(mg_addr)c->args[0].j), b.pixels, b.width, b.height, b.stride)) return;
    mg_java_gfx_bitmap_changed(bm);
    c->ret.j = 1;
}

N(Media_freeFrame) { mg_mcodec_free_frame((mg_mframe *)(mg_addr)c->args[0].j); }
N(Media_flush) { mg_mcodec_flush(CODEC); }
N(Media_destroy) { mg_mcodec_destroy(CODEC); }
N(Media_size) { c->ret.i = mg_mcodec_size(CODEC); }

static const mg_jhle k_media[] = {
    { "megler/MediaCodecNative", "supported", NULL, Media_supported },
    { "megler/MediaCodecNative", "create", NULL, Media_create },
    { "megler/MediaCodecNative", "configure", NULL, Media_configure },
    { "megler/MediaCodecNative", "decode", NULL, Media_decode },
    { "megler/MediaCodecNative", "nextFrame", NULL, Media_nextFrame },
    { "megler/MediaCodecNative", "nextPcm", NULL, Media_nextPcm },
    { "megler/MediaCodecNative", "renderFrame", NULL, Media_renderFrame },
    { "megler/MediaCodecNative", "freeFrame", NULL, Media_freeFrame },
    { "megler/MediaCodecNative", "flush", NULL, Media_flush },
    { "megler/MediaCodecNative", "destroy", NULL, Media_destroy },
    { "megler/MediaCodecNative", "size", NULL, Media_size },
    { NULL, NULL, NULL, NULL },
};

const mg_jhle *mg_java_media_natives(void) { return k_media; }
