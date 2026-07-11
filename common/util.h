/*
 * util.h - small string helpers (C89, no snprintf anywhere)
 */
#ifndef UTIL_H
#define UTIL_H

/* bounded copy/concat; always NUL-terminate when size > 0.
   return the length of the string they tried to create,
   so (return >= size) means truncation. */
long gcds_strlcpy(char *dst, const char *src, long size);
long gcds_strlcat(char *dst, const char *src, long size);

/* 1 if s starts with pfx */
int gcds_starts(const char *s, const char *pfx);

#endif /* UTIL_H */
