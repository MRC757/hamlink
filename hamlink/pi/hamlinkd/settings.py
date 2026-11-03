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
    tnc_port: str = "/dev/rfcomm0" # Bluetooth (rfcomm) or USB serial port of the radio's KISS TNC
    tnc_baud: int = 1200           # the TNC's on-air speed, for airtime estimates
    # safety and housekeeping
    max_tx_seconds: float = 180.0  # longest single transmission allowed (MT63 messages are long)
    max_tx_percent: float = 25.0   # share of any hour this station may transmit
    record_rx: bool = False        # write received audio to WAV files (troubleshooting)
    record_dir: str = ""           # "" = <state dir>/recordings
    record_max_mb: int = 500
    control_port: int = 8737       # localhost control port (CLI now, web page later)
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
        return p


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
