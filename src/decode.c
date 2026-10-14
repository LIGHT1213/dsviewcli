#include "decode.h"
#include "unpack.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------- shared packed-bit helper ---------------- */

static uint8_t *extract_channel(const dslab_run *r, int slot)
{
    size_t bytes = (size_t)((r->total_samples + 7) / 8);
    uint8_t *dst = malloc(bytes ? bytes : 1);
    if (!dst)
        return NULL;
    memset(dst, 0, bytes ? bytes : 1);
    size_t group = (size_t)r->ch_slots * 8;
    size_t groups = r->raw_len / group;
    for (size_t g = 0; g < groups; g++) {
        const uint8_t *src = r->raw + g * group + (size_t)slot * 8;
        /* copy samples 0..63 LSB-first from src into output bitstream
         * LSB-first as well (bit k of byte k/8) for O(1) access */
        for (unsigned n = 0; n < 64; n++) {
            uint64_t s = (uint64_t)g * 64 + n;
            if (s >= r->total_samples)
                break;
            if ((src[n / 8] >> (n & 7)) & 1)
                dst[s / 8] |= (uint8_t)(1u << (s % 8));
        }
    }
    return dst;
}

static inline int bit_at(const uint8_t *bits, uint64_t idx)
{
    return (bits[idx / 8] >> (idx % 8)) & 1;
}


/* ---------------- UART ---------------- */

int decode_uart(const dslab_run *r, int slot, const uart_cfg *cfg,
                uart_result *out)
{
    memset(out, 0, sizeof(*out));
    out->samples_per_bit = (double)r->rate_hz / (double)cfg->baud;
    out->rate_ok = out->samples_per_bit >= 2.0;
    out->analyzed_samples = r->total_samples;
    if (slot < 0 || slot >= r->ch_slots || !out->rate_ok || cfg->data_bits < 5 ||
        cfg->data_bits > 8 || (cfg->parity != 0 && cfg->parity != 1 && cfg->parity != 2))
        return -1;

    uint8_t *bits = extract_channel(r, slot);
    if (!bits)
        return -1;

    uint64_t cap = 65536;
    out->frames = malloc(sizeof(uart_frame) * (size_t)cap);
    double spb = out->samples_per_bit;
    int parity_bits = cfg->parity ? 1 : 0;

    uint64_t i = 0;
    while (i + (uint64_t)(spb * 2) < r->total_samples) {
        /* hunt for a start bit: idle high, falling edge */
        if (bit_at(bits, i) == 1 || bit_at(bits, i + 1) == 1) {
            i++;
            continue;
        }
        /* candidate start at i (line low); verify start-bit mid still low */
        uint64_t mid = i + (uint64_t)(0.5 * spb);
        if (mid >= r->total_samples)
            break;
        if (bit_at(bits, mid) != 0) {
            i = mid;
            continue;
        }
        int parity_error = 0, framing_error = 0;
        uint8_t value = 0;
        for (int b = 0; b < cfg->data_bits; b++) {
            uint64_t sp = i + (uint64_t)((1.5 + b) * spb);
            if (sp >= r->total_samples) { framing_error = 1; break; }
            if (bit_at(bits, sp))
                value |= (uint8_t)(1u << b);
        }
        if (cfg->parity) {
            uint64_t sp = i + (uint64_t)((1.5 + cfg->data_bits) * spb);
            if (sp >= r->total_samples) {
                parity_error = 1;
            } else {
                int bit = bit_at(bits, sp);
                int ones = 0;
                for (int b = 0; b < cfg->data_bits; b++)
                    ones += (value >> b) & 1;
                int expect = (cfg->parity == 1)
                    ? ((ones % 2) == 0 ? 0 : 1)  /* even parity bit */
                    : ((ones % 2) == 0 ? 1 : 0); /* odd parity bit */
                if (bit != expect)
                    parity_error = 1;
            }
        }
        uint64_t stop_sp = i + (uint64_t)((1.5 + cfg->data_bits + parity_bits) * spb);
        if (stop_sp >= r->total_samples)
            framing_error = 1;
        else if (bit_at(bits, stop_sp) == 0)
            framing_error = 1; /* stop bit must be high */

        if (out->frames && out->count < cap) {
            out->frames[out->count].start_sample = i;
            out->frames[out->count].value = value;
            out->frames[out->count].parity_error = parity_error;
            out->frames[out->count].framing_error = framing_error;
            out->frames[out->count].end_sample =
                i + (uint64_t)((1.0 + cfg->data_bits + parity_bits + cfg->stop_bits) * spb);
            out->count++;
        }
        i = stop_sp + 1;
    }
    free(bits);
    return 0;
}

void decode_uart_free(uart_result *out)
{
    free(out->frames);
    out->frames = NULL;
}

/* ---------------- I2C ---------------- */

int decode_i2c(const dslab_run *r, int scl_slot, int sda_slot, i2c_result *out)
{
    memset(out, 0, sizeof(*out));
    if (scl_slot < 0 || sda_slot < 0 || scl_slot >= r->ch_slots || sda_slot >= r->ch_slots)
        return -1;
    out->analyzed_samples = r->total_samples;

    uint8_t *scl = extract_channel(r, scl_slot);
    uint8_t *sda = extract_channel(r, sda_slot);
    if (!scl || !sda) {
        free(scl);
        free(sda);
        return -1;
    }

    uint64_t cap_txns = 4096;
    out->txns = calloc((size_t)cap_txns, sizeof(i2c_txn));
    i2c_txn *cur = NULL;
    uint64_t bitpos = 0;
    int bits = 0;
    uint8_t byte = 0;
    int prev_scl = 1, prev_sda = 1;
    /* start with whatever the idle levels are; require both high before
     * the first START to avoid spurious decodes mid-bus */
    int bus_free = (bit_at(scl, 0) == 1 && bit_at(sda, 0) == 1);

    for (uint64_t i = 0; i < r->total_samples; i++) {
        int scl_b = bit_at(scl, i);
        int sda_b = bit_at(sda, i);

        if (prev_scl == 1 && scl_b == 1) {
            /* SDA transitions while SCL is (and stays) high */
            if (prev_sda == 1 && sda_b == 0) {
                /* START or RESTART */
                if (cur && cur->stop_sample == 0 && cur->byte_count > 0) {
                    /* close current txn without stop, start a new one */
                }
                if (out->count < cap_txns) {
                    cur = &out->txns[out->count++];
                    cur->start_sample = i;
                    cur->restarted = !bus_free;
                    cur->addr = -1;
                    cur->is_read = -1;
                } else {
                    cur = NULL;
                }
                bits = 0;
                byte = 0;
                bus_free = 0;
            } else if (prev_sda == 0 && sda_b == 1) {
                /* STOP */
                if (cur)
                    cur->stop_sample = i;
                cur = NULL;
                bits = 0;
                bus_free = 1;
            }
        }

        if (prev_scl == 0 && scl_b == 1) {
            /* rising edge: data bit */
            int b = sda_b;
            if (cur) {
                int idx = bits % 9;
                if (idx < 8) {
                    byte |= (uint8_t)(b << (7 - idx));
                    bits++;
                } else {
                    /* 9th bit = ACK (0 = ACK) */
                    i2c_byte *nb = realloc(cur->bytes,
                                           sizeof(i2c_byte) * (size_t)(cur->byte_count + 1));
                    if (nb) {
                        cur->bytes = nb;
                        i2c_byte *pb = &cur->bytes[cur->byte_count++];
                        pb->sample = i;
                        pb->value = byte;
                        pb->ack = b;
                        if (cur->byte_count == 1) {
                            cur->addr = (byte >> 1) & 0x7f;
                            cur->is_read = byte & 1;
                            pb->kind = I2C_BYTE_ADDR;
                        } else {
                            pb->kind = I2C_BYTE_DATA;
                        }
                    }
                    bits = 0;
                    byte = 0;
                }
            } else {
                bits = 0;
                byte = 0;
            }
        }

        prev_scl = scl_b;
        prev_sda = sda_b;
    }

    free(scl);
    free(sda);
    return 0;
}

void decode_i2c_free(i2c_result *out)
{
    for (uint64_t i = 0; i < out->count; i++)
        free(out->txns[i].bytes);
    free(out->txns);
    memset(out, 0, sizeof(*out));
}

/* ---------------- SPI ---------------- */

int decode_spi(const dslab_run *r, int clk_slot, int mosi_slot, int miso_slot,
               int cs_slot, const spi_cfg *cfg, spi_result *out)
{
    memset(out, 0, sizeof(*out));
    if (clk_slot < 0 || cs_slot < 0 || clk_slot >= r->ch_slots || cs_slot >= r->ch_slots)
        return -1;
    if (cfg->mode < 0 || cfg->mode > 3 || cfg->word_bits <= 0 || cfg->word_bits > 32)
        return -1;
    out->analyzed_samples = r->total_samples;

    uint8_t *clk = extract_channel(r, clk_slot);
    uint8_t *mosi = mosi_slot >= 0 ? extract_channel(r, mosi_slot) : NULL;
    uint8_t *miso = miso_slot >= 0 ? extract_channel(r, miso_slot) : NULL;
    if (!clk || !mosi) {
        free(clk); free(mosi); free(miso);
        return -1;
    }

    /* CPOL=0: idle low, sample on rising (mode 0) / falling (mode 1)
     * CPOL=1: idle high, sample on falling (mode 2) / rising (mode 3) */
    int cpol = cfg->mode >> 1;
    int cpha = cfg->mode & 1;
    int sample_on_rising = (cpol == 0) ? (cpha == 0) : (cpha == 1);

    uint64_t cap_txns = 4096;
    out->txns = calloc((size_t)cap_txns, sizeof(spi_txn));
    spi_txn *cur = NULL;
    uint32_t mosi_acc = 0, miso_acc = 0;
    int bit_in_word = 0;

    uint8_t *cs = extract_channel(r, cs_slot);
    if (!cs) {
        decode_spi_free(out);
        free(clk);
        free(mosi);
        free(miso);
        return -1;
    }
    int prev_clk = cpol; /* expected idle level */
    for (uint64_t i = 0; i < r->total_samples; i++) {
        int clk_b = bit_at(clk, i);
        int cs_active = bit_at(cs, i) == 0;

        if (!cs_active) {
            if (cur) {
                cur->end_sample = i;
                cur->misaligned = (cur->bit_count % (uint64_t)cfg->word_bits) != 0;
                cur = NULL;
                bit_in_word = 0;
                mosi_acc = miso_acc = 0;
            }
            prev_clk = clk_b;
            continue;
        }

        if (!cur) {
            if (out->count < cap_txns) {
                cur = &out->txns[out->count++];
                cur->start_sample = i;
                cur->miso_present = miso != NULL;
                cur->bit_count = 0;
                cur->word_count = 0;
            } else {
                cur = NULL;
            }
            bit_in_word = 0;
            mosi_acc = miso_acc = 0;
        }

        int rising = prev_clk == 0 && clk_b == 1;
        int falling = prev_clk == 1 && clk_b == 0;
        if ((sample_on_rising && rising) || (!sample_on_rising && falling)) {
            int pos = cfg->lsb_first ? bit_in_word : (cfg->word_bits - 1 - bit_in_word);
            mosi_acc |= (uint32_t)bit_at(mosi, i) << pos;
            if (miso)
                miso_acc |= (uint32_t)bit_at(miso, i) << pos;
            bit_in_word++;
            cur->bit_count++;
            if (bit_in_word == cfg->word_bits) {
                int wbytes = (cfg->word_bits + 7) / 8;
                uint8_t *nm = realloc(cur->mosi, (size_t)(cur->word_count + 1) * (size_t)wbytes);
                if (nm) {
                    cur->mosi = nm;
                    for (int b = 0; b < wbytes; b++)
                        cur->mosi[cur->word_count * (uint64_t)wbytes + (uint64_t)b] =
                            (uint8_t)(mosi_acc >> (8 * b));
                }
                if (miso) {
                    uint8_t *nmi = realloc(cur->miso, (size_t)(cur->word_count + 1) * (size_t)wbytes);
                    if (nmi) {
                        cur->miso = nmi;
                        for (int b = 0; b < wbytes; b++)
                            cur->miso[cur->word_count * (uint64_t)wbytes + (uint64_t)b] =
                                (uint8_t)(miso_acc >> (8 * b));
                    }
                }
                cur->word_count++;
                bit_in_word = 0;
                mosi_acc = miso_acc = 0;
            }
        }
        prev_clk = clk_b;
    }
    if (cur) {
        cur->end_sample = r->total_samples;
        cur->misaligned = (cur->bit_count % (uint64_t)cfg->word_bits) != 0;
    }

    free(clk);
    free(mosi);
    free(miso);
    free(cs);
    return 0;
}

void decode_spi_free(spi_result *out)
{
    for (uint64_t i = 0; i < out->count; i++) {
        free(out->txns[i].mosi);
        free(out->txns[i].miso);
    }
    free(out->txns);
    memset(out, 0, sizeof(*out));
}
