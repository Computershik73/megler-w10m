/*
 * meglet.Gfx: the native methods under the Java framework's android.graphics
 * (mg_java_gfx.c). The Java driver registers the table and says where the
 * drawing goes; everything else here is for the rest of the core.
 */
#ifndef MG_JAVA_GFX_H
#define MG_JAVA_GFX_H

#include "../../include/meglet.h"
#include "../mg_base.h"
#include "../jni/mg_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The table for mg_dvm_register: every native method of the class "meglet/Gfx". */
const mg_jhle *mg_java_gfx_natives(void);
/* Who draws. The struct is copied; NULL: nobody, and the natives do what they can without. Set it before the app's code runs. */
void mg_java_gfx_set_host(const meglet_gfx_host *host);
/* What Gfx.present(bm) hands on: the bitmap's memory, with everything drawn into it there, or NULL pixels when bm is 0. */
void mg_java_gfx_set_present(void (*fn)(void *user, const void *pixels, int width, int height, int stride), void *user);
/*
 * What Gfx.present(bm) tries first, when it is set: the shell's canvas that alone draws the bitmap, unflushed, to be shown
 * from the shell's own picture (meglet_gfx_host.present). True: shown, and the memory is left as it is. False: the memory is
 * brought up to date and handed to the function above, as without this. Called under the bitmaps' lock.
 */
void mg_java_gfx_set_present_canvas(bool (*fn)(void *user, void *canvas, int width, int height), void *user);
/*
 * After a picture shown that way: when `due` says so, the bitmap's memory is brought up to date and handed to `probe` (a
 * diagnosis of what the window shows: mg_java_gl.c's trace-surfaces). Not under the bitmaps' lock.
 */
void mg_java_gfx_set_present_probe(bool (*due)(void), void (*probe)(const void *pixels, int width, int height, int stride));
/* Bytes of bitmap memory now allocated and bitmaps alive, for the memory report. */
void mg_java_gfx_stats(uint64_t *bitmap_bytes, uint32_t *bitmaps);
/* Bytes the display lists of RenderNodes and Pictures now take, how many lists there are, and how many nodes. */
void mg_java_gfx_list_stats(uint64_t *bytes, uint32_t *lists, uint32_t *nodes);
/* Nanoseconds the calling thread has spent in the shell's drawing (the meglet_gfx_host's calls), summed: the frame meter's. */
u64 mg_java_gfx_host_ns(void);

/*
 * A bitmap by its handle, which is what android.graphics.Bitmap keeps in its
 * field mNative: for libjnigraphics (AndroidBitmap_getInfo, _lockPixels) and
 * for anything else in the core that fills a Bitmap from native code.
 *
 * The memory is the core's order, B, G, R, A, premultiplied, top row first.
 * Android's own (ANDROID_BITMAP_FORMAT_RGBA_8888, which is what native code
 * that locks a bitmap expects to find) is R, G, B, A. So a lock for the app's
 * native code is mg_java_gfx_lock_rgba: it turns the memory to Android's
 * order where it lies, and mg_java_gfx_unlock_rgba turns it back and takes it
 * as changed. Between the two nothing else may read or draw the bitmap,
 * which Android asks of the app as well. Locks of one bitmap nest.
 *
 * False for a handle that is zero or not a live bitmap.
 */
typedef struct mg_java_gfx_bitmap {
    void *pixels;
    i32 width, height, stride;
} mg_java_gfx_bitmap;
/* The memory as it is (B, G, R, A), everything drawn so far in it. */
bool mg_java_gfx_bitmap_info(i64 bm, mg_java_gfx_bitmap *out);
/* After writing through that address in the core's own order. */
void mg_java_gfx_bitmap_changed(i64 bm);
bool mg_java_gfx_lock_rgba(i64 bm, mg_java_gfx_bitmap *out);
void mg_java_gfx_unlock_rgba(i64 bm);

#ifdef __cplusplus
}
#endif

#endif
