# HamLink help

HamLink sends text, voice clips, positions and reports over FM radios. This help covers the equipment each modem needs, offline maps, and what to check when something does not work. The same text is in the Windows and Android apps and in docs/help/HELP.md.

## Getting started

1. **Settings**: enter your callsign, choose the radio link (audio or Bluetooth TNC), the PTT method and the modem.
2. Press **Start**. The badge shows LISTENING when the radio is running.
3. Type a message (To: CQ for everyone) and press **Send text**.
4. **Every station on a channel must use the same modem.** A station on a different modem hears nothing at all.

## Equipment for each modem

The faster the modem, the cleaner the audio path it needs. Acoustic coupling (no cable, the phone or PC simply held next to the radio or the other device) adds room echo, which the fast modems cannot cope with.

- **MT63-2000 (slow but robust)**: works by acoustic coupling, even at about 1 m from the speaker in a room, and through any radio. The best choice for weak, noisy or fading signals, and for a quick test without a cable. Slow: about 7 times the airtime of AFSK.
- **OFDM**: works by acoustic coupling within about 30 cm of the speaker, better with a cable. Not faster than AFSK, and on a normal radio link AFSK with FX.25 now decodes slightly weaker signals; OFDM may cope better with echo and multipath.
- **AFSK 1200 (the default, with FX.25 error correction)**: needs an audio cable (DigiRig or similar) for reliable use. Acoustic coupling works only with the microphone within about 10 cm of the speaker. The only modem a radio's built-in TNC can send (Bluetooth TNC link).
- **PSK 2400**: needs an audio cable (DigiRig or similar). About twice as fast as AFSK and about as robust over a cable. Acoustic coupling fails beyond about 10 to 30 cm.
- **PSK 3600 (fastest)**: needs an audio cable and a strong, clean signal. About 2.4 times as fast as AFSK. Does not work by acoustic coupling except pressed against the speaker. If messages need retries, drop to PSK 2400.

### Radio interfaces

- **DigiRig (Mobile or Lite) or a similar USB sound card with a serial PTT line**: works with every modem. Settings: choose its audio input and output, and PTT **Serial RTS (DigiRig)** (Android: **RTS**). On Android, plug it in with a USB-C OTG cable or adapter.
- **USB sound card without a PTT line** (or a cable to the radio's mic and speaker jacks): set PTT to **None / radio VOX** and turn on the radio's VOX. Raise **PTT lead-in** if the start of each transmission is cut off.
- **No interface at all**: set PTT to **Manual**. HamLink counts down and tells you when to press and release the radio's PTT. Use MT63 or OFDM if you hold the phone or PC to the radio.
- **Radio with a built-in KISS TNC** (BTECH UV-PRO, Vero VR-N76 and similar): choose the **Bluetooth TNC** link. The radio does the modulation and keying, so only AFSK 1200 is available.

### Two devices without radios (acoustic test)

Set carrier sense to **data only** (room noise otherwise keeps the channel "busy") and PTT to **None / radio VOX** on both, choose MT63 or OFDM, and hold the phone near the PC's speaker.

## Offline maps

The map uses, in this order: an **offline map file** if you have loaded one, then **tiles saved** from earlier browsing, then the **tile server** when "Download map tiles when needed" is on.

### Loading an offline map file

- **Windows**: Map view, **Map setup...**, **Open map file...**, choose the .mbtiles file, **Save**.
- **Android**: Map tab, **Map setup**, **Open map file...**, pick the file, **Save**. The file is copied into HamLink's storage, so the original can be deleted afterwards.
- **Remove** in the same place stops using it. Where the file has no tiles, the tile server and saved tiles fill in. Zoomed in further than the file goes, its tiles are enlarged.

Map files must be **raster MBTiles** (png or jpg images). Vector files (PMTiles, or MBTiles in pbf format) are refused.

### Where to get map files

Build one with **tools/make_mbtiles.py** from the HamLink download (needs Python 3 and internet, once). Give it the area as west,south,east,north in degrees and the zoom range:

```
python tools/make_mbtiles.py --preset usgs-topo --bbox -76.75,36.60,-76.45,36.85 --zoom 8-15 --name "My area" --out myarea.mbtiles
```

- **USGS Topo** (United States): --preset usgs-topo gives topographic maps with contours, trails and names; --preset usgs-imagery-topo gives aerial photos with labels. Both go to zoom 16.
- **Your own OpenStreetMap tiles** (anywhere): render them with a tile server such as TileServer GL from free Protomaps data, then point the script (or HamLink itself) at that server. See docs/OFFLINE_MAPS.md.
- **Not allowed**: bulk downloading from openstreetmap.org (its policy forbids it; HamLink and the script refuse), and Google, Bing or Esri maps.

Sizes: a town at zoom 8-15 is about 10 MB, a county at zoom 8-16 about 100 MB, a state at zoom 6-14 about 1 GB.

### A tile server on your network

Map setup, **Tile server URL**: https:// works anywhere; plain http:// is allowed only to this device or the local network (for example http://192.168.1.20:8080/styles/basic/{z}/{x}/{y}.png from a laptop at a command post).

## Troubleshooting

### Nothing is received

- Check that **every station uses the same modem**.
- Check that the radio is **started** (badge LISTENING, not OFF).
- Watch the **level bar** while the other station transmits. If it does not move, HamLink is not hearing audio: check the audio input in Settings (Windows), the cable, and the radio's volume. On Android, allow the microphone permission.
- Use **Test / Calibrate** (Android: **Test**) to check levels. Audio that is far too loud distorts as badly as audio that is too quiet.
- Acoustic coupling with AFSK or PSK: move closer or switch to MT63 (see *Equipment for each modem*).

### Signals are heard but almost nothing decodes

The usual cause is something in the receive audio path changing the signal. Check these in order:

- **"WARNING: received transmissions drop by about 20 dB..."** in the log means the audio input turns steady sounds down after about a second, and data signals are steady sounds. On the other station's test tone you can see it too: the level bar jumps up, then falls back after a moment.
- **USB-C headset-style cables and adapters with built-in noise cancelling** (sold as "ENC", "noise cancelling" or "call noise reduction") do this inside the cable, and no setting can turn it off. They still transmit cleanly, so they are fine for sending only. To receive, use an interface without noise cancelling: a DigiRig, a plain USB sound card, or the computer's own headset jack. Until then, **MT63** may still get through such a cable if the radio's volume is in a middle range (about half to three-quarters): too low and the signal sinks into the cable's noise gate after the cut, too high and the cable's input clips. AFSK, PSK and OFDM did not decode through one at any volume in testing.
- **Windows audio enhancements**: Settings, System, Sound, choose the radio interface's microphone, and set **Audio enhancements** to Off (also **Voice clarity** if shown). Do the same for its speakers. HamLink asks Windows for raw audio, which skips these effects when the device allows it; the log line "Audio input: raw" confirms that.
- **Windows microphone level**: Control Panel sound (Windows key + R, mmsys.cpl), Recording, the radio interface, Properties, **Levels**. Start at about **50**. At 100 many USB interfaces overload and distort even when the level bar looks normal.
- **The radio's volume knob**: about a quarter is usually plenty into a USB interface. A loud speaker output into a microphone input distorts.
- **Radio menu**: turn off any **noise reduction** (NR, DENOISE), **compander** or **scrambler** on both radios, and set both to the same **Wide/Narrow** bandwidth.
- **Record and replay**: tick *Record received radio audio* in Settings (Windows), then replay the file through every modem with tools/replay_wav.py, or send it with your report.

### My messages stay [queued]

- **CHANNEL BUSY** stays on: room noise or a noisy channel holds transmissions back. Set carrier sense to **data only**.
- **Manual PTT**: queued messages wait for you to press **Transmit** (on the Messages view or under the map), then key the radio when told.
- "PTT line is not ready": the DigiRig or serial adapter is not plugged in or not found. Check the PTT port in Settings, or choose None / radio VOX or Manual.

### The other station decodes nothing from me

- Set the transmit level with **Test / Calibrate**; too much audio overdrives the radio.
- If the radio keys late and cuts off the start, raise **PTT lead-in** in Settings.
- Make sure the radio is actually transmitting (its TX light) and both radios are on the same frequency.

### Relay does not relay

- On the relay station, tick **Relay**.
- On the sending station, **Max relay hops** in Settings must be 1 or more.
- A relay re-sends only messages it has not heard relayed already, and never your own.

### Reports

- A SITREP from another station shows as a **version mismatch**: that station runs a different HamLink release; update everyone to the same version.
- A station's map colour comes from its latest **SITREP** (green A, orange B, red C; blue when none or older than 4 hours). SALUTE reports colour only their own marker.

### Map

- **Map is blank**: no internet and nothing saved for this area. Load an offline map file, or view the area once while online.
- **Map file not loaded**: Map setup shows why (not found, vector format, no tiles).
- **Tile server URL shown in red**: it needs {z}, {x} and {y}, and https:// (http:// only on the local network).
- **Android, "location permission needed"**: Map setup, Allow location access, and choose **Precise**.

### Windows

- **"audio capture stopped"**: Windows took the microphone away (another program, a headset plugged in, sleep). Press Stop, then Start.
- **SmartScreen warning** on first run: More info, Run anyway (the program is not code-signed).
- Errors are written to %LOCALAPPDATA%\HamLink\error.log. Include it, or a screenshot, when reporting a problem.

### Android

- Changes in Settings apply when you press **Save**; a running radio restarts with them.
- To report a crash, send a screenshot and what you tapped; a PC with adb can also collect the log (adb logcat).

### Still stuck

Report problems at github.com/MRC757/hamlink (Issues) with the exact error text, the modem, and how the stations are connected.
