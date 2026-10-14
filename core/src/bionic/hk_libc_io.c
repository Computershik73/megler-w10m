/*
 * File descriptors and the file system, as guest code sees them.
 *
 * Guest descriptors are this layer's own small integers. Behind one is a host
 * file, an in-memory file this layer made up (/proc/cpuinfo), a pipe, an
 * eventfd, or one of the standard streams -- which go to the log. Pipes and
 * eventfds exist here because native_app_glue and every engine's main loop
 * are built on them, and nothing on the host has their shape.
 *
 * Guest paths are Android's. A mount table says where each prefix lives on
 * the host; what has no mount does not exist, except the handful of /proc and
 * /sys files that code reads to learn about the device.
 */
#include "hk_bionic.h"

#include <stdio.h>

#include "../elf/hk_elf.h"
#include "hk_fd.h"

/* --------------------------------------------------------------- the paths
 *
 * The shared storage, Android's "SD card", is one folder for every app:
 * /storage/emulated/0. The other names it goes by are that path written
 * another way and are rewritten to it before anything else looks at a path,
 * as the symbolic links they are on Android would be followed.
 *
 * Below it some names are mounts of their own: the app's own part
 * (Android/data/<package>, Android/obb/<package>), which stays in the app's
 * folder on the host, and the links -- folders of the phone itself, whose
 * host paths are foreign ones (host/hk_host.h). Android's shared storage
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
static mount g_mounts[96];
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

void hk_fs_mount(const char *guest_prefix, const char *host_dir)
{
    mount *m = mount_find(guest_prefix);
    if (m) {
        free(m->host);
        m->host = hk_strdup(host_dir);
        m->link = m->read_only = false;
        return;
    }
    if (g_nmounts >= (int)HK_COUNT(g_mounts)) {
        hk_err("files: no room for one more mount: %s does not exist", guest_prefix);
        return;
    }
    m = &g_mounts[g_nmounts++];
    memset(m, 0, sizeof(*m));
    m->guest = hk_strdup(guest_prefix);
    m->host = hk_strdup(host_dir);
    m->glen = strlen(guest_prefix);
    m->exact = !strncmp(guest_prefix, EXTERNAL "/", EXTERNAL_LEN + 1) ? EXTERNAL_LEN + 1 : m->glen;
}

void hk_fs_configure(const char *package, const char *internal_dir, const char *external_dir)
{
    static const char *const own[] = { "data", "obb" };
    char *p, *h;
    size_t i;
    hk_strlcpy(g_package, package, sizeof(g_package));
    snprintf(g_internal, sizeof(g_internal), "/data/data/%s", package);
    hk_path_mkdirs(internal_dir);
    hk_fs_mount(g_internal, internal_dir);
    p = hk_sprintf("/data/user/0/%s", package);
    hk_fs_mount(p, internal_dir);
    free(p);
    /*
     * The app's own part of the shared storage. The folders are made now: a mount with nothing behind it would not exist, and its
     * parents are the shared storage's, so an app making the path one name at a time could not make it either.
     */
    for (i = 0; i < HK_COUNT(own); i++) {
        p = hk_sprintf(EXTERNAL "/Android/%s/%s", own[i], package);
        h = hk_sprintf("%s/Android/%s/%s", external_dir, own[i], package);
        hk_path_mkdirs(h);
        hk_fs_mount(p, h);
        free(p);
        free(h);
    }
    p = hk_sprintf("%s/tmp", internal_dir);
    hk_path_mkdirs(p);
    hk_fs_mount("/data/local/tmp", p);
    hk_fs_mount("/tmp", p);
    free(p);
    hk_strlcpy(g_cwd, g_internal, sizeof(g_cwd));
    if (!g_space_dir) g_space_dir = hk_strdup(internal_dir);
}

void hk_fs_shared_storage(const char *shared_dir, const hk_fs_link *links, int count)
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
    hk_path_mkdirs(shared_dir);
    hk_fs_mount(EXTERNAL, shared_dir);
    free(g_space_dir);
    g_space_dir = hk_strdup(shared_dir);
    /* Real folders, so that an app can make what Android lets it make beside its own part (Android/media/<package>). */
    p = hk_sprintf("%s/Android/data", shared_dir);
    hk_path_mkdirs(p);
    free(p);
    p = hk_sprintf("%s/Android/obb", shared_dir);
    hk_path_mkdirs(p);
    free(p);
    for (i = 0; i < HK_COUNT(k_standard_dirs); i++) {
        bool linked = false;
        for (k = 0; k < count && !linked; k++) linked = strlen(links[k].name) == strlen(k_standard_dirs[i]) && ascii_same(links[k].name, k_standard_dirs[i], strlen(k_standard_dirs[i]), true);
        if (linked) continue;
        p = hk_sprintf("%s/%s", shared_dir, k_standard_dirs[i]);
        hk_path_mkdir(p, NULL);
        free(p);
    }
    for (k = 0; k < count; k++) {
        mount *m;
        p = hk_sprintf(EXTERNAL "/%s", links[k].name);
        h = hk_sprintf(HK_FOREIGN_PREFIX "%d", links[k].root);
        hk_fs_mount(p, h);
        m = mount_find(p);
        if (m) {
            m->link = true;
            m->read_only = links[k].read_only;
        }
        free(p);
        free(h);
    }
}

const char *hk_fs_internal_dir(void) { return g_internal; }
const char *hk_fs_external_dir(void) { return g_external; }
const char *hk_fs_package(void) { return g_package; }

/* A path that begins with one of the shared storage's other names, said with its own; else the path as it is. Takes `g`. */
static char *without_alias(char *g)
{
    size_t i;
    for (i = 0; i < HK_COUNT(k_aliases); i++) {
        size_t n = strlen(k_aliases[i].path);
        if (!strncmp(g, k_aliases[i].path, n) && (!g[n] || g[n] == '/')) {
            char *out = hk_sprintf(EXTERNAL "%s", g + n);
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
    char *out = (char *)hk_alloc(cap), *full;
    const char *p;
    if (path[0] == '/') full = hk_strdup(path);
    else full = hk_sprintf("%s/%s", g_cwd, path);
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
#if HK_HOST_MAPS_NAMES
    if (rest[0]) {
        /* The guest's names as the host takes them (host/hk_host.h): here a backslash in one is still known to be a name's. */
        char *mapped = hk_name_to_host(rest, HK_NAME_FOREIGN), *out;
        out = hk_sprintf("%s%s%s", best->host, (best->glen == 1 && g[1]) ? "/" : "", mapped);
        free(mapped);
        return out;
    }
#endif
    return hk_sprintf("%s%s%s", best->host, (best->glen == 1 && g[1]) ? "/" : "", rest);
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
    for (i = 0; i < HK_COUNT(k_aliases); i++)
        if (strlen(k_aliases[i].path) > len && k_aliases[i].path[len] == '/' && !strncmp(k_aliases[i].path, g, len)) return true;
    return false;
}

/* Whether a normalized path is in a link that may only be read. */
static bool in_read_only(const char *g)
{
    const mount *m = mount_of(g);
    return m && m->read_only;
}

char *hk_fs_host_path(const char *guest_path)
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
        t->s = (char *)hk_realloc(t->s, t->cap);
    }
    va_start(ap, fmt);
    n = vsnprintf(t->s + t->n, t->cap - t->n, fmt, ap);
    va_end(ap);
    if (n > 0) t->n += (size_t)n < t->cap - t->n ? (size_t)n : t->cap - t->n - 1;
}

static int maps_one(hk_addr bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    textbuf *t = (textbuf *)user;
    const Elf32_Phdr *ph = (const Elf32_Phdr *)phdr;
    unsigned i;
    for (i = 0; i < phnum; i++) {
        u32 lo, hi;
        if (ph[i].p_type != PT_LOAD) continue;
        lo = HK_ALIGN_DOWN(ph[i].p_vaddr, HK_PAGE);
        hi = HK_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, HK_PAGE);
        tb_add(t, "%08x-%08x %c%c%cp %08x 103:01 %u /data/app/%s-1/lib/arm/%s\n", (unsigned)(bias + lo), (unsigned)(bias + hi),
               (ph[i].p_flags & PF_R) ? 'r' : '-', (ph[i].p_flags & PF_W) ? 'w' : '-', (ph[i].p_flags & PF_X) ? 'x' : '-',
               (unsigned)HK_ALIGN_DOWN(ph[i].p_offset, HK_PAGE), 1000 + i, g_package, name);
    }
    return 0;
}

/* The content of a /proc or /sys file some code reads to learn about the device; NULL when there is none by that name. */
static char *synthetic(const char *g, size_t *size)
{
    textbuf t;
    int cpus = hk_cpu_count(), i;
    memset(&t, 0, sizeof(t));
    if (!strcmp(g, "/proc/cpuinfo")) {
        for (i = 0; i < cpus; i++)
            tb_add(&t, "processor\t: %d\nmodel name\t: ARMv7 Processor rev 4 (v7l)\nBogoMIPS\t: 38.40\n"
                       "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae\n"
                       "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xd03\nCPU revision\t: 4\n\n", i);
        tb_add(&t, "Hardware\t: Qualcomm Technologies, Inc MSM\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n");
    } else if (!strcmp(g, "/proc/meminfo")) {
        u64 limit = hk_mem_limit(), used = hk_mem_usage();
        if (!limit) limit = 1024ull * 1024 * 1024;
        tb_add(&t, "MemTotal:       %u kB\nMemFree:        %u kB\nMemAvailable:   %u kB\nBuffers:        0 kB\nCached:         0 kB\n"
                   "SwapTotal:      0 kB\nSwapFree:       0 kB\n",
               (unsigned)(limit >> 10), (unsigned)((limit > used ? limit - used : 0) >> 10), (unsigned)((limit > used ? limit - used : 0) >> 10));
    } else if (!strcmp(g, "/proc/self/maps")) {
        hk_ld_iterate(maps_one, &t);
        if (!t.s) tb_add(&t, "%s", "");
    } else if (!strcmp(g, "/proc/self/cmdline")) {
        tb_add(&t, "%s", g_package);
        t.n++;                 /* the terminating NUL is part of the file */
    } else if (!strcmp(g, "/proc/self/status")) {
        tb_add(&t, "Name:\t%.15s\nState:\tR (running)\nTgid:\t4242\nPid:\t4242\nPPid:\t1\nTracerPid:\t0\nUid:\t10100\t10100\t10100\t10100\n"
                   "Gid:\t10100\t10100\t10100\t10100\nVmRSS:\t%u kB\nThreads:\t1\n", g_package, (unsigned)(hk_mem_usage() >> 10));
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
    } else {
        return NULL;
    }
    *size = t.n;
    return t.s;
}

/* ----------------------------------------------------- open descriptions */

enum { OBJ_FILE = 1, OBJ_MEM, OBJ_NULL, OBJ_RANDOM, OBJ_PIPE_R, OBJ_PIPE_W, OBJ_EVENT, OBJ_LOG, OBJ_EPOLL, OBJ_DIRFD, OBJ_EXT };

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
} epoll_item;

typedef struct gobj {
    int type, refs;
    int flags;                     /* the guest's O_* flags */
    hk_file *file;
    char *path;                    /* guest path, for files and directories */
    u8 *mem;
    size_t mem_size;
    u64 pos;
    pipe_buf *pipe;
    u64 counter;                   /* eventfd */
    bool semaphore;
    char line[512];                /* a standard stream's unfinished line */
    u32 line_len;
    int log_level;
    epoll_item *items;
    int nitems;
    const hk_fd_ops *ops;          /* OBJ_EXT: another module's object (a socket) */
    void *ext;
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
static hk_lock g_io_lock = HK_LOCK_INIT;
static hk_cond g_io_cond;
static bool g_io_inited;

static void io_init(void)
{
    gobj *o;
    int i;
    if (g_io_inited) return;
    g_io_inited = true;
    hk_cond_init(&g_io_cond);
    /* 0 reads as end of file; 1 and 2 are the log. */
    o = (gobj *)hk_alloc(sizeof(*o));
    o->type = OBJ_NULL;
    o->refs = 1;
    g_fds[0] = o;
    for (i = 1; i <= 2; i++) {
        o = (gobj *)hk_alloc(sizeof(*o));
        o->type = OBJ_LOG;
        o->refs = 1;
        o->flags = 1;
        o->log_level = i == 1 ? HK_LOG_INFO : HK_LOG_WARN;
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
    hk_lock_acquire(&g_io_lock);
    fd = fd_alloc_locked(o, 3);
    hk_lock_release(&g_io_lock);
    if (fd < 0) hk_set_errno(GE_MFILE);
    return fd;
}

static gobj *fd_get(int fd)
{
    gobj *o;
    io_init();
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    hk_lock_acquire(&g_io_lock);
    o = g_fds[fd];
    if (o) o->refs++;
    hk_lock_release(&g_io_lock);
    return o;
}

static void obj_destroy(gobj *o)
{
    if (o->file) hk_file_close(o->file);
    if (o->type == OBJ_EXT && o->ops && o->ops->close) o->ops->close(o->ext);
    if (o->type == OBJ_LOG && o->line_len) hk_log(o->log_level, "%.*s", (int)o->line_len, o->line);
    free(o->mem);
    free(o->path);
    free(o->items);
    free(o);
}

static void fd_put(gobj *o)
{
    bool last;
    pipe_buf *p = NULL;
    hk_lock_acquire(&g_io_lock);
    last = --o->refs == 0;
    if (last && o->pipe) {
        p = o->pipe;
        if (o->type == OBJ_PIPE_R) p->readers--;
        else p->writers--;
        if (p->readers || p->writers) p = NULL;
        hk_cond_broadcast(&g_io_cond);
    }
    hk_lock_release(&g_io_lock);
    if (last) {
        free(p);
        obj_destroy(o);
    }
}

int hk_fd_register(const hk_fd_ops *ops, void *ext)
{
    gobj *o = (gobj *)hk_alloc(sizeof(*o));
    int fd;
    o->type = OBJ_EXT;
    o->refs = 1;
    o->ops = ops;
    o->ext = ext;
    fd = fd_new(o);
    if (fd < 0) free(o);
    return fd;
}

void *hk_fd_ext(int fd, const hk_fd_ops *ops)
{
    gobj *o = fd_get(fd);
    void *e = NULL;
    if (!o) return NULL;
    if (o->type == OBJ_EXT && o->ops == ops) e = o->ext;
    fd_put(o);
    return e;
}

void hk_fd_notify(void)
{
    hk_lock_acquire(&g_io_lock);
    hk_cond_broadcast(&g_io_cond);
    hk_lock_release(&g_io_lock);
}

/* ------------------------------------------------------------------ open */

int hk_fd_open_host(const char *host_path, int host_flags)
{
    int err = 0, fd;
    hk_file *f = hk_file_open(host_path, host_flags, &err);
    gobj *o;
    if (!f) {
        hk_set_errno(hk_errno_of(err));
        return -1;
    }
    o = (gobj *)hk_alloc(sizeof(*o));
    o->type = OBJ_FILE;
    o->refs = 1;
    o->file = f;
    o->flags = (host_flags & HK_O_WRITE) ? ((host_flags & HK_O_READ) ? 2 : 1) : 0;
    fd = fd_new(o);
    if (fd < 0) obj_destroy(o);
    return fd;
}

static int g_open(const char *path, int flags, int mode)
{
    char *g, *h;
    gobj *o;
    hk_stat st;
    int fd, err = 0, hf = 0, acc = flags & G_O_ACCMODE;
    (void)mode;
    hk_trace("open(%s, 0x%x)", path ? path : "(null)", (unsigned)flags);
    if (!path || !*path) {
        hk_set_errno(GE_NOENT);
        return -1;
    }
    g = normalize(path);
    o = (gobj *)hk_alloc(sizeof(*o));
    o->refs = 1;
    o->flags = flags;
    o->path = g;
    if (!strcmp(g, "/dev/null") || !strcmp(g, "/dev/zero")) {
        o->type = OBJ_NULL;
    } else if (!strcmp(g, "/dev/urandom") || !strcmp(g, "/dev/random")) {
        o->type = OBJ_RANDOM;
    } else if (!strncmp(g, "/proc/", 6) || !strncmp(g, "/sys/", 5)) {
        size_t n = 0;
        o->mem = (u8 *)synthetic(g, &n);
        if (!o->mem) {
            obj_destroy(o);
            hk_set_errno(GE_NOENT);
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
        } else if (hk_path_is_foreign(h) && !(flags & G_O_DIRECTORY)) {
            is_dir = asked = false;
        } else {
            /* (A directory that is only the way to a mount is one with nothing of the host's behind it.) */
            is_dir = hk_path_stat(h, &st, NULL) ? st.is_dir : leads_to_mount(g);
        }
        if (!why && !is_dir && (flags & G_O_DIRECTORY)) why = GE_NOTDIR;
        if (!why && !is_dir) {
            if (acc == 0) hf = HK_O_READ;
            else if (acc == 1) hf = HK_O_WRITE;
            else hf = HK_O_READ | HK_O_WRITE;
            if (flags & G_O_CREAT) hf |= HK_O_CREATE;
            if (flags & G_O_EXCL) hf |= HK_O_EXCL;
            if (flags & G_O_TRUNC) hf |= HK_O_TRUNC;
            if (flags & G_O_APPEND) hf |= HK_O_APPEND;
            o->file = hk_file_open(h, hf, &err);
            if (o->file) o->type = OBJ_FILE;
            else if (!asked && err == HK_E_ISDIR) is_dir = true;
            else why = hk_errno_of(err);
        }
        if (!why && is_dir) {
            if (acc) why = GE_ISDIR;
            else o->type = OBJ_DIRFD;
        }
        free(h);
        if (why) {
            obj_destroy(o);
            hk_set_errno(why);
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
            hk_set_errno(GE_BADF);
            return -1;
        }
        full = hk_sprintf("%s/%s", d->path, path);
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
        hk_set_errno(GE_BADF);
        return -1;
    }
    hk_lock_acquire(&g_io_lock);
    o = g_fds[fd];
    g_fds[fd] = NULL;
    hk_lock_release(&g_io_lock);
    if (!o) {
        hk_set_errno(GE_BADF);
        return -1;
    }
    fd_put(o);
    return 0;
}

/* ------------------------------------------------------------ read, write */

/*
 * Waits, with the lock held, for the state of some descriptor to change.
 * A signal sent to the thread ends the wait, and its handler runs -- with the
 * lock let go of -- before this returns. False when the time (0: none) ran out.
 */
static bool io_wait(u64 ns)
{
    hk_gthread *t = hk_gthread_self();
    bool ok = true;
    t->sig_wait_cond = &g_io_cond;
    if (!hk_signals_ready(t)) {
        if (ns) ok = hk_cond_wait_ns(&g_io_cond, &g_io_lock, ns);
        else hk_cond_wait(&g_io_cond, &g_io_lock);
    }
    t->sig_wait_cond = NULL;
    if (hk_signals_ready(t)) {
        hk_lock_release(&g_io_lock);
        hk_signals_check();
        hk_lock_acquire(&g_io_lock);
    }
    return ok;
}

static i32 obj_read(gobj *o, void *buf, u32 n, bool positioned, u64 offset)
{
    int err = 0;
    i64 r;
    switch (o->type) {
    case OBJ_FILE:
        r = positioned ? hk_file_pread(o->file, buf, n, offset, &err) : hk_file_read(o->file, buf, n, &err);
        if (r < 0) hk_set_errno(hk_errno_of(err));
        return (i32)r;
    case OBJ_MEM: {
        u64 at = positioned ? offset : o->pos;
        u32 left = at < o->mem_size ? (u32)(o->mem_size - at) : 0;
        if (n > left) n = left;
        memcpy(buf, o->mem + at, n);
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
        hk_random(buf, n);
        return (i32)n;
    case OBJ_PIPE_R: {
        pipe_buf *p = o->pipe;
        u32 got = 0;
        hk_lock_acquire(&g_io_lock);
        while (!p->count) {
            if (!p->writers) {
                hk_lock_release(&g_io_lock);
                return 0;
            }
            if (o->flags & G_O_NONBLOCK) {
                hk_lock_release(&g_io_lock);
                hk_set_errno(GE_AGAIN);
                return -1;
            }
            io_wait(0);
        }
        while (got < n && p->count) {
            ((u8 *)buf)[got++] = p->data[p->head];
            p->head = (p->head + 1) % PIPE_CAP;
            p->count--;
        }
        hk_cond_broadcast(&g_io_cond);
        hk_lock_release(&g_io_lock);
        return (i32)got;
    }
    case OBJ_EVENT: {
        u64 v;
        if (n < 8) {
            hk_set_errno(GE_INVAL);
            return -1;
        }
        hk_lock_acquire(&g_io_lock);
        while (!o->counter) {
            if (o->flags & G_O_NONBLOCK) {
                hk_lock_release(&g_io_lock);
                hk_set_errno(GE_AGAIN);
                return -1;
            }
            io_wait(0);
        }
        v = o->semaphore ? 1 : o->counter;
        o->counter -= v;
        hk_cond_broadcast(&g_io_cond);
        hk_lock_release(&g_io_lock);
        memcpy(buf, &v, 8);
        return 8;
    }
    case OBJ_EXT:
        if (o->ops->read) return o->ops->read(o->ext, buf, n, (o->flags & G_O_NONBLOCK) != 0);
        break;
    default:
        break;
    }
    hk_set_errno(o->type == OBJ_DIRFD ? GE_ISDIR : GE_BADF);
    return -1;
}

static i32 obj_write(gobj *o, const void *buf, u32 n, bool positioned, u64 offset)
{
    int err = 0;
    i64 r;
    switch (o->type) {
    case OBJ_FILE:
        r = positioned ? hk_file_pwrite(o->file, buf, n, offset, &err) : hk_file_write(o->file, buf, n, &err);
        if (r < 0) hk_set_errno(hk_errno_of(err));
        return (i32)r;
    case OBJ_NULL:
    case OBJ_RANDOM:
        return (i32)n;
    case OBJ_LOG: {
        /* A line at a time: what the app prints arrives in pieces. */
        u32 i;
        hk_lock_acquire(&g_io_lock);
        for (i = 0; i < n; i++) {
            char c = ((const char *)buf)[i];
            if (c == '\n' || o->line_len == sizeof(o->line) - 1) {
                o->line[o->line_len] = 0;
                hk_lock_release(&g_io_lock);
                hk_log(o->log_level, "%s", o->line);
                hk_lock_acquire(&g_io_lock);
                o->line_len = 0;
                if (c == '\n') continue;
            }
            o->line[o->line_len++] = c;
        }
        hk_lock_release(&g_io_lock);
        return (i32)n;
    }
    case OBJ_PIPE_W: {
        pipe_buf *p = o->pipe;
        u32 put = 0;
        hk_lock_acquire(&g_io_lock);
        while (put < n) {
            if (!p->readers) {
                hk_lock_release(&g_io_lock);
                hk_set_errno(GE_PIPE);
                return put ? (i32)put : -1;
            }
            if (p->count == PIPE_CAP) {
                if (o->flags & G_O_NONBLOCK) {
                    hk_lock_release(&g_io_lock);
                    if (put) return (i32)put;
                    hk_set_errno(GE_AGAIN);
                    return -1;
                }
                hk_cond_broadcast(&g_io_cond);
                io_wait(0);
                continue;
            }
            p->data[(p->head + p->count) % PIPE_CAP] = ((const u8 *)buf)[put++];
            p->count++;
        }
        hk_cond_broadcast(&g_io_cond);
        hk_lock_release(&g_io_lock);
        return (i32)put;
    }
    case OBJ_EVENT: {
        u64 v;
        if (n < 8) {
            hk_set_errno(GE_INVAL);
            return -1;
        }
        memcpy(&v, buf, 8);
        hk_lock_acquire(&g_io_lock);
        o->counter += v;
        hk_cond_broadcast(&g_io_cond);
        hk_lock_release(&g_io_lock);
        return 8;
    }
    case OBJ_EXT:
        if (o->ops->write) return o->ops->write(o->ext, buf, n, (o->flags & G_O_NONBLOCK) != 0);
        break;
    default:
        break;
    }
    hk_set_errno(GE_BADF);
    return -1;
}

#define WITH_FD(fd, o, fail)        \
    gobj *o = fd_get(fd);           \
    if (!o) {                       \
        hk_set_errno(GE_BADF);      \
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

i32 hk_fd_read(int fd, void *buf, u32 n) { return g_read(fd, buf, n); }
i32 hk_fd_write(int fd, const void *buf, u32 n) { return g_write(fd, buf, n); }
int hk_fd_close(int fd) { return g_close(fd); }

i32 hk_fd_pread(int fd, void *buf, u32 n, u64 offset)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_read(o, buf, n, true, offset);
    fd_put(o);
    return r;
}

void *hk_fd_hold(int fd)
{
    /* A file of the host's only: a mapping of anything else (a pipe, a made-up file) has nothing to be written back to. */
    gobj *o = fd_get(fd);
    if (o && o->type != OBJ_FILE) {
        fd_put(o);
        o = NULL;
    }
    return o;
}
i32 hk_fd_held_pwrite(void *held, const void *buf, u32 n, u64 offset) { return held ? obj_write((gobj *)held, buf, n, true, offset) : -1; }
void hk_fd_let_go(void *held)
{
    if (held) fd_put((gobj *)held);
}

static i32 g_pread(int fd, void *buf, u32 n, i32 offset) { return hk_fd_pread(fd, buf, n, (u64)(u32)offset); }
static i32 g_pread64(int fd, void *buf, u32 n, i64 offset) { return hk_fd_pread(fd, buf, n, (u64)offset); }

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
        i32 r = g_read(fd, (void *)(hk_addr)iov[i].base, iov[i].len);
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
        i32 r = g_write(fd, (const void *)(hk_addr)iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((u32)r < iov[i].len) break;
    }
    return total;
}

i64 hk_fd_seek(int fd, i64 offset, int whence)
{
    i64 r = -1;
    int err = 0;
    WITH_FD(fd, o, -1)
    if (o->type == OBJ_FILE) {
        r = hk_file_seek(o->file, offset, whence, &err);
        if (r < 0) hk_set_errno(hk_errno_of(err));
    } else if (o->type == OBJ_MEM) {
        i64 at = whence == 0 ? offset : whence == 1 ? (i64)o->pos + offset : (i64)o->mem_size + offset;
        if (at < 0) hk_set_errno(GE_INVAL);
        else {
            o->pos = (u64)at;
            r = at;
        }
    } else if (o->type == OBJ_NULL || o->type == OBJ_RANDOM) {
        r = 0;
    } else {
        hk_set_errno(GE_SPIPE);
    }
    fd_put(o);
    return r;
}

static i32 g_lseek(int fd, i32 offset, int whence)
{
    i64 r = hk_fd_seek(fd, offset, whence);
    if (r > 0x7FFFFFFF) {
        hk_set_errno(GE_OVERFLOW);
        return -1;
    }
    return (i32)r;
}
static i64 g_lseek64(int fd, i64 offset, int whence) { return hk_fd_seek(fd, offset, whence); }

static int g_ftruncate64(int fd, i64 size)
{
    int err = 0, r = -1;
    WITH_FD(fd, o, -1)
    if (o->type != OBJ_FILE) hk_set_errno(GE_INVAL);
    else if (!hk_file_truncate(o->file, (u64)size, &err)) hk_set_errno(hk_errno_of(err));
    else r = 0;
    fd_put(o);
    return r;
}
static int g_ftruncate(int fd, i32 size) { return g_ftruncate64(fd, size); }

static int g_fsync(int fd)
{
    WITH_FD(fd, o, -1)
    if (o->type == OBJ_FILE) hk_file_sync(o->file);
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

static void fill_stat(g_stat *out, const hk_stat *st, const char *gpath)
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
    hk_stat st;
    memset(&st, 0, sizeof(st));
    st.size = size;
    st.mtime_ns = st.atime_ns = st.ctime_ns = (i64)hk_time_realtime_ns();
    fill_stat(out, &st, gpath);
    out->st_mode = mode;
}

static int g_stat_path(const char *path, g_stat *out)
{
    char *g, *h;
    hk_stat st;
    int err = 0;
    size_t n = 0;
    char *text;
    if (!path || !*path) {
        hk_set_errno(GE_NOENT);
        return -1;
    }
    g = normalize(path);
    if (!strncmp(g, "/dev/", 5) && (!strcmp(g, "/dev/null") || !strcmp(g, "/dev/zero") || !strcmp(g, "/dev/urandom") || !strcmp(g, "/dev/random"))) {
        fill_special(out, G_S_IFCHR | 0666, 0, g);
        free(g);
        return 0;
    }
    text = (!strncmp(g, "/proc/", 6) || !strncmp(g, "/sys/", 5)) ? synthetic(g, &n) : NULL;
    if (text) {
        free(text);
        fill_special(out, G_S_IFREG | 0444, n, g);
        free(g);
        return 0;
    }
    h = host_of_normal(g);
    if (!h || !hk_path_stat(h, &st, &err)) {
        /* A directory that is only the way to a mount ("/data", "/storage/emulated") exists too. */
        if (leads_to_mount(g)) {
            fill_special(out, G_S_IFDIR | 0771, 4096, g);
            free(h);
            free(g);
            return 0;
        }
        hk_set_errno(h ? hk_errno_of(err) : GE_NOENT);
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
    hk_stat st;
    int r = 0;
    WITH_FD(fd, o, -1)
    switch (o->type) {
    case OBJ_FILE:
        if (!hk_file_stat(o->file, &st)) {
            hk_set_errno(GE_IO);
            r = -1;
        } else {
            fill_stat(out, &st, o->path);
        }
        break;
    case OBJ_MEM: fill_special(out, G_S_IFREG | 0444, o->mem_size, o->path); break;
    case OBJ_DIRFD: fill_special(out, G_S_IFDIR | 0771, 4096, o->path); break;
    case OBJ_PIPE_R: case OBJ_PIPE_W: fill_special(out, G_S_IFIFO | 0600, 0, "pipe"); break;
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
            hk_set_errno(GE_BADF);
            return -1;
        }
        full = hk_sprintf("%s/%s", d->path, path);
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
            hk_set_errno(GE_ACCES);
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
    if (g_space_dir && hk_path_space(g_space_dir, &total, &available) && total) {
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
            hk_set_errno(GE_NOENT);                 \
            return -1;                              \
        }                                           \
        g = normalize(path);                        \
        why = fixed_name(g, fixed);                 \
        h = why ? NULL : host_of_normal(g);         \
        free(g);                                    \
        if (!h) {                                   \
            hk_set_errno(why ? why : GE_ACCES);     \
            return -1;                              \
        }                                           \
        ok = call;                                  \
        free(h);                                    \
        if (!ok) {                                  \
            hk_set_errno(hk_errno_of(err));         \
            return -1;                              \
        }                                           \
        return 0;                                   \
    }

PATH_OP(g_unlink, GE_BUSY, hk_path_remove(h, &err))
PATH_OP(g_rmdir, GE_BUSY, hk_path_rmdir(h, &err))
PATH_OP(mkdir_host, GE_EXIST, hk_path_mkdir(h, &err))

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
        hk_set_errno(GE_NOENT);
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
    ok = a && b && hk_path_rename(a, b, &err);
    if (!ok) hk_set_errno(why ? why : a && b ? hk_errno_of(err) : GE_ACCES);
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
        hk_set_errno(GE_NOTDIR);
        return -1;
    }
    g = normalize(path);
    hk_strlcpy(g_cwd, g, sizeof(g_cwd));
    free(g);
    return 0;
}

static char *g_getcwd(char *buf, size_t n)
{
    if (!buf) return hk_g_strdup(g_cwd);
    if (strlen(g_cwd) + 1 > n) {
        hk_set_errno(GE_RANGE);
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
        hk_set_errno(GE_INVAL);
        return NULL;
    }
    if (g_stat_path(path, &st) != 0) return NULL;
    g = normalize(path);
    if (!out) out = (char *)hk_g_malloc(4096);
    if (out) hk_strlcpy(out, g, 4096);
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
    hk_set_errno(GE_INVAL);
    return -1;
}

static int g_ok_path(const char *path, int a) { (void)path; (void)a; return 0; }
static int g_ok_fd(int fd, int a) { (void)fd; (void)a; return 0; }
static int g_chown(const char *path, int u, int g) { (void)path; (void)u; (void)g; return 0; }
static int g_utime(const char *path, const void *times) { (void)path; (void)times; return 0; }
static int g_fail_link(const char *a, const char *b)
{
    (void)a; (void)b;
    hk_set_errno(GE_PERM);
    return -1;
}
static int g_umask(int mask) { (void)mask; return 022; }
/* The checked forms a library built with _FORTIFY_SOURCE calls: the same, the buffer's size being the caller's business. */
static int g_umask_chk(int mask) { return g_umask(mask); }
static i32 g_pread64_chk(int fd, void *buf, u32 n, i64 offset, u32 buf_size)
{
    (void)buf_size;
    return hk_fd_pread(fd, buf, n, (u64)offset);
}
static i32 g_pread_chk(int fd, void *buf, u32 n, i32 offset, u32 buf_size)
{
    (void)buf_size;
    return hk_fd_pread(fd, buf, n, (u64)(u32)offset);
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
        i32 got = offset ? hk_fd_pread(in_fd, buf, want, (u64)*offset) : hk_fd_read(in_fd, buf, want), put;
        if (got < 0) return done ? (i32)done : -1;
        if (got == 0) break;
        put = hk_fd_write(out_fd, buf, (u32)got);
        if (put < 0) return done ? (i32)done : -1;
        if (offset) *offset += put;
        else if (put < got) hk_fd_seek(in_fd, (i64)put - got, 1);    /* what was read and not taken is still to be read */
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
    int saved = *hk_errno_ptr(), err;
    if (offset < 0 || len <= 0) return GE_INVAL;
    at = hk_fd_seek(fd, 0, 1);
    end = at < 0 ? -1 : hk_fd_seek(fd, 0, 2);
    if (end < 0) {
        err = *hk_errno_ptr();
        *hk_errno_ptr() = saved;
        return err ? err : GE_BADF;
    }
    hk_fd_seek(fd, at, 0);
    if (end >= offset + len) return 0;
    if (g_ftruncate64(fd, offset + len) == 0) return 0;
    err = *hk_errno_ptr();
    *hk_errno_ptr() = saved;
    return err ? err : GE_IO;
}

/* What this file system does not have: pipes spliced in the kernel, FIFOs and device files, extended attributes, watches. */
static int g_io_nosys(void) { hk_set_errno(GE_NOSYS); return -1; }
static int g_io_perm(void) { hk_set_errno(GE_PERM); return -1; }
static int g_io_notsup(void) { hk_set_errno(GE_OPNOTSUPP); return -1; }
/* And no descriptor is a terminal. */
static int g_io_notty(void) { hk_set_errno(GE_NOTTY); return -1; }

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
    default: hk_set_errno(GE_INVAL); return -1;
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
        d->names = (char **)hk_realloc(d->names, sizeof(char *) * d->cap);
        d->types = (u8 *)hk_realloc(d->types, d->cap);
    }
    d->names[d->count] = hk_strdup(name);
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
        hk_stat st;
        if (!hk_path_stat(host, &st, NULL)) return;
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
        hk_set_errno(GE_NOENT);
        return NULL;
    }
    g = normalize(path);
    h = host_of_normal(g);
    has_host = h != NULL;
    d = (g_dir *)hk_alloc(sizeof(*d));
    d->fd = -1;
    dir_add(".", true, d);
    dir_add("..", true, d);
    listed = h && hk_path_list(h, dir_add, d, &err);
    free(h);
    /* A directory that is only the way to a mount has no folder of the host's to list, and is one all the same. */
    if (!listed && !leads_to_mount(g)) {
        u32 n;
        for (n = 0; n < d->count; n++) free(d->names[n]);
        free(d->names);
        free(d->types);
        free(d);
        free(g);
        hk_set_errno(has_host ? hk_errno_of(err) : GE_NOENT);
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
    for (i = 0; i < HK_COUNT(k_aliases); i++) {
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
    else hk_set_errno(GE_BADF);
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
    hk_strlcpy(d->ent.d_name, d->names[d->at], sizeof(d->ent.d_name));
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
    hk_set_errno(GE_OPNOTSUPP);
    return -1;
}

static int g_scandir(const char *path, g_dirent ***out, hk_addr filter, hk_addr compare)
{
    g_dir *d = g_opendir(path);
    g_dirent **list = NULL, *e;
    int n = 0, cap = 0, i, j;
    if (!d) return -1;
    while ((e = g_readdir(d)) != NULL) {
        g_dirent *copy;
        if (filter && !HK_GFN(int (*)(const g_dirent *), filter)(e)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            list = (g_dirent **)hk_g_realloc(list, sizeof(g_dirent *) * (size_t)cap);
        }
        copy = (g_dirent *)hk_g_malloc(sizeof(g_dirent));
        *copy = *e;
        list[n++] = copy;
    }
    g_closedir(d);
    if (compare) {
        for (i = 1; i < n; i++) {
            g_dirent *key = list[i];
            for (j = i - 1; j >= 0 && HK_GFN(int (*)(const g_dirent **, const g_dirent **), compare)((const g_dirent **)&list[j], (const g_dirent **)&key) > 0; j--)
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
    pipe_buf *p = (pipe_buf *)hk_alloc(sizeof(*p));
    gobj *r = (gobj *)hk_alloc(sizeof(*r)), *w = (gobj *)hk_alloc(sizeof(*w));
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

int hk_fd_eventfd(unsigned initial, int flags)
{
    gobj *o = (gobj *)hk_alloc(sizeof(*o));
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
        hk_set_errno(GE_BADF);
        return -1;
    }
    hk_lock_acquire(&g_io_lock);
    nfd = fd_alloc_locked(o, at_least);
    if (nfd >= 0) g_cloexec[nfd] = cloexec;
    hk_lock_release(&g_io_lock);
    if (nfd < 0) {
        fd_put(o);
        hk_set_errno(GE_MFILE);
    }
    /* The reference fd_get took is the new descriptor's. */
    return nfd;
}

static int g_dup(int fd) { return g_dup_at(fd, 0, false); }

static int g_dup3(int fd, int nfd, int flags)
{
    gobj *o, *old;
    if (nfd < 0 || nfd >= MAX_FDS) {
        hk_set_errno(GE_BADF);
        return -1;
    }
    o = fd_get(fd);
    if (!o) {
        hk_set_errno(GE_BADF);
        return -1;
    }
    if (fd == nfd) {
        fd_put(o);
        return nfd;
    }
    hk_lock_acquire(&g_io_lock);
    old = g_fds[nfd];
    g_fds[nfd] = o;
    g_cloexec[nfd] = (flags & G_O_CLOEXEC) != 0;
    hk_lock_release(&g_io_lock);
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
    case 4: /* F_SETFL: only these two can change */
        o->flags = (o->flags & ~(G_O_NONBLOCK | G_O_APPEND)) | (arg & (G_O_NONBLOCK | G_O_APPEND));
        if (o->type == OBJ_EXT && o->ops->set_nonblock) o->ops->set_nonblock(o->ext, (arg & G_O_NONBLOCK) != 0);
        break;
    default: break;                                /* locks and the rest: granted */
    }
    fd_put(o);
    return r;
}

static int g_ioctl(int fd, unsigned request, void *arg)
{
    int r = -1;
    WITH_FD(fd, o, -1)
    if (request == 0x541B) {                       /* FIONREAD */
        int n = 0;
        hk_lock_acquire(&g_io_lock);
        if (o->type == OBJ_PIPE_R) n = (int)o->pipe->count;
        else if (o->type == OBJ_MEM) n = (int)(o->mem_size > o->pos ? o->mem_size - o->pos : 0);
        hk_lock_release(&g_io_lock);
        if (o->type == OBJ_EXT && o->ops->readable) n = o->ops->readable(o->ext);
        *(int *)arg = n;
        r = 0;
    } else if (request == 0x5421) {                /* FIONBIO */
        if (*(int *)arg) o->flags |= G_O_NONBLOCK;
        else o->flags &= ~G_O_NONBLOCK;
        if (o->type == OBJ_EXT && o->ops->set_nonblock) o->ops->set_nonblock(o->ext, *(int *)arg != 0);
        r = 0;
    } else {
        hk_set_errno(GE_NOTTY);
    }
    fd_put(o);
    return r;
}

static int g_isatty(int fd) { (void)fd; hk_set_errno(GE_NOTTY); return 0; }

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
    int r = 0;
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
    case OBJ_EXT:
        if (o->ops->poll) r = o->ops->poll(o->ext, events);
        break;
    case OBJ_EPOLL:
        break;
    default:
        r = G_POLLIN | G_POLLOUT;
        break;
    }
    return r & (events | G_POLLERR | G_POLLHUP);
}

int hk_fd_poll(hk_pollfd *fds, int count, int timeout_ms)
{
    u64 deadline = timeout_ms > 0 ? hk_time_monotonic_ns() + (u64)timeout_ms * 1000000ull : 0;
    bool any_ext = false;
    int i;
    io_init();
    hk_lock_acquire(&g_io_lock);
    for (;;) {
        int hits = 0;
        for (i = 0; i < count; i++) {
            gobj *o = fds[i].fd >= 0 && fds[i].fd < MAX_FDS ? g_fds[fds[i].fd] : NULL;
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            if (!o) fds[i].revents = G_POLLNVAL;
            else {
                fds[i].revents = (i16)ready_locked(o, fds[i].events);
                if (o->type == OBJ_EXT) any_ext = true;
            }
            if (fds[i].revents) hits++;
        }
        if (hits || timeout_ms == 0) {
            hk_lock_release(&g_io_lock);
            return hits;
        }
        if (timeout_ms < 0) {
            /* Objects of other modules (sockets) cannot wake this condition from the host's side: look again soon. */
            if (any_ext) io_wait(5000000ull);
            else io_wait(0);
        } else {
            u64 now = hk_time_monotonic_ns(), left;
            if (now >= deadline) {
                hk_lock_release(&g_io_lock);
                return 0;
            }
            left = deadline - now;
            if (any_ext && left > 5000000ull) left = 5000000ull;
            io_wait(left);
        }
    }
}

static int g_poll(hk_pollfd *fds, unsigned count, int timeout_ms) { return hk_fd_poll(fds, (int)count, timeout_ms); }

static int g_ppoll(hk_pollfd *fds, unsigned count, const i32 *ts, const void *mask)
{
    (void)mask;
    return hk_fd_poll(fds, (int)count, ts ? (int)(ts[0] * 1000 + ts[1] / 1000000) : -1);
}

/* select: bionic's fd_set is 1024 bits in 32-bit words. */
static int g_select(int nfds, u32 *rd, u32 *wr, u32 *ex, const i32 *tv)
{
    hk_pollfd *p;
    int n = 0, i, hits, total = 0;
    if (nfds < 0 || nfds > MAX_FDS) {
        hk_set_errno(GE_INVAL);
        return -1;
    }
    p = (hk_pollfd *)hk_alloc(sizeof(hk_pollfd) * (size_t)(nfds + 1));
    for (i = 0; i < nfds; i++) {
        int ev = 0;
        if (rd && (rd[i / 32] >> (i % 32)) & 1) ev |= G_POLLIN;
        if (wr && (wr[i / 32] >> (i % 32)) & 1) ev |= G_POLLOUT;
        if (ex && (ex[i / 32] >> (i % 32)) & 1) ev |= G_POLLPRI;
        if (!ev) continue;
        p[n].fd = i;
        p[n].events = (i16)ev;
        n++;
    }
    hits = hk_fd_poll(p, n, tv ? (int)(tv[0] * 1000 + tv[1] / 1000) : -1);
    if (rd) memset(rd, 0, (size_t)((nfds + 31) / 32) * 4);
    if (wr) memset(wr, 0, (size_t)((nfds + 31) / 32) * 4);
    if (ex) memset(ex, 0, (size_t)((nfds + 31) / 32) * 4);
    for (i = 0; i < n && hits > 0; i++) {
        int fd = p[i].fd;
        if (rd && (p[i].revents & (G_POLLIN | G_POLLHUP | G_POLLERR)) && (p[i].events & G_POLLIN)) { rd[fd / 32] |= 1u << (fd % 32); total++; }
        if (wr && (p[i].revents & (G_POLLOUT | G_POLLERR)) && (p[i].events & G_POLLOUT)) { wr[fd / 32] |= 1u << (fd % 32); total++; }
    }
    free(p);
    return total;
}

/* epoll, as a list that poll is run over. */
typedef struct g_epoll_event {
    u32 events;
    u64 data;
} g_epoll_event;

static int g_epoll_create(int size)
{
    gobj *o = (gobj *)hk_alloc(sizeof(*o));
    int fd;
    (void)size;
    o->type = OBJ_EPOLL;
    o->refs = 1;
    fd = fd_new(o);
    if (fd < 0) free(o);
    return fd;
}

static int g_epoll_ctl(int epfd, int op, int fd, const g_epoll_event *ev)
{
    int i, r = 0;
    WITH_FD(epfd, o, -1)
    if (o->type != OBJ_EPOLL) {
        fd_put(o);
        hk_set_errno(GE_INVAL);
        return -1;
    }
    hk_lock_acquire(&g_io_lock);
    for (i = 0; i < o->nitems && o->items[i].fd != fd; i++) { }
    if (op == 1) {                                 /* EPOLL_CTL_ADD */
        if (i < o->nitems) r = GE_EXIST;
        else {
            o->items = (epoll_item *)hk_realloc(o->items, sizeof(epoll_item) * (size_t)(o->nitems + 1));
            o->items[o->nitems].fd = fd;
            o->items[o->nitems].events = ev->events;
            o->items[o->nitems].data = ev->data;
            o->nitems++;
        }
    } else if (op == 2) {                          /* EPOLL_CTL_DEL */
        if (i == o->nitems) r = GE_NOENT;
        else o->items[i] = o->items[--o->nitems];
    } else if (op == 3) {                          /* EPOLL_CTL_MOD */
        if (i == o->nitems) r = GE_NOENT;
        else {
            o->items[i].events = ev->events;
            o->items[i].data = ev->data;
        }
    } else {
        r = GE_INVAL;
    }
    hk_cond_broadcast(&g_io_cond);
    hk_lock_release(&g_io_lock);
    fd_put(o);
    if (r) {
        hk_set_errno(r);
        return -1;
    }
    return 0;
}

static int g_epoll_wait(int epfd, g_epoll_event *out, int max, int timeout_ms)
{
    hk_pollfd *p;
    u64 *data;
    int n, i, hits, got = 0;
    WITH_FD(epfd, o, -1)
    if (o->type != OBJ_EPOLL || max <= 0) {
        fd_put(o);
        hk_set_errno(GE_INVAL);
        return -1;
    }
    hk_lock_acquire(&g_io_lock);
    n = o->nitems;
    p = (hk_pollfd *)hk_alloc(sizeof(hk_pollfd) * (size_t)(n + 1));
    data = (u64 *)hk_alloc(sizeof(u64) * (size_t)(n + 1));
    for (i = 0; i < n; i++) {
        p[i].fd = o->items[i].fd;
        p[i].events = (i16)(o->items[i].events & 0x1F);
        data[i] = o->items[i].data;
    }
    hk_lock_release(&g_io_lock);
    hits = hk_fd_poll(p, n, timeout_ms);
    for (i = 0; i < n && got < max && hits > 0; i++) {
        if (!p[i].revents) continue;
        out[got].events = (u32)p[i].revents;
        out[got].data = data[i];
        got++;
    }
    free(p);
    free(data);
    fd_put(o);
    return got;
}

static int g_epoll_pwait(int epfd, g_epoll_event *out, int max, int timeout_ms, const void *mask)
{
    (void)mask;
    return g_epoll_wait(epfd, out, max, timeout_ms);
}

static int g_eventfd(unsigned initial, int flags) { return hk_fd_eventfd(initial, flags); }

static int g_fchmod(int fd, int mode) { (void)fd; (void)mode; return 0; }
static int g_flock(int fd, int op) { (void)fd; (void)op; return 0; }
static void g_sync(void) { }

#define IO(n, f) HK_WRAP(n, f)

const hk_sym hk_tab_io[] = {
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
    IO("utimes", g_utime), IO("link", g_fail_link), IO("symlink", g_fail_link), IO("umask", g_umask), IO("flock", g_flock),
    IO("fchown", g_ok_fd),
    IO("opendir", g_opendir), IO("fdopendir", g_fdopendir), IO("readdir", g_readdir), IO("readdir64", g_readdir),
    IO("readdir_r", g_readdir_r), IO("readdir64_r", g_readdir_r), IO("closedir", g_closedir), IO("rewinddir", g_rewinddir),
    IO("telldir", g_telldir), IO("seekdir", g_seekdir), IO("dirfd", g_dirfd), IO("scandir", g_scandir), IO("scandir64", g_scandir),
    IO("alphasort", g_alphasort), IO("alphasort64", g_alphasort),
    IO("pipe", g_pipe), IO("pipe2", g_pipe2), IO("eventfd", g_eventfd), IO("dup", g_dup), IO("dup2", g_dup2), IO("dup3", g_dup3),
    IO("fcntl", g_fcntl), IO("__fcntl64", g_fcntl), IO("ioctl", g_ioctl), IO("isatty", g_isatty),
    IO("poll", g_poll), IO("__poll_chk", g_poll), IO("ppoll", g_ppoll), IO("select", g_select),
    IO("epoll_create", g_epoll_create), IO("epoll_create1", g_epoll_create), IO("epoll_ctl", g_epoll_ctl),
    IO("epoll_wait", g_epoll_wait), IO("epoll_pwait", g_epoll_pwait),
    HK_END
};
