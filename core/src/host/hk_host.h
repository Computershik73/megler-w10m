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
u64 hk_time_realtime_ns(void);         /* since the Unix epoch */
/* Offset of local time from UTC in seconds at `unix_seconds`, and whether DST is on. */
i32 hk_time_utc_offset(i64 unix_seconds, bool *dst);

/* ------------------------------------------------------------------ files */

typedef struct hk_file hk_file;

enum {
    HK_O_READ = 1, HK_O_WRITE = 2, HK_O_CREATE = 4, HK_O_TRUNC = 8, HK_O_APPEND = 16, HK_O_EXCL = 32,
};
enum { HK_E_OK = 0, HK_E_NOENT, HK_E_ACCESS, HK_E_EXIST, HK_E_NOTDIR, HK_E_ISDIR, HK_E_NOSPC, HK_E_IO, HK_E_INVAL, HK_E_NOTEMPTY, HK_E_BUSY };

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
