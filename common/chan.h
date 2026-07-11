/*
 * chan.h - transport channel abstraction (PLAN_00 D10).
 * Everything above this layer (lineio, sessions) sees only a
 * reliable bidirectional byte stream; TCP vs serial is invisible.
 */
#ifndef CHAN_H
#define CHAN_H

#include "net.h"
#include "ser.h"

#define GCDS_CHAN_TCP 0
#define GCDS_CHAN_SER 1

typedef struct gcds_chan {
    int kind;
    gcds_sock_t s;       /* valid when kind == GCDS_CHAN_TCP */
    gcds_ser_t ser;      /* valid when kind == GCDS_CHAN_SER */
} gcds_chan_t;

void chan_tcp(gcds_chan_t *c, gcds_sock_t s);
void chan_ser(gcds_chan_t *c, gcds_ser_t f);

/* read exactly n bytes / write all n bytes: 0 ok, -1 error or EOF */
int chan_read_n(gcds_chan_t *c, char *buf, long n);
int chan_write_all(gcds_chan_t *c, const char *buf, long n);

/* preferred max frame payload for this channel (PLAN_01 1.1) */
long chan_chunk(gcds_chan_t *c);

void chan_close(gcds_chan_t *c);

#endif /* CHAN_H */
