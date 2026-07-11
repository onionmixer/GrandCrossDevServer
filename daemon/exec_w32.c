/*
 * exec_w32.c - Win32 execution backend.
 *
 * system() on the Windows CRT runs "%COMSPEC% /c <string>" and
 * returns cmd.exe's exit code directly. cmd.exe (unlike DOS
 * COMMAND.COM) supports 2> redirection and ( ) grouping, so the
 * shape mirrors the POSIX backend:
 *   set "N=V" && ... && ( usercmd ) > "outf" 2> "errf"
 *
 * Limitation (documented): env values containing a double quote
 * are rejected (cmd.exe quoting is not safely composable), and
 * %VAR% inside values is expanded by cmd.exe.
 */
#ifdef GCDS_WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "gcdsp.h"
#include "util.h"
#include "exec.h"

#define CMDBUF_MAX (16384L)

static char cmdbuf[CMDBUF_MAX];

int build_wcmd(char *dst, long size, const char *cmdline,
               const gcds_env_t *env, long nenv)
{
    long i;

    dst[0] = '\0';
    for (i = 0; i < nenv; i++) {
        if (strchr(env[i].val, '"') != NULL)
            return -1;
        gcds_strlcat(dst, "set \"", size);
        gcds_strlcat(dst, env[i].name, size);
        gcds_strlcat(dst, "=", size);
        gcds_strlcat(dst, env[i].val, size);
        gcds_strlcat(dst, "\" && ", size);
    }
    gcds_strlcat(dst, "( ", size);
    gcds_strlcat(dst, cmdline, size);
    if (gcds_strlcat(dst, " )", size) >= size)
        return -1;
    return 0;
}

int run_command(const char *cmdline, const gcds_env_t *env, long nenv,
                const char *outf, const char *errf)
{
    int rc;

    if (strchr(outf, '"') != NULL || strchr(errf, '"') != NULL)
        return -1;

    if (build_wcmd(cmdbuf, CMDBUF_MAX, cmdline, env, nenv) < 0)
        return -1;
    gcds_strlcat(cmdbuf, " > \"", CMDBUF_MAX);
    gcds_strlcat(cmdbuf, outf, CMDBUF_MAX);
    gcds_strlcat(cmdbuf, "\" 2> \"", CMDBUF_MAX);
    gcds_strlcat(cmdbuf, errf, CMDBUF_MAX);
    if (gcds_strlcat(cmdbuf, "\"", CMDBUF_MAX) >= CMDBUF_MAX)
        return -1;

    rc = system(cmdbuf);
    if (rc < 0)
        return -1;
    if (rc > 255)
        return 255;     /* clamp (PLAN_01 section 5) */
    return rc;
}

const char *os_tag(void)
{
    return "win32";
}

int os_hostname(char *buf, long size)
{
    DWORD n;

    n = (DWORD)(size - 1);
    if (!GetComputerNameA(buf, &n)) {
        gcds_strlcpy(buf, "unknown", size);
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

#else

typedef int gcds_exec_w32_unused;

#endif /* GCDS_WIN32 */
