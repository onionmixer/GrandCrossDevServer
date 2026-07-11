/*
 * ser_next.c - NeXTSTEP/OPENSTEP sgtty serial backend.
 * NeXTSTEP ships <termios.h> but the functions (tcsetattr,
 * cfsetispeed, ...) are missing from libc - the kernel speaks
 * old-BSD sgtty ioctls, so this backend uses them directly
 * (doc/next.md). Raw 8N1, no flow control (PLAN_01 1.1: XON/XOFF
 * would corrupt binary frames; sgtty's TANDEM flag stays off).
 *
 * Devices: /dev/ttya, /dev/ttyb (NeXT on-board serial).
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sgtty.h>
#include <fcntl.h>
#include <unistd.h>         /* present on OPENSTEP (doc/next.md) */

#include "ser.h"

/* sgtty speed codes. Which header defines them varies (sgtty.h /
   sys/ttydev.h / sys/ioctl.h), but the VALUES have been fixed
   since 4.2BSD - fall back to those if undeclared. 19200/38400
   are the EXTA/EXTB "external" clocks on old BSD. */
#ifndef B1200
#define B1200 9
#endif
#ifndef B2400
#define B2400 11
#endif
#ifndef B4800
#define B4800 12
#endif
#ifndef B9600
#define B9600 13
#endif
#ifndef B19200
#ifdef EXTA
#define B19200 EXTA
#else
#define B19200 14
#endif
#endif
#ifndef B38400
#ifdef EXTB
#define B38400 EXTB
#else
#define B38400 15
#endif
#endif

static long baud_const(long baud)
{
    switch (baud) {
    case 1200:   return (long)B1200;
    case 2400:   return (long)B2400;
    case 4800:   return (long)B4800;
    case 9600:   return (long)B9600;
#ifdef B19200
    case 19200:  return (long)B19200;
#endif
#ifdef B38400
    case 38400:  return (long)B38400;
#endif
    default:     return -1;
    }
}

gcds_ser_t ser_open(const char *dev, long baud)
{
    gcds_ser_t f;
    struct sgttyb sg;
    long bc;

    bc = baud_const(baud);
    if (bc < 0)
        return GCDS_BADSER;

    f = open(dev, O_RDWR);
    if (f < 0)
        return GCDS_BADSER;

    if (ioctl(f, TIOCGETP, (char *)&sg) < 0) {
        close(f);
        return GCDS_BADSER;
    }

    /* RAW: 8-bit pass-through, no echo, no CR/LF translation,
       no signals. Not CBREAK (that keeps output processing).
       TANDEM (XON/XOFF) intentionally left clear. */
    sg.sg_ispeed = (char)bc;
    sg.sg_ospeed = (char)bc;
    sg.sg_flags = RAW;

    if (ioctl(f, TIOCSETP, (char *)&sg) < 0) {
        close(f);
        return GCDS_BADSER;
    }
    return f;
}

long ser_read(gcds_ser_t f, char *buf, long n)
{
    return (long)read(f, buf, (int)n);
}

long ser_write(gcds_ser_t f, const char *buf, long n)
{
    return (long)write(f, buf, (int)n);
}

void ser_close(gcds_ser_t f)
{
    close(f);
}
