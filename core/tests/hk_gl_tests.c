/*
 * OpenGL ES and EGL from an app's Java (core/src/drivers/hk_java_gl.c, hk_java_gles.c), called as the runtime calls them:
 * an hk_jcall with one value for each parameter of the Java declaration, arrays, strings and buffers made by the runtime.
 * The OpenGL ES is the stand-in that draws (hk_gl_standin.c), given to the core where ANGLE would be, so what a call drew
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

#include "../src/drivers/hk_java_gl.h"
#include "../src/dvm/hk_dvm.h"
#include "../src/dvm/hk_dvm_internal.h"
#include "../src/host/hk_host.h"
#include "../src/ndk/hk_egl.h"
#include "hk_gfx_tests.h"
#include "hk_gl_standin.h"

static int t_checks, t_failed;
static hk_test_sink t_sink;
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

static hk_jvalue I(i32 v)
{
    hk_jvalue x;
    x.j = 0;
    x.i = v;
    return x;
}

static hk_jvalue J(i64 v)
{
    hk_jvalue x;
    x.j = v;
    return x;
}

static hk_jvalue F(float v)
{
    hk_jvalue x;
    x.j = 0;
    x.f = v;
    return x;
}

static hk_jvalue Z(bool v)
{
    hk_jvalue x;
    x.j = 0;
    x.z = v;
    return x;
}

static hk_jvalue L(hk_jobj *v)
{
    hk_jvalue x;
    x.j = 0;
    x.l = v;
    return x;
}

static const hk_jhle *find_native(const char *cls, const char *name, const char *sig)
{
    const hk_jhle *tables[2], *e;
    int t;
    tables[0] = hk_java_gl_natives();
    tables[1] = hk_java_gles_natives();
    for (t = 0; t < 2; t++)
        for (e = tables[t]; e->cls; e++)
            if (!strcmp(e->cls, cls) && !strcmp(e->name, name) && (!e->sig || !strcmp(e->sig, sig))) return e;
    return NULL;
}

/* A native by its class ("GLES20", "EGLNative"), name and signature, with `count` values for its parameters, in the order of the Java declaration. */
static hk_jvalue call(const char *cls, const char *name, const char *sig, int count, ...)
{
    char full[80];
    const hk_jhle *e;
    hk_jvalue args[16];
    hk_jcall c;
    va_list ap;
    int i;
    memset(&c, 0, sizeof(c));
    memset(args, 0, sizeof(args));
    va_start(ap, count);
    for (i = 0; i < count && i < 16; i++) args[i] = va_arg(ap, hk_jvalue);
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

static hk_jobj *ints(int n)
{
    hk_jobj *a = hk_dvm_new_array("[I", n);
    return a;
}
static i32 *ints_of(hk_jobj *a) { return (i32 *)a->u.arr.data; }

static const char *thrown(void)
{
    static char text[300];
    text[0] = 0;
    if (hk_dvm_pending()) {
        hk_dvm_describe_pending(text, sizeof(text));
        hk_dvm_clear_pending();
    }
    return text;
}

static const char *text_of(hk_jobj *s)
{
    const char *t = s ? hk_dvm_string_utf8(s) : NULL;
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
    const hk_jhle *d;
    int declared = 0, bound = 0, generated = 0, by_hand = 0;
    say("OpenGL ES from Java: the GLES classes' natives bound");
    for (d = hk_java_gles_declared(); d->cls; d++) {
        const hk_jhle *e = find_native(d->cls, d->name, d->sig);
        declared++;
        if (e) bound++;
        else say("  not bound: %s.%s%s", d->cls, d->name, d->sig);
    }
    for (d = hk_java_gles_natives(); d->cls; d++) generated++;
    for (d = hk_java_gl_natives(); d->cls; d++)
        if (!strncmp(d->cls, "android/opengl/", 15)) by_hand++;
    say("  %d declared by the framework's GLES10 to GLES32, %d bound: %d generated (tools/gles-natives.py), %d by hand", declared, bound, generated, by_hand);
    CHECK(declared == 832);
    CHECK(bound == declared);
    CHECK(generated + by_hand == declared);
}

static void test_calls(void)
{
    hk_jobj *a, *s;
    i64 x, surf;
    i32 shader, program;
    hk_jvalue r;
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
    CHECK_HEX(hk_gl_standin_pixel(3, 3), 0x4080BFFF);
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
    s = hk_dvm_new_string(vertex);
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
    CHECK(GL20("glGetAttribLocation", "(ILjava/lang/String;)I", 2, I(program), L(hk_dvm_new_string("aPosition"))).i == 0);
    CHECK(GL20("glGetUniformLocation", "(ILjava/lang/String;)I", 2, I(program), L(hk_dvm_new_string("uColor"))).i == 0);
    CHECK(GL20("glGetUniformLocation", "(ILjava/lang/String;)I", 2, I(program), L(hk_dvm_new_string("nothing"))).i == -1);
    {
        hk_jobj *size = ints(2), *type = ints(3);
        r = GL20("glGetActiveAttrib", "(II[II[II)Ljava/lang/String;", 6, I(program), I(0), L(size), I(1), L(type), I(2));
        CHECK(!strcmp(text_of(r.l), "aPosition") && ints_of(size)[1] == 1 && ints_of(type)[2] == 0x8B52);
        r = GL20("glGetActiveUniform", "(II[II[II)Ljava/lang/String;", 6, I(program), I(0), L(size), I(0), L(type), I(0));
        CHECK(!strcmp(text_of(r.l), "uColor") && ints_of(type)[0] == 0x8B52);
    }
    /* The generated form with a byte[] for the name. */
    {
        hk_jobj *length = ints(1), *size = ints(1), *type = ints(1), *name = hk_dvm_new_array("[B", 32);
        GL20("glGetActiveAttrib", "(III[II[II[II[BI)V", 11, I(program), I(0), I(30), L(length), I(0), L(size), I(0), L(type), I(0), L(name), I(2));
        CHECK(ints_of(length)[0] == 9 && !memcmp((const char *)name->u.arr.data + 2, "aPosition", 10));
    }
    /* A function this OpenGL ES does not have does nothing and says so once. */
    GL20("glSampleCoverage", "(FZ)V", 2, F(0.5f), Z(true));
    CHECK(!hk_dvm_pending());
    CHECK(EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0)).z);
    EGL("destroySurface", "(J)V", 1, J(surf));
    EGL("destroyContext", "(J)V", 1, J(x));
}

/* java.nio: a direct buffer of floats from a ByteBuffer, with a position; a buffer over an array; one of neither kind. Only on libcore. */
static hk_jobj *jcall_static(const char *cls, const char *name, const char *sig, hk_jvalue arg)
{
    hk_jvalue r;
    r.j = 0;
    if (!hk_dvm_call_static(cls, name, sig, &arg, &r)) return NULL;
    return r.l;
}

static hk_jobj *jcall(hk_jobj *o, const char *name, const char *sig, hk_jvalue arg)
{
    hk_jvalue r;
    r.j = 0;
    if (!o || !hk_dvm_call_virtual(o, name, sig, &arg, &r)) return NULL;
    return r.l;
}

/*
 * A direct FloatBuffer as an app makes one (ByteBuffer.allocateDirect, the native order, asFloatBuffer), with the floats
 * written into its memory from here: FloatBuffer.put on a direct buffer is libcore's native code, which a PC does not run.
 */
static hk_jobj *direct_floats(const float *values, int n)
{
    hk_jobj *bb = jcall_static("java/nio/ByteBuffer", "allocateDirect", "(I)Ljava/nio/ByteBuffer;", I(n * 4)), *order, *fb;
    hk_jvalue none, address;
    none.j = address.j = 0;
    order = jcall_static("java/nio/ByteOrder", "nativeOrder", "()Ljava/nio/ByteOrder;", none);
    bb = jcall(bb, "order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;", L(order));
    fb = jcall(bb, "asFloatBuffer", "()Ljava/nio/FloatBuffer;", none);
    if (!fb || !hk_dvm_get_field(bb, "address", &address) || !address.j) {
        say("  (no direct buffer: %s)", thrown());
        return NULL;
    }
    memcpy((void *)(hk_addr)address.j, values, (size_t)n * 4);
    return fb;
}

static void set_position(hk_jobj *buffer, int position)
{
    if (!jcall(buffer, "position", "(I)Ljava/nio/Buffer;", I(position))) {
        hk_dvm_clear_pending();
        jcall(buffer, "position", "(I)Ljava/nio/FloatBuffer;", I(position));
    }
}

static void test_buffers(void)
{
    /* A triangle over the left half of the surface, and the same one a vertex further on in a longer buffer. */
    static const float tri[6] = { -1, -1, 0, -1, -1, 1 };
    static const float later[8] = { 9, 9, -1, -1, 0, -1, -1, 1 };
    hk_jobj *fb, *bytes, *ib, *heap, *wrong, *a;
    hk_jvalue none;
    i64 x, surf;
    i32 shader, program;
    u8 *p;
    say("OpenGL ES from Java: java.nio buffers");
    if (!hk_dvm_class_exists("java/nio/ByteBuffer")) {
        say("  (not on the built-in java.lang: there is no java.nio)");
        return;
    }
    none.j = 0;
    x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    surf = EGL("createSurface", "(II)J", 2, I(16), I(16)).j;
    CHECK(x && surf && EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x)).z);
    shader = GL20("glCreateShader", "(I)I", 1, I(0x8B31)).i;
    GL20("glShaderSource", "(ILjava/lang/String;)V", 2, I(shader), L(hk_dvm_new_string("attribute vec2 aPosition; uniform vec4 uColor;")));
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
    CHECK_HEX(hk_gl_standin_pixel(2, 2), 0xFFFF00FF);
    CHECK_HEX(hk_gl_standin_pixel(14, 14), 0x0000FFFF);
    /* Its position counts, in elements of its own kind: two floats on is one vertex on. */
    GL20("glClear", "(I)V", 1, I(0x4000));
    fb = direct_floats(later, 8);
    set_position(fb, 2);
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(fb));
    GL20("glDrawArrays", "(III)V", 3, I(4), I(0), I(3));
    CHECK_HEX(hk_gl_standin_pixel(2, 2), 0xFFFF00FF);
    CHECK(!hk_dvm_pending());
    /* A FloatBuffer over an array (FloatBuffer.wrap): the array's own memory. */
    GL20("glClear", "(I)V", 1, I(0x4000));
    a = hk_dvm_new_array("[F", 6);
    memcpy(a->u.arr.data, tri, sizeof(tri));
    heap = jcall_static("java/nio/FloatBuffer", "wrap", "([F)Ljava/nio/FloatBuffer;", L(a));
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(heap));
    GL20("glDrawArrays", "(III)V", 3, I(4), I(0), I(3));
    CHECK(heap && hk_gl_standin_pixel(2, 2) == 0xFFFF00FF);
    /* Pixels read back into a direct ByteBuffer, R, G, B, A, the bottom row first. */
    bytes = jcall_static("java/nio/ByteBuffer", "allocateDirect", "(I)Ljava/nio/ByteBuffer;", I(16 * 16 * 4));
    CHECK(bytes != NULL);
    GL20("glReadPixels", "(IIIIIILjava/nio/Buffer;)V", 7, I(0), I(0), I(16), I(16), I(0x1908), I(0x1401), L(bytes));
    {
        hk_jvalue address;
        address.j = 0;
        hk_dvm_get_field(bytes, "address", &address);
        p = (u8 *)(hk_addr)address.j;
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
        hk_jobj *size = jcall_static("java/nio/IntBuffer", "wrap", "([I)Ljava/nio/IntBuffer;", L(ints(1))), *type = jcall_static("java/nio/IntBuffer", "wrap", "([I)Ljava/nio/IntBuffer;", L(a));
        hk_jvalue r = GL20("glGetActiveAttrib", "(IILjava/nio/IntBuffer;Ljava/nio/IntBuffer;)Ljava/lang/String;", 4, I(program), I(0), L(size), L(type));
        CHECK(!strcmp(text_of(r.l), "aPosition") && ints_of(a)[0] == 0x8B50);
    }
    /* A buffer that is neither direct nor over an array of its own kind (a view of a heap ByteBuffer): Android's exception. */
    wrong = jcall(jcall_static("java/nio/ByteBuffer", "allocate", "(I)Ljava/nio/ByteBuffer;", I(64)), "asFloatBuffer", "()Ljava/nio/FloatBuffer;", none);
    GL20("glVertexAttribPointer", "(IIIZILjava/nio/Buffer;)V", 6, I(0), I(2), I(0x1406), Z(false), I(0), L(wrong));
    CHECK(wrong && strstr(thrown(), "Must use a native order direct Buffer") != NULL);
    /* A null buffer is NULL: glBufferData with no data. */
    {
        hk_jobj *names = ints(1);
        GL20("glGenBuffers", "(I[II)V", 3, I(1), L(names), I(0));
        GL20("glBindBuffer", "(II)V", 2, I(0x8892), I(ints_of(names)[0]));
        GL20("glBufferData", "(IILjava/nio/Buffer;I)V", 4, I(0x8892), I(24), L(NULL), I(0x88E4));
        CHECK(!hk_dvm_pending());
        GL20("glBindBuffer", "(II)V", 2, I(0x8892), I(0));
    }
    EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0));
    EGL("destroySurface", "(J)V", 1, J(surf));
    EGL("destroyContext", "(J)V", 1, J(x));
    thrown();
}

/* A GL frame of one colour, with a triangle over its top left (in OpenGL's terms: y up), into the current surface. */
static void draw_frame(i32 program, hk_jobj *fb, float r, float g, float b)
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
    GL20("glShaderSource", "(ILjava/lang/String;)V", 2, I(shader), L(hk_dvm_new_string("attribute vec2 aPosition; uniform vec4 uColor;")));
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
    hk_jobj *view, *fb = NULL;
    u8 *views;
    i64 x, x3, surf, small;
    i32 program = 0;
    int pictures;
    say("OpenGL ES from Java: the window composed of the views and a surface's frame under them");
    memset(&W, 0, sizeof(W));
    hk_java_gl_start(64, 48, shown);
    views = views_picture(64, 48, 10, 8, 40, 20, 20);
    /* Before any window surface the views are not the compositor's business. */
    CHECK(!hk_java_gl_views(views, 64, 48, 64 * 4, k_all));
    view = hk_dvm_new("java/lang/Object", "()V", NULL);
    x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    surf = EGL("createWindowSurface", "(Ljava/lang/Object;II)J", 3, L(view), I(40), I(20)).j;
    CHECK(x && surf && EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x)).z);
    if (hk_dvm_class_exists("java/nio/ByteBuffer")) {
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
    CHECK(hk_java_gl_views(views, 64, 48, 64 * 4, k_all));
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
        CHECK(hk_java_gl_views(views, 64, 48, 64 * 4, part));
        CHECK(W.part[0] == 40 && W.part[1] == 10 && W.part[2] == 50 && W.part[3] == 15);
        CHECK_HEX(window_pixel(45, 12), 0xFF0000);
        CHECK_HEX(window_pixel(30, 12), 0x0080FF);
        draw_frame(program, fb, 0, 1, 0);
        EGL("swap", "(JJIIII)Z", 6, J(surf), J(x), I(10), I(8), I(40), I(20));
        CHECK_HEX(window_pixel(45, 12), 0xFF0000);
        CHECK_HEX(window_pixel(30, 12), 0x00FF00);
    }
    /* The app's own framebuffer and (OpenGL ES 3) pack buffer bound at the swap: the frame is still the surface's, and both are bound after. */
    hk_gl_standin_reset(false, false);
    hk_java_gl_reset();
    x3 = EGL("createContext", "(IJ)J", 2, I(3), J(0)).j;
    CHECK(x3 && EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x3)).z);
    {
        hk_jobj *a = ints(2);
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
    CHECK(hk_java_gl_views(views, 64, 48, 64 * 4, k_all));
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

static void swapping(void *arg)
{
    int i;
    (void)arg;
    hk_dvm_self();
    EGL("makeCurrent", "(JJ)Z", 2, J(G.surf), J(G.x));
    for (i = 0; i < 40; i++) {
        GL20("glClearColor", "(FFFF)V", 4, F(0), F(1), F(0), F(1));
        GL20("glClear", "(I)V", 1, I(0x4000));
        EGL("swap", "(JJIIII)Z", 6, J(G.surf), J(G.x), I(0), I(0), I(64), I(48));
        hk_atomic_add(&G.frames, 1);
    }
    EGL("makeCurrent", "(JJ)Z", 2, J(0), J(0));
    hk_atomic_xchg(&G.done, 1);
}

static void test_two_threads(void)
{
    hk_jobj *view = hk_dvm_new("java/lang/Object", "()V", NULL);
    u8 *views = views_picture(64, 48, 0, 0, 32, 48, -1);
    hk_thread *t;
    int i, tries;
    say("OpenGL ES from Java: a GL thread swapping while the views change on another");
    memset(&W, 0, sizeof(W));
    memset(&G, 0, sizeof(G));
    hk_java_gl_start(64, 48, shown);
    G.x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    G.surf = EGL("createWindowSurface", "(Ljava/lang/Object;II)J", 3, L(view), I(64), I(48)).j;
    EGL("place", "(Ljava/lang/Object;IIII)V", 5, L(view), I(0), I(0), I(64), I(48));
    t = hk_thread_start(swapping, NULL, 1u << 20, "gl-test");
    CHECK(t != NULL);
    for (i = 0; i < 100 && t; i++) {
        i32 part[4];
        part[0] = i % 64;
        part[1] = 0;
        part[2] = part[0] + 1;
        part[3] = 48;
        hk_java_gl_views(views, 64, 48, 64 * 4, i ? part : k_all);
        hk_sleep_ns(3000000ull);
    }
    for (tries = 0; tries < 500 && !G.done; tries++) hk_sleep_ns(10000000ull);
    if (t) hk_thread_join(t);
    CHECK(G.done && G.frames == 40);
    /* The views' left half is the hole: green there, grey on the right. */
    hk_java_gl_views(views, 64, 48, 64 * 4, k_all);
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
    hk_jobj *view = hk_dvm_new("java/lang/Object", "()V", NULL);
    u8 *views;
    hk_java_gl_stats a, b;
    i64 x, surf;
    int i, y, xx;
    u64 t0, t1;
    say("OpenGL ES from Java: what a swap costs at 720 x 1280");
    memset(&W, 0, sizeof(W));
    hk_gl_standin_reset(true, false);
    hk_java_gl_reset();
    hk_java_gl_start(720, 1280, shown);
    views = views_picture(720, 1280, 0, 98, 720, 1182, -1);
    /* A line of text over the GL view: rows where views and frame mix. */
    for (y = 400; y < 440; y++)
        for (xx = 40; xx < 680; xx += 3) views[((size_t)y * 720 + (size_t)xx) * 4 + 3] = 255;
    x = EGL("createContext", "(IJ)J", 2, I(2), J(0)).j;
    surf = EGL("createWindowSurface", "(Ljava/lang/Object;II)J", 3, L(view), I(720), I(1182)).j;
    EGL("makeCurrent", "(JJ)Z", 2, J(surf), J(x));
    EGL("place", "(Ljava/lang/Object;IIII)V", 5, L(view), I(0), I(98), I(720), I(1182));
    t0 = hk_time_monotonic_ns();
    hk_java_gl_views(views, 720, 1280, 720 * 4, k_all);
    t1 = hk_time_monotonic_ns();
    hk_java_gl_get_stats(&a);
    for (i = 0; i < 30; i++) {
        GL20("glClearColor", "(FFFF)V", 4, F(0), F((float)i / 30), F(1), F(1));
        GL20("glClear", "(I)V", 1, I(0x4000));
        EGL("swap", "(JJIIII)Z", 6, J(surf), J(x), I(0), I(98), I(720), I(1182));
    }
    hk_java_gl_get_stats(&b);
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

int hk_gl_natives_tests_run(hk_test_sink sink, void *user, int *checks)
{
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    if (!hk_dvm_running()) {
        say("(OpenGL ES from Java: the runtime is not running, nothing is checked)");
        return 0;
    }
    test_bound();
    hk_egl_set_provider(hk_gl_standin);
    hk_gl_standin_reset(true, false);
    hk_java_gl_reset();
    test_calls();
    test_buffers();
    hk_gl_standin_reset(true, false);
    hk_java_gl_reset();
    test_window();
    hk_gl_standin_reset(true, false);
    hk_java_gl_reset();
    test_two_threads();
    test_cost();
    hk_java_gl_start(0, 0, NULL);
    hk_egl_set_provider(NULL);
    hk_java_gl_reset();
    free(W.pixels);
    memset(&W, 0, sizeof(W));
    say("  %d checks, %d failed", t_checks, t_failed);
    *checks += t_checks;
    return t_failed;
}
