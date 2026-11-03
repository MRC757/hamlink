#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure how much CPU each modem needs, through the same flat C API the apps use (hamlink_api.h).

For a Raspberry Pi relay the question is whether one core keeps up with the receiver in real time, with room to spare.
For every modem this reports:
  * RX idle    - % of one CPU core to receive channel noise (a relay's normal state: listening)
  * RX traffic - % of one core while receiving a stream of messages (and how many of them decoded)
  * TX encode  - time to build one transmission, as % of that transmission's length on the air

Below about 50% of one core leaves plenty of room; near or above 100% the receiver falls behind real time.

    python3 tools/bench_modems.py                          # finds build/core/libhamlink_api.so or the Windows DLL
    python3 tools/bench_modems.py --lib path/to/libhamlink_api.so --seconds 60 --modem mt63

Build the library on Linux / a Pi first:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
                                          cmake --build build --target hamlink_api -j4
Needs numpy.
"""
import argparse
import ctypes as C
import os
import platform
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FS = 48000
CHUNK = 960  # 20 ms, as the apps feed audio
MODEMS = {"afsk": 0, "ofdm": 1, "mt63": 2, "psk2400": 3, "psk3600": 4}
MSG = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.c_void_p, C.c_int)
ST = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_int)
EV = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)
KEEP = []  # ctypes callbacks must outlive the contexts that hold them


def find_lib():
    for p in ("build/core/libhamlink_api.so", "build-host/core/libhamlink_api.so", "core/build/libhamlink_api.so",
              "windows/native/hamlink_core.dll"):
        if os.path.exists(os.path.join(ROOT, p)):
            return os.path.join(ROOT, p)
    raise SystemExit("hamlink library not found: build it (see the top of this file) or pass --lib")


def load(path):
    L = C.CDLL(os.path.abspath(path))
    L.hl_api_version.restype = C.c_int
    L.hl_create.restype = C.c_void_p
    L.hl_create.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int, C.c_uint32, MSG, ST, EV, C.c_void_p]
    L.hl_destroy.argtypes = [C.c_void_p]
    L.hl_set_modem.argtypes = [C.c_void_p, C.c_int]
    L.hl_set_modem.restype = C.c_int
    L.hl_send.argtypes = [C.c_void_p, C.c_char_p, C.c_int, C.c_char_p, C.c_int, C.c_int, C.c_int64]
    L.hl_poll_tx.restype = C.c_void_p
    L.hl_poll_tx.argtypes = [C.c_void_p, C.c_int64, C.c_float, C.POINTER(C.c_int)]
    L.hl_process_audio.restype = C.c_float
    L.hl_process_audio.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int64, C.c_float]
    return L


class Station:
    def __init__(self, L, call, modem, seed):
        self.L, self.got = L, []
        cbs = (MSG(lambda u, s, d, t, p, n: self.got.append(t)), ST(lambda *a: None), EV(lambda *a: None))
        KEEP.append(cbs)
        self.c = L.hl_create(call, 0, 1, 500, seed, *cbs, None)
        if L.hl_set_modem(self.c, modem) != 0:
            raise RuntimeError("could not start this modem")

    def feed(self, pcm, now):
        """Feeds pcm in 20 ms chunks; returns (new time in ms, seconds of CPU spent)."""
        pcm = np.ascontiguousarray(pcm, np.int16)
        base = pcm.ctypes.data
        t0 = time.perf_counter()
        for i in range(0, len(pcm), CHUNK):
            self.L.hl_process_audio(self.c, base + 2 * i, min(CHUNK, len(pcm) - i), now + i // 48, 0.004)
        return now + len(pcm) // 48, time.perf_counter() - t0

    def close(self):
        self.L.hl_destroy(self.c)


def noise(rng, n, sigma=30.0):
    return np.clip(np.round(rng.normal(0, sigma, n)), -32768, 32767).astype(np.int16)


def make_bursts(L, modem, count, rng):
    """`count` transmissions of a 60-byte text message, and the CPU time spent building each one."""
    tx = Station(L, b"BENCH", modem, 1)
    now, bursts, enc = 1000, [], []
    for i in range(count):
        text = f"bench {i:03d} the quick brown fox jumps over the lazy dog 0123456789".encode()
        L.hl_send(tx.c, b"CQ", 1, text, len(text), 0, now)
        for _ in range(400):
            now, _ = tx.feed(noise(rng, 2400), now)          # receivers and timers run on audio time
            n = C.c_int()
            t0 = time.perf_counter()
            p = L.hl_poll_tx(tx.c, now, 0.5, C.byref(n))
            dt = time.perf_counter() - t0
            if p:
                bursts.append(np.ctypeslib.as_array(C.cast(p, C.POINTER(C.c_int16)), (n.value,)).copy())
                enc.append(dt)
                break
            now += 50
    tx.close()
    return bursts, enc


def bench(L, name, modem, seconds, rng):
    # receive, idle: noise only
    rx = Station(L, b"RELAY", modem, 2)
    _, cpu = rx.feed(noise(rng, seconds * FS), 1000)
    idle = cpu / seconds
    rx.close()

    # receive, traffic: transmissions back to back with 1.5 s of noise between, plus noise on the signal (~20 dB SNR)
    one, _ = make_bursts(L, modem, 1, rng)
    per = len(one[0]) / FS + 1.5
    count = max(2, int(seconds / per))
    bursts, enc = make_bursts(L, modem, count, rng)
    parts = []
    for b in bursts:
        parts += [noise(rng, int(1.5 * FS)), np.clip(b.astype(np.int32) + noise(rng, len(b), 300), -32768, 32767)]
    parts.append(noise(rng, 6 * FS))                         # let slow receivers (MT63) finish
    stream = np.concatenate(parts).astype(np.int16)
    rx = Station(L, b"RELAY", modem, 2)
    _, cpu = rx.feed(stream, 1000)
    busy = cpu / (len(stream) / FS)
    got = sum(1 for t in rx.got if t == 1)
    rx.close()

    air = sum(len(b) for b in bursts) / FS
    return idle * 100, busy * 100, got, len(bursts), sum(enc) / air * 100, air / len(bursts)


def cpu_name():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.lower().startswith(("model name", "hardware", "model\t")):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--lib", help="libhamlink_api.so or hamlink_core.dll (default: found in the build tree)")
    ap.add_argument("--seconds", type=int, default=30, help="audio per test (default 30)")
    ap.add_argument("--modem", choices=["all"] + list(MODEMS), default="all")
    a = ap.parse_args()
    path = a.lib or find_lib()
    L = load(path)
    print(f"HamLink modem benchmark: API version {L.hl_api_version()}, {os.path.basename(path)}")
    print(f"machine: {platform.machine()}, {cpu_name()}, {os.cpu_count()} CPUs, Python {platform.python_version()}")
    print(f"{a.seconds} s of audio per test; figures are % of ONE core\n")
    print(f"{'modem':9s} {'RX idle':>8s} {'RX traffic':>11s} {'decoded':>9s} {'TX encode':>10s} {'burst':>7s}")
    rng = np.random.default_rng(7)
    for name in (MODEMS if a.modem == "all" else [a.modem]):
        idle, busy, got, sent, enc, burst = bench(L, name, MODEMS[name], a.seconds, rng)
        print(f"{name:9s} {idle:7.1f}% {busy:10.1f}% {got:4d}/{sent:<4d} {enc:9.1f}% {burst:6.1f}s", flush=True)


if __name__ == "__main__":
    main()
