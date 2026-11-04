#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PSK delivery sweep: messages decoded against signal-to-noise ratio, for PSK 2400 and PSK 3600 (AFSK for comparison).

Each modem sends N short CQ messages through a simulated handheld audio path, and a receiver on the same modem counts
what it decodes. Three channels: flat; tilt with a sound-card clock offset (de-emphasis, two cheap clocks); and a
resonant one like acoustic coupling (a peak at 1700 Hz, 1200 Hz about 14 dB down). The automated regression tests
are tests/test_psk.py; this tool shows the whole curve, for checking a receiver change or comparing builds.

    python tools/psk_sweep.py                       # library from the build tree
    python tools/psk_sweep.py --lib other.dll --messages 20 --channel flat
    python tools/psk_sweep.py --compare old.dll     # a second library in the same table

Needs numpy only. The library is libhamlink_api.so (Linux, Pi) or windows/native/hamlink_core.dll.
"""
import argparse
import ctypes as C
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_modems import FS, MODEMS, Station, find_lib, load  # noqa: E402

SNRS = {"afsk": (2, 4, 6, 8, 10), "psk2400": (2, 4, 6, 8, 10), "psk3600": (6, 8, 10, 12, 14)}


def band_shape(n, tilt_db=0.0, resonance=False):
    """Frequency response for an n-sample signal: 300-3000 Hz pass band, optional tilt and resonance."""
    f = np.fft.rfftfreq(n, 1 / FS)
    g = np.where((f >= 300) & (f <= 3000), 1.0, 0.0)
    if tilt_db:
        g *= 10 ** (-tilt_db * np.clip((f - 1200) / 1000, 0, 1.8) / 20)  # loses tilt_db per kHz above 1200 Hz
    if resonance:
        q, f0 = 1.5, 1700.0
        g *= 1 / np.sqrt(1 + (q * (f / f0 - f0 / np.maximum(f, 1))) ** 2)
    return g


def channel(x, snr_db, rng, tilt_db=0.0, ppm=0.0, resonance=False):
    y = np.fft.irfft(np.fft.rfft(x.astype(float)) * band_shape(len(x), tilt_db, resonance), len(x))
    if ppm:
        t = np.arange(int(len(y) / (1 + ppm * 1e-6))) * (1 + ppm * 1e-6)
        y = np.interp(t, np.arange(len(y)), y)
    p = np.mean(y[np.abs(y) > 1e-3] ** 2)
    # noise in a 2700 Hz band at the requested SNR, as tests/hamlink.py fm_channel does
    y = y + rng.normal(0, np.sqrt(p / 10 ** (snr_db / 10) * (FS / 2) / 2700), len(y))
    return y


def bursts(L, modem, count):
    tx = Station(L, b"W1SWP", modem, 5)
    out, now = [], 1000
    for i in range(count):
        text = f"sweep {i:03d} the quick brown fox jumps over the lazy dog".encode()
        L.hl_send(tx.c, b"CQ", 1, text, len(text), 0, now)
        for _ in range(400):
            now, _ = tx.feed(np.zeros(2400, np.int16), now)
            n = C.c_int()
            p = L.hl_poll_tx(tx.c, now, 0.5, C.byref(n))
            if p:
                out.append(np.ctypeslib.as_array(C.cast(p, C.POINTER(C.c_int16)), (n.value,)).astype(float))
                break
            now += 50
    tx.close()
    return out


def decoded(L, modem, bl, snr, rng, **ch):
    parts = []
    for b in bl:
        parts += [np.zeros(FS), channel(b, snr, rng, **ch)]
    parts.append(np.zeros(6 * FS))
    sig = np.concatenate(parts)
    sig += rng.normal(0, 3, len(sig))
    sig = np.clip(sig / max(1.0, np.abs(sig).max()) * 0.3 * 32767, -32767, 32767).astype(np.int16)
    rx = Station(L, b"W2SWP", modem, 6)
    rx.feed(sig, 1000)
    got = sum(1 for t in rx.got if t == 1)
    rx.close()
    return got


CHANNELS = {
    "flat": {},
    "tilt": dict(tilt_db=6, ppm=150),
    "resonant": dict(resonance=True),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", help="libhamlink_api.so or hamlink_core.dll (default: found in the build tree)")
    ap.add_argument("--compare", help="a second library, shown in its own column")
    ap.add_argument("--messages", type=int, default=12, help="messages per point (default 12)")
    ap.add_argument("--channel", choices=["all"] + list(CHANNELS), default="all")
    ap.add_argument("--modem", choices=["all", "afsk", "psk2400", "psk3600"], default="all")
    a = ap.parse_args()
    libs = [(os.path.basename(a.lib or find_lib()), load(a.lib or find_lib()))]
    if a.compare:
        libs.append((os.path.basename(a.compare) + " (compare)", load(a.compare)))
    print(f"PSK sweep: {a.messages} messages per point, decoded / sent; " + ", ".join(n for n, _ in libs))
    for modem in (["afsk", "psk2400", "psk3600"] if a.modem == "all" else [a.modem]):
        bl = bursts(libs[0][1], MODEMS[modem], a.messages)
        for cname in (CHANNELS if a.channel == "all" else [a.channel]):
            for snr in SNRS[modem]:
                rng_seed = 1000 + snr
                cells = [f"{decoded(L, MODEMS[modem], bl, snr, np.random.default_rng(rng_seed), **CHANNELS[cname]):3d}/{len(bl)}"
                         for _, L in libs]
                print(f"{modem:8s} {cname:9s} SNR {snr:3d} dB   " + "   ".join(cells), flush=True)


if __name__ == "__main__":
    main()
