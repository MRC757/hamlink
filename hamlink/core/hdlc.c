/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hdlc.h"
#include <string.h>

uint16_t hdlc_crc16(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *d++;
        for (int i = 0; i < 8; i++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)(crc ^ 0xFFFF);
}

static int put_bit(hdlc_bits_t *b, int v) {
    if (b->n >= b->cap) return -1;
    b->bits[b->n++] = (uint8_t)v;
    return 0;
}

int hdlc_put_flags(hdlc_bits_t *b, int count) {
    static const uint8_t flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    for (int c = 0; c < count; c++)
        for (int i = 0; i < 8; i++)
            if (put_bit(b, flag[i])) return -1;
    b->ones = 0;
    return 0;
}

int hdlc_put_frame(hdlc_bits_t *b, const uint8_t *d, size_t n) {
    uint16_t crc = hdlc_crc16(d, n);
    b->ones = 0;
    for (size_t i = 0; i < n + 2; i++) {
        uint8_t byte = i < n ? d[i] : (i == n ? (uint8_t)(crc & 0xFF) : (uint8_t)(crc >> 8));
        for (int k = 0; k < 8; k++) {
            int v = (byte >> k) & 1;
            if (put_bit(b, v)) return -1;
            if (v) {
                if (++b->ones == 5) {
                    if (put_bit(b, 0)) return -1;
                    b->ones = 0;
                }
            } else {
                b->ones = 0;
            }
        }
    }
    return 0;
}

void hdlc_rx_init(hdlc_rx_t *h) { memset(h, 0, sizeof *h); }

int hdlc_decode_raw(const uint8_t *raw, size_t nb, uint8_t *out, size_t cap) {
    uint8_t tmp[HDLC_MAX_FRAME + 4];
    size_t nbytes = 0;
    int ones = 0, bitcnt = 0;
    unsigned cur = 0;
    for (size_t i = 0; i < nb; i++) {
        int v = raw[i];
        if (ones == 5) {
            if (v) return 0; /* six ones inside a frame */
            ones = 0;
            continue;        /* stuffed zero */
        }
        ones = v ? ones + 1 : 0;
        cur |= (unsigned)v << bitcnt;
        if (++bitcnt == 8) {
            if (nbytes >= sizeof tmp) return 0;
            tmp[nbytes++] = (uint8_t)cur;
            cur = 0;
            bitcnt = 0;
        }
    }
    if (bitcnt != 0 || nbytes < 3) return 0;
    size_t len = nbytes - 2;
    uint16_t crc = hdlc_crc16(tmp, len);
    uint16_t got = (uint16_t)(tmp[len] | (tmp[len + 1] << 8));
    if (crc != got || len > cap) return 0;
    memcpy(out, tmp, len);
    return (int)len;
}

int hdlc_rx_bit(hdlc_rx_t *h, int bit, uint8_t *out, size_t cap) {
    h->pat = (uint8_t)((h->pat >> 1) | ((bit & 1) << 7));
    if (h->n < HDLC_RAW_MAX) h->raw[h->n++] = (uint8_t)(bit & 1);
    else h->overflow = 1;

    if (h->pat != 0x7E) return 0;

    /* Flag seen: the last 8 raw bits are the flag itself. */
    int result = 0;
    h->last_nb = 0;
    if (!h->overflow && h->n >= 8 + 24) {
        h->last_nb = h->n - 8;
        result = hdlc_decode_raw(h->raw, h->n - 8, out, cap);
    }
    h->n = 0;
    h->overflow = 0;
    return result;
}
