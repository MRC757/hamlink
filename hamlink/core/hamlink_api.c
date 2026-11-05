/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hamlink_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "afsk.h"
#include "audiocheck.h"
#include "mesh.h"
#include "mt63.h"
#include "psk.h"
#include "geo.h"
#include "mapmodel.h"
#include "report.h"
#include "nmea.h"
#include "ofdm.h"
#include "tnc.h"
#include "tone.h"
#include "voice.h"

#define HL_API_VERSION 11
#define HL_FEC_DEFAULT 16

#define REPEAT_SLOTS 8
#define SEEN_SLOTS 64
#define SEEN_MS (60LL * 60 * 1000) /* identical reports from the same station within 1 h are duplicates */
typedef struct { int used, left; int64_t due; uint8_t len; uint8_t data[REPORT_MAX_BYTES]; } repeat_t;
typedef struct { uint32_t hash; int64_t t; } seen_t;

struct hl_ctx {
    mesh_t *mesh;
    afsk_demod_t *demod;
    voice_codec_t *voice;
    int preamble_ms;
    hl_message_cb on_msg;
    hl_status_cb on_status;
    hl_event_cb on_event;
    void *user;
    tone_gen_t tone;
    meter_t meter;
    char callsign[16];
    int modem, link, manual;
    int fec;                     /* FX.25 check bytes on the AFSK audio link (0 = plain AX.25-style HDLC) */
    uint32_t tnc_baud;
    ofdm_tx_t *otx;
    ofdm_rx_t *orx;
    mt63_tx_t *mtx;
    mt63_rx_t *mrx;
    psk_rx_t *prx;
    int listen_all;              /* run every receiver, whatever modem is selected (hl_set_listen_all) */
    int rx_modem;                /* modem that decoded the frame being handled (hl_rx_modem), -1 outside a frame */
    hl_monitor_cb on_mon;        /* monitor mode callback (hl_set_monitor_callback) */
    void *mon_user;
    int monitor;
    kiss_dec_t kiss;
    nmea_t nmea;
    map_model_t *map;
    int clock_set;
    int carrier_mode;            /* HL_CS_OFF / HL_CS_DATA / HL_CS_DATA_OR_LEVEL (default) */
    int busy;                    /* last channel-busy decision */
    uint32_t repair_rejected;    /* repaired frames that failed the structural check */
    int repeat_extra;            /* extra copies of each broadcast report we originate (0 = off, the default) */
    int64_t repeat_interval_ms;
    repeat_t rep[REPEAT_SLOTS];
    seen_t seen[SEEN_SLOTS];
    int seen_next;
    audiocheck_t audiocheck;     /* warns once about an input that suppresses steady signals */
    uint32_t rng;
    int64_t last_ms; /* most recent engine time seen; mesh callbacks do not carry it */
    int64_t clock_unix, clock_ms;
    int64_t tnc_free_at, tnc_now;
    uint8_t tncout[16384];
    int16_t *txaudio;
    size_t txcap;
    uint8_t txbuf[4096];
    size_t txlens[32];
};

static int is_psk(int modem) { return modem == HL_MODEM_PSK2400 || modem == HL_MODEM_PSK3600; }
static int psk_mode(int modem) { return modem == HL_MODEM_PSK3600 ? PSK_8PSK : PSK_QPSK; }
/* Frames handed to the mesh next were decoded by this modem (-1: none). Relays of them go out on it again; the selected
 * modem is passed as -1 so its frames share bursts with our own. */
static void rx_from(hl_ctx *c, int modem) {
    c->rx_modem = modem;
    mesh_set_rx_modem(c->mesh, modem == c->modem ? -1 : modem);
}

static int64_t unix_now(hl_ctx *c, int64_t now_ms) {
    return c->clock_set ? c->clock_unix + (now_ms - c->clock_ms) / 1000 : 0;
}

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* 1 if this exact report from this station was already seen within SEEN_MS (a sender's repeat, or a copy that took
 * another path); else remembers it and returns 0. */
static int report_seen(hl_ctx *c, const char *src, const uint8_t *data, size_t len, int64_t now) {
    uint32_t h = fnv1a(fnv1a(2166136261u, (const uint8_t *)src, strlen(src) + 1), data, len);
    if (h == 0) h = 1;
    for (int i = 0; i < SEEN_SLOTS; i++)
        if (c->seen[i].hash == h && now - c->seen[i].t < SEEN_MS) return 1;
    c->seen[c->seen_next].hash = h;
    c->seen[c->seen_next].t = now;
    c->seen_next = (c->seen_next + 1) % SEEN_SLOTS;
    return 0;
}

static void cb_message(void *u, const char *src, const char *dst, int type, const uint8_t *data, size_t len) {
    hl_ctx *c = (hl_ctx *)u;
    if (type == MT_REPORT) {
        if (report_seen(c, src, data, len, c->last_ms)) return; /* duplicate: neither the map nor the app sees it twice */
        if (c->map) map_ingest(c->map, src, data, len, unix_now(c, c->last_ms));
    }
    if (c->on_msg) c->on_msg(c->user, src, dst, type, data, (int)len);
}
static void cb_monitor(void *u, const char *src, const char *dst, int type, uint16_t id, const uint8_t *data,
                       size_t len) {
    hl_ctx *c = (hl_ctx *)u;
    if (type == MT_REPORT) {
        /* an overheard position or SITREP still places the station on the map: where it is does not depend on whom it
         * was reporting to */
        if (report_seen(c, src, data, len, c->last_ms)) return;
        if (c->map) map_ingest(c->map, src, data, len, unix_now(c, c->last_ms));
    }
    if (c->on_mon) c->on_mon(c->mon_user, src, dst, type, (int)id, data, (int)len);
}
static void cb_status(void *u, uint16_t id, int status) {
    hl_ctx *c = (hl_ctx *)u;
    if (c->on_status) c->on_status(c->user, (int)id, status);
}
static void cb_event(void *u, const char *text) {
    hl_ctx *c = (hl_ctx *)u;
    if (c->on_event) c->on_event(c->user, text);
}

int HL_CALL hl_api_version(void) { return HL_API_VERSION; }
int HL_CALL hl_voice_max_clip_bytes(void) { return VOICE_MAX_CLIP_BYTES; }

hl_ctx *HL_CALL hl_create(const char *callsign, int relay, int ttl, int preamble_ms, uint32_t seed,
                          hl_message_cb on_msg, hl_status_cb on_status, hl_event_cb on_event, void *user) {
    if (!callsign) return NULL;
    hl_ctx *c = (hl_ctx *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->on_msg = on_msg;
    c->on_status = on_status;
    c->on_event = on_event;
    c->user = user;
    c->rx_modem = -1;
    audiocheck_init(&c->audiocheck);
    if (preamble_ms < 100) preamble_ms = 100;
    if (preamble_ms > 2000) preamble_ms = 2000;
    c->preamble_ms = preamble_ms;
    snprintf(c->callsign, sizeof c->callsign, "%s", callsign);
    c->tnc_baud = 1200;
    c->repeat_interval_ms = 30000;
    c->carrier_mode = HL_CS_DATA_OR_LEVEL;
    c->rng = seed * 2654435761u + 1u;
    kiss_dec_init(&c->kiss);
    nmea_init(&c->nmea);
    c->map = map_create();
    map_set_self(c->map, callsign);

    mesh_cfg_t cfg;
    mesh_cfg_defaults(&cfg, callsign);
    cfg.relay_enabled = relay ? 1 : 0;
    cfg.default_ttl = (uint8_t)(ttl < 0 ? 0 : ttl > 3 ? 3 : ttl);
    cfg.preamble_ms = (uint32_t)preamble_ms;
    mesh_cb_t cb = {cb_message, cb_status, cb_event};
    c->mesh = mesh_create(&cfg, &cb, c, seed);
    c->fec = HL_FEC_DEFAULT;
    if (c->mesh) mesh_set_fec(c->mesh, c->fec);
    c->demod = afsk_demod_create();
    c->voice = voice_create();
    tone_init(&c->tone, TONE_OFF);
    meter_init(&c->meter);
    if (!c->mesh || !c->demod || !c->voice || !c->map) {
        hl_destroy(c);
        return NULL;
    }
    return c;
}

void HL_CALL hl_destroy(hl_ctx *c) {
    if (!c) return;
    mesh_destroy(c->mesh);
    map_destroy(c->map);
    ofdm_tx_destroy(c->otx);
    ofdm_rx_destroy(c->orx);
    mt63_tx_destroy(c->mtx);
    mt63_rx_destroy(c->mrx);
    psk_rx_destroy(c->prx);
    afsk_demod_destroy(c->demod);
    voice_destroy(c->voice);
    free(c->txaudio);
    free(c);
}

void HL_CALL hl_set_relay(hl_ctx *c, int on) { mesh_set_relay(c->mesh, on ? 1 : 0); }

static void service_repeats(hl_ctx *c, int64_t now) {
    for (int i = 0; i < REPEAT_SLOTS; i++) {
        repeat_t *r = &c->rep[i];
        if (!r->used || now < r->due) continue;
        uint16_t id;
        int rc = mesh_send(c->mesh, "CQ", MT_REPORT, r->data, r->len, 0, (uint64_t)now, &id);
        if (rc == MESH_OK) {
            r->left--;
            c->rng = c->rng * 1664525u + 1013904223u;
            /* random spread so two stations' repeats do not stay in step and collide every time */
            r->due = now + c->repeat_interval_ms + (int64_t)((c->rng >> 8) % 10000u);
            char ev[64];
            snprintf(ev, sizeof ev, "REPEAT report copy sent, %d more", r->left);
            if (c->on_event) c->on_event(c->user, ev);
            if (r->left <= 0) r->used = 0;
        } else {
            r->due = now + 5000; /* queue full: try again shortly */
        }
    }
}

float HL_CALL hl_process_audio(hl_ctx *c, const int16_t *pcm, int n, int64_t now_ms, float busy_threshold) {
    c->last_ms = now_ms;
    if (n > 0 && pcm) {
        afsk_demod_process(c->demod, pcm, (size_t)n);
        meter_feed(&c->meter, pcm, (size_t)n);
        if (c->orx) ofdm_rx_process(c->orx, pcm, (size_t)n);
        if (c->mrx && (c->modem == HL_MODEM_MT63 || c->listen_all)) mt63_rx_process(c->mrx, pcm, (size_t)n);
        if (c->prx && (is_psk(c->modem) || c->listen_all)) psk_rx_process(c->prx, pcm, (size_t)n);
        if (c->link == HL_LINK_AUDIO && audiocheck_feed(&c->audiocheck, pcm, (size_t)n) && c->on_event) {
            char w[400];
            snprintf(w, sizeof w, "WARNING: received transmissions drop by about %.0f dB after their first second. Something "
                     "in the audio input is suppressing steady signals, so data will not decode reliably. Usual causes: a "
                     "USB-C headset-style cable with built-in noise cancelling, or Windows audio enhancements. "
                     "See Help > Troubleshooting.", c->audiocheck.last_drop_db);
            c->on_event(c->user, w);
        }
    }
    uint8_t frame[HDLC_MAX_FRAME];
    int len, repaired;
    rx_from(c, HL_MODEM_AFSK);
    while ((len = afsk_demod_next_frame_ex(c->demod, frame, sizeof frame, &repaired)) > 0) {
        if (repaired && !mesh_frame_plausible(frame, (size_t)len)) { c->repair_rejected++; continue; }
        mesh_rx_frame(c->mesh, frame, (size_t)len, (uint64_t)now_ms);
    }
    if (c->orx) {
        uint8_t pl[OFDM_MAX_PAYLOAD];
        char call[10];
        int flips, plen;
        rx_from(c, HL_MODEM_OFDM);
        while ((plen = ofdm_rx_next(c->orx, pl, call, &flips)) > 0) {
            const uint8_t *fr[16];
            size_t fl[16];
            int nf = ofdm_unpack(pl, plen, fr, fl, 16);
            for (int i = 0; i < nf; i++) mesh_rx_frame(c->mesh, fr[i], fl[i], (uint64_t)now_ms);
        }
    }
    if (c->mrx && (c->modem == HL_MODEM_MT63 || c->listen_all)) {
        uint8_t mf[MT63_MAX_FRAME];
        int ml;
        rx_from(c, HL_MODEM_MT63);
        while ((ml = mt63_rx_next(c->mrx, mf, sizeof mf)) > 0) mesh_rx_frame(c->mesh, mf, (size_t)ml, (uint64_t)now_ms);
    }
    if (c->prx && (is_psk(c->modem) || c->listen_all)) {
        uint8_t pf[PSK_MAX_FRAME];
        int pl, mode;
        while ((pl = psk_rx_next_mode(c->prx, pf, sizeof pf, &mode)) > 0) {
            rx_from(c, mode == PSK_8PSK ? HL_MODEM_PSK3600 : HL_MODEM_PSK2400);
            mesh_rx_frame(c->mesh, pf, (size_t)pl, (uint64_t)now_ms);
        }
    }
    rx_from(c, -1);
    float level = afsk_demod_level(c->demod);
    /* Data carrier detect: AFSK flags/clock, the MT63 receiver's lock, or a PSK frame in progress. With any modem but
     * AFSK selected, "data only" still uses the level too, so its transmissions hold us off before (or without) a lock. */
    /* Listening to all modems, every running receiver counts. */
    int mt63 = c->modem == HL_MODEM_MT63 || c->listen_all;
    int data = afsk_demod_dcd(c->demod) || (mt63 && c->mrx && mt63_rx_locked(c->mrx)) ||
               ((is_psk(c->modem) || c->listen_all) && c->prx && psk_rx_busy(c->prx)) ||
               ((c->modem == HL_MODEM_OFDM || c->listen_all) && c->orx && ofdm_rx_busy(c->orx));
    int loud = level > busy_threshold;
    c->busy = c->carrier_mode == HL_CS_OFF ? 0
            : c->carrier_mode == HL_CS_DATA ? (data || (c->modem != HL_MODEM_AFSK && loud))
            : (data || loud);
    mesh_set_channel_busy(c->mesh, c->busy, (uint64_t)now_ms);
    mesh_tick(c->mesh, (uint64_t)now_ms);
    service_repeats(c, now_ms);
    return level;
}

int HL_CALL hl_send(hl_ctx *c, const char *dst, int type, const uint8_t *data, int len, int ack_req,
                    int64_t now_ms) {
    c->last_ms = now_ms;
    if (!dst || !data || len <= 0) return MESH_ERR_ARG;
    uint16_t id = 0;
    int rc = mesh_send(c->mesh, dst, type, data, (size_t)len, ack_req ? 1 : 0, (uint64_t)now_ms, &id);
    return rc == MESH_OK ? (int)id : rc;
}

const int16_t *HL_CALL hl_poll_tx(hl_ctx *c, int64_t now_ms, float amp, int *n_samples) {
    if (n_samples) *n_samples = 0;
    if (c->link != HL_LINK_AUDIO) return NULL;
    int bm = -1;
    int n = mesh_poll_tx_modem(c->mesh, (uint64_t)now_ms, c->txbuf, sizeof c->txbuf, c->txlens, 32, &bm);
    if (n <= 0) return NULL;
    /* Relayed frames go out on the modem they arrived on; everything else on the selected one. */
    int modem = bm < 0 ? c->modem : bm;
    if (modem == HL_MODEM_OFDM && !c->otx) c->otx = ofdm_tx_create();
    if (modem == HL_MODEM_MT63 && !c->mtx) c->mtx = mt63_tx_create();
    const uint8_t *frames[32];
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        frames[i] = c->txbuf + off;
        off += c->txlens[i];
    }
    int noise = (c->preamble_ms + 179) / 180; /* OFDM lead-in symbols are 180 ms each */
    if (noise < 1) noise = 1;
    if (noise > 6) noise = 6;
    size_t cap = modem == HL_MODEM_OFDM && c->otx ? ofdm_burst_max_samples(c->txlens, n, noise)
               : modem == HL_MODEM_MT63 && c->mtx ? mt63_burst_max_samples(c->txlens, n, c->preamble_ms)
               : is_psk(modem) ? psk_burst_max_samples(c->txlens, n, c->preamble_ms, psk_mode(modem))
                                                     : modem_burst_max_samples_fec(c->txlens, n, c->preamble_ms, 2, c->fec);
    if (cap == 0) return NULL;
    if (cap > c->txcap) {
        int16_t *nb = (int16_t *)realloc(c->txaudio, cap * sizeof(int16_t));
        if (!nb) return NULL;
        c->txaudio = nb;
        c->txcap = cap;
    }
    if (amp < 0.01f) amp = 0.01f;
    if (amp > 1.0f) amp = 1.0f;
    size_t k;
    if (modem == HL_MODEM_OFDM && c->otx) {
        k = ofdm_encode_burst(c->otx, frames, c->txlens, n, c->callsign, noise, c->txaudio, c->txcap);
        for (size_t i = 0; i < k; i++) c->txaudio[i] = (int16_t)((float)c->txaudio[i] * amp);
    } else if (modem == HL_MODEM_MT63 && c->mtx) {
        k = mt63_encode_burst(c->mtx, frames, c->txlens, n, c->preamble_ms, amp, c->txaudio, c->txcap);
    } else if (is_psk(modem)) {
        k = psk_encode_burst(frames, c->txlens, n, c->preamble_ms, psk_mode(modem), amp, c->txaudio, c->txcap);
    } else {
        k = modem_encode_burst_fec(frames, c->txlens, n, c->preamble_ms, 2, amp, c->fec, c->txaudio, c->txcap);
    }
    if (k == 0) return NULL;
    if (n_samples) *n_samples = (int)k;
    return c->txaudio;
}

int64_t HL_CALL hl_next_wakeup(hl_ctx *c) {
    int64_t w = (int64_t)mesh_next_wakeup(c->mesh);
    for (int i = 0; i < REPEAT_SLOTS; i++)
        if (c->rep[i].used && (w == 0 || c->rep[i].due < w)) w = c->rep[i].due;
    return w;
}

int HL_CALL hl_voice_encode(hl_ctx *c, const int16_t *pcm8k, int n, uint8_t *out, int cap) {
    if (n <= 0 || cap <= 0) return 0;
    return (int)voice_encode(c->voice, pcm8k, (size_t)n, out, (size_t)cap);
}

int HL_CALL hl_voice_decode(hl_ctx *c, const uint8_t *clip, int len, int16_t *pcm8k, int cap) {
    if (len <= 0 || cap <= 0) return 0;
    return (int)voice_decode(c->voice, clip, (size_t)len, pcm8k, (size_t)cap);
}

void HL_CALL hl_tone_start(hl_ctx *c, int mode) {
    tone_init(&c->tone, mode >= TONE_OFF && mode <= TONE_ALT ? mode : TONE_OFF);
}

float HL_CALL hl_tone_fill(hl_ctx *c, int16_t *out, int n, float amp) {
    if (n <= 0 || !out) return c->tone.amp;
    return tone_fill(&c->tone, amp, out, (size_t)n);
}

void HL_CALL hl_note_external_tx(hl_ctx *c, int64_t now_ms, int duration_ms) {
    mesh_note_external_tx(c->mesh, (uint64_t)now_ms, duration_ms < 0 ? 0 : (uint32_t)duration_ms);
}

void HL_CALL hl_get_meter(hl_ctx *c, float *out5) {
    out5[0] = c->meter.rms_db;
    out5[1] = c->meter.peak_db;
    out5[2] = c->meter.t1200_db;
    out5[3] = c->meter.t2200_db;
    out5[4] = (float)c->meter.clipped;
}

/* ---------------- modem / link selection (API v3) ---------------- */

/* One place that decides fragment size and ACK timing from modem, link and manual-PTT mode. */
static void apply_timing(hl_ctx *c) {
    /* FX.25 wraps only what this station modulates itself: AFSK on the audio link (OFDM has its own FEC; a TNC radio
     * modulates its own frames) */
    mesh_set_fec(c->mesh, c->link == HL_LINK_AUDIO && c->modem == HL_MODEM_AFSK ? c->fec : 0);
    mesh_set_burst(c->mesh, 0, 0);
    mesh_set_frame_extra(c->mesh, 0);
    if (c->link == HL_LINK_TNC) {
        /* the radio keys up, runs CSMA and queues frames by itself, so allow plenty of time for an ACK */
        mesh_configure(c->mesh, MESH_FRAG_CLASS_SMALL, c->tnc_baud >= 9600 ? 3500 : 5500, c->tnc_baud);
        return;
    }
    if (c->modem == HL_MODEM_MT63) {
        /* 20 seven-bit characters per second; every transmission also spends ~5 s on receiver lock-in and the interleaver
         * flush, so let several frames share it (up to 30 s per burst) and wait for an ACK burst that itself takes ~8 s. */
        mesh_set_burst(c->mesh, mt63_burst_overhead_ms(c->preamble_ms) - (uint32_t)c->preamble_ms, 30000);
        mesh_configure(c->mesh, MESH_FRAG_CLASS_SMALL, c->manual ? 60000u : 12000u, MT63_CHARS_PER_S * 7);
        return;
    }
    if (is_psk(c->modem)) {
        /* per frame bytes cost 4.5 (QPSK) or 3 (8-PSK) symbols each with pilots; sync, header and 19 bytes of length,
         * CRC and RS are a fixed cost per frame; the tail and filter run-out a fixed cost per burst. Large fragments
         * spread that fixed cost. */
        int q = c->modem == HL_MODEM_PSK2400;
        mesh_set_frame_extra(c->mesh, q ? 164u : 141u);
        mesh_set_burst(c->mesh, 40, 0);
        mesh_configure(c->mesh, MESH_FRAG_CLASS_LARGE, c->manual ? 45000u : 3500u, q ? 2130u : 3200u);
        return;
    }
    int ofdm = c->modem == HL_MODEM_OFDM;
    /* OFDM packets are ~1.1-1.8 s long and ACKs ride in their own burst: wait longer before retrying.
     * With a hand-operated PTT the other side needs a human to key every ACK, so wait far longer. */
    uint32_t ack = c->manual ? 45000u : ofdm ? 5500u : 3500u;
    mesh_configure(c->mesh, ofdm ? MESH_FRAG_CLASS_LARGE : MESH_FRAG_CLASS_SMALL, ack, 1200);
}

int HL_CALL hl_set_fec(hl_ctx *c, int check_bytes) {
    if (check_bytes != 0 && check_bytes != 16 && check_bytes != 32 && check_bytes != 64) return -1;
    c->fec = check_bytes;
    apply_timing(c);
    return 0;
}

void HL_CALL hl_get_fec_info(hl_ctx *c, uint32_t *out3) { afsk_demod_fec_stats(c->demod, out3); }

int HL_CALL hl_set_modem(hl_ctx *c, int modem) {
    if (modem == HL_MODEM_OFDM) {
        if (!c->otx) c->otx = ofdm_tx_create();
        if (!c->orx) c->orx = ofdm_rx_create();
        if (!c->otx || !c->orx) {
            ofdm_tx_destroy(c->otx); ofdm_rx_destroy(c->orx);
            c->otx = NULL; c->orx = NULL; c->modem = HL_MODEM_AFSK;
            apply_timing(c);
            return -1;
        }
        c->modem = HL_MODEM_OFDM;
    } else if (modem == HL_MODEM_MT63) {
        if (!c->mtx) c->mtx = mt63_tx_create();
        if (!c->mrx) c->mrx = mt63_rx_create();
        if (!c->mtx || !c->mrx) {
            mt63_tx_destroy(c->mtx); mt63_rx_destroy(c->mrx);
            c->mtx = NULL; c->mrx = NULL; c->modem = HL_MODEM_AFSK;
            apply_timing(c);
            return -1;
        }
        c->modem = HL_MODEM_MT63;
    } else if (is_psk(modem)) {
        if (!c->prx) c->prx = psk_rx_create();
        if (!c->prx) { c->modem = HL_MODEM_AFSK; apply_timing(c); return -1; }
        c->modem = modem;
    } else {
        c->modem = HL_MODEM_AFSK;
    }
    apply_timing(c);
    return 0;
}

void HL_CALL hl_set_listen_all(hl_ctx *c, int on) {
    if (on) {
        if (!c->orx) c->orx = ofdm_rx_create();
        if (!c->mrx) c->mrx = mt63_rx_create();
        if (!c->prx) c->prx = psk_rx_create();
    }
    c->listen_all = on ? 1 : 0;
}

void HL_CALL hl_set_monitor(hl_ctx *c, int on) {
    c->monitor = on ? 1 : 0;
    mesh_set_monitor(c->mesh, c->monitor, cb_monitor, c);
}

void HL_CALL hl_set_monitor_callback(hl_ctx *c, hl_monitor_cb cb, void *user) {
    c->on_mon = cb;
    c->mon_user = user;
}

int HL_CALL hl_rx_modem(hl_ctx *c) { return c->rx_modem; }

void HL_CALL hl_set_station_id(hl_ctx *c, int on) { mesh_set_station_id(c->mesh, on); }

void HL_CALL hl_set_link(hl_ctx *c, int link, int tnc_baud) {
    c->link = link == HL_LINK_TNC ? HL_LINK_TNC : HL_LINK_AUDIO;
    if (c->link == HL_LINK_TNC) {
        c->tnc_baud = tnc_baud >= 2400 ? (uint32_t)tnc_baud : 1200u;
        kiss_dec_init(&c->kiss);
        c->tnc_free_at = 0;
    }
    apply_timing(c);
}

void HL_CALL hl_set_manual_ptt(hl_ctx *c, int on) {
    c->manual = on ? 1 : 0;
    apply_timing(c);
}

int HL_CALL hl_pending_tx(hl_ctx *c) { return mesh_pending_tx(c->mesh); }

static void on_kiss(void *user, uint8_t port, uint8_t command, const uint8_t *data, size_t len) {
    hl_ctx *c = (hl_ctx *)user;
    (void)port;
    if (command != 0) return; /* only data frames; TNC parameter/status frames are ignored */
    const uint8_t *info;
    size_t ilen;
    if (!ax25_ui_unwrap(data, len, &info, &ilen, NULL)) return; /* other APRS/packet traffic */
    c->rx_modem = HL_MODEM_AFSK; /* a TNC radio only does AFSK; relays go back through the TNC regardless */
    mesh_rx_frame(c->mesh, info, ilen, (uint64_t)c->tnc_now);
    c->rx_modem = -1;
}

void HL_CALL hl_tnc_rx(hl_ctx *c, const uint8_t *bytes, int n, int64_t now_ms) {
    c->tnc_now = now_ms;
    c->last_ms = now_ms;
    if (n > 0 && bytes) kiss_dec_feed(&c->kiss, bytes, (size_t)n, on_kiss, c);
    mesh_tick(c->mesh, (uint64_t)now_ms);
    service_repeats(c, now_ms);
}

const uint8_t *HL_CALL hl_tnc_poll_tx(hl_ctx *c, int64_t now_ms, int *n_bytes) {
    if (n_bytes) *n_bytes = 0;
    if (c->link != HL_LINK_TNC || now_ms < c->tnc_free_at) return NULL;
    int n = mesh_poll_tx(c->mesh, (uint64_t)now_ms, c->txbuf, sizeof c->txbuf, c->txlens, 32);
    if (n <= 0) return NULL;
    size_t off = 0, out = 0;
    uint32_t air_ms = c->preamble_ms;
    for (int i = 0; i < n; i++) {
        uint8_t ax[MESH_MAX_FRAME + AX25_HDR_LEN];
        size_t al = ax25_ui_wrap(c->callsign, c->txbuf + off, c->txlens[i], ax, sizeof ax);
        off += c->txlens[i];
        if (!al) continue;
        size_t kl = kiss_encode(0, 0, ax, al, c->tncout + out, sizeof c->tncout - out);
        if (!kl) break; /* cannot happen with the buffer sized for 32 maximal frames */
        out += kl;
        air_ms += mesh_airtime_ms(al, c->tnc_baud);
    }
    if (out == 0) return NULL;
    c->tnc_free_at = now_ms + (int64_t)air_ms + 400; /* the radio is busy sending; do not queue more yet */
    if (n_bytes) *n_bytes = (int)out;
    return c->tncout;
}

void HL_CALL hl_get_stats(hl_ctx *c, uint32_t *out8) {
    mesh_stats_t st;
    mesh_get_stats(c->mesh, &st);
    out8[0] = st.rx_frames; out8[1] = st.rx_dups; out8[2] = st.tx_frames; out8[3] = st.relayed;
    out8[4] = st.relay_suppressed; out8[5] = st.retries;
    out8[6] = c->orx ? ofdm_rx_packets_ok(c->orx) : 0;
    out8[7] = c->orx ? ofdm_rx_packets_failed(c->orx) : 0;
}

/* ---------------- geo (API v5) ---------------- */

int HL_CALL hl_geo_parse(const char *text, double *lat, double *lon, double *radius_m, int *kind) {
    return geo_parse(text, lat, lon, radius_m, kind);
}
int HL_CALL hl_geo_format(double lat, double lon, int fmt, char *out, int cap) {
    return cap > 0 ? geo_format(lat, lon, fmt, out, (size_t)cap) : 0;
}
int HL_CALL hl_geo_maidenhead(double lat, double lon, int chars, char *out, int cap) {
    if (cap < chars + 1) return 0;
    return geo_maidenhead_encode(lat, lon, chars, out);
}
int HL_CALL hl_geo_mgrs(double lat, double lon, int digits, char *out, int cap) {
    return cap > 0 ? geo_mgrs_encode(lat, lon, digits, out, (size_t)cap) : 0;
}
void HL_CALL hl_geo_to_world(double lat, double lon, int zoom, double *px, double *py) { geo_to_world(lat, lon, zoom, px, py); }
void HL_CALL hl_geo_from_world(double px, double py, int zoom, double *lat, double *lon) { geo_from_world(px, py, zoom, lat, lon); }
double HL_CALL hl_geo_distance_m(double a, double b, double c, double d) { return geo_distance_m(a, b, c, d); }
double HL_CALL hl_geo_bearing_deg(double a, double b, double c, double d) { return geo_bearing_deg(a, b, c, d); }

int HL_CALL hl_nmea_feed(hl_ctx *c, const uint8_t *bytes, int n, double *out6) {
    nmea_fix_t f;
    if (n <= 0 || !bytes || !nmea_feed(&c->nmea, bytes, (size_t)n, &f)) return 0;
    out6[0] = f.lat; out6[1] = f.lon; out6[2] = f.speed_kmh; out6[3] = f.course_deg; out6[4] = f.hdop; out6[5] = f.sats;
    return 1;
}

/* ---------------- reports and map (API v5) ---------------- */

void HL_CALL hl_set_clock(hl_ctx *c, int64_t unix_s, int64_t now_ms) {
    c->clock_set = unix_s > 0;
    c->clock_unix = unix_s;
    c->clock_ms = now_ms;
}

int HL_CALL hl_schema_count(void) { return RSCHEMA_COUNT; }
const char *HL_CALL hl_schema_json(int index) { return index >= 0 && index < RSCHEMA_COUNT ? RSCHEMAS[index]->json : NULL; }

int HL_CALL hl_report_send(hl_ctx *c, const char *dst, const char *schema, const char *kv, int ack_req, int64_t now_ms,
                           char *err, int errcap) {
    c->last_ms = now_ms;
    if (err && errcap > 0) err[0] = 0;
    const rschema_t *s = report_schema_by_name(schema);
    if (!s || !dst || !kv) return MESH_ERR_ARG;
    report_t r;
    report_init(&r, s);
    int rc = report_from_kv(&r, kv, unix_now(c, now_ms), err, errcap > 0 ? (size_t)errcap : 0);
    if (rc) return -100 + rc;
    uint8_t buf[REPORT_MAX_BYTES];
    int n = report_encode(&r, buf, sizeof buf);
    if (n < 0) return -100 + n;
    int id = hl_send(c, dst, MT_REPORT, buf, n, ack_req, now_ms);
    if (id >= 0) {
        map_ingest(c->map, c->callsign, buf, (size_t)n, unix_now(c, now_ms)); /* show our own report */
        if (c->repeat_extra > 0 && !strcmp(dst, "CQ")) {   /* addressed reports use ARQ instead */
            repeat_t *slot = NULL;
            for (int i = 0; i < REPEAT_SLOTS && !slot; i++) if (!c->rep[i].used) slot = &c->rep[i];
            if (!slot) {                                    /* full: the oldest pending repeat gives way */
                slot = &c->rep[0];
                for (int i = 1; i < REPEAT_SLOTS; i++) if (c->rep[i].due < slot->due) slot = &c->rep[i];
            }
            slot->used = 1;
            slot->left = c->repeat_extra;
            slot->len = (uint8_t)n;
            memcpy(slot->data, buf, (size_t)n);
            c->rng = c->rng * 1664525u + 1013904223u;
            slot->due = now_ms + c->repeat_interval_ms + (int64_t)((c->rng >> 8) % 10000u);
        }
    }
    return id;
}

int HL_CALL hl_report_summary(const uint8_t *data, int len, char *out, int cap) {
    report_t r;
    if (len <= 0 || cap <= 0 || report_decode(&r, data, (size_t)len)) return 0;
    return report_summary(&r, out, (size_t)cap);
}
int HL_CALL hl_report_detail(const uint8_t *data, int len, char *out, int cap) {
    report_t r;
    if (len <= 0 || cap <= 0 || report_decode(&r, data, (size_t)len)) return 0;
    return report_detail(&r, out, (size_t)cap);
}

void HL_CALL hl_map_set_self_position(hl_ctx *c, double lat, double lon, double radius_m, int64_t now_ms) {
    map_set_self_position(c->map, lat, lon, radius_m, unix_now(c, now_ms));
}
int HL_CALL hl_map_snapshot(hl_ctx *c, int64_t now_ms, char *out, int cap) {
    return cap > 0 ? map_snapshot(c->map, unix_now(c, now_ms), out, (size_t)cap) : 0;
}
int HL_CALL hl_map_detail(hl_ctx *c, const char *key, int64_t now_ms, char *out, int cap) {
    return cap > 0 ? map_detail(c->map, key, unix_now(c, now_ms), out, (size_t)cap) : 0;
}
void HL_CALL hl_map_clear(hl_ctx *c) { map_clear(c->map); }

void HL_CALL hl_set_report_repeat(hl_ctx *c, int extra_copies, int interval_ms) {
    c->repeat_extra = extra_copies < 0 ? 0 : extra_copies > 3 ? 3 : extra_copies;
    c->repeat_interval_ms = interval_ms < 10000 ? 10000 : interval_ms > 600000 ? 600000 : interval_ms;
    if (c->repeat_extra == 0) memset(c->rep, 0, sizeof c->rep); /* turning it off cancels pending copies */
}

/* ---------------- receive DSP options (API v7) ---------------- */
void HL_CALL hl_set_carrier_sense(hl_ctx *c, int mode) {
    c->carrier_mode = (mode == HL_CS_OFF || mode == HL_CS_DATA) ? mode : HL_CS_DATA_OR_LEVEL;
}
void HL_CALL hl_set_bit_repair(hl_ctx *c, int on) { afsk_demod_set_repair(c->demod, on); }
int HL_CALL hl_channel_busy(hl_ctx *c) { return c->busy; }
void HL_CALL hl_get_rx_info(hl_ctx *c, uint32_t *out6) {
    out6[0] = (uint32_t)afsk_demod_dcd(c->demod);
    out6[1] = afsk_demod_frames_ok(c->demod);
    out6[2] = afsk_demod_frames_repaired(c->demod);
    out6[3] = afsk_demod_repair_ambiguous(c->demod);
    out6[4] = c->repair_rejected;
    out6[5] = (uint32_t)c->busy;
}
