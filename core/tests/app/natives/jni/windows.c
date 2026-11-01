/*
 * The second library of the test app `natives` (megler.test.natives.Windows): native code on SurfaceViews, as a game engine
 * draws on the one its Java hands it -- Asphalt 9's way: ANativeWindow_fromSurface in surfaceChanged on the main thread, the
 * window handed to a render thread of the library's own (a pthread, never attached to Java), which picks an EGL
 * configuration out of eglGetConfigs, makes an OpenGL ES 3 context, gives the window its buffers' geometry, makes a window
 * surface on it and swaps it continuously; surfaceDestroyed lets go of the window without waiting for the renderer, which
 * finds out at its next swap. And ANativeWindow_lock on another SurfaceView, in RGB 565 at a size of its own; and a TextureView's
 * SurfaceTexture drawn through its Surface's window, first in software (RGBX_8888), then once with EGL.
 *
 * The renderer says what it saw in the log and how far it has come in `phase`, which the Java side (WindowTest) reads:
 * 1 thirty frames on the first window; 2 its surface resized by the window and ten frames at the new size; 3 the window gone
 * (a swap refused) and the surface destroyed; 4 thirty frames on the second window. Each frame is cleared to one colour
 * and its top left quarter drawn in another (a triangle pair), so that the picture shows where the frame went and which way up.
 */
#include <jni.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "NativesWindow"
#define SAY(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static ANativeWindow *g_java_window;   /* the window surfaceChanged holds until surfaceDestroyed */
static ANativeWindow *g_next;          /* a window handed to the renderer and not taken yet: a reference of its own */
static int g_windows;                  /* windows handed over */
static int g_stop, g_started;
static volatile int g_phase;
static pthread_t g_thread;

/* ------------------------------------------------------------- the renderer */

static GLuint shader(GLenum type, const char *source)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, NULL);
    glCompileShader(s);
    return s;
}

static EGLConfig choose(EGLDisplay d, EGLint *count)
{
    EGLint n = 0, i;
    EGLConfig *all, chosen = NULL;
    eglGetConfigs(d, NULL, 0, &n);
    *count = n;
    if (n <= 0) return NULL;
    all = (EGLConfig *)calloc((size_t)n, sizeof(EGLConfig));
    eglGetConfigs(d, all, n, &n);
    for (i = 0; i < n && !chosen; i++) {
        EGLint type = 0, renderable = 0, r = 0, g = 0, b = 0, a = 0;
        eglGetConfigAttrib(d, all[i], EGL_SURFACE_TYPE, &type);
        eglGetConfigAttrib(d, all[i], EGL_RENDERABLE_TYPE, &renderable);
        eglGetConfigAttrib(d, all[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(d, all[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(d, all[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(d, all[i], EGL_ALPHA_SIZE, &a);
        if ((type & EGL_WINDOW_BIT) && (renderable & 0x44) && r == 8 && g == 8 && b == 8 && a == 8) chosen = all[i];
    }
    free(all);
    return chosen;
}

static void *render_main(void *arg)
{
    static const EGLint es3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, 0x30FB /* EGL_CONTEXT_MINOR_VERSION_KHR */, 0, EGL_NONE };
    static const GLfloat quarter[] = { -1, 0, 0, 0, 0, 1, -1, 0, 0, 1, -1, 1 };
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0, configs = 0, version = 0;
    EGLConfig config;
    EGLContext ctx;
    GLuint program = 0;
    GLint position = -1, colour = -1;
    (void)arg;
    eglInitialize(d, &major, &minor);
    config = choose(d, &configs);
    ctx = config ? eglCreateContext(d, config, EGL_NO_CONTEXT, es3) : EGL_NO_CONTEXT;
    if (ctx != EGL_NO_CONTEXT) eglQueryContext(d, ctx, EGL_CONTEXT_CLIENT_VERSION, &version);
    SAY("render: display egl=%d.%d configs=%d chosen=%d context=%d version=%d", major, minor, configs, config != NULL, ctx != EGL_NO_CONTEXT, version);
    for (;;) {
        ANativeWindow *win;
        EGLSurface surf;
        ANativeWindow_Buffer buffer;
        EGLint w = 0, h = 0, lw = 0, lh = 0;
        int number, format, frames = 0, resized_at = -1, made, current, refused, width;
        const GLfloat *fill, *half;
        static const GLfloat magenta[] = { 1, 0, 1, 1 }, yellow[] = { 1, 1, 0, 1 }, cyan[] = { 0, 1, 1, 1 }, blue[] = { 0, 0, 1, 1 };
        pthread_mutex_lock(&g_lock);
        while (!g_next && !g_stop) pthread_cond_wait(&g_wake, &g_lock);
        win = g_next;
        g_next = NULL;
        number = g_windows;
        pthread_mutex_unlock(&g_lock);
        if (!win) break;
        if (ctx == EGL_NO_CONTEXT) {
            ANativeWindow_release(win);
            continue;
        }
        /* As Asphalt 9: the window's own format back to it with the geometry; the first window at its own size, the second at 64 x 32. */
        format = ANativeWindow_getFormat(win);
        if (number == 1) ANativeWindow_setBuffersGeometry(win, 0, 0, format);
        else ANativeWindow_setBuffersGeometry(win, 64, 32, format);
        surf = eglCreateWindowSurface(d, config, win, NULL);
        made = surf != EGL_NO_SURFACE && eglMakeCurrent(d, surf, surf, ctx);
        current = made && eglGetCurrentSurface(EGL_DRAW) == surf && eglGetCurrentSurface(EGL_READ) == surf;
        eglQuerySurface(d, surf, EGL_WIDTH, &w);
        eglQuerySurface(d, surf, EGL_HEIGHT, &h);
        /* A window an EGL surface is on cannot be locked as well. */
        refused = ANativeWindow_lock(win, &buffer, NULL);
        SAY("render: window %d surface %dx%d made=%d current=%d format=%d lock=%d", number, w, h, made, current, format, refused);
        if (!made) {
            if (surf != EGL_NO_SURFACE) eglDestroySurface(d, surf);
            ANativeWindow_release(win);
            continue;
        }
        if (!program) {
            program = glCreateProgram();
            glAttachShader(program, shader(GL_VERTEX_SHADER, "attribute vec4 aPosition;\nvoid main() { gl_Position = aPosition; }\n"));
            glAttachShader(program, shader(GL_FRAGMENT_SHADER, "precision mediump float;\nuniform vec4 uColor;\nvoid main() { gl_FragColor = uColor; }\n"));
            glLinkProgram(program);
            position = glGetAttribLocation(program, "aPosition");
            colour = glGetUniformLocation(program, "uColor");
        }
        fill = number == 1 ? magenta : cyan;
        half = number == 1 ? yellow : blue;
        lw = w;
        lh = h;
        for (;;) {
            int stop;
            eglQuerySurface(d, surf, EGL_WIDTH, &w);
            eglQuerySurface(d, surf, EGL_HEIGHT, &h);
            if (w != lw || h != lh) {
                SAY("render: window %d surface now %dx%d after %d frames", number, w, h, frames);
                resized_at = frames;
                lw = w;
                lh = h;
            }
            glViewport(0, 0, w, h);
            glClearColor(fill[0], fill[1], fill[2], fill[3]);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(program);
            glUniform4fv(colour, 1, half);
            glVertexAttribPointer((GLuint)position, 2, GL_FLOAT, GL_FALSE, 0, quarter);
            glEnableVertexAttribArray((GLuint)position);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            if (!eglSwapBuffers(d, surf)) {
                EGLint error = eglGetError();
                width = ANativeWindow_getWidth(win);
                SAY("render: window %d swap refused error=0x%x after %d frames width=%d", number, error, frames, width);
                break;
            }
            frames++;
            if (number == 1 && frames == 30 && g_phase < 1) g_phase = 1;
            if (number == 1 && resized_at >= 0 && frames == resized_at + 10) g_phase = 2;
            if (number == 2 && frames == 30) g_phase = 4;
            pthread_mutex_lock(&g_lock);
            stop = g_stop;
            pthread_mutex_unlock(&g_lock);
            if (stop) break;
        }
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(d, surf);
        ANativeWindow_release(win);
        if (number == 1) g_phase = 3;
    }
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(d, ctx);
    SAY("render: ended");
    return NULL;
}

/* -------------------------------------------------------------- from Java */

/* surfaceChanged of the EGL view: the window of its Surface, handed to the renderer when it is a new one. */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Windows_surfaceChanged(JNIEnv *env, jclass cls, jobject surface, jint w, jint h)
{
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    int same, number, width = win ? ANativeWindow_getWidth(win) : -1, height = win ? ANativeWindow_getHeight(win) : -1, format = win ? ANativeWindow_getFormat(win) : -1;
    char text[120];
    (void)cls;
    pthread_mutex_lock(&g_lock);
    same = win && win == g_java_window;
    if (same) {
        ANativeWindow_release(win);
    } else if (win) {
        if (g_java_window) ANativeWindow_release(g_java_window);
        g_java_window = win;
        /* The renderer's own reference, taken by it with the window. */
        ANativeWindow_acquire(win);
        if (g_next) ANativeWindow_release(g_next);
        g_next = win;
        g_windows++;
        pthread_cond_broadcast(&g_wake);
    }
    number = g_windows;
    if (!g_started && win) {
        g_started = 1;
        pthread_create(&g_thread, NULL, render_main, NULL);
    }
    pthread_mutex_unlock(&g_lock);
    snprintf(text, sizeof(text), "window=%d same=%d %dx%d format=%d for %dx%d", win ? number : 0, same, width, height, format, w, h);
    return (*env)->NewStringUTF(env, text);
}

/* surfaceDestroyed: the window let go of at once, as Asphalt 9 does; the renderer is not waited for. */
JNIEXPORT void JNICALL Java_megler_test_natives_Windows_surfaceDestroyed(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    pthread_mutex_lock(&g_lock);
    if (g_java_window) ANativeWindow_release(g_java_window);
    g_java_window = NULL;
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jint JNICALL Java_megler_test_natives_Windows_phase(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return g_phase;
}

/* The activity ends: the renderer finishes what it is doing and ends; waited for. */
JNIEXPORT void JNICALL Java_megler_test_natives_Windows_stop(JNIEnv *env, jclass cls)
{
    int started;
    (void)env; (void)cls;
    pthread_mutex_lock(&g_lock);
    g_stop = 1;
    started = g_started;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    if (started) pthread_join(g_thread, NULL);
}

/* Red, green above, blue, white below: the quarters of a frame drawn in software. */
static int quarter_of(int x, int y, int width, int height) { return (y < height / 2 ? 0 : 2) + (x < width / 2 ? 0 : 1); }

/*
 * A TextureView's surface (a Surface made on its SurfaceTexture) drawn in software: RGBX_8888 at the texture's size, in the
 * four quarters' colours, R, G, B and a byte the window ignores.
 */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Windows_textureLock(JNIEnv *env, jclass cls, jobject surface)
{
    static const uint8_t colours[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    ANativeWindow_Buffer buffer;
    char text[120];
    int geometry, rc, post = -99, x, y;
    (void)cls;
    if (!win) return (*env)->NewStringUTF(env, "no window");
    geometry = ANativeWindow_setBuffersGeometry(win, 0, 0, WINDOW_FORMAT_RGBX_8888);
    memset(&buffer, 0, sizeof(buffer));
    rc = ANativeWindow_lock(win, &buffer, NULL);
    if (rc == 0) {
        for (y = 0; y < buffer.height; y++) {
            uint8_t *row = (uint8_t *)buffer.bits + (size_t)y * (size_t)buffer.stride * 4;
            for (x = 0; x < buffer.width; x++) {
                memcpy(row + x * 4, colours[quarter_of(x, y, buffer.width, buffer.height)], 3);
                row[x * 4 + 3] = 0;
            }
        }
        post = ANativeWindow_unlockAndPost(win);
    }
    snprintf(text, sizeof(text), "%dx%d stride=%d format=%d geometry=%d rc=%d post=%d", buffer.width, buffer.height, buffer.stride, buffer.format, geometry, rc, post);
    ANativeWindow_release(win);
    return (*env)->NewStringUTF(env, text);
}

/*
 * The same TextureView's surface destroyed by the calling thread while a thread of the library's has it current -- HERE
 * WeGo's way: its UI thread makes its map's surface anew while its render thread is still drawing a frame in the old one. The
 * destroy is taken, and the surface stays the thread's to query, draw on and swap until it lets it go; only then does it end,
 * as EGL has it.
 */
typedef struct elsewhere {
    EGLDisplay d;
    EGLSurface surf;
    EGLContext ctx;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int step;                          /* 1 current on the thread, 2 destroyed by the caller, 3 the thread done */
    int made, queried, swapped, released;
    EGLint width, error;
} elsewhere;

static void *elsewhere_main(void *p)
{
    elsewhere *e = (elsewhere *)p;
    e->made = eglMakeCurrent(e->d, e->surf, e->surf, e->ctx);
    pthread_mutex_lock(&e->lock);
    e->step = 1;
    pthread_cond_broadcast(&e->cond);
    while (e->step < 2) pthread_cond_wait(&e->cond, &e->lock);
    pthread_mutex_unlock(&e->lock);
    if (e->made) {
        e->queried = eglQuerySurface(e->d, e->surf, EGL_WIDTH, &e->width);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        e->swapped = eglSwapBuffers(e->d, e->surf);
        e->error = eglGetError();
        e->released = eglMakeCurrent(e->d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    pthread_mutex_lock(&e->lock);
    e->step = 3;
    pthread_cond_broadcast(&e->cond);
    pthread_mutex_unlock(&e->lock);
    return NULL;
}

JNIEXPORT jstring JNICALL Java_megler_test_natives_Windows_textureEglElsewhere(JNIEnv *env, jclass cls, jobject surface)
{
    static const EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    EGLint configs = 0;
    EGLConfig config;
    EGLBoolean destroyed = EGL_FALSE;
    elsewhere e;
    pthread_t t;
    char text[160];
    (void)cls;
    if (!win) return (*env)->NewStringUTF(env, "no window");
    memset(&e, 0, sizeof(e));
    pthread_mutex_init(&e.lock, NULL);
    pthread_cond_init(&e.cond, NULL);
    e.d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(e.d, NULL, NULL);
    config = choose(e.d, &configs);
    e.ctx = eglCreateContext(e.d, config, EGL_NO_CONTEXT, es2);
    e.surf = eglCreateWindowSurface(e.d, config, win, NULL);
    if (e.surf != EGL_NO_SURFACE && e.ctx != EGL_NO_CONTEXT && pthread_create(&t, NULL, elsewhere_main, &e) == 0) {
        pthread_mutex_lock(&e.lock);
        while (e.step < 1) pthread_cond_wait(&e.cond, &e.lock);
        pthread_mutex_unlock(&e.lock);
        destroyed = eglDestroySurface(e.d, e.surf);
        pthread_mutex_lock(&e.lock);
        e.step = 2;
        pthread_cond_broadcast(&e.cond);
        while (e.step < 3) pthread_cond_wait(&e.cond, &e.lock);
        pthread_mutex_unlock(&e.lock);
        pthread_join(t, NULL);
    }
    if (e.ctx != EGL_NO_CONTEXT) eglDestroyContext(e.d, e.ctx);
    ANativeWindow_release(win);
    snprintf(text, sizeof(text), "made=%d destroyed=%d queried=%d width=%d swapped=%d error=0x%x released=%d", e.made, destroyed, e.queried,
             e.width, e.swapped, e.error, e.released);
    return (*env)->NewStringUTF(env, text);
}

/*
 * The same TextureView's surface drawn once with EGL, on the calling thread (the main one): an OpenGL ES 2 context, a window
 * surface at the texture's size, the frame cleared to green with its top left quarter red, one swap, all of it ended.
 */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Windows_textureEgl(JNIEnv *env, jclass cls, jobject surface)
{
    static const EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    static const GLfloat quarter[] = { -1, 0, 0, 0, 0, 1, -1, 0, 0, 1, -1, 1 }, red[] = { 1, 0, 0, 1 };
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint configs = 0, w = 0, h = 0, error;
    EGLConfig config;
    EGLContext ctx;
    EGLSurface surf;
    GLuint program;
    int made, swapped = 0;
    char text[120];
    (void)cls;
    if (!win) return (*env)->NewStringUTF(env, "no window");
    eglInitialize(d, NULL, NULL);
    config = choose(d, &configs);
    ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, es2);
    surf = eglCreateWindowSurface(d, config, win, NULL);
    made = surf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT && eglMakeCurrent(d, surf, surf, ctx);
    if (made) {
        eglQuerySurface(d, surf, EGL_WIDTH, &w);
        eglQuerySurface(d, surf, EGL_HEIGHT, &h);
        program = glCreateProgram();
        glAttachShader(program, shader(GL_VERTEX_SHADER, "attribute vec4 aPosition;\nvoid main() { gl_Position = aPosition; }\n"));
        glAttachShader(program, shader(GL_FRAGMENT_SHADER, "precision mediump float;\nuniform vec4 uColor;\nvoid main() { gl_FragColor = uColor; }\n"));
        glLinkProgram(program);
        glViewport(0, 0, w, h);
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program);
        glUniform4fv(glGetUniformLocation(program, "uColor"), 1, red);
        glVertexAttribPointer((GLuint)glGetAttribLocation(program, "aPosition"), 2, GL_FLOAT, GL_FALSE, 0, quarter);
        glEnableVertexAttribArray((GLuint)glGetAttribLocation(program, "aPosition"));
        glDrawArrays(GL_TRIANGLES, 0, 6);
        swapped = eglSwapBuffers(d, surf);
    }
    error = eglGetError();
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(d, surf);
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(d, ctx);
    ANativeWindow_release(win);
    snprintf(text, sizeof(text), "surface %dx%d made=%d swapped=%d error=0x%x", w, h, made, swapped, error);
    return (*env)->NewStringUTF(env, text);
}

/*
 * The lock view's surface, drawn in software: RGB 565 at 32 x 16 whatever the view's size, in the quarters' colours, written
 * row by row at the buffer's stride, then posted.
 */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Windows_lockPaint(JNIEnv *env, jclass cls, jobject surface)
{
    ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
    ANativeWindow_Buffer buffer;
    char text[120];
    int geometry, rc, post = -99, x, y;
    (void)cls;
    if (!win) return (*env)->NewStringUTF(env, "no window");
    geometry = ANativeWindow_setBuffersGeometry(win, 32, 16, WINDOW_FORMAT_RGB_565);
    memset(&buffer, 0, sizeof(buffer));
    rc = ANativeWindow_lock(win, &buffer, NULL);
    if (rc == 0) {
        for (y = 0; y < buffer.height; y++) {
            uint16_t *row = (uint16_t *)buffer.bits + (size_t)y * (size_t)buffer.stride;
            static const uint16_t colours[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
            for (x = 0; x < buffer.width; x++) row[x] = colours[quarter_of(x, y, buffer.width, buffer.height)];
        }
        post = ANativeWindow_unlockAndPost(win);
    }
    snprintf(text, sizeof(text), "%dx%d stride=%d format=%d geometry=%d rc=%d post=%d", buffer.width, buffer.height, buffer.stride, buffer.format, geometry, rc, post);
    ANativeWindow_release(win);
    return (*env)->NewStringUTF(env, text);
}
