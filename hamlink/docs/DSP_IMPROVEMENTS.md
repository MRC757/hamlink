# Improving receive (audio detection) with DSP

Research notes, a prioritised plan, and results. **Steps 0-3 are implemented (version 0.1.2)**; steps 4-6 are deferred and
tracked in `docs/ROADMAP.md`. Sources are listed at the end; figures from other projects are theirs, measured on their
demodulators. Our figures come from the simulator and are tested in `tests/test_dsp.py`.

## Results of steps 1-3 (simulator; 119-byte frames, 300-3000 Hz band limit, white noise, SNR in 2.7 kHz)
| | 8 dB | 9 dB | 10 dB | 11 dB |
|---|---|---|---|---|
| Frames decoded before | 56% | 88% | 97% | 99.7% |
| With single-bit repair | **76%** | **97%** | **99.7%** | 100% |

* **Wrong frames: none.** 1,200 frames sent at 5-7 dB, where most frames are damaged, produced no frame with wrong content,
  and 20 minutes of noise produced no false frame. This is the property that matters most; the tests check it on every run.
* **Carrier detect:** on during 99.5% of a data burst at 6-20 dB SNR, on within 20 ms of the burst starting, off within
  0.5 s of it ending. Off at least 98% of the time for white noise, band-limited noise, voice-like audio and steady tones
  (600, 1000, 1200, 1700, 2200, 3400 Hz, and the 1200/2200 Hz calibration tone). Before the fix, a 1000 Hz tone fooled the
  clock detector 10% of the time; see step 1.
* **Two-speed clock loop: no measurable change** at 8-10 dB, with or without clock error, or with 30 ms preambles. Kept at
  settings that measured the same as before (0.25 searching, 0.15 locked; previously 0.15 throughout). It may still help on
  real audio with phase distortion, which the simulator lacks.
* On real radios the gains may be larger or smaller: the simulator has no limiter, emphasis mismatch or squelch effects.

## What the AFSK receiver does today (`core/afsk.c`)
* DC blocker, then two sliding correlators (one bit long, 40 samples at 48 kHz) for 1200 and 2200 Hz. Each output is
  divided by its own decaying peak (a per-tone AGC), and the comparison of the two decides mark or space.
* One slicer, one fixed-gain clock-recovery loop, hard bit decisions into HDLC. A frame with one wrong bit is lost.
* "Channel busy" for carrier sense is **the RMS level of the audio**, not detection of a signal.

Measured in our simulator (`tests/sweep.py`; 300-3000 Hz band limit, tilt, clock error, white noise):
* 100% of frames down to about 12 dB SNR, about 97% at 10 dB and 55% at 8 dB (README).
* **Twist is already handled.** New measurement for this note: tilt from -9 to +12 dB at 30, 14 and 11 dB SNR decoded
  99-100% of frames in every case, so the per-tone normalisation does the job that Dire Wolf's AGC and multiple slicers do.
* The simulator has **no phase distortion, limiter or squelch effects**, which are what make real packet audio hard
  (WB2OSZ's analysis of the TNC Test CD shows very distorted real signals). So the simulator cannot rank most of the
  ideas below. Real recordings are needed, which is why step 0 comes first.

## Prioritised plan

### 0. Measure on real audio first (done: `tools/decode_wav.py`)
The WA8LMF TNC Test CD is the usual benchmark for 1200 baud packet receivers. Its AX.25 frames use the same HDLC framing
and CRC as HamLink's audio link, so our demodulator can be scored on it directly:
`python3 tools/decode_wav.py 02_Track_2.wav`. Published scores on Track 2 give a yardstick: Dire Wolf decodes about 1000+
error-free frames, older hardware TNCs and first-generation sound-card modems fewer. The CD could not be downloaded from
this build environment at first; **scores are now in step 4** (measured 2026-11-01). Recordings
of your own radio (squelch, VOX, acoustic coupling) are just as valuable: `decode_wav.py` accepts any WAV.

### 1. Detect data, not loudness, for "channel busy" (high value, low cost, no protocol change)
Today any loud audio holds off transmission (room noise during acoustic tests, voice on the channel) and a weak data
signal below the RMS threshold does not. Two proven methods:
* **HDLC-based carrier detect** (Dire Wolf): busy when HDLC flag patterns are being received, idle after a run of bits
  with no transitions. Voice, noise and steady tones do not trigger it.
* **Clock-recovery-based carrier detect** (VP-Digi): count tone transitions that land near the expected bit edge versus
  far from it; busy when the counter passes a threshold, with hysteresis so it does not flicker.

Recommendation: busy = data detected (either method) OR audio far above the noise floor (keeps holding off for voice).
This fixes the known "carrier sense off for acoustic tests" workaround and makes CSMA work for weak signals. Test it with
the existing simulator (voice-like noise, tones, and weak data) before any radio.

**Implemented** (`core/afsk.c`): both detectors. HDLC: on after two flags back to back, off after 7 ones in a row. Clock:
transitions within +/-2.5 samples (of 40) of the expected edge add 3, others subtract 1; on at 30, off at 12. A steady
1000 Hz tone beats with the 2200 Hz correlator at exactly 1200 Hz and locked the clock detector, so clock-based detect now
counts only within 1,800 bits (1.5 s) of an HDLC flag; tones never make flags. Carrier sense has three modes:
*Data or loud audio* (default, the old behaviour plus data detect), *Data only* (for acoustic tests: room noise and voice are
ignored), and *Off*. With the OFDM modem selected, *Data only* also uses the level, because the AFSK detector cannot
recognise OFDM.

### 2. Two-speed clock recovery (low cost)
Our loop has one fixed gain. VP-Digi uses a faster correction while not locked and a slower one once a signal is locked
(0.50 vs 0.74 retention factors at 1200 baud), so it acquires quickly on the preamble and is not pulled around by noise
during the frame. Cheap to try and measurable in the simulator at 8-10 dB SNR. **Implemented; no measurable effect in the simulator** (see
Results).

### 3. Repair single-bit errors with the CRC (medium value, low cost, no protocol change)
When a frame fails its CRC, flip each bit in turn and retest. Dire Wolf's numbers: on the Test CD it adds about 1% of
frames, but listening to live local stations it recovered about 23% more frames in one test, and in a 9-hour comparison
the fix-up raised the count from 5667 to 6088 (7%). Their caution matters for us: repairing **more** than one bit produces
occasional wrong data, so they advise against it for anything important. Recommendation: single-bit repair only, on by
default, plus a plausibility check of the repaired mesh header (valid type, callsign characters, fragment numbers).
Cost: about 8 x frame-length CRC checks per bad frame, negligible on a phone.

**Implemented, with three guards against wrong data** (`core/afsk.c`, `core/mesh.c`):
1. One wrong tone decision corrupts two adjacent data bits (NRZI), so a candidate fix flips both.
2. Only the **8 least certain** tone decisions are tried (certainty = how far the mark/space comparison was from zero at
   the sampling instant), not every bit. Trying every bit of a 1,000-bit frame would give roughly a 1.5% chance (1000/65536)
   of a false CRC match; 8 candidates give about 0.01%.
3. A repair is accepted only if **exactly one** candidate passes the CRC, and the repaired frame must pass a structural
   check of the mesh header (valid type, callsign characters and padding, fragment numbering and sizes) before the mesh sees
   it. Counters for repairs and both kinds of rejection are available (`hl_get_rx_info`).
It can be switched off in Settings. Double-bit repair was not added (Dire Wolf's caution, above).

### 4. Multiple slicers / parallel demodulators, with a band-pass prefilter — **implemented 2026-11-01**
Measured with `tools/score_tnc_cd.py` (Windows, scores the shipped `hamlink_core.dll`, no compiler needed; any DLL can
be passed with `--dll`) on the WA8LMF TNC Test CD (ISO "TNC_Test_2", tracks resampled 44.1 to 48 kHz, left channel, error-free frames,
bit repair off). The ISO's track names do not match their contents: tracks 1 and 2 are the same 25.8-minute traffic
recording, flat and de-emphasised; track 3 holds the 100 Mic-E bursts.

| Receiver | Track 2 (de-emphasised) | Track 1 (flat) | Track 4 (drive test) |
|---|---|---|---|
| Before: one slicer, no prefilter | 608 | 964 | 88 |
| 9 weightings -4..+4 dB, no prefilter | 687 | 979 | 91 |
| 900-2500 Hz prefilter only, 0 dB | 865 | 981 | 99 |
| **Now: 900-2500 Hz prefilter + slicers at -1 and +2 dB** | **971** (983 with repair) | **1001** (1019) | **103** (109) |

What was learned:
* **The prefilter matters most.** The one-bit tone correlators pass hum, hiss and voice-band energy outside the tones.
  A 241-tap (5-bit) FIR band-pass did as well as a 481-tap one; 1000-2400 Hz scored like 900-2500; 700-2700 was worse.
* **Weightings must be small here.** Each tone is normalised to its own peak before slicing, so at +/-6 dB one tone wins
  every bit; Dire Wolf's -6..+12 dB range applies to raw levels and does not carry over. With the prefilter,
  de-emphasised audio slices best near +2 dB and flat audio near -1 dB, hence those two slicers.
* **No false decodes:** 0 frames from 60 minutes of white noise (-40, -20, -6 dBFS) and from the CD's three
  calibration-tone tracks, with and without bit repair.
* CPU: about 80-170x real time on one x86 core including Python call overhead (about 1% of a core), against about
  1,200x before; the FIR is the main cost. Measure on a phone before relying on it there, and consider an IIR band-pass
  if it is too slow.
* **Denormals:** the first version ran slower than real time (1.8 s of CPU per second of audio) during exact digital
  silence after a signal, because the DC blocker's output decayed into denormal floats and stayed there. The blocker
  output and the level meter are now flushed to zero below 1e-20; such silence processes like any other audio.
* Not re-run after the change: the Linux pytest suite (`tests/`), including `test_dsp.py`.
Dire Wolf runs 9 slicers with different mark/space weightings (about 2.3 dB apart, -6 to +12 dB) and keeps any that
decode. On Track 2 that took it from 988 to 1004 frames (and 1022 with bit repair). The best single weighting differed
between flat and de-emphasised audio, which is the point: one setting cannot suit every radio. Our per-tone normalisation
may already capture most of this (step 0 will tell). A cheaper variant that also covers phase distortion: two or three
correlator window lengths or a bandpass prefilter (Dire Wolf's "E" profile added one and gained about 18 frames over the
older "C" profile). Costs 2-9x the demodulator CPU, currently 0.2% of one x86 core, so affordable.

### 5. Forward error correction for the AFSK link — **implemented 2026-11-01 as FX.25** (`core/fx25.c`)
FX.25 was chosen over IL2P because it is backward compatible: stations without FX.25 (older HamLink builds, plain AX.25
software) still decode every frame that arrives error-free, so no flag day is needed. Format: `docs/PROTOCOL.md`. Default:
16 check bytes; Settings offers off, 16, 32 and 64. Receiving FX.25 is always on.

Verification (Windows, through the C API the apps use; two stations joined by simulated audio):
* Reed-Solomon against an independent implementation (Python `reedsolo`, same field, roots and byte order): check bytes
  identical on 900 random blocks; every block with up to nroots/2 byte errors corrected (1,180 cases); all 900 blocks with
  more errors refused, none miscorrected.
* Previous release (no FX.25) receiving FX.25 transmissions on a clean channel: 30/30 frames at each of 16, 32 and 64.
* Acknowledged two-fragment messages with FX.25 both ways: 4/4 delivered. Test CD scores unchanged (971 / 1001 / 100 /
  103 frames); no frames from 30 minutes of noise.
* Messages delivered out of 120 (80-byte text broadcasts, white noise, SNR in 2.7 kHz):

| SNR | Plain | FX.25 16 | FX.25 32 | FX.25 64 |
|---|---|---|---|---|
| 4 dB | 1 | 6 | 40 | 98 |
| 5 dB | 10 | 90 | 117 | 117 |
| 6 dB | 77 | 119 | 120 | 120 |
| 7 dB | 114 | 120 | 120 | 120 |
| 8 dB | 119 | 120 | 120 | 120 |

* Cost: that burst (500 ms lead-in, message, station ID) took 1.11 s plain, 1.55 s with 16 check bytes, 1.65 s with 32,
  1.87 s with 64. Short frames such as ACKs nearly double; a full data fragment grows about 25%.
* Not tested: interoperability with Dire Wolf or UZ7HO on real audio (the tags, code and packing follow Dire Wolf's
  source), and real radios. White noise is the friendliest kind of damage; bursts of interference may behave differently.

Background, from before the choice was made. 1200 baud packet has no FEC: one bad bit loses the frame. Options:
* **FX.25** wraps an AX.25 frame in Reed-Solomon parity so FEC-aware receivers correct errors while plain AX.25 receivers
  still decode it. Its original small test (61 frames) found 15% decodable without FEC and 46% with it. FX.25 matters most
  in **Bluetooth-TNC mode** if a radio's TNC supports it (the BTECH radios' TNC is not documented to).
* **IL2P** (NinoTNC, also in Dire Wolf 1.7) drops AX.25 compatibility for less overhead: Reed-Solomon blocks, a 24-bit
  sync word for better packet detection and carrier detect, and no bit stuffing. Field use on TARPN links found false
  decodes on marginal links, which led to "IL2P+CRC".
* HamLink controls both ends of its audio link, so it could adopt IL2P-style Reed-Solomon per frame as a protocol v2
  option. Note that the **OFDM modem already has strong FEC** (polar codes); when this was written it decoded 4-6 dB
  deeper than AFSK in simulation. (Update 2026-11-03: after steps 1-3 and FX.25 (step 5), AFSK with FX.25 delivers 500-byte
  acknowledged messages down to 5 dB SNR in simulation, OFDM down to 7 dB; see the README's modem summary.)

### 6. Audio-path details worth checking on real hardware — deferred to on-air testing, see ROADMAP
* **Squelch:** a closed squelch takes time to open and adds a noise burst ("tail") at the end. The PTT lead-in covers the
  opening; carrier detect from step 1 should ignore the tail.
* **Input clipping:** the receive meter already counts clipped samples; clipped audio destroys the tone ratio.
* **Phone audio processing:** Android's UNPROCESSED source is used when the phone supports it; otherwise automatic gain
  and noise suppression can damage data. Worth logging which source was used on each test phone.
* **Acoustic coupling:** keep the phone close to the radio speaker so the direct sound dominates room echo.

## How to evaluate any change
1. Simulator first (`tests/sweep.py`, plus new cases with voice-like noise and phase distortion) for no regressions.
2. Test CD and your own recordings with `tools/decode_wav.py`: frames decoded, and **zero** frames with wrong content.
3. On air: retry counts and failed deliveries in the app log, same radios and positions, A/B with the change on and off.

## Sources
* WB2OSZ, *A Better APRS Packet Demodulator, Part 1: 1200 baud* (Dire Wolf docs): AGC, prefilter, multiple slicers,
  bit repair, Test CD and live comparison figures.
  https://cloud.xlxbra.net/direwolf/direwolf_git_v1.6/doc/A-Better-APRS-Packet-Demodulator-Part-1-1200-baud.pdf
* WB2OSZ, *One Bad Apple Don't Spoil the Whole Bunch* (bit repair levels and the caution about multi-bit repair).
  https://cloud.xlxbra.net/direwolf/direwolf_git_v1.7/direwolf-doc_git/One-Bad-Apple-Dont-Spoil-the-Whole-Bunch.pdf
* WB2OSZ, *WA8LMF TNC Test CD Results* (scores of many TNCs and modems).
  https://raw.githubusercontent.com/wb2osz/direwolf/master/doc/WA8LMF-TNC-Test-CD-Results.pdf
* WB2OSZ, *A Closer Look at the WA8LMF TNC Test CD* (real-world distortion on the test signals).
  https://cloud.xlxbra.net/direwolf/direwolf_git_v1.6/doc/A-Closer-Look-at-the-WA8LMF-TNC-Test-CD.pdf
* Dire Wolf repository (FX.25 and IL2P support). https://github.com/wb2osz/direwolf
* Dire Wolf `hdlc_rec.c` (HDLC-based data carrier detect). https://sources.debian.org/src/direwolf/1.0-2/hdlc_rec.c
* VP-Digi `modem.c` (PLL-based DCD, locked/unlocked loop gains). https://git.ruhnet.net/RuhNetConsulting/vp-digi/raw/branch/main/Core/Src/modem.c
* Mobilinkd TNC3 AFSK demodulator notebook (twist from emphasis/limiting, phase distortion and ISI).
  https://nbviewer.org/github/mobilinkd/afsk-demodulator/blob/master/afsk-demodulator.ipynb
* FX.25 (Wikipedia summary of the TAPR draft and its initial test). https://en.wikipedia.org/wiki/FX.25_Forward_Error_Correction
* Nino Carrillo KK4HEJ, *Improved Layer 2 Protocol*, TAPR DCC 2020. https://web.tapr.org/meetings/DCC_2020/KK4HEJ/IL2P%202020%20DCC.pdf
* OARC wiki, NinoTNC (IL2P+CRC and why it was added). https://wiki.oarc.uk/packet:ninotnc
