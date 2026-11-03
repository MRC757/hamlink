# SPDX-License-Identifier: GPL-3.0-or-later
"""OFDM (COFDMTV) modem option, driven only through the flat hl_* API over the simulated FM channel."""
import os
import numpy as np
from test_api import A, N, run


def links(*pairs, **ch):
    d = dict(snr_db=ch.get("snr", 20), twist_db=4, ppm=100)
    out = {}
    for a, b in pairs:
        out[(a, b)] = dict(d)
        out[(b, a)] = dict(d, ppm=-100)
    return out


def deliver(modem, snr, size=500, relay=False, seconds=150, seed=1):
    a, b = N("W1AAA", seed=1, modem=modem), N("W2BBB", seed=3, modem=modem)
    nodes, pairs = [a, b], [("W1AAA", "W2BBB")]
    if relay:
        r = N("W9RLY", relay=1, seed=2, modem=modem)
        nodes, pairs = [a, r, b], [("W1AAA", "W9RLY"), ("W2BBB", "W9RLY")]
    msg = os.urandom(size)
    ids, air, done = {}, {}, {}
    def send(t): ids["id"] = A.hl_send(a.c, b"W2BBB", 2, msg, len(msg), 1, int(t * 1000))
    run(nodes, links(*pairs, snr=snr), seconds, [(1.0, send)], airtime=air, seed=seed)
    st = a.status.get(ids["id"])
    return dict(delivered=st == 1, status=st, intact=[m[3] for m in b.msgs] == [msg],
                retries=a.stats()["retries"], air=sum(air.values()), nodes=nodes, a=a, b=b)


def test_ofdm_text_with_ack_and_stats():
    r = deliver(modem=1, snr=18, size=300)
    assert r["delivered"] and r["intact"] and r["retries"] == 0
    assert r["b"].stats()["ofdm_ok"] >= 2          # data packet(s) at B...
    assert r["a"].stats()["ofdm_ok"] >= 1          # ...and the ACK packet at A


def test_ofdm_voice_clip_through_hidden_node_relay():
    r = deliver(modem=1, snr=18, size=500, relay=True, seconds=200)
    assert r["delivered"] and r["intact"]
    assert any("RELAY" in e for e in r["nodes"][1].events)


def test_ofdm_and_afsk_fx25_both_deliver_on_a_marginal_link():
    """At 7 dB SNR both get a 500-byte acknowledged message through.

    When this test was written, plain AFSK lost most frames here and OFDM's polar code won. Since then AFSK gained the
    900-2500 Hz prefilter, a second slicer and FX.25 (on by default), and on this channel model it now delivers further
    down than OFDM (seed 4: AFSK + FX.25 delivered at 5 dB, OFDM failed below 7 dB). Both must keep delivering here."""
    ofdm = deliver(modem=1, snr=7, size=500, seconds=200, seed=4)
    afsk = deliver(modem=0, snr=7, size=500, seconds=200, seed=4)
    assert ofdm["delivered"] and ofdm["intact"]
    assert ofdm["retries"] <= 1
    assert afsk["delivered"] and afsk["intact"]


def test_ofdm_airtime_is_comparable_to_afsk_on_a_clean_link():
    """Honest check on the throughput claim: raw speed is about the same; the gain is sensitivity."""
    o = deliver(modem=1, snr=25, size=1000, seconds=200)
    f = deliver(modem=0, snr=25, size=1000, seconds=200)
    assert o["delivered"] and f["delivered"]
    ratio = o["air"] / f["air"]
    print(f"airtime for 1000 B + ACK: OFDM {o['air']:.1f}s vs AFSK {f['air']:.1f}s (ratio {ratio:.2f})")
    assert 0.7 < ratio < 1.6


def test_afsk_station_hears_ofdm_not_the_other_way_round():
    """An OFDM station still decodes AFSK frames, so a mixed pair is one-way only (documented limit)."""
    a, b = N("W1AAA", seed=1, modem=0), N("W2BBB", seed=3, modem=1)
    msg = b"hello from afsk"
    run([a, b], links(("W1AAA", "W2BBB")), 30, [(1.0, lambda t: A.hl_send(a.c, b"CQ", 1, msg, len(msg), 0, int(t * 1000)))])
    assert [m[3] for m in b.msgs] == [msg]     # OFDM-mode B decoded the AFSK broadcast
    c, d = N("W1AAA", seed=1, modem=1), N("W2BBB", seed=3, modem=0)
    run([c, d], links(("W1AAA", "W2BBB")), 30, [(1.0, lambda t: A.hl_send(c.c, b"CQ", 1, msg, len(msg), 0, int(t * 1000)))])
    assert d.msgs == []                        # AFSK-mode D cannot decode OFDM
