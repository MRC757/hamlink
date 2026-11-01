# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end protocol tests over the simulated audio channel.  Run: python3 -m pytest -q tests"""
import os
import numpy as np
from hamlink import *

GOOD = dict(snr=22, twist=4, ppm=120)


def link(a, b, **kw):
    d = dict(GOOD)
    d.update(kw)
    return {(a, b): dict(d), (b, a): dict(d, ppm=-d.get("ppm", 0))}


def test_direct_text_with_ack():
    a, b = Node("W1AAA", seed=11), Node("W2BBB", seed=22)
    sim = Sim([a, b], link("W1AAA", "W2BBB"), duration_s=60)
    msg = os.urandom(300)
    ids = {}
    sim.run(40, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", msg, now=s.t)))])
    assert b.messages == [("W1AAA", "W2BBB", MT_TEXT, msg)]
    assert a.status[ids["id"]] == ST_DELIVERED
    assert a.stats()["retries"] == 0


def test_arq_recovers_from_frame_loss():
    # 35% loss in both directions: with the default 4 retries roughly 1 run in 10 ends FAILED even though
    # the message arrived (every ACK got lost). 8 retries makes delivery deterministic. Any single run can, by chance,
    # get every frame through first time (about 2%), so several runs are made and retries must occur in most.
    retried = 0
    for seed in (5, 6, 7, 8):
        a, b = Node("W1AAA", seed=3, max_retries=8), Node("W2BBB", seed=4)
        sim = Sim([a, b], link("W1AAA", "W2BBB", drop=0.35), duration_s=200, seed=seed)
        msg = bytes((seed * 31 + i) & 0xFF for i in range(96 * 7 + 10))  # 8 fragments
        ids = {}
        sim.run(180, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", msg, now=s.t)))])
        assert a.status[ids["id"]] == ST_DELIVERED, seed
        assert [m[3] for m in b.messages] == [msg], seed
        retried += a.stats()["retries"] >= 1
    assert retried >= 3

def test_unreachable_destination_fails_after_retries():
    a, b = Node("W1AAA", seed=3, max_retries=2), Node("W2BBB", seed=4)
    sim = Sim([a, b], {}, duration_s=120)  # no links at all
    ids = {}
    sim.run(100, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", b"hello", now=s.t)))])
    assert a.status[ids["id"]] == ST_FAILED
    assert a.stats()["retries"] == 2


def test_hidden_node_via_simplex_relay():
    """A and B cannot hear each other; R sits on the hill and hears both."""
    a, r, b = Node("W1AAA", seed=1), Node("W9RLY", relay=True, seed=2), Node("W2BBB", seed=3)
    links = {}
    links.update(link("W1AAA", "W9RLY"))
    links.update(link("W2BBB", "W9RLY"))
    sim = Sim([a, r, b], links, duration_s=150)
    msg = os.urandom(500)  # 6 fragments, like a ~3 s Codec2-700C voice clip + margin
    ids = {}
    sim.run(120, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", msg, typ=MT_VOICE, now=s.t)))])
    assert b.messages == [("W1AAA", "W2BBB", MT_VOICE, msg)]
    assert a.status[ids["id"]] == ST_DELIVERED, a.status
    assert r.stats()["relayed"] >= 7  # 6 data frames + at least one ACK
    # regression: relay must keep a burst together/in order, else the destination sends a
    # premature partial ACK and the sender retransmits fragments that are already in flight
    assert a.stats()["retries"] == 0
    assert any(e.startswith("RELAY W1AAA>W2BBB") for e in r.events)


def test_no_relay_when_relay_mode_off():
    a, r, b = Node("W1AAA", seed=1, max_retries=1), Node("W9RLY", relay=False, seed=2), Node("W2BBB", seed=3)
    links = {}
    links.update(link("W1AAA", "W9RLY"))
    links.update(link("W2BBB", "W9RLY"))
    sim = Sim([a, r, b], links, duration_s=100)
    ids = {}
    sim.run(80, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", b"ping", now=s.t)))])
    assert b.messages == []
    assert a.status[ids["id"]] == ST_FAILED


def test_two_relays_suppress_duplicates():
    a, r1, r2, b = (Node("W1AAA", seed=1), Node("W9RL1", relay=True, seed=2),
                    Node("W9RL2", relay=True, seed=3), Node("W2BBB", seed=4))
    links = {}
    for x in ("W9RL1", "W9RL2"):
        links.update(link("W1AAA", x))
        links.update(link("W2BBB", x))
    links.update(link("W9RL1", "W9RL2"))  # relays hear each other -> can suppress
    sim = Sim([a, r1, r2, b], links, duration_s=120)
    ids = {}
    sim.run(100, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", os.urandom(200), now=s.t)))])
    assert a.status[ids["id"]] == ST_DELIVERED
    assert len(b.messages) == 1
    assert r1.stats()["relay_suppressed"] + r2.stats()["relay_suppressed"] > 0


def test_broadcast_is_relayed_and_not_acked():
    a, r, b = Node("W1AAA", seed=1), Node("W9RLY", relay=True, seed=2), Node("W2BBB", seed=3)
    links = {}
    links.update(link("W1AAA", "W9RLY"))
    links.update(link("W2BBB", "W9RLY"))
    sim = Sim([a, r, b], links, duration_s=60)
    ids = {}
    sim.run(40, [(1.0, lambda s: ids.setdefault("id", a.send("CQ", b"CQ CQ de W1AAA", ack=True, now=s.t)))])
    assert b.messages == [("W1AAA", "CQ", MT_TEXT, b"CQ CQ de W1AAA")]
    assert a.status[ids["id"]] == ST_SENT
    assert b.stats()["tx_frames"] <= 1  # at most its ID beacon; never an ACK


def test_station_id_after_traffic_and_under_10_minutes():
    a, r, b = Node("W1AAA", seed=1), Node("W9RLY", relay=True, seed=2), Node("W2BBB", seed=3)
    links = {}
    links.update(link("W1AAA", "W9RLY"))
    links.update(link("W2BBB", "W9RLY"))
    sim = Sim([a, r, b], links, duration_s=800)
    # steady chat: a message every 9 s keeps the channel active (gaps shorter than the idle-ID timer)
    script = [(5 + 9 * i, (lambda s, i=i: a.send("W2BBB", b"msg %d " % i * 4, now=s.t)
                           if i % 2 == 0 else b.send("W1AAA", b"reply %d " % i * 4, now=s.t)))
              for i in range(0, 80)]
    sim.run(760, script)
    for n in (a, b, r):
        tx_times = [s for s, _, _ in n.tx_log]
        assert tx_times, n.name
        assert n.id_times, f"{n.name} never identified"
        marks = sorted(n.id_times) + [tx_times[-1]]
        prev = tx_times[0]
        worst = 0
        for t_id in marks:
            worst = max(worst, t_id - prev)
            prev = t_id
        assert worst < 600, f"{n.name}: {worst:.0f}s without ID"
    # and the ID frame carries the station's own callsign
    ids = [f for _, _, fr in r.tx_log for f in fr if f[0] == MT_ID]
    assert ids and ids[0][3:11].rstrip(b"\0") == b"W9RLY" and ids[0][23:].startswith(b"DE W9RLY")


def test_hostile_callsign_bytes_are_sanitised():
    """A CRC-valid frame with junk in the callsign field must not leak non-ASCII to callers."""
    import struct
    b = Node("W2BBB", seed=3)
    hdr = bytes([MT_TEXT, 0, 0]) + b"\xff\xfe\x80AB\x01\x02\x03" + b"W2BBB\0\0\0" + struct.pack("<HBB", 7, 0, 1)
    frame = hdr + b"hi"
    L.mesh_rx_frame(b.m, frame, len(frame), 1000)
    assert len(b.messages) == 1
    src = b.messages[0][0]
    assert src.isascii() and src.isprintable() and "?" in src


def test_large_fragment_class_is_received_by_a_default_station():
    """Class 1 (145-byte fragments) and class 0 stations interoperate: the class travels in the header."""
    a, b = Node("W1AAA", seed=1, frag_class=1), Node("W2BBB", seed=2)
    sim = Sim([a, b], link("W1AAA", "W2BBB"), duration_s=90)
    msg = os.urandom(1000)           # 7 large fragments (it would be 11 small ones)
    ids = {}
    sim.run(70, [(1.0, lambda s: ids.setdefault("id", a.send("W2BBB", msg, now=s.t)))])
    assert [m[3] for m in b.messages] == [msg]
    assert a.status[ids["id"]] == ST_DELIVERED and a.stats()["retries"] == 0
    sizes = {len(f) for _, _, fr in a.tx_log for f in fr if f[0] == MT_VOICE or f[0] == MT_TEXT}
    assert max(sizes) == 23 + 145


def test_unknown_fragment_class_is_ignored():
    import struct
    b = Node("W2BBB", seed=3)
    for cls in (2, 3):
        hdr = bytes([MT_TEXT, cls << 1, 0]) + b"W1AAA\0\0\0" + b"W2BBB\0\0\0" + struct.pack("<HBB", 9, 0, 1)
        frame = hdr + b"hi"
        L.mesh_rx_frame(b.m, frame, len(frame), 1000 + cls)
    assert b.messages == []
