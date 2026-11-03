# SPDX-License-Identifier: GPL-3.0-or-later
"""Audio for the audio link. Every backend delivers received audio as 48 kHz mono int16 blocks and plays transmissions.

* SoundDeviceAudio: a real sound card (DigiRig) through PortAudio (`sounddevice`): ALSA on the Pi, any API on Windows.
* Air / AirAudio: a simulated radio channel joining several stations in one process, in simulated time, for tests.
"""
import queue
import time

import numpy as np

FS = 48000
BLOCK = 960  # 20 ms


class SoundDeviceAudio:
    blocking = True  # play() returns when the audio has been played

    def __init__(self, in_name="", out_name="", log=None):
        import sounddevice as sd  # imported here so tests and the TNC link do not need PortAudio
        self.sd = sd
        self.log = log or (lambda m: None)
        self.in_dev = self._find(in_name, "input")
        self.out_dev = self._find(out_name, "output")
        self.q = queue.Queue(maxsize=500)       # 10 s of audio
        self.stream = None
        self.in_channels = 1
        self.last_input = 0.0
        self.description = (f"in: {self._name(self.in_dev, 'input')}, out: {self._name(self.out_dev, 'output')}")

    # ---- devices
    def _find(self, part, kind):
        if not part:
            return None  # system default
        key = "max_input_channels" if kind == "input" else "max_output_channels"
        for i, d in enumerate(self.sd.query_devices()):
            if d[key] > 0 and part.lower() in d["name"].lower():
                return i
        raise RuntimeError(f"no {kind} audio device whose name contains '{part}' (see hamlinkd --list-devices)")

    def _name(self, idx, kind):
        try:
            return self.sd.query_devices(idx, kind)["name"]
        except Exception:
            return "default"

    @staticmethod
    def list_devices():
        import sounddevice as sd
        out = []
        for i, d in enumerate(sd.query_devices()):
            kinds = ("in " if d["max_input_channels"] else "") + ("out" if d["max_output_channels"] else "")
            out.append(f"{i:3d}  {kinds:6s} {d['name']}  ({d['default_samplerate']:.0f} Hz)")
        return out

    # ---- receive
    def start(self):
        def cb(indata, frames, t, status):
            self.last_input = time.monotonic()
            try:
                self.q.put_nowait(indata[:, 0].copy())
            except queue.Full:
                pass  # the service fell behind; drop rather than block the audio thread
        for ch in (1, 2):  # some USB codecs / ALSA devices only open in stereo
            try:
                self.stream = self.sd.InputStream(samplerate=FS, channels=ch, dtype="int16", blocksize=BLOCK,
                                                  device=self.in_dev, callback=cb)
                self.stream.start()
                self.in_channels = ch
                self.last_input = time.monotonic()
                return
            except Exception as e:
                last = e
        raise RuntimeError(f"cannot open the audio input: {last}")

    def stop(self):
        if self.stream:
            try:
                self.stream.stop()
                self.stream.close()
            except Exception:
                pass
            self.stream = None

    def read(self, timeout):
        try:
            return self.q.get(timeout=timeout) if timeout > 0 else self.q.get_nowait()
        except queue.Empty:
            return None

    def flush(self):
        while self.read(0) is not None:
            pass

    def alive(self, now=None):
        """Input blocks arrived recently (a vanished USB sound card stops them)."""
        return time.monotonic() - self.last_input < 5.0

    # ---- transmit
    def play(self, pcm):
        try:
            self.sd.play(pcm, FS, device=self.out_dev, blocking=True)
        except Exception:
            self.sd.play(np.column_stack([pcm, pcm]), FS, device=self.out_dev, blocking=True)  # stereo-only device

    def transmitting(self):
        return False


class Air:
    """A simulated simplex channel. Stations hear each other only where a link exists; every station also hears hiss.
    Time advances by one 20 ms block per step()."""

    def __init__(self, noise=30.0, seed=1):
        self.stations = []
        self.links = {}
        self.rng = np.random.default_rng(seed)
        self.noise = noise
        self.t_ms = 1000

    def station(self, name):
        a = AirAudio(self, name)
        self.stations.append(a)
        return a

    def link(self, a, b, gain=1.0):
        self.links[(a, b)] = self.links[(b, a)] = gain

    def hears(self, rx, tx):
        return self.links.get((rx.name, tx.name), 0.0)

    def step(self):
        """Delivers the next 20 ms to every station, then advances every transmission by 20 ms."""
        for rx in self.stations:
            x = self.rng.normal(0, self.noise, BLOCK)
            for tx in self.stations:
                if tx is not rx and tx.tx is not None:
                    g = self.hears(rx, tx)
                    if g:
                        seg = tx.tx[tx.tx_pos:tx.tx_pos + BLOCK].astype(float)
                        x[:len(seg)] += g * seg
            rx.inbox.append(np.clip(np.round(x), -32768, 32767).astype(np.int16))
        for tx in self.stations:
            if tx.tx is not None:
                tx.tx_pos += BLOCK
                tx.air_s += BLOCK / FS
                if tx.tx_pos >= len(tx.tx):
                    tx.tx = None
        self.t_ms += 20


class AirAudio:
    blocking = False  # play() starts a transmission; transmitting() says when it has finished
    description = "simulated channel"

    def __init__(self, air, name):
        self.air, self.name = air, name
        self.inbox = []
        self.tx = None
        self.tx_pos = 0
        self.air_s = 0.0

    def start(self):
        pass

    def stop(self):
        pass

    def read(self, timeout):
        return self.inbox.pop(0) if self.inbox else None

    def flush(self):
        self.inbox.clear()

    def alive(self, now=None):
        return True

    def play(self, pcm):
        self.tx, self.tx_pos = pcm, 0

    def transmitting(self):
        return self.tx is not None
