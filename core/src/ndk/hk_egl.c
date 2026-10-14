#include "hk_egl.h"

#include <stdio.h>

#include "../bionic/hk_bionic.h"
#include "../hk_core.h"
#include "../host/hk_host.h"
#include "../run/hk_run.h"
#include "hk_ndk.h"

/* ------------------------------------------------------------- the source */

static void *(*g_provider)(const char *name);
static void *g_lib_egl, *g_lib_gles;
static bool g_angle_tried;
static hk_lock g_egl_lock = HK_LOCK_INIT;      /* the display */
static hk_lock g_angle_lock = HK_LOCK_INIT;    /* loading ANGLE: taken while the display's lock is held */

void hk_egl_set_provider(void *(*resolve)(const char *name)) { g_provider = resolve; }

/* ANGLE's own function by name. */
static void *angle(const char *name)
{
    void *p;
    if (g_provider) return g_provider(name);
    if (!g_angle_tried) {
        hk_lock_acquire(&g_angle_lock);
        if (!g_angle_tried) {
            g_lib_gles = hk_dl_open("libGLESv2.dll");
            g_lib_egl = hk_dl_open("libEGL.dll");
            if (!g_lib_egl || !g_lib_gles)
                hk_err("egl: ANGLE did not load (libEGL.dll %s, libGLESv2.dll %s)", g_lib_egl ? "ok" : "missing", g_lib_gles ? "ok" : "missing");
            g_angle_tried = true;
        }
        hk_lock_release(&g_angle_lock);
    }
    p = g_lib_egl ? hk_dl_sym(g_lib_egl, name) : NULL;
    if (!p && g_lib_gles) p = hk_dl_sym(g_lib_gles, name);
    return p;
}

/* -------------------------------------------------------------- EGL names */

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef unsigned EGLBoolean, EGLenum;

#define EGL_SUCCESS 0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_ALLOC 0x3003
#define EGL_BAD_DISPLAY 0x3008
#define EGL_BAD_NATIVE_WINDOW 0x300B
#define EGL_BAD_SURFACE 0x300D
#define EGL_SURFACE_TYPE 0x3033
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_RED_SIZE 0x3024
#define EGL_NONE 0x3038
#define EGL_PBUFFER_BIT 1
#define EGL_WINDOW_BIT 4
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES2_BIT 4
#define EGL_OPENGL_ES3_BIT 0x40
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE 0x3204
#define EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE 0x3205
#define EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE 0x3208
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE 0x3209
#define EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE 0x320B
#define EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE 0x320F
#define EGL_DEVICE_EXT 0x322C
#define EGL_D3D11_DEVICE_ANGLE 0x33A1
#define EGL_D3D_TEXTURE_ANGLE 0x33A3

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_MAX_TEXTURE_SIZE 0x0D33

#define A(type, name) ((type)angle(name))

typedef EGLDisplay (*fn_platform_display)(EGLenum, void *, const EGLint *);
typedef EGLBoolean (*fn_initialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLint (*fn_get_error)(void);
typedef const char *(*fn_query_string)(EGLDisplay, EGLint);
typedef EGLBoolean (*fn_choose_config)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean (*fn_get_configs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
typedef EGLBoolean (*fn_get_config_attrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
typedef EGLSurface (*fn_pbuffer)(EGLDisplay, EGLConfig, const EGLint *);
typedef EGLSurface (*fn_pbuffer_from)(EGLDisplay, EGLenum, void *, EGLConfig, const EGLint *);
typedef EGLBoolean (*fn_destroy_surface)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_query_surface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
typedef EGLContext (*fn_create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (*fn_destroy_context)(EGLDisplay, EGLContext);
typedef EGLBoolean (*fn_make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*fn_swap)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*fn_query_display_attrib)(EGLDisplay, EGLint, intptr_t *);
typedef EGLBoolean (*fn_query_device_attrib)(void *, EGLint, intptr_t *);
typedef const u8 *(*fn_gl_get_string)(unsigned);
typedef void (*fn_gl_get_integerv)(unsigned, int *);
typedef void (*fn_gl_void)(void);

/* ------------------------------------------------------------ the display */

static EGLDisplay g_display;
static bool g_initialized, g_device_attached;
static EGLint g_major, g_minor;
static int g_d3d_level;                /* 0x9300, 0xA000, 0xB100...; 0 when unknown */
static void *g_d3d_device;
static char g_how[48];                 /* which attempt made the display */
static HK_THREAD_LOCAL EGLint g_error = EGL_SUCCESS;

static EGLDisplay make_display(void)
{
    /* What a feature-level-10+ GPU takes, then what a 9_3 phone needs; software rendering is not worth trying on a phone. */
    static const EGLint by_default[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE, 1,
        EGL_NONE,
    };
    static const EGLint level_9_3[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, 9,
        EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, 3,
        EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE, 1,
        EGL_NONE,
    };
    static const struct { const EGLint *attribs; const char *name; } tries[] = {
        { by_default, "Direct3D 11" },
        { level_9_3, "Direct3D 11 at feature level 9_3" },
    };
    fn_platform_display get = A(fn_platform_display, "eglGetPlatformDisplayEXT");
    fn_initialize init = A(fn_initialize, "eglInitialize");
    size_t i;
    if (!get) get = A(fn_platform_display, "eglGetPlatformDisplay");
    if (!get || !init) return NULL;
    for (i = 0; i < HK_COUNT(tries); i++) {
        EGLDisplay d;
        /* Said before it is tried: this is the first thing ANGLE does on a device, and where a bad package would end the process. */
        hk_info("egl: asking ANGLE for a display as %s", tries[i].name);
        d = get(EGL_PLATFORM_ANGLE_ANGLE, NULL, tries[i].attribs);
        if (d && init(d, &g_major, &g_minor)) {
            hk_strlcpy(g_how, tries[i].name, sizeof(g_how));
            return d;
        }
        hk_warn("egl: no display as %s", tries[i].name);
    }
    return NULL;
}

/* ------------------------------------------ the thread that outlives the app's
 *
 * Making ANGLE's display, and asking the shell for a swap chain on its
 * device, leave things behind that belong to the thread that did it: Windows
 * keeps a hidden window for a thread that has spoken to the UI thread's
 * dispatcher or to the application's events, and the window goes when the
 * thread ends. An app may set its graphics up on a thread that then ends.
 * Unity does, and half a second later the shell's UI thread brought the whole
 * app down, inside the system, posting to a window that was no longer there.
 * So these few things are done on a thread of this layer's own, which never
 * ends; the thread that asked waits for it.
 */
static struct {
    hk_lock lock;
    hk_cond wake, done;
    void (*fn)(void *);
    void *arg;
    u32 posted, finished;
    hk_thread *thread;
} G_SERVICE = { HK_LOCK_INIT, HK_COND_INIT, HK_COND_INIT, 0, 0, 0, 0, 0 };

static void service_main(void *unused)
{
    (void)unused;
    hk_lock_acquire(&G_SERVICE.lock);
    for (;;) {
        void (*fn)(void *);
        void *arg;
        while (!G_SERVICE.fn) hk_cond_wait(&G_SERVICE.wake, &G_SERVICE.lock);
        fn = G_SERVICE.fn;
        arg = G_SERVICE.arg;
        hk_lock_release(&G_SERVICE.lock);
        fn(arg);
        hk_lock_acquire(&G_SERVICE.lock);
        G_SERVICE.fn = NULL;
        G_SERVICE.finished++;
        hk_cond_broadcast(&G_SERVICE.done);
    }
}

static void on_service_thread(void (*fn)(void *), void *arg)
{
    u32 mine;
    hk_lock_acquire(&G_SERVICE.lock);
    if (!G_SERVICE.thread) G_SERVICE.thread = hk_thread_start(service_main, NULL, 512 * 1024, "graphics-service");
    if (!G_SERVICE.thread) {
        hk_lock_release(&G_SERVICE.lock);
        fn(arg);
        return;
    }
    /* One job at a time, in the order asked. */
    while (G_SERVICE.fn) hk_cond_wait(&G_SERVICE.done, &G_SERVICE.lock);
    G_SERVICE.fn = fn;
    G_SERVICE.arg = arg;
    mine = ++G_SERVICE.posted;
    hk_cond_broadcast(&G_SERVICE.wake);
    while ((i32)(G_SERVICE.finished - mine) < 0) hk_cond_wait(&G_SERVICE.done, &G_SERVICE.lock);
    hk_lock_release(&G_SERVICE.lock);
}

/* What the shell is asked for, as jobs for that thread. */
typedef struct shell_job {
    int what;                          /* 0 attach_device, 1 acquire_target, 2 release_target */
    void *device, *texture;
    int width, height;
    bool ok;
} shell_job;

static void shell_job_run(void *p)
{
    shell_job *j = (shell_job *)p;
    const megler_run_config *run = hk_run_config();
    if (j->what == 0) j->ok = run->window.attach_device(run->window.user, j->device);
    else if (j->what == 1) j->texture = run->window.acquire_target(run->window.user, j->width, j->height);
    else run->window.release_target(run->window.user);
}

static bool shell_attach_device(void *device)
{
    shell_job j;
    memset(&j, 0, sizeof(j));
    j.what = 0;
    j.device = device;
    on_service_thread(shell_job_run, &j);
    return j.ok;
}

static void *shell_acquire_target(int width, int height)
{
    shell_job j;
    memset(&j, 0, sizeof(j));
    j.what = 1;
    j.width = width;
    j.height = height;
    on_service_thread(shell_job_run, &j);
    return j.texture;
}

static void shell_release_target(void)
{
    shell_job j;
    memset(&j, 0, sizeof(j));
    j.what = 2;
    on_service_thread(shell_job_run, &j);
}

static void make_display_job(void *p) { *(EGLDisplay *)p = make_display(); }

/* The display, initialised; NULL when ANGLE cannot make one. */
static EGLDisplay display(void)
{
    if (g_initialized) return g_display;
    hk_lock_acquire(&g_egl_lock);
    if (!g_initialized) {
        /* The flag "no-egl": there is no display, as on a device ANGLE cannot run on. */
        if (!hk_flag("no-egl")) on_service_thread(make_display_job, &g_display);
        if (g_display) {
            fn_query_display_attrib qd = A(fn_query_display_attrib, "eglQueryDisplayAttribEXT");
            fn_query_device_attrib qv = A(fn_query_device_attrib, "eglQueryDeviceAttribEXT");
            intptr_t device = 0, d3d = 0;
            if (qd && qv && qd(g_display, EGL_DEVICE_EXT, &device) && device && qv((void *)device, EGL_D3D11_DEVICE_ANGLE, &d3d)) {
                g_d3d_device = (void *)d3d;
                g_d3d_level = hk_host_d3d11_feature_level(g_d3d_device);
            }
            hk_info("egl: ANGLE display as %s, EGL %d.%d, Direct3D feature level %d_%d", g_how, g_major, g_minor, g_d3d_level >> 12, (g_d3d_level >> 8) & 15);
            {
                fn_query_string qs = A(fn_query_string, "eglQueryString");
                const char *ext = qs ? qs(g_display, EGL_EXTENSIONS) : NULL;
                size_t n = ext ? strlen(ext) : 0, at = 0;
                while (at < n) {
                    size_t take = n - at > 700 ? 700 : n - at;
                    hk_info("egl extensions: %.*s", (int)take, ext + at);
                    at += take;
                }
            }
        } else {
            hk_err("egl: ANGLE could not make a display");
        }
        g_initialized = true;
    }
    hk_lock_release(&g_egl_lock);
    return g_display;
}

/* --------------------------------------------------------------- surfaces */

typedef struct surface {
    EGLSurface handle;
    hk_nwindow *window;
    int width, height;
    bool from_host;                    /* its texture is the shell's */
} surface;
static surface g_surfaces[16];

static surface *surface_of(EGLSurface s)
{
    size_t i;
    for (i = 0; i < HK_COUNT(g_surfaces); i++)
        if (g_surfaces[i].handle == s && s) return &g_surfaces[i];
    return NULL;
}

/* ------------------------------------------------------- what guests call */

static EGLint g_eglGetError(void)
{
    EGLint e = g_error;
    fn_get_error real = A(fn_get_error, "eglGetError");
    g_error = EGL_SUCCESS;
    if (e != EGL_SUCCESS) return e;
    return real ? real() : EGL_NOT_INITIALIZED;
}

static EGLDisplay g_eglGetDisplay(void *native)
{
    (void)native;
    return display();
}

static EGLBoolean g_eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    if (!d || d != display()) {
        g_error = EGL_BAD_DISPLAY;
        return 0;
    }
    if (major) *major = g_major;
    if (minor) *minor = g_minor;
    return 1;
}

/* The display outlives any one user of it: the app's terminate is its own business. */
static EGLBoolean g_eglTerminate(EGLDisplay d) { (void)d; return 1; }

static EGLBoolean g_eglChooseConfig(EGLDisplay d, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *num)
{
    fn_choose_config real = A(fn_choose_config, "eglChooseConfig");
    fn_get_config_attrib get = A(fn_get_config_attrib, "eglGetConfigAttrib");
    EGLint copy[96], found = 0, kept = 0, i;
    EGLConfig all[256];
    int n = 0;
    if (!real) return 0;
    /* A window surface is a pbuffer here, so that is what the configuration has to support. */
    for (; attribs && attribs[n] != EGL_NONE && n < 92; n += 2) {
        copy[n] = attribs[n];
        copy[n + 1] = attribs[n + 1];
        if (copy[n] == EGL_SURFACE_TYPE && (copy[n + 1] & EGL_WINDOW_BIT)) copy[n + 1] = (copy[n + 1] & ~EGL_WINDOW_BIT) | EGL_PBUFFER_BIT;
    }
    copy[n] = EGL_NONE;
    if (!real(d, copy, all, (EGLint)HK_COUNT(all), &found)) return 0;
    /*
     * An Android device offers 8888, 888 and 565. ANGLE has 10-10-10-2 and
     * half-float configurations as well, and EGL sorts the deepest first: an
     * app that asks for 8 bits and takes the first answer (most do) would get
     * one of those. They are left out.
     */
    for (i = 0; i < found; i++) {
        EGLint red = 8;
        if (get) get(d, all[i], EGL_RED_SIZE, &red);
        if (red > 8) continue;
        if (configs && kept < size) configs[kept] = all[i];
        kept++;
    }
    if (num) *num = configs && kept > size ? size : kept;
    return 1;
}

static EGLBoolean g_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint attrib, EGLint *value)
{
    fn_get_config_attrib real = A(fn_get_config_attrib, "eglGetConfigAttrib");
    EGLBoolean ok;
    if (!real) return 0;
    if (attrib == EGL_NATIVE_VISUAL_ID) {
        /* An ANativeWindow format: RGBA_8888 (1), or RGB_565 (4) for a 5-bit red channel. */
        EGLint red = 8;
        real(d, c, EGL_RED_SIZE, &red);
        *value = red == 5 ? 4 : 1;
        return 1;
    }
    ok = real(d, c, attrib, value);
    if (ok && attrib == EGL_SURFACE_TYPE && (*value & EGL_PBUFFER_BIT)) *value |= EGL_WINDOW_BIT;
    return ok;
}

/*
 * The configuration for a surface over the shell's texture. That texture is
 * 8 bits a channel with alpha, and ANGLE takes a client texture only for a
 * configuration whose channel sizes match it and that is not multisampled. An
 * app that asked for RGB565 or for multisampling gets the nearest
 * configuration that fits -- same depth, stencil and OpenGL ES version -- and
 * its context, made without a configuration (see g_eglCreateContext), goes
 * with either.
 *
 * Two configurations can be alike in every attribute and differ in the order
 * of their channels (RGBA and BGRA), which nothing in EGL tells apart and
 * which decides whether the texture is taken. So this gives every candidate,
 * the app's own first, and the surface is made with the first that works.
 */
static int surface_configs(EGLDisplay d, EGLConfig wanted, EGLConfig *out, int max)
{
    fn_get_config_attrib get = A(fn_get_config_attrib, "eglGetConfigAttrib");
    fn_choose_config choose = A(fn_choose_config, "eglChooseConfig");
    EGLint r = 0, g = 0, b = 0, a = 0, samples = 0, depth = 0, stencil = 0, renderable = EGL_OPENGL_ES2_BIT, n = 0, i;
    EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 0, 0x3026, 0, EGL_RENDERABLE_TYPE, 0, EGL_NONE };
    EGLConfig found[64];
    int count = 0;
    if (!get || !choose) {
        out[0] = wanted;
        return 1;
    }
    get(d, wanted, EGL_RED_SIZE, &r);
    get(d, wanted, 0x3023, &g);
    get(d, wanted, 0x3022, &b);
    get(d, wanted, 0x3021, &a);
    get(d, wanted, 0x3031, &samples);
    if (r == 8 && g == 8 && b == 8 && a == 8 && samples <= 1) out[count++] = wanted;
    else hk_info("egl: the app's configuration is %d%d%d%d%s; its window uses an 8888 one", r, g, b, a, samples > 1 ? " multisampled" : "");
    get(d, wanted, 0x3025, &depth);
    get(d, wanted, 0x3026, &stencil);
    get(d, wanted, EGL_RENDERABLE_TYPE, &renderable);
    attribs[11] = depth;
    attribs[13] = stencil;
    attribs[15] = renderable;
    if (choose(d, attribs, found, 64, &n)) {
        for (i = 0; i < n && count < max; i++) {
            EGLint fr = 0, fa = 0, fs = 0;
            get(d, found[i], EGL_RED_SIZE, &fr);
            get(d, found[i], 0x3021, &fa);
            get(d, found[i], 0x3031, &fs);
            if (fr == 8 && fa == 8 && fs <= 1 && found[i] != wanted) out[count++] = found[i];
        }
    }
    /* Nothing fits: the app's own, which ANGLE will say no to in its own words. */
    if (!count) out[count++] = wanted;
    return count;
}

/*
 * Contexts are made without a configuration where ANGLE allows it, so a
 * context goes with whatever configuration its surface ended up with.
 */
static EGLContext g_eglCreateContext(EGLDisplay d, EGLConfig config, EGLContext share, const EGLint *attribs)
{
    static int no_config = -1;
    fn_create_context real = A(fn_create_context, "eglCreateContext");
    hk_trace("eglCreateContext(share %p)", (void *)share);
    if (!real) return NULL;
    if (no_config < 0) {
        fn_query_string qs = A(fn_query_string, "eglQueryString");
        const char *ext = qs ? qs(d, EGL_EXTENSIONS) : NULL;
        no_config = ext && strstr(ext, "EGL_KHR_no_config_context") ? 1 : 0;
    }
    return real(d, no_config ? NULL : config, share, attribs);
}

static EGLSurface g_eglCreateWindowSurface(EGLDisplay d, EGLConfig config, hk_nwindow *window, const EGLint *attribs)
{
    const megler_run_config *run = hk_run_config();
    surface *s = NULL;
    size_t i;
    EGLint none[] = { EGL_NONE };
    (void)attribs;
    if (!window) {
        g_error = EGL_BAD_NATIVE_WINDOW;
        return NULL;
    }
    for (i = 0; i < HK_COUNT(g_surfaces) && !s; i++)
        if (!g_surfaces[i].handle) s = &g_surfaces[i];
    if (!s) {
        g_error = EGL_BAD_ALLOC;
        return NULL;
    }
    hk_nwindow_buffer_size(window, &s->width, &s->height);
    s->window = window;
    s->from_host = false;
    if (run->window.acquire_target && g_d3d_device) {
        fn_pbuffer_from from = A(fn_pbuffer_from, "eglCreatePbufferFromClientBuffer");
        void *texture;
        if (!g_device_attached) {
            g_device_attached = true;
            if (run->window.attach_device && !shell_attach_device(g_d3d_device)) hk_err("egl: the shell could not make a swap chain on ANGLE's device");
        }
        fn_get_error err = A(fn_get_error, "eglGetError");
        EGLConfig candidates[32];
        int count = surface_configs(d, config, candidates, (int)HK_COUNT(candidates)), k, first_error = 0;
        texture = shell_acquire_target(s->width, s->height);
        if (!from) hk_err("egl: this ANGLE has no eglCreatePbufferFromClientBuffer");
        for (k = 0; texture && from && k < count && !s->handle; k++) {
            s->handle = from(d, EGL_D3D_TEXTURE_ANGLE, texture, candidates[k], none);
            if (!s->handle && !first_error && err) first_error = err();
        }
        if (s->handle) {
            s->from_host = true;
            if (k > 1) hk_info("egl: the shell's texture was taken with the %s of %d configurations tried", k == 2 ? "second" : "last", k);
        } else {
            hk_err("egl: no surface over the shell's %dx%d texture (%d configurations tried, EGL error 0x%x); frames will not be shown", s->width, s->height,
                   texture && from ? count : 0, first_error);
            if (texture && run->window.release_target) shell_release_target();
        }
    }
    if (!s->handle) {
        /* No shell behind us (the tests), or its texture was refused: an off-screen buffer keeps the app running. */
        fn_pbuffer pb = A(fn_pbuffer, "eglCreatePbufferSurface");
        EGLint size[] = { EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE };
        size[1] = s->width;
        size[3] = s->height;
        s->handle = pb ? pb(d, config, size) : NULL;
    }
    if (!s->handle) {
        s->window = NULL;
        return NULL;
    }
    hk_nwindow_acquire(window);
    hk_info("egl: window surface %dx%d%s", s->width, s->height, s->from_host ? "" : " (off-screen)");
    return s->handle;
}

static EGLBoolean g_eglDestroySurface(EGLDisplay d, EGLSurface handle)
{
    fn_destroy_surface real = A(fn_destroy_surface, "eglDestroySurface");
    surface *s = surface_of(handle);
    EGLBoolean ok = real ? real(d, handle) : 0;
    if (s) {
        const megler_run_config *run = hk_run_config();
        if (s->from_host && run->window.release_target) shell_release_target();
        if (s->window) hk_nwindow_release(s->window);
        memset(s, 0, sizeof(*s));
    }
    return ok;
}

static EGLBoolean g_eglQuerySurface(EGLDisplay d, EGLSurface handle, EGLint attrib, EGLint *value)
{
    fn_query_surface real = A(fn_query_surface, "eglQuerySurface");
    surface *s = surface_of(handle);
    if (s && attrib == EGL_WIDTH) {
        *value = s->width;
        return 1;
    }
    if (s && attrib == EGL_HEIGHT) {
        *value = s->height;
        return 1;
    }
    return real ? real(d, handle, attrib, value) : 0;
}

static EGLBoolean g_eglSwapBuffers(EGLDisplay d, EGLSurface handle)
{
    surface *s = surface_of(handle);
    hk_trace("eglSwapBuffers(%p)", (void *)handle);
    if (!s) {
        fn_swap real = A(fn_swap, "eglSwapBuffers");
        return real ? real(d, handle) : 0;
    }
    if (s->from_host) {
        const megler_run_config *run = hk_run_config();
        fn_gl_void flush = A(fn_gl_void, "glFlush");
        if (flush) flush();
        if (run->window.present) run->window.present(run->window.user);
    }
    hk_run_count_frame();
    return 1;
}

static EGLBoolean g_eglSwapInterval(EGLDisplay d, EGLint interval) { (void)d; (void)interval; return 1; }

static void *g_eglGetProcAddress(const char *name) { return hk_egl_resolve(name); }

/* ----------------------------------------------- GLES functions with floats
 *
 * One adapter per function: it receives each float as the 32 bits the guest
 * put in an integer register and calls ANGLE with real floats. The letters
 * name the parameters in order -- i for anything integer-sized, f for a float.
 */
#define AD_f(n) static void (*h_##n)(float); static void w_##n(u32 a) { h_##n(hk_f(a)); }
#define AD_ff(n) static void (*h_##n)(float, float); static void w_##n(u32 a, u32 b) { h_##n(hk_f(a), hk_f(b)); }
#define AD_fff(n) static void (*h_##n)(float, float, float); static void w_##n(u32 a, u32 b, u32 c) { h_##n(hk_f(a), hk_f(b), hk_f(c)); }
#define AD_ffff(n) static void (*h_##n)(float, float, float, float); static void w_##n(u32 a, u32 b, u32 c, u32 d) { h_##n(hk_f(a), hk_f(b), hk_f(c), hk_f(d)); }
#define AD_fffff(n) static void (*h_##n)(float, float, float, float, float); static void w_##n(u32 a, u32 b, u32 c, u32 d, u32 e) { h_##n(hk_f(a), hk_f(b), hk_f(c), hk_f(d), hk_f(e)); }
#define AD_ffffff(n) static void (*h_##n)(float, float, float, float, float, float); static void w_##n(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) { h_##n(hk_f(a), hk_f(b), hk_f(c), hk_f(d), hk_f(e), hk_f(f)); }
#define AD_fi(n) static void (*h_##n)(float, int); static void w_##n(u32 a, int b) { h_##n(hk_f(a), b); }
#define AD_if(n) static void (*h_##n)(int, float); static void w_##n(int a, u32 b) { h_##n(a, hk_f(b)); }
#define AD_iff(n) static void (*h_##n)(int, float, float); static void w_##n(int a, u32 b, u32 c) { h_##n(a, hk_f(b), hk_f(c)); }
#define AD_ifff(n) static void (*h_##n)(int, float, float, float); static void w_##n(int a, u32 b, u32 c, u32 d) { h_##n(a, hk_f(b), hk_f(c), hk_f(d)); }
#define AD_iffff(n) static void (*h_##n)(int, float, float, float, float); static void w_##n(int a, u32 b, u32 c, u32 d, u32 e) { h_##n(a, hk_f(b), hk_f(c), hk_f(d), hk_f(e)); }
#define AD_iif(n) static void (*h_##n)(int, int, float); static void w_##n(int a, int b, u32 c) { h_##n(a, b, hk_f(c)); }
#define AD_iiff(n) static void (*h_##n)(int, int, float, float); static void w_##n(int a, int b, u32 c, u32 d) { h_##n(a, b, hk_f(c), hk_f(d)); }
#define AD_iifff(n) static void (*h_##n)(int, int, float, float, float); static void w_##n(int a, int b, u32 c, u32 d, u32 e) { h_##n(a, b, hk_f(c), hk_f(d), hk_f(e)); }
#define AD_iiffff(n) static void (*h_##n)(int, int, float, float, float, float); static void w_##n(int a, int b, u32 c, u32 d, u32 e, u32 f) { h_##n(a, b, hk_f(c), hk_f(d), hk_f(e), hk_f(f)); }
#define AD_iifi(n) static void (*h_##n)(int, int, float, int); static void w_##n(int a, int b, u32 c, int d) { h_##n(a, b, hk_f(c), d); }

#define GL_FLOAT_FUNCTIONS(X)                                                                                                    \
    X(ffff, glBlendColor) X(ffff, glClearColor) X(f, glClearDepthf) X(ff, glDepthRangef) X(f, glLineWidth) X(ff, glPolygonOffset) \
    X(fi, glSampleCoverage) X(iif, glTexParameterf) X(if, glUniform1f) X(iff, glUniform2f) X(ifff, glUniform3f)                    \
    X(iffff, glUniform4f) X(if, glVertexAttrib1f) X(iff, glVertexAttrib2f) X(ifff, glVertexAttrib3f) X(iffff, glVertexAttrib4f)    \
    X(iifi, glClearBufferfi) X(iif, glSamplerParameterf) X(iif, glProgramUniform1f) X(iiff, glProgramUniform2f)                    \
    X(iifff, glProgramUniform3f) X(iiffff, glProgramUniform4f) X(f, glMinSampleShading)                                            \
    X(if, glAlphaFunc) X(ffff, glColor4f) X(if, glFogf) X(ffffff, glFrustumf) X(if, glLightModelf) X(iif, glLightf)                 \
    X(iif, glMaterialf) X(iffff, glMultiTexCoord4f) X(fff, glNormal3f) X(ffffff, glOrthof) X(if, glPointParameterf)                \
    X(f, glPointSize) X(ffff, glRotatef) X(fff, glScalef) X(iif, glTexEnvf) X(fff, glTranslatef) X(fffff, glDrawTexfOES)            \
    X(iif, glTexGenfOES) X(f, glClearDepthfOES) X(ff, glDepthRangefOES) X(ffffff, glFrustumfOES) X(ffffff, glOrthofOES)

#define X(kind, name) AD_##kind(name)
GL_FLOAT_FUNCTIONS(X)
#undef X

typedef struct adapter {
    const char *name;
    void *wrapper;
    void **host;
} adapter;

#define X(kind, name) { #name, (void *)w_##name, (void **)&h_##name },
static const adapter g_adapters[] = { GL_FLOAT_FUNCTIONS(X) };
#undef X

static const hk_sym g_egl_overrides[] = {
    HK_WRAP("eglGetError", g_eglGetError),
    HK_WRAP("eglGetDisplay", g_eglGetDisplay),
    HK_WRAP("eglInitialize", g_eglInitialize),
    HK_WRAP("eglTerminate", g_eglTerminate),
    HK_WRAP("eglChooseConfig", g_eglChooseConfig),
    HK_WRAP("eglGetConfigAttrib", g_eglGetConfigAttrib),
    HK_WRAP("eglCreateWindowSurface", g_eglCreateWindowSurface),
    HK_WRAP("eglCreateContext", g_eglCreateContext),
    HK_WRAP("eglDestroySurface", g_eglDestroySurface),
    HK_WRAP("eglQuerySurface", g_eglQuerySurface),
    HK_WRAP("eglSwapBuffers", g_eglSwapBuffers),
    HK_WRAP("eglSwapInterval", g_eglSwapInterval),
    HK_WRAP("eglGetProcAddress", g_eglGetProcAddress),
    HK_END
};

bool hk_egl_is_gl_name(const char *name)
{
    return (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') || (name[0] == 'e' && name[1] == 'g' && name[2] == 'l' && name[3] >= 'A' && name[3] <= 'Z');
}

static void *resolve(const char *name)
{
    static bool told;
    size_t i;
    if (!hk_egl_is_gl_name(name)) return NULL;
    if (!told) {
        told = true;
        hk_gltex_set_host(angle);
    }
    if (name[0] == 'e') {
        for (i = 0; g_egl_overrides[i].name; i++)
            if (!strcmp(g_egl_overrides[i].name, name)) return g_egl_overrides[i].addr;
        return angle(name);
    }
    for (i = 0; i < HK_COUNT(g_adapters); i++) {
        if (strcmp(g_adapters[i].name, name)) continue;
        if (!*g_adapters[i].host) *g_adapters[i].host = angle(name);
        /* ANGLE may not have it (an OpenGL ES 1 function): then the guest gets "no such function", not an adapter to nothing. */
        return *g_adapters[i].host ? g_adapters[i].wrapper : NULL;
    }
    return hk_gltex_wrap(name, angle(name));
}

/* ------------------------------------------------- tracing the app's GL calls
 *
 * The flag "trace-gl": every EGL and GLES function the app is given goes
 * through a few generated instructions that put its name in the trace
 * (hk_base.c) before going on to the function. Which call came last before
 * something went wrong is then in the crash report. Each of these thunks has
 * a page of its own: a page is written and then made executable for good, and
 * one that other threads are already running through is never touched again.
 */
#if HK_HOST_ARM32
static struct {
    const char *name;
    void *thunk;
    u8 kind;                           /* which arguments are worth writing down: see gl_called */
} g_traced[1024];
static int g_traced_count;
static u8 *g_thunk_block;
static u32 g_thunk_block_used;
static hk_lock g_thunk_lock = HK_LOCK_INIT;

/*
 * `saved` is what the thunk pushed: r0-r3, r4 and lr, and above them the
 * caller's stack, where the fifth argument onwards is. The calls that take
 * memory say how much, and what the adapter's own count stands at.
 */
static void gl_called(u32 id, const u32 *saved)
{
    const char *name = g_traced[id].name;
    u32 budget = 0, used = 0;
    static volatile i32 removed_said;
    /* The device going away is found out at the next call: the one before this did it, or something outside the app. */
    if (!removed_said && hk_host_d3d11_removed_reason(g_d3d_device) && !hk_atomic_xchg(&removed_said, 1)) {
        hk_trace("the Direct3D device is gone (0x%x), found before gl %s", (unsigned)hk_host_d3d11_removed_reason(g_d3d_device), name);
        hk_err("egl: the Direct3D device has been removed (0x%x); it was found gone before %s", (unsigned)hk_host_d3d11_removed_reason(g_d3d_device), name);
    }
    switch (g_traced[id].kind) {
    case 1:         /* glTexStorage2D(target, levels, format, width, height) */
        hk_host_gpu_memory(g_d3d_device, &budget, &used);
        hk_trace("gl %s levels %u format 0x%x %ux%u; graphics memory %u of %u MB", name, saved[1], saved[2], saved[3], saved[6], used, budget);
        break;
    case 2:         /* glTexImage2D / glCompressedTexImage2D(target, level, format, width, height, ...) */
        hk_host_gpu_memory(g_d3d_device, &budget, &used);
        hk_trace("gl %s level %u format 0x%x %ux%u; graphics memory %u of %u MB", name, saved[1], saved[2], saved[3], saved[6], used, budget);
        break;
    case 3:         /* glCompressedTexSubImage2D(target, level, x, y, width, height, format, size, data) */
        hk_trace("gl %s level %u %ux%u format 0x%x, %u bytes", name, saved[1], saved[6], saved[7], saved[8], saved[9]);
        break;
    case 4:         /* glRenderbufferStorage(target, format, width, height) */
        hk_trace("gl %s format 0x%x %ux%u", name, saved[1], saved[2], saved[3]);
        break;
    case 5:         /* glRenderbufferStorageMultisample(target, samples, format, width, height) */
        hk_trace("gl %s x%u format 0x%x %ux%u", name, saved[1], saved[2], saved[3], saved[6]);
        break;
    case 6:         /* glBufferData(target, size, data, usage) */
        hk_trace("gl %s %u bytes", name, saved[1]);
        break;
    default:
        hk_trace("gl %s", name);
        break;
    }
}

static u8 gl_kind(const char *name)
{
    if (!strcmp(name, "glTexStorage2D")) return 1;
    if (!strcmp(name, "glTexImage2D") || !strcmp(name, "glCompressedTexImage2D")) return 2;
    if (!strcmp(name, "glCompressedTexSubImage2D")) return 3;
    if (!strcmp(name, "glRenderbufferStorage")) return 4;
    if (!strcmp(name, "glRenderbufferStorageMultisample")) return 5;
    if (!strcmp(name, "glBufferData")) return 6;
    return 0;
}

static void *traced(const char *name, void *target)
{
    static const u16 code[] = {
        0xB51F,             /* push {r0-r4, lr}: six words, the stack stays a multiple of eight */
        0x4669,             /* mov r1, sp: what was pushed, and the caller's stack above it */
        0x4803,             /* ldr r0, [pc, #12]: which function */
        0x4C04,             /* ldr r4, [pc, #16]: gl_called */
        0x47A0,             /* blx r4 */
        0xE8BD, 0x401F,     /* pop {r0-r4, lr} */
        0xBF00,             /* nop: the next instruction and the words after it on a multiple of four */
        0xF8DF, 0xF008,     /* ldr pc, [pc, #8]: the function itself, with the arguments as they came */
    };
    u8 *p;
    u32 words[3];
    int i;
    if (!target) return NULL;
    hk_lock_acquire(&g_thunk_lock);
    for (i = 0; i < g_traced_count; i++) {
        if (!strcmp(g_traced[i].name, name)) {
            hk_lock_release(&g_thunk_lock);
            return g_traced[i].thunk;
        }
    }
    if (g_traced_count == (int)HK_COUNT(g_traced)) {
        hk_lock_release(&g_thunk_lock);
        return target;
    }
    if (!g_thunk_block || g_thunk_block_used + HK_PAGE > 65536) {
        g_thunk_block = (u8 *)hk_mem_alloc(65536);
        g_thunk_block_used = 0;
    }
    if (!g_thunk_block) {
        hk_lock_release(&g_thunk_lock);
        return target;
    }
    p = g_thunk_block + g_thunk_block_used;
    g_thunk_block_used += HK_PAGE;
    words[0] = (u32)g_traced_count;
    words[1] = (u32)(hk_addr)gl_called;
    words[2] = (u32)(hk_addr)target;
    memcpy(p, code, sizeof(code));
    memcpy(p + sizeof(code), words, sizeof(words));
    if (!hk_mem_protect(p, HK_PAGE, HK_PROT_RX)) {
        hk_lock_release(&g_thunk_lock);
        return target;
    }
    hk_icache_flush(p, HK_PAGE);
    g_traced[g_traced_count].name = hk_strdup(name);
    g_traced[g_traced_count].kind = gl_kind(name);
    g_traced[g_traced_count].thunk = (void *)((hk_addr)p | 1);
    g_traced_count++;
    hk_lock_release(&g_thunk_lock);
    return (void *)((hk_addr)p | 1);
}
#else
static void *traced(const char *name, void *target)
{
    (void)name;
    return target;
}
#endif

static void *resolve(const char *name);

void *hk_egl_resolve(const char *name)
{
    static int tracing = -1;
    void *fn = resolve(name);
    if (tracing < 0) tracing = hk_flag("trace-gl") ? 1 : 0;
    return tracing && fn ? traced(name, fn) : fn;
}

void *hk_egl_host(const char *name)
{
    static bool told;
    if (!hk_egl_is_gl_name(name)) return NULL;
    /* The EGL functions above take no floats: the same ones guests get. */
    if (name[0] == 'e') return resolve(name);
    if (!told) {
        told = true;
        hk_gltex_set_host(angle);
    }
    return hk_gltex_wrap(name, angle(name));
}

u32 hk_egl_device_removed(void) { return hk_host_d3d11_removed_reason(g_d3d_device); }

/* ------------------------------------------------------ external textures */

bool hk_egl_external_upload(u32 external, u32 *helper, void **image, int *helper_w, int *helper_h, const u8 *rgba, int w, int h)
{
    enum {
        TEXTURE_2D = 0x0DE1, TEXTURE_EXTERNAL = 0x8D65, BINDING_2D = 0x8069, BINDING_EXTERNAL = 0x8D67, UNPACK_BUFFER = 0x88EC,
        UNPACK_BUFFER_BINDING = 0x88EF, UNPACK_ROW_LENGTH = 0x0CF2, UNPACK_SKIP_ROWS = 0x0CF3, UNPACK_SKIP_PIXELS = 0x0CF4, UNPACK_ALIGNMENT = 0x0CF5,
        RGBA = 0x1908, UNSIGNED_BYTE = 0x1401, LINEAR = 0x2601, CLAMP_TO_EDGE = 0x812F, MIN_FILTER = 0x2801, MAG_FILTER = 0x2800, WRAP_S = 0x2802,
        WRAP_T = 0x2803, GL_TEXTURE_2D_KHR = 0x30B1, NONE = 0x3038
    };
    typedef void (*fn_get_integerv)(u32, i32 *);
    typedef void (*fn_gen)(i32, u32 *);
    typedef void (*fn_bind)(u32, u32);
    typedef void (*fn_param)(u32, u32, i32);
    typedef void (*fn_storei)(u32, i32);
    typedef void (*fn_image)(u32, i32, i32, i32, i32, i32, u32, u32, const void *);
    typedef void (*fn_sub_image)(u32, i32, i32, i32, i32, i32, u32, u32, const void *);
    typedef void *(*fn_create_image)(EGLDisplay, EGLContext, u32, void *, const EGLint *);
    typedef EGLBoolean (*fn_destroy_image)(EGLDisplay, void *);
    typedef void (*fn_target)(u32, void *);
    typedef EGLContext (*fn_current_context)(void);
    typedef u32 (*fn_error)(void);
    fn_get_integerv get = A(fn_get_integerv, "glGetIntegerv");
    fn_gen gen = A(fn_gen, "glGenTextures"), del = A(fn_gen, "glDeleteTextures");
    fn_bind bind = A(fn_bind, "glBindTexture"), bind_buffer = A(fn_bind, "glBindBuffer");
    fn_param param = A(fn_param, "glTexParameteri");
    fn_storei storei = A(fn_storei, "glPixelStorei");
    fn_image tex_image = A(fn_image, "glTexImage2D");
    fn_sub_image sub_image = A(fn_sub_image, "glTexSubImage2D");
    fn_create_image create_image = A(fn_create_image, "eglCreateImageKHR");
    fn_destroy_image destroy_image = A(fn_destroy_image, "eglDestroyImageKHR");
    fn_target target = A(fn_target, "glEGLImageTargetTexture2DOES");
    fn_current_context current = A(fn_current_context, "eglGetCurrentContext");
    fn_error error = A(fn_error, "glGetError");
    i32 was_2d = 0, was_external = 0, was_buffer = 0, was_row = 0, was_skip_rows = 0, was_skip_pixels = 0, was_alignment = 4;
    bool ok = true;
    if (!get || !gen || !bind || !param || !storei || !tex_image || !sub_image || !create_image || !target || !current || !rgba || w <= 0 || h <= 0) return false;
    get(BINDING_2D, &was_2d);
    get(UNPACK_BUFFER_BINDING, &was_buffer);
    get(UNPACK_ROW_LENGTH, &was_row);
    get(UNPACK_SKIP_ROWS, &was_skip_rows);
    get(UNPACK_SKIP_PIXELS, &was_skip_pixels);
    get(UNPACK_ALIGNMENT, &was_alignment);
    if (was_buffer && bind_buffer) bind_buffer(UNPACK_BUFFER, 0);
    storei(UNPACK_ROW_LENGTH, 0);
    storei(UNPACK_SKIP_ROWS, 0);
    storei(UNPACK_SKIP_PIXELS, 0);
    storei(UNPACK_ALIGNMENT, 4);
    if (!*helper || *helper_w != w || *helper_h != h) {
        static const EGLint none[] = { NONE };
        if (*image && destroy_image) destroy_image(g_display, *image);
        if (*helper && del) del(1, helper);
        *image = NULL;
        *helper = 0;
        gen(1, helper);
        bind(TEXTURE_2D, *helper);
        param(TEXTURE_2D, MIN_FILTER, LINEAR);
        param(TEXTURE_2D, MAG_FILTER, LINEAR);
        param(TEXTURE_2D, WRAP_S, CLAMP_TO_EDGE);
        param(TEXTURE_2D, WRAP_T, CLAMP_TO_EDGE);
        tex_image(TEXTURE_2D, 0, RGBA, w, h, 0, RGBA, UNSIGNED_BYTE, rgba);
        *helper_w = w;
        *helper_h = h;
        *image = create_image(g_display, current(), GL_TEXTURE_2D_KHR, (void *)(hk_addr)*helper, none);
        if (*image) {
            get(BINDING_EXTERNAL, &was_external);
            bind(TEXTURE_EXTERNAL, external);
            target(TEXTURE_EXTERNAL, *image);
            bind(TEXTURE_EXTERNAL, (u32)was_external);
        } else {
            hk_warn("egl: no image of a %dx%d texture for a film (EGL error 0x%x); the film stays black", w, h,
                    (unsigned)(A(fn_get_error, "eglGetError") ? A(fn_get_error, "eglGetError")() : 0));
            ok = false;
        }
    } else {
        bind(TEXTURE_2D, *helper);
        sub_image(TEXTURE_2D, 0, 0, 0, w, h, RGBA, UNSIGNED_BYTE, rgba);
    }
    if (error) {
        u32 e = error();
        if (e) {
            hk_note_once("egl: putting a film's frame on a texture gave OpenGL error 0x%x", (unsigned)e);
            ok = false;
        }
    }
    bind(TEXTURE_2D, (u32)was_2d);
    storei(UNPACK_ROW_LENGTH, was_row);
    storei(UNPACK_SKIP_ROWS, was_skip_rows);
    storei(UNPACK_SKIP_PIXELS, was_skip_pixels);
    storei(UNPACK_ALIGNMENT, was_alignment);
    if (was_buffer && bind_buffer) bind_buffer(UNPACK_BUFFER, (u32)was_buffer);
    return ok;
}

/* ------------------------------------------------------------------ probe */

void hk_egl_probe(void (*line)(const char *fmt, ...))
{
    EGLDisplay d = (line("angle: trying (if this is the last line, ANGLE ended the process while making its display)"), display());
    fn_query_string qs = A(fn_query_string, "eglQueryString");
    fn_choose_config choose = A(fn_choose_config, "eglChooseConfig");
    fn_create_context create = A(fn_create_context, "eglCreateContext");
    fn_destroy_context destroy = A(fn_destroy_context, "eglDestroyContext");
    fn_pbuffer pbuffer = A(fn_pbuffer, "eglCreatePbufferSurface");
    fn_destroy_surface destroy_surface = A(fn_destroy_surface, "eglDestroySurface");
    fn_make_current current = A(fn_make_current, "eglMakeCurrent");
    fn_gl_get_string gs = A(fn_gl_get_string, "glGetString");
    fn_gl_get_integerv gi = A(fn_gl_get_integerv, "glGetIntegerv");
    int version;
    if (!d || !qs || !choose || !create || !pbuffer || !current || !gs) {
        line("angle: NOT AVAILABLE (%s)", g_lib_egl || g_provider ? "no display could be made" : "its DLLs did not load");
        return;
    }
    line("angle: %s, EGL %d.%d, vendor %s", g_how, g_major, g_minor, qs(d, EGL_VENDOR));
    if (g_d3d_level) line("d3d-feature-level: %d_%d", g_d3d_level >> 12, (g_d3d_level >> 8) & 15);
    else line("d3d-feature-level: unknown");
    {
        /* What the window surface rests on: a surface over a texture of the shell's, and a context that goes with any configuration. */
        const char *egl_ext = qs(d, EGL_EXTENSIONS);
        line("egl-texture-surface: %s", egl_ext && strstr(egl_ext, "EGL_ANGLE_d3d_texture_client_buffer") ? "yes" : "NO (EGL_ANGLE_d3d_texture_client_buffer is missing)");
        line("egl-no-config-context: %s", egl_ext && strstr(egl_ext, "EGL_KHR_no_config_context") ? "yes" : "no");
    }
    /* The newest OpenGL ES the device gives a context for. */
    for (version = 3; version >= 2; version--) {
        EGLint cfg_attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0, EGL_RED_SIZE, 8, EGL_NONE };
        EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 0, EGL_NONE };
        EGLint size[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        EGLConfig config = NULL;
        EGLint n = 0;
        EGLContext ctx;
        EGLSurface surf;
        cfg_attribs[3] = version == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT;
        ctx_attribs[1] = version;
        if (!choose(d, cfg_attribs, &config, 1, &n) || n < 1) continue;
        line("gles: trying an OpenGL ES %d context", version);
        ctx = create(d, config, NULL, ctx_attribs);
        if (!ctx) continue;
        surf = pbuffer(d, config, size);
        if (surf && current(d, surf, surf, ctx)) {
            const char *ext = (const char *)gs(GL_EXTENSIONS);
            int max_texture = 0;
            if (gi) gi(GL_MAX_TEXTURE_SIZE, &max_texture);
            line("gles: %s", (const char *)gs(GL_VERSION));
            line("gles-renderer: %s", (const char *)gs(GL_RENDERER));
            line("gles-max-texture: %d", max_texture);
            if (ext) {
                line("gles-etc1: %s", strstr(ext, "GL_OES_compressed_ETC1_RGB8_texture") ? "yes" : "no");
                line("gles-dxt: %s", strstr(ext, "GL_EXT_texture_compression_dxt1") ? "yes" : "no");
                line("gles-astc: %s", strstr(ext, "GL_KHR_texture_compression_astc_ldr") ? "yes" : "no");
                line("gles-depth-texture: %s", strstr(ext, "GL_OES_depth_texture") || version >= 3 ? "yes" : "no");
                line("gles-npot: %s", strstr(ext, "GL_OES_texture_npot") || version >= 3 ? "yes" : "no");
            }
            current(d, NULL, NULL, NULL);
            if (destroy_surface) destroy_surface(d, surf);
            if (destroy) destroy(d, ctx);
            return;
        }
        if (surf && destroy_surface) destroy_surface(d, surf);
        if (destroy) destroy(d, ctx);
    }
    line("gles: no OpenGL ES context could be made");
}
