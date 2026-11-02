# HamLink for Windows 11

WPF (.NET 8) front end over the same C core as the Android app (`hamlink_core.dll` = modem + ARQ + relay
+ Codec2). Text, push-to-talk voice clips, simplex store-and-forward relay mode, and a map with
position and SALUTE reports. **Builds and runs on Windows 11; not yet tested with a radio or a second station**: please
report problems with the full text. Errors are logged with stack traces to `%LOCALAPPDATA%\HamLink\error.log`.

## Build
On any PC with the .NET 8 or 9 SDK (needs NuGet **once** for NAudio 2.2.1 and System.IO.Ports 8.0.0):

    powershell -File build.ps1          # or: dotnet publish -c Release -r win-x64 --self-contained -o publish

Copy the whole `publish` folder to the target machine. It is **self-contained** (no .NET install) and
**not single-file** (nothing is unpacked to %TEMP%), so it runs on an air-gapped PC. `hamlink_core.dll` only
depends on the Windows Universal CRT, which is built into Windows 10/11.

In VS Code, open the repository root: *Terminal > Run Build Task* builds and F5 runs under the debugger (needs the
C# Dev Kit extension).

To rebuild the native DLL after changing `core/`: `pip install ziglang`, then `bash build-native.sh` (Linux, macOS, or
Windows in Git Bash; mingw-w64 also works). MSVC cannot build it because Codec2 uses C99 variable-length arrays. The DLL
includes C++17 code (the OFDM modem) with the C++ runtime linked in statically. If `hamlink_core.dll` is missing or a
different API version, the app says so at start-up instead of failing later.

## Modems
Pick the modem in Settings or with the buttons in the header (every station on the channel must use the same one):
*AFSK 1200* (default, packet-radio compatible), *OFDM*, *MT63-2000*, which is **slow but robust** (about 7 times the
airtime of AFSK, but it gets through far weaker and noisier signals), *PSK 2400* (about twice as fast as AFSK, about as
robust) or *PSK 3600* (fastest, about 2.4 times AFSK, needs a strong clean signal; PSK 2400 and 3600 stations hear each
other). Details: `../docs/PROTOCOL.md`.

## Error correction (FX.25)
Settings > *AFSK error correction*: FX.25 with 16 check bytes is the default; 32 or 64 repair more damage at the cost of
longer transmissions, and *Off* sends plain frames. Receiving FX.25 is always on, and stations without FX.25 still read
frames that arrive intact. When FX.25 rescues a damaged frame, the message log says so. Details: `../docs/PROTOCOL.md`.

## Hardware / Windows setup (DigiRig + Baofeng)
1. Plug the DigiRig into USB. Windows adds a sound device ("USB Audio Device") and a COM port.
2. **Settings > Privacy & security > Microphone > "Let desktop apps access your microphone" = On.**
   If it is off the radio input is silently all zeros.
3. In Sound settings, for the DigiRig input and output: turn **Audio enhancements OFF** (noise
   suppression and AGC wreck tones), format 48000 Hz 16-bit. Leave the output volume fixed (e.g. 100%) and
   set deviation with the app's *TX audio level* slider, so Windows volume keys cannot change it.
4. **Do not set the DigiRig as the Windows default playback or recording device.** The app picks it
   explicitly; if it were the default, system sounds and notifications would be transmitted.
5. Radio: VOX off, squelch up so the speaker is silent with no signal, low power to start.
6. App: Settings -> callsign; radio audio in/out (auto-picks a "USB" device); PTT line RTS or DTR and the
   DigiRig's COM port; your **own** mic and speakers for voice clips. Start.
7. Press **Test / Calibrate**: *PTT only* verifies the PTT line; the tone modes with the live level slider
   and the receive meter implement the calibration procedure in the top-level README. The tone is limited to
   15 s, refuses to start on a busy channel, and a station ID follows it.

## No DigiRig yet? Acoustic coupling with a hand-pressed PTT
Settings > PTT = *Manual*, radio audio input and output = *Windows default (speakers + mic)*, carrier sense *Data only*.
The main window then shows a Manual PTT panel: press Transmit, key the radio during the countdown, release when
told. Full staged plan in `../docs/TESTING.md`.

## KISS TNC mode (paired Bluetooth radio or serial TNC)
Settings > Radio link > *KISS TNC*. No audio or PTT setup is needed: the radio modulates, keys and does carrier
sense. Pair the radio in Windows Bluetooth settings, enable KISS TNC on the radio, then choose the **outgoing**
virtual COM port Windows created for it (opening that port makes Windows connect, which can take several
seconds and is retried automatically). DTR/RTS are left low. Test / Calibrate does not apply in this mode.
The radio-side details and caveats are in the top-level README (none of this is tested on real hardware).

## Map, position reports and SALUTE
Messages / Map switch in the header. Position sources: typed lat/lon, grid square, the Windows location service, or a
serial NMEA GPS (COM port; read while the radio is running). The project targets `net8.0-windows10.0.19041.0` so the Windows
location API is available. To use it: Settings > Privacy & security > Location > turn on Location services
and "Let desktop apps access your location". Map tiles are cached in `%LOCALAPPDATA%\HamLink\tiles`.
**New report...** (radio started) opens the report form at your own position; **right-click a point on the map** to open it
at that location instead. Details: `docs/MAPS_AND_REPORTS.md`, extending: `docs/EXTENDING.md`.
Tried on one PC: grid-square position, tile download and drawing, and a SALUTE report from a right-click. The Windows location
service and serial GPS are untested.

## Notes
* Opening a serial port can pulse RTS/DTR for a few ms in some Windows drivers, which may blip the
  transmitter at app start. Start the app with the radio off or on a dummy load.
* Settings are stored in `%AppData%\HamLink\settings.json`.
* Settings > *Record received radio audio* saves what the radio hears to `%LOCALAPPDATA%\HamLink\recordings` (a new WAV
  file at each Start and every 30 minutes; about 350 MB per hour; the oldest files are deleted above 1 GB; moments when
  you transmit are skipped). `py tools/replay_wav.py <file>` replays a recording through every modem and shows what
  decodes, which is the quickest way to diagnose a failed on-air test.
* Relay mode: tick "Relay mode" on the hilltop PC. It re-sends frames not addressed to it and
  identifies itself after traffic and at least every 9 minutes. It stops auto-playing received voice.
* Audio uses WinMM (MME) through NAudio so Windows handles sample-rate conversion for the 48 kHz
  modem; expect ~40-100 ms of extra audio latency, which the 500 ms PTT lead-in absorbs.
