/*
 * A guest library for the loader's tests: Android code, built with clang for
 * armv7a-linux-androideabi (tools/build-guest.sh) once as Thumb and once as
 * ARM. No NDK is needed -- everything it uses from Android is declared here
 * and bound by name when the library is loaded.
 */
typedef unsigned int size_t;
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_key_t;
typedef int pthread_once_t;
typedef long pthread_t;
typedef struct { int flags; void *stack_base; size_t stack_size, guard_size; int sched_policy, sched_priority; } pthread_attr_t;
typedef long jmp_buf[64];

int printf(const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int sscanf(const char *, const char *, ...);
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void *memalign(size_t, size_t);
void free(void *);
size_t strlen(const char *);
char *strcpy(char *, const char *);
int strcmp(const char *, const char *);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
char *strdup(const char *);
double sin(double);
double pow(double, double);
float sqrtf(float);
double strtod(const char *, char **);
double modf(double, double *);
long lround(double);
int *__errno(void);
int __android_log_print(int, const char *, const char *, ...);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
int setjmp(jmp_buf);
void longjmp(jmp_buf, int) __attribute__((noreturn));
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_broadcast(pthread_cond_t *);
int pthread_key_create(pthread_key_t *, void (*)(void *));
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_once(pthread_once_t *, void (*)(void));
int gettid(void);
int open(const char *, int, ...);
int close(int);
int read(int, void *, size_t);
int write(int, const void *, size_t);
int pipe(int[2]);
void *fopen(const char *, const char *);
int fclose(void *);
size_t fwrite(const void *, size_t, size_t, void *);
size_t fread(void *, size_t, size_t, void *);
int fseek(void *, long, int);
long ftell(void *);
char *fgets(char *, int, void *);
int fprintf(void *, const char *, ...);
int unlink(const char *);
char *getenv(const char *);
int __system_property_get(const char *, char *);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);

/* ------------------------------------------------- data and relocations */

static int g_counter = 5;
static const char *const g_names[] = { "zero", "one", "two" };
int t_exported_value = 0x1234;
static int g_ctor_ran;

__attribute__((constructor)) static void ctor(void) { g_ctor_ran = 1234; }

int t_add(int a, int b) { return a + b; }
int *t_counter_ptr(void) { return &g_counter; }
const char *t_name(int i) { return g_names[i]; }
int t_ctor_ran(void) { return g_ctor_ran; }
const char *t_hello(void) { return "hello from guest"; }

/* --------------------------------------------------------- floating point */

/* Arguments and results cross in integer registers (softfp). */
double t_fmul(double a, float b) { return a * b; }
float t_mixed(int a, float b, double c, float d, int e, double f) { return (float)(a + b + c + d + e + f); }
float t_sqrt(float x) { return sqrtf(x); }
double t_sin(double x) { return sin(x); }
double t_pow(double x, double y) { return pow(x, y); }
long t_lround(double x) { return lround(x); }
double t_modf(double x, double *ip) { return modf(x, ip); }
double t_strtod(const char *s) { return strtod(s, 0); }
/* The host's function pointer, called with softfp arguments. */
double t_call_d(double (*fn)(double, double), double a, double b) { return fn(a, b); }

/* ------------------------------------------------------------- variadics */

int t_format(char *buf, int n) { return snprintf(buf, (size_t)n, "%d %s %.3f %lld %c %x", 42, "str", 3.14159, 1234567890123LL, 'z', 0xBEEF); }

int t_scan(const char *s, int *a, float *f, char *word)
{
    return sscanf(s, "%d %f %15s", a, f, word);
}

static int sum_va(int n, ...)
{
    __builtin_va_list ap;
    int total = 0;
    __builtin_va_start(ap, n);
    while (n--) total += __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    return total;
}
int t_sum(void) { return sum_va(6, 1, 2, 3, 4, 5, 6); }

void t_log(void) { __android_log_print(4, "t_basic", "logging %d and %s and %.2f", 7, "text", 2.5); }

/* ---------------------------------------------------- integer arithmetic */

long long t_div64(long long a, long long b) { return a / b; }
long long t_mod64(long long a, long long b) { return a % b; }
unsigned long long t_udiv64(unsigned long long a, unsigned long long b) { return a / b; }
unsigned t_udiv(unsigned a, unsigned b) { return a / b; }
int t_mod(int a, int b) { return a % b; }

/* ---------------------------------------------------------------- memory */

int t_heap(void)
{
    char *a = malloc(100), *b, *c;
    int ok = 1;
    if (!a) return 0;
    memset(a, 0x5A, 100);
    a = realloc(a, 5000);
    if (!a || a[99] != 0x5A) ok = 0;
    b = memalign(64, 300);
    if (!b || ((unsigned)b & 63)) ok = 0;
    c = calloc(10, 10);
    if (!c || c[50]) ok = 0;
    free(a);
    free(b);
    free(c);
    a = strdup("duplicate");
    if (!a || strcmp(a, "duplicate")) ok = 0;
    free(a);
    return ok;
}

/* ------------------------------------------------------------- callbacks */

static int by_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int t_sort(void)
{
    int v[64], i;
    unsigned x = 12345;
    for (i = 0; i < 64; i++) {
        x = x * 1103515245u + 12345u;
        v[i] = (int)(x >> 16) % 1000;
    }
    qsort(v, 64, sizeof(int), by_int);
    for (i = 1; i < 64; i++)
        if (v[i - 1] > v[i]) return 0;
    return 1;
}

int t_callback(int (*fn)(int), int v) { return fn(v) + 1; }

/* -------------------------------------------------------- setjmp, errno */

static void jumper(jmp_buf env, int v) { longjmp(env, v); }

int t_setjmp(void)
{
    jmp_buf env;
    volatile int stage = 0;
    int r = setjmp(env);
    if (r == 0) {
        stage = 1;
        jumper(env, 77);
        return -1;
    }
    return stage == 1 ? r : -2;
}

int t_errno(void)
{
    int fd = open("/data/data/does-not-exist/file", 0);
    return fd < 0 ? *__errno() : 0;
}

/* --------------------------------------------------------------- threads */

static pthread_mutex_t g_lock;                 /* PTHREAD_MUTEX_INITIALIZER is all zeros */
static pthread_mutex_t g_rlock = 0x4000;       /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER */
static pthread_cond_t g_cond;
static int g_shared, g_go;
static pthread_key_t g_key;
static pthread_once_t g_once;
static int g_once_runs;
static __thread int tls_counter = 100;         /* ELF TLS: read through the thread pointer */
static __thread char tls_text[16] = "tls";

static void once_fn(void) { g_once_runs++; }

static void *worker(void *arg)
{
    int i, n = (int)(long)arg;
    pthread_once(&g_once, once_fn);
    pthread_setspecific(g_key, (void *)(long)(gettid() + 1));
    pthread_mutex_lock(&g_lock);
    while (!g_go) pthread_cond_wait(&g_cond, &g_lock);
    pthread_mutex_unlock(&g_lock);
    for (i = 0; i < n; i++) {
        pthread_mutex_lock(&g_rlock);
        pthread_mutex_lock(&g_rlock);
        g_shared++;
        tls_counter++;
        pthread_mutex_unlock(&g_rlock);
        pthread_mutex_unlock(&g_rlock);
    }
    /* Each thread has its own copies, starting from the initial values. */
    if (tls_counter != 100 + n || tls_text[0] != 't') return (void *)-1L;
    if (pthread_getspecific(g_key) != (void *)(long)(gettid() + 1)) return (void *)-2L;
    return (void *)(long)tls_counter;
}

int t_threads(int threads, int n)
{
    pthread_t t[16];
    int i, bad = 0;
    void *ret;
    if (threads > 16) threads = 16;
    g_shared = 0;
    g_go = 0;
    pthread_key_create(&g_key, 0);
    for (i = 0; i < threads; i++)
        if (pthread_create(&t[i], 0, worker, (void *)(long)n) != 0) return -1;
    pthread_mutex_lock(&g_lock);
    g_go = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    for (i = 0; i < threads; i++) {
        pthread_join(t[i], &ret);
        if ((long)ret != 100 + n) bad++;
    }
    if (bad || g_once_runs != 1) return -100 - bad;
    return g_shared;
}

int t_tls_get(void) { return tls_counter; }
void t_tls_set(int v) { tls_counter = v; }
int *t_tls_addr(void) { return &tls_counter; }

/* ----------------------------------------------------------------- files */

/*
 * Stopping the world, the way a garbage collector does it: a signal to every
 * thread, whose handler says "stopped" and waits in sigsuspend for the signal
 * that lets it go on. One thread is in a loop that never calls anything, one
 * waits on a semaphore, one sleeps: the three places a thread can be found.
 */
typedef struct {
    void (*handler)(int, void *, void *);
    unsigned mask;
    int flags;
    void (*restorer)(void);
} t_sigaction;
typedef struct { unsigned count; } t_sem;
int sigaction(int, const t_sigaction *, t_sigaction *);
int sigsuspend(const unsigned *);
int pthread_kill(pthread_t, int);
int sem_init(t_sem *, int, unsigned);
int sem_post(t_sem *);
int sem_wait(t_sem *);
int usleep(unsigned);
struct t_timespec { long tv_sec, tv_nsec; };
int clock_gettime(int, struct t_timespec *);

/* Milliseconds on the monotonic clock: a wait is bounded by this, not by counting sleeps, which may each take a timer tick. */
static long now_ms(void)
{
    struct t_timespec ts;
    clock_gettime(1, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum { SIG_STOP = 30, SIG_GO = 24 };              /* SIGPWR and SIGXCPU, the two a collector on Android uses */
static t_sem g_idle;
static volatile int g_released, g_spins, g_quit;
/* For each of the three threads: the round it last stopped in, the round it was last let go from, and whether it has ended. */
static volatile int g_stopped_in[3], g_left[3], g_ended[3];
static __thread int tls_stops, tls_index;

static void on_stop(int sig, void *info, void *context)
{
    unsigned all_but_go = ~(1u << (SIG_GO - 1));
    int seen = g_released;
    (void)sig; (void)info; (void)context;
    tls_stops++;
    g_stopped_in[tls_index] = seen + 1;
    while (g_released == seen) sigsuspend(&all_but_go);
    g_left[tls_index] = seen + 1;
}
static void on_go(int sig, void *info, void *context) { (void)sig; (void)info; (void)context; }

static void *spinner(void *arg)
{
    tls_index = (int)(long)arg;
    while (!g_quit) g_spins++;
    g_ended[tls_index] = 1;
    return (void *)(long)tls_stops;
}
static void *waiter(void *arg)
{
    tls_index = (int)(long)arg;
    sem_wait(&g_idle);
    g_ended[tls_index] = 1;
    return (void *)(long)tls_stops;
}
static void *sleeper(void *arg)
{
    tls_index = (int)(long)arg;
    while (!g_quit) usleep(200000);
    g_ended[tls_index] = 1;
    return (void *)(long)tls_stops;
}

/* Which of the three have not reached `value` in `what` after three seconds: bit 0 the running one, bit 1 the waiting one, bit 2 the sleeping one. */
static int laggards(volatile int *what, int value)
{
    long until = now_ms() + 3000;
    int i, mask = 7;
    for (;;) {
        for (i = 0; i < 3; i++)
            if (what[i] == value) mask &= ~(1 << i);
        if (!mask || now_ms() > until) return mask;
        usleep(1000);
    }
}

/*
 * 0, or where it went wrong: 100 + which threads did not stop, 200 + which
 * did not go on, 300 + which did not end (bits as in laggards), 4 the running
 * thread ran on while stopped, 6 it did not run again, 10 + the thread that
 * did not handle every stop itself.
 */
int t_signals(int rounds)
{
    t_sigaction stop = { on_stop, ~(1u << (SIG_GO - 1)), 4 | 0x10000000, 0 }, go = { on_go, ~0u, 4 | 0x10000000, 0 };
    pthread_t t[3];
    void *result;
    int round, i, spins, late;
    sem_init(&g_idle, 0, 0);
    g_quit = g_spins = g_released = 0;
    for (i = 0; i < 3; i++) g_stopped_in[i] = g_left[i] = g_ended[i] = 0;
    if (sigaction(SIG_STOP, &stop, 0) || sigaction(SIG_GO, &go, 0)) return 1;
    if (pthread_create(&t[0], 0, spinner, (void *)0) || pthread_create(&t[1], 0, waiter, (void *)1) || pthread_create(&t[2], 0, sleeper, (void *)2)) return 2;
    for (i = (int)now_ms() + 3000; g_spins < 1000 && (int)now_ms() < i;) usleep(1000);
    if (g_spins < 1000) return 3;
    usleep(20000);                                 /* the other two have reached their waits */
    for (round = 1; round <= rounds; round++) {
        for (i = 0; i < 3; i++)
            if (pthread_kill(t[i], SIG_STOP)) return 5;
        late = laggards(g_stopped_in, round);
        if (late) return 100 + late;
        /* All three have said so; the one that was running must not be. */
        spins = g_spins;
        usleep(3000);
        if (g_spins != spins) return 4;
        g_released++;
        for (i = 0; i < 3; i++)
            if (pthread_kill(t[i], SIG_GO)) return 5;
        late = laggards(g_left, round);
        if (late) return 200 + late;
        for (i = (int)now_ms() + 2000; g_spins == spins && (int)now_ms() < i;) usleep(500);
        if (g_spins == spins) return 6;
    }
    g_quit = 1;
    sem_post(&g_idle);
    late = laggards(g_ended, 1);
    if (late) return 300 + late;
    for (i = 0; i < 3; i++) {
        pthread_join(t[i], &result);
        /* Each handled every stop itself, on its own thread. */
        if ((long)result != rounds) return 10 + i;
    }
    return 0;
}

int t_files(const char *dir)
{
    char path[256], line[64];
    void *f;
    int ok = 1, fds[2];
    long size;
    snprintf(path, sizeof(path), "%s/t_basic.txt", dir);
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "line %d\n", 1);
    fwrite("second\n", 1, 7, f);
    fclose(f);
    f = fopen(path, "r");
    if (!f) return -2;
    if (!fgets(line, sizeof(line), f) || strcmp(line, "line 1\n")) ok = 0;
    if (!fgets(line, sizeof(line), f) || strcmp(line, "second\n")) ok = 0;
    if (fgets(line, sizeof(line), f)) ok = 0;
    fseek(f, 0, 2);
    size = ftell(f);
    fclose(f);
    if (size != 14) ok = 0;
    unlink(path);
    if (fopen(path, "r")) ok = 0;
    if (pipe(fds) != 0) return -3;
    if (write(fds[1], "pipe", 4) != 4 || read(fds[0], line, 4) != 4 || line[0] != 'p' || line[3] != 'e') ok = 0;
    close(fds[0]);
    close(fds[1]);
    return ok;
}

int t_proc(void)
{
    char line[256];
    void *f = fopen("/proc/cpuinfo", "r");
    int found = 0;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        int i;
        for (i = 0; line[i]; i++)
            if (line[i] == 'n' && line[i + 1] == 'e' && line[i + 2] == 'o' && line[i + 3] == 'n') found = 1;
    }
    fclose(f);
    return found;
}

int t_props(void)
{
    char value[92];
    return __system_property_get("ro.build.version.sdk", value) > 0 && getenv("HOME") != 0 ? (value[0] - '0') * 10 + (value[1] - '0') : -1;
}

int t_dl(void)
{
    void *self = dlopen("libc.so", 0);
    int (*len)(const char *) = self ? dlsym(self, "strlen") : 0;
    return len ? len("four") : -1;
}

/*
 * A frame far bigger than a page, written at its far end first. Nothing in
 * Android code feels its way down a stack, and a host that hands a stack out
 * page by page (Windows) must have handed all of it out beforehand.
 */
__attribute__((noinline)) static int big_frame(int seed)
{
    volatile unsigned char buf[300 * 1024];
    buf[0] = (unsigned char)seed;
    buf[sizeof(buf) - 1] = (unsigned char)(seed + 1);
    buf[150000] = 7;
    return buf[0] + buf[sizeof(buf) - 1] + buf[150000];
}

static void *big_frame_thread(void *arg) { return (void *)(long)big_frame((int)(long)arg); }

/* 2 * (seed + seed + 1 + 7): once on the calling thread and once on a new one. */
int t_big_frame(int seed)
{
    pthread_t t;
    void *result = 0;
    int here = big_frame(seed);
    if (pthread_create(&t, 0, big_frame_thread, (void *)(long)seed) != 0) return -1;
    pthread_join(t, &result);
    return here + (int)(long)result;
}

/*
 * Memory mapped and unmapped the way an allocator does it: a megabyte and a
 * bit, the end unmapped first to line the block up, the rest when it is done
 * with. Four thousand times is more than a 32-bit process has addresses for
 * unless each block's addresses really come back. 0, or the round it failed at.
 */
void *mmap(void *, size_t, int, int, int, long);
int munmap(void *, size_t);

int t_map_pieces(int rounds)
{
    const size_t want = 1024 * 1024, extra = 15 * 4096;
    int i;
    for (i = 1; i <= rounds; i++) {
        unsigned char *p = mmap(0, want + extra, 3, 0x22, -1, 0);
        if (p == (unsigned char *)-1) return i;
        if (munmap(p + want, extra) != 0) return -i;
        p[0] = (unsigned char)i;
        p[want - 1] = (unsigned char)(i + 1);
        if (p[0] != (unsigned char)i || munmap(p, want) != 0) return -i;
    }
    return 0;
}

/*
 * libt_dep.so loaded where this asks, as Chromium's linker has its library loaded (android_dlopen_ext,
 * ANDROID_DLEXT_RESERVED_ADDRESS): into a range it reserved, the image at the range's start. A range too small is refused
 * with a reason that says so, and nothing is loaded; the whole range unmapped afterwards leaves the library, as Chromium
 * unmaps what its library did not use; asked for again, the library is given as it is. 7, or the negated step that failed.
 * Run before anything else loads libt_dep.so.
 */
typedef struct {
    unsigned long long flags;
    void *reserved_addr;
    size_t reserved_size;
    int relro_fd, library_fd;
    long long library_fd_offset;
    void *library_namespace;
} android_dlextinfo;
void *android_dlopen_ext(const char *, int, const android_dlextinfo *);
char *dlerror(void);
char *strstr(const char *, const char *);
int memcmp(const void *, const void *, size_t);

int t_dlext(void)
{
    const size_t size = 4 << 20;
    unsigned char *range = mmap(0, size, 0, 0x22, -1, 0);
    android_dlextinfo x;
    void *lib;
    int (*twice)(int, int);
    const char *err;
    if (range == (unsigned char *)-1) return -1;
    memset(&x, 0, sizeof(x));
    x.flags = 1;                                     /* ANDROID_DLEXT_RESERVED_ADDRESS */
    x.reserved_addr = range;
    x.reserved_size = 4096;
    x.relro_fd = x.library_fd = -1;
    if (android_dlopen_ext("libt_dep.so", 0, &x) != 0) return -2;
    err = dlerror();
    if (!err || !strstr(err, "reserved space")) return -3;
    x.reserved_size = size;
    lib = android_dlopen_ext("libt_dep.so", 0, &x);
    if (!lib) return -4;
    twice = (int (*)(int, int))dlsym(lib, "d_add_twice");
    if (!twice || (unsigned char *)twice < range || (unsigned char *)twice >= range + size || memcmp(range, "\177ELF", 4) != 0) return -5;
    if (munmap(range, size) != 0 || twice(1, 2) != 5) return -6;
    x.flags = 2;                                     /* ANDROID_DLEXT_RESERVED_ADDRESS_HINT, somewhere else */
    x.reserved_addr = range + size;
    if (android_dlopen_ext("libt_dep.so", 0, &x) != lib) return -7;
    return 7;
}

/*
 * What Chromium's and Firefox's libraries take of bionic besides functions, and the small functions its headers make
 * calls of: a time past 2038 through time64.h, the IPv6 constants, CPU_COUNT, CMSG_NXTHDR, sysinfo, getopt's globals.
 * 6, or the negated step that failed.
 */
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; long gmtoff; const char *zone; } t_tm;
typedef struct { void *name; unsigned namelen; void *iov; size_t iovlen; void *control; size_t controllen; int flags; } t_msghdr;
t_tm *gmtime64_r(const long long *, t_tm *);
long long timegm64(t_tm *);
int __sched_cpucount(size_t, const void *);
unsigned char *__cmsg_nxthdr(t_msghdr *, unsigned char *);
int sysinfo(unsigned *);
extern const unsigned char in6addr_any[16], in6addr_loopback[16];
extern int optind;

int t_libc_more(void)
{
    long long t = 4102444800LL;                      /* 2100-01-01 00:00:00 UTC */
    t_tm tm;
    unsigned char set[8] = { 0xB1, 0, 0, 0, 0, 0, 0, 0x80 };
    unsigned control[8] = { 16, 1, 1, 0, 16, 1, 1, 0 };
    t_msghdr m;
    unsigned info[16];
    int i;
    memset(&tm, 0, sizeof(tm));
    if (!gmtime64_r(&t, &tm) || tm.year != 200 || tm.mon != 0 || tm.mday != 1 || tm.hour != 0 || timegm64(&tm) != t) return -1;
    for (i = 0; i < 15; i++)
        if (in6addr_any[i] || in6addr_loopback[i]) return -2;
    if (in6addr_any[15] != 0 || in6addr_loopback[15] != 1) return -2;
    if (__sched_cpucount(sizeof(set), set) != 5) return -3;
    memset(&m, 0, sizeof(m));
    m.control = control;
    m.controllen = sizeof(control);
    if (__cmsg_nxthdr(&m, (unsigned char *)control) != (unsigned char *)&control[4] || __cmsg_nxthdr(&m, (unsigned char *)&control[4]) != 0) return -4;
    memset(info, 0xFF, sizeof(info));
    if (sysinfo(info) != 0 || info[4] == 0 || info[5] > info[4] || info[13] != 1) return -5;
    if (optind != 1) return -6;
    return 6;
}

/*
 * Shared memory, as Chromium copies its library's RELRO and Firefox talks to itself: ASharedMemory (libandroid) and
 * memfd_create (libc). Mapped, written and unmapped, what was written is there for the next mapping; its protection,
 * once narrowed, bounds every later mapping and is never widened again. 9, or the negated step that failed.
 */
int ASharedMemory_create(const char *, size_t);
size_t ASharedMemory_getSize(int);
int ASharedMemory_setProt(int, int);
int memfd_create(const char *, unsigned);
int ftruncate(int, long);
long lseek(int, long, int);

int t_shm(void)
{
    int fd = ASharedMemory_create("t_shm", 8192), m;
    unsigned char *p, *q;
    char back[4] = { 0 };
    if (fd < 0 || ASharedMemory_getSize(fd) != 8192) return -1;
    p = mmap(0, 8192, 3, 1, fd, 0);                  /* PROT_READ | PROT_WRITE, MAP_SHARED */
    if (p == (unsigned char *)-1 || p[100] != 0) return -2;
    p[100] = 77;
    if (munmap(p, 8192) != 0) return -3;
    if (ASharedMemory_setProt(fd, 1) != 0) return -4;  /* PROT_READ */
    if (mmap(0, 8192, 3, 1, fd, 0) != (void *)-1) return -5;
    q = mmap(0, 8192, 1, 1, fd, 0);
    if (q == (unsigned char *)-1 || q[100] != 77 || munmap(q, 8192) != 0) return -6;
    if (ASharedMemory_setProt(fd, 3) == 0) return -7;
    close(fd);
    m = memfd_create("t_memfd", 1);
    if (m < 0 || ftruncate(m, 100) != 0 || write(m, "abc", 3) != 3 || lseek(m, 0, 0) != 0) return -8;
    if (read(m, back, 3) != 3 || back[0] != 'a' || back[2] != 'c' || lseek(m, 98, 0) != 98 || write(m, "xyz", 3) != 2) return -9;
    close(m);
    return 9;
}

/*
 * ashmem as libcutils drives it (Chromium has a copy): /dev/ashmem opened, named and sized by ioctl, mapped; a region is
 * ashmem's character device, with /dev/ashmem's device number, which is how that code knows one -- a region from
 * ASharedMemory_create too; its protection mask through ioctl, narrowed and then binding mmap. And memfd's seals: one
 * against writing takes PROT_WRITE away. 10, or the negated step that failed.
 */
int ioctl(int, unsigned, ...);
int fcntl(int, int, ...);
int fstat(int, void *);
int stat(const char *, void *);

int t_ashmem(void)
{
    unsigned st[32], dev_rdev;
    int fd = open("/dev/ashmem", 2), shm, m;           /* O_RDWR */
    unsigned char *p;
    char name[256];
    if (fd < 0) return -1;
    if (ioctl(fd, 0x41007701, "t_ashmem") != 0 || ioctl(fd, 0x40047703, 4096) != 0 || ioctl(fd, 0x7704) != 4096) return -2;
    if (ioctl(fd, 0x81007702, name) != 0 || strcmp(name, "t_ashmem") != 0) return -3;
    if (stat("/dev/ashmem", st) != 0 || (st[4] & 0170000) != 0020000 || !st[8]) return -4;     /* S_IFCHR, st_rdev */
    dev_rdev = st[8];
    if (fstat(fd, st) != 0 || (st[4] & 0170000) != 0020000 || st[8] != dev_rdev) return -5;
    p = mmap(0, 4096, 3, 1, fd, 0);
    if (p == (unsigned char *)-1) return -6;
    p[7] = 9;
    munmap(p, 4096);
    if (ioctl(fd, 0x7706) != 7 || ioctl(fd, 0x40047705, 1) != 0 || ioctl(fd, 0x7706) != 1 || ioctl(fd, 0x40047705, 3) == 0) return -7;
    p = mmap(0, 4096, 1, 1, fd, 0);
    if (p == (unsigned char *)-1 || p[7] != 9 || mmap(0, 4096, 3, 1, fd, 0) != (void *)-1) return -8;
    munmap(p, 4096);
    close(fd);
    shm = ASharedMemory_create("t_ashmem2", 4096);
    if (shm < 0 || fstat(shm, st) != 0 || st[8] != dev_rdev || ioctl(shm, 0x7704) != 4096) return -9;
    close(shm);
    m = memfd_create("t_seals", 2);                           /* MFD_ALLOW_SEALING */
    if (m < 0 || ftruncate(m, 4096) != 0 || fcntl(m, 1034) != 0 || fcntl(m, 1033, 0x10) != 0 || fcntl(m, 1034) != 0x10 ||
        mmap(0, 4096, 3, 1, m, 0) != (void *)-1)
        return -10;
    close(m);
    return 10;
}

/*
 * mremap, as an allocator grows and shrinks a block it mapped: grown with MREMAP_MAYMOVE, the bytes are there and the
 * new pages zero; shrunk, it stays where it is; grown without leave to move, ENOMEM. 5, or the negated step that failed.
 */
void *mremap(void *, size_t, size_t, int, ...);

int t_mremap(void)
{
    unsigned char *p = mmap(0, 8192, 3, 0x22, -1, 0), *q;
    if (p == (unsigned char *)-1) return -1;
    p[0] = 1;
    p[8191] = 2;
    q = mremap(p, 8192, 5 * 4096, 1);
    if (q == (unsigned char *)-1 || q[0] != 1 || q[8191] != 2 || q[8192] != 0 || q[5 * 4096 - 1] != 0) return -2;
    q[5 * 4096 - 1] = 3;
    if (mremap(q, 5 * 4096, 4096, 0) != q || q[0] != 1) return -3;
    if (mremap(q, 4096, 3 * 4096, 0) != (void *)-1 || *__errno() != 12) return -4;
    if (munmap(q, 4096) != 0) return -5;
    return 5;
}

/*
 * Clocks of processor time, as Gecko's profiler and Chromium's tracing read them: pthread_getcpuclockid's clock for this
 * thread, which advances while the thread computes; CLOCK_THREAD_CPUTIME_ID; the process's through clock_getcpuclockid;
 * clock_getres for each; and the clock of a thread id nobody has, which is refused. 6, or the negated step that failed.
 */
pthread_t pthread_self(void);
int pthread_getcpuclockid(pthread_t, int *);
int clock_getres(int, struct t_timespec *);
int clock_getcpuclockid(int, int *);

int t_cpuclock(void)
{
    int c, p;
    struct t_timespec a, b, r;
    volatile unsigned x = 0;
    long until;
    unsigned i;
    if (pthread_getcpuclockid(pthread_self(), &c) != 0 || c >= 0 || !(c & 4)) return -1;
    if (clock_gettime(c, &a) != 0 || clock_getres(c, &r) != 0) return -2;
    /* (Windows adds to a thread's time at its clock's ticks, some 15.6 ms apart.) */
    for (until = now_ms() + 3000;;) {
        for (i = 0; i < 200000; i++) x += i;
        clock_gettime(c, &b);
        if (b.tv_sec != a.tv_sec || b.tv_nsec != a.tv_nsec || now_ms() > until) break;
    }
    if (b.tv_sec * 1000000000LL + b.tv_nsec <= a.tv_sec * 1000000000LL + a.tv_nsec) return -3;
    if (clock_gettime(3, &r) != 0 || r.tv_sec * 1000000000LL + r.tv_nsec < b.tv_sec * 1000000000LL + b.tv_nsec) return -4;
    if (clock_getcpuclockid(0, &p) != 0 || p >= 0 || (p & 4) || clock_gettime(p, &r) != 0 || clock_getres(p, &r) != 0) return -5;
    if (clock_gettime((int)((~32000u) << 3) | 6, &r) != -1) return -6;
    return 6;
}

/*
 * What Firefox's libraries import of libc that was not there: sigaction by its old system call, as mozglue installs its
 * handlers, and rt_sigaction (a handler for a fault's signal refused that way); syslog; fputwc, fgetwc and ungetwc in UTF-8; posix_fadvise and posix_fallocate; and
 * process_vm_readv on the process's own memory. 7, or the negated step that failed.
 */
long syscall(long, ...);
void openlog(const char *, int, int);
void syslog(int, const char *, ...);
void closelog(void);
unsigned fputwc(unsigned, void *);
unsigned fgetwc(void *);
unsigned ungetwc(unsigned, void *);
int posix_fadvise(int, long, long, int);
int posix_fallocate(int, long, long);
int process_vm_readv(int, const void *, unsigned long, const void *, unsigned long, unsigned long);
int getpid(void);

static void ff_handler(int sig) { (void)sig; }

int t_ffcalls(const char *dir)
{
    struct { void (*handler)(int); unsigned mask; int flags; void *restorer; } act, old;    /* bionic's, and the old call's */
    unsigned rt[5], st[32];
    char path[256], src[8] = "copied", dst[8] = { 0 };
    struct { void *base; size_t len; } local = { dst, 7 }, remote = { src, 7 };
    void *f;
    int fd;
    unsigned w;
    memset(&act, 0, sizeof(act));
    act.handler = ff_handler;
    memset(&old, 0, sizeof(old));
    if (syscall(67, 10, &act, 0) != 0 || syscall(67, 10, 0, &old) != 0 || old.handler != ff_handler) return -1;     /* SIGUSR1 */
    memset(rt, 0xEE, sizeof(rt));
    if (syscall(174, 10, 0, rt, 8) != 0 || rt[0] != (unsigned)(size_t)ff_handler) return -2;
    act.handler = 0;
    syscall(67, 10, &act, 0);
    act.handler = ff_handler;
    if (syscall(67, 11, &act, 0) != -1 || syscall(67, 11, 0, &old) != 0) return -2;     /* SIGSEGV: refused, asking allowed */
    openlog("t_basic", 0, 0);
    syslog(4, "syslog at %s, %d: %m", "warning", 7);
    closelog();
    snprintf(path, sizeof(path), "%s/wide.txt", dir);
    f = fopen(path, "w+");
    if (!f) return -3;
    if (fputwc(0x416, f) != 0x416 || fputwc('a', f) != 'a') return -4;
    fseek(f, 0, 0);
    w = fgetwc(f);
    if (w != 0x416 || ungetwc(w, f) != w || fgetwc(f) != 0x416 || fgetwc(f) != 'a' || fgetwc(f) != 0xFFFFFFFFu) return -5;
    fclose(f);
    fd = open(path, 2);
    if (fd < 0 || posix_fadvise(fd, 0, 0, 2) != 0 || posix_fallocate(fd, 0, 4096) != 0 || fstat(fd, st) != 0 || st[12] != 4096) return -6;
    close(fd);
    unlink(path);
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 7 || strcmp(dst, "copied") != 0) return -7;
    /* munmap by number (91), as Gecko's allocator calls it */
    {
        void *p = mmap(0, 4096, 3, 0x22, -1, 0);
        if (p == (void *)-1 || syscall(91, p, 4096) != 0) return -7;
    }
    return 7;
}

/*
 * The system's fonts as Gecko and Chromium enumerate them (API 29): an iterator, each font's file under /system/fonts
 * with a weight and a slant, and the end. Where there is no /system/fonts (the tests without an Android root) the
 * iterator is empty. The number of fonts, or the negated step that failed.
 */
void *ASystemFontIterator_open(void);
void *ASystemFontIterator_next(void *);
void ASystemFontIterator_close(void *);
void AFont_close(void *);
const char *AFont_getFontFilePath(const void *);
unsigned short AFont_getWeight(const void *);
int AFont_isItalic(const void *);
size_t AFont_getAxisCount(const void *);
int strncmp(const char *, const char *, size_t);

int t_fonts(void)
{
    void *it = ASystemFontIterator_open(), *f;
    int n = 0;
    if (!it) return -1;
    while ((f = ASystemFontIterator_next(it)) != 0) {
        const char *path = AFont_getFontFilePath(f);
        unsigned short w = AFont_getWeight(f);
        if (!path || strncmp(path, "/system/fonts/", 14) || w < 100 || w > 900 || AFont_getAxisCount(f) != 0) return -2;
        if (strstr(path, "Italic") && !AFont_isItalic(f)) return -3;
        AFont_close(f);
        n++;
    }
    ASystemFontIterator_close(it);
    return n;
}

/*
 * The command line a Chromium-based app reads at its start: /data/local/chrome-command-line exists (stat gives its size,
 * as File.exists and File.length ask), and it says single-process with the GPU in-process. 3, or the negated step.
 */
int t_chrome_cmdline(void)
{
    static const char want[] = "chrome --single-process --in-process-gpu\n";
    unsigned st[32];
    char text[128];
    int fd, n;
    if (stat("/data/local/chrome-command-line", st) != 0 || st[12] != sizeof(want) - 1) return -1;
    fd = open("/data/local/chrome-command-line", 0);
    if (fd < 0) return -2;
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n != (int)sizeof(want) - 1) return -3;
    text[n] = 0;
    return strcmp(text, want) == 0 ? 3 : -3;
}

/*
 * An IFUNC: a symbol the loader resolves at load time by running a resolver in the library's own code, which picks one of
 * several implementations (libc's string and memory routines are IFUNCs that pick a CPU-tuned version). The resolver is
 * the library's code, so the loader must have made the text executable before it runs it (hk_ld.c's two-pass relocate).
 * t_ifunc calls the resolved symbol; it must give the chosen implementation's answer (an argument plus 200).
 */
static volatile int t_ifunc_which = 1;   /* volatile: the resolver is a real runtime choice, not folded at link time */
static int t_ifunc_small(int x) { return x + 100; }
static int t_ifunc_big(int x) { return x + 200; }
typedef int (*t_ifunc_fn)(int);
static t_ifunc_fn t_ifunc_resolve(void) { return t_ifunc_which ? t_ifunc_big : t_ifunc_small; }
int t_ifunc_target(int x) __attribute__((ifunc("t_ifunc_resolve")));

int t_ifunc(void)
{
    return t_ifunc_target(5);   /* 205 */
}

/*
 * Byte-range locks as SQLite's WAL takes them on its -shm file: a write lock set (F_SETLK), then F_GETLK on the same
 * range, which a process's own locks never block: F_UNLCK, in the 32-bit struct and in flock64. 3, or the negated step.
 */
int t_flock(const char *dir)
{
    struct { short type, whence; long start, len; int pid; } lk;
    struct { short type, whence; int pad; long long start, len; int pid; } lk64;
    char path[256];
    int fd;
    snprintf(path, sizeof(path), "%s/lock.db-shm", dir);
    fd = open(path, 0102, 0600);                        /* O_RDWR | O_CREAT */
    if (fd < 0) return -1;
    memset(&lk, 0, sizeof(lk));
    lk.type = 1;                                         /* F_WRLCK */
    lk.start = 128;
    lk.len = 1;
    if (fcntl(fd, 6, &lk) != 0) return -1;               /* F_SETLK */
    lk.type = 1;
    if (fcntl(fd, 5, &lk) != 0 || lk.type != 2) return -2;                /* F_GETLK: F_UNLCK */
    memset(&lk64, 0, sizeof(lk64));
    lk64.type = 1;
    lk64.start = 120;
    lk64.len = 8;
    if (fcntl(fd, 12, &lk64) != 0 || lk64.type != 2) return -3;           /* F_GETLK64 */
    close(fd);
    unlink(path);
    return 3;
}

/*
 * fnmatch, which Chromium's FileEnumerator asks of every name it lists: the number of the 16 cases that came out as POSIX
 * and bionic have them.
 */
int fnmatch(const char *, const char *, int);

int t_fnmatch(void)
{
    static const struct { const char *pattern, *s; int flags, want; } cases[] = {
        { "*.pak", "resources.pak", 0, 0 },          { "*.pak", "resources.dat", 0, 1 },
        { "a?c", "abc", 0, 0 },                      { "a?c", "ac", 0, 1 },
        { "[a-c]x", "bx", 0, 0 },                    { "[!a-c]x", "bx", 0, 1 },
        { "[]]", "]", 0, 0 },                        { "[", "[", 0, 0 },
        { "\\*", "*", 0, 0 },                        { "\\*", "\\*", 1, 0 },     /* FNM_NOESCAPE */
        { "*", "a/b", 2, 1 },                        { "*/b", "a/b", 2, 0 },     /* FNM_PATHNAME */
        { "*", ".hidden", 4, 1 },                    { ".*", ".hidden", 4, 0 },  /* FNM_PERIOD */
        { "a", "a/b/c", 8, 0 },                      { "ABC", "abc", 0x10, 0 },  /* FNM_LEADING_DIR, FNM_CASEFOLD */
    };
    int i, right = 0;
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) right += fnmatch(cases[i].pattern, cases[i].s, cases[i].flags) == cases[i].want;
    return right;
}

/*
 * timerfd, as Chromium's message pump keeps its delayed work: nothing to read before its time, readable when it comes
 * (poll and epoll both wake for it, though nothing writes to it), the count of expirations read, a repeating one counting
 * each, an absolute time on CLOCK_MONOTONIC, and disarmed by a zero time. 8, or the negated step that failed.
 */
typedef struct { int interval_sec, interval_nsec, value_sec, value_nsec; } t_itimerspec;
typedef struct { int fd; short events, revents; } t_pollfd;
int timerfd_create(int, int);
int timerfd_settime(int, int, const t_itimerspec *, t_itimerspec *);
int timerfd_gettime(int, t_itimerspec *);
int poll(t_pollfd *, unsigned, int);
int epoll_create1(int);
int epoll_ctl(int, int, int, void *);
int epoll_wait(int, void *, int, int);

int t_timerfd(void)
{
    int fd = timerfd_create(1, 0x800 | 0x80000), ep;  /* CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC */
    unsigned long long n = 0;
    t_itimerspec it, cur;
    t_pollfd p;
    struct t_timespec now;
    struct { unsigned events; unsigned long long data; } ev[2];   /* ARM's epoll_event: the data at 8 */
    if (fd < 0) return -1;
    if (read(fd, &n, 8) != -1 || *__errno() != 11) return -2;            /* EAGAIN */
    memset(&it, 0, sizeof(it));
    it.value_nsec = 20 * 1000000;
    p.fd = fd;
    p.events = 1;
    p.revents = 0;
    if (timerfd_settime(fd, 0, &it, 0) != 0 || poll(&p, 1, 2000) != 1 || !(p.revents & 1) || read(fd, &n, 8) != 8 || n != 1) return -3;
    it.interval_nsec = it.value_nsec = 5 * 1000000;
    if (timerfd_settime(fd, 0, &it, 0) != 0) return -4;
    usleep(40 * 1000);
    if (read(fd, &n, 8) != 8 || n < 3) return -4;
    if (timerfd_gettime(fd, &cur) != 0 || cur.interval_nsec != 5 * 1000000 || cur.value_sec != 0 || cur.value_nsec <= 0) return -5;
    memset(&it, 0, sizeof(it));
    if (timerfd_settime(fd, 0, &it, 0) != 0 || timerfd_gettime(fd, &cur) != 0 || cur.value_sec || cur.value_nsec || read(fd, &n, 8) != -1) return -6;
    clock_gettime(1, &now);
    it.value_sec = (int)now.tv_sec;
    it.value_nsec = (int)now.tv_nsec + 10 * 1000000;
    if (it.value_nsec >= 1000000000) {
        it.value_sec++;
        it.value_nsec -= 1000000000;
    }
    ep = epoll_create1(0);
    memset(ev, 0, sizeof(ev));
    ev[0].events = 1;                                /* EPOLLIN */
    ev[0].data = 77;
    if (ep < 0 || epoll_ctl(ep, 1, fd, ev) != 0 || timerfd_settime(fd, 1, &it, 0) != 0) return -7;  /* EPOLL_CTL_ADD, TFD_TIMER_ABSTIME */
    if (epoll_wait(ep, ev, 2, 2000) != 1 || ev[0].data != 77 || read(fd, &n, 8) != 8 || n != 1) return -8;
    close(ep);
    close(fd);
    return 8;
}

/* A deep enough frame to need the stack protector's cookie, when built with it. */
int t_stack(const char *s)
{
    char buf[64];
    strcpy(buf, s);
    return (int)strlen(buf);
}
