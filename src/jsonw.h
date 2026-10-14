/*
 * dslab — minimal streaming JSON writer (stdout stays machine-parseable).
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_JSONW_H
#define DSLAB_JSONW_H

#include <stdio.h>

#define JW_MAX_DEPTH 16

typedef struct {
    FILE *out;
    int depth;                 /* current container depth */
    char started[JW_MAX_DEPTH];/* element already printed at this depth */
    int after_key;             /* last output was "key":, next value gets no comma */
} JsonW;

void jw_init(JsonW *w, FILE *out);
void jw_obj(JsonW *w);          /* { */
void jw_obj_end(JsonW *w);      /* } */
void jw_arr(JsonW *w);          /* [ */
void jw_arr_end(JsonW *w);      /* ] */
void jw_key(JsonW *w, const char *name); /* "name": */
void jw_key_obj(JsonW *w, const char *name); /* "name":{ */
void jw_key_arr(JsonW *w, const char *name); /* "name":[ */
void jw_key_end(JsonW *w);                 /* } */
void jw_str(JsonW *w, const char *v);
void jw_int(JsonW *w, long long v);
void jw_uint(JsonW *w, unsigned long long v);
void jw_dbl(JsonW *w, double v);
void jw_bool(JsonW *w, int v);
void jw_null(JsonW *w);
void jw_key_str(JsonW *w, const char *name, const char *v);
void jw_key_int(JsonW *w, const char *name, long long v);
void jw_key_uint(JsonW *w, const char *name, unsigned long long v);
void jw_key_dbl(JsonW *w, const char *name, double v);
void jw_key_bool(JsonW *w, const char *name, int v);

#endif
