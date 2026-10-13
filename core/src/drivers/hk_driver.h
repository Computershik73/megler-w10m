/*
 * Drivers: each plays the part of one kind of app's Java side.
 *
 * An Android game is native code plus a thin Java shell that loads it, hands
 * it a window and input, and answers its questions about the device. No Java
 * runs here, so for each engine there is a driver that does what that shell
 * does, from C, against the JNI world in jni/.
 */
#ifndef HK_DRIVER_H
#define HK_DRIVER_H

#include "../../include/megler.h"
#include "../apk/hk_apk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_driver {
    const char *name;      /* the engine name hk_apk_read_info reports */
    bool (*start)(const megler_run_config *config, const hk_apk_info *info);
    void (*touch)(int phase, int pointer_id, float x, float y);
    void (*key)(int android_keycode, bool down);
    void (*set_paused)(bool paused);
    void (*set_focused)(bool focused);
    void (*stop)(void);
} hk_driver;

const hk_driver *hk_driver_find(const char *engine);
/* Whether an engine has a driver, for the APK report. */
bool hk_driver_supported(const char *engine);

extern const hk_driver hk_driver_native_activity;
extern const hk_driver hk_driver_unity;
extern const hk_driver hk_driver_java;

#ifdef __cplusplus
}
#endif

#endif
