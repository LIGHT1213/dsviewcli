/*
 * dslab — DSLogic LA_CROSS_DATA raw sample unpacking.
 *
 * Wire format (see docs/source-audit.md §6, derived from upstream
 * DSView/pv/data/logicsnapshot.cpp @2e9e2c8e):
 *   - atomic group = N_enabled_channels * 8 bytes, N = slot count
 *   - slot c of group g occupies bytes [(g*N + c)*8, +8)
 *   - within a slot, sample n (0..63) is bit (n%8) of byte n/8 (LSB-first)
 *   - slot order = enabled channels sorted by channel index ascending
 *
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_UNPACK_H
#define DSLAB_UNPACK_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t transitions;   /* level toggles within the window */
    uint8_t  first_level;   /* level at sample 0 */
    uint8_t  last_level;
    int64_t  first_edge_sample; /* -1 when no edge */
    int64_t  last_edge_sample;
    uint64_t rising;
    uint64_t falling;
} dslab_ch_stat;

/* Max samples representable by raw_len bytes with n enabled channels. */
uint64_t dslab_cross_max_samples(size_t raw_len, int ch_count);

/* Value of sample `i` on slot `slot` (bounds-checked, returns -1 when the
 * byte lies outside the buffer). */
int dslab_cross_sample(const uint8_t *raw, size_t raw_len, int ch_count,
                       uint64_t i, int slot);

/* Statistics for every slot; out must have ch_count entries.
 * max_samples caps the analyzed window (use dslab_cross_max_samples). */
void dslab_cross_stats(const uint8_t *raw, size_t raw_len, int ch_count,
                       uint64_t max_samples, dslab_ch_stat *out);

/* Extract one slot into a packed bitstream (MSB-first in each byte:
 * byte k bit (7 - n%8) = sample k*8+n) so external tools can consume it.
 * dst must hold (max_samples+7)/8 bytes. Returns bytes written. */
size_t dslab_cross_extract(const uint8_t *raw, size_t raw_len, int ch_count,
                           int slot, uint64_t max_samples, uint8_t *dst);

#endif
