# HamLink on a Raspberry Pi: plan

Status: **plan, not started** (2026-11-03).

Target: an **unattended hilltop relay** on a **Raspberry Pi Zero 2 W**, set up and checked from a **web browser**, working
with a **DigiRig** (audio + serial PTT, every modem) or a **radio with a built-in Bluetooth KISS TNC** (AFSK 1200 only;
the radio does the modem). A relay re-sends what it hears so that stations that cannot hear each other can talk
(`docs/PROTOCOL.md`, relay and duplicate suppression).

## What carries over and what is new
* **Unchanged:** the C core. `core/CMakeLists.txt` already builds it on Linux as `libhamlink_api.so` (the same flat API the
  Windows app calls through P/Invoke and Android through JNI): all five modems, FX.25, ARQ, relay, station ID, reports,
  map model, the audio-path warning. The pytest suite already runs it on Linux.
* **New:** a front end. The Windows app (WPF, NAudio) and the Android app do not run on a Pi. It becomes a small
  **service** (`hamlinkd`) plus a **web interface** served by the Pi itself.

```
 Phone / laptop browser ──Wi-Fi (the Pi's own hotspot when you visit the hill)──► Pi Zero 2 W
                                         ┌──────────────────────────────────────────────┐
                                         │ hamlinkd (systemd service, starts at power-on)│
                                         │  ├ web UI: status, log, settings, test/cal.   │
                                         │  ├ link A: DigiRig: ALSA audio + serial RTS   │
                                         │  ├ link B: Bluetooth KISS TNC (rfcomm serial) │
                                         │  ├ watchdog, ID beacon, health checks         │
                                         │  └ libhamlink_api.so (unchanged core)         │
                                         └──────────────────────────────────────────────┘
```

**Language: Python 3** for the service, calling the core through ctypes (the tests' `tests/hamlink.py` and
`tools/replay_wav.py` already do this). The signal processing stays in C, so Python's speed does not matter at 20 ms audio
blocks, and Python is the quickest to build, change and debug on a Pi. Libraries: `sounddevice` (PortAudio/ALSA),
`pyserial`, `aiohttp` (web server and WebSocket); all in Raspberry Pi OS or pip.

## Hardware

| Part | Choice | Notes |
|---|---|---|
| Computer | **Raspberry Pi Zero 2 W** (4 x Cortex-A53, 512 MB, 2.4 GHz Wi-Fi, Bluetooth 4.2) | About 1 W idle. 512 MB is plenty for a Python service. **To check in phase 0:** MT63 and OFDM receive load (OFDM used 0.7% of one x86 core; the A53 is several times slower; MT63 has not been measured). |
| Radio interface A | **DigiRig Mobile** + Baofeng K1 cable set (DigiRig rev 1.6 or later) | USB sound chip (no drivers on Linux) and a CP2102 serial port whose **RTS** keys the radio. The Zero 2 W has one micro-USB OTG data port: needs a micro-USB OTG adapter (or a small hub). Must be an interface **without noise cancelling** (see the APRS-K1 PRO findings in the README). |
| Radio interface B | Radio with a built-in **KISS TNC over Bluetooth** (BTECH UV-PRO, Vero VR-N76) | No cable: paired over Bluetooth classic, appears as `/dev/rfcomm0`. AFSK 1200 only, every station on the channel must then use AFSK. The radio's own Bluetooth power saving needs the reconnect logic the other apps already have. |
| Storage | Industrial or high-endurance microSD; file system **read-only** (overlay) in service | A hilltop power cut must not corrupt the card. Settings and logs go to a small writable partition or RAM. |
| Clock | No battery-backed clock on the Pi. Use NTP when online, `fake-hwclock`, or a GPS (gpsd) | The relay itself does not need the time (IDs and retries use elapsed time), but log timestamps and any reports it originates do. |
| Power | 5 V, about 1-2 W for the Pi plus the radio | Plan the solar panel / battery for the radio's receive current; the Pi is the smaller load. |
| Enclosure | Shielded or at least separated from the antenna; ferrite chokes on USB | RF getting into the Pi or the USB audio is the classic relay-site failure (and a stuck transmitter). Set the radio's **TX time-out timer** as a backstop. |

## The service (`hamlinkd`)
* **Links:** `audio` (DigiRig: ALSA in/out at 48 kHz, PTT on serial RTS or DTR, or VOX) and `tnc` (KISS over
  `/dev/rfcommN` or any serial port, with backoff and reconnect). Later: CM108 GPIO PTT (DigiRig Lite) and a Pi GPIO pin.
* **Relay mode on by default**, with the hop limit from Settings; automatic station ID (already in the core).
* **Health:** restart on any exception; systemd `Restart=always` and the hardware watchdog; detects a vanished USB
  sound card or serial port and re-opens it; the core's audio-path WARNING and errors go to the log.
* **Safety:** a maximum transmit time per burst and per hour (stuck-carrier protection on top of the radio's time-out
  timer); refuses to transmit while the audio input is dead.
* **Record received audio** (off by default; on demand from the web page) to WAV, so `tools/replay_wav.py` works on
  recordings from the hill.
* **Settings** in one JSON file (same names as the Windows `settings.json` where they mean the same thing).
* **Command line:** `hamlinkd --status`, `--send TEXT`, `--test-tone`, for set-up over SSH.

## Web interface (phone browser, on the Pi's hotspot)
Kept small: a relay is configured on the hill once and then checked now and then.
* **Status:** link, audio level, channel busy, frames heard / relayed / suppressed, uptime, CPU temperature, supply
  voltage warnings (`vcgencmd get_throttled`), last error.
* **Log:** recent messages, relays and events, live (WebSocket).
* **Settings:** callsign, link (DigiRig or Bluetooth TNC: pick a paired radio), modem, PTT, TX level, hop limit, ID
  interval, Wi-Fi hotspot name and password.
* **Test / Calibrate:** PTT-only and tone tests and the receive meter, as in the other apps.
* **Help:** renders `docs/help/HELP.md`, as the apps do.
* **Not needed for a relay:** the map and report forms (a relay forwards reports without drawing them). They can come
  later from the same core API (map snapshot, report schemas) if a Pi field station is wanted.
* **Access:** password required; listens only on the Pi's hotspot/local network. Plain HTTP on a private hotspot to
  start; HTTPS with a self-signed certificate later.

**Field access:** the Pi runs a Wi-Fi access point (`hamlink-relay`). On the hill, join it with a phone and open
`http://hamlink.local` (or `http://10.42.0.1`). Optionally it can join a known Wi-Fi network instead when one is in range.

## Phases

| # | Phase | Delivers | Needs a Pi? |
|---|---|---|---|
| 0 | **Core on ARM64** | `libhamlink_api.so` built for aarch64 by GitHub Actions (native ARM64 runners) with the pytest suite run there; a benchmark script for every modem's receive and transmit load | No (benchmark: yes) |
| 1 | **Service, DigiRig link** | `hamlinkd` with audio, serial PTT, relay on, ID, logging, recording, settings, CLI, systemd unit, install script | Yes |
| 2 | **Bluetooth TNC link** | rfcomm pairing helper, KISS link with reconnect, tested against a UV-PRO-class radio | Yes + radio |
| 3 | **Web interface** | Status, log, settings, test/calibrate, Help, password | Yes |
| 4 | **Hilltop hardening** | Read-only root file system, hardware watchdog, transmit-time limits, access point, throttling and temperature alarms, an SD card image | Yes |
| 5 | **Field trial** | Two stations out of range of each other talking through the relay; logs and recordings from the hill | Yes + radios |

Later options: remote status over the radio itself (care needed: remote control of a station has its own rules),
KISS server for Dire Wolf / Pat, JS8Call bridge (needs a bigger Pi), map and reports for a Pi field station.

## Rules to keep in mind (US; check your own country's)
* An unattended relay is an **automatically controlled** station: it must identify, a control operator must be able to
  shut it down, and automatic control of digital stations is limited to certain frequencies. Pick the frequency with that
  in mind and keep a way to turn it off (the web page, the CLI, or power).
* The relay identifies itself (the core's automatic ID) and forwards traffic among licensed stations only; no encryption.

## Open questions
1. Measured receive load of MT63 and OFDM on the Zero 2 W (phase 0). If MT63 does not keep up, the relay offers it only
   on a Pi 3/4/5.
2. Whether the UV-PRO-class radios keep a Bluetooth link up for days, and how fast they reconnect after power saving.
3. Power budget and enclosure for the chosen site.
