# hamlinkd: the HamLink relay service (Raspberry Pi)

Phase 1 of [docs/RASPBERRY_PI.md](../docs/RASPBERRY_PI.md): the HamLink core running as a headless relay under
systemd. It uses the same C core as the Windows and Android apps, so it relays for them on the same modem. The web
page (phase 2) comes next; for now the relay is set up and checked over SSH.

## Install

On Raspberry Pi OS Lite (64-bit), logged in over SSH:

```
curl -sSL https://raw.githubusercontent.com/MRC757/hamlink/master/hamlink/pi/install.sh | bash
sudo hamlinkd --setup
sudo systemctl start hamlinkd
```

Or clone the repository and run `hamlink/pi/install.sh` to read the script first. It installs the system packages,
creates a `hamlink` user (audio, serial and Bluetooth access), fetches the core library (a prebuilt ARM64 copy from the
GitHub release when one is attached, otherwise it builds it, which takes a while on a Zero 2 W), and installs the
`hamlinkd` command and the systemd service. Run it again, or `sudo hamlinkd --update`, to update; settings are kept.

## Use

| Command | |
|---|---|
| `sudo hamlinkd --setup` | callsign, radio link, modem, audio devices, PTT, relay, transmit limit |
| `hamlinkd --list-devices` | sound cards and serial ports, for the settings |
| `hamlinkd --status` | the running relay: levels, channel busy, queue, frames relayed, CPU temperature, under-voltage |
| `hamlinkd --log 50` | its last 50 log lines (all of it: `journalctl -u hamlinkd -f`) |
| `hamlinkd --send N0CALL "text"` | queue a message (`CQ` for everyone) |
| `hamlinkd --test-tone [alt\|1200\|2200\|ptt]` | transmit 10 s of test tone, to set `tx_level` against the other station's meter |

Settings are in `/etc/hamlink/hamlinkd.json` (see `hamlinkd/settings.py` for every key). After editing the file:
`sudo systemctl restart hamlinkd`.

### Radio links

* **DigiRig** (or another USB sound card): `link` `audio`, `ptt` `rts` (DigiRig) or `dtr`, or `none` for VOX.
  `audio_in` / `audio_out` take part of the device name from `--list-devices` (for example `USB Audio`); `ptt_port`
  can stay empty when the DigiRig's serial port is the only one. Every modem works.
* **Radio with a Bluetooth KISS TNC** (UV-PRO, VR-N76 and similar): pair it once with `bluetoothctl`, bind it to
  `/dev/rfcomm0` (`sudo rfcomm bind 0 <radio address>`), then `link` `tnc`. AFSK 1200 only; the radio does the keying.

### Safety limits

* `max_tx_seconds` (180): a longer transmission is dropped, not sent.
* `max_tx_percent` (25): the relay stops taking new transmissions once it has transmitted this share of the last hour.
* No transmission is started while the audio input is dead (sound card unplugged), so a broken receive path cannot
  leave the relay transmitting blind.
* PTT is released on every exit path, and the serial lines start unkeyed.

## Development

The package runs on any computer with the core library: `build/core/libhamlink_api.so` on Linux, or
`windows/native/hamlink_core.dll` on Windows (`HAMLINK_LIB` overrides). From `hamlink/pi`:

```
python -m pytest -q tests          # needs pytest and numpy
python -m hamlinkd --config ./hamlinkd.json
```

The tests run several stations in one process, in simulated time, over a simulated radio channel (`audio.Air`) or
a pair of in-memory TNCs: a relay between two stations out of range of each other (AFSK and PSK), the transmit limits,
the dead-input rule, the test tone, the TNC link, and the control port in real time. CI runs them on x86-64 and ARM64.

Layout: `core.py` (ctypes binding), `station.py` (the engine: receive, transmit sequence, limits, recording, status),
`audio.py` (sound card via PortAudio, simulated channel), `ptt.py`, `tnc.py`, `control.py` (localhost JSON-lines port
8737, which the CLI uses and the web page will), `cli.py`, `settings.py`.
