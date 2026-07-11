/*
 * xdr.h - minimal XDR (RFC 1014) cursor over a fixed buffer.
 * Big-endian, 4-byte aligned. Used to decode RPC call args and
 * encode replies for the simple NFSv2 server (gnfsd).
 *
 * Linux host tool: not bound by the project's C89/portability
 * rules (it runs on the machine being shared, always Linux).
 */
#ifndef GN_XDR_H
#define GN_XDR_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    unsigned char *buf;
    size_t cap;         /* buffer capacity */
    size_t pos;         /* cursor */
    int    err;         /* set on overflow/underflow */
} xdr_t;

void xdr_init(xdr_t *x, void *buf, size_t cap);
void xdr_reset(xdr_t *x, size_t pos);

/* decode (return 0 ok, -1 on underflow; leaves x->err set) */
int xdr_get_u32(xdr_t *x, uint32_t *v);
int xdr_get_i32(xdr_t *x, int32_t *v);
/* variable-length opaque/string: returns pointer into buffer +
   length; NOT null-terminated. */
int xdr_get_bytes(xdr_t *x, const unsigned char **p, uint32_t *len);
/* fixed-length opaque (e.g. 32-byte fhandle) */
int xdr_get_fixed(xdr_t *x, unsigned char *out, uint32_t len);
/* copy a string (<max incl NUL) into out; 0 ok, -1 err/too long */
int xdr_get_string(xdr_t *x, char *out, size_t max);

/* encode (return 0 ok, -1 on overflow) */
int xdr_put_u32(xdr_t *x, uint32_t v);
int xdr_put_i32(xdr_t *x, int32_t v);
int xdr_put_bytes(xdr_t *x, const void *p, uint32_t len);   /* opaque<> */
int xdr_put_fixed(xdr_t *x, const void *p, uint32_t len);   /* opaque[n] */

size_t xdr_len(const xdr_t *x);     /* bytes written/consumed */

#endif /* GN_XDR_H */
