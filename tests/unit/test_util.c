/* Unit tests for parsing/time helpers (tests/unit, no hardware). */
#include <stdio.h>
#include <string.h>
#include "util.h"

static int failures = 0;

#define CHECK_EQ_U(a, b, msg) do { \
    if ((uint64_t)(a) != (uint64_t)(b)) { \
        fprintf(stderr, "FAIL: %s: got %llu want %llu (line %d)\n", \
                msg, (unsigned long long)(a), (unsigned long long)(b), __LINE__); \
        failures++; } \
} while (0)

#define CHECK_EQ_I(a, b, msg) do { \
    if ((int)(a) != (int)(b)) { \
        fprintf(stderr, "FAIL: %s: got %d want %d (line %d)\n", \
                msg, (int)(a), (int)(b), __LINE__); \
        failures++; } \
} while (0)

/* returns failure count (invoked by test_unpack.c's runner) */
int test_util_run(void)
{
    int ok = 0;

    CHECK_EQ_U(dslab_parse_duration_ns("10ms", &ok), 10000000ULL, "10ms");
    CHECK_EQ_I(ok, 1, "10ms ok");
    CHECK_EQ_U(dslab_parse_duration_ns("1s", &ok), 1000000000ULL, "1s");
    CHECK_EQ_U(dslab_parse_duration_ns("100us", &ok), 100000ULL, "100us");
    CHECK_EQ_U(dslab_parse_duration_ns("250ns", &ok), 250ULL, "250ns");
    CHECK_EQ_U(dslab_parse_duration_ns("1.5s", &ok), 1500000000ULL, "1.5s");
    CHECK_EQ_U(dslab_parse_duration_ns("100", &ok), 100000000ULL, "bare=100ms");
    dslab_parse_duration_ns("abc", &ok);
    CHECK_EQ_I(ok, 0, "garbage rejected");
    dslab_parse_duration_ns("10kg", &ok);
    CHECK_EQ_I(ok, 0, "bad unit rejected");

    CHECK_EQ_U(dslab_parse_rate_hz("20M", &ok), 20000000ULL, "20M");
    CHECK_EQ_U(dslab_parse_rate_hz("1M", &ok), 1000000ULL, "1M");
    CHECK_EQ_U(dslab_parse_rate_hz("100k", &ok), 100000ULL, "100k");
    CHECK_EQ_U(dslab_parse_rate_hz("400kHz", &ok), 400000ULL, "400kHz");
    CHECK_EQ_U(dslab_parse_rate_hz("20MHz", &ok), 20000000ULL, "20MHz");
    CHECK_EQ_U(dslab_parse_rate_hz("50", &ok), 50ULL, "bare Hz");
    dslab_parse_rate_hz("fast", &ok);
    CHECK_EQ_I(ok, 0, "rate garbage rejected");

    uint8_t ch[32];
    CHECK_EQ_I(dslab_parse_channels("D0,D1", ch), 2, "D0,D1 -> 2");
    CHECK_EQ_I(ch[0], 0, "sorted[0]=0");
    CHECK_EQ_I(ch[1], 1, "sorted[1]=1");
    CHECK_EQ_I(dslab_parse_channels("3,1,D2", ch), 3, "mixed -> 3");
    CHECK_EQ_I(ch[0], 1, "sorted ascending");
    CHECK_EQ_I(ch[2], 3, "sorted ascending end");
    CHECK_EQ_I(dslab_parse_channels("D1,D1", ch), 1, "dedup");
    CHECK_EQ_I(dslab_parse_channels("", ch), -1, "empty rejected");
    CHECK_EQ_I(dslab_parse_channels("Dx", ch), -1, "bad name rejected");

    /* time math: 1 sample at 20MHz = 50ns; 20000 samples = 1ms exactly */
    char buf[64];
    dslab_sample_time_str(20000, 20000000ULL, buf, sizeof(buf));
    CHECK_EQ_I(strcmp(buf, "1.000000 ms"), 0, "20k samples @20MHz = 1.000000 ms");

    return failures;
}
