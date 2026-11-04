/*
 * The host layer over POSIX: Linux, where the ARM32 build of the core runs
 * under qemu-arm so that the tests can execute guest code.
 */
#if !defined(_WIN32)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "mg_host.h"

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
    case MG_PROT_R: return PROT_READ;
    case MG_PROT_RW: return PROT_READ | PROT_WRITE;
    case MG_PROT_RX: return PROT_READ | PROT_EXEC;
    case MG_PROT_RWX: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
    }
}

void *mg_mem_reserve(size_t size)
{
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* Sections: a memfd of that size, viewed with mmap(MAP_SHARED). */
struct mg_section {
    int fd;
};

static volatile i32 g_views_kb;

static void views_count(size_t size, int sign) { mg_atomic_add(&g_views_kb, sign * (i32)((size + 1023) >> 10)); }

u32 mg_views_kb(void) { return (u32)g_views_kb; }

mg_section *mg_section_create(u64 size)
{
    mg_section *s;
    int fd;
    if (!size) return NULL;
    fd = (int)syscall(SYS_memfd_create, "megler-section", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return NULL;
    }
    s = (mg_section *)calloc(1, sizeof(*s));
    if (!s) {
        close(fd);
        return NULL;
    }
    s->fd = fd;
    return s;
}

void *mg_section_map(mg_section *s, u64 offset, size_t size, bool writable, void **base)
{
    void *v;
    if (!s || !size) return NULL;
    v = mmap(NULL, size, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, s->fd, (off_t)offset);
    if (v == MAP_FAILED) return NULL;
    views_count(size, 1);
    if (base) *base = v;
    return v;
}

void *mg_section_map_code(mg_section *s, u64 offset, size_t size, void **base)
{
    void *v;
    if (!s || !size) return NULL;
    v = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, s->fd, (off_t)offset);
    if (v == MAP_FAILED) return NULL;
    views_count(size, 1);
    if (base) *base = v;
    return v;
}

void mg_section_unmap(void *base, size_t size)
{
    if (!base || !size) return;
    munmap(base, size);
    views_count(size, -1);
}

void mg_section_close(mg_section *s)
{
    if (!s) return;
    close(s->fd);
    free(s);
}

void *mg_mem_reserve_at(void *p, size_t size)
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

bool mg_mem_commit(void *p, size_t size, int prot)
{
    return mprotect(p, size, prot_of(prot)) == 0;
}

bool mg_mem_rwx(void) { return true; }

bool mg_mem_protect(void *p, size_t size, int prot)
{
    return mprotect(p, size, prot_of(prot)) == 0;
}

bool mg_mem_decommit(void *p, size_t size)
{
    void *q = mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return q != MAP_FAILED;
}

bool mg_mem_protect_committed(void *p, size_t size, int prot) { return mg_mem_protect(p, size, prot); }
i64 mg_mem_committed(const void *p, size_t size)
{
    (void)p;
    (void)size;
    return -1;
}

size_t mg_mem_discard(void *p, size_t size, void (*fn)(u8 *at, size_t n, void *user), void *user)
{
    /* Decommitted as on Windows (no access until committed again, zero then), which is what the layer above is tried by. */
    if (fn) fn((u8 *)p, size, user);
    return mg_mem_decommit(p, size) ? size : 0;
}

static void (*g_before_system_write)(void *p, size_t size);
void mg_mem_set_before_system_write(void (*fn)(void *p, size_t size)) { g_before_system_write = fn; }

bool mg_mem_page_state(const void *p, bool *committed, int *prot)
{
    (void)p;
    (void)committed;
    (void)prot;
    return false;
}
#define BEFORE_SYSTEM_WRITE(p, n) do { if (g_before_system_write && (n)) g_before_system_write((p), (n)); } while (0)

void mg_mem_release(void *p, size_t size) { munmap(p, size); }

void mg_icache_flush(void *p, size_t size)
{
#if defined(__arm__)
    __builtin___clear_cache((char *)p, (char *)p + size);
#else
    (void)p; (void)size;
#endif
}

void *mg_mem_alloc(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* (No write watch here: a caller takes every page for written.) */
void *mg_mem_alloc_watched(size_t size)
{
    (void)size;
    return NULL;
}

int mg_mem_written_pages(void *p, size_t size, u32 *out, int max)
{
    (void)p;
    (void)size;
    (void)out;
    (void)max;
    return -1;
}

static u64 (*g_limit)(void), (*g_usage)(void);
void mg_mem_set_probe(u64 (*limit)(void), u64 (*usage)(void)) { g_limit = limit; g_usage = usage; }
u64 mg_mem_limit(void) { return g_limit ? g_limit() : 0; }
u64 mg_mem_usage(void) { return g_usage ? g_usage() : 0; }

/* --------------------------------------------------------------- threads */

struct mg_thread {
    pthread_t t;
    mg_thread_fn fn;
    void *arg;
    /*
     * Stopped for the collector (mg_thread_suspend): its registers as the signal found them, whether it may go on, and
     * whether its handler is in its wait (1 from the registers' saving until it leaves the wait).
     */
    mg_thread_regs regs;
    volatile i32 go_on, stopped;
};

static MG_THREAD_LOCAL mg_thread *t_current;

static void (*volatile g_end_hook)(void);
void mg_thread_set_end_hook(void (*fn)(void)) { g_end_hook = fn; }

static void *thread_main(void *p)
{
    mg_thread *t = (mg_thread *)p;
    t_current = t;
    t->fn(t->arg);
    if (g_end_hook) g_end_hook();
    return NULL;
}

void mg_thread_runs_app_code(void) { }

u64 mg_thread_cpu_ns(mg_thread *t)
{
    clockid_t c = CLOCK_THREAD_CPUTIME_ID;
    struct timespec ts;
    if (t && pthread_getcpuclockid(t->t, &c) != 0) return 0;
    if (clock_gettime(c, &ts) != 0) return 0;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 mg_process_cpu_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

mg_thread *mg_thread_current(void)
{
    if (!t_current) {
        t_current = (mg_thread *)mg_alloc(sizeof(*t_current));
        t_current->t = pthread_self();
    }
    return t_current;
}

/* A signal of the host's own carries the interruption; its handler looks at where the thread was. */
#define INTERRUPT_SIGNAL (SIGRTMIN + 6)
static void (*g_interrupt_fn)(void);
static bool (*g_interrupt_in_guest)(mg_addr pc);

static volatile i32 g_interrupts_asked, g_interrupts_done;

void mg_thread_interrupt_report(char *out, size_t size)
{
    snprintf(out, size, "%d asked for, %d found the thread in guest code", (int)g_interrupts_asked, (int)g_interrupts_done);
}

static void on_interrupt(int sig, siginfo_t *si, void *ctx)
{
    int saved = errno;
    (void)sig; (void)si;
#if defined(__arm__)
    if (g_interrupt_fn && g_interrupt_in_guest && g_interrupt_in_guest((mg_addr)(((ucontext_t *)ctx)->uc_mcontext.arm_pc & ~1u))) {
        mg_atomic_add(&g_interrupts_done, 1);
        g_interrupt_fn();
    }
#else
    (void)ctx;
#endif
    errno = saved;
}

void mg_thread_set_interrupt(void (*fn)(void), bool (*in_guest_code)(mg_addr pc))
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
 * Stopping a thread for the collector: a signal whose handler saves the registers the thread was stopped with, says so in
 * the thread's own flag, and waits for the other signal (sigsuspend, which a handler may call) until it is told to go on,
 * and clears the flag as it goes on: mg_thread_resume returns only once the handler has left its wait, so that the next
 * mg_thread_suspend cannot find it still there. A flag of each thread's (they were two semaphores for all) lets the
 * collector's helpers stop and resume several threads at once.
 */
#define SUSPEND_SIGNAL (SIGRTMIN + 7)
#define RESUME_SIGNAL (SIGRTMIN + 8)
static pthread_once_t g_suspend_once = PTHREAD_ONCE_INIT;

static void on_resume(int sig) { (void)sig; }

static void on_suspend(int sig, siginfo_t *si, void *ctx)
{
    int saved = errno;
    mg_thread *t = t_current;
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
        t->regs.sp = (mg_addr)g[REG_RSP];
        t->regs.pc = (mg_addr)g[REG_RIP];
        t->regs.flags = (u32)g[REG_EFL];
        for (i = 0; i < (int)MG_COUNT(which); i++) t->regs.r[i] = (mg_addr)g[which[i]];
        t->regs.n = (int)MG_COUNT(which);
    }
#else
    (void)ctx;
    t->regs.n = 0;
    t->regs.sp = (mg_addr)&wait_for;
#endif
    sigfillset(&wait_for);
    sigdelset(&wait_for, RESUME_SIGNAL);
    mg_atomic_xchg(&t->stopped, 1);
    while (!t->go_on) sigsuspend(&wait_for);
    mg_atomic_xchg(&t->stopped, 0);
    errno = saved;
}

static void suspend_setup(void)
{
    struct sigaction sa;
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

bool mg_thread_suspend(mg_thread *t, mg_thread_regs *out)
{
    memset(out, 0, sizeof(*out));
    if (!t || pthread_equal(t->t, pthread_self())) return false;
    pthread_once(&g_suspend_once, suspend_setup);
    t->go_on = 0;
    if (pthread_kill(t->t, SUSPEND_SIGNAL) != 0) return false;
    /* (An interlocked read: the registers were saved before the flag was set.) */
    while (mg_atomic_cas(&t->stopped, 1, 1) != 1) sched_yield();
    *out = t->regs;
    return true;
}

void mg_thread_resume(mg_thread *t)
{
    if (!t) return;
    mg_atomic_xchg(&t->go_on, 1);
    if (pthread_kill(t->t, RESUME_SIGNAL) != 0) return;
    while (mg_atomic_cas(&t->stopped, 0, 0) != 0) sched_yield();
}

/* (Whether it found the thread in guest code is not known here: true means only that it was asked.) */
bool mg_thread_interrupt(mg_thread *t)
{
    if (!t || !g_interrupt_fn) return false;
    mg_atomic_add(&g_interrupts_asked, 1);
    return pthread_kill(t->t, INTERRUPT_SIGNAL) == 0;
}

mg_thread *mg_thread_start(mg_thread_fn fn, void *arg, size_t stack_size, const char *name)
{
    mg_thread *t = (mg_thread *)mg_alloc(sizeof(*t));
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

void mg_thread_join(mg_thread *t)
{
    pthread_join(t->t, NULL);
    free(t);
}

void mg_thread_detach(mg_thread *t)
{
    /* The thread's main still reads t; it is small and lives as long as the process. */
    pthread_detach(t->t);
}

void mg_thread_exit(void)
{
    if (g_end_hook) g_end_hook();
    pthread_exit(NULL);
}
u32 mg_thread_self_id(void) { return (u32)syscall(SYS_gettid); }
u32 mg_thread_id(const mg_thread *t) { (void)t; return 0; }

int mg_thread_snapshot_id(u32 id, mg_thread_regs *regs, u32 *words, int max_words)
{
    (void)id;
    (void)words;
    (void)max_words;
    memset(regs, 0, sizeof(*regs));
    return -1;
}
void mg_thread_yield(void) { sched_yield(); }

void mg_sleep_ns(u64 ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

int mg_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void mg_lock_init(mg_lock *l) { pthread_mutex_init(&l->m, NULL); }
void mg_lock_acquire(mg_lock *l) { pthread_mutex_lock(&l->m); }
bool mg_lock_try(mg_lock *l) { return pthread_mutex_trylock(&l->m) == 0; }
void mg_lock_release(mg_lock *l) { pthread_mutex_unlock(&l->m); }

void mg_cond_init(mg_cond *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &a);
    pthread_condattr_destroy(&a);
    c->monotonic = true;
}

void mg_cond_wait(mg_cond *c, mg_lock *l) { pthread_cond_wait(&c->c, &l->m); }

bool mg_cond_wait_ns(mg_cond *c, mg_lock *l, u64 ns)
{
    /* A statically initialised condition waits on the realtime clock; one made by mg_cond_init on the monotonic one. */
    struct timespec ts;
    clock_gettime(c->monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ns / 1000000000ull);
    ts.tv_nsec += (long)(ns % 1000000000ull);
    if (ts.tv_nsec >= 1000000000l) { ts.tv_nsec -= 1000000000l; ts.tv_sec++; }
    return pthread_cond_timedwait(&c->c, &l->m, &ts) == 0;
}

void mg_cond_signal(mg_cond *c) { pthread_cond_signal(&c->c); }
void mg_cond_broadcast(mg_cond *c) { pthread_cond_broadcast(&c->c); }

i32 mg_atomic_cas(volatile i32 *p, i32 expect, i32 value) { return __sync_val_compare_and_swap(p, expect, value); }
i32 mg_atomic_add(volatile i32 *p, i32 delta) { return __sync_add_and_fetch(p, delta); }
i32 mg_atomic_xchg(volatile i32 *p, i32 value) { return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST); }
void *mg_atomic_cas_ptr(void *volatile *p, void *expect, void *value) { return __sync_val_compare_and_swap(p, expect, value); }
i64 mg_atomic_cas64(volatile i64 *p, i64 expect, i64 value) { return __sync_val_compare_and_swap(p, expect, value); }

bool mg_futex_wait(volatile i32 *addr, i32 expect, u64 timeout_ns)
{
    struct timespec ts;
    long r;
    ts.tv_sec = (time_t)(timeout_ns / 1000000000ull);
    ts.tv_nsec = (long)(timeout_ns % 1000000000ull);
    r = syscall(SYS_futex, addr, 0 /* FUTEX_WAIT */, expect, timeout_ns ? &ts : NULL, NULL, 0);
    return !(r < 0 && errno == ETIMEDOUT);
}

void mg_futex_wake(volatile i32 *addr, bool all)
{
    syscall(SYS_futex, addr, 1 /* FUTEX_WAKE */, all ? 0x7FFFFFFF : 1, NULL, NULL, 0);
}

/* A thread's stack is all there from the start here (and the first thread's grows wherever it is touched). */
void mg_thread_whole_stack(void) { }

void mg_thread_stack_bounds(void **lo, void **hi)
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

u32 mg_tls_alloc(void)
{
    pthread_key_t k;
    if (pthread_key_create(&k, NULL) != 0) mg_fatal("out of thread-local slots");
    return (u32)k;
}
void *mg_tls_get(u32 key) { return pthread_getspecific((pthread_key_t)key); }
void mg_tls_set(u32 key, void *value) { pthread_setspecific((pthread_key_t)key, value); }

/* ------------------------------------------------- the guest thread pointer */

/* initial-exec: a fixed offset from the thread pointer in every thread, which is what the emitted load relies on. */
static __thread void *g_guest_tp __attribute__((tls_model("initial-exec")));

void mg_guest_tp_set(void *tp) { g_guest_tp = tp; }
void *mg_guest_tp_get(void) { return g_guest_tp; }

bool mg_guest_tp_where(u32 *opc2, u32 *offset)
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

static mg_fault_handler g_fault_handler;

static void on_signal(int sig, siginfo_t *si, void *ctx)
{
    mg_fault f;
    memset(&f, 0, sizeof(f));
    f.kind = sig == SIGILL ? MG_FAULT_ILLEGAL : (sig == SIGSEGV || sig == SIGBUS) ? MG_FAULT_ACCESS : MG_FAULT_OTHER;
    /* A trap (bkpt: a compiler's __builtin_trap, Chromium's CHECK) is given the number Windows gives a breakpoint, which the
       core's report names; the other signals keep their own. */
    f.code = sig == SIGTRAP ? 0x80000003u : (u32)sig;
    f.address = (mg_addr)si->si_addr;
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

void mg_fault_resume_at(mg_fault *f, mg_addr target)
{
    if (!f->regs) return;
    f->regs[15] = (u32)(target & ~(mg_addr)1);
    if (f->cpsr) *f->cpsr = (target & 1) ? (*f->cpsr | 0x20u) : (*f->cpsr & ~0x20u);
}

void mg_fault_set_handler(mg_fault_handler h)
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

u64 mg_time_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 mg_time_ticks(void) { return mg_time_monotonic_ns(); }
u64 mg_ticks_ns(u64 ticks) { return ticks; }

u64 mg_time_realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

i32 mg_time_utc_offset(i64 unix_seconds, bool *dst)
{
    time_t t = (time_t)unix_seconds;
    struct tm tm;
    localtime_r(&t, &tm);
    if (dst) *dst = tm.tm_isdst > 0;
    return (i32)tm.tm_gmtoff;
}

/* ------------------------------------------------------------------ files */

struct mg_file { int fd; };

void *mg_file_map(mg_file *f, u64 offset, size_t size, void **base, size_t *mapped)
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
    case 0: return MG_E_OK;
    case ENOENT: return MG_E_NOENT;
    case EACCES: case EPERM: case EROFS: return MG_E_ACCESS;
    case EEXIST: return MG_E_EXIST;
    case ENOTDIR: return MG_E_NOTDIR;
    case EISDIR: return MG_E_ISDIR;
    case ENOSPC: return MG_E_NOSPC;
    case EINVAL: return MG_E_INVAL;
    case ENOTEMPTY: return MG_E_NOTEMPTY;
    case EBUSY: return MG_E_BUSY;
    case EXDEV: return MG_E_XDEV;
    default: return MG_E_IO;
    }
}
#define SET_ERR() do { if (err) *err = err_of(errno); } while (0)

/* A foreign path's descriptor (mg_host.h: its owner hands over the descriptor plus one); -1 with *err set. */
static int foreign_fd(const char *path, int flags, int kind, int *err)
{
    void *h = mg_foreign_open(path, flags, kind, err);
    return h ? (int)(intptr_t)h - 1 : -1;
}

/* (Files by handle are Windows' alone: an app container's picked files.) */
bool mg_file_by_handle(const char *path, void *(*open)(void *user), void *user)
{
    (void)path; (void)open; (void)user;
    return false;
}

mg_file *mg_file_open(const char *path, int flags, int *err)
{
    int o = 0, fd;
    mg_file *f;
    if (mg_path_is_foreign(path)) {
        fd = foreign_fd(path, flags, MG_FOREIGN_FILE, err);
        if (fd < 0) return NULL;
    } else {
        if ((flags & MG_O_READ) && (flags & MG_O_WRITE)) o = O_RDWR;
        else if (flags & MG_O_WRITE) o = O_WRONLY;
        else o = O_RDONLY;
        if (flags & MG_O_CREATE) o |= O_CREAT;
        if (flags & MG_O_TRUNC) o |= O_TRUNC;
        if (flags & MG_O_APPEND) o |= O_APPEND;
        if (flags & MG_O_EXCL) o |= O_EXCL;
        fd = open(path, o | O_CLOEXEC, 0666);
        if (fd < 0) { SET_ERR(); return NULL; }
    }
    {
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
            close(fd);
            if (err) *err = MG_E_ISDIR;
            return NULL;
        }
    }
    f = (mg_file *)mg_alloc(sizeof(*f));
    f->fd = fd;
    if (err) *err = MG_E_OK;
    return f;
}

void mg_file_close(mg_file *f)
{
    if (!f) return;
    close(f->fd);
    free(f);
}

i64 mg_file_read(mg_file *f, void *buf, size_t n, int *err)
{
    ssize_t r;
    BEFORE_SYSTEM_WRITE(buf, n);
    do r = read(f->fd, buf, n); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 mg_file_write(mg_file *f, const void *buf, size_t n, int *err)
{
    ssize_t r;
    do r = write(f->fd, buf, n); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 mg_file_pread(mg_file *f, void *buf, size_t n, u64 offset, int *err)
{
    ssize_t r;
    BEFORE_SYSTEM_WRITE(buf, n);
    do r = pread(f->fd, buf, n, (off_t)offset); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 mg_file_pwrite(mg_file *f, const void *buf, size_t n, u64 offset, int *err)
{
    ssize_t r;
    do r = pwrite(f->fd, buf, n, (off_t)offset); while (r < 0 && errno == EINTR);
    if (r < 0) SET_ERR();
    return r;
}

i64 mg_file_seek(mg_file *f, i64 offset, int whence, int *err)
{
    off_t r = lseek(f->fd, (off_t)offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) SET_ERR();
    return (i64)r;
}

bool mg_file_truncate(mg_file *f, u64 size, int *err)
{
    if (ftruncate(f->fd, (off_t)size) != 0) { SET_ERR(); return false; }
    return true;
}

bool mg_file_sync(mg_file *f) { return fsync(f->fd) == 0; }

/*
 * Linux's locks of an open file description (F_OFD_SETLK): each open's own and binding every other, of this process too,
 * as Windows' handles' locks are -- and not let go when another descriptor of the file is closed, as POSIX's per-process
 * locks are. A kernel or emulator without them gets the per-process kind (F_SETLK), which binds other processes only.
 */
#ifndef F_OFD_SETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#endif
static int g_ofd = 1;                  /* 1 the OFD kind, 0 the per-process kind once the first was refused */

static int range_lock(int fd, int cmd_ofd, int cmd_process, struct flock *l)
{
    if (g_ofd) {
        l->l_pid = 0;
        if (fcntl(fd, cmd_ofd, l) == 0) return 0;
        if (errno != EINVAL) return -1;
        g_ofd = 0;
    }
    return fcntl(fd, cmd_process, l);
}

bool mg_file_lock(mg_file *f, u64 offset, u64 length, bool exclusive, int *err)
{
    struct flock l;
    memset(&l, 0, sizeof(l));
    l.l_type = exclusive ? F_WRLCK : F_RDLCK;
    l.l_whence = SEEK_SET;
    l.l_start = (off_t)offset;
    l.l_len = (off_t)length;
    if (range_lock(f->fd, F_OFD_SETLK, F_SETLK, &l) == 0) return true;
    if (errno == EAGAIN || errno == EACCES) {
        if (err) *err = MG_E_BUSY;
        return false;
    }
    /* (A file system without locks: as if granted.) */
    if (errno == ENOLCK || errno == EOPNOTSUPP || errno == EINVAL) return true;
    if (err) *err = errno == EBADF ? MG_E_ACCESS : MG_E_IO;
    return false;
}

void mg_file_unlock(mg_file *f, u64 offset, u64 length)
{
    struct flock l;
    memset(&l, 0, sizeof(l));
    l.l_type = F_UNLCK;
    l.l_whence = SEEK_SET;
    l.l_start = (off_t)offset;
    l.l_len = (off_t)length;
    range_lock(f->fd, F_OFD_SETLK, F_SETLK, &l);
}

bool mg_file_locked_elsewhere(mg_file *f, u64 offset, u64 length)
{
    struct flock l;
    memset(&l, 0, sizeof(l));
    l.l_type = F_WRLCK;
    l.l_whence = SEEK_SET;
    l.l_start = (off_t)offset;
    l.l_len = (off_t)length;
    if (range_lock(f->fd, F_OFD_GETLK, F_GETLK, &l) != 0) return false;
    return l.l_type != F_UNLCK;
}

static void fill_stat(const struct stat *s, mg_stat *st)
{
    st->is_dir = S_ISDIR(s->st_mode);
    st->size = (u64)s->st_size;
    st->mtime_ns = (i64)s->st_mtim.tv_sec * 1000000000ll + s->st_mtim.tv_nsec;
    st->atime_ns = (i64)s->st_atim.tv_sec * 1000000000ll + s->st_atim.tv_nsec;
    st->ctime_ns = (i64)s->st_ctim.tv_sec * 1000000000ll + s->st_ctim.tv_nsec;
    st->id = (u64)s->st_ino;
}

bool mg_file_stat(mg_file *f, mg_stat *st)
{
    struct stat s;
    if (fstat(f->fd, &s) != 0) return false;
    fill_stat(&s, st);
    return true;
}

bool mg_path_stat(const char *path, mg_stat *st, int *err)
{
    struct stat s;
    if (mg_path_is_foreign(path)) {
        int fd = foreign_fd(path, MG_O_READ, MG_FOREIGN_ANY, err), r;
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

bool mg_path_mkdir(const char *path, int *err)
{
    if (mg_path_is_foreign(path)) return mg_foreign_mkdir(path, err);
    if (mkdir(path, 0777) != 0) { SET_ERR(); return false; }
    return true;
}

bool mg_path_remove(const char *path, int *err)
{
    if (mg_path_is_foreign(path)) return mg_foreign_remove(path, false, err);
    if (unlink(path) != 0) { SET_ERR(); return false; }
    return true;
}

bool mg_path_rmdir(const char *path, int *err)
{
    if (mg_path_is_foreign(path)) return mg_foreign_remove(path, true, err);
    if (rmdir(path) != 0) { SET_ERR(); return false; }
    return true;
}

bool mg_path_rename(const char *from, const char *to, int *err)
{
    if (mg_path_is_foreign(from) || mg_path_is_foreign(to)) return mg_foreign_rename(from, to, err);
    if (rename(from, to) != 0) { SET_ERR(); return false; }
    return true;
}

bool mg_path_list(const char *path, mg_dir_cb cb, void *user, int *err)
{
    DIR *d;
    struct dirent *e;
    bool foreign = mg_path_is_foreign(path);
    if (foreign) {
        /* A foreign directory is read through the descriptor its owner hands over, which the DIR then owns. */
        int fd = foreign_fd(path, MG_O_READ, MG_FOREIGN_DIR, err);
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
            /* Its names were given to its owner as Windows takes them (mg_host.h); the app has them back as it named them. */
            char *name = mg_strdup(e->d_name);
            mg_name_from_host(name);
            cb(name, is_dir, user);
            free(name);
            continue;
        }
        cb(e->d_name, is_dir, user);
    }
    closedir(d);
    return true;
}

bool mg_map_open(mg_map *m, const char *path)
{
    struct stat s;
    int fd = mg_path_is_foreign(path) ? foreign_fd(path, MG_O_READ, MG_FOREIGN_FILE, NULL) : open(path, O_RDONLY | O_CLOEXEC);
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

bool mg_path_space(const char *path, u64 *total, u64 *available)
{
    struct statvfs s;
    if (mg_path_is_foreign(path) || statvfs(path, &s) != 0) return false;
    *total = (u64)s.f_blocks * s.f_frsize;
    *available = (u64)s.f_bavail * s.f_frsize;
    return true;
}

void mg_map_close(mg_map *m)
{
    if (m->data) munmap((void *)m->data, (size_t)m->size);
    memset(m, 0, sizeof(*m));
}

/* ---------------------------------------------------------------- sockets
 *
 * Non-blocking BSD sockets. One thread of the host's waits on all of them at
 * once, edge-triggered (epoll with EPOLLET): every change of any socket's
 * state wakes it, and it tells whoever mg_sock_set_notify named. What holds
 * at a given moment is poll()'s answer with no wait.
 */
struct mg_sock {
    int fd;
    int family, type;
};

static int g_watch_fd = -1;
static pthread_once_t g_watch_once = PTHREAD_ONCE_INIT;
static void (*volatile g_sock_notify)(void);
void mg_sock_set_notify(void (*changed)(void)) { g_sock_notify = changed; }

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
            mg_err("host: the wait on the sockets failed (errno %d): nobody waiting on a socket is told of a change any more", errno);
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
        mg_err("host: epoll_create1 failed (errno %d): nobody waiting on a socket is told of a change", errno);
        return;
    }
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, watch_main, NULL) != 0) mg_err("host: the thread that waits on the sockets could not be started");
    pthread_attr_destroy(&a);
}

static int net_err(int e)
{
    switch (e) {
    case 0: return MG_NE_OK;
    case EAGAIN: return MG_NE_AGAIN;
    case EINPROGRESS: return MG_NE_INPROGRESS;
    case EALREADY: return MG_NE_ALREADY;
    case EISCONN: return MG_NE_ISCONN;
    case ENOTCONN: return MG_NE_NOTCONN;
    case ECONNREFUSED: return MG_NE_CONNREFUSED;
    case ECONNRESET: return MG_NE_CONNRESET;
    case ECONNABORTED: return MG_NE_CONNABORTED;
    case ETIMEDOUT: return MG_NE_TIMEDOUT;
    case ENETUNREACH: return MG_NE_NETUNREACH;
    case EHOSTUNREACH: case EHOSTDOWN: return MG_NE_HOSTUNREACH;
    case ENETDOWN: return MG_NE_NETDOWN;
    case ENETRESET: return MG_NE_NETRESET;
    case EADDRINUSE: return MG_NE_ADDRINUSE;
    case EADDRNOTAVAIL: return MG_NE_ADDRNOTAVAIL;
    case EAFNOSUPPORT: case EPFNOSUPPORT: return MG_NE_AFNOSUPPORT;
    case EPROTONOSUPPORT: case EPROTOTYPE: return MG_NE_PROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return MG_NE_SOCKTNOSUPPORT;
    case EOPNOTSUPP: return MG_NE_OPNOTSUPP;
    case ENOPROTOOPT: return MG_NE_NOPROTOOPT;
    case EINVAL: return MG_NE_INVAL;
    case EACCES: case EPERM: return MG_NE_ACCES;
    case EMSGSIZE: return MG_NE_MSGSIZE;
    case ENOBUFS: case ENOMEM: return MG_NE_NOBUFS;
    case EMFILE: case ENFILE: return MG_NE_MFILE;
    case ESHUTDOWN: return MG_NE_SHUTDOWN;
    case EPIPE: return MG_NE_PIPE;
    case EDESTADDRREQ: return MG_NE_DESTADDRREQ;
    case EFAULT: return MG_NE_FAULT;
    default: return MG_NE_IO;
    }
}

static socklen_t addr_to_native(const mg_sockaddr *a, struct sockaddr_storage *ss)
{
    memset(ss, 0, sizeof(*ss));
    if (a->family == MG_AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)ss;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->addr, 4);
        return sizeof(*in);
    }
    if (a->family == MG_AF_INET6) {
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

static void addr_from_native(const struct sockaddr *sa, socklen_t len, mg_sockaddr *a)
{
    memset(a, 0, sizeof(*a));
    if (!sa) return;
    if (sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        a->family = MG_AF_INET;
        a->port = ntohs(in->sin_port);
        memcpy(a->addr, &in->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        a->family = MG_AF_INET6;
        a->port = ntohs(in6->sin6_port);
        a->flowinfo = in6->sin6_flowinfo;
        memcpy(a->addr, &in6->sin6_addr, 16);
        a->scope_id = in6->sin6_scope_id;
    }
}

static mg_sock *sock_wrap(int fd, int family, int type)
{
    mg_sock *k = (mg_sock *)mg_alloc(sizeof(*k));
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

mg_sock *mg_sock_open(int family, int type, int protocol, int *err)
{
    int af = family == MG_AF_INET ? AF_INET : family == MG_AF_INET6 ? AF_INET6 : 0;
    int st = type == MG_SOCK_STREAM ? SOCK_STREAM : type == MG_SOCK_DGRAM ? SOCK_DGRAM : type == MG_SOCK_RAW ? SOCK_RAW : 0, fd;
    if (!af || !st) {
        *err = !af ? MG_NE_AFNOSUPPORT : MG_NE_SOCKTNOSUPPORT;
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

void mg_sock_close(mg_sock *k)
{
    if (!k) return;
    if (g_watch_fd >= 0) epoll_ctl(g_watch_fd, EPOLL_CTL_DEL, k->fd, NULL);
    close(k->fd);
    free(k);
}

bool mg_sock_bind(mg_sock *k, const mg_sockaddr *a, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len = addr_to_native(a, &ss);
    if (!len) {
        *err = MG_NE_AFNOSUPPORT;
        return false;
    }
    if (bind(k->fd, (const struct sockaddr *)&ss, len) != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

bool mg_sock_listen(mg_sock *k, int backlog, int *err)
{
    if (listen(k->fd, backlog < 1 ? 1 : backlog) != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

mg_sock *mg_sock_accept(mg_sock *k, mg_sockaddr *peer, int *err)
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

bool mg_sock_connect(mg_sock *k, const mg_sockaddr *a, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len;
    if (!a->family) {
        memset(&ss, 0, sizeof(ss));
        ss.ss_family = AF_UNSPEC;
        len = sizeof(struct sockaddr);
    } else if (!(len = addr_to_native(a, &ss))) {
        *err = MG_NE_AFNOSUPPORT;
        return false;
    }
    if (connect(k->fd, (const struct sockaddr *)&ss, len) == 0) return true;
    *err = net_err(errno);
    return false;
}

static int native_flags(int flags)
{
    return ((flags & MG_MSG_PEEK) ? MSG_PEEK : 0) | ((flags & MG_MSG_OOB) ? MSG_OOB : 0) | ((flags & MG_MSG_DONTROUTE) ? MSG_DONTROUTE : 0);
}

i32 mg_sock_send(mg_sock *k, const void *buf, u32 n, int flags, const mg_sockaddr *to, int *err)
{
    struct sockaddr_storage ss;
    socklen_t len = 0;
    ssize_t r;
    if (to && !(len = addr_to_native(to, &ss))) {
        *err = MG_NE_AFNOSUPPORT;
        return -1;
    }
    r = sendto(k->fd, buf, n, native_flags(flags) | MSG_NOSIGNAL | MSG_DONTWAIT, to ? (const struct sockaddr *)&ss : NULL, len);
    if (r < 0) {
        *err = net_err(errno == EWOULDBLOCK ? EAGAIN : errno);
        return -1;
    }
    return (i32)r;
}

i32 mg_sock_recv(mg_sock *k, void *buf, u32 n, int flags, mg_sockaddr *from, int *said, int *err)
{
    struct sockaddr_storage ss;
    struct iovec iov;
    struct msghdr m;
    ssize_t r;
    memset(&m, 0, sizeof(m));
    memset(&ss, 0, sizeof(ss));
    BEFORE_SYSTEM_WRITE(buf, n);
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
    if (said && (m.msg_flags & MSG_TRUNC)) *said = MG_MSG_TRUNCATED;
    if (from) addr_from_native((const struct sockaddr *)&ss, m.msg_namelen, from);
    return (i32)r;
}

bool mg_sock_shutdown(mg_sock *k, int how, int *err)
{
    if (shutdown(k->fd, how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR) != 0) {
        *err = net_err(errno);
        return false;
    }
    return true;
}

bool mg_sock_name(mg_sock *k, bool peer, mg_sockaddr *a, int *err)
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

int mg_sock_ready(mg_sock *k)
{
    struct pollfd p;
    int out = 0;
    p.fd = k->fd;
    p.events = POLLIN | POLLOUT | POLLPRI | POLLRDHUP;
    p.revents = 0;
    if (poll(&p, 1, 0) <= 0) return 0;
    if (p.revents & (POLLIN | POLLPRI)) out |= MG_SOCK_IN;
    if (p.revents & POLLOUT) out |= MG_SOCK_OUT;
    if (p.revents & POLLERR) out |= MG_SOCK_ERR;
    if (p.revents & POLLHUP) out |= MG_SOCK_HUP;
    if (p.revents & POLLRDHUP) out |= MG_SOCK_RDHUP;
    return out;
}

int mg_sock_error(mg_sock *k)
{
    int e = 0;
    socklen_t len = sizeof(e);
    if (getsockopt(k->fd, SOL_SOCKET, SO_ERROR, &e, &len) != 0) return MG_NE_OK;
    return net_err(e);
}

i32 mg_sock_pending(mg_sock *k)
{
    int n = 0;
    return ioctl(k->fd, FIONREAD, &n) == 0 ? n : 0;
}

bool mg_sock_at_mark(mg_sock *k)
{
    int at = 0;
    return ioctl(k->fd, SIOCATMARK, &at) == 0 && at;
}

static int option_of(int option, int *level)
{
    *level = SOL_SOCKET;
    switch (option) {
    case MG_SO_REUSEADDR: return SO_REUSEADDR;
    case MG_SO_KEEPALIVE: return SO_KEEPALIVE;
    case MG_SO_BROADCAST: return SO_BROADCAST;
    case MG_SO_OOBINLINE: return SO_OOBINLINE;
    case MG_SO_RCVBUF: return SO_RCVBUF;
    case MG_SO_SNDBUF: return SO_SNDBUF;
    case MG_SO_DONTROUTE: return SO_DONTROUTE;
    case MG_SO_LINGER: return SO_LINGER;
    }
    *level = IPPROTO_TCP;
    switch (option) {
    case MG_TCP_NODELAY: return TCP_NODELAY;
    case MG_TCP_KEEPIDLE: return TCP_KEEPIDLE;
    case MG_TCP_KEEPINTVL: return TCP_KEEPINTVL;
    case MG_TCP_KEEPCNT: return TCP_KEEPCNT;
    case MG_TCP_MAXSEG: return TCP_MAXSEG;
    }
    *level = IPPROTO_IP;
    switch (option) {
    case MG_IP_TOS: return IP_TOS;
    case MG_IP_TTL: return IP_TTL;
    case MG_IP_MULTICAST_TTL: return IP_MULTICAST_TTL;
    case MG_IP_MULTICAST_LOOP: return IP_MULTICAST_LOOP;
    case MG_IP_MULTICAST_IF: return IP_MULTICAST_IF;
    }
    *level = IPPROTO_IPV6;
    switch (option) {
    case MG_IPV6_V6ONLY: return IPV6_V6ONLY;
    case MG_IPV6_UNICAST_HOPS: return IPV6_UNICAST_HOPS;
    case MG_IPV6_MULTICAST_HOPS: return IPV6_MULTICAST_HOPS;
    case MG_IPV6_MULTICAST_LOOP: return IPV6_MULTICAST_LOOP;
    case MG_IPV6_MULTICAST_IF: return IPV6_MULTICAST_IF;
    case MG_IPV6_TCLASS: return IPV6_TCLASS;
    }
    return -1;
}

bool mg_sock_set(mg_sock *k, int option, int value, int *err)
{
    int level, name = option_of(option, &level), r;
    if (name < 0) {
        *err = MG_NE_NOPROTOOPT;
        return false;
    }
    if (option == MG_SO_LINGER) {
        struct linger lg;
        lg.l_onoff = value >= 0;
        lg.l_linger = value < 0 ? 0 : value;
        r = setsockopt(k->fd, level, name, &lg, sizeof(lg));
    } else if (option == MG_IP_MULTICAST_IF) {
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

bool mg_sock_get(mg_sock *k, int option, int *value, int *err)
{
    int level, name = option_of(option, &level), v = 0;
    socklen_t len = sizeof(v);
    if (name < 0) {
        *err = MG_NE_NOPROTOOPT;
        return false;
    }
    if (option == MG_SO_LINGER) {
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

bool mg_sock_multicast(mg_sock *k, bool join, const mg_sockaddr *group, const mg_sockaddr *iface, u32 index, int *err)
{
    int r;
    if (group->family == MG_AF_INET) {
        struct ip_mreqn m;
        memset(&m, 0, sizeof(m));
        memcpy(&m.imr_multiaddr, group->addr, 4);
        if (iface && iface->family == MG_AF_INET) memcpy(&m.imr_address, iface->addr, 4);
        m.imr_ifindex = (int)index;
        r = setsockopt(k->fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof(m));
    } else if (group->family == MG_AF_INET6) {
        struct ipv6_mreq m;
        memset(&m, 0, sizeof(m));
        memcpy(&m.ipv6mr_multiaddr, group->addr, 16);
        m.ipv6mr_interface = index;
        r = setsockopt(k->fd, IPPROTO_IPV6, join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP, &m, sizeof(m));
    } else {
        *err = MG_NE_AFNOSUPPORT;
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
    case EAI_NONAME: return MG_EAI_NONAME;
    case EAI_AGAIN: return MG_EAI_AGAIN;
    case EAI_FAIL: return MG_EAI_FAIL;
#ifdef EAI_NODATA
    case EAI_NODATA: return MG_EAI_NODATA;
#endif
    case EAI_FAMILY: return MG_EAI_FAMILY;
    case EAI_MEMORY: return MG_EAI_MEMORY;
    case EAI_SYSTEM: return MG_EAI_SYSTEM;
    default: return MG_EAI_FAIL;
    }
}

int mg_net_resolve(const char *name, int family, int flags, mg_sockaddr **addrs, int *count, char **canonical)
{
    struct addrinfo hints, *res = NULL, *ai;
    mg_sockaddr *out = NULL;
    int e, got = 0, cap = 0, i;
    *addrs = NULL;
    *count = 0;
    if (canonical) *canonical = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family == MG_AF_INET ? AF_INET : family == MG_AF_INET6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = ((flags & MG_RESOLVE_CANONICAL) ? AI_CANONNAME : 0) | ((flags & MG_RESOLVE_ADDRCONFIG) ? AI_ADDRCONFIG : 0);
    e = getaddrinfo(name, NULL, &hints, &res);
    if (e) return eai_of(e);
    for (ai = res; ai; ai = ai->ai_next) {
        mg_sockaddr a;
        addr_from_native(ai->ai_addr, ai->ai_addrlen, &a);
        if (!a.family) continue;
        for (i = 0; i < got && memcmp(&out[i], &a, sizeof(a)); i++) { }
        if (i < got) continue;
        if (got == cap) {
            cap = cap ? cap * 2 : 8;
            out = (mg_sockaddr *)mg_realloc(out, sizeof(*out) * (size_t)cap);
        }
        out[got++] = a;
    }
    if (canonical && res && res->ai_canonname) *canonical = mg_strdup(res->ai_canonname);
    freeaddrinfo(res);
    if (!got) {
        free(out);
        if (canonical) {
            free(*canonical);
            *canonical = NULL;
        }
        return MG_EAI_NODATA;
    }
    *addrs = out;
    *count = got;
    return MG_EAI_OK;
}

int mg_net_name_of(const mg_sockaddr *a, char *name, size_t size)
{
    struct sockaddr_storage ss;
    socklen_t len = addr_to_native(a, &ss);
    int e;
    if (!len) return MG_EAI_FAMILY;
    e = getnameinfo((const struct sockaddr *)&ss, len, name, (socklen_t)size, NULL, 0, NI_NAMEREQD);
    return e ? eai_of(e) : MG_EAI_OK;
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

/* The name servers of /etc/resolv.conf, in its order, each once. */
int mg_net_dns_servers(char (*out)[48], int max)
{
    char line[256];
    int n = 0, i;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        char addr[64];
        unsigned char probe[16];
        bool seen = false;
        if (sscanf(line, " nameserver %63s", addr) != 1) continue;
        if (inet_pton(AF_INET, addr, probe) != 1 && inet_pton(AF_INET6, addr, probe) != 1) continue;
        for (i = 0; i < n && !seen; i++) seen = !strcmp(out[i], addr);
        if (!seen) mg_strlcpy(out[n++], addr, sizeof(out[0]));
    }
    fclose(f);
    return n;
}

int mg_net_interfaces(mg_netif *out, int max)
{
    struct ifaddrs *list = NULL, *ifa;
    char names[64][IF_NAMESIZE];
    int n = 0, i, probe;
    if (getifaddrs(&list) != 0) return -1;
    probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    for (ifa = list; ifa; ifa = ifa->ifa_next) {
        mg_netif *f = NULL;
        if (!ifa->ifa_name) continue;
        for (i = 0; i < n && i < (int)MG_COUNT(names); i++)
            if (!strcmp(names[i], ifa->ifa_name)) break;
        if (i < n && i < max) f = &out[i];
        if (i == n) {
            if (n < (int)MG_COUNT(names)) mg_strlcpy(names[n], ifa->ifa_name, IF_NAMESIZE);
            if (n < max) {
                struct ifreq rq;
                f = &out[n];
                memset(f, 0, sizeof(*f));
                f->index = if_nametoindex(ifa->ifa_name);
                if (ifa->ifa_flags & IFF_LOOPBACK) f->kind = MG_IF_LOOPBACK;
                else if (!strncmp(ifa->ifa_name, "wl", 2)) f->kind = MG_IF_WIFI;
                else if (!strncmp(ifa->ifa_name, "eth", 3) || !strncmp(ifa->ifa_name, "en", 2)) f->kind = MG_IF_ETHERNET;
                else if (!strncmp(ifa->ifa_name, "rmnet", 5) || !strncmp(ifa->ifa_name, "wwan", 4)) f->kind = MG_IF_CELLULAR;
                else if (!strncmp(ifa->ifa_name, "tun", 3) || !strncmp(ifa->ifa_name, "tap", 3) || !strncmp(ifa->ifa_name, "sit", 3)) f->kind = MG_IF_TUNNEL;
                else f->kind = MG_IF_OTHER;
                if (ifa->ifa_flags & IFF_UP) f->flags |= MG_IFF_UP;
                if (ifa->ifa_flags & IFF_RUNNING) f->flags |= MG_IFF_RUNNING;
                if (ifa->ifa_flags & IFF_MULTICAST) f->flags |= MG_IFF_MULTICAST;
                if (ifa->ifa_flags & IFF_BROADCAST) f->flags |= MG_IFF_BROADCAST;
                if (ifa->ifa_flags & IFF_POINTOPOINT) f->flags |= MG_IFF_POINTOPOINT;
                memset(&rq, 0, sizeof(rq));
                mg_strlcpy(rq.ifr_name, ifa->ifa_name, sizeof(rq.ifr_name));
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
        } else if (f->naddrs < MG_NETIF_ADDRS) {
            mg_sockaddr a;
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

void *mg_dl_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *mg_dl_sym(void *lib, const char *name) { return dlsym(lib, name); }

bool mg_host_module_at(mg_addr address, char *name, size_t size, mg_addr *base)
{
    Dl_info info;
    const char *file;
    if (!dladdr((void *)address, &info) || !info.dli_fname) return false;
    file = strrchr(info.dli_fname, '/');
    snprintf(name, size, "%s", file ? file + 1 : info.dli_fname);
    if (base) *base = (mg_addr)info.dli_fbase;
    return true;
}

/* ------------------------------------------------------------------- misc */

bool mg_host_address_space(u32 *free_mb, u32 *largest_free_mb, u32 *reserved_mb, u32 *committed_mb)
{
    (void)free_mb; (void)largest_free_mb; (void)reserved_mb; (void)committed_mb;
    return false;
}

u32 mg_thread_live_count(void) { return 0; }

bool mg_host_address_census(mg_host_address_kinds *k)
{
    (void)k;
    return false;
}

u64 mg_host_reserved_in(const void *base, size_t size)
{
    (void)base;
    (void)size;
    return 0;
}

bool mg_host_gpu_memory(void *device, u32 *budget_mb, u32 *used_mb)
{
    (void)device; (void)budget_mb; (void)used_mb;
    return false;
}

bool mg_host_keyed_mutex(void *mutex, bool acquire, u32 ms)
{
    (void)mutex; (void)acquire; (void)ms;
    return false;
}

bool mg_host_free_stretches(u32 *mb, int n)
{
    (void)mb; (void)n;
    return false;
}

u32 mg_host_d3d11_removed_reason(void *device)
{
    (void)device;
    return 0;
}

bool mg_host_d3d11_options(void *device, bool *constant_buffer_offsets, bool *constant_buffer_partial_updates)
{
    (void)device;
    *constant_buffer_offsets = *constant_buffer_partial_updates = false;
    return false;
}

int mg_host_d3d11_feature_level(void *device)
{
    (void)device;
    return 0;
}

void mg_random(void *buf, size_t n)
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

void mg_host_print(const char *line)
{
    fputs(line, stderr);
    fputc('\n', stderr);
}

const char *mg_host_name(void)
{
#if defined(__arm__)
    return "linux-arm";
#else
    return "linux";
#endif
}

#endif /* !_WIN32 */
