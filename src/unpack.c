#include "unpack.h"

#include <string.h>

uint64_t dslab_cross_max_samples(size_t raw_len, int ch_count)
{
    if (ch_count <= 0)
        return 0;
    size_t group = (size_t)ch_count * 8;
    size_t groups = raw_len / group;
    return (uint64_t)groups * 64;
}

int dslab_cross_sample(const uint8_t *raw, size_t raw_len, int ch_count,
                       uint64_t i, int slot)
{
    if (!raw || ch_count <= 0 || slot < 0 || slot >= ch_count)
        return -1;
    uint64_t group = i >> 6;      /* i / 64  */
    unsigned n = (unsigned)(i & 63);
    size_t off = (size_t)(group * (uint64_t)ch_count * 8ULL + (uint64_t)slot * 8ULL + n / 8);
    if (off >= raw_len)
        return -1;
    return (raw[off] >> (n & 7)) & 1;
}

void dslab_cross_stats(const uint8_t *raw, size_t raw_len, int ch_count,
                       uint64_t max_samples, dslab_ch_stat *out)
{
    size_t group = (size_t)ch_count * 8;
    size_t groups = raw_len / group;
    if ((uint64_t)groups * 64 > max_samples)
        groups = (size_t)(max_samples >> 6);

    for (int c = 0; c < ch_count; c++) {
        out[c].transitions = 0;
        out[c].first_level = 0;
        out[c].last_level = 0;
        out[c].first_edge_sample = -1;
        out[c].last_edge_sample = -1;
        out[c].rising = 0;
        out[c].falling = 0;
    }
    if (!raw || groups == 0)
        return;

    for (int c = 0; c < ch_count; c++) {
        const uint8_t *base = raw + (size_t)c * 8;
        uint64_t sample = 0;
        int prev = -1;
        for (size_t g = 0; g < groups; g++) {
            for (unsigned b = 0; b < 8; b++) {
                uint8_t byte = base[g * group + b];
                for (unsigned bit = 0; bit < 8; bit++) {
                    int lvl = (byte >> bit) & 1;
                    if (prev < 0) {
                        out[c].first_level = (uint8_t)lvl;
                    } else if (lvl != prev) {
                        out[c].transitions++;
                        if (out[c].first_edge_sample < 0)
                            out[c].first_edge_sample = (int64_t)sample;
                        out[c].last_edge_sample = (int64_t)sample;
                        if (lvl)
                            out[c].rising++;
                        else
                            out[c].falling++;
                    }
                    prev = lvl;
                    out[c].last_level = (uint8_t)lvl;
                    sample++;
                }
            }
        }
    }
}

size_t dslab_cross_extract(const uint8_t *raw, size_t raw_len, int ch_count,
                           int slot, uint64_t max_samples, uint8_t *dst)
{
    size_t group = (size_t)ch_count * 8;
    size_t groups = raw_len / group;
    if ((uint64_t)groups * 64 > max_samples)
        groups = (size_t)(max_samples >> 6);
    size_t out_bytes = (groups * 64 + 7) / 8;
    if (!dst)
        return out_bytes;
    memset(dst, 0, out_bytes);
    for (size_t g = 0; g < groups; g++) {
        const uint8_t *src = raw + g * group + (size_t)slot * 8;
        /* samples 0..63 of this group: byte n/8, LSB-first. Output wants
         * MSB-first packing: out byte j bit (7-k) = sample j*8+k. */
        for (unsigned n = 0; n < 64 && (uint64_t)(g * 64 + n) < max_samples; n++) {
            int bit = (src[n / 8] >> (n & 7)) & 1;
            uint64_t abs_s = (uint64_t)g * 64 + n;
            if (bit)
                dst[abs_s / 8] |= (uint8_t)(0x80u >> (abs_s % 8));
        }
    }
    return out_bytes;
}
