/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FX.25: Reed-Solomon forward error correction around an HDLC/AX.25 frame (TAPR FX.25 draft, as Dire Wolf and UZ7HO
 * Soundmodem implement it). On the air: a 64-bit correlation tag naming the block size, then the frame as ordinary HDLC
 * (flag, bit-stuffed frame and FCS, flag, flag pattern to fill) packed into k data bytes, then 16, 32 or 64 check bytes.
 * Every byte goes LSB first, NRZI like the rest. A receiver without FX.25 still decodes the frame inside when it arrives
 * error-free; an FX.25 receiver corrects up to nroots/2 bad bytes per block. */
#ifndef HAMLINK_FX25_H
#define HAMLINK_FX25_H
#include <stddef.h>
#include <stdint.h>
#include "hdlc.h"

#define FX25_BLOCK 255
#define FX25_MAX_DATA 239

/* Bytes a frame of len bytes (without FCS) takes on the air as FX.25 with nroots (16, 32 or 64) check bytes, tag included,
 * assuming worst-case bit stuffing. 0 if it would not fit a block (it is then sent as plain HDLC). */
size_t fx25_air_bytes(size_t len, int nroots);
/* Append the frame as one FX.25 block. 0 = done; 1 = too large for FX.25, nothing written (send plain HDLC);
 * -1 = bit buffer full. */
int fx25_put_frame(hdlc_bits_t *b, const uint8_t *frame, size_t len, int nroots);

typedef struct {
    uint64_t accum;
    int state; /* 0 hunting for a tag, 1 data bytes, 2 check bytes */
    int k, nroots, dlen, clen;
    uint8_t imask;
    uint8_t block[FX25_BLOCK];
} fx25_rx_t;

void fx25_rx_init(fx25_rx_t *r);
/* Feed one NRZI-decoded bit. When a block completes, the RS decoder succeeds and the frame inside has a good FCS, copies
 * the frame (without FCS) to out, sets *fixed to the number of bytes corrected and returns its length; else 0. */
int fx25_rx_bit(fx25_rx_t *r, int bit, uint8_t *out, size_t cap, int *fixed);

/* The Reed-Solomon code itself: GF(256) with polynomial 0x11d, generator roots alpha^1..alpha^nroots, data highest-degree
 * first (the convention of Phil Karn's library, which FX.25 implementations use). Exposed for tests. */
void fx25_rs_encode(const uint8_t *data, int nroots, uint8_t *check); /* data: 255 - nroots bytes */
int fx25_rs_decode(uint8_t *block, int nroots); /* 255 bytes in place; byte errors fixed, or -1 if uncorrectable */

#endif
