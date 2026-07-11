/*
 * lineio.c - one-byte line reads by design: lines are short and
 * this can never over-read into a following binary frame, which
 * keeps the receiver state machine trivial on every platform.
 */
#include <stdio.h>
#include <string.h>

#include "gcdsp.h"
#include "lineio.h"

int lio_get_line(gcds_chan_t *c, char *buf, long size)
{
    long n;
    char ch;

    n = 0;
    for (;;) {
        if (chan_read_n(c, &ch, 1) < 0)
            return LIO_EIO;
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            buf[n] = '\0';
            return LIO_OK;
        }
        if (n >= size - 1)
            return LIO_ELONG;
        buf[n] = ch;
        n++;
    }
}

int lio_put_line(gcds_chan_t *c, const char *line)
{
    if (chan_write_all(c, line, (long)strlen(line)) < 0)
        return LIO_EIO;
    if (chan_write_all(c, "\n", 1) < 0)
        return LIO_EIO;
    return LIO_OK;
}

int lio_put_frame(gcds_chan_t *c, char tag, const char *data, long len)
{
    char hdr[32];

    if (len < 1 || len > GCDSP_FRAME_MAX)
        return LIO_EIO;
    sprintf(hdr, "%c %ld\n", tag, len);   /* bounded: tag + <=10 digits */
    if (chan_write_all(c, hdr, (long)strlen(hdr)) < 0)
        return LIO_EIO;
    if (chan_write_all(c, data, len) < 0)
        return LIO_EIO;
    return LIO_OK;
}
