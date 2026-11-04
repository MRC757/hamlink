# SPDX-License-Identifier: GPL-3.0-or-later
"""API v11: monitor mode, listening on all modems, and relaying on the modem a frame arrived on
(docs/STATIONS_PAGE.md, section 5). Driven only through the flat hl_* API."""
import ctypes as C
import numpy as np
from hamlink import FS
from test_api import A, N, run
from test_tnc import kiss_frames, kiss_wrap, poll, tnode

MON = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.c_int, C.POINTER(C.c_ubyte), C.c_int)
A.hl_set_monitor.argtypes = [C.c_void_p, C.c_int]
A.hl_set_monitor_callback.argtypes = [C.c_void_p, MON, C.c_void_p]
A.hl_set_listen_all.argtypes = [C.c_void_p, C.c_int]
A.hl_rx_modem.argtypes = [C.c_void_p]
A.hl_channel_busy.argtypes = [C.c_void_p]
A.hl_tnc_rx.argtypes = [C.c_void_p, C.c_char_p, C.c_int, C.c_int64]

AFSK, OFDM, MT63, PSK2400, PSK3600 = 0, 1, 2, 3, 4
TEXT, VOICE, ACK, ID = 1, 2, 3, 4


def monitor(node):
    """Turns monitor mode on; overheard traffic lands in node.heard as (src, dst, type, msg_id, data)."""
    node.heard = []
    node.mon_cb = MON(lambda u, s, d, t, i, p, n: node.heard.append((s.decode(), d.decode(), t, i, bytes(p[:n]))))
    A.hl_set_monitor_callback(node.c, node.mon_cb, None)
    A.hl_set_monitor(node.c, 1)
    return node


def links(pairs, snr=25):
    d = dict(snr_db=snr, twist_db=3, ppm=50)
    out = {}
    for a, b in pairs:
        out[(a, b)] = dict(d)
        out[(b, a)] = dict(d, ppm=-50)
    return out


# ---------------------------------------------------------------- monitor mode

def test_monitor_overhears_a_conversation_and_its_ack_without_transmitting():
    a, b, c = N("W1AAA", seed=1), N("W2BBB", seed=2), monitor(N("W3CCC", seed=3))
    msg = b"meet at the north gate at 1400"
    ids, air = {}, {}
    run([a, b, c], links([("W1AAA", "W2BBB"), ("W1AAA", "W3CCC"), ("W2BBB", "W3CCC")]), 30,
        [(1.0, lambda t: ids.setdefault("id", A.hl_send(a.c, b"W2BBB", TEXT, msg, len(msg), 1, int(t * 1000))))],
        airtime=air)
    assert a.status.get(ids["id"]) == 1 and [m[3] for m in b.msgs] == [msg]
    data = [h for h in c.heard if h[2] == TEXT]
    assert data == [("W1AAA", "W2BBB", TEXT, ids["id"], msg)]        # once, with the message ID
    acks = [h for h in c.heard if h[2] == ACK]
    assert acks and acks[0][:4] == ("W2BBB", "W1AAA", ACK, ids["id"])  # B's ACK, for that message
    assert c.msgs == [] and "W3CCC" not in air                          # never delivered to C, C never transmitted


def test_monitor_reassembles_a_multipart_message():
    a, b, c = N("W1AAA", seed=1), N("W2BBB", seed=2), monitor(N("W3CCC", seed=3))
    clip = bytes(np.random.default_rng(4).integers(0, 256, 400, dtype=np.uint8))  # several fragments
    run([a, b, c], links([("W1AAA", "W2BBB"), ("W1AAA", "W3CCC"), ("W2BBB", "W3CCC")]), 40,
        [(1.0, lambda t: A.hl_send(a.c, b"W2BBB", VOICE, clip, len(clip), 1, int(t * 1000)))])
    assert [(h[0], h[1], h[4]) for h in c.heard if h[2] == VOICE] == [("W1AAA", "W2BBB", clip)]


def test_monitor_is_off_by_default():
    a, b, c = N("W1AAA", seed=1), N("W2BBB", seed=2), N("W3CCC", seed=3)
    c.heard = []
    c.mon_cb = MON(lambda u, s, d, t, i, p, n: c.heard.append(s))
    A.hl_set_monitor_callback(c.c, c.mon_cb, None)          # a callback alone does not turn it on
    msg = b"private to B"
    run([a, b, c], links([("W1AAA", "W2BBB"), ("W1AAA", "W3CCC")]), 20,
        [(1.0, lambda t: A.hl_send(a.c, b"W2BBB", TEXT, msg, len(msg), 1, int(t * 1000)))])
    assert [m[3] for m in b.msgs] == [msg] and c.heard == [] and c.msgs == []


def test_overheard_traffic_cannot_push_out_a_message_to_us():
    """Nine overheard messages stay incomplete (their last part is lost); a message to C still arrives whole."""
    c = monitor(tnode("W3CCC", seed=3))
    t = 1000
    for i in range(9):
        s = tnode(f"W{i}SRC", seed=10 + i)
        A.hl_send(s.c, f"W{i}DST".encode(), TEXT, bytes([65 + i]) * 300, 300, 1, t)
        out = b""
        for k in range(200):
            out += poll(s, t + 100 * k)
        frames = kiss_frames(out)
        assert len(frames) >= 3
        for f in frames[:-1]:                                  # drop the last fragment
            w = kiss_wrap(f)
            A.hl_tnc_rx(c.c, w, len(w), t)
        t += 100
    y = tnode("W9YYY", seed=30)
    msg = b"for C: " + b"z" * 250
    A.hl_send(y.c, b"W3CCC", TEXT, msg, len(msg), 1, t)
    out = b""
    for k in range(200):
        out += poll(y, t + 100 * k)
    A.hl_tnc_rx(c.c, out, len(out), t + 1000)
    assert [m[3] for m in c.msgs] == [msg]
    assert all(h[2] != TEXT for h in c.heard)                  # the overheard ones never completed


# ---------------------------------------------------------------- listen on all modems

def burst(modem, call, text, seed):
    """One transmission (48 kHz int16) from a fresh station on this modem."""
    n = N(call, seed=seed, modem=modem)
    assert A.hl_send(n.c, b"CQ", TEXT, text, len(text), 0, 1000) >= 0
    for k in range(1, 500):
        A.hl_process_audio(n.c, None, 0, 1000 + 20 * k, 0.004)
        cnt = C.c_int()
        p = A.hl_poll_tx(n.c, 1000 + 20 * k, 0.5, C.byref(cnt))
        if p and cnt.value:
            return np.ctypeslib.as_array(p, shape=(cnt.value,)).copy()
    raise AssertionError("no burst")


def receive(node, signal, rng):
    """Feeds audio with a little noise; returns the fraction of the signal's blocks during which the channel was busy."""
    x = np.concatenate([np.zeros(FS // 2), signal.astype(float), np.zeros(3 * FS)])
    x = np.clip(x + rng.normal(0, 30, len(x)), -32768, 32767).astype(np.int16)
    busy = []
    for i in range(0, len(x) - 960, 960):
        seg = np.ascontiguousarray(x[i:i + 960])
        A.hl_process_audio(node.c, seg.ctypes.data, 960, 50_000 + i // 48, 0.004)
        if FS // 2 <= i < FS // 2 + len(signal):
            busy.append(A.hl_channel_busy(node.c))
    return float(np.mean(busy))


def listener(listen_all):
    d = N("W4DDD", seed=7)                                     # AFSK selected
    if listen_all:
        A.hl_set_listen_all(d.c, 1)
    return d


def test_listen_on_all_modems_hears_every_modem_and_says_which():
    rng = np.random.default_rng(5)
    senders = [(PSK2400, "W5PSK"), (PSK3600, "W6PSK"), (OFDM, "W7OFD"), (MT63, "W8MT6")]
    d = listener(True)
    got = []
    for i, (modem, call) in enumerate(senders):
        text = f"hello from {call}".encode()
        before = len(d.msgs)
        busy = receive(d, burst(modem, call, text, 20 + i), rng)
        new = d.msgs[before:]
        assert [m[0] for m in new] == [call] and new[0][3] == text, (modem, d.msgs)
        assert busy > 0.5, (modem, busy)                         # carrier sense counted this modem's receiver
        got.append(modem)
    assert got == [m for m, _ in senders]


def test_listen_on_all_modems_is_off_by_default():
    rng = np.random.default_rng(6)
    d = listener(False)
    for i, (modem, call) in enumerate([(PSK2400, "W5PSK"), (OFDM, "W7OFD"), (MT63, "W8MT6")]):
        receive(d, burst(modem, call, b"not for an AFSK-only receiver", 40 + i), rng)
    assert d.msgs == []


def test_rx_modem_reports_the_decoding_modem_inside_the_callback():
    rng = np.random.default_rng(8)
    d = N("W4DDD", seed=7)
    A.hl_set_listen_all(d.c, 1)
    seen = []
    MSG = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.POINTER(C.c_ubyte), C.c_int)
    # replace the context with one whose message callback asks hl_rx_modem
    A.hl_destroy(d.c)
    holder = {}
    d.cb = (MSG(lambda u, s, ds, t, p, n: seen.append((s.decode(), A.hl_rx_modem(holder["c"])))), d.cb[1], d.cb[2])
    d.c = A.hl_create(b"W4DDD", 0, 1, 500, 7, *d.cb, None)
    holder["c"] = d.c
    A.hl_set_listen_all(d.c, 1)
    for i, (modem, call) in enumerate([(AFSK, "W1AFS"), (PSK3600, "W6PSK"), (MT63, "W8MT6")]):
        receive(d, burst(modem, call, b"which modem?", 60 + i), rng)
    assert seen == [("W1AFS", AFSK), ("W6PSK", PSK3600), ("W8MT6", MT63)]
    assert A.hl_rx_modem(d.c) == -1                              # outside a callback


# ---------------------------------------------------------------- relaying on the arrival modem

def test_relay_resends_on_the_modem_the_message_arrived_on():
    """A and B use PSK 2400 and cannot hear each other; the relay is set to MT63 but listens on all modems."""
    a, b = N("W1AAA", seed=1, modem=PSK2400), N("W2BBB", seed=2, modem=PSK2400)
    r = N("W9RLY", relay=1, seed=3, modem=MT63)
    A.hl_set_listen_all(r.c, 1)
    msg = b"through the hilltop on PSK"
    ids = {}
    run([a, r, b], links([("W1AAA", "W9RLY"), ("W2BBB", "W9RLY")]), 60,
        [(1.0, lambda t: ids.setdefault("id", A.hl_send(a.c, b"W2BBB", TEXT, msg, len(msg), 1, int(t * 1000))))])
    assert [m[3] for m in b.msgs] == [msg]                      # B only decodes PSK: the relay used PSK
    assert a.status.get(ids["id"]) == 1                          # and B's ACK came back the same way
    assert r.stats()["relayed"] >= 2
