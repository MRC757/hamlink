# SPDX-License-Identifier: GPL-3.0-or-later
"""PSK 2400 (QPSK) and PSK 3600 (8-PSK), driven only through the flat hl_* API over the simulated FM channel.
Regression tests for delivery, speed, robustness and level independence. tools/psk_sweep.py measures the same things
over a range of signal-to-noise ratios."""
import ctypes as C
import os
import numpy as np
from hamlink import FS
from test_api import A, N, run

AFSK, PSK2400, PSK3600 = 0, 3, 4
TEXT, VOICE = 1, 2


def links(pairs, snr, twist=3, ppm=50):
    d = dict(snr_db=snr, twist_db=twist, ppm=ppm)
    out = {}
    for a, b in pairs:
        out[(a, b)] = dict(d)
        out[(b, a)] = dict(d, ppm=-ppm)
    return out


def deliver(modem, snr, size=500, rx_modem=None, relay=False, seconds=120, seed=1, twist=3, ppm=50):
    a = N("W1AAA", seed=1, modem=modem)
    b = N("W2BBB", seed=3, modem=rx_modem if rx_modem is not None else modem)
    nodes, pairs = [a, b], [("W1AAA", "W2BBB")]
    if relay:
        r = N("W9RLY", relay=1, seed=2, modem=modem)
        nodes, pairs = [a, r, b], [("W1AAA", "W9RLY"), ("W2BBB", "W9RLY")]
    msg = os.urandom(size)
    ids, air = {}, {}
    def send(t): ids["id"] = A.hl_send(a.c, b"W2BBB", VOICE, msg, len(msg), 1, int(t * 1000))
    run(nodes, links(pairs, snr, twist, ppm), seconds, [(1.0, send)], airtime=air, seed=seed)
    return dict(delivered=a.status.get(ids["id"]) == 1, intact=[m[3] for m in b.msgs] == [msg],
                retries=a.stats()["retries"], air=sum(air.values()), nodes=nodes)


# ---------------------------------------------------------------- delivery

def test_psk2400_delivers_an_acknowledged_message():
    r = deliver(PSK2400, snr=12)
    assert r["delivered"] and r["intact"] and r["retries"] == 0


def test_psk3600_delivers_on_a_clean_link():
    r = deliver(PSK3600, snr=16)
    assert r["delivered"] and r["intact"] and r["retries"] == 0


def test_psk2400_holds_on_near_its_threshold():
    """Measured threshold about 6 dB SNR; at 8 dB it must still get through (retries allowed)."""
    r = deliver(PSK2400, snr=8, seed=4)
    assert r["delivered"] and r["intact"]


def test_psk3600_holds_on_near_its_threshold():
    """Measured threshold about 10 dB SNR."""
    r = deliver(PSK3600, snr=12, seed=4)
    assert r["delivered"] and r["intact"]


def test_psk_copes_with_twist_and_clock_offset():
    """De-emphasis-style tilt and a 200 ppm sound-card clock difference: the equaliser and timing loop must absorb them."""
    r = deliver(PSK2400, snr=12, twist=8, ppm=200, seed=5)
    assert r["delivered"] and r["intact"]


def test_psk2400_and_3600_stations_hear_each_other():
    """One receiver decodes both: a 3600 sender reaches a 2400 station and gets its ACK back (sent at 2400)."""
    r = deliver(PSK3600, snr=16, rx_modem=PSK2400)
    assert r["delivered"] and r["intact"]


def test_psk_through_a_relay():
    r = deliver(PSK2400, snr=14, relay=True, seconds=150)
    assert r["delivered"] and r["intact"]
    assert any("RELAY" in e for e in r["nodes"][1].events)


def test_psk_is_faster_than_afsk():
    """1000-byte acknowledged message on a clean link: PSK 2400 well under AFSK with FX.25, PSK 3600 faster still."""
    air = {m: deliver(m, snr=20, size=1000, seconds=150)["air"] for m in (AFSK, PSK2400, PSK3600)}
    assert air[PSK2400] < 0.7 * air[AFSK], air
    assert air[PSK3600] < air[PSK2400], air


# ---------------------------------------------------------------- receiver properties

def burst(modem, text, seed=7):
    n = N("W5PSK", seed=seed, modem=modem)
    assert A.hl_send(n.c, b"CQ", TEXT, text, len(text), 0, 1000) >= 0
    for k in range(1, 500):
        A.hl_process_audio(n.c, None, 0, 1000 + 20 * k, 0.004)
        cnt = C.c_int()
        p = A.hl_poll_tx(n.c, 1000 + 20 * k, 0.5, C.byref(cnt))
        if p and cnt.value:
            return np.ctypeslib.as_array(p, shape=(cnt.value,)).astype(float)
    raise AssertionError("no burst")


def feed(node, x):
    pcm = np.round(np.clip(x, -32768, 32767)).astype(np.int16)
    for i in range(0, len(pcm) - 960, 960):
        seg = np.ascontiguousarray(pcm[i:i + 960])
        A.hl_process_audio(node.c, seg.ctypes.data, 960, 2000 + i // 48, 0.004)


def test_psk_decodes_quiet_input_as_well_as_loud():
    """No absolute level floor (OFDM had one at -47 dBFS): the same burst at 25 dB SNR decodes from -20 to -65 dBFS."""
    rng = np.random.default_rng(11)
    for modem in (PSK2400, PSK3600):
        text = b"level test " * 6
        b = burst(modem, text)
        for level in (-20, -45, -65):
            rx = N("W2BBB", seed=3, modem=modem)
            sig = np.concatenate([np.zeros(FS), b, np.zeros(2 * FS)])
            sig *= 32768 * 10 ** (level / 20) / np.sqrt(np.mean(b ** 2))
            sig += rng.normal(0, np.sqrt(np.mean(sig[FS:FS + len(b)] ** 2)) / 10 ** (25 / 20), len(sig))
            feed(rx, sig)
            assert [m[3] for m in rx.msgs] == [text], (modem, level)


def test_psk_receiver_finds_nothing_in_noise():
    """A minute of noise at several levels: no false messages."""
    rng = np.random.default_rng(12)
    rx = N("W2BBB", seed=3, modem=PSK2400)
    for sigma in (30, 300, 3000):
        feed(rx, rng.normal(0, sigma, 20 * FS))
    assert rx.msgs == []


def test_psk_burst_is_shorter_than_afsk_for_the_same_frame():
    text = b"x" * 120
    afsk, p24, p36 = (len(burst(m, text)) for m in (AFSK, PSK2400, PSK3600))
    assert p36 < p24 < afsk, (afsk, p24, p36)
