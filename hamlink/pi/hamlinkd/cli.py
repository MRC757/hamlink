# SPDX-License-Identifier: GPL-3.0-or-later
"""hamlinkd: the HamLink relay service.

  hamlinkd                     run the relay (what the systemd service does)
  hamlinkd --setup             create or edit the settings file interactively
  hamlinkd --list-devices      list sound cards and serial ports
  hamlinkd --status            ask the running service for its status
  hamlinkd --log [N]           the running service's last N log lines
  hamlinkd --send CALL TEXT    queue a text message (CQ for everyone)
  hamlinkd --test-tone [MODE]  transmit a 10 s test tone: alt (default), 1200, 2200, ptt
  hamlinkd --check             check that the core library loads (used by install.sh)
  hamlinkd --check-config      check the settings file
  hamlinkd --update            fetch the newest version and restart (runs install.sh again)
"""
import argparse
import json
import os
import signal
import subprocess
import sys
import threading

from . import control, settings as S


def main(argv=None):
    ap = argparse.ArgumentParser(prog="hamlinkd", description="HamLink relay service")
    ap.add_argument("--config", default=S.DEFAULT_PATH, help=f"settings file (default {S.DEFAULT_PATH})")
    ap.add_argument("--lib", default="", help="path to libhamlink_api.so (default: the installed copy)")
    ap.add_argument("--state-dir", default="/var/lib/hamlink" if sys.platform != "win32" else ".",
                    help="where recordings go")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--setup", action="store_true")
    g.add_argument("--list-devices", action="store_true")
    g.add_argument("--status", action="store_true")
    g.add_argument("--log", nargs="?", const=50, type=int, metavar="N")
    g.add_argument("--send", nargs=2, metavar=("CALL", "TEXT"))
    g.add_argument("--test-tone", nargs="?", const="alt", choices=["alt", "1200", "2200", "ptt"])
    g.add_argument("--check", action="store_true")
    g.add_argument("--check-config", action="store_true")
    g.add_argument("--update", action="store_true")
    a = ap.parse_args(argv)

    if a.setup:
        return setup(a.config)
    if a.list_devices:
        return list_devices()
    if a.check:
        from . import core
        path = core.find_library(a.lib)
        core.load(path)
        print(f"{path}: core API {core.API_VERSION}, OK")
        return 0
    if a.update:
        script = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "install.sh")
        return subprocess.call(["bash", script])
    cfg = S.load(a.config)
    if a.check_config:
        bad = cfg.problems()
        print("; ".join(bad) if bad else "settings OK")
        return 1 if bad else 0
    if a.status or a.log is not None or a.send or a.test_tone:
        req = ({"cmd": "status"} if a.status else {"cmd": "log", "n": a.log} if a.log is not None else
               {"cmd": "send", "to": a.send[0], "text": a.send[1]} if a.send else {"cmd": "test_tone", "mode": a.test_tone})
        try:
            rep = control.request(cfg.control_port, req)
        except OSError as e:
            print(f"hamlinkd is not running (port {cfg.control_port}): {e}", file=sys.stderr)
            return 2
        if not rep.get("ok"):
            print("error: " + rep.get("error", "?"), file=sys.stderr)
            return 1
        if a.log is not None:
            for e in rep["events"]:
                print(e["text"])
        elif a.status:
            print(json.dumps(rep["status"], indent=2))
        elif a.send:
            print(f"queued as message {rep['id']}")
        else:
            print("test tone started")
        return 0
    return run(cfg, a)


def run(cfg, a):
    from . import core, station
    bad = cfg.problems()
    if bad:
        print(f"{a.config}: " + "; ".join(bad) + " (run hamlinkd --setup)", file=sys.stderr)
        return 2
    lib = core.load(core.find_library(a.lib))
    log_file = open(cfg.log_file, "a", encoding="utf-8", buffering=1) if cfg.log_file else None

    def sink(line):
        print(line, flush=True)
        if log_file:
            log_file.write(line + "\n")

    audio = ptt = tnc = None
    if cfg.link == "audio":
        from . import audio as au, ptt as pt
        audio = au.SoundDeviceAudio(cfg.audio_in, cfg.audio_out)
        ptt = pt.make(cfg.ptt, cfg.ptt_port, log=sink)
    else:
        from . import tnc as tn
        tnc = tn.SerialTnc(cfg.tnc_port, log=sink)
    st = station.Station(cfg, lib, audio=audio, ptt=ptt, tnc=tnc, state_dir=a.state_dir, log=sink)
    srv = control.serve(st, cfg.control_port)
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    try:
        st.run(stop)
    finally:
        srv.shutdown()
        st.log("stopped")
        if log_file:
            log_file.close()
    return 0


def list_devices():
    try:
        from .audio import SoundDeviceAudio
        print("Sound devices (use part of the name for audio_in / audio_out):")
        for line in SoundDeviceAudio.list_devices():
            print("  " + line)
    except Exception as e:
        print(f"Sound devices: unavailable ({e})")
    try:
        from serial.tools import list_ports
        print("Serial ports (ptt_port, or tnc_port for a USB TNC):")
        for p in list_ports.comports():
            print(f"  {p.device}  {p.description}")
    except Exception as e:
        print(f"Serial ports: unavailable ({e})")
    if os.path.exists("/dev/rfcomm0"):
        print("Bluetooth TNC: /dev/rfcomm0 is bound")
    return 0


def setup(path):
    cfg = S.load(path)
    print(f"HamLink relay settings ({path}). Enter keeps the value in brackets.")

    def ask(name, prompt, conv=str):
        cur = getattr(cfg, name)
        v = input(f"{prompt} [{cur}]: ").strip()
        if v:
            setattr(cfg, name, conv(v))

    yes = lambda v: v.lower() in ("y", "yes", "1", "true", "on")
    ask("callsign", "Callsign", str.upper)
    ask("link", "Radio link: audio (DigiRig) or tnc (Bluetooth KISS TNC)")
    if cfg.link == "audio":
        ask("modem", "Modem: afsk, ofdm, mt63, psk2400, psk3600")
        ask("audio_in", "Audio input (part of the name, blank = default; see --list-devices)")
        ask("audio_out", "Audio output")
        ask("ptt", "PTT: rts (DigiRig), dtr, none (VOX)")
        if cfg.ptt != "none":
            ask("ptt_port", "PTT serial port (blank = the only USB serial port)")
        ask("tx_level", "Transmit level 0.01-1", float)
    else:
        ask("tnc_port", "TNC port")
    ask("relay", "Relay messages (y/n)", yes)
    ask("max_tx_percent", "Most of any hour this station may transmit, %", float)
    bad = cfg.problems()
    if bad:
        print("Not saved: " + "; ".join(bad))
        return 1
    S.save(cfg, path)
    print(f"Saved. Restart the service: sudo systemctl restart hamlinkd")
    return 0


if __name__ == "__main__":
    sys.exit(main())
