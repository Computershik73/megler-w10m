/*
 * malloc and mmap for guest code.
 *
 * The guest frees with free() whatever malloc, memalign or posix_memalign gave
 * it, and the host's allocators do not all agree to that (an aligned block on
 * Windows has its own free). So every guest block carries a small header that
 * says where the host's block starts, and free() needs to know nothing else.
 */
#include "hk_bionic.h"
#include "hk_fd.h"

#include "../ld/hk_ld.h"

typedef struct ghdr {
    u32 offset;            /* from the host block's start to the guest pointer */
    u32 size;              /* what the guest asked for */
} ghdr;

#define HDR(p) ((ghdr *)((u8 *)(p) - sizeof(ghdr)))

void *hk_g_malloc(size_t n)
{
    u8 *raw;
    if (n > 0x7FFFFF00u) {
        hk_set_errno(GE_NOMEM);
        return NULL;
    }
    raw = (u8 *)malloc(n + sizeof(ghdr));
    if (!raw) {
        hk_set_errno(GE_NOMEM);
        return NULL;
    }
    ((ghdr *)raw)->offset = sizeof(ghdr);
    ((ghdr *)raw)->size = (u32)n;
    return raw + sizeof(ghdr);
}

void *hk_g_calloc(size_t n, size_t m)
{
    u64 total = (u64)n * (u64)m;
    u8 *raw;
    if (total > 0x7FFFFF00u) {
        hk_set_errno(GE_NOMEM);
        return NULL;
    }
    raw = (u8 *)calloc(1, (size_t)total + sizeof(ghdr));
    if (!raw) {
        hk_set_errno(GE_NOMEM);
        return NULL;
    }
    ((ghdr *)raw)->offset = sizeof(ghdr);
    ((ghdr *)raw)->size = (u32)total;
    return raw + sizeof(ghdr);
}

void hk_g_free(void *p)
{
    if (p) free((u8 *)p - HDR(p)->offset);
}

static void *g_memalign(size_t align, size_t n)
{
    u8 *raw, *p;
    if (align <= 8) return hk_g_malloc(n);
    if ((align & (align - 1)) || n > 0x7FFFFF00u - align) {
        hk_set_errno(GE_INVAL);
        return NULL;
    }
    raw = (u8 *)malloc(n + align + sizeof(ghdr));
    if (!raw) {
        hk_set_errno(GE_NOMEM);
        return NULL;
    }
    p = (u8 *)HK_ALIGN_UP((hk_addr)raw + sizeof(ghdr), (hk_addr)align);
    HDR(p)->offset = (u32)(p - raw);
    HDR(p)->size = (u32)n;
    return p;
}

void *hk_g_realloc(void *p, size_t n)
{
    void *q;
    u32 old;
    if (!p) return hk_g_malloc(n);
    if (!n) {
        hk_g_free(p);
        return NULL;
    }
    if (n > 0x7FFFFF00u) {
        hk_set_errno(GE_NOMEM);
        return NULL;
    }
    if (HDR(p)->offset == sizeof(ghdr)) {
        u8 *raw = (u8 *)realloc((u8 *)p - sizeof(ghdr), n + sizeof(ghdr));
        if (!raw) {
            hk_set_errno(GE_NOMEM);
            return NULL;
        }
        ((ghdr *)raw)->size = (u32)n;
        return raw + sizeof(ghdr);
    }
    /* An aligned block: the host cannot grow it in place and keep the alignment. */
    old = HDR(p)->size;
    q = hk_g_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    hk_g_free(p);
    return q;
}

char *hk_g_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)hk_g_malloc(n);
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
static void *g_valloc(size_t n) { return g_memalign(HK_PAGE, n); }
static void *g_pvalloc(size_t n) { return g_memalign(HK_PAGE, HK_ALIGN_UP(n, HK_PAGE)); }
static size_t g_malloc_usable_size(const void *p) { return p ? HDR(p)->size : 0; }
static int g_mallopt(int param, int value) { (void)param; (void)value; return 1; }
static int g_malloc_trim(size_t pad) { (void)pad; return 0; }

typedef struct g_mallinfo_t {
    u32 arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
} g_mallinfo_t;

static g_mallinfo_t g_mallinfo(void)
{
    g_mallinfo_t m;
    u64 used = hk_mem_usage();
    memset(&m, 0, sizeof(m));
    m.uordblks = used > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)used;
    m.arena = m.uordblks;
    return m;
}

static void *g_reallocarray(void *p, size_t n, size_t m)
{
    u64 total = (u64)n * (u64)m;
    if (total > 0x7FFFFF00u) {
        hk_set_errno(GE_NOMEM);
        return NULL;
    }
    return hk_g_realloc(p, (size_t)total);
}

/* The C++ runtime's allocation functions, for apps that link the system's minimal libstdc++. */
static void *g_new(size_t n)
{
    void *p = hk_g_malloc(n ? n : 1);
    if (!p) hk_fatal("guest: operator new(%u) failed", (unsigned)n);
    return p;
}
static void *g_new_nothrow(size_t n, const void *tag) { (void)tag; return hk_g_malloc(n ? n : 1); }
static void g_delete(void *p) { hk_g_free(p); }
static void g_delete_nothrow(void *p, const void *tag) { (void)tag; hk_g_free(p); }
static void g_delete_sized(void *p, size_t n) { (void)n; hk_g_free(p); }

/* -------------------------------------------------------------------- mmap
 *
 * Anonymous memory and private copies of files, and shared memory: a MAP_SHARED mapping of an ashmem region or a memfd is
 * a view of the region's own pages (hk_fd_shm_map), so that every mapping of it, in any thread, is the same memory, as on
 * Linux. Chromium moves everything between its browser and its renderer through such regions (Mojo's data pipes, which
 * carry a page's bytes), mapping one twice: with each mapping a copy of the region, the renderer read nothing of what the
 * network wrote, and every page the WebView loaded was empty. A region asked for as
 * writable and executable at once -- a JIT's code heap -- cannot exist on the
 * host, so it is writable until something executes in it and executable until
 * something writes to it: the fault handler flips the page (hk_mmap_fix_fault).
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
 * itself (hk_fd_file_map), which costs the app no memory of its own: the WebView maps its ICU data and its .pak files so,
 * 55 MB of them, each read into memory of the app's before. Any other mapping of a file is a copy of what it holds.
 */
#define G_PROT_READ 1
#define G_PROT_WRITE 2
#define G_PROT_EXEC 4
#define G_MAP_SHARED 1
#define G_MAP_FIXED 0x10
#define G_MAP_ANONYMOUS 0x20
#define G_MAP_FAILED ((void *)(hk_addr)-1)

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
     * A shared writable mapping of a file (MAP_SHARED, PROT_WRITE): the file, held, and which of its bytes the region
     * holds from its base. The pages are the layer's own copy of them; what the app writes into them is written back to
     * the file at msync, at munmap, and when the app ends (hk_mem_write_back) -- not as it is written, as the kernel would.
     */
    void *file;
    u64 file_offset;
    size_t file_bytes;
    /* A view of shared memory (hk_fd_shm_map): unmapped, not released, when its last page goes; its pages stay till then. */
    void *view;
    size_t view_size;
    struct region *next;
} region;

/* Marks the pages of [offset, offset + size) of a region as the app's or not; both are multiples of a page. */
static void region_mark(region *r, size_t offset, size_t size, bool on)
{
    size_t page = offset / HK_PAGE, end = (offset + size) / HK_PAGE;
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
static hk_lock g_region_lock = HK_LOCK_INIT;

/* What the app wrote into [from, from + size) of a shared file mapping, back to the file: the pages it still has. Under the lock. */
static void write_back(region *r, size_t from, size_t size)
{
    size_t end = from + size, page;
    if (!r->file) return;
    if (end > r->file_bytes) end = r->file_bytes;
    for (page = from / HK_PAGE; page * HK_PAGE < end; page++) {
        size_t a = page * HK_PAGE < from ? from : page * HK_PAGE, b = (page + 1) * HK_PAGE < end ? (page + 1) * HK_PAGE : end;
        if (!(r->live[page >> 3] & (1u << (page & 7)))) continue;
        hk_fd_held_pwrite(r->file, r->base + a, (u32)(b - a), r->file_offset + a);
    }
}

void hk_mem_write_back(void)
{
    region *r;
    /* At the app's end, which may be a fault's: a lock left taken is not waited for, and nothing is written then. */
    if (!hk_lock_try(&g_region_lock)) return;
    for (r = g_regions; r; r = r->next) write_back(r, 0, r->file_bytes);
    hk_lock_release(&g_region_lock);
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
    if (prot & G_PROT_WRITE) return HK_PROT_RW;
    if (prot & G_PROT_EXEC) return HK_PROT_RX;
    if (prot & G_PROT_READ) return HK_PROT_R;
    return HK_PROT_NONE;
}

/* fd-backed mappings read the file through the io layer. */
i32 hk_fd_pread(int fd, void *buf, u32 n, u64 offset);

static void *g_mmap_common(void *addr, size_t len, int prot, int flags, int fd, u64 offset)
{
    size_t size = HK_ALIGN_UP(len, HK_PAGE), reserve = HK_ALIGN_UP(len, 65536);
    region *r;
    u8 *p;
    if (!len) {
        hk_set_errno(GE_INVAL);
        return G_MAP_FAILED;
    }
    if (!(flags & G_MAP_ANONYMOUS)) {
        /* Shared memory whose protection was narrowed (ASharedMemory_setProt) maps no wider, as ashmem refuses it. */
        int allowed = hk_fd_shm_prot(fd);
        if (allowed >= 0 && (prot & ~allowed & 7)) {
            hk_set_errno(GE_PERM);
            return G_MAP_FAILED;
        }
    }
    if ((flags & G_MAP_SHARED) && !(flags & (G_MAP_FIXED | G_MAP_ANONYMOUS))) {
        /* Shared memory: a view of the region's pages (see the top). Anything else that is not shared memory goes on below. */
        void *base = NULL;
        size_t mapped = 0;
        bool writable = false;
        p = (u8 *)hk_fd_shm_map(fd, offset, len, &writable, &base, &mapped);
        if (p) {
            if ((prot & G_PROT_WRITE) && !writable) {
                hk_section_unmap(base, mapped);
                hk_set_errno(GE_ACCES);
                return G_MAP_FAILED;
            }
            r = (region *)hk_alloc(sizeof(*r));
            r->base = p;
            r->size = size;
            r->view = base;
            r->view_size = mapped;
            r->live = (u8 *)hk_alloc(size / HK_PAGE / 8 + 1);
            region_mark(r, 0, size, true);
            hk_lock_acquire(&g_region_lock);
            r->next = g_regions;
            g_regions = r;
            hk_lock_release(&g_region_lock);
            if (host_prot(prot) != HK_PROT_RW) hk_mem_protect(p, size, host_prot(prot));
            hk_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p, shared memory", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
            return p;
        }
        if (*hk_errno_ptr() != GE_NODEV) return G_MAP_FAILED;
        if ((prot & 7) == G_PROT_READ) {
            /* A file read-only: a view of the file (see the top), where it is a regular file opened read-only. */
            p = (u8 *)hk_fd_file_map(fd, offset, len, &base, &mapped);
            if (p) {
                r = (region *)hk_alloc(sizeof(*r));
                r->base = p;
                r->size = size;
                r->view = base;
                r->view_size = mapped;
                r->live = (u8 *)hk_alloc(size / HK_PAGE / 8 + 1);
                region_mark(r, 0, size, true);
                hk_lock_acquire(&g_region_lock);
                r->next = g_regions;
                g_regions = r;
                hk_lock_release(&g_region_lock);
                hk_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p, a view of the file", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
                return p;
            }
        }
    }
    if (flags & G_MAP_FIXED) {
        bool view;
        /* Only over memory this layer already owns: a fresh address cannot be demanded of the host. */
        hk_lock_acquire(&g_region_lock);
        r = region_of(addr);
        if (!r || (u8 *)addr + size > r->base + r->size || ((hk_addr)addr & (HK_PAGE - 1))) {
            hk_lock_release(&g_region_lock);
            hk_note_once("mmap(MAP_FIXED) at an address the layer does not own");
            hk_set_errno(GE_NOMEM);
            return G_MAP_FAILED;
        }
        region_mark(r, (size_t)((u8 *)addr - r->base), size, true);
        view = r->view != NULL;
        hk_lock_release(&g_region_lock);
        p = (u8 *)addr;
        if (!(prot & 7) && (flags & G_MAP_ANONYMOUS) && !view) {
            /* No access: given back, to be committed (zeroed) when mprotect gives it some. */
            hk_mem_decommit(p, size);
            hk_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p, decommitted", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
            return p;
        }
        if (!hk_mem_commit(p, size, HK_PROT_RW) || !hk_mem_protect(p, size, HK_PROT_RW)) {
            hk_set_errno(GE_NOMEM);
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
        p = addr && !((hk_addr)addr & 0xFFFF) ? (u8 *)hk_mem_reserve_at(addr, reserve) : NULL;
        if (!p) p = (u8 *)hk_mem_reserve(reserve);
        /* (Anonymous and with no access, reserved alone: see the top.) */
        if (!p || (((prot & 7) || !(flags & G_MAP_ANONYMOUS)) && !hk_mem_commit(p, size, HK_PROT_RW))) {
            if (p) hk_mem_release(p, reserve);
            hk_set_errno(GE_NOMEM);
            return G_MAP_FAILED;
        }
        r = (region *)hk_alloc(sizeof(*r));
        r->base = p;
        r->size = reserve;
        r->live = (u8 *)hk_alloc(reserve / HK_PAGE / 8 + 1);
        region_mark(r, 0, size, true);
        hk_lock_acquire(&g_region_lock);
        r->next = g_regions;
        g_regions = r;
        hk_lock_release(&g_region_lock);
    }
    if (!(flags & G_MAP_ANONYMOUS)) {
        size_t got = 0;
        while (got < len) {
            i32 n = hk_fd_pread(fd, p + got, (u32)(len - got), offset + got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if ((flags & G_MAP_SHARED) && (prot & G_PROT_WRITE)) {
            if (flags & G_MAP_FIXED) {
                hk_note_once("mmap: a shared writable file mapping over memory already mapped is a private copy here; writes do not reach the file");
            } else {
                /* The file, to write back to (region.file). */
                hk_lock_acquire(&g_region_lock);
                r = region_of(p);
                if (r && !r->file) {
                    r->file = hk_fd_hold(fd);
                    r->file_offset = offset;
                    r->file_bytes = len;
                }
                hk_lock_release(&g_region_lock);
            }
        }
    }
    if ((prot & G_PROT_WRITE) && (prot & G_PROT_EXEC)) {
        hk_lock_acquire(&g_region_lock);
        r = region_of(p);
        if (r) r->wx = true;
        hk_lock_release(&g_region_lock);
    } else if (host_prot(prot) != HK_PROT_RW && ((prot & 7) || !(flags & G_MAP_ANONYMOUS))) {
        hk_mem_protect(p, size, host_prot(prot));
        if (prot & G_PROT_EXEC) hk_icache_flush(p, size);
    }
    hk_trace("mmap(%p, %u, prot %d, flags 0x%x, fd %d) -> %p", addr, (unsigned)len, prot, (unsigned)flags, fd, (void *)p);
    return p;
}

static void *g_mmap(void *addr, size_t len, int prot, int flags, int fd, i32 offset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)(u32)offset);
}

static void *g_mmap64(void *addr, size_t len, int prot, int flags, int fd, i64 offset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)offset);
}

/* mmap2's offset is in pages. */
void *hk_g_mmap2(void *addr, size_t len, int prot, int flags, int fd, u32 pgoffset)
{
    return g_mmap_common(addr, len, prot, flags, fd, (u64)pgoffset * HK_PAGE);
}

static int g_munmap(void *addr, size_t len)
{
    region *r, **link;
    size_t size = HK_ALIGN_UP(len, HK_PAGE);
    u8 *keep_lo, *keep_hi;
    hk_trace("munmap(%p, %u)", addr, (unsigned)len);
    /*
     * What a library loaded into the app's own range occupies stays mapped (hk_ld_placed_overlap): its image, and the code
     * the patcher added right after it, which on Android is the free end of the range. Chromium's linker unmaps that end
     * once the library is in, and the stubs there went with it.
     */
    if (size && hk_ld_placed_overlap(addr, size, &keep_lo, &keep_hi)) {
        if ((u8 *)addr < keep_lo) g_munmap(addr, (size_t)(keep_lo - (u8 *)addr));
        if (keep_hi < (u8 *)addr + size) g_munmap(keep_hi, (size_t)((u8 *)addr + size - keep_hi));
        return 0;
    }
    hk_lock_acquire(&g_region_lock);
    for (link = &g_regions; (r = *link) != NULL; link = &r->next) {
        size_t offset;
        if ((u8 *)addr < r->base || (u8 *)addr >= r->base + r->size) continue;
        offset = (size_t)((u8 *)addr - r->base) & ~(size_t)(HK_PAGE - 1);
        if (size > r->size - offset) size = r->size - offset;
        write_back(r, offset, size);
        region_mark(r, offset, size, false);
        if (!r->live_pages) {
            /* The last of it: the reservation goes back. */
            *link = r->next;
            hk_lock_release(&g_region_lock);
            hk_fd_let_go(r->file);
            if (r->view) hk_section_unmap(r->view, r->view_size);
            else hk_mem_release(r->base, r->size);
            free(r->live);
            free(r);
            return 0;
        }
        /*
         * Part of a region: the host frees address space only whole, so the
         * pages go and the addresses stay reserved until the rest has gone
         * too. (Under the lock: once it is let go another thread may unmap
         * the rest, and the addresses may be someone else's the moment after.)
         */
        if (!r->view) hk_mem_decommit(r->base + offset, size);
        hk_lock_release(&g_region_lock);
        return 0;
    }
    hk_lock_release(&g_region_lock);
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
    size_t o = HK_ALIGN_UP(old_size, HK_PAGE), n = HK_ALIGN_UP(new_size, HK_PAGE);
    void *p;
    if (((hk_addr)old & (HK_PAGE - 1)) || !o || !n || (flags & ~1)) {
        hk_set_errno(GE_INVAL);
        return G_MAP_FAILED;
    }
    if (n <= o) {
        if (n < o) g_munmap((u8 *)old + n, o - n);
        return old;
    }
    if (!(flags & 1)) {
        hk_set_errno(GE_NOMEM);
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
 */
static struct { u8 *lo, *hi; } g_wx_code[64];
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
        if (p >= g_wx_code[i].lo && p + size <= g_wx_code[i].hi) return true;
    if (g_wx_code_count == HK_COUNT(g_wx_code)) return false;
    g_wx_code[g_wx_code_count].lo = p;
    g_wx_code[g_wx_code_count].hi = p + size;
    g_wx_code_count++;
    return true;
}

static int g_mprotect(void *addr, size_t len, int prot)
{
    size_t size = HK_ALIGN_UP(len + ((hk_addr)addr & (HK_PAGE - 1)), HK_PAGE);
    u8 *p = (u8 *)HK_ALIGN_DOWN((hk_addr)addr, (hk_addr)HK_PAGE);
    bool owned;
    hk_lock_acquire(&g_region_lock);
    owned = region_of(p) != NULL;
    hk_lock_release(&g_region_lock);
    if (!owned) owned = hk_ld_is_guest_code((hk_addr)p);
    hk_trace("mprotect(%p, %u, prot %d)%s", addr, (unsigned)len, prot, owned ? "" : " -- not the app's memory, left alone");
    if (!owned) {
        /* An inline hook may target a bionic export implemented in the host
         * executable. Do not promise executable access we cannot grant: the
         * caller would then patch read-only host code (WhatsApp's abort hook).
         * Keep the existing treatment of non-executable stack guard requests. */
        if (prot & G_PROT_EXEC) {
            hk_warn("mprotect(%p, %u, prot %d): host memory, executable access refused", addr, (unsigned)len, prot);
            hk_set_errno(GE_ACCES);
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
        if (hk_atomic_add(&said, 1) <= 6)
            hk_warn("mprotect(%p, %u, prot %d): not memory this layer gave the app; left as it is", addr, (unsigned)len, prot);
        return 0;
    }
    {
        /* Memory of a region of the app's that may have been reserved alone (PROT_NONE, see mmap) is committed to be given access. */
        region *r;
        bool view;
        hk_lock_acquire(&g_region_lock);
        r = region_of(p);
        view = r && r->view;
        hk_lock_release(&g_region_lock);
        if (r && !view && (prot & 7) && !hk_mem_commit(p, size, HK_PROT_RW)) {
            hk_set_errno(GE_NOMEM);
            return -1;
        }
        if (r && !view && !(prot & 7)) {
            /* No access: for what is committed; what is not is no-access already. */
            return hk_mem_protect_committed(p, size, HK_PROT_NONE) ? 0 : (hk_set_errno(GE_ACCES), -1);
        }
    }
    if ((prot & G_PROT_WRITE) && (prot & G_PROT_EXEC)) {
        region *r;
        bool kept;
        hk_lock_acquire(&g_region_lock);
        r = region_of(p);
        if (r) r->wx = true;
        kept = r || wx_code_add(p, size);
        hk_lock_release(&g_region_lock);
        if (!kept) hk_note_once("mprotect: writable and executable at once, more library pages than are kept (%p); it stays as it is", addr);
        else hk_mem_protect(p, size, HK_PROT_RW);
        return 0;
    }
    if (!hk_mem_protect(p, size, host_prot(prot))) {
        hk_set_errno(GE_ACCES);
        return -1;
    }
    if (prot & G_PROT_EXEC) hk_icache_flush(p, size);
    return 0;
}

/* A write-or-execute fault in a region, or a library's page, that wants both: give the page what this access needs. */
bool hk_mmap_fix_fault(hk_addr address, bool write, bool exec)
{
    region *r;
    bool wx;
    u8 *page = (u8 *)HK_ALIGN_DOWN(address, (hk_addr)HK_PAGE);
    if (!write && !exec) return false;
    hk_lock_acquire(&g_region_lock);
    r = region_of((const void *)address);
    wx = r ? r->wx : wx_code_has(page);
    hk_lock_release(&g_region_lock);
    if (!wx) return false;
    if (exec) {
        if (!hk_mem_protect(page, HK_PAGE, HK_PROT_RX)) return false;
        hk_icache_flush(page, HK_PAGE);
        return true;
    }
    return hk_mem_protect(page, HK_PAGE, HK_PROT_RW);
}

static int g_madvise(void *addr, size_t len, int advice) { (void)addr; (void)len; (void)advice; return 0; }
static int g_mlock(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
/* msync: what was written into a shared file mapping goes to the file now. */
static int g_msync(void *addr, size_t len, int flags)
{
    region *r;
    (void)flags;
    hk_lock_acquire(&g_region_lock);
    r = region_of(addr);
    if (r) write_back(r, (size_t)((u8 *)addr - r->base), len);
    hk_lock_release(&g_region_lock);
    return 0;
}
static int g_mincore(void *addr, size_t len, u8 *vec)
{
    size_t i, n = HK_ALIGN_UP(len, HK_PAGE) / HK_PAGE;
    (void)addr;
    for (i = 0; i < n; i++) vec[i] = 1;
    return 0;
}

const hk_sym hk_tab_mem[] = {
    HK_WRAP("malloc", hk_g_malloc),
    HK_WRAP("calloc", hk_g_calloc),
    HK_WRAP("realloc", hk_g_realloc),
    HK_WRAP("free", hk_g_free),
    HK_WRAP("memalign", g_memalign),
    HK_WRAP("posix_memalign", g_posix_memalign),
    HK_WRAP("aligned_alloc", g_aligned_alloc),
    HK_WRAP("valloc", g_valloc),
    HK_WRAP("pvalloc", g_pvalloc),
    HK_WRAP("malloc_usable_size", g_malloc_usable_size),
    HK_WRAP("mallopt", g_mallopt),
    HK_WRAP("malloc_trim", g_malloc_trim),
    HK_WRAP("mallinfo", g_mallinfo),
    HK_WRAP("reallocarray", g_reallocarray),
    HK_WRAP("_Znwj", g_new),
    HK_WRAP("_Znaj", g_new),
    HK_WRAP("_ZnwjRKSt9nothrow_t", g_new_nothrow),
    HK_WRAP("_ZnajRKSt9nothrow_t", g_new_nothrow),
    HK_WRAP("_ZdlPv", g_delete),
    HK_WRAP("_ZdaPv", g_delete),
    HK_WRAP("_ZdlPvRKSt9nothrow_t", g_delete_nothrow),
    HK_WRAP("_ZdaPvRKSt9nothrow_t", g_delete_nothrow),
    HK_WRAP("_ZdlPvj", g_delete_sized),
    HK_WRAP("_ZdaPvj", g_delete_sized),
    HK_WRAP("mmap", g_mmap), HK_WRAP("__mmap2", hk_g_mmap2),
    HK_WRAP("mmap64", g_mmap64),
    HK_WRAP("munmap", g_munmap), HK_WRAP("mremap", g_mremap),
    HK_WRAP("mprotect", g_mprotect),
    HK_WRAP("madvise", g_madvise),
    HK_WRAP("posix_madvise", g_madvise),
    HK_WRAP("mlock", g_mlock),
    HK_WRAP("munlock", g_mlock),
    HK_WRAP("msync", g_msync),
    HK_WRAP("mincore", g_mincore),
    HK_END
};
