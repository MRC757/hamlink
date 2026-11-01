# SPDX-License-Identifier: GPL-3.0-or-later
"""ctypes bindings + FM channel model + multi-node simulator for the HamLink C core."""
import ctypes as C
import os
import subprocess
import numpy as np
from scipy import signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build-host")
FS = 48000


def _load():
    os.makedirs(BUILD, exist_ok=True)
    subprocess.check_call(["cmake", "-S", ROOT, "-B", BUILD, "-DCMAKE_BUILD_TYPE=Release"],
                          stdout=subprocess.DEVNULL)
    subprocess.check_call(["cmake", "--build", BUILD, "--target", "hamlink_core_shared_voice",
                           "-j8"], stdout=subprocess.DEVNULL)
    return C.CDLL(os.path.join(BUILD, "core", "libhamlink_core_shared_voice.so"))


L = _load()

MT_TEXT, MT_VOICE, MT_ACK, MT_ID = 1, 2, 3, 4
ST_FAILED, ST_DELIVERED, ST_SENT = 0, 1, 2


class Cfg(C.Structure):
    _fields_ = [("callsign", C.c_char * 9), ("relay_enabled", C.c_int),
                ("default_ttl", C.c_uint8), ("max_retries", C.c_uint8),
                ("ack_timeout_ms", C.c_uint32), ("burst_cap_ms", C.c_uint32),
                ("preamble_ms", C.c_uint32), ("frag_class", C.c_uint32), ("bps", C.c_uint32)]


MSG_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.c_void_p, C.c_size_t)
ST_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_uint16, C.c_int)
EV_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)


class Cb(C.Structure):
    _fields_ = [("on_message", MSG_CB), ("on_status", ST_CB), ("on_event", EV_CB)]


class Stats(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in
                ("rx_frames", "rx_dups", "tx_frames", "relayed", "relay_suppressed", "retries")]


L.mesh_create.restype = C.c_void_p
L.mesh_create.argtypes = [C.POINTER(Cfg), C.POINTER(Cb), C.c_void_p, C.c_uint32]
L.mesh_destroy.argtypes = [C.c_void_p]
L.mesh_send.argtypes = [C.c_void_p, C.c_char_p, C.c_int, C.c_char_p, C.c_size_t, C.c_int,
                        C.c_uint64, C.POINTER(C.c_uint16)]
L.mesh_rx_frame.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t, C.c_uint64]
L.mesh_set_channel_busy.argtypes = [C.c_void_p, C.c_int, C.c_uint64]
L.mesh_tick.argtypes = [C.c_void_p, C.c_uint64]
L.mesh_poll_tx.argtypes = [C.c_void_p, C.c_uint64, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t), C.c_int]
L.mesh_next_wakeup.restype = C.c_uint64
L.mesh_next_wakeup.argtypes = [C.c_void_p]
L.mesh_note_external_tx.argtypes = [C.c_void_p, C.c_uint64, C.c_uint32]
L.mesh_set_relay.argtypes = [C.c_void_p, C.c_int]
L.mesh_get_stats.argtypes = [C.c_void_p, C.POINTER(Stats)]
L.mesh_pending_tx.argtypes = [C.c_void_p]
L.afsk_demod_create.restype = C.c_void_p
L.afsk_demod_destroy.argtypes = [C.c_void_p]
L.afsk_demod_process.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
L.afsk_demod_next_frame.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
L.afsk_demod_level.restype = C.c_float
L.afsk_demod_level.argtypes = [C.c_void_p]
L.modem_encode_burst.restype = C.c_size_t
L.modem_encode_burst.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_float,
                                 C.c_void_p, C.c_size_t]
L.modem_burst_max_samples.restype = C.c_size_t
L.modem_burst_max_samples.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int]


def encode_burst(frames, preamble_ms=400, tail_flags=2, amp=0.5):
    n = len(frames)
    arr = (C.c_char_p * n)(*frames)
    lens = (C.c_size_t * n)(*[len(f) for f in frames])
    cap = L.modem_burst_max_samples(lens, n, preamble_ms, tail_flags)
    out = (C.c_int16 * cap)()
    k = L.modem_encode_burst(arr, lens, n, preamble_ms, tail_flags, C.c_float(amp), out, cap)
    assert k > 0
    return np.frombuffer(out, dtype=np.int16, count=k).copy()


class Demod:
    def __init__(self):
        self.h = L.afsk_demod_create()
        self.buf = (C.c_ubyte * 400)()

    def __del__(self):
        L.afsk_demod_destroy(self.h)

    def process(self, x):
        x = np.ascontiguousarray(x, dtype=np.int16)
        L.afsk_demod_process(self.h, x.ctypes.data, len(x))
        out = []
        while True:
            n = L.afsk_demod_next_frame(self.h, self.buf, 400)
            if n == 0:
                return out
            out.append(bytes(self.buf[:n]))

    def level(self):
        return L.afsk_demod_level(self.h)


# ---------------------------------------------------------------- channel model
def fm_channel(x, snr_db=25.0, twist_db=0.0, ppm=0.0, gain=1.0, rng=None):
    """Crude cheap-HT audio path: 300-3000 Hz band limit, optional spectral tilt
    (twist_db of loss at high audio frequencies), sample-clock offset, AWGN."""
    rng = rng or np.random.default_rng()
    y = x.astype(np.float64) / 32768.0
    sos = signal.butter(4, [300, 3000], btype="band", fs=FS, output="sos")
    y = signal.sosfilt(sos, y)
    if twist_db:
        b, a = signal.butter(1, 1700, btype="low", fs=FS)
        low = signal.lfilter(b, a, y)
        y = low + 10 ** (-twist_db / 20) * (y - low)
    if ppm:
        t = np.arange(len(y)) * (1.0 + ppm * 1e-6)
        y = np.interp(t, np.arange(len(y)), y)
    y *= gain
    sig_p = np.mean(y ** 2) + 1e-12
    if snr_db is not None:
        sigma = np.sqrt(sig_p / 10 ** (snr_db / 10) * (FS / 2) / 2700.0)
        y = y + rng.normal(0, sigma, len(y))
    return np.clip(y * 32768.0, -32768, 32767).astype(np.int16)


# ---------------------------------------------------------------- mesh node + simulator
class Node:
    def __init__(self, name, relay=False, ttl=2, seed=1, **cfg_over):
        self.name = name
        self.messages = []   # (src, dst, type, data)
        self.status = {}     # msgid -> status
        self.events = []
        self.tx_log = []     # (t_start_s, t_end_s, [frame bytes])
        self.id_times = []   # times (s) an ID frame was transmitted
        cfg = Cfg()
        L.mesh_cfg_defaults(C.byref(cfg), name.encode())
        cfg.relay_enabled = 1 if relay else 0
        cfg.default_ttl = ttl
        for k, v in cfg_over.items():
            setattr(cfg, k, v)

        def on_msg(_u, src, dst, typ, data, n):
            self.messages.append((src.decode().strip("\0"), dst.decode().strip("\0"), typ,
                                  C.string_at(data, n)))

        def on_st(_u, mid, st):
            self.status[mid] = st

        def on_ev(_u, text):
            self.events.append(text.decode())

        self._cb = Cb(MSG_CB(on_msg), ST_CB(on_st), EV_CB(on_ev))  # keep alive
        self.cfg = cfg
        self.m = L.mesh_create(C.byref(cfg), C.byref(self._cb), None, seed)
        self.demod = Demod()
        self.tx_until = 0.0
        self.buf = (C.c_ubyte * 4096)()
        self.lens = (C.c_size_t * 32)()

    def send(self, dst, data, typ=MT_TEXT, ack=True, now=0.0):
        mid = C.c_uint16()
        rc = L.mesh_send(self.m, dst.encode(), typ, data, len(data), 1 if ack else 0,
                         int(now * 1000), C.byref(mid))
        assert rc == 0, rc
        return mid.value

    def stats(self):
        s = Stats()
        L.mesh_get_stats(self.m, C.byref(s))
        return {n: getattr(s, n) for n, _ in Stats._fields_}


class Sim:
    """Discrete-time simulator. links[(a,b)] = dict(snr, twist, ppm, gain, drop) for audio a->b;
    absent link means out of range."""

    def __init__(self, nodes, links, duration_s=120, seed=7, busy_thresh=0.004):
        self.nodes = {n.name: n for n in nodes}
        self.links = links
        self.rng = np.random.default_rng(seed)
        self.T = int(duration_s * FS)
        self.rx = {n.name: np.zeros(self.T + FS * 30, dtype=np.int32) for n in nodes}
        self.t = 0.0
        self.busy_thresh = busy_thresh
        self.dt = 0.02

    def _transmit(self, node, frames, t):
        audio = encode_burst(frames, preamble_ms=node.cfg.preamble_ms)
        dur = len(audio) / FS
        node.tx_until = t + dur
        node.tx_log.append((t, t + dur, frames))
        if any(f[0] == MT_ID for f in frames):
            node.id_times.append(t)
        start = int(round(t * FS))
        for (a, b), lk in self.links.items():
            if a != node.name:
                continue
            y = fm_channel(audio, snr_db=lk.get("snr", 25), twist_db=lk.get("twist", 0),
                           ppm=lk.get("ppm", 0), gain=lk.get("gain", 1.0), rng=self.rng)
            end = min(start + len(y), len(self.rx[b]))
            self.rx[b][start:end] += y[: end - start]

    def run(self, until_s, script=None):
        """script: list of (time_s, callable(sim)) events"""
        script = sorted(script or [], key=lambda e: e[0])
        si = 0
        n_steps = int((until_s - self.t) / self.dt)
        spl = int(self.dt * FS)
        for _ in range(n_steps):
            t = self.t
            while si < len(script) and script[si][0] <= t:
                script[si][1](self)
                si += 1
            i0 = int(round(t * FS))
            now_ms = int((t + self.dt) * 1000)
            for node in self.nodes.values():
                seg = np.clip(self.rx[node.name][i0:i0 + spl], -32768, 32767).astype(np.int16)
                if node.tx_until > t:
                    seg = np.zeros_like(seg)  # half duplex: deaf while keyed
                for f in node.demod.process(seg):
                    lk = [v for (a, b), v in self.links.items() if b == node.name]
                    drop = max([v.get("drop", 0) for v in lk] or [0])
                    if self.rng.random() >= drop:
                        L.mesh_rx_frame(node.m, f, len(f), now_ms)
                busy = node.demod.level() > self.busy_thresh or node.tx_until > t + self.dt
                L.mesh_set_channel_busy(node.m, 1 if busy else 0, now_ms)
                L.mesh_tick(node.m, now_ms)
                if node.tx_until <= t:
                    n = L.mesh_poll_tx(node.m, now_ms, node.buf, 4096, node.lens, 32)
                    if n:
                        frames, off = [], 0
                        raw = bytes(node.buf)
                        for k in range(n):
                            frames.append(raw[off:off + node.lens[k]])
                            off += node.lens[k]
                        self._transmit(node, frames, t + self.dt)
            self.t += self.dt
