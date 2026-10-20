/*
 * A stand-in for the shell's decoders (meglet_codec_host), for tests where there are none: portable C that decodes
 * nothing, but takes samples as a real decoder must be given them and hands out what can be checked.
 *
 * H.264 and AAC and MP3; not H.265, so that a kind the shell lacks is seen missing. A unit of H.264 must be Annex-B
 * with a slice in it, and the first after open or flush must have the SPS and the PPS before its first slice; an AAC
 * packet must be raw (no ADTS header) and the decoder opened with an AudioSpecificConfig. A sample that is not so is
 * refused (push says false) and counted. Each unit taken is one picture, handed out in the order the units came --
 * decode order, as Apple's VideoToolbox does it, so the stand-in says `reorder` 4 and the layer puts them in display
 * order. A picture is NV12 of the size the decoder was opened with (64 x 48 when none was said), one flat grey whose
 * luma says its time: hk_codec_standin_luma. Each AAC packet is 1024 frames of sound at the rate and channels opened
 * with, each MP3 push 1152; the samples are a 440 Hz tone at a quarter of full scale.
 */
#ifndef HK_CODEC_STANDIN_H
#define HK_CODEC_STANDIN_H

#include "../include/meglet.h"

#ifdef __cplusplus
extern "C" {
#endif

const meglet_codec_host *hk_codec_standin(void);
/* The luma of the stand-in's picture for a sample of that time: 16 + 8 N for the Nth thirtieth of a second, N below 28. */
int hk_codec_standin_luma(int64_t time_us);

typedef struct hk_codec_standin_counts {
    int opened, closed, pushed, refused, flushed, drained, pictures_out, pictures_released;
} hk_codec_standin_counts;
void hk_codec_standin_counts_get(hk_codec_standin_counts *out);

#ifdef __cplusplus
}
#endif

#endif
