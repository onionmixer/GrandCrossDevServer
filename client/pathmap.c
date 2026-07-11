/*
 * pathmap.c - local cwd -> remote CWD translation.
 * Spec: PLAN_05 section 2.
 *   host.<alias>.map.<n> = <local-prefix>|<remote-prefix>[|<sep>]
 * Component-boundary matching, longest local prefix wins,
 * trailing '/' on the local prefix ignored, remainder separators
 * converted to <sep> (default: '\' if the remote prefix contains
 * one, else '/'), no doubled separator at the junction.
 */
#include <string.h>
#include <unistd.h>

#include "gcdsp.h"
#include "util.h"
#include "conf.h"
#include "remote.h"

#define PM_MAX 8

/* prefix length after stripping trailing slashes (keeps >= 1) */
static long pfx_norm_len(const char *pfx)
{
    long n;

    n = (long)strlen(pfx);
    while (n > 1 && pfx[n - 1] == '/')
        n--;
    return n;
}

/* component-boundary prefix match; returns matched length or -1 */
static long pfx_match(const char *cwd, const char *pfx)
{
    long n;

    n = pfx_norm_len(pfx);
    if (n <= 0 || strncmp(cwd, pfx, (size_t)n) != 0)
        return -1;
    if (cwd[n] != '\0' && cwd[n] != '/')
        return -1;      /* /mnt/proj must not match /mnt/project */
    return n;
}

/* build out = remote + rest ('/' -> sep), avoiding a doubled
   separator at the junction; 0 ok, -1 overflow */
static int pm_build(char *out, long size, const char *remote,
                    const char *rest, char sep)
{
    long n;
    long j;

    if (gcds_strlcpy(out, remote, size) >= size)
        return -1;
    n = (long)strlen(out);

    /* rest starts with '/' (or is empty); if remote already ends
       with the separator, drop the leading one from rest */
    if (rest[0] == '/' && n > 0 && out[n - 1] == sep)
        rest++;

    for (j = 0; rest[j] != '\0'; j++) {
        if (n >= size - 1)
            return -1;
        out[n] = (rest[j] == '/') ? sep : rest[j];
        n++;
    }
    out[n] = '\0';
    return 0;
}

/* ---- reverse mapping for output lines (PLAN_05 section 4) ---- */

static int ci_eq(char a, char b)
{
    if (a >= 'A' && a <= 'Z')
        a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
        b = (char)(b - 'A' + 'a');
    return a == b;
}

/* match needle at hay position; ci = case-insensitive.
   returns needle length on match, -1 otherwise */
static long at_match(const char *hay, const char *needle, int ci)
{
    long i;

    for (i = 0; needle[i] != '\0'; i++) {
        if (hay[i] == '\0')
            return -1;
        if (ci ? !ci_eq(hay[i], needle[i]) : hay[i] != needle[i])
            return -1;
    }
    return i;
}

/* replace remote-prefix occurrences in one text line with the
   local prefix, converting the token's separators back to '/'.
   sep ':' (classic Mac) only replaces the prefix - colon doubles
   as a plain character (file.c:10:) so token conversion is off. */
int pm_back_line(const gcds_kv_t *kv, long nkv, const char *alias,
                 char *line, long size)
{
    static char maps[PM_MAX][CONF_VAL_MAX];
    static char out[GCDSP_LINE_MAX * 2];
    char key[CONF_KEY_MAX];
    char what[24];
    const char *v;
    long nmap;
    long i;
    long li;
    long oi;

    nmap = 0;
    for (i = 1; i <= PM_MAX; i++) {
        what[0] = '\0';
        gcds_strlcat(what, "map.", (long)sizeof(what));
        what[4] = (char)('0' + i);
        what[5] = '\0';
        rc_key(key, CONF_KEY_MAX, alias, what);
        v = conf_get(kv, nkv, key);
        if (v != NULL && strchr(v, '|') != NULL) {
            gcds_strlcpy(maps[nmap], v, CONF_VAL_MAX);
            nmap++;
        }
    }
    if (nmap == 0)
        return 0;

    li = 0;
    oi = 0;
    while (line[li] != '\0' && oi < (long)sizeof(out) - 2) {
        long m;
        long rlen;
        char *local;
        char *remote;
        char *sepf;
        char sep;

        rlen = -1;
        local = NULL;
        sep = '/';
        for (m = 0; m < nmap; m++) {
            static char ent[CONF_VAL_MAX];

            gcds_strlcpy(ent, maps[m], CONF_VAL_MAX);
            local = ent;
            remote = strchr(ent, '|');
            *remote = '\0';
            remote++;
            sepf = strchr(remote, '|');
            if (sepf != NULL) {
                *sepf = '\0';
                sep = sepf[1];
            } else {
                sep = (strchr(remote, '\\') != NULL) ? '\\' : '/';
            }
            rlen = at_match(line + li, remote, sep == '\\');
            if (rlen > 0) {
                /* emit local prefix (trailing '/' stripped) */
                long ll;
                long j;

                ll = (long)strlen(local);
                while (ll > 1 && local[ll - 1] == '/')
                    ll--;
                for (j = 0; j < ll &&
                     oi < (long)sizeof(out) - 2; j++)
                    out[oi++] = local[j];
                li += rlen;
                /* convert token separators back to '/' */
                if (sep == '\\') {
                    while (line[li] != '\0' && line[li] != ' ' &&
                           line[li] != '\t' && line[li] != '"' &&
                           oi < (long)sizeof(out) - 2) {
                        out[oi++] = (line[li] == '\\')
                                    ? '/' : line[li];
                        li++;
                    }
                }
                break;
            }
        }
        if (rlen <= 0) {
            out[oi++] = line[li];
            li++;
        }
    }
    out[oi] = '\0';
    gcds_strlcpy(line, out, size);
    return 1;
}

/* returns 1 mapped (out filled), 0 no mapping, -1 error */
int pm_cwd(const gcds_kv_t *kv, long nkv, const char *alias,
           char *out, long size)
{
    char cwd[512];
    char key[CONF_KEY_MAX];
    char what[24];
    static char map[CONF_VAL_MAX];
    static char best[CONF_VAL_MAX];
    const char *v;
    long i;
    long m;
    long bestlen;

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        return 0;

    /* longest matching local prefix wins (PLAN_05 section 2) */
    bestlen = -1;
    for (i = 1; i <= PM_MAX; i++) {
        what[0] = '\0';
        gcds_strlcat(what, "map.", (long)sizeof(what));
        what[4] = (char)('0' + i);
        what[5] = '\0';
        rc_key(key, CONF_KEY_MAX, alias, what);
        v = conf_get(kv, nkv, key);
        if (v == NULL)
            continue;
        gcds_strlcpy(map, v, CONF_VAL_MAX);
        {
            char *bar;

            bar = strchr(map, '|');
            if (bar == NULL)
                continue;
            *bar = '\0';
        }
        m = pfx_match(cwd, map);
        if (m > bestlen) {
            bestlen = m;
            gcds_strlcpy(best, v, CONF_VAL_MAX);
        }
    }
    if (bestlen < 0)
        return 0;

    {
        char *remote;
        char *sepf;
        char sep;

        remote = strchr(best, '|');
        *remote = '\0';
        remote++;
        sepf = strchr(remote, '|');
        if (sepf != NULL) {
            *sepf = '\0';
            sep = sepf[1];
            if (sep == '\0')
                sep = '/';
        } else {
            sep = (strchr(remote, '\\') != NULL) ? '\\' : '/';
        }
        if (pm_build(out, size, remote, cwd + bestlen, sep) < 0)
            return -1;
    }
    return 1;
}
