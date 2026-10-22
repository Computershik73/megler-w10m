/* megler.h over the core. */
#include "../include/megler.h"

#include <stdio.h>

#include "bionic/mg_bionic.h"
#include "mg_core.h"
#include "host/mg_host.h"
#include "drivers/mg_java.h"
#include "run/mg_run.h"
#include "webp/mg_webp.h"

static megler_log_fn g_log;
static void *g_log_user;
static char g_data_dir[600], g_package_dir[600], g_storage_dir[600];

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
bool mg_flag(const char *name)
{
    char path[700];
    mg_stat st;
    bool on;
    if (!g_data_dir[0]) return false;
    snprintf(path, sizeof(path), "%s/flags", g_data_dir);
    mg_path_mkdirs(path);
    snprintf(path, sizeof(path), "%s/flags/%s", g_data_dir, name);
    on = mg_path_stat(path, &st, NULL);
    if (on) mg_warn("flag: %s is on (a file of that name is in the data folder's flags)", name);
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
        mg_warn("flag: %s is on (a file of that name is in the data folder's flags)", name);
        e->fn(name + n, e->user);
    }
}

/* Flags whose names carry what they are about (trace-calls-<class name prefix>): every one that starts with `prefix`. */
void mg_flags_each(const char *prefix, void (*fn)(const char *rest, void *user), void *user)
{
    char path[700];
    struct flags_each e;
    int err = 0;
    if (!g_data_dir[0]) return;
    e.prefix = prefix;
    e.fn = fn;
    e.user = user;
    snprintf(path, sizeof(path), "%s/flags", g_data_dir);
    mg_path_list(path, flags_each_one, &e, &err);
}

const char *megler_package_dir(void) { return g_package_dir; }
const char *megler_storage_dir(void) { return g_storage_dir[0] ? g_storage_dir : g_data_dir; }

static void copy_dir(char *out, size_t cap, const char *in)
{
    size_t n;
    char *p;
    mg_strlcpy(out, in ? in : ".", cap);
    for (p = out; *p; p++)
        if (*p == '\\') *p = '/';
    n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
}

bool megler_init(const megler_config *config)
{
    copy_dir(g_data_dir, sizeof(g_data_dir), config->data_dir);
    copy_dir(g_package_dir, sizeof(g_package_dir), config->package_dir);
    if (config->storage_dir && config->storage_dir[0]) {
        copy_dir(g_storage_dir, sizeof(g_storage_dir), config->storage_dir);
        mg_path_mkdirs(g_storage_dir);
    }
    g_log = config->log;
    g_log_user = config->log_user;
    if (g_log) mg_log_set_sink(log_sink, NULL);
    mg_mem_set_probe(config->memory_limit, config->memory_usage);
    mg_path_mkdirs(g_data_dir);
    return mg_core_init();
}

bool megler_webp_size(const void *data, size_t size, int *width, int *height) { return mg_webp_size(data, size, width, height); }
bool megler_webp_decode(const void *data, size_t size, void *pixels, int width, int height, int stride)
{
    return mg_webp_decode(data, size, pixels, width, height, stride);
}

bool megler_file_by_handle(const char *path, megler_handle_opener open, void *user) { return mg_file_by_handle(path, open, user); }

const char *megler_version(void) { return "0.1"; }
void megler_free(void *p) { free(p); }

bool megler_run(const megler_run_config *config) { return mg_run_start(config); }
bool megler_running(void) { return mg_run_active(); }
const char *megler_last_error(void) { return mg_run_last_error(); }
void megler_touch(int phase, int pointer_id, float x, float y) { mg_run_touch(phase, pointer_id, x, y); }
void megler_key(int android_keycode, bool down) { mg_run_key(android_keycode, down); }
/* Text entry is the Java driver's alone: its framework has the text fields. For any other app these do nothing. */
void megler_text_commit(const char *utf8) { mg_java_text(utf8); }
void megler_text_key(int key) { mg_java_text_key(key); }
void megler_keyboard_closed(void) { mg_java_keyboard_closed(); }
void megler_keyboard_height(int pixels) { mg_java_keyboard_height(pixels); }
void megler_set_insets(int left, int top, int right, int bottom) { mg_java_set_insets(left, top, right, bottom); }
void megler_set_paused(bool paused) { mg_run_set_paused(paused); }
void megler_set_focused(bool focused) { mg_run_set_focused(focused); }
void megler_stop(void) { mg_run_stop(); }
void megler_start_activity(const char *activity_class) { mg_java_start_activity(activity_class); }

bool megler_ask_clear_data(const char *package)
{
    char *dir, *path;
    mg_file *f;
    int err = 0;
    if (!package || !package[0] || strchr(package, '/') || strchr(package, '\\') || !g_data_dir[0]) return false;
    dir = mg_sprintf("%s/clear-data", megler_storage_dir());
    path = mg_sprintf("%s/%s", dir, package);
    mg_path_mkdirs(dir);
    f = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    if (f) mg_file_close(f);
    if (f) mg_info("run: %s's data is to be cleared when it next starts", package);
    else mg_warn("run: the ask to clear %s's data could not be written (%s, error %d)", package, path, err);
    free(dir);
    free(path);
    return f != NULL;
}
uint32_t megler_frames(void) { return mg_run_frames(); }

/* The phone, for an app. The time zone is kept where Android's own code looks for it (bionic/mg_bionic.h). */
void megler_set_timezone(const char *id)
{
    mg_timezone_set(id);
    if (id && id[0]) mg_info("phone: the time zone is %s", mg_timezone_id());
}

void megler_set_locale(const char *tag)
{
    mg_locale_set(tag);
    mg_info("phone: the language and region are %s", mg_locale_tag());
}

void megler_set_theme(uint32_t accent_argb, bool night)
{
    mg_java_set_theme(accent_argb, night);
    mg_info("phone: the accent colour is #%06X, the theme %s", (unsigned)(accent_argb & 0xFFFFFF), night ? "dark" : "light");
}

/*
 * The battery is the core's mg_battery (run/mg_run.h), which has all that Android's intent says; the shell's has what a
 * phone with Windows can say, and the rest is made up here, plausibly: a lithium cell's voltage runs from about 3.4 V
 * empty to 4.2 V full, and an app that shows it wants a number in that range more than it wants the truth of the curve.
 */
void megler_set_battery(const megler_battery *now)
{
    mg_battery b;
    if (!now) {
        mg_battery_set(NULL);
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
    mg_strlcpy(b.technology, "Li-ion", sizeof(b.technology));
    mg_battery_set(&b);
}
