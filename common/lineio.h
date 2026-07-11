/*
 * lineio.h - protocol line and frame I/O on top of a channel.
 * Lines are LF-terminated; CR bytes are ignored on receive
 * (PLAN_01 section 1).
 */
#ifndef LINEIO_H
#define LINEIO_H

#include "chan.h"

#define LIO_OK       0
#define LIO_EIO     (-1)    /* channel error or EOF */
#define LIO_ELONG   (-2)    /* line exceeded size (protocol violation) */

/* read one line into buf (size incl. NUL); LF stripped, CR ignored */
int lio_get_line(gcds_chan_t *c, char *buf, long size);

/* write line + LF */
int lio_put_line(gcds_chan_t *c, const char *line);

/* write one frame: "<tag> <len>\n" + len raw bytes.
   len must be 1..GCDSP_FRAME_MAX; caller chunks (chan_chunk). */
int lio_put_frame(gcds_chan_t *c, char tag, const char *data, long len);

#endif /* LINEIO_H */
