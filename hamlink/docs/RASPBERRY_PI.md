# HamLink on a Raspberry Pi: plan

Status (2026-11-03): **phase 0 done except the measurement on a real Pi.** `.github/workflows/core-linux.yml` builds
`libhamlink_api.so` on GitHub's Linux x86-64 and ARM64 runners, runs the modem benchmark and the full test suite (88
passed on both). `tools/bench_modems.py` measures each modem's CPU load; on the ARM64 runner (a fast server core, not a
Pi) every modem's receiver used 0.8-1.4% of one core and decoded every test message. Next: run it on a Zero 2 W.

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

## If the Zero 2 W is not powerful enough
**Software: essentially no change.** The Pi Zero 2 W, Pi 3 A+, Pi 3 B+ and Pi 4 all run the same 64-bit Raspberry Pi OS
and the same aarch64 build of the core, so the service, its settings and the SD card image move across unchanged. The
differences are hardware only:
* **Pi 3 A+** (quad Cortex-A53 at 1.4 GHz, 512 MB): the middle step, about 40% faster than the Zero 2 W for under twice
  its power. Full-size USB-A port, so no OTG adapter for the DigiRig.
* **Pi 4** (quad Cortex-A72, several times faster): needs a 3 A, 5 V supply and about 3-4 times the Zero's power.
  Turning off HDMI and the LEDs saves a few tenths of a watt.
* The Pi 5 is not recommended for a solar site: more power for speed a relay does not need.

So the real cost of moving up is **power**: a bigger solar panel and battery (below).

## Power: solar panel and LFP battery
Estimates, to be checked with a USB power meter on the real hardware (phase 1). Assumptions:
* Radio: Baofeng-class handheld on an 8 V battery eliminator, **receive 0.6 W** (battery save off, which a relay needs),
  **transmit 12 W input** at 5 W RF. DigiRig 0.15 W. Pi averages: **Zero 2 W 1.0 W**, **Pi 3 A+ 1.8 W**, **Pi 4 3.5 W**,
  headless with the Wi-Fi hotspot on.
* Traffic: **light** = the relay transmits 1% of the time (about 15 min a day), **busy** = 5% (about 72 min a day).
* DC-DC converters 87% efficient (12.8 V battery to 5 V for the Pi, to 8 V for the radio).
* Battery: **LFP (LiFePO4), 12.8 V**, 85% of capacity used, covering **3 days without sun** (5 days for a site that is
  hard to reach).
* Solar: **2.5 peak sun hours** (a December day at mid-US latitudes, about 35-40 N, panel tilted for winter), 75% system
  efficiency (charge controller, temperature, dirt, wiring), and a 1.5 x panel margin so the battery refills after a run
  of cloudy days.

| Computer | Traffic | Average draw | Energy per day | LFP battery, 3 days | LFP battery, 5 days | Solar panel |
|---|---|---|---|---|---|---|
| Pi Zero 2 W | light | 1.9 W | 52 Wh | 14 Ah | 24 Ah | 41 W |
| Pi Zero 2 W | busy | 2.4 W | 65 Wh | 18 Ah | 30 Ah | 52 W |
| Pi 3 A+ | light | 2.7 W | 74 Wh | 20 Ah | 34 Ah | 59 W |
| Pi 3 A+ | busy | 3.2 W | 87 Wh | 24 Ah | 40 Ah | 70 W |
| Pi 4 | light | 4.4 W | 121 Wh | 33 Ah | 55 Ah | 96 W |
| Pi 4 | busy | 4.9 W | 134 Wh | 37 Ah | 62 Ah | 107 W |

**Suggested equipment** (standard sizes, busy relay):

| Computer | LFP battery (12.8 V) | Solar panel | Charge controller |
|---|---|---|---|
| Pi Zero 2 W | 20 Ah (3 days) or 30 Ah (5 days) | 50-60 W | 10 A, LFP charge profile |
| Pi 3 A+ | 30 Ah or 40 Ah | 70-80 W | 10 A |
| Pi 4 | 40 Ah or 60-100 Ah | 100-120 W | 10-15 A |

**Recommendation: build the site for the Pi 4 from the start (100-120 W panel, 50 Ah LFP).** It is about twice the cost
of the Zero's power system, but it runs a Zero 2 W relay for about 8-10 days without sun (544 Wh usable at 52-65 Wh a
day), and moving to a Pi 4 later is then just a board swap on the hill, with no new power hardware.

Notes for the site:
* **LFP must not be charged below 0 C (32 F).** On a winter hilltop use a battery whose BMS blocks cold charging, or a
  self-heating LFP battery, or a charge controller with a temperature sensor.
* Set the controller's **low-voltage disconnect** (about 12.0 V for LFP) with automatic reconnect, so a flat battery
  shuts the load off cleanly and the site comes back by itself when the sun returns. The read-only file system (phase 4)
  makes the sudden power cut harmless to the Pi.
* Higher transmit power changes the numbers a lot: at 5 W RF the radio's transmit share is already the biggest single
  item on a busy relay. Low power (1 W RF, about 5 W input) cuts it by more than half where the path allows.
* The figures for the radio come from typical handheld specifications, not measurements of your radio; measure receive
  and transmit current at the site's supply voltage before buying.

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
3. Measured power of the actual radio and Pi (the sizing above uses typical figures), and the enclosure for the site.
