/* Running one app: what megler.h's running functions are made of (run/hk_run.c picks the driver). */
#ifndef HK_RUN_H
#define HK_RUN_H

#include "../../include/megler.h"
#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

bool hk_run_start(const megler_run_config *config);
bool hk_run_active(void);
const char *hk_run_last_error(void);
void hk_run_set_error(const char *fmt, ...) HK_PRINTF(1, 2);
void hk_run_touch(int phase, int pointer_id, float x, float y);
void hk_run_key(int android_keycode, bool down);
void hk_run_set_paused(bool paused);
void hk_run_set_focused(bool focused);
void hk_run_stop(void);
u32 hk_run_frames(void);

/*
 * Where an app's files are, set before its code runs: its own folders,
 * <data_dir>/apps/<package>/data and .../sdcard (which holds its own part of
 * the shared storage), and the shared storage. hk_run_start calls this with
 * megler_data_dir().
 */
void hk_run_files(const char *data_dir, const char *package, const megler_storage_host *storage);
/*
 * The shared storage alone: <data_dir>/sdcard as /storage/emulated/0, and the
 * shell's links in it (megler.h, megler_storage_host; NULL or all zero: none).
 * The links and their names are copied. Not while an app runs.
 */
void hk_run_storage(const char *data_dir, const megler_storage_host *storage);

/*
 * The phone's battery, in the words of Android's ACTION_BATTERY_CHANGED. hk_battery_read is the one place the layer's
 * Android side asks (today the Java driver, for the sticky battery intent and BatteryManager); nothing else in the core
 * holds a level or a voltage. Until hk_battery_set says otherwise the answer is a full, healthy battery on mains power:
 * the shell knows the real one (Windows.Devices.Power) and sets it whenever it changes, from any thread (megler.h's
 * megler_set_battery, which fills in what a phone with Windows does not say). An app sees a new state when it next asks,
 * and is told of one: hk_battery_changes counts the times the charge, the status or what it is plugged into became
 * another, and a driver that watches the count sends Android's broadcasts (the Java driver does, on the app's main thread).
 */
typedef struct hk_battery {
    int level, scale;    /* the charge, level out of scale; Android's own scale is 100 */
    int status;          /* BatteryManager.BATTERY_STATUS_*: 1 unknown, 2 charging, 3 discharging, 4 not charging, 5 full */
    int plugged;         /* 0 on its battery, else BATTERY_PLUGGED_*: 1 AC, 2 USB, 4 wireless */
    int health;          /* BATTERY_HEALTH_*: 1 unknown, 2 good, 3 overheat, 4 dead, 5 over voltage, 6 failure, 7 cold */
    int voltage_mv;      /* millivolts */
    int temperature;     /* tenths of a degree Celsius */
    bool present;        /* there is a battery */
    char technology[16]; /* its kind as Android names it: "Li-ion" */
} hk_battery;
void hk_battery_read(hk_battery *out);
/* NULL: back to the answer before any call. */
void hk_battery_set(const hk_battery *now);
u32 hk_battery_changes(void);

/* The running app's configuration and window host, for the layers below (EGL presents through the host). */
const megler_run_config *hk_run_config(void);
void hk_run_count_frame(void);
/* The app has ended; tells the shell once. */
void hk_run_ended(int status, const char *reason);

const char *megler_data_dir(void);
const char *megler_package_dir(void);

#ifdef __cplusplus
}
#endif

#endif
