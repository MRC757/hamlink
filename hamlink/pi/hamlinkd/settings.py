# SPDX-License-Identifier: GPL-3.0-or-later
"""Relay settings: one JSON file (default /etc/hamlink/hamlinkd.json on the Pi, ./hamlinkd.json elsewhere)."""
import dataclasses
import json
import os
import re
import sys

DEFAULT_PATH = "/etc/hamlink/hamlinkd.json" if sys.platform != "win32" else os.path.abspath("hamlinkd.json")


@dataclasses.dataclass
class Settings:
    callsign: str = ""
    link: str = "audio"            # "audio" (DigiRig / sound card + PTT) or "tnc" (radio with a KISS TNC)
    modem: str = "afsk"            # afsk, ofdm, mt63, psk2400, psk3600 (audio link); a TNC radio always does AFSK
    afsk_fec: int = 16             # FX.25 check bytes: 0, 16, 32, 64
    relay: bool = True             # this is a relay
    monitor: bool = False          # also show messages between other stations (Traffic list); off by default
    listen_all: bool = False       # run every modem's receiver, whatever modem is selected; off by default
    station_id: bool = True        # separate ID frames; off: only after relaying or a test tone (a relay still IDs)
    ttl: int = 1                   # relay hops for messages this station originates
    preamble_ms: int = 500         # lead-in after keying (covers the radio's PTT-to-RF delay)
    tx_level: float = 0.3          # transmit amplitude 0..1 (calibrate with --test-tone and the other station's meter)
    carrier_sense: str = "data_or_level"   # off, data, data_or_level
    busy_threshold: float = 0.004  # input RMS treated as "channel busy" by data_or_level
    bit_repair: bool = True
    # audio link
    audio_in: str = ""             # part of the input device name ("" = system default); see --list-devices
    audio_out: str = ""            # part of the output device name
    ptt: str = "rts"               # none (radio VOX / auto-PTT cable), rts, dtr
    ptt_port: str = ""             # serial port for RTS/DTR ("" = the only one present, e.g. /dev/ttyUSB0)
    # TNC link
    tnc_port: str = "/dev/rfcomm0" # bt:AA:BB:CC:DD:EE:FF (a paired radio), /dev/rfcommN, or a USB serial port
    tnc_baud: int = 1200           # the TNC's on-air speed, for airtime estimates
    # safety and housekeeping
    max_tx_seconds: float = 180.0  # longest single transmission allowed (MT63 messages are long)
    max_tx_percent: float = 25.0   # share of any hour this station may transmit
    record_rx: bool = False        # write received audio to WAV files (troubleshooting)
    record_dir: str = ""           # "" = <state dir>/recordings
    record_max_mb: int = 500
    control_port: int = 8737       # localhost control port (the CLI)
    web_port: int = 80 if sys.platform != "win32" else 8080   # the web page; 0 turns it off
    log_file: str = ""             # "" = log to the console / systemd journal only

    def problems(self):
        """Human-readable reasons the settings cannot run, or []."""
        p = []
        if not re.fullmatch(r"[A-Z0-9/-]{3,8}", self.callsign or "") or self.callsign == "CQ":
            p.append("callsign is missing or not valid (3-8 letters/digits)")
        if self.link not in ("audio", "tnc"):
            p.append("link must be audio or tnc")
        if self.modem not in ("afsk", "ofdm", "mt63", "psk2400", "psk3600"):
            p.append("unknown modem")
        if self.afsk_fec not in (0, 16, 32, 64):
            p.append("afsk_fec must be 0, 16, 32 or 64")
        if self.ptt not in ("none", "rts", "dtr"):
            p.append("ptt must be none, rts or dtr")
        if self.carrier_sense not in ("off", "data", "data_or_level"):
            p.append("carrier_sense must be off, data or data_or_level")
        if not 0.01 <= self.tx_level <= 1.0:
            p.append("tx_level must be 0.01..1")
        if self.link == "tnc" and self.tnc_port.startswith("bt:") and not BT_RE.fullmatch(self.tnc_port):
            p.append("tnc_port bt: needs a Bluetooth address, e.g. bt:38:D2:00:01:02:03 (optionally :channel)")
        if not 0 < self.max_tx_percent <= 100 or not 0 < self.max_tx_seconds <= 600:
            p.append("max_tx_percent must be 0..100 and max_tx_seconds 0..600")
        if self.ttl not in range(0, 8) or not 0 <= self.preamble_ms <= 3000:
            p.append("ttl must be 0..7 and preamble_ms 0..3000")
        return p


BT_RE = re.compile(r"bt:([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(:\d{1,2})?")

# For the web page's settings form: (name, label, choices or None, help). Fields not listed are file-only.
FORM = [
    ("callsign", "Callsign", None, "This relay's callsign; it is also sent as the station ID."),
    ("link", "Radio link", ["audio", "tnc"], "audio: DigiRig or another USB sound card. tnc: a radio's KISS TNC."),
    ("modem", "Modem", ["afsk", "ofdm", "mt63", "psk2400", "psk3600"],
     "Audio link only. Every station on the channel must use the same modem."),
    ("relay", "Relay", None, "Re-send messages for stations that cannot hear each other."),
    ("monitor", "Monitor all traffic", None, "Show messages between other stations on the Traffic tab. Never acknowledged."),
    ("station_id", "Send station ID", None,
     "Off: no separate ID after this station's own messages (they carry its callsign). A relay still identifies after "
     "relaying, since relayed messages carry the sender's callsign."),
    ("listen_all", "Listen on all modems", None,
     "Hear stations on any modem and relay each message on the modem it arrived on. Replies go out on the selected modem."),
    ("ttl", "Max relay hops", None, "For messages this station originates."),
    ("audio_in", "Audio input", None, "Part of the device name; blank for the default."),
    ("audio_out", "Audio output", None, "Part of the device name; blank for the default."),
    ("ptt", "PTT", ["rts", "dtr", "none"], "rts for a DigiRig; none when the radio uses VOX."),
    ("ptt_port", "PTT serial port", None, "Blank: the only USB serial port."),
    ("tx_level", "Transmit level", None, "0.01 to 1. Set with the test tone and the other station's meter."),
    ("preamble_ms", "PTT lead-in (ms)", None, "Raise it if the start of each transmission is cut off."),
    ("afsk_fec", "FX.25 check bytes", [0, 16, 32, 64], "AFSK error correction; 16 is the default."),
    ("carrier_sense", "Carrier sense", ["data_or_level", "data", "off"], "Wait for a clear channel before sending."),
    ("busy_threshold", "Busy level", None, "Input level treated as a busy channel (data_or_level)."),
    ("tnc_port", "TNC", None, "bt:<address> of a paired radio (see Bluetooth), /dev/rfcomm0, or a USB serial port."),
    ("tnc_baud", "TNC on-air speed", None, "1200 for AFSK 1200."),
    ("max_tx_seconds", "Longest transmission (s)", None, "Longer ones are dropped."),
    ("max_tx_percent", "Transmit share of an hour (%)", None, "The relay holds its queue beyond this."),
    ("record_rx", "Record received audio", None, "WAV files for troubleshooting (uses SD card space)."),
    ("record_max_mb", "Recording space (MB)", None, "Oldest files are deleted beyond this."),
]


def from_form(current, data):
    """A copy of current with the form's values applied and converted to each field's type."""
    s = dataclasses.replace(current)
    types = {f.name: type(getattr(current, f.name)) for f in dataclasses.fields(Settings)}
    for name, _, _, _ in FORM:
        if name not in data:
            continue
        v, t = data[name], types[name]
        if t is bool:
            v = v if isinstance(v, bool) else str(v).lower() in ("1", "true", "yes", "on")
        else:
            v = t(v)
        setattr(s, name, v)
    s.callsign = (s.callsign or "").strip().upper()
    return s


def load(path=DEFAULT_PATH):
    s = Settings()
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        known = {f.name for f in dataclasses.fields(Settings)}
        for k, v in data.items():
            if k in known:
                setattr(s, k, v)
    s.callsign = (s.callsign or "").strip().upper()
    return s


def save(s, path=DEFAULT_PATH):
    d = os.path.dirname(os.path.abspath(path))
    os.makedirs(d, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(dataclasses.asdict(s), f, indent=2)
    os.replace(tmp, path)
