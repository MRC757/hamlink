/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MT63-2000 modem (long interleave) for mesh frames, around the MT63 implementation from fldigi (third_party/mt63).
 * 64 tones from 500 to 2500 Hz, 20 seven-bit characters per second, Walsh-code FEC spread over 64 symbols (3.2 s).
 * Slow but very robust against noise, fading and interference.
 *
 * Frames ride in the character stream: 0x16 0x69, then [len][frame][CRC-16 over len and frame] packed into 7-bit
 * characters, least significant bit first. Characters that MT63's own FEC gets wrong are caught by the CRC. fldigi tuned
 * to MT63-2000L at 1500 Hz decodes the signal but shows the frames as gibberish. */
#ifndef HAMLINK_MT63_H
#define HAMLINK_MT63_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT63_CHARS_PER_S 20
#define MT63_MAX_FRAME 255

typedef struct mt63_tx mt63_tx_t;
typedef struct mt63_rx mt63_rx_t;

mt63_tx_t *mt63_tx_create(void);
void mt63_tx_destroy(mt63_tx_t *t);
/* Characters one frame of len bytes takes on the air. */
size_t mt63_frame_chars(size_t len);
/* Airtime added to every burst on top of its frames: receiver lock-in (preamble_ms + 1.6 s), interleaver flush, jam. */
uint32_t mt63_burst_overhead_ms(int preamble_ms);
/* Upper bound on samples mt63_encode_burst needs (48 kHz). */
size_t mt63_burst_max_samples(const size_t *lens, int n, int preamble_ms);
/* Modulate the frames as one MT63-2000L transmission at 48 kHz, peak amplitude amp (0..1). Returns samples, 0 on error. */
size_t mt63_encode_burst(mt63_tx_t *t, const uint8_t *const *frames, const size_t *lens, int n, int preamble_ms, float amp,
                         int16_t *out, size_t cap);

mt63_rx_t *mt63_rx_create(void);
void mt63_rx_destroy(mt63_rx_t *r);
void mt63_rx_process(mt63_rx_t *r, const int16_t *pcm48k, size_t n);
/* Pops one received frame (CRC checked). Returns its length, 0 if none. */
int mt63_rx_next(mt63_rx_t *r, uint8_t *frame, size_t cap);
/* 1 while the receiver is locked to an MT63 signal (use as data carrier detect). */
int mt63_rx_locked(const mt63_rx_t *r);
uint32_t mt63_rx_frames_ok(const mt63_rx_t *r);

#ifdef __cplusplus
}
#endif
#endif
