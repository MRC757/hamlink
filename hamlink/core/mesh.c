/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Header layout (23 bytes):
 *  0 type | 1 flags (bit0 ACKREQ, bits4-7 attempt/serial) | 2 ttl | 3..10 src | 11..18 dst
 *  19..20 msgid (LE) | 21 seq | 22 total
 * ACK payload: 4-byte little-endian bitmap of fragments received.
 * ID payload : ASCII text (callsign). */
#define F_ACKREQ 0x01
#define F_CLASS_SHIFT 1
#define F_CLASS_MASK 0x06

#define Q_SIZE 96
#define MAX_TXMSG 4
#define MAX_RXMSG 8
#define DEDUP_SIZE 256
#define DEDUP_TTL_MS 300000ULL
#define RXSLOT_TTL_MS 300000ULL
#define ID_MAX_INTERVAL_MS 540000ULL /* 9 min: stay under the 10 min rule */
#define ID_IDLE_MS 12000ULL          /* ID once traffic goes quiet */
#define BAUD 1200U

enum { K_ACKID = 0, K_OWN = 1, K_RELAY = 2 }; /* also transmit priority order */

typedef struct {
    int used;
    uint8_t kind;
    int8_t txm;
    uint32_t key, seqno;
    uint64_t due;
    uint8_t len;
    uint8_t data[MESH_MAX_FRAME];
} qent_t;

typedef struct {
    int used;
    uint16_t id;
    uint8_t dst[MESH_CALL_LEN];
    uint8_t type, total, attempt, ttl, fclass;
    int ackreq;
    uint32_t acked;
    size_t len;
    uint8_t *data;
    int pending_frames;
    uint64_t ack_deadline; /* 0 = frames not (all) on the air yet */
} txmsg_t;

typedef struct {
    int used;
    uint8_t src[MESH_CALL_LEN], dst[MESH_CALL_LEN];
    uint16_t id;
    uint8_t type, total, fclass;
    uint32_t mask;
    int delivered, ack_pending, ack_gap;
    size_t last_len;
    uint64_t last_rx, ack_due;
    uint8_t *data;
} rxslot_t;

typedef struct {
    uint32_t key;
    uint64_t ts;
} dedup_t;

struct mesh {
    mesh_cfg_t cfg;
    mesh_cb_t cb;
    void *user;
    uint8_t me[MESH_CALL_LEN];
    uint32_t rng, enq_counter;
    qent_t q[Q_SIZE];
    txmsg_t txm[MAX_TXMSG];
    rxslot_t rx[MAX_RXMSG];
    dedup_t dd[DEDUP_SIZE];
    int dd_pos;
    int busy;
    uint64_t holdoff_until, last_tx_end, id_timer_start;
    uint64_t relay_batch_t, relay_batch_due; /* frames of one burst share a relay delay */
    int tx_since_id, id_queued;
    uint16_t next_id;
    uint8_t ack_serial;
    mesh_stats_t st;
};

/* ------------------------------------------------------------------ */

static uint32_t rnd(mesh_t *m) {
    uint32_t x = m->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return m->rng = x ? x : 0x9E3779B9u;
}

static void pack_call(uint8_t out[MESH_CALL_LEN], const char *s) {
    memset(out, 0, MESH_CALL_LEN);
    for (int i = 0; i < MESH_CALL_LEN && s[i]; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[i] = (uint8_t)c;
    }
}

/* Callsigns arrive from the air, so never trust them: printable ASCII only (safe for JNI
 * NewStringUTF and for logs), NUL-terminated at the first padding byte. */
static void unpack_call(char out[MESH_CALL_LEN + 1], const uint8_t *in) {
    int i = 0;
    for (; i < MESH_CALL_LEN && in[i]; i++) out[i] = (in[i] >= 0x20 && in[i] < 0x7F) ? (char)in[i] : '?';
    out[i] = 0;
}

static int is_broadcast(const uint8_t *c) {
    static const uint8_t cq[MESH_CALL_LEN] = {'C', 'Q', 0, 0, 0, 0, 0, 0};
    return memcmp(c, cq, MESH_CALL_LEN) == 0;
}

static void event2(mesh_t *m, const char *fmt, const char *a, const char *b) {
    if (!m->cb.on_event) return;
    char line[96];
    snprintf(line, sizeof line, fmt, a, b);
    m->cb.on_event(m->user, line);
}

static uint32_t airtime_bps(size_t frame_len, uint32_t bps) {
    size_t bits = (frame_len + 2) * 8;
    bits += bits / 30 + 16; /* ~3% stuffing + 2 flags */
    return (uint32_t)((bits * 1000 + bps - 1) / bps);
}

uint32_t mesh_frame_airtime_ms(size_t frame_len) { return airtime_bps(frame_len, BAUD); }
uint32_t mesh_airtime_ms(size_t frame_len, uint32_t bps) { return airtime_bps(frame_len, bps < 300 ? BAUD : bps); }

/* How long to wait for the next fragment before sending a partial ACK. Fragments of one burst arrive about
 * one frame airtime apart (OFDM packets ~1.1 s, AFSK frames ~0.9 s; a TNC adds its own TX delay), so
 * wait well over that, or the sender mistakes a partial ACK for a lost burst and retransmits. */
static uint32_t ack_gap_ms(const mesh_t *m, size_t frag_size) {
    return airtime_bps(MESH_HDR_LEN + frag_size, m->cfg.bps) * 9 / 5 + 300;
}

int mesh_frag_size(int frag_class) {
    return frag_class == MESH_FRAG_CLASS_SMALL ? 96 : frag_class == MESH_FRAG_CLASS_LARGE ? 145 : 0;
}

void mesh_cfg_defaults(mesh_cfg_t *cfg, const char *callsign) {
    memset(cfg, 0, sizeof *cfg);
    snprintf(cfg->callsign, sizeof cfg->callsign, "%s", callsign ? callsign : "");
    cfg->relay_enabled = 0;
    cfg->default_ttl = 2;
    cfg->max_retries = 4;
    cfg->ack_timeout_ms = 3500;
    cfg->burst_cap_ms = 12000;
    cfg->preamble_ms = 400;
    cfg->frag_class = MESH_FRAG_CLASS_SMALL;
    cfg->bps = BAUD;
}

mesh_t *mesh_create(const mesh_cfg_t *cfg, const mesh_cb_t *cb, void *user, uint32_t seed) {
    mesh_t *m = (mesh_t *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cfg = *cfg;
    if (m->cfg.max_retries > 14) m->cfg.max_retries = 14;
    if (!mesh_frag_size((int)m->cfg.frag_class)) m->cfg.frag_class = MESH_FRAG_CLASS_SMALL;
    if (m->cfg.bps < 300) m->cfg.bps = BAUD;
    if (cb) m->cb = *cb;
    m->user = user;
    pack_call(m->me, cfg->callsign);
    m->rng = seed ? seed : 0x1234567u;
    m->next_id = (uint16_t)(rnd(m) & 0xFFFF);
    for (int i = 0; i < MAX_TXMSG; i++) m->txm[i].data = (uint8_t *)malloc(MESH_MAX_MSG);
    for (int i = 0; i < MAX_RXMSG; i++) m->rx[i].data = (uint8_t *)malloc(MESH_MAX_MSG);
    return m;
}

void mesh_destroy(mesh_t *m) {
    if (!m) return;
    for (int i = 0; i < MAX_TXMSG; i++) free(m->txm[i].data);
    for (int i = 0; i < MAX_RXMSG; i++) free(m->rx[i].data);
    free(m);
}

void mesh_set_relay(mesh_t *m, int enabled) { m->cfg.relay_enabled = enabled ? 1 : 0; }

void mesh_configure(mesh_t *m, int frag_class, uint32_t ack_timeout_ms, uint32_t bps) {
    if (mesh_frag_size(frag_class)) m->cfg.frag_class = (uint32_t)frag_class;
    if (ack_timeout_ms >= 500) m->cfg.ack_timeout_ms = ack_timeout_ms;
    if (bps >= 300) m->cfg.bps = bps;
}
void mesh_get_stats(const mesh_t *m, mesh_stats_t *out) { *out = m->st; }

int mesh_pending_tx(const mesh_t *m) {
    int n = 0;
    for (int i = 0; i < Q_SIZE; i++) n += m->q[i].used;
    return n;
}

/* ---------------- frame helpers ---------------- */

static uint32_t frame_key(const uint8_t *f, size_t len) {
    uint32_t h = 2166136261u;
#define MIX(b) (h = (h ^ (uint8_t)(b)) * 16777619u)
    MIX(f[0]);
    for (int i = 3; i < 11; i++) MIX(f[i]);  /* src */
    MIX(f[19]); MIX(f[20]); MIX(f[21]);      /* msgid, seq */
    MIX(f[1] >> 4);                           /* attempt / serial */
    if (f[0] == MT_ACK)
        for (size_t i = MESH_HDR_LEN; i < len; i++) MIX(f[i]);
#undef MIX
    return h;
}

static size_t build_frame(uint8_t *out, uint8_t type, uint8_t flags, uint8_t ttl,
                          const uint8_t *src, const uint8_t *dst, uint16_t id, uint8_t seq,
                          uint8_t total, const uint8_t *payload, size_t plen) {
    out[0] = type;
    out[1] = flags;
    out[2] = ttl;
    memcpy(out + 3, src, MESH_CALL_LEN);
    memcpy(out + 11, dst, MESH_CALL_LEN);
    out[19] = (uint8_t)(id & 0xFF);
    out[20] = (uint8_t)(id >> 8);
    out[21] = seq;
    out[22] = total;
    if (plen) memcpy(out + MESH_HDR_LEN, payload, plen);
    return MESH_HDR_LEN + plen;
}

static qent_t *q_alloc(mesh_t *m) {
    for (int i = 0; i < Q_SIZE; i++)
        if (!m->q[i].used) {
            m->q[i].used = 1;
            m->q[i].seqno = ++m->enq_counter;
            m->q[i].txm = -1;
            return &m->q[i];
        }
    return NULL;
}

static int q_free_count(const mesh_t *m) { return Q_SIZE - mesh_pending_tx(m); }

static int dedup_seen(mesh_t *m, uint32_t key, uint64_t now) {
    for (int i = 0; i < DEDUP_SIZE; i++)
        if (m->dd[i].ts && m->dd[i].key == key && now - m->dd[i].ts < DEDUP_TTL_MS) return 1;
    m->dd[m->dd_pos].key = key;
    m->dd[m->dd_pos].ts = now ? now : 1;
    m->dd_pos = (m->dd_pos + 1) % DEDUP_SIZE;
    return 0;
}

/* ---------------- sending ---------------- */

static int queue_frag(mesh_t *m, int ti, uint8_t seq, uint64_t due) {
    txmsg_t *t = &m->txm[ti];
    qent_t *e = q_alloc(m);
    if (!e) return -1;
    size_t fsz = (size_t)mesh_frag_size(t->fclass);
    size_t off = (size_t)seq * fsz;
    size_t plen = t->len - off;
    if (plen > fsz) plen = fsz;
    uint8_t flags = (uint8_t)((t->ackreq ? F_ACKREQ : 0) | (t->fclass << F_CLASS_SHIFT) | (t->attempt << 4));
    e->len = (uint8_t)build_frame(e->data, t->type, flags, t->ttl, m->me, t->dst, t->id, seq,
                                  t->total, t->data + off, plen);
    e->kind = K_OWN;
    e->txm = (int8_t)ti;
    e->due = due;
    e->key = frame_key(e->data, e->len);
    t->pending_frames++;
    return 0;
}

int mesh_send(mesh_t *m, const char *dst, int type, const uint8_t *data, size_t len,
              int ack_req, uint64_t now, uint16_t *msgid_out) {
    if (!dst || !data || len == 0 || (type != MT_TEXT && type != MT_VOICE && type != MT_REPORT)) return MESH_ERR_ARG;
    int fclass = (int)m->cfg.frag_class;
    size_t fsz = (size_t)mesh_frag_size(fclass);
    if (len > fsz * MESH_MAX_FRAGS) return MESH_ERR_TOOBIG;
    int ti = -1;
    for (int i = 0; i < MAX_TXMSG; i++)
        if (!m->txm[i].used) { ti = i; break; }
    if (ti < 0) return MESH_ERR_BUSY;
    txmsg_t *t = &m->txm[ti];
    uint8_t total = (uint8_t)((len + fsz - 1) / fsz);
    if (q_free_count(m) < total) return MESH_ERR_BUSY;

    uint8_t *keep = t->data;
    memset(t, 0, sizeof *t);
    t->data = keep;
    t->used = 1;
    t->id = m->next_id++;
    pack_call(t->dst, dst);
    t->type = (uint8_t)type;
    t->total = total;
    t->fclass = (uint8_t)fclass;
    t->ttl = m->cfg.default_ttl;
    t->ackreq = ack_req && !is_broadcast(t->dst);
    t->len = len;
    memcpy(t->data, data, len);
    for (uint8_t s = 0; s < total; s++) queue_frag(m, ti, s, now);
    if (msgid_out) *msgid_out = t->id;
    return MESH_OK;
}

static void txm_finish(mesh_t *m, int ti, int status) {
    txmsg_t *t = &m->txm[ti];
    uint16_t id = t->id;
    for (int i = 0; i < Q_SIZE; i++)
        if (m->q[i].used && m->q[i].kind == K_OWN && m->q[i].txm == ti) m->q[i].used = 0;
    t->used = 0;
    if (m->cb.on_status) m->cb.on_status(m->user, id, status);
}

static void txm_retry(mesh_t *m, int ti, uint64_t now) {
    txmsg_t *t = &m->txm[ti];
    t->attempt++;
    if (t->attempt > m->cfg.max_retries) {
        txm_finish(m, ti, MESH_ST_FAILED);
        return;
    }
    m->st.retries++;
    t->ack_deadline = 0;
    uint32_t full = t->total == 32 ? 0xFFFFFFFFu : ((1u << t->total) - 1u);
    for (uint8_t s = 0; s < t->total; s++)
        if (!(((t->acked & full) >> s) & 1u)) queue_frag(m, ti, s, now + 100 + rnd(m) % 300);
    if (t->pending_frames == 0) t->ack_deadline = now + 1000; /* queue was full; try again */
}

/* ---------------- receiving ---------------- */

static void handle_ack(mesh_t *m, const uint8_t *f, size_t len, uint64_t now) {
    if (len < MESH_HDR_LEN + 4) return;
    uint16_t id = (uint16_t)(f[19] | (f[20] << 8));
    uint32_t bm = (uint32_t)f[MESH_HDR_LEN] | ((uint32_t)f[MESH_HDR_LEN + 1] << 8) |
                  ((uint32_t)f[MESH_HDR_LEN + 2] << 16) | ((uint32_t)f[MESH_HDR_LEN + 3] << 24);
    for (int ti = 0; ti < MAX_TXMSG; ti++) {
        txmsg_t *t = &m->txm[ti];
        if (!t->used || !t->ackreq || t->id != id || memcmp(t->dst, f + 3, MESH_CALL_LEN)) continue;
        uint32_t full = t->total == 32 ? 0xFFFFFFFFu : ((1u << t->total) - 1u);
        t->acked |= bm & full;
        if (t->acked == full) {
            txm_finish(m, ti, MESH_ST_DELIVERED);
            return;
        }
        /* drop queued retransmissions the receiver already has */
        for (int i = 0; i < Q_SIZE; i++)
            if (m->q[i].used && m->q[i].kind == K_OWN && m->q[i].txm == ti &&
                ((t->acked >> m->q[i].data[21]) & 1u)) {
                m->q[i].used = 0;
                t->pending_frames--;
            }
        if (t->ack_deadline != 0 && t->pending_frames == 0) txm_retry(m, ti, now);
        return;
    }
}

static rxslot_t *rx_slot_for(mesh_t *m, const uint8_t *src, uint16_t id, uint64_t now) {
    rxslot_t *free_s = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_RXMSG; i++) {
        rxslot_t *s = &m->rx[i];
        if (s->used && now - s->last_rx > RXSLOT_TTL_MS) s->used = 0;
        if (s->used && s->id == id && !memcmp(s->src, src, MESH_CALL_LEN)) return s;
        if (!s->used && !free_s) free_s = s;
        if (s->used && (!oldest || s->last_rx < oldest->last_rx)) oldest = s;
    }
    return free_s ? free_s : oldest;
}

static void send_ack(mesh_t *m, rxslot_t *s, uint64_t now) {
    qent_t *e = q_alloc(m);
    if (!e) return;
    uint8_t payload[4] = {(uint8_t)s->mask, (uint8_t)(s->mask >> 8), (uint8_t)(s->mask >> 16),
                          (uint8_t)(s->mask >> 24)};
    uint8_t flags = (uint8_t)((m->ack_serial++ & 0x0F) << 4);
    e->len = (uint8_t)build_frame(e->data, MT_ACK, flags, m->cfg.default_ttl, m->me, s->src, s->id,
                                  0, 1, payload, 4);
    e->kind = K_ACKID;
    e->due = now;
    e->key = frame_key(e->data, e->len);
}

static void handle_data(mesh_t *m, const uint8_t *f, size_t len, int to_me, uint64_t now) {
    uint8_t type = f[0], seq = f[21], total = f[22];
    uint8_t fclass = (uint8_t)((f[1] & F_CLASS_MASK) >> F_CLASS_SHIFT);
    size_t fsz = (size_t)mesh_frag_size(fclass);
    size_t plen = len - MESH_HDR_LEN;
    uint16_t id = (uint16_t)(f[19] | (f[20] << 8));
    if (fsz == 0) return; /* unknown fragment class from a newer protocol version */
    if (total < 1 || total > MESH_MAX_FRAGS || seq >= total) return;
    if (plen == 0 || plen > fsz) return;
    if (seq != total - 1 && plen != fsz) return;

    rxslot_t *s = rx_slot_for(m, f + 3, id, now);
    if (!s) return;
    if (!s->used || s->total != total || s->type != type || s->fclass != fclass) {
        uint8_t *keep = s->data;
        memset(s, 0, sizeof *s);
        s->data = keep;
        s->used = 1;
        memcpy(s->src, f + 3, MESH_CALL_LEN);
        memcpy(s->dst, f + 11, MESH_CALL_LEN);
        s->id = id;
        s->type = type;
        s->total = total;
        s->fclass = fclass;
    }
    memcpy(s->data + (size_t)seq * fsz, f + MESH_HDR_LEN, plen);
    s->mask |= 1u << seq;
    if (seq == total - 1) s->last_len = plen;
    s->last_rx = now;
    uint32_t full = total == 32 ? 0xFFFFFFFFu : ((1u << total) - 1u);
    int complete = (s->mask == full);

    if (to_me && (f[1] & F_ACKREQ)) {
        int immediate = (seq == total - 1) || complete;
        if (immediate) {
            uint64_t due = now + 40 + rnd(m) % 80;
            if (!s->ack_pending || s->ack_gap || due < s->ack_due) s->ack_due = due;
            s->ack_pending = 1;
            s->ack_gap = 0;
        } else if (!s->ack_pending) {
            s->ack_pending = 1;
            s->ack_gap = 1;
            s->ack_due = now + ack_gap_ms(m, fsz);
        } else if (s->ack_gap) {
            s->ack_due = now + ack_gap_ms(m, fsz);
        }
    }

    if (complete && !s->delivered) {
        s->delivered = 1;
        if (m->cb.on_message) {
            char sc[MESH_CALL_LEN + 1], dc[MESH_CALL_LEN + 1];
            unpack_call(sc, s->src);
            unpack_call(dc, s->dst);
            m->cb.on_message(m->user, sc, dc, type, s->data,
                             (size_t)(total - 1) * fsz + s->last_len);
        }
    }
}

static int call_ok(const uint8_t *c) {
    int n = 0;
    while (n < MESH_CALL_LEN && c[n]) {
        uint8_t ch = c[n];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '/' || ch == '-')) return 0;
        n++;
    }
    for (int i = n; i < MESH_CALL_LEN; i++) if (c[i]) return 0; /* NUL padded */
    return n >= 2;
}

int mesh_frame_plausible(const uint8_t *f, size_t len) {
    if (!f || len < MESH_HDR_LEN || len > MESH_MAX_FRAME) return 0;
    uint8_t type = f[0], seq = f[21], total = f[22];
    if (type < MT_TEXT || type > MT_LAST_KNOWN_RELAYED) return 0;
    if (f[2] > 7) return 0;                                   /* ttl: we never send more than 3 */
    if (!call_ok(f + 3) || !call_ok(f + 11)) return 0;
    size_t plen = len - MESH_HDR_LEN;
    if (type == MT_ACK) return plen == 4;
    if (type == MT_ID) return plen > 0;
    size_t fsz = (size_t)mesh_frag_size((f[1] & F_CLASS_MASK) >> F_CLASS_SHIFT);
    if (fsz == 0 || total < 1 || total > MESH_MAX_FRAGS || seq >= total) return 0;
    if (plen == 0 || plen > fsz || (seq != total - 1 && plen != fsz)) return 0;
    return 1;
}

void mesh_rx_frame(mesh_t *m, const uint8_t *f, size_t len, uint64_t now) {
    if (len < MESH_HDR_LEN || len > MESH_MAX_FRAME) return;
    uint8_t type = f[0];
    if (type < MT_TEXT || type > MT_LAST_KNOWN_RELAYED) return;
    if (!memcmp(f + 3, m->me, MESH_CALL_LEN)) return; /* our own frame echoed back */
    m->st.rx_frames++;

    uint32_t key = frame_key(f, len);
    if (dedup_seen(m, key, now)) {
        m->st.rx_dups++;
        for (int i = 0; i < Q_SIZE; i++)
            if (m->q[i].used && m->q[i].kind == K_RELAY && m->q[i].key == key) {
                m->q[i].used = 0; /* someone else relayed it first */
                m->st.relay_suppressed++;
            }
        return;
    }

    const uint8_t *dst = f + 11;
    int to_me = !memcmp(dst, m->me, MESH_CALL_LEN);
    int bcast = is_broadcast(dst);

    if (type == MT_ID) {
        char sc[MESH_CALL_LEN + 1];
        unpack_call(sc, f + 3);
        event2(m, "HEARD %s%s", sc, "");
        return;
    }
    if (type == MT_ACK) {
        if (to_me) handle_ack(m, f, len, now);
    } else if (to_me || bcast) {
        handle_data(m, f, len, to_me, now);
    }

    if (m->cfg.relay_enabled && f[2] > 0 && !to_me) {
        qent_t *e = q_alloc(m);
        if (e) {
            memcpy(e->data, f, len);
            e->len = (uint8_t)len;
            e->data[2] = (uint8_t)(f[2] - 1);
            e->kind = K_RELAY;
            e->key = key;
            /* Keep a burst together and in order: one random delay per batch, not per frame. */
            if (m->relay_batch_t == 0 || now - m->relay_batch_t > 2500)
                m->relay_batch_due = now + 120 + rnd(m) % 700;
            m->relay_batch_t = now;
            e->due = m->relay_batch_due;
            m->st.relayed++;
            char sc[MESH_CALL_LEN + 1], dc[MESH_CALL_LEN + 1];
            unpack_call(sc, f + 3);
            unpack_call(dc, f + 11);
            event2(m, "RELAY %s>%s", sc, dc);
        }
    }
}

/* ---------------- scheduling ---------------- */

void mesh_set_channel_busy(mesh_t *m, int busy, uint64_t now) {
    if (m->busy && !busy) m->holdoff_until = now + 80 + rnd(m) % 250;
    m->busy = busy ? 1 : 0;
}

static void queue_id(mesh_t *m, uint64_t now) {
    qent_t *e = q_alloc(m);
    if (!e) return;
    char text[24];
    char cs[MESH_CALL_LEN + 1];
    unpack_call(cs, m->me);
    int n = snprintf(text, sizeof text, "DE %s", cs);
    e->len = (uint8_t)build_frame(e->data, MT_ID, 0, 0, m->me, (const uint8_t *)"CQ\0\0\0\0\0\0",
                                  (uint16_t)rnd(m), 0, 1, (const uint8_t *)text, (size_t)n);
    e->kind = K_ACKID;
    e->due = now;
    e->key = frame_key(e->data, e->len);
    m->id_queued = 1;
}

void mesh_note_external_tx(mesh_t *m, uint64_t now, uint32_t duration_ms) {
    if (!m->tx_since_id) {
        m->tx_since_id = 1;
        m->id_timer_start = now;
    }
    m->last_tx_end = now + duration_ms;
    if (!m->id_queued) queue_id(m, now + duration_ms);
}

void mesh_tick(mesh_t *m, uint64_t now) {
    for (int ti = 0; ti < MAX_TXMSG; ti++) {
        txmsg_t *t = &m->txm[ti];
        if (t->used && t->ackreq && t->ack_deadline && now >= t->ack_deadline) txm_retry(m, ti, now);
    }
    for (int i = 0; i < MAX_RXMSG; i++) {
        rxslot_t *s = &m->rx[i];
        if (s->used && s->ack_pending && now >= s->ack_due) {
            s->ack_pending = 0;
            send_ack(m, s, now);
        }
    }
    if (m->tx_since_id && !m->id_queued) {
        int quiet = now >= m->last_tx_end && now - m->last_tx_end >= ID_IDLE_MS && mesh_pending_tx(m) == 0;
        if (now - m->id_timer_start >= ID_MAX_INTERVAL_MS || quiet) queue_id(m, now);
    }
}

static int cmp_eligible(const mesh_t *m, int a, int b) {
    return (int)((int32_t)(m->q[a].seqno - m->q[b].seqno));
}

int mesh_poll_tx(mesh_t *m, uint64_t now, uint8_t *buf, size_t cap, size_t *lens, int max_frames) {
    if (m->busy || now < m->holdoff_until) return 0;
    int pick[Q_SIZE], np = 0;
    uint32_t total_ms = m->cfg.preamble_ms;
    size_t bytes = 0;
    int full = 0;

    for (int prio = 0; prio < 3 && !full; prio++) {
        int cand[Q_SIZE], nc = 0;
        for (int i = 0; i < Q_SIZE; i++)
            if (m->q[i].used && m->q[i].kind == prio && m->q[i].due <= now) cand[nc++] = i;
        for (int a = 1; a < nc; a++) { /* insertion sort by enqueue order */
            int v = cand[a], b = a - 1;
            while (b >= 0 && cmp_eligible(m, cand[b], v) > 0) { cand[b + 1] = cand[b]; b--; }
            cand[b + 1] = v;
        }
        for (int c = 0; c < nc; c++) {
            qent_t *e = &m->q[cand[c]];
            uint32_t ms = airtime_bps(e->len, m->cfg.bps);
            if (np >= max_frames || bytes + e->len > cap ||
                (np > 0 && total_ms + ms > m->cfg.burst_cap_ms)) { full = 1; break; }
            pick[np++] = cand[c];
            total_ms += ms;
            bytes += e->len;
        }
    }
    if (np == 0) return 0;

    size_t off = 0;
    int sent_non_id = 0, sent_id = 0;
    for (int k = 0; k < np; k++) {
        qent_t *e = &m->q[pick[k]];
        memcpy(buf + off, e->data, e->len);
        lens[k] = e->len;
        off += e->len;
        m->st.tx_frames++;
        if (e->data[0] == MT_ID) sent_id = 1; else sent_non_id = 1;
        e->used = 0;
        if (e->kind == K_OWN && e->txm >= 0) {
            txmsg_t *t = &m->txm[e->txm];
            t->pending_frames--;
        }
    }
    /* now that the burst is assembled, start ACK timers / finish unacknowledged sends */
    int expect_ack = 0;
    for (int ti = 0; ti < MAX_TXMSG; ti++) {
        txmsg_t *t = &m->txm[ti];
        if (!t->used || t->pending_frames > 0 || t->ack_deadline) continue;
        int touched = 0;
        for (int k = 0; k < np; k++)
            if (m->q[pick[k]].kind == K_OWN && m->q[pick[k]].txm == ti) touched = 1;
        if (!touched) continue;
        if (t->ackreq) expect_ack = 1;
        if (t->ackreq)
            /* Each relay hop re-sends the whole burst (store-and-forward) after a random
             * delay, and the ACK needs a relayed hop back, so scale with ttl and burst length. */
            t->ack_deadline = now + total_ms + m->cfg.ack_timeout_ms +
                              (uint64_t)t->ttl * (total_ms + 2500);
        else
            txm_finish(m, ti, MESH_ST_SENT);
    }
    m->last_tx_end = now + total_ms;
    /* We just asked for an ACK: keep the channel free for it. Without this, a frame we queued meanwhile (a broadcast,
     * say) goes out the moment our burst ends and collides with the ACK on a half-duplex channel: the broadcast is lost
     * and the acknowledged message needs a retry. Window = their ACK delay (40-120 ms) + their lead-in + the ACK frame. */
    if (expect_ack) {
        uint64_t w = now + total_ms + m->cfg.preamble_ms + airtime_bps(MESH_HDR_LEN + 4, m->cfg.bps) + 120 + 400;
        if (w > m->holdoff_until) m->holdoff_until = w;
    }
    if (sent_non_id && !m->tx_since_id) {
        m->tx_since_id = 1;
        m->id_timer_start = now;
    }
    if (sent_id) {
        m->tx_since_id = 0;
        m->id_queued = 0;
    }
    return np;
}

uint64_t mesh_next_wakeup(const mesh_t *m) {
    uint64_t best = 0;
#define CONSIDER(t) do { uint64_t _t = (t); if (_t && (!best || _t < best)) best = _t; } while (0)
    for (int i = 0; i < Q_SIZE; i++)
        if (m->q[i].used) {
            uint64_t t = m->q[i].due;
            if (t < m->holdoff_until) t = m->holdoff_until;
            CONSIDER(t ? t : 1);
        }
    for (int i = 0; i < MAX_TXMSG; i++)
        if (m->txm[i].used) CONSIDER(m->txm[i].ack_deadline);
    for (int i = 0; i < MAX_RXMSG; i++)
        if (m->rx[i].used && m->rx[i].ack_pending) CONSIDER(m->rx[i].ack_due);
    if (m->tx_since_id && !m->id_queued) {
        CONSIDER(m->last_tx_end + ID_IDLE_MS);
        CONSIDER(m->id_timer_start + ID_MAX_INTERVAL_MS);
    }
#undef CONSIDER
    return best;
}
