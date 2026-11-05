/* The decoders behind MediaCodec over real decoders: mg_codec_tests.h says what is checked. */
#include "mg_codec_tests.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/megler.h"
#include "../src/drivers/mg_java.h"
#include "../src/mg_base.h"
#include "../src/host/mg_host.h"

static int g_checks, g_failed;
static mg_test_sink g_sink;
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
    char *path = mg_sprintf("%s/%s", dir, name);
    size_t size = 0, at = 60;
    u32 i, count;
    memset(s, 0, sizeof(*s));
    s->file = mg_file_read_all(path, &size);
    free(path);
    if (!s->file || size < 60 || memcmp(s->file, "MGMEDIA1", 8)) return false;
    memcpy(s->mime, s->file + 8, 32);
    s->width = (i32)mg_rd32(s->file + 40);
    s->height = (i32)mg_rd32(s->file + 44);
    s->rate = (i32)mg_rd32(s->file + 48);
    s->channels = (i32)mg_rd32(s->file + 52);
    count = mg_rd32(s->file + 56);
    s->csd = (record *)mg_alloc(sizeof(record) * (count + 1));
    s->samples = (record *)mg_alloc(sizeof(record) * (count + 1));
    for (i = 0; i < count; i++) {
        record r;
        if (at + 16 > size) return false;
        r.flags = mg_rd32(s->file + at);
        r.size = mg_rd32(s->file + at + 4);
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
static void take_pictures(mg_mcodec *c, const stream *s, pictures *out)
{
    i64 info[3];
    mg_mframe *f;
    while ((f = mg_mcodec_next_frame(c, info)) != NULL) {
        const megler_codec_picture *p = mg_mcodec_frame_picture(f);
        int number = picture_number(info[0]), second = out->split && number >= out->split, want = 16 + 8 * (second ? number - out->split : number), x, y;
        i32 w = second ? out->split_width : s->width, h = second ? out->split_height : s->height;
        long sum = 0;
        u8 *bgra;
        if (!p) {
            /* (A picture on the graphics processor that could not be read into memory.) */
            if (out->grey) say("  picture %d: not to be had in memory", number);
            out->grey = false;
            if (out->n < (int)MG_COUNT(out->numbers)) out->numbers[out->n] = number;
            out->n++;
            mg_mcodec_free_frame(f);
            continue;
        }
        bgra = (u8 *)mg_alloc((size_t)p->width * (size_t)p->height * 4);
        /* (The luma itself where the picture is NV12; one turned into colour by the host is checked by its colour alone.) */
        if (p->format == MEGLER_CODEC_NV12) {
            for (y = p->height / 2 - 8; y < p->height / 2 + 8; y++)
                for (x = p->width / 2 - 8; x < p->width / 2 + 8; x++) sum += p->data[(size_t)y * (size_t)p->stride + (size_t)x];
            if (labs(sum / 256 - want) > 4) {
                if (out->grey) say("  picture %d: its luma is %ld, not %d", number, sum / 256, want);
                out->grey = false;
            }
        }
        if (mg_mcodec_render(p, bgra, p->width, p->height, p->width * 4)) {
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
        if (out->n < (int)MG_COUNT(out->numbers)) out->numbers[out->n] = number;
        out->n++;
        mg_mcodec_free_frame(f);
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
    for (i = 0; i < p->n && i < (int)MG_COUNT(p->numbers) && k + 8 < sizeof(line); i++) k += (size_t)snprintf(line + k, sizeof(line) - k, " %d", p->numbers[i]);
    say("  %s: %d pictures%s%s, %dx%d", what, p->n, p->n ? ":" : "", line, p->width, p->height);
}

enum { CSD, IN_FIRST_UNIT, CONFIG_BUFFER, LENGTH_PREFIXED };

/* A unit with its start codes turned into four-byte lengths, as MP4 has it. */
static u8 *length_prefixed(const u8 *d, size_t n, size_t *out_n)
{
    u8 *out = (u8 *)mg_alloc(n + 64);
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
static void feed_pictures(mg_mcodec *c, const stream *s, int from, int to, bool end, int form, pictures *out)
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
            unit = at = (u8 *)mg_alloc(n);
            for (k = 0; k < s->ncsd; k++) memcpy(at, s->csd[k].data, s->csd[k].size), at += s->csd[k].size;
            memcpy(at, r->data, r->size);
            mg_mcodec_decode(c, unit, n, r->time_us, flags);
            free(unit);
        } else if (form == LENGTH_PREFIXED) {
            size_t n;
            u8 *unit = length_prefixed(r->data, r->size, &n);
            mg_mcodec_decode(c, unit, n, r->time_us, flags);
            free(unit);
        } else {
            mg_mcodec_decode(c, r->data, r->size, r->time_us, flags);
        }
        out->fed++;
        take_pictures(c, s, out);
        if (out->n && !out->first_after) out->first_after = out->fed;
    }
}

static mg_mcodec *open_pictures(const stream *s, int form)
{
    const u8 *csd[3] = { NULL, NULL, NULL };
    size_t size[3] = { 0, 0, 0 };
    u8 record_bytes[600];
    mg_mcodec *c = mg_mcodec_create(s->mime);
    int k;
    if (!c) return NULL;
    if (form == CSD)
        for (k = 0; k < s->ncsd && k < 3; k++) csd[k] = s->csd[k].data, size[k] = s->csd[k].size;
    if (form == LENGTH_PREFIXED && s->ncsd == 2 && s->csd[0].size + s->csd[1].size < 500) {
        csd[0] = record_bytes;
        size[0] = avcc(&s->csd[0], &s->csd[1], record_bytes);
    }
    if (!mg_mcodec_configure(c, s->width, s->height, 0, 0, csd, size)) {
        mg_mcodec_destroy(c);
        return NULL;
    }
    if (form == CONFIG_BUFFER) {
        u8 both[600];
        size_t n = 0;
        for (k = 0; k < s->ncsd && n + s->csd[k].size <= sizeof(both); k++) memcpy(both + n, s->csd[k].data, s->csd[k].size), n += s->csd[k].size;
        mg_mcodec_decode(c, both, n, 0, 2);
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
    if (!mg_mcodec_supported(s.mime)) {
        say("%s: no decoder here (%s)", name, s.mime);
        stream_free(&s);
        return;
    }
    for (form = CSD; form <= LENGTH_PREFIXED; form++) {
        pictures p;
        mg_mcodec *c;
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
        CHECK(mg_mcodec_size(c) == (s.width << 16 | s.height));
        mg_mcodec_destroy(c);
    }
    /* A flush after seven units, then from the IDR at picture 12: that and the eleven after it, and nothing from before the flush. */
    for (i = 0; i < s.nsamples; i++)
        if (picture_number(s.samples[i].time_us) == 12) idr = i;
    {
        pictures before, after;
        mg_mcodec *c = open_pictures(&s, CSD);
        memset(&before, 0, sizeof(before));
        memset(&after, 0, sizeof(after));
        before.grey = before.sized = after.grey = after.sized = true;
        say("%s, a flush after seven units, then from the IDR at 12", name);
        CHECK(c && idr > 7);
        if (c && idr > 7) {
            feed_pictures(c, &s, 0, 7, false, CSD, &before);
            mg_mcodec_flush(c);
            take_pictures(c, &s, &before);
            feed_pictures(c, &s, idr, s.nsamples, true, CSD, &after);
            say_numbers("before the flush", &before);
            say_numbers("after it", &after);
            CHECK(in_order(&after, 12, s.nsamples - idr));
            CHECK(after.grey && after.sized);
        }
        mg_mcodec_destroy(c);
    }
    stream_free(&s);
}

/*
 * A stream that changes its size, as adaptive streaming makes one: the 24 pictures of avc.mgau, then the parameter sets
 * of avc-small.mgau in a codec-config buffer and its 12 pictures at 96 x 64, their times going on from the first's. All
 * 36 come out in order, each at its own size.
 */
static void test_size_change(const char *dir)
{
    stream big, small;
    pictures p;
    mg_mcodec *c;
    u8 both[600];
    size_t n = 0;
    int i, k;
    if (!stream_read(&big, dir, "avc.mgau") || !stream_read(&small, dir, "avc-small.mgau")) {
        g_checks++;
        g_failed++;
        say("  FAIL: avc.mgau or avc-small.mgau in %s cannot be read", dir);
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
        mg_mcodec_decode(c, both, n, 0, 2);
        for (i = 0; i < small.nsamples; i++) {
            const record *r = &small.samples[i];
            mg_mcodec_decode(c, r->data, r->size, r->time_us + (i64)big.nsamples * 1000000 / 30, i == small.nsamples - 1 ? 4 : 0);
            take_pictures(c, &big, &p);
        }
        say_numbers("in display order", &p);
        CHECK(in_order(&p, 0, big.nsamples + small.nsamples));
        CHECK(p.grey && p.sized);
        CHECK(mg_mcodec_size(c) == (small.width << 16 | small.height));
        mg_mcodec_destroy(c);
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
    mg_mcodec *c;
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
    if (!mg_mcodec_supported(s.mime)) {
        say("%s: no decoder here (%s)", name, s.mime);
        stream_free(&s);
        return;
    }
    say("%s", name);
    c = mg_mcodec_create(s.mime);
    if (!adts && s.ncsd) csd[0] = s.csd[0].data, size[0] = s.csd[0].size;
    /* MP3 is configured with no rate or channels: its first frame says them. */
    CHECK(c && mg_mcodec_configure(c, 0, 0, per_packet == 1152 ? 0 : s.rate, per_packet == 1152 ? 0 : s.channels, csd, size));
    for (i = 0; c && i < s.nsamples; i++) {
        const record *r = &s.samples[i];
        u8 *pk;
        size_t bytes;
        u8 framed[2048];
        if (adts && r->size + 7 <= sizeof(framed)) mg_mcodec_decode(c, framed, put_adts(framed, r->data, r->size), r->time_us, i == s.nsamples - 1 ? 4 : 0);
        else mg_mcodec_decode(c, r->data, r->size, r->time_us, i == s.nsamples - 1 ? 4 : 0);
        while ((pk = mg_mcodec_next_pcm(c, info, &bytes)) != NULL) {
            if (have + bytes / 2 > cap) {
                cap = (have + bytes / 2) * 2;
                pcm = (i16 *)mg_realloc(pcm, cap * 2);
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
    mg_mcodec_destroy(c);
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
        mg_mcodec *c;
        if (!stream_read(&s, t->dir, t->video ? "avc.mgau" : "aac.mgau")) return;
        if (t->video) {
            pictures p;
            memset(&p, 0, sizeof(p));
            p.grey = p.sized = true;
            c = open_pictures(&s, CSD);
            if (c) {
                feed_pictures(c, &s, 0, s.nsamples, true, CSD, &p);
                if (in_order(&p, 0, s.nsamples) && p.grey && p.sized) t->good++;
                mg_mcodec_destroy(c);
            }
        } else {
            const u8 *csd[3] = { s.csd[0].data, NULL, NULL };
            size_t size[3] = { s.csd[0].size, 0, 0 }, frames = 0;
            int i;
            c = mg_mcodec_create(s.mime);
            if (c && mg_mcodec_configure(c, 0, 0, s.rate, s.channels, csd, size)) {
                for (i = 0; i < s.nsamples; i++) {
                    i64 info[3];
                    size_t bytes;
                    u8 *pcm;
                    mg_mcodec_decode(c, s.samples[i].data, s.samples[i].size, s.samples[i].time_us, i == s.nsamples - 1 ? 4 : 0);
                    while ((pcm = mg_mcodec_next_pcm(c, info, &bytes)) != NULL) {
                        frames += bytes / 4;
                        free(pcm);
                    }
                }
                if (frames == (size_t)s.nsamples * 1024) t->good++;
            }
            mg_mcodec_destroy(c);
        }
        stream_free(&s);
    }
}

static void test_together(const char *dir)
{
    together video = { dir, true, 0, 5 }, sound = { dir, false, 0, 5 };
    mg_thread *a, *b;
    say("H.264 and AAC decoded at once on two threads, five times each");
    a = mg_thread_start(together_run, &video, 1 << 20, "codec-video");
    b = mg_thread_start(together_run, &sound, 1 << 20, "codec-sound");
    CHECK(a && b);
    if (a) mg_thread_join(a);
    if (b) mg_thread_join(b);
    say("  %d and %d of 5 runs gave everything", video.good, sound.good);
    CHECK(video.good == 5 && sound.good == 5);
}

/* ---------------------------------------------------------------------------------------------------------- speed */

static void speed_of(const char *dir, const char *file)
{
    stream s;
    mg_mcodec *c;
    u8 *bgra;
    i64 info[3], last = -1;
    u64 start, render_ns = 0, total_ns;
    int i, pictures = 0, first_after = 0;
    bool ordered = true;
    i32 w = 0, h = 0;
    char *path = mg_sprintf("%s/%s", dir, file);
    mg_stat st;
    bool there = mg_path_stat(path, &st, NULL);
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
    if (!mg_mcodec_supported(s.mime)) {
        say("%s: no decoder here (%s)", file, s.mime);
        stream_free(&s);
        return;
    }
    c = open_pictures(&s, CSD);
    CHECK(c != NULL);
    bgra = (u8 *)mg_alloc((size_t)s.width * (size_t)s.height * 4);
    start = mg_time_monotonic_ns();
    for (i = 0; c && i < s.nsamples; i++) {
        mg_mframe *f;
        mg_mcodec_decode(c, s.samples[i].data, s.samples[i].size, s.samples[i].time_us, i == s.nsamples - 1 ? 4 : 0);
        while ((f = mg_mcodec_next_frame(c, info)) != NULL) {
            u64 t = mg_time_monotonic_ns();
            const megler_codec_picture *p = mg_mcodec_frame_picture(f);
            if (p && p->width <= s.width && p->height <= s.height) mg_mcodec_render(p, bgra, p->width, p->height, p->width * 4);
            render_ns += mg_time_monotonic_ns() - t;
            if (info[0] <= last) ordered = false;
            last = info[0];
            w = (i32)info[1];
            h = (i32)info[2];
            if (!pictures++) first_after = i + 1;
            mg_mcodec_free_frame(f);
        }
    }
    total_ns = mg_time_monotonic_ns() - start;
    if (pictures) {
        double decode_ms = (double)(total_ns - render_ns) / 1e6 / pictures, render_ms = (double)render_ns / 1e6 / pictures;
        say("%s: %d pictures of %dx%d (the first after %d units): %.2f ms a picture to decode, %.2f ms to turn into a bitmap; %.0f pictures a second in all",
            file, pictures, w, h, first_after, decode_ms, render_ms, pictures / ((double)total_ns / 1e9));
    } else {
        say("%s: no picture came out", file);
    }
    CHECK(pictures == s.nsamples && ordered && w == s.width && h == s.height);
    free(bgra);
    mg_mcodec_destroy(c);
    stream_free(&s);
}

/* renderFrame alone, with no decoder at work beside it: a picture of 1920 x 1080 in NV12 into a bitmap, the best of ten. */
static void speed_of_render(void)
{
    enum { W = 1920, H = 1080 };
    u8 *nv12 = (u8 *)mg_alloc((size_t)W * H * 3 / 2), *bgra = (u8 *)mg_alloc((size_t)W * H * 4);
    megler_codec_picture p;
    u64 best = (u64)-1;
    u32 seed = 1;
    size_t i;
    int run;
    for (i = 0; i < (size_t)W * H * 3 / 2; i++) {
        seed = seed * 1103515245u + 12345u;
        nv12[i] = (u8)(seed >> 16);
    }
    memset(&p, 0, sizeof(p));
    p.format = MEGLER_CODEC_NV12;
    p.width = W;
    p.height = H;
    p.data = nv12;
    p.chroma = nv12 + (size_t)W * H;
    p.stride = p.chroma_stride = W;
    for (run = 0; run < 10; run++) {
        u64 t = mg_time_monotonic_ns();
        mg_mcodec_render(&p, bgra, W, H, W * 4);
        t = mg_time_monotonic_ns() - t;
        if (t < best) best = t;
    }
    say("renderFrame alone: a picture of %dx%d in NV12 into a bitmap in %.2f ms (the best of ten)", W, H, (double)best / 1e6);
    free(nv12);
    free(bgra);
}

int mg_codec_speed_run(const char *streams, const struct megler_codec_host *host, mg_test_sink sink, void *user, int *checks)
{
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    mg_java_media_set_host(host);
    say("how fast the decoders behind MediaCodec are (the streams in %s)", streams);
    speed_of_render();
    speed_of(streams, "speed-avc-720p.mgau");
    speed_of(streams, "speed-avc-1080p.mgau");
    speed_of(streams, "speed-hevc-1080p.mgau");
    mg_java_media_set_host(NULL);
    *checks += g_checks;
    return g_failed;
}

int mg_codec_tests_h265_run(const char *streams, const struct megler_codec_host *host, const char *how, mg_test_sink sink, void *user, int *checks)
{
    char name[200];
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    mg_java_media_set_host(host);
    snprintf(name, sizeof(name), "H.265, %s", how);
    test_pictures(streams, "hevc.mgau", name);
    snprintf(name, sizeof(name), "H.265 Main 10, %s", how);
    test_pictures(streams, "hevc10.mgau", name);
    mg_java_media_set_host(NULL);
    *checks += g_checks;
    return g_failed;
}

/*
 * Pictures decoded on the graphics processor: avc.mgau through a host whose pictures stay there (MEGLER_CODEC_GPU), each
 * shown as the newest frame of a texture of the gfx host's (show_picture) and drawn by a canvas on the graphics processor
 * (draw_texture), as a TextureView draws a video's, and read into memory too (read_picture): the middle is the grey the
 * picture's number says both ways. 24 pictures, in order, every one of them on the graphics processor.
 */
int mg_codec_gpu_run(const char *streams, const struct megler_codec_host *host, const struct megler_gfx_host *gfx, mg_test_sink sink, void *user,
                     int *checks)
{
    enum { SIDE = 64, ID = 7 };
    stream s;
    mg_mcodec *c;
    u32 *px = (u32 *)mg_alloc(SIDE * SIDE * 4);
    void *cv;
    int i, shown = 0, read = 0, gpu = 0, total = 0, last = -1;
    bool ordered = true;
    i64 info[3];
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    mg_java_media_set_host(host);
    say("pictures decoded on the graphics processor, shown from there and read back (avc.mgau)");
    if (!stream_read(&s, streams, "avc.mgau")) {
        CHECK(!"avc.mgau can be read");
        mg_java_media_set_host(NULL);
        free(px);
        *checks += g_checks;
        return g_failed;
    }
    cv = gfx->canvas_new(gfx->user, px, SIDE, SIDE, SIDE * 4);
    c = open_pictures(&s, CSD);
    CHECK(c != NULL && cv != NULL);
    for (i = 0; c && cv && i < s.nsamples; i++) {
        mg_mframe *f;
        mg_mcodec_decode(c, s.samples[i].data, s.samples[i].size, s.samples[i].time_us, i == s.nsamples - 1 ? 4 : 0);
        while ((f = mg_mcodec_next_frame(c, info)) != NULL) {
            int number = picture_number(info[0]), want = (298 * (16 + 8 * number - 16) + 128) >> 8;
            const megler_codec_picture *p;
            total++;
            if (number <= last) ordered = false;
            last = number;
            if (mg_mcodec_frame_on_gpu(f)) {
                gpu++;
                if (mg_mcodec_frame_show(f, ID)) {
                    u32 mid;
                    int r, g, b;
                    gfx->draw_color(gfx->user, cv, 0xFF000000u, MEGLER_GFX_BLEND_SRC);
                    gfx->draw_texture(gfx->user, cv, ID, 0, 0, SIDE, SIDE, 255);
                    gfx->flush(gfx->user, cv);
                    mid = px[(SIDE / 2) * SIDE + SIDE / 2];
                    b = (int)(mid & 255), g = (int)(mid >> 8 & 255), r = (int)(mid >> 16 & 255);
                    if (abs(r - want) <= 8 && abs(g - want) <= 8 && abs(b - want) <= 8) shown++;
                    else if (shown >= 0) {
                        say("  picture %d shown from the graphics processor: %d, %d, %d, not %d", number, r, g, b, want);
                        shown = -1000;
                    }
                }
            }
            p = mg_mcodec_frame_picture(f);
            if (p && p->format != MEGLER_CODEC_NV12) {
                u32 mid;
                memcpy(&mid, p->data + (size_t)(p->height / 2) * (size_t)p->stride + (size_t)(p->width / 2) * 4, 4);
                if (abs((int)(mid & 255) - want) <= 8 && abs((int)(mid >> 8 & 255) - want) <= 8 && abs((int)(mid >> 16 & 255) - want) <= 8) read++;
                else if (read >= 0) {
                    say("  picture %d read back: %d, %d, %d, not %d", number, (int)(mid >> 16 & 255), (int)(mid >> 8 & 255), (int)(mid & 255), want);
                    read = -1000;
                }
            }
            mg_mcodec_free_frame(f);
        }
    }
    say("  %d pictures, %d of them on the graphics processor, %d shown from there as they should be, %d read back as they should be", total, gpu, shown,
        read);
    if (!gpu) {
        say("  (this host's pictures are not on the graphics processor: nothing more to check)");
    } else {
        CHECK(total == s.nsamples && ordered);
        CHECK(gpu == total);
        CHECK(shown == total);
        CHECK(read == total);
    }
    if (c) mg_mcodec_destroy(c);
    if (cv) gfx->canvas_free(gfx->user, cv);
    gfx->texture_frame(gfx->user, ID, NULL, 0, 0);
    stream_free(&s);
    free(px);
    mg_java_media_set_host(NULL);
    *checks += g_checks;
    return g_failed;
}

int mg_codec_tests_run(const char *streams, const struct megler_codec_host *host, mg_test_sink sink, void *user, int *checks)
{
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    mg_java_media_set_host(host);
    say("the decoders behind MediaCodec, over real ones (the streams in %s)", streams);
    CHECK(mg_mcodec_supported("video/avc") && mg_mcodec_supported("audio/mp4a-latm") && mg_mcodec_supported("audio/mpeg"));
    test_pictures(streams, "avc.mgau", "H.264");
    test_size_change(streams);
    test_pictures(streams, "hevc.mgau", "H.265");
    test_pictures(streams, "hevc10.mgau", "H.265 Main 10");
    test_sound(streams, "aac.mgau", "AAC with its AudioSpecificConfig", false, 1024);
    test_sound(streams, "aac.mgau", "AAC with ADTS headers and nothing else", true, 1024);
    test_sound(streams, "mp3.mgau", "MP3, configured without its rate or channels", false, 1152);
    test_together(streams);
    mg_java_media_set_host(NULL);
    *checks += g_checks;
    return g_failed;
}
