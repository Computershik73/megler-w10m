/*
 * The second library of the test app `natives` (megler.test.natives.Windows): native code on SurfaceViews, as a game engine
 * draws on the one its Java hands it -- Asphalt 9's way: ANativeWindow_fromSurface in surfaceChanged on the main thread, the
 * window handed to a render thread of the library's own (a pthread, never attached to Java), which picks an EGL
 * configuration out of eglGetConfigs, makes an OpenGL ES 3 context, gives the window its buffers' geometry, makes a window
 * surface on it and swaps it continuously; surfaceDestroyed lets go of the window without waiting for the renderer, which
 * finds out at its next swap. And ANativeWindow_lock on another SurfaceView, in RGB 565 at a size of its own.
 *
 * The renderer says what it saw in the log and how far it has come in `phase`, which the Java side (WindowTest) reads:
 * 1 thirty frames on the first window; 2 its surface resized by the window and ten frames at the new size; 3 the window gone
 * (a swap refused) and the surface destroyed; 4 thirty frames on the second window. Each frame is cleared to one colour
 * and its left half drawn in another (a triangle pair), so that the picture shows where the frame's halves went.
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
    static const GLfloat left[] = { -1, -1, 0, -1, 0, 1, -1, -1, 0, 1, -1, 1 };
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
            glVertexAttribPointer((GLuint)position, 2, GL_FLOAT, GL_FALSE, 0, left);
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

/*
 * The lock view's surface, drawn in software: RGB 565 at 32 x 16 whatever the view's size, in quarters -- red, green above,
 * blue, white below -- written row by row at the buffer's stride, then posted.
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
            for (x = 0; x < buffer.width; x++) {
                int top = y < buffer.height / 2, l = x < buffer.width / 2;
                row[x] = top ? (l ? 0xF800 : 0x07E0) : (l ? 0x001F : 0xFFFF);
            }
        }
        post = ANativeWindow_unlockAndPost(win);
    }
    snprintf(text, sizeof(text), "%dx%d stride=%d format=%d geometry=%d rc=%d post=%d", buffer.width, buffer.height, buffer.stride, buffer.format, geometry, rc, post);
    ANativeWindow_release(win);
    return (*env)->NewStringUTF(env, text);
}
