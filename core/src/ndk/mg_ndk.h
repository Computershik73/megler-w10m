/*
 * libandroid.so: the NDK's own objects -- the native window, the looper, the
 * input queue, the asset manager, the configuration -- for the drivers and
 * for EGL. Guest code reaches them through the functions in mg_ndk.c's table.
 */
#ifndef MG_NDK_H
#define MG_NDK_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- ANativeWindow */

typedef struct mg_nwindow_ops mg_nwindow_ops;

typedef struct mg_nwindow {
    volatile i32 refs;
    int width, height;             /* the surface, in pixels */
    int buffer_width, buffer_height;   /* what the app asked to render at (ANativeWindow_setBuffersGeometry); 0: the surface's size */
    int format;
    const mg_nwindow_ops *ops;     /* a window that is not a driver's (a SurfaceView's: drivers/mg_java_ndk.c); NULL for a driver's */
} mg_nwindow;

/*
 * A window of someone else's than the driver that drew the app's whole surface: the Java driver's windows of a SurfaceView's
 * Surface (drivers/mg_java_ndk.c). Its size follows its surface, it can go (surfaceDestroyed) while native code still holds
 * it, and EGL window surfaces and software buffers on it are its own; the NDK's functions and EGL (ndk/mg_egl.c) ask it.
 */
struct mg_nwindow_ops {
    /* Its surface's size now. */
    void (*size)(const mg_nwindow *w, int *width, int *height);
    /* Whether its surface is still there: false from surfaceDestroyed on, for good, as on Android. */
    bool (*valid)(const mg_nwindow *w);
    /* ANativeWindow_lock and ANativeWindow_unlockAndPost: buffer an ANativeWindow_Buffer, dirty an ARect or NULL; 0 or Android's negative error. */
    int (*lock)(mg_nwindow *w, void *buffer, i32 *dirty);
    int (*unlock)(mg_nwindow *w);
    /*
     * An EGL window surface on it (eglCreateWindowSurface), made with the first of `configs` (EGL configurations) that works:
     * its object, NULL when none can be made. Then for that object: the EGLSurface of EGL's own it is now and its size, which
     * the app's surface stands for in every EGL call (it is another after a swap that found the window resized); a swap on the
     * calling thread, the surface current there (false: the window is gone); and its end.
     */
    void *(*egl_create)(mg_nwindow *w, void *const *configs, int count);
    void *(*egl_handle)(void *surface, int *width, int *height);
    bool (*egl_swap)(void *surface);
    void (*egl_destroy)(void *surface);
    /* The last reference went. */
    void (*gone)(mg_nwindow *w);
};

mg_nwindow *mg_nwindow_create(int width, int height);
void mg_nwindow_acquire(mg_nwindow *w);
void mg_nwindow_release(mg_nwindow *w);
/* The size frames are rendered at: what ANativeWindow_setBuffersGeometry asked for, else the surface's now. */
void mg_nwindow_buffer_size(const mg_nwindow *w, int *width, int *height);
/* Whether the window can still be drawn on (a driver's always can). */
bool mg_nwindow_valid(const mg_nwindow *w);

/* ------------------------------------------------------------- AInputQueue */

typedef struct mg_input_queue mg_input_queue;

mg_input_queue *mg_input_queue_create(void);
/* A touch in surface pixels; phase: 0 down, 1 move, 2 up, 3 cancel. Builds the Android motion event, multi-touch included. */
void mg_input_queue_touch(mg_input_queue *q, int phase, int pointer_id, float x, float y);
void mg_input_queue_key(mg_input_queue *q, int keycode, bool down);

/* ----------------------------------------------------------------- ALooper */

/*
 * A thread that runs a Java Looper. On Android such a thread has a native
 * looper under its message queue, which is what ALooper_forThread gives
 * native code there, and descriptors native code adds to it have their
 * callbacks run between the thread's messages. Here the Java message loop says
 * of its thread that it is one (mg_looper_java_thread, once), and asks every
 * turn for what is ready to be run (mg_looper_poll_thread, which never
 * waits). The looper itself is made only when native code first asks for it,
 * so a message loop nobody's native code is interested in costs nothing;
 * mg_looper_poll_thread answers whether the thread has one.
 */
void mg_looper_java_thread(void);
bool mg_looper_poll_thread(void);
/* ALooper_addFd and ALooper_removeFd, for the NDK's other objects that watch a descriptor on an app's looper (mg_sensors.c's event queues). */
int mg_looper_add_fd(void *looper, int fd, int ident, int events, mg_addr callback, void *data);
int mg_looper_remove_fd(void *looper, int fd);

/* ----------------------------------------------------------- AAssetManager */

/* The app's asset manager: the "assets/" folders of the app's APKs. */
void *mg_asset_manager(void);
/*
 * An asset manager of other APKs than the app's, in the order they are looked in: what AAssetManager_fromJava gives for
 * the framework's AssetManager of another package (drivers/mg_java_ndk.c: Google Play services' inside an app that hosts
 * them). The same manager for the same APKs each time, and it lasts as long as the process does, as the APKs do.
 */
struct mg_zip;
void *mg_asset_manager_of(const struct mg_zip *const *apks, int count);

/* ----------------------------------------------------------- AConfiguration */

void mg_configuration_set(int width, int height, int density_dpi);

#ifdef __cplusplus
}
#endif

#endif
