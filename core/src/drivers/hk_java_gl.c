/*
 * OpenGL ES and EGL for an app's Java, and the window they are shown in (docs/ARCHITECTURE.md, "OpenGL ES from an app's
 * Java"). In this file, in order:
 *
 *   - what hk_java_gles.c's generated natives use: a function for a name, arrays, buffers and strings as memory;
 *   - the GLES natives that are no plain call (strings, buffers, String[]), by hand;
 *   - EGL: contexts and pbuffers on the core's one display, for megler.EGLNative and for the GL threads;
 *   - the window: the views' picture over the frames of the EGL window surfaces, composed here and handed to the shell;
 *   - a swap: the frame read back, composed, shown, paced;
 *   - megler.EGLNative;
 *   - the GL thread of a GLSurfaceView (megler.Native.startGL).
 */
#include "hk_java_gl.h"

#include <stdio.h>

#include "../bionic/hk_bionic.h"
#include "../dvm/hk_dvm_internal.h"
#include "../host/hk_host.h"
#include "../ndk/hk_egl.h"
#include "../run/hk_run.h"

#define N(name) static void name(hk_jcall *c)

enum {
    GL_VENDOR = 0x1F00, GL_EXTENSIONS = 0x1F03, GL_RGBA = 0x1908, GL_BGRA_EXT = 0x80E1, GL_UNSIGNED_BYTE = 0x1401,
    GL_INFO_LOG_LENGTH = 0x8B84, GL_SHADER_SOURCE_LENGTH = 0x8B88, GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87,
    GL_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x8B8A, GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH = 0x8C76, GL_UNIFORM_BLOCK_NAME_LENGTH = 0x8A41,
    GL_BUFFER_MAP_LENGTH = 0x9120, GL_NAME_LENGTH = 0x92F9,
    GL_FRAMEBUFFER = 0x8D40, GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_READ_FRAMEBUFFER = 0x8CA8, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
    GL_PIXEL_PACK_BUFFER = 0x88EB, GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED, GL_PACK_ROW_LENGTH = 0x0D02, GL_PACK_SKIP_ROWS = 0x0D03,
    GL_PACK_SKIP_PIXELS = 0x0D04, GL_PACK_ALIGNMENT = 0x0D05,
};

enum {
    EGL_NONE = 0x3038, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056, EGL_CONTEXT_CLIENT_VERSION = 0x3098, EGL_RENDERABLE_TYPE = 0x3040,
    EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 1, EGL_OPENGL_ES2_BIT = 4, EGL_OPENGL_ES3_BIT = 0x40, EGL_RED_SIZE = 0x3024,
    EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022, EGL_ALPHA_SIZE = 0x3021, EGL_DEPTH_SIZE = 0x3025, EGL_STENCIL_SIZE = 0x3026,
    EGL_EXTENSIONS = 0x3055,
};

/* ------------------------------------------- for hk_java_gles.c's natives */

#define NOT_THERE ((void *)(hk_addr)1)

void *hk_java_gl_fn(void **slot, const char *name)
{
    void *f = *slot;
    if (f == NOT_THERE) return NULL;
    if (f) return f;
    f = hk_egl_host(name);
    if (!f) {
        hk_note_once("java: this OpenGL ES has no %s: the app's calls of it do nothing", name);
        *slot = NOT_THERE;
        return NULL;
    }
    *slot = f;
    return f;
}

bool hk_java_gl_array(hk_jcall *c, int k, int esize, void **out)
{
    hk_jobj *a = c->args[k].l;
    i32 offset = c->args[k + 1].i;
    *out = NULL;
    /* Android's words for it. Its own checks of how much a call needs past the offset are not made: the call is given the rest of the array. */
    if (!a) return hk_dvm_throw("java/lang/IllegalArgumentException", "array == null");
    if (offset < 0) return hk_dvm_throw("java/lang/IllegalArgumentException", "offset < 0");
    if (offset > hk_dvm_array_length(a)) return hk_dvm_throw("java/lang/IllegalArgumentException", "length - offset < needed");
    *out = (u8 *)a->u.arr.data + (size_t)offset * (size_t)esize;
    return true;
}

/* java.nio.Buffer's fields, where every buffer has them (an instance field's slot is the same in every subclass). */
static struct {
    volatile i32 state;                /* 0 not looked for, 1 found, 2 not there */
    u32 address, position, limit, shift;
} BF;

static bool buffer_fields(void)
{
    if (!BF.state) {
        hk_dvm_class *b = hk_dvm_find_class("java/nio/Buffer");
        hk_dvm_field *a = b ? hk_dvm_find_field(b, "address", false) : NULL, *p = b ? hk_dvm_find_field(b, "position", false) : NULL;
        hk_dvm_field *l = b ? hk_dvm_find_field(b, "limit", false) : NULL, *s = b ? hk_dvm_find_field(b, "_elementSizeShift", false) : NULL;
        if (!b) hk_dvm_clear_pending();
        if (a && p && l && s) {
            BF.address = a->slot;
            BF.position = p->slot;
            BF.limit = l->slot;
            BF.shift = s->slot;
        }
        hk_atomic_xchg(&BF.state, a && p && l && s ? 1 : 2);
    }
    return BF.state == 1;
}

/* A buffer's memory from its position on, and the bytes from there to its limit. */
static bool buffer_at(hk_jobj *b, void **out, size_t *left)
{
    hk_jvalue *slots, hb, offset;
    i64 address;
    i32 position, limit, shift;
    *out = NULL;
    if (left) *left = 0;
    if (!b) return true;
    if (!b->dvm || !buffer_fields()) return hk_dvm_throw("java/lang/IllegalArgumentException", "Must use a native order direct Buffer");
    slots = hk_dvm_slots(b);
    address = slots[BF.address].j;
    position = slots[BF.position].i;
    limit = slots[BF.limit].i;
    shift = slots[BF.shift].i;
    if (left) *left = limit > position ? (size_t)(limit - position) << shift : 0;
    if (address) {
        *out = (void *)(hk_addr)(address + ((i64)position << shift));
        return true;
    }
    /* A buffer over an array (HeapIntBuffer and its kind): the array from arrayOffset + position, as NIOAccess gives it. */
    if (hk_dvm_get_field(b, "hb", &hb) && hb.l && hk_dvm_get_field(b, "offset", &offset)) {
        *out = (u8 *)hb.l->u.arr.data + ((size_t)(offset.i + position) << shift);
        return true;
    }
    return hk_dvm_throw("java/lang/IllegalArgumentException", "Must use a native order direct Buffer");
}

bool hk_java_gl_buffer(hk_jcall *c, int k, bool needed, void **out)
{
    if (needed && !c->args[k].l) return hk_dvm_throw("java/lang/IllegalArgumentException", "buffer == null");
    return buffer_at(c->args[k].l, out, NULL);
}

bool hk_java_gl_string(hk_jcall *c, int k, const char **out)
{
    *out = hk_dvm_string_utf8(c->args[k].l);
    return *out || hk_dvm_throw("java/lang/IllegalArgumentException", "string == null");
}

/* ---------------------------------------------- the GLES natives by hand */

enum {
    F_GET_STRING, F_GET_STRINGI, F_SHADER_SOURCE, F_GET_SHADERIV, F_GET_SHADER_LOG, F_GET_PROGRAMIV, F_GET_PROGRAM_LOG, F_GET_SHADER_SOURCE,
    F_ACTIVE_ATTRIB, F_ACTIVE_UNIFORM, F_TF_VARYING, F_BLOCKIV, F_BLOCK_NAME, F_PIPELINEIV, F_PIPELINE_LOG, F_RESOURCEIV, F_RESOURCE_NAME,
    F_UNIFORM_INDICES, F_TF_VARYINGS, F_SHADER_PROGRAMV, F_MAP_RANGE, F_BUFFER_I64V, F_BUFFER_POINTERV, F_DEBUG_LOG,
    F_GET_INTEGERV, F_BIND_FRAMEBUFFER, F_BIND_BUFFER, F_PIXEL_STOREI, F_READ_PIXELS,
    F_COUNT
};
static void *g_fn[F_COUNT];
#define FN(type, id, name) ((type)hk_java_gl_fn(&g_fn[id], name))

typedef const u8 *(*fn_get_string)(u32);
typedef const u8 *(*fn_get_stringi)(u32, u32);
typedef void (*fn_shader_source)(u32, i32, const char *const *, const i32 *);
typedef void (*fn_getiv)(u32, u32, i32 *);
typedef void (*fn_text)(u32, i32, i32 *, char *);
typedef void (*fn_active)(u32, u32, i32, i32 *, void *, void *, char *);
typedef void (*fn_blockiv)(u32, u32, u32, i32 *);
typedef void (*fn_block_name)(u32, u32, i32, i32 *, char *);
typedef void (*fn_resourceiv)(u32, u32, u32, i32, const u32 *, i32, i32 *, i32 *);
typedef void (*fn_resource_name)(u32, u32, u32, i32, i32 *, char *);
typedef void (*fn_uniform_indices)(u32, i32, const char *const *, void *);
typedef void (*fn_tf_varyings)(u32, i32, const char *const *, u32);
typedef u32 (*fn_shader_programv)(u32, i32, const char *const *);
typedef void *(*fn_map_range)(u32, intptr_t, intptr_t, u32);
typedef void (*fn_buffer_i64v)(u32, u32, i64 *);
typedef void (*fn_buffer_pointerv)(u32, u32, void **);
typedef u32 (*fn_debug_log)(u32, i32, void *, void *, void *, void *, void *, void *);
typedef void (*fn_get_integerv)(u32, i32 *);
typedef void (*fn_bind)(u32, u32);
typedef void (*fn_pixel_storei)(u32, i32);
typedef void (*fn_read_pixels)(i32, i32, i32, i32, u32, u32, void *);

N(gl_nothing) { (void)c; }

/* As Android answers what its own glue leaves out ("not yet implemented"): the label calls, the pointer one, a debug callback into Java. */
N(gl_not_yet) { hk_dvm_throw("java/lang/UnsupportedOperationException", "not yet implemented"); }

static hk_jobj *string_or_null(const u8 *s) { return s ? hk_dvm_new_string((const char *)s) : NULL; }

/* glGetString(name): null with no context current, as on Android, whose C function gives NULL then. */
N(gl_GetString)
{
    fn_get_string f = FN(fn_get_string, F_GET_STRING, "glGetString");
    c->ret.l = f ? string_or_null(f((u32)c->args[0].i)) : NULL;
}

N(gl_GetStringi)
{
    fn_get_stringi f = FN(fn_get_stringi, F_GET_STRINGI, "glGetStringi");
    c->ret.l = f ? string_or_null(f((u32)c->args[0].i, (u32)c->args[1].i)) : NULL;
}

N(gl_ShaderSource)
{
    fn_shader_source f = FN(fn_shader_source, F_SHADER_SOURCE, "glShaderSource");
    const char *text;
    if (!f || !hk_java_gl_string(c, 1, &text)) return;
    f((u32)c->args[0].i, 1, &text, NULL);
}

/* A text of an object -- a log, a shader's source -- asked for its length first; "" when it has none. */
static hk_jobj *object_text(u32 object, u32 length_name, int getter, const char *getter_name, int reader, const char *reader_name)
{
    fn_getiv get = FN(fn_getiv, getter, getter_name);
    fn_text read = FN(fn_text, reader, reader_name);
    i32 length = 0, got = 0;
    char *text;
    hk_jobj *s;
    if (!get || !read) return hk_dvm_new_string("");
    get(object, length_name, &length);
    if (length <= 0) return hk_dvm_new_string("");
    text = (char *)hk_alloc((size_t)length + 1);
    text[0] = 0;
    read(object, length, &got, text);
    text[got > 0 && got <= length ? got : 0] = 0;
    s = hk_dvm_new_string(text);
    free(text);
    return s;
}

N(gl_GetShaderInfoLog) { c->ret.l = object_text((u32)c->args[0].i, GL_INFO_LOG_LENGTH, F_GET_SHADERIV, "glGetShaderiv", F_GET_SHADER_LOG, "glGetShaderInfoLog"); }
N(gl_GetProgramInfoLog) { c->ret.l = object_text((u32)c->args[0].i, GL_INFO_LOG_LENGTH, F_GET_PROGRAMIV, "glGetProgramiv", F_GET_PROGRAM_LOG, "glGetProgramInfoLog"); }
N(gl_GetShaderSource) { c->ret.l = object_text((u32)c->args[0].i, GL_SHADER_SOURCE_LENGTH, F_GET_SHADERIV, "glGetShaderiv", F_GET_SHADER_SOURCE, "glGetShaderSource"); }
N(gl_GetProgramPipelineInfoLog)
{
    c->ret.l = object_text((u32)c->args[0].i, GL_INFO_LOG_LENGTH, F_PIPELINEIV, "glGetProgramPipelineiv", F_PIPELINE_LOG, "glGetProgramPipelineInfoLog");
}

/*
 * The forms of glGetActiveAttrib, glGetActiveUniform and glGetTransformFeedbackVarying that return the name: the program's
 * longest name asked first (`longest`), its size and type written to two arrays with offsets (`arrays`) or two buffers.
 */
static void active_name(hk_jcall *c, bool arrays, u32 longest, int id, const char *name)
{
    fn_getiv get = FN(fn_getiv, F_GET_PROGRAMIV, "glGetProgramiv");
    fn_active f = (fn_active)hk_java_gl_fn(&g_fn[id], name);
    void *size, *type;
    i32 length = 0, got = 0;
    char *text;
    if (!get || !f) return;
    if (arrays ? !(hk_java_gl_array(c, 2, 4, &size) && hk_java_gl_array(c, 4, 4, &type)) : !(hk_java_gl_buffer(c, 2, true, &size) && hk_java_gl_buffer(c, 3, true, &type))) return;
    get((u32)c->args[0].i, longest, &length);
    if (length <= 0) length = 256;
    text = (char *)hk_alloc((size_t)length + 1);
    text[0] = 0;
    f((u32)c->args[0].i, (u32)c->args[1].i, length, &got, size, type, text);
    text[got > 0 && got <= length ? got : 0] = 0;
    c->ret.l = hk_dvm_new_string(text);
    free(text);
}

N(gl_GetActiveAttrib_a) { active_name(c, true, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, F_ACTIVE_ATTRIB, "glGetActiveAttrib"); }
N(gl_GetActiveAttrib_b) { active_name(c, false, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, F_ACTIVE_ATTRIB, "glGetActiveAttrib"); }
N(gl_GetActiveUniform_a) { active_name(c, true, GL_ACTIVE_UNIFORM_MAX_LENGTH, F_ACTIVE_UNIFORM, "glGetActiveUniform"); }
N(gl_GetActiveUniform_b) { active_name(c, false, GL_ACTIVE_UNIFORM_MAX_LENGTH, F_ACTIVE_UNIFORM, "glGetActiveUniform"); }
N(gl_GetTransformFeedbackVarying_a) { active_name(c, true, GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH, F_TF_VARYING, "glGetTransformFeedbackVarying"); }
N(gl_GetTransformFeedbackVarying_b) { active_name(c, false, GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH, F_TF_VARYING, "glGetTransformFeedbackVarying"); }
/* The form whose name is a single byte, which Android's own glue refuses so. */
N(gl_GetTransformFeedbackVarying_byte) { hk_dvm_throw("java/lang/UnsupportedOperationException", "deprecated"); }

N(gl_GetActiveUniformBlockName_s)
{
    fn_blockiv get = FN(fn_blockiv, F_BLOCKIV, "glGetActiveUniformBlockiv");
    fn_block_name f = FN(fn_block_name, F_BLOCK_NAME, "glGetActiveUniformBlockName");
    i32 length = 0, got = 0;
    char *text;
    if (!get || !f) return;
    get((u32)c->args[0].i, (u32)c->args[1].i, GL_UNIFORM_BLOCK_NAME_LENGTH, &length);
    if (length <= 0) length = 256;
    text = (char *)hk_alloc((size_t)length + 1);
    text[0] = 0;
    f((u32)c->args[0].i, (u32)c->args[1].i, length, &got, text);
    text[got > 0 && got <= length ? got : 0] = 0;
    c->ret.l = hk_dvm_new_string(text);
    free(text);
}

/* glGetActiveUniformBlockName(program, index, Buffer length, Buffer name): the name buffer's room is what C calls bufSize. */
N(gl_GetActiveUniformBlockName_b)
{
    fn_block_name f = FN(fn_block_name, F_BLOCK_NAME, "glGetActiveUniformBlockName");
    void *length, *name;
    size_t room = 0;
    if (!f || !buffer_at(c->args[2].l, &length, NULL) || !buffer_at(c->args[3].l, &name, &room)) return;
    if (!name) {
        hk_dvm_throw("java/lang/IllegalArgumentException", "uniformBlockName == null");
        return;
    }
    f((u32)c->args[0].i, (u32)c->args[1].i, (i32)room, (i32 *)length, (char *)name);
}

/* A String[] as C strings, or NULL with IllegalArgumentException pending; free the result. */
static const char **strings_of(hk_jobj *a, i32 *count)
{
    const char **v;
    i32 i, n;
    *count = 0;
    if (!a) {
        hk_dvm_throw("java/lang/IllegalArgumentException", "names == null");
        return NULL;
    }
    n = hk_dvm_array_length(a);
    v = (const char **)hk_alloc(sizeof(char *) * (size_t)(n ? n : 1));
    for (i = 0; i < n; i++) {
        v[i] = hk_dvm_string_utf8(a->u.oarr.v[i]);
        if (!v[i]) {
            free((void *)v);
            hk_dvm_throw("java/lang/IllegalArgumentException", "null string in array");
            return NULL;
        }
    }
    *count = n;
    return v;
}

static void uniform_indices(hk_jcall *c, bool array)
{
    fn_uniform_indices f = FN(fn_uniform_indices, F_UNIFORM_INDICES, "glGetUniformIndices");
    const char **names;
    void *indices;
    i32 n;
    if (!f || !(array ? hk_java_gl_array(c, 2, 4, &indices) : hk_java_gl_buffer(c, 2, true, &indices))) return;
    names = strings_of(c->args[1].l, &n);
    if (!names) return;
    f((u32)c->args[0].i, n, names, indices);
    free((void *)names);
}

N(gl_GetUniformIndices_b) { uniform_indices(c, false); }
N(gl_GetUniformIndices_a) { uniform_indices(c, true); }

N(gl_TransformFeedbackVaryings)
{
    fn_tf_varyings f = FN(fn_tf_varyings, F_TF_VARYINGS, "glTransformFeedbackVaryings");
    const char **names;
    i32 n;
    if (!f || !(names = strings_of(c->args[1].l, &n))) return;
    f((u32)c->args[0].i, n, names, (u32)c->args[2].i);
    free((void *)names);
}

N(gl_CreateShaderProgramv)
{
    fn_shader_programv f = FN(fn_shader_programv, F_SHADER_PROGRAMV, "glCreateShaderProgramv");
    const char **sources;
    i32 n;
    if (!f || !(sources = strings_of(c->args[1].l, &n))) return;
    c->ret.i = (i32)f((u32)c->args[0].i, n, sources);
    free((void *)sources);
}

/* glMapBufferRange: the mapped memory as a direct ByteBuffer of the length asked, or null. */
N(gl_MapBufferRange)
{
    fn_map_range f = FN(fn_map_range, F_MAP_RANGE, "glMapBufferRange");
    void *p = f ? f((u32)c->args[0].i, (intptr_t)c->args[1].i, (intptr_t)c->args[2].i, (u32)c->args[3].i) : NULL;
    c->ret.l = p ? hk_jni_new_direct_buffer(p, c->args[2].i) : NULL;
}

N(gl_GetBufferPointerv)
{
    fn_buffer_i64v length_of = FN(fn_buffer_i64v, F_BUFFER_I64V, "glGetBufferParameteri64v");
    fn_buffer_pointerv f = FN(fn_buffer_pointerv, F_BUFFER_POINTERV, "glGetBufferPointerv");
    i64 length = 0;
    void *p = NULL;
    if (!length_of || !f) return;
    length_of((u32)c->args[0].i, GL_BUFFER_MAP_LENGTH, &length);
    f((u32)c->args[0].i, (u32)c->args[1].i, &p);
    c->ret.l = p ? hk_jni_new_direct_buffer(p, length) : NULL;
}

N(gl_GetProgramResourceName)
{
    fn_resourceiv get = FN(fn_resourceiv, F_RESOURCEIV, "glGetProgramResourceiv");
    fn_resource_name f = FN(fn_resource_name, F_RESOURCE_NAME, "glGetProgramResourceName");
    u32 prop = GL_NAME_LENGTH;
    i32 length = 0, got = 0;
    char *text;
    if (!get || !f) return;
    get((u32)c->args[0].i, (u32)c->args[1].i, (u32)c->args[2].i, 1, &prop, 1, NULL, &length);
    if (length <= 0) length = 256;
    text = (char *)hk_alloc((size_t)length + 1);
    text[0] = 0;
    f((u32)c->args[0].i, (u32)c->args[1].i, (u32)c->args[2].i, length, &got, text);
    text[got > 0 && got <= length ? got : 0] = 0;
    c->ret.l = hk_dvm_new_string(text);
    free(text);
}

/* glGetDebugMessageLog(count, sources, types, ids, severities, lengths, ByteBuffer messageLog): the log buffer's room is bufSize. */
N(gl_GetDebugMessageLog_b)
{
    fn_debug_log f = FN(fn_debug_log, F_DEBUG_LOG, "glGetDebugMessageLog");
    void *p[6];
    size_t room = 0;
    int i;
    for (i = 0; i < 6; i++)
        if (!buffer_at(c->args[1 + i].l, &p[i], i == 5 ? &room : NULL)) return;
    if (f) c->ret.i = (i32)f((u32)c->args[0].i, (i32)room, p[0], p[1], p[2], p[3], p[4], p[5]);
}

/* ------------------------------------------------------------------- EGL */

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef i32 EGLint;

static struct {
    hk_lock lock;
    volatile i32 state;                /* 0 not tried, 1 ready, 2 no EGL here */
    EGLDisplay display;
    EGLConfig config;
    bool es3, surfaceless;
    u32 (*initialize)(EGLDisplay, EGLint *, EGLint *);
    u32 (*choose)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    u32 (*destroy_context)(EGLDisplay, EGLContext);
    EGLSurface (*create_pbuffer)(EGLDisplay, EGLConfig, const EGLint *);
    u32 (*destroy_surface)(EGLDisplay, EGLSurface);
    u32 (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLint (*get_error)(void);
} E = { HK_LOCK_INIT, 0, NULL, NULL, false, false, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/*
 * The one configuration every context and surface of the app's Java is made with: 8 bits a channel, depth and stencil,
 * for pbuffers (a window surface is a pbuffer here), OpenGL ES 3 where the display has it.
 */
static bool pick_config(bool es3, bool depth)
{
    EGLint attribs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                         EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLint n = 0;
    if (es3) attribs[3] = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
    if (!depth) attribs[13] = attribs[15] = 0;
    return E.choose(E.display, attribs, &E.config, 1, &n) && n >= 1 && E.config;
}

static bool egl_ready(void)
{
    if (E.state) return E.state == 1;
    hk_lock_acquire(&E.lock);
    if (!E.state) {
        EGLDisplay (*get_display)(void *) = (EGLDisplay(*)(void *))hk_egl_host("eglGetDisplay");
        const char *(*query)(EGLDisplay, EGLint) = (const char *(*)(EGLDisplay, EGLint))hk_egl_host("eglQueryString");
        EGLint major = 0, minor = 0;
        bool ok;
        E.initialize = (u32(*)(EGLDisplay, EGLint *, EGLint *))hk_egl_host("eglInitialize");
        E.choose = (u32(*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))hk_egl_host("eglChooseConfig");
        E.create_context = (EGLContext(*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))hk_egl_host("eglCreateContext");
        E.destroy_context = (u32(*)(EGLDisplay, EGLContext))hk_egl_host("eglDestroyContext");
        E.create_pbuffer = (EGLSurface(*)(EGLDisplay, EGLConfig, const EGLint *))hk_egl_host("eglCreatePbufferSurface");
        E.destroy_surface = (u32(*)(EGLDisplay, EGLSurface))hk_egl_host("eglDestroySurface");
        E.make_current = (u32(*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))hk_egl_host("eglMakeCurrent");
        E.get_error = (EGLint(*)(void))hk_egl_host("eglGetError");
        ok = get_display && E.initialize && E.choose && E.create_context && E.destroy_context && E.create_pbuffer && E.destroy_surface && E.make_current
             && E.get_error;
        E.display = ok ? get_display(NULL) : NULL;
        ok = ok && E.display && E.initialize(E.display, &major, &minor);
        if (ok) {
            E.es3 = pick_config(true, true);
            ok = E.es3 || pick_config(false, true) || pick_config(false, false);
        }
        if (ok) {
            const char *ext = query ? query(E.display, EGL_EXTENSIONS) : NULL;
            E.surfaceless = ext && strstr(ext, "EGL_KHR_surfaceless_context");
            hk_info("java: EGL for the app's Java: EGL %d.%d, OpenGL ES %s, every surface 8 bits a channel%s", (int)major, (int)minor, E.es3 ? "3 and 2" : "2",
                    E.surfaceless ? ", contexts without a surface" : "");
        } else {
            hk_warn("java: there is no EGL for the app's Java here: its contexts and surfaces are refused");
        }
        hk_atomic_xchg(&E.state, ok ? 1 : 2);
    }
    hk_lock_release(&E.lock);
    return E.state == 1;
}

typedef struct gl_ctx {
    EGLContext handle;
    int version;                       /* as made: 1, 2 or 3 */
    i32 bgra;                          /* frames read back as B, G, R, A: 1 yes, 0 no, -1 not asked yet */
} gl_ctx;

static gl_ctx *ctx_new(int version, const gl_ctx *share)
{
    EGLint attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext h;
    gl_ctx *x;
    int v = version < 1 ? 1 : version > 3 ? 3 : version;
    if (!egl_ready()) return NULL;
    attribs[1] = v;
    h = E.create_context(E.display, E.config, share ? share->handle : NULL, attribs);
    if (!h && v == 1) {
        /* Android always has OpenGL ES 1; ANGLE may not. An app that asks for it (GL10; a GLSurfaceView's default) gets 2, whose drawing calls of 1 do nothing. */
        hk_note_once("java: this OpenGL ES has no version 1: an app's version-1 contexts (GL10, a GLSurfaceView's default) are version 2, where the fixed-function calls do nothing");
        v = 2;
        attribs[1] = 2;
        h = E.create_context(E.display, E.config, share ? share->handle : NULL, attribs);
    }
    if (!h) {
        hk_warn("java: no OpenGL ES %d context could be made (EGL error 0x%x)", v, (unsigned)E.get_error());
        return NULL;
    }
    x = (gl_ctx *)hk_alloc(sizeof(*x));
    memset(x, 0, sizeof(*x));
    x->handle = h;
    x->version = v;
    x->bgra = -1;
    return x;
}

static void ctx_free(gl_ctx *x)
{
    if (!x) return;
    E.destroy_context(E.display, x->handle);
    free(x);
}

struct layer;

typedef struct gl_surf {
    EGLSurface handle;
    int width, height;
    struct layer *layer;               /* the view it is shown at; NULL for a pbuffer of the app's own */
    u8 *pixels;                        /* where a frame is read back to, rows bottom first */
    size_t pixels_size;
    u64 due;                           /* when the next swap may return */
} gl_surf;

static void layer_attach(struct layer *l);
static void layer_detach(struct layer *l);
static void layer_unused(struct layer *l);

static gl_surf *surf_new(int width, int height, struct layer *l)
{
    EGLint attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    gl_surf *s;
    EGLSurface h;
    if (!egl_ready()) return NULL;
    attribs[1] = width > 0 ? width : 1;
    attribs[3] = height > 0 ? height : 1;
    h = E.create_pbuffer(E.display, E.config, attribs);
    if (!h) {
        hk_warn("java: no %dx%d surface could be made (EGL error 0x%x)", (int)attribs[1], (int)attribs[3], (unsigned)E.get_error());
        if (l) layer_unused(l);
        return NULL;
    }
    s = (gl_surf *)hk_alloc(sizeof(*s));
    memset(s, 0, sizeof(*s));
    s->handle = h;
    s->width = attribs[1];
    s->height = attribs[3];
    s->layer = l;
    if (l) layer_attach(l);
    return s;
}

static void surf_free(gl_surf *s)
{
    if (!s) return;
    if (s->layer) layer_detach(s->layer);
    E.destroy_surface(E.display, s->handle);
    free(s->pixels);
    free(s);
}

/* A 1x1 pbuffer of the calling thread's, for a context with no surface where EGL has no contexts without one. */
static HK_THREAD_LOCAL EGLSurface t_tiny;

static bool make_current(gl_surf *s, gl_ctx *x)
{
    EGLSurface h;
    if (!egl_ready()) return !x;
    if (!x) return E.make_current(E.display, NULL, NULL, NULL) != 0;
    h = s ? s->handle : NULL;
    if (!h && !E.surfaceless) {
        if (!t_tiny) {
            EGLint attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            t_tiny = E.create_pbuffer(E.display, E.config, attribs);
        }
        h = t_tiny;
    }
    return E.make_current(E.display, h, h, x->handle) != 0;
}

/* ------------------------------------------------------------ the window */

#define MAX_LAYERS 8
enum { ROW_HOLE, ROW_COVERED, ROW_MIXED };

/* A SurfaceView EGL draws on: where it is, and its last frame. */
typedef struct layer {
    hk_jobj *view;                     /* the view (a root of the collector's while it is here); NULL: a free slot */
    int surfaces;                      /* EGL surfaces made on it that are still there */
    bool active;                       /* its frames are shown (a surface was made; the view has not lost it since) */
    bool placed;                       /* where it is on the screen is known */
    bool pending;                      /* the view said where it is (place); taken with the next picture of the views */
    i32 x, y, w, h;                    /* the view's rectangle on the screen */
    i32 px, py, pw, ph;                /* the one it said */
    u8 *frame;                         /* its last frame as read back, fw x fh, rows bottom first; fw 0: none */
    size_t frame_size;
    i32 fw, fh;
    bool bgra;
    u8 *rows;                          /* for each row of [y, y + h): a hole there, covered by the views, or both */
    size_t rows_size;
    bool rows_valid;
} layer;

/* The window's lock: the views' picture, the composed one and the layers, and every call of the shell's present from here. */
static hk_lock g_window_lock = HK_LOCK_INIT;

static struct {
    hk_java_show_fn show;
    i32 width, height;
    bool used;                         /* a window surface has been made: the views' picture is kept from then on */
    u8 *views, *out;                   /* the views' last picture and the window as composed: width x height, rows top first */
    bool have_views, shown_whole;
    layer layers[MAX_LAYERS];
    bool roots;
    hk_java_gl_stats stats, window;    /* in all, and as they stood at the start of the log's window */
    u64 first_swap, window_started;
} C;

static i32 max_i(i32 a, i32 b) { return a > b ? a : b; }
static i32 min_i(i32 a, i32 b) { return a < b ? a : b; }

/* The views' picture and the composed one, made when the first window surface is: an app without one never pays for them. */
static void use_window(void)
{
    size_t n = (size_t)C.width * (size_t)C.height * 4;
    if (C.used || !n) return;
    C.views = (u8 *)hk_alloc(n);
    C.out = (u8 *)hk_alloc(n);
    memset(C.views, 0, n);
    memset(C.out, 0, n);
    C.used = true;
    hk_info("java: the app draws with EGL on a view: from now on its window is composed here, the views over OpenGL's frames");
}

static layer *layer_of(hk_jobj *view, bool make)
{
    int i, free_slot = -1;
    for (i = 0; i < MAX_LAYERS; i++) {
        if (C.layers[i].view == view) return &C.layers[i];
        if (!C.layers[i].view && free_slot < 0) free_slot = i;
    }
    if (!make || free_slot < 0) {
        if (make) hk_note_once("java: more than %d views draw with EGL at once: the others' frames are not shown", MAX_LAYERS);
        return NULL;
    }
    memset(&C.layers[free_slot], 0, sizeof(layer));
    C.layers[free_slot].view = view;
    return &C.layers[free_slot];
}

/* The window's composed picture is the views' there again: no frame under them. */
static void copy_views(i32 x0, i32 y0, i32 x1, i32 y1)
{
    size_t stride = (size_t)C.width * 4;
    i32 r;
    x0 = max_i(x0, 0);
    y0 = max_i(y0, 0);
    x1 = min_i(x1, C.width);
    y1 = min_i(y1, C.height);
    for (r = y0; r < y1 && x0 < x1; r++) memcpy(C.out + (size_t)r * stride + (size_t)x0 * 4, C.views + (size_t)r * stride + (size_t)x0 * 4, (size_t)(x1 - x0) * 4);
}

/* Hands the composed picture to the shell, of which [x0, x1) x [y0, y1) changed. The first time, all of it. */
static void show_part(i32 x0, i32 y0, i32 x1, i32 y1)
{
    i32 part[4];
    part[0] = max_i(x0, 0);
    part[1] = max_i(y0, 0);
    part[2] = min_i(x1, C.width);
    part[3] = min_i(y1, C.height);
    if (part[2] <= part[0] || part[3] <= part[1]) return;
    if (!C.shown_whole) {
        part[0] = part[1] = 0;
        part[2] = C.width;
        part[3] = C.height;
        C.shown_whole = true;
    }
    if (C.show) C.show(C.out, C.width, C.height, C.width * 4, part);
}

/* How the views' picture lies over a layer, row by row, for the screen's rows [top, bottom). */
static void classify(layer *l, i32 top, i32 bottom)
{
    i32 x0 = max_i(l->x, 0), x1 = min_i(l->x + l->w, C.width), n = x1 - x0, r, i;
    size_t stride = (size_t)C.width * 4;
    top = max_i(top, max_i(l->y, 0));
    bottom = min_i(bottom, min_i(l->y + l->h, C.height));
    for (r = top; r < bottom; r++) {
        const u8 *a = C.views + (size_t)r * stride + (size_t)x0 * 4 + 3;
        i32 holes = 0, covered = 0;
        for (i = 0; i < n; i++, a += 4) {
            if (*a == 0) holes++;
            else if (*a == 255) covered++;
        }
        l->rows[r - l->y] = (u8)(n <= 0 || covered == n ? ROW_COVERED : holes == n ? ROW_HOLE : ROW_MIXED);
    }
}

/*
 * The views over a layer's frame, into the composed picture, in [x0, x1) x [y0, y1) of the screen. The frame is opaque, as a
 * SurfaceView's surface is unless the app asks otherwise; a frame of another size than the view is scaled to it (the
 * nearest pixel). The views' pixels are premultiplied: what they leave of what is under them is 255 - alpha.
 */
static void compose(layer *l, i32 x0, i32 y0, i32 x1, i32 y1)
{
    size_t stride = (size_t)C.width * 4, frow = (size_t)l->fw * 4;
    bool same = l->fw == l->w, bgra = l->bgra;
    i32 r, col;
    x0 = max_i(max_i(x0, l->x), 0);
    y0 = max_i(max_i(y0, l->y), 0);
    x1 = min_i(min_i(x1, l->x + l->w), C.width);
    y1 = min_i(min_i(y1, l->y + l->h), C.height);
    if (!l->frame || l->fw <= 0 || l->fh <= 0 || x0 >= x1 || y0 >= y1) return;
    for (r = y0; r < y1; r++) {
        i32 lr = r - l->y, fr = l->fh - 1 - (i32)((i64)lr * l->fh / l->h);
        const u8 *g = l->frame + (size_t)fr * frow;
        u8 *o = C.out + (size_t)r * stride;
        const u8 *v = C.views + (size_t)r * stride;
        u8 kind = l->rows_valid ? l->rows[lr] : (u8)ROW_MIXED;
        if (kind == ROW_COVERED) continue;      /* the composed picture has the views' row there already */
        if (kind == ROW_HOLE && same && bgra) {
            memcpy(o + (size_t)x0 * 4, g + (size_t)(x0 - l->x) * 4, (size_t)(x1 - x0) * 4);
            continue;
        }
        for (col = x0; col < x1; col++) {
            const u8 *gp = g + (size_t)(same ? col - l->x : (i32)((i64)(col - l->x) * l->fw / l->w)) * 4, *vp = v + (size_t)col * 4;
            u8 *op = o + (size_t)col * 4;
            u32 a = kind == ROW_HOLE ? 0 : vp[3], left = 255 - a;
            u32 b = bgra ? gp[0] : gp[2], gg = gp[1], rr = bgra ? gp[2] : gp[0];
            if (a == 255) {
                memcpy(op, vp, 4);
                continue;
            }
            if (a == 0) {
                op[0] = (u8)b;
                op[1] = (u8)gg;
                op[2] = (u8)rr;
                op[3] = 255;
                continue;
            }
            op[0] = (u8)min_i(255, (i32)(vp[0] + (b * left + 127) / 255));
            op[1] = (u8)min_i(255, (i32)(vp[1] + (gg * left + 127) / 255));
            op[2] = (u8)min_i(255, (i32)(vp[2] + (rr * left + 127) / 255));
            op[3] = 255;
        }
    }
}

/* With the lock held: a layer's rectangle changes to the one its view said, and its rows are to be looked at again. */
static void take_place(layer *l, i32 *x0, i32 *y0, i32 *x1, i32 *y1)
{
    bool moved = !l->placed || l->x != l->px || l->y != l->py || l->w != l->pw || l->h != l->ph;
    l->pending = false;
    if (!moved) return;
    if (l->placed) {
        copy_views(l->x, l->y, l->x + l->w, l->y + l->h);
        *x0 = min_i(*x0, l->x);
        *y0 = min_i(*y0, l->y);
        *x1 = max_i(*x1, l->x + l->w);
        *y1 = max_i(*y1, l->y + l->h);
    }
    l->x = l->px;
    l->y = l->py;
    l->w = l->pw;
    l->h = l->ph;
    l->placed = true;
    if (l->rows_size < (size_t)l->h) {
        free(l->rows);
        l->rows_size = (size_t)l->h;
        l->rows = (u8 *)hk_alloc(l->rows_size);
    }
    l->rows_valid = false;
    *x0 = min_i(*x0, l->x);
    *y0 = min_i(*y0, l->y);
    *x1 = max_i(*x1, l->x + l->w);
    *y1 = max_i(*y1, l->y + l->h);
}

static void layer_attach(layer *l)
{
    hk_lock_acquire(&g_window_lock);
    use_window();
    l->surfaces++;
    l->active = true;
    hk_lock_release(&g_window_lock);
}

/* With the lock held: a layer shows nothing any more; what was under the views there is gone from the window. */
static void layer_off(layer *l)
{
    if (!l->active) return;
    l->active = false;
    l->fw = l->fh = 0;
    if (C.have_views && l->placed) {
        copy_views(l->x, l->y, l->x + l->w, l->y + l->h);
        show_part(l->x, l->y, l->x + l->w, l->y + l->h);
    }
}

/* A surface on the layer's view is gone. The layer is forgotten with its last surface, unless its view still means to show one. */
static void layer_detach(layer *l)
{
    hk_lock_acquire(&g_window_lock);
    if (l->surfaces > 0) l->surfaces--;
    if (!l->surfaces) {
        layer_off(l);
        free(l->frame);
        free(l->rows);
        memset(l, 0, sizeof(*l));
    }
    hk_lock_release(&g_window_lock);
}

/* A layer made for a surface that could not be made: forgotten again, unless a surface of its view's holds it. */
static void layer_unused(layer *l)
{
    hk_lock_acquire(&g_window_lock);
    if (!l->surfaces) {
        free(l->frame);
        free(l->rows);
        memset(l, 0, sizeof(*l));
    }
    hk_lock_release(&g_window_lock);
}

void hk_java_gl_start(int width, int height, hk_java_show_fn show)
{
    int i;
    hk_lock_acquire(&g_window_lock);
    /* (Once in an app's run; a test starts it again on a window of another size.) */
    for (i = 0; i < MAX_LAYERS; i++) {
        free(C.layers[i].frame);
        free(C.layers[i].rows);
        memset(&C.layers[i], 0, sizeof(layer));
    }
    free(C.views);
    free(C.out);
    C.views = C.out = NULL;
    C.used = C.have_views = C.shown_whole = false;
    memset(&C.stats, 0, sizeof(C.stats));
    C.first_swap = C.window_started = 0;
    C.width = width;
    C.height = height;
    C.show = show;
    if (!C.roots) {
        for (i = 0; i < MAX_LAYERS; i++) hk_dvm_add_root(&C.layers[i].view);
        C.roots = true;
    }
    hk_lock_release(&g_window_lock);
}

void hk_java_gl_reset(void)
{
    hk_lock_acquire(&E.lock);
    hk_atomic_xchg(&E.state, 0);
    hk_lock_release(&E.lock);
    hk_atomic_xchg(&BF.state, 0);
    memset(g_fn, 0, sizeof(g_fn));
    hk_java_gles_forget();
}

bool hk_java_gl_views(const void *pixels, int width, int height, int stride, const i32 part[4])
{
    i32 x0 = 0, y0 = 0, x1 = width, y1 = height, r, sx0, sy0, sx1, sy1;
    size_t line = (size_t)width * 4;
    int i;
    hk_lock_acquire(&g_window_lock);
    if (!C.used || width != C.width || height != C.height || !pixels) {
        hk_lock_release(&g_window_lock);
        return false;
    }
    if (C.have_views && part && part[2] > part[0] && part[3] > part[1]) {
        x0 = max_i(part[0], 0);
        y0 = max_i(part[1], 0);
        x1 = min_i(part[2], width);
        y1 = min_i(part[3], height);
    }
    for (r = y0; r < y1 && x0 < x1; r++) {
        const u8 *from = (const u8 *)pixels + (size_t)r * (size_t)stride + (size_t)x0 * 4;
        memcpy(C.views + (size_t)r * line + (size_t)x0 * 4, from, (size_t)(x1 - x0) * 4);
        memcpy(C.out + (size_t)r * line + (size_t)x0 * 4, from, (size_t)(x1 - x0) * 4);
    }
    C.have_views = true;
    sx0 = x0;
    sy0 = y0;
    sx1 = x1;
    sy1 = y1;
    /* Where the views said their surfaces are, as this picture of them has them. */
    for (i = 0; i < MAX_LAYERS; i++)
        if (C.layers[i].view && C.layers[i].pending) take_place(&C.layers[i], &sx0, &sy0, &sx1, &sy1);
    for (i = 0; i < MAX_LAYERS; i++) {
        layer *l = &C.layers[i];
        if (!l->view || !l->active || !l->placed || l->w <= 0 || l->h <= 0) continue;
        if (!l->rows_valid) {
            classify(l, 0, C.height);
            l->rows_valid = true;
        } else {
            classify(l, y0, y1);
        }
        compose(l, sx0, sy0, sx1, sy1);
    }
    show_part(sx0, sy0, sx1, sy1);
    hk_lock_release(&g_window_lock);
    return true;
}

void hk_java_gl_get_stats(hk_java_gl_stats *out)
{
    hk_lock_acquire(&g_window_lock);
    *out = C.stats;
    hk_lock_release(&g_window_lock);
}

/* ------------------------------------------------------------------ swap */

/*
 * With the lock held: what the frames since `a` (NULL: since the first) have cost, every five seconds while they are shown
 * and once more when a GL thread ends. The longest is the run's.
 */
static void stats_line(const hk_java_gl_stats *a, u64 since, u64 now)
{
    const hk_java_gl_stats *b = &C.stats;
    u32 swaps = b->swaps - (a ? a->swaps : 0), shown = b->shown - (a ? a->shown : 0);
    double seconds = (double)(now - since) / 1e9, n = swaps ? (double)swaps : 1.0;
    u64 read = b->read_ns - (a ? a->read_ns : 0), composing = b->compose_ns - (a ? a->compose_ns : 0), shell = b->show_ns - (a ? a->show_ns : 0);
    if (!swaps) return;
    hk_info("java: OpenGL frames %s%.1f s: %u swaps of window surfaces, %u shown (%.1f a second); a swap's own work %.2f ms on average "
            "(reading back %.2f, composing %.2f, the shell %.2f), the longest %.2f; the window %dx%d",
            a ? "" : "in all, ", seconds, (unsigned)swaps, (unsigned)shown, seconds > 0 ? shown / seconds : 0.0, (double)(read + composing + shell) / n / 1e6,
            (double)read / n / 1e6, (double)composing / n / 1e6, (double)shell / n / 1e6, (double)b->longest_ns / 1e6, (int)C.width, (int)C.height);
}

/*
 * The frame of a window surface, read back on the thread that drew it with its context current: rows bottom first, as
 * B, G, R, A where this OpenGL ES offers that (GL_EXT_read_format_bgra), else R, G, B, A. From the surface whatever
 * framebuffer the app has bound, into memory whatever pack buffer and pack state it has; all of that as it was after.
 */
static bool read_back(gl_surf *s, gl_ctx *x)
{
    fn_get_integerv get = FN(fn_get_integerv, F_GET_INTEGERV, "glGetIntegerv");
    fn_bind bind_fb = FN(fn_bind, F_BIND_FRAMEBUFFER, "glBindFramebuffer"), bind_buffer = FN(fn_bind, F_BIND_BUFFER, "glBindBuffer");
    fn_pixel_storei store = FN(fn_pixel_storei, F_PIXEL_STOREI, "glPixelStorei");
    fn_read_pixels read = FN(fn_read_pixels, F_READ_PIXELS, "glReadPixels");
    i32 fb = 0, pack = 4, pack_buffer = 0, row_length = 0, skip_rows = 0, skip_pixels = 0;
    u32 fb_target = x->version >= 3 ? GL_READ_FRAMEBUFFER : GL_FRAMEBUFFER;
    size_t need = (size_t)s->width * (size_t)s->height * 4;
    if (!get || !store || !read) return false;
    if (x->bgra < 0) {
        fn_get_string string = FN(fn_get_string, F_GET_STRING, "glGetString");
        const u8 *ext = string ? string(GL_EXTENSIONS) : NULL;
        x->bgra = ext && strstr((const char *)ext, "GL_EXT_read_format_bgra") ? 1 : 0;
    }
    if (s->pixels_size < need) {
        free(s->pixels);
        s->pixels = (u8 *)hk_alloc(need);
        s->pixels_size = need;
    }
    if (x->version >= 2 && bind_fb) {
        get(x->version >= 3 ? GL_READ_FRAMEBUFFER_BINDING : GL_FRAMEBUFFER_BINDING, &fb);
        if (fb) bind_fb(fb_target, 0);
    }
    if (x->version >= 3) {
        get(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        get(GL_PACK_ROW_LENGTH, &row_length);
        get(GL_PACK_SKIP_ROWS, &skip_rows);
        get(GL_PACK_SKIP_PIXELS, &skip_pixels);
        if (pack_buffer && bind_buffer) bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
        if (row_length) store(GL_PACK_ROW_LENGTH, 0);
        if (skip_rows) store(GL_PACK_SKIP_ROWS, 0);
        if (skip_pixels) store(GL_PACK_SKIP_PIXELS, 0);
    }
    get(GL_PACK_ALIGNMENT, &pack);
    if (pack != 4) store(GL_PACK_ALIGNMENT, 4);
    read(0, 0, s->width, s->height, x->bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    if (pack != 4) store(GL_PACK_ALIGNMENT, pack);
    if (x->version >= 3) {
        if (pack_buffer && bind_buffer) bind_buffer(GL_PIXEL_PACK_BUFFER, (u32)pack_buffer);
        if (row_length) store(GL_PACK_ROW_LENGTH, row_length);
        if (skip_rows) store(GL_PACK_SKIP_ROWS, skip_rows);
        if (skip_pixels) store(GL_PACK_SKIP_PIXELS, skip_pixels);
    }
    if (fb) bind_fb(fb_target, (u32)fb);
    return true;
}

/* A swap returns no sooner than a sixtieth of a second after the one before was due, as a swap interval of 1 has it. */
static void pace(gl_surf *s)
{
    const u64 period = 16666667ull;
    u64 now = hk_time_monotonic_ns();
    if (!s->due || now > s->due + 100000000ull) s->due = now;
    s->due += period;
    if (s->due > now) hk_sleep_ns(s->due - now);
}

/*
 * A swap: a window surface's frame read back, composed under the views and shown, and the swap paced. A pbuffer's swap
 * does nothing, as on Android. The thread is the one that drew, with the context current.
 */
static bool swap(gl_surf *s, gl_ctx *x)
{
    layer *l;
    u64 t0, t1, t2 = 0, t3, work;
    bool shown = false;
    if (!s || !x) return false;
    l = s->layer;
    if (!l) return true;
    t0 = hk_time_monotonic_ns();
    if (!read_back(s, x)) return false;
    t1 = hk_time_monotonic_ns();
    hk_lock_acquire(&g_window_lock);
    if (l->active) {
        /* The frame just read is the layer's now; the layer's last one is where the next is read to. */
        u8 *spare = l->frame;
        size_t spare_size = l->frame_size;
        l->frame = s->pixels;
        l->frame_size = s->pixels_size;
        l->fw = s->width;
        l->fh = s->height;
        l->bgra = x->bgra == 1;
        s->pixels = spare;
        s->pixels_size = spare_size;
        if (C.have_views && l->placed && l->w > 0 && l->h > 0) {
            if (!l->rows_valid) {
                classify(l, 0, C.height);
                l->rows_valid = true;
            }
            compose(l, l->x, l->y, l->x + l->w, l->y + l->h);
            t2 = hk_time_monotonic_ns();
            show_part(l->x, l->y, l->x + l->w, l->y + l->h);
            shown = true;
        }
    }
    if (!shown) t2 = hk_time_monotonic_ns();
    t3 = hk_time_monotonic_ns();
    work = t3 - t0;
    C.stats.swaps++;
    C.stats.read_ns += t1 - t0;
    C.stats.compose_ns += t2 - t1;
    C.stats.show_ns += t3 - t2;
    if (shown) C.stats.shown++;
    if (work > C.stats.longest_ns) C.stats.longest_ns = work;
    if (!C.window_started) {
        C.first_swap = C.window_started = t0;
        C.window = C.stats;
    } else if (t3 - C.window_started >= 5000000000ull) {
        stats_line(&C.window, C.window_started, t3);
        C.window = C.stats;
        C.window_started = t3;
    }
    hk_lock_release(&g_window_lock);
    if (shown) hk_run_count_frame();
    pace(s);
    return true;
}

/* ------------------------------------------------------------ megler.EGLNative */

/* The calling thread is a GL thread of a GLSurfaceView's: there EGL answers for the thread's own context (MeglerEGL). */
static HK_THREAD_LOCAL bool t_render;

static gl_ctx *ctx_arg(hk_jcall *c, int k) { return (gl_ctx *)(hk_addr)c->args[k].j; }
static gl_surf *surf_arg(hk_jcall *c, int k) { return (gl_surf *)(hk_addr)c->args[k].j; }

/* createContext(int version, long share): a context; 0 when none could be made. */
N(EN_createContext) { c->ret.j = (i64)(hk_addr)ctx_new(c->args[0].i, ctx_arg(c, 1)); }
N(EN_destroyContext) { ctx_free(ctx_arg(c, 0)); }
/* createSurface(int width, int height): a pbuffer of the app's own, which nothing shows. */
N(EN_createSurface) { c->ret.j = (i64)(hk_addr)surf_new(c->args[0].i, c->args[1].i, NULL); }

/* createWindowSurface(Object view, int width, int height): a pbuffer whose frames are shown where the view is. */
N(EN_createWindowSurface)
{
    layer *l;
    hk_lock_acquire(&g_window_lock);
    l = c->args[0].l ? layer_of(c->args[0].l, true) : NULL;
    hk_lock_release(&g_window_lock);
    c->ret.j = (i64)(hk_addr)surf_new(c->args[1].i, c->args[2].i, l);
}

N(EN_destroySurface) { surf_free(surf_arg(c, 0)); }
N(EN_makeCurrent) { c->ret.z = make_current(surf_arg(c, 0), ctx_arg(c, 1)); }

/* swap(long surface, long context, int x, int y, int width, int height): the view's rectangle as the app's thread saw it, for a view that has not said where it is yet. */
N(EN_swap)
{
    gl_surf *s = surf_arg(c, 0);
    layer *l = s ? s->layer : NULL;
    if (l && c->args[4].i > 0 && c->args[5].i > 0) {
        hk_lock_acquire(&g_window_lock);
        if (!l->placed && !l->pending) {
            i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            l->px = c->args[2].i;
            l->py = c->args[3].i;
            l->pw = c->args[4].i;
            l->ph = c->args[5].i;
            take_place(l, &x0, &y0, &x1, &y1);
        }
        hk_lock_release(&g_window_lock);
    }
    c->ret.z = swap(s, ctx_arg(c, 1));
}

N(EN_isRenderThread) { c->ret.z = t_render; }

/* glesVersion(): 3.0 where the display has OpenGL ES 3 configurations, else 2.0 (also with no EGL at all: Android always has 2). */
N(EN_glesVersion) { c->ret.i = egl_ready() && E.es3 ? 0x30000 : 0x20000; }

/* hide(): no view's frames are shown (the original's: every SurfaceView of its with EGL on it said this when it went). */
N(EN_hide)
{
    int i;
    (void)c;
    hk_lock_acquire(&g_window_lock);
    for (i = 0; i < MAX_LAYERS; i++)
        if (C.layers[i].view) layer_off(&C.layers[i]);
    hk_lock_release(&g_window_lock);
}

/* place(Object view, int x, int y, int width, int height): where the view is on the screen, said as it is drawn with its hole. */
N(EN_place)
{
    layer *l;
    hk_lock_acquire(&g_window_lock);
    l = c->args[0].l ? layer_of(c->args[0].l, false) : NULL;
    if (l) {
        l->px = c->args[1].i;
        l->py = c->args[2].i;
        l->pw = c->args[3].i;
        l->ph = c->args[4].i;
        l->pending = true;
    }
    hk_lock_release(&g_window_lock);
}

/* forget(Object view): the view's surface is gone (it left its window, or its window was hidden): its frames are not shown. */
N(EN_forget)
{
    layer *l;
    hk_lock_acquire(&g_window_lock);
    l = c->args[0].l ? layer_of(c->args[0].l, false) : NULL;
    if (l) layer_off(l);
    hk_lock_release(&g_window_lock);
}

/* ------------------------------------------------------------ the GL thread */

/*
 * A GLSurfaceView's GL thread (megler.Native.startGL): one for each view whose renderer is started, a thread of the app's
 * (Java attached, its native code can run on it). What it does each turn the view decides (GLSurfaceView.meglerFrame, the
 * overlay's): docs/ARCHITECTURE.md, "OpenGL ES from an app's Java".
 */
#define MAX_GL_THREADS 8
enum { FRAME_DRAW = 1, FRAME_EXIT = 2, FRAME_NO_SURFACE = 4, FRAME_PAUSED = 8, FRAME_PRESERVE = 16 };

typedef struct gl_view {
    hk_jobj *view, *renderer, *gl10, *config;      /* roots of the collector's while the thread runs */
    int version, number;
    volatile i32 running;
} gl_view;

static hk_lock g_thread_lock = HK_LOCK_INIT;
static hk_cond g_thread_wake = HK_COND_INIT;

static struct {
    volatile i32 wakes;                /* counted up by every requestRender: a thread waits until it moves */
    gl_view views[MAX_GL_THREADS];
    bool roots;
    i32 numbers;
} T;

N(Native_requestRender)
{
    (void)c;
    hk_lock_acquire(&g_thread_lock);
    T.wakes++;
    hk_cond_broadcast(&g_thread_wake);
    hk_lock_release(&g_thread_lock);
}

static void gl_wait(i32 seen)
{
    hk_lock_acquire(&g_thread_lock);
    /* (A second at most: a wake lost by some other path costs no more than that.) */
    if (T.wakes == seen) hk_cond_wait_ns(&g_thread_wake, &g_thread_lock, 1000000000ull);
    hk_lock_release(&g_thread_lock);
}

/* MeglerEGL's own answers on this thread: the thread's context and surface (0 for none), so that EGL called by the renderer finds them. */
static void tell_egl(gl_view *g, gl_ctx *x, gl_surf *s)
{
    hk_jvalue args[6];
    memset(args, 0, sizeof(args));
    args[0].j = (i64)(hk_addr)x;
    args[1].j = (i64)(hk_addr)s;
    args[2].i = s ? s->width : 0;
    args[3].i = s ? s->height : 0;
    args[4].i = x ? x->version : 0;
    args[5].l = g->view;
    if (!hk_dvm_call_static("javax/microedition/khronos/egl/MeglerEGL", "meglerRenderThread", "(JJIIILandroid/view/SurfaceView;)V", args, NULL)) hk_dvm_clear_pending();
}

static void gl_main(void *arg)
{
    gl_view *g = (gl_view *)arg;
    char name[32];
    gl_ctx *x = NULL;
    gl_surf *s = NULL;
    layer *l = NULL;
    int w = 0, h = 0;
    bool created = false, changed = false;
    hk_jobj *thrown = NULL;
    hk_jvalue v, args[3];
    /* A thread of the app's: its native code may run on it (a renderer's onDrawFrame calling the app's library). */
    hk_gthread_self();
    hk_dvm_self();
    snprintf(name, sizeof(name), "GLThread %d", g->number);
    hk_dvm_thread_attach(name, NULL, false);
    t_render = true;
    memset(args, 0, sizeof(args));
    /* What the renderer is handed: the GL10 (the app's wrapper's, if it set one) and the configuration its chooser chose. */
    v.j = 0;
    if (hk_dvm_call_virtual(g->view, "meglerRendererGL", "()Ljavax/microedition/khronos/opengles/GL;", NULL, &v)) g->gl10 = v.l;
    v.j = 0;
    if (hk_dvm_pending() == NULL && hk_dvm_call_virtual(g->view, "meglerConfig", "()Ljavax/microedition/khronos/egl/EGLConfig;", NULL, &v)) g->config = v.l;
    if (hk_dvm_pending()) goto end;
    x = ctx_new(g->version, NULL);
    if (!x) {
        hk_warn("java: %s: no OpenGL ES %d context, so the renderer of %s is not started", name, g->version, hk_dvm_class_name(g->view));
        goto end;
    }
    make_current(NULL, x);
    tell_egl(g, x, NULL);
    hk_info("java: %s draws the renderer %s of %s, with OpenGL ES %d", name, hk_dvm_class_name(g->renderer), hk_dvm_class_name(g->view), x->version);
    for (;;) {
        i32 seen = T.wakes, sw, sh;
        i64 state;
        v.j = 0;
        /* The events queued for this thread run here, inside meglerFrame. */
        if (!hk_dvm_call_virtual(g->view, "meglerFrame", "()J", NULL, &v)) break;
        state = v.j;
        if (state & FRAME_EXIT) break;
        if (state & (FRAME_NO_SURFACE | FRAME_PAUSED)) {
            /* As Android's GLThread: the surface goes when the view's does and on pause, and the context too on pause unless the app asked to keep it. */
            if (s) {
                make_current(NULL, x);
                surf_free(s);
                s = NULL;
                tell_egl(g, x, NULL);
            }
            if ((state & FRAME_PAUSED) && !(state & FRAME_PRESERVE) && x) {
                make_current(NULL, NULL);
                ctx_free(x);
                x = NULL;
                created = false;
                tell_egl(g, NULL, NULL);
            }
            gl_wait(seen);
            continue;
        }
        sw = (i32)((state >> 32) & 0xFFFF);
        sh = (i32)((state >> 16) & 0xFFFF);
        if (!x) {
            x = ctx_new(g->version, NULL);
            if (!x) break;
            make_current(NULL, x);
            created = false;
            tell_egl(g, x, NULL);
        }
        if (!s || sw != w || sh != h) {
            /* The new surface before the old one goes: the view's layer, where it is and its last frame, stay meanwhile. */
            gl_surf *old = s;
            hk_lock_acquire(&g_window_lock);
            l = layer_of(g->view, true);
            hk_lock_release(&g_window_lock);
            s = surf_new(sw, sh, l);
            if (old) {
                make_current(NULL, x);
                surf_free(old);
            }
            if (!s) break;
            w = sw;
            h = sh;
            changed = true;
            make_current(s, x);
            tell_egl(g, x, s);
            /* The view keeps its area clear from now on: the frame is under it. */
            args[0].j = 0;
            args[0].z = 1;
            if (!hk_dvm_call_virtual(g->view, "meglerSetGL", "(Z)V", args, NULL)) break;
        }
        if (!created) {
            args[0].l = g->gl10;
            args[1].l = g->config;
            if (!hk_dvm_call_virtual(g->renderer, "onSurfaceCreated", "(Ljavax/microedition/khronos/opengles/GL10;Ljavax/microedition/khronos/egl/EGLConfig;)V", args, NULL))
                break;
            created = true;
        }
        if (changed) {
            args[0].l = g->gl10;
            args[1].j = 0;
            args[1].i = w;
            args[2].j = 0;
            args[2].i = h;
            if (!hk_dvm_call_virtual(g->renderer, "onSurfaceChanged", "(Ljavax/microedition/khronos/opengles/GL10;II)V", args, NULL)) break;
            changed = false;
        }
        if (state & FRAME_DRAW) {
            args[0].l = g->gl10;
            if (!hk_dvm_call_virtual(g->renderer, "onDrawFrame", "(Ljavax/microedition/khronos/opengles/GL10;)V", args, NULL)) break;
            /* (The renderer may have made another context current: the frame is this one's.) */
            make_current(s, x);
            swap(s, x);
        } else {
            gl_wait(seen);
        }
    }
end:
    /* What the renderer threw ends the thread as an exception nothing catches ends any of the app's threads; first the thread's own things go. */
    thrown = hk_dvm_pending();
    if (thrown) {
        char what[300];
        hk_dvm_describe_pending(what, sizeof(what));
        hk_warn("java: %s ends: the renderer threw %s", name, what);
        hk_dvm_clear_pending();
    }
    if (s) {
        make_current(NULL, x);
        surf_free(s);
    }
    if (x) {
        make_current(NULL, NULL);
        ctx_free(x);
    }
    tell_egl(g, NULL, NULL);
    hk_lock_acquire(&g_window_lock);
    if (C.first_swap) stats_line(NULL, C.first_swap, hk_time_monotonic_ns());
    hk_lock_release(&g_window_lock);
    hk_info("java: %s ended", name);
    g->view = g->renderer = g->gl10 = g->config = NULL;
    if (thrown) hk_dvm_throw_object(thrown);
    t_render = false;
    hk_dvm_thread_detach();
    hk_atomic_xchg(&g->running, 0);
}

/* startGL(GLSurfaceView view, Renderer renderer, int esVersion): the view's renderer, on a GL thread of its own. */
N(Native_startGL)
{
    gl_view *g = NULL;
    hk_thread *t;
    int i;
    hk_lock_acquire(&g_thread_lock);
    if (!T.roots) {
        for (i = 0; i < MAX_GL_THREADS; i++) {
            hk_dvm_add_root(&T.views[i].view);
            hk_dvm_add_root(&T.views[i].renderer);
            hk_dvm_add_root(&T.views[i].gl10);
            hk_dvm_add_root(&T.views[i].config);
        }
        T.roots = true;
    }
    for (i = 0; i < MAX_GL_THREADS && !g; i++)
        if (!T.views[i].running) g = &T.views[i];
    if (g) {
        memset(g, 0, sizeof(*g));
        g->view = c->args[0].l;
        g->renderer = c->args[1].l;
        g->version = c->args[2].i;
        g->number = ++T.numbers;
        g->running = 1;
    }
    hk_lock_release(&g_thread_lock);
    if (!g) {
        hk_warn("java: more than %d GLSurfaceViews draw at once: this one's renderer is not started", MAX_GL_THREADS);
        return;
    }
    hk_lock_acquire(&g_window_lock);
    C.stats.threads++;
    hk_lock_release(&g_window_lock);
    /* Android gives a thread of the app's a megabyte; this layer's own frames are on it too. */
    t = hk_thread_start(gl_main, g, 2u << 20, "GLThread");
    if (t) {
        hk_thread_detach(t);
    } else {
        hk_warn("java: no thread could be started for a GLSurfaceView's renderer");
        g->view = g->renderer = NULL;
        hk_atomic_xchg(&g->running, 0);
    }
}

/* ---------------------------------------------------------------- tables */

#define G(cls, name, sig, fn) { "android/opengl/" cls, name, sig, fn }

static const hk_jhle k_natives[] = {
    { "megler/Native", "startGL", NULL, Native_startGL },
    { "megler/Native", "requestRender", NULL, Native_requestRender },
    { "megler/EGLNative", "createContext", "(IJ)J", EN_createContext },
    { "megler/EGLNative", "destroyContext", "(J)V", EN_destroyContext },
    { "megler/EGLNative", "createSurface", "(II)J", EN_createSurface },
    { "megler/EGLNative", "createWindowSurface", "(Ljava/lang/Object;II)J", EN_createWindowSurface },
    { "megler/EGLNative", "destroySurface", "(J)V", EN_destroySurface },
    { "megler/EGLNative", "makeCurrent", "(JJ)Z", EN_makeCurrent },
    { "megler/EGLNative", "swap", "(JJIIII)Z", EN_swap },
    { "megler/EGLNative", "isRenderThread", "()Z", EN_isRenderThread },
    { "megler/EGLNative", "hide", "()V", EN_hide },
    { "megler/EGLNative", "place", "(Ljava/lang/Object;IIII)V", EN_place },
    { "megler/EGLNative", "forget", "(Ljava/lang/Object;)V", EN_forget },
    { "megler/EGLNative", "glesVersion", "()I", EN_glesVersion },
    /* The GLES natives that are no plain call. */
    G("GLES10", "_nativeClassInit", "()V", gl_nothing),
    G("GLES10Ext", "_nativeClassInit", "()V", gl_nothing),
    G("GLES11", "_nativeClassInit", "()V", gl_nothing),
    G("GLES11Ext", "_nativeClassInit", "()V", gl_nothing),
    G("GLES30", "_nativeClassInit", "()V", gl_nothing),
    G("GLES31", "_nativeClassInit", "()V", gl_nothing),
    G("GLES32", "_nativeClassInit", "()V", gl_nothing),
    G("GLES10", "glGetString", "(I)Ljava/lang/String;", gl_GetString),
    G("GLES20", "glGetString", "(I)Ljava/lang/String;", gl_GetString),
    G("GLES20", "glShaderSource", "(ILjava/lang/String;)V", gl_ShaderSource),
    G("GLES20", "glGetProgramInfoLog", "(I)Ljava/lang/String;", gl_GetProgramInfoLog),
    G("GLES20", "glGetShaderInfoLog", "(I)Ljava/lang/String;", gl_GetShaderInfoLog),
    G("GLES20", "glGetShaderSource", "(I)Ljava/lang/String;", gl_GetShaderSource),
    G("GLES20", "glGetActiveAttrib", "(II[II[II)Ljava/lang/String;", gl_GetActiveAttrib_a),
    G("GLES20", "glGetActiveAttrib", "(IILjava/nio/IntBuffer;Ljava/nio/IntBuffer;)Ljava/lang/String;", gl_GetActiveAttrib_b),
    G("GLES20", "glGetActiveUniform", "(II[II[II)Ljava/lang/String;", gl_GetActiveUniform_a),
    G("GLES20", "glGetActiveUniform", "(IILjava/nio/IntBuffer;Ljava/nio/IntBuffer;)Ljava/lang/String;", gl_GetActiveUniform_b),
    G("GLES30", "glGetActiveUniformBlockName", "(II)Ljava/lang/String;", gl_GetActiveUniformBlockName_s),
    G("GLES30", "glGetActiveUniformBlockName", "(IILjava/nio/Buffer;Ljava/nio/Buffer;)V", gl_GetActiveUniformBlockName_b),
    G("GLES30", "glGetBufferPointerv", "(II)Ljava/nio/Buffer;", gl_GetBufferPointerv),
    G("GLES30", "glGetStringi", "(II)Ljava/lang/String;", gl_GetStringi),
    G("GLES30", "glGetTransformFeedbackVarying", "(IILjava/nio/IntBuffer;Ljava/nio/IntBuffer;)Ljava/lang/String;", gl_GetTransformFeedbackVarying_b),
    G("GLES30", "glGetTransformFeedbackVarying", "(II[II[II)Ljava/lang/String;", gl_GetTransformFeedbackVarying_a),
    G("GLES30", "glGetTransformFeedbackVarying", "(IIILjava/nio/IntBuffer;Ljava/nio/IntBuffer;Ljava/nio/IntBuffer;B)V", gl_GetTransformFeedbackVarying_byte),
    G("GLES30", "glGetUniformIndices", "(I[Ljava/lang/String;Ljava/nio/IntBuffer;)V", gl_GetUniformIndices_b),
    G("GLES30", "glGetUniformIndices", "(I[Ljava/lang/String;[II)V", gl_GetUniformIndices_a),
    G("GLES30", "glMapBufferRange", "(IIII)Ljava/nio/Buffer;", gl_MapBufferRange),
    G("GLES30", "glTransformFeedbackVaryings", "(I[Ljava/lang/String;I)V", gl_TransformFeedbackVaryings),
    G("GLES31", "glCreateShaderProgramv", "(I[Ljava/lang/String;)I", gl_CreateShaderProgramv),
    G("GLES31", "glGetProgramPipelineInfoLog", "(I)Ljava/lang/String;", gl_GetProgramPipelineInfoLog),
    G("GLES31", "glGetProgramResourceName", "(III)Ljava/lang/String;", gl_GetProgramResourceName),
    G("GLES32", "glDebugMessageCallback", "(Landroid/opengl/GLES32$DebugProc;)V", gl_not_yet),
    G("GLES32", "glGetDebugMessageLog", "(ILjava/nio/IntBuffer;Ljava/nio/IntBuffer;Ljava/nio/IntBuffer;Ljava/nio/IntBuffer;Ljava/nio/IntBuffer;Ljava/nio/ByteBuffer;)I",
      gl_GetDebugMessageLog_b),
    G("GLES32", "glGetDebugMessageLog", "(ILjava/nio/IntBuffer;Ljava/nio/IntBuffer;Ljava/nio/IntBuffer;Ljava/nio/IntBuffer;)[Ljava/lang/String;", gl_not_yet),
    G("GLES32", "glGetDebugMessageLog", "(I[II[II[II[II)[Ljava/lang/String;", gl_not_yet),
    G("GLES32", "glGetObjectLabel", "(II)Ljava/lang/String;", gl_not_yet),
    G("GLES32", "glGetObjectPtrLabel", "(J)Ljava/lang/String;", gl_not_yet),
    G("GLES32", "glGetPointerv", "(I)J", gl_not_yet),
    G("GLES32", "glObjectPtrLabel", "(JLjava/lang/String;)V", gl_not_yet),
    { NULL, NULL, NULL, NULL },
};

const hk_jhle *hk_java_gl_natives(void) { return k_natives; }
