#include "hk_jni.h"

#include <stdio.h>

#include "../bionic/hk_bionic.h"
#include "../dvm/hk_dvm_internal.h"
#include "../ld/hk_ld.h"

/*
 * Two Java worlds meet in this file. One is the layer's own: classes that
 * exist because native code asked for them by name, whose methods are C
 * functions of a driver or nothing. The other is the Dalvik runtime's (dvm/):
 * classes a DEX file defines, whose methods are bytecode. Until the runtime is
 * started there is only the first, and nothing below costs anything. Once it
 * is, a class a DEX file defines is the runtime's -- what native code asks
 * about it is answered there (get_method, get_field, invoke, construct) --
 * every string and array is made there, and a class of the first kind gets a
 * stand-in there (a "foreign" class) so that its objects are objects bytecode
 * can hold. An hk_jclass says which it is in `dvm` and `foreign`.
 */

/* How many parameters a method called through this layer may have. */
#define JARGS 64

/* ---------------------------------------------------------------- classes */

typedef struct jmeth {
    hk_jclass *cls;
    char *name, *sig;
    bool is_static;
    hk_jhle_fn fn;
    char argk[JARGS];                  /* one kind letter per argument: Z B C S I J F D L */
    int nargs;
    char retk;
    bool warned;
    hk_dvm_method *dvm;                /* the runtime's method, for a class it has */
    struct jmeth *next;
} jmeth;

typedef struct jfield {
    hk_jclass *cls;
    char *name;
    char kind;
    bool is_static;
    hk_jvalue value;                   /* a static field's */
    bool has_value, warned;            /* a static field's: whether anything ever gave it one, and whether its absence has been said */
    hk_dvm_field *dvm;                 /* the runtime's field, for a class it has: its value is in the object's slots, or the class's */
    struct jfield *next;
} jfield;

typedef struct jnative {
    char *name, *sig;
    void *fn;
    struct jnative *next;
} jnative;

struct hk_jclass {
    char *name;
    hk_jclass *super;
    hk_jobj mirror;
    jmeth *meths;
    jfield *fields;
    jnative *natives;
    hk_dvm_class *dvm;                 /* while the runtime runs: its class of this name */
    bool foreign;                      /* ...which is only a stand-in: no DEX file defines the class, it is this layer's own */
    hk_jclass *next;
    hk_jclass *hash_next;
};

#define CLASS_BUCKETS 1024
static hk_jclass *g_classes;
static hk_jclass *g_class_hash[CLASS_BUCKETS];
static hk_lock g_jni_lock = HK_LOCK_INIT;
static const hk_jhle *g_tables[32];
static int g_ntables;
static HK_THREAD_LOCAL hk_jobj *t_pending;

void hk_jni_register(const hk_jhle *table)
{
    if (g_ntables < (int)HK_COUNT(g_tables)) g_tables[g_ntables++] = table;
}

static u32 name_hash(const char *name)
{
    u32 h = 2166136261u;
    for (; *name; name++) h = (h ^ (u8)*name) * 16777619u;
    return h % CLASS_BUCKETS;
}

static hk_jclass *find_class(const char *name)
{
    hk_jclass *c;
    for (c = g_class_hash[name_hash(name)]; c; c = c->hash_next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* The class of that name, made if it is new. Nothing else: not its superclass, not its place in the runtime. */
static hk_jclass *declare(const char *name)
{
    hk_jclass *c;
    u32 h = name_hash(name);
    hk_lock_acquire(&g_jni_lock);
    c = find_class(name);
    if (!c) {
        c = (hk_jclass *)hk_alloc(sizeof(*c));
        c->name = hk_strdup(name);
        c->mirror.refs = 1;
        c->mirror.kind = HK_J_CLASS;
        c->mirror.permanent = true;
        c->mirror.u.klass.jc = c;
        c->next = g_classes;
        g_classes = c;
        c->hash_next = g_class_hash[h];
        g_class_hash[h] = c;
    }
    hk_lock_release(&g_jni_lock);
    return c;
}

/*
 * Gives a class its place in the runtime, once that runs: the runtime's own
 * class when a DEX file defines the name (linked now; if that fails the
 * exception is left pending and the class stays unbound), a stand-in
 * otherwise. Never called with g_jni_lock held: linking comes back here
 * through jni_class_for.
 */
static void bind_class(hk_jclass *c)
{
    const char *e = c->name;
    hk_dvm_class *d;
    char inner[512];
    while (*e == '[') e++;
    if (e != c->name && *e == 'L') {
        /* An array of objects: its elements' class first, which may be one of this layer's own. */
        size_t n = strlen(e);
        if (n < 3 || n - 2 >= sizeof(inner)) return;
        memcpy(inner, e + 1, n - 2);
        inner[n - 2] = 0;
        if (!hk_jni_class(inner, NULL)->dvm) return;
    }
    if (e != c->name || hk_dvm_class_exists(c->name)) d = hk_dvm_find_class(c->name);
    else d = hk_dvm_foreign_class(c->name, c);
    if (!d) return;
    if (!d->jc) d->jc = c;
    c->foreign = d->foreign;
    c->dvm = d;
}

hk_jclass *hk_jni_class(const char *name, const char *super)
{
    hk_jclass *c = declare(name);
    if (super && !c->super && strcmp(name, super)) c->super = hk_jni_class(super, NULL);
    if (!c->mirror.cls && strcmp(name, "java/lang/Class")) c->mirror.cls = hk_jni_class("java/lang/Class", NULL);
    if (!c->dvm && g_dvm.running) bind_class(c);
    return c;
}

/* What the runtime asks when it has linked a class: this layer's class of that name, which from now on is that one. */
static hk_jclass *jni_class_for(const char *name, const char *super, hk_dvm_class *d)
{
    hk_jclass *c = declare(name);
    if (super && !c->super && strcmp(name, super)) c->super = declare(super);
    c->foreign = d->foreign;
    c->dvm = d;
    return c;
}

/* The java.lang.Class object: the runtime's while it runs, so that bytecode can be handed it. */
static hk_jobj *mirror_of(hk_jclass *c)
{
    hk_jobj *m;
    if (!c->dvm && g_dvm.running) bind_class(c);
    m = c->dvm ? hk_dvm_mirror(c->dvm) : NULL;
    return m ? m : &c->mirror;
}

/* Whether the runtime answers for the class: a DEX file defines it. */
static bool is_real(const hk_jclass *c) { return c && c->dvm && !c->foreign; }

hk_jobj *hk_jni_class_object(const char *name) { return mirror_of(hk_jni_class(name, NULL)); }

const char *hk_jni_class_name(const hk_jobj *o)
{
    if (!o) return "null";
    if (o->kind == HK_J_CLASS) return o->u.klass.jc ? o->u.klass.jc->name : "?";
    return o->cls ? o->cls->name : "?";
}

/* ----------------------------------------------------- the pending exception */

/* One exception can be waiting on a thread: the runtime keeps it while it runs. */
static hk_jobj *pending(void) { return g_dvm.running ? hk_dvm_pending() : t_pending; }

static void set_pending(hk_jobj *e)
{
    if (!g_dvm.running) t_pending = e;
    else if (!e) hk_dvm_clear_pending();
    else if (e->dvm) hk_dvm_throw_object(e);
    else hk_warn("jni: an exception of %s made before the runtime started cannot be thrown in it", hk_jni_class_name(e));
}

/* --------------------------------------------------------------- objects */

static hk_jobj *new_obj(int kind, hk_jclass *cls)
{
    hk_jobj *o;
    if (g_dvm.running && kind == HK_J_OBJECT) {
        /* While the runtime runs every object is one of its own, also those of classes that are only this layer's. */
        if (!cls->dvm) bind_class(cls);
        if (cls->dvm) {
            o = hk_dvm_alloc_object(cls->dvm);
            if (o) return o;
        }
        hk_warn("jni: an object of %s could not be made in the runtime", cls->name);
    }
    o = (hk_jobj *)hk_alloc(sizeof(*o));
    o->refs = 1;
    o->kind = (u8)kind;
    o->cls = cls;
    return o;
}

hk_jobj *hk_jni_new_object(const char *cls) { return new_obj(HK_J_OBJECT, hk_jni_class(cls, NULL)); }

hk_jobj *hk_jni_new_string(const char *utf8)
{
    hk_jobj *o;
    if (!utf8) return NULL;
    if (g_dvm.running) return hk_dvm_new_string(utf8);
    o = new_obj(HK_J_STRING, hk_jni_class("java/lang/String", NULL));
    o->u.str.utf8 = hk_strdup(utf8);
    return o;
}

const char *hk_jni_string(hk_jobj *s)
{
    if (!s || s->kind != HK_J_STRING) return NULL;
    /* The runtime's strings are UTF-16; their UTF-8 is made when first asked for and kept. */
    return s->u.str.utf8 || !s->dvm ? s->u.str.utf8 : hk_dvm_string_utf8(s);
}

static u8 elem_size(char etype)
{
    switch (etype) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'J': case 'D': return 8;
    default: return 4;
    }
}

hk_jobj *hk_jni_new_prim_array(char etype, u32 len)
{
    char name[3] = { '[', etype, 0 };
    hk_jobj *o;
    if (g_dvm.running) return hk_dvm_new_array(name, (i32)len);
    o = new_obj(HK_J_PRIM_ARRAY, hk_jni_class(name, NULL));
    o->u.arr.etype = etype;
    o->u.arr.esz = elem_size(etype);
    o->u.arr.len = len;
    o->u.arr.data = hk_alloc((size_t)len * o->u.arr.esz + 8);
    return o;
}

hk_jobj *hk_jni_ref(hk_jobj *o)
{
    if (o && !o->permanent) hk_atomic_add(&o->refs, 1);
    return o;
}

void hk_jni_unref(hk_jobj *o)
{
    u32 i;
    int k;
    if (!o || o->permanent) return;
    if (hk_atomic_add(&o->refs, -1) > 0) return;
    /* Strings and arrays are made and dropped all the time; plain objects are few and may be kept by C code that never took a reference. */
    if (o->kind == HK_J_OBJECT) return;
    if (o->kind == HK_J_STRING) {
        free(o->u.str.utf8);
        free(o->u.str.u16);
    } else if (o->kind == HK_J_PRIM_ARRAY) {
        free(o->u.arr.data);
    } else if (o->kind == HK_J_OBJ_ARRAY) {
        for (i = 0; i < o->u.oarr.len; i++) hk_jni_unref(o->u.oarr.v[i]);
        free(o->u.oarr.v);
    }
    for (k = 0; k < o->nfields; k++) free(o->fields[k].name);
    free(o->fields);
    free(o);
}

void hk_jni_throw(const char *cls, const char *msg)
{
    hk_jobj *e;
    if (g_dvm.running && is_real(hk_jni_class(cls, NULL))) {
        /* One of the runtime's own exception classes: made as bytecode makes it, constructor and all. */
        hk_dvm_throw(cls, "%s", msg ? msg : "");
        return;
    }
    e = hk_jni_new_object(cls);
    e->native = hk_strdup(msg ? msg : "");
    hk_warn("jni: %s: %s", cls, msg ? msg : "");
    set_pending(e);
}

/* ---------------------------------------------------------- UTF-8, UTF-16 */

static u16 *to_utf16(const char *s, i32 *len)
{
    size_t n = strlen(s), i = 0, k = 0;
    u16 *out = (u16 *)hk_alloc((n + 1) * 2);
    while (i < n) {
        u32 c = (u8)s[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, j;
        c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        for (j = 1; j <= extra && i + (size_t)j < n; j++) c = (c << 6) | ((u8)s[i + (size_t)j] & 0x3F);
        i += (size_t)extra + 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            out[k++] = (u16)(0xD800 + (c >> 10));
            out[k++] = (u16)(0xDC00 + (c & 0x3FF));
        } else {
            out[k++] = (u16)c;
        }
    }
    *len = (i32)k;
    return out;
}

static char *from_utf16(const u16 *s, i32 len)
{
    char *out = (char *)hk_alloc((size_t)len * 3 + 1);
    size_t k = 0;
    i32 i;
    for (i = 0; i < len; i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) out[k++] = (char)c;
        else if (c < 0x800) {
            out[k++] = (char)(0xC0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[k++] = (char)(0xE0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | (c >> 18));
            out[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[k] = 0;
    return out;
}

static void need_utf16(hk_jobj *s)
{
    if (!s->u.str.u16) s->u.str.u16 = to_utf16(s->u.str.utf8, &s->u.str.len16);
}

void hk_jni_string_set(hk_jobj *s, const char *utf8)
{
    if (!s || s->kind != HK_J_STRING) return;
    if (s->dvm) {
        /* A string of the runtime's is made whole (construct): its text cannot be given afterwards. */
        hk_err("jni: hk_jni_string_set on a string of the runtime's");
        return;
    }
    free(s->u.str.utf8);
    free(s->u.str.u16);
    s->u.str.u16 = NULL;
    s->u.str.len16 = 0;
    s->u.str.utf8 = hk_strdup(utf8 ? utf8 : "");
}

void hk_jni_string_set_utf16(hk_jobj *s, const u16 *chars, i32 len)
{
    char *utf8 = from_utf16(chars, len);
    hk_jni_string_set(s, utf8);
    free(utf8);
}

const u16 *hk_jni_string_utf16(hk_jobj *s, i32 *len)
{
    if (!s || s->kind != HK_J_STRING) {
        if (len) *len = 0;
        return NULL;
    }
    need_utf16(s);
    if (len) *len = s->u.str.len16;
    return s->u.str.u16;
}

/* -------------------------------------------------------- methods, fields */

/* One letter per argument of a signature, and the return kind. */
static void parse_sig(const char *sig, char *argk, int cap, int *nargs, char *retk)
{
    const char *p = sig;
    int n = 0;
    *retk = 'V';
    if (*p == '(') p++;
    while (*p && *p != ')') {
        char k = *p;
        if (k == '[') {
            while (*p == '[') p++;
            if (*p == 'L') while (*p && *p != ';') p++;
            k = 'L';
        } else if (k == 'L') {
            while (*p && *p != ';') p++;
        }
        if (*p) p++;
        if (n < cap) argk[n++] = k;
    }
    *nargs = n;
    if (*p == ')') p++;
    if (*p) *retk = (*p == '[') ? 'L' : *p;
}

static hk_jhle_fn find_hle(const hk_jclass *cls, const char *name, const char *sig)
{
    const hk_jclass *c;
    int t;
    for (c = cls; c; c = c->super) {
        for (t = g_ntables - 1; t >= 0; t--) {
            const hk_jhle *h;
            for (h = g_tables[t]; h->cls; h++)
                if (!strcmp(h->name, name) && !strcmp(h->cls, c->name) && (!h->sig || !strcmp(h->sig, sig))) return h->fn;
        }
    }
    return NULL;
}

static jmeth *get_method(hk_jclass *cls, const char *name, const char *sig, bool is_static)
{
    hk_dvm_method *dm = NULL;
    jmeth *m;
    if (is_real(cls)) {
        /* A class the runtime has: the method is there or it is not, as on Android, and asking initialises the class. */
        if (!hk_dvm_ensure_init(cls->dvm)) return NULL;
        dm = hk_dvm_find_method(cls->dvm, name, sig, is_static);
        if (!dm) {
            hk_dvm_throw("java/lang/NoSuchMethodError", "no %s method \"%s.%s%s\"", is_static ? "static" : "non-static", cls->name, name, sig);
            return NULL;
        }
    }
    hk_lock_acquire(&g_jni_lock);
    for (m = cls->meths; m; m = m->next)
        if (m->is_static == is_static && !strcmp(m->name, name) && !strcmp(m->sig, sig)) break;
    if (!m) {
        m = (jmeth *)hk_alloc(sizeof(*m));
        m->cls = cls;
        m->name = hk_strdup(name);
        m->sig = hk_strdup(sig);
        m->is_static = is_static;
        parse_sig(sig, m->argk, (int)sizeof(m->argk), &m->nargs, &m->retk);
        m->next = cls->meths;
        cls->meths = m;
    }
    hk_lock_release(&g_jni_lock);
    m->dvm = dm;
    if (!dm && !m->fn) m->fn = find_hle(cls, name, sig);
    return m;
}

/* `exact`: the method itself (CallNonvirtual, a constructor), not what the object's class has made of it. */
/*
 * A String constructor called through JNI on a string AllocObject made (Call(Nonvirtual)VoidMethod with String.<init>):
 * libcore's constructors only throw ("Use StringFactory instead"), and ART turns the call into StringFactory's method with
 * the same parameters and makes the receiver's reference the new string (InvokeVirtualOrInterfaceWith*, is_string_init).
 * References here are the objects themselves, so the receiver takes the new string's contents instead: its text in a
 * buffer of its own (which hk_dvm_object_dying frees, `native` holding it), its hash and kept UTF-8 forgotten. Native code
 * that makes a string so and then uses the object it allocated -- the app-lock library of VK mp3 mod -- sees the string.
 */
static void string_init_on(hk_jobj *self, jmeth *m, const hk_jvalue *args)
{
    hk_jobj *made = NULL;
    u16 *text;
    if (!self || self->kind != HK_J_STRING) {
        hk_dvm_throw("java/lang/InstantiationException", "java.lang.String's constructor called through JNI on an object that is no string AllocObject made");
        return;
    }
    if (!hk_dvm_string_init(m->sig, args, &made) || !made) return;
    text = (u16 *)hk_alloc(((size_t)(u32)made->u.str.len16 + 1) * 2);
    if (made->u.str.len16) memcpy(text, made->u.str.u16, (size_t)made->u.str.len16 * 2);
    text[made->u.str.len16] = 0;
    if (self->native) free(self->native);
    self->native = text;
    self->u.str.u16 = text;
    self->u.str.len16 = made->u.str.len16;
    if (self->u.str.utf8) {
        free(self->u.str.utf8);
        self->u.str.utf8 = NULL;
    }
    if (g_dvm.c_string->nslots > 0) hk_dvm_slots(self)[0].j = 0;   /* libcore's String: its one field, the hash, made again when asked */
}

static hk_jvalue invoke_as(hk_jobj *self, jmeth *m, const hk_jvalue *args, bool exact)
{
    hk_jcall c;
    memset(&c, 0, sizeof(c));
    if (!m) return c.ret;                                        /* the lookup failed and said so; the caller went on regardless */
    if (m->dvm && !m->is_static && m->dvm->cls == g_dvm.c_string && !strcmp(m->name, "<init>")) {
        string_init_on(self, m, args);
        return c.ret;
    }
    if (m->dvm) {
        hk_dvm_method *target = m->dvm;
        hk_jvalue clean[JARGS];
        int i;
        if (m->is_static) {
            if (!hk_dvm_ensure_init(target->cls)) return c.ret;
        } else if (!self || !self->dvm) {
            hk_dvm_throw("java/lang/NullPointerException", "%s.%s%s called through JNI on %s", m->cls->name, m->name, m->sig, self ? "an object that is not the runtime's" : "null");
            return c.ret;
        } else if (!exact) {
            target = hk_dvm_virtual_target(target, self);
        }
        /* An object made before the runtime started is not an object to it: it gets null in its place. */
        for (i = 0; i < m->nargs; i++)
            if (m->argk[i] == 'L' && args[i].l && !args[i].l->dvm) break;
        if (i < m->nargs) {
            memcpy(clean, args, sizeof(hk_jvalue) * (size_t)m->nargs);
            for (i = 0; i < m->nargs; i++)
                if (m->argk[i] == 'L' && clean[i].l && !clean[i].l->dvm) clean[i].l = NULL;
            args = clean;
        }
        hk_dvm_invoke(target, m->is_static ? NULL : self, args, &c.ret);
        return c.ret;
    }
    hk_trace("java %s.%s%s", self && self->cls ? self->cls->name : m->cls->name, m->name, m->sig);
    /* The object's own class may implement what the declared one does not. */
    if (!m->fn && self && self->cls && self->cls != m->cls) m->fn = find_hle(self->cls, m->name, m->sig);
    if (!m->fn) {
        if (!m->warned) {
            m->warned = true;
            hk_warn("jni: %s.%s%s is not implemented (returns zero)", self && self->cls ? self->cls->name : m->cls->name, m->name, m->sig);
        }
        return c.ret;
    }
    c.self = m->is_static ? NULL : self;
    c.cls = m->cls;
    c.name = m->name;
    c.sig = m->sig;
    c.args = args;
    m->fn(&c);
    return c.ret;
}

static hk_jvalue invoke(hk_jobj *self, jmeth *m, const hk_jvalue *args) { return invoke_as(self, m, args, false); }

static void args_from_va(const jmeth *m, va_list ap, hk_jvalue *out)
{
    int i;
    if (!m) return;
    for (i = 0; i < m->nargs; i++) {
        out[i].j = 0;
        switch (m->argk[i]) {
        case 'J': out[i].j = va_arg(ap, i64); break;
        case 'D': out[i].d = va_arg(ap, double); break;
        case 'F': out[i].f = (float)va_arg(ap, double); break;       /* a float is passed to "..." as a double */
        case 'L': out[i].l = va_arg(ap, hk_jobj *); break;
        default: out[i].i = va_arg(ap, i32); break;
        }
    }
}

static jfield *get_field(hk_jclass *cls, const char *name, const char *sig, bool is_static)
{
    hk_dvm_field *df = NULL;
    hk_jclass *c;
    jfield *f;
    if (is_real(cls)) {
        if (!hk_dvm_ensure_init(cls->dvm)) return NULL;
        df = hk_dvm_find_field_typed(cls->dvm, name, sig, is_static);
        if (!df) {
            hk_dvm_throw("java/lang/NoSuchFieldError", "no \"%s\" field \"%s\" in class \"%s\" or its superclasses", sig, name, cls->name);
            return NULL;
        }
        for (f = cls->fields; f; f = f->next)
            if (f->dvm == df) return f;
    } else {
        /* A static field declared on a superclass is that class's field. */
        for (c = cls; c; c = c->super)
            for (f = c->fields; f; f = f->next)
                if (f->is_static == is_static && !f->dvm && !strcmp(f->name, name)) return f;
    }
    f = (jfield *)hk_alloc(sizeof(*f));
    f->cls = cls;
    f->name = hk_strdup(name);
    f->kind = (sig[0] == '[') ? 'L' : sig[0];
    f->is_static = is_static;
    f->dvm = df;
    hk_lock_acquire(&g_jni_lock);
    f->next = cls->fields;
    cls->fields = f;
    hk_lock_release(&g_jni_lock);
    return f;
}

void hk_jni_set_static(const char *cls, const char *name, char kind, hk_jvalue v)
{
    char sig[2] = { kind, 0 };
    hk_jclass *c = hk_jni_class(cls, NULL);
    jfield *f;
    if (is_real(c)) {
        hk_dvm_set_static(cls, name, v);
        return;
    }
    f = get_field(c, name, sig, true);
    f->value = v;
    f->has_value = true;
}

/*
 * A static field read that nothing ever set: a constant of Android's
 * (AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE, Context.AUDIO_SERVICE...) that
 * this layer does not know. Native code gets null or zero, and whatever it
 * does with that fails somewhere else with no word of why, so it is said
 * here, once a field.
 */
static const jfield *static_read(jfield *f)
{
    if (f && !f->has_value && !f->warned) {
        f->warned = true;
        hk_warn("jni: the static field %s.%s was read and nothing has given it a value (the app gets %s)", f->cls ? f->cls->name : "?", f->name,
                f->kind == 'L' ? "null" : "zero");
    }
    return f;
}

/* Where a static field's value is: in the runtime's class, or here. */
static hk_jvalue *static_slot(jfield *f, bool reading)
{
    if (f->dvm) return hk_dvm_ensure_init(f->dvm->cls) ? &f->dvm->cls->statics[f->dvm->slot] : &f->value;
    if (reading) static_read(f);
    else f->has_value = true;
    return &f->value;
}

static hk_jvalue *instance_slot(hk_jobj *o, const jfield *f)
{
    static hk_jvalue nowhere;
    int i;
    if (f->dvm) {
        /* A field of a class the runtime has: one of the object's slots, if the object is of such a class at all. */
        if (o->dvm && f->dvm->slot < hk_dvm_class_of(o)->nslots) return &hk_dvm_slots(o)[f->dvm->slot];
        nowhere.j = 0;
        return &nowhere;
    }
    for (i = 0; i < o->nfields; i++)
        if (!strcmp(o->fields[i].name, f->name)) return &o->fields[i].v;
    o->fields = (hk_jfield_value *)hk_realloc(o->fields, sizeof(hk_jfield_value) * (size_t)(o->nfields + 1));
    memset(&o->fields[o->nfields], 0, sizeof(hk_jfield_value));
    o->fields[o->nfields].name = hk_strdup(f->name);
    o->fields[o->nfields].is_object = f->kind == 'L';
    return &o->fields[o->nfields++].v;
}

void hk_jni_set_field(hk_jobj *o, const char *name, char kind, hk_jvalue v)
{
    char sig[2] = { kind, 0 };
    if (!o || !o->cls) return;
    if (is_real(o->cls)) hk_dvm_set_field(o, name, v);
    else *instance_slot(o, get_field(o->cls, name, sig, false)) = v;
}

hk_jvalue hk_jni_get_field(hk_jobj *o, const char *name)
{
    hk_jvalue none;
    int i;
    none.j = 0;
    if (!o) return none;
    if (is_real(o->cls)) {
        hk_dvm_get_field(o, name, &none);
        return none;
    }
    for (i = 0; i < o->nfields; i++)
        if (!strcmp(o->fields[i].name, name)) return o->fields[i].v;
    return none;
}

bool hk_jni_exception_clear(void)
{
    bool was = pending() != NULL;
    set_pending(NULL);
    return was;
}

/* ------------------------------------------------------------- reflection */

typedef struct reflected {
    jmeth *m;
    hk_jobj *object;
    struct reflected *next;
} reflected;

static reflected *g_reflected;

hk_jobj *hk_jni_reflect_method(const char *cls, const char *name, const char *sig, bool is_static)
{
    jmeth *m = get_method(hk_jni_class(cls, NULL), name, sig, is_static);
    reflected *r;
    hk_jobj *o;
    hk_lock_acquire(&g_jni_lock);
    for (r = g_reflected; r; r = r->next)
        if (r->m == m) break;
    hk_lock_release(&g_jni_lock);
    if (r) return r->object;
    o = hk_jni_new_object("java/lang/reflect/Method");
    o->native = m;
    o->permanent = true;
    r = (reflected *)hk_alloc(sizeof(*r));
    r->m = m;
    r->object = o;
    hk_lock_acquire(&g_jni_lock);
    r->next = g_reflected;
    g_reflected = r;
    hk_lock_release(&g_jni_lock);
    return o;
}

static const jmeth *reflected_method(const hk_jobj *method)
{
    return method && method->kind == HK_J_OBJECT && method->cls && !strcmp(method->cls->name, "java/lang/reflect/Method") ? (const jmeth *)method->native
                                                                                                                         : NULL;
}
const char *hk_jni_reflected_name(const hk_jobj *method) { const jmeth *m = reflected_method(method); return m ? m->name : NULL; }
const char *hk_jni_reflected_sig(const hk_jobj *method) { const jmeth *m = reflected_method(method); return m ? m->sig : NULL; }

/* ----------------------------------------------------------------- natives */

/* A name as JNI spells it inside a symbol: "/" is "_", and what a symbol cannot hold is escaped. */
static size_t mangle(const char *p, char stop, char *out, size_t k, size_t cap)
{
    for (; *p && *p != stop && k + 8 < cap; p++) {
        u8 ch = (u8)*p;
        if (ch == '/') out[k++] = '_';
        else if (ch == '_') { out[k++] = '_'; out[k++] = '1'; }
        else if (ch == ';') { out[k++] = '_'; out[k++] = '2'; }
        else if (ch == '[') { out[k++] = '_'; out[k++] = '3'; }
        else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) out[k++] = (char)ch;
        else {
            /* Anything else is its UTF-16 code unit in hex ("$" is _00024); the name is in UTF-8 as DEX files write it. */
            unsigned unit = ch;
            if ((ch & 0xE0) == 0xC0 && ((u8)p[1] & 0xC0) == 0x80) {
                unit = ((ch & 0x1Fu) << 6) | ((u8)p[1] & 0x3Fu);
                p += 1;
            } else if ((ch & 0xF0) == 0xE0 && ((u8)p[1] & 0xC0) == 0x80 && ((u8)p[2] & 0xC0) == 0x80) {
                unit = ((ch & 0x0Fu) << 12) | (((u8)p[1] & 0x3Fu) << 6) | ((u8)p[2] & 0x3Fu);
                p += 2;
            }
            k += (size_t)snprintf(out + k, cap - k, "_0%04x", unit);
        }
    }
    out[k] = 0;
    return k;
}

/* For the runtime's classes: how many native methods libraries registered, how many table entries named none, how many were found by their JNI names. */
static volatile i32 g_natives_registered, g_natives_unknown, g_natives_by_name;

void hk_jni_native_counts(i32 *registered, i32 *unknown, i32 *by_name)
{
    *registered = g_natives_registered;
    *unknown = g_natives_unknown;
    *by_name = g_natives_by_name;
}

/* The two names a library may export a native method under: Java_<class>_<method>, and the same with its parameters after two underscores. */
static void native_names(const char *cls, const char *name, const char *sig, char *brief, char *full, size_t cap)
{
    size_t k = (size_t)snprintf(brief, cap, "Java_");
    k = mangle(cls, 0, brief, k, cap);
    brief[k++] = '_';
    k = mangle(name, 0, brief, k, cap);
    memcpy(full, brief, k + 1);
    full[0] = sig && sig[0] == '(' && k + 4 < cap ? full[0] : 0;
    if (!full[0]) return;
    full[k++] = '_';
    full[k++] = '_';
    mangle(sig + 1, ')', full, k, cap);
}

/*
 * A function some loaded library exports under that name. The libraries are
 * of two kinds, the app's and the runtime's own (ld/hk_ld.h), and the one
 * whose class it is comes first: a class of the app's finds its methods in
 * the app's libraries, a class of the runtime's in the runtime's. ART looks
 * nowhere else -- only in the libraries of the class's own loader; here the
 * other kind is still looked in last, since which DEX file is the app's is
 * only known where the Java driver says so. Within a kind, the library loaded
 * first wins when two export one name (ART: the one whose path sorts first).
 */
static void *exported_native(const char *symbol, bool app_class)
{
    void *fn = hk_ld_sym_among(!app_class, symbol, NULL);
    return fn ? fn : hk_ld_sym_among(app_class, symbol, NULL);
}

static void *find_native(const char *cls, const char *name, const char *sig, bool app_class)
{
    hk_jclass *c = hk_jni_class(cls, NULL);
    jnative *n;
    char brief[1024], full[1024];
    void *fn;
    for (n = c->natives; n; n = n->next)
        if (!strcmp(n->name, name) && (!sig || !strcmp(n->sig, sig))) return n->fn;
    /* Not registered: the library may export it under its JNI name. */
    native_names(cls, name, sig, brief, full, sizeof(brief));
    fn = exported_native(brief, app_class);
    if (!fn && full[0]) fn = exported_native(full, app_class);
    return fn;
}

void *hk_jni_native(const char *cls, const char *name, const char *sig) { return find_native(cls, name, sig, true); }

static void native_tried(const char *cls, const char *name, const char *sig, char *out, size_t n)
{
    char brief[1024], full[1024];
    native_names(cls, name, sig, brief, full, sizeof(brief));
    snprintf(out, n, "%s%s%s", brief, full[0] ? " and " : "", full);
}

#define W8 u32, u32, u32, u32, u32, u32, u32, u32
#define A8(b) w[b], w[(b) + 1], w[(b) + 2], w[(b) + 3], w[(b) + 4], w[(b) + 5], w[(b) + 6], w[(b) + 7]
#define NATIVE_WORDS 120

/*
 * The call itself: `n` words, the first four in r0-r3 and the rest on the
 * stack, which is where AAPCS wants them whatever the function's parameters
 * are. A function is handed more words than it reads; that costs nothing, the
 * caller owns the stack. Three sizes, so that the usual call does not copy a
 * hundred words.
 */
static u64 call_words(void *fn, const u32 *w, int n)
{
    typedef u64 (*fn24)(W8, W8, W8);
    typedef u64 (*fn56)(W8, W8, W8, W8, W8, W8, W8);
    typedef u64 (*fn120)(W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8);
    if (n <= 24) return HK_GFN(fn24, fn)(A8(0), A8(8), A8(16));
    if (n <= 56) return HK_GFN(fn56, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48));
    return HK_GFN(fn120, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48), A8(56), A8(64), A8(72), A8(80), A8(88), A8(96), A8(104), A8(112));
}

/* `bare`: a @CriticalNative method, which is given its arguments alone: no JNIEnv, no class. */
static hk_jvalue call_native(void *fn, hk_jobj *self_or_class, const char *sig, const hk_jvalue *args, bool bare)
{
    char argk[JARGS], retk;
    u32 w[NATIVE_WORDS];
    int nargs, n = 0, i;
    u64 r;
    hk_jvalue ret;
    ret.j = 0;
    parse_sig(sig, argk, (int)sizeof(argk), &nargs, &retk);
    if (!bare) {
        w[n++] = (u32)(hk_addr)hk_jni_env();
        w[n++] = (u32)(hk_addr)self_or_class;
    }
    /* AAPCS, integer registers only: one word each, a 64-bit value in two words starting at an even one. */
    for (i = 0; i < nargs; i++) {
        if (argk[i] == 'J' || argk[i] == 'D') {
            u64 v;
            if (n & 1) w[n++] = 0;
            if (n + 2 > NATIVE_WORDS) break;
            memcpy(&v, &args[i], 8);
            w[n++] = (u32)v;
            w[n++] = (u32)(v >> 32);
        } else {
            if (n + 1 > NATIVE_WORDS) break;
            if (argk[i] == 'F') memcpy(&w[n++], &args[i].f, 4);
            else if (argk[i] == 'L') w[n++] = (u32)(hk_addr)args[i].l;
            else if (argk[i] == 'Z' || argk[i] == 'B') w[n++] = (u32)(i32)args[i].b;
            else if (argk[i] == 'C') w[n++] = args[i].c;
            else if (argk[i] == 'S') w[n++] = (u32)(i32)args[i].s;
            else w[n++] = (u32)args[i].i;
        }
    }
    if (i < nargs) {
        hk_err("jni: a native method with more arguments than can be passed (%s)", sig);
        return ret;
    }
    /*
     * The words past the arguments that call_words passes too, zero (the function does not read them). Only those: the
     * whole array, 480 bytes, was cleared on every call before, and most calls pass 24 words.
     */
    {
        int pass = n <= 24 ? 24 : n <= 56 ? 56 : NATIVE_WORDS;
        while (n < pass) w[n++] = 0;
    }
    r = call_words(fn, w, n);
    switch (retk) {
    case 'V': break;
    case 'J': case 'D': memcpy(&ret, &r, 8); break;
    case 'F': { u32 lo = (u32)r; memcpy(&ret.f, &lo, 4); break; }
    case 'L': ret.l = (hk_jobj *)(hk_addr)(u32)r; break;
    default: ret.i = (i32)(u32)r; break;
    }
    return ret;
}

hk_jvalue hk_jni_call_native(void *fn, hk_jobj *self_or_class, const char *sig, const hk_jvalue *args)
{
    return call_native(fn, self_or_class, sig, args, false);
}

static hk_jvalue call_native_bare(void *fn, const char *sig, const hk_jvalue *args) { return call_native(fn, NULL, sig, args, true); }

/* A native method of one of the runtime's classes: asked once for each, when it is first called. */
static void *native_for_dvm(const char *cls, const char *name, const char *sig, bool app_class)
{
    hk_jclass *c = hk_jni_class(cls, NULL);
    void *fn = find_native(cls, name, sig, app_class);
    jnative *n;
    if (!fn) return NULL;
    for (n = c->natives; n; n = n->next)
        if (n->fn == fn && !strcmp(n->name, name)) return fn;
    hk_atomic_add(&g_natives_by_name, 1);
    return fn;
}

void hk_jni_dvm_join(hk_dvm_jni *out)
{
    out->find = native_for_dvm;
    out->tried = native_tried;
    out->call = hk_jni_call_native;
    out->call_bare = call_native_bare;
    out->class_for = jni_class_for;
}

/* ------------------------------------------------------- the JNIEnv table */

typedef void *JNIEnvP;                 /* JNIEnv *: the guest's first argument, unused here */

static hk_jclass *class_of(hk_jobj *o)
{
    if (!o) return hk_jni_class("java/lang/Object", NULL);
    return o->kind == HK_J_CLASS ? o->u.klass.jc : (o->cls ? o->cls : hk_jni_class("java/lang/Object", NULL));
}

static i32 j_GetVersion(JNIEnvP e) { (void)e; return 0x00010006; }
static hk_jobj *j_DefineClass(JNIEnvP e, const char *name, hk_jobj *loader, const void *buf, i32 len)
{
    (void)e; (void)loader; (void)buf; (void)len;
    if (g_dvm.running) {
        /* Android's answer too: a class comes from a DEX file through a class loader, never from bytes handed to JNI. */
        hk_warn("jni: DefineClass(%s) is not supported, as on Android", name ? name : "null");
        return NULL;
    }
    return hk_jni_class_object(name);
}

/*
 * Whose classes a FindClass finds. ART asks who is calling (GetClassLoader in
 * art/runtime/jni_internal.cc): inside JNI_OnLoad, the class loader the
 * library is being loaded for; inside a native method, the loader of that
 * method's class; on a thread with no Java method under it -- one native code
 * made and attached -- the system class loader, which knows Android's classes
 * and none of the app's. Apps are written around the last rule (they look
 * their classes up in JNI_OnLoad and keep them), so it is kept here, and not
 * replaced by the one class path this runtime really has. There is a class
 * loader of the app's only where the Java driver has said which classes are
 * the app's; everywhere else every class is found from everywhere.
 */
enum { ASKER_BOOT, ASKER_APP, ASKER_NOBODY };

static int asker(const hk_dvm_thread *t)
{
    if (t->onload_running && t->top == t->onload_frame) return t->onload_app ? ASKER_APP : ASKER_BOOT;
    if (!t->top) return ASKER_NOBODY;
    return hk_dvm_class_is_app(t->top->m->cls) ? ASKER_APP : ASKER_BOOT;
}

/* Whether a DEX file defines the class a JNI name stands for: the class itself, or what an array of that name is an array of. */
static bool name_is_defined(const char *name)
{
    char inner[300];
    size_t n;
    if (name[0] != '[') return hk_dvm_class_exists(name);
    while (*name == '[') name++;
    if (*name != 'L') return name[0] && !name[1];               /* an array of a primitive type */
    n = strlen(name);
    if (n < 3 || n - 2 >= sizeof(inner)) return false;
    memcpy(inner, name + 1, n - 2);
    inner[n - 2] = 0;
    return hk_dvm_class_exists(inner);
}

/* What ART leaves when a class is not found: the boot class path's own error for a caller that is one of the runtime's classes, a class loader's for every other. */
static void throw_not_found(const char *name, int who, bool hidden)
{
    char dotted[300];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof(dotted); i++) dotted[i] = name[i] == '/' ? '.' : name[i];
    dotted[i] = 0;
    if (who == ASKER_BOOT) {
        hk_dvm_throw("java/lang/NoClassDefFoundError", "%s", name);
    } else if (who == ASKER_NOBODY) {
        hk_dvm_throw("java/lang/ClassNotFoundException", "Didn't find class \"%s\" on path: DexPathList[[directory \".\"],nativeLibraryDirectories=[/system/lib]]%s", dotted,
                     hidden ? " (FindClass on a thread with no Java method under it asks the system class loader, which has none of the app's classes)" : "");
    } else {
        hk_dvm_throw("java/lang/ClassNotFoundException", "Didn't find class \"%s\"", dotted);
    }
}

static hk_jobj *j_FindClass(JNIEnvP e, const char *name)
{
    hk_jclass *c;
    const hk_dvm_class *k;
    int who = ASKER_APP;
    (void)e;
    if (!g_dvm.running) return hk_jni_class_object(name);
    /* With the runtime up, a class a DEX file defines is found as Android finds it: linked, or not found and an exception. (It is initialised when a method or field of it is asked for.) */
    hk_dvm_clear_pending();
    if (g_dvm.app_loader) {
        /*
         * A Java app: a class is one a DEX file defines, or it is not there. (Elsewhere a name nothing defines is a class
         * of this layer's own, made by being asked for: the Java parts the other drivers play in C.)
         */
        who = asker(hk_dvm_self());
        if (!name_is_defined(name)) {
            throw_not_found(name, who, false);
            return NULL;
        }
    }
    c = hk_jni_class(name, NULL);
    if (!c->dvm || hk_dvm_pending()) {
        /* Found, and its linking failed: that exception is the answer. */
        if (!hk_dvm_pending()) hk_dvm_throw("java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    for (k = c->dvm; k->component; k = k->component) { }
    if (who != ASKER_APP && hk_dvm_class_is_app(k)) {
        throw_not_found(name, who, true);
        return NULL;
    }
    return mirror_of(c);
}

/* A method or field identifier of a Method, Constructor or Field object: libcore's real ones while the runtime runs. */
static jmeth *j_FromReflectedMethod(JNIEnvP e, hk_jobj *m)
{
    hk_dvm_method *dm = g_dvm.running ? hk_dvm_reflected_method(m) : NULL;
    (void)e;
    if (dm) return dm->cls->jc ? get_method(dm->cls->jc, dm->name, dm->sig, (dm->flags & DVM_ACC_STATIC) != 0) : NULL;
    return m && !is_real(m->cls) ? (jmeth *)m->native : NULL;
}
static jfield *j_FromReflectedField(JNIEnvP e, hk_jobj *f)
{
    hk_dvm_field *df = g_dvm.running ? hk_dvm_reflected_field(f) : NULL;
    (void)e;
    if (df) return df->cls->jc ? get_field(df->cls->jc, df->name, df->type, (df->flags & DVM_ACC_STATIC) != 0) : NULL;
    return f && !is_real(f->cls) ? (jfield *)f->native : NULL;
}
static hk_jobj *j_ToReflectedMethod(JNIEnvP e, hk_jobj *cls, jmeth *id, u8 is_static)
{
    hk_jobj *o;
    (void)e; (void)cls; (void)is_static;
    if (id && id->dvm) return hk_dvm_reflect_method(id->dvm);
    if (g_dvm.libcore) return NULL;                             /* nothing but a method of the runtime's can be a Method of libcore's */
    o = hk_jni_new_object("java/lang/reflect/Method");
    o->native = id;
    return o;
}
static hk_jobj *j_ToReflectedField(JNIEnvP e, hk_jobj *cls, jfield *id, u8 is_static)
{
    hk_jobj *o;
    (void)e; (void)cls; (void)is_static;
    if (id && id->dvm) return hk_dvm_reflect_field(id->dvm);
    if (g_dvm.libcore) return NULL;
    o = hk_jni_new_object("java/lang/reflect/Field");
    o->native = id;
    return o;
}
static hk_jobj *j_GetSuperclass(JNIEnvP e, hk_jobj *cls)
{
    hk_jclass *c = class_of(cls);
    (void)e;
    if (is_real(c)) return c->dvm->super && !(c->dvm->flags & DVM_ACC_INTERFACE) ? hk_dvm_mirror(c->dvm->super) : NULL;
    return c->super ? mirror_of(c->super) : NULL;
}
static bool is_subclass(const hk_jclass *c, const hk_jclass *of)
{
    for (; c; c = c->super)
        if (c == of) return true;
    return !strcmp(of->name, "java/lang/Object");
}
static u8 j_IsAssignableFrom(JNIEnvP e, hk_jobj *a, hk_jobj *b)
{
    hk_jclass *ca = class_of(a), *cb = class_of(b);
    (void)e;
    if (ca->dvm && cb->dvm) return hk_dvm_assignable(ca->dvm, cb->dvm);
    return is_subclass(ca, cb);
}
static i32 j_Throw(JNIEnvP e, hk_jobj *obj)
{
    (void)e;
    if (!g_dvm.running) hk_warn("jni: exception thrown from native code: %s", hk_jni_class_name(obj));
    set_pending(obj);
    return 0;
}
static i32 j_ThrowNew(JNIEnvP e, hk_jobj *cls, const char *msg)
{
    (void)e;
    hk_jni_throw(class_of(cls)->name, msg);
    return 0;
}
static hk_jobj *j_ExceptionOccurred(JNIEnvP e) { (void)e; return pending(); }
/* Android prints the exception and its stack trace to the log and leaves it pending; so does this. */
static void j_ExceptionDescribe(JNIEnvP e)
{
    hk_jobj *p = pending();
    char text[400], stack[1600];
    (void)e;
    if (p && p->dvm && hk_dvm_describe_pending(text, sizeof(text))) {
        stack[0] = 0;
        hk_dvm_pending_stack(stack, sizeof(stack));
        hk_warn("jni: pending exception %s\n%s", text, stack);
    } else if (p) {
        hk_warn("jni: pending exception %s: %s", hk_jni_class_name(p), p->native ? (const char *)p->native : "");
    }
}
static void j_ExceptionClear(JNIEnvP e) { (void)e; set_pending(NULL); }
static u8 j_ExceptionCheck(JNIEnvP e) { (void)e; return pending() != NULL; }
static void j_FatalError(JNIEnvP e, const char *msg) { (void)e; hk_fatal("jni: FatalError: %s", msg ? msg : ""); }
/*
 * How many global and weak global references native code holds to an object, for GetObjectRefType and for nothing else
 * (nothing is collected, so a reference keeps nothing alive). An open table by the object's address. An object whose
 * counts are back at zero leaves a marker in its place, so that the chains through it hold, and the markers go when the
 * table is next made anew: its size follows what is referenced now, not everything that ever was.
 */
typedef struct ref_count {
    hk_jobj *o;
    i32 global, weak;
} ref_count;
#define REF_GONE ((hk_jobj *)(hk_addr)1)
static ref_count *g_refs;
static u32 g_refs_cap, g_refs_used, g_refs_live;   /* the capacity is a power of two; used counts the markers too */
static hk_lock g_refs_lock = HK_LOCK_INIT;

static u32 ref_slot(const hk_jobj *o) { return g_refs_cap ? (u32)(((hk_addr)o >> 3) * 2654435761u) & (g_refs_cap - 1) : 0; }

/* The object's entry, or NULL. With the lock held. */
static ref_count *ref_find(const hk_jobj *o)
{
    u32 i, k;
    for (k = 0, i = ref_slot(o); k < g_refs_cap && g_refs[i].o; k++, i = (i + 1) & (g_refs_cap - 1))
        if (g_refs[i].o == o) return &g_refs[i];
    return NULL;
}

static void ref_counted(hk_jobj *o, int global, int weak)
{
    ref_count *r;
    u32 i, k;
    if (!o) return;
    hk_lock_acquire(&g_refs_lock);
    r = ref_find(o);
    if (!r && (global > 0 || weak > 0)) {
        if ((g_refs_used + 1) * 2 > g_refs_cap) {
            /* Made anew, with room for four times what is live. */
            ref_count *old = g_refs;
            u32 old_cap = g_refs_cap, cap = 256;
            while (cap < g_refs_live * 4 + 4) cap *= 2;
            g_refs = (ref_count *)hk_alloc(sizeof(ref_count) * cap);
            g_refs_cap = cap;
            g_refs_used = 0;
            for (k = 0; k < old_cap; k++) {
                if (!old[k].o || old[k].o == REF_GONE) continue;
                for (i = ref_slot(old[k].o); g_refs[i].o; i = (i + 1) & (cap - 1)) { }
                g_refs[i] = old[k];
                g_refs_used++;
            }
            free(old);
        }
        for (i = ref_slot(o); g_refs[i].o; i = (i + 1) & (g_refs_cap - 1)) { }
        r = &g_refs[i];
        r->o = o;
        r->global = r->weak = 0;
        g_refs_used++;
        g_refs_live++;
    }
    if (r) {
        r->global = r->global + global > 0 ? r->global + global : 0;
        r->weak = r->weak + weak > 0 ? r->weak + weak : 0;
        if (!r->global && !r->weak) {
            r->o = REF_GONE;
            g_refs_live--;
        }
    }
    hk_lock_release(&g_refs_lock);
}

/*
 * For the collector (dvm/hk_dvm_gc.c): the JNI layer's objects that are roots. These are the global and weak global
 * references, the Method objects of FromReflectedMethod, and the stand-in classes' static fields. Weak ones are kept as
 * strong for now: a weak reference here is the object's own address, which a freed object's successor could take.
 * hk_jni_gc_lock(true) is taken before the other threads are stopped, so that none of them is stopped halfway through the
 * table of references. The rest is read as it is: a list whose head is set last, values a word wide.
 */
void hk_jni_gc_lock(bool take)
{
    if (take) hk_lock_acquire(&g_refs_lock);
    else hk_lock_release(&g_refs_lock);
}

void hk_jni_gc_roots(void (*fn)(hk_jobj *o, void *user), void *user)
{
    const reflected *r;
    const hk_jclass *c;
    const jfield *f;
    u32 i;
    for (i = 0; i < g_refs_cap; i++)
        if (g_refs[i].o && g_refs[i].o != REF_GONE && (g_refs[i].global > 0 || g_refs[i].weak > 0)) fn(g_refs[i].o, user);
    for (r = g_reflected; r; r = r->next)
        if (r->object) fn(r->object, user);
    for (c = g_classes; c; c = c->next)
        for (f = c->fields; f; f = f->next)
            if (f->is_static && f->kind == 'L' && !f->dvm && f->value.l) fn(f->value.l, user);
}

static hk_jobj *j_NewGlobalRef(JNIEnvP e, hk_jobj *o) { (void)e; ref_counted(o, 1, 0); return hk_jni_ref(o); }
static void j_DeleteGlobalRef(JNIEnvP e, hk_jobj *o) { (void)e; ref_counted(o, -1, 0); hk_jni_unref(o); }
static hk_jobj *j_NewWeakGlobalRef(JNIEnvP e, hk_jobj *o) { (void)e; ref_counted(o, 0, 1); return hk_jni_ref(o); }
static void j_DeleteWeakGlobalRef(JNIEnvP e, hk_jobj *o) { (void)e; ref_counted(o, 0, -1); hk_jni_unref(o); }

static i32 j_PushLocalFrame(JNIEnvP e, i32 cap) { (void)e; (void)cap; return 0; }
static hk_jobj *j_PopLocalFrame(JNIEnvP e, hk_jobj *result) { (void)e; return result; }
static hk_jobj *j_NewRef(JNIEnvP e, hk_jobj *o) { (void)e; return hk_jni_ref(o); }
static void j_DeleteRef(JNIEnvP e, hk_jobj *o) { (void)e; hk_jni_unref(o); }
static u8 j_IsSameObject(JNIEnvP e, hk_jobj *a, hk_jobj *b) { (void)e; return a == b; }
static i32 j_EnsureLocalCapacity(JNIEnvP e, i32 cap) { (void)e; (void)cap; return 0; }
/* AllocObject: an object of the class, its constructor not run. A String is an empty string of its own, as ART gives one
   (mirror::String::AllocEmptyString); a String constructor called on it then gives it its contents (string_init_on). */
static hk_jobj *j_AllocObject(JNIEnvP e, hk_jobj *cls)
{
    hk_jclass *c = class_of(cls);
    (void)e;
    if (is_real(c) && c->dvm == g_dvm.c_string) return hk_dvm_alloc_string(NULL, 0);
    return new_obj(HK_J_OBJECT, c);
}
static hk_jobj *j_GetObjectClass(JNIEnvP e, hk_jobj *o)
{
    (void)e;
    if (o && o->dvm) return hk_dvm_mirror(hk_dvm_class_of(o));
    return mirror_of(class_of(o));
}
static u8 j_IsInstanceOf(JNIEnvP e, hk_jobj *o, hk_jobj *cls)
{
    (void)e;
    if (!o) return 1;
    /* A proxy stands in for whatever interfaces it was made for, and those are not recorded: it is one of anything asked. */
    if (o->kind == HK_J_OBJECT && o->cls && !strcmp(o->cls->name, "java/lang/reflect/Proxy")) return 1;
    if (o->dvm && class_of(cls)->dvm) return hk_dvm_is_instance(o, class_of(cls)->dvm);
    return is_subclass(o->kind == HK_J_CLASS ? hk_jni_class("java/lang/Class", NULL) : o->cls, class_of(cls));
}
static jmeth *j_GetMethodID(JNIEnvP e, hk_jobj *cls, const char *name, const char *sig) { (void)e; return get_method(class_of(cls), name, sig, false); }
static jmeth *j_GetStaticMethodID(JNIEnvP e, hk_jobj *cls, const char *name, const char *sig) { (void)e; return get_method(class_of(cls), name, sig, true); }

/* A constructor: the new object, then <init> on it if anything implements it. */
static hk_jobj *construct(hk_jobj *cls, jmeth *m, const hk_jvalue *args)
{
    hk_jclass *c = class_of(cls);
    hk_jobj *o;
    if (!m) return NULL;
    if (is_real(c)) {
        /* A class of the runtime's: the object, then the constructor's bytecode. A string is made whole from the arguments. */
        if (!hk_dvm_ensure_init(c->dvm)) return NULL;
        if (c->dvm == g_dvm.c_string) return hk_dvm_string_init(m->sig, args, &o) ? o : NULL;
        o = hk_dvm_alloc_object(c->dvm);
        if (!o) return NULL;
        invoke_as(o, m, args, true);
        return hk_dvm_pending() ? NULL : o;
    }
    o = new_obj(HK_J_OBJECT, c);
    if (!o->dvm && !strcmp(o->cls->name, "java/lang/String")) {
        o->kind = HK_J_STRING;
        o->u.str.utf8 = hk_strdup("");
    }
    if (m->fn || find_hle(o->cls, m->name, m->sig)) invoke(o, m, args);
    else m->warned = true;             /* a constructor with nothing to do is the common case, not worth a line */
    return o;
}

static hk_jobj *j_NewObjectA(JNIEnvP e, hk_jobj *cls, jmeth *m, const hk_jvalue *args) { (void)e; return construct(cls, m, args); }
static hk_jobj *j_NewObjectV(JNIEnvP e, hk_jobj *cls, jmeth *m, va_list ap)
{
    hk_jvalue args[JARGS];
    (void)e;
    args_from_va(m, ap, args);
    return construct(cls, m, args);
}
static hk_jobj *j_NewObject(JNIEnvP e, hk_jobj *cls, jmeth *m, ...)
{
    hk_jvalue args[JARGS];
    va_list ap;
    (void)e;
    va_start(ap, m);
    args_from_va(m, ap, args);
    va_end(ap);
    return construct(cls, m, args);
}

/*
 * The Call<Type>Method families. Floating-point results go back as bits in
 * integer registers, like every float that crosses to the guest.
 */
#define RET_L(v) return (v).l
/* A field read hands the guest a reference of its own, which it may delete. */
#define RET_LREF(v) return hk_jni_ref((v).l)
#define RET_Z(v) return (v).z
#define RET_B(v) return (v).b
#define RET_C(v) return (v).c
#define RET_S(v) return (v).s
#define RET_I(v) return (v).i
#define RET_J(v) return (v).j
#define RET_F(v) return hk_fb((v).f)
#define RET_D(v) return hk_db((v).d)
#define RET_V(v) (void)(v)

#define CALL_FAMILY(Name, ctype, RET)                                                                       \
    static ctype j_Call##Name##MethodA(JNIEnvP e, hk_jobj *o, jmeth *m, const hk_jvalue *args)              \
    {                                                                                                       \
        hk_jvalue r = invoke(o, m, args);                                                                   \
        (void)e;                                                                                            \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_Call##Name##MethodV(JNIEnvP e, hk_jobj *o, jmeth *m, va_list ap)                         \
    {                                                                                                       \
        hk_jvalue args[JARGS], r;                                                                              \
        (void)e;                                                                                            \
        args_from_va(m, ap, args);                                                                          \
        r = invoke(o, m, args);                                                                             \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_Call##Name##Method(JNIEnvP e, hk_jobj *o, jmeth *m, ...)                                 \
    {                                                                                                       \
        hk_jvalue args[JARGS], r;                                                                              \
        va_list ap;                                                                                         \
        (void)e;                                                                                            \
        va_start(ap, m);                                                                                    \
        args_from_va(m, ap, args);                                                                          \
        va_end(ap);                                                                                         \
        r = invoke(o, m, args);                                                                             \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallNonvirtual##Name##MethodA(JNIEnvP e, hk_jobj *o, hk_jobj *c, jmeth *m, const hk_jvalue *args) \
    {                                                                                                       \
        hk_jvalue r = invoke_as(o, m, args, true);                                                                   \
        (void)e; (void)c;                                                                                   \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallNonvirtual##Name##MethodV(JNIEnvP e, hk_jobj *o, hk_jobj *c, jmeth *m, va_list ap)   \
    {                                                                                                       \
        hk_jvalue args[JARGS], r;                                                                              \
        (void)e; (void)c;                                                                                   \
        args_from_va(m, ap, args);                                                                          \
        r = invoke_as(o, m, args, true);                                                                             \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallNonvirtual##Name##Method(JNIEnvP e, hk_jobj *o, hk_jobj *c, jmeth *m, ...)           \
    {                                                                                                       \
        hk_jvalue args[JARGS], r;                                                                              \
        va_list ap;                                                                                         \
        (void)e; (void)c;                                                                                   \
        va_start(ap, m);                                                                                    \
        args_from_va(m, ap, args);                                                                          \
        va_end(ap);                                                                                         \
        r = invoke_as(o, m, args, true);                                                                             \
        RET(r);                                                                                             \
    }

CALL_FAMILY(Object, hk_jobj *, RET_L)
CALL_FAMILY(Boolean, u8, RET_Z)
CALL_FAMILY(Byte, i8, RET_B)
CALL_FAMILY(Char, u16, RET_C)
CALL_FAMILY(Short, i16, RET_S)
CALL_FAMILY(Int, i32, RET_I)
CALL_FAMILY(Long, i64, RET_J)
CALL_FAMILY(Float, u32, RET_F)
CALL_FAMILY(Double, u64, RET_D)
CALL_FAMILY(Void, void, RET_V)

/* Static calls: the class stands where the object did. */
#define STATIC_FAMILY(Name, ctype, RET)                                                                     \
    static ctype j_CallStatic##Name##MethodA(JNIEnvP e, hk_jobj *c, jmeth *m, const hk_jvalue *args)        \
    {                                                                                                       \
        hk_jvalue r = invoke(NULL, m, args);                                                                \
        (void)e; (void)c;                                                                                   \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallStatic##Name##MethodV(JNIEnvP e, hk_jobj *c, jmeth *m, va_list ap)                   \
    {                                                                                                       \
        hk_jvalue args[JARGS], r;                                                                              \
        (void)e; (void)c;                                                                                   \
        args_from_va(m, ap, args);                                                                          \
        r = invoke(NULL, m, args);                                                                          \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallStatic##Name##Method(JNIEnvP e, hk_jobj *c, jmeth *m, ...)                           \
    {                                                                                                       \
        hk_jvalue args[JARGS], r;                                                                              \
        va_list ap;                                                                                         \
        (void)e; (void)c;                                                                                   \
        va_start(ap, m);                                                                                    \
        args_from_va(m, ap, args);                                                                          \
        va_end(ap);                                                                                         \
        r = invoke(NULL, m, args);                                                                          \
        RET(r);                                                                                             \
    }

STATIC_FAMILY(Object, hk_jobj *, RET_L)
STATIC_FAMILY(Boolean, u8, RET_Z)
STATIC_FAMILY(Byte, i8, RET_B)
STATIC_FAMILY(Char, u16, RET_C)
STATIC_FAMILY(Short, i16, RET_S)
STATIC_FAMILY(Int, i32, RET_I)
STATIC_FAMILY(Long, i64, RET_J)
STATIC_FAMILY(Float, u32, RET_F)
STATIC_FAMILY(Double, u64, RET_D)
STATIC_FAMILY(Void, void, RET_V)

/* Fields. */
static jfield *j_GetFieldID(JNIEnvP e, hk_jobj *cls, const char *name, const char *sig) { (void)e; return get_field(class_of(cls), name, sig, false); }
static jfield *j_GetStaticFieldID(JNIEnvP e, hk_jobj *cls, const char *name, const char *sig) { (void)e; return get_field(class_of(cls), name, sig, true); }

#define FIELD_FAMILY(Name, ctype, member, GET, SETCONV)                                                        \
    static ctype j_Get##Name##Field(JNIEnvP e, hk_jobj *o, jfield *f)                                         \
    {                                                                                                         \
        hk_jvalue v;                                                                                          \
        (void)e;                                                                                              \
        v.j = 0;                                                                                              \
        if (o && f) v = *instance_slot(o, f);                                                                 \
        GET(v);                                                                                               \
    }                                                                                                         \
    static void j_Set##Name##Field(JNIEnvP e, hk_jobj *o, jfield *f, ctype value)                             \
    {                                                                                                         \
        (void)e;                                                                                              \
        if (o && f) instance_slot(o, f)->member = SETCONV(value);                                             \
    }                                                                                                         \
    static ctype j_GetStatic##Name##Field(JNIEnvP e, hk_jobj *c, jfield *f)                                   \
    {                                                                                                         \
        hk_jvalue v;                                                                                          \
        (void)e; (void)c;                                                                                     \
        v.j = 0;                                                                                              \
        if (f) v = *static_slot(f, true);                                                                     \
        GET(v);                                                                                               \
    }                                                                                                         \
    static void j_SetStatic##Name##Field(JNIEnvP e, hk_jobj *c, jfield *f, ctype value)                       \
    {                                                                                                         \
        (void)e; (void)c;                                                                                     \
        if (f) static_slot(f, false)->member = SETCONV(value);                                                \
    }

#define SAME(x) (x)
FIELD_FAMILY(Object, hk_jobj *, l, RET_LREF, hk_jni_ref)
FIELD_FAMILY(Boolean, u8, z, RET_Z, SAME)
FIELD_FAMILY(Byte, i8, b, RET_B, SAME)
FIELD_FAMILY(Char, u16, c, RET_C, SAME)
FIELD_FAMILY(Short, i16, s, RET_S, SAME)
FIELD_FAMILY(Int, i32, i, RET_I, SAME)
FIELD_FAMILY(Long, i64, j, RET_J, SAME)
FIELD_FAMILY(Float, u32, f, RET_F, hk_f)
FIELD_FAMILY(Double, u64, d, RET_D, hk_d)

/* Strings. */
static hk_jobj *j_NewString(JNIEnvP e, const u16 *chars, i32 len)
{
    char *utf8;
    hk_jobj *s;
    (void)e;
    if (g_dvm.running) return hk_dvm_new_string_utf16(chars, len);
    utf8 = from_utf16(chars, len);
    s = hk_jni_new_string(utf8);
    free(utf8);
    return s;
}
static i32 j_GetStringLength(JNIEnvP e, hk_jobj *s)
{
    (void)e;
    if (!s || s->kind != HK_J_STRING) return 0;
    need_utf16(s);
    return s->u.str.len16;
}
static const u16 *j_GetStringChars(JNIEnvP e, hk_jobj *s, u8 *is_copy)
{
    (void)e;
    if (is_copy) *is_copy = 0;
    if (!s || s->kind != HK_J_STRING) return NULL;
    need_utf16(s);
    return s->u.str.u16;
}
static void j_ReleaseStringChars(JNIEnvP e, hk_jobj *s, const u16 *chars) { (void)e; (void)s; (void)chars; }
static hk_jobj *j_NewStringUTF(JNIEnvP e, const char *utf8) { (void)e; return hk_jni_new_string(utf8); }
static i32 j_GetStringUTFLength(JNIEnvP e, hk_jobj *s) { (void)e; return hk_jni_string(s) ? (i32)strlen(hk_jni_string(s)) : 0; }
static const char *j_GetStringUTFChars(JNIEnvP e, hk_jobj *s, u8 *is_copy)
{
    (void)e;
    if (is_copy) *is_copy = 0;
    return hk_jni_string(s);
}
static void j_ReleaseStringUTFChars(JNIEnvP e, hk_jobj *s, const char *chars) { (void)e; (void)s; (void)chars; }
/* A region that is not inside the string: nothing is copied, and with the runtime up it is the exception Android throws for it. */
static bool string_region(hk_jobj *s, i32 start, i32 len)
{
    if (!s || s->kind != HK_J_STRING) return false;
    need_utf16(s);
    if (start >= 0 && len >= 0 && (i64)start + len <= s->u.str.len16) return true;
    if (g_dvm.running) hk_dvm_throw("java/lang/StringIndexOutOfBoundsException", "offset=%d length=%d string.length()=%d", (int)start, (int)len, (int)s->u.str.len16);
    return false;
}
static void j_GetStringRegion(JNIEnvP e, hk_jobj *s, i32 start, i32 len, u16 *buf)
{
    (void)e;
    if (!string_region(s, start, len)) return;
    memcpy(buf, s->u.str.u16 + start, (size_t)len * 2);
}
static void j_GetStringUTFRegion(JNIEnvP e, hk_jobj *s, i32 start, i32 len, char *buf)
{
    char *part;
    (void)e;
    if (!string_region(s, start, len)) return;
    part = from_utf16(s->u.str.u16 + start, len);
    strcpy(buf, part);
    free(part);
}

/* Arrays. */
static i32 j_GetArrayLength(JNIEnvP e, hk_jobj *a)
{
    (void)e;
    if (!a) return 0;
    return a->kind == HK_J_PRIM_ARRAY ? (i32)a->u.arr.len : a->kind == HK_J_OBJ_ARRAY ? (i32)a->u.oarr.len : 0;
}
static hk_jobj *j_NewObjectArray(JNIEnvP e, i32 len, hk_jobj *cls, hk_jobj *init)
{
    hk_jclass *ec = class_of(cls);
    char name[520];
    hk_jobj *a;
    i32 i;
    (void)e;
    /* An array of arrays is named by putting a bracket before the element's own name. */
    if (ec->name[0] == '[') snprintf(name, sizeof(name), "[%s", ec->name);
    else snprintf(name, sizeof(name), "[L%s;", ec->name);
    if (g_dvm.running) {
        if (!ec->dvm) bind_class(ec);
        a = hk_dvm_new_array(name, len);
        for (i = 0; a && i < len; i++) a->u.oarr.v[i] = init && init->dvm ? init : NULL;
        return a;
    }
    a = new_obj(HK_J_OBJ_ARRAY, hk_jni_class(name, NULL));
    a->u.oarr.len = (u32)(len < 0 ? 0 : len);
    a->u.oarr.v = (hk_jobj **)hk_alloc(sizeof(hk_jobj *) * (a->u.oarr.len + 1));
    for (i = 0; i < len; i++) a->u.oarr.v[i] = hk_jni_ref(init);
    return a;
}
static hk_jobj *j_GetObjectArrayElement(JNIEnvP e, hk_jobj *a, i32 i)
{
    (void)e;
    if (!a || a->kind != HK_J_OBJ_ARRAY || i < 0 || (u32)i >= a->u.oarr.len) {
        if (a && a->dvm && a->kind == HK_J_OBJ_ARRAY) hk_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d", (int)a->u.oarr.len, (int)i);
        return NULL;
    }
    return hk_jni_ref(a->u.oarr.v[i]);
}
static void j_SetObjectArrayElement(JNIEnvP e, hk_jobj *a, i32 i, hk_jobj *v)
{
    (void)e;
    if (!a || a->kind != HK_J_OBJ_ARRAY || i < 0 || (u32)i >= a->u.oarr.len) {
        if (a && a->dvm) hk_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d", (int)hk_dvm_array_length(a), (int)i);
        return;
    }
    if (a->dvm && v && (!v->dvm || !hk_dvm_is_instance(v, hk_dvm_class_of(a)->component))) {
        hk_dvm_throw("java/lang/ArrayStoreException", "%s cannot be stored in an array of type %s", hk_jni_class_name(v), hk_jni_class_name(a));
        return;
    }
    hk_jni_ref(v);
    hk_jni_unref(a->u.oarr.v[i]);
    a->u.oarr.v[i] = v;
}

#define PRIM_ARRAY(Name, letter)                                                                            \
    static hk_jobj *j_New##Name##Array(JNIEnvP e, i32 len)                                                  \
    {                                                                                                       \
        (void)e;                                                                                            \
        return hk_jni_new_prim_array(letter, (u32)(len < 0 ? 0 : len));                                     \
    }

PRIM_ARRAY(Boolean, 'Z') PRIM_ARRAY(Byte, 'B') PRIM_ARRAY(Char, 'C') PRIM_ARRAY(Short, 'S')
PRIM_ARRAY(Int, 'I') PRIM_ARRAY(Long, 'J') PRIM_ARRAY(Float, 'F') PRIM_ARRAY(Double, 'D')

/* The elements are handed out in place, for every element type alike. */
static void *j_GetArrayElements(JNIEnvP e, hk_jobj *a, u8 *is_copy)
{
    (void)e;
    if (is_copy) *is_copy = 0;
    return a && a->kind == HK_J_PRIM_ARRAY ? a->u.arr.data : NULL;
}
static void j_ReleaseArrayElements(JNIEnvP e, hk_jobj *a, void *elems, i32 mode) { (void)e; (void)a; (void)elems; (void)mode; }
/* A region that is not inside the array: nothing is copied, and with the runtime up it is the exception Android throws for it. */
static bool array_region(hk_jobj *a, i32 start, i32 len)
{
    if (!a || a->kind != HK_J_PRIM_ARRAY) return false;
    if (start >= 0 && len >= 0 && (u64)(u32)start + (u32)len <= a->u.arr.len) return true;
    if (a->dvm) hk_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "offset=%d length=%d array.length=%d", (int)start, (int)len, (int)a->u.arr.len);
    return false;
}
static void j_GetArrayRegion(JNIEnvP e, hk_jobj *a, i32 start, i32 len, void *buf)
{
    (void)e;
    if (!array_region(a, start, len)) return;
    memcpy(buf, (u8 *)a->u.arr.data + (size_t)start * a->u.arr.esz, (size_t)len * a->u.arr.esz);
}
static void j_SetArrayRegion(JNIEnvP e, hk_jobj *a, i32 start, i32 len, const void *buf)
{
    (void)e;
    if (!array_region(a, start, len)) return;
    memcpy((u8 *)a->u.arr.data + (size_t)start * a->u.arr.esz, buf, (size_t)len * a->u.arr.esz);
}

typedef struct g_native_method {
    u32 name, sig, fn;
} g_native_method;

static i32 j_RegisterNatives(JNIEnvP e, hk_jobj *cls, const g_native_method *methods, i32 count)
{
    hk_jclass *c = class_of(cls);
    i32 i, unknown = 0;
    (void)e;
    for (i = 0; i < count; i++) {
        jnative *n = (jnative *)hk_alloc(sizeof(*n));
        const char *sig = (const char *)(hk_addr)methods[i].sig;
        /* An exclamation mark before the signature asked older Androids for a faster kind of call; it is not part of it. */
        if (sig[0] == '!') sig++;
        n->name = hk_strdup((const char *)(hk_addr)methods[i].name);
        n->sig = hk_strdup(sig);
        n->fn = (void *)(hk_addr)methods[i].fn;
        hk_lock_acquire(&g_jni_lock);
        n->next = c->natives;
        c->natives = n;
        hk_lock_release(&g_jni_lock);
        /* For a class the runtime has, the method must be one the class declares native. Android refuses the table; here it is said and the rest is taken. */
        if (is_real(c) && !hk_dvm_native_registered(c->dvm, n->name, n->sig, n->fn)) {
            unknown++;
            hk_atomic_add(&g_natives_unknown, 1);
            hk_warn("jni: %s has no native method %s%s to register", c->name, n->name, n->sig);
        } else if (is_real(c)) {
            hk_atomic_add(&g_natives_registered, 1);
        }
    }
    hk_dbg("jni: %s registered %d native methods%s", c->name, count, unknown ? ", some of them unknown to the class" : "");
    return 0;
}
static i32 j_UnregisterNatives(JNIEnvP e, hk_jobj *cls) { (void)e; (void)cls; return 0; }
static i32 j_MonitorEnter(JNIEnvP e, hk_jobj *o)
{
    (void)e;
    if (o && o->dvm) hk_dvm_monitor_enter(o);
    return 0;
}
static i32 j_MonitorExit(JNIEnvP e, hk_jobj *o)
{
    (void)e;
    if (o && o->dvm && !hk_dvm_monitor_exit(o)) {
        hk_dvm_throw("java/lang/IllegalMonitorStateException", "MonitorExit on an object this thread has not locked");
        return -1;
    }
    return 0;
}
static i32 j_GetJavaVM(JNIEnvP e, void **vm)
{
    (void)e;
    *vm = hk_jni_vm();
    return 0;
}

typedef struct direct_buffer {
    void *address;
    i64 capacity;
} direct_buffer;

static hk_jobj *j_NewDirectByteBuffer(JNIEnvP e, void *address, i64 capacity)
{
    hk_jobj *o;
    direct_buffer *b;
    (void)e;
    if (g_dvm.running && is_real(hk_jni_class("java/nio/DirectByteBuffer", NULL))) {
        /* libcore's own buffer, made by the constructor it keeps for this: DirectByteBuffer(long address, int capacity). */
        hk_jvalue args[2];
        args[0].j = (i64)(hk_addr)address;
        args[1].j = 0;
        args[1].i = (i32)capacity;
        return hk_dvm_new("java/nio/DirectByteBuffer", "(JI)V", args);
    }
    o = hk_jni_new_object("java/nio/DirectByteBuffer");
    b = (direct_buffer *)hk_alloc(sizeof(*b));
    b->address = address;
    b->capacity = capacity;
    o->native = b;
    return o;
}
hk_jobj *hk_jni_new_direct_buffer(void *address, i64 capacity) { return j_NewDirectByteBuffer(NULL, address, capacity); }

hk_jobj *hk_jni_new_object_array(const char *element_class, u32 len)
{
    return j_NewObjectArray(NULL, (i32)len, hk_jni_class_object(element_class), NULL);
}

/* A buffer of libcore's: its address and capacity are java.nio.Buffer's fields. */
static bool real_buffer(hk_jobj *o) { return o && o->dvm && is_real(o->cls) && hk_dvm_instance_of(o, "java/nio/Buffer"); }

static void *j_GetDirectBufferAddress(JNIEnvP e, hk_jobj *o)
{
    (void)e;
    if (real_buffer(o)) return (void *)(hk_addr)hk_jni_get_field(o, "address").j;
    return o && o->native && !strcmp(hk_jni_class_name(o), "java/nio/DirectByteBuffer") ? ((direct_buffer *)o->native)->address : NULL;
}
static i64 j_GetDirectBufferCapacity(JNIEnvP e, hk_jobj *o)
{
    (void)e;
    if (real_buffer(o)) return hk_jni_get_field(o, "address").j ? hk_jni_get_field(o, "capacity").i : -1;
    return o && o->native && !strcmp(hk_jni_class_name(o), "java/nio/DirectByteBuffer") ? ((direct_buffer *)o->native)->capacity : -1;
}
/*
 * GetObjectRefType: 0 invalid, 1 local, 2 global, 3 weak global. A reference is the object's address here, so the kind is
 * not in the reference: what is kept is, for each object, how many global and how many weak global references native code
 * holds to it (ref_counted), and the answer is the strongest of those: global if there is one, weak if there is only that,
 * local otherwise. It is exact unless a library asks about a local reference to an object it also holds globally.
 */
static i32 j_GetObjectRefType(JNIEnvP e, hk_jobj *o)
{
    const ref_count *r;
    i32 kind = 1;
    (void)e;
    if (!o) return 0;
    hk_lock_acquire(&g_refs_lock);
    r = ref_find(o);
    if (r) kind = r->global > 0 ? 2 : r->weak > 0 ? 3 : 1;
    hk_lock_release(&g_refs_lock);
    return kind;
}

/* ------------------------------------------------------------- the tables */

static void *g_env_table[240];
static void **g_env = g_env_table;         /* a JNIEnv is a pointer to the table pointer */
static void *g_vm_table[8];
static void **g_vm = g_vm_table;

void *hk_jni_env(void) { return &g_env; }
void *hk_jni_vm(void) { return &g_vm; }

/*
 * Threads. A thread Java started, and every thread this layer runs the app
 * on, is attached from its first moment, as Android's are. A thread the app's
 * native code made for itself (pthread_create) is not, until it calls
 * AttachCurrentThread: GetEnv answers JNI_EDETACHED for it, which is how a
 * library finds out that it must attach (and, later, detach). Attaching gives
 * it its java.lang.Thread, named and grouped as it asked (JavaVMAttachArgs),
 * a daemon when it attached as one.
 */
enum { G_JNI_OK = 0, G_JNI_ERR = -1, G_JNI_EDETACHED = -2, G_JNI_EVERSION = -3 };

typedef struct g_attach_args {
    i32 version;
    u32 name, group;                   /* the guest's pointers */
} g_attach_args;

static HK_THREAD_LOCAL bool t_attached;

static bool thread_is_javas(void) { return t_attached || !hk_gthread_self()->start; }
/* ART takes these three and no other (JavaVMExt::IsBadJniVersion). */
static bool bad_jni_version(i32 v) { return v != 0x00010002 && v != 0x00010004 && v != 0x00010006; }

static i32 attach(void **env, const g_attach_args *args, bool daemon)
{
    if (!env) return G_JNI_ERR;
    if (!thread_is_javas()) {
        if (args && bad_jni_version(args->version)) {
            hk_err("jni: bad JNI version passed to AttachCurrentThread: %d", (int)args->version);
            return G_JNI_EVERSION;
        }
        if (!hk_dvm_thread_attach(args ? (const char *)(hk_addr)args->name : NULL, args ? (hk_jobj *)(hk_addr)args->group : NULL, daemon)) {
            *env = NULL;
            return G_JNI_ERR;
        }
        t_attached = true;
    }
    *env = hk_jni_env();
    return G_JNI_OK;
}

static i32 vm_Destroy(void *vm) { (void)vm; return 0; }
static i32 vm_Attach(void *vm, void **env, void *args) { (void)vm; return attach(env, (const g_attach_args *)args, false); }
static i32 vm_AttachAsDaemon(void *vm, void **env, void *args) { (void)vm; return attach(env, (const g_attach_args *)args, true); }
static i32 vm_Detach(void *vm)
{
    (void)vm;
    if (!t_attached) return thread_is_javas() ? G_JNI_OK : G_JNI_ERR;   /* one of Java's own stays what it is */
    hk_dvm_thread_detach();
    t_attached = false;
    return G_JNI_OK;
}
static i32 vm_GetEnv(void *vm, void **env, i32 version)
{
    (void)vm;
    if (!env) return G_JNI_ERR;
    if (!thread_is_javas()) {
        *env = NULL;
        return G_JNI_EDETACHED;
    }
    if (bad_jni_version(version)) {
        hk_err("jni: bad JNI version passed to GetEnv: %d", (int)version);
        return G_JNI_EVERSION;
    }
    *env = hk_jni_env();
    return G_JNI_OK;
}

static i32 g_GetCreatedJavaVMs(void **vms, i32 cap, i32 *count)
{
    if (cap > 0 && vms) vms[0] = hk_jni_vm();
    if (count) *count = 1;
    return 0;
}

static const hk_sym g_tab_jni[] = {
    HK_WRAP("JNI_GetCreatedJavaVMs", g_GetCreatedJavaVMs),
    HK_END
};

#define SLOT(i, f) g_env_table[i] = (void *)(f)
#define SLOT3(i, base) SLOT(i, base), SLOT(i + 1, base##V), SLOT(i + 2, base##A)

void hk_jni_init(void)
{
    static bool done;
    static const char prims[] = "ZBCSIJFD";
    int i;
    if (done) return;
    done = true;
    hk_bionic_register(g_tab_jni);
    hk_jni_class("java/lang/Object", NULL);
    hk_jni_class("java/lang/Class", "java/lang/Object");
    hk_jni_class("java/lang/String", "java/lang/Object");
    hk_jni_class("java/lang/Throwable", "java/lang/Object");
    hk_jni_class("java/lang/Exception", "java/lang/Throwable");
    hk_jni_class("java/lang/RuntimeException", "java/lang/Exception");
    hk_jni_class("java/lang/Error", "java/lang/Throwable");

    SLOT(4, j_GetVersion); SLOT(5, j_DefineClass); SLOT(6, j_FindClass); SLOT(7, j_FromReflectedMethod); SLOT(8, j_FromReflectedField);
    SLOT(9, j_ToReflectedMethod); SLOT(10, j_GetSuperclass); SLOT(11, j_IsAssignableFrom); SLOT(12, j_ToReflectedField); SLOT(13, j_Throw);
    SLOT(14, j_ThrowNew); SLOT(15, j_ExceptionOccurred); SLOT(16, j_ExceptionDescribe); SLOT(17, j_ExceptionClear); SLOT(18, j_FatalError);
    SLOT(19, j_PushLocalFrame); SLOT(20, j_PopLocalFrame); SLOT(21, j_NewGlobalRef); SLOT(22, j_DeleteGlobalRef); SLOT(23, j_DeleteRef);
    SLOT(24, j_IsSameObject); SLOT(25, j_NewRef); SLOT(26, j_EnsureLocalCapacity); SLOT(27, j_AllocObject);
    SLOT(28, j_NewObject); SLOT(29, j_NewObjectV); SLOT(30, j_NewObjectA); SLOT(31, j_GetObjectClass); SLOT(32, j_IsInstanceOf);
    SLOT(33, j_GetMethodID);
    SLOT3(34, j_CallObjectMethod); SLOT3(37, j_CallBooleanMethod); SLOT3(40, j_CallByteMethod); SLOT3(43, j_CallCharMethod);
    SLOT3(46, j_CallShortMethod); SLOT3(49, j_CallIntMethod); SLOT3(52, j_CallLongMethod); SLOT3(55, j_CallFloatMethod);
    SLOT3(58, j_CallDoubleMethod); SLOT3(61, j_CallVoidMethod);
    SLOT3(64, j_CallNonvirtualObjectMethod); SLOT3(67, j_CallNonvirtualBooleanMethod); SLOT3(70, j_CallNonvirtualByteMethod);
    SLOT3(73, j_CallNonvirtualCharMethod); SLOT3(76, j_CallNonvirtualShortMethod); SLOT3(79, j_CallNonvirtualIntMethod);
    SLOT3(82, j_CallNonvirtualLongMethod); SLOT3(85, j_CallNonvirtualFloatMethod); SLOT3(88, j_CallNonvirtualDoubleMethod);
    SLOT3(91, j_CallNonvirtualVoidMethod);
    SLOT(94, j_GetFieldID);
    SLOT(95, j_GetObjectField); SLOT(96, j_GetBooleanField); SLOT(97, j_GetByteField); SLOT(98, j_GetCharField); SLOT(99, j_GetShortField);
    SLOT(100, j_GetIntField); SLOT(101, j_GetLongField); SLOT(102, j_GetFloatField); SLOT(103, j_GetDoubleField);
    SLOT(104, j_SetObjectField); SLOT(105, j_SetBooleanField); SLOT(106, j_SetByteField); SLOT(107, j_SetCharField);
    SLOT(108, j_SetShortField); SLOT(109, j_SetIntField); SLOT(110, j_SetLongField); SLOT(111, j_SetFloatField); SLOT(112, j_SetDoubleField);
    SLOT(113, j_GetStaticMethodID);
    SLOT3(114, j_CallStaticObjectMethod); SLOT3(117, j_CallStaticBooleanMethod); SLOT3(120, j_CallStaticByteMethod);
    SLOT3(123, j_CallStaticCharMethod); SLOT3(126, j_CallStaticShortMethod); SLOT3(129, j_CallStaticIntMethod);
    SLOT3(132, j_CallStaticLongMethod); SLOT3(135, j_CallStaticFloatMethod); SLOT3(138, j_CallStaticDoubleMethod);
    SLOT3(141, j_CallStaticVoidMethod);
    SLOT(144, j_GetStaticFieldID);
    SLOT(145, j_GetStaticObjectField); SLOT(146, j_GetStaticBooleanField); SLOT(147, j_GetStaticByteField); SLOT(148, j_GetStaticCharField);
    SLOT(149, j_GetStaticShortField); SLOT(150, j_GetStaticIntField); SLOT(151, j_GetStaticLongField); SLOT(152, j_GetStaticFloatField);
    SLOT(153, j_GetStaticDoubleField);
    SLOT(154, j_SetStaticObjectField); SLOT(155, j_SetStaticBooleanField); SLOT(156, j_SetStaticByteField); SLOT(157, j_SetStaticCharField);
    SLOT(158, j_SetStaticShortField); SLOT(159, j_SetStaticIntField); SLOT(160, j_SetStaticLongField); SLOT(161, j_SetStaticFloatField);
    SLOT(162, j_SetStaticDoubleField);
    SLOT(163, j_NewString); SLOT(164, j_GetStringLength); SLOT(165, j_GetStringChars); SLOT(166, j_ReleaseStringChars);
    SLOT(167, j_NewStringUTF); SLOT(168, j_GetStringUTFLength); SLOT(169, j_GetStringUTFChars); SLOT(170, j_ReleaseStringUTFChars);
    SLOT(171, j_GetArrayLength); SLOT(172, j_NewObjectArray); SLOT(173, j_GetObjectArrayElement); SLOT(174, j_SetObjectArrayElement);
    SLOT(175, j_NewBooleanArray); SLOT(176, j_NewByteArray); SLOT(177, j_NewCharArray); SLOT(178, j_NewShortArray);
    SLOT(179, j_NewIntArray); SLOT(180, j_NewLongArray); SLOT(181, j_NewFloatArray); SLOT(182, j_NewDoubleArray);
    for (i = 0; i < 8; i++) {
        (void)prims;
        SLOT(183 + i, j_GetArrayElements);
        SLOT(191 + i, j_ReleaseArrayElements);
        SLOT(199 + i, j_GetArrayRegion);
        SLOT(207 + i, j_SetArrayRegion);
    }
    SLOT(215, j_RegisterNatives); SLOT(216, j_UnregisterNatives); SLOT(217, j_MonitorEnter); SLOT(218, j_MonitorExit); SLOT(219, j_GetJavaVM);
    SLOT(220, j_GetStringRegion); SLOT(221, j_GetStringUTFRegion); SLOT(222, j_GetArrayElements); SLOT(223, j_ReleaseArrayElements);
    SLOT(224, j_GetStringChars); SLOT(225, j_ReleaseStringChars); SLOT(226, j_NewWeakGlobalRef); SLOT(227, j_DeleteWeakGlobalRef); SLOT(228, j_ExceptionCheck);
    SLOT(229, j_NewDirectByteBuffer); SLOT(230, j_GetDirectBufferAddress); SLOT(231, j_GetDirectBufferCapacity); SLOT(232, j_GetObjectRefType);

    g_vm_table[3] = (void *)vm_Destroy;
    g_vm_table[4] = (void *)vm_Attach;
    g_vm_table[5] = (void *)vm_Detach;
    g_vm_table[6] = (void *)vm_GetEnv;
    g_vm_table[7] = (void *)vm_AttachAsDaemon;
}
