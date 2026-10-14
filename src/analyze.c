#include "analyze.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

void timing_agg_add(timing_agg *a, double v)
{
    if (a->count == 0) {
        a->min = v;
        a->max = v;
    } else {
        if (v < a->min) a->min = v;
        if (v > a->max) a->max = v;
    }
    /* Welford for numerical stability */
    a->count++;
    double d = v - a->mean;
    a->mean += d / (double)a->count;
    a->stddev += d * (v - a->mean);
}

void timing_agg_finish(timing_agg *a)
{
    if (a->count >= 2)
        a->stddev = sqrt(a->stddev / (double)a->count); /* population */
    else
        a->stddev = 0;
}

int timing_analyze(const dslab_run *r, int slot,
                   uint64_t win_start, uint64_t win_end,
                   timing_result *out)
{
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= r->ch_slots)
        return -1;
    if (win_end == 0 || win_end > r->total_samples)
        win_end = r->total_samples;
    if (win_start >= win_end)
        return -1;
    out->window_start = win_start;
    out->window_end = win_end;

    uint64_t cap = TIMING_EDGE_CAP;
    out->edges = malloc(sizeof(dslab_edge) * (size_t)cap);
    if (!out->edges)
        return -1;

    out->edge_count = run_edges(r, slot, win_start, win_end, out->edges, cap);
    out->edges_kept = out->edge_count < cap ? out->edge_count : cap;
    out->edges_truncated = out->edge_count > cap ? out->edge_count - cap : 0;

    if (out->edges_kept == 0) {
        out->first_level = run_sample(r, slot, win_start);
        return 0; /* static signal: ok with zero edges */
    }

    out->first_level = out->edges[0].rising ? 0 : 1; /* level before first edge */

    timing_agg_init(&out->period);
    timing_agg_init(&out->high_width);
    timing_agg_init(&out->low_width);
    uint64_t run_start = win_start;
    int level = out->first_level;
    int64_t last_rising = -1;

    for (uint64_t i = 0; i < out->edges_kept; i++) {
        const dslab_edge *e = &out->edges[i];
        uint64_t run_len = e->sample - run_start;
        if (level)
            timing_agg_add(&out->high_width, (double)run_len);
        else
            timing_agg_add(&out->low_width, (double)run_len);
        if (e->rising) {
            if (last_rising >= 0)
                timing_agg_add(&out->period, (double)(e->sample - (uint64_t)last_rising));
            last_rising = (int64_t)e->sample;
        }
        level = e->rising;
        run_start = e->sample;
    }

    timing_agg_finish(&out->period);
    timing_agg_finish(&out->high_width);
    timing_agg_finish(&out->low_width);

    /* duty = mean high run / (mean high run + mean low run) */
    out->duty_mean = 0;
    out->duty_count = 0;
    if (out->high_width.count && out->low_width.count) {
        double h = out->high_width.mean, l = out->low_width.mean;
        out->duty_mean = h / (h + l);
        out->duty_count = out->high_width.count < out->low_width.count
                              ? out->high_width.count : out->low_width.count;
    }
    return 0;
}

int timing_channel_delay(const dslab_run *r, int slot, int ref_slot,
                         uint64_t win_start, uint64_t win_end,
                         timing_delay *out)
{
    memset(out, 0, sizeof(*out));
    if (slot < 0 || ref_slot < 0 || slot >= r->ch_slots || ref_slot >= r->ch_slots)
        return -1;
    if (win_end == 0 || win_end > r->total_samples)
        win_end = r->total_samples;

    dslab_edge *ea = malloc(sizeof(dslab_edge) * (size_t)TIMING_EDGE_CAP);
    dslab_edge *eb = malloc(sizeof(dslab_edge) * (size_t)TIMING_EDGE_CAP);
    if (!ea || !eb) {
        free(ea);
        free(eb);
        return -1;
    }
    uint64_t na = run_edges(r, slot, win_start, win_end, ea, TIMING_EDGE_CAP);
    uint64_t nb = run_edges(r, ref_slot, win_start, win_end, eb, TIMING_EDGE_CAP);
    if (!na || !nb) {
        free(ea);
        free(eb);
        return 0; /* no comparable edges */
    }

    /* first rising edges */
    int64_t fa = -1, fb = -1;
    for (uint64_t i = 0; i < na; i++)
        if (ea[i].rising) { fa = (int64_t)ea[i].sample; break; }
    for (uint64_t i = 0; i < nb; i++)
        if (eb[i].rising) { fb = (int64_t)eb[i].sample; break; }
    if (fa < 0 || fb < 0) {
        free(ea);
        free(eb);
        return 0;
    }
    out->first_delay = fa - fb;

    /* pair k-th rising edges */
    timing_agg_init(&out->delay);
    out->min_delay = INT64_MAX;
    out->max_delay = INT64_MIN;
    uint64_t ia = 0, ib = 0;
    while (ia < na && ib < nb) {
        while (ia < na && !ea[ia].rising) ia++;
        while (ib < nb && !eb[ib].rising) ib++;
        if (ia >= na || ib >= nb)
            break;
        int64_t d = (int64_t)ea[ia].sample - (int64_t)eb[ib].sample;
        timing_agg_add(&out->delay, (double)d);
        if (d < out->min_delay) out->min_delay = d;
        if (d > out->max_delay) out->max_delay = d;
        ia++;
        ib++;
    }
    timing_agg_finish(&out->delay);
    free(ea);
    free(eb);
    out->ok = out->delay.count > 0;
    return 0;
}
