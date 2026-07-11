/*
 * session.h - one GCDSP session over an open channel (PLAN_01).
 */
#ifndef SESSION_H
#define SESSION_H

#include "chan.h"
#include "exec.h"

/* async job accepted by RUNA; executed by main after session close */
typedef struct {
    int pending;
    long jobid;
    char cmd[GCDSP_LINE_MAX];
    gcds_env_t env[GCDSP_ENV_MAX];
    long nenv;
} gcds_job_t;

/* stored result of the last completed async job */
typedef struct {
    int valid;
    long jobid;
    int code;
} gcds_res_t;

/* module setup: paths/token/capability flags used by all sessions.
   live_ok: advertise LIVE (TCP mode on a GCDS_HAS_LIVE build)
   ix_ok: advertise INTERACTIVE (GCDS_HAS_IX; on Win32 TCP mode
   only - a COM channel cannot be watched by Winsock select) */
void session_init(const char *tmpdir, const char *token,
                  int adv_async, int live_ok, int ix_ok);

/* optional TCP peer allow-list (PLAN_00 D4) and output cap.
   allow: space/comma IPv4 list, NULL/"" = allow all.
   maxout: max bytes streamed/stored per job, 0 = unlimited. */
void session_set_acl(const char *allow);
void session_set_maxout(long maxout);
int  session_peer_ok(const char *ip);   /* 1 allowed, 0 denied */
long session_maxout(void);              /* current cap, 0=off */

/* result file paths (shared with main.c which runs pending jobs) */
const char *sess_ro_path(void);
const char *sess_re_path(void);

/* remove any leftover capture temp files (shutdown cleanup) */
void sess_cleanup_tmp(void);

/* run one session; never closes the channel (owner does).
   sets job->pending when a RUNA was accepted.
   ls: listen socket for control sessions during execution
   (GCDS_BADSOCK in serial mode / non-LIVE builds). */
void session_run(gcds_chan_t *c, gcds_job_t *job, gcds_res_t *res,
                 gcds_sock_t ls);

/* control session during execution (PLAN_01 4.1): AUTH + PING/
   STAT(busy jobid)/QUIT only. called from live.c. */
void ctl_session(gcds_chan_t *c, long busy_jobid);

#endif /* SESSION_H */
