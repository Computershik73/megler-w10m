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
 *     returns gfloat / gdouble (mg_base.h) -- on 32-bit ARM the bits, u32 / u64,
 *     which is exactly how softfp passes a float / a double; on desktop Windows,
 *     where the core is of the guest's ABI (docs/DESKTOP.md), the values -- and
 *     converts with mg_gf / mg_gd / mg_gfb / mg_gdb.
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

/*
 * bionic's own integer types, as wide as the guest's: `long` is 32 bits in 32-bit Android and 64 in 64-bit Android, and
 * every build of the core that runs guest code has its guest's `long` -- MSVC and gcc for 32-bit ARM, gcc for Linux on
 * the others, and the core's ELF on desktop Windows (docs/DESKTOP.md), compiled for the guest's ABI. A field, parameter
 * or result that bionic declares long, ssize_t, off_t or time_t is one of these; one that holds a pointer is a pointer or
 * a uintptr_t, never a u32; a float or a double is a gfloat or a gdouble (mg_base.h).
 */
typedef long glong;
typedef unsigned long gulong;
typedef long gssize;
typedef long goff;
typedef long gtime;

/* struct timespec and struct timeval as guest code has them: a time_t and a long. */
typedef struct g_timespec {
    gtime tv_sec;
    glong tv_nsec;
} g_timespec;
typedef struct g_timeval {
    gtime tv_sec;
    glong tv_usec;
} g_timeval;
/*
 * The largest count read, write and their kin move at once: Linux's MAX_RW_COUNT, which it gives a caller asking for more
 * (the rest is a short read or write, as POSIX allows). It keeps the descriptors' layer's counts 31 bits wide where size_t
 * is 64.
 */
#define MG_RW_MAX 0x7FFFF000u
static inline u32 mg_rw_count(size_t n) { return n > MG_RW_MAX ? MG_RW_MAX : (u32)n; }

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
    mg_tab_pthread[], mg_tab_dl[], mg_tab_log[], mg_tab_aeabi[], mg_tab_net[], mg_tab_netdb[], mg_tab_zlib[], mg_tab_ndk[], mg_tab_native_activity[], mg_tab_regex[];
const mg_sym *mg_opensles_table(void);
const mg_sym *mg_media_table(void);    /* ndk/mg_media.c */
const mg_sym *mg_aaudio_table(void);   /* ndk/mg_aaudio.c */
const mg_sym *mg_ahb_table(void);      /* ndk/mg_ahb.c: AHardwareBuffer */
const mg_sym *mg_sensors_table(void);  /* ndk/mg_sensors.c: ASensorManager and its queues */
/* The device's API level as native code is told it (the property ro.build.version.sdk, 28 unless a driver has said otherwise). */
int mg_sdk_level(void);
const mg_sym *mg_java_ndk_table(void); /* drivers/mg_java_ndk.c: libjnigraphics, on the Java framework's Bitmap; ANativeWindow_fromSurface */

/*
 * A long double as guest code passes it: a double's bits on 32-bit ARM, where it is a double (softfp); the value
 * elsewhere -- a double on x86 too, IEEE's 128-bit quad on the 64-bit ABIs, which the core's compiler has as it is
 * (docs/DESKTOP.md). mg_gld takes one as a double, mg_gldb makes one of a double: the *l functions here compute in double.
 */
#if MG_GUEST_SOFTFP
typedef gdouble gldouble;
#define mg_gld(x) mg_gd(x)
#define mg_gldb(d) mg_gdb(d)
#else
typedef long double gldouble;
#define mg_gld(x) ((double)(x))
#define mg_gldb(d) ((gldouble)(d))
#endif

/* Float wrappers: the host function named, taking and returning floats as the guest passes them (bits on 32-bit ARM). */
#define MG_SF_D_D(name, fn) static gdouble name(gdouble x) { return mg_gdb(fn(mg_gd(x))); }
#define MG_SF_D_DD(name, fn) static gdouble name(gdouble x, gdouble y) { return mg_gdb(fn(mg_gd(x), mg_gd(y))); }
#define MG_SF_F_F(name, fn) static gfloat name(gfloat x) { return mg_gfb(fn(mg_gf(x))); }
#define MG_SF_F_FF(name, fn) static gfloat name(gfloat x, gfloat y) { return mg_gfb(fn(mg_gf(x), mg_gf(y))); }
/* And the long double ones, computed in double. */
#define MG_SF_L_L(name, fn) static gldouble name(gldouble x) { return mg_gldb(fn(mg_gld(x))); }
#define MG_SF_L_LL(name, fn) static gldouble name(gldouble x, gldouble y) { return mg_gldb(fn(mg_gld(x), mg_gld(y))); }

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
/* argv as the kernel lays it out for a process, envp (empty) and the auxiliary vector after it: what a library's init
   functions are given (mg_libc_misc.c). */
char **mg_bionic_initial_argv(void);
/* Its auxiliary vector as /proc/self/auxv has it: tags and values as longs, AT_NULL's pair the last; the number of words. */
size_t mg_bionic_auxv_words(gulong *out, size_t max);
/* ARM EABI kernel convention: negative errno in r0, guest errno unchanged. The svc's place is regs[15]. */
u32 mg_bionic_syscall_raw(const u32 *regs);
#if MG_GUEST_CODE && !MG_ARCH_ARM32
/* bionic/mg_syscall_abi.c: the guest's call `nr` and its arguments as 32-bit ARM's, -1 for one not handled, -2 for arguments
   ARM's cannot take. mg_bionic_syscall: a system call by the guest's own number, -errno on failure (ARM64's svc traps,
   x86_64's syscalls led out); `site` the guest's code that made it (the svc, the piece of island). */
long mg_syscall_abi_to_arm(long nr, long a[6]);
long mg_bionic_syscall(long nr, const long args[6], mg_addr site);
#endif
/* Where in the guest's code the system call this thread is making by number was made (the two above), 0 when none is. */
mg_addr mg_bionic_syscall_site(void);
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
    mg_cond io_cond;                   /* what its waits for descriptors wait on (mg_libc_io.c, "who waits for what") */
    bool io_cond_ready;
    mg_thread *volatile sig_thread;    /* the thread itself, to interrupt it in the middle of guest code */
    /* While a guarded call of guest code runs on the thread (mg_jni_call_native_guarded): the stub setjmp's buffer it goes
       back to when that code faults beyond repair (mg_core.c), instead of the app ending. */
    mg_addr *volatile fault_guard;
    /* How many of the host's hardware breakpoints the thread has been given (mg_gthread_take_breakpoints). */
    volatile i32 breakpoints;
    /* Its Android nice value, -20 (most urgent) to 19, as setpriority or Process.setThreadPriority set it (mg_gthread_set_nice). */
    volatile i32 nice;
    struct mg_gthread *next;
} mg_gthread;

/* The calling thread's guest state, made on first use (a host thread that walks into guest code is adopted). */
mg_gthread *mg_gthread_self(void);
/* A thread id for `t`, which is the live thread of that id until mg_gthread_release_tid. */
i32 mg_gthread_take_tid(mg_gthread *t);
void mg_gthread_release_tid(i32 tid);
/* The host thread of the live guest thread whose gettid() is `tid`, or NULL (its processor time, mg_libc_time.c). */
mg_thread *mg_gthread_host_of_tid(i32 tid);
/* The host's hardware breakpoints given to the caller and every live guest thread (a library left as the file has it). */
void mg_gthreads_arm_breakpoints(void);
/*
 * A guest thread's Android nice value, and its host thread's priority after it (mg_bionic.c says how): what setpriority
 * and Process.setThreadPriority set. `t` the caller's own or another's (one whose host thread is gone is only noted).
 */
void mg_gthread_set_nice(mg_gthread *t, int nice);
/* The thread of the caller feeds an audio track: it has an audio thread's priority from now on, unless it asked for more. */
void mg_gthread_feeds_audio(void);
/*
 * The host's hardware breakpoints given to the calling guest thread, `t`, if it has fewer than the host has now: where a
 * thread starts and where it calls native code from Java, so that one the loader could not give them to (stopped, it would
 * not take them) has them before it runs such a library's code. A comparison when it has them.
 */
static inline void mg_gthread_take_breakpoints(mg_gthread *t)
{
    int n = mg_breakpoint_count();
    if (t->breakpoints != n && mg_breakpoints_arm(NULL)) t->breakpoints = n;
}

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
int mg_signal_mask(int how, const gulong *set, gulong *old); /* pthread_sigmask: a sigset_t, a long */
/* Runs the handlers of the signals waiting for the calling thread. Only where no lock of this layer is held. */
MG_HOSTCB void mg_signals_check(void);
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
/* What the guest has asked malloc, calloc, realloc and memalign for and not freed, in bytes. */
u64 mg_g_malloc_bytes(void);
/* The addresses the app's mappings (mmap) span, and the bytes of them reserved and not committed. */
void mg_g_mapped_addresses(u64 *spanned, u64 *reserved);
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
/* The same, waiting for each mapping's turn, for the mappings of up to `largest` bytes (0: all): every few seconds. */
void mg_mem_write_back_all(size_t largest);
/* All of them, waiting for the lock no later than `deadline` (monotonic ns), false when it gave up: as the app is suspended. */
bool mg_mem_write_back_all_until(u64 deadline);
/* A look at who holds the mappings' lock (mg_holder_look), for the hang report: false when nobody does. */
bool mg_mem_lock_look(mg_hold_seen *seen);
/*
 * Whether any copy of a file is mapped shared; and what was written to a file (a held one, mg_fd_hold) through a descriptor,
 * `n` bytes at `at`, copied into every such mapping of it, as the kernel's mappings of a file show what is written to it.
 */
bool mg_mem_file_copies(void);
void mg_mem_file_written(void *file, u64 at, const void *buf, size_t n);
/* What madvise has given back to the host so far, and how much of it was touched (committed) again since. */
void mg_mmap_given_back(u64 *given, u64 *again);
/* What the app's mappings hold now, as far as they are its own (mapped, not made no-access, not given back): anonymous
   memory, and the copies of files mapped privately. Views of shared memory are not in it. */
void mg_mmap_usage(u64 *anon, u64 *files);
/* Go's runtime's reservations up front refused on a 32-bit host (mg_libc_mem.c, go_up_front_refused): how many, and their bytes. */
void mg_mmap_go_refused(u32 *count, u64 *bytes);
/*
 * Whose the app's private mapping that starts at `base` is, said for the census of the addresses: "Go's runtime in <library>",
 * "<library>'s mappings", "the app's mappings"; NULL when no mapping of the app's starts there.
 */
const char *mg_mmap_whose(const void *base);

/* Android system properties, for __system_property_get. */
int mg_sysprop_foreach(void (*visit)(const void *, void *), void *cookie);
const void *mg_sysprop_find_nth(unsigned index);
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
