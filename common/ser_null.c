/*
 * ser_null.c - serial backend stub, for platforms whose TCP daemon
 * doesn't need serial and whose libc lacks POSIX termios (e.g.
 * NeXTSTEP/OPENSTEP, which has <termios.h> but not tcsetattr()).
 *
 * The Makefile links this INSTEAD of ser_psx.c. Serial mode then
 * fails cleanly (ser_open returns GCDS_BADSER); TCP mode is
 * unaffected. A real NeXTSTEP serial backend would use 4.3BSD
 * sgtty ioctls (future work).
 */
#include "ser.h"

gcds_ser_t ser_open(const char *dev, long baud)
{
    (void)dev;
    (void)baud;
    return GCDS_BADSER;
}

long ser_read(gcds_ser_t f, char *buf, long n)
{
    (void)f;
    (void)buf;
    (void)n;
    return -1;
}

long ser_write(gcds_ser_t f, const char *buf, long n)
{
    (void)f;
    (void)buf;
    (void)n;
    return -1;
}

void ser_close(gcds_ser_t f)
{
    (void)f;
}
