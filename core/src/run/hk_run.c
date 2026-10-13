/*
 * Running one app.
 *
 * Starting an app is: open its APKs, read what its manifest says it is, pick
 * the driver that plays the part of its Java side (drivers/), and let that
 * load the native libraries and run them on threads of their own. From then
 * on this file only passes things along: input in, the end of the app out.
 */
#include "hk_run.h"

#include <stdio.h>

#include "../apk/hk_apk.h"
#include "../bionic/hk_bionic.h"
#include "../drivers/hk_driver.h"
#include "../hk_core.h"
#include "../host/hk_host.h"
#include "../ld/hk_ld.h"

static megler_run_config g_config;
static char *g_apk_path;
static char g_error[400];
static volatile i32 g_active, g_ended, g_frames;
static const hk_driver *g_driver;
static bool g_started_once;

void hk_run_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
    hk_err("run: %s", g_error);
}

const char *hk_run_last_error(void) { return g_error; }
const megler_run_config *hk_run_config(void) { return &g_config; }
bool hk_run_active(void) { return g_active != 0; }
u32 hk_run_frames(void) { return (u32)g_frames; }
void hk_run_count_frame(void) { hk_atomic_add(&g_frames, 1); }

void hk_run_ended(int status, const char *reason)
{
    if (hk_atomic_xchg(&g_ended, 1)) return;
    hk_atomic_xchg(&g_active, 0);
    hk_info("run: the app has ended (status %d%s%s)", status, reason ? ", " : "", reason ? reason : "");
    if (g_config.on_exit) g_config.on_exit(g_config.user, status, reason);
}

static void on_guest_exit(int status) { hk_run_ended(status, NULL); }
static void on_crash(const char *report) { hk_run_ended(-1, report); }

bool hk_run_start(const megler_run_config *config)
{
    hk_apk_info info;
    char *internal, *external;
    int i;
    g_error[0] = 0;
    if (g_started_once) {
        hk_run_set_error("an app has already run in this process; close Megler and open it again to start another");
        return false;
    }
    if (!hk_core_can_run()) {
        hk_run_set_error("Android code cannot run: executable memory was refused (is the codeGeneration capability granted?)");
        return false;
    }
    if (!config || !config->apk_path) {
        hk_run_set_error("no APK given");
        return false;
    }
    if (!hk_apk_read_info(config->apk_path, &info)) {
        hk_run_set_error("%s", info.error[0] ? info.error : "the APK cannot be read");
        return false;
    }
    if (!info.runnable) {
        hk_run_set_error("%s", info.reason);
        hk_apk_info_free(&info);
        return false;
    }
    g_driver = hk_driver_find(info.engine);
    if (!g_driver) {
        hk_run_set_error("no driver yet for this kind of app (%s)", info.engine);
        hk_apk_info_free(&info);
        return false;
    }
    g_started_once = true;
    g_config = *config;
    g_apk_path = hk_strdup(config->apk_path);
    g_config.apk_path = g_apk_path;

    if (!hk_ld_add_apk(config->apk_path)) {
        hk_run_set_error("the APK cannot be opened");
        hk_apk_info_free(&info);
        return false;
    }
    for (i = 0; i < config->split_count; i++) hk_ld_add_apk(config->split_apks[i]);

    /* Each app gets its own Android data directories under ours. */
    internal = hk_sprintf("%s/apps/%s/data", megler_data_dir(), info.package);
    external = hk_sprintf("%s/apps/%s/sdcard", megler_data_dir(), info.package);
    hk_fs_configure(info.package, internal, external);
    free(internal);
    free(external);

    hk_guest_exit_hook = on_guest_exit;
    hk_core_set_crash_hook(on_crash);
    hk_atomic_xchg(&g_active, 1);
    hk_info("run: %s (%s) with the %s driver, %dx%d at %d dpi", info.package, info.label[0] ? info.label : "no label", g_driver->name,
            config->width, config->height, config->density_dpi);
    if (!g_driver->start(&g_config, &info)) {
        hk_atomic_xchg(&g_active, 0);
        if (!g_error[0]) hk_run_set_error("the app did not start");
        hk_apk_info_free(&info);
        return false;
    }
    hk_apk_info_free(&info);
    return true;
}

void hk_run_touch(int phase, int pointer_id, float x, float y)
{
    if (g_active && g_driver && g_driver->touch) g_driver->touch(phase, pointer_id, x, y);
}

void hk_run_key(int android_keycode, bool down)
{
    if (g_active && g_driver && g_driver->key) g_driver->key(android_keycode, down);
}

void hk_run_set_paused(bool paused)
{
    if (g_active && g_driver && g_driver->set_paused) g_driver->set_paused(paused);
}

void hk_run_set_focused(bool focused)
{
    if (g_active && g_driver && g_driver->set_focused) g_driver->set_focused(focused);
}

void hk_run_stop(void)
{
    if (!g_active) return;
    if (g_driver && g_driver->stop) g_driver->stop();
    else hk_run_ended(0, "stopped");
}
