/*
 * main.c - gcds: instruct a remote gcdsd and mirror its
 * stdout/stderr/exit code locally (PLAN_00 section 3).
 *
 * exit codes: remote command's code as-is; 125 for any local,
 * connection or protocol failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcdsp.h"
#include "util.h"
#include "conf.h"
#include "lineio.h"
#include "remote.h"

#define EX_FAIL 125

/* pathmap.c / asyncjob.c / toolias.c */
int pm_cwd(const gcds_kv_t *kv, long nkv, const char *alias,
           char *out, long size);
long poll_result(const gcds_kv_t *kv, long nkv, const char *alias,
                 long jobid);
int tool_expand(const gcds_kv_t *kv, long nkv, const char *alias,
                const char *name, char *out, long size);
long ix_client(gcds_rc_t *rc, const char *cmd);   /* ixmode.c */
int xfer_put(gcds_rc_t *rc, const char *lpath, const char *rpath);
int xfer_get(gcds_rc_t *rc, const char *rpath, const char *lpath);

static gcds_kv_t g_kv[CONF_ENT_MAX];
static long g_nkv;

/* load "<dir><name>" into g_kv (dir may be NULL). 0 on success, -1 if
   the file is not there. */
static int try_conf(const char *dir, const char *name)
{
    static char path[CONF_VAL_MAX];

    path[0] = '\0';
    if (dir != NULL)
        gcds_strlcat(path, dir, CONF_VAL_MAX);
    gcds_strlcat(path, name, CONF_VAL_MAX);
    g_nkv = conf_load(path, g_kv, CONF_ENT_MAX);
    return (g_nkv >= 0) ? 0 : -1;
}

static int load_conf(void)
{
    const char *p;

    p = getenv("GCDS_CONF");
    if (p != NULL) {
        g_nkv = conf_load(p, g_kv, CONF_ENT_MAX);
        return (g_nkv >= 0) ? 0 : -1;
    }
    /* 3-char extension first - that is what etc/ and dist/ ship, so the
       config works with no rename; .conf stays supported. */
    if (try_conf(NULL, "gcds.cnf") == 0)
        return 0;
    if (try_conf(NULL, "gcds.conf") == 0)
        return 0;
    p = getenv("HOME");
    if (p != NULL) {
        if (try_conf(p, "/.gcds.cnf") == 0)
            return 0;
        if (try_conf(p, "/.gcds.conf") == 0)
            return 0;
    }
    fprintf(stderr,
            "gcds: no config ($GCDS_CONF, ./gcds.cnf, ./gcds.conf, "
            "~/.gcds.cnf, ~/.gcds.conf)\n");
    return -1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: gcds [opts] <host> <command...>   run remotely\n"
        "       gcds --ping <host>                alive check\n"
        "       gcds --stat <host>                status check\n"
        "       gcds --put <host> <local> <remote>   upload\n"
        "       gcds --get <host> <remote> <local>   download\n");
    fprintf(stderr,
        "opts:  --async      force RUNA/RESULT job model\n"
        "       -i           interactive (RUNI): stdin forwarded,\n"
        "                    Ctrl-C kills the remote command\n"
        "       --mapback    rewrite remote paths in output to\n"
        "                    local paths (text output only)\n"
        "       @<name>      first command word: tool alias\n");
}

int main(int argc, char **argv)
{
    static char cmd[GCDSP_LINE_MAX];
    static char line[GCDSP_LINE_MAX];
    static char rcwd[CONF_VAL_MAX];
    gcds_rc_t rc;
    const char *alias;
    int do_ping;
    int do_stat;
    int do_ix;
    int do_put;
    int do_get;
    int mapback;
    int force_async;
    int ai;
    long code;

    do_ping = 0;
    do_stat = 0;
    do_ix = 0;
    do_put = 0;
    do_get = 0;
    mapback = 0;
    force_async = 0;
    ai = 1;
    while (ai < argc && argv[ai][0] == '-') {
        if (strcmp(argv[ai], "--ping") == 0)
            do_ping = 1;
        else if (strcmp(argv[ai], "--stat") == 0)
            do_stat = 1;
        else if (strcmp(argv[ai], "--async") == 0)
            force_async = 1;
        else if (strcmp(argv[ai], "-i") == 0)
            do_ix = 1;
        else if (strcmp(argv[ai], "--mapback") == 0)
            mapback = 1;
        else if (strcmp(argv[ai], "--put") == 0)
            do_put = 1;
        else if (strcmp(argv[ai], "--get") == 0)
            do_get = 1;
        else {
            usage();
            return EX_FAIL;
        }
        ai++;
    }
    if (ai >= argc) {
        usage();
        return EX_FAIL;
    }
    alias = argv[ai];
    ai++;

    if (do_put || do_get) {
        if (argc - ai != 2) {       /* need exactly two paths */
            usage();
            return EX_FAIL;
        }
    } else if (!do_ping && !do_stat && ai >= argc) {
        usage();
        return EX_FAIL;
    }
    if (load_conf() < 0)
        return EX_FAIL;
    if (net_init() != 0)
        return EX_FAIL;

    /* join remaining args into the remote command line (RUN modes
       only); a leading @name expands to the host's tool alias */
    cmd[0] = '\0';
    if (!do_put && !do_get) {
        if (ai < argc && argv[ai][0] == '@') {
            if (tool_expand(g_kv, g_nkv, alias, argv[ai] + 1,
                            cmd, GCDSP_LINE_MAX) < 0)
                return EX_FAIL;
            ai++;
        }
        for (; ai < argc; ai++) {
            if (cmd[0] != '\0')
                gcds_strlcat(cmd, " ", GCDSP_LINE_MAX);
            if (gcds_strlcat(cmd, argv[ai], GCDSP_LINE_MAX)
                >= GCDSP_LINE_MAX - 8) {
                fprintf(stderr, "gcds: command line too long\n");
                return EX_FAIL;
            }
        }
    }

    if (rc_open(&rc, g_kv, g_nkv, alias) < 0) {
        fprintf(stderr, "gcds: %s: no response (busy or down)\n",
                alias);
        return EX_FAIL;
    }
    if (rc_auth(&rc, g_kv, g_nkv, alias) < 0) {
        rc_close(&rc);
        return EX_FAIL;
    }

    if (do_put || do_get) {
        int xr;
        if (do_put)
            xr = xfer_put(&rc, argv[ai], argv[ai + 1]);
        else
            xr = xfer_get(&rc, argv[ai], argv[ai + 1]);
        rc_close(&rc);
        return (xr == 0) ? 0 : EX_FAIL;
    }

    if (do_ping) {
        if (rc_cmd(&rc, "PING", line, GCDSP_LINE_MAX) == 0 &&
            strcmp(line, "OK") == 0) {
            printf("%s: alive (%s, %s)\n", alias, rc.ostag, rc.rhost);
            rc_close(&rc);
            return 0;
        }
        fprintf(stderr, "gcds: %s: bad ping reply\n", alias);
        rc_close(&rc);
        return EX_FAIL;
    }

    if (do_stat) {
        if (rc_cmd(&rc, "STAT", line, GCDSP_LINE_MAX) == 0 &&
            gcds_starts(line, "OK")) {
            printf("%s: %s\n", alias, line + 3);
            rc_close(&rc);
            return 0;
        }
        fprintf(stderr, "gcds: %s: bad stat reply\n", alias);
        rc_close(&rc);
        return EX_FAIL;
    }

    /* cwd mapping (PLAN_00 D6) */
    code = pm_cwd(g_kv, g_nkv, alias, rcwd, CONF_VAL_MAX);
    if (code < 0) {
        fprintf(stderr, "gcds: mapped cwd too long\n");
        rc_close(&rc);
        return EX_FAIL;
    }
    if (code == 1) {
        line[0] = '\0';
        gcds_strlcat(line, "CWD ", GCDSP_LINE_MAX);
        gcds_strlcat(line, rcwd, GCDSP_LINE_MAX);
        if (rc_cmd(&rc, line, line, GCDSP_LINE_MAX) < 0 ||
            strcmp(line, "OK") != 0) {
            fprintf(stderr, "gcds: %s: CWD %s failed: %s\n",
                    alias, rcwd, line);
            rc_close(&rc);
            return EX_FAIL;
        }
    }

    if (mapback)
        rc_mapback(g_kv, g_nkv, alias);

    if (do_ix) {
        if (!rc.cap_ix) {
            fprintf(stderr, "gcds: %s does not support interactive "
                    "execution (no INTERACTIVE capability)\n", alias);
            rc_close(&rc);
            return EX_FAIL;
        }
        code = ix_client(&rc, cmd);
        rc_close(&rc);
        return (code < 0) ? EX_FAIL : (int)code;
    }

    if (rc.cap_async || force_async) {
        long jobid;

        line[0] = '\0';
        gcds_strlcat(line, "RUNA ", GCDSP_LINE_MAX);
        gcds_strlcat(line, cmd, GCDSP_LINE_MAX);
        if (rc_cmd(&rc, line, line, GCDSP_LINE_MAX) < 0 ||
            !gcds_starts(line, "OK ")) {
            fprintf(stderr, "gcds: %s: submit failed: %s\n",
                    alias, line);
            rc_close(&rc);
            return EX_FAIL;
        }
        jobid = atol(line + 3);
        rc_close(&rc);          /* daemon closes too, then runs */
        fprintf(stderr, "gcds: job %ld submitted to %s\n",
                jobid, alias);
        code = poll_result(g_kv, g_nkv, alias, jobid);
        return (code < 0) ? EX_FAIL : (int)code;
    }

    line[0] = '\0';
    gcds_strlcat(line, "RUN ", GCDSP_LINE_MAX);
    gcds_strlcat(line, cmd, GCDSP_LINE_MAX);
    if (lio_put_line(&rc.ch, line) != LIO_OK) {
        rc_close(&rc);
        return EX_FAIL;
    }
    code = rc_stream(&rc);
    rc_close(&rc);
    return (code < 0) ? EX_FAIL : (int)code;
}
