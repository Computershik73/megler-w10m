/*
 * The driver for an app that is Java: an Activity, views, no engine.
 *
 * Unlike the other drivers this one plays no Java part in C. It starts the
 * Dalvik runtime (dvm/) on Android's libcore, puts the Java framework and the
 * app's own DEX files after it, and calls megler.AppRunner.run on a thread that
 * stands for Android's main thread. The framework is the original Megler's
 * (tools/build-framework.sh): android.* in Java, which does in the app's own
 * process what Android does in system_server -- the Application, the
 * activity stack, the main Looper, views measured, laid out and drawn into a
 * bitmap. What it cannot do in Java it asks of megler.* classes' native
 * methods. This file is megler.Native: the log, the APK's files, the screen's
 * size, input, the end of the app. megler.Gfx, the drawing, is hk_java_gfx.c;
 * OpenGL ES and EGL, and a GLSurfaceView's thread, hk_java_gl.c.
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

#include "../bionic/hk_bionic.h"
#include "../bionic/hk_fd.h"
#include "../dvm/hk_dvm_internal.h"
#include "../host/hk_host.h"
#include "../ld/hk_ld.h"
#include "../ndk/hk_ndk.h"
#include "../run/hk_run.h"
#include "../zip/hk_zip.h"
#include "hk_driver.h"
#include "hk_java.h"
#include "hk_java_gfx.h"
#include "hk_java_gl.h"

#define MAX_EVENTS 256
#define MAX_APKS 12

/* The phases of an event as megler.AppRunner.deliver reads them. */
enum { EV_DOWN = 0, EV_MOVE = 1, EV_UP = 2, EV_CANCEL = 3, EV_KEY = 4, EV_BACK = 5, EV_HOST_PAUSE = 6, EV_TEXT = 7, EV_INSETS = 8 };

#define MAX_TEXT 512

static struct {
    const megler_run_config *config;
    char *root;                        /* the Android root, a host path */
    char activity[200], application[200], package[128];
    char guest_apk[400];               /* the APK's path as the app sees it */
    hk_thread *main;
    /* The APKs by the framework's numbers: 0 the app's, 1 the platform's resources, 2.. the app's splits. */
    hk_zip apk[MAX_APKS];
    bool apk_open[MAX_APKS];
    char *apk_path[MAX_APKS];
    hk_lock lock;
    i32 events[MAX_EVENTS * 4];
    int nevents;
    volatile i32 stop, exited, ended;
    int end_status;
    char end_reason[1600];
    hk_jobj *clipboard;
    hk_java_stats stats;
    /*
     * What the shell's keyboard has sent and the app has not taken yet, oldest first: "t<text>" typed text, "d" Backspace,
     * "e" Enter, "h" the keyboard was put away -- the letters megler.InputMethods reads. One EV_TEXT event is queued for
     * each, and each such event takes one, so that text and the keys that go as key events stay in the order they were sent.
     */
    char *text[MAX_TEXT];
    int ntext;
    volatile i32 keyboard_height;      /* pixels of the surface's bottom the keyboard covers */
    volatile i32 enter_newline;        /* the field the keyboard was last shown for takes Enter as a new line */
    /* The battery as the app was last told of it by a broadcast (at the start: as it was then), and the core's count of its changes at that time. */
    hk_battery battery;
    u32 battery_changes;
} J;

static i32 bits_of(float f)
{
    i32 v;
    memcpy(&v, &f, 4);
    return v;
}

static const char *text_of(hk_jobj *s)
{
    const char *t = hk_dvm_string_utf8(s);
    return t ? t : "";
}

/* ----------------------------------------------------------- megler.Native */

#define N(name) static void name(hk_jcall *c)

/* log(int priority, String tag, String msg): Android's priorities, 2 verbose to 7 assert, as logcat letters them. */
N(Native_log)
{
    static const char letters[] = "??VDIWEF";
    int priority = c->args[0].i;
    const char *tag = text_of(c->args[1].l), *msg = text_of(c->args[2].l);
    char letter = priority >= 2 && priority <= 7 ? letters[priority] : 'I';
    hk_log(priority >= 6 ? HK_LOG_ERROR : priority == 5 ? HK_LOG_WARN : HK_LOG_INFO, "%c/%s: %s", letter, tag, msg);
}

static hk_jobj *bytes_of(const u8 *data, size_t size)
{
    hk_jobj *a = hk_dvm_new_array("[B", (i32)size);
    if (a && size) memcpy(a->u.arr.data, data, size);
    return a;
}

/* A file of one of the APKs, whole; null when it is not there. */
static hk_jobj *apk_file(int apk, const char *path)
{
    const hk_zip_entry *e;
    size_t size = 0;
    u8 *data;
    hk_jobj *a;
    if (apk < 0 || apk >= MAX_APKS || !J.apk_open[apk]) return NULL;
    e = hk_zip_find(&J.apk[apk], path);
    if (!e) return NULL;
    data = hk_zip_read(&J.apk[apk], e, 256u << 20, &size);
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
    const hk_zip_entry *e;
    u64 offset = 0;
    int fd;
    if (apk < 0 || apk >= MAX_APKS || !J.apk_open[apk]) return -1;
    e = hk_zip_find(&J.apk[apk], path);
    if (!e || !hk_zip_stored_at(&J.apk[apk], e, &offset)) return -1;
    fd = hk_fd_open_host(J.apk_path[apk], HK_O_READ);
    if (fd < 0) return -1;
    return ((i64)fd << 40) | (i64)offset;
}

static i64 apk_length(int apk, const char *path)
{
    const hk_zip_entry *e = apk >= 0 && apk < MAX_APKS && J.apk_open[apk] ? hk_zip_find(&J.apk[apk], path) : NULL;
    return e ? (i64)e->usize : -1;
}

static char *asset_name(hk_jobj *path)
{
    const char *p = text_of(path);
    while (*p == '/') p++;
    return hk_sprintf("assets/%s", p);
}

N(Native_readAsset)
{
    char *name = asset_name(c->args[0].l);
    c->ret.l = apk_file(0, name);
    free(name);
}

N(Native_assetFd)
{
    char *name = asset_name(c->args[0].l);
    c->ret.j = apk_fd(0, name);
    free(name);
}

N(Native_assetLength)
{
    char *name = asset_name(c->args[0].l);
    c->ret.j = apk_length(0, name);
    free(name);
}

typedef struct asset_list {
    const char *dir;                   /* "assets/sub/" */
    size_t dir_len;
    char **names;
    int n, cap;
} asset_list;

/* An entry under the folder: its own name if it is a file there, the name of the folder it is in if it lies deeper; each once. */
static void list_one(const hk_zip_entry *e, void *user)
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
        l->names = (char **)hk_realloc(l->names, sizeof(char *) * (size_t)l->cap);
    }
    l->names[l->n] = (char *)hk_alloc(len + 1);
    memcpy(l->names[l->n], rest, len);
    l->names[l->n++][len] = 0;
}

N(Native_listAssets)
{
    asset_list l;
    char *dir = asset_name(c->args[0].l), *prefix;
    size_t n = strlen(dir);
    hk_jobj *a;
    int i;
    while (n && dir[n - 1] == '/') dir[--n] = 0;
    prefix = hk_sprintf("%s/", dir);
    memset(&l, 0, sizeof(l));
    l.dir = prefix;
    l.dir_len = strlen(prefix);
    if (J.apk_open[0]) hk_zip_each(&J.apk[0], prefix, list_one, &l);
    a = hk_dvm_new_array("[Ljava/lang/String;", l.n);
    for (i = 0; i < l.n; i++) {
        if (a) a->u.oarr.v[i] = hk_dvm_new_string(l.names[i]);
        free(l.names[i]);
    }
    free(l.names);
    free(prefix);
    free(dir);
    c->ret.l = a;
}

N(Native_readApkFile) { c->ret.l = apk_file(c->args[0].i, text_of(c->args[1].l)); }
N(Native_apkFileFd) { c->ret.j = apk_fd(c->args[0].i, text_of(c->args[1].l)); }
N(Native_apkFileLength) { c->ret.j = apk_length(c->args[0].i, text_of(c->args[1].l)); }

/*
 * The APK as the app's own code names it: under /data/app/<package>-1, where Android installs an app, and readable there
 * (the folder the file is in on the host is mounted at that place), so that the class loader the framework makes over
 * it finds the APK's resources, and code that opens its own APK as a zip file can.
 */
N(Native_apkPath) { c->ret.l = hk_dvm_new_string(J.guest_apk); }

/* setAppClassLoader(loader): the PathClassLoader the framework made for the app; the app's classes name it as theirs from now on. */
N(Native_setAppClassLoader) { hk_dvm_set_app_loader(c->args[0].l); }

N(Native_splitPaths) { c->ret.l = hk_dvm_new_array("[Ljava/lang/String;", 0); }

/*
 * Looper.prepare() and every turn of a message loop: the thread's native
 * looper, for native code that watches descriptors on its UI thread
 * (ALooper_forThread, ALooper_addFd). See ndk/hk_ndk.h. nativeLooperPoll
 * answers whether there is one, which is what makes an idle loop of a thread
 * other than the main one come round every 8 ms instead of sleeping.
 */
N(Native_looperPrepare) { (void)c; hk_looper_java_thread(); }
N(Native_looperPoll) { c->ret.z = hk_looper_poll_thread(); }
N(Native_nothing) { (void)c; }
N(Native_dataDir) { c->ret.l = hk_dvm_new_string(hk_fs_internal_dir()); }
N(Native_externalDir) { c->ret.l = hk_dvm_new_string(hk_fs_external_dir()); }
N(Native_packageName) { c->ret.l = hk_dvm_new_string(J.package); }
N(Native_screenWidth) { c->ret.i = J.config->width; }
N(Native_screenHeight) { c->ret.i = J.config->height; }
N(Native_density) { c->ret.f = (float)(J.config->density_dpi > 0 ? J.config->density_dpi : 160) / 160.0f; }
N(Native_uptimeNanos) { c->ret.j = (i64)hk_time_monotonic_ns(); }
static void Native_frameMark(hk_jcall *c);
static void Native_presentRect(hk_jcall *c);
static void Native_checkPartial(hk_jcall *c);

/*
 * The battery has been set to something else since the app was last told (hk_battery_set, from any thread; this is the
 * app's main thread). Android broadcasts ACTION_BATTERY_CHANGED to whoever is registered for it, and before that
 * ACTION_POWER_CONNECTED or ACTION_POWER_DISCONNECTED when it went between running on its battery and not; the
 * framework's megler.Broadcasts sends them (the overlay's batteryChanged). What is compared is what the app was told last
 * with what is there now, so a battery that changed and changed back between two looks is no news, and nothing is sent
 * more often than the charge, the status or the plug changes.
 */
static void battery_news(void)
{
    hk_battery now;
    hk_jvalue power;
    J.battery_changes = hk_battery_changes();
    hk_battery_read(&now);
    if (now.level == J.battery.level && now.scale == J.battery.scale && now.status == J.battery.status && now.plugged == J.battery.plugged) return;
    power.j = 0;
    power.i = (now.plugged != 0) == (J.battery.plugged != 0) ? 0 : now.plugged ? 1 : 2;
    J.battery = now;
    if (!hk_dvm_call_static("megler/Broadcasts", "batteryChanged", "(I)V", &power, NULL)) {
        hk_note_once("java: the battery changed and the app was not told: %s", hk_dvm_pending() ? hk_dvm_class_of(hk_dvm_pending())->name : "megler.Broadcasts.batteryChanged did not run");
        hk_dvm_clear_pending();
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
    hk_jobj *a = NULL;
    int n;
    if (J.stop == 1 && hk_atomic_cas(&J.stop, 1, 2) == 1) {
        hk_dvm_call_static("megler/AppRunner", "finishAll", "()V", NULL, NULL);
        if (hk_dvm_pending()) return;
    }
    if (hk_battery_changes() != J.battery_changes) battery_news();
    hk_lock_acquire(&J.lock);
    n = J.nevents;
    if (n) {
        a = hk_dvm_new_array("[I", 1 + 4 * n);
        if (a) {
            i32 *v = (i32 *)a->u.arr.data;
            v[0] = n;
            memcpy(v + 1, J.events, sizeof(i32) * 4 * (size_t)n);
            J.nevents = 0;
        }
    }
    hk_lock_release(&J.lock);
    hk_atomic_add(&J.stats.polls, 1);
    c->ret.l = a;
}

/* The clipboard is the app's own until the shell has one to offer. */
N(Native_setClipboard) { J.clipboard = c->args[0].l; }
N(Native_getClipboard) { c->ret.l = J.clipboard; }

/* The battery as the core has it (hk_battery_read, run/hk_run.h), in the order megler.Native.battery's comment gives. */
N(Native_battery)
{
    hk_battery b;
    hk_jobj *a = hk_dvm_new_array("[I", 8);
    hk_battery_read(&b);
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

N(Native_batteryTechnology)
{
    hk_battery b;
    hk_battery_read(&b);
    c->ret.l = hk_dvm_new_string(b.technology);
}

N(Native_insets)
{
    /* Left, top, right, bottom of the screen the app must keep clear -- none: the shell gives the app a surface that is all its own -- and what the keyboard covers of its bottom. */
    hk_jobj *a = hk_dvm_new_array("[I", 5);
    if (a) ((i32 *)a->u.arr.data)[4] = J.keyboard_height;
    c->ret.l = a;
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
    hk_atomic_xchg(&J.enter_newline, enter == MEGLER_ENTER_NEWLINE);
    if (k->show) k->show(k->user, kind, flags, enter);
    else hk_note_once("java: the app asked for the keyboard, and the shell gave the layer none to ask (megler_run_config.keyboard)");
}

/* pollText(): the oldest thing the keyboard sent, as a String[1], or null. Asked once for each EV_TEXT event. */
N(Native_pollText)
{
    char *item = NULL;
    hk_jobj *a;
    hk_lock_acquire(&J.lock);
    if (J.ntext) {
        item = J.text[0];
        memmove(&J.text[0], &J.text[1], sizeof(char *) * (size_t)--J.ntext);
    }
    hk_lock_release(&J.lock);
    c->ret.l = NULL;
    if (!item) return;
    a = hk_dvm_new_array("[Ljava/lang/String;", 1);
    if (a) a->u.oarr.v[0] = hk_dvm_new_string(item);
    free(item);
    c->ret.l = a;
}

/* What the shell would have to do and cannot be asked yet: said once, and the app goes on. */
N(Native_share) { hk_info("java: the app shares \"%s\" (nothing here to share it with)", text_of(c->args[0].l)); }
N(Native_openUrl) { hk_info("java: the app opens %s (nothing here to open it)", text_of(c->args[0].l)); }
N(Native_setOrientation) { hk_note_once("java: the app asked for orientation %d; the surface stays as the shell made it", (int)c->args[0].i); }

/*
 * exit(): the framework calls it when the last activity is gone. The main
 * loop is told to end, and megler.AppRunner.run returns to main_thread below.
 */
N(Native_exit)
{
    hk_jvalue looper;
    (void)c;
    hk_atomic_xchg(&J.exited, 1);
    looper.j = 0;
    if (hk_dvm_call_static("android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", NULL, &looper) && looper.l) hk_dvm_call_virtual(looper.l, "quit", "()V", NULL, NULL);
}

static const hk_jhle k_native[] = {
    { "megler/Native", "log", NULL, Native_log },
    { "megler/Native", "readAsset", NULL, Native_readAsset },
    { "megler/Native", "listAssets", NULL, Native_listAssets },
    { "megler/Native", "assetFd", NULL, Native_assetFd },
    { "megler/Native", "assetLength", NULL, Native_assetLength },
    { "megler/Native", "readApkFile", NULL, Native_readApkFile },
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
    { "megler/Native", "packageName", NULL, Native_packageName },
    { "megler/Native", "screenWidth", NULL, Native_screenWidth },
    { "megler/Native", "screenHeight", NULL, Native_screenHeight },
    { "megler/Native", "density", NULL, Native_density },
    { "megler/Native", "setContentView", NULL, Native_nothing },
    { "megler/Native", "pollInput", NULL, Native_pollInput },
    { "megler/Native", "pollText", NULL, Native_pollText },
    { "megler/Native", "showKeyboard", NULL, Native_showKeyboard },
    { "megler/Native", "setClipboard", NULL, Native_setClipboard },
    { "megler/Native", "getClipboard", NULL, Native_getClipboard },
    { "megler/Native", "share", NULL, Native_share },
    { "megler/Native", "setOrientation", NULL, Native_setOrientation },
    { "megler/Native", "insets", NULL, Native_insets },
    { "megler/Native", "nightMode", NULL, Native_nothing },
    { "megler/Native", "vibrate", NULL, Native_nothing },
    { "megler/Native", "openUrl", NULL, Native_openUrl },
    { "megler/Native", "exit", NULL, Native_exit },
    { "megler/Native", "uptimeNanos", NULL, Native_uptimeNanos },
    { "megler/Native", "frameMark", NULL, Native_frameMark },
    { "megler/Native", "presentRect", NULL, Native_presentRect },
    { "megler/Native", "checkPartial", NULL, Native_checkPartial },
    { NULL, NULL, NULL, NULL },
};

/* ------------------------------------------- what is not answered yet */

/*
 * The framework's other native classes: sound, web views, sensors and video
 * decoding. None is written here yet. Each of their methods answers zero,
 * false or null, said once in the log for the class, and the framework's own
 * Java makes of that what an app can live with: a sound that did not load is
 * id 0, a sensor is not available, a codec not supported, a web view an empty
 * rectangle. The one that needs another answer than zero to say "no" is below.
 * (Databases, megler.Sqlite, are hk_java_sqlite.c; OpenGL ES and EGL,
 * hk_java_gl.c and hk_java_gles.c, whose tables are registered after this one:
 * the entries here for their classes are only for a native the framework might
 * declare and they do not bind.)
 */
N(Absent)
{
    /* (The native method's own frame is the top one: its class is what is missing.) The callers say who wanted it. */
    static const hk_dvm_class *said[16];
    static volatile i32 nsaid;
    const hk_dvm_frame *f = hk_dvm_self()->top, *up;
    const hk_dvm_class *cls = f && f->m ? f->m->cls : NULL;
    char who[500];
    size_t k = 0;
    int n, i;
    c->ret.j = 0;
    /* Once for each class, whoever asks. (Two threads asking at the same moment may each say it.) */
    for (i = 0; i < nsaid && i < (int)HK_COUNT(said); i++)
        if (said[i] == cls) return;
    i = hk_atomic_add(&nsaid, 1) - 1;
    if (i < (int)HK_COUNT(said)) said[i] = cls;
    who[0] = 0;
    for (up = f ? f->prev : NULL, n = 0; up && n < 5 && k + 120 < sizeof(who); up = up->prev, n++)
        k += (size_t)snprintf(who + k, sizeof(who) - k, "%s%s.%s", n ? " < " : "", up->m->cls->name, up->m->name);
    hk_warn("java: %s is not there yet: its native methods answer nothing (first asked by %s)", cls ? cls->name : "a class of the framework", who[0] ? who : "the layer");
}
/*
 * Sensors.available(type): no, and that is no gap to report. The framework's SensorManager asks it for every kind of sensor
 * when the class is first used, which is at every app's start (ContextImpl's table of system services names the class).
 * "There is no such sensor" is the true answer until there are sensors; an app that registers a listener still meets the
 * line above.
 */
N(Sensors_available) { c->ret.j = 0; }
/*
 * MediaCodecNative.supported(mime): no, for every type, and no gap to report either. A MediaCodec is fed an app's compressed
 * samples one at a time; the shell's decoding (megler_media_host, Media Foundation's source reader on a phone) takes a whole
 * file by its path, which the NDK's AMediaExtractor and AMediaCodec are made to fit (ndk/hk_media.c) and a Java MediaCodec
 * cannot be. Decoding samples needs a call of the shell's that megler.h does not have. The overlay's MediaCodecList lists a
 * decoder only when this says yes, so its list is empty, as a phone's without decoders is (AIDA64's Codecs page is that
 * list). The class's other methods are still "not there yet" when something calls them.
 */
N(MediaCodecNative_supported) { c->ret.j = 0; }
/* An AudioTrack is initialised when its id is not negative. */
N(Absent_minus_one) { Absent(c); c->ret.j = 0; c->ret.i = -1; }

static const hk_jhle k_absent[] = {
    { "megler/Audio", "trackOpen", NULL, Absent_minus_one },
    { "megler/Sensors", "available", NULL, Sensors_available },
    { "megler/MediaCodecNative", "supported", NULL, MediaCodecNative_supported },
    { "megler/Audio", NULL, NULL, Absent },
    { "megler/Web", NULL, NULL, Absent },
    { "megler/Sensors", NULL, NULL, Absent },
    { "megler/MediaCodecNative", NULL, NULL, Absent },
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
static HK_THREAD_LOCAL u64 t_present_ns;
/* Native.presentRect: what the next present on this thread changed, left, top, right, bottom; right 0: not said, all of it. */
static HK_THREAD_LOCAL i32 t_part[4];

N(Native_presentRect)
{
    int i;
    for (i = 0; i < 4; i++) t_part[i] = c->args[i].i;
}

/*
 * A picture of the whole window to the shell, of which `part` (left, top, right, bottom) changed; part[2] 0: all of it.
 * The views' (present, below) and, once the app draws with EGL on a view, the window composed of the views and the GL
 * frames (hk_java_gl.c), on the main thread and on the threads that swap, one call at a time.
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
    started = hk_time_monotonic_ns();
    if (!hk_java_gl_views(pixels, width, height, stride, t_part)) show(pixels, width, height, stride, t_part);
    t_part[0] = t_part[1] = t_part[2] = t_part[3] = 0;
    hk_run_count_frame();
    hk_atomic_add(&J.stats.frames, 1);
    t_present_ns += hk_time_monotonic_ns() - started;
}

/*
 * Gfx.present, the window's views, shown by the shell from its own picture of the canvas that draws them, without the memory
 * (megler_gfx_host.present): what hk_java_gfx.c tries before it brings the memory up to date. Not while OpenGL frames are
 * composed under the views, which needs the views' memory; and false when the shell cannot, the memory's way then following.
 */
static bool present_canvas(void *user, void *canvas, int width, int height)
{
    u64 started;
    bool shown;
    (void)user;
    if (!J.config->gfx.present || hk_java_gl_composing()) return false;
    started = hk_time_monotonic_ns();
    if (t_part[2] > t_part[0] && t_part[3] > t_part[1])
        shown = J.config->gfx.present(J.config->gfx.user, canvas, t_part[0], t_part[1], t_part[2] - t_part[0], t_part[3] - t_part[1]);
    else
        shown = J.config->gfx.present(J.config->gfx.user, canvas, 0, 0, width, height);
    if (!shown) return false;
    t_part[0] = t_part[1] = t_part[2] = t_part[3] = 0;
    hk_run_count_frame();
    hk_atomic_add(&J.stats.frames, 1);
    t_present_ns += hk_time_monotonic_ns() - started;
    return true;
}

void hk_java_get_stats(hk_java_stats *out) { *out = J.stats; }

static bool g_check_partial;
void hk_java_check_partial(bool on) { g_check_partial = on; }
N(Native_checkPartial) { c->ret.z = g_check_partial; }

/* ------------------------------------------------------- the frame meter */

/*
 * Where the main thread's time goes while the app draws, from the core's side and in summed counters: nothing is logged per
 * call or per frame. megler.ViewRoot.doFrame (overlaid for this) calls Native.frameMark(1) as a frame begins with its measure
 * and layout, frameMark(2) as its draw traversal begins and frameMark(3, pixels drawn) as it ends. At each mark the main
 * thread's clock, its time in the shell's drawing (hk_java_gfx_host_ns), in handing pictures over (present, above), in
 * Object.wait and Thread.sleep, in native methods, allocating and waiting for monitors, and its instructions
 * (hk_dvm_thread_costs) are read, and what passed since the mark before
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
    u64 collections, collecting_ns;          /* and the collector's count and time (hk_dvm_gc_totals) */
    u32 frames, drawn;
    u64 longest, redrawn;                    /* the longest frame from mark 1 to mark 3; pixels drawn */
    meter_sum part[PARTS];
} meter_window;

static struct {
    const hk_dvm_thread *thread;             /* the main thread, at its first mark; NULL: no frame yet */
    meter_sum last;                          /* the readings at the last mark (wall: the clock) */
    int part;                                /* where the time since the last mark goes */
    u64 frame_started;
    meter_window window, run;
} M;

#define METER_EVERY_NS 5000000000ull

static void meter_read(meter_sum *r)
{
    r->wall = hk_time_monotonic_ns();
    r->host = hk_java_gfx_host_ns();
    hk_dvm_costs c;
    r->present = t_present_ns;
    hk_dvm_thread_costs(&c);
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
    hk_dvm_stats s;
    memset(w, 0, sizeof(*w));
    hk_dvm_get_stats(&s);
    w->started = now;
    w->all_executed = hk_dvm_executed_all();
    w->object_bytes = s.object_bytes;
    hk_dvm_gc_totals(&w->collections, &w->collecting_ns);
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

static void meter_line(const char *what, const meter_window *w, u64 now)
{
    hk_dvm_stats s;
    u64 busy[PARTS], java = 0, host = 0, present = 0, waited = 0, executed = 0, elapsed = now - w->started, screen;
    u64 native = 0, natives = 0, alloc = 0, allocs = 0, blocked = 0, wall = 0, other, interpreted, live_bytes;
    u64 bitmap_bytes = 0, process = hk_mem_usage(), collections = 0, collecting_ns = 0;
    u32 bitmaps = 0, n = w->frames;
    char of[40], redrawn[60], rates[300], mem[40], gc[120];
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
    hk_dvm_get_stats(&s);
    live_bytes = hk_dvm_live_bytes();
    hk_java_gfx_stats(&bitmap_bytes, &bitmaps);
    screen = (u64)(J.config->width > 0 ? J.config->width : 0) * (u64)(J.config->height > 0 ? J.config->height : 0);
    of[0] = 0;
    if (w->drawn != n) snprintf(of, sizeof(of), " of %u frames", (unsigned)n);
    if (w->drawn && screen)
        snprintf(redrawn, sizeof(redrawn), "redrawn %u%% of the screen a frame", (unsigned)(w->redrawn * 100 / (screen * w->drawn)));
    else
        hk_strlcpy(redrawn, "nothing drawn", sizeof(redrawn));
    /*
     * Where Java's time went besides the shell's drawing. The shell's drawing, present and the waits all happen inside native
     * methods, so the other natives are what is left of the natives' time without them. Interpreting is the rest: the time
     * outside native methods, allocation and waits for monitors -- bytecode, calls and returns, resolution, the
     * interpreter's own work. The pace is the instructions over that time. And what every thread executed.
     */
    other = native > host + present + waited ? native - host - present - waited : 0;
    interpreted = wall > native + alloc + blocked ? wall - native - alloc - blocked : 0;
    snprintf(rates, sizeof(rates), "in other native methods %.1f (%.0f calls), allocating %.1f (%.0f objects), waiting for monitors %.1f, "
             "interpreting %.1f at %.1f M instructions/s; %.1f M instructions on all threads",
             per_frame_ms(other, n), n ? (double)natives / n : 0.0, per_frame_ms(alloc, n), n ? (double)allocs / n : 0.0, per_frame_ms(blocked, n),
             per_frame_ms(interpreted, n), interpreted ? (double)executed * 1e3 / (double)interpreted : 0.0,
             (double)(hk_dvm_executed_all() - w->all_executed) / 1e6);
    mem[0] = 0;
    if (process) snprintf(mem, sizeof(mem), "; the process %u MB", (unsigned)(process >> 20));
    /* The collector, as its own part: what it took in the time the line covers, every thread of the runtime's stopped for most of it. */
    hk_dvm_gc_totals(&collections, &collecting_ns);
    snprintf(gc, sizeof(gc), "collecting %.1f ms in %u collections", (double)(collecting_ns - w->collecting_ns) / 1e6, (unsigned)(collections - w->collections));
    hk_info("java: frames %s%.1f s: %u drawn%s, %.1f ms of work each on average (the longest frame %.1f without input and messages): Java %.1f "
            "(input and messages %.1f, measure and layout %.1f, "
            "draw traversal %.1f), of it in the shell's drawing %.1f, %s; present %.1f; idle %u%%; %s; heap %u MB (objects %u live, %u "
            "made in these %.1f s; bitmaps %u)%s; %s",
            what, (double)elapsed / 1e9, (unsigned)w->drawn, of, per_frame_ms(java + present, n), per_frame_ms(w->longest, 1), per_frame_ms(java, n),
            per_frame_ms(busy[PART_BETWEEN], n), per_frame_ms(busy[PART_LAYOUT], n), per_frame_ms(busy[PART_DRAW], n), per_frame_ms(host, n), rates,
            per_frame_ms(present, n), (unsigned)(elapsed ? waited * 100 / elapsed : 0), gc,
            (unsigned)((live_bytes + bitmap_bytes) >> 20), (unsigned)(live_bytes >> 20), (unsigned)((s.object_bytes - w->object_bytes) >> 20),
            (double)elapsed / 1e9, (unsigned)(bitmap_bytes >> 20), mem, redrawn);
}

/* megler.Native.frameMark(phase, redrawn), from megler.ViewRoot.doFrame on the main thread. */
N(Native_frameMark)
{
    int phase = c->args[0].i;
    const hk_dvm_thread *self = hk_dvm_self();
    meter_sum now;
    if (M.thread && M.thread != self) return;
    meter_read(&now);
    if (!M.thread) {
        M.thread = self;
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
    if (!M.thread || M.thread != hk_dvm_self()) return;
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
    if (hk_zip_open(&J.apk[index], host_path, err, sizeof(err))) {
        J.apk_open[index] = true;
        J.apk_path[index] = hk_strdup(host_path);
    } else if (index != 1) {
        hk_warn("java: %s: %s", host_path, err);
    }
}

/* Gives the APK its Android path, and tells the runtime that its classes are on the class path under that name. */
static void mount_apk(void)
{
    const char *path = J.config->apk_path, *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *name = back && (!slash || back > slash) ? back + 1 : slash ? slash + 1 : path;
    char *dir = name > path ? hk_strdup(path) : hk_strdup("."), *guest_dir = hk_sprintf("/data/app/%s-1", J.package);
    if (name > path) dir[name - path - 1] = 0;
    hk_fs_mount(guest_dir, dir);
    snprintf(J.guest_apk, sizeof(J.guest_apk), "%s/%s", guest_dir, name);
    hk_dvm_dex_source_added(J.guest_apk);
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
 * from there by the linker (dvm/hk_dvm_jvm.c).
 */
static void place_one(const char *name, u64 size, void *user)
{
    const char *dir = (const char *)user, *abi = hk_ld_abi();
    char *path = hk_sprintf("%s/%s", dir, name), entry[400];
    hk_stat st;
    int i;
    if (!(hk_path_stat(path, &st, NULL) && !st.is_dir && st.size == size)) {
        snprintf(entry, sizeof(entry), "lib/%s/%s", abi ? abi : "", name);
        for (i = 0; hk_ld_apk_at(i); i++) {
            const hk_zip *z = hk_ld_apk_at(i);
            const hk_zip_entry *e = hk_zip_find(z, entry);
            size_t n = 0;
            u8 *bytes = e ? hk_zip_read(z, e, 0x40000000u, &n) : NULL;
            if (!e) continue;
            if (!bytes || !hk_file_write_all(path, bytes, n)) {
                /* No room, or it cannot be read: an empty file still says "it is there" to the class loader. */
                hk_warn("java: %s could not be written to the app's library folder (%s)", name, bytes ? "the file could not be written" : "the APK's entry could not be read");
                hk_file_write_all(path, "", 0);
            }
            free(bytes);
            break;
        }
    }
    free(path);
}

static void place_libraries(void)
{
    const char *data = hk_fs_internal_dir();
    char *guest = hk_sprintf("%s/lib", data ? data : ""), *host = data && data[0] ? hk_fs_host_path(guest) : NULL;
    int n;
    if (host && hk_path_mkdirs(host)) {
        n = hk_ld_apk_libs(place_one, host);
        if (n) hk_info("java: the app's %d native librar%s (%s) %s in %s", n, n == 1 ? "y" : "ies", hk_ld_abi(), n == 1 ? "is" : "are", guest);
    }
    free(host);
    free(guest);
}

/*
 * Where an exception came from, for a person: the innermost method on its stack trace that is the app's own
 * ("com.example.Main.onCreate"), or, when none of the app's is on it, the innermost of all. Asked of the exception itself
 * (Throwable.getStackTrace), which still has its trace after the framework has logged it. Empty when it has none.
 */
static void thrown_in(hk_jobj *e, char *out, size_t n)
{
    hk_jvalue trace, cls, method;
    u32 i;
    int pass;
    out[0] = 0;
    trace.j = 0;
    if (!e || !hk_dvm_call_virtual(e, "getStackTrace", "()[Ljava/lang/StackTraceElement;", NULL, &trace) || !trace.l) {
        hk_dvm_clear_pending();
        return;
    }
    for (pass = 0; pass < 2 && !out[0]; pass++) {
        for (i = 0; i < trace.l->u.oarr.len && !out[0]; i++) {
            hk_jobj *el = trace.l->u.oarr.v[i];
            char slashed[300];
            const char *name;
            size_t k;
            cls.j = method.j = 0;
            if (!el || !hk_dvm_call_virtual(el, "getClassName", "()Ljava/lang/String;", NULL, &cls) || !hk_dvm_call_virtual(el, "getMethodName", "()Ljava/lang/String;", NULL, &method)) {
                hk_dvm_clear_pending();
                continue;
            }
            name = text_of(cls.l);
            for (k = 0; name[k] && k + 1 < sizeof(slashed); k++) slashed[k] = name[k] == '.' ? '/' : name[k];
            slashed[k] = 0;
            if (pass == 0 && !(hk_dvm_class_exists(slashed) && hk_dvm_class_is_app(hk_dvm_find_class(slashed)))) continue;
            snprintf(out, n, "%s.%s", name, text_of(method.l));
        }
    }
    hk_dvm_clear_pending();
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
    hk_strlcpy(last, inner + sizeof(sep) - 1, sizeof(last));
    *cause = 0;
    hk_strlcpy(first, what, sizeof(first));
    clip(first, 150);
    clip(last, 150);
    snprintf(what, 340, "%s, caused by %s", first, last);
}

static void main_thread(void *arg)
{
    char *framework = hk_sprintf("%s/framework/megler-framework.dex", J.root), *res = hk_sprintf("%s/framework/framework-res.apk", J.root);
    char why[1600], stack[1200];
    hk_jvalue args[2];
    int i;
    (void)arg;
    /* Android's main thread is one of the app's own: libcore's native libraries run on it. */
    hk_gthread_self();
    why[0] = 0;
    if (!hk_dvm_start_android(J.root)) snprintf(why, sizeof(why), "the Java runtime did not start on the Android root %s", J.root);
    else if (!hk_dvm_add_dex_file(framework)) snprintf(why, sizeof(why), "the Java framework is not in the Android root (%s)", framework);
    else if (hk_dvm_app_follows(), hk_dvm_add_apk(J.config->apk_path) < 1) snprintf(why, sizeof(why), "the APK has no classes.dex that can be read");
    if (!why[0]) {
        hk_jvalue sdk;
        /*
         * One process, one answer to "which Android is this": what the framework's Build.VERSION.SDK_INT tells the app's Java
         * is what its native code is told too (the property, android_get_device_api_level, AConfiguration_getSdkVersion).
         */
        sdk.j = 0;
        if (hk_dvm_get_static("android/os/Build$VERSION", "SDK_INT", &sdk) && sdk.i > 0) {
            char level[16];
            snprintf(level, sizeof(level), "%d", (int)sdk.i);
            hk_sysprop_set("ro.build.version.sdk", level);
        } else {
            hk_dvm_clear_pending();
        }
        for (i = 0; i < J.config->split_count; i++) hk_dvm_add_apk(J.config->split_apks[i]);
        mount_apk();
        place_libraries();
        open_apk(0, J.config->apk_path);
        open_apk(1, res);
        for (i = 0; i < J.config->split_count; i++) open_apk(2 + i, J.config->split_apks[i]);
        if (!J.apk_open[1]) hk_info("java: no framework-res.apk in the Android root: the platform's own resources (themes, widgets' pictures) are not there");
        hk_dvm_register(k_native);
        hk_dvm_register(k_absent);
        hk_dvm_register(hk_java_sqlite_natives());
        hk_java_gfx_set_host(&J.config->gfx);
        hk_java_gfx_set_present(present, NULL);
        hk_java_gfx_set_present_canvas(present_canvas, NULL);
        /* What C keeps of the app's objects across calls: the collector keeps it too. */
        hk_dvm_add_root(&J.clipboard);
        hk_dvm_register(hk_java_gfx_natives());
        /* OpenGL ES and EGL from Java, and the window they are shown in. */
        hk_java_gl_start(J.config->width, J.config->height, show);
        hk_dvm_register(hk_java_gles_natives());
        hk_dvm_register(hk_java_gl_natives());
        memset(args, 0, sizeof(args));
        args[0].l = hk_dvm_new_string(J.activity);
        args[1].l = J.application[0] ? hk_dvm_new_string(J.application) : NULL;
        hk_info("java: starting %s%s%s", J.activity, J.application[0] ? " in " : "", J.application);
        if (!hk_dvm_call_static("megler/AppRunner", "run", "(Ljava/lang/String;Ljava/lang/String;)V", args, NULL)) {
            /*
             * An exception nothing caught came all the way up the main thread: thrown in an activity's onCreate, a
             * listener, a message. Android ends the app there as well -- its default handler logs "FATAL EXCEPTION: main",
             * kills the process and tells the user that the app has stopped -- so there is nothing to carry on with. What is
             * left to do is to say why, once, in a sentence a person can be shown: the exception, and the method of the
             * app's that threw it. The whole stack is in the log.
             */
            char what[2400], where[300];
            hk_jobj *thrown = hk_dvm_pending();
            stack[0] = 0;
            hk_dvm_pending_stack(stack, sizeof(stack));
            if (!hk_dvm_describe_pending(what, sizeof(what))) hk_strlcpy(what, "an exception", sizeof(what));
            hk_dvm_clear_pending();
            hk_err("java: the main thread ended with %s%s%s", what, stack[0] ? "\n" : "", stack);
            thrown_in(thrown, where, sizeof(where));
            shorten(what);
            snprintf(why, sizeof(why), "the app stopped because nothing caught %s%s%s%s, which ends an app on Android too", what, where[0] ? " (in " : "", where, where[0] ? ")" : "");
        }
        meter_end();
    }
    free(framework);
    free(res);
    J.end_status = why[0] ? -1 : 0;
    hk_strlcpy(J.end_reason, why[0] ? why : J.stop ? "stopped" : "", sizeof(J.end_reason));
    hk_atomic_xchg(&J.ended, 1);
    /* The reason is megler_last_error's too: the shell may ask after the app is gone. */
    if (why[0]) hk_run_set_error("%s", why);
    if (why[0]) hk_run_ended(-1, why);
    else hk_run_ended(0, J.stop ? "stopped" : NULL);
}

bool hk_java_ended(int *status, const char **reason)
{
    if (!J.ended) return false;
    if (status) *status = J.end_status;
    if (reason) *reason = J.end_reason;
    return true;
}

static bool java_start(const megler_run_config *config, const hk_apk_info *info)
{
    hk_stat st;
    char *probe;
    memset(&J, 0, sizeof(J));
    hk_lock_init(&J.lock);
    J.config = config;
    J.root = config->android_root && config->android_root[0] ? hk_strdup(config->android_root) : hk_sprintf("%s/android", megler_package_dir());
    hk_strlcpy(J.activity, info->activity, sizeof(J.activity));
    hk_strlcpy(J.application, info->application, sizeof(J.application));
    hk_strlcpy(J.package, info->package, sizeof(J.package));
    /* The battery the app starts with is not news to it; what it becomes afterwards is (battery_news). */
    J.battery_changes = hk_battery_changes();
    hk_battery_read(&J.battery);
    probe = hk_sprintf("%s/framework/core-oj.dex", J.root);
    if (!hk_path_stat(probe, &st, NULL)) {
        hk_run_set_error("this app is Java, and the files to run Java (Android's libcore and the framework) are not in %s", J.root);
        free(probe);
        return false;
    }
    free(probe);
    if (!J.activity[0]) {
        hk_run_set_error("the manifest names no launcher activity");
        return false;
    }
    /* The device as the NDK describes it to the app's native code (AConfiguration): this surface. */
    hk_configuration_set(config->width, config->height, config->density_dpi > 0 ? config->density_dpi : 160);
    /* A stack the size Android gives an app's main thread would be 8 MB; bytecode's frames are not on it, and 4 is room for libcore's native code. */
    J.main = hk_thread_start(main_thread, NULL, 4u << 20, "android-main");
    return J.main != NULL;
}

static void push_event(int phase, int id, i32 x, i32 y)
{
    hk_lock_acquire(&J.lock);
    if (J.nevents == MAX_EVENTS && phase == EV_MOVE) {
        /* A full queue means the main thread is busy: a move can be lost, what ends a gesture cannot. */
        hk_lock_release(&J.lock);
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
    hk_lock_release(&J.lock);
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
        hk_note_once("java: the keyboard sent something while no Java app runs: it is dropped");
        return;
    }
    item = hk_sprintf("%s%s", kind, utf8 ? utf8 : "");
    hk_lock_acquire(&J.lock);
    if (J.ntext < MAX_TEXT) {
        J.text[J.ntext++] = item;
        queued = true;
    }
    hk_lock_release(&J.lock);
    /* (Said before the event is queued, so that it comes before what the framework says of it.) */
    if (kind[0] != 'h' && hk_atomic_cas(&said, 0, 1) == 0)
        hk_info("java: the first thing the keyboard sent (%s) reached the core %s", kind[0] == 't' ? "text" : kind[0] == 'd' ? "a Backspace" : "an Enter",
                queued ? "and waits for the app's main thread" : "and is dropped: the app has not taken what was sent before");
    if (queued) push_event(EV_TEXT, 0, 0, 0);
    else free(item);                   /* the app has not taken five hundred of them: it is not listening */
}

void hk_java_text(const char *utf8)
{
    if (utf8 && utf8[0]) push_text("t", utf8);
}

void hk_java_text_key(int key)
{
    /* Android's key codes for the keys that go to the field as a hardware keyboard's would. */
    static const int codes[] = { 0, 0, 0, 112 /* FORWARD_DEL */, 21, 22, 19, 20 /* DPAD_LEFT, RIGHT, UP, DOWN */, 122, 123 /* MOVE_HOME, MOVE_END */ };
    if (key == MEGLER_TEXT_BACKSPACE) push_text("d", NULL);
    /*
     * Enter is a new line where the field said so when the keyboard was shown for it, and typed as one. (The framework's
     * own "e" would decide it again, and decides it the wrong way round: megler.InputMethods makes a new line only where
     * IME_FLAG_NO_ENTER_ACTION is not set, which is never in a field of several lines. The original's shell on iOS sends
     * the line end as text too.) Anywhere else it is the editor's action.
     */
    else if (key == MEGLER_TEXT_ENTER) push_text(J.enter_newline ? "t\n" : "e", NULL);
    else if (key >= MEGLER_TEXT_DELETE && key <= MEGLER_TEXT_END && java_runs()) {
        push_event(EV_KEY, codes[key], 1, 0);
        push_event(EV_KEY, codes[key], 0, 0);
    }
}

void hk_java_keyboard_closed(void) { push_text("h", NULL); }

void hk_java_keyboard_height(int pixels)
{
    if (!java_runs()) return;
    hk_atomic_xchg(&J.keyboard_height, pixels > 0 ? pixels : 0);
    push_event(EV_INSETS, 0, 0, 0);
}

static void java_stop(void)
{
    /* Before the main loop runs there is nobody to ask. */
    hk_atomic_cas(&J.stop, 0, 1);
}

const hk_driver hk_driver_java = { "java", java_start, java_touch, java_key, java_set_paused, NULL, java_stop };
