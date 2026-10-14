/* The rest of libc: process, environment, sorting, random numbers, signals, locales, system configuration. */
#include "hk_bionic.h"

#include "../hk_core.h"
#include "../ld/hk_ld.h"

#include <stdio.h>

#include "hk_fd.h"

long hk_g_futex(volatile i32 *addr, int op, int val, const void *timeout);
void *hk_g_mmap2(void *addr, size_t len, int prot, int flags, int fd, u32 pgoffset);

/* ---------------------------------------------------------------- process */

static u32 g_stack_chk_guard;

static void g_stack_chk_fail(void) { hk_fatal("guest: stack corruption detected (__stack_chk_fail)"); }

typedef struct exit_fn {
    hk_addr fn;
    void *arg, *dso;
    bool with_arg;
} exit_fn;
static exit_fn *g_exit_fns;
static int g_nexit, g_exit_cap;
static hk_lock g_exit_lock = HK_LOCK_INIT;

static int add_exit(hk_addr fn, void *arg, void *dso, bool with_arg)
{
    hk_lock_acquire(&g_exit_lock);
    if (g_nexit == g_exit_cap) {
        g_exit_cap = g_exit_cap ? g_exit_cap * 2 : 64;
        g_exit_fns = (exit_fn *)hk_realloc(g_exit_fns, sizeof(exit_fn) * (size_t)g_exit_cap);
    }
    g_exit_fns[g_nexit].fn = fn;
    g_exit_fns[g_nexit].arg = arg;
    g_exit_fns[g_nexit].dso = dso;
    g_exit_fns[g_nexit].with_arg = with_arg;
    g_nexit++;
    hk_lock_release(&g_exit_lock);
    return 0;
}

static int g_atexit(hk_addr fn) { return add_exit(fn, NULL, NULL, false); }
static int g_cxa_atexit(hk_addr fn, void *arg, void *dso) { return add_exit(fn, arg, dso, true); }
/* Thread-local destructors: run at process exit here, not at the thread's. */
static int g_cxa_thread_atexit(hk_addr fn, void *arg, void *dso) { return add_exit(fn, arg, dso, true); }
static int g_aeabi_atexit(void *arg, hk_addr fn, void *dso) { return add_exit(fn, arg, dso, true); }
/* Libraries are never unloaded, so there is nothing to finalise early. */
static void g_cxa_finalize(void *dso) { (void)dso; }

static void run_exit_fns(void)
{
    for (;;) {
        exit_fn e;
        hk_lock_acquire(&g_exit_lock);
        if (!g_nexit) {
            hk_lock_release(&g_exit_lock);
            return;
        }
        e = g_exit_fns[--g_nexit];
        hk_lock_release(&g_exit_lock);
        if (e.with_arg) HK_GFN(void (*)(void *), e.fn)(e.arg);
        else HK_GFN(void (*)(void), e.fn)();
    }
}

static void g_exit(int status)
{
    hk_info("guest: exit(%d)", status);
    if (hk_guest_exit_hook) hk_guest_exit_hook(status);
    run_exit_fns();
    exit(status);
}

static void g__exit(int status)
{
    hk_info("guest: _exit(%d)", status);
    if (hk_guest_exit_hook) hk_guest_exit_hook(status);
    _Exit(status);
}

static void g_abort(void)
{
    hk_err("guest: abort()");
    if (hk_guest_exit_hook) hk_guest_exit_hook(134);
    abort();
}

static void g_assert(const char *file, int line, const char *expr) { hk_fatal("guest: %s:%d: assertion \"%s\" failed", file, line, expr); }
static void g_assert2(const char *file, int line, const char *fn, const char *expr)
{
    hk_fatal("guest: %s:%d: %s: assertion \"%s\" failed", file, line, fn, expr);
}
static void g_cxa_pure_virtual(void) { hk_fatal("guest: pure virtual function called"); }
static void g_cxa_bad_cast(void) { hk_fatal("guest: bad cast"); }

/* C++ function-local statics: the guard's first byte says "initialised", its second "in progress". */
static int g_cxa_guard_acquire(volatile i32 *guard)
{
    for (;;) {
        i32 v = *guard;
        if (v & 1) return 0;
        if (v & 0x100) {
            hk_gwait(guard, v, 10000000ull);
            continue;
        }
        if (hk_atomic_cas(guard, v, v | 0x100) == v) return 1;
    }
}
static void g_cxa_guard_release(volatile i32 *guard)
{
    hk_atomic_xchg(guard, 1);
    hk_futex_wake(guard, true);
}
static void g_cxa_guard_abort(volatile i32 *guard)
{
    hk_atomic_xchg(guard, 0);
    hk_futex_wake(guard, true);
}

static int g_getpid(void) { return 4242; }
static int g_getppid(void) { return 1; }
static int g_getuid(void) { return 10100; }
static int g_fail_nosys(void)
{
    hk_set_errno(GE_NOSYS);
    return -1;
}
static int g_zero(void) { return 0; }

/* ------------------------------------------------------------ environment */

static char **g_environ;
static int g_nenv, g_env_cap;
static hk_lock g_env_lock = HK_LOCK_INIT;

static int env_find(const char *name, size_t n)
{
    int i;
    for (i = 0; i < g_nenv; i++)
        if (!strncmp(g_environ[i], name, n) && g_environ[i][n] == '=') return i;
    return -1;
}

static void env_reserve(void)
{
    if (g_nenv + 2 > g_env_cap) {
        g_env_cap = g_env_cap ? g_env_cap * 2 : 32;
        g_environ = (char **)hk_realloc(g_environ, sizeof(char *) * (size_t)g_env_cap);
    }
}

static int g_setenv(const char *name, const char *value, int overwrite)
{
    size_t n;
    int i;
    char *entry;
    if (!name || !*name || strchr(name, '=')) {
        hk_set_errno(GE_INVAL);
        return -1;
    }
    n = strlen(name);
    hk_lock_acquire(&g_env_lock);
    i = env_find(name, n);
    if (i >= 0 && !overwrite) {
        hk_lock_release(&g_env_lock);
        return 0;
    }
    /* The old string is not freed: getenv hands out pointers into it. */
    entry = hk_sprintf("%s=%s", name, value ? value : "");
    if (i >= 0) g_environ[i] = entry;
    else {
        env_reserve();
        g_environ[g_nenv++] = entry;
        g_environ[g_nenv] = NULL;
    }
    hk_lock_release(&g_env_lock);
    return 0;
}

static char *g_getenv(const char *name)
{
    int i;
    char *v = NULL;
    if (!name) return NULL;
    hk_lock_acquire(&g_env_lock);
    i = env_find(name, strlen(name));
    if (i >= 0) v = g_environ[i] + strlen(name) + 1;
    hk_lock_release(&g_env_lock);
    return v;
}

static int g_unsetenv(const char *name)
{
    int i;
    hk_lock_acquire(&g_env_lock);
    i = env_find(name, strlen(name));
    if (i >= 0) {
        g_environ[i] = g_environ[--g_nenv];
        g_environ[g_nenv] = NULL;
    }
    hk_lock_release(&g_env_lock);
    return 0;
}

static int g_putenv(char *entry)
{
    const char *eq = strchr(entry, '=');
    char *name;
    int r;
    if (!eq) return g_unsetenv(entry);
    name = hk_strdup(entry);
    name[eq - entry] = 0;
    r = g_setenv(name, eq + 1, 1);
    free(name);
    return r;
}

static int g_clearenv(void)
{
    hk_lock_acquire(&g_env_lock);
    g_nenv = 0;
    if (g_environ) g_environ[0] = NULL;
    hk_lock_release(&g_env_lock);
    return 0;
}

void hk_env_init(void)
{
    env_reserve();
    g_environ[0] = NULL;
    g_setenv("PATH", "/sbin:/system/sbin:/system/bin:/system/xbin", 1);
    g_setenv("ANDROID_ROOT", "/system", 1);
    g_setenv("ANDROID_DATA", "/data", 1);
    g_setenv("EXTERNAL_STORAGE", hk_fs_external_dir(), 1);
    g_setenv("HOME", hk_fs_internal_dir(), 1);
    g_setenv("TMPDIR", "/data/local/tmp", 1);
    g_setenv("LANG", "en_US.UTF-8", 1);
}

/* ------------------------------------------------------- the system, asked */

static long g_sysconf(int name)
{
    switch (name) {
    case 0x0000: return 131072;                    /* _SC_ARG_MAX */
    case 0x0002: return 100;                       /* _SC_CLK_TCK */
    case 0x0004: return 1024;                      /* _SC_OPEN_MAX */
    case 0x0006: return 100;                       /* _SC_CLK_TCK in older headers */
    case 0x0027: case 0x0028: return HK_PAGE;      /* _SC_PAGESIZE, _SC_PAGE_SIZE */
    case 0x0060: case 0x0061: return hk_cpu_count();   /* _SC_NPROCESSORS_CONF, _ONLN */
    case 0x0062: {                                 /* _SC_PHYS_PAGES */
        u64 limit = hk_mem_limit();
        return (long)((limit ? limit : 1024ull * 1024 * 1024) / HK_PAGE);
    }
    case 0x0063: {                                 /* _SC_AVPHYS_PAGES */
        u64 limit = hk_mem_limit(), used = hk_mem_usage();
        if (!limit) limit = 1024ull * 1024 * 1024;
        return (long)((limit > used ? limit - used : 0) / HK_PAGE);
    }
    case 0x0047: case 0x0048: return 1024;         /* _SC_GETGR_R_SIZE_MAX, _SC_GETPW_R_SIZE_MAX */
    case 0x004C: return 8192;                      /* _SC_THREAD_STACK_MIN */
    case 0x0050: return 64;                        /* _SC_THREAD_KEYS_MAX-ish */
    default:
        hk_set_errno(GE_INVAL);
        return -1;
    }
}

static int g_getpagesize(void) { return HK_PAGE; }

typedef struct g_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
} g_utsname;

static int g_uname(g_utsname *u)
{
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Linux");
    strcpy(u->nodename, "localhost");
    strcpy(u->release, "4.4.0-megler");
    strcpy(u->version, "#1 SMP PREEMPT");
    strcpy(u->machine, "armv7l");
    strcpy(u->domainname, "localdomain");
    return 0;
}

static int g_gethostname(char *buf, size_t n)
{
    hk_strlcpy(buf, "localhost", n);
    return 0;
}

static int g_property_get(const char *name, char *value)
{
    const char *v = hk_sysprop(name);
    if (!v) {
        value[0] = 0;
        return 0;
    }
    hk_strlcpy(value, v, 92);                      /* PROP_VALUE_MAX */
    return (int)strlen(value);
}

/* A "prop_info" is just the name here. */
static const void *g_property_find(const char *name) { return hk_sysprop(name) ? hk_strdup(name) : NULL; }
static int g_property_read(const void *pi, char *name, char *value)
{
    if (name) hk_strlcpy(name, (const char *)pi, 32);
    return g_property_get((const char *)pi, value);
}
static void g_property_read_callback(const void *pi, hk_addr cb, void *cookie)
{
    const char *v = hk_sysprop((const char *)pi);
    HK_GFN(void (*)(void *, const char *, const char *, u32), cb)(cookie, (const char *)pi, v ? v : "", 0);
}
static int g_property_set(const char *name, const char *value)
{
    hk_sysprop_set(name, value);
    return 0;
}

static u8 g_at_random[16];

static unsigned long g_getauxval(unsigned long type)
{
    switch (type) {
    case 6: return HK_PAGE;                        /* AT_PAGESZ */
    case 15: return (unsigned long)(hk_addr)"v7l"; /* AT_PLATFORM */
    case 16:                                       /* AT_HWCAP: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 */
        return 2u | 4u | 16u | 64u | 128u | 4096u | 8192u | 32768u | 65536u | (1u << 17) | (1u << 18) | (1u << 19);
    case 25: return (unsigned long)(hk_addr)g_at_random;   /* AT_RANDOM */
    case 26: return 0;                             /* AT_HWCAP2: no ARMv8 crypto claimed */
    default: return 0;
    }
}

static int g_prctl(int option, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
    (void)b; (void)c; (void)d;
    if (option == 15 && a) hk_strlcpy(hk_gthread_self()->name, (const char *)(hk_addr)a, sizeof(hk_gthread_self()->name));   /* PR_SET_NAME */
    if (option == 16 && a) hk_strlcpy((char *)(hk_addr)a, hk_gthread_self()->name, 16);                                    /* PR_GET_NAME */
    return 0;
}

static int g_cacheflush(long start, long end, long flags)
{
    (void)flags;
    if (end > start) hk_icache_flush((void *)(hk_addr)start, (size_t)(end - start));
    return 0;
}

/*
 * rt_sigaction by number: the kernel's struct (handler, flags, restorer, a 64-bit mask) to bionic's 32-bit sigaction
 * (handler, a 32-bit mask, flags, restorer) and back. The mask's real-time half is not kept: sigaction here has none.
 */
static long rt_sigaction(int sig, const u32 *act, u32 *old, size_t mask_size)
{
    typedef int (*sigaction_fn)(int, const u32 *, u32 *);
    u32 in[4], out[4];
    int r;
    if (mask_size != 8) {
        hk_set_errno(GE_INVAL);
        return -1;
    }
    if (act) {
        in[0] = act[0];
        in[1] = act[3];
        in[2] = act[1];
        in[3] = act[2];
    }
    r = ((sigaction_fn)hk_bionic_find("sigaction"))(sig, act ? in : NULL, old ? out : NULL);
    if (r == 0 && old) {
        old[0] = out[0];
        old[1] = out[2];
        old[2] = out[3];
        old[3] = out[1];
        old[4] = 0;
    }
    return r;
}

/*
 * A handler for a fault's signal asked for by system call, past libc's sigaction: refused. A fault in guest code is this
 * layer's to repair or report, never delivered to the guest (see "signals" below), and the code that goes past libc
 * checks that it is: Gecko's mozglue installs its SIGSEGV handler with syscall(__NR_sigaction) and then writes to a page
 * of its own with no access, to see its handler called. Refused, it goes on without the handler, as it does where a
 * kernel will not have it. libc's sigaction keeps taking such handlers, as it always has.
 */
static bool fault_signal_refused(long sig, long act)
{
    if (!act || (sig != 4 && sig != 7 && sig != 8 && sig != 11)) return false;
    hk_note_once("a handler for signal %ld asked for by system call is refused: faults are not delivered to guest handlers here", sig);
    hk_set_errno(GE_NOSYS);
    return true;
}

/*
 * syscall(): the handful that code calls by number. ARM EABI numbers. Gecko's mozglue installs its signal handlers with
 * syscall(__NR_sigaction) (67), past libc's sigaction, whose old struct is bionic's 32-bit one.
 */
static long g_syscall(long nr, long a, long b, long c, long d, long e, long f)
{
    typedef int (*gettime_fn)(int, void *);
    typedef int (*sigaction_fn)(int, const void *, void *);
    typedef int (*sigaltstack_fn)(const void *, void *);
    typedef int (*memfd_fn)(const char *, unsigned);
    switch (nr) {
    case 67:                                                                                                  /* sigaction */
        if (fault_signal_refused(a, b)) return -1;
        return ((sigaction_fn)hk_bionic_find("sigaction"))((int)a, (const void *)(hk_addr)b, (void *)(hk_addr)c);
    case 174:                                                                                                 /* rt_sigaction */
        if (fault_signal_refused(a, b)) return -1;
        return rt_sigaction((int)a, (const u32 *)(hk_addr)b, (u32 *)(hk_addr)c, (size_t)d);
    case 186: return ((sigaltstack_fn)hk_bionic_find("sigaltstack"))((const void *)(hk_addr)a, (void *)(hk_addr)b);  /* sigaltstack */
    case 385: return ((memfd_fn)hk_bionic_find("memfd_create"))((const char *)(hk_addr)a, (unsigned)b);           /* memfd_create */
    case 20: return g_getpid();                                            /* getpid */
    case 224: return hk_gthread_self()->tid;                               /* gettid */
    case 240: return hk_g_futex((volatile i32 *)(hk_addr)a, (int)b, (int)c, (const void *)(hk_addr)d);   /* futex */
    case 263: return ((gettime_fn)hk_bionic_find("clock_gettime"))((int)a, (void *)(hk_addr)b);          /* clock_gettime */
    case 192: return (long)(hk_addr)hk_g_mmap2((void *)(hk_addr)a, (size_t)b, (int)c, (int)d, (int)e, (u32)f);   /* mmap2 */
    case 384: hk_random((void *)(hk_addr)a, (size_t)b); return b;          /* getrandom */
    case 158: hk_thread_yield(); return 0;                                 /* sched_yield */
    case 238: case 268: return 0;                                          /* tkill, tgkill */
    case 0xF0002: return g_cacheflush(a, b, c);                            /* __ARM_NR_cacheflush */
    case 345: return 0;                                                    /* getcpu */
    default:
        hk_note_once("syscall(%ld) is not supported", nr);
        hk_set_errno(GE_NOSYS);
        return -1;
    }
}

typedef struct g_rlimit {
    u32 cur, max;
} g_rlimit;

static int g_getrlimit(int resource, g_rlimit *r)
{
    /* RLIMIT_STACK 3, RLIMIT_NOFILE 7; everything else unlimited */
    if (resource == 3) r->cur = r->max = 8u * 1024 * 1024;
    else if (resource == 7) r->cur = r->max = 1024;
    else r->cur = r->max = 0xFFFFFFFFu;
    return 0;
}
static int g_setrlimit(int resource, const g_rlimit *r) { (void)resource; (void)r; return 0; }
static int g_getrusage(int who, void *usage)
{
    (void)who;
    memset(usage, 0, 72);
    return 0;
}
static int g_getpriority(int which, int who) { (void)which; (void)who; return 0; }
static int g_setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
static int g_nice(int inc) { (void)inc; return 0; }

/* ------------------------------------------------------- sorting, searching */

static void swap_bytes(u8 *a, u8 *b, size_t size)
{
    while (size--) {
        u8 t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

typedef int (*cmp_fn)(const void *, const void *);

static void sort_range(u8 *base, size_t n, size_t size, cmp_fn cmp)
{
    /* Quicksort, the smaller part by recursion and the larger by looping; small runs by insertion. */
    while (n > 12) {
        u8 *lo = base, *hi = base + (n - 1) * size, *mid = base + (n / 2) * size, *i, *j;
        size_t left;
        if (cmp(mid, lo) < 0) swap_bytes(mid, lo, size);
        if (cmp(hi, lo) < 0) swap_bytes(hi, lo, size);
        if (cmp(hi, mid) < 0) swap_bytes(hi, mid, size);
        /* The median of three goes just before the end and is the pivot. */
        swap_bytes(mid, hi - size, size);
        mid = hi - size;
        i = lo;
        j = hi - size;
        for (;;) {
            do i += size; while (cmp(i, mid) < 0);
            do j -= size; while (j > lo && cmp(j, mid) > 0);
            if (i >= j) break;
            swap_bytes(i, j, size);
        }
        swap_bytes(i, hi - size, size);
        left = (size_t)(i - base) / size;
        if (left < n - left - 1) {
            sort_range(base, left, size, cmp);
            base = i + size;
            n = n - left - 1;
        } else {
            sort_range(i + size, n - left - 1, size, cmp);
            n = left;
        }
    }
    {
        size_t a, b;
        for (a = 1; a < n; a++)
            for (b = a; b > 0 && cmp(base + (b - 1) * size, base + b * size) > 0; b--) swap_bytes(base + (b - 1) * size, base + b * size, size);
    }
}

static void g_qsort(void *base, size_t n, size_t size, hk_addr cmp)
{
    if (n > 1 && size) sort_range((u8 *)base, n, size, HK_GFN(cmp_fn, cmp));
}

static void *g_bsearch(const void *key, const void *base, size_t n, size_t size, hk_addr cmp)
{
    cmp_fn f = HK_GFN(cmp_fn, cmp);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const u8 *p = (const u8 *)base + mid * size;
        int c = f(key, p);
        if (!c) return (void *)p;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* ---------------------------------------------------------- random numbers */

static u32 g_rand_state = 1;
static int g_rand_r(u32 *state)
{
    *state = *state * 1103515245u + 12345u;
    return (int)((*state >> 1) & 0x7FFFFFFF);
}
static int g_rand(void) { return g_rand_r(&g_rand_state); }
static void g_srand(unsigned seed) { g_rand_state = seed; }
static long g_random(void) { return g_rand(); }

static u64 g_r48 = 0x1234ABCD330Eull;
static u64 r48_next(void)
{
    g_r48 = (g_r48 * 0x5DEECE66Dull + 0xB) & 0xFFFFFFFFFFFFull;
    return g_r48;
}
static void g_srand48(long seed) { g_r48 = (((u64)(u32)seed) << 16) | 0x330E; }
static long g_lrand48(void) { return (long)(r48_next() >> 17); }
static long g_mrand48(void) { return (long)(i32)(r48_next() >> 16); }
static u64 g_drand48(void) { return hk_db((double)r48_next() / 281474976710656.0); }

static u32 g_arc4random(void)
{
    u32 v;
    hk_random(&v, 4);
    return v;
}
static u32 g_arc4random_uniform(u32 bound) { return bound ? g_arc4random() % bound : 0; }
static void g_arc4random_buf(void *buf, size_t n) { hk_random(buf, n); }
static int g_getentropy(void *buf, size_t n) { hk_random(buf, n); return 0; }
static i32 g_getrandom(void *buf, size_t n, unsigned flags) { (void)flags; hk_random(buf, n); return (i32)n; }

/* ---------------------------------------------------------------- signals
 *
 * Handlers are recorded here. raise() calls one at once; pthread_kill leaves
 * the signal waiting for the thread it names, and the handler runs on that
 * thread (hk_bionic.h says where and when). Nothing comes from the system: a
 * fault in guest code is this layer's to repair or to report, not the guest's.
 *
 * 32-bit bionic's sigset_t is one word: signals 1 to 32. The real-time ones,
 * 33 to 64, are sent all the same (libcore's AsynchronousCloseMonitor sends
 * 34 to wake a thread blocked on a socket being closed); a thread keeps them
 * in a word of their own, and a 32-bit mask has no say over them.
 */
typedef struct g_sigaction {
    u32 handler, mask, flags, restorer;
} g_sigaction;
static g_sigaction g_sigs[65];

#define SIG_BIT(sig) (1u << ((sig) - 1))
#define SIG_RT_BIT(sig) (1u << ((sig) - 33))
#define G_SA_SIGINFO 4u
#define G_SA_RESTART 0x10000000u
#define G_SA_NODEFER 0x40000000u

static int g_sigaction_fn(int sig, const g_sigaction *act, g_sigaction *old)
{
    if (sig < 1 || sig > 64) {
        hk_set_errno(GE_INVAL);
        return -1;
    }
    if (old) *old = g_sigs[sig];
    if (act) g_sigs[sig] = *act;
    return 0;
}

static u32 g_signal(int sig, u32 handler)
{
    u32 old;
    if (sig < 1 || sig > 64) return 0xFFFFFFFFu;
    old = g_sigs[sig].handler;
    g_sigs[sig].handler = handler;
    g_sigs[sig].flags = 0;
    return old;
}

static void call_handler(const g_sigaction *a, int sig)
{
    if (a->flags & G_SA_SIGINFO) {
        /* A siginfo that says which signal, and a context with nothing in it: no handler seen so far reads more. */
        u32 info[32], context[192];
        memset(info, 0, sizeof(info));
        memset(context, 0, sizeof(context));
        info[0] = (u32)sig;
        HK_GFN(void (*)(int, void *, void *), a->handler)(sig, info, context);
    } else {
        HK_GFN(void (*)(int), a->handler)(sig);
    }
}

static int g_raise(int sig)
{
    g_sigaction a;
    if (sig < 1 || sig > 64) return -1;
    a = g_sigs[sig];
    if (a.handler == 1) return 0;                  /* SIG_IGN */
    if (a.handler == 0) {                          /* SIG_DFL */
        if (sig == 6 || sig == 11 || sig == 4 || sig == 7 || sig == 8) hk_fatal("guest: raise(%d) with no handler", sig);
        return 0;
    }
    call_handler(&a, sig);
    return 0;
}

void hk_signals_check(void)
{
    hk_gthread *t = hk_gthread_self();
    for (;;) {
        u32 ready = (u32)t->sig_pending & ~t->sig_blocked, ready_rt = (u32)t->sig_pending_rt & ~t->sig_blocked_rt, bit, saved, saved_rt;
        volatile i32 *word = &t->sig_pending;
        g_sigaction a;
        int sig = 1;
        if (ready) {
            while (!(ready & SIG_BIT(sig))) sig++;
            bit = SIG_BIT(sig);
        } else if (ready_rt) {
            sig = 33;
            while (!(ready_rt & SIG_RT_BIT(sig))) sig++;
            bit = SIG_RT_BIT(sig);
            word = &t->sig_pending_rt;
        } else {
            return;
        }
        for (;;) {
            i32 seen = *word;
            if (hk_atomic_cas(word, seen, (i32)((u32)seen & ~bit)) == seen) break;
        }
        a = g_sigs[sig];
        if (a.handler <= 1) continue;
        /* While it runs, the signals its sigaction names are held back, and the signal itself unless it asked otherwise. */
        saved = t->sig_blocked;
        saved_rt = t->sig_blocked_rt;
        t->sig_blocked |= a.mask;
        if (!(a.flags & G_SA_NODEFER)) {
            if (sig <= 32) t->sig_blocked |= bit;
            else t->sig_blocked_rt |= bit;
        }
        hk_atomic_add(&t->sig_handled, 1);
        if (!(a.flags & G_SA_RESTART)) hk_atomic_add(&t->sig_unrestarted, 1);
        hk_trace("signal %d handled on thread %d", sig, (int)t->tid);
        call_handler(&a, sig);
        t->sig_blocked = saved;
        t->sig_blocked_rt = saved_rt;
    }
}

/* The flag "no-interrupt": threads are only told where they wait, never stopped in the middle of guest code. */
static bool g_sig_no_interrupt;

/* Whatever the thread is doing, it is told: woken where it waits, or interrupted where it runs guest code. */
static void sig_kick(hk_gthread *t)
{
    volatile i32 *addr = t->sig_wait_addr;
    hk_cond *cond = t->sig_wait_cond;
    hk_atomic_add(&t->sig_park, 1);
    hk_futex_wake(&t->sig_park, true);
    if (addr) hk_futex_wake(addr, true);
    if (cond) hk_cond_broadcast(cond);
    if (!addr && !cond && t->sig_thread && !g_sig_no_interrupt) hk_thread_interrupt(t->sig_thread);
}

/*
 * The threads with a signal still waiting. A thread that was inside this
 * layer or the system when it was told is told again every millisecond until
 * its handler has run: it comes back to guest code, or reaches a wait, soon.
 */
static struct {
    hk_lock lock;
    hk_gthread *threads[64];
    int count;
    hk_thread *thread;
    volatile i32 busy;
} G_SIGPUMP;

static void sigpump_main(void *arg)
{
    (void)arg;
    for (;;) {
        int i;
        hk_lock_acquire(&G_SIGPUMP.lock);
        for (i = 0; i < G_SIGPUMP.count;) {
            hk_gthread *t = G_SIGPUMP.threads[i];
            if (!hk_signals_ready(t)) {
                G_SIGPUMP.threads[i] = G_SIGPUMP.threads[--G_SIGPUMP.count];
                continue;
            }
            sig_kick(t);
            i++;
        }
        hk_atomic_xchg(&G_SIGPUMP.busy, G_SIGPUMP.count ? 1 : 0);
        hk_lock_release(&G_SIGPUMP.lock);
        if (G_SIGPUMP.busy) hk_sleep_ns(1000000ull);
        else hk_futex_wait(&G_SIGPUMP.busy, 0, 0);
    }
}

static bool sig_in_guest_code(hk_addr pc) { return hk_ld_is_guest_code(pc); }

static void sigpump_add(hk_gthread *t)
{
    int i;
    hk_lock_acquire(&G_SIGPUMP.lock);
    for (i = 0; i < G_SIGPUMP.count && G_SIGPUMP.threads[i] != t; i++) { }
    if (i == G_SIGPUMP.count && G_SIGPUMP.count < (int)HK_COUNT(G_SIGPUMP.threads)) G_SIGPUMP.threads[G_SIGPUMP.count++] = t;
    if (!G_SIGPUMP.thread) {
        g_sig_no_interrupt = hk_flag("no-interrupt");
        hk_thread_set_interrupt(hk_signals_check, sig_in_guest_code);
        G_SIGPUMP.thread = hk_thread_start(sigpump_main, NULL, 128 * 1024, "signals");
    }
    hk_atomic_xchg(&G_SIGPUMP.busy, 1);
    hk_lock_release(&G_SIGPUMP.lock);
    hk_futex_wake(&G_SIGPUMP.busy, true);
}

void hk_signals_thread_gone(hk_gthread *t)
{
    int i;
    hk_lock_acquire(&G_SIGPUMP.lock);
    for (i = 0; i < G_SIGPUMP.count; i++)
        if (G_SIGPUMP.threads[i] == t) G_SIGPUMP.threads[i--] = G_SIGPUMP.threads[--G_SIGPUMP.count];
    hk_lock_release(&G_SIGPUMP.lock);
}

int hk_signal_send(hk_gthread *t, int sig)
{
    u32 handler;
    volatile i32 *word;
    u32 bit;
    if (!t) return 3;                              /* ESRCH */
    if (sig == 0) return 0;
    if (sig < 1 || sig > 64) return GE_INVAL;
    handler = g_sigs[sig].handler;
    if (handler == 1) return 0;
    if (handler == 0) {
        hk_note_once("pthread_kill(%d): the app has no handler for it, and nothing is done", sig);
        return 0;
    }
    hk_trace("signal %d sent to thread %d", sig, (int)t->tid);
    word = sig <= 32 ? &t->sig_pending : &t->sig_pending_rt;
    bit = sig <= 32 ? SIG_BIT(sig) : SIG_RT_BIT(sig);
    for (;;) {
        i32 seen = *word;
        if (hk_atomic_cas(word, seen, (i32)((u32)seen | bit)) == seen) break;
    }
    if (t == hk_gthread_self()) {
        hk_signals_check();
        return 0;
    }
    /* (Set up before the first kick: that is what interrupts a thread in the middle of guest code.) */
    sigpump_add(t);
    sig_kick(t);
    return 0;
}

bool hk_gwait(volatile i32 *addr, i32 expect, u64 timeout_ns)
{
    hk_gthread *t = hk_gthread_self();
    bool ok = true;
    t->sig_wait_addr = addr;
    if (!hk_signals_ready(t)) ok = hk_futex_wait(addr, expect, timeout_ns);
    t->sig_wait_addr = NULL;
    if (hk_signals_ready(t)) hk_signals_check();
    return ok;
}

void hk_gsleep(u64 ns)
{
    hk_gthread *t = hk_gthread_self();
    u64 end;
    /* A short one is over before anyone would be kept waiting, and keeps the host's better timer. */
    if (ns < 3000000ull) {
        hk_sleep_ns(ns);
        if (hk_signals_ready(t)) hk_signals_check();
        return;
    }
    end = hk_time_monotonic_ns() + ns;
    for (;;) {
        i32 park = t->sig_park;
        u64 now;
        if (hk_signals_ready(t)) {
            hk_signals_check();
            continue;
        }
        now = hk_time_monotonic_ns();
        if (now >= end) return;
        hk_futex_wait(&t->sig_park, park, end - now);
    }
}

int hk_signal_mask(int how, const u32 *set, u32 *old)
{
    hk_gthread *t = hk_gthread_self();
    if (old) *old = t->sig_blocked;
    if (set) {
        if (how == 0) t->sig_blocked |= *set;              /* SIG_BLOCK */
        else if (how == 1) t->sig_blocked &= ~*set;        /* SIG_UNBLOCK */
        else if (how == 2) t->sig_blocked = *set;          /* SIG_SETMASK */
        else return GE_INVAL;
        if (hk_signals_ready(t)) hk_signals_check();
    }
    return 0;
}

static int g_kill(int pid, int sig) { return (pid == g_getpid() || pid == 0) && sig ? g_raise(sig) : 0; }
static int g_tgkill(int tgid, int tid, int sig) { (void)tgid; (void)tid; return sig ? g_raise(sig) : 0; }
static int g_sigprocmask(int how, const u32 *set, u32 *old)
{
    int e = hk_signal_mask(how, set, old);
    if (e) {
        hk_set_errno(e);
        return -1;
    }
    return 0;
}
static int g_sigemptyset(u32 *set) { *set = 0; return 0; }
static int g_sigfillset(u32 *set) { *set = 0xFFFFFFFFu; return 0; }
static int g_sigaddset(u32 *set, int sig) { if (sig >= 1 && sig <= 32) *set |= 1u << (sig - 1); return 0; }
static int g_sigdelset(u32 *set, int sig) { if (sig >= 1 && sig <= 32) *set &= ~(1u << (sig - 1)); return 0; }
static int g_sigismember(const u32 *set, int sig) { return sig >= 1 && sig <= 32 ? (int)((*set >> (sig - 1)) & 1) : 0; }
static int g_sigpending(u32 *set)
{
    hk_gthread *t = hk_gthread_self();
    *set = (u32)t->sig_pending & t->sig_blocked;
    return 0;
}

/* Waits, with `set` as the mask, until a handler has run on this thread. */
static int g_sigsuspend(const u32 *set)
{
    hk_gthread *t = hk_gthread_self();
    u32 saved = t->sig_blocked;
    i32 before = t->sig_handled;
    t->sig_blocked = set ? *set : 0;
    for (;;) {
        i32 park = t->sig_park;
        hk_signals_check();
        if (t->sig_handled != before) break;
        hk_futex_wait(&t->sig_park, park, 0);
    }
    t->sig_blocked = saved;
    if (hk_signals_ready(t)) hk_signals_check();
    hk_set_errno(GE_INTR);
    return -1;
}
static int g_sigwait(const u32 *set, int *sig) { (void)set; (void)sig; hk_set_errno(GE_INVAL); return GE_INVAL; }

typedef struct g_stack_t {
    u32 sp;
    i32 flags;
    u32 size;
} g_stack_t;

static int g_sigaltstack(const g_stack_t *ss, g_stack_t *old)
{
    hk_gthread *t = hk_gthread_self();
    if (old) {
        old->sp = (u32)(hk_addr)t->altstack_sp;
        old->size = t->altstack_size;
        old->flags = t->altstack_sp ? 0 : 2;       /* SS_DISABLE */
    }
    if (ss) {
        t->altstack_sp = (void *)(hk_addr)ss->sp;
        t->altstack_size = ss->size;
    }
    return 0;
}

static int g_sigrtmin(void) { return 36; }
static int g_sigrtmax(void) { return 64; }

/* ---------------------------------------------------------------- locales */

typedef struct g_lconv {
    u32 decimal_point, thousands_sep, grouping, int_curr_symbol, currency_symbol, mon_decimal_point, mon_thousands_sep, mon_grouping,
        positive_sign, negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space, p_sign_posn, n_sign_posn,
        int_p_cs_precedes, int_p_sep_by_space, int_n_cs_precedes, int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
} g_lconv;
static g_lconv g_lconv_c;
static int g_locale_c;                             /* what a locale_t points at */

static g_lconv *g_localeconv(void)
{
    static const char dot[] = ".", empty[] = "";
    u32 *p = &g_lconv_c.decimal_point;
    int i;
    if (!g_lconv_c.decimal_point) {
        p[0] = (u32)(hk_addr)dot;
        for (i = 1; i < 10; i++) p[i] = (u32)(hk_addr)empty;
        memset(&g_lconv_c.int_frac_digits, 127, 14);
    }
    return &g_lconv_c;
}

static char *g_setlocale(int category, const char *locale)
{
    (void)category;
    if (locale && *locale && strcmp(locale, "C") && strcmp(locale, "POSIX") && strcmp(locale, "C.UTF-8") && strcmp(locale, "en_US.UTF-8")) return NULL;
    return (char *)"C.UTF-8";
}

static void *g_newlocale(int mask, const char *locale, void *base)
{
    (void)mask; (void)base;
    if (locale && *locale && strcmp(locale, "C") && strcmp(locale, "POSIX") && strcmp(locale, "C.UTF-8") && strcmp(locale, "en_US.UTF-8")) {
        hk_set_errno(GE_NOENT);
        return NULL;
    }
    return &g_locale_c;
}
static void *g_uselocale(void *loc) { (void)loc; return &g_locale_c; }
static void g_freelocale(void *loc) { (void)loc; }
static void *g_duplocale(void *loc) { (void)loc; return &g_locale_c; }
static char *g_nl_langinfo(int item) { return (char *)(item == 14 ? "UTF-8" : ""); }   /* CODESET */
static size_t g_mb_cur_max(void) { return 4; }

/* ------------------------------------------------------------- odds and ends */

static char *g_basename(const char *path)
{
    static HK_THREAD_LOCAL char buf[512];
    size_t n;
    const char *s;
    if (!path || !*path) return strcpy(buf, ".");
    hk_strlcpy(buf, path, sizeof(buf));
    n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;
    s = strrchr(buf, '/');
    return (s && s[1]) ? (char *)s + 1 : buf;
}

static char *g_dirname(const char *path)
{
    static HK_THREAD_LOCAL char buf[512];
    size_t n;
    char *s;
    if (!path || !*path) return strcpy(buf, ".");
    hk_strlcpy(buf, path, sizeof(buf));
    n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;
    s = strrchr(buf, '/');
    if (!s) return strcpy(buf, ".");
    if (s == buf) s[1] = 0;
    else *s = 0;
    return buf;
}

static int g_mkstemp(char *tmpl)
{
    typedef int (*open_fn)(const char *, int, int);
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t n = strlen(tmpl);
    int tries, i, fd;
    if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) {
        hk_set_errno(GE_INVAL);
        return -1;
    }
    for (tries = 0; tries < 100; tries++) {
        u8 r[6];
        hk_random(r, 6);
        for (i = 0; i < 6; i++) tmpl[n - 6 + i] = chars[r[i] % 36];
        fd = ((open_fn)hk_bionic_find("open"))(tmpl, 2 | 0100 | 0200, 0600);
        if (fd >= 0) return fd;
        if (*hk_errno_ptr() != GE_EXIST) return -1;
    }
    return -1;
}

static char *g_mktemp(char *tmpl)
{
    int fd = g_mkstemp(tmpl);
    if (fd < 0) {
        tmpl[0] = 0;
        return tmpl;
    }
    hk_fd_close(fd);
    return tmpl;
}

static char *g_mkdtemp(char *tmpl)
{
    typedef int (*mkdir_fn)(const char *, int);
    typedef int (*unlink_fn)(const char *);
    int fd = g_mkstemp(tmpl);
    if (fd < 0) return NULL;
    hk_fd_close(fd);
    ((unlink_fn)hk_bionic_find("unlink"))(tmpl);
    return ((mkdir_fn)hk_bionic_find("mkdir"))(tmpl, 0700) == 0 ? tmpl : NULL;
}

typedef struct g_passwd {
    u32 pw_name, pw_passwd;
    u32 pw_uid, pw_gid;
    u32 pw_dir, pw_shell;
} g_passwd;

static g_passwd *g_getpwuid(int uid)
{
    static g_passwd pw;
    (void)uid;
    pw.pw_name = (u32)(hk_addr)"u0_a100";
    pw.pw_passwd = (u32)(hk_addr)"";
    pw.pw_uid = pw.pw_gid = 10100;
    pw.pw_dir = (u32)(hk_addr)hk_fs_internal_dir();
    pw.pw_shell = (u32)(hk_addr)"/system/bin/sh";
    return &pw;
}
static g_passwd *g_getpwnam(const char *name) { (void)name; return g_getpwuid(10100); }
/* The reentrant forms: the same one user, written into the caller's own structure. */
static int g_getpwuid_r(int uid, g_passwd *pw, char *buf, size_t n, u32 *result)
{
    (void)buf; (void)n;
    *pw = *g_getpwuid(uid);
    *result = (u32)(hk_addr)pw;
    return 0;
}
static int g_getpwnam_r(const char *name, g_passwd *pw, char *buf, size_t n, u32 *result)
{
    (void)name;
    return g_getpwuid_r(10100, pw, buf, n, result);
}
static const char *g_getprogname(void) { return "app_process"; }

/* One group as there is one user, the app's own, under the same name. Any other is not found, which is an answer and not an error. */
typedef struct g_group {
    u32 gr_name, gr_passwd, gr_gid, gr_mem;
} g_group;
static int group_into(bool found, g_group *grp, char *buf, size_t n, u32 *result)
{
    static const char name[] = "u0_a100";
    static const u32 no_members = 0;
    *result = 0;
    if (!found) return 0;
    if (n < sizeof(name)) return GE_RANGE;
    memcpy(buf, name, sizeof(name));
    grp->gr_name = (u32)(hk_addr)buf;
    grp->gr_passwd = (u32)(hk_addr)(buf + sizeof(name) - 1);
    grp->gr_gid = 10100;
    grp->gr_mem = (u32)(hk_addr)&no_members;
    *result = (u32)(hk_addr)grp;
    return 0;
}
static int g_getgrgid_r(u32 gid, g_group *grp, char *buf, size_t n, u32 *result) { return group_into(gid == 10100, grp, buf, n, result); }
static int g_getgrnam_r(const char *name, g_group *grp, char *buf, size_t n, u32 *result)
{
    return group_into(name && !strcmp(name, "u0_a100"), grp, buf, n, result);
}

/* The one user and group cannot be changed for others: setting them to what they are (or leaving them, -1) succeeds, anything else is not permitted. */
static bool own_id(u32 id) { return id == 10100 || id == 0xFFFFFFFFu; }
static int g_set_one_id(u32 id)
{
    if (own_id(id)) return 0;
    hk_set_errno(GE_PERM);
    return -1;
}
static int g_set_two_ids(u32 real, u32 effective)
{
    if (own_id(real) && own_id(effective)) return 0;
    hk_set_errno(GE_PERM);
    return -1;
}
/* One process, a group of its own. */
static int g_getpgid(int pid)
{
    if (pid == 0 || pid == g_getpid()) return g_getpid();
    hk_set_errno(GE_SRCH);
    return -1;
}
static int g_setpgid(int pid, int pgid)
{
    if (pid != 0 && pid != g_getpid()) {
        hk_set_errno(GE_SRCH);
        return -1;
    }
    if (pgid == 0 || pgid == g_getpid()) return 0;
    hk_set_errno(GE_PERM);
    return -1;
}

/* Capabilities: an app has none. Version 1 of the call has one set of three words, versions 2 and 3 have two; any other is told which is spoken. */
static int g_capget(u32 *header, u32 *data)
{
    if (!header) {
        hk_set_errno(GE_FAULT);
        return -1;
    }
    if (header[0] == 0x19980330u || header[0] == 0x20071026u || header[0] == 0x20080522u) {
        if (data) memset(data, 0, header[0] == 0x19980330u ? 12 : 24);
        return 0;
    }
    header[0] = 0x20080522u;
    hk_set_errno(GE_INVAL);
    return -1;
}
/*
 * ptrace: no process here may trace another, nor itself be traced, which is what Android answers an app that is not a
 * debugger of a debuggable app. Crash reporters use it to read the registers of the app's other threads; they take EPERM.
 */
static long g_ptrace(int request, int pid, void *addr, void *data)
{
    (void)request; (void)pid; (void)addr; (void)data;
    hk_set_errno(GE_PERM);
    return -1;
}

/* setpgrp(): bionic's is setpgid(0, 0). */
static int g_setpgrp(void) { return g_setpgid(0, 0); }

/*
 * The page size as data. Bionic exports __page_size and __page_shift on 32-bit devices, for code built with NDKs that read
 * PAGE_SIZE from libc rather than from the header (Android 9 still has them, "for backwards compatibility"); a crash SDK
 * built that way read address 0 for it here and ended the app.
 */
static u32 g_page_size = 4096, g_page_shift = 12;

static int g_capset(u32 *header, const u32 *data)
{
    (void)header; (void)data;
    hk_set_errno(GE_PERM);
    return -1;
}

/* The table of mounted file systems: there is no /proc/mounts to open, so no entries to read. */
static void *g_setmntent(const char *path, const char *mode)
{
    (void)path; (void)mode;
    hk_set_errno(GE_NOENT);
    return NULL;
}
static void *g_getmntent_r(void *file, void *entry, char *buf, int n)
{
    (void)file; (void)entry; (void)buf; (void)n;
    return NULL;
}
static int g_endmntent(void *file) { (void)file; return 1; }
static char *g_getlogin(void) { return (char *)"u0_a100"; }

static int g_system(const char *cmd)
{
    (void)cmd;
    return cmd ? -1 : 0;
}

/*
 * getopt's globals, which code that parses its own arguments reads (libchrome links them though nothing here runs it as a
 * program). getopt itself is not here.
 */
static u32 g_optarg;
static i32 g_optind = 1, g_opterr = 1, g_optopt;

/* sysinfo (Linux's struct sysinfo, 64 bytes on 32-bit): the uptime, the memory the app may use and what is free of it. */
static int g_sysinfo(u32 *out)
{
    u64 limit = hk_mem_limit(), used = hk_mem_usage();
    if (!limit) limit = 2048ull << 20;
    memset(out, 0, 64);
    out[0] = (u32)(hk_time_monotonic_ns() / 1000000000ull);   /* uptime */
    out[4] = (u32)(limit > 0xFFFFFFFFu ? 0xFFFFFFFFu : limit);  /* totalram */
    out[5] = (u32)(limit > used ? (limit - used > 0xFFFFFFFFu ? 0xFFFFFFFFu : limit - used) : 0);   /* freeram */
    ((u16 *)out)[20] = 1;                                     /* procs */
    out[13] = 1;                                              /* mem_unit */
    return 0;
}

/*
 * syslog: into the log, as bionic's does (to Android's log, with the tag openlog gave). Priorities 0-3 are errors, 4
 * warnings, 5-6 information, 7 debugging. %m is the text of errno, as syslog's format has it.
 */
static char g_syslog_ident[64] = "syslog";
static int g_syslog_mask = 0xFF;

static void g_openlog(const char *ident, int option, int facility)
{
    (void)option;
    (void)facility;
    hk_strlcpy(g_syslog_ident, ident && *ident ? ident : "syslog", sizeof(g_syslog_ident));
}
static void g_closelog(void) { }
static int g_setlogmask(int mask)
{
    int old = g_syslog_mask;
    if (mask) g_syslog_mask = mask;
    return old;
}

static void g_vsyslog(int priority, const char *fmt, va_list ap)
{
    typedef const char *(*strerror_fn)(int);
    char form[512], line[1024];
    const char *p;
    size_t n = 0;
    int level = priority & 7;
    if (!fmt || !(g_syslog_mask & (1 << level))) return;
    /* %m first: the rest is printf's. */
    for (p = fmt; *p && n + 1 < sizeof(form); p++) {
        if (p[0] == '%' && p[1] == '%') {
            if (n + 2 < sizeof(form)) {
                form[n++] = '%';
                form[n++] = '%';
            }
            p++;
        } else if (p[0] == '%' && p[1] == 'm') {
            const char *t = ((strerror_fn)hk_bionic_find("strerror"))(*hk_errno_ptr());
            for (; t && *t && n + 2 < sizeof(form); t++) {
                if (*t == '%') form[n++] = '%';
                form[n++] = *t;
            }
            p++;
        } else {
            form[n++] = *p;
        }
    }
    form[n] = 0;
    vsnprintf(line, sizeof(line), form, ap);
    hk_log(level <= 3 ? HK_LOG_ERROR : level == 4 ? HK_LOG_WARN : level <= 6 ? HK_LOG_INFO : HK_LOG_DEBUG, "%c/%s: %s",
           level <= 3 ? 'E' : level == 4 ? 'W' : level <= 6 ? 'I' : 'D', g_syslog_ident, line);
}

static void g_syslog(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_vsyslog(priority, fmt, ap);
    va_end(ap);
}

/*
 * process_vm_readv and process_vm_writev: this process's own memory only (getpid() is 4242 here), copied between the two
 * lists of pieces; another process is ESRCH. 32-bit iovecs: a base and a length.
 */
static i32 vm_copy(int pid, const u32 *local, unsigned nlocal, const u32 *remote, unsigned nremote, bool read)
{
    unsigned li = 0, ri = 0;
    u32 loff = 0, roff = 0;
    i32 done = 0;
    if (pid != 4242) {
        hk_set_errno(GE_SRCH);
        return -1;
    }
    while (li < nlocal && ri < nremote) {
        u32 lleft = local[2 * li + 1] - loff, rleft = remote[2 * ri + 1] - roff, n = lleft < rleft ? lleft : rleft;
        u8 *l = (u8 *)(hk_addr)local[2 * li] + loff, *rm = (u8 *)(hk_addr)remote[2 * ri] + roff;
        if (n) {
            if (read) memcpy(l, rm, n);
            else memcpy(rm, l, n);
            done += (i32)n;
        }
        loff += n;
        roff += n;
        if (loff == local[2 * li + 1]) { li++; loff = 0; }
        if (roff == remote[2 * ri + 1]) { ri++; roff = 0; }
    }
    return done;
}
static i32 g_process_vm_readv(int pid, const u32 *local, unsigned nl, const u32 *remote, unsigned nr, unsigned flags)
{
    (void)flags;
    return vm_copy(pid, local, nl, remote, nr, true);
}
static i32 g_process_vm_writev(int pid, const u32 *local, unsigned nl, const u32 *remote, unsigned nr, unsigned flags)
{
    (void)flags;
    return vm_copy(pid, local, nl, remote, nr, false);
}

void hk_misc_init(void)
{
    hk_random(&g_stack_chk_guard, sizeof(g_stack_chk_guard));
    hk_random(g_at_random, sizeof(g_at_random));
}

#define M(n, f) HK_WRAP(n, f)

const hk_sym hk_tab_misc[] = {
    HK_DATA("optarg", &g_optarg), HK_DATA("optind", &g_optind), HK_DATA("opterr", &g_opterr), HK_DATA("optopt", &g_optopt), M("sysinfo", g_sysinfo),
    M("__errno", hk_errno_ptr), HK_DATA("__stack_chk_guard", &g_stack_chk_guard), M("__stack_chk_fail", g_stack_chk_fail),
    M("atexit", g_atexit), M("__cxa_atexit", g_cxa_atexit), M("__cxa_thread_atexit_impl", g_cxa_thread_atexit),
    M("__aeabi_atexit", g_aeabi_atexit), M("__cxa_finalize", g_cxa_finalize), M("exit", g_exit), M("_exit", g__exit), M("_Exit", g__exit),
    M("abort", g_abort), M("__assert", g_assert), M("__assert2", g_assert2), M("__cxa_pure_virtual", g_cxa_pure_virtual),
    M("__cxa_bad_cast", g_cxa_bad_cast), M("__cxa_guard_acquire", g_cxa_guard_acquire), M("__cxa_guard_release", g_cxa_guard_release),
    M("__cxa_guard_abort", g_cxa_guard_abort),
    M("getpid", g_getpid), M("getppid", g_getppid), M("getuid", g_getuid), M("geteuid", g_getuid), M("getgid", g_getuid),
    M("getegid", g_getuid), M("setuid", g_zero), M("setgid", g_zero), M("getpgrp", g_getpid), M("setsid", g_getpid),
    M("fork", g_fail_nosys), M("vfork", g_fail_nosys), M("execv", g_fail_nosys), M("execve", g_fail_nosys), M("execvp", g_fail_nosys),
    M("execl", g_fail_nosys), M("execlp", g_fail_nosys), M("wait", g_fail_nosys), M("waitpid", g_fail_nosys), M("system", g_system),
    M("getenv", g_getenv), M("setenv", g_setenv), M("unsetenv", g_unsetenv), M("putenv", g_putenv), M("clearenv", g_clearenv),
    M("secure_getenv", g_getenv), HK_DATA("environ", &g_environ),
    M("sysconf", g_sysconf), M("getpagesize", g_getpagesize), M("__getpagesize", g_getpagesize), M("uname", g_uname),
    M("gethostname", g_gethostname),
    M("__system_property_get", g_property_get), M("__system_property_find", g_property_find),
    M("__system_property_read", g_property_read), M("__system_property_read_callback", g_property_read_callback),
    M("__system_property_set", g_property_set),
    M("getauxval", g_getauxval), M("prctl", g_prctl), M("cacheflush", g_cacheflush), M("syscall", g_syscall),
    M("openlog", g_openlog), M("closelog", g_closelog), M("setlogmask", g_setlogmask), M("syslog", g_syslog), M("vsyslog", g_vsyslog),
    M("process_vm_readv", g_process_vm_readv), M("process_vm_writev", g_process_vm_writev),
    M("getrlimit", g_getrlimit), M("getrlimit64", g_getrlimit), M("setrlimit", g_setrlimit), M("getrusage", g_getrusage),
    M("getpriority", g_getpriority), M("setpriority", g_setpriority), M("nice", g_nice),
    M("qsort", g_qsort), M("bsearch", g_bsearch),
    M("rand", g_rand), M("srand", g_srand), M("rand_r", g_rand_r), M("random", g_random), M("srandom", g_srand),
    M("srand48", g_srand48), M("lrand48", g_lrand48), M("mrand48", g_mrand48), M("drand48", g_drand48),
    M("arc4random", g_arc4random), M("arc4random_uniform", g_arc4random_uniform), M("arc4random_buf", g_arc4random_buf),
    M("getentropy", g_getentropy), M("getrandom", g_getrandom),
    M("sigaction", g_sigaction_fn), M("signal", g_signal), M("bsd_signal", g_signal), M("sysv_signal", g_signal), M("raise", g_raise),
    M("kill", g_kill), M("tgkill", g_tgkill), M("sigprocmask", g_sigprocmask), M("sigemptyset", g_sigemptyset),
    M("sigfillset", g_sigfillset), M("sigaddset", g_sigaddset), M("sigdelset", g_sigdelset), M("sigismember", g_sigismember),
    M("sigpending", g_sigpending), M("sigsuspend", g_sigsuspend), M("sigwait", g_sigwait), M("sigaltstack", g_sigaltstack),
    M("__libc_current_sigrtmin", g_sigrtmin), M("__libc_current_sigrtmax", g_sigrtmax),
    M("localeconv", g_localeconv), M("setlocale", g_setlocale), M("newlocale", g_newlocale), M("uselocale", g_uselocale),
    M("freelocale", g_freelocale), M("duplocale", g_duplocale), M("nl_langinfo", g_nl_langinfo), M("nl_langinfo_l", g_nl_langinfo),
    M("__ctype_get_mb_cur_max", g_mb_cur_max),
    M("basename", g_basename), M("__gnu_basename", g_basename), M("dirname", g_dirname), M("mkstemp", g_mkstemp),
    M("mkstemp64", g_mkstemp), M("mktemp", g_mktemp), M("mkdtemp", g_mkdtemp),
    M("getpwuid", g_getpwuid), M("getpwnam", g_getpwnam), M("getlogin", g_getlogin),
    M("getpwuid_r", g_getpwuid_r), M("getpwnam_r", g_getpwnam_r), M("getprogname", g_getprogname),
    M("getgrgid_r", g_getgrgid_r), M("getgrnam_r", g_getgrnam_r),
    M("seteuid", g_set_one_id), M("setegid", g_set_one_id), M("setreuid", g_set_two_ids), M("setregid", g_set_two_ids),
    M("getpgid", g_getpgid), M("setpgid", g_setpgid), M("capget", g_capget), M("capset", g_capset),
    M("setpgrp", g_setpgrp), M("ptrace", g_ptrace), HK_DATA("__page_size", &g_page_size), HK_DATA("__page_shift", &g_page_shift),
    M("setmntent", g_setmntent), M("getmntent_r", g_getmntent_r), M("endmntent", g_endmntent),
    /* A process that never forks has no use for what is to be done around a fork. */
    M("__register_atfork", g_zero),
    HK_END
};
