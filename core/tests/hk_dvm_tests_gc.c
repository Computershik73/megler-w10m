/*
 * The collector (docs/JAVA.md, "The collector"), from the heap up: what an address inside an object is found to be.
 */
#include "../src/dvm/hk_dvm_internal.h"
#include "hk_dvm_tests.h"

/* A thread that holds an object only in a C local while it waits: what its stack keeps, found while it is stopped. */
typedef struct held {
    hk_jobj *volatile object;
    volatile i32 stage;                /* 1 it holds it, 2 it may let go */
} held;

static HK_NOINLINE void hold_in_a_local(held *h)
{
    hk_jobj *volatile mine = hk_dvm_new_array("[I", 7);
    h->object = mine;
    hk_atomic_xchg(&h->stage, 1);
    while (h->stage == 1) hk_thread_yield();
    /* Still used after the census: kept to here by the stack. */
    h->object = mine;
}

static void holder_main(void *p)
{
    held *h = (held *)p;
    hk_dvm_self();
    hold_in_a_local(h);
}

/*
 * A census: a collection that marks and frees nothing. Every object that something holds is found, through each kind of root,
 * and garbage is not: an array only a C local of another thread holds, while that thread is stopped; a global reference; a
 * static field (a fixture's); a chain through an object array; an interior pointer to an array's elements.
 */
static void test_gc_census(void)
{
    static held h;
    hk_dvm_gc_result r;
    hk_jobj *global, *outer, *inner, *interior_of;
    /* The garbage's addresses where no scan looks: the C library's heap. */
    hk_addr *garbage = (hk_addr *)malloc(sizeof(hk_addr) * 100);
    volatile hk_addr interior;
    hk_thread *t;
    hk_jobj *volatile kept;
    int i, unmarked = 0;
    say("the collector: a census (marks only)");
    if (!garbage) return;
    for (i = 0; i < 100; i++) garbage[i] = (hk_addr)hk_dvm_new_array("[B", 40 + i);
    global = hk_dvm_new_string("held by a global reference");
    hk_dvm_add_root(&global);
    outer = hk_dvm_new_array("[Ljava/lang/Object;", 3);
    inner = hk_dvm_new_string("inside an array");
    outer->u.oarr.v[1] = inner;
    kept = outer;
    interior_of = hk_dvm_new_array("[C", 50);
    interior = (hk_addr)interior_of->u.arr.data + 20;
    interior_of = NULL;
    memset((void *)&h, 0, sizeof(h));
    t = hk_thread_start(holder_main, (void *)&h, 0, "gc-holder");
    CHECK(t != NULL);
    while (t && h.stage != 1) hk_thread_yield();
    CHECK(hk_dvm_gc_census(&r));
    say("  %u objects, %u reachable (%u KB), %u of them found first by a conservative root; %u threads stopped; %.1f ms to stop, %.1f to mark",
        (unsigned)r.objects, (unsigned)r.live_objects, (unsigned)(r.live_bytes >> 10), (unsigned)r.conservative, (unsigned)r.threads_stopped,
        (double)r.stop_ns / 1e6, (double)r.mark_ns / 1e6);
    CHECK(r.live_objects > 0 && r.live_objects < r.objects);
    CHECK(r.threads_stopped >= 1);
    CHECK(hk_dvm_gc_marked(h.object));
    CHECK(hk_dvm_gc_marked(global));
    CHECK(hk_dvm_gc_marked(kept) && hk_dvm_gc_marked(inner));
    hk_dvm_heap_lock();
    CHECK(hk_dvm_heap_find(interior) && hk_dvm_heap_is_marked(hk_dvm_heap_find(interior)));
    hk_dvm_heap_unlock();
    /* The garbage is not reachable. (A word that happens to look like one of them could keep it: nearly all, not all.) */
    for (i = 0; i < 100; i++) unmarked += !hk_dvm_gc_marked((hk_jobj *)garbage[i]);
    say("  %d of the 100 arrays nothing holds were found unreachable", unmarked);
    CHECK(unmarked >= 95);
    free(garbage);
    hk_atomic_xchg(&h.stage, 2);
    if (t) hk_thread_join(t);
}

/*
 * A collection: what nothing holds is freed, what something holds is not -- a string only a C local holds, one a registered
 * root holds -- and both read as they were. (The tests run with the collector's poison on: freed memory is filled with 0xDB
 * and never used again, so an object freed too early would read as that.)
 */
static hk_jobj *g_rooted;

static void test_gc_collect(void)
{
    hk_dvm_gc_result r;
    hk_jobj *volatile kept = hk_dvm_new_string("kept on the stack");
    int i;
    say("the collector: a collection frees what nothing holds");
    g_rooted = hk_dvm_new_string("kept by a root");
    hk_dvm_add_root(&g_rooted);
    /* (No collection of the tests' own budget among these: the one below is to find them all.) */
    hk_dvm_gc_set_min_budget(64u << 20);
    for (i = 0; i < 2000; i++) hk_dvm_new_array("[B", 100);
    CHECK(hk_dvm_gc("test", &r));
    hk_dvm_gc_set_min_budget(1u << 20);
    say("  freed %u objects (%u KB), %u live (%u KB), %u found first through a conservative root; %.1f ms in all",
        (unsigned)r.freed_objects, (unsigned)(r.freed_bytes >> 10), (unsigned)r.live_objects, (unsigned)(r.live_bytes >> 10), (unsigned)r.conservative,
        (double)(r.stop_ns + r.mark_ns + r.sweep_ns) / 1e6);
    CHECK(r.freed_objects >= 1900);
    CHECK(!strcmp(hk_dvm_string_utf8(kept), "kept on the stack"));
    CHECK(!strcmp(hk_dvm_string_utf8(g_rooted), "kept by a root"));
    /* Nothing more to free a moment later, and what is held is still there. */
    CHECK(hk_dvm_gc("test again", &r));
    CHECK(r.freed_objects < 100);
    CHECK(!strcmp(hk_dvm_string_utf8(kept), "kept on the stack"));
}

void test_gc(void)
{
    hk_jobj *small = hk_dvm_new_array("[B", 10), *mid = hk_dvm_new_array("[I", 300), *big = hk_dvm_new_array("[J", 2000);
    int local = 0;
    say("the heap: an address inside an object is that object's");
    CHECK(small && mid && big);
    if (!small || !mid || !big) return;
    hk_dvm_heap_lock();
    CHECK(hk_dvm_heap_find((hk_addr)small) == (void *)small);
    /* Inside the elements: the array they belong to, which is what keeps them when a C local holds only them. */
    CHECK(hk_dvm_heap_find((hk_addr)small->u.arr.data + 9) == (void *)small);
    CHECK(hk_dvm_heap_find((hk_addr)mid->u.arr.data + 1199) == (void *)mid);
    /* A large one (over 2 KB), from its own allocation. */
    CHECK(hk_dvm_heap_find((hk_addr)big) == (void *)big);
    CHECK(hk_dvm_heap_find((hk_addr)big->u.arr.data + 15999) == (void *)big);
    /* Not the heap's: an address on the stack, and none. */
    CHECK(hk_dvm_heap_find((hk_addr)&local) == NULL);
    CHECK(hk_dvm_heap_find(0) == NULL);
    hk_dvm_heap_unlock();
    test_gc_census();
    test_gc_collect();
}
