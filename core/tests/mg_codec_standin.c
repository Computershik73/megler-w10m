/* The stand-in for the shell's decoders: mg_codec_standin.h says what it does. */
#include "mg_codec_standin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mg_base.h"
#include "../src/host/mg_host.h"

typedef struct picture {
    megler_codec_picture p;
    struct picture *next;
    u8 pixels[1];
} picture;

typedef struct block {
    struct block *next;
    int frames;
    int64_t time_us;
    i16 samples[1];
} block;

typedef struct decoder {
    int kind, width, height, rate, channels;
    bool fresh;
    picture *pictures, **pictures_tail;
    block *blocks, **blocks_tail, *handed;
    i64 frame_at;                      /* frames of sound made so far: the tone goes on across packets */
} decoder;

static mg_lock g_lock = MG_LOCK_INIT;
static mg_codec_standin_counts g_counts;

static void count(int *field)
{
    mg_lock_acquire(&g_lock);
    (*field)++;
    mg_lock_release(&g_lock);
}

void mg_codec_standin_counts_get(mg_codec_standin_counts *out)
{
    mg_lock_acquire(&g_lock);
    *out = g_counts;
    mg_lock_release(&g_lock);
}

int mg_codec_standin_luma(int64_t time_us)
{
    i64 n = (time_us * 30 + 500000) / 1000000;
    return 16 + 8 * (int)((n % 28 + 28) % 28);
}

static bool supports(void *user, int kind)
{
    (void)user;
    return kind == MEGLER_CODEC_H264 || kind == MEGLER_CODEC_AAC || kind == MEGLER_CODEC_MP3;
}

static void *open_decoder(void *user, int kind, const megler_codec_format *f)
{
    decoder *d;
    (void)user;
    if (!supports(NULL, kind)) return NULL;
    if (kind == MEGLER_CODEC_AAC && (!f->config || f->config_size < 2)) return NULL;
    if (kind != MEGLER_CODEC_H264 && (f->sample_rate <= 0 || f->channels <= 0)) return NULL;
    d = (decoder *)mg_alloc(sizeof(*d));
    d->kind = kind;
    d->width = f->width > 0 ? f->width : 64;
    d->height = f->height > 0 ? f->height : 48;
    d->rate = f->sample_rate;
    d->channels = f->channels > 2 ? 2 : f->channels;
    d->fresh = true;
    d->pictures_tail = &d->pictures;
    d->blocks_tail = &d->blocks;
    count(&g_counts.opened);
    return d;
}

/* Whether an access unit is Annex-B with a slice in it, and (for a fresh decoder) the SPS and the PPS before the first. */
static bool unit_fit(const u8 *p, size_t n, bool fresh)
{
    size_t i = 0;
    bool sps = false, pps = false, slice = false;
    if (n < 4 || p[0] || p[1] || !(p[2] == 1 || (!p[2] && p[3] == 1))) return false;
    while (i + 3 < n) {
        if (!p[i] && !p[i + 1] && p[i + 2] == 1) {
            int t = p[i + 3] & 0x1f;
            if (t == 7) sps = true;
            else if (t == 8) pps = true;
            else if (t >= 1 && t <= 5) {
                if (fresh && !(sps && pps)) return false;
                slice = true;
            }
            i += 3;
        } else {
            i++;
        }
    }
    return slice;
}

static void add_sound(decoder *d, int frames, int64_t time_us)
{
    block *b = (block *)mg_alloc(sizeof(*b) + (size_t)frames * (size_t)d->channels * 2);
    int i, k;
    b->frames = frames;
    b->time_us = time_us;
    for (i = 0; i < frames; i++, d->frame_at++) {
        i16 v = (i16)(sin((double)d->frame_at * 2 * 3.14159265358979 * 440 / d->rate) * 8192);
        for (k = 0; k < d->channels; k++) b->samples[i * d->channels + k] = v;
    }
    *d->blocks_tail = b;
    d->blocks_tail = &b->next;
}

static bool push(void *user, void *codec, const uint8_t *data, size_t size, int64_t time_us)
{
    decoder *d = (decoder *)codec;
    (void)user;
    count(&g_counts.pushed);
    if (d->kind == MEGLER_CODEC_H264) {
        size_t luma = (size_t)d->width * (size_t)d->height, chroma = luma / 2;
        picture *pic;
        if (!unit_fit(data, size, d->fresh)) {
            count(&g_counts.refused);
            return false;
        }
        d->fresh = false;
        pic = (picture *)mg_alloc(sizeof(*pic) + luma + chroma);
        memset(pic->pixels, mg_codec_standin_luma(time_us), luma);
        memset(pic->pixels + luma, 128, chroma);
        pic->p.format = MEGLER_CODEC_NV12;
        pic->p.width = d->width;
        pic->p.height = d->height;
        pic->p.data = pic->pixels;
        pic->p.stride = d->width;
        pic->p.chroma = pic->pixels + luma;
        pic->p.chroma_stride = d->width;
        pic->p.time_us = time_us;
        *d->pictures_tail = pic;
        d->pictures_tail = &pic->next;
        return true;
    }
    if (d->kind == MEGLER_CODEC_AAC && size >= 2 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0) {
        count(&g_counts.refused);
        return false;
    }
    if (!size) {
        count(&g_counts.refused);
        return false;
    }
    add_sound(d, d->kind == MEGLER_CODEC_AAC ? 1024 : 1152, time_us);
    return true;
}

static void drain(void *user, void *codec)
{
    (void)user;
    (void)codec;
    count(&g_counts.drained);
}

static void *pull_picture(void *user, void *codec, megler_codec_picture *out)
{
    decoder *d = (decoder *)codec;
    picture *pic = d->pictures;
    (void)user;
    if (!pic) return NULL;
    d->pictures = pic->next;
    if (!d->pictures) d->pictures_tail = &d->pictures;
    *out = pic->p;
    count(&g_counts.pictures_out);
    return pic;
}

static void release_picture(void *user, void *pic)
{
    (void)user;
    free(pic);
    count(&g_counts.pictures_released);
}

static bool pull_sound(void *user, void *codec, megler_codec_sound *out)
{
    decoder *d = (decoder *)codec;
    block *b = d->blocks;
    (void)user;
    free(d->handed);
    d->handed = NULL;
    if (!b) return false;
    d->blocks = b->next;
    if (!d->blocks) d->blocks_tail = &d->blocks;
    d->handed = b;
    out->samples = b->samples;
    out->frames = b->frames;
    out->sample_rate = d->rate;
    out->channels = d->channels;
    out->time_us = b->time_us;
    return true;
}

static void drop(decoder *d)
{
    while (d->pictures) {
        picture *next = d->pictures->next;
        release_picture(NULL, d->pictures);
        d->pictures = next;
    }
    d->pictures_tail = &d->pictures;
    while (d->blocks) {
        block *next = d->blocks->next;
        free(d->blocks);
        d->blocks = next;
    }
    d->blocks_tail = &d->blocks;
    free(d->handed);
    d->handed = NULL;
}

static void flush(void *user, void *codec)
{
    decoder *d = (decoder *)codec;
    (void)user;
    drop(d);
    d->fresh = true;
    count(&g_counts.flushed);
}

static void close_decoder(void *user, void *codec)
{
    (void)user;
    drop((decoder *)codec);
    free(codec);
    count(&g_counts.closed);
}

const megler_codec_host *mg_codec_standin(void)
{
    static const megler_codec_host host = { NULL, 4, supports, open_decoder, push, drain, pull_picture, release_picture, pull_sound, flush, close_decoder };
    return &host;
}
