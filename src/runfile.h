/*
 * dslab — offline run (capture directory) loading + channel bit access.
 * Reads capture.raw (dslogic-cross-v1) + capture.json metadata.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_RUNFILE_H
#define DSLAB_RUNFILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "jsparse.h"

typedef struct {
    char dir[512];
    uint8_t *raw;
    size_t raw_len;
    uint64_t rate_hz;
    int ch_slots;                 /* enabled channel slot count */
    char channels[32][8];         /* names: D0, D1... */
    int channel_count;            /* == ch_slots */
    uint64_t total_samples;       /* raw_len*8/ch_slots (complete groups) */
    int complete;
    int overflow;
    js_val *meta;                 /* parsed capture.json (owned) */
} dslab_run;

/* Load <dir>/capture.raw + capture.json. Returns 0 on success.
 * err gets a message on failure. */
int run_load(const char *dir, dslab_run *r, char *err, size_t errlen);
void run_free(dslab_run *r);

/* Slot index for "D3" (position in the enabled channel list); -1 when the
 * channel was not captured. */
int run_channel_slot(const dslab_run *r, const char *name);

/* Value of sample idx on slot (bounds-checked; -1 outside). */
int run_sample(const dslab_run *r, int slot, uint64_t idx);

/* ---- edge scanning (streaming over preloaded raw) ---- */
typedef struct {
    uint64_t sample;   /* sample index of the edge */
    int rising;        /* 0->1 ? */
} dslab_edge;

/* Collect up to max_edges edges in [start_sample, end_sample). Returns the
 * total number of edges found (may exceed max_edges; only max_edges are
 * stored). Set edges==NULL to only count. */
uint64_t run_edges(const dslab_run *r, int slot, uint64_t start, uint64_t end,
                   dslab_edge *edges, uint64_t max_edges);

/* Sample index -> time string "123.456 us" (exact integer math, ns). */
void run_time_str(uint64_t sample_idx, uint64_t rate_hz, char *buf, size_t bufsz);

#endif
