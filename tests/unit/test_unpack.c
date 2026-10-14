/* Unit tests for LA_CROSS_DATA unpacking (tests/unit, no hardware). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "unpack.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
} while (0)

static void test_max_samples(void)
{
    CHECK(dslab_cross_max_samples(16, 2) == 64, "16B/2ch = 1 group = 64 samples");
    CHECK(dslab_cross_max_samples(15, 2) == 0, "partial group ignored");
    CHECK(dslab_cross_max_samples(160, 2) == 640, "10 groups");
    CHECK(dslab_cross_max_samples(24, 3) == 64, "3ch group is 24B");
}

static void test_sample_access(void)
{
    /* 2 channels, one group. Slot0 = 0b1010_0101 in byte0 (LSB-first:
     * samples 0,2,4,6,...), slot1 byte0 = 0xFF (all high). */
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x55;          /* ch slot0 byte0 */
    raw[8] = 0xFF;          /* ch slot1 byte0 */

    CHECK(dslab_cross_sample(raw, 16, 2, 0, 0) == 1, "slot0 sample0 = bit0 of 0x55");
    CHECK(dslab_cross_sample(raw, 16, 2, 1, 0) == 0, "slot0 sample1 = bit1");
    CHECK(dslab_cross_sample(raw, 16, 2, 2, 0) == 1, "slot0 sample2");
    CHECK(dslab_cross_sample(raw, 16, 2, 7, 0) == 0, "slot0 sample7 = bit7");
    CHECK(dslab_cross_sample(raw, 16, 2, 63, 0) == 0, "slot0 sample63 (byte7=0)");
    CHECK(dslab_cross_sample(raw, 16, 2, 0, 1) == 1, "slot1 sample0 high");
    CHECK(dslab_cross_sample(raw, 16, 2, 9, 1) == 0, "slot1 sample9 (byte1=0)");
    CHECK(dslab_cross_sample(raw, 16, 2, 64, 0) == -1, "past end returns -1");
}

static void test_second_group_offsets(void)
{
    /* two groups; second group slot0 byte0 has only sample0 set */
    uint8_t raw[32];
    memset(raw, 0, sizeof(raw));
    raw[16] = 0x01; /* group1 slot0 byte0 bit0 */
    CHECK(dslab_cross_sample(raw, 32, 2, 64, 0) == 1, "group1 sample64 = bit0 of byte16");
    CHECK(dslab_cross_sample(raw, 32, 2, 65, 0) == 0, "group1 sample65");
    CHECK(dslab_cross_sample(raw, 32, 2, 64, 1) == 0, "group1 slot1 low");}

static void test_stats(void)
{
    /* ch0: toggles every 8 samples (byte0=0xFF byte1=0x00 repeating);
     * ch1: constant 0. Two groups. */
    uint8_t raw[32];
    memset(raw, 0, sizeof(raw));
    for (int g = 0; g < 2; g++) {
        uint8_t *slot0 = raw + g * 16;
        slot0[0] = 0xFF; slot0[1] = 0x00; /* samples 0-7 high, 8-15 low */
        uint8_t *slot1 = raw + g * 16 + 8;
        memset(slot1, 0, 8);
    }
    dslab_ch_stat st[2];
    dslab_cross_stats(raw, 32, 2, 128, st);
    CHECK(st[0].first_level == 1, "ch0 starts high");
    CHECK(st[0].transitions == 3, "ch0: 1->0 at 8, 0->1 at 64, 1->0 at 72");
    CHECK(st[0].falling == 2, "ch0 falling=2");
    CHECK(st[0].rising == 1, "ch0 rising=1");
    CHECK(st[0].first_edge_sample == 8, "first edge at sample 8");
    CHECK(st[0].last_edge_sample == 72, "last edge at sample 72");
    CHECK(st[1].transitions == 0, "ch1 static");
    CHECK(st[1].first_level == 0, "ch1 low");
}

static void test_extract(void)
{
    /* slot0: samples 0..6 low, sample7 high -> MSB-first byte0 = 0x01 */
    uint8_t raw[16];
    memset(raw, 0, sizeof(raw));
    raw[0] = 0x80; /* bit7 -> sample7 */
    uint8_t dst[8];
    size_t n = dslab_cross_extract(raw, 16, 1, 0, 64, dst);
    CHECK(n == 8, "64 samples -> 8 bytes");
    CHECK(dst[0] == 0x01, "MSB-first: sample7 high -> 0x01");
    CHECK(dst[7] == 0, "rest zero");
}

static void test_golden_stream(void)
{
    /* Golden fixture: 2ch square waves in quadrature at period 16 samples,
     * 4 groups (256 samples). ch0 rises at 0, ch1 rises at 8. */
    enum { GROUPS = 4, CH = 2, RAWLEN = GROUPS * CH * 8 };
    uint8_t raw[RAWLEN];
    memset(raw, 0, sizeof(raw));
    for (int g = 0; g < GROUPS; g++) {
        for (int slot = 0; slot < CH; slot++) {
            for (int n = 0; n < 64; n++) {
                int s = g * 64 + n;
                int phase = (s + slot * 8) % 16;
                int level = phase < 8 ? 1 : 0;
                if (level)
                    raw[g * CH * 8 + slot * 8 + n / 8] |= (uint8_t)(1u << (n % 8));
            }
        }
    }
    /* verify a few positions through the public accessor */
    CHECK(dslab_cross_sample(raw, RAWLEN, CH, 0, 0) == 1, "ch0 s0 high");
    CHECK(dslab_cross_sample(raw, RAWLEN, CH, 8, 0) == 0, "ch0 s8 low");
    CHECK(dslab_cross_sample(raw, RAWLEN, CH, 0, 1) == 0, "ch1 s0 low");
    CHECK(dslab_cross_sample(raw, RAWLEN, CH, 8, 1) == 1, "ch1 s8 high");
    CHECK(dslab_cross_sample(raw, RAWLEN, CH, 255, 0) == 0, "ch0 s255 low");
    CHECK(dslab_cross_sample(raw, RAWLEN, CH, 255, 1) == 1, "ch1 s255 high");
    dslab_ch_stat st[CH];
    dslab_cross_stats(raw, RAWLEN, CH, 256, st);
    /* period = 16 samples over 256 samples: an edge every 8 samples at
     * s=8,16,...,248 -> 31 edges per channel; ch0 starts high (first edge
     * is a fall at s=8), ch1 starts low (first edge is a rise at s=8). */
    CHECK(st[0].transitions == 31, "ch0 has 31 edges");
    CHECK(st[1].transitions == 31, "ch1 has 31 edges");
    CHECK(st[0].falling == 16, "ch0 falling=16");
    CHECK(st[0].rising == 15, "ch0 rising=15");
    CHECK(st[1].rising == 16, "ch1 rising=16");
    CHECK(st[1].falling == 15, "ch1 falling=15");
    CHECK(st[0].first_edge_sample == 8, "ch0 first edge at sample 8");
    CHECK(st[1].first_edge_sample == 8, "ch1 first edge at sample 8");
    CHECK(st[0].last_edge_sample == 248, "ch0 last edge at 248");
    CHECK(st[0].first_level == 1, "ch0 starts high");
    CHECK(st[1].first_level == 0, "ch1 starts low");
}

int test_util_run(void); /* tests/unit/test_util.c */

int main(void)
{
    test_max_samples();
    test_sample_access();
    test_second_group_offsets();
    test_stats();
    test_extract();
    test_golden_stream();
    failures += test_util_run();
    if (failures) {
        fprintf(stderr, "%d unit test failure(s)\n", failures);
        return 1;
    }
    printf("unit tests (unpack+util): all passed\n");
    return 0;
}
