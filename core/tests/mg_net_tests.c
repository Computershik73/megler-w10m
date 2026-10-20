/*
 * The network, tried on this machine's loopback.
 *
 * The C library's functions are called as an app's code calls them, by the
 * names it binds against (mg_bionic_find), with bionic's structures and
 * Linux's numbers; what they must answer is what Linux answers. Everything
 * goes to 127.0.0.1 or ::1 and back: nothing here needs a network beyond the
 * machine, and nothing here knows which host it runs on.
 *
 * The same file has the check a phone runs (mg_net_check_run, meglet_net_check
 * in meglet.h), which also goes out to the internet, and which the tests run
 * without that part.
 */
#include "mg_net_tests.h"

#include <stdio.h>

#include "../src/bionic/mg_bionic.h"
#include "../src/bionic/mg_fd.h"
#include "../src/mg_base.h"
#include "../src/host/mg_host.h"

static int t_checks, t_failed;
static mg_test_sink t_sink;
static void *t_sink_user;

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (t_sink) t_sink(line, t_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define CHECK(cond)                                                    \
    do {                                                               \
        t_checks++;                                                    \
        if (!(cond)) {                                                 \
            t_failed++;                                                \
            say("  FAIL %s:%d: %s (errno %d)", __FILE__, __LINE__, #cond, *mg_errno_ptr()); \
        }                                                              \
    } while (0)

#define CHECK_INT(expr, want)                                                                      \
    do {                                                                                           \
        long long got_ = (long long)(expr), want_ = (long long)(want);                             \
        t_checks++;                                                                                \
        if (got_ != want_) {                                                                       \
            t_failed++;                                                                            \
            say("  FAIL %s:%d: %s = %lld, expected %lld", __FILE__, __LINE__, #expr, got_, want_); \
        }                                                                                          \
    } while (0)

/* Linux's numbers, as an app has them. */
enum {
    AF_UNIX_ = 1, AF_INET_ = 2, AF_INET6_ = 10, SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2, SOCK_SEQPACKET_ = 5, SOCK_NONBLOCK_ = 04000, SOCK_CLOEXEC_ = 02000000,
    SOL_SOCKET_ = 1, SO_REUSEADDR_ = 2, SO_TYPE_ = 3, SO_ERROR_ = 4, SO_SNDBUF_ = 7, SO_RCVBUF_ = 8, SO_KEEPALIVE_ = 9, SO_PRIORITY_ = 12, SO_LINGER_ = 13,
    SO_RCVTIMEO_ = 20, SO_ACCEPTCONN_ = 30, SO_DOMAIN_ = 39, IPPROTO_TCP_ = 6, IPPROTO_UDP_ = 17, IPPROTO_IPV6_ = 41, TCP_NODELAY_ = 1, TCP_KEEPIDLE_ = 4,
    IPV6_V6ONLY_ = 26, MSG_PEEK_ = 2, MSG_TRUNC_ = 0x20, MSG_DONTWAIT_ = 0x40, MSG_WAITALL_ = 0x100, SHUT_RD_ = 0, SHUT_WR_ = 1, SHUT_RDWR_ = 2,
    POLLIN_ = 1, POLLOUT_ = 4, POLLERR_ = 8, POLLHUP_ = 0x10, POLLNVAL_ = 0x20, POLLRDHUP_ = 0x2000, O_NONBLOCK_ = 04000, F_GETFL_ = 3, F_SETFL_ = 4,
    F_GETFD_ = 1, FIONREAD_ = 0x541B, SIOCGIFFLAGS_ = 0x8913, SIOCGIFINDEX_ = 0x8933, IFF_UP_ = 1, IFF_LOOPBACK_ = 8, EPOLLIN_ = 1, EPOLLET_ = (int)0x80000000u,
    EPOLL_CTL_ADD_ = 1, AI_PASSIVE_ = 1, AI_CANONNAME_ = 2, AI_NUMERICHOST_ = 4, AI_ADDRCONFIG_ = 0x400, NI_NUMERICHOST_ = 2, NI_NUMERICSERV_ = 8,
    NI_NAMEREQD_ = 4, EAI_BADFLAGS_ = 3, EAI_NONAME_ = 8, EAI_SERVICE_ = 9,
};
#define S_IFMT_ 0170000u
#define S_IFSOCK_ 0140000u

/* Bionic's structures: pointers and size_t the host's own, which on the phone and under qemu are bionic's 32 bits. */
typedef struct t_iovec {
    void *base;
    size_t len;
} t_iovec;
typedef struct t_msghdr {
    void *name;
    u32 namelen;
    t_iovec *iov;
    size_t iovlen;
    void *control;
    size_t controllen;
    int flags;
} t_msghdr;
typedef struct t_epoll_event {
    u32 events;
    u64 data;
} t_epoll_event;
typedef struct t_addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    u32 ai_addrlen;
    char *ai_canonname;
    u8 *ai_addr;
    struct t_addrinfo *ai_next;
} t_addrinfo;
typedef struct t_hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype, h_length;
    char **h_addr_list;
} t_hostent;
typedef struct t_ifaddrs {
    struct t_ifaddrs *ifa_next;
    char *ifa_name;
    u32 ifa_flags;
    u8 *ifa_addr, *ifa_netmask, *ifa_ifu;
    void *ifa_data;
} t_ifaddrs;
typedef struct t_sigaction {
    u32 handler, mask, flags, restorer;
} t_sigaction;

static struct {
    int (*socket)(int, int, int);
    int (*socketpair)(int, int, int, int *);
    int (*bind)(int, const void *, u32);
    int (*listen)(int, int);
    int (*accept)(int, void *, u32 *);
    int (*accept4)(int, void *, u32 *, int);
    int (*connect)(int, const void *, u32);
    i32 (*send)(int, const void *, u32, int);
    i32 (*recv)(int, void *, u32, int);
    i32 (*sendto)(int, const void *, u32, int, const void *, u32);
    i32 (*recvfrom)(int, void *, u32, int, void *, u32 *);
    i32 (*sendmsg)(int, const t_msghdr *, int);
    i32 (*recvmsg)(int, t_msghdr *, int);
    int (*shutdown)(int, int);
    int (*getsockname)(int, void *, u32 *);
    int (*getpeername)(int, void *, u32 *);
    int (*setsockopt)(int, int, int, const void *, u32);
    int (*getsockopt)(int, int, int, void *, u32 *);
    int (*close)(int);
    i32 (*read)(int, void *, u32);
    i32 (*write)(int, const void *, u32);
    int (*poll)(mg_pollfd *, unsigned, int);
    int (*select)(int, u32 *, u32 *, u32 *, i32 *);
    int (*epoll_create1)(int);
    int (*epoll_ctl)(int, int, int, const t_epoll_event *);
    int (*epoll_wait)(int, t_epoll_event *, int, int);
    int (*pipe2)(int *, int);
    int (*eventfd)(unsigned, int);
    int (*fcntl)(int, int, int);
    int (*ioctl)(int, unsigned, void *);
    int (*fstat)(int, void *);
    int (*dup2)(int, int);
    int (*getaddrinfo)(const char *, const char *, const t_addrinfo *, t_addrinfo **);
    int (*android_getaddrinfofornet)(const char *, const char *, const t_addrinfo *, unsigned, unsigned, t_addrinfo **);
    void (*freeaddrinfo)(t_addrinfo *);
    int (*getnameinfo)(const void *, u32, char *, size_t, char *, size_t, int);
    const char *(*gai_strerror)(int);
    int (*inet_pton)(int, const char *, void *);
    const char *(*inet_ntop)(int, const void *, char *, u32);
    int (*inet_aton)(const char *, u32 *);
    int (*getifaddrs)(t_ifaddrs **);
    void (*freeifaddrs)(t_ifaddrs *);
    u32 (*if_nametoindex)(const char *);
    char *(*if_indextoname)(u32, char *);
    t_hostent *(*gethostbyname)(const char *);
    int (*sigaction)(int, const t_sigaction *, t_sigaction *);
} N;

static bool libc_found(void)
{
    static const char *const names[] = { "socket", "socketpair", "bind", "listen", "accept", "accept4", "connect", "send", "recv", "sendto", "recvfrom",
                                         "sendmsg", "recvmsg", "shutdown", "getsockname", "getpeername", "setsockopt", "getsockopt", "close", "read", "write",
                                         "poll", "select", "epoll_create1", "epoll_ctl", "epoll_wait", "pipe2", "eventfd", "fcntl", "ioctl", "fstat", "dup2",
                                         "getaddrinfo", "android_getaddrinfofornet", "freeaddrinfo", "getnameinfo", "gai_strerror", "inet_pton", "inet_ntop",
                                         "inet_aton", "getifaddrs", "freeifaddrs", "if_nametoindex", "if_indextoname", "gethostbyname", "sigaction" };
    void **slot = (void **)&N;
    size_t i;
    for (i = 0; i < MG_COUNT(names); i++) {
        slot[i] = mg_bionic_find(names[i]);
        if (!slot[i]) return false;
    }
    return true;
}

static int err(void) { return *mg_errno_ptr(); }
static u64 now_ms(void) { return mg_time_monotonic_ns() / 1000000ull; }

static const u8 k_lo4[4] = { 127, 0, 0, 1 };
static const u8 k_any6[16] = { 0 };
static const u8 k_mapped_lo[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 127, 0, 0, 1 };

static u32 sa4(u8 *sa, const u8 *ip, u16 port)
{
    memset(sa, 0, 16);
    mg_wr16(sa, AF_INET_);
    sa[2] = (u8)(port >> 8);
    sa[3] = (u8)port;
    memcpy(sa + 4, ip, 4);
    return 16;
}

static u32 sa6(u8 *sa, const u8 *ip, u16 port)
{
    memset(sa, 0, 28);
    mg_wr16(sa, AF_INET6_);
    sa[2] = (u8)(port >> 8);
    sa[3] = (u8)port;
    memcpy(sa + 8, ip, 16);
    return 28;
}

static u16 port_of(const u8 *sa) { return (u16)((sa[2] << 8) | sa[3]); }

/* A TCP server on 127.0.0.1 (or :: with `v6`, which takes IPv4 too) on a port of the system's choosing: the descriptor, its port in *port. */
static int listener(bool v6, u16 *port)
{
    u8 sa[28];
    u32 len = sizeof(sa);
    int s = N.socket(v6 ? AF_INET6_ : AF_INET_, SOCK_STREAM_, 0);
    if (s < 0) return -1;
    if ((v6 ? N.bind(s, sa, sa6(sa, k_any6, 0)) : N.bind(s, sa, sa4(sa, k_lo4, 0))) != 0 || N.listen(s, 8) != 0 || N.getsockname(s, sa, &len) != 0) {
        N.close(s);
        return -1;
    }
    *port = port_of(sa);
    return s;
}

/* A connected pair over loopback TCP: *a the connecting end, *b the accepted one. */
static bool tcp_pair(int *a, int *b)
{
    u16 port = 0;
    u8 sa[16];
    int l = listener(false, &port);
    *a = *b = -1;
    if (l < 0) return false;
    *a = N.socket(AF_INET_, SOCK_STREAM_, 0);
    if (*a < 0 || N.connect(*a, sa, sa4(sa, k_lo4, port)) != 0) {
        N.close(l);
        return false;
    }
    *b = N.accept(l, NULL, NULL);
    N.close(l);
    return *b >= 0;
}

static u32 fnv(const u8 *p, u32 n, u32 h)
{
    u32 i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* ------------------------------------------------ TCP, blocking, both ways */

typedef struct client_job {
    u16 port;
    bool v6;
    int fd;
    int result, error;
    char got[32];
    u8 own[28];
    u32 own_len;
} client_job;

/* Connects to the port (as libcore does: an IPv6 socket, the IPv4 loopback written ::ffff:127.0.0.1), says "hello", reads the answer, closes. */
static void client_main(void *arg)
{
    client_job *j = (client_job *)arg;
    u8 sa[28];
    i32 n;
    j->fd = N.socket(j->v6 ? AF_INET6_ : AF_INET_, SOCK_STREAM_, 0);
    j->result = j->v6 ? N.connect(j->fd, sa, sa6(sa, k_mapped_lo, j->port)) : N.connect(j->fd, sa, sa4(sa, k_lo4, j->port));
    j->error = err();
    if (j->result != 0) return;
    j->own_len = sizeof(j->own);
    N.getsockname(j->fd, j->own, &j->own_len);
    N.send(j->fd, "hello", 5, 0);
    n = N.recv(j->fd, j->got, 5, MSG_WAITALL_);
    j->got[n > 0 ? n : 0] = 0;
    N.close(j->fd);
}

static void test_tcp_blocking(bool v6)
{
    client_job job;
    u8 peer[28];
    u32 peer_len = sizeof(peer);
    char buf[16];
    mg_thread *t;
    u16 port = 0;
    int l = listener(v6, &port), c;
    i32 n;
    say("network: TCP over %s, blocking, a server and a client in one process", v6 ? "IPv6 (the client reaching it as ::ffff:127.0.0.1)" : "IPv4");
    CHECK(l >= 0 && port > 0);
    if (l < 0) return;
    memset(&job, 0, sizeof(job));
    job.port = port;
    job.v6 = v6;
    t = mg_thread_start(client_main, &job, 256 * 1024, "net-client");
    c = N.accept(l, peer, &peer_len);
    CHECK(c >= 0);
    n = c >= 0 ? N.recv(c, buf, 5, MSG_WAITALL_) : -1;
    CHECK(n == 5 && !memcmp(buf, "hello", 5));
    CHECK(c >= 0 && N.send(c, "world", 5, 0) == 5);
    mg_thread_join(t);
    CHECK_INT(job.result, 0);
    CHECK(!strcmp(job.got, "world"));
    /* What the server says of its peer is what the client says of itself. */
    if (v6) {
        CHECK(peer_len == 28 && mg_rd16(peer) == AF_INET6_ && !memcmp(peer + 8, k_mapped_lo, 16));
        CHECK(job.own_len == 28 && mg_rd16(job.own) == AF_INET6_ && port_of(job.own) == port_of(peer));
    } else {
        CHECK(peer_len == 16 && mg_rd16(peer) == AF_INET_ && !memcmp(peer + 4, k_lo4, 4) && port_of(peer) == port_of(job.own));
    }
    /* After the client is gone: the end, then nothing. */
    CHECK(c >= 0 && N.recv(c, buf, sizeof(buf), 0) == 0);
    if (c >= 0) N.close(c);
    N.close(l);
}

/* ------------------------------------- connecting without blocking, refusal */

static void test_nonblocking_connect(void)
{
    mg_pollfd p;
    u8 sa[16];
    u16 port = 0;
    int l = listener(false, &port), s, r, e, v = -1, c, n;
    u32 len = sizeof(v);
    u64 t0;
    bool answered;
    say("network: a connection made without blocking: EINPROGRESS, poll for writability, SO_ERROR");
    s = N.socket(AF_INET_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0);
    CHECK(l >= 0 && s >= 0);
    /* (Never connected yet, poll on Linux says writable and hung up.) */
    p.fd = s;
    p.events = POLLIN_ | POLLOUT_;
    CHECK(N.poll(&p, 1, 0) == 1 && p.revents == (POLLOUT_ | POLLHUP_));
    r = N.connect(s, sa, sa4(sa, k_lo4, port));
    e = err();
    CHECK(r == 0 || e == GE_INPROGRESS);
    p.events = POLLOUT_;
    CHECK(N.poll(&p, 1, 3000) == 1 && (p.revents & POLLOUT_) && !(p.revents & POLLERR_));
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_ERROR_, &v, &len) == 0 && v == 0 && len == 4);
    /* Asked again once it is made: EISCONN, as Linux says. */
    CHECK(N.connect(s, sa, 16) == -1 && err() == GE_ISCONN);
    c = N.accept(l, NULL, NULL);
    CHECK(c >= 0);
    /* Non-blocking reads with nothing there; MSG_DONTWAIT on a blocking one. */
    CHECK(N.recv(s, sa, 1, 0) == -1 && err() == GE_AGAIN);
    CHECK(N.recv(c, sa, 1, MSG_DONTWAIT_) == -1 && err() == GE_AGAIN);
    N.close(c);
    N.close(s);
    N.close(l);

    /*
     * Without blocking first, with a short wait. The phone (0.1.0.138) does not refuse a connection to a closed
     * port of its own loopback: the SYN goes unanswered, a non-blocking attempt is still EINPROGRESS after 5 s, and
     * a blocking one ended only after 127 s. There "no answer within 2 s" is the phone's behaviour, said and not
     * counted, and the blocking attempt is left out so the Self-test does not wait two minutes. Anywhere else it is
     * refused at once, and no answer is a failure.
     */
    say("network: refused: a port nobody listens on, not blocking and blocking");
    l = listener(false, &port);
    N.close(l);
    s = N.socket(AF_INET_, SOCK_STREAM_ | SOCK_NONBLOCK_, 0);
    t0 = now_ms();
    r = N.connect(s, sa, sa4(sa, k_lo4, port));
    e = err();
    answered = true;
    if (r == -1 && e == GE_INPROGRESS) {
        p.fd = s;
        p.events = POLLOUT_;
        n = N.poll(&p, 1, 2000);
        if (n == 0 && !strcmp(mg_host_name(), "win32-arm")) {
            answered = false;
            say("  no answer within 2 s: this phone does not refuse a connection to a closed port of its own loopback but lets it "
                "time out (a blocking one took about two minutes); the phone's behaviour, not counted, and the blocking attempt left out");
        } else {
            CHECK(n == 1 && (p.revents & POLLERR_));
            len = sizeof(v);
            CHECK(N.getsockopt(s, SOL_SOCKET_, SO_ERROR_, &v, &len) == 0 && v == GE_CONNREFUSED);
            /* Asked for, the error is gone. */
            len = sizeof(v);
            CHECK(N.getsockopt(s, SOL_SOCKET_, SO_ERROR_, &v, &len) == 0 && v == 0);
        }
    } else {
        CHECK(r == -1 && e == GE_CONNREFUSED);
    }
    if (answered) say("  refused without blocking after %d ms", (int)(now_ms() - t0));
    N.close(s);
    if (!answered) return;
    s = N.socket(AF_INET_, SOCK_STREAM_, 0);
    t0 = now_ms();
    r = N.connect(s, sa, 16);
    CHECK(r == -1 && err() == GE_CONNREFUSED);
    say("  refused after %d ms", (int)(now_ms() - t0));
    N.close(s);
}

/* -------------------------------------------------- accept with a timeout */

typedef struct late_job {
    u16 port;
    int delay_ms;
    int fd;
} late_job;

static void late_connect(void *arg)
{
    late_job *j = (late_job *)arg;
    u8 sa[16];
    mg_sleep_ns((u64)j->delay_ms * 1000000ull);
    j->fd = N.socket(AF_INET_, SOCK_STREAM_, 0);
    N.connect(j->fd, sa, sa4(sa, k_lo4, j->port));
}

static void test_accept_timeout(void)
{
    mg_pollfd p;
    late_job job;
    mg_thread *t;
    u16 port = 0;
    int l = listener(false, &port), r, c;
    u64 t0;
    say("network: accept with a timeout, through poll");
    CHECK(l >= 0);
    p.fd = l;
    p.events = POLLIN_;
    t0 = now_ms();
    r = N.poll(&p, 1, 200);
    t0 = now_ms() - t0;
    CHECK(r == 0 && t0 >= 180 && t0 < 1500);
    say("  nobody came: poll ended after %d ms of 200", (int)t0);
    /* Somebody comes 100 ms into a wait of 5 s: the wait ends then, not at its end. */
    job.port = port;
    job.delay_ms = 100;
    job.fd = -1;
    t = mg_thread_start(late_connect, &job, 128 * 1024, "net-late");
    t0 = now_ms();
    r = N.poll(&p, 1, 5000);
    t0 = now_ms() - t0;
    CHECK(r == 1 && p.revents == POLLIN_ && t0 < 2000);
    say("  a connection 100 ms in: poll ended after %d ms", (int)t0);
    c = N.accept4(l, NULL, NULL, SOCK_NONBLOCK_ | SOCK_CLOEXEC_);
    CHECK(c >= 0 && (N.fcntl(c, F_GETFL_, 0) & O_NONBLOCK_) && N.fcntl(c, F_GETFD_, 0) == 1);
    /* None waiting now: a non-blocking accept says so. */
    CHECK(N.fcntl(l, F_SETFL_, O_NONBLOCK_) == 0 && N.accept(l, NULL, NULL) == -1 && err() == GE_AGAIN);
    mg_thread_join(t);
    N.close(job.fd);
    N.close(c);
    N.close(l);
}

/* ------------------------------------------------------- large transfers */

typedef struct pump_job {
    int fd;
    u32 total, sent, hash;
    bool nonblock;
} pump_job;

/* Sends `total` bytes of a pattern in pieces of changing size; blocking, or with poll for room when the socket is non-blocking. */
static void pump_main(void *arg)
{
    pump_job *j = (pump_job *)arg;
    u8 *buf = (u8 *)mg_alloc(70000);
    u32 piece = 1, i;
    j->hash = 2166136261u;
    while (j->sent < j->total) {
        u32 n = j->total - j->sent < piece ? j->total - j->sent : piece;
        i32 r;
        for (i = 0; i < n; i++) buf[i] = (u8)((j->sent + i) * 7 + ((j->sent + i) >> 13));
        r = N.send(j->fd, buf, n, 0);
        if (r < 0 && j->nonblock && err() == GE_AGAIN) {
            mg_pollfd p;
            p.fd = j->fd;
            p.events = POLLOUT_;
            if (N.poll(&p, 1, 10000) != 1) break;
            continue;
        }
        if (r <= 0) break;
        j->hash = fnv(buf, (u32)r, j->hash);
        j->sent += (u32)r;
        piece = piece * 3 + 1;
        if (piece > 65536) piece = 1 + piece % 9973;
    }
    free(buf);
}

static void test_large(bool nonblocking_writer)
{
    pump_job job;
    mg_thread *t;
    u8 *buf = (u8 *)mg_alloc(7777);
    u32 got = 0, hash = 2166136261u;
    int a, b;
    u64 t0;
    say("network: 6 MB over loopback TCP in pieces of every size, %s, the reader starting late", nonblocking_writer ? "the writer not blocking and waiting with poll" : "blocking");
    CHECK(tcp_pair(&a, &b));
    memset(&job, 0, sizeof(job));
    job.fd = a;
    job.total = 6u << 20;
    job.nonblock = nonblocking_writer;
    if (nonblocking_writer) N.fcntl(a, F_SETFL_, O_NONBLOCK_);
    t0 = now_ms();
    t = mg_thread_start(pump_main, &job, 256 * 1024, "net-pump");
    /* (The writer fills what the system holds and has to wait for room.) */
    mg_sleep_ns(150000000ull);
    for (;;) {
        i32 n = N.recv(b, buf, 7777, 0);
        if (n <= 0) break;
        hash = fnv(buf, (u32)n, hash);
        got += (u32)n;
        if (got >= job.total) break;
    }
    mg_thread_join(t);
    CHECK(job.sent == job.total && got == job.total && hash == job.hash);
    say("  %u bytes in %d ms, the same as were sent", got, (int)(now_ms() - t0));
    N.close(a);
    N.close(b);
    free(buf);
}

/* ------------------------------------------------------- shutdown and EOF */

static void test_shutdown(void)
{
    mg_pollfd p;
    char buf[16];
    int a, b;
    say("network: shutdown and the end of a stream");
    CHECK(tcp_pair(&a, &b));
    CHECK(N.send(a, "last", 4, 0) == 4 && N.shutdown(a, SHUT_WR_) == 0);
    /* The other side reads what was sent, then the end; poll says readable and that the peer is done. */
    CHECK(N.recv(b, buf, sizeof(buf), MSG_WAITALL_) == 4 && !memcmp(buf, "last", 4));
    p.fd = b;
    p.events = POLLIN_ | POLLRDHUP_;
    CHECK(N.poll(&p, 1, 2000) == 1 && (p.revents & POLLIN_) && (p.revents & POLLRDHUP_));
    CHECK(N.recv(b, buf, sizeof(buf), 0) == 0 && N.read(b, buf, sizeof(buf)) == 0);
    /* Writing the other way still works. */
    CHECK(N.send(b, "back", 4, 0) == 4 && N.recv(a, buf, 4, MSG_WAITALL_) == 4 && !memcmp(buf, "back", 4));
    /* What was shut down for writing refuses to write (EPIPE). */
    CHECK(N.send(a, "x", 1, 0) == -1 && err() == GE_PIPE);
    CHECK(N.shutdown(a, 7) == -1 && err() == GE_INVAL);
    N.close(b);
    CHECK(N.recv(a, buf, sizeof(buf), 0) == 0);
    N.close(a);
    /* A socket that was never connected cannot be shut down, nor asked its peer. */
    a = N.socket(AF_INET_, SOCK_STREAM_, 0);
    CHECK(N.shutdown(a, SHUT_RDWR_) == -1 && err() == GE_NOTCONN);
    {
        u8 sa[28];
        u32 len = sizeof(sa);
        CHECK(N.getpeername(a, sa, &len) == -1 && err() == GE_NOTCONN);
    }
    N.close(a);
}

/* -------------------------------------------------------------------- UDP */

static void test_udp(void)
{
    u8 sa_a[16], sa_b[16], from[28], buf[64];
    u32 len, from_len;
    t_iovec iov[2];
    t_msghdr m;
    char part1[3], part2[8];
    int a = N.socket(AF_INET_, SOCK_DGRAM_, 0), b = N.socket(AF_INET_, SOCK_DGRAM_, IPPROTO_UDP_), v = 0;
    say("network: UDP over loopback: sendto, recvfrom, connected, a datagram cut short");
    CHECK(a >= 0 && b >= 0);
    CHECK(N.bind(a, sa_a, sa4(sa_a, k_lo4, 0)) == 0 && N.bind(b, sa_b, sa4(sa_b, k_lo4, 0)) == 0);
    len = sizeof(sa_a);
    CHECK(N.getsockname(a, sa_a, &len) == 0 && len == 16 && port_of(sa_a) > 0);
    len = sizeof(sa_b);
    CHECK(N.getsockname(b, sa_b, &len) == 0 && port_of(sa_b) > 0);
    len = sizeof(v);
    CHECK(N.getsockopt(a, SOL_SOCKET_, SO_TYPE_, &v, &len) == 0 && v == SOCK_DGRAM_);
    CHECK(N.sendto(a, "ping", 4, 0, sa_b, 16) == 4);
    from_len = sizeof(from);
    CHECK(N.recvfrom(b, buf, sizeof(buf), 0, from, &from_len) == 4 && !memcmp(buf, "ping", 4));
    CHECK(from_len == 16 && mg_rd16(from) == AF_INET_ && port_of(from) == port_of(sa_a) && !memcmp(from + 4, k_lo4, 4));
    /* Connected: send and recv without addresses, and the peer known. */
    CHECK(N.connect(b, sa_a, 16) == 0 && N.send(b, "pong", 4, 0) == 4);
    CHECK(N.recv(a, buf, sizeof(buf), 0) == 4 && !memcmp(buf, "pong", 4));
    len = sizeof(from);
    CHECK(N.getpeername(b, from, &len) == 0 && port_of(from) == port_of(sa_a));
    /* Gathered from two pieces, scattered into two, and a datagram longer than the buffer cut short and said so. */
    iov[0].base = (void *)"abc";
    iov[0].len = 3;
    iov[1].base = (void *)"defghij";
    iov[1].len = 7;
    memset(&m, 0, sizeof(m));
    m.iov = iov;
    m.iovlen = 2;
    CHECK(N.sendmsg(b, &m, 0) == 10);
    iov[0].base = part1;
    iov[0].len = 3;
    iov[1].base = part2;
    iov[1].len = 4;
    m.name = from;
    m.namelen = sizeof(from);
    CHECK(N.recvmsg(a, &m, 0) == 7 && !memcmp(part1, "abc", 3) && !memcmp(part2, "defg", 4) && (m.flags & MSG_TRUNC_) && m.namelen == 16 && m.controllen == 0);
    /* A datagram socket has no peer to write to until it is given one (EDESTADDRREQ). */
    CHECK(N.send(a, "x", 1, 0) == -1 && err() == GE_DESTADDRREQ);
    N.close(a);
    N.close(b);
}

/*
 * A ping socket (Linux's SOCK_DGRAM with IPPROTO_ICMP), as Android's InetAddress.isReachable opens one: an echo request to
 * 127.0.0.1 and its reply, the identifier the socket's port as getsockname says it and the sequence and data the
 * request's; and a request that is no echo refused. Where the host gives a program without privileges no such socket
 * (Linux's net.ipv4.ping_group_range), that is said and the rest left out; on Windows the core makes it (IcmpSendEcho2)
 * and it must work.
 */
static void test_ping(void)
{
    u8 req[14] = { 8, 0, 0, 0, 0, 0, 0, 7, 'm', 'e', 'g', 'l', 'e', 't' }, reply[64], sa[16], from[28];
    i32 tv[2] = { 2, 0 };
    u32 len, from_len;
    int s = N.socket(AF_INET_, SOCK_DGRAM_, 1 /* IPPROTO_ICMP */), n;
    u16 id;
    say("network: a ping socket: an echo request to 127.0.0.1 and its reply");
#if !defined(_WIN32)
    if (s < 0) {
        say("  (no ping socket for this user here: errno %d)", err());
        return;
    }
#endif
    CHECK(s >= 0);
    if (s < 0) return;
    CHECK(N.setsockopt(s, SOL_SOCKET_, SO_RCVTIMEO_, tv, 8) == 0);
    CHECK_INT(N.sendto(s, req, sizeof(req), 0, sa, sa4(sa, k_lo4, 0)), sizeof(req));
    len = sizeof(sa);
    CHECK(N.getsockname(s, sa, &len) == 0);
    id = port_of(sa);
    from_len = sizeof(from);
    n = N.recvfrom(s, reply, sizeof(reply), 0, from, &from_len);
    CHECK_INT(n, sizeof(req));
    CHECK(reply[0] == 0 && reply[1] == 0 && ((reply[4] << 8) | reply[5]) == id && reply[6] == 0 && reply[7] == 7 && !memcmp(reply + 8, "meglet", 6));
    CHECK(from_len == 16 && !memcmp(from + 4, k_lo4, 4));
    req[0] = 13;
    CHECK(N.sendto(s, req, sizeof(req), 0, sa, sa4(sa, k_lo4, 0)) == -1 && err() == GE_INVAL);
    N.close(s);
}

/* ----------------------------------------------------------------- options */

static void test_options(void)
{
    i32 tv[2], lg[2];
    u32 len;
    char buf[4];
    int a, b, v, s = N.socket(AF_INET6_, SOCK_STREAM_, 0);
    u64 t0;
    say("network: socket options");
    CHECK(s >= 0);
    v = 1;
    CHECK(N.setsockopt(s, SOL_SOCKET_, SO_REUSEADDR_, &v, 4) == 0);
    v = 0;
    len = 4;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_REUSEADDR_, &v, &len) == 0 && v == 1);
    v = 1;
    CHECK(N.setsockopt(s, SOL_SOCKET_, SO_KEEPALIVE_, &v, 4) == 0);
    v = 0;
    len = 4;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_KEEPALIVE_, &v, &len) == 0 && v == 1);
    v = 1;
    CHECK(N.setsockopt(s, IPPROTO_TCP_, TCP_NODELAY_, &v, 4) == 0);
    v = 0;
    len = 4;
    CHECK(N.getsockopt(s, IPPROTO_TCP_, TCP_NODELAY_, &v, &len) == 0 && v == 1);
    v = 60;
    CHECK(N.setsockopt(s, IPPROTO_TCP_, TCP_KEEPIDLE_, &v, 4) == 0);
    v = 0;
    len = 4;
    CHECK(N.getsockopt(s, IPPROTO_TCP_, TCP_KEEPIDLE_, &v, &len) == 0 && v == 60);
    len = 4;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_RCVBUF_, &v, &len) == 0 && v > 0);
    len = 4;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_SNDBUF_, &v, &len) == 0 && v > 0);
    len = 4;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_TYPE_, &v, &len) == 0 && v == SOCK_STREAM_ && len == 4);
    len = 4;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_DOMAIN_, &v, &len) == 0 && v == AF_INET6_);
    len = 4;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_ACCEPTCONN_, &v, &len) == 0 && v == 0);
    /* Linux's default for an IPv6 socket, which Android's code counts on: it takes IPv4 too. */
    len = 4;
    CHECK(N.getsockopt(s, IPPROTO_IPV6_, IPV6_V6ONLY_, &v, &len) == 0 && v == 0);
    lg[0] = 1;
    lg[1] = 5;
    CHECK(N.setsockopt(s, SOL_SOCKET_, SO_LINGER_, lg, 8) == 0);
    memset(lg, 0, sizeof(lg));
    len = 8;
    CHECK(N.getsockopt(s, SOL_SOCKET_, SO_LINGER_, lg, &len) == 0 && len == 8 && lg[0] == 1 && lg[1] == 5);
    /* One with no meaning here is said to be set; one Linux does not know is refused; a value too short too. */
    v = 6;
    CHECK(N.setsockopt(s, SOL_SOCKET_, SO_PRIORITY_, &v, 4) == 0);
    CHECK(N.setsockopt(s, SOL_SOCKET_, 9999, &v, 4) == -1 && err() == GE_NOPROTOOPT);
    CHECK(N.setsockopt(s, SOL_SOCKET_, SO_KEEPALIVE_, &v, 2) == -1 && err() == GE_INVAL);
    CHECK(N.setsockopt(s, SOL_SOCKET_, SO_TYPE_, &v, 4) == -1 && err() == GE_NOPROTOOPT);
    N.close(s);
    /* On something that is not a socket: ENOTSOCK (libcore asks this of every file it closes). */
    {
        int fds[2];
        len = 4;
        CHECK(N.pipe2(fds, 0) == 0 && N.getsockopt(fds[0], SOL_SOCKET_, SO_TYPE_, &v, &len) == -1 && err() == GE_NOTSOCK);
        N.close(fds[0]);
        N.close(fds[1]);
        CHECK(N.getsockopt(fds[0], SOL_SOCKET_, SO_TYPE_, &v, &len) == -1 && err() == GE_BADF);
    }
    /* SO_RCVTIMEO: a read with nothing coming ends after it with EAGAIN. */
    CHECK(tcp_pair(&a, &b));
    tv[0] = 0;
    tv[1] = 150000;
    CHECK(N.setsockopt(a, SOL_SOCKET_, SO_RCVTIMEO_, tv, 8) == 0);
    memset(tv, 0, sizeof(tv));
    len = 8;
    CHECK(N.getsockopt(a, SOL_SOCKET_, SO_RCVTIMEO_, tv, &len) == 0 && tv[0] == 0 && tv[1] == 150000);
    t0 = now_ms();
    CHECK(N.recv(a, buf, 1, 0) == -1 && err() == GE_AGAIN);
    t0 = now_ms() - t0;
    CHECK(t0 >= 130 && t0 < 1500);
    say("  SO_RCVTIMEO of 150 ms: the read gave up after %d ms", (int)t0);
    N.close(a);
    N.close(b);
}

/* --------------------------------------- the descriptor: fstat, fcntl, ioctl */

static void test_descriptor(void)
{
    u8 st[128];
    char buf[8];
    int a, b, n = 0, d;
    say("network: a socket is a descriptor: fstat, fcntl, ioctl, dup, read and write");
    CHECK(tcp_pair(&a, &b));
    memset(st, 0, sizeof(st));
    CHECK(N.fstat(a, st) == 0 && (mg_rd32(st + 16) & S_IFMT_) == S_IFSOCK_);
    CHECK(N.write(a, "abc", 3) == 3);
    /* FIONREAD says what is there once it is. */
    {
        int tries;
        for (tries = 0; tries < 200 && (N.ioctl(b, FIONREAD_, &n) != 0 || n < 3); tries++) mg_sleep_ns(5000000ull);
    }
    CHECK(n == 3);
    CHECK(N.recv(b, buf, 1, MSG_PEEK_) == 1 && buf[0] == 'a' && N.read(b, buf, 3) == 3 && !memcmp(buf, "abc", 3));
    /* A copy of the descriptor is the same socket; closing one leaves the other. */
    d = N.dup2(b, 77);
    CHECK(d == 77 && N.close(b) == 0 && N.write(a, "xy", 2) == 2 && N.read(d, buf, 2) == 2 && !memcmp(buf, "xy", 2));
    CHECK(N.fcntl(d, F_SETFL_, O_NONBLOCK_) == 0 && (N.fcntl(d, F_GETFL_, 0) & O_NONBLOCK_) && N.read(d, buf, 1) == -1 && err() == GE_AGAIN);
    N.close(d);
    N.close(a);
}

/* ------------------------------------------- waiting over sockets with the rest */

typedef struct poke_job {
    int fd;                            /* written to after the delay: 8 bytes for an eventfd, else one */
    int delay_ms;
    bool eventfd;
} poke_job;

static void poke_main(void *arg)
{
    poke_job *j = (poke_job *)arg;
    u64 one = 1;
    mg_sleep_ns((u64)j->delay_ms * 1000000ull);
    N.write(j->fd, j->eventfd ? (const void *)&one : (const void *)"!", j->eventfd ? 8 : 1);
}

/* Which of the three a poke reaches: the poll that waits on all of them ends at once, saying that one only. */
static void poke_and_poll(mg_pollfd *p, int which, int to_fd, bool is_eventfd, const char *what)
{
    poke_job job;
    mg_thread *t;
    u64 t0;
    int r, i;
    job.fd = to_fd;
    job.delay_ms = 80;
    job.eventfd = is_eventfd;
    t = mg_thread_start(poke_main, &job, 128 * 1024, "net-poke");
    t0 = now_ms();
    r = N.poll(p, 3, 5000);
    t0 = now_ms() - t0;
    mg_thread_join(t);
    CHECK(r == 1 && (p[which].revents & POLLIN_) && t0 < 2500);
    for (i = 0; i < 3; i++)
        if (i != which) CHECK(p[i].revents == 0);
    say("  %s written 80 ms into a wait of 5 s: poll ended after %d ms, that one readable", what, (int)t0);
}

static void test_waiting(void)
{
    mg_pollfd p[3];
    t_epoll_event ev, out[4];
    u32 rd[32], wr[32];
    i32 tv[2];
    char buf[8];
    u64 drained, t0;
    int a, b, fds[2], efd, ep, r, top;
    say("network: poll, select and epoll over a socket, a pipe and an eventfd together");
    CHECK(tcp_pair(&a, &b));
    CHECK(N.pipe2(fds, 0) == 0);
    efd = N.eventfd(0, 0);
    CHECK(efd >= 0);
    p[0].fd = b;
    p[1].fd = fds[0];
    p[2].fd = efd;
    p[0].events = p[1].events = p[2].events = POLLIN_;
    t0 = now_ms();
    r = N.poll(p, 3, 100);
    t0 = now_ms() - t0;
    CHECK(r == 0 && t0 >= 90 && t0 < 1500);
    say("  nothing to read: poll of 100 ms ended after %d ms", (int)t0);
    poke_and_poll(p, 0, a, false, "the socket");
    CHECK(N.read(b, buf, 1) == 1);
    poke_and_poll(p, 1, fds[1], false, "the pipe");
    CHECK(N.read(fds[0], buf, 1) == 1);
    poke_and_poll(p, 2, efd, true, "the eventfd");
    CHECK(N.read(efd, &drained, 8) == 8 && drained == 1);

    /* select: the same three, the socket readable; the timeout says what was left. */
    CHECK(N.send(a, "s", 1, 0) == 1);
    top = b > fds[0] ? b : fds[0];
    top = top > efd ? top : efd;
    memset(rd, 0, sizeof(rd));
    rd[b / 32] |= 1u << (b % 32);
    rd[fds[0] / 32] |= 1u << (fds[0] % 32);
    rd[efd / 32] |= 1u << (efd % 32);
    memset(wr, 0, sizeof(wr));
    wr[fds[1] / 32] |= 1u << (fds[1] % 32);
    tv[0] = 2;
    tv[1] = 0;
    r = N.select(top + 1, rd, wr, NULL, tv);
    CHECK(r == 2 && ((rd[b / 32] >> (b % 32)) & 1) && !((rd[fds[0] / 32] >> (fds[0] % 32)) & 1) && !((rd[efd / 32] >> (efd % 32)) & 1));
    CHECK((wr[fds[1] / 32] >> (fds[1] % 32)) & 1);
    CHECK(tv[0] >= 1 && tv[0] <= 2);
    CHECK(N.read(b, buf, 1) == 1);
    /* Nothing: select times out after its time. */
    memset(rd, 0, sizeof(rd));
    rd[b / 32] |= 1u << (b % 32);
    tv[0] = 0;
    tv[1] = 60000;
    t0 = now_ms();
    r = N.select(b + 1, rd, NULL, NULL, tv);
    t0 = now_ms() - t0;
    CHECK(r == 0 && rd[b / 32] == 0 && t0 >= 50 && t0 < 1500 && tv[0] == 0 && tv[1] == 0);
    /* A descriptor that is not open in a set: EBADF before any wait. */
    memset(rd, 0, sizeof(rd));
    rd[900 / 32] |= 1u << (900 % 32);
    CHECK(N.select(901, rd, NULL, NULL, NULL) == -1 && err() == GE_BADF);

    /* epoll: the socket edge-triggered, the pipe and the eventfd level-triggered. */
    ep = N.epoll_create1(0);
    CHECK(ep >= 0);
    ev.events = EPOLLIN_ | (u32)EPOLLET_;
    ev.data = 0x1111222233334444ull;
    CHECK(N.epoll_ctl(ep, EPOLL_CTL_ADD_, b, &ev) == 0);
    ev.events = EPOLLIN_;
    ev.data = 2;
    CHECK(N.epoll_ctl(ep, EPOLL_CTL_ADD_, fds[0], &ev) == 0);
    ev.data = 3;
    CHECK(N.epoll_ctl(ep, EPOLL_CTL_ADD_, efd, &ev) == 0);
    CHECK(N.epoll_ctl(ep, EPOLL_CTL_ADD_, efd, &ev) == -1 && err() == GE_EXIST);
    t0 = now_ms();
    CHECK(N.epoll_wait(ep, out, 4, 80) == 0);
    t0 = now_ms() - t0;
    CHECK(t0 >= 70 && t0 < 1500);
    CHECK(N.send(a, "ep", 2, 0) == 2);
    r = N.epoll_wait(ep, out, 4, 3000);
    CHECK(r == 1 && out[0].events == EPOLLIN_ && out[0].data == 0x1111222233334444ull);
    /* Edge-triggered: not again while nothing new has come, though it is still readable. */
    CHECK(N.epoll_wait(ep, out, 4, 50) == 0);
    CHECK(N.read(b, buf, 2) == 2);
    {
        u64 one = 1;
        CHECK(N.write(efd, &one, 8) == 8);
    }
    r = N.epoll_wait(ep, out, 4, 3000);
    CHECK(r == 1 && out[0].data == 3);
    /* Level-triggered: again, while it is not read. */
    r = N.epoll_wait(ep, out, 4, 0);
    CHECK(r == 1 && out[0].data == 3);
    CHECK(N.read(efd, &drained, 8) == 8);
    N.close(ep);
    N.close(efd);
    N.close(fds[0]);
    N.close(fds[1]);
    N.close(a);
    N.close(b);
}

/* ----------------------------------------------------- the app's own: AF_UNIX */

static void test_unix(void)
{
    mg_pollfd p;
    u8 st[128], sa[112];
    char buf[16];
    t_msghdr m;
    t_iovec iov;
    u32 len;
    int sv[2], v = 0, l, c, s, x;
    say("network: socketpair and named AF_UNIX sockets, the app's own");
    CHECK(N.socketpair(AF_UNIX_, SOCK_STREAM_, 0, sv) == 0);
    CHECK(N.write(sv[0], "ping", 4) == 4 && N.read(sv[1], buf, sizeof(buf)) == 4 && !memcmp(buf, "ping", 4));
    CHECK(N.send(sv[1], "pong", 4, 0) == 4 && N.recv(sv[0], buf, sizeof(buf), 0) == 4 && !memcmp(buf, "pong", 4));
    memset(st, 0, sizeof(st));
    CHECK(N.fstat(sv[0], st) == 0 && (mg_rd32(st + 16) & S_IFMT_) == S_IFSOCK_);
    len = 4;
    CHECK(N.getsockopt(sv[0], SOL_SOCKET_, SO_TYPE_, &v, &len) == 0 && v == SOCK_STREAM_);
    /* Nothing to read: poll waits; written, it ends. */
    p.fd = sv[1];
    p.events = POLLIN_;
    CHECK(N.poll(&p, 1, 30) == 0);
    {
        poke_job job;
        mg_thread *t;
        job.fd = sv[0];
        job.delay_ms = 50;
        job.eventfd = false;
        t = mg_thread_start(poke_main, &job, 128 * 1024, "net-poke");
        CHECK(N.poll(&p, 1, 5000) == 1 && p.revents == POLLIN_);
        mg_thread_join(t);
        CHECK(N.read(sv[1], buf, 1) == 1);
    }
    CHECK(N.fcntl(sv[1], F_SETFL_, O_NONBLOCK_) == 0 && N.read(sv[1], buf, 1) == -1 && err() == GE_AGAIN);
    /* One end shut down for writing: the other reads the end; closed: writing to it is EPIPE. */
    CHECK(N.shutdown(sv[0], SHUT_WR_) == 0 && N.read(sv[1], buf, 1) == 0);
    CHECK(N.close(sv[0]) == 0 && N.write(sv[1], "x", 1) == -1 && err() == GE_PIPE);
    N.close(sv[1]);

    /* Messages keep their bounds, and one too long for the buffer is cut short and said to be. */
    CHECK(N.socketpair(AF_UNIX_, SOCK_SEQPACKET_ | SOCK_CLOEXEC_, 0, sv) == 0);
    CHECK(N.send(sv[0], "ab", 2, 0) == 2 && N.send(sv[0], "cdefgh", 6, 0) == 6);
    CHECK(N.recv(sv[1], buf, sizeof(buf), 0) == 2 && !memcmp(buf, "ab", 2));
    memset(&m, 0, sizeof(m));
    iov.base = buf;
    iov.len = 3;
    m.iov = &iov;
    m.iovlen = 1;
    CHECK(N.recvmsg(sv[1], &m, 0) == 3 && !memcmp(buf, "cde", 3) && (m.flags & MSG_TRUNC_));
    N.close(sv[0]);
    CHECK(N.recv(sv[1], buf, sizeof(buf), 0) == 0);
    N.close(sv[1]);

    /*
     * What libcore does to close a socket another thread is blocked on: a
     * pair, one end shut down both ways and the other closed, put in the
     * socket's place with dup2. The blocked thread then finds the end of a
     * stream, and an accept on it is refused.
     */
    CHECK(N.socketpair(AF_UNIX_, SOCK_STREAM_, 0, sv) == 0);
    CHECK(N.shutdown(sv[0], SHUT_RDWR_) == 0 && N.close(sv[1]) == 0);
    x = N.socket(AF_INET_, SOCK_STREAM_, 0);
    CHECK(N.dup2(sv[0], x) == x && N.close(sv[0]) == 0);
    p.fd = x;
    p.events = POLLIN_;
    CHECK(N.poll(&p, 1, 0) == 1 && (p.revents & POLLIN_) && (p.revents & POLLHUP_));
    CHECK(N.read(x, buf, 1) == 0 && N.accept(x, NULL, NULL) == -1 && err() == GE_INVAL);
    N.close(x);

    /* A name in the abstract space, listened on and connected to. */
    memset(sa, 0, sizeof(sa));
    mg_wr16(sa, AF_UNIX_);
    memcpy(sa + 3, "meglet-net-test", 13);
    l = N.socket(AF_UNIX_, SOCK_STREAM_, 0);
    CHECK(N.bind(l, sa, 2 + 1 + 13) == 0 && N.listen(l, 4) == 0);
    s = N.socket(AF_UNIX_, SOCK_STREAM_, 0);
    CHECK(N.bind(s, sa, 2 + 1 + 13) == -1 && err() == GE_ADDRINUSE);
    CHECK(N.connect(s, sa, 2 + 1 + 13) == 0);
    c = N.accept(l, NULL, NULL);
    CHECK(c >= 0 && N.write(s, "hi", 2) == 2 && N.read(c, buf, 2) == 2 && !memcmp(buf, "hi", 2));
    len = sizeof(sa);
    CHECK(N.getpeername(s, sa, &len) == 0 && len == 2 + 1 + 13 && !memcmp(sa + 3, "meglet-net-test", 13));
    N.close(c);
    N.close(s);
    N.close(l);
    s = N.socket(AF_UNIX_, SOCK_STREAM_, 0);
    CHECK(N.connect(s, sa, 2 + 1 + 13) == -1 && err() == GE_CONNREFUSED);
    N.close(s);
}

/* --------------------------------------------------------------- names */

static void test_names(void)
{
    t_addrinfo hints, *res = NULL, *ai;
    t_hostent *h;
    char host[64], serv[16], text[64];
    u8 sa[16], a6[16], a4[4];
    u32 v4 = 0;
    int r, n;
    say("network: getaddrinfo, getnameinfo and the text of addresses");
    /* "localhost", as libcore's InetAddress asks for it: no network needed, one address, every field as bionic fills it. */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = 0;
    hints.ai_socktype = SOCK_STREAM_;
    hints.ai_flags = AI_ADDRCONFIG_;
    r = N.android_getaddrinfofornet("localhost", NULL, &hints, 0, 0, &res);
    CHECK_INT(r, 0);
    if (!r && res) {
        ai = res;
        CHECK_INT(ai->ai_flags, AI_ADDRCONFIG_);
        CHECK_INT(ai->ai_family, AF_INET_);
        CHECK_INT(ai->ai_socktype, SOCK_STREAM_);
        CHECK_INT(ai->ai_protocol, IPPROTO_TCP_);
        CHECK_INT(ai->ai_addrlen, 16);
        CHECK(ai->ai_canonname == NULL);
        CHECK(ai->ai_addr && mg_rd16(ai->ai_addr) == AF_INET_ && port_of(ai->ai_addr) == 0 && !memcmp(ai->ai_addr + 4, k_lo4, 4));
        CHECK(ai->ai_next == NULL);
        if (sizeof(void *) == 4) {
            /* As the guest's code reads it: bionic's (BSD's) order, the canonical name at 20 and the address at 24. */
            const u32 *w = (const u32 *)(const void *)ai;
            CHECK(w[0] == (u32)AI_ADDRCONFIG_ && w[1] == AF_INET_ && w[2] == SOCK_STREAM_ && w[3] == IPPROTO_TCP_ && w[4] == 16);
            CHECK(w[5] == 0 && w[6] == (u32)(mg_addr)ai->ai_addr && w[7] == 0 && sizeof(t_addrinfo) == 32);
        }
        N.freeaddrinfo(res);
    }
    /* With its canonical name asked for. */
    hints.ai_flags = AI_CANONNAME_;
    res = NULL;
    CHECK(N.getaddrinfo("localhost", "http", &hints, &res) == 0 && res && res->ai_canonname && !strcmp(res->ai_canonname, "localhost") && port_of(res->ai_addr) == 80);
    if (res) N.freeaddrinfo(res);
    /* A number, no type asked for: once as a datagram one and once as a stream one, in that order, with the port. */
    res = NULL;
    r = N.getaddrinfo("192.168.7.9", "8080", NULL, &res);
    CHECK_INT(r, 0);
    for (ai = res, n = 0; ai; ai = ai->ai_next, n++) {
        CHECK(ai->ai_family == AF_INET_ && ai->ai_addrlen == 16 && port_of(ai->ai_addr) == 8080 && ai->ai_addr[4] == 192 && ai->ai_addr[7] == 9);
        CHECK(n == 0 ? ai->ai_socktype == SOCK_DGRAM_ && ai->ai_protocol == IPPROTO_UDP_ : ai->ai_socktype == SOCK_STREAM_ && ai->ai_protocol == IPPROTO_TCP_);
    }
    CHECK_INT(n, 2);
    if (res) N.freeaddrinfo(res);
    /* IPv6 numbers: a scope, and IPv4 written in IPv6. */
    res = NULL;
    hints.ai_flags = 0;
    hints.ai_family = AF_INET6_;
    CHECK(N.getaddrinfo("fe80::1%3", NULL, &hints, &res) == 0 && res && res->ai_family == AF_INET6_ && res->ai_addrlen == 28 && res->ai_addr[8] == 0xFE &&
          res->ai_addr[23] == 1 && mg_rd32(res->ai_addr + 24) == 3);
    if (res) N.freeaddrinfo(res);
    /* Wildcard and loopback when no name is given. */
    res = NULL;
    hints.ai_family = AF_INET_;
    hints.ai_flags = AI_PASSIVE_;
    CHECK(N.getaddrinfo(NULL, "5000", &hints, &res) == 0 && res && mg_rd32(res->ai_addr + 4) == 0 && port_of(res->ai_addr) == 5000 && !res->ai_next);
    if (res) N.freeaddrinfo(res);
    /* What is refused, with bionic's numbers. */
    hints.ai_flags = AI_NUMERICHOST_;
    res = NULL;
    CHECK_INT(N.getaddrinfo("localhost", NULL, &hints, &res), EAI_NONAME_);
    hints.ai_flags = 0x800;
    CHECK_INT(N.getaddrinfo("localhost", NULL, &hints, &res), EAI_BADFLAGS_);
    hints.ai_flags = 0;
    CHECK_INT(N.getaddrinfo("localhost", "no-such-service", &hints, &res), EAI_SERVICE_);
    CHECK(!strcmp(N.gai_strerror(EAI_NONAME_), "Name or service not known") && res == NULL);
    /* getnameinfo: numbers, and the name of the loopback without asking anyone. */
    sa4(sa, k_lo4, 443);
    CHECK(N.getnameinfo(sa, 16, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST_ | NI_NUMERICSERV_) == 0 && !strcmp(host, "127.0.0.1") && !strcmp(serv, "443"));
    CHECK(N.getnameinfo(sa, 16, host, sizeof(host), serv, sizeof(serv), NI_NAMEREQD_) == 0 && !strcmp(host, "localhost") && !strcmp(serv, "https"));
    /* Text and back, as bionic writes it. */
    CHECK(N.inet_pton(AF_INET6_, "2001:db8::1", a6) == 1 && a6[0] == 0x20 && a6[1] == 0x01 && a6[15] == 1);
    CHECK(N.inet_ntop(AF_INET6_, a6, text, sizeof(text)) && !strcmp(text, "2001:db8::1"));
    CHECK(N.inet_pton(AF_INET6_, "::ffff:1.2.3.4", a6) == 1 && N.inet_ntop(AF_INET6_, a6, text, sizeof(text)) && !strcmp(text, "::ffff:1.2.3.4"));
    CHECK(N.inet_pton(AF_INET6_, "1:0:0:2:0:0:0:3", a6) == 1 && N.inet_ntop(AF_INET6_, a6, text, sizeof(text)) && !strcmp(text, "1:0:0:2::3"));
    CHECK(N.inet_pton(AF_INET6_, "::", a6) == 1 && N.inet_ntop(AF_INET6_, a6, text, sizeof(text)) && !strcmp(text, "::"));
    CHECK(N.inet_pton(AF_INET6_, "::1", a6) == 1 && N.inet_ntop(AF_INET6_, a6, text, sizeof(text)) && !strcmp(text, "::1"));
    CHECK(N.inet_pton(AF_INET6_, "1:::2", a6) == 0 && N.inet_pton(AF_INET6_, "12345::", a6) == 0 && N.inet_pton(AF_INET6_, "1:2:3:4:5:6:7:8:9", a6) == 0);
    CHECK(N.inet_pton(AF_INET_, "10.20.30.40", a4) == 1 && a4[0] == 10 && a4[3] == 40 && N.inet_pton(AF_INET_, "1.2.3", a4) == 0 && N.inet_pton(AF_INET_, "01.2.3.4", a4) == 0);
    CHECK(N.inet_pton(99, "1.2.3.4", a4) == -1 && err() == GE_AFNOSUPPORT);
    CHECK(N.inet_ntop(AF_INET6_, a6, text, 3) == NULL && err() == GE_NOSPC);
    CHECK(N.inet_aton("127.1", &v4) == 1 && ((const u8 *)&v4)[0] == 127 && ((const u8 *)&v4)[3] == 1);
    /* gethostbyname, for old code. */
    h = N.gethostbyname("localhost");
    CHECK(h && h->h_addrtype == AF_INET_ && h->h_length == 4 && h->h_addr_list[0] && !memcmp(h->h_addr_list[0], k_lo4, 4) && !h->h_addr_list[1] && !strcmp(h->h_name, "localhost"));
}

/* ------------------------------------------------------------- interfaces */

static void test_interfaces(void)
{
    t_ifaddrs *list = NULL, *f;
    u8 rq[32];
    char name[16];
    bool lo_link = false, lo_v4 = false;
    int s, count = 0;
    u32 index;
    say("network: getifaddrs, if_nametoindex and the ioctls that ask about an interface");
    CHECK(N.getifaddrs(&list) == 0 && list);
    for (f = list; f; f = f->ifa_next) {
        count++;
        if (strcmp(f->ifa_name, "lo")) continue;
        CHECK((f->ifa_flags & (IFF_UP_ | IFF_LOOPBACK_)) == (IFF_UP_ | IFF_LOOPBACK_));
        if (f->ifa_addr && mg_rd16(f->ifa_addr) == 17) lo_link = true;
        if (f->ifa_addr && mg_rd16(f->ifa_addr) == AF_INET_ && !memcmp(f->ifa_addr + 4, k_lo4, 4)) {
            lo_v4 = true;
            CHECK(f->ifa_netmask && mg_rd16(f->ifa_netmask) == AF_INET_ && f->ifa_netmask[4] == 255 && f->ifa_netmask[5] == 0);
        }
    }
    CHECK(lo_link && lo_v4);
    say("  %d entries; the loopback is lo, with 127.0.0.1", count);
    N.freeifaddrs(list);
    index = N.if_nametoindex("lo");
    CHECK(index > 0 && N.if_indextoname(index, name) && !strcmp(name, "lo"));
    CHECK(N.if_nametoindex("no-such0") == 0);
    s = N.socket(AF_INET_, SOCK_DGRAM_, 0);
    memset(rq, 0, sizeof(rq));
    mg_strlcpy((char *)rq, "lo", 16);
    CHECK(N.ioctl(s, SIOCGIFFLAGS_, rq) == 0 && (mg_rd16(rq + 16) & IFF_LOOPBACK_));
    CHECK(N.ioctl(s, SIOCGIFINDEX_, rq) == 0 && mg_rd32(rq + 16) == index);
    mg_strlcpy((char *)rq, "no-such0", 16);
    CHECK(N.ioctl(s, SIOCGIFFLAGS_, rq) == -1 && err() == GE_NODEV);
    N.close(s);
}

/* ------------------------------------------------- EINTR, and closing under a wait */

#if MG_HOST_ARM32
static volatile i32 g_sig_handled;
static void on_signal(int sig)
{
    (void)sig;
    mg_atomic_add(&g_sig_handled, 1);
}

typedef struct blocked_job {
    int fd;
    bool accept;
    mg_gthread *self;
    volatile i32 started;
    int result, error;
} blocked_job;

static void blocked_main(void *arg)
{
    blocked_job *j = (blocked_job *)arg;
    char buf[4];
    j->self = mg_gthread_self();
    mg_atomic_xchg(&j->started, 1);
    j->result = j->accept ? N.accept(j->fd, NULL, NULL) : N.recv(j->fd, buf, sizeof(buf), 0);
    j->error = err();
}

/*
 * A thread blocked in accept, or in a read, is sent a signal whose handler did
 * not ask for SA_RESTART: its call ends with EINTR. That is how libcore's
 * AsynchronousCloseMonitor gets a thread out of a socket that is being
 * closed, with signal 34, one of the real-time ones; the descriptor has by
 * then been made the end of a pair, as test_unix does.
 */
static void test_interrupted(void)
{
    t_sigaction sa, old;
    blocked_job job;
    mg_thread *t;
    u16 port = 0;
    int l = listener(false, &port), a, b, k;
    say("network: a thread blocked on a socket is sent signal 34: EINTR, as libcore's closing of a socket under it needs");
    memset(&sa, 0, sizeof(sa));
    sa.handler = (u32)(mg_addr)on_signal;
    CHECK(N.sigaction(34, &sa, &old) == 0);
    for (k = 0; k < 2; k++) {
        memset(&job, 0, sizeof(job));
        if (k == 0) {
            job.fd = l;
            job.accept = true;
        } else {
            CHECK(tcp_pair(&a, &b));
            job.fd = a;
        }
        g_sig_handled = 0;
        t = mg_thread_start(blocked_main, &job, 256 * 1024, "net-blocked");
        while (!job.started) mg_sleep_ns(1000000ull);
        mg_sleep_ns(100000000ull);
        CHECK_INT(mg_signal_send(job.self, 34), 0);
        mg_thread_join(t);
        CHECK(job.result == -1 && job.error == GE_INTR && g_sig_handled == 1);
        if (k == 1) {
            N.close(a);
            N.close(b);
        }
    }
    N.sigaction(34, &old, NULL);
    N.close(l);
}
#endif

/* ------------------------------------------------- the check for a phone
 *
 * The network as an app finds it, through the C library, a line for every
 * step: mg_net_check_run, which a phone runs (meglet_net_check in meglet.h), and
 * the tests run here without its part that goes to the internet.
 */

static bool g_check_quiet;
static int g_refused;

static void step_said(bool ok, int e, const char *fmt, ...)
{
    char text[400], why[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    t_checks++;
    if (!ok) t_failed++;
    snprintf(why, sizeof(why), " -- errno %d", e);
    if (!g_check_quiet || !ok) say("  %s %s%s", ok ? "ok  " : "FAIL", text, ok ? "" : why);
}

#define STEP(cond, ...)                    \
    do {                                   \
        bool ok_ = (cond);                 \
        int err_ = err();                  \
        step_said(ok_, err_, __VA_ARGS__); \
    } while (0)

static void noted(const char *fmt, ...)
{
    char text[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (!g_check_quiet) say("       %s", text);
}

/* Refused by the phone, not failed here: said plainly, and counted apart. */
static void refused_said(const char *fmt, ...)
{
    char text[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    t_checks++;
    g_refused++;
    say("  REFUSED BY THE PHONE %s", text);
}

/* Waits for `events` on one descriptor, up to `ms`: whether they came. */
static bool wait_for(int fd, int events, int ms)
{
    mg_pollfd p;
    p.fd = fd;
    p.events = (i16)events;
    return N.poll(&p, 1, ms) == 1 && (p.revents & events);
}

/* A connection, not blocking, given `ms` to be made: the descriptor, or -1 with errno. */
static int connect_within(const u8 *sa, u32 len, int ms)
{
    int s = N.socket(mg_rd16(sa), SOCK_STREAM_ | SOCK_NONBLOCK_, 0), v = 0;
    u32 vlen = 4;
    if (s < 0) return -1;
    if (N.connect(s, sa, len) != 0 && err() != GE_INPROGRESS) goto fail;
    if (!wait_for(s, POLLOUT_, ms)) {
        mg_set_errno(GE_TIMEDOUT);
        goto fail;
    }
    if (N.getsockopt(s, SOL_SOCKET_, SO_ERROR_, &v, &vlen) != 0) goto fail;
    if (v) {
        mg_set_errno(v);
        goto fail;
    }
    N.fcntl(s, F_SETFL_, 0);
    return s;
fail:
    v = err();
    N.close(s);
    mg_set_errno(v);
    return -1;
}

/* A line read up to its CR LF, waiting up to `ms`: its length, or -1. */
static int read_line(int fd, char *out, int cap, int ms)
{
    int n = 0;
    u64 end = now_ms() + (u64)ms;
    while (n < cap - 1) {
        u64 now = now_ms();
        i32 r;
        if (now >= end || !wait_for(fd, POLLIN_, (int)(end - now))) {
            mg_set_errno(GE_TIMEDOUT);
            return -1;
        }
        r = N.recv(fd, out + n, 1, 0);
        if (r <= 0) return -1;
        if (out[n] == '\n') break;
        n++;
    }
    out[n] = 0;
    if (n && out[n - 1] == '\r') out[--n] = 0;
    return n;
}

static void check_internet(const char *host, const char *path)
{
    t_addrinfo hints, *res = NULL, *ai;
    char line[200], request[300];
    int r, v4 = 0, v6 = 0, s = -1;
    u64 t0 = now_ms();
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM_;
    hints.ai_flags = AI_ADDRCONFIG_;
    r = N.getaddrinfo(host, "80", &hints, &res);
    for (ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET_) v4++;
        else v6++;
    }
    STEP(r == 0 && res, "%s resolves: %d IPv4 and %d IPv6 addresses, %d ms%s%s", host, v4, v6, (int)(now_ms() - t0), r ? " -- " : "", r ? N.gai_strerror(r) : "");
    for (ai = res; ai && s < 0; ai = ai->ai_next) {
        t0 = now_ms();
        s = connect_within(ai->ai_addr, ai->ai_addrlen, 10000);
        if (s < 0) noted("a connection over %s to port 80 failed (errno %d) after %d ms", ai->ai_family == AF_INET_ ? "IPv4" : "IPv6", err(), (int)(now_ms() - t0));
        else noted("connected over %s in %d ms", ai->ai_family == AF_INET_ ? "IPv4" : "IPv6", (int)(now_ms() - t0));
    }
    if (res) N.freeaddrinfo(res);
    STEP(s >= 0, "a TCP connection to %s, port 80", host);
    if (s < 0) return;
    snprintf(request, sizeof(request), "HEAD %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Meglet\r\nConnection: close\r\n\r\n", path, host);
    t0 = now_ms();
    STEP(N.send(s, request, (u32)strlen(request), 0) == (i32)strlen(request), "a HEAD request for %s sent", path);
    r = read_line(s, line, sizeof(line), 10000);
    STEP(r > 0 && !strncmp(line, "HTTP/1.", 7), "the answer's status line: \"%s\", %d ms", r > 0 ? line : "", (int)(now_ms() - t0));
    N.close(s);
}

typedef struct serve_job {
    int listener;
    int result;
} serve_job;

/* The check's own server: one connection, one request line, one answer. */
static void serve_main(void *arg)
{
    serve_job *j = (serve_job *)arg;
    char line[100];
    int c;
    j->result = -1;
    if (!wait_for(j->listener, POLLIN_, 5000)) return;
    c = N.accept(j->listener, NULL, NULL);
    if (c < 0) return;
    if (read_line(c, line, sizeof(line), 5000) > 0 && !strncmp(line, "HEAD / HTTP/1.", 14)) {
        static const char answer[] = "HTTP/1.0 200 OK\r\nServer: meglet-net-check\r\n\r\n";
        /* The request read through to its blank line first: a socket closed with something unread in it is reset, and its answer lost. */
        while (read_line(c, line, sizeof(line), 5000) > 0) { }
        j->result = N.send(c, answer, sizeof(answer) - 1, 0) == (i32)(sizeof(answer) - 1) ? 0 : -1;
    }
    N.close(c);
}

static void check_loopback_tcp(void)
{
    serve_job job;
    mg_thread *t;
    char line[100];
    u8 sa[16];
    u16 port = 0;
    int l = listener(false, &port), s, e, r;
    STEP(l >= 0, "a TCP server of the check's own, listening on 127.0.0.1 port %u", (unsigned)port);
    if (l < 0) return;
    job.listener = l;
    job.result = -1;
    t = mg_thread_start(serve_main, &job, 256 * 1024, "net-check-server");
    s = connect_within(sa, sa4(sa, k_lo4, port), 5000);
    e = err();
    if (s < 0 && (e == GE_ACCES || e == GE_TIMEDOUT || e == GE_CONNREFUSED || e == GE_NETUNREACH || e == GE_PERM)) {
        refused_said("an app's connection to its own server on loopback (errno %d): this phone does not let an app reach itself over 127.0.0.1", e);
        mg_thread_join(t);
        N.close(l);
        return;
    }
    STEP(s >= 0, "connected to it");
    if (s >= 0) {
        static const char request[] = "HEAD / HTTP/1.0\r\n\r\n";
        STEP(N.send(s, request, sizeof(request) - 1, 0) == (i32)(sizeof(request) - 1), "a HEAD request sent to it");
        r = read_line(s, line, sizeof(line), 5000);
        STEP(r > 0 && !strcmp(line, "HTTP/1.0 200 OK"), "its answer: \"%s\"", r > 0 ? line : "");
        N.close(s);
    }
    mg_thread_join(t);
    STEP(job.result == 0, "and it read the request and answered");
    N.close(l);
}

static void check_loopback_udp(void)
{
    u8 sa_a[16], sa_b[16], from[16];
    char buf[16];
    u32 len, from_len = sizeof(from);
    int a = N.socket(AF_INET_, SOCK_DGRAM_, 0), b = N.socket(AF_INET_, SOCK_DGRAM_, 0);
    bool bound = a >= 0 && b >= 0 && N.bind(a, sa_a, sa4(sa_a, k_lo4, 0)) == 0 && N.bind(b, sa_b, sa4(sa_b, k_lo4, 0)) == 0;
    len = sizeof(sa_b);
    bound = bound && N.getsockname(b, sa_b, &len) == 0;
    STEP(bound, "two UDP sockets bound on 127.0.0.1");
    if (bound) {
        bool got;
        STEP(N.sendto(a, "ping", 4, 0, sa_b, 16) == 4, "a datagram sent from one to the other");
        got = wait_for(b, POLLIN_, 3000) && N.recvfrom(b, buf, sizeof(buf), 0, from, &from_len) == 4 && !memcmp(buf, "ping", 4);
        if (!got && err() != 0) refused_said("a datagram between two of an app's own sockets on loopback did not arrive (errno %d)", err());
        else if (!got) refused_said("a datagram between two of an app's own sockets on loopback did not arrive in 3 s");
        else {
            STEP(true, "it arrived, from the first one's port");
            STEP(N.sendto(b, "pong", 4, 0, from, from_len) == 4 && wait_for(a, POLLIN_, 3000) && N.recv(a, buf, sizeof(buf), 0) == 4 && !memcmp(buf, "pong", 4),
                 "and the answer came back");
        }
    }
    if (a >= 0) N.close(a);
    if (b >= 0) N.close(b);
}

static void check_interfaces(void)
{
    t_ifaddrs *list = NULL, *f, *g;
    char text[300];
    size_t at = 0;
    bool lo = false;
    STEP(N.getifaddrs(&list) == 0, "the interfaces are listed");
    text[0] = 0;
    for (f = list; f; f = f->ifa_next) {
        int addrs = 0;
        bool first = true;
        if (!f->ifa_addr || mg_rd16(f->ifa_addr) != 17) continue;
        for (g = list; g; g = g->ifa_next) {
            if (g == f && first) first = false;
            if (!strcmp(g->ifa_name, f->ifa_name) && g->ifa_addr && mg_rd16(g->ifa_addr) != 17) addrs++;
        }
        if (!strcmp(f->ifa_name, "lo")) lo = true;
        at += (size_t)snprintf(text + at, at < sizeof(text) ? sizeof(text) - at : 0, "%s%s (%d address%s%s)", at ? ", " : "", f->ifa_name, addrs, addrs == 1 ? "" : "es",
                               (f->ifa_flags & 0x41) == 0x41 ? ", up" : "");
        if (at >= sizeof(text)) break;
    }
    noted("%s", text);
    STEP(lo, "the loopback is among them, as lo");
    if (list) N.freeifaddrs(list);
}

int mg_net_check_run(const char *public_host, const char *path, mg_test_sink sink, void *user, int *checks, int *refused)
{
    int was_checks = t_checks, was_failed = t_failed, failed;
    mg_test_sink was_sink = t_sink;
    void *was_user = t_sink_user;
    t_sink = sink;
    t_sink_user = user;
    t_failed = 0;
    g_refused = 0;
    if (!libc_found()) {
        t_checks++;
        t_failed++;
        say("  FAIL the C library's network functions are not there: is the core started?");
    } else {
        t_addrinfo *res = NULL;
        STEP(N.getaddrinfo("localhost", NULL, NULL, &res) == 0 && res, "\"localhost\" resolves without asking the network");
        if (res) N.freeaddrinfo(res);
        check_interfaces();
        if (public_host) check_internet(public_host, path ? path : "/");
        else noted("(the internet is left out: no public name was given)");
        check_loopback_tcp();
        check_loopback_udp();
    }
    failed = t_failed;
    if (checks) *checks += t_checks - was_checks;
    if (refused) *refused = g_refused;
    t_failed = was_failed + failed;
    t_sink = was_sink;
    t_sink_user = was_user;
    return failed;
}

/* ------------------------------------------------------------------- run */

int mg_net_tests_run(mg_test_sink sink, void *user, int *checks)
{
    int was_checks = t_checks, was_failed = t_failed, refused = 0;
    u8 sa[16];
    int probe, s;
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    if (!libc_found()) {
        CHECK(libc_found());
        goto out;
    }
    /*
     * A phone may refuse an app its own loopback. There it is said once, and
     * what needs loopback left out, rather than counted as this layer's
     * failure; anywhere else (this PC, qemu) it is a failure.
     */
    {
        u16 port = 0;
        int e;
        probe = listener(false, &port);
        s = probe >= 0 ? connect_within(sa, sa4(sa, k_lo4, port), 3000) : -1;
        e = err();
        if (s < 0) {
            bool phone = !strcmp(mg_host_name(), "win32-arm");
            say("network: THIS HOST REFUSES AN APP ITS OWN LOOPBACK (errno %d): the tests that need it are left out%s", e, phone ? "" : ", and that is a failure here");
            if (!phone) CHECK(s >= 0);
            if (probe >= 0) N.close(probe);
            test_names();
            test_interfaces();
            test_unix();
            goto out;
        }
        N.close(s);
        N.close(probe);
    }
    test_tcp_blocking(false);
    test_tcp_blocking(true);
    test_nonblocking_connect();
    test_accept_timeout();
    test_large(false);
    test_large(true);
    test_shutdown();
    test_udp();
    test_ping();
    test_options();
    test_descriptor();
    test_waiting();
    test_unix();
    test_names();
    test_interfaces();
#if MG_HOST_ARM32
    test_interrupted();
#else
    say("network: (the test of a signal ending a wait on a socket needs a 32-bit host: sigaction's handler is 32 bits)");
#endif
    say("network: the check a phone runs, without the internet");
    g_check_quiet = true;
    {
        int before = t_failed;
        mg_net_check_run(NULL, NULL, t_sink, t_sink_user, NULL, &refused);
        g_check_quiet = false;
        CHECK_INT(t_failed - before, 0);
        CHECK_INT(refused, 0);
    }
out:
    if (checks) *checks += t_checks;
    {
        int failed = t_failed;
        t_checks = was_checks;
        t_failed = was_failed;
        return failed;
    }
}
