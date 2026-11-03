# SPDX-License-Identifier: GPL-3.0-or-later
"""Receive DSP: data carrier detect, single-bit repair, carrier-sense modes. See docs/DSP_IMPROVEMENTS.md."""
import ctypes as C
import numpy as np
from scipy import signal
from hamlink import L, FS, encode_burst, fm_channel, Demod
from test_api import A, N, run

L.afsk_demod_create.restype = C.c_void_p
L.afsk_demod_dcd.argtypes = [C.c_void_p]
L.afsk_demod_set_repair.argtypes = [C.c_void_p, C.c_int]
L.afsk_demod_frames_repaired.argtypes = [C.c_void_p]
L.afsk_demod_frames_repaired.restype = C.c_uint32
L.afsk_demod_process.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
L.afsk_demod_next_frame.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
L.afsk_demod_destroy.argtypes = [C.c_void_p]
A.hl_set_carrier_sense.argtypes = [C.c_void_p, C.c_int]
A.hl_set_bit_repair.argtypes = [C.c_void_p, C.c_int]
A.hl_channel_busy.argtypes = [C.c_void_p]
A.hl_get_rx_info.argtypes = [C.c_void_p, C.c_void_p]


def dcd_trace(y, step=480):
    """DCD state every 10 ms."""
    d = L.afsk_demod_create()
    y = np.ascontiguousarray(np.clip(y, -32767, 32767).astype(np.int16))
    out = []
    for i in range(0, len(y) - step, step):
        seg = np.ascontiguousarray(y[i:i + step])
        L.afsk_demod_process(d, seg.ctypes.data, step)
        out.append(L.afsk_demod_dcd(d))
    L.afsk_demod_destroy(d)
    return np.array(out)


def decode(y, repair):
    d = L.afsk_demod_create()
    L.afsk_demod_set_repair(d, repair)
    y = np.ascontiguousarray(y)
    L.afsk_demod_process(d, y.ctypes.data, len(y))
    buf = (C.c_ubyte * 400)()
    got = []
    while True:
        n = L.afsk_demod_next_frame(d, buf, 400)
        if not n:
            break
        got.append(bytes(buf[:n]))
    rep = L.afsk_demod_frames_repaired(d)
    L.afsk_demod_destroy(d)
    return got, rep


T = np.arange(FS * 20) / FS
RNG = np.random.default_rng(5)
_bn = signal.sosfilt(signal.butter(4, [300, 3000], btype="band", fs=FS, output="sos"), RNG.normal(0, 1, len(T)))
BAND_NOISE = _bn / np.std(_bn) * 6000
SPEECH_LIKE = BAND_NOISE * (0.5 + 0.5 * np.sin(2 * np.pi * 4 * T)) ** 2 * 1.5 + 4000 * np.sin(2 * np.pi * (180 + 40 * np.sin(2 * np.pi * 3 * T)) * T)


def test_dcd_ignores_noise_voice_and_tones():
    cases = {"white noise": RNG.normal(0, 4000, len(T)), "band noise": BAND_NOISE, "speech-like": SPEECH_LIKE}
    for f in (600, 1000, 1200, 1700, 2200, 3400):   # 1000 Hz used to fool the clock detector (beat at the bit rate)
        cases[f"tone {f}"] = 8000 * np.sin(2 * np.pi * f * T)
    cases["calibration tone 1200/2200"] = 8000 * np.sin(2 * np.pi * np.where((T % 2) < 1, 1200, 2200) * T)
    for name, x in cases.items():
        assert dcd_trace(x).mean() < 0.02, name


def test_dcd_detects_data_fast_and_releases():
    rng = np.random.default_rng(6)
    for snr in (6, 8, 10, 14, 20):
        fr = [bytes(rng.integers(0, 256, 119, dtype=np.uint8)) for _ in range(4)]
        a = encode_burst(fr, preamble_ms=300)
        y = fm_channel(np.concatenate([np.zeros(FS, np.int16), a, np.zeros(10 * FS, np.int16)]), snr_db=snr, rng=rng)
        tr = dcd_trace(y)
        nb = len(a) // 480
        on = tr[100:100 + nb]
        assert on.mean() > 0.97, snr                       # held for the whole burst
        assert np.argmax(on) <= 5, snr                     # on within 50 ms of the burst starting
        after = tr[100 + nb:]
        assert (after[:60] == 0).any(), snr                # released within 600 ms of the end
        # Afterwards, in open-squelch hiss, only occasional false detections: plain hiss gives them about 1-2% of the
        # time with or without the prefilter, and just after a burst about 3% (48 runs: at most 1.07 s busy in the 10 s
        # after a burst). They delay a transmission slightly and do not affect decoding; with the squelch closed the
        # receiver hears silence and there are none. An earlier version required none at all in a 2.4 s window, which
        # passed or failed on the luck of the noise.
        assert after[60:].sum() * 0.01 < 1.5, snr          # seconds of false busy in the 10 s after the burst


def test_bit_repair_recovers_frames_and_never_outputs_wrong_data():
    rng = np.random.default_rng(7)
    stats = {}
    for snr in (8, 9):
        sent = off = on = 0
        for b in range(30):
            frames = [bytes(rng.integers(0, 256, 119, dtype=np.uint8)) for _ in range(8)]
            y = fm_channel(np.concatenate([np.zeros(9600, np.int16), encode_burst(frames), np.zeros(9600, np.int16)]), snr_db=snr, rng=rng)
            g0, _ = decode(y, 0)
            g1, _ = decode(y, 1)
            assert all(g in frames for g in g0 + g1)        # nothing that was not sent
            off += sum(f in g0 for f in frames)
            on += sum(f in g1 for f in frames)
            sent += 8
        stats[snr] = (off / sent, on / sent)
    # Measured when written (before the prefilter and second slicer): 8 dB 56% -> 76%, 9 dB 88% -> 97%. With them the
    # receiver alone gets most frames, so repair has less to do: 8 dB 95.8% -> 99.6%, 9 dB 100% -> 100%.
    assert stats[8][1] > stats[8][0] + 0.02, stats
    assert stats[8][1] >= 0.98, stats
    assert stats[9][1] >= stats[9][0], stats


def test_bit_repair_heavy_corruption_and_noise_give_no_false_frames():
    rng = np.random.default_rng(99)
    wrong = 0
    for b in range(60):
        frames = [bytes(rng.integers(0, 256, 119, dtype=np.uint8)) for _ in range(8)]
        y = fm_channel(np.concatenate([np.zeros(4800, np.int16), encode_burst(frames), np.zeros(4800, np.int16)]),
                       snr_db=float(rng.choice([5, 6, 7])), rng=rng)
        got, _ = decode(y, 1)
        wrong += sum(g not in frames for g in got)
    assert wrong == 0
    noise = np.ascontiguousarray(rng.normal(0, 3000, FS * 300).clip(-32768, 32767).astype(np.int16))  # 5 minutes
    got, _ = decode(noise, 1)
    assert got == []


def test_carrier_sense_modes_with_loud_room_noise():
    """Acoustic test in a noisy room: loud non-data audio blocks sending in the default mode (data or level),
    but not in data-only mode, which still holds off for real data."""
    def node_level_busy(mode):
        a = N("W1AAA", seed=1)
        A.hl_set_carrier_sense(a.c, mode)
        x = np.ascontiguousarray(BAND_NOISE[:FS * 3].astype(np.int16))
        busy = []
        for i in range(0, len(x) - 960, 960):
            seg = np.ascontiguousarray(x[i:i + 960])
            A.hl_process_audio(a.c, seg.ctypes.data, 960, int(i / 48), 0.004)
            busy.append(A.hl_channel_busy(a.c))
        return np.mean(busy[20:])
    assert node_level_busy(2) > 0.95     # default: loud audio = busy
    assert node_level_busy(1) < 0.05     # data only: room noise ignored
    assert node_level_busy(0) == 0

    # data-only mode still detects a real transmission
    a = N("W1AAA", seed=1)
    A.hl_set_carrier_sense(a.c, 1)
    fr = [bytes(range(60))]
    y = np.ascontiguousarray(fm_channel(encode_burst(fr), snr_db=20, rng=np.random.default_rng(1)))
    hits = []
    for i in range(0, len(y) - 960, 960):
        seg = np.ascontiguousarray(y[i:i + 960])
        A.hl_process_audio(a.c, seg.ctypes.data, 960, int(i / 48), 10.0)   # level threshold impossibly high
        hits.append(A.hl_channel_busy(a.c))
    assert np.mean(hits[5:]) > 0.9


def test_rx_info_counts_repairs_end_to_end():
    LINK = dict(snr_db=7.5, twist_db=2, ppm=80)
    links = {("W1AAA", "W2BBB"): LINK, ("W2BBB", "W1AAA"): LINK}
    a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
    run([a, b], links, 170, [(1.0 + 4 * k, (lambda k: lambda t: A.hl_send(a.c, b"CQ", 1, b"x" * 90 + bytes([k]), 91, 0, int(t * 1000)))(k)) for k in range(40)])
    info = (C.c_uint32 * 6)()
    A.hl_get_rx_info(b.c, info)
    frames_ok, repaired = info[1], info[2]
    assert frames_ok > 0 and repaired > 0, list(info)
    assert all(m[3][:90] == b"x" * 90 for m in b.msgs)  # every delivered message intact
