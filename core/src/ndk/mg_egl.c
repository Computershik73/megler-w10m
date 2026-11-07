#include "mg_egl.h"

#include <stdio.h>

#include "../bionic/mg_bionic.h"
#include "../bionic/mg_fd.h"
#include "../mg_core.h"
#include "../host/mg_host.h"
#include "../ld/mg_ld.h"
#include "../run/mg_run.h"
#include "mg_ahb.h"
#include "mg_ndk.h"

/* Where the function this is written in returns to: called by a guest, an address in the guest's code. */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define CALLER() _ReturnAddress()
#else
#define CALLER() __builtin_return_address(0)
#endif

/* ------------------------------------------------------------- the source */

static void *(*g_provider)(const char *name);
static void *g_lib_egl, *g_lib_gles;
static bool g_angle_tried;
static mg_lock g_egl_lock = MG_LOCK_INIT;      /* the display */
static mg_lock g_angle_lock = MG_LOCK_INIT;    /* loading ANGLE: taken while the display's lock is held */

void mg_egl_set_provider(void *(*resolve)(const char *name)) { g_provider = resolve; }

/* On x86_64 and x86 ANGLE's function is given as its adapter (mg_gl_adapters.c), of the core's convention, which is the guest's. */
#if MG_GL_ADAPTERS
#define ADAPTED(name, p) mg_gl_adapter(name, p)
#else
#define ADAPTED(name, p) (p)
#endif

/* ANGLE's own function by name, or its adapter: everything here, and everything given to guest code, calls ANGLE through this. */
static void *angle(const char *name)
{
    void *p;
    if (g_provider) return ADAPTED(name, g_provider(name));
    if (!g_angle_tried) {
        mg_lock_acquire(&g_angle_lock);
        if (!g_angle_tried) {
            g_lib_gles = mg_dl_open("libGLESv2.dll");
            g_lib_egl = mg_dl_open("libEGL.dll");
            if (!g_lib_egl || !g_lib_gles)
                mg_err("egl: ANGLE did not load (libEGL.dll %s, libGLESv2.dll %s)", g_lib_egl ? "ok" : "missing", g_lib_gles ? "ok" : "missing");
            g_angle_tried = true;
        }
        mg_lock_release(&g_angle_lock);
    }
    p = g_lib_egl ? mg_dl_sym(g_lib_egl, name) : NULL;
    if (!p && g_lib_gles) p = mg_dl_sym(g_lib_gles, name);
    return ADAPTED(name, p);
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
#define EGL_FEATURE_OVERRIDES_DISABLED_ANGLE 0x3467
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
typedef EGLDisplay (*fn_platform_display_attrib)(EGLenum, void *, const intptr_t *);
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
static MG_THREAD_LOCAL EGLint g_error = EGL_SUCCESS;

/* A display to be made: for whom (NULL: the app), the native display ANGLE is given, and what came of it. */
typedef struct made_display {
    const char *whose;
    void *native;
    EGLDisplay d;
    EGLint major, minor;
    char how[48];
} made_display;

/*
 * ANGLE's own choices that the app's display is made without (EGL_ANGLE_feature_control, by feature name):
 *
 * allowTranslateUniformBlockToStructuredBuffer -- on Windows 10 ANGLE turns a uniform block that is one array of structures
 * into a structured buffer (it compiles faster with dynamic indexing), and then makes a Direct3D buffer of its own, with
 * its view, for each range of a GL buffer bound to it. A game that keeps its uniforms in one large buffer and binds a new
 * range of it for each draw (glBindBufferRange) has a new Direct3D buffer made for nearly every draw: in a race of
 * Asphalt 9's, two thirds of the render thread went to ResourceManager11::allocate<ID3D11Buffer> and the driver's calls
 * into the kernel below it, at one or two frames a second, with the graphics processor idle. Without it a uniform block is
 * a constant buffer, and a bound range is that buffer at an offset (Direct3D 11.1's constant buffer offsets).
 *
 * The WebView's display is made as ANGLE would make it: Chromium chooses for itself.
 */
static const char *const k_app_features_off[] = { "allowTranslateUniformBlockToStructuredBuffer", NULL };

/*
 * The feature level of the graphics processor's own Direct3D where it is below the 9_3 ANGLE needs -- a Surface RT's and a
 * Surface 2's Tegra drivers offer 9_1 (docs/RT.md) -- and 0 where it is not: 9_3 and up, as every phone's, or no Direct3D at
 * all. The flag egl-level-9_1 takes it for 9_1 whatever it is, to try that way on a machine that has more.
 */
static int below_9_3(void)
{
    const int level = mg_flag("egl-level-9_1") ? 0x9100 : mg_host_d3d11_hardware_level();
    return level > 0 && level < 0x9300 ? level : 0;
}
static bool g_said_software;

static void make_display(made_display *m)
{
    /*
     * What a feature-level-10+ GPU takes, then what a 9_3 phone needs. Software rendering is not worth trying on a phone: WARP,
     * Direct3D in software, is the third attempt, made only once those two have failed on a graphics processor below 9_3
     * (below_9_3), which no phone's is -- a Surface RT's or a Surface 2's, whose ANGLE has no other way to draw.
     */
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
    static const EGLint warp[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE,
        EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE, 1,
        EGL_NONE,
    };
    static const struct { const EGLint *attribs; const char *name; } tries[] = {
        { by_default, "Direct3D 11" },
        { level_9_3, "Direct3D 11 at feature level 9_3" },
        { warp, "Direct3D 11 on WARP, in software" },
    };
    fn_platform_display get = A(fn_platform_display, "eglGetPlatformDisplayEXT");
    /* EGL 1.5's, whose attributes are as wide as a pointer: the features turned off are a list of names. */
    fn_platform_display_attrib get_attrib = m->whose ? NULL : A(fn_platform_display_attrib, "eglGetPlatformDisplay");
    fn_initialize init = A(fn_initialize, "eglInitialize");
    size_t i;
    if (!get) get = A(fn_platform_display, "eglGetPlatformDisplay");
    if (!get || !init) return;
    for (i = 0; i < MG_COUNT(tries); i++) {
        EGLDisplay d;
        if (tries[i].attribs == warp) {
            const int level = below_9_3();
            if (!level) break;
            if (!g_said_software) {
                g_said_software = true;
                mg_warn("egl: the graphics processor has feature level %d_%d: OpenGL ES drawn in software by WARP, slowly", level >> 12,
                        (level >> 8) & 15);
            }
        }
        /* Said before it is tried: this is the first thing ANGLE does on a device, and where a bad package would end the process. */
        mg_info("egl: asking ANGLE for a display%s%s as %s%s", m->whose ? " for " : "", m->whose ? m->whose : "", tries[i].name,
                get_attrib ? ", without its allowTranslateUniformBlockToStructuredBuffer" : "");
        if (get_attrib) {
            intptr_t attribs[16];
            size_t k = 0, n;
            for (n = 0; tries[i].attribs[n] != EGL_NONE && k + 4 < MG_COUNT(attribs); n += 2) {
                attribs[k++] = tries[i].attribs[n];
                attribs[k++] = tries[i].attribs[n + 1];
            }
            attribs[k++] = EGL_FEATURE_OVERRIDES_DISABLED_ANGLE;
            attribs[k++] = (intptr_t)k_app_features_off;
            attribs[k] = EGL_NONE;
            d = get_attrib(EGL_PLATFORM_ANGLE_ANGLE, m->native, attribs);
        } else {
            d = get(EGL_PLATFORM_ANGLE_ANGLE, m->native, tries[i].attribs);
        }
        if (d && init(d, &m->major, &m->minor)) {
            mg_strlcpy(m->how, tries[i].name, sizeof(m->how));
            m->d = d;
            return;
        }
        mg_warn("egl: no display%s%s as %s", m->whose ? " for " : "", m->whose ? m->whose : "", tries[i].name);
    }
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
    mg_lock lock;
    mg_cond wake, done;
    void (*fn)(void *);
    void *arg;
    u32 posted, finished;
    mg_thread *thread;
    const char *name;
} service;

/*
 * Three such threads: this one, and for each of the layer's displays one for hardware buffers' textures (below), which keeps
 * a context of the layer's own current, and whose jobs come from the app's graphics threads while this one may be waiting
 * for the shell.
 */
static service G_SERVICE = { MG_LOCK_INIT, MG_COND_INIT, MG_COND_INIT, 0, 0, 0, 0, 0, "graphics-service" };
static service G_BUFFERS = { MG_LOCK_INIT, MG_COND_INIT, MG_COND_INIT, 0, 0, 0, 0, 0, "graphics-buffers" };
static service G_BUFFERS_WEBVIEW = { MG_LOCK_INIT, MG_COND_INIT, MG_COND_INIT, 0, 0, 0, 0, 0, "graphics-buffers-webview" };

static MG_HOSTCB void service_main(void *p)
{
    service *s = (service *)p;
    mg_lock_acquire(&s->lock);
    for (;;) {
        void (*fn)(void *);
        void *arg;
        while (!s->fn) mg_cond_wait(&s->wake, &s->lock);
        fn = s->fn;
        arg = s->arg;
        mg_lock_release(&s->lock);
        fn(arg);
        mg_lock_acquire(&s->lock);
        s->fn = NULL;
        s->finished++;
        mg_cond_broadcast(&s->done);
    }
}

static void run_on(service *s, void (*fn)(void *), void *arg)
{
    u32 mine;
    mg_lock_acquire(&s->lock);
    if (!s->thread) s->thread = mg_thread_start(service_main, s, 512 * 1024, s->name);
    if (!s->thread) {
        mg_lock_release(&s->lock);
        fn(arg);
        return;
    }
    /* One job at a time, in the order asked. */
    while (s->fn) mg_cond_wait(&s->done, &s->lock);
    s->fn = fn;
    s->arg = arg;
    mine = ++s->posted;
    mg_cond_broadcast(&s->wake);
    while ((i32)(s->finished - mine) < 0) mg_cond_wait(&s->done, &s->lock);
    mg_lock_release(&s->lock);
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
    const megler_run_config *run = mg_run_config();
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

static void make_display_job(void *p) { make_display((made_display *)p); }

bool mg_egl_display_try(char *how, size_t size)
{
    made_display m;
    memset(&m, 0, sizeof(m));
    make_display(&m);
    mg_strlcpy(how, m.how, size);
    return m.d != NULL;
}

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
        mg_info("egl: told as EGL 1.4: this EGL %d.%d has no fence syncs (EGL_KHR_fence_sync)", g_major, g_minor);
    }
}

/* ------------------------------------------------------------- ANGLE's programs kept on disk */

/*
 * EGL_ANDROID_blob_cache on the app's display: ANGLE hands over each program it links -- its shaders translated to HLSL
 * and compiled by d3dcompiler, which on the phone takes tens to hundreds of milliseconds a program -- and asks for it
 * again before it links the same one. On Android the system's EGL keeps those for every app (its shader cache); here no
 * one did, and every start of a game or of a map compiled every shader again. Each entry is a file of the app's data,
 * angle-cache\<the key's hash>.blob: "MGBC", the key's and the value's sizes, the key (compared, as two keys may share a
 * hash), the value. Written beside, then renamed over. At most BLOB_CACHE_MAX bytes: past that the folder is emptied
 * and fills again. The WebView's display is left to Chromium, which keeps its own.
 */
typedef intptr_t blob_size;
typedef void (*fn_set_blob)(const void *key, blob_size key_size, const void *value, blob_size value_size);
typedef blob_size (*fn_get_blob)(const void *key, blob_size key_size, void *value, blob_size value_size);
typedef void (*fn_set_blob_funcs)(EGLDisplay, fn_set_blob, fn_get_blob);
#define BLOB_CACHE_MAX (48u << 20)
static mg_lock g_blob_lock = MG_LOCK_INIT;
static char g_blob_dir[600];
static u64 g_blob_bytes;
static bool g_blob_counted;
static volatile i32 g_blob_hits, g_blob_misses, g_blob_stores;

static u64 blob_hash(const void *key, blob_size size)
{
    const u8 *p = (const u8 *)key;
    u64 h = 1469598103934665603ull;
    blob_size i;
    for (i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void blob_path(char *out, size_t size, const void *key, blob_size key_size)
{
    snprintf(out, size, "%s/%016llx.blob", g_blob_dir, (unsigned long long)blob_hash(key, key_size));
}

static MG_HOSTCB void blob_count_one(const char *name, bool is_dir, void *user)
{
    char path[700];
    mg_stat st;
    (void)user;
    if (is_dir) return;
    snprintf(path, sizeof(path), "%s/%s", g_blob_dir, name);
    if (mg_path_stat(path, &st, NULL)) g_blob_bytes += st.size;
}

static MG_HOSTCB void blob_remove_one(const char *name, bool is_dir, void *user)
{
    char path[700];
    (void)user;
    if (is_dir) return;
    snprintf(path, sizeof(path), "%s/%s", g_blob_dir, name);
    mg_path_remove(path, NULL);
}

static blob_size blob_get(const void *key, blob_size key_size, void *value, blob_size value_size)
{
    char path[700];
    size_t size = 0;
    u8 *data;
    u32 ks, vs;
    blob_size found = 0;
    if (!key || key_size <= 0) return 0;
    blob_path(path, sizeof(path), key, key_size);
    mg_lock_acquire(&g_blob_lock);
    data = mg_file_read_all(path, &size);
    mg_lock_release(&g_blob_lock);
    if (data && size >= 12 && !memcmp(data, "MGBC", 4)) {
        memcpy(&ks, data + 4, 4);
        memcpy(&vs, data + 8, 4);
        if ((blob_size)ks == key_size && 12ull + ks + vs == size && !memcmp(data + 12, key, (size_t)ks)) {
            found = (blob_size)vs;
            /* Asked with room enough, it is copied; asked with less (or none), its size alone is the answer. */
            if (value && value_size >= found) memcpy(value, data + 12 + ks, vs);
        }
    }
    free(data);
    if (found && value) {
        i32 n = mg_atomic_add(&g_blob_hits, 1);
        if (n == 1 || n == 10 || n == 100 || n == 1000)
            mg_info("egl: ANGLE's program cache: %d programs found there, %d not, %d kept", (int)n, (int)g_blob_misses, (int)g_blob_stores);
    } else if (!found) {
        mg_atomic_add(&g_blob_misses, 1);
    }
    return found;
}

static void blob_set(const void *key, blob_size key_size, const void *value, blob_size value_size)
{
    char path[700], next[720];
    u8 *data;
    u32 ks = (u32)key_size, vs = (u32)value_size;
    size_t size;
    if (!key || key_size <= 0 || !value || value_size <= 0 || (u64)key_size + (u64)value_size > BLOB_CACHE_MAX / 4) return;
    size = 12 + (size_t)ks + vs;
    data = (u8 *)malloc(size);
    if (!data) return;
    memcpy(data, "MGBC", 4);
    memcpy(data + 4, &ks, 4);
    memcpy(data + 8, &vs, 4);
    memcpy(data + 12, key, ks);
    memcpy(data + 12 + ks, value, vs);
    blob_path(path, sizeof(path), key, key_size);
    snprintf(next, sizeof(next), "%s.new", path);
    mg_lock_acquire(&g_blob_lock);
    if (!g_blob_counted) {
        g_blob_counted = true;
        g_blob_bytes = 0;
        mg_path_list(g_blob_dir, blob_count_one, NULL, NULL);
    }
    if (g_blob_bytes + size > BLOB_CACHE_MAX) {
        mg_info("egl: ANGLE's program cache reached %u MB: emptied, to fill again", (unsigned)(g_blob_bytes >> 20));
        mg_path_list(g_blob_dir, blob_remove_one, NULL, NULL);
        g_blob_bytes = 0;
    }
    if (mg_file_write_all(next, data, size) && mg_path_rename(next, path, NULL)) {
        i32 n = mg_atomic_add(&g_blob_stores, 1);
        g_blob_bytes += size;
        if (n == 1 || n == 10 || n == 100 || n == 1000)
            mg_info("egl: ANGLE's program cache: %d programs kept, %u KB in all", (int)n, (unsigned)(g_blob_bytes >> 10));
    } else {
        mg_path_remove(next, NULL);
    }
    mg_lock_release(&g_blob_lock);
    free(data);
}

/*
 * The app's display given the cache, before its first context is made (g_eglCreateContext): a program is linked in a
 * context, and the functions may be given a display once. Tried until a display that has the extension takes them.
 */
static volatile bool g_blob_started;
static mg_lock g_blob_start_lock = MG_LOCK_INIT;
static EGLDisplay display(void);

static void blob_cache_offer(EGLDisplay d)
{
    fn_set_blob_funcs set;
    fn_query_string qs;
    const char *data = megler_data_dir(), *extensions;
    if (g_blob_started || !d || d != display() || !data || !*data) return;
    mg_lock_acquire(&g_blob_start_lock);
    qs = A(fn_query_string, "eglQueryString");
    extensions = qs ? qs(d, EGL_EXTENSIONS) : NULL;
    set = A(fn_set_blob_funcs, "eglSetBlobCacheFuncsANDROID");
    if (!g_blob_started && extensions && strstr(extensions, "EGL_ANDROID_blob_cache") && set && !mg_flag("no-angle-cache")) {
        snprintf(g_blob_dir, sizeof(g_blob_dir), "%s/angle-cache", data);
        if (!mg_path_mkdirs(g_blob_dir)) {
            mg_warn("egl: no folder for ANGLE's program cache (%s): its programs are compiled at each start", g_blob_dir);
        } else {
            set(d, blob_set, blob_get);
            g_blob_started = true;
            mg_info("egl: ANGLE's programs kept in %s (EGL_ANDROID_blob_cache), at most %u MB", g_blob_dir, BLOB_CACHE_MAX >> 20);
        }
    }
    mg_lock_release(&g_blob_start_lock);
}

/* The display, initialised; NULL when ANGLE cannot make one. */
static EGLDisplay display(void)
{
    if (g_initialized) return g_display;
    mg_lock_acquire(&g_egl_lock);
    if (!g_initialized) {
        made_display m;
        memset(&m, 0, sizeof(m));
        /* The flag "no-egl": there is no display, as on a device ANGLE cannot run on. */
        if (!mg_flag("no-egl")) on_service_thread(make_display_job, &m);
        g_display = m.d;
        g_major = m.major;
        g_minor = m.minor;
        mg_strlcpy(g_how, m.how, sizeof(g_how));
        if (g_display) {
            fn_query_display_attrib qd = A(fn_query_display_attrib, "eglQueryDisplayAttribEXT");
            fn_query_device_attrib qv = A(fn_query_device_attrib, "eglQueryDeviceAttribEXT");
            intptr_t device = 0, d3d = 0;
            if (qd && qv && qd(g_display, EGL_DEVICE_EXT, &device) && device && qv((void *)device, EGL_D3D11_DEVICE_ANGLE, &d3d)) {
                g_d3d_device = (void *)d3d;
                g_d3d_level = mg_host_d3d11_feature_level(g_d3d_device);
            }
            mg_info("egl: ANGLE display as %s, EGL %d.%d, Direct3D feature level %d_%d", g_how, g_major, g_minor, g_d3d_level >> 12, (g_d3d_level >> 8) & 15);
            {
                bool offsets = false, partial = false;
                if (mg_host_d3d11_options(g_d3d_device, &offsets, &partial))
                    mg_info("egl: the driver's Direct3D 11.1: constant buffers at an offset %s, updated in part %s%s", offsets ? "yes" : "no", partial ? "yes" : "no",
                            offsets ? "" : " (ANGLE makes a buffer of its own for each range of a uniform buffer bound)");
            }
            {
                fn_query_string qs = A(fn_query_string, "eglQueryString");
                const char *ext = qs ? qs(g_display, EGL_EXTENSIONS) : NULL;
                size_t n = ext ? strlen(ext) : 0, at = 0;
                while (at < n) {
                    size_t take = n - at > 700 ? 700 : n - at;
                    mg_info("egl extensions: %.*s", (int)take, ext + at);
                    at += take;
                }
                tell_version(ext, qs ? qs(g_display, EGL_VERSION) : NULL);
            }
        } else {
            mg_err("egl: ANGLE could not make a display");
        }
        g_initialized = true;
    }
    mg_lock_release(&g_egl_lock);
    return g_display;
}

/*
 * The WebView's display. ANGLE's Direct3D 11 renderer belongs to a display and serves every context made on it with one
 * state manager, which keeps what was last bound and set (the vertex array, the framebuffer, the program, the pipeline's
 * states), and with its device's one immediate context. It counts on that display's contexts taking turns: eglMakeCurrent
 * makes a context current, which resets those caches and marks the whole state of the context now current to be set again.
 * Two threads that each keep a context of the same display current and draw take no turns. Neither makes current again;
 * each draws with what the other left bound, and both use the immediate context, which is not made for two threads, at
 * once -- ANGLE as gecko-w10m builds it gives each context that shares nothing a lock of its own, and lets them. A game
 * drawing while the WebView's Chromium draws on its GPU thread is that: Asphalt 9 ended in glDrawElements, ANGLE reading the
 * vertex array through a null pointer the other context's eglMakeCurrent had left, a second and a half after the WebView of
 * an ad page started (package 276). So Chromium's code has a display of its own. ANGLE makes another display for another
 * native display: EGL_D3D11_ONLY_DISPLAY_ANGLE, which beside a Direct3D 11 renderer asked for by name says nothing more,
 * and the display comes with a renderer, a state manager and a Direct3D device of its own. Which code is Chromium's is
 * told by where eglGetDisplay is called from: libwebviewchromium.so. Where ANGLE cannot make it, the WebView shares the
 * app's display, as before.
 */
#define EGL_D3D11_ONLY_DISPLAY_ANGLE ((void *)(mg_addr)-3)
static EGLDisplay g_webview_display;
static bool g_webview_initialized;

static EGLDisplay webview_display(void)
{
    if (g_webview_initialized) return g_webview_display;
    if (!display()) return NULL;
    mg_lock_acquire(&g_egl_lock);
    if (!g_webview_initialized) {
        made_display m;
        memset(&m, 0, sizeof(m));
        m.whose = "the WebView";
        m.native = EGL_D3D11_ONLY_DISPLAY_ANGLE;
        on_service_thread(make_display_job, &m);
        if (m.d && m.d != g_display) {
            g_webview_display = m.d;
            mg_info("egl: the WebView's Chromium draws on a display of its own (%s, with a Direct3D device of its own), not on the app's", m.how);
        } else {
            mg_warn("egl: no display of its own for the WebView's Chromium: it draws on the app's");
        }
        g_webview_initialized = true;
    }
    mg_lock_release(&g_egl_lock);
    return g_webview_display;
}

/* Whether a display is one of the layer's: the app's or the WebView's. */
static bool ours(EGLDisplay d) { return d && (d == display() || (g_webview_initialized && d == g_webview_display)); }

/* Which of the layer's displays a display is: 1 the WebView's, 0 the app's (or one that is neither). */
static int which_display(EGLDisplay d) { return d && g_webview_initialized && d == g_webview_display ? 1 : 0; }

/*
 * Whether the WebView's Chromium is given that display of its own: only with the flag webview-own-display (or a test's
 * mg_egl_webview_own_display). Since the turns below, two threads' contexts of one display drawing at once are made to take
 * turns, which is what the WebView's own display was for; and a display of its own is a Direct3D device of its own, with
 * its own heaps of graphics memory in the process's 2 GB: Spotify's WebView, drawn on the graphics processor through the
 * draw functor (drivers/mg_java_webview.c), took 225 MB of addresses within seconds of making it, its largest free stretch
 * went from 279 MB to 62, and Chromium ended asking for 120 MB in one piece (package 555). So it shares the app's.
 */
static i32 g_webview_own = -1;

void mg_egl_webview_own_display(bool on) { g_webview_own = on ? 1 : 0; }

void *mg_egl_display_for_library(const char *name)
{
    if (g_webview_own < 0) g_webview_own = mg_flag("webview-own-display") ? 1 : 0;
    if (name && g_webview_own > 0 && strstr(name, "libwebviewchromium")) {
        EGLDisplay w = webview_display();
        if (w) return w;
    }
    return display();
}

void *mg_egl_webview_display(void) { return webview_display(); }

/* --------------------------------------------------------------- surfaces */

/*
 * The app's window surfaces. On the window a driver made for the app's whole surface, a surface is EGL's own, over the
 * shell's texture. On a window of someone else's -- a SurfaceView's, which native code had from ANativeWindow_fromSurface
 * (drivers/mg_java_ndk.c) -- it is the window's own (`native`, mg_nwindow_ops), and what the app is given as its EGLSurface is
 * this entry: the surface of EGL's that it stands for is another once the window has been resized (a swap after the size
 * changed makes a new one, as Android's next buffer has the window's new size), so every EGL call that takes one is given the
 * one it stands for now (real_of), and eglGetCurrentSurface gives the app's back.
 */
typedef struct surface {
    EGLSurface handle;                 /* what the app was given: EGL's own surface, or this entry for a window's own */
    mg_nwindow *window;
    int width, height;
    bool from_host;                    /* its texture is the shell's */
    void *native;                      /* the window's own surface (mg_nwindow_ops.egl_create); NULL for one of EGL's */
    int current;                       /* a window's own: the threads it is current on, as draw and as read, each counted */
    bool doomed;                       /* eglDestroySurface came while it was current: it ends once the last thread lets it go */
} surface;
static surface g_surfaces[16];
static mg_lock g_surfaces_lock = MG_LOCK_INIT;
/* The windows' own surfaces current on this thread, as the app named them (eglMakeCurrent's draw and read). */
static MG_THREAD_LOCAL surface *t_draw, *t_read;

static surface *surface_of(EGLSurface s)
{
    size_t i;
    surface *found = NULL;
    if (!s) return NULL;
    mg_lock_acquire(&g_surfaces_lock);
    for (i = 0; i < MG_COUNT(g_surfaces) && !found; i++)
        if (g_surfaces[i].handle == s) found = &g_surfaces[i];
    mg_lock_release(&g_surfaces_lock);
    return found;
}

/* The entry a handle is, when it is one of the windows' own surfaces (the app's handle is the entry's address). */
static surface *window_surface(EGLSurface h)
{
    mg_addr at = (mg_addr)h, base = (mg_addr)g_surfaces;
    surface *s;
    if (at < base || at >= base + sizeof(g_surfaces) || (at - base) % sizeof(surface)) return NULL;
    s = (surface *)h;
    return s->native && s->handle == h ? s : NULL;
}

/* The surface of EGL's that a window's own stands for now, and its size. */
static EGLSurface window_real(surface *s, int *width, int *height) { return (EGLSurface)s->window->ops->egl_handle(s->native, width, height); }

/*
 * An EGL call of the app's that EGL refused: said in the log, the first ten of each kind, with the error -- which stays the
 * app's to read with eglGetError, as if nothing had asked first.
 */
static void refused(const char *what, volatile i32 *said)
{
    typedef EGLint (*fn_error)(void);
    fn_error err = A(fn_error, "eglGetError");
    EGLint e = err ? err() : 0;
    i32 n;
    if (e && e != EGL_SUCCESS) g_error = e;
    n = mg_atomic_add(said, 1);
    if (n <= 10) mg_warn("egl: %s was refused (EGL error 0x%x)%s", what, (unsigned)e, n == 10 ? "; the next of these are not said" : "");
}

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
    const void *pc = CALLER();
    mg_lib *lib = pc ? mg_ld_lib_of(pc) : NULL;
    (void)native;
    /* The WebView's display for Chromium's code (webview_display), the app's for everyone else's. */
    return mg_egl_display_for_library(lib ? mg_ld_lib_name(lib) : NULL);
}

static EGLBoolean g_eglInitialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    if (!ours(d)) {
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
    if (name == EGL_VERSION && ours(d) && g_told_version[0]) return g_told_version;
    if (name == EGL_EXTENSIONS && ours(d)) return layer_extensions(real ? real(d, name) : NULL);
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
    if (!real(d, copy, all, (EGLint)MG_COUNT(all), &found)) return 0;
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
    else mg_info("egl: the app's configuration is %d%d%d%d%s; its window uses an 8888 one", r, g, b, a, samples > 1 ? " multisampled" : "");
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
 * only while the shell copies a frame out of it (drivers/mg_java_gl.c). With key 0 throughout: whoever has it next takes it.
 */
bool mg_egl_keyed_mutex(void *mutex, bool acquire)
{
    typedef unsigned (*fn_keyed)(void *, int);
    if (!mutex) return false;
    /* (The tests' stand-in for ANGLE has mutexes of its own: the seam is its "function" by this name.) */
    if (g_provider) {
        fn_keyed f = (fn_keyed)g_provider("meglerKeyedMutexANGLE");
        return f && f(mutex, acquire ? 1 : 0);
    }
    return mg_host_keyed_mutex(mutex, acquire, acquire ? 1000 : 0);
}

void *mg_egl_surface_mutex(void *d, void *surface)
{
    typedef EGLBoolean (*fn_pointer)(EGLDisplay, EGLSurface, EGLint, void **);
    fn_pointer query = A(fn_pointer, "eglQuerySurfacePointerANGLE");
    void *mutex = NULL;
    if (!query || !surface || !query(d, surface, EGL_DXGI_KEYED_MUTEX_ANGLE, &mutex)) return NULL;
    return mutex;
}

static EGLSurface g_eglCreatePbufferSurface(EGLDisplay d, EGLConfig config, const EGLint *attribs)
{
    static volatile i32 said, refused_mutex, refused_made;
    fn_pbuffer real = A(fn_pbuffer, "eglCreatePbufferSurface");
    EGLSurface h = real ? real(d, config, attribs) : NULL;
    void *mutex;
    if (!h) {
        refused("eglCreatePbufferSurface", &refused_made);
        return NULL;
    }
    mutex = mg_egl_surface_mutex(d, h);
    if (!mutex) return h;
    if (!mg_atomic_xchg(&said, 1))
        mg_info("egl: ANGLE shares its pbuffers' textures with a keyed mutex: each is held by ANGLE's device from its making, else what is drawn in it is dropped");
    if (!mg_egl_keyed_mutex(mutex, true) && mg_atomic_add(&refused_mutex, 1) <= 10)
        mg_warn("egl: a new pbuffer's keyed mutex could not be acquired: what is drawn in it will be dropped");
    return h;
}

/*
 * Contexts are made without a configuration where ANGLE allows it, so a
 * context goes with whatever configuration its surface ended up with.
 */
/*
 * The app's contexts that live, by share group, for what their objects take (mg_egl_objects_memory): ANGLE counts a share
 * group's buffers, textures and renderbuffers once, asked of any context in it. Sixteen at most; more go uncounted.
 */
static struct { EGLDisplay d; EGLContext ctx; u32 group; } g_live[16];
static u32 g_live_groups;
static mg_lock g_live_lock = MG_LOCK_INIT;

static void live_add(EGLDisplay d, EGLContext ctx, EGLContext share)
{
    u32 group = 0;
    size_t i;
    mg_lock_acquire(&g_live_lock);
    for (i = 0; share && i < MG_COUNT(g_live); i++)
        if (g_live[i].ctx == share) group = g_live[i].group;
    if (!group) group = ++g_live_groups;
    for (i = 0; i < MG_COUNT(g_live); i++)
        if (!g_live[i].ctx) {
            g_live[i].d = d;
            g_live[i].ctx = ctx;
            g_live[i].group = group;
            break;
        }
    mg_lock_release(&g_live_lock);
}

static void live_remove(EGLContext ctx)
{
    size_t i;
    mg_lock_acquire(&g_live_lock);
    for (i = 0; i < MG_COUNT(g_live); i++)
        if (g_live[i].ctx == ctx) g_live[i].ctx = NULL;
    mg_lock_release(&g_live_lock);
}

bool mg_egl_objects_memory(u32 *mb)
{
    enum { CONTEXT_MEMORY_USAGE_ANGLE = 0x3462 };
    typedef EGLBoolean (*fn_query_context)(EGLDisplay, EGLContext, EGLint, EGLint *);
    static int known = -1;
    fn_query_context query;
    u32 counted[MG_COUNT(g_live)];
    size_t i, j, ncounted = 0;
    u64 total = 0;
    bool any = false;
    /* (Nothing asked of ANGLE before the app has a context: an app that never draws with OpenGL does not load it for this.) */
    for (i = 0; i < MG_COUNT(g_live) && !g_live[i].ctx; i++) {}
    if (i == MG_COUNT(g_live) || !g_display) return false;
    query = A(fn_query_context, "eglQueryContext");
    if (known < 0) {
        fn_query_string qs = A(fn_query_string, "eglQueryString");
        const char *ext = qs && g_display ? qs(g_display, EGL_EXTENSIONS) : NULL;
        if (!ext) return false;
        known = strstr(ext, "EGL_ANGLE_memory_usage_report") ? 1 : 0;
    }
    if (!known || !query) return false;
    mg_lock_acquire(&g_live_lock);
    for (i = 0; i < MG_COUNT(g_live); i++) {
        EGLint v[2] = { 0, 0 };
        if (!g_live[i].ctx) continue;
        for (j = 0; j < ncounted && counted[j] != g_live[i].group; j++) {}
        if (j < ncounted) continue;
        if (!query(g_live[i].d, g_live[i].ctx, CONTEXT_MEMORY_USAGE_ANGLE, v)) continue;
        counted[ncounted++] = g_live[i].group;
        total += (u64)(u32)v[0] | (u64)(u32)v[1] << 32;
        any = true;
    }
    mg_lock_release(&g_live_lock);
    *mb = (u32)(total >> 20);
    return any;
}

static EGLContext g_eglCreateContext(EGLDisplay d, EGLConfig config, EGLContext share, const EGLint *attribs)
{
    static int no_config = -1;
    static volatile i32 said;
    fn_create_context real = A(fn_create_context, "eglCreateContext");
    EGLContext made;
    mg_trace("eglCreateContext(share %p)", (void *)share);
    if (!real) return NULL;
    if (no_config < 0) {
        fn_query_string qs = A(fn_query_string, "eglQueryString");
        const char *ext = qs ? qs(d, EGL_EXTENSIONS) : NULL;
        no_config = ext && strstr(ext, "EGL_KHR_no_config_context") ? 1 : 0;
    }
    if (!g_blob_started) blob_cache_offer(d);
    made = real(d, no_config ? NULL : config, share, attribs);
    if (!made) refused("eglCreateContext", &said);
    else live_add(d, made, share);
    mg_trace("eglCreateContext -> %p", made);
    return made;
}

/* A free entry, taken (its handle set to something not null until the caller fills it); NULL when all are in use. */
static surface *take_entry(void)
{
    size_t i;
    surface *s = NULL;
    mg_lock_acquire(&g_surfaces_lock);
    for (i = 0; i < MG_COUNT(g_surfaces) && !s; i++)
        if (!g_surfaces[i].handle) s = &g_surfaces[i];
    if (s) {
        memset(s, 0, sizeof(*s));
        s->handle = (EGLSurface)s;
    }
    mg_lock_release(&g_surfaces_lock);
    return s;
}

static void free_entry(surface *s)
{
    mg_lock_acquire(&g_surfaces_lock);
    memset(s, 0, sizeof(*s));
    mg_lock_release(&g_surfaces_lock);
}

/*
 * A window surface on a window of someone else's (a SurfaceView's): the window's own, made with the configurations an 8888
 * surface can have that are nearest the app's (the window's frames go to the shell as 8888 textures), the app's first.
 */
static EGLSurface window_surface_create(EGLDisplay d, EGLConfig config, mg_nwindow *window)
{
    EGLConfig candidates[32];
    surface *s;
    void *native;
    int count;
    if (!mg_nwindow_valid(window)) {
        mg_info("egl: no window surface on a window whose surface has gone");
        g_error = EGL_BAD_NATIVE_WINDOW;
        return NULL;
    }
    s = take_entry();
    if (!s) {
        mg_warn("egl: no window surface: %d are in use", (int)MG_COUNT(g_surfaces));
        g_error = EGL_BAD_ALLOC;
        return NULL;
    }
    count = surface_configs(d, config, candidates, (int)MG_COUNT(candidates));
    native = window->ops->egl_create(window, candidates, count);
    if (!native) {
        free_entry(s);
        mg_warn("egl: no window surface on a view's window (EGL_BAD_ALLOC to the app)");
        g_error = EGL_BAD_ALLOC;
        return NULL;
    }
    mg_nwindow_acquire(window);
    s->window = window;
    s->native = native;
    window_real(s, &s->width, &s->height);
    mg_info("egl: window surface %dx%d on a view's window: its frames are shown where the view is", s->width, s->height);
    return s->handle;
}

static EGLSurface g_eglCreateWindowSurface(EGLDisplay d, EGLConfig config, mg_nwindow *window, const EGLint *attribs)
{
    const megler_run_config *run = mg_run_config();
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
        mg_nwindow_buffer_size(window, &width, &height);
        if (run->window.acquire_target && g_d3d_device) {
            fn_pbuffer_from from = A(fn_pbuffer_from, "eglCreatePbufferFromClientBuffer");
            void *texture;
            if (!g_device_attached) {
                g_device_attached = true;
                if (run->window.attach_device && !shell_attach_device(g_d3d_device)) mg_err("egl: the shell could not make a swap chain on ANGLE's device");
            }
            fn_get_error err = A(fn_get_error, "eglGetError");
            EGLConfig candidates[32];
            int count = surface_configs(d, config, candidates, (int)MG_COUNT(candidates)), k, first_error = 0;
            texture = shell_acquire_target(width, height);
            if (!from) mg_err("egl: this ANGLE has no eglCreatePbufferFromClientBuffer");
            for (k = 0; texture && from && k < count && !h; k++) {
                h = from(d, EGL_D3D_TEXTURE_ANGLE, texture, candidates[k], none);
                if (!h && !first_error && err) first_error = err();
            }
            if (h) {
                from_host = true;
                if (k > 1) mg_info("egl: the shell's texture was taken with the %s of %d configurations tried", k == 2 ? "second" : "last", k);
            } else {
                mg_err("egl: no surface over the shell's %dx%d texture (%d configurations tried, EGL error 0x%x); frames will not be shown", width, height,
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
        mg_nwindow_acquire(window);
        mg_lock_acquire(&g_surfaces_lock);
        s->window = window;
        s->width = width;
        s->height = height;
        s->from_host = from_host;
        s->handle = h;
        mg_lock_release(&g_surfaces_lock);
        mg_info("egl: window surface %dx%d%s", width, height, from_host ? "" : " (off-screen)");
        return h;
    }
}

/* A window's own surface ended: with the window's part of it, and the entry is free again. */
static void window_surface_end(surface *w)
{
    mg_nwindow *window = w->window;
    void *native = w->native;
    free_entry(w);
    window->ops->egl_destroy(native);
    mg_nwindow_release(window);
}

/*
 * What each thread has current, by its id, as ANGLE was given it (turn_bound), and the surfaces and contexts eglDestroySurface
 * and eglDestroyContext were asked to end while a thread had them current. EGL ends such a one once no thread has it
 * current, and ANGLE does so; but a thread's turn (turn_refresh) makes nothing current on it for a moment, which ANGLE took
 * for the thread letting go: it ended the pbuffer or the context there, and the next moment it was made current again,
 * ended. Yandex Maps' map ended in ANGLE so, an access at address 4, on the first turns after a second thread's context
 * (package 528). They are ended here, ANGLE asked only once no thread has them current. A thread that ends with something
 * current keeps its slot, which is only compared, never followed. (Window surfaces have their own: surface_hold.)
 */
#define BINDINGS 64
static struct {
    u32 tid;
    EGLSurface draw, read;
    EGLContext ctx;
} g_bindings[BINDINGS];
static struct {
    EGLDisplay d;
    void *h;
    bool context;
} g_doomed[32];
static volatile i32 g_ndoomed;
static mg_lock g_bindings_lock = MG_LOCK_INIT;

/* Whether a thread has the surface (or the context) current. With g_bindings_lock held. */
static bool bound_anywhere(const void *h, bool context)
{
    int i;
    for (i = 0; i < BINDINGS; i++) {
        if (!g_bindings[i].tid) continue;
        if (context ? g_bindings[i].ctx == h : (g_bindings[i].draw == h || g_bindings[i].read == h)) return true;
    }
    return false;
}

/* What the calling thread has current now (nothing: its slot freed). */
static void binding_set(EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    u32 me = mg_thread_self_id();
    int i, free_at = -1;
    mg_lock_acquire(&g_bindings_lock);
    for (i = 0; i < BINDINGS; i++) {
        if (g_bindings[i].tid == me) break;
        if (!g_bindings[i].tid && free_at < 0) free_at = i;
    }
    if (i == BINDINGS) i = ctx ? free_at : -1;
    if (i >= 0) {
        g_bindings[i].tid = ctx ? me : 0;
        g_bindings[i].draw = ctx ? draw : NULL;
        g_bindings[i].read = ctx ? read : NULL;
        g_bindings[i].ctx = ctx;
    }
    mg_lock_release(&g_bindings_lock);
}

/* A pbuffer or a context to end that a thread has current: put off, true; false to end it now. */
static bool doom(EGLDisplay d, void *h, bool context)
{
    bool later = false;
    int i;
    if (!h) return false;
    mg_lock_acquire(&g_bindings_lock);
    if (bound_anywhere(h, context)) {
        for (i = 0; i < g_ndoomed; i++)
            if (g_doomed[i].h == h && g_doomed[i].context == context) break;
        if (i < g_ndoomed) {
            later = true;
        } else if (g_ndoomed < (i32)MG_COUNT(g_doomed)) {
            g_doomed[g_ndoomed].d = d;
            g_doomed[g_ndoomed].h = h;
            g_doomed[g_ndoomed].context = context;
            g_ndoomed++;
            later = true;
        }
    }
    mg_lock_release(&g_bindings_lock);
    if (later) {
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) < 10)
            mg_info("egl: a %s destroyed while it is current on a thread: ended once no thread has it current, as EGL has it", context ? "context" : "pbuffer");
    }
    return later;
}

/*
 * A window's own surface made current on the calling thread, or no longer: counted, and one that eglDestroySurface was
 * asked to end while it was current somewhere ends when the last thread lets it go. EGL defers it so, and Android's does:
 * HERE WeGo's UI thread destroys its map's surface and makes a new one while its render thread is still drawing a frame in
 * the old one, which it makes current again only after. Ended at once, the surface's texture and EGL's own went from under
 * that frame, and ANGLE ended the app (Framebuffer11::markAttachmentsDirty at address 4, packages 498 and 499).
 */
static void surface_hold(surface *s)
{
    if (!s) return;
    mg_lock_acquire(&g_surfaces_lock);
    s->current++;
    mg_lock_release(&g_surfaces_lock);
}

static void surface_let_go(surface *s)
{
    bool end;
    if (!s) return;
    mg_lock_acquire(&g_surfaces_lock);
    if (s->current > 0) s->current--;
    end = s->current == 0 && s->doomed && s->native;
    mg_lock_release(&g_surfaces_lock);
    if (end) {
        mg_info("egl: a window surface destroyed while it was current is ended now that no thread has it current");
        window_surface_end(s);
    }
}

/* A surface of EGL's own (not a window's) ended: ANGLE's, and the entry the layer kept for it. */
static EGLBoolean surface_end_now(EGLDisplay d, EGLSurface handle)
{
    fn_destroy_surface real = A(fn_destroy_surface, "eglDestroySurface");
    surface *s = surface_of(handle);
    EGLBoolean ok = real ? real(d, handle) : 0;
    if (s) {
        const megler_run_config *run = mg_run_config();
        if (s->from_host && run->window.release_target) shell_release_target();
        if (s->window) mg_nwindow_release(s->window);
        free_entry(s);
    }
    return ok;
}

static EGLBoolean g_eglDestroySurface(EGLDisplay d, EGLSurface handle)
{
    surface *w = window_surface(handle);
    mg_trace("eglDestroySurface(%p)%s", handle, w ? ", a window's" : "");
    if (w) {
        bool later;
        mg_lock_acquire(&g_surfaces_lock);
        later = w->current > 0;
        if (later) w->doomed = true;
        mg_lock_release(&g_surfaces_lock);
        if (later) {
            static volatile i32 said;
            if (mg_atomic_add(&said, 1) < 10) mg_info("egl: a window surface destroyed while it is current on a thread: ended once no thread has it current, as EGL has it");
            return 1;
        }
        window_surface_end(w);
        return 1;
    }
    if (doom(d, handle, false)) return 1;
    return surface_end_now(d, handle);
}

static EGLBoolean g_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx);
static EGLContext turn_own_ctx(void);
static void turn_forget(EGLDisplay d);

/*
 * A context destroyed: put off while another thread has it current (doom). The calling thread's own current one is let
 * go of on it first, and ended now: an app that destroys the context it has current is done with it (Yandex Maps' map
 * probes OpenGL ES 3 so as it starts), and kept current it counted as a second thread with a context for the whole run,
 * which had every frame of its map's thread made current again after the other's turns -- where ANGLE ended it (package
 * 530).
 */
static EGLBoolean g_eglDestroyContext(EGLDisplay d, EGLContext ctx)
{
    fn_destroy_context real = A(fn_destroy_context, "eglDestroyContext");
    mg_trace("eglDestroyContext(%p)%s", ctx, ctx && turn_own_ctx() == ctx ? ", current here" : "");
    if (ctx && turn_own_ctx() == ctx) {
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) < 5) mg_info("egl: a thread destroyed the context it has current: let go of on that thread, and ended");
        g_eglMakeCurrent(d, NULL, NULL, NULL);
    }
    live_remove(ctx);
    if (doom(d, ctx, true)) return 1;
    if (!real || !real(d, ctx)) return 0;
    turn_forget(d);
    return 1;
}

/* After a thread's make-current: what was put off and no thread has current any more, ended now (in the thread's turn). */
static void doomed_sweep(void)
{
    if (!g_ndoomed) return;
    for (;;) {
        EGLDisplay d = NULL;
        void *h = NULL;
        bool context = false, found = false;
        int i;
        mg_lock_acquire(&g_bindings_lock);
        for (i = 0; i < g_ndoomed; i++)
            if (!bound_anywhere(g_doomed[i].h, g_doomed[i].context)) {
                d = g_doomed[i].d;
                h = g_doomed[i].h;
                context = g_doomed[i].context;
                g_doomed[i] = g_doomed[g_ndoomed - 1];
                g_ndoomed--;
                found = true;
                break;
            }
        mg_lock_release(&g_bindings_lock);
        if (!found) return;
        if (context) {
            fn_destroy_context real = A(fn_destroy_context, "eglDestroyContext");
            if (real && real(d, (EGLContext)h)) turn_forget(d);
        } else {
            surface_end_now(d, (EGLSurface)h);
        }
    }
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
/* ------------------------------------------------------------- GL errors
 *
 * The flag trace-gl-errors: what ANGLE finds wrong with an app's OpenGL ES, in the log. Each context an app makes current
 * gets ANGLE's debug output (GL_KHR_debug) with a callback of the layer's, which logs ANGLE's errors, warnings and
 * performance notes -- the first five of each message, then every hundredth with its count -- and a shader that does not
 * compile or a program that does not link is said with its info log (glCompileShader and glLinkProgram go through the
 * layer while the flag is on). A scene drawn only in part is usually ANGLE refusing draws or shaders, of which an app says
 * nothing itself. At most 3000 lines a run.
 */
typedef void (*fn_debug_proc)(u32 source, u32 type, u32 id, u32 severity, i32 length, const char *message, const void *user);
typedef void (*fn_debug_callback)(fn_debug_proc proc, const void *user);
typedef void (*fn_debug_control)(u32 source, u32 type, u32 severity, i32 count, const u32 *ids, u8 enabled);
typedef void (*fn_gl_enum)(u32 cap);
typedef void (*fn_gl_uint)(u32 name);
typedef void (*fn_get_iv)(u32 name, u32 pname, i32 *value);
typedef void (*fn_get_log)(u32 name, i32 size, i32 *length, char *log);

static int g_gl_errors = -1;
static volatile i32 g_gl_error_lines;
static mg_lock g_gl_errors_lock = MG_LOCK_INIT;
static struct {
    u32 hash, count;
} g_gl_seen[512];
static EGLContext g_gl_debugged[64];
static fn_gl_uint h_compile_shader, h_link_program;

static bool gl_errors_wanted(void)
{
    if (g_gl_errors < 0) g_gl_errors = mg_flag("trace-gl-errors") ? 1 : 0;
    return g_gl_errors != 0;
}

static bool gl_error_line_left(void) { return mg_atomic_add(&g_gl_error_lines, 1) <= 3000; }

static void gl_debug_message(u32 source, u32 type, u32 id, u32 severity, i32 length, const char *message, const void *user)
{
    static const char *const k_types[] = { "error", "deprecated", "undefined behaviour", "portability", "performance", "other" };
    u32 hash = 2166136261u, count = 0;
    const char *m;
    int i, n = 0;
    (void)source;
    (void)user;
    if (!message) return;
    for (m = message; *m && (length < 0 || m - message < length); m++) hash = (hash ^ (u8)*m) * 16777619u;
    hash ^= type * 31u + id;
    mg_lock_acquire(&g_gl_errors_lock);
    for (i = 0; i < (int)MG_COUNT(g_gl_seen); i++) {
        u32 at = (hash + (u32)i) & (MG_COUNT(g_gl_seen) - 1);
        if (!g_gl_seen[at].count || g_gl_seen[at].hash == hash) {
            g_gl_seen[at].hash = hash;
            count = ++g_gl_seen[at].count;
            break;
        }
    }
    mg_lock_release(&g_gl_errors_lock);
    if (count > 5 && count % 100) return;
    if (!gl_error_line_left()) return;
    n = length >= 0 && length < 600 ? length : 600;
    mg_info("gl: ANGLE says (%s, severity 0x%x, %u time%s): %.*s", type >= 0x824C && type <= 0x8251 ? k_types[type - 0x824C] : "message", (unsigned)severity,
            (unsigned)count, count == 1 ? "" : "s", n, message);
}

/* The calling thread's context, just made current: ANGLE's debug output to the layer's callback, the first time. */
static void gl_debug_context(EGLContext ctx)
{
    fn_gl_enum enable = A(fn_gl_enum, "glEnable");
    fn_debug_callback set = A(fn_debug_callback, "glDebugMessageCallbackKHR");
    fn_debug_control control = A(fn_debug_control, "glDebugMessageControlKHR");
    size_t i, free_at = MG_COUNT(g_gl_debugged);
    mg_lock_acquire(&g_gl_errors_lock);
    for (i = 0; i < MG_COUNT(g_gl_debugged); i++) {
        if (g_gl_debugged[i] == ctx) {
            mg_lock_release(&g_gl_errors_lock);
            return;
        }
        if (!g_gl_debugged[i] && free_at == MG_COUNT(g_gl_debugged)) free_at = i;
    }
    if (free_at < MG_COUNT(g_gl_debugged)) g_gl_debugged[free_at] = ctx;
    mg_lock_release(&g_gl_errors_lock);
    if (!set) set = A(fn_debug_callback, "glDebugMessageCallback");
    if (!control) control = A(fn_debug_control, "glDebugMessageControl");
    if (!enable || !set) return;
    enable(0x92E0);                                    /* GL_DEBUG_OUTPUT */
    enable(0x8242);                                    /* GL_DEBUG_OUTPUT_SYNCHRONOUS: said on the thread that made the call */
    set(gl_debug_message, NULL);
    if (control) control(0x1100, 0x1100, 0x1100, 0, NULL, 1);    /* GL_DONT_CARE: all of them */
    if (gl_error_line_left()) mg_info("gl: ANGLE's debug output is on for context %p (the flag trace-gl-errors)", (void *)ctx);
}

static void gl_said_log(bool program, u32 name)
{
    fn_get_log get_log = A(fn_get_log, program ? "glGetProgramInfoLog" : "glGetShaderInfoLog");
    fn_get_iv get = A(fn_get_iv, program ? "glGetProgramiv" : "glGetShaderiv");
    char log[1200];
    i32 ok = 1, type = 0, length = 0;
    if (!get) return;
    get(name, program ? 0x8B82 : 0x8B81, &ok);           /* GL_LINK_STATUS, GL_COMPILE_STATUS */
    if (ok) return;
    if (!program) get(name, 0x8B4F, &type);              /* GL_SHADER_TYPE */
    log[0] = 0;
    if (get_log) get_log(name, (i32)sizeof(log) - 1, &length, log);
    log[length > 0 && length < (i32)sizeof(log) ? length : (i32)sizeof(log) - 1] = 0;
    if (gl_error_line_left())
        mg_info("gl: %s %u did not %s: %s", program ? "program" : type == 0x8B31 ? "vertex shader" : type == 0x8B30 ? "fragment shader" : "shader", (unsigned)name,
                program ? "link" : "compile", log[0] ? log : "(no log)");
}

static void w_compile_shader(u32 shader)
{
    h_compile_shader(shader);
    gl_said_log(false, shader);
}

static void w_link_program(u32 program)
{
    h_link_program(program);
    gl_said_log(true, program);
}

/* ---------------------------------------------------------------- turns
 *
 * ANGLE's Direct3D 11 renderer belongs to a display and serves every context made on it with one state manager and its
 * device's one immediate context, and counts on the display's contexts taking turns, each made current again before it
 * draws after another (webview_display above). As gecko-w10m builds it, ANGLE locks a context's share group, not the
 * display: two threads that keep contexts of one display current and call at once run in it at once, and either draws with
 * what the other left bound. HERE WeGo compiles its shaders on one thread while it draws on another, and in Yandex Maps a
 * thread of Google Play services' made a context of its own while the map drew: both ended in glDrawElements, ANGLE marking
 * a framebuffer's attachments through a null pointer (packages 501-503).
 *
 * So every function of EGL's and OpenGL ES's that the app is given (mg_egl_resolve), and that the layer's own Java natives
 * call (mg_egl_host), goes through a thunk that takes its display's turn around the call. Until a thread makes a context of
 * a display current while another thread has one of it current, nothing is taken: one thread drawing, as most apps do,
 * pays the thunk alone. From then on every call of that display's threads holds the display's lock (a thread's calls inside
 * its own nest in its turn), and a call of a thread whose context is not the one ANGLE last had current first makes it
 * current again -- none, then its own --, which is what has ANGLE set its whole state again. The app's display and the
 * WebView's take turns each of their own. The flag no-gl-turns leaves the thunks out.
 *
 * The layer's own code that calls ANGLE on an app's thread takes the turn too (mg_egl_external_upload), and so does a job of
 * a hardware buffers' thread (buffer_job_run); a job for a thread that holds the display's turn and waits for it runs in
 * that turn, lent, as the two never run at once.
 */
typedef struct gl_turn {
    mg_lock lock;
    EGLContext last;                   /* the context ANGLE last had current on this display */
    volatile i32 on;                   /* its calls take turns */
    volatile i32 threads;              /* threads with a context of this display current */
    volatile i32 loose;                /* calls under way that began before turns were on */
    volatile i32 switches;             /* times a context was made current again for its turn */
    u64 switch_ns;
    const char *whose;
} gl_turn;
static gl_turn g_turn_app = { MG_LOCK_INIT, NULL, 0, 0, 0, 0, 0, "the app's" };
static gl_turn g_turn_web = { MG_LOCK_INIT, NULL, 0, 0, 0, 0, 0, "the WebView's" };

#define TURN_DEPTH 64
typedef struct turn_thread {
    int depth;                         /* calls of EGL and OpenGL ES under way on this thread, one inside another */
    gl_turn *held;                     /* the display whose lock the outermost holds */
    gl_turn *loose;                    /* the display whose loose calls the outermost is counted in */
    gl_turn *lent;                     /* a job's: the display whose turn the thread it works for holds */
    struct turn_thread *lender;        /* a job's: that thread */
    EGLDisplay dpy;                    /* what this thread has current, as ANGLE was given it */
    EGLSurface draw, read;
    EGLContext ctx;
    mg_addr lr[TURN_DEPTH];            /* the return addresses of the thunks' callers */
} turn_thread;
static MG_THREAD_LOCAL turn_thread t_turn;

static gl_turn *turn_of(EGLDisplay d) { return d && d == g_webview_display ? &g_turn_web : &g_turn_app; }
static EGLContext turn_own_ctx(void) { return t_turn.ctx; }

/* The calling thread's context made current again for its turn: none, then its own, and ANGLE sets its whole state again. */
static void turn_refresh(turn_thread *t, gl_turn *g)
{
    fn_make_current make = A(fn_make_current, "eglMakeCurrent");
    u64 t0 = mg_time_monotonic_ns();
    i32 n;
    mg_trace("turn: ctx %p (draw %p, read %p) made current again after %p's", t->ctx, t->draw, t->read, g->last);
    if (!make || !make(t->dpy, NULL, NULL, NULL) || !make(t->dpy, t->draw, t->read, t->ctx)) {
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 5) mg_warn("egl: a thread's context could not be made current again for its turn on %s display", g->whose);
    }
    g->last = t->ctx;
    g->switch_ns += mg_time_monotonic_ns() - t0;
    n = mg_atomic_add(&g->switches, 1);
    if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000)
        mg_info("egl: %d time%s a thread's context made current again for its turn on %s display, %.1f ms in all", (int)n, n == 1 ? "" : "s",
                g->whose, (double)g->switch_ns / 1e6);
}

static void turn_begin(void)
{
    turn_thread *t = &t_turn;
    gl_turn *g;
    if (t->depth++) return;
    g = turn_of(t->dpy);
    if (t->lent == g) {
        /* A job for a thread that holds this display's turn and waits for the job: in that turn. */
        if (t->ctx && g->last != t->ctx) turn_refresh(t, g);
        return;
    }
    if (!g->on) {
        mg_atomic_add(&g->loose, 1);
        t->loose = g;
        if (!g->on) return;
        /* Turns came on as this call began: it waits for its turn as the rest do. */
        mg_atomic_add(&g->loose, -1);
        t->loose = NULL;
    }
    mg_lock_acquire(&g->lock);
    t->held = g;
    if (t->ctx && g->last != t->ctx) turn_refresh(t, g);
}

static void turn_end(void)
{
    turn_thread *t = &t_turn;
    if (--t->depth) return;
    if (t->loose) {
        mg_atomic_add(&t->loose->loose, -1);
        t->loose = NULL;
    }
    if (t->held) {
        gl_turn *g = t->held;
        t->held = NULL;
        mg_lock_release(&g->lock);
    }
}

/*
 * Before a make-current of `ctx` on `d` (inside turn_begin): a second thread with a context of the display current turns
 * turns on, once the calls already under way that began before have ended -- not this thread's own, nor the one a job of
 * this thread's works for --, and the make-current itself is in the display's turn.
 */
static void turn_before(EGLDisplay d, EGLContext ctx)
{
    turn_thread *t = &t_turn;
    gl_turn *g = turn_of(d);
    i32 others;
    int waited;
    if (!ctx) return;
    others = g->threads - (t->ctx && turn_of(t->dpy) == g ? 1 : 0);
    if (others > 0 && !mg_atomic_xchg(&g->on, 1)) {
        for (waited = 0; waited < 100; waited++) {
            if (g->loose - (t->loose == g) - (t->lender && t->lender->loose == g) <= 0) break;
            mg_sleep_ns(500000ull);
        }
        mg_info("egl: two threads have contexts of %s display current at once; their calls of EGL and OpenGL ES take turns from now on%s",
                g->whose, waited == 100 ? " (a call under way had not ended after 50 ms)" : "");
    }
    if (g->on && !t->held && t->lent != g) {
        if (t->loose) {
            mg_atomic_add(&t->loose->loose, -1);
            t->loose = NULL;
        }
        mg_lock_acquire(&g->lock);
        t->held = g;
    }
}

/* After a make-current that EGL took: what the calling thread has current now, as ANGLE was given it. */
static void turn_bound(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    turn_thread *t = &t_turn;
    if (t->ctx) mg_atomic_add(&turn_of(t->dpy)->threads, -1);
    t->dpy = ctx ? d : NULL;
    t->draw = ctx ? draw : NULL;
    t->read = ctx ? read : NULL;
    t->ctx = ctx;
    if (ctx) {
        gl_turn *g = turn_of(d);
        mg_atomic_add(&g->threads, 1);
        g->last = ctx;
    }
    binding_set(draw, read, ctx);
    doomed_sweep();
}

/*
 * A context ANGLE has ended: to let go of its objects ANGLE made it current, and then the calling thread's own again -- for a
 * thread with none, no context at all -- so the display's last is no thread's now, and the next call of whichever thread has
 * a context makes its own current again. Google Play services' probe of OpenGL ES made and ended a context every few
 * milliseconds on a thread of its own while the WebView drew Play Store's sign-in, and the WebView's next glDrawArrays read
 * ANGLE's state manager through a null pointer (StateManager11::updateState, package 569; mg_gl_tests.c, test_turns_destroy).
 */
static void turn_forget(EGLDisplay d) { turn_of(d)->last = NULL; }

/* After a lent job: ANGLE last had the job's context current, and this thread goes on in its turn with its own. */
static void turn_back(void)
{
    turn_thread *t = &t_turn;
    if (t->held && t->ctx && t->held->last != t->ctx) turn_refresh(t, t->held);
}

#if MG_GUEST_CODE
/*
 * The thunks: one block, written once, of the bodies and a short entry for each slot, which puts the slot's number where
 * the body looks for it and jumps there. A body keeps the arguments as they came (the registers that carry them; those on
 * the stack are left as the caller put them, the function finding them where it would), takes the turn (turn_enter: it
 * keeps the caller's return address and gives the slot's function), calls the function, gives the turn back (turn_leave:
 * it gives the return address back) and returns the function's result as the function left it.
 *
 * On 32-bit ARM the entry is movw r12, #slot and a jump; the guests' body is for Android's soft-float calls, the layer's own
 * (mg_egl_host, Windows' and Linux's hard-float calls) keeps d0-d7 for the function too, and d0 of its result.
 *
 * On the desktop's processors (docs/DESKTOP.md) the core's convention is Android's, the guests' and the layer's alike, and
 * one body serves both. The function is called with the caller's stack as it was, its return address the body's own: on x86
 * and x86-64 the body puts that in the place of the caller's (turn_enter has kept it) and jumps to the function; on ARM64 it
 * calls it with the caller's stack pointer, as 32-bit ARM's does. x86-64 keeps rdi, rsi, rdx, rcx, r8, r9, rax (the vector
 * count of a variadic call) and xmm0-xmm7 around turn_enter, and rax, rdx, xmm0 and xmm1 of the result around turn_leave,
 * the slot coming in r11 (scratch, no argument's); x86 keeps ecx and edx (the arguments are on the stack), and eax and edx
 * of the result, the slot coming in eax; ARM64 keeps x0-x8 and q0-q7, and x0, x1, q0 and q1 of the result, the slot coming
 * in x16. These turns were ARM's alone before: on the desktop two threads of Chromium's drew in ANGLE's one state manager at
 * once, and the WebView ended in its first page's glDrawArrays, ANGLE marking a framebuffer's attachments through a pointer
 * that was not one (Framebuffer11::markAttachmentsDirty) -- what packages 501-503 did on the phone before the turns.
 */
#define TURN_GUEST 2048
#define TURN_HOST 512
#if MG_HOST_ARM32
#define TURN_ENTRY 12
#define TURN_ENTRIES_AT 128
#else
#define TURN_ENTRY 24
#define TURN_ENTRIES_AT 256
#endif
static void *g_turn_target[TURN_GUEST + TURN_HOST];
static int g_turn_used[2];
static u8 *g_turn_code;
static bool g_turn_tried;
static mg_lock g_turn_slots_lock = MG_LOCK_INIT;

static void *turn_enter(mg_addr lr, u32 slot)
{
    turn_thread *t = &t_turn;
    if (t->depth < TURN_DEPTH) t->lr[t->depth] = lr;
    else mg_err("egl: calls of EGL and OpenGL ES %d deep, one inside another, on one thread: the deepest cannot return", t->depth);
    turn_begin();
    return g_turn_target[slot];
}

static mg_addr turn_leave(void)
{
    turn_thread *t = &t_turn;
    turn_end();
    return t->depth < TURN_DEPTH ? t->lr[t->depth] : 0;
}

#if MG_HOST_ARM32
static bool turn_code(void)
{
    static const u16 guest[] = {
        0xB40F,             /* push {r0-r3} */
        0x4670,             /* mov r0, lr */
        0x4661,             /* mov r1, r12: the slot */
        0xF8DF, 0xC018,     /* ldr.w r12, [pc, #24]: turn_enter */
        0x47E0,             /* blx r12 */
        0x4684,             /* mov r12, r0: the function */
        0xBC0F,             /* pop {r0-r3} */
        0x47E0,             /* blx r12 */
        0xB403,             /* push {r0, r1}: its result */
        0xF8DF, 0xC00C,     /* ldr.w r12, [pc, #12]: turn_leave */
        0x47E0,             /* blx r12 */
        0x4686,             /* mov lr, r0 */
        0xBC03,             /* pop {r0, r1} */
        0x4770,             /* bx lr */
    };
    static const u16 host[] = {
        0xB40F,             /* push {r0-r3} */
        0xED2D, 0x0B10,     /* vpush {d0-d7}: 80 bytes pushed, the stack still a multiple of eight */
        0x4670,             /* mov r0, lr */
        0x4661,             /* mov r1, r12 */
        0xF8DF, 0xC024,     /* ldr.w r12, [pc, #36]: turn_enter */
        0x47E0,             /* blx r12 */
        0x4684,             /* mov r12, r0 */
        0xECBD, 0x0B10,     /* vpop {d0-d7} */
        0xBC0F,             /* pop {r0-r3} */
        0x47E0,             /* blx r12 */
        0xB403,             /* push {r0, r1} */
        0xED2D, 0x0B02,     /* vpush {d0} */
        0xF8DF, 0xC010,     /* ldr.w r12, [pc, #16]: turn_leave */
        0x47E0,             /* blx r12 */
        0x4686,             /* mov lr, r0 */
        0xECBD, 0x0B02,     /* vpop {d0} */
        0xBC03,             /* pop {r0, r1} */
        0x4770,             /* bx lr */
    };
    const size_t size = (TURN_ENTRIES_AT + (TURN_GUEST + TURN_HOST) * TURN_ENTRY + MG_PAGE - 1) & ~(size_t)(MG_PAGE - 1);
    u32 words[2];
    u8 *p = (u8 *)mg_mem_alloc(size);
    int i;
    if (!p) return false;
    words[0] = (u32)(mg_addr)turn_enter;
    words[1] = (u32)(mg_addr)turn_leave;
    memcpy(p, guest, sizeof(guest));
    memcpy(p + sizeof(guest), words, sizeof(words));
    memcpy(p + 64, host, sizeof(host));
    memcpy(p + 64 + sizeof(host), words, sizeof(words));
    for (i = 0; i < TURN_GUEST + TURN_HOST; i++) {
        u8 *e = p + TURN_ENTRIES_AT + (size_t)i * TURN_ENTRY;
        u16 code[4];
        u32 body = (u32)(mg_addr)(p + (i < TURN_GUEST ? 0 : 64)) | 1;
        code[0] = (u16)(0xF240 | ((i >> 12) & 0xF) | (((i >> 11) & 1) << 10));     /* movw r12, #i */
        code[1] = (u16)((((i >> 8) & 7) << 12) | (12 << 8) | (i & 0xFF));
        code[2] = 0xF8DF;                                                            /* ldr.w pc, [pc, #0]: the body */
        code[3] = 0xF000;
        memcpy(e, code, sizeof(code));
        memcpy(e + sizeof(code), &body, sizeof(body));
    }
    if (!mg_mem_protect(p, size, MG_PROT_RX)) return false;
    mg_icache_flush(p, size);
    g_turn_code = p;
    return true;
}
#define TURN_THUMB 1
#else
#define TURN_THUMB 0
/* The body, as clang assembles it (the addresses of turn_enter and turn_leave, and on x86 of `after`, written in after). */
#if MG_ARCH_X86_64
static const u8 k_turn_body[] = {
    0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51, 0x50,           /* push rdi, rsi, rdx, rcx, r8, r9, rax */
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00,                       /* sub rsp, 128 */
    0xF3, 0x0F, 0x7F, 0x04, 0x24,                                   /* movdqu [rsp], xmm0 */
    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x10,                             /* movdqu [rsp + 16], xmm1 ... */
    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x20,
    0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x30,
    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x40,
    0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x50,
    0xF3, 0x0F, 0x7F, 0x74, 0x24, 0x60,
    0xF3, 0x0F, 0x7F, 0x7C, 0x24, 0x70,                             /* ... movdqu [rsp + 112], xmm7 */
    0x48, 0x8B, 0xBC, 0x24, 0xB8, 0x00, 0x00, 0x00,                 /* mov rdi, [rsp + 184]: the caller's return address */
    0x44, 0x89, 0xDE,                                               /* mov esi, r11d: the slot */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                             /* movabs rax, turn_enter (at 0x4c) */
    0xFF, 0xD0,                                                     /* call rax */
    0x49, 0x89, 0xC3,                                               /* mov r11, rax: the function */
    0xF3, 0x0F, 0x6F, 0x04, 0x24,                                   /* movdqu xmm0, [rsp] ... */
    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x10,
    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x20,
    0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x30,
    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x40,
    0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x50,
    0xF3, 0x0F, 0x6F, 0x74, 0x24, 0x60,
    0xF3, 0x0F, 0x6F, 0x7C, 0x24, 0x70,                             /* ... movdqu xmm7, [rsp + 112] */
    0x48, 0x81, 0xC4, 0x80, 0x00, 0x00, 0x00,                       /* add rsp, 128 */
    0x58, 0x41, 0x59, 0x41, 0x58, 0x59, 0x5A, 0x5E, 0x5F,           /* pop rax, r9, r8, rcx, rdx, rsi, rdi */
    0x4C, 0x8D, 0x15, 0x07, 0x00, 0x00, 0x00,                       /* lea r10, [rip + 7]: after */
    0x4C, 0x89, 0x14, 0x24,                                         /* mov [rsp], r10: the function returns there */
    0x41, 0xFF, 0xE3,                                               /* jmp r11 */
    /* after (0xa6): */
    0x50, 0x52,                                                     /* push rax, rdx */
    0x48, 0x83, 0xEC, 0x20,                                         /* sub rsp, 32 */
    0xF3, 0x0F, 0x7F, 0x04, 0x24,                                   /* movdqu [rsp], xmm0 */
    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x10,                             /* movdqu [rsp + 16], xmm1 */
    0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                             /* movabs rax, turn_leave (at 0xb9) */
    0xFF, 0xD0,                                                     /* call rax */
    0x49, 0x89, 0xC3,                                               /* mov r11, rax: the caller's return address */
    0xF3, 0x0F, 0x6F, 0x04, 0x24,                                   /* movdqu xmm0, [rsp] */
    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x10,                             /* movdqu xmm1, [rsp + 16] */
    0x48, 0x83, 0xC4, 0x20,                                         /* add rsp, 32 */
    0x5A, 0x58,                                                     /* pop rdx, rax */
    0x41, 0xFF, 0xE3,                                               /* jmp r11 */
};
enum { TURN_AT_ENTER = 0x4c, TURN_AT_LEAVE = 0xb9, TURN_AT_AFTER = 0, TURN_AFTER = 0 };
#elif MG_ARCH_X86
static const u8 k_turn_body[] = {
    0x51, 0x52,                                                     /* push ecx, edx */
    0x83, 0xEC, 0x0C,                                               /* sub esp, 12: the stack as it was, a multiple of 16 at the call */
    0x50,                                                           /* push eax: the slot */
    0xFF, 0x74, 0x24, 0x18,                                         /* push [esp + 24]: the caller's return address */
    0xB8, 0, 0, 0, 0,                                               /* mov eax, turn_enter (at 0x0b) */
    0xFF, 0xD0,                                                     /* call eax */
    0x83, 0xC4, 0x14,                                               /* add esp, 20 */
    0x5A, 0x59,                                                     /* pop edx, ecx */
    0xC7, 0x04, 0x24, 0, 0, 0, 0,                                   /* mov [esp], after (at 0x19): the function returns there */
    0xFF, 0xE0,                                                     /* jmp eax: the function */
    /* after (0x1f): */
    0x50, 0x52,                                                     /* push eax, edx */
    0x83, 0xEC, 0x08,                                               /* sub esp, 8 */
    0xB9, 0, 0, 0, 0,                                               /* mov ecx, turn_leave (at 0x25) */
    0xFF, 0xD1,                                                     /* call ecx */
    0x83, 0xC4, 0x08,                                               /* add esp, 8 */
    0x89, 0xC1,                                                     /* mov ecx, eax: the caller's return address */
    0x5A, 0x58,                                                     /* pop edx, eax */
    0xFF, 0xE1,                                                     /* jmp ecx */
};
enum { TURN_AT_ENTER = 0x0b, TURN_AT_LEAVE = 0x25, TURN_AT_AFTER = 0x19, TURN_AFTER = 0x1f };
#else
static const u32 k_turn_body[] = {
    0xA9BF7BFD, 0xA9BF07E0, 0xA9BF0FE2, 0xA9BF17E4, 0xA9BF1FE6,     /* stp x29, x30; x0, x1; x2, x3; x4, x5; x6, x7, [sp, #-16]! */
    0xA9BF43E8,                                                     /* stp x8, x16, [sp, #-16]! */
    0xADBF07E0, 0xADBF0FE2, 0xADBF17E4, 0xADBF1FE6,                 /* stp q0, q1; q2, q3; q4, q5; q6, q7, [sp, #-32]! */
    0xAA1E03E0,                                                     /* mov x0, x30: the caller's return address */
    0x2A1003E1,                                                     /* mov w1, w16: the slot */
    0x580002D1,                                                     /* ldr x17, turn_enter (at 0x88) */
    0xD63F0220,                                                     /* blr x17 */
    0xAA0003F1,                                                     /* mov x17, x0: the function */
    0xACC11FE6, 0xACC117E4, 0xACC10FE2, 0xACC107E0,                 /* ldp q6, q7; q4, q5; q2, q3; q0, q1, [sp], #32 */
    0xA8C143E8, 0xA8C11FE6, 0xA8C117E4, 0xA8C10FE2, 0xA8C107E0,     /* ldp x8, x16; x6, x7; x4, x5; x2, x3; x0, x1, [sp], #16 */
    0xA8C17BFD,                                                     /* ldp x29, x30, [sp], #16 */
    0xD63F0220,                                                     /* blr x17: the function, with the caller's stack */
    0xA9BF07E0,                                                     /* stp x0, x1, [sp, #-16]! */
    0xADBF07E0,                                                     /* stp q0, q1, [sp, #-32]! */
    0x58000111,                                                     /* ldr x17, turn_leave (at 0x90) */
    0xD63F0220,                                                     /* blr x17 */
    0xAA0003FE,                                                     /* mov x30, x0: the caller's return address */
    0xACC107E0,                                                     /* ldp q0, q1, [sp], #32 */
    0xA8C107E0,                                                     /* ldp x0, x1, [sp], #16 */
    0xD65F03C0,                                                     /* ret */
    0, 0, 0, 0,                                                     /* (0x88) turn_enter, (0x90) turn_leave */
};
enum { TURN_AT_ENTER = 0x88, TURN_AT_LEAVE = 0x90, TURN_AT_AFTER = 0, TURN_AFTER = 0 };
#endif
_Static_assert(sizeof(k_turn_body) <= TURN_ENTRIES_AT, "the thunks' body fits before their entries");

static bool turn_code(void)
{
    const size_t size = (TURN_ENTRIES_AT + (TURN_GUEST + TURN_HOST) * TURN_ENTRY + MG_PAGE - 1) & ~(size_t)(MG_PAGE - 1);
    u8 *p = (u8 *)mg_mem_alloc(size);
    mg_addr enter = (mg_addr)turn_enter, leave = (mg_addr)turn_leave;
    int i;
    if (!p) return false;
    memcpy(p, k_turn_body, sizeof(k_turn_body));
    memcpy(p + TURN_AT_ENTER, &enter, sizeof(enter));
    memcpy(p + TURN_AT_LEAVE, &leave, sizeof(leave));
    if (TURN_AT_AFTER) {
        mg_addr after = (mg_addr)(p + TURN_AFTER);
        memcpy(p + TURN_AT_AFTER, &after, sizeof(after));
    }
    for (i = 0; i < TURN_GUEST + TURN_HOST; i++) {
        u8 *e = p + TURN_ENTRIES_AT + (size_t)i * TURN_ENTRY;
        memset(e, 0, TURN_ENTRY);
#if MG_ARCH_X86_64
        {
            u64 body = (u64)(mg_addr)p;
            e[0] = 0x41, e[1] = 0xBB;                               /* mov r11d, i */
            memcpy(e + 2, &i, 4);
            e[6] = 0x49, e[7] = 0xBA;                               /* movabs r10, body */
            memcpy(e + 8, &body, 8);
            e[16] = 0x41, e[17] = 0xFF, e[18] = 0xE2;               /* jmp r10 */
            memset(e + 19, 0xCC, TURN_ENTRY - 19);
        }
#elif MG_ARCH_X86
        {
            i32 rel = (i32)((mg_addr)p - (mg_addr)(e + 10));
            e[0] = 0xB8;                                            /* mov eax, i */
            memcpy(e + 1, &i, 4);
            e[5] = 0xE9;                                            /* jmp body */
            memcpy(e + 6, &rel, 4);
            memset(e + 10, 0xCC, TURN_ENTRY - 10);
        }
#else
        {
            u32 code[4];
            u64 body = (u64)(mg_addr)p;
            code[0] = 0xD2800010u | ((u32)i << 5);                  /* movz x16, #i */
            code[1] = 0x58000071u;                                  /* ldr x17, [pc + 12]: the body's address, below */
            code[2] = 0xD61F0220u;                                  /* br x17 */
            code[3] = 0xD503201Fu;                                  /* nop */
            memcpy(e, code, sizeof(code));
            memcpy(e + 16, &body, 8);
        }
#endif
    }
    if (!mg_mem_protect(p, size, MG_PROT_RX)) return false;
    mg_icache_flush(p, size);
    g_turn_code = p;
    return true;
}
#endif

/* The thunk that calls `target` in its display's turn: for a guest's calls, or the layer's own (`host`). */
static void *turn_thunk(void *target, bool host)
{
    static int off = -1;
    int base = host ? TURN_GUEST : 0, n = host ? TURN_HOST : TURN_GUEST, i;
    void *out = target;
    if (off < 0) off = mg_flag("no-gl-turns") ? 1 : 0;
    if (!target || off) return target;
    mg_lock_acquire(&g_turn_slots_lock);
    if (!g_turn_tried) {
        g_turn_tried = true;
        if (!turn_code()) mg_err("egl: no memory for the thunks that have EGL and OpenGL ES calls take turns: they do not");
    }
    if (g_turn_code) {
        for (i = 0; i < g_turn_used[host] && g_turn_target[base + i] != target; i++) {}
        if (i == g_turn_used[host] && i < n) {
            g_turn_target[base + i] = target;
            g_turn_used[host]++;
        }
        if (i < n) out = (void *)((mg_addr)(g_turn_code + TURN_ENTRIES_AT + (size_t)(base + i) * TURN_ENTRY) | TURN_THUMB);
        else mg_note_once("egl: more functions of EGL and OpenGL ES than the thunks that have them take turns; the next ones do not");
    }
    mg_lock_release(&g_turn_slots_lock);
    return out;
}
#else
static void *turn_thunk(void *target, bool host)
{
    (void)host;
    return target;
}
#endif

static EGLBoolean g_eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext ctx)
{
    static volatile i32 said;
    fn_make_current real = A(fn_make_current, "eglMakeCurrent");
    surface *wd = window_surface(draw), *wr = window_surface(read);
    EGLSurface real_draw = wd ? window_real(wd, NULL, NULL) : draw, real_read = wr ? window_real(wr, NULL, NULL) : read;
    EGLBoolean ok;
    if (!real) return 0;
    turn_begin();
    turn_before(d, ctx);
    ok = real(d, real_draw, real_read, ctx);
    mg_trace("eglMakeCurrent(draw %p, read %p, ctx %p) -> %d", real_draw, real_read, ctx, (int)ok);
    if (ok) {
        surface *was_draw = t_draw, *was_read = t_read;
        t_draw = ctx ? wd : NULL;
        t_read = ctx ? wr : NULL;
        surface_hold(t_draw);
        surface_hold(t_read);
        surface_let_go(was_draw);
        surface_let_go(was_read);
        turn_bound(d, real_draw, real_read, ctx);
        if (ctx && gl_errors_wanted()) gl_debug_context(ctx);
    } else {
        refused(wd || wr ? "eglMakeCurrent of a window surface on a view's window" : "eglMakeCurrent", &said);
    }
    turn_end();
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
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 10) mg_warn("egl: eglSwapBuffers of a window surface on a view's window that is not current on this thread: EGL_BAD_SURFACE");
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
        EGLContext ctx = context ? context() : NULL;
        if (!make || !ctx || !make(d, after, read, ctx)) mg_warn("egl: the window's surface of its new size could not be made current");
        else turn_bound(d, after, read, ctx);
    }
    if (!shown) {
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 10)
            mg_info("egl: eglSwapBuffers of a window surface whose view's surface has gone: EGL_BAD_NATIVE_WINDOW, as on Android (the app is to make a new one)");
        g_error = EGL_BAD_NATIVE_WINDOW;
        return 0;
    }
    return 1;
}

static EGLBoolean g_eglSwapBuffers(EGLDisplay d, EGLSurface handle)
{
    surface *w = window_surface(handle), *s;
    mg_trace("eglSwapBuffers(%p)", (void *)handle);
    if (w) return window_swap(d, w);
    s = surface_of(handle);
    if (!s) {
        fn_swap real = A(fn_swap, "eglSwapBuffers");
        return real ? real(d, handle) : 0;
    }
    if (s->from_host) {
        const megler_run_config *run = mg_run_config();
        fn_gl_void flush = A(fn_gl_void, "glFlush");
        if (flush) flush();
        if (run->window.present) run->window.present(run->window.user);
    }
    mg_run_count_frame();
    return 1;
}

static EGLBoolean g_eglSwapInterval(EGLDisplay d, EGLint interval) { (void)d; (void)interval; return 1; }

static void *g_eglGetProcAddress(const char *name) { return mg_egl_resolve(name); }

/* --------------------------------------------------------- hardware buffers
 *
 * Android's EGL makes images of hardware buffers (EGL_ANDROID_image_native_buffer, ndk/mg_ahb.c): what Chromium's GPU
 * service draws a page's canvases and WebGL into. ANGLE has no such buffers, so a buffer's image is an image of a texture
 * of ANGLE's made for it in a context of the layer's own (EGL_KHR_gl_texture_2D_image), on a thread of the layer's own that
 * keeps that context current: G_BUFFERS, and G_BUFFERS_WEBVIEW for the WebView's display, where Chromium's images are
 * made (a buffer's texture is on the display its first image was asked for on, and an image of it on the other is
 * refused). The extensions are named where ANGLE has images of textures; an image of a buffer is asked for with no context
 * (EGL_NO_CONTEXT), which is the spec's.
 */
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define EGL_BAD_PARAMETER 0x300C

typedef void *(*fn_create_image)(EGLDisplay, EGLContext, EGLenum, void *, const EGLint *);

static mg_lock g_ext_lock = MG_LOCK_INIT;
static const char *g_ext_from;         /* ANGLE's string, as it was when g_ext was made of it */
static char *g_ext;
/* Hardware buffers' textures on each of the layer's displays (the app's, the WebView's): the thread, the layer's own context current on it, a framebuffer. */
typedef struct buffers {
    service *thread;
    EGLContext ctx;
    u32 fbo;
} buffers;
static buffers g_buffers[2] = { { &G_BUFFERS, NULL, 0 }, { &G_BUFFERS_WEBVIEW, NULL, 0 } };

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
    mg_lock_acquire(&g_ext_lock);
    if (g_ext_from != angle || !g_ext) {
        char *ext = (char *)mg_alloc(strlen(angle) + sizeof(fences) + sizeof(buffers));
        strcpy(ext, angle);
        if (!has_ext(angle, "EGL_KHR_fence_sync")) strcat(ext, fences);
        if (images) strcat(ext, buffers);
        /* (One made before may still be read by whoever asked for it then: it is kept.) */
        g_ext = ext;
        g_ext_from = angle;
    }
    out = g_ext;
    mg_lock_release(&g_ext_lock);
    return out;
}

/* On a display's G_BUFFERS: the layer's own context there, current from the first job on. */
static bool buffer_context(int which)
{
    static const EGLint es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    fn_create_context create = A(fn_create_context, "eglCreateContext");
    fn_make_current make = A(fn_make_current, "eglMakeCurrent");
    EGLDisplay d = which ? webview_display() : display();
    EGLContext ctx;
    EGLSurface surface = NULL;
    if (g_buffers[which].ctx) return true;
    if (!d || !create || !make) return false;
    ctx = create(d, NULL, NULL, es2);
    if (ctx) turn_before(d, ctx);
    if (!ctx || !make(d, NULL, NULL, ctx)) {
        /* Without contexts that need no configuration, or none current without a surface: a configuration's, over a pbuffer. */
        static const EGLint want[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_NONE };
        static const EGLint one[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        fn_choose_config choose = A(fn_choose_config, "eglChooseConfig");
        fn_pbuffer pbuffer = A(fn_pbuffer, "eglCreatePbufferSurface");
        fn_destroy_context destroy = A(fn_destroy_context, "eglDestroyContext");
        EGLConfig config = NULL;
        EGLint n = 0;
        if (ctx && destroy) destroy(d, ctx);
        ctx = NULL;
        if (choose && pbuffer && choose(d, want, &config, 1, &n) && n > 0) {
            ctx = create(d, config, NULL, es2);
            surface = ctx ? pbuffer(d, config, one) : NULL;
        }
        if (ctx) turn_before(d, ctx);
        if (!ctx || !surface || !make(d, surface, surface, ctx)) {
            mg_err("egl: no context of the layer's own for hardware buffers' textures%s; no image of one can be made", which ? " on the WebView's display" : "");
            return false;
        }
    }
    turn_bound(d, surface, surface, ctx);
    g_buffers[which].ctx = ctx;
    return true;
}

enum { J_IMAGE, J_READ, J_UPLOAD, J_FORGET };
typedef struct buffer_job {
    mg_ahb *b;
    int what;
    void *image;
    bool ok;
    turn_thread *lender;               /* the thread that asked, which waits for it */
} buffer_job;

static void buffer_job_work(buffer_job *j);

/* A job in its display's turn: the turn of the thread that asked, when that one holds it (turn_begin). */
static void buffer_job_run(void *p)
{
    buffer_job *j = (buffer_job *)p;
    turn_thread *t = &t_turn;
    EGLDisplay d = j->b->display ? webview_display() : display();
    if (!t->ctx) t->dpy = d;
    t->lender = j->lender;
    t->lent = j->lender && j->lender->held == turn_of(d) ? j->lender->held : NULL;
    turn_begin();
    buffer_job_work(j);
    turn_end();
    t->lent = NULL;
    t->lender = NULL;
}

static void buffer_job_work(buffer_job *j)
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
    mg_ahb *b = j->b;
    buffers *own = &g_buffers[b->display];
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
    if (!gen || !bind || !param || !storei || !tex_image || !sub_image || !flush || !buffer_context(b->display) || !mg_ahb_gl_format(b, &format, &type)) return;
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
        j->image = create_image(b->display ? g_webview_display : g_display, own->ctx, EGL_GL_TEXTURE_2D_KHR, (void *)(mg_addr)b->texture, preserved);
        flush();
        j->ok = j->image != NULL;
        break;
    case J_READ:
        if (!b->texture || !b->pixels || !read || !bind_fbo || !fbo_texture || (!own->fbo && !gen_fbo)) return;
        if (!own->fbo) gen_fbo(1, &own->fbo);
        bind_fbo(FRAMEBUFFER, own->fbo);
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

static bool buffer_job_do(mg_ahb *b, int what, void **image)
{
    buffer_job j;
    memset(&j, 0, sizeof(j));
    j.b = b;
    j.what = what;
    j.lender = &t_turn;
    run_on(g_buffers[b->display].thread, buffer_job_run, &j);
    turn_back();
    if (image) *image = j.image;
    return j.ok;
}

void *mg_egl_ahb_image(mg_ahb *b, void *d)
{
    void *image = NULL;
    int which = which_display(d);
    mg_lock_acquire(&b->lock);
    if (b->texture && b->display != which) {
        mg_note_once("egl: an image of a hardware buffer was asked for on one of the layer's displays while its texture is on the other: refused");
    } else {
        b->display = (u8)which;
        buffer_job_do(b, J_IMAGE, &image);
    }
    mg_lock_release(&b->lock);
    return image;
}
bool mg_egl_ahb_read(mg_ahb *b) { return buffer_job_do(b, J_READ, NULL); }
bool mg_egl_ahb_upload(mg_ahb *b) { return buffer_job_do(b, J_UPLOAD, NULL); }
void mg_egl_ahb_forget(mg_ahb *b) { buffer_job_do(b, J_FORGET, NULL); }

static void *g_eglGetNativeClientBufferANDROID(const void *buffer)
{
    mg_ahb *b = mg_ahb_of(buffer);
    if (!b) {
        g_error = EGL_BAD_PARAMETER;
        return NULL;
    }
    return (u8 *)b + MG_AHB_ANWB_OFFSET;
}

static void *g_eglCreateImageKHR(EGLDisplay d, EGLContext ctx, EGLenum target, void *buffer, const EGLint *attribs)
{
    fn_create_image real = A(fn_create_image, "eglCreateImageKHR");
    if (target == EGL_NATIVE_BUFFER_ANDROID) {
        mg_ahb *b = mg_ahb_of_client_buffer(buffer);
        void *image;
        if (!ours(d)) {
            g_error = EGL_BAD_DISPLAY;
            return NULL;
        }
        if (!b) {
            g_error = EGL_BAD_PARAMETER;
            return NULL;
        }
        image = mg_egl_ahb_image(b, d);
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
 * named. Here every context of a display puts its commands on that display's one Direct3D 11 device in the order they
 * come (the WebView's display has a device of its own, and Chromium's fences are its own), so a fence needs nothing for
 * the graphics processor to wait on: making one flushes the current context's commands, and the CPU's wait for it
 * (eglClientWaitSyncKHR, or asking its status) is glFinish on the context current then -- which, on that one device,
 * waits for everything put there before. A wait of the graphics processor (eglWaitSyncKHR) is nothing.
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

static mg_lock g_fence_lock = MG_LOCK_INIT;
static fence *g_fences;

static bool is_fence(void *sync)
{
    fence *f;
    mg_lock_acquire(&g_fence_lock);
    for (f = g_fences; f && (void *)f != sync; f = f->next) { }
    mg_lock_release(&g_fence_lock);
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
    if (!ours(d)) {
        g_error = EGL_BAD_DISPLAY;
        return NULL;
    }
    for (i = 0; attribs && attribs[i] != EGL_NONE; i += 2) {
        if (type == EGL_SYNC_NATIVE_FENCE_ANDROID && attribs[i] == EGL_SYNC_NATIVE_FENCE_FD_ANDROID) {
            /* A fence of someone else's to stand for: taken as signalled, and its descriptor is EGL's to close. */
            if (attribs[i + 1] >= 0) mg_fd_close(attribs[i + 1]);
            continue;
        }
        g_error = EGL_BAD_ATTRIBUTE;
        return NULL;
    }
    fence_gl("glFlush");
    f = (fence *)mg_alloc(sizeof(*f));
    f->type = type;
    mg_lock_acquire(&g_fence_lock);
    f->next = g_fences;
    g_fences = f;
    mg_lock_release(&g_fence_lock);
    return f;
}

static EGLBoolean g_eglDestroySyncKHR(EGLDisplay d, void *sync)
{
    fn_destroy_sync real = A(fn_destroy_sync, "eglDestroySyncKHR");
    fence **at;
    mg_lock_acquire(&g_fence_lock);
    for (at = &g_fences; *at && (void *)*at != sync; at = &(*at)->next) { }
    if (*at) {
        fence *f = *at;
        *at = f->next;
        mg_lock_release(&g_fence_lock);
        free(f);
        return 1;
    }
    mg_lock_release(&g_fence_lock);
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
    mg_lock_acquire(&g_fence_lock);
    for (f = g_fences; f && (void *)f != sync; f = f->next) { }
    if (f) type = f->type;
    mg_lock_release(&g_fence_lock);
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
 * 32-bit ARM's (Android's softfp): one adapter per function: it receives each
 * float as the 32 bits the guest put in an integer register and calls ANGLE
 * with real floats. The letters name the parameters in order -- i for anything
 * integer-sized, f for a float. Elsewhere the guest passes floats as floats,
 * as the core does (on x86_64 and x86 every function has an adapter of
 * mg_gl_adapters.c's for the calling convention, floats among the rest).
 */
#if MG_GUEST_SOFTFP
#define AD_f(n) static void (*h_##n)(float); static void w_##n(u32 a) { h_##n(mg_f(a)); }
#define AD_ff(n) static void (*h_##n)(float, float); static void w_##n(u32 a, u32 b) { h_##n(mg_f(a), mg_f(b)); }
#define AD_fff(n) static void (*h_##n)(float, float, float); static void w_##n(u32 a, u32 b, u32 c) { h_##n(mg_f(a), mg_f(b), mg_f(c)); }
#define AD_ffff(n) static void (*h_##n)(float, float, float, float); static void w_##n(u32 a, u32 b, u32 c, u32 d) { h_##n(mg_f(a), mg_f(b), mg_f(c), mg_f(d)); }
#define AD_fffff(n) static void (*h_##n)(float, float, float, float, float); static void w_##n(u32 a, u32 b, u32 c, u32 d, u32 e) { h_##n(mg_f(a), mg_f(b), mg_f(c), mg_f(d), mg_f(e)); }
#define AD_ffffff(n) static void (*h_##n)(float, float, float, float, float, float); static void w_##n(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) { h_##n(mg_f(a), mg_f(b), mg_f(c), mg_f(d), mg_f(e), mg_f(f)); }
#define AD_fi(n) static void (*h_##n)(float, int); static void w_##n(u32 a, int b) { h_##n(mg_f(a), b); }
#define AD_if(n) static void (*h_##n)(int, float); static void w_##n(int a, u32 b) { h_##n(a, mg_f(b)); }
#define AD_iff(n) static void (*h_##n)(int, float, float); static void w_##n(int a, u32 b, u32 c) { h_##n(a, mg_f(b), mg_f(c)); }
#define AD_ifff(n) static void (*h_##n)(int, float, float, float); static void w_##n(int a, u32 b, u32 c, u32 d) { h_##n(a, mg_f(b), mg_f(c), mg_f(d)); }
#define AD_iffff(n) static void (*h_##n)(int, float, float, float, float); static void w_##n(int a, u32 b, u32 c, u32 d, u32 e) { h_##n(a, mg_f(b), mg_f(c), mg_f(d), mg_f(e)); }
#define AD_iif(n) static void (*h_##n)(int, int, float); static void w_##n(int a, int b, u32 c) { h_##n(a, b, mg_f(c)); }
#define AD_iiff(n) static void (*h_##n)(int, int, float, float); static void w_##n(int a, int b, u32 c, u32 d) { h_##n(a, b, mg_f(c), mg_f(d)); }
#define AD_iifff(n) static void (*h_##n)(int, int, float, float, float); static void w_##n(int a, int b, u32 c, u32 d, u32 e) { h_##n(a, b, mg_f(c), mg_f(d), mg_f(e)); }
#define AD_iiffff(n) static void (*h_##n)(int, int, float, float, float, float); static void w_##n(int a, int b, u32 c, u32 d, u32 e, u32 f) { h_##n(a, b, mg_f(c), mg_f(d), mg_f(e), mg_f(f)); }
#define AD_iifi(n) static void (*h_##n)(int, int, float, int); static void w_##n(int a, int b, u32 c, int d) { h_##n(a, b, mg_f(c), d); }

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
#endif

static const mg_sym g_egl_overrides[] = {
    MG_WRAP("eglGetError", g_eglGetError),
    MG_WRAP("eglGetDisplay", g_eglGetDisplay),
    MG_WRAP("eglInitialize", g_eglInitialize),
    MG_WRAP("eglQueryString", g_eglQueryString),
    MG_WRAP("eglGetNativeClientBufferANDROID", g_eglGetNativeClientBufferANDROID),
    MG_WRAP("eglCreateImageKHR", g_eglCreateImageKHR),
    MG_WRAP("eglCreateSyncKHR", g_eglCreateSyncKHR),
    MG_WRAP("eglDestroySyncKHR", g_eglDestroySyncKHR),
    MG_WRAP("eglClientWaitSyncKHR", g_eglClientWaitSyncKHR),
    MG_WRAP("eglGetSyncAttribKHR", g_eglGetSyncAttribKHR),
    MG_WRAP("eglWaitSyncKHR", g_eglWaitSyncKHR),
    MG_WRAP("eglDupNativeFenceFDANDROID", g_eglDupNativeFenceFDANDROID),
    MG_WRAP("eglTerminate", g_eglTerminate),
    MG_WRAP("eglChooseConfig", g_eglChooseConfig),
    MG_WRAP("eglGetConfigAttrib", g_eglGetConfigAttrib),
    MG_WRAP("eglCreateWindowSurface", g_eglCreateWindowSurface),
    MG_WRAP("eglCreatePbufferSurface", g_eglCreatePbufferSurface),
    MG_WRAP("eglCreateContext", g_eglCreateContext),
    MG_WRAP("eglDestroySurface", g_eglDestroySurface),
    MG_WRAP("eglDestroyContext", g_eglDestroyContext),
    MG_WRAP("eglQuerySurface", g_eglQuerySurface),
    MG_WRAP("eglSwapBuffers", g_eglSwapBuffers),
    MG_WRAP("eglMakeCurrent", g_eglMakeCurrent),
    MG_WRAP("eglGetCurrentSurface", g_eglGetCurrentSurface),
    MG_WRAP("eglSurfaceAttrib", g_eglSurfaceAttrib),
    MG_WRAP("eglSwapInterval", g_eglSwapInterval),
    MG_WRAP("eglGetProcAddress", g_eglGetProcAddress),
    MG_END
};

bool mg_egl_is_gl_name(const char *name)
{
    return (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z') || (name[0] == 'e' && name[1] == 'g' && name[2] == 'l' && name[3] >= 'A' && name[3] <= 'Z');
}

static void *resolve(const char *name)
{
    static bool told;
    size_t i;
    if (!mg_egl_is_gl_name(name)) return NULL;
    if (!told) {
        told = true;
        mg_gltex_set_host(angle);
    }
    if (name[0] == 'e') {
        for (i = 0; g_egl_overrides[i].name; i++)
            if (!strcmp(g_egl_overrides[i].name, name)) return g_egl_overrides[i].addr;
        return angle(name);
    }
    if (gl_errors_wanted() && !strcmp(name, "glCompileShader")) {
        if (!h_compile_shader) h_compile_shader = (fn_gl_uint)angle(name);
        return h_compile_shader ? (void *)w_compile_shader : NULL;
    }
    if (gl_errors_wanted() && !strcmp(name, "glLinkProgram")) {
        if (!h_link_program) h_link_program = (fn_gl_uint)angle(name);
        return h_link_program ? (void *)w_link_program : NULL;
    }
#if MG_GUEST_SOFTFP
    for (i = 0; i < MG_COUNT(g_adapters); i++) {
        if (strcmp(g_adapters[i].name, name)) continue;
        if (!*g_adapters[i].host) *g_adapters[i].host = angle(name);
        /* ANGLE may not have it (an OpenGL ES 1 function): then the guest gets "no such function", not an adapter to nothing. */
        return *g_adapters[i].host ? g_adapters[i].wrapper : NULL;
    }
#endif
    return mg_gltex_wrap(name, angle(name));
}

/* ------------------------------------------------- tracing the app's GL calls
 *
 * The flag "trace-gl": every EGL and GLES function the app is given goes
 * through a few generated instructions that put its name in the trace
 * (mg_base.c) before going on to the function. Which call came last before
 * something went wrong is then in the crash report. Each of these thunks has
 * a page of its own: a page is written and then made executable for good, and
 * one that other threads are already running through is never touched again.
 */
#if MG_HOST_ARM32
static struct {
    const char *name;
    void *thunk;
    u8 kind;                           /* which arguments are worth writing down: see gl_called */
} g_traced[1024];
static int g_traced_count;
static u8 *g_thunk_block;
static u32 g_thunk_block_used;
static mg_lock g_thunk_lock = MG_LOCK_INIT;

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
    if (!removed_said && mg_host_d3d11_removed_reason(g_d3d_device) && !mg_atomic_xchg(&removed_said, 1)) {
        mg_trace("the Direct3D device is gone (0x%x), found before gl %s", (unsigned)mg_host_d3d11_removed_reason(g_d3d_device), name);
        mg_err("egl: the Direct3D device has been removed (0x%x); it was found gone before %s", (unsigned)mg_host_d3d11_removed_reason(g_d3d_device), name);
    }
    switch (g_traced[id].kind) {
    case 1:         /* glTexStorage2D(target, levels, format, width, height) */
        mg_host_gpu_memory(g_d3d_device, &budget, &used);
        mg_trace("gl %s levels %u format 0x%x %ux%u; graphics memory %u of %u MB", name, saved[1], saved[2], saved[3], saved[6], used, budget);
        break;
    case 2:         /* glTexImage2D / glCompressedTexImage2D(target, level, format, width, height, ...) */
        mg_host_gpu_memory(g_d3d_device, &budget, &used);
        mg_trace("gl %s level %u format 0x%x %ux%u; graphics memory %u of %u MB", name, saved[1], saved[2], saved[3], saved[6], used, budget);
        break;
    case 3:         /* glCompressedTexSubImage2D(target, level, x, y, width, height, format, size, data) */
        mg_trace("gl %s level %u %ux%u format 0x%x, %u bytes", name, saved[1], saved[6], saved[7], saved[8], saved[9]);
        break;
    case 4:         /* glRenderbufferStorage(target, format, width, height) */
        mg_trace("gl %s format 0x%x %ux%u", name, saved[1], saved[2], saved[3]);
        break;
    case 5:         /* glRenderbufferStorageMultisample(target, samples, format, width, height) */
        mg_trace("gl %s x%u format 0x%x %ux%u", name, saved[1], saved[2], saved[3], saved[6]);
        break;
    case 6:         /* glBufferData(target, size, data, usage) */
        mg_trace("gl %s %u bytes", name, saved[1]);
        break;
    /*
     * What a framebuffer is made of and drawn to, with the names in it: which one is bound, what is attached where, which
     * draw buffers are on, what is deleted. A draw that ANGLE ends on (Framebuffer11::markAttachmentsDirty, Yandex Maps and
     * HERE WeGo, packages 498 to 501) is read against them.
     */
    case 7:         /* glBindFramebuffer(target, framebuffer) */
        mg_trace("gl %s 0x%x %u", name, saved[0], saved[1]);
        break;
    case 8:         /* glFramebufferTexture2D(target, attachment, textarget, texture, level) */
        mg_trace("gl %s 0x%x attachment 0x%x textarget 0x%x texture %u level %u", name, saved[0], saved[1], saved[2], saved[3], saved[6]);
        break;
    case 9:         /* glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer) */
        mg_trace("gl %s 0x%x attachment 0x%x renderbuffer %u", name, saved[0], saved[1], saved[3]);
        break;
    case 10: {      /* glDrawBuffers(n, bufs); glDeleteTextures, glDeleteFramebuffers, glDeleteRenderbuffers(n, names) */
        const u32 *v = (const u32 *)(mg_addr)saved[1];
        u32 n = saved[0];
        if (!v || n == 0) mg_trace("gl %s %u", name, n);
        else if (n == 1) mg_trace("gl %s 1: 0x%x", name, v[0]);
        else if (n == 2) mg_trace("gl %s 2: 0x%x 0x%x", name, v[0], v[1]);
        else if (n == 3) mg_trace("gl %s 3: 0x%x 0x%x 0x%x", name, v[0], v[1], v[2]);
        else mg_trace("gl %s %u: 0x%x 0x%x 0x%x 0x%x ...", name, n, v[0], v[1], v[2], v[3]);
        break;
    }
    case 11: {      /* glInvalidateFramebuffer(target, n, attachments), glDiscardFramebufferEXT */
        const u32 *v = (const u32 *)(mg_addr)saved[2];
        if (!v || saved[1] == 0) mg_trace("gl %s 0x%x %u", name, saved[0], saved[1]);
        else if (saved[1] == 1) mg_trace("gl %s 0x%x 1: 0x%x", name, saved[0], v[0]);
        else mg_trace("gl %s 0x%x %u: 0x%x 0x%x ...", name, saved[0], saved[1], v[0], v[1]);
        break;
    }
    case 12:        /* glFramebufferTextureLayer(target, attachment, texture, level, layer) */
        mg_trace("gl %s 0x%x attachment 0x%x texture %u level %u layer %u", name, saved[0], saved[1], saved[2], saved[3], saved[6]);
        break;
    case 13:        /* glBindRenderbuffer(target, renderbuffer); glReadBuffer(src) */
        mg_trace("gl %s 0x%x %u", name, saved[0], saved[1]);
        break;
    default:
        mg_trace("gl %s", name);
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
    if (!strcmp(name, "glBindFramebuffer")) return 7;
    if (!strcmp(name, "glFramebufferTexture2D")) return 8;
    if (!strcmp(name, "glFramebufferRenderbuffer")) return 9;
    if (!strcmp(name, "glDrawBuffers") || !strcmp(name, "glDrawBuffersEXT") || !strcmp(name, "glDeleteTextures") || !strcmp(name, "glDeleteFramebuffers")
        || !strcmp(name, "glDeleteRenderbuffers"))
        return 10;
    if (!strcmp(name, "glInvalidateFramebuffer") || !strcmp(name, "glDiscardFramebufferEXT")) return 11;
    if (!strcmp(name, "glFramebufferTextureLayer")) return 12;
    if (!strcmp(name, "glBindRenderbuffer") || !strcmp(name, "glReadBuffer")) return 13;
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
    mg_lock_acquire(&g_thunk_lock);
    for (i = 0; i < g_traced_count; i++) {
        if (!strcmp(g_traced[i].name, name)) {
            mg_lock_release(&g_thunk_lock);
            return g_traced[i].thunk;
        }
    }
    if (g_traced_count == (int)MG_COUNT(g_traced)) {
        mg_lock_release(&g_thunk_lock);
        return target;
    }
    if (!g_thunk_block || g_thunk_block_used + MG_PAGE > 65536) {
        g_thunk_block = (u8 *)mg_mem_alloc(65536);
        g_thunk_block_used = 0;
    }
    if (!g_thunk_block) {
        mg_lock_release(&g_thunk_lock);
        return target;
    }
    p = g_thunk_block + g_thunk_block_used;
    g_thunk_block_used += MG_PAGE;
    words[0] = (u32)g_traced_count;
    words[1] = (u32)(mg_addr)gl_called;
    words[2] = (u32)(mg_addr)target;
    memcpy(p, code, sizeof(code));
    memcpy(p + sizeof(code), words, sizeof(words));
    if (!mg_mem_protect(p, MG_PAGE, MG_PROT_RX)) {
        mg_lock_release(&g_thunk_lock);
        return target;
    }
    mg_icache_flush(p, MG_PAGE);
    g_traced[g_traced_count].name = mg_strdup(name);
    g_traced[g_traced_count].kind = gl_kind(name);
    g_traced[g_traced_count].thunk = (void *)((mg_addr)p | 1);
    g_traced_count++;
    mg_lock_release(&g_thunk_lock);
    return (void *)((mg_addr)p | 1);
}
#else
static void *traced(const char *name, void *target)
{
    (void)name;
    return target;
}
#endif

static void *resolve(const char *name);

void *mg_egl_resolve(const char *name)
{
    static int tracing = -1;
    void *fn = resolve(name);
    if (tracing < 0) tracing = mg_flag("trace-gl") ? 1 : 0;
    return turn_thunk(tracing && fn ? traced(name, fn) : fn, false);
}

void *mg_egl_host(const char *name)
{
    static bool told;
    if (!mg_egl_is_gl_name(name)) return NULL;
    /* The EGL functions above take no floats: the same ones guests get. */
    if (name[0] == 'e') return turn_thunk(resolve(name), true);
    if (!told) {
        told = true;
        mg_gltex_set_host(angle);
    }
    return turn_thunk(mg_gltex_wrap(name, angle(name)), true);
}

u32 mg_egl_device_removed(void) { return mg_host_d3d11_removed_reason(g_d3d_device); }

bool mg_egl_gpu_memory(u32 *used_mb, u32 *budget_mb) { return g_d3d_device && mg_host_gpu_memory(g_d3d_device, budget_mb, used_mb); }

/* ------------------------------------------------------ external textures */

static bool external_upload(u32 external, u32 *helper, void **image, int *helper_w, int *helper_h, const u8 *rgba, int w, int h)
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
    typedef EGLDisplay (*fn_current_display)(void);
    typedef u32 (*fn_error)(void);
    fn_current_display current_display = A(fn_current_display, "eglGetCurrentDisplay");
    EGLDisplay d = current_display && current_display() ? current_display() : g_display;
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
        if (*image && destroy_image) destroy_image(d, *image);
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
        *image = create_image(d, current(), GL_TEXTURE_2D_KHR, (void *)(mg_addr)*helper, none);
        if (*image) {
            get(BINDING_EXTERNAL, &was_external);
            bind(TEXTURE_EXTERNAL, external);
            target(TEXTURE_EXTERNAL, *image);
            bind(TEXTURE_EXTERNAL, (u32)was_external);
        } else {
            mg_warn("egl: no image of a %dx%d texture for a film (EGL error 0x%x); the film stays black", w, h,
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
            mg_note_once("egl: putting a film's frame on a texture gave OpenGL error 0x%x", (unsigned)e);
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

bool mg_egl_external_upload(u32 external, u32 *helper, void **image, int *helper_w, int *helper_h, const u8 *rgba, int w, int h)
{
    bool ok;
    turn_begin();
    ok = external_upload(external, helper, image, helper_w, helper_h, rgba, w, h);
    turn_end();
    return ok;
}

/* ------------------------------------------------------------------ probe */

void mg_egl_probe(void (*line)(const char *fmt, ...))
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
