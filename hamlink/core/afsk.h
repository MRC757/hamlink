/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bell 202 style 1200 baud AFSK modem (mark 1200 Hz / space 2200 Hz), 48 kHz, NRZI. */
#ifndef HAMLINK_AFSK_H
#define HAMLINK_AFSK_H
#include <stddef.h>
#include <stdint.h>
#include "hdlc.h"

#define AFSK_FS 48000
#define AFSK_BAUD 1200
#define AFSK_SPB (AFSK_FS / AFSK_BAUD) /* 40 samples per bit */
#define AFSK_RXQ 16

/* ---------------- modulator ---------------- */
typedef struct {
    double phase;
    int tone; /* 0 = mark(1200), 1 = space(2200) */
    float amp; /* 0..1 of full scale */
} afsk_mod_t;

void afsk_mod_init(afsk_mod_t *m, float amp);
/* Writes nbits*AFSK_SPB samples. Returns number of samples written. */
size_t afsk_mod_bits(afsk_mod_t *m, const uint8_t *bits, size_t nbits, int16_t *out);

/* Convenience: preamble flags + frames (2 flags between) + tail flags -> audio.
 * Returns samples written, or 0 if cap_samples is too small / allocation failed. */
size_t modem_encode_burst(const uint8_t *const *frames, const size_t *lens, int n,
                          int preamble_ms, int tail_flags, float amp,
                          int16_t *out, size_t cap_samples);
/* Upper bound on samples modem_encode_burst may need. */
size_t modem_burst_max_samples(const size_t *lens, int n, int preamble_ms, int tail_flags);
/* Same, with each frame wrapped in FX.25 (fec_nroots = 16, 32 or 64 check bytes; 0 = plain HDLC). Frames too large for
 * an FX.25 block go out as plain HDLC. Receivers always decode both. */
size_t modem_encode_burst_fec(const uint8_t *const *frames, const size_t *lens, int n,
                              int preamble_ms, int tail_flags, float amp, int fec_nroots,
                              int16_t *out, size_t cap_samples);
size_t modem_burst_max_samples_fec(const size_t *lens, int n, int preamble_ms, int tail_flags, int fec_nroots);

/* ---------------- demodulator ----------------
 * 900-2500 Hz band-pass prefilter, one-bit tone correlators, then two slicers with different mark/space weightings
 * (-1 and +2 dB), each with its own clock recovery, HDLC receiver, FX.25 decoder and bit repair. A frame decoded more
 * than once is delivered once. */
typedef struct afsk_demod afsk_demod_t;

afsk_demod_t *afsk_demod_create(void);
void afsk_demod_destroy(afsk_demod_t *d);
void afsk_demod_process(afsk_demod_t *d, const int16_t *samples, size_t n);
/* Pops one received good frame. Returns its length or 0 when none queued. */
int afsk_demod_next_frame(afsk_demod_t *d, uint8_t *buf, size_t cap);
/* Smoothed RMS input level, 0..1 of full scale (use for busy-channel detection). */
float afsk_demod_level(const afsk_demod_t *d);
uint32_t afsk_demod_frames_ok(const afsk_demod_t *d);

/* Data carrier detect: 1 while AFSK data (not just sound) is being received. Two detectors, either one sets it:
 * HDLC flags in the bit stream (on after two back-to-back flags, off after 7 ones in a row), and clock-recovery quality
 * (tone transitions landing where bit edges are expected), the latter only within ~1.5 s of an HDLC flag. Voice, noise
 * and steady tones leave it (almost always) off. */
int afsk_demod_dcd(const afsk_demod_t *d);

/* Single-bit repair (default on): when a frame fails its CRC, flip each of the 8 least certain bit decisions in turn;
 * accept only if exactly one flip yields a good CRC. Repaired frames are flagged so the caller can apply a plausibility
 * check before trusting them. */
void afsk_demod_set_repair(afsk_demod_t *d, int on);
/* Like afsk_demod_next_frame; *repaired = 1 if the frame was recovered by bit repair. */
int afsk_demod_next_frame_ex(afsk_demod_t *d, uint8_t *buf, size_t cap, int *repaired);
uint32_t afsk_demod_frames_repaired(const afsk_demod_t *d);
/* FX.25 receive counters: {blocks decoded, frames that only FEC recovered (the plain decode missed them), bytes corrected} */
void afsk_demod_fec_stats(const afsk_demod_t *d, uint32_t *out3);
uint32_t afsk_demod_repair_ambiguous(const afsk_demod_t *d);   /* rejected: more than one flip gave a good CRC */

#endif
