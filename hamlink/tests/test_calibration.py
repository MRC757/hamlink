# SPDX-License-Identifier: GPL-3.0-or-later
"""Tone generator, receive meter and the station-ID hook behind the PTT test / calibration screens."""
import ctypes as C
import numpy as np
import test_api as T
from test_api import A, N
from hamlink import Demod, MT_ID

A.hl_tone_start.argtypes = [C.c_void_p, C.c_int]
A.hl_tone_fill.restype = C.c_float
A.hl_tone_fill.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_float]
A.hl_note_external_tx.argtypes = [C.c_void_p, C.c_int64, C.c_int]
A.hl_get_meter.argtypes = [C.c_void_p, C.c_void_p]


def tone(c, mode, amp, seconds, chunk=960):
    A.hl_tone_start(c, mode)
    out = []
    for _ in range(int(seconds * 48000 / chunk)):
        b = np.zeros(chunk, dtype=np.int16)
        A.hl_tone_fill(c, b.ctypes.data, chunk, amp)
        out.append(b)
    return np.concatenate(out)


def meter(c):
    m = np.zeros(5, dtype=np.float32)
    A.hl_get_meter(c, m.ctypes.data)
    return dict(rms=m[0], peak=m[1], t1200=m[2], t2200=m[3], clipped=int(m[4]))


def feed(c, x, now=0):
    x = np.ascontiguousarray(x, dtype=np.int16)
    for i in range(0, len(x), 960):
        s = x[i:i + 960].copy()
        A.hl_process_audio(c, s.ctypes.data, len(s), now, 0.004)


def test_meter_reads_tone_levels_in_dbfs():
    n = N("W1AAA")
    feed(n.c, tone(n.c, 1, 0.5, 1.0))
    m = meter(n.c)
    assert abs(m["t1200"] - (-6.02)) < 0.3          # half-scale sine = -6 dBFS
    assert m["t2200"] < -60                          # no 2200 Hz present
    assert abs(m["peak"] - (-6.02)) < 0.3
    assert m["clipped"] == 0
    feed(n.c, tone(n.c, 2, 0.25, 1.0))
    m = meter(n.c)
    assert abs(m["t2200"] - (-12.04)) < 0.3 and m["t1200"] < -60


def test_meter_measures_twist_between_tones():
    """The calibration screen shows 2200 Hz vs 1200 Hz level; a 6 dB-down 2200 Hz tone must read as -6 dB."""
    n = N("W1AAA")
    a = tone(n.c, 1, 0.5, 0.5)
    b = tone(n.c, 2, 0.25, 0.5)
    feed(n.c, a); m1 = meter(n.c)
    feed(n.c, b); m2 = meter(n.c)
    assert abs((m2["t2200"] - m1["t1200"]) - (-6.02)) < 0.4


def test_meter_flags_clipping_and_silence():
    n = N("W1AAA")
    loud = np.clip(tone(n.c, 1, 1.0, 0.5).astype(np.int32) * 2, -32768, 32767).astype(np.int16)
    feed(n.c, loud)
    assert meter(n.c)["clipped"] > 0
    feed(n.c, np.zeros(48000, dtype=np.int16))
    m = meter(n.c)
    assert m["clipped"] == 0 and m["peak"] <= -100


def test_tone_fades_in_out_without_clicks_and_alt_is_phase_continuous():
    n = N("W1AAA")
    x = tone(n.c, 3, 0.6, 2.5).astype(np.int64)   # ALT: 1200 Hz, then 2200 Hz after 1 s, then back
    ramp = np.minimum(0.6, (np.arange(len(x[:400])) + 1) / (48000 * 0.005)) * 32767   # 5 ms per full scale
    assert x[0] == 0 and (np.abs(x[:400]) <= ramp + 2).all()   # never exceeds the fade-in envelope
    d = np.abs(np.diff(x))
    assert d.max() < 0.6 * 32767 * 2 * np.sin(np.pi * 2200 / 48000) * 1.05   # no step bigger than a 2200 Hz sine's slope
    seg = x[48000 + 4800:96000 - 4800]                 # inside the 2200 Hz second
    assert abs(np.argmax(np.abs(np.fft.rfft(seg))) * 48000 / len(seg) - 2200) < 30
    seg = x[4800:48000 - 4800]                         # inside the 1200 Hz first second
    assert abs(np.argmax(np.abs(np.fft.rfft(seg))) * 48000 / len(seg) - 1200) < 30
    # fade-out: target 0 ends at exactly 0 within ~5 ms * amp
    b = np.zeros(960, dtype=np.int16)
    reached = A.hl_tone_fill(n.c, b.ctypes.data, 960, 0.0)
    assert reached == 0.0 and b[-1] == 0


def test_off_mode_is_silent_ptt_only():
    n = N("W1AAA")
    assert np.abs(tone(n.c, 0, 0.9, 0.3)).max() == 0


def test_test_transmission_triggers_station_id():
    """A test tone is a transmission: the station must identify right after it, once."""
    n = N("W1AAA", seed=5)
    dec = Demod()
    k, buf = C.c_int(), None
    A.hl_note_external_tx(n.c, 1000, 4000)               # tone ran 1.0 s .. 5.0 s
    assert not A.hl_poll_tx(n.c, 3000, 0.5, C.byref(k))  # still "on the air": nothing yet
    p = A.hl_poll_tx(n.c, 5100, 0.5, C.byref(k))
    assert p and k.value > 0
    audio = np.ctypeslib.as_array(p, shape=(k.value,)).copy()
    frames = dec.process(np.concatenate([np.zeros(4800, np.int16), audio, np.zeros(4800, np.int16)]))
    assert len(frames) == 1 and frames[0][0] == MT_ID
    assert frames[0][3:11].rstrip(b"\0") == b"W1AAA" and frames[0][23:] == b"DE W1AAA"
    assert not A.hl_poll_tx(n.c, 9000, 0.5, C.byref(k))   # sent once, not repeatedly
    # a second test later IDs again
    A.hl_note_external_tx(n.c, 20000, 1000)
    assert A.hl_poll_tx(n.c, 21100, 0.5, C.byref(k))
