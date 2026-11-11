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
#include "../mg_core.h"
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
 * /proc/<the app's own pid> and /proc/thread-self said as /proc/self, which they are on Linux: getpid() is 4242 here
 * (bionic/mg_libc_misc.c), and the files made up for /proc/self are the process's. Tuya Smart's libthing_security.so
 * reads /proc/<getpid()>/status, went on with fopen's NULL, and ended in fgets (0.1.0.673); Google Play services' safeboot
 * said "Unable to access /proc/pid/status: /proc/4242/status". Takes `g`.
 */
static char *proc_self(char *g)
{
    static const char *const names[] = { "/proc/4242", "/proc/thread-self" };
    size_t i;
    for (i = 0; i < MG_COUNT(names); i++) {
        size_t n = strlen(names[i]);
        if (!strncmp(g, names[i], n) && (!g[n] || g[n] == '/')) {
            char *out = mg_sprintf("/proc/self%s", g + n);
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
    return without_alias(proc_self(out));
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

char *mg_fs_app_path_host(const char *path)
{
    char *g, *h = NULL;
    const mount *m;
    if (!path || path[0] != '/') return NULL;
    g = normalize(path);
    m = g ? mount_of(g) : NULL;
    if (m && m->glen > 1) h = host_of_normal(g);
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

/* The folder of an APK's libraries for the ABI: lib/arm, lib/arm64, lib/x86, lib/x86_64; and the system's, lib or lib64. */
#if MG_ARCH_ARM32
#define G_LIB_ISA "arm"
#elif MG_ARCH_ARM64
#define G_LIB_ISA "arm64"
#elif MG_ARCH_X86_64
#define G_LIB_ISA "x86_64"
#else
#define G_LIB_ISA "x86"
#endif
#if MG_ARCH_ARM64 || MG_ARCH_X86_64
#define G_SYSTEM_LIB "/system/lib64/"
#else
#define G_SYSTEM_LIB "/system/lib/"
#endif

/*
 * A loaded library's segments, as the kernel's maps has them: addresses of eight hex digits at least, as many as they take.
 * The app's own libraries are its APK's (/data/app/<package>-1/lib/<isa>/), and Android's own, which the runtime loads from
 * its root (libjavacore.so, libicuuc.so, libcrypto.so and the rest: mg_ld_load_runtime), the system's, where Android keeps
 * them. They were all named the app's: Zona (package 573) looks in maps for libraries in its folder that its APK does not
 * have, called Android's eight "EXTRA .SO FOUND", and was then served the same decoy video for every film.
 */
static int maps_one(mg_addr bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    maps_out *o = (maps_out *)user;
    textbuf *t = o->t;
    const ElfW(Phdr) *ph = (const ElfW(Phdr) *)phdr;
    const mg_lib *lib = NULL;
    unsigned i;
    for (i = 0; i < phnum && !lib; i++)
        if (ph[i].p_type == PT_LOAD) lib = mg_ld_lib_of((const void *)(bias + (mg_addr)ph[i].p_vaddr));
    for (i = 0; i < phnum; i++) {
        mg_addr lo, hi;
        if (ph[i].p_type != PT_LOAD) continue;
        lo = (mg_addr)MG_ALIGN_DOWN(ph[i].p_vaddr, MG_PAGE);
        hi = (mg_addr)MG_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, MG_PAGE);
        tb_add(t, "%08llx-%08llx %c%c%cp %08llx 103:01 %u ", (unsigned long long)(bias + lo), (unsigned long long)(bias + hi), (ph[i].p_flags & PF_R) ? 'r' : '-',
               (ph[i].p_flags & PF_W) ? 'w' : '-', (ph[i].p_flags & PF_X) ? 'x' : '-', (unsigned long long)MG_ALIGN_DOWN(ph[i].p_offset, MG_PAGE), 1000 + i);
        if (lib && mg_ld_lib_is_runtime(lib)) tb_add(t, G_SYSTEM_LIB "%s\n", name);
        else tb_add(t, "/data/app/%s-1/lib/" G_LIB_ISA "/%s\n", g_package, name);
        if (o->details) smaps_details(t, (u32)(hi - lo), (int)ph[i].p_flags);
    }
    return 0;
}

static int g_open(const char *path, int flags, int mode);
static int g_close(int fd);

/*
 * The app's APK as maps shows it on Android, where the runtime maps its base.apk to read its resources and code: the first
 * 64 KB of it, mapped read-only and shared (a view of the file) the first time maps is read, and kept. Zona looks for the
 * APK's path in such a line to check its signature, and with none ("apkPath = ", "isGoodApk = 0") took itself for tampered.
 */
static void maps_apk(textbuf *t, bool details)
{
    static volatile i32 state;                     /* 0 not tried, 1 being mapped, 2 done */
    static void *at;
    if (mg_atomic_cas(&state, 0, 1) == 0) {
        char *path = mg_sprintf("/data/app/%s-1/base.apk", g_package);
        int fd = g_open(path, 0 /* O_RDONLY */, 0);
        if (fd >= 0) {
            void *p = mg_g_mmap2(NULL, 65536, 1 /* PROT_READ */, 1 /* MAP_SHARED */, fd, 0);
            if (p && p != (void *)(mg_addr)-1) at = p;
            g_close(fd);
        }
        free(path);
        mg_atomic_xchg(&state, 2);
    }
    if (state != 2 || !at) return;
    tb_add(t, "%08llx-%08llx r--s 00000000 103:01 999 /data/app/%s-1/base.apk\n", (unsigned long long)(mg_addr)at,
           (unsigned long long)((mg_addr)at + 65536), g_package);
    if (details) smaps_details(t, 65536, PF_R);
}

/*
 * The command line Chromium-based apps (Brave and its kind) read at start. Renderer and GPU run as Android services in
 * processes of their own, which this platform cannot start: in a single process the renderer runs on threads of the
 * browser's, and the GPU too.
 */
#define CHROME_COMMAND_LINE "/data/local/chrome-command-line"

/*
 * The kernel's UUIDs, as text: /proc/sys/kernel/random/boot_id, one for as long as the phone has been up (made from the
 * minute it started, which the realtime and monotonic clocks give alike in every process), and .../uuid, a new random one
 * at every read. Facebook reads the boot's ("Error reading boot_id from procfs" where there was none).
 */
static u64 mix64(u64 x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static void kernel_uuid(textbuf *t, bool boot)
{
    u8 b[16];
    int i;
    if (boot) {
        u64 minute = (mg_time_realtime_ns() - mg_time_monotonic_ns()) / 60000000000ull, hi = mix64(minute ^ 0x6D65676C6572ull), lo = mix64(hi);
        for (i = 0; i < 8; i++) {
            b[i] = (u8)(hi >> (8 * i));
            b[8 + i] = (u8)(lo >> (8 * i));
        }
    } else {
        mg_random(b, sizeof(b));
    }
    b[6] = (u8)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (u8)((b[8] & 0x3F) | 0x80);   /* the RFC 4122 variant */
    tb_add(t, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
           b[11], b[12], b[13], b[14], b[15]);
}

/* Whether a path is one of the files synthetic() makes up. */
static bool is_synthetic(const char *g)
{
    return !strncmp(g, "/proc/", 6) || !strncmp(g, "/sys/", 5) || !strcmp(g, CHROME_COMMAND_LINE);
}

/*
 * The content of a /proc or /sys file some code reads to learn about the device, or of the Chromium command line; NULL
 * when there is none by that name.
 */
#if MG_ARCH_X86_64 || MG_ARCH_X86
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static void cpuid(u32 leaf, u32 sub, u32 r[4])
{
    int v[4];
    __cpuidex(v, (int)leaf, (int)sub);
    r[0] = (u32)v[0]; r[1] = (u32)v[1]; r[2] = (u32)v[2]; r[3] = (u32)v[3];
}
#else
#include <cpuid.h>
static void cpuid(u32 leaf, u32 sub, u32 r[4]) { __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]); }
#endif

/*
 * The x86 processor as Linux's /proc/cpuinfo says it, from cpuid: its vendor, family, model and stepping, its name, and the
 * features Linux names, so that code choosing its routines by the file finds what the processor has (code that asks
 * cpuid itself gets the processor's own answer: the instruction is not the system's to answer).
 */
typedef struct x86_cpu {
    char vendor[13], name[49], flags[640];
    int family, model, stepping;
    u32 max_leaf;
} x86_cpu;

static void x86_cpu_read(x86_cpu *c)
{
    static const struct { u8 leaf, reg, bit; const char *name; } features[] = {
        /* leaf 1: edx (reg 3), ecx (reg 2); leaf 7: ebx (reg 1), ecx (reg 2), edx (reg 3); 0x81 (0x80000001): ecx, edx */
        { 1, 3, 0, "fpu" }, { 1, 3, 1, "vme" }, { 1, 3, 2, "de" }, { 1, 3, 3, "pse" }, { 1, 3, 4, "tsc" }, { 1, 3, 5, "msr" },
        { 1, 3, 6, "pae" }, { 1, 3, 7, "mce" }, { 1, 3, 8, "cx8" }, { 1, 3, 9, "apic" }, { 1, 3, 11, "sep" }, { 1, 3, 12, "mtrr" },
        { 1, 3, 13, "pge" }, { 1, 3, 14, "mca" }, { 1, 3, 15, "cmov" }, { 1, 3, 16, "pat" }, { 1, 3, 17, "pse36" },
        { 1, 3, 19, "clflush" }, { 1, 3, 23, "mmx" }, { 1, 3, 24, "fxsr" }, { 1, 3, 25, "sse" }, { 1, 3, 26, "sse2" },
        { 1, 3, 28, "ht" }, { 0x81, 3, 11, "syscall" }, { 0x81, 3, 20, "nx" }, { 0x81, 3, 27, "rdtscp" }, { 0x81, 3, 29, "lm" },
        { 1, 2, 0, "pni" }, { 1, 2, 1, "pclmulqdq" }, { 1, 2, 9, "ssse3" }, { 1, 2, 12, "fma" }, { 1, 2, 13, "cx16" },
        { 1, 2, 19, "sse4_1" }, { 1, 2, 20, "sse4_2" }, { 1, 2, 22, "movbe" }, { 1, 2, 23, "popcnt" }, { 1, 2, 25, "aes" },
        { 1, 2, 26, "xsave" }, { 1, 2, 28, "avx" }, { 1, 2, 29, "f16c" }, { 1, 2, 30, "rdrand" }, { 1, 2, 31, "hypervisor" },
        { 0x81, 2, 0, "lahf_lm" }, { 0x81, 2, 5, "abm" }, { 0x81, 2, 8, "3dnowprefetch" },
        { 7, 1, 0, "fsgsbase" }, { 7, 1, 3, "bmi1" }, { 7, 1, 5, "avx2" }, { 7, 1, 8, "bmi2" }, { 7, 1, 9, "erms" },
        { 7, 1, 16, "avx512f" }, { 7, 1, 17, "avx512dq" }, { 7, 1, 18, "rdseed" }, { 7, 1, 19, "adx" },
        { 7, 1, 23, "clflushopt" }, { 7, 1, 28, "avx512cd" }, { 7, 1, 29, "sha_ni" }, { 7, 1, 30, "avx512bw" },
        { 7, 1, 31, "avx512vl" }, { 7, 2, 9, "vaes" }, { 7, 2, 10, "vpclmulqdq" },
    };
    u32 r[4], l1[4] = { 0 }, l7[4] = { 0 }, x1[4] = { 0 }, max_ext;
    size_t i, n = 0;
    memset(c, 0, sizeof(*c));
    cpuid(0, 0, r);
    c->max_leaf = r[0];
    memcpy(c->vendor, &r[1], 4);
    memcpy(c->vendor + 4, &r[3], 4);
    memcpy(c->vendor + 8, &r[2], 4);
    if (c->max_leaf >= 1) cpuid(1, 0, l1);
    if (c->max_leaf >= 7) cpuid(7, 0, l7);
    cpuid(0x80000000u, 0, r);
    max_ext = r[0];
    if (max_ext >= 0x80000001u) cpuid(0x80000001u, 0, x1);
    if (max_ext >= 0x80000004u) {
        cpuid(0x80000002u, 0, r); memcpy(c->name, r, 16);
        cpuid(0x80000003u, 0, r); memcpy(c->name + 16, r, 16);
        cpuid(0x80000004u, 0, r); memcpy(c->name + 32, r, 16);
    }
    for (i = 0; c->name[i] == ' '; i++) {}
    if (i) memmove(c->name, c->name + i, strlen(c->name + i) + 1);
    if (!c->name[0]) mg_strlcpy(c->name, "x86 processor", sizeof(c->name));
    c->family = (int)((l1[0] >> 8) & 15);
    c->model = (int)((l1[0] >> 4) & 15);
    if (c->family == 15) c->family += (int)((l1[0] >> 20) & 255);
    if (c->family == 6 || c->family >= 15) c->model |= (int)((l1[0] >> 12) & 0xF0);
    c->stepping = (int)(l1[0] & 15);
    for (i = 0; i < MG_COUNT(features); i++) {
        const u32 *regs = features[i].leaf == 1 ? l1 : features[i].leaf == 7 ? l7 : x1;
        int len;
        if (!(regs[features[i].reg] >> features[i].bit & 1)) continue;
        len = snprintf(c->flags + n, sizeof(c->flags) - n, "%s%s", n ? " " : "", features[i].name);
        if (len < 0 || (size_t)len >= sizeof(c->flags) - n) break;
        n += (size_t)len;
    }
}
#endif

static char *synthetic(const char *g, size_t *size)
{
    textbuf t;
    int cpus = mg_cpu_count(), i;
    memset(&t, 0, sizeof(t));
    if (!strcmp(g, "/proc/cpuinfo")) {
        /* The processor's own, as Linux's file for it says it: what an app chooses its code by (NEON, sse4_2, asimd). */
#if MG_ARCH_X86_64 || MG_ARCH_X86
        x86_cpu c;
        x86_cpu_read(&c);
        for (i = 0; i < cpus; i++)
            tb_add(&t, "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %d\nmodel\t\t: %d\nmodel name\t: %s\nstepping\t: %d\n"
                       "cpu MHz\t\t: 2000.000\ncache size\t: 4096 KB\nphysical id\t: 0\nsiblings\t: %d\ncore id\t\t: %d\ncpu cores\t: %d\n"
                       "fpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\nflags\t\t: %s\nbogomips\t: 4000.00\n"
                       "clflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 39 bits physical, 48 bits virtual\n\n",
                   i, c.vendor, c.family, c.model, c.name, c.stepping, cpus, i, cpus, (unsigned)c.max_leaf, c.flags);
#elif MG_ARCH_ARM64
        /* (ARMv8's features every Windows on ARM device has: Advanced SIMD, the cryptographic extension, CRC32.) */
        for (i = 0; i < cpus; i++)
            tb_add(&t, "processor\t: %d\nBogoMIPS\t: 38.40\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid\n"
                       "CPU implementer\t: 0x51\nCPU architecture: 8\nCPU variant\t: 0xd\nCPU part\t: 0x805\nCPU revision\t: 14\n\n", i);
        tb_add(&t, "Hardware\t: Qualcomm Technologies, Inc\n");
#else
        /* (A processor with no SDIV is a Surface RT's Cortex-A9 (mg_cpu_arm_divide), which has no VFPv4 or LPAE either: an
           app that chose VFPv4's code by them would fault on it, Windows emulating no floating point, and one that chose
           SDIV would be trapped at every division. A phone's, and a Surface 2's Cortex-A15, have all three.) */
        const bool divide = mg_cpu_arm_divide();
        for (i = 0; i < cpus; i++)
            tb_add(&t, "processor\t: %d\nmodel name\t: ARMv7 Processor rev 4 (v7l)\nBogoMIPS\t: 38.40\n"
                       "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls %s\n"
                       "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: %s\nCPU revision\t: 4\n\n",
                   i, divide ? "vfpv4 idiva idivt vfpd32 lpae" : "vfpd32", divide ? "0xd03" : "0xc09");
        tb_add(&t, "Hardware\t: Qualcomm Technologies, Inc MSM\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n");
#endif
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
        maps_apk(&t, o.details);
        mg_ld_iterate(maps_one, &o);
        /* (With no library loaded yet, smaps still says the page size, of a region of the app's heap.) */
        if (o.details && !t.n) {
            tb_add(&t, "10000000-10001000 rw-p 00000000 00:00 0 [anon:libc_malloc]\n");
            smaps_details(&t, MG_PAGE, PF_R | PF_W);
        }
        if (!t.s) tb_add(&t, "%s", "");
    } else if (!strcmp(g, "/proc/self/auxv")) {
        /*
         * The auxiliary vector, as the kernel's file has it (bionic/mg_libc_misc.c, mg_bionic_auxv_words): the page size,
         * AT_RANDOM's 16 bytes, the hardware's capabilities, the vDSO. Go's runtime, loaded as a library on Android, reads
         * its vector from here, having none from its loader: without the file it probed the page size with mincore and
         * read its clocks by number, thousands of faults a second, where the vDSO's clock is a call.
         */
        gulong words[64];
        size_t n = mg_bionic_auxv_words(words, MG_COUNT(words));
        t.s = (char *)mg_realloc(NULL, n * sizeof(gulong) + 1);
        memcpy(t.s, words, n * sizeof(gulong));
        t.n = n * sizeof(gulong);
    } else if (!strcmp(g, "/proc/self/cmdline")) {
        tb_add(&t, "%s", g_package);
        t.n++;                 /* the terminating NUL is part of the file */
    } else if (!strcmp(g, "/proc/self/status")) {
        /* The memory lines as the kernel writes them: what the process uses (Windows' count of it) is its resident and
           anonymous size, and its high-water mark the most it has been seen to use. Google Play services' memory metric
           reads VmHWM, and "failed to find: VmHWM" when it was not there (package 576). */
        static volatile u32 hwm_kb;
        u32 rss = (u32)(mg_mem_usage() >> 10), hwm;
        if (rss > hwm_kb) hwm_kb = rss;
        hwm = hwm_kb > rss ? hwm_kb : rss;
        tb_add(&t, "Name:\t%.15s\nState:\tR (running)\nTgid:\t4242\nPid:\t4242\nPPid:\t1\nTracerPid:\t0\nUid:\t10100\t10100\t10100\t10100\n"
                   "Gid:\t10100\t10100\t10100\t10100\n", g_package);
        tb_add(&t, "VmPeak:\t%8u kB\nVmSize:\t%8u kB\nVmLck:\t%8u kB\nVmPin:\t%8u kB\nVmHWM:\t%8u kB\nVmRSS:\t%8u kB\n"
                   "RssAnon:\t%8u kB\nRssFile:\t%8u kB\nRssShmem:\t%8u kB\nVmData:\t%8u kB\nVmStk:\t%8u kB\nVmExe:\t%8u kB\n"
                   "VmLib:\t%8u kB\nVmPTE:\t%8u kB\nVmPMD:\t%8u kB\nVmSwap:\t%8u kB\nThreads:\t1\n",
               hwm, rss, 0u, 0u, hwm, rss, rss, 0u, 0u, rss, 136u, 20u, 0u, 0u, 0u, 0u);
    } else if (!strcmp(g, "/proc/stat")) {
        tb_add(&t, "cpu  100 0 100 10000 0 0 0 0 0 0\n");
        for (i = 0; i < cpus; i++) tb_add(&t, "cpu%d 100 0 100 10000 0 0 0 0 0 0\n", i);
    } else if (!strcmp(g, "/proc/sys/kernel/random/boot_id") || !strcmp(g, "/proc/sys/kernel/random/uuid")) {
        kernel_uuid(&t, !strcmp(g, "/proc/sys/kernel/random/boot_id"));
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

enum { OBJ_FILE = 1, OBJ_MEM, OBJ_NULL, OBJ_RANDOM, OBJ_PIPE_R, OBJ_PIPE_W, OBJ_EVENT, OBJ_LOG, OBJ_EPOLL, OBJ_DIRFD, OBJ_EXT, OBJ_TIMER, OBJ_INOTIFY };

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
    i32 seq;                       /* the object's io_seq when it was last reported */
    bool disarmed;                 /* reported once with EPOLLONESHOT */
} epoll_item;

typedef struct gobj {
    int type;
    /* Its holds: the descriptors that name it, the calls under way on it, a shared mapping's (mg_fd_hold). Changed atomically
       (mg_fd_held_ref adds one without the lock); the last one let go under the lock (fd_put). */
    volatile i32 refs;
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
    volatile i32 io_seq;           /* OBJ_EXT: the app's calls on it that have ended (epoll_check, EPOLLET) */
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
    /* inotify (OBJ_INOTIFY, below): its watches, the events not read yet (oldest first), their count and bytes as read() gives
       them, the number of its next watch, and the next inotify among them all (g_inotify) */
    struct ino_watch *ino_watches;
    struct ino_event *ino_first, *ino_last;
    u32 ino_count, ino_bytes;
    i32 ino_next_wd;
    struct gobj *ino_next;
} gobj;

#define G_O_ACCMODE 3
#define G_O_CREAT 0100
#define G_O_EXCL 0200
#define G_O_TRUNC 01000
#define G_O_APPEND 02000
#define G_O_NONBLOCK 04000
/* (The one of these that is the processor's: ARM's and arm64's 040000, the x86s' 0200000, where 040000 is O_DIRECT.) */
#define G_O_DIRECTORY (MG_ARCH_X86_64 || MG_ARCH_X86 ? 0200000 : 040000)
#define G_O_CLOEXEC 02000000

/* inotify's events and flags, as Linux numbers them (linux/inotify.h). */
#define G_IN_ACCESS 0x1u
#define G_IN_MODIFY 0x2u
#define G_IN_ATTRIB 0x4u
#define G_IN_CLOSE_WRITE 0x8u
#define G_IN_CLOSE_NOWRITE 0x10u
#define G_IN_OPEN 0x20u
#define G_IN_MOVED_FROM 0x40u
#define G_IN_MOVED_TO 0x80u
#define G_IN_CREATE 0x100u
#define G_IN_DELETE 0x200u
#define G_IN_DELETE_SELF 0x400u
#define G_IN_MOVE_SELF 0x800u
#define G_IN_ALL_EVENTS 0xFFFu
#define G_IN_Q_OVERFLOW 0x4000u
#define G_IN_IGNORED 0x8000u
#define G_IN_ONLYDIR 0x01000000u
#define G_IN_DONT_FOLLOW 0x02000000u
#define G_IN_EXCL_UNLINK 0x04000000u
#define G_IN_MASK_CREATE 0x10000000u
#define G_IN_MASK_ADD 0x20000000u
#define G_IN_ISDIR 0x40000000u
#define G_IN_ONESHOT 0x80000000u
/* The watches of every inotify there is: none, and the file calls look nothing up for them. */
static volatile i32 g_ino_watches;
static void ino_path(const char *g, u32 mask, u32 cookie, bool is_dir);
static void ino_self(const char *g, u32 mask, bool is_dir);
static void ino_destroy(struct gobj *o);

#define MAX_FDS 1024
static gobj *g_fds[MAX_FDS];
static u8 g_cloexec[MAX_FDS];
static mg_lock g_io_lock = MG_LOCK_INIT;
static bool g_io_inited;

/*
 * The descriptors' lock is never taken by a thread that holds the mappings' lock (mg_libc_mem.c). A thread that holds this
 * one and meets a page the app gave back with madvise -- a pipe's bytes copied into the app's buffer, epoll_wait's events
 * written into it, a select's sets or an epoll_ctl's event read from it -- is mended by the fault handler, which takes the
 * mappings' lock; a thread that held the mappings' lock and waited here meanwhile waited for good, and so did the other,
 * and every mmap, munmap, mprotect, madvise and descriptor of the app's after them. The write-back's hold of its file was
 * taken so, and mmap's hold of a shared file's descriptor. Should anything take it so again, the log says where at once,
 * and mg_fd_lock_taken_under_mappings counts it (the tests look).
 */
static volatile i32 g_io_under_mappings;
static void io_lock_wanted(const char *site)
{
    if (!mg_mem_lock_mine()) return;
    if (mg_atomic_add(&g_io_under_mappings, 1) == 1)
        mg_log(MG_LOG_ERROR, "io: the descriptors' lock is taken in %s by a thread that holds the mappings' lock: a thread that holds the "
               "descriptors' lock and meets a page given back waits for the mappings' lock, and the two may wait for each other for good", site);
}
#define mg_lock_acquire(l) ((l) == &g_io_lock ? (io_lock_wanted(__func__), mg_lock_acquire(l)) : mg_lock_acquire(l))
u32 mg_fd_lock_taken_under_mappings(void) { return (u32)g_io_under_mappings; }

/*
 * Who waits for what. Every thread that waits for descriptors -- poll, select and epoll_wait (wait_ready), a read or a
 * write of a pipe or an eventfd, the calls of a socket (bionic/mg_net.c, mg_fd_wait_locked) -- waits on a condition of
 * its own (mg_gthread.io_cond), and a change wakes those that wait on what changed: the objects their last look looked at
 * (ready_locked notes each), a pipe by either of its ends. A change of an object whose module announces its changes
 * without saying which (a socket: mg_fd_notify) wakes those that look at any such object.
 *
 * Every change woke every waiter, which each looked again at all of its own, in turn, under the one lock: 2000 bytes
 * passed back and forth between two threads over pipes took 87 ms with no other waiter and 1.76 s while 32 other threads
 * waited in epoll_wait each on an eventfd of its own, as an app's Loopers do (core/tests, t_wait_herd), and Play Store
 * downloaded apps at 270 KB/s, each of its QUIC packets of 1.3 KB waking all of its threads and Google Play services'
 * (package 575).
 */
#define IO_WATCH 32
/*
 * A wait that looks at an announcing object looks again after this at the latest: a change its module failed to announce
 * is seen within it, as the many other changes, which each woke every waiter, made it seen before. (Their callers work out
 * their own limits: a wait that ends so is one more look.)
 */
#define IO_ANNOUNCED_LOOK_NS 200000000ull
typedef struct io_waiter {
    struct io_waiter *prev, *next;     /* among g_waiters while it waits */
    mg_cond *cond;
    const void *watch[IO_WATCH];       /* what it waits on: an object, or for either end of a pipe its buffer */
    int nwatch;
    bool all;                          /* more than IO_WATCH, or not said: woken by every change */
    bool announced;                    /* among them an object whose module announces its changes (mg_fd_notify) */
} io_waiter;
static io_waiter *g_waiters;           /* those waiting now */
static io_waiter *g_looking;           /* the one whose look (wait_ready's check) is under way: ready_locked notes for it */

static const void *wake_key(const gobj *o) { return o->pipe ? (const void *)o->pipe : (const void *)o; }
static bool announces(const gobj *o) { return o->type == OBJ_EXT && o->ops && o->ops->announces; }

static void watch_note(io_waiter *w, const gobj *o)
{
    const void *k = wake_key(o);
    int i;
    if (announces(o)) w->announced = true;
    for (i = 0; i < w->nwatch; i++)
        if (w->watch[i] == k) return;
    if (w->nwatch < IO_WATCH) w->watch[w->nwatch++] = k;
    else w->all = true;
}

/* Wakes, with the lock held, those that wait on `o`; NULL: every waiter. */
static void wake_for(const gobj *o)
{
    const void *k = o ? wake_key(o) : NULL;
    io_waiter *w;
    int i;
    for (w = g_waiters; w; w = w->next) {
        bool hit = !o || w->all;
        for (i = 0; !hit && i < w->nwatch; i++) hit = w->watch[i] == k;
        if (hit) mg_cond_signal(w->cond);
    }
}

/* Wakes, with the lock held, those that look at an object whose module announces its changes. */
static void wake_announced(void)
{
    io_waiter *w;
    for (w = g_waiters; w; w = w->next)
        if (w->all || w->announced) mg_cond_signal(w->cond);
}

static void io_init(void)
{
    gobj *o;
    int i;
    if (g_io_inited) return;
    g_io_inited = true;
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
    if (o) mg_atomic_add(&o->refs, 1);
    mg_lock_release(&g_io_lock);
    return o;
}

static void obj_destroy(gobj *o)
{
    /* The last descriptor of an open file closed: what its watches are told (inotify, below), as Linux tells them when the
       file is released -- whether it was open for writing, and a directory's. */
    if (g_ino_watches && o->path && ((o->type == OBJ_FILE && o->file) || o->type == OBJ_DIRFD))
        ino_path(o->path, (o->flags & G_O_ACCMODE) ? G_IN_CLOSE_WRITE : G_IN_CLOSE_NOWRITE, 0, o->type == OBJ_DIRFD);
    if (o->type == OBJ_INOTIFY) ino_destroy(o);
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
    last = mg_atomic_add(&o->refs, -1) == 0;
    if (last && o->pipe) {
        p = o->pipe;
        if (o->type == OBJ_PIPE_R) p->readers--;
        else p->writers--;
        if (p->readers || p->writers) p = NULL;
        wake_for(o);
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

MG_HOSTCB void mg_fd_notify(void)
{
    mg_lock_acquire(&g_io_lock);
    wake_announced();
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

/*
 * The flags trace-files-<library>: each file the code of a library whose name starts with <library> opens or looks at
 * through the C library (open, openat, stat, lstat, fstatat, access, faccessat, readlink, opendir), said with the path and
 * what the call gave, at most 3000 lines a run. What a library that checks the device it runs on looks at: Google Play
 * services' DroidGuard opens /proc/cpuinfo so (its calls by number, /sys/fs/selinux/enforce, are trace-syscalls'). Known
 * by the call's return address, which is the library's where its code calls the C library directly, as on a 32-bit ARM
 * host; a call from another function of this layer is not said.
 */
const char *megler_data_dir(void);
const char *mg_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr, const void **lib_base);
static char g_files_traced[4][64];
static volatile i32 g_files_traced_n = -1, g_files_lines;

static void files_traced_add(const char *rest, void *user)
{
    (void)user;
    if (g_files_traced_n < (i32)MG_COUNT(g_files_traced) && *rest) mg_strlcpy(g_files_traced[g_files_traced_n++], rest, sizeof(g_files_traced[0]));
}

static void file_said(const char *what, const char *path, long result, const void *ret)
{
    const char *lib = NULL;
    const void *sym = NULL, *base = NULL;
    char err[24];
    i32 i;
    if (g_files_traced_n < 0) {
        if (!megler_data_dir()[0]) return;
        g_files_traced_n = 0;
        mg_flags_each("trace-files-", files_traced_add, NULL);
    }
    if (!g_files_traced_n || !ret) return;
    mg_ld_symbol_at(ret, &lib, &sym, &base);
    if (!lib) return;
    for (i = 0; i < g_files_traced_n; i++) {
        if (strncmp(lib, g_files_traced[i], strlen(g_files_traced[i]))) continue;
        err[0] = 0;
        if (result < 0) snprintf(err, sizeof(err), " (errno %d)", *mg_errno_ptr());
        if (mg_atomic_add(&g_files_lines, 1) <= 3000)
            mg_info("files: %s+0x%x %s(\"%s\") = %ld%s", lib, (unsigned)((const u8 *)ret - (const u8 *)base), what, path ? path : "(null)", result, err);
        return;
    }
}
/* Where the function this is written in returns to: called by a guest, an address in the guest's code (as mg_libc_mem.c's). */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define FILE_CALLER() _ReturnAddress()
#else
#define FILE_CALLER() __builtin_return_address(0)
#endif
#define FILE_SAID(what, path, result) file_said(what, path, (long)(result), FILE_CALLER())

/*
 * A file opened over and over: the first 200 opens of one path within 10 s are said once, with the Java frames of the
 * thread that opened it last (mg_core_thread_detail), as an app's loop shows itself (Google Play services read its module
 * configuration and rewrote ChimeraConfigService.xml a hundred times a second, on several threads, in every app). A few
 * paths at most are said; the table is by a hash of the path, and a collision only starts a count again.
 */
static void open_counted(const char *path)
{
    enum { SLOTS = 64, TIMES = 200, WINDOW_MS = 10000, SAID_MAX = 6 };
    static struct { volatile u32 hash, count, from, said; } slots[SLOTS];
    static volatile i32 said_total;
    u32 h = 2166136261u, now = (u32)(mg_time_monotonic_ns() / 1000000ull), i;
    const char *c;
    char frames[1500];
    for (c = path; *c; c++) h = (h ^ (u8)*c) * 16777619u;
    i = h % SLOTS;
    if (slots[i].hash != h || now - slots[i].from > WINDOW_MS) {
        slots[i].hash = h;
        slots[i].from = now;
        slots[i].count = 1;
        return;
    }
    if (++slots[i].count != TIMES || slots[i].said || mg_atomic_add(&said_total, 1) > SAID_MAX) return;
    slots[i].said = 1;
    mg_core_thread_detail(frames, sizeof(frames));
    mg_warn("open: %s opened %u times in %u ms%s", path, (unsigned)TIMES, (unsigned)(now - slots[i].from),
            frames[0] ? frames : " (not from Java)");
}

static int g_open(const char *path, int flags, int mode)
{
    char *g, *h;
    gobj *o;
    mg_stat st;
    int fd, err = 0, hf = 0, acc = flags & G_O_ACCMODE;
    (void)mode;
    mg_trace("open(%s, 0x%x)", path ? path : "(null)", (unsigned)flags);
    if (path && *path) open_counted(path);
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
        bool is_dir, asked = true, read_only = false, existed = true;
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
        } else if (mg_path_stat(h, &st, NULL)) {
            is_dir = st.is_dir;
            read_only = st.read_only;
        } else {
            /* (A directory that is only the way to a mount is one with nothing of the host's behind it.) */
            is_dir = leads_to_mount(g);
            existed = is_dir;
        }
        if (!why && !is_dir && (flags & G_O_DIRECTORY)) why = GE_NOTDIR;
        /* A file made read-only (chmod) is not opened for writing, whoever this process is on the host (root under qemu). */
        if (!why && !is_dir && acc && read_only) why = GE_ACCES;
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
        /* What its watches and its directory's are told (inotify, below): a file made, one emptied by O_TRUNC, the open. */
        if (g_ino_watches) {
            if (o->type == OBJ_FILE && (flags & G_O_CREAT) && !existed) ino_path(g, G_IN_CREATE, 0, false);
            else if (o->type == OBJ_FILE && (flags & G_O_TRUNC) && acc) ino_path(g, G_IN_MODIFY, 0, false);
            ino_path(g, G_IN_OPEN, 0, o->type == OBJ_DIRFD);
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
/* What _FORTIFY_SOURCE calls for an open whose flags need no mode: the same, with none. */
static int g_open_2(const char *path, int flags) { return g_open(path, flags, 0); }
static int g_openat_2(int dirfd, const char *path, int flags) { return g_openat(dirfd, path, flags, 0); }

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
 * Waits, with the lock held, for a change of what `w` waits on (who waits for what, above), on the thread's own
 * condition. A signal sent to the thread ends the wait, and its handler runs -- with the lock let go of -- before this
 * returns. Returns MG_FD_* (mg_fd.h): woken, the time (0: none) ran out, or a handler ran, and whether that one asked for
 * SA_RESTART.
 */
static int io_wait_as(io_waiter *w, u64 ns)
{
    mg_gthread *t = mg_gthread_self();
    i32 handled = t->sig_handled, unrestarted = t->sig_unrestarted;
    bool ok = true;
    if (!t->io_cond_ready) {
        mg_cond_init(&t->io_cond);
        t->io_cond_ready = true;
    }
    w->cond = &t->io_cond;
    w->prev = NULL;
    w->next = g_waiters;
    if (g_waiters) g_waiters->prev = w;
    g_waiters = w;
    t->sig_wait_cond = w->cond;
    if (!mg_signals_ready(t)) {
        if (ns) ok = mg_cond_wait_ns(w->cond, &g_io_lock, ns);
        else mg_cond_wait(w->cond, &g_io_lock);
    }
    t->sig_wait_cond = NULL;
    if (w->prev) w->prev->next = w->next;
    else g_waiters = w->next;
    if (w->next) w->next->prev = w->prev;
    if (mg_signals_ready(t)) {
        mg_lock_release(&g_io_lock);
        mg_signals_check();
        mg_lock_acquire(&g_io_lock);
    }
    if (t->sig_unrestarted != unrestarted) return MG_FD_INTERRUPTED;
    if (t->sig_handled != handled) return MG_FD_HANDLER_RAN;
    return ok ? MG_FD_WOKEN : MG_FD_TIMED_OUT;
}

/* io_wait_as for a change of one object (NULL: of anything), as a read or a write of a pipe or an eventfd waits. */
static int io_wait_for(const gobj *o, u64 ns)
{
    io_waiter w;
    w.nwatch = 0;
    w.all = !o;
    w.announced = false;
    if (o) watch_note(&w, o);
    return io_wait_as(&w, ns);
}

void mg_fd_lock(void)
{
    io_init();
    mg_lock_acquire(&g_io_lock);
}
void mg_fd_unlock(void) { mg_lock_release(&g_io_lock); }
void mg_fd_notify_locked(void) { wake_announced(); }
/* A socket's call waits for a change of a socket (bionic/mg_net.c), which its module announces without saying which. */
int mg_fd_wait_locked(u64 timeout_ns)
{
    io_waiter w;
    w.nwatch = 0;
    w.all = false;
    w.announced = true;
    if (!timeout_ns || timeout_ns > IO_ANNOUNCED_LOOK_NS) timeout_ns = IO_ANNOUNCED_LOOK_NS;
    return io_wait_as(&w, timeout_ns);
}

static bool shm_copy(gobj *o, u64 at, void *buf, size_t n, bool write);
static i32 ino_read(gobj *o, void *buf, u32 n);

static i32 obj_read(gobj *o, void *buf, u32 n, bool positioned, u64 offset)
{
    int err = 0;
    i64 r;
    switch (o->type) {
    case OBJ_FILE:
        r = positioned ? mg_file_pread(o->file, buf, n, offset, &err) : mg_file_read(o->file, buf, n, &err);
        if (r < 0) mg_set_errno(mg_errno_of(err));
        else if (r > 0 && g_ino_watches) ino_path(o->path, G_IN_ACCESS, 0, false);
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
            io_wait_for(o, 0);
        }
        while (got < n && p->count) {
            ((u8 *)buf)[got++] = p->data[p->head];
            p->head = (p->head + 1) % PIPE_CAP;
            p->count--;
        }
        wake_for(o);
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
            io_wait_for(o, 0);
        }
        v = o->semaphore ? 1 : o->counter;
        o->counter -= v;
        wake_for(o);
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
            io_wait_for(o, o->timer_next ? (o->timer_next > now ? o->timer_next - now : 1) : 0);
        }
        v = o->timer_ticks;
        o->timer_ticks = 0;
        mg_lock_release(&g_io_lock);
        memcpy(buf, &v, 8);
        return 8;
    }
    case OBJ_EXT:
        if (o->ops->read) {
            i32 got = o->ops->read(o->ext, buf, n, (o->flags & G_O_NONBLOCK) != 0);
            mg_atomic_add(&o->io_seq, 1);
            return got;
        }
        break;
    case OBJ_INOTIFY:
        return ino_read(o, buf, n);
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
        if (r < 0) {
            mg_set_errno(mg_errno_of(err));
        } else if (r > 0 && mg_mem_file_copies()) {
            /* What was written, into the copies of the file that are mapped shared (mg_mem_file_written): where it went. */
            i64 end = positioned ? (i64)offset + r : mg_file_seek(o->file, 0, 1, &err);
            if (end >= r) mg_mem_file_written(o, (u64)(end - r), buf, (size_t)r);
        }
        if (r > 0 && g_ino_watches) ino_path(o->path, G_IN_MODIFY, 0, false);
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
                wake_for(o);
                io_wait_for(o, 0);
                continue;
            }
            p->data[(p->head + p->count) % PIPE_CAP] = ((const u8 *)buf)[put++];
            p->count++;
        }
        wake_for(o);
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
        wake_for(o);
        mg_lock_release(&g_io_lock);
        return 8;
    }
    case OBJ_EXT:
        if (o->ops->write) {
            i32 put = o->ops->write(o->ext, buf, n, (o->flags & G_O_NONBLOCK) != 0);
            mg_atomic_add(&o->io_seq, 1);
            return put;
        }
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

/* (A count of more than MG_RW_MAX is a short read or write of that many, as on Linux: mg_rw_count.) */
static gssize g_read(int fd, void *buf, size_t n)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_read(o, buf, mg_rw_count(n), false, 0);
    fd_put(o);
    return r;
}

static gssize g_write(int fd, const void *buf, size_t n)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_write(o, buf, mg_rw_count(n), false, 0);
    fd_put(o);
    return r;
}

i32 mg_fd_read(int fd, void *buf, u32 n) { return (i32)g_read(fd, buf, n); }
i32 mg_fd_write(int fd, const void *buf, u32 n) { return (i32)g_write(fd, buf, n); }
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
/* A count alone, without the lock: `held` is held already, so it cannot go meanwhile, and the write-back asks under the
   mappings' lock, under which the descriptors' lock is not to be taken (io_lock_wanted). */
void *mg_fd_held_ref(void *held)
{
    gobj *o = (gobj *)held;
    if (!o) return NULL;
    mg_atomic_add(&o->refs, 1);
    return o;
}
/* (A shared mapping's own pages, which a write-back copies and writes with the mappings' lock let go: the file is written, and
   the other mappings of it are given the bytes by the one writing back, mg_libc_mem.c.) */
i32 mg_fd_held_pwrite(void *held, const void *buf, u32 n, u64 offset)
{
    gobj *o = (gobj *)held;
    int err = 0;
    i64 r;
    if (!o) return -1;
    if (o->type != OBJ_FILE) return obj_write(o, buf, n, true, offset);
    r = mg_file_pwrite(o->file, buf, n, offset, &err);
    if (r < 0) mg_set_errno(mg_errno_of(err));
    return (i32)r;
}
i32 mg_fd_held_pread(void *held, void *buf, u32 n, u64 offset)
{
    gobj *o = (gobj *)held;
    int err = 0;
    i64 r;
    if (!o) return -1;
    if (o->type != OBJ_FILE) return obj_read(o, buf, n, true, offset);
    r = mg_file_pread(o->file, buf, n, offset, &err);
    if (r < 0) mg_set_errno(mg_errno_of(err));
    return (i32)r;
}
bool mg_fd_held_same(const void *a, const void *b)
{
    const gobj *x = (const gobj *)a, *y = (const gobj *)b;
    return x == y || (x && y && x->type == OBJ_FILE && y->type == OBJ_FILE && x->path && y->path && !strcmp(x->path, y->path));
}
void mg_fd_let_go(void *held)
{
    if (held) fd_put((gobj *)held);
}
/* Data went through a held socket (send, recv, accept, and their kin in mg_net.c): epoll_check's EPOLLET reports it anew. Not
   for the calls that move none (getsockopt, setsockopt, names): an app that made one for each report would be told again
   and again. */
void mg_fd_moved(void *held)
{
    if (held && ((gobj *)held)->type == OBJ_EXT) mg_atomic_add(&((gobj *)held)->io_seq, 1);
}

/* (An off_t is a long: 32 bits in 32-bit Android, where pread's is taken as unsigned, as it always was here.) */
static gssize g_pread(int fd, void *buf, size_t n, goff offset) { return mg_fd_pread(fd, buf, mg_rw_count(n), (u64)(gulong)offset); }
static gssize g_pread64(int fd, void *buf, size_t n, i64 offset) { return mg_fd_pread(fd, buf, mg_rw_count(n), (u64)offset); }

static gssize g_pwrite64(int fd, const void *buf, size_t n, i64 offset)
{
    i32 r;
    WITH_FD(fd, o, -1)
    r = obj_write(o, buf, mg_rw_count(n), true, (u64)offset);
    fd_put(o);
    return r;
}
static gssize g_pwrite(int fd, const void *buf, size_t n, goff offset) { return g_pwrite64(fd, buf, n, (i64)(gulong)offset); }

typedef struct g_iovec {
    void *base;
    size_t len;
} g_iovec;

static gssize g_readv(int fd, const g_iovec *iov, int count)
{
    gssize total = 0;
    int i;
    for (i = 0; i < count; i++) {
        gssize r = g_read(fd, iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].len) break;
    }
    return total;
}

static gssize g_writev(int fd, const g_iovec *iov, int count)
{
    gssize total = 0;
    int i;
    for (i = 0; i < count; i++) {
        gssize r = g_write(fd, iov[i].base, iov[i].len);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].len) break;
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
    } else if (o->type == OBJ_DIRFD) {
        /* A directory's: where getdents64 goes on from, the start again (Go's rewinddir) or a d_off it gave. */
        if (whence == 0 && offset >= 0) {
            o->pos = (u64)offset;
            r = offset;
        } else if (whence == 1 && offset == 0) {
            r = (i64)o->pos;
        } else {
            mg_set_errno(GE_INVAL);
        }
    } else {
        mg_set_errno(GE_SPIPE);
    }
    fd_put(o);
    return r;
}

static goff g_lseek(int fd, goff offset, int whence)
{
    i64 r = mg_fd_seek(fd, offset, whence);
    /* A position past a 32-bit off_t (32-bit Android's) is EOVERFLOW, as there: lseek64 says it. */
    if (sizeof(goff) < sizeof(i64) && r > 0x7FFFFFFF) {
        mg_set_errno(GE_OVERFLOW);
        return -1;
    }
    return (goff)r;
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

void *mg_fd_shm_map(int fd, u64 offset, size_t len, bool code, bool *writable, void **base, size_t *mapped)
{
    void *p = NULL;
    WITH_FD(fd, o, NULL)
    if (o->type != OBJ_MEM || !o->shm) {
        mg_set_errno(GE_NODEV);
    } else if (!o->section || offset + len > MG_ALIGN_UP(o->mem_size, MG_PAGE) || (offset & (MG_PAGE - 1))) {
        mg_set_errno(GE_INVAL);
    } else {
        *writable = (o->shm_prot & 2) != 0;
        p = code ? mg_section_map_code(o->section, offset, MG_ALIGN_UP(len, MG_PAGE), base) : NULL;
        if (p) *writable = false;
        else p = mg_section_map(o->section, offset, MG_ALIGN_UP(len, MG_PAGE), *writable, base);
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
    if (!r && g_ino_watches) ino_path(o->path, G_IN_MODIFY, 0, false);
    fd_put(o);
    return r;
}
static int g_ftruncate(int fd, goff size) { return g_ftruncate64(fd, size); }

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
static int g_posix_fadvise(int fd, goff offset, goff len, int advice) { return g_posix_fadvise64(fd, offset, len, advice); }

static int g_fsync(int fd)
{
    WITH_FD(fd, o, -1)
    if (o->type == OBJ_FILE) mg_file_sync(o->file);
    fd_put(o);
    return 0;
}

/* ------------------------------------------------------------------- stat */

/*
 * bionic's struct stat (and stat64, the same), as each ABI lays it out: the kernel's stat64 on 32-bit ARM and on x86 (whose
 * 64-bit fields are 4-byte aligned, as the compiler aligns them for the core too), the generic one on arm64, x86_64's own.
 * (The 64-bit ABIs' longs are written i64 and u64 here: the same in their builds, and the same layout in MSVC's x64 one,
 * whose long is 32 bits.)
 */
#if MG_ARCH_X86_64
typedef struct g_stat {
    u64 st_dev, st_ino;
    u64 st_nlink;
    u32 st_mode, st_uid, st_gid, pad0;
    u64 st_rdev;
    i64 st_size;
    i64 st_blksize, st_blocks;
    i64 at_sec, at_nsec, mt_sec, mt_nsec, ct_sec, ct_nsec;
    i64 unused[3];
} g_stat;
#elif MG_ARCH_ARM64
typedef struct g_stat {
    u64 st_dev, st_ino;
    u32 st_mode, st_nlink, st_uid, st_gid;
    u64 st_rdev;
    u64 pad1;
    i64 st_size;
    i32 st_blksize, pad2;
    i64 st_blocks;
    i64 at_sec, at_nsec, mt_sec, mt_nsec, ct_sec, ct_nsec;
    u32 unused4, unused5;
} g_stat;
#else
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
    gtime at_sec;
    glong at_nsec;
    gtime mt_sec;
    glong mt_nsec;
    gtime ct_sec;
    glong ct_nsec;
    u64 st_ino;
} g_stat;
#endif

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
    out->st_ino = path_hash(gpath);
#if !MG_GUEST_LP64
    out->st_ino32 = (u32)out->st_ino;              /* (the kernel's stat64 has it twice) */
#endif
    /* (A file made read-only by chmod -- its write bits taken away, mg_path_set_writable -- says so.) */
    out->st_mode = st->is_dir ? (G_S_IFDIR | 0771) : (G_S_IFREG | (st->read_only ? 0440 : 0660));
    out->st_nlink = 1;
    out->st_uid = out->st_gid = 10100;
    out->st_size = (i64)st->size;
    out->st_blksize = 4096;
    out->st_blocks = (st->size + 511) / 512;
    out->at_sec = (gtime)(st->atime_ns / 1000000000ll);
    out->at_nsec = (glong)(st->atime_ns % 1000000000ll);
    out->mt_sec = (gtime)(st->mtime_ns / 1000000000ll);
    out->mt_nsec = (glong)(st->mtime_ns % 1000000000ll);
    out->ct_sec = (gtime)(st->ctime_ns / 1000000000ll);
    out->ct_nsec = (glong)(st->ctime_ns % 1000000000ll);
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

/*
 * Everything that is there can be read. What cannot be written is what lies in a link the shell marked so, and a file whose
 * mode has no write bit: one made read-only by chmod (File.setReadOnly), or one of the system's (/proc's, 0444). Facebook
 * makes each module it downloads read-only and checks File.canWrite before it loads one: "Downloaded module file must be read
 * only", and no language pack of its own ever loaded.
 */
static int access_of(const char *path, int mode, int r, const g_stat *st)
{
    if (r == 0 && (mode & 2)) {
        char *g = normalize(path);
        bool no = in_read_only(g) || (st && (st->st_mode & 0170000u) != G_S_IFDIR && !(st->st_mode & 0222u));
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
    return access_of(path, mode, g_stat_path(path, &st), &st);
}

static int g_faccessat(int dirfd, const char *path, int mode, int flags)
{
    g_stat st;
    int r = g_fstatat(dirfd, path, &st, flags);
    /* (A path relative to a directory other than the current one is not looked up again: such a one is only said to be there --
       but a file of it with no write bit is not writable.) */
    if (dirfd == -100 || (path && path[0] == '/')) return access_of(path, mode, r, &st);
    if (r == 0 && (mode & 2) && (st.st_mode & 0170000u) != G_S_IFDIR && !(st.st_mode & 0222u)) {
        mg_set_errno(GE_ACCES);
        return -1;
    }
    return r;
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
/* (The kernel's statfs64, whose words other than the counts and the id are longs: 32 bits in 32-bit Android, 64 in 64-bit.) */
typedef struct g_statfs {
    gulong f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    gulong f_namelen, f_frsize, f_flags, f_spare[4];
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

/* (Every field a long, and 24 bytes kept free at the end in 64-bit Android.) */
typedef struct g_statvfs {
    gulong f_bsize, f_frsize;
    gulong f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
#if MG_GUEST_LP64
    u32 reserved[6];
#endif
} g_statvfs;

/* A count of blocks as a long of the guest's: 32-bit Android's holds 16 TB in 4 KB blocks before it would not fit. */
static gulong statvfs_count(u64 n)
{
#if MG_GUEST_LP64
    return (gulong)n;
#else
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (gulong)n;
#endif
}

static int g_statvfs_fn(const char *path, g_statvfs *out)
{
    u64 blocks, free_blocks;
    (void)path;
    volume_blocks(&blocks, &free_blocks);
    memset(out, 0, sizeof(*out));
    out->f_bsize = out->f_frsize = 4096;
    out->f_blocks = statvfs_count(blocks);
    out->f_bfree = out->f_bavail = statvfs_count(free_blocks);
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

/* (`done`: what the watches are told once it is done, of the normalized path `g`: inotify, below.) */
#define PATH_OP(name, fixed, call, done)            \
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
        if (!h) {                                   \
            free(g);                                \
            mg_set_errno(why ? why : GE_ACCES);     \
            return -1;                              \
        }                                           \
        ok = call;                                  \
        free(h);                                    \
        if (!ok) {                                  \
            free(g);                                \
            mg_set_errno(mg_errno_of(err));         \
            return -1;                              \
        }                                           \
        if (g_ino_watches) done;                    \
        free(g);                                    \
        return 0;                                   \
    }

/*
 * A name removed, as Linux tells the watches: the file's own that its count of links went down (IN_ATTRIB), its
 * directory's that the name went (IN_DELETE), and its own that it is gone (IN_DELETE_SELF), which ends them (IN_IGNORED).
 */
static void ino_removed(const char *g, bool dir)
{
    if (!dir) ino_self(g, G_IN_ATTRIB, false);
    ino_path(g, G_IN_DELETE, 0, dir);
    ino_self(g, G_IN_DELETE_SELF, dir);
}

PATH_OP(g_unlink, GE_BUSY, mg_path_remove(h, &err), ino_removed(g, false))
PATH_OP(g_rmdir, GE_BUSY, mg_path_rmdir(h, &err), ino_removed(g, true))
PATH_OP(mkdir_host, GE_EXIST, mg_path_mkdir(h, &err), ino_path(g, G_IN_CREATE, 0, true))

static int g_mkdir(const char *path, int mode) { (void)mode; return mkdir_host(path); }
static int g_mkdirat(int dirfd, const char *path, int mode) { (void)dirfd; (void)mode; return mkdir_host(path); }
static int g_unlinkat(int dirfd, const char *path, int flags) { (void)dirfd; return (flags & 0x200) ? g_rmdir(path) : g_unlink(path); }

static int g_remove(const char *path)
{
    g_stat st;
    if (g_stat_path(path, &st) != 0) return -1;
    return (st.st_mode & G_S_IFDIR) ? g_rmdir(path) : g_unlink(path);
}

static void ino_renamed(const char *from, const char *to, bool dir, bool replaced);

static int g_rename(const char *from, const char *to)
{
    char *ga, *gb, *a, *b;
    int err = 0, why;
    bool ok, replaced = false;
    mg_stat st;
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
    /* (Whether a name is replaced, for the watches of what it named: asked only when there are watches.) */
    if (g_ino_watches && a && b && strcmp(ga, gb)) replaced = mg_path_stat(b, &st, NULL);
    /* (Between a link and anything else the host layer says EXDEV, as between two file systems: the app copies and removes.) */
    ok = a && b && mg_path_rename(a, b, &err);
    if (!ok) mg_set_errno(why ? why : a && b ? mg_errno_of(err) : GE_ACCES);
    if (ok && g_ino_watches && strcmp(ga, gb)) ino_renamed(ga, gb, mg_path_stat(b, &st, NULL) && st.is_dir, replaced);
    free(ga);
    free(gb);
    free(a);
    free(b);
    return ok ? 0 : -1;
}
static int g_renameat(int d1, const char *from, int d2, const char *to) { (void)d1; (void)d2; return g_rename(from, to); }

static int g_truncate(const char *path, goff size)
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

static gssize g_readlink(const char *path, char *buf, size_t n);

static char *g_realpath(const char *path, char *out)
{
    g_stat st;
    char *g, link[256];
    gssize k;
    int was;
    if (!path) {
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    /* The one link there is, the process's executable, is what it names (readlink, below), as on Android: there is no such file here. */
    was = *mg_errno_ptr();
    k = g_readlink(path, link, sizeof(link) - 1);
    if (k > 0) {
        link[k] = 0;
        if (!out) out = (char *)mg_g_malloc(4096);
        if (out) mg_strlcpy(out, link, 4096);
        return out;
    }
    *mg_errno_ptr() = was;
    if (g_stat_path(path, &st) != 0) return NULL;
    g = normalize(path);
    if (!out) out = (char *)mg_g_malloc(4096);
    if (out) mg_strlcpy(out, g, 4096);
    free(g);
    return out;
}

static gssize g_readlink(const char *path, char *buf, size_t n)
{
    /* The one link code reads: its own executable, the zygote of the ABI's width. */
    if (path && !strcmp(path, "/proc/self/exe")) {
        const char *exe = MG_GUEST_LP64 ? "/system/bin/app_process64" : "/system/bin/app_process32";
        size_t len = strlen(exe);
        if (len > n) len = n;
        memcpy(buf, exe, len);
        return (gssize)len;
    }
    mg_set_errno(GE_INVAL);
    return -1;
}

static int g_ok_fd(int fd, int a) { (void)fd; (void)a; return 0; }
static int g_chown(const char *path, int u, int g) { (void)path; (void)u; (void)g; return 0; }

static int g_fail_link(const char *a, const char *b)
{
    (void)a; (void)b;
    mg_set_errno(GE_PERM);
    return -1;
}
/*
 * And their *at forms, which Flutter's engine (dart:io) imports: as this file's other *at calls, the path as it is, its
 * directory descriptor not looked at. Each was missing, and its first call would have ended in the unresolved stub.
 */
static gssize g_readlinkat(int dirfd, const char *path, char *buf, size_t n) { (void)dirfd; return g_readlink(path, buf, n); }
static int g_symlinkat(const char *target, int dirfd, const char *path) { (void)dirfd; return g_fail_link(target, path); }

static int g_umask(int mask) { (void)mask; return 022; }
/* The checked forms a library built with _FORTIFY_SOURCE calls: the same, the buffer's size being the caller's business. */
static int g_umask_chk(int mask) { return g_umask(mask); }
static gssize g_pread64_chk(int fd, void *buf, size_t n, i64 offset, size_t buf_size)
{
    (void)buf_size;
    return g_pread64(fd, buf, n, offset);
}
static gssize g_pread_chk(int fd, void *buf, size_t n, goff offset, size_t buf_size)
{
    (void)buf_size;
    return g_pread(fd, buf, n, offset);
}
static gssize g_pwrite64_chk(int fd, const void *buf, size_t n, i64 offset, size_t buf_size)
{
    (void)buf_size;
    return g_pwrite64(fd, buf, n, offset);
}
static gssize g_readlink_chk(const char *path, char *buf, size_t n, size_t buf_size)
{
    (void)buf_size;
    return g_readlink(path, buf, n);
}

/*
 * sendfile: bytes from one descriptor to another. There is no kernel here to
 * move them without a copy, so they are read and written; with an offset the
 * source's own position is left where it was, as the call promises.
 */
static gssize send_file(int out_fd, int in_fd, i64 *offset, size_t n)
{
    char buf[4096];
    u32 done = 0, count = mg_rw_count(n);
    while (done < count) {
        u32 want = count - done < sizeof(buf) ? count - done : (u32)sizeof(buf);
        i32 got = offset ? mg_fd_pread(in_fd, buf, want, (u64)*offset) : mg_fd_read(in_fd, buf, want), put;
        if (got < 0) return done ? (gssize)done : -1;
        if (got == 0) break;
        put = mg_fd_write(out_fd, buf, (u32)got);
        if (put < 0) return done ? (gssize)done : -1;
        if (offset) *offset += put;
        else if (put < got) mg_fd_seek(in_fd, (i64)put - got, 1);    /* what was read and not taken is still to be read */
        done += (u32)put;
        if (put < got) break;
    }
    return (gssize)done;
}
static gssize g_sendfile64(int out_fd, int in_fd, i64 *offset, size_t count) { return send_file(out_fd, in_fd, offset, count); }
static gssize g_sendfile(int out_fd, int in_fd, goff *offset, size_t count)
{
    i64 at = offset ? *offset : 0;
    gssize n = send_file(out_fd, in_fd, offset ? &at : NULL, count);
    if (offset) *offset = (goff)at;
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
/* (The form of off_t's width, which bionic has beside it.) */
static int g_posix_fallocate(int fd, goff offset, goff len) { return g_posix_fallocate64(fd, offset, len); }
/*
 * fallocate, Linux's (bionic's since API 21): its plain kind, mode 0, is posix_fallocate's, answered through errno; any
 * other is refused with EOPNOTSUPP, as the system call is (mg_libc_misc.c, 352). MAX's libtracernative.so (VK's crash
 * reporter) imports it, and nothing provided it: its call would have said "done" and left the file as it was.
 */
static int g_fallocate64(int fd, int mode, i64 offset, i64 len)
{
    int err;
    if (mode != 0) {
        mg_set_errno(GE_OPNOTSUPP);
        return -1;
    }
    err = g_posix_fallocate64(fd, offset, len);
    if (err) {
        mg_set_errno(err);
        return -1;
    }
    return 0;
}
static int g_fallocate(int fd, int mode, goff offset, goff len) { return g_fallocate64(fd, mode, offset, len); }

/*
 * What this file system does not have: pipes spliced in the kernel, FIFOs and device files, extended attributes.
 * (A long -1, which is the int -1 too: splice's and getxattr's results are ssize_t.)
 */
static glong g_io_nosys(void) { mg_set_errno(GE_NOSYS); return -1; }
static int g_io_perm(void) { mg_set_errno(GE_PERM); return -1; }
static glong g_io_notsup(void) { mg_set_errno(GE_OPNOTSUPP); return -1; }
/* And no descriptor is a terminal. */
static int g_io_notty(void) { mg_set_errno(GE_NOTTY); return -1; }

/* pathconf: the limits of the one kind of file system there is. Those it has no figure for answer -1 and leave errno alone, which is how "no limit" is said. */
static glong g_pathconf(const char *path, int name)
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
static glong g_fpathconf(int fd, int name) { (void)fd; return g_pathconf("", name); }

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

static MG_HOSTCB void dir_add(const char *name, bool is_dir, void *user)
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

/*
 * getdents64: a directory's entries from a descriptor of it, in the kernel's records (struct linux_dirent64: d_ino, d_off,
 * d_reclen, d_type, then the name and its NUL, each record 8-byte aligned), from where the last call left off, as many as
 * fit; 0 at the end. Go's os package reads a directory so, by number (os.ReadDir: Psiphon's, in WhatsApp's libgojni.so).
 * The listing is made again for each call, as opendir makes it.
 */
static gssize g_getdents64(int fd, void *buf, size_t size)
{
    gobj *o = fd_get(fd);
    g_dir *d;
    u32 used = 0, count = mg_rw_count(size);
    bool more;
    if (!o) {
        mg_set_errno(GE_BADF);
        return -1;
    }
    if (o->type != OBJ_DIRFD || !o->path) {
        fd_put(o);
        mg_set_errno(GE_NOTDIR);
        return -1;
    }
    d = g_opendir(o->path);
    if (!d) {
        fd_put(o);
        return -1;
    }
    d->at = o->pos < d->count ? (u32)o->pos : d->count;
    while (d->at < d->count) {
        const char *name = d->names[d->at];
        size_t len = strlen(name);
        u32 reclen = (u32)MG_ALIGN_UP(19 + len + 1, 8);
        u8 *rec = (u8 *)buf + used;
        u64 ino = path_hash(name);
        i64 off = (i64)d->at + 1;
        u16 rl = (u16)reclen;
        if (used + reclen > count) break;
        memset(rec, 0, reclen);
        memcpy(rec, &ino, 8);
        memcpy(rec + 8, &off, 8);
        memcpy(rec + 16, &rl, 2);
        rec[18] = d->types[d->at];
        memcpy(rec + 19, name, len + 1);
        used += reclen;
        d->at++;
    }
    more = d->at < d->count;
    o->pos = d->at;
    g_closedir(d);
    fd_put(o);
    if (!used && more) {
        mg_set_errno(GE_INVAL);          /* not even one fits */
        return -1;
    }
    return (gssize)used;
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
    gtime interval_sec;
    glong interval_nsec;
    gtime value_sec;
    glong value_nsec;
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
    out->interval_sec = (gtime)(o->timer_interval / 1000000000ull);
    out->interval_nsec = (glong)(o->timer_interval % 1000000000ull);
    out->value_sec = (gtime)(left / 1000000000ull);
    out->value_nsec = (glong)(left % 1000000000ull);
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
    wake_for(o);
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

/* ---------------------------------------------------------------- inotify */

/*
 * inotify: watches on files and directories, and the events of what the app's own calls do to them. Every file call of its
 * code comes through here, its Java's (libcore's natives) as much as its native libraries', so what one thread of the app
 * writes and closes another is told of, as on Linux; what another process changes (another app, in the shared storage) is
 * not seen. A watch is of a path, not of an inode: a file moved takes its watches with it (IN_MOVE_SELF), one removed ends
 * them (IN_DELETE_SELF, then IN_IGNORED, at the unlink and not at the last close). Events are queued as Linux queues them:
 * one the same as the last one unread is not queued again, and past 16384 one IN_Q_OVERFLOW stands for the rest. A read
 * gives as many whole records as fit (struct inotify_event, the name padded to 16 bytes), EINVAL when not even the first
 * does, and waits for one unless the descriptor does not block; poll, select, epoll and FIONREAD see them.
 *
 * Firebase Cloud Messaging's C++ library (Jetpack Joyride's libmortargame.so, Firebase C++ SDK) reads the messages its Java
 * side writes into a file of the app's on a thread of its own, which waits in inotify for that file's IN_CLOSE_WRITE, and
 * asserts that inotify_init gave it a descriptor: with ENOSYS here every start ended in abort() ten seconds in, as its
 * messaging started ("F/firebase: file_descriptor >= 0", 0.1.0.681).
 */
typedef struct ino_event {
    struct ino_event *next;
    i32 wd;
    u32 mask, cookie, len;             /* len: the name's bytes in the record, its NUL and padding with it (0: none) */
    char name[1];
} ino_event;

typedef struct ino_watch {
    struct ino_watch *next;
    i32 wd;
    u32 mask;
    bool dir;
    char *path;                        /* normalized, as the file calls name what they do */
} ino_watch;

#define INO_QUEUE_MAX 16384u            /* Linux's /proc/sys/fs/inotify/max_queued_events */

static gobj *g_inotify;                /* every inotify there is, under the descriptors' lock */
static volatile i32 g_ino_cookie;      /* what ties a move's two events together */

/* An event queued for an inotify, with the lock held, its readers woken. */
static void ino_queue(gobj *o, i32 wd, u32 mask, u32 cookie, const char *name)
{
    ino_event *e, *last = o->ino_last;
    size_t n = name && *name ? strlen(name) : 0;
    if (last && last->wd == wd && last->mask == mask && last->cookie == cookie && !strcmp(last->name, n ? name : "")) return;
    if (o->ino_count >= INO_QUEUE_MAX) {
        if (last && last->mask == G_IN_Q_OVERFLOW) return;
        wd = -1;
        mask = G_IN_Q_OVERFLOW;
        cookie = 0;
        n = 0;
    }
    e = (ino_event *)mg_alloc(sizeof(*e) + n);
    e->wd = wd;
    e->mask = mask;
    e->cookie = cookie;
    e->len = n ? (u32)((n + 1 + 15) & ~(size_t)15) : 0;
    if (n) memcpy(e->name, name, n);
    if (last) last->next = e;
    else o->ino_first = e;
    o->ino_last = e;
    o->ino_count++;
    o->ino_bytes += 16 + e->len;
    wake_for(o);
}

/* A watch ended, with the lock held: taken off its inotify, which reads IN_IGNORED for it. */
static void ino_drop(gobj *o, ino_watch **at)
{
    ino_watch *w = *at;
    *at = w->next;
    ino_queue(o, w->wd, G_IN_IGNORED, 0, NULL);
    free(w->path);
    free(w);
    mg_atomic_add(&g_ino_watches, -1);
}

/*
 * What happened to `g`, told to the watches on its directory (the events of an entry, with its name) unless `self_only`,
 * and then to those on it (the events of its own in `mask`), in that order, as Linux queues the two: each told what of it
 * its mask takes. IN_DELETE_SELF ends the watches on it whatever their masks; IN_ONESHOT one once it has told something.
 */
static void ino_tell(const char *g, u32 mask, u32 cookie, bool is_dir, bool self_only)
{
    const u32 self_events = G_IN_ACCESS | G_IN_MODIFY | G_IN_ATTRIB | G_IN_CLOSE_WRITE | G_IN_CLOSE_NOWRITE | G_IN_OPEN | G_IN_DELETE_SELF |
                            G_IN_MOVE_SELF;
    const u32 entry_events = G_IN_ACCESS | G_IN_MODIFY | G_IN_ATTRIB | G_IN_CLOSE_WRITE | G_IN_CLOSE_NOWRITE | G_IN_OPEN | G_IN_MOVED_FROM |
                             G_IN_MOVED_TO | G_IN_CREATE | G_IN_DELETE;
    const char *slash, *name;
    size_t dir_len;
    gobj *o;
    int pass;
    if (!g || !g_ino_watches || !(slash = strrchr(g, '/'))) return;
    name = slash + 1;
    dir_len = (size_t)(slash - g);
    mg_lock_acquire(&g_io_lock);
    for (o = g_inotify; o; o = o->ino_next) {
        for (pass = self_only ? 1 : 0; pass < 2; pass++) {
            ino_watch **at = &o->ino_watches;
            while (*at) {
                ino_watch *w = *at;
                bool self = pass == 1 && !strcmp(w->path, g);
                bool entry = pass == 0 && w->dir && *name &&
                             (dir_len ? strlen(w->path) == dir_len && !strncmp(w->path, g, dir_len) : !strcmp(w->path, "/"));
                u32 hit = self ? (mask & self_events) : entry ? (mask & entry_events) : 0, told = hit & w->mask;
                if (told) {
                    /* (IN_ISDIR says a directory is what the event is of; Linux leaves it off a watched object's going.) */
                    u32 isdir = is_dir && !(told & (G_IN_DELETE_SELF | G_IN_MOVE_SELF)) ? G_IN_ISDIR : 0;
                    ino_queue(o, w->wd, told | isdir, entry ? cookie : 0, entry ? name : NULL);
                }
                if ((self && (hit & G_IN_DELETE_SELF)) || (told && (w->mask & G_IN_ONESHOT))) {
                    ino_drop(o, at);
                    continue;
                }
                at = &w->next;
            }
        }
    }
    mg_lock_release(&g_io_lock);
}

static void ino_path(const char *g, u32 mask, u32 cookie, bool is_dir) { ino_tell(g, mask, cookie, is_dir, false); }
static void ino_self(const char *g, u32 mask, bool is_dir) { ino_tell(g, mask, 0, is_dir, true); }

/*
 * A name moved from `from` to `to`: the watches on what `to` named before, if it was replaced, told it is gone; the old
 * directory's IN_MOVED_FROM and the new one's IN_MOVED_TO, one cookie for the two; the moved object's own IN_MOVE_SELF; and
 * every watch on it, or on anything below it for a directory, on the new path from now on.
 */
static void ino_renamed(const char *from, const char *to, bool dir, bool replaced)
{
    u32 cookie = (u32)mg_atomic_add(&g_ino_cookie, 1);
    size_t n = strlen(from);
    gobj *o;
    if (replaced) ino_self(to, G_IN_DELETE_SELF, dir);
    ino_path(from, G_IN_MOVED_FROM, cookie, dir);
    ino_path(to, G_IN_MOVED_TO, cookie, dir);
    ino_self(from, G_IN_MOVE_SELF, dir);
    mg_lock_acquire(&g_io_lock);
    for (o = g_inotify; o; o = o->ino_next) {
        ino_watch *w;
        for (w = o->ino_watches; w; w = w->next) {
            if (strncmp(w->path, from, n) || (w->path[n] && (!dir || w->path[n] != '/'))) continue;
            {
                char *moved = mg_sprintf("%s%s", to, w->path + n);
                free(w->path);
                w->path = moved;
            }
        }
    }
    mg_lock_release(&g_io_lock);
}

/* An inotify's last descriptor closed: its watches and events go with it. Never called with the lock held. */
static void ino_destroy(gobj *o)
{
    gobj **p;
    mg_lock_acquire(&g_io_lock);
    for (p = &g_inotify; *p; p = &(*p)->ino_next) {
        if (*p == o) {
            *p = o->ino_next;
            break;
        }
    }
    while (o->ino_watches) {
        ino_watch *w = o->ino_watches;
        o->ino_watches = w->next;
        free(w->path);
        free(w);
        mg_atomic_add(&g_ino_watches, -1);
    }
    while (o->ino_first) {
        ino_event *e = o->ino_first;
        o->ino_first = e->next;
        free(e);
    }
    o->ino_last = NULL;
    mg_lock_release(&g_io_lock);
}

/* inotify_init1: IN_NONBLOCK and IN_CLOEXEC, which are O_NONBLOCK's and O_CLOEXEC's bits; any other flag is EINVAL. */
static int g_inotify_init1(int flags)
{
    gobj *o;
    int fd;
    if (flags & ~(G_O_NONBLOCK | G_O_CLOEXEC)) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    io_init();
    o = (gobj *)mg_alloc(sizeof(*o));
    o->type = OBJ_INOTIFY;
    o->refs = 1;
    o->flags = flags & G_O_NONBLOCK;                 /* O_RDONLY */
    o->ino_next_wd = 1;
    /* (Among them all before its descriptor is: another thread closing that number at once finds it to take off again.) */
    mg_lock_acquire(&g_io_lock);
    fd = fd_alloc_locked(o, 3);
    if (fd >= 0) {
        o->ino_next = g_inotify;
        g_inotify = o;
        g_cloexec[fd] = (flags & G_O_CLOEXEC) != 0;
    }
    mg_lock_release(&g_io_lock);
    if (fd < 0) {
        free(o);
        mg_set_errno(GE_MFILE);
    }
    return fd;
}

static int g_inotify_init(void) { return g_inotify_init1(0); }

/* The inotify a descriptor is, held; NULL with EBADF, or EINVAL for another kind of descriptor. */
static gobj *inotify_of(int fd)
{
    gobj *o = fd_get(fd);
    if (!o) {
        mg_set_errno(GE_BADF);
        return NULL;
    }
    if (o->type != OBJ_INOTIFY) {
        fd_put(o);
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    return o;
}

/*
 * A watch on a path that is there (ENOENT), a directory where IN_ONLYDIR asks for one (ENOTDIR): its number, the same one
 * again for a path this inotify watches already, whose mask is then replaced, or added to with IN_MASK_ADD, or which
 * IN_MASK_CREATE refuses (EEXIST). A mask with no event in it is EINVAL, as are IN_MASK_ADD and IN_MASK_CREATE together.
 */
static int g_inotify_add_watch(int fd, const char *path, u32 mask)
{
    gobj *o;
    char *g, *h;
    bool there, dir = false;
    ino_watch *w;
    int wd = -1;
    mg_stat st;
    if (!(mask & G_IN_ALL_EVENTS) || ((mask & G_IN_MASK_ADD) && (mask & G_IN_MASK_CREATE))) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    if (!(o = inotify_of(fd))) return -1;
    if (!path || !*path) {
        fd_put(o);
        mg_set_errno(GE_NOENT);
        return -1;
    }
    g = normalize(path);
    h = host_of_normal(g);
    if (h) {
        there = mg_path_stat(h, &st, NULL);
        dir = there && st.is_dir;
    } else if (is_synthetic(g)) {
        size_t n = 0;
        char *text = synthetic(g, &n);
        there = text != NULL;
        free(text);
    } else {
        there = dir = leads_to_mount(g);
    }
    free(h);
    if (!there || ((mask & G_IN_ONLYDIR) && !dir)) {
        free(g);
        fd_put(o);
        mg_set_errno(!there ? GE_NOENT : GE_NOTDIR);
        return -1;
    }
    mg_lock_acquire(&g_io_lock);
    for (w = o->ino_watches; w && strcmp(w->path, g); w = w->next) {}
    if (w && (mask & G_IN_MASK_CREATE)) {
        mg_set_errno(GE_EXIST);
    } else if (w) {
        w->mask = (mask & G_IN_MASK_ADD) ? (w->mask | (mask & ~G_IN_MASK_ADD)) : mask;
        wd = w->wd;
    } else {
        w = (ino_watch *)mg_alloc(sizeof(*w));
        w->wd = wd = o->ino_next_wd++;
        w->mask = mask & ~(G_IN_MASK_ADD | G_IN_MASK_CREATE);
        w->dir = dir;
        w->path = g;
        g = NULL;
        w->next = o->ino_watches;
        o->ino_watches = w;
        mg_atomic_add(&g_ino_watches, 1);
    }
    mg_lock_release(&g_io_lock);
    free(g);
    fd_put(o);
    return wd;
}

static int g_inotify_rm_watch(int fd, int wd)
{
    gobj *o = inotify_of(fd);
    ino_watch **at;
    int r = -1;
    if (!o) return -1;
    mg_lock_acquire(&g_io_lock);
    for (at = &o->ino_watches; *at && (*at)->wd != wd; at = &(*at)->next) {}
    if (*at) {
        ino_drop(o, at);
        r = 0;
    } else {
        mg_set_errno(GE_INVAL);
    }
    mg_lock_release(&g_io_lock);
    fd_put(o);
    return r;
}

/* read() of an inotify: whole records, oldest first, as many as fit; with none queued, a wait unless it does not block. */
static i32 ino_read(gobj *o, void *buf, u32 n)
{
    u32 got = 0;
    mg_lock_acquire(&g_io_lock);
    while (!o->ino_first) {
        if (o->flags & G_O_NONBLOCK) {
            mg_lock_release(&g_io_lock);
            mg_set_errno(GE_AGAIN);
            return -1;
        }
        io_wait_for(o, 0);
    }
    while (o->ino_first && 16 + o->ino_first->len <= n - got) {
        ino_event *e = o->ino_first;
        u8 *p = (u8 *)buf + got;
        memcpy(p, &e->wd, 4);
        memcpy(p + 4, &e->mask, 4);
        memcpy(p + 8, &e->cookie, 4);
        memcpy(p + 12, &e->len, 4);
        if (e->len) {
            memset(p + 16, 0, e->len);
            memcpy(p + 16, e->name, strlen(e->name));
        }
        got += 16 + e->len;
        o->ino_first = e->next;
        if (!o->ino_first) o->ino_last = NULL;
        o->ino_count--;
        o->ino_bytes -= 16 + e->len;
        free(e);
    }
    mg_lock_release(&g_io_lock);
    if (!got) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    return (i32)got;
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

/*
 * fcntl's third argument is an int or a pointer, as the command has it, and is taken here as wide as a pointer: an int
 * is its low half (on the 64-bit ABIs a caller leaves the rest of the register as it was).
 */
static int g_fcntl(int fd, int cmd, uintptr_t parg)
{
    int r = 0, arg = (int)parg;
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
        if (!parg) {
            mg_set_errno(GE_FAULT);
            r = -1;
        } else {
            mg_wr16((u8 *)parg, 2);
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
/* (Two of them say a long's size in their number: _IOW(0x77, 3, size_t), _IOW(0x77, 5, unsigned long).) */
#define G_ASHMEM_SET_SIZE (0x40007703u | (u32)sizeof(size_t) << 16)
#define G_ASHMEM_SET_PROT_MASK (0x40007705u | (u32)sizeof(gulong) << 16)

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
    case G_ASHMEM_SET_SIZE: {
        size_t size = (size_t)(mg_addr)arg;
        if (size > 0x40000000u) break;
        return shm_resize(o, size) ? 0 : -1;
    }
    case 0x7704: return (int)o->mem_size;          /* ASHMEM_GET_SIZE */
    case G_ASHMEM_SET_PROT_MASK:
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
        else if (o->type == OBJ_INOTIFY) n = (int)o->ino_bytes;   /* the records queued, as read() would give them */
        mg_lock_release(&g_io_lock);
        if (o->type == OBJ_EXT && o->ops->readable) n = o->ops->readable(o->ext);
        if (o->type == OBJ_FILE) {
            /* A file's: what is left of it after the position, as Linux answers it for a regular file (do_vfs_ioctl). Android's
               FileInputStream.available() is this ioctl (IoBridge.available), and Geekbench 5 reads a result into an array of
               available()'s size: it read 0 bytes of its 47 KB result, and showed an empty result page and an empty history
               (the HP Elite x3, 689). */
            mg_stat st;
            int err = 0;
            i64 at = mg_file_seek(o->file, 0, 1, &err);
            if (at >= 0 && mg_file_stat(o->file, &st) && st.size > (u64)at)
                n = st.size - (u64)at > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)(st.size - (u64)at);
        }
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
    if (g_looking) watch_note(g_looking, o);
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
    case OBJ_INOTIFY:
        if (o->ino_first) r |= G_POLLIN;
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
 * signal mask while waiting (ppoll, pselect, epoll_pwait), a sigset_t: an
 * unsigned long, whose signals 1 to 32 are what is kept of it here.
 */
typedef int (*ready_check)(void *ctx, bool *looks_itself);

static int wait_ready(ready_check check, void *ctx, int timeout_ms, bool interruptible, const gulong *mask)
{
    u64 deadline = timeout_ms > 0 ? mg_time_monotonic_ns() + (u64)timeout_ms * 1000000ull : 0;
    mg_gthread *t = mask ? mg_gthread_self() : NULL;
    u32 saved = 0;
    int result;
    io_init();
    if (t) {
        saved = t->sig_blocked;
        t->sig_blocked = (u32)*mask;
    }
    mg_lock_acquire(&g_io_lock);
    for (;;) {
        bool looks_itself = false;
        int hits, w;
        u64 left = 0, wake;
        io_waiter me;
        g_timer_wake = 0;
        /* What the look looks at is what the wait is for (who waits for what, above). */
        me.nwatch = 0;
        me.all = me.announced = false;
        g_looking = &me;
        hits = check(ctx, &looks_itself);
        g_looking = NULL;
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
        if (me.announced && (!left || left > IO_ANNOUNCED_LOOK_NS)) left = IO_ANNOUNCED_LOOK_NS;
        w = io_wait_as(&me, left);
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

/* A struct timespec's or timeval's seconds and fraction as milliseconds, rounded up; -2 when they are not a time. */
static int timeout_of(gtime sec, glong frac, i64 per_ms)
{
    i64 ms;
    if (sec < 0 || frac < 0 || frac >= per_ms * 1000) return -2;
    if (sec > 0x7FFFFFFF / 1000) return 0x7FFFFFFF;
    ms = (i64)sec * 1000 + ((i64)frac + per_ms - 1) / per_ms;
    return ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)ms;
}

static int g_ppoll(mg_pollfd *fds, unsigned count, const g_timespec *ts, const gulong *mask)
{
    poll_ctx c;
    int ms = ts ? timeout_of(ts->tv_sec, ts->tv_nsec, 1000000) : -1;
    if (ms == -2) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    c.fds = fds;
    c.count = (int)count;
    return wait_ready(poll_check, &c, ms, true, mask);
}

/*
 * select: bionic's fd_set is 1024 bits in longs, and on a little-endian processor descriptor n is bit n % 32 of the
 * 32-bit word n / 32 whatever a long's width, which is how they are read here; what is cleared of them is whole longs,
 * as Linux clears them. Readable, writable and exceptional are what Linux makes of poll's bits.
 */
static int select_wait(int nfds, u32 *rd, u32 *wr, u32 *ex, int timeout_ms, const gulong *mask)
{
    mg_pollfd *p;
    poll_ctx c;
    int n = 0, i, hits, total = 0;
    size_t bytes = (size_t)((nfds + 8 * sizeof(gulong) - 1) / (8 * sizeof(gulong))) * sizeof(gulong);
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
    if (rd) memset(rd, 0, bytes);
    if (wr) memset(wr, 0, bytes);
    if (ex) memset(ex, 0, bytes);
    for (i = 0; i < n && hits > 0; i++) {
        int fd = p[i].fd;
        if (rd && (p[i].revents & (G_POLLIN | G_POLLHUP | G_POLLERR)) && (p[i].events & G_POLLIN)) { rd[fd / 32] |= 1u << (fd % 32); total++; }
        if (wr && (p[i].revents & (G_POLLOUT | G_POLLERR)) && (p[i].events & G_POLLOUT)) { wr[fd / 32] |= 1u << (fd % 32); total++; }
        if (ex && (p[i].revents & G_POLLPRI) && (p[i].events & G_POLLPRI)) { ex[fd / 32] |= 1u << (fd % 32); total++; }
    }
    free(p);
    return total;
}

static int g_select(int nfds, u32 *rd, u32 *wr, u32 *ex, g_timeval *tv)
{
    u64 began = mg_time_monotonic_ns();
    int ms = tv ? timeout_of(tv->tv_sec, tv->tv_usec, 1000) : -1, r = select_wait(nfds, rd, wr, ex, ms, NULL);
    if (tv && ms >= 0 && r >= 0) {
        /* As Linux does: the time that was left. */
        u64 spent = (mg_time_monotonic_ns() - began) / 1000ull, all = (u64)ms * 1000ull, left = spent < all ? all - spent : 0;
        tv->tv_sec = (gtime)(left / 1000000ull);
        tv->tv_usec = (glong)(left % 1000000ull);
    }
    return r;
}

static int g_pselect(int nfds, u32 *rd, u32 *wr, u32 *ex, const g_timespec *ts, const gulong *mask)
{
    return select_wait(nfds, rd, wr, ex, ts ? timeout_of(ts->tv_sec, ts->tv_nsec, 1000000) : -1, mask);
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
/* bionic's struct epoll_event: packed on x86_64 alone, as the kernel's is there (12 bytes; 16 on ARM, 12 on x86). */
#if MG_ARCH_X86_64
#pragma pack(push, 1)
#endif
typedef struct g_epoll_event {
    u32 events;
    u64 data;
} g_epoll_event;
#if MG_ARCH_X86_64
#pragma pack(pop)
#endif

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
    wake_for(o);
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
    if (g_looking) watch_note(g_looking, ep);      /* epoll_ctl changes it */
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
        /* Read or written since its report: it may have been emptied and filled again between two looks, a new edge on
           Linux. Reported again -- an edge-triggered app takes one report too many, never one too few. */
        if (it->seq != o->io_seq) it->last = 0;
        fresh = (it->events & G_EPOLLET) ? now & ~it->last : now;
        it->last &= now;
        if (!fresh) continue;
        it->last = now;
        it->seq = o->io_seq;
        c->out[got].events = (u32)now;
        c->out[got].data = it->data;
        got++;
        if (it->events & G_EPOLLONESHOT) it->disarmed = true;
    }
    if (n) ep->epoll_next = (ep->epoll_next + i) % n;
    return got;
}

static int g_epoll_pwait(int epfd, g_epoll_event *out, int max, int timeout_ms, const gulong *mask)
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
/* The counter's eight bytes read or written, 0 or -1, as bionic's eventfd_read and eventfd_write (Zalo's libz3rdparty.so). */
static int g_eventfd_read(int fd, u64 *value) { return g_read(fd, value, 8) == 8 ? 0 : -1; }
static int g_eventfd_write(int fd, u64 value) { return g_write(fd, &value, 8) == 8 ? 0 : -1; }
/*
 * chmod, fchmodat and fchmod: of a file's mode its owner's write bit is kept (mg_path_set_writable: Windows' read-only
 * attribute, the owner's write bit on a POSIX host), which stat, access and open then go by; the rest of the mode is not
 * kept, and a directory's not at all. File.setReadOnly and setWritable come here: Facebook makes each module it downloads read-only and
 * loads only one that is ("Downloaded module file must be read only"), and Android 14 loads code only from a file so. A file
 * that is not there is ENOENT; one of the system's (made up here) and a link the shell marked read-only are left as they
 * are. libc++'s std::filesystem::permissions calls fchmodat (Zalo's libc++_shared.so and libzinstant.so).
 */
static int chmod_normal(const char *g, unsigned mode)
{
    char *h;
    int err = 0;
    bool ok;
    if (!strncmp(g, "/dev/", 5) || is_synthetic(g)) return 0;
    h = host_of_normal(g);
    if (!h) {
        if (leads_to_mount(g)) return 0;
        mg_set_errno(GE_NOENT);
        return -1;
    }
    /* (The owner's write bit: the app owns its files, and Linux gives an owner without it no writing, whatever the group's.) */
    ok = in_read_only(g) || mg_path_set_writable(h, (mode & 0200u) != 0, &err);
    if (ok && g_ino_watches) {
        mg_stat st;
        ino_path(g, G_IN_ATTRIB, 0, mg_path_stat(h, &st, NULL) && st.is_dir);
    }
    free(h);
    if (!ok) {
        mg_set_errno(mg_errno_of(err));
        return -1;
    }
    return 0;
}

static int g_chmod(const char *path, unsigned mode)
{
    char *g;
    int r;
    if (!path || !*path) {
        mg_set_errno(GE_NOENT);
        return -1;
    }
    g = normalize(path);
    r = chmod_normal(g, mode);
    free(g);
    return r;
}

static int g_fchmodat(int dirfd, const char *path, unsigned mode, int flags)
{
    (void)flags;
    /* AT_FDCWD is -100. Relative to another directory: join the two paths, as openat does. */
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
        r = g_chmod(full, mode);
        free(full);
        return r;
    }
    return g_chmod(path, mode);
}

/*
 * The times of a file set, as utimensat, utimes, utime and the rest of their kind set them (mg_path_set_times): they were
 * answered and nothing done, and an app was told they were -- java.io.File.setLastModified said true and lastModified()
 * gave the old time, and a file copied with its time kept (a backup restored, a gallery's photo) had the time of the copy.
 * The guest's times are its own longs (its time_t and long: 32 bits on 32-bit ARM and x86, 64 on 64-bit), each pair
 * the last access first, then the last change; utimensat's UTIME_NOW and UTIME_OMIT, and none at all for both now. A
 * folder of the phone's own is left as it is, and a file in a link to read only refused (EACCES), as its writing is.
 */
#define G_UTIME_NOW ((1l << 30) - 1l)
#define G_UTIME_OMIT ((1l << 30) - 2l)

static i64 guest_long(const void *p)
{
    i64 wide;
    i32 narrow;
    if (MG_GUEST_LP64) {
        memcpy(&wide, p, 8);
        return wide;
    }
    memcpy(&narrow, p, 4);
    return narrow;
}

static int times_normal(const char *g, i64 a, i64 m)
{
    char *h;
    int err = 0;
    bool ok;
    if (!strncmp(g, "/dev/", 5) || is_synthetic(g)) return 0;
    h = host_of_normal(g);
    if (!h) {
        if (leads_to_mount(g)) return 0;
        mg_set_errno(GE_NOENT);
        return -1;
    }
    if (in_read_only(g)) {
        ok = false;
        err = MG_E_ACCESS;
    } else {
        ok = mg_path_set_times(h, a, m, &err);
    }
    if (ok && g_ino_watches) {
        mg_stat st;
        ino_path(g, G_IN_ATTRIB, 0, mg_path_stat(h, &st, NULL) && st.is_dir);
    }
    free(h);
    if (!ok) {
        mg_set_errno(mg_errno_of(err));
        return -1;
    }
    return 0;
}

/* The times set of `path`, relative to the directory `dirfd` as openat takes it, or of `dirfd`'s own file where `path` is NULL. */
static int set_times(int dirfd, const char *path, i64 a, i64 m)
{
    char *g;
    int r;
    if (!path) {
        gobj *o = fd_get(dirfd);
        r = -1;
        if (!o) mg_set_errno(GE_BADF);
        else if (!o->path) mg_set_errno(GE_INVAL);
        else r = times_normal(o->path, a, m);
        if (o) fd_put(o);
        return r;
    }
    if (!*path) {
        mg_set_errno(GE_NOENT);
        return -1;
    }
    if (dirfd != -100 && path[0] != '/') {
        gobj *d = fd_get(dirfd);
        char *full;
        if (!d || !d->path) {
            if (d) fd_put(d);
            mg_set_errno(GE_BADF);
            return -1;
        }
        full = mg_sprintf("%s/%s", d->path, path);
        fd_put(d);
        g = normalize(full);
        free(full);
    } else {
        g = normalize(path);
    }
    r = times_normal(g, a, m);
    free(g);
    return r;
}

/* A pair of timespecs (or of timevals, `micro`), the guest's, as nanoseconds: now for none, and UTIME_NOW and UTIME_OMIT. */
static void guest_times(const void *times, bool micro, i64 *a, i64 *m)
{
    const u8 *p = (const u8 *)times;
    size_t word = MG_GUEST_LP64 ? 8 : 4;
    int i;
    i64 *out[2] = { a, m };
    for (i = 0; i < 2; i++) {
        i64 sec, frac;
        if (!p) {
            *out[i] = (i64)mg_time_realtime_ns();
            continue;
        }
        sec = guest_long(p + 2 * word * (size_t)i);
        frac = guest_long(p + 2 * word * (size_t)i + word);
        if (!micro && frac == G_UTIME_NOW) *out[i] = (i64)mg_time_realtime_ns();
        else if (!micro && frac == G_UTIME_OMIT) *out[i] = MG_TIME_KEEP;
        else *out[i] = sec * 1000000000ll + (micro ? frac * 1000ll : frac);
    }
}

static int g_utimensat(int dirfd, const char *path, const void *times, int flags)
{
    i64 a, m;
    (void)flags;
    guest_times(times, false, &a, &m);
    return set_times(dirfd, path, a, m);
}
static int g_futimens(int fd, const void *times) { return g_utimensat(fd, NULL, times, 0); }
static int g_utimes(const char *path, const void *times)
{
    i64 a, m;
    guest_times(times, true, &a, &m);
    return set_times(-100, path, a, m);
}
static int g_futimes(int fd, const void *times)
{
    i64 a, m;
    guest_times(times, true, &a, &m);
    return set_times(fd, NULL, a, m);
}
/* (And futimesat, which libcore's natives look up by name: Yandex Maps' log said it was not there.) */
static int g_futimesat(int dirfd, const char *path, const void *times)
{
    i64 a, m;
    guest_times(times, true, &a, &m);
    return set_times(path ? dirfd : -100, path, a, m);
}
/* utime: a struct utimbuf, two time_t of whole seconds. */
static int g_utime(const char *path, const void *times)
{
    i64 a, m;
    if (!times) {
        a = m = (i64)mg_time_realtime_ns();
    } else {
        size_t word = MG_GUEST_LP64 ? 8 : 4;
        a = guest_long(times) * 1000000000ll;
        m = guest_long((const u8 *)times + word) * 1000000000ll;
    }
    return set_times(-100, path, a, m);
}

static int g_fchmod(int fd, int mode)
{
    int r = 0;
    WITH_FD(fd, o, -1)
    if (o->type == OBJ_FILE && o->path) r = chmod_normal(o->path, (unsigned)mode);
    fd_put(o);
    return r;
}
static int g_flock(int fd, int op) { (void)fd; (void)op; return 0; }
static void g_sync(void) { }

/* The calls that name a file, as guest code makes them: each said under the flags trace-files-<library> (file_said). */
static int g_open_said(const char *path, int flags, int mode) { int r = g_open(path, flags, mode); FILE_SAID("open", path, r); return r; }
static int g_open_2_said(const char *path, int flags) { int r = g_open_2(path, flags); FILE_SAID("open", path, r); return r; }
static int g_openat_said(int dirfd, const char *path, int flags, int mode) { int r = g_openat(dirfd, path, flags, mode); FILE_SAID("openat", path, r); return r; }
static int g_openat_2_said(int dirfd, const char *path, int flags) { int r = g_openat_2(dirfd, path, flags); FILE_SAID("openat", path, r); return r; }
static int g_stat_said(const char *path, g_stat *out) { int r = g_stat_path(path, out); FILE_SAID("stat", path, r); return r; }
static int g_fstatat_said(int dirfd, const char *path, g_stat *out, int flags) { int r = g_fstatat(dirfd, path, out, flags); FILE_SAID("fstatat", path, r); return r; }
static int g_access_said(const char *path, int mode) { int r = g_access(path, mode); FILE_SAID("access", path, r); return r; }
static int g_faccessat_said(int dirfd, const char *path, int mode, int flags) { int r = g_faccessat(dirfd, path, mode, flags); FILE_SAID("faccessat", path, r); return r; }
static gssize g_readlink_said(const char *path, char *buf, size_t n) { gssize r = g_readlink(path, buf, n); FILE_SAID("readlink", path, r); return r; }
static gssize g_readlinkat_said(int dirfd, const char *path, char *buf, size_t n) { gssize r = g_readlinkat(dirfd, path, buf, n); FILE_SAID("readlinkat", path, r); return r; }
static g_dir *g_opendir_said(const char *path) { g_dir *d = g_opendir(path); FILE_SAID("opendir", path, d ? 0 : -1); return d; }

#define IO(n, f) MG_WRAP(n, f)

const mg_sym mg_tab_io[] = {
    IO("open", g_open_said), IO("open64", g_open_said), IO("__open_2", g_open_2_said), IO("openat", g_openat_said), IO("openat64", g_openat_said),
    IO("__openat_2", g_openat_2_said), IO("creat", g_creat), IO("creat64", g_creat), IO("close", g_close),
    IO("read", g_read), IO("write", g_write), IO("__read_chk", g_read), IO("__write_chk", g_write),
    IO("pread", g_pread), IO("pread64", g_pread64), IO("pwrite", g_pwrite), IO("pwrite64", g_pwrite64),
    IO("__pread_chk", g_pread_chk), IO("__pread64_chk", g_pread64_chk), IO("__umask_chk", g_umask_chk),
    IO("__pwrite64_chk", g_pwrite64_chk), IO("__readlink_chk", g_readlink_chk),
    IO("sendfile", g_sendfile), IO("sendfile64", g_sendfile64), IO("posix_fallocate64", g_posix_fallocate64), IO("splice", g_io_nosys),
    IO("mkfifo", g_io_perm), IO("mknod", g_io_perm), IO("pathconf", g_pathconf), IO("fpathconf", g_fpathconf),
    IO("getxattr", g_io_notsup), IO("setxattr", g_io_notsup), IO("listxattr", g_io_notsup), IO("removexattr", g_io_notsup),
    IO("inotify_init", g_inotify_init), IO("inotify_init1", g_inotify_init1), IO("inotify_add_watch", g_inotify_add_watch),
    IO("inotify_rm_watch", g_inotify_rm_watch),
    /* (close_range is for a child about to exec, and no child is started here.) */
    IO("close_range", g_io_nosys),
    IO("tcgetattr", g_io_notty), IO("tcsetattr", g_io_notty), IO("tcdrain", g_io_notty), IO("tcsendbreak", g_io_notty),
    IO("readv", g_readv), IO("writev", g_writev), IO("lseek", g_lseek), IO("lseek64", g_lseek64),
    IO("ftruncate", g_ftruncate), IO("ftruncate64", g_ftruncate64), IO("truncate", g_truncate),
    IO("posix_fadvise", g_posix_fadvise), IO("posix_fadvise64", g_posix_fadvise64), IO("posix_fallocate", g_posix_fallocate), IO("fallocate", g_fallocate), IO("fallocate64", g_fallocate64),
    IO("fsync", g_fsync), IO("fdatasync", g_fsync), IO("sync", g_sync),
    IO("stat", g_stat_said), IO("stat64", g_stat_said), IO("lstat", g_stat_said), IO("lstat64", g_stat_said),
    IO("fstat", g_fstat), IO("fstat64", g_fstat), IO("fstatat", g_fstatat_said), IO("fstatat64", g_fstatat_said),
    IO("access", g_access_said), IO("faccessat", g_faccessat_said),
    IO("statfs", g_statfs_fn), IO("statfs64", g_statfs_fn), IO("fstatfs", g_fstatfs_fn), IO("fstatfs64", g_fstatfs_fn),
    IO("statvfs", g_statvfs_fn), IO("fstatvfs", g_fstatvfs_fn),
    /* (With 32-bit counts on a 32-bit Android, as the plain ones: the two structures are the same there.) */
    IO("statvfs64", g_statvfs_fn), IO("fstatvfs64", g_fstatvfs_fn),
    IO("unlink", g_unlink), IO("unlinkat", g_unlinkat), IO("rmdir", g_rmdir), IO("mkdir", g_mkdir), IO("mkdirat", g_mkdirat),
    IO("remove", g_remove), IO("rename", g_rename), IO("renameat", g_renameat),
    IO("chdir", g_chdir), IO("getcwd", g_getcwd), IO("realpath", g_realpath), IO("readlink", g_readlink_said),
    IO("chmod", g_chmod), IO("fchmod", g_fchmod), IO("chown", g_chown), IO("lchown", g_chown), IO("utime", g_utime),
    IO("utimes", g_utimes), IO("lutimes", g_utimes), IO("futimes", g_futimes), IO("link", g_fail_link), IO("symlink", g_fail_link),
    IO("readlinkat", g_readlinkat_said), IO("symlinkat", g_symlinkat), IO("utimensat", g_utimensat), IO("futimens", g_futimens), IO("futimesat", g_futimesat),
    IO("umask", g_umask), IO("flock", g_flock),
    IO("fchown", g_ok_fd),
    IO("opendir", g_opendir_said), IO("fdopendir", g_fdopendir), IO("readdir", g_readdir), IO("readdir64", g_readdir),
    IO("readdir_r", g_readdir_r), IO("readdir64_r", g_readdir_r), IO("closedir", g_closedir), IO("rewinddir", g_rewinddir),
    IO("telldir", g_telldir), IO("seekdir", g_seekdir), IO("dirfd", g_dirfd), IO("scandir", g_scandir), IO("scandir64", g_scandir),
    IO("alphasort", g_alphasort), IO("alphasort64", g_alphasort), IO("getdents64", g_getdents64),
    IO("pipe", g_pipe), IO("pipe2", g_pipe2), IO("eventfd", g_eventfd), IO("memfd_create", g_memfd_create),
    IO("eventfd_read", g_eventfd_read), IO("eventfd_write", g_eventfd_write), IO("fchmodat", g_fchmodat),
    IO("timerfd_create", g_timerfd_create), IO("timerfd_settime", g_timerfd_settime), IO("timerfd_gettime", g_timerfd_gettime), IO("dup", g_dup), IO("dup2", g_dup2), IO("dup3", g_dup3),
    IO("fcntl", g_fcntl), IO("__fcntl64", g_fcntl), IO("ioctl", g_ioctl), IO("isatty", g_isatty),
    IO("poll", g_poll), IO("__poll_chk", g_poll), IO("ppoll", g_ppoll), IO("__ppoll_chk", g_ppoll), IO("select", g_select),
    IO("pselect", g_pselect), IO("epoll_create", g_epoll_create), IO("epoll_create1", g_epoll_create1), IO("epoll_ctl", g_epoll_ctl),
    IO("epoll_wait", g_epoll_wait), IO("epoll_pwait", g_epoll_pwait),
    MG_END
};
