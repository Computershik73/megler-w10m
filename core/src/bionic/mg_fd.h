/* Guest file descriptors, for the parts of the layer that are not libc: the looper, assets, sockets. */
#ifndef MG_FD_H
#define MG_FD_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Android's struct pollfd. */
typedef struct mg_pollfd {
    int fd;
    i16 events, revents;
} mg_pollfd;

#define MG_POLLIN 1
#define MG_POLLOUT 4
#define MG_POLLERR 8
#define MG_POLLHUP 0x10

/* poll() over guest descriptors: -1 waits forever, 0 does not wait. */
int mg_fd_poll(mg_pollfd *fds, int count, int timeout_ms);

i32 mg_fd_read(int fd, void *buf, u32 n);
i32 mg_fd_write(int fd, const void *buf, u32 n);
i32 mg_fd_pread(int fd, void *buf, u32 n, u64 offset);
/*
 * A descriptor's open file, held apart from the descriptor, which the app may close meanwhile: what a shared file mapping
 * writes back to (mg_libc_mem.c). NULL for a descriptor that is not open. Let go with mg_fd_let_go.
 */
void *mg_fd_hold(int fd);
i32 mg_fd_held_pwrite(void *held, const void *buf, u32 n, u64 offset);
void mg_fd_let_go(void *held);
i64 mg_fd_seek(int fd, i64 offset, int whence);
int mg_fd_close(int fd);
/*
 * mmap(MAP_SHARED) of shared memory (ashmem, memfd, ASharedMemory): a view of the same pages as every other mapping of it,
 * at an address of its own. *writable says whether the view may be written; *base and *mapped are what
 * mg_section_unmap takes. NULL with errno ENODEV when the descriptor is not shared memory. With `code` (PROT_EXEC without
 * PROT_WRITE) the view is one that may be run, read-only (mg_section_map_code), where the host gives one.
 */
/*
 * A read-only view of [offset, offset + len) of a regular file opened read-only, backed by the file (mg_file_map), as
 * mg_fd_shm_map gives: NULL with GE_NODEV for anything else, or where the range is past the file's end.
 */
void *mg_fd_file_map(int fd, u64 offset, size_t len, void **base, size_t *mapped);
void *mg_fd_shm_map(int fd, u64 offset, size_t len, bool code, bool *writable, void **base, size_t *mapped);
/* A descriptor on a host file (MG_O_* flags), for handing the guest a file that has no guest path -- the APK itself. */
int mg_fd_open_host(const char *host_path, int host_flags);
int mg_fd_eventfd(unsigned initial, int flags);
/*
 * Shared memory as a descriptor (ASharedMemory_create, memfd_create): `size` zeroed bytes of a section of the host's, read
 * and written like a file and mapped by mmap (mg_fd_shm_map), every mapping a view of the same pages. The descriptor holds
 * no view of its own: read and write map what they copy for the copy. -1 with errno on failure.
 */
int mg_fd_shm_create(const char *name, u64 size);
/* Its protection (ASharedMemory_setProt): from then on a mapping may be no more than that (G_PROT_* bits). 0, or -1 with errno. */
int mg_fd_shm_set_prot(int fd, int prot);
/* The protection a shared memory descriptor allows, or -1 when the descriptor is not one. */
int mg_fd_shm_prot(int fd);
/* Its size, or -1. */
i64 mg_fd_shm_size(int fd);

/* A descriptor whose object another module implements. Each call may be NULL. */
typedef struct mg_fd_ops {
    i32 (*read)(void *obj, void *buf, u32 n, bool nonblock);
    i32 (*write)(void *obj, const void *buf, u32 n, bool nonblock);
    /* Which of Android's POLL* bits hold now, of `events` and POLLERR and POLLHUP. Called with the descriptor table locked: it must not block or use descriptors. */
    int (*poll)(void *obj, int events);
    int (*readable)(void *obj);        /* bytes that can be read without blocking */
    void (*set_nonblock)(void *obj, bool on);
    /* Never called with the descriptor table locked. */
    void (*close)(void *obj);
    /* An ioctl other than FIONREAD and FIONBIO: 0, or -1 with errno set. NULL: ENOTTY. */
    int (*ioctl)(void *obj, unsigned request, void *arg);
    /* What fstat says it is: its S_IF* bits (S_IFSOCK); 0 a character device. */
    u32 file_type;
    /* Every change of what `poll` says is followed by mg_fd_notify, so a poll need not look again by itself every few milliseconds. */
    bool announces;
} mg_fd_ops;

int mg_fd_register(const mg_fd_ops *ops, void *obj);
/* The same with the descriptor's flags as Android numbers them: its access mode, O_NONBLOCK, O_CLOEXEC. */
int mg_fd_register_with(const mg_fd_ops *ops, void *obj, int flags);
/* The object behind a descriptor registered with these ops, or NULL. */
void *mg_fd_ext(int fd, const mg_fd_ops *ops);
/*
 * The same, kept for the caller until mg_fd_let_go(*held) whatever the app does to the descriptor meanwhile (another thread may close it while
 * this one waits on it). *flags: the descriptor's O_* flags now (O_NONBLOCK). NULL with errno EBADF when the descriptor is not open and
 * ENOTSOCK when it is something else, sockets being what these are for.
 */
void *mg_fd_ext_hold(int fd, const mg_fd_ops *ops, void **held, int *flags);
/* Wake every poll: something a registered object reports has changed. */
void mg_fd_notify(void);

/*
 * Waiting as the layer's own descriptors wait: under the descriptor table's lock, on the one condition every change of every descriptor
 * is announced on. A module that looks at its object's state under the lock and then waits misses no announcement made after it looked.
 */
void mg_fd_lock(void);
void mg_fd_unlock(void);
void mg_fd_notify_locked(void);
/* What ended a wait. MG_FD_INTERRUPTED: a handler that did not ask for SA_RESTART ran; MG_FD_HANDLER_RAN: one that did. */
enum { MG_FD_WOKEN = 0, MG_FD_TIMED_OUT, MG_FD_HANDLER_RAN, MG_FD_INTERRUPTED };
/* With the lock held: until something is announced, `timeout_ns` (0: no limit) has passed, or a signal for the thread has been handled (the lock let go of meanwhile). */
int mg_fd_wait_locked(u64 timeout_ns);

/* Where a prefix of Android's file system is on the host. */
void mg_fs_mount(const char *guest_prefix, const char *host_dir);

#ifdef __cplusplus
}
#endif

#endif
