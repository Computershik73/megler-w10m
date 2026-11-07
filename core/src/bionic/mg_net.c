/*
 * Sockets, as Android's C library has them, over the host's (host/mg_host.h).
 *
 * A socket is one of the layer's descriptors (mg_libc_io.c): read, write,
 * close, dup, fcntl, ioctl and fstat work on it as on a file, and poll, select
 * and epoll wait on it together with pipes, eventfds and files. The host's
 * sockets never block. What blocks is this file, which waits the way every
 * descriptor of the layer waits -- on the table's one condition -- and the
 * host's notice that some socket has changed (mg_sock_set_notify) is
 * announced on that condition like a pipe being written to. That is also what
 * lets a signal end the wait: libcore closes a socket that another thread is
 * blocked on by sending that thread signal 34, and the call ends with EINTR
 * as on Linux. SO_RCVTIMEO and SO_SNDTIMEO are this file's waits' limits.
 *
 * Internet sockets (AF_INET, AF_INET6) are the host's. Linux's numbers --
 * families, types, options, flags, errors -- and its address structures are
 * turned into the host's neutral ones here and back. Sockets of AF_UNIX are
 * this file's own, inside the process: socketpair, and names bound, listened
 * on and connected to within the app (nothing outside it could be reached on
 * a phone anyway). Libraries make a pair as a wake-up channel, and libcore
 * makes one to stand in for a socket it closes under a blocked thread.
 *
 * No ancillary data: what sendmsg is given beside the data is not sent, and
 * recvmsg returns none -- no descriptors pass over AF_UNIX.
 */
#include "mg_net.h"

#include <stdarg.h>
#include <stdio.h>

#include "../mg_core.h"

#include "mg_fd.h"

#define G_SOCK_NONBLOCK 04000
#define G_SOCK_CLOEXEC 02000000
#define G_O_NONBLOCK 04000
#define G_O_CLOEXEC 02000000

#define G_SOL_SOCKET 1
#define G_SO_DEBUG 1
#define G_SO_REUSEADDR 2
#define G_SO_TYPE 3
#define G_SO_ERROR 4
#define G_SO_DONTROUTE 5
#define G_SO_BROADCAST 6
#define G_SO_SNDBUF 7
#define G_SO_RCVBUF 8
#define G_SO_KEEPALIVE 9
#define G_SO_OOBINLINE 10
#define G_SO_NO_CHECK 11
#define G_SO_PRIORITY 12
#define G_SO_LINGER 13
#define G_SO_BSDCOMPAT 14
#define G_SO_REUSEPORT 15
#define G_SO_PASSCRED 16
#define G_SO_PEERCRED 17
#define G_SO_RCVLOWAT 18
#define G_SO_SNDLOWAT 19
#define G_SO_RCVTIMEO 20
#define G_SO_SNDTIMEO 21
#define G_SO_BINDTODEVICE 25
#define G_SO_TIMESTAMP 29
#define G_SO_ACCEPTCONN 30
#define G_SO_SNDBUFFORCE 32
#define G_SO_RCVBUFFORCE 33
#define G_SO_MARK 36
#define G_SO_PROTOCOL 38
#define G_SO_DOMAIN 39

#define G_IPPROTO_IP 0
#define G_IPPROTO_ICMP 1
#define G_IPPROTO_TCP 6
#define G_IPPROTO_UDP 17
#define G_IPPROTO_IPV6 41
#define G_IPPROTO_ICMPV6 58

#define G_TCP_NODELAY 1
#define G_TCP_MAXSEG 2
#define G_TCP_KEEPIDLE 4
#define G_TCP_KEEPINTVL 5
#define G_TCP_KEEPCNT 6

#define G_IP_TOS 1
#define G_IP_TTL 2
#define G_IP_MULTICAST_IF 32
#define G_IP_MULTICAST_TTL 33
#define G_IP_MULTICAST_LOOP 34
#define G_IP_ADD_MEMBERSHIP 35
#define G_IP_DROP_MEMBERSHIP 36
#define G_MCAST_JOIN_GROUP 42
#define G_MCAST_LEAVE_GROUP 45

#define G_IPV6_UNICAST_HOPS 16
#define G_IPV6_MULTICAST_IF 17
#define G_IPV6_MULTICAST_HOPS 18
#define G_IPV6_MULTICAST_LOOP 19
#define G_IPV6_JOIN_GROUP 20
#define G_IPV6_LEAVE_GROUP 21
#define G_IPV6_V6ONLY 26
#define G_IPV6_TCLASS 67

#define G_MSG_OOB 1
#define G_MSG_PEEK 2
#define G_MSG_DONTROUTE 4
#define G_MSG_TRUNC 0x20
#define G_MSG_DONTWAIT 0x40
#define G_MSG_WAITALL 0x100
#define G_MSG_ERRQUEUE 0x2000

#define G_POLLIN 1
#define G_POLLOUT 4
#define G_POLLERR 8
#define G_POLLHUP 0x10
#define G_POLLRDNORM 0x40
#define G_POLLWRNORM 0x100
#define G_POLLRDHUP 0x2000

#define G_S_IFSOCK 0140000u

/* Linux's default room in a socket's buffer: how much one of the app's own (AF_UNIX) holds before a writer waits. */
#define LOCAL_ROOM 212992u

/* Guest structures. Pointers and size_t are the core's own, which are the guest's: 32 bits on the phone, 64 in 64-bit Android. */
typedef struct g_iovec {
    void *base;
    size_t len;
} g_iovec;

typedef struct g_msghdr {
    void *name;
    u32 namelen;
    g_iovec *iov;
    size_t iovlen;
    void *control;
    size_t controllen;
    int flags;
} g_msghdr;

typedef struct g_mmsghdr {
    g_msghdr hdr;
    u32 len;
} g_mmsghdr;

/* ------------------------------------------------------------- the socket */

enum { S_FRESH, S_LISTENING, S_CONNECTING, S_CONNECTED, S_FAILED };

typedef struct gsock {
    int family, type, protocol;        /* Android's numbers; the protocol the type's own when 0 was asked */
    int state;                         /* S_*: of a stream or seqpacket socket */
    bool shut_rd, shut_wr;
    u64 rcvtimeo, sndtimeo;            /* SO_RCVTIMEO, SO_SNDTIMEO in ns; 0: none */
    int keepidle, keepintvl, keepcnt;  /* what was set where the host has no such option, to be said back */
    int rcvbuf, sndbuf;                /* AF_UNIX: what was asked, said back */
    bool first, first_own;             /* the app's first connection, its outcome not yet said (first_connection_ended) */
    bool traced;                       /* a connection the flag trace-net is to say the outcome of */
    char trace_to[56];
    u64 trace_from;
    /* trace-net, once a traced connection is made: what went over it, said when it is closed (io_said). */
    bool io_traced;
    u64 io_from, io_first_in;          /* when it was made; when its first bytes came in (0: none yet) */
    u64 sent, received;
    u32 sends, recvs, would_block, timeouts;
    u64 last_out, last_in;             /* when bytes last went out, came in (0: never) */
    u64 said_sent, said_received;      /* what the last round of open connections said of it (open_round) */
    u32 said_blocked;
    bool said_open;
    int last_send_err, last_recv_err;
    bool got_end;                      /* a read returned 0: the other end has closed or shut down writing */
    u8 head_out[16], head_in[16];      /* the first bytes each way, for what the protocol is */
    u8 head_out_len, head_in_len;
    u16 first_port;
    mg_sock *host;                     /* AF_INET and AF_INET6 */
    /* AF_UNIX, the app's own: all of what follows under the descriptor table's lock. */
    struct gsock *peer;                /* the other end, while both are open */
    bool peer_done;                    /* the other end writes no more: it shut down writing, or is closed */
    bool peer_closed;                  /* it is closed: what is written here would go nowhere (EPIPE) */
    u8 *rbuf;                          /* what there is to read: a ring */
    u32 rcap, rhead, rcount;
    u32 *msgs;                         /* datagram and seqpacket: the length of each message in the ring, a ring as well */
    u32 mcap, mhead, mcount;
    char *name;                        /* the name it is bound to: sun_path's bytes, an abstract one beginning with a NUL */
    u32 namelen;
    char *to;                          /* a datagram socket connected to a name: that name */
    u32 tolen;
    struct gsock **queue;              /* listening: connections made and not yet accepted */
    int nqueue, backlog;
    struct gsock *next_bound;
} gsock;

static i32 sock_read(void *obj, void *buf, u32 n, bool nonblock);
static i32 sock_write(void *obj, const void *buf, u32 n, bool nonblock);
static int sock_poll(void *obj, int events);
static int sock_readable(void *obj);
static void sock_close(void *obj);
static int sock_ioctl(void *obj, unsigned request, void *arg);

static const mg_fd_ops k_sock_ops = { sock_read, sock_write, sock_poll, sock_readable, NULL, sock_close, sock_ioctl, G_S_IFSOCK, true };

static int fail(int e)
{
    mg_set_errno(e);
    return -1;
}

static int gerr(int e)
{
    switch (e) {
    case MG_NE_OK: return 0;
    case MG_NE_AGAIN: return GE_AGAIN;
    case MG_NE_INPROGRESS: return GE_INPROGRESS;
    case MG_NE_ALREADY: return GE_ALREADY;
    case MG_NE_ISCONN: return GE_ISCONN;
    case MG_NE_NOTCONN: return GE_NOTCONN;
    case MG_NE_CONNREFUSED: return GE_CONNREFUSED;
    case MG_NE_CONNRESET: return GE_CONNRESET;
    case MG_NE_CONNABORTED: return GE_CONNABORTED;
    case MG_NE_TIMEDOUT: return GE_TIMEDOUT;
    case MG_NE_NETUNREACH: return GE_NETUNREACH;
    case MG_NE_HOSTUNREACH: return GE_HOSTUNREACH;
    case MG_NE_NETDOWN: return GE_NETDOWN;
    case MG_NE_NETRESET: return GE_NETRESET;
    case MG_NE_ADDRINUSE: return GE_ADDRINUSE;
    case MG_NE_ADDRNOTAVAIL: return GE_ADDRNOTAVAIL;
    case MG_NE_AFNOSUPPORT: return GE_AFNOSUPPORT;
    case MG_NE_PROTONOSUPPORT: return GE_PROTONOSUPPORT;
    case MG_NE_SOCKTNOSUPPORT: return GE_SOCKTNOSUPPORT;
    case MG_NE_OPNOTSUPP: return GE_OPNOTSUPP;
    case MG_NE_NOPROTOOPT: return GE_NOPROTOOPT;
    case MG_NE_INVAL: return GE_INVAL;
    case MG_NE_ACCES: return GE_ACCES;
    case MG_NE_MSGSIZE: return GE_MSGSIZE;
    case MG_NE_NOBUFS: return GE_NOBUFS;
    case MG_NE_MFILE: return GE_MFILE;
    case MG_NE_SHUTDOWN: return GE_PIPE;
    case MG_NE_PIPE: return GE_PIPE;
    case MG_NE_DESTADDRREQ: return GE_DESTADDRREQ;
    case MG_NE_FAULT: return GE_FAULT;
    default: return GE_IO;
    }
}

static const char *errno_name(int e)
{
    switch (e) {
    case 0: return "made";
    case GE_CONNREFUSED: return "refused (ECONNREFUSED)";
    case GE_TIMEDOUT: return "timed out (ETIMEDOUT)";
    case GE_NETUNREACH: return "no route to the network (ENETUNREACH)";
    case GE_HOSTUNREACH: return "no route to the host (EHOSTUNREACH)";
    case GE_ACCES: return "refused by the system (EACCES)";
    case GE_INPROGRESS: return "still under way when the app's SO_SNDTIMEO ran out (EINPROGRESS)";
    case GE_INTR: return "cut short by a signal (EINTR)";
    default: return "failed";
    }
}

/* An option this layer does not pass on to the host (Windows has nothing like it, or nothing an app could notice): the app is told it was set, and the log says so once. */
static int quietly(const char *what)
{
    mg_note_once("network: the socket option %s is not passed on to the host here; the app is told it was set", what);
    return 0;
}

static void net_start(void)
{
    static volatile i32 started;
    if (!started && !mg_atomic_xchg(&started, 1)) mg_sock_set_notify(mg_fd_notify);
}

static const char *kind_of(int family, int type)
{
    if (family == G_AF_UNIX) return type == G_SOCK_STREAM ? "a stream socket of the app's own (AF_UNIX)" : "a message socket of the app's own (AF_UNIX)";
    if (type == G_SOCK_STREAM) return family == G_AF_INET6 ? "TCP over IPv6 (which reaches IPv4 too)" : "TCP over IPv4";
    if (type == G_SOCK_DGRAM) return family == G_AF_INET6 ? "UDP over IPv6 (which reaches IPv4 too)" : "UDP over IPv4";
    return "a raw socket";
}

static void said_first_socket(const gsock *s)
{
    static volatile i32 said;
    if (!said && !mg_atomic_xchg(&said, 1)) mg_info("network: the app's first socket: %s", kind_of(s->family, s->type));
}

/*
 * The app's first TCP connection, said once its outcome is known: at once for
 * a blocking one; for one made without blocking (libcore makes every one so,
 * and waits with poll) when the app learns it -- SO_ERROR, a second connect,
 * or the first send or receive. The port only, and whether it is this phone's
 * own address: nothing of the network's.
 */
static volatile i32 g_first_connection;    /* 0: none yet; 1: one under way, not yet said; 2: said */

static bool own_address(const mg_sockaddr *a)
{
    static const u8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF }, one[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    if (a->family == MG_AF_INET) return a->addr[0] == 127;
    return a->family == MG_AF_INET6 && (!memcmp(a->addr, one, 16) || (!memcmp(a->addr, mapped, 12) && a->addr[12] == 127));
}

/*
 * The flag trace-net (a file of that name in LocalState\flags, read once): every TCP connection the app makes is said in the
 * log, where to and how it ended, and every name it looks up (mg_netdb.c). An app whose request goes nowhere often says
 * nothing about it; this says whether a connection was made at all. At most 1000 lines a run.
 */
bool mg_net_trace_wanted(void)
{
    static int wanted = -1;
    if (wanted < 0) wanted = mg_flag("trace-net") ? 1 : 0;
    return wanted != 0;
}

void mg_net_trace(const char *fmt, ...)
{
    static volatile i32 said;
    char line[400];
    va_list ap;
    if (!mg_net_trace_wanted() || mg_atomic_add(&said, 1) > 1000) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    mg_info("network: %s", line);
}

/* An address and port as text: a.b.c.d:port, or [v6]:port. */
static void addr_text(const mg_sockaddr *a, char *out, size_t size)
{
    if (a->family == MG_AF_INET6) {
        size_t k = (size_t)snprintf(out, size, "[");
        int i;
        for (i = 0; i < 16 && k + 6 < size; i += 2) k += (size_t)snprintf(out + k, size - k, "%s%x", i ? ":" : "", (unsigned)(a->addr[i] << 8 | a->addr[i + 1]));
        snprintf(out + k, size - k, "]:%u", (unsigned)a->port);
    } else {
        snprintf(out, size, "%u.%u.%u.%u:%u", a->addr[0], a->addr[1], a->addr[2], a->addr[3], (unsigned)a->port);
    }
}

/* What a traced socket's lines call it. */
static const char *proto_of(const gsock *s) { return s->type == G_SOCK_STREAM ? "TCP" : "UDP"; }

static void first_connection_began(gsock *s, const mg_sockaddr *a)
{
    if (mg_net_trace_wanted()) {
        addr_text(a, s->trace_to, sizeof(s->trace_to));
        s->trace_from = mg_time_monotonic_ns();
        s->traced = true;
    }
    if (g_first_connection || mg_atomic_cas(&g_first_connection, 0, 1) != 0) return;
    s->first = true;
    s->first_port = a->port;
    s->first_own = own_address(a);
}

/*
 * trace-net: the traced connections that are open, and a thread that says every twenty seconds what has gone over each
 * since it last said it -- a connection an app keeps open is otherwise said only when it is closed, which may be never,
 * and an app that stands still waits on one of them: a request sent and no answer, or nothing sent at all. A connection
 * whose counts have not changed is not said again. At most 64 connections are followed at once.
 */
static mg_lock g_open_lock = MG_LOCK_INIT;
static gsock *g_open[64];

static void open_round(void)
{
    u64 now = mg_time_monotonic_ns();
    size_t i;
    mg_lock_acquire(&g_open_lock);
    for (i = 0; i < MG_COUNT(g_open); i++) {
        gsock *s = g_open[i];
        char out_ago[40] = "", in_ago[40] = "", errs[64] = "";
        if (!s || (s->said_open && s->said_sent == s->sent && s->said_received == s->received && s->said_blocked == s->would_block)) continue;
        s->said_open = true;
        s->said_sent = s->sent;
        s->said_received = s->received;
        s->said_blocked = s->would_block;
        if (s->last_out) snprintf(out_ago, sizeof(out_ago), ", the last %llu s ago", (unsigned long long)((now - s->last_out) / 1000000000ull));
        if (s->last_in) snprintf(in_ago, sizeof(in_ago), ", the last %llu s ago", (unsigned long long)((now - s->last_in) / 1000000000ull));
        if (s->last_send_err || s->last_recv_err) snprintf(errs, sizeof(errs), "; the last errors: send %d, receive %d", s->last_send_err, s->last_recv_err);
        mg_net_trace("%s to %s: open for %llu s; sent %llu bytes in %u calls%s; received %llu bytes in %u calls%s%s; %u reads or writes would have blocked%s",
                     proto_of(s), s->trace_to, (unsigned long long)((now - s->io_from) / 1000000000ull), (unsigned long long)s->sent, s->sends, out_ago,
                     (unsigned long long)s->received, s->recvs, in_ago, s->got_end ? ", then the other end's close" : "", s->would_block, errs);
    }
    mg_lock_release(&g_open_lock);
}

static MG_HOSTCB void open_observer(void *unused)
{
    (void)unused;
    for (;;) {
        mg_sleep_ns(20000000000ull);
        open_round();
    }
}

static void open_add(gsock *s)
{
    static volatile i32 started;
    size_t i;
    mg_lock_acquire(&g_open_lock);
    for (i = 0; i < MG_COUNT(g_open); i++)
        if (!g_open[i]) {
            g_open[i] = s;
            break;
        }
    mg_lock_release(&g_open_lock);
    if (!mg_atomic_xchg(&started, 1)) {
        mg_thread *t = mg_thread_start(open_observer, NULL, 64u * 1024u, "net-open-observer");
        if (t) mg_thread_detach(t);
    }
}

static void open_remove(gsock *s)
{
    size_t i;
    mg_lock_acquire(&g_open_lock);
    for (i = 0; i < MG_COUNT(g_open); i++)
        if (g_open[i] == s) g_open[i] = NULL;
    mg_lock_release(&g_open_lock);
}

/* `how`: the call the app learnt it by, which says what it does next -- a send, or waiting to be answered. */
static void first_connection_ended(gsock *s, int e, const char *how)
{
    if (s->traced) {
        s->traced = false;
        mg_net_trace("TCP to %s: %s after %llu ms (the app learnt it by %s)", s->trace_to, errno_name(e),
                     (unsigned long long)((mg_time_monotonic_ns() - s->trace_from) / 1000000), how);
        if (!e) {
            s->io_traced = true;
            s->io_from = mg_time_monotonic_ns();
            open_add(s);
        }
    }
    if (!s->first) return;
    s->first = false;
    mg_atomic_xchg(&g_first_connection, 2);
    mg_info("network: the app's first connection, TCP to port %u%s: %s", (unsigned)s->first_port, s->first_own ? " of this phone itself" : "", errno_name(e));
}

/*
 * trace-net: what a send or a receive on a traced connection did (`r` its result, errno its error), counted for io_said. The
 * counts are a diagnosis's, kept without a lock: two threads at one socket at once may each miss the other's last call.
 */
static void io_note(gsock *s, bool sending, const void *buf, i32 r)
{
    u8 *head = sending ? s->head_out : s->head_in, *len = sending ? &s->head_out_len : &s->head_in_len;
    if (!s->io_traced) return;
    /* The first bytes each way are said as they go, the first 120 as text (what is not printable a dot): a connection the app keeps open says at once who has not spoken. */
    if (r > 0 && buf && (sending ? !s->sends : !s->recvs)) {
        char text[124];
        i32 i, n = r < 120 ? r : 120;
        for (i = 0; i < n; i++) {
            u8 c = ((const u8 *)buf)[i];
            text[i] = c >= 32 && c < 127 ? (char)c : '.';
        }
        text[n] = 0;
        mg_net_trace("%s to %s: the first bytes %s, %d of them, %llu ms after it was made: %s", proto_of(s), s->trace_to, sending ? "sent" : "received", (int)r,
                     (unsigned long long)((mg_time_monotonic_ns() - s->io_from) / 1000000), text);
    }
    if (r > 0) {
        u32 take = (u32)r < (u32)(sizeof(s->head_out) - *len) ? (u32)r : (u32)(sizeof(s->head_out) - *len);
        if (take && buf) memcpy(head + *len, buf, take);
        *len = (u8)(*len + take);
        if (sending) {
            s->sent += (u32)r;
            s->sends++;
            s->last_out = mg_time_monotonic_ns();
        } else {
            s->last_in = mg_time_monotonic_ns();
            if (!s->io_first_in) s->io_first_in = s->last_in;
            s->received += (u32)r;
            s->recvs++;
        }
    } else if (r == 0 && !sending) {
        s->got_end = true;
    } else if (r < 0) {
        int e = *mg_errno_ptr();
        if (e == GE_AGAIN && (sending ? s->sndtimeo : s->rcvtimeo)) s->timeouts++;
        else if (e == GE_AGAIN) s->would_block++;
        else if (sending) s->last_send_err = e;
        else s->last_recv_err = e;
    }
}

static void hex_of(const u8 *p, u32 n, char *out, size_t size)
{
    u32 i;
    size_t k = 0;
    out[0] = 0;
    for (i = 0; i < n && k + 3 < size; i++) k += (size_t)snprintf(out + k, size - k, "%02x", p[i]);
    if (!n) snprintf(out, size, "none");
}

/* trace-net: a traced connection closed by the app -- how long it lasted and what went over it. */
static void io_said(gsock *s)
{
    char out[40], in[40], send_err[40] = "", recv_err[40] = "", first_in[40] = "";
    u64 now = mg_time_monotonic_ns();
    if (!s->io_traced) return;
    s->io_traced = false;
    open_remove(s);
    hex_of(s->head_out, s->head_out_len, out, sizeof(out));
    hex_of(s->head_in, s->head_in_len, in, sizeof(in));
    if (s->last_send_err) snprintf(send_err, sizeof(send_err), ", the last send's error %d", s->last_send_err);
    if (s->last_recv_err) snprintf(recv_err, sizeof(recv_err), ", the last receive's error %d", s->last_recv_err);
    if (s->io_first_in) snprintf(first_in, sizeof(first_in), ", %llu ms after it was made", (unsigned long long)((s->io_first_in - s->io_from) / 1000000));
    mg_net_trace("%s to %s: closed by the app after %llu ms; sent %llu bytes in %u calls (first %s)%s; received %llu bytes in %u calls (first %s%s)%s%s; "
                 "%u reads or writes would have blocked, %u ran out of SO_RCVTIMEO/SO_SNDTIMEO (%llu/%llu ms)",
                 proto_of(s), s->trace_to, (unsigned long long)((now - s->io_from) / 1000000), (unsigned long long)s->sent, s->sends, out, send_err,
                 (unsigned long long)s->received, s->recvs, in, first_in, s->got_end ? ", then the other end's close" : "", recv_err,
                 s->would_block, s->timeouts, (unsigned long long)(s->rcvtimeo / 1000000), (unsigned long long)(s->sndtimeo / 1000000));
}

static u64 deadline_of(u64 timeo) { return timeo ? mg_time_monotonic_ns() + timeo : 0; }

/* The socket behind a descriptor, kept until mg_fd_let_go(held); NULL with errno EBADF or ENOTSOCK. */
static gsock *hold(int fd, void **held, bool *nonblock)
{
    int flags = 0;
    gsock *s = (gsock *)mg_fd_ext_hold(fd, &k_sock_ops, held, &flags);
    if (nonblock) *nonblock = (flags & G_O_NONBLOCK) != 0;
    return s;
}

static gsock *sock_new(int family, int type, int protocol)
{
    gsock *s = (gsock *)mg_alloc(sizeof(*s));
    s->family = family;
    s->type = type;
    s->protocol = protocol;
    return s;
}

/* A descriptor for it, with SOCK_NONBLOCK and SOCK_CLOEXEC as asked; -1 (and the socket closed) when there is no room. */
static int sock_register(gsock *s, int flags)
{
    int fd = mg_fd_register_with(&k_sock_ops, s, 2 | ((flags & G_SOCK_NONBLOCK) ? G_O_NONBLOCK : 0) | ((flags & G_SOCK_CLOEXEC) ? G_O_CLOEXEC : 0));
    if (fd < 0) sock_close(s);
    return fd;
}

/* ------------------------------------------------- addresses, Android's way */

static void put_port(u8 *p, u16 port)
{
    p[0] = (u8)(port >> 8);
    p[1] = (u8)port;
}

u32 mg_net_sockaddr_out(const mg_sockaddr *a, bool v6, u8 *out)
{
    static const u8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    memset(out, 0, G_SIN6_LEN);
    if (v6) {
        mg_wr16(out, G_AF_INET6);
        put_port(out + 2, a->port);
        if (a->family == MG_AF_INET6) {
            mg_wr32(out + 4, a->flowinfo);
            memcpy(out + 8, a->addr, 16);
            mg_wr32(out + 24, a->scope_id);
        } else if (a->family == MG_AF_INET) {
            memcpy(out + 8, mapped, 12);
            memcpy(out + 20, a->addr, 4);
        }
        return G_SIN6_LEN;
    }
    mg_wr16(out, G_AF_INET);
    put_port(out + 2, a->port);
    if (a->family == MG_AF_INET) memcpy(out + 4, a->addr, 4);
    else if (a->family == MG_AF_INET6 && !memcmp(a->addr, mapped, 12)) memcpy(out + 4, a->addr + 12, 4);
    return G_SIN_LEN;
}

int mg_net_sockaddr_in(const void *sa, u32 len, mg_sockaddr *a)
{
    const u8 *p = (const u8 *)sa;
    u16 f;
    memset(a, 0, sizeof(*a));
    if (!p) return GE_FAULT;
    if (len < 2) return GE_INVAL;
    f = mg_rd16(p);
    if (f == G_AF_INET || f == G_AF_UNSPEC) {
        if (len < G_SIN_LEN) return f == G_AF_UNSPEC ? 0 : GE_INVAL;
        a->family = f == G_AF_INET ? MG_AF_INET : 0;
        a->port = (u16)((p[2] << 8) | p[3]);
        memcpy(a->addr, p + 4, 4);
        return 0;
    }
    if (f == G_AF_INET6) {
        /* (The scope was not there in RFC 2133's: a structure 24 bytes long is taken too, as Linux takes it.) */
        if (len < 24) return GE_INVAL;
        a->family = MG_AF_INET6;
        a->port = (u16)((p[2] << 8) | p[3]);
        a->flowinfo = mg_rd32(p + 4);
        memcpy(a->addr, p + 8, 16);
        a->scope_id = len >= G_SIN6_LEN ? mg_rd32(p + 24) : 0;
        return 0;
    }
    return GE_AFNOSUPPORT;
}

void mg_net_give(const u8 *what, u32 n, void *out, u32 *len)
{
    if (!out || !len) return;
    memcpy(out, what, *len < n ? *len : n);
    *len = n;
}

/* An address as a socket of this family takes it: an IPv6 one IPv4 addresses (`mapped`: for sending, as ::ffff:a.b.c.d), an IPv4 one only its own. */
static int addr_fits(const gsock *s, mg_sockaddr *a, bool mapped)
{
    if (s->family == G_AF_INET6) {
        if (a->family == MG_AF_INET && mapped) {
            u8 v4[4];
            memcpy(v4, a->addr, 4);
            memset(a->addr, 0, 16);
            a->addr[10] = a->addr[11] = 0xFF;
            memcpy(a->addr + 12, v4, 4);
            a->family = MG_AF_INET6;
        }
        return a->family == MG_AF_INET6 ? 0 : GE_AFNOSUPPORT;
    }
    return a->family == MG_AF_INET ? 0 : GE_AFNOSUPPORT;
}

static void give_addr(const gsock *s, const mg_sockaddr *a, void *out, u32 *len)
{
    u8 buf[G_SIN6_LEN];
    u32 n = mg_net_sockaddr_out(a, s->family == G_AF_INET6, buf);
    mg_net_give(buf, n, out, len);
}

/* An AF_UNIX socket's name as a sockaddr_un: the family alone when it has none (Linux's "unnamed"), with the NUL after a path. */
static void give_local_name(const char *name, u32 namelen, void *out, u32 *len)
{
    u8 buf[2 + 110];
    u32 n = 2;
    mg_wr16(buf, G_AF_UNIX);
    if (name && namelen) {
        memcpy(buf + 2, name, namelen);
        n += namelen;
        if (name[0]) buf[n++] = 0;
    }
    mg_net_give(buf, n, out, len);
}

/* --------------------------------------------- the app's own sockets: AF_UNIX
 *
 * Everything below is done with the descriptor table's lock held, and waits
 * on its condition (mg_fd.h), so that poll sees these change as it sees pipes.
 */

static gsock *g_bound;                 /* sockets bound to a name */

static u32 local_room(const gsock *p) { return p->rcount >= LOCAL_ROOM ? 0 : LOCAL_ROOM - p->rcount; }

static void ring_put(gsock *s, const u8 *p, u32 n)
{
    u32 tail, first;
    if (!n) return;
    if (s->rcount + n > s->rcap) {
        u32 cap = s->rcap ? s->rcap : 4096, k;
        u8 *grown;
        while (cap < s->rcount + n) cap *= 2;
        grown = (u8 *)mg_alloc(cap);
        for (k = 0; k < s->rcount; k++) grown[k] = s->rbuf[(s->rhead + k) % s->rcap];
        free(s->rbuf);
        s->rbuf = grown;
        s->rcap = cap;
        s->rhead = 0;
    }
    tail = (s->rhead + s->rcount) % s->rcap;
    first = n < s->rcap - tail ? n : s->rcap - tail;
    memcpy(s->rbuf + tail, p, first);
    memcpy(s->rbuf, p + first, n - first);
    s->rcount += n;
}

/* The first `n` bytes to read, copied into `out` when it is not NULL, and taken away when `consume`. */
static void ring_take(gsock *s, u8 *out, u32 n, bool consume)
{
    if (!n) return;
    if (out) {
        u32 first = n < s->rcap - s->rhead ? n : s->rcap - s->rhead;
        memcpy(out, s->rbuf + s->rhead, first);
        memcpy(out + first, s->rbuf, n - first);
    }
    if (consume) {
        s->rhead = (s->rhead + n) % s->rcap;
        s->rcount -= n;
        if (!s->rcount) s->rhead = 0;
    }
}

static void msg_put(gsock *s, const u8 *p, u32 n)
{
    if (s->mcount == s->mcap) {
        u32 cap = s->mcap ? s->mcap * 2 : 16, k;
        u32 *grown = (u32 *)mg_alloc(sizeof(u32) * cap);
        for (k = 0; k < s->mcount; k++) grown[k] = s->msgs[(s->mhead + k) % s->mcap];
        free(s->msgs);
        s->msgs = grown;
        s->mcap = cap;
        s->mhead = 0;
    }
    s->msgs[(s->mhead + s->mcount) % s->mcap] = n;
    s->mcount++;
    ring_put(s, p, n);
}

static gsock *find_bound(const char *name, u32 namelen)
{
    gsock *b;
    for (b = g_bound; b; b = b->next_bound)
        if (b->namelen == namelen && !memcmp(b->name, name, namelen)) return b;
    return NULL;
}

/* Under the lock: what poll says of one of the app's own sockets. */
static int local_poll(const gsock *s)
{
    int r = 0;
    bool connected_kind = s->type != G_SOCK_DGRAM;
    if (s->state == S_LISTENING) return s->nqueue ? G_POLLIN | G_POLLRDNORM : 0;
    if ((s->type == G_SOCK_STREAM ? s->rcount : s->mcount) || s->peer_done || s->shut_rd) r |= G_POLLIN | G_POLLRDNORM;
    if (s->peer_done || s->shut_rd) r |= G_POLLRDHUP;
    /* (Writable also where writing fails at once: unconnected, shut down, the other end gone.) */
    if (!s->peer || s->shut_wr || local_room(s->peer) > 0) r |= G_POLLOUT | G_POLLWRNORM;
    if (s->peer_closed || (s->shut_rd && s->shut_wr) || (connected_kind && s->state == S_FRESH)) r |= G_POLLHUP;
    return r;
}

/* The lock is held; a wait for a change, as long as `deadline` allows: 0 to look again, or the errno that ends the call. */
static int local_wait(const gsock *s, u64 deadline, u64 timeo)
{
    u64 left = 0;
    int w;
    if (deadline) {
        u64 now = mg_time_monotonic_ns();
        if (now >= deadline) return GE_AGAIN;
        left = deadline - now;
    }
    w = mg_fd_wait_locked(left);
    (void)s;
    if (w == MG_FD_INTERRUPTED || (w == MG_FD_HANDLER_RAN && timeo)) return GE_INTR;
    return 0;
}

static i32 local_send(gsock *s, const u8 *buf, u32 n, int flags, const char *to, u32 tolen, bool nonblock)
{
    u64 deadline = deadline_of(s->sndtimeo);
    u32 done = 0;
    int e = 0;
    bool msg = s->type != G_SOCK_STREAM;
    if (flags & G_MSG_DONTWAIT) nonblock = true;
    mg_fd_lock();
    for (;;) {
        gsock *p;
        if (s->shut_wr) {
            e = GE_PIPE;
            break;
        }
        if (to && s->type != G_SOCK_DGRAM) {
            e = s->peer ? GE_ISCONN : GE_OPNOTSUPP;
            break;
        }
        if (!to && !s->peer && s->to) {
            to = s->to;
            tolen = s->tolen;
        }
        p = to ? find_bound(to, tolen) : s->peer;
        if (to && (!p || p->type != G_SOCK_DGRAM)) {
            e = to[0] ? GE_NOENT : GE_CONNREFUSED;
            break;
        }
        if (!p) {
            e = s->peer_closed ? GE_PIPE : GE_NOTCONN;
            break;
        }
        if (p->shut_rd && !msg) {
            e = GE_PIPE;
            break;
        }
        if (msg) {
            if (n > LOCAL_ROOM) {
                e = GE_MSGSIZE;
                break;
            }
            if (local_room(p) >= n || !p->rcount) {
                msg_put(p, buf, n);
                done = n;
                mg_fd_notify_locked();
                break;
            }
        } else {
            u32 room = local_room(p), take = n - done < room ? n - done : room;
            if (take) {
                ring_put(p, buf + done, take);
                done += take;
                mg_fd_notify_locked();
            }
            if (done == n) break;
        }
        if (nonblock) {
            if (!done) e = GE_AGAIN;
            break;
        }
        if ((e = local_wait(s, deadline, s->sndtimeo)) != 0) {
            if (done) e = 0;
            break;
        }
    }
    mg_fd_unlock();
    return e ? fail(e) : (i32)done;
}

static i32 local_recv(gsock *s, u8 *buf, u32 n, int flags, int *said, bool nonblock)
{
    u64 deadline = deadline_of(s->rcvtimeo);
    u32 done = 0;
    int e = 0;
    bool peek = (flags & G_MSG_PEEK) != 0, all = (flags & G_MSG_WAITALL) && s->type == G_SOCK_STREAM && !peek;
    if (flags & G_MSG_DONTWAIT) nonblock = true;
    mg_fd_lock();
    for (;;) {
        if (s->state == S_LISTENING) {
            e = GE_INVAL;
            break;
        }
        if (s->type == G_SOCK_STREAM && s->rcount) {
            u32 take = n - done < s->rcount ? n - done : s->rcount;
            ring_take(s, buf + done, take, !peek);
            done += take;
            if (!peek) mg_fd_notify_locked();
            if (!all || done == n) break;
            continue;
        }
        if (s->type != G_SOCK_STREAM && s->mcount) {
            u32 len = s->msgs[s->mhead], take = n < len ? n : len;
            ring_take(s, buf, take, false);
            if (!peek) {
                ring_take(s, NULL, len, true);
                s->mhead = (s->mhead + 1) % s->mcap;
                s->mcount--;
                mg_fd_notify_locked();
            }
            if (take < len && said) *said = 1;
            done = take;
            break;
        }
        if (s->peer_done || s->shut_rd) break;          /* the end */
        if (s->type != G_SOCK_DGRAM && s->state != S_CONNECTED) {
            e = GE_INVAL;
            break;
        }
        if (nonblock) {
            if (!done) e = GE_AGAIN;
            break;
        }
        if ((e = local_wait(s, deadline, s->rcvtimeo)) != 0) {
            if (done) e = 0;
            break;
        }
    }
    mg_fd_unlock();
    return e ? fail(e) : (i32)done;
}

/* A socket of the app's own that is going: whoever it was connected to sees the end, and connections no one accepted are let go. Takes the lock. */
static void local_close(gsock *s)
{
    gsock **at;
    int i;
    mg_fd_lock();
    if (s->peer) {
        s->peer->peer = NULL;
        s->peer->peer_done = s->peer->peer_closed = true;
    }
    for (at = &g_bound; *at; at = &(*at)->next_bound) {
        if (*at == s) {
            *at = s->next_bound;
            break;
        }
    }
    for (i = 0; i < s->nqueue; i++) {
        gsock *q = s->queue[i];
        if (q->peer) {
            q->peer->peer = NULL;
            q->peer->peer_done = q->peer->peer_closed = true;
        }
        free(q->rbuf);
        free(q->msgs);
        free(q->name);
        free(q);
    }
    mg_fd_notify_locked();
    mg_fd_unlock();
    free(s->queue);
    free(s->rbuf);
    free(s->msgs);
    free(s->name);
    free(s->to);
}

/* sun_path as a name's bytes: a path up to its NUL, an abstract name (a NUL first) as long as the length says. */
static int local_name_of(const void *sa, u32 len, const char **name, u32 *namelen)
{
    const char *p = (const char *)sa + 2;
    u32 n;
    if (!sa) return GE_FAULT;
    if (len < 2 || len > 110 || mg_rd16(sa) != G_AF_UNIX) return GE_INVAL;
    n = len - 2;
    if (n && p[0]) {
        u32 k = 0;
        while (k < n && p[k]) k++;
        n = k;
    }
    *name = p;
    *namelen = n;
    return 0;
}

static int local_bind(gsock *s, const void *sa, u32 len)
{
    const char *name;
    u32 namelen;
    char made[6];
    int e = local_name_of(sa, len, &name, &namelen);
    if (e) return e;
    mg_fd_lock();
    if (s->name) {
        e = GE_INVAL;
    } else {
        if (!namelen) {
            /* As Linux does with no name: one of its own choosing, abstract, five hex digits. */
            static u32 next = 0x10000;
            do {
                made[0] = 0;
                snprintf(made + 1, sizeof(made) - 1, "%05x", (unsigned)(next++ & 0xFFFFF));
            } while (find_bound(made, 6));
            name = made;
            namelen = 6;
        }
        if (find_bound(name, namelen)) {
            e = GE_ADDRINUSE;
        } else {
            s->name = (char *)mg_alloc(namelen + 1);
            memcpy(s->name, name, namelen);
            s->namelen = namelen;
            s->next_bound = g_bound;
            g_bound = s;
        }
    }
    mg_fd_unlock();
    return e;
}

static int local_connect(gsock *s, const void *sa, u32 len, bool nonblock)
{
    const char *name;
    u32 namelen;
    u64 deadline = deadline_of(s->sndtimeo);
    int e = local_name_of(sa, len, &name, &namelen);
    if (e) return e;
    mg_fd_lock();
    if (s->type == G_SOCK_DGRAM) {
        /* A datagram socket is only given a default destination. */
        gsock *b = find_bound(name, namelen);
        if (!b || b->type != G_SOCK_DGRAM) e = name[0] ? GE_NOENT : GE_CONNREFUSED;
        else {
            free(s->to);
            s->to = (char *)mg_alloc(namelen + 1);
            memcpy(s->to, name, namelen);
            s->tolen = namelen;
        }
        mg_fd_unlock();
        return e;
    }
    for (;;) {
        gsock *l = find_bound(name, namelen), *q;
        if (s->state == S_CONNECTED) e = GE_ISCONN;
        else if (s->state == S_LISTENING) e = GE_INVAL;
        else if (!l) e = name[0] ? GE_NOENT : GE_CONNREFUSED;
        else if (l->type != s->type) e = GE_PROTOTYPE;
        else if (l->state != S_LISTENING) e = GE_CONNREFUSED;
        if (e) break;
        if (l->nqueue < (l->backlog > 0 ? l->backlog : 1)) {
            /* The listener's end, waiting to be accepted: named as the listener is, as Linux names it. */
            q = sock_new(G_AF_UNIX, s->type, 0);
            q->state = S_CONNECTED;
            q->peer = s;
            s->peer = q;
            s->state = S_CONNECTED;
            if (l->name) {
                q->name = (char *)mg_alloc(l->namelen + 1);
                memcpy(q->name, l->name, l->namelen);
                q->namelen = l->namelen;
            }
            l->queue = (gsock **)mg_realloc(l->queue, sizeof(gsock *) * (size_t)(l->nqueue + 1));
            l->queue[l->nqueue++] = q;
            mg_fd_notify_locked();
            break;
        }
        if (nonblock) {
            e = GE_AGAIN;
            break;
        }
        /* (The listener may be gone when this comes back: it is looked for by its name again.) */
        if ((e = local_wait(s, deadline, s->sndtimeo)) != 0) break;
    }
    mg_fd_unlock();
    return e;
}

/* ----------------------------------------------- the host's sockets: waiting */

/*
 * Waits as every descriptor of the layer waits until something of `want`
 * (MG_SOCK_*) holds of a host socket -- or an error or a hang-up, which end
 * any wait -- `deadline` passes, or a signal's handler cuts the wait short. 0
 * to try again, or the errno: EAGAIN when the socket's own timeout ran out,
 * EINTR for a handler that did not ask for SA_RESTART, or for any handler when
 * the socket has a timeout (as Linux does). The state is looked at under the
 * lock the host's notice also takes, so a change after the look is not missed.
 */
static int sock_wait(gsock *s, int want, u64 deadline, u64 timeo)
{
    int w;
    u64 left = 0;
    mg_fd_lock();
    if (mg_sock_ready(s->host) & (want | MG_SOCK_ERR | MG_SOCK_HUP)) {
        mg_fd_unlock();
        return 0;
    }
    if (deadline) {
        u64 now = mg_time_monotonic_ns();
        if (now >= deadline) {
            mg_fd_unlock();
            return GE_AGAIN;
        }
        left = deadline - now;
    }
    w = mg_fd_wait_locked(left);
    mg_fd_unlock();
    if (w == MG_FD_INTERRUPTED || (w == MG_FD_HANDLER_RAN && timeo)) return GE_INTR;
    return 0;
}

/* A connection under way that has since been made: it is connected. (One that failed stays under way until its error is asked for.) */
static void settle(gsock *s, const char *how)
{
    if (s->state == S_CONNECTING && s->host) {
        int h = mg_sock_ready(s->host);
        if ((h & MG_SOCK_OUT) && !(h & (MG_SOCK_ERR | MG_SOCK_HUP))) {
            s->state = S_CONNECTED;
            first_connection_ended(s, 0, how);
        }
    }
}

static int host_flags(int flags)
{
    return ((flags & G_MSG_PEEK) ? MG_MSG_PEEK : 0) | ((flags & G_MSG_OOB) ? MG_MSG_OOB : 0) | ((flags & G_MSG_DONTROUTE) ? MG_MSG_DONTROUTE : 0);
}

static i32 host_send(gsock *s, const u8 *buf, u32 n, int flags, const mg_sockaddr *to, bool nonblock)
{
    u64 deadline = deadline_of(s->sndtimeo);
    u32 done = 0;
    if (flags & G_MSG_DONTWAIT) nonblock = true;
    if (s->shut_wr) return fail(GE_PIPE);
    settle(s, "a send");
    for (;;) {
        int err = 0, e;
        i32 r = mg_sock_send(s->host, buf + done, n - done, host_flags(flags), to, &err);
        if (r >= 0) {
            done += (u32)r;
            /* (Blocking, a stream's send goes on until all of it is taken, as Linux's does.) */
            if (s->type != G_SOCK_STREAM || done >= n || nonblock) return (i32)done;
        } else if (err != MG_NE_AGAIN || nonblock) {
            if (done) return (i32)done;
            if (err == MG_NE_NOTCONN) return fail(s->type == G_SOCK_STREAM ? GE_PIPE : GE_DESTADDRREQ);
            return fail(gerr(err));
        }
        e = sock_wait(s, MG_SOCK_OUT, deadline, s->sndtimeo);
        if (e) return done ? (i32)done : fail(e);
    }
}

static i32 host_recv(gsock *s, u8 *buf, u32 n, int flags, mg_sockaddr *from, int *said, bool nonblock)
{
    u64 deadline = deadline_of(s->rcvtimeo);
    u32 done = 0;
    bool stream = s->type == G_SOCK_STREAM, all = (flags & G_MSG_WAITALL) && stream && !(flags & G_MSG_PEEK);
    if (flags & G_MSG_DONTWAIT) nonblock = true;
    settle(s, "a receive");
    for (;;) {
        int err = 0, e;
        i32 r = mg_sock_recv(s->host, buf + done, n - done, host_flags(flags), stream ? NULL : from, said, &err);
        /* (A datagram socket's peer refused what it was sent: Linux says ECONNREFUSED, Windows WSAECONNRESET.) */
        if (r < 0 && !stream && err == MG_NE_CONNRESET) err = MG_NE_CONNREFUSED;
        if (r >= 0) {
            if (!stream || r == 0) return (i32)(done + (u32)r);
            done += (u32)r;
            if (!all || done >= n || nonblock) return (i32)done;
        } else if (err == MG_NE_SHUTDOWN && stream) {
            /* Windows, after shutdown(SHUT_RD): Linux reads what is left and then the end. */
            return (i32)done;
        } else if (err != MG_NE_AGAIN || nonblock) {
            if (done) return (i32)done;
            return fail(gerr(err));
        }
        e = sock_wait(s, MG_SOCK_IN | MG_SOCK_RDHUP, deadline, s->rcvtimeo);
        if (e) return done ? (i32)done : fail(e);
    }
}

/* ------------------------------------------------------------- send, recv */

static i32 send_any(gsock *s, const void *buf, u32 n, int flags, const void *to, u32 tolen, bool nonblock)
{
    mg_sockaddr a;
    int e;
    if (!s->host) {
        const char *name = NULL;
        u32 namelen = 0;
        if (to && (e = local_name_of(to, tolen, &name, &namelen)) != 0) return fail(e);
        return local_send(s, (const u8 *)buf, n, flags, name, namelen, nonblock);
    }
    if (to && s->type == G_SOCK_STREAM) to = NULL;    /* (Linux ignores an address given to a connected stream) */
    if (to) {
        if ((e = mg_net_sockaddr_in(to, tolen, &a)) != 0 || (e = addr_fits(s, &a, true)) != 0) return fail(e);
    }
    {
        i32 r = host_send(s, (const u8 *)buf, n, flags, to ? &a : NULL, nonblock);
        io_note(s, true, buf, r);
        return r;
    }
}

/* `from`, `fromlen`: where the sender's address goes; a stream has none to say (length 0). `said`: 1 when a message was cut short. */
static i32 recv_any(gsock *s, void *buf, u32 n, int flags, void *from, u32 *fromlen, int *said, bool nonblock)
{
    mg_sockaddr a;
    int cut = 0;
    i32 r;
    memset(&a, 0, sizeof(a));
    if (said) *said = 0;
    /*
     * The socket's error queue (MSG_ERRQUEUE, which IP_RECVERR asks Linux to fill; setsockopt takes that option and passes
     * it on to nothing): always empty here, and Linux answers an empty queue with EAGAIN at once. Taken for an ordinary
     * receive, it took the next datagram, the one the app was waiting for: Facebook's QUIC (folly's AsyncUDPSocket) empties
     * the queue before every read, and every answer of the server went there; its connections timed out.
     */
    if (flags & G_MSG_ERRQUEUE) return fail(GE_AGAIN);
    if (!s->host) {
        r = local_recv(s, (u8 *)buf, n, flags, &cut, nonblock);
        if (r >= 0 && from && fromlen) {
            if (s->type == G_SOCK_STREAM) *fromlen = 0;
            else give_local_name(NULL, 0, from, fromlen);
        }
    } else {
        r = host_recv(s, (u8 *)buf, n, flags, &a, &cut, nonblock);
        if (!(flags & G_MSG_PEEK)) io_note(s, false, buf, r);
        if (r >= 0 && from && fromlen) {
            if (s->type == G_SOCK_STREAM || !a.family) *fromlen = 0;
            else give_addr(s, &a, from, fromlen);
        }
    }
    if (said) *said = cut;
    return r;
}

static i32 sock_read(void *obj, void *buf, u32 n, bool nonblock) { return recv_any((gsock *)obj, buf, n, 0, NULL, NULL, NULL, nonblock); }
static i32 sock_write(void *obj, const void *buf, u32 n, bool nonblock) { return send_any((gsock *)obj, buf, n, 0, NULL, 0, nonblock); }

#define HOLD_OR_FAIL(fd, s, held, nonblock, fail_value) \
    void *held;                                         \
    bool nonblock;                                      \
    gsock *s = hold(fd, &held, &nonblock);              \
    if (!s) return fail_value;

/* (A size_t count, of which as much as a read or write moves at once is taken: mg_rw_count.) */
static gssize g_sendto(int fd, const void *buf, size_t n, int flags, const void *to, u32 tolen)
{
    i32 r;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    r = send_any(s, buf, mg_rw_count(n), flags, to, tolen, nonblock);
    mg_fd_moved(held);
    mg_fd_let_go(held);
    return r;
}

static gssize g_send(int fd, const void *buf, size_t n, int flags) { return g_sendto(fd, buf, n, flags, NULL, 0); }

static gssize g_recvfrom(int fd, void *buf, size_t n, int flags, void *from, u32 *fromlen)
{
    i32 r;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    r = recv_any(s, buf, mg_rw_count(n), flags, from, fromlen, NULL, nonblock);
    mg_fd_moved(held);
    mg_fd_let_go(held);
    return r;
}

static gssize g_recv(int fd, void *buf, size_t n, int flags) { return g_recvfrom(fd, buf, n, flags, NULL, NULL); }

/* The checked forms a library built with _FORTIFY_SOURCE calls: the same, the buffer's size being the caller's business. */
static gssize g_sendto_chk(int fd, const void *buf, size_t n, size_t buflen, int flags, const void *to, u32 tolen)
{
    (void)buflen;
    return g_sendto(fd, buf, n, flags, to, tolen);
}
static gssize g_recvfrom_chk(int fd, void *buf, size_t n, size_t buflen, int flags, void *from, u32 *fromlen)
{
    (void)buflen;
    return g_recvfrom(fd, buf, n, flags, from, fromlen);
}

static u32 iov_total(const g_msghdr *m)
{
    size_t i;
    u32 total = 0;
    for (i = 0; i < m->iovlen; i++) total += (u32)m->iov[i].len;
    return total;
}

static i32 sendmsg_held(gsock *s, const g_msghdr *m, int flags, bool nonblock)
{
    u32 total, at = 0;
    size_t i;
    u8 *all;
    i32 r;
    if (!m || (m->iovlen && !m->iov)) return fail(GE_FAULT);
    if (m->controllen) mg_note_once("network: sendmsg was given ancillary data (descriptors or credentials); it is not sent");
    if (m->iovlen == 1) return send_any(s, m->iov[0].base, (u32)m->iov[0].len, flags, m->name, m->namelen, nonblock);
    /* Several pieces are one message (and one write): gathered first. */
    total = iov_total(m);
    all = (u8 *)mg_alloc(total ? total : 1);
    for (i = 0; i < m->iovlen; i++) {
        memcpy(all + at, m->iov[i].base, m->iov[i].len);
        at += (u32)m->iov[i].len;
    }
    r = send_any(s, all, total, flags, m->name, m->namelen, nonblock);
    free(all);
    return r;
}

static i32 recvmsg_held(gsock *s, g_msghdr *m, int flags, bool nonblock)
{
    u32 total, at = 0, fromlen;
    size_t i;
    int said = 0;
    u8 *all;
    i32 r;
    if (!m || (m->iovlen && !m->iov)) return fail(GE_FAULT);
    fromlen = m->namelen;
    total = iov_total(m);
    all = m->iovlen == 1 ? (u8 *)m->iov[0].base : (u8 *)mg_alloc(total ? total : 1);
    r = recv_any(s, all, total, flags, m->name, m->name ? &fromlen : NULL, &said, nonblock);
    if (r >= 0) {
        if (m->iovlen != 1) {
            for (i = 0; i < m->iovlen && at < (u32)r; i++) {
                u32 take = (u32)r - at < (u32)m->iov[i].len ? (u32)r - at : (u32)m->iov[i].len;
                memcpy(m->iov[i].base, all + at, take);
                at += take;
            }
        }
        m->namelen = m->name ? fromlen : 0;
        m->controllen = 0;
        m->flags = said ? G_MSG_TRUNC : 0;
    }
    if (m->iovlen != 1) free(all);
    return r;
}

static gssize g_sendmsg(int fd, const g_msghdr *m, int flags)
{
    i32 r;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    r = sendmsg_held(s, m, flags, nonblock);
    mg_fd_moved(held);
    mg_fd_let_go(held);
    return r;
}

static gssize g_recvmsg(int fd, g_msghdr *m, int flags)
{
    i32 r;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    r = recvmsg_held(s, m, flags, nonblock);
    mg_fd_moved(held);
    mg_fd_let_go(held);
    return r;
}

/* Several messages: one after another until one is not taken. */
static int g_sendmmsg(int fd, g_mmsghdr *v, unsigned count, int flags)
{
    unsigned i;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    for (i = 0; i < count; i++) {
        i32 r = sendmsg_held(s, &v[i].hdr, flags, nonblock);
        if (r < 0) break;
        v[i].len = (u32)r;
    }
    mg_fd_moved(held);
    mg_fd_let_go(held);
    return i ? (int)i : -1;
}

/* Several messages: the first as asked, the rest only those already there. (The timeout is not looked at.) */
static int g_recvmmsg(int fd, g_mmsghdr *v, unsigned count, int flags, const void *timeout)
{
    unsigned i;
    (void)timeout;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    for (i = 0; i < count; i++) {
        i32 r = recvmsg_held(s, &v[i].hdr, i ? flags | G_MSG_DONTWAIT : flags, nonblock);
        if (r < 0) break;
        v[i].len = (u32)r;
    }
    mg_fd_moved(held);
    mg_fd_let_go(held);
    if (i) *mg_errno_ptr() = 0;
    return i ? (int)i : -1;
}

/* ------------------------------------------------- making and connecting */

static int g_socket(int domain, int type, int protocol)
{
    int kind = type & 0xF, flags = type & ~0xF, fd, err = 0;
    gsock *s;
    if (flags & ~(G_SOCK_NONBLOCK | G_SOCK_CLOEXEC)) return fail(GE_INVAL);
    if (domain == G_AF_UNIX) {
        if (kind != G_SOCK_STREAM && kind != G_SOCK_DGRAM && kind != G_SOCK_SEQPACKET) return fail(GE_SOCKTNOSUPPORT);
        if (protocol && protocol != G_AF_UNIX) return fail(GE_PROTONOSUPPORT);
        s = sock_new(domain, kind, 0);
    } else if (domain == G_AF_INET || domain == G_AF_INET6) {
        int ht = kind == G_SOCK_STREAM ? MG_SOCK_STREAM : kind == G_SOCK_DGRAM ? MG_SOCK_DGRAM : kind == G_SOCK_RAW ? MG_SOCK_RAW : 0;
        mg_sock *h;
        if (!ht) return fail(GE_SOCKTNOSUPPORT);
        if (kind == G_SOCK_STREAM && protocol && protocol != G_IPPROTO_TCP) return fail(GE_PROTONOSUPPORT);
        if (kind == G_SOCK_DGRAM && protocol && protocol != G_IPPROTO_UDP && protocol != G_IPPROTO_ICMP && protocol != G_IPPROTO_ICMPV6)
            return fail(GE_PROTONOSUPPORT);
        net_start();
        h = mg_sock_open(domain == G_AF_INET6 ? MG_AF_INET6 : MG_AF_INET, ht, protocol, &err);
        if (!h) return fail(gerr(err));
        s = sock_new(domain, kind, protocol ? protocol : kind == G_SOCK_STREAM ? G_IPPROTO_TCP : kind == G_SOCK_DGRAM ? G_IPPROTO_UDP : 0);
        s->host = h;
    } else {
        if (domain == G_AF_NETLINK)
            mg_note_once("network: an AF_NETLINK socket was asked for (Linux's kernel telling of routes and addresses); there is none here, and the app is told so (EAFNOSUPPORT)");
        return fail(domain == G_AF_PACKET ? GE_PERM : GE_AFNOSUPPORT);
    }
    fd = sock_register(s, flags);
    if (fd >= 0) said_first_socket(s);
    return fd;
}

static int g_socketpair(int domain, int type, int protocol, int sv[2])
{
    int kind = type & 0xF, flags = type & ~0xF;
    gsock *a, *b;
    if (flags & ~(G_SOCK_NONBLOCK | G_SOCK_CLOEXEC)) return fail(GE_INVAL);
    if (domain != G_AF_UNIX) return fail(domain == G_AF_INET || domain == G_AF_INET6 ? GE_OPNOTSUPP : GE_AFNOSUPPORT);
    if (kind != G_SOCK_STREAM && kind != G_SOCK_DGRAM && kind != G_SOCK_SEQPACKET) return fail(GE_SOCKTNOSUPPORT);
    if (protocol && protocol != G_AF_UNIX) return fail(GE_PROTONOSUPPORT);
    if (!sv) return fail(GE_FAULT);
    a = sock_new(G_AF_UNIX, kind, 0);
    b = sock_new(G_AF_UNIX, kind, 0);
    a->peer = b;
    b->peer = a;
    a->state = b->state = S_CONNECTED;
    sv[0] = sock_register(a, flags);
    if (sv[0] < 0) {
        /* (a's closing told b it is gone; b is no descriptor's yet.) */
        free(b);
        return -1;
    }
    sv[1] = sock_register(b, flags);
    if (sv[1] < 0) {
        mg_fd_close(sv[0]);
        return -1;
    }
    said_first_socket(a);
    return 0;
}

static int g_bind(int fd, const void *sa, u32 len)
{
    mg_sockaddr a;
    int e, err = 0;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    (void)nonblock;
    if (!s->host) {
        e = local_bind(s, sa, len);
    } else if ((e = mg_net_sockaddr_in(sa, len, &a)) == 0) {
        /* AF_UNSPEC with the wildcard address is taken as AF_INET's by an IPv4 socket, as Linux does for old code. */
        if (!a.family && s->family == G_AF_INET && !mg_rd32(a.addr)) a.family = MG_AF_INET;
        if ((e = addr_fits(s, &a, false)) == 0 && !mg_sock_bind(s->host, &a, &err)) e = gerr(err);
    }
    mg_fd_let_go(held);
    return e ? fail(e) : 0;
}

static int g_listen(int fd, int backlog)
{
    int e = 0, err = 0;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    (void)nonblock;
    if (s->type == G_SOCK_DGRAM || s->type == G_SOCK_RAW) {
        e = GE_OPNOTSUPP;
    } else if (!s->host) {
        static const u8 unnamed[2] = { G_AF_UNIX, 0 };
        /* An unnamed one is given a name of its own first, as Linux does. */
        if (!s->name) local_bind(s, unnamed, 2);
        mg_fd_lock();
        if (s->state == S_CONNECTED) e = GE_INVAL;
        else {
            s->state = S_LISTENING;
            s->backlog = backlog;
        }
        mg_fd_unlock();
    } else if (!mg_sock_listen(s->host, backlog, &err)) {
        e = gerr(err);
    } else {
        s->state = S_LISTENING;
    }
    mg_fd_let_go(held);
    return e ? fail(e) : 0;
}

static int g_accept4(int fd, void *addr, u32 *addrlen, int flags)
{
    int e = 0, nfd;
    gsock *n = NULL;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    if (flags & ~(G_SOCK_NONBLOCK | G_SOCK_CLOEXEC)) e = GE_INVAL;
    else if (s->state != S_LISTENING) e = GE_INVAL;
    else if (!s->host) {
        u64 deadline = deadline_of(s->rcvtimeo);
        mg_fd_lock();
        while (!e && !s->nqueue) {
            if (s->state != S_LISTENING) e = GE_INVAL;
            else if (nonblock) e = GE_AGAIN;
            else e = local_wait(s, deadline, s->rcvtimeo);
        }
        if (!e) {
            n = s->queue[0];
            memmove(s->queue, s->queue + 1, sizeof(gsock *) * (size_t)(--s->nqueue));
        }
        mg_fd_unlock();
        if (n && addr && addrlen) {
            mg_fd_lock();
            give_local_name(n->peer ? n->peer->name : NULL, n->peer ? n->peer->namelen : 0, addr, addrlen);
            mg_fd_unlock();
        }
    } else {
        u64 deadline = deadline_of(s->rcvtimeo);
        mg_sockaddr peer;
        for (;;) {
            int err = 0;
            mg_sock *h = mg_sock_accept(s->host, &peer, &err);
            if (h) {
                n = sock_new(s->family, s->type, s->protocol);
                n->host = h;
                n->state = S_CONNECTED;
                if (addr && addrlen) give_addr(s, &peer, addr, addrlen);
                break;
            }
            /* (A connection reset before it was taken is no longer there to take: the next one is waited for.) */
            if (err == MG_NE_CONNRESET || err == MG_NE_CONNABORTED) continue;
            if (err != MG_NE_AGAIN || nonblock) {
                e = gerr(err);
                break;
            }
            if ((e = sock_wait(s, MG_SOCK_IN, deadline, s->rcvtimeo)) != 0) break;
        }
    }
    mg_fd_moved(held);
    mg_fd_let_go(held);
    if (e) return fail(e);
    nfd = sock_register(n, flags);
    return nfd;
}

static int g_accept(int fd, void *addr, u32 *addrlen) { return g_accept4(fd, addr, addrlen, 0); }

static int g_connect(int fd, const void *sa, u32 len)
{
    mg_sockaddr a;
    int e = 0, err = 0;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    if (!s->host) {
        e = local_connect(s, sa, len, nonblock);
        mg_fd_let_go(held);
        return e ? fail(e) : 0;
    }
    if ((e = mg_net_sockaddr_in(sa, len, &a)) != 0) goto out;
    if (s->type != G_SOCK_STREAM) {
        /* A datagram socket: its peer is set, or with AF_UNSPEC taken away. */
        if (a.family && (e = addr_fits(s, &a, true)) != 0) goto out;
        if (!mg_sock_connect(s->host, &a, &err)) e = gerr(err);
        else if (a.family && mg_net_trace_wanted() && !s->io_traced) {
            /* trace-net: a datagram socket with a peer of its own (QUIC's) is followed as a connection is. */
            addr_text(&a, s->trace_to, sizeof(s->trace_to));
            s->io_traced = true;
            s->io_from = mg_time_monotonic_ns();
            open_add(s);
            mg_net_trace("UDP to %s: a peer of its own", s->trace_to);
        }
        goto out;
    }
    if (s->state == S_CONNECTING) {
        /* Asked again while the first is under way: whether it is, as Linux says it. */
        int h = mg_sock_ready(s->host);
        if (!(h & (MG_SOCK_OUT | MG_SOCK_ERR | MG_SOCK_HUP))) e = GE_ALREADY;
        else if ((e = gerr(mg_sock_error(s->host))) != 0) s->state = S_FAILED;
        else s->state = S_CONNECTED;
        if (e != GE_ALREADY) first_connection_ended(s, e, "connect, asked again");
        if (s->state == S_CONNECTED) e = GE_ISCONN;
        goto out;
    }
    if (s->state == S_CONNECTED || s->state == S_LISTENING) {
        e = GE_ISCONN;
        goto out;
    }
    if ((e = addr_fits(s, &a, false)) != 0) goto out;
    first_connection_began(s, &a);
    if (mg_sock_connect(s->host, &a, &err)) {
        s->state = S_CONNECTED;
    } else if (err != MG_NE_INPROGRESS) {
        e = gerr(err);
        s->state = S_FAILED;
    } else {
        u64 deadline = deadline_of(s->sndtimeo);
        s->state = S_CONNECTING;
        if (nonblock) {
            /* (Its outcome is said when the app learns it.) */
            e = GE_INPROGRESS;
            goto out;
        }
        for (;;) {
            int h = mg_sock_ready(s->host);
            if (h & (MG_SOCK_OUT | MG_SOCK_ERR | MG_SOCK_HUP)) {
                e = gerr(mg_sock_error(s->host));
                s->state = e ? S_FAILED : S_CONNECTED;
                break;
            }
            /* (A blocking connection whose SO_SNDTIMEO ran out is still under way, and Linux says so.) */
            if ((e = sock_wait(s, MG_SOCK_OUT, deadline, s->sndtimeo)) != 0) {
                if (e == GE_AGAIN) e = GE_INPROGRESS;
                break;
            }
        }
    }
    first_connection_ended(s, e, nonblock ? "connect itself" : "a blocking connect");
out:
    mg_fd_let_go(held);
    return e ? fail(e) : 0;
}

static int g_shutdown(int fd, int how)
{
    int e = 0, err = 0;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    (void)nonblock;
    if (how < 0 || how > 2) {
        e = GE_INVAL;
    } else if (!s->host) {
        mg_fd_lock();
        if (s->type != G_SOCK_DGRAM && s->state != S_CONNECTED && s->state != S_LISTENING) e = GE_NOTCONN;
        else {
            if (how != 1) s->shut_rd = true;
            if (how != 0) {
                s->shut_wr = true;
                if (s->peer) s->peer->peer_done = true;
            }
            mg_fd_notify_locked();
        }
        mg_fd_unlock();
    } else {
        settle(s, "shutdown");
        if (!mg_sock_shutdown(s->host, how, &err)) e = gerr(err);
        else {
            if (how != 1) s->shut_rd = true;
            if (how != 0) s->shut_wr = true;
            mg_fd_notify();
        }
    }
    mg_fd_let_go(held);
    return e ? fail(e) : 0;
}

static int name_of(int fd, void *addr, u32 *len, bool peer)
{
    mg_sockaddr a;
    int e = 0, err = 0;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    (void)nonblock;
    if (!addr || !len) {
        e = GE_FAULT;
    } else if (!s->host) {
        mg_fd_lock();
        if (!peer) give_local_name(s->name, s->namelen, addr, len);
        else if (s->peer) give_local_name(s->peer->name, s->peer->namelen, addr, len);
        else e = GE_NOTCONN;
        mg_fd_unlock();
    } else if (!mg_sock_name(s->host, peer, &a, &err)) {
        e = gerr(err);
    } else {
        give_addr(s, &a, addr, len);
    }
    mg_fd_let_go(held);
    return e ? fail(e) : 0;
}

static int g_getsockname(int fd, void *addr, u32 *len) { return name_of(fd, addr, len, false); }
static int g_getpeername(int fd, void *addr, u32 *len) { return name_of(fd, addr, len, true); }

/* ---------------------------------------------------------------- options */

/* An option's value as an int: a whole int, or for some of IPPROTO_IP's a single byte as Linux also takes. */
static int opt_int(const void *val, u32 len, bool byte_too, int *v)
{
    if (!val) return GE_FAULT;
    if (len >= 4) *v = (int)mg_rd32(val);
    else if (byte_too && len >= 1) *v = *(const u8 *)val;
    else return GE_INVAL;
    return 0;
}

static int give_int(int v, void *val, u32 *len)
{
    u32 n;
    if (!val || !len) return GE_FAULT;
    n = *len < 4 ? *len : 4;
    memcpy(val, &v, n);
    *len = n;
    return 0;
}

static int set_host(gsock *s, int option, int v, const char *what)
{
    int err = 0;
    if (mg_sock_set(s->host, option, v, &err)) return 0;
    return err == MG_NE_NOPROTOOPT ? quietly(what) : gerr(err);
}

static int get_host(gsock *s, int option, int *v)
{
    int err = 0;
    return mg_sock_get(s->host, option, v, &err) ? 0 : gerr(err);
}

/* The interface an IPv4 multicast option names by its index, as one of its addresses (which is how the hosts take it). */
static void v4_of_index(u32 index, mg_sockaddr *out)
{
    mg_gif *list = NULL;
    int n = mg_net_interfaces_android(&list), i, k;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < n; i++) {
        if (list[i].host.index != index) continue;
        for (k = 0; k < list[i].host.naddrs; k++)
            if (list[i].host.addrs[k].addr.family == MG_AF_INET) {
                *out = list[i].host.addrs[k].addr;
                break;
            }
    }
    free(list);
}

/* ip_mreq (group, interface's address) or ip_mreqn (and the interface's index after them). */
static int join_v4(gsock *s, bool join, const u8 *p, u32 len)
{
    mg_sockaddr group, iface;
    u32 index = 0;
    int err = 0;
    if (len < 8) return GE_INVAL;
    memset(&group, 0, sizeof(group));
    memset(&iface, 0, sizeof(iface));
    group.family = MG_AF_INET;
    memcpy(group.addr, p, 4);
    if (mg_rd32(p + 4)) {
        iface.family = MG_AF_INET;
        memcpy(iface.addr, p + 4, 4);
    }
    if (len >= 12) index = mg_rd32(p + 8);
    return mg_sock_multicast(s->host, join, &group, &iface, index, &err) ? 0 : gerr(err);
}

/* group_req: the interface's index, then a sockaddr_storage, which is aligned as a pointer is. */
static int join_group(gsock *s, bool join, const u8 *p, u32 len)
{
    u32 at = (u32)sizeof(void *);
    mg_sockaddr group;
    int e, err = 0;
    if (len < at + 16) return GE_INVAL;
    if ((e = mg_net_sockaddr_in(p + at, len - at, &group)) != 0) return e;
    if (!group.family) return GE_INVAL;
    return mg_sock_multicast(s->host, join, &group, NULL, mg_rd32(p), &err) ? 0 : gerr(err);
}

static int setsockopt_held(gsock *s, int level, int name, const void *val, u32 len)
{
    const u8 *p = (const u8 *)val;
    int v = 0, e = 0;
    bool local = !s->host;
    if (level == G_SOL_SOCKET) {
        switch (name) {
        case G_SO_RCVTIMEO:
        case G_SO_SNDTIMEO: {
            /* A struct timeval: two longs. */
            g_timeval tv;
            gtime sec;
            glong usec;
            u64 ns;
            if (!p) return GE_FAULT;
            if (len < sizeof(g_timeval)) return GE_INVAL;
            memcpy(&tv, p, sizeof(tv));
            sec = tv.tv_sec;
            usec = tv.tv_usec;
            if (sec < 0 || usec < 0 || usec >= 1000000) return GE_DOM;
            ns = (u64)sec * 1000000000ull + (u64)usec * 1000ull;
            if (name == G_SO_RCVTIMEO) s->rcvtimeo = ns;
            else s->sndtimeo = ns;
            return 0;
        }
        case G_SO_LINGER:
            if (!p) return GE_FAULT;
            if (len < 8) return GE_INVAL;
            if (local) return 0;
            return set_host(s, MG_SO_LINGER, mg_rd32(p) ? (int)mg_rd32(p + 4) : -1, "SO_LINGER");
        case G_SO_TYPE: case G_SO_ERROR: case G_SO_ACCEPTCONN: case G_SO_DOMAIN: case G_SO_PROTOCOL: case G_SO_PEERCRED:
            return GE_NOPROTOOPT;
        }
        if ((e = opt_int(val, len, false, &v)) != 0) return e;
        switch (name) {
        case G_SO_REUSEADDR: return local ? 0 : set_host(s, MG_SO_REUSEADDR, v != 0, "SO_REUSEADDR");
        case G_SO_KEEPALIVE: return local ? 0 : set_host(s, MG_SO_KEEPALIVE, v != 0, "SO_KEEPALIVE");
        case G_SO_BROADCAST: return local ? 0 : set_host(s, MG_SO_BROADCAST, v != 0, "SO_BROADCAST");
        case G_SO_OOBINLINE: return local ? 0 : set_host(s, MG_SO_OOBINLINE, v != 0, "SO_OOBINLINE");
        case G_SO_DONTROUTE: return local ? 0 : set_host(s, MG_SO_DONTROUTE, v != 0, "SO_DONTROUTE");
        case G_SO_RCVBUF: case G_SO_RCVBUFFORCE:
            if (local) {
                s->rcvbuf = v;
                return 0;
            }
            return set_host(s, MG_SO_RCVBUF, v, "SO_RCVBUF");
        case G_SO_SNDBUF: case G_SO_SNDBUFFORCE:
            if (local) {
                s->sndbuf = v;
                return 0;
            }
            return set_host(s, MG_SO_SNDBUF, v, "SO_SNDBUF");
        case G_SO_REUSEPORT: return quietly("SO_REUSEPORT");
        case G_SO_PRIORITY: return quietly("SO_PRIORITY");
        case G_SO_MARK: return quietly("SO_MARK");
        case G_SO_BINDTODEVICE: return quietly("SO_BINDTODEVICE");
        case G_SO_TIMESTAMP: return quietly("SO_TIMESTAMP");
        case G_SO_RCVLOWAT: case G_SO_SNDLOWAT: return quietly("SO_RCVLOWAT/SO_SNDLOWAT");
        case G_SO_PASSCRED: case G_SO_DEBUG: case G_SO_NO_CHECK: case G_SO_BSDCOMPAT: return 0;
        default: return GE_NOPROTOOPT;
        }
    }
    if (local) return GE_OPNOTSUPP;
    if (level == G_IPPROTO_TCP) {
        if (s->type != G_SOCK_STREAM) return GE_NOPROTOOPT;
        if ((e = opt_int(val, len, false, &v)) != 0) return e;
        switch (name) {
        case G_TCP_NODELAY: return set_host(s, MG_TCP_NODELAY, v != 0, "TCP_NODELAY");
        case G_TCP_MAXSEG: return set_host(s, MG_TCP_MAXSEG, v, "TCP_MAXSEG");
        case G_TCP_KEEPIDLE:
            if (v < 1) return GE_INVAL;
            s->keepidle = v;
            return set_host(s, MG_TCP_KEEPIDLE, v, "TCP_KEEPIDLE");
        case G_TCP_KEEPINTVL:
            if (v < 1) return GE_INVAL;
            s->keepintvl = v;
            return set_host(s, MG_TCP_KEEPINTVL, v, "TCP_KEEPINTVL");
        case G_TCP_KEEPCNT:
            if (v < 1) return GE_INVAL;
            s->keepcnt = v;
            return set_host(s, MG_TCP_KEEPCNT, v, "TCP_KEEPCNT");
        case 3: return quietly("TCP_CORK");
        case 7: return quietly("TCP_SYNCNT");
        case 8: return quietly("TCP_LINGER2");
        case 9: return quietly("TCP_DEFER_ACCEPT");
        case 10: return quietly("TCP_WINDOW_CLAMP");
        case 12: return quietly("TCP_QUICKACK");
        case 13: return quietly("TCP_CONGESTION");
        case 18: return quietly("TCP_USER_TIMEOUT");
        case 23: return quietly("TCP_FASTOPEN");
        case 25: return quietly("TCP_NOTSENT_LOWAT");
        default: return GE_NOPROTOOPT;
        }
    }
    if (level == G_IPPROTO_IP) {
        switch (name) {
        case G_IP_ADD_MEMBERSHIP: case G_IP_DROP_MEMBERSHIP:
            if (!p) return GE_FAULT;
            return join_v4(s, name == G_IP_ADD_MEMBERSHIP, p, len);
        case G_MCAST_JOIN_GROUP: case G_MCAST_LEAVE_GROUP:
            if (!p) return GE_FAULT;
            return join_group(s, name == G_MCAST_JOIN_GROUP, p, len);
        case G_IP_MULTICAST_IF: {
            /* An address (in_addr), or ip_mreq's interface, or ip_mreqn's interface and index. */
            mg_sockaddr a;
            u32 addr;
            if (!p) return GE_FAULT;
            if (len < 4) return GE_INVAL;
            addr = len >= 8 ? mg_rd32(p + 4) : mg_rd32(p);
            if (!addr && len >= 12 && mg_rd32(p + 8)) {
                v4_of_index(mg_rd32(p + 8), &a);
                memcpy(&addr, a.addr, 4);
            }
            return set_host(s, MG_IP_MULTICAST_IF, (int)addr, "IP_MULTICAST_IF");
        }
        }
        if ((e = opt_int(val, len, true, &v)) != 0) return e;
        switch (name) {
        case G_IP_TOS: return set_host(s, MG_IP_TOS, v, "IP_TOS");
        case G_IP_TTL: return set_host(s, MG_IP_TTL, v, "IP_TTL");
        case G_IP_MULTICAST_TTL: return set_host(s, MG_IP_MULTICAST_TTL, v, "IP_MULTICAST_TTL");
        case G_IP_MULTICAST_LOOP: return set_host(s, MG_IP_MULTICAST_LOOP, v != 0, "IP_MULTICAST_LOOP");
        case 8: return quietly("IP_PKTINFO");
        case 10: return quietly("IP_MTU_DISCOVER");
        case 11: return quietly("IP_RECVERR");
        case 13: return quietly("IP_RECVTOS");
        case 49: return quietly("IP_MULTICAST_ALL");
        default: return GE_NOPROTOOPT;
        }
    }
    if (level == G_IPPROTO_IPV6) {
        if (s->family != G_AF_INET6) return GE_NOPROTOOPT;
        switch (name) {
        case G_IPV6_JOIN_GROUP: case G_IPV6_LEAVE_GROUP: {
            mg_sockaddr group;
            int err = 0;
            if (!p) return GE_FAULT;
            if (len < 20) return GE_INVAL;
            memset(&group, 0, sizeof(group));
            group.family = MG_AF_INET6;
            memcpy(group.addr, p, 16);
            return mg_sock_multicast(s->host, name == G_IPV6_JOIN_GROUP, &group, NULL, mg_rd32(p + 16), &err) ? 0 : gerr(err);
        }
        case G_MCAST_JOIN_GROUP: case G_MCAST_LEAVE_GROUP:
            if (!p) return GE_FAULT;
            return join_group(s, name == G_MCAST_JOIN_GROUP, p, len);
        }
        if ((e = opt_int(val, len, false, &v)) != 0) return e;
        switch (name) {
        case G_IPV6_V6ONLY: return set_host(s, MG_IPV6_V6ONLY, v != 0, "IPV6_V6ONLY");
        case G_IPV6_UNICAST_HOPS: return set_host(s, MG_IPV6_UNICAST_HOPS, v, "IPV6_UNICAST_HOPS");
        case G_IPV6_MULTICAST_HOPS: return set_host(s, MG_IPV6_MULTICAST_HOPS, v, "IPV6_MULTICAST_HOPS");
        case G_IPV6_MULTICAST_LOOP: return set_host(s, MG_IPV6_MULTICAST_LOOP, v != 0, "IPV6_MULTICAST_LOOP");
        case G_IPV6_MULTICAST_IF: return set_host(s, MG_IPV6_MULTICAST_IF, v, "IPV6_MULTICAST_IF");
        case G_IPV6_TCLASS: return set_host(s, MG_IPV6_TCLASS, v, "IPV6_TCLASS");
        case 23: return quietly("IPV6_MTU_DISCOVER");
        case 25: return quietly("IPV6_RECVERR");
        case 49: return quietly("IPV6_RECVPKTINFO");
        default: return GE_NOPROTOOPT;
        }
    }
    return GE_NOPROTOOPT;
}

static int g_setsockopt(int fd, int level, int name, const void *val, u32 len)
{
    int e;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    (void)nonblock;
    e = setsockopt_held(s, level, name, val, len);
    mg_fd_let_go(held);
    return e ? fail(e) : 0;
}

static int getsockopt_held(gsock *s, int level, int name, void *val, u32 *len)
{
    u8 buf[16];                        /* (the largest: a struct timeval of 64-bit Android) */
    int v = 0, e = 0;
    bool local = !s->host;
    if (!val || !len) return GE_FAULT;
    if (level == G_SOL_SOCKET) {
        switch (name) {
        case G_SO_TYPE: return give_int(s->type, val, len);
        case G_SO_DOMAIN: return give_int(s->family, val, len);
        case G_SO_PROTOCOL: return give_int(s->protocol, val, len);
        case G_SO_ACCEPTCONN: return give_int(s->state == S_LISTENING, val, len);
        case G_SO_ERROR:
            if (local) return give_int(0, val, len);
            if (s->state == S_CONNECTING) {
                int h = mg_sock_ready(s->host);
                if (!(h & (MG_SOCK_OUT | MG_SOCK_ERR | MG_SOCK_HUP))) return give_int(0, val, len);
                v = gerr(mg_sock_error(s->host));
                s->state = v ? S_FAILED : S_CONNECTED;
                first_connection_ended(s, v, "getsockopt SO_ERROR");
                return give_int(v, val, len);
            }
            return give_int(gerr(mg_sock_error(s->host)), val, len);
        case G_SO_RCVTIMEO:
        case G_SO_SNDTIMEO: {
            u64 ns = name == G_SO_RCVTIMEO ? s->rcvtimeo : s->sndtimeo;
            g_timeval tv;
            tv.tv_sec = (gtime)(ns / 1000000000ull);
            tv.tv_usec = (glong)(ns % 1000000000ull / 1000ull);
            memcpy(buf, &tv, sizeof(tv));
            mg_net_give(buf, sizeof(tv), val, len);
            return 0;
        }
        case G_SO_LINGER:
            v = -1;
            if (!local && (e = get_host(s, MG_SO_LINGER, &v)) != 0) return e;
            mg_wr32(buf, v >= 0);
            mg_wr32(buf + 4, (u32)(v >= 0 ? v : 0));
            mg_net_give(buf, 8, val, len);
            return 0;
        case G_SO_PEERCRED:
            /* Whoever is at the other end is this very process. */
            if (!local || !s->peer) return local ? GE_NOTCONN : GE_NOPROTOOPT;
            mg_wr32(buf, 4242);
            mg_wr32(buf + 4, 10100);
            mg_wr32(buf + 8, 10100);
            mg_net_give(buf, 12, val, len);
            return 0;
        case G_SO_RCVBUF: case G_SO_SNDBUF:
            if (local) return give_int(name == G_SO_RCVBUF ? (s->rcvbuf ? s->rcvbuf : (int)LOCAL_ROOM) : (s->sndbuf ? s->sndbuf : (int)LOCAL_ROOM), val, len);
            if ((e = get_host(s, name == G_SO_RCVBUF ? MG_SO_RCVBUF : MG_SO_SNDBUF, &v)) != 0) return e;
            return give_int(v, val, len);
        case G_SO_REUSEADDR: case G_SO_KEEPALIVE: case G_SO_BROADCAST: case G_SO_OOBINLINE: case G_SO_DONTROUTE: {
            int option = name == G_SO_REUSEADDR ? MG_SO_REUSEADDR : name == G_SO_KEEPALIVE ? MG_SO_KEEPALIVE : name == G_SO_BROADCAST ? MG_SO_BROADCAST
                         : name == G_SO_OOBINLINE ? MG_SO_OOBINLINE : MG_SO_DONTROUTE;
            if (!local && (e = get_host(s, option, &v)) != 0) return e;
            return give_int(v != 0, val, len);
        }
        case G_SO_RCVLOWAT: case G_SO_SNDLOWAT: return give_int(1, val, len);
        case G_SO_PRIORITY: case G_SO_MARK: case G_SO_REUSEPORT: case G_SO_PASSCRED: case G_SO_TIMESTAMP: case G_SO_DEBUG: return give_int(0, val, len);
        default: return GE_NOPROTOOPT;
        }
    }
    if (local) return GE_OPNOTSUPP;
    if (level == G_IPPROTO_TCP) {
        int option, *kept, fallback;
        if (s->type != G_SOCK_STREAM) return GE_NOPROTOOPT;
        switch (name) {
        case G_TCP_NODELAY: option = MG_TCP_NODELAY; kept = NULL; fallback = 0; break;
        case G_TCP_MAXSEG: option = MG_TCP_MAXSEG; kept = NULL; fallback = 536; break;
        /* (Linux's defaults where the host cannot be asked: two hours, 75 s, nine probes.) */
        case G_TCP_KEEPIDLE: option = MG_TCP_KEEPIDLE; kept = &s->keepidle; fallback = 7200; break;
        case G_TCP_KEEPINTVL: option = MG_TCP_KEEPINTVL; kept = &s->keepintvl; fallback = 75; break;
        case G_TCP_KEEPCNT: option = MG_TCP_KEEPCNT; kept = &s->keepcnt; fallback = 9; break;
        default: return GE_NOPROTOOPT;
        }
        if (get_host(s, option, &v) != 0) v = kept && *kept ? *kept : fallback;
        return give_int(option == MG_TCP_NODELAY ? v != 0 : v, val, len);
    }
    if (level == G_IPPROTO_IP) {
        int option;
        switch (name) {
        case G_IP_TOS: option = MG_IP_TOS; break;
        case G_IP_TTL: option = MG_IP_TTL; break;
        case G_IP_MULTICAST_TTL: option = MG_IP_MULTICAST_TTL; break;
        case G_IP_MULTICAST_LOOP: option = MG_IP_MULTICAST_LOOP; break;
        case G_IP_MULTICAST_IF:
            if ((e = get_host(s, MG_IP_MULTICAST_IF, &v)) != 0) return e;
            memcpy(buf, &v, 4);
            mg_net_give(buf, 4, val, len);
            return 0;
        default: return GE_NOPROTOOPT;
        }
        if ((e = get_host(s, option, &v)) != 0) return e;
        if (*len < 4 && *len >= 1) {
            /* (These are said in one byte to one who asks with one.) */
            *(u8 *)val = (u8)v;
            *len = 1;
            return 0;
        }
        return give_int(v, val, len);
    }
    if (level == G_IPPROTO_IPV6) {
        int option;
        if (s->family != G_AF_INET6) return GE_NOPROTOOPT;
        switch (name) {
        case G_IPV6_V6ONLY: option = MG_IPV6_V6ONLY; break;
        case G_IPV6_UNICAST_HOPS: option = MG_IPV6_UNICAST_HOPS; break;
        case G_IPV6_MULTICAST_HOPS: option = MG_IPV6_MULTICAST_HOPS; break;
        case G_IPV6_MULTICAST_LOOP: option = MG_IPV6_MULTICAST_LOOP; break;
        case G_IPV6_MULTICAST_IF: option = MG_IPV6_MULTICAST_IF; break;
        case G_IPV6_TCLASS: option = MG_IPV6_TCLASS; break;
        default: return GE_NOPROTOOPT;
        }
        if ((e = get_host(s, option, &v)) != 0) return e;
        return give_int(option == MG_IPV6_V6ONLY || option == MG_IPV6_MULTICAST_LOOP ? v != 0 : v, val, len);
    }
    return GE_NOPROTOOPT;
}

static int g_getsockopt(int fd, int level, int name, void *val, u32 *len)
{
    int e;
    HOLD_OR_FAIL(fd, s, held, nonblock, -1)
    (void)nonblock;
    e = getsockopt_held(s, level, name, val, len);
    mg_fd_let_go(held);
    return e ? fail(e) : 0;
}

/* --------------------------------------------- the descriptor's other calls */

static int sock_poll(void *obj, int events)
{
    gsock *s = (gsock *)obj;
    int r = 0, h;
    (void)events;
    if (!s->host) return local_poll(s);
    /* A TCP socket never connected is closed as far as poll on Linux tells: writable and hung up. */
    if (s->type == G_SOCK_STREAM && (s->state == S_FRESH)) return G_POLLOUT | G_POLLWRNORM | G_POLLHUP;
    h = mg_sock_ready(s->host);
    if (s->state == S_LISTENING) return (h & MG_SOCK_IN) ? G_POLLIN | G_POLLRDNORM : 0;
    if (h & MG_SOCK_IN) r |= G_POLLIN | G_POLLRDNORM;
    if (h & MG_SOCK_OUT) r |= G_POLLOUT | G_POLLWRNORM;
    if (h & MG_SOCK_ERR) r |= G_POLLERR;
    if (h & MG_SOCK_HUP) r |= G_POLLHUP;
    if (h & MG_SOCK_RDHUP) r |= G_POLLIN | G_POLLRDNORM | G_POLLRDHUP;
    if (s->type == G_SOCK_STREAM) {
        if (s->shut_rd) r |= G_POLLIN | G_POLLRDNORM | G_POLLRDHUP;
        if ((s->shut_rd || (h & MG_SOCK_RDHUP)) && s->shut_wr) r |= G_POLLHUP;
    }
    return r;
}

static int sock_readable(void *obj)
{
    gsock *s = (gsock *)obj;
    int n;
    if (s->host) return mg_sock_pending(s->host);
    mg_fd_lock();
    n = s->type == G_SOCK_STREAM ? (int)s->rcount : s->mcount ? (int)s->msgs[s->mhead] : 0;
    mg_fd_unlock();
    return n;
}

static void sock_close(void *obj)
{
    gsock *s = (gsock *)obj;
    /* (A first connection closed before anyone learnt how it went is not said: the next one is.) */
    if (s->first) mg_atomic_xchg(&g_first_connection, 0);
    io_said(s);
    if (s->host) mg_sock_close(s->host);
    else local_close(s);
    free(s);
}

static int sock_ioctl(void *obj, unsigned request, void *arg)
{
    gsock *s = (gsock *)obj;
    int r;
    if (request == 0x8905) {                       /* SIOCATMARK */
        if (!arg) return fail(GE_FAULT);
        *(int *)arg = s->host ? mg_sock_at_mark(s->host) : 0;
        return 0;
    }
    if (request == 0x5411) {                       /* SIOCOUTQ: what is sent and not yet acknowledged, which the host does not say */
        if (!arg) return fail(GE_FAULT);
        *(int *)arg = 0;
        return 0;
    }
    r = mg_net_if_ioctl(request, arg);
    if (r != -2) return r;
    return fail(GE_NOTTY);
}

/*
 * CMSG_NXTHDR, which bionic's header makes a call: the control message after `c` in the message's control data, or NULL.
 * A struct cmsghdr is a size_t length and two ints (12 bytes in 32-bit Android, 16 in 64-bit), and each message is
 * aligned to a long, as CMSG_ALIGN aligns it.
 */
typedef struct g_cmsghdr {
    size_t len;
    int level, type;
} g_cmsghdr;
#define G_CMSG_ALIGN(n) (((n) + sizeof(gulong) - 1) & ~(sizeof(gulong) - 1))

static g_cmsghdr *g_cmsg_nxthdr(const g_msghdr *msg, g_cmsghdr *c)
{
    u8 *control = (u8 *)msg->control, *next;
    size_t controllen = msg->controllen;
    if (!c) return controllen >= sizeof(g_cmsghdr) ? (g_cmsghdr *)control : NULL;
    next = (u8 *)c + G_CMSG_ALIGN(c->len);
    if ((size_t)(next + sizeof(g_cmsghdr) - control) > controllen) return NULL;
    return (g_cmsghdr *)next;
}

/* FD_SET, FD_CLR and FD_ISSET as bionic's fortified header calls them: checked against the set's 1024 descriptors. */
static void fd_checked(int fd, size_t size, const char *what)
{
    if (fd < 0 || fd >= 1024) mg_fatal("guest: %s: descriptor %d is outside an fd_set", what, fd);
    if (size < 128) mg_fatal("guest: %s: an fd_set of %u bytes", what, (unsigned)size);
}
static void g_fd_set_chk(int fd, u32 *set, size_t size) { fd_checked(fd, size, "FD_SET"); set[fd / 32] |= 1u << (fd % 32); }
static void g_fd_clr_chk(int fd, u32 *set, size_t size) { fd_checked(fd, size, "FD_CLR"); set[fd / 32] &= ~(1u << (fd % 32)); }
static int g_fd_isset_chk(int fd, const u32 *set, size_t size) { fd_checked(fd, size, "FD_ISSET"); return (set[fd / 32] >> (fd % 32)) & 1; }

const mg_sym mg_tab_net[] = {
    MG_WRAP("__cmsg_nxthdr", g_cmsg_nxthdr), MG_WRAP("__FD_SET_chk", g_fd_set_chk), MG_WRAP("__FD_CLR_chk", g_fd_clr_chk),
    MG_WRAP("__FD_ISSET_chk", g_fd_isset_chk),
    MG_WRAP("socket", g_socket), MG_WRAP("socketpair", g_socketpair), MG_WRAP("connect", g_connect), MG_WRAP("bind", g_bind),
    MG_WRAP("listen", g_listen), MG_WRAP("accept", g_accept), MG_WRAP("accept4", g_accept4), MG_WRAP("send", g_send),
    MG_WRAP("sendto", g_sendto), MG_WRAP("__sendto_chk", g_sendto_chk), MG_WRAP("sendmsg", g_sendmsg), MG_WRAP("sendmmsg", g_sendmmsg),
    MG_WRAP("recv", g_recv), MG_WRAP("recvfrom", g_recvfrom), MG_WRAP("__recvfrom_chk", g_recvfrom_chk), MG_WRAP("recvmsg", g_recvmsg),
    MG_WRAP("recvmmsg", g_recvmmsg), MG_WRAP("shutdown", g_shutdown), MG_WRAP("setsockopt", g_setsockopt), MG_WRAP("getsockopt", g_getsockopt),
    MG_WRAP("getsockname", g_getsockname), MG_WRAP("getpeername", g_getpeername),
    MG_END
};
