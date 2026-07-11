/*
 * ser_psx.c - POSIX termios serial backend.
 * (NeXTSTEP sgtty variant is a Phase 3 work item: PLAN_02 3.1)
 */
#include <sys/types.h>      /* size_t (NeXTSTEP: not via termios.h) */
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "ser.h"

#ifndef O_NOCTTY            /* absent on 4.3BSD/NeXTSTEP; flag is
                              only a "don't grab controlling tty"
                              hint, safe to no-op */
#define O_NOCTTY 0
#endif

static long baud_const(long baud)
{
    switch (baud) {
    case 1200:   return (long)B1200;
    case 2400:   return (long)B2400;
    case 4800:   return (long)B4800;
    case 9600:   return (long)B9600;
    case 19200:  return (long)B19200;
    case 38400:  return (long)B38400;
    default:     return -1;
    }
}

gcds_ser_t ser_open(const char *dev, long baud)
{
    gcds_ser_t f;
    struct termios tio;
    long bc;

    bc = baud_const(baud);
    if (bc < 0)
        return GCDS_BADSER;

    f = open(dev, O_RDWR | O_NOCTTY);
    if (f < 0)
        return GCDS_BADSER;

    if (tcgetattr(f, &tio) < 0) {
        close(f);
        return GCDS_BADSER;
    }

    /* raw 8N1: no XON/XOFF, no CR/LF translation, no echo */
    tio.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR | ISTRIP);
    tio.c_iflag |= IGNBRK;
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_cc[VMIN] = 1;     /* blocking: at least one byte */
    tio.c_cc[VTIME] = 0;

    cfsetispeed(&tio, (speed_t)bc);
    cfsetospeed(&tio, (speed_t)bc);

    if (tcsetattr(f, TCSANOW, &tio) < 0) {
        close(f);
        return GCDS_BADSER;
    }
    return f;
}

long ser_read(gcds_ser_t f, char *buf, long n)
{
    return (long)read(f, buf, (size_t)n);
}

long ser_write(gcds_ser_t f, const char *buf, long n)
{
    return (long)write(f, buf, (size_t)n);
}

void ser_close(gcds_ser_t f)
{
    close(f);
}
