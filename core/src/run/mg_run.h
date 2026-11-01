/* Running one app: what megler.h's running functions are made of (run/mg_run.c picks the driver). */
#ifndef MG_RUN_H
#define MG_RUN_H

#include "../../include/megler.h"
#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mg_run_start(const megler_run_config *config);
bool mg_run_active(void);
const char *mg_run_last_error(void);
void mg_run_set_error(const char *fmt, ...) MG_PRINTF(1, 2);
void mg_run_touch(int phase, int pointer_id, float x, float y);
void mg_run_key(int android_keycode, bool down);
void mg_run_set_paused(bool paused);
void mg_run_set_focused(bool focused);
void mg_run_stop(void);
u32 mg_run_frames(void);

/*
 * Where an app's files are, set before its code runs: its own folders,
 * <data_dir>/apps/<package>/data, <storage_dir>/apps/<package>/sdcard (which
 * holds its own part of the shared storage), and the shared storage, in
 * storage_dir (megler_config.storage_dir; the same as data_dir unless the shell
 * says otherwise). mg_run_start calls this with megler_data_dir() and
 * megler_storage_dir().
 */
void mg_run_files(const char *data_dir, const char *storage_dir, const char *package, const megler_storage_host *storage);
/*
 * What mg_run_files does first: when megler_ask_clear_data asked for it (<storage_dir>/clear-data/<package>), the app's
 * private folder and its own Android/data folder go, and the ask; true then.
 */
bool mg_run_clear_data_if_asked(const char *data_dir, const char *storage_dir, const char *package);
/*
 * The host folder that holds an app's own part of the shared storage (its Android/data/<package> and Android/obb/<package>
 * are under it, as under /storage/emulated/0): <data_dir>/apps/<package>/sdcard, malloc'd. Where mg_run_files mounts it,
 * and where megler_bundle_unpack puts a bundle's expansion files before the app has ever run.
 */
char *mg_run_own_storage(const char *data_dir, const char *package);
/*
 * The shared storage alone: <data_dir>/sdcard as /storage/emulated/0, and the
 * shell's links in it (megler.h, megler_storage_host; NULL or all zero: none).
 * The links and their names are copied. Not while an app runs.
 */
void mg_run_storage(const char *data_dir, const megler_storage_host *storage);

/*
 * The phone's battery, in the words of Android's ACTION_BATTERY_CHANGED. mg_battery_read is the one place the layer's
 * Android side asks (today the Java driver, for the sticky battery intent and BatteryManager); nothing else in the core
 * holds a level or a voltage. Until mg_battery_set says otherwise the answer is a full, healthy battery on mains power:
 * the shell knows the real one (Windows.Devices.Power) and sets it whenever it changes, from any thread (megler.h's
 * megler_set_battery, which fills in what a phone with Windows does not say). An app sees a new state when it next asks,
 * and is told of one: mg_battery_changes counts the times the charge, the status or what it is plugged into became
 * another, and a driver that watches the count sends Android's broadcasts (the Java driver does, on the app's main thread).
 */
typedef struct mg_battery {
    int level, scale;    /* the charge, level out of scale; Android's own scale is 100 */
    int status;          /* BatteryManager.BATTERY_STATUS_*: 1 unknown, 2 charging, 3 discharging, 4 not charging, 5 full */
    int plugged;         /* 0 on its battery, else BATTERY_PLUGGED_*: 1 AC, 2 USB, 4 wireless */
    int health;          /* BATTERY_HEALTH_*: 1 unknown, 2 good, 3 overheat, 4 dead, 5 over voltage, 6 failure, 7 cold */
    int voltage_mv;      /* millivolts */
    int temperature;     /* tenths of a degree Celsius */
    bool present;        /* there is a battery */
    char technology[16]; /* its kind as Android names it: "Li-ion" */
} mg_battery;
void mg_battery_read(mg_battery *out);
/* NULL: back to the answer before any call. */
void mg_battery_set(const mg_battery *now);
u32 mg_battery_changes(void);

/* The phone's cellular side, as megler_set_cellular says it (megler.h). Until it is said: no SIM. */
typedef struct mg_cellular {
    bool sim_ready;
    char operator_name[64];
    char mcc_mnc[8];
    int network_type;
    bool roaming;
} mg_cellular;
void mg_cellular_read(mg_cellular *out);
void mg_cellular_set(const mg_cellular *now);

/* The running app's configuration and window host, for the layers below (EGL presents through the host). */
const megler_run_config *mg_run_config(void);
void mg_run_count_frame(void);
/* The app has ended; tells the shell once. */
void mg_run_ended(int status, const char *reason);

const char *megler_data_dir(void);
const char *megler_package_dir(void);
/* megler_config.storage_dir, or the data folder when the shell gave none. */
const char *megler_storage_dir(void);

#ifdef __cplusplus
}
#endif

#endif
