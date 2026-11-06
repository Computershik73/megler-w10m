/*
 * The runtime's heap: the memory every object lives in (alloc_raw, mg_dvm_object.c). It is the runtime's own rather than
 * the C library's, so that the collector (docs/JAVA.md, "The collector") can tell a word that points into an object from
 * any other word, keep mark bits beside the objects instead of in them, and free them.
 *
 * Small objects, up to 2 KB, are cells in blocks of 64 KB. Each block holds one size class, and keeps a bitmap of the
 * cells in use, a bitmap of marks, a free list and how far it has handed out cells from its start. Blocks come sixteen at a
 * time in arenas of 1 MB from the host. Large objects are one allocation each from the C heap, kept in an array sorted by
 * address. An address's arena is found from a table of the megabytes of addresses (g_arena_starting), a large object by
 * binary search.
 *
 * A block a collection leaves empty is a spare one; past the first SPARE_COMMITTED of them, a spare block's memory after
 * its first page (which has its header, all that the lists and the collector read of a spare block) is given back to the
 * host, and committed again, zero, when the block is given a class. The arenas stayed committed whole before, at the most
 * the heap had ever had: 350 MB in Play Store with Google Play services, which a collection had brought down to 200.
 *
 * One lock covers it all. The collector takes it before it stops the other threads, so no thread can be stopped halfway
 * through the lists.
 */
#include "mg_dvm_internal.h"

#include <stdio.h>

#define BLOCK_SIZE 65536u
#define ARENA_BLOCKS 16u
#define ARENA_SIZE (BLOCK_SIZE * ARENA_BLOCKS)
#define BLOCK_MAGIC 0x4B4C4248u        /* "HBLK" */
#define MAX_SMALL 2048u
#define MAX_CELLS (BLOCK_SIZE / 16u)
#define BITMAP_WORDS (MAX_CELLS / 32u)

/* The sizes a cell can have, in bytes: finer where objects are many. */
static const u16 k_classes[] = { 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 2048 };
#define NCLASSES ((int)(sizeof(k_classes) / sizeof(k_classes[0])))

typedef struct free_cell {
    struct free_cell *next;
} free_cell;

typedef struct block {
    u32 magic;
    i32 cls;                           /* index into k_classes; -1: not in use yet */
    u32 cell, ncells, used, bump;      /* bytes a cell, cells it holds, cells in use, cells handed out from the start */
    u32 recip;                         /* 2^32 / cell, rounded up: a cell's index without a division (cell_index) */
    u8 *first;                         /* the first cell */
    free_cell *free;                   /* cells given back, to hand out again */
    bool bare;                         /* a spare block whose memory past its first page went back to the host */
    struct block *next;                /* the next block of the same class that has room */
    u32 inuse[BITMAP_WORDS];
    /*
     * The marks, BITMAP_WORDS of them, in memory of the arena's own outside its pages (add_arena): the collector writes them
     * while it marks alongside the app, and a page of the arena written then is one it looks at again (the write watch).
     */
    u32 *mark;
} block;
#define MARK_BYTES (BITMAP_WORDS * sizeof(u32))

typedef struct large {
    u8 *start;
    size_t size;
    volatile i32 mark;
} large;

static mg_lock g_heap_lock = MG_LOCK_INIT;
/*
 * The list of large objects, for the collector's threads that mark while the app makes them (mg_dvm_heap_alloc_black):
 * held over a lookup and its mark, and over an insertion, which moves the list's entries and may move the list.
 */
static mg_lock g_large_lock = MG_LOCK_INIT;
/* New objects are made marked: the collector is marking alongside the app, and what is made meanwhile lives. */
static volatile i32 g_black;
/* Whether every arena's pages are watched for writes (mg_mem_alloc_watched), and whether that was tried. */
static bool g_watch, g_watch_tried;
static u8 **g_arenas;                  /* sorted by address */
static u32 g_narenas, g_arenas_cap;
static block *g_spare;                 /* blocks of arenas not yet given a class */
static block *g_room[NCLASSES];        /* per class: blocks that may have room, the one in use first */
static large *g_large;                 /* sorted by start */
static u32 g_nlarge, g_large_cap;
static u8 g_class_of[MAX_SMALL / 16 + 1];
static volatile i32 g_classes_ready;
static u64 g_small_bytes, g_large_bytes, g_reserved, g_bare;
/* Spare blocks kept committed, ready for the next classes asked for (8 MB); the others' memory goes back (see the top). */
#define SPARE_COMMITTED 128u
static u32 g_objects;                  /* cells in use and large objects */
/* No mark is set: from a sweep, which clears every block's as it goes, until a marking starts (mg_dvm_heap_clear_marks). */
static bool g_marks_clear = true;
static mg_addr g_lo = ~(mg_addr)0, g_hi;   /* every object lies in [g_lo, g_hi) */

/*
 * Which arena an address is in, without a search: the arena that starts in each megabyte of 32-bit addresses (an arena is
 * a megabyte long, so no two start in the same one), and an address is in the one that starts in its own megabyte, below
 * it, or in the one that starts in the megabyte before. The collector asks for every reference it follows and every word
 * of the stacks it scans; the binary search over Yandex Maps' 160 arenas took eight steps each time. Arenas above 4 GB (a
 * 64-bit host's) are not in it, and those hosts search (g_arenas_high).
 */
#define ARENA_SHIFT 20
_Static_assert(ARENA_SIZE == 1u << ARENA_SHIFT, "an arena is a megabyte");
static u8 *g_arena_starting[1u << (32 - ARENA_SHIFT)];
static bool g_arenas_high;

static void widen(mg_addr start, size_t size)
{
    if (start < g_lo) g_lo = start;
    if (start + size > g_hi) g_hi = start + size;
}

void mg_dvm_heap_lock(void) { mg_lock_acquire(&g_heap_lock); }
void mg_dvm_heap_unlock(void) { mg_lock_release(&g_heap_lock); }

static void classes_init(void)
{
    u32 s;
    int c = 0;
    for (s = 0; s <= MAX_SMALL / 16; s++) {
        while (c < NCLASSES - 1 && k_classes[c] < s * 16) c++;
        g_class_of[s] = (u8)c;
    }
    mg_atomic_xchg(&g_classes_ready, 1);
}

/* A new arena: its blocks to the spare list, its address into the sorted list. False when the host had no memory. */
static bool add_arena(void)
{
    u8 *a = NULL;
    u32 *marks;
    u32 i, at;
    /* Watched for writes, where the host can, from the first arena on; one that cannot be leaves none taken for watched. */
    if (!g_watch_tried || g_watch) a = (u8 *)mg_mem_alloc_watched(ARENA_SIZE);
    if (!g_watch_tried) {
        g_watch_tried = true;
        g_watch = a != NULL;
        mg_info("heap: %s", g_watch ? "the arenas' pages are watched for writes: the collector can mark alongside the app"
                                    : "the host does not watch the arenas' pages for writes: every collection stops the app while it marks");
    } else if (g_watch && !a) {
        g_watch = false;
        mg_warn("heap: an arena could not be watched for writes: collections stop the app while they mark from now on");
    }
    if (!a) a = (u8 *)mg_mem_alloc(ARENA_SIZE);
    if (!a) return false;
    marks = (u32 *)calloc(ARENA_BLOCKS, MARK_BYTES);
    if (!marks) {
        mg_mem_release(a, ARENA_SIZE);
        return false;
    }
    if (g_narenas == g_arenas_cap) {
        u32 cap = g_arenas_cap ? g_arenas_cap * 2 : 64;
        u8 **grown = (u8 **)realloc(g_arenas, sizeof(*grown) * cap);
        if (!grown) {
            mg_mem_release(a, ARENA_SIZE);
            free(marks);
            return false;
        }
        g_arenas = grown;
        g_arenas_cap = cap;
    }
    for (at = g_narenas; at > 0 && g_arenas[at - 1] > a; at--) g_arenas[at] = g_arenas[at - 1];
    g_arenas[at] = a;
    g_narenas++;
    if ((u64)(mg_addr)a + ARENA_SIZE <= ((u64)1 << 32)) g_arena_starting[(u32)((mg_addr)a >> ARENA_SHIFT)] = a;
    else g_arenas_high = true;
    g_reserved += ARENA_SIZE;
    widen((mg_addr)a, ARENA_SIZE);
    /* Spare blocks are handed out lowest first. */
    for (i = ARENA_BLOCKS; i > 0; i--) {
        block *b = (block *)(void *)(a + (size_t)(i - 1) * BLOCK_SIZE);
        b->magic = BLOCK_MAGIC;
        b->cls = -1;
        b->mark = marks + (size_t)(i - 1) * BITMAP_WORDS;
        b->next = g_spare;
        g_spare = b;
    }
    return true;
}

/*
 * The index of the cell an offset into a block's cells falls in: the offset times the cell size's reciprocal, which is
 * exact for offsets under a block's 64 KB and cells of 16 bytes and more (every offset and size was tried). A division
 * is a call of the C library's on 32-bit ARM, and this is done for every allocation and every object a collection marks.
 */
static inline u32 cell_index(const block *b, size_t off) { return (u32)(((u64)(u32)off * b->recip) >> 32); }

/* A spare block made into one of class `c`. */
static block *new_block(int c)
{
    block *b;
    u32 header;
    if (!g_spare && !add_arena()) return NULL;
    b = g_spare;
    if (b->bare) {
        if (!mg_mem_commit((u8 *)b + MG_PAGE, BLOCK_SIZE - MG_PAGE, MG_PROT_RW)) return NULL;
        b->bare = false;
        g_bare -= BLOCK_SIZE - MG_PAGE;
    }
    g_spare = b->next;
    header = (u32)MG_ALIGN_UP(sizeof(block), (size_t)16);
    b->cls = c;
    b->cell = k_classes[c];
    b->recip = (u32)(0xFFFFFFFFu / b->cell) + 1u;
    b->ncells = (BLOCK_SIZE - header) / b->cell;
    b->used = b->bump = 0;
    b->first = (u8 *)b + header;
    b->free = NULL;
    memset(b->inuse, 0, sizeof(b->inuse));
    memset(b->mark, 0, MARK_BYTES);
    b->next = g_room[c];
    g_room[c] = b;
    return b;
}

static void *small_alloc(size_t size)
{
    int c;
    block *b;
    u8 *cell = NULL;
    u32 index;
    if (!g_classes_ready) classes_init();
    c = g_class_of[(size + 15) / 16];
    mg_lock_acquire(&g_heap_lock);
    for (;;) {
        b = g_room[c];
        if (!b) b = new_block(c);
        if (!b) break;
        if (b->free) {
            cell = (u8 *)b->free;
            b->free = b->free->next;
            break;
        }
        if (b->bump < b->ncells) {
            cell = b->first + (size_t)b->bump * b->cell;
            b->bump++;
            break;
        }
        /* Full: off the list of blocks with room, until a sweep gives it some. */
        g_room[c] = b->next;
        b->next = NULL;
    }
    if (cell) {
        /*
         * Zeroed before it is in use: the collector stops a thread wherever it is, and one stopped between the two left a
         * cell marked in use with what an object of another size had left in it, which the second stop of marking alongside
         * followed (TikTok Lite, after nine minutes: a "class" of two UTF-16 digits). A cell part made has zeros where its
         * header is not written yet, which the collector passes over.
         */
        memset(cell, 0, b->cell);
        index = cell_index(b, (size_t)(cell - b->first));
        b->inuse[index >> 5] |= 1u << (index & 31);
        if (g_black) {
            /* (The collector's threads set marks of the same word meanwhile: an interlocked OR.) */
            volatile i32 *word = (volatile i32 *)&b->mark[index >> 5];
            u32 old;
            do old = (u32)*word;
            while ((u32)mg_atomic_cas(word, (i32)old, (i32)(old | (1u << (index & 31)))) != old);
        }
        b->used++;
        g_objects++;
        g_small_bytes += b->cell;
    }
    mg_lock_release(&g_heap_lock);
    return cell;
}

static void *large_alloc(size_t size)
{
    u8 *p = (u8 *)calloc(1, size);
    u32 lo, hi;
    if (!p) return NULL;
    mg_lock_acquire(&g_heap_lock);
    mg_lock_acquire(&g_large_lock);
    if (g_nlarge == g_large_cap) {
        u32 cap = g_large_cap ? g_large_cap * 2 : 256;
        large *grown = (large *)realloc(g_large, sizeof(*grown) * cap);
        if (!grown) {
            mg_lock_release(&g_large_lock);
            mg_lock_release(&g_heap_lock);
            free(p);
            return NULL;
        }
        g_large = grown;
        g_large_cap = cap;
    }
    /* Where it goes in address order. */
    lo = 0;
    hi = g_nlarge;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_large[mid].start < p) lo = mid + 1;
        else hi = mid;
    }
    memmove(&g_large[lo + 1], &g_large[lo], sizeof(*g_large) * (g_nlarge - lo));
    g_large[lo].start = p;
    g_large[lo].size = size;
    g_large[lo].mark = g_black ? 1 : 0;
    g_nlarge++;
    mg_lock_release(&g_large_lock);
    g_objects++;
    g_large_bytes += size;
    widen((mg_addr)p, size);
    mg_lock_release(&g_heap_lock);
    return p;
}

void *mg_dvm_heap_alloc(size_t size)
{
    return size <= MAX_SMALL ? small_alloc(size) : large_alloc(size);
}

/* The block an address is in, or NULL. With the heap's lock held. */
static inline block *block_in(u8 *a, mg_addr p)
{
    block *b = (block *)(void *)(a + ((p - (mg_addr)a) / BLOCK_SIZE) * BLOCK_SIZE);
    return b->magic == BLOCK_MAGIC && b->cls >= 0 ? b : NULL;
}

static block *block_of(mg_addr p)
{
    u32 lo = 0, hi = g_narenas;
    if ((u64)p < ((u64)1 << 32)) {
        u32 m = (u32)(p >> ARENA_SHIFT);
        u8 *a = g_arena_starting[m];
        if (a && p >= (mg_addr)a) return block_in(a, p);
        a = m ? g_arena_starting[m - 1] : NULL;
        if (a && p < (mg_addr)a + ARENA_SIZE) return block_in(a, p);
        if (!g_arenas_high) return NULL;
    }
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        mg_addr a = (mg_addr)g_arenas[mid];
        if (p < a) hi = mid;
        else if (p >= a + ARENA_SIZE) lo = mid + 1;
        else return block_in(g_arenas[mid], p);
    }
    return NULL;
}

/* With the heap's lock held (the collector's). */
void *mg_dvm_heap_find(mg_addr p)
{
    block *b;
    u32 lo, hi;
    if (p < g_lo || p >= g_hi) return NULL;
    b = block_of(p);
    if (b) {
        u32 index;
        if (p < (mg_addr)b->first) return NULL;
        index = cell_index(b, (size_t)(p - (mg_addr)b->first));
        if (index >= b->bump || !(b->inuse[index >> 5] & (1u << (index & 31)))) return NULL;
        return b->first + (size_t)index * b->cell;
    }
    lo = 0;
    hi = g_nlarge;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const large *l = &g_large[mid];
        if (p < (mg_addr)l->start) hi = mid;
        else if (p >= (mg_addr)l->start + l->size) lo = mid + 1;
        else return l->start;
    }
    return NULL;
}

/* The large object that starts at `p`, or NULL. With the lock held. */
static large *large_at(const void *p)
{
    u32 lo = 0, hi = g_nlarge;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if ((const u8 *)p < g_large[mid].start) hi = mid;
        else if ((const u8 *)p > g_large[mid].start) lo = mid + 1;
        else return &g_large[mid];
    }
    return NULL;
}

/* Only an object's start in use is marked: anything else (a stale or made-up reference) is not an object, and false. */
bool mg_dvm_heap_set_mark(void *o)
{
    block *b;
    if ((mg_addr)o < g_lo || (mg_addr)o >= g_hi) return false;
    b = block_of((mg_addr)o);
    if (b) {
        u32 index, bit;
        if ((u8 *)o < b->first) return false;
        index = cell_index(b, (size_t)((u8 *)o - b->first));
        if ((size_t)index * b->cell != (size_t)((u8 *)o - b->first)) return false;
        bit = 1u << (index & 31);
        if (index >= b->bump || !(b->inuse[index >> 5] & bit) || (b->mark[index >> 5] & bit)) return false;
        b->mark[index >> 5] |= bit;
        return true;
    } else {
        large *l = large_at(o);
        if (!l || l->mark) return false;
        l->mark = 1;
        return true;
    }
}

bool mg_dvm_heap_set_mark_shared(void *o)
{
    block *b;
    if ((mg_addr)o < g_lo || (mg_addr)o >= g_hi) return false;
    b = block_of((mg_addr)o);
    if (b) {
        u32 index, bit, old;
        volatile i32 *word;
        if ((u8 *)o < b->first) return false;
        index = cell_index(b, (size_t)((u8 *)o - b->first));
        if ((size_t)index * b->cell != (size_t)((u8 *)o - b->first)) return false;
        bit = 1u << (index & 31);
        if (index >= b->bump || !(b->inuse[index >> 5] & bit)) return false;
        word = (volatile i32 *)&b->mark[index >> 5];
        for (;;) {
            old = (u32)*word;
            if (old & bit) return false;
            if ((u32)mg_atomic_cas(word, (i32)old, (i32)(old | bit)) == old) return true;
        }
    } else {
        large *l;
        bool set;
        mg_lock_acquire(&g_large_lock);
        l = large_at(o);
        set = l && !l->mark && mg_atomic_cas(&l->mark, 0, 1) == 0;
        mg_lock_release(&g_large_lock);
        return set;
    }
}

bool mg_dvm_heap_is_marked(const void *o)
{
    block *b = block_of((mg_addr)o);
    if (b) {
        u32 index = cell_index(b, (size_t)((const u8 *)o - b->first));
        return (b->mark[index >> 5] & (1u << (index & 31))) != 0;
    } else {
        large *l;
        bool marked;
        mg_lock_acquire(&g_large_lock);
        l = large_at(o);
        marked = l && l->mark;
        mg_lock_release(&g_large_lock);
        return marked;
    }
}

void mg_dvm_heap_alloc_black(bool on) { mg_atomic_xchg(&g_black, on ? 1 : 0); }
bool mg_dvm_heap_black(void) { return g_black != 0; }
bool mg_dvm_heap_watched(void) { return g_watch; }

/*
 * Every arena's write watch started again: before the first stop of a collection alongside the app, the app running (a
 * page written between this and that stop is looked at again at the second stop, for nothing). It was done with the world
 * stopped, for every page the app had written since the last collection, in each first stop of Spotify's on a Lumia 650:
 * 85 to 105 ms each, every 3 to 5 s (package 664). The arenas are listed under the heap's lock and watched again outside
 * it, so that what allocates does not wait meanwhile; one made after it is watched from its start.
 */
void mg_dvm_heap_watch_restart(void)
{
    u32 pages[ARENA_SIZE / MG_PAGE], i, n;
    u8 **arenas;
    if (!g_watch) return;
    mg_lock_acquire(&g_heap_lock);
    n = g_narenas;
    arenas = n ? (u8 **)malloc(sizeof(*arenas) * n) : NULL;
    if (arenas) memcpy(arenas, g_arenas, sizeof(*arenas) * n);
    mg_lock_release(&g_heap_lock);
    if (!arenas) return;
    for (i = 0; i < n; i++) mg_mem_written_pages(arenas[i], ARENA_SIZE, pages, (int)MG_COUNT(pages), true);
    free(arenas);
}

/* The marked objects of block `b` any of whose bytes lie in [lo, hi). */
static void marked_in(block *b, u8 *lo, u8 *hi, void (*fn)(void *o))
{
    u8 *end = b->first + (size_t)b->bump * b->cell;
    u32 c, last;
    if (lo < b->first) lo = b->first;
    if (hi > end) hi = end;
    if (lo >= hi) return;
    c = cell_index(b, (size_t)(lo - b->first));
    last = cell_index(b, (size_t)(hi - 1 - b->first));
    for (; c <= last; c++)
        if (b->mark[c >> 5] & b->inuse[c >> 5] & (1u << (c & 31))) fn(b->first + (size_t)c * b->cell);
}

u32 mg_dvm_heap_each_written(void (*fn)(void *o))
{
    u32 pages[ARENA_SIZE / MG_PAGE], i, written = 0;
    int n, k;
    for (i = 0; i < g_narenas; i++) {
        u8 *a = g_arenas[i];
        /* (The watch is not started again here: the next collection does, before its first stop.) */
        n = g_watch ? mg_mem_written_pages(a, ARENA_SIZE, pages, (int)MG_COUNT(pages), false) : -1;
        if (n < 0) {
            /* Not watched (or the host would not say): every page of it. */
            for (k = 0; k < (int)ARENA_BLOCKS; k++) {
                block *b = (block *)(void *)(a + (size_t)k * BLOCK_SIZE);
                if (b->cls >= 0) marked_in(b, (u8 *)b, (u8 *)b + BLOCK_SIZE, fn);
            }
            written += ARENA_SIZE / MG_PAGE;
            continue;
        }
        for (k = 0; k < n; k++) {
            u8 *page = a + (size_t)pages[k] * MG_PAGE;
            block *b = (block *)(void *)(a + ((size_t)pages[k] * MG_PAGE / BLOCK_SIZE) * BLOCK_SIZE);
            if (b->cls >= 0) marked_in(b, page, page + MG_PAGE, fn);
        }
        written += (u32)n;
    }
    /* The large objects are the C heap's memory, which nothing watches: every marked one. */
    for (i = 0; i < g_nlarge; i++)
        if (g_large[i].mark) fn(g_large[i].start);
    return written;
}

/*
 * Every mark cleared, for a marking that starts now: nothing to do after a sweep, which cleared them; otherwise (a census
 * marked, or a collection alongside the app was given up) every block's and every large object's. It went over every block
 * of the heap at each first stop of a collection alongside the app, Spotify's 3,000 and more (package 664).
 */
void mg_dvm_heap_clear_marks(void)
{
    u32 i, k;
    if (!g_marks_clear) {
        for (i = 0; i < g_narenas; i++)
            for (k = 0; k < ARENA_BLOCKS; k++) {
                block *b = (block *)(void *)(g_arenas[i] + (size_t)k * BLOCK_SIZE);
                if (b->cls >= 0) memset(b->mark, 0, MARK_BYTES);
            }
        for (i = 0; i < g_nlarge; i++) g_large[i].mark = 0;
    }
    g_marks_clear = false;
}

u32 mg_dvm_heap_objects(void) { return g_objects; }

/*
 * The sweep of the blocks, shared among the collector's threads (mg_dvm_gc_share): each takes the next block until there is
 * none, and finds its dead cells a word of the bitmaps at a time (in use and not marked). What each freed is its own count,
 * added up after. `dying` frees only C memory an object owns, which any thread may.
 */
typedef struct sweep_share {
    u64 objects, bytes;
} sweep_share;

static struct {
    void (*dying)(void *o);
    bool poison;
    volatile i32 next;
    u32 total;
    sweep_share share[MG_DVM_GC_WORKERS];
} S;

static void sweep_block(block *b, sweep_share *s)
{
    u32 words = (b->bump + 31) / 32, w, j;
    for (w = 0; w < words; w++) {
        u32 dead = b->inuse[w] & ~b->mark[w];
        if (!dead) continue;
        b->inuse[w] &= ~dead;
        for (j = 0; dead; j++, dead >>= 1) {
            u8 *cell;
            if (!(dead & 1)) continue;
            cell = b->first + (size_t)(w * 32 + j) * b->cell;
            S.dying(cell);
            b->used--;
            s->objects++;
            s->bytes += b->cell;
            if (S.poison) {
                /* Not in use, and not on the free list: never handed out again. */
                memset(cell, 0xDB, b->cell);
            } else {
                ((free_cell *)(void *)cell)->next = b->free;
                b->free = (free_cell *)(void *)cell;
            }
        }
    }
    memset(b->mark, 0, MARK_BYTES);
}

static void sweep_some(void *arg, int worker)
{
    sweep_share *s = &S.share[worker];
    (void)arg;
    for (;;) {
        u32 at = (u32)(mg_atomic_add(&S.next, 1) - 1);
        block *b;
        if (at >= S.total) return;
        b = (block *)(void *)(g_arenas[at / ARENA_BLOCKS] + (size_t)(at % ARENA_BLOCKS) * BLOCK_SIZE);
        if (b->cls >= 0) sweep_block(b, s);
    }
}

/*
 * The sweep, with the heap's lock held: every object in use and not marked is handed to `dying` (which frees what it owns
 * besides itself) and its memory is freed -- a cell to its block's free list, a large object to the C heap -- and every
 * mark is cleared for the next collection. Then the lists of blocks with room are made again, and a block left empty
 * becomes a spare one, for any class. `poison`: freed memory is filled with 0xDB and never handed out again, so that a use
 * after it is freed shows (the tests' way to find a root the collector missed). Yandex Maps' sweeps took 30 to 45 ms on
 * one thread, the heap locked meanwhile.
 */
u32 mg_dvm_heap_sweep(void (*dying)(void *o), bool poison, u64 *freed_objects, u64 *freed_bytes)
{
    u32 i, k, kept = 0, workers;
    int cl, w;
    memset(&S, 0, sizeof(S));
    S.dying = dying;
    S.poison = poison;
    S.total = g_narenas * ARENA_BLOCKS;
    workers = (u32)mg_dvm_gc_share(sweep_some, NULL);
    *freed_objects = *freed_bytes = 0;
    for (w = 0; w < MG_DVM_GC_WORKERS; w++) {
        *freed_objects += S.share[w].objects;
        *freed_bytes += S.share[w].bytes;
    }
    g_objects -= (u32)*freed_objects;
    g_small_bytes -= *freed_bytes;
    for (cl = 0; cl < NCLASSES; cl++) g_room[cl] = NULL;
    for (i = 0; i < g_narenas; i++)
        for (k = 0; k < ARENA_BLOCKS; k++) {
            block *b = (block *)(void *)(g_arenas[i] + (size_t)k * BLOCK_SIZE);
            if (b->cls < 0) continue;
            if (b->used == 0 && !poison) {
                /* Empty: any class may have it. */
                b->cls = -1;
                b->next = g_spare;
                g_spare = b;
            } else if (b->free || b->bump < b->ncells) {
                b->next = g_room[b->cls];
                g_room[b->cls] = b;
            } else {
                b->next = NULL;
            }
        }
    for (i = 0; i < g_nlarge; i++) {
        large *l = &g_large[i];
        if (l->mark) {
            l->mark = 0;
            g_large[kept++] = *l;
            continue;
        }
        dying(l->start);
        g_objects--;
        g_large_bytes -= l->size;
        (*freed_objects)++;
        *freed_bytes += l->size;
        if (poison) memset(l->start, 0xDB, l->size);
        else free(l->start);
    }
    g_nlarge = kept;
    g_marks_clear = true;
    /* Spare blocks past the first few: their memory past the header's page back to the host. */
    {
        block *b;
        u32 n = 0;
        for (b = g_spare; b; b = b->next)
            if (!b->bare && ++n > SPARE_COMMITTED && mg_mem_decommit((u8 *)b + MG_PAGE, BLOCK_SIZE - MG_PAGE)) {
                b->bare = true;
                g_bare += BLOCK_SIZE - MG_PAGE;
            }
    }
    return workers;
}

/*
 * What the large objects are when they take more than 32 MB: their bytes by class, the most first, the first `top` of
 * them ("[B 120.3 MB in 310, ..."). False, and nothing written, below that. With the heap's lock held.
 */
bool mg_dvm_heap_large_report(char *out, size_t cap, int top)
{
    struct {
        const mg_dvm_class *c;
        u64 bytes;
        u32 n;
    } k[64];
    u32 nk = 0, i, j;
    size_t n = 0;
    if (!cap) return false;
    out[0] = 0;
    if (g_large_bytes < ((u64)32 << 20)) return false;
    for (i = 0; i < g_nlarge; i++) {
        const mg_dvm_class *c = mg_dvm_class_of((const mg_jobj *)(const void *)g_large[i].start);
        for (j = 0; j < nk && k[j].c != c; j++) {}
        if (j == nk) {
            if (nk == MG_COUNT(k)) continue;
            k[nk].c = c;
            k[nk].bytes = 0;
            k[nk].n = 0;
            nk++;
        }
        k[j].bytes += g_large[i].size;
        k[j].n++;
    }
    for (; top > 0 && nk && n + 80 < cap; top--) {
        u32 best = 0;
        for (j = 1; j < nk; j++)
            if (k[j].bytes > k[best].bytes) best = j;
        n += (size_t)snprintf(out + n, cap - n, "%s%s %.1f MB in %u", n ? ", " : "", k[best].c && k[best].c->name ? k[best].c->name : "?",
                              (double)k[best].bytes / 1048576.0, (unsigned)k[best].n);
        k[best] = k[--nk];
    }
    return true;
}

void mg_dvm_heap_each_marked(void (*fn)(void *o))
{
    u32 i, k, c;
    for (i = 0; i < g_narenas; i++)
        for (k = 0; k < ARENA_BLOCKS; k++) {
            block *b = (block *)(void *)(g_arenas[i] + (size_t)k * BLOCK_SIZE);
            if (b->cls < 0) continue;
            for (c = 0; c < b->bump; c++)
                if (b->mark[c >> 5] & b->inuse[c >> 5] & (1u << (c & 31))) fn(b->first + (size_t)c * b->cell);
        }
    for (i = 0; i < g_nlarge; i++)
        if (g_large[i].mark) fn(g_large[i].start);
}

u64 mg_dvm_live_bytes(void) { return g_small_bytes + g_large_bytes; }

void mg_dvm_heap_stats(mg_dvm_heap_info *out)
{
    mg_lock_acquire(&g_heap_lock);
    out->small_bytes = g_small_bytes;
    out->large_bytes = g_large_bytes;
    out->reserved = g_reserved - g_bare;
    out->large_objects = g_nlarge;
    out->objects = g_objects;
    mg_lock_release(&g_heap_lock);
}
