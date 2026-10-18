/*
 * A stand-in for EGL and OpenGL ES that draws: hk_gl_standin.h says what it does and does not. Its functions are the
 * host's own, as ANGLE's are: floats are floats.
 */
#include "hk_gl_standin.h"

#include <stdio.h>

#include "../src/host/hk_host.h"

#define MAX_OBJECTS 256
#define MAX_VARS 16

enum {
    GL_COLOR_BUFFER_BIT = 0x4000, GL_TRIANGLES = 4, GL_TRIANGLE_STRIP = 5, GL_TRIANGLE_FAN = 6, GL_FLOAT = 0x1406, GL_RGBA = 0x1908,
    GL_BGRA_EXT = 0x80E1, GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C, GL_VERTEX_SHADER = 0x8B31, GL_COMPILE_STATUS = 0x8B81, GL_LINK_STATUS = 0x8B82,
    GL_VALIDATE_STATUS = 0x8B83, GL_INFO_LOG_LENGTH = 0x8B84, GL_SHADER_SOURCE_LENGTH = 0x8B88, GL_SHADER_TYPE = 0x8B4F,
    GL_ACTIVE_UNIFORMS = 0x8B86, GL_ACTIVE_UNIFORM_MAX_LENGTH = 0x8B87, GL_ACTIVE_ATTRIBUTES = 0x8B89, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH = 0x8B8A,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_PIXEL_PACK_BUFFER = 0x88EB, GL_FRAMEBUFFER = 0x8D40,
    GL_READ_FRAMEBUFFER = 0x8CA8, GL_DRAW_FRAMEBUFFER = 0x8CA9, GL_PACK_ALIGNMENT = 0x0D05, GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_MAX_TEXTURE_SIZE = 0x0D33, GL_VIEWPORT = 0x0BA2, GL_FRAMEBUFFER_BINDING = 0x8CA6, GL_READ_FRAMEBUFFER_BINDING = 0x8CAA,
    GL_PIXEL_PACK_BUFFER_BINDING = 0x88ED, GL_CURRENT_PROGRAM = 0x8B8D, GL_MAX_VERTEX_ATTRIBS = 0x8869, GL_ARRAY_BUFFER_BINDING = 0x8894,
    GL_TEXTURE_BINDING_2D = 0x8069, GL_INVALID_ENUM = 0x0500, GL_INVALID_VALUE = 0x0501, GL_INVALID_OPERATION = 0x0502,
    GL_FLOAT_VEC2 = 0x8B50, GL_FLOAT_VEC3 = 0x8B51, GL_FLOAT_VEC4 = 0x8B52, GL_FLOAT_MAT4 = 0x8B5C, GL_INT = 0x1404, GL_SAMPLER_2D = 0x8B5E,
};

typedef struct sb_surface {
    int w, h;
    u8 *px;                            /* R, G, B, A, rows bottom first */
    int km;                            /* its keyed mutex, as ANGLE's: 0 released (key 0), 1 held by this OpenGL ES, 2 by a shell */
} sb_surface;

/* What this OpenGL ES does with a surface whose keyed mutex it does not hold: nothing, as Direct3D drops it. */
static bool held(const sb_surface *s) { return s && s->km == 1; }

/* A texture's pixels, R, G, B, A, rows in the order given (bottom first, as OpenGL has it); shared by the EGL images of it. */
typedef struct sb_store {
    int w, h, refs;
    u8 *px;
} sb_store;

typedef struct sb_image {
    sb_store *store;
} sb_image;

typedef struct sb_attr {
    bool on;
    i32 size, stride;
    u32 type, buffer;
    const void *ptr;
} sb_attr;

typedef struct sb_context {
    int version;
    float clear[4];
    i32 viewport[4];
    u32 program, array_buffer, pack_buffer, texture;
    i32 fb, read_fb, pack_align, unpack_align;
    u32 error;
    sb_attr attr[MAX_VARS];
} sb_context;

typedef struct sb_var {
    char name[48];
    u32 type;
    i32 location;
} sb_var;

static hk_lock g_lock = HK_LOCK_INIT;

static struct {
    bool bgra, es1;
    hk_gl_standin_stats stats;
    struct { bool alive; u32 type; char *source; } shaders[MAX_OBJECTS];
    struct {
        bool alive, linked;
        u32 shaders[4];
        int nshaders;
        sb_var attrs[MAX_VARS], uniforms[MAX_VARS];
        int nattrs, nuniforms;
        i32 bound[MAX_VARS];           /* glBindAttribLocation: a location for the attribute named in bound_names */
        char bound_names[MAX_VARS][48];
        int nbound;
        float values[MAX_VARS][16];
    } programs[MAX_OBJECTS];
    struct { bool alive; u8 *data; size_t size; } buffers[MAX_OBJECTS];
    struct { bool alive; int w, h; sb_store *store; } textures[MAX_OBJECTS];
    struct { bool alive; u32 texture; } framebuffers[MAX_OBJECTS];   /* a framebuffer object's colour texture */
} S;

static HK_THREAD_LOCAL sb_context *t_ctx;
static HK_THREAD_LOCAL sb_surface *t_surf;
static bool g_complete;
static bool complete_integer(u32 name, i32 *v);

/* ------------------------------------------------------------------- EGL */

static void *e_GetPlatformDisplay(unsigned platform, void *native, const int *attribs) { (void)platform; (void)native; (void)attribs; return (void *)(hk_addr)0xD15; }
static void *e_GetDisplay(void *native) { (void)native; return (void *)(hk_addr)0xD15; }
static unsigned e_Initialize(void *d, int *major, int *minor)
{
    (void)d;
    if (major) *major = 1;
    if (minor) *minor = 5;
    return 1;
}
static unsigned e_True(void) { return 1; }
static unsigned e_Terminate(void *d) { (void)d; return 1; }
static unsigned e_BindAPI(unsigned api) { (void)api; return 1; }
static int e_GetError(void) { return 0x3000; }
static const char *e_QueryString(void *d, int name)
{
    (void)d;
    return name == 0x3055 ? "EGL_KHR_no_config_context EGL_KHR_surfaceless_context EGL_KHR_create_context EGL_ANGLE_query_surface_pointer "
                            "EGL_ANGLE_surface_d3d_texture_2d_share_handle EGL_KHR_image_base EGL_KHR_gl_texture_2D_image"
           : name == 0x3053 ? "Megler (stand-in)"
                            : "1.5 (stand-in)";
}
/*
 * EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE: the surface itself stands for its texture's shared handle (hk_gl_standin_shared).
 * EGL_DXGI_KEYED_MUTEX_ANGLE: its keyed mutex, which every surface has, as every pbuffer of gecko-w10m's ANGLE has.
 */
static unsigned e_QuerySurfacePointer(void *d, void *surface, int attribute, void **value)
{
    (void)d;
    if (!surface || (attribute != 0x3200 && attribute != 0x33A2) || !value) return 0;
    *value = attribute == 0x3200 ? surface : (void *)&((sb_surface *)surface)->km;
    return 1;
}

/* A keyed mutex acquired (key 0, not waiting) or released by this OpenGL ES's own "device": the core's seam (ndk/hk_egl.c). */
static unsigned e_KeyedMutex(void *mutex, int acquire)
{
    int *km = (int *)mutex;
    unsigned ok;
    if (!km) return 0;
    hk_lock_acquire(&g_lock);
    ok = acquire ? *km == 0 : *km == 1;
    if (ok) *km = acquire ? 1 : 0;
    hk_lock_release(&g_lock);
    return ok;
}
static unsigned e_ChooseConfig(void *d, const int *attribs, void **configs, int size, int *num)
{
    (void)d; (void)attribs;
    if (configs && size > 0) configs[0] = (void *)(hk_addr)0xC0F;
    if (num) *num = 1;
    return 1;
}
static unsigned e_GetConfigAttrib(void *d, void *c, int attrib, int *value)
{
    (void)d; (void)c;
    switch (attrib) {
    case 0x3024: case 0x3023: case 0x3022: case 0x3021: *value = 8; break;    /* red, green, blue, alpha */
    case 0x3025: *value = 24; break;                                          /* depth */
    case 0x3026: *value = 8; break;                                           /* stencil */
    case 0x3033: *value = 5; break;                                           /* window and pbuffer */
    case 0x3040: case 0x3042: *value = 0x45; break;                           /* renderable and conformant: OpenGL ES 1, 2, 3 */
    case 0x302E: *value = 1; break;
    /* And the rest of what a config says, as a driver's one config of 8888 with depth and stencil would. */
    case 0x3020: *value = 32; break;                                          /* buffer size */
    case 0x303F: *value = 0x308E; break;                                      /* colour buffer type: RGB */
    case 0x3027: case 0x3034: *value = 0x3038; break;                         /* caveat, transparency: none */
    case 0x3028: *value = 1; break;                                           /* config id */
    case 0x3039: case 0x303A: *value = 1; break;                              /* binds to RGB and RGBA textures */
    case 0x303C: *value = 1; break;                                           /* max swap interval */
    case 0x302C: case 0x302A: *value = 4096; break;                           /* max pbuffer width, height */
    case 0x302B: *value = 4096 * 4096; break;                                 /* max pbuffer pixels */
    default: *value = 0;
    }
    return 1;
}
static void *e_CreateContext(void *d, void *c, void *share, const int *attribs)
{
    sb_context *x;
    int version = 1, i;
    (void)d; (void)c; (void)share;
    for (i = 0; attribs && attribs[i] != 0x3038; i += 2)
        if (attribs[i] == 0x3098) version = attribs[i + 1];
    hk_lock_acquire(&g_lock);
    if (version == 1 && !S.es1) {
        S.stats.refused_es1++;
        hk_lock_release(&g_lock);
        return NULL;
    }
    /* The complete mode is a driver of OpenGL ES 2.0 alone (see there). */
    if (version >= 3 && g_complete) {
        hk_lock_release(&g_lock);
        return NULL;
    }
    x = (sb_context *)calloc(1, sizeof(*x));
    x->version = version;
    x->pack_align = x->unpack_align = 4;
    S.stats.contexts++;
    S.stats.contexts_alive++;
    if (version > S.stats.largest_es) S.stats.largest_es = version;
    hk_lock_release(&g_lock);
    return x;
}
static unsigned e_DestroyContext(void *d, void *ctx)
{
    (void)d;
    if (!ctx) return 0;
    hk_lock_acquire(&g_lock);
    S.stats.contexts_alive--;
    hk_lock_release(&g_lock);
    if (t_ctx == ctx) t_ctx = NULL;
    free(ctx);
    return 1;
}
static void *e_CreatePbuffer(void *d, void *c, const int *attribs)
{
    sb_surface *s = (sb_surface *)calloc(1, sizeof(*s));
    int i;
    (void)d; (void)c;
    s->w = s->h = 1;
    for (i = 0; attribs && attribs[i] != 0x3038; i += 2) {
        if (attribs[i] == 0x3057) s->w = attribs[i + 1];
        if (attribs[i] == 0x3056) s->h = attribs[i + 1];
    }
    s->px = (u8 *)calloc((size_t)s->w * (size_t)s->h, 4);
    hk_lock_acquire(&g_lock);
    S.stats.surfaces++;
    S.stats.surfaces_alive++;
    hk_lock_release(&g_lock);
    return s;
}
static unsigned e_DestroySurface(void *d, void *surface)
{
    sb_surface *s = (sb_surface *)surface;
    (void)d;
    if (!s) return 0;
    hk_lock_acquire(&g_lock);
    S.stats.surfaces_alive--;
    hk_lock_release(&g_lock);
    if (t_surf == s) t_surf = NULL;
    free(s->px);
    free(s);
    return 1;
}
static unsigned e_MakeCurrent(void *d, void *draw, void *read, void *ctx)
{
    (void)d; (void)read;
    t_ctx = (sb_context *)ctx;
    t_surf = ctx ? (sb_surface *)draw : NULL;
    /* A context made current the first time gets the surface's size as its viewport, as OpenGL has it. */
    if (t_ctx && t_surf && !t_ctx->viewport[2]) {
        t_ctx->viewport[2] = t_surf->w;
        t_ctx->viewport[3] = t_surf->h;
    }
    return 1;
}
static void *e_GetCurrentContext(void) { return t_ctx; }
static unsigned e_SwapBuffers(void *d, void *s)
{
    (void)d; (void)s;
    hk_lock_acquire(&g_lock);
    S.stats.swaps++;
    hk_lock_release(&g_lock);
    return 1;
}
static unsigned e_SwapInterval(void *d, int interval) { (void)d; (void)interval; return 1; }
static unsigned e_QuerySurface(void *d, void *surface, int attrib, int *value)
{
    sb_surface *s = (sb_surface *)surface;
    (void)d;
    *value = !s ? 0 : attrib == 0x3057 ? s->w : attrib == 0x3056 ? s->h : 0;
    return s != NULL;
}

/* ---------------------------------------------------------------- shaders */

static void g_error(u32 e)
{
    if (t_ctx && !t_ctx->error) t_ctx->error = e;
}

static u32 g_CreateShader(u32 type)
{
    u32 i;
    hk_lock_acquire(&g_lock);
    for (i = 1; i < MAX_OBJECTS && S.shaders[i].alive; i++) { }
    if (i < MAX_OBJECTS) {
        S.shaders[i].alive = true;
        S.shaders[i].type = type;
        S.shaders[i].source = NULL;
    }
    hk_lock_release(&g_lock);
    return i < MAX_OBJECTS ? i : 0;
}
static void g_ShaderSource(u32 shader, i32 count, const char *const *text, const i32 *lengths)
{
    size_t n = 0;
    i32 i;
    char *s;
    if (shader >= MAX_OBJECTS || !S.shaders[shader].alive) {
        g_error(GL_INVALID_VALUE);
        return;
    }
    for (i = 0; i < count; i++) n += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(text[i]);
    s = (char *)malloc(n + 1);
    n = 0;
    for (i = 0; i < count; i++) {
        size_t k = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(text[i]);
        memcpy(s + n, text[i], k);
        n += k;
    }
    s[n] = 0;
    free(S.shaders[shader].source);
    S.shaders[shader].source = s;
}
static void g_CompileShader(u32 shader) { (void)shader; }
static void g_DeleteShader(u32 shader)
{
    if (shader < MAX_OBJECTS && S.shaders[shader].alive) {
        free(S.shaders[shader].source);
        memset(&S.shaders[shader], 0, sizeof(S.shaders[shader]));
    }
}
static const char *shader_log(u32 shader)
{
    static char log[64];
    snprintf(log, sizeof(log), "stand-in: shader %u compiled", (unsigned)shader);
    return log;
}
static void g_GetShaderiv(u32 shader, u32 name, i32 *v)
{
    if (shader >= MAX_OBJECTS || !S.shaders[shader].alive) {
        g_error(GL_INVALID_VALUE);
        return;
    }
    switch (name) {
    case GL_COMPILE_STATUS: *v = 1; break;
    case GL_SHADER_TYPE: *v = (i32)S.shaders[shader].type; break;
    case GL_INFO_LOG_LENGTH: *v = (i32)strlen(shader_log(shader)) + 1; break;
    case GL_SHADER_SOURCE_LENGTH: *v = S.shaders[shader].source ? (i32)strlen(S.shaders[shader].source) + 1 : 0; break;
    default: g_error(GL_INVALID_ENUM);
    }
}
static void copy_text(const char *text, i32 size, i32 *length, char *out)
{
    i32 n = (i32)strlen(text);
    if (size <= 0) {
        if (length) *length = 0;
        return;
    }
    if (n > size - 1) n = size - 1;
    memcpy(out, text, (size_t)n);
    out[n] = 0;
    if (length) *length = n;
}
static void g_GetShaderInfoLog(u32 shader, i32 size, i32 *length, char *out) { copy_text(shader_log(shader), size, length, out); }
static void g_GetShaderSource(u32 shader, i32 size, i32 *length, char *out)
{
    copy_text(shader < MAX_OBJECTS && S.shaders[shader].source ? S.shaders[shader].source : "", size, length, out);
}

/* ---------------------------------------------------------------- programs */

static u32 g_CreateProgram(void)
{
    u32 i;
    hk_lock_acquire(&g_lock);
    for (i = 1; i < MAX_OBJECTS && S.programs[i].alive; i++) { }
    if (i < MAX_OBJECTS) {
        memset(&S.programs[i], 0, sizeof(S.programs[i]));
        S.programs[i].alive = true;
    }
    hk_lock_release(&g_lock);
    return i < MAX_OBJECTS ? i : 0;
}
static bool program_ok(u32 p)
{
    if (p < MAX_OBJECTS && S.programs[p].alive) return true;
    g_error(GL_INVALID_VALUE);
    return false;
}
static void g_AttachShader(u32 p, u32 shader)
{
    if (program_ok(p) && S.programs[p].nshaders < 4) S.programs[p].shaders[S.programs[p].nshaders++] = shader;
}
static void g_DetachShader(u32 p, u32 shader) { (void)p; (void)shader; }
static void g_DeleteProgram(u32 p)
{
    if (p < MAX_OBJECTS) S.programs[p].alive = false;
}
static void g_BindAttribLocation(u32 p, u32 index, const char *name)
{
    if (!program_ok(p) || S.programs[p].nbound >= MAX_VARS) return;
    S.programs[p].bound[S.programs[p].nbound] = (i32)index;
    snprintf(S.programs[p].bound_names[S.programs[p].nbound++], 48, "%s", name);
}

static u32 type_of(const char *word)
{
    static const struct { const char *name; u32 type; } k[] = {
        { "float", GL_FLOAT }, { "vec2", GL_FLOAT_VEC2 }, { "vec3", GL_FLOAT_VEC3 }, { "vec4", GL_FLOAT_VEC4 }, { "mat4", GL_FLOAT_MAT4 },
        { "int", GL_INT }, { "sampler2D", GL_SAMPLER_2D },
    };
    size_t i;
    for (i = 0; i < HK_COUNT(k); i++)
        if (!strcmp(k[i].name, word)) return k[i].type;
    return GL_FLOAT_VEC4;
}

/* The source's declarations: "attribute|uniform|in [precision] type name;" -- one a statement, which is all the tests write. */
static void declarations(sb_var *attrs, int *nattrs, sb_var *uniforms, int *nuniforms, const char *source, bool vertex)
{
    const char *p = source;
    while (p && *p) {
        char words[4][48];
        int n = 0, k;
        const char *end = strchr(p, ';');
        if (!end) break;
        while (p < end && n < 4) {
            size_t len = 0;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            while (p + len < end && p[len] != ' ' && p[len] != '\t' && p[len] != '\n' && p[len] != '\r' && p[len] != '[') len++;
            if (!len) break;
            snprintf(words[n++], 48, "%.*s", (int)len, p);
            p += len;
            while (p < end && *p == '[') p = end;
        }
        p = end + 1;
        if (n < 3) continue;
        k = !strcmp(words[1], "lowp") || !strcmp(words[1], "mediump") || !strcmp(words[1], "highp") ? 2 : 1;
        if (k + 1 >= n) continue;
        if (!strcmp(words[0], "uniform") && *nuniforms < MAX_VARS) {
            int i, seen = 0;
            for (i = 0; i < *nuniforms; i++) seen |= !strcmp(uniforms[i].name, words[k + 1]);
            if (seen) continue;
            snprintf(uniforms[*nuniforms].name, 48, "%s", words[k + 1]);
            uniforms[*nuniforms].type = type_of(words[k]);
            uniforms[*nuniforms].location = *nuniforms;
            (*nuniforms)++;
        } else if ((!strcmp(words[0], "attribute") || (vertex && !strcmp(words[0], "in"))) && *nattrs < MAX_VARS) {
            snprintf(attrs[*nattrs].name, 48, "%s", words[k + 1]);
            attrs[*nattrs].type = type_of(words[k]);
            attrs[*nattrs].location = -1;
            (*nattrs)++;
        }
    }
}

static void g_LinkProgram(u32 p)
{
    int i, j, next = 0;
    if (!program_ok(p)) return;
    S.programs[p].nattrs = S.programs[p].nuniforms = 0;
    for (i = 0; i < S.programs[p].nshaders; i++) {
        u32 s = S.programs[p].shaders[i];
        if (s < MAX_OBJECTS && S.shaders[s].alive)
            declarations(S.programs[p].attrs, &S.programs[p].nattrs, S.programs[p].uniforms, &S.programs[p].nuniforms, S.shaders[s].source,
                         S.shaders[s].type == GL_VERTEX_SHADER);
    }
    /* Attributes bound by name keep their location; the others take the next free one. */
    for (i = 0; i < S.programs[p].nattrs; i++)
        for (j = 0; j < S.programs[p].nbound; j++)
            if (!strcmp(S.programs[p].bound_names[j], S.programs[p].attrs[i].name)) S.programs[p].attrs[i].location = S.programs[p].bound[j];
    for (i = 0; i < S.programs[p].nattrs; i++) {
        if (S.programs[p].attrs[i].location >= 0) continue;
        for (;;) {
            bool taken = false;
            for (j = 0; j < S.programs[p].nattrs; j++) taken |= S.programs[p].attrs[j].location == next;
            if (!taken) break;
            next++;
        }
        S.programs[p].attrs[i].location = next++;
    }
    S.programs[p].linked = true;
}
static const char *program_log(u32 p)
{
    static char log[64];
    snprintf(log, sizeof(log), "stand-in: program %u linked", (unsigned)p);
    return log;
}
static int max_len(int n, const char *s)
{
    int k = (int)strlen(s);
    return k > n ? k : n;
}

static void g_GetProgramiv(u32 p, u32 name, i32 *v)
{
    int i, longest = 0;
    if (!program_ok(p)) return;
    switch (name) {
    case GL_LINK_STATUS: *v = S.programs[p].linked; break;
    case GL_VALIDATE_STATUS: *v = 1; break;
    case GL_INFO_LOG_LENGTH: *v = (i32)strlen(program_log(p)) + 1; break;
    case GL_ACTIVE_ATTRIBUTES: *v = S.programs[p].nattrs; break;
    case GL_ACTIVE_UNIFORMS: *v = S.programs[p].nuniforms; break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
        for (i = 0; i < S.programs[p].nattrs; i++) longest = max_len(longest, S.programs[p].attrs[i].name);
        *v = longest + 1;
        break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH:
        for (i = 0; i < S.programs[p].nuniforms; i++) longest = max_len(longest, S.programs[p].uniforms[i].name);
        *v = longest + 1;
        break;
    default: g_error(GL_INVALID_ENUM);
    }
}
static void g_GetProgramInfoLog(u32 p, i32 size, i32 *length, char *out) { copy_text(program_log(p), size, length, out); }
static void g_UseProgram(u32 p)
{
    if (t_ctx) t_ctx->program = p;
}
static void g_ValidateProgram(u32 p) { (void)p; }
static i32 g_GetAttribLocation(u32 p, const char *name)
{
    int i;
    if (!program_ok(p)) return -1;
    for (i = 0; i < S.programs[p].nattrs; i++)
        if (!strcmp(S.programs[p].attrs[i].name, name)) return S.programs[p].attrs[i].location;
    return -1;
}
static i32 g_GetUniformLocation(u32 p, const char *name)
{
    int i;
    if (!program_ok(p)) return -1;
    for (i = 0; i < S.programs[p].nuniforms; i++)
        if (!strcmp(S.programs[p].uniforms[i].name, name)) return S.programs[p].uniforms[i].location;
    return -1;
}
static void active(const sb_var *v, int n, u32 index, i32 size, i32 *length, i32 *count, u32 *type, char *name)
{
    if ((int)index >= n) {
        g_error(GL_INVALID_VALUE);
        return;
    }
    copy_text(v[index].name, size, length, name);
    if (count) *count = 1;
    if (type) *type = v[index].type;
}
static void g_GetActiveAttrib(u32 p, u32 index, i32 size, i32 *length, i32 *count, u32 *type, char *name)
{
    if (program_ok(p)) active(S.programs[p].attrs, S.programs[p].nattrs, index, size, length, count, type, name);
}
static void g_GetActiveUniform(u32 p, u32 index, i32 size, i32 *length, i32 *count, u32 *type, char *name)
{
    if (program_ok(p)) active(S.programs[p].uniforms, S.programs[p].nuniforms, index, size, length, count, type, name);
}
static float *uniform(i32 location)
{
    u32 p = t_ctx ? t_ctx->program : 0;
    if (!p || p >= MAX_OBJECTS || location < 0 || location >= MAX_VARS) return NULL;
    return S.programs[p].values[location];
}
static void g_Uniform1f(i32 l, float a)
{
    float *u = uniform(l);
    if (u) u[0] = a;
}
static void g_Uniform1i(i32 l, i32 a)
{
    float *u = uniform(l);
    if (u) u[0] = (float)a;
}
static void g_Uniform4f(i32 l, float a, float b, float c, float d)
{
    float *u = uniform(l);
    if (!u) return;
    u[0] = a;
    u[1] = b;
    u[2] = c;
    u[3] = d;
}
static void g_Uniform4fv(i32 l, i32 count, const float *v)
{
    float *u = uniform(l);
    if (u && count > 0) memcpy(u, v, 4 * sizeof(float));
}
static void g_UniformMatrix4fv(i32 l, i32 count, u8 transpose, const float *v)
{
    float *u = uniform(l);
    (void)transpose;
    if (u && count > 0) memcpy(u, v, 16 * sizeof(float));
}

/* ---------------------------------------------------------- buffers, state */

static u32 new_name(int kind)
{
    u32 i;
    hk_lock_acquire(&g_lock);
    for (i = 1; i < MAX_OBJECTS && (kind == 2 ? S.framebuffers[i].alive : kind ? S.textures[i].alive : S.buffers[i].alive); i++) { }
    if (i < MAX_OBJECTS) {
        if (kind == 2) {
            S.framebuffers[i].alive = true;
            S.framebuffers[i].texture = 0;
        } else if (kind) {
            S.textures[i].alive = true;
        } else {
            S.buffers[i].alive = true;
        }
    }
    hk_lock_release(&g_lock);
    return i < MAX_OBJECTS ? i : 0;
}
static void g_GenBuffers(i32 n, u32 *names)
{
    i32 i;
    for (i = 0; i < n; i++) names[i] = new_name(0);
}
static void g_DeleteBuffers(i32 n, const u32 *names)
{
    i32 i;
    for (i = 0; i < n; i++)
        if (names[i] && names[i] < MAX_OBJECTS) {
            free(S.buffers[names[i]].data);
            memset(&S.buffers[names[i]], 0, sizeof(S.buffers[names[i]]));
        }
}
static void g_BindBuffer(u32 target, u32 name)
{
    if (!t_ctx) return;
    if (target == GL_ARRAY_BUFFER) t_ctx->array_buffer = name;
    else if (target == GL_PIXEL_PACK_BUFFER) t_ctx->pack_buffer = name;
    else if (target != GL_ELEMENT_ARRAY_BUFFER) g_error(GL_INVALID_ENUM);
}
static void g_BufferData(u32 target, intptr_t size, const void *data, u32 usage)
{
    u32 b = !t_ctx ? 0 : target == GL_ARRAY_BUFFER ? t_ctx->array_buffer : target == GL_PIXEL_PACK_BUFFER ? t_ctx->pack_buffer : 0;
    (void)usage;
    if (!b || b >= MAX_OBJECTS) {
        g_error(GL_INVALID_OPERATION);
        return;
    }
    free(S.buffers[b].data);
    S.buffers[b].data = (u8 *)calloc(1, size > 0 ? (size_t)size : 1);
    S.buffers[b].size = (size_t)size;
    if (data && size > 0) memcpy(S.buffers[b].data, data, (size_t)size);
}
static void g_GenTextures(i32 n, u32 *names)
{
    i32 i;
    for (i = 0; i < n; i++) names[i] = new_name(1);
}
static void store_release(sb_store *s)
{
    if (s && --s->refs == 0) {
        free(s->px);
        free(s);
    }
}
static void g_DeleteTextures(i32 n, const u32 *names)
{
    i32 i;
    hk_lock_acquire(&g_lock);
    for (i = 0; i < n; i++)
        if (names[i] && names[i] < MAX_OBJECTS) {
            S.textures[names[i]].alive = false;
            store_release(S.textures[names[i]].store);
            S.textures[names[i]].store = NULL;
        }
    hk_lock_release(&g_lock);
}
static void g_BindTexture(u32 target, u32 name)
{
    (void)target;
    if (t_ctx) t_ctx->texture = name;
}
/* Rows of RGBA, RGB or RGB 5-6-5 pixels, as the unpack alignment lays them out, into a store's R, G, B, A. */
static void store_put(sb_store *s, i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, const u8 *in)
{
    i32 bpp = type == 0x8363 ? 2 : format == GL_RGBA ? 4 : 3, align = t_ctx ? t_ctx->unpack_align : 4, r, c;
    size_t row = ((size_t)w * (size_t)bpp + (size_t)align - 1) / (size_t)align * (size_t)align;
    if (!in) return;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            const u8 *p = in + (size_t)r * row + (size_t)c * (size_t)bpp;
            u8 *o;
            if (x + c < 0 || y + r < 0 || x + c >= s->w || y + r >= s->h) continue;
            o = s->px + ((size_t)(y + r) * (size_t)s->w + (size_t)(x + c)) * 4;
            if (type == 0x8363) {
                u16 v = (u16)(p[0] | p[1] << 8);
                o[0] = (u8)((v >> 11) * 255 / 31);
                o[1] = (u8)(((v >> 5) & 63) * 255 / 63);
                o[2] = (u8)((v & 31) * 255 / 31);
                o[3] = 255;
            } else {
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                o[3] = bpp == 4 ? p[3] : 255;
            }
        }
}
static void g_TexImage2D(u32 target, i32 level, i32 internal, i32 w, i32 h, i32 border, u32 format, u32 type, const void *pixels)
{
    u32 t = t_ctx ? t_ctx->texture : 0;
    sb_store *s;
    (void)target; (void)level; (void)internal; (void)border;
    if (!t || t >= MAX_OBJECTS || w <= 0 || h <= 0) return;
    s = (sb_store *)calloc(1, sizeof(*s));
    s->w = w;
    s->h = h;
    s->refs = 1;
    s->px = (u8 *)calloc((size_t)w * (size_t)h, 4);
    store_put(s, 0, 0, w, h, format, type, (const u8 *)pixels);
    hk_lock_acquire(&g_lock);
    store_release(S.textures[t].store);
    S.textures[t].store = s;
    S.textures[t].w = w;
    S.textures[t].h = h;
    hk_lock_release(&g_lock);
}
static void g_TexSubImage2D(u32 target, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, const void *pixels)
{
    u32 t = t_ctx ? t_ctx->texture : 0;
    (void)target; (void)level;
    if (!t || t >= MAX_OBJECTS || !S.textures[t].store) {
        g_error(GL_INVALID_OPERATION);
        return;
    }
    store_put(S.textures[t].store, x, y, w, h, format, type, (const u8 *)pixels);
}

/* EGL images of textures (EGL_KHR_gl_texture_2D_image): the texture's pixels, which a texture made of the image shares. */
static void *e_CreateImage(void *d, void *ctx, unsigned target, void *buffer, const int *attribs)
{
    u32 t = (u32)(hk_addr)buffer;
    sb_image *im = NULL;
    (void)d; (void)ctx; (void)attribs;
    hk_lock_acquire(&g_lock);
    if (target == 0x30B1 && t && t < MAX_OBJECTS && S.textures[t].alive && S.textures[t].store) {
        im = (sb_image *)calloc(1, sizeof(*im));
        im->store = S.textures[t].store;
        im->store->refs++;
    }
    hk_lock_release(&g_lock);
    return im;
}
static unsigned e_DestroyImage(void *d, void *image)
{
    sb_image *im = (sb_image *)image;
    (void)d;
    if (!im) return 0;
    hk_lock_acquire(&g_lock);
    store_release(im->store);
    hk_lock_release(&g_lock);
    free(im);
    return 1;
}
static void g_EGLImageTargetTexture2DOES(u32 target, void *image)
{
    sb_image *im = (sb_image *)image;
    u32 t = t_ctx ? t_ctx->texture : 0;
    (void)target;
    if (!im || !t || t >= MAX_OBJECTS) {
        g_error(GL_INVALID_OPERATION);
        return;
    }
    hk_lock_acquire(&g_lock);
    im->store->refs++;
    store_release(S.textures[t].store);
    S.textures[t].store = im->store;
    S.textures[t].w = im->store->w;
    S.textures[t].h = im->store->h;
    hk_lock_release(&g_lock);
}

/* Framebuffer objects with a texture for their colour: drawn into by glClear and read by glReadPixels. */
static volatile i32 g_next_name = 1000;   /* the complete mode's names (below) */
static void g_GenFramebuffers(i32 n, u32 *names)
{
    i32 i;
    /* Past the table's room (a WebView's GL makes many), names that draw nothing, as the complete mode's are. */
    for (i = 0; i < n; i++)
        if (!(names[i] = new_name(2))) names[i] = (u32)hk_atomic_add(&g_next_name, 1);
}
static void g_FramebufferTexture2D(u32 target, u32 attachment, u32 textarget, u32 texture, i32 level)
{
    i32 fb = t_ctx ? (target == GL_READ_FRAMEBUFFER ? t_ctx->read_fb : t_ctx->fb) : 0;
    (void)attachment; (void)textarget; (void)level;
    if (fb > 0 && fb < MAX_OBJECTS) S.framebuffers[fb].texture = texture;
}
static u32 g_CheckFramebufferStatus(u32 target) { (void)target; return 0x8CD5; }   /* GL_FRAMEBUFFER_COMPLETE */
/* The store of the colour texture of a framebuffer object, or NULL (none, or the window's). */
static sb_store *fb_store(i32 fb)
{
    u32 t;
    if (fb <= 0 || fb >= MAX_OBJECTS || !S.framebuffers[fb].alive) return NULL;
    t = S.framebuffers[fb].texture;
    return t && t < MAX_OBJECTS && S.textures[t].alive ? S.textures[t].store : NULL;
}
static u8 g_IsTexture(u32 name) { return name && name < MAX_OBJECTS && S.textures[name].alive; }
static void g_TexParameteri(u32 target, u32 name, i32 v) { (void)target; (void)name; (void)v; }
static void g_PixelStorei(u32 name, i32 v)
{
    if (!t_ctx) return;
    if (name == GL_PACK_ALIGNMENT) t_ctx->pack_align = v;
    else if (name == GL_UNPACK_ALIGNMENT) t_ctx->unpack_align = v;
}
static void g_BindFramebuffer(u32 target, u32 fb)
{
    if (!t_ctx) return;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) t_ctx->fb = (i32)fb;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) t_ctx->read_fb = (i32)fb;
}
static void g_OneEnum(u32 v) { (void)v; }
static void g_TwoEnums(u32 a, u32 b) { (void)a; (void)b; }
static void g_Nothing(void) { }
static void g_LineWidth(float w) { (void)w; }
static void g_DepthMask(u8 on) { (void)on; }
static void g_Viewport(i32 x, i32 y, i32 w, i32 h)
{
    if (!t_ctx) return;
    t_ctx->viewport[0] = x;
    t_ctx->viewport[1] = y;
    t_ctx->viewport[2] = w;
    t_ctx->viewport[3] = h;
}
static void g_Scissor(i32 x, i32 y, i32 w, i32 h) { (void)x; (void)y; (void)w; (void)h; }
static void g_ClearColor(float r, float g, float b, float a)
{
    if (!t_ctx) return;
    t_ctx->clear[0] = r;
    t_ctx->clear[1] = g;
    t_ctx->clear[2] = b;
    t_ctx->clear[3] = a;
}
static u8 byte_of(float v) { return (u8)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f)); }
static void g_Clear(u32 mask)
{
    size_t i, n;
    u8 c[4];
    sb_store *s = t_ctx ? fb_store(t_ctx->fb) : NULL;
    if (!t_ctx || (!t_surf && !s) || !(mask & GL_COLOR_BUFFER_BIT)) return;
    for (i = 0; i < 4; i++) c[i] = byte_of(t_ctx->clear[i]);
    if (s) {
        n = (size_t)s->w * (size_t)s->h;
        for (i = 0; i < n; i++) memcpy(s->px + 4 * i, c, 4);
    } else {
        if (!held(t_surf)) {
            hk_lock_acquire(&g_lock);
            S.stats.dropped++;
            hk_lock_release(&g_lock);
            return;
        }
        n = (size_t)t_surf->w * (size_t)t_surf->h;
        for (i = 0; i < n; i++) memcpy(t_surf->px + 4 * i, c, 4);
    }
    hk_lock_acquire(&g_lock);
    S.stats.clears++;
    memcpy(S.stats.clear, t_ctx->clear, sizeof(S.stats.clear));
    hk_lock_release(&g_lock);
}
static void g_VertexAttribPointer(u32 index, i32 size, u32 type, u8 normalized, i32 stride, const void *ptr)
{
    (void)normalized;
    if (!t_ctx || index >= MAX_VARS) {
        g_error(GL_INVALID_VALUE);
        return;
    }
    t_ctx->attr[index].size = size;
    t_ctx->attr[index].type = type;
    t_ctx->attr[index].stride = stride;
    t_ctx->attr[index].ptr = ptr;
    t_ctx->attr[index].buffer = t_ctx->array_buffer;
}
static void g_EnableVertexAttribArray(u32 index)
{
    if (t_ctx && index < MAX_VARS) t_ctx->attr[index].on = true;
}
static void g_DisableVertexAttribArray(u32 index)
{
    if (t_ctx && index < MAX_VARS) t_ctx->attr[index].on = false;
}

/* One triangle, its corners in clip space, filled where pixel centres are inside it. */
static void triangle(const float v[3][4], const u8 rgba[4])
{
    float px[3], py[3], area;
    i32 x0, y0, x1, y1, X, Y, i;
    for (i = 0; i < 3; i++) {
        float w = v[i][3] != 0 ? v[i][3] : 1;
        px[i] = (float)t_ctx->viewport[0] + (v[i][0] / w + 1) * 0.5f * (float)t_ctx->viewport[2];
        py[i] = (float)t_ctx->viewport[1] + (v[i][1] / w + 1) * 0.5f * (float)t_ctx->viewport[3];
    }
    area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    if (area == 0) return;
    x0 = (i32)(px[0] < px[1] ? (px[0] < px[2] ? px[0] : px[2]) : (px[1] < px[2] ? px[1] : px[2]));
    x1 = (i32)(px[0] > px[1] ? (px[0] > px[2] ? px[0] : px[2]) : (px[1] > px[2] ? px[1] : px[2])) + 1;
    y0 = (i32)(py[0] < py[1] ? (py[0] < py[2] ? py[0] : py[2]) : (py[1] < py[2] ? py[1] : py[2]));
    y1 = (i32)(py[0] > py[1] ? (py[0] > py[2] ? py[0] : py[2]) : (py[1] > py[2] ? py[1] : py[2])) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t_surf->w) x1 = t_surf->w;
    if (y1 > t_surf->h) y1 = t_surf->h;
    for (Y = y0; Y < y1; Y++)
        for (X = x0; X < x1; X++) {
            float cx = (float)X + 0.5f, cy = (float)Y + 0.5f;
            float w0 = (px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy);
            float w1 = (px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy);
            float w2 = (px[0] - cx) * (py[1] - cy) - (px[1] - cx) * (py[0] - cy);
            if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0))
                memcpy(t_surf->px + ((size_t)Y * (size_t)t_surf->w + (size_t)X) * 4, rgba, 4);
        }
}

static void g_DrawArrays(u32 mode, i32 first, i32 count)
{
    u32 p;
    int i, position = 0, colour = -1;
    const sb_attr *a;
    const u8 *base;
    i32 stride;
    u8 rgba[4] = { 255, 255, 255, 255 };
    if (!t_ctx || !t_surf) return;
    if (!held(t_surf)) {
        hk_lock_acquire(&g_lock);
        S.stats.dropped++;
        hk_lock_release(&g_lock);
        return;
    }
    p = t_ctx->program;
    if (p && p < MAX_OBJECTS) {
        for (i = 0; i < S.programs[p].nattrs; i++)
            if (strstr(S.programs[p].attrs[i].name, "osition")) position = S.programs[p].attrs[i].location;
        for (i = 0; i < S.programs[p].nuniforms; i++)
            if (strstr(S.programs[p].uniforms[i].name, "olor")) colour = S.programs[p].uniforms[i].location;
        if (colour >= 0)
            for (i = 0; i < 4; i++) rgba[i] = byte_of(S.programs[p].values[colour][i]);
    }
    if (position < 0 || position >= MAX_VARS) return;
    a = &t_ctx->attr[position];
    if (!a->on || a->type != GL_FLOAT || a->size < 2) {
        g_error(GL_INVALID_OPERATION);
        return;
    }
    base = a->buffer ? (a->buffer < MAX_OBJECTS && S.buffers[a->buffer].data ? S.buffers[a->buffer].data + (size_t)(hk_addr)a->ptr : NULL) : (const u8 *)a->ptr;
    if (!base) return;
    stride = a->stride ? a->stride : a->size * 4;
    for (i = 0; i + 2 < count; i += mode == GL_TRIANGLES ? 3 : 1) {
        float v[3][4];
        int k, c;
        for (k = 0; k < 3; k++) {
            int at = mode == GL_TRIANGLE_FAN && k == 0 ? first : first + i + k;
            float f[4] = { 0, 0, 0, 1 };
            memcpy(f, base + (size_t)at * (size_t)stride, (size_t)(a->size > 4 ? 4 : a->size) * 4);
            for (c = 0; c < 4; c++) v[k][c] = f[c];
        }
        if (mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP && mode != GL_TRIANGLE_FAN) break;
        triangle((const float(*)[4])v, rgba);
    }
    hk_lock_acquire(&g_lock);
    S.stats.draws++;
    hk_lock_release(&g_lock);
}

/* Reads into a pixel pack buffer when one is bound, and garbage from a framebuffer object: the core must leave neither for its own reads. */
static void g_ReadPixels(i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, void *data)
{
    u8 *out = (u8 *)data;
    i32 r, c;
    sb_store *s = t_ctx ? fb_store(t_ctx->read_fb) : NULL;
    if (s && t_ctx && !t_ctx->pack_buffer && format == GL_RGBA && type == 0x1401) {
        /* A framebuffer object with a texture: its pixels. */
        for (r = 0; r < h; r++)
            for (c = 0; c < w; c++) {
                u8 *o = out + ((size_t)r * (size_t)w + (size_t)c) * 4;
                if (y + r < 0 || y + r >= s->h || x + c < 0 || x + c >= s->w) memset(o, 0, 4);
                else memcpy(o, s->px + ((size_t)(y + r) * (size_t)s->w + (size_t)(x + c)) * 4, 4);
            }
        hk_lock_acquire(&g_lock);
        S.stats.reads++;
        hk_lock_release(&g_lock);
        return;
    }
    if (!t_ctx || !t_surf) return;
    if (t_ctx->pack_buffer) {
        u32 b = t_ctx->pack_buffer;
        if (b >= MAX_OBJECTS || !S.buffers[b].data) return;
        out = S.buffers[b].data + (size_t)(hk_addr)data;
    }
    if ((format != GL_RGBA && !(format == GL_BGRA_EXT && S.bgra)) || type != 0x1401) {
        g_error(GL_INVALID_OPERATION);
        return;
    }
    if (!held(t_surf)) {
        /* (What Direct3D's dropped copy leaves in the staging texture ANGLE reads from: nothing.) */
        for (r = 0; r < h; r++) memset(out + (size_t)r * (size_t)w * 4, 0, (size_t)w * 4);
        hk_lock_acquire(&g_lock);
        S.stats.dropped++;
        hk_lock_release(&g_lock);
        return;
    }
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            u8 *o = out + ((size_t)r * (size_t)w + (size_t)c) * 4;
            const u8 *s = t_surf->px + ((size_t)(y + r) * (size_t)t_surf->w + (size_t)(x + c)) * 4;
            if (y + r < 0 || y + r >= t_surf->h || x + c < 0 || x + c >= t_surf->w) {
                memset(o, 0, 4);
            } else if (t_ctx->read_fb) {
                memset(o, 0xEE, 4);
            } else if (format == GL_BGRA_EXT) {
                o[0] = s[2];
                o[1] = s[1];
                o[2] = s[0];
                o[3] = s[3];
            } else {
                memcpy(o, s, 4);
            }
        }
    hk_lock_acquire(&g_lock);
    S.stats.reads++;
    hk_lock_release(&g_lock);
}

static u32 g_GetError(void)
{
    u32 e = t_ctx ? t_ctx->error : 0;
    if (t_ctx) t_ctx->error = 0;
    return e;
}

static const char *extensions(void) { return S.bgra ? "GL_EXT_read_format_bgra GL_OES_rgb8_rgba8 GL_OES_depth24" : "GL_OES_rgb8_rgba8 GL_OES_depth24"; }

static const u8 *g_GetString(u32 name)
{
    if (!t_ctx) return NULL;
    switch (name) {
    case GL_VENDOR: return (const u8 *)"Megler";
    case GL_RENDERER: return (const u8 *)"stand-in for OpenGL ES";
    case GL_VERSION: return (const u8 *)(t_ctx->version >= 3 ? "OpenGL ES 3.0 (stand-in)" : "OpenGL ES 2.0 (stand-in)");
    case GL_SHADING_LANGUAGE_VERSION: return (const u8 *)"OpenGL ES GLSL ES 1.00 (stand-in)";
    case GL_EXTENSIONS: return (const u8 *)extensions();
    default: g_error(GL_INVALID_ENUM); return NULL;
    }
}

static void g_GetIntegerv(u32 name, i32 *v)
{
    if (!t_ctx) return;
    switch (name) {
    case GL_MAX_TEXTURE_SIZE: *v = 4096; break;
    case GL_MAX_VERTEX_ATTRIBS: *v = MAX_VARS; break;
    case GL_VIEWPORT: memcpy(v, t_ctx->viewport, sizeof(t_ctx->viewport)); break;
    case GL_FRAMEBUFFER_BINDING: *v = t_ctx->fb; break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = t_ctx->read_fb; break;
    case GL_PACK_ALIGNMENT: *v = t_ctx->pack_align; break;
    case GL_UNPACK_ALIGNMENT: *v = t_ctx->unpack_align; break;
    case GL_PIXEL_PACK_BUFFER_BINDING: *v = (i32)t_ctx->pack_buffer; break;
    case GL_CURRENT_PROGRAM: *v = (i32)t_ctx->program; break;
    case GL_ARRAY_BUFFER_BINDING: *v = (i32)t_ctx->array_buffer; break;
    case GL_TEXTURE_BINDING_2D: *v = (i32)t_ctx->texture; break;
    default:
        if (!(g_complete && complete_integer(name, v))) *v = 0;
    }
}

static void g_GetFloatv(u32 name, float *v)
{
    if (t_ctx && name == 0x0C22) memcpy(v, t_ctx->clear, sizeof(t_ctx->clear));   /* GL_COLOR_CLEAR_VALUE */
    else if (v) v[0] = 0;
}

/* ---------------------------------------------------------- complete mode
 *
 * For an app that brings a GL stack of its own and builds it on the system's EGL and OpenGL ES: the WebView's Chromium,
 * whose ANGLE runs its OpenGL ES back end over libEGL.so and libGLESv2.so, loads every entry point of EGL 1.4 and OpenGL ES
 * 2.0 by name, creates its context and queries what the driver can do, all before a page is shown -- and its pages are
 * drawn in software here, so nothing of that GL draws. In this mode (hk_gl_standin_set_complete) the stand-in is a driver
 * of OpenGL ES 2.0 only with every function there: the ones above as they are, the rest doing nothing, names made by the
 * glGen* calls, an OpenGL ES 2.0 device's limits answered, framebuffers complete, shader precision highp. The EGL
 * functions that were missing are there in either mode: they are EGL 1.4's.
 */

static unsigned e_GetConfigs(void *d, void **configs, int size, int *num)
{
    (void)d;
    if (configs && size > 0) configs[0] = (void *)(hk_addr)0xC0F;
    if (num) *num = 1;
    return 1;
}
static void *e_GetCurrentSurface(int which) { (void)which; return t_surf; }
static void *e_GetCurrentDisplay(void) { return t_ctx ? (void *)(hk_addr)0xD15 : NULL; }
/* A window surface is a surface of the window's size where the window says one, 1 x 1 otherwise: nothing shows it. */
static void *e_CreateWindowSurface(void *d, void *c, void *window, const int *attribs)
{
    (void)window;
    return e_CreatePbuffer(d, c, attribs);
}
static unsigned e_SurfaceThing(void *d, void *s, int a) { (void)d; (void)s; (void)a; return 1; }
static unsigned e_SurfaceAttrib(void *d, void *s, int a, int v) { (void)d; (void)s; (void)a; (void)v; return 1; }
static unsigned e_QueryContext(void *d, void *ctx, int attrib, int *value)
{
    (void)d;
    if (!ctx || !value) return 0;
    *value = attrib == 0x3098 ? ((sb_context *)ctx)->version : attrib == 0x3028 ? 0xC0F : attrib == 0x3086 ? 0x3084 : 0;
    return 1;
}
static unsigned e_QueryAPI(void) { return 0x30A0; }   /* EGL_OPENGL_ES_API */
static unsigned e_WaitNative(int engine) { (void)engine; return 1; }

static u32 g_Inert(void) { return 0; }
static void g_Gen(i32 n, u32 *out)
{
    i32 i;
    for (i = 0; out && i < n; i++) out[i] = (u32)hk_atomic_add(&g_next_name, 1);
}
static void g_GetBooleanv(u32 name, u8 *v) { (void)name; if (v) v[0] = 0; }
static void g_GetParamiv3(u32 a, u32 b, i32 *v) { (void)a; (void)b; if (v) v[0] = 0; }
static void g_GetParamiv4(u32 a, u32 b, u32 c, i32 *v) { (void)a; (void)b; (void)c; if (v) v[0] = 0; }
static void g_GetShaderPrecisionFormat(u32 shader, u32 precision, i32 *range, i32 *bits)
{
    (void)shader;
    /* The low, medium and high float and int precisions of a device that has highp everywhere. */
    if (range) range[0] = range[1] = precision >= 0x8DF3 ? 31 : 127;
    if (bits) *bits = precision >= 0x8DF3 ? 0 : 23;
}
static u32 g_IsSomething(u32 name) { return name != 0; }

/* The complete mode's answers to glGetIntegerv beyond the drawing stand-in's own. */
static bool complete_integer(u32 name, i32 *v)
{
    switch (name) {
    case 0x851C: case 0x84E8: *v = 4096; return true;           /* MAX_CUBE_MAP_TEXTURE_SIZE, MAX_RENDERBUFFER_SIZE */
    case 0x0D3A: v[0] = v[1] = 4096; return true;               /* MAX_VIEWPORT_DIMS */
    case 0x8872: case 0x8B4C: *v = 16; return true;             /* MAX_TEXTURE_IMAGE_UNITS, MAX_VERTEX_TEXTURE_IMAGE_UNITS */
    case 0x8B4D: *v = 32; return true;                          /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */
    case 0x8DFB: *v = 256; return true;                         /* MAX_VERTEX_UNIFORM_VECTORS */
    case 0x8DFD: *v = 224; return true;                         /* MAX_FRAGMENT_UNIFORM_VECTORS */
    case 0x8DFC: *v = 15; return true;                          /* MAX_VARYING_VECTORS */
    case 0x0D50: *v = 4; return true;                           /* SUBPIXEL_BITS */
    case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: case 0x0D57: *v = 8; return true;   /* RED..ALPHA_BITS, STENCIL_BITS */
    case 0x0D56: *v = 24; return true;                          /* DEPTH_BITS */
    case 0x8B9B: *v = GL_RGBA; return true;                     /* IMPLEMENTATION_COLOR_READ_FORMAT */
    case 0x8B9A: *v = 0x1401; return true;                      /* IMPLEMENTATION_COLOR_READ_TYPE: UNSIGNED_BYTE */
    case 0x846D: case 0x846E: v[0] = v[1] = 1; return true;     /* ALIASED_POINT_SIZE_RANGE, ALIASED_LINE_WIDTH_RANGE */
    default: return false;
    }
}

static void *complete_fn(const char *name)
{
    static const struct { const char *name; void *fn; } k[] = {
        { "glGenRenderbuffers", (void *)g_Gen }, { "glGenVertexArrays", (void *)g_Gen },
        { "glGenVertexArraysOES", (void *)g_Gen }, { "glGenQueries", (void *)g_Gen }, { "glGenQueriesEXT", (void *)g_Gen },
        { "glGenSamplers", (void *)g_Gen }, { "glGenTransformFeedbacks", (void *)g_Gen },
        { "glGetBooleanv", (void *)g_GetBooleanv },
        { "glGetShaderPrecisionFormat", (void *)g_GetShaderPrecisionFormat },
        { "glGetRenderbufferParameteriv", (void *)g_GetParamiv3 }, { "glGetTexParameteriv", (void *)g_GetParamiv3 },
        { "glGetTexParameterfv", (void *)g_GetParamiv3 }, { "glGetBufferParameteriv", (void *)g_GetParamiv3 },
        { "glGetVertexAttribiv", (void *)g_GetParamiv3 }, { "glGetVertexAttribfv", (void *)g_GetParamiv3 },
        { "glGetFramebufferAttachmentParameteriv", (void *)g_GetParamiv4 },
        { "glIsBuffer", (void *)g_IsSomething }, { "glIsFramebuffer", (void *)g_IsSomething }, { "glIsRenderbuffer", (void *)g_IsSomething },
        { "glIsProgram", (void *)g_IsSomething }, { "glIsShader", (void *)g_IsSomething },
    };
    size_t i;
    for (i = 0; i < HK_COUNT(k); i++)
        if (!strcmp(k[i].name, name)) return k[i].fn;
    /* Everything else of OpenGL ES 2.0 and its extensions: done, and nothing comes of it. */
    return name[0] == 'g' && name[1] == 'l' ? (void *)g_Inert : NULL;
}

void hk_gl_standin_set_complete(bool on) { g_complete = on; }

/* ------------------------------------------------------------- the table */

void *hk_gl_standin(const char *name)
{
    static const struct { const char *name; void *fn; } k[] = {
        { "eglGetPlatformDisplayEXT", (void *)e_GetPlatformDisplay }, { "eglGetDisplay", (void *)e_GetDisplay }, { "eglInitialize", (void *)e_Initialize },
        { "eglTerminate", (void *)e_Terminate }, { "eglGetError", (void *)e_GetError }, { "eglQueryString", (void *)e_QueryString },
        { "eglChooseConfig", (void *)e_ChooseConfig }, { "eglGetConfigAttrib", (void *)e_GetConfigAttrib }, { "eglCreateContext", (void *)e_CreateContext },
        { "eglDestroyContext", (void *)e_DestroyContext }, { "eglCreatePbufferSurface", (void *)e_CreatePbuffer },
        { "eglDestroySurface", (void *)e_DestroySurface }, { "eglMakeCurrent", (void *)e_MakeCurrent }, { "eglGetCurrentContext", (void *)e_GetCurrentContext },
        { "eglSwapBuffers", (void *)e_SwapBuffers }, { "eglSwapInterval", (void *)e_SwapInterval }, { "eglQuerySurface", (void *)e_QuerySurface },
        { "eglQuerySurfacePointerANGLE", (void *)e_QuerySurfacePointer }, { "meglerKeyedMutexANGLE", (void *)e_KeyedMutex },
        { "eglBindAPI", (void *)e_BindAPI }, { "eglReleaseThread", (void *)e_True }, { "eglWaitClient", (void *)e_True },
        { "eglGetConfigs", (void *)e_GetConfigs }, { "eglGetCurrentSurface", (void *)e_GetCurrentSurface },
        { "eglGetCurrentDisplay", (void *)e_GetCurrentDisplay }, { "eglCreateWindowSurface", (void *)e_CreateWindowSurface },
        { "eglBindTexImage", (void *)e_SurfaceThing }, { "eglReleaseTexImage", (void *)e_SurfaceThing },
        { "eglSurfaceAttrib", (void *)e_SurfaceAttrib }, { "eglQueryContext", (void *)e_QueryContext }, { "eglQueryAPI", (void *)e_QueryAPI },
        { "eglWaitGL", (void *)e_True }, { "eglWaitNative", (void *)e_WaitNative },
        { "glCreateShader", (void *)g_CreateShader }, { "glShaderSource", (void *)g_ShaderSource }, { "glCompileShader", (void *)g_CompileShader },
        { "glDeleteShader", (void *)g_DeleteShader }, { "glGetShaderiv", (void *)g_GetShaderiv }, { "glGetShaderInfoLog", (void *)g_GetShaderInfoLog },
        { "glGetShaderSource", (void *)g_GetShaderSource }, { "glCreateProgram", (void *)g_CreateProgram }, { "glAttachShader", (void *)g_AttachShader },
        { "glDetachShader", (void *)g_DetachShader }, { "glDeleteProgram", (void *)g_DeleteProgram }, { "glBindAttribLocation", (void *)g_BindAttribLocation },
        { "glLinkProgram", (void *)g_LinkProgram }, { "glGetProgramiv", (void *)g_GetProgramiv }, { "glGetProgramInfoLog", (void *)g_GetProgramInfoLog },
        { "glUseProgram", (void *)g_UseProgram }, { "glValidateProgram", (void *)g_ValidateProgram }, { "glGetAttribLocation", (void *)g_GetAttribLocation },
        { "glGetUniformLocation", (void *)g_GetUniformLocation }, { "glGetActiveAttrib", (void *)g_GetActiveAttrib },
        { "glGetActiveUniform", (void *)g_GetActiveUniform }, { "glUniform1f", (void *)g_Uniform1f }, { "glUniform1i", (void *)g_Uniform1i },
        { "glUniform4f", (void *)g_Uniform4f }, { "glUniform4fv", (void *)g_Uniform4fv }, { "glUniformMatrix4fv", (void *)g_UniformMatrix4fv },
        { "glGenBuffers", (void *)g_GenBuffers }, { "glDeleteBuffers", (void *)g_DeleteBuffers }, { "glBindBuffer", (void *)g_BindBuffer },
        { "glBufferData", (void *)g_BufferData }, { "glGenTextures", (void *)g_GenTextures }, { "glDeleteTextures", (void *)g_DeleteTextures },
        { "glBindTexture", (void *)g_BindTexture }, { "glTexImage2D", (void *)g_TexImage2D }, { "glIsTexture", (void *)g_IsTexture },
        { "glTexParameteri", (void *)g_TexParameteri }, { "glPixelStorei", (void *)g_PixelStorei }, { "glBindFramebuffer", (void *)g_BindFramebuffer },
        { "glEnable", (void *)g_OneEnum }, { "glDisable", (void *)g_OneEnum }, { "glActiveTexture", (void *)g_OneEnum }, { "glCullFace", (void *)g_OneEnum },
        { "glFrontFace", (void *)g_OneEnum }, { "glDepthFunc", (void *)g_OneEnum }, { "glBlendFunc", (void *)g_TwoEnums }, { "glHint", (void *)g_TwoEnums },
        { "glFlush", (void *)g_Nothing }, { "glFinish", (void *)g_Nothing }, { "glReleaseShaderCompiler", (void *)g_Nothing },
        { "glLineWidth", (void *)g_LineWidth }, { "glDepthMask", (void *)g_DepthMask }, { "glViewport", (void *)g_Viewport },
        { "glScissor", (void *)g_Scissor }, { "glClearColor", (void *)g_ClearColor }, { "glClear", (void *)g_Clear },
        { "glVertexAttribPointer", (void *)g_VertexAttribPointer }, { "glEnableVertexAttribArray", (void *)g_EnableVertexAttribArray },
        { "glDisableVertexAttribArray", (void *)g_DisableVertexAttribArray }, { "glDrawArrays", (void *)g_DrawArrays },
        { "glReadPixels", (void *)g_ReadPixels }, { "glGetError", (void *)g_GetError }, { "glGetString", (void *)g_GetString },
        { "glGetIntegerv", (void *)g_GetIntegerv }, { "glGetFloatv", (void *)g_GetFloatv },
        { "glTexSubImage2D", (void *)g_TexSubImage2D }, { "eglCreateImageKHR", (void *)e_CreateImage },
        { "eglDestroyImageKHR", (void *)e_DestroyImage }, { "glEGLImageTargetTexture2DOES", (void *)g_EGLImageTargetTexture2DOES },
        { "glGenFramebuffers", (void *)g_GenFramebuffers }, { "glFramebufferTexture2D", (void *)g_FramebufferTexture2D },
        { "glCheckFramebufferStatus", (void *)g_CheckFramebufferStatus },
    };
    size_t i;
    for (i = 0; i < HK_COUNT(k); i++)
        if (!strcmp(k[i].name, name)) return k[i].fn;
    return g_complete ? complete_fn(name) : NULL;
}

void hk_gl_standin_stats_get(hk_gl_standin_stats *out)
{
    hk_lock_acquire(&g_lock);
    *out = S.stats;
    hk_lock_release(&g_lock);
}

void hk_gl_standin_reset(bool bgra, bool es1)
{
    hk_lock_acquire(&g_lock);
    memset(&S.stats, 0, sizeof(S.stats));
    S.bgra = bgra;
    S.es1 = es1;
    hk_lock_release(&g_lock);
}

bool hk_gl_standin_keyed(void *share_handle, bool acquire)
{
    sb_surface *s = (sb_surface *)share_handle;
    bool ok;
    if (!s) return false;
    hk_lock_acquire(&g_lock);
    ok = acquire ? s->km == 0 : s->km == 2;
    if (ok) s->km = acquire ? 2 : 0;
    hk_lock_release(&g_lock);
    return ok;
}

bool hk_gl_standin_shared(void *share_handle, const u8 **rgba, int *width, int *height)
{
    const sb_surface *s = (const sb_surface *)share_handle;
    if (!s || !s->px) return false;
    *rgba = s->px;
    *width = s->w;
    *height = s->h;
    return true;
}

u32 hk_gl_standin_pixel(int x, int y)
{
    const u8 *p;
    if (!t_surf || x < 0 || y < 0 || x >= t_surf->w || y >= t_surf->h) return 0;
    p = t_surf->px + ((size_t)y * (size_t)t_surf->w + (size_t)x) * 4;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
