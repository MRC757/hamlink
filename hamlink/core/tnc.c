/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tnc.h"
#include <string.h>

size_t kiss_encode(uint8_t port, uint8_t command, const uint8_t *data, size_t len, uint8_t *out, size_t cap) {
    size_t k = 0;
    if (cap < 3) return 0;
    out[k++] = KISS_FEND;
    uint8_t cmd = (uint8_t)(((port & 0x0F) << 4) | (command & 0x0F));
    for (size_t i = 0; i < len + 1; i++) {
        uint8_t b = i == 0 ? cmd : data[i - 1];
        if (b == KISS_FEND || b == KISS_FESC) {
            if (k + 2 > cap) return 0;
            out[k++] = KISS_FESC;
            out[k++] = b == KISS_FEND ? KISS_TFEND : KISS_TFESC;
        } else {
            if (k + 1 > cap) return 0;
            out[k++] = b;
        }
    }
    if (k + 1 > cap) return 0;
    out[k++] = KISS_FEND;
    return k;
}

void kiss_dec_init(kiss_dec_t *d) { memset(d, 0, sizeof *d); }

void kiss_dec_feed(kiss_dec_t *d, const uint8_t *bytes, size_t n, kiss_frame_cb cb, void *user) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        if (b == KISS_FEND) {
            if (d->in_frame && d->n >= 1 && cb) {
                uint8_t cmd = d->buf[0];
                cb(user, (uint8_t)(cmd >> 4), (uint8_t)(cmd & 0x0F), d->buf + 1, d->n - 1);
            }
            d->n = 0;
            d->in_frame = 1;
            d->esc = 0;
            continue;
        }
        if (!d->in_frame) continue; /* noise before the first FEND */
        if (d->esc) {
            d->esc = 0;
            b = b == KISS_TFEND ? KISS_FEND : b == KISS_TFESC ? KISS_FESC : b;
        } else if (b == KISS_FESC) {
            d->esc = 1;
            continue;
        }
        if (d->n < KISS_MAX_FRAME) d->buf[d->n++] = b;
        else { d->overflows++; d->in_frame = 0; d->n = 0; } /* drop the oversize frame */
    }
}

/* ---- AX.25 ---- */

static void put_addr(uint8_t *out, const char *call, int ssid, int last, int cbit) {
    int n = 0;
    for (const char *p = call; *p && n < 6; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == '-' || c == '/') break;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[n++] = (uint8_t)(c << 1);
    }
    while (n < 6) out[n++] = (uint8_t)(' ' << 1);
    out[6] = (uint8_t)(0x60 | (cbit ? 0x80 : 0) | ((ssid & 0x0F) << 1) | (last ? 1 : 0));
}

size_t ax25_ui_wrap(const char *src_call, const uint8_t *info, size_t len, uint8_t *out, size_t cap) {
    if (!src_call || len + AX25_HDR_LEN > cap || len > 256) return 0;
    int ssid = 0;
    const char *dash = strchr(src_call, '-');
    if (dash && dash[1] >= '0' && dash[1] <= '9') {
        ssid = dash[1] - '0';
        if (dash[2] >= '0' && dash[2] <= '9') ssid = ssid * 10 + (dash[2] - '0');
        if (ssid > 15) ssid = 0;
    }
    put_addr(out, AX25_DEST, 0, 0, 1);
    put_addr(out + 7, src_call, ssid, 1, 0);
    out[14] = 0x03; /* UI */
    out[15] = 0xF0; /* no layer 3 */
    if (len) memcpy(out + 16, info, len);
    return len + AX25_HDR_LEN;
}

int ax25_ui_unwrap(const uint8_t *f, size_t len, const uint8_t **info, size_t *info_len, char *src) {
    if (len < AX25_HDR_LEN) return 0;
    for (int i = 0; i < 6; i++)
        if ((f[i] >> 1) != (uint8_t)AX25_DEST[i]) return 0;
    size_t pos = 13; /* index of the source address SSID byte (dest = 0..6, source = 7..13) */
    int guard = 0;
    while (!(f[pos] & 1)) { /* skip digipeaters until the end-of-address bit */
        pos += 7;
        if (++guard > 8 || pos >= len) return 0;
    }
    if (pos + 3 > len) return 0;
    if (f[pos + 1] != 0x03 || f[pos + 2] != 0xF0) return 0;
    if (src) {
        int k = 0;
        for (int i = 0; i < 6; i++) {
            char c = (char)(f[7 + i] >> 1);
            if (c != ' ' && c >= 0x20 && c < 0x7F) src[k++] = c;
        }
        int ssid = (f[13] >> 1) & 0x0F;
        if (ssid && k < 7) { src[k++] = '-'; if (ssid >= 10) src[k++] = '1'; src[k++] = (char)('0' + ssid % 10); }
        src[k] = 0;
    }
    *info = f + pos + 3;
    *info_len = len - (pos + 3);
    return 1;
}
