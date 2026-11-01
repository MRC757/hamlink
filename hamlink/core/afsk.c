/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "afsk.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.283185307179586

/* ------------------------------------------------------------------ */
/* Modulator                                                           */
/* ------------------------------------------------------------------ */

void afsk_mod_init(afsk_mod_t *m, float amp) {
    m->phase = 0.0;
    m->tone = 0;
    m->amp = amp;
}

size_t afsk_mod_bits(afsk_mod_t *m, const uint8_t *bits, size_t nbits, int16_t *out) {
    size_t k = 0;
    const double step[2] = {TWO_PI * 1200.0 / AFSK_FS, TWO_PI * 2200.0 / AFSK_FS};
    const double a = (double)m->amp * 32767.0;
    for (size_t i = 0; i < nbits; i++) {
        if (bits[i] == 0) m->tone ^= 1; /* NRZI: 0 = change tone, 1 = keep */
        double dph = step[m->tone];
        for (int s = 0; s < AFSK_SPB; s++) {
            out[k++] = (int16_t)lrint(sin(m->phase) * a);
            m->phase += dph;
            if (m->phase >= TWO_PI) m->phase -= TWO_PI;
        }
    }
    return k;
}

size_t modem_burst_max_samples(const size_t *lens, int n, int preamble_ms, int tail_flags) {
    size_t bits = (size_t)((preamble_ms * AFSK_BAUD / 1000) / 8 + 2) * 8;
    for (int i = 0; i < n; i++) bits += (lens[i] + 2) * 8 * 6 / 5 + 16 + 16;
    bits += (size_t)(tail_flags + 2) * 8;
    return bits * AFSK_SPB;
}

size_t modem_encode_burst(const uint8_t *const *frames, const size_t *lens, int n,
                          int preamble_ms, int tail_flags, float amp,
                          int16_t *out, size_t cap_samples) {
    size_t max_samples = modem_burst_max_samples(lens, n, preamble_ms, tail_flags);
    size_t max_bits = max_samples / AFSK_SPB;
    uint8_t *bits = (uint8_t *)malloc(max_bits);
    if (!bits) return 0;
    hdlc_bits_t b = {bits, 0, max_bits, 0};
    int pre_flags = (preamble_ms * AFSK_BAUD / 1000) / 8 + 1;
    int rc = hdlc_put_flags(&b, pre_flags);
    for (int i = 0; i < n && !rc; i++) {
        rc |= hdlc_put_frame(&b, frames[i], lens[i]);
        rc |= hdlc_put_flags(&b, 2);
    }
    rc |= hdlc_put_flags(&b, tail_flags);
    size_t samples = 0;
    if (!rc && b.n * AFSK_SPB <= cap_samples) {
        afsk_mod_t m;
        afsk_mod_init(&m, amp);
        samples = afsk_mod_bits(&m, b.bits, b.n, out);
    }
    free(bits);
    return samples;
}

/* ------------------------------------------------------------------ */
/* Demodulator                                                         */
/* ------------------------------------------------------------------ */

/* lcm(40, 240) = 240 samples: both tones repeat exactly over 240 samples. */
#define PERIOD 240
#define WIN AFSK_SPB
#define PEAK_FLOOR 0.1f
#define PEAK_DECAY 0.99992f
/* Clock recovery: faster correction while searching for a signal, slower once locked so noise does not pull it around
 * (the approach VP-Digi uses). Values tuned in tests/test_dsp.py. */
#ifndef PLL_GAIN_SEARCH
#define PLL_GAIN_SEARCH 0.25f
#endif
#ifndef PLL_GAIN_LOCKED
#define PLL_GAIN_LOCKED 0.15f
#endif
/* Clock-quality carrier detect: a transition within +/-EDGE_WIN samples of the expected edge adds Q_INC, any other
 * transition subtracts Q_DEC. Random (noise) transitions land in the window 5/40 of the time: net drift is negative. */
#define EDGE_WIN 2.5f
#define Q_INC 3
#define Q_DEC 1
#define Q_MAX 60
#define Q_ON 30
#define Q_OFF 12
/* The clock detector alone can be fooled by a steady tone whose beat with a filter runs at the bit rate (1000 Hz does
 * it). Such a tone never produces HDLC flags, so clock-based detect only counts within this many bits of a flag. */
#define FLAG_RECENT_BITS 1800
#define REPAIR_CANDIDATES 8
#define REPAIR_MIN_BITS 200   /* smallest mesh frame (23-byte header + 1 + CRC) is 208 bits */
#define REPAIR_MAX_BITS 1700  /* largest mesh frame (23 + 145 + CRC) with worst-case stuffing is about 1630 */

static float g_cm[PERIOD], g_sm[PERIOD], g_cs[PERIOD], g_ss[PERIOD];
static int g_tables_ready = 0;

static void init_tables(void) {
    if (g_tables_ready) return;
    for (int i = 0; i < PERIOD; i++) {
        double pm = TWO_PI * 1200.0 * i / AFSK_FS;
        double ps = TWO_PI * 2200.0 * i / AFSK_FS;
        g_cm[i] = (float)cos(pm);
        g_sm[i] = (float)sin(pm);
        g_cs[i] = (float)cos(ps);
        g_ss[i] = (float)sin(ps);
    }
    g_tables_ready = 1;
}

struct afsk_demod {
    float hist[WIN];
    int hpos;
    int idx; /* sample index modulo PERIOD */
    double mc, ms, sc, ss;
    float xprev, yprev;
    float lvl;
    float pm, ps; /* running peak amplitude per tone */
    int last_sign;
    float phase;
    int prev_tone;
    hdlc_rx_t hdlc;
    uint8_t q[AFSK_RXQ][HDLC_MAX_FRAME];
    uint16_t qlen[AFSK_RXQ];
    uint8_t qrep[AFSK_RXQ];
    int qhead, qtail;
    uint32_t frames_ok, frames_repaired, repair_ambiguous;
    /* carrier detect */
    int ones_run, bits_since_flag, hdlc_dcd, q_clock, clock_dcd;
    /* bit repair: certainty of the tone decision behind every data bit of the current segment (index = hdlc raw index) */
    int repair_on;
    float seg_conf[HDLC_RAW_MAX];
};

afsk_demod_t *afsk_demod_create(void) {
    init_tables();
    afsk_demod_t *d = (afsk_demod_t *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->pm = d->ps = PEAK_FLOOR;
    d->prev_tone = 1;
    d->repair_on = 1;
    d->bits_since_flag = 1 << 30;
    hdlc_rx_init(&d->hdlc);
    return d;
}

void afsk_demod_destroy(afsk_demod_t *d) { free(d); }

int afsk_demod_dcd(const afsk_demod_t *d) {
    return d->hdlc_dcd || (d->clock_dcd && d->bits_since_flag < FLAG_RECENT_BITS);
}

static void queue_frame(afsk_demod_t *d, const uint8_t *f, int len, int repaired) {
    int next = (d->qhead + 1) % AFSK_RXQ;
    if (next == d->qtail) return; /* full: drop */
    memcpy(d->q[d->qhead], f, (size_t)len);
    d->qlen[d->qhead] = (uint16_t)len;
    d->qrep[d->qhead] = (uint8_t)repaired;
    d->qhead = next;
    d->frames_ok++;
    if (repaired) d->frames_repaired++;
}

/* A segment of nb data bits between two flags failed its CRC. Data bit i = (tone[i] == tone[i-1]), so one wrong tone
 * decision corrupts data bits j and j+1. Try flipping each of the least certain tone decisions; keep the result only if
 * exactly one candidate passes the CRC (two passing means at least one is a false match, so neither can be trusted). */
static void try_repair(afsk_demod_t *d, size_t nb) {
    if (nb < REPAIR_MIN_BITS || nb > REPAIR_MAX_BITS) return;
    int cand[REPAIR_CANDIDATES], nc = 0;
    for (size_t j = 0; j + 1 < nb; j++) { /* the last data bit's tone also starts the closing flag: leave it */
        float c = d->seg_conf[j];
        if (nc < REPAIR_CANDIDATES) { cand[nc++] = (int)j; }
        else {
            int worst = 0;
            for (int k = 1; k < nc; k++) if (d->seg_conf[cand[k]] > d->seg_conf[cand[worst]]) worst = k;
            if (c < d->seg_conf[cand[worst]]) cand[worst] = (int)j;
        }
    }
    uint8_t bits[HDLC_RAW_MAX];
    uint8_t out[HDLC_MAX_FRAME], best[HDLC_MAX_FRAME];
    int passes = 0, best_len = 0;
    for (int k = 0; k < nc; k++) {
        size_t j = (size_t)cand[k];
        memcpy(bits, d->hdlc.raw, nb);       /* hdlc.raw still holds the segment: it is only reset in length */
        bits[j] ^= 1;
        bits[j + 1] ^= 1;
        int len = hdlc_decode_raw(bits, nb, out, sizeof out);
        if (len > 0) { passes++; best_len = len; memcpy(best, out, (size_t)len); }
    }
    if (passes == 1) queue_frame(d, best, best_len, 1);
    else if (passes > 1) d->repair_ambiguous++;
}

void afsk_demod_process(afsk_demod_t *d, const int16_t *s, size_t n) {
    uint8_t frame[HDLC_MAX_FRAME];
    for (size_t i = 0; i < n; i++) {
        float x = (float)s[i] * (1.0f / 32768.0f);
        float y = x - d->xprev + 0.995f * d->yprev; /* DC blocker */
        d->xprev = x;
        d->yprev = y;
        x = y;

        d->lvl += (x * x - d->lvl) * (1.0f / 480.0f);

        int k = d->idx;
        int ko = k >= WIN ? k - WIN : k + PERIOD - WIN;
        float xo = d->hist[d->hpos];
        d->hist[d->hpos] = x;
        d->hpos = (d->hpos + 1) % WIN;
        d->mc += (double)(x * g_cm[k] - xo * g_cm[ko]);
        d->ms += (double)(x * g_sm[k] - xo * g_sm[ko]);
        d->sc += (double)(x * g_cs[k] - xo * g_cs[ko]);
        d->ss += (double)(x * g_ss[k] - xo * g_ss[ko]);
        d->idx = (k + 1) % PERIOD;

        float am = (float)sqrt(d->mc * d->mc + d->ms * d->ms);
        float as = (float)sqrt(d->sc * d->sc + d->ss * d->ss);
        d->pm = fmaxf(fmaxf(am, d->pm * PEAK_DECAY), PEAK_FLOOR);
        d->ps = fmaxf(fmaxf(as, d->ps * PEAK_DECAY), PEAK_FLOOR);
        float stat = am / d->pm - as / d->ps; /* >0 => mark */
        int sign = stat > 0.0f;
        int dcd = afsk_demod_dcd(d);

        if (sign != d->last_sign) {
            /* Window-averaged detector crosses zero half a bit after the true edge. */
            float err = d->phase - (AFSK_SPB / 2.0f);
            d->phase -= err * (dcd ? PLL_GAIN_LOCKED : PLL_GAIN_SEARCH);
            d->last_sign = sign;
            d->q_clock += fabsf(err) <= EDGE_WIN ? Q_INC : -Q_DEC;
            if (d->q_clock < 0) d->q_clock = 0;
            if (d->q_clock > Q_MAX) d->q_clock = Q_MAX;
            if (d->q_clock >= Q_ON) d->clock_dcd = 1;
            else if (d->q_clock <= Q_OFF) d->clock_dcd = 0;
        }
        d->phase += 1.0f;
        if (d->phase >= (float)AFSK_SPB) {
            d->phase -= (float)AFSK_SPB;
            int tone = sign ? 0 : 1; /* 0 = mark */
            int bit = (tone == d->prev_tone) ? 1 : 0;
            d->prev_tone = tone;

            /* remember how certain this bit's tone decision was, at the index the HDLC receiver will store it */
            size_t at = d->hdlc.n;
            if (at < HDLC_RAW_MAX) d->seg_conf[at] = fabsf(stat);

            /* HDLC carrier detect */
            d->ones_run = bit ? d->ones_run + 1 : 0;
            if (d->ones_run >= 7) d->hdlc_dcd = 0; /* idle/abort: data never has more than 5 ones in a row */
            if (d->bits_since_flag < (1 << 30)) d->bits_since_flag++;

            int len = hdlc_rx_bit(&d->hdlc, bit, frame, sizeof frame);
            if (d->hdlc.n == 0 && d->hdlc.pat == 0x7E) { /* a flag just closed a segment */
                if (d->bits_since_flag == 8) d->hdlc_dcd = 1; /* two flags back to back: a preamble or frame gap */
                d->bits_since_flag = 0;
                if (len > 0) queue_frame(d, frame, len, 0);
                else if (d->repair_on && d->hdlc.last_nb) try_repair(d, d->hdlc.last_nb);
            }
        }
    }
}

void afsk_demod_set_repair(afsk_demod_t *d, int on) { d->repair_on = on ? 1 : 0; }
uint32_t afsk_demod_frames_repaired(const afsk_demod_t *d) { return d->frames_repaired; }
uint32_t afsk_demod_repair_ambiguous(const afsk_demod_t *d) { return d->repair_ambiguous; }

int afsk_demod_next_frame_ex(afsk_demod_t *d, uint8_t *buf, size_t cap, int *repaired) {
    if (repaired) *repaired = 0;
    if (d->qtail == d->qhead) return 0;
    if (repaired) *repaired = d->qrep[d->qtail];
    return afsk_demod_next_frame(d, buf, cap);
}

int afsk_demod_next_frame(afsk_demod_t *d, uint8_t *buf, size_t cap) {
    if (d->qtail == d->qhead) return 0;
    int len = d->qlen[d->qtail];
    if ((size_t)len > cap) len = (int)cap;
    memcpy(buf, d->q[d->qtail], (size_t)len);
    d->qtail = (d->qtail + 1) % AFSK_RXQ;
    return len;
}

float afsk_demod_level(const afsk_demod_t *d) { return sqrtf(d->lvl); }
uint32_t afsk_demod_frames_ok(const afsk_demod_t *d) { return d->frames_ok; }
