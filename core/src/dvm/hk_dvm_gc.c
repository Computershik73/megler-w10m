/*
 * The collector: a non-moving mark and sweep that stops the world (docs/JAVA.md, "The collector"). This part marks: it
 * stops every other thread of the runtime's where it is, follows references from the roots, and says what it found. The
 * sweep is the next step; until it is written, a collection frees nothing (hk_dvm_gc_census).
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
    u64 marked, marked_bytes, conservative;   /* objects marked; their bytes; marked first by a conservative root */
    bool counting_conservative;
} gc_state;

static gc_state G;

/* An object reached: marked, and its references to follow later. `o` is an object's start, or anything else (ignored). */
static void reach(void *o)
{
    if (!o || !hk_dvm_heap_set_mark(o)) return;
    G.marked++;
    G.marked_bytes += HK_DVM_OBJ(o)->size;
    if (G.counting_conservative) G.conservative++;
    if (G.n < G.cap) G.stack[G.n++] = o;
    else G.overflow = true;
}

/* A precise reference: an object of the runtime's (a pointer to one that is not in the heap, from before it started, is not). */
static void reach_ref(hk_jobj *o, void *user)
{
    (void)user;
    if (o && o->dvm) reach(o);
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

bool hk_dvm_gc_census(hk_dvm_gc_result *out)
{
    u64 t0, t1, t2;
    u32 objects;
    void **stack;
    int i;
    memset(out, 0, sizeof(*out));
    if (!g_dvm.running) return false;
    hk_lock_acquire(&g_gc_lock);
    t0 = hk_time_monotonic_ns();
    g_collector = hk_dvm_self();
    hk_dvm_lock();
    hk_jni_gc_lock(true);
    hk_dvm_heap_lock();
    /* The mark stack, before anything is stopped: one entry for every object there is. */
    objects = hk_dvm_heap_objects();
    stack = (void **)malloc(sizeof(void *) * ((size_t)objects + 64));
    if (!stack) {
        hk_dvm_heap_unlock();
        hk_jni_gc_lock(false);
        hk_dvm_unlock();
        hk_lock_release(&g_gc_lock);
        return false;
    }
    memset(&G, 0, sizeof(G));
    G.stack = stack;
    G.cap = objects + 64;
    g_nstopped = 0;
    g_stop_failed = false;
    hk_dvm_threads_each(stop_thread, NULL);
    t1 = hk_time_monotonic_ns();
    if (!g_stop_failed) {
        hk_dvm_heap_clear_marks();
        mark();
    }
    for (i = 0; i < g_nstopped; i++) hk_thread_resume(g_stopped[i].t->host);
    t2 = hk_time_monotonic_ns();
    out->objects = objects;
    out->live_objects = G.marked;
    out->live_bytes = G.marked_bytes;
    out->conservative = G.conservative;
    out->threads_stopped = (u32)g_nstopped;
    out->stop_ns = t1 - t0;
    out->mark_ns = t2 - t1;
    hk_dvm_heap_unlock();
    hk_jni_gc_lock(false);
    hk_dvm_unlock();
    g_collector = NULL;
    free(stack);
    hk_lock_release(&g_gc_lock);
    return !g_stop_failed;
}

bool hk_dvm_gc_marked(const hk_jobj *o)
{
    bool marked;
    hk_dvm_heap_lock();
    marked = o && hk_dvm_heap_find((hk_addr)o) == (const void *)o && hk_dvm_heap_is_marked(o);
    hk_dvm_heap_unlock();
    return marked;
}
