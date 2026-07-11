/*
 * gcdslog.c - serial console capture for kernel-panic / crash messages.
 *
 * Out-of-band companion to gcdsd (PLAN_04 section 3): when a driver takes
 * the kernel down, the userland daemon dies with it, so the dying
 * messages must be read from the target's SERIAL CONSOLE by a separate
 * host-side tool. gcdslog opens a serial device, timestamps each line it
 * receives, and writes to stdout and/or a log file. It speaks no GCDSP -
 * it is a plain logger.
 *
 * This tool is Linux-host-only (the host is always the Linux commander),
 * so - unlike the rest of the project - it is NOT bound by the strict
 * C89 / conservative-socket rules and uses termios freely.
 *
 *   gcdslog [-b baud] [-o logfile] [-a] [-r] <device>
 *
 *   <device>     serial port, e.g. /dev/ttyUSB0 (a USB-serial adapter to
 *                the target's console port), or a PTY for emulator tests.
 *   -b baud      line speed (default 115200 - the usual console rate).
 *   -o logfile   also append timestamped lines here (default stdout only).
 *   -a           show all bytes as they arrive (also timestamp partial
 *                lines on an idle gap) - useful for a hang with no newline.
 *   -r           do NOT auto-reconnect; exit when the port closes.
 *
 * Target-side setup is per-OS (documented in doc/panic-capture.md):
 *   Linux:     boot param  console=ttyS0,115200
 *   Windows:   kernel debug (KD) over serial - gcdslog captures raw bytes,
 *              symbol resolution is WinDbg's job
 *   BeOS/Haiku:serial debug output boot option
 *   NeXTSTEP:  serial console
 *
 * Ctrl-C to stop.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE            /* cfmakeraw, localtime_r (or -D_GNU_SOURCE) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <sys/select.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* map a numeric baud to its termios Bxxxx constant, or -1 */
static speed_t baud_const(long baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    default:     return (speed_t)-1;
    }
}

static int open_serial(const char *dev, speed_t spd)
{
    struct termios t;
    int fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (tcgetattr(fd, &t) == 0) {         /* PTYs/real serial: set raw 8N1 */
        cfmakeraw(&t);
        t.c_cflag |= (CLOCAL | CREAD);
        t.c_cflag &= ~CRTSCTS;            /* no hardware flow control */
        t.c_iflag &= ~(IXON | IXOFF | IXANY);
        cfsetispeed(&t, spd);
        cfsetospeed(&t, spd);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    /* clear O_NONBLOCK now that the port is configured; we use select() */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    return fd;
}

/* current wall-clock stamp "YYYY-MM-DD HH:MM:SS" */
static void stamp(char *buf, size_t n)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void emit(FILE *log, const char *line)
{
    char ts[32];
    stamp(ts, sizeof ts);
    printf("[%s] %s\n", ts, line);
    fflush(stdout);
    if (log) {
        fprintf(log, "[%s] %s\n", ts, line);
        fflush(log);
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: gcdslog [-b baud] [-o logfile] [-a] [-r] <device>\n"
        "  captures a target's serial console (kernel panic, etc.),\n"
        "  timestamping each line. default baud 115200. Ctrl-C stops.\n");
}

int main(int argc, char **argv)
{
    long baud = 115200;
    const char *logpath = NULL, *dev = NULL;
    int show_all = 0, no_reconnect = 0, ai;
    speed_t spd;
    FILE *log = NULL;
    char line[4096];
    size_t ll = 0;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "-b") == 0 && ai + 1 < argc)
            baud = strtol(argv[++ai], NULL, 10);
        else if (strcmp(argv[ai], "-o") == 0 && ai + 1 < argc)
            logpath = argv[++ai];
        else if (strcmp(argv[ai], "-a") == 0)
            show_all = 1;
        else if (strcmp(argv[ai], "-r") == 0)
            no_reconnect = 1;
        else if (argv[ai][0] == '-') { usage(); return 2; }
        else dev = argv[ai];
    }
    if (!dev) { usage(); return 2; }

    spd = baud_const(baud);
    if (spd == (speed_t)-1) {
        fprintf(stderr, "gcdslog: unsupported baud %ld\n", baud);
        return 2;
    }
    if (logpath) {
        log = fopen(logpath, "a");
        if (!log) {
            fprintf(stderr, "gcdslog: cannot open log %s: %s\n",
                    logpath, strerror(errno));
            return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGHUP, SIG_IGN);

    emit(log, "gcdslog: start");

    while (!g_stop) {
        int fd = open_serial(dev, spd);
        if (fd < 0) {
            if (no_reconnect) {
                fprintf(stderr, "gcdslog: cannot open %s: %s\n",
                        dev, strerror(errno));
                break;
            }
            sleep(1);                     /* adapter not present yet */
            continue;
        }
        emit(log, "gcdslog: connected");

        for (;;) {
            fd_set rfds;
            struct timeval tv;
            int rc;

            if (g_stop) break;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            tv.tv_sec = 1; tv.tv_usec = 0;

            rc = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (rc == 0) {                /* idle: flush a stuck partial line */
                if (show_all && ll > 0) {
                    line[ll] = '\0';
                    emit(log, line);
                    ll = 0;
                }
                continue;
            }

            {
                char buf[1024];
                ssize_t n = read(fd, buf, sizeof buf);
                ssize_t i;
                if (n <= 0) break;        /* port closed / error -> reconnect */
                for (i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == '\r') continue;          /* CR of console CRLF */
                    if (c == '\n') {
                        line[ll] = '\0';
                        emit(log, line);
                        ll = 0;
                    } else {
                        if (ll < sizeof(line) - 1)
                            line[ll++] = c;
                        else {            /* overlong line: flush */
                            line[ll] = '\0';
                            emit(log, line);
                            ll = 0;
                            line[ll++] = c;
                        }
                    }
                }
            }
        }

        close(fd);
        if (ll > 0) { line[ll] = '\0'; emit(log, line); ll = 0; }
        emit(log, "gcdslog: port closed");
        if (no_reconnect) break;
        if (!g_stop) sleep(1);            /* wait for the port to come back */
    }

    emit(log, "gcdslog: stop");
    if (log) fclose(log);
    return 0;
}
