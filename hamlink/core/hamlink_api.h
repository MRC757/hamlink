/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Flat C API over the HamLink core, for P/Invoke (Windows) and ctypes (tests).
 * One context per radio. NOT thread safe: call everything from one engine thread; callbacks run
 * synchronously on that thread inside hl_process_audio / hl_send / hl_poll_tx. */
#ifndef HAMLINK_API_H
#define HAMLINK_API_H
#include <stdint.h>

#if defined(_WIN32)
#define HL_API __declspec(dllexport)
#define HL_CALL __cdecl
#else
#define HL_API __attribute__((visibility("default")))
#define HL_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hl_ctx hl_ctx;

typedef void(HL_CALL *hl_message_cb)(void *user, const char *src, const char *dst, int type,
                                     const uint8_t *data, int len);
typedef void(HL_CALL *hl_status_cb)(void *user, int msg_id, int status); /* 0 failed 1 delivered 2 sent */
typedef void(HL_CALL *hl_event_cb)(void *user, const char *text);

#define HL_TYPE_TEXT 1
#define HL_TYPE_VOICE 2

HL_API int HL_CALL hl_api_version(void); /* bump when the ABI changes */

HL_API hl_ctx *HL_CALL hl_create(const char *callsign, int relay, int ttl, int preamble_ms, uint32_t seed,
                                 hl_message_cb on_msg, hl_status_cb on_status, hl_event_cb on_event, void *user);
HL_API void HL_CALL hl_destroy(hl_ctx *c);
HL_API void HL_CALL hl_set_relay(hl_ctx *c, int on);

/* Feed 48 kHz mono int16 audio (n may be 0 just to run timers). Returns smoothed RMS level 0..1. */
HL_API float HL_CALL hl_process_audio(hl_ctx *c, const int16_t *pcm, int n, int64_t now_ms, float busy_threshold);

/* Returns message id (0..65535) or negative: -1 bad arg, -2 queue busy, -3 too big. */
HL_API int HL_CALL hl_send(hl_ctx *c, const char *dst, int type, const uint8_t *data, int len, int ack_req,
                           int64_t now_ms);

/* If the channel is clear and frames are due, returns pointer to 48 kHz int16 audio for one PTT burst
 * and sets *n_samples; else returns NULL. Buffer is owned by the context, valid until the next call. */
HL_API const int16_t *HL_CALL hl_poll_tx(hl_ctx *c, int64_t now_ms, float amp, int *n_samples);
HL_API int64_t HL_CALL hl_next_wakeup(hl_ctx *c); /* ms timestamp, 0 = idle */

/* Voice clips: 8 kHz int16 <-> Codec2 700C clip bytes. Return counts (0 on error). */
HL_API int HL_CALL hl_voice_encode(hl_ctx *c, const int16_t *pcm8k, int n, uint8_t *out, int cap);
HL_API int HL_CALL hl_voice_decode(hl_ctx *c, const uint8_t *clip, int len, int16_t *pcm8k, int cap);
HL_API int HL_CALL hl_voice_max_clip_bytes(void);

/* ---- PTT test / calibration (API v2) ---- */
#define HL_TONE_OFF 0   /* PTT only, silence */
#define HL_TONE_1200 1
#define HL_TONE_2200 2
#define HL_TONE_ALT 3   /* 1200 Hz 1 s, 2200 Hz 1 s, repeating */
/* Start a tone sequence (resets phase, fades in from zero). */
HL_API void HL_CALL hl_tone_start(hl_ctx *c, int mode);
/* Fill n samples at 48 kHz toward amplitude `amp` (0..1). Returns the amplitude reached (0 once faded out). */
HL_API float HL_CALL hl_tone_fill(hl_ctx *c, int16_t *out, int n, float amp);
/* Tell the mesh we transmitted outside it for duration_ms starting at now_ms; queues a station ID. */
HL_API void HL_CALL hl_note_external_tx(hl_ctx *c, int64_t now_ms, int duration_ms);
/* Receive meter, updated every 100 ms of audio passed to hl_process_audio. out5 = {rms_dBFS, peak_dBFS,
 * level of 1200 Hz tone dBFS, level of 2200 Hz tone dBFS, clipped sample count in the last block}. */
HL_API void HL_CALL hl_get_meter(hl_ctx *c, float *out5);

/* ---- modem choice and TNC link (API v3) ---- */
#define HL_MODEM_AFSK 0 /* 1200 baud Bell 202 AFSK + HDLC (default) */
#define HL_MODEM_OFDM 1 /* COFDMTV (Rattlegram family): polar-coded OFDM; not faster than AFSK */
#define HL_MODEM_MT63 2 /* MT63-2000L (API v9): slow (20 characters/s, ~5 s overhead per transmission) but very robust:
                         * decodes at -3 dB SNR in simulation, about 9 dB below AFSK */
#define HL_MODEM_PSK2400 3 /* (API v10) single-carrier QPSK, 1200 baud at 1700 Hz, 2400 bit/s on the air, RS per frame */
#define HL_MODEM_PSK3600 4 /* (API v10) the same with 8-PSK, 3600 bit/s; needs a cleaner signal. A PSK station decodes both */
#define HL_LINK_AUDIO 0 /* sound card + PTT: hl_process_audio / hl_poll_tx */
#define HL_LINK_TNC 1   /* radio with a built-in KISS TNC (Bluetooth SPP / serial): hl_tnc_rx / hl_tnc_poll_tx */
/* Select the transmit modem for the audio link. Receive: AFSK is always decoded, OFDM too once it has been selected, MT63
 * while it is selected, and both PSK rates while either is selected. All stations that must hear each other need the same
 * modem (PSK 2400 and 3600 stations hear each other). Returns 0, or -1 if the modem could not start (AFSK stays selected). */
HL_API int HL_CALL hl_set_modem(hl_ctx *c, int modem);
/* Choose audio or TNC link. tnc_baud (1200 or 9600) is only used to estimate airtime for ACK timing. */
HL_API void HL_CALL hl_set_link(hl_ctx *c, int link, int tnc_baud);
/* TNC link: bytes read from the KISS stream (any chunking; n may be 0 just to run timers). */
HL_API void HL_CALL hl_tnc_rx(hl_ctx *c, const uint8_t *bytes, int n, int64_t now_ms);
/* TNC link: KISS bytes to write to the radio now, or NULL. Paced by estimated airtime. Valid until the next call. */
HL_API const uint8_t *HL_CALL hl_tnc_poll_tx(hl_ctx *c, int64_t now_ms, int *n_bytes);
/* ---- hand-operated PTT (API v4) ---- */
/* Number of frames queued for transmission (ACKs, relays, IDs, message fragments), whether or not due yet. */
HL_API int HL_CALL hl_pending_tx(hl_ctx *c);
/* Tell the core a human keys the radio for every transmission: ACK/retry timers are stretched to 45 s so a
 * sender does not give up while the other station's operator gets ready to key an ACK. */
HL_API void HL_CALL hl_set_manual_ptt(hl_ctx *c, int on);
/* ---- geo helpers (API v5). All strings are ASCII. ---- */
#define HL_FMT_DECIMAL 0
#define HL_FMT_DMS 1
#define HL_FMT_MAIDENHEAD 2
#define HL_FMT_MGRS 3
/* Parse typed decimal/DMS coordinates, a Maidenhead locator (4-10 chars) or an MGRS reference. kind: 0 lat/lon,
 * 1 Maidenhead, 2 MGRS. radius_m = uncertainty of the typed value. Returns 1 on success. */
HL_API int HL_CALL hl_geo_parse(const char *text, double *lat, double *lon, double *radius_m, int *kind);
/* Format a position for display (fmt = HL_FMT_*). Returns the string length, 0 on error. */
HL_API int HL_CALL hl_geo_format(double lat, double lon, int fmt, char *out, int cap);
HL_API int HL_CALL hl_geo_maidenhead(double lat, double lon, int chars, char *out, int cap);
HL_API int HL_CALL hl_geo_mgrs(double lat, double lon, int digits, char *out, int cap);
/* Web Mercator world pixel (256 px tiles) <-> latitude/longitude. */
HL_API void HL_CALL hl_geo_to_world(double lat, double lon, int zoom, double *px, double *py);
HL_API void HL_CALL hl_geo_from_world(double px, double py, int zoom, double *lat, double *lon);
HL_API double HL_CALL hl_geo_distance_m(double lat1, double lon1, double lat2, double lon2);
HL_API double HL_CALL hl_geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);
/* Serial GPS: feed raw NMEA bytes. Returns 1 and fills out6 = {lat, lon, speed km/h, course deg, hdop, satellites}
 * when a valid fix completed. */
HL_API int HL_CALL hl_nmea_feed(hl_ctx *c, const uint8_t *bytes, int n, double *out6);

/* ---- reports and map (API v5) ---- */
#define HL_TYPE_REPORT 5
/* Tell the core the current wall-clock time (unix seconds) at engine time now_ms. Needed for report timestamps
 * ("time=now") and for ageing icons; without it nothing fades and "now" is rejected. Call again when GPS/NTP corrects it. */
HL_API void HL_CALL hl_set_clock(hl_ctx *c, int64_t unix_s, int64_t now_ms);
/* Report schemas compiled from the JSON files in the schemas directory. hl_schema_json returns static JSON (fields, choices, colours) for the
 * UIs to build entry forms from; NULL past the end. */
HL_API int HL_CALL hl_schema_count(void);
HL_API const char *HL_CALL hl_schema_json(int index);
/* Build a report from "key=value" lines (see docs/MAPS_AND_REPORTS.md) and send it (dst "CQ" = everyone in range).
 * Returns the message id (>= 0), a mesh error -1..-3, or -100 + report error (-101 bad arg ... -107 too large) with a
 * human readable reason in err. Our own report is also shown on our map. */
HL_API int HL_CALL hl_report_send(hl_ctx *c, const char *dst, const char *schema, const char *kv, int ack_req,
                                  int64_t now_ms, char *err, int errcap);
/* One-line / multi-line text for a received MT_REPORT payload (0 if it does not decode). */
HL_API int HL_CALL hl_report_summary(const uint8_t *data, int len, char *out, int cap);
HL_API int HL_CALL hl_report_detail(const uint8_t *data, int len, char *out, int cap);
/* Our own position from any source (GPS, typed, grid); radius_m = uncertainty. Does not transmit. */
HL_API void HL_CALL hl_map_set_self_position(hl_ctx *c, double lat, double lon, double radius_m, int64_t now_ms);
/* Tab separated item list, one line each (format in core/mapmodel.h). Returns full length; truncates at cap. */
HL_API int HL_CALL hl_map_snapshot(hl_ctx *c, int64_t now_ms, char *out, int cap);
HL_API int HL_CALL hl_map_detail(hl_ctx *c, const char *key, int64_t now_ms, char *out, int cap);
HL_API void HL_CALL hl_map_clear(hl_ctx *c);

/* ---- report repeats (API v6) ---- */
/* Send each broadcast (CQ) report we originate extra_copies more times (0..3; 0 = off, the default), the first about
 * interval_ms after the original (10 s..10 min, default 30 s) plus 0-10 s of random spread. Receivers drop identical
 * copies from the same station seen within the last hour, so the map and the app see each report once. Addressed
 * reports are not repeated: they are acknowledged and retried by ARQ. */
HL_API void HL_CALL hl_set_report_repeat(hl_ctx *c, int extra_copies, int interval_ms);

/* ---- receive DSP options (API v7) ---- */
#define HL_CS_OFF 0           /* never hold off (acoustic tests in a noisy room, or a repeater channel) */
#define HL_CS_DATA 1          /* hold off only while data is detected (AFSK carrier detect). With OFDM, also on level */
#define HL_CS_DATA_OR_LEVEL 2 /* data detected, or audio above busy_threshold (voice on the channel). Default */
HL_API void HL_CALL hl_set_carrier_sense(hl_ctx *c, int mode);

/* (API v11) Listen on all modems: every receiver runs (AFSK, OFDM, MT63, PSK 2400/3600), whatever modem is selected, and
 * channel busy counts them all. Transmit stays on the selected modem, except that relayed frames go out on the modem
 * they arrived on. Replies and ACKs still use the selected modem. Off by default. */
HL_API void HL_CALL hl_set_listen_all(hl_ctx *c, int on);
/* (API v11) Inside a message or monitor callback: the modem (HL_MODEM_*) that decoded the frame; -1 elsewhere. */
HL_API int HL_CALL hl_rx_modem(hl_ctx *c);

/* (API v11) Monitor mode: messages between other stations are reassembled and reported through the monitor callback,
 * never acknowledged; relaying is unaffected. The callback also gets overheard ACKs (type 3, msg_id = the message
 * acknowledged) and station IDs (type 4, dst "CQ"). Overheard position and status reports update the map. Off by
 * default. */
typedef void(HL_CALL *hl_monitor_cb)(void *user, const char *src, const char *dst, int type, int msg_id,
                                     const uint8_t *data, int len);
HL_API void HL_CALL hl_set_monitor(hl_ctx *c, int on);
HL_API void HL_CALL hl_set_monitor_callback(hl_ctx *c, hl_monitor_cb cb, void *user);

/* (API v11) Separate station ID frames (default on). Off: none after this station's own messages, ACKs and reports,
 * which carry its callsign in every frame header; an ID is still sent after relaying another station's frames and
 * after a test tone, since those transmissions do not carry it. A station that only listens never transmits either
 * way. Check that this meets your licence's identification rules. */
HL_API void HL_CALL hl_set_station_id(hl_ctx *c, int on);
/* Single-bit repair of AFSK frames that fail their CRC (default on). Only the 8 least certain bit decisions are tried,
 * a repair is accepted only when exactly one candidate passes, and repaired frames must pass a structural check. */
HL_API void HL_CALL hl_set_bit_repair(hl_ctx *c, int on);
/* Channel-busy decision from the last hl_process_audio call (what carrier sense used). */
HL_API int HL_CALL hl_channel_busy(hl_ctx *c);
/* out6 = {AFSK data detected now, AFSK frames decoded, of which repaired, repairs rejected as ambiguous,
 *         repairs rejected by the structural check, channel busy now} */
HL_API void HL_CALL hl_get_rx_info(hl_ctx *c, uint32_t *out6);

/* ---- forward error correction on the AFSK link (API v8) ---- */
/* Send AFSK frames as FX.25 with 16, 32 or 64 Reed-Solomon check bytes (corrects up to 8, 16 or 32 bad bytes per frame),
 * or 0 for plain HDLC. Default 16. FX.25 is backward compatible: stations without it still decode the frame inside when
 * it arrives error-free. Receiving FX.25 is always on. Costs airtime: about +25% on a full data frame, nearly double on an
 * ACK. Ignored for OFDM (it has its own FEC) and in TNC mode. Returns 0, or -1 for any other value. */
HL_API int HL_CALL hl_set_fec(hl_ctx *c, int check_bytes);
/* out3 = {FX.25 blocks decoded, frames only FEC recovered (the plain decode missed them), bytes corrected} */
HL_API void HL_CALL hl_get_fec_info(hl_ctx *c, uint32_t *out3);

/* out8 = {rx_frames, rx_dups, tx_frames, relayed, relay_suppressed, retries, ofdm_packets_ok, ofdm_packets_failed} */
HL_API void HL_CALL hl_get_stats(hl_ctx *c, uint32_t *out8);

#ifdef __cplusplus
}
#endif
#endif
