/*
 * dslab — deterministic offline protocol decoders (M3): UART / I2C / SPI.
 * Decoders operate on saved runs (capture.raw) and never guess: unknown or
 * malformed elements are reported as errors/flags, not dropped silently.
 * Part of the dsviewcli project (GPL-3.0-or-later).
 */
#ifndef DSLAB_DECODE_H
#define DSLAB_DECODE_H

#include <stdint.h>
#include "runfile.h"

/* ---------- UART ---------- */
typedef struct {
    uint64_t baud;
    int data_bits;      /* 5..8 */
    int parity;         /* 0 none, 1 even, 2 odd */
    int stop_bits;      /* 1 or 2 */
} uart_cfg;

typedef struct {
    uint64_t start_sample;
    uint8_t value;      /* data bits (LSB-first reception) */
    int parity_error;
    int framing_error;
    uint64_t end_sample;
} uart_frame;

typedef struct {
    uart_frame *frames;
    uint64_t count;
    int rate_ok;        /* sample rate supports the baud? */
    double samples_per_bit;
    uint64_t analyzed_samples;
} uart_result;

int decode_uart(const dslab_run *r, int slot, const uart_cfg *cfg,
                uart_result *out);
void decode_uart_free(uart_result *out);

/* ---------- I2C ---------- */
typedef struct {
    uint64_t sample;
    int is_restart;
} i2c_start_ev;

typedef struct {
    uint64_t sample;
} i2c_stop_ev;

typedef enum { I2C_BYTE_ADDR, I2C_BYTE_DATA } i2c_byte_kind;

typedef struct {
    uint64_t sample;
    uint8_t value;
    int kind;           /* i2c_byte_kind */
    int ack;            /* 0 ACK, 1 NACK, -1 missing (no clock edge) */
} i2c_byte;

typedef struct {
    uint64_t start_sample;
    uint64_t stop_sample; /* 0 when unterminated */
    int restarted;
    int addr;             /* 7-bit address, -1 when unknown */
    int is_read;
    i2c_byte *bytes;
    uint64_t byte_count;
} i2c_txn;

typedef struct {
    i2c_txn *txns;
    uint64_t count;
    uint64_t analyzed_samples;
} i2c_result;

int decode_i2c(const dslab_run *r, int scl_slot, int sda_slot,
               i2c_result *out);
void decode_i2c_free(i2c_result *out);

/* ---------- SPI ---------- */
typedef struct {
    int mode;           /* 0..3 */
    int lsb_first;      /* 0 = MSB first */
    int word_bits;      /* default 8 */
} spi_cfg;

typedef struct {
    uint64_t start_sample;
    uint64_t end_sample;
    uint64_t bit_count;
    uint8_t *mosi;      /* word_bits/8 bytes per word */
    uint8_t *miso;
    uint64_t word_count;
    int miso_present;
    int misaligned;     /* bit_count % word_bits != 0 */
} spi_txn;

typedef struct {
    spi_txn *txns;
    uint64_t count;
    uint64_t analyzed_samples;
} spi_result;

int decode_spi(const dslab_run *r, int clk_slot, int mosi_slot, int miso_slot,
               int cs_slot, const spi_cfg *cfg, spi_result *out);
void decode_spi_free(spi_result *out);

#endif
