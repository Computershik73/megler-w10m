/*
 * EGL and OpenGL ES between two calling conventions: desktop Windows on x86_64 and x86 (docs/DESKTOP.md).
 *
 * Guest code calls EGL and OpenGL ES with Android's convention, and so does the core, whose C is of Android's ABI there.
 * ANGLE's functions are Windows' code, of Windows' convention: Microsoft's x64 one on x86_64, __stdcall on x86 (MG_GLAPI,
 * mg_base.h). So on those two nothing is given ANGLE's own function: mg_egl.c's angle() gives its adapter instead, which is
 * what guest code is bound to and what the core's own callers call -- a function of the guest's convention that calls
 * ANGLE's. tools/gl-adapters.py makes one for every entry point of the NDK's headers (mg_gl_adapters.inc), and its table.
 *
 * A function that takes a function of its caller's is written here: ANGLE would call that one with Windows' convention.
 * ANGLE is given a function of this file's instead, of that convention -- MG_GLCB for what the headers call through a
 * GL_APIENTRY or EGLAPIENTRY pointer, MG_HOSTCB for EGL_ANDROID_blob_cache's, which they give no convention -- and it calls
 * the caller's:
 *
 *   - OpenGL ES's debug message callback (GL_KHR_debug, OpenGL ES 3.2) comes with a pointer of the caller's, which ANGLE
 *     passes back with each message: the callback and that pointer are kept as a pair, and ANGLE is given the pair;
 *   - EGL_KHR_debug's callback is the process's one;
 *   - EGL_ANDROID_blob_cache's two functions are a display's, and ANGLE calls them with nothing that tells which display's
 *     they are: each display that is given some has a pair of this file's own, of a few.
 *
 * What ANGLE gives back of them -- glGetPointerv's GL_DEBUG_CALLBACK_FUNCTION and GL_DEBUG_CALLBACK_USER_PARAM,
 * eglQueryDebugKHR's EGL_DEBUG_CALLBACK_KHR -- is given back as the caller gave it.
 *
 * ARM64's two conventions are one for every call that is not variadic, and no function of EGL's or OpenGL ES's is: there
 * ANGLE's functions are given as they are. 32-bit ARM's floats have adapters of their own (mg_egl.c).
 */
#include "mg_egl.h"

#if MG_GL_ADAPTERS

#include "../mg_core.h"
#include "../host/mg_host.h"

typedef struct mg_gl_entry {
    const char *name;
    void *adapter;                     /* a_<name>: what the guest and the core call */
    void **host;                       /* h_<name>: ANGLE's function, which the adapter calls */
    u8 arg_bytes;                      /* on x86's stack */
} mg_gl_entry;

#include "mg_gl_adapters.inc"

static const mg_gl_entry *entry_of(const char *name)
{
    size_t lo = 0, hi = MG_COUNT(k_gl_entries);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, k_gl_entries[mid].name);
        if (!c) return &k_gl_entries[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

void *mg_gl_adapter(const char *name, void *host)
{
    const mg_gl_entry *e;
    if (!name || !host) return NULL;
    e = entry_of(name);
    if (!e) {
        mg_note_once("egl: ANGLE has %s, which none of the NDK's headers declares: it is not given (no adapter between the two conventions)", name);
        return NULL;
    }
    /* (Set again where it differs: a test that gives the core another OpenGL ES, mg_egl_set_provider.) */
    if (*e->host != host) *e->host = host;
    return e->adapter;
}

int mg_gl_adapter_arg_bytes(const char *name)
{
    const mg_gl_entry *e = name ? entry_of(name) : NULL;
    return e ? e->arg_bytes : -1;
}

enum {
    GL_DEBUG_CALLBACK_FUNCTION = 0x8244, GL_DEBUG_CALLBACK_USER_PARAM = 0x8245,
    EGL_SUCCESS = 0x3000, EGL_DEBUG_CALLBACK_KHR = 0x33B8,
};

/* ------------------------------------------- OpenGL ES's debug message callback */

typedef void (*guest_debug_proc)(u32 source, u32 type, u32 id, u32 severity, i32 length, const char *message, const void *user);
typedef struct debug_pair {
    struct debug_pair *next;
    guest_debug_proc proc;
    const void *user;
} debug_pair;
/* Kept for good: ANGLE may yet pass one back after its context was given another. One per distinct callback and pointer. */
static debug_pair *g_debug_pairs;
static mg_lock g_debug_lock = MG_LOCK_INIT;

static MG_GLCB void debug_message(u32 source, u32 type, u32 id, u32 severity, i32 length, const char *message, const void *user)
{
    const debug_pair *p = (const debug_pair *)user;
    if (p && p->proc) p->proc(source, type, id, severity, length, message, p->user);
}

static debug_pair *debug_pair_of(guest_debug_proc proc, const void *user)
{
    debug_pair *p;
    mg_lock_acquire(&g_debug_lock);
    for (p = g_debug_pairs; p && !(p->proc == proc && p->user == user); p = p->next) {}
    if (!p) {
        p = (debug_pair *)mg_alloc(sizeof(*p));
        p->proc = proc;
        p->user = user;
        p->next = g_debug_pairs;
        g_debug_pairs = p;
    }
    mg_lock_release(&g_debug_lock);
    return p;
}

static const debug_pair *known_pair(const void *user)
{
    debug_pair *p;
    if (!user) return NULL;
    mg_lock_acquire(&g_debug_lock);
    for (p = g_debug_pairs; p && (const void *)p != user; p = p->next) {}
    mg_lock_release(&g_debug_lock);
    return p;
}

/* The caller's callback, or none (NULL: messages are not called back any more). */
static void debug_callback(void (MG_GLAPI *set)(void *, const void *), void *callback, const void *user)
{
    if (callback) set((void *)debug_message, debug_pair_of((guest_debug_proc)callback, user));
    else set(NULL, user);
}

static void a_glDebugMessageCallback(void *callback, const void *userParam) { debug_callback(h_glDebugMessageCallback, callback, userParam); }
static void a_glDebugMessageCallbackKHR(void *callback, const void *userParam) { debug_callback(h_glDebugMessageCallbackKHR, callback, userParam); }

/* GL_DEBUG_CALLBACK_FUNCTION and GL_DEBUG_CALLBACK_USER_PARAM as the caller gave them; anything else as ANGLE has it. */
static void get_pointer(void (MG_GLAPI *get)(u32, void *), u32 pname, void *params)
{
    void **out = (void **)params;
    const debug_pair *p;
    get(pname, params);
    if (!out) return;
    if (pname == GL_DEBUG_CALLBACK_FUNCTION && *out == (void *)debug_message) {
        void *user = NULL;
        get(GL_DEBUG_CALLBACK_USER_PARAM, &user);
        p = known_pair(user);
        *out = p ? (void *)p->proc : NULL;
    } else if (pname == GL_DEBUG_CALLBACK_USER_PARAM && (p = known_pair(*out)) != NULL) {
        *out = (void *)p->user;
    }
}

static void a_glGetPointerv(u32 pname, void *params) { get_pointer(h_glGetPointerv, pname, params); }
static void a_glGetPointervKHR(u32 pname, void *params) { get_pointer(h_glGetPointervKHR, pname, params); }

/* -------------------------------------------------- EGL_KHR_debug's callback */

typedef void (*guest_egl_debug_proc)(u32 error, const char *command, i32 type, void *thread_label, void *object_label, const char *message);
static guest_egl_debug_proc volatile g_egl_debug;

static MG_GLCB void egl_debug_message(u32 error, const char *command, i32 type, void *thread_label, void *object_label, const char *message)
{
    guest_egl_debug_proc p = g_egl_debug;
    if (p) p(error, command, type, thread_label, object_label, message);
}

static i32 a_eglDebugMessageControlKHR(void *callback, const void *attrib_list)
{
    guest_egl_debug_proc was = g_egl_debug;
    i32 r;
    g_egl_debug = (guest_egl_debug_proc)callback;
    r = h_eglDebugMessageControlKHR(callback ? (void *)egl_debug_message : NULL, attrib_list);
    /* Refused (EGL_BAD_ATTRIBUTE): the callback is as it was. */
    if (r != EGL_SUCCESS) g_egl_debug = was;
    return r;
}

static u32 a_eglQueryDebugKHR(i32 attribute, void *value)
{
    intptr_t *out = (intptr_t *)value;
    u32 ok = h_eglQueryDebugKHR(attribute, value);
    if (ok && out && attribute == EGL_DEBUG_CALLBACK_KHR && *out == (intptr_t)egl_debug_message) *out = (intptr_t)g_egl_debug;
    return ok;
}

/* ------------------------------------------- EGL_ANDROID_blob_cache's functions */

typedef void (*guest_set_blob)(const void *key, intptr_t key_size, const void *value, intptr_t value_size);
typedef intptr_t (*guest_get_blob)(const void *key, intptr_t key_size, void *value, intptr_t value_size);
#define BLOB_DISPLAYS 4
static struct {
    void *display;
    guest_set_blob set;
    guest_get_blob get;
} g_blob[BLOB_DISPLAYS];
static mg_lock g_blob_lock = MG_LOCK_INIT;

#define BLOB_PAIR(n)                                                                                                         \
    static MG_HOSTCB void blob_set_##n(const void *k, intptr_t ks, const void *v, intptr_t vs) { g_blob[n].set(k, ks, v, vs); } \
    static MG_HOSTCB intptr_t blob_get_##n(const void *k, intptr_t ks, void *v, intptr_t vs) { return g_blob[n].get(k, ks, v, vs); }
BLOB_PAIR(0)
BLOB_PAIR(1)
BLOB_PAIR(2)
BLOB_PAIR(3)
static void *const k_blob_set[BLOB_DISPLAYS] = { (void *)blob_set_0, (void *)blob_set_1, (void *)blob_set_2, (void *)blob_set_3 };
static void *const k_blob_get[BLOB_DISPLAYS] = { (void *)blob_get_0, (void *)blob_get_1, (void *)blob_get_2, (void *)blob_get_3 };

static void a_eglSetBlobCacheFuncsANDROID(void *dpy, void *set, void *get)
{
    int i, at = -1;
    if (!set || !get) {
        /* (EGL_BAD_PARAMETER: ANGLE's to say.) */
        h_eglSetBlobCacheFuncsANDROID(dpy, set, get);
        return;
    }
    mg_lock_acquire(&g_blob_lock);
    for (i = 0; i < BLOB_DISPLAYS && at < 0; i++)
        if (g_blob[i].display == dpy) at = i;
    for (i = 0; i < BLOB_DISPLAYS && at < 0; i++)
        if (!g_blob[i].display) {
            g_blob[i].set = (guest_set_blob)set;
            g_blob[i].get = (guest_get_blob)get;
            g_blob[i].display = dpy;
            at = i;
        }
    mg_lock_release(&g_blob_lock);
    if (at < 0) {
        mg_warn("egl: eglSetBlobCacheFuncsANDROID on more displays than this layer has functions for (%d): not passed on", BLOB_DISPLAYS);
        return;
    }
    /* A display given them before keeps the first: ANGLE refuses the second (EGL_BAD_PARAMETER), as EGL has it. */
    h_eglSetBlobCacheFuncsANDROID(dpy, k_blob_set[at], k_blob_get[at]);
}

#endif /* MG_GL_ADAPTERS */
