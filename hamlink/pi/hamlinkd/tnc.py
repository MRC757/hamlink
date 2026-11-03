# SPDX-License-Identifier: GPL-3.0-or-later
"""The KISS byte stream of a radio's built-in TNC: a paired Bluetooth radio (bt:<address>, an RFCOMM socket, no
rfcomm binding needed), an rfcomm device (/dev/rfcomm0) or a USB serial port. The core does the KISS framing; this only
moves bytes and reconnects when the radio goes away."""
import socket
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


class BluetoothTnc(SerialTnc):
    """RFCOMM straight to the radio (Linux). The radio must be paired and trusted first (hamlinkd --pair-tnc).
    The channel defaults to 1, the serial port profile's usual channel; bt:<address>:<channel> overrides it."""

    def __init__(self, spec, log=None):
        parts = spec[3:].split(":")
        self.addr = ":".join(parts[:6]).upper()
        self.channel = int(parts[6]) if len(parts) > 6 else 1
        super().__init__(spec, log)
        self.description = f"KISS TNC on Bluetooth {self.addr} channel {self.channel}"

    connecting = None

    def _open(self):
        """Connects on a background thread (a radio that is off takes seconds to fail), so the engine never waits."""
        if self.connecting is not None:
            if self.connecting.is_alive():
                return False
            self.connecting = None
            return self.s is not None
        now = time.monotonic()
        if now < self.next_try:
            return False
        self.next_try = now + 10.0  # a radio that is off or out of range: try again every 10 s
        import threading
        self.connecting = threading.Thread(target=self._connect, daemon=True)
        self.connecting.start()
        return False

    def _connect(self):
        try:
            s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
            s.settimeout(8)
            s.connect((self.addr, self.channel))
            s.setblocking(False)
            self.s = s
            self.log(f"TNC connected (Bluetooth {self.addr})")
            self.connected_once = True
        except Exception as e:
            if self.connected_once is not None:
                self.log(f"TNC {self.addr} not reachable: {e} (radio off, out of range, or not paired?)")
                self.connected_once = None
            self.s = None

    def read(self):
        if not self.connected():
            return b""
        try:
            d = self.s.recv(4096)
            if not d:
                self._lost("radio closed the connection")
            return d
        except (BlockingIOError, InterruptedError):
            return b""
        except Exception as e:
            self._lost(e)
            return b""

    def write(self, data):
        if not self.connected():
            raise RuntimeError(f"TNC not connected ({self.addr})")
        try:
            self.s.setblocking(True)
            self.s.settimeout(5)
            self.s.sendall(data)
            self.s.setblocking(False)
        except Exception as e:
            self._lost(e)
            raise RuntimeError(f"TNC write failed: {e}")


def make(port, log=None):
    return BluetoothTnc(port, log) if port.startswith("bt:") else SerialTnc(port, log)


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
