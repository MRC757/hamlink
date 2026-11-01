# SPDX-License-Identifier: GPL-3.0-or-later
"""Bluetooth/serial KISS TNC link mode, driven only through the flat hl_* API with a simulated byte link."""
import ctypes as C
import os
import numpy as np
from test_api import A, N

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD


def poll(node, now_ms):
    n = C.c_int()
    p = A.hl_tnc_poll_tx(node.c, int(now_ms), C.byref(n))
    return bytes(p[:n.value]) if p and n.value else b""


def kiss_frames(stream):
    """Independent KISS parser used to inspect what the app writes to the radio."""
    out, cur, esc, inside = [], bytearray(), False, False
    for b in stream:
        if b == FEND:
            if inside and cur: out.append(bytes(cur))
            cur, inside, esc = bytearray(), True, False
        elif inside:
            if esc:
                cur.append(FEND if b == TFEND else FESC if b == TFESC else b); esc = False
            elif b == FESC: esc = True
            else: cur.append(b)
    return out


def kiss_wrap(payload, cmd=0):
    out = bytearray([FEND, cmd])
    for b in payload:
        out += bytes([FESC, TFEND]) if b == FEND else bytes([FESC, TFESC]) if b == FESC else bytes([b])
    out.append(FEND)
    return bytes(out)


def ax25_ui(dst, src, info, pid=0xF0, ctrl=0x03):
    def addr(c, last):
        c = c.ljust(6)[:6]
        return bytes((ord(ch) << 1) for ch in c) + bytes([0x60 | (1 if last else 0)])
    return addr(dst, False) + addr(src, True) + bytes([ctrl, pid]) + info


def tnode(name, relay=0, seed=1, baud=1200, ttl=1):
    n = N(name, relay=relay, seed=seed, ttl=ttl)
    A.hl_set_link(n.c, 1, baud)
    n.tx_until = 0.0
    n.inbox = []        # [ready_time, bytes] pending deliveries
    n.wire_log = b""    # everything the app wrote to the "radio"
    return n


def run_tnc(nodes, adj, seconds, script=(), baud=1200, rng_seed=9, drop=0.0, extra=None):
    rng = np.random.default_rng(rng_seed)
    by = {n.name: n for n in nodes}
    script = sorted(script, key=lambda e: e[0]); si = 0
    t, dt = 0.0, 0.02
    while t < seconds:
        while si < len(script) and script[si][0] <= t:
            script[si][1](t); si += 1
        now = int(t * 1000)
        for n in nodes:
            # bytes arriving from the radio, in small random chunks (Bluetooth packetisation)
            chunk = b""
            while n.inbox and n.inbox[0][0] <= t and len(chunk) < 13:
                k = int(rng.integers(1, 14 - len(chunk)))
                take, n.inbox[0][1] = n.inbox[0][1][:k], n.inbox[0][1][k:]
                chunk += take
                if not n.inbox[0][1]: n.inbox.pop(0)
            A.hl_tnc_rx(n.c, chunk, len(chunk), now)
            out = poll(n, now)
            if out:
                n.wire_log += out
                nframes = len(kiss_frames(out))
                air = (len(out) - 2 * nframes) * 8 * 1.03 / baud + 0.35 + nframes * 0.05
                n.tx_until = t + air
                for peer in adj.get(n.name, ()):
                    if rng.random() >= drop:
                        by[peer].inbox.append([t + air, bytearray(out)]); 
                        by[peer].inbox[-1][1] = bytes(by[peer].inbox[-1][1])
        if extra: extra(t, by)
        t += dt


def test_direct_text_with_ack_over_tnc():
    a, b = tnode("W1AAA", seed=1), tnode("W2BBB", seed=2)
    msg = b"hello over the radio's own TNC " * 5
    ids = {}
    run_tnc([a, b], {"W1AAA": ["W2BBB"], "W2BBB": ["W1AAA"]}, 40,
            [(1.0, lambda t: ids.setdefault("id", A.hl_send(a.c, b"W2BBB", 1, msg, len(msg), 1, int(t * 1000))))])
    assert [m[3] for m in b.msgs] == [msg]
    assert a.status[ids["id"]] == 1
    assert a.stats()["retries"] == 0


def test_wire_format_is_valid_kiss_wrapping_ax25_ui():
    a, b = tnode("W1AAA/P", seed=1), tnode("W2BBB", seed=2)
    run_tnc([a, b], {"W1AAA/P": ["W2BBB"]}, 10, [(1.0, lambda t: A.hl_send(a.c, b"CQ", 1, b"test", 4, 0, int(t * 1000)))])
    frames = kiss_frames(a.wire_log)
    assert frames and all(f[0] == 0x00 for f in frames)             # KISS data frames, port 0
    ax = frames[0][1:]
    assert bytes(c >> 1 for c in ax[:6]) == b"HAMLNK"                # destination
    assert bytes(c >> 1 for c in ax[7:13]) == b"W1AAA "             # source: first 6 chars of the callsign
    assert ax[13] & 1 and ax[14:16] == bytes([0x03, 0xF0])           # end-of-address, UI, no layer 3
    assert ax[16] == 1 and ax[19:27].rstrip(b"\0") == b"W1AAA/P"     # our mesh frame follows (type TEXT, full callsign)
    assert b.msgs[0][3] == b"test" and b.msgs[0][0] == "W1AAA/P"


def test_hidden_node_relay_voice_clip_over_tnc():
    a, r, b = tnode("W1AAA", seed=1), tnode("W9RLY", relay=1, seed=2), tnode("W2BBB", seed=3)
    adj = {"W1AAA": ["W9RLY"], "W9RLY": ["W1AAA", "W2BBB"], "W2BBB": ["W9RLY"]}
    clip = os.urandom(500)
    ids = {}
    run_tnc([a, r, b], adj, 120, [(1.0, lambda t: ids.setdefault("id", A.hl_send(a.c, b"W2BBB", 2, clip, len(clip), 1, int(t * 1000))))])
    assert [m[3] for m in b.msgs] == [clip]
    assert a.status[ids["id"]] == 1
    assert a.stats()["retries"] == 0


def test_kiss_escaping_and_foreign_traffic():
    """Payloads full of FEND/FESC bytes survive; APRS beacons, non-data KISS frames and garbage are ignored."""
    a, b = tnode("W1AAA", seed=1), tnode("W2BBB", seed=2)
    nasty = bytes([FEND, FESC, TFEND, TFESC, 0, FEND, FEND, FESC] * 12)
    junk = [
        kiss_wrap(ax25_ui("APRS", "N0CALL", b"!4903.50N/07201.75W-test")),   # normal APRS beacon
        kiss_wrap(b"\x01\x20", cmd=0x01),                                    # KISS TXDELAY parameter frame
        bytes([0x55, 0xAA, FEND, FEND, FEND]),                               # line noise and empty frames
        kiss_wrap(ax25_ui("HAMLNK", "N0CALL", b"\x01short")),                # ours by address, but not a valid mesh frame
        kiss_wrap(ax25_ui("HAMLNK", "N0CALL", b"x" * 40, pid=0xCF)),          # wrong PID
    ]
    def inject(t, by):
        if abs(t - 0.5) < 0.01:
            for j in junk: by["W2BBB"].inbox.append([t, j])
    run_tnc([a, b], {"W1AAA": ["W2BBB"], "W2BBB": ["W1AAA"]}, 30,
            [(1.0, lambda t: A.hl_send(a.c, b"W2BBB", 1, nasty, len(nasty), 1, int(t * 1000)))], extra=inject)
    assert [m[3] for m in b.msgs] == [nasty]


def test_tnc_output_is_paced_by_estimated_airtime():
    a = tnode("W1AAA", seed=1)
    big = os.urandom(900)
    A.hl_send(a.c, b"W2BBB", 1, big, len(big), 1, 0)
    first = poll(a, 0)
    assert first
    assert poll(a, 100) == b""              # radio is still busy sending the first burst
    assert poll(a, 2000) == b""
    assert poll(a, 120_000) != b"" or a.stats()["tx"] > 0


def test_station_id_after_tnc_traffic():
    a, b = tnode("W1AAA", seed=1), tnode("W2BBB", seed=2)
    run_tnc([a, b], {"W1AAA": ["W2BBB"], "W2BBB": ["W1AAA"]}, 45,
            [(1.0, lambda t: A.hl_send(a.c, b"W2BBB", 1, b"ping", 4, 1, int(t * 1000)))])
    types = [f[1 + 16] for f in kiss_frames(a.wire_log)]    # mesh type byte after the AX.25 header
    assert 4 in types                                       # an ID frame went out once traffic stopped
