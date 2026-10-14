/*
 * The runtime's heap: the memory every object lives in (alloc_raw, hk_dvm_object.c). It is the runtime's own rather than
 * the C library's, so that the collector (docs/JAVA.md, "The collector") can tell a word that points into an object from
 * any other word, keep mark bits beside the objects instead of in them, and free them.
 *
 * Small objects, up to 2 KB, are cells in blocks of 64 KB. Each block holds one size class, and keeps a bitmap of the
 * cells in use, a bitmap of marks, a free list and how far it has handed out cells from its start. Blocks come sixteen at a
 * time in arenas of 1 MB from the host. Large objects are one allocation each from the C heap, kept in an array sorted by
 * address. An address is looked up by binary search, over the arenas and then within the large objects.
 *
 * One lock covers it all. The collector takes it before it stops the other threads, so no thread can be stopped halfway
 * through the lists.
 */
#include "hk_dvm_internal.h"

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
    u8 *first;                         /* the first cell */
    free_cell *free;                   /* cells given back, to hand out again */
    struct block *next;                /* the next block of the same class that has room */
    u32 inuse[BITMAP_WORDS], mark[BITMAP_WORDS];
} block;

typedef struct large {
    u8 *start;
    size_t size;
    bool mark;
} large;

static hk_lock g_heap_lock = HK_LOCK_INIT;
static u8 **g_arenas;                  /* sorted by address */
static u32 g_narenas, g_arenas_cap;
static block *g_spare;                 /* blocks of arenas not yet given a class */
static block *g_room[NCLASSES];        /* per class: blocks that may have room, the one in use first */
static large *g_large;                 /* sorted by start */
static u32 g_nlarge, g_large_cap;
static u8 g_class_of[MAX_SMALL / 16 + 1];
static volatile i32 g_classes_ready;
static u64 g_small_bytes, g_large_bytes, g_reserved;
static u32 g_objects;                  /* cells in use and large objects */
static hk_addr g_lo = ~(hk_addr)0, g_hi;   /* every object lies in [g_lo, g_hi) */

static void widen(hk_addr start, size_t size)
{
    if (start < g_lo) g_lo = start;
    if (start + size > g_hi) g_hi = start + size;
}

void hk_dvm_heap_lock(void) { hk_lock_acquire(&g_heap_lock); }
void hk_dvm_heap_unlock(void) { hk_lock_release(&g_heap_lock); }

static void classes_init(void)
{
    u32 s;
    int c = 0;
    for (s = 0; s <= MAX_SMALL / 16; s++) {
        while (c < NCLASSES - 1 && k_classes[c] < s * 16) c++;
        g_class_of[s] = (u8)c;
    }
    hk_atomic_xchg(&g_classes_ready, 1);
}

/* A new arena: its blocks to the spare list, its address into the sorted list. False when the host had no memory. */
static bool add_arena(void)
{
    u8 *a = (u8 *)hk_mem_alloc(ARENA_SIZE);
    u32 i, at;
    if (!a) return false;
    if (g_narenas == g_arenas_cap) {
        u32 cap = g_arenas_cap ? g_arenas_cap * 2 : 64;
        u8 **grown = (u8 **)realloc(g_arenas, sizeof(*grown) * cap);
        if (!grown) {
            hk_mem_release(a, ARENA_SIZE);
            return false;
        }
        g_arenas = grown;
        g_arenas_cap = cap;
    }
    for (at = g_narenas; at > 0 && g_arenas[at - 1] > a; at--) g_arenas[at] = g_arenas[at - 1];
    g_arenas[at] = a;
    g_narenas++;
    g_reserved += ARENA_SIZE;
    widen((hk_addr)a, ARENA_SIZE);
    /* Spare blocks are handed out lowest first. */
    for (i = ARENA_BLOCKS; i > 0; i--) {
        block *b = (block *)(void *)(a + (size_t)(i - 1) * BLOCK_SIZE);
        b->magic = BLOCK_MAGIC;
        b->cls = -1;
        b->next = g_spare;
        g_spare = b;
    }
    return true;
}

/* A spare block made into one of class `c`. */
static block *new_block(int c)
{
    block *b;
    u32 header;
    if (!g_spare && !add_arena()) return NULL;
    b = g_spare;
    g_spare = b->next;
    header = (u32)HK_ALIGN_UP(sizeof(block), (size_t)16);
    b->cls = c;
    b->cell = k_classes[c];
    b->ncells = (BLOCK_SIZE - header) / b->cell;
    b->used = b->bump = 0;
    b->first = (u8 *)b + header;
    b->free = NULL;
    memset(b->inuse, 0, sizeof(b->inuse));
    memset(b->mark, 0, sizeof(b->mark));
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
    hk_lock_acquire(&g_heap_lock);
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
        index = (u32)((size_t)(cell - b->first) / b->cell);
        b->inuse[index >> 5] |= 1u << (index & 31);
        b->used++;
        g_objects++;
        g_small_bytes += b->cell;
    }
    hk_lock_release(&g_heap_lock);
    if (cell) memset(cell, 0, b->cell);
    return cell;
}

static void *large_alloc(size_t size)
{
    u8 *p = (u8 *)calloc(1, size);
    u32 lo, hi;
    if (!p) return NULL;
    hk_lock_acquire(&g_heap_lock);
    if (g_nlarge == g_large_cap) {
        u32 cap = g_large_cap ? g_large_cap * 2 : 256;
        large *grown = (large *)realloc(g_large, sizeof(*grown) * cap);
        if (!grown) {
            hk_lock_release(&g_heap_lock);
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
    g_large[lo].mark = false;
    g_nlarge++;
    g_objects++;
    g_large_bytes += size;
    widen((hk_addr)p, size);
    hk_lock_release(&g_heap_lock);
    return p;
}

void *hk_dvm_heap_alloc(size_t size)
{
    return size <= MAX_SMALL ? small_alloc(size) : large_alloc(size);
}

/* The block an address is in, or NULL. With the heap's lock held. */
static block *block_of(hk_addr p)
{
    u32 lo = 0, hi = g_narenas;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        hk_addr a = (hk_addr)g_arenas[mid];
        if (p < a) hi = mid;
        else if (p >= a + ARENA_SIZE) lo = mid + 1;
        else {
            block *b = (block *)(void *)(a + ((p - a) / BLOCK_SIZE) * BLOCK_SIZE);
            return b->magic == BLOCK_MAGIC && b->cls >= 0 ? b : NULL;
        }
    }
    return NULL;
}

/* With the heap's lock held (the collector's). */
void *hk_dvm_heap_find(hk_addr p)
{
    block *b;
    u32 lo, hi;
    if (p < g_lo || p >= g_hi) return NULL;
    b = block_of(p);
    if (b) {
        u32 index;
        if (p < (hk_addr)b->first) return NULL;
        index = (u32)((p - (hk_addr)b->first) / b->cell);
        if (index >= b->bump || !(b->inuse[index >> 5] & (1u << (index & 31)))) return NULL;
        return b->first + (size_t)index * b->cell;
    }
    lo = 0;
    hi = g_nlarge;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const large *l = &g_large[mid];
        if (p < (hk_addr)l->start) hi = mid;
        else if (p >= (hk_addr)l->start + l->size) lo = mid + 1;
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
bool hk_dvm_heap_set_mark(void *o)
{
    block *b;
    if ((hk_addr)o < g_lo || (hk_addr)o >= g_hi) return false;
    b = block_of((hk_addr)o);
    if (b) {
        u32 index, bit;
        if ((u8 *)o < b->first || (size_t)((u8 *)o - b->first) % b->cell) return false;
        index = (u32)((size_t)((u8 *)o - b->first) / b->cell);
        bit = 1u << (index & 31);
        if (index >= b->bump || !(b->inuse[index >> 5] & bit) || (b->mark[index >> 5] & bit)) return false;
        b->mark[index >> 5] |= bit;
        return true;
    } else {
        large *l = large_at(o);
        if (!l || l->mark) return false;
        l->mark = true;
        return true;
    }
}

bool hk_dvm_heap_is_marked(const void *o)
{
    block *b = block_of((hk_addr)o);
    if (b) {
        u32 index = (u32)((size_t)((const u8 *)o - b->first) / b->cell);
        return (b->mark[index >> 5] & (1u << (index & 31))) != 0;
    } else {
        large *l = large_at(o);
        return l && l->mark;
    }
}

void hk_dvm_heap_clear_marks(void)
{
    u32 i, k;
    for (i = 0; i < g_narenas; i++)
        for (k = 0; k < ARENA_BLOCKS; k++) {
            block *b = (block *)(void *)(g_arenas[i] + (size_t)k * BLOCK_SIZE);
            if (b->cls >= 0) memset(b->mark, 0, sizeof(b->mark));
        }
    for (i = 0; i < g_nlarge; i++) g_large[i].mark = false;
}

u32 hk_dvm_heap_objects(void) { return g_objects; }

/*
 * The sweep, with the heap's lock held: every object in use and not marked is handed to `dying` (which frees what it owns
 * besides itself) and its memory is freed -- a cell to its block's free list, a large object to the C heap -- and every
 * mark is cleared for the next collection. Then the lists of blocks with room are made again, and a block left empty
 * becomes a spare one, for any class. `poison`: freed memory is filled with 0xDB and never handed out again, so that a use
 * after it is freed shows (the tests' way to find a root the collector missed).
 */
void hk_dvm_heap_sweep(void (*dying)(void *o), bool poison, u64 *freed_objects, u64 *freed_bytes)
{
    u32 i, k, c, kept = 0;
    int cl;
    *freed_objects = *freed_bytes = 0;
    for (cl = 0; cl < NCLASSES; cl++) g_room[cl] = NULL;
    for (i = 0; i < g_narenas; i++)
        for (k = 0; k < ARENA_BLOCKS; k++) {
            block *b = (block *)(void *)(g_arenas[i] + (size_t)k * BLOCK_SIZE);
            if (b->cls < 0) continue;
            for (c = 0; c < b->bump; c++) {
                u32 w = c >> 5, bit = 1u << (c & 31);
                u8 *cell;
                if (!(b->inuse[w] & bit) || (b->mark[w] & bit)) continue;
                cell = b->first + (size_t)c * b->cell;
                dying(cell);
                b->inuse[w] &= ~bit;
                b->used--;
                g_objects--;
                g_small_bytes -= b->cell;
                (*freed_objects)++;
                *freed_bytes += b->cell;
                if (poison) {
                    /* Not in use, and not on the free list: never handed out again. */
                    memset(cell, 0xDB, b->cell);
                } else {
                    ((free_cell *)(void *)cell)->next = b->free;
                    b->free = (free_cell *)(void *)cell;
                }
            }
            memset(b->mark, 0, sizeof(b->mark));
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
            l->mark = false;
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
}

void hk_dvm_heap_each_marked(void (*fn)(void *o))
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

u64 hk_dvm_live_bytes(void) { return g_small_bytes + g_large_bytes; }

void hk_dvm_heap_stats(hk_dvm_heap_info *out)
{
    hk_lock_acquire(&g_heap_lock);
    out->small_bytes = g_small_bytes;
    out->large_bytes = g_large_bytes;
    out->reserved = g_reserved;
    out->large_objects = g_nlarge;
    out->objects = g_objects;
    hk_lock_release(&g_heap_lock);
}
