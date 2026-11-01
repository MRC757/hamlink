/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HDLC bit-level framing: flags, bit stuffing, CRC-16/X.25 (same FCS as AX.25).
 * Bits are stored one per byte (0/1) in transmission order. */
#ifndef HAMLINK_HDLC_H
#define HAMLINK_HDLC_H
#include <stddef.h>
#include <stdint.h>

#define HDLC_MAX_FRAME 320
#define HDLC_RAW_MAX (((HDLC_MAX_FRAME + 2) * 8 * 6) / 5 + 32)

uint16_t hdlc_crc16(const uint8_t *d, size_t n);

typedef struct {
    uint8_t *bits;
    size_t n, cap;
    int ones;
} hdlc_bits_t;

/* Return 0 on success, -1 if the bit buffer is full. */
int hdlc_put_flags(hdlc_bits_t *b, int count);
int hdlc_put_frame(hdlc_bits_t *b, const uint8_t *data, size_t len);

typedef struct {
    uint8_t pat;
    uint8_t raw[HDLC_RAW_MAX];
    size_t n;
    int overflow;
    size_t last_nb; /* after a flag: number of data bits between it and the previous flag (0 if too short/overflow) */
} hdlc_rx_t;

void hdlc_rx_init(hdlc_rx_t *h);
/* Feed one decoded (NRZI-decoded) bit. Returns frame length (>0) and copies the
 * payload (without FCS) into out when a frame with a good CRC completes. */
int hdlc_rx_bit(hdlc_rx_t *h, int bit, uint8_t *out, size_t cap);
/* Unstuff and CRC-check nb NRZI-decoded bits found between two flags. Returns payload length (>0) or 0. */
int hdlc_decode_raw(const uint8_t *raw, size_t nb, uint8_t *out, size_t cap);

#endif
