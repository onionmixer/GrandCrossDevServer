#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "handle.h"

#define FH_MAGIC 0x47465348UL   /* "GFSH" */

/* growable path table. Each entry keeps the 64-bit hash of the path
   relative to the export root, which is what makes a handle outlive the
   process (see handle.h). */
static char          **g_paths;
static unsigned long  *g_hi;      /* high half of the 64-bit hash */
static unsigned long  *g_lo;      /* low half                     */
static long            g_count;
static long            g_cap;

static char  g_root[1024];
static long  g_rootlen;
static long  g_rebuilt;           /* 1 once the tree has been walked */

void fh_set_root(const char *root)
{
    long n;

    if (root == NULL)
        return;
    strncpy(g_root, root, sizeof(g_root) - 1);
    g_root[sizeof(g_root) - 1] = '\0';
    n = (long)strlen(g_root);
    while (n > 1 && g_root[n - 1] == '/')
        g_root[--n] = '\0';
    g_rootlen = n;
}

/* path relative to the export root, so a handle stays valid even if the
   export is remounted at a different absolute path */
static const char *relpath(const char *path)
{
    if (g_rootlen > 0 && strncmp(path, g_root, (size_t)g_rootlen) == 0) {
        const char *r = path + g_rootlen;
        while (*r == '/')
            r++;
        return r;
    }
    return path;
}

/* FNV-1a, split into two 32-bit halves so no 64-bit type is needed */
static void hash64(const char *s, unsigned long *hi, unsigned long *lo)
{
    unsigned long h1 = 0x811C9DC5UL, h2 = 0x01000193UL;

    while (*s) {
        unsigned char c = (unsigned char)*s++;
        h1 = ((h1 ^ c) * 16777619UL) & 0xFFFFFFFFUL;
        h2 = ((h2 + c) * 2246822519UL) & 0xFFFFFFFFUL;
        h2 ^= (h1 >> 13);
    }
    *hi = h1 & 0xFFFFFFFFUL;
    *lo = h2 & 0xFFFFFFFFUL;
}

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

/* intern a path, returning its table index or -1 */
static long intern(const char *path)
{
    long i;
    char *dup;
    unsigned long hi, lo;

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_paths[i], path) == 0)
            return i;
    }
    if (g_count >= g_cap) {
        long nc = (g_cap == 0) ? 64 : g_cap * 2;
        char **np = (char **)realloc(g_paths,
                                     (size_t)nc * sizeof(char *));
        unsigned long *nh, *nl;
        if (np == NULL)
            return -1;
        g_paths = np;
        nh = (unsigned long *)realloc(g_hi, (size_t)nc * sizeof(*nh));
        if (nh == NULL)
            return -1;
        g_hi = nh;
        nl = (unsigned long *)realloc(g_lo, (size_t)nc * sizeof(*nl));
        if (nl == NULL)
            return -1;
        g_lo = nl;
        g_cap = nc;
    }
    dup = (char *)malloc(strlen(path) + 1);
    if (dup == NULL)
        return -1;
    strcpy(dup, path);
    hash64(relpath(path), &hi, &lo);
    g_paths[g_count] = dup;
    g_hi[g_count] = hi;
    g_lo[g_count] = lo;
    return g_count++;
}

int fh_from_path(const char *path, unsigned char *fh)
{
    long i = intern(path);

    if (i < 0)
        return -1;
    memset(fh, 0, NFS_FHSIZE);
    put_be32(fh, (unsigned long)i);
    put_be32(fh + 4, FH_MAGIC);
    put_be32(fh + 8, hash_str(path));   /* index verifier */
    put_be32(fh + 12, g_hi[i]);         /* survives a restart */
    put_be32(fh + 16, g_lo[i]);
    return 0;
}

/* walk the export once, interning everything, so handles issued by a
   previous run of the server can be resolved by hash */
static void rebuild_dir(const char *dir, int depth)
{
    DIR *d;
    struct dirent *de;
    char child[1024];
    struct stat st;

    if (depth > 32)
        return;                       /* runaway guard */
    d = opendir(dir);
    if (d == NULL)
        return;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if ((long)(strlen(dir) + strlen(de->d_name) + 2) > (long)sizeof(child))
            continue;
        strcpy(child, dir);
        if (child[strlen(child) - 1] != '/')
            strcat(child, "/");
        strcat(child, de->d_name);
        if (intern(child) < 0)
            break;
        /* lstat: never follow a symlink, so link loops cannot trap us */
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rebuild_dir(child, depth + 1);
    }
    closedir(d);
}

static void rebuild(void)
{
    if (g_rebuilt || g_rootlen == 0)
        return;
    g_rebuilt = 1;                    /* one walk per server lifetime */
    intern(g_root);
    rebuild_dir(g_root, 0);
}

const char *fh_to_path(const unsigned char *fh)
{
    unsigned long idx, hi, lo;
    long i;

    if (get_be32(fh + 4) != FH_MAGIC)
        return NULL;

    /* fast path: the index still refers to the same path */
    idx = get_be32(fh);
    if ((long)idx < g_count &&
        get_be32(fh + 8) == (hash_str(g_paths[idx]) & 0xFFFFFFFFUL))
        return g_paths[idx];

    /* slow path: the table does not have this entry (a restart empties
       it). Resolve by the path hash the handle carries, walking the
       export once if we have not already. */
    hi = get_be32(fh + 12);
    lo = get_be32(fh + 16);
    if (hi == 0 && lo == 0)
        return NULL;                  /* handle from an older gnfsd */
    for (;;) {
        for (i = 0; i < g_count; i++) {
            if (g_hi[i] == hi && g_lo[i] == lo)
                return g_paths[i];
        }
        if (g_rebuilt)
            return NULL;              /* already walked; genuinely stale */
        rebuild();
    }
}
