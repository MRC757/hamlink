# SPDX-License-Identifier: GPL-3.0-or-later
"""hamlinkd against the real core library, in simulated time: stations share an in-process radio channel (Air) or a
pair of in-memory TNCs. Needs the core library (build/core/libhamlink_api.so, or windows/native/hamlink_core.dll)."""
import json
import os
import sys

import numpy as np
import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from hamlinkd import audio, control, core, ptt, settings, station, tnc  # noqa: E402


@pytest.fixture(scope="module")
def lib():
    try:
        return core.load(core.find_library())
    except FileNotFoundError as e:
        pytest.skip(str(e))


class FakePtt(ptt.NoPtt):
    def __init__(self):
        self.keyed, self.keys = False, 0

    def key(self, on):
        self.keys += 1 if on and not self.keyed else 0
        self.keyed = on


def make(lib, air, call, seed, **kw):
    s = settings.Settings(callsign=call, **kw)
    assert s.problems() == []
    lines = []
    st = station.Station(s, lib, audio=air.station(call), ptt=FakePtt(), log=lambda e: lines.append(e["text"]), seed=seed)
    st.lines = lines
    return st


def run(air, stations, seconds, until=None):
    for _ in range(int(seconds * 50)):
        for st in stations:
            st.step(air.t_ms)
        air.step()
        if until and until():
            return True
    return False


def idle(stations):
    return all(st.tx is None and st.core.pending_tx() == 0 and not st.ptt.keyed for st in stations)


def delivered(st, msg_id):
    return any(f"message {msg_id} delivered" in e["text"] for e in st.events)


@pytest.mark.parametrize("modem", ["afsk", "psk2400"])
def test_relay_carries_a_message_between_stations_out_of_range(lib, modem):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False, modem=modem)
    r = make(lib, air, "N0RLY", 22, relay=True, modem=modem)
    b = make(lib, air, "N0BBB", 33, relay=False, modem=modem)
    air.link("N0AAA", "N0RLY")
    air.link("N0RLY", "N0BBB")  # A and B cannot hear each other
    msg_id = a.send_text("N0BBB", "hello through the hilltop", True, air.t_ms)
    assert run(air, [a, r, b], 60, until=lambda: delivered(a, msg_id)), "\n".join(a.lines + r.lines + b.lines)
    assert [m["text"] for m in b.messages] == ["hello through the hilltop"]
    assert r.core.stats()["relayed"] >= 1
    assert run(air, [a, r, b], 30, until=lambda: idle([a, r, b]))  # the relayed ACK, then quiet
    for st in (a, r, b):
        st.close()


def test_no_relay_no_delivery(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False)
    r = make(lib, air, "N0RLY", 22, relay=False)
    b = make(lib, air, "N0BBB", 33, relay=False)
    air.link("N0AAA", "N0RLY")
    air.link("N0RLY", "N0BBB")
    a.send_text("N0BBB", "nobody relays this", True, air.t_ms)
    run(air, [a, r, b], 20)
    assert not b.messages
    assert [m["text"] for m in r.messages] == []  # addressed to B: R does not show it either


def test_hourly_transmit_limit_holds_the_queue(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False, max_tx_percent=0.01)  # 0.36 s an hour
    b = make(lib, air, "N0BBB", 33, relay=False)
    air.link("N0AAA", "N0BBB")
    for i in range(3):
        a.send_text("CQ", f"broadcast {i}", False, air.t_ms)
    run(air, [a, b], 20)
    assert a.ptt.keys == 1  # the first burst used up the hour
    assert any("transmit limit reached" in line for line in a.lines)
    assert a.core.pending_tx() > 0


def test_overlong_transmission_is_dropped(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False, max_tx_seconds=0.2)
    air.link("N0AAA", "N0AAA")
    a.send_text("CQ", "x" * 200, False, air.t_ms)
    run(air, [a], 5)
    assert a.ptt.keys == 0
    assert any("dropped a" in line for line in a.lines)


def test_dead_audio_input_blocks_transmit(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False)
    a.audio.alive = lambda now=None: False
    a.send_text("CQ", "should wait", False, air.t_ms)
    run(air, [a], 5)
    assert a.ptt.keys == 0
    assert any("audio input has stopped" in line for line in a.lines)


def test_test_tone_keys_and_releases(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False)
    a.start_test_tone("alt", air.t_ms)
    assert a.ptt.keyed
    run(air, [a], station.TEST_TONE_S + 1)
    assert any("test tone ended" in line for line in a.lines)
    assert run(air, [a], 10, until=lambda: idle([a]))  # then the station ID
    assert a.ptt.keys == 2


def test_tnc_link_delivers_with_ack(lib):
    ta, tb = tnc.LoopTnc.pair()
    sa = settings.Settings(callsign="N0AAA", link="tnc", relay=False)
    sb = settings.Settings(callsign="N0BBB", link="tnc", relay=False)
    a = station.Station(sa, lib, tnc=ta, log=lambda l: None, seed=1)
    b = station.Station(sb, lib, tnc=tb, log=lambda l: None, seed=2)
    t = 1000
    msg_id = a.send_text("N0BBB", "via the radio's TNC", True, t)
    for _ in range(50 * 30):
        t += 20
        a.step(t)
        b.step(t)
        if delivered(a, msg_id):
            break
    assert delivered(a, msg_id)
    assert [m["text"] for m in b.messages] == ["via the radio's TNC"]


def test_station_runs_commands_on_its_own_thread(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False)
    a.step(air.t_ms)
    assert a.status()["callsign"] == "N0AAA"
    q = a.submit(lambda s, now: s.send_text("CQ", "from the web page", False, now))
    a.step(air.t_ms)
    ok, msg_id = q.get_nowait()
    assert ok and msg_id >= 0
    assert any("from the web page" in e["text"] for e in a.events)


def test_settings_round_trip_and_validation(tmp_path):
    p = str(tmp_path / "hamlinkd.json")
    s = settings.Settings(callsign="n0aaa", modem="mt63")
    settings.save(s, p)
    with open(p, encoding="utf-8") as f:
        raw = json.load(f)
    raw["unknown_future_key"] = 1
    with open(p, "w", encoding="utf-8") as f:
        json.dump(raw, f)
    t = settings.load(p)
    assert t.callsign == "N0AAA" and t.modem == "mt63" and t.problems() == []
    assert settings.Settings(callsign="").problems()
    assert settings.Settings(callsign="N0AAA", ptt="vox").problems()


def test_tx_budget_rolls_over():
    b = station.TxBudget(180, 25)
    b.add(0, 900)
    assert np.isclose(b.used_s(1000), 900)
    assert not b.allows_more(1000)
    assert b.allows_more(3_600_001)


class BlockingLoopback:
    """Stands in for a sound card in real time: silence in, play() blocks for the burst's length."""
    blocking = True
    description = "test sound card"

    def __init__(self):
        self.played = []

    def start(self):
        pass

    def stop(self):
        pass

    def read(self, timeout):
        return None

    def flush(self):
        pass

    def alive(self, now=None):
        return True

    def play(self, pcm):
        import time
        self.played.append(len(pcm))
        time.sleep(len(pcm) / audio.FS)

    def transmitting(self):
        return False


def test_real_time_service_with_tcp_control(lib, tmp_path):
    import threading
    from hamlinkd import service
    cfg = str(tmp_path / "hamlinkd.json")
    settings.save(settings.Settings(callsign="N0AAA", relay=False, control_port=18737), cfg)
    snd, key = BlockingLoopback(), FakePtt()
    svc = service.Service(cfg, make_io=lambda c, log: (snd, key, None), sink=lambda e: None)
    srv = control.serve(svc, 18737)
    th = threading.Thread(target=svc.run, daemon=True)
    th.start()
    try:
        for _ in range(100):
            if svc.status().get("state") == "running":
                break
            threading.Event().wait(0.05)
        rep = control.request(18737, {"cmd": "send", "to": "CQ", "text": "real time"})
        assert rep["ok"] and rep["id"] >= 0
        for _ in range(200):
            if snd.played and not key.keyed:
                break
            threading.Event().wait(0.05)
        assert snd.played and key.keys == 1 and not key.keyed
        stat = control.request(18737, {"cmd": "status"})["status"]
        assert stat["callsign"] == "N0AAA" and stat["ptt"] == "none (VOX)" and stat["state"] == "running"
    finally:
        svc.stop()
        th.join(5)
        srv.shutdown()
    assert not th.is_alive()


def test_monitor_fills_the_traffic_list_with_delivery(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False)
    b = make(lib, air, "N0BBB", 33, relay=False)
    m = make(lib, air, "N0MON", 22, relay=False, monitor=True)
    for x, y in (("N0AAA", "N0BBB"), ("N0AAA", "N0MON"), ("N0BBB", "N0MON")):
        air.link(x, y)
    msg_id = a.send_text("N0BBB", "overheard on the hill", True, air.t_ms)
    assert run(air, [a, b, m], 40, until=lambda: delivered(a, msg_id))
    run(air, [a, b, m], 3)
    assert [(e["src"], e["dst"], e["text"]) for e in m.traffic] == [("N0AAA", "N0BBB", "overheard on the hill")]
    assert m.traffic[0]["status"] == "delivered (ACK from N0BBB)"
    assert m.ptt.keys == 0 and not m.messages          # listened only


def test_listen_all_relay_passes_another_modem_on_that_modem(lib):
    air = audio.Air()
    a = make(lib, air, "N0AAA", 11, relay=False, modem="psk2400")
    b = make(lib, air, "N0BBB", 33, relay=False, modem="psk2400")
    r = make(lib, air, "N0RLY", 22, relay=True, modem="mt63", listen_all=True)
    air.link("N0AAA", "N0RLY")
    air.link("N0RLY", "N0BBB")
    msg_id = a.send_text("N0BBB", "PSK through an MT63 relay", True, air.t_ms)
    assert run(air, [a, r, b], 60, until=lambda: delivered(a, msg_id)), "\n".join(r.lines)
    assert [m["text"] for m in b.messages] == ["PSK through an MT63 relay"]
