/*
 * Objects, arrays, strings and Class objects; monitors; the calling thread's
 * state; making and describing exceptions.
 *
 * Nothing is freed yet. An object is one zeroed allocation from the runtime's
 * heap (hk_dvm_heap.c) that lives as long as the process; docs/JAVA.md has the
 * collector's design, and everything here goes through alloc_raw.
 */
#include "hk_dvm_internal.h"

#include <stdio.h>

/* ------------------------------------------------------------------ threads */

static HK_THREAD_LOCAL hk_dvm_thread *t_self;
static hk_dvm_thread *g_threads;

void hk_dvm_thread_costs(hk_dvm_costs *out)
{
    hk_dvm_thread *t = hk_dvm_self();
    out->executed = t->executed;
    out->waited_ns = t->waited_ns;
    out->native_ns = t->native_ns;
    out->natives = t->natives;
    out->alloc_ns = t->alloc_ns;
    out->allocs = t->allocs;
    out->blocked_ns = t->blocked_ns;
}

u64 hk_dvm_executed_all(void)
{
    hk_dvm_thread *t;
    u64 n = 0;
    hk_dvm_lock();
    for (t = g_threads; t; t = t->next) n += t->executed;
    hk_dvm_unlock();
    return n;
}

hk_dvm_thread *hk_dvm_self(void)
{
    hk_dvm_thread *t = t_self;
    void *lo = NULL, *hi = NULL;
    if (t) return t;
    t = (hk_dvm_thread *)hk_alloc(sizeof(*t));
    /* Where the host's stack ends, if the host knows and what it says holds for where this function stands now. */
    hk_thread_stack_bounds(&lo, &hi);
    if (lo && (hk_addr)&lo > (hk_addr)lo && (hk_addr)&lo < (hk_addr)hi) t->c_stack_low = (hk_addr)lo + 48u * 1024u;
    t->stack = (hk_dvm_reg *)hk_alloc(sizeof(hk_dvm_reg) * DVM_STACK_WORDS);
    t->stack_end = t->stack + DVM_STACK_WORDS - DVM_STACK_RESERVE;
    t->sp = t->stack;
    t->id = hk_thread_self_id();
    hk_dvm_lock();
    t->next = g_threads;
    g_threads = t;
    hk_dvm_unlock();
    t_self = t;
    return t;
}

/* --------------------------------------------------------------- allocation */

static hk_jobj *g_out_of_memory;       /* made at the start, for when nothing more can be made */
static volatile i32 g_objects;
static volatile i32 g_object_kb, g_object_bytes_low;

static hk_jobj *alloc_raw(hk_dvm_class *c, int kind, u64 body)
{
    hk_dvm_obj *o = NULL;
    size_t size = HK_DVM_HEADER + (size_t)body;
    i32 low;
    hk_dvm_thread *self = hk_dvm_self();
    /* The time it takes, for bytecode's own allocations: those made inside a native method are in that method's time. */
    u64 started = self->native_depth ? 0 : hk_time_monotonic_ns();
    self->allocs++;
    if (body <= 0x7FFF0000u) o = (hk_dvm_obj *)hk_dvm_heap_alloc(size);
    if (started) self->alloc_ns += hk_time_monotonic_ns() - started;
    if (!o) {
        if (!g_out_of_memory) hk_fatal("dvm: out of memory (%u bytes for a %s)", (unsigned)body, c->name);
        hk_dvm_throw_object(g_out_of_memory);
        return NULL;
    }
    if (g_dvm.profiling) {
        /* Who makes the objects: the innermost method of the thread, a native one included. */
        hk_dvm_thread *t = hk_dvm_self();
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
    hk_atomic_add(&g_objects, 1);
    /* Bytes as kilobytes and a remainder: two 32-bit counters that atomic adds can keep, where one would wrap at 2 GB. */
    low = hk_atomic_add(&g_object_bytes_low, (i32)(size & 1023));
    if (low >= 1024) {
        hk_atomic_add(&g_object_bytes_low, -1024);
        hk_atomic_add(&g_object_kb, 1);
    }
    hk_atomic_add(&g_object_kb, (i32)(size >> 10));
    return &o->j;
}

void hk_dvm_get_stats(hk_dvm_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->dex_files = hk_dvm_dex_count();
    out->classes_defined = hk_dvm_defined_count();
    out->classes_linked = (u32)g_dvm.n_linked;
    out->classes_initialised = (u32)g_dvm.n_initialised;
    out->strings_interned = (u32)g_dvm.n_interned;
    out->objects = (u32)g_objects;
    out->object_bytes = (u64)(u32)g_object_kb * 1024 + (u64)(u32)g_object_bytes_low;
    out->instructions = g_dvm.steps;
    out->meta_bytes = (u64)(u32)g_dvm.meta_units * 16;
}

hk_jobj *hk_dvm_alloc_object(hk_dvm_class *c)
{
    hk_jobj *o;
    if (c->prim || c->name[0] == '[' || (c->flags & (DVM_ACC_INTERFACE | DVM_ACC_ABSTRACT)) || c == g_dvm.c_class) {
        hk_dvm_throw("java/lang/InstantiationError", "%s", c->name);
        return NULL;
    }
    if (c == g_dvm.c_string) return hk_dvm_alloc_string(NULL, 0);
    o = alloc_raw(c, HK_J_OBJECT, (u64)c->nslots * sizeof(hk_jvalue));
    return o;
}

static u32 element_size(char kind)
{
    switch (kind) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    case 'J': case 'D': return 8;
    default: return (u32)sizeof(hk_jobj *);
    }
}

hk_jobj *hk_dvm_alloc_array(hk_dvm_class *array_class, i32 len)
{
    hk_jobj *a;
    u32 esz = element_size(array_class->elem);
    if (len < 0) {
        hk_dvm_throw("java/lang/NegativeArraySizeException", "%d", (int)len);
        return NULL;
    }
    a = alloc_raw(array_class, array_class->elem == 'L' ? HK_J_OBJ_ARRAY : HK_J_PRIM_ARRAY, (u64)(u32)len * esz);
    if (!a) return NULL;
    if (array_class->elem == 'L') {
        a->u.oarr.v = (hk_jobj **)(void *)hk_dvm_slots(a);
        a->u.oarr.len = (u32)len;
    } else {
        a->u.arr.data = hk_dvm_slots(a);
        a->u.arr.len = (u32)len;
        a->u.arr.esz = (u8)esz;
        a->u.arr.etype = array_class->elem;
    }
    return a;
}

hk_jobj *hk_dvm_new_array(const char *array_class, i32 len)
{
    hk_dvm_class *c = array_class[0] == '[' ? hk_dvm_find_class(array_class) : NULL;
    if (!c) {
        if (array_class[0] != '[') hk_dvm_throw("java/lang/IllegalArgumentException", "%s is not an array class", array_class);
        return NULL;
    }
    return hk_dvm_alloc_array(c, len);
}

/* ------------------------------------------------------------------ strings */

hk_jobj *hk_dvm_alloc_string(const u16 *chars, i32 len)
{
    hk_dvm_class *c = g_dvm.c_string ? g_dvm.c_string : hk_dvm_find_class("java/lang/String");
    hk_jobj *s;
    if (!c) return NULL;
    if (len < 0) len = 0;
    /*
     * The class's slots first (libcore's String has one, its hash), then the text. One unit more than the text, zero:
     * nothing here needs it, and a debugger or a careless reader is the better for it.
     */
    s = alloc_raw(c, HK_J_STRING, (u64)c->nslots * sizeof(hk_jvalue) + ((u64)(u32)len + 1) * 2);
    if (!s) return NULL;
    s->u.str.u16 = (u16 *)(void *)(hk_dvm_slots(s) + c->nslots);
    s->u.str.len16 = len;
    if (chars && len) memcpy(s->u.str.u16, chars, (size_t)len * 2);
    return s;
}

hk_jobj *hk_dvm_new_string_utf16(const u16 *chars, i32 len) { return hk_dvm_alloc_string(chars, len); }

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

hk_jobj *hk_dvm_new_string(const char *utf8)
{
    hk_jobj *s;
    if (!utf8) return NULL;
    s = hk_dvm_alloc_string(NULL, utf8_to_utf16(utf8, NULL));
    if (s) utf8_to_utf16(utf8, s->u.str.u16);
    return s;
}

const u16 *hk_dvm_string_utf16(hk_jobj *s, i32 *len)
{
    if (!s || s->kind != HK_J_STRING) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s->u.str.len16;
    return s->u.str.u16;
}

const char *hk_dvm_string_utf8(hk_jobj *s)
{
    const u16 *u;
    char *out;
    size_t k = 0;
    i32 n, i;
    if (!s || s->kind != HK_J_STRING) return NULL;
    if (s->u.str.utf8) return s->u.str.utf8;
    u = s->u.str.u16;
    n = s->u.str.len16;
    out = (char *)hk_alloc((size_t)n * 3 + 1);
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
    if (hk_atomic_cas_ptr((void *volatile *)(void *)&s->u.str.utf8, NULL, out) != NULL) free(out);
    return s->u.str.utf8;
}

typedef struct interned {
    hk_jobj *s;
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

hk_jobj *hk_dvm_intern_string(hk_jobj *s)
{
    interned *e;
    u32 h;
    if (!s || s->kind != HK_J_STRING) return s;
    h = hash_utf16(s->u.str.u16, s->u.str.len16);
    hk_dvm_lock();
    for (e = g_interned[h % INTERN_BUCKETS]; e; e = e->next)
        if (e->hash == h && e->s->u.str.len16 == s->u.str.len16 && !memcmp(e->s->u.str.u16, s->u.str.u16, (size_t)s->u.str.len16 * 2)) break;
    if (!e) {
        e = (interned *)hk_alloc(sizeof(*e));
        e->s = s;
        e->hash = h;
        e->next = g_interned[h % INTERN_BUCKETS];
        g_interned[h % INTERN_BUCKETS] = e;
        hk_atomic_add(&g_dvm.n_interned, 1);
    }
    hk_dvm_unlock();
    return e->s;
}

hk_jobj *hk_dvm_const_string(hk_dvm_dex *d, u32 idx)
{
    hk_jobj *s;
    const u8 *p;
    u16 *out;
    u32 len16, k = 0;
    if (idx >= d->nstr) {
        hk_dvm_throw("java/lang/VerifyError", "string index %u of %u in %s", (unsigned)idx, (unsigned)d->nstr, d->name);
        return NULL;
    }
    s = d->scache[idx];
    if (s) return s;
    p = (const u8 *)hk_dvm_dex_string(d, idx, &len16);
    s = hk_dvm_alloc_string(NULL, (i32)len16);
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
    s = hk_dvm_intern_string(s);
    d->scache[idx] = s;
    return s;
}

bool hk_dvm_arraycopy(hk_jobj *src, i32 sp, hk_jobj *dst, i32 dp, i32 n)
{
    i32 i;
    if (!src || !dst) {
        return hk_dvm_throw("java/lang/NullPointerException", "%s", src ? "dst == null" : "src == null");
    }
    if ((src->kind != HK_J_PRIM_ARRAY && src->kind != HK_J_OBJ_ARRAY) || src->kind != dst->kind
        || (src->kind == HK_J_PRIM_ARRAY && src->u.arr.etype != dst->u.arr.etype)) {
        return hk_dvm_throw("java/lang/ArrayStoreException", "arraycopy from a %s to a %s", hk_dvm_class_of(src)->name, hk_dvm_class_of(dst)->name);
    }
    if (sp < 0 || dp < 0 || n < 0 || sp > hk_dvm_array_length(src) - n || dp > hk_dvm_array_length(dst) - n) {
        return hk_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d", (int)hk_dvm_array_length(src),
                            (int)sp, (int)hk_dvm_array_length(dst), (int)dp, (int)n);
    }
    if (src->kind == HK_J_PRIM_ARRAY) {
        memmove((u8 *)dst->u.arr.data + (size_t)dp * dst->u.arr.esz, (const u8 *)src->u.arr.data + (size_t)sp * src->u.arr.esz, (size_t)n * src->u.arr.esz);
        return true;
    }
    /* References into an array of a narrower class are checked one by one, and what was stored before the first that does not fit stays. */
    if (!hk_dvm_assignable(hk_dvm_class_of(src)->component, hk_dvm_class_of(dst)->component)) {
        for (i = 0; i < n; i++) {
            hk_jobj *o = src->u.oarr.v[sp + i];
            if (o && !hk_dvm_is_instance(o, hk_dvm_class_of(dst)->component)) {
                return hk_dvm_throw("java/lang/ArrayStoreException", "%s cannot be stored in an array of type %s", hk_dvm_class_of(o)->name, hk_dvm_class_of(dst)->name);
            }
            dst->u.oarr.v[dp + i] = o;
        }
        return true;
    }
    memmove(dst->u.oarr.v + dp, src->u.oarr.v + sp, sizeof(hk_jobj *) * (size_t)n);
    return true;
}

/* ------------------------------------------------------------ Class objects */

hk_jobj *hk_dvm_mirror(hk_dvm_class *c)
{
    hk_jobj *m = c->mirror;
    hk_dvm_class *cc;
    if (m) return m;
    cc = g_dvm.c_class ? g_dvm.c_class : hk_dvm_find_class("java/lang/Class");
    if (!cc) return NULL;
    m = alloc_raw(cc, HK_J_CLASS, (u64)cc->nslots * sizeof(hk_jvalue));
    if (!m) return NULL;
    m->native = c;
    m->u.klass.jc = c->jc;
    /* Made twice by two threads at once: the first one stays, the other is an object nothing refers to. */
    if (hk_atomic_cas_ptr((void *volatile *)(void *)&c->mirror, NULL, m) == NULL && g_dvm.fill_mirror) g_dvm.fill_mirror(m, c);
    return c->mirror;
}

u32 hk_dvm_identity_hash(const hk_jobj *o)
{
    /* Objects do not move, so an object's address is its identity. */
    u64 a = (u64)(hk_addr)o >> 3;
    return (u32)((a ^ (a >> 29)) * 2654435761u);
}

/* ----------------------------------------------------------------- monitors */

/*
 * A Java monitor: a lock with its owner and how many times the owner holds
 * it, made the first time an object is locked. wait() lets go of every level
 * and takes them all back, as Java's does.
 */
typedef struct monitor {
    hk_lock mu;
    hk_cond cv;
    volatile u32 owner;
    int count;
    u32 notified;                      /* counts notify and notifyAll, under mu: how a waiter tells being woken from waking by itself */
} monitor;

static monitor *monitor_of(hk_jobj *o)
{
    hk_dvm_obj *d = HK_DVM_OBJ(o);
    monitor *m = (monitor *)d->monitor;
    if (m) return m;
    m = (monitor *)hk_alloc(sizeof(*m));
    hk_lock_init(&m->mu);
    hk_cond_init(&m->cv);
    if (hk_atomic_cas_ptr(&d->monitor, NULL, m) != NULL) free(m);
    return (monitor *)d->monitor;
}

void hk_dvm_monitor_enter(hk_jobj *o)
{
    monitor *m = monitor_of(o);
    u32 me = hk_thread_self_id();
    if (m->owner == me) {
        m->count++;
        return;
    }
    if (!hk_lock_try(&m->mu)) {
        /* Another thread has it: the wait, counted for bytecode's own monitor-enter (inside a native method it is the method's). */
        hk_dvm_thread *self = hk_dvm_self();
        u64 started = self->native_depth ? 0 : hk_time_monotonic_ns();
        hk_lock_acquire(&m->mu);
        if (started) self->blocked_ns += hk_time_monotonic_ns() - started;
    }
    m->owner = me;
    m->count = 1;
}

bool hk_dvm_monitor_exit(hk_jobj *o)
{
    monitor *m = (monitor *)HK_DVM_OBJ(o)->monitor;
    if (!m || m->owner != hk_thread_self_id()) return false;
    if (--m->count == 0) {
        m->owner = 0;
        hk_lock_release(&m->mu);
    }
    return true;
}

bool hk_dvm_monitor_wait(hk_jobj *o, i64 ms)
{
    monitor *m = (monitor *)HK_DVM_OBJ(o)->monitor;
    u32 me = hk_thread_self_id();
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
        hk_lock_release(&m->mu);
        hk_thread_yield();
        hk_lock_acquire(&m->mu);
    } else {
        /*
         * Until notified, or until the time is up by the clock: the host's wait may end early by itself, and on Windows a
         * timed one ends at a tick of the system timer, which can be most of the time asked for too soon. Java allows a
         * wait to end for no reason, but code that sleeps by waiting should get the time it asked for.
         */
        u32 seen = m->notified;
        u64 started = hk_time_monotonic_ns(), deadline = ms > 0 ? started + (u64)ms * 1000000ull : 0;
        hk_dvm_thread *self = hk_dvm_self();
        while (m->notified == seen) {
            u64 now;
            if (!deadline) {
                hk_cond_wait(&m->cv, &m->mu);
                continue;
            }
            now = hk_time_monotonic_ns();
            if (now >= deadline) break;
            hk_cond_wait_ns(&m->cv, &m->mu, deadline - now);
        }
        self->waited_ns += hk_time_monotonic_ns() - started;
    }
    m->owner = me;
    m->count = held;
    return true;
}

bool hk_dvm_monitor_notify(hk_jobj *o, bool all)
{
    monitor *m = (monitor *)HK_DVM_OBJ(o)->monitor;
    if (!m || m->owner != hk_thread_self_id()) return false;
    m->notified++;
    if (all) hk_cond_broadcast(&m->cv);
    else hk_cond_signal(&m->cv);
    return true;
}

/* --------------------------------------------------------------- exceptions */

void hk_dvm_throw_object(hk_jobj *e) { hk_dvm_self()->pending = e; }
hk_jobj *hk_dvm_pending(void) { return hk_dvm_self()->pending; }
void hk_dvm_clear_pending(void) { hk_dvm_self()->pending = NULL; }

bool hk_dvm_throw(const char *cls, const char *fmt, ...)
{
    hk_dvm_thread *t = hk_dvm_self();
    hk_dvm_class *c;
    hk_dvm_method *ctor;
    hk_jobj *e;
    hk_jvalue arg, ignored;
    char msg[600];
    msg[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
    }
    if (g_dvm.trace >= 1) hk_info("dvm: throw %s: %s", cls, msg);
    if (!hk_dvm_class_exists(cls)) {
        /* The boot class path has no such exception: say that instead, with what was meant. */
        if (!strcmp(cls, "java/lang/NoClassDefFoundError")) hk_fatal("dvm: the boot class path has no java.lang.NoClassDefFoundError to throw (%s)", msg);
        return hk_dvm_throw("java/lang/NoClassDefFoundError", "%s (to throw it: %s)", cls, msg);
    }
    t->pending = NULL;
    c = hk_dvm_find_class(cls);
    if (!c || !hk_dvm_ensure_init(c)) return false;             /* what stopped it is pending instead */
    e = hk_dvm_alloc_object(c);
    if (!e) return false;
    arg.j = 0;
    ctor = hk_dvm_find_method(c, "<init>", "(Ljava/lang/String;)V", false);
    /*
     * Whatever happens from here on, something is pending when this returns: what stopped the constructor if it threw, and
     * otherwise the exception, even one whose constructor could not be run at all (the stack was full).
     */
    if (ctor) {
        arg.l = fmt ? hk_dvm_new_string(msg) : NULL;
        if (!hk_dvm_invoke(ctor, e, &arg, &ignored) && t->pending) return false;
    } else {
        ctor = hk_dvm_find_method(c, "<init>", "()V", false);
        if (ctor && !hk_dvm_invoke(ctor, e, NULL, &ignored) && t->pending) return false;
        if (fmt && g_dvm.f_message && hk_dvm_is_instance(e, g_dvm.c_throwable)) hk_dvm_slots(e)[g_dvm.f_message->slot].l = hk_dvm_new_string(msg);
    }
    t->pending = e;
    return false;
}

void hk_dvm_objects_start(void)
{
    hk_jvalue arg, ignored;
    hk_dvm_class *c = hk_dvm_class_exists("java/lang/OutOfMemoryError") ? hk_dvm_find_class("java/lang/OutOfMemoryError") : NULL;
    hk_dvm_method *ctor = c ? hk_dvm_find_method(c, "<init>", "(Ljava/lang/String;)V", false) : NULL;
    hk_jobj *e = c && hk_dvm_ensure_init(c) ? hk_dvm_alloc_object(c) : NULL;
    arg.j = 0;
    if (e && ctor) {
        arg.l = hk_dvm_new_string("the heap is exhausted");
        if (hk_dvm_invoke(ctor, e, &arg, &ignored)) g_out_of_memory = e;
    }
    hk_dvm_clear_pending();
}

static const char *message_of(hk_jobj *e)
{
    hk_jobj *m;
    if (!g_dvm.f_message || !hk_dvm_is_instance(e, g_dvm.c_throwable)) return NULL;
    m = hk_dvm_slots(e)[g_dvm.f_message->slot].l;
    return hk_dvm_string_utf8(m);
}

static size_t append(char *buf, size_t n, size_t k, const char *fmt, ...) HK_PRINTF(4, 5);
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

const char *hk_dvm_describe_pending(char *buf, size_t n)
{
    hk_jobj *e = hk_dvm_self()->pending;
    const char *msg;
    size_t k = 0;
    int depth;
    if (!e || !n) return NULL;
    buf[0] = 0;
    k = append_dotted(buf, n, k, hk_dvm_class_of(e)->name);
    msg = message_of(e);
    if (msg) k = append(buf, n, k, ": %s", msg);
    for (depth = 0; depth < 4 && g_dvm.f_cause && hk_dvm_is_instance(e, g_dvm.c_throwable); depth++) {
        hk_jobj *cause = hk_dvm_slots(e)[g_dvm.f_cause->slot].l;
        if (!cause || cause == e) break;
        k = append(buf, n, k, " | caused by ");
        k = append_dotted(buf, n, k, hk_dvm_class_of(cause)->name);
        msg = message_of(cause);
        if (msg) k = append(buf, n, k, ": %s", msg);
        e = cause;
    }
    return buf;
}

hk_jobj *hk_dvm_capture_stack(void)
{
    hk_dvm_thread *t = hk_dvm_self();
    hk_dvm_frame *f = t->top, *k;
    hk_dvm_class *longs;
    hk_jobj *a;
    i64 *out;
    i32 n = 0, i = 0;
    /* Not the frames that are the exception being made: its constructors and fillInStackTrace itself. */
    while (f && g_dvm.c_throwable && hk_dvm_assignable(f->m->cls, g_dvm.c_throwable)
           && (f->m->name[0] == '<' || !strcmp(f->m->name, "fillInStackTrace") || !strcmp(f->m->name, "nativeFillInStackTrace")))
        f = f->prev;
    for (k = f; k; k = k->prev) n++;
    if (n > 1024) n = 1024;
    longs = hk_dvm_find_class("[J");
    a = longs ? hk_dvm_alloc_array(longs, n * 2) : NULL;
    if (!a) return NULL;
    out = (i64 *)a->u.arr.data;
    for (k = f; k && i < n; k = k->prev, i++) {
        out[2 * i] = (i64)(hk_addr)k->m;
        out[2 * i + 1] = k->m->insns ? (i64)k->pc : -1;
    }
    return a;
}

int hk_dvm_pending_stack(char *buf, size_t n)
{
    hk_jobj *e = hk_dvm_self()->pending, *trace;
    const i64 *v;
    size_t k = 0;
    i32 frames, i;
    if (n) buf[0] = 0;
    if (!e || !n || !g_dvm.f_backtrace || !hk_dvm_is_instance(e, g_dvm.c_throwable)) return 0;
    trace = hk_dvm_slots(e)[g_dvm.f_backtrace->slot].l;
    if (!trace || trace->kind != HK_J_PRIM_ARRAY || trace->u.arr.etype != 'J') return 0;
    v = (const i64 *)trace->u.arr.data;
    frames = (i32)(trace->u.arr.len / 2);
    for (i = 0; i < frames; i++) {
        const hk_dvm_method *m = (const hk_dvm_method *)(hk_addr)v[2 * i];
        const char *source = hk_dvm_dex_source(m->cls);
        int line = v[2 * i + 1] >= 0 ? hk_dvm_dex_line(m, (u32)v[2 * i + 1]) : -1;
        k = append(buf, n, k, "  at ");
        k = append_dotted(buf, n, k, m->cls->name);
        if (m->flags & DVM_ACC_NATIVE) k = append(buf, n, k, ".%s(native)\n", m->name);
        else if (line >= 0) k = append(buf, n, k, ".%s(%s:%d)\n", m->name, source ? source : "?", line);
        else k = append(buf, n, k, ".%s(pc %d)\n", m->name, (int)v[2 * i + 1]);
    }
    return (int)frames;
}
