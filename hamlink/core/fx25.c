/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fx25.h"
#include <string.h>

/* Correlation tags from the FX.25 draft (Tag_01 .. Tag_0B). Shortened codes leave the untransmitted data bytes zero. */
static const struct {
    uint64_t tag;
    uint8_t k; /* data bytes sent */
    uint8_t nroots;
} TAGS[] = {
    {0xB74DB7DF8A532F3EULL, 239, 16}, {0x26FF60A600CC8FDEULL, 128, 16}, {0xC7DC0508F3D9B09EULL, 64, 16},
    {0x8F056EB4369660EEULL, 32, 16},  {0x6E260B1AC5835FAEULL, 223, 32}, {0xFF94DC634F1CFF4EULL, 128, 32},
    {0x1EB7B9CDBC09C00EULL, 64, 32},  {0xDBF869BD2DBB1776ULL, 32, 32},  {0x3ADB0C13DEAE2836ULL, 191, 64},
    {0xAB69DB6A543188D6ULL, 128, 64}, {0x4A4ABEC4A724B796ULL, 64, 64},
};
#define NTAGS ((int)(sizeof TAGS / sizeof TAGS[0]))
#define TAG_MAX_BIT_ERRORS 8
#define MIN_FRAME 15 /* two AX.25 addresses and a control byte; HamLink frames are longer */

/* ------------------------------------------------------------------ Reed-Solomon over GF(256) */

static uint8_t gexp[512], glog[256];
static uint8_t gen[3][65]; /* generator polynomials for 16, 32, 64 roots, highest degree first, monic */
static int rs_ready;

static int gi(int nroots) { return nroots == 16 ? 0 : nroots == 32 ? 1 : nroots == 64 ? 2 : -1; }
static uint8_t gmul(uint8_t a, uint8_t b) { return a && b ? gexp[glog[a] + glog[b]] : 0; }
static uint8_t gdiv(uint8_t a, uint8_t b) { return a ? gexp[glog[a] + 255 - glog[b]] : 0; }

static void rs_init(void) {
    if (rs_ready) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gexp[i] = (uint8_t)x;
        glog[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) gexp[i] = gexp[i - 255];
    for (int t = 0; t < 3; t++) {
        int n = 16 << t;
        uint8_t *g = gen[t];
        memset(g, 0, 65);
        g[0] = 1;
        for (int i = 0; i < n; i++) { /* multiply by (x + alpha^(1+i)) */
            uint8_t r = gexp[1 + i];
            for (int j = i + 1; j >= 1; j--) g[j] ^= gmul(g[j - 1], r);
        }
    }
    rs_ready = 1;
}

void fx25_rs_encode(const uint8_t *data, int nroots, uint8_t *check) {
    rs_init();
    const uint8_t *g = gen[gi(nroots)];
    uint8_t r[64] = {0};
    for (int i = 0; i < FX25_BLOCK - nroots; i++) {
        uint8_t fb = data[i] ^ r[0];
        memmove(r, r + 1, (size_t)(nroots - 1));
        r[nroots - 1] = 0;
        if (fb)
            for (int j = 0; j < nroots; j++) r[j] ^= gmul(fb, g[j + 1]);
    }
    memcpy(check, r, (size_t)nroots);
}

static int syndromes(const uint8_t *block, int nroots, uint8_t *s) {
    int nonzero = 0;
    for (int j = 0; j < nroots; j++) {
        uint8_t root = gexp[1 + j], v = 0;
        for (int i = 0; i < FX25_BLOCK; i++) v = gmul(v, root) ^ block[i];
        s[j] = v;
        nonzero |= v;
    }
    return nonzero;
}

int fx25_rs_decode(uint8_t *block, int nroots) {
    rs_init();
    uint8_t s[64];
    if (!syndromes(block, nroots, s)) return 0;

    /* Berlekamp-Massey: error locator lambda (lowest degree first) */
    uint8_t lam[65] = {1}, prev[65] = {1}, tmp[65];
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int r = 0; r < nroots; r++) {
        uint8_t d = s[r];
        for (int i = 1; i <= L; i++) d ^= gmul(lam[i], s[r - i]);
        if (d == 0) { m++; continue; }
        uint8_t coef = gdiv(d, b);
        if (2 * L <= r) {
            memcpy(tmp, lam, sizeof lam);
            for (int i = 0; i + m <= nroots; i++) lam[i + m] ^= gmul(coef, prev[i]);
            L = r + 1 - L;
            memcpy(prev, tmp, sizeof prev);
            b = d;
            m = 1;
        } else {
            for (int i = 0; i + m <= nroots; i++) lam[i + m] ^= gmul(coef, prev[i]);
            m++;
        }
    }
    if (2 * L > nroots) return -1;

    /* Chien search: an error in byte p (degree e = 254 - p) makes lambda(alpha^-e) = 0 */
    int pos[32], cnt = 0;
    for (int p = 0; p < FX25_BLOCK; p++) {
        uint8_t xinv = gexp[(255 - (254 - p)) % 255], v = 0;
        for (int i = L; i >= 0; i--) v = gmul(v, xinv) ^ lam[i];
        if (v == 0) {
            if (cnt == L) return -1;
            pos[cnt++] = p;
        }
    }
    if (cnt != L) return -1;

    /* Forney (first root alpha^1, so no X power factor): e = omega(X^-1) / lambda'(X^-1) */
    uint8_t om[64];
    for (int i = 0; i < nroots; i++) {
        om[i] = 0;
        for (int j = 0; j <= L && j <= i; j++) om[i] ^= gmul(lam[j], s[i - j]);
    }
    for (int k = 0; k < cnt; k++) {
        uint8_t xinv = gexp[(255 - (254 - pos[k])) % 255];
        uint8_t num = 0, den = 0, xp = 1; /* xp = xinv^(i-1) for odd i */
        for (int i = nroots - 1; i >= 0; i--) num = gmul(num, xinv) ^ om[i];
        for (int i = 1; i <= L; i += 2) {
            den ^= gmul(lam[i], xp);
            xp = gmul(xp, gmul(xinv, xinv));
        }
        if (den == 0) return -1;
        block[pos[k]] ^= gdiv(num, den);
    }
    return syndromes(block, nroots, s) ? -1 : cnt;
}

/* ------------------------------------------------------------------ framing */

/* Flag, the bit-stuffed frame and FCS, flag, then the flag pattern continued to fill all `size` bytes (LSB first).
 * Returns the bytes up to and including the closing flag, or -1 if that does not fit. */
static int stuff_block(const uint8_t *frame, size_t len, uint8_t *out, int size) {
    memset(out, 0, (size_t)size);
    int olen = 0, cap = size * 8, ones = 0;
#define PUT(v) do { if (olen >= cap) return -1; if (v) out[olen >> 3] |= (uint8_t)(1u << (olen & 7)); olen++; } while (0)
    for (int k = 0; k < 8; k++) PUT((0x7E >> k) & 1);
    uint16_t fcs = hdlc_crc16(frame, len);
    for (size_t i = 0; i < len + 2; i++) {
        uint8_t byte = i < len ? frame[i] : i == len ? (uint8_t)(fcs & 0xFF) : (uint8_t)(fcs >> 8);
        for (int k = 0; k < 8; k++) {
            int v = (byte >> k) & 1;
            PUT(v);
            if (v) {
                if (++ones == 5) { PUT(0); ones = 0; }
            } else {
                ones = 0;
            }
        }
    }
    for (int k = 0; k < 8; k++) PUT((0x7E >> k) & 1);
    int used = (olen + 7) / 8;
    for (int k = 0; olen < cap; k = (k + 1) & 7) PUT((0x7E >> k) & 1);
#undef PUT
    return used;
}

static int pick_tag(int dlen, int nroots) {
    int best = -1;
    for (int i = 0; i < NTAGS; i++)
        if (TAGS[i].nroots == nroots && dlen <= TAGS[i].k && (best < 0 || TAGS[i].k < TAGS[best].k)) best = i;
    return best;
}

size_t fx25_air_bytes(size_t len, int nroots) {
    if (gi(nroots) < 0) return 0;
    size_t bits = 16 + (len + 2) * 8 + (len + 2) * 8 / 5; /* two flags, frame and FCS, worst-case stuffing */
    int t = pick_tag((int)((bits + 7) / 8), nroots);
    return t < 0 ? 0 : 8 + TAGS[t].k + (size_t)nroots;
}

static int put_raw(hdlc_bits_t *b, const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 8; k++) {
            if (b->n >= b->cap) return -1;
            b->bits[b->n++] = (uint8_t)((bytes[i] >> k) & 1);
        }
    return 0;
}

int fx25_put_frame(hdlc_bits_t *b, const uint8_t *frame, size_t len, int nroots) {
    if (gi(nroots) < 0 || len > FX25_MAX_DATA) return 1;
    uint8_t data[FX25_MAX_DATA];
    int dlen = stuff_block(frame, len, data, FX25_MAX_DATA);
    int t = dlen < 0 ? -1 : pick_tag(dlen, nroots);
    if (t < 0) return 1;
    memset(data + TAGS[t].k, 0, (size_t)(FX25_MAX_DATA - TAGS[t].k));
    uint8_t check[64], tag[8];
    fx25_rs_encode(data, nroots, check);
    for (int i = 0; i < 8; i++) tag[i] = (uint8_t)(TAGS[t].tag >> (8 * i));
    if (put_raw(b, tag, 8) || put_raw(b, data, TAGS[t].k) || put_raw(b, check, (size_t)nroots)) return -1;
    b->ones = 0;
    return 0;
}

/* ------------------------------------------------------------------ receiver */

void fx25_rx_init(fx25_rx_t *r) {
    rs_init();
    memset(r, 0, sizeof *r);
}

static int popcount64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(v);
#else
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
#endif
}

/* Remove the flags and bit stuffing from the corrected data bytes and check the FCS (as Dire Wolf's receiver does). */
static int unstuff(const uint8_t *in, int ilen, uint8_t *out, size_t cap) {
    if (ilen < 1 || in[0] != 0x7E) return 0;
    while (ilen > 0 && in[0] == 0x7E) { in++; ilen--; }
    uint8_t buf[FX25_MAX_DATA];
    uint8_t pat = 0, acc = 0;
    int nbits = 0, n = 0;
    for (int i = 0; i < ilen; i++)
        for (int k = 0; k < 8; k++) {
            int v = (in[i] >> k) & 1;
            pat = (uint8_t)((pat >> 1) | (v << 7));
            if (pat == 0xFE) return 0; /* seven ones */
            if (v) {
                acc = (uint8_t)((acc >> 1) | 0x80);
            } else {
                if (pat == 0x7E) { /* closing flag */
                    if (nbits != 7 || n < MIN_FRAME + 2) return 0;
                    size_t len = (size_t)n - 2;
                    if (hdlc_crc16(buf, len) != (uint16_t)(buf[len] | (buf[len + 1] << 8)) || len > cap) return 0;
                    memcpy(out, buf, len);
                    return (int)len;
                }
                if ((pat >> 2) == 0x1F) continue; /* stuffed zero after five ones */
                acc >>= 1;
            }
            if (++nbits == 8) {
                if (n >= (int)sizeof buf) return 0;
                buf[n++] = acc;
                nbits = 0;
            }
        }
    return 0;
}

int fx25_rx_bit(fx25_rx_t *r, int bit, uint8_t *out, size_t cap, int *fixed) {
    if (r->state == 0) {
        r->accum = (r->accum >> 1) | ((uint64_t)(bit & 1) << 63);
        for (int i = 0; i < NTAGS; i++)
            if (popcount64(r->accum ^ TAGS[i].tag) <= TAG_MAX_BIT_ERRORS) {
                r->k = TAGS[i].k;
                r->nroots = TAGS[i].nroots;
                r->dlen = r->clen = 0;
                r->imask = 1;
                memset(r->block, 0, sizeof r->block);
                r->state = 1;
                break;
            }
        return 0;
    }
    int coffs = FX25_BLOCK - r->nroots;
    uint8_t *byte = r->state == 1 ? &r->block[r->dlen] : &r->block[coffs + r->clen];
    if (bit) *byte |= r->imask;
    r->imask = (uint8_t)(r->imask << 1);
    if (r->imask) return 0;
    r->imask = 1;
    if (r->state == 1) {
        if (++r->dlen == r->k) r->state = 2;
        return 0;
    }
    if (++r->clen < r->nroots) return 0;

    r->state = 0;
    r->accum = 0;
    int nfix = fx25_rs_decode(r->block, r->nroots);
    if (nfix < 0) return 0;
    for (int i = r->k; i < coffs; i++)
        if (r->block[i]) return 0; /* a "correction" in the bytes that were never sent: not a real block */
    int len = unstuff(r->block, r->k, out, cap);
    if (len > 0 && fixed) *fixed = nfix;
    return len;
}
