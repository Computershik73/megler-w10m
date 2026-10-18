#include "hk_egl.h"

#include <stdio.h>

#include "../bionic/hk_bionic.h"
#include "../bionic/hk_fd.h"
#include "../hk_core.h"
#include "../host/hk_host.h"
#include "../run/hk_run.h"
#include "hk_ahb.h"
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
#define EGL_DRAW 0x3059
#define EGL_READ 0x305A
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
#define EGL_DXGI_KEYED_MUTEX_ANGLE 0x33A2

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
typedef EGLSurface (*fn_current_surface)(EGLint);
typedef EGLContext (*fn_current_ctx)(void);
typedef EGLBoolean (*fn_surface_attrib)(EGLDisplay, EGLSurface, EGLint, EGLint);
typedef EGLBoolean (*fn_query_display_attrib)(EGLDisplay, EGLint, intptr_t *);
typedef EGLBoolean (*fn_query_device_attrib)(void *, EGLint, intptr_t *);
typedef const u8 *(*fn_gl_get_string)(unsigned);
typedef void (*fn_gl_get_integerv)(unsigned, int *);
typedef void (*fn_gl_void)(void);

/* ------------------------------------------------------------ the display */

static EGLDisplay g_display;
static bool g_initialized, g_device_attached;
static EGLint g_major, g_minor;
static EGLint g_told_minor;            /* the version the guest is told is 1.<this>: see tell_version */
static char g_told_version[96];        /* and its EGL_VERSION string, where that is not ANGLE's own */
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
typedef struct service {
    hk_lock lock;
    hk_cond wake, done;
    void (*fn)(void *);
    void *arg;
    u32 posted, finished;
    hk_thread *thread;
    const char *name;
} service;

/*
 * Two such threads: this one, and one for hardware buffers' textures (below), which keeps a context of the layer's own
 * current, and whose jobs come from the app's graphics threads while this one may be waiting for the shell.
 */
static service G_SERVICE = { HK_LOCK_INIT, HK_COND_INIT, HK_COND_INIT, 0, 0, 0, 0, 0, "graphics-service" };
static service G_BUFFERS = { HK_LOCK_INIT, HK_COND_INIT, HK_COND_INIT, 0, 0, 0, 0, 0, "graphics-buffers" };

static void service_main(void *p)
{
    service *s = (service *)p;
    hk_lock_acquire(&s->lock);
    for (;;) {
        void (*fn)(void *);
        void *arg;
        while (!s->fn) hk_cond_wait(&s->wake, &s->lock);
        fn = s->fn;
        arg = s->arg;
        hk_lock_release(&s->lock);
        fn(arg);
        hk_lock_acquire(&s->lock);
        s->fn = NULL;
        s->finished++;
        hk_cond_broadcast(&s->done);
    }
}

static void run_on(service *s, void (*fn)(void *), void *arg)
{
    u32 mine;
    hk_lock_acquire(&s->lock);
    if (!s->thread) s->thread = hk_thread_start(service_main, s, 512 * 1024, s->name);
    if (!s->thread) {
        hk_lock_release(&s->lock);
        fn(arg);
        return;
    }
    /* One job at a time, in the order asked. */
    while (s->fn) hk_cond_wait(&s->done, &s->lock);
    s->fn = fn;
    s->arg = arg;
    mine = ++s->posted;
    hk_cond_broadcast(&s->wake);
    while ((i32)(s->finished - mine) < 0) hk_cond_wait(&s->done, &s->lock);
    hk_lock_release(&s->lock);
}

static void on_service_thread(void (*fn)(void *), void *arg) { run_on(&G_SERVICE, fn, arg); }

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

/*
 * The version the guest is told. EGL 1.5 makes fence syncs part of EGL itself, and what is built on EGL takes 1.5 to
 * mean them: the ANGLE inside Chromium's WebView does, and then calls eglCreateSyncKHR, which it has loaded only where
 * EGL_KHR_fence_sync is named -- a call through nothing. ANGLE over Direct3D 11 says 1.5 without having fence syncs:
 * the guest is told 1.4 then, as most Android devices say.
 */
static void tell_version(const char *extensions, const char *version)
{
    const char *rest = version ? strchr(version, ' ') : NULL;
    g_told_minor = g_minor;
    g_told_version[0] = 0;
    if (g_major == 1 && g_minor >= 5 && !(extensions && strstr(extensions, "EGL_KHR_fence_sync"))) {
        g_told_minor = 4;
        snprintf(g_told_version, sizeof(g_told_version), "1.4%s", rest ? rest : "");
        hk_info("egl: told as EGL 1.4: this EGL %d.%d has no fence syncs (EGL_KHR_fence_sync)", g_major, g_minor);
    }
}

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
                tell_version(ext, qs ? qs(g_display, EGL_VERSION) : NULL);
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

/*
 * The app's window surfaces. On the window a driver made for the app's whole surface, a surface is EGL's own, over the
 * shell's texture. On a window of someone else's -- a SurfaceView's, which native code had from ANativeWindow_fromSurface
 * (drivers/hk_java_ndk.c) -- it is the window's own (`native`, hk_nwindow_ops), and what the app is given as its EGLSurface is
 * this entry: the surface of EGL's that it stands for is another once the window has been resized (a swap after the size
 * changed makes a new one, as Android's next buffer has the window's new size), so every EGL call that takes one is given the
 * one it stands for now (real_of), and eglGetCurrentSurface gives the app's back.
 */
typedef struct surface {
    EGLSurface handle;                 /* what the app was given: EGL's own surface, or this entry for a window's own */
    hk_nwindow *window;
    int width, height;
    bool from_host;                    /* its texture is the shell's */
    void *native;                      /* the window's own surface (hk_nwindow_ops.egl_create); NULL for one of EGL's */
} surface;
static surface g_surfaces[16];
static hk_lock g_surfaces_lock = HK_LOCK_INIT;
/* The windows' own surfaces current on this thread, as the app named them (eglMakeCurrent's draw and read). */
static HK_THREAD_LOCAL surface *t_draw, *t_read;

static surface *surface_of(EGLSurface s)
{
    size_t i;
    surface *found = NULL;
    if (!s) return NULL;
    hk_lock_acquire(&g_surfaces_lock);
    for (i = 0; i < HK_COUNT(g_surfaces) && !found; i++)
        if (g_surfaces[i].handle == s) found = &g_surfaces[i];
    hk_lock_release(&g_surfaces_lock);
    return found;
}

/* The entry a handle is, when it is one of the windows' own surfaces (the app's handle is the entry's address). */
static surface *window_surface(EGLSurface h)
{
    hk_addr at = (hk_addr)h, base = (hk_addr)g_surfaces;
    surface *s;
    if (at < base || at >= base + sizeof(g_surfaces) || (at - base) % sizeof(surface)) return NULL;
    s = (surface *)h;
    return s->native && s->handle == h ? s : NULL;
}

/* The surface of EGL's that a window's own stands for now, and its size. */
static EGLSurface window_real(surface *s, int *width, int *height) { return (EGLSurface)s->window->ops->egl_handle(s->native, width, height); }

/* What EGL itself is to be given for a surface the app names. */
static EGLSurface real_of(EGLSurface h)
{
    surface *s = window_surface(h);
    return s ? window_real(s, NULL, NULL) : h;
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
    if (minor) *minor = g_told_minor;
    return 1;
}

static const char *layer_extensions(const char *angle);

static const char *g_eglQueryString(EGLDisplay d, EGLint name)
{
    fn_query_string real = A(fn_query_string, "eglQueryString");
    if (name == EGL_VERSION && d && d == display() && g_told_version[0]) return g_told_version;
    if (name == EGL_EXTENSIONS && d && d == display()) return layer_extensions(real ? real(d, name) : NULL);
    return real ? real(d, name) : NULL;
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

/* ------------------------------------------------------- pbuffers' keyed mutexes
 *
 * ANGLE, as gecko-w10m builds it (ANGLE_ENABLE_KEYEDMUTEX), gives every pbuffer it can share a texture made with a DXGI keyed
 * mutex (D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX), and never acquires it itself: that is the user's to do, as Firefox does
 * around its drawing (eglQuerySurfacePointerANGLE, EGL_DXGI_KEYED_MUTEX_ANGLE). Direct3D drops whatever a device does with such
 * a texture while it does not hold its mutex -- drawing, clearing, reading, copying: all of an app's frames were nothing, and
 * so was every copy the shell made of them (docs/ARCHITECTURE.md, "OpenGL ES from an app's Java"). An Android app's own
 * pbuffer is simply its own; so here ANGLE's device holds each pbuffer's mutex from the moment it is made, and lets go of it
 * only while the shell copies a frame out of it (drivers/hk_java_gl.c). With key 0 throughout: whoever has it next takes it.
 */
bool hk_egl_keyed_mutex(void *mutex, bool acquire)
{
    typedef unsigned (*fn_keyed)(void *, int);
    if (!mutex) return false;
    /* (The tests' stand-in for ANGLE has mutexes of its own: the seam is its "function" by this name.) */
    if (g_provider) {
        fn_keyed f = (fn_keyed)g_provider("meglerKeyedMutexANGLE");
        return f && f(mutex, acquire ? 1 : 0);
    }
    return hk_host_keyed_mutex(mutex, acquire, acquire ? 1000 : 0);
}

void *hk_egl_surface_mutex(void *d, void *surface)
{
    typedef EGLBoolean (*fn_pointer)(EGLDisplay, EGLSurface, EGLint, void **);
    fn_pointer query = A(fn_pointer, "eglQuerySurfacePointerANGLE");
    void *mutex = NULL;
    if (!query || !surface || !query(d, surface, EGL_DXGI_KEYED_MUTEX_ANGLE, &mutex)) return NULL;
    return mutex;
}

static EGLSurface g_eglCreatePbufferSurface(EGLDisplay d, EGLConfig config, const EGLint *attribs)
{
    static volatile i32 said, refused;
    fn_pbuffer real = A(fn_pbuffer, "eglCreatePbufferSurface");
    EGLSurface h = real ? real(d, config, attribs) : NULL;
    void *mutex = h ? hk_egl_surface_mutex(d, h) : NULL;
    if (!mutex) return h;
    if (!hk_atomic_xchg(&said, 1))
        hk_info("egl: ANGLE shares its pbuffers' textures with a keyed mutex: each is held by ANGLE's device from its making, else what is drawn in it is dropped");
    if (!hk_egl_keyed_mutex(mutex, true) && hk_atomic_add(&refused, 1) <= 10)
        hk_warn("egl: a new pbuffer's keyed mutex could not be acquired: what is drawn in it will be dropped");
    return h;
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

/* A free entry, taken (its handle set to something not null until the caller fills it); NULL when all are in use. */
static surface *take_entry(void)
{
    size_t i;
    surface *s = NULL;
    hk_lock_acquire(&g_surfaces_lock);
    for (i = 0; i < HK_COUNT(g_surfaces) && !s; i++)
        if (!g_surfaces[i].handle) s = &g_surfaces[i];
    if (s) {
        memset(s, 0, sizeof(*s));
        s->handle = (EGLSurface)s;
    }
    hk_lock_release(&g_surfaces_lock);
    return s;
}

static void free_entry(surface *s)
{
    hk_lock_acquire(&g_surfaces_lock);
    memset(s, 0, sizeof(*s));
    hk_lock_release(&g_surfaces_lock);
}

/*
 * A window surface on a window of someone else's (a SurfaceView's): the window's own, made with the configurations an 8888
 * surface can have that are nearest the app's (the window's frames go to the shell as 8888 textures), the app's first.
 */
static EGLSurface window_surface_create(EGLDisplay d, EGLConfig config, hk_nwindow *window)
{
    EGLConfig candidates[32];
    surface *s;
    void *native;
    int count;
    if (!hk_nwindow_valid(window)) {
        hk_info("egl: no window surface on a window whose surface has gone");
        g_error = EGL_BAD_NATIVE_WINDOW;
        return NULL;
    }
    s = take_entry();
    if (!s) {
        hk_warn("egl: no window surface: %d are in use", (int)HK_COUNT(g_surfaces));
        g_error = EGL_BAD_ALLOC;
        return NULL;
    }
    count = surface_configs(d, config, candidates, (int)HK_COUNT(candidates));
    native = window->ops->egl_create(window, candidates, count);
    if (!native) {
        free_entry(s);
        g_error = EGL_BAD_ALLOC;
        return NULL;
    }
    hk_nwindow_acquire(window);
    s->window = window;
    s->native = native;
    window_real(s, &s->width, &s->height);
    hk_info("egl: window surface %dx%d on a view's window: its frames are shown where the view is", s->width, s->height);
    return s->handle;
}

static EGLSurface g_eglCreateWindowSurface(EGLDisplay d, EGLConfig config, hk_nwindow *window, const EGLint *attribs)
{
    const megler_run_config *run = hk_run_config();
    surface *s = NULL;
    EGLint none[] = { EGL_NONE };
    (void)attribs;
    if (!window) {
        g_error = EGL_BAD_NATIVE_WINDOW;
        return NULL;
    }
    if (window->ops && window->ops->egl_create) return window_surface_create(d, config, window);
    s = take_entry();
    if (!s) {
        g_error = EGL_BAD_ALLOC;
        return NULL;
    }
    {
        /* (The entry stays taken, its handle its own address, until EGL's surface is in it.) */
        EGLSurface h = NULL;
        int width, height;
        bool from_host = false;
        hk_nwindow_buffer_size(window, &width, &height);
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
            texture = shell_acquire_target(width, height);
            if (!from) hk_err("egl: this ANGLE has no eglCreatePbufferFromClientBuffer");
            for (k = 0; texture && from && k < count && !h; k++) {
                h = from(d, EGL_D3D_TEXTURE_ANGLE, texture, candidates[k], none);
                if (!h && !first_error && err) first_error = err();
            }
            if (h) {
                from_host = true;
                if (k > 1) hk_info("egl: the shell's texture was taken with the %s of %d configurations tried", k == 2 ? "second" : "last", k);
            } else {
                hk_err("egl: no surface over the shell's %dx%d texture (%d configurations tried, EGL error 0x%x); frames will not be shown", width, height,
                       texture && from ? count : 0, first_error);
                if (texture && run->window.release_target) shell_release_target();
            }
        }
        if (!h) {
            /* No shell behind us (the tests), or its texture was refused: an off-screen buffer keeps the app running. */
            EGLint size[] = { EGL_WIDTH, 0, EGL_HEIGHT, 0, EGL_NONE };
            size[1] = width;
            size[3] = height;
            h = g_eglCreatePbufferSurface(d, config, size);
        }
        if (!h) {
            free_entry(s);
            return NULL;
        }
        hk_nwindow_acquire(window);
        hk_lock_acquire(&g_surfaces_lock);
        s->window = window;
        s->width = width;
        s->height = height;
        s->from_host = from_host;
        s->handle = h;
        hk_lock_release(&g_surfaces_lock);
        hk_info("egl: window surface %dx%d%s", width, height, from_host ? "" : " (off-screen)");
        return h;
    }
}

static EGLBoolean g_eglDestroySurface(EGLDisplay d, EGLSurface handle)
{
    fn_destroy_surface real = A(fn_destroy_surface, "eglDestroySurface");
    surface *w = window_surface(handle), *s;
    EGLBoolean ok;
    if (w) {
        /* A window's own: it ends with the window's part of it, and the entry is free again. */
        hk_nwindow *window = w->window;
        void *native = w->native;
        if (t_draw == w) t_draw = NULL;
        if (t_read == w) t_read = NULL;
        free_entry(w);
        window->ops->egl_destroy(native);
        hk_nwindow_release(window);
        return 1;
    }
    s = surface_of(handle);
    ok = real ? real(d, handle) : 0;
    if (s) {
        const megler_run_config *run = hk_run_config();
        if (s->from_host && run->window.release_target) shell_release_target();
        if (s->window) hk_nwindow_release(s->window);
        free_entry(s);
    }
    return ok;
}

static EGLBoolean g_eglQuerySurface(EGLDisplay d, EGLSurface handle, EGLint attrib, EGLint *value)
{
    fn_query_surface real = A(fn_query_surface, "eglQuerySurface");
    surface *w = window_surface(handle), *s;
    if (w) {
        int width = 0, height = 0;
        EGLSurface h = window_real(w, &width, &height);
        if (attrib == EGL_WIDTH || attrib == EGL_HEIGHT) {
            if (value) *value = attrib == EGL_WIDTH ? width : height;
            return 1;
        }
        return real ? real(d, h, attrib, value) : 0;
    }
    s = surface_of(handle);
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

/*
 * The surfaces the app makes current are what EGL is given for them (real_of); which of the windows' own are current on this
 * thread is kept, for eglGetCurrentSurface, a swap, and a surface replaced under the app's handle.
 */
static EGLBoolean g_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    fn_make_current real = A(fn_make_current, "eglMakeCurrent");
    surface *wd = window_surface(draw), *wr = window_surface(read);
    EGLBoolean ok;
    if (!real) return 0;
    ok = real(d, wd ? window_real(wd, NULL, NULL) : draw, wr ? window_real(wr, NULL, NULL) : read, ctx);
    if (ok) {
        t_draw = ctx ? wd : NULL;
        t_read = ctx ? wr : NULL;
    }
    return ok;
}

static EGLSurface g_eglGetCurrentSurface(EGLint which)
{
    fn_current_surface real = A(fn_current_surface, "eglGetCurrentSurface");
    EGLSurface h = real ? real(which) : NULL;
    surface *w = which == EGL_READ ? t_read : which == EGL_DRAW ? t_draw : NULL;
    if (h && w && window_surface(w->handle) == w && window_real(w, NULL, NULL) == h) return w->handle;
    return h;
}

static EGLBoolean g_eglSurfaceAttrib(EGLDisplay d, EGLSurface handle, EGLint attrib, EGLint value)
{
    fn_surface_attrib real = A(fn_surface_attrib, "eglSurfaceAttrib");
    return real ? real(d, real_of(handle), attrib, value) : 0;
}

/*
 * A swap of a window's own surface: the window shows the frame (false: its surface has gone -- EGL_BAD_NATIVE_WINDOW, what
 * Android's GLSurfaceView expects then). A swap that found the window resized leaves the app's handle standing for a new
 * surface of EGL's, which is made current here where the old one was.
 */
static EGLBoolean window_swap(EGLDisplay d, surface *w)
{
    EGLSurface before, after;
    bool shown;
    if (t_draw != w) {
        /* As EGL has it: the surface swapped must be the calling thread's current one. */
        g_error = EGL_BAD_SURFACE;
        return 0;
    }
    before = window_real(w, NULL, NULL);
    shown = w->window->ops->egl_swap(w->native);
    after = window_real(w, &w->width, &w->height);
    if (after != before) {
        fn_make_current make = A(fn_make_current, "eglMakeCurrent");
        fn_current_surface current = A(fn_current_surface, "eglGetCurrentSurface");
        fn_current_ctx context = A(fn_current_ctx, "eglGetCurrentContext");
        EGLSurface read = t_read == w ? after : current ? current(EGL_READ) : after;
        if (!make || !context || !make(d, after, read, context())) hk_warn("egl: the window's surface of its new size could not be made current");
    }
    if (!shown) {
        g_error = EGL_BAD_NATIVE_WINDOW;
        return 0;
    }
    return 1;
}

static EGLBoolean g_eglSwapBuffers(EGLDisplay d, EGLSurface handle)
{
    surface *w = window_surface(handle), *s;
    hk_trace("eglSwapBuffers(%p)", (void *)handle);
    if (w) return window_swap(d, w);
    s = surface_of(handle);
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

/* --------------------------------------------------------- hardware buffers
 *
 * Android's EGL makes images of hardware buffers (EGL_ANDROID_image_native_buffer, ndk/hk_ahb.c): what Chromium's GPU
 * service draws a page's canvases and WebGL into. ANGLE has no such buffers, so a buffer's image is an image of a texture
 * of ANGLE's made for it in a context of the layer's own (EGL_KHR_gl_texture_2D_image), on a thread of the layer's own that
 * keeps that context current: G_BUFFERS. The extensions are named where ANGLE has images of textures; an image of a
 * buffer is asked for with no context (EGL_NO_CONTEXT), which is the spec's.
 */
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define EGL_BAD_PARAMETER 0x300C

typedef void *(*fn_create_image)(EGLDisplay, EGLContext, EGLenum, void *, const EGLint *);

static hk_lock g_ext_lock = HK_LOCK_INIT;
static const char *g_ext_from;         /* ANGLE's string, as it was when g_ext was made of it */
static char *g_ext;
static EGLContext g_buffer_ctx;        /* the layer's own, current on G_BUFFERS */
static u32 g_buffer_fbo;

/* Whether an extension is named in a list of them. */
static bool has_ext(const char *list, const char *name)
{
    size_t n = strlen(name);
    const char *at = list;
    while (list && (at = strstr(at, name)) != NULL) {
        if ((at == list || at[-1] == ' ') && (at[n] == ' ' || at[n] == 0)) return true;
        at += n;
    }
    return false;
}

/*
 * ANGLE's extensions and the layer's own: fence syncs (below), and the two of hardware buffers where ANGLE has what they
 * are made of.
 */
static const char *layer_extensions(const char *angle)
{
    static const char fences[] = " EGL_KHR_fence_sync EGL_KHR_wait_sync EGL_ANDROID_native_fence_sync";
    static const char buffers[] = " EGL_ANDROID_get_native_client_buffer EGL_ANDROID_image_native_buffer";
    const char *out;
    bool images;
    if (!angle) return angle;
    images = has_ext(angle, "EGL_KHR_image_base") && has_ext(angle, "EGL_KHR_gl_texture_2D_image");
    hk_lock_acquire(&g_ext_lock);
    if (g_ext_from != angle || !g_ext) {
        char *ext = (char *)hk_alloc(strlen(angle) + sizeof(fences) + sizeof(buffers));
        strcpy(ext, angle);
        if (!has_ext(angle, "EGL_KHR_fence_sync")) strcat(ext, fences);
        if (images) strcat(ext, buffers);
        /* (One made before may still be read by whoever asked for it then: it is kept.) */
        g_ext = ext;
        g_ext_from = angle;
    }
    out = g_ext;
    hk_lock_release(&g_ext_lock);
    return out;
}

/* On G_BUFFERS: the layer's own context, current there from the first job on. */
static bool buffer_context(void)
{
    static const EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    fn_create_context create = A(fn_create_context, "eglCreateContext");
    fn_make_current make = A(fn_make_current, "eglMakeCurrent");
    EGLContext ctx;
    EGLSurface surface = NULL;
    if (g_buffer_ctx) return true;
    if (!display() || !create || !make) return false;
    ctx = create(g_display, NULL, NULL, es2);
    if (!ctx || !make(g_display, NULL, NULL, ctx)) {
        /* Without contexts that need no configuration, or none current without a surface: a configuration's, over a pbuffer. */
        static const EGLint want[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_NONE };
        static const EGLint one[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        fn_choose_config choose = A(fn_choose_config, "eglChooseConfig");
        fn_pbuffer pbuffer = A(fn_pbuffer, "eglCreatePbufferSurface");
        fn_destroy_context destroy = A(fn_destroy_context, "eglDestroyContext");
        EGLConfig config = NULL;
        EGLint n = 0;
        if (ctx && destroy) destroy(g_display, ctx);
        ctx = NULL;
        if (choose && pbuffer && choose(g_display, want, &config, 1, &n) && n > 0) {
            ctx = create(g_display, config, NULL, es2);
            surface = ctx ? pbuffer(g_display, config, one) : NULL;
        }
        if (!ctx || !surface || !make(g_display, surface, surface, ctx)) {
            hk_err("egl: no context of the layer's own for hardware buffers' textures; no image of one can be made");
            return false;
        }
    }
    g_buffer_ctx = ctx;
    return true;
}

enum { J_IMAGE, J_READ, J_UPLOAD, J_FORGET };
typedef struct buffer_job {
    hk_ahb *b;
    int what;
    void *image;
    bool ok;
} buffer_job;

static void buffer_job_run(void *p)
{
    enum {
        TEXTURE_2D = 0x0DE1, LINEAR = 0x2601, CLAMP_TO_EDGE = 0x812F, MIN_FILTER = 0x2801, MAG_FILTER = 0x2800, WRAP_S = 0x2802,
        WRAP_T = 0x2803, UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05, FRAMEBUFFER = 0x8D40, COLOR_ATTACHMENT0 = 0x8CE0,
        RGBA = 0x1908, UNSIGNED_BYTE = 0x1401, UNSIGNED_SHORT_5_6_5 = 0x8363
    };
    typedef void (*fn_gen)(i32, u32 *);
    typedef void (*fn_bind)(u32, u32);
    typedef void (*fn_param)(u32, u32, i32);
    typedef void (*fn_storei)(u32, i32);
    typedef void (*fn_image)(u32, i32, i32, i32, i32, i32, u32, u32, const void *);
    typedef void (*fn_sub_image)(u32, i32, i32, i32, i32, i32, u32, u32, const void *);
    typedef void (*fn_read)(i32, i32, i32, i32, u32, u32, void *);
    typedef void (*fn_fbo_texture)(u32, u32, u32, u32, i32);
    static const EGLint preserved[] = { EGL_IMAGE_PRESERVED_KHR, 1, EGL_NONE };
    buffer_job *j = (buffer_job *)p;
    hk_ahb *b = j->b;
    fn_gen gen = A(fn_gen, "glGenTextures"), del = A(fn_gen, "glDeleteTextures"), gen_fbo = A(fn_gen, "glGenFramebuffers");
    fn_bind bind = A(fn_bind, "glBindTexture"), bind_fbo = A(fn_bind, "glBindFramebuffer");
    fn_param param = A(fn_param, "glTexParameteri");
    fn_storei storei = A(fn_storei, "glPixelStorei");
    fn_image tex_image = A(fn_image, "glTexImage2D");
    fn_sub_image sub_image = A(fn_sub_image, "glTexSubImage2D");
    fn_read read = A(fn_read, "glReadPixels");
    fn_fbo_texture fbo_texture = A(fn_fbo_texture, "glFramebufferTexture2D");
    fn_gl_void flush = A(fn_gl_void, "glFlush");
    fn_create_image create_image = A(fn_create_image, "eglCreateImageKHR");
    u32 format, type, w = b->width, h = b->height;
    if (!gen || !bind || !param || !storei || !tex_image || !sub_image || !flush || !buffer_context() || !hk_ahb_gl_format(b, &format, &type)) return;
    switch (j->what) {
    case J_IMAGE:
        if (!create_image) return;
        if (!b->texture) {
            gen(1, &b->texture);
            bind(TEXTURE_2D, b->texture);
            param(TEXTURE_2D, MIN_FILTER, LINEAR);
            param(TEXTURE_2D, MAG_FILTER, LINEAR);
            param(TEXTURE_2D, WRAP_S, CLAMP_TO_EDGE);
            param(TEXTURE_2D, WRAP_T, CLAMP_TO_EDGE);
            storei(UNPACK_ALIGNMENT, 1);
            tex_image(TEXTURE_2D, 0, (i32)format, (i32)w, (i32)h, 0, format, type, b->pixels);
            b->cpu_newer = false;
        } else if (b->cpu_newer && b->pixels) {
            bind(TEXTURE_2D, b->texture);
            storei(UNPACK_ALIGNMENT, 1);
            sub_image(TEXTURE_2D, 0, 0, 0, (i32)w, (i32)h, format, type, b->pixels);
            b->cpu_newer = false;
        }
        j->image = create_image(g_display, g_buffer_ctx, EGL_GL_TEXTURE_2D_KHR, (void *)(hk_addr)b->texture, preserved);
        flush();
        j->ok = j->image != NULL;
        break;
    case J_READ:
        if (!b->texture || !b->pixels || !read || !bind_fbo || !fbo_texture || (!g_buffer_fbo && !gen_fbo)) return;
        if (!g_buffer_fbo) gen_fbo(1, &g_buffer_fbo);
        bind_fbo(FRAMEBUFFER, g_buffer_fbo);
        fbo_texture(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, b->texture, 0);
        storei(PACK_ALIGNMENT, 1);
        if (format == RGBA) {
            read(0, 0, (i32)w, (i32)h, RGBA, UNSIGNED_BYTE, b->pixels);
        } else {
            /* What every OpenGL ES reads, R, G, B, A bytes, packed into the buffer's own format. */
            u8 *rgba = (u8 *)malloc((size_t)w * h * 4);
            size_t i, n = (size_t)w * h;
            if (rgba) {
                read(0, 0, (i32)w, (i32)h, RGBA, UNSIGNED_BYTE, rgba);
                for (i = 0; i < n; i++) {
                    const u8 *s = rgba + i * 4;
                    if (type == UNSIGNED_SHORT_5_6_5) {
                        u16 v = (u16)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
                        b->pixels[i * 2] = (u8)v;
                        b->pixels[i * 2 + 1] = (u8)(v >> 8);
                    } else {
                        memcpy(b->pixels + i * 3, s, 3);
                    }
                }
                free(rgba);
            }
        }
        fbo_texture(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
        bind_fbo(FRAMEBUFFER, 0);
        j->ok = true;
        break;
    case J_UPLOAD:
        if (!b->texture || !b->pixels) return;
        bind(TEXTURE_2D, b->texture);
        storei(UNPACK_ALIGNMENT, 1);
        sub_image(TEXTURE_2D, 0, 0, 0, (i32)w, (i32)h, format, type, b->pixels);
        flush();
        j->ok = true;
        break;
    case J_FORGET:
        /* Images made of it keep what they show: an EGL image outlives its texture. */
        if (b->texture && del) del(1, &b->texture);
        b->texture = 0;
        flush();
        j->ok = true;
        break;
    }
}

static bool buffer_job_do(hk_ahb *b, int what, void **image)
{
    buffer_job j;
    memset(&j, 0, sizeof(j));
    j.b = b;
    j.what = what;
    run_on(&G_BUFFERS, buffer_job_run, &j);
    if (image) *image = j.image;
    return j.ok;
}

void *hk_egl_ahb_image(hk_ahb *b)
{
    void *image = NULL;
    hk_lock_acquire(&b->lock);
    buffer_job_do(b, J_IMAGE, &image);
    hk_lock_release(&b->lock);
    return image;
}
bool hk_egl_ahb_read(hk_ahb *b) { return buffer_job_do(b, J_READ, NULL); }
bool hk_egl_ahb_upload(hk_ahb *b) { return buffer_job_do(b, J_UPLOAD, NULL); }
void hk_egl_ahb_forget(hk_ahb *b) { buffer_job_do(b, J_FORGET, NULL); }

static void *g_eglGetNativeClientBufferANDROID(const void *buffer)
{
    hk_ahb *b = hk_ahb_of(buffer);
    if (!b) {
        g_error = EGL_BAD_PARAMETER;
        return NULL;
    }
    return (u8 *)b + HK_AHB_ANWB_OFFSET;
}

static void *g_eglCreateImageKHR(EGLDisplay d, EGLContext ctx, EGLenum target, void *buffer, const EGLint *attribs)
{
    fn_create_image real = A(fn_create_image, "eglCreateImageKHR");
    if (target == EGL_NATIVE_BUFFER_ANDROID) {
        hk_ahb *b = hk_ahb_of_client_buffer(buffer);
        void *image;
        if (!d || d != display()) {
            g_error = EGL_BAD_DISPLAY;
            return NULL;
        }
        if (!b) {
            g_error = EGL_BAD_PARAMETER;
            return NULL;
        }
        image = hk_egl_ahb_image(b);
        if (!image) g_error = EGL_BAD_ALLOC;
        return image;
    }
    return real ? real(d, ctx, target, buffer, attribs) : NULL;
}

/* ------------------------------------------------------------- fence syncs
 *
 * EGL_KHR_fence_sync, EGL_KHR_wait_sync and EGL_ANDROID_native_fence_sync, which ANGLE over Direct3D 11 does not have.
 * The ANGLE inside Chromium's WebView makes fences of both kinds through the system's EGL once a page's canvas is in a
 * hardware buffer (Chromium ends each access to one with a native fence), and called through nothing when none were
 * named. Here every context of ANGLE's puts its commands on the one Direct3D 11 device in the order they come, so a
 * fence needs nothing for the graphics processor to wait on: making one flushes the current context's commands, and the
 * CPU's wait for it (eglClientWaitSyncKHR, or asking its status) is glFinish on the context current then -- which, on
 * that one device, waits for everything put there before. A wait of the graphics processor (eglWaitSyncKHR) is nothing.
 * A native fence's file descriptor given to make one is closed, its fence taken as signalled; one is never given out
 * (eglDupNativeFenceFDANDROID answers EGL_NO_NATIVE_FENCE_FD_ANDROID): Chromium then has no fence to pass on and waits on
 * none. Other kinds of sync (EGL_KHR_reusable_sync) are ANGLE's.
 */
#define EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR 0x30F0
#define EGL_SYNC_STATUS_KHR 0x30F1
#define EGL_SIGNALED_KHR 0x30F2
#define EGL_CONDITION_SATISFIED_KHR 0x30F6
#define EGL_SYNC_TYPE_KHR 0x30F7
#define EGL_SYNC_CONDITION_KHR 0x30F8
#define EGL_SYNC_FENCE_KHR 0x30F9
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#define EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID 0x3146
#define EGL_NO_NATIVE_FENCE_FD_ANDROID (-1)
#define EGL_BAD_ATTRIBUTE 0x3004
#define EGL_BAD_MATCH 0x3009

typedef struct fence {
    struct fence *next;
    EGLenum type;
} fence;

typedef void *(*fn_create_sync)(EGLDisplay, EGLenum, const EGLint *);
typedef EGLBoolean (*fn_destroy_sync)(EGLDisplay, void *);
typedef EGLint (*fn_client_wait_sync)(EGLDisplay, void *, EGLint, u64);
typedef EGLBoolean (*fn_get_sync_attrib)(EGLDisplay, void *, EGLint, EGLint *);
typedef EGLint (*fn_wait_sync)(EGLDisplay, void *, EGLint);
typedef EGLContext (*fn_current_context)(void);

static hk_lock g_fence_lock = HK_LOCK_INIT;
static fence *g_fences;

static bool is_fence(void *sync)
{
    fence *f;
    hk_lock_acquire(&g_fence_lock);
    for (f = g_fences; f && (void *)f != sync; f = f->next) { }
    hk_lock_release(&g_fence_lock);
    return f != NULL;
}

/* glFlush or glFinish on the context current on this thread, if there is one. */
static void fence_gl(const char *name)
{
    fn_current_context current = A(fn_current_context, "eglGetCurrentContext");
    fn_gl_void gl = A(fn_gl_void, name);
    if (current && current() && gl) gl();
}

static void *g_eglCreateSyncKHR(EGLDisplay d, EGLenum type, const EGLint *attribs)
{
    fn_create_sync real = A(fn_create_sync, "eglCreateSyncKHR");
    fence *f;
    int i;
    if (type != EGL_SYNC_FENCE_KHR && type != EGL_SYNC_NATIVE_FENCE_ANDROID) return real ? real(d, type, attribs) : NULL;
    if (!d || d != display()) {
        g_error = EGL_BAD_DISPLAY;
        return NULL;
    }
    for (i = 0; attribs && attribs[i] != EGL_NONE; i += 2) {
        if (type == EGL_SYNC_NATIVE_FENCE_ANDROID && attribs[i] == EGL_SYNC_NATIVE_FENCE_FD_ANDROID) {
            /* A fence of someone else's to stand for: taken as signalled, and its descriptor is EGL's to close. */
            if (attribs[i + 1] >= 0) hk_fd_close(attribs[i + 1]);
            continue;
        }
        g_error = EGL_BAD_ATTRIBUTE;
        return NULL;
    }
    fence_gl("glFlush");
    f = (fence *)hk_alloc(sizeof(*f));
    f->type = type;
    hk_lock_acquire(&g_fence_lock);
    f->next = g_fences;
    g_fences = f;
    hk_lock_release(&g_fence_lock);
    return f;
}

static EGLBoolean g_eglDestroySyncKHR(EGLDisplay d, void *sync)
{
    fn_destroy_sync real = A(fn_destroy_sync, "eglDestroySyncKHR");
    fence **at;
    hk_lock_acquire(&g_fence_lock);
    for (at = &g_fences; *at && (void *)*at != sync; at = &(*at)->next) { }
    if (*at) {
        fence *f = *at;
        *at = f->next;
        hk_lock_release(&g_fence_lock);
        free(f);
        return 1;
    }
    hk_lock_release(&g_fence_lock);
    return real ? real(d, sync) : 0;
}

static EGLint g_eglClientWaitSyncKHR(EGLDisplay d, void *sync, EGLint flags, u64 timeout)
{
    fn_client_wait_sync real = A(fn_client_wait_sync, "eglClientWaitSyncKHR");
    if (!is_fence(sync)) return real ? real(d, sync, flags, timeout) : 0;
    fence_gl("glFinish");
    return EGL_CONDITION_SATISFIED_KHR;
}

static EGLBoolean g_eglGetSyncAttribKHR(EGLDisplay d, void *sync, EGLint attribute, EGLint *value)
{
    fn_get_sync_attrib real = A(fn_get_sync_attrib, "eglGetSyncAttribKHR");
    fence *f;
    EGLenum type = 0;
    hk_lock_acquire(&g_fence_lock);
    for (f = g_fences; f && (void *)f != sync; f = f->next) { }
    if (f) type = f->type;
    hk_lock_release(&g_fence_lock);
    if (!f) return real ? real(d, sync, attribute, value) : 0;
    if (!value) {
        g_error = EGL_BAD_PARAMETER;
        return 0;
    }
    switch (attribute) {
    case EGL_SYNC_TYPE_KHR: *value = (EGLint)type; return 1;
    case EGL_SYNC_STATUS_KHR: fence_gl("glFinish"); *value = EGL_SIGNALED_KHR; return 1;
    case EGL_SYNC_CONDITION_KHR:
        *value = type == EGL_SYNC_NATIVE_FENCE_ANDROID ? EGL_SYNC_NATIVE_FENCE_SIGNALED_ANDROID : EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR;
        return 1;
    default: g_error = EGL_BAD_ATTRIBUTE; return 0;
    }
}

static EGLint g_eglWaitSyncKHR(EGLDisplay d, void *sync, EGLint flags)
{
    fn_wait_sync real = A(fn_wait_sync, "eglWaitSyncKHR");
    if (!is_fence(sync)) return real ? real(d, sync, flags) : 0;
    return 1;
}

static EGLint g_eglDupNativeFenceFDANDROID(EGLDisplay d, void *sync)
{
    (void)d;
    if (!is_fence(sync)) g_error = EGL_BAD_PARAMETER;
    return EGL_NO_NATIVE_FENCE_FD_ANDROID;
}

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
    HK_WRAP("eglQueryString", g_eglQueryString),
    HK_WRAP("eglGetNativeClientBufferANDROID", g_eglGetNativeClientBufferANDROID),
    HK_WRAP("eglCreateImageKHR", g_eglCreateImageKHR),
    HK_WRAP("eglCreateSyncKHR", g_eglCreateSyncKHR),
    HK_WRAP("eglDestroySyncKHR", g_eglDestroySyncKHR),
    HK_WRAP("eglClientWaitSyncKHR", g_eglClientWaitSyncKHR),
    HK_WRAP("eglGetSyncAttribKHR", g_eglGetSyncAttribKHR),
    HK_WRAP("eglWaitSyncKHR", g_eglWaitSyncKHR),
    HK_WRAP("eglDupNativeFenceFDANDROID", g_eglDupNativeFenceFDANDROID),
    HK_WRAP("eglTerminate", g_eglTerminate),
    HK_WRAP("eglChooseConfig", g_eglChooseConfig),
    HK_WRAP("eglGetConfigAttrib", g_eglGetConfigAttrib),
    HK_WRAP("eglCreateWindowSurface", g_eglCreateWindowSurface),
    HK_WRAP("eglCreatePbufferSurface", g_eglCreatePbufferSurface),
    HK_WRAP("eglCreateContext", g_eglCreateContext),
    HK_WRAP("eglDestroySurface", g_eglDestroySurface),
    HK_WRAP("eglQuerySurface", g_eglQuerySurface),
    HK_WRAP("eglSwapBuffers", g_eglSwapBuffers),
    HK_WRAP("eglMakeCurrent", g_eglMakeCurrent),
    HK_WRAP("eglGetCurrentSurface", g_eglGetCurrentSurface),
    HK_WRAP("eglSurfaceAttrib", g_eglSurfaceAttrib),
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
