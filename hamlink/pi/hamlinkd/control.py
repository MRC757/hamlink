# SPDX-License-Identifier: GPL-3.0-or-later
"""Localhost control: one JSON object per line in, one per line out, on 127.0.0.1:<control_port>.

Requests: {"cmd": "status"} | {"cmd": "log", "n": 50, "since": 0} | {"cmd": "messages"} | {"cmd": "restart"} |
          {"cmd": "send", "to": "CQ", "text": "...", "ack": true} | {"cmd": "test_tone", "mode": "alt"}
Replies:  {"ok": true, ...} or {"ok": false, "error": "..."}
Only localhost is served. The web page (web.py) calls dispatch() directly, behind its own login.
"""
import json
import socket
import socketserver
import threading


class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        svc = self.server.service
        for raw in self.rfile:
            try:
                req = json.loads(raw.decode("utf-8"))
                rep = dispatch(svc, req)
            except Exception as e:
                rep = {"ok": False, "error": str(e)}
            self.wfile.write((json.dumps(rep) + "\n").encode("utf-8"))
            self.wfile.flush()


def dispatch(svc, req):
    """One request against a service.Service (shared by the control port and the web page)."""
    cmd = req.get("cmd")
    if cmd == "status":
        return {"ok": True, "status": svc.status()}
    if cmd == "log":
        return {"ok": True, "events": svc.log_since(int(req.get("since", 0)), int(req.get("n", 200)))}
    if cmd == "messages":
        st = svc.station
        return {"ok": True, "messages": list(st.messages) if st else []}
    if cmd == "traffic":
        st = svc.station
        return {"ok": True, "traffic": list(st.traffic) if st else []}
    if cmd == "send":
        to = (req.get("to") or "CQ").strip().upper()
        text, ack = req.get("text", ""), bool(req.get("ack", True))
        msg_id = svc.call(lambda s, now: s.send_text(to, text, ack and to != "CQ", now))
        return {"ok": True, "id": msg_id}
    if cmd == "test_tone":
        mode = req.get("mode", "alt")
        svc.call(lambda s, now: s.start_test_tone(mode, now))
        return {"ok": True}
    if cmd == "restart":
        svc.restart()
        return {"ok": True}
    return {"ok": False, "error": f"unknown command {cmd!r}"}


class _Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(service, port):
    """Starts the control server on a daemon thread; returns the server (shutdown() to stop)."""
    srv = _Server(("127.0.0.1", port), _Handler)
    srv.service = service
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
