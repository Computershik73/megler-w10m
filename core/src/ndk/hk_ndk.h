/*
 * libandroid.so: the NDK's own objects -- the native window, the looper, the
 * input queue, the asset manager, the configuration -- for the drivers and
 * for EGL. Guest code reaches them through the functions in hk_ndk.c's table.
 */
#ifndef HK_NDK_H
#define HK_NDK_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- ANativeWindow */

typedef struct hk_nwindow_ops hk_nwindow_ops;

typedef struct hk_nwindow {
    volatile i32 refs;
    int width, height;             /* the surface, in pixels */
    int buffer_width, buffer_height;   /* what the app asked to render at (ANativeWindow_setBuffersGeometry); 0: the surface's size */
    int format;
    const hk_nwindow_ops *ops;     /* a window that is not a driver's (a SurfaceView's: drivers/hk_java_ndk.c); NULL for a driver's */
} hk_nwindow;

/*
 * A window of someone else's than the driver that drew the app's whole surface: the Java driver's windows of a SurfaceView's
 * Surface (drivers/hk_java_ndk.c). Its size follows its surface, it can go (surfaceDestroyed) while native code still holds
 * it, and EGL window surfaces and software buffers on it are its own; the NDK's functions and EGL (ndk/hk_egl.c) ask it.
 */
struct hk_nwindow_ops {
    /* Its surface's size now. */
    void (*size)(const hk_nwindow *w, int *width, int *height);
    /* Whether its surface is still there: false from surfaceDestroyed on, for good, as on Android. */
    bool (*valid)(const hk_nwindow *w);
    /* ANativeWindow_lock and ANativeWindow_unlockAndPost: buffer an ANativeWindow_Buffer, dirty an ARect or NULL; 0 or Android's negative error. */
    int (*lock)(hk_nwindow *w, void *buffer, i32 *dirty);
    int (*unlock)(hk_nwindow *w);
    /*
     * An EGL window surface on it (eglCreateWindowSurface), made with the first of `configs` (EGL configurations) that works:
     * its object, NULL when none can be made. Then for that object: the EGLSurface of EGL's own it is now and its size, which
     * the app's surface stands for in every EGL call (it is another after a swap that found the window resized); a swap on the
     * calling thread, the surface current there (false: the window is gone); and its end.
     */
    void *(*egl_create)(hk_nwindow *w, void *const *configs, int count);
    void *(*egl_handle)(void *surface, int *width, int *height);
    bool (*egl_swap)(void *surface);
    void (*egl_destroy)(void *surface);
    /* The last reference went. */
    void (*gone)(hk_nwindow *w);
};

hk_nwindow *hk_nwindow_create(int width, int height);
void hk_nwindow_acquire(hk_nwindow *w);
void hk_nwindow_release(hk_nwindow *w);
/* The size frames are rendered at: what ANativeWindow_setBuffersGeometry asked for, else the surface's now. */
void hk_nwindow_buffer_size(const hk_nwindow *w, int *width, int *height);
/* Whether the window can still be drawn on (a driver's always can). */
bool hk_nwindow_valid(const hk_nwindow *w);

/* ------------------------------------------------------------- AInputQueue */

typedef struct hk_input_queue hk_input_queue;

hk_input_queue *hk_input_queue_create(void);
/* A touch in surface pixels; phase: 0 down, 1 move, 2 up, 3 cancel. Builds the Android motion event, multi-touch included. */
void hk_input_queue_touch(hk_input_queue *q, int phase, int pointer_id, float x, float y);
void hk_input_queue_key(hk_input_queue *q, int keycode, bool down);

/* ----------------------------------------------------------------- ALooper */

/*
 * A thread that runs a Java Looper. On Android such a thread has a native
 * looper under its message queue, which is what ALooper_forThread gives
 * native code there, and descriptors native code adds to it have their
 * callbacks run between the thread's messages. Here the Java message loop says
 * of its thread that it is one (hk_looper_java_thread, once), and asks every
 * turn for what is ready to be run (hk_looper_poll_thread, which never
 * waits). The looper itself is made only when native code first asks for it,
 * so a message loop nobody's native code is interested in costs nothing;
 * hk_looper_poll_thread answers whether the thread has one.
 */
void hk_looper_java_thread(void);
bool hk_looper_poll_thread(void);

/* ----------------------------------------------------------- AAssetManager */

/* The one asset manager: the "assets/" folders of the app's APKs. */
void *hk_asset_manager(void);

/* ----------------------------------------------------------- AConfiguration */

void hk_configuration_set(int width, int height, int density_dpi);

#ifdef __cplusplus
}
#endif

#endif
