#include "usb_scan.h"

#include <libusb-1.0/libusb.h>
#include <string.h>

/* Subset of libsigrok4DSL dsl.h supported_DSLogic[] relevant to capture,
 * values cross-checked against upstream @2e9e2c8e (dsl.h:448-833). */
const dslab_profile dslab_profiles[] = {
    { DSL_VENDOR_ID, 0x0001, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic",        "DSLogic.fw",        "DSLogic33.bin",        "DSLogic50.bin",        16, 400000000ULL, 1 },
    { DSL_VENDOR_ID, 0x0003, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic Pro",    "DSLogicPro.fw",     "DSLogicPro.bin",       "DSLogicPro.bin",       16, 400000000ULL, 1 },
    { DSL_VENDOR_ID, 0x0020, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic PLus",   "DSLogicPlus.fw",    "DSLogicPlus.bin",      "DSLogicPlus.bin",      16, 400000000ULL, 1 },
    { DSL_VENDOR_ID, 0x0021, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic Basic",  "DSLogicBasic.fw",   "DSLogicBasic.bin",     "DSLogicBasic.bin",     16, 400000000ULL, 0 },
    { DSL_VENDOR_ID, 0x0029, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic U2Basic","DSLogicU2Basic.fw", "DSLogicU2Basic.bin",   "DSLogicU2Basic.bin",   16, 100000000ULL, 1 },
    { DSL_VENDOR_ID, 0x002A, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic U3Pro16","DSLogicU3Pro16.fw", "DSLogicU3Pro16.bin",   "DSLogicU3Pro16.bin",   16, 1000000000ULL, 1 },
    { DSL_VENDOR_ID, 0x002A, LIBUSB_SPEED_SUPER,"DreamSourceLab", "DSLogic U3Pro16","DSLogicU3Pro16.fw", "DSLogicU3Pro16.bin",   "DSLogicU3Pro16.bin",   16, 1000000000ULL, 1 },
    { DSL_VENDOR_ID, 0x002C, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic U3Pro32","DSLogicU3Pro32.fw", "DSLogicU3Pro32.bin",   "DSLogicU3Pro32.bin",   32, 1000000000ULL, 1 },
    { DSL_VENDOR_ID, 0x002C, LIBUSB_SPEED_SUPER,"DreamSourceLab", "DSLogic U3Pro32","DSLogicU3Pro32.fw", "DSLogicU3Pro32.bin",   "DSLogicU3Pro32.bin",   32, 1000000000ULL, 1 },
    { DSL_VENDOR_ID, 0x002D, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic U2Pro16","DSLogicU2Pro16.fw", "DSLogicU2Pro16.bin",   "DSLogicU2Pro16.bin",   16, 1000000000ULL, 1 },
    { DSL_VENDOR_ID, 0x0030, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic PLus",   "DSLogicPlus.fw",    "DSLogicPlus-pgl12.bin","DSLogicPlus-pgl12.bin",16, 400000000ULL, 1 },
    { DSL_VENDOR_ID, 0x0031, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic U2Basic","DSLogicU2Basic.fw", "DSLogicU2Basic-pgl12.bin","DSLogicU2Basic-pgl12.bin",16, 100000000ULL, 1 },
    { DSL_VENDOR_ID, 0x0034, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic PLus",   "DSLogicPlus-pgl12-2.fw","DSLogicPlus-pgl12-2.bin","DSLogicPlus-pgl12-2.bin",16, 400000000ULL, 1 },
    { DSL_VENDOR_ID, 0x0035, LIBUSB_SPEED_HIGH, "DreamSourceLab", "DSLogic U2Basic","DSLogicU2Basic-pgl12-2.fw","DSLogicU2Basic-pgl12-2.bin","DSLogicU2Basic-pgl12-2.bin",16, 100000000ULL, 1 },
    { 0, 0, 0, NULL, NULL, NULL, NULL, NULL, 0, 0, 0 }
};

const dslab_profile *dslab_profile_lookup(uint16_t vid, uint16_t pid, int speed)
{
    for (const dslab_profile *p = dslab_profiles; p->vid; p++)
        if (p->vid == vid && p->pid == pid && p->usb_speed == speed)
            return p;
    return NULL;
}

int dslab_usb_scan(dslab_usb_device *out, int max)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    int rc = libusb_init(&ctx);
    if (rc != 0)
        return -1;

    ssize_t n = libusb_get_device_list(ctx, &list);
    int found = 0;
    for (ssize_t i = 0; i < n && found < max; i++) {
        struct libusb_device_descriptor des;
        if (libusb_get_device_descriptor(list[i], &des) != 0)
            continue;
        if (des.idVendor != DSL_VENDOR_ID)
            continue;

        dslab_usb_device *d = &out[found++];
        memset(d, 0, sizeof(*d));
        d->vid = des.idVendor;
        d->pid = des.idProduct;
        d->bus = libusb_get_bus_number(list[i]);
        d->addr = libusb_get_device_address(list[i]);
        d->speed = libusb_get_device_speed(list[i]);
        d->profile = dslab_profile_lookup(des.idVendor, des.idProduct, d->speed);

        libusb_device_handle *h = NULL;
        rc = libusb_open(list[i], &h);
        if (rc != 0) {
            d->open_error = 1;
            d->open_error_code = rc;
            continue;
        }
        if (des.iManufacturer)
            libusb_get_string_descriptor_ascii(h, des.iManufacturer,
                (unsigned char *)d->manufacturer, sizeof(d->manufacturer));
        if (des.iProduct)
            libusb_get_string_descriptor_ascii(h, des.iProduct,
                (unsigned char *)d->product, sizeof(d->product));
        libusb_close(h);
        d->fw_loaded = !strncmp(d->manufacturer, "DreamSourceLab", 14) &&
                       !strncmp(d->product, "USB-based DSL Instrument v2", 27);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found;
}
