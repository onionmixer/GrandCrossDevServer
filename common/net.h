/*
 * net.h - TCP backend. Only the conservative BSD/Winsock/Watt-32
 * intersection is used (PLAN_02 section 2). IPv4 only, blocking I/O.
 */
#ifndef NET_H
#define NET_H

#ifdef GCDS_WIN32
typedef unsigned int gcds_sock_t;    /* SOCKET; cast in net.c */
#define GCDS_BADSOCK ((gcds_sock_t)~0)
#else
typedef int gcds_sock_t;
#define GCDS_BADSOCK (-1)
#endif

int  net_init(void);                 /* Win32: WSAStartup(1.1); else no-op */
void net_cleanup(void);

gcds_sock_t net_listen(unsigned short port);
/* peer_ip: 16-byte buffer for dotted quad, or NULL */
gcds_sock_t net_accept(gcds_sock_t ls, char *peer_ip);
gcds_sock_t net_connect(const char *host, unsigned short port);

/* single send/recv; may transfer fewer bytes; <0 on error */
long net_send(gcds_sock_t s, const char *buf, long n);
long net_recv(gcds_sock_t s, char *buf, long n);
void net_close(gcds_sock_t s);

#endif /* NET_H */
