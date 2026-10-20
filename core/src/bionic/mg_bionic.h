/*
 * Android's C library (bionic) and the other system libraries an app's native
 * code links against, implemented over the host.
 *
 * Guest code binds against names. For each name the table says what it gets:
 *
 *   MG_DIRECT  the host's own function, as it is. Right when the signature has
 *              no floating point, no structure whose layout differs, no
 *              constant whose value differs, no callback, and returns no
 *              memory the guest will free: strlen, memcpy, strtol.
 *   MG_WRAP    ours.
 *   MG_DATA    an object the guest reads or writes: __sF, _ctype_, environ.
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
 *     float / a double, and converts with mg_f / mg_d / mg_fb / mg_db.
 *   - Variadic functions take everything in integer registers and on the stack
 *     in both conventions, and a va_list is one pointer in both, so a host
 *     variadic function is called by the guest as it is, and a guest va_list
 *     is a host va_list.
 *   - Guest function pointers are called through MG_GFN (ld/mg_ld.h), with any
 *     float argument passed as its bits.
 */
#ifndef MG_BIONIC_H
#define MG_BIONIC_H

#include "../mg_base.h"
#include "../host/mg_host.h"
#include "../ld/mg_ld.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mg_sym {
    const char *name;
    void *addr;
} mg_sym;

#define MG_DIRECT(fn) { #fn, (void *)(fn) }
#define MG_WRAP(name, impl) { name, (void *)(impl) }
#define MG_DATA(name, ptr) { name, (void *)(ptr) }
#define MG_END { NULL, NULL }

/* Registered once at startup, by mg_bionic_init. */
void mg_bionic_init(void);
void mg_bionic_register(const mg_sym *table);
void *mg_g_mmap2(void *addr, size_t len, int prot, int flags, int fd, u32 pgoffset);
void *mg_bionic_find(const char *name);
bool mg_bionic_is_system_lib(const char *soname);

/* The tables, one per file; each ends with MG_END. */
extern const mg_sym mg_tab_mem[], mg_tab_str[], mg_tab_wchar[], mg_tab_stdio[], mg_tab_io[], mg_tab_misc[], mg_tab_time[], mg_tab_math[],
    mg_tab_pthread[], mg_tab_dl[], mg_tab_log[], mg_tab_aeabi[], mg_tab_net[], mg_tab_netdb[], mg_tab_zlib[], mg_tab_ndk[], mg_tab_native_activity[];
const mg_sym *mg_opensles_table(void);
const mg_sym *mg_media_table(void);    /* ndk/mg_media.c */
const mg_sym *mg_aaudio_table(void);   /* ndk/mg_aaudio.c */
const mg_sym *mg_ahb_table(void);      /* ndk/mg_ahb.c: AHardwareBuffer */
/* The device's API level as native code is told it (the property ro.build.version.sdk, 28 unless a driver has said otherwise). */
int mg_sdk_level(void);
const mg_sym *mg_java_ndk_table(void); /* drivers/mg_java_ndk.c: libjnigraphics, on the Java framework's Bitmap; ANativeWindow_fromSurface */

/* Float wrappers: the host function named, taking and returning bits. */
#define MG_SF_D_D(name, fn) static u64 name(u64 x) { return mg_db(fn(mg_d(x))); }
#define MG_SF_D_DD(name, fn) static u64 name(u64 x, u64 y) { return mg_db(fn(mg_d(x), mg_d(y))); }
#define MG_SF_F_F(name, fn) static u32 name(u32 x) { return mg_fb(fn(mg_f(x))); }
#define MG_SF_F_FF(name, fn) static u32 name(u32 x, u32 y) { return mg_fb(fn(mg_f(x), mg_f(y))); }

/* ----------------------------------------------------------------- errno
 *
 * The guest's errno is its own per-thread integer holding Linux numbers.
 */
enum {
    GE_PERM = 1, GE_NOENT = 2, GE_SRCH = 3, GE_INTR = 4, GE_IO = 5, GE_NXIO = 6, GE_2BIG = 7, GE_NOEXEC = 8, GE_BADF = 9, GE_CHILD = 10,
    GE_AGAIN = 11, GE_NOMEM = 12, GE_ACCES = 13, GE_FAULT = 14, GE_BUSY = 16, GE_EXIST = 17, GE_XDEV = 18, GE_NODEV = 19, GE_NOTDIR = 20,
    GE_ISDIR = 21, GE_INVAL = 22, GE_NFILE = 23, GE_MFILE = 24, GE_NOTTY = 25, GE_FBIG = 27, GE_NOSPC = 28, GE_SPIPE = 29, GE_ROFS = 30,
    GE_PIPE = 32, GE_DOM = 33, GE_RANGE = 34, GE_DEADLK = 35, GE_NAMETOOLONG = 36, GE_NOSYS = 38, GE_NOTEMPTY = 39, GE_ILSEQ = 84,
    GE_OVERFLOW = 75, GE_NOTSOCK = 88, GE_DESTADDRREQ = 89, GE_MSGSIZE = 90, GE_PROTOTYPE = 91, GE_NOPROTOOPT = 92, GE_PROTONOSUPPORT = 93,
    GE_SOCKTNOSUPPORT = 94, GE_OPNOTSUPP = 95, GE_PFNOSUPPORT = 96, GE_AFNOSUPPORT = 97, GE_ADDRINUSE = 98, GE_ADDRNOTAVAIL = 99,
    GE_NETDOWN = 100, GE_NETUNREACH = 101, GE_NETRESET = 102, GE_CONNABORTED = 103, GE_CONNRESET = 104, GE_NOBUFS = 105, GE_ISCONN = 106,
    GE_NOTCONN = 107, GE_SHUTDOWN = 108, GE_TIMEDOUT = 110, GE_CONNREFUSED = 111, GE_HOSTDOWN = 112, GE_HOSTUNREACH = 113, GE_ALREADY = 114,
    GE_INPROGRESS = 115
};

int *mg_errno_ptr(void);
/* ARM EABI kernel convention: negative errno in r0, guest errno unchanged. */
u32 mg_bionic_syscall_raw(const u32 *regs);
void mg_set_errno(int guest_errno);
/* A host file-layer error (MG_E_*) as a Linux errno. */
int mg_errno_of(int host_err);

/* ----------------------------------------------------------- guest threads */

#define MG_PTHREAD_KEYS 160

typedef struct mg_gthread {
    void **tp;                         /* the guest thread pointer block */
    i32 tid;                           /* small and unique among live threads: what gettid() returns */
    int gerrno;
    void *keys[MG_PTHREAD_KEYS];
    u32 key_seq[MG_PTHREAD_KEYS];
    mg_addr start;
    void *arg, *retval;
    volatile i32 exited;               /* a futex word: joiners wait on it */
    volatile i32 detached;
    volatile i32 joined;
    mg_thread *host;
    char name[32];
    void *stack_lo, *stack_hi;
    void *cleanup;                     /* __pthread_cleanup_push's chain */
    void *altstack_sp;
    u32 altstack_size;
    /* Signals sent to the thread (mg_libc_misc.c, "signals"). */
    volatile i32 sig_pending;          /* bit n-1: signal n is waiting for its handler */
    u32 sig_blocked;                   /* the thread's signal mask */
    /* The same for the real-time signals, 33 to 64 (bit n-33), which a 32-bit sigset_t has no room for: libcore sends 34 to a thread blocked on a socket that is being closed. */
    volatile i32 sig_pending_rt;
    u32 sig_blocked_rt;
    volatile i32 sig_park;             /* goes up with every signal sent to it: what sigsuspend and long sleeps wait on */
    volatile i32 sig_handled;          /* how many handlers have run on it */
    volatile i32 sig_unrestarted;      /* how many of them did not ask for SA_RESTART: a wait for a socket they cut short ends with EINTR */
    volatile i32 *volatile sig_wait_addr;  /* the word the thread is waiting on, while it is */
    mg_cond *volatile sig_wait_cond;   /* or the condition */
    mg_thread *volatile sig_thread;    /* the thread itself, to interrupt it in the middle of guest code */
    struct mg_gthread *next;
} mg_gthread;

/* The calling thread's guest state, made on first use (a host thread that walks into guest code is adopted). */
mg_gthread *mg_gthread_self(void);
/* A thread id for `t`, which is the live thread of that id until mg_gthread_release_tid. */
i32 mg_gthread_take_tid(mg_gthread *t);
void mg_gthread_release_tid(i32 tid);
/* The host thread of the live guest thread whose gettid() is `tid`, or NULL (its processor time, mg_libc_time.c). */
mg_thread *mg_gthread_host_of_tid(i32 tid);

/*
 * Signals between the guest's threads: pthread_kill and the handlers it
 * reaches. A garbage collector stops the world with these, so a game with
 * managed code (Unity's, Mono's) does not get past its first collection
 * without them.
 *
 * A handler runs on the thread the signal was sent to. That thread is either
 * in the middle of guest code, where it is interrupted (mg_thread_interrupt),
 * or waiting in one of this layer's own waits, which are cut short for it
 * (mg_gwait, mg_gsleep, and the descriptor waits of mg_libc_io.c), or
 * somewhere else in this layer or the system, where it is left alone and
 * asked again a millisecond later.
 */
int mg_signal_send(mg_gthread *t, int sig);            /* pthread_kill: 0 or an errno; signals 1 to 64 */
int mg_signal_mask(int how, const u32 *set, u32 *old); /* pthread_sigmask */
/* Runs the handlers of the signals waiting for the calling thread. Only where no lock of this layer is held. */
void mg_signals_check(void);
/* A fault's signal for the app's own handler (an inline hook's trap, mg_mmap_app_trap): true if one ran and f says where to go on. */
bool mg_signal_fault(int sig, mg_fault *f);
static inline bool mg_signals_ready(const mg_gthread *t)
{
    return ((u32)t->sig_pending & ~t->sig_blocked) != 0 || ((u32)t->sig_pending_rt & ~t->sig_blocked_rt) != 0;
}
/* mg_futex_wait for a guest thread: a signal for it ends the wait, and its handler has run by the time this returns. */
bool mg_gwait(volatile i32 *addr, i32 expect, u64 timeout_ns);
/* mg_sleep_ns likewise; the handlers run, and the sleep goes on for the time that is left. */
void mg_gsleep(u64 ns);
/* Whether the app's threads give way to the shell's (mg_thread_runs_app_code): off with the flag "normal-priority". */
bool mg_app_threads_give_way(void);
/* The flag trace-cpu: a thread that says every five seconds how much of the processor the app had, and its busiest threads. */
void mg_bionic_cpu_trace_start(void);
/* The hang report, in every run (Windows): when the log's or the guest threads' lock is held ten seconds, who holds it and where every thread is (LocalState\hang.txt and the log). */
void mg_bionic_hang_watch_start(void);
/* The flag trace-native-stacks: a thread that says every ten seconds where each of the app's threads is (pc, lr, its stack's way back). */
void mg_bionic_native_stacks_start(void);
/* The flag trace-profile: the busiest threads sampled sixty times a second, by module and address (LocalState\profile.txt). */
void mg_bionic_profile_start(void);
/* The thread is ending: nothing more is sent its way. */
void mg_signals_thread_gone(mg_gthread *t);

/* The allocator the guest's malloc family is; anything a shim returns for the guest to free comes from it. */
void *mg_g_malloc(size_t n);
void *mg_g_calloc(size_t n, size_t m);
void *mg_g_realloc(void *p, size_t n);
void mg_g_free(void *p);
char *mg_g_strdup(const char *s);

/* --------------------------------------------------------------- the file system
 *
 * Guest paths are Android's; these say where they are on the host. Both
 * before an app's code runs (run/mg_run.c calls them, in this order).
 *
 * mg_fs_configure: the app's own folders. `internal_dir` is its
 * /data/data/<package>. `external_dir` is where its own part of the shared
 * storage is kept: /storage/emulated/0/Android/data/<package> is
 * <external_dir>/Android/data/<package>, and Android/obb/<package> likewise.
 *
 * mg_fs_shared_storage: the storage every app shares, /storage/emulated/0
 * (which /sdcard, /mnt/sdcard, /storage/self/primary and /storage/sdcard0
 * are other names of). `shared_dir` is its folder on the host, where
 * Android's standard folders are made (Music, Pictures, Download...). A link
 * takes the place of the folder of its name there: /storage/emulated/0/<name>
 * is then the foreign path of `root` (host/mg_host.h), one of the phone's own
 * folders. A name is one part of a path. Links of an earlier call are gone.
 *
 * mg_fs_other_app: another app's own part of the shared storage, kept in its
 * own folder as mg_fs_configure keeps this app's (`external_dir` is that
 * app's): its Android/data/<package> and Android/obb/<package> are there for
 * this app too, those of them that exist, as Android 9 lets an app with the
 * storage permission reach them. Nothing for this app's own package.
 */
typedef struct mg_fs_link {
    const char *name;
    int root;
    bool read_only;                    /* access(W_OK) says no; the writes themselves are refused by whoever owns the root */
} mg_fs_link;
void mg_fs_configure(const char *package, const char *internal_dir, const char *external_dir);
void mg_fs_shared_storage(const char *shared_dir, const mg_fs_link *links, int count);
void mg_fs_other_app(const char *package, const char *external_dir);
/* The host path of a guest path, malloc'd; NULL for a path that has no place (it then does not exist). */
char *mg_fs_host_path(const char *guest_path);
const char *mg_fs_internal_dir(void);  /* guest paths */
const char *mg_fs_external_dir(void);
const char *mg_fs_package(void);

/* Writes what an app wrote into shared file mappings back to their files (mg_libc_mem.c); at the app's end. */
void mg_mem_write_back(void);

/* Android system properties, for __system_property_get. */
const char *mg_sysprop(const char *name);
void mg_sysprop_set(const char *name, const char *value);

/*
 * The phone's time zone as tzdata names it ("Europe/Moscow"), which the shell knows and says (megler_set_timezone); "" until
 * it has, and after NULL or "". The property persist.sys.timezone says the same, "UTC" while there is none: it is where
 * Android's own code looks (ICU's default zone, an app's __system_property_get). The Java runtime asks mg_timezone_id for
 * libcore's default zone (dvm/mg_dvm_art.c). Set before an app runs: both read it once. The C library's localtime does not
 * read it; it has the host's offset for each instant (mg_time_utc_offset).
 */
void mg_timezone_set(const char *id);
const char *mg_timezone_id(void);
/*
 * The phone's language and region (megler_set_locale), a BCP 47 tag: "en-US" until one is set, and in place of one that does
 * not begin with a language (two or three letters). The property persist.sys.locale says it. mg_locale_parts gives its
 * language, lower case, and its region, upper case ("" when it names none): "zh-Hant-TW" is "zh" and "TW".
 */
void mg_locale_set(const char *tag);
const char *mg_locale_tag(void);
void mg_locale_parts(char *language, size_t language_size, char *region, size_t region_size);

/* exit() from guest code: the driver decides what it means (the default ends the process). */
extern void (*mg_guest_exit_hook)(int status);

#ifdef __cplusplus
}
#endif

#endif /* MG_BIONIC_H */
