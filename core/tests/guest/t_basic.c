/*
 * A guest library for the loader's tests: Android code, built with clang for
 * each ABI (tools/build-guest.sh): for armv7a-linux-androideabi once as Thumb
 * and once as ARM, for the others once as it is and once with packed
 * relocations. No NDK is needed -- everything it uses from Android is declared
 * here (and in t_abi.h, what differs between the ABIs) and bound by name when
 * the library is loaded.
 */
#include "t_abi.h"

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
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
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
    if (!b || ((size_t)b & 63)) ok = 0;
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
static pthread_mutex_t g_rlock = T_RECURSIVE_MUTEX_INITIALIZER;
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

/*
 * TLS_SLOT_APP, slot 2 of the thread pointer's block: the app's own from Android 10 on. Go's runtime keeps its g there,
 * through `mrc p15, 0, rX, c13, c0, 3` as below, when the device says API 29 or later (WhatsApp's libgojni.so). Empty on
 * a thread that has not set it, this one and a new one alike, kept as set, and left alone by what sets errno. 0 when so;
 * otherwise which step found it otherwise. (The thread pointer as each processor's bionic reads it: slot 0 of the block
 * is the block's own address on the x86s, where the segment's base is not to be read otherwise.)
 */
static void **tp_get(void)
{
    void **tp;
#if defined(__arm__)
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
#elif defined(__aarch64__)
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
#elif defined(__x86_64__)
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
#else
    __asm__ volatile("mov %%gs:0, %0" : "=r"(tp));
#endif
    return tp;
}

static int app_slot_steps(int base)
{
    void **tp = tp_get();
    if (tp[2] != 0) return base + 1;
    tp[2] = (void *)(long)(0x51525354 + base);
    if (close(-1) != -1 || *__errno() != 9 /* EBADF */) {
        tp[2] = 0;
        return base + 2;
    }
    if (tp[2] != (void *)(long)(0x51525354 + base)) return base + 3;
    tp[2] = 0;
    return 0;
}

static void *app_slot_thread(void *arg) { return (void *)(long)app_slot_steps((int)(long)arg); }

int t_tls_app_slot(void)
{
    pthread_t t;
    void *ret = (void *)-1L;
    int r = app_slot_steps(10);
    if (r) return r;
    if (pthread_create(&t, 0, app_slot_thread, (void *)20L) != 0) return 30;
    pthread_join(t, &ret);
    return (int)(long)ret;
}

/*
 * What a library's init functions are given, looked at as Go's runtime looks at it when it is loaded as a library:
 * argv, envp right after argv's NULL, and past envp's NULL the auxiliary vector, with the page size and the address of
 * 16 random bytes in it.
 */
static int g_init_argc = -1, g_init_env_after_argv, g_init_pagesz, g_init_random;
static t_word g_init_vdso;

__attribute__((constructor)) static void auxv_probe(int argc, char **argv, char **envp)
{
    const t_word *auxv;
    int n = argc + 1, i;
    g_init_argc = argc;
    g_init_env_after_argv = envp == argv + argc + 1;
    if (!g_init_env_after_argv) return;
    while (argv[n] && n < 16) n++;
    n++;
    auxv = (const t_word *)&argv[n];
    for (i = 0; auxv[i] && i < 64; i += 2) {
        if (auxv[i] == 6) g_init_pagesz = (int)auxv[i + 1];
        if (auxv[i] == 33) g_init_vdso = auxv[i + 1];
        if (auxv[i] == 25 && auxv[i + 1]) {
            const unsigned char *r = (const unsigned char *)(size_t)auxv[i + 1];
            int k, any = 0;
            for (k = 0; k < 16; k++) any |= r[k];
            g_init_random = 1 + (any != 0);
        }
    }
}

int t_init_auxv(void) { return g_init_argc == 1 && g_init_env_after_argv && g_init_pagesz == 4096 && g_init_random == 2; }

int t_tls_get(void) { return tls_counter; }
void t_tls_set(int v) { tls_counter = v; }
int *t_tls_addr(void) { return &tls_counter; }

/* ----------------------------------------------------------------- files */

/*
 * Stopping the world, the way a garbage collector does it: a signal to every
 * thread, whose handler says "stopped" and waits in sigsuspend for the signal
 * that lets it go on. One thread is in a loop that never calls anything, one
 * waits on a semaphore, one sleeps: the three places a thread can be found.
 * (bionic's struct sigaction: the kernel's on the 32-bit ABIs, the flags first
 * on the 64-bit ones; its sem_t, 16 bytes there.)
 */
#if defined(__LP64__)
typedef struct {
    int flags;
    void (*handler)(int, void *, void *);
    t_word mask;
    void (*restorer)(void);
} t_sigaction;
#define T_SIGACTION(handler, mask, flags) { flags, handler, mask, 0 }
typedef struct { unsigned count; int reserved[3]; } t_sem;
#else
typedef struct {
    void (*handler)(int, void *, void *);
    unsigned mask;
    int flags;
    void (*restorer)(void);
} t_sigaction;
#define T_SIGACTION(handler, mask, flags) { handler, mask, flags, 0 }
typedef struct { unsigned count; } t_sem;
#endif
int sigaction(int, const t_sigaction *, t_sigaction *);
int sigsuspend(const t_word *);
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
    t_word all_but_go = ~((t_word)1 << (SIG_GO - 1));
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
    t_sigaction stop = T_SIGACTION(on_stop, ~((t_word)1 << (SIG_GO - 1)), 4 | 0x10000000), go = T_SIGACTION(on_go, ~(t_word)0, 4 | 0x10000000);
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

/* /proc/cpuinfo says what the processor has that the ABI's code looks for: NEON on ARM, ASIMD on ARM64, SSE4.2 and SSSE3
   on x86_64 and x86 (what their ABIs promise). 1 when it does. */
#if defined(__aarch64__)
#define CPU_FEATURE "asimd"
#elif defined(__x86_64__)
#define CPU_FEATURE "sse4_2"
#elif defined(__i386__)
#define CPU_FEATURE "ssse3"
#endif

int t_proc(void)
{
    char line[256];
    void *f = fopen("/proc/cpuinfo", "r");
    int found = 0;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        int i;
        for (i = 0; line[i]; i++) {
#if defined(__arm__)
            if (line[i] == 'n' && line[i + 1] == 'e' && line[i + 2] == 'o' && line[i + 3] == 'n') found = 1;
#else
            int k;
            for (k = 0; CPU_FEATURE[k] && line[i + k] == CPU_FEATURE[k]; k++) {}
            if (!CPU_FEATURE[k]) found = 1;
#endif
        }
    }
    fclose(f);
    return found;
}

/* The first line of a file, or "" (Facebook reads the kernel's boot UUID). */
static void first_line(const char *path, char *line, int n)
{
    void *f = fopen(path, "r");
    line[0] = 0;
    if (!f) return;
    if (!fgets(line, n, f)) line[0] = 0;
    fclose(f);
}

static int uuid_shaped(const char *s)
{
    int i;
    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return 0;
        } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return 0;
        }
    }
    return s[36] == '\n';
}

/* The kernel's UUIDs: boot_id the same at every read, uuid another at every read, both shaped as UUIDs. 1 when so. */
int t_boot_id(void)
{
    char a[64], b[64], u1[64], u2[64];
    int i, same = 1, differ = 0;
    first_line("/proc/sys/kernel/random/boot_id", a, sizeof(a));
    first_line("/proc/sys/kernel/random/boot_id", b, sizeof(b));
    first_line("/proc/sys/kernel/random/uuid", u1, sizeof(u1));
    first_line("/proc/sys/kernel/random/uuid", u2, sizeof(u2));
    for (i = 0; i < 37; i++) {
        if (a[i] != b[i]) same = 0;
        if (u1[i] != u2[i]) differ = 1;
    }
    return uuid_shaped(a) && uuid_shaped(u1) && uuid_shaped(u2) && same && differ;
}

/* A line of /proc/self/status that starts with `key`, as the kernel writes it ("VmHWM:\t    1234 kB"): its number of
   kB, or -1. */
static long status_kb(const char *key)
{
    char line[256];
    void *f = fopen("/proc/self/status", "r");
    long kb = -1;
    if (!f) return -1;
    while (kb < 0 && fgets(line, sizeof(line), f)) {
        int i = 0;
        long n = 0;
        while (key[i] && line[i] == key[i]) i++;
        if (key[i] || line[i] != ':') continue;
        i++;
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (line[i] < '0' || line[i] > '9') continue;
        while (line[i] >= '0' && line[i] <= '9') n = n * 10 + (line[i++] - '0');
        if (line[i] == ' ' && line[i + 1] == 'k' && line[i + 2] == 'B') kb = n;
    }
    fclose(f);
    return kb;
}

/* /proc/self/status's memory lines, as Google Play services' memory metric reads them: VmHWM, VmRSS, VmPeak, VmSize
   and VmSwap there, the high-water mark not below the resident size, the peak not below the size. 1 when so. */
int t_proc_status(void)
{
    long hwm = status_kb("VmHWM"), rss = status_kb("VmRSS"), peak = status_kb("VmPeak"), size = status_kb("VmSize");
    return hwm >= 0 && rss >= 0 && hwm >= rss && peak >= size && size >= 0 && status_kb("VmSwap") == 0;
}

/* /proc/self/smaps, as Instagram reads it for the page size: the first "KernelPageSize:" line's number of kB (4), or -1. */
int t_smaps(void)
{
    char line[256];
    void *f = fopen("/proc/self/smaps", "r");
    int size = -1;
    if (!f) return -1;
    while (size < 0 && fgets(line, sizeof(line), f)) {
        const char *key = "KernelPageSize:";
        int i = 0, n = 0;
        while (key[i] && line[i] == key[i]) i++;
        if (key[i]) continue;
        while (line[i] == ' ') i++;
        while (line[i] >= '0' && line[i] <= '9') n = n * 10 + (line[i++] - '0');
        if (line[i] == ' ' && line[i + 1] == 'k' && line[i + 2] == 'B') size = n;
    }
    fclose(f);
    return size;
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
 * The linker's entry points that Android 8's libdl.so calls, looked up in libdl.so as Facebook's apps look them up (Instagram's
 * __loader_dlopen): a library opened and a function found with a caller's address, and a library that is not there refused
 * with an error to read.
 */
int t_loader_dl(void)
{
    void *dl = dlopen("libdl.so", 0), *libc;
    void *(*open_as)(const char *, int, const void *) = dl ? dlsym(dl, "__loader_dlopen") : 0;
    void *(*sym_as)(void *, const char *, const void *) = dl ? dlsym(dl, "__loader_dlsym") : 0;
    char *(*error)(void) = dl ? dlsym(dl, "__loader_dlerror") : 0;
    int (*len)(const char *);
    if (!open_as || !sym_as || !error) return -1;
    libc = open_as("libc.so", 0, (const void *)t_loader_dl);
    len = libc ? sym_as(libc, "strlen", (const void *)t_loader_dl) : 0;
    if (!len) return -2;
    if (open_as("libno-such-thing.so", 0, (const void *)t_loader_dl) || !error()) return -3;
    return len("seven");
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
 * 6, or the negated step that failed. (time64.h is the 32-bit ABIs' alone: on the 64-bit ones time_t is 64 bits, and
 * the time is gmtime_r's and timegm's. A cmsghdr's length is a size_t: 12 bytes of header on the 32-bit ABIs, 16 on the
 * 64-bit ones.)
 */
typedef struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; long gmtoff; const char *zone; } t_tm;
typedef struct { void *name; unsigned namelen; void *iov; size_t iovlen; void *control; size_t controllen; int flags; } t_msghdr;
#if defined(__LP64__)
t_tm *gmtime_r(const long *, t_tm *);
long timegm(t_tm *);
#define gmtime64_r gmtime_r
#define timegm64 timegm
#else
t_tm *gmtime64_r(const long long *, t_tm *);
long long timegm64(t_tm *);
#endif
int __sched_cpucount(size_t, const void *);
unsigned char *__cmsg_nxthdr(t_msghdr *, unsigned char *);
int sysinfo(t_word *);
extern const unsigned char in6addr_any[16], in6addr_loopback[16];
extern int optind;

int t_libc_more(void)
{
#if defined(__LP64__)
    long t = 4102444800L;                            /* 2100-01-01 00:00:00 UTC */
#else
    long long t = 4102444800LL;                      /* 2100-01-01 00:00:00 UTC */
#endif
    t_tm tm;
    unsigned char set[8] = { 0xB1, 0, 0, 0, 0, 0, 0, 0x80 };
#if defined(__LP64__)
    unsigned control[8] = { 16, 0, 1, 1, 16, 0, 1, 1 };
#else
    unsigned control[8] = { 16, 1, 1, 0, 16, 1, 1, 0 };
#endif
    t_msghdr m;
    t_word info[16];
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
    if (sysinfo(info) != 0 || info[4] == 0 || info[5] > info[4] || (unsigned)info[13] != 1) return -5;   /* totalram, freeram, mem_unit */
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
 * Two mappings of one region of shared memory are the same memory, as Chromium's Mojo maps a data pipe's region in the
 * writer and again in the reader: what one view writes the other reads at once, and so does read() on the descriptor;
 * what pwrite() writes both views see; a view outlives the other's munmap and the descriptor's close. The same for a
 * memfd sized by ftruncate. 6, or the negated step that failed.
 */
long pwrite(int, const void *, size_t, long);
long pread(int, void *, size_t, long);

int t_shm_views(void)
{
    int fd = ASharedMemory_create("t_shm_views", 16384), m;
    unsigned char *a, *b;
    char back[2] = { 0 };
    if (fd < 0) return -1;
    a = mmap(0, 16384, 3, 1, fd, 0);                  /* PROT_READ | PROT_WRITE, MAP_SHARED */
    b = mmap(0, 16384, 1, 1, fd, 0);                  /* PROT_READ */
    if (a == (unsigned char *)-1 || b == (unsigned char *)-1 || a == b) return -2;
    a[5000] = 42;
    if (b[5000] != 42 || pread(fd, back, 1, 5000) != 1 || back[0] != 42) return -3;
    if (pwrite(fd, "Q", 1, 16000) != 1 || a[16000] != 'Q' || b[16000] != 'Q') return -4;
    munmap(a, 16384);
    close(fd);
    if (b[5000] != 42 || b[16000] != 'Q') return -5;
    munmap(b, 16384);
    m = memfd_create("t_shm_views2", 1);
    if (m < 0 || ftruncate(m, 8192) != 0) return -6;
    a = mmap(0, 8192, 3, 1, m, 0);
    b = mmap(0, 8192, 3, 1, m, 0);
    if (a == (unsigned char *)-1 || b == (unsigned char *)-1) return -6;
    b[8191] = 7;
    if (a[8191] != 7) return -6;
    munmap(a, 8192);
    munmap(b, 8192);
    close(m);
    return 6;
}

/*
 * ashmem as libcutils drives it (Chromium has a copy): /dev/ashmem opened, named and sized by ioctl, mapped; a region is
 * ashmem's character device, with /dev/ashmem's device number, which is how that code knows one -- a region from
 * ASharedMemory_create too; its protection mask through ioctl, narrowed and then binding mmap. And memfd's seals: one
 * against writing takes PROT_WRITE away. 10, or the negated step that failed. (ASHMEM_SET_SIZE and ASHMEM_SET_PROT_MASK
 * carry the size of a size_t and a long in their numbers: 4 on the 32-bit ABIs, 8 on the 64-bit ones.)
 */
int ioctl(int, unsigned, ...);
int fcntl(int, int, ...);
int fstat(int, void *);
int stat(const char *, void *);

/* struct stat as words of an array, laid out as bionic's of each processor: its st_mode, st_rdev's low word and st_size's. */
#if defined(__x86_64__)
enum { ST_WORDS = 36, ST_MODE = 6, ST_RDEV = 10, ST_SIZE = 12 };
#elif defined(__i386__)
enum { ST_WORDS = 32, ST_MODE = 4, ST_RDEV = 8, ST_SIZE = 11 };   /* (a long long is 4-aligned there) */
#else
enum { ST_WORDS = 32, ST_MODE = 4, ST_RDEV = 8, ST_SIZE = 12 };
#endif
enum { ASHMEM_SET_SIZE = 0x40007703 | (int)sizeof(size_t) << 16, ASHMEM_SET_PROT_MASK = 0x40007705 | (int)sizeof(long) << 16 };

int t_ashmem(void)
{
    unsigned st[ST_WORDS], dev_rdev;
    int fd = open("/dev/ashmem", 2), shm, m;           /* O_RDWR */
    unsigned char *p;
    char name[256];
    if (fd < 0) return -1;
    if (ioctl(fd, 0x41007701, "t_ashmem") != 0 || ioctl(fd, ASHMEM_SET_SIZE, 4096) != 0 || ioctl(fd, 0x7704) != 4096) return -2;
    if (ioctl(fd, 0x81007702, name) != 0 || strcmp(name, "t_ashmem") != 0) return -3;
    if (stat("/dev/ashmem", st) != 0 || (st[ST_MODE] & 0170000) != 0020000 || !st[ST_RDEV]) return -4;     /* S_IFCHR, st_rdev */
    dev_rdev = st[ST_RDEV];
    if (fstat(fd, st) != 0 || (st[ST_MODE] & 0170000) != 0020000 || st[ST_RDEV] != dev_rdev) return -5;
    p = mmap(0, 4096, 3, 1, fd, 0);
    if (p == (unsigned char *)-1) return -6;
    p[7] = 9;
    munmap(p, 4096);
    if (ioctl(fd, 0x7706) != 7 || ioctl(fd, ASHMEM_SET_PROT_MASK, 1) != 0 || ioctl(fd, 0x7706) != 1 || ioctl(fd, ASHMEM_SET_PROT_MASK, 3) == 0) return -7;
    p = mmap(0, 4096, 1, 1, fd, 0);
    if (p == (unsigned char *)-1 || p[7] != 9 || mmap(0, 4096, 3, 1, fd, 0) != (void *)-1) return -8;
    munmap(p, 4096);
    close(fd);
    shm = ASharedMemory_create("t_ashmem2", 4096);
    if (shm < 0 || fstat(shm, st) != 0 || st[ST_RDEV] != dev_rdev || ioctl(shm, 0x7704) != 4096) return -9;
    close(shm);
    m = memfd_create("t_seals", 2);                           /* MFD_ALLOW_SEALING */
    if (m < 0 || ftruncate(m, 4096) != 0 || fcntl(m, 1034) != 0 || fcntl(m, 1033, 0x10) != 0 || fcntl(m, 1034) != 0x10 ||
        mmap(0, 4096, 3, 1, m, 0) != (void *)-1)
        return -10;
    close(m);
    return 10;
}

/*
 * Surface control (Android 10's ASurfaceControl, libandroid), as Chromium asks for it: by dlsym, every function there; no
 * layer can be made (no system compositor), a transaction can, and a buffer's fence given to it is closed, as a
 * transaction that took it would close it. 4, or the negated step that failed.
 */
int t_surface_control(void)
{
    static const char *const names[] = {
        "ASurfaceControl_createFromWindow", "ASurfaceControl_create", "ASurfaceControl_release", "ASurfaceTransaction_create",
        "ASurfaceTransaction_delete", "ASurfaceTransaction_apply", "ASurfaceTransaction_setOnComplete", "ASurfaceTransaction_reparent",
        "ASurfaceTransaction_setVisibility", "ASurfaceTransaction_setZOrder", "ASurfaceTransaction_setBuffer",
        "ASurfaceTransaction_setGeometry", "ASurfaceTransaction_setBufferTransparency", "ASurfaceTransaction_setDamageRegion",
        "ASurfaceTransaction_setBufferDataSpace", "ASurfaceTransaction_setHdrMetadata_cta861_3",
        "ASurfaceTransaction_setHdrMetadata_smpte2086", "ASurfaceTransactionStats_getPresentFenceFd",
        "ASurfaceTransactionStats_getLatchTime", "ASurfaceTransactionStats_getASurfaceControls",
        "ASurfaceTransactionStats_releaseASurfaceControls", "ASurfaceTransactionStats_getPreviousReleaseFenceFd",
    };
    void *android = dlopen("libandroid.so", 0), *t;
    void *(*from_window)(void *, const char *), *(*create)(void);
    void (*set_buffer)(void *, void *, void *, int), (*del)(void *);
    int fds[2];
    unsigned i;
    if (!android) return -1;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!dlsym(android, names[i])) return -1;
    from_window = (void *(*)(void *, const char *))dlsym(android, "ASurfaceControl_createFromWindow");
    if (from_window(0, "t") != 0) return -2;
    create = (void *(*)(void))dlsym(android, "ASurfaceTransaction_create");
    set_buffer = (void (*)(void *, void *, void *, int))dlsym(android, "ASurfaceTransaction_setBuffer");
    del = (void (*)(void *))dlsym(android, "ASurfaceTransaction_delete");
    t = create();
    if (!t || pipe(fds) != 0) return -3;
    set_buffer(t, 0, 0, fds[0]);
    if (close(fds[0]) != -1) return -4;
    close(fds[1]);
    del(t);
    return 4;
}

/*
 * Hardware buffers (AHardwareBuffer, libandroid), as Chromium asks for them: by dlsym, a 64x64 R8G8B8A8 one for the
 * graphics processor, and read as ANGLE reads one, through the ANativeWindowBuffer that is two pointers into it
 * (android_util.cpp in AOSP's external/angle): its magic, width, height, format, layer count and usage there; a format no
 * texture is made of refused with BAD_VALUE; written and read back through a lock; released. 6, or the negated step that
 * failed.
 */
int t_hardware_buffer(void)
{
    struct desc { unsigned width, height, layers, format; unsigned long long usage; unsigned stride, rfu0; unsigned long long rfu1; };
    struct anwb {   /* Android's ANativeWindowBuffer, as ANGLE declares it: 96 bytes on the 32-bit ABIs, 168 on the 64-bit ones */
        int magic, version;
        void *reserved[4];
        void (*inc_ref)(void *), (*dec_ref)(void *);
        int width, height, stride, format, usage_deprecated;
        __UINTPTR_TYPE__ layer_count;
        void *reserved1;
        const void *handle;
        unsigned long long usage;
        void *reserved_proc[8 - sizeof(unsigned long long) / sizeof(void *)];
    };
    struct desc want = { 64, 64, 1, 1, 0x333, 0, 0, 0 }, fp16 = { 64, 64, 1, 0x16, 0x300, 0, 0, 0 }, got;
    void *android = dlopen("libandroid.so", 0), *buffer = 0, *none = (void *)1;
    unsigned char *px = 0;
    const struct anwb *w;
    int (*allocate)(const void *, void **), (*supported)(const void *), (*lock)(void *, unsigned long long, int, const void *, void **);
    int (*unlock)(void *, int *);
    void (*describe)(const void *, void *), (*release)(void *);
    if (!android) return -1;
    allocate = (int (*)(const void *, void **))dlsym(android, "AHardwareBuffer_allocate");
    supported = (int (*)(const void *))dlsym(android, "AHardwareBuffer_isSupported");
    lock = (int (*)(void *, unsigned long long, int, const void *, void **))dlsym(android, "AHardwareBuffer_lock");
    unlock = (int (*)(void *, int *))dlsym(android, "AHardwareBuffer_unlock");
    describe = (void (*)(const void *, void *))dlsym(android, "AHardwareBuffer_describe");
    release = (void (*)(void *))dlsym(android, "AHardwareBuffer_release");
    if (!allocate || !supported || !lock || !unlock || !describe || !release) return -1;
    if (supported(&want) != 1 || supported(&fp16) != 0 || allocate(&fp16, &none) != -22 || none) return -2;
    if (allocate(&want, &buffer) != 0 || !buffer) return -3;
    w = (const struct anwb *)((const char *)buffer + 2 * sizeof(void *));
    if (sizeof(struct anwb) != (sizeof(void *) == 8 ? 168 : 96) || w->magic != 0x5f626672 || w->version != (int)sizeof(struct anwb) ||
        w->width != 64 || w->height != 64 || w->stride != 64 ||
        w->format != 1 || w->layer_count != 1 || w->usage != 0x333)
        return -4;
    describe(buffer, &got);
    if (got.width != 64 || got.height != 64 || got.layers != 1 || got.format != 1 || got.stride != 64) return -5;
    if (lock(buffer, 0x30, -1, 0, (void **)&px) != 0 || !px) return -6;
    px[0] = 0x5a;
    px[64 * 64 * 4 - 1] = 0xa5;
    if (unlock(buffer, 0) != 0 || lock(buffer, 0x3, -1, 0, (void **)&px) != 0 || px[0] != 0x5a || px[64 * 64 * 4 - 1] != 0xa5) return -6;
    unlock(buffer, 0);
    release(buffer);
    return 6;
}

/*
 * AAudio (libaaudio), as Chromium and Oboe start a stream: a builder first, AAUDIO_OK and a builder (ndk/mg_aaudio.c; its
 * streams are tested by t_aaudio.c), and the builder's setUsage, which Chromium binds weakly, is there. 2, or the negated
 * step that failed.
 */
int t_aaudio(void)
{
    void *aaudio = dlopen("libaaudio.so", 0), *builder = 0;
    int (*create)(void **), (*destroy)(void *);
    if (!aaudio) return -1;
    create = (int (*)(void **))dlsym(aaudio, "AAudio_createStreamBuilder");
    destroy = (int (*)(void *))dlsym(aaudio, "AAudioStreamBuilder_delete");
    if (!create || !destroy || !dlsym(aaudio, "AAudioStreamBuilder_setUsage")) return -1;
    if (create(&builder) != 0 || !builder) return -2;
    destroy(builder);
    return 2;
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
 * mmap's address without MAP_FIXED, a hint: followed when the range is free, as Linux follows it, and not when it is taken
 * (another address is given, not a failure). The range asked for is a 2 MB one inside 4 MB this test reserved and gave
 * back, as PartitionAlloc picks its super pages. 3, or the negated step that failed.
 */
int t_mmap_hint(void)
{
    unsigned char *p = mmap(0, 4 << 20, 0, 0x22, -1, 0), *want, *q, *r;
    if (p == (unsigned char *)-1) return -1;
    want = (unsigned char *)(((unsigned long)p + (2 << 20) - 1) & ~((2ul << 20) - 1));
    if (munmap(p, 4 << 20) != 0) return -1;
    q = mmap(want, 2 << 20, 0, 0x22, -1, 0);
    if (q != want) return -2;
    r = mmap(want, 4096, 3, 0x22, -1, 0);
    if (r == (unsigned char *)-1 || r == want) return -3;
    r[0] = 1;
    munmap(r, 4096);
    munmap(q, 2 << 20);
    return 3;
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
 * process_vm_readv on the process's own memory. 7, or the negated step that failed. (The old sigaction is the 32-bit
 * ABIs' alone; the kernel's rt_sigaction struct is a handler, flags, a restorer and the mask everywhere, in words.)
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
ssize_t process_vm_readv(int, const void *, unsigned long, const void *, unsigned long, unsigned long);
int getpid(void);

/* The system calls these tests make by number, in each processor's numbering (the kernel's unistd.h of each). */
#if defined(__arm__)
enum { NR_sigaction = 67, NR_rt_sigaction = 174, NR_munmap = 91, NR_timerfd_create = 350, NR_timerfd_settime = 353, NR_timerfd_gettime = 354,
       NR_nanosleep = 162, NR_rt_sigprocmask = 175, NR_madvise = 220, NR_mincore = 219, NR_epoll_pwait = 346, NR_fcntl = 55, NR_fallocate = 352,
       NR_socket = 281, NR_setsockopt = 294, NR_bind = 282, NR_listen = 284, NR_getsockname = 286, NR_connect = 283, NR_poll = 168,
       NR_accept4 = 366, NR_sendto = 290, NR_recvfrom = 292, NR_send = 289, NR_recv = 291, NR_getpeername = 287, NR_getsockopt = 295,
       NR_shutdown = 293, NR_socketpair = 288, NR_sendmsg = 296, NR_recvmsg = 297, NR_splice = 340, NR_flock = 143, NR_ftruncate = 194,
       NR_pwrite64 = 181, NR_pread64 = 180, NR_fdatasync = 148, NR_getdents64 = 217, NR_prlimit64 = 369, NR_uname = 122, NR_getuid = 199,
       NR_getppid = 64 };
#elif defined(__i386__)
enum { NR_sigaction = 67, NR_rt_sigaction = 174, NR_munmap = 91, NR_timerfd_create = 322, NR_timerfd_settime = 325, NR_timerfd_gettime = 326,
       NR_nanosleep = 162, NR_rt_sigprocmask = 175, NR_madvise = 219, NR_mincore = 218, NR_epoll_pwait = 319, NR_fcntl = 55, NR_fallocate = 324,
       NR_socket = 359, NR_setsockopt = 366, NR_bind = 361, NR_listen = 363, NR_getsockname = 367, NR_connect = 362, NR_poll = 168,
       NR_accept4 = 364, NR_sendto = 369, NR_recvfrom = 371, NR_getpeername = 368, NR_getsockopt = 365, NR_shutdown = 373, NR_socketpair = 360,
       NR_sendmsg = 370, NR_recvmsg = 372, NR_splice = 313, NR_flock = 143, NR_ftruncate = 194, NR_pwrite64 = 181, NR_pread64 = 180,
       NR_fdatasync = 148, NR_getdents64 = 220, NR_prlimit64 = 340, NR_uname = 122, NR_getuid = 199, NR_getppid = 64 };
#elif defined(__x86_64__)
enum { NR_rt_sigaction = 13, NR_munmap = 11, NR_timerfd_create = 283, NR_timerfd_settime = 286, NR_timerfd_gettime = 287, NR_nanosleep = 35,
       NR_rt_sigprocmask = 14, NR_madvise = 28, NR_mincore = 27, NR_epoll_pwait = 281, NR_fcntl = 72, NR_fallocate = 285, NR_socket = 41,
       NR_setsockopt = 54, NR_bind = 49, NR_listen = 50, NR_getsockname = 51, NR_connect = 42, NR_poll = 7, NR_accept4 = 288, NR_sendto = 44,
       NR_recvfrom = 45, NR_getpeername = 52, NR_getsockopt = 55, NR_shutdown = 48, NR_socketpair = 53, NR_sendmsg = 46, NR_recvmsg = 47,
       NR_splice = 275, NR_flock = 73, NR_ftruncate = 77, NR_pwrite64 = 18, NR_pread64 = 17, NR_fdatasync = 75, NR_getdents64 = 217,
       NR_prlimit64 = 302, NR_uname = 63, NR_getuid = 102, NR_getppid = 110 };
#else
enum { NR_rt_sigaction = 134, NR_munmap = 215, NR_timerfd_create = 85, NR_timerfd_settime = 86, NR_timerfd_gettime = 87, NR_nanosleep = 101,
       NR_rt_sigprocmask = 135, NR_madvise = 233, NR_mincore = 232, NR_epoll_pwait = 22, NR_fcntl = 25, NR_fallocate = 47, NR_socket = 198,
       NR_setsockopt = 208, NR_bind = 200, NR_listen = 201, NR_getsockname = 204, NR_connect = 203, NR_ppoll = 73, NR_accept4 = 242,
       NR_sendto = 206, NR_recvfrom = 207, NR_getpeername = 205, NR_getsockopt = 209, NR_shutdown = 210, NR_socketpair = 199, NR_sendmsg = 211,
       NR_recvmsg = 212, NR_splice = 76, NR_flock = 32, NR_ftruncate = 46, NR_pwrite64 = 68, NR_pread64 = 67, NR_fdatasync = 83,
       NR_getdents64 = 61, NR_prlimit64 = 261, NR_uname = 160, NR_getuid = 174, NR_getppid = 173 };
#endif

static void ff_handler(int sig) { (void)sig; }

int t_ffcalls(const char *dir)
{
#if !defined(__LP64__)
    struct { void (*handler)(int); unsigned mask; int flags; void *restorer; } act, old;    /* bionic's, and the old call's */
#endif
    t_word rt[5];
    unsigned st[ST_WORDS];
    char path[256], src[8] = "copied", dst[8] = { 0 };
    struct { void *base; size_t len; } local = { dst, 7 }, remote = { src, 7 };
    void *f;
    int fd;
    unsigned w;
#if !defined(__LP64__)
    memset(&act, 0, sizeof(act));
    act.handler = ff_handler;
    memset(&old, 0, sizeof(old));
    if (syscall(NR_sigaction, 10, &act, 0) != 0 || syscall(NR_sigaction, 10, 0, &old) != 0 || old.handler != ff_handler) return -1;     /* SIGUSR1 */
    memset(rt, 0xEE, sizeof(rt));
    if (syscall(NR_rt_sigaction, 10, 0, rt, 8) != 0 || rt[0] != (t_word)(size_t)ff_handler) return -2;
    act.handler = 0;
    syscall(NR_sigaction, 10, &act, 0);
    act.handler = ff_handler;
    if (syscall(NR_sigaction, 11, &act, 0) != -1 || syscall(NR_sigaction, 11, 0, &old) != 0) return -2;     /* SIGSEGV: refused, asking allowed */
#else
    {
        /* SIGUSR1 by rt_sigaction alone, set and asked for. */
        t_word rta[5] = { (t_word)(size_t)ff_handler, 0, 0, 0, 0 };
        memset(rt, 0xEE, sizeof(rt));
        if (syscall(NR_rt_sigaction, 10, rta, 0, 8) != 0 || syscall(NR_rt_sigaction, 10, 0, rt, 8) != 0 || rt[0] != (t_word)(size_t)ff_handler) return -1;
        rta[0] = 0;
        syscall(NR_rt_sigaction, 10, rta, 0, 8);
    }
#endif
    {
        /* rt_sigaction takes one, as Go's runtime installs its SIGBUS handler (and ends when it is refused). */
        t_word rta[5] = { (t_word)(size_t)ff_handler, 0, 0, 0, 0 };
        memset(rt, 0, sizeof(rt));
        if (syscall(NR_rt_sigaction, 7, rta, 0, 8) != 0 || syscall(NR_rt_sigaction, 7, 0, rt, 8) != 0 || rt[0] != (t_word)(size_t)ff_handler) return -2;
        rta[0] = 0;
        syscall(NR_rt_sigaction, 7, rta, 0, 8);
    }
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
    if (fd < 0 || posix_fadvise(fd, 0, 0, 2) != 0 || posix_fallocate(fd, 0, 4096) != 0 || fstat(fd, st) != 0 || st[ST_SIZE] != 4096) return -6;
    close(fd);
    unlink(path);
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != 7 || strcmp(dst, "copied") != 0) return -7;
    /* munmap by number (91 on ARM), as Gecko's allocator calls it */
    {
        void *p = mmap(0, 4096, 3, 0x22, -1, 0);
        if (p == (void *)-1 || syscall(NR_munmap, p, 4096) != 0) return -7;
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
    unsigned st[ST_WORDS];
    char text[128];
    int fd, n;
    if (stat("/data/local/chrome-command-line", st) != 0 || st[ST_SIZE] != sizeof(want) - 1) return -1;
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
 * the library's code, so the loader must have made the text executable before it runs it (mg_ld.c's two-pass relocate).
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
 * (On the 64-bit ABIs struct flock is flock64 already, and F_GETLK64 is F_GETLK; x86's long long is 4-aligned.)
 */
enum { F_GETLK64_ = sizeof(long) == 8 ? 5 : 12 };

int t_flock(const char *dir)
{
    struct { short type, whence; long start, len; int pid; } lk;
#if defined(__i386__)
    struct { short type, whence; long long start, len; int pid; } lk64;
#else
    struct { short type, whence; int pad; long long start, len; int pid; } lk64;
#endif
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
    if (fcntl(fd, F_GETLK64_, &lk64) != 0 || lk64.type != 2) return -3;   /* F_GETLK64 */
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
 * each, an absolute time on CLOCK_MONOTONIC, and disarmed by a zero time. 8, or the negated step that failed. (A struct
 * epoll_event has its data at 8 on ARM and at 4 on the x86s, where the kernel's is packed.)
 */
typedef struct { long interval_sec, interval_nsec, value_sec, value_nsec; } t_itimerspec;
typedef struct { int fd; short events, revents; } t_pollfd;
#if defined(__x86_64__) || defined(__i386__)
typedef struct __attribute__((packed)) { unsigned events; unsigned long long data; } t_epoll_event;
#else
typedef struct { unsigned events; unsigned long long data; } t_epoll_event;
#endif
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
    t_epoll_event ev[2];
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

/* Chromium calls timerfd through syscall() rather than libc's timerfd functions. */
int t_timerfd_syscall(void)
{
    int fd = (int)syscall(NR_timerfd_create, 1, 0x800 | 0x80000);  /* CLOCK_MONOTONIC, NONBLOCK, CLOEXEC */
    t_itimerspec it, cur;
    unsigned long long n = 0;
    t_pollfd p;
    if (fd < 0) return -1;
    memset(&it, 0, sizeof(it));
    it.value_nsec = 20 * 1000000;
    if (syscall(NR_timerfd_settime, fd, 0, &it, 0) != 0) { close(fd); return -2; }
    p.fd = fd; p.events = 1; p.revents = 0;
    if (poll(&p, 1, 2000) != 1 || !(p.revents & 1) || read(fd, &n, 8) != 8 || n != 1) { close(fd); return -3; }
    if (syscall(NR_timerfd_gettime, fd, &cur) != 0 || cur.value_sec || cur.value_nsec) { close(fd); return -4; }
    close(fd);
    return 4;
}

/* A deep enough frame to need the stack protector's cookie, when built with it. */
int t_stack(const char *s)
{
    char buf[64];
    strcpy(buf, s);
    return (int)strlen(buf);
}


/* libc inline hooks must modify guest entries, never the host's text. Copy the
 * overwritten instructions and jump back, as Android hook installers do.
 * (32-bit ARM's: the hookable entries are made for Thumb's hooks, arm/mg_stubs.) */
int mprotect(void *, size_t, int);
#if defined(__arm__)
void *sysv_signal(int, void *);
static void *(*hook_original)(int, void *);
static int hook_calls;
static void *hook_signal(int sig, void *handler)
{
    hook_calls++;
    return hook_original(sig, handler);
}
static void hook_abort(void) { hook_calls++; }
static void hook_handler(int sig) { (void)sig; }
static void hook_jump(unsigned char *p, void *to)
{
    unsigned short op[2] = { 0xf8df, 0xf000 }; /* ldr.w pc, [pc, #0] */
    unsigned addr = (unsigned)(size_t)to;
    memcpy(p, op, 4);
    memcpy(p + 4, &addr, 4);
}
int t_libc_inline_hook(void)
{
    void *libc = dlopen("libc.so", 0);
    void *entry = dlsym(libc, "sysv_signal"), *ab = dlsym(libc, "abort");
    unsigned char *code = (void *)((size_t)entry & ~(size_t)1);
    unsigned char *abort_code = (void *)((size_t)ab & ~(size_t)1);
    unsigned char saved[8], abort_saved[8], *tramp;
    void *page = (void *)((size_t)code & ~(size_t)4095);
    void *abort_page = (void *)((size_t)abort_code & ~(size_t)4095);
    int (*flush)(long, long, long) = dlsym(libc, "cacheflush");
    void *old;
    int result = 0;
    if (!entry || !ab || !flush || !(1 & (size_t)entry) || page == abort_page) return -1;
    if (entry != (void *)sysv_signal || dlsym(libc, "sysv_signal") != entry) return -2;
    tramp = mmap(0, 4096, 3, 0x22, -1, 0);
    if (tramp == (void *)-1) return -3;
    memcpy(saved, code, 8);
    memcpy(abort_saved, abort_code, 8);
    memcpy(tramp, saved, 8);
    hook_jump(tramp + 8, (void *)((size_t)(code + 8) | 1));
    if (mprotect(tramp, 4096, 5)) { munmap(tramp, 4096); return -4; }
    hook_original = (void *)((size_t)tramp | 1);
    old = hook_original(10, 0);
    if (mprotect(page, 4096, 7)) { munmap(tramp, 4096); return -5; }
    hook_calls = 0;
    hook_jump(code, hook_signal);
    flush((long)code, (long)(code + 8), 0);
    /* Execute while still requested RWX, then request write access again as
     * hook installers do before restoring the prologue. */
    if (sysv_signal(10, hook_handler) != 0 || hook_calls != 1) result = -6;
    if (mprotect(page, 4096, 7)) return -14;
    memcpy(code, saved, 8);
    flush((long)code, (long)(code + 8), 0);
    if (mprotect(page, 4096, 5)) result = -7;
    if (sysv_signal(10, old) != (void *)hook_handler || hook_calls != 1) result = -8;
    munmap(tramp, 4096);
    if (mprotect(abort_page, 4096, 7)) return -9;
    hook_jump(abort_code, hook_abort);
    flush((long)abort_code, (long)(abort_code + 8), 0);
    if (mprotect(abort_page, 4096, 5)) return -10;
    ((void (*)(void))ab)();
    if (hook_calls != 2) result = -11;
    if (mprotect(abort_page, 4096, 7)) return -12;
    memcpy(abort_code, abort_saved, 8);
    flush((long)abort_code, (long)(abort_code + 8), 0);
    if (mprotect(abort_page, 4096, 5)) return -13;
    return result;
}
#endif

/* An inline hook on a function of the app's own library, as Instagram's
 * libdistract puts one on its libc++_shared.so: the page, asked to be writable
 * and executable at once, is written to and run, asked again, written to and
 * run. The function has a page of its own, as the hooked code is in another
 * library than the hook's: code that writes to its own page needs a write fault
 * told from an execute fault, which qemu does not tell. The jump is each
 * processor's own: ldr pc on ARM, `jmp *0(%rip)` and its address on x86_64,
 * jmp rel32 on x86, ldr x16 and br x16 on ARM64 (the function long enough to
 * take it). */
int own_hook(int a) { return a + 100; }
__attribute__((noinline, aligned(4096))) int own_hooked(int a)
{
#if defined(__x86_64__) || defined(__aarch64__)
    __asm__ volatile("nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop");
#else
    __asm__ volatile("nop\n nop\n nop\n nop\n nop\n nop");
#endif
    return a + 1;
}
#if !defined(__arm__)
/* What makes written code runnable: nothing on the x86s, whose caches keep up with a write; ARM64's cache maintenance. */
static void flush_code(unsigned char *start, unsigned char *end)
{
#if defined(__aarch64__)
    unsigned char *p;
    for (p = (unsigned char *)((size_t)start & ~(size_t)63); p < end; p += 64) __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish" : : : "memory");
    for (p = (unsigned char *)((size_t)start & ~(size_t)63); p < end; p += 64) __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
    __asm__ volatile("dsb ish\n isb" : : : "memory");
#else
    (void)start;
    (void)end;
#endif
}

__attribute__((aligned(4096))) int t_own_code_hook(void)
{
    int (*volatile fn)(int) = own_hooked;
    unsigned char *code = (unsigned char *)(size_t)fn, saved[16], jump[16];
    void *page = (void *)((size_t)code & ~(size_t)4095);
    size_t to = (size_t)own_hook, n;
    int result = 0;
    if (((size_t)code & 4095) || ((size_t)t_own_code_hook & ~(size_t)4095) == (size_t)page) return -1;
#if defined(__x86_64__)
    jump[0] = 0xff;                                   /* jmp *0(%rip), the address after it */
    jump[1] = 0x25;
    memset(jump + 2, 0, 4);
    memcpy(jump + 6, &to, 8);
    n = 14;
#elif defined(__i386__)
    {
        int rel = (int)(to - ((size_t)code + 5));    /* jmp rel32 */
        jump[0] = 0xe9;
        memcpy(jump + 1, &rel, 4);
        n = 5;
    }
#else
    {
        static const unsigned op[2] = { 0x58000050u, 0xd61f0200u };   /* ldr x16, #8; br x16; the address after them */
        memcpy(jump, op, 8);
        memcpy(jump + 8, &to, 8);
        n = 16;
    }
#endif
    memcpy(saved, code, n);
    if (mprotect(page, 4096, 7)) return -2;
    memcpy(code, jump, n);
    flush_code(code, code + n);
    if (fn(1) != 101) result = -3;
    if (mprotect(page, 4096, 7)) return -4;
    memcpy(code, saved, n);
    flush_code(code, code + n);
    if (fn(1) != 2) result = -5;
    if (mprotect(page, 4096, 5)) return -6;
    if (fn(2) != 3) result = -7;
    return result;
}
#else
__attribute__((aligned(4096))) int t_own_code_hook(void)
{
    int (*volatile fn)(int) = own_hooked;
    size_t at = (size_t)fn;
    unsigned char *code = (unsigned char *)(at & ~(size_t)1), saved[8], jump[8];
    void *page = (void *)((size_t)code & ~(size_t)4095);
    void *libc = dlopen("libc.so", 0);
    int (*flush)(long, long, long) = dlsym(libc, "cacheflush");
    unsigned to = (unsigned)(size_t)own_hook;
    int result = 0;
    if (!flush || ((size_t)code & 4095) || ((size_t)t_own_code_hook & ~(size_t)4095) == (size_t)page) return -1;
    if (at & 1) {
        unsigned short op[2] = { 0xf8df, 0xf000 }; /* ldr.w pc, [pc, #0] */
        memcpy(jump, op, 4);
    } else {
        unsigned op = 0xe51ff004; /* ldr pc, [pc, #-4] */
        memcpy(jump, &op, 4);
    }
    memcpy(jump + 4, &to, 4);
    memcpy(saved, code, 8);
    if (mprotect(page, 4096, 7)) return -2;
    memcpy(code, jump, 8);
    flush((long)code, (long)(code + 8), 0);
    if (fn(1) != 101) result = -3;
    if (mprotect(page, 4096, 7)) return -4;
    memcpy(code, saved, 8);
    flush((long)code, (long)(code + 8), 0);
    if (fn(1) != 2) result = -5;
    if (mprotect(page, 4096, 5)) return -6;
    if (fn(2) != 3) result = -7;
    return result;
}

/* The way libdistract hooks a function: its first instruction made a trap
 * (Thumb's udf #0xfa, which the phone's system keeps for itself; an ARM udf in
 * ARM state), and a SIGILL handler that moves the ucontext's pc to the hook.
 * (32-bit ARM's: the layer delivers such a trap to the app's handler there.) */
static int trap_calls;
static void trap_handler(int sig, void *info, void *context)
{
    unsigned *uc = context, to = (unsigned)(size_t)own_hook;
    (void)sig;
    if (((unsigned *)info)[0] != 4 || uc[23] != ((unsigned)(size_t)own_hooked & ~1u)) return;
    uc[23] = to & ~1u;                              /* arm_pc */
    uc[24] = (uc[24] & ~0x20u) | ((to & 1) << 5);   /* arm_cpsr's T bit */
    trap_calls++;
}
int t_trap_hook(void)
{
    int (*volatile fn)(int) = own_hooked;
    size_t at = (size_t)fn;
    unsigned char *code = (unsigned char *)(at & ~(size_t)1), saved[4];
    void *page = (void *)((size_t)code & ~(size_t)4095);
    t_sigaction act = { trap_handler, 0, 4, 0 }, old; /* SA_SIGINFO */
    unsigned short thumb_trap = 0xdefa;
    unsigned arm_trap = 0xe7ff4cfb;
    int result = 0;
    if (sigaction(4, &act, &old)) return -1;
    memcpy(saved, code, 4);
    if (mprotect(page, 4096, 7)) return -2;
    if (at & 1) memcpy(code, &thumb_trap, 2);
    else memcpy(code, &arm_trap, 4);
    if (mprotect(page, 4096, 5)) return -3;
    trap_calls = 0;
    if (fn(1) != 101 || trap_calls != 1) result = -4;
    if (fn(2) != 102 || trap_calls != 2) result = -5;
    if (mprotect(page, 4096, 7)) return -6;
    memcpy(code, saved, 4);
    if (mprotect(page, 4096, 5)) return -7;
    if (fn(1) != 2 || trap_calls != 2) result = -8;
    sigaction(4, &old, 0);
    return result;
}
#endif

/*
 * A wait is woken by what it waits on, whatever else other threads wait for: two threads pass a byte back and forth over
 * two pipes `rounds` times while `idle` other threads wait in epoll_wait, each on an eventfd of its own, as an app's
 * Loopers do. The microseconds the passing took, or the negated step that failed.
 */
int eventfd(unsigned, int);
static int g_idle_fd[64], g_ping[2], g_pong[2];
static volatile int g_idle_in;

static void *idle_waiter(void *arg)
{
    int i = (int)(long)arg, ep = epoll_create1(0);
    t_epoll_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].events = 1;                                /* EPOLLIN */
    if (ep < 0 || epoll_ctl(ep, 1, g_idle_fd[i], ev) != 0) return (void *)-1L;
    __sync_fetch_and_add(&g_idle_in, 1);
    epoll_wait(ep, ev, 2, -1);
    close(ep);
    return 0;
}

static void *ponger(void *arg)
{
    int rounds = (int)(long)arg, i;
    char b;
    for (i = 0; i < rounds; i++)
        if (read(g_ping[0], &b, 1) != 1 || write(g_pong[1], &b, 1) != 1) return (void *)-1L;
    return 0;
}

long long t_wait_herd(int idle, int rounds)
{
    pthread_t t[64], p;
    int i;
    char b = 1;
    unsigned long long one = 1;
    struct t_timespec a, z;
    long until;
    if (idle > 64) idle = 64;
    if (pipe(g_ping) || pipe(g_pong)) return -1;
    g_idle_in = 0;
    for (i = 0; i < idle; i++) {
        g_idle_fd[i] = eventfd(0, 0);
        if (g_idle_fd[i] < 0 || pthread_create(&t[i], 0, idle_waiter, (void *)(long)i) != 0) return -2;
    }
    for (until = now_ms() + 10000; g_idle_in < idle && now_ms() < until;) usleep(1000);
    usleep(50000);                                   /* each is in its epoll_wait */
    if (pthread_create(&p, 0, ponger, (void *)(long)rounds) != 0) return -3;
    clock_gettime(1, &a);
    for (i = 0; i < rounds; i++)
        if (write(g_ping[1], &b, 1) != 1 || read(g_pong[0], &b, 1) != 1) return -4;
    clock_gettime(1, &z);
    pthread_join(p, 0);
    for (i = 0; i < idle; i++) {
        write(g_idle_fd[i], &one, 8);
        pthread_join(t[i], 0);
        close(g_idle_fd[i]);
    }
    close(g_ping[0]);
    close(g_ping[1]);
    close(g_pong[0]);
    close(g_pong[1]);
    return (long long)(z.tv_sec - a.tv_sec) * 1000000 + (z.tv_nsec - a.tv_nsec) / 1000;
}

/*
 * What Go's runtime calls by number, past libc: nanosleep, rt_sigprocmask (a 64-bit set; another size refused), madvise
 * and mincore on a page of its own, epoll_pwait, fcntl, and fallocate with its offset and length in register pairs (a
 * mode other than 0 refused). 0, or the step that failed.
 *
 * A 64-bit offset or length is two words on the 32-bit ABIs, after a pad where ARM's pair has to begin at an even
 * register; one word on the 64-bit ones.
 */
#if defined(__arm__)
#define ARG64(v) v, 0
#define PAIR_PAD 0,
#elif defined(__i386__)
#define ARG64(v) v, 0
#define PAIR_PAD
#else
#define ARG64(v) (long)(v)
#define PAIR_PAD
#endif

int t_go_calls(const char *dir)
{
    struct { long sec, nsec; } nap = { 0, 2000000 };
    unsigned set[2] = { 1u << 9, 0 }, old[2] = { 0, 0 }, st[ST_WORDS];
    unsigned char vec[1];
    char path[256];
    void *page;
    int ep, fd;
    if (syscall(NR_nanosleep, &nap, 0) != 0) return 1;
    if (syscall(NR_rt_sigprocmask, 0 /* SIG_BLOCK */, set, 0, 8) != 0 || syscall(NR_rt_sigprocmask, 0, 0, old, 8) != 0 || !(old[0] & (1u << 9))) return 2;
    if (syscall(NR_rt_sigprocmask, 1 /* SIG_UNBLOCK */, set, 0, 8) != 0 || syscall(NR_rt_sigprocmask, 0, 0, old, 4) != -1 || *__errno() != 22) return 3;
    page = mmap(0, 4096, 3, 0x22, -1, 0);
    if (page == (void *)-1) return 4;
    ((char *)page)[0] = 1;
    if (syscall(NR_madvise, page, 4096, 4 /* MADV_DONTNEED */) != 0 || syscall(NR_mincore, page, 4096, vec) != 0) return 5;
    munmap(page, 4096);
    ep = epoll_create1(0);
    if (ep < 0 || syscall(NR_epoll_pwait, ep, st, 4, 0, 0, 8) != 0 || syscall(NR_fcntl, ep, 1 /* F_GETFD */, 0) < 0) return 6;
    close(ep);
    snprintf(path, sizeof(path), "%s/fallocated.bin", dir);
    fd = open(path, 0102, 0600);                       /* O_RDWR | O_CREAT */
    if (fd < 0) return 7;
    if (syscall(NR_fallocate, fd, 0, ARG64(0), ARG64(8192)) != 0 || fstat(fd, st) != 0 || st[ST_SIZE] != 8192) return 8;
    if (syscall(NR_fallocate, fd, 1 /* FALLOC_FL_KEEP_SIZE */, ARG64(0), ARG64(4096)) != -1 || *__errno() != 95) return 9;
    close(fd);
    unlink(path);
    return 0;
}

/*
 * Go's net and os packages by number, past libc, as Psiphon's tunnel core in WhatsApp's libgojni.so makes the calls: a
 * TCP socket listening on loopback (non-blocking, as Go's are), one connecting to it, accepted, a few bytes each way, the
 * ends' names, its error and its end; a socket pair with a message each way; splice refused as for sockets (EINVAL); a
 * file locked, grown, written and read at offsets (each in a register pair after a pad) and synced; mapped read-only and
 * shared, showing what is written to it afterwards, as Bolt reads its database; a directory's entries, twice; the limit on
 * descriptors, the system's name and the ids. 0, or the step that failed. (Only 32-bit ARM has send and recv by number,
 * the others sendto and recvfrom; ARM64 has ppoll and no poll; O_DIRECTORY is x86's own number on the x86s.)
 */
#if defined(__arm__)
#define SYS_SEND(fd, buf, n) syscall(NR_send, fd, buf, n, 0)
#define SYS_RECV(fd, buf, n) syscall(NR_recv, fd, buf, n, 0)
#else
#define SYS_SEND(fd, buf, n) syscall(NR_sendto, fd, buf, n, 0, 0, 0)
#define SYS_RECV(fd, buf, n) syscall(NR_recvfrom, fd, buf, n, 0, 0, 0)
#endif
#if defined(__aarch64__)
static const struct t_timespec five_s = { 5, 0 };
#define SYS_POLL(pf, n) syscall(NR_ppoll, pf, n, &five_s, 0, 8)
#else
#define SYS_POLL(pf, n) syscall(NR_poll, pf, n, 5000)
#endif
#if defined(__x86_64__) || defined(__i386__)
#define O_DIRECTORY_ 0200000
#else
#define O_DIRECTORY_ 040000
#endif

int t_go_net_files(const char *dir)
{
    struct { unsigned short family, port; unsigned addr; unsigned char zero[8]; } sa, peer;
    struct { void *base; size_t len; } io;
    struct { void *name; unsigned namelen; void *iov; size_t iovlen; void *control; size_t controllen; int flags; } m;
    struct { unsigned long long cur, max; } lim;
    t_pollfd pf;
    unsigned len, one = 1, st[ST_WORDS];
    int ls, cs, as, sv[2], fd, fd2, dfd, n, off, found = 0, err = -1;
    char buf[64], path[256], ents[2048];
    unsigned char *map;
    ls = syscall(NR_socket, 2 /* AF_INET */, 1 /* SOCK_STREAM */ | 04000 /* SOCK_NONBLOCK */ | 02000000 /* SOCK_CLOEXEC */, 0);
    if (ls < 0) return 1;
    if (syscall(NR_setsockopt, ls, 1 /* SOL_SOCKET */, 2 /* SO_REUSEADDR */, &one, 4) != 0) return 2;
    memset(&sa, 0, sizeof(sa));
    sa.family = 2;
    sa.addr = 0x0100007F;                              /* 127.0.0.1 */
    if (syscall(NR_bind, ls, &sa, sizeof(sa)) != 0 || syscall(NR_listen, ls, 8) != 0) return 3;
    len = sizeof(sa);
    if (syscall(NR_getsockname, ls, &sa, &len) != 0 || !sa.port) return 4;
    cs = syscall(NR_socket, 2, 1, 0);
    if (cs < 0 || syscall(NR_connect, cs, &sa, sizeof(sa)) != 0) return 5;
    pf.fd = ls;
    pf.events = 1;                                     /* POLLIN */
    pf.revents = 0;
    if (SYS_POLL(&pf, 1) != 1) return 6;
    len = sizeof(peer);
    as = syscall(NR_accept4, ls, &peer, &len, 02000000);
    if (as < 0) return 7;
    if (syscall(NR_sendto, cs, "ping", 4, 0, 0, 0) != 4) return 8;
    pf.fd = as;
    if (SYS_POLL(&pf, 1) != 1 || syscall(NR_recvfrom, as, buf, sizeof(buf), 0, 0, 0) != 4 || memcmp(buf, "ping", 4)) return 9;
    if (SYS_SEND(as, "pong", 4) != 4 || SYS_RECV(cs, buf, sizeof(buf)) != 4 || memcmp(buf, "pong", 4)) return 10;
    len = sizeof(peer);
    if (syscall(NR_getpeername, cs, &peer, &len) != 0 || peer.port != sa.port) return 11;
    len = 4;
    if (syscall(NR_getsockopt, cs, 1, 4 /* SO_ERROR */, &err, &len) != 0 || err != 0) return 12;
    if (syscall(NR_shutdown, cs, 1 /* SHUT_WR */) != 0 || SYS_POLL(&pf, 1) != 1 || syscall(NR_recvfrom, as, buf, sizeof(buf), 0, 0, 0) != 0) return 13;
    close(as);
    close(cs);
    close(ls);
    if (syscall(NR_socketpair, 1 /* AF_UNIX */, 1, 0, sv) != 0) return 14;
    memset(&m, 0, sizeof(m));
    io.base = "hello";
    io.len = 5;
    m.iov = &io;
    m.iovlen = 1;
    if (syscall(NR_sendmsg, sv[0], &m, 0) != 5) return 15;
    io.base = buf;
    io.len = sizeof(buf);
    if (syscall(NR_recvmsg, sv[1], &m, 0) != 5 || memcmp(buf, "hello", 5)) return 16;
    close(sv[0]);
    close(sv[1]);
    if (syscall(NR_splice, 0, 0, 0, 0, 0, 0) != -1 || *__errno() != 22) return 17;
    snprintf(path, sizeof(path), "%s/bolt.db", dir);
    fd = open(path, 01102, 0600);                      /* O_RDWR | O_CREAT | O_TRUNC */
    if (fd < 0) return 18;
    if (syscall(NR_flock, fd, 2 | 4 /* LOCK_EX | LOCK_NB */) != 0) return 19;
    if (syscall(NR_ftruncate, fd, PAIR_PAD ARG64(3 * 4096)) != 0 || fstat(fd, st) != 0 || st[ST_SIZE] != 3 * 4096) return 20;
    if (syscall(NR_pwrite64, fd, "meta", 4, PAIR_PAD ARG64(4096)) != 4) return 21;
    if (syscall(NR_pread64, fd, buf, 4, PAIR_PAD ARG64(4096)) != 4 || memcmp(buf, "meta", 4)) return 22;
    if (syscall(NR_fdatasync, fd) != 0) return 23;
    map = mmap(0, 4 * 4096, 1 /* PROT_READ */, 1 /* MAP_SHARED */, fd, 0);
    if (map == (void *)-1) return 24;
    if (memcmp(map + 4096, "meta", 4)) return 25;
    if (syscall(NR_pwrite64, fd, "page", 4, PAIR_PAD ARG64(2 * 4096 + 8)) != 4 || memcmp(map + 2 * 4096 + 8, "page", 4)) return 26;
    fd2 = open(path, 02);
    if (fd2 < 0) return 27;
    if (syscall(NR_ftruncate, fd2, PAIR_PAD ARG64(4 * 4096)) != 0 || pwrite(fd2, "grow", 4, 3 * 4096) != 4) return 28;
    close(fd2);
    if (memcmp(map + 3 * 4096, "grow", 4)) return 29;
    if (lseek(fd, 100, 0) != 100 || write(fd, "seq", 3) != 3 || memcmp(map + 100, "seq", 3)) return 30;
    munmap(map, 4 * 4096);
    close(fd);
    dfd = open(dir, O_DIRECTORY_ | 02000000);
    if (dfd < 0) return 31;
    while ((n = syscall(NR_getdents64, dfd, ents, sizeof(ents))) > 0) {
        for (off = 0; off < n;) {
            unsigned short reclen;
            memcpy(&reclen, ents + off + 16, 2);
            if (!reclen || reclen % 8) return 32;
            if (!strcmp(ents + off + 19, "bolt.db") && ents[off + 18] == 8 /* DT_REG */) found++;
            off += reclen;
        }
    }
    if (n < 0 || found != 1) return 33;
    if (lseek(dfd, 0, 0) != 0 || syscall(NR_getdents64, dfd, ents, sizeof(ents)) <= 0) return 34;
    close(dfd);
    unlink(path);
    if (syscall(NR_prlimit64, 0, 7 /* RLIMIT_NOFILE */, 0, &lim) != 0 || !lim.cur || lim.cur > lim.max) return 35;
    if (syscall(NR_uname, ents) != 0 || strcmp(ents, "Linux")) return 36;
    if (syscall(NR_getuid) < 0 || syscall(NR_getppid) < 0) return 37;
    return 0;
}

/*
 * The vDSO in the auxiliary vector (AT_SYSINFO_EHDR), read as Go's runtime reads it on Android: from /proc/self/auxv,
 * which has the same vector as the init functions' block, the page size and AT_RANDOM in it too; then the image as
 * runtime/vdso_linux.go reads one: the load offset from the first PT_LOAD, the dynamic table, the version LINUX_2.6
 * among the definitions, __vdso_clock_gettime of that version through the hash table's chain. Called, it says the
 * monotonic clock, between two readings of clock_gettime. 0, or the step that failed.
 *
 * Each processor's as its kernel's is: the vector in words of its own width, the image an ELF of its own class and
 * machine; ARM64's clock is __kernel_clock_gettime of LINUX_2.6.39 (vdso_linux_arm64.go), the others' as above.
 */
#if defined(__arm__)
enum { VDSO_MACHINE = 40, VDSO_VER_HASH = 0x3ae75f6, VDSO_FN_HASH = 0xd35ec75 };
#define VDSO_VER "LINUX_2.6"
#define VDSO_FN "__vdso_clock_gettime"
#elif defined(__aarch64__)
enum { VDSO_MACHINE = 183, VDSO_VER_HASH = 0x75fcb89, VDSO_FN_HASH = 0xb0cd725 };
#define VDSO_VER "LINUX_2.6.39"
#define VDSO_FN "__kernel_clock_gettime"
#elif defined(__x86_64__)
enum { VDSO_MACHINE = 62, VDSO_VER_HASH = 0x3ae75f6, VDSO_FN_HASH = 0xd35ec75 };
#define VDSO_VER "LINUX_2.6"
#define VDSO_FN "__vdso_clock_gettime"
#else
enum { VDSO_MACHINE = 3, VDSO_VER_HASH = 0x3ae75f6, VDSO_FN_HASH = 0xd35ec75 };
#define VDSO_VER "LINUX_2.6"
#define VDSO_FN "__vdso_clock_gettime"
#endif

int t_vdso(void)
{
    t_word file[128], ehdr = 0, pagesz = 0, random = 0;
    int fd = open("/proc/self/auxv", 0), got, k;
    if (fd < 0) return 10;
    got = read(fd, file, sizeof(file));
    close(fd);
    if (got <= 0 || got % (int)(2 * sizeof(t_word))) return 11;
    for (k = 0; k + 1 < got / (int)sizeof(t_word) && file[k]; k += 2) {
        if (file[k] == 33) ehdr = file[k + 1];
        if (file[k] == 6) pagesz = file[k + 1];
        if (file[k] == 25) random = file[k + 1];
    }
    if (file[k] != 0 || !ehdr || ehdr != g_init_vdso || pagesz != 4096 || !random) return 12;
#if defined(__LP64__)
    typedef struct { unsigned char ident[16]; unsigned short type, machine; unsigned version; unsigned long entry, phoff, shoff; unsigned flags;
                     unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } t_ehdr;
    typedef struct { unsigned type, flags; unsigned long offset, vaddr, paddr, filesz, memsz, align; } t_phdr;
    typedef struct { long tag; unsigned long val; } t_dyn;
    typedef struct { unsigned name; unsigned char info, other; unsigned short shndx; unsigned long value, size; } t_sym;
#else
    typedef struct { unsigned char ident[16]; unsigned short type, machine; unsigned version, entry, phoff, shoff, flags;
                     unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } t_ehdr;
    typedef struct { unsigned type, offset, vaddr, paddr, filesz, memsz, flags, align; } t_phdr;
    typedef struct { int tag; unsigned val; } t_dyn;
    typedef struct { unsigned name, value, size; unsigned char info, other; unsigned short shndx; } t_sym;
#endif
    typedef struct { unsigned short version, flags, ndx, cnt; unsigned hash, aux, next; } t_verdef;
    typedef int (*gettime_fn)(int, struct t_timespec *);
    const t_ehdr *h = (const t_ehdr *)(size_t)g_init_vdso;
    const t_phdr *ph;
    const t_dyn *d = 0;
    const unsigned *hash = 0;
    const t_sym *syms = 0;
    const char *strs = 0;
    const unsigned short *versym = 0;
    const t_verdef *vd = 0;
    t_word base, load = 0;
    unsigned i, ver = 0, chain;
    int loaded = 0;
    struct t_timespec a, b, c;
    if (!h) return 1;
    if (h->ident[0] != 0x7f || h->ident[1] != 'E' || h->ident[2] != 'L' || h->ident[3] != 'F' || h->ident[4] != (int)(sizeof(void *) / 4) ||
        h->machine != VDSO_MACHINE)
        return 2;
    base = (t_word)(size_t)h;
    ph = (const t_phdr *)(size_t)(base + h->phoff);
    for (i = 0; i < h->phnum; i++) {
        if (ph[i].type == 1 && !loaded) {
            loaded = 1;
            load = base + ph[i].offset - ph[i].vaddr;
        }
        if (ph[i].type == 2) d = (const t_dyn *)(size_t)(base + ph[i].offset);
    }
    if (!loaded || !d) return 3;
    for (; d->tag; d++) {
        t_word p = load + d->val;
        if (d->tag == 4) hash = (const unsigned *)(size_t)p;
        if (d->tag == 5) strs = (const char *)(size_t)p;
        if (d->tag == 6) syms = (const t_sym *)(size_t)p;
        if (d->tag == 0x6ffffff0) versym = (const unsigned short *)(size_t)p;
        if (d->tag == 0x6ffffffc) vd = (const t_verdef *)(size_t)p;
    }
    if (!hash || !strs || !syms || !versym || !vd) return 4;
    for (;;) {
        if (!(vd->flags & 1)) {
            const unsigned *aux = (const unsigned *)((const char *)vd + vd->aux);
            if (vd->hash == VDSO_VER_HASH && !strcmp(strs + aux[0], VDSO_VER)) {
                ver = vd->ndx & 0x7fff;
                break;
            }
        }
        if (!vd->next) break;
        vd = (const t_verdef *)((const char *)vd + vd->next);
    }
    if (!ver) return 5;
    for (chain = hash[2 + VDSO_FN_HASH % hash[0]]; chain; chain = hash[2 + hash[0] + chain]) {
        const t_sym *s = &syms[chain];
        gettime_fn fn;
        if ((s->info & 15) != 2 || (s->info >> 4) != 1 || !s->shndx || strcmp(strs + s->name, VDSO_FN)) continue;
        if ((versym[chain] & 0x7fff) != ver) continue;
        fn = (gettime_fn)(size_t)(load + s->value);
        if (clock_gettime(1, &a) != 0 || fn(1, &b) != 0 || clock_gettime(1, &c) != 0) return 7;
        if ((long long)b.tv_sec * 1000000000 + b.tv_nsec < (long long)a.tv_sec * 1000000000 + a.tv_nsec) return 8;
        if ((long long)c.tv_sec * 1000000000 + c.tv_nsec < (long long)b.tv_sec * 1000000000 + b.tv_nsec) return 9;
        return 0;
    }
    return 6;
}
