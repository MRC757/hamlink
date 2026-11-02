# Position reports, situation reports and the map

Everything here rides on the same radio link as text: a report is one small mesh message (type 5, REPORT) that is
ACKed, relayed and de-duplicated like any other. A position report is **13 bytes** of payload and a typical SALUTE
report **35 bytes** (worst case 72), so each fits in a single frame: roughly one second on the air at 1200 baud including
the PTT lead-in (computed from frame sizes, not measured on a radio).

Reports are **not encrypted** and anyone can read them. The format is public (this file, `docs/PROTOCOL.md`, `schemas/`),
which is what amateur rules about not obscuring a message's meaning generally ask for; check your own country's rules.

## Where "my position" comes from
Choose in **Map > Map setup** (Android) or **Map > Map setup...** (Windows). All sources feed one "current position";
the map, "Send my position" and the optional beacon use whichever is selected.

| Source | Android | Windows | Notes |
|---|---|---|---|
| Device GPS | yes | Windows location service, or a serial NMEA GPS on a COM port | GPS needs no internet. On Windows the location service is only accepted when its accuracy is within 150 m; Wi-Fi/IP positioning is refused rather than sent as if it were GPS. |
| Typed lat/lon | yes | yes | Decimal (`38.8895, -77.0353`), degrees-minutes-seconds with hemisphere letters, or an MGRS reference. |
| Maidenhead grid square | yes | yes | 4, 6 or 8+ characters (`FN31`, `FN31pr`, `FN31pr55`). Sent as the centre of the square; receivers draw an **uncertainty ring** (about 100 km for 4 characters, 3.5 km for 6, 550 m for 8). |

The position report carries the source (`GPS`, `Typed coordinates`, `Grid square (n char)`), so the receiver knows
whether a position is exact.

**Beacon:** off by default. 5-30 minute choices. Every beacon is a transmission on a shared simplex channel.

**Repeat my reports:** off by default. Reports sent to everyone (CQ) are not acknowledged, so one lost transmission loses the
report. With 1 or 2 extra copies selected, each position or SALUTE report you send goes out again about 30 s later (plus up
to 10 s of random spread, so two stations' repeats do not keep colliding), and again if 2 is chosen. Other stations drop the
identical copies, so the report appears once on their map and in their log. Beacons are repeated too. Reports addressed to
one station are never repeated: they are acknowledged and retried. Turning the option off cancels copies not yet sent. In
manual-PTT mode each copy waits for you to press Transmit, like any other frame.

## What the map shows
* **Station icons**: one per callsign that sent a position report. Blue rectangle by default.
* **Contact icons**: one per situation report, at the location *in the report* (where the reporter saw something, which is
  usually not where the reporter is). Shape and colour come from the report's assessment.
* **Station colour**: a station's icon shows the *Conditions at operator's location* of that station's latest
  **Station SITREP**: green for A (all stable), orange for B (moderate disruptions), red for C (severe disruptions). A
  station that has sent no SITREP, or whose SITREP has expired (4 h), is blue. SALUTE reports never change the station's
  colour: they colour only their own contact icon, since what an operator saw says nothing about the operator.
* **Ageing**: icons are fully opaque until the schema's `staleMin` (30 min), then fade linearly to 25% and are removed at
  `expireMin` (4 h for SALUTE, 24 h for positions). Your own icon never fades.
* **Tap an icon** for the full report, its age, and distance and bearing from you.

| Assessment | Colour | Shape |
|---|---|---|
| Friendly | blue | rectangle |
| Neutral | green | square |
| Unknown | yellow | quatrefoil |
| Suspect | orange | diamond |
| Hostile | red | diamond |

This follows the usual MIL-STD-2525 / APP-6 affiliation colours and shapes in a simplified form (the symbols are not the
standard's full icon set). **The assessment is the reporter's judgement**, sent as one field; the app does not infer it.

## SALUTE
Size, Activity, Location, Unit, Time, Equipment: the standard spot-report headings.

| Heading | Field(s) |
|---|---|
| Size | `size_n` (0-1023) and `size_what` (personnel, vehicles, aircraft, vessels, other) |
| Activity | `activity` (observing, moving, stationary, attacking, defending, digging in, withdrawing, patrolling, assembling, other), optional `activity_note`, optional `heading` (N..NW) |
| Location | `loc`: position of the **observed** thing, any format the position box accepts |
| Unit | `unit`: free text, up to 24 characters, optional |
| Time | `observed`: when it was seen (just now, 5 min ago ... 2 h ago); stored to the minute, UTC |
| Equipment | `equip`: any of small arms, crew-served weapons, light vehicles, armored vehicles, aircraft, UAS/drone, vessels, comms gear, explosives/IED, other; optional `equip_note` |
| (added) | `assess`: Friendly / Neutral / Unknown / Suspect / Hostile. Drives the colour. |

Free-text fields are limited to 24 characters each. Allowed: A-Z, 0-9, space and `. , - / : ; @ # + ( ) = ! ? ' " % * & _ < > [ ] $ ^ ~`;
lower case is converted to upper case, and anything else is refused with a message naming the field.

## Station SITREP
A short status report about the **operator's own station and area**, broadcast to everyone in range (CQ). Every field is a
drop-down. It carries no position and is not drawn on the map: receivers see a one-line summary in the message log, such
as `B: moderate disruptions / H: home`, and the full list in the report details. A typical SITREP is about 4 bytes of
payload. Open it from **New report...** and choose *Station SITREP* (the radio must be running).

| Field | Key | Choices |
|---|---|---|
| 1. Conditions at operator's location | `cond` | A: all stable, B: moderate disruptions, C: severe disruptions |
| 2. Operator's position | `pos` | H: home, M: mobile, P: portable |
| 3. Commercial power | `power` | Y: yes, I: intermittent, N: no |
| 4. Water | `water` | Y: yes, C: contaminated, N: no |
| 5. Sanitation | `sanitation` | Y: yes, N: no |
| 6. Medical | `medical` | Y: yes, N: no |
| 7. Grid communications | `comms` | Y: yes, N: no |
| 8. Transportation | `transport` | Y: yes, N: no |

Fields 1-2 are required. Fields 3-8 are optional and may be left at "(not set)" when all is OK and all services are available.

Version 2 (after HamLink 0.2.0) dropped the "location by state" field of version 1: it came from an HF long-distance
net format and is not needed on a local VHF/UHF channel. Versions 1 and 2 cannot read each other's SITREPs (shown as a
version mismatch), so all stations should run a build newer than 0.2.0.

## Maps and tiles
* The map is drawn from standard OpenStreetMap-style raster tiles (`{z}/{x}/{y}.png`).
* **Offline behaviour (default):** tiles are fetched only for the part of the map you are looking at, saved on the device,
  and reused with no internet later. To prepare for a trip, pan and zoom over your operating area once while online.
  The default cache limit is 300 MB (least recently used tiles are deleted first).
* **Why there is no "download this region" button:** OpenStreetMap's public tile servers do not allow bulk or offline
  downloading (see the OSM Foundation tile usage policy). The app follows that policy: no prefetch, at most 2 connections,
  an identifying User-Agent (set a contact address in Map setup; your callsign is used otherwise), HTTP caching honoured,
  and the attribution is always shown. If you need a whole region offline, use a tile source that permits it (your own
  server, a commercial provider, or a Protomaps/MBTiles extract) and put its URL in Map setup, or add an offline-pack
  `TileSource` (see `docs/EXTENDING.md`). Offline packs are not implemented yet.
* **Air-gapped Windows PC:** view the area on another PC and copy `%LOCALAPPDATA%\HamLink\tiles` across. Turn off
  "Download map tiles" so the app never tries the network.
* Showing the map needs no radio; icons appear only while the radio is running (the map model lives in the core).

## Time
Icon ageing and the "time" fields use the phone's or PC's clock (set into the core every 5 minutes). If a station's clock is
wrong, its reports look older or newer than they are. Phones and Windows PCs normally sync their clock; an air-gapped PC may
not. A report whose time is in the future is treated as brand new. Reports that arrive already expired are ignored.

## Text interfaces (for people extending the app)
**`key=value` lines** (what `hl_report_send` takes; the forms build this for you):
```
assess=Hostile
size_n=3
size_what=vehicles
activity=moving
loc=FN31pr                 # or 41.7292,-72.7083 or an MGRS reference
observed=1793000000        # unix seconds, or "now"
equip=light vehicles, comms gear
```
Enum values may be given by name (any case) or number; flags are comma separated names (`none` for no flags); blank optional
fields are omitted.

**Map snapshot** (`hl_map_snapshot`): one tab-separated line per icon:
`S|C  key  name/reporter  lat  lon  radius_m  #RRGGBB  shape  alpha  age_min  severity  label  self`.
Details for a key come from `hl_map_detail`. The apps draw only what these lines say.

## What is not verified
Everything above the C core (Android and Windows map, GPS, tile download, report forms) is code that has been
syntax-checked but **never compiled or run**; see the README status section. The core is tested: report encode/decode
(including a 200,000-case fuzz run under AddressSanitizer), map ageing and colours, and end-to-end delivery over the
simulated radio channel, directly and through a relay.
