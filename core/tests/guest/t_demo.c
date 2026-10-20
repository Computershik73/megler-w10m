/*
 * The demo: a NativeActivity app whose picture says, at a glance, whether what
 * the layer does on a device is right. Nothing checks it but the person holding
 * the phone:
 *
 *   an F in the top left corner     the picture is the right way up and not mirrored
 *   red, green, blue squares,       the colour channels are in the right order
 *     left to right, at the bottom
 *   a triangle turning clockwise,   frames arrive, and time passes at the right rate
 *     once in four seconds
 *   a bar along the top edge        the frame rate: the full width is 60 frames a second
 *   a square under each finger      touches arrive where they were made, each finger its own
 *   a tone for the first second,    sound is heard, and without crackle
 *     and while a finger is down
 *
 * Back ends it. It logs what it was given and its frame rate once a second.
 * Written against the NDK directly, like t_native.c.
 */
typedef unsigned int size_t;
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef long pthread_t;

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

struct timespec {
    long tv_sec, tv_nsec;
};

int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pipe(int[2]);
int read(int, void *, size_t);
int write(int, const void *, size_t);
void *memset(void *, int, size_t);
int clock_gettime(int, struct timespec *);
float sinf(float);
float cosf(float);
int __android_log_print(int, const char *, const char *, ...);

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
int AMotionEvent_getPointerId(const void *, size_t);
float AMotionEvent_getX(const void *, size_t);
float AMotionEvent_getY(const void *, size_t);
int AKeyEvent_getKeyCode(const void *);
int AKeyEvent_getAction(const void *);
void ANativeActivity_finish(ANativeActivity *);

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
EGLDisplay eglGetDisplay(void *);
unsigned eglInitialize(EGLDisplay, int *, int *);
unsigned eglChooseConfig(EGLDisplay, const int *, EGLConfig *, int, int *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, void *, const int *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const int *);
unsigned eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
unsigned eglSwapBuffers(EGLDisplay, EGLSurface);
unsigned eglQuerySurface(EGLDisplay, EGLSurface, int, int *);
unsigned eglDestroySurface(EGLDisplay, EGLSurface);
unsigned eglDestroyContext(EGLDisplay, EGLContext);
int eglGetError(void);

void glViewport(int, int, int, int);
void glClearColor(float, float, float, float);
void glClear(unsigned);
unsigned glCreateShader(unsigned);
void glShaderSource(unsigned, int, const char *const *, const int *);
void glCompileShader(unsigned);
void glGetShaderiv(unsigned, unsigned, int *);
void glGetShaderInfoLog(unsigned, int, int *, char *);
unsigned glCreateProgram(void);
void glAttachShader(unsigned, unsigned);
void glBindAttribLocation(unsigned, unsigned, const char *);
void glLinkProgram(unsigned);
void glGetProgramiv(unsigned, unsigned, int *);
void glGetProgramInfoLog(unsigned, int, int *, char *);
void glUseProgram(unsigned);
void glVertexAttribPointer(unsigned, int, unsigned, unsigned char, int, const void *);
void glEnableVertexAttribArray(unsigned);
void glDrawArrays(unsigned, int, int);
unsigned glGetError(void);
const unsigned char *glGetString(unsigned);

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
    void *CreateAudioRecorder, *CreateMidiPlayer, *CreateListener, *Create3DGroup;
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
extern const void *SL_IID_ENGINE, *SL_IID_PLAY, *SL_IID_BUFFERQUEUE;
unsigned slCreateEngine(SLObjectItf *, unsigned, const void *, unsigned, const void *, const void *);

#define TAG "megler-demo"
#define LOG(...) __android_log_print(4, TAG, __VA_ARGS__)

enum { CMD_INPUT_CREATED = 1, CMD_WINDOW_CREATED, CMD_WINDOW_DESTROYED, CMD_FOCUS, CMD_BLUR, CMD_OTHER, CMD_DESTROY, CMD_INPUT_DESTROYED };
enum { MAX_FINGERS = 10, MAX_VERTICES = 6 * 64 };

static struct {
    ANativeActivity *activity;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int msg_read, msg_write;
    void *queue, *pending_queue, *window, *pending_window;
    int running, handled, animating;
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int width, height;
    unsigned program;
    /* x, y, then r, g, b for each vertex */
    float vertices[MAX_VERTICES * 5];
    int vertex_count;
    struct {
        int id, down;
        float x, y;
    } fingers[MAX_FINGERS];
    volatile int fingers_down;
    double started, second_started;
    int frames_this_second, fps;
    int touch_events;
} app;

/* How many buffers the sound output has asked for: it is the audio thread that counts. */
static volatile int g_callbacks;

static double now(void)
{
    struct timespec ts;
    clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* MARK: The activity's side: each callback hands a command to the app's thread and waits for it, as android_native_app_glue does. */

static void send_and_wait(int cmd)
{
    char c = (char)cmd;
    pthread_mutex_lock(&app.lock);
    app.handled = 0;
    write(app.msg_write, &c, 1);
    while (!app.handled) pthread_cond_wait(&app.cond, &app.lock);
    pthread_mutex_unlock(&app.lock);
}

static void on_other(ANativeActivity *a) { (void)a; send_and_wait(CMD_OTHER); }
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

/* MARK: Drawing */

static const char *const VS =
    "attribute vec2 a_position; attribute vec3 a_color; varying vec3 v_color;"
    "void main() { v_color = a_color; gl_Position = vec4(a_position, 0.0, 1.0); }";
static const char *const FS = "precision mediump float; varying vec3 v_color; void main() { gl_FragColor = vec4(v_color, 1.0); }";

static unsigned compile(unsigned kind, const char *source)
{
    unsigned shader = glCreateShader(kind);
    int ok = 0;
    glShaderSource(shader, 1, &source, 0);
    glCompileShader(shader);
    glGetShaderiv(shader, 0x8B81 /* COMPILE_STATUS */, &ok);
    if (!ok) {
        char text[256];
        text[0] = 0;
        glGetShaderInfoLog(shader, sizeof(text), 0, text);
        LOG("the %s shader did not compile: %s", kind == 0x8B31 ? "vertex" : "fragment", text);
    }
    return shader;
}

static void init_display(void)
{
    static const int config_attribs[] = { 0x3033 /* SURFACE_TYPE */, 4 /* WINDOW_BIT */, 0x3040 /* RENDERABLE_TYPE */, 4 /* ES2 */,
                                          0x3022, 8, 0x3023, 8, 0x3024, 8, 0x3038 };
    static const int context_attribs[] = { 0x3098 /* CONTEXT_CLIENT_VERSION */, 2, 0x3038 };
    EGLConfig config = 0;
    int n = 0, ok = 0;
    app.display = eglGetDisplay(0);
    if (!eglInitialize(app.display, 0, 0)) LOG("eglInitialize failed: 0x%x", eglGetError());
    if (!eglChooseConfig(app.display, config_attribs, &config, 1, &n) || n < 1) LOG("eglChooseConfig found nothing: 0x%x", eglGetError());
    app.surface = eglCreateWindowSurface(app.display, config, app.window, 0);
    if (!app.surface) LOG("eglCreateWindowSurface failed: 0x%x", eglGetError());
    app.context = eglCreateContext(app.display, config, 0, context_attribs);
    if (!app.context) LOG("eglCreateContext failed: 0x%x", eglGetError());
    if (!eglMakeCurrent(app.display, app.surface, app.surface, app.context)) LOG("eglMakeCurrent failed: 0x%x", eglGetError());
    eglQuerySurface(app.display, app.surface, 0x3057 /* WIDTH */, &app.width);
    eglQuerySurface(app.display, app.surface, 0x3056 /* HEIGHT */, &app.height);
    glViewport(0, 0, app.width, app.height);
    app.program = glCreateProgram();
    glAttachShader(app.program, compile(0x8B31, VS));
    glAttachShader(app.program, compile(0x8B30, FS));
    glBindAttribLocation(app.program, 0, "a_position");
    glBindAttribLocation(app.program, 1, "a_color");
    glLinkProgram(app.program);
    glGetProgramiv(app.program, 0x8B82 /* LINK_STATUS */, &ok);
    if (!ok) {
        char text[256];
        text[0] = 0;
        glGetProgramInfoLog(app.program, sizeof(text), 0, text);
        LOG("the program did not link: %s", text);
    }
    glUseProgram(app.program);
    LOG("%s on %s, surface %dx%d", (const char *)glGetString(0x1F02 /* VERSION */), (const char *)glGetString(0x1F01 /* RENDERER */), app.width, app.height);
    app.started = app.second_started = now();
    app.frames_this_second = 0;
}

/* A point given in pixels from the top left corner, as touches are. */
static void vertex(float x, float y, float r, float g, float b)
{
    float *v;
    if (app.vertex_count >= MAX_VERTICES) return;
    v = app.vertices + app.vertex_count * 5;
    v[0] = x / (float)app.width * 2.0f - 1.0f;
    v[1] = 1.0f - y / (float)app.height * 2.0f;
    v[2] = r;
    v[3] = g;
    v[4] = b;
    app.vertex_count++;
}

static void rect(float x, float y, float w, float h, float r, float g, float b)
{
    vertex(x, y, r, g, b);
    vertex(x + w, y, r, g, b);
    vertex(x, y + h, r, g, b);
    vertex(x + w, y, r, g, b);
    vertex(x + w, y + h, r, g, b);
    vertex(x, y + h, r, g, b);
}

static void draw_frame(void)
{
    static const float finger_colors[5][3] = { { 1, 1, 0 }, { 0, 1, 1 }, { 1, 0, 1 }, { 1, 0.5f, 0 }, { 0.5f, 1, 0.5f } };
    /* The unit everything is sized in: a twentieth of the shorter side. */
    float u = (float)(app.width < app.height ? app.width : app.height) / 20.0f;
    float cx = (float)app.width * 0.5f, cy = (float)app.height * 0.5f;
    double t = now();
    /* Clockwise on the screen, where y grows downwards: the angle grows. */
    float angle = (float)((t - app.started) * (6.283185307 / 4.0));
    float radius = u * 5.0f;
    int i;
    if (!app.surface) return;
    app.vertex_count = 0;

    /* The frame rate: the full width is 60 frames a second. */
    rect(0, 0, (float)app.width * (float)(app.fps > 60 ? 60 : app.fps) / 60.0f, u * 0.4f, 0.2f, 0.9f, 0.3f);

    /* F, in the top left corner: a stem and two arms. */
    rect(u, u * 1.5f, u, u * 5, 1, 1, 1);
    rect(u * 2, u * 1.5f, u * 2.5f, u, 1, 1, 1);
    rect(u * 2, u * 3.5f, u * 1.5f, u, 1, 1, 1);

    /* Red, green, blue, left to right along the bottom. */
    rect(u, (float)app.height - u * 3, u * 2, u * 2, 1, 0, 0);
    rect(u * 4, (float)app.height - u * 3, u * 2, u * 2, 0, 1, 0);
    rect(u * 7, (float)app.height - u * 3, u * 2, u * 2, 0, 0, 1);

    /* The triangle, a colour at each corner. */
    for (i = 0; i < 3; i++) {
        float a = angle + (float)i * 2.0943951f;
        vertex(cx + sinf(a) * radius, cy - cosf(a) * radius, i == 0 ? 1.0f : 0.1f, i == 1 ? 1.0f : 0.1f, i == 2 ? 1.0f : 0.1f);
    }

    for (i = 0; i < MAX_FINGERS; i++) {
        const float *c = finger_colors[i % 5];
        if (!app.fingers[i].down) continue;
        rect(app.fingers[i].x - u * 1.5f, app.fingers[i].y - u * 1.5f, u * 3, u * 3, c[0], c[1], c[2]);
        /* A cross through the point itself, to the edges: an offset shows against it. */
        rect(0, app.fingers[i].y - 1, (float)app.width, 2, c[0], c[1], c[2]);
        rect(app.fingers[i].x - 1, 0, 2, (float)app.height, c[0], c[1], c[2]);
    }

    glClearColor(0.08f, 0.10f, 0.16f, 1.0f);
    glClear(0x4000);
    glVertexAttribPointer(0, 2, 0x1406 /* FLOAT */, 0, 5 * sizeof(float), app.vertices);
    glVertexAttribPointer(1, 3, 0x1406, 0, 5 * sizeof(float), app.vertices + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(4 /* TRIANGLES */, 0, app.vertex_count);
    eglSwapBuffers(app.display, app.surface);

    app.frames_this_second++;
    if (t - app.second_started >= 1.0) {
        unsigned error = glGetError();
        app.fps = (int)((double)app.frames_this_second / (t - app.second_started) + 0.5);
        LOG("%d frames a second, %d touch events, %d fingers down, %d sound buffers asked for%s", app.fps, app.touch_events, app.fingers_down, g_callbacks,
            error ? ", a GL error" : "");
        if (error) LOG("glGetError: 0x%x", error);
        app.frames_this_second = 0;
        app.second_started = t;
    }
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

/* MARK: Sound: a 440 Hz tone for the first second and while a finger is down, silence otherwise, in buffers of a hundredth of a second. */

enum { RATE = 44100, CHUNK = 441 };
static short g_pcm[2][CHUNK];
static int g_which, g_filled;
static float g_phase, g_level;

static void fill(short *out)
{
    /* The level glides to where it is going, so a touch does not click. */
    float target = app.fingers_down > 0 || g_filled < 100 ? 0.25f : 0.0f;
    int i;
    g_filled++;
    for (i = 0; i < CHUNK; i++) {
        g_level += (target - g_level) * 0.002f;
        out[i] = (short)(sinf(g_phase) * g_level * 32767.0f);
        g_phase += 6.2831853f * 440.0f / (float)RATE;
        if (g_phase > 6.2831853f) g_phase -= 6.2831853f;
    }
}

static void queue_callback(SLBufferQueueItf queue, void *context)
{
    (void)context;
    g_callbacks++;
    fill(g_pcm[g_which]);
    (*queue)->Enqueue(queue, g_pcm[g_which], sizeof(g_pcm[0]));
    g_which ^= 1;
}

static void start_audio(void)
{
    SLObjectItf engine_object = 0, mix = 0, player = 0;
    SLEngineItf engine = 0;
    SLPlayItf play = 0;
    SLBufferQueueItf queue = 0;
    /* SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE with two buffers; PCM, mono, 44100 Hz (in millihertz), 16 bits, front centre, little-endian */
    unsigned locator[2] = { 0x800007BDu, 2 }, format[7] = { 2, 1, RATE * 1000u, 16, 16, 4, 2 }, out_locator[2] = { 4, 0 };
    void *source[2], *sink[2];
    source[0] = locator;
    source[1] = format;
    sink[0] = out_locator;
    sink[1] = 0;
    if (slCreateEngine(&engine_object, 0, 0, 0, 0, 0) != 0) { LOG("slCreateEngine failed"); return; }
    (*engine_object)->Realize(engine_object, 0);
    (*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine);
    (*engine)->CreateOutputMix(engine, &mix, 0, 0, 0);
    (*mix)->Realize(mix, 0);
    out_locator[1] = (unsigned)mix;
    if ((*engine)->CreateAudioPlayer(engine, &player, source, sink, 1, &SL_IID_BUFFERQUEUE, 0) != 0) { LOG("CreateAudioPlayer failed"); return; }
    (*player)->Realize(player, 0);
    (*player)->GetInterface(player, SL_IID_PLAY, &play);
    (*player)->GetInterface(player, SL_IID_BUFFERQUEUE, &queue);
    if (!play || !queue) { LOG("the player has no play or buffer queue interface"); return; }
    (*queue)->RegisterCallback(queue, queue_callback, 0);
    queue_callback(queue, 0);
    queue_callback(queue, 0);
    (*play)->SetPlayState(play, 3 /* PLAYING */);
    LOG("sound: a buffer queue player at %d Hz", RATE);
}

/* MARK: Input */

static void set_finger(int id, int down, float x, float y)
{
    int i, free_slot = -1;
    for (i = 0; i < MAX_FINGERS; i++) {
        if (app.fingers[i].down && app.fingers[i].id == id) break;
        if (!app.fingers[i].down && free_slot < 0) free_slot = i;
    }
    if (i == MAX_FINGERS) {
        if (!down || free_slot < 0) return;
        i = free_slot;
    }
    app.fingers[i].id = id;
    app.fingers[i].down = down;
    app.fingers[i].x = x;
    app.fingers[i].y = y;
}

static void handle_motion(const void *event)
{
    int action = AMotionEvent_getAction(event), masked = action & 0xFF, index = (action >> 8) & 0xFF;
    int count = (int)AMotionEvent_getPointerCount(event), i, down = 0;
    app.touch_events++;
    if (masked == 3 /* CANCEL */) {
        memset(app.fingers, 0, sizeof(app.fingers));
    } else {
        for (i = 0; i < count; i++) {
            /* UP lifts the only finger, POINTER_UP the one the action names. */
            int lifted = masked == 1 || (masked == 6 && i == index);
            set_finger(AMotionEvent_getPointerId(event, (size_t)i), !lifted, AMotionEvent_getX(event, (size_t)i), AMotionEvent_getY(event, (size_t)i));
        }
    }
    for (i = 0; i < MAX_FINGERS; i++) down += app.fingers[i].down;
    if (masked != 2 /* MOVE */)
        LOG("touch: action %d, pointer %d of %d, at %d,%d; %d down", masked, index, count, (int)AMotionEvent_getX(event, (size_t)(index < count ? index : 0)),
            (int)AMotionEvent_getY(event, (size_t)(index < count ? index : 0)), down);
    app.fingers_down = down;
}

static void handle_input(void)
{
    void *event;
    while (AInputQueue_getEvent(app.queue, &event) >= 0) {
        int handled = 0;
        if (AInputQueue_preDispatchEvent(app.queue, event)) continue;
        if (AInputEvent_getType(event) == 2 /* MOTION */) {
            handle_motion(event);
            handled = 1;
        } else if (AInputEvent_getType(event) == 1 /* KEY */) {
            if (AKeyEvent_getKeyCode(event) == 4 /* BACK */) {
                if (AKeyEvent_getAction(event) == 1 /* UP */) ANativeActivity_finish(app.activity);
                handled = 1;
            }
        }
        AInputQueue_finishEvent(app.queue, event, handled);
    }
}

/* MARK: The app's thread */

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
    start_audio();
    while (!destroyed) {
        int ident, events;
        void *data;
        while ((ident = ALooper_pollAll(app.animating ? 0 : -1, 0, &events, &data)) >= 0) {
            if (ident == 1) {
                char cmd = 0;
                if (read(app.msg_read, &cmd, 1) != 1) continue;
                switch (cmd) {
                case CMD_FOCUS: app.animating = app.surface != 0; break;
                case CMD_BLUR:
                    app.animating = 0;
                    memset(app.fingers, 0, sizeof(app.fingers));
                    app.fingers_down = 0;
                    break;
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
                    app.window = 0;
                    break;
                case CMD_DESTROY: destroyed = 1; break;
                default: break;
                }
                pthread_mutex_lock(&app.lock);
                app.handled = 1;
                pthread_cond_broadcast(&app.cond);
                pthread_mutex_unlock(&app.lock);
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
    activity->callbacks->onStart = on_other;
    activity->callbacks->onResume = on_other;
    activity->callbacks->onPause = on_other;
    activity->callbacks->onStop = on_other;
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
