/*
 * dslab — CLI for DSLogic logic analyzers without the DSView GUI.
 * Part of the dsviewcli project (GPL-3.0-or-later, see docs/licensing.md).
 *
 * M1 surface: devices / capabilities / doctor / capture once
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "jsonw.h"
#include "util.h"
#include "usb_scan.h"
#include "dsl_session.h"
#include "capture.h"
#include "trigger.h"
#include "session_mgr.h"
#include "analyze_cmd.h"

#ifndef DSLAB_VERSION
#define DSLAB_VERSION "0.2.0"
#endif

static void print_error_json(const char *command, const char *code, const char *message)
{
    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 0);
    jw_key_str(&w, "command", command);
    jw_key_obj(&w, "error");
    jw_key_str(&w, "code", code);
    jw_key_str(&w, "message", message);
    jw_key_end(&w);
    jw_key_arr(&w, "warnings");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);
}

static const char *speed_name(int speed)
{
    switch (speed) {
    case 1: return "full";
    case 2: return "high";
    case 3: return "super";
    case 4: return "super-plus";
    default: return "unknown";
    }
}

/* ---------------- devices ---------------- */
static int cmd_devices(int json)
{
    dslab_usb_device devs[16];
    int n = dslab_usb_scan(devs, 16);
    if (n < 0) {
        if (json)
            print_error_json("devices", DSLAB_E_INTERNAL, "libusb init failed");
        return EX_DEVICE;
    }
    if (!json) {
        printf("%-12s %-8s %-6s %-20s %-10s %s\n",
               "BUS.ADDR", "PID", "SPEED", "MODEL", "FW-LOADED", "PRODUCT");
        for (int i = 0; i < n; i++)
            printf("%u.%-10u 0x%04x  %-6s %-20s %-10s %s\n",
                   devs[i].bus, devs[i].addr, devs[i].pid,
                   speed_name(devs[i].speed),
                   devs[i].profile ? devs[i].profile->model : "(unknown)",
                   devs[i].fw_loaded ? "yes" :
                       devs[i].open_error ? "open-failed" : "no",
                   devs[i].product);
        return EX_OK;
    }
    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 1);
    jw_key_str(&w, "command", "devices");
    jw_key_uint(&w, "count", (unsigned long long)n);
    jw_key_arr(&w, "devices");
    for (int i = 0; i < n; i++) {
        jw_obj(&w);
        jw_key_uint(&w, "bus", devs[i].bus);
        jw_key_uint(&w, "address", devs[i].addr);
        jw_key_str(&w, "vid", "2A0E");
        char pid[8];
        snprintf(pid, sizeof(pid), "0x%04x", devs[i].pid);
        jw_key_str(&w, "pid", pid);
        jw_key_str(&w, "speed", speed_name(devs[i].speed));
        jw_key_str(&w, "model", devs[i].profile ? devs[i].profile->model : "unknown");
        jw_key_str(&w, "vendor", devs[i].manufacturer);
        jw_key_str(&w, "product", devs[i].product);
        jw_key_bool(&w, "firmware_loaded", devs[i].fw_loaded);
        if (devs[i].open_error) {
            jw_key_str(&w, "open_error", libusb_error_name(devs[i].open_error_code));
            jw_key_str(&w, "hint",
                       "device present but could not be opened: busy or missing driver");
        }
        if (devs[i].profile) {
            jw_key_str(&w, "fpga_bitstream", devs[i].profile->fpga_bit33);
            jw_key_bool(&w, "capture_supported", 1);
        }
        jw_key_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);
    return n > 0 ? EX_OK : EX_DEVICE;
}

/* ---------------- capabilities ---------------- */
static int cmd_capabilities(const char *device_hint, int json)
{
    const char *command = "capabilities";
    const char *res_dir = getenv("DSLAB_RES_DIR");
    if (!res_dir || !*res_dir)
        res_dir = DSLAB_RES_DIR_DEFAULT;

    int rc = dsl_global_init(res_dir);
    if (rc != SR_OK && rc != SR_ERR_HAVE_DONE) {
        if (json)
            print_error_json(command, DSLAB_E_INTERNAL, "ds_lib_init failed");
        return EX_DEVICE;
    }

    struct ds_device_full_info info;
    memset(&info, 0, sizeof(info));
    if (dsl_activate(device_hint, &info) != SR_OK) {
        dsl_global_exit();
        if (json)
            print_error_json(command, DSLAB_E_NO_DEVICE, "no usable USB device");
        else
            fprintf(stderr, "no usable USB device\n");
        return EX_DEVICE;
    }

    int16_t vld_ch = 0, total_ch = 0;
    uint64_t hw_depth = 0;
    gboolean stream = 0;
    int16_t ch_mode = 0;
    dsl_cfg_get_i16(SR_CONF_VLD_CH_NUM, &vld_ch);
    dsl_cfg_get_i16(SR_CONF_TOTAL_CH_NUM, &total_ch);
    dsl_cfg_get_u64(SR_CONF_HW_DEPTH, &hw_depth);
    dsl_cfg_get_i16(SR_CONF_CHANNEL_MODE, &ch_mode);
    GVariant *gv = NULL;
    if (ds_get_actived_device_config(NULL, NULL, SR_CONF_STREAM, &gv) == SR_OK && gv) {
        stream = g_variant_get_boolean(gv);
        g_variant_unref(gv);
    }

    JsonW w;
    if (json)
        jw_init(&w, stdout);
    else {
        jw_init(&w, stdout);
    }
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 1);
    jw_key_str(&w, "command", command);
    jw_key_obj(&w, "device");
    jw_key_str(&w, "model", info.name);
    jw_key_str(&w, "driver", info.driver_name);
    jw_key_end(&w);
    jw_key_int(&w, "vld_ch_num", vld_ch);
    jw_key_int(&w, "total_ch_num", total_ch);
    jw_key_uint(&w, "hw_depth_bytes", hw_depth);
    jw_key_int(&w, "current_channel_mode", ch_mode);
    jw_key_bool(&w, "stream", stream);
    jw_key_bool(&w, "buffer_capable", 1);

    /* samplerate list from the driver */
    jw_key_arr(&w, "samplerates");
    GVariant *srlist = NULL;
    size_t n_rates = 0;
    if (ds_get_actived_device_config_list(NULL, SR_CONF_SAMPLERATE, &srlist) == SR_OK && srlist) {
        GVariant *arr = g_variant_lookup_value(srlist, "samplerates", G_VARIANT_TYPE("at"));
        if (arr) {
            n_rates = g_variant_n_children(arr);
            for (gsize i = 0; i < n_rates; i++) {
                GVariant *el = g_variant_get_child_value(arr, i);
                jw_uint(&w, g_variant_get_uint64(el));
                g_variant_unref(el);
            }
            g_variant_unref(arr);
        }
        g_variant_unref(srlist);
    }
    jw_arr_end(&w);

    /* channel mode list (DSL pointer-in-GVariant convention) */
    jw_key_arr(&w, "channel_modes");
    GVariant *cml = NULL;
    if (ds_get_actived_device_config_list(NULL, SR_CONF_CHANNEL_MODE, &cml) == SR_OK && cml) {
        struct sr_list_item *items =
            (struct sr_list_item *)(uintptr_t)g_variant_get_uint64(cml);
        for (; items && items->id != -1; items++) {
            jw_obj(&w);
            jw_key_int(&w, "id", items->id);
            jw_key_str(&w, "name", items->name ? items->name : "");
            jw_key_end(&w);
        }
        g_variant_unref(cml);
    }
    jw_arr_end(&w);

    jw_key_arr(&w, "warnings");
    if (n_rates == 0)
        jw_str(&w, "samplerate list unavailable");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);

    dsl_release();
    dsl_global_exit();
    return EX_OK;
}

/* ---------------- doctor ---------------- */
static int cmd_doctor(int json)
{
    const char *command = "doctor";
    const char *res_dir = getenv("DSLAB_RES_DIR");
    if (!res_dir || !*res_dir)
        res_dir = DSLAB_RES_DIR_DEFAULT;

    int checks_fail = 0;

    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 1); /* finalized below */
    jw_key_str(&w, "command", command);
    jw_key_str(&w, "dslab_version", DSLAB_VERSION);
    jw_key_str(&w, "resource_dir", res_dir);
    jw_key_str(&w, "platform",
#if defined(_WIN32)
               "windows"
#elif defined(__APPLE__)
               "macos"
#else
               "linux"
#endif
    );
    jw_key_arr(&w, "checks");

    /* check: USB enumeration */
    dslab_usb_device devs[16];
    int n = dslab_usb_scan(devs, 16);
    jw_obj(&w);
    jw_key_str(&w, "id", "usb_enumeration");
    jw_key_str(&w, "status", n < 0 ? "fail" : n == 0 ? "warn" : "ok");
    if (n < 0) checks_fail = 1;
    jw_key_int(&w, "dsl_devices_found", n < 0 ? 0 : n);
    jw_key_str(&w, "detail", n < 0 ? "libusb init failed"
                : n == 0 ? "no DreamSourceLab USB device connected"
                : "DreamSourceLab device(s) enumerated");
    jw_key_end(&w);

    /* check: resource files for each connected device profile */
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            const dslab_profile *p = devs[i].profile;
            char id[64];
            snprintf(id, sizeof(id), "resources_%04x", devs[i].pid);
            jw_obj(&w);
            jw_key_str(&w, "id", id);
            char path[1024];
            int missing_fpga = 0, missing_fw = 0;
            if (p) {
                snprintf(path, sizeof(path), "%s/%s", res_dir, p->fpga_bit33);
                FILE *f = fopen(path, "rb");
                if (!f) missing_fpga = 1;
                else fclose(f);
                snprintf(path, sizeof(path), "%s/%s", res_dir, p->firmware);
                f = fopen(path, "rb");
                if (!f) missing_fw = 1;
                else fclose(f);
            }
            const char *status = "ok";
            char detail[512];
            if (!p) {
                status = "warn";
                snprintf(detail, sizeof(detail),
                         "PID 0x%04x not in profile table", devs[i].pid);
            } else if (missing_fpga) {
                status = "fail";
                checks_fail = 1;
                snprintf(detail, sizeof(detail),
                         "FPGA bitstream missing: %s/%s", res_dir, p->fpga_bit33);
            } else if (missing_fw) {
                status = "warn";
                snprintf(detail, sizeof(detail),
                         "fw file not shipped (usually unneeded for preload devices): %s/%s",
                         res_dir, p->firmware);
            } else {
                snprintf(detail, sizeof(detail), "fw + fpga files present");
            }
            jw_key_str(&w, "status", status);
            jw_key_str(&w, "detail", detail);
            jw_key_end(&w);

            /* open errors (busy/driver) */
            if (devs[i].open_error) {
                jw_obj(&w);
                jw_key_str(&w, "id", "usb_open");
                jw_key_str(&w, "status", "fail");
                checks_fail = 1;
                char d[256];
                snprintf(d, sizeof(d), "libusb_open failed: %s (device busy or "
                         "WinUSB/libusb driver missing — see docs/device-setup.md)",
                         libusb_error_name(devs[i].open_error_code));
                jw_key_str(&w, "detail", d);
                jw_key_end(&w);
            }
        }
    }

    /* check: library init + device activation + firmware version */
    int rc = dsl_global_init(res_dir);
    if (rc != SR_OK && rc != SR_ERR_HAVE_DONE) {
        jw_obj(&w);
        jw_key_str(&w, "id", "dsl_lib_init");
        jw_key_str(&w, "status", "fail");
        checks_fail = 1;
        char d[128];
        snprintf(d, sizeof(d), "ds_lib_init rc=%d", rc);
        jw_key_str(&w, "detail", d);
        jw_key_end(&w);
    } else if (n > 0) {
        struct ds_device_full_info info;
        memset(&info, 0, sizeof(info));
        if (dsl_activate(NULL, &info) == SR_OK) {
            jw_obj(&w);
            jw_key_str(&w, "id", "dsl_activate");
            jw_key_str(&w, "status", "ok");
            char d[256];
            snprintf(d, sizeof(d), "activated '%s' (driver %s)",
                     info.name, info.driver_name);
            jw_key_str(&w, "detail", d);
            jw_key_end(&w);
            dsl_release();
        } else {
            jw_obj(&w);
            jw_key_str(&w, "id", "dsl_activate");
            jw_key_str(&w, "status", "fail");
            checks_fail = 1;
            jw_key_str(&w, "detail", "device enumerated but activation failed "
                       "(firmware/FPGA/permission problem; see stderr log)");
            jw_key_end(&w);
        }
        dsl_global_exit();
    } else {
        dsl_global_exit();
    }

    jw_arr_end(&w); /* checks */
    jw_key_arr(&w, "warnings");
    if (n == 0)
        jw_str(&w, "no hardware connected: capture cannot be verified on this host");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);
    (void)json;
    return checks_fail ? EX_DEVICE : EX_OK;
}

/* ---------------- capture ---------------- */

static void default_capture_opts(dslab_capture_opts *o)
{
    memset(o, 0, sizeof(*o));
    o->device_hint = "auto";
    o->mode = "stream";
    o->channel_mode_id = -1;
    o->rate_hz = 1000000ULL;
    o->duration_ns = 10000000ULL;
    o->json = 1;
    o->channels[0] = 0;
    o->channel_count = 1;
}

/* Parse the common capture options. Returns 0 on success; on failure the
 * error JSON has been printed and an EX_* code is returned. */
static int parse_capture_common(int argc, char **argv, dslab_capture_opts *o,
                                const char *cmdname)
{
    const char *duration_s = NULL, *rate_s = NULL;
    const char *trig_s = NULL;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc)
            o->device_hint = argv[++i];
        else if (!strcmp(argv[i], "--channels") && i + 1 < argc) {
            int n = dslab_parse_channels(argv[++i], o->channels);
            if (n <= 0) {
                print_error_json(cmdname, DSLAB_E_INTERNAL,
                                 "invalid --channels (expect e.g. D0,D1)");
                return EX_USAGE;
            }
            o->channel_count = n;
        } else if (!strcmp(argv[i], "--rate") && i + 1 < argc)
            rate_s = argv[++i];
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc)
            duration_s = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            o->mode = argv[++i];
            if (strcmp(o->mode, "stream") != 0) {
                print_error_json(cmdname, DSLAB_E_UNSUPPORTED_CONFIG,
                                 "M2 supports --mode stream only");
                return EX_USAGE;
            }
        } else if (!strcmp(argv[i], "--trigger") && i + 1 < argc)
            trig_s = argv[++i];
        else if (!strcmp(argv[i], "--trigger-position") && i + 1 < argc) {
            char *end = NULL;
            long v = strtol(argv[++i], &end, 10);
            if (end == argv[i] || v < 0 || v > 100) {
                print_error_json(cmdname, DSLAB_E_UNSUPPORTED_CONFIG,
                                 "--trigger-position must be 0..100");
                return EX_USAGE;
            }
            o->trig.position_pct = (int)v;
        } else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) {
            int ok = 0;
            uint64_t ns = dslab_parse_duration_ns(argv[++i], &ok);
            if (!ok) {
                print_error_json(cmdname, DSLAB_E_INTERNAL, "invalid --timeout");
                return EX_USAGE;
            }
            o->timeout_ms = ns / 1000000ULL;
        } else if (!strcmp(argv[i], "--output") && i + 1 < argc)
            o->output_dir = argv[++i];
        else if (!strcmp(argv[i], "--session") && i + 1 < argc)
            ++i; /* consumed by the worker pre-scan; accepted so it passes */ else if (!strcmp(argv[i], "--json"))
            o->json = 1;
        else {
            char msg[256];
            snprintf(msg, sizeof(msg), "unknown option '%s'", argv[i]);
            print_error_json(cmdname, DSLAB_E_INTERNAL, msg);
            return EX_USAGE;
        }
    }

    int ok = 0;
    if (rate_s) {
        o->rate_hz = dslab_parse_rate_hz(rate_s, &ok);
        if (!ok) {
            print_error_json(cmdname, DSLAB_E_INTERNAL, "invalid --rate");
            return EX_USAGE;
        }
    }
    if (duration_s) {
        o->duration_ns = dslab_parse_duration_ns(duration_s, &ok);
        if (!ok) {
            print_error_json(cmdname, DSLAB_E_INTERNAL, "invalid --duration");
            return EX_USAGE;
        }
    }
    if (trig_s) {
        char terr[160];
        if (dslab_trig_parse(trig_s, &o->trig, terr, sizeof(terr)) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "invalid --trigger: %s", terr);
            print_error_json(cmdname, DSLAB_E_UNSUPPORTED_CONFIG, msg);
            return EX_USAGE;
        }
        o->trig_spec_str = trig_s;
    }
    return 0;
}

static int cmd_capture_once(int argc, char **argv)
{
    dslab_capture_opts o;
    default_capture_opts(&o);
    int rc = parse_capture_common(argc, argv, &o, "capture.once");
    if (rc)
        return rc;
    dslab_capture_result res;
    rc = dslab_capture_once(&o, &res);
    if (rc != EX_OK && !res.printed && res.error_code[0])
        print_error_json("capture.once", res.error_code, res.error_msg);
    return rc;
}

/* `capture _worker` runs detached; its stdout is redirected to worker.log. */
static int cmd_capture_worker(int argc, char **argv)
{
    const char *session = NULL;
    dslab_capture_opts o;
    default_capture_opts(&o);
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--session") && i + 1 < argc)
            session = argv[++i];
    int rc = parse_capture_common(argc, argv, &o, "capture.worker");
    if (rc)
        return rc;
    if (!session || !*session) {
        fprintf(stderr, "worker: missing --session\n");
        return EX_USAGE;
    }
    char dir[512];
    if (ses_dir_for_create(session, dir, sizeof(dir)) != 0) {
        fprintf(stderr, "worker: bad session id '%s'\n", session);
        return EX_USAGE;
    }
    ses_worker_redirect_stdio(dir);
    snprintf(o.session_id, sizeof(o.session_id), "%s", session);
    o.worker_mode = 1;
    dslab_capture_result res;
    rc = dslab_capture_once(&o, &res);
    fprintf(stderr, "worker: final state=%s rc=%d%s%s\n", res.final_state, rc,
            res.error_code[0] ? " code=" : "", res.error_code);
    return rc;
}

static int cmd_capture_start(int argc, char **argv)
{
    dslab_capture_opts o;
    default_capture_opts(&o);
    int rc = parse_capture_common(argc, argv, &o, "capture.start");
    if (rc)
        return rc;

    /* Single active session at a time (device is exclusive). */
    char active[32];
    if (ses_any_active(active, sizeof(active))) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "session '%s' is still active; stop it first (dslab capture stop --session %s)",
                 active, active);
        print_error_json("capture.start", DSLAB_E_DEVICE_BUSY, msg);
        return EX_DEVICE;
    }

    char sid[32];
    ses_gen_id(sid, o.rate_hz ^ o.duration_ns);
    char dir[512];
    if (ses_dir_for_create(sid, dir, sizeof(dir)) != 0 || dslab_mkdirs(dir) != 0) {
        print_error_json("capture.start", DSLAB_E_IO, "cannot create session dir");
        return EX_CAPTURE;
    }

    /* initial state + config so `status` works immediately */
    {
        uint64_t limit = (uint64_t)(((unsigned __int128)o.rate_hz * o.duration_ns +
                                     500000000ULL) / 1000000000ULL);
        uint64_t timeout_ms = o.timeout_ms ? o.timeout_ms
                              : (o.duration_ns / 1000000ULL) * 10ULL + 5000ULL;
        char *cfg = malloc(2048);
        if (!cfg) {
            print_error_json("capture.start", DSLAB_E_IO, "oom");
            return EX_CAPTURE;
        }
        char chans[256] = "";
        size_t off = 0;
        for (int i = 0; i < o.channel_count; i++)
            off += (size_t)snprintf(chans + off, sizeof(chans) - off, "%sD%d",
                                    i ? "," : "", o.channels[i]);
        char trigstr[256] = "";
        if (o.trig.count) {
            off = 0;
            for (int i = 0; i < o.trig.count; i++)
                off += (size_t)snprintf(trigstr + off, sizeof(trigstr) - off, "%sD%d:%c",
                                        i ? "," : "", o.trig.items[i].ch_index,
                                        o.trig.items[i].trig_char);
        }
        snprintf(cfg, 2048,
                 "{\"device\":\"%s\",\"channels\":\"%s\",\"rate_hz\":%llu,"
                 "\"duration_ms\":%llu,\"limit_samples\":%llu,\"mode\":\"stream\","
                 "\"timeout_ms\":%llu,\"trigger\":{\"spec\":\"%s\",\"position_pct\":%d}}",
                 o.device_hint ? o.device_hint : "auto", chans,
                 (unsigned long long)o.rate_hz,
                 (unsigned long long)(o.duration_ns / 1000000ULL),
                 (unsigned long long)limit,
                 (unsigned long long)timeout_ms, trigstr, o.trig.position_pct);
        ses_write_state(dir, sid, "created", 0, cfg, 0, 0, NULL);
        free(cfg);
    }

    /* spawn the detached worker */
    char exe[1024];
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) {
#else
    ssize_t nl = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (nl < 0) {
#endif
        print_error_json("capture.start", DSLAB_E_INTERNAL, "cannot resolve dslab path");
        return EX_CAPTURE;
    }
#ifndef _WIN32
    exe[nl] = '\0';
#endif

    char a_device[128], a_channels[128], a_rate[64], a_duration[64], a_mode[32];
    char a_timeout[64], a_session[64];
    snprintf(a_device, sizeof(a_device), "%s", o.device_hint);
    a_channels[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < o.channel_count; i++)
        off += (size_t)snprintf(a_channels + off, sizeof(a_channels) - off, "%sD%d",
                                i ? "," : "", o.channels[i]);
    snprintf(a_rate, sizeof(a_rate), "%llu", (unsigned long long)o.rate_hz);
    snprintf(a_duration, sizeof(a_duration), "%lluns",
             (unsigned long long)o.duration_ns);
    snprintf(a_mode, sizeof(a_mode), "%s", o.mode);
    snprintf(a_timeout, sizeof(a_timeout), "%llums",
             (unsigned long long)(o.timeout_ms ? o.timeout_ms
                              : (o.duration_ns / 1000000ULL) * 10ULL + 5000ULL));
    snprintf(a_session, sizeof(a_session), "%s", sid);

    char targs[256] = "";
    if (o.trig.count) {
        off = 0;
        for (int i = 0; i < o.trig.count; i++)
            off += (size_t)snprintf(targs + off, sizeof(targs) - off, "%sD%d:%c",
                                    i ? "," : "", o.trig.items[i].ch_index,
                                    o.trig.items[i].trig_char);
    }

    char *wargv[24];
    int ai = 0;
    wargv[ai++] = exe;
    wargv[ai++] = (char *)"capture";
    wargv[ai++] = (char *)"_worker";
    wargv[ai++] = (char *)"--session";
    wargv[ai++] = a_session;
    wargv[ai++] = (char *)"--device";
    wargv[ai++] = a_device;
    wargv[ai++] = (char *)"--channels";
    wargv[ai++] = a_channels;
    wargv[ai++] = (char *)"--rate";
    wargv[ai++] = a_rate;
    wargv[ai++] = (char *)"--duration";
    wargv[ai++] = a_duration;
    wargv[ai++] = (char *)"--mode";
    wargv[ai++] = a_mode;
    wargv[ai++] = (char *)"--timeout";
    wargv[ai++] = a_timeout;
    if (o.trig.count) {
        wargv[ai++] = (char *)"--trigger";
        wargv[ai++] = targs;
        if (o.trig.position_pct != 0) {
            static char pos[16];
            snprintf(pos, sizeof(pos), "%d", o.trig.position_pct);
            wargv[ai++] = (char *)"--trigger-position";
            wargv[ai++] = pos;
        }
    }
    wargv[ai] = NULL;

    long pid = ses_spawn_detached(exe, wargv, dir);
    if (pid < 0) {
        print_error_json("capture.start", DSLAB_E_INTERNAL, "failed to spawn worker");
        return EX_CAPTURE;
    }

    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 1);
    jw_key_str(&w, "command", "capture.start");
    jw_key_str(&w, "session_id", sid);
    jw_key_str(&w, "state", "created");
    jw_key_int(&w, "worker_pid", pid);
    jw_key_str(&w, "run_dir", dir);
    jw_key_str(&w, "hint",
               "poll: dslab capture status --session <id>; stop: dslab capture stop --session <id>");
    jw_key_arr(&w, "warnings");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);
    return EX_OK;
}

static void print_session_status_json(const char *sid, const char *dir)
{
    long pid = 0;
    int64_t hb = 0;
    char *st = ses_read_state(dir, &pid, &hb);
    JsonW w;
    jw_init(&w, stdout);
    jw_obj(&w);
    jw_key_str(&w, "schema_version", "1.0");
    jw_key_bool(&w, "ok", 1);
    jw_key_str(&w, "command", "capture.status");
    jw_key_str(&w, "session_id", sid);
    jw_key_str(&w, "state", st ? st : "unknown");
    jw_key_int(&w, "worker_pid", pid);
    if (hb) {
        int64_t age = (int64_t)dslab_unix_ms() - hb;
        jw_key_int(&w, "heartbeat_age_ms", age);
    }
    /* progress / raw sizes */
    char rawpath[600];
    snprintf(rawpath, sizeof(rawpath), "%s/capture.raw", dir);
    struct stat stt;
    if (stat(rawpath, &stt) == 0)
        jw_key_uint(&w, "raw_bytes", (unsigned long long)stt.st_size);
    jw_key_str(&w, "run_dir", dir);
    jw_key_arr(&w, "warnings");
    if (st && !SES_TERMINATED(st) && hb && (int64_t)dslab_unix_ms() - hb > 5000 &&
        !ses_pid_alive(pid))
        jw_str(&w, "worker appears dead (stale heartbeat, pid not alive)");
    if (st && !strcmp(st, "timeout"))
        jw_str(&w, "capture hit its hard timeout");
    jw_arr_end(&w);
    jw_obj_end(&w);
    fputc('\n', stdout);
    free(st);
}

static int cmd_capture_status(int argc, char **argv)
{
    const char *session = NULL;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--session") && i + 1 < argc)
            session = argv[++i];
    if (!session) {
        print_error_json("capture.status", DSLAB_E_INTERNAL, "missing --session");
        return EX_USAGE;
    }
    char dir[512];
    if (ses_dir_for(session, dir, sizeof(dir)) != 0) {
        print_error_json("capture.status", "SESSION_NOT_FOUND",
                         "no such session (checked runs/<id>/state.json)");
        return EX_DEVICE;
    }
    print_session_status_json(session, dir);
    return EX_OK;
}

static int cmd_capture_stop(int argc, char **argv)
{
    const char *session = NULL;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--session") && i + 1 < argc)
            session = argv[++i];
    if (!session) {
        print_error_json("capture.stop", DSLAB_E_INTERNAL, "missing --session");
        return EX_USAGE;
    }
    char dir[512];
    if (ses_dir_for(session, dir, sizeof(dir)) != 0) {
        print_error_json("capture.stop", "SESSION_NOT_FOUND",
                         "no such session (checked runs/<id>/state.json)");
        return EX_DEVICE;
    }
    ses_request_stop(dir);
    /* wait for a terminal state (bounded) */
    for (int i = 0; i < 300; i++) {
        char *st = ses_read_state(dir, NULL, NULL);
        int done = st && SES_TERMINATED(st);
        free(st);
        if (done)
            break;
        dslab_sleep_ms(50);
    }
    print_session_status_json(session, dir);
    return EX_OK;
}

static int cmd_capture(int argc, char **argv)
{
    if (argc < 1) {
        print_error_json("capture", DSLAB_E_INTERNAL,
                         "missing capture subcommand (once|start|status|stop)");
        return EX_USAGE;
    }
    if (!strcmp(argv[0], "once"))
        return cmd_capture_once(argc - 1, argv + 1);
    if (!strcmp(argv[0], "start"))
        return cmd_capture_start(argc - 1, argv + 1);
    if (!strcmp(argv[0], "status"))
        return cmd_capture_status(argc - 1, argv + 1);
    if (!strcmp(argv[0], "stop"))
        return cmd_capture_stop(argc - 1, argv + 1);
    if (!strcmp(argv[0], "_worker"))
        return cmd_capture_worker(argc - 1, argv + 1);

    char msg[128];
    snprintf(msg, sizeof(msg), "unknown capture subcommand '%s' (once|start|status|stop)",
             argv[0]);
    print_error_json("capture", DSLAB_E_INTERNAL, msg);
    return EX_USAGE;
}

static void usage(FILE *out)
{
    fprintf(out,
        "dslab " DSLAB_VERSION " — DSLogic capture CLI (no DSView GUI needed)\n"
        "usage:\n"
        "  dslab devices [--json]\n"
        "  dslab capabilities [--device auto|<model>] --json\n"
        "  dslab doctor --json\n"
        "  dslab capture once [--device auto] [--channels D0,D1] [--rate 20M]\n"
        "                     [--duration 10ms] [--trigger 'D0:rising,D1:low']\n"
        "                     [--trigger-position 10] [--mode stream] [--timeout 15s]\n"
        "                     [--output ./runs/test01] --json\n"
        "  dslab capture start <same options> --json      # returns session_id\n"
        "  dslab capture status --session <id> --json\n"
        "  dslab capture stop  --session <id> --json\n"
        "  dslab analyze timing <run-dir> [--channel D0] --json\n"
        "  dslab decode uart|i2c|spi <run-dir> [options] --json\n"
        "  dslab assert <run-dir> --spec <file.json> --json\n"
        "  dslab version\n"
        "\n"
        "Every --json command prints exactly one JSON object on stdout;\n"
        "diagnostics go to stderr. Exit codes: 0 ok, 2 usage, 3 device error,\n"
        "4 capture error, 5 assertion failure.\n"
        "Resource dir override: DSLAB_RES_DIR env (default: built-in vendor/res).\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return EX_USAGE;
    }
    const char *cmd = argv[1];
    int json = 0;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--json"))
            json = 1;

    if (!strcmp(cmd, "version")) {
        printf("dslab %s\n", DSLAB_VERSION);
        return EX_OK;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage(stdout);
        return EX_OK;
    }
    if (!strcmp(cmd, "devices"))
        return cmd_devices(json);
    if (!strcmp(cmd, "capabilities")) {
        const char *hint = "auto";
        for (int i = 2; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--device"))
                hint = argv[i + 1];
        return cmd_capabilities(hint, json);
    }
    if (!strcmp(cmd, "doctor"))
        return cmd_doctor(json);
    if (!strcmp(cmd, "capture"))
        return cmd_capture(argc - 2, argv + 2);
    if (!strcmp(cmd, "analyze")) {
        if (argc >= 2 && !strcmp(argv[2], "timing"))
            return cmd_analyze_timing(argc - 3, argv + 3);
        print_error_json("analyze", "INTERNAL_ERROR", "usage: analyze timing <run> [opts]");
        return EX_USAGE;
    }
    if (!strcmp(cmd, "decode"))
        return cmd_decode(argc - 2, argv + 2);
    if (!strcmp(cmd, "assert"))
        return cmd_assert(argc - 2, argv + 2);
    if (!strcmp(cmd, "export"))
        return cmd_export(argc - 2, argv + 2);

    usage(stderr);
    return EX_USAGE;
}
