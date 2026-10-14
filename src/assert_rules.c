#include "assert_rules.h"
#include "analyze.h"
#include "decode.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

static void ar_push(ar_report *rep, const char *id, ar_verdict v, const char *fmt, ...);

static void win_from_rule(const dslab_run *r, const js_val *rule,
                          uint64_t *ws, uint64_t *we)
{
    uint64_t rate = r->rate_hz;
    double start_us = js_get_dbl(rule, "window_start_us", 0);
    double dur_us = js_get_dbl(rule, "window_us", 0);
    *ws = (uint64_t)(start_us * 1000.0 * (double)rate / 1000000000.0);
    if (dur_us > 0)
        *we = *ws + (uint64_t)(dur_us * 1000.0 * (double)rate / 1000000000.0);
    else
        *we = 0; /* to end */
    if (*ws > r->total_samples)
        *ws = r->total_samples;
    if (*we > r->total_samples)
        *we = r->total_samples;
}

/* samples <-> microseconds helpers (exact via ns) */
static double samples_to_us(uint64_t rate, double samples)
{
    return samples * 1000000.0 / (double)rate;
}
static double us_to_samples(uint64_t rate, double us)
{
    return us * (double)rate / 1000000.0;
}

static int timing_rule(const dslab_run *r, const js_val *rule, const char *type,
                       ar_report *rep, const char *id)
{
    const char *chan = js_get_str(rule, "channel", "D0");
    int slot = run_channel_slot(r, chan);
    if (slot < 0) {
        ar_push(rep, id, AR_INDETERMINATE, "channel %s not in capture", chan);
        return 0;
    }
    uint64_t ws, we;
    win_from_rule(r, rule, &ws, &we);
    if (ws >= (we ? we : r->total_samples)) {
        ar_push(rep, id, AR_INDETERMINATE, "empty window");
        return 0;
    }

    timing_result tr;
    if (timing_analyze(r, slot, ws, we, &tr) != 0) {
        ar_push(rep, id, AR_INDETERMINATE, "analysis failed");
        return 0;
    }
    if (tr.edge_count == 0) {
        free(tr.edges);
        ar_push(rep, id, AR_INDETERMINATE, "no edges on %s in window", chan);
        return 0;
    }

    char d[256];
    if (!strcmp(type, "timing.period_range")) {
        if (tr.period.count == 0) {
            snprintf(d, sizeof(d), "fewer than 2 rising edges");
            ar_push(rep, id, AR_INDETERMINATE, "%s", d);
        } else {
            double min_us = samples_to_us(r->rate_hz, tr.period.min);
            double max_us = samples_to_us(r->rate_hz, tr.period.max);
            double want_min = js_get_dbl(rule, "min_us", 0);
            double want_max = js_get_dbl(rule, "max_us", 1e18);
            int ok = min_us >= want_min - 1e-9 && max_us <= want_max + 1e-9;
            snprintf(d, sizeof(d), "period min %.3f us max %.3f us over %llu periods "
                     "(±1 sample = %.3f us)",
                     min_us, max_us, (unsigned long long)tr.period.count,
                     samples_to_us(r->rate_hz, 1));
            ar_push(rep, id, ok ? AR_PASS : AR_FAIL, "%s", d);
        }
    } else if (!strcmp(type, "timing.freq_range")) {
        if (tr.period.count == 0) {
            ar_push(rep, id, AR_INDETERMINATE, "fewer than 2 rising edges");
        } else {
            double freq = (double)r->rate_hz / tr.period.mean;
            double want_min = js_get_dbl(rule, "min_hz", 0);
            double want_max = js_get_dbl(rule, "max_hz", 1e18);
            int ok = freq >= want_min && freq <= want_max;
            snprintf(d, sizeof(d), "frequency %.3f Hz (mean period %.3f us, n=%llu)",
                     freq, samples_to_us(r->rate_hz, tr.period.mean),
                     (unsigned long long)tr.period.count);
            ar_push(rep, id, ok ? AR_PASS : AR_FAIL, "%s", d);
        }
    } else if (!strcmp(type, "timing.pulse_high_max") || !strcmp(type, "timing.pulse_low_max")) {
        int high = !strcmp(type, "timing.pulse_high_max");
        const timing_agg *a = high ? &tr.high_width : &tr.low_width;
        if (a->count == 0) {
            ar_push(rep, id, AR_INDETERMINATE, "no %s pulses found", high ? "high" : "low");
        } else {
            double got_us = samples_to_us(r->rate_hz, a->max);
            double want = js_get_dbl(rule, "max_us", 1e18);
            int ok = got_us <= want + 1e-9;
            snprintf(d, sizeof(d), "max %s pulse %.3f us over %llu pulses",
                     high ? "high" : "low", got_us, (unsigned long long)a->count);
            ar_push(rep, id, ok ? AR_PASS : AR_FAIL, "%s", d);
        }
    } else if (!strcmp(type, "timing.duty_range")) {
        if (!tr.duty_count) {
            ar_push(rep, id, AR_INDETERMINATE, "no complete cycles for duty");
        } else {
            double want_min = js_get_dbl(rule, "min", 0);
            double want_max = js_get_dbl(rule, "max", 1);
            int ok = tr.duty_mean >= want_min - 1e-9 && tr.duty_mean <= want_max + 1e-9;
            snprintf(d, sizeof(d), "duty %.4f (n=%llu cycles)", tr.duty_mean,
                     (unsigned long long)tr.duty_count);
            ar_push(rep, id, ok ? AR_PASS : AR_FAIL, "%s", d);
        }
    } else {
        ar_push(rep, id, AR_INDETERMINATE, "unknown timing rule type");
    }
    free(tr.edges);
    return 0;
}

static int delay_rule(const dslab_run *r, const js_val *rule, ar_report *rep, const char *id)
{
    const char *from = js_get_str(rule, "from", "D0");
    const char *to = js_get_str(rule, "to", "D1");
    int fs = run_channel_slot(r, from), ts = run_channel_slot(r, to);
    if (fs < 0 || ts < 0) {
        ar_push(rep, id, AR_INDETERMINATE, "channel missing (need %s and %s)", from, to);
        return 0;
    }
    uint64_t ws, we;
    win_from_rule(r, rule, &ws, &we);
    timing_delay dl;
    if (timing_channel_delay(r, ts, fs, ws, we, &dl) != 0 || !dl.ok) {
        ar_push(rep, id, AR_INDETERMINATE, "no comparable rising edges");
        return 0;
    }
    double max_abs_us = js_get_dbl(rule, "max_abs_us", 1e18);
    double got_us = samples_to_us(r->rate_hz,
                                  (double)(dl.max_delay > -dl.min_delay
                                           ? dl.max_delay : dl.min_delay));
    int ok = fabs(got_us) <= max_abs_us + 1e-9;
    char d[256];
    snprintf(d, sizeof(d), "rising-edge delay first=%lld min=%lld max=%lld samples "
             "(max |delay| %.3f us over %llu pairs)",
             (long long)dl.first_delay, (long long)dl.min_delay,
             (long long)dl.max_delay, fabs(got_us),
             (unsigned long long)dl.count);
    ar_push(rep, id, ok ? AR_PASS : AR_FAIL, "%s", d);
    return 0;
}

static int hexbyte(const char *s, int pos)
{
    int hi = -1, lo = -1;
    char h = s[pos], l = s[pos + 1];
    if (!h || !l) return -1;
    if (h >= '0' && h <= '9') hi = h - '0';
    else if (h >= 'a' && h <= 'f') hi = h - 'a' + 10;
    else if (h >= 'A' && h <= 'F') hi = h - 'A' + 10;
    if (l >= '0' && l <= '9') lo = l - '0';
    else if (l >= 'a' && l <= 'f') lo = l - 'a' + 10;
    else if (l >= 'A' && l <= 'F') lo = l - 'A' + 10;
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

static int i2c_rule(const dslab_run *r, const js_val *rule, const char *type,
                    ar_report *rep, const char *id)
{
    const char *scln = js_get_str(rule, "scl", "D0");
    const char *sdan = js_get_str(rule, "sda", "D1");
    int scl = run_channel_slot(r, scln), sda = run_channel_slot(r, sdan);
    if (scl < 0 || sda < 0) {
        ar_push(rep, id, AR_INDETERMINATE, "need SCL(%s) and SDA(%s)", scln, sdan);
        return 0;
    }
    i2c_result ir;
    if (decode_i2c(r, scl, sda, &ir) != 0 || ir.count == 0) {
        decode_i2c_free(&ir);
        ar_push(rep, id, AR_INDETERMINATE, "no I2C transactions decoded");
        return 0;
    }
    char d[256];
    if (!strcmp(type, "i2c.address_ack")) {
        const char *addr_s = js_get_str(rule, "address", "");
        int want_addr = (int)strtol(addr_s, NULL, 0);
        int want_read = js_get_dbl(rule, "read", -1);
        int found = 0, acked = 0;
        uint64_t ev_s = 0, ev_e = 0;
        for (uint64_t i = 0; i < ir.count; i++) {
            const i2c_txn *t = &ir.txns[i];
            if (t->addr == want_addr && (want_read < 0 || t->is_read == want_read)
                && t->byte_count >= 1) {
                found = 1;
                ev_s = t->start_sample;
                ev_e = t->stop_sample;
                if (t->bytes[0].ack == 0)
                    acked = 1;
            }
        }
        if (!found) {
            snprintf(d, sizeof(d), "address %s (%02x) not seen in %llu txns",
                     addr_s, want_addr, (unsigned long long)ir.count);
            ar_push(rep, id, AR_FAIL, "%s", d);
        } else if (!acked) {
            snprintf(d, sizeof(d), "address %s seen but NACKed", addr_s);
            ar_push(rep, id, AR_FAIL, "%s", d);
        } else {
            snprintf(d, sizeof(d), "address %s ACKed", addr_s);
            ar_push(rep, id, AR_PASS, "%s", d);
        }
        (void)ev_s; (void)ev_e;
    } else if (!strcmp(type, "i2c.no_nack")) {
        int nacks = 0;
        for (uint64_t i = 0; i < ir.count; i++)
            for (uint64_t b = 0; b < ir.txns[i].byte_count; b++)
                if (ir.txns[i].bytes[b].ack == 1)
                    nacks++;
        snprintf(d, sizeof(d), "%llu transactions, %d NACK byte(s)",
                 (unsigned long long)ir.count, nacks);
        ar_push(rep, id, nacks ? AR_FAIL : AR_PASS, "%s", d);
    } else {
        ar_push(rep, id, AR_INDETERMINATE, "unknown i2c rule type");
    }
    decode_i2c_free(&ir);
    return 0;
}

static int uart_rule(const dslab_run *r, const js_val *rule, const char *type,
                     ar_report *rep, const char *id)
{
    const char *chan = js_get_str(rule, "channel", "D0");
    int slot = run_channel_slot(r, chan);
    if (slot < 0) {
        ar_push(rep, id, AR_INDETERMINATE, "channel %s not in capture", chan);
        return 0;
    }
    uart_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.baud = (uint64_t)js_get_dbl(rule, "baud", 115200);
    cfg.data_bits = (int)js_get_num(rule, "bits", 8);
    cfg.parity = (int)js_get_num(rule, "parity", 0);
    cfg.stop_bits = (int)js_get_num(rule, "stop", 1);
    uart_result ur;
    if (decode_uart(r, slot, &cfg, &ur) != 0 || !ur.rate_ok) {
        decode_uart_free(&ur);
        ar_push(rep, id, AR_INDETERMINATE,
                "sample rate insufficient for %llu baud", (unsigned long long)cfg.baud);
        return 0;
    }
    char d[256];
    if (!strcmp(type, "uart.frames")) {
        const char *contains = js_get_str(rule, "contains", "");
        /* parse "48 65" */
        uint8_t want[64];
        int nwant = 0;
        for (const char *p = contains; *p && nwant < 64;) {
            while (*p == ' ') p++;
            if (!*p) break;
            int b = hexbyte(p, 0);
            if (b < 0) {
                ar_push(rep, id, AR_INDETERMINATE, "bad 'contains' hex");
                decode_uart_free(&ur);
                return 0;
            }
            want[nwant++] = (uint8_t)b;
            p += 2;
        }
        uint64_t min_frames = js_get_num(rule, "min_frames", 1);
        int match = 0;
        for (uint64_t i = 0; i + (uint64_t)nwant <= ur.count && !match; i++) {
            match = 1;
            for (int k = 0; k < nwant; k++)
                if (ur.frames[i + (uint64_t)k].value != want[k]) {
                    match = 0;
                    break;
                }
        }
        if (ur.count < min_frames) {
            snprintf(d, sizeof(d), "only %llu frames decoded (need %llu)",
                     (unsigned long long)ur.count, (unsigned long long)min_frames);
            ar_push(rep, id, AR_FAIL, "%s", d);
        } else if (nwant && !match) {
            snprintf(d, sizeof(d), "byte sequence '%s' not found in %llu frames",
                     contains, (unsigned long long)ur.count);
            ar_push(rep, id, AR_FAIL, "%s", d);
        } else {
            snprintf(d, sizeof(d), "%llu frames, sequence %s",
                     (unsigned long long)ur.count,
                     nwant ? "found" : "(not constrained)");
            ar_push(rep, id, AR_PASS, "%s", d);
        }
    } else if (!strcmp(type, "uart.no_errors")) {
        int errs = 0;
        for (uint64_t i = 0; i < ur.count; i++)
            errs += ur.frames[i].framing_error || ur.frames[i].parity_error;
        snprintf(d, sizeof(d), "%llu frames, %d with errors",
                 (unsigned long long)ur.count, errs);
        ar_push(rep, id, errs ? AR_FAIL : AR_PASS, "%s", d);
    } else {
        ar_push(rep, id, AR_INDETERMINATE, "unknown uart rule type");
    }
    decode_uart_free(&ur);
    return 0;
}

static void ar_push(ar_report *rep, const char *id, ar_verdict v, const char *fmt, ...)
{
    ar_result *nr = realloc(rep->results, sizeof(ar_result) * (size_t)(rep->count + 1));
    if (!nr)
        return;
    rep->results = nr;
    ar_result *res = &rep->results[rep->count++];
    memset(res, 0, sizeof(*res));
    snprintf(res->id, sizeof(res->id), "%s", id ? id : "");
    res->verdict = v;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(res->detail, sizeof(res->detail), fmt, ap);
    va_end(ap);
    res->evidence_start_sample = 0;
    res->evidence_end_sample = 0;
    if (v == AR_FAIL)
        rep->fails++;
    if (v == AR_INDETERMINATE)
        rep->indeterminate++;
}

ar_report *ar_run(const dslab_run *r, const js_val *spec_root)
{
    const js_val *rules = js_get(spec_root, "rules");
    if (!rules || rules->type != JS_ARR)
        return NULL;
    ar_report *rep = calloc(1, sizeof(ar_report));
    if (!rep)
        return NULL;

    for (int i = 0; i < rules->count; i++) {
        const js_val *rule = rules->items[i];
        const char *type = js_get_str(rule, "type", "");
        const char *id = js_get_str(rule, "id", "");
        if (!strncmp(type, "timing.", 7) && strcmp(type, "timing.channel_delay") != 0)
            timing_rule(r, rule, type, rep, id);
        else if (!strcmp(type, "timing.channel_delay"))
            delay_rule(r, rule, rep, id);
        else if (!strncmp(type, "i2c.", 4))
            i2c_rule(r, rule, type, rep, id);
        else if (!strncmp(type, "uart.", 5))
            uart_rule(r, rule, type, rep, id);
        else
            ar_push(rep, id, AR_INDETERMINATE, "unknown rule type '%s'", type);
    }
    return rep;
}

void ar_report_free(ar_report *rep)
{
    if (!rep)
        return;
    free(rep->results);
    free(rep);
}
