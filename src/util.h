/*
 * dslab — small platform/parsing helpers.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_UTIL_H
#define DSLAB_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* Exit codes: distinct classes per docs/cli.md */
#define EX_OK          0
#define EX_USAGE       2
#define EX_DEVICE      3
#define EX_CAPTURE     4
#define EX_ASSERT      5

/* Normalized error codes surfaced in JSON "error.code" */
#define DSLAB_E_NO_DEVICE          "NO_DEVICE"
#define DSLAB_E_DEVICE_BUSY        "DEVICE_BUSY"
#define DSLAB_E_PERMISSION         "PERMISSION_DENIED"
#define DSLAB_E_UNSUPPORTED_CONFIG "UNSUPPORTED_CONFIG"
#define DSLAB_E_ARM_TIMEOUT        "ARM_TIMEOUT"
#define DSLAB_E_TRIGGER_TIMEOUT    "TRIGGER_TIMEOUT"
#define DSLAB_E_USB_DISCONNECTED   "USB_DISCONNECTED"
#define DSLAB_E_OVERFLOW           "CAPTURE_OVERFLOW"
#define DSLAB_E_INCOMPLETE         "INCOMPLETE_CAPTURE"
#define DSLAB_E_IO                 "IO_ERROR"
#define DSLAB_E_INTERNAL           "INTERNAL_ERROR"

/* Parse "10ms" / "1s" / "100us" / "500ns" (bare number = milliseconds).
 * Returns ns; *ok set to 0 on parse failure. */
uint64_t dslab_parse_duration_ns(const char *s, int *ok);

/* Parse "20M" / "1M" / "100k" / "400" (bare number = Hz).
 * Returns Hz; *ok set to 0 on parse failure. Case-insensitive suffixes. */
uint64_t dslab_parse_rate_hz(const char *s, int *ok);

/* Parse "D0,D1" / "0,1" into sorted unique channel indices (max 32).
 * Returns count, 0/-1 on error. */
int dslab_parse_channels(const char *s, uint8_t idx[32]);

/* Recursive directory creation (like mkdir -p). 0 on success. */
int dslab_mkdirs(const char *path);

/* UTC ISO-8601 timestamp with milliseconds, e.g. 2026-10-14T12:34:56.789Z */
void dslab_now_iso8601(char buf[40]);

/* Monotonic-ish wall clock in ms (GetTickCount64 / clock_gettime). */
uint64_t dslab_now_ms(void);

/* Wall-clock UTC in unix epoch milliseconds (for heartbeats). */
uint64_t dslab_unix_ms(void);

/* Current process id as long (portable). */
long dslab_getpid(void);

void dslab_sleep_ms(unsigned ms);

/* Sample index -> time string in seconds with fixed precision (no
 * accumulated floating point: exact integer sample arithmetic). */
void dslab_sample_time_str(uint64_t sample_idx, uint64_t rate_hz, char *buf, size_t bufsz);

/* Format a count of bytes human-friendly into buf ("1.5 MiB"). */
void dslab_format_bytes(uint64_t bytes, char *buf, size_t bufsz);

#endif
