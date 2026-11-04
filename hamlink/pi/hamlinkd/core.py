# SPDX-License-Identifier: GPL-3.0-or-later
"""ctypes binding to the HamLink core (core/hamlink_api.h): libhamlink_api.so on Linux / the Pi, hamlink_core.dll on
Windows (for development). Only what the relay service needs."""
import ctypes as C
import os
import sys

API_VERSION = 11
MODEMS = {"afsk": 0, "ofdm": 1, "mt63": 2, "psk2400": 3, "psk3600": 4}
LINK_AUDIO, LINK_TNC = 0, 1
CARRIER_SENSE = {"off": 0, "data": 1, "data_or_level": 2}
TONES = {"ptt": 0, "1200": 1, "2200": 2, "alt": 3}
TYPE_TEXT, TYPE_VOICE, TYPE_ACK, TYPE_ID, TYPE_REPORT = 1, 2, 3, 4, 5
MODEM_NAMES = {0: "AFSK", 1: "OFDM", 2: "MT63", 3: "PSK 2400", 4: "PSK 3600"}
STATUS = {0: "failed", 1: "delivered", 2: "sent"}

MSG_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.c_void_p, C.c_int)
ST_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_int)
EV_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)
MON_CB = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p, C.c_char_p, C.c_int, C.c_int, C.c_void_p, C.c_int)

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(os.path.dirname(_HERE))  # hamlink/


def find_library(explicit=""):
    """The core library: an explicit path, $HAMLINK_LIB, the installed copy, or a build in the source tree."""
    cands = [explicit, os.environ.get("HAMLINK_LIB", "")]
    if sys.platform == "win32":
        cands += [os.path.join(_REPO, "windows", "native", "hamlink_core.dll")]
    else:
        cands += ["/opt/hamlink/lib/libhamlink_api.so", os.path.join(_REPO, "build", "core", "libhamlink_api.so"),
                  os.path.join(_REPO, "build-host", "core", "libhamlink_api.so")]
    for p in cands:
        if p and os.path.exists(p):
            return os.path.abspath(p)
    raise FileNotFoundError("HamLink core library not found (build it with cmake, or set HAMLINK_LIB)")


def load(path):
    L = C.CDLL(path)
    L.hl_api_version.restype = C.c_int
    if L.hl_api_version() != API_VERSION:
        raise RuntimeError(f"{path}: core API version {L.hl_api_version()}, this service needs {API_VERSION}")
    sig = {
        "hl_create": (C.c_void_p, [C.c_char_p, C.c_int, C.c_int, C.c_int, C.c_uint32, MSG_CB, ST_CB, EV_CB, C.c_void_p]),
        "hl_destroy": (None, [C.c_void_p]),
        "hl_set_relay": (None, [C.c_void_p, C.c_int]),
        "hl_set_modem": (C.c_int, [C.c_void_p, C.c_int]),
        "hl_set_link": (None, [C.c_void_p, C.c_int, C.c_int]),
        "hl_set_fec": (C.c_int, [C.c_void_p, C.c_int]),
        "hl_set_carrier_sense": (None, [C.c_void_p, C.c_int]),
        "hl_set_bit_repair": (None, [C.c_void_p, C.c_int]),
        "hl_set_clock": (None, [C.c_void_p, C.c_int64, C.c_int64]),
        "hl_process_audio": (C.c_float, [C.c_void_p, C.c_void_p, C.c_int, C.c_int64, C.c_float]),
        "hl_poll_tx": (C.c_void_p, [C.c_void_p, C.c_int64, C.c_float, C.POINTER(C.c_int)]),
        "hl_tnc_rx": (None, [C.c_void_p, C.c_void_p, C.c_int, C.c_int64]),
        "hl_tnc_poll_tx": (C.c_void_p, [C.c_void_p, C.c_int64, C.POINTER(C.c_int)]),
        "hl_send": (C.c_int, [C.c_void_p, C.c_char_p, C.c_int, C.c_char_p, C.c_int, C.c_int, C.c_int64]),
        "hl_pending_tx": (C.c_int, [C.c_void_p]),
        "hl_channel_busy": (C.c_int, [C.c_void_p]),
        "hl_get_stats": (None, [C.c_void_p, C.POINTER(C.c_uint32)]),
        "hl_get_fec_info": (None, [C.c_void_p, C.POINTER(C.c_uint32)]),
        "hl_get_meter": (None, [C.c_void_p, C.POINTER(C.c_float)]),
        "hl_tone_start": (None, [C.c_void_p, C.c_int]),
        "hl_tone_fill": (C.c_float, [C.c_void_p, C.c_void_p, C.c_int, C.c_float]),
        "hl_note_external_tx": (None, [C.c_void_p, C.c_int64, C.c_int]),
        "hl_report_summary": (C.c_int, [C.c_void_p, C.c_int, C.c_char_p, C.c_int]),
        "hl_set_monitor": (None, [C.c_void_p, C.c_int]),
        "hl_set_monitor_callback": (None, [C.c_void_p, MON_CB, C.c_void_p]),
        "hl_set_listen_all": (None, [C.c_void_p, C.c_int]),
        "hl_rx_modem": (C.c_int, [C.c_void_p]),
    }
    for name, (res, args) in sig.items():
        f = getattr(L, name)
        f.restype, f.argtypes = res, args
    return L


class Core:
    """One HamLink station context. Callbacks fire synchronously from inside the hl_* calls, on the calling thread."""

    def __init__(self, lib, callsign, relay=True, ttl=1, preamble_ms=500, seed=1,
                 on_message=None, on_status=None, on_event=None, on_monitor=None):
        self.L = lib
        self._on_message, self._on_status, self._on_event = on_message, on_status, on_event
        self._on_monitor = on_monitor
        # keep the ctypes callback objects alive for the lifetime of the context
        self._cbs = (MSG_CB(self._msg), ST_CB(self._st), EV_CB(self._ev))
        self._mon = MON_CB(self._monitor)
        self.c = lib.hl_create(callsign.encode(), 1 if relay else 0, ttl, preamble_ms, seed, *self._cbs, None)
        if not self.c:
            raise RuntimeError("hl_create failed (callsign?)")
        lib.hl_set_monitor_callback(self.c, self._mon, None)

    def _monitor(self, _u, src, dst, typ, msg_id, data, n):
        if self._on_monitor:
            self._on_monitor(src.decode(errors="replace"), dst.decode(errors="replace"), typ, msg_id,
                             C.string_at(data, n) if n > 0 else b"")

    def set_monitor(self, on):
        self.L.hl_set_monitor(self.c, 1 if on else 0)

    def set_listen_all(self, on):
        self.L.hl_set_listen_all(self.c, 1 if on else 0)

    def rx_modem(self):
        """Inside a message or monitor callback: the modem that decoded the frame (-1 elsewhere)."""
        return self.L.hl_rx_modem(self.c)

    def _msg(self, _u, src, dst, typ, data, n):
        if self._on_message:
            self._on_message(src.decode(errors="replace"), dst.decode(errors="replace"), typ, C.string_at(data, n))

    def _st(self, _u, msg_id, status):
        if self._on_status:
            self._on_status(msg_id, STATUS.get(status, str(status)))

    def _ev(self, _u, text):
        if self._on_event:
            self._on_event(text.decode(errors="replace"))

    def close(self):
        if self.c:
            self.L.hl_destroy(self.c)
            self.c = None

    # ---- settings
    def set_modem(self, name):
        return self.L.hl_set_modem(self.c, MODEMS[name])

    def set_link(self, link, tnc_baud=1200):
        self.L.hl_set_link(self.c, link, tnc_baud)

    def set_fec(self, check_bytes):
        return self.L.hl_set_fec(self.c, check_bytes)

    def set_carrier_sense(self, mode):
        self.L.hl_set_carrier_sense(self.c, CARRIER_SENSE[mode])

    def set_relay(self, on):
        self.L.hl_set_relay(self.c, 1 if on else 0)

    def set_clock(self, unix_s, now_ms):
        self.L.hl_set_clock(self.c, int(unix_s), now_ms)

    # ---- audio link
    def process(self, pcm, now_ms, busy_threshold):
        """pcm: a contiguous int16 numpy array (may be empty, which just runs the timers). Returns the input level."""
        if pcm is None or len(pcm) == 0:
            return self.L.hl_process_audio(self.c, None, 0, now_ms, busy_threshold)
        return self.L.hl_process_audio(self.c, pcm.ctypes.data, len(pcm), now_ms, busy_threshold)

    def poll_tx(self, now_ms, level):
        """A transmission to play now (int16 numpy array at 48 kHz), or None."""
        import numpy as np
        n = C.c_int()
        p = self.L.hl_poll_tx(self.c, now_ms, level, C.byref(n))
        if not p or n.value <= 0:
            return None
        return np.ctypeslib.as_array(C.cast(p, C.POINTER(C.c_int16)), (n.value,)).copy()

    # ---- TNC link
    def tnc_rx(self, data, now_ms):
        if data:
            buf = C.create_string_buffer(bytes(data), len(data))
            self.L.hl_tnc_rx(self.c, buf, len(data), now_ms)
        else:
            self.L.hl_tnc_rx(self.c, None, 0, now_ms)

    def tnc_poll_tx(self, now_ms):
        n = C.c_int()
        p = self.L.hl_tnc_poll_tx(self.c, now_ms, C.byref(n))
        return C.string_at(p, n.value) if p and n.value > 0 else None

    # ---- messages and state
    def send_text(self, dst, text, ack, now_ms):
        b = text.encode()
        return self.L.hl_send(self.c, dst.upper().encode(), TYPE_TEXT, b, len(b), 1 if ack else 0, now_ms)

    def pending_tx(self):
        return self.L.hl_pending_tx(self.c)

    def busy(self):
        return bool(self.L.hl_channel_busy(self.c))

    def stats(self):
        s = (C.c_uint32 * 8)()
        self.L.hl_get_stats(self.c, s)
        keys = ("rx_frames", "rx_dups", "tx_frames", "relayed", "relay_suppressed", "retries", "ofdm_ok", "ofdm_failed")
        return dict(zip(keys, list(s)))

    def meter(self):
        m = (C.c_float * 5)()
        self.L.hl_get_meter(self.c, m)
        return {"rms_dbfs": m[0], "peak_dbfs": m[1], "tone1200_dbfs": m[2], "tone2200_dbfs": m[3], "clipped": int(m[4])}

    def report_summary(self, data):
        out = C.create_string_buffer(256)
        n = self.L.hl_report_summary(data, len(data), out, 256)
        return out.value.decode(errors="replace") if n > 0 else f"{len(data)}-byte report"

    # ---- test tone
    def tone_start(self, mode):
        self.L.hl_tone_start(self.c, TONES[mode])

    def tone_fill(self, n, level):
        import numpy as np
        buf = np.zeros(n, np.int16)
        amp = self.L.hl_tone_fill(self.c, buf.ctypes.data, n, level)
        return buf, amp

    def note_external_tx(self, now_ms, duration_ms):
        self.L.hl_note_external_tx(self.c, now_ms, duration_ms)
