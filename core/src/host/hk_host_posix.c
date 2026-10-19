/*
 * The host layer over POSIX: Linux, where the ARM32 build of the core runs
 * under qemu-arm so that the tests can execute guest code.
 */
#if !defined(_WIN32)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "hk_host.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

/* ---------------------------------------------------------------- memory */

static int prot_of(int prot)
{
    switch (prot) {
    case HK_PROT_R: return PROT_READ;
    case HK_PROT_RW: return PROT_READ | PROT_WRITE;
    case HK_PROT_RX: return PROT_READ | PROT_EXEC;
    default: return PROT_NONE;
    }
}

void *hk_mem_reserve(size_t size)
{
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* Sections: a memfd of that size, viewed with mmap(MAP_SHARED). */
struct hk_section {
    int fd;
};

static volatile i32 g_views_kb;

static void views_count(size_t size, int sign) { hk_atomic_add(&g_views_kb, sign * (i32)((size + 1023) >> 10)); }

u32 hk_views_kb(void) { return (u32)g_views_kb; }

hk_section *hk_section_create(u64 size)
{
    hk_section *s;
    int fd;
    if (!size) return NULL;
    fd = (int)syscall(SYS_memfd_create, "megler-section", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NULL;
    }
    s = (hk_section *)calloc(1, sizeof(*s));
    if (!s) {
        close(fd);
        return NULL;
    }
    s->fd = fd;
    return s;
}

void *hk_section_map(hk_section *s, u64 offset, size_t size, bool writable, void **base)
{
    void *v;
    if (!s || !size) return NULL;
    v = mmap(NULL, size, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, s->fd, (off_t)offset);
    if (v == MAP_FAILED) return NULL;
    views_count(size, 1);
    if (base) *base = v;
    return v;
}

void hk_section_unmap(void *base, size_t size)
{
    if (!base || !size) return;
    munmap(base, size);
    views_count(size, -1);
}

void hk_section_close(hk_section *s)
{
    if (!s) return;
    close(s->fd);
    free(s);
}

void *hk_mem_reserve_at(void *p, size_t size)
{
    /* A hint, which the kernel follows when the range is free; anything else is given back. */
    void *got = p ? mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) : MAP_FAILED;
    if (got == MAP_FAILED) return NULL;
    if (got != p) {
        munmap(got, size);
        return NULL;
    }
    return got;
}

bool hk_mem_commit(void *p, size_t size, int prot)
{
    return mprotect(p, size, prot_of(prot)) == 0;
}

bool hk_mem_protect(void *p, size_t size, int prot)
{
    return mprotect(p, size, prot_of(prot)) == 0;
}

bool hk_mem_decommit(void *p, size_t size)
{
    void *q = mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return q != MAP_FAILED;
}

bool hk_mem_protect_committed(void *p, size_t size, int prot) { return hk_mem_protect(p, size, prot); }
i64 hk_mem_committed(const void *p, size_t size)
{
    (void)p;
    (void)size;
    return -1;
}

void hk_mem_release(void *p, size_t size) { munmap(p, size); }

void hk_icache_flush(void *p, size_t size)
{
#if defined(__arm__)
    __builtin___clear_cache((char *)p, (char *)p + size);
#else
    (void)p; (void)size;
#endif
}

void *hk_mem_alloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static u64 (*g_limit)(void), (*g_usage)(void);
void hk_mem_set_probe(u64 (*limit)(void), u64 (*usage)(void)) { g_limit = limit; g_usage = usage; }
u64 hk_mem_limit(void) { return g_limit ? g_limit() : 0; }
u64 hk_mem_usage(void) { return g_usage ? g_usage() : 0; }

/* --------------------------------------------------------------- threads */

struct hk_thread {
    pthread_t t;
    hk_thread_fn fn;
    void *arg;
    /* Stopped for the collector (hk_thread_suspend): its registers as the signal found them, and whether it may go on. */
    hk_thread_regs regs;
    volatile i32 go_on;
};

static HK_THREAD_LOCAL hk_thread *t_current;

static void (*volatile g_end_hook)(void);
void hk_thread_set_end_hook(void (*fn)(void)) { g_end_hook = fn; }

static void *thread_main(void *p)
{
    hk_thread *t = (hk_thread *)p;
    t_current = t;
    t->fn(t->arg);
    if (g_end_hook) g_end_hook();
    return NULL;
}

void hk_thread_runs_app_code(void) { }

u64 hk_thread_cpu_ns(hk_thread *t)
{
    clockid_t c = CLOCK_THREAD_CPUTIME_ID;
    struct timespec ts;
    if (t && pthread_getcpuclockid(t->t, &c) != 0) return 0;
    if (clock_gettime(c, &ts) != 0) return 0;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 hk_process_cpu_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

hk_thread *hk_thread_current(void)
{
    if (!t_current) {
        t_current = (hk_thread *)hk_alloc(sizeof(*t_current));
        t_current->t = pthread_self();
    }
    return t_current;
}

/* A signal of the host's own carries the interruption; its handler looks at where the thread was. */
#define INTERRUPT_SIGNAL (SIGRTMIN + 6)
static void (*g_interrupt_fn)(void);
static bool (*g_interrupt_in_guest)(hk_addr pc);

static volatile i32 g_interrupts_asked, g_interrupts_done;

void hk_thread_interrupt_report(char *out, size_t size)
{
    snprintf(out, size, "%d asked for, %d found the thread in guest code", (int)g_interrupts_asked, (int)g_interrupts_done);
}

static void on_interrupt(int sig, siginfo_t *si, void *ctx)
{
    int saved = errno;
    (void)sig; (void)si;
#if defined(__arm__)
    if (g_interrupt_fn && g_interrupt_in_guest && g_interrupt_in_guest((hk_addr)(((ucontext_t *)ctx)->uc_mcontext.arm_pc & ~1u))) {
        hk_atomic_add(&g_interrupts_done, 1);
        g_interrupt_fn();
    }
#else
    (void)ctx;
#endif
    errno = saved;
}

void hk_thread_set_interrupt(void (*fn)(void), bool (*in_guest_code)(hk_addr pc))
{
    struct sigaction sa;
    g_interrupt_fn = fn;
    g_interrupt_in_guest = in_guest_code;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_interrupt;
    /* NODEFER: `fn` may wait (a handler that parks its thread), and another interruption has to get through meanwhile. */
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(INTERRUPT_SIGNAL, &sa, NULL);
}

/*
 * Stopping a thread for the collector: a signal whose handler saves the registers the thread was stopped with, says so, and
 * waits for the other signal (sigsuspend, which a handler may call) until it is told to go on, and says it goes on. One
 * thread is stopped or resumed at a time, so one semaphore says "stopped" and one "going on": hk_thread_resume returns only
 * once the handler has left its wait, so that the next hk_thread_suspend cannot find it still there.
 */
#define SUSPEND_SIGNAL (SIGRTMIN + 7)
#define RESUME_SIGNAL (SIGRTMIN + 8)
static sem_t g_stopped, g_going_on;
static pthread_once_t g_suspend_once = PTHREAD_ONCE_INIT;

static void on_resume(int sig) { (void)sig; }

static void on_suspend(int sig, siginfo_t *si, void *ctx)
{
    int saved = errno;
    hk_thread *t = t_current;
    sigset_t wait_for;
    (void)sig; (void)si;
    if (!t) {
        errno = saved;
        return;
    }
#if defined(__arm__)
    {
        const mcontext_t *m = &((ucontext_t *)ctx)->uc_mcontext;
        t->regs.sp = m->arm_sp;
        t->regs.pc = m->arm_pc;
        t->regs.flags = (u32)m->arm_cpsr;
        t->regs.r[0] = m->arm_r0; t->regs.r[1] = m->arm_r1; t->regs.r[2] = m->arm_r2; t->regs.r[3] = m->arm_r3; t->regs.r[4] = m->arm_r4;
        t->regs.r[5] = m->arm_r5; t->regs.r[6] = m->arm_r6; t->regs.r[7] = m->arm_r7; t->regs.r[8] = m->arm_r8; t->regs.r[9] = m->arm_r9;
        t->regs.r[10] = m->arm_r10; t->regs.r[11] = m->arm_fp; t->regs.r[12] = m->arm_ip; t->regs.r[13] = m->arm_lr;
        t->regs.n = 14;
    }
#elif defined(__x86_64__)
    {
        const greg_t *g = ((ucontext_t *)ctx)->uc_mcontext.gregs;
        static const int which[] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RBP, REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13,
                                     REG_R14, REG_R15 };
        int i;
        t->regs.sp = (hk_addr)g[REG_RSP];
        t->regs.pc = (hk_addr)g[REG_RIP];
        t->regs.flags = (u32)g[REG_EFL];
        for (i = 0; i < (int)HK_COUNT(which); i++) t->regs.r[i] = (hk_addr)g[which[i]];
        t->regs.n = (int)HK_COUNT(which);
    }
#else
    (void)ctx;
    t->regs.n = 0;
    t->regs.sp = (hk_addr)&wait_for;
#endif
    sigfillset(&wait_for);
    sigdelset(&wait_for, RESUME_SIGNAL);
    sem_post(&g_stopped);
    while (!t->go_on) sigsuspend(&wait_for);
    sem_post(&g_going_on);
    errno = saved;
}

static void suspend_setup(void)
{
    struct sigaction sa;
    sem_init(&g_stopped, 0, 0);
    sem_init(&g_going_on, 0, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_suspend;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(SUSPEND_SIGNAL, &sa, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_resume;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(RESUME_SIGNAL, &sa, NULL);
}

bool hk_thread_suspend(hk_thread *t, hk_thread_regs *out)
{
    memset(out, 0, sizeof(*out));
    if (!t || pthread_equal(t->t, pthread_self())) return false;
    pthread_once(&g_suspend_once, suspend_setup);
    t->go_on = 0;
    if (pthread_kill(t->t, SUSPEND_SIGNAL) != 0) return false;
    while (sem_wait(&g_stopped) != 0 && errno == EINTR) { }
    *out = t->regs;
    return true;
}

void hk_thread_resume(hk_thread *t)
{
    if (!t) return;
    hk_atomic_xchg(&t->go_on, 1);
    if (pthread_kill(t->t, RESUME_SIGNAL) != 0) return;
    while (sem_wait(&g_going_on) != 0 && errno == EINTR) { }
}

/* (Whether it found the thread in guest code is not known here: true means only that it was asked.) */
bool hk_thread_interrupt(hk_thread *t)
{
    if (!t || !g_interrupt_fn) return false;
    hk_atomic_add(&g_interrupts_asked, 1);
    return pthread_kill(t->t, INTERRUPT_SIGNAL) == 0;
}

hk_thread *hk_thread_start(hk_thread_fn fn, void *arg, size_t stack_size, const char *name)
{
    hk_thread *t = (hk_thread *)hk_alloc(sizeof(*t));
    pthread_attr_t a;
    (void)name;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_init(&a);
    if (stack_size) pthread_attr_setstacksize(&a, stack_size < 65536 ? 65536 : stack_size);
    if (pthread_create(&t->t, &a, thread_main, t) != 0) {
        pthread_attr_destroy(&a);
        free(t);
        return NULL;
    }
    pthread_attr_destroy(&a);
    return t;
}

void hk_thread_join(hk_thread *t)
{
    pthread_join(t->t, NULL);
    free(t);
}

void hk_thread_detach(hk_thread *t)
{
    /* The thread's main still reads t; it is small and lives as long as the process. */
    pthread_detach(t->t);
}

void hk_thread_exit(void)
{
    if (g_end_hook) g_end_hook();
    pthread_exit(NULL);
}
u32 hk_thread_self_id(void) { return (u32)syscall(SYS_gettid); }
u32 hk_thread_id(const hk_thread *t) { (void)t; return 0; }
void hk_thread_yield(void) { sched_yield(); }

void hk_sleep_ns(u64 ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

int hk_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void hk_lock_init(hk_lock *l) { pthread_mutex_init(&l->m, NULL); }
void hk_lock_acquire(hk_lock *l) { pthread_mutex_lock(&l->m); }
bool hk_lock_try(hk_lock *l) { return pthread_mutex_trylock(&l->m) == 0; }
void hk_lock_release(hk_lock *l) { pthread_mutex_unlock(&l->m); }

void hk_cond_init(hk_cond *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &a);
    pthread_condattr_destroy(&a);
    c->monotonic = true;
}

void hk_cond_wait(hk_cond *c, hk_lock *l) { pthread_cond_wait(&c->c, &l->m); }

bool hk_cond_wait_ns(hk_cond *c, hk_lock *l, u64 ns)
{
    /* A statically initialised condition waits on the realtime clock; one made by hk_cond_init on the monotonic one. */
    struct timespec ts;
    clock_gettime(c->monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ns / 1000000000ull);
    ts.tv_nsec += (long)(ns % 1000000000ull);
    if (ts.tv_nsec >= 1000000000l) { ts.tv_nsec -= 1000000000l; ts.tv_sec++; }
    return pthread_cond_timedwait(&c->c, &l->m, &ts) == 0;
}

void hk_cond_signal(hk_cond *c) { pthread_cond_signal(&c->c); }
void hk_cond_broadcast(hk_cond *c) { pthread_cond_broadcast(&c->c); }

i32 hk_atomic_cas(volatile i32 *p, i32 expect, i32 value) { return __sync_val_compare_and_swap(p, expect, value); }
i32 hk_atomic_add(volatile i32 *p, i32 delta) { return __sync_add_and_fetch(p, delta); }
i32 hk_atomic_xchg(volatile i32 *p, i32 value) { return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST); }
void *hk_atomic_cas_ptr(void *volatile *p, void *expect, void *value) { return __sync_val_compare_and_swap(p, expect, value); }
i64 hk_atomic_cas64(volatile i64 *p, i64 expect, i64 value) { return __sync_val_compare_and_swap(p, expect, value); }

bool hk_futex_wait(volatile i32 *addr, i32 expect, u64 timeout_ns)
{
    struct timespec ts;
    long r;
    ts.tv_sec = (time_t)(timeout_ns / 1000000000ull);
    ts.tv_nsec = (long)(timeout_ns % 1000000000ull);
    r = syscall(SYS_futex, addr, 0 /* FUTEX_WAIT */, expect, timeout_ns ? &ts : NULL, NULL, 0);
    return !(r < 0 && errno == ETIMEDOUT);
}

void hk_futex_wake(volatile i32 *addr, bool all)
{
    syscall(SYS_futex, addr, 1 /* FUTEX_WAKE */, all ? 0x7FFFFFFF : 1, NULL, NULL, 0);
}

/* A thread's stack is all there from the start here (and the first thread's grows wherever it is touched). */
void hk_thread_whole_stack(void) { }

void hk_thread_stack_bounds(void **lo, void **hi)
{
    pthread_attr_t a;
    void *base = NULL;
    size_t size = 0;
    *lo = *hi = NULL;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return;
    if (pthread_attr_getstack(&a, &base, &size) == 0) {
        *lo = base;
        *hi = (u8 *)base + size;
    }
    pthread_attr_destroy(&a);
}

u32 hk_tls_alloc(void)
{
    pthread_key_t k;
    if (pthread_key_create(&k, NULL) != 0) hk_fatal("out of thread-local slots");
    return (u32)k;
}
void *hk_tls_get(u32 key) { return pthread_getspecific((pthread_key_t)key); }
void hk_tls_set(u32 key, void *value) { pthread_setspecific((pthread_key_t)key, value); }

/* ------------------------------------------------- the guest thread pointer */

/* initial-exec: a fixed offset from the thread pointer in every thread, which is what the emitted load relies on. */
static __thread void *g_guest_tp __attribute__((tls_model("initial-exec")));

void hk_guest_tp_set(void *tp) { g_guest_tp = tp; }
void *hk_guest_tp_get(void) { return g_guest_tp; }

bool hk_guest_tp_where(u32 *opc2, u32 *offset)
{
#if defined(__arm__)
    /* glibc's thread pointer is TPIDRURO (opc2 3), the register guest code reads too. */
    intptr_t off = (intptr_t)((char *)&g_guest_tp - (char *)__builtin_thread_pointer());
    if (off < 0 || off > 4095) return false;
    *opc2 = 3;
    *offset = (u32)off;
    return true;
#else
    (void)opc2; (void)offset;
    return false;
#endif
}

/* ----------------------------------------------------------------- faults */

static hk_fault_handler g_fault_handler;

static void on_signal(int sig, siginfo_t *si, void *ctx)
{
    hk_fault f;
    memset(&f, 0, sizeof(f));
    f.kind = sig == SIGILL ? HK_FAULT_ILLEGAL : (sig == SIGSEGV || sig == SIGBUS) ? HK_FAULT_ACCESS : HK_FAULT_OTHER;
    /* A trap (bkpt: a compiler's __builtin_trap, Chromium's CHECK) is given the number Windows gives a breakpoint, which the
       core's report names; the other signals keep their own. */
    f.code = sig == SIGTRAP ? 0x80000003u : (u32)sig;
    f.address = (hk_addr)si->si_addr;
#if defined(__arm__)
    {
        ucontext_t *uc = (ucontext_t *)ctx;
        f.regs = (u32 *)&uc->uc_mcontext.arm_r0;
        f.cpsr = (u32 *)&uc->uc_mcontext.arm_cpsr;
        /* error_code is the fault status register: bit 11 says the access was a write. */
        f.write = (uc->uc_mcontext.error_code & (1u << 11)) != 0;
        f.exec = sig == SIGSEGV && f.address == (uc->uc_mcontext.arm_pc & ~1u);
    }
#else
    (void)ctx;
#endif
    if (g_fault_handler && g_fault_handler(&f)) return;
    /* Not ours: die the way the signal would have. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void hk_fault_resume_at(hk_fault *f, hk_addr target)
{
    if (!f->regs) return;
    f->regs[15] = (u32)(target & ~(hk_addr)1);
    if (f->cpsr) *f->cpsr = (target & 1) ? (*f->cpsr | 0x20u) : (*f->cpsr & ~0x20u);
}

void hk_fault_set_handler(hk_fault_handler h)
{
    struct sigaction sa;
    g_fault_handler = h;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    /* And a trap, which Android code ends with on purpose: reported where it was, as on the phone, before the process goes. */
    sigaction(SIGTRAP, &sa, NULL);
}

/* ------------------------------------------------------------------- time */

u64 hk_time_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 hk_time_ticks(void) { return hk_time_monotonic_ns(); }
u64 hk_ticks_ns(u64 ticks) { return ticks; }

u64 hk_time_realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

i32 hk_time_utc_offset(i64 unix_seconds, bool *dst)
{
    time_t t = (time_t)unix_seconds;
    struct tm tm;
    localtime_r(&t, &tm);
    if (dst) *dst = tm.tm_isdst > 0;
    return (i32)tm.tm_gmtoff;
}

/* ------------------------------------------------------------------ files */

struct hk_file { int fd; };

void *hk_file_map(hk_file *f, u64 offset, size_t size, void **base, size_t *mapped)
{
    u64 aligned = offset & ~(u64)(sysconf(_SC_PAGESIZE) - 1);
    size_t delta = (size_t)(offset - aligned);
    void *v;
    if (!f || !size) return NULL;
    v = mmap(NULL, size + delta, PROT_READ, MAP_SHARED, f->fd, (off_t)aligned);
    if (v == MAP_FAILED) return NULL;
    views_count(size + delta, 1);
    *base = v;
    *mapped = size + delta;
    return (u8 *)v + delta;
}

static int err_of(int e)
{
    switch (e) {
    case 0: return HK_E_OK;
    case ENOENT: return HK_E_NOENT;
    case EACCES: case EPERM: case EROFS: return HK_E_ACCESS;
    case EEXIST: return HK_E_EXIST;
    case ENOTDIR: return HK_E_NOTDIR;
    case EISDIR: return HK_E_ISDIR;
    case ENOSPC: return HK_E_NOSPC;
    case EINVAL: return HK_E_INVAL;
    case ENOTEMPTY: return HK_E_NOTEMPTY;
    case EBUSY: return HK_E_BUSY;
    case EXDEV: return HK_E_XDEV;
    default: return HK_E_IO;
    }
}
#define SET_ERR() do { if (err) *err = err_of(errno); } while (0)

/* A foreign path's descriptor (hk_host.h: its owner hands over the descriptor plus one); -1 with *err set. */
static int foreign_fd(const char *path, int flags, int kind, int *err)
{
    void *h = hk_foreign_open(path, flags, kind, err);
    return h ? (int)(intptr_t)h - 1 : -1;
}

hk_file *hk_file_open(const char *path, int flags, int *err)
{
    int o = 0, fd;
    hk_file *f;
    if (hk_path_is_foreign(path)) {
        fd = foreign_fd(path, flags, HK_FOREIGN_FILE, err);
        if (fd < 0) return NULL;
    } else {
        if ((flags & HK_O_READ) && (flags & HK_O_WRITE)) o = O_RDWR;
        else if (flags & HK_O_WRITE) o = O_WRONLY;
        else o = O_RDONLY;
        if (flags & HK_O_CREATE) o |= O_CREAT;
        if (flags & HK_O_TRUNC) o |= O_TRUNC;
        if (flags & HK_O_APPEND) o |= O_APPEND;
        if (flags & HK_O_EXCL) o |= O_EXCL;
        fd = open(path, o | O_CLOEXEC, 0666);
        if (fd < 0) { SET_ERR(); return NULL; }
    }
    {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            close(fd);
            if (err) *err = HK_E_ISDIR;
            return NULL;
        }
    }
    f = (hk_file *)hk_alloc(sizeof(*f));
    f->fd = fd;
    if (err) *err = HK_E_OK;
    return f;
}

void hk_file_close(hk_file *f)
{
    if (!f) return;
    close(f->fd);
    free(f);
}

i64 hk_file_read(hk_file *f, void *buf, size_t n, int *err)
{
    ssize_t r;
    do r = read(f->fd, buf, n); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 hk_file_write(hk_file *f, const void *buf, size_t n, int *err)
{
    ssize_t r;
    do r = write(f->fd, buf, n); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 hk_file_pread(hk_file *f, void *buf, size_t n, u64 offset, int *err)
{
    ssize_t r;
    do r = pread(f->fd, buf, n, (off_t)offset); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 hk_file_pwrite(hk_file *f, const void *buf, size_t n, u64 offset, int *err)
{
    ssize_t r;
    do r = pwrite(f->fd, buf, n, (off_t)offset); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 hk_file_seek(hk_file *f, i64 offset, int whence, int *err)
{
    off_t r = lseek(f->fd, (off_t)offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) SET_ERR();
    return (i64)r;
}

bool hk_file_truncate(hk_file *f, u64 size, int *err)
{
    if (ftruncate(f->fd, (off_t)size) != 0) { SET_ERR(); return false; }
    return true;
}

bool hk_file_sync(hk_file *f) { return fsync(f->fd) == 0; }

static void fill_stat(const struct stat *s, hk_stat *st)
{
    st->is_dir = S_ISDIR(s->st_mode);
    st->size = (u64)s->st_size;
    st->mtime_ns = (i64)s->st_mtim.tv_sec * 1000000000ll + s->st_mtim.tv_nsec;
    st->atime_ns = (i64)s->st_atim.tv_sec * 1000000000ll + s->st_atim.tv_nsec;
    st->ctime_ns = (i64)s->st_ctim.tv_sec * 1000000000ll + s->st_ctim.tv_nsec;
    st->id = (u64)s->st_ino;
}

bool hk_file_stat(hk_file *f, hk_stat *st)
{
    struct stat s;
    if (fstat(f->fd, &s) != 0) return false;
    fill_stat(&s, st);
    return true;
}

bool hk_path_stat(const char *path, hk_stat *st, int *err)
{
    struct stat s;
    if (hk_path_is_foreign(path)) {
        int fd = foreign_fd(path, HK_O_READ, HK_FOREIGN_ANY, err), r;
        if (fd < 0) return false;
        r = fstat(fd, &s);
        if (r != 0) SET_ERR();
        close(fd);
        if (r != 0) return false;
    } else if (stat(path, &s) != 0) {
        SET_ERR();
        return false;
    }
    fill_stat(&s, st);
    return true;
}

bool hk_path_mkdir(const char *path, int *err)
{
    if (hk_path_is_foreign(path)) return hk_foreign_mkdir(path, err);
    if (mkdir(path, 0777) != 0) { SET_ERR(); return false; }
    return true;
}

bool hk_path_remove(const char *path, int *err)
{
    if (hk_path_is_foreign(path)) return hk_foreign_remove(path, false, err);
    if (unlink(path) != 0) { SET_ERR(); return false; }
    return true;
}

bool hk_path_rmdir(const char *path, int *err)
{
    if (hk_path_is_foreign(path)) return hk_foreign_remove(path, true, err);
    if (rmdir(path) != 0) { SET_ERR(); return false; }
    return true;
}

bool hk_path_rename(const char *from, const char *to, int *err)
{
    if (hk_path_is_foreign(from) || hk_path_is_foreign(to)) return hk_foreign_rename(from, to, err);
    if (rename(from, to) != 0) { SET_ERR(); return false; }
    return true;
}

bool hk_path_list(const char *path, hk_dir_cb cb, void *user, int *err)
{
    DIR *d;
    struct dirent *e;
    bool foreign = hk_path_is_foreign(path);
    if (foreign) {
        /* A foreign directory is read through the descriptor its owner hands over, which the DIR then owns. */
        int fd = foreign_fd(path, HK_O_READ, HK_FOREIGN_DIR, err);
        if (fd < 0) return false;
        d = fdopendir(fd);
        if (!d) {
            SET_ERR();
            close(fd);
            return false;
        }
    } else {
        d = opendir(path);
        if (!d) { SET_ERR(); return false; }
    }
    while ((e = readdir(d)) != NULL) {
        bool is_dir;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            /* (Asked of the directory itself, not by path: a foreign one has no path to ask by.) */
            struct stat s;
            is_dir = fstatat(dirfd(d), e->d_name, &s, 0) == 0 && S_ISDIR(s.st_mode);
        }
        if (foreign) {
            /* Its names were given to its owner as Windows takes them (hk_host.h); the app has them back as it named them. */
            char *name = hk_strdup(e->d_name);
            hk_name_from_host(name);
            cb(name, is_dir, user);
            free(name);
            continue;
        }
        cb(e->d_name, is_dir, user);
    }
    closedir(d);
    return true;
}

bool hk_map_open(hk_map *m, const char *path)
{
    struct stat s;
    int fd = hk_path_is_foreign(path) ? foreign_fd(path, HK_O_READ, HK_FOREIGN_FILE, NULL) : open(path, O_RDONLY | O_CLOEXEC);
    void *p;
    memset(m, 0, sizeof(*m));
    if (fd < 0) return false;
    if (fstat(fd, &s) != 0 || s.st_size <= 0) { close(fd); return false; }
    p = mmap(NULL, (size_t)s.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    m->data = (const u8 *)p;
    m->size = (u64)s.st_size;
    return true;
}

bool hk_path_space(const char *path, u64 *total, u64 *available)
{
    struct statvfs s;
    if (hk_path_is_foreign(path) || statvfs(path, &s) != 0) return false;
    *total = (u64)s.f_blocks * s.f_frsize;
    *available = (u64)s.f_bavail * s.f_frsize;
    return true;
}

void hk_map_close(hk_map *m)
{
    if (m->data) munmap((void *)m->data, (size_t)m->size);
    memset(m, 0, sizeof(*m));
}

/* ---------------------------------------------------------------- sockets
 *
 * Non-blocking BSD sockets. One thread of the host's waits on all of them at
 * once, edge-triggered (epoll with EPOLLET): every change of any socket's
 * state wakes it, and it tells whoever hk_sock_set_notify named. What holds
 * at a given moment is poll()'s answer with no wait.
 */
struct hk_sock {
    int fd;
    int family, type;
};

static int g_watch_fd = -1;
static pthread_once_t g_watch_once = PTHREAD_ONCE_INIT;
static void (*volatile g_sock_notify)(void);
void hk_sock_set_notify(void (*changed)(void)) { g_sock_notify = changed; }

static void *watch_main(void *arg)
{
    struct epoll_event ev[32];
    sigset_t all;
    (void)arg;
    /* (The signals the layer sends are for the app's threads.) */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);
    for (;;) {
        int n = epoll_wait(g_watch_fd, ev, 32, -1);
        if (n < 0 && errno != EINTR) {
            hk_err("host: the wait on the sockets failed (errno %d): nobody waiting on a socket is told of a change any more", errno);
            return NULL;
        }
        if (n > 0 && g_sock_notify) g_sock_notify();
    }
}

static void watch_start(void)
{
    pthread_t t;
    pthread_attr_t a;
    g_watch_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_watch_fd < 0) {
        hk_err("host: epoll_create1 failed (errno %d): nobody waiting on a socket is told of a change", errno);
        return;
    }
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, watch_main, NULL) != 0) hk_err("host: the thread that waits on the sockets could not be started");
    pthread_attr_destroy(&a);
}

static int net_err(int e)
{
    switch (e) {
    case 0: return HK_NE_OK;
    case EAGAIN: return HK_NE_AGAIN;
    case EINPROGRESS: return HK_NE_INPROGRESS;
    case EALREADY: return HK_NE_ALREADY;
    case EISCONN: return HK_NE_ISCONN;
    case ENOTCONN: return HK_NE_NOTCONN;
    case ECONNREFUSED: return HK_NE_CONNREFUSED;
    case ECONNRESET: return HK_NE_CONNRESET;
    case ECONNABORTED: return HK_NE_CONNABORTED;
    case ETIMEDOUT: return HK_NE_TIMEDOUT;
    case ENETUNREACH: return HK_NE_NETUNREACH;
    case EHOSTUNREACH: case EHOSTDOWN: return HK_NE_HOSTUNREACH;
    case ENETDOWN: return HK_NE_NETDOWN;
    case ENETRESET: return HK_NE_NETRESET;
    case EADDRINUSE: return HK_NE_ADDRINUSE;
    case EADDRNOTAVAIL: return HK_NE_ADDRNOTAVAIL;
    case EAFNOSUPPORT: case EPFNOSUPPORT: return HK_NE_AFNOSUPPORT;
    case EPROTONOSUPPORT: case EPROTOTYPE: return HK_NE_PROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return HK_NE_SOCKTNOSUPPORT;
    case EOPNOTSUPP: return HK_NE_OPNOTSUPP;
    case ENOPROTOOPT: return HK_NE_NOPROTOOPT;
    case EINVAL: return HK_NE_INVAL;
    case EACCES: case EPERM: return HK_NE_ACCES;
    case EMSGSIZE: return HK_NE_MSGSIZE;
    case ENOBUFS: case ENOMEM: return HK_NE_NOBUFS;
    case EMFILE: case ENFILE: return HK_NE_MFILE;
    case ESHUTDOWN: return HK_NE_SHUTDOWN;
    case EPIPE: return HK_NE_PIPE;
    case EDESTADDRREQ: return HK_NE_DESTADDRREQ;
    case EFAULT: return HK_NE_FAULT;
    default: return HK_NE_IO;
    }
}

static socklen_t addr_to_native(const hk_sockaddr *a, struct sockaddr_storage *ss)
{
    memset(ss, 0, sizeof(*ss));
    if (a->family == HK_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->addr, 4);
        return sizeof(*in);
    }
    if (a->family == HK_AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ss;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(a->port);
        in6->sin6_flowinfo = a->flowinfo;
        memcpy(&in6->sin6_addr, a->addr, 16);
        in6->sin6_scope_id = a->scope_id;
        return sizeof(*in6);
    }
    return 0;
}

static void addr_from_native(const struct sockaddr *sa, socklen_t len, hk_sockaddr *a)
{
    memset(a, 0, sizeof(*a));
    if (!sa) return;
    if (sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        a->family = HK_AF_INET;
        a->port = ntohs(in->sin_port);
        memcpy(a->addr, &in->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        a->family = HK_AF_INET6;
        a->port = ntohs(in6->sin6_port);
        a->flowinfo = in6->sin6_flowinfo;
        memcpy(a->addr, &in6->sin6_addr, 16);
        a->scope_id = in6->sin6_scope_id;
    }
}

static hk_sock *sock_wrap(int fd, int family, int type)
{
    hk_sock *k = (hk_sock *)hk_alloc(sizeof(*k));
    struct epoll_event ev;
    pthread_once(&g_watch_once, watch_start);
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDHUP | EPOLLET;
    if (g_watch_fd >= 0) epoll_ctl(g_watch_fd, EPOLL_CTL_ADD, fd, &ev);
    k->fd = fd;
    k->family = family;
    k->type = type;
    return k;
}

hk_sock *hk_sock_open(int family, int type, int protocol, int *err)
{
    int af = family == HK_AF_INET ? AF_INET : family == HK_AF_INET6 ? AF_INET6 : 0;
    int st = type == HK_SOCK_STREAM ? SOCK_STREAM : type == HK_SOCK_DGRAM ? SOCK_DGRAM : type == HK_SOCK_RAW ? SOCK_RAW : 0, fd;
    if (!af || !st) {
        *err = !af ? HK_NE_AFNOSUPPORT : HK_NE_SOCKTNOSUPPORT;
        return NULL;
    }
    fd = socket(af, st | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0) {
        *err = net_err(errno);
        return NULL;
    }
    if (af == AF_INET6) {
        int off = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    }
    return sock_wrap(fd, family, type);
}

void hk_sock_close(hk_sock *k)
{
    if (!k) return;
    if (g_watch_fd >= 0) epoll_ctl(g_watch_fd, EPOLL_CTL_DEL, k->fd, NULL);
    close(k->fd);
    free(k);
}

bool hk_sock_bind(hk_sock *k, const hk_sockaddr *a, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len = addr_to_native(a, &ss);
    if (!len) {
        *err = HK_NE_AFNOSUPPORT;
        return false;
    }
    if (bind(k->fd, (const struct sockaddr *)&ss, len) != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

bool hk_sock_listen(hk_sock *k, int backlog, int *err)
{
    if (listen(k->fd, backlog < 1 ? 1 : backlog) != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

hk_sock *hk_sock_accept(hk_sock *k, hk_sockaddr *peer, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int fd = accept4(k->fd, (struct sockaddr *)&ss, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        *err = net_err(errno == EWOULDBLOCK ? EAGAIN : errno);
        return NULL;
    }
    if (peer) addr_from_native((const struct sockaddr *)&ss, len, peer);
    return sock_wrap(fd, k->family, k->type);
}

bool hk_sock_connect(hk_sock *k, const hk_sockaddr *a, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len;
    if (!a->family) {
        memset(&ss, 0, sizeof(ss));
        ss.ss_family = AF_UNSPEC;
        len = sizeof(struct sockaddr);
    } else if (!(len = addr_to_native(a, &ss))) {
        *err = HK_NE_AFNOSUPPORT;
        return false;
    }
    if (connect(k->fd, (const struct sockaddr *)&ss, len) == 0) return true;
    *err = net_err(errno);
    return false;
}

static int native_flags(int flags)
{
    return ((flags & HK_MSG_PEEK) ? MSG_PEEK : 0) | ((flags & HK_MSG_OOB) ? MSG_OOB : 0) | ((flags & HK_MSG_DONTROUTE) ? MSG_DONTROUTE : 0);
}

i32 hk_sock_send(hk_sock *k, const void *buf, u32 n, int flags, const hk_sockaddr *to, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len = 0;
    ssize_t r;
    if (to && !(len = addr_to_native(to, &ss))) {
        *err = HK_NE_AFNOSUPPORT;
        return -1;
    }
    r = sendto(k->fd, buf, n, native_flags(flags) | MSG_NOSIGNAL | MSG_DONTWAIT, to ? (const struct sockaddr *)&ss : NULL, len);
    if (r < 0) {
        *err = net_err(errno == EWOULDBLOCK ? EAGAIN : errno);
        return -1;
    }
    return (i32)r;
}

i32 hk_sock_recv(hk_sock *k, void *buf, u32 n, int flags, hk_sockaddr *from, int *said, int *err)
{
    struct sockaddr_storage ss;
    struct iovec iov;
    struct msghdr m;
    ssize_t r;
    memset(&m, 0, sizeof(m));
    memset(&ss, 0, sizeof(ss));
    iov.iov_base = buf;
    iov.iov_len = n;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (from) {
        m.msg_name = &ss;
        m.msg_namelen = sizeof(ss);
    }
    if (said) *said = 0;
    r = recvmsg(k->fd, &m, native_flags(flags) | MSG_DONTWAIT);
    if (r < 0) {
        *err = net_err(errno == EWOULDBLOCK ? EAGAIN : errno);
        return -1;
    }
    if (said && (m.msg_flags & MSG_TRUNC)) *said = HK_MSG_TRUNCATED;
    if (from) addr_from_native((const struct sockaddr *)&ss, m.msg_namelen, from);
    return (i32)r;
}

bool hk_sock_shutdown(hk_sock *k, int how, int *err)
{
    if (shutdown(k->fd, how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR) != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

bool hk_sock_name(hk_sock *k, bool peer, hk_sockaddr *a, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    int r = peer ? getpeername(k->fd, (struct sockaddr *)&ss, &len) : getsockname(k->fd, (struct sockaddr *)&ss, &len);
    if (r != 0) {
        *err = net_err(errno);
        return false;
    }
    addr_from_native((const struct sockaddr *)&ss, len, a);
    if (!a->family) a->family = k->family;
    return true;
}

int hk_sock_ready(hk_sock *k)
{
    struct pollfd p;
    int out = 0;
    p.fd = k->fd;
    p.events = POLLIN | POLLOUT | POLLPRI | POLLRDHUP;
    p.revents = 0;
    if (poll(&p, 1, 0) <= 0) return 0;
    if (p.revents & (POLLIN | POLLPRI)) out |= HK_SOCK_IN;
    if (p.revents & POLLOUT) out |= HK_SOCK_OUT;
    if (p.revents & POLLERR) out |= HK_SOCK_ERR;
    if (p.revents & POLLHUP) out |= HK_SOCK_HUP;
    if (p.revents & POLLRDHUP) out |= HK_SOCK_RDHUP;
    return out;
}

int hk_sock_error(hk_sock *k)
{
    int e = 0;
    socklen_t len = sizeof(e);
    if (getsockopt(k->fd, SOL_SOCKET, SO_ERROR, &e, &len) != 0) return HK_NE_OK;
    return net_err(e);
}

i32 hk_sock_pending(hk_sock *k)
{
    int n = 0;
    return ioctl(k->fd, FIONREAD, &n) == 0 ? n : 0;
}

bool hk_sock_at_mark(hk_sock *k)
{
    int at = 0;
    return ioctl(k->fd, SIOCATMARK, &at) == 0 && at;
}

static int option_of(int option, int *level)
{
    *level = SOL_SOCKET;
    switch (option) {
    case HK_SO_REUSEADDR: return SO_REUSEADDR;
    case HK_SO_KEEPALIVE: return SO_KEEPALIVE;
    case HK_SO_BROADCAST: return SO_BROADCAST;
    case HK_SO_OOBINLINE: return SO_OOBINLINE;
    case HK_SO_RCVBUF: return SO_RCVBUF;
    case HK_SO_SNDBUF: return SO_SNDBUF;
    case HK_SO_DONTROUTE: return SO_DONTROUTE;
    case HK_SO_LINGER: return SO_LINGER;
    }
    *level = IPPROTO_TCP;
    switch (option) {
    case HK_TCP_NODELAY: return TCP_NODELAY;
    case HK_TCP_KEEPIDLE: return TCP_KEEPIDLE;
    case HK_TCP_KEEPINTVL: return TCP_KEEPINTVL;
    case HK_TCP_KEEPCNT: return TCP_KEEPCNT;
    case HK_TCP_MAXSEG: return TCP_MAXSEG;
    }
    *level = IPPROTO_IP;
    switch (option) {
    case HK_IP_TOS: return IP_TOS;
    case HK_IP_TTL: return IP_TTL;
    case HK_IP_MULTICAST_TTL: return IP_MULTICAST_TTL;
    case HK_IP_MULTICAST_LOOP: return IP_MULTICAST_LOOP;
    case HK_IP_MULTICAST_IF: return IP_MULTICAST_IF;
    }
    *level = IPPROTO_IPV6;
    switch (option) {
    case HK_IPV6_V6ONLY: return IPV6_V6ONLY;
    case HK_IPV6_UNICAST_HOPS: return IPV6_UNICAST_HOPS;
    case HK_IPV6_MULTICAST_HOPS: return IPV6_MULTICAST_HOPS;
    case HK_IPV6_MULTICAST_LOOP: return IPV6_MULTICAST_LOOP;
    case HK_IPV6_MULTICAST_IF: return IPV6_MULTICAST_IF;
    case HK_IPV6_TCLASS: return IPV6_TCLASS;
    }
    return -1;
}

bool hk_sock_set(hk_sock *k, int option, int value, int *err)
{
    int level, name = option_of(option, &level), r;
    if (name < 0) {
        *err = HK_NE_NOPROTOOPT;
        return false;
    }
    if (option == HK_SO_LINGER) {
        struct linger lg;
        lg.l_onoff = value >= 0;
        lg.l_linger = value < 0 ? 0 : value;
        r = setsockopt(k->fd, level, name, &lg, sizeof(lg));
    } else if (option == HK_IP_MULTICAST_IF) {
        struct in_addr a;
        memcpy(&a, &value, 4);
        r = setsockopt(k->fd, level, name, &a, sizeof(a));
    } else {
        r = setsockopt(k->fd, level, name, &value, sizeof(value));
    }
    if (r != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

bool hk_sock_get(hk_sock *k, int option, int *value, int *err)
{
    int level, name = option_of(option, &level), v = 0;
    socklen_t len = sizeof(v);
    if (name < 0) {
        *err = HK_NE_NOPROTOOPT;
        return false;
    }
    if (option == HK_SO_LINGER) {
        struct linger lg;
        len = sizeof(lg);
        memset(&lg, 0, sizeof(lg));
        if (getsockopt(k->fd, level, name, &lg, &len) != 0) {
            *err = net_err(errno);
            return false;
        }
        *value = lg.l_onoff ? lg.l_linger : -1;
        return true;
    }
    if (getsockopt(k->fd, level, name, &v, &len) != 0) {
        *err = net_err(errno);
        return false;
    }
    *value = len == 1 ? (v & 0xFF) : v;
    return true;
}

bool hk_sock_multicast(hk_sock *k, bool join, const hk_sockaddr *group, const hk_sockaddr *iface, u32 index, int *err)
{
    int r;
    if (group->family == HK_AF_INET) {
        struct ip_mreqn m;
        memset(&m, 0, sizeof(m));
        memcpy(&m.imr_multiaddr, group->addr, 4);
        if (iface && iface->family == HK_AF_INET) memcpy(&m.imr_address, iface->addr, 4);
        m.imr_ifindex = (int)index;
        r = setsockopt(k->fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof(m));
    } else if (group->family == HK_AF_INET6) {
        struct ipv6_mreq m;
        memset(&m, 0, sizeof(m));
        memcpy(&m.ipv6mr_multiaddr, group->addr, 16);
        m.ipv6mr_interface = index;
        r = setsockopt(k->fd, IPPROTO_IPV6, join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP, &m, sizeof(m));
    } else {
        *err = HK_NE_AFNOSUPPORT;
        return false;
    }
    if (r != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

/* ---------------------------------------------------- names and interfaces */

static int eai_of(int e)
{
    switch (e) {
    case EAI_NONAME: return HK_EAI_NONAME;
    case EAI_AGAIN: return HK_EAI_AGAIN;
    case EAI_FAIL: return HK_EAI_FAIL;
#ifdef EAI_NODATA
    case EAI_NODATA: return HK_EAI_NODATA;
#endif
    case EAI_FAMILY: return HK_EAI_FAMILY;
    case EAI_MEMORY: return HK_EAI_MEMORY;
    case EAI_SYSTEM: return HK_EAI_SYSTEM;
    default: return HK_EAI_FAIL;
    }
}

int hk_net_resolve(const char *name, int family, int flags, hk_sockaddr **addrs, int *count, char **canonical)
{
    struct addrinfo hints, *res = NULL, *ai;
    hk_sockaddr *out = NULL;
    int e, got = 0, cap = 0, i;
    *addrs = NULL;
    *count = 0;
    if (canonical) *canonical = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family == HK_AF_INET ? AF_INET : family == HK_AF_INET6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = ((flags & HK_RESOLVE_CANONICAL) ? AI_CANONNAME : 0) | ((flags & HK_RESOLVE_ADDRCONFIG) ? AI_ADDRCONFIG : 0);
    e = getaddrinfo(name, NULL, &hints, &res);
    if (e) return eai_of(e);
    for (ai = res; ai; ai = ai->ai_next) {
        hk_sockaddr a;
        addr_from_native(ai->ai_addr, ai->ai_addrlen, &a);
        if (!a.family) continue;
        for (i = 0; i < got && memcmp(&out[i], &a, sizeof(a)); i++) { }
        if (i < got) continue;
        if (got == cap) {
            cap = cap ? cap * 2 : 8;
            out = (hk_sockaddr *)hk_realloc(out, sizeof(*out) * (size_t)cap);
        }
        out[got++] = a;
    }
    if (canonical && res && res->ai_canonname) *canonical = hk_strdup(res->ai_canonname);
    freeaddrinfo(res);
    if (!got) {
        free(out);
        if (canonical) {
            free(*canonical);
            *canonical = NULL;
        }
        return HK_EAI_NODATA;
    }
    *addrs = out;
    *count = got;
    return HK_EAI_OK;
}

int hk_net_name_of(const hk_sockaddr *a, char *name, size_t size)
{
    struct sockaddr_storage ss;
    socklen_t len = addr_to_native(a, &ss);
    int e;
    if (!len) return HK_EAI_FAMILY;
    e = getnameinfo((const struct sockaddr *)&ss, len, name, (socklen_t)size, NULL, 0, NI_NAMEREQD);
    return e ? eai_of(e) : HK_EAI_OK;
}

static int prefix_of(const struct sockaddr *mask)
{
    const u8 *b;
    int n = 0, len, i;
    if (!mask) return 0;
    if (mask->sa_family == AF_INET) {
        b = (const u8 *)&((const struct sockaddr_in *)mask)->sin_addr;
        len = 4;
    } else if (mask->sa_family == AF_INET6) {
        b = (const u8 *)&((const struct sockaddr_in6 *)mask)->sin6_addr;
        len = 16;
    } else {
        return 0;
    }
    for (i = 0; i < len; i++)
        for (int bit = 7; bit >= 0 && ((b[i] >> bit) & 1); bit--) n++;
    return n;
}

int hk_net_interfaces(hk_netif *out, int max)
{
    struct ifaddrs *list = NULL, *ifa;
    char names[64][IF_NAMESIZE];
    int n = 0, i, probe;
    if (getifaddrs(&list) != 0) return -1;
    probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    for (ifa = list; ifa; ifa = ifa->ifa_next) {
        hk_netif *f = NULL;
        if (!ifa->ifa_name) continue;
        for (i = 0; i < n && i < (int)HK_COUNT(names); i++)
            if (!strcmp(names[i], ifa->ifa_name)) break;
        if (i < n && i < max) f = &out[i];
        if (i == n) {
            if (n < (int)HK_COUNT(names)) hk_strlcpy(names[n], ifa->ifa_name, IF_NAMESIZE);
            if (n < max) {
                struct ifreq rq;
                f = &out[n];
                memset(f, 0, sizeof(*f));
                f->index = if_nametoindex(ifa->ifa_name);
                if (ifa->ifa_flags & IFF_LOOPBACK) f->kind = HK_IF_LOOPBACK;
                else if (!strncmp(ifa->ifa_name, "wl", 2)) f->kind = HK_IF_WIFI;
                else if (!strncmp(ifa->ifa_name, "eth", 3) || !strncmp(ifa->ifa_name, "en", 2)) f->kind = HK_IF_ETHERNET;
                else if (!strncmp(ifa->ifa_name, "rmnet", 5) || !strncmp(ifa->ifa_name, "wwan", 4)) f->kind = HK_IF_CELLULAR;
                else if (!strncmp(ifa->ifa_name, "tun", 3) || !strncmp(ifa->ifa_name, "tap", 3) || !strncmp(ifa->ifa_name, "sit", 3)) f->kind = HK_IF_TUNNEL;
                else f->kind = HK_IF_OTHER;
                if (ifa->ifa_flags & IFF_UP) f->flags |= HK_IFF_UP;
                if (ifa->ifa_flags & IFF_RUNNING) f->flags |= HK_IFF_RUNNING;
                if (ifa->ifa_flags & IFF_MULTICAST) f->flags |= HK_IFF_MULTICAST;
                if (ifa->ifa_flags & IFF_BROADCAST) f->flags |= HK_IFF_BROADCAST;
                if (ifa->ifa_flags & IFF_POINTOPOINT) f->flags |= HK_IFF_POINTOPOINT;
                memset(&rq, 0, sizeof(rq));
                hk_strlcpy(rq.ifr_name, ifa->ifa_name, sizeof(rq.ifr_name));
                f->mtu = probe >= 0 && ioctl(probe, SIOCGIFMTU, &rq) == 0 ? (u32)rq.ifr_mtu : 1500;
            }
            n++;
        }
        if (!f || !ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *ll = (const struct sockaddr_ll *)ifa->ifa_addr;
            if (ll->sll_halen > 0 && ll->sll_halen <= sizeof(f->hw)) {
                memcpy(f->hw, ll->sll_addr, ll->sll_halen);
                f->hw_len = ll->sll_halen;
            }
        } else if (f->naddrs < HK_NETIF_ADDRS) {
            hk_sockaddr a;
            addr_from_native(ifa->ifa_addr, ifa->ifa_addr->sa_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in), &a);
            if (!a.family) continue;
            f->addrs[f->naddrs].addr = a;
            f->addrs[f->naddrs].prefix = prefix_of(ifa->ifa_netmask);
            f->naddrs++;
        }
    }
    if (probe >= 0) close(probe);
    freeifaddrs(list);
    return n;
}

/* ------------------------------------------------------ host shared libraries */

void *hk_dl_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *hk_dl_sym(void *lib, const char *name) { return dlsym(lib, name); }

bool hk_host_module_at(hk_addr address, char *name, size_t size, hk_addr *base)
{
    Dl_info info;
    const char *file;
    if (!dladdr((void *)address, &info) || !info.dli_fname) return false;
    file = strrchr(info.dli_fname, '/');
    snprintf(name, size, "%s", file ? file + 1 : info.dli_fname);
    if (base) *base = (hk_addr)info.dli_fbase;
    return true;
}

/* ------------------------------------------------------------------- misc */

bool hk_host_address_space(u32 *free_mb, u32 *largest_free_mb, u32 *reserved_mb, u32 *committed_mb)
{
    (void)free_mb; (void)largest_free_mb; (void)reserved_mb; (void)committed_mb;
    return false;
}

bool hk_host_gpu_memory(void *device, u32 *budget_mb, u32 *used_mb)
{
    (void)device; (void)budget_mb; (void)used_mb;
    return false;
}

bool hk_host_keyed_mutex(void *mutex, bool acquire, u32 ms)
{
    (void)mutex; (void)acquire; (void)ms;
    return false;
}

bool hk_host_free_stretches(u32 *mb, int n)
{
    (void)mb; (void)n;
    return false;
}

u32 hk_host_d3d11_removed_reason(void *device)
{
    (void)device;
    return 0;
}

bool hk_host_d3d11_options(void *device, bool *constant_buffer_offsets, bool *constant_buffer_partial_updates)
{
    (void)device;
    *constant_buffer_offsets = *constant_buffer_partial_updates = false;
    return false;
}

int hk_host_d3d11_feature_level(void *device)
{
    (void)device;
    return 0;
}

void hk_random(void *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    if (fd >= 0) {
        while (got < n) {
            ssize_t r = read(fd, (u8 *)buf + got, n - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    for (; got < n; got++) ((u8 *)buf)[got] = (u8)rand();
}

void hk_host_print(const char *line)
{
    fputs(line, stderr);
    fputc('\n', stderr);
}

const char *hk_host_name(void)
{
#if defined(__arm__)
    return "linux-arm";
#else
    return "linux";
#endif
}

#endif /* !_WIN32 */
