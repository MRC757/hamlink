# hamlinkd: the HamLink relay service (Raspberry Pi)

`hamlinkd` runs the HamLink core as an unattended relay on a Raspberry Pi. It uses the same C core as the Windows and
Android apps, so it relays for them on the same modem. You set it up and check on it from a phone's browser. The plan
and its phases are in [docs/RASPBERRY_PI.md](../docs/RASPBERRY_PI.md).

**Status:** everything that can be built without a Pi is done and tested in CI. The CI checks run on GitHub's ARM64
Ubuntu machines: unit tests, a real `install.sh` run, and the running systemd service checked over HTTP. It has not
yet run on a Pi, a DigiRig or a Bluetooth radio.

## Two ways to install

### A. Ready-made SD card image (simplest)
1. Download `hamlink-relay-<version>.img.xz` from the GitHub release (or from the `pi-release` workflow's artifacts).
   Write it to a micro-SD card with Raspberry Pi Imager (*Use custom*).
   * Optional: in Imager's settings, add a user with SSH, for logging in from a PC.
   * Do **not** set Wi-Fi there: the relay makes its own hotspot.
2. Boot the Pi. After a minute or two it starts a Wi-Fi hotspot called **`hamlink-relay`**, password **`hamlink-setup`**.
3. Join it from a phone and open **http://10.42.0.1/** (or http://hamlink-relay.local/).
4. Choose the page's password. The first visitor sets it, so do this straight away.
5. Settings: enter the callsign and the radio link, then save. The radio starts.
6. Change the hotspot password over SSH: `sudo /opt/hamlink/src/hamlink/pi/harden.sh --hotspot <name> <password>`.

### B. Install script on your own Raspberry Pi OS Lite (64-bit)
```
curl -sSL https://raw.githubusercontent.com/MRC757/hamlink/master/hamlink/pi/install.sh | bash
```
Or clone the repository and run `hamlink/pi/install.sh`, if you want to read the script first. It:
* installs the system packages;
* creates a `hamlink` user with audio, serial and Bluetooth access;
* fetches the core library: a prebuilt ARM64 copy from the newest release that has one, otherwise it builds it, which
  takes a while on a Zero 2 W;
* installs the `hamlinkd` command and the systemd service, and starts it.

Then open `http://<the Pi's name>.local/` to finish setting up, or run `sudo hamlinkd --setup` over SSH.

Run the script again, or `sudo hamlinkd --update`, to update. Settings and the page password are kept.

For a hill, also run `sudo /opt/hamlink/src/hamlink/pi/harden.sh` (see *Hilltop hardening*).

## The web page

| Tab | |
|---|---|
| Status | Listening, busy or transmitting; input level; link and PTT state; frames heard, sent and relayed; airtime in the last hour; uptime; CPU temperature; supply-voltage warnings |
| Messages | Send a message (CQ, or to one station with an acknowledgement); messages received |
| Log | The relay's log, live |
| Settings | Callsign, link, modem, audio devices (offered from the devices present), PTT, transmit level, limits, recording. Saving restarts the radio. Also: change the page password, log out |
| Test | 10 s test tones (alternating, 1200 Hz, 2200 Hz, PTT only) and the receive meter, for setting levels |
| Bluetooth | Scan for and pair a radio with a KISS TNC, then use it with one tap |
| Help | The HamLink help (`docs/help/HELP.md`) |

**Security.**
* The page needs a password. It is stored as a PBKDF2 hash in `/etc/hamlink/web.json` (mode 600).
* A login session is an HttpOnly, SameSite=Strict cookie.
* Every change needs a header that another web site cannot make a browser send.
* Five wrong passwords lock the login for a minute.
* The page uses plain HTTP: keep it on the Pi's WPA2 hotspot or a network you trust.
* To reset a forgotten password over SSH: `sudo hamlinkd --set-password`.

## Command line (over SSH)

| Command | |
|---|---|
| `sudo hamlinkd --setup` | callsign, link, modem, devices, PTT, relay, transmit limit, asked one at a time |
| `hamlinkd --list-devices` | sound cards and serial ports |
| `hamlinkd --status` | the running relay's status (JSON) |
| `hamlinkd --log 50` | its last 50 log lines (everything: `journalctl -u hamlinkd -f`) |
| `hamlinkd --send N0CALL "text"` | queue a message (`CQ` for everyone) |
| `hamlinkd --test-tone [alt\|1200\|2200\|ptt]` | transmit a 10 s test tone |
| `sudo hamlinkd --pair-tnc` | scan for and pair a Bluetooth radio |
| `sudo hamlinkd --set-password` | set the web page password |
| `sudo hamlinkd --update` | update to the newest version |

The settings are in `/etc/hamlink/hamlinkd.json`; `hamlinkd/settings.py` lists every key. The service notices when the
file changes (from the web page, `--setup`, or an editor) and restarts the radio within a few seconds.

## Radio links

* **DigiRig** (or another USB sound card).
  * Settings: link `audio`, PTT `rts` (DigiRig) or `dtr`, or `none` for VOX.
  * Audio input and output take part of a device name (for example `USB Audio`).
  * The PTT port can stay blank when the DigiRig's serial port is the only one.
  * Every modem works.
* **Radio with a Bluetooth KISS TNC** (UV-PRO, VR-N76 and similar).
  * Pair it on the Bluetooth tab (or `sudo hamlinkd --pair-tnc`), then **Use**. This sets link `tnc` and TNC port
    `bt:<address>`.
  * The service connects straight to the radio over RFCOMM, so no `rfcomm bind` is needed.
  * If the radio is off or out of range, it tries again every 10 s.
  * A different channel can be given as `bt:<address>:<channel>` (the default is 1).
  * `/dev/rfcomm0` or a USB serial port also work.
  * AFSK 1200 only; the radio does the keying.

## Safety

* `max_tx_seconds` (180): a longer transmission is dropped, not sent.
* `max_tx_percent` (25): beyond this share of the last hour, the relay holds its queue.
* No transmission starts while the audio input is dead (a USB sound card unplugged).
* PTT is released on every exit path, and the serial lines start unkeyed.
* Watchdogs:
  * systemd restarts the service if its radio loop stalls for 30 s.
  * After `harden.sh`, the Pi's hardware watchdog reboots it if the whole system hangs.
* The log warns once when the supply voltage drops, the CPU throttles, or it passes 80 °C.
* A missing sound card or radio does not stop the service: the web page stays up, shows the error, and the radio is
  retried every 10 s.

## Hilltop hardening (`harden.sh`, run as root)

| Command | |
|---|---|
| `harden.sh` | hardware watchdog, logs in RAM only, no swap file, Wi-Fi country (`HAMLINK_COUNTRY`, default US), hostname `hamlink-relay`, and the hotspot `hamlink-relay` / `hamlink-setup` if none is set yet |
| `harden.sh --hotspot NAME PASSWORD` | change the hotspot |
| `harden.sh --client NAME PASSWORD` | also join a known Wi-Fi network; it is used instead of the hotspot whenever it is in range |
| `harden.sh --read-only` / `--writable` | turn read-only mode (an overlay in RAM) on or off; takes effect after a reboot |

**Read-only mode** protects the SD card from power cuts, but **every change is lost at the next reboot**: settings
from the web page, the password, updates. The settings page warns when it is on. Set everything up first, then turn
it on.

The SD card image has `harden.sh` applied already (not read-only mode).

## Development

The package runs on any computer that has the core library:
* `build/core/libhamlink_api.so` on Linux;
* `windows/native/hamlink_core.dll` on Windows;
* or the path in `HAMLINK_LIB`.

On Windows, the web page is on port 8080. From `hamlink/pi`:
```
python -m pytest -q tests          # needs pytest and numpy
python -m hamlinkd --config ./hamlinkd.json
```

**Tests.**
* `tests/test_hamlinkd.py`: three stations in one process, in simulated time, over a simulated radio channel
  (`audio.Air`) or a pair of in-memory TNCs. Covers a relay between two stations out of range of each other (AFSK and
  PSK), the transmit limits, the dead-input rule, the test tone, and the TNC link.
* `tests/test_web.py`: the web page over HTTP. Covers the first-run password, login protections, settings that start
  the radio, sending, the settings-file watcher, failure and retry, and Help.
* `tests/install_check.sh`: run by CI after a real `install.sh`, against the systemd service.

**Layout.**

| File | |
|---|---|
| `core.py` | ctypes binding |
| `station.py` | the radio engine: receive, the transmit sequence, limits, recording, status, health |
| `service.py` | keeps a station running; restarts it on settings changes and failures |
| `web.py` + `static/index.html` | the web page |
| `control.py` | localhost JSON-lines port 8737, used by the CLI |
| `audio.py`, `ptt.py`, `tnc.py`, `bt.py` | the hardware |
| `cli.py`, `settings.py` | the command line and the settings |

**Release files.** `.github/workflows/pi-release.yml` builds the ARM64 library and the SD card image (with
`build_image.sh`) and attaches them to each published release.
