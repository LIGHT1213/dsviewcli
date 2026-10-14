/*
 * dslab — minimal JSON parser (DOM, read-only) for our own capture.json
 * and assert spec files. Not a general-purpose parser: no numbers beyond
 * double/uint64 precision guarantees, nesting capped at 16 levels.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_JSPARSE_H
#define DSLAB_JSPARSE_H

#include <stddef.h>

typedef enum { JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_ARR, JS_OBJ } js_type;

typedef struct js_val js_val;

struct js_val {
    js_type type;
    /* JS_BOOL / JS_NUM */
    double num;
    unsigned long long unum;
    /* JS_STR */
    char *str;
    /* JS_ARR / JS_OBJ: parallel arrays */
    js_val **items;   /* array elements or object values */
    char **keys;      /* object keys (NULL for arrays) */
    int count;
};

/* Parse a NUL-terminated JSON document. Returns NULL on error (err gets a
 * short message). Caller frees with js_free(). */
js_val *js_parse(const char *text, char *err, size_t errlen);
void js_free(js_val *v);

/* Object member lookup (NULL when missing). */
const js_val *js_get(const js_val *obj, const char *key);
/* Convenience typed getters (defaults when missing/type-mismatch). */
const char *js_get_str(const js_val *obj, const char *key, const char *dflt);
unsigned long long js_get_num(const js_val *obj, const char *key,
                              unsigned long long dflt);
double js_get_dbl(const js_val *obj, const char *key, double dflt);
int js_get_bool(const js_val *obj, const char *key, int dflt);

#endif
