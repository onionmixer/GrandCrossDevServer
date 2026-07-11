/*
 * gnext.h - NeXTSTEP / OPENSTEP (4.3BSD, cc/gcc 2.7) compatibility.
 * Active only under -DGCDS_NEXT. Include AFTER the system headers
 * in files that need it (it overrides some libc macros/types).
 *
 * NeXTSTEP predates POSIX 2001, so several types/macros our POSIX
 * code assumes are missing or shaped for `union wait`.
 */
#ifndef GNEXT_H
#define GNEXT_H

#ifdef GCDS_NEXT

#include <sys/types.h>
#include <sys/time.h>       /* select(), FD_SET, struct timeval */

/* POSIX types absent without _POSIX_SOURCE on 4.3BSD */
typedef int socklen_t;
typedef int pid_t;

/* wait() status here is a plain int, but the stock W* macros
   assume `union wait` (they read .w_S/.w_T). Replace them with
   int-based versions. */
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#define WIFEXITED(s)   (((s) & 0xff) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)    ((s) & 0x7f)
#ifndef WNOHANG
#define WNOHANG 1
#endif

/* 4.3BSD has no waitpid(); wait4() covers it. The int status is
   aliased to `union wait` (same 32-bit word - matches the
   int-based W* macros above). */
#include <sys/wait.h>       /* union wait */
#include <sys/resource.h>   /* struct rusage */
#define waitpid(p, s, o) \
    ((pid_t)wait4((pid_t)(p), (union wait *)(s), (o), \
                  (struct rusage *)0))

/* 4.3BSD spells setpgid() as setpgrp(pid, pgrp) with the same
   argument meaning, EXCEPT that 0 defaults are not portable:
   callers must pass explicit ids (live.c does - it uses
   setpgid(0, getpid()) in the child, setpgid(pid, pid) in the
   parent; pid 0 = calling process works on 4.3BSD too). */
#define setpgid(p, g) setpgrp((p), (g))

#endif /* GCDS_NEXT */
#endif /* GNEXT_H */
