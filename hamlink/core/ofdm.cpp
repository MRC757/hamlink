/* SPDX-License-Identifier: GPL-3.0-or-later */
// C wrapper around the vendored Rattlegram COFDMTV encoder/decoder (third_party/aicodix, 0BSD-style).
#define assert(expr) do {} while (0)
// The vendored headers rely on these being pulled in transitively by older toolchains.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <functional>
#include <limits>
#include <new>
#include "encoder.hh"
#include "decoder.hh"
#include "ofdm.h"

namespace {
const int kRate = 48000;
const int kSymbolsAfterNoise = 1 /*sync*/ + 1 /*preamble*/ + 4 /*payload*/ + 1 /*tail*/;
}

struct ofdm_tx {
    Encoder<kRate> enc;
};

struct ofdm_rx {
    Decoder<kRate> dec;
    struct Pkt {
        uint8_t data[OFDM_MAX_PAYLOAD];
        int len;
        char call[10];
        int flips;
    } q[OFDM_RX_QUEUE];
    int head = 0, tail = 0;
    uint32_t ok = 0, failed = 0;
};

extern "C" {

ofdm_tx_t *ofdm_tx_create(void) { return new (std::nothrow) ofdm_tx; }
void ofdm_tx_destroy(ofdm_tx_t *t) { delete t; }

size_t ofdm_packet_samples(int noise_symbols) {
    if (noise_symbols < 0) noise_symbols = 0;
    return (size_t)(noise_symbols + kSymbolsAfterNoise) * OFDM_SYMBOL_SAMPLES;
}

size_t ofdm_tx_modulate(ofdm_tx_t *t, const uint8_t *payload, size_t len, const char *callsign,
                        int noise_symbols, int16_t *out, size_t cap) {
    if (!t || !payload || !out || len == 0 || len > OFDM_MAX_PAYLOAD) return 0;
    if (noise_symbols < 0) noise_symbols = 0;
    if (noise_symbols > 12) noise_symbols = 12;
    size_t need = ofdm_packet_samples(noise_symbols);
    if (cap < need) return 0;

    uint8_t padded[OFDM_MAX_PAYLOAD + 8];
    std::memset(padded, 0, sizeof padded);
    std::memcpy(padded, payload, len);

    // Rattlegram call signs are base-37 (A-Z, 0-9, space): keep only those, upper-cased.
    int8_t call[10] = {0};
    int n = 0;
    for (const char *p = callsign ? callsign : ""; *p && n < 9; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) call[n++] = (int8_t)c;
    }
    if (n == 0) { call[0] = 'N'; call[1] = 'O'; call[2] = 'C'; call[3] = 'A'; call[4] = 'L'; call[5] = 'L'; }

    t->enc.configure(padded, (int)len, call, OFDM_CARRIER_HZ, noise_symbols, false);
    size_t written = 0;
    while (written + OFDM_SYMBOL_SAMPLES <= cap) {
        if (!t->enc.produce(out + written, 0)) break; // false = finished (that call wrote only silence)
        written += OFDM_SYMBOL_SAMPLES;
    }
    return written;
}

static const size_t kMaxFrame = OFDM_MAX_PAYLOAD - 2; // magic byte + length byte

int ofdm_count_packets(const size_t *lens, int n) {
    int packets = 0;
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        if (lens[i] == 0 || lens[i] > kMaxFrame) continue;
        if (used == 0 || used + 1 + lens[i] > OFDM_MAX_PAYLOAD) {
            packets++;
            used = 1; // magic
        }
        used += 1 + lens[i];
    }
    return packets;
}

size_t ofdm_burst_max_samples(const size_t *lens, int n, int noise_symbols) {
    int p = ofdm_count_packets(lens, n);
    if (p == 0) return 0;
    return ofdm_packet_samples(noise_symbols) + (size_t)(p - 1) * ofdm_packet_samples(0);
}

size_t ofdm_encode_burst(ofdm_tx_t *t, const uint8_t *const *frames, const size_t *lens, int n,
                         const char *callsign, int noise_symbols, int16_t *out, size_t cap) {
    if (cap < ofdm_burst_max_samples(lens, n, noise_symbols)) return 0;
    int total_packets = ofdm_count_packets(lens, n);
    size_t written = 0;
    int packet = 0;
    int i = 0;
    while (i < n) {
        uint8_t buf[OFDM_MAX_PAYLOAD];
        size_t used = 0;
        while (i < n) {
            if (lens[i] == 0 || lens[i] > kMaxFrame) { i++; continue; }
            if (used == 0) buf[used++] = OFDM_BUNDLE_MAGIC;
            if (used + 1 + lens[i] > OFDM_MAX_PAYLOAD) break;
            buf[used++] = (uint8_t)lens[i];
            std::memcpy(buf + used, frames[i], lens[i]);
            used += lens[i];
            i++;
        }
        if (used == 0) break;
        int noise = packet == 0 ? noise_symbols : 0;
        size_t k = ofdm_tx_modulate(t, buf, used, callsign, noise, out + written, cap - written);
        if (k == 0) return 0;
        packet++;
        if (packet < total_packets && k > (size_t)(OFDM_SYMBOL_SAMPLES - OFDM_TAIL_KEEP_SAMPLES))
            k -= (size_t)(OFDM_SYMBOL_SAMPLES - OFDM_TAIL_KEEP_SAMPLES);
        written += k;
    }
    return written;
}

int ofdm_unpack(const uint8_t *payload, int len, const uint8_t **frames, size_t *lens, int max) {
    if (len < 3 || payload[0] != OFDM_BUNDLE_MAGIC) return 0;
    int pos = 1, count = 0;
    while (pos < len && count < max) {
        int l = payload[pos];
        if (l == 0) break; // zero padding
        if (pos + 1 + l > len) break; // truncated record: stop, keep what we have
        frames[count] = payload + pos + 1;
        lens[count] = (size_t)l;
        count++;
        pos += 1 + l;
    }
    return count;
}

ofdm_rx_t *ofdm_rx_create(void) { return new (std::nothrow) ofdm_rx; }
void ofdm_rx_destroy(ofdm_rx_t *r) { delete r; }

static void push(ofdm_rx_t *r, int mode) {
    uint8_t buf[OFDM_MAX_PAYLOAD + 8];
    int flips = r->dec.fetch(buf);
    if (flips < 0) { r->failed++; return; }
    float cfo; int32_t m; uint8_t call[10] = {0};
    r->dec.staged(&cfo, &m, call);
    int len = mode == 14 ? 170 : mode == 15 ? 128 : mode == 16 ? 85 : 0;
    if (len == 0) return;
    int next = (r->head + 1) % OFDM_RX_QUEUE;
    if (next == r->tail) return; // queue full: drop
    ofdm_rx::Pkt &p = r->q[r->head];
    std::memcpy(p.data, buf, OFDM_MAX_PAYLOAD);
    p.len = len;
    p.flips = flips;
    int i = 0, first = 0;
    while (first < 9 && call[first] == ' ') first++; // the 9-char field is left-padded with spaces
    for (int k = first; k < 9 && call[k]; k++) p.call[i++] = (char)((call[k] >= 0x20 && call[k] < 0x7F) ? call[k] : '?');
    p.call[i] = 0;
    r->head = next;
    r->ok++;
}

void ofdm_rx_process(ofdm_rx_t *r, const int16_t *pcm, size_t n) {
    // The decoder wants at most one symbol period per feed(); process() once per completed period.
    while (n > 0) {
        size_t k = n < (size_t)OFDM_SYMBOL_SAMPLES ? n : (size_t)OFDM_SYMBOL_SAMPLES;
        if (r->dec.feed(pcm, (int)k, 0)) {
            int st = r->dec.process();
            if (st == STATUS_DONE) {
                float cfo; int32_t mode; uint8_t call[10];
                r->dec.staged(&cfo, &mode, call);
                push(r, mode);
            }
        }
        pcm += k;
        n -= k;
    }
}

int ofdm_rx_next(ofdm_rx_t *r, uint8_t payload[OFDM_MAX_PAYLOAD], char callsign[10], int *flips) {
    if (r->tail == r->head) return 0;
    ofdm_rx::Pkt &p = r->q[r->tail];
    std::memcpy(payload, p.data, OFDM_MAX_PAYLOAD);
    std::memcpy(callsign, p.call, 10);
    if (flips) *flips = p.flips;
    int len = p.len;
    r->tail = (r->tail + 1) % OFDM_RX_QUEUE;
    return len;
}

uint32_t ofdm_rx_packets_ok(const ofdm_rx_t *r) { return r->ok; }
uint32_t ofdm_rx_packets_failed(const ofdm_rx_t *r) { return r->failed; }

} // extern "C"
