# HamLink for Windows 11

WPF (.NET 8) front end over the same C core as the Android app (`hamlink_core.dll` = modem + ARQ + relay
+ Codec2). Text, push-to-talk voice clips, simplex store-and-forward relay mode, and a map with
position and SALUTE reports. **Freshly building and launching, not yet verified end to end**: please
report build errors or crashes with the full text.

## Build
On any PC with the .NET 8 SDK (needs NuGet **once** for NAudio 2.2.1 and System.IO.Ports 8.0.0):

    powershell -File build.ps1          # or: dotnet publish -c Release -r win-x64 --self-contained -o publish

Copy the whole `publish` folder to the target machine. It is **self-contained** (no .NET install) and
**not single-file** (nothing is unpacked to %TEMP%), so it runs on an air-gapped PC. `hamlink_core.dll` only
depends on the Windows Universal CRT, which is built into Windows 10/11.

To rebuild the native DLL after changing `core/`: `./build-native.sh` (Linux/macOS; zig or mingw-w64). The DLL now
includes C++17 code (the OFDM modem) with the C++ runtime linked in statically.

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
and "Let desktop apps access your location". Map tiles are cached in `%LOCALAPPDATA%\HamLink\tiles`. Details: `docs/MAPS_AND_REPORTS.md`,
extending: `docs/EXTENDING.md`. None of this has been compiled or run.

## Notes
* Opening a serial port can pulse RTS/DTR for a few ms in some Windows drivers, which may blip the
  transmitter at app start. Start the app with the radio off or on a dummy load.
* Settings are stored in `%AppData%\HamLink\settings.json`.
* Relay mode: tick "Relay mode" on the hilltop PC. It re-sends frames not addressed to it and
  identifies itself after traffic and at least every 9 minutes. It stops auto-playing received voice.
* Audio uses WinMM (MME) through NAudio so Windows handles sample-rate conversion for the 48 kHz
  modem; expect ~40-100 ms of extra audio latency, which the 500 ms PTT lead-in absorbs.
