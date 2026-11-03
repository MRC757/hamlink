# SPDX-License-Identifier: GPL-3.0-or-later
"""Push-to-talk: none (radio VOX or an auto-PTT cable), or the RTS / DTR line of a serial port (DigiRig)."""
import glob
import sys


class NoPtt:
    description = "none (VOX)"

    def key(self, on):
        pass

    def ready(self):
        return True

    def close(self):
        pass


class SerialPtt:
    """Opens the port with both lines low (unkeyed), then drives the chosen line. Reopens after the USB device returns."""

    def __init__(self, line, port="", log=None):
        self.line, self.log = line, log or (lambda m: None)
        self.port = port or self.only_port()
        self.description = f"{line.upper()} on {self.port}"
        self.s = None
        self._open()

    @staticmethod
    def only_port():
        if sys.platform == "win32":
            raise RuntimeError("set ptt_port (e.g. COM5)")
        ports = sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))
        if len(ports) != 1:
            raise RuntimeError(f"set ptt_port: found {len(ports)} USB serial ports {ports}")
        return ports[0]

    def _open(self):
        import serial
        try:
            s = serial.Serial()
            s.port = self.port
            s.rts = False
            s.dtr = False
            s.open()
            self.s = s
        except Exception as e:
            self.s = None
            self.log(f"PTT port {self.port} not available: {e}")

    def ready(self):
        if self.s is None:
            self._open()
        return self.s is not None

    def key(self, on):
        if self.s is None and not self.ready():
            if on:
                raise RuntimeError(f"PTT line is not ready ({self.port})")
            return
        try:
            if self.line == "rts":
                self.s.rts = on
            else:
                self.s.dtr = on
        except Exception as e:
            self.close()
            if on:
                raise RuntimeError(f"PTT failed on {self.port}: {e}")

    def close(self):
        if self.s is not None:
            try:
                self.s.rts = False
                self.s.dtr = False
                self.s.close()
            except Exception:
                pass
            self.s = None


def make(kind, port="", log=None):
    return NoPtt() if kind == "none" else SerialPtt(kind, port, log)
