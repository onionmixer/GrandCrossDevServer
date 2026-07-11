/*
 * asyncjob.c - poll-and-fetch side of the async job model
 * (PLAN_01 section 7). The submit half lives in main.c because it
 * reuses the already-open session.
 *
 * Connect failures and timeouts during polling are not errors:
 * they mean "still running" on a single-tasking remote.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "gcdsp.h"
#include "util.h"
#include "lineio.h"
#include "remote.h"

#define POLL_SEC 3

long poll_result(const gcds_kv_t *kv, long nkv, const char *alias,
                 long jobid)
{
    gcds_rc_t rc;
    static char line[GCDSP_LINE_MAX];
    char want[48];
    long tries;
    long code;

    sprintf(want, "OK result %ld", jobid);
    tries = 0;

    for (;;) {
        if (tries > 0)
            sleep(POLL_SEC);
        tries++;
        if (tries % 10 == 0)
            fprintf(stderr, "gcds: job %ld still running on %s "
                    "(%ld polls)\n", jobid, alias, tries);

        if (rc_open(&rc, kv, nkv, alias) < 0)
            continue;           /* busy or down: keep polling */
        if (rc_auth(&rc, kv, nkv, alias) < 0) {
            rc_close(&rc);
            return -1;          /* auth failure is fatal */
        }
        if (rc_cmd(&rc, "STAT", line, GCDSP_LINE_MAX) < 0) {
            rc_close(&rc);
            continue;
        }

        if (strcmp(line, want) == 0) {
            sprintf(want, "RESULT %ld", jobid);
            if (lio_put_line(&rc.ch, want) != LIO_OK) {
                rc_close(&rc);
                continue;
            }
            code = rc_stream(&rc);
            rc_close(&rc);
            if (code < 0)
                continue;       /* fetch broke: RESULT is idempotent */
            return code;
        }
        if (gcds_starts(line, "OK busy")) {
            rc_close(&rc);
            continue;           /* LIVE daemon, job in progress */
        }
        rc_close(&rc);
        if (strcmp(line, "OK idle") == 0) {
            fprintf(stderr, "gcds: job %ld: result unavailable "
                    "(exec failed or daemon restarted)\n", jobid);
            return -1;
        }
        fprintf(stderr, "gcds: job %ld: unexpected status: %s\n",
                jobid, line);
        return -1;
    }
}
