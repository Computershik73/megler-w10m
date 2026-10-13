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
i64 hk_fd_seek(int fd, i64 offset, int whence);
int hk_fd_close(int fd);
/* A descriptor on a host file (HK_O_* flags), for handing the guest a file that has no guest path -- the APK itself. */
int hk_fd_open_host(const char *host_path, int host_flags);
int hk_fd_eventfd(unsigned initial, int flags);

/* A descriptor whose object another module implements. Each call may be NULL. */
typedef struct hk_fd_ops {
    i32 (*read)(void *obj, void *buf, u32 n, bool nonblock);
    i32 (*write)(void *obj, const void *buf, u32 n, bool nonblock);
    /* Which of HK_POLL* in `events` hold now. Called with the descriptor table locked: it must not block or use descriptors. */
    int (*poll)(void *obj, int events);
    int (*readable)(void *obj);        /* bytes that can be read without blocking */
    void (*set_nonblock)(void *obj, bool on);
    void (*close)(void *obj);
} hk_fd_ops;

int hk_fd_register(const hk_fd_ops *ops, void *obj);
/* The object behind a descriptor registered with these ops, or NULL. */
void *hk_fd_ext(int fd, const hk_fd_ops *ops);
/* Wake every poll: something a registered object reports has changed. */
void hk_fd_notify(void);

/* Where a prefix of Android's file system is on the host. */
void hk_fs_mount(const char *guest_prefix, const char *host_dir);

#ifdef __cplusplus
}
#endif

#endif
