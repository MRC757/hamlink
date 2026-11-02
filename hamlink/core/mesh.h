/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HamLink mesh layer: fragmentation, selective-repeat ARQ, flood relay with
 * duplicate suppression, CSMA scheduling and station ID.
 * Sans-IO: the caller feeds received frames and the clock, and polls for frames to send. */
#ifndef HAMLINK_MESH_H
#define HAMLINK_MESH_H
#include <stddef.h>
#include <stdint.h>

#define MESH_CALL_LEN 8
#define MESH_HDR_LEN 23
/* Fragment payload size is chosen per message and carried in header flags bits 1-2 ("class"):
 * class 0 = 96 bytes (1200 baud AFSK), class 1 = 145 bytes (sized so one fragment exactly fills a
 * 170-byte OFDM packet together with the 2-byte bundle framing). Receivers handle both. */
#define MESH_MAX_PAYLOAD 145
#define MESH_FRAG_CLASS_SMALL 0
#define MESH_FRAG_CLASS_LARGE 1
#define MESH_MAX_FRAME (MESH_HDR_LEN + MESH_MAX_PAYLOAD)
#define MESH_MAX_FRAGS 32
#define MESH_MAX_MSG (MESH_MAX_FRAGS * MESH_MAX_PAYLOAD)

enum { MT_TEXT = 1, MT_VOICE = 2, MT_ACK = 3, MT_ID = 4, MT_REPORT = 5 };
#define MT_LAST_KNOWN_RELAYED 15 /* types 5..15 are relayed untouched even if this build does not know them */
enum { MESH_ST_FAILED = 0, MESH_ST_DELIVERED = 1, MESH_ST_SENT = 2 };
enum { MESH_OK = 0, MESH_ERR_ARG = -1, MESH_ERR_BUSY = -2, MESH_ERR_TOOBIG = -3 };

typedef struct {
    char callsign[MESH_CALL_LEN + 1];
    int relay_enabled;
    uint8_t default_ttl;     /* hops a frame may be relayed (own frames) */
    uint8_t max_retries;     /* ARQ retries after the first attempt */
    uint32_t ack_timeout_ms; /* wait after our burst before retrying */
    uint32_t burst_cap_ms;   /* max airtime per PTT burst (keep under repeater/TOT limits) */
    uint32_t preamble_ms;    /* PTT-to-data lead-in, used only for airtime estimates */
    uint32_t frag_class;     /* fragment size class for messages we originate (see above) */
    uint32_t bps;            /* effective channel bit rate used for airtime estimates (default 1200) */
} mesh_cfg_t;

typedef struct {
    void (*on_message)(void *user, const char *src, const char *dst, int type,
                       const uint8_t *data, size_t len);
    void (*on_status)(void *user, uint16_t msgid, int status);
    void (*on_event)(void *user, const char *text); /* human-readable log lines */
} mesh_cb_t;

typedef struct mesh mesh_t;

void mesh_cfg_defaults(mesh_cfg_t *cfg, const char *callsign);
mesh_t *mesh_create(const mesh_cfg_t *cfg, const mesh_cb_t *cb, void *user, uint32_t seed);
void mesh_destroy(mesh_t *m);
void mesh_set_relay(mesh_t *m, int enabled);
/* Change fragment class, ACK timeout and the airtime-estimate bit rate on a live instance. */
void mesh_configure(mesh_t *m, int frag_class, uint32_t ack_timeout_ms, uint32_t bps);
/* Frames will go out as FX.25 with nroots (16, 32, 64) check bytes, or plain HDLC (0, the default): airtime estimates
 * for burst packing and ACK timing follow. */
void mesh_set_fec(mesh_t *m, int nroots);
/* Modems with a fixed per-transmission cost (MT63: receiver lock-in and interleaver flush) add extra_ms to every burst's
 * airtime estimate; cap_ms (nonzero) replaces the configured burst length cap so several frames can share that cost. */
void mesh_set_burst(mesh_t *m, uint32_t extra_ms, uint32_t cap_ms);
/* Fixed airtime every frame adds on top of its bytes (PSK: sync word, header, RS check bytes). 0 by default. */
void mesh_set_frame_extra(mesh_t *m, uint32_t ms);
/* Payload bytes per fragment for a class, 0 if unknown. */
int mesh_frag_size(int frag_class);

/* Queue a message. dst is a callsign or "CQ" for broadcast (no ACK). */
int mesh_send(mesh_t *m, const char *dst, int type, const uint8_t *data, size_t len,
              int ack_req, uint64_t now_ms, uint16_t *msgid_out);

/* Structural check of a received frame (type, callsigns, fragment numbering and sizes) without acting on it. Used to vet
 * frames recovered by bit repair, where a false CRC match is possible though rare. 1 = plausible. */
int mesh_frame_plausible(const uint8_t *frame, size_t len);
void mesh_rx_frame(mesh_t *m, const uint8_t *frame, size_t len, uint64_t now_ms);
void mesh_set_channel_busy(mesh_t *m, int busy, uint64_t now_ms);
void mesh_tick(mesh_t *m, uint64_t now_ms);

/* If the channel is clear and something is due, copies up to max_frames frames
 * back-to-back into buf (lens[i] each) and returns the count (0 = nothing to send).
 * The caller should key PTT, transmit the burst, and unkey. */
int mesh_poll_tx(mesh_t *m, uint64_t now_ms, uint8_t *buf, size_t cap, size_t *lens,
                 int max_frames);

/* Next time (ms) the caller should call mesh_tick/mesh_poll_tx again; 0 = idle. */
uint64_t mesh_next_wakeup(const mesh_t *m);

/* Call after a transmission made outside the mesh (e.g. a test tone) so the station still identifies:
 * marks us as having transmitted and queues a station ID to go out right after it ends. */
void mesh_note_external_tx(mesh_t *m, uint64_t now_ms, uint32_t duration_ms);

uint32_t mesh_frame_airtime_ms(size_t frame_len);
/* Same estimate at an arbitrary bit rate (e.g. a 9600 baud TNC). */
uint32_t mesh_airtime_ms(size_t frame_len, uint32_t bps);
int mesh_pending_tx(const mesh_t *m);

typedef struct {
    uint32_t rx_frames, rx_dups, tx_frames, relayed, relay_suppressed, retries;
} mesh_stats_t;
void mesh_get_stats(const mesh_t *m, mesh_stats_t *out);

#endif
