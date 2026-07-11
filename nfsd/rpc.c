#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rpc.h"

static int g_verbose = 0;
void rpc_set_verbose(int on) { g_verbose = on; }

/* ---- duplicate request cache (DRC) ------------------------------
 * UDP has no delivery guarantee, so clients retransmit. Re-running
 * a non-idempotent op (CREATE/REMOVE/RENAME/...) on a retransmit
 * corrupts results (e.g. REMOVE succeeds, the retransmit returns
 * NOENT, the client sees a spurious error). We remember the reply
 * for recently-seen non-idempotent requests, keyed by the client
 * endpoint + xid, and replay it on a retransmit.
 */
#define DRC_SLOTS    256
#define DRC_REPLYMAX 512        /* non-idempotent replies are small */
#define DRC_TTL      30         /* seconds a cached reply is valid  */

typedef struct {
    int      valid;
    uint32_t addr;              /* client sin_addr.s_addr (net order) */
    uint16_t port;              /* client sin_port (net order)        */
    uint32_t xid;
    time_t   when;
    int      len;
    unsigned char reply[DRC_REPLYMAX];
} drc_ent_t;

static drc_ent_t g_drc[DRC_SLOTS];
static int g_drc_next;          /* ring write cursor */

/* find a live cached reply for this request, or NULL */
static drc_ent_t *drc_lookup(struct sockaddr_in *peer, uint32_t xid,
                             time_t now)
{
    int i;
    for (i = 0; i < DRC_SLOTS; i++) {
        drc_ent_t *e = &g_drc[i];
        if (e->valid && e->xid == xid &&
            e->addr == peer->sin_addr.s_addr &&
            e->port == peer->sin_port &&
            now - e->when <= DRC_TTL)
            return e;
    }
    return NULL;
}

/* store a reply (truncated if oversize -> then not replayable, so
   only store when it fits) */
static void drc_store(struct sockaddr_in *peer, uint32_t xid,
                      const unsigned char *reply, int len, time_t now)
{
    drc_ent_t *e;
    if (len > DRC_REPLYMAX)
        return;                 /* don't cache; op stays re-runnable */
    e = &g_drc[g_drc_next];
    g_drc_next = (g_drc_next + 1) % DRC_SLOTS;
    e->valid = 1;
    e->addr = peer->sin_addr.s_addr;
    e->port = peer->sin_port;
    e->xid = xid;
    e->when = now;
    e->len = len;
    memcpy(e->reply, reply, (size_t)len);
}

int rpc_parse_call(xdr_t *x, rpc_call_t *c)
{
    uint32_t mtype, rpcvers;
    uint32_t cred_flavor, cred_len;
    uint32_t verf_flavor, verf_len;
    const unsigned char *p;
    uint32_t got;

    c->uid = (uint32_t)-1;
    c->gid = (uint32_t)-1;

    if (xdr_get_u32(x, &c->xid) < 0)
        return -1;
    if (xdr_get_u32(x, &mtype) < 0 || mtype != RPC_CALL)
        return -1;
    if (xdr_get_u32(x, &rpcvers) < 0 || rpcvers != RPC_VERS)
        return -1;
    if (xdr_get_u32(x, &c->prog) < 0)
        return -1;
    if (xdr_get_u32(x, &c->vers) < 0)
        return -1;
    if (xdr_get_u32(x, &c->proc) < 0)
        return -1;

    /* credentials */
    if (xdr_get_u32(x, &cred_flavor) < 0)
        return -1;
    if (xdr_get_bytes(x, &p, &cred_len) < 0)
        return -1;
    if (cred_flavor == AUTH_UNIX && cred_len >= 12) {
        /* stamp(4), machinename<>, uid(4), gid(4), gids<> */
        xdr_t cx;
        uint32_t stamp, mlen;
        const unsigned char *mp;

        xdr_init(&cx, (void *)p, cred_len);
        if (xdr_get_u32(&cx, &stamp) == 0 &&
            xdr_get_bytes(&cx, &mp, &mlen) == 0 &&
            xdr_get_u32(&cx, &c->uid) == 0 &&
            xdr_get_u32(&cx, &c->gid) == 0) {
            /* ok */
        }
    }
    /* verifier */
    if (xdr_get_u32(x, &verf_flavor) < 0)
        return -1;
    if (xdr_get_bytes(x, &p, &verf_len) < 0)
        return -1;
    (void)got;
    return 0;
}

/* accepted reply header up to and including accept_stat */
static int reply_head(xdr_t *x, uint32_t xid, uint32_t reply_stat,
                      uint32_t accept_stat)
{
    if (xdr_put_u32(x, xid) < 0)
        return -1;
    if (xdr_put_u32(x, RPC_REPLY) < 0)
        return -1;
    if (xdr_put_u32(x, reply_stat) < 0)      /* MSG_ACCEPTED */
        return -1;
    /* verifier: AUTH_NULL, len 0 */
    if (xdr_put_u32(x, AUTH_NULL) < 0)
        return -1;
    if (xdr_put_u32(x, 0) < 0)
        return -1;
    if (xdr_put_u32(x, accept_stat) < 0)
        return -1;
    return 0;
}

int rpc_reply_success(xdr_t *x, uint32_t xid)
{
    return reply_head(x, xid, MSG_ACCEPTED, RPC_SUCCESS);
}

int rpc_reply_accept_err(xdr_t *x, uint32_t xid, uint32_t stat)
{
    return reply_head(x, xid, MSG_ACCEPTED, stat);
}

int rpc_reply_prog_mismatch(xdr_t *x, uint32_t xid,
                            uint32_t lo, uint32_t hi)
{
    if (reply_head(x, xid, MSG_ACCEPTED, RPC_PROG_MISMATCH) < 0)
        return -1;
    if (xdr_put_u32(x, lo) < 0)
        return -1;
    if (xdr_put_u32(x, hi) < 0)
        return -1;
    return 0;
}

#define RPC_BUFSZ 65536

/* handle one datagram already read into inbuf[0..n) from `peer`,
   writing the reply (if any) back on `sock`. */
static void handle_one(int sock, unsigned char *inbuf, ssize_t n,
                       struct sockaddr_in *peer, socklen_t plen,
                       const rpc_route_t *routes, int nroutes)
{
    static unsigned char outbuf[RPC_BUFSZ];
    xdr_t in, out;
    rpc_call_t c;
    int i;
    const rpc_route_t *match;
    int prog_ok, vers_lo, vers_hi;

    xdr_init(&in, inbuf, (size_t)n);
    xdr_init(&out, outbuf, sizeof(outbuf));

    if (rpc_parse_call(&in, &c) < 0)
        return;                 /* malformed: drop */

    if (g_verbose)
        fprintf(stderr, "gnfsd: %s prog=%u vers=%u proc=%u\n",
                inet_ntoa(peer->sin_addr), c.prog, c.vers, c.proc);

    /* find handler; track prog/vers availability for errors */
    match = NULL;
    prog_ok = 0;
    vers_lo = 0x7fffffff;
    vers_hi = 0;
    for (i = 0; i < nroutes; i++) {
        if (routes[i].prog == c.prog) {
            prog_ok = 1;
            if ((int)routes[i].vers < vers_lo)
                vers_lo = (int)routes[i].vers;
            if ((int)routes[i].vers > vers_hi)
                vers_hi = (int)routes[i].vers;
            if (routes[i].vers == c.vers && routes[i].proc == c.proc)
                match = &routes[i];
        }
    }

    /* non-idempotent op: replay a cached reply if this is a
       retransmit we've already handled */
    if (match != NULL && match->cache) {
        time_t now = time(NULL);
        drc_ent_t *hit = drc_lookup(peer, c.xid, now);
        if (hit != NULL) {
            if (g_verbose)
                fprintf(stderr, "gnfsd:   (DRC replay xid=%u)\n", c.xid);
            sendto(sock, hit->reply, (size_t)hit->len, 0,
                   (struct sockaddr *)peer, plen);
            return;
        }
    }

    if (match != NULL) {
        if (rpc_reply_success(&out, c.xid) < 0)
            return;
        if (match->fn(&c, &in, &out) < 0)
            return;             /* handler asked to drop */
    } else if (!prog_ok) {
        rpc_reply_accept_err(&out, c.xid, RPC_PROG_UNAVAIL);
    } else {
        int vers_here = 0;
        for (i = 0; i < nroutes; i++)
            if (routes[i].prog == c.prog && routes[i].vers == c.vers)
                vers_here = 1;
        if (!vers_here)
            rpc_reply_prog_mismatch(&out, c.xid, (uint32_t)vers_lo,
                                    (uint32_t)vers_hi);
        else
            rpc_reply_accept_err(&out, c.xid, RPC_PROC_UNAVAIL);
    }

    if (out.err)
        return;
    /* remember non-idempotent replies for retransmit replay */
    if (match != NULL && match->cache)
        drc_store(peer, c.xid, outbuf, (int)xdr_len(&out),
                  time(NULL));
    sendto(sock, outbuf, xdr_len(&out), 0,
           (struct sockaddr *)peer, plen);
}

void rpc_serve(const int *socks, int nsocks,
               const rpc_route_t *routes, int nroutes)
{
    static unsigned char inbuf[RPC_BUFSZ];
    int maxfd, i;

    maxfd = 0;
    for (i = 0; i < nsocks; i++)
        if (socks[i] > maxfd)
            maxfd = socks[i];

    for (;;) {
        fd_set rd;
        int r;

        FD_ZERO(&rd);
        for (i = 0; i < nsocks; i++)
            FD_SET(socks[i], &rd);
        r = select(maxfd + 1, &rd, NULL, NULL, NULL);
        if (r <= 0)
            continue;
        for (i = 0; i < nsocks; i++) {
            struct sockaddr_in peer;
            socklen_t plen;
            ssize_t n;

            if (!FD_ISSET(socks[i], &rd))
                continue;
            plen = sizeof(peer);
            n = recvfrom(socks[i], inbuf, sizeof(inbuf), 0,
                         (struct sockaddr *)&peer, &plen);
            if (n < 0)
                continue;
            handle_one(socks[i], inbuf, n, &peer, plen,
                       routes, nroutes);
        }
    }
}
