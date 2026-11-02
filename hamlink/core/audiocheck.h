/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HL_AUDIOCHECK_H
#define HL_AUDIOCHECK_H
#include <stddef.h>
#include <stdint.h>

/*
 * Spots an audio input that suppresses steady signals: a noise-cancelling ("ENC") USB-C headset-style cable, or an
 * operating-system noise suppressor. Symptom: every transmission arrives at a steady level for its first ~0.7 s, then
 * a steady 12 dB or more quieter for the rest, and nothing decodes. A real radio path does not do that.
 *
 * Works on 50 ms blocks of 48 kHz audio. Each transmission long enough to judge (2.2 s) counts as a hit (steady, then
 * steadily lower by >= 12 dB) or a miss (steady, no such drop). It reports once, after two hits and more hits than
 * misses, so one fading signal cannot trigger it.
 */
#define AC_BLOCK 2400      /* 50 ms at 48 kHz */
#define AC_JUDGE 44        /* blocks: judge a transmission 2.2 s after it starts */

typedef struct {
    double sumsq;
    int nsamp;
    float blk[AC_JUDGE];
    int nblk;
    int active, judged, below, run, started;
    float quiet_db;        /* level between transmissions (slowly tracked) */
    int hits, misses, warned;
    float last_drop_db;
} audiocheck_t;

void audiocheck_init(audiocheck_t *a);
/* Returns 1 exactly once, when the warning should be shown; the measured drop is then in a->last_drop_db. */
int audiocheck_feed(audiocheck_t *a, const int16_t *pcm, size_t n);

#endif
