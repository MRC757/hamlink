#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay a recording through HamLink's receivers (the shipped Windows hamlink_core.dll) and show what decodes.

Use it on WAV files the Windows app saves with Settings > "Record received radio audio" (in
%LOCALAPPDATA%\\HamLink\\recordings), or on any WAV/FLAC recording, to see which modem decodes what and how cleanly,
without a radio. Each modem gets its own receiver; messages are shown when they are addressed to --call or to everyone.

    py -m pip install numpy scipy soundfile
    py tools/replay_wav.py rx-20261101-153000.wav
    py tools/replay_wav.py --modem psk2400 --call KJ4QEO recording.flac

Windows only (it loads the Windows DLL).
"""
import argparse
import ctypes as C
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from score_tnc_cd import DEFAULT_DLL, channels_48k  # noqa: E402

API_VERSION = 10
MODEMS = {"afsk": 0, "ofdm": 1, "mt63": 2, "psk2400": 3, "psk3600": 4}
TYPES = {1: "text", 2: "voice", 5: "report"}
CHUNK = 960  # 20 ms at 48 kHz, as the app feeds it

MSG = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.c_void_p, C.c_int)
ST = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_int)
EV = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)


def load(path):
    lib = C.CDLL(os.path.abspath(path))
    lib.hl_api_version.restype = C.c_int
    if lib.hl_api_version() != API_VERSION:
        raise SystemExit(f"{path}: API version {lib.hl_api_version()}, this script expects {API_VERSION}")
    lib.hl_create.restype = C.c_void_p
    lib.hl_create.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int, C.c_uint32, MSG, ST, EV, C.c_void_p]
    lib.hl_destroy.argtypes = [C.c_void_p]
    lib.hl_set_modem.argtypes = [C.c_void_p, C.c_int]
    lib.hl_process_audio.restype = C.c_float
    lib.hl_process_audio.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int64, C.c_float]
    lib.hl_get_stats.argtypes = [C.c_void_p, C.POINTER(C.c_uint32)]
    lib.hl_get_fec_info.argtypes = [C.c_void_p, C.POINTER(C.c_uint32)]
    lib.hl_report_summary.argtypes = [C.c_char_p, C.c_int, C.c_char_p, C.c_int]
    return lib


def replay(lib, pcm, modem, call):
    got = []

    def on_msg(user, src, dst, typ, data, n):
        payload = C.string_at(data, n)
        if typ == 1:
            text = payload.decode("utf-8", "replace")
        elif typ == 5:
            out = C.create_string_buffer(256)
            text = out.value.decode() if lib.hl_report_summary(payload, n, out, 256) > 0 else f"{n} bytes"
        else:
            text = f"{n} bytes"
        got.append((src.decode(), dst.decode(), TYPES.get(typ, f"type {typ}"), text))

    cbs = (MSG(on_msg), ST(lambda *a: None), EV(lambda *a: None))
    ctx = lib.hl_create(call.encode(), 0, 1, 500, 1, *cbs, None)
    if MODEMS[modem] and lib.hl_set_modem(ctx, MODEMS[modem]) != 0:
        raise SystemExit(f"{modem}: could not start that modem")
    # 5 s of silence after the recording: receivers (MT63 most of all) deliver a frame a while after its audio
    pcm = np.concatenate([pcm, np.zeros(48000 * 5, np.int16)])
    base = pcm.ctypes.data
    for i in range(0, len(pcm), CHUNK):
        lib.hl_process_audio(ctx, base + 2 * i, min(CHUNK, len(pcm) - i), 1000 + i // 48, 0.004)
    stats, fec = (C.c_uint32 * 8)(), (C.c_uint32 * 3)()
    lib.hl_get_stats(ctx, stats)
    lib.hl_get_fec_info(ctx, fec)
    lib.hl_destroy(ctx)
    return got, stats[0], fec[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file", help="WAV or FLAC recording, any sample rate (first channel is used)")
    ap.add_argument("--modem", choices=["all"] + list(MODEMS), default="all")
    ap.add_argument("--call", default="N0CALL", help="receiving callsign: messages addressed to it are shown too. "
                    "Not the sender's callsign: a station ignores frames that claim to be its own")
    ap.add_argument("--dll", default=DEFAULT_DLL, help="hamlink_core.dll to use (default: the one the app ships)")
    a = ap.parse_args()
    lib = load(a.dll)
    _, pcm = next(channels_48k(a.file))
    print(f"{os.path.basename(a.file)}: {len(pcm) / 48000:.1f} s of audio")
    for m in (MODEMS if a.modem == "all" else [a.modem]):
        got, frames, fec_only = replay(lib, pcm, m, a.call)
        extra = f", {fec_only} only thanks to FX.25" if m == "afsk" and fec_only else ""
        print(f"  {m:8s} {frames} HamLink frames heard{extra}, {len(got)} messages/reports delivered")
        for src, dst, typ, text in got:
            print(f"      {src} -> {dst} [{typ}] {text[:100]}")


if __name__ == "__main__":
    main()
