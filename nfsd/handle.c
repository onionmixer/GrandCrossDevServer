#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "handle.h"

#define FH_MAGIC 0x47465348UL   /* "GFSH" */

/* entry limit: the index travels in a 32-bit handle word, and must also
   fit a signed long on ILP32 hosts */
#define GN_MAXENT 0x7FFFFFFFL

/* growable path table. Each entry keeps the 64-bit hash of the path
   relative to the export root, which is what makes a handle outlive the
   process (see handle.h). Entries are only ever appended; each path
   string is malloc'd on its own and never moves, because nfs.c holds
   the returned pointers ("stable interned pointer"). */
static char          **g_paths;
static unsigned long  *g_hi;      /* high half of the 64-bit hash */
static unsigned long  *g_lo;      /* low half                     */
static long            g_count;
static long            g_cap;

/* open-addressing index over the table, keyed by the stored (hi, lo).
   One index serves both lookups: intern() probes it and confirms the
   absolute path with strcmp; fh_to_path() probes it and compares
   (hi, lo). Without it both were linear scans, which made the restart
   walk (rebuild) quadratic in the tree size - hours on a multi-million
   entry export, with the single-threaded server deaf the whole time.

   Inserts happen in table order and nothing is ever removed (a rehash
   re-inserts in table order too), so along a probe chain entries with
   equal keys sit in increasing index order: the first (hi, lo) match
   is the lowest index - the same answer the old front-to-back scan
   gave. */
static long          *g_slot;     /* table index, or -1 = empty slot */
static unsigned long  g_nslot;    /* slot count: 0 or a power of two */

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

/* slot hash for the index. Only picks a starting slot - equality is
   always decided on the full key - so it is free to differ from the
   wire hashes above (which must never change). murmur3's 32-bit
   finalizer spreads every input bit into the low bits we mask with. */
static unsigned long slot_hash(unsigned long hi, unsigned long lo)
{
    unsigned long h = (hi ^ (lo * 0x9E3779B1UL)) & 0xFFFFFFFFUL;

    h ^= h >> 16;
    h = (h * 0x85EBCA6BUL) & 0xFFFFFFFFUL;
    h ^= h >> 13;
    h = (h * 0xC2B2AE35UL) & 0xFFFFFFFFUL;
    h ^= h >> 16;
    return h;
}

/* put table entry i into the first free slot of its probe chain */
static void slot_put(long *slots, unsigned long nslot, long i)
{
    unsigned long s = slot_hash(g_hi[i], g_lo[i]) & (nslot - 1);

    while (slots[s] >= 0)
        s = (s + 1) & (nslot - 1);
    slots[s] = i;
}

/* make room for `want` entries at load <= 1/2. On failure the old
   index is untouched. */
static int slot_reserve(long want)
{
    unsigned long ns, k;
    long *nsl;
    long i;

    if ((unsigned long)want <= g_nslot / 2)
        return 0;
    ns = (g_nslot == 0) ? 1024UL : g_nslot;
    while ((unsigned long)want > ns / 2) {
        if (ns >= 0x80000000UL)       /* keep ns*2 from wrapping on ILP32 */
            return -1;
        ns *= 2;
    }
    if (ns > ((size_t)-1) / sizeof(long))
        return -1;
    nsl = (long *)malloc((size_t)ns * sizeof(long));
    if (nsl == NULL)
        return -1;
    for (k = 0; k < ns; k++)
        nsl[k] = -1;
    for (i = 0; i < g_count; i++)     /* table order: keeps the chain order */
        slot_put(nsl, ns, i);
    free(g_slot);
    g_slot = nsl;
    g_nslot = ns;
    return 0;
}

/* grow the three parallel arrays. g_cap only moves once all three have
   grown, so a failure part-way leaves a consistent (if roomier) table. */
static int table_reserve(void)
{
    long nc;
    char **np;
    unsigned long *nh, *nl;

    if (g_count < g_cap)
        return 0;
    if (g_cap >= GN_MAXENT)
        return -1;
    if (g_cap == 0)
        nc = 64;
    else if (g_cap > GN_MAXENT / 2)
        nc = GN_MAXENT;
    else
        nc = g_cap * 2;
    if ((unsigned long)nc > ((size_t)-1) / sizeof(unsigned long))
        return -1;
    np = (char **)realloc(g_paths, (size_t)nc * sizeof(char *));
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
    return 0;
}

/* lowest table index whose stored hash is (hi, lo), or -1 */
static long find_hash(unsigned long hi, unsigned long lo)
{
    unsigned long s;
    long i;

    if (g_nslot == 0)
        return -1;
    for (s = slot_hash(hi, lo) & (g_nslot - 1); (i = g_slot[s]) >= 0;
         s = (s + 1) & (g_nslot - 1)) {
        if (g_hi[i] == hi && g_lo[i] == lo)
            return i;
    }
    return -1;
}

/* intern a path, returning its table index or -1 */
static long intern(const char *path)
{
    unsigned long hi, lo, s;
    long i;
    char *dup;

    hash64(relpath(path), &hi, &lo);
    if (g_nslot != 0) {
        for (s = slot_hash(hi, lo) & (g_nslot - 1); (i = g_slot[s]) >= 0;
             s = (s + 1) & (g_nslot - 1)) {
            if (g_hi[i] == hi && g_lo[i] == lo &&
                strcmp(g_paths[i], path) == 0)
                return i;
        }
    }
    /* new entry: reserve everything before changing anything, so an
       allocation failure leaves every existing entry findable */
    if (table_reserve() < 0 || slot_reserve(g_count + 1) < 0)
        return -1;
    dup = (char *)malloc(strlen(path) + 1);
    if (dup == NULL)
        return -1;
    strcpy(dup, path);
    g_paths[g_count] = dup;
    g_hi[g_count] = hi;
    g_lo[g_count] = lo;
    slot_put(g_slot, g_nslot, g_count);
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
    long before;
    time_t t0;

    if (g_rebuilt || g_rootlen == 0)
        return;
    g_rebuilt = 1;                    /* one walk per server lifetime */
    /* the walk blocks the (single-threaded) server; say so, so a busy
       gnfsd is not mistaken for a hung one */
    before = g_count;
    t0 = time(NULL);
    fprintf(stderr, "gnfsd: unknown handle - walking %s to re-resolve...\n",
            g_root);
    intern(g_root);
    rebuild_dir(g_root, 0);
    fprintf(stderr, "gnfsd: walk done: %ld new entries (%ld total), %lds\n",
            g_count - before, g_count, (long)(time(NULL) - t0));
}

const char *fh_to_path(const unsigned char *fh)
{
    unsigned long idx, hi, lo;
    long i;

    if (get_be32(fh + 4) != FH_MAGIC)
        return NULL;
    idx = get_be32(fh);
    hi = get_be32(fh + 12);
    lo = get_be32(fh + 16);

    /* fast path: the index still refers to the same path. The 32-bit
       verifier alone can match by chance after a restart, so a handle
       that carries (hi, lo) must match those too; (0, 0) is the format
       of an older gnfsd and is checked as before. */
    if (idx < (unsigned long)g_count &&
        get_be32(fh + 8) == (hash_str(g_paths[idx]) & 0xFFFFFFFFUL) &&
        ((hi == 0 && lo == 0) || (g_hi[idx] == hi && g_lo[idx] == lo)))
        return g_paths[idx];

    /* slow path: the table does not have this entry (a restart empties
       it). Resolve by the path hash the handle carries, walking the
       export once if we have not already. */
    if (hi == 0 && lo == 0)
        return NULL;                  /* handle from an older gnfsd */
    for (;;) {
        i = find_hash(hi, lo);
        if (i >= 0)
            return g_paths[i];
        if (g_rebuilt)
            return NULL;              /* already walked; genuinely stale */
        rebuild();
    }
}
