/*
 * Everything the core asks of the operating system, in one place.
 *
 * Two implementations: hk_host_win32.c (the Windows 10 Mobile app container, and
 * desktop Windows for the x64 build of the tests) and hk_host_posix.c (Linux,
 * where the ARM32 build runs under qemu-arm and the tests execute guest code).
 * Nothing else in the core includes an OS header.
 *
 * Paths are UTF-8 with forward slashes.
 */
#ifndef HK_HOST_H
#define HK_HOST_H

#include "../hk_base.h"

#if !defined(_WIN32)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- memory
 *
 * Windows 10 Mobile lets a package with the codeGeneration capability turn
 * committed read-write pages into execute-read ones and back
 * (VirtualProtectFromApp). It never allows an executable commit or a page that
 * is writable and executable at once. So everything here is written with a
 * page writable and run with it executable: a library's text is copied in,
 * patched, and then flipped, once.
 */
enum { HK_PROT_NONE = 0, HK_PROT_R = 1, HK_PROT_RW = 3, HK_PROT_RX = 5 };

#define HK_PAGE 4096u

/* Address space only, a multiple of 64 KB (Windows' allocation granularity). NULL on failure. */
void *hk_mem_reserve(size_t size);
/* Back reserved pages with zeroed memory: HK_PROT_R or HK_PROT_RW. */
bool hk_mem_commit(void *p, size_t size, int prot);
bool hk_mem_protect(void *p, size_t size, int prot);
bool hk_mem_decommit(void *p, size_t size);
void hk_mem_release(void *p, size_t size);
/* After writing code and before running it. */
void hk_icache_flush(void *p, size_t size);
/* Reserve and commit read-write in one step (anonymous mmap). */
void *hk_mem_alloc(size_t size);

/* What the app may use before the OS ends it, and what it uses now; 0 when unknown. */
u64 hk_mem_limit(void);
u64 hk_mem_usage(void);
void hk_mem_set_probe(u64 (*limit)(void), u64 (*usage)(void));

/* --------------------------------------------------------------- threads */

typedef struct hk_thread hk_thread;
typedef void (*hk_thread_fn)(void *arg);

hk_thread *hk_thread_start(hk_thread_fn fn, void *arg, size_t stack_size, const char *name);
void hk_thread_join(hk_thread *t);     /* frees t */
void hk_thread_detach(hk_thread *t);   /* frees t */
/* Ends the calling thread where it stands. */
HK_NORETURN void hk_thread_exit(void);
u32 hk_thread_self_id(void);
/*
 * The calling thread runs the app's code from here on: it gives way to the
 * threads of the shell (its UI thread above all) whenever they have something
 * to do, and otherwise runs as it would.
 */
void hk_thread_runs_app_code(void);
/* The calling thread, however it was started; lives as long as the process. */
hk_thread *hk_thread_current(void);
/*
 * Interrupting a thread in the middle of guest code, which is what a signal
 * sent to it does on Android. `fn` is then called on that thread, and the
 * thread goes on where it was with every register as it was.
 * hk_thread_interrupt only does this to a thread that is executing code for
 * which `in_guest_code` says yes at that instant: a thread inside this layer
 * or the system may hold a lock that `fn` needs. It returns false when the
 * thread was elsewhere, and the caller asks again a little later.
 * `in_guest_code` is called while the other thread is stopped: it takes no
 * lock.
 */
void hk_thread_set_interrupt(void (*fn)(void), bool (*in_guest_code)(hk_addr pc));
bool hk_thread_interrupt(hk_thread *t);
/* What has become of the interruptions asked for so far, in a line: for the tests and the log. */
void hk_thread_interrupt_report(char *out, size_t size);
/*
 * Stopping another thread where it is, for the collector (dvm/): hk_thread_suspend stops `t`, whatever it is executing, and
 * reads its registers; it stays stopped until hk_thread_resume. Never the calling thread. The registers are only read: the
 * context is never written back (no SetThreadContext), so the thread goes on exactly where it was, in the state it was in.
 * On Windows SuspendThread then GetThreadContext, which is how Windows says to wait until the thread has stopped; elsewhere
 * a signal of the host's own whose handler saves the registers and waits. False when the thread could not be stopped (it
 * has ended); it is then not stopped.
 */
typedef struct hk_thread_regs {
    hk_addr sp, pc;
    u32 flags;                         /* CPSR on ARM: bit 5 is the Thumb state */
    int n;
    hk_addr r[20];                     /* the general registers that may hold anything, sp and pc not among them */
} hk_thread_regs;
bool hk_thread_suspend(hk_thread *t, hk_thread_regs *out);
void hk_thread_resume(hk_thread *t);
/* Called on a thread started by hk_thread_start as it ends (its function returned, or hk_thread_exit): the collector forgets it. */
void hk_thread_set_end_hook(void (*fn)(void));
void hk_thread_yield(void);
void hk_sleep_ns(u64 ns);
int hk_cpu_count(void);

#if defined(_WIN32)
typedef struct hk_lock { void *srw; } hk_lock;        /* SRWLOCK */
typedef struct hk_cond { void *cv; } hk_cond;         /* CONDITION_VARIABLE */
#define HK_LOCK_INIT { 0 }
#define HK_COND_INIT { 0 }
#else
typedef struct hk_lock { pthread_mutex_t m; } hk_lock;
typedef struct hk_cond { pthread_cond_t c; bool monotonic; } hk_cond;
#define HK_LOCK_INIT { PTHREAD_MUTEX_INITIALIZER }
#define HK_COND_INIT { PTHREAD_COND_INITIALIZER, false }
#endif

void hk_lock_init(hk_lock *l);
void hk_lock_acquire(hk_lock *l);
bool hk_lock_try(hk_lock *l);
void hk_lock_release(hk_lock *l);
void hk_cond_init(hk_cond *c);
void hk_cond_wait(hk_cond *c, hk_lock *l);
/* Returns false when the time ran out. */
bool hk_cond_wait_ns(hk_cond *c, hk_lock *l, u64 ns);
void hk_cond_signal(hk_cond *c);
void hk_cond_broadcast(hk_cond *c);

i32 hk_atomic_cas(volatile i32 *p, i32 expect, i32 value);   /* returns what was there */
i32 hk_atomic_add(volatile i32 *p, i32 delta);                /* returns the new value */
i32 hk_atomic_xchg(volatile i32 *p, i32 value);
void *hk_atomic_cas_ptr(void *volatile *p, void *expect, void *value);
/* The same for 64 bits, which a 32-bit processor does with a pair of exclusive instructions; `p` is aligned to 8 bytes. */
i64 hk_atomic_cas64(volatile i64 *p, i64 expect, i64 value);

/*
 * Waiting on a word of memory (a futex; WaitOnAddress on Windows): what the
 * guest's mutexes, conditions and semaphores are built from, since bionic's
 * are four bytes each with no room for a host object.
 * hk_futex_wait returns when *addr != expect, on a wake, or spuriously; false
 * only when `timeout_ns` (0: none) ran out.
 */
bool hk_futex_wait(volatile i32 *addr, i32 expect, u64 timeout_ns);
void hk_futex_wake(volatile i32 *addr, bool all);
/* The calling thread's stack: [lo, hi). */
void hk_thread_stack_bounds(void **lo, void **hi);
/*
 * Makes all of the calling thread's stack usable at once, for a thread about
 * to run Android code. Windows hands a stack out a page at a time, to code
 * that feels its way down (every frame bigger than a page probes each page
 * in turn, and touching the guard page below the used part is what brings
 * the next one). Android code does not: a function with a 300 KB array just
 * moves sp and writes. That lands beyond the guard page, in memory not yet
 * given, and the fault cannot even be delivered, since the stack it would be
 * delivered on is the one that is missing: Windows ends the process with no
 * handler run and nothing said. (Seen with Unity on a phone: a texture
 * decoder on the graphics thread, 0.7 s into the first scene.)
 */
void hk_thread_whole_stack(void);

/* Thread-local slots of the host's own. */
u32 hk_tls_alloc(void);
void *hk_tls_get(u32 key);
void hk_tls_set(u32 key, void *value);

/* ------------------------------------------------- the guest thread pointer
 *
 * Android code reads its thread pointer straight from the CPU:
 * `mrc p15, 0, rX, c13, c0, 3` (TPIDRURO). That register belongs to the host
 * here (Windows keeps its own data there, glibc its thread pointer), so every
 * such instruction is redirected to a few instructions that load the guest's
 * thread pointer from host thread-local storage instead (arm/hk_patch.c).
 * These are the host's half: where the value lives, and how two instructions
 * can fetch it into a register without touching anything else -- read
 * coprocessor register c13, c0, <opc2> (the host's own thread block), then
 * load the word at <offset> from it.
 */
void hk_guest_tp_set(void *tp);
void *hk_guest_tp_get(void);
/* False when this host cannot say (and thread-pointer reads then cannot be redirected). */
bool hk_guest_tp_where(u32 *opc2, u32 *offset);

/* ----------------------------------------------------------------- faults
 *
 * Illegal instructions and access violations in guest code come here first
 * (a vectored exception handler; SIGILL/SIGSEGV/SIGBUS handlers under Linux).
 * The handler may change the registers and return true to resume.
 */
enum { HK_FAULT_ILLEGAL = 1, HK_FAULT_ACCESS, HK_FAULT_OTHER };

typedef struct hk_fault {
    int kind;
    u32 code;              /* the OS's own number: an NTSTATUS, or a signal */
    hk_addr address;       /* for HK_FAULT_ACCESS: what was touched */
    bool write, exec;
    u32 *regs;             /* r0..r15, writable; NULL on a host that is not ARM32 */
    u32 *cpsr;             /* writable */
} hk_fault;

typedef bool (*hk_fault_handler)(hk_fault *f);
void hk_fault_set_handler(hk_fault_handler h);
/*
 * Where the faulting thread goes on when the handler returns true. `target`
 * is a code address as a function pointer holds it: bit 0 set for Thumb.
 *
 * Not something to do to regs[15] by hand. Where the Thumb bit of a context
 * lives is the host's own business: Linux wants an even pc and the T bit in
 * cpsr; Windows on ARM resumes in whatever state bit 0 of Pc says and ignores
 * the T bit it is handed (seen on a phone: a context resumed at an even Pc
 * with T set ran on in ARM state).
 */
void hk_fault_resume_at(hk_fault *f, hk_addr target);

/* ------------------------------------------------------------------- time */

u64 hk_time_monotonic_ns(void);
/*
 * A count that only grows, for timing what is short and frequent (the Java runtime's native calls and allocations): as
 * cheap to read as the host allows, with no arithmetic. hk_ticks_ns turns a count, or a difference of two, into
 * nanoseconds. On Windows it is QueryPerformanceCounter's own count, which hk_time_monotonic_ns divides three times; on
 * 32-bit ARM those are calls of the C library's 64-bit division. Elsewhere it is nanoseconds.
 */
u64 hk_time_ticks(void);
u64 hk_ticks_ns(u64 ticks);
u64 hk_time_realtime_ns(void);         /* since the Unix epoch */
/* Offset of local time from UTC in seconds at `unix_seconds`, and whether DST is on. */
i32 hk_time_utc_offset(i64 unix_seconds, bool *dst);

/* ------------------------------------------------------------------ files */

typedef struct hk_file hk_file;

enum {
    HK_O_READ = 1, HK_O_WRITE = 2, HK_O_CREATE = 4, HK_O_TRUNC = 8, HK_O_APPEND = 16, HK_O_EXCL = 32,
};
enum { HK_E_OK = 0, HK_E_NOENT, HK_E_ACCESS, HK_E_EXIST, HK_E_NOTDIR, HK_E_ISDIR, HK_E_NOSPC, HK_E_IO, HK_E_INVAL, HK_E_NOTEMPTY, HK_E_BUSY,
       HK_E_XDEV };                        /* a rename from one file system to another */

typedef struct hk_stat {
    bool is_dir;
    u64 size;
    i64 mtime_ns, atime_ns, ctime_ns;  /* since the Unix epoch */
    u64 id;                            /* something distinct per file */
} hk_stat;

hk_file *hk_file_open(const char *path, int flags, int *err);
void hk_file_close(hk_file *f);
/* Bytes moved, or -1 with *err set. Positioned variants leave the file's own position alone. */
i64 hk_file_read(hk_file *f, void *buf, size_t n, int *err);
i64 hk_file_write(hk_file *f, const void *buf, size_t n, int *err);
i64 hk_file_pread(hk_file *f, void *buf, size_t n, u64 offset, int *err);
i64 hk_file_pwrite(hk_file *f, const void *buf, size_t n, u64 offset, int *err);
i64 hk_file_seek(hk_file *f, i64 offset, int whence, int *err);   /* whence: 0 set, 1 cur, 2 end */
bool hk_file_truncate(hk_file *f, u64 size, int *err);
bool hk_file_sync(hk_file *f);
bool hk_file_stat(hk_file *f, hk_stat *st);

bool hk_path_stat(const char *path, hk_stat *st, int *err);
bool hk_path_mkdir(const char *path, int *err);
bool hk_path_mkdirs(const char *path);
bool hk_path_remove(const char *path, int *err);        /* a file */
bool hk_path_rmdir(const char *path, int *err);
bool hk_path_rename(const char *from, const char *to, int *err);
/* Calls cb for every entry but . and ..; false with *err set when the directory cannot be listed. */
typedef void (*hk_dir_cb)(const char *name, bool is_dir, void *user);
bool hk_path_list(const char *path, hk_dir_cb cb, void *user, int *err);

/*
 * Names Windows refuses. Android allows every character in a name but '/' and NUL; Windows refuses < > : " \ | ? * and
 * the characters 1 to 31, a name that ends in a dot or a space, and the names of its devices (CON, PRN, AUX, NUL, COM0 to
 * 9, LPT0 to 9, with or without an extension). Such a name is given to Windows with each of those characters as the one
 * U+F000 above it -- a refused character, the dot or space at the end, the first letter of a device's name -- which is
 * what Cygwin and WSL do, and a name read back from Windows has every character from U+F001 to U+F07F as the one below
 * it again. A name that needed nothing is passed as it is. (One that holds a character of that private-use range of its
 * own is read back as the ASCII character; nothing on Android names files so.)
 *
 * hk_name_to_host makes every name in a path one Windows takes, malloc'd. HK_NAME_HOST: a host path, where '\' is a
 * separator as well as '/', and a leading \\?\ or \\.\ and a drive's "C:" are the place, not names. HK_NAME_FOREIGN: a
 * foreign path's part below its root, where '/' alone separates and a '\' is part of a name. hk_name_from_host undoes it
 * in a name read back, in place. The Win32 host layer does the first for every path it gives Windows and the second for
 * every name it lists; foreign paths are mapped before their owner is asked (below), on every host.
 */
#define HK_NAME_FOREIGN 0
#define HK_NAME_HOST 1
char *hk_name_to_host(const char *path, int how);
void hk_name_from_host(char *name);
/*
 * Whether this host's own file system needs the names mapped: Windows' does. Where a guest's path is joined to a host
 * folder (bionic/hk_libc_io.c) the guest's part is then mapped as a foreign path's is, so that a backslash in a name
 * stays a name's and does not become a separator; the host layer's own mapping finds nothing more to do in it.
 */
#if defined(_WIN32)
#define HK_HOST_MAPS_NAMES 1
#else
#define HK_HOST_MAPS_NAMES 0
#endif
/* The whole file, malloc'd (a NUL follows the bytes, not counted in *size); NULL when it cannot be read. */
u8 *hk_file_read_all(const char *path, size_t *size);
bool hk_file_write_all(const char *path, const void *data, size_t size);

/* A read-only view of a whole file (for APKs: nothing is copied until it is read). */
typedef struct hk_map {
    const u8 *data;
    u64 size;
    void *h1, *h2;
} hk_map;
bool hk_map_open(hk_map *m, const char *path);
void hk_map_close(hk_map *m);

/* How much the volume a folder is on holds and how much of that this process may still fill, in bytes; false where it cannot be asked (of a foreign path: never). */
bool hk_path_space(const char *path, u64 *total, u64 *available);

/* ---------------------------------------------------------- foreign paths
 *
 * Files the operating system will not open by path for this process but
 * hands over as handles some other way. On a phone that is the user's music,
 * pictures and videos: an app container may not name them to CreateFile2, and
 * Windows.Storage's broker gives out real file handles for them.
 *
 * Such a file has a host path all the same: HK_FOREIGN_PREFIX, the number of
 * a root, and a path below it. "megler-link:/2/Camera Roll/a.jpg" is a file of
 * root 2, and "megler-link:/2" that root itself. Every function above that
 * takes a path takes one of these. Instead of opening by path it asks
 * whoever was registered here for the operating system's own handle and goes
 * on with that, so reading, writing, mapping and listing are the same code
 * as for any other file. Making, removing and renaming have no handle to go
 * through, and are asked for as such.
 *
 * What follows the root's number reaches the callbacks with forward slashes
 * and none at either end; "" is the root itself. Its names are mapped as
 * Windows needs them (hk_name_to_host, HK_NAME_FOREIGN): a backslash or a
 * colon in a name, say, reaches them as U+F05C or U+F03A, and the names
 * listed through their handles are mapped back by the host layer. A path
 * with an empty part or a "." or ".." part is refused here (HK_E_INVAL) and
 * never reaches them. A root itself cannot be
 * made (HK_E_EXIST), removed or renamed (HK_E_BUSY), and that too is answered
 * here.
 *
 * A rename between a foreign path and an ordinary one, either way, is
 * HK_E_XDEV: two file systems, as far as anything above can tell.
 */
#define HK_FOREIGN_PREFIX "megler-link:/"

enum {
    HK_FOREIGN_FILE = 0,               /* a file, to read and write: HK_E_ISDIR for a directory */
    HK_FOREIGN_DIR,                    /* a directory, to list it: HK_E_NOTDIR for a file */
    HK_FOREIGN_ANY,                    /* whichever it is, for its size, kind and times only */
};

typedef struct hk_foreign {
    void *user;
    /*
     * The operating system's own handle, the caller's to close: a HANDLE on
     * Win32, the file descriptor plus one on POSIX. NULL with *err one of
     * HK_E_*. `flags` are HK_O_* (HK_O_APPEND may be ignored on Win32, where
     * the host layer seeks before each write itself); `kind` HK_FOREIGN_*.
     */
    void *(*open)(void *user, int root, const char *rel, int flags, int kind, int *err);
    bool (*mkdir)(void *user, int root, const char *rel, int *err);
    /* `dir`: a directory, which must be empty (HK_E_NOTEMPTY); else a file (HK_E_ISDIR for a directory). */
    bool (*remove)(void *user, int root, const char *rel, bool dir, int *err);
    /* As rename(2): onto an existing file it replaces it. Roots may differ. HK_E_XDEV when it cannot be done as a rename. */
    bool (*rename)(void *user, int from_root, const char *from, int to_root, const char *to, int *err);
} hk_foreign;

/* Who answers for foreign paths from now on; the structure is copied. NULL: nobody, and every foreign path is HK_E_NOENT. */
void hk_foreign_set(const hk_foreign *f);
bool hk_path_is_foreign(const char *path);

/*
 * For the two host files, which are otherwise the same for a foreign path as
 * for any other: the handle of one (NULL with *err set), and the three
 * things done by name.
 */
void *hk_foreign_open(const char *path, int flags, int kind, int *err);
bool hk_foreign_mkdir(const char *path, int *err);
bool hk_foreign_remove(const char *path, bool dir, int *err);
bool hk_foreign_rename(const char *from, const char *to, int *err);

/* ---------------------------------------------------------------- sockets
 *
 * The operating system's own sockets: Winsock 2 on Windows, BSD sockets on
 * POSIX. Everything here is in this header's neutral terms -- families,
 * options, flags and errors are its own numbers, addresses a structure of its
 * own -- and bionic/hk_net.c turns Android's ABI into them and back.
 *
 * A socket here never blocks. What would wait answers HK_NE_AGAIN (and a
 * connection under way HK_NE_INPROGRESS), and the caller waits the way it
 * waits for every other descriptor of the layer: on its one condition
 * (bionic/hk_libc_io.c). For that the host says when the state of a socket
 * may have changed -- a connection arrived, made or failed, data arrived,
 * there is room to send again, the other side closed -- by calling the
 * function hk_sock_set_notify names, on a thread of its own, and
 * hk_sock_ready says what holds now. A change is announced after it has
 * happened, so a caller that looks with hk_sock_ready and then waits for the
 * announcement, both under one lock that the announcement also takes, misses
 * nothing.
 */
typedef struct hk_sock hk_sock;

enum { HK_AF_INET = 1, HK_AF_INET6 = 2 };
enum { HK_SOCK_STREAM = 1, HK_SOCK_DGRAM = 2, HK_SOCK_RAW = 3 };

typedef struct hk_sockaddr {
    int family;                        /* HK_AF_INET, HK_AF_INET6; 0: no address */
    u16 port;                          /* in the host's byte order */
    u8 addr[16];                       /* in the network's: an IPv4 address in the first four */
    u32 flowinfo, scope_id;            /* IPv6 */
} hk_sockaddr;

/* What went wrong. */
enum {
    HK_NE_OK = 0, HK_NE_AGAIN, HK_NE_INPROGRESS, HK_NE_ALREADY, HK_NE_ISCONN, HK_NE_NOTCONN, HK_NE_CONNREFUSED, HK_NE_CONNRESET,
    HK_NE_CONNABORTED, HK_NE_TIMEDOUT, HK_NE_NETUNREACH, HK_NE_HOSTUNREACH, HK_NE_NETDOWN, HK_NE_NETRESET, HK_NE_ADDRINUSE,
    HK_NE_ADDRNOTAVAIL, HK_NE_AFNOSUPPORT, HK_NE_PROTONOSUPPORT, HK_NE_SOCKTNOSUPPORT, HK_NE_OPNOTSUPP, HK_NE_NOPROTOOPT, HK_NE_INVAL,
    HK_NE_ACCES, HK_NE_MSGSIZE, HK_NE_NOBUFS, HK_NE_MFILE, HK_NE_SHUTDOWN, HK_NE_PIPE, HK_NE_DESTADDRREQ, HK_NE_FAULT, HK_NE_IO
};

/* What hk_sock_ready says. HK_SOCK_RDHUP: the other side will send no more (its FIN, or a reset). */
enum { HK_SOCK_IN = 1, HK_SOCK_OUT = 2, HK_SOCK_ERR = 4, HK_SOCK_HUP = 8, HK_SOCK_RDHUP = 16 };
/* Flags of hk_sock_send / hk_sock_recv, and what hk_sock_recv says of what it read. */
enum { HK_MSG_PEEK = 1, HK_MSG_OOB = 2, HK_MSG_DONTROUTE = 4 };
enum { HK_MSG_TRUNCATED = 1 };

/* Options, set and read as one integer each. */
enum {
    HK_SO_REUSEADDR = 1, HK_SO_KEEPALIVE, HK_SO_BROADCAST, HK_SO_OOBINLINE, HK_SO_RCVBUF, HK_SO_SNDBUF,
    HK_SO_LINGER,                      /* seconds; -1: off */
    HK_SO_DONTROUTE, HK_TCP_NODELAY, HK_TCP_KEEPIDLE, HK_TCP_KEEPINTVL, HK_TCP_KEEPCNT, HK_TCP_MAXSEG,
    HK_IP_TOS, HK_IP_TTL, HK_IP_MULTICAST_TTL, HK_IP_MULTICAST_LOOP,
    HK_IP_MULTICAST_IF,                /* an IPv4 address as its four bytes in the network's order, read as a u32 */
    HK_IPV6_V6ONLY, HK_IPV6_UNICAST_HOPS, HK_IPV6_MULTICAST_HOPS, HK_IPV6_MULTICAST_LOOP,
    HK_IPV6_MULTICAST_IF,              /* an interface's index */
    HK_IPV6_TCLASS
};

/*
 * A new socket: `type` HK_SOCK_*, `protocol` IANA's number (6 TCP, 17 UDP, 0
 * the type's own). An IPv6 socket takes IPv4 addresses too (written as
 * ::ffff:a.b.c.d) until HK_IPV6_V6ONLY says otherwise, which is Linux's
 * default and not Windows'. NULL with *err set.
 */
hk_sock *hk_sock_open(int family, int type, int protocol, int *err);
void hk_sock_close(hk_sock *s);
bool hk_sock_bind(hk_sock *s, const hk_sockaddr *a, int *err);
bool hk_sock_listen(hk_sock *s, int backlog, int *err);
/* A connection that has arrived, or NULL with *err HK_NE_AGAIN when none has. */
hk_sock *hk_sock_accept(hk_sock *s, hk_sockaddr *peer, int *err);
/* false with *err HK_NE_INPROGRESS while a connection is being made; hk_sock_error says how it ended. A datagram socket's family 0: no peer any more. */
bool hk_sock_connect(hk_sock *s, const hk_sockaddr *a, int *err);
/* Bytes sent (as many as there was room for), or -1 with *err set. `to` NULL: to the peer. */
i32 hk_sock_send(hk_sock *s, const void *buf, u32 n, int flags, const hk_sockaddr *to, int *err);
/* Bytes read, 0 at the end of a stream, or -1 with *err set. `from` and `said` (HK_MSG_TRUNCATED) may be NULL. */
i32 hk_sock_recv(hk_sock *s, void *buf, u32 n, int flags, hk_sockaddr *from, int *said, int *err);
bool hk_sock_shutdown(hk_sock *s, int how, int *err);   /* how: 0 reading, 1 writing, 2 both */
bool hk_sock_name(hk_sock *s, bool peer, hk_sockaddr *a, int *err);
/* What holds now, HK_SOCK_*; never waits. */
int hk_sock_ready(hk_sock *s);
/* The error a connection ended with (SO_ERROR), HK_NE_*; asking clears it. */
int hk_sock_error(hk_sock *s);
/* Bytes that can be read without waiting (FIONREAD); whether the next byte is urgent data's mark (SIOCATMARK). */
i32 hk_sock_pending(hk_sock *s);
bool hk_sock_at_mark(hk_sock *s);
/* HK_NE_NOPROTOOPT: the option means nothing to this host. */
bool hk_sock_set(hk_sock *s, int option, int value, int *err);
bool hk_sock_get(hk_sock *s, int option, int *value, int *err);
/* Joins or leaves a multicast group: by an IPv4 interface's address (`iface`, family 0 for any) or an interface's index. */
bool hk_sock_multicast(hk_sock *s, bool join, const hk_sockaddr *group, const hk_sockaddr *iface, u32 index, int *err);
/* Who is told that some socket's state may have changed; called on a thread of the host's, never with a lock of the caller's held. */
void hk_sock_set_notify(void (*changed)(void));

/* ---------------------------------------------------- names and interfaces */

enum { HK_EAI_OK = 0, HK_EAI_NONAME, HK_EAI_AGAIN, HK_EAI_FAIL, HK_EAI_NODATA, HK_EAI_FAMILY, HK_EAI_MEMORY, HK_EAI_SYSTEM };
enum { HK_RESOLVE_CANONICAL = 1, HK_RESOLVE_ADDRCONFIG = 2 };
/*
 * The addresses of a name, as the system's resolver has them (DNS, the hosts
 * file): a malloc'd array of *count, port 0, in the order the system prefers
 * them. `family` HK_AF_* or 0 for both. *canonical, when asked for with
 * HK_RESOLVE_CANONICAL, is the canonical name, malloc'd, or stays NULL.
 * Returns HK_EAI_*. A name only: numbers are the caller's to parse.
 */
int hk_net_resolve(const char *name, int family, int flags, hk_sockaddr **addrs, int *count, char **canonical);
/* The name an address has (a reverse lookup), HK_EAI_NONAME when it has none. */
int hk_net_name_of(const hk_sockaddr *a, char *name, size_t size);

/* The host's network interfaces. */
enum { HK_IF_LOOPBACK = 1, HK_IF_WIFI, HK_IF_ETHERNET, HK_IF_CELLULAR, HK_IF_TUNNEL, HK_IF_OTHER };
enum { HK_IFF_UP = 1, HK_IFF_RUNNING = 2, HK_IFF_MULTICAST = 4, HK_IFF_BROADCAST = 8, HK_IFF_POINTOPOINT = 16 };
#define HK_NETIF_ADDRS 12
typedef struct hk_netif {
    u32 index;
    int kind;                          /* HK_IF_* */
    u32 flags;                         /* HK_IFF_* */
    u32 mtu;
    u8 hw[8];
    int hw_len;                        /* 0: no hardware address said */
    int naddrs;
    struct {
        hk_sockaddr addr;
        int prefix;                    /* bits of the network part */
    } addrs[HK_NETIF_ADDRS];
} hk_netif;
/* Fills up to `max` and returns how many there are; -1 when they cannot be listed. */
int hk_net_interfaces(hk_netif *out, int max);

/* ------------------------------------------------------ host shared libraries */

/* A DLL of the app package / a shared object, by file name. NULL when it does not load. */
void *hk_dl_open(const char *name);
void *hk_dl_sym(void *lib, const char *name);
/* Which of the host's own modules (the app, a DLL of the package or of the system) an address is in: its file name and where it is loaded. For a crash report. */
bool hk_host_module_at(hk_addr address, char *name, size_t size, hk_addr *base);

/* ------------------------------------------------------------------- misc */

/* The feature level of a Direct3D 11 device (an ID3D11Device *), as D3D_FEATURE_LEVEL numbers it (0x9300, 0xb100); 0 where there is no Direct3D. */
int hk_host_d3d11_feature_level(void *device);
/* Why a Direct3D 11 device is gone (an HRESULT such as DXGI_ERROR_DEVICE_HUNG), or 0 while it is still there. */
u32 hk_host_d3d11_removed_reason(void *device);
/* How much graphics memory the process may use and does (megabytes), as the adapter of a Direct3D 11 device says; false where it cannot be asked. */
bool hk_host_gpu_memory(void *device, u32 *budget_mb, u32 *used_mb);
/*
 * The process's address space, in megabytes: how much is free, the largest
 * free stretch, and how much is reserved without being committed. A 32-bit
 * process has two gigabytes, and it is the largest free stretch, not the
 * total, that decides whether the next big allocation -- anyone's, the
 * system's included -- can be made. False where it cannot be walked.
 */
bool hk_host_address_space(u32 *free_mb, u32 *largest_free_mb, u32 *reserved_mb, u32 *committed_mb);

/* Bytes from the OS's random source. */
void hk_random(void *buf, size_t n);
/* A line to the debugger / stderr; the log's last resort. */
void hk_host_print(const char *line);
const char *hk_host_name(void);        /* "win32-arm", "win32-x64", "linux-arm" */

#ifdef __cplusplus
}
#endif

#endif /* HK_HOST_H */
