/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Single-carrier PSK modem for mesh frames: 1200 baud on a 1700 Hz carrier (root-raised-cosine, about 920-2480 Hz),
 * QPSK = 2400 bit/s or 8-PSK = 3600 bit/s on the air. 48 kHz audio.
 *
 * A burst is: a BPSK training preamble (PTT lead-in), then for each frame a 63-symbol BPSK sync word, a 48-symbol BPSK
 * header (length and rate, each bit sent twice), and the data with 8 known pilot symbols after every 64 data symbols;
 * then a short tail. Data = [len][frame][CRC-16] plus 16 Reed-Solomon check bytes (the FX.25 code, corrects 8 bad bytes),
 * scrambled. The receiver recovers symbol timing (Gardner), trains a fractionally spaced equaliser on each sync word,
 * tracks it on the pilots and decisions, and reads the rate from the header, so it decodes QPSK and 8-PSK alike. */
#ifndef HAMLINK_PSK_H
#define HAMLINK_PSK_H
#include <stddef.h>
#include <stdint.h>

#define PSK_BAUD 1200
#define PSK_CARRIER_HZ 1700
#define PSK_MAX_FRAME 236
#define PSK_QPSK 1 /* 2 bits per symbol: 2400 bit/s */
#define PSK_8PSK 2 /* 3 bits per symbol: 3600 bit/s */

/* Upper bound on samples psk_encode_burst needs. */
size_t psk_burst_max_samples(const size_t *lens, int n, int preamble_ms, int mode);
/* Modulate the frames as one burst at peak amplitude amp (0..1). Frames longer than PSK_MAX_FRAME are skipped.
 * Returns samples written, 0 on error. */
size_t psk_encode_burst(const uint8_t *const *frames, const size_t *lens, int n, int preamble_ms, int mode, float amp,
                        int16_t *out, size_t cap);
/* Airtime of one frame (sync, header, data, pilots) in ms, for scheduling estimates. */
uint32_t psk_frame_ms(size_t len, int mode);

typedef struct psk_rx psk_rx_t;
psk_rx_t *psk_rx_create(void);
void psk_rx_destroy(psk_rx_t *r);
void psk_rx_process(psk_rx_t *r, const int16_t *pcm, size_t n);
/* Pops one received frame (RS corrected, CRC checked). Returns its length, 0 if none. */
int psk_rx_next(psk_rx_t *r, uint8_t *frame, size_t cap);
/* The same, and *mode = PSK_QPSK or PSK_8PSK: the mode the frame was sent in (one receiver decodes both). */
int psk_rx_next_mode(psk_rx_t *r, uint8_t *frame, size_t cap, int *mode);
/* 1 while a frame is being received (after its sync word): use as data carrier detect. */
int psk_rx_busy(const psk_rx_t *r);
/* {frames delivered, bytes the RS decoder corrected, frames lost after sync (header, RS or CRC failed), sync words seen} */
void psk_rx_stats(const psk_rx_t *r, uint32_t *out4);

#endif
