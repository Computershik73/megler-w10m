/*
 * A NativeActivity "game" for the tests: what android_native_app_glue does,
 * written out -- ANativeActivity_onCreate starts a thread, the activity's
 * callbacks pass commands to it down a pipe, and the thread runs a looper,
 * brings up EGL on the window it is given, draws with OpenGL ES 2 and answers
 * touches. It exercises the same NDK calls a real one does, in the same order.
 *
 * What it saw is kept in t_native_state, for the test to check.
 */
#include "t_abi.h"

typedef struct ANativeActivity ANativeActivity;
typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity *);
    void (*onResume)(ANativeActivity *);
    void *(*onSaveInstanceState)(ANativeActivity *, size_t *);
    void (*onPause)(ANativeActivity *);
    void (*onStop)(ANativeActivity *);
    void (*onDestroy)(ANativeActivity *);
    void (*onWindowFocusChanged)(ANativeActivity *, int);
    void (*onNativeWindowCreated)(ANativeActivity *, void *);
    void (*onNativeWindowResized)(ANativeActivity *, void *);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *, void *);
    void (*onNativeWindowDestroyed)(ANativeActivity *, void *);
    void (*onInputQueueCreated)(ANativeActivity *, void *);
    void (*onInputQueueDestroyed)(ANativeActivity *, void *);
    void (*onContentRectChanged)(ANativeActivity *, const void *);
    void (*onConfigurationChanged)(ANativeActivity *);
    void (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;
struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    void *vm, *env, *clazz;
    const char *internalDataPath, *externalDataPath;
    int sdkVersion;
    void *instance, *assetManager;
    const char *obbPath;
};

/* jni.h, as far as this uses it: the environment is a pointer to a table of functions. */
typedef void *jobject;
typedef const struct JNINativeInterface *JNIEnv;
struct JNINativeInterface {
    void *reserved[4];
    void *slots[229];
};
#define JNI(env, index, type) ((type)(*(env))->slots[(index) - 4])

int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_broadcast(pthread_cond_t *);
pthread_t pthread_self(void);
int pthread_getattr_np(pthread_t, pthread_attr_t *);
int pthread_attr_getstack(const pthread_attr_t *, void **, size_t *);
int pipe(int[2]);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int close(int);
int strcmp(const char *, const char *);
size_t strlen(const char *);
void *memset(void *, int, size_t);
int snprintf(char *, size_t, const char *, ...);
int __android_log_print(int, const char *, const char *, ...);
void *fopen(const char *, const char *);
int fclose(void *);
size_t fwrite(const void *, size_t, size_t, void *);

void *ALooper_prepare(int);
int ALooper_addFd(void *, int, int, int, void *, void *);
int ALooper_pollAll(int, int *, int *, void **);
void AInputQueue_attachLooper(void *, void *, int, void *, void *);
void AInputQueue_detachLooper(void *);
int AInputQueue_getEvent(void *, void **);
int AInputQueue_preDispatchEvent(void *, void *);
void AInputQueue_finishEvent(void *, void *, int);
int AInputEvent_getType(const void *);
int AMotionEvent_getAction(const void *);
size_t AMotionEvent_getPointerCount(const void *);
float AMotionEvent_getX(const void *, size_t);
float AMotionEvent_getY(const void *, size_t);
int AKeyEvent_getKeyCode(const void *);
int ANativeWindow_getWidth(void *);
int ANativeWindow_getHeight(void *);
int ANativeWindow_setBuffersGeometry(void *, int, int, int);
void *AAssetManager_open(void *, const char *, int);
int AAsset_read(void *, void *, size_t);
int AAsset_getLength(void *);
void AAsset_close(void *);
void *AConfiguration_new(void);
void AConfiguration_fromAssetManager(void *, void *);
int AConfiguration_getDensity(void *);
void AConfiguration_delete(void *);
void ANativeActivity_finish(ANativeActivity *);

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
EGLDisplay eglGetDisplay(void *);
unsigned eglInitialize(EGLDisplay, int *, int *);
unsigned eglChooseConfig(EGLDisplay, const int *, EGLConfig *, int, int *);
unsigned eglGetConfigAttrib(EGLDisplay, EGLConfig, int, int *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, void *, const int *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const int *);
unsigned eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
unsigned eglSwapBuffers(EGLDisplay, EGLSurface);
unsigned eglQuerySurface(EGLDisplay, EGLSurface, int, int *);
unsigned eglDestroySurface(EGLDisplay, EGLSurface);
unsigned eglDestroyContext(EGLDisplay, EGLContext);

void glViewport(int, int, int, int);
void glClearColor(float, float, float, float);
void glClear(unsigned);
unsigned glCreateShader(unsigned);
void glShaderSource(unsigned, int, const char *const *, const int *);
void glCompileShader(unsigned);
unsigned glCreateProgram(void);
void glAttachShader(unsigned, unsigned);
void glLinkProgram(unsigned);
void glUseProgram(unsigned);
int glGetUniformLocation(unsigned, const char *);
int glGetAttribLocation(unsigned, const char *);
void glUniform1f(int, float);
void glUniform4f(int, float, float, float, float);
void glUniformMatrix4fv(int, int, unsigned char, const float *);
void glVertexAttribPointer(unsigned, int, unsigned, unsigned char, int, const void *);
void glEnableVertexAttribArray(unsigned);
void glDrawArrays(unsigned, int, int);
void glLineWidth(float);
const unsigned char *glGetString(unsigned);

/* OpenSL ES, as far as this uses it: an interface is a pointer to a pointer to a table of functions. */
typedef const struct SLObjectItf_ *const *SLObjectItf;
struct SLObjectItf_ {
    unsigned (*Realize)(SLObjectItf, unsigned);
    unsigned (*Resume)(SLObjectItf, unsigned);
    unsigned (*GetState)(SLObjectItf, unsigned *);
    unsigned (*GetInterface)(SLObjectItf, const void *, void *);
    void *RegisterCallback, *AbortAsyncOperation;
    void (*Destroy)(SLObjectItf);
};
typedef const struct SLEngineItf_ *const *SLEngineItf;
struct SLEngineItf_ {
    void *CreateLEDDevice, *CreateVibraDevice;
    unsigned (*CreateAudioPlayer)(SLEngineItf, SLObjectItf *, void *, void *, unsigned, const void *, const void *);
    unsigned (*CreateAudioRecorder)(SLEngineItf, SLObjectItf *, void *, void *, unsigned, const void *, const void *);
    void *CreateMidiPlayer, *CreateListener, *Create3DGroup;
    unsigned (*CreateOutputMix)(SLEngineItf, SLObjectItf *, unsigned, const void *, const void *);
};
typedef const struct SLPlayItf_ *const *SLPlayItf;
struct SLPlayItf_ {
    unsigned (*SetPlayState)(SLPlayItf, unsigned);
};
typedef const struct SLBufferQueueItf_ *const *SLBufferQueueItf;
struct SLBufferQueueItf_ {
    unsigned (*Enqueue)(SLBufferQueueItf, const void *, unsigned);
    unsigned (*Clear)(SLBufferQueueItf);
    unsigned (*GetState)(SLBufferQueueItf, void *);
    unsigned (*RegisterCallback)(SLBufferQueueItf, void (*)(SLBufferQueueItf, void *), void *);
};
typedef const struct SLRecordItf_ *const *SLRecordItf;
struct SLRecordItf_ {
    unsigned (*SetRecordState)(SLRecordItf, unsigned);
    unsigned (*GetRecordState)(SLRecordItf, unsigned *);
};
typedef const struct SLAndroidConfigurationItf_ *const *SLAndroidConfigurationItf;
struct SLAndroidConfigurationItf_ {
    unsigned (*SetConfiguration)(SLAndroidConfigurationItf, const char *, const void *, unsigned);
    unsigned (*GetConfiguration)(SLAndroidConfigurationItf, const char *, unsigned *, void *);
};
extern const void *SL_IID_ENGINE, *SL_IID_PLAY, *SL_IID_BUFFERQUEUE, *SL_IID_RECORD, *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_ANDROIDCONFIGURATION;
unsigned slCreateEngine(SLObjectItf *, unsigned, const void *, unsigned, const void *, const void *);
int usleep(unsigned);

enum { CMD_INPUT_CREATED = 1, CMD_WINDOW_CREATED, CMD_WINDOW_DESTROYED, CMD_FOCUS, CMD_BLUR, CMD_START, CMD_RESUME, CMD_PAUSE, CMD_STOP, CMD_DESTROY, CMD_INPUT_DESTROYED };

struct t_native_state {
    int started, resumed, paused, stopped, destroyed, focused;
    int window_width, window_height, surface_width, surface_height;
    int frames, touches, last_action, pointers, keys, last_key;
    float last_x, last_y;
    int density, asset_length, package_ok, files_dir_ok, sdk_int, finished_drawing;
    char asset_text[32];
    int audio_ok, audio_callbacks;
    int audio_stack_kb, audio_frame;
    /*
     * The recorders (record_thread): over, and the step that went wrong (0: none); the buffers of the microphone's sound
     * looked at, their first and last sample; the buffers of silence looked at; the samples in either that were not what
     * they should be; the callbacks that queued a buffer again, and those that came after a Destroy had returned.
     */
    int rec_done, rec_failed, rec_buffers, rec_first, rec_last, rec_silent, rec_bad, rec_callbacks, rec_late;
};
struct t_native_state t_native_state;

static struct {
    ANativeActivity *activity;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int msg_read, msg_write;
    void *queue, *pending_queue, *window, *pending_window;
    int running, handled;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    unsigned program;
    int u_color, u_time, a_position;
    int animating;
} app;

static void send(int cmd)
{
    char c = (char)cmd;
    write(app.msg_write, &c, 1);
}

/* The activity's thread waits for the app's thread to have dealt with the command, as the glue does. */
static void send_and_wait(int cmd)
{
    pthread_mutex_lock(&app.lock);
    app.handled = 0;
    send(cmd);
    while (!app.handled) pthread_cond_wait(&app.cond, &app.lock);
    pthread_mutex_unlock(&app.lock);
}

static void on_start(ANativeActivity *a) { (void)a; send_and_wait(CMD_START); }
static void on_resume(ANativeActivity *a) { (void)a; send_and_wait(CMD_RESUME); }
static void on_pause(ANativeActivity *a) { (void)a; send_and_wait(CMD_PAUSE); }
static void on_stop(ANativeActivity *a) { (void)a; send_and_wait(CMD_STOP); }
static void on_destroy(ANativeActivity *a) { (void)a; send_and_wait(CMD_DESTROY); }
static void on_focus(ANativeActivity *a, int focused) { (void)a; send_and_wait(focused ? CMD_FOCUS : CMD_BLUR); }
static void on_window_created(ANativeActivity *a, void *window)
{
    (void)a;
    app.pending_window = window;
    send_and_wait(CMD_WINDOW_CREATED);
}
static void on_window_destroyed(ANativeActivity *a, void *window)
{
    (void)a; (void)window;
    send_and_wait(CMD_WINDOW_DESTROYED);
}
static void on_input_created(ANativeActivity *a, void *queue)
{
    (void)a;
    app.pending_queue = queue;
    send_and_wait(CMD_INPUT_CREATED);
}
static void on_input_destroyed(ANativeActivity *a, void *queue)
{
    (void)a; (void)queue;
    send_and_wait(CMD_INPUT_DESTROYED);
}

static const char *const VS = "attribute vec2 a_position; uniform float u_time; void main() { gl_Position = vec4(a_position * cos(u_time), 0.0, 1.0); }";
static const char *const FS = "precision mediump float; uniform vec4 u_color; void main() { gl_FragColor = u_color; }";

static void init_display(void)
{
    static const int config_attribs[] = { 0x3033 /* SURFACE_TYPE */, 4 /* WINDOW_BIT */, 0x3040 /* RENDERABLE_TYPE */, 4 /* ES2 */,
                                          0x3022, 8, 0x3023, 8, 0x3024, 8, 0x3025, 16, 0x3038 };
    static const int context_attribs[] = { 0x3098 /* CONTEXT_CLIENT_VERSION */, 2, 0x3038 };
    EGLConfig config = 0;
    int n = 0, format = 0;
    unsigned vs, fs;
    app.display = eglGetDisplay(0);
    eglInitialize(app.display, 0, 0);
    eglChooseConfig(app.display, config_attribs, &config, 1, &n);
    eglGetConfigAttrib(app.display, config, 0x302E /* NATIVE_VISUAL_ID */, &format);
    ANativeWindow_setBuffersGeometry(app.window, 0, 0, format);
    app.surface = eglCreateWindowSurface(app.display, config, app.window, 0);
    app.context = eglCreateContext(app.display, config, 0, context_attribs);
    eglMakeCurrent(app.display, app.surface, app.surface, app.context);
    eglQuerySurface(app.display, app.surface, 0x3057, &t_native_state.surface_width);
    eglQuerySurface(app.display, app.surface, 0x3056, &t_native_state.surface_height);
    t_native_state.window_width = ANativeWindow_getWidth(app.window);
    t_native_state.window_height = ANativeWindow_getHeight(app.window);
    glViewport(0, 0, t_native_state.surface_width, t_native_state.surface_height);
    vs = glCreateShader(0x8B31);
    glShaderSource(vs, 1, &VS, 0);
    glCompileShader(vs);
    fs = glCreateShader(0x8B30);
    glShaderSource(fs, 1, &FS, 0);
    glCompileShader(fs);
    app.program = glCreateProgram();
    glAttachShader(app.program, vs);
    glAttachShader(app.program, fs);
    glLinkProgram(app.program);
    glUseProgram(app.program);
    app.u_color = glGetUniformLocation(app.program, "u_color");
    app.u_time = glGetUniformLocation(app.program, "u_time");
    app.a_position = glGetAttribLocation(app.program, "a_position");
    __android_log_print(4, "t_native", "GL %s, surface %dx%d", (const char *)glGetString(0x1F02), t_native_state.surface_width, t_native_state.surface_height);
}

static void draw_frame(void)
{
    static const float triangle[] = { 0.0f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f };
    float t = (float)t_native_state.frames * 0.125f;
    if (!app.surface) return;
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(0x4000);
    glUniform4f(app.u_color, 1.0f, 0.5f, 0.25f, 1.0f);
    glUniform1f(app.u_time, t);
    glLineWidth(2.5f);
    glVertexAttribPointer((unsigned)app.a_position, 2, 0x1406, 0, 0, triangle);
    glEnableVertexAttribArray((unsigned)app.a_position);
    glDrawArrays(4, 0, 3);
    eglSwapBuffers(app.display, app.surface);
    t_native_state.frames++;
}

static void term_display(void)
{
    if (app.display) {
        eglMakeCurrent(app.display, 0, 0, 0);
        if (app.context) eglDestroyContext(app.display, app.context);
        if (app.surface) eglDestroySurface(app.display, app.surface);
    }
    app.surface = 0;
    app.context = 0;
    app.animating = 0;
}

/* What an app asks Java through the activity object: its package name, its files directory, the SDK level. */
static void ask_java(void)
{
    JNIEnv *env = (JNIEnv *)app.activity->env;
    jobject activity = app.activity->clazz;
    jobject cls = JNI(env, 31, jobject (*)(JNIEnv *, jobject))(env, activity);                                    /* GetObjectClass */
    void *mid = JNI(env, 33, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, cls, "getPackageName", "()Ljava/lang/String;");
    jobject name = JNI(env, 34, jobject (*)(JNIEnv *, jobject, void *, ...))(env, activity, mid);                 /* CallObjectMethod */
    const char *text = JNI(env, 169, const char *(*)(JNIEnv *, jobject, void *))(env, name, 0);                   /* GetStringUTFChars */
    jobject dir, path, version;
    void *fid;
    t_native_state.package_ok = text && !strcmp(text, "org.megler.test.native");
    mid = JNI(env, 33, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, cls, "getFilesDir", "()Ljava/io/File;");
    dir = JNI(env, 34, jobject (*)(JNIEnv *, jobject, void *, ...))(env, activity, mid);
    mid = JNI(env, 33, void *(*)(JNIEnv *, jobject, const char *, const char *))(
        env, JNI(env, 31, jobject (*)(JNIEnv *, jobject))(env, dir), "getAbsolutePath", "()Ljava/lang/String;");
    path = JNI(env, 34, jobject (*)(JNIEnv *, jobject, void *, ...))(env, dir, mid);
    text = JNI(env, 169, const char *(*)(JNIEnv *, jobject, void *))(env, path, 0);
    t_native_state.files_dir_ok = text && !strcmp(text, app.activity->internalDataPath);
    version = JNI(env, 6, jobject (*)(JNIEnv *, const char *))(env, "android/os/Build$VERSION");                 /* FindClass */
    fid = JNI(env, 144, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, version, "SDK_INT", "I");  /* GetStaticFieldID */
    t_native_state.sdk_int = JNI(env, 150, int (*)(JNIEnv *, jobject, void *))(env, version, fid);                /* GetStaticIntField */
}

/* Three tenths of a second of a constant level through a buffer queue, each buffer asked for by the callback of the one before. */
static short g_pcm[3][2205];
static int g_enqueued;

/*
 * A game's mixing function: 600 KB of buffers in its frame, the far end written first (Smash Hit's two, nested, take
 * 128 KB each). The array's address is given to an empty asm, which keeps the array the frame's, whole.
 */
__attribute__((noinline)) static int mixing_frame(int seed)
{
    volatile unsigned char buf[600 * 1024];
    __asm__ volatile("" : : "r"((const void *)buf) : "memory");
    buf[0] = (unsigned char)seed;
    buf[sizeof(buf) - 1] = (unsigned char)(seed + 1);
    buf[300000] = 7;
    return buf[0] + buf[sizeof(buf) - 1] + buf[300000];
}

/*
 * The stack of the thread the callbacks come on, as Android's has it: pthread's default megabyte. The frame is tried
 * only where 700 KB lie below this function, so that a thread with less fails the test instead of ending it.
 */
static void audio_thread_stack(void)
{
    pthread_attr_t attr;
    void *base = 0;
    size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) != 0 || pthread_attr_getstack(&attr, &base, &size) != 0) return;
    t_native_state.audio_stack_kb = (int)(size >> 10);
    if ((char *)&attr > (char *)base && (size_t)((char *)&attr - (char *)base) >= 700 * 1024) t_native_state.audio_frame = mixing_frame(10);
}

static void queue_callback(SLBufferQueueItf queue, void *context)
{
    (void)context;
    if (!t_native_state.audio_callbacks) audio_thread_stack();
    if (g_enqueued < 3) {
        (*queue)->Enqueue(queue, g_pcm[g_enqueued], sizeof(g_pcm[0]));
        g_enqueued++;
    }
    t_native_state.audio_callbacks++;
}

/* The one engine an app has: its players and its recorders are made by it. */
static SLEngineItf g_engine;

static void start_audio(void)
{
    SLObjectItf engine_object = 0, mix = 0, player = 0;
    SLEngineItf engine = 0;
    SLPlayItf play = 0;
    SLBufferQueueItf queue = 0;
    /* SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE with two buffers; PCM, mono, 22050 Hz (in millihertz), 16 bits, front centre, little-endian;
       SL_DATALOCATOR_OUTPUTMIX and the mix, a pointer after a 32-bit type: at 8 on the 64-bit ABIs */
#if defined(__LP64__)
    unsigned locator[2] = { 0x800007BDu, 2 }, format[7] = { 2, 1, 22050000, 16, 16, 4, 2 };
    struct { unsigned type; SLObjectItf mix; } out_locator = { 4, 0 };
#else
    unsigned locator[2] = { 0x800007BDu, 2 }, format[7] = { 2, 1, 22050000, 16, 16, 4, 2 }, out_locator[2] = { 4, 0 };
#endif
    void *source[2], *sink[2];
    int i, k;
    for (k = 0; k < 3; k++)
        for (i = 0; i < 2205; i++) g_pcm[k][i] = 0x2000;
    source[0] = locator;
    source[1] = format;
#if defined(__LP64__)
    sink[0] = &out_locator;
#else
    sink[0] = out_locator;
#endif
    sink[1] = 0;
    if (slCreateEngine(&engine_object, 0, 0, 0, 0, 0) != 0) return;
    (*engine_object)->Realize(engine_object, 0);
    (*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine);
    g_engine = engine;
    (*engine)->CreateOutputMix(engine, &mix, 0, 0, 0);
    (*mix)->Realize(mix, 0);
#if defined(__LP64__)
    out_locator.mix = mix;
#else
    out_locator[1] = (unsigned)mix;
#endif
    if ((*engine)->CreateAudioPlayer(engine, &player, source, sink, 1, &SL_IID_BUFFERQUEUE, 0) != 0) return;
    (*player)->Realize(player, 0);
    (*player)->GetInterface(player, SL_IID_PLAY, &play);
    (*player)->GetInterface(player, SL_IID_BUFFERQUEUE, &queue);
    if (!play || !queue) return;
    (*queue)->RegisterCallback(queue, queue_callback, 0);
    (*queue)->Enqueue(queue, g_pcm[0], sizeof(g_pcm[0]));
    g_enqueued = 1;
    (*play)->SetPlayState(play, 3);
    t_native_state.audio_ok = 1;
}

/*
 * A call's microphone, in the order WebRTC's opensles_recorder.cc makes its calls: the recorder made from the microphone
 * into a queue of two buffers of ten milliseconds (16 bits), with the queue and the configuration asked for as required;
 * the recording preset set before Realize; both buffers queued before SetRecordState(RECORDING); each queued again from
 * the callback that hands it back filled; and at the end stopped, cleared, its callback taken away and destroyed, by a
 * thread that is not the callbacks'. All this while the player above plays, as in a call.
 *
 * Twice: at 16000 Hz mono, where the test's microphone gives a ramp from 1 in steps of 3, of which the first twelve
 * buffers are looked at; then at 48000 Hz stereo, where the test's microphone refuses, as a user who says no makes it,
 * and three buffers of silence are looked at.
 */
enum { REC_BUFFERS = 12, REC_SILENT = 3 };
static short g_rec_pcm[2][960];
static SLRecordItf g_rec_record;
static volatile int g_rec_samples;     /* in a buffer: 160, then 960 */
static volatile int g_rec_destroyed;
static int g_rec_turn;                 /* which of the two buffers the next callback hands back */
static short g_rec_expected = 1;       /* the ramp's next sample */

static void record_callback(SLBufferQueueItf queue, void *context)
{
    short *pcm = g_rec_pcm[g_rec_turn];
    const int samples = g_rec_samples;
    unsigned state = 0;
    int i;
    if (g_rec_destroyed) {
        t_native_state.rec_late++;
        return;
    }
    /* (WebRTC asks this first, and takes nothing once the recorder does not record.) */
    if ((*g_rec_record)->GetRecordState(g_rec_record, &state) != 0 || state != 3) return;
    if (context) {
        /* The recorder whose microphone was refused. */
        if (t_native_state.rec_silent < REC_SILENT) {
            for (i = 0; i < samples; i++)
                if (pcm[i]) t_native_state.rec_bad++;
            t_native_state.rec_silent++;
        }
    } else if (t_native_state.rec_buffers < REC_BUFFERS) {
        for (i = 0; i < samples; i++, g_rec_expected = (short)(g_rec_expected + 3))
            if (pcm[i] != g_rec_expected) t_native_state.rec_bad++;
        if (!t_native_state.rec_buffers) t_native_state.rec_first = pcm[0];
        t_native_state.rec_last = pcm[samples - 1];
        t_native_state.rec_buffers++;
    }
    /* Queued again, holding what no microphone here gives: what the next callback finds in it was written. */
    for (i = 0; i < samples; i++) pcm[i] = 0x5555;
    g_rec_turn ^= 1;
    (*queue)->Enqueue(queue, pcm, (unsigned)samples * sizeof(short));
    t_native_state.rec_callbacks++;
}

#define REC_STEP(n, ok)                \
    do {                               \
        if (!(ok)) return base + (n);  \
    } while (0)

/* One recorder from its making to its end; `count` is what its callbacks count, of which `enough` are waited for. 0, or the step that failed. */
static int record_once(SLEngineItf engine, int base, unsigned millihertz, unsigned channels, void *context, const volatile int *count, int enough)
{
    /* SL_DATALOCATOR_IODEVICE: SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT, and no device object (a pointer: at 16 on the 64-bit ABIs);
       SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE with two buffers; PCM, 16 bits, front centre or front left and right, little-endian */
    unsigned from[6] = { 3, 1, 0xFFFFFFFFu, 0, 0, 0 }, to[2] = { 0x800007BDu, 2 }, format[7] = { 2, 0, 0, 16, 16, 0, 2 };
    void *source[2], *sink[2];
    const void *ids[2];
    const unsigned required[2] = { 1, 1 };
    struct { unsigned count, index; } queued = { 9, 9 };
    SLObjectItf recorder = 0;
    SLAndroidConfigurationItf config = 0;
    SLBufferQueueItf queue = 0;
    unsigned preset = 4, size = 4, told = 0, state = 0, bytes;     /* SL_ANDROID_RECORDING_PRESET_VOICE_COMMUNICATION */
    int i, k;
    format[1] = channels;
    format[2] = millihertz;
    format[5] = channels == 2 ? 3 : 4;
    source[0] = from;
    source[1] = 0;
    sink[0] = to;
    sink[1] = format;
    ids[0] = SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
    ids[1] = SL_IID_ANDROIDCONFIGURATION;
    g_rec_samples = (int)(millihertz / 100000 * channels);
    bytes = (unsigned)g_rec_samples * sizeof(short);
    g_rec_turn = 0;
    g_rec_record = 0;
    for (k = 0; k < 2; k++)
        for (i = 0; i < 960; i++) g_rec_pcm[k][i] = 0x5555;
    REC_STEP(1, (*engine)->CreateAudioRecorder(engine, &recorder, source, sink, 2, ids, required) == 0 && recorder);
    g_rec_destroyed = 0;
    /* The configuration is asked for and set before Realize. */
    REC_STEP(2, (*recorder)->GetInterface(recorder, SL_IID_ANDROIDCONFIGURATION, &config) == 0 && config);
    REC_STEP(3, (*config)->SetConfiguration(config, "androidRecordingPreset", &preset, sizeof(preset)) == 0);
    REC_STEP(4, (*config)->GetConfiguration(config, "androidRecordingPreset", &size, &told) == 0 && told == 4 && size == 4);
    REC_STEP(5, (*recorder)->Realize(recorder, 0) == 0);
    REC_STEP(6, (*recorder)->GetInterface(recorder, SL_IID_RECORD, &g_rec_record) == 0 && g_rec_record);
    REC_STEP(7, (*recorder)->GetInterface(recorder, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue) == 0 && queue);
    REC_STEP(8, (*queue)->RegisterCallback(queue, record_callback, context) == 0);
    REC_STEP(9, (*queue)->GetState(queue, &queued) == 0 && queued.count == 0 && queued.index == 0);
    /* Two buffers, all the queue was made for: a third is refused (SL_RESULT_BUFFER_INSUFFICIENT). */
    REC_STEP(10, (*queue)->Enqueue(queue, g_rec_pcm[0], bytes) == 0 && (*queue)->Enqueue(queue, g_rec_pcm[1], bytes) == 0);
    REC_STEP(11, (*queue)->Enqueue(queue, g_rec_pcm[0], bytes) == 7 && (*queue)->GetState(queue, &queued) == 0 && queued.count == 2);
    REC_STEP(12, (*g_rec_record)->GetRecordState(g_rec_record, &state) == 0 && state == 1 && (*g_rec_record)->SetRecordState(g_rec_record, 3) == 0 &&
                     (*g_rec_record)->GetRecordState(g_rec_record, &state) == 0 && state == 3);
    for (i = 0; i < 1000 && *count < enough; i++) usleep(2000);
    REC_STEP(13, *count >= enough);
    REC_STEP(14, (*g_rec_record)->SetRecordState(g_rec_record, 1) == 0 && (*g_rec_record)->GetRecordState(g_rec_record, &state) == 0 && state == 1);
    REC_STEP(15, (*queue)->Clear(queue) == 0 && (*queue)->GetState(queue, &queued) == 0 && queued.count == 0 && queued.index == 0);
    REC_STEP(16, (*queue)->RegisterCallback(queue, 0, 0) == 0);
    (*recorder)->Destroy(recorder);
    g_rec_destroyed = 1;
    return 0;
}

static void *record_thread(void *arg)
{
    int failed = record_once(g_engine, 0, 16000000, 1, 0, &t_native_state.rec_buffers, REC_BUFFERS);
    (void)arg;
    if (!failed) failed = record_once(g_engine, 20, 48000000, 2, &g_rec_turn, &t_native_state.rec_silent, REC_SILENT);
    t_native_state.rec_failed = failed;
    t_native_state.rec_done = 1;
    return 0;
}

static void start_recorder(void)
{
    pthread_t thread;
    if (!g_engine) {
        t_native_state.rec_failed = 99;
        t_native_state.rec_done = 1;
        return;
    }
    pthread_create(&thread, 0, record_thread, 0);
}

static void read_asset_and_save(void)
{
    char path[300];
    void *asset = AAssetManager_open(app.activity->assetManager, "hello.txt", 0), *config, *f;
    if (asset) {
        t_native_state.asset_length = AAsset_getLength(asset);
        AAsset_read(asset, t_native_state.asset_text, sizeof(t_native_state.asset_text) - 1);
        AAsset_close(asset);
    }
    config = AConfiguration_new();
    AConfiguration_fromAssetManager(config, app.activity->assetManager);
    t_native_state.density = AConfiguration_getDensity(config);
    AConfiguration_delete(config);
    snprintf(path, sizeof(path), "%s/save.dat", app.activity->internalDataPath);
    f = fopen(path, "wb");
    if (f) {
        fwrite("saved", 1, 5, f);
        fclose(f);
    }
}

static void handle_input(void)
{
    void *event;
    while (AInputQueue_getEvent(app.queue, &event) >= 0) {
        int handled = 0;
        if (AInputQueue_preDispatchEvent(app.queue, event)) continue;
        if (AInputEvent_getType(event) == 2) {
            t_native_state.last_action = AMotionEvent_getAction(event);
            t_native_state.pointers = (int)AMotionEvent_getPointerCount(event);
            t_native_state.last_x = AMotionEvent_getX(event, 0);
            t_native_state.last_y = AMotionEvent_getY(event, 0);
            /* Counted last: the test waits for the count and then reads the rest. */
            t_native_state.touches++;
            handled = 1;
        } else if (AInputEvent_getType(event) == 1) {
            t_native_state.last_key = AKeyEvent_getKeyCode(event);
            t_native_state.keys++;
            /* Back ends the app, as it does in most games' menus. */
            if (t_native_state.last_key == 4) ANativeActivity_finish(app.activity);
            handled = 1;
        }
        AInputQueue_finishEvent(app.queue, event, handled);
    }
}

static void done(void)
{
    pthread_mutex_lock(&app.lock);
    app.handled = 1;
    pthread_cond_broadcast(&app.cond);
    pthread_mutex_unlock(&app.lock);
}

static void *app_thread(void *arg)
{
    void *looper = ALooper_prepare(1);
    int destroyed = 0;
    (void)arg;
    ALooper_addFd(looper, app.msg_read, 1, 1, 0, 0);
    pthread_mutex_lock(&app.lock);
    app.running = 1;
    pthread_cond_broadcast(&app.cond);
    pthread_mutex_unlock(&app.lock);
    ask_java();
    read_asset_and_save();
    start_audio();
    start_recorder();
    while (!destroyed) {
        int ident, events;
        void *data;
        while ((ident = ALooper_pollAll(app.animating ? 0 : -1, 0, &events, &data)) >= 0) {
            if (ident == 1) {
                char cmd = 0;
                if (read(app.msg_read, &cmd, 1) != 1) continue;
                switch (cmd) {
                case CMD_START: t_native_state.started++; break;
                case CMD_RESUME: t_native_state.resumed++; break;
                case CMD_PAUSE: t_native_state.paused++; app.animating = 0; break;
                case CMD_STOP: t_native_state.stopped++; break;
                case CMD_FOCUS: t_native_state.focused = 1; app.animating = app.surface != 0; break;
                case CMD_BLUR: t_native_state.focused = 0; app.animating = 0; break;
                case CMD_INPUT_CREATED:
                    app.queue = app.pending_queue;
                    AInputQueue_attachLooper(app.queue, looper, 2, 0, 0);
                    break;
                case CMD_INPUT_DESTROYED:
                    AInputQueue_detachLooper(app.queue);
                    app.queue = 0;
                    break;
                case CMD_WINDOW_CREATED:
                    app.window = app.pending_window;
                    init_display();
                    draw_frame();
                    break;
                case CMD_WINDOW_DESTROYED:
                    term_display();
                    t_native_state.finished_drawing = 1;
                    app.window = 0;
                    break;
                case CMD_DESTROY: t_native_state.destroyed++; destroyed = 1; break;
                default: break;
                }
                done();
            } else if (ident == 2 && app.queue) {
                handle_input();
            }
            if (destroyed) break;
        }
        if (app.animating) draw_frame();
    }
    return 0;
}

void ANativeActivity_onCreate(ANativeActivity *activity, void *saved_state, size_t saved_size)
{
    int fds[2];
    pthread_t thread;
    (void)saved_state; (void)saved_size;
    memset(&app, 0, sizeof(app));
    app.activity = activity;
    activity->callbacks->onStart = on_start;
    activity->callbacks->onResume = on_resume;
    activity->callbacks->onPause = on_pause;
    activity->callbacks->onStop = on_stop;
    activity->callbacks->onDestroy = on_destroy;
    activity->callbacks->onWindowFocusChanged = on_focus;
    activity->callbacks->onNativeWindowCreated = on_window_created;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onInputQueueCreated = on_input_created;
    activity->callbacks->onInputQueueDestroyed = on_input_destroyed;
    pipe(fds);
    app.msg_read = fds[0];
    app.msg_write = fds[1];
    pthread_create(&thread, 0, app_thread, 0);
    pthread_mutex_lock(&app.lock);
    while (!app.running) pthread_cond_wait(&app.cond, &app.lock);
    pthread_mutex_unlock(&app.lock);
}
