/*
 * live_w32.c - Win32 supervised execution (PLAN_02 4, 5.2).
 * Winsock select() only watches sockets, so this is the polling
 * hybrid: select({listen, session}, 100ms) + PeekNamedPipe for
 * child output + WaitForSingleObject(child, 0).
 *
 * Differences from the POSIX loop, by design:
 * - K kills with TerminateProcess immediately (no SIGTERM grace;
 *   exit reported as 255 - PLAN_01 5.1).
 * - a serial session (COM) cannot be select()ed: RUNI is not
 *   offered in serial mode (main.c withholds INTERACTIVE), and
 *   client death during RUN is only noticed at the next write.
 */
#if (defined(GCDS_HAS_LIVE) || defined(GCDS_HAS_IX)) && \
    defined(GCDS_WIN32)

#include <winsock.h>
#include <windows.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcdsp.h"
#include "util.h"
#include "lineio.h"
#include "session.h"
#include "live.h"

#define WCMD_MAX (16384L)
#define POLL_MS  100

static char g_wcmd[WCMD_MAX];

/* Job Object API (Win2000+), loaded at runtime so the daemon
   still links/loads on NT4. A job groups cmd.exe with all its
   descendants; terminating the job kills grandchildren too -
   the Windows equivalent of the POSIX process-group kill.
   Without it (NT4), TerminateProcess kills only cmd.exe and a
   surviving grandchild would hang the supervisor on pipe EOF. */
typedef HANDLE (WINAPI *CJO_t)(void *, const char *);
typedef BOOL (WINAPI *AP2J_t)(HANDLE, HANDLE);
typedef BOOL (WINAPI *TJO_t)(HANDLE, unsigned);

static int g_job_loaded = 0;
static CJO_t p_CreateJobObject = NULL;
static AP2J_t p_AssignProcessToJobObject = NULL;
static TJO_t p_TerminateJobObject = NULL;

static void load_job_api(void)
{
    HMODULE k;

    if (g_job_loaded)
        return;
    g_job_loaded = 1;
    k = GetModuleHandleA("kernel32.dll");
    if (k == NULL)
        return;
    p_CreateJobObject =
        (CJO_t)GetProcAddress(k, "CreateJobObjectA");
    p_AssignProcessToJobObject =
        (AP2J_t)GetProcAddress(k, "AssignProcessToJobObject");
    p_TerminateJobObject =
        (TJO_t)GetProcAddress(k, "TerminateJobObject");
}

static HANDLE make_job(HANDLE proc)
{
    HANDLE job;

    load_job_api();
    if (p_CreateJobObject == NULL || p_AssignProcessToJobObject == NULL)
        return NULL;
    job = p_CreateJobObject(NULL, NULL);
    if (job == NULL)
        return NULL;
    if (!p_AssignProcessToJobObject(job, proc)) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

/* kill the whole job if we have one, else just the process */
static void kill_job(HANDLE job, HANDLE proc)
{
    if (job != NULL && p_TerminateJobObject != NULL)
        p_TerminateJobObject(job, 255);
    else
        TerminateProcess(proc, 255);
}

/* returns 1 if some watched socket is readable within POLL_MS */
static int sock_wait(gcds_sock_t ls, gcds_chan_t *sess, int *ls_rd,
                     int *sess_rd)
{
    fd_set rd;
    struct timeval tv;
    int any;

    *ls_rd = 0;
    *sess_rd = 0;
    any = 0;
    FD_ZERO(&rd);
    if (ls != GCDS_BADSOCK) {
        FD_SET((SOCKET)ls, &rd);
        any = 1;
    }
    if (sess != NULL && sess->kind == GCDS_CHAN_TCP) {
        FD_SET((SOCKET)sess->s, &rd);
        any = 1;
    }
    if (!any) {
        Sleep(POLL_MS);
        return 0;
    }
    tv.tv_sec = 0;
    tv.tv_usec = POLL_MS * 1000L;
    if (select(0, &rd, NULL, NULL, &tv) <= 0)
        return 0;
    if (ls != GCDS_BADSOCK && FD_ISSET((SOCKET)ls, &rd))
        *ls_rd = 1;
    if (sess != NULL && sess->kind == GCDS_CHAN_TCP &&
        FD_ISSET((SOCKET)sess->s, &rd))
        *sess_rd = 1;
    return 1;
}

/* poll one child pipe; stream available bytes as a frame.
   returns 0 open, 1 EOF/broken, -1 session write error */
static int pump(gcds_chan_t *sess, char tag, HANDLE h, long chunk,
                long *total)
{
    static char buf[GCDSP_FRAME_MAX];
    DWORD avail;
    DWORD got;
    long want;

    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
        return 1;               /* broken pipe: child side closed */
    if (avail == 0)
        return 0;
    want = (long)avail;
    if (want > chunk)
        want = chunk;
    if (!ReadFile(h, buf, (DWORD)want, &got, NULL))
        return 1;
    if (got == 0)
        return 0;
    *total += (long)got;
    if (sess != NULL) {
        if (lio_put_frame(sess, tag, buf, (long)got) != LIO_OK)
            return -1;
    }
    return 0;
}

/* one I/K frame during RUNI; 0 ok, -1 fatal */
static int runi_frame(gcds_chan_t *sess, const char *line,
                      HANDLE *hin, HANDLE hproc, HANDLE job,
                      int *killed)
{
    static char buf[GCDSP_FRAME_MAX];
    long n;
    DWORD put;

    if (line[0] == 'K' && line[1] == '\0') {
        if (!*killed) {
            kill_job(job, hproc);
            *killed = 1;
        }
        return 0;
    }
    if (line[0] == 'I' && line[1] == ' ') {
        n = atol(line + 2);
        if (n < 0 || n > GCDSP_FRAME_MAX)
            return -1;
        if (n == 0) {
            if (*hin != NULL) {
                CloseHandle(*hin);
                *hin = NULL;
            }
            return 0;
        }
        if (chan_read_n(sess, buf, n) < 0)
            return -1;
        if (*hin != NULL && !*killed) {
            if (!WriteFile(*hin, buf, (DWORD)n, &put, NULL)) {
                CloseHandle(*hin);
                *hin = NULL;
            }
        }
        return 0;
    }
    return -1;
}

int live_exec(int mode, gcds_chan_t *sess, gcds_sock_t ls,
              const char *cmd, const gcds_env_t *env, long nenv,
              const char *outf, const char *errf, long jobid)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE in_r, in_w;
    HANDLE out_r, out_w;
    HANDLE err_r, err_w;
    HANDLE job;
    int out_done, err_done;
    int killed;
    int lost;
    long chunk;
    long total;
    long cap;
    int capped;
    DWORD code;

    if (build_wcmd(g_wcmd + 16, WCMD_MAX - 16, cmd, env, nenv) < 0)
        return -1;
    total = 0;
    cap = session_maxout();
    capped = 0;
    memcpy(g_wcmd, "cmd.exe /c ", 11);
    memmove(g_wcmd + 11, g_wcmd + 16, strlen(g_wcmd + 16) + 1);

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    in_r = in_w = out_r = out_w = err_r = err_w = NULL;
    if (!CreatePipe(&in_r, &in_w, &sa, 0))
        return -1;
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

    if (mode == LIVE_JOB) {
        out_w = CreateFileA(outf, GENERIC_WRITE, 0, &sa,
                            CREATE_ALWAYS, 0, NULL);
        err_w = CreateFileA(errf, GENERIC_WRITE, 0, &sa,
                            CREATE_ALWAYS, 0, NULL);
        if (out_w == INVALID_HANDLE_VALUE ||
            err_w == INVALID_HANDLE_VALUE) {
            CloseHandle(in_r);
            CloseHandle(in_w);
            if (out_w != INVALID_HANDLE_VALUE)
                CloseHandle(out_w);
            if (err_w != INVALID_HANDLE_VALUE)
                CloseHandle(err_w);
            return -1;
        }
    } else {
        if (!CreatePipe(&out_r, &out_w, &sa, 0) ||
            !CreatePipe(&err_r, &err_w, &sa, 0)) {
            CloseHandle(in_r);
            CloseHandle(in_w);
            if (out_r != NULL) CloseHandle(out_r);
            if (out_w != NULL) CloseHandle(out_w);
            return -1;
        }
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    memset(&pi, 0, sizeof(pi));

    /* CREATE_SUSPENDED so we can assign the process to a job
       before it runs and spawns grandchildren (race-free) */
    if (!CreateProcessA(NULL, g_wcmd, NULL, NULL, TRUE,
                        CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        CloseHandle(out_w);
        CloseHandle(err_w);
        if (out_r != NULL) CloseHandle(out_r);
        if (err_r != NULL) CloseHandle(err_r);
        return -1;
    }
    job = make_job(pi.hProcess);    /* NULL on NT4/failure */
    ResumeThread(pi.hThread);

    /* parent keeps: in_w (RUNI only), out_r/err_r (non-JOB);
       the child inherited its own copies of the rest */
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    out_w = NULL;
    err_w = NULL;
    CloseHandle(pi.hThread);
    if (mode != LIVE_RUNI) {
        CloseHandle(in_w);      /* child stdin sees EOF */
        in_w = NULL;
    }

    out_done = (mode == LIVE_JOB);
    err_done = (mode == LIVE_JOB);
    killed = 0;
    lost = 0;
    chunk = (sess != NULL) ? chan_chunk(sess) : 0;

    for (;;) {
        int ls_rd;
        int sess_rd;
        int r;

        sock_wait(ls, (mode == LIVE_JOB) ? NULL : sess,
                  &ls_rd, &sess_rd);

        if (ls_rd) {
            gcds_sock_t cs;
            gcds_chan_t cc;
            char cip[16];

            cs = net_accept(ls, cip);
            if (cs != GCDS_BADSOCK) {
                if (session_peer_ok(cip)) {
                    chan_tcp(&cc, cs);
                    ctl_session(&cc, (mode == LIVE_JOB) ? jobid : 0);
                    chan_close(&cc);
                } else {
                    net_close(cs);
                }
            }
        }

        if (sess_rd) {
            if (mode == LIVE_RUN) {
                lost = 1;       /* client must stay silent */
            } else {
                static char line[GCDSP_LINE_MAX];

                if (lio_get_line(sess, line, GCDSP_LINE_MAX)
                    != LIO_OK)
                    lost = 1;
                else if (runi_frame(sess, line, &in_w,
                                    pi.hProcess, job, &killed) < 0)
                    lost = 1;
            }
        }

        if (!out_done) {
            r = pump(sess, 'O', out_r, chunk, &total);
            if (r != 0) {
                if (r < 0)
                    lost = 1;
                if (r > 0)
                    out_done = 1;
            }
        }
        if (!err_done) {
            r = pump(sess, 'E', err_r, chunk, &total);
            if (r != 0) {
                if (r < 0)
                    lost = 1;
                if (r > 0)
                    err_done = 1;
            }
        }

        /* output cap: streaming (non-JOB) via total, JOB via file
           sizes (child writes the files directly) */
        if (cap > 0 && !capped) {
            long tot = total;
            if (mode == LIVE_JOB) {
                struct _stat sb;
                tot = 0;
                if (_stat(outf, &sb) == 0) tot += (long)sb.st_size;
                if (_stat(errf, &sb) == 0) tot += (long)sb.st_size;
            }
            if (tot > cap) {
                capped = 1;
                if (mode != LIVE_JOB)
                    lio_put_frame(sess, 'E',
                        "\n[gcdsd: output truncated at cap]\n", 34);
                kill_job(job, pi.hProcess);
            }
        }

        if (lost) {
            kill_job(job, pi.hProcess);
            break;
        }

        if (out_done && err_done &&
            WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
            break;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job != NULL)
        CloseHandle(job);
    if (in_w != NULL)
        CloseHandle(in_w);
    if (out_r != NULL)
        CloseHandle(out_r);
    if (err_r != NULL)
        CloseHandle(err_r);

    if (lost)
        return -2;
    if (code > 255)
        return 255;
    return (int)code;
}

#else

typedef int gcds_live_w32_unused;

#endif /* (GCDS_HAS_LIVE || GCDS_HAS_IX) && GCDS_WIN32 */
