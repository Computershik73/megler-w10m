/* megler.h over the core. */
#include "../include/megler.h"

#include <stdio.h>

#include "bionic/hk_bionic.h"
#include "hk_core.h"
#include "host/hk_host.h"
#include "drivers/hk_java.h"
#include "run/hk_run.h"

static megler_log_fn g_log;
static void *g_log_user;
static char g_data_dir[600], g_package_dir[600];

static void log_sink(int level, const char *line, void *user)
{
    (void)user;
    if (g_log) g_log(level, line, g_log_user);
}

const char *megler_data_dir(void) { return g_data_dir; }

/*
 * A switch for trying something on a device without building for it: on when
 * a file of that name is in <data_dir>/flags. (It only has to be there: a
 * file put there through the Device Portal cannot be opened by the app, but
 * it can be seen.) The folder is made at start-up so that there is somewhere
 * to put one.
 */
bool hk_flag(const char *name)
{
    char path[700];
    hk_stat st;
    bool on;
    if (!g_data_dir[0]) return false;
    snprintf(path, sizeof(path), "%s/flags", g_data_dir);
    hk_path_mkdirs(path);
    snprintf(path, sizeof(path), "%s/flags/%s", g_data_dir, name);
    on = hk_path_stat(path, &st, NULL);
    if (on) hk_warn("flag: %s is on (a file of that name is in the data folder's flags)", name);
    return on;
}

struct flags_each {
    const char *prefix;
    void (*fn)(const char *rest, void *user);
    void *user;
};

static void flags_each_one(const char *name, bool is_dir, void *user)
{
    struct flags_each *e = (struct flags_each *)user;
    size_t n = strlen(e->prefix);
    if (!is_dir && !strncmp(name, e->prefix, n) && name[n]) {
        hk_warn("flag: %s is on (a file of that name is in the data folder's flags)", name);
        e->fn(name + n, e->user);
    }
}

/* Flags whose names carry what they are about (trace-calls-<class name prefix>): every one that starts with `prefix`. */
void hk_flags_each(const char *prefix, void (*fn)(const char *rest, void *user), void *user)
{
    char path[700];
    struct flags_each e;
    int err = 0;
    if (!g_data_dir[0]) return;
    e.prefix = prefix;
    e.fn = fn;
    e.user = user;
    snprintf(path, sizeof(path), "%s/flags", g_data_dir);
    hk_path_list(path, flags_each_one, &e, &err);
}

const char *megler_package_dir(void) { return g_package_dir; }

static void copy_dir(char *out, size_t cap, const char *in)
{
    size_t n;
    char *p;
    hk_strlcpy(out, in ? in : ".", cap);
    for (p = out; *p; p++)
        if (*p == '\\') *p = '/';
    n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}

bool megler_init(const megler_config *config)
{
    copy_dir(g_data_dir, sizeof(g_data_dir), config->data_dir);
    copy_dir(g_package_dir, sizeof(g_package_dir), config->package_dir);
    g_log = config->log;
    g_log_user = config->log_user;
    if (g_log) hk_log_set_sink(log_sink, NULL);
    hk_mem_set_probe(config->memory_limit, config->memory_usage);
    hk_path_mkdirs(g_data_dir);
    return hk_core_init();
}

const char *megler_version(void) { return "0.1"; }
void megler_free(void *p) { free(p); }

bool megler_run(const megler_run_config *config) { return hk_run_start(config); }
bool megler_running(void) { return hk_run_active(); }
const char *megler_last_error(void) { return hk_run_last_error(); }
void megler_touch(int phase, int pointer_id, float x, float y) { hk_run_touch(phase, pointer_id, x, y); }
void megler_key(int android_keycode, bool down) { hk_run_key(android_keycode, down); }
/* Text entry is the Java driver's alone: its framework has the text fields. For any other app these do nothing. */
void megler_text_commit(const char *utf8) { hk_java_text(utf8); }
void megler_text_key(int key) { hk_java_text_key(key); }
void megler_keyboard_closed(void) { hk_java_keyboard_closed(); }
void megler_keyboard_height(int pixels) { hk_java_keyboard_height(pixels); }
void megler_set_paused(bool paused) { hk_run_set_paused(paused); }
void megler_set_focused(bool focused) { hk_run_set_focused(focused); }
void megler_stop(void) { hk_run_stop(); }
uint32_t megler_frames(void) { return hk_run_frames(); }

/* The phone, for an app. The time zone is kept where Android's own code looks for it (bionic/hk_bionic.h). */
void megler_set_timezone(const char *id)
{
    hk_timezone_set(id);
    if (id && id[0]) hk_info("phone: the time zone is %s", hk_timezone_id());
}

void megler_set_locale(const char *tag)
{
    hk_locale_set(tag);
    hk_info("phone: the language and region are %s", hk_locale_tag());
}

/*
 * The battery is the core's hk_battery (run/hk_run.h), which has all that Android's intent says; the shell's has what a
 * phone with Windows can say, and the rest is made up here, plausibly: a lithium cell's voltage runs from about 3.4 V
 * empty to 4.2 V full, and an app that shows it wants a number in that range more than it wants the truth of the curve.
 */
void megler_set_battery(const megler_battery *now)
{
    hk_battery b;
    if (!now) {
        hk_battery_set(NULL);
        return;
    }
    memset(&b, 0, sizeof(b));
    b.level = now->level < 0 ? 0 : now->level > 100 ? 100 : now->level;
    b.scale = 100;
    b.status = now->status >= 1 && now->status <= 5 ? now->status : 1;
    b.plugged = now->plugged > 0 ? now->plugged : 0;
    b.present = now->present;
    b.health = now->present ? 2 : 1;
    b.voltage_mv = now->voltage_mv > 0 ? now->voltage_mv : 3400 + 8 * b.level;
    b.temperature = now->temperature == INT_MIN ? 250 : now->temperature;
    hk_strlcpy(b.technology, "Li-ion", sizeof(b.technology));
    hk_battery_set(&b);
}
