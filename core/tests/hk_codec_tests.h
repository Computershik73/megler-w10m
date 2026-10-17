/* The decoders behind MediaCodec, with real decoders under them (hk_codec_tests.c). */
#ifndef HK_CODEC_TESTS_H
#define HK_CODEC_TESTS_H

#include "hk_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The core's codec (drivers/hk_java_media.c) over a host of real decoders -- the shell's own, Media Foundation's
 * transforms, on a PC (hk_tests --codec-windows, core/tests/hk_codec_windows.cpp) -- fed the streams that
 * tools/media-test-streams.py wrote into `streams` (core/tests/app/data/assets), as MediaCodec is fed by an app: what
 * comes out is checked against what the streams hold. H.264: every picture, in display order, the grey its number
 * says, at the visible size, with the parameter sets in each of the forms an app may give them, and after a flush only
 * what came after it. H.265 the same where the host has a decoder for it. AAC (raw and with ADTS headers) and MP3:
 * about as many frames as the packets hold, at 44100 Hz in two channels, a tone of 440 Hz as loud as the stream's. The
 * host is set for the codec while the tests run and taken away after. Returns the number of failed checks and adds the
 * number made to *checks.
 */
struct megler_codec_host;
int hk_codec_tests_run(const char *streams, const struct megler_codec_host *host, hk_test_sink sink, void *user, int *checks);

/*
 * How fast the host's picture decoders are, with the core's codec over them as MediaCodec has it: the streams
 * `tools/media-test-streams.py --speed` makes (H.264 at 720p and 1080p, H.265 at 1080p, two seconds of a moving picture
 * each), those of them that are in `streams`, each decoded whole as fast as it goes: the time a picture takes to decode
 * and to be turned into a bitmap (renderFrame), and how many units went in before the first picture came out. Checked:
 * every picture came out, in order, at the stream's size. Returns the number of failed checks and adds the number made
 * to *checks.
 */
int hk_codec_speed_run(const char *streams, const struct megler_codec_host *host, hk_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
