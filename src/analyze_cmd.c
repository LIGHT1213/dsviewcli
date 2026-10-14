#include "analyze_cmd.h"
#include "jsonw.h"
#include "util.h"
#include "runfile.h"
#include "analyze.h"
#include "decode.h"
#include "assert_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void err_json(const char *command, const char *code, const char *msg)
{
    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 0);
    jw_key_str(&w, "command", command);
    jw_key_obj(&w, "error");
    jw_key_str(&w, "code", code);
    jw_key_str(&w, "message", msg);
    jw_key_end(&w);
    jw_key_arr(&w, "warnings");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return NULL;
}

/* "--window 100us:5ms" -> start/end samples (0 = default) */
static int parse_window(const dslab_run *r, const char *spec,
                        uint64_t *ws, uint64_t *we)
{
    *ws = 0;
    *we = 0;
    if (!spec)
        return 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *colon = strchr(buf, ':');
    if (!colon)
        return -1;
    *colon = '\0';
    int ok1 = 0, ok2 = 0;
    uint64_t s = dslab_parse_duration_ns(buf, &ok1);
    uint64_t d = dslab_parse_duration_ns(colon + 1, &ok2);
    if (!ok1 || !ok2)
        return -1;
    *ws = (uint64_t)(((unsigned __int128)s * r->rate_hz) / 1000000000ULL);
    uint64_t len = (uint64_t)(((unsigned __int128)d * r->rate_hz) / 1000000000ULL);
    *we = *ws + len;
    if (*ws > r->total_samples)
        *ws = r->total_samples;
    if (*we > r->total_samples)
        *we = r->total_samples;
    return 0;
}

static void agg_json(JsonW *w, const char *name, const timing_agg *a, uint64_t rate)
{
    if (!a->count)
        return;
    jw_key_obj(w, name);
    jw_key_uint(w, "count", a->count);
    /* samples as exact integers */
    jw_key_uint(w, "min_samples", (unsigned long long)a->min);
    jw_key_uint(w, "max_samples", (unsigned long long)a->max);
    jw_key_uint(w, "mean_samples", (unsigned long long)(a->mean + 0.5));
    jw_key_dbl(w, "mean_us", a->mean * 1000000.0 / (double)rate);
    jw_key_dbl(w, "min_us", a->min * 1000000.0 / (double)rate);
    jw_key_dbl(w, "max_us", a->max * 1000000.0 / (double)rate);
    jw_key_dbl(w, "jitter_us_stddev", a->stddev * 1000000.0 / (double)rate);
    jw_key_end(w);
}

int cmd_analyze_timing(int argc, char **argv)
{
    if (argc < 1) {
        err_json("analyze.timing", "INTERNAL_ERROR", "missing run dir");
        return EX_USAGE;
    }
    const char *dir = argv[0];
    const char *chan = arg_value(argc, argv, "--channel");
    const char *ref = arg_value(argc, argv, "--reference");
    const char *win = arg_value(argc, argv, "--window");
    if (!chan)
        chan = "D0";

    dslab_run r;
    char err[256];
    if (run_load(dir, &r, err, sizeof(err)) != 0) {
        err_json("analyze.timing", "IO_ERROR", err);
        return EX_CAPTURE;
    }
    uint64_t ws, we;
    if (parse_window(&r, win, &ws, &we) != 0) {
        run_free(&r);
        err_json("analyze.timing", "INTERNAL_ERROR",
                 "bad --window (expect e.g. 100us:5ms)");
        return EX_USAGE;
    }

    int slot = run_channel_slot(&r, chan);
    if (slot < 0) {
        run_free(&r);
        char msg[128];
        snprintf(msg, sizeof(msg), "channel %s not in this capture", chan);
        err_json("analyze.timing", "UNSUPPORTED_CONFIG", msg);
        return EX_USAGE;
    }

    timing_result tr;
    timing_analyze(&r, slot, ws, we, &tr);

    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 1);
    jw_key_str(&w, "command", "analyze.timing");
    jw_key_str(&w, "run_dir", dir);
    jw_key_uint(&w, "sample_rate_hz", r.rate_hz);
    jw_key_dbl(&w, "sample_interval_ns", 1000000000.0 / (double)r.rate_hz);
    jw_key_str(&w, "quantization_note",
               "all durations quantized to +/-1 sample; stddev is the population jitter estimate");
    jw_key_str(&w, "channel", chan);
    jw_key_uint(&w, "window_start_sample", ws);
    jw_key_uint(&w, "window_end_sample", we ? we : r.total_samples);
    jw_key_uint(&w, "analyzed_samples",
                (we ? we : r.total_samples) - ws);
    jw_key_uint(&w, "edge_count", tr.edge_count);
    jw_key_int(&w, "first_level", tr.first_level);
    agg_json(&w, "period", &tr.period, r.rate_hz);
    agg_json(&w, "pulse_high", &tr.high_width, r.rate_hz);
    agg_json(&w, "pulse_low", &tr.low_width, r.rate_hz);
    if (tr.duty_count) {
        jw_key_dbl(&w, "duty_cycle", tr.duty_mean);
        jw_key_uint(&w, "duty_cycle_count", tr.duty_count);
    }
    if (tr.period.count)
        jw_key_dbl(&w, "frequency_hz",
                   (double)r.rate_hz / tr.period.mean);

    /* capped edge list for quick localization */
    jw_key_arr(&w, "edges");
    uint64_t list = tr.edges_kept < 64 ? tr.edges_kept : 64;
    for (uint64_t i = 0; i < list; i++) {
        jw_obj(&w);
        jw_key_uint(&w, "sample", tr.edges[i].sample);
        jw_key_str(&w, "edge", tr.edges[i].rising ? "rising" : "falling");
        char tb[48];
        run_time_str(tr.edges[i].sample, r.rate_hz, tb, sizeof(tb));
        jw_key_str(&w, "time", tb);
        jw_key_end(&w);
    }
    jw_arr_end(&w);

    /* optional two-channel delay */
    if (ref) {
        int rslot = run_channel_slot(&r, ref);
        if (rslot < 0) {
            jw_key_str(&w, "channel_delay_error", "reference channel not in capture");
        } else {
            timing_delay dl;
            timing_channel_delay(&r, slot, rslot, ws, we, &dl);
            jw_key_obj(&w, "channel_delay");
            jw_key_str(&w, "from", ref);
            jw_key_str(&w, "to", chan);
            if (dl.ok) {
                jw_key_uint(&w, "pairs", dl.count);
                jw_key_int(&w, "first_delay_samples", (int)dl.first_delay);
                jw_key_int(&w, "min_delay_samples", (int)dl.min_delay);
                jw_key_int(&w, "max_delay_samples", (int)dl.max_delay);
                jw_key_dbl(&w, "mean_delay_us",
                           dl.delay.mean * 1000000.0 / (double)r.rate_hz);
            } else {
                jw_key_bool(&w, "ok", 0);
            }
            jw_key_end(&w);
        }
    }

    jw_key_arr(&w, "warnings");
    if (!r.complete)
        jw_str(&w, "capture was marked incomplete");
    if (r.overflow)
        jw_str(&w, "capture had overflow");
    if (tr.edge_count == 0)
        jw_str(&w, "static channel: no edges in window");
    if (tr.edges_truncated)
        jw_str(&w, "edge list truncated for stats at 1000000 edges");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);

    free(tr.edges);
    run_free(&r);
    return EX_OK;
}

/* ---------------- decode ---------------- */

int cmd_decode(int argc, char **argv)
{
    if (argc < 2) {
        err_json("decode", "INTERNAL_ERROR", "usage: decode uart|i2c|spi <run> [opts]");
        return EX_USAGE;
    }
    const char *proto = argv[0];
    const char *dir = argv[1];
    dslab_run r;
    char err[256];
    if (run_load(dir, &r, err, sizeof(err)) != 0) {
        err_json("decode", "IO_ERROR", err);
        return EX_CAPTURE;
    }

    char command[32];
    snprintf(command, sizeof(command), "decode.%s", proto);

    if (!strcmp(proto, "uart")) {
        const char *chan = arg_value(argc, argv, "--channel");
        if (!chan) chan = "D0";
        uart_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        const char *baud_s = arg_value(argc, argv, "--baud");
        cfg.baud = baud_s ? strtoull(baud_s, NULL, 0) : 115200;
        const char *bits_s = arg_value(argc, argv, "--bits");
        cfg.data_bits = bits_s ? atoi(bits_s) : 8;
        const char *par_s = arg_value(argc, argv, "--parity");
        cfg.parity = par_s ? (!strcmp(par_s, "even") ? 1 : !strcmp(par_s, "odd") ? 2 : 0) : 0;
        const char *stop_s = arg_value(argc, argv, "--stop");
        cfg.stop_bits = stop_s ? atoi(stop_s) : 1;

        int slot = run_channel_slot(&r, chan);
        uart_result ur;
        int rc = decode_uart(&r, slot, &cfg, &ur);
        if (slot < 0) {
            run_free(&r);
            char msg[128];
            snprintf(msg, sizeof(msg), "channel %s not in this capture", chan);
            err_json(command, "UNSUPPORTED_CONFIG", msg);
            return EX_USAGE;
        }
        JsonW w;
        jw_init(&w, stdout);
        jw_obj(&w);
        jw_key_str(&w, "schema_version", "1.0");
        jw_key_bool(&w, "ok", rc == 0 && ur.rate_ok);
        jw_key_str(&w, "command", command);
        jw_key_str(&w, "run_dir", dir);
        jw_key_uint(&w, "sample_rate_hz", r.rate_hz);
        jw_key_str(&w, "channel", chan);
        jw_key_uint(&w, "baud", cfg.baud);
        jw_key_int(&w, "data_bits", cfg.data_bits);
        jw_key_str(&w, "parity", cfg.parity == 1 ? "even" : cfg.parity == 2 ? "odd" : "none");
        jw_key_int(&w, "stop_bits", cfg.stop_bits);
        jw_key_dbl(&w, "samples_per_bit", ur.samples_per_bit);
        jw_key_uint(&w, "frame_count", ur.count);
        uint64_t errs = 0;
        for (uint64_t i = 0; i < ur.count; i++)
            errs += ur.frames[i].framing_error || ur.frames[i].parity_error;
        jw_key_uint(&w, "frame_errors", errs);
        jw_key_arr(&w, "frames");
        uint64_t list = ur.count < 256 ? ur.count : 256;
        for (uint64_t i = 0; i < list; i++) {
            jw_obj(&w);
            jw_key_uint(&w, "start_sample", ur.frames[i].start_sample);
            char tb[48];
            run_time_str(ur.frames[i].start_sample, r.rate_hz, tb, sizeof(tb));
            jw_key_str(&w, "time", tb);
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%02x", ur.frames[i].value);
            char chr[8];
            unsigned c = ur.frames[i].value;
            snprintf(chr, sizeof(chr), "%s",
                     (c >= 32 && c < 127) ? (char[2]){ (char)c, 0 } : ".");
            jw_key_str(&w, "value", hex);
            jw_key_str(&w, "char", chr);
            jw_key_bool(&w, "parity_error", ur.frames[i].parity_error);
            jw_key_bool(&w, "framing_error", ur.frames[i].framing_error);
            jw_key_end(&w);
        }
        jw_arr_end(&w);
        jw_key_arr(&w, "warnings");
        if (!ur.rate_ok)
            jw_str(&w, "sample rate < 2 samples/bit: decode unreliable/refused");
        if (ur.count > 256)
            jw_str(&w, "frame list capped at 256");
        jw_arr_end(&w);
        jw_obj_end(&w);
        fputc('\n', stdout);
        decode_uart_free(&ur);
        run_free(&r);
        return EX_OK;
    }

    if (!strcmp(proto, "i2c")) {
        const char *scln = arg_value(argc, argv, "--scl");
        const char *sdan = arg_value(argc, argv, "--sda");
        if (!scln) scln = "D0";
        if (!sdan) sdan = "D1";
        int scl = run_channel_slot(&r, scln);
        int sda = run_channel_slot(&r, sdan);
        if (scl < 0 || sda < 0) {
            run_free(&r);
            char msg[160];
            snprintf(msg, sizeof(msg), "need SCL(%s) and SDA(%s) channels", scln, sdan);
            err_json(command, "UNSUPPORTED_CONFIG", msg);
            return EX_USAGE;
        }
        i2c_result ir;
        decode_i2c(&r, scl, sda, &ir);
        JsonW w;
        jw_init(&w, stdout);
        jw_obj(&w);
        jw_key_str(&w, "schema_version", "1.0");
        jw_key_bool(&w, "ok", 1);
        jw_key_str(&w, "command", command);
        jw_key_str(&w, "run_dir", dir);
        jw_key_uint(&w, "sample_rate_hz", r.rate_hz);
        jw_key_str(&w, "scl", scln);
        jw_key_str(&w, "sda", sdan);
        jw_key_uint(&w, "transaction_count", ir.count);
        jw_key_arr(&w, "transactions");
        uint64_t list = ir.count < 128 ? ir.count : 128;
        for (uint64_t i = 0; i < list; i++) {
            const i2c_txn *t = &ir.txns[i];
            jw_obj(&w);
            jw_key_uint(&w, "start_sample", t->start_sample);
            char tb[48];
            run_time_str(t->start_sample, r.rate_hz, tb, sizeof(tb));
            jw_key_str(&w, "time", tb);
            jw_key_bool(&w, "restart", t->restarted);
            if (t->addr >= 0) {
                char a[10];
                snprintf(a, sizeof(a), "0x%02x", t->addr);
                jw_key_str(&w, "address", a);
                jw_key_str(&w, "rw", t->is_read ? "read" : "write");
            }
            jw_key_uint(&w, "stop_sample", t->stop_sample);
            jw_key_arr(&w, "bytes");
            for (uint64_t b = 0; b < t->byte_count; b++) {
                jw_obj(&w);
                char hex[8];
                snprintf(hex, sizeof(hex), "0x%02x", t->bytes[b].value);
                jw_key_str(&w, "value", hex);
                jw_key_str(&w, "kind",
                           t->bytes[b].kind == I2C_BYTE_ADDR ? "addr" : "data");
                jw_key_str(&w, "ack", t->bytes[b].ack == 0 ? "ACK"
                           : t->bytes[b].ack == 1 ? "NACK" : "missing");
                jw_key_uint(&w, "sample", t->bytes[b].sample);
                jw_key_end(&w);
            }
            jw_arr_end(&w);
            jw_key_end(&w);
        }
        jw_arr_end(&w);
        jw_key_arr(&w, "warnings");
        for (uint64_t i = 0; i < ir.count; i++)
            if (ir.txns[i].stop_sample == 0)
                jw_str(&w, "unterminated transaction (no STOP seen)");
        if (ir.count > 128)
            jw_str(&w, "transaction list capped at 128");
        jw_arr_end(&w);
        jw_obj_end(&w);
        fputc('\n', stdout);
        decode_i2c_free(&ir);
        run_free(&r);
        return EX_OK;
    }

    if (!strcmp(proto, "spi")) {
        const char *clkn = arg_value(argc, argv, "--clk");
        const char *mosin = arg_value(argc, argv, "--mosi");
        const char *mison = arg_value(argc, argv, "--miso");
        const char *csn = arg_value(argc, argv, "--cs");
        const char *mode_s = arg_value(argc, argv, "--mode");
        const char *order_s = arg_value(argc, argv, "--order");
        const char *word_s = arg_value(argc, argv, "--word");
        if (!clkn) clkn = "D0";
        if (!csn) csn = "D3";
        int clk = run_channel_slot(&r, clkn);
        int mosi = mosin ? run_channel_slot(&r, mosin) : -1;
        int miso = mison ? run_channel_slot(&r, mison) : -1;
        int cs = run_channel_slot(&r, csn);
        if (clk < 0 || cs < 0 || mosi < 0) {
            run_free(&r);
            char msg[200];
            snprintf(msg, sizeof(msg), "need clk(%s), cs(%s) and mosi channels%s",
                     clkn, csn, mosi ? "" : " (--mosi missing)");
            err_json(command, "UNSUPPORTED_CONFIG", msg);
            return EX_USAGE;
        }
        spi_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.mode = mode_s ? atoi(mode_s) : 0;
        cfg.lsb_first = order_s && !strcmp(order_s, "lsb") ? 1 : 0;
        cfg.word_bits = word_s ? atoi(word_s) : 8;
        spi_result sr;
        int rc = decode_spi(&r, clk, mosi, miso, cs, &cfg, &sr);
        if (rc != 0) {
            run_free(&r);
            err_json(command, "UNSUPPORTED_CONFIG", "bad spi options (mode 0..3, word<=32)");
            return EX_USAGE;
        }
        JsonW w;
        jw_init(&w, stdout);
        jw_obj(&w);
        jw_key_str(&w, "schema_version", "1.0");
        jw_key_bool(&w, "ok", 1);
        jw_key_str(&w, "command", command);
        jw_key_str(&w, "run_dir", dir);
        jw_key_uint(&w, "sample_rate_hz", r.rate_hz);
        jw_key_str(&w, "clk", clkn);
        if (mosin) jw_key_str(&w, "mosi", mosin);
        if (mison) jw_key_str(&w, "miso", mison);
        jw_key_str(&w, "cs", csn);
        jw_key_int(&w, "mode", cfg.mode);
        jw_key_str(&w, "order", cfg.lsb_first ? "lsb" : "msb");
        jw_key_int(&w, "word_bits", cfg.word_bits);
        jw_key_uint(&w, "transaction_count", sr.count);
        jw_key_arr(&w, "transactions");
        uint64_t list = sr.count < 128 ? sr.count : 128;
        for (uint64_t i = 0; i < list; i++) {
            const spi_txn *t = &sr.txns[i];
            jw_obj(&w);
            jw_key_uint(&w, "start_sample", t->start_sample);
            jw_key_uint(&w, "end_sample", t->end_sample);
            char tb[48];
            run_time_str(t->start_sample, r.rate_hz, tb, sizeof(tb));
            jw_key_str(&w, "time", tb);
            jw_key_uint(&w, "bits", t->bit_count);
            jw_key_uint(&w, "words", t->word_count);
            /* hex byte lists */
            char hex[1024];
            size_t off = 0;
            hex[0] = '\0';
            for (uint64_t b = 0; b < t->word_count && off + 6 < sizeof(hex); b++)
                off += (size_t)snprintf(hex + off, sizeof(hex) - off, "%s%02x",
                                        b ? " " : "", t->mosi ? t->mosi[b] : 0);
            jw_key_str(&w, "mosi_hex", hex);
            off = 0;
            hex[0] = '\0';
            if (t->miso) {
                for (uint64_t b = 0; b < t->word_count && off + 6 < sizeof(hex); b++)
                    off += (size_t)snprintf(hex + off, sizeof(hex) - off, "%s%02x",
                                            b ? " " : "", t->miso[b]);
            }
            jw_key_str(&w, "miso_hex", hex);
            jw_key_bool(&w, "misaligned", t->misaligned);
            jw_key_end(&w);
        }
        jw_arr_end(&w);
        jw_key_arr(&w, "warnings");
        for (uint64_t i = 0; i < sr.count; i++)
            if (sr.txns[i].misaligned)
                jw_str(&w, "transaction bit count not a multiple of word size");
        if (sr.count > 128)
            jw_str(&w, "transaction list capped at 128");
        jw_arr_end(&w);
        jw_obj_end(&w);
        fputc('\n', stdout);
        decode_spi_free(&sr);
        run_free(&r);
        return EX_OK;
    }

    run_free(&r);
    err_json("decode", "INTERNAL_ERROR", "unknown protocol (uart|i2c|spi)");
    return EX_USAGE;
}

/* ---------------- assert ---------------- */

int cmd_assert(int argc, char **argv)
{
    const char *dir = NULL, *spec_path = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--spec") && i + 1 < argc)
            spec_path = argv[++i];
        else if (argv[i][0] != '-' && !dir)
            dir = argv[i];
    }
    if (!dir || !spec_path) {
        err_json("assert", "INTERNAL_ERROR", "usage: assert <run-dir> --spec <file.json>");
        return EX_USAGE;
    }

    dslab_run r;
    char err[256];
    if (run_load(dir, &r, err, sizeof(err)) != 0) {
        err_json("assert", "IO_ERROR", err);
        return EX_CAPTURE;
    }
    FILE *f = fopen(spec_path, "rb");
    if (!f) {
        run_free(&r);
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot open spec %s", spec_path);
        err_json("assert", "IO_ERROR", msg);
        return EX_USAGE;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)sz + 1);
    if (!text || fread(text, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(text);
        run_free(&r);
        err_json("assert", "IO_ERROR", "cannot read spec");
        return EX_USAGE;
    }
    text[sz] = '\0';
    fclose(f);
    js_val *spec = js_parse(text, err, sizeof(err));
    free(text);
    if (!spec) {
        run_free(&r);
        char msg[300];
        snprintf(msg, sizeof(msg), "spec parse error: %s", err);
        err_json("assert", "IO_ERROR", msg);
        return EX_USAGE;
    }

    ar_report *rep = ar_run(&r, spec);
    js_free(spec);
    if (!rep) {
        run_free(&r);
        err_json("assert", "IO_ERROR", "spec has no rules[] array");
        return EX_USAGE;
    }

    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", rep->fails == 0);
    jw_key_str(&w, "command", "assert");
    jw_key_str(&w, "run_dir", dir);
    jw_key_str(&w, "spec_path", spec_path);
    jw_key_int(&w, "rule_count", rep->count);
    jw_key_int(&w, "failed", rep->fails);
    jw_key_int(&w, "indeterminate", rep->indeterminate);
    jw_key_str(&w, "verdict",
               rep->fails ? "fail" : rep->indeterminate ? "indeterminate" : "pass");
    jw_key_arr(&w, "rules");
    for (int i = 0; i < rep->count; i++) {
        jw_obj(&w);
        jw_key_str(&w, "id", rep->results[i].id);
        jw_key_str(&w, "verdict",
                   rep->results[i].verdict == AR_PASS ? "pass"
                   : rep->results[i].verdict == AR_FAIL ? "fail" : "indeterminate");
        jw_key_str(&w, "detail", rep->results[i].detail);
        jw_key_uint(&w, "evidence_start_sample", rep->results[i].evidence_start_sample);
        jw_key_uint(&w, "evidence_end_sample", rep->results[i].evidence_end_sample);
        jw_key_end(&w);
    }
    jw_arr_end(&w);
    jw_key_arr(&w, "warnings");
    if (!r.complete)
        jw_str(&w, "capture was marked incomplete: passing rules are not conclusive");
    if (r.overflow)
        jw_str(&w, "capture had overflow: timing rules may be unreliable");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);

    int fails = rep->fails;
    ar_report_free(rep);
    run_free(&r);
    return fails ? EX_ASSERT : EX_OK;
}
