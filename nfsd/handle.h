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

/* resolve a 32-byte handle to an interned path, or NULL if the
   handle is unknown/invalid. */
const char *fh_to_path(const unsigned char *fh);

#endif /* GN_HANDLE_H */
