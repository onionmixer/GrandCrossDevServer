/*
 * gcdsp.h - GCDSP protocol constants (see PLAN_01_PROTOCOL.md)
 *
 * Strict C89. Lengths and counts are `long` everywhere in this
 * project because `int` is 16bit on the DOS target (PLAN_02).
 */
#ifndef GCDSP_H
#define GCDSP_H

#define GCDSP_VER        1
#define GCDSP_DEF_PORT   9910

/* max control line length, including the terminating LF */
#define GCDSP_LINE_MAX   1024L

/* max payload bytes of one O/E/I frame (TCP).
   DOS large model must fit all statics in a 64K DGROUP, so the
   network-less serial build (chunks at 256 anyway) uses smaller
   buffers - PLAN_02 section 4. */
#ifdef GCDS_DOS
#define GCDSP_FRAME_MAX  1024L
#else
#define GCDSP_FRAME_MAX  4096L
#endif

/* recommended max frame payload on a serial channel */
#define GCDSP_SER_FRAME  256L

/* per-session ENV entry limits */
#ifdef GCDS_DOS
#define GCDSP_ENV_MAX    16
#else
#define GCDSP_ENV_MAX    32
#endif
#define GCDSP_ENVN_MAX   64      /* name buffer (incl. NUL)  */
#define GCDSP_ENVV_MAX   192     /* value buffer (incl. NUL) */

#define GCDSP_TOK_MAX    255     /* AUTH token length         */
#define GCDSP_JOB_MAX    32767L  /* jobid wraps 1..GCDSP_JOB_MAX */

#endif /* GCDSP_H */
