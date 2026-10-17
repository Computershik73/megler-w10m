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
        hk_dvm_method *fin = hk_dvm_find_virtual(c, hk_dvm_intern("finalize"), hk_dvm_intern("()V"));
        c->ref_slots = slots;
        c->nref_slots = n;
        /* A java.lang.ref class: which kind, by the nearest of the three it descends from. */
        for (k = c; k && !c->gc_ref; k = k->super) {
            if (!strcmp(k->name, "java/lang/ref/SoftReference")) c->gc_ref = 1;
            else if (!strcmp(k->name, "java/lang/ref/WeakReference")) c->gc_ref = 2;
            else if (!strcmp(k->name, "java/lang/ref/PhantomReference")) c->gc_ref = 3;
        }
        /* Finalizable, as ART has it: a finalize() other than Object's, and not one that only returns. */
        c->finalizable = fin && fin->cls != g_dvm.c_object && fin->insns && fin->ninsns > 1;
        hk_atomic_xchg(&c->ref_slots_ready, 1);
        slots = NULL;
    }
    hk_dvm_unlock();
    free(slots);
}

/* --------------------------------------------- references and finalizers */

/*
 * java.lang.ref.Reference's own slots, found once: its referent is not followed while marking, and pendingNext links what
 * the reference thread hands to ReferenceQueue.enqueuePending. -1: not there (the built-in java.lang has no references).
 */
static i32 g_referent = -1, g_pending_next = -1, g_queue = -1;
static volatile i32 g_ref_slots_known;

static void ref_slots_known(void)
{
    hk_dvm_class *r;
    hk_dvm_field *f;
    if (g_ref_slots_known) return;
    r = hk_dvm_class_exists("java/lang/ref/Reference") ? hk_dvm_find_class("java/lang/ref/Reference") : NULL;
    if (!r) hk_dvm_clear_pending();
    if (r && (f = hk_dvm_find_field(r, "referent", false)) != NULL) g_referent = (i32)f->slot;
    if (r && (f = hk_dvm_find_field(r, "pendingNext", false)) != NULL) g_pending_next = (i32)f->slot;
    if (r && (f = hk_dvm_find_field(r, "queue", false)) != NULL) g_queue = (i32)f->slot;
    hk_atomic_xchg(&g_ref_slots_known, 1);
}

/* Objects to finalize: those made (`g_finalizable`, not roots), and those found unreachable, waiting for their finalize() (roots). */
static hk_lock g_final_lock = HK_LOCK_INIT;
static hk_cond g_final_cond = HK_COND_INIT;
static hk_jobj **g_finalizable, **g_to_finalize;
static u32 g_nfinalizable, g_cap_finalizable, g_nto_finalize, g_cap_to_finalize;
static volatile i32 g_finalizing;      /* finalize() calls under way on the reference thread */
static volatile i32 g_references;      /* reference objects ever made: what the list of those found while marking may need */
/* What the last collections cleared and the reference thread is to hand to ReferenceQueue.enqueuePending: circular, through pendingNext. */
static hk_jobj *g_pending_refs;
static hk_thread *g_ref_thread;

void hk_dvm_gc_born(hk_jobj *o)
{
    hk_dvm_class *c = hk_dvm_class_of(o);
    if (c->gc_ref) {
        ref_slots_known();
        hk_atomic_add(&g_references, 1);
    }
    if (!c->finalizable) return;
    hk_lock_acquire(&g_final_lock);
    if (g_nfinalizable == g_cap_finalizable) {
        u32 cap = g_cap_finalizable ? g_cap_finalizable * 2 : 256;
        hk_jobj **grown = (hk_jobj **)realloc(g_finalizable, sizeof(*grown) * cap);
        if (grown) {
            g_finalizable = grown;
            g_cap_finalizable = cap;
        }
    }
    if (g_nfinalizable < g_cap_finalizable) g_finalizable[g_nfinalizable++] = o;
    hk_lock_release(&g_final_lock);
}

/* ---------------------------------------------------------------- marking */

typedef struct gc_state {
    void **stack;
    u32 n, cap;
    bool overflow;
    u64 marked, marked_bytes, conservative, conservative_bytes;   /* objects marked; their bytes; marked first by a conservative root */
    bool counting_conservative;
    hk_jobj **found;                   /* reference objects whose referent was not followed, to decide about after marking */
    u32 nfound, cap_found;
    u32 cleared, enqueued, finalizable;
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
        hk_jvalue *slots = hk_dvm_slots(j);
        bool reference = o->cls->gc_ref && g_referent >= 0 && g_pending_next >= 0;
        for (i = 0; i < o->cls->nref_slots; i++) {
            /* A reference's referent is not followed: it is decided about after marking (process_references). */
            if (reference && o->cls->ref_slots[i] == (u32)g_referent) continue;
            reach_ref(slots[o->cls->ref_slots[i]].l, NULL);
        }
        if (reference && slots[g_referent].l) {
            /* One already waiting to be enqueued (pendingNext set) keeps its referent as it is; so does one the list has no room for. */
            if (!slots[g_pending_next].l && G.nfound < G.cap_found) G.found[G.nfound++] = j;
            else reach_ref(slots[g_referent].l, NULL);
        }
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
    reach_ref(t->waiting_monitor, NULL);
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

/* A cleared reference with a queue joins the circular list the reference thread will hand to ReferenceQueue.enqueuePending. */
static void enqueue(hk_jobj *r)
{
    hk_jvalue *slots = hk_dvm_slots(r), *head;
    if (g_queue < 0 || !slots[g_queue].l) return;
    if (!g_pending_refs) {
        slots[g_pending_next].l = r;
        g_pending_refs = r;
    } else {
        /* Into the ring, after its head. */
        head = hk_dvm_slots(g_pending_refs);
        slots[g_pending_next].l = head[g_pending_next].l;
        head[g_pending_next].l = r;
    }
    G.enqueued++;
}

/*
 * After the strong marking: soft references keep their referents unless soft ones are being cleared; weak (and cleared soft)
 * references whose referent nothing else reached are cleared and enqueued; objects to finalize that nothing reached are
 * kept for their finalize(), with everything they reach; then phantom references whose referent is still not reached are
 * cleared and enqueued -- Java's order.
 */
static void process_references(bool clear_soft)
{
    u32 i;
    if (!clear_soft) {
        for (i = 0; i < G.nfound; i++)
            if (hk_dvm_class_of(G.found[i])->gc_ref == 1) reach_ref(hk_dvm_slots(G.found[i])[g_referent].l, NULL);
        drain();
    }
    for (i = 0; i < G.nfound; i++) {
        hk_jobj *r = G.found[i];
        hk_jvalue *slots = hk_dvm_slots(r);
        if (hk_dvm_class_of(r)->gc_ref == 3 || !slots[g_referent].l || hk_dvm_heap_is_marked(slots[g_referent].l)) continue;
        slots[g_referent].l = NULL;
        G.cleared++;
        enqueue(r);
    }
    /* Finalizable objects nothing reached: to finalize, and so reached (with what they reach) until that is done. */
    for (i = 0; i < g_nfinalizable;) {
        hk_jobj *o = g_finalizable[i];
        if (hk_dvm_heap_is_marked(o) || g_nto_finalize == g_cap_to_finalize) {
            i++;
            continue;
        }
        g_to_finalize[g_nto_finalize++] = o;
        g_finalizable[i] = g_finalizable[--g_nfinalizable];
        G.finalizable++;
        reach_ref(o, NULL);
    }
    drain();
    for (i = 0; i < G.nfound; i++) {
        hk_jobj *r = G.found[i];
        hk_jvalue *slots = hk_dvm_slots(r);
        if (hk_dvm_class_of(r)->gc_ref != 3 || !slots[g_referent].l || hk_dvm_heap_is_marked(slots[g_referent].l)) continue;
        slots[g_referent].l = NULL;
        G.cleared++;
        enqueue(r);
    }
}

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
    /* What waits for the reference thread: objects to finalize, and the references it has not handed on yet. */
    for (i = 0; i < g_nto_finalize; i++) reach_ref(g_to_finalize[i], NULL);
    reach_ref(g_pending_refs, NULL);
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

bool hk_dvm_gc_counted(size_t bytes)
{
    if (!g_enabled || !g_dvm.running || t_collecting) return false;
    count(&g_since, bytes);
    return due();
}

/* The world stopped, everything reachable marked; `sweep`: and what is not, freed. False when not every thread could be stopped. */
static void reference_work(void);

static bool collect(const char *reason, bool sweep, hk_dvm_gc_result *out)
{
    u64 t0, t1 = 0, t2 = 0, t3;
    u32 objects;
    void **stack;
    hk_jobj **found;
    int i;
    bool ok, clear_soft = !strcmp(reason, "out of memory");
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
    hk_lock_acquire(&g_final_lock);
    hk_dvm_heap_lock();
    /*
     * What marking needs, before anything is stopped: a mark stack with an entry for every object there is, a list for the
     * references found, and room for every finalizable object to be found unreachable.
     */
    objects = hk_dvm_heap_objects();
    stack = (void **)malloc(sizeof(void *) * ((size_t)objects + 64));
    found = (hk_jobj **)malloc(sizeof(hk_jobj *) * ((size_t)(u32)g_references + 64));
    if (g_cap_to_finalize < g_nto_finalize + g_nfinalizable) {
        hk_jobj **grown = (hk_jobj **)realloc(g_to_finalize, sizeof(*grown) * ((size_t)g_nto_finalize + g_nfinalizable + 64));
        if (grown) {
            g_to_finalize = grown;
            g_cap_to_finalize = g_nto_finalize + g_nfinalizable + 64;
        }
    }
    ok = stack != NULL && found != NULL;
    if (ok) {
        memset(&G, 0, sizeof(G));
        G.stack = stack;
        G.cap = objects + 64;
        G.found = found;
        G.cap_found = (u32)g_references + 64;
        g_nstopped = 0;
        g_stop_failed = false;
        hk_dvm_threads_each(stop_thread, NULL);
        t1 = hk_time_monotonic_ns();
        ok = !g_stop_failed;
        if (ok) {
            hk_dvm_heap_clear_marks();
            mark();
            /* A census decides nothing: what it found stays as it is. */
            if (sweep && g_referent >= 0) process_references(clear_soft);
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
    hk_lock_release(&g_final_lock);
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
    free(found);
    out->cleared = G.cleared;
    out->enqueued = G.enqueued;
    out->to_finalize = G.finalizable;
    hk_lock_release(&g_gc_lock);
    /* Work for the reference thread: wake it, and start it the first time. */
    if (ok && (g_pending_refs || g_nto_finalize)) reference_work();
    return ok;
}

bool hk_dvm_gc_census(hk_dvm_gc_result *out) { return collect("census", false, out); }

/*
 * The reference thread: a thread of the runtime's own, attached to Java as a daemon, that does what Java code must do after a
 * collection and the collector itself may not (it stops every thread, and runs on whichever one allocated): it hands the
 * cleared references to ReferenceQueue.enqueuePending (libcore's, which runs a Cleaner and enqueues the rest), and calls
 * finalize() on the objects found to finalize, catching what each throws. An object stays a root while it waits; after its
 * finalize() the next collection frees it, unless it made itself reachable again.
 */
static void reference_main(void *arg)
{
    hk_jvalue v;
    (void)arg;
    hk_dvm_self();
    hk_dvm_thread_attach("HeapTaskDaemon", NULL, true);
    for (;;) {
        hk_jobj *list, *o = NULL;
        hk_lock_acquire(&g_final_lock);
        while (!g_pending_refs && !g_nto_finalize) hk_cond_wait(&g_final_cond, &g_final_lock);
        list = g_pending_refs;
        g_pending_refs = NULL;
        if (!list && g_nto_finalize) {
            o = g_to_finalize[--g_nto_finalize];
            hk_atomic_add(&g_finalizing, 1);
        }
        hk_lock_release(&g_final_lock);
        if (list) {
            v.j = 0;
            v.l = list;
            if (!hk_dvm_call_static("java/lang/ref/ReferenceQueue", "enqueuePending", "(Ljava/lang/ref/Reference;)V", &v, NULL)) hk_dvm_clear_pending();
        }
        if (o) {
            if (!hk_dvm_call_virtual(o, "finalize", "()V", NULL, NULL)) hk_dvm_clear_pending();
            o = NULL;
            hk_lock_acquire(&g_final_lock);
            hk_atomic_add(&g_finalizing, -1);
            hk_cond_broadcast(&g_final_cond);
            hk_lock_release(&g_final_lock);
        }
    }
}

static void reference_work(void)
{
    hk_lock_acquire(&g_final_lock);
    if (!g_ref_thread) {
        g_ref_thread = hk_thread_start(reference_main, NULL, 1u << 20, "references");
        if (g_ref_thread) hk_thread_detach(g_ref_thread);
    }
    hk_cond_broadcast(&g_final_cond);
    hk_lock_release(&g_final_lock);
}

void hk_dvm_gc_run_finalization(void)
{
    u64 until = hk_time_monotonic_ns() + 10000000000ull;
    hk_lock_acquire(&g_final_lock);
    while ((g_nto_finalize || g_finalizing) && hk_time_monotonic_ns() < until) hk_cond_wait_ns(&g_final_cond, &g_final_lock, 100000000ull);
    hk_lock_release(&g_final_lock);
}

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
