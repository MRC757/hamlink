# Staged bring-up: from no hardware to a Bluetooth TNC

Test in this order. Each stage adds one unknown, so a failure points at that one thing. Everything below is
**untested on real radios**: treat the expected results as hypotheses, not promises.

| Stage | What you need | Link | PTT | What it proves |
|---|---|---|---|---|
| 0 | two devices (phone + phone/PC), no radios | Audio | None | the app, modems, ARQ and UI work end to end |
| 1 | + two handheld FM radios | Audio, **acoustic** | **Manual** | the signal survives a real FM radio and your audio levels |
| 2 | + DigiRig (later) | Audio, USB | RTS/DTR | clean audio path and automatic PTT |
| 3 | + Bluetooth KISS radio (later) | KISS TNC | by the radio | the radio's own TNC |

Every setting named here is in **Settings** (Android: *Settings*, Windows: *Settings*), and the active
combination is always shown under the callsign on the main screen, e.g.
`phone speaker + mic (acoustic) · manual PTT (you key the radio) · AFSK 1200 · carrier sense: data only`.
The AFSK/OFDM modem can also be switched live from the main screen (both stations must match).
Link, PTT, audio and carrier-sense changes apply after Stop, then Start.

You need an amateur licence for anything that transmits. Use a simplex frequency you may use, low power, and
identify as the rules require. No encryption.

**Record what the receiving station hears** (Windows: Settings > *Record received radio audio*). Each Start begins a
WAV file in `%LOCALAPPDATA%\HamLink\recordings`. When a test fails, the recording shows why: replay it through every
modem with `py tools/replay_wav.py <file>` (see the main README), or send it with your report.

---

## Stage 0: two devices, no radios
Speaker of one device facing the microphone of the other, 20-30 cm apart, quiet room.

Settings on **both** devices
* Radio link: Audio + PTT. Modem: start with AFSK 1200.
* PTT: **None / radio VOX (transmit immediately)**.
* Android: *Use USB audio interface* **off**. Windows: radio audio input and output = **Windows default (acoustic)**.
* **Carrier sense: Data only** (room noise on a phone mic would otherwise look like a busy channel and block sending; *Data only* ignores noise and voice but still waits for real packet data. *Off* also works for the very first tries).
* Callsigns: the two stations must use **different strings** (a station ignores frames carrying its own callsign), e.g. `W1AAA-1` and `W1AAA-2`. This is fine for stage 0 (no RF). Once a radio transmits, identify on the air as the rules require, with your licensed callsign.
* Windows: in Sound settings turn **Audio enhancements off** for the microphone, and allow desktop apps to use it.

Steps
1. Start both. Level bar on each should move when the other device is not playing; near zero when quiet.
2. Device A: To = `CQ`, send a short text. B should show it. (Broadcast: no ACK expected.)
3. A: To = B's callsign, send text. A's line should change to *delivered*. If it says *FAILED*, B never heard it or A never heard the ACK.
4. Press **Test** (Android) / **Test / Calibrate** (Windows) on A, pick *Alternating*, start. Watch B's meter: both tone rows should show and nothing should say *clipping*. Adjust volume or the TX level slider until B's peak is roughly -20 to -6 dBFS.
5. Send a voice clip, then switch **both** to OFDM and repeat steps 2-5. OFDM is meant for weak links; expect it to hold up better when you move the devices apart or add noise.
6. Try a relay: three devices, one in Relay mode, the other two far enough apart not to hear each other.

Done when: text with *delivered* works in both modems at 20-30 cm.

## Stage 1: acoustic coupling with a hand-operated PTT
Same as stage 0, but the sound now goes through two real FM radios.

Radios: low power, same simplex frequency, wide FM if your radio offers it, VOX **off**, squelch set so the
speaker is silent with no signal. Put each radio's microphone next to its phone's speaker and each radio's
speaker next to its phone's microphone (a speaker-mic accessory held against the phone also works). Keep the
two stations far enough apart that the phones do not hear each other's loudspeakers directly (another room is ideal).

Settings on both: PTT = **Manual (I press the radio's PTT)**; carrier sense **Data only** (or *Off* for the very first tries; the default *Data or loud audio* works once the radios' squelch keeps the speaker silent).

How a transmission works in Manual mode
1. Send something. The **Manual PTT** panel shows *N frame(s) waiting*.
2. Tap **Transmit**. The panel counts down: *Key the radio's PTT now and HOLD it. Audio starts in 3 s*.
3. Press and hold the radio's PTT during the countdown. The app plays the burst: *TRANSMITTING: keep holding PTT*.
4. When it says *Release the radio's PTT now*, let go.
5. Anything else queued (an ACK you owe, a relay copy, your callsign ID) shows up in the same panel. Tap Transmit again.
6. The countdown length is adjustable (2-10 s). Sender timers are stretched to 45 s so a sender does not give up while the other operator gets ready.

Steps
1. **PTT test** first: Test > *PTT only* does nothing audible; use *Alternating* with the tone on the receiving side's meter. Fix levels here, not with real messages.
2. Broadcast (`CQ`) text: sender taps Transmit and keys; receiver should log it.
3. Directed text with ACK: sender transmits; the receiver's panel shows *1 frame waiting* (the ACK): receiver taps Transmit and keys; sender shows *delivered*.
4. Station ID: after traffic a callsign ID frame is queued. Send it with Transmit, within 10 minutes of transmitting. (The app queues it; it cannot key the radio for you, so you must do it, or say your callsign.)
5. Repeat in OFDM.

If it does not work

| Symptom | Likely cause | Try |
|---|---|---|
| Receiver hears nothing, meter flat | radio speaker too quiet / squelch closed on a weak signal | radio volume up; open squelch for the test |
| Meter moves but no messages | levels, or tones distorted | keep peak -20 to -6 dBFS; lower phone volume if the radio's mic input clips; try OFDM |
| *clipping* warning | radio speaker too loud for the phone mic | lower radio volume |
| First part of every message missing | radio takes time to key up | raise PTT lead-in (Settings) to 800-1000 ms; press PTT a little earlier |
| Only some messages arrive | marginal level, background noise, wind on the mic | move closer, shield the mic, use OFDM |
| Sender shows FAILED but receiver got it | the ACK was not sent/heard (the receiver forgot to Transmit) | receiver must tap Transmit for the ACK |
| *Nothing was ready to send* | queue empty, or carrier sense says busy | check the waiting count; set carrier sense to *Data only* |
| Transmit button greyed | a countdown or test is already running | Cancel |

Record for each attempt: modem, both devices' levels (peak dBFS on the meter), distance, whether delivered,
and the retry count. That is what is needed to tune levels and lead-in.

## Stage 1b: map and reports (no GPS, no internet needed for the radio part)
Do this after text works in Stage 1. It uses the same acoustic link.
1. Both devices: **Map > Map setup**. Pick *Typed lat/lon* (or *Grid square*) and enter a position, for example one grid square
   apart: `FN31pr` on one, `FN31ps` on the other. The status line under the map should echo the coordinates. On a phone you can try
   *Phone GPS* instead (outdoors; allow location).
2. Start the radio, tap **Send my position**, and run the manual-PTT transmit as for a text message. The other device should log
   `W1AAA [report] ...` and show a blue rectangle for that callsign at the typed position (with a ring if it was a grid square).
3. **New report**, fill in a SALUTE (choose *Hostile*, any size, put the location a few km away), send it. On the receiver the
   contact appears as a red diamond, the sender's blue rectangle turns red, and tapping either shows details with distance and bearing.
4. Check ageing without waiting: icons fade after 30 minutes and disappear after 4 hours (SALUTE) / 24 hours (position). Setting the
   device clock forward is the quick way to see it.
5. Map pictures need internet once (leave *Download map tiles* on); the radio part does not. Turn the device's data off afterwards and
   confirm the area you viewed still draws, which is the offline behaviour.
If a report is refused, the dialog names the field. Reports are single frames, so they travel exactly like a short text message.

## Stage 2: DigiRig (when you have it)
Android: *Use USB audio interface* on. Windows: radio audio in/out = the DigiRig (or leave auto). PTT = RTS (or DTR). Carrier sense *Data or loud audio* (default). Run **Test** to set the TX level (README, "Check PTT, then calibrate TX level"). Never make the DigiRig the Windows default audio device.

## Stage 3: Bluetooth KISS radio (when you have one)
Radio link = Bluetooth TNC (Android) / KISS TNC (Windows). Pair first, enable KISS TNC on the radio. See the README section.
The first thing to check is that the status line says *connected*; then a broadcast `CQ` between two stations. Expect to debug: this path is unproven on real hardware.
