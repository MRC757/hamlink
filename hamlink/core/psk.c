/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "psk.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fx25.h"
#include "hdlc.h"

#define TWO_PI 6.283185307179586
#define FS 48000
#define SPS 40            /* samples per symbol at 48 kHz */
#define DEC 5             /* the receiver works at 48 kHz / 5 = 9.6 kHz ... */
#define SPS2 8            /* ... where a symbol is 8 samples */
#define ALPHA 0.3         /* root-raised-cosine roll-off: 1200 * 1.3 = 1560 Hz wide */
#define SPAN 6            /* RRC half-length in symbols */
#define TX_TAPS (2 * SPAN * SPS + 1)
#define RX_TAPS (2 * SPAN * SPS2 + 1)
#define LP_TAPS 49        /* anti-alias low-pass before decimating to 9.6 kHz */
#define NCO_LEN 480       /* 1700 Hz at 48 kHz repeats every 480 samples */
#define SYNC_LEN 63
#define PRE_PERIOD 31
#define MIN_PRE 120       /* preamble symbols, at least */
#define TAIL 32
#define HDR_BITS 24
#define HDR_SYMS (2 * HDR_BITS)
#define GROUP 64          /* data symbols between pilot blocks */
#define PILOTS 8
#define RS_CHECK 16
#define EQ_TAPS 14        /* T/2-spaced equaliser: 7 symbols */
#define EQ_LEAD 3         /* symbols after the current one inside the equaliser window */
#define RING 4096         /* symbol history */
#define SYNC_THRESHOLD 0.30f
#define SYNC_WINDOW 6     /* symbols to look for a stronger sync peak after the first crossing */
#define TIMING_GAIN 0.04f     /* inside a frame: steady */
#define TIMING_GAIN_ACQ 0.15f /* while hunting: settle within the preamble whatever the starting phase */
#define LMS_MU 0.02f
#define PLL_KP 0.03f
#define PLL_KI 0.0003f
#define RXQ 8

typedef struct { float re, im; } cpx;
static inline cpx cmul(cpx a, cpx b) { cpx r = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; return r; }
static inline cpx cconjmul(cpx a, cpx b) { cpx r = {a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im}; return r; } /* a * conj(b) */
static inline float cabs2(cpx a) { return a.re * a.re + a.im * a.im; }

/* ------------------------------------------------------------------ shared tables */

static int8_t SYNC[SYNC_LEN], PRE[PRE_PERIOD], PIL[PILOTS];
static float tx_rrc[TX_TAPS], rx_rrc[RX_TAPS], lp[LP_TAPS], nco_c[NCO_LEN], nco_s[NCO_LEN];
static uint8_t gray_inv[8]; /* gray_inv[v] = k with k ^ (k >> 1) == v */
static int tables_ready;

/* maximal-length sequence of x^deg + x^tap + 1 as +-1 */
static void mseq(int deg, int tap, int len, int8_t *out) {
    unsigned s = 1;
    for (int i = 0; i < len; i++) {
        out[i] = (s & 1) ? -1 : 1;
        unsigned fb = (s ^ (s >> tap)) & 1;
        s = (s >> 1) | (fb << (deg - 1));
    }
}

static double rrc(double t) {
    const double a = ALPHA, pi = TWO_PI / 2;
    if (fabs(t) < 1e-9) return 1.0 - a + 4.0 * a / pi;
    if (fabs(fabs(4.0 * a * t) - 1.0) < 1e-9)
        return a / sqrt(2.0) * ((1 + 2 / pi) * sin(pi / (4 * a)) + (1 - 2 / pi) * cos(pi / (4 * a)));
    return (sin(pi * t * (1 - a)) + 4 * a * t * cos(pi * t * (1 + a))) / (pi * t * (1 - (4 * a * t) * (4 * a * t)));
}

static void init_tables(void) {
    if (tables_ready) return;
    mseq(6, 5, SYNC_LEN, SYNC);
    mseq(5, 3, PRE_PERIOD, PRE);
    int8_t p15[15];
    mseq(4, 3, 15, p15);
    memcpy(PIL, p15, PILOTS);
    for (int i = 0; i < TX_TAPS; i++) tx_rrc[i] = (float)rrc((i - SPAN * SPS) / (double)SPS);
    double e = 0;
    for (int i = 0; i < RX_TAPS; i++) { rx_rrc[i] = (float)rrc((i - SPAN * SPS2) / (double)SPS2); e += rx_rrc[i] * rx_rrc[i]; }
    for (int i = 0; i < RX_TAPS; i++) rx_rrc[i] = (float)(rx_rrc[i] / sqrt(e));
    double sum = 0;
    for (int i = 0; i < LP_TAPS; i++) {
        double n = i - (LP_TAPS - 1) / 2.0, fc = 1500.0 / FS;
        double v = n == 0 ? 2 * fc : sin(TWO_PI * fc * n) / (TWO_PI / 2 * n);
        lp[i] = (float)(v * (0.54 - 0.46 * cos(TWO_PI * i / (LP_TAPS - 1))));
        sum += lp[i];
    }
    for (int i = 0; i < LP_TAPS; i++) lp[i] = (float)(lp[i] / sum);
    for (int i = 0; i < NCO_LEN; i++) {
        nco_c[i] = (float)cos(TWO_PI * PSK_CARRIER_HZ * i / FS);
        nco_s[i] = (float)sin(TWO_PI * PSK_CARRIER_HZ * i / FS);
    }
    for (int k = 0; k < 8; k++) gray_inv[k ^ (k >> 1)] = (uint8_t)k;
    tables_ready = 1;
}

static int bits_per_sym(int mode) { return mode == PSK_8PSK ? 3 : 2; }

/* constellation point k: QPSK at 45 + 90k degrees, 8-PSK at 45k degrees */
static cpx point(int mode, int k) {
    double a = mode == PSK_8PSK ? k * TWO_PI / 8 : TWO_PI / 8 + k * TWO_PI / 4;
    cpx p = {(float)cos(a), (float)sin(a)};
    return p;
}

/* data scrambler x^9 + x^5 + 1, restarted for every frame */
typedef struct { unsigned s; } scr_t;
static int scr_next(scr_t *z) {
    int b = z->s & 1;
    unsigned fb = (z->s ^ (z->s >> 5)) & 1;
    z->s = (z->s >> 1) | (fb << 8);
    return b;
}

/* [len][frame][crc16 lo, hi] followed by 16 RS check bytes; returns the total byte count */
static size_t frame_bytes(const uint8_t *f, size_t len, uint8_t *out) {
    uint8_t blk[FX25_BLOCK];
    memset(blk, 0, sizeof blk);
    blk[0] = (uint8_t)len;
    memcpy(blk + 1, f, len);
    uint16_t crc = hdlc_crc16(blk, len + 1);
    blk[len + 1] = (uint8_t)(crc & 0xFF);
    blk[len + 2] = (uint8_t)(crc >> 8);
    size_t n = len + 3;
    fx25_rs_encode(blk, RS_CHECK, blk + FX25_BLOCK - RS_CHECK);
    memcpy(out, blk, n);
    memcpy(out + n, blk + FX25_BLOCK - RS_CHECK, RS_CHECK);
    return n + RS_CHECK;
}

static size_t data_syms(size_t len, int mode) {
    size_t bits = (len + 3 + RS_CHECK) * 8, b = (size_t)bits_per_sym(mode);
    return (bits + b - 1) / b;
}

static size_t frame_syms(size_t len, int mode) {
    size_t nd = data_syms(len, mode), groups = (nd + GROUP - 1) / GROUP;
    return SYNC_LEN + HDR_SYMS + nd + (groups ? groups - 1 : 0) * PILOTS;
}

static size_t pre_syms(int preamble_ms) {
    size_t p = (size_t)(preamble_ms < 0 ? 0 : preamble_ms) * PSK_BAUD / 1000;
    return p < MIN_PRE ? MIN_PRE : p;
}

uint32_t psk_frame_ms(size_t len, int mode) { return (uint32_t)(frame_syms(len, mode) * 1000 / PSK_BAUD); }

size_t psk_burst_max_samples(const size_t *lens, int n, int preamble_ms, int mode) {
    size_t s = pre_syms(preamble_ms) + TAIL;
    for (int i = 0; i < n; i++)
        if (lens[i] && lens[i] <= PSK_MAX_FRAME) s += frame_syms(lens[i], mode);
    return (s + 2 * SPAN + 2) * SPS;
}

/* ------------------------------------------------------------------ transmitter */

size_t psk_encode_burst(const uint8_t *const *frames, const size_t *lens, int n, int preamble_ms, int mode, float amp,
                        int16_t *out, size_t cap) {
    init_tables();
    if (mode != PSK_QPSK && mode != PSK_8PSK) return 0;
    size_t need = psk_burst_max_samples(lens, n, preamble_ms, mode);
    if (need > cap) return 0;
    size_t maxsym = need / SPS;
    cpx *sym = (cpx *)calloc(maxsym, sizeof(cpx));
    if (!sym) return 0;
    size_t ns = 0;
    size_t pre = pre_syms(preamble_ms);
    int off = (int)((30 - (long)((pre - 1) % PRE_PERIOD) + PRE_PERIOD) % PRE_PERIOD); /* last preamble symbol = PRE[30] */
    for (size_t i = 0; i < pre; i++) sym[ns++].re = PRE[(i + (size_t)off) % PRE_PERIOD];
    int bps = bits_per_sym(mode);
    for (int f = 0; f < n; f++) {
        if (!lens[f] || lens[f] > PSK_MAX_FRAME) continue;
        for (int i = 0; i < SYNC_LEN; i++) sym[ns++].re = SYNC[i];
        uint8_t hdr[3] = {(uint8_t)lens[f], (uint8_t)mode, (uint8_t)(lens[f] ^ mode ^ 0x5A)};
        for (int i = 0; i < HDR_BITS; i++) {
            float v = ((hdr[i / 8] >> (i % 8)) & 1) ? -1.0f : 1.0f;
            sym[ns++].re = v;
            sym[ns++].re = v;
        }
        uint8_t bytes[FX25_BLOCK];
        size_t nb = frame_bytes(frames[f], lens[f], bytes), nd = data_syms(lens[f], mode);
        scr_t z = {0x1FF};
        size_t bit = 0;
        for (size_t d = 0; d < nd; d++) {
            int v = 0;
            for (int b = 0; b < bps; b++, bit++) {
                int x = bit < nb * 8 ? (bytes[bit / 8] >> (bit % 8)) & 1 : 0;
                v |= (x ^ scr_next(&z)) << b;
            }
            sym[ns++] = point(mode, gray_inv[v]);
            if ((d + 1) % GROUP == 0 && d + 1 < nd)
                for (int p = 0; p < PILOTS; p++) sym[ns++].re = PIL[p];
        }
    }
    for (int i = 0; i < TAIL; i++) sym[ns++].re = PRE[i % PRE_PERIOD];

    size_t len = (ns + 2 * SPAN) * SPS;
    float *bi = (float *)calloc(len, sizeof(float)), *bq = (float *)calloc(len, sizeof(float));
    if (!bi || !bq) { free(sym); free(bi); free(bq); return 0; }
    for (size_t k = 0; k < ns; k++) {
        size_t c = k * SPS; /* tap 0 of symbol k lands here; its centre at (k + SPAN) * SPS */
        for (int t = 0; t < TX_TAPS; t++) { bi[c + t] += sym[k].re * tx_rrc[t]; bq[c + t] += sym[k].im * tx_rrc[t]; }
    }
    float peak = 1e-9f;
    for (size_t i = 0; i < len; i++) {
        float v = bi[i] * nco_c[i % NCO_LEN] - bq[i] * nco_s[i % NCO_LEN];
        bi[i] = v;
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    float scale = amp * 32767.0f / peak;
    for (size_t i = 0; i < len; i++) out[i] = (int16_t)lrintf(bi[i] * scale);
    free(sym); free(bi); free(bq);
    return len;
}

/* ------------------------------------------------------------------ receiver */

enum { HUNT = 0, HEADER = 1, DATA = 2 };

struct psk_rx {
    /* front end: mix to baseband, low-pass, decimate to 9.6 kHz, matched filter, AGC */
    cpx lph[LP_TAPS];
    int lpp, nco, dec;
    cpx mfh[RX_TAPS];
    int mfp;
    float agc;
    /* symbol timing (Gardner), on the 9.6 kHz stream */
    cpx tb[64];
    long nin;
    double tnext;
    cpx prev_on;
    /* symbols: on-time and half-symbol-earlier samples */
    cpx zon[RING], zmid[RING];
    long K;
    /* framing and equaliser */
    int state, trained;
    float mprev;
    int cand;           /* symbols since the sync metric first crossed the threshold (0 = none) */
    float best;         /* strongest metric in that window ... */
    long best_ks;       /* ... and where its sync word ended */
    long ks, j;
    cpx w[EQ_TAPS];
    float phi, dphi;
    int mode, len, pil;
    size_t nd, dcount;
    float hsoft[HDR_SYMS];
    int hn;
    uint8_t bits[FX25_BLOCK * 8];
    size_t nbits;
    /* output */
    uint8_t q[RXQ][PSK_MAX_FRAME];
    int qlen[RXQ], qmode[RXQ], qh, qt;
    uint32_t ok, fixed, lost, syncs;
};

psk_rx_t *psk_rx_create(void) {
    init_tables();
    psk_rx_t *r = (psk_rx_t *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->agc = 1e-6f;
    r->tnext = SPS2;
    return r;
}

void psk_rx_destroy(psk_rx_t *r) { free(r); }
int psk_rx_busy(const psk_rx_t *r) { return r && r->state != HUNT; }
void psk_rx_stats(const psk_rx_t *r, uint32_t *out4) { out4[0] = r->ok; out4[1] = r->fixed; out4[2] = r->lost; out4[3] = r->syncs; }

int psk_rx_next(psk_rx_t *r, uint8_t *frame, size_t cap) { return psk_rx_next_mode(r, frame, cap, NULL); }

int psk_rx_next_mode(psk_rx_t *r, uint8_t *frame, size_t cap, int *mode) {
    if (r->qt == r->qh) return 0;
    if (mode) *mode = r->qmode[r->qt];
    int len = r->qlen[r->qt];
    if ((size_t)len > cap) len = (int)cap;
    memcpy(frame, r->q[r->qt], (size_t)len);
    r->qt = (r->qt + 1) % RXQ;
    return len;
}

#define Z(arr, i) (arr)[(size_t)(i) & (RING - 1)]

static void regressor(const psk_rx_t *r, long k, cpx *u) {
    for (int t = 0; t < EQ_TAPS / 2; t++) {
        u[2 * t] = Z(r->zon, k + EQ_LEAD - t);
        u[2 * t + 1] = Z(r->zmid, k + EQ_LEAD - t);
    }
}

static cpx equalise(const psk_rx_t *r, const cpx *u) {
    cpx y = {0, 0};
    for (int i = 0; i < EQ_TAPS; i++) { cpx p = cmul(r->w[i], u[i]); y.re += p.re; y.im += p.im; }
    return y;
}

/* Least-squares equaliser from the sync word: solve (sum conj(u) u^T + lambda I) w = sum conj(u) d. */
static int train(psk_rx_t *r) {
    double A[EQ_TAPS][EQ_TAPS][2], b[EQ_TAPS][2];
    memset(A, 0, sizeof A);
    memset(b, 0, sizeof b);
    cpx u[EQ_TAPS];
    for (int i = 0; i < SYNC_LEN; i++) {
        regressor(r, r->ks - (SYNC_LEN - 1) + i, u);
        double d = SYNC[i];
        for (int p = 0; p < EQ_TAPS; p++) {
            for (int q = 0; q < EQ_TAPS; q++) { /* conj(u_p) * u_q */
                A[p][q][0] += (double)u[p].re * u[q].re + (double)u[p].im * u[q].im;
                A[p][q][1] += (double)u[p].re * u[q].im - (double)u[p].im * u[q].re;
            }
            b[p][0] += u[p].re * d;
            b[p][1] += -u[p].im * d;
        }
    }
    double tr = 0;
    for (int p = 0; p < EQ_TAPS; p++) tr += A[p][p][0];
    for (int p = 0; p < EQ_TAPS; p++) A[p][p][0] += 1e-3 * tr / EQ_TAPS + 1e-9;
    /* complex Gaussian elimination with partial pivoting */
    for (int c = 0; c < EQ_TAPS; c++) {
        int piv = c;
        for (int i = c + 1; i < EQ_TAPS; i++)
            if (A[i][c][0] * A[i][c][0] + A[i][c][1] * A[i][c][1] > A[piv][c][0] * A[piv][c][0] + A[piv][c][1] * A[piv][c][1]) piv = i;
        if (piv != c) {
            for (int q = 0; q < EQ_TAPS; q++) for (int z = 0; z < 2; z++) { double t = A[c][q][z]; A[c][q][z] = A[piv][q][z]; A[piv][q][z] = t; }
            for (int z = 0; z < 2; z++) { double t = b[c][z]; b[c][z] = b[piv][z]; b[piv][z] = t; }
        }
        double pr = A[c][c][0], pi = A[c][c][1], pm = pr * pr + pi * pi;
        if (pm < 1e-18) return -1;
        for (int i = c + 1; i < EQ_TAPS; i++) {
            /* f = A[i][c] / A[c][c] */
            double fr = (A[i][c][0] * pr + A[i][c][1] * pi) / pm, fi = (A[i][c][1] * pr - A[i][c][0] * pi) / pm;
            for (int q = c; q < EQ_TAPS; q++) {
                A[i][q][0] -= fr * A[c][q][0] - fi * A[c][q][1];
                A[i][q][1] -= fr * A[c][q][1] + fi * A[c][q][0];
            }
            b[i][0] -= fr * b[c][0] - fi * b[c][1];
            b[i][1] -= fr * b[c][1] + fi * b[c][0];
        }
    }
    double x[EQ_TAPS][2];
    for (int c = EQ_TAPS - 1; c >= 0; c--) {
        double sr = b[c][0], si = b[c][1];
        for (int q = c + 1; q < EQ_TAPS; q++) {
            sr -= A[c][q][0] * x[q][0] - A[c][q][1] * x[q][1];
            si -= A[c][q][0] * x[q][1] + A[c][q][1] * x[q][0];
        }
        double pr = A[c][c][0], pi = A[c][c][1], pm = pr * pr + pi * pi;
        x[c][0] = (sr * pr + si * pi) / pm;
        x[c][1] = (si * pr - sr * pi) / pm;
    }
    for (int i = 0; i < EQ_TAPS; i++) { r->w[i].re = (float)x[i][0]; r->w[i].im = (float)x[i][1]; }
    r->phi = r->dphi = 0;
    return 0;
}

/* Equalise symbol k, adapt toward reference d (or the nearest point when d is NULL), return the phase-corrected value. */
static cpx track(psk_rx_t *r, long k, const cpx *dref, cpx *decided) {
    cpx u[EQ_TAPS];
    regressor(r, k, u);
    cpx y = equalise(r, u);
    cpx rot = {cosf(r->phi), -sinf(r->phi)};
    cpx yr = cmul(y, rot);
    cpx d;
    if (dref) d = *dref;
    else if (r->state == HEADER) { d.re = yr.re >= 0 ? 1.0f : -1.0f; d.im = 0; }
    else {
        double a = atan2(yr.im, yr.re);
        int k8 = r->mode == PSK_8PSK ? ((int)lround(a / (TWO_PI / 8)) % 8 + 8) % 8
                                     : ((int)lround((a - TWO_PI / 8) / (TWO_PI / 4)) % 4 + 4) % 4;
        d = point(r->mode, k8);
    }
    if (decided) *decided = d;
    /* phase tracking (decision directed) */
    float pe = cconjmul(yr, d).im / (cabs2(d) + 1e-9f);
    r->dphi += PLL_KI * pe;
    r->phi += PLL_KP * pe + r->dphi;
    /* normalised LMS on the equaliser, error in the unrotated domain */
    cpx unrot = {cosf(r->phi), sinf(r->phi)};
    cpx dd = cmul(d, unrot);
    cpx e = {dd.re - y.re, dd.im - y.im};
    float p = 1e-6f;
    for (int i = 0; i < EQ_TAPS; i++) p += cabs2(u[i]);
    float mu = LMS_MU / p;
    for (int i = 0; i < EQ_TAPS; i++) { /* w += mu * e * conj(u) */
        cpx g = cconjmul(e, u[i]);
        r->w[i].re += mu * g.re;
        r->w[i].im += mu * g.im;
    }
    return yr;
}

static void finish_frame(psk_rx_t *r) {
    size_t n = (size_t)r->len + 3, total = n + RS_CHECK;
    uint8_t bytes[FX25_BLOCK];
    memset(bytes, 0, sizeof bytes);
    scr_t z = {0x1FF};
    for (size_t i = 0; i < total * 8; i++) bytes[i / 8] |= (uint8_t)((r->bits[i] ^ scr_next(&z)) << (i % 8));
    uint8_t blk[FX25_BLOCK];
    memset(blk, 0, sizeof blk);
    memcpy(blk, bytes, n);
    memcpy(blk + FX25_BLOCK - RS_CHECK, bytes + n, RS_CHECK);
    int fx = fx25_rs_decode(blk, RS_CHECK);
    int ok = fx >= 0;
    for (size_t i = n; ok && i < FX25_BLOCK - RS_CHECK; i++) if (blk[i]) ok = 0; /* "corrected" a byte never sent */
    if (ok && (blk[0] != r->len || hdlc_crc16(blk, n - 2) != (uint16_t)(blk[n - 2] | (blk[n - 1] << 8)))) ok = 0;
    int next = (r->qh + 1) % RXQ;
    if (ok && next != r->qt) {
        memcpy(r->q[r->qh], blk + 1, (size_t)r->len);
        r->qlen[r->qh] = r->len;
        r->qmode[r->qh] = r->mode;
        r->qh = next;
        r->ok++;
        r->fixed += (uint32_t)fx;
    } else if (!ok) {
        r->lost++;
    }
    r->state = HUNT;
    r->mprev = 0; r->cand = 0;
}

static void process_symbol(psk_rx_t *r, long k) {
    cpx d;
    if (r->state == HEADER) {
        cpx y = track(r, k, NULL, &d);
        r->hsoft[r->hn++] = y.re;
        if (r->hn < HDR_SYMS) return;
        uint8_t h[3] = {0, 0, 0};
        for (int i = 0; i < HDR_BITS; i++)
            if (r->hsoft[2 * i] + r->hsoft[2 * i + 1] < 0) h[i / 8] |= (uint8_t)(1 << (i % 8));
        if (h[2] != (uint8_t)(h[0] ^ h[1] ^ 0x5A) || (h[1] != PSK_QPSK && h[1] != PSK_8PSK) || h[0] == 0 ||
            h[0] > PSK_MAX_FRAME) {
            r->lost++;
            r->state = HUNT;
            r->mprev = 0; r->cand = 0;
            return;
        }
        r->len = h[0];
        r->mode = h[1];
        r->nd = data_syms((size_t)r->len, r->mode);
        r->dcount = 0;
        r->nbits = 0;
        r->pil = 0;
        r->state = DATA;
        return;
    }
    if (r->pil > 0) { /* known pilot symbol */
        cpx ref = {(float)PIL[PILOTS - r->pil], 0};
        track(r, k, &ref, NULL);
        r->pil--;
        return;
    }
    track(r, k, NULL, &d);
    double a = atan2(d.im, d.re);
    int bps = bits_per_sym(r->mode);
    int kk = r->mode == PSK_8PSK ? ((int)lround(a / (TWO_PI / 8)) % 8 + 8) % 8
                                 : ((int)lround((a - TWO_PI / 8) / (TWO_PI / 4)) % 4 + 4) % 4;
    int v = kk ^ (kk >> 1);
    for (int b = 0; b < bps && r->nbits < sizeof r->bits; b++) r->bits[r->nbits++] = (uint8_t)((v >> b) & 1);
    r->dcount++;
    if (r->dcount == r->nd) { finish_frame(r); return; }
    if (r->dcount % GROUP == 0) r->pil = PILOTS;
}

static void new_symbol(psk_rx_t *r) {
    if (r->state == HUNT && r->K >= SYNC_LEN) {
        cpx c = {0, 0};
        float e = 1e-9f;
        for (int i = 0; i < SYNC_LEN; i++) {
            cpx z = Z(r->zon, r->K - SYNC_LEN + i);
            c.re += z.re * SYNC[i];
            c.im += z.im * SYNC[i];
            e += cabs2(z);
        }
        float m = cabs2(c) / (SYNC_LEN * e);
        /* Take the strongest metric within a few symbols of the first threshold crossing: the first local maximum can
         * be a side lobe next to the preamble. */
        if (r->cand == 0 && m > SYNC_THRESHOLD) { r->cand = 1; r->best = m; r->best_ks = r->K - 1; }
        else if (r->cand > 0) {
            if (m > r->best) { r->best = m; r->best_ks = r->K - 1; }
            if (++r->cand > SYNC_WINDOW) {
                r->cand = 0;
                r->ks = r->best_ks;
                r->syncs++;
                r->state = HEADER;
                r->trained = 0;
                r->hn = 0;
                r->mode = PSK_QPSK;
            }
        }
        r->mprev = m;
    }
    if (r->state != HUNT && !r->trained && r->K > r->ks + EQ_LEAD + 1) {
        if (train(r)) { r->state = HUNT; r->mprev = 0; r->cand = 0; r->lost++; return; }
        r->trained = 1;
        r->j = r->ks + 1;
    }
    while (r->state != HUNT && r->trained && r->j + EQ_LEAD < r->K) process_symbol(r, r->j++);
}

static cpx interp(const psk_rx_t *r, double t) {
    long i = (long)floor(t);
    float f = (float)(t - i);
    cpx a = r->tb[i & 63], b = r->tb[(i + 1) & 63];
    cpx y = {a.re + f * (b.re - a.re), a.im + f * (b.im - a.im)};
    return y;
}

static void baseband_sample(psk_rx_t *r, cpx x) {
    r->mfh[r->mfp] = x;
    r->mfp = (r->mfp + 1) % RX_TAPS;
    cpx y = {0, 0};
    for (int i = 0, p = r->mfp; i < RX_TAPS; i++, p = p + 1 == RX_TAPS ? 0 : p + 1) {
        y.re += rx_rrc[i] * r->mfh[p].re;
        y.im += rx_rrc[i] * r->mfh[p].im;
    }
    r->agc += (cabs2(y) - r->agc) * 0.002f;
    float g = 1.0f / sqrtf(r->agc + 1e-12f);
    y.re *= g;
    y.im *= g;
    r->tb[r->nin & 63] = y;
    long n = r->nin++;
    while (r->tnext + 1.0 <= (double)n) {
        cpx on = interp(r, r->tnext), mid = interp(r, r->tnext - SPS2 / 2.0);
        cpx diff = {on.re - r->prev_on.re, on.im - r->prev_on.im};
        float e = diff.re * mid.re + diff.im * mid.im; /* Gardner: Re{(y[k] - y[k-1]) conj(y[k-1/2])} */
        if (e > 1.0f) e = 1.0f;
        if (e < -1.0f) e = -1.0f;
        r->prev_on = on;
        Z(r->zon, r->K) = on;
        Z(r->zmid, r->K) = mid;
        r->K++;
        r->tnext += SPS2 - (r->state == HUNT ? TIMING_GAIN_ACQ : TIMING_GAIN) * e;
        new_symbol(r);
    }
}

void psk_rx_process(psk_rx_t *r, const int16_t *pcm, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float x = pcm[i] * (1.0f / 32768.0f);
        cpx m = {x * nco_c[r->nco], -x * nco_s[r->nco]};
        r->nco = r->nco + 1 == NCO_LEN ? 0 : r->nco + 1;
        r->lph[r->lpp] = m;
        r->lpp = r->lpp + 1 == LP_TAPS ? 0 : r->lpp + 1;
        if (++r->dec < DEC) continue;
        r->dec = 0;
        cpx y = {0, 0};
        for (int k = 0, p = r->lpp; k < LP_TAPS; k++, p = p + 1 == LP_TAPS ? 0 : p + 1) {
            y.re += lp[k] * r->lph[p].re;
            y.im += lp[k] * r->lph[p].im;
        }
        baseband_sample(r, y);
    }
}
