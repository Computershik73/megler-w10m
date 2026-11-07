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

#ifdef __cplusplus
}
#endif

#endif
