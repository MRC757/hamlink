// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES // M_PI with MSVC
#endif
#include "mt63.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <deque>
#include <vector>
#include "dsp.h"
#include "mt63base.h"
extern "C" {
#include "hdlc.h"
}

namespace {

const double CENTER_HZ = 1500.0; // MT63-2000 occupies 500..2500 Hz
const int BANDWIDTH = 2000;
const int LONG_INTERLEAVE = 1;   // MT63-2000L: 64-symbol interleave, the more robust variant
// Symbols the synchroniser integrates over. fldigi's default, 16. Its optional 32 failed to decode even a clean
// 2000L signal from its own transmitter in our tests (any lead-in, any block size), so it is not used.
const int RX_INTEGRATION = 16;
const int LOCK_MS = 1600;        // idle characters after the PTT lead-in so the receiver locks before the first frame
const char SYNC1 = 0x16, SYNC2 = 0x69;
const int RATIO = 6;             // 48 kHz <-> 8 kHz
const int NTAPS = 133;           // low-pass at 3.4 kHz for the rate change (MT63-2000 tops out at 2.5 kHz)

struct Fir {
    float h[NTAPS];
    Fir() {
        double sum = 0;
        for (int i = 0; i < NTAPS; i++) {
            double n = i - (NTAPS - 1) / 2.0, fc = 3400.0 / 48000.0;
            double v = n == 0 ? 2 * fc : std::sin(2 * M_PI * fc * n) / (M_PI * n);
            v *= 0.54 - 0.46 * std::cos(2 * M_PI * i / (NTAPS - 1));
            h[i] = (float)v;
            sum += v;
        }
        for (int i = 0; i < NTAPS; i++) h[i] = (float)(h[i] / sum);
    }
};
const Fir &fir() { static const Fir f; return f; }

// Characters for one frame: sync, then [len][frame][crc] packed 7 bits per character, LSB first.
void frame_chars(const uint8_t *f, size_t len, std::vector<char> &out) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)len);
    b.insert(b.end(), f, f + len);
    uint16_t crc = hdlc_crc16(b.data(), b.size());
    b.push_back((uint8_t)(crc & 0xFF));
    b.push_back((uint8_t)(crc >> 8));
    out.push_back(SYNC1);
    out.push_back(SYNC2);
    unsigned acc = 0;
    int nbits = 0;
    for (uint8_t v : b) {
        acc |= (unsigned)v << nbits;
        nbits += 8;
        while (nbits >= 7) { out.push_back((char)(acc & 0x7F)); acc >>= 7; nbits -= 7; }
    }
    if (nbits) out.push_back((char)(acc & 0x7F));
}

size_t chars_for(size_t len) { return 2 + ((len + 3) * 8 + 6) / 7; }

int lead_chars(int preamble_ms) { return (preamble_ms + LOCK_MS) * MT63_CHARS_PER_S / 1000 + 1; }

} // namespace

// The fldigi classes leave many scalar members uninitialised in their constructors (fldigi itself only ever uses
// long-lived objects that are Preset again before each use). Build them in zeroed memory so every member starts at 0.
template <class T> T *zeroed_new() {
    void *mem = std::calloc(1, sizeof(T));
    return mem ? new (mem) T : nullptr;
}
template <class T> void zeroed_delete(T *p) {
    if (!p) return;
    p->~T();
    std::free(p);
}

// ------------------------------------------------------------------ transmitter

struct mt63_tx {
    MT63tx tx;
};

extern "C" mt63_tx_t *mt63_tx_create(void) {
    mt63_tx_t *t = zeroed_new<mt63_tx>();
    if (t && t->tx.Preset(CENTER_HZ, BANDWIDTH, LONG_INTERLEAVE)) { zeroed_delete(t); return nullptr; }
    return t;
}
extern "C" void mt63_tx_destroy(mt63_tx_t *t) { zeroed_delete(t); }
extern "C" size_t mt63_frame_chars(size_t len) { return chars_for(len); }

extern "C" uint32_t mt63_burst_overhead_ms(int preamble_ms) {
    // lead-in characters, 63 flush characters and the jam symbol, one symbol (50 ms) each
    return (uint32_t)((lead_chars(preamble_ms) + 64 + 1) * 1000 / MT63_CHARS_PER_S);
}

extern "C" size_t mt63_burst_max_samples(const size_t *lens, int n, int preamble_ms) {
    size_t chars = (size_t)lead_chars(preamble_ms) + 64 + 2;
    for (int i = 0; i < n; i++) chars += chars_for(lens[i]);
    return chars * (8000 / MT63_CHARS_PER_S) * RATIO * 2 + NTAPS * 2; // 2x margin on the symbol length
}

extern "C" size_t mt63_encode_burst(mt63_tx_t *t, const uint8_t *const *frames, const size_t *lens, int n,
                                    int preamble_ms, float amp, int16_t *out, size_t cap) {
    if (!t || n <= 0) return 0;
    if (t->tx.Preset(CENTER_HZ, BANDWIDTH, LONG_INTERLEAVE)) return 0;
    std::vector<char> chars((size_t)lead_chars(preamble_ms), 0);
    for (int i = 0; i < n; i++)
        if (lens[i] > 0 && lens[i] <= MT63_MAX_FRAME) frame_chars(frames[i], lens[i], chars);

    std::vector<double> s8;
    auto emit = [&]() { s8.insert(s8.end(), t->tx.Comb.Output.Data, t->tx.Comb.Output.Data + t->tx.Comb.Output.Len); };
    for (int i = 0; i < t->tx.DataInterleave; i++) t->tx.SendChar(0); // prime the interleaver, not sent (as fldigi does)
    for (char c : chars) { t->tx.SendChar(c); emit(); }
    for (int i = 1; i < t->tx.DataInterleave; i++) { t->tx.SendChar(0); emit(); } // push the last characters out
    t->tx.SendJam();
    emit();

    double peak = 1e-9;
    for (double v : s8) peak = std::fmax(peak, std::fabs(v));
    double scale = (double)amp * 32767.0 / peak * RATIO; // RATIO: interpolation gain
    size_t need = s8.size() * RATIO;
    if (need > cap) return 0;
    // interpolate 8 -> 48 kHz: out[m] = sum_k h[k] * x_up[m - k], with x_up non-zero every RATIO samples
    const float *h = fir().h;
    for (size_t m = 0; m < need; m++) {
        double acc = 0;
        int first = (int)(m % RATIO);
        for (int k = first; k < NTAPS; k += RATIO) {
            long j = ((long)m - k) / RATIO;
            if (j < 0) break;
            acc += h[k] * s8[(size_t)j];
        }
        double v = std::round(acc * scale);
        out[m] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return need;
}

// ------------------------------------------------------------------ receiver

struct mt63_rx {
    MT63rx rx;
    double_buff in;
    float hist[NTAPS] = {0};
    int hpos = 0, phase = 0;
    std::vector<char> chars;
    std::deque<std::vector<uint8_t>> frames;
    uint32_t frames_ok = 0;
};

extern "C" mt63_rx_t *mt63_rx_create(void) {
    mt63_rx_t *r = zeroed_new<mt63_rx>();
    if (!r) return nullptr;
    // twice, as fldigi effectively does: the second Preset sees every member already set by the first
    for (int i = 0; i < 2; i++)
        if (r->rx.Preset(CENTER_HZ, BANDWIDTH, LONG_INTERLEAVE, RX_INTEGRATION)) { zeroed_delete(r); return nullptr; }
    return r;
}
extern "C" void mt63_rx_destroy(mt63_rx_t *r) { zeroed_delete(r); }
extern "C" int mt63_rx_locked(const mt63_rx_t *r) { return r ? const_cast<MT63rx &>(r->rx).SYNC_LockStatus() : 0; }
extern "C" uint32_t mt63_rx_frames_ok(const mt63_rx_t *r) { return r ? r->frames_ok : 0; }

// Find frames in the character stream; a candidate that fails its CRC is skipped one character at a time.
static void scan(mt63_rx_t *r) {
    std::vector<char> &c = r->chars;
    for (;;) {
        size_t i = 0;
        while (i + 1 < c.size() && !(c[i] == SYNC1 && c[i + 1] == SYNC2)) i++;
        if (i + 1 >= c.size()) { // no sync yet: keep only a possible first sync character
            bool keep = !c.empty() && c.back() == SYNC1;
            c.erase(c.begin(), c.end() - (keep ? 1 : 0));
            return;
        }
        c.erase(c.begin(), c.begin() + (long)i);
        if (c.size() < 4) return;
        size_t len = (size_t)((c[2] & 0x7F) | ((c[3] & 0x01) << 7));
        size_t total = chars_for(len);
        if (c.size() < total) return;
        std::vector<uint8_t> b;
        unsigned acc = 0;
        int nbits = 0;
        for (size_t k = 2; k < total && b.size() < len + 3; k++) {
            acc |= (unsigned)(c[k] & 0x7F) << nbits;
            nbits += 7;
            if (nbits >= 8) { b.push_back((uint8_t)(acc & 0xFF)); acc >>= 8; nbits -= 8; }
        }
        bool ok = len >= 1 && b.size() == len + 3 &&
                  hdlc_crc16(b.data(), len + 1) == (uint16_t)(b[len + 1] | (b[len + 2] << 8));
        if (ok) {
            r->frames.emplace_back(b.begin() + 1, b.begin() + 1 + (long)len);
            r->frames_ok++;
            c.erase(c.begin(), c.begin() + (long)total);
        } else {
            c.erase(c.begin());
        }
    }
}

extern "C" void mt63_rx_process(mt63_rx_t *r, const int16_t *pcm, size_t n) {
    if (!r || n == 0) return;
    if (r->in.EnsureSpace((int)(n / RATIO + 2))) return;
    int m = 0;
    const float *h = fir().h;
    for (size_t i = 0; i < n; i++) { // decimate 48 -> 8 kHz
        r->hist[r->hpos] = (float)pcm[i] * (1.0f / 32768.0f);
        r->hpos = (r->hpos + 1) % NTAPS;
        if (++r->phase < RATIO) continue;
        r->phase = 0;
        double acc = 0;
        for (int k = 0, p = r->hpos; k < NTAPS; k++, p = (p + 1) % NTAPS) acc += h[k] * r->hist[p];
        r->in.Data[m++] = acc;
    }
    r->in.Len = m;
    if (m == 0) return;
    r->rx.Process(&r->in);
    for (int i = 0; i < r->rx.Output.Len; i++) r->chars.push_back((char)(r->rx.Output.Data[i] & 0x7F));
    if (r->chars.size() > 4096) r->chars.erase(r->chars.begin(), r->chars.end() - 1024);
    scan(r);
}

extern "C" int mt63_rx_next(mt63_rx_t *r, uint8_t *frame, size_t cap) {
    if (!r || r->frames.empty()) return 0;
    std::vector<uint8_t> f = std::move(r->frames.front());
    r->frames.pop_front();
    size_t len = f.size() < cap ? f.size() : cap;
    memcpy(frame, f.data(), len);
    return (int)len;
}
