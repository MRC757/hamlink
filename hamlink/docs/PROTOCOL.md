# HamLink over-the-air format (v1)

Publicly documented, unencrypted. Every frame carries the sender's callsign in clear.

## Physical layer
Bell 202-style AFSK, 1200 baud: mark 1200 Hz, space 2200 Hz, NRZI (0 = change tone, 1 = keep),
48 kHz audio. Phase-continuous. Audio is fed to / taken from the radio's mic and speaker paths.

## Link layer
HDLC framing: `0x7E` flags, bit stuffing after five 1s, CRC-16/X.25 (poly 0x8408, init 0xFFFF,
xorout 0xFFFF, low byte first). A PTT burst is: lead-in flags (default 500 ms), then frames
separated by two flags, then two tail flags. One burst never exceeds `burst_cap_ms` (12 s).

### FX.25 forward error correction (AFSK, on by default)
Each frame may instead be sent as an **FX.25** block (TAPR FX.25 draft, interoperable with Dire Wolf and UZ7HO
Soundmodem): a 64-bit correlation tag, then the frame as ordinary HDLC (flag, bit-stuffed frame and FCS, flag, and the
flag pattern repeated to fill) in k data bytes, then the Reed-Solomon check bytes. All bytes LSB first, NRZI as above.
Code: GF(256), field polynomial 0x11d, generator roots alpha^1 .. alpha^nroots (RS(255, 255 - nroots), shortened to the
block size by leaving the unsent data bytes zero). The sender picks the smallest block that fits:

| Check bytes (setting) | Blocks (data + check bytes) | Corrects up to |
|---|---|---|
| 16 (default) | 32+16, 64+16, 128+16, 239+16 | 8 bad bytes per frame |
| 32 | 32+32, 64+32, 128+32, 223+32 | 16 |
| 64 | 64+64, 128+64, 191+64 | 32 |

Frames too large for a block are sent as plain HDLC. Receivers always try both: a tag is accepted with up to 8 of its 64
bits wrong, the block is corrected, and the frame inside must still pass its CRC. A receiver without FX.25 sees the tag and
check bytes as noise between flags and decodes the frame inside whenever it arrived error-free, so FX.25 and plain
stations can share a channel. FX.25 is used only where this station modulates: not with the OFDM modem (it has its own
FEC) and not in KISS TNC mode. Airtime estimates for burst packing and ACK timing use the FX.25 block size.

## Mesh header (23 bytes) followed by payload (max 96 bytes)
| Offset | Size | Field |
|---|---|---|
| 0 | 1 | type: 1 TEXT, 2 VOICE, 3 ACK, 4 ID, 5 REPORT (types 6-15 are relayed but not understood) |
| 1 | 1 | flags: bit0 ACK requested; bits 4-7 attempt number (ACK: serial) |
| 2 | 1 | ttl: remaining relay hops |
| 3 | 8 | source callsign, ASCII, NUL padded |
| 11 | 8 | destination callsign, or `CQ` for broadcast |
| 19 | 2 | message id (little endian) |
| 21 | 1 | fragment sequence number |
| 22 | 1 | total fragments (1..32) |

Messages are fragmented into at most 32 fragments; every fragment except the last has the full size of the
message's **fragment class**, carried in header flags bits 1-2: class 0 = 96 bytes (default), class 1 = 145 bytes
(used with OFDM so one fragment fills a 170-byte packet). Classes 2-3 are reserved: frames with them are ignored.
Receivers accept both classes whatever modem they use. Flags bit 0 = ACK requested, bits 4-7 = attempt number.

* **TEXT**: UTF-8.
* **VOICE**: byte 0 = format id (1 = Codec2 700C), then 4-byte Codec2 frames, 40 ms each (100 B/s).
* **REPORT**: `[schema id][schema version][bit stream]`, bit-packed MSB first and zero padded to a byte. Field order, widths and
  meanings come from the schema (`schemas/*.json`; compiled into `core/schemas_gen.c`). Per field: optional fields start with a
  presence bit; `enum` = ceil(log2(max code + 1)) bits; `uint` = `bits`; `flags` = one bit per flag; `time` = 24 bits, minutes since
  2020-01-01T00:00Z; `latlon` = 24-bit latitude `round((lat+90)/180*(2^24-1))` then 24-bit longitude `round((lon+180)/360*2^24) mod 2^24`
  (about 1.2 m / 2.4 m resolution); `text` = length (ceil(log2(max+1)) bits) then 6 bits per character from the 64-symbol set in
  `core/report.c`. A receiver rejects a wrong version, trailing bytes, non-zero padding or out-of-range values. Maximum 94 bytes
  (one class-0 fragment). Details: `docs/MAPS_AND_REPORTS.md`.
* **ACK**: payload = 4-byte little-endian bitmap of fragments received (selective repeat).
  The sender retransmits only missing fragments, with the attempt nibble incremented.
* **ID**: payload `DE <callsign>`. Sent after ~12 s of quiet following any transmission, and at
  least every 9 minutes while transmitting. Relay-only stations are identified this way, since a
  relayed frame still carries the original sender's callsign.

## Relay (simplex store-and-forward digipeater)
A station in relay mode re-sends any frame it hears that is not addressed to it, has `ttl > 0`,
and has not been seen in the last 5 minutes (dedup key = type, source, message id, fragment,
attempt, and for ACKs the bitmap). The copy has `ttl - 1`. Frames from one received burst share a
single random delay (120-820 ms) so the burst is re-sent together and in order. If another relay
is heard re-sending the same frame before ours goes out, ours is cancelled.

## Report repeats (optional)
A sender may transmit a broadcast REPORT again as a new message (new message id, identical payload); HamLink does this only
when *Repeat my reports* is on (default off): 1-2 extra copies, the first about 30 s after the original plus 0-10 s of random
spread. Receivers drop a REPORT whose source callsign and payload are identical to one seen in the last hour, so a repeat is
never shown twice. Addressed reports are not repeated: ARQ covers them.

## Channel access
After sending a frame that requests an ACK, a station keeps the channel free for the ACK: it starts no new burst for the
receiver's ACK delay (40-120 ms) plus its PTT lead-in, the ACK frame and 400 ms margin. Without this a queued frame (a
broadcast, say) went out as soon as the burst ended and collided with the ACK on the half-duplex channel.
Carrier sense uses data carrier detect (HDLC flags or bit-clock quality, see `docs/DSP_IMPROVEMENTS.md`) and, in the
default mode, also the audio level. Half duplex, carrier-sense. A station does not key up while the audio level shows the channel
busy, and waits a random 80-330 ms after the channel clears.

## OFDM modem option (audio link)
COFDMTV as in the Rattlegram app (vendored, see `third_party/aicodix`): 48 kHz audio, carrier 1500 Hz
(band 700-2300 Hz), 160 ms symbols + 1/8 guard = 180 ms, differential PSK, polar code with CRC32C.
A packet is [lead-in noise symbols] + sync + preamble + 4 payload symbols + tail. Payloads of up to 85 / 128 / 170
bytes select increasingly less robust codes automatically. The packet header carries the sender's callsign.
Mesh frames are bundled: payload = `0xB1`, then records `[len][frame]` (len 1..169), zero padded. `0xB1` can
never start valid UTF-8, so stock Rattlegram text messages are not mistaken for bundles (and are ignored).
In a burst only the first packet gets lead-in symbols and non-final packets keep just a 20 ms tail fade.
All stations must use the same modem; an OFDM-mode station can still decode AFSK, not the other way round.

## MT63-2000 modem option (audio link): slow but robust
MT63-2000 with the long interleave ("MT63-2000L", as fldigi sends it; vendored fldigi code, see `third_party/mt63`):
64 carriers from 500 to 2500 Hz (centre 1500 Hz), 20 symbols per second, one 7-bit character per symbol, Walsh-code FEC
spread over 64 symbols. Processed at 8 kHz, resampled to and from the 48 kHz audio.

A transmission is: the interleaver primed with 64 NUL characters (not sent, as fldigi does), NUL characters for the PTT
lead-in plus 1.6 s so the receiver locks, the frames, 63 NUL characters to flush the interleaver, and the jam symbol.
That fixed part costs about 5.4 s with the default 500 ms lead-in. Each mesh frame is sent as characters: `0x16 0x69`
(sync), then the bytes `[len][frame][CRC-16 over len and frame, low byte first]` (CRC as in the HDLC link layer) packed
7 bits per character, least significant bit first. Receivers find frames by the sync and keep them only if the CRC
passes. A 96-byte data fragment takes about 7 s of characters, an ACK about 1.9 s.

Mesh timing for MT63: airtime estimated at 140 bit/s plus the fixed part per burst, bursts up to 30 s so several frames
share that fixed part, ACK timeout 12 s (60 s with manual PTT). Carrier sense counts the MT63 receiver's lock as data.
fldigi tuned to MT63-2000L at 1500 Hz decodes the characters but shows HamLink frames as gibberish; plain text typed in
fldigi is ignored by HamLink. All stations must use the same modem.

## KISS TNC link
Each mesh frame is one AX.25 UI frame: destination `HAMLNK`, source = sender callsign (first 6 chars, SSID
digits honoured), control `0x03`, PID `0xF0`, info = mesh frame. It is sent as one KISS data frame (port 0,
standard FEND/FESC escaping). Received KISS frames are accepted only if they are data frames, UI/PID F0, and
addressed to `HAMLNK`. The AX.25 source address identifies the station on the air; the mesh header still carries
the full callsign.
