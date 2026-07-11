#include <stdlib.h>
#include <string.h>
#include "handle.h"

#define FH_MAGIC 0x47465348UL   /* "GFSH" */

/* growable path table + a simple open-addressing hash for dedup */
static char   **g_paths;
static long     g_count;
static long     g_cap;

static unsigned long hash_str(const char *s)
{
    unsigned long h = 5381;
    while (*s)
        h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned long get_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

int fh_from_path(const char *path, unsigned char *fh)
{
    long i;

    /* linear scan is fine for a simple server; dedup by string.
       (hash could be added if the table grows large) */
    for (i = 0; i < g_count; i++) {
        if (strcmp(g_paths[i], path) == 0)
            break;
    }
    if (i == g_count) {
        char *dup;
        if (g_count >= g_cap) {
            long nc = (g_cap == 0) ? 64 : g_cap * 2;
            char **np = (char **)realloc(g_paths,
                                         (size_t)nc * sizeof(char *));
            if (np == NULL)
                return -1;
            g_paths = np;
            g_cap = nc;
        }
        dup = (char *)malloc(strlen(path) + 1);
        if (dup == NULL)
            return -1;
        strcpy(dup, path);
        g_paths[g_count] = dup;
        i = g_count;
        g_count++;
    }
    memset(fh, 0, NFS_FHSIZE);
    put_be32(fh, (unsigned long)i);
    put_be32(fh + 4, FH_MAGIC);
    put_be32(fh + 8, hash_str(path));   /* extra verifier */
    return 0;
}

const char *fh_to_path(const unsigned char *fh)
{
    unsigned long idx;

    if (get_be32(fh + 4) != FH_MAGIC)
        return NULL;
    idx = get_be32(fh);
    if ((long)idx >= g_count)
        return NULL;
    /* the verifier was stored truncated to 32 bits (put_be32), so
       compare against the low 32 bits of the hash */
    if (get_be32(fh + 8) != (hash_str(g_paths[idx]) & 0xFFFFFFFFUL))
        return NULL;
    return g_paths[idx];
}
