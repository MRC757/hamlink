# SPDX-License-Identifier: GPL-3.0-or-later
"""The relay engine: feeds received audio (or TNC bytes) to the core and puts its transmissions on the air.

Everything happens in step(now_ms), called every 20 ms by run() on the service thread, or by a test with simulated
time. Core callbacks therefore fire on that thread. Other threads (the control server) only call submit() and
read status(). The transmit sequence mirrors the Windows engine: key, play, 200 ms tail, unkey, 150 ms, then drop
the audio captured while keyed.
"""
import collections
import os
import queue
import threading
import time
import wave

from . import core as hl

FS = 48000
TAIL_MS, SETTLE_MS = 200, 150
CLOCK_EVERY_MS = 300_000
TEST_TONE_S = 10.0


class TxBudget:
    """Airtime limits: no single transmission over max_s, and at most pct % of any rolling hour."""

    def __init__(self, max_s, pct):
        self.max_s, self.pct = max_s, pct
        self.spans = collections.deque()  # (start_ms, seconds)

    def used_s(self, now_ms):
        while self.spans and self.spans[0][0] < now_ms - 3_600_000:
            self.spans.popleft()
        return sum(s for _, s in self.spans)

    def allows_more(self, now_ms):
        return self.used_s(now_ms) < 3600 * self.pct / 100.0

    def add(self, now_ms, seconds):
        self.spans.append((now_ms, seconds))


class Recorder:
    """Received audio to 10-minute WAV files; the oldest are deleted to stay under max_mb."""

    def __init__(self, folder, max_mb):
        self.folder, self.max_bytes = folder, max_mb * 1_000_000
        os.makedirs(folder, exist_ok=True)
        self.w, self.frames = None, 0

    def write(self, pcm):
        opened = None
        if self.w is None or self.frames >= FS * 600:
            self.close()
            self._trim()
            path = os.path.join(self.folder, time.strftime("rx-%Y%m%d-%H%M%S.wav"))
            self.w = wave.open(path, "wb")
            self.w.setnchannels(1)
            self.w.setsampwidth(2)
            self.w.setframerate(FS)
            self.frames = 0
            opened = path
        self.w.writeframes(pcm.tobytes())
        self.frames += len(pcm)
        return opened

    def _trim(self):
        files = sorted(os.path.join(self.folder, f) for f in os.listdir(self.folder) if f.endswith(".wav"))
        total = sum(os.path.getsize(f) for f in files)
        while files and total > self.max_bytes * 0.9:
            total -= os.path.getsize(files[0])
            os.remove(files.pop(0))

    def close(self):
        if self.w is not None:
            self.w.close()
            self.w = None


class Station:
    def __init__(self, settings, lib, audio=None, ptt=None, tnc=None, state_dir=".", log=None, seed=None):
        self.s = settings
        self.log_sink = log or print_entry
        self.events = collections.deque(maxlen=300)
        self.messages = collections.deque(maxlen=100)
        self.traffic = collections.deque(maxlen=200)  # monitor mode: messages between other stations
        self.cmds = queue.Queue()
        self.audio, self.ptt, self.tnc = audio, ptt, tnc
        self.state_dir = state_dir
        self.core = hl.Core(lib, settings.callsign, relay=settings.relay, ttl=settings.ttl,
                            preamble_ms=settings.preamble_ms, seed=seed if seed is not None else int(time.time()),
                            on_message=self._on_message, on_status=self._on_status, on_event=self._on_event,
                            on_monitor=self._on_monitor)
        if settings.link == "tnc":
            self.core.set_link(hl.LINK_TNC, settings.tnc_baud)
        else:
            if self.core.set_modem(settings.modem) != 0:
                raise RuntimeError(f"core refused modem {settings.modem}")
        self.core.set_fec(settings.afsk_fec)
        self.core.set_monitor(settings.monitor)
        if settings.link == "audio":
            self.core.set_listen_all(settings.listen_all)
        self.modem_now = hl.MODEMS.get(settings.modem, 0) if settings.link == "audio" else -1
        self.core.set_carrier_sense(settings.carrier_sense)
        self.core.set_relay(settings.relay)
        self.budget = TxBudget(settings.max_tx_seconds, settings.max_tx_percent)
        self.recorder = None
        if settings.record_rx and settings.link == "audio":
            self.recorder = Recorder(settings.record_dir or os.path.join(state_dir, "recordings"), settings.record_max_mb)
        self.tx = None           # None, or {"phase": "air"|"tail"|"settle", "until": ms, "seconds": s, "test": bool}
        self.next_clock = 0
        self.level = 0.0
        self.started_ms = None
        self.limited_logged = False
        self.dead_logged = False
        self.next_status = 0
        self.next_health = 0
        self.health = {}
        self._status = {}
        self.running = False

    # ---- logging and callbacks (service thread)
    def log(self, text, kind="event"):
        e = {"t": time.time(), "kind": kind, "text": text}
        self.events.append(e)
        self.log_sink(e)

    def _via(self):
        m = self.core.rx_modem()
        return hl.MODEM_NAMES.get(m) if m >= 0 and self.modem_now >= 0 and m != self.modem_now else None

    def _on_monitor(self, src, dst, typ, msg_id, data):
        via = self._via()
        if typ == hl.TYPE_ACK:
            for e in reversed(self.traffic):
                if e["src"] == dst and e["id"] == msg_id:
                    e["status"] = f"delivered (ACK from {src})"
                    break
            return
        if typ == hl.TYPE_ID:
            return
        if typ == hl.TYPE_TEXT:
            text = data.decode(errors="replace")
        elif typ == hl.TYPE_REPORT:
            text = "[report] " + self.core.report_summary(data)
        elif typ == hl.TYPE_VOICE:
            text = f"[voice clip, {len(data)} bytes]"
        else:
            return
        self.traffic.append({"t": time.time(), "src": src, "dst": dst, "id": msg_id, "text": text, "via": via,
                             "status": None})
        self.log(f"HEARD {src} > {dst}: {text}" + (f" (via {via})" if via else ""), "heard")

    def _on_message(self, src, dst, typ, data):
        via = self._via()
        if typ == hl.TYPE_TEXT:
            text = data.decode(errors="replace")
        elif typ == hl.TYPE_REPORT:
            text = "[report] " + self.core.report_summary(data)
        elif typ == hl.TYPE_VOICE:
            text = f"[voice clip, {len(data)} bytes]"
        else:
            return
        self.messages.append({"t": time.time(), "src": src, "dst": dst, "text": text, "via": via})
        self.log(f"RX {src} > {dst}: {text}" + (f" (via {via})" if via else ""), "rx")

    def _on_status(self, msg_id, status):
        self.log(f"message {msg_id} {status}", "status")

    def _on_event(self, text):
        self.log(text)

    # ---- commands from other threads
    def submit(self, fn):
        """Runs fn(station) on the service thread; returns a queue that receives its result (or the exception)."""
        q = queue.Queue(maxsize=1)
        self.cmds.put((fn, q))
        return q

    def status(self):
        return dict(self._status)

    # ---- operations (service thread)
    def send_text(self, dst, text, ack, now_ms):
        if not text.strip():
            raise ValueError("empty message")
        msg_id = self.core.send_text(dst or "CQ", text, ack, now_ms)
        if msg_id < 0:
            raise RuntimeError("the core refused the message (too long, or a bad callsign)")
        self.log(f"queued message {msg_id} to {dst or 'CQ'}: {text}", "tx")
        return msg_id

    def start_test_tone(self, mode, now_ms):
        """Transmits a test tone for TEST_TONE_S seconds (audio link only)."""
        if self.s.link != "audio":
            raise RuntimeError("the test tone needs the audio link")
        if self.tx is not None:
            raise RuntimeError("already transmitting")
        if self.core.busy():
            raise RuntimeError("channel is busy")
        self.core.tone_start(mode)
        pcm, _ = self.core.tone_fill(int(FS * TEST_TONE_S), self.s.tx_level)
        self._key_and_play(pcm, now_ms, test=True)
        self.log(f"test tone {mode} for {TEST_TONE_S:.0f} s at level {self.s.tx_level}")

    # ---- the loop
    def step(self, now_ms):
        if self.started_ms is None:
            self.started_ms = now_ms
        while True:
            try:
                fn, q = self.cmds.get_nowait()
            except queue.Empty:
                break
            try:
                q.put((True, fn(self, now_ms)))
            except Exception as e:
                q.put((False, e))
        if now_ms >= self.next_clock:
            self.next_clock = now_ms + CLOCK_EVERY_MS
            self.core.set_clock(time.time(), now_ms)
        if self.s.link == "tnc":
            self._step_tnc(now_ms)
        else:
            self._step_audio(now_ms)
        if now_ms >= self.next_status:
            self.next_status = now_ms + 1000
            self._update_status(now_ms)

    def _step_tnc(self, now_ms):
        data = self.tnc.read()
        self.core.tnc_rx(data, now_ms)  # empty runs the ACK / retry / ID timers
        if self.tnc.connected():
            out = self.core.tnc_poll_tx(now_ms)
            if out:
                try:
                    self.tnc.write(out)
                except Exception as e:
                    self.log(f"write to the radio failed; ARQ will retry ({e})", "error")

    def _step_audio(self, now_ms):
        if self.tx is not None:
            self._step_tx(now_ms)
            return
        got = False
        while True:
            block = self.audio.read(0)
            if block is None:
                break
            got = True
            self.level = self.core.process(block, now_ms, self.s.busy_threshold)
            if self.recorder:
                try:
                    opened = self.recorder.write(block)
                    if opened:
                        self.log(f"recording received audio to {opened}")
                except Exception as e:
                    self.log(f"recording stopped: {e}", "error")
                    self.recorder.close()
                    self.recorder = None
        if not got:
            self.level = self.core.process(None, now_ms, self.s.busy_threshold)
        self._maybe_transmit(now_ms)

    def _maybe_transmit(self, now_ms):
        # Never take a burst off the core's queue when it cannot go out; ARQ would have to recover it.
        if not self.audio.alive(now_ms):
            if not self.dead_logged:
                self.log("audio input has stopped (sound card unplugged?); not transmitting", "error")
                self.dead_logged = True
            return
        if self.dead_logged:
            self.log("audio input is back")
            self.dead_logged = False
        if not self.budget.allows_more(now_ms):
            if not self.limited_logged:
                self.log(f"transmit limit reached ({self.s.max_tx_percent:.0f}% of the last hour); holding", "error")
                self.limited_logged = True
            return
        self.limited_logged = False
        if not self.ptt.ready():
            return
        pcm = self.core.poll_tx(now_ms, self.s.tx_level)
        if pcm is None:
            return
        seconds = len(pcm) / FS
        if seconds > self.s.max_tx_seconds:
            self.log(f"dropped a {seconds:.0f} s transmission (max_tx_seconds is {self.s.max_tx_seconds:.0f})", "error")
            return
        self._key_and_play(pcm, now_ms)

    def _key_and_play(self, pcm, now_ms, test=False):
        seconds = len(pcm) / FS
        try:
            self.ptt.key(True)
        except Exception as e:
            self.log(f"transmit failed: {e}", "error")
            return
        self.budget.add(now_ms, seconds)
        self.tx = {"phase": "air", "until": now_ms + int(seconds * 1000) + 1500, "seconds": seconds, "test": test}
        try:
            self.audio.play(pcm)  # blocks for a sound card; starts the transmission on the simulated channel
        except Exception as e:
            self.log(f"transmit failed: {e}", "error")
        if self.audio.blocking:  # the audio has been played: finish in real time, as the Windows engine does
            time.sleep(TAIL_MS / 1000)
            self._unkey()
            time.sleep(SETTLE_MS / 1000)
            self.audio.flush()
            self._tx_done(now_ms)
        else:
            self._step_tx(now_ms)

    def _unkey(self):
        try:
            self.ptt.key(False)
        except Exception as e:
            self.log(f"unkey failed: {e}", "error")

    def _tx_done(self, now_ms):
        test = self.tx["test"]
        self.tx = None
        if test:
            self.core.note_external_tx(now_ms, 300)
            self.log("test tone ended (station ID will follow)")

    def _step_tx(self, now_ms):
        self.audio.flush()  # drop everything heard while keyed
        tx = self.tx
        if tx["phase"] == "air":
            if self.audio.transmitting() and now_ms < tx["until"]:
                return
            tx["phase"], tx["until"] = "tail", now_ms + TAIL_MS
        if tx["phase"] == "tail":
            if now_ms < tx["until"]:
                return
            self._unkey()
            tx["phase"], tx["until"] = "settle", now_ms + SETTLE_MS
        if tx["phase"] == "settle" and now_ms >= tx["until"]:
            self._tx_done(now_ms)

    def _update_status(self, now_ms):
        st = {
            "callsign": self.s.callsign,
            "link": self.s.link,
            "modem": self.s.modem if self.s.link == "audio" else "afsk (TNC)",
            "relay": self.s.relay,
            "monitor": self.s.monitor,
            "listen_all": self.s.listen_all,
            "uptime_s": (now_ms - self.started_ms) // 1000,
            "transmitting": self.tx is not None,
            "channel_busy": self.core.busy(),
            "pending_tx": self.core.pending_tx(),
            "tx_seconds_last_hour": round(self.budget.used_s(now_ms), 1),
            "stats": self.core.stats(),
        }
        if self.s.link == "audio":
            st["input_level"] = round(float(self.level), 4)
            st["meter"] = {k: round(v, 1) if isinstance(v, float) else v for k, v in self.core.meter().items()}
            st["audio"] = self.audio.description
            st["audio_alive"] = self.audio.alive(now_ms)
            st["ptt"] = self.ptt.description
            st["ptt_ready"] = self.ptt.ready()
        else:
            st["tnc"] = self.tnc.description
            st["tnc_connected"] = self.tnc.connected()
        if now_ms >= self.next_health:
            self.next_health = now_ms + 30_000
            old, self.health = self.health, system_health()
            for key, text in (("undervoltage_now", "supply voltage is LOW (battery, cable or regulator)"),
                              ("throttled_now", "the CPU is throttled (heat or low voltage)"),
                              ("hot", "the CPU is above 80 °C")):
                if key == "hot":
                    now_on, was_on = self.health.get("cpu_temp_c", 0) > 80, old.get("cpu_temp_c", 0) > 80
                else:
                    now_on, was_on = self.health.get(key, False), old.get(key, False)
                if now_on and not was_on:
                    self.log("WARNING: " + text, "error")
        st.update(self.health)
        self._status = st

    # ---- real time
    def run(self, stop_event=None):
        """Real-time loop: until stop_event is set (or forever)."""
        stop_event = stop_event or threading.Event()
        t0 = time.monotonic()
        if self.audio:
            self.audio.start()
        self.running = True
        self.log(f"{self.s.callsign} relay started: " +
                 (f"{self.s.modem}, {self.audio.description}, PTT {self.ptt.description}" if self.s.link == "audio"
                  else self.tnc.description))
        try:
            n = 0
            while not stop_event.is_set():
                self.step(int((time.monotonic() - t0) * 1000) + 1000)
                n += 1
                if n % 100 == 0:
                    sd_notify("WATCHDOG=1")  # systemd restarts the service if this loop stalls
                stop_event.wait(0.02)
        finally:
            self.running = False
            self.close()

    def close(self):
        try:
            if self.ptt:
                self.ptt.key(False)
                self.ptt.close()
        except Exception:
            pass
        if self.audio:
            self.audio.stop()
        if self.tnc:
            self.tnc.close()
        if self.recorder:
            self.recorder.close()
        self.core.close()


def print_entry(e):
    print(time.strftime("%Y-%m-%d %H:%M:%S ", time.localtime(e["t"])) + e["text"], flush=True)


def sd_notify(msg):
    """systemd notification (Type=notify, WatchdogSec); does nothing outside systemd."""
    addr = os.environ.get("NOTIFY_SOCKET")
    if not addr:
        return
    import socket
    if addr.startswith("@"):
        addr = "\0" + addr[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.sendto(msg.encode(), addr)
    except OSError:
        pass


def system_health():
    """CPU temperature and under-voltage / throttling flags on a Raspberry Pi; empty elsewhere."""
    h = {}
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            h["cpu_temp_c"] = round(int(f.read()) / 1000.0, 1)
    except Exception:
        pass
    try:
        import subprocess
        out = subprocess.run(["vcgencmd", "get_throttled"], capture_output=True, text=True, timeout=2).stdout
        v = int(out.strip().split("=")[1], 16)
        h["undervoltage_now"] = bool(v & 0x1)
        h["throttled_now"] = bool(v & 0x4)
        h["undervoltage_since_boot"] = bool(v & 0x10000)
    except Exception:
        pass
    return h
