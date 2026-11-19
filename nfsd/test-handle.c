/*
 * test-handle.c - deterministic tests for the handle table (handle.c).
 *
 *   make test-handle && ./test-handle
 *
 * handle.c is #included so its static functions and table are visible.
 * Every index lookup is cross-checked against a plain linear scan - the
 * pre-index implementation - so the index must give the same answers,
 * including which entry wins when two share a (hi, lo) hash. A real
 * duplicate key comes from the "/" export alias: "//x" and "/x" have
 * the same relative path "x" but are different interned strings.
 * Allocation failure is injected through malloc/realloc wrappers. The
 * incremental export walk is run over a small real tree in /tmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

static long g_allocs;          /* allocations seen                   */
static long g_fail_at = -1;    /* fail the allocation with this number */

static void *t_malloc(size_t n)
{
    if (g_allocs++ == g_fail_at)
        return NULL;
    return malloc(n);
}

static void *t_realloc(void *p, size_t n)
{
    if (g_allocs++ == g_fail_at)
        return NULL;
    return realloc(p, n);
}

#define malloc  t_malloc
#define realloc t_realloc
#include "handle.c"
#undef malloc
#undef realloc

static int g_fail;

#define CHECK(cond, what) do { if (!(cond)) { \
    printf("  FAIL %s (line %d)\n", what, __LINE__); g_fail++; } } while (0)

/* the old implementation, as the reference */
static long ref_by_path(const char *p)
{
    long i;
    for (i = 0; i < g_count; i++)
        if (strcmp(g_paths[i], p) == 0)
            return i;
    return -1;
}

static long ref_by_hash(unsigned long hi, unsigned long lo)
{
    long i;
    for (i = 0; i < g_count; i++)
        if (g_hi[i] == hi && g_lo[i] == lo)
            return i;
    return -1;
}

static void reset(const char *root)
{
    long i;
    for (i = 0; i < g_count; i++)
        free(g_paths[i]);
    free(g_paths); free(g_hi); free(g_lo); free(g_slot);
    g_paths = NULL; g_hi = g_lo = NULL; g_slot = NULL;
    g_count = g_cap = 0; g_nslot = 0;
    while (g_wtop > 0)                 /* a "restart": no walk, no table */
        closedir(g_wstack[--g_wtop].d);
    g_walk = WALK_IDLE;
    g_rootlen = 0; g_root[0] = '\0';
    fh_set_root(root);
}

/* the index itself: occupied slots == entries, each index in range and
   present exactly once; then every entry: index lookup == reference */
static int table_consistent(void)
{
    long i, used = 0;
    unsigned long s;
    unsigned char *seen = (unsigned char *)calloc((size_t)g_count + 1, 1);

    for (s = 0; s < g_nslot; s++) {
        i = g_slot[s];
        if (i < 0)
            continue;
        if (i >= g_count || seen[i]) {
            free(seen);
            return 0;
        }
        seen[i] = 1;
        used++;
    }
    free(seen);
    if (used != g_count)
        return 0;
    for (i = 0; i < g_count; i++) {
        if (find_hash(g_hi[i], g_lo[i]) != ref_by_hash(g_hi[i], g_lo[i]))
            return 0;
        if (intern(g_paths[i]) != i)
            return 0;
    }
    return 1;
}

static unsigned long g_rng = 12345;
static unsigned long rnd(void)
{
    g_rng = (g_rng * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
    return g_rng;
}

static void test_random_paths(void)
{
    char p[256];
    long i, n = 30000, before;
    const char *keep;

    printf("random paths vs linear reference\n");
    reset("/export/root");
    for (i = 0; i < n; i++) {
        sprintf(p, "/export/root/d%lu/s%lu/f%lu.c", rnd() % 97, rnd() % 31,
                rnd() % 4000);
        before = ref_by_path(p);
        CHECK(intern(p) == (before >= 0 ? before : g_count - 1),
              "intern == reference");
    }
    CHECK(g_count > 20000, "enough distinct entries");
    CHECK(table_consistent(), "index consistent after growth");
    keep = g_paths[0];
    for (i = 0; i < 40000; i++) {
        sprintf(p, "/export/root/more/%ld", i);
        intern(p);
    }
    CHECK(g_paths[0] == keep, "interned pointer stable across growth");
    CHECK(g_nslot >= (unsigned long)g_count * 2, "load factor <= 1/2");
    printf("  %ld entries, %lu slots\n", g_count, g_nslot);
}

static void test_alias_dupkey(void)
{
    char a[64], b[64];
    long i, ia, ib, k;

    printf("duplicate (hi, lo) via \"/\" export alias\n");
    for (k = 0; k < 2; k++) {
        reset("/");
        for (i = 0; i < 1500; i++) {      /* straddles several rehashes */
            sprintf(a, "//alias%ld", i);
            sprintf(b, "/alias%ld", i);
            if (k == 0) { ia = intern(a); ib = intern(b); }
            else        { ib = intern(b); ia = intern(a); }
            CHECK(ia != ib, "alias strings are distinct entries");
            CHECK(g_hi[ia] == g_hi[ib] && g_lo[ia] == g_lo[ib],
                  "alias shares (hi, lo)");
        }
        CHECK(table_consistent(), "lowest index wins, before and after rehash");
    }
}

static void test_handles(void)
{
    unsigned char fh[NFS_FHSIZE], bad[NFS_FHSIZE];
    const char *p;
    long i;
    char path[64];

    printf("handle round trip and fast-path checks\n");
    reset("/export");
    for (i = 0; i < 1000; i++) {
        sprintf(path, "/export/f%ld", i);
        CHECK(fh_from_path(path, fh) == 0, "fh_from_path");
        p = fh_to_path(fh);
        CHECK(p != NULL && strcmp(p, path) == 0, "round trip");
    }
    /* index 7 with a forged verifier but another entry's (hi, lo):
       the fast path must not accept it on the verifier alone */
    fh_from_path("/export/f7", bad);
    fh_from_path("/export/f8", fh);
    memcpy(bad + 12, fh + 12, 8);
    p = fh_to_path(bad);
    CHECK(p != NULL && strcmp(p, "/export/f8") == 0,
          "fast path rejects mismatched (hi, lo), slow path resolves");
    /* index out of range (signed-compare trap on ILP32) */
    fh_from_path("/export/f9", fh);
    fh[0] = 0xFF; fh[1] = 0xFF; fh[2] = 0xFF; fh[3] = 0xF0;
    p = fh_to_path(fh);
    CHECK(p != NULL && strcmp(p, "/export/f9") == 0, "huge index falls to hash");
    /* old-format handle (hi, lo) = 0: fast path only */
    fh_from_path("/export/f3", fh);
    memset(fh + 12, 0, 8);
    p = fh_to_path(fh);
    CHECK(p != NULL && strcmp(p, "/export/f3") == 0, "old format, fast path");
    fh[0] = 0x00; fh[1] = 0x00; fh[2] = 0x7F; fh[3] = 0x00;
    CHECK(fh_to_path(fh) == NULL, "old format, wrong index -> stale");
    fh[4] ^= 1;
    CHECK(fh_to_path(fh) == NULL, "bad magic");
}

static void test_alloc_failure(void)
{
    char p[64];
    long i, k, n0, ok, failed = 0;

    printf("allocation failure leaves the table intact\n");
    for (k = 0; k < 400; k++) {
        reset("/r");
        g_fail_at = -1;
        for (i = 0; i < 700; i++) {   /* crosses the 64 and 512 boundaries */
            sprintf(p, "/r/x%ld", i);
            intern(p);
        }
        n0 = g_count;
        /* fail the k-th allocation from here on while adding more */
        g_allocs = 0;
        g_fail_at = k;
        for (i = 700; i < 1100; i++) {
            sprintf(p, "/r/x%ld", i);
            if (intern(p) < 0) {
                failed++;
                break;
            }
        }
        g_fail_at = -1;
        CHECK(g_count >= n0, "no entry lost");
        CHECK(table_consistent(), "every entry findable after failure");
        sprintf(p, "/r/after");
        ok = intern(p);
        CHECK(ok == g_count - 1 && intern(p) == ok, "recovers after failure");
    }
    CHECK(failed > 0, "some injected failures were hit");
    printf("  %ld injected failures\n", failed);
}

/* ---- incremental walk over a real tree ---- */

static char g_tree[64];

static void touch(const char *p)
{
    FILE *f = fopen(p, "w");
    if (f != NULL)
        fclose(f);
}

static int open_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL)
        return -1;
    while (readdir(d) != NULL)
        n++;
    closedir(d);
    return n;
}

/* tree: 6 dirs x 1000 files, a depth-40 chain, a symlink loop */
static void make_tree(void)
{
    char p[1024];
    int i, j;

    strcpy(g_tree, "/tmp/gn-th-XXXXXX");
    if (mkdtemp(g_tree) == NULL) {
        printf("  cannot create temp tree\n");
        exit(1);
    }
    for (i = 0; i < 6; i++) {
        sprintf(p, "%s/d%d", g_tree, i);
        mkdir(p, 0755);
        for (j = 0; j < 1000; j++) {
            sprintf(p, "%s/d%d/f%d", g_tree, i, j);
            touch(p);
        }
    }
    strcpy(p, g_tree);
    for (i = 1; i <= 40; i++) {        /* entry at depth i is "c" x i */
        strcat(p, "/c");
        mkdir(p, 0755);
    }
    sprintf(p, "%s/d0/loop", g_tree);
    if (symlink("..", p) != 0)          /* a loop the walk must not follow */
        printf("  (no symlink: loop case not covered)\n");
}

static void chain_path(char *p, int depth)
{
    int i;
    strcpy(p, g_tree);
    for (i = 0; i < depth; i++)
        strcat(p, "/c");
}

static void test_walk(void)
{
    unsigned char old5[NFS_FHSIZE], olddeep[NFS_FHSIZE], d33[NFS_FHSIZE],
                  d34[NFS_FHSIZE], gone[NFS_FHSIZE], fresh[NFS_FHSIZE];
    char p[1024], q[1024];
    const char *r;
    long steps = 0, before, maxstep = 0;
    int fds;

    printf("incremental walk over a real tree\n");
    make_tree();
    /* a previous server lifetime hands out handles ... */
    reset(g_tree);
    sprintf(p, "%s/d5/f999", g_tree);  fh_from_path(p, old5);
    chain_path(q, 30);                  fh_from_path(q, olddeep);
    chain_path(q, 33);                  fh_from_path(q, d33);
    chain_path(q, 34);                  fh_from_path(q, d34);
    sprintf(p, "%s/d1/gone", g_tree);   touch(p); fh_from_path(p, gone);
    unlink(p);
    /* ... then it restarts */
    fds = open_fds();
    reset(g_tree);
    CHECK(fh_resolve(old5, &r) == FH_WAIT, "unknown old handle -> WAIT");
    CHECK(fh_walk_pending(), "walk started by it");
    CHECK(fh_resolve(old5, &r) == FH_WAIT, "still WAIT before any step");
    /* a new client mounts meanwhile: new handles resolve at once */
    sprintf(p, "%s/d2/f7", g_tree);
    CHECK(fh_from_path(p, fresh) == 0 && fh_resolve(fresh, &r) == FH_OK &&
          strcmp(r, p) == 0, "new handle resolves during the walk");
    while (fh_walk_pending()) {
        before = g_count;
        fh_walk_step();
        if (g_count - before > maxstep)
            maxstep = g_count - before;
        steps++;
    }
    printf("  %ld entries in %ld slices (max %ld per slice)\n",
           g_count, steps, maxstep);
    CHECK(steps > 1, "walk took several slices");
    CHECK(maxstep <= WALK_SLICE_ENTRIES, "slice bounded");
    CHECK(g_walk == WALK_DONE, "walk done");
    sprintf(p, "%s/d5/f999", g_tree);
    CHECK(fh_resolve(old5, &r) == FH_OK && strcmp(r, p) == 0,
          "old handle resolves after the walk");
    chain_path(q, 30);
    CHECK(fh_resolve(olddeep, &r) == FH_OK && strcmp(r, q) == 0,
          "deep old handle resolves");
    CHECK(fh_resolve(d33, &r) == FH_OK, "depth 33 reached (as before)");
    CHECK(fh_resolve(d34, &r) == FH_STALE, "depth 34 not walked (as before)");
    CHECK(fh_resolve(gone, &r) == FH_STALE, "deleted path -> STALE");
    CHECK(g_wtop == 0 && open_fds() == fds, "every DIR closed");
    CHECK(table_consistent(), "index consistent after the walk");

    printf("walk that runs out of memory\n");
    reset(g_tree);
    g_allocs = 0;
    g_fail_at = 3000;                   /* somewhere inside the walk */
    CHECK(fh_resolve(old5, &r) == FH_WAIT, "WAIT while walking");
    while (fh_walk_pending())
        fh_walk_step();
    g_fail_at = -1;
    CHECK(g_walk == WALK_FAILED, "walk FAILED, not done");
    /* whether the partial walk reached it depends on readdir order;
       what must never happen is a WAIT that no walk will ever end */
    CHECK(fh_resolve(olddeep, &r) != FH_WAIT, "no WAIT after a failed walk");
    CHECK(fh_resolve(d34, &r) == FH_STALE, "no second walk");
    CHECK(g_wtop == 0 && open_fds() == fds, "every DIR closed on failure");
    CHECK(table_consistent(), "partial table consistent");

    sprintf(p, "rm -rf '%s'", g_tree);
    if (system(p) != 0)
        printf("  (could not remove %s)\n", g_tree);
}

int main(void)
{
    /* the walk logs to stderr, including the "walk FAILED" this test
       provokes on purpose - keep that from reading as a test failure */
    if (freopen("/dev/null", "w", stderr) == NULL)
        return 1;
    test_random_paths();
    test_alias_dupkey();
    test_handles();
    test_alloc_failure();
    test_walk();
    if (g_fail) {
        printf("== %d FAIL ==\n", g_fail);
        return 1;
    }
    printf("== ALL PASS ==\n");
    return 0;
}
