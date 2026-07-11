/*
 * toolias.c - tool alias expansion (PLAN_04 section 4).
 * "gcds <host> @<name> args..." replaces @<name> with the value of
 * host.<host>.tool.<name>. Nothing fancier by design: no variable
 * substitution; complex logic belongs in remote-side scripts.
 */
#include <stdio.h>
#include <string.h>

#include "gcdsp.h"
#include "util.h"
#include "conf.h"
#include "remote.h"

/* 0 ok (out = alias command prefix), -1 unknown alias */
int tool_expand(const gcds_kv_t *kv, long nkv, const char *alias,
                const char *name, char *out, long size)
{
    char what[CONF_KEY_MAX];
    char key[CONF_KEY_MAX];
    const char *v;

    what[0] = '\0';
    gcds_strlcat(what, "tool.", (long)sizeof(what));
    gcds_strlcat(what, name, (long)sizeof(what));
    rc_key(key, CONF_KEY_MAX, alias, what);
    v = conf_get(kv, nkv, key);
    if (v == NULL) {
        fprintf(stderr, "gcds: no tool alias '%s' for host '%s' "
                "(missing %s)\n", name, alias, key);
        return -1;
    }
    gcds_strlcpy(out, v, size);
    return 0;
}
