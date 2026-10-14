/*
 * dslab — assertion engine (M3): evaluate JSON rule specs against a saved
 * run. Verdicts: pass / fail / indeterminate (e.g. no edges found, capture
 * incomplete). Exit code 5 when any rule fails.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_ASSERT_RULES_H
#define DSLAB_ASSERT_RULES_H

#include "runfile.h"

typedef enum { AR_PASS, AR_FAIL, AR_INDETERMINATE } ar_verdict;

typedef struct {
    char id[64];
    ar_verdict verdict;
    char detail[256];
    uint64_t evidence_start_sample;
    uint64_t evidence_end_sample;
} ar_result;

typedef struct {
    ar_result *results;
    int count;
    int fails;
    int indeterminate;
} ar_report;

/*
Supported rule types (fields in brackets, time fields in *_us microseconds):

  timing.period_range    [channel, min_us, max_us]  rising-edge period stats
  timing.freq_range      [channel, min_hz, max_hz]
  timing.pulse_high_max  [channel, max_us]
  timing.pulse_low_max   [channel, max_us]
  timing.duty_range      [channel, min, max]        0..1
  timing.channel_delay   [from, to, max_abs_us]     |k-th rising edge delay|
  i2c.address_ack        [address ("0x50"), read(0/1)] txn to address exists and ACKed
  i2c.no_nack            []                         no NACK anywhere
  uart.frames            [channel, baud, contains ("48 65"), min_frames] decoded byte
                                                          sequence contains bytes
  uart.no_errors         [channel, baud]            no framing/parity errors

All rules accept optional [window_start_us, window_us].
*/
ar_report *ar_run(const dslab_run *r, const js_val *spec_root);
void ar_report_free(ar_report *rep);

#endif
