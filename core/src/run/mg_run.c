/*
 * Running one app.
 *
 * Starting an app is: open its APKs, read what its manifest says it is, pick
 * the driver that plays the part of its Java side (drivers/), and let that
 * load the native libraries and run them on threads of their own. From then
 * on this file only passes things along: input in, the end of the app out.
 */
#include "mg_run.h"

#include <stdio.h>

#include "../apk/mg_apk.h"
#include "../bionic/mg_bionic.h"
#include "../bionic/mg_fd.h"
#include "../drivers/mg_driver.h"
#include "../mg_core.h"
#include "../host/mg_host.h"
#include "../ld/mg_ld.h"

static megler_run_config g_config;
static char *g_apk_path;
/* The app's splits as one list, what megler_run_config gave in a folder and as paths (g_config.split_apks points here). */
static char **g_splits;
static int g_nsplits;
static char g_error[400];
static volatile i32 g_active, g_ended, g_frames;
static const mg_driver *g_driver;
static bool g_started_once;

void mg_run_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, ap);
    va_end(ap);
    mg_err("run: %s", g_error);
}

const char *mg_run_last_error(void) { return g_error; }
const megler_run_config *mg_run_config(void) { return &g_config; }
bool mg_run_active(void) { return g_active != 0; }
u32 mg_run_frames(void) { return (u32)g_frames; }
void mg_run_count_frame(void) { mg_atomic_add(&g_frames, 1); }

/*
 * "Not running" first, then the shell is told (megler.h says so of megler_running). The other order would have the shell told
 * of an end while megler_running() still answered true: the core's own app test asks exactly that after on_exit, and
 * megler_storage_check would refuse a shell that went to it on being told. Whoever polls megler_running instead has to wait
 * for on_exit before reading what on_exit stores; that is the one moment this order leaves open.
 */
void mg_run_ended(int status, const char *reason)
{
    if (mg_atomic_xchg(&g_ended, 1)) return;
    /* What the app wrote into shared file mappings reaches the files (bionic/mg_libc_mem.c). */
    mg_mem_write_back();
    mg_atomic_xchg(&g_active, 0);
    mg_info("run: the app has ended (status %d%s%s)", status, reason ? ", " : "", reason ? reason : "");
    if (g_config.on_exit) g_config.on_exit(g_config.user, status, reason);
}

/* ------------------------------------------------------ the battery (mg_run.h) */

/* A full, healthy lithium battery on mains power, at a full cell's voltage and at room temperature. */
static const mg_battery k_battery_default = { 100, 100, 5, 1, 2, 4200, 250, true, "Li-ion" };
static mg_battery g_battery;
static bool g_battery_set;
static volatile i32 g_battery_changes;
static mg_lock g_battery_lock = MG_LOCK_INIT;

void mg_battery_read(mg_battery *out)
{
    mg_lock_acquire(&g_battery_lock);
    *out = g_battery_set ? g_battery : k_battery_default;
    mg_lock_release(&g_battery_lock);
}

void mg_battery_set(const mg_battery *now)
{
    mg_battery was;
    const mg_battery *is;
    mg_lock_acquire(&g_battery_lock);
    was = g_battery_set ? g_battery : k_battery_default;
    g_battery_set = now != NULL;
    if (now) {
        g_battery = *now;
        /* A scale of nothing would be divided by; the text is the caller's and may not end. */
        if (g_battery.scale <= 0) g_battery.scale = 100;
        g_battery.technology[sizeof(g_battery.technology) - 1] = 0;
    }
    is = g_battery_set ? &g_battery : &k_battery_default;
    /* What an app is told of when it changes (mg_run.h). Being set to what it is already is no change, however often. */
    if (is->level != was.level || is->scale != was.scale || is->status != was.status || is->plugged != was.plugged) mg_atomic_add(&g_battery_changes, 1);
    mg_lock_release(&g_battery_lock);
}

u32 mg_battery_changes(void) { return (u32)g_battery_changes; }

/* ----------------------------------------------------- the SIM (mg_run.h) */

static mg_cellular g_cellular;
static mg_lock g_cellular_lock = MG_LOCK_INIT;

void mg_cellular_read(mg_cellular *out)
{
    mg_lock_acquire(&g_cellular_lock);
    *out = g_cellular;
    mg_lock_release(&g_cellular_lock);
}

void mg_cellular_set(const mg_cellular *now)
{
    mg_lock_acquire(&g_cellular_lock);
    if (now) g_cellular = *now;
    else memset(&g_cellular, 0, sizeof(g_cellular));
    g_cellular.operator_name[sizeof(g_cellular.operator_name) - 1] = 0;
    g_cellular.mcc_mnc[sizeof(g_cellular.mcc_mnc) - 1] = 0;
    mg_lock_release(&g_cellular_lock);
}

static void on_guest_exit(int status) { mg_run_ended(status, NULL); }
static void on_crash(const char *report) { mg_run_ended(-1, report); }

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
_Static_assert(BOTH(MEGLER_STORAGE_READ, MG_O_READ, 1) && BOTH(MEGLER_STORAGE_WRITE, MG_O_WRITE, 2) && BOTH(MEGLER_STORAGE_CREATE, MG_O_CREATE, 4) &&
               BOTH(MEGLER_STORAGE_TRUNCATE, MG_O_TRUNC, 8) && BOTH(MEGLER_STORAGE_APPEND, MG_O_APPEND, 16) && BOTH(MEGLER_STORAGE_EXCLUSIVE, MG_O_EXCL, 32),
               "megler.h's storage flags are the host layer's");
_Static_assert(BOTH(MEGLER_STORAGE_FILE, MG_FOREIGN_FILE, 0) && BOTH(MEGLER_STORAGE_FOLDER, MG_FOREIGN_DIR, 1) && BOTH(MEGLER_STORAGE_ANY, MG_FOREIGN_ANY, 2),
               "megler.h's storage kinds are the host layer's");
_Static_assert(BOTH(MEGLER_STORAGE_OK, MG_E_OK, 0) && BOTH(MEGLER_STORAGE_NOT_FOUND, MG_E_NOENT, 1) && BOTH(MEGLER_STORAGE_DENIED, MG_E_ACCESS, 2) &&
               BOTH(MEGLER_STORAGE_EXISTS, MG_E_EXIST, 3) && BOTH(MEGLER_STORAGE_NOT_A_FOLDER, MG_E_NOTDIR, 4) && BOTH(MEGLER_STORAGE_IS_A_FOLDER, MG_E_ISDIR, 5) &&
               BOTH(MEGLER_STORAGE_FULL, MG_E_NOSPC, 6) && BOTH(MEGLER_STORAGE_FAILED, MG_E_IO, 7) && BOTH(MEGLER_STORAGE_INVALID, MG_E_INVAL, 8) &&
               BOTH(MEGLER_STORAGE_NOT_EMPTY, MG_E_NOTEMPTY, 9) && BOTH(MEGLER_STORAGE_BUSY, MG_E_BUSY, 10) && BOTH(MEGLER_STORAGE_OTHER_DEVICE, MG_E_XDEV, 11),
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
    if (!link_there(link)) return MG_E_NOENT;
    return link_writable(link) && has_function ? 0 : MG_E_ACCESS;
}

static void *storage_open(void *user, int link, const char *path, int flags, int kind, int *err)
{
    bool changes = kind == MG_FOREIGN_FILE && (flags & (MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC | MG_O_APPEND));
    (void)user;
    *err = changes ? link_refuses(link, true) : link_there(link) ? 0 : MG_E_NOENT;
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

void mg_run_storage(const char *data_dir, const megler_storage_host *storage)
{
    static const mg_foreign foreign = { NULL, storage_open, storage_mkdir, storage_remove, storage_rename };
    mg_fs_link links[MEGLER_STORAGE_MAX_LINKS];
    char *shared = mg_sprintf("%s/sdcard", data_dir);
    int i, k, n = 0;
    mg_foreign_set(NULL);
    for (i = 0; i < MEGLER_STORAGE_MAX_LINKS; i++) {
        free(g_link_names[i]);
        g_link_names[i] = NULL;
    }
    memset(&g_storage, 0, sizeof(g_storage));
    memset(g_links, 0, sizeof(g_links));
    if (storage && storage->links && storage->link_count > 0 && storage->open) {
        g_storage = *storage;
        if (g_storage.link_count > MEGLER_STORAGE_MAX_LINKS) {
            mg_warn("storage: %d links were given and %d is as many as there can be: the rest are left out", g_storage.link_count, MEGLER_STORAGE_MAX_LINKS);
            g_storage.link_count = MEGLER_STORAGE_MAX_LINKS;
        }
        g_storage.links = g_links;
        for (i = 0; i < g_storage.link_count; i++) {
            const char *name = storage->links[i].name, *taken = NULL;
            /* (A link that is left out keeps its number: the numbers are the shell's.) */
            if (!link_name_ok(name)) {
                mg_warn("storage: link %d is left out: \"%s\" cannot be the name of a folder in the shared storage", i, name ? name : "");
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
                mg_warn("storage: link %d (\"%s\") is left out: link \"%s\" has that name already", i, name, taken);
                continue;
            }
            g_link_names[i] = mg_strdup(name);
            g_links[i].name = g_link_names[i];
            g_links[i].writable = storage->links[i].writable;
            links[n].name = g_link_names[i];
            links[n].root = i;
            links[n].read_only = !g_links[i].writable;
            n++;
        }
        if (n) mg_foreign_set(&foreign);
    }
    mg_fs_shared_storage(shared, links, n);
    for (i = 0; i < n; i++) mg_info("storage: /sdcard/%s is a folder of the phone's own (link %d%s)", links[i].name, links[i].root, links[i].read_only ? ", to read only" : "");
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

/*
 * The other apps that have run here, each with a folder in <data dir>/apps: their own parts of the shared storage are where
 * this app finds them, as on Android 9, where an app with the storage permission reaches every app's Android/data and
 * Android/obb (Android 11 stopped that). A file manager puts a file where a game looks for one: Asphalt 9 reads its
 * qaTestingConfigs.txt in its own two folders. The names of folders that cannot be a package's are passed over.
 */
typedef struct other_apps {
    const char *data_dir;
    int count;
} other_apps;

static void other_app(const char *name, bool is_dir, void *user)
{
    other_apps *o = (other_apps *)user;
    const char *c;
    char *own;
    if (!is_dir || name[0] == '.' || !name[0]) return;
    for (c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_')) return;
    own = mg_run_own_storage(o->data_dir, name);
    mg_fs_other_app(name, own);
    free(own);
    o->count++;
}

char *mg_run_own_storage(const char *data_dir, const char *package) { return mg_sprintf("%s/apps/%s/sdcard", data_dir, package); }

/*
 * The services every app shares -- microG, which Android runs once for all apps -- keep their data in one folder beside the
 * shared storage, <storage_dir>/services/data, which is /data/megler-services to every app: microG's state as an app hosts it
 * (megler.MicroG) and the accounts (megler.Accounts) are there. The service's own package, run as an app (microG's
 * settings), has that folder for its private files, so that what it is set to there is what every app's microG finds: device
 * registration turned on, the check-in's Android ID, the accounts it signed in.
 */
#define SERVICES_GUEST "/data/megler-services"
static bool service_package(const char *package) { return !strcmp(package, "com.google.android.gms") || !strcmp(package, "com.google.android.gsf"); }

/* The host folder of an app's private files (/data/data/<package>), malloc'd: its own, or the services' shared one's part. */
static char *internal_dir(const char *data_dir, const char *storage_dir, const char *package)
{
    return service_package(package) ? mg_sprintf("%s/services/data/%s", storage_dir, package) : mg_sprintf("%s/apps/%s/data", data_dir, package);
}

/* A folder and everything in it. */
struct tree {
    const char *path;
    int files, failed;
};

static void remove_tree(const char *path, struct tree *t);

static void remove_entry(const char *name, bool is_dir, void *user)
{
    struct tree *t = (struct tree *)user;
    char *path = mg_sprintf("%s/%s", t->path, name);
    if (is_dir) remove_tree(path, t);
    else if (mg_path_remove(path, NULL)) t->files++;
    else t->failed++;
    free(path);
}

static void remove_tree(const char *path, struct tree *t)
{
    const char *outer = t->path;
    t->path = path;
    mg_path_list(path, remove_entry, t, NULL);
    t->path = outer;
    if (!mg_path_rmdir(path, NULL)) t->failed++;
}

/*
 * Clearing an app's data, asked for by megler_ask_clear_data (a file named after the package in the shared storage's folder
 * clear-data): done now, before the app's folders are set up and anything of it is open, as Android's "Clear storage" is
 * done with the app stopped. Its private folder goes, and its own Android/data folder in the shared storage; its Android/obb
 * stays, as on Android. Then the ask goes.
 */
bool mg_run_clear_data_if_asked(const char *data_dir, const char *storage_dir, const char *package)
{
    char *ask = mg_sprintf("%s/clear-data/%s", storage_dir, package), *internal, *own, *external;
    struct tree t;
    mg_stat st;
    if (!mg_path_stat(ask, &st, NULL)) {
        free(ask);
        return false;
    }
    internal = internal_dir(data_dir, storage_dir, package);
    own = mg_run_own_storage(storage_dir, package);
    external = mg_sprintf("%s/Android/data/%s", own, package);
    memset(&t, 0, sizeof(t));
    if (mg_path_stat(internal, &st, NULL)) remove_tree(internal, &t);
    if (mg_path_stat(external, &st, NULL)) remove_tree(external, &t);
    mg_path_remove(ask, NULL);
    if (t.failed) mg_warn("run: the app's data was cleared, as asked, but for %d file(s) or folder(s) that could not be removed (%d files removed)", t.failed, t.files);
    else mg_info("run: the app's data was cleared, as asked: %d files removed from %s and %s", t.files, internal, external);
    free(ask);
    free(internal);
    free(own);
    free(external);
    return true;
}

void mg_run_files(const char *data_dir, const char *storage_dir, const char *package, const megler_storage_host *storage)
{
    char *services = mg_sprintf("%s/services/data", storage_dir);
    char *internal;
    char *own = mg_run_own_storage(storage_dir, package), *apps;
    left_behind left;
    other_apps others;
    mg_run_clear_data_if_asked(data_dir, storage_dir, package);
    internal = internal_dir(data_dir, storage_dir, package);
    mg_fs_configure(package, internal, own);
    mg_path_mkdirs(services);
    mg_fs_mount(SERVICES_GUEST, services);
    free(services);
    mg_run_storage(storage_dir, storage);
    others.data_dir = storage_dir;
    others.count = 0;
    apps = mg_sprintf("%s/apps", storage_dir);
    mg_path_list(apps, other_app, &others, NULL);
    free(apps);
    if (others.count > 1) mg_info("storage: the own parts of %d other app(s) are in /sdcard/Android/data and obb, as on Android 9", others.count - 1);
    /*
     * Until the storage was shared every app had a whole one to itself, in the folder its own part is still kept in. What an app put
     * there outside Android/data and Android/obb is still on the disk and no longer in /sdcard. Nothing is moved; it is said.
     */
    memset(&left, 0, sizeof(left));
    if (mg_path_list(own, left_one, &left, NULL) && left.count)
        mg_warn("storage: %d thing(s) this app kept in a storage of its own are not in the shared one, where it now looks: %s%s (still in %s)", left.count, left.names,
                left.count > 6 ? ", ..." : "", own);
    free(internal);
    free(own);
}

/* The splits megler_run_config names, in its folder (sorted by name) and then in its list, each once, into g_splits. */
static void gather_splits(const megler_run_config *config)
{
    char **listed = NULL;
    int n = mg_apk_list_splits(config->splits_dir, &listed), i, k;
    g_splits = (char **)mg_alloc(sizeof(char *) * (size_t)(n + (config->split_count > 0 ? config->split_count : 0) + 1));
    g_nsplits = 0;
    for (i = 0; i < n; i++) g_splits[g_nsplits++] = listed[i];
    free(listed);
    for (i = 0; i < config->split_count; i++) {
        if (!config->split_apks || !config->split_apks[i]) continue;
        for (k = 0; k < g_nsplits && strcmp(g_splits[k], config->split_apks[i]); k++) { }
        if (k == g_nsplits) g_splits[g_nsplits++] = mg_strdup(config->split_apks[i]);
    }
}

bool mg_run_start(const megler_run_config *config)
{
    static mg_apk_info info;
    int i;
    g_error[0] = 0;
    if (g_started_once) {
        mg_run_set_error("an app has already run in this process; close Megler and open it again to start another");
        return false;
    }
    if (!mg_core_can_run()) {
        mg_run_set_error("Android code cannot run: executable memory was refused (is the codeGeneration capability granted?)");
        return false;
    }
    if (!config || !config->apk_path) {
        mg_run_set_error("no APK given");
        return false;
    }
    mg_apk_free_list(g_splits, g_nsplits);
    gather_splits(config);
    if (!mg_apk_read_app(config->apk_path, (const char *const *)g_splits, g_nsplits, config->splits_dir, &info)) {
        mg_run_set_error("%s", info.error[0] ? info.error : "the APK cannot be read");
        return false;
    }
    if (!info.runnable) {
        mg_run_set_error("%s", info.reason);
        mg_apk_info_free(&info);
        return false;
    }
    g_driver = mg_driver_find(info.engine);
    if (!g_driver) {
        mg_run_set_error("no driver yet for this kind of app (%s)", info.engine);
        mg_apk_info_free(&info);
        return false;
    }
    g_started_once = true;
    g_config = *config;
    g_apk_path = mg_strdup(config->apk_path);
    g_config.apk_path = g_apk_path;
    g_config.gms_apk_path = config->gms_apk_path ? mg_strdup(config->gms_apk_path) : NULL;
    g_config.gsf_apk_path = config->gsf_apk_path ? mg_strdup(config->gsf_apk_path) : NULL;
    /*
     * The drivers see the splits as one list, of those the reading took (a split of another app is no split of this one),
     * in the order of info.splits, which names them.
     */
    g_config.split_apks = (const char *const *)g_splits;
    g_config.split_count = 0;
    for (i = 0; i < g_nsplits; i++) {
        mg_apk_info *part = (mg_apk_info *)mg_alloc(sizeof(*part));
        if (g_config.split_count < MG_APK_MAX_SPLITS && mg_apk_read_info(g_splits[i], part) && !strcmp(part->package, info.package) && part->split[0])
            g_splits[g_config.split_count++] = g_splits[i];
        else free(g_splits[i]);
        free(part);
    }
    g_nsplits = g_config.split_count;

    if (!mg_ld_add_apk(config->apk_path)) {
        mg_run_set_error("the APK cannot be opened");
        mg_apk_info_free(&info);
        return false;
    }
    for (i = 0; i < g_config.split_count; i++) mg_ld_add_apk(g_config.split_apks[i]);
    if (g_config.split_count) {
        char names[600];
        size_t at = 0;
        names[0] = 0;
        for (i = 0; i < info.nsplits && at + 4 < sizeof(names); i++) at += (size_t)snprintf(names + at, sizeof(names) - at, "%s%s", i ? ", " : "", info.splits[i]);
        mg_info("run: the app is in %d APKs, its base and the split%s %s", g_config.split_count + 1, g_config.split_count == 1 ? "" : "s", names);
    }

    /* Each app gets its own Android data directories under ours, and all of them one shared storage. */
    mg_run_files(megler_data_dir(), megler_storage_dir(), info.package, &g_config.storage);

    mg_guest_exit_hook = on_guest_exit;
    mg_core_set_crash_hook(on_crash);
    mg_atomic_xchg(&g_active, 1);
    mg_info("run: %s (%s) with the %s driver, %dx%d at %d dpi", info.package, info.label[0] ? info.label : "no label", g_driver->name,
            config->width, config->height, config->density_dpi);
    mg_bionic_hang_watch_start();
    if (mg_flag("trace-cpu")) mg_bionic_cpu_trace_start();
    if (mg_flag("trace-native-stacks")) mg_bionic_native_stacks_start();
    if (mg_flag("trace-profile")) mg_bionic_profile_start();
    if (!g_driver->start(&g_config, &info)) {
        mg_atomic_xchg(&g_active, 0);
        if (!g_error[0]) mg_run_set_error("the app did not start");
        mg_apk_info_free(&info);
        return false;
    }
    mg_apk_info_free(&info);
    return true;
}

void mg_run_touch(int phase, int pointer_id, float x, float y)
{
    if (g_active && g_driver && g_driver->touch) g_driver->touch(phase, pointer_id, x, y);
}

void mg_run_key(int android_keycode, bool down)
{
    if (g_active && g_driver && g_driver->key) g_driver->key(android_keycode, down);
}

void mg_run_set_paused(bool paused)
{
    if (g_active && g_driver && g_driver->set_paused) g_driver->set_paused(paused);
}

void mg_run_set_focused(bool focused)
{
    if (g_active && g_driver && g_driver->set_focused) g_driver->set_focused(focused);
}

void mg_run_stop(void)
{
    if (!g_active) return;
    if (g_driver && g_driver->stop) g_driver->stop();
    else mg_run_ended(0, "stopped");
}
