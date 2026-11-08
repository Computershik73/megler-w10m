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
 * least by the app's limit, which Windows let it pass, and collected every 0.8 s (package 619). And with the process's free
 * addresses known, no more than a quarter of them beyond 192 MB.
 */
static u64 g_test_available, g_test_addresses;
static MG_HOSTCB u64 test_available(void) { return g_test_available; }
static u64 test_addresses(void) { return g_test_addresses; }

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
    /*
     * The process's free addresses known, as a 32-bit host knows them: no more than a quarter of what is free beyond 192 MB,
     * 10 MB of 232; the least when there are fewer.
     */
    mg_dvm_gc_set_address_probe(test_addresses);
    g_test_addresses = (192ull + 40) << 20;
    CHECK(mg_dvm_gc("test budget", &r));
    CHECK(mg_dvm_gc_budget() == (10u << 20));
    g_test_addresses = 150ull << 20;
    CHECK(mg_dvm_gc("test budget", &r));
    CHECK(mg_dvm_gc_budget() == (1u << 20));
    mg_dvm_gc_set_address_probe(NULL);
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

/*
 * The heap near what lives while threads make garbage fast (docs/JAVA.md, "When it runs"): four threads make 480 MB of
 * arrays nothing keeps, small ones and now and then one over 2 KB, and now and then put a new array in place of one of a
 * live set of some 130,000 arrays, 30 MB, which one root holds -- what lives stays the same. Collections are asked for by
 * allocation and mark alongside the threads, as on the phone. The heap's blocks, sampled every 10 ms meanwhile, and what is
 * in use, are said against what lived at the end (a collection that stops the threads, with nothing made). Twice: as the
 * host's scheduler has it, and with the markers at the lowest priority starved as a phone's busy cores starve them
 * (mg_dvm_gc_set_marker_pace: 32 objects a millisecond each). WhatsApp's heap on an HP Elite x3 grew to 565 MB of blocks
 * for some 95 MB that lived while its collections marked alongside it for up to 16 s, and its process ran out of addresses
 * (0.1.0.671). All of it runs on a thread of its own, which ends before what lived is let go: a word of the live set's
 * left on this thread's stack kept all of it (a conservative root).
 */
#define NEAR_ROWS 128
#define NEAR_COLS 1024
#define NEAR_THREADS 4
#define NEAR_PACE 32u
/* (Less in the phone's Self-test, whose processors are slower: a phone's own run is what the qemu runs measure.) */
#define NEAR_GARBAGE ((MG_ARCH_ARM32 && MG_HOST_WINDOWS ? 192u : 480u) << 20)
static mg_jobj *g_near;
static volatile i32 g_near_made_kb, g_near_done;

/* The live set's array at (row, col): its size, and a pattern its bytes are kept with. */
static i32 near_size(u32 row, u32 col) { return (col & 63) == 7 ? 3000 : 24 + (i32)((row * 131u + col * 7u) % 177u); }

static mg_jobj *near_new(u32 row, u32 col)
{
    i32 n = near_size(row, col), i;
    mg_jobj *a = mg_dvm_new_array("[B", n);
    u8 *d = (u8 *)a->u.arr.data;
    for (i = 0; i < n; i += 61) d[i] = (u8)(row ^ col);
    return a;
}

static u32 near_damage(void)
{
    u32 bad = 0, row, col;
    for (row = 0; row < NEAR_ROWS; row++) {
        mg_jobj *r = g_near->u.oarr.v[row];
        for (col = 0; col < NEAR_COLS; col++) {
            mg_jobj *a = r->u.oarr.v[col];
            i32 i;
            if (!a || a->kind != MG_J_PRIM_ARRAY || a->u.arr.len != near_size(row, col)) {
                bad++;
                continue;
            }
            for (i = 0; i < a->u.arr.len; i += 61)
                if (((const u8 *)a->u.arr.data)[i] != (u8)(row ^ col)) {
                    bad++;
                    break;
                }
        }
    }
    return bad;
}

static MG_HOSTCB void near_main(void *p)
{
    u32 seed = (u32)(mg_addr)p * 2246822519u + 7u, n = 0, made = 0;
    mg_dvm_self();
    while ((u32)g_near_made_kb < (NEAR_GARBAGE >> 10)) {
        i32 size;
        seed = seed * 1103515245u + 12345u;
        size = (n & 15) == 15 ? 3000 + (i32)((seed >> 8) % 9000u) : 16 + (i32)((seed >> 8) % 1500u);
        mg_dvm_new_array("[B", size);
        made += (u32)size + 32;
        if (!(++n & 7)) {
            u32 row = (seed >> 4) % NEAR_ROWS, col = (seed >> 13) % NEAR_COLS;
            g_near->u.oarr.v[row]->u.oarr.v[col] = near_new(row, col);
        }
        if (made >= 64u << 10) {
            mg_atomic_add(&g_near_made_kb, (i32)(made >> 10));
            made &= 1023;
        }
    }
    mg_atomic_add(&g_near_done, 1);
}

/* One run, on a thread of its own (near_driver), and what came of it. */
typedef struct near_run {
    u32 pace, started, damage;
    u64 before, collections, most_blocks, most_used, most_addresses, live0, live, ns;
    bool collected;
    mg_dvm_heap_info after;
} near_run;

static MG_HOSTCB void near_driver(void *p)
{
    near_run *n = (near_run *)p;
    mg_dvm_gc_result r;
    mg_dvm_heap_info s;
    u64 pause = 0, t0;
    u32 row, col;
    int t;
    mg_dvm_self();
    /* (What lives before the live set: the other tests' objects.) */
    n->collected = mg_dvm_gc("test near live, before it", &r);
    n->live0 = r.live_bytes;
    g_near = mg_dvm_new_array("[Ljava/lang/Object;", NEAR_ROWS);
    mg_dvm_add_root(&g_near);
    for (row = 0; row < NEAR_ROWS; row++) {
        mg_jobj *rw = mg_dvm_new_array("[Ljava/lang/Object;", NEAR_COLS);
        g_near->u.oarr.v[row] = rw;
        for (col = 0; col < NEAR_COLS; col++) rw->u.oarr.v[col] = near_new(row, col);
    }
    n->collected = mg_dvm_gc("test near live, before", &r) && n->collected;
    mg_dvm_gc_totals(&n->before, &pause);
    mg_dvm_gc_set_marker_pace(n->pace);
    mg_dvm_gc_set_alongside(true);
    g_near_made_kb = g_near_done = 0;
    t0 = mg_time_monotonic_ns();
    for (t = 0; t < NEAR_THREADS; t++) {
        mg_thread *th = mg_thread_start(near_main, (void *)(mg_addr)(t + 1), (size_t)256 << 10, "near");
        if (th) {
            mg_thread_detach(th);
            n->started++;
        }
    }
    while (g_near_done < (i32)n->started) {
        mg_dvm_heap_stats(&s);
        if (s.reserved > n->most_blocks) n->most_blocks = s.reserved;
        if (s.addresses > n->most_addresses) n->most_addresses = s.addresses;
        if (s.small_bytes + s.large_bytes > n->most_used) n->most_used = s.small_bytes + s.large_bytes;
        mg_sleep_ns(10000000ull);
    }
    n->ns = mg_time_monotonic_ns() - t0;
    mg_dvm_gc_alongside_wait();
    mg_dvm_gc_set_marker_pace(0);
    mg_dvm_gc_totals(&n->collections, &pause);
    n->collected = mg_dvm_gc("test near live, after", &r) && n->collected;
    n->live = r.live_bytes;
    mg_dvm_heap_stats(&n->after);
    n->damage = near_damage();
    mg_dvm_gc_set_alongside(false);
    g_near = NULL;
}

static void near_test(u32 pace)
{
    near_run n;
    mg_dvm_gc_result r;
    mg_dvm_heap_info gone;
    mg_thread *th;
    memset(&n, 0, sizeof(n));
    n.pace = pace;
    th = mg_thread_start(near_driver, &n, (size_t)256 << 10, "near driver");
    CHECK(th != NULL);
    if (!th) return;
    mg_thread_join(th);
    say("  %s: %u MB made in %.1f s by %u threads, %u collections; %.1f MB lived at the end: the heap's blocks at most %.1f MB (%.1f times "
        "that), in use at most %.1f MB, the blocks %.1f MB once collected; the arenas' addresses at most %u MB, %u MB once collected (%u "
        "given back so far)",
        pace ? "the markers starved" : "as the scheduler has it", (unsigned)((u32)g_near_made_kb >> 10), (double)n.ns / 1e9, (unsigned)n.started,
        (unsigned)(n.collections - n.before), (double)n.live / 1048576.0, (double)n.most_blocks / 1048576.0,
        n.live ? (double)n.most_blocks / (double)n.live : 0.0, (double)n.most_used / 1048576.0, (double)n.after.reserved / 1048576.0,
        (unsigned)(n.most_addresses >> 20), (unsigned)(n.after.addresses >> 20), (unsigned)n.after.released);
    CHECK(n.collected && n.started == NEAR_THREADS);
    CHECK_INT((i32)n.damage, 0);
    CHECK(n.collections > n.before);
    /*
     * The heap near what lives: what is in use at most what lives, two budgets (a budget is half of what lives) and what a
     * thread makes before it sees one due; the blocks a little more, the cells' rounding and the blocks half used; the
     * arenas' addresses as much again as two budgets of spare blocks (mg_dvm_heap_trim).
     */
    CHECK(n.most_used <= n.live * 5 / 2 + (24u << 20));
    CHECK(n.most_blocks <= n.live * 3 + (24u << 20));
    CHECK(n.most_addresses <= n.live * 3 + (32u << 20));
    /*
     * What lived let go, its thread gone: the arenas left empty past what the budget keeps (two budgets of spare blocks, 16 MB
     * at the least here, and 8 MB more) go back to the host, their addresses too.
     */
    CHECK(mg_dvm_gc("test near live, let go", &r));
    mg_dvm_heap_stats(&gone);
    say("  the live set let go: %.1f MB live (%.1f before it), the arenas' addresses %u MB, %u more given back", (double)r.live_bytes / 1048576.0,
        (double)n.live0 / 1048576.0, (unsigned)(gone.addresses >> 20), (unsigned)(gone.released - n.after.released));
    CHECK(n.live > n.live0 && r.live_bytes < n.live && n.live - r.live_bytes > (n.live - n.live0) / 4 * 3);
    CHECK(gone.addresses <= n.after.addresses);
    CHECK(gone.released > n.after.released || n.after.addresses <= (40u << 20));
}

static void test_gc_heap_near_live(void)
{
    say("the collector: the heap near what lives while four threads make garbage fast");
    mg_dvm_gc_set_poison(false);
    mg_dvm_gc_set_min_budget(8u << 20);
    near_test(0);
    near_test(NEAR_PACE);
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
    test_gc_parallel();
    test_gc_alongside();
    /* Where the objects so far were made (the samples of every 256 KB a thread makes), said and forgotten: and so said again with nothing new. */
    mg_dvm_made_say();
    mg_dvm_made_say();
    test_gc_gives_back();
    test_gc_budget_by_free_memory();
    test_gc_heap_near_live();
}
