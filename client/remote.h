/*
 * remote.h - client-side session helpers (Linux host only, but
 * kept to the same C89/portability rules as the rest).
 */
#ifndef REMOTE_H
#define REMOTE_H

#include "chan.h"
#include "conf.h"

typedef struct {
    gcds_chan_t ch;
    int open;
    int cap_async;
    int cap_ix;
    int cap_live;
    char ostag[32];
    char rhost[64];
} gcds_rc_t;

/* build "host.<alias>.<what>" */
void rc_key(char *key, long size, const char *alias, const char *what);

/* connect (TCP or serial per config), read greeting, parse caps.
   returns 0 ok, -1 failure (timeout, refused, bad greeting). */
int rc_open(gcds_rc_t *rc, const gcds_kv_t *kv, long nkv,
            const char *alias);

/* AUTH with host token from config; 0 ok, -1 fail */
int rc_auth(gcds_rc_t *rc, const gcds_kv_t *kv, long nkv,
            const char *alias);

/* send one line, read one reply line; 0 ok (reply filled), -1 io */
int rc_cmd(gcds_rc_t *rc, const char *line, char *reply, long size);

/* read one reply line, no timeout (for PUT/GET transfers) */
int rc_get_reply(gcds_rc_t *rc, char *reply, long size);

/* consume O/E/X frames: O->stdout, E->stderr.
   returns exit code 0..255, or -1 on protocol/IO error. */
long rc_stream(gcds_rc_t *rc);

/* process one server line of an O/E/X stream; 0 = continue,
   nonzero = finished with *code set (used by ixmode.c too) */
int rc_out_frame(gcds_rc_t *rc, const char *line, long *code);

/* enable reverse path mapping on streamed output (PLAN_05 4) */
void rc_mapback(const gcds_kv_t *kv, long nkv, const char *alias);

void rc_close(gcds_rc_t *rc);

#endif /* REMOTE_H */
