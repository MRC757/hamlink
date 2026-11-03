# HamLink

Text and voice-clip messaging over cheap handheld VHF/UHF FM radios (Baofeng UV-5R class), using an
Android phone **or a Windows 11 PC** as the modem and a DigiRig for audio + PTT. Includes a simplex store-and-forward relay
mode: leave a phone + radio on a hilltop and stations that can't hear each other talk through it.

> **Review build 0.2.2 (2026-11-02).** Licence: GPL-3.0-or-later (see *Licence* below). The C core is tested on a Linux host.
> The **Windows app runs on Windows 11** and the **Android app runs on a phone**; the two have exchanged messages, reports
> and relayed traffic by acoustic coupling, and MT63 messages over **Baofeng radios** (phone to laptop). The radio test
> used a noise-cancelling USB-C cable that only MT63 survives; AFSK, PSK and OFDM over radio still need testing with a
> DigiRig or plain sound card.
> Downloads for both are on the [Releases](https://github.com/MRC757/hamlink/releases) page. Crashes and errors, with the
> exact error text, are the most useful feedback at this stage.

* Two ways to reach the radio: **audio + PTT** (a DigiRig or sound card, works with any FM handheld) or a
  **KISS TNC over Bluetooth/serial** (radios with a built-in TNC, e.g. BTECH UV-PRO / Vero VR-N76, or a
  Bluetooth KISS TNC). In TNC mode the radio does the modulation, keying and carrier sense.
* Five modems on the audio link, chosen in Settings (every station on a channel must use the same one). First three:
  **AFSK 1200 baud** (Bell 202 tones + HDLC; the default), **OFDM** (COFDMTV, the modem from the
  Rattlegram app) and **MT63-2000** (the fldigi mode, long interleave). Measured in simulation: OFDM decodes about
  **4-6 dB deeper into the noise** than AFSK (100% of packets at 8 dB SNR, where AFSK frames were 55%) but it is
  **not faster**: on a clean link it used about 20% more airtime than AFSK for the same message, because every ACK burst
  carries OFDM lead-in symbols. **MT63-2000 is slow but robust**: a short message took 10.5 s on the air against 1.5 s
  for AFSK, and every transmission spends ~5 s on receiver lock-in and interleaver flush, but it delivered 26-30 of 30
  messages from +6 down to -4 dB SNR, where AFSK with FX.25 delivered none below +6 dB. Choose AFSK for speed and
  packet-radio interoperability, OFDM or MT63 for weak or noisy paths, MT63 when getting through matters more than time.
* Two more for speed: **PSK 2400** (QPSK) and **PSK 3600** (8-PSK), HamLink's own 1200-baud single-carrier PSK with an
  adaptive equaliser and Reed-Solomon per frame (`docs/PROTOCOL.md`). Over a simulated handheld audio path a 1000-byte
  acknowledged message took 8.0 s and 6.4 s, against 15.2 s on AFSK with FX.25. PSK 2400 was as robust as AFSK with
  FX.25 (everything delivered down to 6 dB SNR); PSK 3600 needs about 10 dB. PSK 2400 and 3600 stations hear each other.
* **FX.25 error correction on AFSK** (on by default, 16 Reed-Solomon check bytes; 32 or 64 or off in Settings): repairs up
  to 8/16/32 damaged bytes per frame, and stations without FX.25 still read frames that arrive intact. In simulated noise
  at 5-6 dB SNR it raised delivery from 8-64% to 75-99% with 16 check bytes, at about 25% more airtime per data frame.
* Selective-repeat ARQ, flood relay with duplicate suppression, CSMA, automatic station ID.
  Format: `docs/PROTOCOL.md`.
* Voice is **store-and-forward clips** (Codec2 700C, 100 B/s), not live voice: 1200 bps can't carry
  real-time voice once framing/ARQ overhead is added. A 5 s clip is ~500 bytes, about 5.5 s of airtime direct,
  about 12 s round trip through one relay.
* **Position reports and SALUTE situation reports on an OpenStreetMap-style map**: positions from device GPS, typed
  lat/lon or a Maidenhead grid square; SALUTE icons take the colour of the reporter's assessment, station icons the
  colour of that station's latest SITREP condition, and both fade as they age; map tiles are cached on the device. **Offline map files** (raster MBTiles) work with no internet;
  `tools/make_mbtiles.py` builds one for your area from USGS topo maps or your own tile server (`docs/OFFLINE_MAPS.md`).
  Report types are data (`schemas/*.json`), so adding one is a JSON file.
  Optional automatic repeat of your broadcast reports (off by default; receivers show each report once).
  To report on a spot: right-click it on the Windows map, long-press it on Android, or use *Pick on map* in the form.
* **Station SITREP**: an eight-field drop-down status report about your own station and area (conditions, home/
  mobile/portable, power, water, sanitation, medical, grid communications, transportation), about 4 bytes on the air.
  See `docs/MAPS_AND_REPORTS.md` and `docs/EXTENDING.md`.
* **Built-in Help** (Windows *Help*, Android *?*): which equipment each modem needs, offline maps, and troubleshooting,
  from `docs/help/HELP.md`. The core also warns in the log when the audio input is suppressing received signals.
* Amateur radio only: callsign required, no encryption, publicly documented protocol.

## For reviewers and testers
Pick whichever track matches what you have. Each one is useful on its own.

| Track | You need | Do this | What to report |
|---|---|---|---|
| 1. Core tests | Linux or macOS, Python 3, cmake, gcc | `python3 -m pip install -r tests/requirements.txt`, then `cd tests && python3 -m pytest -q` (about 3 minutes) | Any failure, with the output |
| 2. Windows app | Windows 10/11 | Unzip the download from [Releases](https://github.com/MRC757/hamlink/releases), or build with the .NET 8 SDK (internet once for NuGet): `powershell -File windows/build.ps1`, then run `windows/publish/HamLink.exe` | Compiler errors (copy the full text), crashes at start, anything on screen that looks wrong |
| 3. Android app | An Android phone (8.0+) | Install the APK from [Releases](https://github.com/MRC757/hamlink/releases), or build it with `android/gradlew assembleDebug` (or open `android/` in Android Studio) | Crashes (logcat), permission prompts that misbehave, anything on screen that looks wrong |
| 4. Two-device test, no radio | Track 2 or 3 working on two devices | `docs/TESTING.md` Stage 0, then Stage 1 (acoustic coupling) and Stage 1b (map and reports) | Whether messages, ACKs, positions and SALUTE reports arrive; retry counts; how the map behaves |
| 5. On the air | Licensed operator, radios, an interface (see *Radio interfaces* below) | `docs/TESTING.md` Stage 2/3 | TX level found, PTT lead-in needed, success rate, distance |

Code review is welcome anywhere. The places most likely to need a second pair of eyes are listed under
*Known issues and open questions* at the end of this file.

## Modes you can select
All of these are user settings (Settings screen); the active combination is shown on the main screen, and
the modem can be switched live there for A/B tests. **Start here: `docs/TESTING.md`**, a staged
plan from two devices with no radio, through acoustic coupling with a hand-pressed PTT, to a DigiRig and a
Bluetooth radio.
* **Radio link:** Audio + PTT, or Bluetooth/serial KISS TNC.
* **Modem** (audio link): AFSK 1200 (with FX.25 off/16/32/64), OFDM, MT63-2000, PSK 2400 or PSK 3600.
* **PTT** (audio link): serial RTS, serial DTR, none / radio VOX (transmit immediately), or **Manual**
  (you press the radio's PTT when the app counts down).
* **Audio:** a USB audio interface (DigiRig) or the phone/PC's own speaker and microphone (acoustic coupling).
* **Carrier sense:** *Data or loud audio* (default), *Data only* (ignores room noise and voice; for acoustic tests), or *Off*.
* **Bit repair** of received frames (default on), PTT lead-in, TX level, relay mode and hop limit.
* **Map:** position source, beacon interval, report repeats, tile server, an offline map file. **Appearance** (Android):
  light, dark or the phone's setting. **Record received radio audio** (Windows) to WAV files for troubleshooting.

## Layout
```
core/            portable C/C++: afsk.c hdlc.c fx25.c (AFSK modem, FX.25) ofdm.cpp (COFDMTV wrapper) mt63.cpp
                 psk.c (PSK 2400/3600) mesh.c (ARQ+relay+ID) tnc.c (KISS + AX.25 UI) voice.c (Codec2) tone.c (test
                 tone + RX meter) audiocheck.c (warns about inputs that suppress signals) hamlink_api.c (flat API)
third_party/codec2  trimmed Codec2 (LGPL-2.1), pre-generated codebooks, no host tools needed
third_party/aicodix vendored Rattlegram COFDMTV modem (permissive 0BSD-style license), one small patch
third_party/mt63    vendored fldigi MT63 modem (GPL-3.0-or-later), initialisation fixes listed in its README
android/         Kotlin + Compose app, JNI bridge, foreground service, DigiRig PTT over USB serial,
                 map (MapView.kt, Tiles.kt, MapPack.kt), position sources (Positions.kt), schema-driven report form,
                 Help (Help.kt)
windows/         C# .NET 8 WPF app, P/Invoke to hamlink_core.dll (core/hamlink_api.c), NAudio audio (raw WASAPI
                 capture: RadioCapture.cs), COM-port PTT, map (MapView.cs, Tiles.cs, MapPack.cs, MapPanel.cs),
                 position sources, report form, Help, self-contained publish for air-gapped PCs. See windows/README.md
schemas/         report types as JSON (position, SALUTE, SITREP): fields, choices, colours, ageing. tools/gen_schemas.py
                 turns them into core/schemas_gen.[ch]; core/report.c, mapmodel.c, geo.c, nmea.c do the rest
docs/            PROTOCOL.md, TESTING.md (staged bring-up), MAPS_AND_REPORTS.md, EXTENDING.md, OFFLINE_MAPS.md,
                 DSP_IMPROVEMENTS.md (receiver research, results), ROADMAP.md (deferred work), help/HELP.md (in-app Help)
tools/           gen_schemas.py (schemas -> C tables), syntax_check.py (Kotlin/C# parse check),
                 decode_wav.py / score_tnc_cd.py (score the demodulator on a recording, e.g. the WA8LMF TNC Test CD),
                 replay_wav.py (replay a recording through every modem), make_mbtiles.py (build an offline map file)
tests/           pytest: modem sweep, ARQ, relay, ID timing, voice, geodesy, reports, map; simulated FM audio
                 channel; report_test.c (C suite run under AddressSanitizer/UBSan)
```

## Run the tests (host)
```
python3 -m pip install -r tests/requirements.txt   # plus cmake and gcc
cd tests && python3 -m pytest -q        # about 3 minutes
python3 sweep.py                        # modem frame-success vs SNR table
python3 tools/gen_schemas.py --check    # from the repo root: generated report tables up to date
python3 tools/syntax_check.py           # Kotlin and C# syntax (no compiler)
```
On Windows, `py tools/score_tnc_cd.py TRACK.flac` scores the receiver in the shipped `hamlink_core.dll` on any WAV/FLAC
recording (needs numpy, scipy and soundfile; no compiler). Results on the WA8LMF TNC Test CD: `docs/DSP_IMPROVEMENTS.md`.
`py tools/replay_wav.py FILE` replays a recording (for example one the Windows app saved with *Record received radio
audio*) through every modem's receiver and lists the messages and reports that decode.

## Build the apps
**Windows:** see `windows/README.md` (`windows/build.ps1`, or `dotnet publish`; builds with the .NET 8 or 9 SDK).
VS Code: open the repository root; *Terminal > Run Build Task* builds, F5 runs under the debugger (C# Dev Kit extension).
A prebuilt x64 `hamlink_core.dll` is checked in at `windows/native/`; `windows/build-native.sh` rebuilds it with zig
(`pip install ziglang`) on Linux, macOS, or Windows in Git Bash. MSVC cannot build it: Codec2 uses C99 variable-length arrays.

**Android:** `cd android && ./gradlew assembleDebug` (Windows: `gradlew.bat assembleDebug`) builds
`app/build/outputs/apk/debug/app-debug.apk`; or open `android/` in Android Studio. Needs JDK 17 and an Android SDK with
platform 35, build-tools 35, CMake 3.22.1 and NDK 27.0.12077973 (point `android/local.properties` at it with `sdk.dir=`).
The Gradle wrapper (8.10.2) is in the repository. Plugins: AGP 8.7.3, Kotlin 2.0.21, Compose BOM 2024.12.01, minSdk 26,
targetSdk 35.
Requires a phone with USB-C host mode (OTG).

## Radio interfaces
The modem needs clean, unprocessed audio in and out of the radio, so the choice of interface is about **how the radio is
keyed** and whether its sound chip **alters received audio**. Only the APRS-K1 PRO has been tried with HamLink so far
(2026-11-02, see *Status*); the rest are specs from the vendors' pages.

| Interface | Keying | HamLink settings | Notes |
|---|---|---|---|
| None: acoustic coupling | You press PTT | PTT = Manual, USB audio off, carrier sense *Data only* | Stage 1 test only. Slow and noisy, but needs nothing. |
| [BTECH APRS-K1](https://baofengtech.com/product/aprs-k1/) (3.5 mm TRRS to K1, about $22) | Radio VOX | PTT = None / radio VOX, USB audio off, AFSK | No PTT line. VOX attack delay is covered by the 500 ms lead-in (raise to 700-800 ms if the start is clipped). Needs a headset jack or a USB-C headset adapter. |
| [BTECH APRS-K1 PRO](https://baofengtech.com/product/aprs-k1-pro/) (one-piece USB-C to K1, about $34) | Keys itself when it hears audio ("Auto PTT", sensitivity dial) | PTT = None / radio VOX (the radio's own VOX **off**), USB audio on, **MT63** | **Tested:** transmits cleanly, but its USB chip has built-in noise cancelling that cuts received signals by about 20 dB after 0.7 s and cannot be turned off, so only MT63 decodes (radio volume mid range; full volume clips). Fine as a transmit-only cable. Set the dial with a tone test (*PTT only* will not key it); reviews report RF pickup, so use ferrite chokes. |
| DigiRig Mobile (+ its Baofeng K1 cable set, DigiRig rev 1.6 or later) | Software, serial RTS | PTT = RTS, USB audio on | Recommended: no audio processing, and the app controls keying. Best choice for an unattended relay. Next to be tested. |
| Radio with a built-in KISS TNC (BTECH UV-PRO, Vero VR-N76) | The radio | Radio link = Bluetooth/KISS TNC | No audio cable. See the TNC section below. |

Speed is set by the radio's audio path (roughly 300-3000 Hz, pre-emphasis, limiter), not the cable: 9600 baud does not
pass a handheld's microphone input. **Avoid USB-C headset-style cables and adapters with noise cancelling** ("ENC", "call
noise reduction") for receiving: they wreck every modem but MT63, and the log shows a WARNING when HamLink detects one.
On Windows, set the radio interface's microphone level to about 50-60 (100 overloads many USB interfaces).

## Wiring and bring-up (Baofeng + DigiRig Mobile)
1. DigiRig Mobile -> Baofeng cable (Kenwood 2-pin K1). Plug DigiRig into the phone's USB-C port.
2. Radio: VOX **off** (PTT comes from the DigiRig's RTS line; if your DigiRig is set up for DTR, pick DTR in Settings),
   squelch up enough that the speaker is silent with no signal, wide bandwidth if the menu offers it,
   start on low power. Set the radio volume so the app's level bar is near zero when idle and rises
   clearly on a signal.
3. App: Settings -> callsign, PTT line (RTS or DTR), Start. The header shows the audio route
   ("USB ...") and PTT status. If it says "phone mic/speaker", Android didn't expose the USB audio device.
4. **Check PTT, then calibrate TX level** (most important). Press **Test** (Android) or
   **Test / Calibrate** (Windows) after Start:
   * *PTT only* keys the radio with silence for a few seconds: confirms the RTS/DTR line and polarity.
   * Tone modes (1200 Hz, 2200 Hz, alternating) send a steady test tone at the level on the slider;
     the slider works live while the tone is on the air. Each test lasts at most 15 s, will not start if
     the channel is busy, always unkeys, and is followed by your callsign ID.
   * The receive meter on the *other* station shows peak/RMS, the 1200 and 2200 Hz tone levels in dBFS
     with peak hold, the twist between them, and an input-clipping warning.
   * Procedure: B watches the meter; A sends the alternating tone from a low level, raising it ~3 dB
     per step with a 2 s pause. Where B's reading stops rising, the radio's limiter is engaged; set A
     about 6 dB (half) below that and press *Use as TX level*. A cheap radio's knee may be soft,
     so confirm with a real message exchange, and repeat if you change wide/narrow bandwidth.
   Over-driving distorts the tones; under-driving costs range. Typical values are 0.1-0.4.
   Calibration is manual by design: one station cannot observe its own deviation.
5. Relay: turn on the Relay switch on the hilltop phone. It re-sends what it hears and IDs itself.

## Bluetooth / serial KISS TNC mode (radios with a built-in TNC)
No audio cable, DigiRig or PTT line: the radio's TNC modulates, keys up and does carrier sense; HamLink
exchanges KISS frames with it. Each mesh frame is sent as a standard AX.25 UI frame (destination `HAMLNK`),
so the on-air packets are valid AX.25 that other packet tools can see; other APRS/packet traffic that the
TNC passes up is ignored.
1. On the radio, enable KISS TNC (BTECH UV-PRO / Vero VR-N76: Settings > General Settings > KISS TNC).
   While it is on, the vendor's own programming app cannot connect.
2. Pair the radio with the phone/PC in the operating system's Bluetooth settings.
3. **Android:** Settings > Radio link > Bluetooth TNC, grant the Bluetooth permission, tick your radio, set the
   TNC speed. **Windows:** Settings > Radio link > KISS TNC and pick the *outgoing* virtual COM port that
   Windows created for the paired radio.
4. Press Start. The status line shows the connection; it retries with backoff, because the radio power-saves
   its classic Bluetooth and the first attempts can time out.
Notes: PTT test and calibration do not apply (there is no audio path). The radio's own TX delay and CSMA
settings apply. ACK timing assumes the TNC speed you select. Any KISS-over-Bluetooth-SPP TNC should work the
same way; only the BTECH-style radios are documented here, and none of this has been tried on real hardware.

## Raspberry Pi hilltop relay (planned)
An unattended relay on a **Raspberry Pi Zero 2 W**: the unchanged core under a small Python service, set up from a phone
browser on the Pi's own Wi-Fi hotspot, with a DigiRig or a Bluetooth KISS TNC radio. Full plan: `docs/RASPBERRY_PI.md`.
* **If the Zero 2 W is too slow** (MT63's load is still to be measured), a Pi 3 A+ or Pi 4 runs the **same software
  and SD card image**; only the power system grows.
* **Solar and LFP battery** (estimates; a Baofeng-class radio at 5 W, a busy relay transmitting 5% of the time, 3 days
  without sun, 2.5 winter sun hours):

| Computer | Average draw | Energy per day | LFP battery (12.8 V) | Solar panel |
|---|---|---|---|---|
| Pi Zero 2 W | 2.4 W | 65 Wh | 20 Ah (30 Ah for 5 days) | 50-60 W |
| Pi 3 A+ | 3.2 W | 87 Wh | 30 Ah (40 Ah) | 70-80 W |
| Pi 4 | 4.9 W | 134 Wh | 40 Ah (60-100 Ah) | 100-120 W |

  Building the site for the Pi 4 from the start (**100-120 W panel, 50 Ah LFP**) costs about twice the Zero's power
  system, runs a Zero relay about 8-10 days without sun, and makes a later move to a Pi 4 a board swap. LFP must not be
  charged below freezing: use a battery or controller with cold-charge protection on a winter hilltop. Assumptions and
  the full table: `docs/RASPBERRY_PI.md`.

## Status: what was and wasn't verified
Verified here (host build, 88 pytest tests plus a sanitizer-built C suite, all against the flat C API or the mesh layer through a simulated
FM audio channel or a simulated byte link):
* AFSK: 100% of frames decode down to ~12 dB SNR in a 2.7 kHz band through a model with 300-3000 Hz
  band-limiting, 6 dB spectral tilt and +/-300 ppm clock error (~97% at 10 dB, ~55% at 8 dB).
* OFDM: 100% of packets down to 8 dB SNR, 96-100% at 6 dB, 0% at 4 dB (same channel model); 4 back-to-back
  packets in a burst decode with the tail symbol trimmed; end-to-end text with ACK, a 500 B voice clip through
  a hidden-node relay, and a marginal 7 dB link where OFDM delivers and AFSK needs more retries or fails.
  Receive CPU on this x86 host: OFDM 0.7% of one core, AFSK 0.2%. Not measured on a phone.
* Mesh: fragmentation, ARQ with 35% frame loss, relay, duplicate suppression, ID within 10 minutes, both
  fragment size classes interoperating, unknown classes ignored, callsign sanitising.
* TNC link: KISS escaping with hostile payloads and random byte chunking, AX.25 header checked by an
  independent parser, foreign APRS/command frames ignored, airtime pacing, relay and ACK over the byte link.
* Manual-PTT timing: pending-frame count, ACK waits stretched to 45 s, preserved across modem/link changes.
* Codec2 700C voice clips, test-tone generator and receive meter, station ID after a test transmission.
* Receive DSP: single-bit repair raises frame success at 8 dB SNR from 56% to 76% and at 9 dB from 88% to 97%, with no
  wrong frames in 1,200 heavily damaged frames or 20 minutes of noise; data carrier detect stays off for noise, voice-like
  audio and steady tones, and comes on within 20 ms of data.
* Receive DSP on real audio (WA8LMF TNC Test CD, measured on Windows with the shipped DLL): with the 900-2500 Hz prefilter
  and two slicers, 971 error-free frames from the de-emphasised traffic track (608 before) and 1001 from the flat one; no
  frames from 60 minutes of noise or the calibration-tone tracks. The pytest suite has not been re-run since this change.
* FX.25 (Windows, through the C API): Reed-Solomon output identical to an independent implementation, corrections and
  refusals checked on 3,000 blocks, backward compatibility with the previous release, acknowledged messages, and delivery
  in simulated noise (`docs/DSP_IMPROVEMENTS.md` step 5). Not tested against Dire Wolf or UZ7HO, or on a real radio.
* MT63-2000 (Windows, through the C API, white noise): broadcasts 10/10 on a clean channel; acknowledged two-fragment
  messages 2/2; 30/30 messages from +6 to -2 dB SNR and 26/30 at -4 dB, none at -6 dB. Selected and started in the
  Windows app. Not tested against fldigi on real audio, on a real radio, or on a phone.
* PSK 2400 / 3600 (Windows, through the C API, over the handheld audio model: 300-3000 Hz, 0-12 dB treble loss, +-300 ppm
  clock): every frame delivered at 12 dB and above in all combinations; acknowledged 1000-byte messages; a PSK 3600
  sender heard by a PSK 2400 receiver; selected and started in the Windows app. Not tested on a real radio, whose FM
  discriminator, limiter and squelch the model does not include, or on a phone.
* Geodesy: Maidenhead encode/decode checked against exact rational arithmetic (9000 random points), UTM/MGRS against
  `pyproj` and the `mgrs` package (a handful of points on exact grid-line ties differ by one metre), NMEA checksums and fix rules.
* Reports and map: bit-packed codec round trips, size limits, every error path, a 200,000-case fuzz run under
  AddressSanitizer/UBSan (it found and fixed a real truncated-input bug), generated tables checked for staleness, icon colours,
  station tint, ageing/expiry, grid-square uncertainty rings, and end-to-end delivery over the simulated radio channel directly
  and through a relay (unknown future report types are relayed and ignored by the map).

Windows app (2026-11-01, one Windows 11 PC, no radio attached): compiled with the .NET 9 SDK and run. The first build
needed fixes (missing usings, an NAudio API mismatch, the App.xaml entry point, and the native DLL renamed from
`hamlink.dll` to `hamlink_core.dll` because it collided with `HamLink.dll` on Windows' case-insensitive file system).
Exercised by hand: start-up, Settings, a grid-square position, OpenStreetMap tiles downloading and drawing, pan/zoom,
Start/Stop, sending text with manual PTT, the modem switch, and a SALUTE report started by right-clicking the map.
At start-up the app checks `hamlink_core.dll` and its API version and stops with a clear message if it is missing or
mismatched; errors are written with stack traces to `%LOCALAPPDATA%\HamLink\error.log`. The DLL (zig, C and C++17, API
version 10) imports only the Universal CRT and KERNEL32. Since then (0.2.1-0.2.2, by hand): offline map files (a USGS
Topo sample drew, with tiles enlarged past its deepest zoom), Help, the Transmit panel in the Map view, SITREP station
colours, and raw WASAPI capture of a USB radio interface.
Android app (2026-11-02): built for the first time, with JDK 17, AGP 8.7.3 and NDK 27, into a 13 MB debug APK for
arm64-v8a, armeabi-v7a and x86_64. It compiled with no Kotlin errors or warnings, including the NDK build of the C++
OFDM and MT63 modems, and all 38 Kotlin `external` functions match the JNI library's exports. It offers the same five
modems and FX.25 setting as Windows.

On-air by acoustic coupling (2026-11-02, Android 15 phone and a Windows 11 laptop, phone mic/speaker near the laptop's):
the Android app runs (map, GPS position, SALUTE and SITREP forms, settings); text in both directions with **MT63-2000**
and **OFDM**; a CQ message from the phone **relayed by the laptop**. **AFSK 1200** and **PSK** failed at arm's length.
A room model (`speaker -> echoes -> mic`, all five modems) reproduces this: every modem decodes every message within
about 10 cm of the speaker or through a cable, but room echo defeats AFSK and PSK beyond about 30 cm while MT63 still
works at 1 m. So use an audio cable (DigiRig) for AFSK and PSK; acoustic coupling is only a quick check.

On-air by radio (2026-11-02, Baofeng handhelds; BTECH USB-C audio cables with radio VOX on the phone and on the laptop):
the phone's transmissions were clean, but the laptop received nothing until the receive path was diagnosed from recordings
(`tools/replay_wav.py` and the *Record received radio audio* option). Three faults were found: Windows audio processing
on the cable (now bypassed by raw WASAPI capture), the Windows microphone level at 100 overloading the cable's input
(about 50-60 is right), and **noise cancelling built into the cable's USB chip**, which cuts every steady signal by about
20 dB after its first 0.7 s and cannot be switched off. With the radio volume in a middle range MT63 decoded all three
test messages (at full volume the cable's input clipped and nothing decoded); AFSK, PSK and OFDM did not decode at any
volume through that cable. The core now detects that last fault and warns in the log; Help explains the fixes.

**Not verified**: AFSK, PSK and OFDM over a real radio (they need an interface without noise cancelling, such as a
DigiRig); the USB-serial PTT line on either app; on Android, the Bluetooth TNC link and voice clips; on Windows, the
location service (`Geolocator`), serial GPS, the tile disk cache when offline, voice clips, KISS TNC mode. In particular:
Bluetooth SPP/virtual-COM connection to a real UV-PRO-class radio (based on third-party descriptions of how it works, not
on a test), whether such a radio accepts these AX.25 frames as-is, and PTT latency with a DigiRig.

Third-party code: Codec2 (LGPL-2.1) and the aicodix Rattlegram modem sources (permissive 0BSD-style license;
see `third_party/aicodix/`). Rattlegram snapshot used: aicodix/rattlegram commit 4df0f3af3c6da63f0d70512392e80c7a31b72099.

## Known issues and open questions
Things a reviewer may trip over, roughly in order of how likely they are to matter:
1. **Radio interfaces.** Over Baofeng radios only MT63 has been proven, through a cable whose noise cancelling ruins the
   other modems. AFSK, PSK and OFDM over radio, the DigiRig's serial PTT line and the Bluetooth TNC link are untested.
   The untested parts of both apps are listed under *Status* above.
2. **Broadcast reports are not acknowledged.** On a lossy link a report can be lost without the sender knowing. Map setup >
   *Repeat my reports* (off by default) sends 1 or 2 extra copies about 30 s apart (plus random spread); receivers drop the
   copies. Each copy is a transmission.
3. **Clock dependence.** Report times and icon ageing use each device's clock. A station with a wrong clock shows
   reports as too old or too new. Air-gapped PCs are the likely case.
4. **Maps offline.** Offline map files must be raster MBTiles built in advance (`docs/OFFLINE_MAPS.md`); vector
   files (PMTiles, vector MBTiles) are refused, and there is no in-app "download this area" because OpenStreetMap's
   servers forbid bulk downloading.
5. **Android tile settings.** After changing the tile server URL in Map setup, switch tabs once so the new source is used.
6. **Hostile/Suspect is the reporter's choice**, sent to everyone in range with no confirmation step.
7. **Windows location service** fixes worse than 150 m are refused (Wi-Fi/IP positioning), so a PC without a GPS receiver
   needs typed coordinates, a grid square or a serial GPS.
8. **Receiver DSP.** Data carrier detect, single-bit repair, a two-speed clock loop, a 900-2500 Hz band-pass prefilter
   and two parallel slicers are in. On the WA8LMF TNC Test CD it decodes 971 error-free frames from the de-emphasised
   traffic track (608 before the prefilter and slicers) and 1001 from the flat one; details in `docs/DSP_IMPROVEMENTS.md`.
   The Linux test suite has not been re-run since that change. Further ideas (FEC) are in `docs/ROADMAP.md`.
9. **Amateur use only.** No encryption, by design. Reports and messages are readable by anyone.
10. **Planned: JS8Call bridge.** A gateway station running HamLink (VHF) and JS8Call (HF) would carry messages, SITREPs and
    SALUTE reports between the two through JS8Call's TCP API, with operator approval of HF transmissions by default.

## Licence
HamLink is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License,
version 3 or (at your option) any later version** (`LICENSE`). Anyone who distributes the apps, or a modified version,
must make the corresponding source available under the same licence. Source files carry
`SPDX-License-Identifier: GPL-3.0-or-later`.

Third-party components keep their own licences, all compatible with GPL-3.0: Codec2 (LGPL-2.1, `third_party/codec2`),
the aicodix Rattlegram modem (0BSD-style, `third_party/aicodix`), the MT63 modem from fldigi by Pawel Jalocha SP9VRC and
Dave Freese W1HKJ (GPL-3.0-or-later, `third_party/mt63`), and at build time NAudio and usb-serial-for-android (MIT),
System.IO.Ports and .NET (MIT), and AndroidX/Compose (Apache-2.0).
