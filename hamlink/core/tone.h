/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test-tone generator and receive-level meter, used by the PTT test / calibration screens. */
#ifndef HAMLINK_TONE_H
#define HAMLINK_TONE_H
#include <stddef.h>
#include <stdint.h>

#define TONE_OFF 0
#define TONE_1200 1
#define TONE_2200 2
#define TONE_ALT 3 /* 1200 Hz for 1 s, then 2200 Hz for 1 s, repeating (phase continuous) */

typedef struct {
    double phase;
    int mode;
    uint64_t pos;
    float amp; /* current amplitude, slews toward the target to avoid clicks */
} tone_gen_t;

void tone_init(tone_gen_t *t, int mode);
/* Fill n samples at 48 kHz. Amplitude (0..1 of full scale) slews to `target` in ~5 ms per full scale
 * step, so starting from 0 fades in and a target of 0 fades out. Returns the amplitude reached. */
float tone_fill(tone_gen_t *t, float target, int16_t *out, size_t n);

/* ---- receive meter: updated every 100 ms block (4800 samples) ---- */
#define METER_BLOCK 4800
typedef struct {
    double g1_s1, g1_s2, g2_s1, g2_s2, sumsq;
    int n, peak, clip;
    /* last completed block, all in dBFS (full-scale sine = 0 dBFS; floor -120) */
    float rms_db, peak_db, t1200_db, t2200_db;
    int clipped; /* samples at/near full scale in the last block */
    int blocks;
} meter_t;

void meter_init(meter_t *m);
void meter_feed(meter_t *m, const int16_t *pcm, size_t n);

#endif
