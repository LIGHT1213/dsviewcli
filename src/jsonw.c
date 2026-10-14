#include "jsonw.h"

#include <string.h>

static void jw_print_escaped(JsonW *w, const char *s)
{
    fputc('"', w->out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", w->out); break;
        case '\\': fputs("\\\\", w->out); break;
        case '\b': fputs("\\b", w->out); break;
        case '\f': fputs("\\f", w->out); break;
        case '\n': fputs("\\n", w->out); break;
        case '\r': fputs("\\r", w->out); break;
        case '\t': fputs("\\t", w->out); break;
        default:
            if (*p < 0x20)
                fprintf(w->out, "\\u%04x", *p);
            else
                fputc(*p, w->out);
        }
    }
    fputc('"', w->out);
}

/* Separator before an element: no comma right after "key":, comma between
 * siblings inside the same container. */
static void jw_pre(JsonW *w)
{
    if (w->after_key) {
        w->after_key = 0;
        return;
    }
    if (w->depth > 0) {
        if (w->started[w->depth - 1])
            fputc(',', w->out);
        else
            w->started[w->depth - 1] = 1;
    }
}

static void jw_open(JsonW *w, char c)
{
    jw_pre(w);
    fputc(c, w->out);
    if (w->depth < JW_MAX_DEPTH) {
        w->started[w->depth] = 0;
        w->depth++;
    }
}

void jw_init(JsonW *w, FILE *out)
{
    memset(w, 0, sizeof(*w));
    w->out = out;
}

void jw_obj(JsonW *w) { jw_open(w, '{'); }

void jw_obj_end(JsonW *w)
{
    fputc('}', w->out);
    if (w->depth > 0)
        w->depth--;
}

void jw_arr(JsonW *w) { jw_open(w, '['); }

void jw_arr_end(JsonW *w)
{
    fputc(']', w->out);
    if (w->depth > 0)
        w->depth--;
}

void jw_key(JsonW *w, const char *name)
{
    jw_pre(w);
    jw_print_escaped(w, name);
    fputc(':', w->out);
    w->after_key = 1;
}

void jw_str(JsonW *w, const char *v)
{
    jw_pre(w);
    jw_print_escaped(w, v ? v : "");
}

void jw_int(JsonW *w, long long v)
{
    jw_pre(w);
    fprintf(w->out, "%lld", v);
}

void jw_uint(JsonW *w, unsigned long long v)
{
    jw_pre(w);
    fprintf(w->out, "%llu", v);
}

void jw_dbl(JsonW *w, double v)
{
    jw_pre(w);
    if (v != v || v - v != 0.0)
        fputs("null", w->out);
    else
        fprintf(w->out, "%.17g", v);
}

void jw_bool(JsonW *w, int v)
{
    jw_pre(w);
    fputs(v ? "true" : "false", w->out);
}

void jw_null(JsonW *w)
{
    jw_pre(w);
    fputs("null", w->out);
}

void jw_key_obj(JsonW *w, const char *name) { jw_key(w, name); jw_open(w, '{'); }
void jw_key_arr(JsonW *w, const char *name) { jw_key(w, name); jw_open(w, '['); }
void jw_key_end(JsonW *w) { jw_obj_end(w); }

void jw_key_str(JsonW *w, const char *name, const char *v) { jw_key(w, name); jw_str(w, v); }
void jw_key_int(JsonW *w, const char *name, long long v) { jw_key(w, name); jw_int(w, v); }
void jw_key_uint(JsonW *w, const char *name, unsigned long long v) { jw_key(w, name); jw_uint(w, v); }
void jw_key_dbl(JsonW *w, const char *name, double v) { jw_key(w, name); jw_dbl(w, v); }
void jw_key_bool(JsonW *w, const char *name, int v) { jw_key(w, name); jw_bool(w, v); }
