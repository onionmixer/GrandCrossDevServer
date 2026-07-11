/*
 * acl.c - IPv4 allow-list matching (see acl.h). Strict C89.
 */
#include <string.h>

#include "acl.h"

/* parse "a.b.c.d" into a host-order 32-bit value.
   returns 0 ok (*out set), -1 malformed. Stops at end/'/'. */
static int parse_ip(const char *s, const char **end, unsigned long *out)
{
    unsigned long v;
    int i;
    int oct;
    int digits;

    v = 0;
    for (i = 0; i < 4; i++) {
        oct = 0;
        digits = 0;
        while (*s >= '0' && *s <= '9') {
            oct = oct * 10 + (*s - '0');
            digits++;
            if (oct > 255 || digits > 3)
                return -1;
            s++;
        }
        if (digits == 0)
            return -1;
        v = (v << 8) | (unsigned long)oct;
        if (i < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    *out = v;
    if (end != NULL)
        *end = s;
    return 0;
}

/* match one entry ("ip" or "ip/bits") against peer (host order). */
static int match_entry(const char *ent, long len, unsigned long peer)
{
    char buf[64];
    const char *p;
    const char *end;
    unsigned long net;
    unsigned long mask;
    int bits;

    if (len <= 0 || len >= (long)sizeof(buf))
        return 0;
    memcpy(buf, ent, (size_t)len);
    buf[len] = '\0';

    p = buf;
    if (parse_ip(p, &end, &net) != 0)
        return 0;
    if (*end == '\0') {
        return peer == net;         /* exact host */
    }
    if (*end != '/')
        return 0;
    p = end + 1;
    bits = 0;
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9') {
        bits = bits * 10 + (*p - '0');
        if (bits > 32)
            return 0;
        p++;
    }
    if (*p != '\0')
        return 0;
    if (bits == 0)
        mask = 0;                   /* /0 matches everything */
    else
        mask = (unsigned long)0xFFFFFFFFUL << (32 - bits);
    mask &= 0xFFFFFFFFUL;
    return (peer & mask) == (net & mask);
}

int acl_allowed(const char *allow, const char *ip)
{
    unsigned long peer;
    const char *s;
    const char *start;

    if (allow == NULL || allow[0] == '\0')
        return 1;                   /* no list = allow all */
    if (parse_ip(ip, NULL, &peer) != 0)
        return 0;                   /* unparseable peer: deny */

    s = allow;
    while (*s != '\0') {
        while (*s == ' ' || *s == '\t' || *s == ',')
            s++;
        start = s;
        while (*s != '\0' && *s != ' ' && *s != '\t' && *s != ',')
            s++;
        if (s > start && match_entry(start, (long)(s - start), peer))
            return 1;
    }
    return 0;
}
