/*
 * net.c - TCP backend. All platform #ifdef mess stays inside this
 * file; everyone else sees net.h only.
 *
 * Win32 path: Winsock 1.1 on purpose (winsock.h + wsock32.lib) so
 * the same source serves NT4 through current Windows (PLAN_02).
 * Watt-32 (DOS) is expected to compile the POSIX-ish path below
 * through its BSD compatibility headers (Phase 3 verifies).
 */
#include <string.h>

#include "util.h"
#include "net.h"

#ifdef GCDS_WIN32

#include <winsock.h>

int net_init(void)
{
    WSADATA wd;

    if (WSAStartup(MAKEWORD(1, 1), &wd) != 0)
        return -1;
    return 0;
}

void net_cleanup(void)
{
    WSACleanup();
}

gcds_sock_t net_listen(unsigned short port)
{
    SOCKET s;
    struct sockaddr_in sa;
    int one;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return GCDS_BADSOCK;

    one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        closesocket(s);
        return GCDS_BADSOCK;
    }
    if (listen(s, 4) == SOCKET_ERROR) {
        closesocket(s);
        return GCDS_BADSOCK;
    }
    return (gcds_sock_t)s;
}

gcds_sock_t net_accept(gcds_sock_t ls, char *peer_ip)
{
    SOCKET s;
    struct sockaddr_in sa;
    int salen;

    salen = (int)sizeof(sa);
    s = accept((SOCKET)ls, (struct sockaddr *)&sa, &salen);
    if (s == INVALID_SOCKET)
        return GCDS_BADSOCK;
    if (peer_ip != NULL)
        gcds_strlcpy(peer_ip, inet_ntoa(sa.sin_addr), 16);
    return (gcds_sock_t)s;
}

gcds_sock_t net_connect(const char *host, unsigned short port)
{
    SOCKET s;
    struct sockaddr_in sa;
    unsigned long addr;
    struct hostent *he;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    addr = inet_addr(host);
    if (addr != INADDR_NONE) {
        sa.sin_addr.s_addr = addr;
    } else {
        he = gethostbyname(host);
        if (he == NULL || he->h_addrtype != AF_INET ||
            he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
            return GCDS_BADSOCK;
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return GCDS_BADSOCK;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa))
        == SOCKET_ERROR) {
        closesocket(s);
        return GCDS_BADSOCK;
    }
    return (gcds_sock_t)s;
}

long net_send(gcds_sock_t s, const char *buf, long n)
{
    return (long)send((SOCKET)s, buf, (int)n, 0);
}

long net_recv(gcds_sock_t s, char *buf, long n)
{
    return (long)recv((SOCKET)s, buf, (int)n, 0);
}

void net_close(gcds_sock_t s)
{
    closesocket((SOCKET)s);
}

#else /* POSIX / BeOS / NeXTSTEP / Watt-32 */

#ifdef GCDS_DOS
/* MS-DOS TCP via Watt-32. tcp.h gives htons/htonl (sys/swap.h)
   and sock_init; the BSD compat headers give socket/bind/...
   (remapped to _w32_*), hostent, gethostbyname, inet_addr.
   No <unistd.h>: sockets close via closesocket (PLAN_02 4). */
#include <tcp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "gnext.h"          /* NeXTSTEP: socklen_t etc (GCDS_NEXT) */
#endif

int net_init(void)
{
#ifdef GCDS_DOS
    /* bring up the packet-driver stack (reads WATTCP.CFG). returns
       0 on success; nonzero is fatal for a TCP-mode daemon */
    if (sock_init() != 0)
        return -1;
    return 0;
#else
    return 0;
#endif
}

void net_cleanup(void)
{
#ifdef GCDS_DOS
    sock_exit();
#endif
}

gcds_sock_t net_listen(unsigned short port)
{
    gcds_sock_t s;
    struct sockaddr_in sa;
    int one;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == GCDS_BADSOCK)
        return GCDS_BADSOCK;

    one = 1;
    /* failure is non-fatal by design (PLAN_02 section 2) */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        net_close(s);
        return GCDS_BADSOCK;
    }
    if (listen(s, 4) < 0) {
        net_close(s);
        return GCDS_BADSOCK;
    }
    return s;
}

gcds_sock_t net_accept(gcds_sock_t ls, char *peer_ip)
{
    gcds_sock_t s;
    struct sockaddr_in sa;
    socklen_t salen;

    salen = (socklen_t)sizeof(sa);
    s = accept(ls, (struct sockaddr *)&sa, &salen);
    if (s == GCDS_BADSOCK)
        return GCDS_BADSOCK;
    if (peer_ip != NULL)
        gcds_strlcpy(peer_ip, inet_ntoa(sa.sin_addr), 16);
    return s;
}

gcds_sock_t net_connect(const char *host, unsigned short port)
{
    gcds_sock_t s;
    struct sockaddr_in sa;
    unsigned long addr;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    addr = inet_addr(host);
    if (addr != (unsigned long)-1) {
        sa.sin_addr.s_addr = addr;
    } else {
#ifdef GCDS_DOS
        /* the DOS daemon only listens; drop gethostbyname so the
           Watt-32 BIND resolver (large DGROUP cost) isn't linked.
           dotted-quad addresses still work (PLAN_02 4). */
        return GCDS_BADSOCK;
#else
        struct hostent *he;

        he = gethostbyname(host);
        if (he == NULL || he->h_addrtype != AF_INET ||
            he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
            return GCDS_BADSOCK;
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
#endif
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == GCDS_BADSOCK)
        return GCDS_BADSOCK;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        net_close(s);
        return GCDS_BADSOCK;
    }
    return s;
}

long net_send(gcds_sock_t s, const char *buf, long n)
{
    return (long)send(s, buf, (size_t)n, 0);
}

long net_recv(gcds_sock_t s, char *buf, long n)
{
    return (long)recv(s, buf, (size_t)n, 0);
}

void net_close(gcds_sock_t s)
{
#ifdef GCDS_DOS
    closesocket(s);     /* Watt-32 socket close (not file close) */
#else
    close(s);
#endif
}

#endif /* GCDS_WIN32 */
