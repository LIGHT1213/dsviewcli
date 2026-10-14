#include "capture.h"
#include "dsl_session.h"
#include "jsonw.h"
#include "unpack.h"
#include "session_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libsigrok.h>

/* Trigger-header check word (upstream dsl.h:120, kept in sync). */
#define TRIG_CHECKID 0x55555555

/* Stream-mode subset relevant to DSLogic-family LA capture (dsl.h:369-441
 * and U2Basic capability bits dsl.h:568/757/811). min_rate from the
 * channel_modes[] table (SR_KHZ(50) for the non-3DN2 stream modes). */
const dslab_stream_mode dslab_stream_modes[] = {
    { DSL_STREAM20x16,  "Use 16 Channels (Max 20MHz)",  16, 50000ULL,   20000000ULL },
    { DSL_STREAM25x12,  "Use 12 Channels (Max 25MHz)",  12, 50000ULL,   25000000ULL },
    { DSL_STREAM50x6,   "Use 6 Channels (Max 50MHz)",    6, 50000ULL,   50000000ULL },
    { DSL_STREAM100x3,  "Use 3 Channels (Max 100MHz)",   3, 50000ULL,  100000000ULL },
    { DSL_BUFFER100x16, "Use Channels 0~15 (Max 100MHz)",16, 50000ULL, 100000000ULL },
    { -1, NULL, 0, 0, 0 }
};

/* ---- state shared with library callbacks (collect thread) ---- */
static struct {
    FILE *raw;
    FILE *events;
    volatile uint64_t bytes_seen;
    volatile int packets;
    volatile int overflow;
    volatile int packet_errors;
    volatile int have_trigger;
    volatile uint64_t trigger_remain_cnt;
    volatile int trigger_check_ok;
    volatile int collect_started;
    volatile int collect_ended;
    volatile int collect_error;
    volatile int collect_detached;
    volatile int device_running;
} cap;

/* worker hooks (NULL for capture-once) */
static struct {
    int active;
    char dir[512];
    char id[32];
    long pid;
    char *config_fld;
} wk;

static void ev_log(const char *event, const char *detail_json)
{
    if (!cap.events)
        return;
    char ts[40];
    dslab_now_iso8601(ts);
    fprintf(cap.events, "{\"ts\":\"%s\",\"event\":\"%s\"%s%s}\n",
            ts, event, detail_json ? ",\"detail\":" : "",
            detail_json ? detail_json : "");
    fflush(cap.events);
}

static void wk_state(const char *state)
{
    if (!wk.active)
        return;
    ses_touch_heartbeat(wk.dir, wk.id, wk.pid, state, cap.bytes_seen, cap.bytes_seen, wk.config_fld);
    if (cap.events) {
        ev_log(state, NULL);
    } else {
        /* state can change before events.jsonl is opened (e.g. configuring) */
        char path[600];
        snprintf(path, sizeof(path), "%s/events.jsonl", wk.dir);
        FILE *f = fopen(path, "ab");
        if (f) {
            char ts[40];
            dslab_now_iso8601(ts);
            fprintf(f, "{\"ts\":\"%s\",\"event\":\"%s\"}\n", ts, state);
            fclose(f);
        }
    }
}

static void on_ds_event(int event)
{
    switch (event) {
    case DS_EV_COLLECT_TASK_START: cap.collect_started = 1; ev_log("collect_start", NULL); break;
    case DS_EV_DEVICE_RUNNING:     cap.device_running = 1;  ev_log("device_running", NULL); break;
    case DS_EV_DEVICE_STOPPED:                              ev_log("device_stopped", NULL); break;
    case DS_EV_COLLECT_TASK_END:  cap.collect_ended = 1;    ev_log("collect_end", NULL); break;
    case DS_EV_COLLECT_TASK_END_BY_ERROR:
        cap.collect_ended = 1; cap.collect_error = 1;
        ev_log("collect_end_by_error", NULL); break;
    case DS_EV_COLLECT_TASK_END_BY_DETACHED:
        cap.collect_ended = 1; cap.collect_detached = 1;
        ev_log("collect_end_by_detached", NULL); break;
    default: break;
    }
}

static void on_datafeed(const struct sr_dev_inst *sdi,
                        const struct sr_datafeed_packet *packet)
{
    (void)sdi;
    if (!packet)
        return;
    switch (packet->type) {
    case SR_DF_HEADER:
        ev_log("df_header", NULL);
        break;
    case SR_DF_TRIGGER: {
        const struct ds_trigger_pos *tp = packet->payload;
        cap.have_trigger = 1;
        if (packet->status != SR_PKT_OK) {
            cap.packet_errors++;
            ev_log("df_trigger", "\"detail\":\"bad packet status\"");
            break;
        }
        if (tp->check_id == TRIG_CHECKID) {
            cap.trigger_check_ok = 1;
            cap.trigger_remain_cnt = (((uint64_t)tp->remain_cnt_h << 32) | tp->remain_cnt_l);
        } else {
            cap.packet_errors++;
        }
        char full[192];
        snprintf(full, sizeof(full), "{\"remain_cnt\":%llu}",
                 (unsigned long long)cap.trigger_remain_cnt);
        ev_log("df_trigger", full);
        break;
    }
    case SR_DF_LOGIC: {
        const struct sr_datafeed_logic *logic = packet->payload;
        if (packet->status != SR_PKT_OK) {
            cap.packet_errors++;
            break;
        }
        if (cap.raw && logic->length) {
            size_t w = fwrite(logic->data, 1, logic->length, cap.raw);
            fflush(cap.raw);
            if (w != logic->length) {
                cap.packet_errors++;
                ev_log("raw_write_error", NULL);
            }
            cap.bytes_seen += w;
        }
        cap.packets++;
        break;
    }
    case SR_DF_OVERFLOW:
        cap.overflow = 1;
        ev_log("df_overflow", NULL);
        break;
    case SR_DF_END:
        ev_log("df_end", NULL);
        break;
    default:
        break;
    }
}

static const dslab_stream_mode *select_stream_mode(const dslab_capture_opts *o,
                                                   char *errbuf, size_t errlen)
{
    if (o->channel_mode_id >= 0) {
        for (const dslab_stream_mode *m = dslab_stream_modes; m->id >= 0; m++)
            if (m->id == o->channel_mode_id) {
                if (o->channels[o->channel_count - 1] >= (uint8_t)m->vld_num)
                    snprintf(errbuf, errlen,
                             "channel D%d exceeds mode '%s' (valid channels 0..%d)",
                             o->channels[o->channel_count - 1], m->name, m->vld_num - 1);
                else if (o->rate_hz < m->min_rate || o->rate_hz > m->max_rate)
                    snprintf(errbuf, errlen,
                             "rate %llu Hz outside mode '%s' range [%llu, %llu]",
                             (unsigned long long)o->rate_hz, m->name,
                             (unsigned long long)m->min_rate,
                             (unsigned long long)m->max_rate);
                else
                    return m;
                return NULL;
            }
        snprintf(errbuf, errlen, "unknown channel mode id %d", o->channel_mode_id);
        return NULL;
    }
    /* auto: narrowest stream mode that fits the highest channel index + rate */
    const dslab_stream_mode *best = NULL;
    for (const dslab_stream_mode *m = dslab_stream_modes; m->id >= 0; m++) {
        if (m->id == DSL_BUFFER100x16)
            continue; /* buffer mode not selectable implicitly */
        if (o->channels[o->channel_count - 1] >= (uint8_t)m->vld_num)
            continue;
        if (o->rate_hz < m->min_rate || o->rate_hz > m->max_rate)
            continue;
        if (!best || m->vld_num < best->vld_num)
            best = m;
    }
    if (!best)
        snprintf(errbuf, errlen,
                 "no stream mode supports %d channel(s) up to D%d at %llu Hz; "
                 "supported: 16ch<=20MHz, 12ch<=25MHz, 6ch<=50MHz, 3ch<=100MHz",
                 o->channel_count, o->channels[o->channel_count - 1],
                 (unsigned long long)o->rate_hz);
    return best;
}

/* Configure the hardware trigger via the audited simple-trigger sequence:
 * reset → set_en → per-probe chars ('X' for don't-care) → position. */
static int apply_trigger(const dslab_capture_opts *o, struct ds_device_full_info *devinfo)
{
    ds_trigger_reset();
    if (o->trig.count == 0)
        return 0; /* hardware auto-triggers immediately */

    ds_trigger_set_en(1);
    for (GSList *l = dsl_channels(); l; l = l->next) {
        struct sr_channel *ch = l->data;
        if (!ch->enabled)
            continue;
        char tc = 'X';
        for (int i = 0; i < o->trig.count; i++)
            if (o->trig.items[i].ch_index == ch->index)
                tc = o->trig.items[i].trig_char;
        ds_trigger_probe_set(ch->index, (unsigned char)tc, (unsigned char)'X');
    }
    int pos = o->trig.position_pct;
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
    ds_trigger_set_pos((uint16_t)pos);
    (void)devinfo;
    return 0;
}

static char *build_config_fld(const dslab_capture_opts *o, uint64_t limit_samples,
                              const char *mode_name, int ch_en, uint64_t timeout_ms)
{
    char *buf = malloc(2048);
    if (!buf)
        return NULL;
    char chans[256] = "";
    size_t off = 0;
    for (int i = 0; i < o->channel_count; i++)
        off += (size_t)snprintf(chans + off, sizeof(chans) - off, "%sD%d",
                                i ? "," : "", o->channels[i]);
    char trigstr[256] = "";
    if (o->trig.count) {
        off = 0;
        for (int i = 0; i < o->trig.count; i++)
            off += (size_t)snprintf(trigstr + off, sizeof(trigstr) - off, "%sD%d:%c",
                                    i ? "," : "", o->trig.items[i].ch_index,
                                    o->trig.items[i].trig_char);
    }
    snprintf(buf, 2048,
             "{\"device\":\"%s\",\"channels\":\"%s\",\"rate_hz\":%llu,"
             "\"duration_ms\":%llu,\"limit_samples\":%llu,\"mode\":\"%s\","
             "\"channel_mode\":\"%s\",\"ch_enabled\":%d,"
             "\"timeout_ms\":%llu,\"trigger\":{\"spec\":\"%s\",\"position_pct\":%d}}",
             o->device_hint ? o->device_hint : "auto", chans,
             (unsigned long long)o->rate_hz,
             (unsigned long long)(o->duration_ns / 1000000ULL),
             (unsigned long long)limit_samples,
             o->mode ? o->mode : "stream", mode_name, ch_en,
             (unsigned long long)timeout_ms, trigstr, o->trig.position_pct);
    return buf;
}

int dslab_capture_once(const dslab_capture_opts *opts, dslab_capture_result *res)
{
    char errbuf[512] = "";
    memset(&cap, 0, sizeof(cap));
    memset(res, 0, sizeof(*res));
    memset(&wk, 0, sizeof(wk));
    if (opts->worker_mode) {
        wk.active = 1;
        snprintf(wk.id, sizeof(wk.id), "%s", opts->session_id);
        snprintf(wk.dir, sizeof(wk.dir), "runs/%s", opts->session_id);
        wk.pid = dslab_getpid();
    }

    if (strcmp(opts->mode ? opts->mode : "stream", "stream") != 0) {
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "mode '%s' not supported in M1/M2 (only 'stream')", opts->mode);
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_CAPTURE;
    }

    /* Trigger channels must be among the enabled capture channels. */
    for (int i = 0; i < opts->trig.count; i++) {
        int found = 0;
        for (int c = 0; c < opts->channel_count; c++)
            if (opts->channels[c] == opts->trig.items[i].ch_index)
                found = 1;
        if (!found) {
            snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
            snprintf(res->error_msg, sizeof(res->error_msg),
                     "trigger channel D%d is not in the enabled channel set",
                     opts->trig.items[i].ch_index);
            snprintf(res->final_state, sizeof(res->final_state), "failed");
            return EX_CAPTURE;
        }
    }

    /* ---- run directory ---- */
    char run_dir[512];
    if (opts->worker_mode) {
        snprintf(run_dir, sizeof(run_dir), "runs/%s", opts->session_id);
    } else if (opts->output_dir && *opts->output_dir) {
        snprintf(run_dir, sizeof(run_dir), "%s", opts->output_dir);
    } else {
        char ts[40];
        dslab_now_iso8601(ts);
        for (char *p = ts; *p; p++)
            if (*p == ':' || *p == '.' || *p == '-')
                *p = *p == '-' ? '-' : '_';
        snprintf(run_dir, sizeof(run_dir), "runs/%s", ts);
    }
    if (dslab_mkdirs(run_dir) != 0) {
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_IO);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "cannot create run dir '%s': %s", run_dir, strerror(errno));
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_CAPTURE;
    }
    char path_raw[600], path_events[600], path_meta[600];
    snprintf(path_raw, sizeof(path_raw), "%s/capture.raw", run_dir);
    snprintf(path_events, sizeof(path_events), "%s/events.jsonl", run_dir);
    snprintf(path_meta, sizeof(path_meta), "%s/capture.json", run_dir);

    /* ---- library setup ---- */
    const char *res_dir = dslab_res_dir_resolve();
    int rc = dsl_global_init(res_dir);
    if (rc != SR_OK && rc != SR_ERR_HAVE_DONE) {
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_INTERNAL);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "ds_lib_init failed rc=%d", rc);
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        goto fail_device;
    }
    ds_set_event_callback(on_ds_event);
    ds_set_datafeed_callback(on_datafeed);
    wk_state("configuring");

    struct ds_device_full_info devinfo;
    memset(&devinfo, 0, sizeof(devinfo));
    rc = dsl_activate(opts->device_hint, &devinfo);
    if (rc != SR_OK) {
        dsl_global_exit();
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_NO_DEVICE);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "no usable USB device found (hint='%s')",
                 opts->device_hint ? opts->device_hint : "auto");
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_DEVICE;
    }

    /* ---- configuration ---- */
    /* The bundled demo device ("--device demo") accepts the generic keys
     * but not DSLogic-specific ones; it exists only for pipeline testing
     * and is never auto-selected. Everything it produces is synthetic. */
    int is_demo = devinfo.dev_type == DEV_TYPE_DEMO;
    if (dsl_cfg_set_i16(SR_CONF_DEVICE_MODE, LOGIC) != SR_OK) {
        dsl_global_exit();
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_INTERNAL);
        snprintf(res->error_msg, sizeof(res->error_msg), "set DEVICE_MODE=LOGIC failed");
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_DEVICE;
    }
    if (!is_demo &&
        dsl_cfg_set_i16(SR_CONF_OPERATION_MODE, LO_OP_STREAM) != SR_OK) {
        dsl_global_exit();
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_INTERNAL);
        snprintf(res->error_msg, sizeof(res->error_msg), "set OPERATION_MODE=STREAM failed");
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_DEVICE;
    }

    const dslab_stream_mode *mode = NULL;
    dslab_stream_mode demo_mode_storage;
    if (is_demo) {
        demo_mode_storage.id = -1;
        demo_mode_storage.name = "demo";
        demo_mode_storage.vld_num = 16;
        demo_mode_storage.min_rate = 1;
        demo_mode_storage.max_rate = 1000000000ULL;
        mode = &demo_mode_storage;
    } else {
        mode = select_stream_mode(opts, errbuf, sizeof(errbuf));
        if (!mode) {
            dsl_global_exit();
            snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
            snprintf(res->error_msg, sizeof(res->error_msg), "%s", errbuf);
            snprintf(res->final_state, sizeof(res->final_state), "failed");
            return EX_CAPTURE;
        }
        if (dsl_cfg_set_i16(SR_CONF_CHANNEL_MODE, (int16_t)mode->id) != SR_OK) {
            dsl_global_exit();
            snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
            snprintf(res->error_msg, sizeof(res->error_msg),
                     "device rejected channel mode '%s'", mode->name);
            snprintf(res->final_state, sizeof(res->final_state), "failed");
            return EX_CAPTURE;
        }
    }

    /* Enable only requested channels (DEVICE_MODE reset all to enabled). */
    for (GSList *l = dsl_channels(); l; l = l->next) {
        struct sr_channel *ch = l->data;
        int want = 0;
        for (int i = 0; i < opts->channel_count; i++)
            if (ch->index == opts->channels[i])
                want = 1;
        dsl_enable_channel(ch->index, want);
    }
    int ch_en = dsl_channel_count_enabled();
    if (ch_en != opts->channel_count) {
        dsl_global_exit();
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "enabled %d channels but expected %d", ch_en, opts->channel_count);
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_CAPTURE;
    }

    /* Validate the requested rate against the samplerate list the device
     * actually reports (authoritative: driver-managed). */
    GVariant *srlist = NULL;
    if (ds_get_actived_device_config_list(NULL, SR_CONF_SAMPLERATE, &srlist) == SR_OK && srlist) {
        GVariant *arr = g_variant_lookup_value(srlist, "samplerates", G_VARIANT_TYPE("at"));
        if (arr) {
            gsize n = g_variant_n_children(arr);
            int supported = 0;
            for (gsize i = 0; i < n; i++) {
                GVariant *el = g_variant_get_child_value(arr, i);
                if (g_variant_get_uint64(el) == opts->rate_hz)
                    supported = 1;
                g_variant_unref(el);
                if (supported)
                    break;
            }
            g_variant_unref(arr);
            if (!supported) {
                g_variant_unref(srlist);
                dsl_global_exit();
                snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
                snprintf(res->error_msg, sizeof(res->error_msg),
                         "rate %llu Hz is not in the device samplerate list (%zu rates)",
                         (unsigned long long)opts->rate_hz, n);
                snprintf(res->final_state, sizeof(res->final_state), "failed");
                return EX_CAPTURE;
            }
        }
        g_variant_unref(srlist);
    }

    uint64_t limit_samples = (uint64_t)(((unsigned __int128)opts->rate_hz * opts->duration_ns +
                                          500000000ULL) / 1000000000ULL);
    if (limit_samples == 0)
        limit_samples = 1;

    if (dsl_cfg_set_u64(SR_CONF_SAMPLERATE, opts->rate_hz) != SR_OK ||
        dsl_cfg_set_u64(SR_CONF_LIMIT_SAMPLES, limit_samples) != SR_OK) {
        dsl_global_exit();
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "device rejected samplerate/limit config");
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_CAPTURE;
    }
    /* Read back what the device now reports. */
    uint64_t rate_now = 0;
    dsl_cfg_get_u64(SR_CONF_SAMPLERATE, &rate_now);
    if (rate_now && rate_now != opts->rate_hz) {
        dsl_global_exit();
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_UNSUPPORTED_CONFIG);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "samplerate readback %llu != requested %llu",
                 (unsigned long long)rate_now, (unsigned long long)opts->rate_hz);
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_CAPTURE;
    }

    /* ---- trigger (before acquisition start; audited sequence) ---- */
    apply_trigger(opts, &devinfo);

    /* ---- expected sample accounting (mirrors driver alignment) ---- */
    /* Real hardware rounds the sample count up to a multiple of 1024
     * (SAMPLES_ALIGN) and pads the stream; the demo generator emits exactly
     * limit_samples with no alignment padding. */
    uint64_t actual_samples = is_demo
        ? limit_samples
        : (limit_samples + 1023ULL) & ~1023ULL;
    uint64_t actual_bytes = actual_samples / 64ULL * (uint64_t)ch_en * 8ULL;

    cap.raw = fopen(path_raw, "wb");
    cap.events = fopen(path_events, "ab"); /* append: wk_state may have logged early */
    if (!cap.raw || !cap.events) {
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_IO);
        snprintf(res->error_msg, sizeof(res->error_msg), "cannot open output files in %s", run_dir);
        if (cap.raw) fclose(cap.raw);
        if (cap.events) fclose(cap.events);
        cap.raw = cap.events = NULL;
        dsl_global_exit();
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_CAPTURE;
    }
    snprintf(res->run_dir, sizeof(res->run_dir), "%s", run_dir);

    uint64_t timeout_ms = opts->timeout_ms ? opts->timeout_ms
                          : (opts->duration_ns / 1000000ULL) * 10ULL + 5000ULL;
    wk.config_fld = build_config_fld(opts, limit_samples, mode->name, ch_en, timeout_ms);

    char ts0[40];
    dslab_now_iso8601(ts0);
    fprintf(cap.events, "{\"ts\":\"%s\",\"event\":\"armed_config\",\"rate_hz\":%llu,"
            "\"limit_samples\":%llu,\"actual_samples\":%llu,\"actual_bytes\":%llu,"
            "\"channels\":%d,\"channel_mode\":\"%s\",\"trigger_en\":%d}\n",
            ts0, (unsigned long long)opts->rate_hz,
            (unsigned long long)limit_samples,
            (unsigned long long)actual_samples,
            (unsigned long long)actual_bytes,
            ch_en, mode->name, opts->trig.count > 0);
    fflush(cap.events);

    /* ---- run ---- */
    uint64_t t_start = dslab_now_ms();
    wk_state("starting");
    rc = ds_start_collect();
    if (rc != SR_OK) {
        fclose(cap.raw); fclose(cap.events);
        cap.raw = cap.events = NULL;
        int last = ds_get_last_error();
        dsl_global_exit();
        if (last == SR_ERR_DEVICE_IS_EXCLUSIVE) {
            snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_DEVICE_BUSY);
            snprintf(res->final_state, sizeof(res->final_state), "failed");
            return EX_DEVICE;
        }
        snprintf(res->error_code, sizeof(res->error_code), "%s", DSLAB_E_ARM_TIMEOUT);
        snprintf(res->error_msg, sizeof(res->error_msg),
                 "ds_start_collect failed rc=%d (%s)", rc, dsl_last_error_str(rc));
        snprintf(res->final_state, sizeof(res->final_state), "failed");
        return EX_CAPTURE;
    }

    /* armed vs capturing: with a user trigger configured, the device is
     * "armed" once running and before any data packet arrives; without a
     * trigger it is capturing immediately (hardware auto-triggers). */
    int announced_armed = 0;
    int stop_by_user = 0;
    int timeout_hit = 0;
    uint64_t last_heartbeat = 0;

    while (!cap.collect_ended) {
        dslab_sleep_ms(5);
        uint64_t now = dslab_now_ms();
        if (wk.active && !announced_armed && cap.device_running) {
            if (opts->trig.count > 0) {
                wk_state("armed");
            } else {
                wk_state("capturing");
            }
            announced_armed = 1;
        }
        if (wk.active && !announced_armed && cap.bytes_seen > 0)
            announced_armed = 1; /* data before running event: treat as capturing */
        if (wk.active && now - last_heartbeat > 200) {
            last_heartbeat = now;
            ses_touch_heartbeat(wk.dir, wk.id, wk.pid,
                                opts->trig.count > 0 && cap.bytes_seen == 0
                                    ? "armed" : "capturing",
                                cap.bytes_seen,
                                ch_en ? cap.bytes_seen / (uint64_t)ch_en * 8ULL : 0,
                                wk.config_fld);
        }
        if (wk.active && ses_stop_requested(wk.dir)) {
            ev_log("stop_requested", NULL);
            stop_by_user = 1;
            ds_stop_collect();
            for (int i = 0; i < 400 && !cap.collect_ended; i++)
                dslab_sleep_ms(5);
            break;
        }
        if (now - t_start > timeout_ms) {
            ev_log("timeout_hit", NULL);
            timeout_hit = 1;
            ds_stop_collect();
            for (int i = 0; i < 400 && !cap.collect_ended; i++)
                dslab_sleep_ms(5);
            break;
        }
    }
    uint64_t t_end = dslab_now_ms();
    if (cap.collect_ended)
        ds_stop_collect(); /* idempotent join when already finished */

    fclose(cap.raw);
    fclose(cap.events);
    cap.raw = cap.events = NULL;

    /* ---- reload raw for stats & final accounting ---- */
    FILE *fr = fopen(path_raw, "rb");
    dslab_ch_stat *stats = calloc((size_t)ch_en, sizeof(*stats));
    uint64_t bytes_on_disk = 0;
    if (fr && stats) {
        fseek(fr, 0, SEEK_END);
        long sz = ftell(fr);
        fseek(fr, 0, SEEK_SET);
        if (sz > 0) {
            uint8_t *buf = malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, fr) == (size_t)sz) {
                bytes_on_disk = (uint64_t)sz;
                uint64_t max_samples = dslab_cross_max_samples((size_t)sz, ch_en);
                if (max_samples > actual_samples)
                    max_samples = actual_samples;
                dslab_cross_stats(buf, (size_t)sz, ch_en, max_samples, stats);
            }
            free(buf);
        }
        fclose(fr);
    }
    uint64_t samples_received = ch_en ? bytes_on_disk / (uint64_t)ch_en * 8ULL : 0;

    dsl_release();
    dsl_global_exit();

    /* ---- terminal state decision ---- */
    int incomplete = samples_received < actual_samples;
    int trigger_armed = opts->trig.count > 0;
    const char *final_state = "complete";
    const char *err_code = NULL;
    if (cap.collect_error) {
        final_state = "failed";
        err_code = DSLAB_E_INTERNAL;
    } else if (cap.collect_detached) {
        final_state = "failed";
        err_code = DSLAB_E_USB_DISCONNECTED;
    } else if (stop_by_user) {
        final_state = "cancelled";
    } else if (timeout_hit) {
        final_state = "timeout";
        if (trigger_armed && !(cap.have_trigger && cap.trigger_check_ok))
            err_code = DSLAB_E_TRIGGER_TIMEOUT;
        else if (incomplete)
            err_code = DSLAB_E_INCOMPLETE;
    } else if (incomplete) {
        err_code = DSLAB_E_INCOMPLETE;
    }
    if (cap.overflow && !err_code)
        err_code = DSLAB_E_OVERFLOW;

    int static_all = 1;
    for (int i = 0; i < ch_en; i++)
        if (stats && stats[i].transitions)
            static_all = 0;
    int trigger_hit = (cap.have_trigger && cap.trigger_check_ok);

    snprintf(res->final_state, sizeof(res->final_state), "%s", final_state);
    res->trigger_hit = trigger_hit;
    res->samples = samples_received;
    res->bytes = bytes_on_disk;
    if (err_code)
        snprintf(res->error_code, sizeof(res->error_code), "%s", err_code);

    /* ---- write capture.json ---- */
    FILE *fm = fopen(path_meta, "wb");
    if (fm) {
        JsonW m;
        jw_init(&m, fm);
        jw_obj(&m);
        jw_key_str(&m, "schema_version", "1.0");
        jw_key_str(&m, "tool", "dslab");
        jw_key_str(&m, "command", opts->worker_mode ? "capture.worker" : "capture.once");
        if (opts->worker_mode)
            jw_key_str(&m, "session_id", opts->session_id);
        char tsb[40];
        dslab_now_iso8601(tsb);
        jw_key_str(&m, "created", tsb);
        jw_key_obj(&m, "device");
        jw_key_str(&m, "model", devinfo.name);
        jw_key_str(&m, "driver", devinfo.driver_name);
        jw_key_end(&m);
        jw_key_obj(&m, "sample_format");
        jw_key_str(&m, "type", "dslogic-cross-v1");
        jw_key_str(&m, "description",
                   "channel-interleaved 64-sample blocks; slot c bytes [(g*N+c)*8,+8); "
                   "sample n = bit(n%8) of byte n/8 (LSB-first); slots = enabled "
                   "channels by index ascending");
        jw_key_int(&m, "channel_slots", ch_en);
        jw_key_end(&m);
        jw_key_obj(&m, "capture");
        jw_key_uint(&m, "sample_rate_hz", rate_now ? rate_now : opts->rate_hz);
        jw_key_uint(&m, "requested_samples", limit_samples);
        jw_key_uint(&m, "expected_samples_aligned", actual_samples);
        jw_key_uint(&m, "received_samples", samples_received);
        jw_key_uint(&m, "expected_bytes", actual_bytes);
        jw_key_uint(&m, "received_bytes", bytes_on_disk);
        jw_key_str(&m, "mode", "stream");
        jw_key_str(&m, "channel_mode", mode->name);
        jw_key_uint(&m, "duration_ms", opts->duration_ns / 1000000ULL);
        jw_key_uint(&m, "timeout_ms", timeout_ms);
        jw_key_uint(&m, "elapsed_ms", t_end - t_start);
        jw_key_str(&m, "raw_path", "capture.raw");
        jw_key_str(&m, "events_path", "events.jsonl");
        jw_key_end(&m);
        jw_key_arr(&m, "enabled_channels");
        for (int i = 0; i < opts->channel_count; i++) {
            char nm[8];
            snprintf(nm, sizeof(nm), "D%d", opts->channels[i]);
            jw_str(&m, nm);
        }
        jw_arr_end(&m);
        jw_key_obj(&m, "trigger");
        jw_key_bool(&m, "configured", trigger_armed);
        if (trigger_armed) {
            jw_key_str(&m, "spec", opts->trig_spec_str ? opts->trig_spec_str : "");
            jw_key_int(&m, "position_pct", opts->trig.position_pct);
        }
        jw_key_bool(&m, "triggered", trigger_hit);
        jw_key_uint(&m, "remain_cnt", cap.trigger_remain_cnt);
        jw_key_end(&m);
        jw_key_obj(&m, "integrity");
        jw_key_bool(&m, "complete", !incomplete);
        jw_key_bool(&m, "overflow", cap.overflow);
        jw_key_bool(&m, "packet_errors", cap.packet_errors != 0);
        jw_key_bool(&m, "static_waveform", static_all);
        jw_key_str(&m, "final_state", final_state);
        jw_key_end(&m);
        jw_obj_end(&m);
        fclose(fm);
    }

    /* ---- worker: final state.json ---- */
    if (wk.active) {
        char result_fld[1024];
        snprintf(result_fld, sizeof(result_fld),
                 "{\"sample_count\":%llu,\"byte_count\":%llu,\"elapsed_ms\":%llu,"
                 "\"triggered\":%s,\"ok\":%s,\"error\":%s%s%s}",
                 (unsigned long long)samples_received,
                 (unsigned long long)bytes_on_disk,
                 (unsigned long long)(t_end - t_start),
                 trigger_hit ? "true" : "false",
                 (!err_code && !incomplete) ? "true" : "false",
                 err_code ? "{\"code\":\"" : "null",
                 err_code ? err_code : "",
                 err_code ? "\"}" : "");
        ses_write_state(wk.dir, wk.id, final_state, wk.pid, wk.config_fld,
                        bytes_on_disk, samples_received, result_fld);
        ev_log(final_state, NULL);
    }

    /* ---- stdout JSON result (capture once only) ---- */
    int exit_code;
    if (err_code && strcmp(err_code, DSLAB_E_INCOMPLETE) == 0)
        exit_code = EX_CAPTURE;
    else if (err_code)
        exit_code = strcmp(err_code, DSLAB_E_DEVICE_BUSY) == 0 ? EX_DEVICE : EX_CAPTURE;
    else if (cap.collect_error || cap.collect_detached)
        exit_code = EX_CAPTURE;
    else if (stop_by_user)
        exit_code = EX_OK; /* user stop is a controlled outcome */
    else
        exit_code = EX_OK;

    if (!wk.active) {
        JsonW w;
        jw_init(&w, stdout);
        jw_obj(&w);
        jw_key_str(&w, "schema_version", "1.0");
        jw_key_bool(&w, "ok", err_code == NULL && !cap.collect_error && !cap.collect_detached);
        jw_key_str(&w, "command", "capture.once");
        jw_key_str(&w, "state", final_state);
        jw_key_obj(&w, "device");
        jw_key_str(&w, "model", devinfo.name);
        jw_key_end(&w);
        jw_key_uint(&w, "sample_rate_hz", rate_now ? rate_now : opts->rate_hz);
        jw_key_str(&w, "channel_mode", mode->name);
        jw_key_arr(&w, "channels");
        for (int i = 0; i < opts->channel_count; i++) {
            char nm[8];
            snprintf(nm, sizeof(nm), "D%d", opts->channels[i]);
            jw_str(&w, nm);
        }
        jw_arr_end(&w);
        jw_key_obj(&w, "capture");
        jw_key_str(&w, "raw_path", path_raw);
        jw_key_str(&w, "meta_path", path_meta);
        jw_key_uint(&w, "sample_count", samples_received);
        jw_key_uint(&w, "byte_count", bytes_on_disk);
        jw_key_uint(&w, "elapsed_ms", t_end - t_start);
        jw_key_bool(&w, "triggered", trigger_hit);
        jw_key_end(&w);
        jw_key_obj(&w, "channels_detail");
        for (int i = 0; i < ch_en; i++) {
            char nm[8];
            snprintf(nm, sizeof(nm), "D%d", opts->channels[i]);
            jw_key_obj(&w, nm);
            jw_key_uint(&w, "transitions", stats ? stats[i].transitions : 0);
            jw_key_uint(&w, "rising", stats ? stats[i].rising : 0);
            jw_key_uint(&w, "falling", stats ? stats[i].falling : 0);
            jw_key_int(&w, "first_edge_sample", stats ? stats[i].first_edge_sample : -1);
            jw_key_end(&w);
        }
        jw_key_end(&w);
        jw_key_arr(&w, "warnings");
        if (incomplete) jw_str(&w, "INCOMPLETE_CAPTURE: fewer samples than expected");
        if (cap.overflow) jw_str(&w, "CAPTURE_OVERFLOW: device reported stream overflow");
        if (static_all) jw_str(&w, "no transitions on any channel (static or unconnected input?)");
        if (cap.collect_error) jw_str(&w, "collect task ended with error");
        if (cap.collect_detached) jw_str(&w, "USB device detached during capture");
        if (cap.packet_errors) jw_str(&w, "malformed packets received");
        if (timeout_hit && !err_code) jw_str(&w, "capture reached hard timeout");
        if (stop_by_user) jw_str(&w, "stopped by user request (partial data)");
        jw_arr_end(&w);
        if (err_code) {
            jw_key_obj(&w, "error");
            jw_key_str(&w, "code", err_code);
            if (incomplete) {
                jw_key_uint(&w, "expected_samples", actual_samples);
                jw_key_uint(&w, "received_samples", samples_received);
            }
            jw_key_end(&w);
        } else {
            jw_key(&w, "error");
            jw_null(&w);
        }
        jw_obj_end(&w);
        fputc('\n', stdout);
        res->printed = 1; /* main() must not print a second JSON */
    }

    free(wk.config_fld);
    free(stats);
    return exit_code;

fail_device:
    return EX_DEVICE;
}
