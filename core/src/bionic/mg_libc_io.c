/*
 * File descriptors and the file system, as guest code sees them.
 *
 * Guest descriptors are this layer's own small integers. Behind one is a host
 * file, an in-memory file this layer made up (/proc/cpuinfo), a pipe, an
 * eventfd, one of the standard streams -- which go to the log -- or an object
 * of another module's: a socket (bionic/mg_net.c). Pipes and eventfds exist
 * here because native_app_glue and every engine's main loop are built on
 * them, and nothing on the host has their shape.
 *
 * Guest paths are Android's. A mount table says where each prefix lives on
 * the host; what has no mount does not exist, except the handful of /proc and
 * /sys files that code reads to learn about the device.
 */
#include "mg_bionic.h"

#include <stdio.h>

#include "../elf/mg_elf.h"
#include "mg_fd.h"

/* --------------------------------------------------------------- the paths
 *
 * The shared storage, Android's "SD card", is one folder for every app:
 * /storage/emulated/0. The other names it goes by are that path written
 * another way and are rewritten to it before anything else looks at a path,
 * as the symbolic links they are on Android would be followed.
 *
 * Below it some names are mounts of their own: the app's own part
 * (Android/data/<package>, Android/obb/<package>), which stays in the app's
 * folder on the host, the same parts of the other apps that have run here,
 * which are in their folders, and the links -- folders of the phone itself, whose
 * host paths are foreign ones (host/mg_host.h). Android's shared storage
 * does not tell A from a, so the part of a path below it is matched without
 * regard to the case of ASCII letters.
 */
#define EXTERNAL "/storage/emulated/0"
#define EXTERNAL_LEN (sizeof(EXTERNAL) - 1)

typedef struct mount {
    char *guest, *host;
    size_t glen;
    size_t exact;                      /* how much of `guest` is matched letter for letter: all of it, or as far as the shared storage */
    bool link;                         /* one of the phone's own folders */
    bool read_only;
} mount;
static mount g_mounts[320];            /* the app's own, every other app's two parts, the links */
static int g_nmounts;
static char g_package[160] = "com.example.app";
static char g_internal[256] = "/data/data/com.example.app";
static char g_external[64] = EXTERNAL;
static char g_cwd[512] = "/";
static char *g_space_dir;              /* a host folder on the volume everything is kept on: what statfs asks about */

static const struct {
    const char *path;
    bool listed;                       /* whether its parent's listing shows it */
} k_aliases[] = {
    { "/sdcard", true }, { "/mnt/sdcard", true }, { "/storage/self/primary", true },
    /* What older apps were told; Android 9 has no such name, and neither has a listing of /storage. */
    { "/storage/sdcard0", false },
};

/* The folders Android's shared storage has from the start (Environment.DIRECTORY_*). */
static const char *const k_standard_dirs[] = { "Alarms", "DCIM", "Documents", "Download", "Movies", "Music", "Notifications", "Pictures", "Podcasts", "Ringtones" };

static bool ascii_same(const char *a, const char *b, size_t n, bool any_case)
{
    size_t i;
    if (!any_case) return !strncmp(a, b, n);
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
        if (!x) break;
    }
    return true;
}

/* Whether the first n characters of a guest path are those of a mount's, n being no more than either has. */
static bool mount_same(const mount *m, const char *g, size_t n)
{
    size_t strict = n < m->exact ? n : m->exact;
    return !strncmp(g, m->guest, strict) && ascii_same(g + strict, m->guest + strict, n - strict, true);
}

static mount *mount_find(const char *guest_prefix)
{
    int i;
    for (i = 0; i < g_nmounts; i++)
        if (!strcmp(g_mounts[i].guest, guest_prefix)) return &g_mounts[i];
    return NULL;
}

void mg_fs_mount(const char *guest_prefix, const char *host_dir)
{
    mount *m = mount_find(guest_prefix);
    if (m) {
        free(m->host);
        m->host = mg_strdup(host_dir);
        m->link = m->read_only = false;
        return;
    }
    if (g_nmounts >= (int)MG_COUNT(g_mounts)) {
        mg_err("files: no room for one more mount: %s does not exist", guest_prefix);
        return;
    }
    m = &g_mounts[g_nmounts++];
    memset(m, 0, sizeof(*m));
    m->guest = mg_strdup(guest_prefix);
    m->host = mg_strdup(host_dir);
    m->glen = strlen(guest_prefix);
    m->exact = !strncmp(guest_prefix, EXTERNAL "/", EXTERNAL_LEN + 1) ? EXTERNAL_LEN + 1 : m->glen;
}

void mg_fs_configure(const char *package, const char *internal_dir, const char *external_dir)
{
    static const char *const own[] = { "data", "obb" };
    char *p, *h;
    size_t i;
    mg_strlcpy(g_package, package, sizeof(g_package));
    snprintf(g_internal, sizeof(g_internal), "/data/data/%s", package);
    mg_path_mkdirs(internal_dir);
    mg_fs_mount(g_internal, internal_dir);
    p = mg_sprintf("/data/user/0/%s", package);
    mg_fs_mount(p, internal_dir);
    free(p);
    /*
     * The app's own part of the shared storage. The folders are made now: a mount with nothing behind it would not exist, and its
     * parents are the shared storage's, so an app making the path one name at a time could not make it either.
     */
    for (i = 0; i < MG_COUNT(own); i++) {
        p = mg_sprintf(EXTERNAL "/Android/%s/%s", own[i], package);
        h = mg_sprintf("%s/Android/%s/%s", external_dir, own[i], package);
        mg_path_mkdirs(h);
        mg_fs_mount(p, h);
        free(p);
        free(h);
    }
    p = mg_sprintf("%s/tmp", internal_dir);
    mg_path_mkdirs(p);
    mg_fs_mount("/data/local/tmp", p);
    mg_fs_mount("/tmp", p);
    free(p);
    mg_strlcpy(g_cwd, g_internal, sizeof(g_cwd));
    if (!g_space_dir) g_space_dir = mg_strdup(internal_dir);
}

void mg_fs_shared_storage(const char *shared_dir, const mg_fs_link *links, int count)
{
    char *p, *h;
    size_t i;
    int k, n = 0;
    /* The links there were are gone: the table is what this call says. */
    for (k = 0; k < g_nmounts; k++) {
        if (g_mounts[k].link) {
            free(g_mounts[k].guest);
            free(g_mounts[k].host);
        } else {
            g_mounts[n++] = g_mounts[k];
        }
    }
    g_nmounts = n;
    mg_path_mkdirs(shared_dir);
    mg_fs_mount(EXTERNAL, shared_dir);
    free(g_space_dir);
    g_space_dir = mg_strdup(shared_dir);
    /* Real folders, so that an app can make what Android lets it make beside its own part (Android/media/<package>). */
    p = mg_sprintf("%s/Android/data", shared_dir);
    mg_path_mkdirs(p);
    free(p);
    p = mg_sprintf("%s/Android/obb", shared_dir);
    mg_path_mkdirs(p);
    free(p);
    for (i = 0; i < MG_COUNT(k_standard_dirs); i++) {
        bool linked = false;
        for (k = 0; k < count && !linked; k++) linked = strlen(links[k].name) == strlen(k_standard_dirs[i]) && ascii_same(links[k].name, k_standard_dirs[i], strlen(k_standard_dirs[i]), true);
        if (linked) continue;
        p = mg_sprintf("%s/%s", shared_dir, k_standard_dirs[i]);
        mg_path_mkdir(p, NULL);
        free(p);
    }
    for (k = 0; k < count; k++) {
        mount *m;
        p = mg_sprintf(EXTERNAL "/%s", links[k].name);
        h = mg_sprintf(MG_FOREIGN_PREFIX "%d", links[k].root);
        mg_fs_mount(p, h);
        m = mount_find(p);
        if (m) {
            m->link = true;
            m->read_only = links[k].read_only;
        }
        free(p);
        free(h);
    }
}

void mg_fs_other_app(const char *package, const char *external_dir)
{
    static const char *const parts[] = { "data", "obb" };
    mg_stat st;
    size_t i;
    if (!strcmp(package, g_package)) return;
    for (i = 0; i < MG_COUNT(parts); i++) {
        char *h = mg_sprintf("%s/Android/%s/%s", external_dir, parts[i], package);
        if (mg_path_stat(h, &st, NULL) && st.is_dir) {
            char *p = mg_sprintf(EXTERNAL "/Android/%s/%s", parts[i], package);
            mg_fs_mount(p, h);
            free(p);
        }
        free(h);
    }
}

const char *mg_fs_internal_dir(void) { return g_internal; }
const char *mg_fs_external_dir(void) { return g_external; }
const char *mg_fs_package(void) { return g_package; }

/* A path that begins with one of the shared storage's other names, said with its own; else the path as it is. Takes `g`. */
static char *without_alias(char *g)
{
    size_t i;
    for (i = 0; i < MG_COUNT(k_aliases); i++) {
        size_t n = strlen(k_aliases[i].path);
        if (!strncmp(g, k_aliases[i].path, n) && (!g[n] || g[n] == '/')) {
            char *out = mg_sprintf(EXTERNAL "%s", g + n);
            free(g);
            return out;
        }
    }
    return g;
}

/*
 * An absolute guest path with ".", ".." and doubled slashes resolved, and the
 * shared storage called by its one name; malloc'd.
 */
static char *normalize(const char *path)
{
    size_t cap = strlen(path) + strlen(g_cwd) + 4, n = 0;
    char *out = (char *)mg_alloc(cap), *full;
    const char *p;
    if (path[0] == '/') full = mg_strdup(path);
    else full = mg_sprintf("%s/%s", g_cwd, path);
    p = full;
    while (*p) {
        const char *seg;
        size_t len;
        while (*p == '/') p++;
        seg = p;
        while (*p && *p != '/') p++;
        len = (size_t)(p - seg);
        if (!len || (len == 1 && seg[0] == '.')) continue;
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n && out[n - 1] != '/') n--;
            if (n) n--;
            continue;
        }
        out[n++] = '/';
        memcpy(out + n, seg, len);
        n += len;
    }
    if (!n) out[n++] = '/';
    out[n] = 0;
    free(full);
    return without_alias(out);
}

/* The mount a normalized path is in: the one with the longest prefix. */
static const mount *mount_of(const char *g)
{
    const mount *best = NULL;
    size_t len = strlen(g);
    int i;
    for (i = 0; i < g_nmounts; i++) {
        const mount *m = &g_mounts[i];
        if (len < m->glen || !mount_same(m, g, m->glen)) continue;
        /* A whole component: "/sdcard" is not a prefix of "/sdcard2". */
        if (m->glen > 1 && g[m->glen] && g[m->glen] != '/') continue;
        if (!best || m->glen > best->glen) best = m;
    }
    return best;
}

static char *host_of_normal(const char *g)
{
    const mount *best = mount_of(g);
    const char *rest;
    if (!best) return NULL;
    rest = g + (best->glen == 1 ? 1 : best->glen);
#if MG_HOST_MAPS_NAMES
    if (rest[0]) {
        /* The guest's names as the host takes them (host/mg_host.h): here a backslash in one is still known to be a name's. */
        char *mapped = mg_name_to_host(rest, MG_NAME_FOREIGN), *out;
        out = mg_sprintf("%s%s%s", best->host, (best->glen == 1 && g[1]) ? "/" : "", mapped);
        free(mapped);
        return out;
    }
#endif
    return mg_sprintf("%s%s%s", best->host, (best->glen == 1 && g[1]) ? "/" : "", rest);
}

/* Whether a normalized path is a mount's own name. */
static bool is_mount_point(const char *g)
{
    const mount *m = mount_of(g);
    return m && !g[m->glen];
}

/*
 * Whether a normalized path is a directory because something is mounted
 * below it: "/storage/emulated" exists for /storage/emulated/0, whether or
 * not a folder of the host stands behind it. `g` is "/" or has no slash at
 * its end.
 */
static bool leads_to_mount(const char *g)
{
    size_t len = strlen(g), i;
    int k;
    if (len == 1) return g_nmounts > 0;
    for (k = 0; k < g_nmounts; k++) {
        const mount *m = &g_mounts[k];
        if (m->glen > len && m->guest[len] == '/' && mount_same(m, g, len)) return true;
    }
    for (i = 0; i < MG_COUNT(k_aliases); i++)
        if (strlen(k_aliases[i].path) > len && k_aliases[i].path[len] == '/' && !strncmp(k_aliases[i].path, g, len)) return true;
    return false;
}

/* Whether a normalized path is in a link that may only be read. */
static bool in_read_only(const char *g)
{
    const mount *m = mount_of(g);
    return m && m->read_only;
}

char *mg_fs_host_path(const char *guest_path)
{
    char *g = normalize(guest_path), *h = host_of_normal(g);
    free(g);
    return h;
}

/* ------------------------------------------------------- made-up files */

typedef struct textbuf {
    char *s;
    size_t n, cap;
} textbuf;

static void tb_add(textbuf *t, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (t->cap - t->n < 512) {
        t->cap = t->cap ? t->cap * 2 : 2048;
        t->s = (char *)mg_realloc(t->s, t->cap);
    }
    va_start(ap, fmt);
    n = vsnprintf(t->s + t->n, t->cap - t->n, fmt, ap);
    va_end(ap);
    if (n > 0) t->n += (size_t)n < t->cap - t->n ? (size_t)n : t->cap - t->n - 1;
}

/* /proc/self/maps, and smaps (`details`): each region followed by its sizes, as Linux gives them. */
typedef struct maps_out {
    textbuf *t;
    bool details;
} maps_out;

/*
 * What smaps says of a region after its maps line: its size, the kernel's page size (Instagram reads KernelPageSize there to
 * learn the page size, and ended at its start with "Could not get page size" when the file was not there), and nothing
 * resident, which is not known here.
 */
static void smaps_details(textbuf *t, u32 size, int prot)
{
    tb_add(t, "Size:           %8u kB\nKernelPageSize:        4 kB\nMMUPageSize:           4 kB\nRss:                   0 kB\nPss:                   0 kB\n"
              "Shared_Clean:          0 kB\nShared_Dirty:          0 kB\nPrivate_Clean:         0 kB\nPrivate_Dirty:         0 kB\n"
              "Referenced:            0 kB\nAnonymous:             0 kB\nAnonHugePages:         0 kB\nSwap:                  0 kB\nSwapPss:               0 kB\n"
              "Locked:                0 kB\nVmFlags:%s%s%s mr mw me\n",
           (unsigned)(size >> 10), (prot & PF_R) ? " rd" : "", (prot & PF_W) ? " wr" : "", (prot & PF_X) ? " ex" : "");
}

static int maps_one(mg_addr bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    maps_out *o = (maps_out *)user;
    textbuf *t = o->t;
    const Elf32_Phdr *ph = (const Elf32_Phdr *)phdr;
    unsigned i;
    for (i = 0; i < phnum; i++) {
        u32 lo, hi;
        if (ph[i].p_type != PT_LOAD) continue;
        lo = MG_ALIGN_DOWN(ph[i].p_vaddr, MG_PAGE);
        hi = MG_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, MG_PAGE);
        tb_add(t, "%08x-%08x %c%c%cp %08x 103:01 %u /data/app/%s-1/lib/arm/%s\n", (unsigned)(bias + lo), (unsigned)(bias + hi),
               (ph[i].p_flags & PF_R) ? 'r' : '-', (ph[i].p_flags & PF_W) ? 'w' : '-', (ph[i].p_flags & PF_X) ? 'x' : '-',
               (unsigned)MG_ALIGN_DOWN(ph[i].p_offset, MG_PAGE), 1000 + i, g_package, name);
        if (o->details) smaps_details(t, hi - lo, (int)ph[i].p_flags);
    }
    return 0;
}

/*
 * The command line Chromium-based apps (Brave and its kind) read at start. Renderer and GPU run as Android services in
 * processes of their own, which this platform cannot start: in a single process the renderer runs on threads of the
 * browser's, and the GPU too.
 */
#define CHROME_COMMAND_LINE "/data/local/chrome-command-line"

/* Whether a path is one of the files synthetic() makes up. */
static bool is_synthetic(const char *g)
{
    return !strncmp(g, "/proc/", 6) || !strncmp(g, "/sys/", 5) || !strcmp(g, CHROME_COMMAND_LINE);
}

/*
 * The content of a /proc or /sys file some code reads to learn about the device, or of the Chromium command line; NULL
 * when there is none by that name.
 */
static char *synthetic(const char *g, size_t *size)
{
    textbuf t;
    int cpus = mg_cpu_count(), i;
    memset(&t, 0, sizeof(t));
    if (!strcmp(g, "/proc/cpuinfo")) {
        for (i = 0; i < cpus; i++)
            tb_add(&t, "processor\t: %d\nmodel name\t: ARMv7 Processor rev 4 (v7l)\nBogoMIPS\t: 38.40\n"
                       "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae\n"
                       "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xd03\nCPU revision\t: 4\n\n", i);
        tb_add(&t, "Hardware\t: Qualcomm Technologies, Inc MSM\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n");
    } else if (!strcmp(g, "/proc/meminfo")) {
        u64 limit = mg_mem_limit(), used = mg_mem_usage();
        if (!limit) limit = 1024ull * 1024 * 1024;
        tb_add(&t, "MemTotal:       %u kB\nMemFree:        %u kB\nMemAvailable:   %u kB\nBuffers:        0 kB\nCached:         0 kB\n"
                   "SwapTotal:      0 kB\nSwapFree:       0 kB\n",
               (unsigned)(limit >> 10), (unsigned)((limit > used ? limit - used : 0) >> 10), (unsigned)((limit > used ? limit - used : 0) >> 10));
    } else if (!strcmp(g, "/proc/self/maps") || !strcmp(g, "/proc/self/smaps")) {
        maps_out o;
        o.t = &t;
        o.details = g[11] == 's';
        mg_ld_iterate(maps_one, &o);
        /* (With no library loaded yet, smaps still says the page size, of a region of the app's heap.) */
        if (o.details && !t.n) {
            tb_add(&t, "10000000-10001000 rw-p 00000000 00:00 0 [anon:libc_malloc]\n");
            smaps_details(&t, MG_PAGE, PF_R | PF_W);
        }
        if (!t.s) tb_add(&t, "%s", "");
    } else if (!strcmp(g, "/proc/self/cmdline")) {
        tb_add(&t, "%s", g_package);
        t.n++;                 /* the terminating NUL is part of the file */
    } else if (!strcmp(g, "/proc/self/status")) {
        tb_add(&t, "Name:\t%.15s\nState:\tR (running)\nTgid:\t4242\nPid:\t4242\nPPid:\t1\nTracerPid:\t0\nUid:\t10100\t10100\t10100\t10100\n"
                   "Gid:\t10100\t10100\t10100\t10100\nVmRSS:\t%u kB\nThreads:\t1\n", g_package, (unsigned)(mg_mem_usage() >> 10));
    } else if (!strcmp(g, "/proc/stat")) {
        tb_add(&t, "cpu  100 0 100 10000 0 0 0 0 0 0\n");
        for (i = 0; i < cpus; i++) tb_add(&t, "cpu%d 100 0 100 10000 0 0 0 0 0 0\n", i);
    } else if (!strcmp(g, "/proc/version")) {
        tb_add(&t, "Linux version 4.4.0-megler (megler@w10m) #1 SMP PREEMPT\n");
    } else if (!strcmp(g, "/sys/devices/system/cpu/possible") || !strcmp(g, "/sys/devices/system/cpu/present") ||
               !strcmp(g, "/sys/devices/system/cpu/online")) {
        if (cpus > 1) tb_add(&t, "0-%d\n", cpus - 1);
        else tb_add(&t, "0\n");
    } else if (!strncmp(g, "/sys/devices/system/cpu/cpu", 27) && strstr(g, "/cpufreq/")) {
        const char *leaf = strrchr(g, '/') + 1;
        if (atoi(g + 27) >= cpus) return NULL;
        if (!strcmp(leaf, "cpuinfo_max_freq") || !strcmp(leaf, "scaling_max_freq") || !strcmp(leaf, "scaling_cur_freq")) tb_add(&t, "1800000\n");
        else if (!strcmp(leaf, "cpuinfo_min_freq") || !strcmp(leaf, "scaling_min_freq")) tb_add(&t, "300000\n");
        else return NULL;
    } else if (!strcmp(g, CHROME_COMMAND_LINE)) {
        tb_add(&t, "chrome --single-process --in-process-gpu\n");
    } else {
        return NULL;
    }
    *size = t.n;
    return t.s;
}

/* ----------------------------------------------------- open descriptions */

enum { OBJ_FILE = 1, OBJ_MEM, OBJ_NULL, OBJ_RANDOM, OBJ_PIPE_R, OBJ_PIPE_W, OBJ_EVENT, OBJ_LOG, OBJ_EPOLL, OBJ_DIRFD, OBJ_EXT, OBJ_TIMER };

#define PIPE_CAP 65536u

typedef struct pipe_buf {
    u8 data[PIPE_CAP];
    u32 head, count;
    int readers, writers;
} pipe_buf;

typedef struct epoll_item {
    int fd;
    u32 events;
    u64 data;
    int last;                      /* what was last reported and still holds (EPOLLET) */
    bool disarmed;                 /* reported once with EPOLLONESHOT */
} epoll_item;

typedef struct gobj {
    int type, refs;
    int flags;                     /* the guest's O_* flags */
    mg_file *file;
    char *path;                    /* guest path, for files and directories */
    u8 *mem;
    size_t mem_size;
    u64 pos;
    pipe_buf *pipe;
    u64 counter;                   /* eventfd */
    bool semaphore;
    /* timerfd: when it next expires, on the monotonic clock (0: disarmed); its interval; the expirations not read yet */
    u64 timer_next, timer_interval, timer_ticks;
    int timer_clock;
    char line[512];                /* a standard stream's unfinished line */
    u32 line_len;
    int log_level;
    epoll_item *items;
    int nitems, epoll_next;
    const mg_fd_ops *ops;          /* OBJ_EXT: another module's object (a socket) */
    void *ext;
    /* OBJ_MEM made as shared memory (mg_fd_shm_create): writable, resized by ftruncate, and the protection mmap allows. */
    bool shm;
    int shm_prot;
    int shm_seals;                 /* memfd's F_ADD_SEALS */
    /*
     * Its pages: a section of the host's (mg_section), of which every mmap is a view (mg_fd_shm_map), all the same memory,
     * as Linux's mappings of an ashmem region or a memfd are. `capacity` is what the section holds; NULL while the size is
     * 0. The descriptor keeps no view of its own: read and write map what they copy for as long as they copy it
     * (shm_copy). A view of its own took the region's size in address space once more while the descriptor was open:
     * Chromium in a single process maps each image it decodes twice, so that made three stretches of its size, and
     * Spotify's WebView ended out of memory on one of 126 MB with 135 MB the largest stretch free.
     */
    mg_section *section;
    size_t capacity;
} gobj;

#define G_O_ACCMODE 3
#define G_O_CREAT 0100
#define G_O_EXCL 0200
#define G_O_TRUNC 01000
#define G_O_APPEND 02000
#define G_O_NONBLOCK 04000
#define G_O_DIRECTORY 040000
#define G_O_CLOEXEC 02000000

#define MAX_FDS 1024
static gobj *g_fds[MAX_FDS];
static u8 g_cloexec[MAX_FDS];
static mg_lock g_io_lock = MG_LOCK_INIT;
static mg_cond g_io_cond;
static bool g_io_inited;

static void io_init(void)
{
    gobj *o;
    int i;
    if (g_io_inited) return;
    g_io_inited = true;
    mg_cond_init(&g_io_cond);
    /* 0 reads as end of file; 1 and 2 are the log. */
    o = (gobj *)mg_alloc(sizeof(*o));
    o->type = OBJ_NULL;
    o->refs = 1;
    g_fds[0] = o;
    for (i = 1; i <= 2; i++) {
        o = (gobj *)mg_alloc(sizeof(*o));
        o->type = OBJ_LOG;
        o->refs = 1;
        o->flags = 1;
        o->log_level = i == 1 ? MG_LOG_INFO : MG_LOG_WARN;
        g_fds[i] = o;
    }
}

/* Called with the lock held. */
static int fd_alloc_locked(gobj *o, int at_least)
{
    int fd;
    for (fd = at_least < 0 ? 0 : at_least; fd < MAX_FDS; fd++) {
        if (!g_fds[fd]) {
            g_fds[fd] = o;
            g_cloexec[fd] = 0;
            return fd;
        }
    }
    return -1;
}

static int fd_new(gobj *o)
{
    int fd;
    io_init();
    mg_lock_acquire(&g_io_lock);
    fd = fd_alloc_locked(o, 3);
    mg_lock_release(&g_io_lock);
    if (fd < 0) mg_set_errno(GE_MFILE);
    return fd;
}

static gobj *fd_get(int fd)
{
    gobj *o;
    io_init();
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    mg_lock_acquire(&g_io_lock);
    o = g_fds[fd];
    if (o) o->refs++;
    mg_lock_release(&g_io_lock);
    return o;
}

static void obj_destroy(gobj *o)
{
    if (o->file) mg_file_close(o->file);
    if (o->type == OBJ_EXT && o->ops && o->ops->close) o->ops->close(o->ext);
    if (o->type == OBJ_LOG && o->line_len) mg_log(o->log_level, "%.*s", (int)o->line_len, o->line);
    /* Shared memory: the app's own views keep the pages until they are unmapped. */
    if (o->section) mg_section_close(o->section);
    free(o->mem);
    free(o->path);
    free(o->items);
    free(o);
}

static void fd_put(gobj *o)
{
    bool last;
    pipe_buf *p = NULL;
    mg_lock_acquire(&g_io_lock);
    last = --o->refs == 0;
    if (last && o->pipe) {
        p = o->pipe;
        if (o->type == OBJ_PIPE_R) p->readers--;
        else p->writers--;
        if (p->readers || p->writers) p = NULL;
        mg_cond_broadcast(&g_io_cond);
    }
    mg_lock_release(&g_io_lock);
    if (last) {
        free(p);
        obj_destroy(o);
    }
}

int mg_fd_register_with(const mg_fd_ops *ops, void *ext, int flags)
{
    gobj *o = (gobj *)mg_alloc(sizeof(*o));
    int fd;
    o->type = OBJ_EXT;
    o->refs = 1;
    o->ops = ops;
    o->ext = ext;
    o->flags = flags & (G_O_ACCMODE | G_O_NONBLOCK);
    fd = fd_new(o);
    if (fd < 0) free(o);
    else if (flags & G_O_CLOEXEC) g_cloexec[fd] = 1;
    return fd;
}

int mg_fd_register(const mg_fd_ops *ops, void *ext) { return mg_fd_register_with(ops, ext, 2); }

void *mg_fd_ext(int fd, const mg_fd_ops *ops)
{
    gobj *o = fd_get(fd);
    void *e = NULL;
    if (!o) return NULL;
    if (o->type == OBJ_EXT && o->ops == ops) e = o->ext;
    fd_put(o);
    return e;
}

void *mg_fd_ext_hold(int fd, const mg_fd_ops *ops, void **held, int *flags)
{
    gobj *o = fd_get(fd);
    *held = NULL;
    if (!o) {
        mg_set_errno(GE_BADF);
        return NULL;
    }
    if (o->type != OBJ_EXT || o->ops != ops) {
        fd_put(o);
        mg_set_errno(GE_NOTSOCK);
        return NULL;
    }
    *held = o;
    if (flags) *flags = o->flags;
    return o->ext;
}

void mg_fd_notify(void)
{
    mg_lock_acquire(&g_io_lock);
    mg_cond_broadcast(&g_io_cond);
    mg_lock_release(&g_io_lock);
}

/* ------------------------------------------------------------------ open */

int mg_fd_open_host(const char *host_path, int host_flags)
{
    int err = 0, fd;
    mg_file *f = mg_file_open(host_path, host_flags, &err);
    gobj *o;
    if (!f) {
        mg_set_errno(mg_errno_of(err));
        return -1;
    }
    o = (gobj *)mg_alloc(sizeof(*o));
    o->type = OBJ_FILE;
    o->refs = 1;
    o->file = f;
    o->flags = (host_flags & MG_O_WRITE) ? ((host_flags & MG_O_READ) ? 2 : 1) : 0;
    fd = fd_new(o);
    if (fd < 0) obj_destroy(o);
    return fd;
}

static int g_open(const char *path, int flags, int mode)
{
    char *g, *h;
    gobj *o;
    mg_stat st;
    int fd, err = 0, hf = 0, acc = flags & G_O_ACCMODE;
    (void)mode;
    mg_trace("open(%s, 0x%x)", path ? path : "(null)", (unsigned)flags);
    if (!path || !*path) {
        mg_set_errno(GE_NOENT);
        return -1;
    }
    g = normalize(path);
    o = (gobj *)mg_alloc(sizeof(*o));
    o->refs = 1;
    o->flags = flags;
    o->path = g;
    if (!strcmp(g, "/dev/null") || !strcmp(g, "/dev/zero")) {
        o->type = OBJ_NULL;
    } else if (!strcmp(g, "/dev/urandom") || !strcmp(g, "/dev/random")) {
        o->type = OBJ_RANDOM;
    } else if (!strcmp(g, "/dev/ashmem")) {
        /* ashmem as libcutils (and Chromium's copy of it) opens it: a region of no size yet, named and sized by ioctl. */
        o->type = OBJ_MEM;
        o->shm = true;
        o->shm_prot = 7;
        o->mem = (u8 *)mg_alloc(1);
        o->mem_size = 0;
    } else if (is_synthetic(g)) {
        size_t n = 0;
        o->mem = (u8 *)synthetic(g, &n);
        if (!o->mem) {
            obj_destroy(o);
            mg_set_errno(GE_NOENT);
            return -1;
        }
        o->mem_size = n;
        o->type = OBJ_MEM;
    } else {
        bool is_dir, asked = true;
        int why = 0;
        h = host_of_normal(g);
        /*
         * Whether it is a directory decides the rest, and an ordinary path is asked first. A foreign one is not: asking costs it a
         * call to whoever owns it (on a phone, Windows' broker), and so does opening. It is opened as a file, and the answer to
         * that says when it was a directory after all.
         */
        if (!h) {
            is_dir = leads_to_mount(g);
            if (!is_dir) why = acc ? GE_ACCES : GE_NOENT;
        } else if (mg_path_is_foreign(h) && !(flags & G_O_DIRECTORY)) {
            is_dir = asked = false;
        } else {
            /* (A directory that is only the way to a mount is one with nothing of the host's behind it.) */
            is_dir = mg_path_stat(h, &st, NULL) ? st.is_dir : leads_to_mount(g);
        }
        if (!why && !is_dir && (flags & G_O_DIRECTORY)) why = GE_NOTDIR;
        if (!why && !is_dir) {
            if (acc == 0) hf = MG_O_READ;
            else if (acc == 1) hf = MG_O_WRITE;
            else hf = MG_O_READ | MG_O_WRITE;
            if (flags & G_O_CREAT) hf |= MG_O_CREATE;
            if (flags & G_O_EXCL) hf |= MG_O_EXCL;
            if (flags & G_O_TRUNC) hf |= MG_O_TRUNC;
            if (flags & G_O_APPEND) hf |= MG_O_APPEND;
            o->file = mg_file_open(h, hf, &err);
            if (o->file) o->type = OBJ_FILE;
            else if (!asked && err == MG_E_ISDIR) is_dir = true;
            else why = mg_errno_of(err);
        }
        if (!why && is_dir) {
            if (acc) why = GE_ISDIR;
            else o->type = OBJ_DIRFD;
        }
        free(h);
        if (why) {
            obj_destroy(o);
            mg_set_errno(why);
            return -1;
        }
    }
    fd = fd_new(o);
    if (fd < 0) {
        obj_destroy(o);
        return -1;
    }
    if (flags & G_O_CLOEXEC) g_cloexec[fd] = 1;
    return fd;
}

static int g_openat(int dirfd, const char *path, int flags, int mode)
{
    /* AT_FDCWD is -100. Relative to another directory: join the two paths. */
    if (dirfd != -100 && path && path[0] != '/') {
        gobj *d = fd_get(dirfd);
        char *full;
        int r;
        if (!d || !d->path) {
            if (d) fd_put(d);
            mg_set_errno(GE_BADF);
            return -1;
        }
        full = mg_sprintf("%s/%s", d->path, path);
        fd_put(d);
        r = g_open(full, flags, mode);
        free(full);
        return r;
    }
    return g_open(path, flags, mode);
}

static int g_creat(const char *path, int mode) { return g_open(path, 1 | G_O_CREAT | G_O_TRUNC, mode); }

static int g_close(int fd)
{
    gobj *o;
    io_init();
    if (fd < 0 || fd >= MAX_FDS) {
        mg_set_errno(GE_BADF);
        return -1;
    }
    mg_lock_acquire(&g_io_lock);
    o = g_fds[fd];
    g_fds[fd] = NULL;
    mg_lock_release(&g_io_lock);
    if (!o) {
        mg_set_errno(GE_BADF);
        return -1;
    }
    fd_put(o);
    return 0;
}

/* ------------------------------------------------------------ read, write */

/*
 * The earliest time an armed timer looked at during a wait's check expires (0: none), so that the wait looks again then
 * (wait_ready). Set by timer_catch_up, read and cleared by wait_ready; all of it under the lock.
 */
static u64 g_timer_wake;

/* A timer's expirations up to now counted, and when it next expires noted in g_timer_wake. Called with the lock held. */
static void timer_catch_up(gobj *o)
{
    u64 now;
    if (!o->timer_next) return;
    now = mg_time_monotonic_ns();
    if (now >= o->timer_next) {
        if (o->timer_interval) {
            u64 k = (now - o->timer_next) / o->timer_interval + 1;
            o->timer_ticks += k;
            o->timer_next += k * o->timer_interval;
        } else {
            o->timer_ticks++;
            o->timer_next = 0;
        }
    }
    if (o->timer_next && (!g_timer_wake || o->timer_next < g_timer_wake)) g_timer_wake = o->timer_next;
}

/*
 * Waits, with the lock held, for the state of some descriptor to change.
 * A signal sent to the thread ends the wait, and its handler runs -- with the
 * lock let go of -- before this returns. Returns MG_FD_* (mg_fd.h): woken,
 * the time (0: none) ran out, or a handler ran, and whether that one asked
 * for SA_RESTART.
 */
static int io_wait(u64 ns)
{
    mg_gthread *t = mg_gthread_self();
    i32 handled = t->sig_handled, unrestarted = t->sig_unrestarted;
    bool ok = true;
    t->sig_wait_cond = &g_io_cond;
    if (!mg_signals_ready(t)) {
        if (ns) ok = mg_cond_wait_ns(&g_io_cond, &g_io_lock, ns);
        else mg_cond_wait(&g_io_cond, &g_io_lock);
    }
    t->sig_wait_cond = NULL;
    if (mg_signals_ready(t)) {
        mg_lock_release(&g_io_lock);
        mg_signals_check();
        mg_lock_acquire(&g_io_lock);
    }
    if (t->sig_unrestarted != unrestarted) return MG_FD_INTERRUPTED;
    if (t->sig_handled != handled) return MG_FD_HANDLER_RAN;
    return ok ? MG_FD_WOKEN : MG_FD_TIMED_OUT;
}

void mg_fd_lock(void)
{
    io_init();
    mg_lock_acquire(&g_io_lock);
}
void mg_fd_unlock(void) { mg_lock_release(&g_io_lock); }
void mg_fd_notify_locked(void) { mg_cond_broadcast(&g_io_cond); }
int mg_fd_wait_locked(u64 timeout_ns) { return io_wait(timeout_ns); }

static bool shm_copy(gobj *o, u64 at, void *buf, size_t n, bool write);

static i32 obj_read(gobj *o, void *buf, u32 n, bool positioned, u64 offset)
{
    int err = 0;
    i64 r;
    switch (o->type) {
    case OBJ_FILE:
        r = positioned ? mg_file_pread(o->file, buf, n, offset, &err) : mg_file_read(o->file, buf, n, &err);
        if (r < 0) mg_set_errno(mg_errno_of(err));
        return (i32)r;
    case OBJ_MEM: {
        u64 at = positioned ? offset : o->pos;
        u32 left = at < o->mem_size ? (u32)(o->mem_size - at) : 0;
        if (n > left) n = left;
        if (!o->shm) {
            memcpy(buf, o->mem + at, n);
        } else if (!shm_copy(o, at, buf, n, false)) {
            mg_set_errno(GE_NOMEM);
            return -1;
        }
        if (!positioned) o->pos += n;
        return (i32)n;
    }
    case OBJ_NULL:
        if (o->path && !strcmp(o->path, "/dev/zero")) {
            memset(buf, 0, n);
            return (i32)n;
        }
        return 0;
    case OBJ_RANDOM:
        mg_random(buf, n);
        return (i32)n;
    case OBJ_PIPE_R: {
        pipe_buf *p = o->pipe;
        u32 got = 0;
        mg_lock_acquire(&g_io_lock);
        while (!p->count) {
            if (!p->writers) {
                mg_lock_release(&g_io_lock);
                return 0;
            }
            if (o->flags & G_O_NONBLOCK) {
                mg_lock_release(&g_io_lock);
                mg_set_errno(GE_AGAIN);
                return -1;
            }
            io_wait(0);
        }
        while (got < n && p->count) {
            ((u8 *)buf)[got++] = p->data[p->head];
            p->head = (p->head + 1) % PIPE_CAP;
            p->count--;
        }
        mg_cond_broadcast(&g_io_cond);
        mg_lock_release(&g_io_lock);
        return (i32)got;
    }
    case OBJ_EVENT: {
        u64 v;
        if (n < 8) {
            mg_set_errno(GE_INVAL);
            return -1;
        }
        mg_lock_acquire(&g_io_lock);
        while (!o->counter) {
            if (o->flags & G_O_NONBLOCK) {
                mg_lock_release(&g_io_lock);
                mg_set_errno(GE_AGAIN);
                return -1;
            }
            io_wait(0);
        }
        v = o->semaphore ? 1 : o->counter;
        o->counter -= v;
        mg_cond_broadcast(&g_io_cond);
        mg_lock_release(&g_io_lock);
        memcpy(buf, &v, 8);
        return 8;
    }
    case OBJ_TIMER: {
        /* The expirations since the last read, as a u64; none yet: EAGAIN, or a wait until the next. */
        u64 v;
        if (n < 8) {
            mg_set_errno(GE_INVAL);
            return -1;
        }
        mg_lock_acquire(&g_io_lock);
        for (;;) {
            u64 now;
            timer_catch_up(o);
            if (o->timer_ticks) break;
            if (o->flags & G_O_NONBLOCK) {
                mg_lock_release(&g_io_lock);
                mg_set_errno(GE_AGAIN);
                return -1;
            }
            now = mg_time_monotonic_ns();
            io_wait(o->timer_next ? (o->timer_next > now ? o->timer_next - now : 1) : 0);
        }
        v = o->timer_ticks;
        o->timer_ticks = 0;
        mg_lock_release(&g_io_lock);
        memcpy(buf, &v, 8);
        return 8;
    }
    case OBJ_EXT:
        if (o->ops->read) return o->ops->read(o->ext, buf, n, (o->flags & G_O_NONBLOCK) != 0);
        break;
    default:
        break;
    }
    mg_set_errno(o->type == OBJ_DIRFD ? GE_ISDIR : GE_BADF);
    return -1;
}

static i32 obj_write(gobj *o, const void *buf, u32 n, bool positioned, u64 offset)
{
    int err = 0;
    i64 r;
    switch (o->type) {
    case OBJ_MEM: {
        /* Shared memory only, and inside its size: it grows by ftruncate, as memfd's does, not by writing past its end. */
        u64 at = positioned ? offset : o->pos;
        u32 left;
        if (!o->shm) {
            mg_set_errno(GE_BADF);
            return -1;
        }
        left = at < o->mem_size ? (u32)(o->mem_size - at) : 0;
        if (n > left) n = left;
        if (!shm_copy(o, at, (void *)buf, n, true)) {
            mg_set_errno(GE_NOMEM);
            return -1;
        }
        if (!positioned) o->pos += n;
        return (i32)n;
    }
    case OBJ_FILE:
        r = positioned ? mg_file_pwrite(o->file, buf, n, offset, &err) : mg_file_write(o->file, buf, n, &err);
        if (r < 0) mg_set_errno(mg_errno_of(err));
        return (i32)r;
    case OBJ_NULL:
    case OBJ_RANDOM:
        return (i32)n;
    case OBJ_LOG: {
        /* A line at a time: what the app prints arrives in pieces. */
        u32 i;
        mg_lock_acquire(&g_io_lock);
        for (i = 0; i < n; i++) {
            char c = ((const char *)buf)[i];
            if (c == '\n' || o->line_len == sizeof(o->line) - 1) {
                o->line[o->line_len] = 0;
                mg_lock_release(&g_io_lock);
                mg_log(o->log_level, "%s", o->line);
                mg_lock_acquire(&g_io_lock);
                o->line_len = 0;
                if (c == '\n') continue;
            }
            o->line[o->line_len++] = c;
        }
        mg_lock_release(&g_io_lock);
        return (i32)n;
    }
    case OBJ_PIPE_W: {
        pipe_buf *p = o->pipe;
        u32 put = 0;
        mg_lock_acquire(&g_io_lock);
        while (put < n) {
            if (!p->readers) {
                mg_lock_release(&g_io_lock);
                mg_set_errno(GE_PIPE);
                return put ? (i32)put : -1;
            }
            if (p->count == PIPE_CAP) {
                if (o->flags & G_O_NONBLOCK) {
                    mg_lock_release(&g_io_lock);
                    if (put) return (i32)put;
                    mg_set_errno(GE_AGAIN);
                    return -1;
                }
                mg_cond_broadcast(&g_io_cond);
                io_wait(0);
                continue;
            }
            p->data[(p->head + p->count) % PIPE_CAP] = ((const u8 *)buf)[put++];
            p->count++;
        }
        mg_cond_broadcast(&g_io_cond);
        mg_lock_release(&g_io_lock);
        return (i32)put;
    }
    case OBJ_EVENT: {
        u64 v;
        if (n < 8) {
            mg_set_errno(GE_INVAL);
            return -1;
        }
        memcpy(&v, buf, 8);
        mg_lock_acquire(&g_io_lock);
        o->counter += v;
        mg_cond_broadcast(&g_io_cond);
        mg_lock_release(&g_io_lock);
        return 8;
    }
    case OBJ_EXT:
        if (o->ops->write) return o->ops->write(o->ext, buf, n, (o->flags & G_O_NONBLOCK) != 0);
        break;
    default:
        break;
    }
    mg_set_errno(GE_BADF);
    return -1;
}

#define WITH_FD(fd, o, fail)        \
    gobj *o = fd_get(fd);           \
    if (!o) {                       \
        mg_set_errno(GE_BADF);      \
        return fail;                \
    }

static i32 g_read(int fd, void *buf, u32 n)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_read(o, buf, n, false, 0);
    fd_put(o);
    return r;
}

static i32 g_write(int fd, const void *buf, u32 n)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_write(o, buf, n, false, 0);
    fd_put(o);
    return r;
}

i32 mg_fd_read(int fd, void *buf, u32 n) { return g_read(fd, buf, n); }
i32 mg_fd_write(int fd, const void *buf, u32 n) { return g_write(fd, buf, n); }
int mg_fd_close(int fd) { return g_close(fd); }

i32 mg_fd_pread(int fd, void *buf, u32 n, u64 offset)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_read(o, buf, n, true, offset);
    fd_put(o);
    return r;
}

void *mg_fd_hold(int fd)
{
    /* A file of the host's, or shared memory: a mapping of anything else (a pipe, a made-up file) has nothing to be written back to. */
    gobj *o = fd_get(fd);
    if (o && o->type != OBJ_FILE && !(o->type == OBJ_MEM && o->shm)) {
        fd_put(o);
        o = NULL;
    }
    return o;
}
i32 mg_fd_held_pwrite(void *held, const void *buf, u32 n, u64 offset) { return held ? obj_write((gobj *)held, buf, n, true, offset) : -1; }
void mg_fd_let_go(void *held)
{
    if (held) fd_put((gobj *)held);
}

static i32 g_pread(int fd, void *buf, u32 n, i32 offset) { return mg_fd_pread(fd, buf, n, (u64)(u32)offset); }
static i32 g_pread64(int fd, void *buf, u32 n, i64 offset) { return mg_fd_pread(fd, buf, n, (u64)offset); }

static i32 g_pwrite64(int fd, const void *buf, u32 n, i64 offset)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_write(o, buf, n, true, (u64)offset);
    fd_put(o);
    return r;
}
static i32 g_pwrite(int fd, const void *buf, u32 n, i32 offset) { return g_pwrite64(fd, buf, n, (i64)(u32)offset); }

typedef struct g_iovec {
    u32 base, len;
} g_iovec;

static i32 g_readv(int fd, const g_iovec *iov, int count)
{
    i32 total = 0;
    int i;
    for (i = 0; i < count; i++) {
        i32 r = g_read(fd, (void *)(mg_addr)iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((u32)r < iov[i].len) break;
    }
    return total;
}

static i32 g_writev(int fd, const g_iovec *iov, int count)
{
    i32 total = 0;
    int i;
    for (i = 0; i < count; i++) {
        i32 r = g_write(fd, (const void *)(mg_addr)iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((u32)r < iov[i].len) break;
    }
    return total;
}

i64 mg_fd_seek(int fd, i64 offset, int whence)
{
    i64 r = -1;
    int err = 0;
    WITH_FD(fd, o, -1)
    if (o->type == OBJ_FILE) {
        r = mg_file_seek(o->file, offset, whence, &err);
        if (r < 0) mg_set_errno(mg_errno_of(err));
    } else if (o->type == OBJ_MEM) {
        i64 at = whence == 0 ? offset : whence == 1 ? (i64)o->pos + offset : (i64)o->mem_size + offset;
        if (at < 0) mg_set_errno(GE_INVAL);
        else {
            o->pos = (u64)at;
            r = at;
        }
    } else if (o->type == OBJ_NULL || o->type == OBJ_RANDOM) {
        r = 0;
    } else {
        mg_set_errno(GE_SPIPE);
    }
    fd_put(o);
    return r;
}

static i32 g_lseek(int fd, i32 offset, int whence)
{
    i64 r = mg_fd_seek(fd, offset, whence);
    if (r > 0x7FFFFFFF) {
        mg_set_errno(GE_OVERFLOW);
        return -1;
    }
    return (i32)r;
}
static i64 g_lseek64(int fd, i64 offset, int whence) { return mg_fd_seek(fd, offset, whence); }

/*
 * `n` bytes of a section from `at`, copied into `buf` or out of it (write), or zeroed (write, `buf` NULL): through a view
 * of just those pages, made for the copy and unmapped after it. False when no view could be made.
 */
static bool section_copy(mg_section *s, u64 at, void *buf, size_t n, bool write)
{
    u64 page = at & ~(u64)(MG_PAGE - 1);
    size_t delta = (size_t)(at - page);
    void *base = NULL;
    u8 *v;
    if (!n) return true;
    v = (u8 *)mg_section_map(s, page, delta + n, write, &base);
    if (!v) return false;
    if (!write) memcpy(buf, v + delta, n);
    else if (buf) memcpy(v + delta, buf, n);
    else memset(v + delta, 0, n);
    mg_section_unmap(base, (size_t)(v - (u8 *)base) + delta + n);
    return true;
}

/* Shared memory's bytes for read and write (see the struct). */
static bool shm_copy(gobj *o, u64 at, void *buf, size_t n, bool write)
{
    return !n || (o->section && section_copy(o->section, at, buf, n, write));
}

/*
 * Shared memory's new size: the bytes past the old end are zero. Within what its section holds that is only a number;
 * beyond it a new section is made and what there was copied in, both mapped for the copy only. Views the app mapped
 * before keep the old pages, which is where Linux too leaves a mapping of a region that grows past it (nothing here maps
 * a region before sizing it).
 */
static bool shm_resize(gobj *o, size_t size)
{
    mg_section *s;
    size_t capacity = MG_ALIGN_UP(size, MG_PAGE);
    if (size <= o->capacity && o->section) {
        size_t lo = size < o->mem_size ? size : o->mem_size, hi = size < o->mem_size ? o->mem_size : size;
        if (!section_copy(o->section, lo, NULL, hi - lo, true)) {
            mg_set_errno(GE_NOMEM);
            return false;
        }
        o->mem_size = size;
        return true;
    }
    if (!size) {
        o->mem_size = 0;
        return true;
    }
    s = mg_section_create(capacity);
    if (!s) {
        mg_set_errno(GE_NOMEM);
        return false;
    }
    if (o->section && o->mem_size) {
        size_t keep = o->mem_size < size ? o->mem_size : size;
        void *base = NULL;
        u8 *from = (u8 *)mg_section_map(o->section, 0, keep, false, &base);
        bool copied = from && section_copy(s, 0, from, keep, true);
        if (from) mg_section_unmap(base, (size_t)(from - (u8 *)base) + keep);
        if (!copied) {
            mg_section_close(s);
            mg_set_errno(GE_NOMEM);
            return false;
        }
    }
    if (o->section) mg_section_close(o->section);
    free(o->mem);
    o->mem = NULL;
    o->section = s;
    o->capacity = capacity;
    o->mem_size = size;
    return true;
}

/*
 * A view of a descriptor's shared memory for mmap(MAP_SHARED) (bionic/mg_libc_mem.c): the same pages as every other
 * mapping of it, writable when the region still allows writing. NULL, with errno, when the descriptor is no shared memory
 * (ENODEV: the caller makes a private copy as before) or the range is past its end.
 */
void *mg_fd_file_map(int fd, u64 offset, size_t len, void **base, size_t *mapped)
{
    void *p = NULL;
    mg_stat st;
    WITH_FD(fd, o, NULL)
    if (o->type != OBJ_FILE || (o->flags & G_O_ACCMODE) != 0 || !mg_file_stat(o->file, &st) || offset + len > st.size) {
        mg_set_errno(GE_NODEV);
    } else {
        p = mg_file_map(o->file, offset, len, base, mapped);
        if (!p) mg_set_errno(GE_NODEV);
    }
    fd_put(o);
    return p;
}

void *mg_fd_shm_map(int fd, u64 offset, size_t len, bool *writable, void **base, size_t *mapped)
{
    void *p = NULL;
    WITH_FD(fd, o, NULL)
    if (o->type != OBJ_MEM || !o->shm) {
        mg_set_errno(GE_NODEV);
    } else if (!o->section || offset + len > MG_ALIGN_UP(o->mem_size, MG_PAGE) || (offset & (MG_PAGE - 1))) {
        mg_set_errno(GE_INVAL);
    } else {
        *writable = (o->shm_prot & 2) != 0;
        p = mg_section_map(o->section, offset, MG_ALIGN_UP(len, MG_PAGE), *writable, base);
        if (!p) mg_set_errno(GE_NOMEM);
        else *mapped = MG_ALIGN_UP(len, MG_PAGE) + (size_t)((u8 *)p - (u8 *)*base);
    }
    fd_put(o);
    return p;
}

static int g_ftruncate64(int fd, i64 size)
{
    int err = 0, r = -1;
    WITH_FD(fd, o, -1)
    if (o->type == OBJ_MEM && o->shm) {
        /* Shared memory: a new size, the bytes past the old end zero. */
        if (size < 0 || size > 0x40000000) mg_set_errno(GE_INVAL);
        else if (shm_resize(o, (size_t)size)) r = 0;
        fd_put(o);
        return r;
    }
    if (o->type != OBJ_FILE) mg_set_errno(GE_INVAL);
    else if (!mg_file_truncate(o->file, (u64)size, &err)) mg_set_errno(mg_errno_of(err));
    else r = 0;
    fd_put(o);
    return r;
}
static int g_ftruncate(int fd, i32 size) { return g_ftruncate64(fd, size); }

/* posix_fadvise: advice, taken and not acted on; the descriptor must be one. Returns the error, as POSIX has it. */
static int g_posix_fadvise64(int fd, i64 offset, i64 len, int advice)
{
    gobj *o = fd_get(fd);
    (void)offset;
    (void)len;
    if (!o) return GE_BADF;
    fd_put(o);
    return advice < 0 || advice > 5 ? GE_INVAL : 0;
}
static int g_posix_fadvise(int fd, i32 offset, i32 len, int advice) { return g_posix_fadvise64(fd, offset, len, advice); }

static int g_fsync(int fd)
{
    WITH_FD(fd, o, -1)
    if (o->type == OBJ_FILE) mg_file_sync(o->file);
    fd_put(o);
    return 0;
}

/* ------------------------------------------------------------------- stat */

typedef struct g_stat {
    u64 st_dev;
    u8 pad0[4];
    u32 st_ino32;
    u32 st_mode, st_nlink, st_uid, st_gid;
    u64 st_rdev;
    u8 pad3[4];
    i64 st_size;
    u32 st_blksize;
    u64 st_blocks;
    i32 at_sec, at_nsec, mt_sec, mt_nsec, ct_sec, ct_nsec;
    u64 st_ino;
} g_stat;

#define G_S_IFREG 0100000u
#define G_S_IFDIR 0040000u
#define G_S_IFCHR 0020000u
#define G_S_IFIFO 0010000u

static u32 path_hash(const char *s)
{
    u32 h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h ? h : 1;
}

static void fill_stat(g_stat *out, const mg_stat *st, const char *gpath)
{
    memset(out, 0, sizeof(*out));
    out->st_dev = 0x801;
    out->st_ino = out->st_ino32 = path_hash(gpath);
    out->st_mode = st->is_dir ? (G_S_IFDIR | 0771) : (G_S_IFREG | 0660);
    out->st_nlink = 1;
    out->st_uid = out->st_gid = 10100;
    out->st_size = (i64)st->size;
    out->st_blksize = 4096;
    out->st_blocks = (st->size + 511) / 512;
    out->at_sec = (i32)(st->atime_ns / 1000000000ll);
    out->at_nsec = (i32)(st->atime_ns % 1000000000ll);
    out->mt_sec = (i32)(st->mtime_ns / 1000000000ll);
    out->mt_nsec = (i32)(st->mtime_ns % 1000000000ll);
    out->ct_sec = (i32)(st->ctime_ns / 1000000000ll);
    out->ct_nsec = (i32)(st->ctime_ns % 1000000000ll);
}

static void fill_special(g_stat *out, u32 mode, u64 size, const char *gpath)
{
    mg_stat st;
    memset(&st, 0, sizeof(st));
    st.size = size;
    st.mtime_ns = st.atime_ns = st.ctime_ns = (i64)mg_time_realtime_ns();
    fill_stat(out, &st, gpath);
    out->st_mode = mode;
}

/*
 * ashmem's character device, misc 10:58 here: /dev/ashmem, and every region of shared memory, which is what
 * ASharedMemory_create gives on Android. libcutils' ashmem code (Chromium has a copy) takes a descriptor for a region only
 * when its device number is /dev/ashmem's.
 */
#define G_ASHMEM_RDEV ((10u << 8) | 58u)
static void fill_ashmem(g_stat *out, u64 size, const char *gpath)
{
    fill_special(out, G_S_IFCHR | 0600, size, gpath);
    out->st_rdev = G_ASHMEM_RDEV;
}

static int g_stat_path(const char *path, g_stat *out)
{
    char *g, *h;
    mg_stat st;
    int err = 0;
    size_t n = 0;
    char *text;
    if (!path || !*path) {
        mg_set_errno(GE_NOENT);
        return -1;
    }
    g = normalize(path);
    if (!strncmp(g, "/dev/", 5) && (!strcmp(g, "/dev/null") || !strcmp(g, "/dev/zero") || !strcmp(g, "/dev/urandom") || !strcmp(g, "/dev/random"))) {
        fill_special(out, G_S_IFCHR | 0666, 0, g);
        free(g);
        return 0;
    }
    if (!strcmp(g, "/dev/ashmem")) {
        fill_ashmem(out, 0, g);
        free(g);
        return 0;
    }
    text = is_synthetic(g) ? synthetic(g, &n) : NULL;
    if (text) {
        free(text);
        fill_special(out, G_S_IFREG | 0444, n, g);
        free(g);
        return 0;
    }
    h = host_of_normal(g);
    if (!h || !mg_path_stat(h, &st, &err)) {
        /* A directory that is only the way to a mount ("/data", "/storage/emulated") exists too. */
        if (leads_to_mount(g)) {
            fill_special(out, G_S_IFDIR | 0771, 4096, g);
            free(h);
            free(g);
            return 0;
        }
        mg_set_errno(h ? mg_errno_of(err) : GE_NOENT);
        free(h);
        free(g);
        return -1;
    }
    fill_stat(out, &st, g);
    free(h);
    free(g);
    return 0;
}

static int g_fstat(int fd, g_stat *out)
{
    mg_stat st;
    int r = 0;
    WITH_FD(fd, o, -1)
    switch (o->type) {
    case OBJ_FILE:
        if (!mg_file_stat(o->file, &st)) {
            mg_set_errno(GE_IO);
            r = -1;
        } else {
            fill_stat(out, &st, o->path);
        }
        break;
    case OBJ_MEM:
        if (o->shm) fill_ashmem(out, o->mem_size, o->path);
        else fill_special(out, G_S_IFREG | 0444, o->mem_size, o->path);
        break;
    case OBJ_DIRFD: fill_special(out, G_S_IFDIR | 0771, 4096, o->path); break;
    case OBJ_PIPE_R: case OBJ_PIPE_W: fill_special(out, G_S_IFIFO | 0600, 0, "pipe"); break;
    case OBJ_EXT:
        if (o->ops->file_type) {
            fill_special(out, o->ops->file_type | 0777, 0, "ext");
            break;
        }
        /* fall through */
    default: fill_special(out, G_S_IFCHR | 0620, 0, "chr"); break;
    }
    fd_put(o);
    return r;
}

static int g_fstatat(int dirfd, const char *path, g_stat *out, int flags)
{
    (void)flags;
    if (dirfd != -100 && path && path[0] != '/') {
        gobj *d = fd_get(dirfd);
        char *full;
        int r;
        if (!d || !d->path) {
            if (d) fd_put(d);
            mg_set_errno(GE_BADF);
            return -1;
        }
        full = mg_sprintf("%s/%s", d->path, path);
        fd_put(d);
        r = g_stat_path(full, out);
        free(full);
        return r;
    }
    return g_stat_path(path, out);
}

/* Everything that is there can be read. What cannot be written is what lies in a link the shell marked so. */
static int access_of(const char *path, int mode, int r)
{
    if (r == 0 && (mode & 2)) {
        char *g = normalize(path);
        bool no = in_read_only(g);
        free(g);
        if (no) {
            mg_set_errno(GE_ACCES);
            return -1;
        }
    }
    return r;
}

static int g_access(const char *path, int mode)
{
    g_stat st;
    return access_of(path, mode, g_stat_path(path, &st));
}

static int g_faccessat(int dirfd, const char *path, int mode, int flags)
{
    g_stat st;
    int r = g_fstatat(dirfd, path, &st, flags);
    /* (A path relative to a directory other than the current one is not looked up again: such a one is only said to be there.) */
    return dirfd == -100 || (path && path[0] == '/') ? access_of(path, mode, r) : r;
}

/*
 * statfs / statvfs. Every path is on one volume as far as an app can tell:
 * the one Megler keeps its data on, whose real size and free room are the
 * answer wherever the path is -- in the app's own folders, in the shared
 * storage, in a link to one of the phone's folders (which is on that volume
 * unless the user keeps the library on a memory card; nobody can be asked
 * through a handle). 4 KB blocks. Where the host cannot say: 16 GB, 4 GB of
 * them free, which is what was always said before.
 */
typedef struct g_statfs {
    u32 f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    u32 f_namelen, f_frsize, f_flags, f_spare[4];
} g_statfs;

static void volume_blocks(u64 *blocks, u64 *free_blocks)
{
    u64 total = 0, available = 0;
    *blocks = 4u * 1024 * 1024;
    *free_blocks = 1024 * 1024;
    if (g_space_dir && mg_path_space(g_space_dir, &total, &available) && total) {
        *blocks = total / 4096;
        *free_blocks = available / 4096;
    }
}

static int g_statfs_fn(const char *path, g_statfs *out)
{
    u64 blocks, free_blocks;
    (void)path;
    volume_blocks(&blocks, &free_blocks);
    memset(out, 0, sizeof(*out));
    out->f_type = 0xEF53;
    out->f_bsize = out->f_frsize = 4096;
    out->f_blocks = blocks;
    out->f_bfree = out->f_bavail = free_blocks;
    out->f_files = out->f_ffree = 1000000;
    out->f_namelen = 255;
    return 0;
}
static int g_fstatfs_fn(int fd, g_statfs *out) { (void)fd; return g_statfs_fn("", out); }

typedef struct g_statvfs {
    u32 f_bsize, f_frsize;
    u32 f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
} g_statvfs;

static int g_statvfs_fn(const char *path, g_statvfs *out)
{
    u64 blocks, free_blocks;
    (void)path;
    volume_blocks(&blocks, &free_blocks);
    memset(out, 0, sizeof(*out));
    out->f_bsize = out->f_frsize = 4096;
    /* (32-bit counts: 16 TB in 4 KB blocks before they would not fit.) */
    out->f_blocks = blocks > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)blocks;
    out->f_bfree = out->f_bavail = free_blocks > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)free_blocks;
    out->f_files = out->f_ffree = out->f_favail = 1000000;
    out->f_namemax = 255;
    return 0;
}
static int g_fstatvfs_fn(int fd, g_statvfs *out) { (void)fd; return g_statvfs_fn("", out); }

/* ------------------------------------------------- names: make, remove, move */

/*
 * A name that is not the app's to take away or to give: a mount's own (the
 * shared storage, a link to one of the phone's folders, the APK) and the
 * directories that exist because something is mounted below them. Making one
 * finds it there (EEXIST); removing or renaming one is refused as for a mount
 * point (EBUSY). 0 for any other name.
 */
static int fixed_name(const char *g, int why) { return is_mount_point(g) || leads_to_mount(g) ? why : 0; }

#define PATH_OP(name, fixed, call)                  \
    static int name(const char *path)               \
    {                                               \
        char *g, *h;                                \
        int err = 0, why;                           \
        bool ok;                                    \
        if (!path || !*path) {                      \
            mg_set_errno(GE_NOENT);                 \
            return -1;                              \
        }                                           \
        g = normalize(path);                        \
        why = fixed_name(g, fixed);                 \
        h = why ? NULL : host_of_normal(g);         \
        free(g);                                    \
        if (!h) {                                   \
            mg_set_errno(why ? why : GE_ACCES);     \
            return -1;                              \
        }                                           \
        ok = call;                                  \
        free(h);                                    \
        if (!ok) {                                  \
            mg_set_errno(mg_errno_of(err));         \
            return -1;                              \
        }                                           \
        return 0;                                   \
    }

PATH_OP(g_unlink, GE_BUSY, mg_path_remove(h, &err))
PATH_OP(g_rmdir, GE_BUSY, mg_path_rmdir(h, &err))
PATH_OP(mkdir_host, GE_EXIST, mg_path_mkdir(h, &err))

static int g_mkdir(const char *path, int mode) { (void)mode; return mkdir_host(path); }
static int g_mkdirat(int dirfd, const char *path, int mode) { (void)dirfd; (void)mode; return mkdir_host(path); }
static int g_unlinkat(int dirfd, const char *path, int flags) { (void)dirfd; return (flags & 0x200) ? g_rmdir(path) : g_unlink(path); }

static int g_remove(const char *path)
{
    g_stat st;
    if (g_stat_path(path, &st) != 0) return -1;
    return (st.st_mode & G_S_IFDIR) ? g_rmdir(path) : g_unlink(path);
}

static int g_rename(const char *from, const char *to)
{
    char *ga, *gb, *a, *b;
    int err = 0, why;
    bool ok;
    if (!from || !*from || !to || !*to) {
        mg_set_errno(GE_NOENT);
        return -1;
    }
    ga = normalize(from);
    gb = normalize(to);
    why = fixed_name(ga, GE_BUSY);
    if (!why) why = fixed_name(gb, GE_BUSY);
    a = why ? NULL : host_of_normal(ga);
    b = why ? NULL : host_of_normal(gb);
    free(ga);
    free(gb);
    /* (Between a link and anything else the host layer says EXDEV, as between two file systems: the app copies and removes.) */
    ok = a && b && mg_path_rename(a, b, &err);
    if (!ok) mg_set_errno(why ? why : a && b ? mg_errno_of(err) : GE_ACCES);
    free(a);
    free(b);
    return ok ? 0 : -1;
}
static int g_renameat(int d1, const char *from, int d2, const char *to) { (void)d1; (void)d2; return g_rename(from, to); }

static int g_truncate(const char *path, i32 size)
{
    int fd = g_open(path, 1, 0), r;
    if (fd < 0) return -1;
    r = g_ftruncate(fd, size);
    g_close(fd);
    return r;
}

static int g_chdir(const char *path)
{
    g_stat st;
    char *g;
    if (g_stat_path(path, &st) != 0) return -1;
    if (!(st.st_mode & G_S_IFDIR)) {
        mg_set_errno(GE_NOTDIR);
        return -1;
    }
    g = normalize(path);
    mg_strlcpy(g_cwd, g, sizeof(g_cwd));
    free(g);
    return 0;
}

static char *g_getcwd(char *buf, size_t n)
{
    if (!buf) return mg_g_strdup(g_cwd);
    if (strlen(g_cwd) + 1 > n) {
        mg_set_errno(GE_RANGE);
        return NULL;
    }
    strcpy(buf, g_cwd);
    return buf;
}

static char *g_realpath(const char *path, char *out)
{
    g_stat st;
    char *g;
    if (!path) {
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    if (g_stat_path(path, &st) != 0) return NULL;
    g = normalize(path);
    if (!out) out = (char *)mg_g_malloc(4096);
    if (out) mg_strlcpy(out, g, 4096);
    free(g);
    return out;
}

static i32 g_readlink(const char *path, char *buf, size_t n)
{
    /* The one link code reads: its own executable. */
    if (path && !strcmp(path, "/proc/self/exe")) {
        const char *exe = "/system/bin/app_process32";
        size_t len = strlen(exe);
        if (len > n) len = n;
        memcpy(buf, exe, len);
        return (i32)len;
    }
    mg_set_errno(GE_INVAL);
    return -1;
}

static int g_ok_path(const char *path, int a) { (void)path; (void)a; return 0; }
static int g_ok_fd(int fd, int a) { (void)fd; (void)a; return 0; }
static int g_chown(const char *path, int u, int g) { (void)path; (void)u; (void)g; return 0; }
static int g_utime(const char *path, const void *times) { (void)path; (void)times; return 0; }
/* (The times of a file are not set here, by any of the utime calls: an app is told they were.) */
static int g_futimes(int fd, const void *times) { (void)fd; (void)times; return 0; }
static int g_fail_link(const char *a, const char *b)
{
    (void)a; (void)b;
    mg_set_errno(GE_PERM);
    return -1;
}
static int g_umask(int mask) { (void)mask; return 022; }
/* The checked forms a library built with _FORTIFY_SOURCE calls: the same, the buffer's size being the caller's business. */
static int g_umask_chk(int mask) { return g_umask(mask); }
static i32 g_pread64_chk(int fd, void *buf, u32 n, i64 offset, u32 buf_size)
{
    (void)buf_size;
    return mg_fd_pread(fd, buf, n, (u64)offset);
}
static i32 g_pread_chk(int fd, void *buf, u32 n, i32 offset, u32 buf_size)
{
    (void)buf_size;
    return mg_fd_pread(fd, buf, n, (u64)(u32)offset);
}
static i32 g_pwrite64_chk(int fd, const void *buf, u32 n, i64 offset, u32 buf_size)
{
    (void)buf_size;
    return g_pwrite64(fd, buf, n, offset);
}
static i32 g_readlink_chk(const char *path, char *buf, size_t n, size_t buf_size)
{
    (void)buf_size;
    return g_readlink(path, buf, n);
}

/*
 * sendfile: bytes from one descriptor to another. There is no kernel here to
 * move them without a copy, so they are read and written; with an offset the
 * source's own position is left where it was, as the call promises.
 */
static i32 send_file(int out_fd, int in_fd, i64 *offset, u32 count)
{
    char buf[4096];
    u32 done = 0;
    while (done < count) {
        u32 want = count - done < sizeof(buf) ? count - done : (u32)sizeof(buf);
        i32 got = offset ? mg_fd_pread(in_fd, buf, want, (u64)*offset) : mg_fd_read(in_fd, buf, want), put;
        if (got < 0) return done ? (i32)done : -1;
        if (got == 0) break;
        put = mg_fd_write(out_fd, buf, (u32)got);
        if (put < 0) return done ? (i32)done : -1;
        if (offset) *offset += put;
        else if (put < got) mg_fd_seek(in_fd, (i64)put - got, 1);    /* what was read and not taken is still to be read */
        done += (u32)put;
        if (put < got) break;
    }
    return (i32)done;
}
static i32 g_sendfile64(int out_fd, int in_fd, i64 *offset, u32 count) { return send_file(out_fd, in_fd, offset, count); }
static i32 g_sendfile(int out_fd, int in_fd, i32 *offset, u32 count)
{
    i64 at = offset ? *offset : 0;
    i32 n = send_file(out_fd, in_fd, offset ? &at : NULL, count);
    if (offset) *offset = (i32)at;
    return n;
}

/* posix_fallocate: the file is made at least that long (the host keeps no account of blocks). It answers with an error number, not through errno. */
static int g_posix_fallocate64(int fd, i64 offset, i64 len)
{
    i64 at, end;
    int saved = *mg_errno_ptr(), err;
    if (offset < 0 || len <= 0) return GE_INVAL;
    at = mg_fd_seek(fd, 0, 1);
    end = at < 0 ? -1 : mg_fd_seek(fd, 0, 2);
    if (end < 0) {
        err = *mg_errno_ptr();
        *mg_errno_ptr() = saved;
        return err ? err : GE_BADF;
    }
    mg_fd_seek(fd, at, 0);
    if (end >= offset + len) return 0;
    if (g_ftruncate64(fd, offset + len) == 0) return 0;
    err = *mg_errno_ptr();
    *mg_errno_ptr() = saved;
    return err ? err : GE_IO;
}
/* (The 32-bit offsets' form, which bionic has beside it.) */
static int g_posix_fallocate(int fd, i32 offset, i32 len) { return g_posix_fallocate64(fd, offset, len); }

/* What this file system does not have: pipes spliced in the kernel, FIFOs and device files, extended attributes, watches. */
static int g_io_nosys(void) { mg_set_errno(GE_NOSYS); return -1; }
static int g_io_perm(void) { mg_set_errno(GE_PERM); return -1; }
static int g_io_notsup(void) { mg_set_errno(GE_OPNOTSUPP); return -1; }
/* And no descriptor is a terminal. */
static int g_io_notty(void) { mg_set_errno(GE_NOTTY); return -1; }

/* pathconf: the limits of the one kind of file system there is. Those it has no figure for answer -1 and leave errno alone, which is how "no limit" is said. */
static i32 g_pathconf(const char *path, int name)
{
    (void)path;
    switch (name) {
    case 0: return 64;                                     /* _PC_FILESIZEBITS */
    case 1: return 1;                                      /* _PC_LINK_MAX: there are no hard links */
    case 2: case 3: return 255;                            /* _PC_MAX_CANON, _PC_MAX_INPUT */
    case 4: return 255;                                    /* _PC_NAME_MAX */
    case 5: return 4096;                                   /* _PC_PATH_MAX */
    case 6: return 4096;                                   /* _PC_PIPE_BUF */
    case 8: case 9: case 11: case 12: return 4096;         /* _PC_ALLOC_SIZE_MIN, _PC_REC_INCR_XFER_SIZE, _PC_REC_MIN_XFER_SIZE, _PC_REC_XFER_ALIGN */
    case 14: return 1;                                     /* _PC_CHOWN_RESTRICTED */
    case 15: return 1;                                     /* _PC_NO_TRUNC */
    case 7: case 10: case 13: case 16: case 17: case 18: case 19: return -1;
    default: mg_set_errno(GE_INVAL); return -1;
    }
}
static i32 g_fpathconf(int fd, int name) { (void)fd; return g_pathconf("", name); }

/* ------------------------------------------------------------ directories */

typedef struct g_dirent {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8 d_type;
    char d_name[256];
} g_dirent;

typedef struct g_dir {
    char **names;
    u8 *types;
    u32 count, cap, at;
    int fd;
    g_dirent ent;
} g_dir;

static void dir_add(const char *name, bool is_dir, void *user)
{
    g_dir *d = (g_dir *)user;
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->names = (char **)mg_realloc(d->names, sizeof(char *) * d->cap);
        d->types = (u8 *)mg_realloc(d->types, d->cap);
    }
    d->names[d->count] = mg_strdup(name);
    d->types[d->count] = is_dir ? 4 : 8;     /* DT_DIR, DT_REG */
    d->count++;
}

/*
 * One name directly below the directory `g` that is there because of a mount
 * or an alias: `below` is that mount's (or alias's) path and `host` what the
 * mount stands for (NULL for an alias, and where it is known to be a
 * directory). The listing shows it once: a folder of the host by the same
 * name, which the mount hides, is not a second entry.
 */
static void dir_add_mounted(g_dir *d, const char *g, const char *below, const char *host, bool any_case)
{
    size_t glen = strlen(g), from = glen == 1 ? 1 : glen + 1, n = 0;
    bool is_dir = true;
    char name[256];
    u32 i;
    while (below[from + n] && below[from + n] != '/' && n < sizeof(name) - 1) n++;
    if (!n) return;
    memcpy(name, below + from, n);
    name[n] = 0;
    /* The mount itself and not the way to one: it is what its host path is, and is not there when that is not. */
    if (!below[from + n] && host) {
        mg_stat st;
        if (!mg_path_stat(host, &st, NULL)) return;
        is_dir = st.is_dir;
    }
    for (i = 0; i < d->count; i++) {
        if (strlen(d->names[i]) == n && ascii_same(d->names[i], name, n, any_case)) {
            d->types[i] = is_dir ? 4 : 8;
            return;
        }
    }
    dir_add(name, is_dir, d);
}

static g_dir *g_opendir(const char *path)
{
    char *g, *h;
    g_dir *d;
    int err = 0, k;
    size_t glen, i;
    bool listed, any_case, has_host;
    if (!path || !*path) {
        mg_set_errno(GE_NOENT);
        return NULL;
    }
    g = normalize(path);
    h = host_of_normal(g);
    has_host = h != NULL;
    d = (g_dir *)mg_alloc(sizeof(*d));
    d->fd = -1;
    dir_add(".", true, d);
    dir_add("..", true, d);
    listed = h && mg_path_list(h, dir_add, d, &err);
    free(h);
    /* A directory that is only the way to a mount has no folder of the host's to list, and is one all the same. */
    if (!listed && !leads_to_mount(g)) {
        u32 n;
        for (n = 0; n < d->count; n++) free(d->names[n]);
        free(d->names);
        free(d->types);
        free(d);
        free(g);
        mg_set_errno(has_host ? mg_errno_of(err) : GE_NOENT);
        return NULL;
    }
    /* What is mounted below it is in it, whether or not the host's folder has an entry of that name. */
    glen = strlen(g);
    any_case = !strncmp(g, EXTERNAL, EXTERNAL_LEN) && (!g[EXTERNAL_LEN] || g[EXTERNAL_LEN] == '/');
    for (k = 0; k < g_nmounts; k++) {
        const mount *m = &g_mounts[k];
        if (m->glen <= glen || (glen > 1 && m->guest[glen] != '/') || !mount_same(m, g, glen)) continue;
        /* (A link is a folder, and asking whether it is costs a call to whoever owns it.) */
        dir_add_mounted(d, g, m->guest, m->link ? NULL : m->host, any_case);
    }
    for (i = 0; i < MG_COUNT(k_aliases); i++) {
        const char *a = k_aliases[i].path;
        if (!k_aliases[i].listed || strlen(a) <= glen || (glen > 1 && a[glen] != '/') || strncmp(a, g, glen)) continue;
        dir_add_mounted(d, g, a, NULL, false);
    }
    free(g);
    return d;
}

static g_dir *g_fdopendir(int fd)
{
    gobj *o = fd_get(fd);
    g_dir *d = NULL;
    if (o && o->path) d = g_opendir(o->path);
    else mg_set_errno(GE_BADF);
    if (o) fd_put(o);
    if (d) d->fd = fd;
    return d;
}

static g_dirent *g_readdir(g_dir *d)
{
    if (!d || d->at >= d->count) return NULL;
    memset(&d->ent, 0, sizeof(d->ent));
    d->ent.d_ino = path_hash(d->names[d->at]);
    d->ent.d_off = d->at + 1;
    d->ent.d_reclen = sizeof(g_dirent);
    d->ent.d_type = d->types[d->at];
    mg_strlcpy(d->ent.d_name, d->names[d->at], sizeof(d->ent.d_name));
    d->at++;
    return &d->ent;
}

static int g_readdir_r(g_dir *d, g_dirent *entry, g_dirent **result)
{
    g_dirent *e = g_readdir(d);
    if (!e) {
        *result = NULL;
        return 0;
    }
    *entry = *e;
    *result = entry;
    return 0;
}

static int g_closedir(g_dir *d)
{
    u32 i;
    if (!d) return -1;
    if (d->fd >= 0) g_close(d->fd);
    for (i = 0; i < d->count; i++) free(d->names[i]);
    free(d->names);
    free(d->types);
    free(d);
    return 0;
}

static void g_rewinddir(g_dir *d) { if (d) d->at = 0; }
static long g_telldir(g_dir *d) { return d ? (long)d->at : -1; }
static void g_seekdir(g_dir *d, long at) { if (d && at >= 0 && (u32)at <= d->count) d->at = (u32)at; }
static int g_dirfd(g_dir *d)
{
    (void)d;
    mg_set_errno(GE_OPNOTSUPP);
    return -1;
}

static int g_scandir(const char *path, g_dirent ***out, mg_addr filter, mg_addr compare)
{
    g_dir *d = g_opendir(path);
    g_dirent **list = NULL, *e;
    int n = 0, cap = 0, i, j;
    if (!d) return -1;
    while ((e = g_readdir(d)) != NULL) {
        g_dirent *copy;
        if (filter && !MG_GFN(int (*)(const g_dirent *), filter)(e)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            list = (g_dirent **)mg_g_realloc(list, sizeof(g_dirent *) * (size_t)cap);
        }
        copy = (g_dirent *)mg_g_malloc(sizeof(g_dirent));
        *copy = *e;
        list[n++] = copy;
    }
    g_closedir(d);
    if (compare) {
        for (i = 1; i < n; i++) {
            g_dirent *key = list[i];
            for (j = i - 1; j >= 0 && MG_GFN(int (*)(const g_dirent **, const g_dirent **), compare)((const g_dirent **)&list[j], (const g_dirent **)&key) > 0; j--)
                list[j + 1] = list[j];
            list[j + 1] = key;
        }
    }
    *out = list;
    return n;
}

static int g_alphasort(const g_dirent **a, const g_dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }

/* ----------------------------------------------------- pipes and eventfds */

static int g_pipe2(int fds[2], int flags)
{
    pipe_buf *p = (pipe_buf *)mg_alloc(sizeof(*p));
    gobj *r = (gobj *)mg_alloc(sizeof(*r)), *w = (gobj *)mg_alloc(sizeof(*w));
    p->readers = p->writers = 1;
    r->type = OBJ_PIPE_R;
    w->type = OBJ_PIPE_W;
    r->refs = w->refs = 1;
    r->pipe = w->pipe = p;
    r->flags = flags & G_O_NONBLOCK;
    w->flags = (flags & G_O_NONBLOCK) | 1;
    fds[0] = fd_new(r);
    fds[1] = fd_new(w);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) g_close(fds[0]);
        else free(r);
        if (fds[1] >= 0) g_close(fds[1]);
        else free(w);
        return -1;
    }
    return 0;
}

static int g_pipe(int fds[2]) { return g_pipe2(fds, 0); }

int mg_fd_shm_create(const char *name, u64 size)
{
    gobj *o;
    int fd;
    if (size > 0x40000000u) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    o = (gobj *)mg_alloc(sizeof(*o));
    o->type = OBJ_MEM;
    o->refs = 1;
    o->shm = true;
    o->shm_prot = 7;                                 /* PROT_READ | PROT_WRITE | PROT_EXEC */
    /* (Its pages are a section's, made for its size: shm_resize. An app asking for too much is told so.) */
    if (!shm_resize(o, (size_t)size)) {
        free(o);
        return -1;
    }
    o->path = mg_sprintf("/dev/ashmem/%s", name ? name : "");
    o->flags = 2;                                    /* O_RDWR */
    fd = fd_new(o);
    if (fd < 0) obj_destroy(o);
    return fd;
}

int mg_fd_shm_set_prot(int fd, int prot)
{
    gobj *o = fd_get(fd);
    int r = -1;
    if (!o) mg_set_errno(GE_BADF);
    else if (o->type != OBJ_MEM || !o->shm) mg_set_errno(GE_INVAL);
    else if (prot & ~o->shm_prot & 7) mg_set_errno(GE_INVAL);          /* protection is only ever taken away, as ashmem's */
    else {
        o->shm_prot = prot & 7;
        r = 0;
    }
    if (o) fd_put(o);
    return r;
}

int mg_fd_shm_prot(int fd)
{
    gobj *o = fd_get(fd);
    int r = o && o->type == OBJ_MEM && o->shm ? o->shm_prot : -1;
    if (o) fd_put(o);
    return r;
}

i64 mg_fd_shm_size(int fd)
{
    gobj *o = fd_get(fd);
    i64 r = o && o->type == OBJ_MEM && o->shm ? (i64)o->mem_size : -1;
    if (o) fd_put(o);
    return r;
}

/* memfd_create (API 30, and the system call before it): shared memory with a name. */
static int g_memfd_create(const char *name, unsigned flags)
{
    int fd = mg_fd_shm_create(name, 0);
    if (fd >= 0 && (flags & 1)) g_cloexec[fd] = 1;   /* MFD_CLOEXEC */
    return fd;
}

/*
 * timerfd (Chromium's message pump keeps its delayed work in one; Linux 2.6.25, every Android): a descriptor that becomes
 * readable when its time comes, and reads as the count of expirations since the last read. The clocks it takes are
 * CLOCK_REALTIME, CLOCK_MONOTONIC and CLOCK_BOOTTIME (and their _ALARM kinds, which wake nothing here); every time is kept
 * on the monotonic clock, an absolute realtime one turned into it when set (a change of the wall clock afterwards does not
 * move it: TFD_TIMER_CANCEL_ON_SET is not done). Poll, select, epoll and the looper look again when an armed timer they
 * watch expires.
 */
typedef struct g_itimerspec {
    i32 interval_sec, interval_nsec, value_sec, value_nsec;
} g_itimerspec;

static int g_timerfd_create(int clock, int flags)
{
    gobj *o;
    int fd;
    if ((clock != 0 && clock != 1 && clock != 7 && clock != 8 && clock != 9) || (flags & ~(G_O_NONBLOCK | 0x80000))) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    o = (gobj *)mg_alloc(sizeof(*o));
    o->type = OBJ_TIMER;
    o->refs = 1;
    o->timer_clock = clock;
    o->flags = 2 | (flags & G_O_NONBLOCK);
    fd = fd_new(o);
    if (fd < 0) free(o);
    else if (flags & 0x80000) g_cloexec[fd] = 1;     /* TFD_CLOEXEC */
    return fd;
}

/* What is left of an armed timer and its interval, as timerfd_gettime gives them. Called with the lock held. */
static void timer_left(gobj *o, g_itimerspec *out)
{
    u64 now = mg_time_monotonic_ns(), left = o->timer_next ? (o->timer_next > now ? o->timer_next - now : 1) : 0;
    out->interval_sec = (i32)(o->timer_interval / 1000000000ull);
    out->interval_nsec = (i32)(o->timer_interval % 1000000000ull);
    out->value_sec = (i32)(left / 1000000000ull);
    out->value_nsec = (i32)(left % 1000000000ull);
}

/* The timer a descriptor is, held; NULL with EBADF or EINVAL. */
static gobj *timer_of(int fd)
{
    gobj *o = fd_get(fd);
    if (!o) {
        mg_set_errno(GE_BADF);
        return NULL;
    }
    if (o->type != OBJ_TIMER) {
        fd_put(o);
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    return o;
}

static int g_timerfd_settime(int fd, int flags, const g_itimerspec *set, g_itimerspec *old)
{
    gobj *o;
    u64 value, now;
    if (!set || set->value_sec < 0 || set->value_nsec < 0 || set->value_nsec >= 1000000000 || set->interval_sec < 0 || set->interval_nsec < 0 ||
        set->interval_nsec >= 1000000000) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    o = timer_of(fd);
    if (!o) return -1;
    mg_lock_acquire(&g_io_lock);
    timer_catch_up(o);
    if (old) timer_left(o, old);
    value = (u64)set->value_sec * 1000000000ull + (u64)set->value_nsec;
    o->timer_interval = (u64)set->interval_sec * 1000000000ull + (u64)set->interval_nsec;
    o->timer_ticks = 0;
    now = mg_time_monotonic_ns();
    if (!value) {
        o->timer_next = 0;                           /* disarmed */
    } else if (flags & 1) {
        /* TFD_TIMER_ABSTIME: a time on the timer's own clock; one already past expires at once. */
        u64 clock_now = o->timer_clock == 0 || o->timer_clock == 8 ? mg_time_realtime_ns() : now;
        o->timer_next = value > clock_now ? now + (value - clock_now) : now;
    } else {
        o->timer_next = now + value;
    }
    /* Whoever waits on it works out again when to look. */
    mg_cond_broadcast(&g_io_cond);
    mg_lock_release(&g_io_lock);
    fd_put(o);
    return 0;
}

static int g_timerfd_gettime(int fd, g_itimerspec *cur)
{
    gobj *o = timer_of(fd);
    if (!o) return -1;
    mg_lock_acquire(&g_io_lock);
    timer_catch_up(o);
    if (cur) timer_left(o, cur);
    mg_lock_release(&g_io_lock);
    fd_put(o);
    return 0;
}

int mg_fd_eventfd(unsigned initial, int flags)
{
    gobj *o = (gobj *)mg_alloc(sizeof(*o));
    int fd;
    o->type = OBJ_EVENT;
    o->refs = 1;
    o->counter = initial;
    o->semaphore = (flags & 1) != 0;                 /* EFD_SEMAPHORE */
    o->flags = 2 | (flags & G_O_NONBLOCK);
    fd = fd_new(o);
    if (fd < 0) free(o);
    return fd;
}

static int g_dup_at(int fd, int at_least, bool cloexec)
{
    gobj *o = fd_get(fd);
    int nfd;
    if (!o) {
        mg_set_errno(GE_BADF);
        return -1;
    }
    mg_lock_acquire(&g_io_lock);
    nfd = fd_alloc_locked(o, at_least);
    if (nfd >= 0) g_cloexec[nfd] = cloexec;
    mg_lock_release(&g_io_lock);
    if (nfd < 0) {
        fd_put(o);
        mg_set_errno(GE_MFILE);
    }
    /* The reference fd_get took is the new descriptor's. */
    return nfd;
}

static int g_dup(int fd) { return g_dup_at(fd, 0, false); }

static int g_dup3(int fd, int nfd, int flags)
{
    gobj *o, *old;
    if (nfd < 0 || nfd >= MAX_FDS) {
        mg_set_errno(GE_BADF);
        return -1;
    }
    o = fd_get(fd);
    if (!o) {
        mg_set_errno(GE_BADF);
        return -1;
    }
    if (fd == nfd) {
        fd_put(o);
        return nfd;
    }
    mg_lock_acquire(&g_io_lock);
    old = g_fds[nfd];
    g_fds[nfd] = o;
    g_cloexec[nfd] = (flags & G_O_CLOEXEC) != 0;
    mg_lock_release(&g_io_lock);
    if (old) fd_put(old);
    return nfd;
}
static int g_dup2(int fd, int nfd) { return g_dup3(fd, nfd, 0); }

static int g_fcntl(int fd, int cmd, int arg)
{
    int r = 0;
    WITH_FD(fd, o, -1)
    switch (cmd) {
    case 0: /* F_DUPFD */
    case 1030: /* F_DUPFD_CLOEXEC */
        fd_put(o);
        return g_dup_at(fd, arg, cmd == 1030);
    case 1: r = g_cloexec[fd]; break;              /* F_GETFD */
    case 2: g_cloexec[fd] = (u8)(arg & 1); break;  /* F_SETFD */
    case 3: r = o->flags; break;                   /* F_GETFL */
    case 1033: /* F_ADD_SEALS: shared memory; a seal against writing takes PROT_WRITE away from later mappings */
    case 1034: /* F_GET_SEALS */
        if (o->type != OBJ_MEM || !o->shm) {
            mg_set_errno(GE_INVAL);
            r = -1;
        } else if (cmd == 1033) {
            o->shm_seals |= arg & 0x1F;
            if (arg & (0x08 | 0x10)) o->shm_prot &= ~2;  /* F_SEAL_WRITE, F_SEAL_FUTURE_WRITE */
        } else {
            r = o->shm_seals;
        }
        break;
    case 4: /* F_SETFL: only these two can change */
        o->flags = (o->flags & ~(G_O_NONBLOCK | G_O_APPEND)) | (arg & (G_O_NONBLOCK | G_O_APPEND));
        if (o->type == OBJ_EXT && o->ops->set_nonblock) o->ops->set_nonblock(o->ext, (arg & G_O_NONBLOCK) != 0);
        break;
    case 5:  /* F_GETLK */
    case 12: /* F_GETLK64 */
        /*
         * The lock that would stop the one described: none. Every lock here is this process's, and a process's own locks
         * never stop it (POSIX), so the answer is l_type F_UNLCK, the struct's first field in both forms. SQLite in WAL
         * mode asks this of its -shm file before it uses it; the struct came back as it was asked, F_WRLCK, which says
         * another process holds the lock: SQLITE_BUSY at every try, then SQLITE_PROTOCOL ("locking protocol").
         */
        if (!arg) {
            mg_set_errno(GE_FAULT);
            r = -1;
        } else {
            mg_wr16((u8 *)(mg_addr)(u32)arg, 2);
        }
        break;
    default: break;                                /* setting locks, and the rest: granted */
    }
    fd_put(o);
    return r;
}

/*
 * ashmem's ioctls on shared memory (linux/ashmem.h), for code that drives the region itself, as libcutils does: its name,
 * its size (set while nothing maps it), its protection (only ever narrowed), and pinning, which is always granted with
 * nothing purged, since nothing is ever taken back here.
 */
static int shm_ioctl(gobj *o, unsigned request, void *arg)
{
    switch (request) {
    case 0x41007701: {                             /* ASHMEM_SET_NAME */
        char name[256];
        snprintf(name, sizeof(name), "%.*s", 255, arg ? (const char *)arg : "");
        free(o->path);
        o->path = mg_sprintf("/dev/ashmem/%s", name);
        return 0;
    }
    case 0x81007702:                               /* ASHMEM_GET_NAME */
        snprintf((char *)arg, 256, "%s", o->path && !strncmp(o->path, "/dev/ashmem/", 12) ? o->path + 12 : "");
        return 0;
    case 0x40047703: {                             /* ASHMEM_SET_SIZE */
        size_t size = (size_t)(mg_addr)arg;
        if (size > 0x40000000u) break;
        return shm_resize(o, size) ? 0 : -1;
    }
    case 0x7704: return (int)o->mem_size;          /* ASHMEM_GET_SIZE */
    case 0x40047705:                               /* ASHMEM_SET_PROT_MASK */
        if ((u32)(mg_addr)arg & ~(u32)o->shm_prot & 7u) break;
        o->shm_prot = (int)((u32)(mg_addr)arg & 7u);
        return 0;
    case 0x7706: return o->shm_prot;               /* ASHMEM_GET_PROT_MASK */
    case 0x40087707: return 0;                     /* ASHMEM_PIN: ASHMEM_NOT_PURGED */
    case 0x40087708: return 0;                     /* ASHMEM_UNPIN */
    case 0x7709: return 1;                         /* ASHMEM_GET_PIN_STATUS: ASHMEM_IS_PINNED */
    case 0x770A: return 0;                         /* ASHMEM_PURGE_ALL_CACHES */
    default:
        mg_set_errno(GE_NOTTY);
        return -1;
    }
    mg_set_errno(GE_INVAL);
    return -1;
}

static int g_ioctl(int fd, unsigned request, void *arg)
{
    int r = -1;
    WITH_FD(fd, o, -1)
    if (request == 0x541B) {                       /* FIONREAD */
        int n = 0;
        mg_lock_acquire(&g_io_lock);
        if (o->type == OBJ_PIPE_R) n = (int)o->pipe->count;
        else if (o->type == OBJ_MEM) n = (int)(o->mem_size > o->pos ? o->mem_size - o->pos : 0);
        mg_lock_release(&g_io_lock);
        if (o->type == OBJ_EXT && o->ops->readable) n = o->ops->readable(o->ext);
        *(int *)arg = n;
        r = 0;
    } else if (request == 0x5421) {                /* FIONBIO */
        if (*(int *)arg) o->flags |= G_O_NONBLOCK;
        else o->flags &= ~G_O_NONBLOCK;
        if (o->type == OBJ_EXT && o->ops->set_nonblock) o->ops->set_nonblock(o->ext, *(int *)arg != 0);
        r = 0;
    } else if (o->type == OBJ_MEM && o->shm && (request & 0xFF00) == 0x7700) {
        r = shm_ioctl(o, request, arg);
    } else if (o->type == OBJ_EXT && o->ops->ioctl) {
        r = o->ops->ioctl(o->ext, request, arg);
    } else {
        mg_set_errno(GE_NOTTY);
    }
    fd_put(o);
    return r;
}

static int g_isatty(int fd) { (void)fd; mg_set_errno(GE_NOTTY); return 0; }

/* ------------------------------------------------------------------- poll */

#define G_POLLIN 1
#define G_POLLPRI 2
#define G_POLLOUT 4
#define G_POLLERR 8
#define G_POLLHUP 0x10
#define G_POLLNVAL 0x20

/* What of `events` is true of the object now. Called with the lock held. */
static int ready_locked(gobj *o, int events)
{
    static int epoll_depth;            /* (an epoll set in an epoll set: looked into, not for ever) */
    int r = 0, i;
    switch (o->type) {
    case OBJ_PIPE_R:
        if (o->pipe->count) r |= G_POLLIN;
        if (!o->pipe->writers) r |= G_POLLHUP;
        break;
    case OBJ_PIPE_W:
        if (o->pipe->count < PIPE_CAP) r |= G_POLLOUT;
        if (!o->pipe->readers) r |= G_POLLERR;
        break;
    case OBJ_EVENT:
        if (o->counter) r |= G_POLLIN;
        r |= G_POLLOUT;
        break;
    case OBJ_TIMER:
        timer_catch_up(o);
        if (o->timer_ticks) r |= G_POLLIN;
        break;
    case OBJ_EXT:
        if (o->ops->poll) r = o->ops->poll(o->ext, events);
        break;
    case OBJ_EPOLL:
        /* Readable when something in its set is ready, as Linux has it. */
        if (epoll_depth < 4) {
            epoll_depth++;
            for (i = 0; i < o->nitems && !r; i++) {
                int fd = o->items[i].fd;
                gobj *x = fd >= 0 && fd < MAX_FDS ? g_fds[fd] : NULL;
                if (x && x != o && !o->items[i].disarmed && ready_locked(x, (int)(o->items[i].events & 0x23FFu))) r = G_POLLIN;
            }
            epoll_depth--;
        }
        break;
    default:
        r = G_POLLIN | G_POLLOUT;
        break;
    }
    return r & (events | G_POLLERR | G_POLLHUP);
}

/*
 * The one loop every wait for descriptors goes round: `check` says, with the
 * lock held, how many of the descriptors it watches are ready (and fills in
 * what it reports of them); between looks the thread waits on the condition
 * every change is announced on -- by this file's pipes and eventfds, and by
 * the objects of other modules that announce theirs (mg_fd_ops.announces):
 * sockets, whose host tells of every change on a thread of its own
 * (bionic/mg_net.c). An object that does not announce is looked at again
 * every 5 ms. So one wait covers sockets, pipes, eventfds and files together,
 * whatever the host's own sockets can be waited on with.
 *
 * `interruptible`: a signal handled meanwhile ends the wait with EINTR, as
 * poll, select and epoll_wait do on Linux whatever the handler asked for.
 * The layer's own callers (the looper) go on waiting instead. `mask`: the
 * signal mask while waiting (ppoll, pselect, epoll_pwait).
 */
typedef int (*ready_check)(void *ctx, bool *looks_itself);

static int wait_ready(ready_check check, void *ctx, int timeout_ms, bool interruptible, const u32 *mask)
{
    u64 deadline = timeout_ms > 0 ? mg_time_monotonic_ns() + (u64)timeout_ms * 1000000ull : 0;
    mg_gthread *t = mask ? mg_gthread_self() : NULL;
    u32 saved = 0;
    int result;
    io_init();
    if (t) {
        saved = t->sig_blocked;
        t->sig_blocked = *mask;
    }
    mg_lock_acquire(&g_io_lock);
    for (;;) {
        bool looks_itself = false;
        int hits, w;
        u64 left = 0, wake;
        g_timer_wake = 0;
        hits = check(ctx, &looks_itself);
        wake = g_timer_wake;
        if (hits || timeout_ms == 0) {
            result = hits;
            break;
        }
        if (timeout_ms > 0) {
            u64 now = mg_time_monotonic_ns();
            if (now >= deadline) {
                result = 0;
                break;
            }
            left = deadline - now;
        }
        if (wake) {
            /* An armed timer among them: looked at again when it expires, which nothing announces. */
            u64 now = mg_time_monotonic_ns(), until = wake > now ? wake - now : 1;
            if (!left || until < left) left = until;
        }
        if (looks_itself && (!left || left > 5000000ull)) left = 5000000ull;
        w = io_wait(left);
        if (interruptible && w >= MG_FD_HANDLER_RAN) {
            result = -1;
            break;
        }
    }
    mg_lock_release(&g_io_lock);
    if (t) {
        t->sig_blocked = saved;
        if (mg_signals_ready(t)) mg_signals_check();
    }
    if (result < 0) mg_set_errno(GE_INTR);
    return result;
}

typedef struct poll_ctx {
    mg_pollfd *fds;
    int count;
} poll_ctx;

static int poll_check(void *ctx, bool *looks_itself)
{
    poll_ctx *c = (poll_ctx *)ctx;
    int i, hits = 0;
    for (i = 0; i < c->count; i++) {
        mg_pollfd *p = &c->fds[i];
        gobj *o = p->fd >= 0 && p->fd < MAX_FDS ? g_fds[p->fd] : NULL;
        p->revents = 0;
        if (p->fd < 0) continue;
        if (!o) {
            p->revents = G_POLLNVAL;
        } else {
            p->revents = (i16)ready_locked(o, (u16)p->events);
            if (o->type == OBJ_EXT && !o->ops->announces) *looks_itself = true;
        }
        if (p->revents) hits++;
    }
    return hits;
}

/* poll for the layer's own callers: a signal's handler runs, and the wait goes on. */
int mg_fd_poll(mg_pollfd *fds, int count, int timeout_ms)
{
    poll_ctx c;
    c.fds = fds;
    c.count = count;
    return wait_ready(poll_check, &c, timeout_ms, false, NULL);
}

static int g_poll(mg_pollfd *fds, unsigned count, int timeout_ms)
{
    poll_ctx c;
    c.fds = fds;
    c.count = (int)count;
    return wait_ready(poll_check, &c, timeout_ms, true, NULL);
}

/* A struct timespec / timeval (two 32-bit words) as milliseconds, rounded up; -2 when it is not one. */
static int timeout_of(const i32 *t, i64 per_ms)
{
    i64 ms;
    if (t[0] < 0 || t[1] < 0 || t[1] >= per_ms * 1000) return -2;
    ms = (i64)t[0] * 1000 + ((i64)t[1] + per_ms - 1) / per_ms;
    return ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)ms;
}

static int g_ppoll(mg_pollfd *fds, unsigned count, const i32 *ts, const u32 *mask)
{
    poll_ctx c;
    int ms = ts ? timeout_of(ts, 1000000) : -1;
    if (ms == -2) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    c.fds = fds;
    c.count = (int)count;
    return wait_ready(poll_check, &c, ms, true, mask);
}

/* select: bionic's fd_set is 1024 bits in 32-bit words. Readable, writable and exceptional are what Linux makes of poll's bits. */
static int select_wait(int nfds, u32 *rd, u32 *wr, u32 *ex, int timeout_ms, const u32 *mask)
{
    mg_pollfd *p;
    poll_ctx c;
    int n = 0, i, hits, total = 0;
    if (nfds < 0 || nfds > MAX_FDS || timeout_ms == -2) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    p = (mg_pollfd *)mg_alloc(sizeof(mg_pollfd) * (size_t)(nfds + 1));
    io_init();
    mg_lock_acquire(&g_io_lock);
    for (i = 0; i < nfds; i++) {
        int ev = 0;
        if (rd && (rd[i / 32] >> (i % 32)) & 1) ev |= G_POLLIN;
        if (wr && (wr[i / 32] >> (i % 32)) & 1) ev |= G_POLLOUT;
        if (ex && (ex[i / 32] >> (i % 32)) & 1) ev |= G_POLLPRI;
        if (!ev) continue;
        if (!g_fds[i]) {
            /* Linux refuses a set with a descriptor that is not open in it, before waiting. */
            mg_lock_release(&g_io_lock);
            free(p);
            mg_set_errno(GE_BADF);
            return -1;
        }
        p[n].fd = i;
        p[n].events = (i16)ev;
        n++;
    }
    mg_lock_release(&g_io_lock);
    c.fds = p;
    c.count = n;
    hits = wait_ready(poll_check, &c, timeout_ms, true, mask);
    if (hits < 0) {
        free(p);
        return -1;
    }
    if (rd) memset(rd, 0, (size_t)((nfds + 31) / 32) * 4);
    if (wr) memset(wr, 0, (size_t)((nfds + 31) / 32) * 4);
    if (ex) memset(ex, 0, (size_t)((nfds + 31) / 32) * 4);
    for (i = 0; i < n && hits > 0; i++) {
        int fd = p[i].fd;
        if (rd && (p[i].revents & (G_POLLIN | G_POLLHUP | G_POLLERR)) && (p[i].events & G_POLLIN)) { rd[fd / 32] |= 1u << (fd % 32); total++; }
        if (wr && (p[i].revents & (G_POLLOUT | G_POLLERR)) && (p[i].events & G_POLLOUT)) { wr[fd / 32] |= 1u << (fd % 32); total++; }
        if (ex && (p[i].revents & G_POLLPRI) && (p[i].events & G_POLLPRI)) { ex[fd / 32] |= 1u << (fd % 32); total++; }
    }
    free(p);
    return total;
}

static int g_select(int nfds, u32 *rd, u32 *wr, u32 *ex, i32 *tv)
{
    u64 began = mg_time_monotonic_ns();
    int ms = tv ? timeout_of(tv, 1000) : -1, r = select_wait(nfds, rd, wr, ex, ms, NULL);
    if (tv && ms >= 0 && r >= 0) {
        /* As Linux does: the time that was left. */
        u64 spent = (mg_time_monotonic_ns() - began) / 1000ull, all = (u64)ms * 1000ull, left = spent < all ? all - spent : 0;
        tv[0] = (i32)(left / 1000000ull);
        tv[1] = (i32)(left % 1000000ull);
    }
    return r;
}

static int g_pselect(int nfds, u32 *rd, u32 *wr, u32 *ex, const i32 *ts, const u32 *mask)
{
    return select_wait(nfds, rd, wr, ex, ts ? timeout_of(ts, 1000000) : -1, mask);
}

/*
 * epoll, as a list that the wait above goes through. Level-triggered as
 * Linux's is by default. EPOLLET is taken to mean what an app that asks for
 * it relies on: a descriptor is not reported again while what was reported
 * still holds, and is once something new holds or a condition went and came
 * back. (Linux also reports it again when more data arrives on a descriptor
 * not yet emptied, which this cannot tell.) EPOLLONESHOT disarms an item once
 * it is reported, until EPOLL_CTL_MOD.
 */
typedef struct g_epoll_event {
    u32 events;
    u64 data;
} g_epoll_event;

#define G_EPOLLRDHUP 0x2000u
#define G_EPOLLONESHOT 0x40000000u
#define G_EPOLLET 0x80000000u
/* The events an item can be waiting for, which are poll's bits. */
#define G_EPOLL_EVENTS (0x3FFu | G_EPOLLRDHUP)

static int g_epoll_create(int size)
{
    gobj *o = (gobj *)mg_alloc(sizeof(*o));
    int fd;
    (void)size;
    o->type = OBJ_EPOLL;
    o->refs = 1;
    fd = fd_new(o);
    if (fd < 0) free(o);
    return fd;
}

static int g_epoll_create1(int flags)
{
    int fd = g_epoll_create(1);
    if (fd >= 0 && (flags & G_O_CLOEXEC)) g_cloexec[fd] = 1;
    return fd;
}

static int g_epoll_ctl(int epfd, int op, int fd, const g_epoll_event *ev)
{
    int i, r = 0;
    WITH_FD(epfd, o, -1)
    if (o->type != OBJ_EPOLL || fd == epfd) {
        fd_put(o);
        mg_set_errno(GE_INVAL);
        return -1;
    }
    mg_lock_acquire(&g_io_lock);
    for (i = 0; i < o->nitems && o->items[i].fd != fd; i++) { }
    if ((op == 1 || op == 3) && !ev) {
        r = GE_FAULT;
    } else if (op == 1) {                          /* EPOLL_CTL_ADD */
        if (i < o->nitems) r = GE_EXIST;
        else if (fd < 0 || fd >= MAX_FDS || !g_fds[fd]) r = GE_BADF;
        else {
            o->items = (epoll_item *)mg_realloc(o->items, sizeof(epoll_item) * (size_t)(o->nitems + 1));
            memset(&o->items[o->nitems], 0, sizeof(epoll_item));
            o->items[o->nitems].fd = fd;
            o->items[o->nitems].events = ev->events;
            o->items[o->nitems].data = ev->data;
            o->nitems++;
        }
    } else if (op == 2) {                          /* EPOLL_CTL_DEL */
        if (i == o->nitems) r = GE_NOENT;
        else o->items[i] = o->items[--o->nitems];
    } else if (op == 3) {                          /* EPOLL_CTL_MOD: armed again */
        if (i == o->nitems) r = GE_NOENT;
        else {
            o->items[i].events = ev->events;
            o->items[i].data = ev->data;
            o->items[i].last = 0;
            o->items[i].disarmed = false;
        }
    } else {
        r = GE_INVAL;
    }
    mg_cond_broadcast(&g_io_cond);
    mg_lock_release(&g_io_lock);
    fd_put(o);
    if (r) {
        mg_set_errno(r);
        return -1;
    }
    return 0;
}

typedef struct epoll_ctx {
    gobj *ep;
    g_epoll_event *out;
    int max;
} epoll_ctx;

static int epoll_check(void *ctx, bool *looks_itself)
{
    epoll_ctx *c = (epoll_ctx *)ctx;
    gobj *ep = c->ep;
    int i, n, got = 0;
    /* A file that has gone is gone from every epoll set too, as on Linux when its last descriptor is closed. */
    for (i = 0; i < ep->nitems;) {
        int fd = ep->items[i].fd;
        if (fd < 0 || fd >= MAX_FDS || !g_fds[fd]) ep->items[i] = ep->items[--ep->nitems];
        else i++;
    }
    n = ep->nitems;
    /* Round from where the last report stopped, so that a full `out` does not leave the same items out every time. */
    for (i = 0; i < n && got < c->max; i++) {
        epoll_item *it = &ep->items[(ep->epoll_next + i) % n];
        gobj *o = g_fds[it->fd];
        int now, fresh;
        if (it->disarmed) continue;
        if (o->type == OBJ_EXT && !o->ops->announces) *looks_itself = true;
        now = ready_locked(o, (int)(it->events & G_EPOLL_EVENTS));
        fresh = (it->events & G_EPOLLET) ? now & ~it->last : now;
        it->last &= now;
        if (!fresh) continue;
        it->last = now;
        c->out[got].events = (u32)now;
        c->out[got].data = it->data;
        got++;
        if (it->events & G_EPOLLONESHOT) it->disarmed = true;
    }
    if (n) ep->epoll_next = (ep->epoll_next + i) % n;
    return got;
}

static int g_epoll_pwait(int epfd, g_epoll_event *out, int max, int timeout_ms, const u32 *mask)
{
    epoll_ctx c;
    int got;
    WITH_FD(epfd, o, -1)
    if (o->type != OBJ_EPOLL || max <= 0) {
        fd_put(o);
        mg_set_errno(GE_INVAL);
        return -1;
    }
    c.ep = o;
    c.out = out;
    c.max = max;
    got = wait_ready(epoll_check, &c, timeout_ms, true, mask);
    fd_put(o);
    return got;
}

static int g_epoll_wait(int epfd, g_epoll_event *out, int max, int timeout_ms) { return g_epoll_pwait(epfd, out, max, timeout_ms, NULL); }

static int g_eventfd(unsigned initial, int flags) { return mg_fd_eventfd(initial, flags); }

static int g_fchmod(int fd, int mode) { (void)fd; (void)mode; return 0; }
static int g_flock(int fd, int op) { (void)fd; (void)op; return 0; }
static void g_sync(void) { }

#define IO(n, f) MG_WRAP(n, f)

const mg_sym mg_tab_io[] = {
    IO("open", g_open), IO("open64", g_open), IO("__open_2", g_open), IO("openat", g_openat), IO("openat64", g_openat),
    IO("__openat_2", g_openat), IO("creat", g_creat), IO("creat64", g_creat), IO("close", g_close),
    IO("read", g_read), IO("write", g_write), IO("__read_chk", g_read), IO("__write_chk", g_write),
    IO("pread", g_pread), IO("pread64", g_pread64), IO("pwrite", g_pwrite), IO("pwrite64", g_pwrite64),
    IO("__pread_chk", g_pread_chk), IO("__pread64_chk", g_pread64_chk), IO("__umask_chk", g_umask_chk),
    IO("__pwrite64_chk", g_pwrite64_chk), IO("__readlink_chk", g_readlink_chk),
    IO("sendfile", g_sendfile), IO("sendfile64", g_sendfile64), IO("posix_fallocate64", g_posix_fallocate64), IO("splice", g_io_nosys),
    IO("mkfifo", g_io_perm), IO("mknod", g_io_perm), IO("pathconf", g_pathconf), IO("fpathconf", g_fpathconf),
    IO("getxattr", g_io_notsup), IO("setxattr", g_io_notsup), IO("listxattr", g_io_notsup), IO("removexattr", g_io_notsup),
    IO("inotify_init", g_io_nosys), IO("inotify_add_watch", g_io_nosys), IO("inotify_rm_watch", g_io_nosys),
    IO("tcgetattr", g_io_notty), IO("tcsetattr", g_io_notty), IO("tcdrain", g_io_notty), IO("tcsendbreak", g_io_notty),
    IO("readv", g_readv), IO("writev", g_writev), IO("lseek", g_lseek), IO("lseek64", g_lseek64),
    IO("ftruncate", g_ftruncate), IO("ftruncate64", g_ftruncate64), IO("truncate", g_truncate),
    IO("posix_fadvise", g_posix_fadvise), IO("posix_fadvise64", g_posix_fadvise64), IO("posix_fallocate", g_posix_fallocate),
    IO("fsync", g_fsync), IO("fdatasync", g_fsync), IO("sync", g_sync),
    IO("stat", g_stat_path), IO("stat64", g_stat_path), IO("lstat", g_stat_path), IO("lstat64", g_stat_path),
    IO("fstat", g_fstat), IO("fstat64", g_fstat), IO("fstatat", g_fstatat), IO("fstatat64", g_fstatat),
    IO("access", g_access), IO("faccessat", g_faccessat),
    IO("statfs", g_statfs_fn), IO("statfs64", g_statfs_fn), IO("fstatfs", g_fstatfs_fn), IO("fstatfs64", g_fstatfs_fn),
    IO("statvfs", g_statvfs_fn), IO("fstatvfs", g_fstatvfs_fn),
    /* (With 32-bit counts on a 32-bit Android, as the plain ones: the two structures are the same there.) */
    IO("statvfs64", g_statvfs_fn), IO("fstatvfs64", g_fstatvfs_fn),
    IO("unlink", g_unlink), IO("unlinkat", g_unlinkat), IO("rmdir", g_rmdir), IO("mkdir", g_mkdir), IO("mkdirat", g_mkdirat),
    IO("remove", g_remove), IO("rename", g_rename), IO("renameat", g_renameat),
    IO("chdir", g_chdir), IO("getcwd", g_getcwd), IO("realpath", g_realpath), IO("readlink", g_readlink),
    IO("chmod", g_ok_path), IO("fchmod", g_fchmod), IO("chown", g_chown), IO("lchown", g_chown), IO("utime", g_utime),
    IO("utimes", g_utime), IO("lutimes", g_utime), IO("futimes", g_futimes), IO("link", g_fail_link), IO("symlink", g_fail_link),
    IO("umask", g_umask), IO("flock", g_flock),
    IO("fchown", g_ok_fd),
    IO("opendir", g_opendir), IO("fdopendir", g_fdopendir), IO("readdir", g_readdir), IO("readdir64", g_readdir),
    IO("readdir_r", g_readdir_r), IO("readdir64_r", g_readdir_r), IO("closedir", g_closedir), IO("rewinddir", g_rewinddir),
    IO("telldir", g_telldir), IO("seekdir", g_seekdir), IO("dirfd", g_dirfd), IO("scandir", g_scandir), IO("scandir64", g_scandir),
    IO("alphasort", g_alphasort), IO("alphasort64", g_alphasort),
    IO("pipe", g_pipe), IO("pipe2", g_pipe2), IO("eventfd", g_eventfd), IO("memfd_create", g_memfd_create),
    IO("timerfd_create", g_timerfd_create), IO("timerfd_settime", g_timerfd_settime), IO("timerfd_gettime", g_timerfd_gettime), IO("dup", g_dup), IO("dup2", g_dup2), IO("dup3", g_dup3),
    IO("fcntl", g_fcntl), IO("__fcntl64", g_fcntl), IO("ioctl", g_ioctl), IO("isatty", g_isatty),
    IO("poll", g_poll), IO("__poll_chk", g_poll), IO("ppoll", g_ppoll), IO("__ppoll_chk", g_ppoll), IO("select", g_select),
    IO("pselect", g_pselect), IO("epoll_create", g_epoll_create), IO("epoll_create1", g_epoll_create1), IO("epoll_ctl", g_epoll_ctl),
    IO("epoll_wait", g_epoll_wait), IO("epoll_pwait", g_epoll_pwait),
    MG_END
};
