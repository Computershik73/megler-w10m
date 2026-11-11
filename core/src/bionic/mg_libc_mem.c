/*
 * malloc and mmap for guest code.
 *
 * The guest frees with free() whatever malloc, memalign or posix_memalign gave
 * it, and the host's allocators do not all agree to that (an aligned block on
 * Windows has its own free). So every guest block carries a small header that
 * says where the host's block starts, and free() needs to know nothing else.
 * A thread that allocates a lot keeps the small blocks it frees in a cache of
 * its own, as jemalloc's threads do on Android ("each thread's cache", below).
 */
#include "mg_bionic.h"

#include <stdio.h>

#include "mg_fd.h"

#include "../mg_core.h"
#include "../ld/mg_ld.h"

typedef struct ghdr {
    u32 offset;            /* from the host block's start to the guest pointer */
    u32 size;              /* what the guest asked for */
} ghdr;

#define HDR(p) ((ghdr *)((u8 *)(p) - sizeof(ghdr)))

/*
 * What the guest has asked malloc for and not freed, in bytes, modulo 2^32 (mg_g_malloc_bytes): its blocks are the host's.
 * A thread with a cache counts its own in it (tcache.malloced); this word is the rest's, and the ended caches'.
 */
static volatile i32 g_malloced;

/*
 * The alignment of every block malloc gives, bionic's: Android 9's jemalloc hands out multiples of its quantum, 16 bytes,
 * but 8 on 32-bit ARM. Code built for the ABI counts on it -- x86-64's SSE stores (movaps) fault on a block of 8, and
 * exteraGram's libtmessages stopped so at its start when the header made every block 8 bytes off the host's 16.
 * The header is the last bytes of G_ALIGN before the guest's pointer, so a host block of G_ALIGN gives one of G_ALIGN.
 * The host's malloc gives two pointers' worth (Windows' MEMORY_ALLOCATION_ALIGNMENT, glibc's): less than G_ALIGN on 32-bit
 * x86, where every block is made as memalign's.
 */
#define G_ALIGN ((size_t)(MG_ARCH_ARM32 ? 8 : 16))
#define H_ALIGN (2 * sizeof(void *))
/*
 * And the size of every block is jemalloc's too, a multiple of that quantum and at least one: malloc(0) gives 16 bytes
 * (8 on 32-bit ARM), malloc(17) 32, and malloc_usable_size says so. Code that writes a little past what it asked for --
 * a terminating NUL into malloc(strlen(s)), a byte into malloc(0) -- writes into its own block on Android, and did here
 * into the next host block's header: MaxVibe's libqrcode.so writes a 0 into a block of malloc(0), which ended at the host
 * block's last byte once the header took G_ALIGN, and Windows found its heap corrupt a moment later, at another block
 * (c0000374, MaxVibe ending at its start one run in two on x64).
 */
#define USABLE(n) ((size_t)MG_ALIGN_UP((size_t)(n) ? (size_t)(n) : 1, G_ALIGN))
/*
 * A big block is at more than the quantum: jemalloc carves a block of a few hundred bytes or more from a run of its size
 * class, at a multiple of that class's own power of two, and gives one of a page or more a page of its own, so code
 * counts on such a block being at 16 or more on 32-bit ARM too -- zlib's NEON slide_hash in Geekbench 5's libgeekbench.so
 * loads its 64 KB hash table with vld1.16 {d16, d17}, [r2:128], an alignment fault at a block of 8 (the HP Elite x3, 688).
 * Every block of BIG_BLOCK bytes or more is at BIG_ALIGN here, the most a NEON load's hint asks for (32) and a cache line.
 */
#define BIG_BLOCK ((size_t)256)
#define BIG_ALIGN ((size_t)64)
static size_t block_align(size_t n) { return USABLE(n) >= BIG_BLOCK ? BIG_ALIGN : G_ALIGN; }

/*
 * The biggest block a thread's cache keeps (below), and the host's bytes behind a block of usable size u: u, but from
 * BIG_BLOCK to TC_MAX the top of u's bin there, eight to a doubling (264 to 288 bytes are 288, 2049 to 2304 are 2304), so
 * that every block of a bin is big enough for anything asked of the bin. A few per cent of those blocks' bytes; malloc_usable_size
 * stays USABLE.
 */
#define TC_MAX ((size_t)4096)
static size_t host_usable(size_t u)
{
    size_t step = BIG_BLOCK / 8;
    if (u <= BIG_BLOCK || u > TC_MAX) return u;
    while (u - 1 >= step * 16) step *= 2;
    return MG_ALIGN_UP(u, step);
}

/*
 * The flag malloc-guard, a diagnosis (64-bit processors; off by default): every block is followed by GUARD bytes of 0xFD
 * after its usable size, which free and realloc look at first, and the code that asked for the block (its caller's return
 * address) is kept in the spare bytes before the header. A block written past its end is said -- its size, how far past
 * and with what, and which library asked for it, usually the one that wrote -- and its guard is made whole again. Windows'
 * heap finds such damage only later, at another block, with nothing to say whose it was: MaxVibe ended so one run in two
 * (c0000374 in a calloc of the core's), and the guard found its libqrcode.so writing a byte into a block of malloc(0),
 * which had no byte to write then (USABLE above).
 */
/* Where the function this is written in returns to: called by a guest, an address in the guest's code (as mg_egl.c's). */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define CALLER() _ReturnAddress()
#else
#define CALLER() __builtin_return_address(0)
#endif

#define GUARD 64
static volatile i32 g_guard = -1;
static void guard_check(void *p, const char *at);
static MG_HOSTCB void guard_watch(void *unused);

static bool guarding(void)
{
    /* (Decided once, before the first block: a block made without a guard is never looked at for one.) */
    if (g_guard < 0) {
        bool on = H_ALIGN >= G_ALIGN && mg_flag("malloc-guard");
        if (mg_atomic_cas(&g_guard, -1, on ? 1 : 0) == -1 && on) {
            mg_thread *t = mg_thread_start(guard_watch, NULL, 64u * 1024u, "malloc-guard");
            if (t) mg_thread_detach(t);
        }
    }
    return g_guard > 0;
}

/*
 * The living guarded blocks, in a set (open addressing, removed ones marked): a thread looks at every one's guard each
 * second, for a block written past its end that is not freed while the app runs.
 */
#define LIVE_GONE ((void *)1)
static void **g_live;
static size_t g_live_cap, g_live_used;
static mg_lock g_live_lock = MG_LOCK_INIT;

static size_t live_slot(void *p, size_t cap) { return (size_t)(((u64)(mg_addr)p >> 4) * 0x9E3779B97F4A7C15ull >> 20) & (cap - 1); }

static void live_add(void *p)
{
    size_t i;
    mg_lock_acquire(&g_live_lock);
    if ((g_live_used + 1) * 2 > g_live_cap) {
        size_t cap = g_live_cap ? g_live_cap * 2 : 65536, k;
        void **was = g_live, **now = (void **)calloc(cap, sizeof(void *));
        if (!now) {
            mg_lock_release(&g_live_lock);
            return;
        }
        g_live_used = 0;
        for (k = 0; k < g_live_cap; k++)
            if (was[k] && was[k] != LIVE_GONE) {
                for (i = live_slot(was[k], cap); now[i]; i = (i + 1) & (cap - 1)) {}
                now[i] = was[k];
                g_live_used++;
            }
        free(was);
        g_live = now;
        g_live_cap = cap;
    }
    for (i = live_slot(p, g_live_cap); g_live[i] && g_live[i] != LIVE_GONE; i = (i + 1) & (g_live_cap - 1)) {}
    if (!g_live[i]) g_live_used++;
    g_live[i] = p;
    mg_lock_release(&g_live_lock);
}

static void live_remove(void *p)
{
    size_t i;
    mg_lock_acquire(&g_live_lock);
    if (g_live_cap)
        for (i = live_slot(p, g_live_cap); g_live[i]; i = (i + 1) & (g_live_cap - 1))
            if (g_live[i] == p) {
                g_live[i] = LIVE_GONE;
                break;
            }
    mg_lock_release(&g_live_lock);
}

static MG_HOSTCB void guard_watch(void *unused)
{
    (void)unused;
    mg_info("malloc-guard: every block malloc gives has a guard of %d bytes after it, looked at when it is freed and each "
            "second while it lives; freed blocks are kept poisoned, %d of them, before they are given back",
            GUARD, 8192);
    for (;;) {
        size_t k;
        mg_sleep_ns(1000000000ull);
        mg_lock_acquire(&g_live_lock);
        for (k = 0; k < g_live_cap; k++)
            if (g_live[k] && g_live[k] != LIVE_GONE) guard_check(g_live[k], "a look at the living");
        mg_lock_release(&g_live_lock);
    }
}

/* Where code is, said for the guard: library+offset and symbol. */
static void say_code(char *out, size_t n, mg_addr pc)
{
    const char *lib = NULL, *sym = NULL;
    const void *sym_addr = NULL, *base = NULL;
    if (pc) sym = mg_ld_symbol_at((const void *)pc, &lib, &sym_addr, &base);
    snprintf(out, n, "%p (%s+0x%lx, %s)", (void *)pc, lib ? lib : "?", (unsigned long)(base ? pc - (mg_addr)base : 0), sym ? sym : "?");
}

static void guard_check(void *p, const char *at)
{
    u8 *g;
    u32 first = GUARD, last = 0, i, off = HDR(p)->offset;
    char bytes[3 * 16 + 1], asked[200];
    if (off < sizeof(ghdr) || off > 2 * MG_PAGE) {
        /* (Not a header malloc wrote: the block's size cannot be taken from it, nor its guard looked at.) */
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) < 20) mg_err("malloc-guard: at %s, the header before the block at %p was overwritten (%08x %08x)", at, p, off, HDR(p)->size);
        return;
    }
    g = (u8 *)p + USABLE(HDR(p)->size);
    for (i = 0; i < GUARD; i++)
        if (g[i] != 0xFD) {
            if (first == GUARD) first = i;
            last = i;
        }
    if (first == GUARD) return;
    for (i = 0; i < 16 && first + i <= last; i++) snprintf(bytes + 3 * i, 4, "%02x ", g[first + i]);
    bytes[3 * i ? 3 * i - 1 : 0] = 0;
    say_code(asked, sizeof(asked), off >= 16 ? *(mg_addr *)((u8 *)p - off) : 0);
    mg_err("malloc-guard: at %s, a block of %u bytes at %p (%u usable) was written past its end, %u to %u bytes after it (%s); "
           "asked for at %s",
           at, (unsigned)HDR(p)->size, p, (unsigned)USABLE(HDR(p)->size), (unsigned)first, (unsigned)last, bytes, asked);
    memset(g, 0xFD, GUARD);
}


/*
 * And the freed: a guarded block is not given back at once but kept in a ring of QUARANTINE, its first POISONED bytes set
 * to 0xDD and its header marked FREED (its own offset and who freed it kept in its guard); a second free of it is said
 * and not done, and when it leaves the ring its bytes are looked at -- a block written after it was freed is said, with
 * who asked for it and who freed it. Blocks of a megabyte and more are given back at once.
 */
#define QUARANTINE 8192
#define POISONED 4096
#define FREED 0xF4EEF4EEu
static void *g_quarantine[QUARANTINE];
static u32 g_quarantine_next;
static mg_lock g_quarantine_lock = MG_LOCK_INIT;

static void release(void *p)
{
    u8 *g;
    u32 off, size = HDR(p)->size, n = size < POISONED ? size : POISONED, i;
    mg_addr freer, caller;
    if (HDR(p)->offset != FREED) {
        mg_err("malloc-guard: the header of the freed block at %p was overwritten after it was freed (%08x %08x); the block is kept",
               p, HDR(p)->offset, size);
        return;
    }
    g = (u8 *)p + USABLE(size);
    memcpy(&off, g, 4);
    memcpy(&freer, g + 8, sizeof(freer));
    for (i = 0; i < n && ((u8 *)p)[i] == 0xDD; i++) {}
    if (i < n) {
        char asked[200], freed[200];
        caller = off >= 16 ? *(mg_addr *)((u8 *)p - off) : 0;
        say_code(asked, sizeof(asked), caller);
        say_code(freed, sizeof(freed), freer);
        mg_err("malloc-guard: a block of %u bytes at %p was written after it was freed, first at +%u (%02x); asked for at %s, "
               "freed at %s", (unsigned)size, p, (unsigned)i, ((u8 *)p)[i], asked, freed);
    }
    free((u8 *)p - off);
}

/* free of a guarded block: false when it was freed already (said). */
static bool guarded_free(void *p, mg_addr freer)
{
    u8 *g;
    u32 off = HDR(p)->offset;
    void *out = NULL;
    if (off == FREED) {
        char freed[200], again[200];
        mg_addr first;
        memcpy(&first, (u8 *)p + USABLE(HDR(p)->size) + 8, sizeof(first));
        say_code(freed, sizeof(freed), first);
        say_code(again, sizeof(again), freer);
        mg_err("malloc-guard: a block of %u bytes at %p was freed twice: first at %s, again at %s", (unsigned)HDR(p)->size, p, freed,
               again);
        return false;
    }
    live_remove(p);
    guard_check(p, "free");
    if (HDR(p)->size >= (1u << 20)) {
        free((u8 *)p - off);
        return true;
    }
    g = (u8 *)p + USABLE(HDR(p)->size);
    memcpy(g, &off, 4);
    memcpy(g + 8, &freer, sizeof(freer));
    HDR(p)->offset = FREED;
    memset(p, 0xDD, HDR(p)->size < POISONED ? HDR(p)->size : POISONED);
    mg_lock_acquire(&g_quarantine_lock);
    out = g_quarantine[g_quarantine_next];
    g_quarantine[g_quarantine_next] = p;
    g_quarantine_next = (g_quarantine_next + 1) % QUARANTINE;
    mg_lock_release(&g_quarantine_lock);
    if (out) release(out);
    return true;
}

/*
 * Each thread's cache of the blocks it has freed, as Android's jemalloc keeps one for each thread (its tcache): a thread
 * that allocates a lot takes its small blocks from its cache and frees them into it, touching neither the host's heap nor
 * anything another thread touches. Before, every block came from the host's malloc and went back to its free, and every
 * one counted its bytes into one word that every thread added to: threads that allocated at once waited for each other
 * there, and in the heap's own lock where the heap is the NT heap. On this PC (x64, the NT heap) four threads freeing and
 * allocating blocks of mixed sizes made 1.4 to 1.6 times what one made, eight fewer than one, and four freeing and
 * allocating small pairs 1.26 times (mg_tests --malloc-bench); Geekbench 5's multi-core workloads, threads of C++ that
 * allocate all the time, scaled worse on the HP Elite x3 than Android's on the same phone (690).
 *
 * A cache keeps blocks of up to TC_MAX bytes in bins: one bin for each usable size below BIG_BLOCK, any block of which is
 * as good as any other, and eight to a doubling above it, whose blocks the host makes of the bin's top size (host_usable).
 * A bin keeps up to 16 blocks (8 above 128 bytes, 4 above a kilobyte), a cache TC_BYTES of the host's at most; a full bin
 * gives its older half back to the host, and every TC_TRIM takes and puts the cache gives back half of what each bin did
 * not need since the last time. A block in a cache is still the host's whole block, its header kept: any thread's cache
 * takes it, or the host's free, so a block freed by another thread than the one that made it -- a consumer's of a
 * producer's -- is no different, and nothing is owned. Big blocks, memalign's at more than a block's own alignment and
 * malloc-guard's are the host's at once.
 *
 * Only a thread that allocates fast has a cache: TC_WARM mallocs within TC_WARM_MS milliseconds. The hundreds of threads
 * an app has that seldom allocate keep none, and allocate as before. A thread that ends gives its cache back to the host,
 * whoever started it (mg_tls_alloc_ending: a fiber-local slot's callback on Windows). What a thread with a cache asks for
 * and frees is counted in its cache, and mg_g_malloc_bytes adds the caches up. The flag no-malloc-cache: no thread has
 * one, and malloc is as it was before (to tell its effect on a phone); none either while malloc-guard is on.
 */
#define TC_SMALL ((u32)(BIG_BLOCK / G_ALIGN) - 1)
#define TC_BINS (TC_SMALL + 33)
#define TC_BYTES ((u32)32768)
#define TC_TRIM 1024u
#define TC_WARM 256u
#define TC_WARM_MS 100u

typedef struct tcache {
    void *head[TC_BINS];               /* each bin's blocks, the last freed first, each linked through its first word */
    u8 count[TC_BINS];
    u8 low[TC_BINS];                   /* the fewest the bin has had since the last trim */
    u32 bytes;                         /* the host's bytes behind its blocks */
    u32 events;                        /* takes and puts since the last trim */
    volatile i32 malloced;             /* the thread's part of g_malloced */
    struct tcache *next, *prev;
} tcache;

static MG_THREAD_LOCAL tcache *t_cache;
/* Towards a cache: the mallocs since `t_since` (milliseconds), and whether the thread's cache has gone with its end. */
static MG_THREAD_LOCAL u32 t_mallocs, t_since;
static MG_THREAD_LOCAL bool t_ended;

static tcache *g_caches;
static u32 g_cache_count;
static bool g_cache_slot_made;
static u32 g_cache_slot;
static mg_lock g_caches_lock = MG_LOCK_INIT;
/* Whether threads may have caches: -1 not decided yet (the flag no-malloc-cache), and the tests' mg_g_malloc_caches_allow. */
static volatile i32 g_caches_allowed = -1;

/* The bin of usable size u, G_ALIGN to TC_MAX: one for each size below BIG_BLOCK (2^8), then eight to a doubling. */
static u32 tc_bin(size_t u)
{
    u32 k = 8;
    if (u < BIG_BLOCK) return (u32)(u / G_ALIGN) - 1;
    if (u == BIG_BLOCK) return TC_SMALL;
    while ((u - 1) >> (k + 1)) k++;
    return TC_SMALL + 1 + (k - 8) * 8 + (u32)((u - 1) >> (k - 3)) - 8;
}

/* The host's bytes behind each block of bin b: host_usable of its sizes. */
static u32 tc_bin_bytes(u32 b)
{
    u32 r;
    if (b < TC_SMALL) return (b + 1) * (u32)G_ALIGN;
    if (b == TC_SMALL) return (u32)BIG_BLOCK;
    r = b - TC_SMALL - 1;
    return (r % 8 + 9) << (r / 8 + 5);
}

static void host_release(void *p) { free((u8 *)p - HDR(p)->offset); }

/* The `n` blocks of bin b freed longest ago given back to the host. */
static void bin_release(tcache *tc, u32 b, u32 n)
{
    void **link = &tc->head[b], *p;
    u32 keep = tc->count[b] - n, i;
    for (i = 0; i < keep; i++) link = (void **)*link;
    p = *link;
    *link = NULL;
    while (p) {
        void *next = *(void **)p;
        host_release(p);
        p = next;
    }
    tc->count[b] = (u8)keep;
    if (tc->low[b] > keep) tc->low[b] = (u8)keep;
    tc->bytes -= n * tc_bin_bytes(b);
}

/* Half of what each bin did not need since the last trim given back. */
static void cache_trim(tcache *tc)
{
    u32 b;
    tc->events = 0;
    for (b = 0; b < TC_BINS; b++) {
        if (tc->low[b]) bin_release(tc, b, (tc->low[b] + 1u) / 2u);
        tc->low[b] = tc->count[b];
    }
}

/* A block of n bytes from the cache, or NULL. */
static void *cache_take(tcache *tc, size_t n)
{
    size_t u = USABLE(n);
    u32 b;
    u8 *p;
    if (u > TC_MAX) return NULL;
    if (++tc->events >= TC_TRIM) cache_trim(tc);
    b = tc_bin(u);
    p = (u8 *)tc->head[b];
    if (!p) return NULL;
    tc->head[b] = *(void **)p;
    if (--tc->count[b] < tc->low[b]) tc->low[b] = tc->count[b];
    tc->bytes -= tc_bin_bytes(b);
    HDR(p)->size = (u32)n;
    tc->malloced += (i32)n;
    return p;
}

/* The freed block p, of `size` bytes, kept in the cache: false when it is the host's to free. */
static bool cache_put(tcache *tc, void *p, u32 size)
{
    size_t u = USABLE(size), align;
    u32 b, bytes, room;
    if (u > TC_MAX) return false;
    /* (A memalign's block made at more than a block of its size is, or at less than a big block's: the host's.) */
    align = u >= BIG_BLOCK ? BIG_ALIGN : G_ALIGN;
    if (HDR(p)->offset > align + sizeof(ghdr) || ((mg_addr)p & (align - 1))) return false;
    if (++tc->events >= TC_TRIM) cache_trim(tc);
    b = tc_bin(u);
    if (tc->head[b] == p) {
        /* (Freed again, the second time right after the first: two mallocs to come would have had it each.) */
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 5) mg_warn("malloc: the block at %p (%u bytes) was freed twice in a row; the second free is left undone", p, (unsigned)size);
        return true;
    }
    bytes = tc_bin_bytes(b);
    room = bytes <= 128 ? 16 : bytes <= 1024 ? 8 : 4;
    if (tc->count[b] >= room) bin_release(tc, b, room / 2);
    if (tc->bytes + bytes > TC_BYTES) return false;
    *(void **)p = tc->head[b];
    tc->head[b] = p;
    tc->count[b]++;
    tc->bytes += bytes;
    tc->malloced -= (i32)size;
    return true;
}

/* Every block of the cache given back to the host, its count to g_malloced, and the cache freed. */
static void cache_drop(tcache *tc)
{
    u32 b;
    for (b = 0; b < TC_BINS; b++)
        if (tc->count[b]) bin_release(tc, b, tc->count[b]);
    mg_lock_acquire(&g_caches_lock);
    if (tc->prev) tc->prev->next = tc->next;
    else g_caches = tc->next;
    if (tc->next) tc->next->prev = tc->prev;
    g_cache_count--;
    mg_atomic_add(&g_malloced, tc->malloced);
    mg_lock_release(&g_caches_lock);
    free(tc);
}

/* The thread ends (any thread: the host's slot with an end): its cache goes. */
static MG_HOSTCB void cache_end(void *v)
{
    tcache *tc = (tcache *)v;
#if !defined(MG_CORE_ELF)
    /* (The ELF core's thread-local variables are fiber-local data too, whose end may have come before this one: left alone there.) */
    if (t_cache == tc) {
        t_cache = NULL;
        t_ended = true;
    }
#endif
    cache_drop(tc);
}

/* A malloc on a thread with no cache: one made for it once it has allocated fast enough (TC_WARM in TC_WARM_MS), or NULL. */
static MG_NOINLINE tcache *cache_warm(void)
{
    tcache *tc;
    u32 now;
    bool first = false;
    if (++t_mallocs < TC_WARM) return NULL;
    t_mallocs = 0;
    now = (u32)(mg_time_monotonic_ns() / 1000000u);
    if (now - t_since > TC_WARM_MS || t_ended) {
        t_since = now;
        return NULL;
    }
    if (g_caches_allowed < 0) g_caches_allowed = mg_flag("no-malloc-cache") ? 0 : 1;
    if (!g_caches_allowed) return NULL;
    tc = (tcache *)calloc(1, sizeof(*tc));
    if (!tc) return NULL;
    mg_lock_acquire(&g_caches_lock);
    if (!g_cache_slot_made) {
        g_cache_slot = mg_tls_alloc_ending(cache_end);
        g_cache_slot_made = first = true;
    }
    tc->next = g_caches;
    if (g_caches) g_caches->prev = tc;
    g_caches = tc;
    g_cache_count++;
    mg_lock_release(&g_caches_lock);
    mg_tls_set_ending(g_cache_slot, tc);
    t_cache = tc;
    if (first)
        mg_info("malloc: thread %u allocates fast enough to keep a cache of the small blocks it frees, the first to (the host's malloc "
                "takes its blocks from %s)", (unsigned)mg_thread_self_id(), mg_heap_kind());
    return tc;
}

/* The calling thread's cache, or NULL: none while malloc-guard is on. */
static tcache *thread_cache(void)
{
    tcache *tc;
    if (guarding()) return NULL;
    tc = t_cache;
    return tc ? tc : cache_warm();
}

/* The thread's part of g_malloced: its cache's, or the word all threads without one add to. */
static void counted(i32 delta)
{
    tcache *tc = t_cache;
    if (tc) tc->malloced += delta;
    else mg_atomic_add(&g_malloced, delta);
}

u64 mg_g_malloc_bytes(void)
{
    tcache *tc;
    u32 sum;
    mg_lock_acquire(&g_caches_lock);
    sum = (u32)mg_atomic_add(&g_malloced, 0);
    for (tc = g_caches; tc; tc = tc->next) sum += (u32)tc->malloced;
    mg_lock_release(&g_caches_lock);
    return sum;
}

void mg_g_malloc_caches(u32 *threads, u64 *bytes)
{
    tcache *tc;
    *bytes = 0;
    mg_lock_acquire(&g_caches_lock);
    *threads = g_cache_count;
    for (tc = g_caches; tc; tc = tc->next) *bytes += tc->bytes;
    mg_lock_release(&g_caches_lock);
}

void mg_g_malloc_caches_allow(int allow) { mg_atomic_xchg(&g_caches_allowed, allow < 0 ? -1 : allow ? 1 : 0); }

/* A block of n bytes at a multiple of align, its header right before it, from a host block made bigger to hold both
   (zeroed by the host for calloc's). */
static void *aligned_block(size_t align, size_t n, bool zero, mg_addr caller)
{
    size_t extra = guarding() ? GUARD : 0, bytes;
    u8 *raw, *p;
    if ((align & (align - 1)) || n > 0x7FFFFF00u - align) {
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    bytes = host_usable(USABLE(n)) + align + sizeof(ghdr) + extra;
    raw = (u8 *)(zero ? calloc(1, bytes) : malloc(bytes));
    if (!raw) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    p = (u8 *)MG_ALIGN_UP((mg_addr)raw + sizeof(ghdr), (mg_addr)align);
    HDR(p)->offset = (u32)(p - raw);
    HDR(p)->size = (u32)n;
    if (extra) {
        if (p - raw >= 16) *(mg_addr *)raw = caller;
        memset(p + USABLE(n), 0xFD, GUARD);
        live_add(p);
    }
    counted((i32)n);
    return p;
}

/* malloc's block (zeroed for calloc's): at G_ALIGN after the host's, or made as memalign's where the host gives less or
   the block is big (BIG_BLOCK). */
static void *plain_block(size_t n, bool zero, mg_addr caller)
{
    size_t extra = guarding() ? GUARD : 0, align = block_align(n);
    u8 *raw;
    if (H_ALIGN < G_ALIGN || align > G_ALIGN) return aligned_block(align, n, zero, caller);
    raw = (u8 *)(zero ? calloc(1, USABLE(n) + G_ALIGN + extra) : malloc(USABLE(n) + G_ALIGN + extra));
    if (!raw) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    HDR(raw + G_ALIGN)->offset = (u32)G_ALIGN;
    HDR(raw + G_ALIGN)->size = (u32)n;
    if (extra) {
        *(mg_addr *)raw = caller;
        memset(raw + G_ALIGN + USABLE(n), 0xFD, GUARD);
        live_add(raw + G_ALIGN);
    }
    counted((i32)n);
    return raw + G_ALIGN;
}

void *mg_g_malloc(size_t n)
{
    tcache *tc;
    if (n > 0x7FFFFF00u) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    if ((tc = thread_cache()) != NULL) {
        void *p = cache_take(tc, n);
        if (p) return p;
    }
    return plain_block(n, false, (mg_addr)CALLER());
}

void *mg_g_calloc(size_t n, size_t m)
{
    u64 total = (u64)n * (u64)m;
    tcache *tc;
    if (total > 0x7FFFFF00u) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    if ((tc = thread_cache()) != NULL) {
        void *p = cache_take(tc, (size_t)total);
        if (p) return memset(p, 0, USABLE((size_t)total));
    }
    return plain_block((size_t)total, true, (mg_addr)CALLER());
}

void mg_g_free(void *p)
{
    u32 size;
    tcache *tc;
    if (!p) return;
    size = HDR(p)->size;
    if (guarding()) {
        if (guarded_free(p, (mg_addr)CALLER())) counted(-(i32)size);
        return;
    }
    tc = t_cache;
    if (tc) {
        if (cache_put(tc, p, size)) return;
        tc->malloced -= (i32)size;
    } else {
        mg_atomic_add(&g_malloced, -(i32)size);
    }
    free((u8 *)p - HDR(p)->offset);
}

/*
 * memalign's block: at `align`, and at least where malloc's of its size is -- a big block at BIG_ALIGN. memalign(16, 300)
 * on 32-bit ARM was at 16 alone, where malloc(300) is at 64 and jemalloc's run of 320-byte blocks leaves one at 64 too.
 */
static void *g_memalign(size_t align, size_t n)
{
    if (align <= G_ALIGN) return mg_g_malloc(n);
    if (align < block_align(n)) align = block_align(n);
    return aligned_block(align, n, false, (mg_addr)CALLER());
}

void *mg_g_realloc(void *p, size_t n)
{
    void *q;
    u32 old;
    if (!p) return mg_g_malloc(n);
    if (!n) {
        mg_g_free(p);
        return NULL;
    }
    if (n > 0x7FFFFF00u) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    /* The host's bytes behind it the same, and its place what its new size asks for: it stays, the host not asked. */
    if (!guarding() && host_usable(USABLE(n)) == host_usable(USABLE(HDR(p)->size)) && !((mg_addr)p & (block_align(n) - 1))) {
        counted((i32)((u32)n - HDR(p)->size));
        HDR(p)->size = (u32)n;
        return p;
    }
    /* The host's realloc, in place where it can: a block at the quantum or at BIG_ALIGN (or a memalign's of no more) stays
       one, at what its new size asks for (block_align); where the host's new block puts it elsewhere, it is moved inside. */
    if (!guarding() && H_ALIGN >= G_ALIGN && HDR(p)->offset >= G_ALIGN && HDR(p)->offset <= BIG_ALIGN + sizeof(ghdr)) {
        u32 was = HDR(p)->size, off = HDR(p)->offset;
        size_t align = block_align(n), keep = USABLE(was) < USABLE(n) ? USABLE(was) : USABLE(n), need;
        u8 *raw = (u8 *)p - off, *grown;
        if (align == G_ALIGN) {
            /* (Its contents first where the smaller block keeps them, G_ALIGN in, and back if the host cannot.) */
            if (off != G_ALIGN) memmove(raw + G_ALIGN, p, keep);
            grown = (u8 *)realloc(raw, USABLE(n) + G_ALIGN);
            if (!grown) {
                if (off != G_ALIGN) memmove(p, raw + G_ALIGN, keep);
                mg_set_errno(GE_NOMEM);
                return NULL;
            }
            q = grown + G_ALIGN;
        } else {
            need = host_usable(USABLE(n)) + align + sizeof(ghdr);
            if (need < off + keep) need = off + keep;
            grown = (u8 *)realloc(raw, need);
            if (!grown) {
                mg_set_errno(GE_NOMEM);
                return NULL;
            }
            q = (u8 *)MG_ALIGN_UP((mg_addr)grown + sizeof(ghdr), (mg_addr)align);
            if (q != grown + off) memmove(q, grown + off, keep);
        }
        HDR(q)->offset = (u32)((u8 *)q - grown);
        HDR(q)->size = (u32)n;
        counted((i32)((u32)n - was));
        return q;
    }
    /* An aligned block: the host cannot grow it in place and keep the alignment (nor a block's guard). */
    old = HDR(p)->size;
    q = plain_block(n, false, (mg_addr)CALLER());
    if (!q) return NULL;
    memcpy(q, p, USABLE(old) < USABLE(n) ? USABLE(old) : USABLE(n));
    if (guarding()) {
        if (guarded_free(p, (mg_addr)CALLER())) counted(-(i32)old);
    } else {
        mg_g_free(p);
    }
    return q;
}

char *mg_g_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)mg_g_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static int g_posix_memalign(void **out, size_t align, size_t n)
{
    void *p;
    if (align < sizeof(void *) || (align & (align - 1))) return GE_INVAL;
    p = g_memalign(align, n);
    if (!p) return GE_NOMEM;
    *out = p;
    return 0;
}

static void *g_aligned_alloc(size_t align, size_t n) { return g_memalign(align, n); }
static void *g_valloc(size_t n) { return g_memalign(MG_PAGE, n); }
static void *g_pvalloc(size_t n) { return g_memalign(MG_PAGE, MG_ALIGN_UP(n, MG_PAGE)); }
static size_t g_malloc_usable_size(const void *p) { return p ? USABLE(HDR(p)->size) : 0; }
static int g_mallopt(int param, int value) { (void)param; (void)value; return 1; }
static int g_malloc_trim(size_t pad) { (void)pad; return 0; }

/* bionic's struct mallinfo: ten size_t's, returned by value. */
typedef struct g_mallinfo_t {
    size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
} g_mallinfo_t;

static g_mallinfo_t g_mallinfo(void)
{
    g_mallinfo_t m;
    u64 used = mg_mem_usage();
    memset(&m, 0, sizeof(m));
#if MG_GUEST_LP64
    m.uordblks = (size_t)used;
#else
    m.uordblks = used > 0xFFFFFFFFu ? 0xFFFFFFFFu : (size_t)used;
#endif
    m.arena = m.uordblks;
    return m;
}

static void *g_reallocarray(void *p, size_t n, size_t m)
{
    u64 total = (u64)n * (u64)m;
    if (total > 0x7FFFFF00u) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    return mg_g_realloc(p, (size_t)total);
}

/*
 * The C++ runtime's allocation functions, for apps that link the system's minimal libstdc++. Their names say size_t's
 * type as the Itanium ABI mangles it: unsigned int (j) in 32-bit Android, unsigned long (m) in 64-bit.
 */
#if MG_GUEST_LP64
#define CXX_SIZE "m"
#else
#define CXX_SIZE "j"
#endif
static void *g_new(size_t n)
{
    void *p = mg_g_malloc(n ? n : 1);
    if (!p) mg_fatal("guest: operator new(%u) failed", (unsigned)n);
    return p;
}
static void *g_new_nothrow(size_t n, const void *tag) { (void)tag; return mg_g_malloc(n ? n : 1); }
static void g_delete(void *p) { mg_g_free(p); }
static void g_delete_nothrow(void *p, const void *tag) { (void)tag; mg_g_free(p); }
static void g_delete_sized(void *p, size_t n) { (void)n; mg_g_free(p); }

/* -------------------------------------------------------------------- mmap
 *
 * Anonymous memory and private copies of files, and shared memory: a MAP_SHARED mapping of an ashmem region or a memfd is
 * a view of the region's own pages (mg_fd_shm_map), so that every mapping of it, in any thread, is the same memory, as on
 * Linux. Chromium moves everything between its browser and its renderer through such regions (Mojo's data pipes, which
 * carry a page's bytes), mapping one twice: with each mapping a copy of the region, the renderer read nothing of what the
 * network wrote, and every page the WebView loaded was empty. A region asked for as
 * writable and executable at once -- a JIT's code heap -- is so on the host where
 * the phone allows it (mg_mem_rwx, the plain VirtualProtect; flag no-rwx: never),
 * and V8's code, written while it runs, costs no fault. Where it does not, it is
 * writable until something executes in it and executable until something writes
 * to it: the fault handler flips the page (mg_mmap_fix_fault), a fault and a
 * flip for every write after a run.
 * So are the pages of the app's own libraries it asks so of, to patch its code
 * (an inline hook: Instagram's libdistract rewrites a function of its
 * libc++_shared.so, and ended on the write while the page stayed executable).
 *
 * Anonymous memory mapped with no access (PROT_NONE) is address space and nothing more, as on Linux: reserved, not
 * committed, until mprotect gives some of it access (committed then, zeroed the first time); mapped again over itself
 * with no access (MAP_FIXED, PROT_NONE), what was there is given back, as Linux gives zeroed pages for it. Allocators
 * reserve that way and commit what they use: Chromium's PartitionAlloc its 2 MB super pages, a Chromium browser's linker
 * the range its library goes into. Each was committed whole before, and Windows counts committed memory against the app's
 * limit whether it is touched or not: a WebView in Spotify ended out of memory with 1586 MB committed and 307 MB of
 * address space free.
 *
 * A file mapped read-only and shared (MAP_SHARED, PROT_READ, the descriptor opened read-only) is a view of the file
 * itself (mg_fd_file_map), which costs the app no memory of its own: the WebView maps its ICU data and its .pak files so,
 * 55 MB of them, each read into memory of the app's before. Any other mapping of a file is a copy of what it holds.
 *
 * Anonymous memory an allocator is done with and gives back with madvise (MADV_DONTNEED, MADV_FREE) is the system's
 * again: its readable and writable pages are decommitted, and stay the app's -- touched again, a page is committed again,
 * zero, as Linux gives it (mg_mmap_fix_fault), and so is one the system itself is about to write into, which would refuse
 * rather than fault (before_system_write). Chromium's PartitionAlloc gives its free pages back so, and V8 the pages of its
 * heap; madvise did nothing before, and Play Store with Google's sign-in page had 1593 MB committed of its 2 GB when the
 * WebView ended. PartitionAlloc also counts on a page given back being zero when it is used again, as a block that has
 * to be zero is then not cleared.
 *
 * Save Go's (madvise by number from a library of Go's, mg_ld_lib_is_go): its runtime cuts its goroutines' stacks from its
 * heap, whose free pages its scavenger gives back so, and soon puts a goroutine on such pages; the first touch of one is
 * then the goroutine's own, on its stack, with the stack pointer on that page, and Windows lays a fault's records right
 * below the stack pointer, there being no other stack for them: it cannot, and ends the process before any handler runs
 * (a Go library's 1000 goroutines with 64 KB frames: every run that gave its heap back between two rounds of them). Go
 * says nothing when it uses such a page again (on Linux it need not), and GODEBUG=harddecommit, which would have it map
 * them again first, never reaches a library of Go's on Android: its _rt0_*_android_lib hands the runtime an argv of
 * "gojni" and no environment. Its pages are given back and committed again at once: zero when next touched, their
 * memory the system's, still counted against the commit limit. (POSIX's fault handler has a stack of its own,
 * host/mg_host_posix.c.)
 */
#define G_PROT_READ 1
#define G_PROT_WRITE 2
#define G_PROT_EXEC 4
#define G_MAP_SHARED 1
#define G_MAP_FIXED 0x10
#define G_MAP_ANONYMOUS 0x20
#define G_MAP_FAILED ((void *)(mg_addr)-1)

typedef struct region {
    u8 *base;
    size_t size;
    bool wx;               /* some page was asked for as writable and executable */
    /*
     * Which pages the app still has, a bit each. The host gives address
     * space back only a whole reservation at a time, and apps unmap in
     * pieces (an allocator maps a little more than it wants, unmaps the end
     * to line the block up, and unmaps the rest when it is done with it):
     * the reservation goes when its last page has. Without this count the
     * second unmapping looked like one more piece, and the addresses of
     * every big block an engine ever freed stayed taken (755 MB of a 2 GB
     * process, with Unity, half a minute in).
     */
    u8 *live;
    u32 live_pages;
    /*
     * A shared mapping of a file (MAP_SHARED) that is the layer's own copy of its bytes: the file, held, and which of its
     * bytes the region holds from its base. What the app writes into a writable one (PROT_WRITE, `write_back`) is written
     * back to the file at msync, at munmap, and when the app ends (mg_mem_write_back) -- not as it is written, as the
     * kernel would. What is written to the file through a descriptor is copied into every such mapping of it as it is
     * written (mg_mem_file_written), into pages made writable for the copy where they are not: `file_prot` is the access
     * they were mapped with, for a host that cannot say a page's (mg_mem_page_state).
     */
    void *file;
    u64 file_offset;
    size_t file_bytes;
    bool write_back;
    int file_prot;
    /* A view of shared memory (mg_fd_shm_map): unmapped, not released, when its last page goes; its pages stay till then. */
    void *view;
    size_t view_size;
    /* Anonymous memory, all of it (no file was mapped into it): what madvise may give back. */
    bool anon;
    /* Which pages madvise gave back and nothing has touched since, a bit each (gone_mark); NULL until the first. */
    u8 *gone;
    u32 gone_pages;
    /* Which pages the app made no-access itself (mmap or mprotect with PROT_NONE), a bit each: a fault there is the app's. */
    u8 *noaccess;
    /*
     * The library whose code mapped it (a system call's place by number, else libc's caller; NULL: none known, the layer's
     * own among them), and whether that was Go's runtime, by number from a library of Go's: whose the census of the
     * addresses says it is (mg_mmap_whose).
     */
    const mg_lib *maker;
    bool go;
    /* Its own number, given as it went into the list (g_region_serial): a write-back knows it again by it, the lock let go meanwhile. */
    u32 serial;
    struct region *next;
} region;

/* Marks the pages of [offset, offset + size) of a region as the app's or not; both are multiples of a page. */
static void region_mark(region *r, size_t offset, size_t size, bool on)
{
    size_t page = offset / MG_PAGE, end = (offset + size) / MG_PAGE;
    for (; page < end; page++) {
        u8 bit = (u8)(1u << (page & 7));
        bool was = (r->live[page >> 3] & bit) != 0;
        if (was == on) continue;
        r->live[page >> 3] ^= bit;
        if (on) r->live_pages++;
        else r->live_pages--;
    }
}

static region *g_regions;
static mg_lock g_region_lock = MG_LOCK_INIT;
/*
 * Who holds g_region_lock, since when and where it was taken: the hang observer says it when the lock stands still
 * (mg_bionic.c). Aurora Store's sign-in page stood still on a Lumia 650 (650) with the lock held by a thread the log did
 * not name -- every mmap, munmap, mprotect and madvise waited, and Chromium's threads with them -- and the UI thread waited
 * for it as the app was suspended. A thread that takes it again while it holds it is said at once: that is a thread
 * stuck for good (a fault met under the lock, whose handling maps).
 */
static mg_holder g_region_hold = MG_HOLDER_INIT;
static void region_taken(const char *site) { mg_holder_take(&g_region_hold, site); }
static void region_wanted(const char *site)
{
    static volatile i32 said;
    if (mg_holder_mine(&g_region_hold) && !mg_atomic_xchg(&said, 1))
        mg_log(MG_LOG_ERROR, "mem: the mappings' lock is taken again in %s by the thread that holds it since %s: this thread stops for good",
               site, g_region_hold.what ? g_region_hold.what : "?");
}
#define mg_lock_acquire(l) ((l) == &g_region_lock ? (region_wanted(__func__), mg_lock_acquire(l), region_taken(__func__)) : mg_lock_acquire(l))
#define mg_lock_try(l) ((l) == &g_region_lock ? (mg_lock_try(l) ? (region_taken(__func__), true) : false) : mg_lock_try(l))
#define mg_lock_release(l) ((l) == &g_region_lock ? mg_holder_let_go(&g_region_hold) : (void)0, mg_lock_release(l))

bool mg_mem_lock_look(mg_hold_seen *seen) { return mg_holder_look(&g_region_hold, seen, NULL, 0); }
bool mg_mem_lock_mine(void) { return mg_holder_mine(&g_region_hold); }

void mg_g_mapped_addresses(u64 *spanned, u64 *reserved)
{
    region *r;
    *spanned = *reserved = 0;
    mg_lock_acquire(&g_region_lock);
    for (r = g_regions; r; r = r->next) {
        *spanned += r->size;
        *reserved += mg_host_reserved_in(r->base, r->size);
    }
    mg_lock_release(&g_region_lock);
}

/*
 * Marks pages of [offset, offset + size) of a region as given back by madvise or not, as region_mark marks; only pages the
 * app has are marked. g_gone counts them in every region, so that nothing is looked up while there are none. Under the lock.
 */
static volatile i32 g_gone;
static void gone_mark(region *r, size_t offset, size_t size, bool on)
{
    size_t page = offset / MG_PAGE, end = (offset + size) / MG_PAGE;
    if (!r->gone) {
        if (!on) return;
        r->gone = (u8 *)mg_alloc(r->size / MG_PAGE / 8 + 1);
    }
    for (; page < end; page++) {
        u8 bit = (u8)(1u << (page & 7));
        bool was = (r->gone[page >> 3] & bit) != 0;
        if (was == on || (on && !(r->live[page >> 3] & bit))) continue;
        r->gone[page >> 3] ^= bit;
        if (on) r->gone_pages++;
        else r->gone_pages--;
        mg_atomic_add(&g_gone, on ? 1 : -1);
    }
}

static bool gone_has(const region *r, size_t page) { return r->gone && (r->gone[page >> 3] & (1u << (page & 7))) != 0; }

/* Marks pages of [offset, offset + size) of a region as no-access by the app's own choice or not. Under the lock. */
static void noaccess_mark(region *r, size_t offset, size_t size, bool on)
{
    size_t page = offset / MG_PAGE, end = (offset + size) / MG_PAGE;
    if (!r->noaccess) {
        if (!on) return;
        r->noaccess = (u8 *)mg_alloc(r->size / MG_PAGE / 8 + 1);
    }
    if (end > r->size / MG_PAGE) end = r->size / MG_PAGE;
    for (; page < end; page++) {
        u8 bit = (u8)(1u << (page & 7));
        if (on) r->noaccess[page >> 3] |= bit;
        else r->noaccess[page >> 3] &= (u8)~bit;
    }
}

static bool noaccess_has(const region *r, size_t page) { return r->noaccess && (r->noaccess[page >> 3] & (1u << (page & 7))) != 0; }
static bool live_has(const region *r, size_t page) { return (r->live[page >> 3] & (1u << (page & 7))) != 0; }

/*
 * What was last done to the access of the app's anonymous memory and to what of it is committed, for a fault that finds a
 * page in a state it should not be in (mg_mmap_fix_fault says those of its page): D madvise gave it back, N mapped again
 * with no access, U unmapped, P made no-access, A given access, F committed again when touched, S committed again for the
 * system to write into.
 */
typedef struct mem_event {
    u32 ms, thread;
    mg_addr at;
    u32 size;
    char kind;
} mem_event;
#define MEM_EVENTS 4096
static mem_event g_mem_events[MEM_EVENTS];
static volatile i32 g_mem_event_next;

static void mem_note(char kind, const void *at, size_t size)
{
    mem_event *e = &g_mem_events[(u32)(mg_atomic_add(&g_mem_event_next, 1) - 1) % MEM_EVENTS];
    e->ms = (u32)(mg_time_monotonic_ns() / 1000000ull);
    e->thread = mg_thread_self_id();
    e->at = (mg_addr)at;
    e->size = (u32)size;
    e->kind = kind;
}

/* The events of the page at `page`, oldest first, into `out`. */
static void mem_events_of(const u8 *page, char *out, size_t cap)
{
    u32 end = (u32)g_mem_event_next, count = end < MEM_EVENTS ? end : MEM_EVENTS, i;
    size_t n = 0;
    out[0] = 0;
    for (i = end - count; i != end && n + 64 < cap; i++) {
        const mem_event *e = &g_mem_events[i % MEM_EVENTS];
        if ((mg_addr)page >= e->at && (mg_addr)page < e->at + e->size)
            n += (size_t)snprintf(out + n, cap - n, " %c %08x+%u at %u ms by %u;", e->kind, (unsigned)e->at, (unsigned)e->size, (unsigned)e->ms,
                                  (unsigned)e->thread);
    }
}

/* The pages given back in [offset, offset + size) of a region, committed again (zero) and the app's as before. Under the lock. */
static u64 g_taken_again, g_given_back;
static bool gone_back(region *r, size_t offset, size_t size)
{
    size_t page = offset / MG_PAGE, end = (offset + size + MG_PAGE - 1) / MG_PAGE, first;
    bool ok = true;
    if (!r->gone_pages) return true;
    if (end > r->size / MG_PAGE) end = r->size / MG_PAGE;
    while (page < end) {
        if (!gone_has(r, page)) {
            page++;
            continue;
        }
        for (first = page; page < end && gone_has(r, page); page++) {}
        if (mg_mem_commit(r->base + first * MG_PAGE, (page - first) * MG_PAGE, MG_PROT_RW)) {
            gone_mark(r, first * MG_PAGE, (page - first) * MG_PAGE, false);
            g_taken_again += (u64)(page - first) * MG_PAGE;
            mem_note('F', r->base + first * MG_PAGE, (page - first) * MG_PAGE);
        } else {
            ok = false;
        }
    }
    return ok;
}

/* Memory the system itself is about to write into (mg_mem_set_before_system_write): the pages of it given back, committed again. */
static MG_HOSTCB void before_system_write(void *p, size_t size)
{
    u8 *at = (u8 *)p, *end = at + size;
    region *r;
    bool held;
    if (!g_gone || !size) return;
    /*
     * A thread that holds the lock already goes on under it. The write-back read the file into g_file_chunk under the lock,
     * and the host's read asks here first: once the app had given pages back (madvise -- Chromium's PartitionAlloc and V8 do
     * it all the time), the 5-second write-back took the lock again, its thread stopped for good, and every mmap, munmap,
     * mprotect and madvise of the app's with it -- Aurora Store's and Gmail's Google sign-in pages stood still on a Lumia 650
     * (650; 651's hang report: "the mappings' lock is taken again in before_system_write ... since mg_mem_write_back_all").
     * No file is read under the lock any more (write_back_out); this stays for whatever else reads under it.
     */
    held = mg_holder_mine(&g_region_hold);
    if (!held) mg_lock_acquire(&g_region_lock);
    for (r = g_regions; r; r = r->next) {
        u8 *lo, *hi;
        if (!r->gone_pages || r->base >= end || r->base + r->size <= at) continue;
        lo = at > r->base ? at : r->base;
        hi = end < r->base + r->size ? end : r->base + r->size;
        gone_back(r, (size_t)(lo - r->base), (size_t)(hi - lo));
    }
    if (!held) mg_lock_release(&g_region_lock);
}

static volatile i32 g_file_copies;   /* regions holding a file: while there are none, a write to a file looks for none */

/* Bytes copied into a region's pages, which the app may hold read-only: made writable for the copy, then given back the
   access they had. Pages the app unmapped, and pages not committed, are left. Under the lock. */
static void copy_into(region *r, u8 *to, const u8 *from, size_t n)
{
    u8 *page = (u8 *)MG_ALIGN_DOWN((mg_addr)to, (mg_addr)MG_PAGE);
    for (; page < to + n; page += MG_PAGE) {
        size_t index = (size_t)(page - r->base) / MG_PAGE;
        u8 bit = (u8)(1u << (index & 7));
        u8 *lo = page < to ? to : page, *hi = page + MG_PAGE < to + n ? page + MG_PAGE : to + n;
        bool committed = true, writable;
        int prot = -1;
        if (!(r->live[index >> 3] & bit)) continue;
        if (!mg_mem_page_state(page, &committed, &prot)) prot = r->noaccess && (r->noaccess[index >> 3] & bit) ? MG_PROT_NONE : r->file_prot;
        if (!committed) continue;
        writable = prot == MG_PROT_RW || prot == MG_PROT_RWX;
        if (!writable && !mg_mem_protect(page, MG_PAGE, MG_PROT_RW)) continue;
        memcpy(lo, from + (lo - to), (size_t)(hi - lo));
        if (!writable) {
            mg_mem_protect(page, MG_PAGE, prot < 0 ? MG_PROT_R : prot);
            if (prot == MG_PROT_RX) mg_icache_flush(page, MG_PAGE);
        }
    }
}

/* `n` bytes written to `file` at `at`, into each region but `skip` that holds a copy of some of them. Under the lock. */
static void copies_written(const region *skip, const void *file, u64 at, const u8 *buf, size_t n)
{
    region *r;
    for (r = g_regions; r; r = r->next) {
        u64 lo, hi;
        if (r == skip || !r->file || !mg_fd_held_same(r->file, file)) continue;
        lo = at > r->file_offset ? at : r->file_offset;
        hi = at + n < r->file_offset + r->file_bytes ? at + n : r->file_offset + r->file_bytes;
        if (lo < hi) copy_into(r, r->base + (size_t)(lo - r->file_offset), buf + (size_t)(lo - at), (size_t)(hi - lo));
    }
}

bool mg_mem_file_copies(void) { return g_file_copies > 0; }

/*
 * What was written to a file through a descriptor, into the copies of it mapped shared. Bolt (Psiphon's database, in
 * WhatsApp's libgojni.so) maps its file shared and read-only, writes it with pwrite and reads what it wrote, its new meta
 * page first, through the mapping; with the copy as it was at mmap the next transaction saw the one before. LMDB writes
 * its meta page through a second descriptor of the file, and SQLite with mmap_size reads through a mapping so too.
 */
void mg_mem_file_written(void *file, u64 at, const void *buf, size_t n)
{
    if (!n || !g_file_copies) return;
    mg_lock_acquire(&g_region_lock);
    copies_written(NULL, file, at, (const u8 *)buf, n);
    mg_lock_release(&g_region_lock);
}

/*
 * Write-backs: what the app wrote into a writable shared file mapping, back to the file and into the file's other mappings.
 * They read and write the file with the mappings' lock let go, one write-back at a time (g_write_back_lock, which nothing
 * else waits for): a chunk of the mapping's pages is copied under the mappings' lock, then compared with what the file holds
 * and written where it differs past it. They were made under the mappings' lock, which every mmap, munmap, mprotect and
 * madvise of the app's waits for, and the fault handler's mending of a page: every 5 s the write-back thread read each
 * mapping's file whole and wrote what differed, and walked the list of mappings from its start for each of them, all with the
 * lock held -- a pass of a moment on a PC, and as long as the phone's flash takes, which is far longer while another thread
 * puts a file on the disk (Viber's hang report has a SQLite thread in fsync). The lock order is g_write_back_lock, then
 * g_region_lock; a write-back may take the mappings' lock, never the other way round. And the mappings' lock is the last
 * taken of all: nothing is waited for under it, not the descriptors' lock either (mg_libc_io.c), whose holder -- a pipe's
 * bytes copied into the app's buffer, epoll_wait's events written into it, a select's sets read from it -- may meet a page
 * the app gave back with madvise and wait in the fault handler for the mappings' lock (fix_fault). The write-back took a
 * hold of its file under it, which took the descriptors' lock, and so did mmap of a shared file: each such thread and a
 * fault's waited for each other for good.
 */
#define FILE_CHUNK (16 * MG_PAGE)
static mg_lock g_write_back_lock = MG_LOCK_INIT;
/* Who writes back (the suspension's word when it cannot wait any longer). */
static mg_holder g_write_back_hold = MG_HOLDER_INIT;
/* The file's bytes as they are, and the mapping's copied to be written: a chunk at a time, under g_write_back_lock. */
static u8 g_file_chunk[FILE_CHUNK], g_page_copy[FILE_CHUNK];
/* The writable shared file mappings there are (a region's `file` and `write_back`), counted under the mappings' lock. */
static volatile i32 g_write_backs;
/* Each region's own number (region.serial), given as it goes into the list: a region looked at before is known again by it. */
static u32 g_region_serial;
void (*volatile mg_mem_write_back_io_hook)(void);

static region *region_of(const void *p);
static bool write_back_regions_by(u64 deadline);

static void write_back_lock(void)
{
    mg_lock_acquire(&g_write_back_lock);
    mg_holder_take(&g_write_back_hold, "a write-back");
}

/* g_write_back_lock by `deadline` (mg_time_monotonic_ns): false when it was not to be had by then. */
static bool write_back_lock_by(u64 deadline)
{
    while (!mg_lock_try(&g_write_back_lock)) {
        if (mg_time_monotonic_ns() >= deadline) return false;
        mg_sleep_ns(2000000ull);
    }
    mg_holder_take(&g_write_back_hold, "a write-back");
    return true;
}

static void write_back_unlock(void)
{
    mg_holder_let_go(&g_write_back_hold);
    mg_lock_release(&g_write_back_lock);
}

/* `r`, if it is still in the list with its `serial`: a region looked at under the lock before, which may have gone since. Under the lock. */
static region *region_still(const region *r, u32 serial)
{
    region *x;
    for (x = g_regions; x; x = x->next)
        if (x == r && x->serial == serial) return x;
    return NULL;
}

/* `r` into the list, with a number of its own. Under the lock. */
static void region_link(region *r)
{
    r->serial = ++g_region_serial;
    r->next = g_regions;
    g_regions = r;
}

/*
 * What the app wrote into [from, from + size) of region `r` (its `serial`), back to the file, and into the file's other
 * mappings: the pages it still has, and of them those that differ from what the file holds -- most of a store's pages are as
 * they were the last time, and write_back_loop goes over them every few seconds. A chunk's pages are copied under the
 * mappings' lock, the file read and written past it; the file held apart for it (mg_fd_held_ref), as the mapping may be
 * unmapped meanwhile, and then the rest is left. With a `deadline` (mg_time_monotonic_ns; 0: none) the mappings' lock is
 * waited for no later than it: false when it gave up then. Under g_write_back_lock, the mappings' lock not held.
 */
static bool write_back_out(region *r, u32 serial, size_t from, size_t size, u64 deadline)
{
    void *file = NULL;
    u64 file_offset = 0;
    size_t end = 0, at;
    bool done = true;
#define WRITE_BACK_REGIONS_LOCK() (!deadline ? (mg_lock_acquire(&g_region_lock), true) : write_back_regions_by(deadline))
    if (!WRITE_BACK_REGIONS_LOCK()) return false;
    if (region_still(r, serial) && r->file && r->write_back && from < r->file_bytes) {
        /* (A count alone, no lock: the region's own hold keeps the file while the region is in the list, see above.) */
        file = mg_fd_held_ref(r->file);
        file_offset = r->file_offset;
        end = size < r->file_bytes - from ? from + size : r->file_bytes;
    }
    mg_lock_release(&g_region_lock);
    if (!file) return true;
    for (at = from / MG_PAGE * MG_PAGE; at < end; at += FILE_CHUNK) {
        const size_t chunk_end = at + FILE_CHUNK < end ? at + FILE_CHUNK : end, first = at / MG_PAGE;
        size_t page;
        u32 have = 0;   /* the chunk's pages copied, a bit each */
        i32 got;
        if (!WRITE_BACK_REGIONS_LOCK()) {
            done = false;
            break;
        }
        if (!region_still(r, serial)) {
            /* (Unmapped meanwhile: its munmap wrote it back.) */
            mg_lock_release(&g_region_lock);
            break;
        }
        for (page = first; page * MG_PAGE < chunk_end; page++) {
            const size_t a = page * MG_PAGE < from ? from : page * MG_PAGE, b = (page + 1) * MG_PAGE < chunk_end ? (page + 1) * MG_PAGE : chunk_end;
            /* (Not a page the app unmapped, made no-access, or gave back: reading it would fault, and it holds nothing of the file's.) */
            if (b <= a || !live_has(r, page) || noaccess_has(r, page) || gone_has(r, page)) continue;
            memcpy(g_page_copy + (a - at), r->base + a, b - a);
            have |= 1u << (page - first);
        }
        mg_lock_release(&g_region_lock);
        if (!have) continue;
        if (mg_mem_write_back_io_hook) mg_mem_write_back_io_hook();
        got = mg_fd_held_pread(file, g_file_chunk, (u32)(chunk_end - at), file_offset + at);
        for (page = first; page * MG_PAGE < chunk_end; page++) {
            size_t a = page * MG_PAGE < from ? from : page * MG_PAGE, b = (page + 1) * MG_PAGE < chunk_end ? (page + 1) * MG_PAGE : chunk_end;
            if (!(have & (1u << (page - first)))) continue;
            /* Up to the file's end, which the app may have moved in since it mapped it: Linux writes a mapping back no further. */
            if (got >= 0 && b > at + (size_t)got) b = at + (size_t)got;
            if (b <= a) continue;
            if (got >= 0 && !memcmp(g_file_chunk + (a - at), g_page_copy + (a - at), b - a)) continue;
            if (mg_fd_held_pwrite(file, g_page_copy + (a - at), (u32)(b - a), file_offset + a) <= 0 || g_file_copies <= 1) continue;
            /* The file's other mappings are given the bytes as a write through a descriptor gives them: memory alone, under the lock. */
            if (!WRITE_BACK_REGIONS_LOCK()) {
                done = false;
                continue;
            }
            copies_written(region_still(r, serial), file, file_offset + a, g_page_copy + (a - at), b - a);
            mg_lock_release(&g_region_lock);
        }
    }
#undef WRITE_BACK_REGIONS_LOCK
    mg_fd_let_go(file);
    return done;
}

/* [addr, addr + len) of the mapping that holds `addr` written back, where it is a writable shared file mapping: msync, munmap. */
static void write_back_range(void *addr, size_t len)
{
    region *r;
    u32 serial = 0;
    size_t from = 0;
    bool wanted;
    mg_lock_acquire(&g_region_lock);
    r = region_of(addr);
    wanted = r && r->file && r->write_back;
    if (wanted) {
        serial = r->serial;
        from = (size_t)((u8 *)addr - r->base);
    }
    mg_lock_release(&g_region_lock);
    if (!wanted) return;
    write_back_lock();
    write_back_out(r, serial, from, len, 0);
    write_back_unlock();
}

/* A writable shared file mapping as it was when the list was made: one write-back's worth of mg_mem_write_back_all. */
typedef struct write_back_item {
    region *r;
    u32 serial;
    size_t bytes;
} write_back_item;

/* The writable shared file mappings of up to `largest` bytes (0: all), `cap` at most, into `items`. Under the lock: one walk. */
static u32 write_back_list(write_back_item *items, u32 cap, size_t largest)
{
    region *r;
    u32 n = 0;
    for (r = g_regions; r && n < cap; r = r->next) {
        if (!r->file || !r->write_back || (largest && r->file_bytes > largest)) continue;
        items[n].r = r;
        items[n].serial = r->serial;
        items[n].bytes = r->file_bytes;
        n++;
    }
    return n;
}

/*
 * Every writable shared file mapping written back, one at a time. The kernel's page cache holds what an app writes into such
 * a mapping, and the file has it even when the app is killed; here it was in the mapping alone until msync, munmap or the
 * app's end. A phone's app is suspended and then ended without notice, and TikTok Lite's key-value store (KEVA), which maps
 * its files so and does not msync, found them half written at the next start: 368 "fail to mark" a start on a Lumia 650
 * from its second start on, and what it kept was wrong (package 628). Now also as the app is suspended
 * (megler_settle_files), all of them, and every 5 s (write_back_loop) those of up to `largest` bytes: each pass reads what
 * it compares, and a store's files are small, where a database mapped whole and written so (Realm's) may not be. The
 * mappings are listed in one walk under the lock; it walked the list from its start once for every mapping there is.
 */
void mg_mem_write_back_all(size_t largest)
{
    const u32 cap = (u32)g_write_backs + 8;
    write_back_item *items = (write_back_item *)mg_alloc(cap * sizeof(*items));
    u32 n, i;
    mg_lock_acquire(&g_region_lock);
    n = write_back_list(items, cap, largest);
    mg_lock_release(&g_region_lock);
    for (i = 0; i < n; i++) {
        write_back_lock();
        write_back_out(items[i].r, items[i].serial, 0, items[i].bytes, 0);
        write_back_unlock();
    }
    free(items);
}

/* Who holds the lock that a write-back could not wait for any longer, said. */
static void write_back_gave_up(const char *as, bool writer)
{
    mg_hold_seen seen;
    if (writer) {
        mg_holder_look(&g_write_back_hold, &seen, NULL, 0);
        mg_log(MG_LOG_WARN, "mem: the files' shared mappings were not all written back %s: another write-back (host thread %u) has been "
               "reading and writing its file for %.1f s", as, (unsigned)seen.owner, (double)seen.held_ns / 1e9);
    } else {
        mg_holder_look(&g_region_hold, &seen, NULL, 0);
        mg_log(MG_LOG_WARN, "mem: the files' shared mappings were not all written back %s: the mappings' lock is held by host thread %u, "
               "taken in %s %.1f s ago", as, (unsigned)seen.owner, seen.what ? seen.what : "?", (double)seen.held_ns / 1e9);
    }
}

/* The mappings' lock by `deadline`, for write_back_out. */
static bool write_back_regions_by(u64 deadline)
{
    while (!mg_lock_try(&g_region_lock)) {
        if (mg_time_monotonic_ns() >= deadline) return false;
        mg_sleep_ns(2000000ull);
    }
    return true;
}

/* All of them, waiting for each lock no later than `deadline`; `as` says when, in the word said if it gave up. */
static bool write_back_all_by(u64 deadline, const char *as)
{
    const u32 cap = (u32)g_write_backs + 8;
    write_back_item *items = (write_back_item *)mg_alloc(cap * sizeof(*items));
    u32 n, i;
    bool done = true;
    if (!write_back_lock_by(deadline)) {
        free(items);
        write_back_gave_up(as, true);
        return false;
    }
    if (!write_back_regions_by(deadline)) {
        write_back_unlock();
        free(items);
        write_back_gave_up(as, false);
        return false;
    }
    n = write_back_list(items, cap, 0);
    mg_lock_release(&g_region_lock);
    for (i = 0; i < n && done; i++) done = write_back_out(items[i].r, items[i].serial, 0, items[i].bytes, deadline);
    write_back_unlock();
    free(items);
    if (!done) write_back_gave_up(as, false);
    return done;
}

/*
 * As the app is suspended, on the shell's UI thread, which is not to stand still behind a lock that stands still --
 * Aurora Store's was "not answered for 5031 ms" as Windows suspended it, waiting in mg_mem_write_back_all (650). False,
 * said with the holder, when it gave up.
 */
bool mg_mem_write_back_all_until(u64 deadline) { return write_back_all_by(deadline, "as the app was suspended"); }

/*
 * At the app's end, which may be a fault's: not when this thread holds either lock (the fault met under it), and a lock
 * left taken by another is waited for a second at most.
 */
void mg_mem_write_back(void)
{
    if (mg_holder_mine(&g_region_hold) || mg_holder_mine(&g_write_back_hold)) return;
    write_back_all_by(mg_time_monotonic_ns() + 1000000000ull, "as the app ended");
}

static MG_HOSTCB void write_back_loop(void *unused)
{
    (void)unused;
    for (;;) {
        mg_sleep_ns(5000000000ull);
        mg_mem_write_back_all(4u << 20);
    }
}

/* The thread of write_back_loop, once there is a writable shared file mapping. */
static void write_back_start(void)
{
    static volatile i32 started;
    mg_thread *t;
    if (started || mg_atomic_xchg(&started, 1)) return;
    t = mg_thread_start(write_back_loop, NULL, 64u * 1024u, "mmap-write-back");
    if (t) mg_thread_detach(t);
}

static region *region_of(const void *p)
{
    region *r;
    for (r = g_regions; r; r = r->next)
        if ((const u8 *)p >= r->base && (const u8 *)p < r->base + r->size) return r;
    return NULL;
}

static int host_prot(int prot)
{
    if (prot & G_PROT_WRITE) return MG_PROT_RW;
    if (prot & G_PROT_EXEC) return MG_PROT_RX;
    if (prot & G_PROT_READ) return MG_PROT_R;
    return MG_PROT_NONE;
}

/* fd-backed mappings read the file through the io layer. */
i32 mg_fd_pread(int fd, void *buf, u32 n, u64 offset);

static bool rwx_on(void);

/*
 * Go's runtime on a 32-bit processor stakes out its heap's addresses as it starts (runtime/malloc.go, mallocinit): an arena
 * of 512 MB, or of 256 or 128 where that is not to be had, each with 4 MB more to line it up (sysReserveAligned), all of
 * it reserved with no access by a system call of its own (mmap2 by number), to keep its heap in one piece -- on Linux, where
 * a 32-bit process has 3 GB. WhatsApp's libgojni.so (Psiphon's tunnel core, Go 1.26.5) took 516 MB so, then 260 in a run
 * where 516 were not to be had, of a 2 GB process that ran out of addresses (the HP Elite x3, 0.1.0.671), for a heap of a
 * few megabytes. On a 32-bit host such a reservation from a library of Go's (its build ID note, and the call's place in its
 * code: mg_bionic_syscall_site) is refused with ENOMEM, which Go takes for none to be had: it then reserves nothing big,
 * its heap growing 4 MB at a time at a hint past its library's image, or anywhere, lined up (mheap.sysAlloc). Every
 * refusal is handled there, in the version WhatsApp has as in 1.27 (a nil from sysReserve: the next size, then none).
 * What Go asks for so on a 64-bit host, and what any other code reserves (Chromium's, Unity's), is left as it is.
 */
#define GO_UP_FRONT (64u << 20)

static mg_lock g_go_lock = MG_LOCK_INIT;
static u32 g_go_refused;
static u64 g_go_refused_bytes;

void mg_mmap_go_refused(u32 *count, u64 *bytes)
{
    mg_lock_acquire(&g_go_lock);
    if (count) *count = g_go_refused;
    if (bytes) *bytes = g_go_refused_bytes;
    mg_lock_release(&g_go_lock);
}

static bool go_up_front_refused(size_t len, int prot, int flags)
{
    static const mg_lib *said[8];
    static u32 nsaid;
    mg_addr site;
    const mg_lib *lib;
    u32 i;
    bool first;
    if (sizeof(void *) != 4 || len < GO_UP_FRONT || (prot & 7) || !(flags & G_MAP_ANONYMOUS) || (flags & (G_MAP_FIXED | G_MAP_SHARED))) return false;
    site = mg_bionic_syscall_site();
    lib = site ? mg_ld_lib_of((const void *)MG_CODE_ADDR(site)) : NULL;
    if (!lib || !mg_ld_lib_is_go(lib)) return false;
    /* (Said once for each library: its runtime asks again for less, and the sizes after the first say nothing new.) */
    mg_lock_acquire(&g_go_lock);
    g_go_refused++;
    g_go_refused_bytes += len;
    for (i = 0; i < nsaid && said[i] != lib; i++) {}
    first = i == nsaid;
    if (first && nsaid < MG_COUNT(said)) said[nsaid++] = lib;
    mg_lock_release(&g_go_lock);
    if (first)
        mg_info("go: %s's runtime asked to reserve %u MB up front: refused on a 32-bit Windows, Go reserves as it grows", mg_ld_lib_name(lib),
                (unsigned)(len >> 20));
    return true;
}

/*
 * And as its heap grows: 4 MB at a hint past what it has (mheap.sysAlloc), and where the hint is not followed, an arena's
 * size and 4 MB more anywhere (sysReserveAligned(nil, ...)), of which it keeps the 4 MB lined up and gives the ends back --
 * which stay reserved here until the rest goes, twice the addresses for every such arena. Such a reservation of Go's (by
 * number from a library of Go's, with no access, a multiple of 4 MB) is made lined up on 4 MB, which Go takes for one it
 * may use whole (`p&(align-1) == 0`): an arena of 8 MB, nothing given back.
 */
#define GO_ARENA (4u << 20)

static bool go_lined_up(size_t len, int prot, int flags)
{
    mg_addr site;
    if (sizeof(void *) != 4 || len < 2 * GO_ARENA || len >= GO_UP_FRONT || (len & (GO_ARENA - 1)) || (prot & 7) || !(flags & G_MAP_ANONYMOUS) ||
        (flags & (G_MAP_FIXED | G_MAP_SHARED)))
        return false;
    site = mg_bionic_syscall_site();
    return site && mg_ld_lib_is_go(mg_ld_lib_of((const void *)MG_CODE_ADDR(site)));
}

/* `size` reserved at an address lined up on `align`: anywhere first, to see where there is room, given back and reserved again lined up there. */
static u8 *reserve_lined_up(size_t size, size_t align)
{
    int tries;
    for (tries = 0; tries < 8; tries++) {
        u8 *q = (u8 *)mg_mem_reserve(size + align), *p;
        if (!q) return NULL;
        mg_mem_release(q, size + align);
        p = (u8 *)MG_ALIGN_UP((mg_addr)q, (mg_addr)align);
        /* (Another thread may have taken it meanwhile: then again.) */
        q = (u8 *)mg_mem_reserve_at(p, size);
        if (q == p) return p;
        if (q) mg_mem_release(q, size);
    }
    return NULL;
}

static void *g_mmap_common(void *addr, size_t len, int prot, int flags, int fd, u64 offset, mg_addr caller)
{
    size_t size = MG_ALIGN_UP(len, MG_PAGE), reserve = MG_ALIGN_UP(len, 65536);
    region *r;
    u8 *p;
    if (!len) {
        mg_set_errno(GE_INVAL);
        return G_MAP_FAILED;
    }
    if (go_up_front_refused(len, prot, flags)) {
        mg_set_errno(GE_NOMEM);
        return G_MAP_FAILED;
    }
    if (!(flags & G_MAP_ANONYMOUS)) {
        /* Shared memory whose protection was narrowed (ASharedMemory_setProt) maps no wider, as ashmem refuses it. */
        int allowed = mg_fd_shm_prot(fd);
        if (allowed >= 0 && (prot & ~allowed & 7)) {
            mg_set_errno(GE_PERM);
            return G_MAP_FAILED;
        }
    }
    if ((flags & G_MAP_SHARED) && !(flags & (G_MAP_FIXED | G_MAP_ANONYMOUS))) {
        /* Shared memory: a view of the region's pages (see the top). Anything else that is not shared memory goes on below. */
        void *base = NULL;
        size_t mapped = 0;
        bool writable = false;
        /* To be run and not written (PROT_EXEC without PROT_WRITE): a view the host lets run (mg_section_map_code). */
        p = (u8 *)mg_fd_shm_map(fd, offset, len, (prot & G_PROT_EXEC) && !(prot & G_PROT_WRITE), &writable, &base, &mapped);
        if (p) {
            if ((prot & G_PROT_WRITE) && !writable) {
                mg_section_unmap(base, mapped);
                mg_set_errno(GE_ACCES);
                return G_MAP_FAILED;
            }
            r = (region *)mg_alloc(sizeof(*r));
            r->base = p;
            r->size = size;
            r->view = base;
            r->view_size = mapped;
            r->live = (u8 *)mg_alloc(size / MG_PAGE / 8 + 1);
            region_mark(r, 0, size, true);
            mg_lock_acquire(&g_region_lock);
            region_link(r);
            mg_lock_release(&g_region_lock);
            if (host_prot(prot) != MG_PROT_RW) mg_mem_protect(p, size, host_prot(prot));
            mg_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p, shared memory", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
            return p;
        }
        if (*mg_errno_ptr() != GE_NODEV) return G_MAP_FAILED;
        if ((prot & 7) == G_PROT_READ) {
            /* A file read-only: a view of the file (see the top), where it is a regular file opened read-only. */
            p = (u8 *)mg_fd_file_map(fd, offset, len, &base, &mapped);
            if (p) {
                r = (region *)mg_alloc(sizeof(*r));
                r->base = p;
                r->size = size;
                r->view = base;
                r->view_size = mapped;
                r->live = (u8 *)mg_alloc(size / MG_PAGE / 8 + 1);
                region_mark(r, 0, size, true);
                mg_lock_acquire(&g_region_lock);
                region_link(r);
                mg_lock_release(&g_region_lock);
                mg_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p, a view of the file", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
                return p;
            }
        }
    }
    if (flags & G_MAP_FIXED) {
        bool view;
        /* Only over memory this layer already owns: a fresh address cannot be demanded of the host. */
        mg_lock_acquire(&g_region_lock);
        r = region_of(addr);
        if (!r || (u8 *)addr + size > r->base + r->size || ((mg_addr)addr & (MG_PAGE - 1))) {
            mg_lock_release(&g_region_lock);
            mg_note_once("mmap(MAP_FIXED) at an address the layer does not own");
            mg_set_errno(GE_NOMEM);
            return G_MAP_FAILED;
        }
        region_mark(r, (size_t)((u8 *)addr - r->base), size, true);
        gone_mark(r, (size_t)((u8 *)addr - r->base), size, false);
        noaccess_mark(r, (size_t)((u8 *)addr - r->base), size, !(prot & 7));
        if (!(flags & G_MAP_ANONYMOUS)) r->anon = false;
        mem_note(!(prot & 7) ? 'N' : 'A', addr, size);
        view = r->view != NULL;
        mg_lock_release(&g_region_lock);
        p = (u8 *)addr;
        if (!(prot & 7) && (flags & G_MAP_ANONYMOUS) && !view) {
            /* No access: given back, to be committed (zeroed) when mprotect gives it some. */
            mg_mem_decommit(p, size);
            mg_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p, decommitted", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
            return p;
        }
        if (!mg_mem_commit(p, size, MG_PROT_RW) || !mg_mem_protect(p, size, MG_PROT_RW)) {
            mg_set_errno(GE_NOMEM);
            return G_MAP_FAILED;
        }
        memset(p, 0, size);
    } else {
        /*
         * An address asked for without MAP_FIXED is a hint, which Linux follows when the range is free: so it is here, where
         * the host can reserve that range (an address on the host's 64 KB granularity); anywhere else otherwise. Chromium's
         * PartitionAlloc reserves its 2 MB super pages at addresses it picks, at random and then one after another, until one
         * suits its pool, and gave up with an out-of-memory crash when every answer was the same place, which it had banned.
         */
        p = addr && !((mg_addr)addr & 0xFFFF) ? (u8 *)mg_mem_reserve_at(addr, reserve) : NULL;
        if (!p && !addr && go_lined_up(len, prot, flags)) p = reserve_lined_up(reserve, GO_ARENA);
        if (!p) p = (u8 *)mg_mem_reserve(reserve);
        /* (Anonymous and with no access, reserved alone: see the top.) */
        if (!p || (((prot & 7) || !(flags & G_MAP_ANONYMOUS)) && !mg_mem_commit(p, size, MG_PROT_RW))) {
            if (p) mg_mem_release(p, reserve);
            mg_set_errno(GE_NOMEM);
            return G_MAP_FAILED;
        }
        r = (region *)mg_alloc(sizeof(*r));
        r->base = p;
        r->size = reserve;
        r->anon = (flags & G_MAP_ANONYMOUS) != 0;
        {
            /* Whose (mg_mmap_whose): the library of the system call's place by number, or of libc's caller. */
            const mg_addr site = mg_bionic_syscall_site();
            r->maker = mg_ld_lib_of((const void *)MG_CODE_ADDR(site ? site : caller));
            r->go = site && r->maker && mg_ld_lib_is_go(r->maker);
        }
        r->live = (u8 *)mg_alloc(reserve / MG_PAGE / 8 + 1);
        region_mark(r, 0, size, true);
        if (!(prot & 7)) noaccess_mark(r, 0, size, true);
        mg_lock_acquire(&g_region_lock);
        region_link(r);
        mg_lock_release(&g_region_lock);
    }
    if (!(flags & G_MAP_ANONYMOUS)) {
        size_t got = 0;
        while (got < len) {
            i32 n = mg_fd_pread(fd, p + got, (u32)(len - got), offset + got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if ((flags & G_MAP_SHARED) && (flags & G_MAP_FIXED) && (prot & G_PROT_WRITE)) {
            mg_note_once("mmap: a shared writable file mapping over memory already mapped is a private copy here; writes do not reach the file");
        } else if ((flags & G_MAP_SHARED) && !(flags & G_MAP_FIXED)) {
            /* The file: to write back to (region.file) when the mapping is writable, and to keep the copy in step with as
               the file is written through a descriptor (mg_mem_file_written). Held before the mappings' lock is taken:
               holding it takes the descriptors' lock, which is never waited for under the mappings' (see write-backs). */
            void *held = mg_fd_hold(fd);
            mg_lock_acquire(&g_region_lock);
            r = region_of(p);
            if (r && !r->file) {
                r->file = held;
                held = NULL;
                r->file_offset = offset;
                r->file_bytes = len;
                r->write_back = (prot & G_PROT_WRITE) != 0;
                r->file_prot = host_prot(prot);
                if (r->file) mg_atomic_add(&g_file_copies, 1);
                if (r->file && r->write_back) mg_atomic_add(&g_write_backs, 1);
            }
            mg_lock_release(&g_region_lock);
            /* (Not the region's after all: another thread unmapped it meanwhile, or it had its file already.) */
            mg_fd_let_go(held);
            if (prot & G_PROT_WRITE) write_back_start();
        }
    }
    if ((prot & G_PROT_WRITE) && (prot & G_PROT_EXEC)) {
        bool rwx = rwx_on();
        mg_lock_acquire(&g_region_lock);
        r = region_of(p);
        if (r) r->wx = true;
        rwx = rwx && r && !r->view;
        mg_lock_release(&g_region_lock);
        if (rwx && mg_mem_protect(p, size, MG_PROT_RWX)) mg_icache_flush(p, size);
    } else if (host_prot(prot) != MG_PROT_RW && ((prot & 7) || !(flags & G_MAP_ANONYMOUS))) {
        mg_mem_protect(p, size, host_prot(prot));
        if (prot & G_PROT_EXEC) mg_icache_flush(p, size);
    }
    mg_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
    return p;
}

/* (An off_t, a long: in 32-bit Android one past 2 GB is taken as unsigned, as it always was here.) */
static void *g_mmap(void *addr, size_t len, int prot, int flags, int fd, goff offset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)(gulong)offset, (mg_addr)CALLER());
}

static void *g_mmap64(void *addr, size_t len, int prot, int flags, int fd, i64 offset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)offset, (mg_addr)CALLER());
}

/* mmap2's offset is in pages. */
void *mg_g_mmap2(void *addr, size_t len, int prot, int flags, int fd, u32 pgoffset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)pgoffset * MG_PAGE, (mg_addr)CALLER());
}

static int g_munmap(void *addr, size_t len)
{
    region *r, **link;
    size_t size = MG_ALIGN_UP(len, MG_PAGE);
    u8 *keep_lo, *keep_hi;
    mg_trace("munmap(%p, %u)", addr, (unsigned)len);
    /*
     * What a library loaded into the app's own range occupies stays mapped (mg_ld_placed_overlap): its image, and the code
     * the patcher added right after it, which on Android is the free end of the range. Chromium's linker unmaps that end
     * once the library is in, and the stubs there went with it.
     */
    if (size && mg_ld_placed_overlap(addr, size, &keep_lo, &keep_hi)) {
        if ((u8 *)addr < keep_lo) g_munmap(addr, (size_t)(keep_lo - (u8 *)addr));
        if (keep_hi < (u8 *)addr + size) g_munmap(keep_hi, (size_t)((u8 *)addr + size - keep_hi));
        return 0;
    }
    /*
     * A writable shared file mapping's pages go to its file first, the mappings' lock let go while the file is read and
     * written (write_back_out). (What another thread writes into those pages meanwhile, as they are being unmapped, is lost,
     * as it may be on Linux, where the write and the unmapping race.)
     */
    write_back_range((u8 *)MG_ALIGN_DOWN((mg_addr)addr, (mg_addr)MG_PAGE), size);
    mg_lock_acquire(&g_region_lock);
    for (link = &g_regions; (r = *link) != NULL; link = &r->next) {
        size_t offset;
        if ((u8 *)addr < r->base || (u8 *)addr >= r->base + r->size) continue;
        offset = (size_t)((u8 *)addr - r->base) & ~(size_t)(MG_PAGE - 1);
        if (size > r->size - offset) size = r->size - offset;
        gone_mark(r, offset, size, false);
        region_mark(r, offset, size, false);
        if (!r->live_pages) {
            /* The last of it: the reservation goes back. */
            *link = r->next;
            if (r->file && r->write_back) mg_atomic_add(&g_write_backs, -1);
            mg_lock_release(&g_region_lock);
            if (r->file) mg_atomic_add(&g_file_copies, -1);
            mg_fd_let_go(r->file);
            if (r->view) mg_section_unmap(r->view, r->view_size);
            else mg_mem_release(r->base, r->size);
            free(r->live);
            free(r->gone);
            free(r->noaccess);
            free(r);
            return 0;
        }
        /*
         * Part of a region: the host frees address space only whole, so the
         * pages go and the addresses stay reserved until the rest has gone
         * too. (Under the lock: once it is let go another thread may unmap
         * the rest, and the addresses may be someone else's the moment after.)
         */
        if (!r->view) mg_mem_decommit(r->base + offset, size);
        mem_note('U', r->base + offset, size);
        mg_lock_release(&g_region_lock);
        return 0;
    }
    mg_lock_release(&g_region_lock);
    return 0;
}

/*
 * mremap, as allocators use it to shrink or grow a block they mapped (NSS's arenas, Chromium's): shrinking unmaps the
 * tail; growing moves the block (MREMAP_MAYMOVE) to a new private anonymous mapping, readable and writable, with the old
 * bytes copied, since nothing here grows a mapping in place; without MREMAP_MAYMOVE growing fails with ENOMEM, which a
 * caller must expect on Linux too when the pages after are taken. MREMAP_FIXED is not done. A shared file mapping moved
 * so is a private copy from then on.
 */
static void *g_mremap(void *old, size_t old_size, size_t new_size, int flags)
{
    size_t o = MG_ALIGN_UP(old_size, MG_PAGE), n = MG_ALIGN_UP(new_size, MG_PAGE);
    void *p;
    if (((mg_addr)old & (MG_PAGE - 1)) || !o || !n || (flags & ~1)) {
        mg_set_errno(GE_INVAL);
        return G_MAP_FAILED;
    }
    if (n <= o) {
        if (n < o) g_munmap((u8 *)old + n, o - n);
        return old;
    }
    if (!(flags & 1)) {
        mg_set_errno(GE_NOMEM);
        return G_MAP_FAILED;
    }
    p = g_mmap_common(NULL, n, G_PROT_READ | G_PROT_WRITE, 0x02 | G_MAP_ANONYMOUS, -1, 0, (mg_addr)CALLER());   /* MAP_PRIVATE */
    if (p == G_MAP_FAILED) return p;
    memcpy(p, old, o);
    g_munmap(old, o);
    return p;
}

/*
 * Pages of the app's libraries (not mmap'd memory, so no region's) asked to be writable and executable at once: an inline
 * hook patching the app's own code. Few, and never given back: a hook stays as long as the library. Under g_region_lock.
 *
 * What the pages held when asked for so is kept (before) until they are made not writable again, and what the app changed
 * in them is said then, by library, function and offset: the patch of each hook, in the log.
 */
static struct { u8 *lo, *hi, *before; } g_wx_code[64];
static u32 g_wx_code_count;

static bool wx_code_has(const u8 *page)
{
    u32 i;
    for (i = 0; i < g_wx_code_count; i++)
        if (page >= g_wx_code[i].lo && page < g_wx_code[i].hi) return true;
    return false;
}

static bool wx_code_add(u8 *p, size_t size)
{
    u32 i;
    for (i = 0; i < g_wx_code_count; i++)
        if (p >= g_wx_code[i].lo && p + size <= g_wx_code[i].hi) break;
    if (i == g_wx_code_count) {
        if (g_wx_code_count == MG_COUNT(g_wx_code)) return false;
        g_wx_code[i].lo = p;
        g_wx_code[i].hi = p + size;
        g_wx_code[i].before = NULL;
        g_wx_code_count++;
    }
    if (!g_wx_code[i].before && (g_wx_code[i].before = malloc((size_t)(g_wx_code[i].hi - g_wx_code[i].lo))) != NULL)
        memcpy(g_wx_code[i].before, g_wx_code[i].lo, (size_t)(g_wx_code[i].hi - g_wx_code[i].lo));
    return true;
}

static void hex_of(char *out, const u8 *b, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        *out++ = digits[b[i] >> 4];
        *out++ = digits[b[i] & 15];
        if ((i & 3) == 3 && i + 1 < n) *out++ = ' ';
    }
    *out = 0;
}

/* Says the spans that differ between what the page held (before) and what it holds; changes fewer than 8 bytes apart are one span. */
static void wx_code_say(const u8 *lo, const u8 *before, size_t size)
{
    size_t i = 0;
    u32 said = 0;
    while (i < size && said < 8) {
        size_t start, end, k;
        char was[80], now[80];
        const char *lib = NULL, *sym;
        const void *sym_addr = NULL, *base = NULL;
        if (lo[i] == before[i]) {
            i++;
            continue;
        }
        start = i;
        end = i + 1;
        for (k = end; k < size && k < end + 8 && k - start < 32; k++)
            if (lo[k] != before[k]) end = k + 1;
        hex_of(was, before + start, end - start);
        hex_of(now, lo + start, end - start);
        sym = mg_ld_symbol_at(lo + start, &lib, &sym_addr, &base);
        mg_info("mprotect: the app changed %u bytes of its code at %p (%s+0x%x %s+0x%x): %s, was %s", (unsigned)(end - start),
                (void *)(lo + start), lib ? lib : "?", (unsigned)(lo + start - (const u8 *)base), sym ? sym : "?",
                sym ? (unsigned)(lo + start - (const u8 *)sym_addr) : 0u, now, was);
        said++;
        i = end;
    }
}

/*
 * Traps the app wrote into its code, for its SIGILL handler to be called there: an inline hook's (Facebook's libdistract
 * makes a hooked function's first instruction `udf #0xfa`, and its handler moves the pc to the hook). Told by what the app
 * changed when it makes the page not writable again: an undefined instruction, Thumb's (0xdeXX) or ARM's (0xe7fXXXfX),
 * written by itself, with what is on either side left as it was -- a hook's trap, not a part of other code.
 *
 * The phone's system takes Thumb's `udf #0xf8` to `#0xff` for its own: met in an app's code, `udf #0xfa` reached no fault
 * handler, and the thread went on at the next instruction with r0 to r2 changed (Instagram's hook on __cxa_throw: the
 * function got garbage for its arguments and wrote through it). Those are made `udf #0`, which is reported as any other
 * undefined instruction; the others are left as they are. A fault at a trap so recorded is the app's SIGILL
 * (mg_signal_fault). Kept for as long as the app runs: at most 256, under g_region_lock.
 */
#define APP_TRAP_THUMB 0xde00u
static struct { mg_addr at; u32 value; } g_app_traps[256];
static u32 g_app_trap_count;

static bool app_trap_add(mg_addr at, u32 value)
{
    u32 i;
    bool ok = true;
    mg_lock_acquire(&g_region_lock);
    for (i = 0; i < g_app_trap_count && g_app_traps[i].at != at; i++) {}
    if (i == g_app_trap_count) {
        if (i == MG_COUNT(g_app_traps)) ok = false;
        else g_app_trap_count++;
    }
    if (ok) {
        g_app_traps[i].at = at;
        g_app_traps[i].value = value;
    }
    mg_lock_release(&g_region_lock);
    return ok;
}

/* For the fault handler: pc is a trap the app wrote, and still there. */
bool mg_mmap_app_trap(mg_addr pc)
{
    u32 i;
    bool found = false;
    mg_lock_acquire(&g_region_lock);
    for (i = 0; i < g_app_trap_count && !found; i++) {
        if (g_app_traps[i].at != pc) continue;
        if (g_app_traps[i].value >> 16) found = *(const u32 *)pc == g_app_traps[i].value;
        else found = *(const u16 *)pc == g_app_traps[i].value;
    }
    mg_lock_release(&g_region_lock);
    return found;
}

static bool same16(const u8 *lo, const u8 *before, size_t i, size_t n) { return i >= n || (lo[i] == before[i] && lo[i + 1] == before[i + 1]); }

/* The traps among what changed in [lo, lo + n): recorded, and the system's own made udf #0 (pages outside [p, p + size) put back executable). */
static void wx_code_traps(u8 *lo, const u8 *before, size_t n, const u8 *p, size_t size)
{
    size_t i;
    for (i = 0; i + 2 <= n; i += 2) {
        u16 now = (u16)(lo[i] | lo[i + 1] << 8);
        u32 word;
        u8 *page = (u8 *)MG_ALIGN_DOWN((mg_addr)(lo + i), (mg_addr)MG_PAGE);
        const char *lib = NULL;
        const void *base = NULL;
        if (same16(lo, before, i, n)) continue;
        mg_ld_symbol_at(lo + i, &lib, NULL, &base);
        if ((now & 0xff00u) == 0xde00u && (i < 2 || same16(lo, before, i - 2, n)) && same16(lo, before, i + 2, n)) {
            u16 put = (now & 0xfff8u) == 0xdef8u ? (u16)APP_TRAP_THUMB : now;
            if (!app_trap_add((mg_addr)(lo + i), put)) {
                mg_note_once("mprotect: more traps in the app's code than are kept; %p is left as it is", (void *)(lo + i));
                continue;
            }
            if (put != now) {
                mg_mem_protect(page, MG_PAGE, MG_PROT_RW);
                *(volatile u16 *)(lo + i) = put;
                if (page < p || page >= p + size) {
                    mg_mem_protect(page, MG_PAGE, MG_PROT_RX);
                    mg_icache_flush(page, MG_PAGE);
                }
            }
            mg_info("mprotect: a trap the app wrote at %p (%s+0x%x), udf #0x%x, is its SIGILL%s", (void *)(lo + i), lib ? lib : "?",
                    (unsigned)(lo + i - (const u8 *)base), now & 0xffu, put != now ? "; made udf #0, as the system keeps that one" : "");
            continue;
        }
        if (!(i & 2) && i + 4 <= n) {
            word = (u32)now | (u32)(lo[i + 2] | lo[i + 3] << 8) << 16;
            if ((word & 0xfff000f0u) == 0xe7f000f0u && (i < 4 || (same16(lo, before, i - 4, n) && same16(lo, before, i - 2, n))) &&
                same16(lo, before, i + 4, n) && same16(lo, before, i + 6, n) && app_trap_add((mg_addr)(lo + i), word)) {
                mg_info("mprotect: a trap the app wrote at %p (%s+0x%x), an ARM udf, is its SIGILL", (void *)(lo + i), lib ? lib : "?",
                        (unsigned)(lo + i - (const u8 *)base));
                i += 2;
            }
        }
    }
}

/* Pages of g_wx_code made not writable: what the app changed in them is said, its traps found, and what they held let go. */
static void wx_code_written(u8 *p, size_t size)
{
    u32 i;
    for (i = 0;; i++) {
        u8 *lo, *before;
        size_t n;
        mg_lock_acquire(&g_region_lock);
        if (i >= g_wx_code_count) {
            mg_lock_release(&g_region_lock);
            return;
        }
        lo = g_wx_code[i].lo;
        n = (size_t)(g_wx_code[i].hi - lo);
        before = g_wx_code[i].before;
        if (before && lo < p + size && p < lo + n) g_wx_code[i].before = NULL;
        else before = NULL;
        mg_lock_release(&g_region_lock);
        if (before) {
            wx_code_say(lo, before, n);
            wx_code_traps(lo, before, n, p, size);
            free(before);
        }
    }
}

/*
 * The flag trace-hooks: the first bytes of memory the app mapped writable and executable at once, as it makes them
 * executable only (by mprotect) -- a hook's trampoline, and a JIT's code, which is why it is a flag.
 */
static void wx_region_say(const u8 *p)
{
    static int wanted = -1;
    static volatile i32 said;
    char hex[3 * 128];
    if (wanted < 0) wanted = mg_flag("trace-hooks") ? 1 : 0;
    if (!wanted || mg_atomic_add(&said, 1) > 32) return;
    hex_of(hex, p, 64);
    mg_info("mprotect: code the app wrote at %p: %s", (const void *)p, hex);
    hex_of(hex, p + 64, 64);
    mg_info("mprotect: code the app wrote at %p: %s", (const void *)(p + 64), hex);
}

static int g_mprotect(void *addr, size_t len, int prot)
{
    size_t size = MG_ALIGN_UP(len, MG_PAGE);
    u8 *p = (u8 *)addr;
    bool owned;
    /*
     * An address inside a page is refused, as Linux refuses it. Rounding it down protected a page the caller did not name:
     * Gojek's protection library asks for a guard page at an address it has not aligned (a block's start plus its header and
     * 4096), goes on when that is refused, and writes to the page before it -- which was the page this made no-access.
     */
    if ((mg_addr)addr & (MG_PAGE - 1)) {
        mg_trace("mprotect(%p, %u, prot %d): not the start of a page, refused", addr, (unsigned)len, prot);
        mg_set_errno(GE_INVAL);
        return -1;
    }
    mg_lock_acquire(&g_region_lock);
    owned = region_of(p) != NULL;
    mg_lock_release(&g_region_lock);
    if (!owned) owned = mg_ld_is_guest_code((mg_addr)p);
    mg_trace("mprotect(%p, %u, prot %d)%s", addr, (unsigned)len, prot, owned ? "" : " -- not the app's memory, left alone");
    if (!owned) {
        /* An inline hook may target a bionic export implemented in the host
         * executable. Do not promise executable access we cannot grant: the
         * caller would then patch read-only host code (WhatsApp's abort hook).
         * Keep the existing treatment of non-executable stack guard requests. */
        if (prot & G_PROT_EXEC) {
            mg_warn("mprotect(%p, %u, prot %d): host memory, executable access refused", addr, (unsigned)len, prot);
            mg_set_errno(GE_ACCES);
            return -1;
        }
        /*
         * Memory the app did not get from mmap and that is not one of its
         * libraries: a thread's stack, where Android code puts a guard page
         * of its own, or something found by reading the process's map. Those
         * are the host's -- a Windows stack has its own guard page, and
         * changing a page of it, or of anything else of the system's, breaks
         * whoever owns it. The app is told it worked.
         */
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 6)
            mg_warn("mprotect(%p, %u, prot %d): not memory this layer gave the app; left as it is", addr, (unsigned)len, prot);
        return 0;
    }
    {
        /* Memory of a region of the app's that may have been reserved alone (PROT_NONE, see mmap) is committed to be given access. */
        region *r;
        bool view;
        mg_lock_acquire(&g_region_lock);
        r = region_of(p);
        view = r && r->view;
        /* (Pages madvise gave back are the app's to say the access of again: committed below if given some, zero.) */
        if (r) {
            size_t off = (size_t)(p - r->base), n = size < r->size - off ? size : r->size - off;
            if (r->gone_pages) gone_mark(r, off, n, false);
            noaccess_mark(r, off, n, !(prot & 7));
            if (r->file) r->file_prot = host_prot(prot);
            mem_note((prot & 7) ? 'A' : 'P', p, n);
        }
        mg_lock_release(&g_region_lock);
        if (r && !view && (prot & 7) && !mg_mem_commit(p, size, MG_PROT_RW)) {
            mg_set_errno(GE_NOMEM);
            return -1;
        }
        if (r && !view && !(prot & 7)) {
            /* No access: for what is committed; what is not is no-access already. */
            return mg_mem_protect_committed(p, size, MG_PROT_NONE) ? 0 : (mg_set_errno(GE_ACCES), -1);
        }
    }
    if ((prot & G_PROT_WRITE) && (prot & G_PROT_EXEC)) {
        region *r;
        bool kept, rwx = rwx_on();
        mg_lock_acquire(&g_region_lock);
        r = region_of(p);
        if (r) r->wx = true;
        kept = r || wx_code_add(p, size);
        /* (A library's pages, which an inline hook patches, are written and run by turns still: what the app writes into its
           code is looked at as the page goes back to executable, for the traps it sets, wx_code_written.) */
        rwx = rwx && r && !r->view;
        mg_lock_release(&g_region_lock);
        if (!kept) mg_note_once("mprotect: writable and executable at once, more library pages than are kept (%p); it stays as it is", addr);
        else if (rwx && mg_mem_protect(p, size, MG_PROT_RWX)) mg_icache_flush(p, size);
        else mg_mem_protect(p, size, MG_PROT_RW);
        return 0;
    }
    if (!(prot & G_PROT_WRITE)) {
        region *r;
        bool wx;
        wx_code_written(p, size);
        mg_lock_acquire(&g_region_lock);
        r = region_of(p);
        wx = r && r->wx;
        mg_lock_release(&g_region_lock);
        if (wx && (prot & G_PROT_EXEC)) wx_region_say(p);
    }
    if (!mg_mem_protect(p, size, host_prot(prot))) {
        mg_set_errno(GE_ACCES);
        return -1;
    }
    if (prot & G_PROT_EXEC) mg_icache_flush(p, size);
    return 0;
}

/* A write-or-execute fault in a region, or a library's page, that wants both: give the page what this access needs. */
/* A fault in a region of the app's that nothing here repaired: the page's state as this layer and the host see it, and its last changes. */
static void say_unrepaired(mg_addr address, bool write, bool exec)
{
    static volatile i32 said;
    u8 *page = (u8 *)MG_ALIGN_DOWN(address, (mg_addr)MG_PAGE);
    char state[300], events[600], host[80];
    bool committed = false;
    int prot = -2;
    region *r;
    mg_lock_acquire(&g_region_lock);
    r = region_of((const void *)address);
    if (!r) {
        mg_lock_release(&g_region_lock);
        return;
    }
    if (mg_atomic_add(&said, 1) > 6) {
        mg_lock_release(&g_region_lock);
        return;
    }
    {
        size_t index = (size_t)(page - r->base) / MG_PAGE;
        snprintf(state, sizeof(state), "region %08x+%u (%s%s%s%s), the page %s, %s, %s", (unsigned)(mg_addr)r->base, (unsigned)r->size,
                 r->anon ? "anonymous" : "not anonymous", r->view ? ", a view" : "", r->file ? ", a file's" : "", r->wx ? ", written and run" : "",
                 live_has(r, index) ? "the app's" : "unmapped", noaccess_has(r, index) ? "made no-access by the app" : "not made no-access",
                 gone_has(r, index) ? "given back" : "not given back");
    }
    mem_events_of(page, events, sizeof(events));
    mg_lock_release(&g_region_lock);
    if (mg_mem_page_state(page, &committed, &prot))
        snprintf(host, sizeof(host), "%s, access %d", committed ? "committed" : "not committed", prot);
    else
        snprintf(host, sizeof(host), "nothing (it cannot say)");
    mg_warn("mmap: a fault at %08x (%s) not repaired: %s; the host says %s; its last changes:%s", (unsigned)address,
            write ? "written" : exec ? "run" : "read", state, host, events[0] ? events : " none kept");
}

static bool fix_fault(mg_addr address, bool write, bool exec, mg_addr pc);

bool mg_mmap_fix_fault(mg_addr address, bool write, bool exec, mg_addr pc)
{
    if (fix_fault(address, write, exec, pc)) return true;
    say_unrepaired(address, write, exec);
    return false;
}

/*
 * The page already has the access the fault wanted: another thread repaired it meanwhile -- two threads touching the same
 * pages given back at once, the second's fault handled after the first had committed them again (Play Store's WebView ended
 * so, PartitionAlloc writing a slot span's free list on two threads) -- and the instruction is to run again. Three times
 * running at the same address on a thread is no race: the fault is then the app's.
 */
static MG_THREAD_LOCAL mg_addr t_retried_at;
static MG_THREAD_LOCAL int t_retries;
static bool now_accessible(const u8 *page, mg_addr address, bool write)
{
    bool committed = false;
    int prot = -2;
    if (!mg_mem_page_state(page, &committed, &prot) || !committed) return false;
    if (!(prot == MG_PROT_RW || (!write && (prot == MG_PROT_R || prot == MG_PROT_RX)))) return false;
    if (t_retried_at == address && ++t_retries > 3) return false;
    if (t_retried_at != address) {
        t_retried_at = address;
        t_retries = 1;
    }
    return true;
}

/*
 * Whether a region wanted writable and executable at once is made so on the host (mg_mem_rwx; the flag no-rwx: never).
 * Found out by mmap and mprotect, which ask first; the fault handler only reads what they found (rwx_known): asking reads
 * a flag file and, on Windows, runs a probe, neither of which a fault handler may do.
 */
static volatile i32 g_rwx = -1;

static bool rwx_on(void)
{
    if (g_rwx < 0) g_rwx = mg_flag("no-rwx") ? 0 : mg_mem_rwx() ? 1 : 0;
    return g_rwx > 0;
}

static bool rwx_known(void) { return g_rwx > 0; }

static bool fix_fault(mg_addr address, bool write, bool exec, mg_addr pc)
{
    region *r;
    bool wx, rwx, back = false;
    u8 *page = (u8 *)MG_ALIGN_DOWN(address, (mg_addr)MG_PAGE);
    if (g_gone) {
        /* A page madvise gave back, touched: committed again, zero, with the others given back of its 64 KB. */
        mg_lock_acquire(&g_region_lock);
        r = region_of((const void *)address);
        if (r && gone_has(r, (size_t)(page - r->base) / MG_PAGE)) {
            size_t block = (size_t)(page - r->base) & ~(size_t)0xFFFF;
            back = gone_back(r, block, 0x10000 < r->size - block ? 0x10000 : r->size - block) && !gone_has(r, (size_t)(page - r->base) / MG_PAGE);
        }
        mg_lock_release(&g_region_lock);
        if (back && !exec) return true;
    }
    if (!back && !exec && now_accessible(page, address, write)) {
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 4)
            mg_info("mmap: a fault at %08x (%s) found its page there already, repaired by another thread: run again", (unsigned)address,
                    write ? "written" : "read");
        return true;
    }
    if (!back && !exec) {
        /*
         * A page of the app's anonymous memory that it has and did not make no-access, which the host has no longer committed
         * and nothing marked as given back: one of this layer's decommits left it so. Committed again, zero, as a page
         * given back is, and said (the first few times) with what was last done to it, so that the way it got there is found.
         * (Not a page of a region written and run by turns, whose faults are flips: below.)
         */
        bool heal = false, committed = true, known = false;
        int prot = 0;
        char events[600];
        mg_lock_acquire(&g_region_lock);
        r = region_of((const void *)address);
        if (r && r->anon && !r->view && !r->file && !r->wx) {
            size_t index = (size_t)(page - r->base) / MG_PAGE;
            known = mg_mem_page_state(page, &committed, &prot);
            if (live_has(r, index) && !noaccess_has(r, index)) {
                /* Not committed: committed again, zero. Committed with no access, which the app did not ask for: given it back. */
                if (!known || !committed) heal = mg_mem_commit(page, MG_PAGE, MG_PROT_RW);
                else if (prot == MG_PROT_NONE) heal = mg_mem_protect(page, MG_PAGE, MG_PROT_RW);
                if (heal) mem_note('F', page, MG_PAGE);
            }
        }
        if (heal) mem_events_of(page, events, sizeof(events));
        mg_lock_release(&g_region_lock);
        if (heal) {
            static volatile i32 said;
            if (mg_atomic_add(&said, 1) <= 8)
                mg_warn("mmap: a page of the app's at %08x was %s and not marked given back (%s), %s; its last changes:%s", (unsigned)(mg_addr)page,
                        known && committed ? "committed with no access" : "not committed", write ? "written" : "read",
                        known && committed ? "given access again" : "committed again", events[0] ? events : " none kept");
            return true;
        }
    }
    if (!write && !exec) return false;
    rwx = rwx_known();
    mg_lock_acquire(&g_region_lock);
    r = region_of((const void *)address);
    wx = r ? r->wx : wx_code_has(page);
    rwx = rwx && r && !r->view;
    mg_lock_release(&g_region_lock);
    if (!wx) return false;
    /*
     * A page of a library's that writes itself: the instruction that writes is on the page it writes, which flipped to
     * writable for the write and back to executable for the instruction would go round and round -- Titanium Backup's
     * libtitanium.so decrypts its code so, from the page it decrypts (xor byte ptr [edx], al, both at its start), and the
     * app stood still at its first screen. Writable and executable at once where the host allows it (as the app asked by
     * mprotect); its traps are still looked for when the app makes the page executable alone again (wx_code_written).
     */
    if (write && !r && rwx_known() && pc && MG_ALIGN_DOWN(pc, (mg_addr)MG_PAGE) == (mg_addr)page && mg_mem_protect(page, MG_PAGE, MG_PROT_RWX)) {
        static volatile i32 said;
        mg_icache_flush(page, MG_PAGE);
        if (!mg_atomic_xchg(&said, 1)) mg_info("mmap: library code at %p writes its own page: writable and executable at once", (void *)pc);
        return true;
    }
    /* A page of a region that is writable and executable at once where the phone allows it, committed again (madvise) or
       made otherwise since: so again, no flip from then on. */
    if (rwx && mg_mem_protect(page, MG_PAGE, MG_PROT_RWX)) {
        mg_icache_flush(page, MG_PAGE);
        return true;
    }
    if (exec) {
        if (!mg_mem_protect(page, MG_PAGE, MG_PROT_RX)) return false;
        mg_icache_flush(page, MG_PAGE);
        return true;
    }
    return mg_mem_protect(page, MG_PAGE, MG_PROT_RW);
}

void mg_mmap_given_back(u64 *given, u64 *again)
{
    mg_lock_acquire(&g_region_lock);
    if (given) *given = g_given_back;
    if (again) *again = g_taken_again;
    mg_lock_release(&g_region_lock);
}

/*
 * The census of the addresses (mg_host_address_census) asks whose an allocation is: an app's mapping is said by the
 * library that made it -- Go's runtime in one, by number -- or as the app's, where none is known. The names are kept, one
 * for each library and kind, since the census tells owners apart by their names' pointers. Under g_region_lock.
 */
static struct { const mg_lib *lib; bool go; char name[80]; } g_owner_names[24];
static u32 g_nowner_names;

static const char *owner_name(const mg_lib *lib, bool go)
{
    u32 i;
    if (!lib) return "the app's mappings";
    for (i = 0; i < g_nowner_names; i++)
        if (g_owner_names[i].lib == lib && g_owner_names[i].go == go) return g_owner_names[i].name;
    if (g_nowner_names == MG_COUNT(g_owner_names)) return "the app's mappings";
    g_owner_names[i].lib = lib;
    g_owner_names[i].go = go;
    snprintf(g_owner_names[i].name, sizeof(g_owner_names[i].name), go ? "Go's runtime in %s" : "%s's mappings", mg_ld_lib_name(lib));
    g_nowner_names++;
    return g_owner_names[i].name;
}

const char *mg_mmap_whose(const void *base)
{
    region *r;
    const char *name = NULL;
    mg_lock_acquire(&g_region_lock);
    for (r = g_regions; r; r = r->next)
        if (r->base == (const u8 *)base && !r->view) {
            name = owner_name(r->maker, r->go);
            break;
        }
    mg_lock_release(&g_region_lock);
    return name;
}

void mg_mmap_usage(u64 *anon, u64 *files)
{
    region *r;
    u64 a = 0, f = 0;
    size_t page, pages;
    mg_lock_acquire(&g_region_lock);
    for (r = g_regions; r; r = r->next) {
        u64 n = 0;
        if (r->view) continue;
        pages = r->size / MG_PAGE;
        for (page = 0; page < pages; page++)
            if (live_has(r, page) && !noaccess_has(r, page) && !gone_has(r, page)) n++;
        if (r->anon) a += n * MG_PAGE;
        else f += n * MG_PAGE;
    }
    mg_lock_release(&g_region_lock);
    if (anon) *anon = a;
    if (files) *files = f;
}

/* madvise: anonymous memory given back (see the top); any other advice is taken and nothing done. */
#define G_MADV_DONTNEED 4
#define G_MADV_FREE 8
static MG_HOSTCB void given_back(u8 *at, size_t n, void *user)
{
    region *r = (region *)user;
    gone_mark(r, (size_t)(at - r->base), n, true);
    mem_note('D', at, n);
}

/* Whether madvise is Go's runtime giving pages of its heap back (see the top): a system call by number of a library of Go's. */
static bool go_gives_back(void)
{
    mg_addr site = mg_bionic_syscall_site();
    return site && mg_ld_lib_is_go(mg_ld_lib_of((const void *)MG_CODE_ADDR(site)));
}

/*
 * Go's pages just given back in [offset, offset + size) of a region committed again at once (see the top), zero; what
 * could not be (the system out of memory) is added to *failed and stays given back. Under the lock.
 */
static u64 g_kept;
static size_t gone_kept(region *r, size_t offset, size_t size, size_t *failed)
{
    size_t page = offset / MG_PAGE, end = (offset + size) / MG_PAGE, first, kept = 0;
    if (end > r->size / MG_PAGE) end = r->size / MG_PAGE;
    while (page < end) {
        if (!gone_has(r, page) || noaccess_has(r, page)) {
            page++;
            continue;
        }
        for (first = page; page < end && gone_has(r, page) && !noaccess_has(r, page); page++) {}
        if (!mg_mem_commit(r->base + first * MG_PAGE, (page - first) * MG_PAGE, MG_PROT_RW)) {
            *failed += (page - first) * MG_PAGE;
            continue;
        }
        gone_mark(r, first * MG_PAGE, (page - first) * MG_PAGE, false);
        mem_note('K', r->base + first * MG_PAGE, (page - first) * MG_PAGE);
        kept += (page - first) * MG_PAGE;
    }
    return kept;
}

static int g_madvise(void *addr, size_t len, int advice)
{
    u8 *p = (u8 *)addr, *end;
    size_t given = 0, kept = 0, failed = 0;
    u64 total = 0, again = 0, go_kept = 0;
    bool say = false, go;
    if ((mg_addr)p & (MG_PAGE - 1)) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    if ((advice != G_MADV_DONTNEED && advice != G_MADV_FREE) || !len) return 0;
    go = go_gives_back();
    end = p + MG_ALIGN_UP(len, MG_PAGE);
    mg_lock_acquire(&g_region_lock);
    while (p < end) {
        region *r = region_of(p), *n;
        u8 *stop = end;
        if (!r) {
            /* Not the app's mapping: on to the next region that is, if any is in the range. */
            for (n = g_regions; n; n = n->next)
                if (n->base > p && n->base < stop) stop = n->base;
            p = stop;
            continue;
        }
        if (r->base + r->size < stop) stop = r->base + r->size;
        if (r->anon && !r->view && !r->file) {
            size_t discarded = mg_mem_discard(p, (size_t)(stop - p), given_back, r);
            given += discarded;
            if (go && discarded) kept += gone_kept(r, (size_t)(p - r->base), (size_t)(stop - p), &failed);
        }
        p = stop;
    }
    if (given) {
        /* (Said at each 64 MB more given back: how much has been, how much of it was touched again, how much was Go's.) */
        say = (g_given_back >> 26) != ((g_given_back + given) >> 26);
        g_given_back += given;
        g_kept += kept;
        total = g_given_back;
        again = g_taken_again;
        go_kept = g_kept;
    }
    mg_lock_release(&g_region_lock);
    if (given) mg_mem_set_before_system_write(before_system_write);
    if (failed) {
        static volatile i32 said;
        if (mg_atomic_add(&said, 1) <= 4)
            mg_warn("mmap: %u KB of Go's heap given back could not be committed again (out of memory): a goroutine put on them ends the process",
                    (unsigned)(failed >> 10));
    }
    if (say && go_kept)
        mg_info("mmap: %u MB given back to the system with madvise so far, %u MB of it touched again, %u MB of it Go's heap, kept committed",
                (unsigned)(total >> 20), (unsigned)(again >> 20), (unsigned)(go_kept >> 20));
    else if (say)
        mg_info("mmap: %u MB given back to the system with madvise so far, %u MB of it touched again", (unsigned)(total >> 20), (unsigned)(again >> 20));
    return 0;
}
static int g_mlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
/* msync: what was written into a shared file mapping goes to the file now (the mappings' lock let go while it does). */
static int g_msync(void *addr, size_t len, int flags)
{
    (void)flags;
    write_back_range(addr, len);
    return 0;
}
static int g_mincore(void *addr, size_t len, u8 *vec)
{
    size_t i, n = MG_ALIGN_UP(len, MG_PAGE) / MG_PAGE;
    (void)addr;
    for (i = 0; i < n; i++) vec[i] = 1;
    return 0;
}

const mg_sym mg_tab_mem[] = {
    MG_WRAP("malloc", mg_g_malloc),
    MG_WRAP("calloc", mg_g_calloc),
    MG_WRAP("realloc", mg_g_realloc),
    MG_WRAP("free", mg_g_free),
    MG_WRAP("memalign", g_memalign),
    MG_WRAP("posix_memalign", g_posix_memalign),
    MG_WRAP("aligned_alloc", g_aligned_alloc),
    MG_WRAP("valloc", g_valloc),
    MG_WRAP("pvalloc", g_pvalloc),
    MG_WRAP("malloc_usable_size", g_malloc_usable_size),
    MG_WRAP("mallopt", g_mallopt),
    MG_WRAP("malloc_trim", g_malloc_trim),
    MG_WRAP("mallinfo", g_mallinfo),
    MG_WRAP("reallocarray", g_reallocarray),
    MG_WRAP("_Znw" CXX_SIZE, g_new),
    MG_WRAP("_Zna" CXX_SIZE, g_new),
    MG_WRAP("_Znw" CXX_SIZE "RKSt9nothrow_t", g_new_nothrow),
    MG_WRAP("_Zna" CXX_SIZE "RKSt9nothrow_t", g_new_nothrow),
    MG_WRAP("_ZdlPv", g_delete),
    MG_WRAP("_ZdaPv", g_delete),
    MG_WRAP("_ZdlPvRKSt9nothrow_t", g_delete_nothrow),
    MG_WRAP("_ZdaPvRKSt9nothrow_t", g_delete_nothrow),
    MG_WRAP("_ZdlPv" CXX_SIZE, g_delete_sized),
    MG_WRAP("_ZdaPv" CXX_SIZE, g_delete_sized),
    MG_WRAP("mmap", g_mmap), MG_WRAP("__mmap2", mg_g_mmap2),
    MG_WRAP("mmap64", g_mmap64),
    MG_WRAP("munmap", g_munmap), MG_WRAP("mremap", g_mremap),
    MG_WRAP("mprotect", g_mprotect),
    MG_WRAP("madvise", g_madvise),
    MG_WRAP("posix_madvise", g_madvise),
    MG_WRAP("mlock", g_mlock),
    MG_WRAP("munlock", g_mlock),
    MG_WRAP("msync", g_msync),
    MG_WRAP("mincore", g_mincore),
    MG_END
};
