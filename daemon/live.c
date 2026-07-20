/*
 * live.c - POSIX supervised execution (fork + pipes + select).
 * The only daemon code allowed to call select() (PLAN_02 2).
 * Win32 counterpart: live_w32.c (polling hybrid).
 */
#if (defined(GCDS_HAS_LIVE) || defined(GCDS_HAS_IX)) && \
    !defined(GCDS_WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#ifndef GCDS_NEXT
#include <sys/select.h>     /* 4.3BSD: absent; gnext.h pulls
                               sys/time.h for select/FD_SET */
#endif
#include <sys/stat.h>
#include <sys/wait.h>

#include "gnext.h"          /* no-op unless -DGCDS_NEXT */
#include "gcdsp.h"
#include "util.h"
#include "lineio.h"
#include "session.h"
#include "textcv.h"
#include "live.h"

#define SCRIPT_MAX (32768L)
#define KILL_GRACE 2        /* seconds between SIGTERM and SIGKILL */

static char g_script[SCRIPT_MAX];

static int chan_fd(gcds_chan_t *c)
{
    if (c->kind == GCDS_CHAN_TCP)
        return (int)c->s;
    return c->ser;
}

/* Signal the child's whole process group, not just the child.
   The command runs as `sh -c "( ... )"`, so grandchildren
   (e.g. sleep, a compiler's subprocess) would otherwise survive
   a kill of sh alone and keep the output pipes open, hanging the
   supervisor on EOF. The child is made a group leader below. */
static void kill_child(pid_t pid, int *killed, time_t *tkill)
{
    if (*killed == 0) {
        if (kill(-pid, SIGTERM) < 0)
            kill(pid, SIGTERM);
        *killed = 1;
        *tkill = time(NULL);
    }
}

static void hardkill_child(pid_t pid)
{
    if (kill(-pid, SIGKILL) < 0)
        kill(pid, SIGKILL);
}

/* drain one child pipe into a tagged frame; returns 0 open,
   1 EOF, -1 session write error. adds bytes read to *total. */
#ifdef GCDS_TEXTCV
/* per-stream hold-back: a multi-byte char split across two reads must
   not be converted in halves (textcv.h) */
static txt_stream_t g_cv_out, g_cv_err;
static char g_cvbuf_p[GCDSP_FRAME_MAX * 2];

static void cv_reset(void)
{
    txt_stream_init(&g_cv_out);
    txt_stream_init(&g_cv_err);
}

/* emit whatever each stream still holds; 0 ok, -1 session write error */
static int cv_flush_all(gcds_chan_t *sess)
{
    int i;
    if (sess == NULL || !txt_active())
        return 0;
    for (i = 0; i < 2; i++) {
        txt_stream_t *cv = i ? &g_cv_err : &g_cv_out;
        char tag = i ? 'E' : 'O';
        long cn = txt_flush(cv, g_cvbuf_p, (long)sizeof(g_cvbuf_p));
        if (cn > 0 && lio_put_frame(sess, tag, g_cvbuf_p, cn) != LIO_OK)
            return -1;
    }
    return 0;
}
#endif

static int pump(gcds_chan_t *sess, char tag, int fd, long chunk,
                long *total)
{
    static char buf[GCDSP_FRAME_MAX];
    long n;

    n = (long)read(fd, buf, (size_t)chunk);
    if (n <= 0)
        return 1;
    *total += n;
    if (sess != NULL) {
#ifdef GCDS_TEXTCV
        if (txt_active()) {           /* local encoding -> wire (UTF-8) */
            txt_stream_t *cv = (tag == 'O') ? &g_cv_out : &g_cv_err;
            long cn = txt_out(cv, buf, n, g_cvbuf_p,
                              (long)sizeof(g_cvbuf_p));
            if (cn < 0)
                return -1;
            if (cn > 0 && lio_put_frame(sess, tag, g_cvbuf_p, cn) != LIO_OK)
                return -1;
            return 0;
        }
#endif
        if (lio_put_frame(sess, tag, buf, n) != LIO_OK)
            return -1;
    }
    return 0;
}

/* handle one I/K frame line from the RUNI client.
   returns 0 ok, -1 fatal (kill child, end session) */
static int runi_frame(gcds_chan_t *sess, const char *line, int *inp,
                      pid_t pid, int *killed, time_t *tkill)
{
    static char buf[GCDSP_FRAME_MAX];
    long n;

    if (line[0] == 'K' && line[1] == '\0') {
        kill_child(pid, killed, tkill);
        return 0;
    }
    if (line[0] == 'I' && line[1] == ' ') {
        n = atol(line + 2);
        if (n < 0 || n > GCDSP_FRAME_MAX)
            return -1;
        if (n == 0) {
            if (*inp >= 0) {
                close(*inp);
                *inp = -1;
            }
            return 0;
        }
        if (chan_read_n(sess, buf, n) < 0)
            return -1;
        /* after K, input is discarded (PLAN_01 5.1) */
        if (*inp >= 0 && *killed == 0) {
            long done;
            long w;

            done = 0;
            while (done < n) {
                w = (long)write(*inp, buf + done, (size_t)(n - done));
                if (w <= 0) {   /* child stdin gone; drop input */
                    close(*inp);
                    *inp = -1;
                    break;
                }
                done += w;
            }
        }
        return 0;
    }
    return -1;      /* anything else during RUNI is a violation */
}

int live_exec(int mode, gcds_chan_t *sess, gcds_sock_t ls,
              const char *cmd, const gcds_env_t *env, long nenv,
              const char *outf, const char *errf, long jobid)
{
    int inp[2];
    int outp[2];
    int errp[2];
    pid_t pid;
    int killed;
    time_t tkill;
    int reaped;
    int status;
    long chunk;
    long total;         /* bytes produced (streamed or written) */
    long cap;
    int capped;

    if (build_script(g_script, SCRIPT_MAX, cmd, env, nenv) < 0)
        return -1;
    total = 0;
    cap = session_maxout();
    capped = 0;

    inp[0] = inp[1] = -1;
    outp[0] = outp[1] = -1;
    errp[0] = errp[1] = -1;

    if (mode == LIVE_JOB) {
        outp[1] = open(outf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        errp[1] = open(errf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outp[1] < 0 || errp[1] < 0) {
            if (outp[1] >= 0)
                close(outp[1]);
            if (errp[1] >= 0)
                close(errp[1]);
            return -1;
        }
    } else {
        if (pipe(outp) < 0 || pipe(errp) < 0)
            return -1;
        if (mode == LIVE_RUNI && pipe(inp) < 0)
            return -1;
    }

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* child: become process-group leader so the supervisor
           can signal the whole job (sh + grandchildren) at once.
           Explicit getpid() (not the 0 default) so the 4.3BSD
           setpgrp(pid, pgrp) mapping in gnext.h works unchanged. */
        setpgid(0, getpid());
        if (mode == LIVE_RUNI)
            dup2(inp[0], 0);
        dup2(outp[1], 1);
        dup2(errp[1], 2);
        if (inp[0] >= 0)  close(inp[0]);
        if (inp[1] >= 0)  close(inp[1]);
        close(outp[1]);
        close(errp[1]);
        if (outp[0] >= 0) close(outp[0]);
        if (errp[0] >= 0) close(errp[0]);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", g_script, (char *)0);
        _exit(127);
    }

    /* parent */
    setpgid(pid, pid);      /* race-free with the child's setpgid */
    if (inp[0] >= 0)
        close(inp[0]);
    close(outp[1]);
    close(errp[1]);
    outp[1] = -1;
    errp[1] = -1;
    if (mode != LIVE_RUNI && inp[1] >= 0) {
        close(inp[1]);
        inp[1] = -1;
    }

    killed = 0;
    tkill = 0;
    reaped = 0;
    status = 0;
    chunk = (sess != NULL) ? chan_chunk(sess) : 0;

#ifdef GCDS_TEXTCV
    cv_reset();
#endif
    for (;;) {
        fd_set rd;
        struct timeval tv;
        int maxfd;
        int sfd;
        int r;

        FD_ZERO(&rd);
        maxfd = -1;
        sfd = -1;

        if (mode != LIVE_JOB) {
            if (outp[0] >= 0) {
                FD_SET(outp[0], &rd);
                if (outp[0] > maxfd) maxfd = outp[0];
            }
            if (errp[0] >= 0) {
                FD_SET(errp[0], &rd);
                if (errp[0] > maxfd) maxfd = errp[0];
            }
            sfd = chan_fd(sess);
            FD_SET(sfd, &rd);
            if (sfd > maxfd) maxfd = sfd;
        }
#ifdef GCDS_HAS_LIVE
        if (ls != GCDS_BADSOCK) {
            FD_SET((int)ls, &rd);
            if ((int)ls > maxfd) maxfd = (int)ls;
        }
#endif
        tv.tv_sec = 0;
        tv.tv_usec = 200000L;   /* 200ms tick: reap + kill grace */
        r = select(maxfd + 1, &rd, NULL, NULL, &tv);

        if (r > 0) {
#ifdef GCDS_HAS_LIVE
            if (ls != GCDS_BADSOCK && FD_ISSET((int)ls, &rd)) {
                gcds_sock_t cs;
                gcds_chan_t cc;

                char cip[16];

                cs = net_accept(ls, cip);
                if (cs != GCDS_BADSOCK) {
                    if (session_peer_ok(cip)) {
                        chan_tcp(&cc, cs);
                        ctl_session(&cc,
                            (mode == LIVE_JOB) ? jobid : 0);
                        chan_close(&cc);
                    } else {
                        net_close(cs);
                    }
                }
            }
#endif
            if (mode != LIVE_JOB) {
                if (outp[0] >= 0 && FD_ISSET(outp[0], &rd)) {
                    r = pump(sess, 'O', outp[0], chunk, &total);
                    if (r != 0) {
                        close(outp[0]);
                        outp[0] = -1;
                        if (r < 0)
                            goto lost;
                    }
                }
                if (errp[0] >= 0 && FD_ISSET(errp[0], &rd)) {
                    r = pump(sess, 'E', errp[0], chunk, &total);
                    if (r != 0) {
                        close(errp[0]);
                        errp[0] = -1;
                        if (r < 0)
                            goto lost;
                    }
                }
                if (cap > 0 && total > cap && !capped) {
                    /* streamed enough: notify, kill, let it drain */
                    capped = 1;
                    lio_put_frame(sess, 'E',
                        "\n[gcdsd: output truncated at cap]\n", 34);
                    kill_child(pid, &killed, &tkill);
                    hardkill_child(pid);
                }
                if (FD_ISSET(sfd, &rd)) {
                    if (mode == LIVE_RUN) {
                        /* the client must stay silent during RUN:
                           any byte (or EOF) kills the job and the
                           session (PLAN_01 section 4) */
                        goto lost;
                    } else {
                        static char line[GCDSP_LINE_MAX];

                        if (lio_get_line(sess, line, GCDSP_LINE_MAX)
                            != LIO_OK)
                            goto lost;
                        if (runi_frame(sess, line, &inp[1], pid,
                                       &killed, &tkill) < 0)
                            goto lost;
                    }
                }
            }
        }

        /* JOB mode: child writes the capture files directly, so
           poll their size and kill if the cap is exceeded (disk
           protection for async jobs) */
        if (mode == LIVE_JOB && cap > 0 && !capped) {
            struct stat sb;
            long tot;

            tot = 0;
            if (stat(outf, &sb) == 0) tot += (long)sb.st_size;
            if (stat(errf, &sb) == 0) tot += (long)sb.st_size;
            if (tot > cap) {
                capped = 1;
                kill_child(pid, &killed, &tkill);
                hardkill_child(pid);
            }
        }

        /* SIGTERM -> SIGKILL escalation */
        if (killed == 1 && time(NULL) - tkill >= KILL_GRACE) {
            hardkill_child(pid);
            killed = 2;
        }

        if (!reaped && waitpid(pid, &status, WNOHANG) == pid)
            reaped = 1;

        if (mode == LIVE_JOB) {
            if (reaped)
                break;
        } else {
            if (outp[0] < 0 && errp[0] < 0) {
                if (!reaped) {
                    waitpid(pid, &status, 0);
                    reaped = 1;
                }
                break;
            }
        }
    }

#ifdef GCDS_TEXTCV
    cv_flush_all(sess);              /* emit held-back tails */
#endif
    if (inp[1] >= 0)
        close(inp[1]);

    if (WIFEXITED(status))
        return WEXITSTATUS(status) & 255;
    if (WIFSIGNALED(status))
        return (128 + WTERMSIG(status)) & 255;
    return -1;

lost:
    kill_child(pid, &killed, &tkill);
    hardkill_child(pid);
    if (inp[1] >= 0)
        close(inp[1]);
    if (outp[0] >= 0)
        close(outp[0]);
    if (errp[0] >= 0)
        close(errp[0]);
    waitpid(pid, &status, 0);
    return -2;
}

#else

typedef int gcds_live_unused;

#endif /* GCDS_HAS_LIVE || GCDS_HAS_IX */
