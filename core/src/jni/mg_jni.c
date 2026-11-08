#include "mg_jni.h"

#include <stdio.h>

#include "../arm/mg_stubs.h"
#include "../bionic/mg_bionic.h"
#include "../dvm/mg_dvm_internal.h"
#include "../ld/mg_ld.h"

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
 * can hold. An mg_jclass says which it is in `dvm` and `foreign`.
 */

/*
 * How many parameters a method called through this layer may have: as many as Java allows (255 words of them). It was 64:
 * WhatsApp's native code calls back JniBridge.jnidispatch with 78 (45 ints and 33 objects), and the last 14 were whatever
 * the stack held -- the interpreter took the int 0x23 for an object and the app ended.
 */
#define JARGS 256

/* ---------------------------------------------------------------- classes */

typedef struct jmeth {
    mg_jclass *cls;
    char *name, *sig;
    bool is_static;
    mg_jhle_fn fn;
    char *argk;                        /* one kind letter per argument: Z B C S I J F D L */
    int nargs;
    char retk;
    bool warned;
    mg_dvm_method *dvm;                /* the runtime's method, for a class it has */
    struct jmeth *next;
} jmeth;

typedef struct jfield {
    mg_jclass *cls;
    char *name;
    char kind;
    bool is_static;
    mg_jvalue value;                   /* a static field's */
    bool has_value, warned;            /* a static field's: whether anything ever gave it one, and whether its absence has been said */
    mg_dvm_field *dvm;                 /* the runtime's field, for a class it has: its value is in the object's slots, or the class's */
    struct jfield *next;
    struct jfield *next_ref;           /* a stand-in's static field of reference type: the next such (g_static_refs) */
} jfield;

typedef struct jnative {
    char *name, *sig;
    void *fn;
    struct jnative *next;
} jnative;

struct mg_jclass {
    char *name;
    mg_jclass *super;
    mg_jobj mirror;
    jmeth *meths;
    jfield *fields;
    jnative *natives;
    mg_dvm_class *dvm;                 /* while the runtime runs: its class of this name */
    bool foreign;                      /* ...which is only a stand-in: no DEX file defines the class, it is this layer's own */
    mg_jclass *hash_next;
};

#define CLASS_BUCKETS 1024
static mg_jclass *g_class_hash[CLASS_BUCKETS];
static mg_lock g_jni_lock = MG_LOCK_INIT;
static const mg_jhle *g_tables[32];
static int g_ntables;
static MG_THREAD_LOCAL mg_jobj *t_pending;

void mg_jni_register(const mg_jhle *table)
{
    if (g_ntables < (int)MG_COUNT(g_tables)) g_tables[g_ntables++] = table;
}

static u32 name_hash(const char *name)
{
    u32 h = 2166136261u;
    for (; *name; name++) h = (h ^ (u8)*name) * 16777619u;
    return h % CLASS_BUCKETS;
}

static mg_jclass *find_class(const char *name)
{
    mg_jclass *c;
    for (c = g_class_hash[name_hash(name)]; c; c = c->hash_next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* The class of that name, made if it is new. Nothing else: not its superclass, not its place in the runtime. */
static mg_jclass *declare(const char *name)
{
    mg_jclass *c;
    u32 h = name_hash(name);
    mg_lock_acquire(&g_jni_lock);
    c = find_class(name);
    if (!c) {
        c = (mg_jclass *)mg_alloc(sizeof(*c));
        c->name = mg_strdup(name);
        c->mirror.refs = 1;
        c->mirror.kind = MG_J_CLASS;
        c->mirror.permanent = true;
        c->mirror.u.klass.jc = c;
        c->hash_next = g_class_hash[h];
        g_class_hash[h] = c;
    }
    mg_lock_release(&g_jni_lock);
    return c;
}

/*
 * Gives a class its place in the runtime, once that runs: the runtime's own
 * class when a DEX file defines the name (linked now; if that fails the
 * exception is left pending and the class stays unbound), a stand-in
 * otherwise. Never called with g_jni_lock held: linking comes back here
 * through jni_class_for.
 */
static void bind_class(mg_jclass *c)
{
    const char *e = c->name;
    mg_dvm_class *d;
    char inner[512];
    while (*e == '[') e++;
    if (e != c->name && *e == 'L') {
        /* An array of objects: its elements' class first, which may be one of this layer's own. */
        size_t n = strlen(e);
        if (n < 3 || n - 2 >= sizeof(inner)) return;
        memcpy(inner, e + 1, n - 2);
        inner[n - 2] = 0;
        if (!mg_jni_class(inner, NULL)->dvm) return;
    }
    if (e != c->name || mg_dvm_class_exists(c->name)) d = mg_dvm_find_class(c->name);
    else d = mg_dvm_foreign_class(c->name, c);
    if (!d) return;
    if (!d->jc) d->jc = c;
    c->foreign = d->foreign;
    c->dvm = d;
}

mg_jclass *mg_jni_class(const char *name, const char *super)
{
    mg_jclass *c = declare(name);
    if (super && !c->super && strcmp(name, super)) c->super = mg_jni_class(super, NULL);
    if (!c->mirror.cls && strcmp(name, "java/lang/Class")) c->mirror.cls = mg_jni_class("java/lang/Class", NULL);
    if (!c->dvm && g_dvm.running) bind_class(c);
    return c;
}

/* What the runtime asks when it has linked a class: this layer's class of that name, which from now on is that one. */
static mg_jclass *jni_class_for(const char *name, const char *super, mg_dvm_class *d)
{
    mg_jclass *c;
    if (d->scope) {
        /* Do not overwrite the client's JNI class wrapper for this same name. */
        c = (mg_jclass *)mg_alloc(sizeof(*c));
        c->name = mg_strdup(name);
        c->mirror.refs = 1; c->mirror.kind = MG_J_CLASS; c->mirror.permanent = true;
        c->mirror.u.klass.jc = c;
        c->super = d->super ? d->super->jc : NULL;
    } else c = declare(name);
    if (super && !c->super && strcmp(name, super)) c->super = declare(super);
    c->foreign = d->foreign;
    c->dvm = d;
    return c;
}

/* The java.lang.Class object: the runtime's while it runs, so that bytecode can be handed it. */
static mg_jobj *mirror_of(mg_jclass *c)
{
    mg_jobj *m;
    if (!c->dvm && g_dvm.running) bind_class(c);
    m = c->dvm ? mg_dvm_mirror(c->dvm) : NULL;
    return m ? m : &c->mirror;
}

/* Whether the runtime answers for the class: a DEX file defines it. */
static bool is_real(const mg_jclass *c) { return c && c->dvm && !c->foreign; }

mg_jobj *mg_jni_class_object(const char *name) { return mirror_of(mg_jni_class(name, NULL)); }

const char *mg_jni_class_name(const mg_jobj *o)
{
    if (!o) return "null";
    if (o->kind == MG_J_CLASS) return o->u.klass.jc ? o->u.klass.jc->name : "?";
    return o->cls ? o->cls->name : "?";
}

/* ----------------------------------------------------- the pending exception */

/* One exception can be waiting on a thread: the runtime keeps it while it runs. */
static mg_jobj *pending(void) { return g_dvm.running ? mg_dvm_pending() : t_pending; }

static void set_pending(mg_jobj *e)
{
    if (!g_dvm.running) t_pending = e;
    else if (!e) mg_dvm_clear_pending();
    else if (e->dvm) mg_dvm_throw_object(e);
    else mg_warn("jni: an exception of %s made before the runtime started cannot be thrown in it", mg_jni_class_name(e));
}

/* --------------------------------------------------------------- objects */

static mg_jobj *new_obj(int kind, mg_jclass *cls)
{
    mg_jobj *o;
    if (g_dvm.running && kind == MG_J_OBJECT) {
        /* While the runtime runs every object is one of its own, also those of classes that are only this layer's. */
        if (!cls->dvm) bind_class(cls);
        if (cls->dvm) {
            o = mg_dvm_alloc_object(cls->dvm);
            if (o) return o;
        }
        mg_warn("jni: an object of %s could not be made in the runtime", cls->name);
    }
    o = (mg_jobj *)mg_alloc(sizeof(*o));
    o->refs = 1;
    o->kind = (u8)kind;
    o->cls = cls;
    return o;
}

mg_jobj *mg_jni_new_object(const char *cls) { return new_obj(MG_J_OBJECT, mg_jni_class(cls, NULL)); }

mg_jobj *mg_jni_new_string(const char *utf8)
{
    mg_jobj *o;
    if (!utf8) return NULL;
    if (g_dvm.running) return mg_dvm_new_string(utf8);
    o = new_obj(MG_J_STRING, mg_jni_class("java/lang/String", NULL));
    o->u.str.utf8 = mg_strdup(utf8);
    return o;
}

const char *mg_jni_string(mg_jobj *s)
{
    if (!s || s->kind != MG_J_STRING) return NULL;
    /* The runtime's strings are UTF-16; their UTF-8 is made when first asked for and kept. */
    return s->u.str.utf8 || !s->dvm ? s->u.str.utf8 : mg_dvm_string_utf8(s);
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

mg_jobj *mg_jni_new_prim_array(char etype, u32 len)
{
    char name[3] = { '[', etype, 0 };
    mg_jobj *o;
    if (g_dvm.running) return mg_dvm_new_array(name, (i32)len);
    o = new_obj(MG_J_PRIM_ARRAY, mg_jni_class(name, NULL));
    o->u.arr.etype = etype;
    o->u.arr.esz = elem_size(etype);
    o->u.arr.len = len;
    o->u.arr.data = mg_alloc((size_t)len * o->u.arr.esz + 8);
    return o;
}

mg_jobj *mg_jni_ref(mg_jobj *o)
{
    if (o && !o->permanent) mg_atomic_add(&o->refs, 1);
    return o;
}

void mg_jni_unref(mg_jobj *o)
{
    u32 i;
    int k;
    if (!o || o->permanent) return;
    if (mg_atomic_add(&o->refs, -1) > 0) return;
    /* Strings and arrays are made and dropped all the time; plain objects are few and may be kept by C code that never took a reference. */
    if (o->kind == MG_J_OBJECT) return;
    if (o->kind == MG_J_STRING) {
        free(o->u.str.utf8);
        free(o->u.str.u16);
    } else if (o->kind == MG_J_PRIM_ARRAY) {
        free(o->u.arr.data);
    } else if (o->kind == MG_J_OBJ_ARRAY) {
        for (i = 0; i < o->u.oarr.len; i++) mg_jni_unref(o->u.oarr.v[i]);
        free(o->u.oarr.v);
    }
    for (k = 0; k < o->nfields; k++) free(o->fields[k].name);
    free(o->fields);
    free(o);
}

void mg_jni_throw(const char *cls, const char *msg)
{
    mg_jobj *e;
    if (g_dvm.running && is_real(mg_jni_class(cls, NULL))) {
        /* One of the runtime's own exception classes: made as bytecode makes it, constructor and all. */
        mg_dvm_throw(cls, "%s", msg ? msg : "");
        return;
    }
    e = mg_jni_new_object(cls);
    e->native = mg_strdup(msg ? msg : "");
    mg_warn("jni: %s: %s", cls, msg ? msg : "");
    set_pending(e);
}

/* ---------------------------------------------------------- UTF-8, UTF-16 */

static u16 *to_utf16(const char *s, i32 *len)
{
    size_t n = strlen(s), i = 0, k = 0;
    u16 *out = (u16 *)mg_alloc((n + 1) * 2);
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
    char *out = (char *)mg_alloc((size_t)len * 3 + 1);
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

static void need_utf16(mg_jobj *s)
{
    if (!s->u.str.u16) s->u.str.u16 = to_utf16(s->u.str.utf8, &s->u.str.len16);
}

void mg_jni_string_set(mg_jobj *s, const char *utf8)
{
    if (!s || s->kind != MG_J_STRING) return;
    if (s->dvm) {
        /* A string of the runtime's is made whole (construct): its text cannot be given afterwards. */
        mg_err("jni: mg_jni_string_set on a string of the runtime's");
        return;
    }
    free(s->u.str.utf8);
    free(s->u.str.u16);
    s->u.str.u16 = NULL;
    s->u.str.len16 = 0;
    s->u.str.utf8 = mg_strdup(utf8 ? utf8 : "");
}

void mg_jni_string_set_utf16(mg_jobj *s, const u16 *chars, i32 len)
{
    char *utf8 = from_utf16(chars, len);
    mg_jni_string_set(s, utf8);
    free(utf8);
}

const u16 *mg_jni_string_utf16(mg_jobj *s, i32 *len)
{
    if (!s || s->kind != MG_J_STRING) {
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

static mg_jhle_fn find_hle(const mg_jclass *cls, const char *name, const char *sig)
{
    const mg_jclass *c;
    int t;
    for (c = cls; c; c = c->super) {
        for (t = g_ntables - 1; t >= 0; t--) {
            const mg_jhle *h;
            for (h = g_tables[t]; h->cls; h++)
                if (!strcmp(h->name, name) && !strcmp(h->cls, c->name) && (!h->sig || !strcmp(h->sig, sig))) return h->fn;
        }
    }
    return NULL;
}

static jmeth *get_method(mg_jclass *cls, const char *name, const char *sig, bool is_static)
{
    mg_dvm_method *dm = NULL;
    jmeth *m;
    if (is_real(cls)) {
        /* A class the runtime has: the method is there or it is not, as on Android, and asking initialises the class. */
        if (!mg_dvm_ensure_init(cls->dvm)) return NULL;
        dm = mg_dvm_find_method(cls->dvm, name, sig, is_static);
        if (!dm) {
            mg_dvm_throw("java/lang/NoSuchMethodError", "no %s method \"%s.%s%s\"", is_static ? "static" : "non-static", cls->name, name, sig);
            return NULL;
        }
    }
    mg_lock_acquire(&g_jni_lock);
    for (m = cls->meths; m; m = m->next)
        if (m->is_static == is_static && !strcmp(m->name, name) && !strcmp(m->sig, sig)) break;
    if (!m) {
        m = (jmeth *)mg_alloc(sizeof(*m));
        m->cls = cls;
        m->name = mg_strdup(name);
        m->sig = mg_strdup(sig);
        m->is_static = is_static;
        {
            char kinds[JARGS];
            parse_sig(sig, kinds, JARGS, &m->nargs, &m->retk);
            m->argk = (char *)mg_alloc((size_t)m->nargs + 1);
            memcpy(m->argk, kinds, (size_t)m->nargs);
        }
        m->next = cls->meths;
        cls->meths = m;
    }
    mg_lock_release(&g_jni_lock);
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
 * buffer of its own (which mg_dvm_object_dying frees, `native` holding it), its hash and kept UTF-8 forgotten. Native code
 * that makes a string so and then uses the object it allocated -- the app-lock library of VK mp3 mod -- sees the string.
 */
static void string_init_on(mg_jobj *self, jmeth *m, const mg_jvalue *args)
{
    mg_jobj *made = NULL;
    u16 *text;
    if (!self || self->kind != MG_J_STRING) {
        mg_dvm_throw("java/lang/InstantiationException", "java.lang.String's constructor called through JNI on an object that is no string AllocObject made");
        return;
    }
    if (!mg_dvm_string_init(m->sig, args, &made) || !made) return;
    text = (u16 *)mg_alloc(((size_t)(u32)made->u.str.len16 + 1) * 2);
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
    if (g_dvm.c_string->nslots > 0) mg_dvm_slots(self)[0].j = 0;   /* libcore's String: its one field, the hash, made again when asked */
}

static mg_jvalue invoke_as(mg_jobj *self, jmeth *m, const mg_jvalue *args, bool exact)
{
    mg_jcall c;
    memset(&c, 0, sizeof(c));
    if (!m) return c.ret;                                        /* the lookup failed and said so; the caller went on regardless */
    if (m->dvm && !m->is_static && m->dvm->cls == g_dvm.c_string && !strcmp(m->name, "<init>")) {
        string_init_on(self, m, args);
        return c.ret;
    }
    if (m->dvm) {
        mg_dvm_method *target = m->dvm;
        mg_jvalue clean[JARGS];
        int i;
        if (m->is_static) {
            if (!mg_dvm_ensure_init(target->cls)) return c.ret;
        } else if (!self || !self->dvm) {
            mg_dvm_throw("java/lang/NullPointerException", "%s.%s%s called through JNI on %s", m->cls->name, m->name, m->sig, self ? "an object that is not the runtime's" : "null");
            return c.ret;
        } else if (!exact) {
            target = mg_dvm_virtual_target(target, self);
        }
        /* An object made before the runtime started is not an object to it: it gets null in its place. */
        for (i = 0; i < m->nargs; i++)
            if (m->argk[i] == 'L' && args[i].l && !args[i].l->dvm) break;
        if (i < m->nargs) {
            memcpy(clean, args, sizeof(mg_jvalue) * (size_t)m->nargs);
            for (i = 0; i < m->nargs; i++)
                if (m->argk[i] == 'L' && clean[i].l && !clean[i].l->dvm) clean[i].l = NULL;
            args = clean;
        }
        mg_dvm_invoke(target, m->is_static ? NULL : self, args, &c.ret);
        return c.ret;
    }
    mg_trace("java %s.%s%s", self && self->cls ? self->cls->name : m->cls->name, m->name, m->sig);
    /* The object's own class may implement what the declared one does not. */
    if (!m->fn && self && self->cls && self->cls != m->cls) m->fn = find_hle(self->cls, m->name, m->sig);
    if (!m->fn) {
        if (!m->warned) {
            m->warned = true;
            mg_warn("jni: %s.%s%s is not implemented (returns zero)", self && self->cls ? self->cls->name : m->cls->name, m->name, m->sig);
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

static mg_jvalue invoke(mg_jobj *self, jmeth *m, const mg_jvalue *args) { return invoke_as(self, m, args, false); }

static void args_from_va(const jmeth *m, va_list ap, mg_jvalue *out)
{
    int i;
    if (!m) return;
    for (i = 0; i < m->nargs; i++) {
        out[i].j = 0;
        switch (m->argk[i]) {
        case 'J': out[i].j = va_arg(ap, i64); break;
        case 'D': out[i].d = va_arg(ap, double); break;
        case 'F': out[i].f = (float)va_arg(ap, double); break;       /* a float is passed to "..." as a double */
        case 'L': out[i].l = va_arg(ap, mg_jobj *); break;
        default: out[i].i = va_arg(ap, i32); break;
        }
    }
}

/*
 * The stand-in classes' static fields of reference type, roots of the collector's (mg_jni_gc_roots), newest first. It
 * went over every field of every class this layer had: 5.5 ms of each stop of Spotify's on a Lumia 650, for none of them.
 */
static jfield *g_static_refs;

static jfield *get_field(mg_jclass *cls, const char *name, const char *sig, bool is_static)
{
    mg_dvm_field *df = NULL;
    mg_jclass *c;
    jfield *f;
    if (is_real(cls)) {
        if (!mg_dvm_ensure_init(cls->dvm)) return NULL;
        df = mg_dvm_find_field_typed(cls->dvm, name, sig, is_static);
        if (!df) {
            mg_dvm_throw("java/lang/NoSuchFieldError", "no \"%s\" field \"%s\" in class \"%s\" or its superclasses", sig, name, cls->name);
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
    f = (jfield *)mg_alloc(sizeof(*f));
    f->cls = cls;
    f->name = mg_strdup(name);
    f->kind = (sig[0] == '[') ? 'L' : sig[0];
    f->is_static = is_static;
    f->dvm = df;
    mg_lock_acquire(&g_jni_lock);
    f->next = cls->fields;
    cls->fields = f;
    if (is_static && !df && f->kind == 'L') {
        f->next_ref = g_static_refs;
        g_static_refs = f;
    }
    mg_lock_release(&g_jni_lock);
    return f;
}

void mg_jni_set_static(const char *cls, const char *name, char kind, mg_jvalue v)
{
    char sig[2] = { kind, 0 };
    mg_jclass *c = mg_jni_class(cls, NULL);
    jfield *f;
    if (is_real(c)) {
        mg_dvm_set_static(cls, name, v);
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
        mg_warn("jni: the static field %s.%s was read and nothing has given it a value (the app gets %s)", f->cls ? f->cls->name : "?", f->name,
                f->kind == 'L' ? "null" : "zero");
    }
    return f;
}

/* Where a static field's value is: in the runtime's class, or here. */
static mg_jvalue *static_slot(jfield *f, bool reading)
{
    if (f->dvm) return mg_dvm_ensure_init(f->dvm->cls) ? &f->dvm->cls->statics[f->dvm->slot] : &f->value;
    if (reading) static_read(f);
    else f->has_value = true;
    return &f->value;
}

static mg_jvalue *instance_slot(mg_jobj *o, const jfield *f)
{
    static mg_jvalue nowhere;
    int i;
    if (f->dvm) {
        /* A field of a class the runtime has: one of the object's slots, if the object is of such a class at all. */
        if (o->dvm && f->dvm->slot < mg_dvm_class_of(o)->nslots) return &mg_dvm_slots(o)[f->dvm->slot];
        nowhere.j = 0;
        return &nowhere;
    }
    for (i = 0; i < o->nfields; i++)
        if (!strcmp(o->fields[i].name, f->name)) return &o->fields[i].v;
    /* (The collector follows the fields of the runtime's objects that have them apart: mg_dvm_gc.c.) */
    if (o->dvm && !o->nfields) mg_dvm_gc_has_fields(o);
    o->fields = (mg_jfield_value *)mg_realloc(o->fields, sizeof(mg_jfield_value) * (size_t)(o->nfields + 1));
    memset(&o->fields[o->nfields], 0, sizeof(mg_jfield_value));
    o->fields[o->nfields].name = mg_strdup(f->name);
    o->fields[o->nfields].is_object = f->kind == 'L';
    return &o->fields[o->nfields++].v;
}

void mg_jni_set_field(mg_jobj *o, const char *name, char kind, mg_jvalue v)
{
    char sig[2] = { kind, 0 };
    if (!o || !o->cls) return;
    if (is_real(o->cls)) mg_dvm_set_field(o, name, v);
    else *instance_slot(o, get_field(o->cls, name, sig, false)) = v;
}

mg_jvalue mg_jni_get_field(mg_jobj *o, const char *name)
{
    mg_jvalue none;
    int i;
    none.j = 0;
    if (!o) return none;
    if (is_real(o->cls)) {
        mg_dvm_get_field(o, name, &none);
        return none;
    }
    for (i = 0; i < o->nfields; i++)
        if (!strcmp(o->fields[i].name, name)) return o->fields[i].v;
    return none;
}

bool mg_jni_exception_clear(void)
{
    bool was = pending() != NULL;
    set_pending(NULL);
    return was;
}

/* ------------------------------------------------------------- reflection */

typedef struct reflected {
    jmeth *m;
    mg_jobj *object;
    struct reflected *next;
} reflected;

static reflected *g_reflected;

mg_jobj *mg_jni_reflect_method(const char *cls, const char *name, const char *sig, bool is_static)
{
    jmeth *m = get_method(mg_jni_class(cls, NULL), name, sig, is_static);
    reflected *r;
    mg_jobj *o;
    mg_lock_acquire(&g_jni_lock);
    for (r = g_reflected; r; r = r->next)
        if (r->m == m) break;
    mg_lock_release(&g_jni_lock);
    if (r) return r->object;
    o = mg_jni_new_object("java/lang/reflect/Method");
    o->native = m;
    o->permanent = true;
    r = (reflected *)mg_alloc(sizeof(*r));
    r->m = m;
    r->object = o;
    mg_lock_acquire(&g_jni_lock);
    r->next = g_reflected;
    g_reflected = r;
    mg_lock_release(&g_jni_lock);
    return o;
}

static const jmeth *reflected_method(const mg_jobj *method)
{
    return method && method->kind == MG_J_OBJECT && method->cls && !strcmp(method->cls->name, "java/lang/reflect/Method") ? (const jmeth *)method->native
                                                                                                                         : NULL;
}
const char *mg_jni_reflected_name(const mg_jobj *method) { const jmeth *m = reflected_method(method); return m ? m->name : NULL; }
const char *mg_jni_reflected_sig(const mg_jobj *method) { const jmeth *m = reflected_method(method); return m ? m->sig : NULL; }

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

void mg_jni_native_counts(i32 *registered, i32 *unknown, i32 *by_name)
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
 * of two kinds, the app's and the runtime's own (ld/mg_ld.h), and the one
 * whose class it is comes first: a class of the app's finds its methods in
 * the app's libraries, a class of the runtime's in the runtime's. ART looks
 * nowhere else -- only in the libraries of the class's own loader; here the
 * other kind is still looked in last, since which DEX file is the app's is
 * only known where the Java driver says so. Within a kind, the library loaded
 * first wins when two export one name (ART: the one whose path sorts first).
 */
static void *exported_native(const char *symbol, bool app_class)
{
    void *fn = mg_ld_sym_among(!app_class, symbol, NULL);
    return fn ? fn : mg_ld_sym_among(app_class, symbol, NULL);
}

static void *find_native(const char *cls, const char *name, const char *sig, bool app_class)
{
    mg_jclass *c = mg_jni_class(cls, NULL);
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

void *mg_jni_native(const char *cls, const char *name, const char *sig) { return find_native(cls, name, sig, true); }

static void native_tried(const char *cls, const char *name, const char *sig, char *out, size_t n)
{
    char brief[1024], full[1024];
    native_names(cls, name, sig, brief, full, sizeof(brief));
    snprintf(out, n, "%s%s%s", brief, full[0] ? " and " : "", full);
}

#if MG_ARCH_ARM32
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
    if (n <= 24) return MG_GFN(fn24, fn)(A8(0), A8(8), A8(16));
    if (n <= 56) return MG_GFN(fn56, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48));
    return MG_GFN(fn120, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48), A8(56), A8(64), A8(72), A8(80), A8(88), A8(96), A8(104), A8(112));
}

/* `bare`: a @CriticalNative method, which is given its arguments alone: no JNIEnv, no class. */
static mg_jvalue call_native(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args, bool bare)
{
    char argk[JARGS], retk;
    u32 w[NATIVE_WORDS];
    int nargs, n = 0, i;
    u64 r;
    mg_jvalue ret;
    ret.j = 0;
    parse_sig(sig, argk, (int)sizeof(argk), &nargs, &retk);
    if (!bare) {
        w[n++] = (u32)(mg_addr)mg_jni_env();
        w[n++] = (u32)(mg_addr)self_or_class;
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
            else if (argk[i] == 'L') w[n++] = (u32)(mg_addr)args[i].l;
            else if (argk[i] == 'Z' || argk[i] == 'B') w[n++] = (u32)(i32)args[i].b;
            else if (argk[i] == 'C') w[n++] = args[i].c;
            else if (argk[i] == 'S') w[n++] = (u32)(i32)args[i].s;
            else w[n++] = (u32)args[i].i;
        }
    }
    if (i < nargs) {
        mg_err("jni: a native method with more arguments than can be passed (%s)", sig);
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
    case 'L': ret.l = (mg_jobj *)(mg_addr)(u32)r; break;
    default: ret.i = (i32)(u32)r; break;
    }
    return ret;
}
#else
/*
 * The other processors, where the core's own convention is the guest's (docs/DESKTOP.md): the arguments sorted as that
 * convention sorts them, and the call made through a pointer typed so that the compiler puts each where the method looks.
 *
 *   x86_64 (System V): the first six integers and pointers in rdi..r9, the first eight floats and doubles in xmm0-7, the
 *     rest on the stack in their order, eight bytes each;
 *   ARM64 (AAPCS64, as Android has it): the same with eight integers in x0-x7 and eight in v0-v7;
 *   x86 (cdecl): everything on the stack in order, four bytes each, a long or a double eight.
 *
 * On the two 64-bit ones the pointer's type is `n` integers, eight doubles and then the stack's words as variable
 * arguments: Android's convention passes those exactly as it passes named ones once the registers are full, so they land
 * on the stack in order. A float goes into its register as a double whose low half is its bits, which is where the
 * method reads it. The result comes from rax / x0, or from xmm0 / d0 through a pointer typed to return a double; on x86
 * from eax:edx, or st(0).
 */
#if MG_ARCH_X86_64
#define GP_REGS 6
#else
#define GP_REGS 8
#endif
#define FP_REGS 8
#define STACK_WORDS 120

#if MG_GUEST_LP64
static double fp_bits(u64 bits) { double d; memcpy(&d, &bits, 8); return d; }
#endif

/* `bare`: a @CriticalNative method, which is given its arguments alone: no JNIEnv, no class. */
static mg_jvalue call_native(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args, bool bare)
{
    char argk[JARGS], retk;
    int nargs, i;
    mg_jvalue ret;
#if MG_GUEST_LP64
    u64 gp[GP_REGS], st[STACK_WORDS];
    double fp[FP_REGS];
    int ngp = 0, nfp = 0, nst = 0;
    u64 r = 0;
    double rd = 0;
    ret.j = 0;
    parse_sig(sig, argk, (int)sizeof(argk), &nargs, &retk);
    memset(gp, 0, sizeof(gp));
    memset(fp, 0, sizeof(fp));
    if (!bare) {
        gp[ngp++] = (u64)(mg_addr)mg_jni_env();
        gp[ngp++] = (u64)(mg_addr)self_or_class;
    }
    for (i = 0; i < nargs; i++) {
        u64 v;
        bool fpc = argk[i] == 'F' || argk[i] == 'D';
        if (argk[i] == 'F') { u32 b; memcpy(&b, &args[i].f, 4); v = b; }
        else if (argk[i] == 'D' || argk[i] == 'J') memcpy(&v, &args[i], 8);
        else if (argk[i] == 'L') v = (u64)(mg_addr)args[i].l;
        else if (argk[i] == 'Z') v = (u64)(u8)args[i].z;
        else if (argk[i] == 'B') v = (u64)(i64)args[i].b;
        else if (argk[i] == 'C') v = (u64)args[i].c;
        else if (argk[i] == 'S') v = (u64)(i64)args[i].s;
        else v = (u64)(i64)args[i].i;
        if (fpc && nfp < FP_REGS) fp[nfp++] = fp_bits(v);
        else if (!fpc && ngp < GP_REGS) gp[ngp++] = v;
        else if (nst < STACK_WORDS) st[nst++] = v;
        else break;
    }
    if (i < nargs) {
        mg_err("jni: a native method with more arguments than can be passed (%s)", sig);
        return ret;
    }
#define S8(b) st[b], st[(b) + 1], st[(b) + 2], st[(b) + 3], st[(b) + 4], st[(b) + 5], st[(b) + 6], st[(b) + 7]
#if MG_ARCH_X86_64
#define GPS gp[0], gp[1], gp[2], gp[3], gp[4], gp[5]
    typedef u64 (*int_fn)(u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double, ...);
    typedef double (*fp_fn)(u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double, ...);
#else
#define GPS gp[0], gp[1], gp[2], gp[3], gp[4], gp[5], gp[6], gp[7]
    typedef u64 (*int_fn)(u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double, ...);
    typedef double (*fp_fn)(u64, u64, u64, u64, u64, u64, u64, u64, double, double, double, double, double, double, double, double, ...);
#endif
#define FPS fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7]
    /* (The stack's words past the arguments are zero, and the method does not read them. Three sizes, as on ARM.) */
    {
        int pass = nst <= 8 ? 8 : nst <= 32 ? 32 : STACK_WORDS;
        while (nst < pass) st[nst++] = 0;
    }
    if (retk == 'F' || retk == 'D') {
        if (nst <= 8) rd = MG_GFN(fp_fn, fn)(GPS, FPS, S8(0));
        else if (nst <= 32) rd = MG_GFN(fp_fn, fn)(GPS, FPS, S8(0), S8(8), S8(16), S8(24));
        else rd = MG_GFN(fp_fn, fn)(GPS, FPS, S8(0), S8(8), S8(16), S8(24), S8(32), S8(40), S8(48), S8(56), S8(64), S8(72), S8(80), S8(88),
                                     S8(96), S8(104), S8(112));
        memcpy(&r, &rd, 8);
    } else {
        if (nst <= 8) r = MG_GFN(int_fn, fn)(GPS, FPS, S8(0));
        else if (nst <= 32) r = MG_GFN(int_fn, fn)(GPS, FPS, S8(0), S8(8), S8(16), S8(24));
        else r = MG_GFN(int_fn, fn)(GPS, FPS, S8(0), S8(8), S8(16), S8(24), S8(32), S8(40), S8(48), S8(56), S8(64), S8(72), S8(80), S8(88),
                                     S8(96), S8(104), S8(112));
    }
#undef GPS
#undef FPS
#undef S8
    switch (retk) {
    case 'V': break;
    case 'J': case 'D': memcpy(&ret, &r, 8); break;
    case 'F': { u32 lo = (u32)r; memcpy(&ret.f, &lo, 4); break; }
    case 'L': ret.l = (mg_jobj *)(mg_addr)r; break;
    case 'Z': ret.i = (u8)r; break;
    case 'B': ret.i = (i8)r; break;
    case 'C': ret.i = (u16)r; break;
    case 'S': ret.i = (i16)r; break;
    default: ret.i = (i32)(u32)r; break;
    }
    return ret;
#else
    /* x86: words of four bytes on the stack, a 64-bit value in two of them in order, nothing aligned. */
    u32 w[STACK_WORDS];
    int n = 0;
    u64 r;
    ret.j = 0;
    parse_sig(sig, argk, (int)sizeof(argk), &nargs, &retk);
    if (!bare) {
        w[n++] = (u32)(mg_addr)mg_jni_env();
        w[n++] = (u32)(mg_addr)self_or_class;
    }
    for (i = 0; i < nargs; i++) {
        if (argk[i] == 'J' || argk[i] == 'D') {
            u64 v;
            if (n + 2 > STACK_WORDS) break;
            memcpy(&v, &args[i], 8);
            w[n++] = (u32)v;
            w[n++] = (u32)(v >> 32);
        } else {
            if (n + 1 > STACK_WORDS) break;
            if (argk[i] == 'F') memcpy(&w[n++], &args[i].f, 4);
            else if (argk[i] == 'L') w[n++] = (u32)(mg_addr)args[i].l;
            else if (argk[i] == 'Z') w[n++] = (u32)(u8)args[i].z;
            else if (argk[i] == 'B') w[n++] = (u32)(i32)args[i].b;
            else if (argk[i] == 'C') w[n++] = args[i].c;
            else if (argk[i] == 'S') w[n++] = (u32)(i32)args[i].s;
            else w[n++] = (u32)args[i].i;
        }
    }
    if (i < nargs) {
        mg_err("jni: a native method with more arguments than can be passed (%s)", sig);
        return ret;
    }
#define W8 u32, u32, u32, u32, u32, u32, u32, u32
#define A8(b) w[b], w[(b) + 1], w[(b) + 2], w[(b) + 3], w[(b) + 4], w[(b) + 5], w[(b) + 6], w[(b) + 7]
    {
        int pass = n <= 24 ? 24 : n <= 56 ? 56 : STACK_WORDS;
        while (n < pass) w[n++] = 0;
    }
    if (retk == 'F' || retk == 'D') {
        typedef double (*fp24)(W8, W8, W8);
        typedef double (*fp56)(W8, W8, W8, W8, W8, W8, W8);
        typedef double (*fp120)(W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8);
        double d;
        if (n <= 24) d = MG_GFN(fp24, fn)(A8(0), A8(8), A8(16));
        else if (n <= 56) d = MG_GFN(fp56, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48));
        else d = MG_GFN(fp120, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48), A8(56), A8(64), A8(72), A8(80), A8(88), A8(96), A8(104), A8(112));
        /* (st(0) as a double: a float's value, exactly, which is then made a float again.) */
        if (retk == 'D') ret.d = d;
        else ret.f = (float)d;
        return ret;
    } else {
        typedef u64 (*fn24)(W8, W8, W8);
        typedef u64 (*fn56)(W8, W8, W8, W8, W8, W8, W8);
        typedef u64 (*fn120)(W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8, W8);
        if (n <= 24) r = MG_GFN(fn24, fn)(A8(0), A8(8), A8(16));
        else if (n <= 56) r = MG_GFN(fn56, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48));
        else r = MG_GFN(fn120, fn)(A8(0), A8(8), A8(16), A8(24), A8(32), A8(40), A8(48), A8(56), A8(64), A8(72), A8(80), A8(88), A8(96), A8(104), A8(112));
    }
#undef W8
#undef A8
    switch (retk) {
    case 'V': break;
    case 'J': memcpy(&ret, &r, 8); break;
    case 'L': ret.l = (mg_jobj *)(mg_addr)(u32)r; break;
    case 'Z': ret.i = (u8)r; break;
    case 'B': ret.i = (i8)r; break;
    case 'C': ret.i = (u16)r; break;
    case 'S': ret.i = (i16)r; break;
    default: ret.i = (i32)(u32)r; break;
    }
    return ret;
#endif
}
#endif


mg_jvalue mg_jni_call_native(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args)
{
    return call_native(fn, self_or_class, sig, args, false);
}

/*
 * The same under a guard: when the native code faults and the fault is not the layer's to repair, the thread comes back
 * here (mg_core.c: the stub longjmp to this setjmp) and *faulted says so; the call's value is then 0. The guard of a call
 * further out is kept and put back, so that a native method called back from a guarded one has its own. Where there is
 * no stub setjmp (a host that is not ARM32), a plain call.
 */
#if defined(__GNUC__) || defined(__clang__)
typedef int (*setjmp_stub_fn)(mg_addr *jb) __attribute__((returns_twice));
#else
typedef int (*setjmp_stub_fn)(mg_addr *jb);
#endif
#if !MG_ARCH_ARM32 && MG_GUEST_CODE
/* core/src/crt/mg_crt_setjmp.c: the core's own, of the guest's convention (docs/DESKTOP.md). */
int mg_setjmp(mg_addr *jb) __attribute__((returns_twice));
#endif

static MG_NOINLINE int guarded(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args, mg_jvalue *out)
{
    mg_gthread *t = mg_gthread_self();
    mg_addr jb[32];
    /* (A comparison, unless a library left as the file has it was loaded since this thread last called native code.) */
    mg_gthread_take_breakpoints(t);
    mg_addr *volatile outer = t->fault_guard;
#if !MG_ARCH_ARM32 && MG_GUEST_CODE
    /* (Called by its own name, which the compiler knows returns twice: through a pointer it would not.) */
    if (mg_setjmp(jb) != 0) {
        t->fault_guard = outer;
        return 1;
    }
#else
    void *setjmp_stub = MG_ARCH_ARM32 ? mg_stub(MG_STUB_SETJMP) : NULL;
    if (!setjmp_stub) {
        *out = call_native(fn, self_or_class, sig, args, false);
        return 0;
    }
    if (MG_GFN(setjmp_stub_fn, setjmp_stub)(jb) != 0) {
        t->fault_guard = outer;
        return 1;
    }
#endif
    t->fault_guard = jb;
    *out = call_native(fn, self_or_class, sig, args, false);
    t->fault_guard = outer;
    return 0;
}

mg_jvalue mg_jni_call_native_guarded(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args, bool *faulted)
{
    mg_jvalue ret;
    ret.j = 0;
    *faulted = guarded(fn, self_or_class, sig, args, &ret) != 0;
    if (*faulted) ret.j = 0;
    return ret;
}

static mg_jvalue call_native_bare(void *fn, const char *sig, const mg_jvalue *args) { return call_native(fn, NULL, sig, args, true); }

/* A native method of one of the runtime's classes: asked once for each, when it is first called. */
static void *native_for_dvm(const char *cls, const char *name, const char *sig, bool app_class)
{
    mg_jclass *c = mg_jni_class(cls, NULL);
    void *fn = find_native(cls, name, sig, app_class);
    jnative *n;
    if (!fn) return NULL;
    for (n = c->natives; n; n = n->next)
        if (n->fn == fn && !strcmp(n->name, name)) return fn;
    mg_atomic_add(&g_natives_by_name, 1);
    return fn;
}

void mg_jni_dvm_join(mg_dvm_jni *out)
{
    out->find = native_for_dvm;
    out->tried = native_tried;
    out->call = mg_jni_call_native;
    out->call_guarded = mg_jni_call_native_guarded;
    out->call_bare = call_native_bare;
    out->class_for = jni_class_for;
}

/* ------------------------------------------------------- the JNIEnv table */

typedef void *JNIEnvP;                 /* JNIEnv *: the guest's first argument, unused here */

static mg_jclass *class_of(mg_jobj *o)
{
    if (!o) return mg_jni_class("java/lang/Object", NULL);
    return o->kind == MG_J_CLASS ? o->u.klass.jc : (o->cls ? o->cls : mg_jni_class("java/lang/Object", NULL));
}

static i32 j_GetVersion(JNIEnvP e) { (void)e; return 0x00010006; }
static mg_jobj *j_DefineClass(JNIEnvP e, const char *name, mg_jobj *loader, const void *buf, i32 len)
{
    (void)e; (void)loader; (void)buf; (void)len;
    if (g_dvm.running) {
        /* Android's answer too: a class comes from a DEX file through a class loader, never from bytes handed to JNI. */
        mg_warn("jni: DefineClass(%s) is not supported, as on Android", name ? name : "null");
        return NULL;
    }
    return mg_jni_class_object(name);
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

static int asker(const mg_dvm_thread *t)
{
    if (t->onload_running && t->top == t->onload_frame) return t->onload_app ? ASKER_APP : ASKER_BOOT;
    if (!t->top) return ASKER_NOBODY;
    return mg_dvm_class_is_app(t->top->m->cls) ? ASKER_APP : ASKER_BOOT;
}

/* Whether a DEX file defines the class a JNI name stands for: the class itself, or what an array of that name is an array of. */
static bool name_is_defined(const char *name)
{
    char inner[300];
    size_t n;
    if (name[0] != '[') return mg_dvm_class_exists(name);
    while (*name == '[') name++;
    if (*name != 'L') return name[0] && !name[1];               /* an array of a primitive type */
    n = strlen(name);
    if (n < 3 || n - 2 >= sizeof(inner)) return false;
    memcpy(inner, name + 1, n - 2);
    inner[n - 2] = 0;
    return mg_dvm_class_exists(inner);
}

/* What ART leaves when a class is not found: the boot class path's own error for a caller that is one of the runtime's classes, a class loader's for every other. */
static void throw_not_found(const char *name, int who, bool hidden)
{
    char dotted[300];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof(dotted); i++) dotted[i] = name[i] == '/' ? '.' : name[i];
    dotted[i] = 0;
    if (who == ASKER_BOOT) {
        mg_dvm_throw("java/lang/NoClassDefFoundError", "%s", name);
    } else if (who == ASKER_NOBODY) {
        mg_dvm_throw("java/lang/ClassNotFoundException", "Didn't find class \"%s\" on path: DexPathList[[directory \".\"],nativeLibraryDirectories=[/system/lib]]%s", dotted,
                     hidden ? " (FindClass on a thread with no Java method under it asks the system class loader, which has none of the app's classes)" : "");
    } else {
        mg_dvm_throw("java/lang/ClassNotFoundException", "Didn't find class \"%s\"", dotted);
    }
}

static mg_jobj *j_FindClass(JNIEnvP e, const char *name)
{
    mg_jclass *c;
    const mg_dvm_class *k;
    int who = ASKER_APP;
    (void)e;
    if (!g_dvm.running) return mg_jni_class_object(name);
    mg_dvm_thread *thread = mg_dvm_self();
    int scope = thread->onload_running && thread->top == thread->onload_frame ? thread->onload_scope :
        thread->top ? thread->top->m->cls->scope : 0;
    if (scope) {
        mg_dvm_class *type;
        if (!mg_dvm_class_exists_scope(name, scope)) { throw_not_found(name, ASKER_APP, false); return NULL; }
        type = mg_dvm_find_class_scope(name, scope);
        return type ? mg_dvm_mirror(type) : NULL;
    }
    /* With the runtime up, a class a DEX file defines is found as Android finds it: linked, or not found and an exception. (It is initialised when a method or field of it is asked for.) */
    mg_dvm_clear_pending();
    if (g_dvm.app_loader) {
        /*
         * A Java app: a class is one a DEX file defines, or it is not there. (Elsewhere a name nothing defines is a class
         * of this layer's own, made by being asked for: the Java parts the other drivers play in C.)
         */
        who = asker(mg_dvm_self());
        if (!name_is_defined(name)) {
            throw_not_found(name, who, false);
            return NULL;
        }
    }
    c = mg_jni_class(name, NULL);
    if (!c->dvm || mg_dvm_pending()) {
        /* Found, and its linking failed: that exception is the answer. */
        if (!mg_dvm_pending()) mg_dvm_throw("java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    for (k = c->dvm; k->component; k = k->component) { }
    if (who != ASKER_APP && mg_dvm_class_is_app(k)) {
        throw_not_found(name, who, true);
        return NULL;
    }
    return mirror_of(c);
}

/* A method or field identifier of a Method, Constructor or Field object: libcore's real ones while the runtime runs. */
static jmeth *j_FromReflectedMethod(JNIEnvP e, mg_jobj *m)
{
    mg_dvm_method *dm = g_dvm.running ? mg_dvm_reflected_method(m) : NULL;
    (void)e;
    if (dm) return dm->cls->jc ? get_method(dm->cls->jc, dm->name, dm->sig, (dm->flags & DVM_ACC_STATIC) != 0) : NULL;
    return m && !is_real(m->cls) ? (jmeth *)m->native : NULL;
}
static jfield *j_FromReflectedField(JNIEnvP e, mg_jobj *f)
{
    mg_dvm_field *df = g_dvm.running ? mg_dvm_reflected_field(f) : NULL;
    (void)e;
    if (df) return df->cls->jc ? get_field(df->cls->jc, df->name, df->type, (df->flags & DVM_ACC_STATIC) != 0) : NULL;
    return f && !is_real(f->cls) ? (jfield *)f->native : NULL;
}
static mg_jobj *j_ToReflectedMethod(JNIEnvP e, mg_jobj *cls, jmeth *id, u8 is_static)
{
    mg_jobj *o;
    (void)e; (void)cls; (void)is_static;
    if (id && id->dvm) return mg_dvm_reflect_method(id->dvm);
    if (g_dvm.libcore) return NULL;                             /* nothing but a method of the runtime's can be a Method of libcore's */
    o = mg_jni_new_object("java/lang/reflect/Method");
    o->native = id;
    return o;
}
static mg_jobj *j_ToReflectedField(JNIEnvP e, mg_jobj *cls, jfield *id, u8 is_static)
{
    mg_jobj *o;
    (void)e; (void)cls; (void)is_static;
    if (id && id->dvm) return mg_dvm_reflect_field(id->dvm);
    if (g_dvm.libcore) return NULL;
    o = mg_jni_new_object("java/lang/reflect/Field");
    o->native = id;
    return o;
}
static mg_jobj *j_GetSuperclass(JNIEnvP e, mg_jobj *cls)
{
    mg_jclass *c = class_of(cls);
    (void)e;
    if (is_real(c)) return c->dvm->super && !(c->dvm->flags & DVM_ACC_INTERFACE) ? mg_dvm_mirror(c->dvm->super) : NULL;
    return c->super ? mirror_of(c->super) : NULL;
}
static bool is_subclass(const mg_jclass *c, const mg_jclass *of)
{
    for (; c; c = c->super)
        if (c == of) return true;
    return !strcmp(of->name, "java/lang/Object");
}
static u8 j_IsAssignableFrom(JNIEnvP e, mg_jobj *a, mg_jobj *b)
{
    mg_jclass *ca = class_of(a), *cb = class_of(b);
    (void)e;
    if (ca->dvm && cb->dvm) return mg_dvm_assignable(ca->dvm, cb->dvm);
    return is_subclass(ca, cb);
}
static i32 j_Throw(JNIEnvP e, mg_jobj *obj)
{
    (void)e;
    if (!g_dvm.running) mg_warn("jni: exception thrown from native code: %s", mg_jni_class_name(obj));
    set_pending(obj);
    return 0;
}
static i32 j_ThrowNew(JNIEnvP e, mg_jobj *cls, const char *msg)
{
    mg_jclass *c = class_of(cls);
    (void)e;
    /*
     * The class handed over, not its name: GmsCore's LevelDB throws its own LevelDbIoErrorException, of GmsCore's scope
     * (megler.ServiceClassLoader), and by name the app's scope had none, so the exception was a stand-in that is no Throwable
     * and logging it ended Play Store (IncompatibleClassChangeError for printStackTrace).
     */
    if (g_dvm.running && is_real(c)) {
        mg_warn("jni: %s: %s", c->name, msg ? msg : "");
        mg_dvm_throw_class(c->dvm, msg ? msg : "");
        return 0;
    }
    mg_jni_throw(c->name, msg);
    return 0;
}
static mg_jobj *j_ExceptionOccurred(JNIEnvP e) { (void)e; return pending(); }
/* Android prints the exception and its stack trace to the log and leaves it pending; so does this. */
static void j_ExceptionDescribe(JNIEnvP e)
{
    mg_jobj *p = pending();
    char text[400], stack[1600];
    (void)e;
    if (p && p->dvm && mg_dvm_describe_pending(text, sizeof(text))) {
        stack[0] = 0;
        mg_dvm_pending_stack(stack, sizeof(stack));
        mg_warn("jni: pending exception %s\n%s", text, stack);
    } else if (p) {
        mg_warn("jni: pending exception %s: %s", mg_jni_class_name(p), p->native ? (const char *)p->native : "");
    }
}
static void j_ExceptionClear(JNIEnvP e) { (void)e; set_pending(NULL); }
static u8 j_ExceptionCheck(JNIEnvP e) { (void)e; return pending() != NULL; }
static void j_FatalError(JNIEnvP e, const char *msg) { (void)e; mg_fatal("jni: FatalError: %s", msg ? msg : ""); }
/*
 * How many global and weak global references native code holds to an object, for GetObjectRefType and for nothing else
 * (nothing is collected, so a reference keeps nothing alive). An open table by the object's address. An object whose
 * counts are back at zero leaves a marker in its place, so that the chains through it hold, and the markers go when the
 * table is next made anew: its size follows what is referenced now, not everything that ever was.
 */
typedef struct ref_count {
    mg_jobj *o;
    i32 global, weak;
} ref_count;
#define REF_GONE ((mg_jobj *)(mg_addr)1)
static ref_count *g_refs;
static u32 g_refs_cap, g_refs_used, g_refs_live;   /* the capacity is a power of two; used counts the markers too */
static mg_lock g_refs_lock = MG_LOCK_INIT;

static u32 ref_slot(const mg_jobj *o) { return g_refs_cap ? (u32)(((mg_addr)o >> 3) * 2654435761u) & (g_refs_cap - 1) : 0; }

/* The object's entry, or NULL. With the lock held. */
static ref_count *ref_find(const mg_jobj *o)
{
    u32 i, k;
    for (k = 0, i = ref_slot(o); k < g_refs_cap && g_refs[i].o; k++, i = (i + 1) & (g_refs_cap - 1))
        if (g_refs[i].o == o) return &g_refs[i];
    return NULL;
}

static void ref_counted(mg_jobj *o, int global, int weak)
{
    ref_count *r;
    u32 i, k;
    if (!o) return;
    mg_lock_acquire(&g_refs_lock);
    r = ref_find(o);
    if (!r && (global > 0 || weak > 0)) {
        if ((g_refs_used + 1) * 2 > g_refs_cap) {
            /* Made anew, with room for four times what is live. */
            ref_count *old = g_refs;
            u32 old_cap = g_refs_cap, cap = 256;
            while (cap < g_refs_live * 4 + 4) cap *= 2;
            g_refs = (ref_count *)mg_alloc(sizeof(ref_count) * cap);
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
    mg_lock_release(&g_refs_lock);
}

/*
 * For the collector (dvm/mg_dvm_gc.c): the JNI layer's objects that are roots. These are the global and weak global
 * references, the Method objects of FromReflectedMethod, and the stand-in classes' static fields. Weak ones are kept as
 * strong for now: a weak reference here is the object's own address, which a freed object's successor could take.
 * mg_jni_gc_lock(true) is taken before the other threads are stopped, so that none of them is stopped halfway through the
 * table of references. The rest is read as it is: a list whose head is set last, values a word wide.
 */
void mg_jni_gc_lock(bool take)
{
    if (take) mg_lock_acquire(&g_refs_lock);
    else mg_lock_release(&g_refs_lock);
}

void mg_jni_gc_roots(void (*fn)(mg_jobj *o, void *user), void *user)
{
    const reflected *r;
    const jfield *f;
    u32 i;
    for (i = 0; i < g_refs_cap; i++)
        if (g_refs[i].o && g_refs[i].o != REF_GONE && (g_refs[i].global > 0 || g_refs[i].weak > 0)) fn(g_refs[i].o, user);
    for (r = g_reflected; r; r = r->next)
        if (r->object) fn(r->object, user);
    for (f = g_static_refs; f; f = f->next_ref)
        if (f->value.l) fn(f->value.l, user);
}

static mg_jobj *j_NewGlobalRef(JNIEnvP e, mg_jobj *o) { (void)e; ref_counted(o, 1, 0); return mg_jni_ref(o); }
static void j_DeleteGlobalRef(JNIEnvP e, mg_jobj *o) { (void)e; ref_counted(o, -1, 0); mg_jni_unref(o); }
static mg_jobj *j_NewWeakGlobalRef(JNIEnvP e, mg_jobj *o) { (void)e; ref_counted(o, 0, 1); return mg_jni_ref(o); }
static void j_DeleteWeakGlobalRef(JNIEnvP e, mg_jobj *o) { (void)e; ref_counted(o, 0, -1); mg_jni_unref(o); }

static i32 j_PushLocalFrame(JNIEnvP e, i32 cap) { (void)e; (void)cap; return 0; }
static mg_jobj *j_PopLocalFrame(JNIEnvP e, mg_jobj *result) { (void)e; return result; }
static mg_jobj *j_NewRef(JNIEnvP e, mg_jobj *o) { (void)e; return mg_jni_ref(o); }
static void j_DeleteRef(JNIEnvP e, mg_jobj *o) { (void)e; mg_jni_unref(o); }
static u8 j_IsSameObject(JNIEnvP e, mg_jobj *a, mg_jobj *b) { (void)e; return a == b; }
static i32 j_EnsureLocalCapacity(JNIEnvP e, i32 cap) { (void)e; (void)cap; return 0; }
/* AllocObject: an object of the class, its constructor not run. A String is an empty string of its own, as ART gives one
   (mirror::String::AllocEmptyString); a String constructor called on it then gives it its contents (string_init_on). */
static mg_jobj *j_AllocObject(JNIEnvP e, mg_jobj *cls)
{
    mg_jclass *c = class_of(cls);
    (void)e;
    if (is_real(c) && c->dvm == g_dvm.c_string) return mg_dvm_alloc_string(NULL, 0);
    return new_obj(MG_J_OBJECT, c);
}
static mg_jobj *j_GetObjectClass(JNIEnvP e, mg_jobj *o)
{
    (void)e;
    if (o && o->dvm) return mg_dvm_mirror(mg_dvm_class_of(o));
    return mirror_of(class_of(o));
}
static u8 j_IsInstanceOf(JNIEnvP e, mg_jobj *o, mg_jobj *cls)
{
    (void)e;
    if (!o) return 1;
    /* A proxy stands in for whatever interfaces it was made for, and those are not recorded: it is one of anything asked. */
    if (o->kind == MG_J_OBJECT && o->cls && !strcmp(o->cls->name, "java/lang/reflect/Proxy")) return 1;
    if (o->dvm && class_of(cls)->dvm) return mg_dvm_is_instance(o, class_of(cls)->dvm);
    return is_subclass(o->kind == MG_J_CLASS ? mg_jni_class("java/lang/Class", NULL) : o->cls, class_of(cls));
}
static jmeth *j_GetMethodID(JNIEnvP e, mg_jobj *cls, const char *name, const char *sig) { (void)e; return get_method(class_of(cls), name, sig, false); }
static jmeth *j_GetStaticMethodID(JNIEnvP e, mg_jobj *cls, const char *name, const char *sig) { (void)e; return get_method(class_of(cls), name, sig, true); }

/* A constructor: the new object, then <init> on it if anything implements it. */
static mg_jobj *construct(mg_jobj *cls, jmeth *m, const mg_jvalue *args)
{
    mg_jclass *c = class_of(cls);
    mg_jobj *o;
    if (!m) return NULL;
    if (is_real(c)) {
        /* A class of the runtime's: the object, then the constructor's bytecode. A string is made whole from the arguments. */
        if (!mg_dvm_ensure_init(c->dvm)) return NULL;
        if (c->dvm == g_dvm.c_string) return mg_dvm_string_init(m->sig, args, &o) ? o : NULL;
        o = mg_dvm_alloc_object(c->dvm);
        if (!o) return NULL;
        invoke_as(o, m, args, true);
        return mg_dvm_pending() ? NULL : o;
    }
    o = new_obj(MG_J_OBJECT, c);
    if (!o->dvm && !strcmp(o->cls->name, "java/lang/String")) {
        o->kind = MG_J_STRING;
        o->u.str.utf8 = mg_strdup("");
    }
    if (m->fn || find_hle(o->cls, m->name, m->sig)) invoke(o, m, args);
    else m->warned = true;             /* a constructor with nothing to do is the common case, not worth a line */
    return o;
}

static mg_jobj *j_NewObjectA(JNIEnvP e, mg_jobj *cls, jmeth *m, const mg_jvalue *args) { (void)e; return construct(cls, m, args); }
static mg_jobj *j_NewObjectV(JNIEnvP e, mg_jobj *cls, jmeth *m, va_list ap)
{
    mg_jvalue args[JARGS];
    (void)e;
    args_from_va(m, ap, args);
    return construct(cls, m, args);
}
static mg_jobj *j_NewObject(JNIEnvP e, mg_jobj *cls, jmeth *m, ...)
{
    mg_jvalue args[JARGS];
    va_list ap;
    (void)e;
    va_start(ap, m);
    args_from_va(m, ap, args);
    va_end(ap);
    return construct(cls, m, args);
}

/*
 * The Call<Type>Method families. Floating-point results go back as the guest's convention has them (gfloat, gdouble): as
 * bits in integer registers on 32-bit ARM, like every float that crosses to the guest there.
 */
#define RET_L(v) return (v).l
/* A field read hands the guest a reference of its own, which it may delete. */
#define RET_LREF(v) return mg_jni_ref((v).l)
#define RET_Z(v) return (v).z
#define RET_B(v) return (v).b
#define RET_C(v) return (v).c
#define RET_S(v) return (v).s
#define RET_I(v) return (v).i
#define RET_J(v) return (v).j
#define RET_F(v) return mg_gfb((v).f)
#define RET_D(v) return mg_gdb((v).d)
#define RET_V(v) (void)(v)

#define CALL_FAMILY(Name, ctype, RET)                                                                       \
    static ctype j_Call##Name##MethodA(JNIEnvP e, mg_jobj *o, jmeth *m, const mg_jvalue *args)              \
    {                                                                                                       \
        mg_jvalue r = invoke(o, m, args);                                                                   \
        (void)e;                                                                                            \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_Call##Name##MethodV(JNIEnvP e, mg_jobj *o, jmeth *m, va_list ap)                         \
    {                                                                                                       \
        mg_jvalue args[JARGS], r;                                                                              \
        (void)e;                                                                                            \
        args_from_va(m, ap, args);                                                                          \
        r = invoke(o, m, args);                                                                             \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_Call##Name##Method(JNIEnvP e, mg_jobj *o, jmeth *m, ...)                                 \
    {                                                                                                       \
        mg_jvalue args[JARGS], r;                                                                              \
        va_list ap;                                                                                         \
        (void)e;                                                                                            \
        va_start(ap, m);                                                                                    \
        args_from_va(m, ap, args);                                                                          \
        va_end(ap);                                                                                         \
        r = invoke(o, m, args);                                                                             \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallNonvirtual##Name##MethodA(JNIEnvP e, mg_jobj *o, mg_jobj *c, jmeth *m, const mg_jvalue *args) \
    {                                                                                                       \
        mg_jvalue r = invoke_as(o, m, args, true);                                                                   \
        (void)e; (void)c;                                                                                   \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallNonvirtual##Name##MethodV(JNIEnvP e, mg_jobj *o, mg_jobj *c, jmeth *m, va_list ap)   \
    {                                                                                                       \
        mg_jvalue args[JARGS], r;                                                                              \
        (void)e; (void)c;                                                                                   \
        args_from_va(m, ap, args);                                                                          \
        r = invoke_as(o, m, args, true);                                                                             \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallNonvirtual##Name##Method(JNIEnvP e, mg_jobj *o, mg_jobj *c, jmeth *m, ...)           \
    {                                                                                                       \
        mg_jvalue args[JARGS], r;                                                                              \
        va_list ap;                                                                                         \
        (void)e; (void)c;                                                                                   \
        va_start(ap, m);                                                                                    \
        args_from_va(m, ap, args);                                                                          \
        va_end(ap);                                                                                         \
        r = invoke_as(o, m, args, true);                                                                             \
        RET(r);                                                                                             \
    }

CALL_FAMILY(Object, mg_jobj *, RET_L)
CALL_FAMILY(Boolean, u8, RET_Z)
CALL_FAMILY(Byte, i8, RET_B)
CALL_FAMILY(Char, u16, RET_C)
CALL_FAMILY(Short, i16, RET_S)
CALL_FAMILY(Int, i32, RET_I)
CALL_FAMILY(Long, i64, RET_J)
CALL_FAMILY(Float, gfloat, RET_F)
CALL_FAMILY(Double, gdouble, RET_D)
CALL_FAMILY(Void, void, RET_V)

/* Static calls: the class stands where the object did. */
#define STATIC_FAMILY(Name, ctype, RET)                                                                     \
    static ctype j_CallStatic##Name##MethodA(JNIEnvP e, mg_jobj *c, jmeth *m, const mg_jvalue *args)        \
    {                                                                                                       \
        mg_jvalue r = invoke(NULL, m, args);                                                                \
        (void)e; (void)c;                                                                                   \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallStatic##Name##MethodV(JNIEnvP e, mg_jobj *c, jmeth *m, va_list ap)                   \
    {                                                                                                       \
        mg_jvalue args[JARGS], r;                                                                              \
        (void)e; (void)c;                                                                                   \
        args_from_va(m, ap, args);                                                                          \
        r = invoke(NULL, m, args);                                                                          \
        RET(r);                                                                                             \
    }                                                                                                       \
    static ctype j_CallStatic##Name##Method(JNIEnvP e, mg_jobj *c, jmeth *m, ...)                           \
    {                                                                                                       \
        mg_jvalue args[JARGS], r;                                                                              \
        va_list ap;                                                                                         \
        (void)e; (void)c;                                                                                   \
        va_start(ap, m);                                                                                    \
        args_from_va(m, ap, args);                                                                          \
        va_end(ap);                                                                                         \
        r = invoke(NULL, m, args);                                                                          \
        RET(r);                                                                                             \
    }

STATIC_FAMILY(Object, mg_jobj *, RET_L)
STATIC_FAMILY(Boolean, u8, RET_Z)
STATIC_FAMILY(Byte, i8, RET_B)
STATIC_FAMILY(Char, u16, RET_C)
STATIC_FAMILY(Short, i16, RET_S)
STATIC_FAMILY(Int, i32, RET_I)
STATIC_FAMILY(Long, i64, RET_J)
STATIC_FAMILY(Float, gfloat, RET_F)
STATIC_FAMILY(Double, gdouble, RET_D)
STATIC_FAMILY(Void, void, RET_V)

/* Fields. */
static jfield *j_GetFieldID(JNIEnvP e, mg_jobj *cls, const char *name, const char *sig) { (void)e; return get_field(class_of(cls), name, sig, false); }
static jfield *j_GetStaticFieldID(JNIEnvP e, mg_jobj *cls, const char *name, const char *sig) { (void)e; return get_field(class_of(cls), name, sig, true); }

#define FIELD_FAMILY(Name, ctype, member, GET, SETCONV)                                                        \
    static ctype j_Get##Name##Field(JNIEnvP e, mg_jobj *o, jfield *f)                                         \
    {                                                                                                         \
        mg_jvalue v;                                                                                          \
        (void)e;                                                                                              \
        v.j = 0;                                                                                              \
        if (o && f) v = *instance_slot(o, f);                                                                 \
        GET(v);                                                                                               \
    }                                                                                                         \
    static void j_Set##Name##Field(JNIEnvP e, mg_jobj *o, jfield *f, ctype value)                             \
    {                                                                                                         \
        (void)e;                                                                                              \
        if (o && f) instance_slot(o, f)->member = SETCONV(value);                                             \
    }                                                                                                         \
    static ctype j_GetStatic##Name##Field(JNIEnvP e, mg_jobj *c, jfield *f)                                   \
    {                                                                                                         \
        mg_jvalue v;                                                                                          \
        (void)e; (void)c;                                                                                     \
        v.j = 0;                                                                                              \
        if (f) v = *static_slot(f, true);                                                                     \
        GET(v);                                                                                               \
    }                                                                                                         \
    static void j_SetStatic##Name##Field(JNIEnvP e, mg_jobj *c, jfield *f, ctype value)                       \
    {                                                                                                         \
        (void)e; (void)c;                                                                                     \
        if (f) static_slot(f, false)->member = SETCONV(value);                                                \
    }

#define SAME(x) (x)
FIELD_FAMILY(Object, mg_jobj *, l, RET_LREF, mg_jni_ref)
FIELD_FAMILY(Boolean, u8, z, RET_Z, SAME)
FIELD_FAMILY(Byte, i8, b, RET_B, SAME)
FIELD_FAMILY(Char, u16, c, RET_C, SAME)
FIELD_FAMILY(Short, i16, s, RET_S, SAME)
FIELD_FAMILY(Int, i32, i, RET_I, SAME)
FIELD_FAMILY(Long, i64, j, RET_J, SAME)
FIELD_FAMILY(Float, gfloat, f, RET_F, mg_gf)
FIELD_FAMILY(Double, gdouble, d, RET_D, mg_gd)

/* Strings. */
static mg_jobj *j_NewString(JNIEnvP e, const u16 *chars, i32 len)
{
    char *utf8;
    mg_jobj *s;
    (void)e;
    if (g_dvm.running) return mg_dvm_new_string_utf16(chars, len);
    utf8 = from_utf16(chars, len);
    s = mg_jni_new_string(utf8);
    free(utf8);
    return s;
}
static i32 j_GetStringLength(JNIEnvP e, mg_jobj *s)
{
    (void)e;
    if (!s || s->kind != MG_J_STRING) return 0;
    need_utf16(s);
    return s->u.str.len16;
}
static const u16 *j_GetStringChars(JNIEnvP e, mg_jobj *s, u8 *is_copy)
{
    (void)e;
    if (is_copy) *is_copy = 0;
    if (!s || s->kind != MG_J_STRING) return NULL;
    need_utf16(s);
    return s->u.str.u16;
}
static void j_ReleaseStringChars(JNIEnvP e, mg_jobj *s, const u16 *chars) { (void)e; (void)s; (void)chars; }
static mg_jobj *j_NewStringUTF(JNIEnvP e, const char *utf8) { (void)e; return mg_jni_new_string(utf8); }
static i32 j_GetStringUTFLength(JNIEnvP e, mg_jobj *s) { (void)e; return mg_jni_string(s) ? (i32)strlen(mg_jni_string(s)) : 0; }
static const char *j_GetStringUTFChars(JNIEnvP e, mg_jobj *s, u8 *is_copy)
{
    (void)e;
    if (is_copy) *is_copy = 0;
    return mg_jni_string(s);
}
static void j_ReleaseStringUTFChars(JNIEnvP e, mg_jobj *s, const char *chars) { (void)e; (void)s; (void)chars; }
/* A region that is not inside the string: nothing is copied, and with the runtime up it is the exception Android throws for it. */
static bool string_region(mg_jobj *s, i32 start, i32 len)
{
    if (!s || s->kind != MG_J_STRING) return false;
    need_utf16(s);
    if (start >= 0 && len >= 0 && (i64)start + len <= s->u.str.len16) return true;
    if (g_dvm.running) mg_dvm_throw("java/lang/StringIndexOutOfBoundsException", "offset=%d length=%d string.length()=%d", (int)start, (int)len, (int)s->u.str.len16);
    return false;
}
static void j_GetStringRegion(JNIEnvP e, mg_jobj *s, i32 start, i32 len, u16 *buf)
{
    (void)e;
    if (!string_region(s, start, len)) return;
    memcpy(buf, s->u.str.u16 + start, (size_t)len * 2);
}
static void j_GetStringUTFRegion(JNIEnvP e, mg_jobj *s, i32 start, i32 len, char *buf)
{
    char *part;
    (void)e;
    if (!string_region(s, start, len)) return;
    part = from_utf16(s->u.str.u16 + start, len);
    strcpy(buf, part);
    free(part);
}

/* Arrays. */
static i32 j_GetArrayLength(JNIEnvP e, mg_jobj *a)
{
    (void)e;
    if (!a) return 0;
    return a->kind == MG_J_PRIM_ARRAY ? (i32)a->u.arr.len : a->kind == MG_J_OBJ_ARRAY ? (i32)a->u.oarr.len : 0;
}
static mg_jobj *j_NewObjectArray(JNIEnvP e, i32 len, mg_jobj *cls, mg_jobj *init)
{
    mg_jclass *ec = class_of(cls);
    char name[520];
    mg_jobj *a;
    i32 i;
    (void)e;
    /* An array of arrays is named by putting a bracket before the element's own name. */
    if (ec->name[0] == '[') snprintf(name, sizeof(name), "[%s", ec->name);
    else snprintf(name, sizeof(name), "[L%s;", ec->name);
    if (g_dvm.running) {
        if (!ec->dvm) bind_class(ec);
        /*
         * The array of the element's own class scope, as reflection's Array.newInstance makes it: it was looked for in the
         * app's, which has no class of another scope. Chromium's WebView, a scope of its own, makes an AppWebMessagePort[]
         * in native code for each message a page posts, and Zona (package 573) ended at the first, its ad's, with a
         * NoClassDefFoundError for that array thrown on the main thread.
         */
        if (ec->dvm && ec->dvm->scope) {
            mg_dvm_class *ac = mg_dvm_find_class_scope(name, ec->dvm->scope);
            a = ac ? mg_dvm_alloc_array(ac, len) : NULL;
        } else a = mg_dvm_new_array(name, len);
        for (i = 0; a && i < len; i++) a->u.oarr.v[i] = init && init->dvm ? init : NULL;
        return a;
    }
    a = new_obj(MG_J_OBJ_ARRAY, mg_jni_class(name, NULL));
    a->u.oarr.len = (u32)(len < 0 ? 0 : len);
    a->u.oarr.v = (mg_jobj **)mg_alloc(sizeof(mg_jobj *) * (a->u.oarr.len + 1));
    for (i = 0; i < len; i++) a->u.oarr.v[i] = mg_jni_ref(init);
    return a;
}
static mg_jobj *j_GetObjectArrayElement(JNIEnvP e, mg_jobj *a, i32 i)
{
    (void)e;
    if (!a || a->kind != MG_J_OBJ_ARRAY || i < 0 || (u32)i >= a->u.oarr.len) {
        if (a && a->dvm && a->kind == MG_J_OBJ_ARRAY) mg_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d", (int)a->u.oarr.len, (int)i);
        return NULL;
    }
    return mg_jni_ref(a->u.oarr.v[i]);
}
static void j_SetObjectArrayElement(JNIEnvP e, mg_jobj *a, i32 i, mg_jobj *v)
{
    (void)e;
    if (!a || a->kind != MG_J_OBJ_ARRAY || i < 0 || (u32)i >= a->u.oarr.len) {
        if (a && a->dvm) mg_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d", (int)mg_dvm_array_length(a), (int)i);
        return;
    }
    if (a->dvm && v && (!v->dvm || !mg_dvm_is_instance(v, mg_dvm_class_of(a)->component))) {
        mg_dvm_throw("java/lang/ArrayStoreException", "%s cannot be stored in an array of type %s", mg_jni_class_name(v), mg_jni_class_name(a));
        return;
    }
    mg_jni_ref(v);
    mg_jni_unref(a->u.oarr.v[i]);
    a->u.oarr.v[i] = v;
}

#define PRIM_ARRAY(Name, letter)                                                                            \
    static mg_jobj *j_New##Name##Array(JNIEnvP e, i32 len)                                                  \
    {                                                                                                       \
        (void)e;                                                                                            \
        return mg_jni_new_prim_array(letter, (u32)(len < 0 ? 0 : len));                                     \
    }

PRIM_ARRAY(Boolean, 'Z') PRIM_ARRAY(Byte, 'B') PRIM_ARRAY(Char, 'C') PRIM_ARRAY(Short, 'S')
PRIM_ARRAY(Int, 'I') PRIM_ARRAY(Long, 'J') PRIM_ARRAY(Float, 'F') PRIM_ARRAY(Double, 'D')

/* The elements are handed out in place, for every element type alike. */
static void *j_GetArrayElements(JNIEnvP e, mg_jobj *a, u8 *is_copy)
{
    (void)e;
    if (is_copy) *is_copy = 0;
    return a && a->kind == MG_J_PRIM_ARRAY ? a->u.arr.data : NULL;
}
static void j_ReleaseArrayElements(JNIEnvP e, mg_jobj *a, void *elems, i32 mode) { (void)e; (void)a; (void)elems; (void)mode; }
/* A region that is not inside the array: nothing is copied, and with the runtime up it is the exception Android throws for it. */
static bool array_region(mg_jobj *a, i32 start, i32 len)
{
    if (!a || a->kind != MG_J_PRIM_ARRAY) return false;
    if (start >= 0 && len >= 0 && (u64)(u32)start + (u32)len <= a->u.arr.len) return true;
    if (a->dvm) mg_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "offset=%d length=%d array.length=%d", (int)start, (int)len, (int)a->u.arr.len);
    return false;
}
static void j_GetArrayRegion(JNIEnvP e, mg_jobj *a, i32 start, i32 len, void *buf)
{
    (void)e;
    if (!array_region(a, start, len)) return;
    memcpy(buf, (u8 *)a->u.arr.data + (size_t)start * a->u.arr.esz, (size_t)len * a->u.arr.esz);
}
static void j_SetArrayRegion(JNIEnvP e, mg_jobj *a, i32 start, i32 len, const void *buf)
{
    (void)e;
    if (!array_region(a, start, len)) return;
    memcpy((u8 *)a->u.arr.data + (size_t)start * a->u.arr.esz, buf, (size_t)len * a->u.arr.esz);
}

/* JNINativeMethod: three of the guest's pointers. */
typedef struct g_native_method {
    const char *name, *sig;
    void *fn;
} g_native_method;

static i32 j_RegisterNatives(JNIEnvP e, mg_jobj *cls, const g_native_method *methods, i32 count)
{
    mg_jclass *c = class_of(cls);
    i32 i, unknown = 0;
    (void)e;
    for (i = 0; i < count; i++) {
        jnative *n = (jnative *)mg_alloc(sizeof(*n));
        const char *sig = methods[i].sig;
        /* An exclamation mark before the signature asked older Androids for a faster kind of call; it is not part of it. */
        if (sig[0] == '!') sig++;
        n->name = mg_strdup(methods[i].name);
        n->sig = mg_strdup(sig);
        n->fn = methods[i].fn;
        mg_lock_acquire(&g_jni_lock);
        n->next = c->natives;
        c->natives = n;
        mg_lock_release(&g_jni_lock);
        /* For a class the runtime has, the method must be one the class declares native. Android refuses the table; here it is said and the rest is taken. */
        if (is_real(c) && !mg_dvm_native_registered(c->dvm, n->name, n->sig, n->fn)) {
            unknown++;
            mg_atomic_add(&g_natives_unknown, 1);
            mg_warn("jni: %s has no native method %s%s to register", c->name, n->name, n->sig);
        } else if (is_real(c)) {
            mg_atomic_add(&g_natives_registered, 1);
        }
    }
    mg_dbg("jni: %s registered %d native methods%s", c->name, count, unknown ? ", some of them unknown to the class" : "");
    return 0;
}
static i32 j_UnregisterNatives(JNIEnvP e, mg_jobj *cls) { (void)e; (void)cls; return 0; }
static i32 j_MonitorEnter(JNIEnvP e, mg_jobj *o)
{
    (void)e;
    if (o && o->dvm) mg_dvm_monitor_enter(o);
    return 0;
}
static i32 j_MonitorExit(JNIEnvP e, mg_jobj *o)
{
    (void)e;
    if (o && o->dvm && !mg_dvm_monitor_exit(o)) {
        mg_dvm_throw("java/lang/IllegalMonitorStateException", "MonitorExit on an object this thread has not locked");
        return -1;
    }
    return 0;
}
static i32 j_GetJavaVM(JNIEnvP e, void **vm)
{
    (void)e;
    *vm = mg_jni_vm();
    return 0;
}

typedef struct direct_buffer {
    void *address;
    i64 capacity;
} direct_buffer;

static mg_jobj *j_NewDirectByteBuffer(JNIEnvP e, void *address, i64 capacity)
{
    mg_jobj *o;
    direct_buffer *b;
    (void)e;
    if (g_dvm.running && is_real(mg_jni_class("java/nio/DirectByteBuffer", NULL))) {
        /* libcore's own buffer, made by the constructor it keeps for this: DirectByteBuffer(long address, int capacity). */
        mg_jvalue args[2];
        args[0].j = (i64)(mg_addr)address;
        args[1].j = 0;
        args[1].i = (i32)capacity;
        return mg_dvm_new("java/nio/DirectByteBuffer", "(JI)V", args);
    }
    o = mg_jni_new_object("java/nio/DirectByteBuffer");
    b = (direct_buffer *)mg_alloc(sizeof(*b));
    b->address = address;
    b->capacity = capacity;
    o->native = b;
    return o;
}
mg_jobj *mg_jni_new_direct_buffer(void *address, i64 capacity) { return j_NewDirectByteBuffer(NULL, address, capacity); }

mg_jobj *mg_jni_new_object_array(const char *element_class, u32 len)
{
    return j_NewObjectArray(NULL, (i32)len, mg_jni_class_object(element_class), NULL);
}

/* A buffer of libcore's: its address and capacity are java.nio.Buffer's fields. */
static bool real_buffer(mg_jobj *o) { return o && o->dvm && is_real(o->cls) && mg_dvm_instance_of(o, "java/nio/Buffer"); }

static void *j_GetDirectBufferAddress(JNIEnvP e, mg_jobj *o)
{
    (void)e;
    if (real_buffer(o)) return (void *)(mg_addr)mg_jni_get_field(o, "address").j;
    return o && o->native && !strcmp(mg_jni_class_name(o), "java/nio/DirectByteBuffer") ? ((direct_buffer *)o->native)->address : NULL;
}
static i64 j_GetDirectBufferCapacity(JNIEnvP e, mg_jobj *o)
{
    (void)e;
    if (real_buffer(o)) return mg_jni_get_field(o, "address").j ? mg_jni_get_field(o, "capacity").i : -1;
    return o && o->native && !strcmp(mg_jni_class_name(o), "java/nio/DirectByteBuffer") ? ((direct_buffer *)o->native)->capacity : -1;
}
/* Whether `o` is one of the references the native method running on this thread was handed: its receiver or class, or a parameter. */
static bool handed_to_native(const mg_jobj *o)
{
    const mg_dvm_thread *t = g_dvm.running ? mg_dvm_self_existing() : NULL;
    const char *s;
    int i;
    if (!t || !t->native_shorty) return false;
    if (o == t->native_target) return true;
    if (!t->native_params) return false;
    for (s = t->native_shorty + 1, i = 0; *s; s++, i++)
        if (*s == 'L' && t->native_params[i].l == o) return true;
    return false;
}

/*
 * GetObjectRefType: 0 invalid, 1 local, 2 global, 3 weak global. A reference is the object's address here, so the kind is
 * not in the reference: what is kept is, for each object, how many global and how many weak global references native code
 * holds to it (ref_counted), and the answer is the strongest of those: global if there is one, weak if there is only that,
 * local otherwise -- but local for a reference the running native method was handed, as Android's are, whatever else
 * refers to the object. Chromium's JavaParamRef checks each parameter so (scoped_java_ref.cc, "Check failed: env &&
 * env->GetObjectRefType(obj) == JNILocalRefType"), and its native code holds its Java peer globally and is handed it as the
 * caller of every call: Ozon's Cronet ended there (0.1.0.671). It is exact unless a library asks about a local reference
 * it got back from a call of its own to an object it also holds globally.
 */
static i32 j_GetObjectRefType(JNIEnvP e, mg_jobj *o)
{
    const ref_count *r;
    i32 kind = 1;
    (void)e;
    if (!o) return 0;
    mg_lock_acquire(&g_refs_lock);
    r = ref_find(o);
    if (r) kind = r->global > 0 ? 2 : r->weak > 0 ? 3 : 1;
    mg_lock_release(&g_refs_lock);
    if (kind != 1 && handed_to_native(o)) kind = 1;
    return kind;
}

/* ------------------------------------------------------------- the tables */

static void *g_env_table[240];
static void **g_env = g_env_table;         /* a JNIEnv is a pointer to the table pointer */
static void *g_vm_table[8];
static void **g_vm = g_vm_table;

void *mg_jni_env(void) { return &g_env; }
void *mg_jni_vm(void) { return &g_vm; }

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
    const char *name;                  /* the guest's pointers */
    mg_jobj *group;
} g_attach_args;

static MG_THREAD_LOCAL bool t_attached;

static bool thread_is_javas(void) { return t_attached || !mg_gthread_self()->start; }
/* ART takes these three and no other (JavaVMExt::IsBadJniVersion). */
static bool bad_jni_version(i32 v) { return v != 0x00010002 && v != 0x00010004 && v != 0x00010006; }

static i32 attach(void **env, const g_attach_args *args, bool daemon)
{
    if (!env) return G_JNI_ERR;
    if (!thread_is_javas()) {
        if (args && bad_jni_version(args->version)) {
            mg_err("jni: bad JNI version passed to AttachCurrentThread: %d", (int)args->version);
            return G_JNI_EVERSION;
        }
        if (!mg_dvm_thread_attach(args ? args->name : NULL, args ? args->group : NULL, daemon)) {
            *env = NULL;
            return G_JNI_ERR;
        }
        t_attached = true;
    }
    *env = mg_jni_env();
    return G_JNI_OK;
}

static i32 vm_Destroy(void *vm) { (void)vm; return 0; }
static i32 vm_Attach(void *vm, void **env, void *args) { (void)vm; return attach(env, (const g_attach_args *)args, false); }
static i32 vm_AttachAsDaemon(void *vm, void **env, void *args) { (void)vm; return attach(env, (const g_attach_args *)args, true); }
static i32 vm_Detach(void *vm)
{
    (void)vm;
    if (!t_attached) return thread_is_javas() ? G_JNI_OK : G_JNI_ERR;   /* one of Java's own stays what it is */
    mg_dvm_thread_detach();
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
    /* JNI_VERSION_1_1 too, as ART takes it here and nowhere else (GetEnv): 2GIS's map library asks with it in JNI_OnLoad. */
    if (bad_jni_version(version) && version != 0x00010001) {
        mg_err("jni: bad JNI version passed to GetEnv: %d", (int)version);
        return G_JNI_EVERSION;
    }
    *env = mg_jni_env();
    return G_JNI_OK;
}

static i32 g_GetCreatedJavaVMs(void **vms, i32 cap, i32 *count)
{
    if (cap > 0 && vms) vms[0] = mg_jni_vm();
    if (count) *count = 1;
    return 0;
}

static const mg_sym g_tab_jni[] = {
    MG_WRAP("JNI_GetCreatedJavaVMs", g_GetCreatedJavaVMs),
    MG_END
};

#define SLOT(i, f) g_env_table[i] = (void *)(f)
#define SLOT3(i, base) SLOT(i, base), SLOT(i + 1, base##V), SLOT(i + 2, base##A)

void mg_jni_init(void)
{
    static bool done;
    static const char prims[] = "ZBCSIJFD";
    int i;
    if (done) return;
    done = true;
    mg_bionic_register(g_tab_jni);
    mg_jni_class("java/lang/Object", NULL);
    mg_jni_class("java/lang/Class", "java/lang/Object");
    mg_jni_class("java/lang/String", "java/lang/Object");
    mg_jni_class("java/lang/Throwable", "java/lang/Object");
    mg_jni_class("java/lang/Exception", "java/lang/Throwable");
    mg_jni_class("java/lang/RuntimeException", "java/lang/Exception");
    mg_jni_class("java/lang/Error", "java/lang/Throwable");

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
