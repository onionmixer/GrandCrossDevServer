/*
 * main.c - gcdsd: iterative single-client daemon (PLAN_00 D1).
 * TCP mode: accept -> session -> close -> (run pending RUNA job).
 * Serial mode: wait HELLO -> session -> (run pending job) -> wait.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#if !defined(GCDS_WIN32) && !defined(GCDS_DOS)
#include <unistd.h>     /* _exit for the signal handler */
#endif

#include "gcdsp.h"
#include "util.h"
#include "conf.h"
#include "lineio.h"
#include "session.h"
#include "live.h"

static gcds_kv_t g_kv[CONF_ENT_MAX];
static long g_nkv;

#if !defined(GCDS_WIN32) && !defined(GCDS_DOS)
/* graceful shutdown: drop capture temp files and exit. Only
   async-signal-safe-ish work (unlink + _exit) is done here. */
static void on_term(int sig)
{
    (void)sig;
    sess_cleanup_tmp();
    _exit(0);
}
#endif

static void run_pending(gcds_job_t *job, gcds_res_t *res, gcds_sock_t ls)
{
    int code;

    if (!job->pending)
        return;
    job->pending = 0;
    fprintf(stderr, "gcdsd: job %ld: %s\n", job->jobid, job->cmd);
#ifdef GCDS_HAS_LIVE
    /* supervised even for async jobs: control sessions (PING/
       STAT busy) stay answerable while the job runs */
    code = live_exec(LIVE_JOB, NULL, ls, job->cmd, job->env,
                     job->nenv, sess_ro_path(), sess_re_path(),
                     job->jobid);
#else
    (void)ls;
    code = run_command(job->cmd, job->env, job->nenv,
                       sess_ro_path(), sess_re_path());
#endif
    if (code < 0) {
        fprintf(stderr, "gcdsd: job %ld: exec failed\n", job->jobid);
        res->valid = 0;
        return;
    }
    res->valid = 1;
    res->jobid = job->jobid;
    res->code = code;
    fprintf(stderr, "gcdsd: job %ld: exit %d\n", job->jobid, code);
}

/* job/res carry a ~9KB env array; keep them out of the stack
   (single iterative instance, so static is semantically fine and
   essential on 16-bit DOS where the stack is only a few KB) */
static gcds_job_t g_job;
static gcds_res_t g_res;

static int serve_tcp(unsigned short port)
{
    gcds_sock_t ls;
    gcds_sock_t s;
    gcds_chan_t ch;
    char ip[16];

    ls = net_listen(port);
    if (ls == GCDS_BADSOCK) {
        fprintf(stderr, "gcdsd: cannot listen on port %u\n",
                (unsigned)port);
        return 1;
    }
    fprintf(stderr, "gcdsd: listening on port %u\n", (unsigned)port);

    g_res.valid = 0;
    g_job.pending = 0;
    for (;;) {
        s = net_accept(ls, ip);
        if (s == GCDS_BADSOCK)
            continue;
        if (!session_peer_ok(ip)) {
            fprintf(stderr, "gcdsd: rejected %s (not in allow)\n", ip);
            net_close(s);
            continue;
        }
        fprintf(stderr, "gcdsd: connection from %s\n", ip);
        chan_tcp(&ch, s);
        session_run(&ch, &g_job, &g_res, ls);
        chan_close(&ch);
        run_pending(&g_job, &g_res, ls);
    }
    /* not reached */
}

static int serve_serial(const char *dev, long baud)
{
    gcds_ser_t f;
    gcds_chan_t ch;
    static char line[GCDSP_LINE_MAX];
    int r;

    f = ser_open(dev, baud);
    if (f == GCDS_BADSER) {
        fprintf(stderr, "gcdsd: cannot open serial %s at %ld\n",
                dev, baud);
        return 1;
    }
    fprintf(stderr, "gcdsd: serial mode on %s (%ld 8N1)\n", dev, baud);
    chan_ser(&ch, f);

    g_res.valid = 0;
    g_job.pending = 0;
    for (;;) {
        /* HELLO attention wait; everything else is line noise
           and is silently dropped (PLAN_01 1.1 resync rule) */
        r = lio_get_line(&ch, line, GCDSP_LINE_MAX);
        if (r == LIO_EIO) {
            fprintf(stderr, "gcdsd: serial port lost\n");
            return 1;
        }
        if (r == LIO_ELONG || strcmp(line, "HELLO") != 0)
            continue;
        session_run(&ch, &g_job, &g_res, GCDS_BADSOCK);
        run_pending(&g_job, &g_res, GCDS_BADSOCK);
    }
    /* not reached */
}

int main(int argc, char **argv)
{
    const char *cfile;
    const char *token;
    const char *tmpdir;
    const char *serial;
    const char *s;
    long port;
    int adv_async;

    cfile = NULL;
    if (argc == 3 && strcmp(argv[1], "-c") == 0) {
        cfile = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "usage: gcdsd [-c conffile]\n");
        return 2;
    }

    if (cfile != NULL) {
        /* explicit -c: use exactly what was asked for */
        g_nkv = conf_load(cfile, g_kv, CONF_ENT_MAX);
        if (g_nkv < 0) {
            fprintf(stderr, "gcdsd: cannot read %s\n", cfile);
            return 2;
        }
    } else {
        /* Default lookup: the 3-char extension comes FIRST - that is what
           dist/ ships, so the config works with no rename.
           MS-DOS is strictly 8.3: a 4-char ".conf" cannot exist on FAT, so
           the DOS build looks for gcdsd.cnf only (also saves DGROUP).
           Windows/POSIX fall back to gcdsd.conf. */
        cfile = "gcdsd.cnf";
        g_nkv = conf_load(cfile, g_kv, CONF_ENT_MAX);
#ifndef GCDS_DOS
        if (g_nkv < 0) {
            cfile = "gcdsd.conf";
            g_nkv = conf_load(cfile, g_kv, CONF_ENT_MAX);
        }
#endif
        if (g_nkv < 0) {
#ifdef GCDS_DOS
            fprintf(stderr, "gcdsd: cannot read gcdsd.cnf\n");
#else
            fprintf(stderr, "gcdsd: cannot read gcdsd.cnf or gcdsd.conf\n");
#endif
            return 2;
        }
    }

    token = conf_get(g_kv, g_nkv, "token");
    if (token == NULL || token[0] == '\0') {
        fprintf(stderr, "gcdsd: 'token' is required in %s\n", cfile);
        return 2;
    }
    port = atol(conf_gets(g_kv, g_nkv, "port", "9910"));
#if defined(GCDS_WIN32) || defined(GCDS_DOS)
    tmpdir = conf_gets(g_kv, g_nkv, "tmpdir", ".");
#else
    tmpdir = conf_gets(g_kv, g_nkv, "tmpdir", "/tmp");
#endif
    s = conf_gets(g_kv, g_nkv, "async", "0");
    adv_async = (s[0] == '1') ? 1 : 0;
    serial = conf_get(g_kv, g_nkv, "serial");

    {
        int live_ok;
        int ix_ok;
        int is_serial;

        is_serial = (serial != NULL && serial[0] != '\0');
        live_ok = 0;
        ix_ok = 0;
#ifdef GCDS_HAS_LIVE
        /* LIVE is TCP-only: a serial channel has no second lane
           for control sessions (PLAN_01 4.1) */
        if (!is_serial)
            live_ok = 1;
#endif
#ifdef GCDS_HAS_IX
#ifdef GCDS_WIN32
        /* Winsock select cannot watch a COM handle */
        if (!is_serial)
            ix_ok = 1;
#else
        ix_ok = 1;      /* POSIX select covers serial fds too */
#endif
#endif
        session_init(tmpdir, token, adv_async, live_ok, ix_ok);
    }
    session_set_acl(conf_get(g_kv, g_nkv, "allow"));
    session_set_maxout(atol(conf_gets(g_kv, g_nkv, "maxout", "0")));

#if !defined(GCDS_WIN32) && !defined(GCDS_DOS)
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
#endif
    if (net_init() != 0) {
        fprintf(stderr, "gcdsd: network init failed\n");
        return 2;
    }

    if (serial != NULL && serial[0] != '\0') {
        /* "dev:baud", baud optional (default 9600) */
        static char dev[CONF_VAL_MAX];
        char *colon;
        long baud;

        gcds_strlcpy(dev, serial, (long)sizeof(dev));
        colon = strrchr(dev, ':');
        baud = 9600;
        if (colon != NULL) {
            *colon = '\0';
            baud = atol(colon + 1);
        }
        return serve_serial(dev, baud);
    }
    return serve_tcp((unsigned short)port);
}
