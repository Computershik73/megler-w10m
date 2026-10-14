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
