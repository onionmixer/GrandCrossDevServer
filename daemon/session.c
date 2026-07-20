/*
 * session.c - GCDSP v1 command dispatch (PLAN_01 sections 3-5,7).
 * Platform-independent: everything OS-specific is behind exec.h
 * and chan.h. Strict C89, fixed buffers only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GCDS_WIN32
#include <direct.h>     /* _chdir */
#include <io.h>         /* _unlink */
#define chdir  _chdir
#define unlink _unlink
#elif defined(GCDS_DOS)
#include <direct.h>     /* chdir (Watcom, plain names) */
#include <io.h>         /* unlink */
#else
#include <unistd.h>     /* chdir, unlink */
#endif

#include "gcdsp.h"
#include "util.h"
#include "acl.h"
#include "textcv.h"
#include "lineio.h"
#include "session.h"
#include "live.h"

#define PATHBUF 256

#define ALLOWBUF 512

static char g_token[GCDSP_TOK_MAX + 1];
static int  g_async;
static int  g_live;
static int  g_ix;
static char g_allow[ALLOWBUF];  /* TCP peer allow-list, "" = all */
static long g_maxout;           /* per-job output cap, 0 = off   */
static char g_outf[PATHBUF];    /* blocking RUN capture */
static char g_errf[PATHBUF];
static char g_rof[PATHBUF];     /* stored RUNA result   */
static char g_ref[PATHBUF];
/* one frame-sized scratch buffer shared by send_file/do_put/do_get.
   the daemon is single-threaded and these never nest, so sharing it
   keeps the DOS large-model DGROUP within 64K (PLAN_02 4). */
static char g_fbuf[GCDSP_FRAME_MAX];

#ifdef GCDS_TEXTCV
/* conversion scratch: local->wire can grow the text (a 2-byte DBCS
   char becomes 3 UTF-8 bytes), so this is sized with headroom. */
static char g_cvbuf[GCDSP_FRAME_MAX * 2];
#endif

/* Separator used when joining tmpdir with our temp file names.
   Windows and MS-DOS use '\' natively, and on DOS '/' is the command
   SWITCH character - a path that may reach COMMAND.COM must not use it
   (exec_w32.c also hands these paths to cmd.exe for redirection).
   The configured tmpdir may be written with either spelling; a trailing
   separator of either kind is accepted and not doubled. */
#if defined(GCDS_WIN32) || defined(GCDS_DOS)
#define TMP_SEP "\\"
#else
#define TMP_SEP "/"
#endif

static void join_tmp(char *dst, const char *dir, const char *name)
{
    long n;

    gcds_strlcpy(dst, dir, PATHBUF);
    n = (long)strlen(dst);
    if (n > 0 && dst[n - 1] != '/' && dst[n - 1] != '\\')
        gcds_strlcat(dst, TMP_SEP, PATHBUF);
    gcds_strlcat(dst, name, PATHBUF);
}

void session_init(const char *tmpdir, const char *token,
                  int adv_async, int live_ok, int ix_ok)
{
    gcds_strlcpy(g_token, token, (long)sizeof(g_token));
    g_async = adv_async;
    g_live = live_ok;
    g_ix = ix_ok;

    join_tmp(g_outf, tmpdir, "gcds_out.tmp");
    join_tmp(g_errf, tmpdir, "gcds_err.tmp");
    join_tmp(g_rof,  tmpdir, "gcds_ro.tmp");
    join_tmp(g_ref,  tmpdir, "gcds_re.tmp");
}

void session_set_acl(const char *allow)
{
    if (allow == NULL)
        g_allow[0] = '\0';
    else
        gcds_strlcpy(g_allow, allow, ALLOWBUF);
}

void session_set_maxout(long maxout)
{
    g_maxout = (maxout > 0) ? maxout : 0;
}

int session_peer_ok(const char *ip)
{
    return acl_allowed(g_allow, ip);
}

long session_maxout(void)
{
    return g_maxout;
}

const char *sess_ro_path(void)
{
    return g_rof;
}

const char *sess_re_path(void)
{
    return g_ref;
}

void sess_cleanup_tmp(void)
{
    unlink(g_outf);
    unlink(g_errf);
    unlink(g_rof);
    unlink(g_ref);
}

static long next_jobid(void)
{
    static long j = 0;

    j++;
    if (j > GCDSP_JOB_MAX)
        j = 1;
    return j;
}

/* stream a capture file as tag frames; absent file = no output.
   *sent accumulates bytes across O and E; once g_maxout is reached
   the rest is dropped and a one-line notice is sent on stderr. */
static int send_file(gcds_chan_t *c, char tag, const char *path,
                     long *sent)
{
#ifdef GCDS_TEXTCV
    txt_stream_t cv;
#endif
    FILE *fp;
    long chunk;
    long n;
    long cap;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return 0;
    cap = g_maxout;
    chunk = chan_chunk(c);
#ifdef GCDS_TEXTCV
    txt_stream_init(&cv);
#endif
    for (;;) {
        n = (long)fread(g_fbuf, 1, (size_t)chunk, fp);
        if (n <= 0)
            break;
#ifdef GCDS_TEXTCV
        /* text frames carry the wire encoding (textcv.h); D frames are
           raw and never come through here */
        if (txt_active()) {
            long cn = txt_out(&cv, g_fbuf, n, g_cvbuf, (long)sizeof(g_cvbuf));
            if (cn < 0) { fclose(fp); return -1; }
            if (cn == 0) continue;          /* all held for next chunk */
            memcpy(g_fbuf, g_cvbuf, (size_t)cn);
            n = cn;
        }
#endif
        if (cap > 0 && *sent + n > cap) {
            n = cap - *sent;        /* send only up to the cap */
            if (n > 0 && lio_put_frame(c, tag, g_fbuf, n) != LIO_OK) {
                fclose(fp);
                return -1;
            }
            *sent += n;
            fclose(fp);
            lio_put_frame(c, 'E',
                "\n[gcdsd: output truncated at cap]\n", 34);
            return 0;
        }
        if (lio_put_frame(c, tag, g_fbuf, n) != LIO_OK) {
            fclose(fp);
            return -1;
        }
        *sent += n;
    }
#ifdef GCDS_TEXTCV
    if (txt_active()) {                     /* emit any held-back tail */
        long cn = txt_flush(&cv, g_cvbuf, (long)sizeof(g_cvbuf));
        if (cn > 0 && lio_put_frame(c, tag, g_cvbuf, cn) != LIO_OK) {
            fclose(fp);
            return -1;
        }
        *sent += cn;
    }
#endif
    fclose(fp);
    return 0;
}

/* PUT: receive D-frames from the client into `path`.
   returns 0 to continue the session, -1 to end it (IO/violation). */
static int do_put(gcds_chan_t *c, const char *path)
{
    char line[48];      /* D-frame header is short; long => reject */
    FILE *fp;
    long len;
    long total;
    long cap;

    if (path == NULL || path[0] == '\0') {
        return lio_put_line(c, "ERR 400 missing path") == LIO_OK ? 0 : -1;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        return lio_put_line(c, "ERR 404 cannot create") == LIO_OK ? 0 : -1;
    }
    if (lio_put_line(c, "OK") != LIO_OK) {
        fclose(fp);
        return -1;
    }
    cap = g_maxout;
    total = 0;
    for (;;) {
        if (lio_get_line(c, line, (long)sizeof(line)) != LIO_OK) {
            fclose(fp);
            unlink(path);
            return -1;              /* client vanished / header too long */
        }
        if (line[0] != 'D' || line[1] != ' ') {
            fclose(fp);
            unlink(path);
            lio_put_line(c, "ERR 400 expected data frame");
            return -1;
        }
        len = atol(line + 2);
        if (len == 0)
            break;                  /* EOF marker */
        if (len < 0 || len > GCDSP_FRAME_MAX) {
            fclose(fp);
            unlink(path);
            lio_put_line(c, "ERR 400 bad frame length");
            return -1;
        }
        if (chan_read_n(c, g_fbuf, len) < 0) {
            fclose(fp);
            unlink(path);
            return -1;
        }
        if (cap > 0 && total + len > cap) {
            fclose(fp);
            unlink(path);
            return lio_put_line(c, "ERR 413 too large") == LIO_OK ? 0 : -1;
        }
        if ((long)fwrite(g_fbuf, 1, (size_t)len, fp) != len) {
            fclose(fp);
            unlink(path);
            return lio_put_line(c, "ERR 500 write failed") == LIO_OK ? 0 : -1;
        }
        total += len;
    }
    if (fclose(fp) != 0) {
        unlink(path);
        return lio_put_line(c, "ERR 500 write failed") == LIO_OK ? 0 : -1;
    }
    {
        char rl[48];
        sprintf(rl, "OK %ld", total);
        return lio_put_line(c, rl) == LIO_OK ? 0 : -1;
    }
}

/* GET: send `path` to the client as D-frames + a D 0 terminator. */
static int do_get(gcds_chan_t *c, const char *path)
{
    FILE *fp;
    long chunk;
    long n;

    if (path == NULL || path[0] == '\0') {
        return lio_put_line(c, "ERR 400 missing path") == LIO_OK ? 0 : -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return lio_put_line(c, "ERR 404 no such file") == LIO_OK ? 0 : -1;
    }
    if (lio_put_line(c, "OK") != LIO_OK) {
        fclose(fp);
        return -1;
    }
    chunk = chan_chunk(c);
    for (;;) {
        n = (long)fread(g_fbuf, 1, (size_t)chunk, fp);
        if (n <= 0)
            break;
        if (lio_put_frame(c, 'D', g_fbuf, n) != LIO_OK) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return lio_put_line(c, "D 0") == LIO_OK ? 0 : -1;
}

static int send_exit(gcds_chan_t *c, int code)
{
    char line[32];

    sprintf(line, "X %d", code & 255);
    return lio_put_line(c, line);
}

/* valid env name: [A-Za-z_][A-Za-z0-9_]* */
static int env_name_ok(const char *s)
{
    long i;

    if (!((s[0] >= 'A' && s[0] <= 'Z') ||
          (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return 0;
    for (i = 1; s[i] != '\0'; i++) {
        if (!((s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
            return 0;
    }
    return 1;
}

static int put_greeting(gcds_chan_t *c)
{
    static char greet[GCDSP_LINE_MAX];
    char host[64];

    os_hostname(host, (long)sizeof(host));
    greet[0] = '\0';
    gcds_strlcat(greet, "GCDS 1 ", GCDSP_LINE_MAX);
    gcds_strlcat(greet, os_tag(), GCDSP_LINE_MAX);
    gcds_strlcat(greet, " ", GCDSP_LINE_MAX);
    gcds_strlcat(greet, host, GCDSP_LINE_MAX);
    if (g_async)
        gcds_strlcat(greet, " ASYNC", GCDSP_LINE_MAX);
    if (g_live)
        gcds_strlcat(greet, " LIVE", GCDSP_LINE_MAX);
    if (g_ix)
        gcds_strlcat(greet, " INTERACTIVE", GCDSP_LINE_MAX);
#if TXT_WIRE_UTF8
    /* text frames are guaranteed UTF-8 (converted if the local
       encoding differs). DOS/NeXTSTEP only assume ASCII and omit it. */
    gcds_strlcat(greet, " UTF8", GCDSP_LINE_MAX);
#endif
    return lio_put_line(c, greet);
}

/* AUTH handshake shared by full and control sessions.
   returns 1 authed, 0 session ended (reply already sent). */
static int handle_auth(gcds_chan_t *c, const char *cmd,
                       const char *arg)
{
    if (strcmp(cmd, "QUIT") == 0) {
        /* pre-auth QUIT: stale-session cleanup on serial
           channels (PLAN_01 1.1) */
        lio_put_line(c, "OK");
        return 0;
    }
    if (strcmp(cmd, "AUTH") != 0 || arg == NULL ||
        strcmp(arg, g_token) != 0) {
        lio_put_line(c, "ERR 401 auth failed");
        return 0;
    }
    if (lio_put_line(c, "OK") != LIO_OK)
        return 0;
    return 1;
}

void ctl_session(gcds_chan_t *c, long busy_jobid)
{
    static char line[GCDSP_LINE_MAX];
    char rl[48];
    int authed;
    char *arg;

    authed = 0;
    if (put_greeting(c) != LIO_OK)
        return;
    for (;;) {
        if (lio_get_line(c, line, GCDSP_LINE_MAX) != LIO_OK)
            return;
        arg = strchr(line, ' ');
        if (arg != NULL) {
            *arg = '\0';
            arg++;
        }
        if (!authed) {
            if (!handle_auth(c, line, arg))
                return;
            authed = 1;
            continue;
        }
        if (strcmp(line, "PING") == 0) {
            if (lio_put_line(c, "OK") != LIO_OK)
                return;
        } else if (strcmp(line, "STAT") == 0) {
            sprintf(rl, "OK busy %ld", busy_jobid);
            if (lio_put_line(c, rl) != LIO_OK)
                return;
        } else if (strcmp(line, "QUIT") == 0) {
            lio_put_line(c, "OK");
            return;
        } else {
            if (lio_put_line(c, "ERR 409 busy") != LIO_OK)
                return;
        }
    }
}

void session_run(gcds_chan_t *c, gcds_job_t *job, gcds_res_t *res,
                 gcds_sock_t ls)
{
    static char line[GCDSP_LINE_MAX];
    static gcds_env_t env[GCDSP_ENV_MAX];
    long nenv;
    int authed;
    int r;
    char *arg;
    int code;

    job->pending = 0;
    nenv = 0;
    authed = 0;
#if !defined(GCDS_HAS_LIVE) && !defined(GCDS_HAS_IX)
    (void)ls;
#endif

    if (put_greeting(c) != LIO_OK)
        return;

    for (;;) {
        r = lio_get_line(c, line, GCDSP_LINE_MAX);
        if (r == LIO_ELONG) {
            lio_put_line(c, "ERR 400 protocol violation");
            return;
        }
        if (r != LIO_OK)
            return;
#ifdef GCDS_TEXTCV
        /* command line, CWD and ENV values arrive in the wire encoding;
           the local shell/filesystem wants the local one */
        if (txt_active()) {
            long cn = txt_in(line, (long)strlen(line), g_cvbuf,
                             (long)sizeof(g_cvbuf));
            if (cn > 0 && cn < GCDSP_LINE_MAX) {
                memcpy(line, g_cvbuf, (size_t)cn);
                line[cn] = '\0';
            }
        }
#endif

        arg = strchr(line, ' ');
        if (arg != NULL) {
            *arg = '\0';
            arg++;
        }

        if (!authed) {
            if (!handle_auth(c, line, arg))
                return;
            authed = 1;
            continue;
        }

        if (strcmp(line, "PING") == 0) {
            if (lio_put_line(c, "OK") != LIO_OK)
                return;

        } else if (strcmp(line, "STAT") == 0) {
            if (res->valid) {
                char rl[48];
                sprintf(rl, "OK result %ld", res->jobid);
                if (lio_put_line(c, rl) != LIO_OK)
                    return;
            } else {
                if (lio_put_line(c, "OK idle") != LIO_OK)
                    return;
            }

        } else if (strcmp(line, "CWD") == 0) {
            if (arg == NULL || chdir(arg) != 0) {
                if (lio_put_line(c, "ERR 404 no such directory")
                    != LIO_OK)
                    return;
            } else {
                if (lio_put_line(c, "OK") != LIO_OK)
                    return;
            }

        } else if (strcmp(line, "ENV") == 0) {
            char *eq;
            long i;

            eq = (arg == NULL) ? NULL : strchr(arg, '=');
            if (eq == NULL) {
                if (lio_put_line(c, "ERR 400 bad env") != LIO_OK)
                    return;
                continue;
            }
            *eq = '\0';
            if (!env_name_ok(arg) ||
                (long)strlen(arg) >= GCDSP_ENVN_MAX ||
                (long)strlen(eq + 1) >= GCDSP_ENVV_MAX) {
                if (lio_put_line(c, "ERR 431 env too long") != LIO_OK)
                    return;
                continue;
            }
            /* overwrite same name */
            for (i = 0; i < nenv; i++) {
                if (strcmp(env[i].name, arg) == 0)
                    break;
            }
            if (i == nenv) {
                if (nenv >= GCDSP_ENV_MAX) {
                    if (lio_put_line(c, "ERR 431 too many env")
                        != LIO_OK)
                        return;
                    continue;
                }
                nenv++;
            }
            gcds_strlcpy(env[i].name, arg, GCDSP_ENVN_MAX);
            gcds_strlcpy(env[i].val, eq + 1, GCDSP_ENVV_MAX);
            if (lio_put_line(c, "OK") != LIO_OK)
                return;

        } else if (strcmp(line, "RUN") == 0) {
            if (arg == NULL) {
                if (lio_put_line(c, "ERR 400 missing command")
                    != LIO_OK)
                    return;
                continue;
            }
            /* a new job invalidates the stored async result
               (PLAN_01: RESULT is kept until the next RUN/RUNA) */
            res->valid = 0;
#ifdef GCDS_HAS_LIVE
            /* supervised: streams O/E while running, serves
               control sessions on ls (PLAN_02 5.2) */
            code = live_exec(LIVE_RUN, c, ls, arg, env, nenv,
                             NULL, NULL, 0);
            if (code == -2)
                return;
            if (code < 0) {
                if (lio_put_line(c, "ERR 500 exec failed") != LIO_OK)
                    return;
                continue;
            }
            if (send_exit(c, code) != LIO_OK)
                return;
#else
            code = run_command(arg, env, nenv, g_outf, g_errf);
            if (code < 0) {
                unlink(g_outf);
                unlink(g_errf);
                if (lio_put_line(c, "ERR 500 exec failed") != LIO_OK)
                    return;
                continue;
            }
            {
                long sent = 0;
                r = send_file(c, 'O', g_outf, &sent);
                if (r == 0)
                    r = send_file(c, 'E', g_errf, &sent);
            }
            unlink(g_outf);
            unlink(g_errf);
            if (r < 0)
                return;
            if (send_exit(c, code) != LIO_OK)
                return;
#endif

        } else if (strcmp(line, "RUNA") == 0) {
            char rl[48];
            long i;

            if (arg == NULL) {
                if (lio_put_line(c, "ERR 400 missing command")
                    != LIO_OK)
                    return;
                continue;
            }
            res->valid = 0;
            job->pending = 1;
            job->jobid = next_jobid();
            gcds_strlcpy(job->cmd, arg, GCDSP_LINE_MAX);
            for (i = 0; i < nenv; i++)
                job->env[i] = env[i];
            job->nenv = nenv;
            sprintf(rl, "OK %ld", job->jobid);
            lio_put_line(c, rl);
            return;     /* owner closes, then executes the job */

        } else if (strcmp(line, "RESULT") == 0) {
            long id;

            id = (arg == NULL) ? -1 : atol(arg);
            if (!res->valid || res->jobid != id) {
                if (lio_put_line(c, "ERR 404 no such result")
                    != LIO_OK)
                    return;
                continue;
            }
            {
                long sent = 0;
                r = send_file(c, 'O', g_rof, &sent);
                if (r == 0)
                    r = send_file(c, 'E', g_ref, &sent);
            }
            if (r < 0)
                return;
            if (send_exit(c, res->code) != LIO_OK)
                return;

        } else if (strcmp(line, "RUNI") == 0) {
#ifdef GCDS_HAS_IX
            if (!g_ix) {
                if (lio_put_line(c, "ERR 501 not supported")
                    != LIO_OK)
                    return;
                continue;
            }
            if (arg == NULL) {
                if (lio_put_line(c, "ERR 400 missing command")
                    != LIO_OK)
                    return;
                continue;
            }
            res->valid = 0;
            code = live_exec(LIVE_RUNI, c, ls, arg, env, nenv,
                             NULL, NULL, 0);
            if (code == -2)
                return;
            if (code < 0) {
                if (lio_put_line(c, "ERR 500 exec failed") != LIO_OK)
                    return;
                continue;
            }
            if (send_exit(c, code) != LIO_OK)
                return;
#else
            if (lio_put_line(c, "ERR 501 not supported") != LIO_OK)
                return;
#endif

        } else if (strcmp(line, "PUT") == 0) {
            if (do_put(c, arg) < 0)
                return;

        } else if (strcmp(line, "GET") == 0) {
            if (do_get(c, arg) < 0)
                return;

        } else if (strcmp(line, "QUIT") == 0) {
            lio_put_line(c, "OK");
            return;

        } else {
            if (lio_put_line(c, "ERR 500 unknown command") != LIO_OK)
                return;
        }
    }
}
