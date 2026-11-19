/*
 * handle.h - NFSv2 file handle (32 bytes) <-> absolute path.
 *
 * A handle carries a table index (fast path), a magic, and a hash of
 * the path relative to the export root. The index is only meaningful
 * for one server lifetime, but the hash is not: if a handle arrives
 * whose index no longer matches (typically after gnfsd restarts, which
 * empties the table), the export tree is walked once to re-intern every
 * path and the handle is resolved by hash. That makes handles survive a
 * restart, so clients need not remount. No state file is written.
 *
 * The walk is incremental so the server keeps answering meanwhile: the
 * server loop calls fh_walk_step() between requests while
 * fh_walk_pending(), and a handle that only the walk can resolve comes
 * back as FH_WAIT - the request is dropped unanswered and the client's
 * retransmit is answered once the walk has got that far.
 */
#ifndef GN_HANDLE_H
#define GN_HANDLE_H

#define NFS_FHSIZE 32

/* tell the handle layer which directory is exported; the rebuild walk
   is confined to it. Call once at startup, before serving. */
void fh_set_root(const char *root);

/* build a handle for an absolute path (interned). writes 32 bytes
   into fh. returns 0 ok, -1 on allocation failure. */
int fh_from_path(const char *path, unsigned char *fh);

/* fh_resolve() results */
#define FH_OK    0      /* *path set (stable interned pointer)         */
#define FH_WAIT  1      /* unknown yet; the export walk is in progress */
#define FH_STALE 2      /* invalid, or not found by a finished walk    */

/* resolve a 32-byte handle without blocking. May start the walk. */
int fh_resolve(const unsigned char *fh, const char **path);

/* blocking form: finishes the walk itself if it has to. Returns the
   interned path, or NULL if stale. For tests and offline tools - the
   server uses fh_resolve(). */
const char *fh_to_path(const unsigned char *fh);

/* 1 while an export walk is in progress */
int fh_walk_pending(void);
/* advance the walk by one bounded slice (~2000 entries / ~50 ms) */
void fh_walk_step(void);

#endif /* GN_HANDLE_H */
