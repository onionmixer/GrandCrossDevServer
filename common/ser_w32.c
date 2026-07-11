/*
 * ser_w32.c - Win32 COM port backend.
 * Fully blocking (all COMMTIMEOUTS zero): ReadFile returns only
 * when the requested count arrived, which matches chan_read_n's
 * exact-read usage; lineio reads one byte at a time.
 */
#ifdef GCDS_WIN32

#include <windows.h>
#include <string.h>

#include "util.h"
#include "ser.h"

gcds_ser_t ser_open(const char *dev, long baud)
{
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;
    char path[80];

    /* "COM1" -> "\\.\COM1" (required for COM10+, harmless below) */
    if (dev[0] != '\\') {
        path[0] = '\0';
        gcds_strlcat(path, "\\\\.\\", (long)sizeof(path));
        gcds_strlcat(path, dev, (long)sizeof(path));
    } else {
        gcds_strlcpy(path, dev, (long)sizeof(path));
    }

    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GCDS_BADSER;

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return GCDS_BADSER;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutX = FALSE;          /* no XON/XOFF (PLAN_01 1.1) */
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fOutxCtsFlow = FALSE;   /* RTS/CTS optional; off for the
                                   3-wire null-modem baseline */
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return GCDS_BADSER;
    }

    memset(&to, 0, sizeof(to));     /* all zero = fully blocking */
    SetCommTimeouts(h, &to);

    return (gcds_ser_t)h;
}

long ser_read(gcds_ser_t f, char *buf, long n)
{
    DWORD got;

    if (!ReadFile((HANDLE)f, buf, (DWORD)n, &got, NULL))
        return -1;
    if (got == 0)
        return -1;
    return (long)got;
}

long ser_write(gcds_ser_t f, const char *buf, long n)
{
    DWORD put;

    if (!WriteFile((HANDLE)f, buf, (DWORD)n, &put, NULL))
        return -1;
    return (long)put;
}

void ser_close(gcds_ser_t f)
{
    CloseHandle((HANDLE)f);
}

#else

/* keep non-Win32 builds that list this file happy */
typedef int gcds_ser_w32_unused;

#endif /* GCDS_WIN32 */
