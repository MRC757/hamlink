#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Score HamLink's AFSK demodulator on a recording: counts frames with a valid HDLC CRC.

Main use: the WA8LMF TNC Test CD (http://wa8lmf.net/TNCtest/), the usual benchmark for 1200 baud packet
demodulators. Its AX.25 frames use the same HDLC framing and CRC-16 as HamLink's audio link, so the count is
directly comparable with published results (Dire Wolf decodes about 1000 frames from Track 2).

    python3 tools/decode_wav.py 02_Track_2.wav           # needs numpy, scipy, and a host build (cd tests; pytest once)
    python3 tools/decode_wav.py my_recording.wav --list  # also print each frame's first bytes

Stereo files are scored per channel. Any sample rate is resampled to 48 kHz.
"""
import argparse
import os
import sys
from fractions import Fraction

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests"))
from hamlink import Demod  # noqa: E402  (builds/loads the host library)


def to_48k_int16(x, fs):
    x = x.astype(np.float64)
    if np.issubdtype(x.dtype, np.integer) or np.max(np.abs(x)) > 1.5:
        x = x / 32768.0
    if fs != 48000:
        r = Fraction(48000, fs).limit_denominator(1000)
        x = resample_poly(x, r.numerator, r.denominator)
    return np.clip(x * 32767.0, -32768, 32767).astype(np.int16)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("wav")
    ap.add_argument("--list", action="store_true", help="print each decoded frame")
    a = ap.parse_args()
    fs, data = wavfile.read(a.wav)
    if data.dtype == np.int16:
        data = data.astype(np.float64) / 32768.0
    chans = [data] if data.ndim == 1 else [data[:, c] for c in range(data.shape[1])]
    total = 0
    for ci, ch in enumerate(chans):
        pcm = to_48k_int16(ch, fs)
        d = Demod()
        frames = []
        step = 48000  # feed one second at a time, as the apps do in 20 ms chunks
        for i in range(0, len(pcm), step):
            frames += d.process(pcm[i:i + step])
        total += len(frames)
        print(f"channel {ci}: {len(frames)} frames with valid CRC in {len(pcm) / 48000:.0f} s of audio")
        if a.list:
            for f in frames:
                print("   ", f[:24].hex())
    if len(chans) > 1:
        print(f"total: {total}")


if __name__ == "__main__":
    main()
