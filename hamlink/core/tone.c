/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tone.h"
#include <math.h>

#define FS 48000.0
#define TWO_PI 6.283185307179586

void tone_init(tone_gen_t *t, int mode) {
    t->phase = 0.0;
    t->mode = mode;
    t->pos = 0;
    t->amp = 0.0f;
}

float tone_fill(tone_gen_t *t, float target, int16_t *out, size_t n) {
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    const float slew = (float)(1.0 / (FS * 0.005));
    for (size_t i = 0; i < n; i++) {
        if (t->amp < target) { t->amp += slew; if (t->amp > target) t->amp = target; }
        else if (t->amp > target) { t->amp -= slew; if (t->amp < target) t->amp = target; }
        double f = 1200.0;
        if (t->mode == TONE_2200) f = 2200.0;
        else if (t->mode == TONE_ALT) f = ((t->pos / (uint64_t)FS) & 1u) ? 2200.0 : 1200.0;
        out[i] = t->mode == TONE_OFF ? 0 : (int16_t)lrint(sin(t->phase) * (double)t->amp * 32767.0);
        t->phase += TWO_PI * f / FS;
        if (t->phase >= TWO_PI) t->phase -= TWO_PI;
        t->pos++;
    }
    return t->amp;
}

static float to_db(double amp_fraction) {
    if (amp_fraction < 1e-6) return -120.0f;
    return (float)(20.0 * log10(amp_fraction));
}

void meter_init(meter_t *m) {
    *m = (meter_t){0};
    m->rms_db = m->peak_db = m->t1200_db = m->t2200_db = -120.0f;
}

void meter_feed(meter_t *m, const int16_t *pcm, size_t n) {
    const double k1 = 2.0 * cos(TWO_PI * 1200.0 / FS), k2 = 2.0 * cos(TWO_PI * 2200.0 / FS);
    for (size_t i = 0; i < n; i++) {
        double x = (double)pcm[i];
        double s;
        s = x + k1 * m->g1_s1 - m->g1_s2; m->g1_s2 = m->g1_s1; m->g1_s1 = s;
        s = x + k2 * m->g2_s1 - m->g2_s2; m->g2_s2 = m->g2_s1; m->g2_s1 = s;
        m->sumsq += x * x;
        int a = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i];
        if (a > m->peak) m->peak = a;
        if (a >= 32000) m->clip++;
        if (++m->n == METER_BLOCK) {
            double p1 = m->g1_s1 * m->g1_s1 + m->g1_s2 * m->g1_s2 - k1 * m->g1_s1 * m->g1_s2;
            double p2 = m->g2_s1 * m->g2_s1 + m->g2_s2 * m->g2_s2 - k2 * m->g2_s1 * m->g2_s2;
            if (p1 < 0) p1 = 0;
            if (p2 < 0) p2 = 0;
            m->t1200_db = to_db(2.0 * sqrt(p1) / METER_BLOCK / 32768.0);
            m->t2200_db = to_db(2.0 * sqrt(p2) / METER_BLOCK / 32768.0);
            m->rms_db = to_db(sqrt(m->sumsq / METER_BLOCK) / 32768.0);
            m->peak_db = to_db(m->peak / 32768.0);
            m->clipped = m->clip;
            m->blocks++;
            m->g1_s1 = m->g1_s2 = m->g2_s1 = m->g2_s2 = m->sumsq = 0;
            m->n = m->peak = m->clip = 0;
        }
    }
}
