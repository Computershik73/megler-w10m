/* The Java driver's own interface to the rest of the core and to the tests (mg_java.c). */
#ifndef MG_JAVA_H
#define MG_JAVA_H

#include "../../include/megler.h"
#include "../mg_base.h"
#include "../jni/mg_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the running Java app has done so far: pictures handed to the shell, and turns of its main loop that asked for input. */
typedef struct mg_java_stats {
    volatile i32 frames;
    volatile i32 polls;
} mg_java_stats;
void mg_java_get_stats(mg_java_stats *out);
/*
 * For the tests: every frame a window draws in part (its dirty region) is checked against the same frame drawn whole, and a
 * difference is a line in the log ("ViewRoot: ... drawn in part missed"). Set before the app starts. Costs a second drawing
 * of the screen per frame.
 */
void mg_java_check_partial(bool on);
/*
 * Whether the app's main thread has ended, and how: the status and the
 * reason mg_run_ended was given. For a test that starts the driver itself, on
 * a host where megler_run refuses every app because guest code cannot run
 * there -- which a Java app without native libraries does not need.
 */
bool mg_java_ended(int *status, const char **reason);

/*
 * The shell's keyboard (megler.h, megler_keyboard_host): text, editing keys, the keyboard put away by the user, and how much of
 * the surface it covers. From any thread; queued for the app's main thread. Nothing happens when no Java app runs.
 */
void mg_java_text(const char *utf8);
/* megler_sms_received, megler_call_logged: queued for the app's main thread (megler.Phone, Native.pollPhone). */
void mg_java_phone_sms(const char *from, const char *body, i64 time_ms);
void mg_java_phone_call(const char *number, i64 time_ms, int type, int duration_s);
void mg_java_text_key(int key);
void mg_java_keyboard_closed(void);
/* An activity of the app another app asks for (megler_start_activity): started from the main loop. */
void mg_java_start_activity(const char *activity_class);
void mg_java_keyboard_height(int pixels);
/* How much of each edge of the surface the phone's bars cover (megler.h, megler_set_insets). Kept for the next app when none runs. */
void mg_java_set_insets(int left, int top, int right, int bottom);
/* The phone's accent colour and night (megler.h, megler_set_theme), for the next app to read as it starts. */
void mg_java_set_theme(uint32_t accent_argb, bool night);

/* mg_java_sqlite.c: megler.Sqlite's native methods, on the SQLite compiled into the core -- or, in a build without it, their refusal. */
const mg_jhle *mg_java_sqlite_natives(void);
/* megler.Audio: AudioTrack, MediaPlayer and SoundPool (mg_java_audio.c). */
const mg_jhle *mg_java_audio_natives(void);

/*
 * megler.MediaCodecNative: the decoders behind MediaCodec, over the shell's (megler_codec_host; mg_java_media.c). The host
 * is copied; NULL or one left zero lists no decoder. Set it before the app's code runs.
 */
const mg_jhle *mg_java_media_natives(void);
void mg_java_media_set_host(const megler_codec_host *host);
/*
 * The same decoders without Java, as the natives use them, for the tests. A codec is made for a MIME type and configured
 * with what MediaFormat says (csd-0 to csd-2: NULL or empty when not there); each decode is one queued buffer with
 * MediaCodec's flags (2 codec-specific data, 4 the end of the stream). Frames come in display order with info = { time,
 * width, height }; sound as 16-bit PCM in a block to free(), info = { time, rate, channels }.
 */
typedef struct mg_mcodec mg_mcodec;
typedef struct mg_mframe mg_mframe;
bool mg_mcodec_supported(const char *mime);
mg_mcodec *mg_mcodec_create(const char *mime);
bool mg_mcodec_configure(mg_mcodec *c, i32 width, i32 height, i32 rate, i32 channels, const u8 *const csd[3], const size_t csd_size[3]);
void mg_mcodec_decode(mg_mcodec *c, const u8 *data, size_t size, i64 time_us, i32 flags);
mg_mframe *mg_mcodec_next_frame(mg_mcodec *c, i64 info[3]);
u8 *mg_mcodec_next_pcm(mg_mcodec *c, i64 info[3], size_t *bytes);
/* The frame's picture in memory: one on the graphics processor is read into it the first time (NULL where it cannot be). */
const megler_codec_picture *mg_mcodec_frame_picture(mg_mframe *f);
/* Whether the frame's picture is on the graphics processor (MEGLER_CODEC_GPU), until it is read into memory. */
bool mg_mcodec_frame_on_gpu(const mg_mframe *f);
/* The frame's picture shown as the newest of the gfx host's texture `texture_id`, where it is on the graphics processor. */
bool mg_mcodec_frame_show(mg_mframe *f, i32 texture_id);
/* A picture into memory of the core's bitmaps (B, G, R, A, top row first), as much of it as fits. */
bool mg_mcodec_render(const megler_codec_picture *p, void *bgra, i32 width, i32 height, i32 stride);
void mg_mcodec_free_frame(mg_mframe *f);
void mg_mcodec_flush(mg_mcodec *c);
void mg_mcodec_destroy(mg_mcodec *c);
/* width << 16 | height: as configured, then the latest picture's. */
i32 mg_mcodec_size(mg_mcodec *c);

/*
 * megler.CameraNative: the cameras behind android.hardware.Camera, over the shell's (megler_camera_host; mg_java_camera.c). The
 * host is copied; NULL or one left zero has no camera. Set it before the app's code runs. When the app ends, whatever it left
 * open is closed (mg_java_camera_close_all).
 */
const mg_jhle *mg_java_camera_natives(void);
void mg_java_camera_set_host(const megler_camera_host *host);
/*
 * megler.LocationNative: the phone's location behind android.location.LocationManager, over the shell's (megler_location_host;
 * mg_java_location.c). The host is copied; NULL or one left zero finds nothing. The same without Java for the tests:
 * whether the app may have it (MEGLER_LOCATION_*, the user asked first with `ask`), start and stop, the newest fix.
 */
const mg_jhle *mg_java_location_natives(void);
void mg_java_location_set_host(const megler_location_host *host);
int mg_jlocation_access(bool ask);
bool mg_jlocation_start(int interval_ms);
void mg_jlocation_stop(void);
bool mg_jlocation_last(megler_location *fix);
void mg_java_camera_close_all(void);
/*
 * The same without Java, as the natives use them, for the tests. A camera is opened by its number and started at one of its
 * sizes; mg_jcamera_wait waits for news, and MG_JCAMERA_FRAME in it means a new picture is held, which the calls after it
 * read until the next: copied as NV21 or YV12 (Android's ImageFormat numbers, 17 and 0x32315659; -1 when `size` is too
 * small), or drawn into memory of the core's bitmaps mirrored and turned as a preview is shown.
 */
enum { MG_JCAMERA_FRAME = 1, MG_JCAMERA_FOCUSED = 2, MG_JCAMERA_NOT_FOCUSED = 4, MG_JCAMERA_GONE = 8 };
enum { MG_JCAMERA_ANY = 1, MG_JCAMERA_HAS_BACK = 2, MG_JCAMERA_HAS_FRONT = 4, MG_JCAMERA_HAS_AUTOFOCUS = 8, MG_JCAMERA_HAS_FLASH = 16 };
typedef struct mg_jcamera mg_jcamera;
int mg_jcamera_count(void);
bool mg_jcamera_info(int index, megler_camera_info *out);
int mg_jcamera_features(void);
mg_jcamera *mg_jcamera_open(int index, int *error);
const megler_camera_caps *mg_jcamera_caps(mg_jcamera *c);
bool mg_jcamera_start(mg_jcamera *c, int width, int height, int fps);
void mg_jcamera_stop(mg_jcamera *c);
int mg_jcamera_wait(mg_jcamera *c, int ms);
/* The picture held: width << 16 | height, and when it was taken. */
i32 mg_jcamera_size(mg_jcamera *c);
i64 mg_jcamera_time(mg_jcamera *c);
size_t mg_jcamera_frame_bytes(i32 width, i32 height, int format);
i32 mg_jcamera_copy(mg_jcamera *c, u8 *dst, size_t size, int format);
bool mg_jcamera_render(mg_jcamera *c, void *bgra, i32 width, i32 height, i32 stride, int rotation, bool mirror);
/* Since it was last started: the pictures kept, and those dropped for coming sooner than the rate it was started at. */
void mg_jcamera_counts(mg_jcamera *c, i64 *kept, i64 *dropped);
bool mg_jcamera_focus(mg_jcamera *c, int mode);
void mg_jcamera_torch(mg_jcamera *c, bool on);
void mg_jcamera_close(mg_jcamera *c);

/* mg_java_webview.c: what Android's platform gives the WebView besides Java (its library loader, its platform support). */
const mg_jhle *mg_java_webview_natives(void);
/*
 * For the tests: the draw functor's table (AwDrawFnFunctionTable), whether or not Chromium would be given it; a functor drawn
 * as megler.WebFunctors.draw has it drawn, on the render thread (the shell's id of its picture | the pbuffer's width << 32 |
 * its height << 48, or 0); and the render thread's EGL forgotten, for a test that gives the core another OpenGL ES, while
 * no functor is drawn.
 */
const void *mg_java_webview_draw_fn_table(void);
i64 mg_java_webview_draw_functor(i32 functor, int left, int top, int width, int height);
void mg_java_webview_forget_egl(void);

#ifdef __cplusplus
}
#endif

#endif
