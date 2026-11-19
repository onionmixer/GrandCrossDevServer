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

/* ---- the export walk (re-resolving handles after a restart) --------
 * The first unknown new-format handle starts a walk of the export that
 * interns every path, so the handle can be found by its (hi, lo). The
 * walk is incremental: fh_walk_step() advances it by one bounded slice
 * and the server loop calls it between requests, so the server keeps
 * answering while it walks (PLAN_handle_rebuild_fix.md 5.2). It is the
 * same depth-first order the old recursive walk used: depth > 32,
 * over-long paths and unreadable directories are skipped, symlinked
 * directories are not followed, mount points are crossed.
 *
 * States: not started -> running -> done | failed. "failed" (the root
 * cannot be opened, or an allocation failed) is not treated as done:
 * it is logged, and an old handle the partial table cannot resolve is
 * answered STALE - retrying would fail the same way. One walk per
 * server lifetime either way.
 *
 * The tree may change while a walk is in progress (it is not a
 * snapshot): a directory renamed under an open DIR is recorded under
 * its old path, an old handle not found once the walk ends is STALE,
 * and new paths are interned by LOOKUP as usual.
 */
#define WALK_MAXDEPTH      32
#define WALK_SLICE_ENTRIES 2000L     /* per slice, and ...           */
#define WALK_SLICE_USEC    50000L    /* ... a time target, not a cap */
#define WALK_LOG_EVERY     500000L

enum { WALK_IDLE, WALK_RUNNING, WALK_DONE, WALK_FAILED };

typedef struct {
    DIR        *d;
    const char *path;                /* interned: stable pointer */
    int         depth;
} walk_frame_t;

static int          g_walk = WALK_IDLE;
static walk_frame_t g_wstack[WALK_MAXDEPTH + 1];
static int          g_wtop;          /* frames in use */
static long         g_wbefore;       /* table size when the walk began */
static long         g_wnextlog;
static time_t       g_wt0;

static long usec_since(const struct timespec *t0)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec - t0->tv_sec) * 1000000L +
           (long)(t.tv_nsec - t0->tv_nsec) / 1000L;
}

static void walk_end(int state, const char *why)
{
    while (g_wtop > 0)
        closedir(g_wstack[--g_wtop].d);
    g_walk = state;
    if (state == WALK_DONE)
        fprintf(stderr, "gnfsd: walk done: %ld new entries (%ld total), "
                "%lds\n", g_count - g_wbefore, g_count,
                (long)(time(NULL) - g_wt0));
    else
        fprintf(stderr, "gnfsd: walk FAILED (%s) after %ld new entries; "
                "old handles it did not reach will be answered STALE\n",
                why, g_count - g_wbefore);
}

static void walk_start(void)
{
    DIR *d;
    long ri;

    g_walk = WALK_RUNNING;
    g_wbefore = g_count;
    g_wnextlog = g_count + WALK_LOG_EVERY;
    g_wt0 = time(NULL);
    fprintf(stderr, "gnfsd: unknown handle - walking %s to re-resolve "
            "(still serving meanwhile)...\n", g_root);
    ri = intern(g_root);
    if (ri < 0) {
        walk_end(WALK_FAILED, "out of memory");
        return;
    }
    d = opendir(g_root);
    if (d == NULL) {
        walk_end(WALK_FAILED, "cannot open the export root");
        return;
    }
    g_wstack[0].d = d;
    g_wstack[0].path = g_paths[ri];
    g_wstack[0].depth = 0;
    g_wtop = 1;
}

int fh_walk_pending(void)
{
    return g_walk == WALK_RUNNING;
}

void fh_walk_step(void)
{
    struct timespec t0;
    long n = 0, ci;
    walk_frame_t *f;
    struct dirent *de;
    char child[1024];
    struct stat st;
    size_t dl;

    if (g_walk != WALK_RUNNING)
        return;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (g_wtop > 0) {
        f = &g_wstack[g_wtop - 1];
        de = readdir(f->d);
        if (de == NULL) {             /* directory finished */
            closedir(f->d);
            g_wtop--;
            continue;
        }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        dl = strlen(f->path);
        if (dl + strlen(de->d_name) + 2 > sizeof(child))
            continue;
        strcpy(child, f->path);
        if (child[dl - 1] != '/')
            strcat(child, "/");
        strcat(child, de->d_name);
        ci = intern(child);
        if (ci < 0) {
            walk_end(WALK_FAILED, "out of memory");
            return;
        }
        /* lstat: never follow a symlink, so link loops cannot trap us */
        if (f->depth < WALK_MAXDEPTH && lstat(child, &st) == 0 &&
            S_ISDIR(st.st_mode)) {
            DIR *cd = opendir(child);
            if (cd != NULL) {         /* depth first, like the recursion */
                g_wstack[g_wtop].d = cd;
                g_wstack[g_wtop].path = g_paths[ci];
                g_wstack[g_wtop].depth = f->depth + 1;
                g_wtop++;
            }
        }
        if (++n >= WALK_SLICE_ENTRIES)
            break;
        if ((n & 63) == 0 && usec_since(&t0) >= WALK_SLICE_USEC)
            break;
    }
    if (g_count >= g_wnextlog) {
        fprintf(stderr, "gnfsd: walk: %ld entries so far, %lds\n",
                g_count - g_wbefore, (long)(time(NULL) - g_wt0));
        g_wnextlog = g_count + WALK_LOG_EVERY;
    }
    if (g_wtop == 0)
        walk_end(WALK_DONE, NULL);
}

int fh_resolve(const unsigned char *fh, const char **path)
{
    unsigned long idx, hi, lo;
    long i;

    if (get_be32(fh + 4) != FH_MAGIC)
        return FH_STALE;
    idx = get_be32(fh);
    hi = get_be32(fh + 12);
    lo = get_be32(fh + 16);

    /* fast path: the index still refers to the same path. The 32-bit
       verifier alone can match by chance after a restart, so a handle
       that carries (hi, lo) must match those too; (0, 0) is the format
       of an older gnfsd and is checked as before. */
    if (idx < (unsigned long)g_count &&
        get_be32(fh + 8) == (hash_str(g_paths[idx]) & 0xFFFFFFFFUL) &&
        ((hi == 0 && lo == 0) || (g_hi[idx] == hi && g_lo[idx] == lo))) {
        *path = g_paths[idx];
        return FH_OK;
    }

    /* slow path: the table does not have this entry (a restart empties
       it). Resolve by the path hash the handle carries; if it is not
       known yet, the export walk will find it - or prove it stale. */
    if (hi == 0 && lo == 0)
        return FH_STALE;              /* handle from an older gnfsd */
    i = find_hash(hi, lo);
    if (i < 0 && g_walk == WALK_IDLE && g_rootlen > 0) {
        walk_start();
        i = find_hash(hi, lo);        /* it may be the root itself */
    }
    if (i >= 0) {
        *path = g_paths[i];
        return FH_OK;
    }
    return g_walk == WALK_RUNNING ? FH_WAIT : FH_STALE;
}

const char *fh_to_path(const unsigned char *fh)
{
    const char *p = NULL;
    int r;

    while ((r = fh_resolve(fh, &p)) == FH_WAIT)
        fh_walk_step();
    return r == FH_OK ? p : NULL;
}
