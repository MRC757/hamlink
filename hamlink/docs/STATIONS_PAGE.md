# Stations page and monitor mode: plan

Status (2026-11-04): **steps 1-4 done** in both apps: the message history, the Stations page, unread counts, search,
"Show on map", and a "Messages" button on map markers. Both apps build; not yet tried on a phone or on the air. Next:
step 5, monitor mode in the core.

A third page in the Windows and Android apps, beside **Messages** and **Map**, that shows traffic one station at a time.
With **monitor mode** on, it also shows messages between *other* stations that this station overheard.

Decisions so far:
* **Retention:** 5 days.
* **CQ:** a separate "All stations (CQ)" entry holds the messages this station sent to CQ.
* **Monitor mode:** planned below (section 5). **Off by default.**
* **Listen on all modems:** a second setting beside it (section 5a). **Off by default.**

## What the operator sees

**Station list.** Every station heard, messaged or overheard, most recent first. Each row shows:
* the callsign;
* the SITREP colour (as on the map);
* when it was last heard;
* distance and direction, if it has sent a position;
* the number of unread messages;
* a small "overheard" marker if all its traffic was between other stations.

A search box filters by callsign. "All stations (CQ)" is always at the top.

**One station.** Everything to and from it, in time order:
* **content:** text, voice clips (Play), reports and positions;
* **delivery status:** shown on messages this station sent: queued, sent, delivered or FAILED;
* **overheard messages:** shown in grey as "W1AAA → W2BBB", with "delivered" once the recipient's ACK is heard.

Underneath is a message box already addressed to the station, plus hold-to-talk. A **Show on map** button opens the
map with the station selected. The other way round, a **Messages** button on a map marker opens this page.

**Messages page:** unchanged. With monitor mode on, overheard messages also appear there, in grey.

## 1. Message store (both apps)

Today both apps keep one list of pre-formatted text lines mixed with events. It is capped at 300 to 400 lines and lost
on exit, and a received message does not record its recipient. A small store sits alongside that list, and the
Messages page keeps working as it does.

**One record per message:**

| Field | |
|---|---|
| time | |
| direction | in, out, or overheard |
| station | the other party: the sender of a received message, the recipient of a sent one; for an overheard message, both callsigns |
| to | me, CQ, or a callsign |
| type | text, voice, report, position |
| content | text, a report summary, or a voice clip in its Codec2 form (about 500 bytes for 5 s, decoded when played) |
| message ID and status | for sent messages, and for overheard ones once an ACK is heard |

**Where records come from:**
* **received:** the receive handler, which already gets sender, recipient, type and data from the core (Windows
  `Engine.OnMessage`, Android `Engine.kt` `onMessage`);
* **sent:** the send paths for text, voice, reports and positions;
* **status:** status updates go to the same record;
* **overheard:** the new monitor callback (section 5).

**Saving:**
* one JSON line per message, appended as messages arrive: Windows `%LOCALAPPDATA%\HamLink\messages.jsonl`,
  Android the app's files folder;
* loaded at start; anything older than **5 days** is dropped when loading;
* **Clear message history** in Settings;
* unread state: the time each station was last opened, saved alongside.

## 2. Station list

The list merges three sources:
* the message store;
* the map's station data the core already provides (position, SITREP colour, age);
* this station's own position, for distance and direction.

Sorted by last activity, with "All stations (CQ)" pinned at the top. A received CQ message is listed under its sender;
messages this station sent to CQ are listed under "All stations (CQ)".

An overheard message W1AAA → W2BBB appears on **both** stations' pages. The **Show overheard** switch at the top of
the list hides or shows it; it is only there while monitor mode is on.

## 3. Windows

* **Navigation:** a third **Stations** button beside Messages and Map.
* **Layout:** a two-pane panel covering the same rows as the map:
  * the station list and search box on the left (about 220 px);
  * the conversation on the right, with a message box under it that reuses the existing send code with the
    station filled in.
* **Manual PTT:** the Transmit panel stays at the bottom, as it does on the Map page.

## 4. Android

* **Navigation:** a third **Stations** tab beside Messages and Map.
* **Screens:** the list fills the screen; tapping a station opens its conversation screen, and Back returns to the
  list.
* **Keyboard:** the conversation reuses the Messages tab's keyboard handling, which hides the header while typing.
* **Unread:** a count on the tab.

## 5. Monitor mode (core change, API version 11)

**Today**, the core reassembles and passes on only messages addressed to this station or to CQ
(`mesh_rx_frame`: `to_me || bcast`). Data frames for other stations are relayed when relay mode is on, otherwise
ignored, and ACKs between other stations are ignored. Station IDs only produce a "HEARD" event line.

**With monitor mode**, the core also reassembles messages between other stations and reports them through a separate
callback.

### Core (`mesh.c`)

* **Monitored frames:** when monitor is on, data frames that are neither to me nor broadcast also go through
  `handle_data`, flagged as monitored:
  * **never ACKed:** the `to_me && F_ACKREQ` path is skipped;
  * **own reassembly pool:** they use a separate pool of slots (8 more), so heavy traffic between other stations can
    never push out a message addressed to this station. Today all messages share 8 slots, and the oldest is reused
    when they run out.
  * **delivered once,** when complete, through the new callback.
* **Relaying is unchanged.** Monitoring only adds a listener. Duplicate suppression already keeps a relayed copy from
  being reported twice.
* **Overheard ACKs:** reported through the same callback (type ACK, with the message ID), so the apps can mark the
  overheard message "delivered".
* **Station IDs:** reported as type ID, so a station that only sent its ID still appears in the list.
* **Incomplete messages:** an overheard message with a missing part is never re-sent to us, because we were not its
  recipient. Phase 1 drops it. A later option: when its slot expires (5 min), report "W1AAA → W2BBB: 3 of 5 parts
  heard" without the content.

### API (`hamlink_api.h`)

* `hl_set_monitor(hl_ctx *c, int on)`
* `hl_set_monitor_callback(hl_ctx *c, hl_monitor_cb cb, void *user)`, where
  `hl_monitor_cb(void *user, const char *src, const char *dst, int type, const uint8_t *data, int len)`.

These are separate calls rather than a new `hl_create` argument, so existing callers only need the version bump.
`HL_API_VERSION` becomes 11, and the version checks in the Windows and Android apps, the Pi service,
`tools/replay_wav.py`, `tools/score_tnc_cd.py`, `tools/bench_modems.py` and `tests/test_api.py` move to 11 together.

### Map

Overheard **position and SITREP reports** go to the map too: a station's position does not depend on whom it was
reporting to. Markers from overheard reports look the same as others; the map detail line says "(overheard)".

### Apps

* **Setting:** **Monitor all traffic (show messages between other stations)**, off by default, beside Relay in
  Settings.
* **When it is on:** overheard messages go into the store as direction "overheard", appear grey on the Messages page,
  and appear on both stations' pages.
* **Voice clips:** overheard voice clips can be played.

### Pi relay

`hamlinkd` turns monitor mode on always. Its web page gains a **Traffic** list of the messages it heard and relayed,
which shows whether the relay is doing its job.

### Rules

Amateur traffic is unencrypted and public, so displaying it raises no issue. Monitor mode only listens: it never
transmits, never ACKs, and does not change what the relay sends.

## 5a. Listen on all modems (core change, same API version 11)

**Today**, the receivers that run depend on the selected modem (`hl_process_audio`):

| Modem | Decoded |
|---|---|
| AFSK 1200 | always |
| OFDM | from the first time OFDM is selected until the radio stops |
| MT63 | only while MT63 is selected |
| PSK 2400 and 3600 | both, while either is selected |

**With the setting on**, every receiver runs at once, whatever modem is selected for sending. Each modem's frames
carry their own CRC, so extra receivers cannot produce false messages. A station hears traffic sent on any modem,
which is useful for a relay, for monitoring, and when other stations have not agreed on a modem.

### Core

* `hl_set_listen_all(hl_ctx *c, int on)`: creates the OFDM, MT63 and PSK receivers if they do not exist yet; only the
  receivers, not the transmitters. While it is on, `hl_process_audio` feeds them all. Turning it off returns to
  today's rule.
* **Transmit is unchanged:** the station still sends on its selected modem.
* **Carrier sense:** while listening to all modems, "channel busy" includes every running receiver: AFSK carrier
  detect, MT63 lock, PSK busy, and an OFDM busy signal. The OFDM signal is new; it is on from sync detection to the
  end of the packet. Today only the selected modem's detector counts, and OFDM's never does.
* **Relaying:** a relay that listens to all modems re-sends what it hears **on its own selected modem**, not the one
  it arrived on. That is the simplest rule, but it converts traffic. This needs a decision before step 6; the
  alternative, re-sending on the modem it arrived on, needs the frame to carry its modem through the queue.

### What it does not do: replies on another modem

Replies and acknowledgements go out on the station's own selected modem. If W1AAA sends on PSK 2400 and this station
replies on MT63, W1AAA hears the reply only if it also listens to all modems. Otherwise its message is reported
FAILED (no ACK) even though it arrived. The setting's description and the Help say so. Replying on the sender's modem
automatically would be a protocol change and is not part of this plan.

### Processor load

Measured per receiver: 0.5-2.4% of one core on the Windows PC, 0.8-1.4% on GitHub's ARM64 machines, so roughly 5% of
a core for all of them. Still to measure on a Pi Zero 2 W. `tools/bench_modems.py` gains an `--all` run with every
receiver at once, so CI reports the combined load too.

### Apps and Pi

* **Setting:** **Listen on all modems (hear stations on any modem; replies still go out on yours)**, off by default,
  beside Monitor in Settings (Windows and Android), and in the Pi relay's settings and web page.
* **Received messages:** messages decoded by a modem other than the selected one are marked with that modem (for
  example "via PSK 2400") on the Messages and Stations pages. This needs the callback to report which receiver
  decoded the frame; it is added alongside the monitor callback.

## 6. Tests

**Core (`tests/test_mesh.py` / `test_api.py`, simulated channel):**
* A sends to B while C, out of the path, monitors: C reports the message once and transmits nothing, and B's ACK
  shows up at C as an overheard ACK;
* a multi-part message reassembles at C;
* with monitor off, C reports nothing (today's behaviour);
* a message to C arrives intact while C is overhearing more than 8 other conversations, so the separate pool works;
* relaying with monitor on is unchanged: same frames, same counts;
* listen on all modems: a station set to AFSK decodes messages from four senders on PSK 2400, PSK 3600, OFDM and MT63;
  with the setting off it decodes none of them; with the setting on, channel busy is reported during each modem's
  transmission.

**Apps:**
* unit tests for the message store: grouping by station, the CQ entry, the 5-day cutoff, saving and loading;
* overheard messages on both stations' pages.

**On the air:** three devices (two apps and the Pi relay, or two apps and a third radio) on MT63 or AFSK:
* conversations show on the right pages and survive a restart;
* overheard messages show with monitor on, and not with it off;
* Clear message history works.

## Order of work

| Step | Work | Size |
|---|---|---|
| 1 | Message store and saving, both apps (no visible change) | medium |
| 2 | Windows Stations page | medium |
| 3 | Android Stations page | medium |
| 4 | Unread counts, search, map links | small |
| 5 | Core: monitor mode and listen on all modems, API 11, core tests, combined processor benchmark | medium |
| 6 | Both settings (off by default) and overheard / other-modem display in both apps; Pi settings and Traffic list | small-medium |
| 7 | Help section, README screenshot, release | small |

Steps 1 to 4 are useful on their own and can be released before monitor mode.
