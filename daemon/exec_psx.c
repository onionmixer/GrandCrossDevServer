/*
 * exec_psx.c - POSIX execution backend.
 *
 * Env vars are injected as a subshell prefix
 *   ( export N='v'; ...; usercmd ) > 'outf' 2> 'errf'
 * instead of putenv(), so nothing leaks into later sessions of
 * this iterative daemon. Single quotes in values are escaped as
 * '\'' which is safe under any POSIX shell.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "gnext.h"          /* NeXTSTEP: int-based W* macros */

#include "gcdsp.h"
#include "util.h"
#include "exec.h"

/* worst case: every value/path char is a quote (4x growth) */
#define CMDBUF_MAX (32768L)

static char cmdbuf[CMDBUF_MAX];

/* append src as a single-quoted shell word; 0 ok, -1 overflow */
static int cat_quoted(char *dst, long size, const char *src)
{
    long d;
    long i;

    d = (long)strlen(dst);
    if (d + 2 >= size)
        return -1;
    dst[d++] = '\'';
    for (i = 0; src[i] != '\0'; i++) {
        if (src[i] == '\'') {
            if (d + 4 >= size)
                return -1;
            dst[d++] = '\'';
            dst[d++] = '\\';
            dst[d++] = '\'';
            dst[d++] = '\'';
        } else {
            if (d + 1 >= size)
                return -1;
            dst[d++] = src[i];
        }
    }
    if (d + 1 >= size)
        return -1;
    dst[d++] = '\'';
    dst[d] = '\0';
    return 0;
}

int build_script(char *dst, long size, const char *cmdline,
                 const gcds_env_t *env, long nenv)
{
    long i;

    dst[0] = '\0';
    gcds_strlcat(dst, "( ", size);
    for (i = 0; i < nenv; i++) {
        gcds_strlcat(dst, "export ", size);
        gcds_strlcat(dst, env[i].name, size);
        gcds_strlcat(dst, "=", size);
        if (cat_quoted(dst, size, env[i].val) < 0)
            return -1;
        gcds_strlcat(dst, "; ", size);
    }
    if (gcds_strlcat(dst, cmdline, size) >= size)
        return -1;
    if (gcds_strlcat(dst, " )", size) >= size)
        return -1;
    return 0;
}

int run_command(const char *cmdline, const gcds_env_t *env, long nenv,
                const char *outf, const char *errf)
{
    int rc;

    if (build_script(cmdbuf, CMDBUF_MAX, cmdline, env, nenv) < 0)
        return -1;
    gcds_strlcat(cmdbuf, " > ", CMDBUF_MAX);
    if (cat_quoted(cmdbuf, CMDBUF_MAX, outf) < 0)
        return -1;
    gcds_strlcat(cmdbuf, " 2> ", CMDBUF_MAX);
    if (cat_quoted(cmdbuf, CMDBUF_MAX, errf) < 0)
        return -1;
    if ((long)strlen(cmdbuf) >= CMDBUF_MAX - 1)
        return -1;

    rc = system(cmdbuf);
    if (rc == -1)
        return -1;
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc) & 255;
    if (WIFSIGNALED(rc))
        return (128 + WTERMSIG(rc)) & 255;
    return -1;
}

const char *os_tag(void)
{
#ifdef GCDS_OSTAG
    return GCDS_OSTAG;
#else
    return "posix";
#endif
}

int os_hostname(char *buf, long size)
{
    if (gethostname(buf, (size_t)(size - 1)) < 0) {
        gcds_strlcpy(buf, "unknown", size);
        return -1;
    }
    buf[size - 1] = '\0';
    return 0;
}
