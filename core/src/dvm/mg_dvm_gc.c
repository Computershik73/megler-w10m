/*
 * The collector: a non-moving mark and sweep that stops the world (docs/JAVA.md, "The collector"). It stops every other
 * thread of the runtime's where it is, follows references from the roots, lets the threads go on, and frees what it did
 * not reach (mg_dvm_gc); or only counts it (mg_dvm_gc_census). It runs on the allocating thread when enough has been made
 * since the last collection (the larger of 8 MB and half of what lived after it, native memory registered counted in), on
 * System.gc(), and before an allocation fails.
 *
 * Roots, and how each is read:
 *  - exactly: every linked class's static fields of reference type, its Class object and the error its initialisation
 *    threw; the interned strings (each DEX file's resolved string constants among them); each thread's pending exception, its
 *    java.lang.Thread and its scratch value; the proxies' method arrays; the JNI layer's global (and weak global)
 *    references, reflected methods and stand-in statics (mg_jni_gc_roots); the runtime's preallocated OutOfMemoryError and
 *    the app's class loader; and what C code registered (mg_dvm_add_root);
 *  - conservatively: every thread's frame stack (the interpreter's registers are untyped) and every thread's C stack from
 *    where it was stopped to its base, with the registers it was stopped with. A word that points into an object keeps it.
 * Inside objects it is exact: a class's reference slots (made at its first instance, mg_dvm_gc_class_ready), all of an
 * object array, nothing of a string or a primitive array, and the JNI layer's by-name fields marked as objects.
 *
 * Between the stop and the resume nothing is allocated, freed or logged: a stopped thread may hold the C library's heap lock
 * or the log's. The mark stack is allocated before the stop, as large as the heap has objects, so it cannot run out; should
 * it all the same, the overflow is finished by going over the heap for marked objects instead of allocating. The mark is
 * shared with helper threads of the collector's own once there is enough of it ("marking in parallel").
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "mg_dvm_internal.h"
#include "../mg_core.h"

/* ------------------------------------------------------------ the C roots */

/* (A Java app's GL threads take 32, the window's layers 8, native code's windows of SurfaceViews 48: 64 was not enough.) */
#define MAX_ROOTS 256
static mg_jobj **g_roots[MAX_ROOTS];
static volatile i32 g_nroots;

void mg_dvm_add_root(mg_jobj **slot)
{
    i32 i, n = g_nroots;
    for (i = 0; i < n; i++)
        if (g_roots[i] == slot) return;
    i = mg_atomic_add(&g_nroots, 1) - 1;
    if (i < MAX_ROOTS) g_roots[i] = slot;
    else mg_err("dvm: more than %d roots registered in C; %p is not one", MAX_ROOTS, (void *)slot);
}

/* --------------------------------------------------------- the class's slots */

void mg_dvm_gc_class_ready(mg_dvm_class *c)
{
    mg_dvm_class *k;
    u32 n = 0, *slots;
    int i;
    if (c->ref_slots_ready) return;
    for (k = c; k; k = k->super)
        for (i = 0; i < k->nifields; i++)
            if ((k->ifields[i].type[0] == 'L' || k->ifields[i].type[0] == '[') && k->ifields[i].slot < 0xFFFFFFF0u) n++;
    slots = n ? (u32 *)mg_alloc(sizeof(u32) * n) : NULL;
    n = 0;
    for (k = c; k; k = k->super)
        for (i = 0; i < k->nifields; i++)
            if ((k->ifields[i].type[0] == 'L' || k->ifields[i].type[0] == '[') && k->ifields[i].slot < 0xFFFFFFF0u) slots[n++] = k->ifields[i].slot;
    mg_dvm_lock();
    if (!c->ref_slots_ready) {
        mg_dvm_method *fin = mg_dvm_find_virtual(c, mg_dvm_intern("finalize"), mg_dvm_intern("()V"));
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
        mg_atomic_xchg(&c->ref_slots_ready, 1);
        slots = NULL;
    }
    mg_dvm_unlock();
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
    mg_dvm_class *r;
    mg_dvm_field *f;
    if (g_ref_slots_known) return;
    r = mg_dvm_class_exists("java/lang/ref/Reference") ? mg_dvm_find_class("java/lang/ref/Reference") : NULL;
    if (!r) mg_dvm_clear_pending();
    if (r && (f = mg_dvm_find_field(r, "referent", false)) != NULL) g_referent = (i32)f->slot;
    if (r && (f = mg_dvm_find_field(r, "pendingNext", false)) != NULL) g_pending_next = (i32)f->slot;
    if (r && (f = mg_dvm_find_field(r, "queue", false)) != NULL) g_queue = (i32)f->slot;
    mg_atomic_xchg(&g_ref_slots_known, 1);
}

/* Objects to finalize: those made (`g_finalizable`, not roots), and those found unreachable, waiting for their finalize() (roots). */
static mg_lock g_final_lock = MG_LOCK_INIT;
static mg_cond g_final_cond = MG_COND_INIT;
static mg_jobj **g_finalizable, **g_to_finalize;
static u32 g_nfinalizable, g_cap_finalizable, g_nto_finalize, g_cap_to_finalize;
static volatile i32 g_finalizing;      /* finalize() calls under way on the reference thread */
static volatile i32 g_references;      /* reference objects ever made: what the list of those found while marking may need */
/* What the last collections cleared and the reference thread is to hand to ReferenceQueue.enqueuePending: circular, through pendingNext. */
static mg_jobj *g_pending_refs;
static mg_thread *g_ref_thread;

void mg_dvm_gc_born(mg_jobj *o)
{
    mg_dvm_class *c = mg_dvm_class_of(o);
    if (c->gc_ref) {
        ref_slots_known();
        mg_atomic_add(&g_references, 1);
    }
    if (!c->finalizable) return;
    mg_lock_acquire(&g_final_lock);
    if (g_nfinalizable == g_cap_finalizable) {
        u32 cap = g_cap_finalizable ? g_cap_finalizable * 2 : 256;
        mg_jobj **grown = (mg_jobj **)realloc(g_finalizable, sizeof(*grown) * cap);
        if (grown) {
            g_finalizable = grown;
            g_cap_finalizable = cap;
        }
    }
    if (g_nfinalizable < g_cap_finalizable) g_finalizable[g_nfinalizable++] = o;
    mg_lock_release(&g_final_lock);
}

/* ---------------------------------------------------------------- marking */

/* One thread's marking: its stack of objects whose references are still to be followed, and what it marked. */
typedef struct marker {
    void **stack;
    u32 n, cap;
    u64 marked, marked_bytes, conservative, conservative_bytes;   /* objects marked; their bytes; marked first by a conservative root */
    bool counting_conservative;
    u32 classes, strings;              /* exact roots read: classes, interned strings */
} marker;

typedef struct gc_state {
    marker main;                       /* the collecting thread's, its stack with room for every object there is */
    bool overflow;
    mg_jobj **found;                   /* reference objects whose referent was not followed, to decide about after marking */
    volatile i32 nfound;
    u32 cap_found;
    u32 cleared, enqueued, finalizable;
    u32 markers;                       /* threads that marked at once, at the most */
    u64 roots_ns, stacks_ns;           /* the mark's time on the exact roots and on the conservative scan, the rest following */
    u64 classes_ns, strings_ns, jni_ns;
    u32 classes, strings, jni_roots;
    bool roots_shared;                 /* the classes and the interned strings read by all the threads (classes_ns their time with what they hold) */
} gc_state;

static gc_state G;
static volatile bool g_parallel;       /* the mark shared with the helpers right now: marks set with an interlocked exchange */
/*
 * Marking alongside the app (collect_alongside): marks set with an interlocked exchange (the app's threads make objects
 * marked meanwhile, in the same words), and the JNI layer's by-name fields of an object left for the second stop, where
 * every object that has them is followed again (they are C memory the layer may move as it adds one).
 */
static volatile bool g_alongside;
/*
 * The machine near the end of its memory: Play Store's heap on a Lumia 650 grew from 133 to 270 MB while a marking that
 * gave way to the app took 8.7 s, until Windows ended it with the phone's 927 MB gone. Then the markers keep their usual
 * priority, and one that makes a budget's worth while the marking goes on waits for it (budget_after keeps the budget
 * within what is free).
 *
 * Not the app's limit, which Windows 10 Mobile does not hold a foreground app to: TikTok Lite ran at 500 MB of a
 * "limit" of 390, and judged by three fifths of it, every collection was pressed and the budget the least, 8 MB: one
 * collection each 0.8 s, which freed 7 MB each of a heap that lived at 175 (package 619).
 */
static volatile bool g_pressed;
#define PRESSED_BELOW (160ull << 20) /* the machine's free memory under which the collector is pressed */
#define KEEP_FREE (96ull << 20)      /* what a budget leaves the machine at least */

/* Whether the machine has less than PRESSED_BELOW free; where the host does not say, whether the app uses more than three
   fifths of its limit (a test's probes); false where neither is known. */
static bool memory_pressed(void)
{
    const u64 available = mg_mem_available();
    if (available) return available < PRESSED_BELOW;
    {
        const u64 limit = mg_mem_limit(), used = mg_mem_usage();
        return limit && used > limit / 5 * 3;
    }
}

static void spill(marker *k);

/*
 * Whether the bytes marked are counted: by a collection that stops the app, whose budget is half of what it marked, and
 * by a census. One alongside the app does not count them (the heap says what lives), and so reads nothing of an object
 * it only pushes at a stop: each was a read of its own of memory nothing else was reading -- the classes' Class objects
 * and the interned strings among them, Spotify's 45,000 and 60,000 -- on a Lumia 650 whose memory was half compressed.
 */
static bool g_counting = true;

/* An object reached: marked, and its references to follow later. `o` is an object's start, or anything else (ignored: nothing is read of it before the heap has said it is an object). */
static void reach(marker *k, void *o)
{
    if (!o || !(g_parallel || g_alongside ? mg_dvm_heap_set_mark_shared(o) : mg_dvm_heap_set_mark(o))) return;
    k->marked++;
    if (k->counting_conservative) k->conservative++;
    if (g_counting) {
        k->marked_bytes += MG_DVM_OBJ(o)->size;
        if (k->counting_conservative) k->conservative_bytes += MG_DVM_OBJ(o)->size;
    }
    if (k->n == k->cap && k != &G.main) spill(k);
    if (k->n < k->cap) k->stack[k->n++] = o;
    else G.overflow = true;
}

/* A precise reference: an object of the runtime's, when the heap has it (one from before it started, or a C object, is not). */
static void reach_ref(mg_jobj *o, void *user)
{
    (void)user;
    reach(&G.main, o);
}

/* A word that may point into an object. */
static void reach_word(marker *k, mg_addr w) { reach(k, mg_dvm_heap_find(w)); }

/* A reference object whose referent is decided about after marking, kept in the list; false when it has no room left. */
static bool found_add(mg_jobj *r)
{
    i32 at = mg_atomic_add(&G.nfound, 1) - 1;
    if ((u32)at < G.cap_found) {
        G.found[at] = r;
        return true;
    }
    mg_atomic_add(&G.nfound, -1);
    return false;
}

static void scan_words(marker *k, mg_addr from, mg_addr to)
{
    mg_addr p;
    from = MG_ALIGN_UP(from, sizeof(mg_addr));
    for (p = from; p + sizeof(mg_addr) <= to; p += sizeof(mg_addr)) reach_word(k, *(const mg_addr *)p);
}

static void follow(marker *k, void *p)
{
    mg_dvm_obj *o = (mg_dvm_obj *)p;
    mg_jobj *j = &o->j;
    u32 i;
    if (j->kind == MG_J_OBJ_ARRAY) {
        /* (An array a stopped thread is making may have its length and not yet its elements: mg_dvm_alloc_array.) */
        if (j->u.oarr.v)
            for (i = 0; i < j->u.oarr.len; i++) reach(k, j->u.oarr.v[i]);
    } else if (j->kind != MG_J_PRIM_ARRAY && j->kind != MG_J_STRING && o->cls && o->cls->ref_slots_ready) {
        mg_jvalue *slots = mg_dvm_slots(j);
        bool reference = o->cls->gc_ref && g_referent >= 0 && g_pending_next >= 0;
        for (i = 0; i < o->cls->nref_slots; i++) {
            /* A reference's referent is not followed: it is decided about after marking (process_references). */
            if (reference && o->cls->ref_slots[i] == (u32)g_referent) continue;
            reach(k, slots[o->cls->ref_slots[i]].l);
        }
        if (reference && slots[g_referent].l) {
            /* One already waiting to be enqueued (pendingNext set) keeps its referent as it is; so does one the list has no room for. */
            if (slots[g_pending_next].l || !found_add(j)) reach(k, slots[g_referent].l);
        }
    }
    if (j->fields && !g_alongside)
        for (i = 0; i < (u32)j->nfields; i++)
            if (j->fields[i].is_object) reach(k, j->fields[i].v.l);
}

/* The runtime's objects given fields by name (mg_jni.c): their fields followed again at the second stop of marking alongside. */
static mg_lock g_fields_lock = MG_LOCK_INIT;
static mg_jobj **g_with_fields;
static u32 g_nwith_fields, g_cap_with_fields;

void mg_dvm_gc_has_fields(mg_jobj *o)
{
    u32 i;
    mg_lock_acquire(&g_fields_lock);
    for (i = 0; i < g_nwith_fields && g_with_fields[i] != o; i++) {}
    if (i == g_nwith_fields) {
        if (g_nwith_fields == g_cap_with_fields) {
            u32 cap = g_cap_with_fields ? g_cap_with_fields * 2 : 64;
            mg_jobj **grown = (mg_jobj **)realloc(g_with_fields, sizeof(*grown) * cap);
            if (grown) {
                g_with_fields = grown;
                g_cap_with_fields = cap;
            }
        }
        if (g_nwith_fields < g_cap_with_fields) g_with_fields[g_nwith_fields++] = o;
    }
    mg_lock_release(&g_fields_lock);
}

void mg_dvm_gc_fields_gone(mg_jobj *o)
{
    u32 i;
    mg_lock_acquire(&g_fields_lock);
    for (i = 0; i < g_nwith_fields; i++)
        if (g_with_fields[i] == o) {
            g_with_fields[i] = g_with_fields[--g_nwith_fields];
            break;
        }
    mg_lock_release(&g_fields_lock);
}

static void follow_main(void *p) { follow(&G.main, p); }
static void follow_by(void *p, void *k) { follow((marker *)k, p); }

/* ------------------------------------------------------ marking in parallel */

/*
 * The mark is shared with helper threads of the collector's own -- one fewer than the processors, three at the most --
 * once the collecting thread's stack holds enough to share: that stack becomes a pool, each thread takes a chunk of it at a
 * time into a stack of its own and follows references from there, gives half of its stack back when another thread has
 * nothing left (or its own is full), and the mark is done when every thread has nothing and the pool is empty. A mark is
 * set with an interlocked compare-and-swap meanwhile, so that each object is pushed by one thread. The helpers are not the
 * runtime's threads (nothing stops them), and neither allocate nor log: their stacks are made with them, before the first
 * collection. Yandex Maps' collections marked 770,000 objects in 325 ms on one thread, every five seconds. The flag
 * no-parallel-gc keeps it on the one.
 */
#define MAX_HELPERS (MG_DVM_GC_WORKERS - 1)
#define HELPER_STACK 16384u            /* entries in a marking thread's own stack */
#define PARALLEL_MIN 2048u             /* objects waiting before the mark is shared */
#define CHUNK 256u

static marker g_markers[MAX_HELPERS + 1];    /* [0]: the collecting thread's own while the mark is shared */
static int g_nhelpers;
static bool g_helpers_tried;
static mg_lock g_mark_lock = MG_LOCK_INIT;
static mg_cond g_mark_start = MG_COND_INIT, g_mark_work = MG_COND_INIT, g_mark_end = MG_COND_INIT;
static void **g_pool;                  /* the collecting thread's stack while shared; under g_mark_lock */
static volatile u32 g_npool;
static u32 g_pool_cap, g_generation, g_finished;
static int g_working, g_idle;          /* threads marking this time; of them, those with nothing to do */
static void (*g_round_fn)(void *arg, int worker);   /* what the helpers do this round (run_round) */
static void *g_round_arg;
static volatile i32 g_any_idle;
static bool g_done;

/* Half of a marking thread's stack to the pool, for the others (its own full, or another one idle). */
static void spill(marker *k)
{
    u32 half = k->n / 2;
    mg_lock_acquire(&g_mark_lock);
    if (g_npool + half > g_pool_cap) half = g_pool_cap - g_npool;
    memcpy(g_pool + g_npool, k->stack + k->n - half, sizeof(void *) * half);
    g_npool += half;
    k->n -= half;
    if (g_idle) mg_cond_broadcast(&g_mark_work);
    mg_lock_release(&g_mark_lock);
}

/* A thread's share of the mark: until every thread has nothing left and the pool is empty. */
static void mark_shared(marker *k)
{
    for (;;) {
        while (k->n) {
            void *o = k->stack[--k->n];
            follow(k, o);
            if (g_any_idle && !g_npool && k->n > 64) spill(k);
        }
        mg_lock_acquire(&g_mark_lock);
        for (;;) {
            if (g_npool) {
                u32 take = g_npool < CHUNK ? g_npool : CHUNK;
                g_npool -= take;
                memcpy(k->stack, g_pool + g_npool, sizeof(void *) * take);
                k->n = take;
                break;
            }
            if (g_done) break;
            g_idle++;
            mg_atomic_xchg(&g_any_idle, 1);
            if (g_idle == g_working) {
                g_done = true;
                mg_cond_broadcast(&g_mark_work);
                break;
            }
            mg_cond_wait(&g_mark_work, &g_mark_lock);
            g_idle--;
            if (!g_idle) mg_atomic_xchg(&g_any_idle, 0);
        }
        mg_lock_release(&g_mark_lock);
        if (!k->n) return;
    }
}

/*
 * The priority a marking thread takes for a round: while the app goes on beside it (marking alongside), the lowest, below
 * the app's own threads, which give way to the shell's (mg_app_threads_give_way) -- four markers had the four cores of an
 * HP Elite x3 for up to a second at each of TikTok Lite's collections, every ten seconds, and its frames stopped; with the
 * world stopped, the usual one, so that the stop is short. `level` the thread's own, as last set.
 */
static void marker_priority(int *level)
{
    const int want = g_alongside && !g_pressed ? -2 : 0;
    if (*level == want) return;
    *level = want;
    mg_thread_set_priority(NULL, want);
}

static MG_HOSTCB void helper_main(void *arg)
{
    int worker = (int)((marker *)arg - g_markers), level = 0;
    /*
     * From the first round: the helpers are made before any (helpers_start), and one that starts running late must still
     * take its part in it, which the others wait for.
     */
    u32 seen = 0;
    for (;;) {
        mg_lock_acquire(&g_mark_lock);
        while (g_generation == seen) mg_cond_wait(&g_mark_start, &g_mark_lock);
        seen = g_generation;
        mg_lock_release(&g_mark_lock);
        marker_priority(&level);
        g_round_fn(g_round_arg, worker);
        mg_lock_acquire(&g_mark_lock);
        g_finished++;
        mg_cond_signal(&g_mark_end);
        mg_lock_release(&g_mark_lock);
    }
}

/* The helpers, once, before any collection stops anything (they are made with their stacks). */
static void helpers_start(void)
{
    int want, i;
    if (g_helpers_tried) return;
    g_helpers_tried = true;
    want = mg_cpu_count() - 1;
    if (want > MAX_HELPERS) want = MAX_HELPERS;
    if (want < 1 || mg_flag("no-parallel-gc")) return;
    for (i = 0; i <= want; i++) {
        g_markers[i].stack = (void **)malloc(sizeof(void *) * HELPER_STACK);
        if (!g_markers[i].stack) break;
        g_markers[i].cap = HELPER_STACK;
        if (i) {
            mg_thread *th = mg_thread_start(helper_main, &g_markers[i], (size_t)64 << 10, "gc marker");
            if (!th) break;
            mg_thread_detach(th);
            g_nhelpers = i;
        }
    }
}

/* `fn` run by the collecting thread (worker 0) and every helper at once, until each has returned. */
static void run_round(void (*fn)(void *arg, int worker), void *arg)
{
    mg_lock_acquire(&g_mark_lock);
    g_round_fn = fn;
    g_round_arg = arg;
    g_finished = 0;
    g_generation++;
    mg_cond_broadcast(&g_mark_start);
    mg_lock_release(&g_mark_lock);
    fn(arg, 0);
    mg_lock_acquire(&g_mark_lock);
    while (g_finished < (u32)g_nhelpers) mg_cond_wait(&g_mark_end, &g_mark_lock);
    mg_lock_release(&g_mark_lock);
}

int mg_dvm_gc_share(void (*fn)(void *arg, int worker), void *arg)
{
    if (!g_nhelpers) {
        fn(arg, 0);
        return 1;
    }
    run_round(fn, arg);
    return g_nhelpers + 1;
}

static void mark_round(void *arg, int worker)
{
    (void)arg;
    mark_shared(&g_markers[worker]);
}

/*
 * The exact roots of the classes and the interned strings, shared: the class table's buckets and the interned strings'
 * slots in parts, each thread taking the next part and marking into its own stack, then the mark going on shared
 * (mark_shared). On the one thread they were Yandex Maps' 55,000 classes in 23 ms and 42,000 strings in 12, of a 140 ms
 * mark (package 528).
 */
#define ROOT_BUCKETS 1024u
#define ROOT_SLOTS 2048u
static volatile i32 g_root_next;
static u32 g_root_class_parts, g_root_string_parts;

static void class_roots(mg_dvm_class *c, void *user);
static void string_root(mg_jobj *s, void *user);

/* The round pushes the roots and follows nothing (class_roots_pushed): each thread's stack goes to the pool. */
static bool g_roots_only;

/* What a thread pushed, nothing followed, to the pool (the rounds that push roots alone). */
static void pool_put(marker *k)
{
    mg_lock_acquire(&g_mark_lock);
    if (g_npool + k->n <= g_pool_cap) {
        memcpy(g_pool + g_npool, k->stack, sizeof(void *) * k->n);
        g_npool += k->n;
    } else {
        G.overflow = true;
    }
    k->n = 0;
    mg_lock_release(&g_mark_lock);
}

static void roots_round(void *arg, int worker)
{
    marker *k = &g_markers[worker];
    (void)arg;
    for (;;) {
        u32 p = (u32)(mg_atomic_add(&g_root_next, 1) - 1);
        if (p < g_root_class_parts) {
            mg_dvm_each_linked_part(p * ROOT_BUCKETS, (p + 1) * ROOT_BUCKETS, class_roots, k);
        } else if (p < g_root_class_parts + g_root_string_parts) {
            p -= g_root_class_parts;
            mg_dvm_interned_part(p * ROOT_SLOTS, (p + 1) * ROOT_SLOTS, string_root, k);
        } else {
            break;
        }
    }
    if (g_roots_only) {
        pool_put(k);
        return;
    }
    mark_shared(k);
}

/* A round (mark_round, roots_round) by the collecting thread and the helpers, the collecting thread's stack their pool. */
static void shared(void (*round)(void *arg, int worker))
{
    int i;
    g_pool = G.main.stack;
    g_npool = G.main.n;
    g_pool_cap = G.main.cap;
    G.main.n = 0;
    for (i = 0; i <= g_nhelpers; i++) {
        marker *k = &g_markers[i];
        k->n = 0;
        k->marked = k->marked_bytes = k->conservative = k->conservative_bytes = 0;
        k->classes = k->strings = 0;
        k->counting_conservative = G.main.counting_conservative;
    }
    g_parallel = true;
    mg_lock_acquire(&g_mark_lock);
    g_working = g_nhelpers + 1;
    g_idle = 0;
    g_any_idle = 0;
    g_done = false;
    mg_lock_release(&g_mark_lock);
    run_round(round, NULL);
    g_parallel = false;
    for (i = 0; i <= g_nhelpers; i++) {
        marker *k = &g_markers[i];
        G.main.marked += k->marked;
        G.main.marked_bytes += k->marked_bytes;
        G.main.conservative += k->conservative;
        G.main.conservative_bytes += k->conservative_bytes;
        G.main.classes += k->classes;
        G.main.strings += k->strings;
    }
    G.markers = (u32)g_nhelpers + 1;
}

/* The collecting thread's stack drained by it and the helpers together. */
static void drain_shared(void) { shared(mark_round); }

static void drain(void)
{
    if (g_nhelpers && G.main.n >= PARALLEL_MIN) drain_shared();
    while (G.main.n) follow(&G.main, G.main.stack[--G.main.n]);
}

/* What the classes hold: statics of reference type, the Class object, the error of a failed initialisation. */
static void class_roots(mg_dvm_class *c, void *user)
{
    marker *k = user ? (marker *)user : &G.main;
    int i;
    k->classes++;
    reach(k, c->mirror);
    reach(k, c->init_error);
    if (!c->statics) return;
    /* (The static slots of references come first: mg_dvm_class.c.) */
    for (i = 0; i < c->nsrefs; i++) reach(k, c->statics[i].l);
}

static void string_root(mg_jobj *s, void *user)
{
    marker *k = user ? (marker *)user : &G.main;
    k->strings++;
    reach(k, s);
}

static void exact_thread_roots(mg_dvm_thread *t, void *user)
{
    (void)user;
    reach_ref(t->pending, NULL);
    reach_ref(t->peer, NULL);
    reach_ref(t->waiting_monitor, NULL);
    reach_word(&G.main, (mg_addr)t->scratch.l);
}

/*
 * The threads stopped for this collection, with their registers. They are listed first, then stopped -- and resumed after
 * -- by the collecting thread and its helpers at once, each taking the next one: stopping Yandex Maps' 250 threads one
 * after another took 10 to 50 ms of every pause (package 526), a wait for each to be stopped.
 */
#define MAX_STOPPED 1024
static struct {
    mg_dvm_thread *t;
    mg_thread_regs regs;
} g_stopped[MAX_STOPPED];
static int g_nstopped;
static volatile i32 g_stop_next;
static mg_dvm_thread *g_collector;
static bool g_stop_failed;

static void list_thread(mg_dvm_thread *t, void *user)
{
    (void)user;
    if (t == g_collector) return;
    /* A thread that cannot be stopped or whose stack cannot be scanned: no collection, rather than one that misses its roots. */
    if (!t->host || !t->stack_hi || g_nstopped == MAX_STOPPED) {
        g_stop_failed = true;
        return;
    }
    g_stopped[g_nstopped++].t = t;
}

static void stop_some(void *arg, int worker)
{
    (void)arg;
    (void)worker;
    for (;;) {
        i32 i = mg_atomic_add(&g_stop_next, 1) - 1;
        if (i >= g_nstopped) return;
        /* Gone: a thread that ended without the host's hook (one the host did not start). Nothing of it is scanned. */
        if (!mg_thread_suspend(g_stopped[i].t->host, &g_stopped[i].regs)) g_stopped[i].t = NULL;
    }
}

static void resume_some(void *arg, int worker)
{
    (void)arg;
    (void)worker;
    for (;;) {
        i32 i = mg_atomic_add(&g_stop_next, 1) - 1;
        if (i >= g_nstopped) return;
        mg_thread_resume(g_stopped[i].t->host);
    }
}

/* Every thread of the runtime's but this one stopped, those gone left out; none when one of them cannot be. */
static void stop_threads(void)
{
    int i, kept = 0;
    g_nstopped = 0;
    g_stop_failed = false;
    mg_dvm_threads_each(list_thread, NULL);
    if (g_stop_failed) {
        g_nstopped = 0;
        return;
    }
    g_stop_next = 0;
    mg_dvm_gc_share(stop_some, NULL);
    for (i = 0; i < g_nstopped; i++)
        if (g_stopped[i].t) g_stopped[kept++] = g_stopped[i];
    g_nstopped = kept;
}

static void resume_threads(void)
{
    g_stop_next = 0;
    mg_dvm_gc_share(resume_some, NULL);
}

/*
 * A thread's frame stack, the registers of every frame, and a margin above the top one for a frame being made: the segments
 * below the current one to where their frames end, and the current one. (Stopped while it moved from one segment to the
 * next, a thread has some of a segment scanned twice, or more than its frames: words, no harm.)
 */
static void scan_frames(mg_dvm_thread *t, void *user)
{
    marker *k = user ? (marker *)user : &G.main;
    mg_dvm_reg *top;
    int i;
    if (!t->stack) return;
    for (i = 0; i < t->nsegs && i < DVM_STACK_SEGS; i++) {
        mg_dvm_reg *base = t->segs[i].base, *end = t->segs[i].sp;
        if (base && end > base && end <= base + t->segs[i].committed) scan_words(k, (mg_addr)base, (mg_addr)end);
    }
    top = t->sp + 64 < t->stack + t->stack_committed ? t->sp + 64 : t->stack + t->stack_committed;
    if (top > t->stack) scan_words(k, (mg_addr)t->stack, (mg_addr)top);
}

/* The parts still in use of the stacks a thread went on from (mg_dvm_invoke's on_more_stack): each from where it left it to its top. */
static void outer_stacks_scanned(marker *k, mg_dvm_thread *t)
{
    int j, n = t->nouter;
    for (j = 0; j < n && j < DVM_C_STACKS; j++)
        if (t->outer_hi[j] > t->outer_lo[j]) scan_words(k, t->outer_lo[j], t->outer_hi[j]);
}

static MG_NOINLINE void scan_own_stack(void)
{
    volatile mg_addr here = 0;
    mg_dvm_thread *t = g_collector;
    if (t && t->stack_hi) scan_words(&G.main, (mg_addr)&here, t->stack_hi);
    if (t) outer_stacks_scanned(&G.main, t);
}

/* A stopped thread's registers, and its C stack from where it was stopped to its base (and the stacks it went on from). */
static void stopped_scanned(marker *k, int i)
{
    const mg_thread_regs *r = &g_stopped[i].regs;
    mg_dvm_thread *t = g_stopped[i].t;
    mg_addr lo = t->stack_lo, hi = t->stack_hi;
    int j;
    for (j = 0; j < r->n; j++) reach_word(k, r->r[j]);
    if (r->sp >= lo && r->sp < hi) scan_words(k, r->sp, hi);
    outer_stacks_scanned(k, t);
}

/*
 * The scans shared, where there are helpers: each thread takes the next stopped thread's registers and C stack, or the next
 * thread's frame stack, and pushes what the words point to, nothing followed (as class_roots_pushed). On the collecting
 * thread alone, the 162 threads of Spotify's process (Google Play services' among them) took 18.8 ms of a collection that
 * stopped it; one alongside the app scans them at both its stops, and the threads were 220 (package 664).
 */
static mg_dvm_thread *g_frames[MAX_STOPPED + 1];
static int g_nframes;
static volatile i32 g_scan_next;

static void frames_listed(mg_dvm_thread *t, void *user)
{
    if (g_nframes < (int)MG_COUNT(g_frames)) g_frames[g_nframes++] = t;
    else *(bool *)user = true;
}

static void stacks_round(void *arg, int worker)
{
    marker *k = &g_markers[worker];
    (void)arg;
    for (;;) {
        i32 p = mg_atomic_add(&g_scan_next, 1) - 1;
        if (p < g_nstopped) stopped_scanned(k, p);
        else if (p < g_nstopped + g_nframes) scan_frames(g_frames[p - g_nstopped], k);
        else break;
    }
    pool_put(k);
}

static void conservative_roots(void)
{
    int i;
    jmp_buf registers;
    bool over = false;
    /* The collector's own registers, onto its stack, and its stack from here to its base. */
    if (setjmp(registers) == 0) scan_own_stack();
    if (g_nhelpers) {
        g_nframes = 0;
        mg_dvm_threads_each(frames_listed, &over);
        if (!over) {
            g_scan_next = 0;
            shared(stacks_round);
            G.main.n = g_npool;
            return;
        }
    }
    for (i = 0; i < g_nstopped; i++) stopped_scanned(&G.main, i);
    mg_dvm_threads_each(scan_frames, NULL);
}

static void heap_rescan_marked(void);

/* A cleared reference with a queue joins the circular list the reference thread will hand to ReferenceQueue.enqueuePending. */
static void enqueue(mg_jobj *r)
{
    mg_jvalue *slots = mg_dvm_slots(r), *head;
    if (g_queue < 0 || !slots[g_queue].l) return;
    if (!g_pending_refs) {
        slots[g_pending_next].l = r;
        g_pending_refs = r;
    } else {
        /* Into the ring, after its head. */
        head = mg_dvm_slots(g_pending_refs);
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
        for (i = 0; i < (u32)G.nfound; i++)
            if (mg_dvm_class_of(G.found[i])->gc_ref == 1) reach_ref(mg_dvm_slots(G.found[i])[g_referent].l, NULL);
        drain();
    }
    for (i = 0; i < (u32)G.nfound; i++) {
        mg_jobj *r = G.found[i];
        mg_jvalue *slots = mg_dvm_slots(r);
        if (mg_dvm_class_of(r)->gc_ref == 3 || !slots[g_referent].l || mg_dvm_heap_is_marked(slots[g_referent].l)) continue;
        slots[g_referent].l = NULL;
        G.cleared++;
        enqueue(r);
    }
    /* Finalizable objects nothing reached: to finalize, and so reached (with what they reach) until that is done. */
    for (i = 0; i < g_nfinalizable;) {
        mg_jobj *o = g_finalizable[i];
        if (mg_dvm_heap_is_marked(o) || g_nto_finalize == g_cap_to_finalize) {
            i++;
            continue;
        }
        g_to_finalize[g_nto_finalize++] = o;
        g_finalizable[i] = g_finalizable[--g_nfinalizable];
        G.finalizable++;
        reach_ref(o, NULL);
    }
    drain();
    for (i = 0; i < (u32)G.nfound; i++) {
        mg_jobj *r = G.found[i];
        mg_jvalue *slots = mg_dvm_slots(r);
        if (mg_dvm_class_of(r)->gc_ref != 3 || !slots[g_referent].l || mg_dvm_heap_is_marked(slots[g_referent].l)) continue;
        slots[g_referent].l = NULL;
        G.cleared++;
        enqueue(r);
    }
}

/* Everything reachable marked; the counts in G. With the runtime's locks held and the other threads stopped. */
/* A root of a kind counted apart (G.strings, G.jni_roots, by `user`). */
static void reach_counted(mg_jobj *o, void *user)
{
    (*(u32 *)user)++;
    reach(&G.main, o);
}

static void mark(void)
{
    u32 i;
    u64 t0 = mg_time_monotonic_ns(), t1, t2;
    /*
     * The classes and the interned strings, among them every DEX file's string constants resolved (mg_dvm_const_string keeps
     * the interned copy: those files' tables of them, a word for every string they have, are not walked). With helpers
     * they are shared, after the other roots ("the exact roots ... shared").
     */
    G.roots_shared = g_nhelpers > 0;
    if (!G.roots_shared) {
        mg_dvm_each_linked_locked(class_roots, NULL);
        t1 = mg_time_monotonic_ns();
        G.classes_ns = t1 - t0;
        mg_dvm_interned_each(string_root, NULL);
        t2 = mg_time_monotonic_ns();
        G.strings_ns = t2 - t1;
    }
    mg_dvm_threads_each(exact_thread_roots, NULL);
    mg_dvm_proxy_roots(reach_ref, NULL);
    mg_dvm_object_roots(reach_ref, NULL);
    t1 = mg_time_monotonic_ns();
    mg_jni_gc_roots(reach_counted, &G.jni_roots);
    G.jni_ns = mg_time_monotonic_ns() - t1;
    for (i = 0; i < (u32)g_nroots && i < MAX_ROOTS; i++)
        if (g_roots[i]) reach_ref(*g_roots[i], NULL);
    /* What waits for the reference thread: objects to finalize, and the references it has not handed on yet. */
    for (i = 0; i < g_nto_finalize; i++) reach_ref(g_to_finalize[i], NULL);
    reach_ref(g_pending_refs, NULL);
    G.roots_ns = mg_time_monotonic_ns() - t0;
    if (G.roots_shared) {
        t1 = mg_time_monotonic_ns();
        g_root_next = 0;
        g_root_class_parts = (mg_dvm_class_buckets() + ROOT_BUCKETS - 1) / ROOT_BUCKETS;
        g_root_string_parts = (mg_dvm_interned_slots() + ROOT_SLOTS - 1) / ROOT_SLOTS;
        shared(roots_round);
        G.classes_ns = mg_time_monotonic_ns() - t1;
    }
    G.classes = G.main.classes;
    G.strings = G.main.strings;
    drain();
    /* What only the conservative scans find, counted apart: the next phone log says how much that is. */
    G.main.counting_conservative = true;
    t1 = mg_time_monotonic_ns();
    conservative_roots();
    G.stacks_ns = mg_time_monotonic_ns() - t1;
    drain();
    G.main.counting_conservative = false;
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
        before = G.main.marked;
        mg_dvm_heap_each_marked(follow_main);
        drain();
    } while (G.main.marked != before || G.overflow);
}

void mg_dvm_gc_thread_gone(mg_dvm_thread *t) { (void)t; }

/* ---------------------------------------------------------- a collection */

static mg_lock g_gc_lock = MG_LOCK_INIT;
static MG_THREAD_LOCAL bool t_collecting;
/* The thread of collections alongside the app (cycle_main), and its priority as marker_priority last set it. */
static MG_THREAD_LOCAL bool t_cycle;
static MG_THREAD_LOCAL int t_cycle_level;
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
static volatile i32 g_busy;

bool mg_dvm_gc_busy(void) { return g_busy != 0; }

void mg_dvm_gc_enable(bool on) { g_enabled = on; }
void mg_dvm_gc_set_poison(bool on) { g_poison = on; }

void mg_dvm_gc_set_min_budget(u64 bytes)
{
    g_min_budget = bytes;
    g_budget = bytes;
}

u64 mg_dvm_gc_budget(void) { return g_budget; }

void mg_dvm_gc_totals(u64 *collections, u64 *pause_ns)
{
    if (collections) *collections = (u64)(u32)g_collections;
    if (pause_ns) *pause_ns = g_pause_ns;
}

static void count(volatile i32 *sum, u64 bytes)
{
    if (bytes > 0x40000000u) bytes = 0x40000000u;
    if (mg_atomic_add(sum, (i32)bytes) < 0) mg_atomic_xchg(sum, 0x7FFFFFFF);
}

void mg_dvm_gc_native(i64 bytes)
{
    /* Only what grows counts toward the next collection; what is freed is freed. */
    if (bytes > 0) count(&g_native_since, (u64)bytes);
}

static bool due(void) { return (u64)(u32)g_since + (u64)(u32)g_native_since >= g_budget; }

/*
 * What may be made before the next collection, `live` having lived through this one: half of it, at least the least; and no
 * more than half of what the machine has free beyond KEEP_FREE (g_pressed) -- where the host does not say, a third of what
 * is left of the app's limit.
 */
static u64 budget_after(u64 live)
{
    const u64 available = mg_mem_available();
    u64 budget = live / 2 > g_min_budget ? live / 2 : g_min_budget, room;
    if (available) {
        room = available > KEEP_FREE ? (available - KEEP_FREE) / 2 : 0;
    } else {
        const u64 limit = mg_mem_limit(), used = mg_mem_usage();
        if (!limit) return budget;
        room = used < limit ? (limit - used) / 3 : 0;
    }
    if (budget > room) budget = room > g_min_budget ? room : g_min_budget;
    return budget;
}

bool mg_dvm_gc_counted(size_t bytes)
{
    if (!g_enabled || !g_dvm.running || t_collecting) return false;
    count(&g_since, bytes);
    return due();
}

/*
 * The threads that made the most since the last collection, with the world stopped: the line says whose objects the
 * collections are for. Yandex Maps' made some 50 MB every 3 s, a collection of 200 ms each time, and its main thread made
 * a few of them (package 533).
 */
#define MAKERS 3
typedef struct makers {
    mg_dvm_thread *t[MAKERS];
    u64 bytes[MAKERS], all;
} makers;

static void note_maker(mg_dvm_thread *t, void *user)
{
    makers *k = (makers *)user;
    u64 made = t->made_bytes - t->made_at_gc;
    int i, j;
    t->made_at_gc = t->made_bytes;
    k->all += made;
    for (i = 0; i < MAKERS && (k->t[i] && k->bytes[i] >= made); i++);
    if (i == MAKERS || !made) return;
    for (j = MAKERS - 1; j > i; j--) {
        k->t[j] = k->t[j - 1];
        k->bytes[j] = k->bytes[j - 1];
    }
    k->t[i] = t;
    k->bytes[i] = made;
}

static void say_makers(char *out, size_t n)
{
    makers k;
    size_t at = 0;
    int i;
    memset(&k, 0, sizeof(k));
    out[0] = 0;
    mg_dvm_threads_each(note_maker, &k);
    for (i = 0; i < MAKERS && k.t[i] && at + 60 < n; i++) {
        char name[48];
        mg_dvm_thread_label(k.t[i], name, sizeof(name));
        at += (size_t)snprintf(out + at, n - at, "%s%s %.1f MB", i ? ", " : "", name, (double)k.bytes[i] / 1048576.0);
    }
    if (at && at + 30 < n) snprintf(out + at, n - at, " (of %.1f MB)", (double)k.all / 1048576.0);
}

/* The world stopped, everything reachable marked; `sweep`: and what is not, freed. False when not every thread could be stopped. */
static void reference_work(void);

static u64 g_large_said_ns;

static bool cycle_busy(void);

static bool collect(const char *reason, bool sweep, mg_dvm_gc_result *out)
{
    char large[600];
    u64 t0, t1 = 0, t2 = 0, t3;
    u32 objects;
    void **stack;
    mg_jobj **found;
    bool ok, clear_soft = !strcmp(reason, "out of memory");
    large[0] = 0;
    memset(out, 0, sizeof(*out));
    if (!g_dvm.running || t_collecting) return false;
    /*
     * A collection alongside the app under way takes the runtime's lock at its stops: a thread that holds it does not wait
     * for that collection (which holds the collector's lock throughout), nothing collected.
     */
    if (cycle_busy() && mg_dvm_lock_mine()) return false;
    mg_lock_acquire(&g_gc_lock);
    /* (Under the collector's lock: two threads that collect at once for the first time start one set of helpers.) */
    helpers_start();
    /* Two threads that found it due at once: the second has nothing left to do. */
    if (sweep && !strcmp(reason, "allocation") && !due()) {
        mg_lock_release(&g_gc_lock);
        return true;
    }
    t_collecting = true;
    g_busy = 1;
    g_counting = true;
    t0 = mg_time_monotonic_ns();
    g_collector = mg_dvm_self();
    mg_dvm_lock();
    mg_jni_gc_lock(true);
    mg_lock_acquire(&g_final_lock);
    mg_dvm_heap_lock();
    /*
     * What marking needs, before anything is stopped: a mark stack with an entry for every object there is, a list for the
     * references found, and room for every finalizable object to be found unreachable.
     */
    objects = mg_dvm_heap_objects();
    stack = (void **)malloc(sizeof(void *) * ((size_t)objects + 64));
    found = (mg_jobj **)malloc(sizeof(mg_jobj *) * ((size_t)(u32)g_references + 64));
    if (g_cap_to_finalize < g_nto_finalize + g_nfinalizable) {
        mg_jobj **grown = (mg_jobj **)realloc(g_to_finalize, sizeof(*grown) * ((size_t)g_nto_finalize + g_nfinalizable + 64));
        if (grown) {
            g_to_finalize = grown;
            g_cap_to_finalize = g_nto_finalize + g_nfinalizable + 64;
        }
    }
    ok = stack != NULL && found != NULL;
    if (ok) {
        memset(&G, 0, sizeof(G));
        G.main.stack = stack;
        G.main.cap = objects + 64;
        G.markers = 1;
        G.found = found;
        G.cap_found = (u32)g_references + 64;
        stop_threads();
        t1 = mg_time_monotonic_ns();
        ok = !g_stop_failed;
        if (ok) {
            if (sweep) say_makers(out->makers, sizeof(out->makers));
            mg_dvm_heap_clear_marks();
            mark();
            /* A census decides nothing: what it found stays as it is. */
            if (sweep && g_referent >= 0) process_references(clear_soft);
        }
        resume_threads();
        t2 = mg_time_monotonic_ns();
        out->objects = objects;
        out->live_objects = G.main.marked;
        out->live_bytes = G.main.marked_bytes;
        out->conservative = G.main.conservative;
        out->conservative_bytes = G.main.conservative_bytes;
        out->markers = G.markers;
        out->roots_ns = G.roots_ns;
        out->stacks_ns = G.stacks_ns;
        out->classes_ns = G.classes_ns;
        out->strings_ns = G.strings_ns;
        out->jni_ns = G.jni_ns;
        out->classes = G.classes;
        out->strings = G.strings;
        out->jni_roots = G.jni_roots;
        out->roots_shared = G.roots_shared;
        out->threads_stopped = (u32)g_nstopped;
        out->stop_ns = t1 - t0;
        out->mark_ns = t2 - t1;
    }
    mg_lock_release(&g_final_lock);
    mg_jni_gc_lock(false);
    mg_dvm_unlock();
    /* The others go on; the heap stays locked while what nothing reached is freed, so nothing is made meanwhile. */
    if (ok && sweep) {
        out->sweepers = mg_dvm_heap_sweep(mg_dvm_object_dying, g_poison, &out->freed_objects, &out->freed_bytes);
        t3 = mg_time_monotonic_ns();
        out->sweep_ns = t3 - t2;
        /*
         * Half what lived: three quarters had Play Store with Google Play services at 350 MB of heap before each collection,
         * for 200 that lived, in a process whose 2 GB the WebView needs too.
         */
        g_budget = budget_after(out->live_bytes);
        mg_atomic_xchg(&g_since, 0);
        mg_atomic_xchg(&g_native_since, 0);
        /* What the large objects are, when they are many (at most once in 30 s): Play Store's took 156 MB. */
        if (t3 - g_large_said_ns > 30000000000ull && mg_dvm_heap_large_report(large, sizeof(large), 6)) g_large_said_ns = t3;
    }
    mg_dvm_heap_unlock();
    g_busy = 0;
    if (large[0]) mg_info("gc: the large objects (over 2 KB each) by class: %s", large);
    if (ok) {
        mg_atomic_add(&g_collections, 1);
        g_pause_ns += out->stop_ns + out->mark_ns + out->sweep_ns;
    }
    g_collector = NULL;
    t_collecting = false;
    free(stack);
    free(found);
    out->cleared = G.cleared;
    out->enqueued = G.enqueued;
    out->to_finalize = G.finalizable;
    mg_lock_release(&g_gc_lock);
    /* Work for the reference thread: wake it, and start it the first time. */
    if (ok && (g_pending_refs || g_nto_finalize)) reference_work();
    return ok;
}

bool mg_dvm_gc_census(mg_dvm_gc_result *out)
{
    mg_dvm_gc_alongside_wait();
    return collect("census", false, out);
}

/* ------------------------------------------------- marking alongside the app */

/*
 * A collection due to allocation marks alongside the app: the world stops twice, briefly, and the marking between is done by
 * the collector's own thread and its helpers while the app runs. Yandex Maps' collections stopped it for 150 to 250 ms
 * every few seconds while its map was moved, most of it marking 700,000 objects (package 537).
 *
 *  - Just before it, the app running: the arenas' write watch started again (mg_dvm_heap_watch_restart).
 *  - The first stop: the marks cleared (unless a sweep left them so), new objects made marked from now on (they live:
 *    mg_dvm_heap_alloc_black), and every root pushed -- the classes' statics, the interned strings, the threads', the JNI
 *    layer's, C's, the frames and C stacks scanned conservatively -- nothing followed yet.
 *  - Alongside: everything they reach marked, by the helpers too, while the app's threads run and make objects.
 *  - The second stop: every root pushed again (the interned strings excepted: roots_pushed), and every marked object on a
 *    page of the heap written since the first stop followed again -- an app's thread that stored a reference into an object
 *    already followed wrote that page -- with every
 *    marked large object and object with fields by name (memory nothing watches), then everything that reaches marked; the
 *    references decided about, as in a stopped collection. Then the sweep, the heap locked, as before.
 *
 * Where the host does not watch pages for writes the second stop takes every page for written: correct, and a stop as long
 * as a stopped collection's (the tests turn it on so). A collection that cannot stop every thread at its second stop is
 * given up, nothing freed.
 */
static volatile i32 g_alongside_on = -1;     /* -1: not decided yet (the heap's watch, the flag no-concurrent-gc) */
static mg_lock g_cycle_lock = MG_LOCK_INIT;
static mg_cond g_cycle_wake = MG_COND_INIT, g_cycle_done = MG_COND_INIT;
static volatile bool g_cycle_asked, g_cycle_running;
static bool g_cycle_thread;
static u64 g_cycles;

void mg_dvm_gc_set_alongside(bool on) { mg_atomic_xchg(&g_alongside_on, on ? 1 : 0); }

static bool alongside_on(void)
{
    /* (MEGLER_GC_ALONGSIDE=1 turns it on where nothing is watched: the tests' whole runs so.) */
    if (g_alongside_on < 0) {
        const char *forced = getenv("MEGLER_GC_ALONGSIDE");
        mg_atomic_xchg(&g_alongside_on, (forced && forced[0] == '1') || (mg_dvm_heap_watched() && !mg_flag("no-concurrent-gc")) ? 1 : 0);
    }
    return g_alongside_on > 0;
}

/* Every root, pushed (not followed): the classes and the interned strings, the other exact roots, the conservative scans. */
/*
 * The classes' statics and the interned strings pushed by the collecting thread and the helpers at once, nothing followed,
 * into the collecting thread's stack. One thread pushing them took some 40 ms of each stop of Yandex Maps' (package 539).
 */
static void class_roots_pushed(bool strings)
{
    if (!g_nhelpers) {
        mg_dvm_each_linked_locked(class_roots, NULL);
        if (strings) mg_dvm_interned_each(string_root, NULL);
        return;
    }
    g_root_next = 0;
    g_root_class_parts = (mg_dvm_class_buckets() + ROOT_BUCKETS - 1) / ROOT_BUCKETS;
    g_root_string_parts = strings ? (mg_dvm_interned_slots() + ROOT_SLOTS - 1) / ROOT_SLOTS : 0;
    g_roots_only = true;
    shared(roots_round);
    g_roots_only = false;
    G.main.n = g_npool;
}

/* What a stop's roots took, for the line of a collection alongside the app. */
typedef struct roots_took {
    u64 classes_ns, exact_ns, jni_ns, stacks_ns;
} roots_took;

/*
 * Every root pushed, nothing followed. `strings`: the interned strings too -- at the first stop; at the second every string
 * the table holds is marked already (one put in meanwhile is marked as it is put, mg_dvm_intern_string), and reading the
 * table again went over Spotify's 60,000 strings and the slots between them for nothing (package 664).
 */
static void roots_pushed(roots_took *took, bool strings)
{
    u64 t0 = mg_time_monotonic_ns(), t1, t2;
    u32 i;
    class_roots_pushed(strings);
    t1 = mg_time_monotonic_ns();
    mg_dvm_threads_each(exact_thread_roots, NULL);
    mg_dvm_proxy_roots(reach_ref, NULL);
    mg_dvm_object_roots(reach_ref, NULL);
    t2 = mg_time_monotonic_ns();
    mg_jni_gc_roots(reach_counted, &G.jni_roots);
    took->jni_ns = mg_time_monotonic_ns() - t2;
    for (i = 0; i < (u32)g_nroots && i < MAX_ROOTS; i++)
        if (g_roots[i]) reach_ref(*g_roots[i], NULL);
    for (i = 0; i < g_nto_finalize; i++) reach_ref(g_to_finalize[i], NULL);
    reach_ref(g_pending_refs, NULL);
    t2 = mg_time_monotonic_ns();
    took->classes_ns = t1 - t0;
    took->exact_ns = t2 - t1;
    G.main.counting_conservative = true;
    conservative_roots();
    G.main.counting_conservative = false;
    took->stacks_ns = mg_time_monotonic_ns() - t2;
}

/* The runtime's locks the collection holds while the world is stopped, so that no stopped thread holds one. */
static void locks_take(void)
{
    mg_dvm_lock();
    mg_jni_gc_lock(true);
    mg_lock_acquire(&g_final_lock);
    mg_dvm_heap_lock();
}

static void locks_give(bool heap_too)
{
    mg_lock_release(&g_final_lock);
    mg_jni_gc_lock(false);
    mg_dvm_unlock();
    if (heap_too) mg_dvm_heap_unlock();
}

/*
 * The second stop's written pages shared: each thread takes the next few pages, follows the marked objects on them into its
 * own stack, and then marks with the others from there (mark_shared). What the app made while the marking went on is on
 * such pages, made marked, and is followed there: on the collecting thread alone, the 12,000 pages Spotify wrote while its
 * first collections marked took 270 to 335 ms of their second stops on a Lumia 650 (package 665).
 */
#define WRITTEN_CHUNK 8
static u8 **g_written;
static u32 g_nwritten;
static volatile i32 g_written_next;

static void written_round(void *arg, int worker)
{
    marker *k = &g_markers[worker];
    (void)arg;
    for (;;) {
        u32 at = (u32)(mg_atomic_add(&g_written_next, WRITTEN_CHUNK) - WRITTEN_CHUNK), end, i;
        if (at >= g_nwritten) break;
        end = at + WRITTEN_CHUNK < g_nwritten ? at + WRITTEN_CHUNK : g_nwritten;
        for (i = at; i < end; i++) mg_dvm_heap_marked_on(g_written[i], follow_by, k);
    }
    mark_shared(k);
}

static void follow_with_fields(void)
{
    u32 i;
    mg_lock_acquire(&g_fields_lock);
    for (i = 0; i < g_nwith_fields; i++)
        if (mg_dvm_heap_is_marked(g_with_fields[i])) follow(&G.main, g_with_fields[i]);
    mg_lock_release(&g_fields_lock);
}

static void collect_alongside(void)
{
    /* (tw the write watch started again, tm the marks cleared, tp the written pages' objects followed, td what they reach, tq the references.) */
    u64 t0, t1, t2, t3, t4, t5, t7, tw, tm = 0, tp = 0, td = 0, tq = 0;
    u64 faults0, faults1 = 0, faults2 = 0, faults3 = 0;   /* the process's page faults at each stop's start and end */
    u32 objects, written = 0, npages = 0;
    u8 **pages = NULL;
    u64 live = 0;
    void **stack;
    mg_jobj **found;
    char makers[200], large[600];
    bool ok;
    mg_dvm_gc_result r;
    roots_took first, second;
    memset(&r, 0, sizeof(r));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    makers[0] = large[0] = 0;
    mg_lock_acquire(&g_gc_lock);
    helpers_start();
    t_collecting = true;
    g_collector = NULL;
    g_counting = false;
    /* The arenas' write watch started again before the first stop, the app running (mg_dvm_heap_watch_restart). */
    tw = mg_time_monotonic_ns();
    mg_dvm_heap_watch_restart();
    /* The first stop. */
    t0 = mg_time_monotonic_ns();
    tw = t0 - tw;
    faults0 = mg_mem_page_faults();
    locks_take();
    objects = mg_dvm_heap_objects();
    stack = (void **)malloc(sizeof(void *) * ((size_t)objects + 64));
    /* (References made alongside are found when their page is looked at again: room for them too.) */
    found = (mg_jobj **)malloc(sizeof(mg_jobj *) * ((size_t)(u32)g_references * 2 + 1024));
    if (g_cap_to_finalize < g_nto_finalize + g_nfinalizable) {
        mg_jobj **grown = (mg_jobj **)realloc(g_to_finalize, sizeof(*grown) * ((size_t)g_nto_finalize + g_nfinalizable + 64));
        if (grown) {
            g_to_finalize = grown;
            g_cap_to_finalize = g_nto_finalize + g_nfinalizable + 64;
        }
    }
    ok = stack != NULL && found != NULL;
    if (ok) {
        memset(&G, 0, sizeof(G));
        G.main.stack = stack;
        G.main.cap = objects + 64;
        G.markers = 1;
        G.found = found;
        G.cap_found = (u32)g_references * 2 + 1024;
        g_busy = 1;
        stop_threads();
        t1 = mg_time_monotonic_ns();
        ok = !g_stop_failed;
        if (ok) {
            say_makers(makers, sizeof(makers));
            tm = mg_time_monotonic_ns();
            mg_dvm_heap_clear_marks();
            tm = mg_time_monotonic_ns() - tm;
            mg_dvm_heap_alloc_black(true);
            roots_pushed(&first, true);
        }
        faults1 = mg_mem_page_faults();
        resume_threads();
        g_busy = 0;
    } else {
        t1 = t0;
    }
    t2 = mg_time_monotonic_ns();
    locks_give(true);
    if (!ok) {
        free(stack);
        free(found);
        t_collecting = false;
        mg_lock_release(&g_gc_lock);
        mg_warn("gc: alongside: not collected (a thread of the runtime's could not be stopped, or there was no memory for the mark stack)");
        return;
    }
    /* Alongside the app: the collection's own thread below the app's threads meanwhile, as its helpers (marker_priority), unless
       the machine is near the end of its memory. */
    g_pressed = memory_pressed();
    g_alongside = true;
    if (t_cycle) marker_priority(&t_cycle_level);
    if (g_nhelpers && G.main.n >= PARALLEL_MIN) drain_shared();
    while (G.main.n) follow(&G.main, G.main.stack[--G.main.n]);
    g_alongside = false;
    if (t_cycle) marker_priority(&t_cycle_level);
    t3 = mg_time_monotonic_ns();
    /* The second stop. */
    locks_take();
    t4 = mg_time_monotonic_ns();
    /* (A list with room for every page the heap has, for the written ones to be shared: made before anything is stopped.) */
    npages = mg_dvm_heap_pages();
    if (g_nhelpers) pages = (u8 **)malloc(sizeof(u8 *) * ((size_t)npages + 1));
    g_busy = 1;
    faults2 = mg_mem_page_faults();
    stop_threads();
    t5 = mg_time_monotonic_ns();
    ok = !g_stop_failed;
    if (ok) {
        roots_pushed(&second, false);
        tp = mg_time_monotonic_ns();
        if (pages) {
            /* The marked large objects and the objects with fields by name on this thread, into the pool; the pages shared. */
            mg_dvm_heap_each_marked_large(follow_main);
            follow_with_fields();
            g_written = pages;
            g_nwritten = written = mg_dvm_heap_written_list(pages, npages);
            g_written_next = 0;
            shared(written_round);
        } else {
            written = mg_dvm_heap_each_written(follow_main);
            follow_with_fields();
        }
        td = mg_time_monotonic_ns();
        tp = td - tp;
        drain();
        while (G.overflow) {
            G.overflow = false;
            heap_rescan_marked();
        }
        tq = mg_time_monotonic_ns();
        td = tq - td;
        if (g_referent >= 0) process_references(false);
        tq = mg_time_monotonic_ns() - tq;
    }
    faults3 = mg_mem_page_faults();
    resume_threads();
    t7 = mg_time_monotonic_ns();
    locks_give(false);
    /* The heap still locked: nothing is being made, and what is made from now on is made unmarked. */
    mg_dvm_heap_alloc_black(false);
    if (ok) {
        u64 t8;
        r.sweepers = mg_dvm_heap_sweep(mg_dvm_object_dying, g_poison, &r.freed_objects, &r.freed_bytes);
        t8 = mg_time_monotonic_ns();
        r.sweep_ns = t8 - t7;
        /* (What lives is what the heap holds now: what was made alongside is marked without being counted.) */
        live = mg_dvm_live_bytes();
        g_budget = budget_after(live);
        mg_atomic_xchg(&g_since, 0);
        mg_atomic_xchg(&g_native_since, 0);
        if (t8 - g_large_said_ns > 30000000000ull && mg_dvm_heap_large_report(large, sizeof(large), 6)) g_large_said_ns = t8;
    } else {
        /* Given up: the marks mean nothing now; the next collection clears them. */
        mg_warn("gc: alongside: a thread of the runtime's could not be stopped the second time: nothing collected this time");
    }
    mg_dvm_heap_unlock();
    g_busy = 0;
    if (large[0]) mg_info("gc: the large objects (over 2 KB each) by class: %s", large);
    if (ok) {
        mg_atomic_add(&g_collections, 1);
        g_pause_ns += (t2 - t0) + (t7 - t4) + r.sweep_ns;
        /* (What each stop's time went to, part by part: the stops of Spotify's on a Lumia 650 were 85 to 180 ms, package 664.) */
        mg_info("gc: alongside the app: stopped %.1f ms and %.1f ms (to stop %.1f and %.1f; at the first the marks %.1f, the classes and "
                "strings %.1f, the other exact roots %.1f -- the JNI layer's %.1f --, the stacks %.1f; at the second the classes and strings "
                "%.1f, the other exact roots %.1f -- the JNI layer's %.1f --, the stacks %.1f, %u written pages' objects %.1f, what they reach "
                "%.1f, the references %.1f; page faults %u and %u); the write watch started again before in %.1f; marked alongside in %.1f ms "
                "by %u threads; sweep %.1f by %u; freed %.1f MB (%u objects), live %.1f MB (%u objects in all); %u threads stopped%s%s",
                (double)(t2 - t0) / 1e6, (double)(t7 - t4) / 1e6, (double)(t1 - t0) / 1e6, (double)(t5 - t4) / 1e6, (double)tm / 1e6,
                (double)first.classes_ns / 1e6, (double)first.exact_ns / 1e6, (double)first.jni_ns / 1e6, (double)first.stacks_ns / 1e6,
                (double)second.classes_ns / 1e6, (double)second.exact_ns / 1e6, (double)second.jni_ns / 1e6, (double)second.stacks_ns / 1e6,
                (unsigned)written, (double)tp / 1e6, (double)td / 1e6, (double)tq / 1e6, (unsigned)(faults1 - faults0),
                (unsigned)(faults3 - faults2), (double)tw / 1e6, (double)(t3 - t2) / 1e6,
                (unsigned)G.markers, (double)r.sweep_ns / 1e6, (unsigned)r.sweepers, (double)r.freed_bytes / 1048576.0,
                (unsigned)r.freed_objects, (double)live / 1048576.0, (unsigned)mg_dvm_heap_objects(), (unsigned)g_nstopped,
                makers[0] ? "; made since the last by " : "", makers);
    }
    t_collecting = false;
    free(stack);
    free(found);
    free(pages);
    mg_lock_release(&g_gc_lock);
    if (ok && (g_pending_refs || g_nto_finalize)) reference_work();
}

static MG_HOSTCB void cycle_main(void *arg)
{
    (void)arg;
    t_cycle = true;
    for (;;) {
        mg_lock_acquire(&g_cycle_lock);
        while (!g_cycle_asked) mg_cond_wait(&g_cycle_wake, &g_cycle_lock);
        g_cycle_asked = false;
        g_cycle_running = true;
        mg_lock_release(&g_cycle_lock);
        collect_alongside();
        mg_lock_acquire(&g_cycle_lock);
        g_cycle_running = false;
        g_cycles++;
        mg_cond_broadcast(&g_cycle_done);
        mg_lock_release(&g_cycle_lock);
    }
}

/* A collection alongside the app asked for, its thread started the first time; false when there is no such thread. */
static bool cycle_ask(void)
{
    bool asked = true;
    mg_lock_acquire(&g_cycle_lock);
    if (!g_cycle_thread) {
        mg_thread *t = mg_thread_start(cycle_main, NULL, (size_t)128 << 10, "gc");
        if (t) {
            mg_thread_detach(t);
            g_cycle_thread = true;
        }
    }
    if (!g_cycle_thread) asked = false;
    else if (!g_cycle_running && !g_cycle_asked) {
        g_cycle_asked = true;
        mg_cond_signal(&g_cycle_wake);
    }
    mg_lock_release(&g_cycle_lock);
    return asked;
}

static bool cycle_busy(void) { return g_cycle_running || g_cycle_asked; }

/* The app's main thread, which waits for a collection alongside the app a budget later than the others (mg_dvm_gc). */
static MG_THREAD_LOCAL bool t_main;

void mg_dvm_gc_main_thread(void) { t_main = true; }

void mg_dvm_gc_alongside_wait(void)
{
    /* (A thread holding the runtime's lock does not wait: the collection's stops take that lock.) */
    if (mg_dvm_lock_mine() || t_collecting) return;
    mg_lock_acquire(&g_cycle_lock);
    while (g_cycle_running || g_cycle_asked) mg_cond_wait(&g_cycle_done, &g_cycle_lock);
    mg_lock_release(&g_cycle_lock);
}

/*
 * The reference thread: a thread of the runtime's own, attached to Java as a daemon, that does what Java code must do after a
 * collection and the collector itself may not (it stops every thread, and runs on whichever one allocated): it hands the
 * cleared references to ReferenceQueue.enqueuePending (libcore's, which runs a Cleaner and enqueues the rest), and calls
 * finalize() on the objects found to finalize, catching what each throws. An object stays a root while it waits; after its
 * finalize() the next collection frees it, unless it made itself reachable again.
 */
static MG_HOSTCB void reference_main(void *arg)
{
    mg_jvalue v;
    (void)arg;
    mg_dvm_self();
    mg_dvm_thread_attach("HeapTaskDaemon", NULL, true);
    for (;;) {
        mg_jobj *list, *o = NULL;
        mg_lock_acquire(&g_final_lock);
        while (!g_pending_refs && !g_nto_finalize) mg_cond_wait(&g_final_cond, &g_final_lock);
        list = g_pending_refs;
        g_pending_refs = NULL;
        if (!list && g_nto_finalize) {
            o = g_to_finalize[--g_nto_finalize];
            mg_atomic_add(&g_finalizing, 1);
        }
        mg_lock_release(&g_final_lock);
        if (list) {
            v.j = 0;
            v.l = list;
            if (!mg_dvm_call_static("java/lang/ref/ReferenceQueue", "enqueuePending", "(Ljava/lang/ref/Reference;)V", &v, NULL)) mg_dvm_clear_pending();
        }
        if (o) {
            if (!mg_dvm_call_virtual(o, "finalize", "()V", NULL, NULL)) mg_dvm_clear_pending();
            o = NULL;
            mg_lock_acquire(&g_final_lock);
            mg_atomic_add(&g_finalizing, -1);
            mg_cond_broadcast(&g_final_cond);
            mg_lock_release(&g_final_lock);
        }
    }
}

static void reference_work(void)
{
    mg_lock_acquire(&g_final_lock);
    if (!g_ref_thread) {
        g_ref_thread = mg_thread_start(reference_main, NULL, 1u << 20, "references");
        if (g_ref_thread) mg_thread_detach(g_ref_thread);
    }
    mg_cond_broadcast(&g_final_cond);
    mg_lock_release(&g_final_lock);
}

void mg_dvm_gc_run_finalization(void)
{
    u64 until = mg_time_monotonic_ns() + 10000000000ull;
    mg_lock_acquire(&g_final_lock);
    while ((g_nto_finalize || g_finalizing) && mg_time_monotonic_ns() < until) mg_cond_wait_ns(&g_final_cond, &g_final_lock, 100000000ull);
    mg_lock_release(&g_final_lock);
}

bool mg_dvm_gc(const char *reason, mg_dvm_gc_result *out)
{
    mg_dvm_gc_result r;
    bool ok;
    if (!out && !strcmp(reason, "allocation") && alongside_on() && cycle_ask()) {
        /*
         * Asked for, and the app goes on. One that makes three budgets' worth before the collection ends waits for it, so
         * that the heap does not grow without end while it marks; where memory is pressed, one budget's worth, which is what
         * the collection was asked at: a thread that makes anything more waits.
         *
         * The app's main thread one budget's worth later than the others. It waited with them: Spotify on a Lumia 650 was
         * pressed at every collection, one each 3 to 5 s while its lists moved, and its main thread stood in each until the
         * collection ended -- the marking alongside, the second stop and the sweep, 0.5 to 0.7 s -- a fifth of its time in
         * all, the frames and the touches waiting meanwhile (package 664). It makes 1 to 2 MB in such a time; the other
         * threads, Google Play services' among them (a third of what was made), still wait.
         */
        u64 times = memory_pressed() ? 1 : 3;
        if (t_main) times++;
        if ((u64)(u32)g_since > times * g_budget) mg_dvm_gc_alongside_wait();
        return true;
    }
    mg_dvm_gc_alongside_wait();
    ok = collect(reason, true, &r);
    if (out) *out = r;
    if (!ok) {
        mg_warn("gc: %s: not collected (a thread of the runtime's could not be stopped, or there was no memory for the mark stack)", reason);
        return false;
    }
    if (!r.objects) return true;
    {
        char roots[200];
        if (r.roots_shared)
            snprintf(roots, sizeof(roots), "the exact roots %.1f -- %u of the JNI layer's %.1f --, then %u classes and %u interned strings and what they hold %.1f",
                     (double)r.roots_ns / 1e6, (unsigned)r.jni_roots, (double)r.jni_ns / 1e6, (unsigned)r.classes, (unsigned)r.strings,
                     (double)r.classes_ns / 1e6);
        else
            snprintf(roots, sizeof(roots), "the exact roots %.1f -- %u classes %.1f, %u interned strings %.1f, %u of the JNI layer's %.1f --",
                     (double)r.roots_ns / 1e6, (unsigned)r.classes, (double)r.classes_ns / 1e6, (unsigned)r.strings, (double)r.strings_ns / 1e6,
                     (unsigned)r.jni_roots, (double)r.jni_ns / 1e6);
    mg_info("gc: %s: %.1f ms (stop %.1f, mark %.1f by %u threads: %s, the stacks %.1f, the rest following references; sweep %.1f by %u); freed "
            "%.1f MB (%u objects), live %.1f MB (%u objects); %u threads stopped; %.2f MB (%u objects) found only through a conservative root%s%s",
            reason, (double)(r.stop_ns + r.mark_ns + r.sweep_ns) / 1e6, (double)r.stop_ns / 1e6, (double)r.mark_ns / 1e6, (unsigned)r.markers,
            roots, (double)r.stacks_ns / 1e6, (double)r.sweep_ns / 1e6, (unsigned)r.sweepers,
            (double)r.freed_bytes / 1048576.0, (unsigned)r.freed_objects, (double)r.live_bytes / 1048576.0, (unsigned)r.live_objects,
            (unsigned)r.threads_stopped, (double)r.conservative_bytes / 1048576.0, (unsigned)r.conservative, r.makers[0] ? "; made since the last by " : "",
            r.makers);
    }
    return true;
}

bool mg_dvm_gc_marked(const mg_jobj *o)
{
    bool marked;
    mg_dvm_heap_lock();
    marked = o && mg_dvm_heap_find((mg_addr)o) == (const void *)o && mg_dvm_heap_is_marked(o);
    mg_dvm_heap_unlock();
    return marked;
}
