# SPDX-License-Identifier: GPL-3.0-or-later
"""The relay's web page: status, log, messages, settings, test tone, Bluetooth pairing and Help, behind a password.

Standard library only (the Zero 2 W has little memory to spare). One static page (static/index.html) talks to the
JSON API below. Security: a password (PBKDF2) set on first visit or with `hamlinkd --set-password`; a session cookie
(HttpOnly, SameSite=Strict); every POST must carry the header X-HamLink: 1, which a page on another site cannot send
without a CORS preflight that this server never grants; repeated wrong passwords lock the login for a while.
Plain HTTP: it is meant for the Pi's own WPA2 hotspot or a trusted local network.
"""
import hashlib
import hmac
import html
import json
import os
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from . import control, settings as S

HERE = os.path.dirname(os.path.abspath(__file__))
HELP_PATHS = [os.path.join(os.path.dirname(os.path.dirname(HERE)), "docs", "help", "HELP.md"),
              "/opt/hamlink/src/hamlink/docs/help/HELP.md"]
SESSION_S = 12 * 3600
ITERATIONS = 100_000
LOCK_AFTER, LOCK_S = 5, 60


class Auth:
    """The password file (web.json next to the settings) and the login sessions."""

    def __init__(self, path):
        self.path = path
        self.sessions = {}
        self.failures = 0
        self.locked_until = 0.0
        self.lock = threading.Lock()

    def password_set(self):
        return os.path.exists(self.path)

    def set_password(self, pw):
        if len(pw) < 8:
            raise ValueError("the password needs at least 8 characters")
        salt = secrets.token_bytes(16)
        rec = {"salt": salt.hex(), "iterations": ITERATIONS,
               "hash": hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, ITERATIONS).hex()}
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
            json.dump(rec, f)
        os.replace(tmp, self.path)
        with self.lock:
            self.sessions.clear()  # a new password logs everyone out

    def check(self, pw):
        with self.lock:
            if time.monotonic() < self.locked_until:
                raise PermissionError("too many wrong passwords; try again in a minute")
        try:
            with open(self.path) as f:
                rec = json.load(f)
            h = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(rec["salt"]), rec["iterations"]).hex()
            good = hmac.compare_digest(h, rec["hash"])
        except (OSError, ValueError, KeyError):
            good = False
        with self.lock:
            if good:
                self.failures = 0
            else:
                self.failures += 1
                if self.failures >= LOCK_AFTER:
                    self.failures = 0
                    self.locked_until = time.monotonic() + LOCK_S
        return good

    def new_session(self):
        tok = secrets.token_urlsafe(32)
        with self.lock:
            now = time.monotonic()
            self.sessions = {t: e for t, e in self.sessions.items() if e > now}
            self.sessions[tok] = now + SESSION_S
        return tok

    def valid(self, tok):
        with self.lock:
            e = self.sessions.get(tok or "")
            return e is not None and e > time.monotonic()

    def end(self, tok):
        with self.lock:
            self.sessions.pop(tok or "", None)


class Handler(BaseHTTPRequestHandler):
    server_version = "hamlinkd"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass  # the journal gets the relay's own log, not every poll

    # ---- plumbing
    def _send(self, code, body, ctype="application/json; charset=utf-8", headers=()):
        if not isinstance(body, bytes):
            body = (json.dumps(body) if ctype.startswith("application/json") else body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _token(self):
        for part in self.headers.get("Cookie", "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == "hl_session":
                return v
        return ""

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n > 65536:
            raise ValueError("request too large")
        return json.loads(self.rfile.read(n) or b"{}")

    def _cookie(self, tok, max_age):
        return ("Set-Cookie", f"hl_session={tok}; Path=/; HttpOnly; SameSite=Strict; Max-Age={max_age}")

    # ---- routes
    def do_GET(self):
        u = urlparse(self.path)
        w = self.server.web
        if u.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "static", "index.html"), "rb") as f:
                return self._send(200, f.read(), "text/html; charset=utf-8")
        if u.path == "/api/session":
            return self._send(200, {"logged_in": w.auth.valid(self._token()), "password_set": w.auth.password_set(),
                                    "callsign": w.svc.settings().callsign})
        if not w.auth.valid(self._token()):
            return self._send(401, {"ok": False, "error": "log in first"})
        q = parse_qs(u.query)
        try:
            if u.path == "/api/status":
                return self._send(200, control.dispatch(w.svc, {"cmd": "status"}))
            if u.path == "/api/log":
                return self._send(200, control.dispatch(w.svc, {"cmd": "log", "since": q.get("since", ["0"])[0]}))
            if u.path == "/api/messages":
                return self._send(200, control.dispatch(w.svc, {"cmd": "messages"}))
            if u.path == "/api/traffic":
                return self._send(200, control.dispatch(w.svc, {"cmd": "traffic"}))
            if u.path == "/api/settings":
                cfg = w.svc.settings()
                return self._send(200, {"ok": True, "values": {n: getattr(cfg, n) for n, *_ in S.FORM},
                                        "form": [{"name": n, "label": l, "choices": c, "help": h,
                                                  "type": type(getattr(cfg, n)).__name__} for n, l, c, h in S.FORM],
                                        "problems": cfg.problems(), "read_only_root": read_only_root()})
            if u.path == "/api/devices":
                return self._send(200, {"ok": True, **devices()})
            if u.path == "/api/bluetooth":
                from . import bt
                return self._send(200, {"ok": True, "devices": bt.known()})
            if u.path == "/help":
                return self._send(200, help_page(), "text/html; charset=utf-8")
        except Exception as e:
            return self._send(500, {"ok": False, "error": str(e)})
        return self._send(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        w = self.server.web
        if self.headers.get("X-HamLink") != "1":
            return self._send(403, {"ok": False, "error": "missing X-HamLink header"})
        path = urlparse(self.path).path
        try:
            body = self._body()
            if path == "/api/login":
                if not w.auth.check(str(body.get("password", ""))):
                    time.sleep(1)
                    return self._send(403, {"ok": False, "error": "wrong password"})
                return self._send(200, {"ok": True}, headers=[self._cookie(w.auth.new_session(), SESSION_S)])
            if path == "/api/setup-password":
                # First visit only: once a password exists, changing it needs the old one (or the CLI).
                with w.setup_lock:
                    if w.auth.password_set():
                        return self._send(403, {"ok": False, "error": "a password is already set"})
                    w.auth.set_password(str(body.get("password", "")))
                w.svc.log("web page password set")
                return self._send(200, {"ok": True}, headers=[self._cookie(w.auth.new_session(), SESSION_S)])
            if not w.auth.valid(self._token()):
                return self._send(401, {"ok": False, "error": "log in first"})
            if path == "/api/logout":
                w.auth.end(self._token())
                return self._send(200, {"ok": True}, headers=[self._cookie("", 0)])
            if path == "/api/password":
                if not w.auth.check(str(body.get("old", ""))):
                    return self._send(403, {"ok": False, "error": "the current password is wrong"})
                w.auth.set_password(str(body.get("new", "")))
                w.svc.log("web page password changed")
                return self._send(200, {"ok": True}, headers=[self._cookie(w.auth.new_session(), SESSION_S)])
            if path == "/api/settings":
                new = S.from_form(w.svc.settings(), body.get("values", {}))
                bad = w.svc.save_settings(new)
                return self._send(200 if not bad else 400, {"ok": not bad, "problems": bad})
            if path == "/api/send":
                return self._send(200, control.dispatch(w.svc, {"cmd": "send", "to": body.get("to", "CQ"),
                                                                "text": body.get("text", ""),
                                                                "ack": bool(body.get("ack", True))}))
            if path == "/api/test_tone":
                return self._send(200, control.dispatch(w.svc, {"cmd": "test_tone", "mode": body.get("mode", "alt")}))
            if path == "/api/restart":
                return self._send(200, control.dispatch(w.svc, {"cmd": "restart"}))
            if path == "/api/bluetooth/scan":
                from . import bt
                return self._send(200, {"ok": True, "devices": bt.scan(12)})
            if path == "/api/bluetooth/pair":
                from . import bt
                bt.pair(str(body.get("address", "")))
                w.svc.log(f"Bluetooth: paired {body.get('address')}")
                return self._send(200, {"ok": True})
        except (ValueError, TypeError) as e:
            return self._send(400, {"ok": False, "error": str(e)})
        except PermissionError as e:
            return self._send(429, {"ok": False, "error": str(e)})
        except Exception as e:
            return self._send(500, {"ok": False, "error": str(e)})
        return self._send(404, {"ok": False, "error": "not found"})


class Web:
    def __init__(self, service, port, auth_path, host="0.0.0.0"):
        self.svc = service
        self.auth = Auth(auth_path)
        self.setup_lock = threading.Lock()
        self.httpd = ThreadingHTTPServer((host, port), Handler)
        self.httpd.daemon_threads = True
        self.httpd.web = self
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, name="web", daemon=True).start()

    def shutdown(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def auth_path(config_path):
    return os.path.join(os.path.dirname(os.path.abspath(config_path)), "web.json")


def read_only_root():
    """True when the root file system is an overlay (raspi-config's read-only mode): changes last until reboot."""
    try:
        with open("/proc/mounts") as f:
            return any(l.split()[1] == "/" and l.split()[0] == "overlay" for l in f)
    except OSError:
        return False


def devices():
    out = {"audio_in": [], "audio_out": [], "serial": []}
    try:
        import sounddevice as sd
        for d in sd.query_devices():
            if d["max_input_channels"]:
                out["audio_in"].append(d["name"])
            if d["max_output_channels"]:
                out["audio_out"].append(d["name"])
    except Exception as e:
        out["audio_error"] = str(e)
    try:
        from serial.tools import list_ports
        out["serial"] = [f"{p.device}" for p in list_ports.comports()]
    except Exception:
        pass
    return out


# ---- Help: docs/help/HELP.md as HTML (a small Markdown subset: headings, lists, code blocks, bold, code, links)
def _inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", t)
    t = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"<i>\1</i>", t)
    t = re.sub(r"\[([^\]]+)\]\((https?://[^)\s]+)\)", r'<a href="\2">\1</a>', t)
    return t


def markdown(md):
    out, para, lists, code = [], [], [], None

    def flush():
        if para:
            out.append("<p>" + _inline(" ".join(para)) + "</p>")
            para.clear()

    def close_lists(depth=0):
        while len(lists) > depth:
            out.append(f"</li></{lists.pop()}>")

    for line in md.splitlines():
        if code is not None:
            if line.strip().startswith("```"):
                out.append("<pre><code>" + html.escape("\n".join(code)) + "</code></pre>")
                code = None
            else:
                code.append(line)
            continue
        if line.strip().startswith("```"):
            flush(); close_lists()
            code = []
            continue
        m = re.match(r"(#{1,4})\s+(.*)", line)
        if m:
            flush(); close_lists()
            n = len(m.group(1))
            out.append(f"<h{n}>{_inline(m.group(2))}</h{n}>")
            continue
        m = re.match(r"(\s*)([-*]|\d+\.)\s+(.*)", line)
        if m:
            flush()
            depth = len(m.group(1)) // 2 + 1
            kind = "ol" if m.group(2)[0].isdigit() else "ul"
            if len(lists) < depth:
                lists.append(kind)
                out.append(f"<{kind}><li>")
            else:
                close_lists(depth)
                out.append("</li><li>")
            out.append(_inline(m.group(3)))
            continue
        if not line.strip():
            flush(); close_lists()
            continue
        if lists:
            out.append(" " + _inline(line.strip()))
        else:
            para.append(line.strip())
    flush(); close_lists()
    return "\n".join(out)


def help_page():
    for p in HELP_PATHS:
        if os.path.exists(p):
            with open(p, encoding="utf-8") as f:
                body = markdown(f.read())
            break
    else:
        body = "<p>Help file not found.</p>"
    return ("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
            "<title>HamLink help</title><style>body{font:16px/1.5 system-ui,sans-serif;max-width:46em;margin:auto;"
            "padding:1em}code,pre{background:#eee;border-radius:3px}pre{padding:.5em;overflow-x:auto}"
            "@media(prefers-color-scheme:dark){body{background:#111;color:#ddd}code,pre{background:#222}a{color:#8bf}}"
            "</style></head><body><p><a href='/'>&larr; Back to the relay</a></p>" + body + "</body></html>")
