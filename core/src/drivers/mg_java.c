/*
 * The driver for an app that is Java: an Activity, views, no engine.
 *
 * Unlike the other drivers this one plays no Java part in C. It starts the
 * Dalvik runtime (dvm/) on Android's libcore, puts the Java framework and the
 * app's own DEX files after it, and calls megler.AppRunner.run on a thread that
 * stands for Android's main thread. The framework is core/framework's
 * (tools/build-framework.sh): android.* in Java, which does in the app's own
 * process what Android does in system_server -- the Application, the
 * activity stack, the main Looper, views measured, laid out and drawn into a
 * bitmap. What it cannot do in Java it asks of megler.* classes' native
 * methods. This file is megler.Native: the log, the APK's files, the screen's
 * size, input, the end of the app. megler.Gfx, the drawing, is mg_java_gfx.c;
 * OpenGL ES and EGL, and a GLSurfaceView's thread, mg_java_gl.c.
 *
 * Everything the runtime needs to start is in one folder laid out like
 * Android's /system, the "Android root" (tools/dvm-android-root.sh):
 * megler_run_config.android_root, or the folder `android` in the package.
 *
 * Input is not delivered: it is asked for. The framework's main loop calls
 * pollInput between messages (every 8 ms at most), so megler_touch and the
 * rest only queue here, and everything that happens to the app happens on its
 * main thread. Stopping is the same: a flag the next poll sees, which
 * finishes every activity; the framework then calls exit, and the main loop
 * is told to end.
 */
#include <stdio.h>

#include "../bionic/mg_bionic.h"
#include "../bionic/mg_fd.h"
#include "../dvm/mg_dvm_internal.h"
#include "../host/mg_host.h"
#include "../mg_core.h"
#include "../ld/mg_ld.h"
#include "../ndk/mg_egl.h"
#include "../ndk/mg_ndk.h"
#include "../run/mg_run.h"
#include "../zip/mg_zip.h"
#include "mg_driver.h"
#include "../apk/mg_arsc.h"
#include "mg_java.h"
#include "mg_java_gfx.h"
#include "mg_java_gl.h"

#define MAX_EVENTS 256
/*
 * APKs 12 on are those the framework adds (addApk): Google Play services' and its framework's for an app that hosts them, the
 * resource archives each keeps for the language (locale-filtered-resources), and the WebView's. Play Store with Google Play
 * services hosted took the four of 16 there were before the WebView's, and its sign-in page had no WebView.
 */
#define MAX_APKS 32
/* The app's splits are APKs 2 to 11: ten, which is all the framework reads (its Resources looks in 2 to 11). */
#define FIRST_SPLIT_APK 2
#define MAX_SPLITS 10

/* The phases of an event as megler.AppRunner.deliver reads them. */
enum { EV_DOWN = 0, EV_MOVE = 1, EV_UP = 2, EV_CANCEL = 3, EV_KEY = 4, EV_BACK = 5, EV_HOST_PAUSE = 6, EV_TEXT = 7, EV_INSETS = 8, EV_PHONE = 10 };

#define MAX_TEXT 512
#define MAX_PHONE 64

static struct {
    const megler_run_config *config;
    char *root;                        /* the Android root, a host path */
    char activity[200], application[200], package[128];
    char guest_apk[400];               /* the APK's path as the app sees it */
    bool apache_http;                  /* the app gets Android's legacy Apache HTTP client (org.apache.http.legacy): see apache_http_wanted */
    bool platform_app;                 /* an app of the platform's own (sharedUserId android.uid.system): see add_platform_framework */
    /*
     * The app's splits (megler_run_config.split_apks, as many as there is room for): their names from their manifests
     * ("config.armeabi_v7a"), their paths as the app sees them (Native.splitPaths), and whether they have code.
     */
    int nsplits;
    char split_names[MAX_SPLITS][96];
    char *guest_splits[MAX_SPLITS];
    bool split_code[MAX_SPLITS];
    mg_thread *main;
    /*
     * The APKs by the framework's numbers: 0 the app's, 1 the platform's resources, 2 to 11 the app's splits, 12 on the shared
     * libraries the framework adds while the app runs (Native.addApk: Android's WebView). The framework looks for a split's
     * files in 2 to 11 alone.
     */
    mg_zip apk[MAX_APKS];
    bool apk_open[MAX_APKS];
    char *apk_path[MAX_APKS];
    mg_lock lock;
    i32 events[MAX_EVENTS * 4];
    int nevents;
    volatile i32 stop, exited, ended;
    int end_status;
    char end_reason[1600];
    mg_jobj *clipboard;
    mg_java_stats stats;
    /*
     * What the shell's keyboard has sent and the app has not taken yet, oldest first: "t<text>" typed text, "d" Backspace,
     * "e" Enter, "h" the keyboard was put away -- the letters megler.InputMethods reads. One EV_TEXT event is queued for
     * each, and each such event takes one, so that text and the keys that go as key events stay in the order they were sent.
     */
    char *text[MAX_TEXT];
    int ntext;
    /*
     * The phone's text messages and calls the app has not taken yet, oldest first (megler_sms_received, megler_call_logged):
     * "s<ms>\n<from>\n<text>" and "c<ms>\n<type>\n<seconds>\n<number>", as megler.Phone reads them. One EV_PHONE event each.
     */
    char *phone[MAX_PHONE];
    int nphone;
    volatile i32 keyboard_height;      /* pixels of the surface's bottom the keyboard covers */
    volatile i32 enter_newline;        /* the field the keyboard was last shown for takes Enter as a new line */
    /* The battery as the app was last told of it by a broadcast (at the start: as it was then), and the core's count of its changes at that time. */
    mg_battery battery;
    u32 battery_changes;
} J;

static i32 bits_of(float f)
{
    i32 v;
    memcpy(&v, &f, 4);
    return v;
}

static const char *text_of(mg_jobj *s)
{
    const char *t = mg_dvm_string_utf8(s);
    return t ? t : "";
}

/* ----------------------------------------------------------- megler.Native */

#define N(name) static void name(mg_jcall *c)

/* The phone's look (megler_set_theme): its accent colour, 0 for none, and whether it is dark. */
static volatile i32 g_theme_accent, g_theme_night;

/*
 * log(int priority, String tag, String msg): Android's priorities, 2 verbose to 7 assert, as logcat letters them. A line of
 * the layer's log holds 1 KB, and a message with a stack trace is longer: it goes in pieces, each cut at the end of one of its
 * lines, the first with the tag; at most 64. Cut at 1 KB, Yandex Maps' crash said its cause's class and half its message and
 * not one of its frames (package 507).
 */
N(Native_log)
{
    static const char letters[] = "??VDIWEF";
    int priority = c->args[0].i, level = priority >= 6 ? MG_LOG_ERROR : priority == 5 ? MG_LOG_WARN : MG_LOG_INFO, piece = 0;
    const char *tag = text_of(c->args[1].l), *msg = text_of(c->args[2].l);
    char letter = priority >= 2 && priority <= 7 ? letters[priority] : 'I';
    size_t len = strlen(msg), at = 0;
    do {
        size_t n = len - at;
        if (n > 900) {
            size_t cut = 900;
            while (cut > 0 && msg[at + cut] != '\n') cut--;
            n = cut ? cut : 900;
        }
        if (!piece) mg_log(level, "%c/%s: %.*s", letter, tag, (int)n, msg + at);
        else mg_log(level, "%.*s", (int)n, msg + at);
        at += n;
        if (at < len && msg[at] == '\n') at++;
    } while (at < len && ++piece < 64);
}

static mg_jobj *bytes_of(const u8 *data, size_t size)
{
    mg_jobj *a = mg_dvm_new_array("[B", (i32)size);
    if (a && size) memcpy(a->u.arr.data, data, size);
    return a;
}

/* A file of one of the APKs, whole; null when it is not there. */
static mg_jobj *apk_file(int apk, const char *path)
{
    const mg_zip_entry *e;
    size_t size = 0;
    u8 *data;
    mg_jobj *a;
    if (apk < 0 || apk >= MAX_APKS || !J.apk_open[apk]) return NULL;
    e = mg_zip_find(&J.apk[apk], path);
    if (!e) return NULL;
    data = mg_zip_read(&J.apk[apk], e, 256u << 20, &size);
    if (!data) return NULL;
    a = bytes_of(data, size);
    free(data);
    return a;
}

/*
 * A file of an APK as a descriptor and where in it the file starts, packed
 * as the framework unpacks it: descriptor << 40 | offset. Only a file that is
 * stored, not deflated, can be read that way; -1 otherwise.
 */
static i64 apk_fd(int apk, const char *path)
{
    const mg_zip_entry *e;
    u64 offset = 0;
    int fd;
    if (apk < 0 || apk >= MAX_APKS || !J.apk_open[apk]) return -1;
    e = mg_zip_find(&J.apk[apk], path);
    if (!e || !mg_zip_stored_at(&J.apk[apk], e, &offset)) return -1;
    fd = mg_fd_open_host(J.apk_path[apk], MG_O_READ);
    if (fd < 0) return -1;
    return ((i64)fd << 40) | (i64)offset;
}

static i64 apk_length(int apk, const char *path)
{
    const mg_zip_entry *e = apk >= 0 && apk < MAX_APKS && J.apk_open[apk] ? mg_zip_find(&J.apk[apk], path) : NULL;
    return e ? (i64)e->usize : -1;
}

/*
 * The APK's entry an asset path names: "x" is assets/x. A path that begins with "/" is from the APK's root instead: the
 * framework's AssetManager.openNonAsset and openNonAssetFd pass "/" and the path (SoLoader reads lib/<abi>/ so;
 * Chromium's ApkAssets opens "assets/icudtl.dat" so). Those were looked for under assets/ -- assets/lib/...,
 * assets/assets/icudtl.dat -- and never found.
 */
static char *asset_name(mg_jobj *path)
{
    const char *p = text_of(path);
    if (*p == '/') {
        while (*p == '/') p++;
        return mg_strdup(p);
    }
    return mg_sprintf("assets/%s", p);
}

/*
 * Which of the app's APKs has an entry: the base, else the first split that has it. An app in several APKs has assets in
 * its splits too -- an asset pack is a split of assets alone -- and AssetManager finds them there as it finds the base's,
 * as on Android, whose AssetManager has every split of the app among its APKs. (Android looks in the last added first;
 * a name in two of the app's APKs is no app's intent, and the base's is taken.) -1 when none has it.
 */
static int app_apk_with(const char *name)
{
    int i;
    if (J.apk_open[0] && mg_zip_find(&J.apk[0], name)) return 0;
    for (i = 0; i < J.nsplits; i++)
        if (J.apk_open[FIRST_SPLIT_APK + i] && mg_zip_find(&J.apk[FIRST_SPLIT_APK + i], name)) return FIRST_SPLIT_APK + i;
    return -1;
}

N(Native_readAsset)
{
    char *name = asset_name(c->args[0].l);
    c->ret.l = apk_file(app_apk_with(name), name);
    free(name);
}

N(Native_assetFd)
{
    char *name = asset_name(c->args[0].l);
    c->ret.j = apk_fd(app_apk_with(name), name);
    free(name);
}

/*
 * (String name) -> long[] { descriptor of the APK, where the entry's deflated bytes begin, how many }: an asset the APK
 * deflates, which AssetManager.open inflates as it is read (InflatedAssetStream) rather than whole into a byte[] -- WhatsApp
 * opened five of 20 MB as it started, and the arrays stayed, held through stale words of stacks. null for any other.
 */
N(Native_assetDeflated)
{
    char *name = asset_name(c->args[0].l);
    int apk = app_apk_with(name);
    const mg_zip_entry *e = apk >= 0 && apk < MAX_APKS && J.apk_open[apk] ? mg_zip_find(&J.apk[apk], name) : NULL;
    u64 at = 0;
    c->ret.l = NULL;
    if (e && e->method == 8 && mg_zip_data_at(&J.apk[apk], e, &at)) {
        int fd = mg_fd_open_host(J.apk_path[apk], MG_O_READ);
        mg_jobj *a = fd >= 0 ? mg_dvm_new_array("[J", 3) : NULL;
        if (a) {
            i64 *v = (i64 *)a->u.arr.data;
            v[0] = fd;
            v[1] = (i64)at;
            v[2] = (i64)e->csize;
            c->ret.l = a;
        } else if (fd >= 0) {
            mg_fd_close(fd);
        }
    }
    free(name);
}

N(Native_assetLength)
{
    char *name = asset_name(c->args[0].l);
    c->ret.j = apk_length(app_apk_with(name), name);
    free(name);
}

typedef struct asset_list {
    const char *dir;                   /* "assets/sub/" */
    size_t dir_len;
    char **names;
    int n, cap;
} asset_list;

/* An entry under the folder: its own name if it is a file there, the name of the folder it is in if it lies deeper; each once. */
static void list_one(const mg_zip_entry *e, void *user)
{
    asset_list *l = (asset_list *)user;
    const char *rest = e->name + l->dir_len, *slash = strchr(rest, '/');
    size_t len = slash ? (size_t)(slash - rest) : strlen(rest);
    int i;
    if (!len) return;
    for (i = 0; i < l->n; i++)
        if (strlen(l->names[i]) == len && !memcmp(l->names[i], rest, len)) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->names = (char **)mg_realloc(l->names, sizeof(char *) * (size_t)l->cap);
    }
    l->names[l->n] = (char *)mg_alloc(len + 1);
    memcpy(l->names[l->n], rest, len);
    l->names[l->n++][len] = 0;
}

/* megler.Native.dnsServers: the phone's name servers (mg_net_dns_servers), for LinkProperties.getDnsServers. */
N(Native_dnsServers)
{
    char servers[8][48];
    int n = mg_net_dns_servers(servers, 8), i;
    mg_jobj *a = mg_dvm_new_array("[Ljava/lang/String;", n);
    for (i = 0; a && i < n; i++) a->u.oarr.v[i] = mg_dvm_new_string(servers[i]);
    c->ret.l = a;
}

N(Native_listAssets)
{
    asset_list l;
    char *dir = asset_name(c->args[0].l), *prefix;
    size_t n = strlen(dir);
    mg_jobj *a;
    int i;
    while (n && dir[n - 1] == '/') dir[--n] = 0;
    prefix = mg_sprintf("%s/", dir);
    memset(&l, 0, sizeof(l));
    l.dir = prefix;
    l.dir_len = strlen(prefix);
    /* The base's, then what the splits have besides, each name once (list_one sees to that). */
    if (J.apk_open[0]) mg_zip_each(&J.apk[0], prefix, list_one, &l);
    for (i = 0; i < J.nsplits; i++)
        if (J.apk_open[FIRST_SPLIT_APK + i]) mg_zip_each(&J.apk[FIRST_SPLIT_APK + i], prefix, list_one, &l);
    a = mg_dvm_new_array("[Ljava/lang/String;", l.n);
    for (i = 0; i < l.n; i++) {
        if (a) a->u.oarr.v[i] = mg_dvm_new_string(l.names[i]);
        free(l.names[i]);
    }
    free(l.names);
    free(prefix);
    free(dir);
    c->ret.l = a;
}

N(Native_readApkFile) { c->ret.l = apk_file(c->args[0].i, text_of(c->args[1].l)); }

/*
 * readApkResourceTable(apk, languages): the APK's resources.arsc, cut down to the resources of no language and of
 * `languages` (mg_arsc_keep_languages), or whole where nothing goes or it cannot be cut; null when there is none. For
 * another package's resources in the app (Google Play services' 84 MB table, of which the phone's language and English
 * are a tenth: apk/mg_arsc.h says why).
 */
N(Native_readApkResourceTable)
{
    int apk = c->args[0].i, n = 0, i;
    mg_jobj *list = c->args[1].l;
    char names[8][8], joined[80];
    const char *langs[8], *file;
    const mg_zip_entry *e;
    size_t size = 0, cut_size = 0;
    u8 *data, *cut;
    char *kept = NULL;
    c->ret.l = NULL;
    if (apk < 0 || apk >= MAX_APKS || !J.apk_open[apk]) return;
    e = mg_zip_find(&J.apk[apk], "resources.arsc");
    if (!e) return;
    joined[0] = 0;
    for (i = 0; list && i < (int)list->u.oarr.len && n < 8; i++)
        if (list->u.oarr.v[i]) {
            const char *l = text_of(list->u.oarr.v[i]);
            size_t at = strlen(joined);
            if (!*l || strspn(l, "abcdefghijklmnopqrstuvwxyz") != strlen(l)) continue;
            mg_strlcpy(names[n], l, sizeof(names[n]));
            langs[n] = names[n];
            snprintf(joined + at, sizeof(joined) - at, "-%s", names[n]);
            n++;
        }
    file = strrchr(J.apk_path[apk], '/');
    if (!file) file = strrchr(J.apk_path[apk], '\\');
    file = file ? file + 1 : J.apk_path[apk];
    /*
     * The table cut down is kept beside the APK, named for the table's CRC-32 and the languages, and read from there the next
     * time, by every app: Google Play services' 84 MB were read and cut at every app's start (0.7 s on the phone), for the
     * same 9 MB each time. The APK is in the services' shared folder, which all of Megler's packages can write to.
     */
    if (e->usize >= (1u << 20) && n) {
        size_t have = 0;
        u8 *bytes;
        kept = mg_sprintf("%s.%08x%s.arsc", J.apk_path[apk], (unsigned)e->crc, joined);
        bytes = mg_file_read_all(kept, &have);
        if (bytes && have >= 12 && bytes[0] == 2 && bytes[1] == 0) {
            c->ret.l = bytes_of(bytes, have);
            mg_info("java: %s's resource table: %u KB with the languages the app has (%s), as cut before", file, (unsigned)(have >> 10), joined + 1);
            free(bytes);
            free(kept);
            return;
        }
        free(bytes);
    }
    if (!(data = mg_zip_read(&J.apk[apk], e, 256u << 20, &size))) {
        free(kept);
        return;
    }
    cut = size >= (1u << 20) && n ? mg_arsc_keep_languages(data, size, langs, n, &cut_size) : NULL;
    if (cut) {
        mg_info("java: %s's resource table: %u KB, %u KB with the languages the app has (%s)", file, (unsigned)(size >> 10), (unsigned)(cut_size >> 10),
                joined + 1);
        c->ret.l = bytes_of(cut, cut_size);
        if (kept) {
            /* Written aside and renamed: another app may read it meanwhile, or write it too. */
            char *part = mg_sprintf("%s.%u.part", kept, (unsigned)mg_thread_self_id());
            int err = 0;
            if (!mg_file_write_all(part, cut, cut_size) || !mg_path_rename(part, kept, &err)) mg_path_remove(part, &err);
            free(part);
        }
        free(cut);
    } else {
        c->ret.l = bytes_of(data, size);
    }
    free(data);
    free(kept);
}

#define FIRST_LIBRARY_APK 12

static void open_apk(int index, const char *host_path);

/*
 * addApk(String path): a shared library's APK, which the framework's AssetManager adds to the app's resources and files
 * (addAssetPathAsSharedLibrary; the WebView's, /system/app/webview/webview.apk). The path is the app's, as Android names it;
 * the APK is opened as one more of the numbered APKs above, from 12 on, and its number is the answer: the same number for a
 * path already open, -1 when it cannot be opened or there is no room.
 */
N(Native_addApk)
{
    const char *path = text_of(c->args[0].l);
    char *host = path[0] ? mg_fs_host_path(path) : NULL;
    int i, free_at = -1;
    c->ret.i = -1;
    if (!host) return;
    mg_lock_acquire(&J.lock);
    for (i = FIRST_LIBRARY_APK; i < MAX_APKS; i++) {
        if (J.apk_open[i] && !strcmp(J.apk_path[i], host)) {
            c->ret.i = i;
            break;
        }
        if (!J.apk_open[i] && free_at < 0) free_at = i;
    }
    if (c->ret.i < 0 && free_at >= 0) {
        open_apk(free_at, host);
        if (J.apk_open[free_at]) {
            c->ret.i = free_at;
            mg_info("java: %s is APK %d of the app's resources and files", path, free_at);
        }
    } else if (c->ret.i < 0) {
        mg_warn("java: %s cannot be added: all %d places for more APKs are taken", path, MAX_APKS - FIRST_LIBRARY_APK);
    }
    mg_lock_release(&J.lock);
    free(host);
}

/* listApkFiles(int apk, String prefix): what lies under the prefix ("assets/x/") in that APK, each name once (as listAssets). */
N(Native_listApkFiles)
{
    asset_list l;
    int apk = c->args[0].i, i;
    mg_jobj *a;
    memset(&l, 0, sizeof(l));
    l.dir = text_of(c->args[1].l);
    l.dir_len = strlen(l.dir);
    if (apk >= 0 && apk < MAX_APKS && J.apk_open[apk]) mg_zip_each(&J.apk[apk], l.dir, list_one, &l);
    a = mg_dvm_new_array("[Ljava/lang/String;", l.n);
    for (i = 0; i < l.n; i++) {
        if (a) a->u.oarr.v[i] = mg_dvm_new_string(l.names[i]);
        free(l.names[i]);
    }
    free(l.names);
    c->ret.l = a;
}
N(Native_apkFileFd) { c->ret.j = apk_fd(c->args[0].i, text_of(c->args[1].l)); }
N(Native_apkFileLength) { c->ret.j = apk_length(c->args[0].i, text_of(c->args[1].l)); }

/*
 * The APK as the app's own code names it: under /data/app/<package>-1, where Android installs an app, and readable there
 * (the folder the file is in on the host is mounted at that place), so that the class loader the framework makes over
 * it finds the APK's resources, and code that opens its own APK as a zip file can.
 */
N(Native_apkPath) { c->ret.l = mg_dvm_new_string(J.guest_apk); }

/* setAppClassLoader(loader): the PathClassLoader the framework made for the app; the app's classes name it as theirs from now on. */
N(Native_setAppClassLoader) { mg_dvm_set_app_loader(c->args[0].l); }

/*
 * The splits as the app's own code names them, beside the base where Android installs them:
 * /data/app/<package>-1/split_<name>.apk (mount_apk). The framework makes ApplicationInfo.splitNames and splitSourceDirs
 * of these, and the app's class loader's path.
 */
N(Native_splitPaths)
{
    mg_jobj *a = mg_dvm_new_array("[Ljava/lang/String;", J.nsplits);
    int i;
    for (i = 0; a && i < J.nsplits; i++) a->u.oarr.v[i] = mg_dvm_new_string(J.guest_splits[i]);
    c->ret.l = a;
}

/*
 * Looper.prepare() and every turn of a message loop: the thread's native
 * looper, for native code that watches descriptors on its UI thread
 * (ALooper_forThread, ALooper_addFd). See ndk/mg_ndk.h. nativeLooperPoll
 * answers whether there is one, which is what makes an idle loop of a thread
 * other than the main one come round every 8 ms instead of sleeping.
 */
N(Native_looperPrepare) { (void)c; mg_looper_java_thread(); }
N(Native_looperPoll) { c->ret.z = mg_looper_poll_thread(); }
N(Native_nothing) { (void)c; }
N(Native_dataDir) { c->ret.l = mg_dvm_new_string(mg_fs_internal_dir()); }
N(Native_externalDir) { c->ret.l = mg_dvm_new_string(mg_fs_external_dir()); }
N(Native_packageName) { c->ret.l = mg_dvm_new_string(J.package); }
N(Native_screenWidth) { c->ret.i = J.config->width; }
N(Native_screenHeight) { c->ret.i = J.config->height; }
N(Native_density) { c->ret.f = (float)(J.config->density_dpi > 0 ? J.config->density_dpi : 160) / 160.0f; }
N(Native_uptimeNanos) { c->ret.j = (i64)mg_time_monotonic_ns(); }
/* flag(name): a switch for trying something on the phone (mg_flag): a file of that name in the data folder's flags. */
N(Native_flag) { c->ret.z = mg_flag(text_of(c->args[0].l)); }
/* askClearData(): this app's data cleared when it next starts (megler_ask_clear_data), for Android's app info screen. */
N(Native_askClearData) { c->ret.z = megler_ask_clear_data(J.package); }
static void Native_frameMark(mg_jcall *c);
static void Native_presentRect(mg_jcall *c);
static void Native_checkPartial(mg_jcall *c);

/*
 * The battery has been set to something else since the app was last told (mg_battery_set, from any thread; this is the
 * app's main thread). Android broadcasts ACTION_BATTERY_CHANGED to whoever is registered for it, and before that
 * ACTION_POWER_CONNECTED or ACTION_POWER_DISCONNECTED when it went between running on its battery and not; the
 * framework's megler.Broadcasts sends them (the overlay's batteryChanged). What is compared is what the app was told last
 * with what is there now, so a battery that changed and changed back between two looks is no news, and nothing is sent
 * more often than the charge, the status or the plug changes.
 */
static void battery_news(void)
{
    mg_battery now;
    mg_jvalue power;
    J.battery_changes = mg_battery_changes();
    mg_battery_read(&now);
    if (now.level == J.battery.level && now.scale == J.battery.scale && now.status == J.battery.status && now.plugged == J.battery.plugged) return;
    power.j = 0;
    power.i = (now.plugged != 0) == (J.battery.plugged != 0) ? 0 : now.plugged ? 1 : 2;
    J.battery = now;
    if (!mg_dvm_call_static("megler/Broadcasts", "batteryChanged", "(I)V", &power, NULL)) {
        mg_note_once("java: the battery changed and the app was not told: %s", mg_dvm_pending() ? mg_dvm_class_of(mg_dvm_pending())->name : "megler.Broadcasts.batteryChanged did not run");
        mg_dvm_clear_pending();
    }
}

/*
 * An activity another app asked for (megler_start_activity, from the shell's thread): kept until the main loop next asks
 * for input, which hands it to megler.AppRunner.startRequested on the app's main thread. The latest ask wins.
 */
static char g_requested_activity[200];
static volatile i32 g_activity_requested;
static mg_lock g_requested_lock = MG_LOCK_INIT;

void mg_java_start_activity(const char *activity_class)
{
    if (!activity_class || !activity_class[0]) return;
    mg_lock_acquire(&g_requested_lock);
    mg_strlcpy(g_requested_activity, activity_class, sizeof(g_requested_activity));
    mg_lock_release(&g_requested_lock);
    mg_atomic_xchg(&g_activity_requested, 1);
    mg_info("java: another app asks for the activity %s", activity_class);
}

static void requested_activity(void)
{
    char name[200];
    mg_jvalue arg;
    if (!g_activity_requested || mg_atomic_cas(&g_activity_requested, 1, 0) != 1) return;
    mg_lock_acquire(&g_requested_lock);
    mg_strlcpy(name, g_requested_activity, sizeof(name));
    mg_lock_release(&g_requested_lock);
    arg.j = 0;
    arg.l = mg_dvm_new_string(name);
    if (!mg_dvm_call_static("megler/AppRunner", "startRequested", "(Ljava/lang/String;)V", &arg, NULL)) {
        mg_warn("java: the activity %s another app asked for was not started: %s", name,
                mg_dvm_pending() ? mg_dvm_class_of(mg_dvm_pending())->name : "megler.AppRunner.startRequested did not run");
        mg_dvm_clear_pending();
    }
}

/*
 * pollInput(): what has happened since the last call, as int[1 + 4n] --
 * n, then (phase, id, x bits, y bits) for each -- or null. Called by the main
 * loop between messages, so this is also where a stop asked from outside
 * reaches the app: every activity is finished, on the thread they live on.
 * And where news of the battery does.
 */
N(Native_pollInput)
{
    mg_jobj *a = NULL;
    int n;
    if (J.stop == 1 && mg_atomic_cas(&J.stop, 1, 2) == 1) {
        mg_dvm_call_static("megler/AppRunner", "finishAll", "()V", NULL, NULL);
        if (mg_dvm_pending()) return;
    }
    if (mg_battery_changes() != J.battery_changes) battery_news();
    requested_activity();
    mg_lock_acquire(&J.lock);
    n = J.nevents;
    if (n) {
        a = mg_dvm_new_array("[I", 1 + 4 * n);
        if (a) {
            i32 *v = (i32 *)a->u.arr.data;
            v[0] = n;
            memcpy(v + 1, J.events, sizeof(i32) * 4 * (size_t)n);
            J.nevents = 0;
        }
    }
    mg_lock_release(&J.lock);
    mg_atomic_add(&J.stats.polls, 1);
    c->ret.l = a;
}

/* The clipboard is the app's own until the shell has one to offer. */
N(Native_setClipboard) { J.clipboard = c->args[0].l; }
N(Native_getClipboard) { c->ret.l = J.clipboard; }

/* The battery as the core has it (mg_battery_read, run/mg_run.h), in the order megler.Native.battery's comment gives. */
N(Native_battery)
{
    mg_battery b;
    mg_jobj *a = mg_dvm_new_array("[I", 8);
    mg_battery_read(&b);
    if (a) {
        i32 *v = (i32 *)a->u.arr.data;
        v[0] = b.level;
        v[1] = b.scale;
        v[2] = b.status;
        v[3] = b.plugged;
        v[4] = b.health;
        v[5] = b.voltage_mv;
        v[6] = b.temperature;
        v[7] = b.present ? 1 : 0;
    }
    c->ret.l = a;
}

/*
 * The microphone (megler_microphone_host): what an AudioRecord reads. The shell pushes what it hears into a ring of two
 * seconds (megler_microphone_push); micRead takes it out, waiting for it when asked to block. When nothing comes for a
 * second (the microphone gone), a blocking read is given silence, so that the app does not hang. One recording at a time,
 * as on Android: a second start takes the microphone over.
 */
static struct {
    mg_lock lock;
    mg_cond more;
    bool inited, on;
    i16 *ring;
    u32 cap, head, count;   /* samples */
    int channels;
} MIC = { MG_LOCK_INIT };

static void mic_init(void)
{
    static mg_lock once = MG_LOCK_INIT;
    mg_lock_acquire(&once);
    if (!MIC.inited) {
        mg_cond_init(&MIC.more);
        MIC.inited = true;
    }
    mg_lock_release(&once);
}

void megler_microphone_push(const int16_t *samples, int frames)
{
    u32 n, i;
    if (!MIC.inited || frames <= 0 || !samples) return;
    mg_lock_acquire(&MIC.lock);
    if (MIC.on && MIC.ring) {
        n = (u32)frames * (u32)MIC.channels;
        for (i = 0; i < n; i++) {
            MIC.ring[(MIC.head + MIC.count) % MIC.cap] = samples[i];
            if (MIC.count < MIC.cap) MIC.count++;
            else MIC.head = (MIC.head + 1) % MIC.cap;   /* full: the oldest goes */
        }
        mg_cond_broadcast(&MIC.more);
    }
    mg_lock_release(&MIC.lock);
}

/* micStart(rate, channels): the microphone asked of the shell; true when it hears. */
N(Native_micStart)
{
    const megler_microphone_host *h = &J.config->microphone;
    int rate = c->args[0].i, channels = c->args[1].i == 2 ? 2 : 1;
    bool ok;
    mic_init();
    if (rate < 4000 || rate > 192000) rate = 44100;
    mg_lock_acquire(&MIC.lock);
    free(MIC.ring);
    MIC.cap = (u32)rate * (u32)channels * 2u;
    MIC.ring = (i16 *)mg_alloc(sizeof(i16) * MIC.cap);
    MIC.channels = channels;
    MIC.head = MIC.count = 0;
    MIC.on = true;
    mg_lock_release(&MIC.lock);
    ok = h->start ? h->start(h->user, rate, channels) : false;
    mg_info("java: the app records at %d Hz in %d channel%s: %s", rate, channels, channels == 2 ? "s" : "",
            ok ? "the microphone hears" : h->start ? "the microphone was refused; the app reads silence" : "no microphone here; the app reads silence");
    if (!ok) {
        mg_lock_acquire(&MIC.lock);
        MIC.on = false;
        mg_lock_release(&MIC.lock);
    }
    c->ret.z = ok;
}

/* micStop(): the microphone no longer wanted. */
N(Native_micStop)
{
    const megler_microphone_host *h = &J.config->microphone;
    bool was;
    (void)c;
    mic_init();
    mg_lock_acquire(&MIC.lock);
    was = MIC.on;
    MIC.on = false;
    mg_cond_broadcast(&MIC.more);
    mg_lock_release(&MIC.lock);
    if (was && h->stop) h->stop(h->user);
}

/* audioRoute(route): where the app's sound is to go, as its AudioManager has it (megler_audio_host.route). */
N(Native_audioRoute)
{
    const megler_audio_host *h = &J.config->audio;
    int route = c->args[0].i;
    if (route < MEGLER_AUDIO_ROUTE_DEFAULT || route > MEGLER_AUDIO_ROUTE_SPEAKER) route = MEGLER_AUDIO_ROUTE_DEFAULT;
    mg_info("java: the app's sound %s", route == MEGLER_AUDIO_ROUTE_EARPIECE ? "goes to the earpiece, for a call"
                                        : route == MEGLER_AUDIO_ROUTE_SPEAKER ? "goes to the loudspeaker, for a call on speaker"
                                                                              : "goes where it always does");
    if (h->route) h->route(h->user, route);
}

/* micRead(short[] buffer, offset, count, blocking): samples heard, into the buffer; how many. */
N(Native_micRead)
{
    mg_jobj *a = c->args[0].l;
    i32 off = c->args[1].i, want = c->args[2].i, got = 0;
    bool blocking = c->args[3].z != 0;
    i16 *out;
    c->ret.i = 0;
    if (!a || off < 0 || want <= 0 || (u32)off + (u32)want > a->u.arr.len) return;
    out = (i16 *)a->u.arr.data + off;
    mic_init();
    mg_lock_acquire(&MIC.lock);
    while (got < want) {
        while (MIC.count > 0 && got < want) {
            out[got++] = MIC.ring[MIC.head];
            MIC.head = (MIC.head + 1) % MIC.cap;
            MIC.count--;
        }
        if (got >= want || !blocking || !MIC.on) break;
        if (!mg_cond_wait_ns(&MIC.more, &MIC.lock, 1000000000ull) && MIC.count == 0) {
            /* Nothing for a second: silence for the rest, as a microphone that hears nothing gives. */
            while (got < want) out[got++] = 0;
        }
    }
    mg_lock_release(&MIC.lock);
    c->ret.i = got;
}

/* cellular(): the SIM as the core has it (mg_cellular_read): "1" or "0" for a SIM ready, the operator's name, MCC and MNC,
   the network's type and "1" or "0" for roaming, separated by tabs. */
N(Native_cellular)
{
    mg_cellular s;
    char *line;
    mg_cellular_read(&s);
    line = mg_sprintf("%d\t%s\t%s\t%d\t%d", s.sim_ready ? 1 : 0, s.operator_name, s.mcc_mnc, s.network_type, s.roaming ? 1 : 0);
    c->ret.l = mg_dvm_new_string(line);
    free(line);
}

N(Native_batteryTechnology)
{
    mg_battery b;
    mg_battery_read(&b);
    c->ret.l = mg_dvm_new_string(b.technology);
}

/*
 * How much of each edge of the surface the phone's bars cover (megler_set_insets), kept apart from J, which a start clears:
 * the shell may say it before the app runs.
 */
static volatile i32 g_bar_insets[4];

N(Native_insets)
{
    /* Left, top, right, bottom of the screen the app must keep clear -- what the phone's bars cover -- and what the keyboard covers of its bottom. */
    mg_jobj *a = mg_dvm_new_array("[I", 5);
    int i;
    if (a) {
        for (i = 0; i < 4; i++) ((i32 *)a->u.arr.data)[i] = g_bar_insets[i];
        ((i32 *)a->u.arr.data)[4] = J.keyboard_height;
    }
    c->ret.l = a;
}

/* systemBars(bars): the bars the app's window wants hidden, and whether its status bar is light (megler.h, megler_system_bars_host). */
N(Native_systemBars)
{
    const megler_system_bars_host *b = &J.config->bars;
    if (b->wanted) b->wanted(b->user, c->args[0].i & (MEGLER_BAR_STATUS | MEGLER_BAR_NAVIGATION | MEGLER_BAR_LIGHT_STATUS));
}

/* ----------------------------------------------------------- the keyboard */

/*
 * showKeyboard(show, inputType, imeOptions): the framework wants the shell's keyboard for the field being edited, or wants
 * it gone. Android's two words about the field are turned into the shell's three (megler.h): what is typed there, more
 * about it, and what Enter does. Called on the app's main thread; the shell's function returns at once.
 */
N(Native_showKeyboard)
{
    const megler_keyboard_host *k = &J.config->keyboard;
    i32 type = c->args[1].i, ime = c->args[2].i;
    int cls = type & 0xF, variation = type & 0xFF0, action = ime & 0xFF, kind, flags = 0, enter;
    if (!c->args[0].z) {
        if (k->hide) k->hide(k->user);
        return;
    }
    if (cls == 2) {
        /* TYPE_CLASS_NUMBER; its variation 0x10 is a PIN. */
        kind = variation == 0x10 ? MEGLER_KEYBOARD_NUMBER_PASSWORD : MEGLER_KEYBOARD_NUMBER;
        if (type & 0x1000) flags |= MEGLER_KEYBOARD_F_SIGNED;
        if (type & 0x2000) flags |= MEGLER_KEYBOARD_F_DECIMAL;
    } else if (cls == 3) {
        kind = MEGLER_KEYBOARD_PHONE;
    } else if (cls == 4) {
        kind = MEGLER_KEYBOARD_DATETIME;
    } else {
        /* TYPE_CLASS_TEXT: passwords (0x80, visible 0x90, web 0xE0), e-mail addresses (0x20, web 0xD0), URIs (0x10). */
        kind = variation == 0x80 || variation == 0x90 || variation == 0xE0 ? MEGLER_KEYBOARD_PASSWORD
               : variation == 0x20 || variation == 0xD0 ? MEGLER_KEYBOARD_EMAIL : variation == 0x10 ? MEGLER_KEYBOARD_URL : MEGLER_KEYBOARD_TEXT;
        if (type & 0x20000) flags |= MEGLER_KEYBOARD_F_MULTILINE;
        if (type & 0x80000) flags |= MEGLER_KEYBOARD_F_NO_SUGGESTIONS;
        if (type & 0x4000) flags |= MEGLER_KEYBOARD_F_CAP_SENTENCES;
        if (type & 0x2000) flags |= MEGLER_KEYBOARD_F_CAP_WORDS;
        if (type & 0x1000) flags |= MEGLER_KEYBOARD_F_CAP_ALL;
    }
    /*
     * What Enter does. IME_FLAG_NO_ENTER_ACTION (0x40000000) is the field saying that Enter is no action there but a new
     * line; a TextView of several lines sets it, on Android and in this framework. Otherwise Enter is the editor's action.
     */
    if (ime & 0x40000000) enter = MEGLER_ENTER_NEWLINE;
    else enter = action == 2 ? MEGLER_ENTER_GO : action == 3 ? MEGLER_ENTER_SEARCH : action == 4 ? MEGLER_ENTER_SEND : action == 5 ? MEGLER_ENTER_NEXT : action == 7 ? MEGLER_ENTER_PREVIOUS : MEGLER_ENTER_DONE;
    mg_atomic_xchg(&J.enter_newline, enter == MEGLER_ENTER_NEWLINE);
    if (k->show) k->show(k->user, kind, flags, enter);
    else mg_note_once("java: the app asked for the keyboard, and the shell gave the layer none to ask (megler_run_config.keyboard)");
}

/* pollText(): the oldest thing the keyboard sent, as a String[1], or null. Asked once for each EV_TEXT event. */
N(Native_pollText)
{
    char *item = NULL;
    mg_jobj *a;
    mg_lock_acquire(&J.lock);
    if (J.ntext) {
        item = J.text[0];
        memmove(&J.text[0], &J.text[1], sizeof(char *) * (size_t)--J.ntext);
    }
    mg_lock_release(&J.lock);
    c->ret.l = NULL;
    if (!item) return;
    a = mg_dvm_new_array("[Ljava/lang/String;", 1);
    if (a) a->u.oarr.v[0] = mg_dvm_new_string(item);
    free(item);
    c->ret.l = a;
}

/* pollPhone(): the oldest of the phone's messages and calls the app has not taken (J.phone), as a String[1], or null. */
N(Native_pollPhone)
{
    char *item = NULL;
    mg_jobj *a;
    mg_lock_acquire(&J.lock);
    if (J.nphone) {
        item = J.phone[0];
        memmove(&J.phone[0], &J.phone[1], sizeof(char *) * (size_t)--J.nphone);
    }
    mg_lock_release(&J.lock);
    c->ret.l = NULL;
    if (!item) return;
    a = mg_dvm_new_array("[Ljava/lang/String;", 1);
    if (a) a->u.oarr.v[0] = mg_dvm_new_string(item);
    free(item);
    c->ret.l = a;
}

/* mediaSessionState(state, actions): the app's active media session's PlaybackState (megler.h, megler_media_session_host). */
N(Native_mediaSessionState)
{
    const megler_media_session_host *m = &J.config->media_session;
    if (m->state) m->state(m->user, (int)c->args[0].i, (long long)c->args[1].j);
}

/* mediaSessionMetadata(title, artist, album, art, artWidth, artHeight): what that session says it plays; art is ARGB, or null. */
N(Native_mediaSessionMetadata)
{
    const megler_media_session_host *m = &J.config->media_session;
    mg_jobj *art = c->args[3].l;
    i32 w = c->args[4].i, h = c->args[5].i;
    const uint32_t *px = NULL;
    if (!m->metadata) return;
    if (art && art->kind == MG_J_PRIM_ARRAY && art->u.arr.etype == 'I' && w > 0 && h > 0 && w <= 256 && h <= 256 &&
        art->u.arr.len >= (u32)(w * h))
        px = (const uint32_t *)art->u.arr.data;
    m->metadata(m->user, c->args[0].l ? text_of(c->args[0].l) : NULL, c->args[1].l ? text_of(c->args[1].l) : NULL,
                c->args[2].l ? text_of(c->args[2].l) : NULL, px, px ? w : 0, px ? h : 0);
}

/* nightMode(), accentColor(): the phone's look as the shell told it (megler_set_theme). paletteTone(seed, palette, tone). */
N(Native_nightMode) { c->ret.z = g_theme_night != 0; }
N(Native_accentColor) { c->ret.i = g_theme_accent; }
N(Native_paletteTone) { c->ret.i = (i32)megler_palette_tone((uint32_t)c->args[0].i, (int)c->args[1].i, (int)c->args[2].i); }

/* share(text), openUrl(uri): to the shell (megler_system_host); said in the log either way. */
N(Native_share)
{
    const megler_system_host *s = &J.config->system;
    mg_info("java: the app shares \"%s\"%s", text_of(c->args[0].l), s->share_text ? "" : " (nothing here to share it with)");
    if (s->share_text) s->share_text(s->user, text_of(c->args[0].l), NULL);
}
N(Native_openUrl)
{
    const megler_system_host *s = &J.config->system;
    mg_info("java: the app opens %s%s", text_of(c->args[0].l), s->open_url ? "" : " (nothing here to open it)");
    if (s->open_url) s->open_url(s->user, text_of(c->args[0].l));
}

/* installApk(path, name): an APK (or a folder of one app's APKs) the app wants installed, by its path in the app's files; true when the shell took it. */
N(Native_installApk)
{
    const megler_system_host *s = &J.config->system;
    const char *guest = text_of(c->args[0].l), *name = text_of(c->args[1].l);
    char *host = mg_fs_host_path(guest);
    c->ret.z = false;
    mg_info("java: the app asks to install %s (%s)%s", name, host ? host : guest, s->install_apk ? "" : ": nothing here to install it with");
    if (host && s->install_apk) c->ret.z = s->install_apk(s->user, host, name);
    free(host);
}

/* launchApp(package, activity): another app the phone has, started; true when the shell started it. */
N(Native_launchApp)
{
    const megler_system_host *s = &J.config->system;
    const char *package = text_of(c->args[0].l);
    const char *activity = c->args[1].l ? text_of(c->args[1].l) : NULL;
    mg_info("java: the app starts %s%s%s%s", package, activity ? "/" : "", activity ? activity : "", s->launch_app ? "" : " (nothing here to start it)");
    c->ret.z = s->launch_app ? s->launch_app(s->user, package, activity && activity[0] ? activity : NULL) : false;
}

/* uninstallApp(package): another app the phone has, to uninstall; true when the shell asks the user (megler_system_host). */
N(Native_uninstallApp)
{
    const megler_system_host *s = &J.config->system;
    const char *package = text_of(c->args[0].l);
    mg_info("java: the app asks to uninstall %s%s", package, s->uninstall_app ? "" : " (nothing here to uninstall it with)");
    c->ret.z = s->uninstall_app ? s->uninstall_app(s->user, package) : false;
}

/* appsList(): apps.txt of the apps the phone has (megler_run_config.apps_dir), or null when there is none. */
N(Native_appsList)
{
    char *path, *text;
    size_t size = 0;
    c->ret.l = NULL;
    if (!J.config->apps_dir) return;
    path = mg_sprintf("%s/apps.txt", J.config->apps_dir);
    text = (char *)mg_file_read_all(path, &size);
    free(path);
    if (!text) return;
    text = (char *)realloc(text, size + 1);
    if (!text) return;
    text[size] = 0;
    c->ret.l = mg_dvm_new_string(text);
    free(text);
}

/* An Android package's name, which has letters, digits, '.' and '_' only: nothing that leaves the apps folder. */
static bool apps_package_ok(const char *package)
{
    const char *p;
    if (!J.config->apps_dir || !package[0] || package[0] == '.') return false;
    for (p = package; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_')) return false;
    return true;
}

/* A file of the apps folder, `path` (freed here), as bytes; null when there is none. */
static mg_jobj *apps_file(char *path)
{
    size_t size = 0;
    u8 *bytes = mg_file_read_all(path, &size);
    mg_jobj *o = bytes ? bytes_of(bytes, size) : NULL;
    free(bytes);
    free(path);
    return o;
}

/* appIcon(package): that app's icon, a PNG, from the same folder; null when there is none. */
N(Native_appIcon)
{
    const char *package = text_of(c->args[0].l);
    c->ret.l = apps_package_ok(package) ? apps_file(mg_sprintf("%s/%s.png", J.config->apps_dir, package)) : NULL;
}

/* appActivities(package): that app's activities as the shell described them (megler_apk_describe_activities), the bytes of
   <package>/activities.json in the same folder; null when there are none. */
N(Native_appActivities)
{
    const char *package = text_of(c->args[0].l);
    c->ret.l = apps_package_ok(package) ? apps_file(mg_sprintf("%s/%s/activities.json", J.config->apps_dir, package)) : NULL;
}

/* appResIcon(package, id): the picture of that app's icon resource `id`, a PNG, from <package>/ in the same folder; null when
   there is none. */
N(Native_appResIcon)
{
    const char *package = text_of(c->args[0].l);
    c->ret.l = apps_package_ok(package) ? apps_file(mg_sprintf("%s/%s/%08x.png", J.config->apps_dir, package, (unsigned)c->args[1].i)) : NULL;
}

/* appInstall(name): what Megler last wrote of the APK the app handed it under `name` (megler.Installs: <package>.apk or
   <package>.apks), the bytes of installs/<name>.txt in the same folder; null when it has written nothing. */
N(Native_appInstall)
{
    const char *name = text_of(c->args[0].l);
    c->ret.l = apps_package_ok(name) ? apps_file(mg_sprintf("%s/installs/%s.txt", J.config->apps_dir, name)) : NULL;
}
N(Native_setOrientation) { mg_note_once("java: the app asked for orientation %d; the surface stays as the shell made it", (int)c->args[0].i); }

/*
 * exit(): the framework calls it when the last activity is gone. The main
 * loop is told to end, and megler.AppRunner.run returns to main_thread below.
 */
/*
 * thrownInService(Throwable t), asked on the thread that threw it (an uncaught exception's handler runs there): the class scope
 * of the service the thread is one of (megler.ServiceClassLoader: Google Play services' code hosted here), else 0. The thread
 * is the service's when the outermost frame of code of an app's own or of a service's -- not of the boot class path's, which
 * every thread starts in -- is the service's: a worker of a pool GmsCore made runs GmsCore's task under the platform's frames.
 * Once the exception's frames have been made StackTraceElements (a handler before printed them), libcore keeps no backtrace:
 * then whose code started the thread says (mg_dvm_thread.owner_scope). Its uncaught exceptions are its process's on Android.
 */
N(Native_thrownInService)
{
    mg_jobj *e = c->args[0].l, *trace;
    const i64 *v;
    i32 n, i;
    c->ret.i = mg_dvm_self()->owner_scope;
    if (!e || !g_dvm.f_backtrace || !mg_dvm_is_instance(e, g_dvm.c_throwable)) return;
    trace = mg_dvm_slots(e)[g_dvm.f_backtrace->slot].l;
    if (!trace || trace->kind != MG_J_PRIM_ARRAY || trace->u.arr.etype != 'J') return;
    v = (const i64 *)trace->u.arr.data;
    n = (i32)(trace->u.arr.len / 2);
    for (i = n - 1; i >= 0; i--) {
        const mg_dvm_method *m = (const mg_dvm_method *)(mg_addr)v[2 * i];
        if (!m || !m->cls) continue;
        if (m->cls->scope > 0) { c->ret.i = mg_dvm_scope_root(m->cls->scope); return; }
        if (mg_dvm_class_is_app(m->cls)) { c->ret.i = 0; return; }
    }
}

N(Native_exit)
{
    mg_jvalue looper;
    (void)c;
    mg_atomic_xchg(&J.exited, 1);
    looper.j = 0;
    if (mg_dvm_call_static("android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", NULL, &looper) && looper.l) mg_dvm_call_virtual(looper.l, "quit", "()V", NULL, NULL);
}

static const mg_jhle k_native[] = {
    { "megler/Native", "log", NULL, Native_log },
    { "megler/Native", "thrownInService", NULL, Native_thrownInService },
    { "megler/Native", "readAsset", NULL, Native_readAsset },
    { "megler/Native", "listAssets", NULL, Native_listAssets },
    { "megler/Native", "assetFd", NULL, Native_assetFd },
    { "megler/Native", "assetDeflated", NULL, Native_assetDeflated },
    { "megler/Native", "assetLength", NULL, Native_assetLength },
    { "megler/Native", "readApkFile", NULL, Native_readApkFile },
    { "megler/Native", "readApkResourceTable", NULL, Native_readApkResourceTable },
    { "megler/Native", "addApk", NULL, Native_addApk },
    { "megler/Native", "listApkFiles", NULL, Native_listApkFiles },
    { "megler/Native", "apkFileFd", NULL, Native_apkFileFd },
    { "megler/Native", "apkFileLength", NULL, Native_apkFileLength },
    { "megler/Native", "apkPath", NULL, Native_apkPath },
    { "megler/Native", "splitPaths", NULL, Native_splitPaths },
    { "megler/Native", "setAppClassLoader", NULL, Native_setAppClassLoader },
    { "megler/Native", "nativeLooperPrepare", NULL, Native_looperPrepare },
    { "megler/Native", "nativeLooperPoll", NULL, Native_looperPoll },
    { "megler/Native", "dataDir", NULL, Native_dataDir },
    { "megler/Native", "externalDir", NULL, Native_externalDir },
    { "megler/Native", "battery", NULL, Native_battery },
    { "megler/Native", "batteryTechnology", NULL, Native_batteryTechnology },
    { "megler/Native", "cellular", NULL, Native_cellular },
    { "megler/Native", "micStart", NULL, Native_micStart },
    { "megler/Native", "micStop", NULL, Native_micStop },
    { "megler/Native", "micRead", NULL, Native_micRead },
    { "megler/Native", "audioRoute", NULL, Native_audioRoute },
    { "megler/Native", "packageName", NULL, Native_packageName },
    { "megler/Native", "screenWidth", NULL, Native_screenWidth },
    { "megler/Native", "screenHeight", NULL, Native_screenHeight },
    { "megler/Native", "density", NULL, Native_density },
    { "megler/Native", "setContentView", NULL, Native_nothing },
    { "megler/Native", "pollInput", NULL, Native_pollInput },
    { "megler/Native", "pollText", NULL, Native_pollText },
    { "megler/Native", "pollPhone", NULL, Native_pollPhone },
    { "megler/Native", "showKeyboard", NULL, Native_showKeyboard },
    { "megler/Native", "systemBars", NULL, Native_systemBars },
    { "megler/Native", "mediaSessionState", NULL, Native_mediaSessionState },
    { "megler/Native", "installApk", NULL, Native_installApk },
    { "megler/Native", "launchApp", NULL, Native_launchApp },
    { "megler/Native", "uninstallApp", NULL, Native_uninstallApp },
    { "megler/Native", "appsList", NULL, Native_appsList },
    { "megler/Native", "appIcon", NULL, Native_appIcon },
    { "megler/Native", "appActivities", NULL, Native_appActivities },
    { "megler/Native", "appResIcon", NULL, Native_appResIcon },
    { "megler/Native", "appInstall", NULL, Native_appInstall },
    { "megler/Native", "mediaSessionMetadata", NULL, Native_mediaSessionMetadata },
    { "megler/Native", "setClipboard", NULL, Native_setClipboard },
    { "megler/Native", "getClipboard", NULL, Native_getClipboard },
    { "megler/Native", "share", NULL, Native_share },
    { "megler/Native", "setOrientation", NULL, Native_setOrientation },
    { "megler/Native", "insets", NULL, Native_insets },
    { "megler/Native", "nightMode", NULL, Native_nightMode },
    { "megler/Native", "accentColor", NULL, Native_accentColor },
    { "megler/Native", "paletteTone", NULL, Native_paletteTone },
    { "megler/Native", "dnsServers", NULL, Native_dnsServers },
    { "megler/Native", "vibrate", NULL, Native_nothing },
    { "megler/Native", "openUrl", NULL, Native_openUrl },
    { "megler/Native", "exit", NULL, Native_exit },
    { "megler/Native", "uptimeNanos", NULL, Native_uptimeNanos },
    { "megler/Native", "frameMark", NULL, Native_frameMark },
    { "megler/Native", "presentRect", NULL, Native_presentRect },
    { "megler/Native", "checkPartial", NULL, Native_checkPartial },
    { "megler/Native", "flag", NULL, Native_flag },
    { "megler/Native", "askClearData", NULL, Native_askClearData },
    { NULL, NULL, NULL, NULL },
};

/* ------------------------------------------- what is not answered yet */

/*
 * The framework's other native classes: web views and sensors. Neither is
 * written here yet. Each of their methods answers zero, false or null, said
 * once in the log for the class, and the framework's own Java makes of that
 * what an app can live with: a sensor is not available, a web view an empty
 * rectangle. (Databases, megler.Sqlite, are mg_java_sqlite.c; sound, megler.Audio,
 * mg_java_audio.c; the decoders behind MediaCodec, megler.MediaCodecNative,
 * mg_java_media.c; OpenGL ES and EGL, mg_java_gl.c and mg_java_gles.c, whose
 * tables are registered after this one: the entries here for their classes are
 * only for a native the framework might declare and they do not bind.)
 */
N(Absent)
{
    /* (The native method's own frame is the top one: its class is what is missing.) The callers say who wanted it. */
    static const mg_dvm_class *said[16];
    static volatile i32 nsaid;
    const mg_dvm_frame *f = mg_dvm_self()->top, *up;
    const mg_dvm_class *cls = f && f->m ? f->m->cls : NULL;
    char who[500];
    size_t k = 0;
    int n, i;
    c->ret.j = 0;
    /* Once for each class, whoever asks. (Two threads asking at the same moment may each say it.) */
    for (i = 0; i < nsaid && i < (int)MG_COUNT(said); i++)
        if (said[i] == cls) return;
    i = mg_atomic_add(&nsaid, 1) - 1;
    if (i < (int)MG_COUNT(said)) said[i] = cls;
    who[0] = 0;
    for (up = f ? f->prev : NULL, n = 0; up && n < 5 && k + 120 < sizeof(who); up = up->prev, n++)
        k += (size_t)snprintf(who + k, sizeof(who) - k, "%s%s.%s", n ? " < " : "", up->m->cls->name, up->m->name);
    mg_warn("java: %s is not there yet: its native methods answer nothing (first asked by %s)", cls ? cls->name : "a class of the framework", who[0] ? who : "the layer");
}
/*
 * Sensors.available(type): no, and that is no gap to report. The framework's SensorManager asks it for every kind of sensor
 * when the class is first used, which is at every app's start (ContextImpl's table of system services names the class).
 * "There is no such sensor" is the true answer until there are sensors; an app that registers a listener still meets the
 * line above.
 */
N(Sensors_available) { c->ret.j = 0; }
static const mg_jhle k_absent[] = {
    { "megler/Sensors", "available", NULL, Sensors_available },
    { "megler/Web", NULL, NULL, Absent },
    { "megler/Sensors", NULL, NULL, Absent },
    { "megler/EGLNative", NULL, NULL, Absent },
    { "android/opengl/GLES10", NULL, NULL, Absent },
    { "android/opengl/GLES10Ext", NULL, NULL, Absent },
    { "android/opengl/GLES11", NULL, NULL, Absent },
    { "android/opengl/GLES11Ext", NULL, NULL, Absent },
    { "android/opengl/GLES20", NULL, NULL, Absent },
    { "android/opengl/GLES30", NULL, NULL, Absent },
    { "android/opengl/GLES31", NULL, NULL, Absent },
    { "android/opengl/GLES32", NULL, NULL, Absent },
    { NULL, NULL, NULL, NULL },
};

/* ------------------------------------------------------------ the screen */

/* The time each thread has spent in present: the frame meter's. */
static MG_THREAD_LOCAL u64 t_present_ns;
/* Native.presentRect: what the next present on this thread changed, left, top, right, bottom; right 0: not said, all of it. */
static MG_THREAD_LOCAL i32 t_part[4];

N(Native_presentRect)
{
    int i;
    for (i = 0; i < 4; i++) t_part[i] = c->args[i].i;
}

/*
 * A picture of the whole window to the shell, of which `part` (left, top, right, bottom) changed; part[2] 0: all of it.
 * The views' (present, below) and, once the app draws with EGL on a view, the window composed of the views and the GL
 * frames (mg_java_gl.c), on the main thread and on the threads that swap, one call at a time.
 */
static void show(const void *pixels, int width, int height, int stride, const i32 part[4])
{
    if (part[2] > part[0] && part[3] > part[1] && J.config->window.present_pixels_part)
        J.config->window.present_pixels_part(J.config->window.user, pixels, width, height, stride, part[0], part[1], part[2] - part[0], part[3] - part[1]);
    else if (J.config->window.present_pixels)
        J.config->window.present_pixels(J.config->window.user, pixels, width, height, stride);
}

/* Gfx.present: the window's views, drawn. The shell is handed the picture; a frame is counted either way, so that a run with no shell can be followed. */
static void present(void *user, const void *pixels, int width, int height, int stride)
{
    u64 started;
    (void)user;
    if (!pixels) return;
    started = mg_time_monotonic_ns();
    if (!mg_java_gl_views(pixels, width, height, stride, t_part)) show(pixels, width, height, stride, t_part);
    t_part[0] = t_part[1] = t_part[2] = t_part[3] = 0;
    mg_run_count_frame();
    mg_atomic_add(&J.stats.frames, 1);
    t_present_ns += mg_time_monotonic_ns() - started;
}

/*
 * Gfx.present, the window's views, shown by the shell from its own picture of the canvas that draws them, without the memory
 * (megler_gfx_host.present): what mg_java_gfx.c tries before it brings the memory up to date. Not while OpenGL frames are
 * composed under the views, which needs the views' memory; and false when the shell cannot, the memory's way then following.
 */
static bool present_canvas(void *user, void *canvas, int width, int height)
{
    u64 started;
    bool shown;
    (void)user;
    if (!J.config->gfx.present || mg_java_gl_composing()) return false;
    started = mg_time_monotonic_ns();
    mg_java_gl_views_shown();
    if (t_part[2] > t_part[0] && t_part[3] > t_part[1])
        shown = J.config->gfx.present(J.config->gfx.user, canvas, t_part[0], t_part[1], t_part[2] - t_part[0], t_part[3] - t_part[1]);
    else
        shown = J.config->gfx.present(J.config->gfx.user, canvas, 0, 0, width, height);
    if (!shown) return false;
    t_part[0] = t_part[1] = t_part[2] = t_part[3] = 0;
    mg_run_count_frame();
    mg_atomic_add(&J.stats.frames, 1);
    t_present_ns += mg_time_monotonic_ns() - started;
    return true;
}

void mg_java_get_stats(mg_java_stats *out) { *out = J.stats; }

static bool g_check_partial;
void mg_java_check_partial(bool on) { g_check_partial = on; }
N(Native_checkPartial) { c->ret.z = g_check_partial; }

/* ------------------------------------------------------- the frame meter */

/*
 * Where the main thread's time goes while the app draws, from the core's side and in summed counters: nothing is logged per
 * call or per frame. megler.ViewRoot.doFrame (overlaid for this) calls Native.frameMark(1) as a frame begins with its measure
 * and layout, frameMark(2) as its draw traversal begins and frameMark(3, pixels drawn) as it ends. At each mark the main
 * thread's clock, its time in the shell's drawing (mg_java_gfx_host_ns), in handing pictures over (present, above), in
 * Object.wait and Thread.sleep, in native methods, allocating and waiting for monitors, and its instructions
 * (mg_dvm_thread_costs) are read, and what passed since the mark before
 * goes to the part of the frame it was spent in: between frames (input, messages, and waiting for them), measure and
 * layout, the draw traversal. A line every five seconds while the app draws; at its end the last of them and one for the
 * whole run. docs/JAVA.md ("The frame meter") says what each figure is.
 */
enum { PART_BETWEEN, PART_LAYOUT, PART_DRAW, PARTS };

typedef struct {
    u64 wall, host, present, waited, executed, native, natives, alloc, allocs, blocked;
} meter_sum;

typedef struct {
    u64 started, all_executed, object_bytes; /* at the beginning: the clock, every thread's instructions, the objects made */
    u64 collections, collecting_ns;          /* and the collector's count and time (mg_dvm_gc_totals) */
    u32 frames, drawn;
    u64 longest, redrawn;                    /* the longest frame from mark 1 to mark 3; pixels drawn */
    meter_sum part[PARTS];
} meter_window;

static struct {
    const mg_dvm_thread *thread;             /* the main thread, at its first mark; NULL: no frame yet */
    meter_sum last;                          /* the readings at the last mark (wall: the clock) */
    int part;                                /* where the time since the last mark goes */
    u64 frame_started;
    meter_window window, run;
} M;

#define METER_EVERY_NS 5000000000ull

static void meter_read(meter_sum *r)
{
    r->wall = mg_time_monotonic_ns();
    r->host = mg_java_gfx_host_ns();
    mg_dvm_costs c;
    r->present = t_present_ns;
    mg_dvm_thread_costs(&c);
    r->executed = c.executed;
    r->waited = c.waited_ns;
    r->native = c.native_ns;
    r->natives = c.natives;
    r->alloc = c.alloc_ns;
    r->allocs = c.allocs;
    r->blocked = c.blocked_ns;
}

static void meter_start(meter_window *w, u64 now)
{
    mg_dvm_stats s;
    memset(w, 0, sizeof(*w));
    mg_dvm_get_stats(&s);
    w->started = now;
    w->all_executed = mg_dvm_executed_all();
    w->object_bytes = s.object_bytes;
    mg_dvm_gc_totals(&w->collections, &w->collecting_ns);
}

/* What has passed since the last mark, to the part it was spent in, in the window and in the run. */
static void meter_advance(const meter_sum *now)
{
    meter_window *w[2];
    int i;
    w[0] = &M.window;
    w[1] = &M.run;
    for (i = 0; i < 2; i++) {
        meter_sum *p = &w[i]->part[M.part];
        p->wall += now->wall - M.last.wall;
        p->host += now->host - M.last.host;
        p->present += now->present - M.last.present;
        p->waited += now->waited - M.last.waited;
        p->executed += now->executed - M.last.executed;
        p->native += now->native - M.last.native;
        p->natives += now->natives - M.last.natives;
        p->alloc += now->alloc - M.last.alloc;
        p->allocs += now->allocs - M.last.allocs;
        p->blocked += now->blocked - M.last.blocked;
    }
    M.last = *now;
}

static double per_frame_ms(u64 ns, u32 frames) { return frames ? (double)ns / 1e6 / frames : 0; }

/*
 * The memory's lines: what the process holds and what holds its addresses, and the JIT's work. They walk the whole address
 * space (VirtualQuery) and every thread: some 300 ms of the main thread every five seconds when the frames line said them,
 * the frame meter (Native.frameMark) 6% of Yandex Maps' main thread (package 530). Said every ten seconds by the sampler's
 * thread (hot_main), and once at the run's end.
 */
static void meter_census(void)
{
    u64 process = mg_mem_usage();
    mg_dvm_stats s;
    char mem[480];
    mg_dvm_get_stats(&s);
    mem[0] = 0;
    if (process) {
        /* And the parts of it the core can name: the Java heap's memory, the DEX files, what madvise has given back. */
        mg_dvm_heap_info hi;
        u64 dex_read = 0, dex_viewed = 0, given = 0, again = 0, anon = 0, files = 0;
        mg_dvm_heap_stats(&hi);
        mg_dvm_dex_memory(&dex_read, &dex_viewed);
        mg_mmap_given_back(&given, &again);
        mg_mmap_usage(&anon, &files);
        snprintf(mem, sizeof(mem), "the process %u MB (the heap's blocks %u MB and large objects %u MB; DEX files %u MB in memory, %u MB viewed "
                 "in their APKs; native libraries %u MB; the app's malloc %u MB; the app's mappings %u MB anonymous and %u MB of files; %u MB "
                 "given back with madvise, %u MB of it touched again)", (unsigned)(process >> 20), (unsigned)(hi.reserved >> 20),
                 (unsigned)(hi.large_bytes >> 20), (unsigned)(dex_read >> 20), (unsigned)(dex_viewed >> 20), (unsigned)(mg_ld_image_bytes() >> 20),
                 (unsigned)(mg_g_malloc_bytes() >> 20), (unsigned)(anon >> 20), (unsigned)(files >> 20), (unsigned)(given >> 20),
                 (unsigned)(again >> 20));
        /*
         * And the addresses: a 32-bit process has 2 GB of them, and what the next allocation needs is a stretch of them free,
         * whoever makes it. Yandex Maps ran out of them with 1.2 GB of its 2 GB of memory used, and the shell's own drawing
         * failed first (package 489); its memory line had not said so.
         */
        {
            u32 free_mb = 0, largest = 0, reserved = 0, committed = 0;
            size_t used = strlen(mem);
            if (mg_host_address_space(&free_mb, &largest, &reserved, &committed))
                snprintf(mem + used, sizeof(mem) - used, "; addresses %u MB free (the largest stretch %u), %u reserved, %u committed", (unsigned)free_mb,
                         (unsigned)largest, (unsigned)reserved, (unsigned)committed);
        }
    }
    /* Its own line, which the frames line had no room left for. */
    if (mem[0]) mg_info("java: memory: %s", mem);
    /*
     * And what holds the addresses, by kind (mg_host_address_census): of the private memory, in allocations of which sizes
     * most of it is; the threads (each with its stack, 1 MB of addresses, and the runtime's with a frame stack besides); what
     * the runtime's tables and classes take of the heap; and the graphics processor's own memory as DXGI says it. A host
     * stack is committed whole for Android code (mg_thread_whole_stack), so it is memory, not only addresses. Where the
     * memory line's parts do not add up, this says where the rest is.
     */
    if (process) {
        mg_host_address_kinds k;
        u32 gpu_used = 0, gpu_budget = 0, threads = 0, attached = 0;
        u64 frames = 0, c_reserved = 0, c_peak = 0;
        char sizes[160];
        int i;
        if (mg_host_address_census(&k)) {
            sizes[0] = 0;
            for (i = 0; i < k.nsizes; i++) {
                size_t used = strlen(sizes);
                snprintf(sizes + used, sizeof(sizes) - used, "%s%u of %u KB", i ? ", " : "", (unsigned)k.sizes[i].count, (unsigned)k.sizes[i].kb);
            }
            mg_egl_gpu_memory(&gpu_used, &gpu_budget);
            mg_dvm_thread_census(&threads, &attached, &frames, &c_reserved, &c_peak);
            mg_info("java: addresses by kind: the program and its DLLs %u MB, views of files and shared memory %u MB, private memory %u MB "
                    "committed and %u reserved, the most of it in allocations of %s; threads the core started %u, the runtime's %u (%u of "
                    "them attached by native code), their frame stacks %u MB and their host stacks %u MB (the deepest used %u KB); the "
                    "runtime's tables and classes %u MB (classes %u, methods %u, fields %u, vtables %u, the DEX files' tables %u, names %u, those of "
                    "files not viewed yet %u); "
                    "the graphics processor's memory %u MB of %u",
                    (unsigned)k.images_mb, (unsigned)k.views_mb, (unsigned)k.private_mb, (unsigned)k.private_reserved_mb, sizes[0] ? sizes : "none",
                    (unsigned)mg_thread_live_count(), (unsigned)threads, (unsigned)attached, (unsigned)(frames >> 20), (unsigned)(c_reserved >> 20),
                    (unsigned)(c_peak >> 10), (unsigned)(s.meta_bytes >> 20), (unsigned)(s.meta_classes >> 20), (unsigned)(s.meta_methods >> 20),
                    (unsigned)(s.meta_fields >> 20), (unsigned)(s.meta_vtables >> 20), (unsigned)(s.meta_dex >> 20), (unsigned)(s.meta_names >> 20),
                    (unsigned)(s.meta_pools >> 20), (unsigned)gpu_used, (unsigned)gpu_budget);
            {
                /* And the reserved, by whose it is, where the core can tell: what is left is the stacks and the DLLs' own. */
                u64 frames_reserved = 0, frames_committed = 0, deepest = 0, spanned = 0, mapped_reserved = 0, libs_reserved = mg_ld_reserved_bytes(), known;
                mg_dvm_frame_census(&frames_reserved, &frames_committed, &deepest);
                mg_g_mapped_addresses(&spanned, &mapped_reserved);
                known = (frames_reserved - frames_committed) + mapped_reserved + libs_reserved;
                mg_info("java: of the private memory reserved and not committed: the frame stacks' %u MB (of the %u MB their segments reserve; "
                        "the deepest thread's frames %u KB now), the app's mappings' %u MB (of the %u MB they span), its libraries' %u MB, "
                        "the rest %u MB (thread stacks, the shell's and the system's DLLs')",
                        (unsigned)((frames_reserved - frames_committed) >> 20), (unsigned)(frames_reserved >> 20), (unsigned)(deepest >> 10),
                        (unsigned)(mapped_reserved >> 20), (unsigned)(spanned >> 20), (unsigned)(libs_reserved >> 20),
                        (unsigned)(((u64)k.private_reserved_mb << 20) > known ? (((u64)k.private_reserved_mb << 20) - known) >> 20 : 0));
            }
        }
    }
    {
        /* The JIT's work (mg_dvm_jit.c): how much of the app runs compiled, and what its code takes. */
        mg_dvm_jit_stats j;
        mg_dvm_jit_census(&j);
        if (j.methods || j.refused)
            mg_info("java: the JIT: %u methods compiled (%u of them waiting for their page to be made executable), %u not worth it; "
                    "%u KB of code on %u KB of pages",
                    (unsigned)j.methods, (unsigned)j.waiting, (unsigned)j.refused, (unsigned)(j.code_bytes >> 10), (unsigned)(j.page_bytes >> 10));
    }
}

static void meter_line(const char *what, const meter_window *w, u64 now)
{
    mg_dvm_stats s;
    u64 busy[PARTS], java = 0, host = 0, present = 0, waited = 0, executed = 0, elapsed = now - w->started, screen;
    u64 native = 0, natives = 0, alloc = 0, allocs = 0, blocked = 0, wall = 0, other, interpreted, live_bytes;
    u64 bitmap_bytes = 0, list_bytes = 0, collections = 0, collecting_ns = 0;
    u32 bitmaps = 0, nodes = 0, n = w->frames;
    char of[40], redrawn[60], rates[300], gc[120];
    int i;
    for (i = 0; i < PARTS; i++) {
        const meter_sum *p = &w->part[i];
        /* The main thread's time in this part less its waiting and its presenting: Java, the shell's drawing included. */
        busy[i] = p->wall > p->waited + p->present ? p->wall - p->waited - p->present : 0;
        java += busy[i];
        host += p->host;
        present += p->present;
        waited += p->waited;
        executed += p->executed;
        native += p->native;
        natives += p->natives;
        alloc += p->alloc;
        allocs += p->allocs;
        blocked += p->blocked;
        wall += p->wall;
    }
    mg_dvm_get_stats(&s);
    live_bytes = mg_dvm_live_bytes();
    mg_java_gfx_stats(&bitmap_bytes, &bitmaps);
    /* And what RenderNodes and Pictures take: their display lists, which are the core's memory as bitmaps are. */
    mg_java_gfx_list_stats(&list_bytes, NULL, &nodes);
    screen = (u64)(J.config->width > 0 ? J.config->width : 0) * (u64)(J.config->height > 0 ? J.config->height : 0);
    of[0] = 0;
    if (w->drawn != n) snprintf(of, sizeof(of), " of %u frames", (unsigned)n);
    if (w->drawn && screen)
        snprintf(redrawn, sizeof(redrawn), "redrawn %u%% of the screen a frame", (unsigned)(w->redrawn * 100 / (screen * w->drawn)));
    else
        mg_strlcpy(redrawn, "nothing drawn", sizeof(redrawn));
    /*
     * Where Java's time went besides the shell's drawing. The shell's drawing, present and the waits all happen inside native
     * methods, so the other natives are what is left of the natives' time without them. Bytecode is the rest: the time
     * outside native methods, allocation and waits for monitors -- interpreted and compiled (mg_dvm_jit.c), calls and returns,
     * resolution, the interpreter's own work. The instructions counted are the interpreter's alone (compiled code counts
     * none), so their pace over that time says how much of it was interpreted. And what every thread's interpreter executed.
     */
    other = native > host + present + waited ? native - host - present - waited : 0;
    interpreted = wall > native + alloc + blocked ? wall - native - alloc - blocked : 0;
    snprintf(rates, sizeof(rates), "in other native methods %.1f (%.0f calls), allocating %.1f (%.0f objects), waiting for monitors %.1f, "
             "bytecode %.1f, interpreted and compiled (%.1f M instructions a second interpreted); %.1f M instructions interpreted on all threads",
             per_frame_ms(other, n), n ? (double)natives / n : 0.0, per_frame_ms(alloc, n), n ? (double)allocs / n : 0.0, per_frame_ms(blocked, n),
             per_frame_ms(interpreted, n), interpreted ? (double)executed * 1e3 / (double)interpreted : 0.0,
             (double)(mg_dvm_executed_all() - w->all_executed) / 1e6);
    /* The collector, as its own part: what it took in the time the line covers, every thread of the runtime's stopped for most of it. */
    mg_dvm_gc_totals(&collections, &collecting_ns);
    snprintf(gc, sizeof(gc), "collecting %.1f ms in %u collections", (double)(collecting_ns - w->collecting_ns) / 1e6, (unsigned)(collections - w->collections));
    mg_info("java: frames %s%.1f s: %u drawn%s, %.1f ms of work each on average (the longest frame %.1f without input and messages): Java %.1f "
            "(input and messages %.1f, measure and layout %.1f, "
            "draw traversal %.1f), of it in the shell's drawing %.1f, %s; present %.1f; idle %u%%; %s; heap %u MB (objects %u live, %u "
            "made in these %.1f s; bitmaps %u; display lists %u KB, %u nodes); %s",
            what, (double)elapsed / 1e9, (unsigned)w->drawn, of, per_frame_ms(java + present, n), per_frame_ms(w->longest, 1), per_frame_ms(java, n),
            per_frame_ms(busy[PART_BETWEEN], n), per_frame_ms(busy[PART_LAYOUT], n), per_frame_ms(busy[PART_DRAW], n), per_frame_ms(host, n), rates,
            per_frame_ms(present, n), (unsigned)(elapsed ? waited * 100 / elapsed : 0), gc,
            (unsigned)((live_bytes + bitmap_bytes) >> 20), (unsigned)(live_bytes >> 20), (unsigned)((s.object_bytes - w->object_bytes) >> 20),
            (double)elapsed / 1e9, (unsigned)(bitmap_bytes >> 20), (unsigned)(list_bytes >> 10), (unsigned)nodes, redrawn);
    /* The memory's lines, at the end of the run here; meanwhile the sampler's thread says them (hot_main), off the main thread. */
    if (what[0]) meter_census();
}

/*
 * Where the main thread's time goes: every 5 ms the method it runs (mg_dvm_thread.running, which it writes itself: always
 * a method or nothing), counted, and every 10 s the ones counted most said, with their share of the samples -- a native
 * one marked, the looper's wait among them. Yandex Maps' main thread took 15 s after its first picture to draw much else,
 * its time bytecode and 100,000 native calls, and nothing said whose.
 */
#define HOT_SLOTS 4096
static struct {
    const mg_dvm_method *m;
    u32 n;
} g_hot[HOT_SLOTS];

/*
 * `collecting`: the samples taken while a collection was under way, whatever the main thread's method; the thread is stopped
 * then, or waits for the heap to make an object. Each was the method that made an object when the heap was due, 9% of Yandex
 * Maps' main thread "in" String.toCharArray (package 531).
 */
static void hot_say(u32 samples, u32 outside, u32 collecting)
{
    char line[1400];
    size_t at;
    int k;
    at = (size_t)snprintf(line, sizeof(line), "hot: the main thread, %u samples in 10 s: outside Java %u%%, the collector %u%%", (unsigned)samples,
                          (unsigned)(samples ? outside * 100 / samples : 0), (unsigned)(samples ? collecting * 100 / samples : 0));
    for (k = 0; k < 14 && at + 160 < sizeof(line); k++) {
        u32 best = 0, i, at_best = 0;
        for (i = 0; i < HOT_SLOTS; i++)
            if (g_hot[i].m && g_hot[i].n > best) {
                best = g_hot[i].n;
                at_best = i;
            }
        if (!best || best * 100 < samples) break;
        at += (size_t)snprintf(line + at, sizeof(line) - at, ", %u%% %s.%s%s", (unsigned)(best * 100 / samples), g_hot[at_best].m->cls->name,
                               g_hot[at_best].m->name, (g_hot[at_best].m->flags & DVM_ACC_NATIVE) ? " (native)" : "");
        g_hot[at_best].n = 0;
    }
    mg_info("%s", line);
}

static void hot_main(void *arg)
{
    u32 samples = 0, outside = 0, collecting = 0;
    u64 since = mg_time_monotonic_ns();
    (void)arg;
    for (;;) {
        const mg_dvm_thread *t;
        const mg_dvm_method *m;
        u64 now;
        mg_sleep_ns(5000000ull);
        t = M.thread;
        m = t ? t->running : NULL;
        samples++;
        if (mg_dvm_gc_busy()) {
            collecting++;
        } else if (!m) {
            outside++;
        } else {
            u32 i = (u32)(((mg_addr)m >> 4) * 2654435761u) & (HOT_SLOTS - 1), tries;
            for (tries = 0; tries < 64 && g_hot[i].m && g_hot[i].m != m; tries++) i = (i + 1) & (HOT_SLOTS - 1);
            if (!g_hot[i].m) g_hot[i].m = m;
            if (g_hot[i].m == m) g_hot[i].n++;
        }
        now = mg_time_monotonic_ns();
        if (now - since >= 10000000000ull) {
            hot_say(samples, outside, collecting);
            meter_census();
            memset(g_hot, 0, sizeof(g_hot));
            samples = outside = collecting = 0;
            since = now;
        }
    }
}

static void hot_start(void)
{
    mg_thread *t = mg_thread_start(hot_main, NULL, (size_t)64 << 10, "hot");
    if (t) mg_thread_detach(t);
}

/* megler.Native.frameMark(phase, redrawn), from megler.ViewRoot.doFrame on the main thread. */
N(Native_frameMark)
{
    int phase = c->args[0].i;
    const mg_dvm_thread *self = mg_dvm_self();
    meter_sum now;
    if (M.thread && M.thread != self) return;
    meter_read(&now);
    if (!M.thread) {
        M.thread = self;
        hot_start();
        M.last = now;
        M.part = PART_BETWEEN;
        meter_start(&M.window, now.wall);
        M.run = M.window;
    }
    meter_advance(&now);
    if (phase == 1) {
        M.frame_started = now.wall;
        M.part = PART_LAYOUT;
    } else if (phase == 2) {
        M.part = PART_DRAW;
    } else if (phase == 3 && M.part != PART_BETWEEN) {
        u64 took = now.wall - M.frame_started;
        meter_window *w[2];
        int i;
        w[0] = &M.window;
        w[1] = &M.run;
        for (i = 0; i < 2; i++) {
            w[i]->frames++;
            if (c->args[1].i > 0) {
                w[i]->drawn++;
                w[i]->redrawn += (u64)c->args[1].i;
            }
            if (took > w[i]->longest) w[i]->longest = took;
        }
        M.part = PART_BETWEEN;
        if (now.wall - M.window.started >= METER_EVERY_NS) {
            meter_line("", &M.window, now.wall);
            meter_start(&M.window, now.wall);
        }
    }
}

/* The app has ended (its main thread, AppRunner.run returned): the window that was not said yet, and the whole run. */
static void meter_end(void)
{
    meter_sum now;
    if (!M.thread || M.thread != mg_dvm_self()) return;
    meter_read(&now);
    meter_advance(&now);
    if (M.window.frames) meter_line("", &M.window, now.wall);
    meter_line("in all, ", &M.run, now.wall);
}

/* ------------------------------------------------------- the main thread */

static void open_apk(int index, const char *host_path)
{
    char err[200];
    if (index >= MAX_APKS || !host_path) return;
    if (mg_zip_open(&J.apk[index], host_path, err, sizeof(err))) {
        J.apk_open[index] = true;
        J.apk_path[index] = mg_strdup(host_path);
    } else if (index != 1) {
        mg_warn("java: %s: %s", host_path, err);
    }
}

/*
 * Gives the APK its Android path, /data/app/<package>-1/base.apk, and tells the runtime that its classes are on the class path under that name. And the
 * splits theirs, beside it, under the names Android gives installed splits (split_config.armeabi_v7a.apk), each a mount of
 * its own file wherever the shell keeps it; those with code are on the class path under their names too.
 */
static void mount_apk(void)
{
    const char *path = J.config->apk_path, *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *name = back && (!slash || back > slash) ? back + 1 : slash ? slash + 1 : path;
    char *dir = name > path ? mg_strdup(path) : mg_strdup("."), *guest_dir = mg_sprintf("/data/app/%s-1", J.package);
    int i;
    if (name > path) dir[name - path - 1] = 0;
    mg_fs_mount(guest_dir, dir);
    if (J.config->gms_apk_path && strcmp(J.package, "com.google.android.gms"))
        mg_fs_mount("/system/priv-app/GmsCore/GmsCore.apk", J.config->gms_apk_path);
    if (J.config->gsf_apk_path && strcmp(J.package, "com.google.android.gsf"))
        mg_fs_mount("/system/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk", J.config->gsf_apk_path);
    /* Each app retains Android's stable base.apk path, regardless of the picked filename. */
    snprintf(J.guest_apk, sizeof(J.guest_apk), "%s/base.apk", guest_dir);
    mg_fs_mount(J.guest_apk, path);
    mg_dvm_dex_source_added(J.guest_apk);
    for (i = 0; i < J.nsplits; i++) {
        J.guest_splits[i] = mg_sprintf("%s/split_%s.apk", guest_dir, J.split_names[i]);
        mg_fs_mount(J.guest_splits[i], J.config->split_apks[i]);
        if (J.split_code[i]) mg_dvm_dex_source_added(J.guest_splits[i]);
    }
    free(dir);
    free(guest_dir);
}

/*
 * The app's native libraries, where Android puts them when it installs an
 * app: the folder the framework names as ApplicationInfo.nativeLibraryDir
 * (<data>/lib), each a file of its own. The class loader looks there
 * (BaseDexClassLoader.findLibrary, which System.loadLibrary asks, opens the
 * file to see that it is there), and so do apps: loaders that read a
 * library's ELF header to load what it needs first (Facebook's SoLoader, in
 * every React Native app), and code that lists the folder. A file is written
 * when it is not there or not of the size the APK's is, so the second start
 * of an app writes nothing. The library that runs is still the APK's, taken
 * from there by the linker (dvm/mg_dvm_jvm.c).
 */
static void place_one(const char *name, u64 size, void *user)
{
    const char *dir = (const char *)user, *abi = mg_ld_abi();
    char *path = mg_sprintf("%s/%s", dir, name), entry[400];
    mg_stat st;
    int i;
    if (!(mg_path_stat(path, &st, NULL) && !st.is_dir && st.size == size)) {
        snprintf(entry, sizeof(entry), "lib/%s/%s", abi ? abi : "", name);
        for (i = 0; mg_ld_apk_at(i); i++) {
            const mg_zip *z = mg_ld_apk_at(i);
            const mg_zip_entry *e = mg_zip_find(z, entry);
            size_t n = 0;
            u8 *bytes = e ? mg_zip_read(z, e, 0x40000000u, &n) : NULL;
            if (!e) continue;
            if (!bytes || !mg_file_write_all(path, bytes, n)) {
                /* No room, or it cannot be read: an empty file still says "it is there" to the class loader. */
                mg_warn("java: %s could not be written to the app's library folder (%s)", name, bytes ? "the file could not be written" : "the APK's entry could not be read");
                mg_file_write_all(path, "", 0);
            }
            free(bytes);
            break;
        }
    }
    free(path);
}

static void place_libraries(void)
{
    const char *data = mg_fs_internal_dir();
    char *guest = mg_sprintf("%s/lib", data ? data : ""), *host = data && data[0] ? mg_fs_host_path(guest) : NULL;
    int n;
    if (host && mg_path_mkdirs(host)) {
        n = mg_ld_apk_libs(place_one, host);
        if (n) mg_info("java: the app's %d native librar%s (%s) %s in %s", n, n == 1 ? "y" : "ies", mg_ld_abi(), n == 1 ? "is" : "are", guest);
    }
    free(host);
    free(guest);
}

/*
 * Where an exception came from, for a person: the innermost method on its stack trace that is the app's own
 * ("com.example.Main.onCreate"), or, when none of the app's is on it, the innermost of all. Asked of the exception itself
 * (Throwable.getStackTrace), which still has its trace after the framework has logged it. Empty when it has none.
 */
static void thrown_in(mg_jobj *e, char *out, size_t n)
{
    mg_jvalue trace, cls, method;
    u32 i;
    int pass;
    out[0] = 0;
    trace.j = 0;
    if (!e || !mg_dvm_call_virtual(e, "getStackTrace", "()[Ljava/lang/StackTraceElement;", NULL, &trace) || !trace.l) {
        mg_dvm_clear_pending();
        return;
    }
    for (pass = 0; pass < 2 && !out[0]; pass++) {
        for (i = 0; i < trace.l->u.oarr.len && !out[0]; i++) {
            mg_jobj *el = trace.l->u.oarr.v[i];
            char slashed[300];
            const char *name;
            size_t k;
            cls.j = method.j = 0;
            if (!el || !mg_dvm_call_virtual(el, "getClassName", "()Ljava/lang/String;", NULL, &cls) || !mg_dvm_call_virtual(el, "getMethodName", "()Ljava/lang/String;", NULL, &method)) {
                mg_dvm_clear_pending();
                continue;
            }
            name = text_of(cls.l);
            for (k = 0; name[k] && k + 1 < sizeof(slashed); k++) slashed[k] = name[k] == '.' ? '/' : name[k];
            slashed[k] = 0;
            if (pass == 0 && !(mg_dvm_class_exists(slashed) && mg_dvm_class_is_app(mg_dvm_find_class(slashed)))) continue;
            snprintf(out, n, "%s.%s", name, text_of(method.l));
        }
    }
    mg_dvm_clear_pending();
}

/* Cuts a text to `max` bytes with "..." for the rest, at a character's boundary (the text is UTF-8). */
static void clip(char *s, size_t max)
{
    size_t k;
    if (strlen(s) <= max || max < 4) return;
    for (k = max - 3; k > 0 && ((u8)s[k] & 0xC0) == 0x80; k--) { }
    memcpy(s + k, "...", 4);
}

/*
 * An exception and its causes as the runtime describes them, "A: text | caused by B: text | caused by C: text", made
 * short enough for a dialog: the exception itself and the innermost cause, which is the one that says what went wrong,
 * each cut to 150 bytes, and what lies between left to the log. The result is at most some 330 bytes.
 */
static void shorten(char *what)
{
    static const char sep[] = " | caused by ";
    char first[160], last[160], *cause = strstr(what, sep), *inner = cause;
    if (!cause) {
        clip(what, 150);
        return;
    }
    while (strstr(inner + 1, sep)) inner = strstr(inner + 1, sep);
    mg_strlcpy(last, inner + sizeof(sep) - 1, sizeof(last));
    *cause = 0;
    mg_strlcpy(first, what, sizeof(first));
    clip(first, 150);
    clip(last, 150);
    snprintf(what, 340, "%s, caused by %s", first, last);
}

/* A flag trace-calls-<prefix>: the calls of the classes under that prefix are said (mg_dvm_call_trace_add). */
static void call_trace_flag(const char *rest, void *user)
{
    (void)user;
    mg_dvm_call_trace_add(rest);
}

/*
 * The platform's libraries the app's class loader has before the app's own code, as Android's LoadedApk puts a shared
 * library's jar ahead of the APK: Android's legacy Apache HTTP client (org.apache.http.legacy.dex in the Android root's
 * framework, tools/dvm-android-root.sh), when apache_http_wanted says so. Instagram's network library (liger) finds
 * org.apache.http.Header in its JNI_OnLoad; without it its start ended in "Failed to call JNI_OnLoad from 'liger'". Its
 * absence from the root is said and the app runs on.
 */
static void add_shared_libraries(void)
{
    char *path;
    if (!J.apache_http) return;
    path = mg_sprintf("%s/framework/org.apache.http.legacy.dex", J.root);
    if (mg_dvm_add_dex_file(path)) mg_info("java: the app's class path has Android's legacy Apache HTTP client (org.apache.http.legacy) before its own code");
    else mg_warn("java: the app asks for Android's legacy Apache HTTP client (org.apache.http.legacy), and it is not in the Android root (%s)", path);
    free(path);
}

/*
 * An app of the platform's own -- its manifest's sharedUserId is android.uid.system, as Android's Settings' is -- uses the
 * platform's hidden classes as freely as its public ones: Settings 9 names 225 classes of Android 9's framework that
 * Megler's does not have (android.provider.SearchIndexablesProvider, android.util.KeyValueListParser,
 * com.android.internal.*), and ended at its start on the first of them. Such an app has Android 9's own framework on its
 * class path after Megler's (framework/platform-framework*.dex, tools/dvm-android-root.sh): a class Megler's framework has
 * is Megler's, and one it lacks is the platform's own. Any other app has Megler's alone, as before.
 */
static void add_platform_framework(void)
{
    static const char *const parts[] = { "platform-framework.dex", "platform-framework-classes2.dex", "platform-framework-classes3.dex" };
    int i, added = 0;
    if (!J.platform_app) return;
    for (i = 0; i < (int)MG_COUNT(parts); i++) {
        char *path = mg_sprintf("%s/framework/%s", J.root, parts[i]);
        added += mg_dvm_add_dex_file(path) ? 1 : 0;
        free(path);
    }
    if (added) mg_info("java: an app of the platform's own: Android 9's framework is on its class path after Megler's (%d files)", added);
    else mg_warn("java: an app of the platform's own, and Android 9's framework is not in the Android root (framework/platform-framework.dex)");
}

/* The framework's methods that are signatures alone, beside it in the Android root: each one's first call is said (mg_dvm_stubs_load). */
static void stubs_load(void)
{
    char *path = mg_sprintf("%s/framework/megler-stubs.txt", J.root);
    if (!mg_dvm_stubs_load(path)) mg_info("api: no list of the framework's methods that are signatures alone (%s): their calls are not said", path);
    free(path);
}

static void main_thread(void *arg)
{
    char *framework = mg_sprintf("%s/framework/megler-framework.dex", J.root), *res = mg_sprintf("%s/framework/framework-res.apk", J.root);
    char why[1600], stack[1200];
    mg_jvalue args[2];
    int i;
    (void)arg;
    /* Android's main thread is one of the app's own: libcore's native libraries run on it. */
    mg_gthread_self();
    why[0] = 0;
    if (!mg_dvm_start_android(J.root)) snprintf(why, sizeof(why), "the Java runtime did not start on the Android root %s", J.root);
    else if (stubs_load(), !mg_dvm_add_dex_file(framework)) snprintf(why, sizeof(why), "the Java framework is not in the Android root (%s)", framework);
    else if (add_platform_framework(), mg_dvm_app_follows(), add_shared_libraries(), mg_dvm_add_apk(J.config->apk_path) < 1) snprintf(why, sizeof(why), "the APK has no classes.dex that can be read");
    if (!why[0]) {
        mg_jvalue sdk;
        static const struct { const char *cls, *field, *prop; } build_props[] = {
            { "android/os/Build$VERSION", "RELEASE", "ro.build.version.release" },
            { "android/os/Build$VERSION", "INCREMENTAL", "ro.build.version.incremental" },
            { "android/os/Build$VERSION", "SECURITY_PATCH", "ro.build.version.security_patch" },
            { "android/os/Build", "ID", "ro.build.id" },
            { "android/os/Build", "DISPLAY", "ro.build.display.id" },
            { "android/os/Build", "FINGERPRINT", "ro.build.fingerprint" },
            { "android/os/Build", "MODEL", "ro.product.model" },
            { "android/os/Build", "BRAND", "ro.product.brand" },
            { "android/os/Build", "PRODUCT", "ro.product.name" },
            { "android/os/Build", "DEVICE", "ro.product.device" },
            { "android/os/Build", "BOARD", "ro.product.board" },
            { "android/os/Build", "MANUFACTURER", "ro.product.manufacturer" },
            { "android/os/Build", "HARDWARE", "ro.hardware" },
        };
        size_t prop_index;
        if (mg_flag("trace-monitors")) mg_dvm_monitor_trace_start();
        if (mg_flag("trace-stacks")) mg_dvm_stacks_trace_start();
        mg_flags_each("trace-calls-", call_trace_flag, NULL);
        /*
         * One process, one answer to "which Android is this": what the framework's Build.VERSION.SDK_INT tells the app's Java
         * is what its native code is told too (the property, android_get_device_api_level, AConfiguration_getSdkVersion).
         */
        sdk.j = 0;
        if (mg_dvm_get_static("android/os/Build$VERSION", "SDK_INT", &sdk) && sdk.i > 0) {
            char level[16];
            snprintf(level, sizeof(level), "%d", (int)sdk.i);
            mg_sysprop_set("ro.build.version.sdk", level);
        } else {
            mg_dvm_clear_pending();
        }
        /* Java apps use the framework overlay's Build. Native libraries in the same
           process must report the same release, build and device via system properties. */
        for (prop_index = 0; prop_index < MG_COUNT(build_props); prop_index++) {
            mg_jvalue value;
            const char *text;
            if (!mg_dvm_get_static(build_props[prop_index].cls, build_props[prop_index].field, &value) || !value.l) {
                mg_dvm_clear_pending();
                continue;
            }
            text = mg_dvm_string_utf8(value.l);
            if (text) mg_sysprop_set(build_props[prop_index].prop, text);
        }
        /* A feature split's code joins the app's class path after the base's, as its class loader has it on Android. */
        for (i = 0; i < J.nsplits; i++) J.split_code[i] = mg_dvm_add_apk(J.config->split_apks[i]) > 0;
        mount_apk();
        place_libraries();
        open_apk(0, J.config->apk_path);
        open_apk(1, res);
        for (i = 0; i < J.nsplits; i++) open_apk(FIRST_SPLIT_APK + i, J.config->split_apks[i]);
        if (!J.apk_open[1]) mg_info("java: no framework-res.apk in the Android root: the platform's own resources (themes, widgets' pictures) are not there");
        mg_dvm_register(k_native);
        mg_dvm_register(k_absent);
        mg_dvm_register(mg_java_sqlite_natives());
        mg_dvm_register(mg_java_webview_natives());
        mg_dvm_register(mg_java_audio_natives());
        mg_java_media_set_host(&J.config->codec);
        mg_dvm_register(mg_java_media_natives());
        mg_java_camera_set_host(&J.config->camera);
        mg_dvm_register(mg_java_camera_natives());
        mg_java_location_set_host(&J.config->location);
        mg_dvm_register(mg_java_location_natives());
        mg_java_gfx_set_host(&J.config->gfx);
        mg_java_gfx_set_present(present, NULL);
        mg_java_gfx_set_present_canvas(present_canvas, NULL);
        mg_java_gfx_set_present_probe(mg_java_gl_views_probe_due, mg_java_gl_views_probe);
        /* What C keeps of the app's objects across calls: the collector keeps it too. */
        mg_dvm_add_root(&J.clipboard);
        mg_dvm_register(mg_java_gfx_natives());
        /* OpenGL ES and EGL from Java, and the window they are shown in: composed by the shell where it can. */
        mg_java_gl_start(J.config->width, J.config->height, show);
        mg_java_gl_set_shell(&J.config->gfx);
        mg_dvm_register(mg_java_gles_natives());
        mg_dvm_register(mg_java_gl_natives());
        memset(args, 0, sizeof(args));
        args[0].l = mg_dvm_new_string(J.activity);
        args[1].l = J.application[0] ? mg_dvm_new_string(J.application) : NULL;
        mg_info("java: starting %s%s%s", J.activity, J.application[0] ? " in " : "", J.application);
        if (!mg_dvm_call_static("megler/AppRunner", "run", "(Ljava/lang/String;Ljava/lang/String;)V", args, NULL)) {
            /*
             * An exception nothing caught came all the way up the main thread: thrown in an activity's onCreate, a
             * listener, a message. Android ends the app there as well -- its default handler logs "FATAL EXCEPTION: main",
             * kills the process and tells the user that the app has stopped -- so there is nothing to carry on with. What is
             * left to do is to say why, once, in a sentence a person can be shown: the exception, and the method of the
             * app's that threw it. The whole stack is in the log.
             */
            char what[2400], where[300];
            mg_jobj *thrown = mg_dvm_pending();
            stack[0] = 0;
            mg_dvm_pending_stack(stack, sizeof(stack));
            if (!mg_dvm_describe_pending(what, sizeof(what))) mg_strlcpy(what, "an exception", sizeof(what));
            mg_dvm_clear_pending();
            mg_err("java: the main thread ended with %s%s%s", what, stack[0] ? "\n" : "", stack);
            thrown_in(thrown, where, sizeof(where));
            shorten(what);
            snprintf(why, sizeof(why), "the app stopped because nothing caught %s%s%s%s, which ends an app on Android too", what, where[0] ? " (in " : "", where, where[0] ? ")" : "");
        }
        meter_end();
    }
    free(framework);
    free(res);
    /* A camera the app left open is let go of: its light off, the phone's camera free for whatever comes next. */
    mg_java_camera_close_all();
    J.end_status = why[0] ? -1 : 0;
    mg_strlcpy(J.end_reason, why[0] ? why : J.stop ? "stopped" : "", sizeof(J.end_reason));
    mg_atomic_xchg(&J.ended, 1);
    /* The reason is megler_last_error's too: the shell may ask after the app is gone. */
    if (why[0]) mg_run_set_error("%s", why);
    if (why[0]) mg_run_ended(-1, why);
    else mg_run_ended(0, J.stop ? "stopped" : NULL);
}

bool mg_java_ended(int *status, const char **reason)
{
    if (!J.ended) return false;
    if (status) *status = J.end_status;
    if (reason) *reason = J.end_reason;
    return true;
}

/*
 * Whether the app gets Android's old Apache HTTP client, org.apache.http.legacy, on its class path: as Android 9's package
 * manager gives it -- to an app whose manifest asks for it (<uses-library>, required or not) and to every app that targets
 * an Android before 9 (OrgApacheHttpLegacyUpdater). Since Android 9 it is not on the boot class path, so an app that has
 * its own copy of those classes and asks for neither keeps its own.
 */
static bool apache_http_wanted(const mg_apk_info *info)
{
    int i;
    if (info->target_sdk > 0 && info->target_sdk < 28) return true;
    for (i = 0; i < info->nuses_libraries; i++)
        if (!strcmp(info->uses_libraries[i], "org.apache.http.legacy")) return true;
    return false;
}

static bool java_start(const megler_run_config *config, const mg_apk_info *info)
{
    mg_stat st;
    char *probe;
    int i;
    memset(&J, 0, sizeof(J));
    mg_lock_init(&J.lock);
    J.config = config;
    J.root = config->android_root && config->android_root[0] ? mg_strdup(config->android_root) : mg_sprintf("%s/android", megler_package_dir());
    mg_strlcpy(J.activity, info->activity, sizeof(J.activity));
    mg_strlcpy(J.application, info->application, sizeof(J.application));
    mg_strlcpy(J.package, info->package, sizeof(J.package));
    J.apache_http = apache_http_wanted(info);
    J.platform_app = !strcmp(info->shared_user_id, "android.uid.system");
    /* The splits, in megler_run's order, which is info->splits' (run/mg_run.c): as many as the framework reads. */
    J.nsplits = config->split_count < info->nsplits ? config->split_count : info->nsplits;
    if (J.nsplits > MAX_SPLITS) {
        mg_warn("java: the app has %d splits, and the framework reads %d: %s and those after it are left out", J.nsplits, MAX_SPLITS, info->splits[MAX_SPLITS]);
        J.nsplits = MAX_SPLITS;
    }
    for (i = 0; i < J.nsplits; i++) mg_strlcpy(J.split_names[i], info->splits[i], sizeof(J.split_names[0]));
    /* The battery the app starts with is not news to it; what it becomes afterwards is (battery_news). */
    J.battery_changes = mg_battery_changes();
    mg_battery_read(&J.battery);
    probe = mg_sprintf("%s/framework/core-oj.dex", J.root);
    if (!mg_path_stat(probe, &st, NULL)) {
        mg_run_set_error("this app is Java, and the files to run Java (Android's libcore and the framework) are not in %s", J.root);
        free(probe);
        return false;
    }
    free(probe);
    if (!J.activity[0]) {
        mg_run_set_error("the manifest names no launcher activity");
        return false;
    }
    /* The device as the NDK describes it to the app's native code (AConfiguration): this surface. */
    mg_configuration_set(config->width, config->height, config->density_dpi > 0 ? config->density_dpi : 160);
    /* A stack the size Android gives an app's main thread would be 8 MB; bytecode's frames are not on it, and 4 is room for libcore's native code. */
    J.main = mg_thread_start(main_thread, NULL, 4u << 20, "android-main");
    return J.main != NULL;
}

static void push_event(int phase, int id, i32 x, i32 y)
{
    mg_lock_acquire(&J.lock);
    if (J.nevents == MAX_EVENTS && phase == EV_MOVE) {
        /* A full queue means the main thread is busy: a move can be lost, what ends a gesture cannot. */
        mg_lock_release(&J.lock);
        return;
    }
    if (J.nevents == MAX_EVENTS) {
        memmove(J.events, J.events + 4, sizeof(i32) * 4 * (MAX_EVENTS - 1));
        J.nevents--;
    }
    J.events[4 * J.nevents] = phase;
    J.events[4 * J.nevents + 1] = id;
    J.events[4 * J.nevents + 2] = x;
    J.events[4 * J.nevents + 3] = y;
    J.nevents++;
    mg_lock_release(&J.lock);
}

static void java_touch(int phase, int pointer_id, float x, float y)
{
    if (phase < EV_DOWN || phase > EV_CANCEL) return;
    push_event(phase, pointer_id, bits_of(x), bits_of(y));
}

static void java_key(int android_keycode, bool down)
{
    /* Back is its own event to the framework, once, when the key comes up; every other key is passed as it is. */
    if (android_keycode == 4) {
        if (!down) push_event(EV_BACK, 0, 0, 0);
    } else {
        push_event(EV_KEY, android_keycode, down ? 1 : 0, 0);
    }
}

static void java_set_paused(bool paused) { push_event(EV_HOST_PAUSE, paused ? 1 : 0, 0, 0); }

/* Whether there is a Java app for the keyboard to talk to. */
static bool java_runs(void) { return J.config && J.main && !J.ended; }

/*
 * One thing the keyboard sent, queued for megler.InputMethods. The first that arrives is said in the log, with whether it was
 * queued, so that a phone's log shows where typed text was lost: the shell says that it sent it, this that the core queued it,
 * and the framework (the overlay's InputMethods) where it went. Never the text itself.
 */
static void push_text(const char *kind, const char *utf8)
{
    static volatile i32 said;
    char *item;
    bool queued = false;
    if (!java_runs()) {
        mg_note_once("java: the keyboard sent something while no Java app runs: it is dropped");
        return;
    }
    item = mg_sprintf("%s%s", kind, utf8 ? utf8 : "");
    mg_lock_acquire(&J.lock);
    if (J.ntext < MAX_TEXT) {
        J.text[J.ntext++] = item;
        queued = true;
    }
    mg_lock_release(&J.lock);
    /* (Said before the event is queued, so that it comes before what the framework says of it.) */
    if (kind[0] != 'h' && mg_atomic_cas(&said, 0, 1) == 0)
        mg_info("java: the first thing the keyboard sent (%s) reached the core %s", kind[0] == 't' ? "text" : kind[0] == 'd' ? "a Backspace" : "an Enter",
                queued ? "and waits for the app's main thread" : "and is dropped: the app has not taken what was sent before");
    if (queued) push_event(EV_TEXT, 0, 0, 0);
    else free(item);                   /* the app has not taken five hundred of them: it is not listening */
}

void mg_java_text(const char *utf8)
{
    if (utf8 && utf8[0]) push_text("t", utf8);
}

/* A message or a call for megler.Phone: queued with its event, or dropped (no Java app, or it takes none). What it says never goes in the log. */
static void push_phone(char *item, const char *what)
{
    bool queued = false;
    if (!java_runs()) {
        mg_info("java: %s, while no Java app runs: not given to an app", what);
        free(item);
        return;
    }
    mg_lock_acquire(&J.lock);
    if (J.nphone < MAX_PHONE) {
        J.phone[J.nphone++] = item;
        queued = true;
    }
    mg_lock_release(&J.lock);
    mg_info("java: %s%s", what, queued ? ", for the app" : ", and the app has not taken the ones before: dropped");
    if (queued) push_event(EV_PHONE, 0, 0, 0);
    else free(item);
}

/* A line of an item may not hold the line end that ends it. */
static char *one_line(const char *s)
{
    char *d = mg_strdup(s ? s : ""), *p;
    for (p = d; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    return d;
}

void mg_java_phone_sms(const char *from, const char *body, i64 time_ms)
{
    char *who = one_line(from);
    push_phone(mg_sprintf("s%lld\n%s\n%s", (long long)time_ms, who, body ? body : ""), "the phone has a text message");
    free(who);
}

void mg_java_phone_call(const char *number, i64 time_ms, int type, int duration_s)
{
    static const char *const kinds[] = { "a call", "an incoming call", "an outgoing call", "a missed call", "a call", "a rejected call" };
    char *who = one_line(number), *what;
    what = mg_sprintf("the phone's history has %s", kinds[type >= 0 && type <= 5 ? type : 0]);
    push_phone(mg_sprintf("c%lld\n%d\n%d\n%s", (long long)time_ms, type, duration_s < 0 ? 0 : duration_s, who), what);
    free(what);
    free(who);
}

void mg_java_text_key(int key)
{
    /* Android's key codes for the keys that go to the field as a hardware keyboard's would. */
    static const int codes[] = { 0, 0, 0, 112 /* FORWARD_DEL */, 21, 22, 19, 20 /* DPAD_LEFT, RIGHT, UP, DOWN */, 122, 123 /* MOVE_HOME, MOVE_END */ };
    if (key == MEGLER_TEXT_BACKSPACE) push_text("d", NULL);
    /*
     * Enter is a new line where the field said so when the keyboard was shown for it, and typed as one. (The framework's
     * own "e" would decide it again, and decides it the wrong way round: megler.InputMethods makes a new line only where
     * IME_FLAG_NO_ENTER_ACTION is not set, which is never in a field of several lines.) Anywhere else it is the editor's
     * action.
     */
    else if (key == MEGLER_TEXT_ENTER) push_text(J.enter_newline ? "t\n" : "e", NULL);
    else if (key >= MEGLER_TEXT_DELETE && key <= MEGLER_TEXT_END && java_runs()) {
        push_event(EV_KEY, codes[key], 1, 0);
        push_event(EV_KEY, codes[key], 0, 0);
    }
}

void mg_java_keyboard_closed(void) { push_text("h", NULL); }

void mg_java_keyboard_height(int pixels)
{
    if (!java_runs()) return;
    mg_atomic_xchg(&J.keyboard_height, pixels > 0 ? pixels : 0);
    push_event(EV_INSETS, 0, 0, 0);
}

void mg_java_set_theme(uint32_t accent_argb, bool night)
{
    mg_atomic_xchg(&g_theme_accent, (i32)accent_argb);
    mg_atomic_xchg(&g_theme_night, night ? 1 : 0);
}

void mg_java_set_insets(int left, int top, int right, int bottom)
{
    int in[4], i;
    bool changed = false;
    in[0] = left; in[1] = top; in[2] = right; in[3] = bottom;
    for (i = 0; i < 4; i++) {
        if (in[i] < 0) in[i] = 0;
        if (mg_atomic_xchg(&g_bar_insets[i], in[i]) != in[i]) changed = true;
    }
    if (changed && java_runs()) push_event(EV_INSETS, 0, 0, 0);
}

static void java_stop(void)
{
    /* Before the main loop runs there is nobody to ask. */
    mg_atomic_cas(&J.stop, 0, 1);
}

const mg_driver mg_driver_java = { "java", java_start, java_touch, java_key, java_set_paused, NULL, java_stop };
