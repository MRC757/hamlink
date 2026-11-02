#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Score the AFSK receiver in the Windows hamlink_core.dll on recordings (no compiler needed).

Counts frames with a valid HDLC CRC, as the receiver itself counts them (hl_get_rx_info), so ordinary AX.25/APRS packets
count too and the result is comparable with other packet decoders. Audio is resampled to 48 kHz and fed in 20 ms chunks,
as the app does. Each file is scored with bit repair off (pure CRC) and on (the app's default).

Main use: the WA8LMF TNC Test CD (tracks as WAV or FLAC). Results and what they mean: docs/DSP_IMPROVEMENTS.md, step 4.

    py -m pip install numpy scipy soundfile
    py tools/score_tnc_cd.py "D:/Audio-Tracks/02_100-Mic-E-Bursts-DE-emphasized.flac"
    py tools/score_tnc_cd.py --dll path/to/other/hamlink_core.dll --all-channels my_recording.wav

Windows only (it loads the Windows DLL). On Linux/macOS use tools/decode_wav.py, which builds the core on the host.
"""
import argparse
import ctypes as C
import os
import time

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DLL = os.path.join(ROOT, "windows", "native", "hamlink_core.dll")
API_VERSION = 10
CHUNK = 960  # 20 ms at 48 kHz

MSG_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.c_void_p, C.c_int)
ST_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_int)
EV_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)
_CALLBACKS = (MSG_CB(lambda *a: None), ST_CB(lambda *a: None), EV_CB(lambda *a: None))  # must outlive the contexts


def load(path):
    lib = C.CDLL(os.path.abspath(path))
    lib.hl_api_version.restype = C.c_int
    if lib.hl_api_version() != API_VERSION:
        raise SystemExit(f"{path}: API version {lib.hl_api_version()}, this script expects {API_VERSION}")
    lib.hl_create.restype = C.c_void_p
    lib.hl_create.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int, C.c_uint32, MSG_CB, ST_CB, EV_CB, C.c_void_p]
    lib.hl_destroy.argtypes = [C.c_void_p]
    lib.hl_set_bit_repair.argtypes = [C.c_void_p, C.c_int]
    lib.hl_process_audio.restype = C.c_float
    lib.hl_process_audio.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int64, C.c_float]
    lib.hl_get_rx_info.argtypes = [C.c_void_p, C.POINTER(C.c_uint32)]
    return lib


def channels_48k(path):
    """Yields (channel, int16 array at 48 kHz), resampling in blocks so long tracks fit in memory."""
    with sf.SoundFile(path) as f:
        fs, nch, n = f.samplerate, f.channels, f.frames
    g = np.gcd(48000, fs)
    up, down = 48000 // g, fs // g
    block, margin = down * 3000, down * 30
    for ch in range(nch):
        out = []
        with sf.SoundFile(path) as f:
            for start in range(0, n, block):
                lo, hi = max(0, start - margin), min(n, start + block + margin)
                f.seek(lo)
                seg = f.read(hi - lo, dtype="float32", always_2d=True)[:, ch]
                y = resample_poly(seg, up, down) if up != down else seg
                a = (start - lo) * up // down
                out.append(y[a:a + (min(n, start + block) - start) * up // down])
        yield ch, np.clip(np.round(np.concatenate(out) * 32767.0), -32768, 32767).astype(np.int16)


def score(lib, pcm, repair):
    """(frames with a valid CRC, of which recovered by bit repair, seconds taken)."""
    ctx = lib.hl_create(b"N0CALL", 0, 1, 500, 1, *_CALLBACKS, None)
    lib.hl_set_bit_repair(ctx, 1 if repair else 0)
    base = pcm.ctypes.data
    t0 = time.perf_counter()
    for i in range(0, len(pcm) - CHUNK + 1, CHUNK):
        lib.hl_process_audio(ctx, base + 2 * i, CHUNK, i // 48, 0.004)
    dt = time.perf_counter() - t0
    info = (C.c_uint32 * 6)()
    lib.hl_get_rx_info(ctx, info)
    lib.hl_destroy(ctx)
    return info[1], info[2], dt


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+", help="WAV or FLAC recordings, any sample rate")
    ap.add_argument("--dll", default=DEFAULT_DLL, help="hamlink_core.dll to test (default: the one the app ships)")
    ap.add_argument("--all-channels", action="store_true", help="score every channel (default: the first only)")
    a = ap.parse_args()
    lib = load(a.dll)
    for path in a.files:
        for ch, pcm in channels_48k(path):
            clean, _, dt = score(lib, pcm, repair=False)
            total, rep, _ = score(lib, pcm, repair=True)
            minutes = len(pcm) / 48000 / 60
            print(f"{os.path.basename(path)} ch{ch} ({minutes:.1f} min): {clean} frames with valid CRC, "
                  f"{total} with bit repair (+{rep} repaired); {minutes * 60 / dt:.0f}x real time", flush=True)
            if not a.all_channels:
                break


if __name__ == "__main__":
    main()
