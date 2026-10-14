/*
 * dslab — side-effect-free libusb enumeration of supported DSL hardware.
 * Mirrors the profile table in libsigrok4DSL/hardware/DSL/dsl.h @2e9e2c8e.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_USB_SCAN_H
#define DSLAB_USB_SCAN_H

#include <stdint.h>
#include <libusb-1.0/libusb.h>

#define DSL_VENDOR_ID 0x2A0E

typedef struct {
    uint16_t vid, pid;
    int      usb_speed;       /* libusb_speed value */
    const char *vendor;
    const char *model;
    const char *firmware;     /* runtime FX2 fw file name (may be absent from res) */
    const char *fpga_bit33;
    const char *fpga_bit50;
    int      total_ch_num;
    uint64_t max_samplerate;  /* of the default stream mode */
    int      buffer_capable;  /* CAPS_FEATURE_BUF */
} dslab_profile;

extern const dslab_profile dslab_profiles[]; /* NULL-terminated */

typedef struct {
    uint16_t vid, pid;
    uint8_t  bus, addr;
    int      speed;
    char     manufacturer[64];
    char     product[64];
    const dslab_profile *profile;   /* NULL when VID matches but PID unknown */
    int      fw_loaded;             /* string descriptors say DSL v2 firmware */
    int      open_error;            /* libusb_open failed (busy/no driver) */
    int      open_error_code;
} dslab_usb_device;

/* Enumerate connected DSL devices (VID 0x2A0E). Returns count, -1 on
 * libusb init failure. */
int dslab_usb_scan(dslab_usb_device *out, int max);

const dslab_profile *dslab_profile_lookup(uint16_t vid, uint16_t pid, int speed);

#endif
