# SPDX-License-Identifier: GPL-3.0-or-later
"""Reports and the map model through the real modem path (flat C API only)."""
import ctypes as C
import json
import time
import pytest
from test_api import A, N, run

A.hl_set_clock.argtypes = [C.c_void_p, C.c_int64, C.c_int64]
A.hl_schema_count.restype = C.c_int
A.hl_schema_json.restype = C.c_char_p
A.hl_schema_json.argtypes = [C.c_int]
A.hl_report_send.argtypes = [C.c_void_p, C.c_char_p, C.c_char_p, C.c_char_p, C.c_int, C.c_int64, C.c_char_p, C.c_int]
A.hl_report_summary.argtypes = [C.c_char_p, C.c_int, C.c_char_p, C.c_int]
A.hl_report_detail.argtypes = [C.c_char_p, C.c_int, C.c_char_p, C.c_int]
A.hl_map_set_self_position.argtypes = [C.c_void_p, C.c_double, C.c_double, C.c_double, C.c_int64]
A.hl_map_snapshot.argtypes = [C.c_void_p, C.c_int64, C.c_char_p, C.c_int]
A.hl_map_detail.argtypes = [C.c_void_p, C.c_char_p, C.c_int64, C.c_char_p, C.c_int]

T0 = 1793000000  # fixed "wall clock" for tests


def clock(*nodes):
    for n in nodes:
        A.hl_set_clock(n.c, T0, 0)


def snap(n, ms):
    b = C.create_string_buffer(8192)
    A.hl_map_snapshot(n.c, ms, b, len(b))
    rows = [l.split("\t") for l in b.value.decode().splitlines() if l]
    return rows


def send(n, dst, schema, kv, ms, ack=0):
    err = C.create_string_buffer(160)
    rc = A.hl_report_send(n.c, dst.encode(), schema.encode(), kv.encode(), ack, ms, err, len(err))
    return rc, err.value.decode()


LINK = dict(snr_db=22, twist_db=3, ppm=80)
LINKS = {("W1AAA", "W2BBB"): LINK, ("W2BBB", "W1AAA"): LINK}

SAL = ("assess=Hostile\nsize_n=3\nsize_what=vehicles\nactivity=moving\nloc=FN31pr\nobserved=now\n"
       "equip=light vehicles\nheading=N\n")


def test_schemas_exposed_as_json():
    assert A.hl_schema_count() == 2
    docs = [json.loads(A.hl_schema_json(i)) for i in range(2)]
    assert {d["name"] for d in docs} == {"POS", "SALUTE"}
    assert A.hl_schema_json(2) is None


def test_position_and_salute_over_afsk_update_map():
    a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
    clock(a, b)
    out = {}
    script = [
        (1.0, lambda t: out.setdefault("pos", send(a, "CQ", "POS", "loc=FN31pr\ntime=now\nsource=GPS\n", int(t * 1000)))),
        (30.0, lambda t: out.setdefault("sal", send(a, "CQ", "SALUTE", SAL, int(t * 1000)))),
    ]
    run([a, b], LINKS, 80, script)
    assert out["pos"][0] >= 0 and out["sal"][0] >= 0, out
    rows = snap(b, 80_000)
    st = [r for r in rows if r[0] == "S"]
    ct = [r for r in rows if r[0] == "C"]
    assert [r[2] for r in st] == ["W1AAA"]
    assert st[0][5] == "0"                                           # exact GPS position: no uncertainty ring
    assert st[0][6] == "#E53935"                                     # station tinted Hostile red by its SALUTE report
    assert len(ct) == 1 and ct[0][6] == "#E53935" and ct[0][7] == "diamond"
    assert ct[0][11].startswith("Hostile: 3 vehicles")
    assert abs(float(ct[0][3]) - 41.73) < 0.05 and abs(float(ct[0][4]) + 72.71) < 0.05
    # sender sees its own reports too
    mine = snap(a, 80_000)
    assert any(r[0] == "S" and r[12] == "1" for r in mine)           # self flag
    assert any(r[0] == "C" for r in mine)


def test_station_colour_reverts_when_report_expires_and_icon_fades():
    a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
    clock(a, b)
    script = [(1.0, lambda t: send(a, "CQ", "POS", "loc=FN31pr\ntime=now\nsource=GPS\n", int(t * 1000))),
              (25.0, lambda t: send(a, "CQ", "SALUTE", SAL, int(t * 1000)))]
    run([a, b], LINKS, 70, script)
    fresh = snap(b, 70_000)
    assert [r[6] for r in fresh if r[0] == "S"] == ["#E53935"]
    # 45 min later: SALUTE is stale (>30 min) so fading, still tinted; position still full alpha (stale at 30 too)
    s45 = snap(b, 70_000 + 45 * 60_000)
    c = [r for r in s45 if r[0] == "C"][0]
    assert 0.25 < float(c[8]) < 1.0
    # 5 h later: SALUTE (expires 4 h) gone -> station back to its own blue, contact removed
    s5h = snap(b, 70_000 + 5 * 3600_000)
    assert [r for r in s5h if r[0] == "C"] == []
    assert [r[6] for r in s5h if r[0] == "S"] == ["#1E88E5"]
    # 25 h later: position (expires 24 h) gone too
    assert snap(b, 70_000 + 25 * 3600_000) == []


def test_grid_square_position_reports_uncertainty_radius():
    a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
    clock(a, b)
    out = {}
    run([a, b], LINKS, 40, [(1.0, lambda t: out.setdefault("r", send(a, "CQ", "POS", "loc=FN31\ntime=now\nsource=Grid square (4 char)\n", int(t * 1000))))])
    assert out["r"][0] >= 0
    st = [r for r in snap(b, 40_000) if r[0] == "S"][0]
    assert float(st[5]) == 100000.0                                  # radius from the schema's source choice


def test_relay_carries_reports_and_detail_shows_distance():
    a, r, b = N("W1AAA", seed=1), N("W9RLY", relay=1, seed=2), N("W2BBB", seed=3)
    clock(a, r, b)
    links = {("W1AAA", "W9RLY"): LINK, ("W9RLY", "W1AAA"): LINK, ("W2BBB", "W9RLY"): LINK, ("W9RLY", "W2BBB"): LINK}
    A.hl_map_set_self_position(b.c, 41.0, -72.0, 0, 0)
    run([a, r, b], links, 90, [(1.0, lambda t: send(a, "CQ", "SALUTE", SAL, int(t * 1000)))])
    ct = [x for x in snap(b, 90_000) if x[0] == "C"]
    assert len(ct) == 1
    d = C.create_string_buffer(1024)
    assert A.hl_map_detail(b.c, ct[0][1].encode(), 90_000, d, len(d)) > 0
    text = d.value.decode()
    assert "Reported by W1AAA" in text and "Hostile" in text and "From you:" in text and "bearing" in text
    assert A.hl_map_detail(b.c, b"C:999", 90_000, d, len(d)) == 0


def test_errors_are_human_readable_and_nothing_is_sent():
    a = N("W1AAA", seed=1)
    clock(a)
    rc, err = send(a, "CQ", "SALUTE", "assess=Purple\n", 1000)
    assert rc == -100 - 4 and "Purple" in err
    rc, err = send(a, "CQ", "SALUTE", "assess=Hostile\n", 1000)
    assert rc == -100 - 3 and "required" in err
    rc, err = send(a, "CQ", "NOPE", "x=1", 1000)
    assert rc == -1
    assert A.hl_pending_tx(a.c) == 0
    b = N("W2BBB", seed=2)                                           # clock never set -> "now" refused
    rc, err = send(b, "CQ", "POS", "loc=FN31\ntime=now\nsource=GPS\n", 1000)
    assert rc == -104 and "clock" in err


def test_unknown_future_report_type_is_relayed_untouched():
    a, r, b = N("W1AAA", seed=1), N("W9RLY", relay=1, seed=2), N("W2BBB", seed=3)
    links = {("W1AAA", "W9RLY"): LINK, ("W9RLY", "W1AAA"): LINK, ("W2BBB", "W9RLY"): LINK, ("W9RLY", "W2BBB"): LINK}
    # message type 9 does not exist in this build; the relay must forward it and the end station must hand it up
    ids = {}
    from test_api import A as API
    # hl_send rejects unknown types at the sender (by design), so drive the relay with a REPORT whose schema id is unknown
    payload = bytes([77, 1, 0xAB, 0xCD])
    run([a, r, b], links, 60, [(1.0, lambda t: ids.setdefault("i", API.hl_send(a.c, b"CQ", 5, payload, len(payload), 0, int(t * 1000))))])
    assert ids["i"] >= 0
    assert r.stats()["relayed"] >= 1
    assert (("W1AAA", "CQ", 5, payload)) in b.msgs                  # delivered to the app
    assert snap(b, 60_000) == []                                     # but the map ignores what it cannot decode


# ---------------------------------------------------------------- report repeats (API v6), default off
A.hl_set_report_repeat.argtypes = [C.c_void_p, C.c_int, C.c_int]


def reports_from(n, src):
    return [m for m in n.msgs if m[0] == src and m[2] == 5]


def test_repeat_is_off_by_default_and_a_lost_report_stays_lost():
    a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
    clock(a, b)
    links = {}                                                      # B cannot hear A at first
    script = [(1.0, lambda t: send(a, "CQ", "SALUTE", SAL, int(t * 1000))),
              (15.0, lambda t: links.update(LINKS))]                # path opens after the report went out
    run([a, b], links, 90, script)
    assert reports_from(b, "W1AAA") == []
    assert [r for r in snap(b, 90_000) if r[0] == "C"] == []


def test_repeat_delivers_after_the_first_copy_is_lost_and_receiver_sees_it_once():
    a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
    clock(a, b)
    A.hl_set_report_repeat(a.c, 2, 30000)
    links = {}
    air = {}
    script = [(1.0, lambda t: send(a, "CQ", "SALUTE", SAL, int(t * 1000))),
              (15.0, lambda t: links.update(LINKS))]
    run([a, b], links, 120, script, airtime=air)
    assert len(reports_from(b, "W1AAA")) == 1                       # second copy arrived; third dropped as duplicate
    assert len([r for r in snap(b, 120_000) if r[0] == "C"]) == 1
    assert sum("REPEAT" in e for e in a.events) == 2
    assert air["W1AAA"] < 6.0                                       # three single-frame bursts plus ID: a few seconds


def test_repeat_skips_addressed_reports_and_can_be_cancelled():
    a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
    clock(a, b)
    A.hl_set_report_repeat(a.c, 2, 30000)
    script = [(1.0, lambda t: send(a, "W2BBB", "SALUTE", SAL, int(t * 1000), ack=1)),   # ARQ, no repeats
              (2.0, lambda t: send(a, "CQ", "POS", "loc=FN31pr\ntime=now\nsource=GPS\n", int(t * 1000))),
              (10.0, lambda t: A.hl_set_report_repeat(a.c, 0, 30000))]                 # off: pending copies cancelled
    run([a, b], LINKS, 90, script)
    assert not any("REPEAT" in e for e in a.events)
    assert len(reports_from(b, "W1AAA")) == 2


def test_broadcast_queued_during_an_acked_send_waits_for_the_ack():
    # Regression: a broadcast queued while our ACK-requesting burst was on the air used to go out the moment the burst
    # ended, colliding with the other station's ACK (half duplex): the broadcast was lost and the message needed a retry.
    for seed in (3, 4, 5):
        a, b = N("W1AAA", seed=1), N("W2BBB", seed=2)
        clock(a, b)
        script = [(1.0, lambda t: send(a, "W2BBB", "SALUTE", SAL, int(t * 1000), ack=1)),
                  (2.0, lambda t: send(a, "CQ", "POS", "loc=FN31pr\ntime=now\nsource=GPS\n", int(t * 1000)))]
        run([a, b], LINKS, 60, script, seed=seed)
        assert len(reports_from(b, "W1AAA")) == 2, seed
        assert a.stats()["retries"] == 0, seed
