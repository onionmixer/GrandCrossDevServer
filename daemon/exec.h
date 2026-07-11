/*
 * exec.h - platform command execution (PLAN_02 section 5).
 * Phase 1: blocking run into temp files.
 */
#ifndef EXEC_H
#define EXEC_H

#include "gcdsp.h"

typedef struct {
    char name[GCDSP_ENVN_MAX];
    char val[GCDSP_ENVV_MAX];
} gcds_env_t;

/* run cmdline through the OS shell with env applied, stdout/stderr
   captured to outf/errf. returns exit code 0..255, or -1 if the
   command could not be executed at all. */
int run_command(const char *cmdline, const gcds_env_t *env, long nenv,
                const char *outf, const char *errf);

#ifndef GCDS_WIN32
/* POSIX only: build "( export N='v'; ...; cmd )" without any
   redirection, for supervised execution (live.c). 0 ok, -1 overflow */
int build_script(char *dst, long size, const char *cmdline,
                 const gcds_env_t *env, long nenv);
#else
/* Win32: build `set "N=V" && ... && ( cmd )` without redirection,
   for system() (run_command adds redirects) and for CreateProcess
   via "cmd.exe /c" (live_w32.c). 0 ok, -1 overflow/bad env */
int build_wcmd(char *dst, long size, const char *cmdline,
               const gcds_env_t *env, long nenv);
#endif

const char *os_tag(void);
int os_hostname(char *buf, long size);

#endif /* EXEC_H */
