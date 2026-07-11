/*
 * ser_dos.c - MS-DOS serial backend (PLAN_02 3.1).
 *
 * FOSSIL driver (X00/BNU/ADF, int 14h extensions) when present -
 * interrupt-driven buffering, no byte loss on real hardware.
 * Falls back to plain BIOS int 14h polling when no FOSSIL is
 * installed: fine under emulators (DOSBox-X) and for lockstep
 * protocols at 9600, lossy on real iron - FOSSIL is the
 * documented requirement for real machines.
 *
 * Both APIs share the int 14h register layout used here:
 *   AH=00 init (AL = 0xE3: 9600 8N1), AH=01 tx AL,
 *   AH=02 rx -> AL, AH=03 status -> AH bit0 = data ready,
 *   FOSSIL only: AH=04 init driver (-> AX 0x1954), AH=05 deinit.
 */
#ifdef GCDS_DOS

#include <string.h>
#include <i86.h>

#include "ser.h"

#define FOSSIL_SIG 0x1954

static int g_fossil = 0;    /* deinit on close if we inited it */

static unsigned s_int14(unsigned ax, unsigned port)
{
    union REGS r;

    r.w.ax = ax;
    r.w.dx = port;
    int86(0x14, &r, &r);
    return r.w.ax;
}

/* DOS idle hint (INT 28h). A tight int14-status poll otherwise
   starves the host emulator / TSR serial driver of the chance to
   deliver freshly arrived bytes to the UART; issuing the idle
   interrupt each spin yields to it. Harmless on real hardware. */
static void s_idle(void)
{
    union REGS r;

    int86(0x28, &r, &r);
}

gcds_ser_t ser_open(const char *dev, long baud)
{
    unsigned port;
    unsigned ax;

    /* "COM1".."COM4" -> 0..3 */
    if ((dev[0] != 'C' && dev[0] != 'c') ||
        (dev[1] != 'O' && dev[1] != 'o') ||
        (dev[2] != 'M' && dev[2] != 'm') ||
        dev[3] < '1' || dev[3] > '4' || dev[4] != '\0')
        return GCDS_BADSER;
    port = (unsigned)(dev[3] - '1');

    if (baud != 9600)
        return GCDS_BADSER;     /* v1: fixed 9600 8N1 */

    /* try FOSSIL first */
    ax = s_int14(0x0400, port);
    if (ax == FOSSIL_SIG)
        g_fossil = 1;

    /* init 9600 8N1 (same AL encoding for FOSSIL and BIOS) */
    s_int14(0x00E3, port);

    return (gcds_ser_t)port;
}

long ser_read(gcds_ser_t f, char *buf, long n)
{
    unsigned port;
    unsigned ax;
    long got;

    port = (unsigned)f;
    got = 0;
    while (got < n) {
        /* poll line status until data ready (AH bit0), yielding to
           the host/TSR each spin so incoming bytes get delivered */
        for (;;) {
            ax = s_int14(0x0300, port);
            if ((ax & 0x0100) != 0)
                break;
            s_idle();
        }
        ax = s_int14(0x0200, port);
        buf[got] = (char)(ax & 0xFF);
        got++;
        /* return early if nothing more is pending: caller loops */
        if (got < n) {
            ax = s_int14(0x0300, port);
            if ((ax & 0x0100) == 0)
                return got;
        }
    }
    return got;
}

long ser_write(gcds_ser_t f, const char *buf, long n)
{
    unsigned port;
    long i;

    port = (unsigned)f;
    for (i = 0; i < n; i++)
        s_int14(0x0100 | (unsigned char)buf[i], port);
    return n;
}

void ser_close(gcds_ser_t f)
{
    if (g_fossil) {
        s_int14(0x0500, (unsigned)f);
        g_fossil = 0;
    }
}

#else

typedef int gcds_ser_dos_unused;

#endif /* GCDS_DOS */
