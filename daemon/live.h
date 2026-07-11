/*
 * live.h - supervised execution loop (PLAN_02 5.1/5.2).
 * POSIX only; compiled when GCDS_HAS_LIVE / GCDS_HAS_IX are set.
 * One loop serves three modes:
 *   LIVE_RUN  - stream O/E to the session while running
 *   LIVE_RUNI - LIVE_RUN + I(stdin)/K(kill) frames from the client
 *   LIVE_JOB  - async job: capture to files, no session
 * In every mode, if `ls` is a valid listen socket, control
 * sessions (PING/STAT) are accepted while the child runs.
 */
#ifndef LIVE_H
#define LIVE_H

#include "chan.h"
#include "exec.h"

#define LIVE_RUN  0
#define LIVE_RUNI 1
#define LIVE_JOB  2

/* returns exit code 0..255; -1 exec failure (nothing streamed);
   -2 session lost or protocol violation (caller ends session).
   outf/errf are used by LIVE_JOB only; jobid feeds STAT busy. */
int live_exec(int mode, gcds_chan_t *sess, gcds_sock_t ls,
              const char *cmd, const gcds_env_t *env, long nenv,
              const char *outf, const char *errf, long jobid);

#endif /* LIVE_H */
