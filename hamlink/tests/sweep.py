# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame-success sweep of the modem through the crude FM channel model."""
import os, sys
import numpy as np
from hamlink import *

def trial(snr, twist=0, ppm=0, nframes=200, size=119, seed=1):
    rng = np.random.default_rng(seed)
    ok = 0; sent = 0
    for b in range(nframes // 8):
        frames = [bytes(rng.integers(0, 256, size, dtype=np.uint8)) for _ in range(8)]
        a = encode_burst(frames)
        pad = np.zeros(9600, dtype=np.int16)
        y = fm_channel(np.concatenate([pad, a, pad]), snr_db=snr, twist_db=twist, ppm=ppm, rng=rng)
        got = Demod().process(y)
        ok += sum(1 for f in frames if f in got); sent += len(frames)
    return ok / sent

if __name__ == "__main__":
    print("SNR(dB, 2.7kHz)  clean   twist6dB  ppm+300  twist6+ppm-250")
    for snr in [30, 24, 20, 17, 14, 12, 10, 8]:
        print(f"{snr:>6}        {trial(snr):6.1%}  {trial(snr,twist=6):7.1%}  {trial(snr,ppm=300):7.1%}  {trial(snr,twist=6,ppm=-250):9.1%}")
