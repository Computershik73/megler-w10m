/*
 * pthreads for guest code.
 *
 * bionic's synchronisation objects are tiny and can be initialised statically:
 * a mutex is one 32-bit word, a condition one word, a semaphore one word.
 * There is no room in them for a host object and no moment at which to create
 * one, so they are implemented the way bionic does it -- state in the word
 * itself, waiting on the word's address (a futex; WaitOnAddress on Windows) --
 * with bionic's bit layout, so that PTHREAD_MUTEX_INITIALIZER,
 * PTHREAD_RECURSIVE_MUTEX_INITIALIZER and the rest mean what they mean there.
 *
 * Guest threads are host threads. Each has a guest thread pointer block
 * (ld/mg_tls.c) and an mg_gthread, which is also its pthread_t.
 */
#include "mg_bionic.h"

#include "../ld/mg_ld_internal.h"


#define G_EBUSY 16
#define G_ETIMEDOUT 110
#define G_EDEADLK 35
#define G_EPERM 1
#define G_EINVAL 22
#define G_EAGAIN 11
#define G_ESRCH 3

/* A struct timespec's time in nanoseconds (as unsigned: one before 1970 is far in the future, as it always was here). */
static u64 ns_of(const g_timespec *t) { return (u64)(gulong)t->tv_sec * 1000000000ull + (u64)(gulong)t->tv_nsec; }

/* How long from now until an absolute time on the realtime (or monotonic) clock; 0 when it has passed. */
static u64 until(const g_timespec *abs, bool monotonic)
{
    u64 now = monotonic ? mg_time_monotonic_ns() : mg_time_realtime_ns();
    u64 at = ns_of(abs);
    return at > now ? at - now : 0;
}

/* ------------------------------------------------------------------ mutex */

#define M_STATE 0x0003
#define M_COUNT 0x1FFC
#define M_COUNT_ONE 0x0004
#define M_SHARED 0x2000
#define M_TYPE 0xC000
#define M_RECURSIVE 0x4000
#define M_ERRORCHECK 0x8000
#define M_OWNER(v) ((i32)((u32)(v) >> 16))

/* Take the lock, with the owner field set to `owner` (0 for a normal mutex). timeout_ns: 0 forever, 1 do not wait. */
static int mutex_acquire(volatile i32 *m, i32 owner, u64 timeout_ns)
{
    u64 deadline = timeout_ns > 1 ? mg_time_monotonic_ns() + timeout_ns : 0;
    for (;;) {
        i32 v = *m, flags = v & (M_TYPE | M_SHARED);
        if (!(v & M_STATE)) {
            /* Free. A thread that had to wait marks the lock contended, since it cannot know it was the only waiter. */
            if (mg_atomic_cas(m, v, flags | (owner << 16) | 1) == v) return 0;
            continue;
        }
        if (timeout_ns == 1) return G_EBUSY;
        if ((v & M_STATE) == 1) {
            if (mg_atomic_cas(m, v, (v & ~M_STATE) | 2) != v) continue;
            v = (v & ~M_STATE) | 2;
        }
        if (deadline) {
            u64 now = mg_time_monotonic_ns();
            if (now >= deadline) return G_ETIMEDOUT;
            mg_gwait(m, v, deadline - now);
        } else {
            mg_gwait(m, v, 0);
        }
        /* Woken or changed: try for it, and leave it marked contended. */
        for (;;) {
            v = *m;
            if (v & M_STATE) break;
            if (mg_atomic_cas(m, v, (v & (M_TYPE | M_SHARED)) | (owner << 16) | 2) == v) return 0;
        }
    }
}

static void mutex_release(volatile i32 *m)
{
    for (;;) {
        i32 v = *m;
        if (mg_atomic_cas(m, v, v & (M_TYPE | M_SHARED)) == v) {
            if ((v & M_STATE) == 2) mg_futex_wake(m, false);
            return;
        }
    }
}

static int mutex_lock_timed(volatile i32 *m, u64 timeout_ns)
{
    i32 v = *m;
    i32 tid;
    if (!(v & M_TYPE)) return mutex_acquire(m, 0, timeout_ns);
    tid = mg_gthread_self()->tid;
    if ((v & M_STATE) && M_OWNER(v) == tid) {
        if ((v & M_TYPE) == M_ERRORCHECK) return timeout_ns == 1 ? G_EBUSY : G_EDEADLK;
        if ((v & M_COUNT) == M_COUNT) return G_EAGAIN;
        /* Only the owner changes the count, but waiters change the state bits beside it. */
        for (;;) {
            v = *m;
            if (mg_atomic_cas(m, v, v + M_COUNT_ONE) == v) return 0;
        }
    }
    return mutex_acquire(m, tid, timeout_ns);
}

static int g_mutex_lock(volatile i32 *m) { return m ? mutex_lock_timed(m, 0) : G_EINVAL; }
static int g_mutex_trylock(volatile i32 *m) { return m ? mutex_lock_timed(m, 1) : G_EINVAL; }

static int g_mutex_unlock(volatile i32 *m)
{
    i32 v;
    if (!m) return G_EINVAL;
    v = *m;
    if (v & M_TYPE) {
        if (!(v & M_STATE) || M_OWNER(v) != mg_gthread_self()->tid) return G_EPERM;
        if (v & M_COUNT) {
            for (;;) {
                v = *m;
                if (mg_atomic_cas(m, v, v - M_COUNT_ONE) == v) return 0;
            }
        }
    }
    mutex_release(m);
    return 0;
}

static int g_mutex_timedlock(volatile i32 *m, const g_timespec *abs)
{
    u64 left = until(abs, false);
    int r;
    /* mutex_lock_timed's 0 means forever and 1 means do not wait. */
    if (left > 1) return mutex_lock_timed(m, left);
    r = mutex_lock_timed(m, 1);
    return r == G_EBUSY ? G_ETIMEDOUT : r;
}

static int g_mutex_lock_timeout_np(volatile i32 *m, unsigned ms)
{
    int r = mutex_lock_timed(m, ms ? (u64)ms * 1000000ull : 1);
    return r == G_ETIMEDOUT ? G_EBUSY : r;
}

static int g_mutex_init(volatile i32 *m, const glong *attr)
{
    i32 v = 0;
    if (attr) {
        int type = *attr & 15;
        if (type == 1) v = M_RECURSIVE;
        else if (type == 2) v = M_ERRORCHECK;
        if (*attr & 0x10) v |= M_SHARED;
    }
    *m = v;
    return 0;
}

static int g_mutex_destroy(volatile i32 *m) { (void)m; return 0; }
/* The attributes are a long in bionic (pthread_mutexattr_t, pthread_condattr_t, pthread_rwlockattr_t), the type in its low bits. */
static int g_mutexattr_init(glong *a) { *a = 0; return 0; }
static int g_mutexattr_destroy(glong *a) { (void)a; return 0; }
static int g_mutexattr_settype(glong *a, int type)
{
    if (type < 0 || type > 2) return G_EINVAL;
    *a = (*a & ~15) | type;
    return 0;
}
static int g_mutexattr_gettype(const glong *a, int *type) { *type = (int)(*a & 15); return 0; }
static int g_mutexattr_setpshared(glong *a, int shared) { *a = shared ? (*a | 0x10) : (*a & ~0x10); return 0; }
static int g_mutexattr_getpshared(const glong *a, int *shared) { *shared = (*a & 0x10) ? 1 : 0; return 0; }

/* -------------------------------------------------------------- condition */

#define C_SHARED 1
#define C_MONOTONIC 2
#define C_ONE 4

static int cond_wait(volatile i32 *c, volatile i32 *m, u64 timeout_ns, bool timed)
{
    i32 seen = *c;
    bool ok = true;
    g_mutex_unlock(m);
    if (!timed) mg_gwait(c, seen, 0);
    else if (timeout_ns) ok = mg_gwait(c, seen, timeout_ns);
    else ok = false;
    g_mutex_lock(m);
    return ok ? 0 : G_ETIMEDOUT;
}

static int g_cond_wait(volatile i32 *c, volatile i32 *m) { return cond_wait(c, m, 0, false); }
static int g_cond_timedwait(volatile i32 *c, volatile i32 *m, const g_timespec *abs)
{
    if (!abs) return cond_wait(c, m, 0, false);
    return cond_wait(c, m, until(abs, (*c & C_MONOTONIC) != 0), true);
}
static int g_cond_timedwait_monotonic(volatile i32 *c, volatile i32 *m, const g_timespec *abs)
{
    return cond_wait(c, m, until(abs, true), true);
}
static int g_cond_timedwait_relative(volatile i32 *c, volatile i32 *m, const g_timespec *rel)
{
    return cond_wait(c, m, ns_of(rel), true);
}
static int g_cond_timeout_np(volatile i32 *c, volatile i32 *m, unsigned ms) { return cond_wait(c, m, (u64)ms * 1000000ull, true); }

static int g_cond_signal(volatile i32 *c)
{
    mg_atomic_add(c, C_ONE);
    mg_futex_wake(c, false);
    return 0;
}

static int g_cond_broadcast(volatile i32 *c)
{
    mg_atomic_add(c, C_ONE);
    mg_futex_wake(c, true);
    return 0;
}

static int g_cond_init(volatile i32 *c, const glong *attr) { *c = attr ? (i32)(*attr & (C_SHARED | C_MONOTONIC)) : 0; return 0; }
static int g_cond_destroy(volatile i32 *c) { (void)c; return 0; }
static int g_condattr_init(glong *a) { *a = 0; return 0; }
static int g_condattr_destroy(glong *a) { (void)a; return 0; }
static int g_condattr_setclock(glong *a, int clock)
{
    /* CLOCK_REALTIME 0, CLOCK_MONOTONIC 1 */
    if (clock != 0 && clock != 1) return G_EINVAL;
    *a = (*a & ~C_MONOTONIC) | (clock ? C_MONOTONIC : 0);
    return 0;
}
static int g_condattr_getclock(const glong *a, int *clock) { *clock = (*a & C_MONOTONIC) ? 1 : 0; return 0; }
static int g_condattr_setpshared(glong *a, int shared) { *a = shared ? (*a | C_SHARED) : (*a & ~C_SHARED); return 0; }
static int g_condattr_getpshared(const glong *a, int *shared) { *shared = (int)(*a & C_SHARED); return 0; }

/* -------------------------------------------------------------- semaphore */

/* The count is the word shifted left by one; bit 0 is bionic's "shared" flag. */
static int g_sem_init(volatile i32 *s, int shared, unsigned value)
{
    *s = (i32)((value << 1) | (shared ? 1u : 0u));
    return 0;
}

static int sem_take(volatile i32 *s, u64 timeout_ns, bool timed)
{
    u64 deadline = timed && timeout_ns > 1 ? mg_time_monotonic_ns() + timeout_ns : 0;
    for (;;) {
        i32 v = *s;
        if ((v >> 1) > 0) {
            if (mg_atomic_cas(s, v, v - 2) == v) return 0;
            continue;
        }
        if (timed) {
            u64 now;
            if (timeout_ns <= 1) {
                mg_set_errno(timeout_ns ? GE_AGAIN : GE_TIMEDOUT);
                return -1;
            }
            now = mg_time_monotonic_ns();
            if (now >= deadline) {
                mg_set_errno(GE_TIMEDOUT);
                return -1;
            }
            mg_gwait(s, v, deadline - now);
        } else {
            mg_gwait(s, v, 0);
        }
    }
}

static int g_sem_wait(volatile i32 *s) { return sem_take(s, 0, false); }
static int g_sem_trywait(volatile i32 *s) { return sem_take(s, 1, true); }
static int g_sem_timedwait(volatile i32 *s, const g_timespec *abs)
{
    u64 left = until(abs, false);
    return sem_take(s, left == 1 ? 2 : left, true);
}
static int g_sem_post(volatile i32 *s)
{
    mg_atomic_add(s, 2);
    mg_futex_wake(s, false);
    return 0;
}
static int g_sem_getvalue(volatile i32 *s, int *value) { *value = *s >> 1; return 0; }
static int g_sem_destroy(volatile i32 *s) { (void)s; return 0; }

/* ----------------------------------------------------------------- rwlock */

/*
 * pthread_rwlock_t is 40 bytes in 32-bit bionic (56 in 64-bit) and all zeros when static: room for a lock, a condition and
 * the counts.
 */
typedef struct g_rwlock {
    volatile i32 lock, cond;
    i32 readers, writer, pending_writers;
    i32 unused[5];
} g_rwlock;

static int rw_lock(g_rwlock *rw, bool write, u64 timeout_ns, bool timed)
{
    i32 tid = mg_gthread_self()->tid;
    int r = 0;
    g_mutex_lock(&rw->lock);
    if (write) {
        if (rw->writer == tid) {
            g_mutex_unlock(&rw->lock);
            return G_EDEADLK;
        }
        rw->pending_writers++;
        while (rw->readers || rw->writer) {
            if (timed && timeout_ns <= 1) { r = timeout_ns ? G_EBUSY : G_ETIMEDOUT; break; }
            if (cond_wait(&rw->cond, &rw->lock, timeout_ns, timed)) { r = G_ETIMEDOUT; break; }
        }
        rw->pending_writers--;
        if (!r) rw->writer = tid;
    } else {
        /* Writers first, or a steady stream of readers starves them. */
        while (rw->writer || rw->pending_writers) {
            if (timed && timeout_ns <= 1) { r = timeout_ns ? G_EBUSY : G_ETIMEDOUT; break; }
            if (cond_wait(&rw->cond, &rw->lock, timeout_ns, timed)) { r = G_ETIMEDOUT; break; }
        }
        if (!r) rw->readers++;
    }
    g_mutex_unlock(&rw->lock);
    return r;
}

static int g_rwlock_rdlock(g_rwlock *rw) { return rw_lock(rw, false, 0, false); }
static int g_rwlock_wrlock(g_rwlock *rw) { return rw_lock(rw, true, 0, false); }
static int g_rwlock_tryrdlock(g_rwlock *rw) { return rw_lock(rw, false, 1, true); }
static int g_rwlock_trywrlock(g_rwlock *rw) { return rw_lock(rw, true, 1, true); }
static int g_rwlock_timedrdlock(g_rwlock *rw, const g_timespec *abs)
{
    u64 left = until(abs, false);
    return rw_lock(rw, false, left == 1 ? 2 : left, true);
}
static int g_rwlock_timedwrlock(g_rwlock *rw, const g_timespec *abs)
{
    u64 left = until(abs, false);
    return rw_lock(rw, true, left == 1 ? 2 : left, true);
}

static int g_rwlock_unlock(g_rwlock *rw)
{
    g_mutex_lock(&rw->lock);
    if (rw->writer) rw->writer = 0;
    else if (rw->readers > 0) rw->readers--;
    g_mutex_unlock(&rw->lock);
    g_cond_broadcast(&rw->cond);
    return 0;
}

static int g_rwlock_init(g_rwlock *rw, const void *attr) { (void)attr; memset(rw, 0, sizeof(*rw)); return 0; }
static int g_rwlock_destroy(g_rwlock *rw) { (void)rw; return 0; }
static int g_attr_long_init(glong *a) { *a = 0; return 0; }
static int g_attr_long_nop(glong *a) { (void)a; return 0; }
static int g_attr_long_set(glong *a, int v) { (void)a; (void)v; return 0; }
static int g_attr_long_get(const glong *a, int *v) { (void)a; *v = 0; return 0; }

/* ------------------------------------------------------------------- once */

static int g_once(volatile i32 *once, mg_addr init)
{
    for (;;) {
        i32 v = *once;
        if (v == 2) return 0;
        if (v == 0) {
            if (mg_atomic_cas(once, 0, 1) != 0) continue;
            MG_GFN(void (*)(void), init)();
            mg_atomic_xchg(once, 2);
            mg_futex_wake(once, true);
            return 0;
        }
        mg_gwait(once, 1, 0);
    }
}

/* ------------------------------------------------------------------- keys */

typedef struct key_slot {
    volatile i32 used;
    u32 seq;               /* bumped on delete: a thread's old value under a reused key reads as NULL */
    mg_addr dtor;
} key_slot;
static key_slot g_keys[MG_PTHREAD_KEYS];

static int g_key_create(int *key, mg_addr dtor)
{
    int i;
    for (i = 0; i < MG_PTHREAD_KEYS; i++) {
        if (mg_atomic_cas(&g_keys[i].used, 0, 1) == 0) {
            g_keys[i].seq++;
            g_keys[i].dtor = dtor;
            *key = i;
            return 0;
        }
    }
    return G_EAGAIN;
}

static int g_key_delete(int key)
{
    if (key < 0 || key >= MG_PTHREAD_KEYS || !g_keys[key].used) return G_EINVAL;
    g_keys[key].dtor = 0;
    g_keys[key].seq++;
    g_keys[key].used = 0;
    return 0;
}

static void *g_getspecific(int key)
{
    mg_gthread *t;
    if (key < 0 || key >= MG_PTHREAD_KEYS) return NULL;
    t = mg_gthread_self();
    return t->key_seq[key] == g_keys[key].seq ? t->keys[key] : NULL;
}

static int g_setspecific(int key, void *value)
{
    mg_gthread *t;
    if (key < 0 || key >= MG_PTHREAD_KEYS || !g_keys[key].used) return G_EINVAL;
    t = mg_gthread_self();
    t->keys[key] = value;
    t->key_seq[key] = g_keys[key].seq;
    return 0;
}

static void run_key_destructors(mg_gthread *t)
{
    int round, i;
    for (round = 0; round < 4; round++) {
        bool any = false;
        for (i = 0; i < MG_PTHREAD_KEYS; i++) {
            void *v = t->keys[i];
            mg_addr dtor = g_keys[i].dtor;
            if (!v || !g_keys[i].used || t->key_seq[i] != g_keys[i].seq) continue;
            t->keys[i] = NULL;
            if (dtor) {
                MG_GFN(void (*)(void *), dtor)(v);
                any = true;
            }
        }
        if (!any) break;
    }
}

/* ---------------------------------------------------------------- threads */

/* bionic's pthread_attr_t, whose fields only these functions read. */
typedef struct g_attr {
    u32 flags;             /* 1: detached */
    void *stack_base;
    size_t stack_size, guard_size;
    i32 sched_policy, sched_priority;
#if MG_GUEST_LP64
    char reserved[16];
#endif
} g_attr;

/* bionic's __pthread_cleanup_t, which pthread_cleanup_push puts on the guest's stack. */
typedef struct g_cleanup {
    struct g_cleanup *prev;
    mg_addr routine;
    void *arg;
} g_cleanup;

static void thread_finish(mg_gthread *t)
{
    while (t->cleanup) {
        g_cleanup *c = (g_cleanup *)t->cleanup;
        t->cleanup = (void *)(mg_addr)c->prev;
        MG_GFN(void (*)(void *), c->routine)((void *)(mg_addr)c->arg);
    }
    run_key_destructors(t);
    mg_trace("thread %d has ended", (int)t->tid);
    mg_signals_thread_gone(t);
    t->sig_thread = NULL;
    mg_tls_thread_exit();
    mg_atomic_xchg(&t->exited, 1);
    mg_futex_wake(&t->exited, true);
    if (mg_atomic_xchg(&t->detached, 2) == 1) {
        /* Detached: nobody will join, so the thread's own exit is where its record goes. */
        mg_gthread_release_tid(t->tid);
        free(t);
    }
}

static MG_HOSTCB void thread_main(void *p)
{
    mg_gthread *t = (mg_gthread *)p;
    void **tp = mg_tls_thread_pointer();
    t->tp = tp;
    tp[1] = t;                                 /* (slot 2 is the app's: mg_gthread_self) */
    mg_thread_stack_bounds(&t->stack_lo, &t->stack_hi);
    t->sig_thread = mg_thread_current();
    /* The process's hardware breakpoints, if a library left as the file has it was loaded (arm/mg_patch.c). */
    mg_gthread_take_breakpoints(t);
    if (mg_app_threads_give_way()) mg_thread_runs_app_code();
    t->retval = MG_GFN(void *(*)(void *), t->start)(t->arg);
    thread_finish(t);
}

static int g_create(mg_gthread **out, const g_attr *attr, mg_addr start, void *arg)
{
    mg_gthread *t = (mg_gthread *)mg_alloc(sizeof(*t));
    size_t stack = attr && attr->stack_size ? attr->stack_size : 1024 * 1024;
    mg_trace("pthread_create(start %p, stack %u)", (void *)start, (unsigned)stack);
    t->start = start;
    t->arg = arg;
    t->detached = attr && (attr->flags & 1) ? 1 : 0;
    t->tid = mg_gthread_take_tid(t);
    /* Before the thread runs: pthread_create's caller may use the id at once, and so may the new thread. */
    *out = t;
    t->host = mg_thread_start(thread_main, t, stack, "guest");
    if (!t->host) {
        mg_gthread_release_tid(t->tid);
        free(t);
        *out = NULL;
        return G_EAGAIN;
    }
    mg_thread_detach(t->host);
    return 0;
}

static void g_exit(void *retval)
{
    mg_gthread *t = mg_gthread_self();
    t->retval = retval;
    thread_finish(t);
    mg_thread_exit();
}

static int g_join(mg_gthread *t, void **retval)
{
    if (!t) return G_ESRCH;
    if (t == mg_gthread_self()) return G_EDEADLK;
    if (t->detached == 1 || mg_atomic_xchg(&t->joined, 1)) return G_EINVAL;
    while (!t->exited) mg_gwait(&t->exited, 0, 0);
    /* The thread sets `detached` to 2 as its very last act; wait for that before freeing what it is still using. */
    while (t->detached != 2) mg_thread_yield();
    if (retval) *retval = t->retval;
    mg_gthread_release_tid(t->tid);
    free(t);
    return 0;
}

static int g_detach(mg_gthread *t)
{
    i32 was;
    if (!t) return G_ESRCH;
    was = mg_atomic_cas(&t->detached, 0, 1);
    if (was == 1) return G_EINVAL;
    if (was == 2) {
        /* Already finished, and not detached at the time: its record is ours to free. */
        mg_gthread_release_tid(t->tid);
        free(t);
    }
    return 0;
}

static mg_gthread *g_self(void) { return mg_gthread_self(); }
static int g_equal(mg_gthread *a, mg_gthread *b) { return a == b; }
static int g_gettid_np(mg_gthread *t) { return t ? t->tid : -1; }
static int g_gettid(void) { return mg_gthread_self()->tid; }

static int g_attr_init(g_attr *a)
{
    memset(a, 0, sizeof(*a));
    a->stack_size = 1024 * 1024;
    a->guard_size = MG_PAGE;
    return 0;
}
static int g_attr_destroy(g_attr *a) { (void)a; return 0; }
static int g_attr_setdetachstate(g_attr *a, int state) { a->flags = state ? (a->flags | 1) : (a->flags & ~1u); return 0; }
static int g_attr_getdetachstate(const g_attr *a, int *state) { *state = a->flags & 1; return 0; }
static int g_attr_setstacksize(g_attr *a, size_t size)
{
    if (size < 8192) return G_EINVAL;
    a->stack_size = size;
    return 0;
}
static int g_attr_getstacksize(const g_attr *a, size_t *size) { *size = a->stack_size; return 0; }
static int g_attr_setstack(g_attr *a, void *base, size_t size)
{
    /* The host makes the stack; an app that brings its own gets one of that size. */
    (void)base;
    a->stack_size = size;
    return 0;
}
static int g_attr_getstack(const g_attr *a, void **base, size_t *size)
{
    *base = a->stack_base;
    *size = a->stack_size;
    return 0;
}
static int g_attr_setguardsize(g_attr *a, size_t size) { a->guard_size = size; return 0; }
static int g_attr_getguardsize(const g_attr *a, size_t *size) { *size = a->guard_size; return 0; }
static int g_attr_setschedpolicy(g_attr *a, int policy) { a->sched_policy = policy; return 0; }
static int g_attr_getschedpolicy(const g_attr *a, int *policy) { *policy = a->sched_policy; return 0; }
static int g_attr_setschedparam(g_attr *a, const i32 *param) { a->sched_priority = *param; return 0; }
static int g_attr_getschedparam(const g_attr *a, i32 *param) { *param = a->sched_priority; return 0; }
static int g_attr_setscope(g_attr *a, int scope) { (void)a; (void)scope; return 0; }
static int g_attr_getscope(const g_attr *a, int *scope) { (void)a; *scope = 0; return 0; }
static int g_attr_setinheritsched(g_attr *a, int v) { (void)a; (void)v; return 0; }

/* Where a thread's stack is: garbage collectors scan it. */
static int g_getattr_np(mg_gthread *t, g_attr *a)
{
    g_attr_init(a);
    if (!t) return G_ESRCH;
    if (!t->stack_lo && t == mg_gthread_self()) mg_thread_stack_bounds(&t->stack_lo, &t->stack_hi);
    a->stack_base = t->stack_lo;
    a->stack_size = (size_t)((u8 *)t->stack_hi - (u8 *)t->stack_lo);
    if (t->detached == 1) a->flags |= 1;
    return 0;
}

static int g_setname_np(mg_gthread *t, const char *name)
{
    if (t && name) mg_strlcpy(t->name, name, sizeof(t->name));
    return 0;
}
static int g_getname_np(mg_gthread *t, char *buf, size_t n)
{
    if (!t) return G_ESRCH;
    mg_strlcpy(buf, t->name, n);
    return 0;
}
static int g_kill(mg_gthread *t, int sig) { return mg_signal_send(t, sig); }
static int g_sigmask(int how, const gulong *set, gulong *old) { return mg_signal_mask(how, set, old); }
static int g_setschedparam(mg_gthread *t, int policy, const i32 *param) { (void)t; (void)policy; (void)param; return 0; }
static int g_getschedparam(mg_gthread *t, int *policy, i32 *param)
{
    (void)t;
    if (policy) *policy = 0;
    if (param) *param = 0;
    return 0;
}
static int g_setschedprio(mg_gthread *t, int prio) { (void)t; (void)prio; return 0; }
static int g_atfork(mg_addr a, mg_addr b, mg_addr c) { (void)a; (void)b; (void)c; return 0; }
static int g_setcancelstate(int state, int *old) { (void)state; if (old) *old = 0; return 0; }
static int g_cancel(mg_gthread *t)
{
    (void)t;
    mg_note_once("pthread_cancel: threads are not cancelled here");
    return 0;
}
static void g_testcancel(void) { }

static void g_cleanup_push(g_cleanup *c, mg_addr routine, void *arg)
{
    mg_gthread *t = mg_gthread_self();
    c->prev = (g_cleanup *)t->cleanup;
    c->routine = routine;
    c->arg = arg;
    t->cleanup = c;
}

static void g_cleanup_pop(g_cleanup *c, int execute)
{
    mg_gthread *t = mg_gthread_self();
    t->cleanup = (void *)(mg_addr)c->prev;
    if (execute) MG_GFN(void (*)(void *), c->routine)((void *)(mg_addr)c->arg);
}

static int g_sched_yield(void) { mg_thread_yield(); return 0; }
static int g_sched_get_priority(int policy) { (void)policy; return 0; }
static int g_sched_getcpu(void) { return 0; }
static int g_sched_setaffinity(int pid, size_t n, const void *mask) { (void)pid; (void)n; (void)mask; return 0; }
static int g_sched_getaffinity(int pid, size_t n, u8 *mask)
{
    int cpus = mg_cpu_count(), i;
    (void)pid;
    memset(mask, 0, n);
    for (i = 0; i < cpus && (size_t)(i / 8) < n; i++) mask[i / 8] |= (u8)(1 << (i % 8));
    /* The raw system call returns the mask's size; the wrapper returns 0. */
    return 0;
}
/*
 * pthread_getcpuclockid: the clock of the thread's processor time, numbered as bionic numbers it -- the tid negated in the
 * high bits, CPUCLOCK_SCHED (2) and the per-thread bit (4) below -- which clock_gettime and clock_getres read back
 * (mg_libc_time.c).
 */
static int g_pthread_getcpuclockid(mg_gthread *t, i32 *clock)
{
    if (!t) return G_ESRCH;
    *clock = (i32)((~(u32)t->tid) << 3) | 2 | 4;
    return 0;
}

/* CPU_COUNT: the bits set in a cpu_set_t of `size` bytes. */
static int g_sched_cpucount(size_t size, const u8 *set)
{
    int n = 0;
    size_t i;
    for (i = 0; i < size; i++) {
        u8 b = set[i];
        for (; b; b &= (u8)(b - 1)) n++;
    }
    return n;
}
static int g_sched_setscheduler(int pid, int policy, const void *param) { (void)pid; (void)policy; (void)param; return 0; }
static int g_sched_setparam(int pid, const void *param) { (void)pid; (void)param; return 0; }
static int g_sched_getscheduler(int pid) { (void)pid; return 0; }
static int g_sched_getparam(int pid, i32 *param) { (void)pid; if (param) *param = 0; return 0; }

/* The futex system call, for the code that makes it directly (libc++'s atomic waits, some engines' own locks). */
long mg_g_futex(volatile i32 *addr, int op, int val, const g_timespec *timeout)
{
    switch (op & 0x7F) {
    case 0: /* FUTEX_WAIT */
    case 9: /* FUTEX_WAIT_BITSET: an absolute timeout */
        if (*addr != val) {
            mg_set_errno(GE_AGAIN);
            return -1;
        }
        if (timeout) {
            u64 ns = (op & 0x7F) == 9 ? until(timeout, !(op & 256)) : ns_of(timeout);
            if (!ns || !mg_gwait(addr, val, ns)) {
                mg_set_errno(GE_TIMEDOUT);
                return -1;
            }
        } else {
            mg_gwait(addr, val, 0);
        }
        return 0;
    case 1:  /* FUTEX_WAKE */
    case 10: /* FUTEX_WAKE_BITSET */
        mg_futex_wake(addr, val > 1);
        return val > 1 ? val : 1;
    default:
        mg_note_once("futex operation %d is not supported", op & 0x7F);
        mg_set_errno(GE_NOSYS);
        return -1;
    }
}

#define PT(n, f) MG_WRAP("pthread_" n, f)

const mg_sym mg_tab_pthread[] = {
    PT("mutex_lock", g_mutex_lock), PT("mutex_trylock", g_mutex_trylock), PT("mutex_unlock", g_mutex_unlock),
    PT("mutex_timedlock", g_mutex_timedlock), PT("mutex_lock_timeout_np", g_mutex_lock_timeout_np), PT("mutex_init", g_mutex_init),
    PT("mutex_destroy", g_mutex_destroy), PT("mutexattr_init", g_mutexattr_init), PT("mutexattr_destroy", g_mutexattr_destroy),
    PT("mutexattr_settype", g_mutexattr_settype), PT("mutexattr_gettype", g_mutexattr_gettype),
    PT("mutexattr_setpshared", g_mutexattr_setpshared), PT("mutexattr_getpshared", g_mutexattr_getpshared),
    PT("mutexattr_setprotocol", g_attr_long_set), PT("mutexattr_getprotocol", g_attr_long_get),
    PT("cond_wait", g_cond_wait), PT("cond_timedwait", g_cond_timedwait), PT("cond_timedwait_monotonic_np", g_cond_timedwait_monotonic),
    PT("cond_timedwait_monotonic", g_cond_timedwait_monotonic), PT("cond_timedwait_relative_np", g_cond_timedwait_relative),
    PT("cond_timeout_np", g_cond_timeout_np), PT("cond_signal", g_cond_signal), PT("cond_broadcast", g_cond_broadcast),
    PT("cond_init", g_cond_init), PT("cond_destroy", g_cond_destroy), PT("condattr_init", g_condattr_init),
    PT("condattr_destroy", g_condattr_destroy), PT("condattr_setclock", g_condattr_setclock), PT("condattr_getclock", g_condattr_getclock),
    PT("condattr_setpshared", g_condattr_setpshared), PT("condattr_getpshared", g_condattr_getpshared),
    MG_WRAP("sem_init", g_sem_init), MG_WRAP("sem_wait", g_sem_wait), MG_WRAP("sem_trywait", g_sem_trywait),
    MG_WRAP("sem_timedwait", g_sem_timedwait), MG_WRAP("sem_post", g_sem_post), MG_WRAP("sem_getvalue", g_sem_getvalue),
    MG_WRAP("sem_destroy", g_sem_destroy),
    PT("rwlock_rdlock", g_rwlock_rdlock), PT("rwlock_wrlock", g_rwlock_wrlock), PT("rwlock_tryrdlock", g_rwlock_tryrdlock),
    PT("rwlock_trywrlock", g_rwlock_trywrlock), PT("rwlock_timedrdlock", g_rwlock_timedrdlock),
    PT("rwlock_timedwrlock", g_rwlock_timedwrlock), PT("rwlock_unlock", g_rwlock_unlock), PT("rwlock_init", g_rwlock_init),
    PT("rwlock_destroy", g_rwlock_destroy), PT("rwlockattr_init", g_attr_long_init), PT("rwlockattr_destroy", g_attr_long_nop),
    PT("rwlockattr_setpshared", g_attr_long_set), PT("rwlockattr_getpshared", g_attr_long_get),
    PT("rwlockattr_setkind_np", g_attr_long_set), PT("rwlockattr_getkind_np", g_attr_long_get),
    PT("once", g_once), PT("key_create", g_key_create), PT("key_delete", g_key_delete), PT("getspecific", g_getspecific),
    PT("setspecific", g_setspecific),
    PT("create", g_create), PT("exit", g_exit), PT("join", g_join), PT("detach", g_detach), PT("self", g_self), PT("equal", g_equal),
    PT("gettid_np", g_gettid_np), PT("attr_init", g_attr_init), PT("attr_destroy", g_attr_destroy),
    PT("attr_setdetachstate", g_attr_setdetachstate), PT("attr_getdetachstate", g_attr_getdetachstate),
    PT("attr_setstacksize", g_attr_setstacksize), PT("attr_getstacksize", g_attr_getstacksize), PT("attr_setstack", g_attr_setstack),
    PT("attr_getstack", g_attr_getstack), PT("attr_setguardsize", g_attr_setguardsize), PT("attr_getguardsize", g_attr_getguardsize),
    PT("attr_setschedpolicy", g_attr_setschedpolicy), PT("attr_getschedpolicy", g_attr_getschedpolicy),
    PT("attr_setschedparam", g_attr_setschedparam), PT("attr_getschedparam", g_attr_getschedparam), PT("attr_setscope", g_attr_setscope),
    PT("attr_getscope", g_attr_getscope), PT("attr_setinheritsched", g_attr_setinheritsched), PT("getattr_np", g_getattr_np),
    PT("setname_np", g_setname_np), PT("getname_np", g_getname_np), PT("kill", g_kill), PT("sigmask", g_sigmask),
    PT("setschedparam", g_setschedparam), PT("getschedparam", g_getschedparam), PT("setschedprio", g_setschedprio),
    PT("atfork", g_atfork), PT("setcancelstate", g_setcancelstate), PT("setcanceltype", g_setcancelstate), PT("cancel", g_cancel),
    PT("testcancel", g_testcancel),
    MG_WRAP("__pthread_cleanup_push", g_cleanup_push), MG_WRAP("__pthread_cleanup_pop", g_cleanup_pop),
    MG_WRAP("gettid", g_gettid), MG_WRAP("__gettid", g_gettid),
    MG_WRAP("sched_yield", g_sched_yield), MG_WRAP("sched_get_priority_max", g_sched_get_priority),
    MG_WRAP("sched_get_priority_min", g_sched_get_priority), MG_WRAP("sched_getcpu", g_sched_getcpu),
    MG_WRAP("pthread_getcpuclockid", g_pthread_getcpuclockid), MG_WRAP("sched_setaffinity", g_sched_setaffinity), MG_WRAP("sched_getaffinity", g_sched_getaffinity), MG_WRAP("__sched_cpucount", g_sched_cpucount),
    MG_WRAP("sched_setscheduler", g_sched_setscheduler), MG_WRAP("sched_getscheduler", g_sched_getscheduler),
    MG_WRAP("sched_getparam", g_sched_getparam), MG_WRAP("sched_setparam", g_sched_setparam),
    MG_END
};
