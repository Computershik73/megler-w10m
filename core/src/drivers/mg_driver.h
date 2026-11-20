/*
 * Drivers: each plays the part of one kind of app's Java side.
 *
 * An Android game is native code plus a thin Java shell that loads it, hands
 * it a window and input, and answers its questions about the device. No Java
 * runs here, so for each engine there is a driver that does what that shell
 * does, from C, against the JNI world in jni/.
 */
#ifndef MG_DRIVER_H
#define MG_DRIVER_H

#include "../../include/megler.h"
#include "../apk/mg_apk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mg_driver {
    const char *name;      /* the engine name mg_apk_read_info reports */
    bool (*start)(const megler_run_config *config, const mg_apk_info *info);
    void (*touch)(int phase, int pointer_id, float x, float y);
    void (*key)(int android_keycode, bool down);
    void (*set_paused)(bool paused);
    void (*set_focused)(bool focused);
    void (*stop)(void);
    /* megler_back: Back, and what the app did with it (MEGLER_BACK_*); NULL where only megler_key has it. */
    int (*back)(unsigned quiet_ms, unsigned max_ms);
    /*
     * megler_set_surface: the surface is another size or turned (mg_run_surface has it now). Any thread; returns at once. NULL
     * where the driver looks for it itself (the Java driver's main loop, at its next poll).
     */
    void (*resize)(void);
} mg_driver;

const mg_driver *mg_driver_find(const char *engine);
/* Whether an engine has a driver, for the APK report. */
bool mg_driver_supported(const char *engine);

extern const mg_driver mg_driver_native_activity;
extern const mg_driver mg_driver_unity;
extern const mg_driver mg_driver_java;

/*
 * android/native_activity.h: ANativeActivityCallbacks and ANativeActivity, as the app's library fills them in and reads them.
 * The callbacks are guest code, called through MG_GFN. Two activities have them: the NativeActivity driver's, whose clazz is
 * a stand-in (mg_native_activity.c), and the framework's android.app.NativeActivity under the Java driver, whose clazz is the
 * app's own activity (mg_java_ndk.c).
 */
typedef struct mg_na_callbacks {
    mg_addr onStart, onResume, onSaveInstanceState, onPause, onStop, onDestroy, onWindowFocusChanged, onNativeWindowCreated,
        onNativeWindowResized, onNativeWindowRedrawNeeded, onNativeWindowDestroyed, onInputQueueCreated, onInputQueueDestroyed,
        onContentRectChanged, onConfigurationChanged, onLowMemory;
} mg_na_callbacks;

typedef struct mg_na_activity {
    mg_na_callbacks *callbacks;
    void *vm, *env, *clazz;
    const char *internalDataPath, *externalDataPath;
    i32 sdkVersion;
    void *instance, *assetManager;
    const char *obbPath;
} mg_na_activity;

/*
 * What native code asks of a NativeActivity of the Java driver's (ANativeActivity_finish and the rest, mg_native_activity.c's
 * table): done by the framework's activity on its main thread (mg_java_ndk.c). False when `a` is none of the Java driver's.
 */
enum { MG_NA_FINISH = 1, MG_NA_WINDOW_FLAGS, MG_NA_WINDOW_FORMAT, MG_NA_SHOW_IME, MG_NA_HIDE_IME };
bool mg_java_native_activity_ask(mg_na_activity *a, int what, i32 arg1, i32 arg2);

#ifdef __cplusplus
}
#endif

#endif
