/*
 * rpc.h - ONC RPC (RFC 1057) call/reply framing over UDP, plus a
 * tiny dispatch table. Enough to serve portmap + mount + nfsv2.
 */
#ifndef GN_RPC_H
#define GN_RPC_H

#include "xdr.h"

/* RPC constants */
#define RPC_CALL   0
#define RPC_REPLY  1
#define RPC_VERS   2

#define MSG_ACCEPTED 0
#define MSG_DENIED   1

/* accept_stat */
#define RPC_SUCCESS       0
#define RPC_PROG_UNAVAIL  1
#define RPC_PROG_MISMATCH 2
#define RPC_PROC_UNAVAIL  3
#define RPC_GARBAGE_ARGS  4

/* auth flavor */
#define AUTH_NULL 0
#define AUTH_UNIX 1

/* a parsed incoming call header */
typedef struct {
    uint32_t xid;
    uint32_t prog;
    uint32_t vers;
    uint32_t proc;
    /* AUTH_UNIX credentials, if present (else uid/gid = -1) */
    uint32_t uid;
    uint32_t gid;
} rpc_call_t;

/* parse a call header from x (positioned at start of message).
   on return x is positioned at the procedure arguments.
   returns 0 ok, -1 malformed. */
int rpc_parse_call(xdr_t *x, rpc_call_t *c);

/* write an accepted reply header (SUCCESS) into x; caller then
   appends procedure results. */
int rpc_reply_success(xdr_t *x, uint32_t xid);
/* write an accepted reply with a non-SUCCESS accept_stat */
int rpc_reply_accept_err(xdr_t *x, uint32_t xid, uint32_t stat);
/* PROG_MISMATCH carries low/high supported versions */
int rpc_reply_prog_mismatch(xdr_t *x, uint32_t xid,
                            uint32_t lo, uint32_t hi);

/*
 * A service handler fills the reply body (after the RPC header,
 * which the dispatcher already wrote) using the reply xdr `out`,
 * reading args from `in`. Returns 0 ok, -1 to drop (no reply).
 */
typedef int (*rpc_handler_t)(rpc_call_t *c, xdr_t *in, xdr_t *out);

typedef struct {
    uint32_t prog;
    uint32_t vers;
    uint32_t proc;
    rpc_handler_t fn;
    int cache;          /* 1 = non-idempotent: keep in the dup cache
                           so a UDP retransmit replays the reply
                           instead of re-running the op */
} rpc_route_t;

/* enable per-request logging to stderr */
void rpc_set_verbose(int on);

/* run the UDP server loop over one or more already-bound sockets,
   dispatching to routes[0..n). blocks forever. */
void rpc_serve(const int *socks, int nsocks,
               const rpc_route_t *routes, int nroutes);

#endif /* GN_RPC_H */
