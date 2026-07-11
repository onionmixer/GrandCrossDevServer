/*
 * net_null.c - TCP stub for network-less builds (DOS serial-only,
 * PLAN_02 section 4: no Watt-32 linked, smaller resident size).
 * gcdsd main falls into serial mode via the conf; a TCP attempt
 * fails cleanly at net_listen.
 */
#ifdef GCDS_NO_NET

#include "net.h"

int net_init(void)
{
    return 0;
}

void net_cleanup(void)
{
}

gcds_sock_t net_listen(unsigned short port)
{
    (void)port;
    return GCDS_BADSOCK;
}

gcds_sock_t net_accept(gcds_sock_t ls, char *peer_ip)
{
    (void)ls;
    (void)peer_ip;
    return GCDS_BADSOCK;
}

gcds_sock_t net_connect(const char *host, unsigned short port)
{
    (void)host;
    (void)port;
    return GCDS_BADSOCK;
}

long net_send(gcds_sock_t s, const char *buf, long n)
{
    (void)s;
    (void)buf;
    (void)n;
    return -1;
}

long net_recv(gcds_sock_t s, char *buf, long n)
{
    (void)s;
    (void)buf;
    (void)n;
    return -1;
}

void net_close(gcds_sock_t s)
{
    (void)s;
}

#else

typedef int gcds_net_null_unused;

#endif /* GCDS_NO_NET */
