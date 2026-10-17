/* The Java driver's own interface to the rest of the core and to the tests (hk_java.c). */
#ifndef HK_JAVA_H
#define HK_JAVA_H

#include "../../include/megler.h"
#include "../hk_base.h"
#include "../jni/hk_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the running Java app has done so far: pictures handed to the shell, and turns of its main loop that asked for input. */
typedef struct hk_java_stats {
    volatile i32 frames;
    volatile i32 polls;
} hk_java_stats;
void hk_java_get_stats(hk_java_stats *out);
/*
 * For the tests: every frame a window draws in part (its dirty region) is checked against the same frame drawn whole, and a
 * difference is a line in the log ("ViewRoot: ... drawn in part missed"). Set before the app starts. Costs a second drawing
 * of the screen per frame.
 */
void hk_java_check_partial(bool on);
/*
 * Whether the app's main thread has ended, and how: the status and the
 * reason hk_run_ended was given. For a test that starts the driver itself, on
 * a host where megler_run refuses every app because guest code cannot run
 * there -- which a Java app without native libraries does not need.
 */
bool hk_java_ended(int *status, const char **reason);

/*
 * The shell's keyboard (megler.h, megler_keyboard_host): text, editing keys, the keyboard put away by the user, and how much of
 * the surface it covers. From any thread; queued for the app's main thread. Nothing happens when no Java app runs.
 */
void hk_java_text(const char *utf8);
void hk_java_text_key(int key);
void hk_java_keyboard_closed(void);
void hk_java_keyboard_height(int pixels);

/* hk_java_sqlite.c: megler.Sqlite's native methods, on the SQLite compiled into the core -- or, in a build without it, their refusal. */
const hk_jhle *hk_java_sqlite_natives(void);
/* megler.Audio: AudioTrack, MediaPlayer and SoundPool (hk_java_audio.c). */
const hk_jhle *hk_java_audio_natives(void);

/*
 * megler.MediaCodecNative: the decoders behind MediaCodec, over the shell's (megler_codec_host; hk_java_media.c). The host
 * is copied; NULL or one left zero lists no decoder. Set it before the app's code runs.
 */
const hk_jhle *hk_java_media_natives(void);
void hk_java_media_set_host(const megler_codec_host *host);
/*
 * The same decoders without Java, as the natives use them, for the tests. A codec is made for a MIME type and configured
 * with what MediaFormat says (csd-0 to csd-2: NULL or empty when not there); each decode is one queued buffer with
 * MediaCodec's flags (2 codec-specific data, 4 the end of the stream). Frames come in display order with info = { time,
 * width, height }; sound as 16-bit PCM in a block to free(), info = { time, rate, channels }.
 */
typedef struct hk_mcodec hk_mcodec;
typedef struct hk_mframe hk_mframe;
bool hk_mcodec_supported(const char *mime);
hk_mcodec *hk_mcodec_create(const char *mime);
bool hk_mcodec_configure(hk_mcodec *c, i32 width, i32 height, i32 rate, i32 channels, const u8 *const csd[3], const size_t csd_size[3]);
void hk_mcodec_decode(hk_mcodec *c, const u8 *data, size_t size, i64 time_us, i32 flags);
hk_mframe *hk_mcodec_next_frame(hk_mcodec *c, i64 info[3]);
u8 *hk_mcodec_next_pcm(hk_mcodec *c, i64 info[3], size_t *bytes);
const megler_codec_picture *hk_mcodec_frame_picture(const hk_mframe *f);
/* A picture into memory of the core's bitmaps (B, G, R, A, top row first), as much of it as fits. */
bool hk_mcodec_render(const megler_codec_picture *p, void *bgra, i32 width, i32 height, i32 stride);
void hk_mcodec_free_frame(hk_mframe *f);
void hk_mcodec_flush(hk_mcodec *c);
void hk_mcodec_destroy(hk_mcodec *c);
/* width << 16 | height: as configured, then the latest picture's. */
i32 hk_mcodec_size(hk_mcodec *c);

#ifdef __cplusplus
}
#endif

#endif
