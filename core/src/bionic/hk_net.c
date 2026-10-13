/*
 * Sockets and name resolution.
 *
 * Not implemented yet: every call fails the way it would on a device with no
 * network, so that code which probes for connectivity carries on offline
 * instead of calling into nothing.
 */
#include "hk_bionic.h"

#include <stdio.h>

static int g_no_network(void)
{
    hk_note_once("network: sockets are not implemented; the app sees no network");
    hk_set_errno(GE_NETUNREACH);
    return -1;
}

/*
 * What is asked of a descriptor that would have to be a socket. None is: no socket can be made here. So the answer is
 * the one a file gets, quietly -- libcore asks it of every file it closes, to know whether it was a socket
 * (getsockopt SO_TYPE), and that is no sign of an app wanting the network.
 */
static int g_not_a_socket(void)
{
    hk_set_errno(GE_NOTSOCK);
    return -1;
}

/* getifaddrs gives no list, so there is none to free; and no interface has any index. */
static void g_free_nothing(void *list) { (void)list; }
static char *g_no_interface(unsigned index, char *name)
{
    (void)index; (void)name;
    hk_set_errno(GE_NXIO);
    return NULL;
}

/* EAI_NONAME */
static int g_getaddrinfo(const char *node, const char *service, const void *hints, void **res)
{
    (void)node; (void)service; (void)hints;
    if (res) *res = NULL;
    hk_note_once("network: name resolution is not implemented");
    return 8;
}
static void g_freeaddrinfo(void *res) { (void)res; }
static const char *g_gai_strerror(int code) { (void)code; return "Name or service not known"; }
static void *g_gethostbyname(const char *name) { (void)name; return NULL; }
/* An address as text: dotted for IPv4 (family 2), groups of four hex digits, none shortened, for IPv6 (family 10). */
static const char *g_inet_ntop(int family, const void *addr, char *out, u32 size)
{
    const u8 *a = (const u8 *)addr;
    char text[48];
    if (family == 2) snprintf(text, sizeof(text), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    else if (family == 10)
        snprintf(text, sizeof(text), "%x:%x:%x:%x:%x:%x:%x:%x", (a[0] << 8) | a[1], (a[2] << 8) | a[3], (a[4] << 8) | a[5], (a[6] << 8) | a[7],
                 (a[8] << 8) | a[9], (a[10] << 8) | a[11], (a[12] << 8) | a[13], (a[14] << 8) | a[15]);
    else {
        hk_set_errno(97);              /* EAFNOSUPPORT */
        return NULL;
    }
    if (strlen(text) + 1 > size) {
        hk_set_errno(28);              /* ENOSPC */
        return NULL;
    }
    strcpy(out, text);
    return out;
}

static int g_h_errno = 1;
static int *g_h_errno_ptr(void) { return &g_h_errno; }

static u32 g_htonl(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
static u16 g_htons(u16 v) { return (u16)((v >> 8) | (v << 8)); }

static u32 g_inet_addr(const char *s)
{
    u32 parts[4] = { 0, 0, 0, 0 };
    int n = 0;
    if (!s) return 0xFFFFFFFFu;
    for (; *s; s++) {
        if (*s == '.') {
            if (++n > 3) return 0xFFFFFFFFu;
        } else if (*s >= '0' && *s <= '9') {
            parts[n] = parts[n] * 10 + (u32)(*s - '0');
            if (parts[n] > 255) return 0xFFFFFFFFu;
        } else {
            return 0xFFFFFFFFu;
        }
    }
    if (n != 3) return 0xFFFFFFFFu;
    return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

static int g_inet_aton(const char *s, u32 *out)
{
    u32 v = g_inet_addr(s);
    if (v == 0xFFFFFFFFu && (!s || strcmp(s, "255.255.255.255"))) return 0;
    if (out) *out = v;
    return 1;
}

static int g_inet_pton(int family, const char *s, void *out)
{
    if (family != 2) {                             /* AF_INET */
        hk_set_errno(GE_AFNOSUPPORT);
        return -1;
    }
    return g_inet_aton(s, (u32 *)out);
}

const hk_sym hk_tab_net[] = {
    HK_WRAP("socket", g_no_network), HK_WRAP("socketpair", g_no_network), HK_WRAP("connect", g_no_network),
    HK_WRAP("bind", g_no_network), HK_WRAP("listen", g_no_network), HK_WRAP("accept", g_no_network),
    HK_WRAP("accept4", g_no_network), HK_WRAP("send", g_no_network), HK_WRAP("sendto", g_no_network),
    HK_WRAP("sendmsg", g_no_network), HK_WRAP("recv", g_no_network), HK_WRAP("recvfrom", g_no_network),
    HK_WRAP("recvmsg", g_no_network), HK_WRAP("shutdown", g_no_network), HK_WRAP("setsockopt", g_not_a_socket),
    HK_WRAP("getsockopt", g_not_a_socket), HK_WRAP("getsockname", g_not_a_socket), HK_WRAP("getpeername", g_not_a_socket),
    HK_WRAP("getifaddrs", g_no_network), HK_WRAP("if_nametoindex", g_no_network),
    HK_WRAP("freeifaddrs", g_free_nothing), HK_WRAP("if_indextoname", g_no_interface),
    HK_WRAP("getaddrinfo", g_getaddrinfo), HK_WRAP("android_getaddrinfofornet", g_getaddrinfo), HK_WRAP("freeaddrinfo", g_freeaddrinfo),
    HK_WRAP("gai_strerror", g_gai_strerror), HK_WRAP("gethostbyname", g_gethostbyname), HK_WRAP("gethostbyaddr", g_gethostbyname),
    HK_WRAP("getnameinfo", g_getaddrinfo), HK_WRAP("__get_h_errno", g_h_errno_ptr),
    HK_WRAP("htonl", g_htonl), HK_WRAP("ntohl", g_htonl), HK_WRAP("htons", g_htons), HK_WRAP("ntohs", g_htons),
    HK_WRAP("inet_addr", g_inet_addr), HK_WRAP("inet_aton", g_inet_aton), HK_WRAP("inet_pton", g_inet_pton), HK_WRAP("inet_ntop", g_inet_ntop),
    HK_END
};
