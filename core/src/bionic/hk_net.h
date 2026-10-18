/*
 * What the network's two files share: hk_net.c (sockets) and hk_netdb.c
 * (names, addresses, interfaces). Android's numbers and structures here are
 * bionic's for 32-bit ARM, which are Linux's.
 */
#ifndef HK_NET_H
#define HK_NET_H

#include "hk_bionic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define G_AF_UNSPEC 0
#define G_AF_UNIX 1
#define G_AF_INET 2
#define G_AF_INET6 10
#define G_AF_NETLINK 16
#define G_AF_PACKET 17

#define G_SOCK_STREAM 1
#define G_SOCK_DGRAM 2
#define G_SOCK_RAW 3
#define G_SOCK_SEQPACKET 5

/* sockaddr_in is 16 bytes, sockaddr_in6 28, sockaddr_un 110, sockaddr_storage 128. */
#define G_SIN_LEN 16u
#define G_SIN6_LEN 28u

/*
 * Android's sockaddr from the host's, into `out` (room for 28 bytes): its
 * length. `v6`: as a socket of AF_INET6 has its addresses, an IPv4 one written
 * ::ffff:a.b.c.d. A host address with no family is the wildcard of `v6`'s.
 */
u32 hk_net_sockaddr_out(const hk_sockaddr *a, bool v6, u8 *out);
/* The host's sockaddr from Android's: 0, or the errno for one that is not an internet address (EINVAL, EAFNOSUPPORT). AF_UNSPEC: family 0. */
int hk_net_sockaddr_in(const void *sa, u32 len, hk_sockaddr *a);
/* The flag trace-net: whether it is on, and a line of it (hk_net.c). */
bool hk_net_trace_wanted(void);
void hk_net_trace(const char *fmt, ...) HK_PRINTF(1, 2);
/* Into a buffer the app gave with its length: as much as fits; *len then says how long the whole was. */
void hk_net_give(const u8 *what, u32 n, void *out, u32 *len);

/* The host's interfaces under the names Android gives them: lo, wlan0, eth0, rmnet0. */
typedef struct hk_gif {
    char name[16];
    hk_netif host;
} hk_gif;
/*
 * A malloc'd array, the loopback first, and how many. Interfaces of the host's that Android would not have (tunnels of its own, ones
 * that are down) are left out. When the host will not list them, the loopback alone (and the log says so once).
 */
int hk_net_interfaces_android(hk_gif **out);
/* Android's IFF_* flags of an interface. */
u32 hk_net_if_flags(const hk_gif *f);
/* The ioctls that ask about interfaces (SIOCGIFCONF, SIOCGIFFLAGS and the rest): 0, -1 with errno set, or -2 for a request that is not one of them. */
int hk_net_if_ioctl(unsigned request, void *arg);

#ifdef __cplusplus
}
#endif

#endif
