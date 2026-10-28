/*
 * The collector (docs/JAVA.md, "The collector"), from the heap up: what an address inside an object is found to be.
 */
#include "../src/dvm/mg_dvm_internal.h"
#include "mg_dvm_tests.h"

/* A thread that holds an object only in a C local while it waits: what its stack keeps, found while it is stopped. */
typedef struct held {
    mg_jobj *volatile object;
    volatile i32 stage;                /* 1 it holds it, 2 it may let go */
} held;

static MG_NOINLINE void hold_in_a_local(held *h)
{
    mg_jobj *volatile mine = mg_dvm_new_array("[I", 7);
    h->object = mine;
    mg_atomic_xchg(&h->stage, 1);
    while (h->stage == 1) mg_thread_yield();
    /* Still used after the census: kept to here by the stack. */
    h->object = mine;
}

static void holder_main(void *p)
{
    held *h = (held *)p;
    mg_dvm_self();
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
    mg_dvm_gc_result r;
    mg_jobj *global, *outer, *inner, *interior_of;
    /* The garbage's addresses where no scan looks: the C library's heap. */
    mg_addr *garbage = (mg_addr *)malloc(sizeof(mg_addr) * 100);
    volatile mg_addr interior;
    mg_thread *t;
    mg_jobj *volatile kept;
    int i, unmarked = 0;
    say("the collector: a census (marks only)");
    if (!garbage) return;
    for (i = 0; i < 100; i++) garbage[i] = (mg_addr)mg_dvm_new_array("[B", 40 + i);
    global = mg_dvm_new_string("held by a global reference");
    mg_dvm_add_root(&global);
    outer = mg_dvm_new_array("[Ljava/lang/Object;", 3);
    inner = mg_dvm_new_string("inside an array");
    outer->u.oarr.v[1] = inner;
    kept = outer;
    interior_of = mg_dvm_new_array("[C", 50);
    interior = (mg_addr)interior_of->u.arr.data + 20;
    interior_of = NULL;
    memset((void *)&h, 0, sizeof(h));
    t = mg_thread_start(holder_main, (void *)&h, 0, "gc-holder");
    CHECK(t != NULL);
    while (t && h.stage != 1) mg_thread_yield();
    CHECK(mg_dvm_gc_census(&r));
    say("  %u objects, %u reachable (%u KB), %u of them found first by a conservative root; %u threads stopped; %.1f ms to stop, %.1f to mark",
        (unsigned)r.objects, (unsigned)r.live_objects, (unsigned)(r.live_bytes >> 10), (unsigned)r.conservative, (unsigned)r.threads_stopped,
        (double)r.stop_ns / 1e6, (double)r.mark_ns / 1e6);
    CHECK(r.live_objects > 0 && r.live_objects < r.objects);
    CHECK(r.threads_stopped >= 1);
    CHECK(mg_dvm_gc_marked(h.object));
    CHECK(mg_dvm_gc_marked(global));
    CHECK(mg_dvm_gc_marked(kept) && mg_dvm_gc_marked(inner));
    mg_dvm_heap_lock();
    CHECK(mg_dvm_heap_find(interior) && mg_dvm_heap_is_marked(mg_dvm_heap_find(interior)));
    mg_dvm_heap_unlock();
    /* The garbage is not reachable. (A word that happens to look like one of them could keep it: nearly all, not all.) */
    for (i = 0; i < 100; i++) unmarked += !mg_dvm_gc_marked((mg_jobj *)garbage[i]);
    say("  %d of the 100 arrays nothing holds were found unreachable", unmarked);
    CHECK(unmarked >= 95);
    free(garbage);
    mg_atomic_xchg(&h.stage, 2);
    if (t) mg_thread_join(t);
}

/*
 * A collection: what nothing holds is freed, what something holds is not -- a string only a C local holds, one a registered
 * root holds -- and both read as they were. (The tests run with the collector's poison on: freed memory is filled with 0xDB
 * and never used again, so an object freed too early would read as that.)
 */
static mg_jobj *g_rooted;
static mg_jobj *g_many_rooted[150];

static void test_gc_collect(void)
{
    mg_dvm_gc_result r;
    mg_jobj *volatile kept = mg_dvm_new_string("kept on the stack");
    int i, unmarked;
    say("the collector: a collection frees what nothing holds");
    g_rooted = mg_dvm_new_string("kept by a root");
    mg_dvm_add_root(&g_rooted);
    /* (No collection of the tests' own budget among these: the one below is to find them all.) */
    mg_dvm_gc_set_min_budget(64u << 20);
    for (i = 0; i < 2000; i++) mg_dvm_new_array("[B", 100);
    CHECK(mg_dvm_gc("test", &r));
    mg_dvm_gc_set_min_budget(1u << 20);
    say("  freed %u objects (%u KB), %u live (%u KB), %u found first through a conservative root; %.1f ms in all",
        (unsigned)r.freed_objects, (unsigned)(r.freed_bytes >> 10), (unsigned)r.live_objects, (unsigned)(r.live_bytes >> 10), (unsigned)r.conservative,
        (double)(r.stop_ns + r.mark_ns + r.sweep_ns) / 1e6);
    CHECK(r.freed_objects >= 1900);
    CHECK(!strcmp(mg_dvm_string_utf8(kept), "kept on the stack"));
    CHECK(!strcmp(mg_dvm_string_utf8(g_rooted), "kept by a root"));
    /* As many C roots as a Java app's GL threads, its window's layers and native code's windows take together, each held. */
    for (i = 0; i < (int)MG_COUNT(g_many_rooted); i++) {
        g_many_rooted[i] = mg_dvm_new_array("[B", 16);
        mg_dvm_add_root(&g_many_rooted[i]);
    }
    CHECK(mg_dvm_gc_census(&r));
    for (i = 0, unmarked = 0; i < (int)MG_COUNT(g_many_rooted); i++) unmarked += !mg_dvm_gc_marked(g_many_rooted[i]);
    CHECK(unmarked == 0);
    /* Nothing more to free a moment later, and what is held is still there. */
    CHECK(mg_dvm_gc("test again", &r));
    CHECK(r.freed_objects < 100);
    CHECK(!strcmp(mg_dvm_string_utf8(kept), "kept on the stack"));
}

/*
 * Blocks a collection leaves empty go back to the host past the first 8 MB of them (mg_dvm_heap.c): 40 MB of small arrays
 * made and let go, then collected without the tests' poison (which keeps every freed cell from use), leave the heap's
 * memory at most 20 MB above where it was; and what is made after is made in blocks committed again, as zero memory.
 */
static void test_gc_gives_back(void)
{
    mg_dvm_gc_result r;
    mg_dvm_heap_info before, made, after;
    mg_jobj *volatile late;
    int i;
    say("the collector: empty blocks past a few go back to the host");
    mg_dvm_gc_set_poison(false);
    mg_dvm_gc_set_min_budget(64u << 20);
    CHECK(mg_dvm_gc("test before", &r));
    mg_dvm_heap_stats(&before);
    for (i = 0; i < 40 * 1024; i++) mg_dvm_new_array("[B", 1000);
    mg_dvm_heap_stats(&made);
    CHECK(mg_dvm_gc("test after", &r));
    mg_dvm_heap_stats(&after);
    say("  the blocks' memory: %u MB before, %u MB with 40 MB made, %u MB once collected", (unsigned)(before.reserved >> 20), (unsigned)(made.reserved >> 20),
        (unsigned)(after.reserved >> 20));
    CHECK(made.reserved >= before.reserved + (36u << 20));
    CHECK(after.reserved <= before.reserved + (20u << 20));
    for (i = 0; i < 20 * 1024; i++) mg_dvm_new_array("[B", 1000);
    late = mg_dvm_new_array("[B", 1000);
    CHECK(late && mg_dvm_array_length(late) == 1000);
    /* And what the large objects are, by class, once they take more than 32 MB: 36 arrays of 1 MB held come first. */
    {
        static mg_jobj *held[36];
        static bool rooted;
        char report[600];
        bool said;
        for (i = 0; i < (int)MG_COUNT(held); i++) {
            held[i] = mg_dvm_new_array("[B", 1 << 20);
            if (!rooted) mg_dvm_add_root(&held[i]);
        }
        rooted = true;
        mg_dvm_heap_lock();
        said = mg_dvm_heap_large_report(report, sizeof(report), 6);
        mg_dvm_heap_unlock();
        say("  the large objects by class: %s", said ? report : "(not said)");
        CHECK(said && !strncmp(report, "[B ", 3) && atof(report + 3) >= 36.0);
        /* (The roots stay, holding nothing.) */
        for (i = 0; i < (int)MG_COUNT(held); i++) held[i] = NULL;
    }
    mg_dvm_gc_set_min_budget(1u << 20);
    mg_dvm_gc_set_poison(true);
}

void test_gc(void)
{
    mg_jobj *small = mg_dvm_new_array("[B", 10), *mid = mg_dvm_new_array("[I", 300), *big = mg_dvm_new_array("[J", 2000);
    int local = 0;
    say("the heap: an address inside an object is that object's");
    CHECK(small && mid && big);
    if (!small || !mid || !big) return;
    mg_dvm_heap_lock();
    CHECK(mg_dvm_heap_find((mg_addr)small) == (void *)small);
    /* Inside the elements: the array they belong to, which is what keeps them when a C local holds only them. */
    CHECK(mg_dvm_heap_find((mg_addr)small->u.arr.data + 9) == (void *)small);
    CHECK(mg_dvm_heap_find((mg_addr)mid->u.arr.data + 1199) == (void *)mid);
    /* A large one (over 2 KB), from its own allocation. */
    CHECK(mg_dvm_heap_find((mg_addr)big) == (void *)big);
    CHECK(mg_dvm_heap_find((mg_addr)big->u.arr.data + 15999) == (void *)big);
    /* Not the heap's: an address on the stack, and none. */
    CHECK(mg_dvm_heap_find((mg_addr)&local) == NULL);
    CHECK(mg_dvm_heap_find(0) == NULL);
    mg_dvm_heap_unlock();
    test_gc_census();
    test_gc_collect();
    test_gc_gives_back();
}
