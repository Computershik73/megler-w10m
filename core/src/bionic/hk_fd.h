/* Guest file descriptors, for the parts of the layer that are not libc: the looper, assets, sockets. */
#ifndef HK_FD_H
#define HK_FD_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Android's struct pollfd. */
typedef struct hk_pollfd {
    int fd;
    i16 events, revents;
} hk_pollfd;

#define HK_POLLIN 1
#define HK_POLLOUT 4
#define HK_POLLERR 8
#define HK_POLLHUP 0x10

/* poll() over guest descriptors: -1 waits forever, 0 does not wait. */
int hk_fd_poll(hk_pollfd *fds, int count, int timeout_ms);

i32 hk_fd_read(int fd, void *buf, u32 n);
i32 hk_fd_write(int fd, const void *buf, u32 n);
i32 hk_fd_pread(int fd, void *buf, u32 n, u64 offset);
/*
 * A descriptor's open file, held apart from the descriptor, which the app may close meanwhile: what a shared file mapping
 * writes back to (hk_libc_mem.c). NULL for a descriptor that is not open. Let go with hk_fd_let_go.
 */
void *hk_fd_hold(int fd);
i32 hk_fd_held_pwrite(void *held, const void *buf, u32 n, u64 offset);
void hk_fd_let_go(void *held);
i64 hk_fd_seek(int fd, i64 offset, int whence);
int hk_fd_close(int fd);
/*
 * mmap(MAP_SHARED) of shared memory (ashmem, memfd, ASharedMemory): a view of the same pages as every other mapping of it,
 * at an address of its own. *writable says whether the view may be written; *base and *mapped are what
 * hk_section_unmap takes. NULL with errno ENODEV when the descriptor is not shared memory.
 */
/*
 * A read-only view of [offset, offset + len) of a regular file opened read-only, backed by the file (hk_file_map), as
 * hk_fd_shm_map gives: NULL with GE_NODEV for anything else, or where the range is past the file's end.
 */
void *hk_fd_file_map(int fd, u64 offset, size_t len, void **base, size_t *mapped);
void *hk_fd_shm_map(int fd, u64 offset, size_t len, bool *writable, void **base, size_t *mapped);
/* A descriptor on a host file (HK_O_* flags), for handing the guest a file that has no guest path -- the APK itself. */
int hk_fd_open_host(const char *host_path, int host_flags);
int hk_fd_eventfd(unsigned initial, int flags);
/*
 * Shared memory as a descriptor (ASharedMemory_create, memfd_create): `size` zeroed bytes of a section of the host's, read
 * and written like a file and mapped by mmap (hk_fd_shm_map), every mapping a view of the same pages. The descriptor holds
 * no view of its own: read and write map what they copy for the copy. -1 with errno on failure.
 */
int hk_fd_shm_create(const char *name, u64 size);
/* Its protection (ASharedMemory_setProt): from then on a mapping may be no more than that (G_PROT_* bits). 0, or -1 with errno. */
int hk_fd_shm_set_prot(int fd, int prot);
/* The protection a shared memory descriptor allows, or -1 when the descriptor is not one. */
int hk_fd_shm_prot(int fd);
/* Its size, or -1. */
i64 hk_fd_shm_size(int fd);

/* A descriptor whose object another module implements. Each call may be NULL. */
typedef struct hk_fd_ops {
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
    /* Every change of what `poll` says is followed by hk_fd_notify, so a poll need not look again by itself every few milliseconds. */
    bool announces;
} hk_fd_ops;

int hk_fd_register(const hk_fd_ops *ops, void *obj);
/* The same with the descriptor's flags as Android numbers them: its access mode, O_NONBLOCK, O_CLOEXEC. */
int hk_fd_register_with(const hk_fd_ops *ops, void *obj, int flags);
/* The object behind a descriptor registered with these ops, or NULL. */
void *hk_fd_ext(int fd, const hk_fd_ops *ops);
/*
 * The same, kept for the caller until hk_fd_let_go(*held) whatever the app does to the descriptor meanwhile (another thread may close it while
 * this one waits on it). *flags: the descriptor's O_* flags now (O_NONBLOCK). NULL with errno EBADF when the descriptor is not open and
 * ENOTSOCK when it is something else, sockets being what these are for.
 */
void *hk_fd_ext_hold(int fd, const hk_fd_ops *ops, void **held, int *flags);
/* Wake every poll: something a registered object reports has changed. */
void hk_fd_notify(void);

/*
 * Waiting as the layer's own descriptors wait: under the descriptor table's lock, on the one condition every change of every descriptor
 * is announced on. A module that looks at its object's state under the lock and then waits misses no announcement made after it looked.
 */
void hk_fd_lock(void);
void hk_fd_unlock(void);
void hk_fd_notify_locked(void);
/* What ended a wait. HK_FD_INTERRUPTED: a handler that did not ask for SA_RESTART ran; HK_FD_HANDLER_RAN: one that did. */
enum { HK_FD_WOKEN = 0, HK_FD_TIMED_OUT, HK_FD_HANDLER_RAN, HK_FD_INTERRUPTED };
/* With the lock held: until something is announced, `timeout_ns` (0: no limit) has passed, or a signal for the thread has been handled (the lock let go of meanwhile). */
int hk_fd_wait_locked(u64 timeout_ns);

/* Where a prefix of Android's file system is on the host. */
void hk_fs_mount(const char *guest_prefix, const char *host_dir);

#ifdef __cplusplus
}
#endif

#endif
