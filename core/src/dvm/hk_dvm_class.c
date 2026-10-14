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
#include "hk_dvm_internal.h"

#include <stdio.h>

hk_dvm_globals g_dvm;

/* ------------------------------------------------------------------ the lock */

static hk_lock g_big = HK_LOCK_INIT;
static volatile u32 g_big_owner;
static int g_big_count;

void hk_dvm_lock(void)
{
    u32 me = hk_thread_self_id();
    if (g_big_owner == me) {
        g_big_count++;
        return;
    }
    hk_lock_acquire(&g_big);
    g_big_owner = me;
    g_big_count = 1;
}

void hk_dvm_unlock(void)
{
    if (--g_big_count == 0) {
        g_big_owner = 0;
        hk_lock_release(&g_big);
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

#define NAME_BUCKETS 8192
static name_entry *g_names[NAME_BUCKETS];

const char *hk_dvm_intern(const char *s)
{
    size_t n = strlen(s);
    u32 h = hash_bytes(s, n) % NAME_BUCKETS;
    name_entry *e;
    hk_dvm_lock();
    for (e = g_names[h]; e; e = e->next)
        if (!strcmp(e->text, s)) break;
    if (!e) {
        e = (name_entry *)hk_dvm_meta_alloc(sizeof(*e) + n);
        memcpy(e->text, s, n + 1);
        e->next = g_names[h];
        g_names[h] = e;
    }
    hk_dvm_unlock();
    return e->text;
}

/* ------------------------------------------------- which file defines a class */

/*
 * Every class any DEX file defines, by name, first definition winning. An
 * entry points at the class's descriptor in its file ("Lfoo/Bar;") and copies
 * nothing: an app defines tens of thousands of classes and uses a fraction.
 */
typedef struct cdef {
    const char *desc;
    hk_dvm_dex *dex;
    u32 def;
    u32 next;                          /* index + 1 of the next entry in the bucket */
} cdef;

#define CDEF_BUCKETS 16384
static cdef *g_cdefs;
static u32 g_ncdefs, g_cdefs_cap;
static u32 g_cdef_buckets[CDEF_BUCKETS];
static hk_dvm_dex *g_dexes[256];
static int g_ndexes;

static const cdef *cdef_find(const char *name)
{
    size_t n = strlen(name);
    u32 i = g_cdef_buckets[hash_bytes(name, n) % CDEF_BUCKETS];
    while (i) {
        const cdef *c = &g_cdefs[i - 1];
        if (!strncmp(c->desc + 1, name, n) && c->desc[n + 1] == ';' && c->desc[n + 2] == 0) return c;
        i = c->next;
    }
    return NULL;
}

void hk_dvm_classes_add_dex(hk_dvm_dex *d)
{
    u32 i, added = 0;
    hk_dvm_lock();
    if (g_ndexes < (int)HK_COUNT(g_dexes)) g_dexes[g_ndexes++] = d;
    for (i = 0; i < d->ncls; i++) {
        const char *desc = hk_dvm_dex_type(d, hk_dvm_rd32(d->b + d->cls_off + 32 * i));
        size_t n = strlen(desc);
        char *name;
        u32 h;
        if (n < 3 || desc[0] != 'L' || desc[n - 1] != ';') continue;
        name = (char *)hk_dvm_meta_alloc(n);
        memcpy(name, desc + 1, n - 2);
        if (!cdef_find(name)) {
            if (g_ncdefs == g_cdefs_cap) {
                g_cdefs_cap = g_cdefs_cap ? g_cdefs_cap * 2 : 1024;
                g_cdefs = (cdef *)hk_realloc(g_cdefs, sizeof(cdef) * g_cdefs_cap);
            }
            h = hash_bytes(name, n - 2) % CDEF_BUCKETS;
            g_cdefs[g_ncdefs].desc = desc;
            g_cdefs[g_ncdefs].dex = d;
            g_cdefs[g_ncdefs].def = i;
            g_cdefs[g_ncdefs].next = g_cdef_buckets[h];
            g_cdef_buckets[h] = ++g_ncdefs;
            added++;
        }
        free(name);
    }
    hk_dvm_unlock();
    hk_info("dvm: %s: %u classes (%u new), %u methods, %u strings", d->name, (unsigned)d->ncls, (unsigned)added, (unsigned)d->nmeth, (unsigned)d->nstr);
}

u32 hk_dvm_defined_count(void) { return g_ncdefs; }
u32 hk_dvm_dex_count(void) { return (u32)g_ndexes; }

const char *hk_dvm_defined_name(u32 i, hk_dvm_dex **dex)
{
    char *name;
    const char *interned;
    size_t n;
    if (i >= g_ncdefs) return NULL;
    if (dex) *dex = g_cdefs[i].dex;
    n = strlen(g_cdefs[i].desc);
    name = (char *)hk_dvm_meta_alloc(n);
    memcpy(name, g_cdefs[i].desc + 1, n - 2);
    interned = hk_dvm_intern(name);
    free(name);
    return interned;
}

/* ---------------------------------------------------------- registered natives */

static const hk_jhle *g_tables[64], *g_fallbacks[16];
static int g_ntables, g_nfallbacks;

void hk_dvm_register_fallback(const hk_jhle *table)
{
    hk_dvm_lock();
    if (g_nfallbacks < (int)HK_COUNT(g_fallbacks)) g_fallbacks[g_nfallbacks++] = table;
    hk_dvm_unlock();
}

void hk_dvm_register(const hk_jhle *table)
{
    hk_dvm_lock();
    if (g_ntables < (int)HK_COUNT(g_tables)) g_tables[g_ntables++] = table;
    else hk_err("dvm: too many tables of native methods");
    hk_dvm_unlock();
}

static hk_jhle_fn registered(const char *cls, const char *name, const char *sig)
{
    int t;
    for (t = g_ntables - 1; t >= 0; t--) {
        const hk_jhle *h;
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
        for (i = 0; i < (int)HK_COUNT(prims); i++)
            if (prims[i][0] == *d) *k += (size_t)snprintf(out + *k, *k < n ? n - *k : 0, "%s", prims[i] + 1);
        d++;
    }
    for (; dims; dims--) *k += (size_t)snprintf(out + *k, *k < n ? n - *k : 0, "[]");
    if (*k >= n) *k = n - 1;
    out[*k] = 0;
    return d;
}

/* "void com.example.Foo.bar(int, java.lang.String)": a method as ART's messages name it. */
static void pretty_method(const hk_dvm_method *m, char *out, size_t n)
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

bool hk_dvm_bind_native(hk_dvm_method *m)
{
    hk_jhle_fn fn;
    void *jni = m->jni;
    if (m->native) return true;
    /* What was registered here in C comes before a library's function, whichever was there first. */
    hk_dvm_lock();
    fn = registered(m->cls->name, m->name, m->sig);
    hk_dvm_unlock();
    if (!fn && jni) {
        m->bound = true;
        return true;
    }
    if (!fn && g_dvm.jni.find) jni = g_dvm.jni.find(m->cls->name, m->name, m->sig, hk_dvm_class_is_app(m->cls));
    if (!fn && !jni) {
        /* Last, what stands in for a library that is not there. */
        const hk_jhle *h;
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
        return hk_dvm_throw("java/lang/UnsatisfiedLinkError", "No implementation found for %s%s%s%s", pretty, tried[0] ? " (tried " : "", tried, tried[0] ? ")" : "");
    }
    if (g_dvm.trace >= 1) hk_info("dvm: native %s.%s%s bound to %s", m->cls->name, m->name, m->sig, fn ? "a C implementation" : "the app's library");
    if (fn) m->native = fn;
    else m->jni = jni;
    m->bound = true;
    return true;
}

/* ------------------------------------------------------------- linked classes */

#define CLASS_BUCKETS 4096
static hk_dvm_class *g_classes[CLASS_BUCKETS];

/* What stopped a class from being linked: the exception to throw once the lock is released. */
typedef struct link_error {
    const char *cls;
    char msg[400];
} link_error;

static void link_fail(link_error *err, const char *cls, const char *fmt, ...) HK_PRINTF(3, 4);
static void link_fail(link_error *err, const char *cls, const char *fmt, ...)
{
    va_list ap;
    if (err->cls) return;              /* the first reason is the one worth reporting */
    err->cls = cls;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
}

static hk_dvm_class *loaded(const char *name)
{
    hk_dvm_class *c;
    for (c = g_classes[hash_bytes(name, strlen(name)) % CLASS_BUCKETS]; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

static void publish(hk_dvm_class *c)
{
    u32 h = hash_bytes(c->name, strlen(c->name)) % CLASS_BUCKETS;
    if (!c->jc && g_dvm.jni.class_for) c->jc = g_dvm.jni.class_for(c->name, c->super ? c->super->name : NULL, c);
    c->next = g_classes[h];
    g_classes[h] = c;
    hk_atomic_add(&g_dvm.n_linked, 1);
    if (!strcmp(c->name, "java/lang/Object")) g_dvm.c_object = c;
    else if (!strcmp(c->name, "java/lang/Class")) g_dvm.c_class = c;
    else if (!strcmp(c->name, "java/lang/String")) g_dvm.c_string = c;
    else if (!strcmp(c->name, "java/lang/Cloneable")) g_dvm.c_cloneable = c;
    else if (!strcmp(c->name, "java/io/Serializable")) g_dvm.c_serializable = c;
    else if (!strcmp(c->name, "java/lang/Throwable")) {
        g_dvm.c_throwable = c;
        g_dvm.f_message = hk_dvm_find_field(c, "detailMessage", false);
        g_dvm.f_cause = hk_dvm_find_field(c, "cause", false);
        g_dvm.f_backtrace = hk_dvm_find_field(c, "backtrace", false);
    }
    if (g_dvm.trace >= 1) hk_info("dvm: linked %s (%u slots, %d in the vtable)", c->name, (unsigned)c->nslots, c->nvtab);
}

static hk_dvm_class *load_class(const char *name, link_error *err);

static hk_dvm_class *load_class_desc(const char *desc, link_error *err)
{
    static const struct { char letter; const char *name; } prims[] = { { 'I', "int" }, { 'J', "long" }, { 'F', "float" }, { 'D', "double" },
        { 'Z', "boolean" }, { 'B', "byte" }, { 'C', "char" }, { 'S', "short" }, { 'V', "void" } };
    size_t n = strlen(desc), i;
    hk_dvm_class *c;
    char *name;
    if (n == 1) {
        for (i = 0; i < HK_COUNT(prims); i++)
            if (prims[i].letter == desc[0]) return load_class(prims[i].name, err);
    }
    if (desc[0] == '[') return load_class(desc, err);
    if (n < 3 || desc[0] != 'L' || desc[n - 1] != ';') {
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", desc);
        return NULL;
    }
    name = (char *)hk_dvm_meta_alloc(n);
    memcpy(name, desc + 1, n - 2);
    c = load_class(name, err);
    free(name);
    return c;
}

static bool same_package(const char *a, const char *b)
{
    const char *sa = strrchr(a, '/'), *sb = strrchr(b, '/');
    size_t la = sa ? (size_t)(sa - a) : 0, lb = sb ? (size_t)(sb - b) : 0;
    return la == lb && !strncmp(a, b, la);
}

static hk_dvm_class *link_primitive(const char *name, char letter)
{
    hk_dvm_class *c = (hk_dvm_class *)hk_dvm_meta_alloc(sizeof(*c));
    c->name = hk_dvm_intern(name);
    c->prim = letter;
    c->flags = DVM_ACC_PUBLIC | DVM_ACC_FINAL | DVM_ACC_ABSTRACT;
    c->state = DVM_CS_INITIALIZED;
    publish(c);
    return c;
}

static hk_dvm_class *link_array(const char *name, link_error *err)
{
    hk_dvm_class *c, *component, *object, *cloneable, *serializable;
    if (!name[1] || (name[1] == 'L' && !strchr(name, ';'))) {
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    component = load_class_desc(name + 1, err);
    object = component ? load_class("java/lang/Object", err) : NULL;
    if (!object) return NULL;
    if (component->prim == 'V') {
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    c = (hk_dvm_class *)hk_dvm_meta_alloc(sizeof(*c));
    c->name = hk_dvm_intern(name);
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
    c->ifaces = (hk_dvm_class **)hk_dvm_meta_alloc(sizeof(*c->ifaces) * 2);
    if (cloneable) c->ifaces[c->nifaces++] = cloneable;
    if (serializable) c->ifaces[c->nifaces++] = serializable;
    c->state = DVM_CS_INITIALIZED;
    publish(c);
    return c;
}

static void free_unlinked(hk_dvm_class *c)
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

static bool read_method(hk_dvm_class *c, hk_dvm_method *m, u32 idx, u32 flags, u32 code_off, link_error *err)
{
    hk_dvm_dex *d = c->dex;
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
    m->name = hk_dvm_intern(hk_dvm_dex_string(d, hk_dvm_rd32(mi + 4), NULL));
    m->sig = hk_dvm_dex_proto_sig(d, hk_dvm_rd16(mi + 2));
    m->shorty = hk_dvm_dex_proto_shorty(d, hk_dvm_rd16(mi + 2));
    m->nparams = (u16)(strlen(m->shorty) ? strlen(m->shorty) - 1 : 0);
    if (code_off) {
        const u8 *ci = d->b + code_off;
        u32 words = 0;
        const char *s;
        if ((u64)code_off + 16 > d->size || (code_off & 3)) {
            link_fail(err, "java/lang/ClassFormatError", "%s.%s: its code lies outside the file", c->name, m->name);
            return false;
        }
        m->code = ci;
        m->regs = hk_dvm_rd16(ci);
        m->ins = hk_dvm_rd16(ci + 2);
        m->outs = hk_dvm_rd16(ci + 4);
        m->ntries = hk_dvm_rd16(ci + 6);
        m->ninsns = hk_dvm_rd32(ci + 12);
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

static hk_dvm_class *link_dex_class(const cdef *cd, const char *name, link_error *err)
{
    hk_dvm_dex *d = cd->dex;
    const u8 *def = d->b + d->cls_off + 32 * cd->def, *end = d->b + d->size;
    hk_dvm_class *c = (hk_dvm_class *)hk_dvm_meta_alloc(sizeof(*c));
    u32 super_idx = hk_dvm_rd32(def + 8), ifaces_off = hk_dvm_rd32(def + 12), data_off = hk_dvm_rd32(def + 24);
    int inherited, i, k;
    c->name = hk_dvm_intern(name);
    c->dex = d;
    c->def = cd->def;
    c->flags = hk_dvm_rd32(def + 4);

    if (super_idx != 0xFFFFFFFFu) {
        c->super = load_class_desc(hk_dvm_dex_type(d, super_idx), err);
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
        if ((u64)ifaces_off + 4 > d->size || (u64)ifaces_off + 4 + (u64)hk_dvm_rd32(d->b + ifaces_off) * 2 > d->size) {
            link_fail(err, "java/lang/ClassFormatError", "%s: its interface list lies outside the file", name);
            goto fail;
        }
        n = hk_dvm_rd32(d->b + ifaces_off);
        c->ifaces = (hk_dvm_class **)hk_dvm_meta_alloc(sizeof(*c->ifaces) * (n + 1));
        for (j = 0; j < n; j++) {
            hk_dvm_class *ic = load_class_desc(hk_dvm_dex_type(d, hk_dvm_rd16(d->b + ifaces_off + 4 + 2 * j)), err);
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
        nsf = hk_dvm_uleb(&p);
        nif = hk_dvm_uleb(&p);
        ndm = hk_dvm_uleb(&p);
        nvm = hk_dvm_uleb(&p);
        if (nsf > d->nfield || nif > d->nfield || ndm > d->nmeth || nvm > d->nmeth) {
            link_fail(err, "java/lang/ClassFormatError", "%s: more members than the file has", name);
            goto fail;
        }
        c->sfields = (hk_dvm_field *)hk_dvm_meta_alloc(sizeof(hk_dvm_field) * (nsf + 1));
        c->ifields = (hk_dvm_field *)hk_dvm_meta_alloc(sizeof(hk_dvm_field) * (nif + 1));
        c->direct = (hk_dvm_method *)hk_dvm_meta_alloc(sizeof(hk_dvm_method) * (ndm + 1));
        c->virt = (hk_dvm_method *)hk_dvm_meta_alloc(sizeof(hk_dvm_method) * (nvm + 1));
        for (group = 0; group < 2; group++) {
            u32 n = group ? nif : nsf;
            idx = 0;
            for (j = 0; j < n; j++) {
                hk_dvm_field *f = group ? &c->ifields[c->nifields] : &c->sfields[c->nsfields];
                const u8 *fi;
                if (p >= end) goto truncated;
                idx += hk_dvm_uleb(&p);
                f->flags = hk_dvm_uleb(&p);
                if (idx >= d->nfield) goto truncated;
                fi = d->b + d->field_off + 8 * idx;
                f->cls = c;
                f->idx = idx;
                f->type = hk_dvm_intern(hk_dvm_dex_type(d, hk_dvm_rd16(fi + 2)));
                f->name = hk_dvm_intern(hk_dvm_dex_string(d, hk_dvm_rd32(fi + 4), NULL));
                if (group) {
                    /* A few fields of libcore's are the runtime's own and have no slot (hk_dvm_internal.h, DVM_SLOT_KLASS). */
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
                hk_dvm_method *m = group ? &c->virt[c->nvirt] : &c->direct[c->ndirect];
                u32 flags, code_off;
                if (p >= end) goto truncated;
                idx += hk_dvm_uleb(&p);
                flags = hk_dvm_uleb(&p);
                code_off = hk_dvm_uleb(&p);
                if (!read_method(c, m, idx, flags, code_off, err)) goto fail;
                if (group) c->nvirt++;
                else c->ndirect++;
            }
        }
    }
    if (c->nsfields) c->statics = (hk_jvalue *)hk_dvm_meta_alloc(sizeof(hk_jvalue) * (size_t)c->nsfields);

    /*
     * The vtable: the superclass's, a method of the same name and signature
     * taking its slot (a package-private one only within its package), new
     * methods appended.
     */
    inherited = c->super ? c->super->nvtab : 0;
    c->vtab = (hk_dvm_method **)hk_dvm_meta_alloc(sizeof(*c->vtab) * (size_t)(inherited + c->nvirt + 1));
    if (inherited) memcpy(c->vtab, c->super->vtab, sizeof(*c->vtab) * (size_t)inherited);
    c->nvtab = inherited;
    for (i = 0; i < c->nvirt; i++) {
        hk_dvm_method *m = &c->virt[i];
        int slot = -1;
        for (k = 0; k < inherited; k++) {
            const hk_dvm_method *o = c->vtab[k];
            if (o->name != m->name || o->sig != m->sig) continue;
            if (!(o->flags & (DVM_ACC_PUBLIC | DVM_ACC_PROTECTED | DVM_ACC_PRIVATE)) && !same_package(o->cls->name, c->name)) continue;
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
static int g_nlinking;

static hk_dvm_class *load_class(const char *name, link_error *err)
{
    static const struct { const char *name; char letter; } prims[] = { { "int", 'I' }, { "long", 'J' }, { "float", 'F' }, { "double", 'D' },
        { "boolean", 'Z' }, { "byte", 'B' }, { "char", 'C' }, { "short", 'S' }, { "void", 'V' } };
    hk_dvm_class *c = loaded(name);
    const cdef *cd;
    size_t i;
    int k;
    if (c) return c;
    if (name[0] == '[') return link_array(name, err);
    cd = cdef_find(name);
    if (!cd) {
        for (i = 0; i < HK_COUNT(prims); i++)
            if (!strcmp(prims[i].name, name)) return link_primitive(name, prims[i].letter);
        link_fail(err, "java/lang/NoClassDefFoundError", "%s", name);
        return NULL;
    }
    for (k = 0; k < g_nlinking; k++) {
        if (!strcmp(g_linking[k], name)) {
            link_fail(err, "java/lang/ClassCircularityError", "%s", name);
            return NULL;
        }
    }
    if (g_nlinking >= (int)HK_COUNT(g_linking)) {
        link_fail(err, "java/lang/ClassFormatError", "%s: its superclasses nest too deep", name);
        return NULL;
    }
    g_linking[g_nlinking++] = name;
    c = link_dex_class(cd, name, err);
    g_nlinking--;
    return c;
}

/* Throws what stopped the linking. A class that was missing further up says who needed it. */
static void throw_link_error(const link_error *err, const char *asked)
{
    if (!err->cls) hk_dvm_throw("java/lang/NoClassDefFoundError", "%s", asked);
    else if (!strcmp(err->cls, "java/lang/NoClassDefFoundError") && strcmp(err->msg, asked) != 0) hk_dvm_throw(err->cls, "%s (needed by %s)", err->msg, asked);
    else hk_dvm_throw(err->cls, "%s", err->msg);
}

hk_dvm_class *hk_dvm_find_class(const char *name)
{
    link_error err;
    hk_dvm_class *c;
    err.cls = NULL;
    err.msg[0] = 0;
    hk_dvm_lock();
    c = load_class(name, &err);
    hk_dvm_unlock();
    if (!c) throw_link_error(&err, name);
    return c;
}

hk_dvm_class *hk_dvm_find_class_desc(const char *descriptor)
{
    link_error err;
    hk_dvm_class *c;
    char name[512];
    err.cls = NULL;
    err.msg[0] = 0;
    hk_dvm_lock();
    c = load_class_desc(descriptor, &err);
    hk_dvm_unlock();
    if (!c) throw_link_error(&err, hk_dvm_descriptor_name(descriptor, name, sizeof(name)) ? name : descriptor);
    return c;
}

bool hk_dvm_class_exists(const char *name)
{
    static const char *const prims[] = { "int", "long", "float", "double", "boolean", "byte", "char", "short", "void" };
    size_t i;
    bool yes;
    if (name[0] == '[') return true;
    for (i = 0; i < HK_COUNT(prims); i++)
        if (!strcmp(prims[i], name)) return true;
    hk_dvm_lock();
    yes = loaded(name) != NULL || cdef_find(name) != NULL;
    hk_dvm_unlock();
    return yes;
}

/* ------------------------------------------------------------------- lookups */

/* A method an interface declares or inherits; a concrete (default) one when `concrete`. */
static hk_dvm_method *interface_method(hk_dvm_class *iface, const char *name, const char *sig, bool concrete)
{
    int i;
    for (i = 0; i < iface->nvirt; i++) {
        hk_dvm_method *m = &iface->virt[i];
        if (m->name == name && m->sig == sig && (!concrete || !(m->flags & DVM_ACC_ABSTRACT))) return m;
    }
    for (i = 0; i < iface->nifaces; i++) {
        hk_dvm_method *m = interface_method(iface->ifaces[i], name, sig, concrete);
        if (m) return m;
    }
    return NULL;
}

static hk_dvm_method *find_method_interned(hk_dvm_class *c, const char *name, const char *sig, bool is_static)
{
    hk_dvm_class *k;
    int i;
    for (k = c; k; k = k->super) {
        for (i = 0; i < k->ndirect; i++) {
            hk_dvm_method *m = &k->direct[i];
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
            hk_dvm_method *m = interface_method(k->ifaces[i], name, sig, false);
            if (m) return m;
        }
    return NULL;
}

hk_dvm_method *hk_dvm_find_method(hk_dvm_class *c, const char *name, const char *sig, bool is_static)
{
    return find_method_interned(c, hk_dvm_intern(name), hk_dvm_intern(sig), is_static);
}

hk_dvm_method *hk_dvm_find_virtual(hk_dvm_class *receiver, const char *name, const char *sig)
{
    hk_dvm_method *abstract_hit = NULL;
    hk_dvm_class *k;
    int i;
    for (k = receiver; k; k = k->super)
        for (i = 0; i < k->nvirt; i++) {
            hk_dvm_method *m = &k->virt[i];
            if (m->name != name || m->sig != sig) continue;
            if (!(m->flags & DVM_ACC_ABSTRACT)) return m;
            if (!abstract_hit) abstract_hit = m;
        }
    for (k = receiver; k; k = k->super)
        for (i = 0; i < k->nifaces; i++) {
            hk_dvm_method *m = interface_method(k->ifaces[i], name, sig, true);
            if (m) return m;
        }
    if (abstract_hit) return abstract_hit;
    /* Declared by an interface and implemented by nothing: the abstract method itself, which whoever calls it reports as such. */
    for (k = receiver; k; k = k->super)
        for (i = 0; i < k->nifaces; i++) {
            hk_dvm_method *m = interface_method(k->ifaces[i], name, sig, false);
            if (m) return m;
        }
    return NULL;
}

/* A field by name and, when `type` is given, type: obfuscated code gives fields of different types the same name. */
static hk_dvm_field *find_field_interned(hk_dvm_class *c, const char *name, const char *type, bool is_static)
{
    hk_dvm_class *k;
    int i;
    for (k = c; k; k = k->super) {
        hk_dvm_field *list = is_static ? k->sfields : k->ifields;
        int n = is_static ? k->nsfields : k->nifields;
        for (i = 0; i < n; i++)
            if (list[i].name == name && (!type || list[i].type == type)) return &list[i];
        if (is_static)
            for (i = 0; i < k->nifaces; i++) {
                hk_dvm_field *f = find_field_interned(k->ifaces[i], name, type, true);
                if (f) return f;
            }
    }
    return NULL;
}

hk_dvm_field *hk_dvm_find_field(hk_dvm_class *c, const char *name, bool is_static)
{
    return find_field_interned(c, hk_dvm_intern(name), NULL, is_static);
}

hk_dvm_field *hk_dvm_find_field_typed(hk_dvm_class *c, const char *name, const char *type, bool is_static)
{
    return find_field_interned(c, hk_dvm_intern(name), hk_dvm_intern(type), is_static);
}

hk_dvm_method *hk_dvm_virtual_target(hk_dvm_method *m, hk_jobj *self)
{
    hk_dvm_class *rc = hk_dvm_class_of(self);
    hk_dvm_method *t;
    if (!(m->cls->flags & DVM_ACC_INTERFACE)) {
        if (m->vidx < 0) return m;                               /* private, a constructor: itself */
        if (m->vidx < rc->nvtab) return rc->vtab[m->vidx];
    }
    t = hk_dvm_find_virtual(rc, m->name, m->sig);
    return t ? t : m;
}

hk_dvm_class *hk_dvm_foreign_class(const char *name, hk_jclass *jc)
{
    hk_dvm_class *c, *object;
    link_error err;
    err.cls = NULL;
    err.msg[0] = 0;
    hk_dvm_lock();
    c = loaded(name);
    object = c ? NULL : load_class("java/lang/Object", &err);
    if (!c && object) {
        c = (hk_dvm_class *)hk_dvm_meta_alloc(sizeof(*c));
        c->name = hk_dvm_intern(name);
        c->flags = DVM_ACC_PUBLIC;
        c->super = object;
        c->vtab = object->vtab;
        c->nvtab = object->nvtab;
        c->state = DVM_CS_INITIALIZED;
        c->foreign = true;
        c->jc = jc;
        publish(c);
    }
    hk_dvm_unlock();
    return c;
}

bool hk_dvm_native_registered(hk_dvm_class *c, const char *name, const char *sig, void *fn)
{
    const char *iname = hk_dvm_intern(name), *isig = hk_dvm_intern(sig);
    int group, i;
    for (group = 0; group < 2; group++) {
        hk_dvm_method *list = group ? c->virt : c->direct;
        int n = group ? c->nvirt : c->ndirect;
        for (i = 0; i < n; i++) {
            if (list[i].name != iname || list[i].sig != isig || !(list[i].flags & DVM_ACC_NATIVE)) continue;
            list[i].jni = fn;
            return true;
        }
    }
    return false;
}

static bool implements(hk_dvm_class *c, hk_dvm_class *iface)
{
    int i;
    if (c == iface) return true;
    for (i = 0; i < c->nifaces; i++)
        if (implements(c->ifaces[i], iface)) return true;
    return false;
}

bool hk_dvm_assignable(hk_dvm_class *sub, hk_dvm_class *sup)
{
    hk_dvm_class *k;
    int i;
    if (!sub || !sup) return false;
    if (sub == sup) return true;
    if (sub->prim || sup->prim) return false;
    if (sup == g_dvm.c_object) return true;
    if (sub->name[0] == '[') {
        if (sup == g_dvm.c_cloneable || sup == g_dvm.c_serializable) return true;
        if (sup->name[0] != '[') return false;
        /* Arrays of references follow their elements' classes; arrays of primitives are only themselves. */
        return sub->elem == 'L' && sup->elem == 'L' && hk_dvm_assignable(sub->component, sup->component);
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

hk_dvm_class *hk_dvm_resolve_type(hk_dvm_dex *d, u32 idx)
{
    hk_dvm_class *c;
    if (idx >= d->ntype) {
        hk_dvm_throw("java/lang/VerifyError", "type index %u of %u in %s", (unsigned)idx, (unsigned)d->ntype, d->name);
        return NULL;
    }
    c = d->tcache[idx];
    if (c) return c;
    c = hk_dvm_find_class_desc(hk_dvm_dex_type(d, idx));
    if (c) d->tcache[idx] = c;
    return c;
}

hk_dvm_method *hk_dvm_resolve_method(hk_dvm_dex *d, u32 idx, int kind)
{
    bool want_static = kind == DVM_INVOKE_STATIC;
    hk_dvm_method *m;
    hk_dvm_class *c;
    const char *name, *sig;
    const u8 *mi;
    if (idx >= d->nmeth) {
        hk_dvm_throw("java/lang/VerifyError", "method index %u of %u in %s", (unsigned)idx, (unsigned)d->nmeth, d->name);
        return NULL;
    }
    m = d->mcache[idx];
    if (!m) {
        mi = d->b + d->meth_off + 8 * idx;
        c = hk_dvm_resolve_type(d, hk_dvm_rd16(mi));
        if (!c) return NULL;
        name = hk_dvm_intern(hk_dvm_dex_string(d, hk_dvm_rd32(mi + 4), NULL));
        sig = hk_dvm_dex_proto_sig(d, hk_dvm_rd16(mi + 2));
        m = find_method_interned(c, name, sig, want_static);
        if (!m) {
            if (find_method_interned(c, name, sig, !want_static)) {
                hk_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s%s is %s", c->name, name, sig, want_static ? "not static" : "static");
            } else {
                hk_dvm_throw("java/lang/NoSuchMethodError", "%s.%s%s", c->name, name, sig);
            }
            return NULL;
        }
        d->mcache[idx] = m;
    }
    if (((m->flags & DVM_ACC_STATIC) != 0) != want_static) {
        hk_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s%s is %s", m->cls->name, m->name, m->sig, want_static ? "not static" : "static");
        return NULL;
    }
    return m;
}

hk_dvm_field *hk_dvm_resolve_field(hk_dvm_dex *d, u32 idx, bool is_static)
{
    hk_dvm_field *f;
    hk_dvm_class *c;
    const char *name, *type;
    const u8 *fi;
    if (idx >= d->nfield) {
        hk_dvm_throw("java/lang/VerifyError", "field index %u of %u in %s", (unsigned)idx, (unsigned)d->nfield, d->name);
        return NULL;
    }
    f = d->fcache[idx];
    if (!f) {
        fi = d->b + d->field_off + 8 * idx;
        c = hk_dvm_resolve_type(d, hk_dvm_rd16(fi));
        if (!c) return NULL;
        type = hk_dvm_intern(hk_dvm_dex_type(d, hk_dvm_rd16(fi + 2)));
        name = hk_dvm_intern(hk_dvm_dex_string(d, hk_dvm_rd32(fi + 4), NULL));
        f = find_field_interned(c, name, type, is_static);
        if (!f) {
            if (find_field_interned(c, name, type, !is_static)) {
                hk_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s is %s", c->name, name, is_static ? "not static" : "static");
            } else {
                hk_dvm_throw("java/lang/NoSuchFieldError", "%s.%s:%s", c->name, name, type);
            }
            return NULL;
        }
        d->fcache[idx] = f;
    }
    if (((f->flags & DVM_ACC_STATIC) != 0) != is_static) {
        hk_dvm_throw("java/lang/IncompatibleClassChangeError", "%s.%s is %s", f->cls->name, f->name, is_static ? "not static" : "static");
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
static bool read_static_value(hk_dvm_dex *d, const u8 **pp, const u8 *end, hk_jvalue *out)
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
        out->l = hk_dvm_const_string(d, (u32)v);
        ok = out->l != NULL;
        p += size;
        break;
    case 0x18: {                                                /* a class */
        hk_dvm_class *c;
        for (i = 0; i < size; i++) v |= (u64)p[i] << (8 * i);
        c = hk_dvm_resolve_type(d, (u32)v);
        out->l = c ? hk_dvm_mirror(c) : NULL;
        ok = c != NULL;
        p += size;
        break;
    }
    case 0x1c: {                                                /* an array: not a static field's value in practice; skipped */
        u32 n = hk_dvm_uleb(&p), k;
        hk_jvalue ignored;
        for (k = 0; k < n && p < end; k++) read_static_value(d, &p, end, &ignored);
        break;
    }
    case 0x1d: {                                                /* an annotation: skipped */
        u32 n, k;
        hk_jvalue ignored;
        hk_dvm_uleb(&p);
        n = hk_dvm_uleb(&p);
        for (k = 0; k < n && p < end; k++) {
            hk_dvm_uleb(&p);
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

static bool init_statics(hk_dvm_class *c)
{
    hk_dvm_dex *d = c->dex;
    const u8 *p, *end;
    u32 off, n, i;
    if (!d || !c->nsfields) return true;
    off = hk_dvm_rd32(d->b + d->cls_off + 32 * c->def + 28);
    if (!off || off >= d->size) return true;
    p = d->b + off;
    end = d->b + d->size;
    n = hk_dvm_uleb(&p);
    for (i = 0; i < n && (int)i < c->nsfields; i++)
        if (!read_static_value(d, &p, end, &c->statics[i])) return false;
    return true;
}

static hk_lock g_init_lock = HK_LOCK_INIT;
static hk_cond g_init_cond = HK_COND_INIT;

/*
 * A class whose initialisation failed cannot be used again, and every later use is NoClassDefFoundError "Could not
 * initialize class X". The first failure is what says why, and an app often catches it and goes on -- kotlinx.coroutines
 * does, for the service it looks up -- so that the error that ends the app names only the class. As Java 17 and ART do,
 * the later errors have the first one for their cause; and the first is said in the log when it happens, with where, once
 * for each class (failed_first).
 */
static bool failed_before(hk_dvm_class *c)
{
    hk_jobj *e;
    hk_dvm_throw("java/lang/NoClassDefFoundError", "Could not initialize class %s", c->name);
    e = hk_dvm_pending();
    if (e && c->init_error && c->init_error != e) {
        hk_jvalue arg;
        hk_dvm_clear_pending();
        arg.j = 0;
        arg.l = c->init_error;
        /* (Throwable.initCause; should it refuse, the error goes without a cause.) */
        if (!hk_dvm_call_virtual(e, "initCause", "(Ljava/lang/Throwable;)Ljava/lang/Throwable;", &arg, NULL)) hk_dvm_clear_pending();
        hk_dvm_throw_object(e);
    }
    return false;
}

static void failed_first(hk_dvm_class *c)
{
    char what[700], where[1600];
    c->init_error = hk_dvm_pending();
    if (!c->init_error) {
        hk_warn("dvm: the initialisation of %s failed, and the class cannot be used from now on", c->name);
        return;
    }
    hk_dvm_describe_pending(what, sizeof(what));
    where[0] = 0;
    hk_dvm_pending_stack(where, sizeof(where));
    hk_warn("dvm: the initialisation of %s failed, and the class cannot be used from now on: %s\n%s", c->name, what, where);
}

bool hk_dvm_ensure_init(hk_dvm_class *c)
{
    u32 me;
    bool ok;
    int i;
    if (c->state == DVM_CS_INITIALIZED) return true;
    me = hk_thread_self_id();
    hk_lock_acquire(&g_init_lock);
    for (;;) {
        if (c->state == DVM_CS_INITIALIZED) {
            hk_lock_release(&g_init_lock);
            return true;
        }
        if (c->state == DVM_CS_FAILED) {
            hk_lock_release(&g_init_lock);
            return failed_before(c);
        }
        if (c->state == DVM_CS_LINKED) break;
        /* Being initialised. By this thread: its initialiser has reached back to the class, which it may use half-made, as Java says. */
        if (c->init_thread == me) {
            hk_lock_release(&g_init_lock);
            return true;
        }
        hk_cond_wait(&g_init_cond, &g_init_lock);
    }
    c->state = DVM_CS_INITIALIZING;
    c->init_thread = me;
    hk_lock_release(&g_init_lock);

    ok = (!c->super || hk_dvm_ensure_init(c->super)) && init_statics(c);
    if (ok && g_dvm.before_init) g_dvm.before_init(c);
    for (i = 0; ok && i < c->ndirect; i++) {
        hk_dvm_method *m = &c->direct[i];
        hk_jvalue ignored;
        if (strcmp(m->name, "<clinit>") != 0) continue;
        if (g_dvm.trace >= 1) hk_info("dvm: <clinit> %s", c->name);
        ok = hk_dvm_invoke(m, NULL, NULL, &ignored);
        if (!ok) {
            /* An exception that is not an Error is wrapped, as Java wraps it: the class is unusable from now on and this says why. */
            hk_jobj *e = hk_dvm_pending();
            hk_dvm_class *error = hk_dvm_class_exists("java/lang/Error") ? hk_dvm_find_class("java/lang/Error") : NULL;
            if (e && error && !hk_dvm_is_instance(e, error) && hk_dvm_class_exists("java/lang/ExceptionInInitializerError")) {
                hk_jvalue arg;
                hk_jobj *wrapped;
                hk_dvm_clear_pending();
                arg.j = 0;
                arg.l = e;
                wrapped = hk_dvm_new("java/lang/ExceptionInInitializerError", "(Ljava/lang/Throwable;)V", &arg);
                if (wrapped) hk_dvm_throw_object(wrapped);
            }
        }
        break;
    }
    if (!ok) failed_first(c);
    hk_lock_acquire(&g_init_lock);
    c->state = ok ? DVM_CS_INITIALIZED : DVM_CS_FAILED;
    hk_cond_broadcast(&g_init_cond);
    hk_lock_release(&g_init_lock);
    if (ok) hk_atomic_add(&g_dvm.n_initialised, 1);
    return ok;
}

void hk_dvm_each_linked(void (*fn)(hk_dvm_class *c, void *user), void *user)
{
    hk_dvm_class **all, *c;
    size_t n = 0, cap = 0, i;
    /* A copy of the list first: `fn` runs without the lock, and classes linked meanwhile are simply not in it. */
    hk_dvm_lock();
    for (i = 0; i < CLASS_BUCKETS; i++)
        for (c = g_classes[i]; c; c = c->next) cap++;
    all = (hk_dvm_class **)hk_alloc(sizeof(*all) * (cap + 1));
    for (i = 0; i < CLASS_BUCKETS; i++)
        for (c = g_classes[i]; c; c = c->next) all[n++] = c;
    hk_dvm_unlock();
    for (i = 0; i < n; i++) fn(all[i], user);
    free(all);
}

hk_dvm_class *hk_dvm_make_proxy_class(const char *name, hk_dvm_class *super, hk_dvm_class **ifaces, int nifaces, hk_dvm_method *const *like, int nmethods,
                                      hk_jhle_fn invoke, hk_jhle_fn construct, const char *construct_sig)
{
    hk_dvm_class *c;
    int i, k, inherited;
    hk_dvm_lock();
    c = loaded(name);
    if (c) {
        hk_dvm_unlock();
        return c;
    }
    c = (hk_dvm_class *)hk_dvm_meta_alloc(sizeof(*c));
    c->name = hk_dvm_intern(name);
    /* 0x40000 is how libcore's Class.isProxy knows one (Android's kAccClassIsProxy). */
    c->flags = DVM_ACC_PUBLIC | DVM_ACC_FINAL | 0x40000;
    c->super = super;
    c->nslots = super->nslots;
    c->nifaces = nifaces;
    c->ifaces = (hk_dvm_class **)hk_dvm_meta_alloc(sizeof(*c->ifaces) * (size_t)(nifaces + 1));
    for (i = 0; i < nifaces; i++) c->ifaces[i] = ifaces[i];
    c->ndirect = 1;
    c->direct = (hk_dvm_method *)hk_dvm_meta_alloc(sizeof(hk_dvm_method) * 2);
    c->direct[0].cls = c;
    c->direct[0].name = hk_dvm_intern("<init>");
    c->direct[0].sig = hk_dvm_intern(construct_sig);
    c->direct[0].shorty = "VL";
    c->direct[0].flags = DVM_ACC_PUBLIC | DVM_ACC_CONSTRUCTOR | DVM_ACC_NATIVE;
    c->direct[0].nparams = 1;
    c->direct[0].vidx = -1;
    c->direct[0].native = construct;
    c->direct[0].bound = true;
    c->nvirt = nmethods;
    c->virt = (hk_dvm_method *)hk_dvm_meta_alloc(sizeof(hk_dvm_method) * (size_t)(nmethods + 1));
    inherited = super->nvtab;
    c->vtab = (hk_dvm_method **)hk_dvm_meta_alloc(sizeof(*c->vtab) * (size_t)(inherited + nmethods + 1));
    memcpy(c->vtab, super->vtab, sizeof(*c->vtab) * (size_t)inherited);
    c->nvtab = inherited;
    for (i = 0; i < nmethods; i++) {
        hk_dvm_method *m = &c->virt[i];
        int slot = -1;
        m->cls = c;
        m->name = like[i]->name;
        m->sig = like[i]->sig;
        m->shorty = like[i]->shorty;
        m->nparams = like[i]->nparams;
        m->flags = DVM_ACC_PUBLIC | DVM_ACC_FINAL | DVM_ACC_NATIVE;
        m->idx = (u32)i;                                             /* which of the methods it stands for */
        m->native = invoke;
        m->bound = true;
        /* The three of Object that a handler answers (equals, hashCode, toString) take their places in the vtable; the rest are new. */
        for (k = 0; k < inherited && slot < 0; k++)
            if (c->vtab[k]->name == m->name && c->vtab[k]->sig == m->sig) slot = k;
        if (slot < 0) slot = c->nvtab++;
        c->vtab[slot] = m;
        m->vidx = slot;
    }
    c->state = DVM_CS_INITIALIZED;
    publish(c);
    hk_dvm_unlock();
    return c;
}
