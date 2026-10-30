/*
 * Classes: which DEX file defines which, linking one when it is first needed
 * (its superclass and interfaces first, its fields laid out after the
 * superclass's, its methods, its vtable), finding methods and fields by name,
 * and running a class's static initialiser once.
 *
 * Linking never throws while the lock on the tables is held: making an
 * exception runs bytecode (its constructor), and bytecode must not run under
 * that lock. What went wrong is carried out in a link_error and thrown once
 * the lock is released.
 */
#include "mg_dvm_internal.h"

#include <stdio.h>
#include <stdlib.h>

mg_dvm_globals g_dvm;

/* ------------------------------------------------------------------ the lock */

static mg_lock g_big = MG_LOCK_INIT;
static volatile u32 g_big_owner;
static int g_big_count;

void mg_dvm_lock(void)
{
    u32 me = mg_thread_self_id();
    if (g_big_owner == me) {
        g_big_count++;
        return;
    }
    mg_lock_acquire(&g_big);
    g_big_owner = me;
    g_big_count = 1;
}

void mg_dvm_unlock(void)
{
    if (--g_big_count == 0) {
        g_big_owner = 0;
        mg_lock_release(&g_big);
    }
}

static u32 hash_bytes(const char *s, size_t n)
{
    u32 h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (u8)s[i]) * 16777619u;
    return h;
}

/* ------------------------------------------------------------ interned names */

typedef struct name_entry {
    struct name_entry *next;
    char text[1];                      /* the string follows: allocated to its length */
} name_entry;

#define NAME_BUCKETS 65536
static name_entry *g_names[NAME_BUCKETS];

const char *mg_dvm_intern(const char *s)
{
    size_t n = strlen(s);
    u32 h = hash_bytes(s, n) % NAME_BUCKETS;
    name_entry *e;
    mg_dvm_lock();
    for (e = g_names[h]; e; e = e->next)
        if (!strcmp(e->text, s)) break;
    if (!e) {
        e = (name_entry *)mg_dvm_meta_alloc_kind(sizeof(*e) + n, DVM_META_NAME);
        memcpy(e->text, s, n + 1);
        e->next = g_names[h];
        g_names[h] = e;
    }
    mg_dvm_unlock();
    return e->text;
}

/* ------------------------------------------------- which file defines a class */

/*
 * Every class any DEX file defines, by name and service-loader scope.
 * The first definition wins within a scope. An
 * entry points at the class's descriptor in its file ("Lfoo/Bar;") and copies
 * nothing: an app defines tens of thousands of classes and uses a fraction.
 */
typedef struct cdef {
    const char *desc;
    mg_dvm_dex *dex;
    u32 def;
    u32 next;                          /* index + 1 of the next entry in the bucket */
} cdef;

/*
 * The buckets are a power of two, at least half as many as the definitions (cdef_grow). They were 16384 for every app, and
 * Yandex Maps defines 210000 classes and Google Play services, in it, 60000 more: a lookup went through fifteen of them
 * with strncmp, for every class looked for, as each of its DEX files was read and as each class was loaded.
 */
static cdef *g_cdefs;
static u32 g_ncdefs, g_cdefs_cap;
static u32 *g_cdef_buckets;
static u32 g_cdef_nbuckets;
static mg_dvm_dex *g_dexes[256];
static int g_ndexes;

/* A name of `n` bytes ("foo/Bar", not necessarily ended there) in a scope. */
static const cdef *cdef_find_n(const char *name, size_t n, int scope)
{
    u32 i;
    if (!g_cdef_nbuckets) return NULL;
    i = g_cdef_buckets[hash_bytes(name, n) & (g_cdef_nbuckets - 1)];
    while (i) {
        const cdef *c = &g_cdefs[i - 1];
        if (c->dex->scope == scope && !strncmp(c->desc + 1, name, n) && c->desc[n + 1] == ';' && c->desc[n + 2] == 0) return c;
        i = c->next;
    }
    return NULL;
}

static const cdef *cdef_find_scope(const char *name, int scope) { return cdef_find_n(name, strlen(name), scope); }

/* Twice the buckets, each definition put into its new one (with the runtime's lock held, as every lookup is). */
static void cdef_grow(void)
{
    u32 n = g_cdef_nbuckets ? g_cdef_nbuckets * 2 : 16384, i;
    u32 *b = (u32 *)mg_alloc(sizeof(u32) * n);
    for (i = 0; i < g_ncdefs; i++) {
        cdef *c = &g_cdefs[i];
        u32 h = hash_bytes(c->desc + 1, strlen(c->desc) - 2) & (n - 1);
        c->next = b[h];
        b[h] = i + 1;
    }
    free(g_cdef_buckets);
    g_cdef_buckets = b;
    g_cdef_nbuckets = n;
}

/*
 * Packages a boot file keeps to itself. Android 9's core-libart has ICU 60's android.icu.number, which Android did not
 * publish then and which its android.icu.text.DecimalFormat is made of; Android 11 published a later one, with other
 * classes and more methods, which apps of now call, and the framework has that one (core/framework-overlay/android/icu/
 * number). A class cannot be both. Core-libart's are registered under megler/libart/ names (megler/libart/android/icu/
 * number/Rounder), and what core-libart names in the package resolves to them: everyone else's android.icu.number is the
 * framework's, and DecimalFormat keeps the classes it was built with.
 */
static const struct { const char *file, *package; } k_own[] = { { "core-libart.dex", "android/icu/number/" } };
#define OWN_PREFIX "megler/libart/"

const char *mg_dvm_own_desc(const mg_dvm_dex *d, const char *desc, char *buf, size_t size)
{
    size_t dims = 0, n = d->own ? strlen(d->own) : 0;
    while (desc[dims] == '[') dims++;
    if (!n || desc[dims] != 'L' || strncmp(desc + dims + 1, d->own, n) != 0) return desc;
    if ((size_t)snprintf(buf, size, "%.*sL" OWN_PREFIX "%s", (int)dims, desc, desc + dims + 1) >= size) return desc;
    return buf;
}

void mg_dvm_classes_add_dex(mg_dvm_dex *d)
{
    u32 i, added = 0;
    size_t pooled = 0, at = 0;
    if (d->scope == 0 && !d->app)
        for (i = 0; i < MG_COUNT(k_own); i++)
            if (strstr(d->name, k_own[i].file)) d->own = k_own[i].package;
    /* A file viewed only for now (mg_dvm_dex_open_lazy): its classes' descriptors are copied, one block for all of them. */
    if (d->lazy) {
        for (i = 0; i < d->ncls; i++) pooled += strlen(mg_dvm_dex_type(d, mg_dvm_rd32(d->b + d->cls_off + 32 * i))) + 1;
        d->names_size = pooled ? pooled : 1;
        d->names = (char *)mg_dvm_meta_alloc_kind(d->names_size, DVM_META_POOL);
    }
    mg_dvm_lock();
    if (g_ndexes < (int)MG_COUNT(g_dexes)) g_dexes[g_ndexes++] = d;
    d->cdef_first = g_ncdefs;
    for (i = 0; i < d->ncls; i++) {
        const char *desc = mg_dvm_dex_type(d, mg_dvm_rd32(d->b + d->cls_off + 32 * i));
        size_t n = strlen(desc);
        if (d->names && at + n + 1 <= pooled) {
            memcpy(d->names + at, desc, n + 1);
            desc = d->names + at;
            at += n + 1;
        }
        const char *name;
        const cdef *existing;
        u32 h;
        if (d->own) {
            char own[512];
            const char *mine = mg_dvm_own_desc(d, desc, own, sizeof(own));
            if (mine != desc) {
                n = strlen(mine);
                desc = (const char *)memcpy(mg_dvm_meta_alloc_kind(n + 1, DVM_META_NAME), mine, n + 1);
            }
        }
        name = desc + 1;               /* the name is the descriptor's, between its L and its ; */
        if (n < 3 || desc[0] != 'L' || desc[n - 1] != ';') continue;
        existing = cdef_find_n(name, n - 2, d->scope);
        if (!existing) {
            if (g_ncdefs == g_cdefs_cap) {
                g_cdefs_cap = g_cdefs_cap ? g_cdefs_cap * 2 : 1024;
                g_cdefs = (cdef *)mg_realloc(g_cdefs, sizeof(cdef) * g_cdefs_cap);
            }
            if (g_ncdefs >= g_cdef_nbuckets * 2) cdef_grow();
            h = hash_bytes(name, n - 2) & (g_cdef_nbuckets - 1);
            g_cdefs[g_ncdefs].desc = desc;
            g_cdefs[g_ncdefs].dex = d;
            g_cdefs[g_ncdefs].def = i;
            g_cdefs[g_ncdefs].next = g_cdef_buckets[h];
            g_cdef_buckets[h] = ++g_ncdefs;
            added++;
        }
    }
    d->cdef_end = g_ncdefs;
    mg_dvm_unlock();
    mg_info("dvm: %s: %u classes (%u new), %u methods, %u strings", d->name, (unsigned)d->ncls, (unsigned)added, (unsigned)d->nmeth, (unsigned)d->nstr);
}

/*
 * A DEX file registered by its classes' names alone, viewed now (mg_dvm_dex_ready): the entries it added point at its own
 * descriptors again, and the copies made while it was not viewed are let go. Every file of Yandex Maps' and Google Play
 * services' is viewed within seconds of the start, and the copies stayed, some 17 MB of the two. Not the names the
 * runtime gave a package a file keeps to itself (mg_dvm_own_desc): those are not in the pool.
 */
void mg_dvm_classes_viewed(mg_dvm_dex *d)
{
    char *pool;
    size_t size;
    u32 i;
    mg_dvm_lock();
    pool = d->names;
    size = d->names_size;
    if (pool && d->b) {
        for (i = d->cdef_first; i < d->cdef_end && i < g_ncdefs; i++) {
            cdef *c = &g_cdefs[i];
            if (c->dex == d && c->desc >= pool && c->desc < pool + size) c->desc = mg_dvm_dex_type(d, mg_dvm_rd32(d->b + d->cls_off + 32 * c->def));
        }
        d->names = NULL;
        d->names_size = 0;
    } else {
        pool = NULL;
    }
    mg_dvm_unlock();
    if (pool) mg_dvm_meta_free_kind(pool, size, DVM_META_POOL);
}

u32 mg_dvm_defined_count(void) { return g_ncdefs; }
u32 mg_dvm_dex_count(void) { return (u32)g_ndexes; }

const char *mg_dvm_defined_name(u32 i, mg_dvm_dex **dex)
{
    char *name;
    const char *interned;
    size_t n;
    if (i >= g_ncdefs) return NULL;
    if (dex) *dex = g_cdefs[i].dex;
    n = strlen(g_cdefs[i].desc);
    name = (char *)mg_alloc(n);
    memcpy(name, g_cdefs[i].desc + 1, n - 2);
    interned = mg_dvm_intern(name);
    free(name);
    return interned;
}

/* ---------------------------------------------------------- registered natives */

static const mg_jhle *g_tables[64], *g_fallbacks[16];
static int g_ntables, g_nfallbacks;

void mg_dvm_register_fallback(const mg_jhle *table)
{
    mg_dvm_lock();
    if (g_nfallbacks < (int)MG_COUNT(g_fallbacks)) g_fallbacks[g_nfallbacks++] = table;
    mg_dvm_unlock();
}

void mg_dvm_register(const mg_jhle *table)
{
    mg_dvm_lock();
    if (g_ntables < (int)MG_COUNT(g_tables)) g_tables[g_ntables++] = table;
    else mg_err("dvm: too many tables of native methods");
    mg_dvm_unlock();
}

static mg_jhle_fn registered(const char *cls, const char *name, const char *sig)
{
    int t;
    for (t = g_ntables - 1; t >= 0; t--) {
        const mg_jhle *h;
        for (h = g_tables[t]; h->cls; h++)
            if ((!h->name || !strcmp(h->name, name)) && !strcmp(h->cls, cls) && (!h->sig || !strcmp(h->sig, sig))) return h->fn;
    }
    return NULL;
}

/* A type as Java source writes it, from its descriptor; returns where the descriptor goes on. */
static const char *pretty_type(const char *d, char *out, size_t n, size_t *k)
{
    static const char *const prims[] = { "Zboolean", "Bbyte", "Cchar", "Sshort", "Iint", "Jlong", "Ffloat", "Ddouble", "Vvoid" };
    int dims = 0, i;
    while (*d == '[') {
        dims++;
        d++;
    }
    if (*d == 'L') {
        for (d++; *d && *d != ';'; d++)
            if (*k + 1 < n) out[(*k)++] = *d == '/' ? '.' : *d;
        if (*d) d++;
    } else if (*d) {
        for (i = 0; i < (int)MG_COUNT(prims); i++)
            if (prims[i][0] == *d) *k += (size_t)snprintf(out + *k, *k < n ? n - *k : 0, "%s", prims[i] + 1);
        d++;
    }
    for (; dims; dims--) *k += (size_t)snprintf(out + *k, *k < n ? n - *k : 0, "[]");
    if (*k >= n) *k = n - 1;
    out[*k] = 0;
    return d;
}

/* "void com.example.Foo.bar(int, java.lang.String)": a method as ART's messages name it. */
static void pretty_method(const mg_dvm_method *m, char *out, size_t n)
{
    const char *p = m->sig, *ret = strchr(m->sig, ')');
    size_t k = 0;
    bool first = true;
    out[0] = 0;
    if (ret) pretty_type(ret + 1, out, n, &k);
    k += (size_t)snprintf(out + k, k < n ? n - k : 0, " ");
    for (p = m->cls->name; *p && k + 1 < n; p++) out[k++] = *p == '/' ? '.' : *p;
    k += (size_t)snprintf(out + k, k < n ? n - k : 0, ".%s(", m->name);
    if (k >= n) k = n - 1;
    for (p = m->sig + (m->sig[0] == '(' ? 1 : 0); *p && *p != ')';) {
        if (!first) k += (size_t)snprintf(out + k, k < n ? n - k : 0, ", ");
        if (k >= n) k = n - 1;
        first = false;
        p = pretty_type(p, out, n, &k);
    }
    snprintf(out + k, k < n ? n - k : 0, ")");
}

mg_dvm_native_info *mg_dvm_native_of(mg_dvm_method *m)
{
    mg_dvm_native_info *n = m->nat, *made, *was;
    if (n) return n;
    made = (mg_dvm_native_info *)mg_dvm_meta_alloc_kind(sizeof(*made), DVM_META_METHOD);
    memset(made, 0, sizeof(*made));
    was = (mg_dvm_native_info *)mg_atomic_cas_ptr((void *volatile *)&m->nat, NULL, made);
    /* (Two threads at once: the other's is the method's, and this one is left, a few bytes.) */
    return was ? was : made;
}

bool mg_dvm_bind_native(mg_dvm_method *m)
{
    mg_dvm_native_info *n = mg_dvm_native_of(m);
    mg_jhle_fn fn;
    void *jni = n->jni;
    if (n->native) return true;
    /* What was registered here in C comes before a library's function, whichever was there first. */
    mg_dvm_lock();
    fn = registered(m->cls->name, m->name, m->sig);
    mg_dvm_unlock();
    if (!fn && jni) {
        n->bound = true;
        return true;
    }
    if (!fn && g_dvm.jni.find) jni = g_dvm.jni.find(m->cls->name, m->name, m->sig, mg_dvm_class_is_app(m->cls));
    if (!fn && !jni) {
        /* Last, what stands in for a library that is not there. */
        const mg_jhle *h;
        int t;
        for (t = g_nfallbacks - 1; t >= 0 && !fn; t--)
            for (h = g_fallbacks[t]; h->cls && !fn; h++)
                if ((!h->name || !strcmp(h->name, m->name)) && !strcmp(h->cls, m->cls->name) && (!h->sig || !strcmp(h->sig, m->sig))) fn = h->fn;
    }
    if (!fn && !jni) {
        /* ART's own sentence, which is what an app's log and its crash reports show for this. */
        char pretty[600], tried[900];
        pretty_method(m, pretty, sizeof(pretty));
        tried[0] = 0;
        if (g_dvm.jni.tried) g_dvm.jni.tried(m->cls->name, m->name, m->sig, tried, sizeof(tried));
        return mg_dvm_throw("java/lang/UnsatisfiedLinkError", "No implementation found for %s%s%s%s", pretty, tried[0] ? " (tried " : "", tried, tried[0] ? ")" : "");
    }
    if (g_dvm.trace >= 1) mg_info("dvm: native %s.%s%s bound to %s", m->cls->name, m->name, m->sig, fn ? "a C implementation" : "the app's library");
    if (fn) n->native = fn;
    else n->jni = jni;
    n->bound = true;
    return true;
}

/* ------------------------------------------------------------- linked classes */

#define CLASS_BUCKETS 65536
static mg_dvm_class *g_classes[CLASS_BUCKETS];

/* What stopped a class from being linked: the exception to throw once the lock is released. */
typedef struct link_error {
    const char *cls;
    char msg[400];
} link_error;

static void link_fail(link_error *err, const char *cls, const char *fmt, ...) MG_PRINTF(3, 4);
static void link_fail(link_error *err, const char *cls, const char *fmt, ...)
{
    va_list ap;
    if (err->cls) return;              /* the first reason is the one worth reporting */
    err->cls = cls;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
}

static mg_dvm_class *loaded_scope(const char *name, int scope)
{
    mg_dvm_class *c;
    for (c = g_classes[hash_bytes(name, strlen(name)) % CLASS_BUCKETS]; c; c = c->next)
        if (c->scope == scope && !strcmp(c->name, name)) return c;
    return NULL;
}

static mg_dvm_class *loaded(const char *name) { return loaded_scope(name, 0); }
static const cdef *cdef_find(const char *name) { return cdef_find_scope(name, 0); }
static const cdef *definition(const char *name, int scope)
{
    const cdef *boot = cdef_find_scope(name, 0), *found;
    int parent;
    bool own_first;
    if (!scope || (boot && !boot->dex->app)) return boot;
    /* A child scope's (mg_dvm_art.c): its own files and its parent's scope, in its loader's order; -1, the boot path alone. */
    if (mg_dvm_scope_child(scope, &parent, &own_first)) {
        if (own_first && (found = cdef_find_scope(name, scope))) return found;
        if (parent >= 0 && (found = definition(name, parent))) return found;
        return own_first ? NULL : cdef_find_scope(name, scope);
    }
    return cdef_find_scope(name, scope);
}

static void publish(mg_dvm_class *c)
{
    u32 h = hash_bytes(c->name, strlen(c->name)) % CLASS_BUCKETS;
    if (!c->jc && g_dvm.jni.class_for) c->jc = g_dvm.jni.class_for(c->name, c->super ? c->super->name : NULL, c);
    c->next = g_classes[h];
    g_classes[h] = c;
    mg_atomic_add(&g_dvm.n_linked, 1);
    if (!strcmp(c->name, "java/lang/Object")) g_dvm.c_object = c;
    else if (!strcmp(c->name, "java/lang/Class")) g_dvm.c_class = c;
    else if (!strcmp(c->name, "java/lang/String")) g_dvm.c_string = c;
    else if (!strcmp(c->name, "java/lang/Cloneable")) g_dvm.c_cloneable = c;
    else if (!strcmp(c->name, "java/io/Serializable")) g_dvm.c_serializable = c;
    else if (!strcmp(c->name, "java/lang/Throwable")) {
        g_dvm.c_throwable = c;
        g_dvm.f_message = mg_dvm_find_field(c, "detailMessage", false);
        g_dvm.f_cause = mg_dvm_find_field(c, "cause", false);
        g_dvm.f_backtrace = mg_dvm_find_field(c, "backtrace", false);
    }
    if (g_dvm.trace >= 1) mg_info("dvm: linked %s (%u slots, %d in the vtable)", c->name, (unsigned)c->nslots, c->nvtab);
}

static mg_dvm_class *load_class_scope(const char *name, int scope, link_error *err);

static mg_dvm_class *load_desc_scope(const char *desc, int scope, link_error *err)
{
    static const struct { char letter; const char *name; } prims[] = { { 'I', "int" }, { 'J', "long" }, { 'F', "float" }, { 'D', "double" },
        { 'Z', "boolean" }, { 'B', "byte" }, { 'C', "char" }, { 'S', "short" }, { 'V', "void" } };
    size_t n = strlen(desc), i;
    mg_dvm_class *c;
    char *name;
    if (n == 1) {
        for (i = 0; i < MG_COUNT(prims); i++)
            if (prims[i].letter == desc[0]) return load_class_scope(prims[i].name, 0, err);
    }
    if (desc[0] == '[') return load_class_scope(desc, scope, err);
    if (n < 3 || desc[0] != 'L' || desc[n - 1] != ';') {
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", desc);
        return NULL;
    }
    name = (char *)mg_alloc(n);
    memcpy(name, desc + 1, n - 2);
    c = load_class_scope(name, scope, err);
    free(name);
    return c;
}

static mg_dvm_class *load_class(const char *name, link_error *err) { return load_class_scope(name, 0, err); }
static mg_dvm_class *load_class_desc(const char *desc, link_error *err) { return load_desc_scope(desc, 0, err); }

static bool same_package(const char *a, const char *b)
{
    const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
    size_t la = sa ? (size_t)(sa - a) : 0, lb = sb ? (size_t)(sb - b) : 0;
    return la == lb && !strncmp(a, b, la);
}

static mg_dvm_class *link_primitive(const char *name, char letter)
{
    mg_dvm_class *c = (mg_dvm_class *)mg_dvm_meta_alloc_kind(sizeof(*c), DVM_META_CLASS);
    c->name = mg_dvm_intern(name);
    c->prim = letter;
    c->flags = DVM_ACC_PUBLIC | DVM_ACC_FINAL | DVM_ACC_ABSTRACT;
    c->state = DVM_CS_INITIALIZED;
    publish(c);
    return c;
}

static mg_dvm_class *link_array(const char *name, int scope, link_error *err)
{
    mg_dvm_class *c, *component, *object, *cloneable, *serializable;
    if (!name[1] || (name[1] == 'L' && !strchr(name, ';'))) {
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    component = load_desc_scope(name + 1, scope, err);
    object = component ? load_class("java/lang/Object", err) : NULL;
    if (!object) return NULL;
    if (component->prim == 'V') {
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    c = (mg_dvm_class *)mg_dvm_meta_alloc_kind(sizeof(*c), DVM_META_CLASS);
    c->name = mg_dvm_intern(name);
    c->scope = component->scope;
    c->component = component;
    c->elem = component->prim ? component->prim : 'L';
    c->flags = (component->flags & DVM_ACC_PUBLIC) | DVM_ACC_FINAL | DVM_ACC_ABSTRACT;
    c->super = object;
    c->nslots = 0;                     /* an array's contents are its elements */
    c->vtab = object->vtab;            /* Object's own methods, none overridden */
    c->nvtab = object->nvtab;
    /* Every array is Cloneable and Serializable, where the boot class path has them. */
    cloneable = cdef_find("java/lang/Cloneable") ? load_class("java/lang/Cloneable", err) : NULL;
    serializable = cdef_find("java/io/Serializable") ? load_class("java/io/Serializable", err) : NULL;
    c->ifaces = (mg_dvm_class **)mg_dvm_meta_alloc_kind(sizeof(*c->ifaces) * 2, DVM_META_CLASS);
    if (cloneable) c->ifaces[c->nifaces++] = cloneable;
    if (serializable) c->ifaces[c->nifaces++] = serializable;
    c->state = DVM_CS_INITIALIZED;
    publish(c);
    return c;
}

static void free_unlinked(mg_dvm_class *c)
{
    free(c->ifaces);
    free(c->sfields);
    free(c->ifields);
    free(c->statics);
    free(c->direct);
    free(c->virt);
    free(c->vtab);
    free(c);
}

/*
 * The framework's methods that are signatures alone (mg_dvm_stubs_load), as the 64-bit hashes (FNV-1a) of the file's lines,
 * "<class> <name> <descriptor>", sorted: 160 KB where the lines themselves were 1.9 MB in every app's process. Read once,
 * before any of the framework's classes is linked; looked up as a method of an android/ or javax/ class is read. (Two
 * lines with one hash would mark a method more: nothing worse than one line too many in the log.)
 */
static u64 *g_stubs;
static size_t g_nstubs;

static u64 stub_hash(const char *s, size_t n)
{
    u64 h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (u8)s[i]) * 1099511628211ull;
    return h;
}

static int stub_cmp(const void *a, const void *b)
{
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

bool mg_dvm_stubs_load(const char *path)
{
    size_t size = 0, n = 0, cap = 0;
    const char *line, *end, *stop;
    u8 *text = mg_file_read_all(path, &size);
    if (!text) return false;
    stop = (const char *)text + size;
    for (line = (const char *)text; line < stop; line++) cap += *line == '\n';
    g_stubs = (u64 *)mg_alloc(sizeof(*g_stubs) * (cap + 1));
    for (line = (const char *)text; line < stop && n <= cap; line = end + 1) {
        size_t len;
        end = (const char *)memchr(line, '\n', (size_t)(stop - line));
        if (!end) end = stop;
        len = (size_t)(end - line);
        if (len && line[len - 1] == '\r') len--;
        if (len) g_stubs[n++] = stub_hash(line, len);
    }
    free(text);
    qsort(g_stubs, n, sizeof(*g_stubs), stub_cmp);
    g_nstubs = n;
    mg_info("api: %u of the framework's methods are their signatures alone here; the first call of each is said (\"api: ... is a stub here\")",
            (unsigned)n);
    return true;
}

bool mg_dvm_stub_listed(const char *cls, const char *name, const char *sig)
{
    char key[512];
    int n;
    u64 h;
    if (!g_nstubs || (strncmp(cls, "android/", 8) && strncmp(cls, "javax/", 6))) return false;
    n = snprintf(key, sizeof(key), "%s %s %s", cls, name, sig);
    if (n <= 0 || n >= (int)sizeof(key)) return false;
    h = stub_hash(key, (size_t)n);
    return bsearch(&h, g_stubs, g_nstubs, sizeof(*g_stubs), stub_cmp) != NULL;
}

static bool read_method(mg_dvm_class *c, mg_dvm_method *m, u32 idx, u32 flags, u32 code_off, link_error *err)
{
    mg_dvm_dex *d = c->dex;
    const u8 *mi;
    if (idx >= d->nmeth) {
        link_fail(err, "java/lang/ClassFormatError", "%s: method index %u of %u", c->name, (unsigned)idx, (unsigned)d->nmeth);
        return false;
    }
    mi = d->b + d->meth_off + 8 * idx;
    m->cls = c;
    m->idx = idx;
    m->flags = flags;
    m->vidx = -1;
    m->name = mg_dvm_intern(mg_dvm_dex_string(d, mg_dvm_rd32(mi + 4), NULL));
    m->sig = mg_dvm_dex_proto_sig(d, mg_dvm_rd16(mi + 2));
    m->shorty = mg_dvm_dex_proto_shorty(d, mg_dvm_rd16(mi + 2));
    m->nparams = (u16)(strlen(m->shorty) ? strlen(m->shorty) - 1 : 0);
    if (code_off && mg_dvm_stub_listed(c->name, m->name, m->sig)) m->stub = 1;
    if (code_off) {
        const u8 *ci = d->b + code_off;
        u32 words = 0;
        const char *s;
        if ((u64)code_off + 16 > d->size || (code_off & 3)) {
            link_fail(err, "java/lang/ClassFormatError", "%s.%s: its code lies outside the file", c->name, m->name);
            return false;
        }
        m->code = ci;
        m->regs = mg_dvm_rd16(ci);
        m->ins = mg_dvm_rd16(ci + 2);
        m->outs = mg_dvm_rd16(ci + 4);
        m->ntries = mg_dvm_rd16(ci + 6);
        m->ninsns = mg_dvm_rd32(ci + 12);
        m->insns = (const u16 *)(const void *)(ci + 16);
        for (s = m->shorty + (*m->shorty ? 1 : 0); *s; s++) words += (*s == 'J' || *s == 'D') ? 2 : 1;
        if (!(flags & DVM_ACC_STATIC)) words++;
        /* The interpreter copies a call's arguments into the last `ins` registers: they must be there, and be what the signature says. */
        if ((u64)code_off + 16 + (u64)m->ninsns * 2 > d->size || m->ins > m->regs || m->ins != words || m->ninsns == 0) {
            link_fail(err, "java/lang/ClassFormatError", "%s.%s%s: %u registers, %u for arguments (the signature needs %u), %u code units", c->name, m->name,
                      m->sig, (unsigned)m->regs, (unsigned)m->ins, (unsigned)words, (unsigned)m->ninsns);
            return false;
        }
    }
    return true;
}

static mg_dvm_class *link_dex_class(const cdef *cd, const char *name, link_error *err)
{
    mg_dvm_dex *d = cd->dex;
    const u8 *def, *end;
    mg_dvm_class *c;
    u32 super_idx, ifaces_off, data_off;
    int inherited, i, k;
    /* The file's bytes, viewed now if they are not yet (a file registered by its classes' names alone). */
    if (!mg_dvm_dex_ready(d)) {
        link_fail(err, "java/lang/NoClassDefFoundError", "%s: its DEX file %s cannot be read", name, d->name);
        return NULL;
    }
    def = d->b + d->cls_off + 32 * cd->def;
    end = d->b + d->size;
    c = (mg_dvm_class *)mg_dvm_meta_alloc_kind(sizeof(*c), DVM_META_CLASS);
    super_idx = mg_dvm_rd32(def + 8);
    ifaces_off = mg_dvm_rd32(def + 12);
    data_off = mg_dvm_rd32(def + 24);
    c->name = mg_dvm_intern(name);
    c->dex = d;
    c->scope = d->scope;
    c->def = cd->def;
    c->flags = mg_dvm_rd32(def + 4);

    if (super_idx != 0xFFFFFFFFu) {
        char own[512];
        c->super = load_desc_scope(mg_dvm_own_desc(d, mg_dvm_dex_type(d, super_idx), own, sizeof(own)), d->scope, err);
        if (!c->super) goto fail;
        if (c->super->flags & DVM_ACC_INTERFACE) {
            link_fail(err, "java/lang/IncompatibleClassChangeError", "%s has the interface %s for its superclass", name, c->super->name);
            goto fail;
        }
    } else if (strcmp(name, "java/lang/Object") != 0) {
        link_fail(err, "java/lang/ClassFormatError", "%s has no superclass", name);
        goto fail;
    }
    if (ifaces_off) {
        u32 n, j;
        if ((u64)ifaces_off + 4 > d->size || (u64)ifaces_off + 4 + (u64)mg_dvm_rd32(d->b + ifaces_off) * 2 > d->size) {
            link_fail(err, "java/lang/ClassFormatError", "%s: its interface list lies outside the file", name);
            goto fail;
        }
        n = mg_dvm_rd32(d->b + ifaces_off);
        c->ifaces = (mg_dvm_class **)mg_dvm_meta_alloc_kind(sizeof(*c->ifaces) * (n + 1), DVM_META_CLASS);
        for (j = 0; j < n; j++) {
            char own[512];
            mg_dvm_class *ic = load_desc_scope(mg_dvm_own_desc(d, mg_dvm_dex_type(d, mg_dvm_rd16(d->b + ifaces_off + 4 + 2 * j)), own, sizeof(own)),
                                               d->scope, err);
            if (!ic) goto fail;
            c->ifaces[c->nifaces++] = ic;
        }
    }
    c->nslots = c->super ? c->super->nslots : 0;

    if (data_off) {
        const u8 *p = d->b + data_off;
        u32 nsf, nif, ndm, nvm, idx, j;
        int group;
        if (data_off >= d->size) {
            link_fail(err, "java/lang/ClassFormatError", "%s: its class data lies outside the file", name);
            goto fail;
        }
        nsf = mg_dvm_uleb(&p);
        nif = mg_dvm_uleb(&p);
        ndm = mg_dvm_uleb(&p);
        nvm = mg_dvm_uleb(&p);
        if (nsf > d->nfield || nif > d->nfield || ndm > d->nmeth || nvm > d->nmeth) {
            link_fail(err, "java/lang/ClassFormatError", "%s: more members than the file has", name);
            goto fail;
        }
        c->sfields = (mg_dvm_field *)mg_dvm_meta_alloc_kind(sizeof(mg_dvm_field) * (nsf + 1), DVM_META_FIELD);
        c->ifields = (mg_dvm_field *)mg_dvm_meta_alloc_kind(sizeof(mg_dvm_field) * (nif + 1), DVM_META_FIELD);
        c->direct = (mg_dvm_method *)mg_dvm_meta_alloc_kind(sizeof(mg_dvm_method) * (ndm + 1), DVM_META_METHOD);
        c->virt = (mg_dvm_method *)mg_dvm_meta_alloc_kind(sizeof(mg_dvm_method) * (nvm + 1), DVM_META_METHOD);
        for (group = 0; group < 2; group++) {
            u32 n = group ? nif : nsf;
            idx = 0;
            for (j = 0; j < n; j++) {
                mg_dvm_field *f = group ? &c->ifields[c->nifields] : &c->sfields[c->nsfields];
                const u8 *fi;
                if (p >= end) goto truncated;
                idx += mg_dvm_uleb(&p);
                f->flags = mg_dvm_uleb(&p);
                if (idx >= d->nfield) goto truncated;
                fi = d->b + d->field_off + 8 * idx;
                f->cls = c;
                f->idx = idx;
                f->type = mg_dvm_intern(mg_dvm_dex_type(d, mg_dvm_rd16(fi + 2)));
                f->name = mg_dvm_intern(mg_dvm_dex_string(d, mg_dvm_rd32(fi + 4), NULL));
                if (group) {
                    /* A few fields of libcore's are the runtime's own and have no slot (mg_dvm_internal.h, DVM_SLOT_KLASS). */
                    if (!strcmp(name, "java/lang/Object") && !strcmp(f->name, "shadow$_klass_")) f->slot = DVM_SLOT_KLASS;
                    else if (!strcmp(name, "java/lang/Object") && !strcmp(f->name, "shadow$_monitor_")) f->slot = DVM_SLOT_MONITOR;
                    else if (!strcmp(name, "java/lang/String") && !strcmp(f->name, "count")) f->slot = DVM_SLOT_COUNT;
                    else f->slot = c->nslots++;
                    c->nifields++;
                } else {
                    f->slot = (u32)c->nsfields++;
                }
            }
        }
        for (group = 0; group < 2; group++) {
            u32 n = group ? nvm : ndm;
            idx = 0;
            for (j = 0; j < n; j++) {
                mg_dvm_method *m = group ? &c->virt[c->nvirt] : &c->direct[c->ndirect];
                u32 flags, code_off;
                if (p >= end) goto truncated;
                idx += mg_dvm_uleb(&p);
                flags = mg_dvm_uleb(&p);
                code_off = mg_dvm_uleb(&p);
                if (!read_method(c, m, idx, flags, code_off, err)) goto fail;
                if (group) c->nvirt++;
                else c->ndirect++;
            }
        }
    }
    if (c->nsfields) {
        /*
         * The static slots again, those of references first: the collector reads statics[0 .. nsrefs) of every linked
         * class and nothing else, where it read every static field's type (some 40,000 classes in Yandex Maps, a cache
         * miss a field, with the world stopped). init_statics fills them by field.
         */
        u32 next = 0;
        for (i = 0; i < c->nsfields; i++)
            if (c->sfields[i].type[0] == 'L' || c->sfields[i].type[0] == '[') c->sfields[i].slot = next++;
        c->nsrefs = (int)next;
        for (i = 0; i < c->nsfields; i++)
            if (c->sfields[i].type[0] != 'L' && c->sfields[i].type[0] != '[') c->sfields[i].slot = next++;
        c->statics = (mg_jvalue *)mg_dvm_meta_alloc_kind(sizeof(mg_jvalue) * (size_t)c->nsfields, DVM_META_FIELD);
    }

    /*
     * The vtable: the superclass's, a method of the same name and signature
     * taking its slot (a package-private one only within its package), new
     * methods appended.
     */
    inherited = c->super ? c->super->nvtab : 0;
    c->vtab = (mg_dvm_method **)mg_dvm_meta_alloc_kind(sizeof(*c->vtab) * (size_t)(inherited + c->nvirt + 1), DVM_META_VTABLE);
    if (inherited) memcpy(c->vtab, c->super->vtab, sizeof(*c->vtab) * (size_t)inherited);
    c->nvtab = inherited;
    for (i = 0; i < c->nvirt; i++) {
        mg_dvm_method *m = &c->virt[i];
        int slot = -1;
        for (k = 0; k < inherited; k++) {
            const mg_dvm_method *o = c->vtab[k];
            if (o->name != m->name || o->sig != m->sig) continue;
            if (!(o->flags & (DVM_ACC_PUBLIC | DVM_ACC_PROTECTED | DVM_ACC_PRIVATE)) && (o->cls->scope != c->scope || !same_package(o->cls->name, c->name))) continue;
            slot = k;
            break;
        }
        if (slot < 0) slot = c->nvtab++;
        c->vtab[slot] = m;
        m->vidx = slot;
    }
    c->state = DVM_CS_LINKED;
    publish(c);
    return c;

truncated:
    link_fail(err, "java/lang/ClassFormatError", "%s: its class data is damaged", name);
fail:
    free_unlinked(c);
    return NULL;
}

/* Names being linked, innermost last: a class that is its own ancestor would otherwise be linked for ever. */
static const char *g_linking[64];
static int g_linking_scope[64];
static int g_nlinking;

static mg_dvm_class *load_class_scope(const char *name, int scope, link_error *err)
{
    static const struct { const char *name; char letter; } prims[] = { { "int", 'I' }, { "long", 'J' }, { "float", 'F' }, { "double", 'D' },
        { "boolean", 'Z' }, { "byte", 'B' }, { "char", 'C' }, { "short", 'S' }, { "void", 'V' } };
    mg_dvm_class *c;
    const cdef *cd = definition(name, scope);
    if (cd) scope = cd->dex->scope;
    c = loaded_scope(name, scope);
    size_t i;
    int k;
    if (c) return c;
    if (name[0] == '[') {
        mg_dvm_class *component = load_desc_scope(name + 1, scope, err);
        if (!component) return NULL;
        c = loaded_scope(name, component->scope);
        return c ? c : link_array(name, component->scope, err);
    }
    if (!cd) {
        for (i = 0; i < MG_COUNT(prims); i++)
            if (!strcmp(prims[i].name, name)) return link_primitive(name, prims[i].letter);
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    for (k = 0; k < g_nlinking; k++) {
        if (g_linking_scope[k] == scope && !strcmp(g_linking[k], name)) {
            link_fail(err, "java/lang/ClassCircularityError", "%s", name);
            return NULL;
        }
    }
    if (g_nlinking >= (int)MG_COUNT(g_linking)) {
        link_fail(err, "java/lang/ClassFormatError", "%s: its superclasses nest too deep", name);
        return NULL;
    }
    g_linking_scope[g_nlinking] = scope;
    g_linking[g_nlinking++] = name;
    c = link_dex_class(cd, name, err);
    g_nlinking--;
    return c;
}

/* Throws what stopped the linking. A class that was missing further up says who needed it. */
static void throw_link_error(const link_error *err, const char *asked)
{
    if (!err->cls) mg_dvm_throw("java/lang/NoClassDefFoundError", "%s", asked);
    else if (!strcmp(err->cls, "java/lang/NoClassDefFoundError") && strcmp(err->msg, asked) != 0) mg_dvm_throw(err->cls, "%s (needed by %s)", err->msg, asked);
    else mg_dvm_throw(err->cls, "%s", err->msg);
}

mg_dvm_class *mg_dvm_find_class_scope(const char *name, int scope)
{
    link_error err;
    mg_dvm_class *c;
    err.cls = NULL;
    err.msg[0] = 0;
    mg_dvm_lock();
    c = load_class_scope(name, scope, &err);
    mg_dvm_unlock();
    if (!c) throw_link_error(&err, name);
    return c;
}

mg_dvm_class *mg_dvm_find_desc_scope(const char *descriptor, int scope)
{
    link_error err;
    mg_dvm_class *c;
    char name[512];
    err.cls = NULL;
    err.msg[0] = 0;
    mg_dvm_lock();
    c = load_desc_scope(descriptor, scope, &err);
    mg_dvm_unlock();
    if (!c) throw_link_error(&err, mg_dvm_descriptor_name(descriptor, name, sizeof(name)) ? name : descriptor);
    return c;
}

mg_dvm_class *mg_dvm_find_class(const char *name) { return mg_dvm_find_class_scope(name, 0); }
mg_dvm_class *mg_dvm_find_class_desc(const char *desc) { return mg_dvm_find_desc_scope(desc, 0); }
bool mg_dvm_class_exists_scope(const char *name, int scope) {
    bool yes;
    if (!scope) return mg_dvm_class_exists(name);
    mg_dvm_lock();
    yes = definition(name, scope) != NULL || loaded_scope(name, scope) != NULL;
    mg_dvm_unlock();
    return yes || name[0] == '[';
}

bool mg_dvm_class_exists(const char *name)
{
    static const char *const prims[] = { "int", "long", "float", "double", "boolean", "byte", "char", "short", "void" };
    size_t i;
    bool yes;
    if (name[0] == '[') return true;
    for (i = 0; i < MG_COUNT(prims); i++)
        if (!strcmp(prims[i], name)) return true;
    mg_dvm_lock();
    yes = loaded(name) != NULL || cdef_find(name) != NULL;
    mg_dvm_unlock();
    return yes;
}

/* ------------------------------------------------------------------- lookups */

/* A method an interface declares or inherits; a concrete (default) one when `concrete`. */
static mg_dvm_method *interface_method(mg_dvm_class *iface, const char *name, const char *sig, bool concrete)
{
    int i;
    for (i = 0; i < iface->nvirt; i++) {
        mg_dvm_method *m = &iface->virt[i];
        if (m->name == name && m->sig == sig && (!concrete || !(m->flags & DVM_ACC_ABSTRACT))) return m;
    }
    for (i = 0; i < iface->nifaces; i++) {
        mg_dvm_method *m = interface_method(iface->ifaces[i], name, sig, concrete);
        if (m) return m;
    }
    return NULL;
}

static mg_dvm_method *find_method_interned(mg_dvm_class *c, const char *name, const char *sig, bool is_static)
{
    mg_dvm_class *k;
    int i;
    for (k = c; k; k = k->super) {
        for (i = 0; i < k->ndirect; i++) {
            mg_dvm_method *m = &k->direct[i];
            if (m->name == name && m->sig == sig && ((m->flags & DVM_ACC_STATIC) != 0) == is_static) return m;
        }
        /* A constructor is its own class's, never an ancestor's: new Sub(1) must not find Super(int). */
        if (name[0] == '<') return NULL;
        if (!is_static)
            for (i = 0; i < k->nvirt; i++)
                if (k->virt[i].name == name && k->virt[i].sig == sig) return &k->virt[i];
    }
    if (is_static) return NULL;
    /* What the class inherits from interfaces without declaring it: an abstract class's unimplemented methods, default methods. */
    for (k = c; k; k = k->super)
        for (i = 0; i < k->nifaces; i++) {
            mg_dvm_method *m = interface_method(k->ifaces[i], name, sig, false);
            if (m) return m;
        }
    return NULL;
}

mg_dvm_method *mg_dvm_find_method(mg_dvm_class *c, const char *name, const char *sig, bool is_static)
{
    return find_method_interned(c, mg_dvm_intern(name), mg_dvm_intern(sig), is_static);
}

mg_dvm_method *mg_dvm_find_virtual(mg_dvm_class *receiver, const char *name, const char *sig)
{
    mg_dvm_method *abstract_hit = NULL;
    mg_dvm_class *k;
    int i;
    for (k = receiver; k; k = k->super)
        for (i = 0; i < k->nvirt; i++) {
            mg_dvm_method *m = &k->virt[i];
            if (m->name != name || m->sig != sig) continue;
            if (!(m->flags & DVM_ACC_ABSTRACT)) return m;
            if (!abstract_hit) abstract_hit = m;
        }
    for (k = receiver; k; k = k->super)
        for (i = 0; i < k->nifaces; i++) {
            mg_dvm_method *m = interface_method(k->ifaces[i], name, sig, true);
            if (m) return m;
        }
    if (abstract_hit) return abstract_hit;
    /* Declared by an interface and implemented by nothing: the abstract method itself, which whoever calls it reports as such. */
    for (k = receiver; k; k = k->super)
        for (i = 0; i < k->nifaces; i++) {
            mg_dvm_method *m = interface_method(k->ifaces[i], name, sig, false);
            if (m) return m;
        }
    return NULL;
}

/* A field by name and, when `type` is given, type: obfuscated code gives fields of different types the same name. */
static mg_dvm_field *find_field_interned(mg_dvm_class *c, const char *name, const char *type, bool is_static)
{
    mg_dvm_class *k;
    int i;
    for (k = c; k; k = k->super) {
        mg_dvm_field *list = is_static ? k->sfields : k->ifields;
        int n = is_static ? k->nsfields : k->nifields;
        for (i = 0; i < n; i++)
            if (list[i].name == name && (!type || list[i].type == type)) return &list[i];
        if (is_static)
            for (i = 0; i < k->nifaces; i++) {
                mg_dvm_field *f = find_field_interned(k->ifaces[i], name, type, true);
                if (f) return f;
            }
    }
    return NULL;
}

mg_dvm_field *mg_dvm_find_field(mg_dvm_class *c, const char *name, bool is_static)
{
    return find_field_interned(c, mg_dvm_intern(name), NULL, is_static);
}

mg_dvm_field *mg_dvm_find_field_typed(mg_dvm_class *c, const char *name, const char *type, bool is_static)
{
    return find_field_interned(c, mg_dvm_intern(name), mg_dvm_intern(type), is_static);
}

mg_dvm_method *mg_dvm_virtual_target(mg_dvm_method *m, mg_jobj *self)
{
    mg_dvm_class *rc = mg_dvm_class_of(self);
    mg_dvm_method *t;
    if (!(m->cls->flags & DVM_ACC_INTERFACE)) {
        if (m->vidx < 0) return m;                               /* private, a constructor: itself */
        if (m->vidx < rc->nvtab) return rc->vtab[m->vidx];
    }
    t = mg_dvm_find_virtual(rc, m->name, m->sig);
    return t ? t : m;
}

mg_dvm_class *mg_dvm_foreign_class(const char *name, mg_jclass *jc)
{
    mg_dvm_class *c, *object;
    link_error err;
    err.cls = NULL;
    err.msg[0] = 0;
    mg_dvm_lock();
    c = loaded(name);
    object = c ? NULL : load_class("java/lang/Object", &err);
    if (!c && object) {
        c = (mg_dvm_class *)mg_dvm_meta_alloc_kind(sizeof(*c), DVM_META_CLASS);
        c->name = mg_dvm_intern(name);
        c->flags = DVM_ACC_PUBLIC;
        c->super = object;
        c->vtab = object->vtab;
        c->nvtab = object->nvtab;
        c->state = DVM_CS_INITIALIZED;
        c->foreign = true;
        c->jc = jc;
        publish(c);
    }
    mg_dvm_unlock();
    return c;
}

bool mg_dvm_native_registered(mg_dvm_class *c, const char *name, const char *sig, void *fn)
{
    const char *iname = mg_dvm_intern(name), *isig = mg_dvm_intern(sig);
    int group, i;
    for (group = 0; group < 2; group++) {
        mg_dvm_method *list = group ? c->virt : c->direct;
        int n = group ? c->nvirt : c->ndirect;
        for (i = 0; i < n; i++) {
            if (list[i].name != iname || list[i].sig != isig || !(list[i].flags & DVM_ACC_NATIVE)) continue;
            mg_dvm_native_of(&list[i])->jni = fn;
            return true;
        }
    }
    return false;
}

static bool implements(mg_dvm_class *c, mg_dvm_class *iface)
{
    int i;
    if (c == iface) return true;
    for (i = 0; i < c->nifaces; i++)
        if (implements(c->ifaces[i], iface)) return true;
    return false;
}

bool mg_dvm_assignable(mg_dvm_class *sub, mg_dvm_class *sup)
{
    mg_dvm_class *k;
    int i;
    if (!sub || !sup) return false;
    if (sub == sup) return true;
    if (sub->prim || sup->prim) return false;
    if (sup == g_dvm.c_object) return true;
    if (sub->name[0] == '[') {
        if (sup == g_dvm.c_cloneable || sup == g_dvm.c_serializable) return true;
        if (sup->name[0] != '[') return false;
        /* Arrays of references follow their elements' classes; arrays of primitives are only themselves. */
        return sub->elem == 'L' && sup->elem == 'L' && mg_dvm_assignable(sub->component, sup->component);
    }
    if (sup->flags & DVM_ACC_INTERFACE) {
        for (k = sub; k; k = k->super)
            for (i = 0; i < k->nifaces; i++)
                if (implements(k->ifaces[i], sup)) return true;
        return false;
    }
    for (k = sub->super; k; k = k->super)
        if (k == sup) return true;
    return false;
}

/* ---------------------------------------------------------------- resolution */

mg_dvm_class *mg_dvm_resolve_type(mg_dvm_dex *d, u32 idx)
{
    mg_dvm_class *c;
    char own[512];
    if (idx >= d->ntype) {
        mg_dvm_throw("java/lang/VerifyError", "type index %u of %u in %s", (unsigned)idx, (unsigned)d->ntype, d->name);
        return NULL;
    }
    c = d->tcache[idx];
    if (c) return c;
    c = mg_dvm_find_desc_scope(mg_dvm_own_desc(d, mg_dvm_dex_type(d, idx), own, sizeof(own)), d->scope);
    if (c) d->tcache[idx] = c;
    return c;
}

/*
 * java.nio's covariant returns. Java 9 gave ByteBuffer, CharBuffer and the other buffers their own position(int),
 * limit(int), mark(), reset(), clear(), flip() and rewind(), returning the buffer's own type, and code compiled against
 * it names those: java/nio/ByteBuffer.position(I)Ljava/nio/ByteBuffer;. Android 9's libcore has only Buffer's, returning
 * Buffer (Android's own libcore has the overrides from a later release). Each of them returns the buffer it was called
 * on, so Buffer's is the same call: a reference to one of them in a buffer class resolves to Buffer's when the class has
 * no method of that exact signature. Code built for Android 10 and later calls them without asking the version (Brave
 * 1.97's Mojo bindings); NULL when this is not that case.
 */
static mg_dvm_method *nio_covariant(mg_dvm_class *c, const char *name, const char *sig)
{
    static const char *const names[] = { "position", "limit", "mark", "reset", "clear", "flip", "rewind" };
    const char *close = strchr(sig, ')');
    char want[96];
    size_t n, cn = strlen(c->name), i;
    if (strncmp(c->name, "java/nio/", 9) || !close || close[1] != 'L' || strncmp(close + 2, c->name, cn) || strcmp(close + 2 + cn, ";")) return NULL;
    for (i = 0; i < MG_COUNT(names); i++)
        if (!strcmp(name, names[i])) break;
    n = (size_t)(close + 1 - sig);
    if (i == MG_COUNT(names) || n + sizeof("Ljava/nio/Buffer;") > sizeof(want)) return NULL;
    memcpy(want, sig, n);
    memcpy(want + n, "Ljava/nio/Buffer;", sizeof("Ljava/nio/Buffer;"));
    return find_method_interned(c, name, mg_dvm_intern(want), false);
}

/*
 * Methods a boot class lacks, from the framework. Core-libart is a file of DEX code, and the libcore overlay
 * (tools/build-libcore-overlay.sh), which adds methods to classes, is compiled from sources; a method Android added to one
 * of core-libart's classes later (ICU's MeasureUnit.getIdentifier(), Android 12's) is a static method of
 * megler.members.<the class's name> in the framework (core/framework-overlay/megler/members) instead: of the same name and
 * signature for a static method, and taking the object first for one called on an object. The class and then its
 * superclasses are asked, as for a method they inherit. A static method is that method itself; one called on an object is
 * a copy of it that is not static and not virtual: the registers of a call are the same (the object, then the
 * arguments), and a method that is not virtual is called as it is (do_invoke). NULL when there is none.
 */
static mg_dvm_method *boot_member(mg_dvm_class *c, const char *name, const char *sig, bool want_static)
{
    static struct { const mg_dvm_method *of; mg_dvm_method *copy; } made[64];
    static int nmade;
    mg_dvm_class *k;
    for (k = c; k; k = k->super) {
        char members[300], want[600];
        mg_dvm_class *e;
        mg_dvm_method *m, *g;
        int i;
        if (!k->dex || !k->dex->boot || k->dex->app || k->dex->scope != 0) continue;
        if ((size_t)snprintf(members, sizeof(members), "megler/members/%s", k->name) >= sizeof(members) || !mg_dvm_class_exists(members)) continue;
        e = mg_dvm_find_class(members);
        if (!e) return NULL;
        if (want_static) m = find_method_interned(e, name, sig, true);
        else if ((size_t)snprintf(want, sizeof(want), "(L%s;%s", k->name, sig + 1) >= sizeof(want)) continue;
        else m = find_method_interned(e, name, mg_dvm_intern(want), true);
        if (!m) continue;
        if (e->state != DVM_CS_INITIALIZED && !mg_dvm_ensure_init(e)) return NULL;
        if (want_static) return m;
        mg_dvm_lock();
        for (i = 0; i < nmade && made[i].of != m; i++) {}
        g = i < nmade ? made[i].copy : NULL;
        if (!g) {
            size_t n = strlen(m->shorty);
            char *shorty = (char *)mg_dvm_meta_alloc_kind(n, DVM_META_METHOD);
            g = (mg_dvm_method *)mg_dvm_meta_alloc_kind(sizeof(*g), DVM_META_METHOD);
            *g = *m;
            /* As the object's method is described: its signature, and its parameters but the object. */
            shorty[0] = m->shorty[0];
            memcpy(shorty + 1, m->shorty + 2, n - 1);
            g->sig = sig;
            g->shorty = shorty;
            g->nparams = (u16)(m->nparams - 1);
            g->flags &= ~(u32)DVM_ACC_STATIC;
            g->vidx = -1;
            if (nmade < (int)MG_COUNT(made)) {
                made[nmade].of = m;
                made[nmade++].copy = g;
            }
        }
        mg_dvm_unlock();
        return g;
    }
    return NULL;
}

mg_dvm_method *mg_dvm_resolve_method(mg_dvm_dex *d, u32 idx, int kind)
{
    bool want_static = kind == DVM_INVOKE_STATIC;
    mg_dvm_method *m;
    mg_dvm_class *c;
    const char *name, *sig;
    const u8 *mi;
    if (idx >= d->nmeth) {
        mg_dvm_throw("java/lang/VerifyError", "method index %u of %u in %s", (unsigned)idx, (unsigned)d->nmeth, d->name);
        return NULL;
    }
    m = d->mcache[idx];
    if (!m) {
        mi = d->b + d->meth_off + 8 * idx;
        c = mg_dvm_resolve_type(d, mg_dvm_rd16(mi));
        if (!c) return NULL;
        name = mg_dvm_intern(mg_dvm_dex_string(d, mg_dvm_rd32(mi + 4), NULL));
        sig = mg_dvm_dex_proto_sig(d, mg_dvm_rd16(mi + 2));
        m = find_method_interned(c, name, sig, want_static);
        if (!m && !want_static) m = nio_covariant(c, name, sig);
        if (!m && !find_method_interned(c, name, sig, !want_static)) {
            m = boot_member(c, name, sig, want_static);
            if (!m && mg_dvm_pending()) return NULL;
        }
        if (!m) {
            if (find_method_interned(c, name, sig, !want_static)) {
                mg_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s%s is %s", c->name, name, sig, want_static ? "not static" : "static");
            } else {
                mg_dvm_throw("java/lang/NoSuchMethodError", "%s.%s%s", c->name, name, sig);
            }
            return NULL;
        }
        d->mcache[idx] = m;
    }
    if (((m->flags & DVM_ACC_STATIC) != 0) != want_static) {
        mg_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s%s is %s", m->cls->name, m->name, m->sig, want_static ? "not static" : "static");
        return NULL;
    }
    return m;
}

mg_dvm_field *mg_dvm_resolve_field(mg_dvm_dex *d, u32 idx, bool is_static)
{
    mg_dvm_field *f;
    mg_dvm_class *c;
    const char *name, *type;
    const u8 *fi;
    if (idx >= d->nfield) {
        mg_dvm_throw("java/lang/VerifyError", "field index %u of %u in %s", (unsigned)idx, (unsigned)d->nfield, d->name);
        return NULL;
    }
    f = d->fcache[idx];
    if (!f) {
        fi = d->b + d->field_off + 8 * idx;
        c = mg_dvm_resolve_type(d, mg_dvm_rd16(fi));
        if (!c) return NULL;
        type = mg_dvm_intern(mg_dvm_dex_type(d, mg_dvm_rd16(fi + 2)));
        name = mg_dvm_intern(mg_dvm_dex_string(d, mg_dvm_rd32(fi + 4), NULL));
        f = find_field_interned(c, name, type, is_static);
        if (!f) {
            if (find_field_interned(c, name, type, !is_static)) {
                mg_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s is %s", c->name, name, is_static ? "not static" : "static");
            } else {
                mg_dvm_throw("java/lang/NoSuchFieldError", "%s.%s:%s", c->name, name, type);
            }
            return NULL;
        }
        d->fcache[idx] = f;
    }
    if (((f->flags & DVM_ACC_STATIC) != 0) != is_static) {
        mg_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s is %s", f->cls->name, f->name, is_static ? "not static" : "static");
        return NULL;
    }
    return f;
}

/* ------------------------------------------------------------ initialisation */

/*
 * One value of a class's static_values array into a field. The format packs
 * each value into as few bytes as hold it: integers sign-extended, chars
 * zero-extended, floats and doubles with their low zero bytes dropped.
 * False when making the value threw.
 */
static bool read_static_value(mg_dvm_dex *d, const u8 **pp, const u8 *end, mg_jvalue *out)
{
    const u8 *p = *pp;
    u8 head;
    int type, arg, size, i;
    u64 v = 0;
    bool ok = true;
    if (p >= end) return true;
    head = *p++;
    type = head & 0x1f;
    arg = head >> 5;
    size = arg + 1;
    if (type != 0x1c && type != 0x1d && type != 0x1e && type != 0x1f && p + size > end) return true;
    switch (type) {
    case 0x00: case 0x02: case 0x04: case 0x06:                  /* byte, short, int, long */
        for (i = 0; i < size; i++) v |= (u64)p[i] << (8 * i);
        if (size < 8 && (p[size - 1] & 0x80)) v |= ~(u64)0 << (8 * size);
        if (type == 0x06) out->j = (i64)v;
        else out->i = (i32)(u32)v;
        p += size;
        break;
    case 0x03:                                                  /* char */
        for (i = 0; i < size; i++) v |= (u64)p[i] << (8 * i);
        out->i = (i32)(u16)v;
        p += size;
        break;
    case 0x10:                                                  /* float: its high bytes */
        for (i = 0; i < size; i++) v |= (u64)p[i] << (8 * (4 - size + i));
        out->i = (i32)(u32)v;
        p += size;
        break;
    case 0x11:                                                  /* double */
        for (i = 0; i < size; i++) v |= (u64)p[i] << (8 * (8 - size + i));
        out->j = (i64)v;
        p += size;
        break;
    case 0x17:                                                  /* a string */
        for (i = 0; i < size; i++) v |= (u64)p[i] << (8 * i);
        out->l = mg_dvm_const_string(d, (u32)v);
        ok = out->l != NULL;
        p += size;
        break;
    case 0x18: {                                                /* a class */
        mg_dvm_class *c;
        for (i = 0; i < size; i++) v |= (u64)p[i] << (8 * i);
        c = mg_dvm_resolve_type(d, (u32)v);
        out->l = c ? mg_dvm_mirror(c) : NULL;
        ok = c != NULL;
        p += size;
        break;
    }
    case 0x1c: {                                                /* an array: not a static field's value in practice; skipped */
        u32 n = mg_dvm_uleb(&p), k;
        mg_jvalue ignored;
        for (k = 0; k < n && p < end; k++) read_static_value(d, &p, end, &ignored);
        break;
    }
    case 0x1d: {                                                /* an annotation: skipped */
        u32 n, k;
        mg_jvalue ignored;
        mg_dvm_uleb(&p);
        n = mg_dvm_uleb(&p);
        for (k = 0; k < n && p < end; k++) {
            mg_dvm_uleb(&p);
            read_static_value(d, &p, end, &ignored);
        }
        break;
    }
    case 0x1e: out->l = NULL; break;
    case 0x1f: out->i = arg; break;                             /* boolean: the value is in the header */
    default: p += size; break;                                  /* method types and handles, fields, methods, enums: left zero */
    }
    *pp = p;
    return ok;
}

static bool init_statics(mg_dvm_class *c)
{
    mg_dvm_dex *d = c->dex;
    const u8 *p, *end;
    u32 off, n, i;
    if (!d || !c->nsfields) return true;
    off = mg_dvm_rd32(d->b + d->cls_off + 32 * c->def + 28);
    if (!off || off >= d->size) return true;
    p = d->b + off;
    end = d->b + d->size;
    n = mg_dvm_uleb(&p);
    for (i = 0; i < n && (int)i < c->nsfields; i++)
        if (!read_static_value(d, &p, end, &c->statics[c->sfields[i].slot])) return false;
    return true;
}

static mg_lock g_init_lock = MG_LOCK_INIT;
static mg_cond g_init_cond = MG_COND_INIT;

/*
 * A class whose initialisation failed cannot be used again, and every later use is NoClassDefFoundError "Could not
 * initialize class X". The first failure is what says why, and an app often catches it and goes on -- kotlinx.coroutines
 * does, for the service it looks up -- so that the error that ends the app names only the class. As Java 17 and ART do,
 * the later errors have the first one for their cause; and the first is said in the log when it happens, with where, once
 * for each class (failed_first).
 */
static bool failed_before(mg_dvm_class *c)
{
    mg_jobj *e;
    mg_dvm_throw("java/lang/NoClassDefFoundError", "Could not initialize class %s", c->name);
    e = mg_dvm_pending();
    if (e && c->init_error && c->init_error != e) {
        mg_jvalue arg;
        mg_dvm_clear_pending();
        arg.j = 0;
        arg.l = c->init_error;
        /* (Throwable.initCause; should it refuse, the error goes without a cause.) */
        if (!mg_dvm_call_virtual(e, "initCause", "(Ljava/lang/Throwable;)Ljava/lang/Throwable;", &arg, NULL)) mg_dvm_clear_pending();
        mg_dvm_throw_object(e);
    }
    return false;
}

static void failed_first(mg_dvm_class *c)
{
    char what[700], where[1600];
    mg_jobj *e = mg_dvm_pending(), *shown = e;
    mg_jvalue inner;
    c->init_error = e;
    if (!e) {
        mg_warn("dvm: the initialisation of %s failed, and the class cannot be used from now on", c->name);
        return;
    }
    /*
     * libcore's ExceptionInInitializerError keeps what the initialiser threw in a field of its own, not as its cause (its
     * getCause answers it): that is what says why, and where it was thrown. Said in its place, and the error left pending.
     */
    inner.j = 0;
    if (mg_dvm_get_field(e, "exception", &inner) && inner.l && inner.l != e) {
        shown = inner.l;
        mg_dvm_clear_pending();
        mg_dvm_throw_object(shown);
    }
    mg_dvm_describe_pending(what, sizeof(what));
    where[0] = 0;
    mg_dvm_pending_stack(where, sizeof(where));
    if (shown != e) {
        mg_dvm_clear_pending();
        mg_dvm_throw_object(e);
    }
    mg_warn("dvm: the initialisation of %s failed, and the class cannot be used from now on: %s%s\n%s", c->name,
            shown != e ? "java.lang.ExceptionInInitializerError, of " : "", what, where);
}

bool mg_dvm_ensure_init(mg_dvm_class *c)
{
    u32 me;
    bool ok;
    int i;
    if (c->state == DVM_CS_INITIALIZED) return true;
    me = mg_thread_self_id();
    mg_lock_acquire(&g_init_lock);
    for (;;) {
        if (c->state == DVM_CS_INITIALIZED) {
            mg_lock_release(&g_init_lock);
            return true;
        }
        if (c->state == DVM_CS_FAILED) {
            mg_lock_release(&g_init_lock);
            return failed_before(c);
        }
        if (c->state == DVM_CS_LINKED) break;
        /* Being initialised. By this thread: its initialiser has reached back to the class, which it may use half-made, as Java says. */
        if (c->init_thread == me) {
            mg_lock_release(&g_init_lock);
            return true;
        }
        mg_cond_wait(&g_init_cond, &g_init_lock);
    }
    c->state = DVM_CS_INITIALIZING;
    c->init_thread = me;
    mg_lock_release(&g_init_lock);

    ok = (!c->super || mg_dvm_ensure_init(c->super)) && init_statics(c);
    if (ok && g_dvm.before_init) g_dvm.before_init(c);
    for (i = 0; ok && i < c->ndirect; i++) {
        mg_dvm_method *m = &c->direct[i];
        mg_jvalue ignored;
        if (strcmp(m->name, "<clinit>") != 0) continue;
        if (g_dvm.trace >= 1) mg_info("dvm: <clinit> %s", c->name);
        ok = mg_dvm_invoke(m, NULL, NULL, &ignored);
        if (!ok) {
            /* An exception that is not an Error is wrapped, as Java wraps it: the class is unusable from now on and this says why. */
            mg_jobj *e = mg_dvm_pending();
            mg_dvm_class *error = mg_dvm_class_exists("java/lang/Error") ? mg_dvm_find_class("java/lang/Error") : NULL;
            if (e && error && !mg_dvm_is_instance(e, error) && mg_dvm_class_exists("java/lang/ExceptionInInitializerError")) {
                mg_jvalue arg;
                mg_jobj *wrapped;
                mg_dvm_clear_pending();
                arg.j = 0;
                arg.l = e;
                wrapped = mg_dvm_new("java/lang/ExceptionInInitializerError", "(Ljava/lang/Throwable;)V", &arg);
                if (wrapped) mg_dvm_throw_object(wrapped);
            }
        }
        break;
    }
    if (!ok) failed_first(c);
    mg_lock_acquire(&g_init_lock);
    c->state = ok ? DVM_CS_INITIALIZED : DVM_CS_FAILED;
    mg_cond_broadcast(&g_init_cond);
    mg_lock_release(&g_init_lock);
    if (ok) mg_atomic_add(&g_dvm.n_initialised, 1);
    return ok;
}

void mg_dvm_each_linked(void (*fn)(mg_dvm_class *c, void *user), void *user)
{
    mg_dvm_class **all, *c;
    size_t n = 0, cap = 0, i;
    /* A copy of the list first: `fn` runs without the lock, and classes linked meanwhile are simply not in it. */
    mg_dvm_lock();
    for (i = 0; i < CLASS_BUCKETS; i++)
        for (c = g_classes[i]; c; c = c->next) cap++;
    all = (mg_dvm_class **)mg_alloc(sizeof(*all) * (cap + 1));
    for (i = 0; i < CLASS_BUCKETS; i++)
        for (c = g_classes[i]; c; c = c->next) all[n++] = c;
    mg_dvm_unlock();
    for (i = 0; i < n; i++) fn(all[i], user);
    free(all);
}

void mg_dvm_each_linked_locked(void (*fn)(mg_dvm_class *c, void *user), void *user)
{
    mg_dvm_class *c;
    size_t i;
    for (i = 0; i < CLASS_BUCKETS; i++)
        for (c = g_classes[i]; c; c = c->next) fn(c, user);
}

mg_dvm_dex *mg_dvm_dex_at(u32 i) { return i < (u32)g_ndexes ? g_dexes[i] : NULL; }

mg_dvm_class *mg_dvm_make_proxy_class(const char *name, mg_dvm_class *super, mg_dvm_class **ifaces, int nifaces, mg_dvm_method *const *like, int nmethods,
                                      mg_jhle_fn invoke, mg_jhle_fn construct, const char *construct_sig)
{
    mg_dvm_class *c;
    int i, k, inherited;
    mg_dvm_lock();
    c = loaded(name);
    if (c) {
        mg_dvm_unlock();
        return c;
    }
    c = (mg_dvm_class *)mg_dvm_meta_alloc_kind(sizeof(*c), DVM_META_CLASS);
    c->name = mg_dvm_intern(name);
    /* 0x40000 is how libcore's Class.isProxy knows one (Android's kAccClassIsProxy). */
    c->flags = DVM_ACC_PUBLIC | DVM_ACC_FINAL | 0x40000;
    c->super = super;
    c->nslots = super->nslots;
    c->nifaces = nifaces;
    c->ifaces = (mg_dvm_class **)mg_dvm_meta_alloc_kind(sizeof(*c->ifaces) * (size_t)(nifaces + 1), DVM_META_CLASS);
    for (i = 0; i < nifaces; i++) c->ifaces[i] = ifaces[i];
    c->ndirect = 1;
    c->direct = (mg_dvm_method *)mg_dvm_meta_alloc_kind(sizeof(mg_dvm_method) * 2, DVM_META_METHOD);
    c->direct[0].cls = c;
    c->direct[0].name = mg_dvm_intern("<init>");
    c->direct[0].sig = mg_dvm_intern(construct_sig);
    c->direct[0].shorty = "VL";
    c->direct[0].flags = DVM_ACC_PUBLIC | DVM_ACC_CONSTRUCTOR | DVM_ACC_NATIVE;
    c->direct[0].nparams = 1;
    c->direct[0].vidx = -1;
    mg_dvm_native_of(&c->direct[0])->native = construct;
    c->direct[0].nat->bound = true;
    c->nvirt = nmethods;
    c->virt = (mg_dvm_method *)mg_dvm_meta_alloc_kind(sizeof(mg_dvm_method) * (size_t)(nmethods + 1), DVM_META_METHOD);
    inherited = super->nvtab;
    c->vtab = (mg_dvm_method **)mg_dvm_meta_alloc_kind(sizeof(*c->vtab) * (size_t)(inherited + nmethods + 1), DVM_META_VTABLE);
    memcpy(c->vtab, super->vtab, sizeof(*c->vtab) * (size_t)inherited);
    c->nvtab = inherited;
    for (i = 0; i < nmethods; i++) {
        mg_dvm_method *m = &c->virt[i];
        int slot = -1;
        m->cls = c;
        m->name = like[i]->name;
        m->sig = like[i]->sig;
        m->shorty = like[i]->shorty;
        m->nparams = like[i]->nparams;
        m->flags = DVM_ACC_PUBLIC | DVM_ACC_FINAL | DVM_ACC_NATIVE;
        m->idx = (u32)i;                                             /* which of the methods it stands for */
        mg_dvm_native_of(m)->native = invoke;
        m->nat->bound = true;
        /* The three of Object that a handler answers (equals, hashCode, toString) take their places in the vtable; the rest are new. */
        for (k = 0; k < inherited && slot < 0; k++)
            if (c->vtab[k]->name == m->name && c->vtab[k]->sig == m->sig) slot = k;
        if (slot < 0) slot = c->nvtab++;
        c->vtab[slot] = m;
        m->vidx = slot;
    }
    c->state = DVM_CS_INITIALIZED;
    publish(c);
    mg_dvm_unlock();
    return c;
}
