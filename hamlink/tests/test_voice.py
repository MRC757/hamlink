# SPDX-License-Identifier: GPL-3.0-or-later
"""Codec2 700C clip pipeline: PCM -> clip bytes -> mesh over the audio channel -> PCM."""
import ctypes as C
import os
import numpy as np
from hamlink import *

L.voice_create.restype = C.c_void_p
L.voice_encode.restype = C.c_size_t
L.voice_encode.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]
L.voice_decode.restype = C.c_size_t
L.voice_decode.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p, C.c_size_t]


def speechlike(seconds, fs=8000):
    t = np.arange(int(seconds * fs)) / fs
    f0 = 110 + 25 * np.sin(2 * np.pi * 1.7 * t)
    ph = 2 * np.pi * np.cumsum(f0) / fs
    env = 0.5 * (1 + np.sin(2 * np.pi * 3.1 * t))
    s = sum(np.sin(h * ph) / h for h in range(1, 25)) * env
    return (3500 * s).astype(np.int16)


def encode(pcm):
    v = L.voice_create()
    out = (C.c_ubyte * 4000)()
    pcm = np.ascontiguousarray(pcm)
    n = L.voice_encode(v, pcm.ctypes.data, len(pcm), out, 4000)
    return bytes(out[:n])


def decode(clip):
    v = L.voice_create()
    out = np.zeros(len(clip) * 80, dtype=np.int16)
    n = L.voice_decode(v, clip, len(clip), out.ctypes.data, len(out))
    return out[:n]


def test_clip_size_is_100_bytes_per_second():
    clip = encode(speechlike(5.0))
    assert clip[0] == 1 and len(clip) == 1 + 125 * 4  # 5 s -> 125 frames
    pcm = decode(clip)
    assert len(pcm) == 125 * 320
    assert 500 < np.sqrt(np.mean(pcm.astype(float) ** 2)) < 8000  # audible, not silence/blown up


def test_max_clip_fits_one_message():
    clip = encode(speechlike(40.0))  # longer than allowed -> truncated
    assert len(clip) <= 32 * 96
    assert len(clip) == 1 + 767 * 4  # ~30.7 s


def test_voice_clip_over_relay_roundtrip():
    clip = encode(speechlike(6.0))
    d = dict(snr=22, twist=4, ppm=100)
    links = {("W1AAA", "W9RLY"): d, ("W9RLY", "W1AAA"): d, ("W2BBB", "W9RLY"): d, ("W9RLY", "W2BBB"): d}
    a, r, b = Node("W1AAA", seed=5, ttl=1), Node("W9RLY", relay=True, seed=6, ttl=1), Node("W2BBB", seed=7, ttl=1)
    sim = Sim([a, r, b], links, duration_s=90)
    ids = {}
    sim.run(70, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", clip, typ=MT_VOICE, now=s.t)))])
    assert a.status[ids["id"]] == ST_DELIVERED
    src, dst, typ, data = b.messages[0]
    assert typ == MT_VOICE and data == clip
    # Codec2's decoder is not bit-exact between instances (randomised phase on unvoiced
    # frames), so compare byte-exact clips above and just sanity-check decoded energy here.
    rms = lambda x: np.sqrt(np.mean(x.astype(float) ** 2))
    assert abs(rms(decode(data)) / rms(decode(clip)) - 1) < 0.05
