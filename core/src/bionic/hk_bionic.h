/*
 * Android's C library (bionic) and the other system libraries an app's native
 * code links against, implemented over the host.
 *
 * Guest code binds against names. For each name the table says what it gets:
 *
 *   HK_DIRECT  the host's own function, as it is. Right when the signature has
 *              no floating point, no structure whose layout differs, no
 *              constant whose value differs, no callback, and returns no
 *              memory the guest will free: strlen, memcpy, strtol.
 *   HK_WRAP    ours.
 *   HK_DATA    an object the guest reads or writes: __sF, _ctype_, environ.
 *
 * Nothing falls back to "whatever the host exports under that name".
 *
 * Calling convention, which decides how a wrapper is written (the long form is
 * in docs/ARCHITECTURE.md):
 *
 *   - Android's armeabi-v7a passes floating-point arguments and results in the
 *     integer registers (softfp); the host passes them in VFP registers. So a
 *     wrapper never has a float or double parameter or result. It takes and
 *     returns u32 / u64 -- the bits -- which is exactly how softfp passes a
 *     float / a double, and converts with hk_f / hk_d / hk_fb / hk_db.
 *   - Variadic functions take everything in integer registers and on the stack
 *     in both conventions, and a va_list is one pointer in both, so a host
 *     variadic function is called by the guest as it is, and a guest va_list
 *     is a host va_list.
 *   - Guest function pointers are called through HK_GFN (ld/hk_ld.h), with any
 *     float argument passed as its bits.
 */
#ifndef HK_BIONIC_H
#define HK_BIONIC_H

#include "../hk_base.h"
#include "../host/hk_host.h"
#include "../ld/hk_ld.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_sym {
    const char *name;
    void *addr;
} hk_sym;

#define HK_DIRECT(fn) { #fn, (void *)(fn) }
#define HK_WRAP(name, impl) { name, (void *)(impl) }
#define HK_DATA(name, ptr) { name, (void *)(ptr) }
#define HK_END { NULL, NULL }

/* Registered once at startup, by hk_bionic_init. */
void hk_bionic_init(void);
void hk_bionic_register(const hk_sym *table);
void *hk_bionic_find(const char *name);
bool hk_bionic_is_system_lib(const char *soname);

/* The tables, one per file; each ends with HK_END. */
extern const hk_sym hk_tab_mem[], hk_tab_str[], hk_tab_wchar[], hk_tab_stdio[], hk_tab_io[], hk_tab_misc[], hk_tab_time[], hk_tab_math[],
    hk_tab_pthread[], hk_tab_dl[], hk_tab_log[], hk_tab_aeabi[], hk_tab_net[], hk_tab_zlib[], hk_tab_ndk[], hk_tab_native_activity[];
const hk_sym *hk_opensles_table(void);
const hk_sym *hk_media_table(void);    /* ndk/hk_media.c */
/* The device's API level as native code is told it (the property ro.build.version.sdk, 28 unless a driver has said otherwise). */
int hk_sdk_level(void);
const hk_sym *hk_java_ndk_table(void); /* drivers/hk_java_ndk.c: libjnigraphics, on the Java framework's Bitmap */

/* Float wrappers: the host function named, taking and returning bits. */
#define HK_SF_D_D(name, fn) static u64 name(u64 x) { return hk_db(fn(hk_d(x))); }
#define HK_SF_D_DD(name, fn) static u64 name(u64 x, u64 y) { return hk_db(fn(hk_d(x), hk_d(y))); }
#define HK_SF_F_F(name, fn) static u32 name(u32 x) { return hk_fb(fn(hk_f(x))); }
#define HK_SF_F_FF(name, fn) static u32 name(u32 x, u32 y) { return hk_fb(fn(hk_f(x), hk_f(y))); }

/* ----------------------------------------------------------------- errno
 *
 * The guest's errno is its own per-thread integer holding Linux numbers.
 */
enum {
    GE_PERM = 1, GE_NOENT = 2, GE_SRCH = 3, GE_INTR = 4, GE_IO = 5, GE_NXIO = 6, GE_2BIG = 7, GE_NOEXEC = 8, GE_BADF = 9, GE_CHILD = 10,
    GE_AGAIN = 11, GE_NOMEM = 12, GE_ACCES = 13, GE_FAULT = 14, GE_BUSY = 16, GE_EXIST = 17, GE_XDEV = 18, GE_NODEV = 19, GE_NOTDIR = 20,
    GE_ISDIR = 21, GE_INVAL = 22, GE_NFILE = 23, GE_MFILE = 24, GE_NOTTY = 25, GE_FBIG = 27, GE_NOSPC = 28, GE_SPIPE = 29, GE_ROFS = 30,
    GE_PIPE = 32, GE_DOM = 33, GE_RANGE = 34, GE_DEADLK = 35, GE_NAMETOOLONG = 36, GE_NOSYS = 38, GE_NOTEMPTY = 39, GE_ILSEQ = 84,
    GE_OVERFLOW = 75, GE_NOTSOCK = 88, GE_OPNOTSUPP = 95, GE_AFNOSUPPORT = 97, GE_ADDRINUSE = 98, GE_NETUNREACH = 101, GE_CONNABORTED = 103,
    GE_CONNRESET = 104, GE_NOBUFS = 105, GE_ISCONN = 106, GE_NOTCONN = 107, GE_TIMEDOUT = 110, GE_CONNREFUSED = 111, GE_HOSTUNREACH = 113,
    GE_ALREADY = 114, GE_INPROGRESS = 115
};

int *hk_errno_ptr(void);
void hk_set_errno(int guest_errno);
/* A host file-layer error (HK_E_*) as a Linux errno. */
int hk_errno_of(int host_err);

/* ----------------------------------------------------------- guest threads */

#define HK_PTHREAD_KEYS 160

typedef struct hk_gthread {
    void **tp;                         /* the guest thread pointer block */
    i32 tid;                           /* small and unique among live threads: what gettid() returns */
    int gerrno;
    void *keys[HK_PTHREAD_KEYS];
    u32 key_seq[HK_PTHREAD_KEYS];
    hk_addr start;
    void *arg, *retval;
    volatile i32 exited;               /* a futex word: joiners wait on it */
    volatile i32 detached;
    volatile i32 joined;
    hk_thread *host;
    char name[32];
    void *stack_lo, *stack_hi;
    void *cleanup;                     /* __pthread_cleanup_push's chain */
    void *altstack_sp;
    u32 altstack_size;
    /* Signals sent to the thread (hk_libc_misc.c, "signals"). */
    volatile i32 sig_pending;          /* bit n-1: signal n is waiting for its handler */
    u32 sig_blocked;                   /* the thread's signal mask */
    /* The same for the real-time signals, 33 to 64 (bit n-33), which a 32-bit sigset_t has no room for: libcore sends 34 to a thread blocked on a socket that is being closed. */
    volatile i32 sig_pending_rt;
    u32 sig_blocked_rt;
    volatile i32 sig_park;             /* goes up with every signal sent to it: what sigsuspend and long sleeps wait on */
    volatile i32 sig_handled;          /* how many handlers have run on it */
    volatile i32 sig_unrestarted;      /* how many of them did not ask for SA_RESTART: a wait for a socket they cut short ends with EINTR */
    volatile i32 *volatile sig_wait_addr;  /* the word the thread is waiting on, while it is */
    hk_cond *volatile sig_wait_cond;   /* or the condition */
    hk_thread *volatile sig_thread;    /* the thread itself, to interrupt it in the middle of guest code */
    struct hk_gthread *next;
} hk_gthread;

/* The calling thread's guest state, made on first use (a host thread that walks into guest code is adopted). */
hk_gthread *hk_gthread_self(void);

/*
 * Signals between the guest's threads: pthread_kill and the handlers it
 * reaches. A garbage collector stops the world with these, so a game with
 * managed code (Unity's, Mono's) does not get past its first collection
 * without them.
 *
 * A handler runs on the thread the signal was sent to. That thread is either
 * in the middle of guest code, where it is interrupted (hk_thread_interrupt),
 * or waiting in one of this layer's own waits, which are cut short for it
 * (hk_gwait, hk_gsleep, and the descriptor waits of hk_libc_io.c), or
 * somewhere else in this layer or the system, where it is left alone and
 * asked again a millisecond later.
 */
int hk_signal_send(hk_gthread *t, int sig);            /* pthread_kill: 0 or an errno; signals 1 to 64 */
int hk_signal_mask(int how, const u32 *set, u32 *old); /* pthread_sigmask */
/* Runs the handlers of the signals waiting for the calling thread. Only where no lock of this layer is held. */
void hk_signals_check(void);
static inline bool hk_signals_ready(const hk_gthread *t)
{
    return ((u32)t->sig_pending & ~t->sig_blocked) != 0 || ((u32)t->sig_pending_rt & ~t->sig_blocked_rt) != 0;
}
/* hk_futex_wait for a guest thread: a signal for it ends the wait, and its handler has run by the time this returns. */
bool hk_gwait(volatile i32 *addr, i32 expect, u64 timeout_ns);
/* hk_sleep_ns likewise; the handlers run, and the sleep goes on for the time that is left. */
void hk_gsleep(u64 ns);
/* Whether the app's threads give way to the shell's (hk_thread_runs_app_code): off with the flag "normal-priority". */
bool hk_app_threads_give_way(void);
/* The thread is ending: nothing more is sent its way. */
void hk_signals_thread_gone(hk_gthread *t);

/* The allocator the guest's malloc family is; anything a shim returns for the guest to free comes from it. */
void *hk_g_malloc(size_t n);
void *hk_g_calloc(size_t n, size_t m);
void *hk_g_realloc(void *p, size_t n);
void hk_g_free(void *p);
char *hk_g_strdup(const char *s);

/* --------------------------------------------------------------- the file system
 *
 * Guest paths are Android's; these say where they are on the host. Both
 * before an app's code runs (run/hk_run.c calls them, in this order).
 *
 * hk_fs_configure: the app's own folders. `internal_dir` is its
 * /data/data/<package>. `external_dir` is where its own part of the shared
 * storage is kept: /storage/emulated/0/Android/data/<package> is
 * <external_dir>/Android/data/<package>, and Android/obb/<package> likewise.
 *
 * hk_fs_shared_storage: the storage every app shares, /storage/emulated/0
 * (which /sdcard, /mnt/sdcard, /storage/self/primary and /storage/sdcard0
 * are other names of). `shared_dir` is its folder on the host, where
 * Android's standard folders are made (Music, Pictures, Download...). A link
 * takes the place of the folder of its name there: /storage/emulated/0/<name>
 * is then the foreign path of `root` (host/hk_host.h), one of the phone's own
 * folders. A name is one part of a path. Links of an earlier call are gone.
 */
typedef struct hk_fs_link {
    const char *name;
    int root;
    bool read_only;                    /* access(W_OK) says no; the writes themselves are refused by whoever owns the root */
} hk_fs_link;
void hk_fs_configure(const char *package, const char *internal_dir, const char *external_dir);
void hk_fs_shared_storage(const char *shared_dir, const hk_fs_link *links, int count);
/* The host path of a guest path, malloc'd; NULL for a path that has no place (it then does not exist). */
char *hk_fs_host_path(const char *guest_path);
const char *hk_fs_internal_dir(void);  /* guest paths */
const char *hk_fs_external_dir(void);
const char *hk_fs_package(void);

/* Writes what an app wrote into shared file mappings back to their files (hk_libc_mem.c); at the app's end. */
void hk_mem_write_back(void);

/* Android system properties, for __system_property_get. */
const char *hk_sysprop(const char *name);
void hk_sysprop_set(const char *name, const char *value);

/*
 * The phone's time zone as tzdata names it ("Europe/Moscow"), which the shell knows and says (megler_set_timezone); "" until
 * it has, and after NULL or "". The property persist.sys.timezone says the same, "UTC" while there is none: it is where
 * Android's own code looks (ICU's default zone, an app's __system_property_get). The Java runtime asks hk_timezone_id for
 * libcore's default zone (dvm/hk_dvm_art.c). Set before an app runs: both read it once. The C library's localtime does not
 * read it; it has the host's offset for each instant (hk_time_utc_offset).
 */
void hk_timezone_set(const char *id);
const char *hk_timezone_id(void);

/* exit() from guest code: the driver decides what it means (the default ends the process). */
extern void (*hk_guest_exit_hook)(int status);

#ifdef __cplusplus
}
#endif

#endif /* HK_BIONIC_H */
