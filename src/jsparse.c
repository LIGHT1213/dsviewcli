#include "jsparse.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#define JS_MAX_ITEMS 4096
#define JS_MAX_DEPTH 16

typedef struct {
    const char *p;
    char *err;
    size_t errlen;
    int depth;
    int failed;
} jp_ctx;

static js_val *jp_value(jp_ctx *c);

static void jp_fail(jp_ctx *c, const char *msg)
{
    if (!c->failed) {
        c->failed = 1;
        snprintf(c->err, c->errlen, "%s (near offset %ld)", msg,
                 (long)(c->p - (const char *)0));
    }
}

static void jp_ws(jp_ctx *c)
{
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')
        c->p++;
}

static js_val *jp_new(js_type t)
{
    js_val *v = calloc(1, sizeof(js_val));
    if (v)
        v->type = t;
    return v;
}

void js_free(js_val *v)
{
    if (!v)
        return;
    if (v->type == JS_STR)
        free(v->str);
    if (v->items) {
        for (int i = 0; i < v->count; i++) {
            js_free(v->items[i]);
            if (v->keys)
                free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
    }
    free(v);
}

static int jp_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static char *jp_string(jp_ctx *c)
{
    if (*c->p != '"') {
        jp_fail(c, "expected string");
        return NULL;
    }
    c->p++;
    size_t cap = 32, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;
#define PUTC(ch) do { \
    if (len + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); \
        if (!nb) { free(buf); return NULL; } buf = nb; } \
    buf[len++] = (char)(ch); \
} while (0)
    while (*c->p && *c->p != '"') {
        if (*c->p == '\\') {
            c->p++;
            switch (*c->p) {
            case '"': PUTC('"'); break;
            case '\\': PUTC('\\'); break;
            case '/': PUTC('/'); break;
            case 'b': PUTC('\b'); break;
            case 'f': PUTC('\f'); break;
            case 'n': PUTC('\n'); break;
            case 'r': PUTC('\r'); break;
            case 't': PUTC('\t'); break;
            case 'u': {
                unsigned cp;
                if (jp_hex4(c->p + 1, &cp) != 0) {
                    jp_fail(c, "bad \\u escape");
                    free(buf);
                    return NULL;
                }
                c->p += 4;
                /* encode UTF-8 (BMP only) */
                if (cp < 0x80) PUTC(cp);
                else if (cp < 0x800) { PUTC(0xC0 | (cp >> 6)); PUTC(0x80 | (cp & 0x3F)); }
                else { PUTC(0xE0 | (cp >> 12)); PUTC(0x80 | ((cp >> 6) & 0x3F)); PUTC(0x80 | (cp & 0x3F)); }
                break;
            }
            default:
                jp_fail(c, "bad escape");
                free(buf);
                return NULL;
            }
            c->p++;
        } else {
            PUTC(*c->p++);
        }
    }
    if (*c->p != '"') {
        jp_fail(c, "unterminated string");
        free(buf);
        return NULL;
    }
    c->p++;
    buf[len] = '\0';
    return buf;
#undef PUTC
}

static js_val *jp_number(jp_ctx *c)
{
    char *end = NULL;
    double d = strtod(c->p, &end);
    if (end == c->p) {
        jp_fail(c, "bad number");
        return NULL;
    }
    c->p = end;
    js_val *v = jp_new(JS_NUM);
    if (!v)
        return NULL;
    v->num = d;
    v->unum = (d >= 0) ? (unsigned long long)d : 0;
    return v;
}

static js_val *jp_value(jp_ctx *c)
{
    jp_ws(c);
    if (c->depth > JS_MAX_DEPTH) {
        jp_fail(c, "nesting too deep");
        return NULL;
    }
    switch (*c->p) {
    case '{': {
        c->p++;
        js_val *v = jp_new(JS_OBJ);
        if (!v) return NULL;
        c->depth++;
        jp_ws(c);
        if (*c->p == '}') { c->p++; c->depth--; return v; }
        while (1) {
            jp_ws(c);
            char *key = jp_string(c);
            if (!key) { js_free(v); return NULL; }
            jp_ws(c);
            if (*c->p != ':') { jp_fail(c, "expected ':'"); free(key); js_free(v); return NULL; }
            c->p++;
            js_val *val = jp_value(c);
            if (!val) { free(key); js_free(v); return NULL; }
            if (v->count >= JS_MAX_ITEMS) { jp_fail(c, "too many items"); free(key); js_free(val); js_free(v); return NULL; }
            js_val **ni = realloc(v->items, sizeof(js_val *) * (size_t)(v->count + 1));
            char **nk = realloc(v->keys, sizeof(char *) * (size_t)(v->count + 1));
            if (!ni || !nk) {
                free(key);
                js_free(val);
                js_free(v);
                free(ni);
                free(nk);
                return NULL;
            }
            v->items = ni;
            v->keys = nk;
            v->items[v->count] = val;
            v->keys[v->count] = key;
            v->count++;
            jp_ws(c);
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == '}') { c->p++; c->depth--; return v; }
            jp_fail(c, "expected ',' or '}'");
            js_free(v);
            return NULL;
        }
    }
    case '[': {
        c->p++;
        js_val *v = jp_new(JS_ARR);
        if (!v) return NULL;
        c->depth++;
        jp_ws(c);
        if (*c->p == ']') { c->p++; c->depth--; return v; }
        while (1) {
            js_val *val = jp_value(c);
            if (!val) { js_free(v); return NULL; }
            if (v->count >= JS_MAX_ITEMS) { jp_fail(c, "too many items"); js_free(val); js_free(v); return NULL; }
            js_val **ni = realloc(v->items, sizeof(js_val *) * (size_t)(v->count + 1));
            if (!ni) { js_free(val); js_free(v); return NULL; }
            v->items = ni;
            v->items[v->count++] = val;
            jp_ws(c);
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == ']') { c->p++; c->depth--; return v; }
            jp_fail(c, "expected ',' or ']'");
            js_free(v);
            return NULL;
        }
    }
    case '"': {
        char *s = jp_string(c);
        if (!s) return NULL;
        js_val *v = jp_new(JS_STR);
        if (!v) { free(s); return NULL; }
        v->str = s;
        return v;
    }
    case 't':
        if (!strncmp(c->p, "true", 4)) { c->p += 4; js_val *v = jp_new(JS_BOOL); if (v) v->num = 1; return v; }
        jp_fail(c, "bad literal");
        return NULL;
    case 'f':
        if (!strncmp(c->p, "false", 5)) { c->p += 5; js_val *v = jp_new(JS_BOOL); if (v) v->num = 0; return v; }
        jp_fail(c, "bad literal");
        return NULL;
    case 'n':
        if (!strncmp(c->p, "null", 4)) { c->p += 4; return jp_new(JS_NULL); }
        jp_fail(c, "bad literal");
        return NULL;
    default:
        if (*c->p == '-' || (*c->p >= '0' && *c->p <= '9'))
            return jp_number(c);
        jp_fail(c, "unexpected character");
        return NULL;
    }
}

js_val *js_parse(const char *text, char *err, size_t errlen)
{
    jp_ctx c = { text, err, errlen, 0, 0 };
    err[0] = '\0';
    js_val *v = jp_value(&c);
    if (!v)
        return NULL;
    jp_ws(&c);
    if (*c.p != '\0') {
        js_free(v);
        snprintf(err, errlen, "trailing content");
        return NULL;
    }
    return v;
}

const js_val *js_get(const js_val *obj, const char *key)
{
    if (!obj || (obj->type != JS_OBJ && obj->type != JS_ARR))
        return NULL;
    if (obj->type == JS_ARR) {
        /* numeric key = index */
        int idx = atoi(key);
        if (idx >= 0 && idx < obj->count)
            return obj->items[idx];
        return NULL;
    }
    for (int i = 0; i < obj->count; i++)
        if (!strcmp(obj->keys[i], key))
            return obj->items[i];
    return NULL;
}

const char *js_get_str(const js_val *obj, const char *key, const char *dflt)
{
    const js_val *v = js_get(obj, key);
    return (v && v->type == JS_STR) ? v->str : dflt;
}

unsigned long long js_get_num(const js_val *obj, const char *key,
                              unsigned long long dflt)
{
    const js_val *v = js_get(obj, key);
    return (v && v->type == JS_NUM) ? v->unum : dflt;
}

double js_get_dbl(const js_val *obj, const char *key, double dflt)
{
    const js_val *v = js_get(obj, key);
    return (v && v->type == JS_NUM) ? v->num : dflt;
}

int js_get_bool(const js_val *obj, const char *key, int dflt)
{
    const js_val *v = js_get(obj, key);
    return (v && v->type == JS_BOOL) ? (int)v->num : dflt;
}
