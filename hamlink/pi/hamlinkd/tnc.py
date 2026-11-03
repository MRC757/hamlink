# SPDX-License-Identifier: GPL-3.0-or-later
"""The KISS byte stream of a radio's built-in TNC: a Bluetooth rfcomm device (/dev/rfcomm0) or a USB serial port.
The core does the KISS framing; this only moves bytes and reconnects when the radio goes away."""
import time


class SerialTnc:
    def __init__(self, port, log=None):
        self.port, self.log = port, log or (lambda m: None)
        self.description = f"KISS TNC on {port}"
        self.s = None
        self.next_try = 0.0
        self.connected_once = False

    def _open(self):
        import serial
        now = time.monotonic()
        if now < self.next_try:
            return False
        self.next_try = now + 5.0
        try:
            self.s = serial.Serial(self.port, 115200, timeout=0)  # the baud rate is ignored by rfcomm
            self.log(f"TNC connected ({self.port})")
            self.connected_once = True
            return True
        except Exception as e:
            if not self.connected_once:
                self.log(f"TNC {self.port} not available: {e}")
                self.connected_once = None  # say it once, not every 5 s
            self.s = None
            return False

    def connected(self):
        return self.s is not None or self._open()

    def read(self):
        if not self.connected():
            return b""
        try:
            return self.s.read(4096) or b""
        except Exception as e:
            self._lost(e)
            return b""

    def write(self, data):
        if not self.connected():
            raise RuntimeError(f"TNC not connected ({self.port})")
        try:
            self.s.write(data)
            self.s.flush()
        except Exception as e:
            self._lost(e)
            raise RuntimeError(f"TNC write failed: {e}")

    def _lost(self, e):
        self.log(f"TNC connection lost: {e}")
        self.close()
        self.connected_once = True

    def close(self):
        if self.s is not None:
            try:
                self.s.close()
            except Exception:
                pass
            self.s = None


class LoopTnc:
    """A pair of in-memory TNCs for tests: what one writes, the other reads (a perfect AFSK channel)."""

    description = "simulated TNC"

    def __init__(self):
        self.inbox = bytearray()
        self.peer = None

    @staticmethod
    def pair():
        a, b = LoopTnc(), LoopTnc()
        a.peer, b.peer = b, a
        return a, b

    def connected(self):
        return True

    def read(self):
        d, self.inbox = bytes(self.inbox), bytearray()
        return d

    def write(self, data):
        self.peer.inbox += data

    def close(self):
        pass
