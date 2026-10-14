/*
 * dslab — trigger specification parsing (M2).
 * Maps a CLI spec like "D0:rising,D1:low" onto libsigrok4DSL's simple
 * trigger chars ('R','F','C','1','0','X' — see upstream logicsignal.cpp
 * commit_trig() and trigger.c ds_trigger_probe_set()).
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_TRIGGER_H
#define DSLAB_TRIGGER_H

#include <stdint.h>

#define DSLAB_TRIG_MAX 32

typedef struct {
    uint8_t ch_index;    /* 0..31 */
    char trig_char;      /* 'R','F','C','1','0' */
} dslab_trig_item;

typedef struct {
    dslab_trig_item items[DSLAB_TRIG_MAX];
    int count;
    int position_pct;    /* pre-trigger capture percent 0..100 */
} dslab_trig_spec;

/* Parse "D0:rising,D1:f" (words: rising|fall|falling|high|1|low|0|change|c,
 * case-insensitive). Returns 0 on success, -1 on error (message in err). */
int dslab_trig_parse(const char *spec, dslab_trig_spec *out, char *err, size_t errlen);

/* Word -> library char, or 0 when unknown. */
char dslab_trig_word_to_char(const char *word);

#endif
