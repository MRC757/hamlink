# SPDX-License-Identifier: GPL-3.0-or-later
"""The running service: keeps a Station going, and the web page and control port up even while there is none.

The station is (re)built from the settings file: at start, after a settings change, and 10 s after a failure (a sound
card that is not plugged in, say). Until the settings are valid (a new SD card has no callsign) only the web page
and the control port run, so the relay can be set up from a phone.
"""
import collections
import itertools
import os
import threading
import time

from . import settings as S
from .station import print_entry, sd_notify

RETRY_S = 10


class Service:
    def __init__(self, config_path, lib_path="", state_dir=".", make_io=None, sink=print_entry):
        self.config_path, self.lib_path, self.state_dir = config_path, lib_path, state_dir
        self.make_io = make_io or real_io
        self.sink = sink
        self.events = collections.deque(maxlen=500)
        self.seq = itertools.count(1)
        self.station = None
        self.error = ""
        self.lib = None
        self.halt = threading.Event()     # stops the current station (restart or shutdown)
        self.stopping = threading.Event()
        self.lock = threading.Lock()
        self.seen = self._mtime()

    # ---- log, shared by every station this process runs
    def log(self, text, kind="event"):
        self.add_entry({"t": time.time(), "kind": kind, "text": text})

    def add_entry(self, e):
        e = dict(e, seq=next(self.seq))
        self.events.append(e)
        self.sink(e)

    def log_since(self, seq, n=200):
        return [e for e in list(self.events) if e["seq"] > seq][-n:]

    # ---- settings
    def settings(self):
        return S.load(self.config_path)

    def save_settings(self, new):
        """Saves valid settings and restarts the station with them; returns the problems otherwise."""
        bad = new.problems()
        if bad:
            return bad
        with self.lock:
            S.save(new, self.config_path)
            self.seen = self._mtime()  # our own change: the watcher need not restart again
        self.log("settings saved; restarting the radio")
        self.restart()
        return []

    def restart(self):
        self.halt.set()

    def stop(self):
        self.stopping.set()
        self.halt.set()

    # ---- status for the web page and CLI
    def status(self):
        st = self.station
        if st is not None and st.running:
            return dict(st.status(), state="running")
        cfg = self.settings()
        bad = cfg.problems()
        return {"state": "not configured" if bad else "stopped", "problems": bad, "error": self.error,
                "callsign": cfg.callsign}

    def call(self, fn, timeout=30):
        """Runs fn(station, now_ms) on the station's thread; raises when no radio is running."""
        st = self.station
        if st is None or not st.running:
            raise RuntimeError("the radio is not running" + (f": {self.error}" if self.error else ""))
        ok, val = st.submit(fn).get(timeout=timeout)
        if not ok:
            raise val
        return val

    # ---- main loop
    def _mtime(self):
        try:
            return os.stat(self.config_path).st_mtime_ns
        except OSError:
            return 0

    def _watch(self):
        """Restarts the radio when the settings file changes (hamlinkd --setup, or edited by hand)."""
        while not self.stopping.wait(3.0):
            with self.lock:
                m = self._mtime()
                if m == self.seen:
                    continue
                self.seen = m
            self.log("settings file changed; restarting the radio")
            self.restart()

    def run(self):
        sd_notify("READY=1")
        threading.Thread(target=self._watch, name="settings-watch", daemon=True).start()
        while not self.stopping.is_set():
            self.halt.clear()
            cfg = self.settings()
            bad = cfg.problems()
            if bad:
                if self.error != "not configured":
                    self.log("not configured yet: " + "; ".join(bad) + " (web page, or hamlinkd --setup)", "error")
                self.error = "not configured"
                self._wait(None)
                continue
            try:
                st = self._build(cfg)
            except Exception as e:
                self.error = str(e)
                self.log(f"radio not started: {e}; trying again in {RETRY_S} s", "error")
                self._wait(RETRY_S)
                continue
            self.error = ""
            self.station = st
            try:
                st.run(self.halt)
            except Exception as e:
                self.error = str(e)
                self.log(f"radio stopped: {e}; restarting in {RETRY_S} s", "error")
                self._wait(RETRY_S)
            finally:
                self.station = None
        self.log("stopped")

    def _wait(self, seconds):
        """Waits for a restart request (or the timeout), keeping the systemd watchdog fed."""
        end = None if seconds is None else time.monotonic() + seconds
        while not self.halt.wait(1.0):
            sd_notify("WATCHDOG=1")
            if end is not None and time.monotonic() >= end:
                return

    def _build(self, cfg):
        from . import core, station
        if self.lib is None:
            self.lib = core.load(core.find_library(self.lib_path))
        audio, ptt, tnc = self.make_io(cfg, self.log)
        try:
            return station.Station(cfg, self.lib, audio=audio, ptt=ptt, tnc=tnc, state_dir=self.state_dir,
                                   log=self.add_entry)
        except Exception:
            for x in (ptt, tnc, audio):
                try:
                    x and (x.close() if hasattr(x, "close") else x.stop())
                except Exception:
                    pass
            raise


def real_io(cfg, log):
    if cfg.link == "audio":
        from . import audio as au, ptt as pt
        a = au.SoundDeviceAudio(cfg.audio_in, cfg.audio_out)
        try:
            return a, pt.make(cfg.ptt, cfg.ptt_port, log=log), None
        except Exception:
            a.stop()
            raise
    from . import tnc as tn
    return None, None, tn.make(cfg.tnc_port, log=log)
