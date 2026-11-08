/*
 * The Unity driver: what com.unity3d.player.UnityPlayer and its activity do.
 *
 * A Unity game is three libraries. libmain.so is a loader of a few dozen
 * functions; libunity.so is the engine; libil2cpp.so (or libmono.so and
 * friends) is the game's own code. The Java side is thin: it loads libmain,
 * asks it to load the rest, hands the engine a Context and a Surface, and then
 * calls nativeRender on a thread of its own for ever. That is what this does,
 * in the order UnityPlayer's constructor and its thread do it. The engine's
 * questions to Java are answered by jni/mg_jni_android.c and by the classes
 * at the end of this file.
 *
 * The natives' names are stable across Unity versions; where a newer one moved
 * or changed them, both are tried.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../bionic/mg_bionic.h"
#include "../mg_core.h"
#include "../host/mg_host.h"
#include "../jni/mg_jni.h"
#include "../jni/mg_jni_android.h"
#include "../jni/mg_jni_loop.h"
#include "../jni/mg_jni_prefs.h"
#include "../ld/mg_ld.h"
#include "../ndk/mg_audio.h"
#include "../ndk/mg_egl.h"
#include "../ndk/mg_media.h"
#include "../ndk/mg_ndk.h"
#include "../run/mg_run.h"
#include "mg_driver.h"

#define PLAYER "com/unity3d/player/UnityPlayer"

/* The classes the engine's natives are registered on: UnityPlayer's own, and Unity 6's hosts of the player, which extend it. */
static const char *const k_players[] = { PLAYER, "com/unity3d/player/UnityPlayerForActivityOrService", "com/unity3d/player/UnityPlayerForGameActivity" };

static struct {
    mg_jobj *activity, *player, *surface;
    mg_nwindow *window;
    mg_thread *thread;
    volatile i32 started;              /* 0 starting, 1 running, -1 failed */
    volatile i32 stop, paused, focused;
    volatile i32 resized;              /* megler_set_surface: the render thread makes the engine's surface again before its next frame */
} U;

static void configuration_screen(int width, int height);

/*
 * A native of the player's on whichever of its classes this version registered it (Unity 6 registers the lifecycle natives
 * on the class that hosts the player in an activity); any signature where `sig` is NULL. NULL, and nothing said, where it has none.
 */
static void *player_native(const char *name, const char *sig)
{
    void *fn = NULL;
    size_t i;
    for (i = 0; i < MG_COUNT(k_players) && !fn; i++) fn = mg_jni_native(k_players[i], name, sig);
    return fn;
}

/* A native of the player's, wherever this version of Unity registered it; NULL, said once, when it has none. */
static void *native_of(const char *cls, const char *name, const char *sig)
{
    void *fn = !strcmp(cls, PLAYER) ? player_native(name, sig) : mg_jni_native(cls, name, sig);
    if (!fn) mg_note_once("unity: the engine has no %s.%s%s", cls, name, sig);
    return fn;
}

static mg_jvalue call_on(const char *cls, mg_jobj *self, const char *name, const char *sig, const mg_jvalue *args)
{
    void *fn = native_of(cls, name, sig);
    mg_jvalue none;
    none.j = 0;
    return fn ? mg_jni_call_native(fn, self, sig, args) : none;
}

static mg_jvalue call(const char *name, const char *sig, const mg_jvalue *args) { return call_on(PLAYER, U.player, name, sig, args); }

static void call_bool(const char *name, bool value)
{
    mg_jvalue a;
    a.j = 0;
    a.z = value;
    call(name, "(Z)V", &a);
}

static void in_deliver(void);

static bool load_engine(void)
{
    mg_lib *main_lib = mg_ld_load("libmain.so");
    void *onload, *load;
    mg_jvalue arg, ok;
    if (!main_lib) {
        mg_run_set_error("the game's libmain.so could not be loaded");
        return false;
    }
    mg_ld_init(main_lib);
    onload = mg_ld_sym(main_lib, "JNI_OnLoad");
    if (!onload) {
        mg_run_set_error("libmain.so has no JNI_OnLoad: it is not Unity's loader");
        return false;
    }
    mg_info("unity: libmain.so, JNI_OnLoad -> 0x%x", (unsigned)MG_GFN(i32 (*)(void *, void *), onload)(mg_jni_vm(), NULL));

    /* NativeLoader.load(directory): libmain opens libunity.so, and the game's code, from there. */
    load = mg_jni_native("com/unity3d/player/NativeLoader", "load", "(Ljava/lang/String;)Z");
    if (!load) {
        mg_run_set_error("libmain.so registered no NativeLoader.load: this version of Unity starts another way");
        return false;
    }
    arg.j = 0;
    arg.l = mg_jni_new_string(mg_jni_android_lib_dir());
    ok = mg_jni_call_native(load, mg_jni_class_object("com/unity3d/player/NativeLoader"), "(Ljava/lang/String;)Z", &arg);
    mg_jni_unref(arg.l);
    mg_info("unity: NativeLoader.load -> %d", (int)ok.z);
    if (!ok.z) {
        mg_run_set_error("Unity's loader could not load the engine (libunity.so and the game's code)");
        return false;
    }
    return true;
}

/*
 * The player is an object of the class its natives are on: UnityPlayer's, or in Unity 6 that of the host the player is made
 * for (UnityPlayerForActivityOrService, UnityPlayerForGameActivity), which extends it: what the engine finds the player's
 * class to be is what Java's would be.
 */
static void player_class(void)
{
    size_t i;
    for (i = 1; i < MG_COUNT(k_players); i++)
        if (!mg_jni_native_sig(PLAYER, "nativeRender") && mg_jni_native_sig(k_players[i], "nativeRender")) {
            U.player->cls = mg_jni_class(k_players[i], PLAYER);
            mg_info("unity: the engine's natives are %s's: the player is one", k_players[i]);
            return;
        }
}

/* Whether a class named in a signature is a Context (an Activity, the app's own activity among them) or a view (the player). */
static bool is_context_class(const char *name, size_t n)
{
    static const char *const kinds[] = { "android/content/Context", "android/content/ContextWrapper", "android/view/ContextThemeWrapper", "android/app/Activity" };
    size_t i;
    for (i = 0; i < MG_COUNT(kinds); i++)
        if (n == strlen(kinds[i]) && !strncmp(name, kinds[i], n)) return true;
    return n > 8 && !strncmp(name + n - 8, "Activity", 8);
}
static bool is_view_class(const char *name, size_t n)
{
    static const char *const kinds[] = { "android/view/View", "android/view/ViewGroup", "android/widget/FrameLayout" };
    size_t i;
    for (i = 0; i < MG_COUNT(kinds); i++)
        if (n == strlen(kinds[i]) && !strncmp(name, kinds[i], n)) return true;
    return n >= strlen(PLAYER) && !strncmp(name, PLAYER, strlen(PLAYER));
}

/*
 * initJni, the end of UnityPlayer's constructor: the engine keeps the player and the Context, which it asks Java through for
 * as long as it runs. Each version has declared it its own way -- (Context) up to 2022, (Context, int) in Unity 6.0 -- and on
 * one of three classes, and a version that declared it otherwise was never called: WOW (Unity 6) went on with no player and no
 * Context, called UnityPlayer's executeMainThreadJobs, initializeGoogleAr and shouldSetGameState on null (the log's
 * "java/lang/Object...is not implemented": the class of null), and ended in strlen(NULL) from libunity.so as its crash handler
 * set itself up. So it is called as the engine registered it, whatever its parameters, with what UnityPlayer passes: the
 * Context for a Context or an activity, the player for a view, 0 for a number (Unity 6's kind of host: 0, an activity), false,
 * null for anything else. Where the engine registered none, the log says which natives it did register.
 */
static void init_jni(void)
{
    mg_jvalue a[8];
    const char *sig = NULL, *cls = PLAYER, *p;
    void *fn = NULL;
    int n = 0;
    size_t i;
    for (i = 0; i < MG_COUNT(k_players) && !fn; i++) {
        sig = mg_jni_native_sig(k_players[i], "initJni");
        if (sig) {
            cls = k_players[i];
            fn = mg_jni_native(cls, "initJni", sig);
        }
    }
    if (!fn) {
        /* (Exported under its JNI name instead, which says no parameters: the Context alone, as before Unity 6.) */
        sig = "(Landroid/content/Context;)V";
        fn = mg_jni_native(PLAYER, "initJni", sig);
    }
    if (!fn) {
        for (i = 0; i < MG_COUNT(k_players); i++) {
            char said[1500];
            if (mg_jni_natives_said(k_players[i], said, sizeof(said)))
                mg_warn("unity: the engine registered no initJni; %s's natives: %s", k_players[i], said);
        }
        mg_note_once("unity: no initJni: the engine has no player and no Context, and the game may not get far");
        return;
    }
    memset(a, 0, sizeof(a));
    for (p = sig + 1; *p && *p != ')' && n < (int)MG_COUNT(a); n++) {
        if (*p == '[') {
            while (*p == '[') p++;
            p = *p == 'L' && strchr(p, ';') ? strchr(p, ';') + 1 : p + (*p ? 1 : 0);
        } else if (*p == 'L') {
            const char *end = strchr(p, ';');
            size_t len = end ? (size_t)(end - p - 1) : 0;
            if (is_context_class(p + 1, len)) a[n].l = U.activity;
            else if (is_view_class(p + 1, len)) a[n].l = U.player;
            p = end ? end + 1 : p + strlen(p);
        } else {
            p++;
        }
    }
    mg_jni_call_native(fn, U.player, sig, a);
    mg_info("unity: initJni%s done (%s's)", sig, cls);
}

/* What the UI thread is to do as the app starts, and what came of it. */
typedef struct ui_start {
    volatile i32 done;
    bool ok;
} ui_start;

static void ui_start_engine(void *arg)
{
    ui_start *s = (ui_start *)arg;
    mg_jvalue a[2];
    s->ok = load_engine();
    if (s->ok) {
        player_class();
        init_jni();
        a[0].j = a[1].j = 0;
        a[0].l = mg_jni_class_object("com/unity3d/player/UnityWebRequest");
        if (player_native("nativeInitWebRequest", NULL)) call("nativeInitWebRequest", "(Ljava/lang/Class;)V", a);
    }
    mg_atomic_xchg(&s->done, 1);
    mg_futex_wake(&s->done, true);
}

static MG_HOSTCB void unity_main(void *arg)
{
    mg_jvalue a[2];
    void *render;
    bool was_paused = false, was_focused = true;
    u64 next_note = 0;
    unsigned calls = 0, notes = 0;
    (void)arg;
    if (mg_app_threads_give_way()) mg_thread_runs_app_code();
    /* This is UnityPlayer's main thread, which has a Looper on Android: a native looper for the asking, its descriptors' callbacks run before each frame. */
    mg_looper_java_thread();
    {
        /*
         * UnityPlayer's constructor runs on the main ("UI") thread: the engine loaded (libmain's JNI_OnLoad, and NativeLoader.load,
         * which loads libunity.so and the game's code and runs their JNI_OnLoad), initJni, the web requests' class. Here that is the
         * main looper's thread (jni/mg_jni_loop.c), which has a native looper as Android's has: the engine takes the UI thread's
         * looper as it loads, and runs its jobs for the UI thread through it; loaded on this thread, which had none, it said
         * "Couldn't retrieve native ALooper for UI thread". The flag unity-load-here loads it on this thread, as before.
         */
        ui_start s;
        memset(&s, 0, sizeof(s));
        if (mg_flag("unity-load-here")) ui_start_engine(&s);
        else mg_jni_loop_call(NULL, ui_start_engine, &s);
        while (!s.done) mg_gwait(&s.done, 0, 100000000ull);
        if (!s.ok) {
            mg_atomic_xchg(&U.started, -1);
            return;
        }
    }

    /* What UnityPlayer queues for this thread once it has a surface, in the order it queues it. */
    a[0].j = 0;
    a[0].i = 0;
    a[1].l = U.surface;
    const char *surface_sig = "(ILandroid/view/Surface;)V";
    void *surface = native_of(PLAYER, "nativeRecreateGfxState", surface_sig);
    bool legacy_surface = !surface;
    if (legacy_surface) {
        surface_sig = "(Landroid/view/Surface;)V";
        surface = native_of(PLAYER, "nativeRecreateGfxState", surface_sig);
    }
    if (!surface) {
        mg_run_set_error("Unity did not register a supported surface callback");
        mg_atomic_xchg(&U.started, -1);
        return;
    }
    mg_atomic_xchg(&U.started, 1);
    mg_jni_call_native(surface, U.player, surface_sig, legacy_surface ? a + 1 : a);
    if (legacy_surface) mg_info("unity: surface attached through the legacy signature");
    if (player_native("nativeSendSurfaceChangedEvent", NULL)) call("nativeSendSurfaceChangedEvent", "()V", NULL);
    call("nativeResume", "()V", NULL);
    call_bool("nativeFocusChanged", true);
    mg_info("unity: resumed and focused; rendering");

    render = native_of(PLAYER, "nativeRender", "()Z");
    /* The flag "late-render": the same, eight seconds later. */
    if (mg_flag("late-render")) {
        mg_info("unity: waiting eight seconds before the first frame is asked for");
        mg_sleep_ns(8000000000ull);
    }
    /* The flag "no-render": the engine is loaded and told where it is, and then left alone. */
    if (mg_flag("no-render")) {
        while (!U.stop) mg_sleep_ns(100000000ull);
        render = NULL;
    }
    while (render && !U.stop) {
        bool paused = U.paused != 0, focused = U.focused != 0;
        mg_jvalue keep;
        /* How it is going, a line a second for the first minute and one every ten seconds after: frames, and the memory the process holds. */
        {
            u64 now = mg_time_monotonic_ns();
            if (now >= next_note) {
                u32 free_mb = 0, largest = 0, reserved = 0, committed = 0;
                mg_host_address_space(&free_mb, &largest, &reserved, &committed);
                mg_info("unity: %u frames asked for, %u shown; %u MB in use of %u; address space %u MB free (largest stretch %u), %u reserved, %u committed",
                        calls, (unsigned)mg_run_frames(), (unsigned)(mg_mem_usage() >> 20), (unsigned)(mg_mem_limit() >> 20), (unsigned)free_mb,
                        (unsigned)largest, (unsigned)reserved, (unsigned)committed);
                {
                    char textures[320];
                    mg_gltex_report(textures, sizeof(textures));
                    mg_info("unity: %s", textures);
                }
                next_note = now + (++notes < 60 ? 1000000000ull : 10000000000ull);
            }
            calls++;
        }
        if (focused != was_focused && !paused) {
            call_bool("nativeFocusChanged", focused);
            was_focused = focused;
        }
        if (paused) {
            if (!was_paused) {
                if (was_focused) call_bool("nativeFocusChanged", false);
                call("nativePause", "()Z", NULL);
                was_paused = true;
                was_focused = false;
                mg_info("unity: paused");
            }
            mg_sleep_ns(20000000ull);
            continue;
        }
        if (was_paused) {
            call("nativeResume", "()V", NULL);
            was_paused = false;
            mg_info("unity: resumed");
            continue;
        }
        if (U.resized && mg_atomic_xchg(&U.resized, 0)) {
            /*
             * The surface is another size or turned: what UnityPlayer does when its SurfaceView's surface changes -- the engine
             * given its surface again (nativeRecreateGfxState), which makes its EGL window surface again at the window's new
             * size, and told it changed -- with the display, the metrics and the configuration it asks saying the new size.
             */
            mg_jvalue g[2];
            mg_surface sf;
            mg_run_surface(&sf);
            mg_jni_android_set_screen(sf.width, sf.height, sf.density_dpi, sf.rotation);
            /* (Turned upside down, the surface is the same size: only the display's rotation says it, as on Android.) */
            if (sf.width != U.window->width || sf.height != U.window->height) {
                configuration_screen(sf.width, sf.height);
                U.window->width = sf.width;
                U.window->height = sf.height;
                mg_info("unity: the surface is %dx%d now, turned %d degrees: the engine's graphics state is made again", sf.width, sf.height, sf.rotation * 90);
                g[0].j = g[1].j = 0;
                g[0].i = 0;
                g[1].l = U.surface;
                mg_jni_call_native(surface, U.player, surface_sig, legacy_surface ? g + 1 : g);
                if (player_native("nativeSendSurfaceChangedEvent", NULL)) call("nativeSendSurfaceChangedEvent", "()V", NULL);
            }
        }
        in_deliver();
        /* (What the engine watches on this thread's looper: its callbacks, as the thread's Looper would run them between messages.) */
        mg_looper_poll_thread();
        keep = mg_jni_call_native(render, U.player, "()Z", NULL);
        if (!keep.z) {
            mg_info("unity: nativeRender returned false: the game asked to quit");
            break;
        }
    }
    if (!was_paused) {
        if (was_focused) call_bool("nativeFocusChanged", false);
        call("nativePause", "()Z", NULL);
    }
    if (player_native("nativeDone", NULL)) call("nativeDone", "()Z", NULL);
    mg_run_ended(0, "stopped");
}

/* ------------------------------------------------- the Java the engine calls */

static void ret_void(mg_jcall *c) { (void)c; }
static void ret_false(mg_jcall *c) { c->ret.z = 0; }
static void ret_true(mg_jcall *c) { c->ret.z = 1; }
static void ret_zero(mg_jcall *c) { c->ret.j = 0; }
static void ret_null(mg_jcall *c) { c->ret.l = NULL; }
static void player_getContext(mg_jcall *c) { c->ret.l = U.activity; }
static void ret_self(mg_jcall *c) { c->ret.l = mg_jni_ref(c->self); }

/* The engine's own way of saying it cannot go on is a dialog; here it is a line in the log. */
static void dialog_text(mg_jcall *c)
{
    const char *text = mg_jni_string(c->args[0].l);
    mg_warn("unity: the game shows a dialog, %s: %s", c->name + 3, text ? text : "(not a string)");
    c->ret.l = mg_jni_ref(c->self);
}

static const mg_jhle g_unity[] = {
    /* What the engine asks the player object itself. */
    { PLAYER, "getContext", NULL, player_getContext },
    { PLAYER, "showSoftInput", NULL, ret_void },
    { PLAYER, "hideSoftInput", NULL, ret_void },
    { PLAYER, "setSoftInputStr", NULL, ret_void },
    { PLAYER, "reportSoftInputStr", NULL, ret_void },
    { PLAYER, "setHideInputField", NULL, ret_void },
    { PLAYER, "setSelection", NULL, ret_void },
    { PLAYER, "setCharacterLimit", NULL, ret_void },
    { PLAYER, "showVideoPlayer", NULL, ret_false },
    { PLAYER, "isUaaLUseCase", NULL, ret_false },
    { PLAYER, "skipPermissionsDialog", NULL, ret_true },
    { PLAYER, "requestUserAuthorization", NULL, ret_void },
    { PLAYER, "addPhoneCallListener", NULL, ret_void },
    { PLAYER, "getNetworkConnectivity", NULL, ret_zero },
    { PLAYER, "getSplashMode", NULL, ret_zero },
    { PLAYER, "executeGLThreadJobs", NULL, ret_void },
    { PLAYER, "disableLogger", NULL, ret_void },
    { PLAYER, "kill", NULL, ret_void },
    { PLAYER, "beginIndeterminateProgress", NULL, ret_void },
    { PLAYER, "initializeGoogleAr", NULL, ret_false },
    { PLAYER, "initializeGoogleVr", NULL, ret_false },
    /* Android 13's game state hints (GameManager): none on the Android 9 the engine is told it runs on. */
    { PLAYER, "shouldSetGameState", NULL, ret_false },
    { PLAYER, "loadLibrary", NULL, ret_true },
    { PLAYER, "getLaunchURL", NULL, ret_null },
    { PLAYER, "getClipboardText", NULL, ret_null },
    { PLAYER, "setClipboardText", NULL, ret_void },
    { "android/app/AlertDialog$Builder", "setTitle", NULL, dialog_text },
    { "android/app/AlertDialog$Builder", "setMessage", NULL, dialog_text },
    { "android/app/AlertDialog$Builder", "setPositiveButton", NULL, ret_self },
    { "android/app/AlertDialog$Builder", "setNegativeButton", NULL, ret_self },
    { "android/app/AlertDialog$Builder", "setCancelable", NULL, ret_self },
    { "android/app/AlertDialog$Builder", "create", NULL, ret_self },
    { "android/app/AlertDialog$Builder", "show", NULL, ret_self },
    { NULL, NULL, NULL, NULL },
};

/* ------------------------------------------- what the engine asks of Android
 *
 * The questions Unity's native side has been seen to ask Java, answered the
 * way a phone with nothing unusual about it would. Anything not here logs
 * once and returns zero (jni/mg_jni.c).
 */
static mg_jobj *keep(mg_jobj *o)
{
    o->permanent = true;
    return o;
}

static const char *arg_text(mg_jcall *c, int i)
{
    const char *s = mg_jni_string(c->args[i].l);
    return s ? s : "";
}

/* The manifest's <meta-data>, kept for the Bundle the engine reads its settings from. */
static struct {
    char name[96], value[160];
} g_meta[48];
static int g_nmeta;
static char g_version_name[64];
static int g_version_code;

static const char *meta_value(const char *name)
{
    int i;
    for (i = 0; i < g_nmeta; i++)
        if (!strcmp(g_meta[i].name, name)) return g_meta[i].value;
    return NULL;
}

static void locale_tag(mg_jcall *c) { c->ret.l = mg_jni_new_string(mg_locale_tag()); }
static void ret_first_string(mg_jcall *c) { c->ret.l = mg_jni_new_string(arg_text(c, 0)); }
static void ret_one(mg_jcall *c) { c->ret.i = 1; }
static void ret_empty_ints(mg_jcall *c) { c->ret.l = mg_jni_new_prim_array('I', 0); }

/*
 * AudioManager. FMOD asks the device's own rate and buffer length before it
 * opens OpenSL ES, and gives up ("Error initializing output device") when it
 * is told nothing: the mixer's rate, so that nothing is resampled twice, and
 * a buffer that the mixer's own period comfortably holds.
 */
static void audio_property(mg_jcall *c)
{
    const char *key = arg_text(c, 0);
    static volatile i32 said;
    const char *value = strstr(key, "SAMPLE_RATE") ? "44100" : strstr(key, "FRAMES_PER_BUFFER") ? "512" : NULL;
    if (mg_atomic_add(&said, 1) <= 6) mg_info("unity: AudioManager.getProperty(%s) -> %s", key, value ? value : "null");
    c->ret.l = value ? mg_jni_new_string(value) : NULL;
}
static void audio_devices(mg_jcall *c) { c->ret.l = mg_jni_new_object_array("android/media/AudioDeviceInfo", 0); }
static void audio_volume(mg_jcall *c) { c->ret.i = 10; }
static void audio_max_volume(mg_jcall *c) { c->ret.i = 15; }

static void display_of(mg_jcall *c) { c->ret.l = mg_jni_android_display(); }
static void display_all(mg_jcall *c)
{
    mg_jobj *a = mg_jni_new_object_array("android/view/Display", 1);
    a->u.oarr.v[0] = mg_jni_android_display();
    c->ret.l = a;
}

/* StringBuilder, as far as native code builds strings with it. */
static void sb_append(mg_jcall *c)
{
    char *old = (char *)c->self->native, number[32];
    const char *more = "";
    if (c->sig[1] == 'L') more = arg_text(c, 0);
    else if (c->sig[1] == 'C') {
        number[0] = (char)c->args[0].c;
        number[1] = 0;
        more = number;
    } else if (c->sig[1] == 'J') {
        snprintf(number, sizeof(number), "%lld", (long long)c->args[0].j);
        more = number;
    } else if (c->sig[1] == 'I' || c->sig[1] == 'Z') {
        snprintf(number, sizeof(number), "%d", c->sig[1] == 'Z' ? (int)c->args[0].z : c->args[0].i);
        more = number;
    }
    c->self->native = mg_sprintf("%s%s", old ? old : "", more);
    free(old);
    c->ret.l = mg_jni_ref(c->self);
}
static void sb_to_string(mg_jcall *c) { c->ret.l = mg_jni_new_string(c->self->native ? (const char *)c->self->native : ""); }

/* An asset as a java.io.InputStream, and the Scanner idiom that reads one whole: new Scanner(in).useDelimiter("\\A").next(). */
typedef struct in_stream {
    u8 *data;
    u32 size, pos;
} in_stream;

static void assets_open(mg_jcall *c)
{
    void *(*open)(void *, const char *, int) = (void *(*)(void *, const char *, int))mg_bionic_find("AAssetManager_open");
    int (*length)(void *) = (int (*)(void *))mg_bionic_find("AAsset_getLength");
    int (*read)(void *, void *, u32) = (int (*)(void *, void *, u32))mg_bionic_find("AAsset_read");
    void (*close)(void *) = (void (*)(void *))mg_bionic_find("AAsset_close");
    void *asset = open && length && read && close ? open(mg_asset_manager(), arg_text(c, 0), 0) : NULL;
    in_stream *s;
    mg_jobj *o;
    if (!asset) {
        mg_info("unity: the engine asked Java for the asset %s, which is not there", arg_text(c, 0));
        mg_jni_throw("java/io/FileNotFoundException", arg_text(c, 0));
        return;
    }
    s = (in_stream *)mg_alloc(sizeof(*s));
    s->size = (u32)length(asset);
    s->data = (u8 *)mg_alloc(s->size + 1);
    read(asset, s->data, s->size);
    close(asset);
    o = mg_jni_new_object("java/io/InputStream");
    o->native = s;
    c->ret.l = o;
}
static void stream_available(mg_jcall *c)
{
    in_stream *s = (in_stream *)c->self->native;
    c->ret.i = s ? (i32)(s->size - s->pos) : 0;
}
static void stream_read(mg_jcall *c)
{
    in_stream *s = (in_stream *)c->self->native;
    mg_jobj *a;
    u32 off = 0, want, left;
    if (!s || s->pos >= s->size) {
        c->ret.i = -1;
        return;
    }
    if (!strcmp(c->sig, "()I")) {
        c->ret.i = s->data[s->pos++];
        return;
    }
    a = c->args[0].l;
    if (!a || a->kind != MG_J_PRIM_ARRAY) return;
    want = a->u.arr.len;
    if (!strcmp(c->sig, "([BII)I")) {
        off = (u32)c->args[1].i;
        want = (u32)c->args[2].i;
    }
    if (off > a->u.arr.len) off = a->u.arr.len;
    if (want > a->u.arr.len - off) want = a->u.arr.len - off;
    left = s->size - s->pos;
    if (want > left) want = left;
    memcpy((u8 *)a->u.arr.data + off, s->data + s->pos, want);
    s->pos += want;
    c->ret.i = (i32)want;
}
static void scanner_init(mg_jcall *c) { c->self->native = c->args[0].l ? c->args[0].l->native : NULL; }
static void scanner_has_next(mg_jcall *c)
{
    in_stream *s = (in_stream *)c->self->native;
    c->ret.z = s && s->pos < s->size;
}
static void scanner_next(mg_jcall *c)
{
    in_stream *s = (in_stream *)c->self->native;
    if (!s || s->pos >= s->size) {
        c->ret.l = mg_jni_new_string("");
        return;
    }
    s->data[s->size] = 0;
    c->ret.l = mg_jni_new_string((const char *)s->data + s->pos);
    s->pos = s->size;
}

/* The package manager, as far as it describes the app itself. */
static mg_jobj *g_package_manager, *g_package_info, *g_resources, *g_configuration, *g_bundle;

static void ctx_package_manager(mg_jcall *c) { c->ret.l = g_package_manager; }
static void pm_package_info(mg_jcall *c) { c->ret.l = g_package_info; }
static void pm_application_info(mg_jcall *c) { c->ret.l = mg_jni_android_app_info(); }
static void pm_installer(mg_jcall *c) { c->ret.l = mg_jni_new_string("com.android.vending"); }
static void bundle_get_boolean(mg_jcall *c)
{
    const char *v = meta_value(arg_text(c, 0));
    c->ret.z = v ? (!strcmp(v, "true") || !strcmp(v, "1")) : (!strcmp(c->sig, "(Ljava/lang/String;Z)Z") ? c->args[1].z : 0);
}
static void bundle_get_int(mg_jcall *c)
{
    const char *v = meta_value(arg_text(c, 0));
    c->ret.i = v ? atoi(v) : (!strcmp(c->sig, "(Ljava/lang/String;I)I") ? c->args[1].i : 0);
}
static void bundle_get_string(mg_jcall *c)
{
    const char *v = meta_value(arg_text(c, 0));
    c->ret.l = v ? mg_jni_new_string(v) : NULL;
}
static void bundle_contains(mg_jcall *c) { c->ret.z = meta_value(arg_text(c, 0)) != NULL; }

/* (SharedPreferences, which is where PlayerPrefs live, are mg_jni_prefs.c's.) */
static void ret_self(mg_jcall *c);

static void ctx_resources(mg_jcall *c) { c->ret.l = g_resources; }
static void res_configuration(mg_jcall *c) { c->ret.l = g_configuration; }
/* The configuration's orientation as the surface is shaped (the render thread, after megler_set_surface). */
static void configuration_screen(int width, int height)
{
    mg_jvalue v;
    if (!g_configuration) return;
    v.j = 0;
    v.i = width > height ? 2 : 1;
    mg_jni_set_field(g_configuration, "orientation", 'I', v);
}
static void res_metrics(mg_jcall *c)
{
    mg_jobj *dm = mg_jni_new_object("android/util/DisplayMetrics");
    mg_jni_android_fill_metrics(dm);
    c->ret.l = dm;
}
static void res_text(mg_jcall *c) { c->ret.l = mg_jni_new_string(""); }
static void window_attributes(mg_jcall *c) { c->ret.l = keep(mg_jni_new_object("android/view/WindowManager$LayoutParams")); }

static void ctx_obb_dirs(mg_jcall *c)
{
    char path[400];
    mg_jobj *a = mg_jni_new_object_array("java/io/File", 1), *f = mg_jni_new_object("java/io/File");
    snprintf(path, sizeof(path), "%s/Android/obb/%s", mg_fs_external_dir(), mg_fs_package());
    f->native = mg_strdup(path);
    a->u.oarr.v[0] = f;
    c->ret.l = a;
}

static void process_tid(mg_jcall *c) { c->ret.i = mg_gthread_self()->tid; }

/* ------------------------------------------------------------ FMOD's output
 *
 * Unity's sound is FMOD, and on Android FMOD plays through a small Java
 * class, org.fmod.FMODAudioDevice: a thread that asks the native mixer for
 * the next block (fmodProcess fills a direct buffer with 16-bit stereo) and
 * writes it to an AudioTrack, whose write takes as long as the block takes to
 * play. The thread is here, and the blocks go to this layer's mixer;
 * holding back while a few are queued is what paces FMOD.
 */
enum { FMOD_RING = 8 };
static struct {
    mg_thread *thread;
    volatile i32 running;
    mg_jobj *self;
} FM;

static MG_HOSTCB void fmod_main(void *arg)
{
    void *info = mg_jni_native("org/fmod/FMODAudioDevice", "fmodGetInfo", "(I)I");
    void *process = mg_jni_native("org/fmod/FMODAudioDevice", "fmodProcess", "(Ljava/nio/ByteBuffer;)I");
    int rate = 0, frames = 0, tries, k = 0;
    i16 *block, *ring[FMOD_RING];
    mg_jobj *buffer;
    mg_voice *voice;
    bool said = false;
    (void)arg;
    if (!info || !process) {
        mg_warn("fmod: FMODAudioDevice has no natives; no sound");
        return;
    }
    /* 0 is the mixer's rate, 1 the length of a block in frames; they are there once FMOD has started its mixer. */
    for (tries = 0; FM.running && tries < 500; tries++) {
        mg_jvalue which, got;
        which.j = 0;
        which.i = 0;
        got = mg_jni_call_native(info, FM.self, "(I)I", &which);
        rate = got.i;
        which.i = 1;
        got = mg_jni_call_native(info, FM.self, "(I)I", &which);
        frames = got.i;
        if (rate > 0 && frames > 0) break;
        mg_sleep_ns(10000000ull);
    }
    if (rate <= 0 || frames <= 0 || frames > 65536) {
        mg_warn("fmod: the mixer never said its rate and block length (%d Hz, %d frames); no sound", rate, frames);
        return;
    }
    voice = mg_voice_create(rate, 2, 16, false);
    if (!voice) {
        mg_warn("fmod: no voice for %d Hz stereo; no sound", rate);
        return;
    }
    block = (i16 *)mg_alloc((size_t)frames * 4);
    for (tries = 0; tries < FMOD_RING; tries++) ring[tries] = (i16 *)mg_alloc((size_t)frames * 4);
    buffer = mg_jni_new_direct_buffer(block, (i64)frames * 4);
    buffer->permanent = true;
    mg_voice_set_playing(voice, true);
    mg_info("fmod: FMODAudioDevice playing, %d Hz stereo in blocks of %d frames", rate, frames);
    while (FM.running) {
        mg_jvalue a, r;
        a.j = 0;
        a.l = buffer;
        r = mg_jni_call_native(process, FM.self, "(Ljava/nio/ByteBuffer;)I", &a);
        if (r.i != 0) {
            if (!said) mg_warn("fmod: fmodProcess returned %d, not FMOD_OK; waiting for it", r.i);
            said = true;
            mg_sleep_ns(2000000ull);
            continue;
        }
        /* The voice plays from the memory it is given: each block stays in the ring until it is long past. */
        memcpy(ring[k], block, (size_t)frames * 4);
        while (FM.running && !mg_voice_enqueue(voice, ring[k], (u32)frames * 4)) mg_sleep_ns(1000000ull);
        k = (k + 1) % FMOD_RING;
        while (FM.running && mg_voice_queued(voice) >= 3) mg_sleep_ns(1000000ull);
    }
    mg_voice_set_playing(voice, false);
}

static void fmod_start(mg_jcall *c)
{
    if (mg_atomic_xchg(&FM.running, 1)) return;
    FM.self = c->self;
    FM.thread = mg_thread_start(fmod_main, NULL, 1024 * 1024, "FMODAudioDevice");
    if (!FM.thread) mg_atomic_xchg(&FM.running, 0);
}
static void fmod_stop(mg_jcall *c)
{
    mg_thread *t = FM.thread;
    (void)c;
    if (!mg_atomic_xchg(&FM.running, 0) || !t) return;
    FM.thread = NULL;
    mg_thread_join(t);
}
static void fmod_running(mg_jcall *c) { c->ret.z = FM.running != 0; }

#define AUDIO "megler/service/audio"
#define BUNDLE "android/os/Bundle"
static const mg_jhle g_unity_java[] = {
    { "java/util/Locale", "toLanguageTag", NULL, locale_tag },
    { "android/net/Uri", "encode", NULL, ret_first_string },
    { "android/net/Uri", "decode", NULL, ret_first_string },
    { "android/os/Process", "setThreadPriority", NULL, ret_void },
    { "android/os/Process", "myTid", NULL, process_tid },
    { AUDIO, "getProperty", NULL, audio_property },
    { AUDIO, "getDevices", NULL, audio_devices },
    { AUDIO, "getStreamVolume", NULL, audio_volume },
    { AUDIO, "getStreamMaxVolume", NULL, audio_max_volume },
    { AUDIO, "requestAudioFocus", NULL, ret_one },
    { AUDIO, "abandonAudioFocus", NULL, ret_one },
    { AUDIO, "getMode", NULL, ret_zero },
    { AUDIO, "getRingerMode", NULL, ret_one },
    { AUDIO, "isMusicActive", NULL, ret_false },
    { AUDIO, "isBluetoothScoOn", NULL, ret_false },
    { AUDIO, "isBluetoothA2dpOn", NULL, ret_false },
    { AUDIO, "isWiredHeadsetOn", NULL, ret_false },
    { AUDIO, "isSpeakerphoneOn", NULL, ret_true },
    { AUDIO, "registerAudioDeviceCallback", NULL, ret_void },
    { AUDIO, "unregisterAudioDeviceCallback", NULL, ret_void },
    { "android/view/Display", "getDisplayId", NULL, ret_zero },
    { "android/view/Display", "isWideColorGamut", NULL, ret_false },
    { "android/view/Display", "isHdr", NULL, ret_false },
    { "android/view/Display", "getAppVsyncOffsetNanos", NULL, ret_zero },
    { "android/view/Display", "getPresentationDeadlineNanos", NULL, ret_zero },
    { "android/view/Display", "getSupportedModes", NULL, ret_null },
    { "android/view/Display", "getMode", NULL, ret_null },
    { "android/view/Display", "getCutout", NULL, ret_null },
    { "android/content/Intent", "<init>", NULL, ret_void },
    { "android/content/Intent", "addCategory", NULL, ret_self },
    { "android/content/Intent", "addFlags", NULL, ret_self },
    { "android/content/Intent", "setFlags", NULL, ret_self },
    { "android/content/Intent", "setPackage", NULL, ret_self },
    { "android/content/Intent", "setAction", NULL, ret_self },
    { "android/content/pm/PackageManager", "queryIntentActivities", NULL, ret_null },
    { "megler/service/display", "getDisplay", NULL, display_of },
    { "megler/service/display", "getDisplays", NULL, display_all },
    { "megler/service/display", "registerDisplayListener", NULL, ret_void },
    { "megler/service/display", "unregisterDisplayListener", NULL, ret_void },
    { "megler/service/input", "getInputDeviceIds", NULL, ret_empty_ints },
    { "megler/service/input", "getInputDevice", NULL, ret_null },
    { "megler/service/input", "registerInputDeviceListener", NULL, ret_void },
    { "megler/service/input", "unregisterInputDeviceListener", NULL, ret_void },
    { "megler/service/media_router", "getSelectedRoute", NULL, ret_null },
    { "megler/service/media_router", "addCallback", NULL, ret_void },
    { "megler/service/uimode", "getCurrentModeType", NULL, ret_one },         /* UI_MODE_TYPE_NORMAL */
    { "megler/service/activity", "getMemoryClass", NULL, audio_max_volume },
    { "android/view/InputDevice", "getDeviceIds", NULL, ret_empty_ints },
    { "android/view/InputDevice", "getDevice", NULL, ret_null },
    { "com/unity3d/player/HFPStatus", "requestHFPStat", NULL, ret_void },
    { "java/lang/Object", "requestHFPStat", NULL, ret_void },
    { "java/lang/StringBuilder", "append", NULL, sb_append },
    { "java/lang/StringBuilder", "toString", NULL, sb_to_string },
    { "android/content/res/AssetManager", "open", NULL, assets_open },
    { "java/io/InputStream", "available", NULL, stream_available },
    { "java/io/InputStream", "read", NULL, stream_read },
    { "java/io/InputStream", "close", NULL, ret_void },
    { "java/util/Scanner", "<init>", NULL, scanner_init },
    { "java/util/Scanner", "useDelimiter", NULL, ret_self },
    { "java/util/Scanner", "hasNext", NULL, scanner_has_next },
    { "java/util/Scanner", "next", NULL, scanner_next },
    { "java/util/Scanner", "close", NULL, ret_void },
    { "android/content/Context", "getPackageManager", NULL, ctx_package_manager },
    { "android/content/Context", "getResources", NULL, ctx_resources },
    { "android/content/Context", "getObbDirs", NULL, ctx_obb_dirs },
    { "android/content/pm/PackageManager", "getPackageInfo", NULL, pm_package_info },
    { "android/content/pm/PackageManager", "getApplicationInfo", NULL, pm_application_info },
    { "android/content/pm/PackageManager", "getInstallerPackageName", NULL, pm_installer },
    { "android/content/pm/PackageManager", "hasSystemFeature", NULL, ret_false },
    { "android/content/pm/PackageManager", "checkPermission", NULL, ret_zero },
    { BUNDLE, "getBoolean", NULL, bundle_get_boolean },
    { BUNDLE, "getInt", NULL, bundle_get_int },
    { BUNDLE, "getString", NULL, bundle_get_string },
    { BUNDLE, "containsKey", NULL, bundle_contains },
    { "android/content/res/Resources", "getConfiguration", NULL, res_configuration },
    { "android/content/res/Resources", "getDisplayMetrics", NULL, res_metrics },
    { "android/content/res/Resources", "getIdentifier", NULL, ret_zero },
    { "android/content/res/Resources", "getString", NULL, res_text },
    { "android/content/res/Resources", "getBoolean", NULL, ret_false },
    { "android/content/res/Resources", "getInteger", NULL, ret_zero },
    { "android/view/Window", "getAttributes", NULL, window_attributes },
    { "android/view/Window", "setAttributes", NULL, ret_void },
    { "org/fmod/FMODAudioDevice", "<init>", NULL, ret_void },
    { "org/fmod/FMODAudioDevice", "start", NULL, fmod_start },
    { "org/fmod/FMODAudioDevice", "stop", NULL, fmod_stop },
    { "org/fmod/FMODAudioDevice", "close", NULL, fmod_stop },
    { "org/fmod/FMODAudioDevice", "isRunning", NULL, fmod_running },
    { "org/fmod/FMODAudioDevice", "startAudioRecord", NULL, ret_zero },
    { "org/fmod/FMODAudioDevice", "stopAudioRecord", NULL, ret_void },
    { NULL, NULL, NULL, NULL },
};

/* The names of the system services, which native code reads from Context's static fields before asking for one. */
static void unity_java_setup(const mg_apk_info *info)
{
    static const char *const services[][2] = {
        { "AUDIO_SERVICE", "audio" }, { "DISPLAY_SERVICE", "display" }, { "WINDOW_SERVICE", "window" }, { "INPUT_SERVICE", "input" },
        { "INPUT_METHOD_SERVICE", "input_method" }, { "MEDIA_ROUTER_SERVICE", "media_router" }, { "CONNECTIVITY_SERVICE", "connectivity" },
        { "VIBRATOR_SERVICE", "vibrator" }, { "ACTIVITY_SERVICE", "activity" }, { "SENSOR_SERVICE", "sensor" }, { "CLIPBOARD_SERVICE", "clipboard" },
        { "POWER_SERVICE", "power" }, { "TELEPHONY_SERVICE", "phone" }, { "WIFI_SERVICE", "wifi" }, { "UI_MODE_SERVICE", "uimode" },
        { "LOCATION_SERVICE", "location" }, { "BATTERY_SERVICE", "batterymanager" }, { "NOTIFICATION_SERVICE", "notification" },
        { "CAMERA_SERVICE", "camera" }, { "STORAGE_SERVICE", "storage" }, { "KEYGUARD_SERVICE", "keyguard" },
    };
    mg_jvalue v;
    size_t i;
    int k;
    g_nmeta = 0;
    for (k = 0; k < info->nmeta && g_nmeta < (int)MG_COUNT(g_meta); k++) {
        mg_strlcpy(g_meta[g_nmeta].name, info->meta[k].name, sizeof(g_meta[0].name));
        mg_strlcpy(g_meta[g_nmeta].value, info->meta[k].value, sizeof(g_meta[0].value));
        g_nmeta++;
    }
    mg_strlcpy(g_version_name, info->version_name, sizeof(g_version_name));
    g_version_code = info->version_code;

    mg_jni_class("java/lang/StringBuilder", "java/lang/Object");
    mg_jni_register(g_unity_java);
    /* Loopers, handlers, the choreographer, and the proxies the engine implements their callbacks with: its frame pacing runs on these. */
    mg_jni_loop_install();
    /* The app's settings, kept between runs: Unity's PlayerPrefs. */
    mg_jni_prefs_install();
    /* SurfaceTexture: what a film's pictures are decoded onto. */
    mg_media_install();
    for (i = 0; i < MG_COUNT(services); i++) {
        v.j = 0;
        v.l = keep(mg_jni_new_string(services[i][1]));
        mg_jni_set_static("android/content/Context", services[i][0], 'L', v);
    }
    g_package_manager = keep(mg_jni_new_object("android/content/pm/PackageManager"));
    g_resources = keep(mg_jni_new_object("android/content/res/Resources"));
    g_bundle = keep(mg_jni_new_object(BUNDLE));

    g_package_info = keep(mg_jni_new_object("android/content/pm/PackageInfo"));
    v.j = 0; v.l = keep(mg_jni_new_string(g_version_name)); mg_jni_set_field(g_package_info, "versionName", 'L', v);
    v.j = 0; v.i = g_version_code; mg_jni_set_field(g_package_info, "versionCode", 'I', v);
    v.j = 0; v.l = keep(mg_jni_new_string(info->package)); mg_jni_set_field(g_package_info, "packageName", 'L', v);
    v.j = 0; v.l = mg_jni_android_app_info(); mg_jni_set_field(g_package_info, "applicationInfo", 'L', v);
    v.j = 0; v.l = g_bundle; mg_jni_set_field(mg_jni_android_app_info(), "metaData", 'L', v);

    /* A phone with nothing attached, held as the surface is shaped (sideways for most games): no hardware keyboard, a touch screen. */
    g_configuration = keep(mg_jni_new_object("android/content/res/Configuration"));
    {
        mg_surface sf;
        mg_run_surface(&sf);
        v.j = 0; v.i = sf.height > sf.width ? 1 : 2; mg_jni_set_field(g_configuration, "orientation", 'I', v);
    }
    v.i = 1; mg_jni_set_field(g_configuration, "keyboard", 'I', v);
    v.i = 2; mg_jni_set_field(g_configuration, "hardKeyboardHidden", 'I', v);
    v.i = 3; mg_jni_set_field(g_configuration, "touchscreen", 'I', v);
    v.i = 1; mg_jni_set_field(g_configuration, "navigation", 'I', v);
    v.i = 0x12; mg_jni_set_field(g_configuration, "screenLayout", 'I', v);    /* SIZE_NORMAL | LONG_NO */
    v.i = 0x11; mg_jni_set_field(g_configuration, "uiMode", 'I', v);          /* TYPE_NORMAL | NIGHT_NO */
    v.j = 0; v.f = 1.0f; mg_jni_set_field(g_configuration, "fontScale", 'F', v);
}

/* -------------------------------------------------------------------- input
 *
 * Android hands the engine a MotionEvent or a KeyEvent (nativeInjectEvent)
 * and the engine reads it back through JNI. Here the shell's touches and keys
 * are queued, and the engine's own thread hands them over before each frame:
 * guest code never runs on the shell's UI thread.
 *
 * The engine does not read an event inside the call. It asks for a copy of
 * it (MotionEvent.obtain(event), new KeyEvent(event)), puts that on a queue
 * of its own, and reads it when its input code next runs. So every event is
 * an object with its own contents, and a copy is another: they come from a
 * ring, each good until IN_RING more have been made, which is long after
 * the engine is done with it. (With one object filled in before each call,
 * and no obtain, nothing a finger did ever reached the game.)
 */
enum { IN_MAX_POINTERS = 10, IN_QUEUE = 256, IN_RING = 256 };

typedef struct in_event {
    bool is_key;
    int action;                        /* MotionEvent's or KeyEvent's */
    int keycode;
    int count, ids[IN_MAX_POINTERS];
    float xs[IN_MAX_POINTERS], ys[IN_MAX_POINTERS];
    i64 down_ms, event_ms;
} in_event;

static struct {
    mg_lock lock;
    in_event queue[IN_QUEUE];
    int head, count;
    /* the fingers that are down */
    int pointers, ids[IN_MAX_POINTERS];
    float xs[IN_MAX_POINTERS], ys[IN_MAX_POINTERS];
    i64 down_ms;
    /* The event objects, and what each says: handed out in turn. */
    mg_jobj *motion[IN_RING], *key[IN_RING];
    in_event motion_said[IN_RING], key_said[IN_RING];
    u32 next_motion, next_key;
} IN;

/* The next object of the ring, saying `e`. */
static mg_jobj *in_object(const in_event *e)
{
    mg_jobj *o;
    mg_lock_acquire(&IN.lock);
    if (e->is_key) {
        u32 k = IN.next_key++ % IN_RING;
        IN.key_said[k] = *e;
        o = IN.key[k];
    } else {
        u32 k = IN.next_motion++ % IN_RING;
        IN.motion_said[k] = *e;
        o = IN.motion[k];
    }
    mg_lock_release(&IN.lock);
    return o;
}

/* What an event object says: its own contents, or nothing at all for an object that is not one of these. */
static const in_event *in_said(const mg_jobj *o)
{
    static const in_event nothing;
    return o && o->native ? (const in_event *)o->native : &nothing;
}

static i64 uptime_ms(void) { return (i64)(mg_time_monotonic_ns() / 1000000ull); }

static void in_push(const in_event *e)
{
    /* A move that the engine has not taken yet is brought up to date, as Android batches them. */
    if (!e->is_key && e->action == 2 && IN.count) {
        in_event *last = &IN.queue[(IN.head + IN.count - 1) % IN_QUEUE];
        if (!last->is_key && last->action == 2 && last->count == e->count) {
            *last = *e;
            return;
        }
    }
    if (IN.count == IN_QUEUE) return;
    IN.queue[(IN.head + IN.count++) % IN_QUEUE] = *e;
}

static void un_touch(int phase, int pointer_id, float x, float y)
{
    in_event e;
    int i, index = -1;
    mg_lock_acquire(&IN.lock);
    for (i = 0; i < IN.pointers; i++)
        if (IN.ids[i] == pointer_id) index = i;
    if (phase == 0 && index < 0 && IN.pointers < IN_MAX_POINTERS) {
        index = IN.pointers++;
        IN.ids[index] = pointer_id;
        if (IN.pointers == 1) IN.down_ms = uptime_ms();
    }
    if (index < 0) {
        mg_lock_release(&IN.lock);
        return;
    }
    IN.xs[index] = x;
    IN.ys[index] = y;
    /* A finger going down or coming up is said (the first few hundred): which taps a game was given is the first question when it misbehaves. */
    if (phase != 1) {
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 400)
            mg_info("unity: finger %d %s at %.0f,%.0f (%d down)", pointer_id, phase == 0 ? "down" : phase == 2 ? "up" : "cancelled", (double)x, (double)y,
                    IN.pointers - (phase == 0 ? 0 : 1));
    }
    memset(&e, 0, sizeof(e));
    /* ACTION_DOWN 0, UP 1, MOVE 2, CANCEL 3, POINTER_DOWN 5, POINTER_UP 6; the pointer's index goes in bits 8-15. */
    if (phase == 0) e.action = IN.pointers == 1 ? 0 : (5 | (index << 8));
    else if (phase == 1) e.action = 2;
    else if (phase == 2) e.action = IN.pointers == 1 ? 1 : (6 | (index << 8));
    else e.action = 3;
    e.count = IN.pointers;
    memcpy(e.ids, IN.ids, sizeof(e.ids));
    memcpy(e.xs, IN.xs, sizeof(e.xs));
    memcpy(e.ys, IN.ys, sizeof(e.ys));
    e.down_ms = IN.down_ms;
    e.event_ms = uptime_ms();
    in_push(&e);
    /* A finger that has gone leaves the set after the event that says so. */
    if (phase == 2) {
        for (i = index; i + 1 < IN.pointers; i++) {
            IN.ids[i] = IN.ids[i + 1];
            IN.xs[i] = IN.xs[i + 1];
            IN.ys[i] = IN.ys[i + 1];
        }
        IN.pointers--;
    } else if (phase == 3) {
        IN.pointers = 0;
    }
    mg_lock_release(&IN.lock);
}

static void un_key(int keycode, bool down)
{
    in_event e;
    memset(&e, 0, sizeof(e));
    e.is_key = true;
    e.action = down ? 0 : 1;
    e.keycode = keycode;
    e.down_ms = e.event_ms = uptime_ms();
    mg_lock_acquire(&IN.lock);
    in_push(&e);
    mg_lock_release(&IN.lock);
}

/* On the engine's thread, before a frame: everything that has come since the last one. */
static void in_deliver(void)
{
    /* (Looked for once: on whichever of the player's classes, by either signature.) */
    static void *inject;
    static const char *sig;
    static bool looked;
    if (!looked) {
        looked = true;
        sig = "(Landroid/view/InputEvent;)Z";
        inject = player_native("nativeInjectEvent", sig);
        if (!inject) {
            /* Newer players take the event and the display it came from. */
            sig = "(Landroid/view/InputEvent;I)Z";
            inject = native_of(PLAYER, "nativeInjectEvent", sig);
        }
    }
    for (;;) {
        mg_jvalue a[2];
        in_event e;
        mg_lock_acquire(&IN.lock);
        if (!IN.count) {
            mg_lock_release(&IN.lock);
            return;
        }
        e = IN.queue[IN.head];
        IN.head = (IN.head + 1) % IN_QUEUE;
        IN.count--;
        mg_lock_release(&IN.lock);
        if (!inject) continue;
        a[0].j = a[1].j = 0;
        a[0].l = in_object(&e);
        mg_jni_call_native(inject, U.player, sig, a);
    }
}

#define SAID in_said(c->self)
static int in_index(mg_jcall *c)
{
    int i = c->args ? c->args[0].i : 0;
    return i >= 0 && i < SAID->count ? i : 0;
}
static void ev_action(mg_jcall *c) { c->ret.i = SAID->action; }
static void ev_action_masked(mg_jcall *c) { c->ret.i = SAID->action & 0xFF; }
static void ev_action_index(mg_jcall *c) { c->ret.i = (SAID->action >> 8) & 0xFF; }
static void ev_count(mg_jcall *c) { c->ret.i = SAID->count; }
static void ev_pointer_id(mg_jcall *c) { c->ret.i = SAID->ids[in_index(c)]; }
static void ev_x(mg_jcall *c) { c->ret.f = SAID->xs[strcmp(c->sig, "()F") ? in_index(c) : 0]; }
static void ev_y(mg_jcall *c) { c->ret.f = SAID->ys[strcmp(c->sig, "()F") ? in_index(c) : 0]; }
static void ev_one(mg_jcall *c) { c->ret.f = 1.0f; }
static void ev_fzero(mg_jcall *c) { c->ret.f = 0.0f; }
static void ev_event_time(mg_jcall *c) { c->ret.j = SAID->event_ms; }
static void ev_down_time(mg_jcall *c) { c->ret.j = SAID->down_ms; }
static void ev_source(mg_jcall *c) { c->ret.i = SAID->is_key ? 0x101 : 0x1002; }          /* SOURCE_KEYBOARD, SOURCE_TOUCHSCREEN */
static void ev_device(mg_jcall *c) { c->ret.i = SAID->is_key ? -1 : 1; }                /* the virtual keyboard; a touch screen */
static void ev_tool(mg_jcall *c) { c->ret.i = 1; }                                       /* TOOL_TYPE_FINGER */
static void ev_axis(mg_jcall *c)
{
    int axis = c->args[0].i, i = !strcmp(c->sig, "(II)F") && c->args[1].i >= 0 && c->args[1].i < SAID->count ? c->args[1].i : 0;
    c->ret.f = axis == 0 ? SAID->xs[i] : axis == 1 ? SAID->ys[i] : axis == 2 ? 1.0f : 0.0f;
}
static void ev_keycode(mg_jcall *c) { c->ret.i = SAID->keycode; }
/* MotionEvent.obtain(event), obtainNoHistory(event): another object saying the same. */
static void ev_obtain(mg_jcall *c)
{
    const mg_jobj *from = c->sig[1] == 'L' ? c->args[0].l : NULL;
    if (from && from->native) c->ret.l = in_object(in_said(from));
}
/* new KeyEvent(event), new KeyEvent(action, code): the object is the engine's own, so what it says is kept for good. */
static void ev_key_init(mg_jcall *c)
{
    in_event *e = (in_event *)mg_alloc(sizeof(*e));
    e->is_key = true;
    if (!strncmp(c->sig, "(Landroid/view/KeyEvent;", 24)) {
        *e = *in_said(c->args[0].l);
        e->is_key = true;
    } else if (!strcmp(c->sig, "(II)V")) {
        e->action = c->args[0].i;
        e->keycode = c->args[1].i;
        e->down_ms = e->event_ms = uptime_ms();
    }
    c->self->native = e;
}

#define MOTION "android/view/MotionEvent"
#define KEY "android/view/KeyEvent"
static const mg_jhle g_unity_input[] = {
    { MOTION, "getAction", NULL, ev_action },
    { MOTION, "getActionMasked", NULL, ev_action_masked },
    { MOTION, "getActionIndex", NULL, ev_action_index },
    { MOTION, "getPointerCount", NULL, ev_count },
    { MOTION, "getPointerId", NULL, ev_pointer_id },
    { MOTION, "getX", NULL, ev_x },
    { MOTION, "getY", NULL, ev_y },
    { MOTION, "getRawX", NULL, ev_x },
    { MOTION, "getRawY", NULL, ev_y },
    { MOTION, "getPressure", NULL, ev_one },
    { MOTION, "getSize", NULL, ev_one },
    { MOTION, "getTouchMajor", NULL, ev_one },
    { MOTION, "getTouchMinor", NULL, ev_one },
    { MOTION, "getToolMajor", NULL, ev_one },
    { MOTION, "getToolMinor", NULL, ev_one },
    { MOTION, "getOrientation", NULL, ev_fzero },
    { MOTION, "getXPrecision", NULL, ev_one },
    { MOTION, "getYPrecision", NULL, ev_one },
    { MOTION, "getAxisValue", NULL, ev_axis },
    { MOTION, "getToolType", NULL, ev_tool },
    { MOTION, "getEventTime", NULL, ev_event_time },
    { MOTION, "getDownTime", NULL, ev_down_time },
    { MOTION, "getSource", NULL, ev_source },
    { MOTION, "getDeviceId", NULL, ev_device },
    { MOTION, "getHistorySize", NULL, ret_zero },
    { MOTION, "getMetaState", NULL, ret_zero },
    { MOTION, "getButtonState", NULL, ret_zero },
    { MOTION, "getFlags", NULL, ret_zero },
    { MOTION, "getEdgeFlags", NULL, ret_zero },
    { MOTION, "obtain", NULL, ev_obtain },
    { MOTION, "obtainNoHistory", NULL, ev_obtain },
    { MOTION, "recycle", NULL, ret_void },
    { KEY, "<init>", NULL, ev_key_init },
    { KEY, "getAction", NULL, ev_action },
    { KEY, "getKeyCode", NULL, ev_keycode },
    { KEY, "getRepeatCount", NULL, ret_zero },
    { KEY, "getMetaState", NULL, ret_zero },
    { KEY, "getModifiers", NULL, ret_zero },
    { KEY, "getFlags", NULL, ret_zero },
    { KEY, "getScanCode", NULL, ret_zero },
    { KEY, "getUnicodeChar", NULL, ret_zero },
    { KEY, "getEventTime", NULL, ev_event_time },
    { KEY, "getDownTime", NULL, ev_down_time },
    { KEY, "getSource", NULL, ev_source },
    { KEY, "getDeviceId", NULL, ev_device },
    { KEY, "getCharacters", NULL, ret_null },
    { NULL, NULL, NULL, NULL },
};

static bool un_start(const megler_run_config *config, const mg_apk_info *info)
{
    char cls[220], *p;
    memset(&U, 0, sizeof(U));
    U.focused = 1;

    mg_strlcpy(cls, info->activity[0] ? info->activity : "com.unity3d.player.UnityPlayerActivity", sizeof(cls));
    for (p = cls; *p; p++)
        if (*p == '.') *p = '/';
    U.activity = mg_jni_android_setup(cls, info->package, config->apk_path, config->width, config->height, config->density_dpi);

    mg_jni_class("android/view/View", "java/lang/Object");
    mg_jni_class("android/view/ViewGroup", "android/view/View");
    mg_jni_class("android/widget/FrameLayout", "android/view/ViewGroup");
    mg_jni_class(PLAYER, "android/widget/FrameLayout");
    mg_jni_class(k_players[1], PLAYER);
    mg_jni_class(k_players[2], PLAYER);
    mg_jni_register(g_unity);
    mg_jni_class("android/view/InputEvent", "java/lang/Object");
    mg_jni_class(MOTION, "android/view/InputEvent");
    mg_jni_class(KEY, "android/view/InputEvent");
    mg_jni_register(g_unity_input);
    unity_java_setup(info);
    memset(&IN, 0, sizeof(IN));
    mg_lock_init(&IN.lock);
    {
        int k;
        for (k = 0; k < IN_RING; k++) {
            IN.motion[k] = mg_jni_new_object(MOTION);
            IN.motion[k]->permanent = true;
            IN.motion[k]->native = &IN.motion_said[k];
            IN.key[k] = mg_jni_new_object(KEY);
            IN.key[k]->permanent = true;
            IN.key[k]->native = &IN.key_said[k];
        }
    }

    U.player = mg_jni_new_object(PLAYER);
    U.player->permanent = true;
    /* The Surface is the window: ANativeWindow_fromSurface gives the engine the one inside. */
    U.window = mg_nwindow_create(config->width, config->height);
    U.surface = mg_jni_new_object("android/view/Surface");
    U.surface->permanent = true;
    U.surface->native = U.window;

    /* The engine's main thread runs the game's own code as well: a stack the size Android gives an app's main thread. */
    U.thread = mg_thread_start(unity_main, NULL, 8u * 1024 * 1024, "UnityMain");
    if (!U.thread) {
        mg_run_set_error("no thread for the game");
        return false;
    }
    /* Up to the engine being loaded and told where it is: that takes seconds, and a failure there is a failure to start. */
    while (U.started == 0) mg_sleep_ns(5000000ull);
    return U.started > 0;
}

static void un_set_paused(bool paused) { mg_atomic_xchg(&U.paused, paused ? 1 : 0); }
static void un_set_focused(bool focused) { mg_atomic_xchg(&U.focused, focused ? 1 : 0); }
static void un_stop(void) { mg_atomic_xchg(&U.stop, 1); }
static void un_resize(void) { mg_atomic_xchg(&U.resized, 1); }

const mg_driver mg_driver_unity = {
    "unity", un_start, un_touch, un_key, un_set_paused, un_set_focused, un_stop, NULL, un_resize,
};
