/*
 * Objects, arrays, strings and Class objects; monitors; the calling thread's
 * state; making and describing exceptions.
 *
 * Nothing is freed yet. An object is one zeroed allocation from the runtime's
 * heap (mg_dvm_heap.c) that lives as long as the process; docs/JAVA.md has the
 * collector's design, and everything here goes through alloc_raw.
 */
#include "mg_dvm_internal.h"

#include <stdio.h>

/* ------------------------------------------------------------------ threads */

static MG_THREAD_LOCAL mg_dvm_thread *t_self;
static mg_dvm_thread *g_threads;
/*
 * What threads that have ended had made and executed (the living ones keep their own counts: mg_dvm_thread.made_objects,
 * executed). Without them a total over the threads falls back when one ends, and the frame meter's difference of two
 * totals went hugely negative.
 */
static u64 g_gone_objects, g_gone_bytes, g_gone_executed;

void mg_dvm_thread_costs(mg_dvm_costs *out)
{
    mg_dvm_thread *t = mg_dvm_self();
    out->executed = t->executed;
    out->waited_ns = t->waited_ns;
    out->native_ns = mg_ticks_ns(t->native_ticks);
    out->natives = t->natives;
    out->alloc_ns = mg_ticks_ns(t->alloc_ticks);
    out->allocs = t->allocs;
    out->blocked_ns = mg_ticks_ns(t->blocked_ticks);
}

u64 mg_dvm_executed_all(void)
{
    mg_dvm_thread *t;
    u64 n;
    mg_dvm_lock();
    n = g_gone_executed;
    for (t = g_threads; t; t = t->next) n += t->executed;
    mg_dvm_unlock();
    return n;
}

static bool g_stacks_trace;
static void stacks_thread_ends(mg_dvm_thread *t);

/* The calling thread has ended (the host's hook, mg_thread_set_end_hook): it is no longer one of the runtime's, to be stopped or scanned. */
static void thread_ends(void)
{
    mg_dvm_thread *t = t_self, **p;
    if (!t) return;
    if (g_stacks_trace) stacks_thread_ends(t);
    mg_dvm_lock();
    for (p = &g_threads; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    mg_dvm_gc_thread_gone(t);
    g_gone_objects += t->made_objects;
    g_gone_bytes += t->made_bytes;
    g_gone_executed += t->executed;
    mg_dvm_unlock();
    t_self = NULL;
    /* Its frame stack goes; the small rest stays, for whoever still holds its address (a Thread whose nativePeer was read a moment ago). */
    free(t->stack);
    t->stack = t->sp = t->stack_end = NULL;
}

void mg_dvm_threads_each(void (*fn)(mg_dvm_thread *t, void *user), void *user)
{
    mg_dvm_thread *t;
    for (t = g_threads; t; t = t->next) fn(t, user);
}

mg_dvm_thread *mg_dvm_self(void)
{
    mg_dvm_thread *t = t_self;
    void *lo = NULL, *hi = NULL;
    if (t) return t;
    t = (mg_dvm_thread *)mg_alloc(sizeof(*t));
    /* Where the host's stack ends, if the host knows and what it says holds for where this function stands now. */
    mg_thread_stack_bounds(&lo, &hi);
    if (lo && (mg_addr)&lo > (mg_addr)lo && (mg_addr)&lo < (mg_addr)hi) t->c_stack_low = (mg_addr)lo + 48u * 1024u;
    /* What the collector needs to stop it and scan its stack: the whole of it, from its real base, wherever it registered from. */
    t->host = mg_thread_current();
    if (lo && hi && (mg_addr)&lo > (mg_addr)lo && (mg_addr)&lo < (mg_addr)hi) {
        t->stack_lo = (mg_addr)lo;
        t->stack_hi = (mg_addr)hi;
    }
    mg_thread_set_end_hook(thread_ends);
    t->stack = (mg_dvm_reg *)mg_alloc(sizeof(mg_dvm_reg) * DVM_STACK_WORDS);
    t->stack_end = t->stack + DVM_STACK_WORDS - DVM_STACK_RESERVE;
    t->sp = t->stack;
    t->id = mg_thread_self_id();
    mg_dvm_lock();
    t->next = g_threads;
    g_threads = t;
    mg_dvm_unlock();
    t_self = t;
    return t;
}

/* --------------------------------------------------------------- allocation */

static mg_jobj *g_out_of_memory;       /* made at the start, for when nothing more can be made */

/* An allocation's bytes go to the collector's count in pieces of at least this much (mg_dvm_thread.gc_unsaid). */
#define GC_PIECE (32u * 1024u)

void mg_dvm_object_roots(void (*fn)(mg_jobj *o, void *user), void *user)
{
    if (g_out_of_memory) fn(g_out_of_memory, user);
    if (g_dvm.app_loader) fn(g_dvm.app_loader, user);
    mg_dvm_loader_roots(fn, user);
}

static mg_jobj *alloc_raw(mg_dvm_class *c, int kind, u64 body)
{
    mg_dvm_obj *o = NULL;
    size_t size = MG_DVM_HEADER + (size_t)body;
    mg_dvm_thread *self = mg_dvm_self();
    /*
     * The time it takes, for bytecode's own allocations (those made inside a native method are in that method's time). A
     * collection that runs here is timed whole. The heap's own work is timed for one allocation in 32, which counts for
     * 32: reading the clock twice costs more than an allocation does.
     */
    bool bytecode = self->native_depth == 0, sampled = bytecode && (self->allocs & 31) == 0, due = false;
    u64 started = 0;
    self->allocs++;
    /* What the collector will follow in objects of the class, known before the first of them exists (it may allocate nothing). */
    if (kind != MG_J_PRIM_ARRAY && kind != MG_J_OBJ_ARRAY && kind != MG_J_STRING && !c->ref_slots_ready) mg_dvm_gc_class_ready(c);
    /*
     * A collection, when enough has been made since the last one (mg_dvm_gc.c), before this one is. The thread tells the
     * collector what it made a piece at a time; a collection is then late by less than a piece a thread.
     */
    self->gc_unsaid += size > 0x7FFFFFFFu ? 0x7FFFFFFFu : (u32)size;
    if (self->gc_unsaid >= GC_PIECE) {
        u32 piece = self->gc_unsaid;
        self->gc_unsaid = 0;
        due = mg_dvm_gc_counted(piece);
    }
    if (due) {
        started = bytecode ? mg_time_ticks() : 0;
        mg_dvm_gc("allocation", NULL);
        if (started) self->alloc_ticks += mg_time_ticks() - started;
    }
    if (sampled) started = mg_time_ticks();
    if (body <= 0x7FFF0000u) o = (mg_dvm_obj *)mg_dvm_heap_alloc(size);
    if (sampled) self->alloc_ticks += (mg_time_ticks() - started) * 32;
    if (!o && body <= 0x7FFF0000u) {
        /* Nothing more to be had: a collection that clears soft references, and once more. */
        started = bytecode ? mg_time_ticks() : 0;
        mg_dvm_gc("out of memory", NULL);
        o = (mg_dvm_obj *)mg_dvm_heap_alloc(size);
        if (started) self->alloc_ticks += mg_time_ticks() - started;
    }
    if (!o) {
        if (!g_out_of_memory) mg_fatal("dvm: out of memory (%u bytes for a %s)", (unsigned)body, c->name);
        mg_dvm_throw_object(g_out_of_memory);
        return NULL;
    }
    if (g_dvm.profiling) {
        /* Who makes the objects: the innermost method of the thread, a native one included. */
        mg_dvm_thread *t = mg_dvm_self();
        if (t->top) {
            t->top->m->prof_objects++;
            t->top->m->prof_bytes += size;
        }
    }
    o->j.refs = 1;
    o->j.kind = (u8)kind;
    o->j.permanent = true;
    o->j.dvm = true;
    o->j.cls = c->jc;
    o->cls = c;
    o->size = (u32)size;
    self->made_objects++;
    self->made_bytes += size;
    if (c->finalizable || c->gc_ref) mg_dvm_gc_born(&o->j);
    return &o->j;
}

void mg_dvm_get_stats(mg_dvm_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->dex_files = mg_dvm_dex_count();
    out->classes_defined = mg_dvm_defined_count();
    out->classes_linked = (u32)g_dvm.n_linked;
    out->classes_initialised = (u32)g_dvm.n_initialised;
    out->strings_interned = (u32)g_dvm.n_interned;
    {
        /* Every thread's own counts, read without stopping it: each is a word or two only that thread writes. */
        mg_dvm_thread *t;
        u64 objects = g_gone_objects, bytes = g_gone_bytes;
        mg_dvm_lock();
        for (t = g_threads; t; t = t->next) {
            objects += t->made_objects;
            bytes += t->made_bytes;
        }
        mg_dvm_unlock();
        out->objects = objects;
        out->object_bytes = bytes;
    }
    out->instructions = g_dvm.steps;
    out->meta_bytes = (u64)(u32)g_dvm.meta_units * 16;
}

mg_jobj *mg_dvm_alloc_object(mg_dvm_class *c)
{
    mg_jobj *o;
    if (c->prim || c->name[0] == '[' || (c->flags & (DVM_ACC_INTERFACE | DVM_ACC_ABSTRACT)) || c == g_dvm.c_class) {
        mg_dvm_throw("java/lang/InstantiationError", "%s", c->name);
        return NULL;
    }
    if (c == g_dvm.c_string) return mg_dvm_alloc_string(NULL, 0);
    o = alloc_raw(c, MG_J_OBJECT, (u64)c->nslots * sizeof(mg_jvalue));
    return o;
}

static u32 element_size(char kind)
{
    switch (kind) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default: return (u32)sizeof(mg_jobj *);
    }
}

mg_jobj *mg_dvm_alloc_array(mg_dvm_class *array_class, i32 len)
{
    mg_jobj *a;
    u32 esz = element_size(array_class->elem);
    if (len < 0) {
        mg_dvm_throw("java/lang/NegativeArraySizeException", "%d", (int)len);
        return NULL;
    }
    a = alloc_raw(array_class, array_class->elem == 'L' ? MG_J_OBJ_ARRAY : MG_J_PRIM_ARRAY, (u64)(u32)len * esz);
    if (!a) return NULL;
    if (array_class->elem == 'L') {
        a->u.oarr.v = (mg_jobj **)(void *)mg_dvm_slots(a);
        a->u.oarr.len = (u32)len;
    } else {
        a->u.arr.data = mg_dvm_slots(a);
        a->u.arr.len = (u32)len;
        a->u.arr.esz = (u8)esz;
        a->u.arr.etype = array_class->elem;
    }
    return a;
}

mg_jobj *mg_dvm_new_array(const char *array_class, i32 len)
{
    mg_dvm_class *c = array_class[0] == '[' ? mg_dvm_find_class(array_class) : NULL;
    if (!c) {
        if (array_class[0] != '[') mg_dvm_throw("java/lang/IllegalArgumentException", "%s is not an array class", array_class);
        return NULL;
    }
    return mg_dvm_alloc_array(c, len);
}

/* ------------------------------------------------------------------ strings */

mg_jobj *mg_dvm_alloc_string(const u16 *chars, i32 len)
{
    mg_dvm_class *c = g_dvm.c_string ? g_dvm.c_string : mg_dvm_find_class("java/lang/String");
    mg_jobj *s;
    if (!c) return NULL;
    if (len < 0) len = 0;
    /*
     * The class's slots first (libcore's String has one, its hash), then the text. One unit more than the text, zero:
     * nothing here needs it, and a debugger or a careless reader is the better for it.
     */
    s = alloc_raw(c, MG_J_STRING, (u64)c->nslots * sizeof(mg_jvalue) + ((u64)(u32)len + 1) * 2);
    if (!s) return NULL;
    s->u.str.u16 = (u16 *)(void *)(mg_dvm_slots(s) + c->nslots);
    s->u.str.len16 = len;
    if (chars && len) memcpy(s->u.str.u16, chars, (size_t)len * 2);
    return s;
}

mg_jobj *mg_dvm_new_string_utf16(const u16 *chars, i32 len) { return mg_dvm_alloc_string(chars, len); }

/* Standard UTF-8 to UTF-16, four-byte sequences becoming surrogate pairs. `out` NULL: only counts. Bad bytes become U+FFFD. */
static i32 utf8_to_utf16(const char *s, u16 *out)
{
    const u8 *p = (const u8 *)s;
    i32 k = 0;
    while (*p) {
        u32 c = *p++;
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1, i;
        if (extra < 0) {
            if (out) out[k] = 0xFFFD;
            k++;
            continue;
        }
        if (extra) c &= (u32)(0x3F >> extra);
        for (i = 0; i < extra && (*p & 0xC0) == 0x80; i++) c = (c << 6) | (*p++ & 0x3F);
        if (c >= 0x10000 && c <= 0x10FFFF) {
            c -= 0x10000;
            if (out) {
                out[k] = (u16)(0xD800 | (c >> 10));
                out[k + 1] = (u16)(0xDC00 | (c & 0x3FF));
            }
            k += 2;
        } else {
            if (out) out[k] = c > 0xFFFF ? 0xFFFD : (u16)c;
            k++;
        }
    }
    return k;
}

mg_jobj *mg_dvm_new_string(const char *utf8)
{
    mg_jobj *s;
    if (!utf8) return NULL;
    s = mg_dvm_alloc_string(NULL, utf8_to_utf16(utf8, NULL));
    if (s) utf8_to_utf16(utf8, s->u.str.u16);
    return s;
}

const u16 *mg_dvm_string_utf16(mg_jobj *s, i32 *len)
{
    if (!s || s->kind != MG_J_STRING) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s->u.str.len16;
    return s->u.str.u16;
}

const char *mg_dvm_string_utf8(mg_jobj *s)
{
    const u16 *u;
    char *out;
    size_t k = 0;
    i32 n, i;
    if (!s || s->kind != MG_J_STRING) return NULL;
    if (s->u.str.utf8) return s->u.str.utf8;
    u = s->u.str.u16;
    n = s->u.str.len16;
    out = (char *)mg_alloc((size_t)n * 3 + 1);
    for (i = 0; i < n; i++) {
        u32 c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
            out[k++] = (char)(0xF0 | (c >> 18));
            out[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c == 0) {
            /* U+0000 as Java's modified UTF-8 writes it: the text is handed out as a C string and must not end here. */
            out[k++] = (char)0xC0;
            out[k++] = (char)0x80;
        } else if (c < 0x80) {
            out[k++] = (char)c;
        } else if (c < 0x800) {
            out[k++] = (char)(0xC0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[k++] = (char)(0xE0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[k] = 0;
    /* Two threads may both have made it: one copy is kept. */
    if (mg_atomic_cas_ptr((void *volatile *)(void *)&s->u.str.utf8, NULL, out) != NULL) free(out);
    return s->u.str.utf8;
}

typedef struct interned {
    mg_jobj *s;
    u32 hash;
    struct interned *next;
} interned;

#define INTERN_BUCKETS 4096
static interned *g_interned[INTERN_BUCKETS];

static u32 hash_utf16(const u16 *s, i32 n)
{
    u32 h = 0;
    i32 i;
    for (i = 0; i < n; i++) h = 31 * h + s[i];
    return h;
}

mg_jobj *mg_dvm_intern_string(mg_jobj *s)
{
    interned *e;
    u32 h;
    if (!s || s->kind != MG_J_STRING) return s;
    h = hash_utf16(s->u.str.u16, s->u.str.len16);
    mg_dvm_lock();
    for (e = g_interned[h % INTERN_BUCKETS]; e; e = e->next)
        if (e->hash == h && e->s->u.str.len16 == s->u.str.len16 && !memcmp(e->s->u.str.u16, s->u.str.u16, (size_t)s->u.str.len16 * 2)) break;
    if (!e) {
        e = (interned *)mg_alloc(sizeof(*e));
        e->s = s;
        e->hash = h;
        e->next = g_interned[h % INTERN_BUCKETS];
        g_interned[h % INTERN_BUCKETS] = e;
        mg_atomic_add(&g_dvm.n_interned, 1);
    }
    mg_dvm_unlock();
    return e->s;
}

void mg_dvm_interned_each(void (*fn)(mg_jobj *s, void *user), void *user)
{
    interned *e;
    size_t i;
    for (i = 0; i < INTERN_BUCKETS; i++)
        for (e = g_interned[i]; e; e = e->next) fn(e->s, user);
}

mg_jobj *mg_dvm_const_string(mg_dvm_dex *d, u32 idx)
{
    mg_jobj *s;
    const u8 *p;
    u16 *out;
    u32 len16, k = 0;
    if (idx >= d->nstr) {
        mg_dvm_throw("java/lang/VerifyError", "string index %u of %u in %s", (unsigned)idx, (unsigned)d->nstr, d->name);
        return NULL;
    }
    s = d->scache[idx];
    if (s) return s;
    p = (const u8 *)mg_dvm_dex_string(d, idx, &len16);
    s = mg_dvm_alloc_string(NULL, (i32)len16);
    if (!s) return NULL;
    out = s->u.str.u16;
    /* A DEX file's strings are modified UTF-8: one to three bytes a UTF-16 unit, surrogates each on their own, U+0000 as two bytes. */
    while (*p && k < len16) {
        u32 c = *p++;
        if (c < 0x80) out[k++] = (u16)c;
        else if ((c & 0xE0) == 0xC0) {
            out[k++] = (u16)(((c & 0x1F) << 6) | (*p & 0x3F));
            if (*p) p++;
        } else if ((c & 0xF0) == 0xE0) {
            u32 c2 = *p ? *p++ : 0, c3 = *p ? *p++ : 0;
            out[k++] = (u16)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
        } else out[k++] = 0xFFFD;
    }
    s->u.str.len16 = (i32)k;
    s = mg_dvm_intern_string(s);
    d->scache[idx] = s;
    return s;
}

bool mg_dvm_arraycopy(mg_jobj *src, i32 sp, mg_jobj *dst, i32 dp, i32 n)
{
    i32 i;
    if (!src || !dst) {
        return mg_dvm_throw("java/lang/NullPointerException", "%s", src ? "dst == null" : "src == null");
    }
    if ((src->kind != MG_J_PRIM_ARRAY && src->kind != MG_J_OBJ_ARRAY) || src->kind != dst->kind
        || (src->kind == MG_J_PRIM_ARRAY && src->u.arr.etype != dst->u.arr.etype)) {
        return mg_dvm_throw("java/lang/ArrayStoreException", "arraycopy from a %s to a %s", mg_dvm_class_of(src)->name, mg_dvm_class_of(dst)->name);
    }
    if (sp < 0 || dp < 0 || n < 0 || sp > mg_dvm_array_length(src) - n || dp > mg_dvm_array_length(dst) - n) {
        return mg_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d", (int)mg_dvm_array_length(src),
                            (int)sp, (int)mg_dvm_array_length(dst), (int)dp, (int)n);
    }
    if (src->kind == MG_J_PRIM_ARRAY) {
        memmove((u8 *)dst->u.arr.data + (size_t)dp * dst->u.arr.esz, (const u8 *)src->u.arr.data + (size_t)sp * src->u.arr.esz, (size_t)n * src->u.arr.esz);
        return true;
    }
    /* References into an array of a narrower class are checked one by one, and what was stored before the first that does not fit stays. */
    if (!mg_dvm_assignable(mg_dvm_class_of(src)->component, mg_dvm_class_of(dst)->component)) {
        for (i = 0; i < n; i++) {
            mg_jobj *o = src->u.oarr.v[sp + i];
            if (o && !mg_dvm_is_instance(o, mg_dvm_class_of(dst)->component)) {
                return mg_dvm_throw("java/lang/ArrayStoreException", "%s cannot be stored in an array of type %s", mg_dvm_class_of(o)->name, mg_dvm_class_of(dst)->name);
            }
            dst->u.oarr.v[dp + i] = o;
        }
        return true;
    }
    memmove(dst->u.oarr.v + dp, src->u.oarr.v + sp, sizeof(mg_jobj *) * (size_t)n);
    return true;
}

/* ------------------------------------------------------------ Class objects */

mg_jobj *mg_dvm_mirror(mg_dvm_class *c)
{
    mg_jobj *m = c->mirror;
    mg_dvm_class *cc;
    if (m) return m;
    cc = g_dvm.c_class ? g_dvm.c_class : mg_dvm_find_class("java/lang/Class");
    if (!cc) return NULL;
    m = alloc_raw(cc, MG_J_CLASS, (u64)cc->nslots * sizeof(mg_jvalue));
    if (!m) return NULL;
    m->native = c;
    m->u.klass.jc = c->jc;
    /*
     * Filled before it is published: a mirror another thread finds must be whole. It was published first and filled after,
     * and a thread that asked for the same class meanwhile had one without its component type, superclass or interfaces --
     * Sberbank's coroutines copying an array (Arrays.copyOf) met a null component type so. Made twice by two threads at
     * once, the first one published stays, the other is an object nothing refers to. (Until then `m` is held by this C
     * stack, which the collector scans.)
     */
    if (g_dvm.fill_mirror) g_dvm.fill_mirror(m, c);
    mg_atomic_cas_ptr((void *volatile *)(void *)&c->mirror, NULL, m);
    return c->mirror;
}

u32 mg_dvm_identity_hash(const mg_jobj *o)
{
    /* Objects do not move, so an object's address is its identity. */
    u64 a = (u64)(mg_addr)o >> 3;
    return (u32)((a ^ (a >> 29)) * 2654435761u);
}

/* ----------------------------------------------------------------- monitors */

/*
 * A Java monitor: a lock with its owner and how many times the owner holds
 * it, made the first time an object is locked. wait() lets go of every level
 * and takes them all back, as Java's does.
 */
typedef struct monitor {
    mg_lock mu;
    mg_cond cv;
    volatile u32 owner;
    int count;
    u32 notified;                      /* counts notify and notifyAll, under mu: how a waiter tells being woken from waking by itself */
} monitor;

static bool g_monitor_trace;
static u32 g_monitor_trace_main;

typedef struct monitor_total {
    u32 object, owner, count;
    const char *cls;
    mg_dvm_method *method;
    u64 ns, max_ns;
} monitor_total;
static monitor_total g_monitor_totals[64];
static int g_monitor_total_count;

/* Written only by the watched main thread, copied while it is suspended.
 * Metadata and numeric addresses only: no retained application objects. */
static void monitor_count_wait(mg_jobj *o, u32 owner, u64 ns, mg_dvm_thread *self)
{
    const char *cls = o->kind == MG_J_CLASS && o->native ? ((mg_dvm_class *)o->native)->name : mg_dvm_class_of(o)->name;
    monitor_total *s;
    int i;
    for (i = 0; i < g_monitor_total_count; ++i)
        if (g_monitor_totals[i].object == (u32)(mg_addr)o && g_monitor_totals[i].cls == cls) break;
    if (i == 64) return;
    if (i == g_monitor_total_count) ++g_monitor_total_count;
    s = &g_monitor_totals[i];
    s->object = (u32)(mg_addr)o;
    s->cls = cls;
    s->owner = owner;
    s->method = self->top ? self->top->m : NULL;
    ++s->count;
    s->ns += ns;
    if (ns > s->max_ns) s->max_ns = ns;
}

typedef struct monitor_sample {
    u32 thread, owner, object;
    u64 waiting_ns;
    const char *object_class;
    char name[80];
    mg_dvm_method *methods[32];
    u32 pcs[32];
    int frames;
} monitor_sample;

/* Copy only while stopped: no allocation, logging or Java calls until every
 * thread has resumed. The VM lock excludes GC and thread-stack destruction. */
static void monitor_sample_copy(mg_dvm_thread *t, monitor_sample *s)
{
    mg_dvm_frame *f;
    mg_dvm_class *cls;
    mg_jobj *o = t->waiting_monitor;
    s->thread = t->id;
    if (o) {
        monitor *m = (monitor *)MG_DVM_OBJ(o)->monitor;
        s->object = (u32)(mg_addr)o;
        s->object_class = o->kind == MG_J_CLASS && o->native ? ((mg_dvm_class *)o->native)->name : mg_dvm_class_of(o)->name;
        s->owner = m ? m->owner : 0;
        s->waiting_ns = mg_time_monotonic_ns() - t->monitor_wait_ns;
    }
    for (cls = t->peer ? mg_dvm_class_of(t->peer) : NULL; cls; cls = cls->super) {
        int i;
        for (i = 0; i < cls->nifields; ++i) {
            mg_dvm_field *field = &cls->ifields[i];
            if (!strcmp(field->name, "name") && !strcmp(field->type, "Ljava/lang/String;")) {
                mg_jobj *name = mg_dvm_slots(t->peer)[field->slot].l;
                if (name && name->kind == MG_J_STRING) {
                    int j, n = name->u.str.len16;
                    if (n >= (int)sizeof(s->name)) n = (int)sizeof(s->name) - 1;
                    for (j = 0; j < n; ++j) s->name[j] = name->u.str.u16[j] < 128 ? (char)name->u.str.u16[j] : '?';
                }
                break;
            }
        }
    }
    for (f = t->top; f && s->frames < 32; f = f->prev) {
        if ((mg_addr)f < (mg_addr)t->stack || (mg_addr)f + sizeof(*f) > (mg_addr)(t->stack + DVM_STACK_WORDS) || !f->m) break;
        s->methods[s->frames] = f->m;
        s->pcs[s->frames++] = f->pc;
    }
}

static void monitor_observer(void *unused)
{
    u64 last = 0, last_totals = 0;
    (void)unused;
    while (g_dvm.running) {
        monitor_sample samples[4];
        monitor_total totals[64];
        mg_dvm_thread *stopped[4], *t;
        mg_thread_regs regs;
        u32 wanted = g_monitor_trace_main;
        int count = 0, ntotals = 0, i, j;
        u64 now;
        mg_sleep_ns(100000000ull);
        now = mg_time_monotonic_ns();
        if (now - last < 2000000000ull) continue;
        memset(samples, 0, sizeof(samples));
        mg_dvm_lock();
        for (t = g_threads; t && t->id != wanted; t = t->next) {}
        if (t && (t->waiting_monitor || now - last_totals >= 5000000000ull) && t->host && mg_thread_suspend(t->host, &regs)) {
            stopped[count++] = t;
            if (now - last_totals >= 5000000000ull) {
                ntotals = g_monitor_total_count;
                memcpy(totals, g_monitor_totals, sizeof(totals));
                last_totals = now;
            }
            if (t->waiting_monitor && now >= t->monitor_wait_ns && now - t->monitor_wait_ns >= 1000000ull) {
                monitor_sample_copy(t, &samples[0]);
                wanted = samples[0].owner;
                while (wanted && count < 4) {
                    bool repeated = false;
                    for (i = 0; i < count; ++i) if (stopped[i]->id == wanted) repeated = true;
                    if (repeated) break;
                    for (t = g_threads; t && t->id != wanted; t = t->next) {}
                    if (!t || !t->host || !mg_thread_suspend(t->host, &regs)) break;
                    /* Ownership can change between observing its id and
                     * stopping that thread. Do not mislabel a former owner. */
                    {
                        mg_jobj *lock = stopped[count - 1]->waiting_monitor;
                        monitor *held = lock ? (monitor *)MG_DVM_OBJ(lock)->monitor : NULL;
                        if (!held || held->owner != t->id) {
                            samples[count - 1].owner = held ? held->owner : 0;
                            mg_thread_resume(t->host);
                            break;
                        }
                    }
                    stopped[count] = t;
                    monitor_sample_copy(t, &samples[count]);
                    wanted = samples[count++].owner;
                }
            }
            for (i = count - 1; i >= 0; --i) mg_thread_resume(stopped[i]->host);
        }
        mg_dvm_unlock();
        for (i = 0; i < 4 && i < ntotals; ++i) {
            int best = i;
            monitor_total tmp, *s;
            for (j = i + 1; j < ntotals; ++j) if (totals[j].ns > totals[best].ns) best = j;
            tmp = totals[i]; totals[i] = totals[best]; totals[best] = tmp;
            s = &totals[i];
            if (s->ns < 10000000ull) continue;
            mg_warn("monitor-total: main=%u object=%08x class=%s waits=%u total=%.1f ms max=%.1f ms last-owner=%u site=%s.%s",
                    g_monitor_trace_main, s->object, s->cls, s->count, (double)s->ns / 1e6, (double)s->max_ns / 1e6, s->owner,
                    s->method ? s->method->cls->name : "?", s->method ? s->method->name : "?");
        }
        if (!samples[0].object) continue;
        last = now;
        for (i = 0; i < count; ++i) {
            monitor_sample *s = &samples[i];
            mg_warn("monitor-trace: %s tid=%u name=%s waiting=%.1f ms object=%08x class=%s owner=%u",
                    i ? "owner" : "MAIN", s->thread, s->name, (double)s->waiting_ns / 1e6,
                    s->object, s->object_class ? s->object_class : "(not entering a monitor)", s->owner);
            for (j = 0; j < s->frames; ++j) {
                mg_dvm_method *m = s->methods[j];
                mg_warn("monitor-trace: tid=%u #%d %s.%s%s pc=%u line=%d", s->thread, j,
                        m->cls->name, m->name, m->sig, s->pcs[j], mg_dvm_dex_line(m, s->pcs[j]));
            }
        }
    }
}

void mg_dvm_monitor_trace_start(void)
{
    mg_thread *observer;
    if (g_monitor_trace) return;
    g_monitor_trace_main = mg_dvm_self()->id;
    g_monitor_trace = true;
    observer = mg_thread_start(monitor_observer, NULL, 128u * 1024u, "monitor-observer");
    if (observer) {
        mg_thread_detach(observer);
        mg_info("monitor-trace: watching Java main tid=%u; waits >=1 ms, at most one sample per 2 s; object totals every 5 s", g_monitor_trace_main);
    } else {
        g_monitor_trace = false;
        mg_warn("monitor-trace: observer could not start");
    }
}

/*
 * The flag trace-stacks (mg_java.c starts it): five seconds after the app's main thread starts and every two seconds after,
 * the Java stack of every thread whose stack changed since the round before is said in the log, its 24 top frames; the
 * others are counted in one line. The first round says all of them. A thread that ends is said as it ends (thread_ends),
 * and one that could not be stopped is named. That says where an app that stands still with nothing in the log is waiting,
 * and which threads came and went in between (VK mp3 mod's sign-in stopped after its anonymous token, with every thread
 * idle ten seconds later). Each thread is stopped only while its frames are copied, under the VM lock, as for the monitor
 * trace. After the app was suspended the rounds go on from when it resumed. At most 6000 lines a run.
 */
#define STACKS_THREADS 64
#define STACKS_FRAMES 24

static volatile i32 g_stacks_lines;

static bool stacks_line_left(void)
{
    return mg_atomic_add(&g_stacks_lines, 1) <= 6000;
}

static bool same_stack(const monitor_sample *a, const monitor_sample *b)
{
    int i;
    if (a->thread != b->thread || a->frames != b->frames) return false;
    for (i = 0; i < a->frames; ++i)
        if (a->methods[i] != b->methods[i] || a->pcs[i] != b->pcs[i]) return false;
    return true;
}

/* A thread that ends, said by itself on its way out (thread_ends), with the frames it still has. */
static void stacks_thread_ends(mg_dvm_thread *t)
{
    monitor_sample s;
    memset(&s, 0, sizeof(s));
    monitor_sample_copy(t, &s);
    if (stacks_line_left()) mg_info("stacks: thread %u \"%s\" ends, %d Java frames left", s.thread, s.name, s.frames);
}

static void stacks_observer(void *unused)
{
    static monitor_sample now[STACKS_THREADS], before[STACKS_THREADS];
    int nbefore = 0, round = 1;
    u64 next = mg_time_monotonic_ns() + 5000000000ull, last = 0;
    (void)unused;
    while (g_dvm.running && mg_atomic_add(&g_stacks_lines, 0) < 6000) {
        mg_dvm_thread *t;
        mg_thread_regs regs;
        u32 missed[8];
        int count = 0, more = 0, nmissed = 0, same = 0, i, j, k;
        char missing[96];
        u64 at;
        mg_sleep_ns(100000000ull);
        at = mg_time_monotonic_ns();
        /* A sleep far longer than asked for: the app was suspended. The rounds go on from now, not to make up for it. */
        if (last && at - last > 1000000000ull && next < at) next = at;
        last = at;
        if (at < next) continue;
        next = at + 2000000000ull;
        memset(now, 0, sizeof(now));
        mg_dvm_lock();
        for (t = g_threads; t; t = t->next) {
            if (count == STACKS_THREADS) {
                ++more;
                continue;
            }
            if (!t->host || !mg_thread_suspend(t->host, &regs)) {
                if (nmissed < (int)MG_COUNT(missed)) missed[nmissed++] = t->id;
                continue;
            }
            monitor_sample_copy(t, &now[count++]);
            mg_thread_resume(t->host);
        }
        mg_dvm_unlock();
        for (i = 0; i < count; ++i) {
            for (k = 0; k < nbefore && !same_stack(&now[i], &before[k]); ++k) {}
            if (k < nbefore) ++same;
        }
        missing[0] = 0;
        for (i = 0, k = 0; i < nmissed && k < (int)sizeof(missing) - 16; ++i)
            k += snprintf(missing + k, sizeof(missing) - (size_t)k, "%s%u", i ? ", " : "; not stopped: ", (unsigned)missed[i]);
        if (!stacks_line_left()) break;
        mg_info("stacks: round %d, %d Java threads, %d as two seconds before%s%s", round, count + nmissed + more, same, more ? " (and more not said)" : "",
                missing);
        for (i = 0; i < count; ++i) {
            monitor_sample *s = &now[i];
            for (k = 0; k < nbefore && !same_stack(s, &before[k]); ++k) {}
            if (k < nbefore) continue;
            if (!stacks_line_left()) break;
            if (s->object)
                mg_info("stacks: thread %u \"%s\", waiting %.1f s for the monitor of a %s that thread %u holds:", s->thread, s->name,
                        (double)s->waiting_ns / 1e9, s->object_class ? s->object_class : "?", s->owner);
            else
                mg_info("stacks: thread %u \"%s\"%s", s->thread, s->name, s->frames ? ":" : ": no Java frames");
            for (j = 0; j < s->frames && j < STACKS_FRAMES && stacks_line_left(); ++j) {
                mg_dvm_method *m = s->methods[j];
                int line = mg_dvm_dex_line(m, s->pcs[j]);
                if (m->flags & DVM_ACC_NATIVE) mg_info("stacks:   %s.%s (native)", m->cls->name, m->name);
                else if (line >= 0) mg_info("stacks:   %s.%s line %d", m->cls->name, m->name, line);
                else mg_info("stacks:   %s.%s at %u", m->cls->name, m->name, s->pcs[j]);
            }
        }
        memcpy(before, now, sizeof(before));
        nbefore = count;
        ++round;
    }
}

void mg_dvm_stacks_trace_start(void)
{
    mg_thread *observer;
    if (g_stacks_trace) return;
    g_stacks_trace = true;
    observer = mg_thread_start(stacks_observer, NULL, 64u * 1024u, "stacks-observer");
    if (observer) {
        mg_thread_detach(observer);
        mg_info("stacks: every Java thread's stack in 5 s, then those that changed every 2 s, and threads as they end (the flag trace-stacks)");
    } else {
        mg_warn("stacks: the observer could not start");
    }
}

void mg_dvm_object_dying(void *p)
{
    mg_dvm_obj *o = (mg_dvm_obj *)p;
    int i;
    if (o->monitor) free(o->monitor);
    if (o->j.kind == MG_J_STRING && o->j.u.str.utf8) free(o->j.u.str.utf8);
    /* A string AllocObject made and a JNI call of a String constructor filled keeps its text in a buffer of its own (mg_jni.c). */
    if (o->j.kind == MG_J_STRING && o->j.native) free(o->j.native);
    if (o->j.fields) {
        for (i = 0; i < o->j.nfields; i++) free(o->j.fields[i].name);
        free(o->j.fields);
    }
}

static monitor *monitor_of(mg_jobj *o)
{
    mg_dvm_obj *d = MG_DVM_OBJ(o);
    monitor *m = (monitor *)d->monitor;
    if (m) return m;
    m = (monitor *)mg_alloc(sizeof(*m));
    mg_lock_init(&m->mu);
    mg_cond_init(&m->cv);
    if (mg_atomic_cas_ptr(&d->monitor, NULL, m) != NULL) free(m);
    return (monitor *)d->monitor;
}

void mg_dvm_monitor_enter(mg_jobj *o)
{
    monitor *m = monitor_of(o);
    u32 me = mg_thread_self_id();
    if (m->owner == me) {
        m->count++;
        return;
    }
    if (!mg_lock_try(&m->mu)) {
        /* Another thread has it: the wait, counted for bytecode's own monitor-enter (inside a native method it is the method's). */
        mg_dvm_thread *self = mg_dvm_self();
        u64 started = self->native_depth ? 0 : mg_time_ticks();
        u32 observed_owner = m->owner;
        if (g_monitor_trace) {
            self->monitor_wait_ns = mg_time_monotonic_ns();
            self->waiting_monitor = o;
        }
        mg_lock_acquire(&m->mu);
        self->waiting_monitor = NULL;
        if (g_monitor_trace && me == g_monitor_trace_main)
            monitor_count_wait(o, observed_owner, mg_time_monotonic_ns() - self->monitor_wait_ns, self);
        if (started) self->blocked_ticks += mg_time_ticks() - started;
    }
    m->owner = me;
    m->count = 1;
}

bool mg_dvm_monitor_exit(mg_jobj *o)
{
    monitor *m = (monitor *)MG_DVM_OBJ(o)->monitor;
    if (!m || m->owner != mg_thread_self_id()) return false;
    if (--m->count == 0) {
        m->owner = 0;
        mg_lock_release(&m->mu);
    }
    return true;
}

bool mg_dvm_monitor_wait(mg_jobj *o, i64 ms)
{
    monitor *m = (monitor *)MG_DVM_OBJ(o)->monitor;
    u32 me = mg_thread_self_id();
    int held;
    if (!m || m->owner != me) return false;
    held = m->count;
    m->count = 0;
    m->owner = 0;
    if (g_dvm.step_limit) {
        /*
         * Under a step limit nothing may wait: the wait ends at once, as Java allows any wait to (a spurious wakeup), and code
         * that waits in a loop then uses its steps up. Others get their turn at the monitor meanwhile.
         */
        mg_lock_release(&m->mu);
        mg_thread_yield();
        mg_lock_acquire(&m->mu);
    } else {
        /*
         * Until notified, or until the time is up by the clock: the host's wait may end early by itself, and on Windows a
         * timed one ends at a tick of the system timer, which can be most of the time asked for too soon. Java allows a
         * wait to end for no reason, but code that sleeps by waiting should get the time it asked for.
         */
        u32 seen = m->notified;
        u64 started = mg_time_monotonic_ns(), deadline = ms > 0 ? started + (u64)ms * 1000000ull : 0;
        mg_dvm_thread *self = mg_dvm_self();
        while (m->notified == seen) {
            u64 now;
            if (!deadline) {
                mg_cond_wait(&m->cv, &m->mu);
                continue;
            }
            now = mg_time_monotonic_ns();
            if (now >= deadline) break;
            mg_cond_wait_ns(&m->cv, &m->mu, deadline - now);
        }
        self->waited_ns += mg_time_monotonic_ns() - started;
    }
    m->owner = me;
    m->count = held;
    return true;
}

bool mg_dvm_monitor_notify(mg_jobj *o, bool all)
{
    monitor *m = (monitor *)MG_DVM_OBJ(o)->monitor;
    if (!m || m->owner != mg_thread_self_id()) return false;
    m->notified++;
    if (all) mg_cond_broadcast(&m->cv);
    else mg_cond_signal(&m->cv);
    return true;
}

/* --------------------------------------------------------------- exceptions */

void mg_dvm_throw_object(mg_jobj *e) { mg_dvm_self()->pending = e; }
mg_jobj *mg_dvm_pending(void) { return mg_dvm_self()->pending; }
void mg_dvm_clear_pending(void) { mg_dvm_self()->pending = NULL; }

bool mg_dvm_throw(const char *cls, const char *fmt, ...)
{
    mg_dvm_thread *t = mg_dvm_self();
    mg_dvm_class *c;
    mg_dvm_method *ctor;
    mg_jobj *e;
    mg_jvalue arg, ignored;
    char msg[600];
    msg[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
    }
    if (g_dvm.trace >= 1) mg_info("dvm: throw %s: %s", cls, msg);
    if (!mg_dvm_class_exists(cls)) {
        /* The boot class path has no such exception: say that instead, with what was meant. */
        if (!strcmp(cls, "java/lang/NoClassDefFoundError")) mg_fatal("dvm: the boot class path has no java.lang.NoClassDefFoundError to throw (%s)", msg);
        return mg_dvm_throw("java/lang/NoClassDefFoundError", "%s (to throw it: %s)", cls, msg);
    }
    t->pending = NULL;
    c = mg_dvm_find_class(cls);
    if (!c || !mg_dvm_ensure_init(c)) return false;             /* what stopped it is pending instead */
    e = mg_dvm_alloc_object(c);
    if (!e) return false;
    arg.j = 0;
    ctor = mg_dvm_find_method(c, "<init>", "(Ljava/lang/String;)V", false);
    /*
     * Whatever happens from here on, something is pending when this returns: what stopped the constructor if it threw, and
     * otherwise the exception, even one whose constructor could not be run at all (the stack was full).
     */
    if (ctor) {
        arg.l = fmt ? mg_dvm_new_string(msg) : NULL;
        if (!mg_dvm_invoke(ctor, e, &arg, &ignored) && t->pending) return false;
    } else {
        ctor = mg_dvm_find_method(c, "<init>", "()V", false);
        if (ctor && !mg_dvm_invoke(ctor, e, NULL, &ignored) && t->pending) return false;
        if (fmt && g_dvm.f_message && mg_dvm_is_instance(e, g_dvm.c_throwable)) mg_dvm_slots(e)[g_dvm.f_message->slot].l = mg_dvm_new_string(msg);
    }
    t->pending = e;
    return false;
}

void mg_dvm_objects_start(void)
{
    mg_jvalue arg, ignored;
    mg_dvm_class *c = mg_dvm_class_exists("java/lang/OutOfMemoryError") ? mg_dvm_find_class("java/lang/OutOfMemoryError") : NULL;
    mg_dvm_method *ctor = c ? mg_dvm_find_method(c, "<init>", "(Ljava/lang/String;)V", false) : NULL;
    mg_jobj *e = c && mg_dvm_ensure_init(c) ? mg_dvm_alloc_object(c) : NULL;
    arg.j = 0;
    if (e && ctor) {
        arg.l = mg_dvm_new_string("the heap is exhausted");
        if (mg_dvm_invoke(ctor, e, &arg, &ignored)) g_out_of_memory = e;
    }
    mg_dvm_clear_pending();
}

static const char *message_of(mg_jobj *e)
{
    mg_jobj *m;
    if (!g_dvm.f_message || !mg_dvm_is_instance(e, g_dvm.c_throwable)) return NULL;
    m = mg_dvm_slots(e)[g_dvm.f_message->slot].l;
    return mg_dvm_string_utf8(m);
}

static size_t append(char *buf, size_t n, size_t k, const char *fmt, ...) MG_PRINTF(4, 5);
static size_t append(char *buf, size_t n, size_t k, const char *fmt, ...)
{
    va_list ap;
    int w;
    if (k + 1 >= n) return k;
    va_start(ap, fmt);
    w = vsnprintf(buf + k, n - k, fmt, ap);
    va_end(ap);
    if (w < 0) return k;
    return k + (size_t)w < n ? k + (size_t)w : n - 1;
}

static size_t append_dotted(char *buf, size_t n, size_t k, const char *name)
{
    size_t start = k;
    k = append(buf, n, k, "%s", name);
    for (; start < k; start++)
        if (buf[start] == '/') buf[start] = '.';
    return k;
}

const char *mg_dvm_describe_pending(char *buf, size_t n)
{
    mg_jobj *e = mg_dvm_self()->pending;
    const char *msg;
    size_t k = 0;
    int depth;
    if (!e || !n) return NULL;
    buf[0] = 0;
    k = append_dotted(buf, n, k, mg_dvm_class_of(e)->name);
    msg = message_of(e);
    if (msg) k = append(buf, n, k, ": %s", msg);
    for (depth = 0; depth < 4 && g_dvm.f_cause && mg_dvm_is_instance(e, g_dvm.c_throwable); depth++) {
        mg_jobj *cause = mg_dvm_slots(e)[g_dvm.f_cause->slot].l;
        if (!cause || cause == e) break;
        k = append(buf, n, k, " | caused by ");
        k = append_dotted(buf, n, k, mg_dvm_class_of(cause)->name);
        msg = message_of(cause);
        if (msg) k = append(buf, n, k, ": %s", msg);
        e = cause;
    }
    return buf;
}

mg_jobj *mg_dvm_capture_stack(void)
{
    mg_dvm_thread *t = mg_dvm_self();
    mg_dvm_frame *f = t->top, *k;
    mg_dvm_class *longs;
    mg_jobj *a;
    i64 *out;
    i32 n = 0, i = 0;
    /* Not the frames that are the exception being made: its constructors and fillInStackTrace itself. */
    while (f && g_dvm.c_throwable && mg_dvm_assignable(f->m->cls, g_dvm.c_throwable)
           && (f->m->name[0] == '<' || !strcmp(f->m->name, "fillInStackTrace") || !strcmp(f->m->name, "nativeFillInStackTrace")))
        f = f->prev;
    for (k = f; k; k = k->prev) n++;
    if (n > 1024) n = 1024;
    longs = mg_dvm_find_class("[J");
    a = longs ? mg_dvm_alloc_array(longs, n * 2) : NULL;
    if (!a) return NULL;
    out = (i64 *)a->u.arr.data;
    for (k = f; k && i < n; k = k->prev, i++) {
        out[2 * i] = (i64)(mg_addr)k->m;
        out[2 * i + 1] = k->m->insns ? (i64)k->pc : -1;
    }
    return a;
}

int mg_dvm_pending_stack(char *buf, size_t n)
{
    mg_jobj *e = mg_dvm_self()->pending, *trace;
    const i64 *v;
    size_t k = 0;
    i32 frames, i;
    if (n) buf[0] = 0;
    if (!e || !n || !g_dvm.f_backtrace || !mg_dvm_is_instance(e, g_dvm.c_throwable)) return 0;
    trace = mg_dvm_slots(e)[g_dvm.f_backtrace->slot].l;
    if (!trace || trace->kind != MG_J_PRIM_ARRAY || trace->u.arr.etype != 'J') return 0;
    v = (const i64 *)trace->u.arr.data;
    frames = (i32)(trace->u.arr.len / 2);
    for (i = 0; i < frames; i++) {
        const mg_dvm_method *m = (const mg_dvm_method *)(mg_addr)v[2 * i];
        const char *source = mg_dvm_dex_source(m->cls);
        int line = v[2 * i + 1] >= 0 ? mg_dvm_dex_line(m, (u32)v[2 * i + 1]) : -1;
        k = append(buf, n, k, "  at ");
        k = append_dotted(buf, n, k, m->cls->name);
        if (m->flags & DVM_ACC_NATIVE) k = append(buf, n, k, ".%s(native)\n", m->name);
        else if (line >= 0) k = append(buf, n, k, ".%s(%s:%d)\n", m->name, source ? source : "?", line);
        else k = append(buf, n, k, ".%s(pc %d)\n", m->name, (int)v[2 * i + 1]);
    }
    return (int)frames;
}
