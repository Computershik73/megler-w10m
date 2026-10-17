/*
 * Hardware buffers (AHardwareBuffer, libandroid; ndk/hk_ahb.c) and what the layer's EGL needs to know of them.
 *
 * On Android an AHardwareBuffer is a GraphicBuffer, memory gralloc gives the graphics processor, the display and the CPU to
 * share. Here it is memory of the layer's own for the CPU and, from the first time an EGL image of it is asked for
 * (EGL_ANDROID_image_native_buffer, ndk/hk_egl.c), a texture of ANGLE's for the graphics processor, made in a context of the
 * layer's own; locking it for reading reads that texture back, unlocking it after writing uploads it again.
 *
 * Code that is given one may look inside it as Android lays a GraphicBuffer out: ANGLE, inside Chromium's WebView, takes
 * the ANativeWindowBuffer two pointers into it (EGLClientBuffer, android_util.cpp in AOSP's external/angle) and reads its
 * width, height, layer count, format and usage there. So a buffer begins as a GraphicBuffer does on 32-bit ARM -- two
 * words, then the ANativeWindowBuffer -- and what is the layer's own comes after.
 */
#ifndef HK_AHB_H
#define HK_AHB_H

#include "../hk_base.h"
#include "../host/hk_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Android's ANativeWindowBuffer as 32-bit ARM lays it out (nativebase.h): 96 bytes. Pointers are words of the guest's. */
typedef struct hk_ahb_anwb {
    i32 magic, version;                /* android_native_base_t: '_bfr', sizeof(ANativeWindowBuffer) */
    u32 reserved[4];
    u32 inc_ref, dec_ref;              /* its reference counting: functions of the layer's (32-bit ARM hosts), else 0 */
    i32 width, height, stride, format, usage_deprecated;
    u32 layer_count;
    u32 reserved1;
    u32 handle;                        /* const native_handle_t *: none */
    u32 usage_lo, usage_hi;            /* uint64_t usage */
    u32 reserved_proc[6];
} hk_ahb_anwb;

#define HK_AHB_ANWB_OFFSET 8           /* where the ANativeWindowBuffer is in an AHardwareBuffer, as ANGLE has it */

typedef struct hk_ahb {
    u32 vtable, refbase;               /* where a GraphicBuffer has its vtable and RefBase's member: nothing here */
    hk_ahb_anwb anwb;
    /* The layer's own. */
    u32 magic;
    volatile i32 refs;
    u32 width, height, format, bpp;    /* AHARDWAREBUFFER_FORMAT_*; bytes a pixel */
    u64 usage;
    hk_lock lock;                      /* what follows */
    u8 *pixels;                        /* the CPU's copy, width * height * bpp, rows top first: made at the first lock */
    u64 locked_usage;                  /* what the lock that is held asked for */
    int locks;
    u32 texture;                       /* ANGLE's texture in the layer's own context; 0 until an EGL image is asked for */
    bool cpu_newer;                    /* `pixels` were written since the texture last had them */
    u64 id;                            /* AHardwareBuffer_getId */
} hk_ahb;

/* The buffer an EGLClientBuffer (an ANativeWindowBuffer *) or an AHardwareBuffer * is, or NULL when it is not one of these. */
hk_ahb *hk_ahb_of_client_buffer(const void *client_buffer);
hk_ahb *hk_ahb_of(const void *ahardwarebuffer);
/* What OpenGL ES calls its pixels, for glTexImage2D and glReadPixels; false for a format no texture is made of (BLOB). */
bool hk_ahb_gl_format(const hk_ahb *b, u32 *format, u32 *type);

#ifdef __cplusplus
}
#endif

#endif
