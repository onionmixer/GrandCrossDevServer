/*
 * gnfsd - a simple, self-contained NFSv2 (RFC 1094) server.
 *
 * Shares one directory of the local (Linux) machine so retro
 * clients (NeXTSTEP/OPENSTEP, BeOS, SunOS-era, ...) can mount it
 * and do file I/O. Implements portmap GETPORT + MOUNT v1 + NFS v2
 * over UDP.
 *
 *   gnfsd [-p pmap_port] [-n nfs_port] [-u user] [-v] <export-dir>
 *
 * Two UDP sockets are served, both dispatching all three programs:
 *   - pmap_port (default 111): where clients look for the
 *     portmapper. Privileged -> run as root for real clients.
 *   - nfs_port  (default 2049): mount + nfs. GETPORT reports this,
 *     AND many old clients (NeXTSTEP) send NFS straight to 2049
 *     regardless of GETPORT - so both must be served there.
 *
 * Privilege drop: the privileged ports need root to bind, but
 * nothing after that does. Once bound we permanently drop to a
 * normal uid/gid (default: the export directory's owner; override
 * with -u). So every file the server touches is done as that one
 * user - all client writes land owned by that user (no root-owned
 * mess), and the server can't reach anything outside that user's
 * reach. This is the "squash to one uid" model; no per-client uid
 * mapping (no fine-grained permission control, by design).
 *
 * For development without root, use two high ports:
 *   gnfsd -p 11111 -n 12049 /tmp/share
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "handle.h"
#include "nfs.h"

/* resolve a user spec (name or numeric uid) to uid+primary gid.
   0 ok, -1 unknown. */
static int resolve_user(const char *spec, uid_t *uid, gid_t *gid)
{
    struct passwd *pw;
    char *end;
    long n;

    pw = getpwnam(spec);
    if (pw != NULL) {
        *uid = pw->pw_uid;
        *gid = pw->pw_gid;
        return 0;
    }
    n = strtol(spec, &end, 10);
    if (*end == '\0' && n >= 0) {
        pw = getpwuid((uid_t)n);
        *uid = (uid_t)n;
        *gid = (pw != NULL) ? pw->pw_gid : (gid_t)n;
        return 0;
    }
    return -1;
}

/* drop to (uid,gid) permanently. must be root. 0 ok, -1 fail. */
static int drop_to(uid_t uid, gid_t gid)
{
    if (setgroups(0, NULL) < 0)
        return -1;
    if (setgid(gid) < 0)
        return -1;
    if (setuid(uid) < 0)
        return -1;
    /* verify we cannot regain root */
    if (setuid(0) == 0)
        return -1;
    return 0;
}

static int bind_udp(int port)
{
    int s;
    struct sockaddr_in sa;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    /* Ask for a bigger receive queue. This server is iterative, so a
       burst (a build or tar copy issuing many requests at once) can
       arrive faster than it is drained; without room the kernel drops
       datagrams silently and the client has to time out and retransmit.
       The kernel clamps this to net.core.rmem_max, so on a stock Linux
       the effect is limited - raise that sysctl if bursts still drop
       (nfsd/README.md). Failure here is not fatal. */
    {
        int rcv = 4 * 1024 * 1024;
        (void)setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

int main(int argc, char **argv)
{
    int pmap_port = 111;
    int nfs_port = 2049;
    int verbose = 0;
    const char *dir = NULL;
    const char *userspec = NULL;
    char abs[PATH_MAX];
    struct stat est;
    uid_t run_uid;
    gid_t run_gid;
    int socks[2];
    int nsocks = 0;
    int i;
    static const char USAGE[] = "usage: gnfsd [-p pmap_port] "
        "[-n nfs_port] [-u user] [-v] <export-dir>\n";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            pmap_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            nfs_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
            userspec = argv[++i];
        else if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        else if (argv[i][0] != '-')
            dir = argv[i];
        else {
            fputs(USAGE, stderr);
            return 2;
        }
    }
    if (dir == NULL) {
        fputs(USAGE, stderr);
        return 2;
    }
    if (realpath(dir, abs) == NULL || stat(abs, &est) < 0) {
        fprintf(stderr, "gnfsd: %s: no such directory\n", dir);
        return 2;
    }

    /* pick the uid/gid to run file ops as: -u, else export owner */
    if (userspec != NULL) {
        if (resolve_user(userspec, &run_uid, &run_gid) < 0) {
            fprintf(stderr, "gnfsd: unknown user '%s'\n", userspec);
            return 2;
        }
    } else {
        run_uid = est.st_uid;
        run_gid = est.st_gid;
    }

    signal(SIGPIPE, SIG_IGN);

    socks[nsocks] = bind_udp(nfs_port);
    if (socks[nsocks] < 0) {
        fprintf(stderr, "gnfsd: bind nfs port %d: ", nfs_port);
        perror("");
        return 1;
    }
    nsocks++;

    if (pmap_port != nfs_port) {
        socks[nsocks] = bind_udp(pmap_port);
        if (socks[nsocks] < 0) {
            fprintf(stderr, "gnfsd: bind portmap port %d: ",
                    pmap_port);
            perror("");
            if (pmap_port < 1024)
                fprintf(stderr, "  (port %d is privileged - run as "
                        "root, or use -p <highport>)\n", pmap_port);
            return 1;
        }
        nsocks++;
    }

    /* ports are bound; root is no longer needed. drop to run_uid so
       all file operations (and thus client-created files) are done
       as that one user. */
    if (geteuid() == 0) {
        if (run_uid == 0) {
            fprintf(stderr, "gnfsd: warning: export owned by root and "
                    "no -u given; running as root (client writes will "
                    "be root-owned)\n");
        } else if (drop_to(run_uid, run_gid) < 0) {
            perror("gnfsd: drop privileges");
            return 1;
        }
    }

    rpc_set_verbose(verbose);
    fh_set_root(abs);
    svc_config(abs, nfs_port);
    {
        struct passwd *pw = getpwuid(geteuid());
        fprintf(stderr, "gnfsd: serving %s\n", abs);
        fprintf(stderr, "gnfsd: NFSv2/UDP  portmap=%d  mount+nfs=%d  "
                "as uid=%d(%s) gid=%d\n", pmap_port, nfs_port,
                (int)geteuid(), pw ? pw->pw_name : "?", (int)getegid());
    }

    rpc_serve(socks, nsocks, GN_ROUTES, GN_NROUTES);
    return 0;
}
