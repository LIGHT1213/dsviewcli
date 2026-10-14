#include "runfile.h"
#include "unpack.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int run_load(const char *dir, dslab_run *r, char *err, size_t errlen)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->dir, sizeof(r->dir), "%s", dir);

    char path[700];
    snprintf(path, sizeof(path), "%s/capture.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)sz + 1);
    if (!text || fread(text, 1, (size_t)sz, f) != (size_t)sz) {
        snprintf(err, errlen, "cannot read %s", path);
        free(text);
        fclose(f);
        return -1;
    }
    text[sz] = '\0';
    fclose(f);
    r->meta = js_parse(text, err, errlen);
    free(text);
    if (!r->meta)
        return -1;

    const js_val *cap = js_get(r->meta, "capture");
    const js_val *fmt = js_get(r->meta, "sample_format");
    const js_val *chans = js_get(r->meta, "enabled_channels");
    if (!cap || !fmt || !chans || chans->type != JS_ARR) {
        snprintf(err, errlen, "capture.json missing capture/sample_format/enabled_channels");
        return -1;
    }
    const char *fmttype = js_get_str(fmt, "type", "");
    if (strcmp(fmttype, "dslogic-cross-v1") != 0) {
        snprintf(err, errlen, "unsupported sample_format '%s'", fmttype);
        return -1;
    }
    r->rate_hz = js_get_num(cap, "sample_rate_hz", 0);
    if (!r->rate_hz) {
        snprintf(err, errlen, "capture.json has no sample_rate_hz");
        return -1;
    }
    r->ch_slots = (int)js_get_num(fmt, "channel_slots", 0);
    if (r->ch_slots <= 0 || chans->count != r->ch_slots) {
        snprintf(err, errlen, "channel_slots (%d) != enabled_channels count (%d)",
                 r->ch_slots, chans->count);
        return -1;
    }
    r->channel_count = chans->count;
    for (int i = 0; i < chans->count && i < 32; i++) {
        if (chans->items[i]->type != JS_STR) {
            snprintf(err, errlen, "enabled_channels[%d] not a string", i);
            return -1;
        }
        snprintf(r->channels[i], sizeof(r->channels[i]), "%s", chans->items[i]->str);
    }
    const js_val *integ = js_get(r->meta, "integrity");
    r->complete = js_get_bool(integ, "complete", 1);
    r->overflow = js_get_bool(integ, "overflow", 0);

    snprintf(path, sizeof(path), "%s/capture.raw", dir);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        snprintf(err, errlen, "empty capture.raw");
        fclose(f);
        return -1;
    }
    r->raw = malloc((size_t)sz);
    if (!r->raw || fread(r->raw, 1, (size_t)sz, f) != (size_t)sz) {
        snprintf(err, errlen, "cannot read %s", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    r->raw_len = (size_t)sz;
    r->total_samples = dslab_cross_max_samples(r->raw_len, r->ch_slots);
    return 0;
}

void run_free(dslab_run *r)
{
    free(r->raw);
    js_free(r->meta);
    memset(r, 0, sizeof(*r));
}

int run_channel_slot(const dslab_run *r, const char *name)
{
    for (int i = 0; i < r->channel_count; i++)
        if (!strcmp(r->channels[i], name))
            return i;
    return -1;
}

int run_sample(const dslab_run *r, int slot, uint64_t idx)
{
    return dslab_cross_sample(r->raw, r->raw_len, r->ch_slots, idx, slot);
}

uint64_t run_edges(const dslab_run *r, int slot, uint64_t start, uint64_t end,
                   dslab_edge *edges, uint64_t max_edges)
{
    if (slot < 0 || slot >= r->ch_slots || !r->raw)
        return 0;
    if (end > r->total_samples)
        end = r->total_samples;
    if (start >= end)
        return 0;

    size_t group = (size_t)r->ch_slots * 8;
    size_t groups = r->raw_len / group;
    uint64_t found = 0;
    int prev = -1;

    for (size_t g = 0; g < groups; g++) {
        const uint8_t *base = r->raw + g * group + (size_t)slot * 8;
        for (unsigned b = 0; b < 8; b++) {
            uint8_t byte = base[b];
            uint64_t s0 = (uint64_t)g * 64 + b * 8;
            if (s0 + 8 <= start)
            {
                prev = (byte >> 7) & 1; /* keep tail bit for boundary */
                continue;
            }
            for (unsigned bit = 0; bit < 8; bit++) {
                uint64_t s = s0 + bit;
                if (s < start)
                {
                    prev = (byte >> bit) & 1;
                    continue;
                }
                if (s >= end)
                    return found;
                int lvl = (byte >> bit) & 1;
                if (prev >= 0 && lvl != prev) {
                    if (edges && found < max_edges) {
                        edges[found].sample = s;
                        edges[found].rising = lvl;
                    }
                    found++;
                }
                prev = lvl;
            }
        }
    }
    return found;
}

void run_time_str(uint64_t sample_idx, uint64_t rate_hz, char *buf, size_t bufsz)
{
    unsigned long long ns = (unsigned long long)(((unsigned __int128)sample_idx * 1000000000ULL) / rate_hz);
    if (ns < 1000)
        snprintf(buf, bufsz, "%llu ns", ns);
    else if (ns < 1000000ULL)
        snprintf(buf, bufsz, "%llu.%03llu us", ns / 1000ULL, (ns / 1000ULL) % 1000ULL ? (ns % 1000ULL) : 0);
    else if (ns < 1000000000ULL)
        snprintf(buf, bufsz, "%llu.%03llu ms", ns / 1000000ULL, (ns % 1000000ULL) / 1000ULL);
    else
        snprintf(buf, bufsz, "%llu.%03llu s", ns / 1000000000ULL, (ns % 1000000000ULL) / 1000000ULL);
}
