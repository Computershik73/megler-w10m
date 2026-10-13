/*
 * The NDK's functions that are about the framework's Java objects, for the
 * native libraries of a Java app. The rest of the NDK is in ndk/ and needs no
 * Java; these need the Java driver and are made known to the linker when it
 * starts (hk_java_ndk_start).
 *
 * libjnigraphics: AndroidBitmap_getInfo, AndroidBitmap_lockPixels,
 * AndroidBitmap_unlockPixels on an android.graphics.Bitmap. The Bitmap's
 * pixels are the core's own memory (hk_java_gfx.c), kept as B, G, R, A,
 * premultiplied; native code is promised ANDROID_BITMAP_FORMAT_RGBA_8888, so
 * a lock turns the memory to that order where it lies and the unlock turns
 * it back and takes the bitmap as changed (hk_java_gfx_lock_rgba). Every
 * Bitmap is 32 bits a pixel here whatever its Config says, and that is the
 * format reported: code that asks for the format first, as the NDK's
 * documentation has it do, is told the truth.
 */
#include "../bionic/hk_bionic.h"
#include "../dvm/hk_dvm_internal.h"
#include "hk_java_gfx.h"

enum { G_BITMAP_OK = 0, G_BITMAP_BAD_PARAMETER = -1, G_BITMAP_JNI_EXCEPTION = -2 };
enum { G_BITMAP_FORMAT_RGBA_8888 = 1 };

/* AndroidBitmapInfo. */
typedef struct g_bitmap_info {
    u32 width, height, stride;
    i32 format;
    u32 flags;                         /* 0: premultiplied alpha, no hardware buffer */
} g_bitmap_info;

/* The core's bitmap of an android.graphics.Bitmap: its field mNative; 0 for anything else, and for a recycled one. */
static i64 bitmap_of(hk_jobj *bitmap)
{
    hk_jvalue v;
    v.j = 0;
    if (!bitmap || !bitmap->dvm || !hk_dvm_instance_of(bitmap, "android/graphics/Bitmap")) return 0;
    if (!hk_dvm_get_field(bitmap, "mNative", &v)) hk_dvm_clear_pending();
    return v.j;
}

static int g_bitmap_get_info(void *env, hk_jobj *bitmap, g_bitmap_info *info)
{
    hk_java_gfx_bitmap b;
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (!bm || !hk_java_gfx_bitmap_info(bm, &b)) return G_BITMAP_BAD_PARAMETER;
    if (info) {
        info->width = (u32)b.width;
        info->height = (u32)b.height;
        info->stride = (u32)b.stride;
        info->format = G_BITMAP_FORMAT_RGBA_8888;
        info->flags = 0;
    }
    return G_BITMAP_OK;
}

static int g_bitmap_lock(void *env, hk_jobj *bitmap, void **address)
{
    hk_java_gfx_bitmap b;
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (address) *address = NULL;
    if (!bm) return G_BITMAP_BAD_PARAMETER;
    if (!hk_java_gfx_lock_rgba(bm, &b)) return G_BITMAP_JNI_EXCEPTION;
    if (address) *address = b.pixels;
    return G_BITMAP_OK;
}

static int g_bitmap_unlock(void *env, hk_jobj *bitmap)
{
    i64 bm = bitmap_of(bitmap);
    (void)env;
    if (!bm) return G_BITMAP_BAD_PARAMETER;
    hk_java_gfx_unlock_rgba(bm);
    return G_BITMAP_OK;
}

static const hk_sym k_java_ndk[] = {
    HK_WRAP("AndroidBitmap_getInfo", g_bitmap_get_info), HK_WRAP("AndroidBitmap_lockPixels", g_bitmap_lock),
    HK_WRAP("AndroidBitmap_unlockPixels", g_bitmap_unlock),
    HK_END
};

void hk_java_ndk_start(void)
{
    static bool registered;
    if (registered) return;
    registered = true;
    hk_bionic_register(k_java_ndk);
}
