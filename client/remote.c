/*
 * remote.c - connect/greet/auth/stream. Timeouts use alarm() +
 * interruptible blocking calls; select() is deliberately avoided
 * even here to keep one I/O style across the codebase.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "gcdsp.h"
#include "util.h"
#include "lineio.h"
#include "remote.h"

#define GREET_TMO 10    /* seconds for connect + greeting + replies */

static void on_alrm(int sig)
{
    (void)sig;
}

static void arm(int sec)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    /* no SA_RESTART: reads must return EINTR */
    sigaction(SIGALRM, &sa, NULL);
    alarm((unsigned)sec);
}

static void disarm(void)
{
    alarm(0);
}

void rc_key(char *key, long size, const char *alias, const char *what)
{
    key[0] = '\0';
    gcds_strlcat(key, "host.", size);
    gcds_strlcat(key, alias, size);
    gcds_strlcat(key, ".", size);
    gcds_strlcat(key, what, size);
}

/* parse "GCDS <ver> <ostag> <host> [caps...]" */
static int parse_greet(gcds_rc_t *rc, char *line)
{
    char *tok;

    rc->cap_async = 0;
    rc->cap_ix = 0;
    rc->cap_live = 0;
    rc->ostag[0] = '\0';
    rc->rhost[0] = '\0';

    tok = strtok(line, " ");
    if (tok == NULL || strcmp(tok, "GCDS") != 0)
        return -1;
    tok = strtok(NULL, " ");
    if (tok == NULL || atol(tok) < 1)
        return -1;
    tok = strtok(NULL, " ");
    if (tok == NULL)
        return -1;
    gcds_strlcpy(rc->ostag, tok, (long)sizeof(rc->ostag));
    tok = strtok(NULL, " ");
    if (tok == NULL)
        return -1;
    gcds_strlcpy(rc->rhost, tok, (long)sizeof(rc->rhost));
    for (;;) {
        tok = strtok(NULL, " ");
        if (tok == NULL)
            break;
        if (strcmp(tok, "ASYNC") == 0)
            rc->cap_async = 1;
        else if (strcmp(tok, "INTERACTIVE") == 0)
            rc->cap_ix = 1;
        else if (strcmp(tok, "LIVE") == 0)
            rc->cap_live = 1;
        /* unknown caps are ignored by spec */
    }
    return 0;
}

int rc_open(gcds_rc_t *rc, const gcds_kv_t *kv, long nkv,
            const char *alias)
{
    char key[CONF_KEY_MAX];
    const char *serial;
    const char *addr;
    static char line[GCDSP_LINE_MAX];
    int r;

    rc->open = 0;

    rc_key(key, CONF_KEY_MAX, alias, "serial");
    serial = conf_get(kv, nkv, key);

    arm(GREET_TMO);
    if (serial != NULL && serial[0] != '\0') {
        static char dev[CONF_VAL_MAX];
        char *colon;
        long baud;
        gcds_ser_t f;

        gcds_strlcpy(dev, serial, (long)sizeof(dev));
        colon = strrchr(dev, ':');
        baud = 9600;
        if (colon != NULL) {
            *colon = '\0';
            baud = atol(colon + 1);
        }
        f = ser_open(dev, baud);
        if (f == GCDS_BADSER) {
            disarm();
            return -1;
        }
        chan_ser(&rc->ch, f);
        /* clear a possible stale session, then attention
           (PLAN_01 1.1) */
        if (lio_put_line(&rc->ch, "QUIT") != LIO_OK ||
            lio_put_line(&rc->ch, "HELLO") != LIO_OK) {
            chan_close(&rc->ch);
            disarm();
            return -1;
        }
    } else {
        long port;
        gcds_sock_t s;

        rc_key(key, CONF_KEY_MAX, alias, "addr");
        addr = conf_get(kv, nkv, key);
        if (addr == NULL) {
            fprintf(stderr, "gcds: no addr/serial for host '%s'\n",
                    alias);
            disarm();
            return -1;
        }
        rc_key(key, CONF_KEY_MAX, alias, "port");
        port = atol(conf_gets(kv, nkv, key, "9910"));
        s = net_connect(addr, (unsigned short)port);
        if (s == GCDS_BADSOCK) {
            disarm();
            return -1;
        }
        chan_tcp(&rc->ch, s);
    }

    /* greeting; on serial, discard non-GCDS lines (resync +
       replies to the stale-session QUIT above) */
    for (;;) {
        r = lio_get_line(&rc->ch, line, GCDSP_LINE_MAX);
        if (r != LIO_OK) {
            if (r == LIO_ELONG && rc->ch.kind == GCDS_CHAN_SER)
                continue;
            chan_close(&rc->ch);
            disarm();
            return -1;
        }
        if (gcds_starts(line, "GCDS "))
            break;
        if (rc->ch.kind == GCDS_CHAN_TCP) {
            chan_close(&rc->ch);
            disarm();
            return -1;
        }
    }
    disarm();

    if (parse_greet(rc, line) < 0) {
        chan_close(&rc->ch);
        return -1;
    }
    rc->open = 1;
    return 0;
}

int rc_auth(gcds_rc_t *rc, const gcds_kv_t *kv, long nkv,
            const char *alias)
{
    char key[CONF_KEY_MAX];
    const char *token;
    static char line[GCDSP_LINE_MAX];

    rc_key(key, CONF_KEY_MAX, alias, "token");
    token = conf_get(kv, nkv, key);
    if (token == NULL) {
        fprintf(stderr, "gcds: no token for host '%s'\n", alias);
        return -1;
    }
    line[0] = '\0';
    gcds_strlcat(line, "AUTH ", GCDSP_LINE_MAX);
    gcds_strlcat(line, token, GCDSP_LINE_MAX);
    if (rc_cmd(rc, line, line, GCDSP_LINE_MAX) < 0)
        return -1;
    if (strcmp(line, "OK") != 0) {
        fprintf(stderr, "gcds: %s: %s\n", alias, line);
        return -1;
    }
    return 0;
}

int rc_cmd(gcds_rc_t *rc, const char *line, char *reply, long size)
{
    int r;

    if (lio_put_line(&rc->ch, line) != LIO_OK)
        return -1;
    arm(GREET_TMO);
    r = lio_get_line(&rc->ch, reply, size);
    disarm();
    return (r == LIO_OK) ? 0 : -1;
}

/* read one reply/frame-header line with no timeout: file transfers
   (PUT/GET) can take arbitrarily long, like a compile (PLAN_01 4). */
int rc_get_reply(gcds_rc_t *rc, char *reply, long size)
{
    return (lio_get_line(&rc->ch, reply, size) == LIO_OK) ? 0 : -1;
}

static int write_fd(int fd, const char *buf, long n)
{
    long done;
    long r;

    done = 0;
    while (done < n) {
        r = (long)write(fd, buf + done, (size_t)(n - done));
        if (r <= 0)
            return -1;
        done += r;
    }
    return 0;
}

/* ---- reverse path mapping of output (PLAN_05 section 4) ---- */

/* pathmap.c */
int pm_back_line(const gcds_kv_t *kv, long nkv, const char *alias,
                 char *line, long size);

static const gcds_kv_t *g_mbkv = NULL;
static long g_mbn = 0;
static const char *g_mbalias = NULL;
static int g_mb = 0;

#define MB_LINE (GCDSP_LINE_MAX * 2)
static char g_acc[2][MB_LINE];      /* [0]=stdout, [1]=stderr */
static long g_accn[2];

void rc_mapback(const gcds_kv_t *kv, long nkv, const char *alias)
{
    g_mbkv = kv;
    g_mbn = nkv;
    g_mbalias = alias;
    g_mb = 1;
    g_accn[0] = 0;
    g_accn[1] = 0;
}

static int mb_emit_line(int fd, char *acc, long n, int newline)
{
    acc[n] = '\0';
    pm_back_line(g_mbkv, g_mbn, g_mbalias, acc, MB_LINE);
    if (write_fd(fd, acc, (long)strlen(acc)) < 0)
        return -1;
    if (newline && write_fd(fd, "\n", 1) < 0)
        return -1;
    return 0;
}

/* accumulate payload; filter and flush on each complete line.
   over-long lines are flushed raw (no filtering). */
static int mb_feed(int fd, const char *buf, long n)
{
    char *acc;
    long *accn;
    long j;

    acc = g_acc[fd - 1];
    accn = &g_accn[fd - 1];
    for (j = 0; j < n; j++) {
        if (buf[j] == '\n') {
            if (mb_emit_line(fd, acc, *accn, 1) < 0)
                return -1;
            *accn = 0;
        } else if (*accn >= MB_LINE - 1) {
            if (write_fd(fd, acc, *accn) < 0)
                return -1;
            *accn = 0;
            acc[(*accn)++] = buf[j];
        } else {
            acc[(*accn)++] = buf[j];
        }
    }
    return 0;
}

static void mb_flush(void)
{
    if (!g_mb)
        return;
    if (g_accn[0] > 0)
        mb_emit_line(1, g_acc[0], g_accn[0], 0);
    if (g_accn[1] > 0)
        mb_emit_line(2, g_acc[1], g_accn[1], 0);
    g_accn[0] = 0;
    g_accn[1] = 0;
}

/* process one server line during O/E/X streaming.
   returns 0 to continue, nonzero when finished (*code set:
   remote exit code, or -1 on error). shared with ixmode.c. */
int rc_out_frame(gcds_rc_t *rc, const char *line, long *code)
{
    static char buf[GCDSP_FRAME_MAX];
    long n;
    int fd;

    if ((line[0] == 'O' || line[0] == 'E') && line[1] == ' ') {
        n = atol(line + 2);
        if (n < 1 || n > GCDSP_FRAME_MAX) {
            *code = -1;
            return -1;
        }
        if (chan_read_n(&rc->ch, buf, n) < 0) {
            *code = -1;
            return -1;
        }
        fd = (line[0] == 'O') ? 1 : 2;
        if (g_mb ? (mb_feed(fd, buf, n) < 0)
                 : (write_fd(fd, buf, n) < 0)) {
            *code = -1;
            return -1;
        }
        return 0;
    }
    if (line[0] == 'X' && line[1] == ' ') {
        mb_flush();
        *code = atol(line + 2) & 255;
        return 1;
    }
    mb_flush();
    if (gcds_starts(line, "ERR"))
        fprintf(stderr, "gcds: remote: %s\n", line);
    *code = -1;
    return -1;
}

long rc_stream(gcds_rc_t *rc)
{
    static char line[GCDSP_LINE_MAX];
    long code;

    code = -1;
    for (;;) {
        /* no timeout here: X may take as long as the compile
           takes (client-silence rule, PLAN_01 section 4) */
        if (lio_get_line(&rc->ch, line, GCDSP_LINE_MAX) != LIO_OK)
            return -1;
        if (rc_out_frame(rc, line, &code) != 0)
            return code;
    }
}

void rc_close(gcds_rc_t *rc)
{
    if (rc->open) {
        chan_close(&rc->ch);
        rc->open = 0;
    }
}
