# SPDX-License-Identifier: GPL-3.0-or-later
"""Drives ONLY the flat hl_* C API (what the Windows P/Invoke layer calls) through the FM channel model."""
import ctypes as C
import os
import numpy as np
from hamlink import BUILD, FS, fm_channel

import subprocess
subprocess.check_call(["cmake", "--build", BUILD, "--target", "hamlink_api", "-j8"], stdout=subprocess.DEVNULL)
A = C.CDLL(os.path.join(BUILD, "core", "libhamlink_api.so"))

MSG = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.POINTER(C.c_ubyte), C.c_int)
STA = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_int)
EVT = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)
A.hl_create.restype = C.c_void_p
A.hl_create.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int, C.c_uint32, MSG, STA, EVT, C.c_void_p]
A.hl_destroy.argtypes = [C.c_void_p]
A.hl_process_audio.restype = C.c_float
A.hl_process_audio.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int64, C.c_float]
A.hl_send.argtypes = [C.c_void_p, C.c_char_p, C.c_int, C.c_char_p, C.c_int, C.c_int, C.c_int64]
A.hl_poll_tx.restype = C.POINTER(C.c_int16)
A.hl_poll_tx.argtypes = [C.c_void_p, C.c_int64, C.c_float, C.POINTER(C.c_int)]
A.hl_voice_encode.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_void_p, C.c_int]
A.hl_voice_decode.argtypes = [C.c_void_p, C.c_char_p, C.c_int, C.c_void_p, C.c_int]
A.hl_set_modem.argtypes = [C.c_void_p, C.c_int]
A.hl_set_link.argtypes = [C.c_void_p, C.c_int, C.c_int]
A.hl_tnc_rx.argtypes = [C.c_void_p, C.c_char_p, C.c_int, C.c_int64]
A.hl_tnc_poll_tx.restype = C.POINTER(C.c_ubyte)
A.hl_tnc_poll_tx.argtypes = [C.c_void_p, C.c_int64, C.POINTER(C.c_int)]
A.hl_get_stats.argtypes = [C.c_void_p, C.c_void_p]
A.hl_pending_tx.argtypes = [C.c_void_p]
A.hl_set_manual_ptt.argtypes = [C.c_void_p, C.c_int]


class N:
    def __init__(self, name, relay=0, seed=1, modem=0, ttl=1):
        self.name, self.msgs, self.status, self.events, self.tx_until = name, [], {}, [], 0.0
        self.cb = (MSG(lambda u, s, d, t, p, n: self.msgs.append((s.decode(), d.decode(), t, bytes(p[:n])))),
                   STA(lambda u, i, st: self.status.__setitem__(i, st)),
                   EVT(lambda u, t: self.events.append(t.decode())))
        self.c = A.hl_create(name.encode(), relay, ttl, 500, seed, *self.cb, None)
        assert self.c
        if modem:
            assert A.hl_set_modem(self.c, modem) == 0
        self.modem = modem

    def stats(self):
        v = (C.c_uint32 * 8)()
        A.hl_get_stats(self.c, v)
        return dict(zip(("rx", "dups", "tx", "relayed", "suppressed", "retries", "ofdm_ok", "ofdm_fail"), list(v)))

    def __del__(self):
        A.hl_destroy(self.c)


def run(nodes, links, seconds, script, airtime=None, seed=3):
    rng = np.random.default_rng(seed)
    rx = {n.name: np.zeros(int((seconds + 30) * FS), dtype=np.int32) for n in nodes}
    dt, spl, t, si = 0.02, int(0.02 * FS), 0.0, 0
    script = sorted(script, key=lambda e: e[0])
    while t < seconds:
        while si < len(script) and script[si][0] <= t:
            script[si][1](t); si += 1
        i0, now = int(round(t * FS)), int((t + dt) * 1000)
        for n in nodes:
            seg = np.clip(rx[n.name][i0:i0 + spl], -32768, 32767).astype(np.int16)
            if n.tx_until > t: seg = np.zeros_like(seg)
            A.hl_process_audio(n.c, seg.ctypes.data, len(seg), now, 0.004)
            if n.tx_until <= t:
                k = C.c_int()
                p = A.hl_poll_tx(n.c, now, 0.5, C.byref(k))
                if p and k.value:
                    audio = np.ctypeslib.as_array(p, shape=(k.value,)).copy()   # copy: buffer is context-owned
                    n.tx_until = t + dt + k.value / FS
                    if airtime is not None: airtime[n.name] = airtime.get(n.name, 0.0) + k.value / FS
                    s0 = int(round((t + dt) * FS))
                    for (a, b), lk in links.items():
                        if a == n.name:
                            y = fm_channel(audio, rng=rng, **lk)
                            rx[b][s0:s0 + len(y)] += y
        t += dt


def test_voice_clip_through_relay_using_only_flat_api():
    pcm = (3000 * np.sin(2 * np.pi * 180 * np.arange(8000 * 4) / 8000)).astype(np.int16)
    a, r, b = N("W1AAA", seed=1), N("W9RLY", relay=1, seed=2), N("W2BBB", seed=3)
    out = (C.c_ubyte * A.hl_voice_max_clip_bytes())()
    nclip = A.hl_voice_encode(a.c, pcm.ctypes.data, len(pcm), out, len(out))
    clip = bytes(out[:nclip])
    assert nclip == 1 + 100 * 4  # 4 s -> 100 frames
    d = dict(snr_db=22, twist_db=4, ppm=100)
    links = {("W1AAA", "W9RLY"): d, ("W9RLY", "W1AAA"): d, ("W2BBB", "W9RLY"): d, ("W9RLY", "W2BBB"): d}
    ids = {}
    run([a, r, b], links, 60, [(1.0, lambda t: ids.setdefault("id", A.hl_send(a.c, b"W2BBB", 2, clip, len(clip), 1, int(t * 1000))))])
    assert ids["id"] >= 0
    assert a.status[ids["id"]] == 1
    assert b.msgs == [("W1AAA", "W2BBB", 2, clip)]
    dec = np.zeros(nclip * 80, dtype=np.int16)
    n = A.hl_voice_decode(b.c, clip, len(clip), dec.ctypes.data, len(dec))
    assert n == 100 * 320
    assert np.sqrt(np.mean(dec[:n].astype(float) ** 2)) > 500


def test_send_error_codes_and_version():
    assert A.hl_api_version() == 11
    a = N("W1AAA")
    assert A.hl_send(a.c, b"W2BBB", 1, b"x" * 4000, 4000, 1, 0) == -3   # too big
    assert A.hl_send(a.c, b"W2BBB", 9, b"x", 1, 1, 0) == -1             # bad type
    assert A.hl_create(None, 0, 1, 500, 1, *a.cb, None) is None
