/*
 * handle.h - NFSv2 file handle (32 bytes) <-> absolute path.
 * Handles are opaque to the client; we encode a table index +
 * a magic verifier. Stable for the server's lifetime; paths are
 * de-duplicated so the same path always yields the same handle.
 */
#ifndef GN_HANDLE_H
#define GN_HANDLE_H

#define NFS_FHSIZE 32

/* build a handle for an absolute path (interned). writes 32 bytes
   into fh. returns 0 ok, -1 on allocation failure. */
int fh_from_path(const char *path, unsigned char *fh);

/* resolve a 32-byte handle to an interned path, or NULL if the
   handle is unknown/invalid. */
const char *fh_to_path(const unsigned char *fh);

#endif /* GN_HANDLE_H */
