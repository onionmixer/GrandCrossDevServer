#include <string.h>
#include "util.h"

long gcds_strlcpy(char *dst, const char *src, long size)
{
    long slen;
    long n;

    slen = (long)strlen(src);
    if (size > 0) {
        n = slen;
        if (n > size - 1)
            n = size - 1;
        memcpy(dst, src, (size_t)n);
        dst[n] = '\0';
    }
    return slen;
}

long gcds_strlcat(char *dst, const char *src, long size)
{
    long dlen;
    long slen;
    long n;

    dlen = (long)strlen(dst);
    slen = (long)strlen(src);
    if (dlen < size - 1) {
        n = slen;
        if (n > size - 1 - dlen)
            n = size - 1 - dlen;
        memcpy(dst + dlen, src, (size_t)n);
        dst[dlen + n] = '\0';
    }
    return dlen + slen;
}

int gcds_starts(const char *s, const char *pfx)
{
    while (*pfx != '\0') {
        if (*s != *pfx)
            return 0;
        s++;
        pfx++;
    }
    return 1;
}
