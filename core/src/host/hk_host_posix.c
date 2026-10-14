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

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
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
};

static HK_THREAD_LOCAL hk_thread *t_current;

static void *thread_main(void *p)
{
    hk_thread *t = (hk_thread *)p;
    t_current = t;
    t->fn(t->arg);
    return NULL;
}

void hk_thread_runs_app_code(void) { }

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

void hk_thread_exit(void) { pthread_exit(NULL); }
u32 hk_thread_self_id(void) { return (u32)syscall(SYS_gettid); }
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
    f.code = (u32)sig;
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
}

/* ------------------------------------------------------------------- time */

u64 hk_time_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

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
    if (hk_path_is_foreign(path)) {
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

u32 hk_host_d3d11_removed_reason(void *device)
{
    (void)device;
    return 0;
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
