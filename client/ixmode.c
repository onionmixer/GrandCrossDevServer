/*
 * ixmode.c - client side of interactive execution (RUNI).
 * Local stdin -> I frames, Ctrl-C -> K frame (remote kill),
 * Ctrl-D (stdin EOF) -> I 0. Second Ctrl-C aborts the client.
 * Linux-only client, so select() on {stdin, channel} is fine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>

#include "gcdsp.h"
#include "util.h"
#include "lineio.h"
#include "remote.h"

static volatile sig_atomic_t g_int;

static void on_int(int sig)
{
    (void)sig;
    g_int++;
}

static int chfd(gcds_chan_t *c)
{
    if (c->kind == GCDS_CHAN_TCP)
        return (int)c->s;
    return c->ser;
}

/* declared in remote.c */
int rc_out_frame(gcds_rc_t *rc, const char *line, long *code);

long ix_client(gcds_rc_t *rc, const char *cmd)
{
    static char line[GCDSP_LINE_MAX];
    static char buf[GCDSP_FRAME_MAX];
    struct sigaction sa;
    struct sigaction oldsa;
    int sfd;
    int stdin_open;
    int sent_k;
    long code;
    long n;
    int r;

    line[0] = '\0';
    gcds_strlcat(line, "RUNI ", GCDSP_LINE_MAX);
    gcds_strlcat(line, cmd, GCDSP_LINE_MAX);
    if (lio_put_line(&rc->ch, line) != LIO_OK)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_int;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;            /* no SA_RESTART: break select */
    sigaction(SIGINT, &sa, &oldsa);
    g_int = 0;

    sfd = chfd(&rc->ch);
    stdin_open = 1;
    sent_k = 0;
    code = -1;

    for (;;) {
        fd_set rd;
        int maxfd;

        if (g_int > 0) {
            if (sent_k) {       /* second Ctrl-C: give up */
                code = -1;
                break;
            }
            if (lio_put_line(&rc->ch, "K") != LIO_OK) {
                code = -1;
                break;
            }
            sent_k = 1;
            g_int = 0;
            fprintf(stderr,
                    "gcds: interrupt sent (Ctrl-C again to abort)\n");
        }

        FD_ZERO(&rd);
        FD_SET(sfd, &rd);
        maxfd = sfd;
        if (stdin_open) {
            FD_SET(0, &rd);
            if (maxfd < 0)
                maxfd = 0;
        }
        r = select(maxfd + 1, &rd, NULL, NULL, NULL);
        if (r < 0)
            continue;           /* EINTR: loop handles g_int */

        if (stdin_open && FD_ISSET(0, &rd)) {
            n = (long)read(0, buf, 1024);
            if (n <= 0) {
                if (lio_put_line(&rc->ch, "I 0") != LIO_OK) {
                    code = -1;
                    break;
                }
                stdin_open = 0;
            } else {
                char hdr[24];

                sprintf(hdr, "I %ld\n", n);
                if (chan_write_all(&rc->ch, hdr,
                                   (long)strlen(hdr)) < 0 ||
                    chan_write_all(&rc->ch, buf, n) < 0) {
                    code = -1;
                    break;
                }
            }
        }

        if (FD_ISSET(sfd, &rd)) {
            if (lio_get_line(&rc->ch, line, GCDSP_LINE_MAX)
                != LIO_OK) {
                code = -1;
                break;
            }
            r = rc_out_frame(rc, line, &code);
            if (r != 0)
                break;          /* X received (code set) or error */
        }
    }

    sigaction(SIGINT, &oldsa, NULL);
    return code;
}
