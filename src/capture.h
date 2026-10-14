/*
 * dslab — capture once (blocking) implementation.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_CAPTURE_H
#define DSLAB_CAPTURE_H

#include <stdint.h>
#include "util.h"

typedef struct {
    const char *device_hint;   /* "auto" or model substring */
    uint8_t channels[32];      /* sorted channel indices */
    int channel_count;
    uint64_t rate_hz;
    uint64_t duration_ns;
    const char *mode;          /* "stream" only in M1 */
    int channel_mode_id;       /* -1 = auto-select */
    const char *output_dir;    /* NULL = runs/<timestamp> */
    uint64_t timeout_ms;       /* hard capture timeout, default duration*10+5s */
    int json;
} dslab_capture_opts;

typedef struct {
    char error_code[32];
    char error_msg[256];
    char run_dir[512];
    int printed; /* result JSON already emitted on stdout */
} dslab_capture_result;

/* Runs a complete blocking capture. Returns EX_* exit code; on EX_OK the
 * JSON result has already been printed. */
int dslab_capture_once(const dslab_capture_opts *opts, dslab_capture_result *res);

/* Stream-mode table used for validation/selection (values from
 * libsigrok4DSL dsl.h channel_modes[] @2e9e2c8e, U2Basic-relevant subset). */
typedef struct {
    int id;                    /* DSL_CHANNEL_ID */
    const char *name;
    int vld_num;               /* max usable channels */
    uint64_t min_rate, max_rate;
} dslab_stream_mode;

extern const dslab_stream_mode dslab_stream_modes[];

#endif
