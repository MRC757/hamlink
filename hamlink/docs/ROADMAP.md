# Roadmap: deferred work

Things decided against for now, or waiting on hardware or data. Each entry says why it is deferred and what would trigger it.
Sources for the DSP items are in `docs/DSP_IMPROVEMENTS.md`.

## Receiver DSP (from `docs/DSP_IMPROVEMENTS.md`)
| Item | What | Why deferred | Do it when |
|---|---|---|---|
| Score on the TNC Test CD | Run `tools/decode_wav.py` on WA8LMF Track 1 and 2 | The CD could not be downloaded in the build environment | Anyone with the CD: first priority, it ranks everything below |
| ~~Parallel demodulators~~ | **Done 2026-11-01:** 900-2500 Hz prefilter + slicers at -1 and +2 dB, 608 to 971 frames on the de-emphasised Test CD track (`docs/DSP_IMPROVEMENTS.md` step 4) | | |
| Double-bit repair | Try pairs of the least certain bits | Dire Wolf warns repairs beyond one bit give occasional wrong data. HamLink has no other integrity check on text | Never by default; maybe as an option for position reports only |
| FEC on the AFSK link | Reed-Solomon per frame, IL2P-style (with a CRC as IL2P+CRC added), or FX.25 so plain AX.25 receivers still decode | Protocol change, every station must update. OFDM already has strong FEC for weak paths | On-air loss rates show AFSK links that OFDM cannot serve (e.g. interoperability needed), or a TNC radio supports FX.25/IL2P |
| FX.25 in Bluetooth-TNC mode | Use the radio TNC's FEC if it has one | The BTECH radios' TNC is not documented to support it | A radio with an FX.25/IL2P KISS TNC is in hand |
| Audio-path checks | Squelch opening and tail, input clipping, which Android audio source a phone really gives, acoustic placement | Needs radios and phones | Stage 1-2 on-air tests (`docs/TESTING.md`); log the RX meter and retry counts |

## Maps and reports
| Item | Why deferred |
|---|---|
| Offline map packs (MBTiles/PMTiles `TileSource`) | Designed for (`docs/EXTENDING.md`), not written. OSM's servers forbid bulk downloads, so packs must come from another provider or a self-made extract |
| Confirmation step before sending Hostile/Suspect | Not decided; reports are reporter's judgement and go to everyone in range |
| Clock sanity check between stations | Report ageing uses each device's clock; an air-gapped PC with a wrong clock shows wrong ages. Could compare with GPS time or with other stations' report times |

## Build and test
| Item | Why deferred |
|---|---|
| Gradle wrapper for Android | Could not be generated in the build environment; Android Studio creates it |
| Compile and run both apps | No .NET or Android SDK in the build environment: the first reviewer builds are the test |
| Windows DLL execution test | No Windows or Wine in the build environment |
