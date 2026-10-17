/* The decoders behind MediaCodec over real decoders: hk_codec_tests.h says what is checked. */
#include "hk_codec_tests.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/megler.h"
#include "../src/drivers/hk_java.h"
#include "../src/hk_base.h"
#include "../src/host/hk_host.h"

static int g_checks, g_failed;
static hk_test_sink g_sink;
static void *g_sink_user;

static void say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_sink) g_sink(line, g_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define CHECK(cond)                                                    \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

/* ----------------------------------------------------------------- the streams (tools/media-test-streams.py's form) */

typedef struct record {
    u32 flags, size;
    i64 time_us;
    const u8 *data;
} record;

typedef struct stream {
    u8 *file;
    char mime[33];
    i32 width, height, rate, channels;
    record *csd, *samples;
    int ncsd, nsamples;
} stream;

static bool stream_read(stream *s, const char *dir, const char *name)
{
    char *path = hk_sprintf("%s/%s", dir, name);
    size_t size = 0, at = 60;
    u32 i, count;
    memset(s, 0, sizeof(*s));
    s->file = hk_file_read_all(path, &size);
    free(path);
    if (!s->file || size < 60 || memcmp(s->file, "HKMEDIA1", 8)) return false;
    memcpy(s->mime, s->file + 8, 32);
    s->width = (i32)hk_rd32(s->file + 40);
    s->height = (i32)hk_rd32(s->file + 44);
    s->rate = (i32)hk_rd32(s->file + 48);
    s->channels = (i32)hk_rd32(s->file + 52);
    count = hk_rd32(s->file + 56);
    s->csd = (record *)hk_alloc(sizeof(record) * (count + 1));
    s->samples = (record *)hk_alloc(sizeof(record) * (count + 1));
    for (i = 0; i < count; i++) {
        record r;
        if (at + 16 > size) return false;
        r.flags = hk_rd32(s->file + at);
        r.size = hk_rd32(s->file + at + 4);
        memcpy(&r.time_us, s->file + at + 8, 8);
        r.data = s->file + at + 16;
        at += 16 + r.size;
        if (at > size) return false;
        if (r.flags & 2) s->csd[s->ncsd++] = r;
        else s->samples[s->nsamples++] = r;
    }
    return true;
}

static void stream_free(stream *s)
{
    free(s->file);
    free(s->csd);
    free(s->samples);
    memset(s, 0, sizeof(*s));
}

static int picture_number(i64 time_us) { return (int)((time_us * 30 + 500000) / 1000000); }

/* ------------------------------------------------------------------------------------------------------- pictures */

typedef struct pictures {
    int n, numbers[64];
    bool grey, sized;
    i32 width, height;
    /* From picture `split` on (when it is not 0), the pictures are another stream's, numbered from there, of that size. */
    int split;
    i32 split_width, split_height;
    int fed, first_after;              /* units fed so far, and how many there were when the first picture came out */
} pictures;

/* Every picture the codec has ready: its number, and whether its middle is the grey that number says, before and after renderFrame. */
static void take_pictures(hk_mcodec *c, const stream *s, pictures *out)
{
    i64 info[3];
    hk_mframe *f;
    while ((f = hk_mcodec_next_frame(c, info)) != NULL) {
        const megler_codec_picture *p = hk_mcodec_frame_picture(f);
        int number = picture_number(info[0]), second = out->split && number >= out->split, want = 16 + 8 * (second ? number - out->split : number), x, y;
        i32 w = second ? out->split_width : s->width, h = second ? out->split_height : s->height;
        long sum = 0;
        u8 *bgra = (u8 *)hk_alloc((size_t)p->width * (size_t)p->height * 4);
        for (y = p->height / 2 - 8; y < p->height / 2 + 8; y++)
            for (x = p->width / 2 - 8; x < p->width / 2 + 8; x++) sum += p->data[(size_t)y * (size_t)p->stride + (size_t)x];
        if (labs(sum / 256 - want) > 4) {
            if (out->grey) say("  picture %d: its luma is %ld, not %d", number, sum / 256, want);
            out->grey = false;
        }
        if (hk_mcodec_render(p, bgra, p->width, p->height, p->width * 4)) {
            const u8 *px = bgra + ((size_t)(p->height / 2) * (size_t)p->width + (size_t)(p->width / 2)) * 4;
            int rgb = (298 * (want - 16) + 128) >> 8;
            if (abs(px[0] - rgb) > 8 || abs(px[1] - rgb) > 8 || abs(px[2] - rgb) > 8 || px[3] != 255) {
                if (out->grey) say("  picture %d: drawn as %d, %d, %d, not %d", number, px[2], px[1], px[0], rgb);
                out->grey = false;
            }
        } else {
            out->grey = false;
        }
        free(bgra);
        if (p->width != w || p->height != h || info[1] != p->width || info[2] != p->height) {
            if (out->sized) say("  picture %d: %dx%d, not %dx%d", number, p->width, p->height, w, h);
            out->sized = false;
        }
        out->width = p->width;
        out->height = p->height;
        if (out->n < (int)HK_COUNT(out->numbers)) out->numbers[out->n] = number;
        out->n++;
        hk_mcodec_free_frame(f);
    }
}

static bool in_order(const pictures *p, int first, int count)
{
    int i;
    if (p->n != count) return false;
    for (i = 0; i < count; i++)
        if (p->numbers[i] != first + i) return false;
    return true;
}

static void say_numbers(const char *what, const pictures *p)
{
    char line[400];
    size_t k = 0;
    int i;
    line[0] = 0;
    for (i = 0; i < p->n && i < (int)HK_COUNT(p->numbers) && k + 8 < sizeof(line); i++) k += (size_t)snprintf(line + k, sizeof(line) - k, " %d", p->numbers[i]);
    say("  %s: %d pictures%s%s, %dx%d", what, p->n, p->n ? ":" : "", line, p->width, p->height);
}

enum { CSD, IN_FIRST_UNIT, CONFIG_BUFFER, LENGTH_PREFIXED };

/* A unit with its start codes turned into four-byte lengths, as MP4 has it. */
static u8 *length_prefixed(const u8 *d, size_t n, size_t *out_n)
{
    u8 *out = (u8 *)hk_alloc(n + 64);
    size_t i = 0, k = 0, start = (size_t)-1;
    while (i + 3 <= n) {
        if (!d[i] && !d[i + 1] && d[i + 2] == 1) {
            if (start != (size_t)-1) {
                size_t end = i, len;
                while (end > start && !d[end - 1]) end--;
                len = end - start;
                out[k++] = (u8)(len >> 24), out[k++] = (u8)(len >> 16), out[k++] = (u8)(len >> 8), out[k++] = (u8)len;
                memcpy(out + k, d + start, len);
                k += len;
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != (size_t)-1) {
        size_t len = n - start;
        out[k++] = (u8)(len >> 24), out[k++] = (u8)(len >> 16), out[k++] = (u8)(len >> 8), out[k++] = (u8)len;
        memcpy(out + k, d + start, len);
        k += len;
    }
    *out_n = k;
    return out;
}

/* MP4's avcC record of an SPS and a PPS (each given with its four-byte start code). */
static size_t avcc(const record *sps, const record *pps, u8 *out)
{
    size_t k = 0, ns = sps->size - 4, np = pps->size - 4;
    out[k++] = 1;
    out[k++] = sps->data[5];
    out[k++] = sps->data[6];
    out[k++] = sps->data[7];
    out[k++] = 0xFF;
    out[k++] = 0xE1;
    out[k++] = (u8)(ns >> 8), out[k++] = (u8)ns;
    memcpy(out + k, sps->data + 4, ns);
    k += ns;
    out[k++] = 1;
    out[k++] = (u8)(np >> 8), out[k++] = (u8)np;
    memcpy(out + k, pps->data + 4, np);
    return k + np;
}

/* Samples from..to of the stream through a configured codec, the last with the end of the stream when `end`. */
static void feed_pictures(hk_mcodec *c, const stream *s, int from, int to, bool end, int form, pictures *out)
{
    int i;
    for (i = from; i < to; i++) {
        const record *r = &s->samples[i];
        i32 flags = end && i == to - 1 ? 4 : 0;
        if (form == IN_FIRST_UNIT && i == from) {
            size_t n = r->size;
            int k;
            u8 *unit, *at;
            for (k = 0; k < s->ncsd; k++) n += s->csd[k].size;
            unit = at = (u8 *)hk_alloc(n);
            for (k = 0; k < s->ncsd; k++) memcpy(at, s->csd[k].data, s->csd[k].size), at += s->csd[k].size;
            memcpy(at, r->data, r->size);
            hk_mcodec_decode(c, unit, n, r->time_us, flags);
            free(unit);
        } else if (form == LENGTH_PREFIXED) {
            size_t n;
            u8 *unit = length_prefixed(r->data, r->size, &n);
            hk_mcodec_decode(c, unit, n, r->time_us, flags);
            free(unit);
        } else {
            hk_mcodec_decode(c, r->data, r->size, r->time_us, flags);
        }
        out->fed++;
        take_pictures(c, s, out);
        if (out->n && !out->first_after) out->first_after = out->fed;
    }
}

static hk_mcodec *open_pictures(const stream *s, int form)
{
    const u8 *csd[3] = { NULL, NULL, NULL };
    size_t size[3] = { 0, 0, 0 };
    u8 record_bytes[600];
    hk_mcodec *c = hk_mcodec_create(s->mime);
    int k;
    if (!c) return NULL;
    if (form == CSD)
        for (k = 0; k < s->ncsd && k < 3; k++) csd[k] = s->csd[k].data, size[k] = s->csd[k].size;
    if (form == LENGTH_PREFIXED && s->ncsd == 2 && s->csd[0].size + s->csd[1].size < 500) {
        csd[0] = record_bytes;
        size[0] = avcc(&s->csd[0], &s->csd[1], record_bytes);
    }
    if (!hk_mcodec_configure(c, s->width, s->height, 0, 0, csd, size)) {
        hk_mcodec_destroy(c);
        return NULL;
    }
    if (form == CONFIG_BUFFER) {
        u8 both[600];
        size_t n = 0;
        for (k = 0; k < s->ncsd && n + s->csd[k].size <= sizeof(both); k++) memcpy(both + n, s->csd[k].data, s->csd[k].size), n += s->csd[k].size;
        hk_mcodec_decode(c, both, n, 0, 2);
    }
    return c;
}

static void test_pictures(const char *dir, const char *file, const char *name)
{
    static const char *const forms[] = { "the parameter sets as csd", "the parameter sets in the first unit", "the parameter sets in a codec-config buffer",
                                         "length-prefixed units, csd-0 the avcC record" };
    stream s;
    int form, idr = -1, i;
    if (!stream_read(&s, dir, file)) {
        g_checks++;
        g_failed++;
        say("  FAIL: %s/%s cannot be read", dir, file);
        return;
    }
    if (!hk_mcodec_supported(s.mime)) {
        say("%s: no decoder here (%s)", name, s.mime);
        stream_free(&s);
        return;
    }
    for (form = CSD; form <= LENGTH_PREFIXED; form++) {
        pictures p;
        hk_mcodec *c;
        if (form == LENGTH_PREFIXED && strcmp(s.mime, "video/avc")) continue;
        memset(&p, 0, sizeof(p));
        p.grey = p.sized = true;
        c = open_pictures(&s, form);
        say("%s, %s", name, forms[form]);
        CHECK(c != NULL);
        if (!c) continue;
        feed_pictures(c, &s, 0, s.nsamples, true, form, &p);
        say_numbers("in display order", &p);
        say("  (the first picture came out after %d units%s)", p.first_after, p.first_after == s.nsamples ? ", at the end of the stream" : "");
        CHECK(in_order(&p, 0, s.nsamples));
        CHECK(p.grey && p.sized);
        CHECK(hk_mcodec_size(c) == (s.width << 16 | s.height));
        hk_mcodec_destroy(c);
    }
    /* A flush after seven units, then from the IDR at picture 12: that and the eleven after it, and nothing from before the flush. */
    for (i = 0; i < s.nsamples; i++)
        if (picture_number(s.samples[i].time_us) == 12) idr = i;
    {
        pictures before, after;
        hk_mcodec *c = open_pictures(&s, CSD);
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        before.grey = before.sized = after.grey = after.sized = true;
        say("%s, a flush after seven units, then from the IDR at 12", name);
        CHECK(c && idr > 7);
        if (c && idr > 7) {
            feed_pictures(c, &s, 0, 7, false, CSD, &before);
            hk_mcodec_flush(c);
            take_pictures(c, &s, &before);
            feed_pictures(c, &s, idr, s.nsamples, true, CSD, &after);
            say_numbers("before the flush", &before);
            say_numbers("after it", &after);
            CHECK(in_order(&after, 12, s.nsamples - idr));
            CHECK(after.grey && after.sized);
        }
        hk_mcodec_destroy(c);
    }
    stream_free(&s);
}

/*
 * A stream that changes its size, as adaptive streaming makes one: the 24 pictures of avc.hkau, then the parameter sets
 * of avc-small.hkau in a codec-config buffer and its 12 pictures at 96 x 64, their times going on from the first's. All
 * 36 come out in order, each at its own size.
 */
static void test_size_change(const char *dir)
{
    stream big, small;
    pictures p;
    hk_mcodec *c;
    u8 both[600];
    size_t n = 0;
    int i, k;
    if (!stream_read(&big, dir, "avc.hkau") || !stream_read(&small, dir, "avc-small.hkau")) {
        g_checks++;
        g_failed++;
        say("  FAIL: avc.hkau or avc-small.hkau in %s cannot be read", dir);
        return;
    }
    say("H.264 that changes its size from %dx%d to %dx%d", big.width, big.height, small.width, small.height);
    memset(&p, 0, sizeof(p));
    p.grey = p.sized = true;
    p.split = big.nsamples;
    p.split_width = small.width;
    p.split_height = small.height;
    c = open_pictures(&big, CSD);
    CHECK(c != NULL);
    if (c) {
        feed_pictures(c, &big, 0, big.nsamples, false, CSD, &p);
        for (k = 0; k < small.ncsd && n + small.csd[k].size <= sizeof(both); k++) memcpy(both + n, small.csd[k].data, small.csd[k].size), n += small.csd[k].size;
        hk_mcodec_decode(c, both, n, 0, 2);
        for (i = 0; i < small.nsamples; i++) {
            const record *r = &small.samples[i];
            hk_mcodec_decode(c, r->data, r->size, r->time_us + (i64)big.nsamples * 1000000 / 30, i == small.nsamples - 1 ? 4 : 0);
            take_pictures(c, &big, &p);
        }
        say_numbers("in display order", &p);
        CHECK(in_order(&p, 0, big.nsamples + small.nsamples));
        CHECK(p.grey && p.sized);
        CHECK(hk_mcodec_size(c) == (small.width << 16 | small.height));
        hk_mcodec_destroy(c);
    }
    stream_free(&big);
    stream_free(&small);
}

/* ---------------------------------------------------------------------------------------------------------- sound */

static size_t put_adts(u8 *out, const u8 *frame, size_t n)
{
    size_t len = n + 7;
    out[0] = 0xFF;
    out[1] = 0xF1;
    out[2] = (u8)(1 << 6 | 4 << 2);
    out[3] = (u8)(2 << 6 | (len >> 11));
    out[4] = (u8)(len >> 3);
    out[5] = (u8)((len & 7) << 5 | 0x1F);
    out[6] = 0xFC;
    memcpy(out + 7, frame, n);
    return len;
}

static void test_sound(const char *dir, const char *file, const char *name, bool adts, int per_packet)
{
    stream s;
    hk_mcodec *c;
    const u8 *csd[3] = { NULL, NULL, NULL };
    size_t size[3] = { 0, 0, 0 }, have = 0, cap = 0;
    i16 *pcm = NULL;
    i64 info[3], rate = 0, channels = 0, last_time = -1;
    bool timed = true;
    int i, blocks = 0;
    if (!stream_read(&s, dir, file)) {
        g_checks++;
        g_failed++;
        say("  FAIL: %s/%s cannot be read", dir, file);
        return;
    }
    if (!hk_mcodec_supported(s.mime)) {
        say("%s: no decoder here (%s)", name, s.mime);
        stream_free(&s);
        return;
    }
    say("%s", name);
    c = hk_mcodec_create(s.mime);
    if (!adts && s.ncsd) csd[0] = s.csd[0].data, size[0] = s.csd[0].size;
    /* MP3 is configured with no rate or channels: its first frame says them. */
    CHECK(c && hk_mcodec_configure(c, 0, 0, per_packet == 1152 ? 0 : s.rate, per_packet == 1152 ? 0 : s.channels, csd, size));
    for (i = 0; c && i < s.nsamples; i++) {
        const record *r = &s.samples[i];
        u8 *pk;
        size_t bytes;
        u8 framed[2048];
        if (adts && r->size + 7 <= sizeof(framed)) hk_mcodec_decode(c, framed, put_adts(framed, r->data, r->size), r->time_us, i == s.nsamples - 1 ? 4 : 0);
        else hk_mcodec_decode(c, r->data, r->size, r->time_us, i == s.nsamples - 1 ? 4 : 0);
        while ((pk = hk_mcodec_next_pcm(c, info, &bytes)) != NULL) {
            if (have + bytes / 2 > cap) {
                cap = (have + bytes / 2) * 2;
                pcm = (i16 *)hk_realloc(pcm, cap * 2);
            }
            memcpy(pcm + have, pk, bytes);
            have += bytes / 2;
            free(pk);
            blocks++;
            rate = info[1];
            channels = info[2];
            if (info[0] < last_time) timed = false;
            last_time = info[0];
        }
    }
    {
        /*
         * The left channel's middle half (a decoder's start and end are its own business): the tone from the first and the
         * last of its rising crossings of zero, each placed between its two samples, and the level from its squares.
         */
        i64 frames = channels > 0 ? (i64)(have / (size_t)channels) : 0, from = frames / 4, to = frames * 3 / 4, f, crossings = 0;
        double sum = 0, rms = 0, hz = 0, first = -1, last = -1;
        for (f = from; f < to && f > 0; f++) {
            double a = pcm[(size_t)(f - 1) * (size_t)channels], b = pcm[(size_t)f * (size_t)channels];
            if (a < 0 && b >= 0) {
                last = (double)(f - 1) + a / (a - b);
                if (first < 0) first = last;
                crossings++;
            }
            sum += b * b;
        }
        if (crossings > 1 && rate > 0) {
            hz = (double)(crossings - 1) * (double)rate / (last - first);
            rms = sqrt(sum / (double)(to - from));
        }
        say("  %d blocks, %lld frames (%d packets of %d), %lld Hz in %lld channels; a tone of %.1f Hz at %.0f RMS", blocks, (long long)frames, s.nsamples, per_packet,
            (long long)rate, (long long)channels, hz, rms);
        CHECK(frames >= (i64)(s.nsamples - 3) * per_packet && frames <= (i64)s.nsamples * per_packet);
        CHECK(rate == 44100 && channels == 2 && timed);
        /* (The stream's tone is half of full scale, made stereo by ffmpeg at 3 dB less in each channel: 8192 RMS.) */
        CHECK(hz > 437 && hz < 443);
        CHECK(rms > 6500 && rms < 10000);
    }
    free(pcm);
    hk_mcodec_destroy(c);
    stream_free(&s);
}

/*
 * Two decoders at once, as an app plays a film: pictures on one thread and sound on another, each decoder made, fed its
 * whole stream and closed, five times over, while the other does the same. Each run gives what it gives alone.
 */
typedef struct together {
    const char *dir;
    bool video;
    int good, runs;
} together;

static void together_run(void *arg)
{
    together *t = (together *)arg;
    int run;
    for (run = 0; run < t->runs; run++) {
        stream s;
        hk_mcodec *c;
        if (!stream_read(&s, t->dir, t->video ? "avc.hkau" : "aac.hkau")) return;
        if (t->video) {
            pictures p;
            memset(&p, 0, sizeof(p));
            p.grey = p.sized = true;
            c = open_pictures(&s, CSD);
            if (c) {
                feed_pictures(c, &s, 0, s.nsamples, true, CSD, &p);
                if (in_order(&p, 0, s.nsamples) && p.grey && p.sized) t->good++;
                hk_mcodec_destroy(c);
            }
        } else {
            const u8 *csd[3] = { s.csd[0].data, NULL, NULL };
            size_t size[3] = { s.csd[0].size, 0, 0 }, frames = 0;
            int i;
            c = hk_mcodec_create(s.mime);
            if (c && hk_mcodec_configure(c, 0, 0, s.rate, s.channels, csd, size)) {
                for (i = 0; i < s.nsamples; i++) {
                    i64 info[3];
                    size_t bytes;
                    u8 *pcm;
                    hk_mcodec_decode(c, s.samples[i].data, s.samples[i].size, s.samples[i].time_us, i == s.nsamples - 1 ? 4 : 0);
                    while ((pcm = hk_mcodec_next_pcm(c, info, &bytes)) != NULL) {
                        frames += bytes / 4;
                        free(pcm);
                    }
                }
                if (frames == (size_t)s.nsamples * 1024) t->good++;
            }
            hk_mcodec_destroy(c);
        }
        stream_free(&s);
    }
}

static void test_together(const char *dir)
{
    together video = { dir, true, 0, 5 }, sound = { dir, false, 0, 5 };
    hk_thread *a, *b;
    say("H.264 and AAC decoded at once on two threads, five times each");
    a = hk_thread_start(together_run, &video, 1 << 20, "codec-video");
    b = hk_thread_start(together_run, &sound, 1 << 20, "codec-sound");
    CHECK(a && b);
    if (a) hk_thread_join(a);
    if (b) hk_thread_join(b);
    say("  %d and %d of 5 runs gave everything", video.good, sound.good);
    CHECK(video.good == 5 && sound.good == 5);
}

/* ---------------------------------------------------------------------------------------------------------- speed */

static void speed_of(const char *dir, const char *file)
{
    stream s;
    hk_mcodec *c;
    u8 *bgra;
    i64 info[3], last = -1;
    u64 start, render_ns = 0, total_ns;
    int i, pictures = 0, first_after = 0;
    bool ordered = true;
    i32 w = 0, h = 0;
    char *path = hk_sprintf("%s/%s", dir, file);
    hk_stat st;
    bool there = hk_path_stat(path, &st, NULL);
    free(path);
    if (!there) {
        say("%s: not there (tools/media-test-streams.py --speed makes it)", file);
        return;
    }
    if (!stream_read(&s, dir, file)) {
        g_checks++;
        g_failed++;
        say("  FAIL: %s/%s cannot be read", dir, file);
        return;
    }
    if (!hk_mcodec_supported(s.mime)) {
        say("%s: no decoder here (%s)", file, s.mime);
        stream_free(&s);
        return;
    }
    c = open_pictures(&s, CSD);
    CHECK(c != NULL);
    bgra = (u8 *)hk_alloc((size_t)s.width * (size_t)s.height * 4);
    start = hk_time_monotonic_ns();
    for (i = 0; c && i < s.nsamples; i++) {
        hk_mframe *f;
        hk_mcodec_decode(c, s.samples[i].data, s.samples[i].size, s.samples[i].time_us, i == s.nsamples - 1 ? 4 : 0);
        while ((f = hk_mcodec_next_frame(c, info)) != NULL) {
            const megler_codec_picture *p = hk_mcodec_frame_picture(f);
            u64 t = hk_time_monotonic_ns();
            if (p->width <= s.width && p->height <= s.height) hk_mcodec_render(p, bgra, p->width, p->height, p->width * 4);
            render_ns += hk_time_monotonic_ns() - t;
            if (info[0] <= last) ordered = false;
            last = info[0];
            w = p->width;
            h = p->height;
            if (!pictures++) first_after = i + 1;
            hk_mcodec_free_frame(f);
        }
    }
    total_ns = hk_time_monotonic_ns() - start;
    if (pictures) {
        double decode_ms = (double)(total_ns - render_ns) / 1e6 / pictures, render_ms = (double)render_ns / 1e6 / pictures;
        say("%s: %d pictures of %dx%d (the first after %d units): %.2f ms a picture to decode, %.2f ms to turn into a bitmap; %.0f pictures a second in all",
            file, pictures, w, h, first_after, decode_ms, render_ms, pictures / ((double)total_ns / 1e9));
    } else {
        say("%s: no picture came out", file);
    }
    CHECK(pictures == s.nsamples && ordered && w == s.width && h == s.height);
    free(bgra);
    hk_mcodec_destroy(c);
    stream_free(&s);
}

int hk_codec_speed_run(const char *streams, const struct megler_codec_host *host, hk_test_sink sink, void *user, int *checks)
{
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    hk_java_media_set_host(host);
    say("how fast the decoders behind MediaCodec are (the streams in %s)", streams);
    speed_of(streams, "speed-avc-720p.hkau");
    speed_of(streams, "speed-avc-1080p.hkau");
    speed_of(streams, "speed-hevc-1080p.hkau");
    hk_java_media_set_host(NULL);
    *checks += g_checks;
    return g_failed;
}

int hk_codec_tests_run(const char *streams, const struct megler_codec_host *host, hk_test_sink sink, void *user, int *checks)
{
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    hk_java_media_set_host(host);
    say("the decoders behind MediaCodec, over real ones (the streams in %s)", streams);
    CHECK(hk_mcodec_supported("video/avc") && hk_mcodec_supported("audio/mp4a-latm") && hk_mcodec_supported("audio/mpeg"));
    test_pictures(streams, "avc.hkau", "H.264");
    test_size_change(streams);
    test_pictures(streams, "hevc.hkau", "H.265");
    test_sound(streams, "aac.hkau", "AAC with its AudioSpecificConfig", false, 1024);
    test_sound(streams, "aac.hkau", "AAC with ADTS headers and nothing else", true, 1024);
    test_sound(streams, "mp3.hkau", "MP3, configured without its rate or channels", false, 1152);
    test_together(streams);
    hk_java_media_set_host(NULL);
    *checks += g_checks;
    return g_failed;
}
