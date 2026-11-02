/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audiocheck.h"
#include <math.h>
#include <string.h>

void audiocheck_init(audiocheck_t *a) {
    memset(a, 0, sizeof *a);
    a->quiet_db = -120.0f;
}

static void stats(const float *v, int n, float *mean, float *sd) {
    double s = 0, q = 0;
    for (int i = 0; i < n; i++) s += v[i];
    double m = s / n;
    for (int i = 0; i < n; i++) q += (v[i] - m) * (v[i] - m);
    *mean = (float)m;
    *sd = (float)sqrt(q / n);
}

/* One 50 ms block; returns 1 when the warning is due. */
static int block(audiocheck_t *a, float db) {
    if (!a->started) { a->started = 1; a->quiet_db = db; return 0; } /* first block: the starting background */
    if (a->active && ++a->run > 600) { a->active = 0; a->quiet_db = db; } /* 30 s of signal: that is the background now */
    if (!a->active) {
        if (db > a->quiet_db + 15.0f && db > -80.0f) {           /* a transmission starts */
            a->active = 1; a->judged = 0; a->nblk = 0; a->below = 0; a->run = 0;
        } else {
            a->quiet_db += (db < a->quiet_db ? 0.3f : 0.02f) * (db - a->quiet_db); /* follows quiet quickly, noise slowly */
            return 0;
        }
    }
    if (db < a->quiet_db + 8.0f) {                               /* back to the quiet level for 0.2 s: it ended */
        if (++a->below >= 4) a->active = 0;
    } else a->below = 0;
    if (a->judged || !a->active) return 0;
    a->blk[a->nblk++] = db;
    if (a->nblk < AC_JUDGE) return 0;

    a->judged = 1;
    float m1, s1, m2, s2;
    stats(a->blk + 2, 10, &m1, &s1);                             /* 0.1 - 0.6 s */
    stats(a->blk + 26, 16, &m2, &s2);                            /* 1.3 - 2.1 s */
    if (s1 > 3.0f || s2 > 3.0f) return 0;                        /* voice or a fading signal: not judged */
    if (m1 - m2 >= 12.0f) { a->hits++; a->last_drop_db = m1 - m2; }
    else a->misses++;
    if (!a->warned && a->hits >= 2 && a->hits > a->misses) { a->warned = 1; return 1; }
    return 0;
}

int audiocheck_feed(audiocheck_t *a, const int16_t *pcm, size_t n) {
    int due = 0;
    for (size_t i = 0; i < n; i++) {
        double v = pcm[i] / 32768.0;
        a->sumsq += v * v;
        if (++a->nsamp == AC_BLOCK) {
            double ms = a->sumsq / AC_BLOCK;
            due |= block(a, (float)(10.0 * log10(ms + 1e-13)));
            a->sumsq = 0;
            a->nsamp = 0;
        }
    }
    return due;
}
