/*
 * malloc and mmap for guest code.
 *
 * The guest frees with free() whatever malloc, memalign or posix_memalign gave
 * it, and the host's allocators do not all agree to that (an aligned block on
 * Windows has its own free). So every guest block carries a small header that
 * says where the host's block starts, and free() needs to know nothing else.
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

/* What the guest has asked malloc for and not freed, in bytes, modulo 2^32 (mg_g_malloc_bytes): its blocks are the host's. */
static volatile i32 g_malloced;

u64 mg_g_malloc_bytes(void) { return (u32)mg_atomic_add(&g_malloced, 0); }

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

/* A block of n bytes at a multiple of align, its header right before it, from a host block made bigger to hold both. */
static void *aligned_block(size_t align, size_t n)
{
    u8 *raw, *p;
    if ((align & (align - 1)) || n > 0x7FFFFF00u - align) {
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    raw = (u8 *)malloc(USABLE(n) + align + sizeof(ghdr));
    if (!raw) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    p = (u8 *)MG_ALIGN_UP((mg_addr)raw + sizeof(ghdr), (mg_addr)align);
    HDR(p)->offset = (u32)(p - raw);
    HDR(p)->size = (u32)n;
    mg_atomic_add(&g_malloced, (i32)n);
    return p;
}

void *mg_g_malloc(size_t n)
{
    u8 *raw;
    if (n > 0x7FFFFF00u) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    if (H_ALIGN < G_ALIGN) return aligned_block(G_ALIGN, n);
    raw = (u8 *)malloc(USABLE(n) + G_ALIGN);
    if (!raw) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    HDR(raw + G_ALIGN)->offset = (u32)G_ALIGN;
    HDR(raw + G_ALIGN)->size = (u32)n;
    mg_atomic_add(&g_malloced, (i32)n);
    return raw + G_ALIGN;
}

void *mg_g_calloc(size_t n, size_t m)
{
    u64 total = (u64)n * (u64)m;
    u8 *raw;
    if (total > 0x7FFFFF00u) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    if (H_ALIGN < G_ALIGN) {
        u8 *p = (u8 *)aligned_block(G_ALIGN, (size_t)total);
        if (p) memset(p, 0, USABLE(total));
        return p;
    }
    raw = (u8 *)calloc(1, USABLE(total) + G_ALIGN);
    if (!raw) {
        mg_set_errno(GE_NOMEM);
        return NULL;
    }
    HDR(raw + G_ALIGN)->offset = (u32)G_ALIGN;
    HDR(raw + G_ALIGN)->size = (u32)total;
    mg_atomic_add(&g_malloced, (i32)total);
    return raw + G_ALIGN;
}

void mg_g_free(void *p)
{
    if (!p) return;
    mg_atomic_add(&g_malloced, -(i32)HDR(p)->size);
    free((u8 *)p - HDR(p)->offset);
}

static void *g_memalign(size_t align, size_t n)
{
    if (align <= G_ALIGN) return mg_g_malloc(n);
    return aligned_block(align, n);
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
    if (H_ALIGN >= G_ALIGN && HDR(p)->offset == G_ALIGN) {
        u32 was = HDR(p)->size;
        u8 *raw = (u8 *)realloc((u8 *)p - G_ALIGN, USABLE(n) + G_ALIGN);
        if (!raw) {
            mg_set_errno(GE_NOMEM);
            return NULL;
        }
        HDR(raw + G_ALIGN)->size = (u32)n;
        mg_atomic_add(&g_malloced, (i32)((u32)n - was));
        return raw + G_ALIGN;
    }
    /* An aligned block: the host cannot grow it in place and keep the alignment. */
    old = HDR(p)->size;
    q = mg_g_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, USABLE(old) < USABLE(n) ? USABLE(old) : USABLE(n));
    mg_g_free(p);
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
    if (!g_gone || !size) return;
    mg_lock_acquire(&g_region_lock);
    for (r = g_regions; r; r = r->next) {
        u8 *lo, *hi;
        if (!r->gone_pages || r->base >= end || r->base + r->size <= at) continue;
        lo = at > r->base ? at : r->base;
        hi = end < r->base + r->size ? end : r->base + r->size;
        gone_back(r, (size_t)(lo - r->base), (size_t)(hi - lo));
    }
    mg_lock_release(&g_region_lock);
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

/* The file's bytes as they are, a chunk at a time, to compare a mapping's pages with before writing them back. Under the lock. */
#define FILE_CHUNK (16 * MG_PAGE)
static u8 g_file_chunk[FILE_CHUNK];

/*
 * What the app wrote into [from, from + size) of a shared file mapping, back to the file, and into the file's other mappings:
 * the pages it still has, and of them those that differ from what the file holds -- most of a store's pages are as they were
 * the last time, and write_back_loop goes over them every few seconds. Under the lock.
 */
static void write_back(region *r, size_t from, size_t size)
{
    size_t end = from + size, at;
    if (!r->file || !r->write_back) return;
    if (end > r->file_bytes) end = r->file_bytes;
    for (at = from / MG_PAGE * MG_PAGE; at < end; at += FILE_CHUNK) {
        size_t chunk_end = at + FILE_CHUNK < end ? at + FILE_CHUNK : end, page;
        i32 got = mg_fd_held_pread(r->file, g_file_chunk, (u32)(chunk_end - at), r->file_offset + at);
        for (page = at / MG_PAGE; page * MG_PAGE < chunk_end; page++) {
            size_t a = page * MG_PAGE < from ? from : page * MG_PAGE, b = (page + 1) * MG_PAGE < chunk_end ? (page + 1) * MG_PAGE : chunk_end;
            /* (Not a page the app made no-access, or gave back: reading it would fault, and it holds nothing of the file's.) */
            if (!(r->live[page >> 3] & (1u << (page & 7))) || (r->noaccess && (r->noaccess[page >> 3] & (1u << (page & 7)))) || gone_has(r, page))
                continue;
            /* Up to the file's end, which the app may have moved in since it mapped it: Linux writes a mapping back no further. */
            if (got >= 0 && b > at + (size_t)got) b = at + (size_t)got;
            if (b <= a) continue;
            if (got >= 0 && !memcmp(g_file_chunk + (a - at), r->base + a, b - a)) continue;
            if (mg_fd_held_pwrite(r->file, r->base + a, (u32)(b - a), r->file_offset + a) > 0 && g_file_copies > 1)
                copies_written(r, r->file, r->file_offset + a, r->base + a, b - a);
        }
    }
}

void mg_mem_write_back(void)
{
    region *r;
    /* At the app's end, which may be a fault's: a lock left taken is not waited for, and nothing is written then. */
    if (!mg_lock_try(&g_region_lock)) return;
    for (r = g_regions; r; r = r->next) write_back(r, 0, r->file_bytes);
    mg_lock_release(&g_region_lock);
}

/*
 * Every writable shared file mapping written back, one region at a time under the lock (a fault or an mmap of another thread
 * waits for one region, not for all of them). The kernel's page cache holds what an app writes into such a mapping, and the file
 * has it even when the app is killed; here it was in the mapping alone until msync, munmap or the app's end. A phone's app is
 * suspended and then ended without notice, and TikTok Lite's key-value store (KEVA), which maps its files so and does not
 * msync, found them half written at the next start: 368 "fail to mark" a start on a Lumia 650 from its second start on, and
 * what it kept was wrong (package 628). Now also as the app is suspended (megler_settle_files), all of them, and every 5 s
 * (write_back_loop) those of up to `largest` bytes: each pass reads what it compares, and a store's files are small, where
 * a database mapped whole and written so (Realm's) may not be.
 */
void mg_mem_write_back_all(size_t largest)
{
    u32 index;
    for (index = 0;; index++) {
        region *r;
        u32 i = 0;
        mg_lock_acquire(&g_region_lock);
        for (r = g_regions; r && i < index; r = r->next) i++;
        if (r && (!largest || r->file_bytes <= largest)) write_back(r, 0, r->file_bytes);
        mg_lock_release(&g_region_lock);
        if (!r) break;
    }
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

static void *g_mmap_common(void *addr, size_t len, int prot, int flags, int fd, u64 offset)
{
    size_t size = MG_ALIGN_UP(len, MG_PAGE), reserve = MG_ALIGN_UP(len, 65536);
    region *r;
    u8 *p;
    if (!len) {
        mg_set_errno(GE_INVAL);
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
            r->next = g_regions;
            g_regions = r;
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
                r->next = g_regions;
                g_regions = r;
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
        r->live = (u8 *)mg_alloc(reserve / MG_PAGE / 8 + 1);
        region_mark(r, 0, size, true);
        if (!(prot & 7)) noaccess_mark(r, 0, size, true);
        mg_lock_acquire(&g_region_lock);
        r->next = g_regions;
        g_regions = r;
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
               the file is written through a descriptor (mg_mem_file_written). */
            mg_lock_acquire(&g_region_lock);
            r = region_of(p);
            if (r && !r->file) {
                r->file = mg_fd_hold(fd);
                r->file_offset = offset;
                r->file_bytes = len;
                r->write_back = (prot & G_PROT_WRITE) != 0;
                r->file_prot = host_prot(prot);
                if (r->file) mg_atomic_add(&g_file_copies, 1);
            }
            mg_lock_release(&g_region_lock);
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
    return g_mmap_common(addr, len, prot, flags, fd, (u64)(gulong)offset);
}

static void *g_mmap64(void *addr, size_t len, int prot, int flags, int fd, i64 offset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)offset);
}

/* mmap2's offset is in pages. */
void *mg_g_mmap2(void *addr, size_t len, int prot, int flags, int fd, u32 pgoffset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)pgoffset * MG_PAGE);
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
    mg_lock_acquire(&g_region_lock);
    for (link = &g_regions; (r = *link) != NULL; link = &r->next) {
        size_t offset;
        if ((u8 *)addr < r->base || (u8 *)addr >= r->base + r->size) continue;
        offset = (size_t)((u8 *)addr - r->base) & ~(size_t)(MG_PAGE - 1);
        if (size > r->size - offset) size = r->size - offset;
        write_back(r, offset, size);
        gone_mark(r, offset, size, false);
        region_mark(r, offset, size, false);
        if (!r->live_pages) {
            /* The last of it: the reservation goes back. */
            *link = r->next;
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
    p = g_mmap_common(NULL, n, G_PROT_READ | G_PROT_WRITE, 0x02 | G_MAP_ANONYMOUS, -1, 0);   /* MAP_PRIVATE */
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
    size_t size = MG_ALIGN_UP(len + ((mg_addr)addr & (MG_PAGE - 1)), MG_PAGE);
    u8 *p = (u8 *)MG_ALIGN_DOWN((mg_addr)addr, (mg_addr)MG_PAGE);
    bool owned;
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

static bool fix_fault(mg_addr address, bool write, bool exec);

bool mg_mmap_fix_fault(mg_addr address, bool write, bool exec)
{
    if (fix_fault(address, write, exec)) return true;
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

static bool fix_fault(mg_addr address, bool write, bool exec)
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
/* msync: what was written into a shared file mapping goes to the file now. */
static int g_msync(void *addr, size_t len, int flags)
{
    region *r;
    (void)flags;
    mg_lock_acquire(&g_region_lock);
    r = region_of(addr);
    if (r) write_back(r, (size_t)((u8 *)addr - r->base), len);
    mg_lock_release(&g_region_lock);
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
