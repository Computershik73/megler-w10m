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

static MG_HOSTCB void holder_main(void *p)
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
    /*
     * (No collection of the tests' own budget among these, as in test_gc_collect: one alongside the app, asked by the
     * allocations below, swept the garbage before the census counted it, which then found every object reachable, 20320
     * of 20320, in a Windows run of 2026-11-07.)
     */
    mg_dvm_gc_alongside_wait();
    mg_dvm_gc_set_min_budget(64u << 20);
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
    mg_dvm_gc_set_min_budget(1u << 20);
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
 * The mark shared among threads (mg_dvm_gc.c, "marking in parallel"): a graph of some 160,000 objects -- 400 arrays of 300
 * arrays of ints, and a chain 20,000 long of two-element arrays, each with an int[] of its place -- held by one root, made
 * among garbage, and collected three times. Every int still reads as it was written (the tests' poison would show an object
 * freed too early), and the chain is whole.
 */
static mg_jobj *g_graph;

static u32 graph_damage(void)
{
    u32 bad = 0, i, k, n;
    mg_jobj *node;
    for (i = 0; i < 400; i++) {
        mg_jobj *row = g_graph->u.oarr.v[i];
        for (k = 0; k < 300; k++) {
            const i32 *v = (const i32 *)row->u.oarr.v[k]->u.arr.data;
            bad += v[0] != (i32)i || v[1] != (i32)k || v[2] != (i32)(i * k);
        }
    }
    for (node = g_graph->u.oarr.v[400], n = 0; node && n <= 20000; node = node->u.oarr.v[0], n++)
        bad += ((const i32 *)node->u.oarr.v[1]->u.arr.data)[0] != (i32)n;
    return bad + (n != 20000);
}

static void test_gc_parallel(void)
{
    mg_dvm_gc_result r;
    mg_jobj *volatile chain = NULL;
    u32 i, k, round;
    say("the collector: the mark shared among threads, over a graph of 160,000 objects");
    g_graph = mg_dvm_new_array("[Ljava/lang/Object;", 401);
    mg_dvm_add_root(&g_graph);
    for (i = 0; i < 400; i++) {
        mg_jobj *row = mg_dvm_new_array("[Ljava/lang/Object;", 300);
        g_graph->u.oarr.v[i] = row;
        for (k = 0; k < 300; k++) {
            mg_jobj *v = mg_dvm_new_array("[I", 3);
            ((i32 *)v->u.arr.data)[0] = (i32)i;
            ((i32 *)v->u.arr.data)[1] = (i32)k;
            ((i32 *)v->u.arr.data)[2] = (i32)(i * k);
            row->u.oarr.v[k] = v;
            if (!(k & 7)) mg_dvm_new_array("[B", 24);           /* garbage among them */
        }
    }
    for (i = 20000; i-- > 0;) {
        mg_jobj *node = mg_dvm_new_array("[Ljava/lang/Object;", 2), *place;
        node->u.oarr.v[0] = chain;
        chain = node;
        place = mg_dvm_new_array("[I", 1);
        ((i32 *)place->u.arr.data)[0] = (i32)i;
        node->u.oarr.v[1] = place;
    }
    g_graph->u.oarr.v[400] = chain;
    chain = NULL;
    for (round = 0; round < 3; round++) {
        CHECK(mg_dvm_gc("the parallel test", &r));
        CHECK_INT((i32)graph_damage(), 0);
    }
    say("  marked by %u threads: %u live objects in %.1f ms (the exact roots %.1f, the stacks %.1f); swept by %u in %.1f ms", (unsigned)r.markers,
        (unsigned)r.live_objects, (double)r.mark_ns / 1e6, (double)r.roots_ns / 1e6, (double)r.stacks_ns / 1e6, (unsigned)r.sweepers,
        (double)r.sweep_ns / 1e6);
    CHECK(r.live_objects >= 160000);
    g_graph = NULL;
}

/*
 * Marking alongside the app (mg_dvm_gc.c): collections asked for by allocation while three threads move arrays about a
 * graph that one root holds -- swapping two of its rows' elements, putting a new array where one was -- and make garbage.
 * Every array still held reads as it was written at the end (the tests' poison would show one freed while held). No page is
 * watched here, and the second stop looks at every marked object again: what this checks is that nothing a thread moved
 * while the collector marked is lost, and that the collections did run alongside, not that the host's watch sees writes.
 */
#define ALONG_ROWS 64
#define ALONG_COLS 64
static mg_jobj *g_along;
static volatile i32 g_along_stop, g_along_done;

static mg_jobj *along_new(u32 tag)
{
    mg_jobj *v = mg_dvm_new_array("[I", 4);
    i32 *d = (i32 *)v->u.arr.data;
    d[0] = (i32)tag;
    d[1] = (i32)~tag;
    d[2] = (i32)(tag * 7u);
    d[3] = 0x5A5A5A5A;
    return v;
}

static u32 along_damage(void)
{
    u32 bad = 0, i, k;
    for (i = 0; i < ALONG_ROWS; i++) {
        mg_jobj *row = g_along->u.oarr.v[i];
        for (k = 0; k < ALONG_COLS; k++) {
            mg_jobj *v = row->u.oarr.v[k];
            const i32 *d;
            if (!v || v->kind != MG_J_PRIM_ARRAY || v->u.arr.len != 4) {
                bad++;
                continue;
            }
            d = (const i32 *)v->u.arr.data;
            bad += d[1] != (i32)~(u32)d[0] || d[2] != (i32)((u32)d[0] * 7u) || d[3] != 0x5A5A5A5A;
        }
    }
    return bad;
}

static MG_HOSTCB void along_main(void *p)
{
    u32 seed = (u32)(mg_addr)p * 2654435761u + 1u, n = 0;
    mg_dvm_self();
    while (!g_along_stop) {
        u32 ra, ca, rb, cb;
        mg_jobj *rowa, *rowb, *va, *vb;
        seed = seed * 1103515245u + 12345u;
        ra = (seed >> 4) % ALONG_ROWS;
        ca = (seed >> 12) % ALONG_COLS;
        seed = seed * 1103515245u + 12345u;
        rb = (seed >> 4) % ALONG_ROWS;
        cb = (seed >> 12) % ALONG_COLS;
        rowa = g_along->u.oarr.v[ra];
        rowb = g_along->u.oarr.v[rb];
        /* Read from one, the other let go of, then written into the first: an array held a moment by this thread alone. */
        va = rowa->u.oarr.v[ca];
        rowa->u.oarr.v[ca] = NULL;
        vb = rowb->u.oarr.v[cb];
        rowb->u.oarr.v[cb] = NULL;
        mg_dvm_new_array("[B", 200);                      /* garbage, and a collection asked for now and then */
        rowb->u.oarr.v[cb] = va ? va : along_new(n);
        rowa->u.oarr.v[ca] = vb ? vb : along_new(n);
        if (!(++n & 7)) rowa->u.oarr.v[ca] = along_new(n);
    }
    mg_atomic_add(&g_along_done, 1);
}

static void test_gc_alongside(void)
{
    u64 before = 0, after = 0, pause = 0;
    u32 i, k;
    int t;
    say("the collector: marking alongside threads that move objects about");
    g_along = mg_dvm_new_array("[Ljava/lang/Object;", ALONG_ROWS);
    mg_dvm_add_root(&g_along);
    for (i = 0; i < ALONG_ROWS; i++) {
        mg_jobj *row = mg_dvm_new_array("[Ljava/lang/Object;", ALONG_COLS);
        g_along->u.oarr.v[i] = row;
        for (k = 0; k < ALONG_COLS; k++) row->u.oarr.v[k] = along_new(i * ALONG_COLS + k);
    }
    mg_dvm_gc_totals(&before, &pause);
    mg_dvm_gc_set_alongside(true);
    g_along_stop = g_along_done = 0;
    for (t = 0; t < 3; t++) {
        mg_thread *th = mg_thread_start(along_main, (void *)(mg_addr)(t + 1), (size_t)256 << 10, "along");
        CHECK(th != NULL);
        if (th) mg_thread_detach(th);
        else mg_atomic_add(&g_along_done, 1);
    }
    for (i = 0; i < 3000; i++) mg_dvm_new_array("[B", 2000);
    mg_sleep_ns(300000000ull);
    g_along_stop = 1;
    while (g_along_done < 3) mg_thread_yield();
    mg_dvm_gc_alongside_wait();
    mg_dvm_gc_totals(&after, &pause);
    say("  %u collections alongside while the threads moved arrays", (unsigned)(after - before));
    CHECK(after >= before + 3);
    CHECK_INT((i32)along_damage(), 0);
    mg_dvm_gc_set_alongside(false);
    g_along = NULL;
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

/*
 * The budget a collection leaves (budget_after): half of what lived, at least the least; with the machine's free memory known,
 * no more than half of what it has beyond 96 MB, and the least when it has less. TikTok Lite on a Lumia 650 was given the
 * least by the app's limit, which Windows let it pass, and collected every 0.8 s (package 619).
 */
static u64 g_test_available;
static MG_HOSTCB u64 test_available(void) { return g_test_available; }

static void test_gc_budget_by_free_memory(void)
{
    static mg_jobj *held[24];
    static bool rooted;
    mg_dvm_gc_result r;
    int i;
    say("the collector: the budget within the machine's free memory");
    mg_dvm_gc_set_min_budget(1u << 20);
    for (i = 0; i < (int)MG_COUNT(held); i++) {
        held[i] = mg_dvm_new_array("[B", 1 << 20);
        if (!rooted) mg_dvm_add_root(&held[i]);
    }
    rooted = true;
    mg_mem_set_available_probe(test_available);
    /* Unknown, and no limit either: half of what lived. */
    g_test_available = 0;
    CHECK(mg_dvm_gc("test budget", &r));
    say("  %u MB live: the budget %u MB with the free memory unknown", (unsigned)(r.live_bytes >> 20), (unsigned)(mg_dvm_gc_budget() >> 20));
    CHECK(r.live_bytes >= (24u << 20) && mg_dvm_gc_budget() == r.live_bytes / 2);
    /* Plenty: half of what lived. */
    g_test_available = 1ull << 30;
    CHECK(mg_dvm_gc("test budget", &r));
    CHECK(mg_dvm_gc_budget() == r.live_bytes / 2);
    /* 10 MB beyond the 96 kept free: 5. */
    g_test_available = (96ull + 10) << 20;
    CHECK(mg_dvm_gc("test budget", &r));
    CHECK(mg_dvm_gc_budget() == (5u << 20));
    /* Less than what is kept free: the least. */
    g_test_available = 64ull << 20;
    CHECK(mg_dvm_gc("test budget", &r));
    CHECK(mg_dvm_gc_budget() == (1u << 20));
    mg_mem_set_available_probe(NULL);
    for (i = 0; i < (int)MG_COUNT(held); i++) held[i] = NULL;
    CHECK(mg_dvm_gc("test budget", &r));
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
    test_gc_parallel();
    test_gc_alongside();
    /* Where the objects so far were made (the samples of every 256 KB a thread makes), said and forgotten: and so said again with nothing new. */
    mg_dvm_made_say();
    mg_dvm_made_say();
    test_gc_gives_back();
    test_gc_budget_by_free_memory();
}
