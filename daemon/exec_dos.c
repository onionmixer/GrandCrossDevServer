/*
 * exec_dos.c - MS-DOS execution backend (PLAN_02 section 4).
 *
 * COMMAND.COM cannot redirect stderr (2>), so instead of
 * system() we duplicate handles 1/2 onto the capture files and
 * spawn the child - DOS children inherit open handles, giving
 * separated stdout/stderr capture.
 *
 * Execution has two paths:
 *  - no shell metacharacters (< > |): spawn the program directly
 *    via spawnvp (PATH search + argv split). This is the common
 *    compiler-invocation case and, crucially, propagates the
 *    child's exit code faithfully (COMMAND.COM /C loses it under
 *    the DOSBox-X internal shell).
 *  - otherwise: COMMAND.COM /C <cmdline> for full shell features
 *    (internal commands, batch). On real DOS this also propagates
 *    errorlevel; under DOSBox-X's internal shell it does not.
 *
 * Env vars are applied with putenv() before the spawn and
 * removed afterwards ("NAME=" deletes under Watcom), keeping the
 * session-scoped semantics of the other backends.
 */
#ifdef GCDS_DOS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>

#include "gcdsp.h"
#include "util.h"
#include "exec.h"

/* putenv needs storage that outlives the call */
static char g_envbuf[GCDSP_ENV_MAX][GCDSP_ENVN_MAX + GCDSP_ENVV_MAX];

/* split a copy of cmdline into argv (in-place); returns argc.
   simple whitespace tokenizer - no quote handling (DOS paths
   rarely contain spaces; 8.3 names never do). */
#define DOS_ARGV_MAX 32
static char g_cmdcopy[GCDSP_LINE_MAX];
static char *g_argv[DOS_ARGV_MAX + 1];

static int split_args(const char *cmdline)
{
    long i;
    int argc;
    int in_word;

    gcds_strlcpy(g_cmdcopy, cmdline, (long)sizeof(g_cmdcopy));
    argc = 0;
    in_word = 0;
    for (i = 0; g_cmdcopy[i] != '\0'; i++) {
        if (g_cmdcopy[i] == ' ' || g_cmdcopy[i] == '\t') {
            g_cmdcopy[i] = '\0';
            in_word = 0;
        } else if (!in_word) {
            if (argc >= DOS_ARGV_MAX)
                break;
            g_argv[argc++] = &g_cmdcopy[i];
            in_word = 1;
        }
    }
    g_argv[argc] = (char *)0;
    return argc;
}

static int has_shell_meta(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '<' || *s == '>' || *s == '|')
            return 1;
    }
    return 0;
}

int run_command(const char *cmdline, const gcds_env_t *env, long nenv,
                const char *outf, const char *errf)
{
    const char *comspec;
    int save1;
    int save2;
    int fo;
    int fe;
    int rc;
    long i;

    comspec = getenv("COMSPEC");
    if (comspec == NULL)
        comspec = "COMMAND.COM";

    for (i = 0; i < nenv; i++) {
        g_envbuf[i][0] = '\0';
        gcds_strlcat(g_envbuf[i], env[i].name,
                     (long)sizeof(g_envbuf[i]));
        gcds_strlcat(g_envbuf[i], "=", (long)sizeof(g_envbuf[i]));
        gcds_strlcat(g_envbuf[i], env[i].val,
                     (long)sizeof(g_envbuf[i]));
        putenv(g_envbuf[i]);
    }

    fo = open(outf, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    fe = open(errf, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fo < 0 || fe < 0) {
        if (fo >= 0)
            close(fo);
        if (fe >= 0)
            close(fe);
        rc = -1;
        goto restore_env;
    }

    fflush(stdout);
    fflush(stderr);
    save1 = dup(1);
    save2 = dup(2);
    dup2(fo, 1);
    dup2(fe, 2);
    close(fo);
    close(fe);

    rc = -1;
    if (!has_shell_meta(cmdline) && split_args(cmdline) >= 1) {
        /* direct spawn first: propagates the child's exit code
           faithfully (the common compiler-invocation case) */
        rc = (int)spawnvp(P_WAIT, g_argv[0], (const char **)g_argv);
    }
    if (rc < 0) {
        /* not a runnable program (internal command like echo/dir,
           or the command uses shell operators): hand it to the
           shell. errorlevel is then only as good as the shell -
           real COMMAND.COM propagates it, DOSBox-X's internal
           shell does not (documented) */
        rc = (int)spawnl(P_WAIT, comspec, comspec, "/C", cmdline,
                         (char *)0);
    }

    dup2(save1, 1);
    dup2(save2, 2);
    close(save1);
    close(save2);

    if (rc < 0)
        rc = -1;
    else if (rc > 255)
        rc = 255;

restore_env:
    for (i = 0; i < nenv; i++) {
        g_envbuf[i][0] = '\0';
        gcds_strlcat(g_envbuf[i], env[i].name,
                     (long)sizeof(g_envbuf[i]));
        gcds_strlcat(g_envbuf[i], "=", (long)sizeof(g_envbuf[i]));
        putenv(g_envbuf[i]);    /* "NAME=" removes the variable */
    }
    return rc;
}

const char *os_tag(void)
{
    return "dos";
}

int os_hostname(char *buf, long size)
{
    const char *h;

    h = getenv("HOSTNAME");
    if (h == NULL || h[0] == '\0')
        h = "dospc";
    gcds_strlcpy(buf, h, size);
    return 0;
}

#else

typedef int gcds_exec_dos_unused;

#endif /* GCDS_DOS */
