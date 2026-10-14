#include "trigger.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

char dslab_trig_word_to_char(const char *word)
{
    char buf[16];
    size_t n = 0;
    for (const char *p = word; *p && n < sizeof(buf) - 1; p++)
        buf[n++] = (char)tolower((unsigned char)*p);
    buf[n] = '\0';
    if (!strcmp(buf, "r") || !strcmp(buf, "rise") || !strcmp(buf, "rising"))
        return 'R';
    if (!strcmp(buf, "f") || !strcmp(buf, "fall") || !strcmp(buf, "falling"))
        return 'F';
    if (!strcmp(buf, "c") || !strcmp(buf, "change") || !strcmp(buf, "edge") ||
        !strcmp(buf, "any") || !strcmp(buf, "toggle"))
        return 'C';
    if (!strcmp(buf, "1") || !strcmp(buf, "high") || !strcmp(buf, "h"))
        return '1';
    if (!strcmp(buf, "0") || !strcmp(buf, "low") || !strcmp(buf, "l"))
        return '0';
    return 0;
}

int dslab_trig_parse(const char *spec, dslab_trig_spec *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    out->position_pct = 0;
    if (!spec || !*spec) {
        snprintf(err, errlen, "empty trigger spec");
        return -1;
    }
    const char *p = spec;
    while (*p) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        /* channel part */
        if ((*p != 'd' && *p != 'D') || !isdigit((unsigned char)p[1])) {
            snprintf(err, errlen, "expected channel (D0..D31) at '%.8s'", p);
            return -1;
        }
        p++;
        char *end = NULL;
        long ch = strtol(p, &end, 10);
        if (end == p || ch < 0 || ch > 31) {
            snprintf(err, errlen, "bad channel number at '%.8s'", p);
            return -1;
        }
        p = end;
        if (*p != ':') {
            snprintf(err, errlen, "missing ':' after D%ld", ch);
            return -1;
        }
        p++;
        const char *wstart = p;
        while (*p && *p != ',')
            p++;
        size_t wlen = (size_t)(p - wstart);
        char word[24];
        if (wlen == 0 || wlen >= sizeof(word)) {
            snprintf(err, errlen, "bad trigger condition for D%ld", ch);
            return -1;
        }
        memcpy(word, wstart, wlen);
        word[wlen] = '\0';
        char tc = dslab_trig_word_to_char(word);
        if (!tc) {
            snprintf(err, errlen, "unknown condition '%s' (use rising|falling|"
                     "change|high|low or r/f/c/1/0)", word);
            return -1;
        }
        for (int i = 0; i < out->count; i++)
            if (out->items[i].ch_index == (uint8_t)ch) {
                snprintf(err, errlen, "duplicate trigger for D%ld", ch);
                return -1;
            }
        if (out->count >= DSLAB_TRIG_MAX) {
            snprintf(err, errlen, "too many trigger items");
            return -1;
        }
        out->items[out->count].ch_index = (uint8_t)ch;
        out->items[out->count].trig_char = tc;
        out->count++;
    }
    if (out->count == 0) {
        snprintf(err, errlen, "no trigger condition found");
        return -1;
    }
    return 0;
}
