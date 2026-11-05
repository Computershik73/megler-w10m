/*
 * OpenGL ES and EGL from an app's Java (core/src/drivers/mg_java_gl.c, mg_java_gles.c), called as the runtime calls them:
 * an mg_jcall with one value for each parameter of the Java declaration, arrays, strings and buffers made by the runtime.
 * The OpenGL ES is the stand-in that draws (mg_gl_standin.c), given to the core where ANGLE would be, so what a call drew
 * can be read back; the shell is this file, which keeps every picture it is handed.
 *
 * What is checked is the Java side's own work: that every native the framework declares is bound, values and floats
 * reaching the function, arrays and their offsets, java.nio buffers of each kind and their positions, strings both ways,
 * Android's exceptions; contexts and surfaces of megler.EGLNative; and the window composed of the views' picture and a
 * surface's frame under them -- a hole, a half-transparent row, the frame upside down as OpenGL has it, read back as
 * B, G, R, A and as R, G, B, A, scaled, moved, gone -- with whatever pack buffer and framebuffer the app left bound. And
 * what a swap costs at a phone's size, on this machine, with the stand-in's read-back (a copy of memory, which ANGLE's is
 * not: that one waits for the GPU).
 */
#include <stdio.h>

#include "../src/drivers/mg_java.h"
#include "../src/drivers/mg_java_gl.h"
#include "../src/dvm/mg_dvm.h"
#include "../src/dvm/mg_dvm_internal.h"
#include "../src/bionic/mg_bionic.h"
#include "../src/host/mg_host.h"
#include "../src/ndk/mg_egl.h"
#include "mg_gfx_tests.h"
#include "mg_gl_standin.h"

const char *megler_data_dir(void);

static int t_checks, t_failed;
static mg_test_sink t_sink;
static void *t_sink_user;

static void say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (t_sink) t_sink(line, t_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define CHECK(cond)                                                    \
    do {                                                               \
        t_checks++;                                                    \
        if (!(cond)) {                                                 \
            t_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

#define CHECK_HEX(expr, want)                                                                        \
    do {                                                                                             \
        u32 got_ = (u32)(expr), want_ = (u32)(want);                                                 \
        t_checks++;                                                                                  \
        if (got_ != want_) {                                                                         \
            t_failed++;                                                                              \
            say("  FAIL %s:%d: %s = %08x, expected %08x", __FILE__, __LINE__, #expr, got_, want_);   \
        }                                                                                            \
    } while (0)

/* --------------------------------------------------------- calling a native */

static mg_jvalue I(i32 v)
{
    mg_jvalue x;
    x.j = 0;
    x.i = v;
    return x;
}

static mg_jvalue J(i64 v)
{
    mg_jvalue x;
    x.j = v;
    return x;
}

static mg_jvalue F(float v)
{
    mg_jvalue x;
    x.j = 0;
    x.f = v;
    return x;
}

static mg_jvalue Z(bool v)
{
    mg_jvalue x;
    x.j = 0;
    x.z = v;
    return x;
}

static mg_jvalue L(mg_jobj *v)
{
    mg_jvalue x;
    x.j = 0;
    x.l = v;
    return x;
}

static const mg_jhle *find_native(const char *cls, const char *name, const char *sig)
{
    const mg_jhle *tables[2], *e;
    int t;
    tables[0] = mg_java_gl_natives();
    tables[1] = mg_java_gles_natives();
    for (t = 0; t < 2; t++)
        for (e = tables[t]; e->cls; e++)
            if (!strcmp(e->cls, cls) && !strcmp(e->name, name) && (!e->sig || !strcmp(e->sig, sig))) return e;
    return NULL;
}

/* A native by its class ("GLES20", "EGLNative"), name and signature, with `count` values for its parameters, in the order of the Java declaration. */
static mg_jvalue call(const char *cls, const char *name, const char *sig, int count, ...)
{
    char full[80];
    const mg_jhle *e;
    mg_jvalue args[16];
    mg_jcall c;
    va_list ap;
    int i;
    memset(&c, 0, sizeof(c));
    memset(args, 0, sizeof(args));
    va_start(ap, count);
    for (i = 0; i < count && i < 16; i++) args[i] = va_arg(ap, mg_jvalue);
    va_end(ap);
    snprintf(full, sizeof(full), "%s/%s", !strcmp(cls, "EGLNative") ? "megler" : "android/opengl", cls);
    e = find_native(full, name, sig);
    if (!e) {
        t_failed++;
        say("  FAIL: no native %s.%s%s", full, name, sig);
        return c.ret;
    }
    c.name = e->name;
    c.sig = sig;
    c.args = args;
    e->fn(&c);
    return c.ret;
}

#define GL20(name, sig, ...) call("GLES20", name, sig, __VA_ARGS__)
#define EGL(name, sig, ...) call("EGLNative", name, sig, __VA_ARGS__)

static mg_jobj *ints(int n)
{
    mg_jobj *a = mg_dvm_new_array("[I", n);
    return a;
}
static i32 *ints_of(mg_jobj *a) { return (i32 *)a->u.arr.data; }

static const char *thrown(void)
{
    static char text[300];
    text[0] = 0;
    if (mg_dvm_pending()) {
        mg_dvm_describe_pending(text, sizeof(text));
        mg_dvm_clear_pending();
    }
    return text;
}

static const char *text_of(mg_jobj *s)
{
    const char *t = s ? mg_dvm_string_utf8(s) : NULL;
    return t ? t : "(null)";
}

/* ------------------------------------------------------------- the shell */

/* What the window shows: every picture handed over, kept, and the part each said had changed. */
static struct {
    u8 *pixels;
    int width, height;
    int pictures;
    i32 part[4];
} W;

static void shown(const void *pixels, int width, int height, int stride, const i32 part[4])
{
    int r;
    if (W.width != width || W.height != height) {
        free(W.pixels);
        W.pixels = (u8 *)calloc((size_t)width * (size_t)height, 4);
        W.width = width;
        W.height = height;
    }
    /* As a shell that keeps its last picture: the part alone is copied. */
    for (r = part[1]; r < part[3]; r++)
        memcpy(W.pixels + ((size_t)r * (size_t)width + (size_t)part[0]) * 4, (const u8 *)pixels + (size_t)r * (size_t)stride + (size_t)part[0] * 4,
               (size_t)(part[2] - part[0]) * 4);
    memcpy(W.part, part, sizeof(W.part));
    W.pictures++;
}

/* A pixel of the window as 0xRRGGBB (the shell takes the picture as opaque). */
static u32 window_pixel(int x, int y)
{
    const u8 *p = W.pixels + ((size_t)y * (size_t)W.width + (size_t)x) * 4;
    return (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}

/* A picture of the views: B, G, R, A premultiplied, rows top first; opaque grey, with a hole and a row of black at half alpha in it. */
static u8 *views_picture(int width, int height, int hx, int hy, int hw, int hh, int dim_row)
{
    u8 *p = (u8 *)malloc((size_t)width * (size_t)height * 4);
    int x, y;
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            u8 *q = p + ((size_t)y * (size_t)width + (size_t)x) * 4;
            bool hole = x >= hx && x < hx + hw && y >= hy && y < hy + hh;
            if (hole && y == dim_row) {
                q[0] = q[1] = q[2] = 0;
                q[3] = 128;
            } else if (hole) {
                q[0] = q[1] = q[2] = q[3] = 0;
            } else {
                q[0] = q[1] = q[2] = 0x80;
                q[3] = 255;
            }
        }
    return p;
}

static const i32 k_all[4] = { 0, 0, 0, 0 };

/* --------------------------------------------------------------- the tests */

/* Every GLES native the framework declares has a function, the generated one's or one by hand. */
static void test_bound(void)
{
    const mg_jhle *d;
    int declared = 0, bound = 0, generated = 0, by_hand = 0;
    say("OpenGL ES from Java: the GLES classes' natives bound");
    for (d = mg_java_gles_declared(); d->cls; d++) {
        const mg_jhle *e = find_native(d->cls, d->name, d->sig);
        declared++;
        if (e) bound++;
        else say("  not bound: %s.%s%s", d->cls, d->name, d->sig);
    }
    for (d = mg_java_gles_natives(); d->cls; d++) generated++;
    for (d = mg_java_gl_natives(); d->cls; d++)
        if (!strncmp(d->cls, "android/opengl/", 15)) by_hand++;
    say("  %d declared by the framework's GLES10 to GLES32, %d bound: %d generated (tools/gles-natives.py), %d by hand", declared, bound, generated, by_hand);
    CHECK(declared == 832);
    CHECK(bound == declared);
    CHECK(generated + by_hand == declared);
}

/*
 * The EGL version an app is told: the tests' stand-ins for ANGLE say EGL 1.5 and name no fence syncs (EGL_KHR_fence_sync),
 * as ANGLE over Direct3D 11 does on the phone; EGL 1.5 has them, so the app is told 1.4 (mg_egl.c, tell_version).
 */
static void test_version(void)
{
    void *(*get_display)(void *) = (void *(*)(void *))mg_egl_resolve("eglGetDisplay");
    unsigned (*init)(void *, int *, int *) = (unsigned (*)(void *, int *, int *))mg_egl_resolve("eglInitialize");
    const char *(*query)(void *, int) = (const char *(*)(void *, int))mg_egl_resolve("eglQueryString");
    void *d;
    int major = 0, minor = 0;
    const char *version;
    say("EGL: the version an app is told, of an EGL 1.5 without fence syncs");
    CHECK(get_display && init && query);
    if (!get_display || !init || !query) return;
    d = get_display(NULL);
    CHECK(d && init(d, &major, &minor));
    version = query(d, 0x3054);
    say("  EGL %d.%d, \"%s\"", major, minor, version ? version : "(none)");
    CHECK(major == 1 && minor == 4);
    CHECK(version && !strncmp(version, "1.4", 3));
}

/*
 * Hardware buffers (ndk/mg_ahb.c) and EGL images of them (mg_egl.c), as Chromium's GPU service and the ANGLE inside it use
 * them, over the stand-in, whose textures keep their pixels and whose EGL images share them: a 4x2 R8G8B8A8 buffer laid out
 * where ANGLE reads it (the ANativeWindowBuffer two pointers in, laid out with the guest's pointers: 96 bytes in 32-bit
 * Android, 168 in 64-bit), written by the CPU, made an EGL image of with no context, a texture of the test's own made of
 * that image and read through a framebuffer object (the CPU's pixels, rows in the buffer's order), cleared green by the
 * graphics processor and locked for reading (green); formats refused; one sent through a socket of the process and
 * received.
 */
static void test_hardware_buffers(void)
{
    typedef struct { u32 width, height, layers, format; u64 usage; u32 stride, rfu0; u64 rfu1; } desc_t;
    /* Android's ANativeWindowBuffer as ANGLE declares it (android_util.cpp). */
    typedef struct {
        i32 magic, version;
        void *reserved[4];
        void *inc_ref, *dec_ref;
        i32 width, height, stride, format, usage_deprecated;
        uintptr_t layer_count;
        void *reserved1;
        const void *handle;
        u64 usage;
        void *reserved_proc[8 - sizeof(u64) / sizeof(void *)];
    } anwb_t;
    typedef int (*alloc_fn)(const desc_t *, void **);
    typedef void (*ref_fn)(void *);
    typedef void (*describe_fn)(const void *, desc_t *);
    typedef int (*supported_fn)(const desc_t *);
    typedef int (*lock_fn)(void *, u64, i32, const void *, void **);
    typedef int (*unlock_fn)(void *, i32 *);
    typedef int (*send_fn)(const void *, int);
    typedef int (*recv_fn)(int, void **);
    typedef int (*socketpair_fn)(int, int, int, int *);
    typedef int (*close_fn)(int);
    alloc_fn allocate = (alloc_fn)mg_bionic_find("AHardwareBuffer_allocate");
    ref_fn release = (ref_fn)mg_bionic_find("AHardwareBuffer_release");
    describe_fn describe = (describe_fn)mg_bionic_find("AHardwareBuffer_describe");
    supported_fn supported = (supported_fn)mg_bionic_find("AHardwareBuffer_isSupported");
    lock_fn lock = (lock_fn)mg_bionic_find("AHardwareBuffer_lock");
    unlock_fn unlock = (unlock_fn)mg_bionic_find("AHardwareBuffer_unlock");
    send_fn send_handle = (send_fn)mg_bionic_find("AHardwareBuffer_sendHandleToUnixSocket");
    recv_fn recv_handle = (recv_fn)mg_bionic_find("AHardwareBuffer_recvHandleFromUnixSocket");
    socketpair_fn socketpair_ = (socketpair_fn)mg_bionic_find("socketpair");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    void *(*get_display)(void *) = (void *(*)(void *))mg_egl_resolve("eglGetDisplay");
    const char *(*query)(void *, int) = (const char *(*)(void *, int))mg_egl_resolve("eglQueryString");
    void *(*client_of)(const void *) = (void *(*)(const void *))mg_egl_resolve("eglGetNativeClientBufferANDROID");
    void *(*create_image)(void *, void *, unsigned, void *, const int *) =
        (void *(*)(void *, void *, unsigned, void *, const int *))mg_egl_resolve("eglCreateImageKHR");
    unsigned (*destroy_image)(void *, void *) = (unsigned (*)(void *, void *))mg_egl_resolve("eglDestroyImageKHR");
    /* The test's own context and texture: the stand-in's own functions, as an app's GL would be its driver's. */
    void *(MG_GLAPI *create_ctx)(void *, void *, void *, const int *) = (void *(MG_GLAPI *)(void *, void *, void *, const int *))mg_gl_standin("eglCreateContext");
    unsigned (MG_GLAPI *make_current)(void *, void *, void *, void *) = (unsigned (MG_GLAPI *)(void *, void *, void *, void *))mg_gl_standin("eglMakeCurrent");
    unsigned (MG_GLAPI *destroy_ctx)(void *, void *) = (unsigned (MG_GLAPI *)(void *, void *))mg_gl_standin("eglDestroyContext");
    void (MG_GLAPI *gen_textures)(i32, u32 *) = (void (MG_GLAPI *)(i32, u32 *))mg_gl_standin("glGenTextures");
    void (MG_GLAPI *delete_textures)(i32, const u32 *) = (void (MG_GLAPI *)(i32, const u32 *))mg_gl_standin("glDeleteTextures");
    void (MG_GLAPI *bind_texture)(u32, u32) = (void (MG_GLAPI *)(u32, u32))mg_gl_standin("glBindTexture");
    void (MG_GLAPI *target_image)(u32, void *) = (void (MG_GLAPI *)(u32, void *))mg_gl_standin("glEGLImageTargetTexture2DOES");
    void (MG_GLAPI *gen_fbos)(i32, u32 *) = (void (MG_GLAPI *)(i32, u32 *))mg_gl_standin("glGenFramebuffers");
    void (MG_GLAPI *bind_fbo)(u32, u32) = (void (MG_GLAPI *)(u32, u32))mg_gl_standin("glBindFramebuffer");
    void (MG_GLAPI *fbo_texture)(u32, u32, u32, u32, i32) = (void (MG_GLAPI *)(u32, u32, u32, u32, i32))mg_gl_standin("glFramebufferTexture2D");
    void (MG_GLAPI *read_pixels)(i32, i32, i32, i32, u32, u32, void *) = (void (MG_GLAPI *)(i32, i32, i32, i32, u32, u32, void *))mg_gl_standin("glReadPixels");
    void (MG_GLAPI *clear_color)(float, float, float, float) = (void (MG_GLAPI *)(float, float, float, float))mg_gl_standin("glClearColor");
    void (MG_GLAPI *clear)(u32) = (void (MG_GLAPI *)(u32))mg_gl_standin("glClear");
    static const int es2[] = { 0x3098, 2, 0x3038 };
    static const int preserved[] = { 0x30D2, 1, 0x3038 };
    desc_t want = { 4, 2, 1, 1, 0x333, 0, 0, 0 }, got, fp16 = { 4, 2, 1, 0x16, 0x300, 0, 0, 0 };
    void *b = NULL, *again = NULL, *client, *image, *ctx, *d, *bad = (void *)1;
    u8 *px = NULL, read[32];
    const anwb_t *anwb;
    u32 tex = 0, fbo = 0;
    int i, sv[2] = { -1, -1 }, fence = 0;
    bool same = true, green = true;
    say("Hardware buffers and EGL images of them (AHardwareBuffer, EGL_ANDROID_image_native_buffer)");
    CHECK(allocate && release && describe && supported && lock && unlock && send_handle && recv_handle && client_of && create_image && destroy_image);
    if (!allocate || !release || !describe || !supported || !lock || !unlock || !send_handle || !recv_handle || !client_of || !create_image ||
        !destroy_image || !get_display || !query || !socketpair_ || !close_)
        return;
    d = get_display(NULL);
    CHECK(query(d, 0x3055) && strstr(query(d, 0x3055), "EGL_ANDROID_image_native_buffer") &&
          strstr(query(d, 0x3055), "EGL_ANDROID_get_native_client_buffer"));
    CHECK(supported(&want) == 1);
    CHECK(supported(&fp16) == 0);
    CHECK(allocate(&fp16, &bad) == -22 && !bad);
    CHECK(allocate(&want, &b) == 0 && b);
    if (!b) return;
    describe(b, &got);
    CHECK(got.width == 4 && got.height == 2 && got.layers == 1 && got.format == 1 && got.usage == 0x333 && got.stride == 4);
    /* Where ANGLE looks: the ANativeWindowBuffer at two pointers in, its magic, size, width, height, stride, format, layer count, usage. */
    anwb = (const anwb_t *)((const u8 *)b + 2 * sizeof(void *));
    CHECK_HEX((u32)anwb->magic, 0x5f626672);
    CHECK(anwb->version == (i32)sizeof(anwb_t) && sizeof(anwb_t) == (sizeof(void *) == 8 ? 168 : 96));
    CHECK(anwb->width == 4);
    CHECK(anwb->height == 2);
    CHECK(anwb->stride == 4);
    CHECK(anwb->format == 1);
    CHECK(anwb->layer_count == 1);
    CHECK_HEX((u32)anwb->usage, 0x333);
    /* The CPU writes: pixel i is (i * 30, 255 - i * 30, i, 255). */
    CHECK(lock(b, 0x30, -1, NULL, (void **)&px) == 0 && px);
    if (!px) return;
    for (i = 0; i < 8; i++) {
        px[i * 4] = (u8)(i * 30);
        px[i * 4 + 1] = (u8)(255 - i * 30);
        px[i * 4 + 2] = (u8)i;
        px[i * 4 + 3] = 255;
    }
    CHECK(unlock(b, &fence) == 0 && fence == -1);
    client = client_of(b);
    CHECK(client == (void *)anwb);
    image = create_image(d, NULL, 0x3140, client, preserved);
    CHECK(image != NULL);
    CHECK(create_image(d, NULL, 0x3140, (void *)read, preserved) == NULL);
    if (!image) {
        release(b);
        return;
    }
    /* A texture of the test's own made of the image, read through a framebuffer object: what the CPU wrote. */
    ctx = create_ctx(d, NULL, NULL, es2);
    CHECK(ctx && make_current(d, NULL, NULL, ctx));
    gen_textures(1, &tex);
    bind_texture(0x0DE1, tex);
    target_image(0x0DE1, image);
    gen_fbos(1, &fbo);
    bind_fbo(0x8D40, fbo);
    fbo_texture(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
    memset(read, 0, sizeof(read));
    read_pixels(0, 0, 4, 2, 0x1908, 0x1401, read);
    for (i = 0; i < 32; i++)
        if (read[i] != px[i]) same = false;
    CHECK(same);
    /* The graphics processor draws into it; a lock for reading sees that. */
    clear_color(0, 1, 0, 1);
    clear(0x4000);
    CHECK(lock(b, 0x3, -1, NULL, (void **)&px) == 0 && px);
    for (i = 0; px && i < 8; i++)
        if (px[i * 4] != 0 || px[i * 4 + 1] != 255 || px[i * 4 + 2] != 0 || px[i * 4 + 3] != 255) green = false;
    CHECK(green);
    CHECK(unlock(b, NULL) == 0);
    say("  4x2 R8G8B8A8: written %s, drawn into %s", same ? "and read through an EGL image" : "and NOT read back", green ? "and read by the CPU" : "and NOT read back");
    bind_fbo(0x8D40, 0);
    delete_textures(1, &tex);
    make_current(d, NULL, NULL, NULL);
    destroy_ctx(d, ctx);
    CHECK(destroy_image(d, image));
    /* Through a socket of the process: the same buffer, with a reference for the receiver. */
    CHECK(socketpair_(1, 1, 0, sv) == 0);
    CHECK(send_handle(b, sv[0]) == 0);
    CHECK(recv_handle(sv[1], &again) == 0 && again == b);
    close_(sv[0]);
    close_(sv[1]);
    if (again) release(again);
    release(b);
    CHECK(mg_bionic_find("AHardwareBuffer_getId") != NULL);
}

/*
 * The WebView's display (mg_egl.c, webview_display): Chromium's code is given a display of its own, so that its GPU thread
 * and an app's drawing threads never have contexts of one ANGLE display current at once. Asked for by the library a call
 * comes from: libwebviewchromium.so's gets the WebView's, any other the app's. It is initialised and told the same as the
 * app's -- version, the layer's extensions -- and an image of a hardware buffer asked for on it is made there, by the
 * WebView's own thread and context; the same buffer is refused on the app's display while its texture is on the WebView's.
 */
static void test_webview_display(void)
{
    typedef struct { u32 width, height, layers, format; u64 usage; u32 stride, rfu0; u64 rfu1; } desc_t;
    typedef int (*alloc_fn)(const desc_t *, void **);
    typedef void (*ref_fn)(void *);
    alloc_fn allocate = (alloc_fn)mg_bionic_find("AHardwareBuffer_allocate");
    ref_fn release = (ref_fn)mg_bionic_find("AHardwareBuffer_release");
    void *(*get_display)(void *) = (void *(*)(void *))mg_egl_resolve("eglGetDisplay");
    unsigned (*initialize)(void *, int *, int *) = (unsigned (*)(void *, int *, int *))mg_egl_resolve("eglInitialize");
    const char *(*query)(void *, int) = (const char *(*)(void *, int))mg_egl_resolve("eglQueryString");
    void *(*client_of)(const void *) = (void *(*)(const void *))mg_egl_resolve("eglGetNativeClientBufferANDROID");
    void *(*create_image)(void *, void *, unsigned, void *, const int *) =
        (void *(*)(void *, void *, unsigned, void *, const int *))mg_egl_resolve("eglCreateImageKHR");
    unsigned (*destroy_image)(void *, void *) = (unsigned (*)(void *, void *))mg_egl_resolve("eglDestroyImageKHR");
    desc_t want = { 4, 2, 1, 1, 0x333, 0, 0, 0 };
    void *app, *web, *b = NULL, *image;
    const char *app_version, *web_version, *ext;
    int major = 0, minor = 0;
    say("The WebView's display: one of its own for Chromium's code, beside the app's");
    CHECK(allocate && release && get_display && initialize && query && client_of && create_image && destroy_image);
    if (!allocate || !release || !get_display || !initialize || !query || !client_of || !create_image || !destroy_image) return;
    app = get_display(NULL);
    /* By default Chromium's code shares the app's display; with webview-own-display it has one of its own. */
    mg_egl_webview_own_display(false);
    CHECK(mg_egl_display_for_library("libwebviewchromium.so") == app);
    mg_egl_webview_own_display(true);
    web = mg_egl_webview_display();
    CHECK(app && web && web != app);
    CHECK(mg_egl_webview_display() == web);
    CHECK(mg_egl_display_for_library("libwebviewchromium.so") == web);
    CHECK(mg_egl_display_for_library("/system/app/webview/webview.apk!/lib/armeabi-v7a/libwebviewchromium.so") == web);
    CHECK(mg_egl_display_for_library("libAsphalt9.so") == app && mg_egl_display_for_library(NULL) == app);
    /* The test's own call comes from no guest library: the app's display, as before. */
    CHECK(get_display(NULL) == app);
    CHECK(initialize(web, &major, &minor) && major == 1);
    app_version = query(app, 0x3054);
    web_version = query(web, 0x3054);
    CHECK(app_version && web_version && !strcmp(app_version, web_version));
    ext = query(web, 0x3055);
    CHECK(ext && strstr(ext, "EGL_ANDROID_image_native_buffer") && strstr(ext, "EGL_KHR_fence_sync"));
    CHECK(allocate(&want, &b) == 0 && b);
    if (!b) return;
    image = create_image(web, NULL, 0x3140, client_of(b), NULL);
    CHECK(image && mg_gl_standin_image_display() == web);
    CHECK(!create_image(app, NULL, 0x3140, client_of(b), NULL));
    if (image) destroy_image(web, image);
    release(b);
}

/*
 * Fence syncs (mg_egl.c), which the ANGLE inside Chromium makes through the system's EGL once a canvas is in a hardware
 * buffer: the three extensions named; a fence made with a context current, of type fence, signalled, its condition
 * prior commands; a wait for it satisfied, a server wait done; a native fence made over a descriptor of someone else's,
 * which is closed, signalled, and gives none out; both destroyed; one made with an attribute it does not take refused.
 */
static void test_fence_syncs(void)
{
    typedef void *(*create_fn)(void *, unsigned, const int *);
    typedef unsigned (*destroy_fn)(void *, void *);
    typedef int (*client_wait_fn)(void *, void *, int, u64);
    typedef unsigned (*attrib_fn)(void *, void *, int, int *);
    typedef int (*wait_fn)(void *, void *, int);
    typedef int (*dup_fn)(void *, void *);
    typedef int (*pipe_fn)(int *);
    typedef int (*close_fn)(int);
    create_fn create = (create_fn)mg_egl_resolve("eglCreateSyncKHR");
    destroy_fn destroy = (destroy_fn)mg_egl_resolve("eglDestroySyncKHR");
    client_wait_fn client_wait = (client_wait_fn)mg_egl_resolve("eglClientWaitSyncKHR");
    attrib_fn attrib = (attrib_fn)mg_egl_resolve("eglGetSyncAttribKHR");
    wait_fn wait = (wait_fn)mg_egl_resolve("eglWaitSyncKHR");
    dup_fn dup_fd = (dup_fn)mg_egl_resolve("eglDupNativeFenceFDANDROID");
    void *(*get_display)(void *) = (void *(*)(void *))mg_egl_resolve("eglGetDisplay");
    const char *(*query)(void *, int) = (const char *(*)(void *, int))mg_egl_resolve("eglQueryString");
    pipe_fn pipe_ = (pipe_fn)mg_bionic_find("pipe");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    void *(MG_GLAPI *create_ctx)(void *, void *, void *, const int *) = (void *(MG_GLAPI *)(void *, void *, void *, const int *))mg_gl_standin("eglCreateContext");
    unsigned (MG_GLAPI *make_current)(void *, void *, void *, void *) = (unsigned (MG_GLAPI *)(void *, void *, void *, void *))mg_gl_standin("eglMakeCurrent");
    unsigned (MG_GLAPI *destroy_ctx)(void *, void *) = (unsigned (MG_GLAPI *)(void *, void *))mg_gl_standin("eglDestroyContext");
    static const int es2[] = { 0x3098, 2, 0x3038 }, none[] = { 0x3038 }, bad[] = { 0x3145, 3, 0x3038 };
    int fds[2] = { -1, -1 }, native_attribs[3], v = 0;
    void *d, *ctx, *f, *n;
    const char *ext;
    say("Fence syncs (EGL_KHR_fence_sync, EGL_KHR_wait_sync, EGL_ANDROID_native_fence_sync)");
    CHECK(create && destroy && client_wait && attrib && wait && dup_fd && pipe_ && close_);
    if (!create || !destroy || !client_wait || !attrib || !wait || !dup_fd || !get_display || !query || !pipe_ || !close_) return;
    d = get_display(NULL);
    ext = query(d, 0x3055);
    CHECK(ext && strstr(ext, "EGL_KHR_fence_sync") && strstr(ext, "EGL_KHR_wait_sync") && strstr(ext, "EGL_ANDROID_native_fence_sync"));
    ctx = create_ctx(d, NULL, NULL, es2);
    CHECK(ctx && make_current(d, NULL, NULL, ctx));
    f = create(d, 0x30F9, none);
    CHECK(f != NULL);
    CHECK(attrib(d, f, 0x30F7, &v) && v == 0x30F9);
    CHECK(attrib(d, f, 0x30F1, &v) && v == 0x30F2);
    CHECK(attrib(d, f, 0x30F8, &v) && v == 0x30F0);
    CHECK(client_wait(d, f, 1, 1000000000ull) == 0x30F6);
    CHECK(wait(d, f, 0) == 1);
    CHECK(create(d, 0x30F9, bad) == NULL);
    /* A native fence over a descriptor of someone else's: the descriptor is EGL's to close. */
    CHECK(pipe_(fds) == 0);
    native_attribs[0] = 0x3145;
    native_attribs[1] = fds[0];
    native_attribs[2] = 0x3038;
    n = create(d, 0x3144, native_attribs);
    CHECK(n != NULL);
    CHECK(close_(fds[0]) == -1);
    close_(fds[1]);
    CHECK(attrib(d, n, 0x30F8, &v) && v == 0x3146);
    CHECK(dup_fd(d, n) == -1);
    CHECK(destroy(d, f) && destroy(d, n));
    make_current(d, NULL, NULL, NULL);
    destroy_ctx(d, ctx);
}

static void test_calls(void)
{
    mg_jobj *a, *s;
    i64 x, surf;
    i32 shader, program;
    mg_jvalue r;
    static const char vertex[] = "attribute vec4 aPosition;\nuniform vec4 uColor;\nvoid main() { gl_Position = aPosition; }\n";
    say("OpenGL ES from Java: contexts, surfaces, and GLES20's natives with values, arrays and strings");
    x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    surf = EGL("createSurface", "(II)J", 2, I(64), I(32)).j;
    CHECK(x && surf);
    CHECK(EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x)).z);
    /* Strings out. */
    CHECK(!strcmp(text_of(GL20("glGetString", "(I)Ljava/lang/String;", 1, I(0x1F00)).l), "Megler"));
    CHECK(!strcmp(text_of(GL20("glGetString", "(I)Ljava/lang/String;", 1, I(0x1F02)).l), "OpenGL ES 2.0 (stand-in)"));
    CHECK(!strcmp(text_of(call("GLES10", "glGetString", "(I)Ljava/lang/String;", 1, I(0x1F01)).l), "stand-in for OpenGL ES"));
    /* Floats in as floats, through the function itself (no adapter): what the stand-in received is read back as a pixel. */
    GL20("glClearColor", "(FFFF)V", 4, F(0.25f), F(0.5f), F(0.75f), F(1.0f));
    GL20("glClear", "(I)V", 1, I(0x4000));
    CHECK_HEX(mg_gl_standin_pixel(3, 3), 0x4080BFFF);
    /* An array and its offset: the element at the offset is the first written, the one before it is left. */
    a = ints(3);
    ints_of(a)[0] = 7;
    GL20("glGetIntegerv", "(I[II)V", 3, I(0x0D33), L(a), I(1));
    CHECK(ints_of(a)[0] == 7 && ints_of(a)[1] == 4096);
    GL20("glGenTextures", "(I[II)V", 3, I(2), L(a), I(1));
    CHECK(ints_of(a)[0] == 7 && ints_of(a)[1] && ints_of(a)[2] && ints_of(a)[1] != ints_of(a)[2]);
    CHECK(GL20("glIsTexture", "(I)Z", 1, I(ints_of(a)[2])).z);
    /* Android's exceptions for a null array and a bad offset; nothing is called. */
    GL20("glGetIntegerv", "(I[II)V", 3, I(0x0D33), L(NULL), I(0));
    CHECK(strstr(thrown(), "IllegalArgumentException: array == null") != NULL);
    GL20("glGetIntegerv", "(I[II)V", 3, I(0x0D33), L(a), I(-1));
    CHECK(strstr(thrown(), "IllegalArgumentException: offset < 0") != NULL);
    GL20("glGetIntegerv", "(I[II)V", 3, I(0x0D33), L(a), I(4));
    CHECK(strstr(thrown(), "IllegalArgumentException") != NULL);
    /* A shader: its source in as a String, its log and its source out as Strings. */
    shader = GL20("glCreateShader", "(I)I", 1, I(0x8B31)).i;
    CHECK(shader > 0);
    s = mg_dvm_new_string(vertex);
    GL20("glShaderSource", "(ILjava/lang/String;)V", 2, I(shader), L(s));
    GL20("glCompileShader", "(I)V", 1, I(shader));
    GL20("glGetShaderiv", "(II[II)V", 4, I(shader), I(0x8B81), L(a), I(0));
    CHECK(ints_of(a)[0] == 1);
    CHECK(strstr(text_of(GL20("glGetShaderInfoLog", "(I)Ljava/lang/String;", 1, I(shader)).l), "compiled") != NULL);
    CHECK(!strcmp(text_of(GL20("glGetShaderSource", "(I)Ljava/lang/String;", 1, I(shader)).l), vertex));
    GL20("glShaderSource", "(ILjava/lang/String;)V", 2, I(shader), L(NULL));
    CHECK(strstr(thrown(), "IllegalArgumentException") != NULL);
    program = GL20("glCreateProgram", "()I", 0).i;
    GL20("glAttachShader", "(II)V", 2, I(program), I(shader));
    GL20("glLinkProgram", "(I)V", 1, I(program));
    GL20("glGetProgramiv", "(II[II)V", 4, I(program), I(0x8B82), L(a), I(2));
    CHECK(ints_of(a)[2] == 1);
    CHECK(strstr(text_of(GL20("glGetProgramInfoLog", "(I)Ljava/lang/String;", 1, I(program)).l), "linked") != NULL);
    /* Names in as Strings; the active attribute's name out as a String, its size and type into arrays at their offsets. */
    CHECK(GL20("glGetAttribLocation", "(ILjava/lang/String;)I", 2, I(program), L(mg_dvm_new_string("aPosition"))).i == 0);
    CHECK(GL20("glGetUniformLocation", "(ILjava/lang/String;)I", 2, I(program), L(mg_dvm_new_string("uColor"))).i == 0);
    CHECK(GL20("glGetUniformLocation", "(ILjava/lang/String;)I", 2, I(program), L(mg_dvm_new_string("nothing"))).i == -1);
    {
        mg_jobj *size = ints(2), *type = ints(3);
        r = GL20("glGetActiveAttrib", "(II[II[II)Ljava/lang/String;", 6, I(program), I(0), L(size), I(1), L(type), I(2));
        CHECK(!strcmp(text_of(r.l), "aPosition") && ints_of(size)[1] == 1 && ints_of(type)[2] == 0x8B52);
        r = GL20("glGetActiveUniform", "(II[II[II)Ljava/lang/String;", 6, I(program), I(0), L(size), I(0), L(type), I(0));
        CHECK(!strcmp(text_of(r.l), "uColor") && ints_of(type)[0] == 0x8B52);
    }
    /* The generated form with a byte[] for the name. */
    {
        mg_jobj *length = ints(1), *size = ints(1), *type = ints(1), *name = mg_dvm_new_array("[B", 32);
        GL20("glGetActiveAttrib", "(III[II[II[II[BI)V", 11, I(program), I(0), I(30), L(length), I(0), L(size), I(0), L(type), I(0), L(name), I(2));
        CHECK(ints_of(length)[0] == 9 && !memcmp((const char *)name->u.arr.data + 2, "aPosition", 10));
    }
    /* A function this OpenGL ES does not have does nothing and says so once. */
    GL20("glSampleCoverage", "(FZ)V", 2, F(0.5f), Z(true));
    CHECK(!mg_dvm_pending());
    CHECK(EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0)).z);
    EGL("destroySurface", "(J)V", 1, J(surf));
    EGL("destroyContext", "(J)V", 1, J(x));
}

/* java.nio: a direct buffer of floats from a ByteBuffer, with a position; a buffer over an array; one of neither kind. Only on libcore. */
static mg_jobj *jcall_static(const char *cls, const char *name, const char *sig, mg_jvalue arg)
{
    mg_jvalue r;
    r.j = 0;
    if (!mg_dvm_call_static(cls, name, sig, &arg, &r)) return NULL;
    return r.l;
}

static mg_jobj *jcall(mg_jobj *o, const char *name, const char *sig, mg_jvalue arg)
{
    mg_jvalue r;
    r.j = 0;
    if (!o || !mg_dvm_call_virtual(o, name, sig, &arg, &r)) return NULL;
    return r.l;
}

/*
 * A direct FloatBuffer as an app makes one (ByteBuffer.allocateDirect, the native order, asFloatBuffer), with the floats
 * written into its memory from here: FloatBuffer.put on a direct buffer is libcore's native code, which a PC does not run.
 */
static mg_jobj *direct_floats(const float *values, int n)
{
    mg_jobj *bb = jcall_static("java/nio/ByteBuffer", "allocateDirect", "(I)Ljava/nio/ByteBuffer;", I(n * 4)), *order, *fb;
    mg_jvalue none, address;
    none.j = address.j = 0;
    order = jcall_static("java/nio/ByteOrder", "nativeOrder", "()Ljava/nio/ByteOrder;", none);
    bb = jcall(bb, "order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;", L(order));
    fb = jcall(bb, "asFloatBuffer", "()Ljava/nio/FloatBuffer;", none);
    if (!fb || !mg_dvm_get_field(bb, "address", &address) || !address.j) {
        say("  (no direct buffer: %s)", thrown());
        return NULL;
    }
    memcpy((void *)(mg_addr)address.j, values, (size_t)n * 4);
    return fb;
}

static void set_position(mg_jobj *buffer, int position)
{
    if (!jcall(buffer, "position", "(I)Ljava/nio/Buffer;", I(position))) {
        mg_dvm_clear_pending();
        jcall(buffer, "position", "(I)Ljava/nio/FloatBuffer;", I(position));
    }
}

static void test_buffers(void)
{
    /* A triangle over the left half of the surface, and the same one a vertex further on in a longer buffer. */
    static const float tri[6] = { -1, -1, 0, -1, -1, 1 };
    static const float later[8] = { 9, 9, -1, -1, 0, -1, -1, 1 };
    mg_jobj *fb, *bytes, *ib, *heap, *wrong, *a;
    mg_jvalue none;
    i64 x, surf;
    i32 shader, program;
    u8 *p;
    say("OpenGL ES from Java: java.nio buffers");
    if (!mg_dvm_class_exists("java/nio/ByteBuffer")) {
        say("  (not on the built-in java.lang: there is no java.nio)");
        return;
    }
    none.j = 0;
    x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    surf = EGL("createSurface", "(II)J", 2, I(16), I(16)).j;
    CHECK(x && surf && EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x)).z);
    shader = GL20("glCreateShader", "(I)I", 1, I(0x8B31)).i;
    GL20("glShaderSource", "(ILjava/lang/String;)V", 2, I(shader), L(mg_dvm_new_string("attribute vec2 aPosition; uniform vec4 uColor;")));
    program = GL20("glCreateProgram", "()I", 0).i;
    GL20("glAttachShader", "(II)V", 2, I(program), I(shader));
    GL20("glLinkProgram", "(I)V", 1, I(program));
    GL20("glUseProgram", "(I)V", 1, I(program));
    GL20("glUniform4f", "(IFFFF)V", 5, I(0), F(1), F(1), F(0), F(1));
    GL20("glClearColor", "(FFFF)V", 4, F(0), F(0), F(1), F(1));
    GL20("glClear", "(I)V", 1, I(0x4000));
    /* A direct FloatBuffer made from a ByteBuffer in the native order: its address. */
    fb = direct_floats(tri, 6);
    CHECK(fb != NULL);
    thrown();
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(fb));
    GL20("glEnableVertexAttribArray", "(I)V", 1, I(0));
    GL20("glDrawArrays", "(III)V", 3, I(4), I(0), I(3));
    CHECK_HEX(mg_gl_standin_pixel(2, 2), 0xFFFF00FF);
    CHECK_HEX(mg_gl_standin_pixel(14, 14), 0x0000FFFF);
    /* Its position counts, in elements of its own kind: two floats on is one vertex on. */
    GL20("glClear", "(I)V", 1, I(0x4000));
    fb = direct_floats(later, 8);
    set_position(fb, 2);
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(fb));
    GL20("glDrawArrays", "(III)V", 3, I(4), I(0), I(3));
    CHECK_HEX(mg_gl_standin_pixel(2, 2), 0xFFFF00FF);
    CHECK(!mg_dvm_pending());
    /* A FloatBuffer over an array (FloatBuffer.wrap): the array's own memory. */
    GL20("glClear", "(I)V", 1, I(0x4000));
    a = mg_dvm_new_array("[F", 6);
    memcpy(a->u.arr.data, tri, sizeof(tri));
    heap = jcall_static("java/nio/FloatBuffer", "wrap", "([F)Ljava/nio/FloatBuffer;", L(a));
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(heap));
    GL20("glDrawArrays", "(III)V", 3, I(4), I(0), I(3));
    CHECK(heap && mg_gl_standin_pixel(2, 2) == 0xFFFF00FF);
    /* Pixels read back into a direct ByteBuffer, R, G, B, A, the bottom row first. */
    bytes = jcall_static("java/nio/ByteBuffer", "allocateDirect", "(I)Ljava/nio/ByteBuffer;", I(16 * 16 * 4));
    CHECK(bytes != NULL);
    GL20("glReadPixels", "(IIIIIILjava/nio/Buffer;)V", 7, I(0), I(0), I(16), I(16), I(0x1908), I(0x1401), L(bytes));
    {
        mg_jvalue address;
        address.j = 0;
        mg_dvm_get_field(bytes, "address", &address);
        p = (u8 *)(mg_addr)address.j;
        CHECK(p && p[0] == 255 && p[1] == 255 && p[2] == 0 && p[(15 * 16 + 15) * 4 + 2] == 255 && p[(15 * 16 + 15) * 4 + 0] == 0);
    }
    /* No buffer where C would write: Android's exception, and nothing written to nowhere. */
    GL20("glReadPixels", "(IIIIIILjava/nio/Buffer;)V", 7, I(0), I(0), I(16), I(16), I(0x1908), I(0x1401), L(NULL));
    CHECK(strstr(thrown(), "IllegalArgumentException") != NULL);
    /* An IntBuffer over an array for a value out, with a position. */
    a = ints(4);
    ib = jcall_static("java/nio/IntBuffer", "wrap", "([I)Ljava/nio/IntBuffer;", L(a));
    jcall(ib, "position", "(I)Ljava/nio/Buffer;", I(3));
    thrown();
    GL20("glGetIntegerv", "(ILjava/nio/IntBuffer;)V", 2, I(0x0D33), L(ib));
    CHECK(ints_of(a)[3] == 4096 && ints_of(a)[0] == 0);
    /* The forms with buffers for the active attribute's size and type. */
    {
        mg_jobj *size = jcall_static("java/nio/IntBuffer", "wrap", "([I)Ljava/nio/IntBuffer;", L(ints(1))), *type = jcall_static("java/nio/IntBuffer", "wrap", "([I)Ljava/nio/IntBuffer;", L(a));
        mg_jvalue r = GL20("glGetActiveAttrib", "(IILjava/nio/IntBuffer;Ljava/nio/IntBuffer;)Ljava/lang/String;", 4, I(program), I(0), L(size), L(type));
        CHECK(!strcmp(text_of(r.l), "aPosition") && ints_of(a)[0] == 0x8B50);
    }
    /* A buffer that is neither direct nor over an array of its own kind (a view of a heap ByteBuffer): Android's exception. */
    wrong = jcall(jcall_static("java/nio/ByteBuffer", "allocate", "(I)Ljava/nio/ByteBuffer;", I(64)), "asFloatBuffer", "()Ljava/nio/FloatBuffer;", none);
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(wrong));
    CHECK(wrong && strstr(thrown(), "Must use a native order direct Buffer") != NULL);
    /* A null buffer is NULL: glBufferData with no data. */
    {
        mg_jobj *names = ints(1);
        GL20("glGenBuffers", "(I[II)V", 3, I(1), L(names), I(0));
        GL20("glBindBuffer", "(II)V", 2, I(0x8892), I(ints_of(names)[0]));
        GL20("glBufferData", "(IILjava/nio/Buffer;I)V", 4, I(0x8892), I(24), L(NULL), I(0x88E4));
        CHECK(!mg_dvm_pending());
        GL20("glBindBuffer", "(II)V", 2, I(0x8892), I(0));
    }
    EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0));
    EGL("destroySurface", "(J)V", 1, J(surf));
    EGL("destroyContext", "(J)V", 1, J(x));
    thrown();
}

/* A GL frame of one colour, with a triangle over its top left (in OpenGL's terms: y up), into the current surface. */
static void draw_frame(i32 program, mg_jobj *fb, float r, float g, float b)
{
    GL20("glClearColor", "(FFFF)V", 4, F(r), F(g), F(b), F(1));
    GL20("glClear", "(I)V", 1, I(0x4000));
    if (!program) return;
    GL20("glUseProgram", "(I)V", 1, I(program));
    GL20("glUniform4f", "(IFFFF)V", 5, I(0), F(1), F(1), F(0), F(1));
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(fb));
    GL20("glEnableVertexAttribArray", "(I)V", 1, I(0));
    GL20("glDrawArrays", "(III)V", 3, I(4), I(0), I(3));
}

static i32 triangle_program(void)
{
    i32 shader = GL20("glCreateShader", "(I)I", 1, I(0x8B31)).i, program = GL20("glCreateProgram", "()I", 0).i;
    GL20("glShaderSource", "(ILjava/lang/String;)V", 2, I(shader), L(mg_dvm_new_string("attribute vec2 aPosition; uniform vec4 uColor;")));
    GL20("glAttachShader", "(II)V", 2, I(program), I(shader));
    GL20("glLinkProgram", "(I)V", 1, I(program));
    return program;
}

/*
 * The window: a views' picture of 64 x 48, opaque grey with a hole at (10, 8) 40 x 20 and, in the hole, row 20 black at half
 * alpha (a dimmed dialog's kind); a window surface of the hole's size under it.
 */
static void test_window(void)
{
    static const float top_left[6] = { -1, 1, 0, 1, -1, 0 };
    mg_jobj *view, *fb = NULL;
    u8 *views;
    i64 x, x3, surf, small;
    i32 program = 0;
    int pictures;
    say("OpenGL ES from Java: the window composed of the views and a surface's frame under them");
    memset(&W, 0, sizeof(W));
    mg_java_gl_start(64, 48, shown);
    views = views_picture(64, 48, 10, 8, 40, 20, 20);
    /* Before any window surface the views are not the compositor's business. */
    CHECK(!mg_java_gl_views(views, 64, 48, 64 * 4, k_all));
    view = mg_dvm_new("java/lang/Object", "()V", NULL);
    x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    surf = EGL("createWindowSurface", "(Ljava/lang/Object;II)J", 3, L(view), I(40), I(20)).j;
    CHECK(x && surf && EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x)).z);
    if (mg_dvm_class_exists("java/nio/ByteBuffer")) {
        fb = direct_floats(top_left, 6);
        program = triangle_program();
    }
    /* A frame before the views have shown their hole is not shown. */
    draw_frame(program, fb, 1, 0.5f, 0);
    pictures = W.pictures;
    CHECK(EGL("swap", "(JJIIII)Z", 6, J(surf), J(x), I(0), I(0), I(0), I(0)).z);
    CHECK(W.pictures == pictures);
    /* The views say where the view is, and their picture comes: shown whole, the first time, and the frame under the hole. */
    EGL("place", "(Ljava/lang/Object;IIII)V", 5, L(view), I(10), I(8), I(40), I(20));
    CHECK(mg_java_gl_views(views, 64, 48, 64 * 4, k_all));
    CHECK(W.pictures == pictures + 1 && W.part[0] == 0 && W.part[1] == 0 && W.part[2] == 64 && W.part[3] == 48);
    CHECK_HEX(window_pixel(5, 5), 0x808080);
    CHECK_HEX(window_pixel(45, 25), 0xFF8000);
    /* Half black over orange: the views' black plus 127/255 of the frame. */
    CHECK_HEX(window_pixel(45, 20), 0x7F4000);
    /* OpenGL's rows run bottom up: what it drew at the top left is at the view's top left, not its bottom. */
    if (program) {
        CHECK_HEX(window_pixel(11, 9), 0xFFFF00);
        CHECK_HEX(window_pixel(11, 26), 0xFF8000);
    }
    /* A new frame under unchanged views: shown as the view's rectangle alone. */
    draw_frame(program, fb, 0, 0.5f, 1);
    CHECK(EGL("swap", "(JJIIII)Z", 6, J(surf), J(x), I(10), I(8), I(40), I(20)).z);
    CHECK(W.part[0] == 10 && W.part[1] == 8 && W.part[2] == 50 && W.part[3] == 28);
    CHECK_HEX(window_pixel(45, 25), 0x0080FF);
    CHECK_HEX(window_pixel(5, 5), 0x808080);
    /* The views change in part (a rectangle over the hole's right end becomes opaque red): the frame stays under the rest. */
    {
        int yy, xx;
        i32 part[4] = { 40, 10, 50, 15 };
        for (yy = 10; yy < 15; yy++)
            for (xx = 40; xx < 50; xx++) {
                u8 *q = views + ((size_t)yy * 64 + (size_t)xx) * 4;
                q[0] = 0;
                q[1] = 0;
                q[2] = 255;
                q[3] = 255;
            }
        CHECK(mg_java_gl_views(views, 64, 48, 64 * 4, part));
        CHECK(W.part[0] == 40 && W.part[1] == 10 && W.part[2] == 50 && W.part[3] == 15);
        CHECK_HEX(window_pixel(45, 12), 0xFF0000);
        CHECK_HEX(window_pixel(30, 12), 0x0080FF);
        draw_frame(program, fb, 0, 1, 0);
        EGL("swap", "(JJIIII)Z", 6, J(surf), J(x), I(10), I(8), I(40), I(20));
        CHECK_HEX(window_pixel(45, 12), 0xFF0000);
        CHECK_HEX(window_pixel(30, 12), 0x00FF00);
    }
    /*
     * ANGLE's programs kept on disk (mg_egl.c, EGL_ANDROID_blob_cache, which the stand-in lists): given their functions when
     * the app has a data folder, they keep a program and find it again.
     */
    {
        const char *data = megler_data_dir();
        if (data && *data) CHECK(mg_gl_standin_blob_given());
        CHECK(mg_gl_standin_blob_check() == 0);
    }
    /* The app's own framebuffer and (OpenGL ES 3) pack buffer bound at the swap: the frame is still the surface's, and both are bound after. */
    mg_gl_standin_reset(false, false);
    mg_java_gl_reset();
    x3 = EGL("createContext", "(IJ)J", 2, I(3), J(0)).j;
    CHECK(x3 && EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x3)).z);
    {
        mg_jobj *a = ints(2);
        if (fb) program = triangle_program();
        draw_frame(program, fb, 1, 0, 1);
        GL20("glGenBuffers", "(I[II)V", 3, I(1), L(a), I(0));
        GL20("glBindBuffer", "(II)V", 2, I(0x88EB), I(ints_of(a)[0]));
        if (fb) GL20("glBufferData", "(IILjava/nio/Buffer;I)V", 4, I(0x88EB), I(64), L(NULL), I(0x88E4));
        GL20("glBindFramebuffer", "(II)V", 2, I(0x8D40), I(5));
        CHECK(EGL("swap", "(JJIIII)Z", 6, J(surf), J(x3), I(10), I(8), I(40), I(20)).z);
        CHECK_HEX(window_pixel(30, 25), 0xFF00FF);
        GL20("glGetIntegerv", "(I[II)V", 3, I(0x8CA6), L(a), I(1));
        CHECK(ints_of(a)[1] == 5);
        GL20("glGetIntegerv", "(I[II)V", 3, I(0x88ED), L(a), I(1));
        CHECK(ints_of(a)[1] == ints_of(a)[0]);
        GL20("glBindFramebuffer", "(II)V", 2, I(0x8D40), I(0));
        GL20("glBindBuffer", "(II)V", 2, I(0x88EB), I(0));
    }
    /* That frame was read back as R, G, B, A (this stand-in lists no GL_EXT_read_format_bgra now): the same colours. */
    if (program) CHECK_HEX(window_pixel(11, 9), 0xFFFF00);
    /* A surface of half the view's size is scaled into it. */
    small = EGL("createWindowSurface", "(Ljava/lang/Object;II)J", 3, L(view), I(20), I(10)).j;
    CHECK(small && EGL("makeCurrent", "(JJ)Z", 2, J(small), J(x3)).z);
    GL20("glViewport", "(IIII)V", 4, I(0), I(0), I(20), I(10));
    if (fb) program = triangle_program();
    draw_frame(program, fb, 0, 0, 1);
    EGL("swap", "(JJIIII)Z", 6, J(small), J(x3), I(10), I(8), I(40), I(20));
    CHECK_HEX(window_pixel(48, 26), 0x0000FF);
    if (program) {
        /* The triangle's top left corner covers frame pixels (0..9, 9) and so the view's (10..29, 8..9). */
        CHECK_HEX(window_pixel(11, 8), 0xFFFF00);
        CHECK_HEX(window_pixel(12, 9), 0xFFFF00);
    }
    /* The view moves (the views' next picture has its hole elsewhere): the old place is the views' again, the new one the frame's. */
    free(views);
    views = views_picture(64, 48, 20, 24, 40, 20, -1);
    EGL("place", "(Ljava/lang/Object;IIII)V", 5, L(view), I(20), I(24), I(40), I(20));
    CHECK(mg_java_gl_views(views, 64, 48, 64 * 4, k_all));
    CHECK_HEX(window_pixel(12, 10), 0x808080);
    CHECK_HEX(window_pixel(55, 40), 0x0000FF);
    /* The view loses its surface: what was under the hole is gone. */
    EGL("forget", "(Ljava/lang/Object;)V", 1, L(view));
    CHECK_HEX(window_pixel(55, 40), 0x000000);
    pictures = W.pictures;
    EGL("swap", "(JJIIII)Z", 6, J(small), J(x3), I(20), I(24), I(40), I(20));
    CHECK(W.pictures == pictures);
    EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0));
    EGL("destroySurface", "(J)V", 1, J(small));
    EGL("destroySurface", "(J)V", 1, J(surf));
    EGL("destroyContext", "(J)V", 1, J(x3));
    EGL("destroyContext", "(J)V", 1, J(x));
    free(views);
    thrown();
}

/* A thread that swaps a surface's frames while the views change on the calling thread: the window's lock between them. */
static struct {
    i64 x, surf;
    volatile i32 frames, done;
} G;

static MG_HOSTCB void swapping(void *arg)
{
    int i;
    (void)arg;
    mg_dvm_self();
    EGL("makeCurrent", "(JJ)Z", 2, J(G.surf), J(G.x));
    for (i = 0; i < 40; i++) {
        GL20("glClearColor", "(FFFF)V", 4, F(0), F(1), F(0), F(1));
        GL20("glClear", "(I)V", 1, I(0x4000));
        EGL("swap", "(JJIIII)Z", 6, J(G.surf), J(G.x), I(0), I(0), I(64), I(48));
        mg_atomic_add(&G.frames, 1);
    }
    EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0));
    mg_atomic_xchg(&G.done, 1);
}

static void test_two_threads(void)
{
    mg_jobj *view = mg_dvm_new("java/lang/Object", "()V", NULL);
    u8 *views = views_picture(64, 48, 0, 0, 32, 48, -1);
    mg_thread *t;
    int i, tries;
    say("OpenGL ES from Java: a GL thread swapping while the views change on another");
    memset(&W, 0, sizeof(W));
    memset(&G, 0, sizeof(G));
    mg_java_gl_start(64, 48, shown);
    G.x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    G.surf = EGL("createWindowSurface", "(Ljava/lang/Object;II)J", 3, L(view), I(64), I(48)).j;
    EGL("place", "(Ljava/lang/Object;IIII)V", 5, L(view), I(0), I(0), I(64), I(48));
    t = mg_thread_start(swapping, NULL, 1u << 20, "gl-test");
    CHECK(t != NULL);
    for (i = 0; i < 100 && t; i++) {
        i32 part[4];
        part[0] = i % 64;
        part[1] = 0;
        part[2] = part[0] + 1;
        part[3] = 48;
        mg_java_gl_views(views, 64, 48, 64 * 4, i ? part : k_all);
        mg_sleep_ns(3000000ull);
    }
    for (tries = 0; tries < 500 && !G.done; tries++) mg_sleep_ns(10000000ull);
    if (t) mg_thread_join(t);
    CHECK(G.done && G.frames == 40);
    /* The views' left half is the hole: green there, grey on the right. */
    mg_java_gl_views(views, 64, 48, 64 * 4, k_all);
    CHECK_HEX(window_pixel(5, 5), 0x00FF00);
    CHECK_HEX(window_pixel(40, 5), 0x808080);
    EGL("destroySurface", "(J)V", 1, J(G.surf));
    EGL("destroyContext", "(J)V", 1, J(G.x));
    free(views);
    thrown();
}

/*
 * What a swap costs at a phone's size: a window of 720 x 1280, a GLSurfaceView of 720 x 1182 under it (AIDA64's main
 * screen), a hole over all of it but a row of text across it, swapped thirty times. Measured here with the stand-in's
 * read-back, which is a copy of memory: the phone's is ANGLE's, and its log says that one.
 */
static void test_cost(void)
{
    mg_jobj *view = mg_dvm_new("java/lang/Object", "()V", NULL);
    u8 *views;
    mg_java_gl_stats a, b;
    i64 x, surf;
    int i, y, xx;
    u64 t0, t1;
    say("OpenGL ES from Java: what a swap costs at 720 x 1280");
    memset(&W, 0, sizeof(W));
    mg_gl_standin_reset(true, false);
    mg_java_gl_reset();
    mg_java_gl_start(720, 1280, shown);
    views = views_picture(720, 1280, 0, 98, 720, 1182, -1);
    /* A line of text over the GL view: rows where views and frame mix. */
    for (y = 400; y < 440; y++)
        for (xx = 40; xx < 680; xx += 3) views[((size_t)y * 720 + (size_t)xx) * 4 + 3] = 255;
    x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    surf = EGL("createWindowSurface", "(Ljava/lang/Object;II)J", 3, L(view), I(720), I(1182)).j;
    EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x));
    EGL("place", "(Ljava/lang/Object;IIII)V", 5, L(view), I(0), I(98), I(720), I(1182));
    t0 = mg_time_monotonic_ns();
    mg_java_gl_views(views, 720, 1280, 720 * 4, k_all);
    t1 = mg_time_monotonic_ns();
    mg_java_gl_get_stats(&a);
    for (i = 0; i < 30; i++) {
        GL20("glClearColor", "(FFFF)V", 4, F(0), F((float)i / 30), F(1), F(1));
        GL20("glClear", "(I)V", 1, I(0x4000));
        EGL("swap", "(JJIIII)Z", 6, J(surf), J(x), I(0), I(98), I(720), I(1182));
    }
    mg_java_gl_get_stats(&b);
    CHECK(b.swaps - a.swaps == 30 && b.shown - a.shown == 30);
    CHECK_HEX(window_pixel(700, 1200), 0x00F7FF);
    say("  a swap's own work %.2f ms: reading back %.2f (the stand-in's: a copy), composing %.2f, the shell's copy %.2f; the views' picture "
        "composed over it %.2f ms",
        (double)((b.read_ns - a.read_ns) + (b.compose_ns - a.compose_ns) + (b.show_ns - a.show_ns)) / 30e6, (double)(b.read_ns - a.read_ns) / 30e6,
        (double)(b.compose_ns - a.compose_ns) / 30e6, (double)(b.show_ns - a.show_ns) / 30e6, (double)(t1 - t0) / 1e6);
    EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0));
    EGL("destroySurface", "(J)V", 1, J(surf));
    EGL("destroyContext", "(J)V", 1, J(x));
    free(views);
    thrown();
}

/*
 * Turns (mg_egl.c): two threads, each with a context of the app's display current, clear at once, one through what guests
 * are given (mg_egl_resolve) and one through what the layer's Java natives are (mg_egl_host). The stand-in counts a clear
 * that came while another was still in it, and one of a context other than the one made current last (what ANGLE's
 * renderer draws wrong with, or through a null pointer): none of either, and the threads' contexts made current again in
 * between. The thunks are ARM's; elsewhere nothing is checked.
 */
typedef struct turn_side {
    void *d, *ctx;
    bool host;
    int clears;
    volatile i32 ready;                /* 1: its context current; -1: it could not be */
    volatile i32 done;
} turn_side;
static volatile i32 g_turn_go;

static void turn_clearing(void *p)
{
    typedef unsigned (*make_fn)(void *, void *, void *, void *);
    typedef void (*clear_fn)(u32);
    turn_side *s = (turn_side *)p;
    make_fn make = (make_fn)(s->host ? mg_egl_host("eglMakeCurrent") : mg_egl_resolve("eglMakeCurrent"));
    clear_fn clear = (clear_fn)(s->host ? mg_egl_host("glClear") : mg_egl_resolve("glClear"));
    int i;
    if (!make || !clear || !make(s->d, NULL, NULL, s->ctx)) {
        mg_atomic_xchg(&s->ready, -1);
        mg_atomic_xchg(&s->done, 1);
        return;
    }
    mg_atomic_xchg(&s->ready, 1);
    while (!g_turn_go) mg_sleep_ns(100000ull);
    for (i = 0; i < 200; i++) {
        clear(0x4000);
        s->clears++;
    }
    make(s->d, NULL, NULL, NULL);
    mg_atomic_xchg(&s->done, 1);
}

/*
 * A pbuffer and a context destroyed while current (mg_egl.c, doom): EGL ends them once no thread has them current, and so
 * does the layer -- a thread's turn makes nothing current for a moment, which ANGLE took for letting them go (Yandex Maps'
 * end in ANGLE, package 528). The stand-in ends at once what it is asked to: the pbuffer is still there after its destroy;
 * the context destroyed by the thread that has it current is let go of there and ended, and the pbuffer with it.
 */
static void test_doomed(void)
{
    typedef void *(*display_fn)(void *);
    typedef unsigned (*init_fn)(void *, int *, int *);
    typedef void *(*create_fn)(void *, void *, void *, const int *);
    typedef void *(*pbuffer_fn)(void *, void *, const int *);
    typedef unsigned (*destroy_fn)(void *, void *);
    typedef unsigned (*make_fn)(void *, void *, void *, void *);
    static const int es2[] = { 0x3098, 2, 0x3038 }, size[] = { 0x3057, 4, 0x3056, 4, 0x3038 };
    display_fn get_display = (display_fn)mg_egl_resolve("eglGetDisplay");
    init_fn initialize = (init_fn)mg_egl_resolve("eglInitialize");
    create_fn create = (create_fn)mg_egl_resolve("eglCreateContext");
    pbuffer_fn pbuffer = (pbuffer_fn)mg_egl_resolve("eglCreatePbufferSurface");
    destroy_fn destroy = (destroy_fn)mg_egl_resolve("eglDestroyContext"), destroy_surface = (destroy_fn)mg_egl_resolve("eglDestroySurface");
    make_fn make = (make_fn)mg_egl_resolve("eglMakeCurrent");
    mg_gl_standin_stats a, b, c;
    int major = 0, minor = 0;
    void *d, *ctx, *pb;
    say("A pbuffer and a context destroyed while current: ended once let go");
    CHECK(get_display && initialize && create && pbuffer && destroy && destroy_surface && make);
    if (!get_display || !initialize || !create || !pbuffer || !destroy || !destroy_surface || !make) return;
    d = get_display(NULL);
    CHECK(d && initialize(d, &major, &minor));
    ctx = create(d, NULL, NULL, es2);
    pb = pbuffer(d, NULL, size);
    CHECK(ctx && pb);
    if (!ctx || !pb) return;
    CHECK(make(d, pb, pb, ctx));
    mg_gl_standin_stats_get(&a);
    CHECK(destroy_surface(d, pb));
    mg_gl_standin_stats_get(&b);
    CHECK(b.surfaces_alive == a.surfaces_alive && b.contexts_alive == a.contexts_alive);
    CHECK(destroy(d, ctx));
    CHECK(make(d, NULL, NULL, NULL));
    mg_gl_standin_stats_get(&c);
    CHECK(c.surfaces_alive == a.surfaces_alive - 1 && c.contexts_alive == a.contexts_alive - 1);
    say("  alive: %d surfaces and %d contexts before, %d and %d after the destroys, %d and %d once let go", (int)a.surfaces_alive, (int)a.contexts_alive,
        (int)b.surfaces_alive, (int)b.contexts_alive, (int)c.surfaces_alive, (int)c.contexts_alive);
}

/*
 * A thread with no context current that makes and ends contexts of the display while another draws, as Google Play
 * services' probe of OpenGL ES did in Play Store's sign-in: ANGLE makes a context current to end it, and then not the other
 * thread's again, so the other's next call has its own made current again (mg_egl.c, turn_forget) -- the WebView's
 * glDrawArrays read ANGLE's state manager through a null pointer (package 569). The stand-in's destroy does what ANGLE's
 * does; none of the drawing thread's clears is of a context ANGLE did not have current.
 */
static void turn_probing(void *p)
{
    typedef void *(*create_fn)(void *, void *, void *, const int *);
    typedef unsigned (*destroy_fn)(void *, void *);
    typedef unsigned (*make_fn)(void *, void *, void *, void *);
    static const int es2[] = { 0x3098, 2, 0x3038 };
    turn_side *s = (turn_side *)p;
    create_fn create = (create_fn)mg_egl_resolve("eglCreateContext");
    destroy_fn destroy = (destroy_fn)mg_egl_resolve("eglDestroyContext");
    make_fn make = (make_fn)mg_egl_resolve("eglMakeCurrent");
    int i;
    /* Its own context current once, while the other's is: the display's calls take turns from then on. */
    if (!create || !destroy || !make || !make(s->d, NULL, NULL, s->ctx)) {
        mg_atomic_xchg(&s->ready, -1);
        mg_atomic_xchg(&s->done, 1);
        return;
    }
    make(s->d, NULL, NULL, NULL);
    mg_atomic_xchg(&s->ready, 1);
    while (!g_turn_go) mg_sleep_ns(100000ull);
    for (i = 0; i < 40; i++) {
        void *c = create(s->d, NULL, NULL, es2);
        if (c) destroy(s->d, c);
        s->clears++;
        mg_sleep_ns(300000ull);
    }
    mg_atomic_xchg(&s->done, 1);
}

static void test_turns_destroy(void)
{
#if MG_HOST_ARM32
    typedef void *(*display_fn)(void *);
    typedef unsigned (*init_fn)(void *, int *, int *);
    typedef void *(*create_fn)(void *, void *, void *, const int *);
    typedef unsigned (*destroy_fn)(void *, void *);
    static const int es2[] = { 0x3098, 2, 0x3038 };
    display_fn get_display = (display_fn)mg_egl_resolve("eglGetDisplay");
    init_fn initialize = (init_fn)mg_egl_resolve("eglInitialize");
    create_fn create = (create_fn)mg_egl_resolve("eglCreateContext");
    destroy_fn destroy = (destroy_fn)mg_egl_resolve("eglDestroyContext");
    turn_side a, b;
    mg_thread *ta, *tb;
    mg_gl_standin_stats before, after;
    int major = 0, minor = 0, tries;
    void *d;
    say("Turns: a thread ends contexts of the display while another draws");
    CHECK(get_display && initialize && create && destroy);
    if (!get_display || !initialize || !create || !destroy) return;
    d = get_display(NULL);
    CHECK(d && initialize(d, &major, &minor));
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.d = b.d = d;
    a.ctx = create(d, NULL, NULL, es2);
    b.ctx = create(d, NULL, NULL, es2);
    CHECK(a.ctx && b.ctx);
    if (!a.ctx || !b.ctx) return;
    g_turn_go = 0;
    mg_gl_standin_set_dwell(50000ull);
    ta = mg_thread_start(turn_clearing, &a, 1u << 20, "turn-draw");
    for (tries = 0; tries < 500 && !a.ready; tries++) mg_sleep_ns(2000000ull);
    tb = mg_thread_start(turn_probing, &b, 1u << 20, "turn-probe");
    CHECK(ta && tb);
    for (tries = 0; tries < 500 && (!a.ready || !b.ready); tries++) mg_sleep_ns(2000000ull);
    CHECK(a.ready == 1 && b.ready == 1);
    mg_gl_standin_stats_get(&before);
    mg_atomic_xchg(&g_turn_go, 1);
    if (ta) mg_thread_join(ta);
    if (tb) mg_thread_join(tb);
    mg_gl_standin_set_dwell(0);
    mg_gl_standin_stats_get(&after);
    CHECK(a.done && b.done && a.clears == 200 && b.clears == 40);
    CHECK(after.stale == before.stale);
    say("  200 clears while 40 contexts were made and ended: %d of a context ANGLE did not have current", (int)(after.stale - before.stale));
    destroy(d, a.ctx);
    destroy(d, b.ctx);
#else
    say("(Turns: the thunks are ARM's, nothing is checked here)");
#endif
}

static void test_turns(void)
{
#if MG_HOST_ARM32
    typedef void *(*display_fn)(void *);
    typedef unsigned (*init_fn)(void *, int *, int *);
    typedef void *(*create_fn)(void *, void *, void *, const int *);
    typedef unsigned (*destroy_fn)(void *, void *);
    static const int es2[] = { 0x3098, 2, 0x3038 };
    display_fn get_display = (display_fn)mg_egl_resolve("eglGetDisplay");
    init_fn initialize = (init_fn)mg_egl_resolve("eglInitialize");
    create_fn create = (create_fn)mg_egl_resolve("eglCreateContext");
    destroy_fn destroy = (destroy_fn)mg_egl_resolve("eglDestroyContext");
    turn_side a, b;
    mg_thread *ta, *tb;
    mg_gl_standin_stats before, after;
    int major = 0, minor = 0, tries;
    void *d;
    say("Turns: two threads with contexts of the app's display current clear at once");
    CHECK(get_display && initialize && create && destroy);
    if (!get_display || !initialize || !create || !destroy) return;
    d = get_display(NULL);
    CHECK(d && initialize(d, &major, &minor));
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.d = b.d = d;
    a.ctx = create(d, NULL, NULL, es2);
    b.ctx = create(d, NULL, NULL, es2);
    b.host = true;
    CHECK(a.ctx && b.ctx);
    if (!a.ctx || !b.ctx) return;
    g_turn_go = 0;
    mg_gl_standin_set_dwell(50000ull);
    ta = mg_thread_start(turn_clearing, &a, 1u << 20, "turn-a");
    tb = mg_thread_start(turn_clearing, &b, 1u << 20, "turn-b");
    CHECK(ta && tb);
    for (tries = 0; tries < 500 && (!a.ready || !b.ready); tries++) mg_sleep_ns(2000000ull);
    CHECK(a.ready == 1 && b.ready == 1);
    mg_gl_standin_stats_get(&before);
    mg_atomic_xchg(&g_turn_go, 1);
    if (ta) mg_thread_join(ta);
    if (tb) mg_thread_join(tb);
    mg_gl_standin_set_dwell(0);
    mg_gl_standin_stats_get(&after);
    CHECK(a.done && b.done && a.clears == 200 && b.clears == 200);
    CHECK(after.overlaps == before.overlaps);
    CHECK(after.stale == before.stale);
    /* Each thread's first clear after the other's made its context current again: none, then its own. */
    CHECK(after.makes - before.makes >= 4);
    say("  400 clears: %d overlapping, %d of a context not made current last; %d make-currents for the turns",
        (int)(after.overlaps - before.overlaps), (int)(after.stale - before.stale), (int)(after.makes - before.makes) - 2);
    destroy(d, a.ctx);
    destroy(d, b.ctx);
#else
    say("(Turns: the thunks are ARM's, nothing is checked here)");
#endif
}

/*
 * The WebView's draw functor (drivers/mg_java_webview.c): the table Chromium is handed (version 4, OpenGL ES), a functor made
 * with it, and drawn as megler.WebFunctors.draw asks -- on the render thread, not the asker's, on_sync before draw_gl; HWUI's
 * parameters for the part of the view the window shows (the clip the part from the surface's top left, the surface its size
 * rounded up to 64, the transform a translation by the part's corner, sRGB); on a pbuffer of the WebView's display, cleared
 * before each draw; its picture handed to the shell as the functor's id above 0x40000000, the keyed mutex let go for the
 * copy, the part where Chromium drew it. A smaller part keeps the pbuffer. Released: on_context_destroyed, then on_destroyed,
 * on the render thread, and the shell's picture let go.
 */
typedef struct {
    i32 version;
    void *query_render_mode, *create_functor, *release_functor, *create_functor_v3, *report_rendering_threads;
} t_fn_table;
typedef struct {
    void *on_sync, *on_context_destroyed, *on_destroyed, *draw_gl, *init_vk, *draw_vk, *post_draw_vk, *remove_overlays;
} t_fn_callbacks;
typedef struct {
    i32 version, clip_left, clip_top, clip_right, clip_bottom, width, height;
    bool deprecated_0;
    float transform[16], transfer[7], to_xyz[9];
    i32 overlays_mode;
    void *get_surface_control, *merge_transaction;
} t_fn_params;

static struct {
    volatile i32 syncs, draws, context_gone, gone, frames, let_go;
    bool in_order, gone_in_order, keyed;
    u32 sync_thread, draw_thread, gone_thread;
    void *data;
    t_fn_params p;
    u32 before;                        /* the surface's top left pixel when draw_gl began */
    int frame_id, fw, fh;
    void *share;
    u32 corner;                        /* the frame's top left pixel, as the shell copied it */
} FT;

static void fn_on_sync(i32 id, void *data, const i32 *params)
{
    (void)id;
    FT.sync_thread = mg_thread_self_id();
    FT.data = data;
    if (params && params[0] == 4) FT.syncs++;
}

static void fn_draw_gl(i32 id, void *data, const t_fn_params *p)
{
    void (*clear_color)(float, float, float, float) = (void (*)(float, float, float, float))mg_egl_host("glClearColor");
    void (*clear)(u32) = (void (*)(u32))mg_egl_host("glClear");
    (void)id;
    (void)data;
    FT.in_order = FT.syncs == FT.draws + 1;
    FT.draw_thread = mg_thread_self_id();
    FT.p = *p;
    FT.before = mg_gl_standin_pixel(0, p->height - 1);
    /* "Chromium's" frame: red. */
    if (clear_color && clear) {
        clear_color(1, 0, 0, 1);
        clear(0x4000);
    }
    FT.draws++;
}

static void fn_context_destroyed(i32 id, void *data)
{
    (void)id;
    (void)data;
    FT.context_gone++;
}

static void fn_destroyed(i32 id, void *data)
{
    (void)id;
    (void)data;
    FT.gone_in_order = FT.context_gone == 1;
    FT.gone_thread = mg_thread_self_id();
    FT.gone++;
}

static MEGLER_CB bool fn_texture_frame(void *user, int id, void *share, int width, int height)
{
    const u8 *rgba;
    int w, h;
    (void)user;
    (void)width;
    (void)height;
    if (!share) {
        FT.let_go = id;
        return true;
    }
    FT.keyed = mg_gl_standin_keyed(share, true);
    if (FT.keyed && mg_gl_standin_shared(share, &rgba, &w, &h) && w > 0 && h > 0) {
        const u8 *px = rgba + (size_t)(h - 1) * (size_t)w * 4;
        FT.fw = w;
        FT.fh = h;
        FT.corner = (u32)px[0] << 24 | (u32)px[1] << 16 | (u32)px[2] << 8 | px[3];
    }
    if (FT.keyed) mg_gl_standin_keyed(share, false);
    FT.frame_id = id;
    FT.share = share;
    FT.frames++;
    return true;
}

static MEGLER_CB void fn_draw_texture(void *user, void *canvas, int id, float l, float t, float r, float b, int alpha)
{
    (void)user, (void)canvas, (void)id, (void)l, (void)t, (void)r, (void)b, (void)alpha;
}

static void test_draw_functor(void)
{
    static t_fn_callbacks cb;
    static megler_gfx_host host;
    const t_fn_table *table = (const t_fn_table *)mg_java_webview_draw_fn_table();
    const megler_gfx_host *was = mg_java_gl_shell();
    static int data;
    void *share;
    i64 drawn;
    i32 id;
    int tries;
    say("The WebView's draw functor: Chromium's page drawn on the render thread, its picture handed to the shell");
    memset(&FT, 0, sizeof(FT));
    memset(&host, 0, sizeof(host));
    host.texture_frame = fn_texture_frame;
    host.draw_texture = fn_draw_texture;
    mg_java_gl_set_shell(&host);
    CHECK(mg_java_gl_shares_frames());
    CHECK(table && table->version == 4 && table->query_render_mode && table->create_functor_v3 && table->release_functor);
    if (!table || !table->create_functor_v3 || !table->release_functor || !table->query_render_mode) goto end;
    CHECK(((i32 (*)(void))table->query_render_mode)() == 0);
    cb.on_sync = (void *)fn_on_sync;
    cb.on_context_destroyed = (void *)fn_context_destroyed;
    cb.on_destroyed = (void *)fn_destroyed;
    cb.draw_gl = (void *)fn_draw_gl;
    id = ((i32 (*)(void *, i32, const t_fn_callbacks *))table->create_functor_v3)(&data, 4, &cb);
    CHECK(id > 0);
    if (id <= 0) goto end;
    /* The part of the view at (10, 20), 100 x 50: a surface of 128 x 64. */
    drawn = mg_java_webview_draw_functor(id, 10, 20, 100, 50);
    CHECK_HEX((u32)drawn, 0x40000000 + id);
    CHECK((drawn >> 32 & 0xFFFF) == 128 && (drawn >> 48 & 0xFFFF) == 64);
    CHECK(FT.syncs == 1 && FT.draws == 1 && FT.in_order && FT.data == &data);
    CHECK(FT.draw_thread && FT.draw_thread == FT.sync_thread && FT.draw_thread != mg_thread_self_id());
    CHECK(FT.p.version == 4 && FT.p.clip_left == 0 && FT.p.clip_top == 0 && FT.p.clip_right == 100 && FT.p.clip_bottom == 50);
    CHECK(FT.p.width == 128 && FT.p.height == 64 && FT.p.overlays_mode == 0);
    CHECK(FT.p.transform[0] == 1 && FT.p.transform[5] == 1 && FT.p.transform[10] == 1 && FT.p.transform[15] == 1);
    CHECK(FT.p.transform[12] == -10 && FT.p.transform[13] == -20 && FT.p.transform[1] == 0 && FT.p.transform[4] == 0);
    CHECK(FT.p.transfer[0] == 2.4f && FT.p.to_xyz[0] > 0.43f && FT.p.to_xyz[0] < 0.44f && FT.p.to_xyz[8] > 0.71f);
    CHECK(FT.frames == 1 && FT.frame_id == 0x40000000 + id && FT.keyed && FT.fw == 128 && FT.fh == 64);
    CHECK_HEX(FT.corner, 0xFF0000FF);
    share = FT.share;
    /* A smaller part: the same surface, cleared before Chromium draws again. */
    drawn = mg_java_webview_draw_functor(id, 0, 0, 90, 40);
    CHECK((u32)drawn == (u32)(0x40000000 + id) && (drawn >> 32 & 0xFFFF) == 128 && (drawn >> 48 & 0xFFFF) == 64);
    CHECK(FT.syncs == 2 && FT.draws == 2 && FT.in_order && FT.frames == 2 && FT.share == share);
    CHECK_HEX(FT.before, 0);
    CHECK(FT.p.clip_right == 90 && FT.p.clip_bottom == 40 && FT.p.transform[12] == 0 && FT.p.transform[13] == 0);
    /* Released: its last callbacks on the render thread, the context's first. */
    ((void (*)(i32))table->release_functor)(id);
    for (tries = 0; tries < 400 && !FT.gone; tries++) mg_sleep_ns(5000000ull);
    CHECK(FT.gone == 1 && FT.context_gone == 1 && FT.gone_in_order && FT.gone_thread == FT.draw_thread);
    for (tries = 0; tries < 100 && FT.let_go != 0x40000000 + id; tries++) mg_sleep_ns(5000000ull);
    CHECK(FT.let_go == 0x40000000 + id);
    /* Gone, it draws nothing. */
    CHECK(mg_java_webview_draw_functor(id, 0, 0, 90, 40) == 0 && FT.draws == 2);
end:
    mg_java_webview_forget_egl();
    mg_java_gl_set_shell(was);
}

int mg_gl_natives_tests_run(mg_test_sink sink, void *user, int *checks)
{
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    if (!mg_dvm_running()) {
        say("(OpenGL ES from Java: the runtime is not running, nothing is checked)");
        return 0;
    }
    test_bound();
    mg_egl_set_provider(mg_gl_standin);
    mg_gl_standin_reset(true, false);
    mg_java_gl_reset();
    test_version();
    test_hardware_buffers();
    test_webview_display();
    mg_egl_webview_own_display(false);
    test_draw_functor();
    test_fence_syncs();
    test_doomed();
    test_turns();
    test_turns_destroy();
    test_calls();
    test_buffers();
    mg_gl_standin_reset(true, false);
    mg_java_gl_reset();
    test_window();
    mg_gl_standin_reset(true, false);
    mg_java_gl_reset();
    test_two_threads();
    test_cost();
    mg_java_gl_start(0, 0, NULL);
    mg_egl_set_provider(NULL);
    mg_java_gl_reset();
    free(W.pixels);
    memset(&W, 0, sizeof(W));
    say("  %d checks, %d failed", t_checks, t_failed);
    *checks += t_checks;
    return t_failed;
}
