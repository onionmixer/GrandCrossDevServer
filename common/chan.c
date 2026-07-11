#include "gcdsp.h"
#include "chan.h"

void chan_tcp(gcds_chan_t *c, gcds_sock_t s)
{
    c->kind = GCDS_CHAN_TCP;
    c->s = s;
    c->ser = GCDS_BADSER;
}

void chan_ser(gcds_chan_t *c, gcds_ser_t f)
{
    c->kind = GCDS_CHAN_SER;
    c->s = GCDS_BADSOCK;
    c->ser = f;
}

int chan_read_n(gcds_chan_t *c, char *buf, long n)
{
    long got;
    long r;

    got = 0;
    while (got < n) {
        if (c->kind == GCDS_CHAN_TCP)
            r = net_recv(c->s, buf + got, n - got);
        else
            r = ser_read(c->ser, buf + got, n - got);
        if (r <= 0)
            return -1;
        got += r;
    }
    return 0;
}

int chan_write_all(gcds_chan_t *c, const char *buf, long n)
{
    long sent;
    long r;

    sent = 0;
    while (sent < n) {
        if (c->kind == GCDS_CHAN_TCP)
            r = net_send(c->s, buf + sent, n - sent);
        else
            r = ser_write(c->ser, buf + sent, n - sent);
        if (r <= 0)
            return -1;
        sent += r;
    }
    return 0;
}

long chan_chunk(gcds_chan_t *c)
{
    if (c->kind == GCDS_CHAN_SER)
        return GCDSP_SER_FRAME;
    return 1024L;
}

void chan_close(gcds_chan_t *c)
{
    if (c->kind == GCDS_CHAN_TCP) {
        if (c->s != GCDS_BADSOCK)
            net_close(c->s);
        c->s = GCDS_BADSOCK;
    } else {
        if (c->ser != GCDS_BADSER)
            ser_close(c->ser);
        c->ser = GCDS_BADSER;
    }
}
