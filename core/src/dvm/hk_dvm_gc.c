/*
 * The collector: a non-moving mark and sweep that stops the world (docs/JAVA.md, "The collector"). It stops every other
 * thread of the runtime's where it is, follows references from the roots, lets the threads go on, and frees what it did
 * not reach (hk_dvm_gc); or only counts it (hk_dvm_gc_census). It runs on the allocating thread when enough has been made
 * since the last collection (the larger of 8 MB and three quarters of what lived after it, native memory registered
 * counted in), on System.gc(), and before an allocation fails.
 *
 * Roots, and how each is read:
 *  - exactly: every linked class's static fields of reference type, its Class object and the error its initialisation
 *    threw; each DEX file's resolved string constants and the interned strings; each thread's pending exception, its
 *    java.lang.Thread and its scratch value; the proxies' method arrays; the JNI layer's global (and weak global)
 *    references, reflected methods and stand-in statics (hk_jni_gc_roots); the runtime's preallocated OutOfMemoryError and
 *    the app's class loader; and what C code registered (hk_dvm_add_root);
 *  - conservatively: every thread's frame stack (the interpreter's registers are untyped) and every thread's C stack from
 *    where it was stopped to its base, with the registers it was stopped with. A word that points into an object keeps it.
 * Inside objects it is exact: a class's reference slots (made at its first instance, hk_dvm_gc_class_ready), all of an
 * object array, nothing of a string or a primitive array, and the JNI layer's by-name fields marked as objects.
 *
 * Between the stop and the resume nothing is allocated, freed or logged: a stopped thread may hold the C library's heap lock
 * or the log's. The mark stack is allocated before the stop, as large as the heap has objects, so it cannot run out; should
 * it all the same, the overflow is finished by going over the heap for marked objects instead of allocating.
 */
#include <setjmp.h>
#include <stdio.h>

#include "hk_dvm_internal.h"

/* ------------------------------------------------------------ the C roots */

#define MAX_ROOTS 64
static hk_jobj **g_roots[MAX_ROOTS];
static volatile i32 g_nroots;

void hk_dvm_add_root(hk_jobj **slot)
{
    i32 i, n = g_nroots;
    for (i = 0; i < n; i++)
        if (g_roots[i] == slot) return;
    i = hk_atomic_add(&g_nroots, 1) - 1;
    if (i < MAX_ROOTS) g_roots[i] = slot;
    else hk_err("dvm: more than %d roots registered in C; %p is not one", MAX_ROOTS, (void *)slot);
}

/* --------------------------------------------------------- the class's slots */

void hk_dvm_gc_class_ready(hk_dvm_class *c)
{
    hk_dvm_class *k;
    u32 n = 0, *slots;
    int i;
    if (c->ref_slots_ready) return;
    for (k = c; k; k = k->super)
        for (i = 0; i < k->nifields; i++)
            if ((k->ifields[i].type[0] == 'L' || k->ifields[i].type[0] == '[') && k->ifields[i].slot < 0xFFFFFFF0u) n++;
    slots = n ? (u32 *)hk_alloc(sizeof(u32) * n) : NULL;
    n = 0;
    for (k = c; k; k = k->super)
        for (i = 0; i < k->nifields; i++)
            if ((k->ifields[i].type[0] == 'L' || k->ifields[i].type[0] == '[') && k->ifields[i].slot < 0xFFFFFFF0u) slots[n++] = k->ifields[i].slot;
    hk_dvm_lock();
    if (!c->ref_slots_ready) {
        c->ref_slots = slots;
        c->nref_slots = n;
        hk_atomic_xchg(&c->ref_slots_ready, 1);
        slots = NULL;
    }
    hk_dvm_unlock();
    free(slots);
}

/* ---------------------------------------------------------------- marking */

typedef struct gc_state {
    void **stack;
    u32 n, cap;
    bool overflow;
    u64 marked, marked_bytes, conservative, conservative_bytes;   /* objects marked; their bytes; marked first by a conservative root */
    bool counting_conservative;
} gc_state;

static gc_state G;

/* An object reached: marked, and its references to follow later. `o` is an object's start, or anything else (ignored: nothing is read of it before the heap has said it is an object). */
static void reach(void *o)
{
    if (!o || !hk_dvm_heap_set_mark(o)) return;
    G.marked++;
    G.marked_bytes += HK_DVM_OBJ(o)->size;
    if (G.counting_conservative) {
        G.conservative++;
        G.conservative_bytes += HK_DVM_OBJ(o)->size;
    }
    if (G.n < G.cap) G.stack[G.n++] = o;
    else G.overflow = true;
}

/* A precise reference: an object of the runtime's, when the heap has it (one from before it started, or a C object, is not). */
static void reach_ref(hk_jobj *o, void *user)
{
    (void)user;
    reach(o);
}

/* A word that may point into an object. */
static void reach_word(hk_addr w) { reach(hk_dvm_heap_find(w)); }

static void scan_words(hk_addr from, hk_addr to)
{
    hk_addr p;
    from = HK_ALIGN_UP(from, sizeof(hk_addr));
    for (p = from; p + sizeof(hk_addr) <= to; p += sizeof(hk_addr)) reach_word(*(const hk_addr *)p);
}

static void follow(void *p)
{
    hk_dvm_obj *o = (hk_dvm_obj *)p;
    hk_jobj *j = &o->j;
    u32 i;
    if (j->kind == HK_J_OBJ_ARRAY) {
        for (i = 0; i < j->u.oarr.len; i++) reach_ref(j->u.oarr.v[i], NULL);
    } else if (j->kind != HK_J_PRIM_ARRAY && j->kind != HK_J_STRING && o->cls && o->cls->ref_slots_ready) {
        const hk_jvalue *slots = hk_dvm_slots(j);
        for (i = 0; i < o->cls->nref_slots; i++) reach_ref(slots[o->cls->ref_slots[i]].l, NULL);
    }
    if (j->fields)
        for (i = 0; i < (u32)j->nfields; i++)
            if (j->fields[i].is_object) reach_ref(j->fields[i].v.l, NULL);
}

static void drain(void)
{
    while (G.n) follow(G.stack[--G.n]);
}

/* What the classes hold: statics of reference type, the Class object, the error of a failed initialisation. */
static void class_roots(hk_dvm_class *c, void *user)
{
    int i;
    (void)user;
    reach_ref(c->mirror, NULL);
    reach_ref(c->init_error, NULL);
    if (!c->statics) return;
    for (i = 0; i < c->nsfields; i++) {
        const hk_dvm_field *f = &c->sfields[i];
        if ((f->type[0] == 'L' || f->type[0] == '[') && f->slot < (u32)c->nsfields) reach_ref(c->statics[f->slot].l, NULL);
    }
}

static void exact_thread_roots(hk_dvm_thread *t, void *user)
{
    (void)user;
    reach_ref(t->pending, NULL);
    reach_ref(t->peer, NULL);
    reach_word((hk_addr)t->scratch.l);
}

/* The threads stopped for this collection, with their registers. */
#define MAX_STOPPED 512
static struct {
    hk_dvm_thread *t;
    hk_thread_regs regs;
} g_stopped[MAX_STOPPED];
static int g_nstopped;
static hk_dvm_thread *g_collector;
static bool g_stop_failed;

static void stop_thread(hk_dvm_thread *t, void *user)
{
    (void)user;
    if (t == g_collector) return;
    /* A thread that cannot be stopped or whose stack cannot be scanned: no collection, rather than one that misses its roots. */
    if (!t->host || !t->stack_hi || g_nstopped == MAX_STOPPED) {
        g_stop_failed = true;
        return;
    }
    if (!hk_thread_suspend(t->host, &g_stopped[g_nstopped].regs)) {
        /* Gone: a thread that ended without the host's hook (one the host did not start). Nothing of it is scanned. */
        return;
    }
    g_stopped[g_nstopped++].t = t;
}

/* A thread's frame stack, the registers of every frame, and a margin above the top one for a frame being made. */
static void scan_frames(hk_dvm_thread *t, void *user)
{
    hk_dvm_reg *top;
    (void)user;
    if (!t->stack) return;
    top = t->sp + 64 < t->stack + DVM_STACK_WORDS ? t->sp + 64 : t->stack + DVM_STACK_WORDS;
    scan_words((hk_addr)t->stack, (hk_addr)top);
}

static HK_NOINLINE void scan_own_stack(void)
{
    volatile hk_addr here = 0;
    hk_dvm_thread *t = g_collector;
    if (t && t->stack_hi) scan_words((hk_addr)&here, t->stack_hi);
}

static void conservative_roots(void)
{
    int i, k;
    jmp_buf registers;
    /* The collector's own registers, onto its stack, and its stack from here to its base. */
    if (setjmp(registers) == 0) scan_own_stack();
    for (i = 0; i < g_nstopped; i++) {
        const hk_thread_regs *r = &g_stopped[i].regs;
        hk_dvm_thread *t = g_stopped[i].t;
        for (k = 0; k < r->n; k++) reach_word(r->r[k]);
        if (r->sp >= t->stack_lo && r->sp < t->stack_hi) scan_words(r->sp, t->stack_hi);
    }
    hk_dvm_threads_each(scan_frames, NULL);
}

static void heap_rescan_marked(void);

/* Everything reachable marked; the counts in G. With the runtime's locks held and the other threads stopped. */
static void mark(void)
{
    hk_dvm_dex *d;
    u32 i, k;
    hk_dvm_each_linked_locked(class_roots, NULL);
    for (i = 0; (d = hk_dvm_dex_at(i)) != NULL; i++)
        if (d->scache)
            for (k = 0; k < d->nstr; k++) reach_ref(d->scache[k], NULL);
    hk_dvm_interned_each(reach_ref, NULL);
    hk_dvm_threads_each(exact_thread_roots, NULL);
    hk_dvm_proxy_roots(reach_ref, NULL);
    hk_dvm_object_roots(reach_ref, NULL);
    hk_jni_gc_roots(reach_ref, NULL);
    for (i = 0; i < (u32)g_nroots && i < MAX_ROOTS; i++)
        if (g_roots[i]) reach_ref(*g_roots[i], NULL);
    drain();
    /* What only the conservative scans find, counted apart: the next phone log says how much that is. */
    G.counting_conservative = true;
    conservative_roots();
    drain();
    G.counting_conservative = false;
    while (G.overflow) {
        G.overflow = false;
        heap_rescan_marked();
    }
}

/* The mark stack ran out: every marked object's references followed again, until a pass marks nothing new. */
static void heap_rescan_marked(void)
{
    u64 before;
    do {
        before = G.marked;
        hk_dvm_heap_each_marked(follow);
        drain();
    } while (G.marked != before || G.overflow);
}

void hk_dvm_gc_thread_gone(hk_dvm_thread *t) { (void)t; }

/* ---------------------------------------------------------- a collection */

static hk_lock g_gc_lock = HK_LOCK_INIT;
static HK_THREAD_LOCAL bool t_collecting;
static bool g_enabled, g_poison;
/*
 * When the next collection is due: bytes made since the last one, and native bytes registered since (VMRuntime, bitmaps).
 * Bytes, not rounded: most objects are a few dozen of them. A sum stops short of 2 GB, which no budget comes near.
 */
#define MIN_BUDGET (8u << 20)
static volatile i32 g_since, g_native_since;
static u64 g_budget = MIN_BUDGET, g_min_budget = MIN_BUDGET;
/* For the frame meter and the log: collections made, and the time the world stood still in them. */
static volatile i32 g_collections;
static u64 g_pause_ns;

void hk_dvm_gc_enable(bool on) { g_enabled = on; }
void hk_dvm_gc_set_poison(bool on) { g_poison = on; }

void hk_dvm_gc_set_min_budget(u64 bytes)
{
    g_min_budget = bytes;
    g_budget = bytes;
}

void hk_dvm_gc_totals(u64 *collections, u64 *pause_ns)
{
    if (collections) *collections = (u64)(u32)g_collections;
    if (pause_ns) *pause_ns = g_pause_ns;
}

static void count(volatile i32 *sum, u64 bytes)
{
    if (bytes > 0x40000000u) bytes = 0x40000000u;
    if (hk_atomic_add(sum, (i32)bytes) < 0) hk_atomic_xchg(sum, 0x7FFFFFFF);
}

void hk_dvm_gc_native(i64 bytes)
{
    /* Only what grows counts toward the next collection; what is freed is freed. */
    if (bytes > 0) count(&g_native_since, (u64)bytes);
}

static bool due(void) { return (u64)(u32)g_since + (u64)(u32)g_native_since >= g_budget; }

void hk_dvm_gc_allocating(size_t bytes)
{
    if (!g_enabled || !g_dvm.running || t_collecting) return;
    count(&g_since, bytes);
    if (due()) hk_dvm_gc("allocation", NULL);
}

/* The world stopped, everything reachable marked; `sweep`: and what is not, freed. False when not every thread could be stopped. */
static bool collect(const char *reason, bool sweep, hk_dvm_gc_result *out)
{
    u64 t0, t1 = 0, t2 = 0, t3;
    u32 objects;
    void **stack;
    int i;
    bool ok;
    memset(out, 0, sizeof(*out));
    if (!g_dvm.running || t_collecting) return false;
    hk_lock_acquire(&g_gc_lock);
    /* Two threads that found it due at once: the second has nothing left to do. */
    if (sweep && !strcmp(reason, "allocation") && !due()) {
        hk_lock_release(&g_gc_lock);
        return true;
    }
    t_collecting = true;
    t0 = hk_time_monotonic_ns();
    g_collector = hk_dvm_self();
    hk_dvm_lock();
    hk_jni_gc_lock(true);
    hk_dvm_heap_lock();
    /* The mark stack, before anything is stopped: one entry for every object there is. */
    objects = hk_dvm_heap_objects();
    stack = (void **)malloc(sizeof(void *) * ((size_t)objects + 64));
    ok = stack != NULL;
    if (ok) {
        memset(&G, 0, sizeof(G));
        G.stack = stack;
        G.cap = objects + 64;
        g_nstopped = 0;
        g_stop_failed = false;
        hk_dvm_threads_each(stop_thread, NULL);
        t1 = hk_time_monotonic_ns();
        ok = !g_stop_failed;
        if (ok) {
            hk_dvm_heap_clear_marks();
            mark();
        }
        for (i = 0; i < g_nstopped; i++) hk_thread_resume(g_stopped[i].t->host);
        t2 = hk_time_monotonic_ns();
        out->objects = objects;
        out->live_objects = G.marked;
        out->live_bytes = G.marked_bytes;
        out->conservative = G.conservative;
        out->conservative_bytes = G.conservative_bytes;
        out->threads_stopped = (u32)g_nstopped;
        out->stop_ns = t1 - t0;
        out->mark_ns = t2 - t1;
    }
    hk_jni_gc_lock(false);
    hk_dvm_unlock();
    /* The others go on; the heap stays locked while what nothing reached is freed, so nothing is made meanwhile. */
    if (ok && sweep) {
        hk_dvm_heap_sweep(hk_dvm_object_dying, g_poison, &out->freed_objects, &out->freed_bytes);
        t3 = hk_time_monotonic_ns();
        out->sweep_ns = t3 - t2;
        g_budget = out->live_bytes * 3 / 4 > g_min_budget ? out->live_bytes * 3 / 4 : g_min_budget;
        hk_atomic_xchg(&g_since, 0);
        hk_atomic_xchg(&g_native_since, 0);
    }
    hk_dvm_heap_unlock();
    if (ok) {
        hk_atomic_add(&g_collections, 1);
        g_pause_ns += out->stop_ns + out->mark_ns + out->sweep_ns;
    }
    g_collector = NULL;
    t_collecting = false;
    free(stack);
    hk_lock_release(&g_gc_lock);
    return ok;
}

bool hk_dvm_gc_census(hk_dvm_gc_result *out) { return collect("census", false, out); }

bool hk_dvm_gc(const char *reason, hk_dvm_gc_result *out)
{
    hk_dvm_gc_result r;
    bool ok = collect(reason, true, &r);
    if (out) *out = r;
    if (!ok) {
        hk_warn("gc: %s: not collected (a thread of the runtime's could not be stopped, or there was no memory for the mark stack)", reason);
        return false;
    }
    if (!r.objects) return true;
    hk_info("gc: %s: %.1f ms (stop %.1f, mark %.1f, sweep %.1f); freed %.1f MB (%u objects), live %.1f MB (%u objects); %u threads stopped; "
            "%.2f MB (%u objects) found only through a conservative root",
            reason, (double)(r.stop_ns + r.mark_ns + r.sweep_ns) / 1e6, (double)r.stop_ns / 1e6, (double)r.mark_ns / 1e6, (double)r.sweep_ns / 1e6,
            (double)r.freed_bytes / 1048576.0, (unsigned)r.freed_objects, (double)r.live_bytes / 1048576.0, (unsigned)r.live_objects,
            (unsigned)r.threads_stopped, (double)r.conservative_bytes / 1048576.0, (unsigned)r.conservative);
    return true;
}

bool hk_dvm_gc_marked(const hk_jobj *o)
{
    bool marked;
    hk_dvm_heap_lock();
    marked = o && hk_dvm_heap_find((hk_addr)o) == (const void *)o && hk_dvm_heap_is_marked(o);
    hk_dvm_heap_unlock();
    return marked;
}
