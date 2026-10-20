/*
 * The NativeActivity driver.
 *
 * android.app.NativeActivity is the one activity Android itself implements for
 * apps with no Java: it loads the library its manifest names, calls
 * ANativeActivity_onCreate in it with an ANativeActivity -- callbacks for the
 * app to fill in, the data directories, the asset manager -- and from then on
 * tells the app about its life through those callbacks: start, resume, an
 * input queue, a window, focus. Nearly every such app is built on
 * android_native_app_glue, which answers each callback by passing a command
 * down a pipe to a thread of its own (android_main) and waiting for it.
 *
 * This file is that activity. A thread here plays Android's main thread: it
 * makes the calls NativeActivity.java would, in its order, and afterwards
 * waits for what the shell reports (paused, focus, stop).
 */
#include <stdio.h>

#include "../bionic/mg_bionic.h"
#include "../bionic/mg_fd.h"
#include "../host/mg_host.h"
#include "../jni/mg_jni_android.h"
#include "../ld/mg_ld.h"
#include "../ndk/mg_ndk.h"
#include "../run/mg_run.h"
#include "mg_driver.h"

/* android/native_activity.h, with pointers as the guest holds them. */
typedef struct g_callbacks {
    u32 onStart, onResume, onSaveInstanceState, onPause, onStop, onDestroy, onWindowFocusChanged, onNativeWindowCreated,
        onNativeWindowResized, onNativeWindowRedrawNeeded, onNativeWindowDestroyed, onInputQueueCreated, onInputQueueDestroyed,
        onContentRectChanged, onConfigurationChanged, onLowMemory;
} g_callbacks;

typedef struct g_activity {
    u32 callbacks, vm, env, clazz, internalDataPath, externalDataPath;
    i32 sdkVersion;
    u32 instance, assetManager, obbPath;
} g_activity;

enum { REQ_NONE = 0, REQ_PAUSE, REQ_RESUME, REQ_FOCUS, REQ_BLUR, REQ_STOP };

static struct {
    g_activity activity;
    g_callbacks callbacks;
    char internal[400], external[400], obb[400], library[128], entry[96];
    mg_nwindow *window;
    mg_input_queue *queue;
    mg_thread *main;
    mg_lock lock;
    mg_cond cond;
    int requests[32];
    int nrequests;
    bool paused, focused, finishing;
    volatile i32 started;              /* 0 starting, 1 running, -1 failed */
} N;

#define CALL0(slot) do { if (N.callbacks.slot) MG_GFN(void (*)(g_activity *), N.callbacks.slot)(&N.activity); } while (0)
#define CALL1(slot, arg) do { if (N.callbacks.slot) MG_GFN(void (*)(g_activity *, void *), N.callbacks.slot)(&N.activity, (void *)(arg)); } while (0)

static void request(int what)
{
    mg_lock_acquire(&N.lock);
    if (N.nrequests < (int)MG_COUNT(N.requests)) N.requests[N.nrequests++] = what;
    mg_cond_signal(&N.cond);
    mg_lock_release(&N.lock);
}

static void main_thread(void *arg)
{
    mg_lib *lib;
    void *entry;
    char name[160];
    (void)arg;
    snprintf(name, sizeof(name), "lib%s.so", N.library);
    lib = mg_ld_load(name);
    if (!lib) {
        mg_run_set_error("the app's library %s could not be loaded", name);
        mg_atomic_xchg(&N.started, -1);
        return;
    }
    mg_ld_init(lib);
    entry = mg_ld_sym(lib, N.entry);
    if (!entry) {
        mg_run_set_error("%s has no %s: it is not a NativeActivity library", name, N.entry);
        mg_atomic_xchg(&N.started, -1);
        return;
    }
    mg_info("native-activity: %s, %s", name, N.entry);

    /* NativeActivity.onCreate, then what the framework does to any activity that comes to the front. */
    MG_GFN(void (*)(g_activity *, void *, size_t), entry)(&N.activity, NULL, 0);
    mg_atomic_xchg(&N.started, 1);
    CALL0(onStart);
    CALL0(onResume);
    CALL1(onInputQueueCreated, N.queue);
    CALL1(onNativeWindowCreated, N.window);
    if (N.callbacks.onWindowFocusChanged) MG_GFN(void (*)(g_activity *, int), N.callbacks.onWindowFocusChanged)(&N.activity, 1);
    N.focused = true;

    for (;;) {
        int what;
        mg_lock_acquire(&N.lock);
        while (!N.nrequests) mg_cond_wait(&N.cond, &N.lock);
        what = N.requests[0];
        memmove(&N.requests[0], &N.requests[1], sizeof(int) * (size_t)--N.nrequests);
        mg_lock_release(&N.lock);
        if (what == REQ_PAUSE && !N.paused) {
            N.paused = true;
            CALL0(onPause);
        } else if (what == REQ_RESUME && N.paused) {
            N.paused = false;
            CALL0(onResume);
        } else if ((what == REQ_FOCUS || what == REQ_BLUR) && N.focused != (what == REQ_FOCUS)) {
            N.focused = what == REQ_FOCUS;
            if (N.callbacks.onWindowFocusChanged) MG_GFN(void (*)(g_activity *, int), N.callbacks.onWindowFocusChanged)(&N.activity, N.focused);
        } else if (what == REQ_STOP) {
            break;
        }
    }
    /* The way an activity is taken down: each of these returns when the app's own thread has let go. */
    if (!N.paused) CALL0(onPause);
    if (N.callbacks.onWindowFocusChanged) MG_GFN(void (*)(g_activity *, int), N.callbacks.onWindowFocusChanged)(&N.activity, 0);
    CALL1(onNativeWindowDestroyed, N.window);
    CALL1(onInputQueueDestroyed, N.queue);
    CALL0(onStop);
    CALL0(onDestroy);
    mg_run_ended(0, "stopped");
}

static bool na_start(const meglet_run_config *config, const mg_apk_info *info)
{
    char cls[220], *p;
    mg_jobj *activity;
    memset(&N, 0, sizeof(N));
    mg_lock_init(&N.lock);
    mg_cond_init(&N.cond);
    mg_strlcpy(N.library, info->native_library[0] ? info->native_library : "main", sizeof(N.library));
    mg_strlcpy(N.entry, info->func_name[0] ? info->func_name : "ANativeActivity_onCreate", sizeof(N.entry));

    mg_strlcpy(cls, info->activity[0] ? info->activity : "android.app.NativeActivity", sizeof(cls));
    for (p = cls; *p; p++)
        if (*p == '.') *p = '/';
    activity = mg_jni_android_setup(cls, info->package, config->apk_path, config->width, config->height, config->density_dpi);

    snprintf(N.internal, sizeof(N.internal), "%s/files", mg_fs_internal_dir());
    snprintf(N.external, sizeof(N.external), "%s/Android/data/%s/files", mg_fs_external_dir(), info->package);
    snprintf(N.obb, sizeof(N.obb), "%s/Android/obb/%s", mg_fs_external_dir(), info->package);
    for (p = N.internal;; p = N.external) {
        char *host = mg_fs_host_path(p);
        if (host) {
            mg_path_mkdirs(host);
            free(host);
        }
        if (p == N.external) break;
    }

    N.window = mg_nwindow_create(config->width, config->height);
    N.queue = mg_input_queue_create();
    N.activity.callbacks = (u32)(mg_addr)&N.callbacks;
    N.activity.vm = (u32)(mg_addr)mg_jni_vm();
    N.activity.env = (u32)(mg_addr)mg_jni_env();
    N.activity.clazz = (u32)(mg_addr)activity;
    N.activity.internalDataPath = (u32)(mg_addr)N.internal;
    N.activity.externalDataPath = (u32)(mg_addr)N.external;
    N.activity.sdkVersion = 28;
    N.activity.assetManager = (u32)(mg_addr)mg_asset_manager();
    N.activity.obbPath = (u32)(mg_addr)N.obb;

    N.main = mg_thread_start(main_thread, NULL, 2 * 1024 * 1024, "android-main");
    if (!N.main) {
        mg_run_set_error("no thread for the app");
        return false;
    }
    /* Up to the app's onCreate returning: loading a large library takes a while, and a failure there is a failure to start. */
    while (N.started == 0) mg_sleep_ns(5000000ull);
    return N.started > 0;
}

static void na_touch(int phase, int pointer_id, float x, float y) { mg_input_queue_touch(N.queue, phase, pointer_id, x, y); }
static void na_key(int keycode, bool down) { mg_input_queue_key(N.queue, keycode, down); }
static void na_set_paused(bool paused) { request(paused ? REQ_PAUSE : REQ_RESUME); }
static void na_set_focused(bool focused) { request(focused ? REQ_FOCUS : REQ_BLUR); }
static void na_stop(void) { request(REQ_STOP); }

const mg_driver mg_driver_native_activity = {
    "native-activity", na_start, na_touch, na_key, na_set_paused, na_set_focused, na_stop,
};

/* ------------------------------------------- what the app calls back with */

static void g_na_finish(g_activity *a)
{
    (void)a;
    if (!N.finishing) {
        N.finishing = true;
        mg_info("native-activity: the app asked to finish");
        request(REQ_STOP);
    }
}
static void g_na_set_window_format(g_activity *a, int format) { (void)a; (void)format; }
static void g_na_set_window_flags(g_activity *a, u32 add, u32 remove) { (void)a; (void)add; (void)remove; }
static void g_na_show_soft_input(g_activity *a, u32 flags)
{
    (void)a; (void)flags;
    mg_note_once("native-activity: the on-screen keyboard is not connected yet");
}
static void g_na_hide_soft_input(g_activity *a, u32 flags) { (void)a; (void)flags; }

const mg_sym mg_tab_native_activity[] = {
    MG_WRAP("ANativeActivity_finish", g_na_finish),
    MG_WRAP("ANativeActivity_setWindowFormat", g_na_set_window_format),
    MG_WRAP("ANativeActivity_setWindowFlags", g_na_set_window_flags),
    MG_WRAP("ANativeActivity_showSoftInput", g_na_show_soft_input),
    MG_WRAP("ANativeActivity_hideSoftInput", g_na_hide_soft_input),
    MG_END
};
