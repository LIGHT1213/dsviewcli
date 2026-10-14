/*
 * dslab — deterministic offline timing analysis over a saved run (M3).
 * All durations are computed in integer sample counts first; conversion to
 * time happens only at reporting (rate = samples/second).
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_ANALYZE_H
#define DSLAB_ANALYZE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "runfile.h"

typedef struct {
    uint64_t count;      /* number of values */
    double mean;
    double min;
    double max;
    double stddev;       /* population stddev — "jitter" definition */
} timing_agg;

static inline void timing_agg_init(timing_agg *a) { memset(a, 0, sizeof(*a)); }
void timing_agg_add(timing_agg *a, double v);
void timing_agg_finish(timing_agg *a);

/* Durations in SAMPLES (double for stddev math; values are integral). */
typedef struct {
    int ok;                 /* 0 when no edges/channel missing */
    uint64_t window_start, window_end; /* analyzed sample range */
    uint64_t edge_count;    /* total edges in window */
    uint64_t edges_truncated; /* edges beyond the collected cap */
    int first_level;        /* level at window start */
    /* rising-edge to rising-edge periods */
    timing_agg period;
    /* run-length stats */
    timing_agg high_width;
    timing_agg low_width;
    double duty_mean;       /* high/(high+low) per period */
    uint64_t duty_count;
    /* raw edges (capped) */
    dslab_edge *edges;
    uint64_t edges_kept;
} timing_result;

#define TIMING_EDGE_CAP 1000000ULL

int timing_analyze(const dslab_run *r, int slot,
                   uint64_t win_start, uint64_t win_end,
                   timing_result *out);

/* Channel-pair delay: k-th rising edge of `slot` vs k-th rising edge of
 * `ref_slot`, delays ref->slot in samples (may be negative). */
typedef struct {
    int ok;
    uint64_t count;
    timing_agg delay;     /* samples */
    int64_t first_delay;  /* samples, ref first rising -> channel first rising */
    int64_t min_delay, max_delay; /* integer samples */
} timing_delay;

int timing_channel_delay(const dslab_run *r, int slot, int ref_slot,
                         uint64_t win_start, uint64_t win_end,
                         timing_delay *out);

#endif
