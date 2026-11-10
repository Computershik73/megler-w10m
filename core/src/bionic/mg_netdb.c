/*
 * Names and addresses, as Android's C library has them: getaddrinfo (and
 * android_getaddrinfofornet, which libcore's InetAddress goes through),
 * getnameinfo, the old gethostby* calls, addresses as text, services and
 * protocols by name, and the network interfaces (getifaddrs, if_nametoindex,
 * and the ioctls that ask about one, which java.net.NetworkInterface uses).
 *
 * Names are the host's resolver's to look up (GetAddrInfoW on Windows). What
 * needs no network is answered here: numbers, and "localhost", which Android
 * has in /system/etc/hosts as 127.0.0.1 (and ip6-localhost as ::1). A name the
 * resolver does not know is EAI_NODATA, as bionic's getaddrinfo has it for
 * every name its DNS answered without addresses, NXDOMAIN too ("No address
 * associated with hostname", what an app shows for a host it cannot reach);
 * EAI_NONAME is a number that is not one, or a name with AI_NUMERICHOST. The
 * results are bionic's structures: struct addrinfo has ai_canonname before
 * ai_addr there, the BSD order, which glibc's has the other way round.
 *
 * Interfaces are the host's (GetAdaptersAddresses on Windows) under the names
 * an Android phone gives them: lo, wlan0, eth0, rmnet0.
 */
#include "mg_net.h"

#include <stdio.h>

/* getaddrinfo's errors and flags (bionic's netdb.h) */
#define G_EAI_ADDRFAMILY 1
#define G_EAI_AGAIN 2
#define G_EAI_BADFLAGS 3
#define G_EAI_FAIL 4
#define G_EAI_FAMILY 5
#define G_EAI_MEMORY 6
#define G_EAI_NODATA 7
#define G_EAI_NONAME 8
#define G_EAI_SERVICE 9
#define G_EAI_SOCKTYPE 10
#define G_EAI_SYSTEM 11
#define G_EAI_BADHINTS 12
#define G_EAI_PROTOCOL 13
#define G_EAI_OVERFLOW 14

#define G_AI_PASSIVE 1
#define G_AI_CANONNAME 2
#define G_AI_NUMERICHOST 4
#define G_AI_NUMERICSERV 8
#define G_AI_ADDRCONFIG 0x400
#define G_AI_MASK (G_AI_PASSIVE | G_AI_CANONNAME | G_AI_NUMERICHOST | G_AI_NUMERICSERV | G_AI_ADDRCONFIG)

#define G_NI_NOFQDN 1
#define G_NI_NUMERICHOST 2
#define G_NI_NAMEREQD 4
#define G_NI_NUMERICSERV 8
#define G_NI_DGRAM 16

/* h_errno */
#define G_HOST_NOT_FOUND 1
#define G_TRY_AGAIN 2
#define G_NO_RECOVERY 3
#define G_NO_DATA 4

#define G_IFF_UP 0x1
#define G_IFF_BROADCAST 0x2
#define G_IFF_LOOPBACK 0x8
#define G_IFF_POINTOPOINT 0x10
#define G_IFF_RUNNING 0x40
#define G_IFF_NOARP 0x80
#define G_IFF_MULTICAST 0x1000
#define G_IFF_LOWER_UP 0x10000

/* Bionic's structures. Pointers are the host's own, which on the phone and under qemu are bionic's 32 bits. */
typedef struct g_addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    u32 ai_addrlen;
    char *ai_canonname;
    void *ai_addr;
    struct g_addrinfo *ai_next;
} g_addrinfo;

typedef struct g_hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype, h_length;
    char **h_addr_list;
} g_hostent;

typedef struct g_servent {
    char *s_name;
    char **s_aliases;
    int s_port;                        /* in the network's byte order */
    char *s_proto;
} g_servent;

typedef struct g_protoent {
    char *p_name;
    char **p_aliases;
    int p_proto;
} g_protoent;

typedef struct g_ifaddrs {
    struct g_ifaddrs *ifa_next;
    char *ifa_name;
    u32 ifa_flags;
    void *ifa_addr, *ifa_netmask;
    void *ifa_ifu;                     /* the broadcast address, or a point-to-point link's other end */
    void *ifa_data;
} g_ifaddrs;

typedef struct g_nameindex {
    u32 if_index;
    char *if_name;
} g_nameindex;

static MG_THREAD_LOCAL int t_h_errno;
static int *g_h_errno_ptr(void) { return &t_h_errno; }

/* ----------------------------------------------------- addresses as text */

static u32 g_htonl(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
static u16 g_htons(u16 v) { return (u16)((v >> 8) | (v << 8)); }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* inet_pton's IPv4: exactly four decimal parts, none with a leading zero (as bionic's). */
static bool parse_v4(const char *s, u8 out[4])
{
    int part = 0, digits = 0;
    u32 v = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            if (digits && !v) return false;
            v = v * 10 + (u32)(*s - '0');
            if (v > 255 || ++digits > 3) return false;
        } else if (*s == '.' || !*s) {
            if (!digits || part > 3) return false;
            out[part++] = (u8)v;
            v = 0;
            digits = 0;
            if (!*s) break;
        } else {
            return false;
        }
    }
    return part == 4;
}

/* inet_pton's IPv6, as the BIND resolver's (which bionic's is) reads it: groups of up to four hex digits, one "::", an IPv4 address at the end. */
static bool parse_v6(const char *src, u8 out[16])
{
    u8 tmp[16], *tp = tmp, *endp = tmp + 16, *gap = NULL;
    const char *token;
    int digits = 0;
    u32 v = 0;
    memset(tmp, 0, sizeof(tmp));
    if (*src == ':' && *++src != ':') return false;
    token = src;
    for (;;) {
        char c = *src++;
        int h;
        if (!c) break;
        if ((h = hexval(c)) >= 0) {
            v = (v << 4) | (u32)h;
            if (++digits > 4) return false;
            continue;
        }
        if (c == ':') {
            token = src;
            if (!digits) {
                if (gap) return false;
                gap = tp;
                continue;
            }
            if (!*src || tp + 2 > endp) return false;
            *tp++ = (u8)(v >> 8);
            *tp++ = (u8)v;
            digits = 0;
            v = 0;
            continue;
        }
        if (c == '.' && tp + 4 <= endp && parse_v4(token, tp)) {
            tp += 4;
            digits = 0;
            break;
        }
        return false;
    }
    if (digits) {
        if (tp + 2 > endp) return false;
        *tp++ = (u8)(v >> 8);
        *tp++ = (u8)v;
    }
    if (gap) {
        int n = (int)(tp - gap), i;
        if (tp == endp) return false;
        for (i = 1; i <= n; i++) {
            endp[-i] = gap[n - i];
            gap[n - i] = 0;
        }
        tp = endp;
    }
    if (tp != endp) return false;
    memcpy(out, tmp, 16);
    return true;
}

static int g_inet_pton(int family, const char *s, void *out)
{
    u8 a[16];
    if (family == G_AF_INET) {
        if (!s || !parse_v4(s, a)) return 0;
        memcpy(out, a, 4);
        return 1;
    }
    if (family == G_AF_INET6) {
        if (!s || !parse_v6(s, a)) return 0;
        memcpy(out, a, 16);
        return 1;
    }
    mg_set_errno(GE_AFNOSUPPORT);
    return -1;
}

/*
 * IPv6 as text the way bionic writes it (the BIND resolver's inet_ntop):
 * hexadecimal groups without leading zeros, the longest run of two or more
 * zero groups as "::", and an IPv4 address mapped (::ffff:a.b.c.d) or
 * compatible (::a.b.c.d) written as one.
 */
static void v6_text(const u8 *a, char *out)
{
    u16 w[8];
    int i, best = -1, best_len = 0, cur = -1, cur_len = 0;
    char *p = out;
    for (i = 0; i < 8; i++) w[i] = (u16)((a[2 * i] << 8) | a[2 * i + 1]);
    for (i = 0; i < 8; i++) {
        if (!w[i]) {
            if (cur < 0) {
                cur = i;
                cur_len = 1;
            } else {
                cur_len++;
            }
        } else if (cur >= 0) {
            if (cur_len > best_len) {
                best = cur;
                best_len = cur_len;
            }
            cur = -1;
        }
    }
    if (cur >= 0 && cur_len > best_len) {
        best = cur;
        best_len = cur_len;
    }
    if (best_len < 2) best = -1;
    for (i = 0; i < 8; i++) {
        if (best >= 0 && i >= best && i < best + best_len) {
            if (i == best) *p++ = ':';
            continue;
        }
        if (i) *p++ = ':';
        if (i == 6 && best == 0 && (best_len == 6 || (best_len == 7 && w[7] != 1) || (best_len == 5 && w[5] == 0xFFFF))) {
            p += sprintf(p, "%u.%u.%u.%u", a[12], a[13], a[14], a[15]);
            break;
        }
        p += sprintf(p, "%x", w[i]);
    }
    if (best >= 0 && best + best_len == 8) *p++ = ':';
    *p = 0;
}

static const char *g_inet_ntop(int family, const void *addr, char *out, u32 size)
{
    const u8 *a = (const u8 *)addr;
    char text[64];
    if (family == G_AF_INET) snprintf(text, sizeof(text), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    else if (family == G_AF_INET6) v6_text(a, text);
    else {
        mg_set_errno(GE_AFNOSUPPORT);
        return NULL;
    }
    if (strlen(text) + 1 > size) {
        mg_set_errno(GE_NOSPC);
        return NULL;
    }
    strcpy(out, text);
    return out;
}

/* inet_aton: one to four parts, each decimal, octal (0...) or hexadecimal (0x...), the last filling what is left -- the old BSD forms. */
static int g_inet_aton(const char *s, u32 *out)
{
    u32 parts[4], v;
    int n = 0;
    if (!s) return 0;
    for (;;) {
        int base = 10, digits = 0;
        v = 0;
        if (*s == '0') {
            base = 8;
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            }
        }
        for (;; s++) {
            int d = hexval(*s);
            if (d < 0 || d >= base) break;
            if (v > 0xFFFFFFFFu / (u32)base) return 0;
            v = v * (u32)base + (u32)d;
            digits++;
        }
        if (!digits && base != 8) return 0;
        if (n == 4) return 0;
        parts[n++] = v;
        if (*s == '.') {
            s++;
            continue;
        }
        if (*s && *s != ' ' && *s != '\t' && *s != '\n') return 0;
        break;
    }
    switch (n) {
    case 1: v = parts[0]; break;
    case 2:
        if (parts[0] > 0xFF || parts[1] > 0xFFFFFF) return 0;
        v = (parts[0] << 24) | parts[1];
        break;
    case 3:
        if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFFFF) return 0;
        v = (parts[0] << 24) | (parts[1] << 16) | parts[2];
        break;
    default:
        if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFF || parts[3] > 0xFF) return 0;
        v = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        break;
    }
    if (out) *out = g_htonl(v);
    return 1;
}

static u32 g_inet_addr(const char *s)
{
    u32 v;
    return g_inet_aton(s, &v) ? v : 0xFFFFFFFFu;
}

static char *g_inet_ntoa(u32 in)
{
    static MG_THREAD_LOCAL char text[16];
    const u8 *a = (const u8 *)&in;
    snprintf(text, sizeof(text), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    return text;
}

/* ------------------------------------------------- services and protocols */

static const struct {
    const char *name;
    u16 port;
    bool tcp, udp;
} k_services[] = {
    { "echo", 7, true, true }, { "discard", 9, true, true }, { "daytime", 13, true, true }, { "ftp-data", 20, true, false }, { "ftp", 21, true, false },
    { "ssh", 22, true, false }, { "telnet", 23, true, false }, { "smtp", 25, true, false }, { "time", 37, true, true }, { "domain", 53, true, true },
    { "bootps", 67, false, true }, { "bootpc", 68, false, true }, { "tftp", 69, false, true }, { "finger", 79, true, false }, { "http", 80, true, true },
    { "www", 80, true, true }, { "kerberos", 88, true, true }, { "pop3", 110, true, false }, { "sunrpc", 111, true, true }, { "nntp", 119, true, false },
    { "ntp", 123, true, true }, { "imap", 143, true, false }, { "snmp", 161, false, true }, { "ldap", 389, true, true }, { "https", 443, true, true },
    { "submission", 587, true, false }, { "ldaps", 636, true, false }, { "imaps", 993, true, false }, { "pop3s", 995, true, false },
    { "socks", 1080, true, true }, { "mysql", 3306, true, false }, { "sip", 5060, true, true }, { "xmpp-client", 5222, true, false },
    { "http-alt", 8080, true, false },
};

/* A service's port by its name for a socket type (0: either); -1 when there is none. */
static int service_port(const char *name, int socktype)
{
    size_t i;
    for (i = 0; i < MG_COUNT(k_services); i++)
        if (!strcmp(k_services[i].name, name) && (socktype != G_SOCK_DGRAM || k_services[i].udp) && (socktype != G_SOCK_STREAM || k_services[i].tcp))
            return k_services[i].port;
    return -1;
}

static const char *service_name(int port, bool udp)
{
    size_t i;
    for (i = 0; i < MG_COUNT(k_services); i++)
        if (k_services[i].port == port && (udp ? k_services[i].udp : k_services[i].tcp)) return k_services[i].name;
    return NULL;
}

static g_servent *servent_of(const char *name, int port, const char *proto)
{
    static MG_THREAD_LOCAL g_servent ent;
    static MG_THREAD_LOCAL char *aliases[1];
    static MG_THREAD_LOCAL char name_buf[32], proto_buf[4];
    mg_strlcpy(name_buf, name, sizeof(name_buf));
    mg_strlcpy(proto_buf, proto, sizeof(proto_buf));
    aliases[0] = NULL;
    ent.s_name = name_buf;
    ent.s_aliases = aliases;
    ent.s_port = g_htons((u16)port);
    ent.s_proto = proto_buf;
    return &ent;
}

static g_servent *g_getservbyname(const char *name, const char *proto)
{
    bool udp = proto && !strcmp(proto, "udp");
    int port = name ? service_port(name, proto ? (udp ? G_SOCK_DGRAM : G_SOCK_STREAM) : 0) : -1;
    return port < 0 ? NULL : servent_of(name, port, udp ? "udp" : "tcp");
}

static g_servent *g_getservbyport(int port_be, const char *proto)
{
    bool udp = proto && !strcmp(proto, "udp");
    const char *name = service_name(g_htons((u16)port_be), udp);
    return name ? servent_of(name, g_htons((u16)port_be), udp ? "udp" : "tcp") : NULL;
}

static const struct {
    const char *name;
    int number;
} k_protocols[] = { { "ip", 0 }, { "icmp", 1 }, { "tcp", 6 }, { "udp", 17 }, { "ipv6", 41 }, { "ipv6-icmp", 58 } };

static g_protoent *protoent_at(size_t i)
{
    static MG_THREAD_LOCAL g_protoent ent;
    static MG_THREAD_LOCAL char *aliases[1];
    aliases[0] = NULL;
    ent.p_name = (char *)k_protocols[i].name;
    ent.p_aliases = aliases;
    ent.p_proto = k_protocols[i].number;
    return &ent;
}

static g_protoent *g_getprotobyname(const char *name)
{
    size_t i;
    for (i = 0; name && i < MG_COUNT(k_protocols); i++)
        if (!strcmp(k_protocols[i].name, name)) return protoent_at(i);
    return NULL;
}

static g_protoent *g_getprotobynumber(int number)
{
    size_t i;
    for (i = 0; i < MG_COUNT(k_protocols); i++)
        if (k_protocols[i].number == number) return protoent_at(i);
    return NULL;
}

/* ------------------------------------------------------------- interfaces */

static bool is_loopback_v6(const u8 *a)
{
    static const u8 one[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    return !memcmp(a, one, 16);
}

int mg_net_interfaces_android(mg_gif **out)
{
    enum { MAX = 32 };
    mg_netif *host = (mg_netif *)mg_alloc(sizeof(mg_netif) * MAX);
    mg_gif *list;
    int n = mg_net_interfaces(host, MAX), kept = 0, pass, i, wlan = 0, eth = 0, rmnet = 0, ppp = 0, other = 0;
    *out = NULL;
    if (n < 0) {
        /* The host would not list them. The loopback is there all the same, and an app that lists interfaces at its start (java.net.NetworkInterface) had better find it than fail. */
        mg_note_once("network: the host would not list its network interfaces; an app is shown the loopback alone");
        memset(host, 0, sizeof(mg_netif));
        host[0].index = 1;
        host[0].kind = MG_IF_LOOPBACK;
        host[0].flags = MG_IFF_UP | MG_IFF_RUNNING;
        host[0].mtu = 65536;
        host[0].naddrs = 2;
        host[0].addrs[0].addr.family = MG_AF_INET;
        host[0].addrs[0].addr.addr[0] = 127;
        host[0].addrs[0].addr.addr[3] = 1;
        host[0].addrs[0].prefix = 8;
        host[0].addrs[1].addr.family = MG_AF_INET6;
        host[0].addrs[1].addr.addr[15] = 1;
        host[0].addrs[1].prefix = 128;
        n = 1;
    }
    if (n > MAX) n = MAX;
    list = (mg_gif *)mg_alloc(sizeof(mg_gif) * (size_t)(n + 1));
    /* The loopback first, as Linux lists it; then those that are up, but the host's own tunnels. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            mg_netif *f = &host[i];
            mg_gif *g;
            if ((pass == 0) != (f->kind == MG_IF_LOOPBACK)) continue;
            if (f->kind == MG_IF_LOOPBACK && kept) continue;
            if (f->kind != MG_IF_LOOPBACK && (f->kind == MG_IF_TUNNEL || !(f->flags & MG_IFF_UP))) continue;
            g = &list[kept++];
            g->host = *f;
            switch (f->kind) {
            case MG_IF_LOOPBACK: mg_strlcpy(g->name, "lo", sizeof(g->name)); break;
            case MG_IF_WIFI: snprintf(g->name, sizeof(g->name), "wlan%d", wlan++); break;
            case MG_IF_ETHERNET: snprintf(g->name, sizeof(g->name), "eth%d", eth++); break;
            case MG_IF_CELLULAR: snprintf(g->name, sizeof(g->name), "rmnet%d", rmnet++); break;
            default:
                if (f->flags & MG_IFF_POINTOPOINT) snprintf(g->name, sizeof(g->name), "ppp%d", ppp++);
                else snprintf(g->name, sizeof(g->name), "if%d", other++);
                break;
            }
        }
    }
    free(host);
    *out = list;
    return kept;
}

u32 mg_net_if_flags(const mg_gif *f)
{
    u32 flags = 0;
    if (f->host.flags & MG_IFF_UP) flags |= G_IFF_UP;
    if (f->host.flags & MG_IFF_RUNNING) flags |= G_IFF_RUNNING | G_IFF_LOWER_UP;
    if (f->host.kind == MG_IF_LOOPBACK) flags |= G_IFF_LOOPBACK;
    if (f->host.flags & MG_IFF_MULTICAST) flags |= G_IFF_MULTICAST;
    if (f->host.flags & MG_IFF_BROADCAST) flags |= G_IFF_BROADCAST;
    if (f->host.flags & MG_IFF_POINTOPOINT) flags |= G_IFF_POINTOPOINT | G_IFF_NOARP;
    return flags;
}

static void mask_of(int family, int prefix, u8 *out)
{
    int len = family == MG_AF_INET ? 4 : 16, i;
    memset(out, 0, 16);
    for (i = 0; i < len * 8 && i < prefix; i++) out[i / 8] |= (u8)(0x80 >> (i % 8));
}

/* The sockaddr_ll of an interface's link (AF_PACKET), as getifaddrs gives it on Linux. */
static u32 link_addr(const mg_gif *f, u8 *out)
{
    memset(out, 0, 20);
    mg_wr16(out, G_AF_PACKET);
    mg_wr32(out + 4, f->host.index);
    mg_wr16(out + 8, f->host.kind == MG_IF_LOOPBACK ? 772 : 1);   /* ARPHRD_LOOPBACK, ARPHRD_ETHER */
    if (f->host.kind == MG_IF_LOOPBACK) out[11] = 6;              /* (a loopback's is six zeros on Linux) */
    else if (f->host.hw_len > 0) {
        out[11] = (u8)f->host.hw_len;
        memcpy(out + 12, f->host.hw, (size_t)f->host.hw_len);
    }
    return 20;
}

/* One entry of getifaddrs's list, with its name and addresses in the same block (freeifaddrs frees each entry). */
typedef struct ifa_block {
    g_ifaddrs ifa;
    char name[16];
    u8 addr[G_SIN6_LEN], mask[G_SIN6_LEN], other[G_SIN6_LEN];
} ifa_block;

static int g_getifaddrs(g_ifaddrs **out)
{
    mg_gif *list = NULL;
    g_ifaddrs *head = NULL, **tail = &head;
    int n, i, k;
    if (!out) return -1;
    n = mg_net_interfaces_android(&list);
    if (n < 0) {
        mg_set_errno(GE_IO);
        return -1;
    }
    for (i = 0; i < n; i++) {
        /* The link of each first, as Linux's netlink answers list them, then every address of it. */
        ifa_block *b = (ifa_block *)mg_g_calloc(1, sizeof(ifa_block));
        mg_strlcpy(b->name, list[i].name, sizeof(b->name));
        b->ifa.ifa_name = b->name;
        b->ifa.ifa_flags = mg_net_if_flags(&list[i]);
        link_addr(&list[i], b->addr);
        b->ifa.ifa_addr = b->addr;
        *tail = &b->ifa;
        tail = &b->ifa.ifa_next;
    }
    for (i = 0; i < n; i++) {
        for (k = 0; k < list[i].host.naddrs; k++) {
            ifa_block *b = (ifa_block *)mg_g_calloc(1, sizeof(ifa_block));
            mg_sockaddr a = list[i].host.addrs[k].addr, m;
            bool v6 = a.family == MG_AF_INET6;
            mg_strlcpy(b->name, list[i].name, sizeof(b->name));
            b->ifa.ifa_name = b->name;
            b->ifa.ifa_flags = mg_net_if_flags(&list[i]);
            /* A link-local address names its interface, as Linux's do. */
            if (v6 && a.addr[0] == 0xFE && (a.addr[1] & 0xC0) == 0x80) a.scope_id = list[i].host.index;
            mg_net_sockaddr_out(&a, v6, b->addr);
            memset(&m, 0, sizeof(m));
            m.family = a.family;
            mask_of(a.family, list[i].host.addrs[k].prefix, m.addr);
            mg_net_sockaddr_out(&m, v6, b->mask);
            b->ifa.ifa_addr = b->addr;
            b->ifa.ifa_netmask = b->mask;
            /* An IPv4 address on a broadcast link has its broadcast address; an IPv6 one never (libcore takes it for an IPv4 one). */
            if (!v6 && (list[i].host.flags & MG_IFF_BROADCAST)) {
                mg_sockaddr bc = a;
                int j;
                for (j = 0; j < 4; j++) bc.addr[j] |= (u8)~m.addr[j];
                mg_net_sockaddr_out(&bc, false, b->other);
                b->ifa.ifa_ifu = b->other;
            }
            *tail = &b->ifa;
            tail = &b->ifa.ifa_next;
        }
    }
    free(list);
    *out = head;
    return 0;
}

static void g_freeifaddrs(g_ifaddrs *list)
{
    while (list) {
        g_ifaddrs *next = list->ifa_next;
        mg_g_free(list);
        list = next;
    }
}

static u32 g_if_nametoindex(const char *name)
{
    mg_gif *list = NULL;
    int n = mg_net_interfaces_android(&list), i;
    u32 index = 0;
    for (i = 0; name && i < n && !index; i++)
        if (!strcmp(list[i].name, name)) index = list[i].host.index;
    free(list);
    if (!index) mg_set_errno(GE_NODEV);
    return index;
}

static char *g_if_indextoname(u32 index, char *out)
{
    mg_gif *list = NULL;
    int n = mg_net_interfaces_android(&list), i;
    char *r = NULL;
    for (i = 0; i < n && !r; i++)
        if (list[i].host.index == index) r = (char *)memcpy(out, list[i].name, strlen(list[i].name) + 1);
    free(list);
    if (!r) mg_set_errno(GE_NXIO);
    return r;
}

static g_nameindex *g_if_nameindex(void)
{
    mg_gif *list = NULL;
    int n = mg_net_interfaces_android(&list), i;
    g_nameindex *v;
    if (n < 0) {
        mg_set_errno(GE_NOBUFS);
        return NULL;
    }
    v = (g_nameindex *)mg_g_calloc((size_t)n + 1, sizeof(g_nameindex));
    for (i = 0; i < n; i++) {
        v[i].if_index = list[i].host.index;
        v[i].if_name = mg_g_strdup(list[i].name);
    }
    free(list);
    return v;
}

static void g_if_freenameindex(g_nameindex *v)
{
    g_nameindex *p;
    for (p = v; p && p->if_name; p++) mg_g_free(p->if_name);
    mg_g_free(v);
}

/*
 * struct ifreq: the interface's name (16 bytes), then a union that is a
 * sockaddr, a short (flags) or an int (index, MTU) as the request has it.
 * Its largest member, struct ifmap, holds two unsigned longs, so the union
 * is 16 bytes in 32-bit Android and 24 in 64-bit.
 */
#define G_IFREQ (16u + (MG_GUEST_LP64 ? 24u : 16u))

static const mg_gif *if_named(const mg_gif *list, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strncmp(list[i].name, name, 16)) return &list[i];
    return NULL;
}

static const mg_sockaddr *first_v4(const mg_gif *f)
{
    int k;
    for (k = 0; k < f->host.naddrs; k++)
        if (f->host.addrs[k].addr.family == MG_AF_INET) return &f->host.addrs[k].addr;
    return NULL;
}

int mg_net_if_ioctl(unsigned request, void *arg)
{
    u8 *rq = (u8 *)arg;
    mg_gif *list = NULL;
    const mg_gif *f;
    const mg_sockaddr *a;
    mg_sockaddr x;
    int n, e = 0, i, k;
    switch (request) {
    case 0x8912: case 0x8913: case 0x8915: case 0x8919: case 0x891b: case 0x8921: case 0x8927: case 0x8933: case 0x8910: break;
    case 0x8914: case 0x8916: case 0x891a: case 0x891c: case 0x8922: case 0x8924:
        /* SIOCSIF*: changing an interface is not an app's to do. */
        mg_set_errno(GE_PERM);
        return -1;
    default:
        return -2;
    }
    if (!rq) {
        mg_set_errno(GE_FAULT);
        return -1;
    }
    n = mg_net_interfaces_android(&list);
    if (n < 0) n = 0;
    if (request == 0x8912) {
        /* SIOCGIFCONF: a struct ifconf -- a length, then where the ifreqs go -- with an ifreq for each IPv4 address, as Linux has it. */
        int room = (int)mg_rd32(rq), used = 0;
        /* (The pointer after the length is aligned as a pointer is.) */
        u8 *buf = *(u8 **)(rq + sizeof(void *));
        for (i = 0; i < n; i++) {
            for (k = 0; k < list[i].host.naddrs; k++) {
                if (list[i].host.addrs[k].addr.family != MG_AF_INET) continue;
                if (buf) {
                    if (used + (int)G_IFREQ > room) break;
                    memset(buf + used, 0, G_IFREQ);
                    mg_strlcpy((char *)buf + used, list[i].name, 16);
                    mg_net_sockaddr_out(&list[i].host.addrs[k].addr, false, buf + used + 16);
                }
                used += (int)G_IFREQ;
            }
        }
        mg_wr32(rq, (u32)used);
        free(list);
        return 0;
    }
    if (request == 0x8910) {
        /* SIOCGIFNAME: the name of the index. */
        u32 index = mg_rd32(rq + 16);
        for (i = 0; i < n && list[i].host.index != index; i++) { }
        if (i < n) mg_strlcpy((char *)rq, list[i].name, 16);
        free(list);
        if (i < n) return 0;
        mg_set_errno(GE_NODEV);
        return -1;
    }
    f = if_named(list, n, (const char *)rq);
    if (!f) {
        free(list);
        mg_set_errno(GE_NODEV);
        return -1;
    }
    memset(rq + 16, 0, 16);
    switch (request) {
    case 0x8913: mg_wr16(rq + 16, (u16)mg_net_if_flags(f)); break;      /* SIOCGIFFLAGS */
    case 0x8921: mg_wr32(rq + 16, f->host.mtu); break;                  /* SIOCGIFMTU */
    case 0x8933: mg_wr32(rq + 16, f->host.index); break;                /* SIOCGIFINDEX */
    case 0x8927:                                                         /* SIOCGIFHWADDR: a sockaddr whose family is the hardware's type */
        mg_wr16(rq + 16, f->host.kind == MG_IF_LOOPBACK ? 772 : 1);
        if (f->host.kind != MG_IF_LOOPBACK && f->host.hw_len > 0) memcpy(rq + 18, f->host.hw, f->host.hw_len < 14 ? (size_t)f->host.hw_len : 14);
        break;
    default:                                                             /* SIOCGIFADDR, SIOCGIFBRDADDR, SIOCGIFNETMASK: of its first IPv4 address */
        a = first_v4(f);
        if (!a) {
            e = GE_ADDRNOTAVAIL;
            break;
        }
        x = *a;
        if (request != 0x8915) {
            int prefix = 0;
            for (k = 0; k < f->host.naddrs; k++)
                if (&f->host.addrs[k].addr == a) prefix = f->host.addrs[k].prefix;
            mask_of(MG_AF_INET, prefix, x.addr);
            if (request == 0x8919) {
                for (k = 0; k < 4; k++) x.addr[k] = (u8)(a->addr[k] | (u8)~x.addr[k]);
            }
        }
        mg_net_sockaddr_out(&x, false, rq + 16);
        break;
    }
    free(list);
    if (e) {
        mg_set_errno(e);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- resolving */

/* What needs no network: "localhost" and its kin, as Android's /system/etc/hosts has them. 0: not one of them; else how many addresses. */
static int known_name(const char *name, int family, mg_sockaddr out[2])
{
    bool v4 = !strcmp(name, "localhost") || !strcmp(name, "localhost.localdomain");
    bool v6 = !strcmp(name, "ip6-localhost") || !strcmp(name, "ip6-loopback");
    memset(out, 0, sizeof(mg_sockaddr) * 2);
    if (!v4 && !v6) return 0;
    if (v4 && family != G_AF_INET6) {
        out[0].family = MG_AF_INET;
        out[0].addr[0] = 127;
        out[0].addr[3] = 1;
        return 1;
    }
    /* (localhost asked for as IPv6 alone is ::1, which Android's file does not say; no app is better served by a failure there.) */
    if (family != G_AF_INET) {
        out[0].family = MG_AF_INET6;
        out[0].addr[15] = 1;
        return 1;
    }
    return -1;
}

/* A number as an address, with a scope after "%" for IPv6 (an index or an interface's name). */
static bool numeric_host(const char *s, mg_sockaddr *a)
{
    char text[64];
    const char *scope = strchr(s, '%');
    u32 v4;
    memset(a, 0, sizeof(*a));
    if (strlen(s) >= sizeof(text)) return false;
    mg_strlcpy(text, s, sizeof(text));
    if (scope) text[scope - s] = 0;
    if (parse_v6(text, a->addr)) {
        a->family = MG_AF_INET6;
        if (scope) {
            char *end;
            unsigned long index = strtoul(scope + 1, &end, 10);
            if (*end || end == scope + 1) {
                index = g_if_nametoindex(scope + 1);
                if (!index) return false;
            }
            a->scope_id = (u32)index;
        }
        return true;
    }
    if (!scope && g_inet_aton(text, &v4)) {
        a->family = MG_AF_INET;
        memcpy(a->addr, &v4, 4);
        return true;
    }
    return false;
}

static int eai_of(int host_eai)
{
    switch (host_eai) {
    case MG_EAI_OK: return 0;
    case MG_EAI_NONAME: return G_EAI_NODATA;
    case MG_EAI_AGAIN: return G_EAI_AGAIN;
    case MG_EAI_NODATA: return G_EAI_NODATA;
    case MG_EAI_FAMILY: return G_EAI_FAMILY;
    case MG_EAI_MEMORY: return G_EAI_MEMORY;
    case MG_EAI_SYSTEM:
        mg_set_errno(GE_IO);
        return G_EAI_SYSTEM;
    default: return G_EAI_FAIL;
    }
}

static const char *eai_text(int code)
{
    static const char *const text[] = {
        "Success", "Address family for hostname not supported", "Temporary failure in name resolution", "Invalid value for ai_flags",
        "Non-recoverable failure in name resolution", "ai_family not supported", "Memory allocation failure", "No address associated with hostname",
        "Name or service not known", "Servname not supported for ai_socktype", "ai_socktype not supported", "System error",
        "Invalid value for hints", "Resolved protocol is unknown", "Argument buffer overflow",
    };
    return code >= 0 && code < (int)MG_COUNT(text) ? text[code] : "Unknown error";
}

/* The first name an app has the host's resolver look up (numbers and "localhost" are not): the name, which is the app's server's, and how many addresses; not the addresses. */
static void said_first_name(const char *name, int e, int count)
{
    static volatile i32 said;
    if (said || mg_atomic_xchg(&said, 1)) return;
    if (e) mg_info("network: the first name the app asked the resolver for, \"%.100s\", was not found: %s", name, eai_text(e));
    else mg_info("network: the first name the app asked the resolver for, \"%.100s\", has %d address%s", name, count, count == 1 ? "" : "es");
}

/*
 * A name of Google's location server (VoilaTileService, which Google Play services' network location asks where the Wi-Fi
 * networks a scan heard are -- GmsCore's GmsCoreVoilaTileGrpc): whether the app's Google Play services looked it up at all,
 * the first time it was found, and each time it was not (four lines at most), so that a phone's log tells a network location
 * that never asked the server from one that asked and had no fix all the same (what the server answered is inside TLS).
 */
static void said_location_server(const char *name, int e, int count)
{
    static volatile i32 found, failed;
    if (strcmp(name, "voilatile-pa.googleapis.com") && strcmp(name, "voilatile-pa.mtls.googleapis.com")) return;
    if (!e) {
        if (!found && !mg_atomic_xchg(&found, 1))
            mg_info("network: Google's location server, \"%.100s\" (Google Play services' network location asks it where Wi-Fi networks are), has %d address%s",
                    name, count, count == 1 ? "" : "es");
    } else if (mg_atomic_add(&failed, 1) <= 3) {
        mg_info("network: Google's location server, \"%.100s\", was not found: %s", name, eai_text(e));
    }
}

/*
 * The addresses of a name for getaddrinfo and gethostbyname: a number, one of
 * the names Android knows without asking, or the host's resolver's answer.
 * `numeric_only`: AI_NUMERICHOST. 0 and a malloc'd array, or an EAI_* code.
 */
static int addresses_of(const char *node, int family, bool numeric_only, bool addrconfig, bool want_canon, mg_sockaddr **out, int *count, char **canon)
{
    mg_sockaddr known[2];
    int n, e;
    *out = NULL;
    *count = 0;
    if (canon) *canon = NULL;
    if (numeric_host(node, &known[0])) {
        if ((family == G_AF_INET && known[0].family != MG_AF_INET) || (family == G_AF_INET6 && known[0].family != MG_AF_INET6)) return G_EAI_NONAME;
        n = 1;
    } else if (numeric_only) {
        return G_EAI_NONAME;
    } else if ((n = known_name(node, family, known)) != 0) {
        if (n < 0) return G_EAI_NODATA;
    } else {
        int hf = family == G_AF_INET ? MG_AF_INET : family == G_AF_INET6 ? MG_AF_INET6 : 0, host_e;
        mg_sockaddr *found = NULL;
        u64 began = mg_time_monotonic_ns();
        host_e = mg_net_resolve(node, hf, (want_canon ? MG_RESOLVE_CANONICAL : 0) | (addrconfig ? MG_RESOLVE_ADDRCONFIG : 0), &found, &n, canon);
        e = eai_of(host_e);
        /* What the host's resolver was asked and how soon it answered: a name found at one time and not at another (on a
           phone with a VPN, Facebook's) is told apart by these. */
        mg_net_trace("the resolver on %s (%s%s): %s%s after %u ms", node, hf == MG_AF_INET ? "IPv4" : hf == MG_AF_INET6 ? "IPv6" : "any family",
                     addrconfig ? ", AI_ADDRCONFIG" : "", host_e == MG_EAI_OK ? "found" : host_e == MG_EAI_NONAME ? "no such name" : host_e == MG_EAI_NODATA ? "no addresses" : "failed",
                     host_e == MG_EAI_OK || host_e == MG_EAI_NONAME || host_e == MG_EAI_NODATA ? "" : " (an error of its own)",
                     (unsigned)((mg_time_monotonic_ns() - began) / 1000000u));
        said_first_name(node, e, n);
        said_location_server(node, e, n);
        if (e) return e;
        *out = found;
        *count = n;
        return 0;
    }
    *out = (mg_sockaddr *)mg_alloc(sizeof(mg_sockaddr) * (size_t)n);
    memcpy(*out, known, sizeof(mg_sockaddr) * (size_t)n);
    *count = n;
    if (canon && want_canon) *canon = mg_strdup(node);
    return 0;
}

static g_addrinfo *new_entry(const mg_sockaddr *a, int flags, int socktype, int protocol, const char *canon)
{
    size_t name_len = canon ? strlen(canon) + 1 : 0;
    g_addrinfo *ai = (g_addrinfo *)mg_g_calloc(1, sizeof(g_addrinfo) + G_SIN6_LEN + name_len);
    u8 *addr = (u8 *)(ai + 1);
    ai->ai_flags = flags;
    ai->ai_family = a->family == MG_AF_INET6 ? G_AF_INET6 : G_AF_INET;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    ai->ai_addrlen = mg_net_sockaddr_out(a, a->family == MG_AF_INET6, addr);
    ai->ai_addr = addr;
    if (canon) {
        ai->ai_canonname = (char *)addr + G_SIN6_LEN;
        memcpy(ai->ai_canonname, canon, name_len);
    }
    return ai;
}

static void g_freeaddrinfo(g_addrinfo *ai)
{
    while (ai) {
        g_addrinfo *next = ai->ai_next;
        mg_g_free(ai);
        ai = next;
    }
}

static int g_getaddrinfo(const char *node, const char *service, const g_addrinfo *hints, g_addrinfo **res)
{
    static const int types[] = { G_SOCK_DGRAM, G_SOCK_STREAM };
    int flags = 0, family = G_AF_UNSPEC, socktype = 0, protocol = 0, port = 0, count = 0, e, t, i;
    mg_sockaddr *addrs = NULL;
    char *canon = NULL;
    g_addrinfo *head = NULL, **tail = &head;
    if (!res) return G_EAI_FAIL;
    *res = NULL;
    if (hints) {
        if (hints->ai_addrlen || hints->ai_canonname || hints->ai_addr || hints->ai_next) return G_EAI_BADHINTS;
        flags = hints->ai_flags;
        family = hints->ai_family;
        socktype = hints->ai_socktype;
        protocol = hints->ai_protocol;
        if (flags & ~G_AI_MASK) return G_EAI_BADFLAGS;
        if (family != G_AF_UNSPEC && family != G_AF_INET && family != G_AF_INET6) return G_EAI_FAMILY;
        if (socktype && socktype != G_SOCK_STREAM && socktype != G_SOCK_DGRAM && socktype != G_SOCK_RAW) return G_EAI_SOCKTYPE;
        if ((socktype == G_SOCK_STREAM && protocol && protocol != 6) || (socktype == G_SOCK_DGRAM && protocol && protocol != 17)) return G_EAI_BADHINTS;
    }
    if ((!node || !*node) && (!service || !*service)) return G_EAI_NONAME;
    if ((flags & G_AI_CANONNAME) && !node) return G_EAI_BADFLAGS;
    if (service && *service) {
        char *end;
        unsigned long v = strtoul(service, &end, 10);
        if (socktype == G_SOCK_RAW) return G_EAI_SERVICE;
        if (!*end && end != service) {
            if (v > 65535) return G_EAI_SERVICE;
            port = (int)v;
        } else {
            if (flags & G_AI_NUMERICSERV) return G_EAI_NONAME;
            if ((port = service_port(service, socktype)) < 0) return G_EAI_SERVICE;
        }
    }
    if (!node || !*node) {
        /* No name: the wildcard address to bind to (AI_PASSIVE), or the loopback to connect to; IPv6 first, as bionic lists them. */
        addrs = (mg_sockaddr *)mg_alloc(sizeof(mg_sockaddr) * 2);
        if (family != G_AF_INET) {
            addrs[count].family = MG_AF_INET6;
            if (!(flags & G_AI_PASSIVE)) addrs[count].addr[15] = 1;
            count++;
        }
        if (family != G_AF_INET6) {
            addrs[count].family = MG_AF_INET;
            if (!(flags & G_AI_PASSIVE)) {
                addrs[count].addr[0] = 127;
                addrs[count].addr[3] = 1;
            }
            count++;
        }
    } else if ((e = addresses_of(node, family, (flags & G_AI_NUMERICHOST) != 0, (flags & G_AI_ADDRCONFIG) != 0, (flags & G_AI_CANONNAME) != 0, &addrs, &count, &canon)) != 0) {
        mg_net_trace("the name %s: not found (getaddrinfo error %d)", node, e);
        return e;
    }
    if (node && *node && mg_net_trace_wanted()) {
        char first[64] = "";
        if (count) g_inet_ntop(addrs[0].family == MG_AF_INET6 ? G_AF_INET6 : G_AF_INET, addrs[0].addr, first, sizeof(first));
        mg_net_trace("the name %s: %d addresses, the first %s", node, count, first);
    }
    /* With no type asked for, every address once as a datagram one and once as a stream one, in that order, as bionic gives them. */
    for (t = 0; t < (socktype ? 1 : 2); t++) {
        int type = socktype ? socktype : types[t];
        for (i = 0; i < count; i++) {
            mg_sockaddr a = addrs[i];
            g_addrinfo *ai;
            a.port = (u16)port;
            ai = new_entry(&a, flags, type, type == G_SOCK_STREAM ? 6 : type == G_SOCK_DGRAM ? 17 : protocol, head ? NULL : (flags & G_AI_CANONNAME) ? (canon ? canon : node) : NULL);
            *tail = ai;
            tail = &ai->ai_next;
        }
    }
    free(addrs);
    free(canon);
    *res = head;
    return head ? 0 : G_EAI_NODATA;
}

/* libcore's InetAddress asks through this, naming the network (which here is the one there is). */
static int g_android_getaddrinfofornet(const char *node, const char *service, const g_addrinfo *hints, unsigned netid, unsigned mark, g_addrinfo **res)
{
    (void)netid;
    (void)mark;
    return g_getaddrinfo(node, service, hints, res);
}

static const char *g_gai_strerror(int code) { return eai_text(code); }

static int g_getnameinfo(const void *sa, u32 salen, char *host, size_t hostlen, char *serv, size_t servlen, int flags)
{
    mg_sockaddr a;
    char text[1025];
    int e;
    if (!sa) return G_EAI_FAIL;
    if ((e = mg_net_sockaddr_in(sa, salen, &a)) != 0 || !a.family) return e == GE_AFNOSUPPORT || !a.family ? G_EAI_FAMILY : G_EAI_FAIL;
    if (serv && servlen) {
        const char *name = (flags & G_NI_NUMERICSERV) ? NULL : service_name(a.port, (flags & G_NI_DGRAM) != 0);
        if (name) mg_strlcpy(text, name, sizeof(text));
        else snprintf(text, sizeof(text), "%u", (unsigned)a.port);
        if (strlen(text) + 1 > servlen) return G_EAI_OVERFLOW;
        strcpy(serv, text);
    }
    if (host && hostlen) {
        bool done = false;
        if (!(flags & G_NI_NUMERICHOST)) {
            /* The names Android knows without asking, and then the host's resolver. */
            if (a.family == MG_AF_INET && a.addr[0] == 127 && !a.addr[1] && !a.addr[2] && a.addr[3] == 1) {
                mg_strlcpy(text, "localhost", sizeof(text));
                done = true;
            } else if (a.family == MG_AF_INET6 && is_loopback_v6(a.addr)) {
                mg_strlcpy(text, "ip6-localhost", sizeof(text));
                done = true;
            } else {
                done = mg_net_name_of(&a, text, sizeof(text)) == MG_EAI_OK;
            }
            if (done && (flags & G_NI_NOFQDN)) {
                char *dot = strchr(text, '.');
                if (dot) *dot = 0;
            }
        }
        if (!done) {
            if (flags & G_NI_NAMEREQD) return G_EAI_NONAME;
            g_inet_ntop(a.family == MG_AF_INET6 ? G_AF_INET6 : G_AF_INET, a.addr, text, sizeof(text));
            if (a.family == MG_AF_INET6 && a.scope_id) snprintf(text + strlen(text), sizeof(text) - strlen(text), "%%%u", (unsigned)a.scope_id);
        }
        if (strlen(text) + 1 > hostlen) return G_EAI_OVERFLOW;
        strcpy(host, text);
    }
    return 0;
}

/* ------------------------------------------------- gethostbyname and kin */

#define HOST_MAX_ADDRS 16
#define HOST_ROOM 2048

/* What the plain calls fill, one for each thread, as bionic keeps it: the hostent, and room after it for what it points to. */
typedef struct host_store {
    g_hostent ent;
    void *align;
} host_store;

/*
 * res_search: a DNS query's raw answer, which the host's resolver here does not give (it answers by name: getaddrinfo).
 * -1 and NO_RECOVERY, and the caller goes another way. Go's net package links it, for its cgo resolver's LookupTXT and
 * the like: WhatsApp's libgojni.so imports it, and an import not provided is a stub that ends the app when called.
 */
static int g_res_search(const char *name, int cls, int type, u8 *answer, int anslen)
{
    (void)name; (void)cls; (void)type; (void)answer; (void)anslen;
    mg_note_once("res_search: raw DNS queries are not answered here; the caller is told NO_RECOVERY");
    t_h_errno = G_NO_RECOVERY;
    return -1;
}

static int h_errno_of(int eai) { return eai == G_EAI_AGAIN ? G_TRY_AGAIN : eai == G_EAI_NODATA ? G_NO_DATA : eai == G_EAI_FAIL ? G_NO_RECOVERY : G_HOST_NOT_FOUND; }

/* Fills a hostent into `buf` (its own pointers into the same buffer): false when it does not fit. */
static bool host_fill(g_hostent *ent, u8 *buf, size_t size, const char *name, int af, const mg_sockaddr *addrs, int count)
{
    size_t len = af == G_AF_INET6 ? 16 : 4, need, name_len = strlen(name) + 1, at = 0, pad = (sizeof(char *) - (uintptr_t)buf % sizeof(char *)) % sizeof(char *);
    char **list;
    int i;
    if (count > HOST_MAX_ADDRS) count = HOST_MAX_ADDRS;
    need = pad + sizeof(char *) * (size_t)(count + 2) + len * (size_t)count + name_len;
    if (!buf || size < need) return false;
    buf += pad;
    /* The pointer arrays first, where they are aligned. */
    list = (char **)buf;
    at = sizeof(char *) * (size_t)(count + 2);
    for (i = 0; i < count; i++) {
        const u8 *a = addrs[i].addr;
        u8 mapped[16];
        if (af == G_AF_INET6 && addrs[i].family == MG_AF_INET) {
            memset(mapped, 0, 16);
            mapped[10] = mapped[11] = 0xFF;
            memcpy(mapped + 12, addrs[i].addr, 4);
            a = mapped;
        }
        memcpy(buf + at, a, len);
        list[i] = (char *)buf + at;
        at += len;
    }
    list[count] = NULL;
    list[count + 1] = NULL;                    /* the aliases: none */
    memcpy(buf + at, name, name_len);
    ent->h_name = (char *)buf + at;
    ent->h_aliases = &list[count + 1];
    ent->h_addrtype = af;
    ent->h_length = (int)len;
    ent->h_addr_list = list;
    return true;
}

/* gethostbyname2_r's work: 0 with *result, or ERANGE; a name not found is 0 with *result NULL and *herr set. */
static int host_by_name(const char *name, int af, g_hostent *ent, u8 *buf, size_t size, g_hostent **result, int *herr)
{
    mg_sockaddr *addrs = NULL, *kept;
    char *canon = NULL;
    int count = 0, e, n = 0, i;
    *result = NULL;
    if (af != G_AF_INET && af != G_AF_INET6) {
        *herr = G_NO_RECOVERY;
        mg_set_errno(GE_AFNOSUPPORT);
        return 0;
    }
    if (!name) {
        *herr = G_HOST_NOT_FOUND;
        return 0;
    }
    e = addresses_of(name, af, false, false, true, &addrs, &count, &canon);
    if (e) {
        *herr = h_errno_of(e);
        return 0;
    }
    /* (Of the family asked for: an IPv4 answer to an IPv6 question is given as ::ffff:a.b.c.d.) */
    kept = (mg_sockaddr *)mg_alloc(sizeof(mg_sockaddr) * (size_t)(count ? count : 1));
    for (i = 0; i < count; i++)
        if (af == G_AF_INET6 || addrs[i].family == MG_AF_INET) kept[n++] = addrs[i];
    free(addrs);
    if (!n) {
        free(kept);
        free(canon);
        *herr = G_NO_DATA;
        return 0;
    }
    if (!host_fill(ent, buf, size, canon ? canon : name, af, kept, n)) {
        free(kept);
        free(canon);
        *herr = G_NO_RECOVERY;
        return GE_RANGE;
    }
    free(kept);
    free(canon);
    *herr = 0;
    *result = ent;
    return 0;
}

static host_store *store_of_thread(void)
{
    static MG_THREAD_LOCAL host_store *s;
    if (!s) s = (host_store *)mg_alloc(sizeof(host_store) + HOST_ROOM);
    return s;
}

static g_hostent *g_gethostbyname2(const char *name, int af)
{
    host_store *s = store_of_thread();
    g_hostent *r = NULL;
    int herr = 0;
    host_by_name(name, af, &s->ent, (u8 *)(s + 1), HOST_ROOM, &r, &herr);
    t_h_errno = herr;
    return r;
}

static g_hostent *g_gethostbyname(const char *name) { return g_gethostbyname2(name, G_AF_INET); }

static int g_gethostbyname2_r(const char *name, int af, g_hostent *ent, char *buf, size_t size, g_hostent **result, int *herr)
{
    int dummy, r;
    if (!herr) herr = &dummy;
    r = host_by_name(name, af, ent, (u8 *)buf, size, result, herr);
    if (r) mg_set_errno(r);
    return r;
}

static int g_gethostbyname_r(const char *name, g_hostent *ent, char *buf, size_t size, g_hostent **result, int *herr)
{
    return g_gethostbyname2_r(name, G_AF_INET, ent, buf, size, result, herr);
}

static g_hostent *g_android_gethostbynamefornet(const char *name, int af, unsigned netid, unsigned mark)
{
    (void)netid;
    (void)mark;
    return g_gethostbyname2(name, af);
}

static int host_by_addr(const void *addr, u32 len, int af, g_hostent *ent, u8 *buf, size_t size, g_hostent **result, int *herr)
{
    mg_sockaddr a;
    char name[1025];
    *result = NULL;
    memset(&a, 0, sizeof(a));
    if (!addr || (af == G_AF_INET && len < 4) || (af == G_AF_INET6 && len < 16) || (af != G_AF_INET && af != G_AF_INET6)) {
        *herr = G_NO_RECOVERY;
        mg_set_errno(af != G_AF_INET && af != G_AF_INET6 ? GE_AFNOSUPPORT : GE_INVAL);
        return 0;
    }
    a.family = af == G_AF_INET6 ? MG_AF_INET6 : MG_AF_INET;
    memcpy(a.addr, addr, af == G_AF_INET6 ? 16 : 4);
    if (af == G_AF_INET && a.addr[0] == 127 && a.addr[3] == 1 && !a.addr[1] && !a.addr[2]) mg_strlcpy(name, "localhost", sizeof(name));
    else if (af == G_AF_INET6 && is_loopback_v6(a.addr)) mg_strlcpy(name, "ip6-localhost", sizeof(name));
    else if (mg_net_name_of(&a, name, sizeof(name)) != MG_EAI_OK) {
        *herr = G_HOST_NOT_FOUND;
        return 0;
    }
    if (!host_fill(ent, buf, size, name, af, &a, 1)) {
        *herr = G_NO_RECOVERY;
        return GE_RANGE;
    }
    *herr = 0;
    *result = ent;
    return 0;
}

static g_hostent *g_gethostbyaddr(const void *addr, u32 len, int af)
{
    host_store *s = store_of_thread();
    g_hostent *r = NULL;
    int herr = 0;
    host_by_addr(addr, len, af, &s->ent, (u8 *)(s + 1), HOST_ROOM, &r, &herr);
    t_h_errno = herr;
    return r;
}

static int g_gethostbyaddr_r(const void *addr, u32 len, int af, g_hostent *ent, char *buf, size_t size, g_hostent **result, int *herr)
{
    int dummy, r;
    if (!herr) herr = &dummy;
    r = host_by_addr(addr, len, af, ent, (u8 *)buf, size, result, herr);
    if (r) mg_set_errno(r);
    return r;
}

static const char *g_hstrerror(int code)
{
    switch (code) {
    case 0: return "No error";
    case G_HOST_NOT_FOUND: return "Unknown host";
    case G_TRY_AGAIN: return "Host name lookup failure";
    case G_NO_RECOVERY: return "Unknown server error";
    case G_NO_DATA: return "No address associated with name";
    default: return "Unknown resolver error";
    }
}

static void g_herror(const char *s)
{
    mg_warn("%s%s%s", s && *s ? s : "", s && *s ? ": " : "", g_hstrerror(t_h_errno));
}

/* in6addr_any and in6addr_loopback: the C library's two constant IPv6 addresses (::, ::1), which code takes the address of. */
static const u8 g_in6addr_any[16] = { 0 };
static const u8 g_in6addr_loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

const mg_sym mg_tab_netdb[] = {
    MG_DATA("in6addr_any", g_in6addr_any), MG_DATA("in6addr_loopback", g_in6addr_loopback),
    MG_WRAP("res_search", g_res_search), MG_WRAP("getaddrinfo", g_getaddrinfo), MG_WRAP("android_getaddrinfofornet", g_android_getaddrinfofornet), MG_WRAP("freeaddrinfo", g_freeaddrinfo),
    MG_WRAP("gai_strerror", g_gai_strerror), MG_WRAP("getnameinfo", g_getnameinfo),
    MG_WRAP("gethostbyname", g_gethostbyname), MG_WRAP("gethostbyname2", g_gethostbyname2), MG_WRAP("gethostbyname_r", g_gethostbyname_r),
    MG_WRAP("gethostbyname2_r", g_gethostbyname2_r), MG_WRAP("android_gethostbynamefornet", g_android_gethostbynamefornet),
    MG_WRAP("gethostbyaddr", g_gethostbyaddr), MG_WRAP("gethostbyaddr_r", g_gethostbyaddr_r), MG_WRAP("__get_h_errno", g_h_errno_ptr),
    MG_WRAP("hstrerror", g_hstrerror), MG_WRAP("herror", g_herror),
    MG_WRAP("getservbyname", g_getservbyname), MG_WRAP("getservbyport", g_getservbyport), MG_WRAP("getprotobyname", g_getprotobyname),
    MG_WRAP("getprotobynumber", g_getprotobynumber),
    MG_WRAP("htonl", g_htonl), MG_WRAP("ntohl", g_htonl), MG_WRAP("htons", g_htons), MG_WRAP("ntohs", g_htons),
    MG_WRAP("inet_addr", g_inet_addr), MG_WRAP("inet_aton", g_inet_aton), MG_WRAP("inet_pton", g_inet_pton), MG_WRAP("inet_ntop", g_inet_ntop),
    MG_WRAP("inet_ntoa", g_inet_ntoa),
    MG_WRAP("getifaddrs", g_getifaddrs), MG_WRAP("freeifaddrs", g_freeifaddrs), MG_WRAP("if_nametoindex", g_if_nametoindex),
    MG_WRAP("if_indextoname", g_if_indextoname), MG_WRAP("if_nameindex", g_if_nameindex), MG_WRAP("if_freenameindex", g_if_freenameindex),
    MG_END
};
