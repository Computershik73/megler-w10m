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

/*
 * megler.CameraNative: the cameras behind android.hardware.Camera, over the shell's (megler_camera_host; hk_java_camera.c). The
 * host is copied; NULL or one left zero has no camera. Set it before the app's code runs. When the app ends, whatever it left
 * open is closed (hk_java_camera_close_all).
 */
const hk_jhle *hk_java_camera_natives(void);
void hk_java_camera_set_host(const megler_camera_host *host);
/*
 * megler.LocationNative: the phone's location behind android.location.LocationManager, over the shell's (megler_location_host;
 * hk_java_location.c). The host is copied; NULL or one left zero finds nothing. The same without Java for the tests:
 * whether the app may have it (MEGLER_LOCATION_*, the user asked first with `ask`), start and stop, the newest fix.
 */
const hk_jhle *hk_java_location_natives(void);
void hk_java_location_set_host(const megler_location_host *host);
int hk_jlocation_access(bool ask);
bool hk_jlocation_start(int interval_ms);
void hk_jlocation_stop(void);
bool hk_jlocation_last(megler_location *fix);
void hk_java_camera_close_all(void);
/*
 * The same without Java, as the natives use them, for the tests. A camera is opened by its number and started at one of its
 * sizes; hk_jcamera_wait waits for news, and HK_JCAMERA_FRAME in it means a new picture is held, which the calls after it
 * read until the next: copied as NV21 or YV12 (Android's ImageFormat numbers, 17 and 0x32315659; -1 when `size` is too
 * small), or drawn into memory of the core's bitmaps mirrored and turned as a preview is shown.
 */
enum { HK_JCAMERA_FRAME = 1, HK_JCAMERA_FOCUSED = 2, HK_JCAMERA_NOT_FOCUSED = 4, HK_JCAMERA_GONE = 8 };
enum { HK_JCAMERA_ANY = 1, HK_JCAMERA_HAS_BACK = 2, HK_JCAMERA_HAS_FRONT = 4, HK_JCAMERA_HAS_AUTOFOCUS = 8, HK_JCAMERA_HAS_FLASH = 16 };
typedef struct hk_jcamera hk_jcamera;
int hk_jcamera_count(void);
bool hk_jcamera_info(int index, megler_camera_info *out);
int hk_jcamera_features(void);
hk_jcamera *hk_jcamera_open(int index, int *error);
const megler_camera_caps *hk_jcamera_caps(hk_jcamera *c);
bool hk_jcamera_start(hk_jcamera *c, int width, int height, int fps);
void hk_jcamera_stop(hk_jcamera *c);
int hk_jcamera_wait(hk_jcamera *c, int ms);
/* The picture held: width << 16 | height, and when it was taken. */
i32 hk_jcamera_size(hk_jcamera *c);
i64 hk_jcamera_time(hk_jcamera *c);
size_t hk_jcamera_frame_bytes(i32 width, i32 height, int format);
i32 hk_jcamera_copy(hk_jcamera *c, u8 *dst, size_t size, int format);
bool hk_jcamera_render(hk_jcamera *c, void *bgra, i32 width, i32 height, i32 stride, int rotation, bool mirror);
/* Since it was last started: the pictures kept, and those dropped for coming sooner than the rate it was started at. */
void hk_jcamera_counts(hk_jcamera *c, i64 *kept, i64 *dropped);
bool hk_jcamera_focus(hk_jcamera *c, int mode);
void hk_jcamera_torch(hk_jcamera *c, bool on);
void hk_jcamera_close(hk_jcamera *c);

/* hk_java_webview.c: what Android's platform gives the WebView besides Java (its library loader, its platform support). */
const hk_jhle *hk_java_webview_natives(void);

#ifdef __cplusplus
}
#endif

#endif
