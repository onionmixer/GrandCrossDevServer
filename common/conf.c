#include <stdio.h>
#include <string.h>

#include "gcdsp.h"
#include "conf.h"
#include "util.h"

static void trim(char *s)
{
    long i;
    long n;

    n = (long)strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        n--;
        s[n] = '\0';
    }
    i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    if (i > 0)
        memmove(s, s + i, (size_t)(n - i + 1));
}

long conf_load(const char *path, gcds_kv_t *kv, long maxkv)
{
    FILE *fp;
    char line[GCDSP_LINE_MAX];
    char *eq;
    long n;

    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;

    n = 0;
    while (n < maxkv && fgets(line, (int)sizeof(line), fp) != NULL) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        trim(line);
        trim(eq + 1);
        if (line[0] == '\0')
            continue;
        gcds_strlcpy(kv[n].key, line, CONF_KEY_MAX);
        gcds_strlcpy(kv[n].val, eq + 1, CONF_VAL_MAX);
        n++;
    }
    fclose(fp);
    return n;
}

const char *conf_get(const gcds_kv_t *kv, long n, const char *key)
{
    long i;

    for (i = 0; i < n; i++) {
        if (strcmp(kv[i].key, key) == 0)
            return kv[i].val;
    }
    return NULL;
}

const char *conf_gets(const gcds_kv_t *kv, long n, const char *key,
                      const char *dflt)
{
    const char *v;

    v = conf_get(kv, n, key);
    if (v == NULL)
        return dflt;
    return v;
}
