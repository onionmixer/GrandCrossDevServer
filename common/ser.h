/*
 * ser.h - serial channel backend (PLAN_02 section 3.1).
 * Blocking read/write only. 8N1, no software flow control ever
 * (XON/XOFF conflicts with binary frames - PLAN_01 section 1.1).
 */
#ifndef SER_H
#define SER_H

#ifdef GCDS_WIN32
typedef void *gcds_ser_t;            /* HANDLE; 0 = invalid (ser_w32
                                       maps INVALID_HANDLE_VALUE) */
#define GCDS_BADSER ((gcds_ser_t)0)
#else
typedef int gcds_ser_t;
#define GCDS_BADSER (-1)
#endif

/* dev: "/dev/ttyUSB0" (POSIX) / "COM1" (Win32/DOS, later phases) */
gcds_ser_t ser_open(const char *dev, long baud);
long ser_read(gcds_ser_t f, char *buf, long n);   /* may be partial */
long ser_write(gcds_ser_t f, const char *buf, long n);
void ser_close(gcds_ser_t f);

#endif /* SER_H */
