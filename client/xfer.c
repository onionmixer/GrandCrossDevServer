/*
 * xfer.c - client side of PUT/GET file transfer (PLAN_01).
 * Streams D-frames over an authenticated session. For remote
 * machines without shared storage (DOS etc): upload source,
 * RUN the compiler, download the artifact.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcdsp.h"
#include "util.h"
#include "lineio.h"
#include "remote.h"

/* upload local file `lpath` to remote `rpath` over an open session.
   returns 0 ok, -1 error (message printed). */
int xfer_put(gcds_rc_t *rc, const char *lpath, const char *rpath)
{
    static char buf[GCDSP_FRAME_MAX];
    static char line[GCDSP_LINE_MAX];
    static char hdr[32];
    FILE *fp;
    long chunk;
    long n;

    fp = fopen(lpath, "rb");
    if (fp == NULL) {
        fprintf(stderr, "gcds: cannot open %s\n", lpath);
        return -1;
    }
    line[0] = '\0';
    gcds_strlcat(line, "PUT ", GCDSP_LINE_MAX);
    gcds_strlcat(line, rpath, GCDSP_LINE_MAX);
    if (rc_cmd(rc, line, line, GCDSP_LINE_MAX) < 0) {
        fclose(fp);
        return -1;
    }
    if (strcmp(line, "OK") != 0) {
        fprintf(stderr, "gcds: remote: %s\n", line);
        fclose(fp);
        return -1;
    }

    chunk = chan_chunk(&rc->ch);
    if (chunk > GCDSP_FRAME_MAX)
        chunk = GCDSP_FRAME_MAX;
    for (;;) {
        n = (long)fread(buf, 1, (size_t)chunk, fp);
        if (n <= 0)
            break;
        sprintf(hdr, "D %ld\n", n);
        if (chan_write_all(&rc->ch, hdr, (long)strlen(hdr)) < 0 ||
            chan_write_all(&rc->ch, buf, n) < 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    if (lio_put_line(&rc->ch, "D 0") != LIO_OK)
        return -1;

    /* final status: "OK <total>" or ERR */
    if (rc_get_reply(rc, line, GCDSP_LINE_MAX) < 0)
        return -1;
    if (!gcds_starts(line, "OK")) {
        fprintf(stderr, "gcds: remote: %s\n", line);
        return -1;
    }
    return 0;
}

/* download remote `rpath` into local `lpath`. 0 ok, -1 error. */
int xfer_get(gcds_rc_t *rc, const char *rpath, const char *lpath)
{
    static char buf[GCDSP_FRAME_MAX];
    static char line[GCDSP_LINE_MAX];
    FILE *fp;
    long n;

    line[0] = '\0';
    gcds_strlcat(line, "GET ", GCDSP_LINE_MAX);
    gcds_strlcat(line, rpath, GCDSP_LINE_MAX);
    if (rc_cmd(rc, line, line, GCDSP_LINE_MAX) < 0)
        return -1;
    if (strcmp(line, "OK") != 0) {
        fprintf(stderr, "gcds: remote: %s\n", line);
        return -1;
    }

    fp = fopen(lpath, "wb");
    if (fp == NULL) {
        fprintf(stderr, "gcds: cannot create %s\n", lpath);
        /* still drain the stream to keep the session sane */
        for (;;) {
            if (rc_get_reply(rc, line, GCDSP_LINE_MAX) < 0)
                return -1;
            if (line[0] == 'D' && line[1] == ' ') {
                n = atol(line + 2);
                if (n == 0)
                    break;
                if (n < 0 || n > GCDSP_FRAME_MAX ||
                    chan_read_n(&rc->ch, buf, n) < 0)
                    return -1;
            } else {
                break;
            }
        }
        return -1;
    }

    for (;;) {
        if (rc_get_reply(rc, line, GCDSP_LINE_MAX) < 0) {
            fclose(fp);
            return -1;
        }
        if (line[0] == 'D' && line[1] == ' ') {
            n = atol(line + 2);
            if (n == 0)
                break;              /* EOF */
            if (n < 0 || n > GCDSP_FRAME_MAX) {
                fclose(fp);
                return -1;
            }
            if (chan_read_n(&rc->ch, buf, n) < 0) {
                fclose(fp);
                return -1;
            }
            if ((long)fwrite(buf, 1, (size_t)n, fp) != n) {
                fprintf(stderr, "gcds: write to %s failed\n", lpath);
                fclose(fp);
                return -1;
            }
        } else {
            fprintf(stderr, "gcds: remote: %s\n", line);
            fclose(fp);
            return -1;
        }
    }
    if (fclose(fp) != 0) {
        fprintf(stderr, "gcds: close %s failed\n", lpath);
        return -1;
    }
    return 0;
}
