#include <string.h>
#include "xdr.h"

void xdr_init(xdr_t *x, void *buf, size_t cap)
{
    x->buf = (unsigned char *)buf;
    x->cap = cap;
    x->pos = 0;
    x->err = 0;
}

void xdr_reset(xdr_t *x, size_t pos)
{
    x->pos = pos;
    x->err = 0;
}

size_t xdr_len(const xdr_t *x)
{
    return x->pos;
}

static int need(xdr_t *x, size_t n)
{
    if (x->err || x->pos + n > x->cap) {
        x->err = 1;
        return -1;
    }
    return 0;
}

int xdr_get_u32(xdr_t *x, uint32_t *v)
{
    unsigned char *p;

    if (need(x, 4) < 0)
        return -1;
    p = x->buf + x->pos;
    *v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    x->pos += 4;
    return 0;
}

int xdr_get_i32(xdr_t *x, int32_t *v)
{
    uint32_t u;
    if (xdr_get_u32(x, &u) < 0)
        return -1;
    *v = (int32_t)u;
    return 0;
}

int xdr_get_bytes(xdr_t *x, const unsigned char **p, uint32_t *len)
{
    uint32_t n;
    uint32_t pad;

    if (xdr_get_u32(x, &n) < 0)
        return -1;
    if (need(x, n) < 0)
        return -1;
    *p = x->buf + x->pos;
    *len = n;
    pad = (4 - (n & 3)) & 3;
    if (need(x, (size_t)n + pad) < 0)
        return -1;
    x->pos += n + pad;
    return 0;
}

int xdr_get_fixed(xdr_t *x, unsigned char *out, uint32_t len)
{
    uint32_t pad;

    pad = (4 - (len & 3)) & 3;
    if (need(x, (size_t)len + pad) < 0)
        return -1;
    memcpy(out, x->buf + x->pos, len);
    x->pos += len + pad;
    return 0;
}

int xdr_get_string(xdr_t *x, char *out, size_t max)
{
    const unsigned char *p;
    uint32_t len;

    if (xdr_get_bytes(x, &p, &len) < 0)
        return -1;
    if (len >= max) {
        x->err = 1;
        return -1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

int xdr_put_u32(xdr_t *x, uint32_t v)
{
    unsigned char *p;

    if (need(x, 4) < 0)
        return -1;
    p = x->buf + x->pos;
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
    x->pos += 4;
    return 0;
}

int xdr_put_i32(xdr_t *x, int32_t v)
{
    return xdr_put_u32(x, (uint32_t)v);
}

int xdr_put_fixed(xdr_t *x, const void *p, uint32_t len)
{
    uint32_t pad;

    pad = (4 - (len & 3)) & 3;
    if (need(x, (size_t)len + pad) < 0)
        return -1;
    memcpy(x->buf + x->pos, p, len);
    if (pad)
        memset(x->buf + x->pos + len, 0, pad);
    x->pos += len + pad;
    return 0;
}

int xdr_put_bytes(xdr_t *x, const void *p, uint32_t len)
{
    if (xdr_put_u32(x, len) < 0)
        return -1;
    return xdr_put_fixed(x, p, len);
}
