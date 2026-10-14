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
 * size, input, the end of the app. megler.Gfx, the drawing, is hk_java_gfx.c.
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

/*
 * pollInput(): what has happened since the last call, as int[1 + 4n] --
 * n, then (phase, id, x bits, y bits) for each -- or null. Called by the main
 * loop between messages, so this is also where a stop asked from outside
 * reaches the app: every activity is finished, on the thread they live on.
 */
N(Native_pollInput)
{
    hk_jobj *a = NULL;
    int n;
    if (J.stop == 1 && hk_atomic_cas(&J.stop, 1, 2) == 1) {
        hk_dvm_call_static("megler/AppRunner", "finishAll", "()V", NULL, NULL);
        if (hk_dvm_pending()) return;
    }
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
N(Native_startGL) { (void)c; hk_note_once("java: a GLSurfaceView's renderer is not started (OpenGL from Java is not there yet)"); }
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
    { "megler/Native", "packageName", NULL, Native_packageName },
    { "megler/Native", "screenWidth", NULL, Native_screenWidth },
    { "megler/Native", "screenHeight", NULL, Native_screenHeight },
    { "megler/Native", "density", NULL, Native_density },
    { "megler/Native", "startGL", NULL, Native_startGL },
    { "megler/Native", "requestRender", NULL, Native_nothing },
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
    { NULL, NULL, NULL, NULL },
};

/* ------------------------------------------- what is not answered yet */

/*
 * The framework's other native classes: sound, web views, sensors, video
 * decoding, and OpenGL ES from Java. None is written here yet. Each of their
 * methods answers zero, false or null, said once in the log for the class,
 * and the framework's own Java makes of that what an app can live with: a
 * sound that did not load is id 0, a sensor is not available, a codec not
 * supported, an EGL context EGL_NO_CONTEXT, a web view an empty rectangle.
 * The one that needs another answer than zero to say "no" is below.
 * (Databases, megler.Sqlite, are hk_java_sqlite.c.)
 */
N(Absent)
{
    /* (The native method's own frame is the top one: its class is what is missing.) */
    const hk_dvm_frame *f = hk_dvm_self()->top;
    hk_note_once("java: %s is not there yet: its native methods answer nothing", f && f->m ? f->m->cls->name : "a class of the framework");
    c->ret.j = 0;
}
/* An AudioTrack is initialised when its id is not negative. */
N(Absent_minus_one) { Absent(c); c->ret.j = 0; c->ret.i = -1; }

static const hk_jhle k_absent[] = {
    { "megler/Audio", "trackOpen", NULL, Absent_minus_one },
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

/* Gfx.present: the window's views, drawn. The shell is handed the picture; a frame is counted either way, so that a run with no shell can be followed. */
static void present(void *user, const void *pixels, int width, int height, int stride)
{
    (void)user;
    if (!pixels) return;
    if (J.config->window.present_pixels) J.config->window.present_pixels(J.config->window.user, pixels, width, height, stride);
    hk_run_count_frame();
    hk_atomic_add(&J.stats.frames, 1);
}

void hk_java_get_stats(hk_java_stats *out) { *out = J.stats; }

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
        hk_dvm_register(hk_java_gfx_natives());
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

static void push_text(const char *kind, const char *utf8)
{
    char *item;
    bool queued = false;
    if (!java_runs()) return;
    item = hk_sprintf("%s%s", kind, utf8 ? utf8 : "");
    hk_lock_acquire(&J.lock);
    if (J.ntext < MAX_TEXT) {
        J.text[J.ntext++] = item;
        queued = true;
    }
    hk_lock_release(&J.lock);
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
