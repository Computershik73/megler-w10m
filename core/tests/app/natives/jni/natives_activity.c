/*
 * The library of the test app's NativeActivity of its own (megler.test.natives.Glue, android.app.lib_name "natives_activity"),
 * as KOReader's LuaJIT launcher is one: ANativeActivity_onCreate, called by the framework's NativeActivity in the activity's
 * onCreate, asks the activity's own Java class through JNI on clazz, on the main thread and from a thread of its own that
 * attaches (KOReader's android_main asks its MainActivity for extractAssets() so); its window, drawn with ANativeWindow_lock
 * in the window's format (RGB 565, NativeActivity's), green; its input queue, read on that thread from a looper of its own:
 * touches handled, keys not (so that Back goes on to the activity, which finishes). Each callback says itself in the log, in
 * the order the framework calls them; the test (core/tests/mg_java_tests.c, natives_activity) reads it.
 */
#include <jni.h>

#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define TAG "NativesActivity"
#define SAY(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static ANativeActivity *g_activity;
static AInputQueue *g_queue, *g_attached;
static ALooper *g_looper;
static int g_stop;

/* What the activity's own Java answers, from the thread asking: its class's name and two of its methods. */
static void ask_java(JNIEnv *env, ANativeActivity *a, const char *who)
{
    jclass cls = (*env)->GetObjectClass(env, a->clazz), class_class = (*env)->FindClass(env, "java/lang/Class");
    jmethodID get_name = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
    jmethodID answer = (*env)->GetMethodID(env, cls, "answer", "(I)I"), greeting = (*env)->GetMethodID(env, cls, "greeting", "()Ljava/lang/String;");
    jstring name = (jstring)(*env)->CallObjectMethod(env, cls, get_name);
    jstring hello = greeting ? (jstring)(*env)->CallObjectMethod(env, a->clazz, greeting) : NULL;
    const char *n = name ? (*env)->GetStringUTFChars(env, name, NULL) : NULL, *h = hello ? (*env)->GetStringUTFChars(env, hello, NULL) : NULL;
    int got = answer ? (*env)->CallIntMethod(env, a->clazz, answer, 20) : -1;
    SAY("%s: clazz is %s, answer(20)=%d, greeting=%s", who, n ? n : "?", got, h ? h : "null");
    if (n) (*env)->ReleaseStringUTFChars(env, name, n);
    if (h) (*env)->ReleaseStringUTFChars(env, hello, h);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

/* The app's own thread, as android_native_app_glue's: attached to Java, a looper of its own with the input queue on it. */
static void *app_thread(void *arg)
{
    JNIEnv *env = NULL;
    JavaVM *vm = g_activity->vm;
    (void)arg;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK && env) {
        ask_java(env, g_activity, "thread");
        (*vm)->DetachCurrentThread(vm);
    }
    pthread_mutex_lock(&g_lock);
    g_looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    for (;;) {
        AInputQueue *q;
        AInputEvent *e = NULL;
        pthread_mutex_lock(&g_lock);
        if (g_stop) {
            if (g_attached) AInputQueue_detachLooper(g_attached);
            g_attached = NULL;
            pthread_cond_broadcast(&g_wake);
            pthread_mutex_unlock(&g_lock);
            break;
        }
        if (g_queue != g_attached) {
            if (g_attached) AInputQueue_detachLooper(g_attached);
            g_attached = g_queue;
            if (g_attached) AInputQueue_attachLooper(g_attached, g_looper, 1, NULL, NULL);
            pthread_cond_broadcast(&g_wake);
        }
        q = g_attached;
        pthread_mutex_unlock(&g_lock);
        if (ALooper_pollAll(50, NULL, NULL, NULL) != 1 || !q) continue;
        pthread_mutex_lock(&g_lock);
        while (q == g_attached && AInputQueue_getEvent(q, &e) >= 0) {
            int handled = 0;
            if (AInputQueue_preDispatchEvent(q, e)) continue;
            if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
                int action = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
                if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_UP)
                    SAY("touch %s at %d,%d", action == AMOTION_EVENT_ACTION_DOWN ? "down" : "up", (int)AMotionEvent_getX(e, 0), (int)AMotionEvent_getY(e, 0));
                handled = 1;
            } else {
                SAY("key %d %s, not handled", AKeyEvent_getKeyCode(e), AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN ? "down" : "up");
            }
            AInputQueue_finishEvent(q, e, handled);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void on_start(ANativeActivity *a) { (void)a; SAY("onStart"); }
static void on_resume(ANativeActivity *a) { (void)a; SAY("onResume"); }
static void on_pause(ANativeActivity *a) { (void)a; SAY("onPause"); }
static void on_stop(ANativeActivity *a) { (void)a; SAY("onStop"); }
static void on_focus(ANativeActivity *a, int focused) { (void)a; SAY("focus %d", focused); }
static void on_rect(ANativeActivity *a, const ARect *r) { (void)a; SAY("content %d,%d %dx%d", r->left, r->top, r->right - r->left, r->bottom - r->top); }

static void on_destroy(ANativeActivity *a)
{
    (void)a;
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    pthread_mutex_unlock(&g_lock);
    SAY("onDestroy");
}

/* The window, painted green in its own format: RGB 565, as NativeActivity's window is unless the app asks for another. */
static void on_window_created(ANativeActivity *a, ANativeWindow *w)
{
    ANativeWindow_Buffer b;
    int x, y, rc;
    (void)a;
    memset(&b, 0, sizeof(b));
    rc = ANativeWindow_lock(w, &b, NULL);
    SAY("window %dx%d format=%d lock=%d", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w), ANativeWindow_getFormat(w), rc);
    if (rc) return;
    for (y = 0; y < b.height; y++)
        for (x = 0; x < b.width; x++) {
            if (b.format == WINDOW_FORMAT_RGB_565) ((uint16_t *)b.bits)[y * b.stride + x] = 0x07E0;
            else ((uint32_t *)b.bits)[y * b.stride + x] = 0xFF00FF00u;
        }
    ANativeWindow_unlockAndPost(w);
}

static void on_window_destroyed(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; SAY("window destroyed"); }

static void on_queue_created(ANativeActivity *a, AInputQueue *q)
{
    (void)a;
    pthread_mutex_lock(&g_lock);
    g_queue = q;
    while (g_attached != q && !g_stop) pthread_cond_wait(&g_wake, &g_lock);
    pthread_mutex_unlock(&g_lock);
    SAY("input queue");
}

/* The queue goes: the thread lets go of it before this returns, as the glue waits for its thread. */
static void on_queue_destroyed(ANativeActivity *a, AInputQueue *q)
{
    (void)a;
    pthread_mutex_lock(&g_lock);
    g_queue = NULL;
    while (g_attached == q) pthread_cond_wait(&g_wake, &g_lock);
    pthread_mutex_unlock(&g_lock);
    SAY("input queue destroyed");
}

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size)
{
    pthread_t t;
    (void)saved;
    g_activity = a;
    SAY("onCreate sdk=%d saved=%u internal=%s", a->sdkVersion, (unsigned)saved_size, a->internalDataPath ? "yes" : "no");
    ask_java(a->env, a, "main");
    a->callbacks->onStart = on_start;
    a->callbacks->onResume = on_resume;
    a->callbacks->onPause = on_pause;
    a->callbacks->onStop = on_stop;
    a->callbacks->onDestroy = on_destroy;
    a->callbacks->onWindowFocusChanged = on_focus;
    a->callbacks->onContentRectChanged = on_rect;
    a->callbacks->onNativeWindowCreated = on_window_created;
    a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onInputQueueCreated = on_queue_created;
    a->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    pthread_mutex_lock(&g_lock);
    g_stop = 0;
    pthread_mutex_unlock(&g_lock);
    if (pthread_create(&t, NULL, app_thread, NULL) == 0) {
        pthread_detach(t);
        pthread_mutex_lock(&g_lock);
        while (!g_looper) pthread_cond_wait(&g_wake, &g_lock);
        pthread_mutex_unlock(&g_lock);
    }
}
