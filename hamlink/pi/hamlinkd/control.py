# SPDX-License-Identifier: GPL-3.0-or-later
"""Localhost control: one JSON object per line in, one per line out, on 127.0.0.1:<control_port>.

Requests: {"cmd": "status"} | {"cmd": "log", "n": 50} | {"cmd": "messages"} |
          {"cmd": "send", "to": "CQ", "text": "...", "ack": true} | {"cmd": "test_tone", "mode": "alt"}
Replies:  {"ok": true, ...} or {"ok": false, "error": "..."}
Only localhost is served; the web page (phase 2) will sit in front of this with its own login.
"""
import json
import socket
import socketserver
import threading


class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        st = self.server.station
        for raw in self.rfile:
            try:
                req = json.loads(raw.decode("utf-8"))
                rep = dispatch(st, req)
            except Exception as e:
                rep = {"ok": False, "error": str(e)}
            self.wfile.write((json.dumps(rep) + "\n").encode("utf-8"))
            self.wfile.flush()


def _run_on_engine(st, fn, timeout=30):
    ok, val = st.submit(fn).get(timeout=timeout)
    if not ok:
        raise val
    return val


def dispatch(st, req):
    cmd = req.get("cmd")
    if cmd == "status":
        return {"ok": True, "status": st.status()}
    if cmd == "log":
        n = int(req.get("n", 50))
        return {"ok": True, "events": list(st.events)[-n:]}
    if cmd == "messages":
        return {"ok": True, "messages": list(st.messages)}
    if cmd == "send":
        to, text, ack = req.get("to", "CQ"), req.get("text", ""), bool(req.get("ack", True))
        msg_id = _run_on_engine(st, lambda s, now: s.send_text(to, text, ack and to.upper() != "CQ", now))
        return {"ok": True, "id": msg_id}
    if cmd == "test_tone":
        mode = req.get("mode", "alt")
        _run_on_engine(st, lambda s, now: s.start_test_tone(mode, now))
        return {"ok": True}
    return {"ok": False, "error": f"unknown command {cmd!r}"}


class _Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(station, port):
    """Starts the control server on a daemon thread; returns the server (shutdown() to stop)."""
    srv = _Server(("127.0.0.1", port), _Handler)
    srv.station = station
    threading.Thread(target=srv.serve_forever, name="control", daemon=True).start()
    return srv


def request(port, req, timeout=40):
    """Client side: one request to a running hamlinkd."""
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as c:
        c.sendall((json.dumps(req) + "\n").encode("utf-8"))
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = c.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode("utf-8"))
