/*
 * conf.h - key=value config parser (PLAN_00 D5).
 * '#' comments and blank lines allowed; no quoting, no escapes.
 * Whitespace around key and '=' is trimmed; the value keeps
 * everything after the first '=' (trimmed at both ends).
 */
#ifndef CONF_H
#define CONF_H

#define CONF_KEY_MAX 64
/* map.<n> values carry two paths ("local|remote"), so this must
   hold roughly twice a worst-case path */
#define CONF_VAL_MAX 384
/* DOS large model: keep g_kv (ENT_MAX * ~448B) small enough to fit
   the 64K DGROUP alongside the other statics - even tighter when
   Watt-32's TCP data is also linked (PLAN_02 4). A DOS daemon
   config has only a handful of keys. */
#ifdef GCDS_DOS
#define CONF_ENT_MAX 12
#else
#define CONF_ENT_MAX 64
#endif

typedef struct {
    char key[CONF_KEY_MAX];
    char val[CONF_VAL_MAX];
} gcds_kv_t;

/* returns entry count, or -1 if the file cannot be read */
long conf_load(const char *path, gcds_kv_t *kv, long maxkv);

/* NULL if absent */
const char *conf_get(const gcds_kv_t *kv, long n, const char *key);

/* conf_get with a default */
const char *conf_gets(const gcds_kv_t *kv, long n, const char *key,
                      const char *dflt);

#endif /* CONF_H */
