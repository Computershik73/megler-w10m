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

/*
 * "Not running" first, then the shell is told (megler.h says so of megler_running). The other order would have the shell told
 * of an end while megler_running() still answered true: the core's own app test asks exactly that after on_exit, and
 * megler_storage_check would refuse a shell that went to it on being told. Whoever polls megler_running instead has to wait
 * for on_exit before reading what on_exit stores; that is the one moment this order leaves open.
 */
void hk_run_ended(int status, const char *reason)
{
    if (hk_atomic_xchg(&g_ended, 1)) return;
    hk_atomic_xchg(&g_active, 0);
    hk_info("run: the app has ended (status %d%s%s)", status, reason ? ", " : "", reason ? reason : "");
    if (g_config.on_exit) g_config.on_exit(g_config.user, status, reason);
}

/* ------------------------------------------------------ the battery (hk_run.h) */

/* A full, healthy lithium battery on mains power, at a full cell's voltage and at room temperature. */
static const hk_battery k_battery_default = { 100, 100, 5, 1, 2, 4200, 250, true, "Li-ion" };
static hk_battery g_battery;
static bool g_battery_set;
static volatile i32 g_battery_changes;
static hk_lock g_battery_lock = HK_LOCK_INIT;

void hk_battery_read(hk_battery *out)
{
    hk_lock_acquire(&g_battery_lock);
    *out = g_battery_set ? g_battery : k_battery_default;
    hk_lock_release(&g_battery_lock);
}

void hk_battery_set(const hk_battery *now)
{
    hk_battery was;
    const hk_battery *is;
    hk_lock_acquire(&g_battery_lock);
    was = g_battery_set ? g_battery : k_battery_default;
    g_battery_set = now != NULL;
    if (now) {
        g_battery = *now;
        /* A scale of nothing would be divided by; the text is the caller's and may not end. */
        if (g_battery.scale <= 0) g_battery.scale = 100;
        g_battery.technology[sizeof(g_battery.technology) - 1] = 0;
    }
    is = g_battery_set ? &g_battery : &k_battery_default;
    /* What an app is told of when it changes (hk_run.h). Being set to what it is already is no change, however often. */
    if (is->level != was.level || is->scale != was.scale || is->status != was.status || is->plugged != was.plugged) hk_atomic_add(&g_battery_changes, 1);
    hk_lock_release(&g_battery_lock);
}

u32 hk_battery_changes(void) { return (u32)g_battery_changes; }

static void on_guest_exit(int status) { hk_run_ended(status, NULL); }
static void on_crash(const char *report) { hk_run_ended(-1, report); }

/* ------------------------------------------------- the app's files
 *
 * The shell's storage host (megler.h) is the owner of the host layer's foreign
 * roots: link n is root n. The flags, kinds and errors of the two are the
 * same numbers, so a call passes through as it is. One thing is decided
 * here: a link the shell did not mark writable is not written to, whoever
 * asks -- the libc, or a part of the layer that goes to the host layer
 * itself.
 */
/* (Each of the two against the number itself: one enumeration compared with another draws a warning from MSVC, cast or not.) */
#define BOTH(a, b, n) ((a) == (n) && (b) == (n))
_Static_assert(BOTH(MEGLER_STORAGE_READ, HK_O_READ, 1) && BOTH(MEGLER_STORAGE_WRITE, HK_O_WRITE, 2) && BOTH(MEGLER_STORAGE_CREATE, HK_O_CREATE, 4) &&
               BOTH(MEGLER_STORAGE_TRUNCATE, HK_O_TRUNC, 8) && BOTH(MEGLER_STORAGE_APPEND, HK_O_APPEND, 16) && BOTH(MEGLER_STORAGE_EXCLUSIVE, HK_O_EXCL, 32),
               "megler.h's storage flags are the host layer's");
_Static_assert(BOTH(MEGLER_STORAGE_FILE, HK_FOREIGN_FILE, 0) && BOTH(MEGLER_STORAGE_FOLDER, HK_FOREIGN_DIR, 1) && BOTH(MEGLER_STORAGE_ANY, HK_FOREIGN_ANY, 2),
               "megler.h's storage kinds are the host layer's");
_Static_assert(BOTH(MEGLER_STORAGE_OK, HK_E_OK, 0) && BOTH(MEGLER_STORAGE_NOT_FOUND, HK_E_NOENT, 1) && BOTH(MEGLER_STORAGE_DENIED, HK_E_ACCESS, 2) &&
               BOTH(MEGLER_STORAGE_EXISTS, HK_E_EXIST, 3) && BOTH(MEGLER_STORAGE_NOT_A_FOLDER, HK_E_NOTDIR, 4) && BOTH(MEGLER_STORAGE_IS_A_FOLDER, HK_E_ISDIR, 5) &&
               BOTH(MEGLER_STORAGE_FULL, HK_E_NOSPC, 6) && BOTH(MEGLER_STORAGE_FAILED, HK_E_IO, 7) && BOTH(MEGLER_STORAGE_INVALID, HK_E_INVAL, 8) &&
               BOTH(MEGLER_STORAGE_NOT_EMPTY, HK_E_NOTEMPTY, 9) && BOTH(MEGLER_STORAGE_BUSY, HK_E_BUSY, 10) && BOTH(MEGLER_STORAGE_OTHER_DEVICE, HK_E_XDEV, 11),
               "megler.h's storage errors are the host layer's");
#undef BOTH

static megler_storage_host g_storage;
static megler_storage_link g_links[MEGLER_STORAGE_MAX_LINKS];
static char *g_link_names[MEGLER_STORAGE_MAX_LINKS];

/* A link the shell gave and this layer took (one with a name that could not be is not there, whatever its number). */
static bool link_there(int link) { return link >= 0 && link < g_storage.link_count && g_links[link].name; }
static bool link_writable(int link) { return link_there(link) && g_links[link].writable; }

/* What a change to a link is refused with before the shell is asked, or 0. */
static int link_refuses(int link, bool has_function)
{
    if (!link_there(link)) return HK_E_NOENT;
    return link_writable(link) && has_function ? 0 : HK_E_ACCESS;
}

static void *storage_open(void *user, int link, const char *path, int flags, int kind, int *err)
{
    bool changes = kind == HK_FOREIGN_FILE && (flags & (HK_O_WRITE | HK_O_CREATE | HK_O_TRUNC | HK_O_APPEND));
    (void)user;
    *err = changes ? link_refuses(link, true) : link_there(link) ? 0 : HK_E_NOENT;
    return *err ? NULL : g_storage.open(g_storage.user, link, path, flags, kind, err);
}

static bool storage_mkdir(void *user, int link, const char *path, int *err)
{
    (void)user;
    *err = link_refuses(link, g_storage.make_folder != NULL);
    return !*err && g_storage.make_folder(g_storage.user, link, path, err);
}

static bool storage_remove(void *user, int link, const char *path, bool dir, int *err)
{
    (void)user;
    *err = link_refuses(link, g_storage.remove != NULL);
    return !*err && g_storage.remove(g_storage.user, link, path, dir, err);
}

static bool storage_rename(void *user, int from_link, const char *from, int to_link, const char *to, int *err)
{
    (void)user;
    *err = link_refuses(from_link, g_storage.rename != NULL);
    if (!*err) *err = link_refuses(to_link, true);
    return !*err && g_storage.rename(g_storage.user, from_link, from, to_link, to, err);
}

/* Whether a link's name can be a folder of /sdcard: one part of a path, and not the folder the apps' own parts are in. */
static bool link_name_ok(const char *name)
{
    static const char android[] = "android";
    size_t n = name ? strlen(name) : 0, i;
    bool is_android = n == sizeof(android) - 1;
    if (!n || n > 200 || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32) return false;
        if (is_android && (c | 32) != android[i]) is_android = false;
    }
    return !is_android;
}

void hk_run_storage(const char *data_dir, const megler_storage_host *storage)
{
    static const hk_foreign foreign = { NULL, storage_open, storage_mkdir, storage_remove, storage_rename };
    hk_fs_link links[MEGLER_STORAGE_MAX_LINKS];
    char *shared = hk_sprintf("%s/sdcard", data_dir);
    int i, k, n = 0;
    hk_foreign_set(NULL);
    for (i = 0; i < MEGLER_STORAGE_MAX_LINKS; i++) {
        free(g_link_names[i]);
        g_link_names[i] = NULL;
    }
    memset(&g_storage, 0, sizeof(g_storage));
    memset(g_links, 0, sizeof(g_links));
    if (storage && storage->links && storage->link_count > 0 && storage->open) {
        g_storage = *storage;
        if (g_storage.link_count > MEGLER_STORAGE_MAX_LINKS) {
            hk_warn("storage: %d links were given and %d is as many as there can be: the rest are left out", g_storage.link_count, MEGLER_STORAGE_MAX_LINKS);
            g_storage.link_count = MEGLER_STORAGE_MAX_LINKS;
        }
        g_storage.links = g_links;
        for (i = 0; i < g_storage.link_count; i++) {
            const char *name = storage->links[i].name, *taken = NULL;
            /* (A link that is left out keeps its number: the numbers are the shell's.) */
            if (!link_name_ok(name)) {
                hk_warn("storage: link %d is left out: \"%s\" cannot be the name of a folder in the shared storage", i, name ? name : "");
                continue;
            }
            /* The shared storage does not tell A from a: nor may two links' names differ in that alone. */
            for (k = 0; k < n && !taken; k++) {
                const char *other = links[k].name;
                size_t a = strlen(name), b = 0;
                if (a != strlen(other)) continue;
                while (b < a && (name[b] == other[b] || ((name[b] ^ other[b]) == 32 && (name[b] | 32) >= 'a' && (name[b] | 32) <= 'z'))) b++;
                if (b == a) taken = other;
            }
            if (taken) {
                hk_warn("storage: link %d (\"%s\") is left out: link \"%s\" has that name already", i, name, taken);
                continue;
            }
            g_link_names[i] = hk_strdup(name);
            g_links[i].name = g_link_names[i];
            g_links[i].writable = storage->links[i].writable;
            links[n].name = g_link_names[i];
            links[n].root = i;
            links[n].read_only = !g_links[i].writable;
            n++;
        }
        if (n) hk_foreign_set(&foreign);
    }
    hk_fs_shared_storage(shared, links, n);
    for (i = 0; i < n; i++) hk_info("storage: /sdcard/%s is a folder of the phone's own (link %d%s)", links[i].name, links[i].root, links[i].read_only ? ", to read only" : "");
    free(shared);
}

/* What is in a folder besides one name: counted, the first few named. */
typedef struct left_behind {
    int count;
    char names[160];
} left_behind;

static void left_one(const char *name, bool is_dir, void *user)
{
    left_behind *l = (left_behind *)user;
    size_t at = strlen(l->names);
    (void)is_dir;
    if (!strcmp(name, "Android")) return;
    if (l->count++ < 6 && at + strlen(name) + 3 < sizeof(l->names)) snprintf(l->names + at, sizeof(l->names) - at, "%s%s", at ? ", " : "", name);
}

void hk_run_files(const char *data_dir, const char *package, const megler_storage_host *storage)
{
    char *internal = hk_sprintf("%s/apps/%s/data", data_dir, package), *own = hk_sprintf("%s/apps/%s/sdcard", data_dir, package);
    left_behind left;
    hk_fs_configure(package, internal, own);
    hk_run_storage(data_dir, storage);
    /*
     * Until the storage was shared every app had a whole one to itself, in the folder its own part is still kept in. What an app put
     * there outside Android/data and Android/obb is still on the disk and no longer in /sdcard. Nothing is moved; it is said.
     */
    memset(&left, 0, sizeof(left));
    if (hk_path_list(own, left_one, &left, NULL) && left.count)
        hk_warn("storage: %d thing(s) this app kept in a storage of its own are not in the shared one, where it now looks: %s%s (still in %s)", left.count, left.names,
                left.count > 6 ? ", ..." : "", own);
    free(internal);
    free(own);
}

bool hk_run_start(const megler_run_config *config)
{
    hk_apk_info info;
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

    /* Each app gets its own Android data directories under ours, and all of them one shared storage. */
    hk_run_files(megler_data_dir(), info.package, &g_config.storage);

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
