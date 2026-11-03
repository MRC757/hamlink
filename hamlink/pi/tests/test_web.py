# SPDX-License-Identifier: GPL-3.0-or-later
"""The service supervisor and the web page, in real time over HTTP: first-run password, login and its protections,
settings that start the radio, sending, the settings-file watcher, and the Help page."""
import http.client
import json
import os
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from hamlinkd import core, service, settings, tnc, web  # noqa: E402


@pytest.fixture(scope="module")
def lib_path():
    try:
        p = core.find_library()
        core.load(p)
        return p
    except FileNotFoundError as e:
        pytest.skip(str(e))


class Client:
    def __init__(self, port):
        self.port, self.cookie = port, ""

    def req(self, method, path, body=None, header=True):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        h = {"Content-Type": "application/json"}
        if header and method == "POST":
            h["X-HamLink"] = "1"
        if self.cookie:
            h["Cookie"] = self.cookie
        c.request(method, path, json.dumps(body) if body is not None else None, h)
        r = c.getresponse()
        data = r.read()
        sc = r.getheader("Set-Cookie")
        if sc:
            self.cookie = sc.split(";")[0]
        c.close()
        ctype = r.getheader("Content-Type", "")
        return r.status, json.loads(data) if ctype.startswith("application/json") else data.decode()

    def get(self, path):
        return self.req("GET", path)

    def post(self, path, body=None, header=True):
        return self.req("POST", path, body or {}, header)


@pytest.fixture
def relay(tmp_path, lib_path):
    """A service with no settings yet (a fresh SD card), its radio link a simulated TNC, and its web page."""
    cfg = str(tmp_path / "hamlinkd.json")
    peer = {}

    def make_io(c, log):
        mine, other = tnc.LoopTnc.pair()
        peer["tnc"] = other
        return None, None, mine

    svc = service.Service(cfg, lib_path, str(tmp_path), make_io=make_io, sink=lambda e: None)
    site = web.Web(svc, 0, web.auth_path(cfg), host="127.0.0.1")
    th = threading.Thread(target=svc.run, daemon=True)
    th.start()
    yield svc, Client(site.port), peer, cfg
    svc.stop()
    th.join(5)
    site.shutdown()


def wait(cond, s=5.0):
    end = time.monotonic() + s
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


def test_first_run_password_then_configure_and_send(relay):
    svc, c, peer, cfg = relay
    assert c.get("/api/session")[1] == {"logged_in": False, "password_set": False, "callsign": ""}
    assert c.get("/api/status")[0] == 401
    assert c.post("/api/setup-password", {"password": "short"})[0] == 400
    assert c.post("/api/setup-password", {"password": "hilltop-relay"})[1]["ok"]
    assert c.get("/api/session")[1]["logged_in"]
    assert c.get("/api/status")[1]["status"]["state"] == "not configured"

    code, rep = c.post("/api/settings", {"values": {"callsign": "n0rly", "link": "nonsense"}})
    assert code == 400 and rep["problems"] == ["link must be audio or tnc"]
    code, rep = c.post("/api/settings", {"values": {"callsign": "n0rly", "link": "tnc", "relay": True,
                                                    "tnc_port": "bt:38:D2:00:01:02:03"}})
    assert rep == {"ok": True, "problems": []}
    assert settings.load(cfg).callsign == "N0RLY"
    assert wait(lambda: c.get("/api/status")[1]["status"].get("state") == "running")

    code, rep = c.post("/api/send", {"to": "cq", "text": "hello from the hill"})
    assert rep["ok"], rep
    assert wait(lambda: len(peer["tnc"].inbox) > 0)  # it went out to the radio
    assert any("hello from the hill" in e["text"] for e in c.get("/api/log?since=0")[1]["events"])

    vals = c.get("/api/settings")[1]
    assert vals["values"]["callsign"] == "N0RLY" and vals["problems"] == []
    assert {f["name"] for f in vals["form"]} >= {"callsign", "link", "modem", "tx_level"}


def test_login_protections(relay):
    svc, c, peer, cfg = relay
    c.post("/api/setup-password", {"password": "hilltop-relay"})
    other = Client(c.port)
    assert other.post("/api/setup-password", {"password": "taken-over!"})[0] == 403  # only on first visit
    assert c.post("/api/send", {"text": "x"}, header=False)[0] == 403  # cross-site form posts are refused
    assert other.post("/api/login", {"password": "wrong-password"})[0] == 403
    for _ in range(4):
        other.post("/api/login", {"password": "wrong-password"})
    code, rep = other.post("/api/login", {"password": "hilltop-relay"})
    assert code == 429  # locked for a while after repeated failures, even with the right password
    assert other.get("/api/status")[0] == 401


def test_password_change_and_logout(relay, monkeypatch):
    svc, c, peer, cfg = relay
    c.post("/api/setup-password", {"password": "hilltop-relay"})
    assert c.post("/api/password", {"old": "nope", "new": "another-one"})[0] == 403
    assert c.post("/api/password", {"old": "hilltop-relay", "new": "another-one"})[1]["ok"]
    assert c.post("/api/logout")[1]["ok"]
    assert c.get("/api/status")[0] == 401
    assert c.post("/api/login", {"password": "another-one"})[1]["ok"]
    with open(web.auth_path(cfg)) as f:
        assert "another-one" not in f.read()


def test_settings_file_edit_restarts_the_radio(relay):
    svc, c, peer, cfg = relay
    settings.save(settings.Settings(callsign="N0AAA", link="tnc"), cfg)
    assert wait(lambda: svc.status().get("callsign") == "N0AAA" and svc.status()["state"] == "running")
    time.sleep(0.01)
    settings.save(settings.Settings(callsign="N0BBB", link="tnc"), cfg)
    assert wait(lambda: svc.status().get("callsign") == "N0BBB" and svc.status()["state"] == "running", 10)


def test_radio_failure_is_reported_and_retried(tmp_path, lib_path):
    cfg = str(tmp_path / "hamlinkd.json")
    settings.save(settings.Settings(callsign="N0AAA", link="tnc"), cfg)
    tries = []

    def make_io(c, log):
        tries.append(1)
        raise RuntimeError("no such sound card")

    svc = service.Service(cfg, lib_path, make_io=make_io, sink=lambda e: None)
    th = threading.Thread(target=svc.run, daemon=True)
    th.start()
    try:
        assert wait(lambda: svc.status().get("error") == "no such sound card")
        assert svc.status()["state"] == "stopped"
        with pytest.raises(RuntimeError, match="not running"):
            svc.call(lambda s, now: None)
    finally:
        svc.stop()
        th.join(5)
    assert len(tries) == 1


def test_help_page_renders_the_help_file(relay):
    svc, c, peer, cfg = relay
    c.post("/api/setup-password", {"password": "hilltop-relay"})
    code, page = c.get("/help")
    assert code == 200 and "<h1>HamLink help</h1>" in page and "<li>" in page and "<b>" in page


def test_markdown_subset_escapes_html():
    out = web.markdown("# T\n\nA <script> **b** `c`\n\n- one\n  - two\n- three\n\n```\nx < y\n```")
    assert "<script>" not in out and "&lt;script&gt;" in out
    assert "<h1>T</h1>" in out and "<b>b</b>" in out and "<code>c</code>" in out
    assert out.count("<ul>") == 2 and "x &lt; y" in out


def test_every_module_imports_and_the_cli_answers(capsys):
    import importlib
    import pkgutil
    import hamlinkd
    for m in pkgutil.iter_modules(hamlinkd.__path__):
        if m.name != "__main__":
            importlib.import_module("hamlinkd." + m.name)
    from hamlinkd import cli
    with pytest.raises(SystemExit):
        cli.main(["--help"])
    assert "--set-password" in capsys.readouterr().out
