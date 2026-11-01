/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mapmodel.h"
#include "geo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int used;
    char call[16];
    double lat, lon, radius_m;
    int64_t pos_unix, rx_unix;
    uint8_t schema_id;
    int self;
} station_t;

typedef struct {
    int used;
    unsigned id;
    char reporter[16];
    report_t rep;
    int64_t rx_unix;
    uint8_t raw[REPORT_MAX_BYTES];
    uint8_t raw_len;
} contact_t;

struct map_model {
    char self[16];
    int has_self_pos;
    double self_lat, self_lon;
    station_t st[MAP_MAX_STATIONS];
    contact_t ct[MAP_MAX_CONTACTS];
    unsigned next_id;
};

map_model_t *map_create(void) { return (map_model_t *)calloc(1, sizeof(map_model_t)); }
void map_destroy(map_model_t *m) { free(m); }
void map_clear(map_model_t *m) {
    char self[16]; memcpy(self, m->self, sizeof self);
    memset(m, 0, sizeof *m);
    memcpy(m->self, self, sizeof self);
}

static void upcase_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = (char)((src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i]);
    dst[i] = 0;
}

void map_set_self(map_model_t *m, const char *callsign) { upcase_copy(m->self, sizeof m->self, callsign); }

static station_t *find_station(map_model_t *m, const char *call, int create, int64_t now) {
    station_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < MAP_MAX_STATIONS; i++) {
        station_t *s = &m->st[i];
        if (s->used && !strcmp(s->call, call)) return s;
        if (!s->used && !free_slot) free_slot = s;
        if (s->used && !s->self && (!oldest || s->rx_unix < oldest->rx_unix)) oldest = s;
    }
    if (!create) return NULL;
    station_t *s = free_slot ? free_slot : oldest;
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->used = 1;
    snprintf(s->call, sizeof s->call, "%s", call);
    s->rx_unix = now;
    return s;
}

static uint8_t station_schema_id(void) {
    for (int i = 0; i < RSCHEMA_COUNT; i++) if (RSCHEMAS[i]->role == RR_STATION) return RSCHEMAS[i]->id;
    return 0;
}

void map_set_self_position(map_model_t *m, double lat, double lon, double radius_m, int64_t now) {
    m->has_self_pos = 1; m->self_lat = lat; m->self_lon = lon;
    if (!m->self[0]) return;
    station_t *s = find_station(m, m->self, 1, now);
    if (!s) return;
    s->self = 1; s->lat = lat; s->lon = lon; s->radius_m = radius_m; s->pos_unix = now; s->rx_unix = now;
    s->schema_id = station_schema_id();
}
int map_self_position(const map_model_t *m, double *lat, double *lon) {
    if (!m->has_self_pos) return 0;
    *lat = m->self_lat; *lon = m->self_lon;
    return 1;
}

/* ---- ageing ---- */

static int64_t report_when(const report_t *r, int64_t rx) {
    int64_t t;
    return report_time_unix(r, &t) ? t : rx;
}
static double age_min(int64_t when, int64_t now) {
    if (now <= 0 || when <= 0 || now <= when) return 0;
    return (double)(now - when) / 60.0;
}
/* alpha 1..MAP_MIN_ALPHA between stale and expire; -1 when expired */
static double alpha_for(const rschema_t *s, double age) {
    double stale = s->stale_min, expire = s->expire_min;
    if (age >= expire) return -1;
    if (age <= stale || expire <= stale) return 1.0;
    return 1.0 - (1.0 - MAP_MIN_ALPHA) * (age - stale) / (expire - stale);
}

static double radius_of(const report_t *r) {       /* any selected enum value carrying radius_m (position source) */
    for (int i = 0; i < r->schema->nfields; i++) {
        const rfield_t *f = &r->schema->fields[i];
        if (f->kind != RK_ENUM || !r->v[i].present) continue;
        const rvalue_t *v = report_enum_value(f, r->v[i].u);
        if (v && v->radius_m > 0) return v->radius_m;
    }
    return 0;
}

void map_prune(map_model_t *m, int64_t now) {
    if (now <= 0) return;
    for (int i = 0; i < MAP_MAX_CONTACTS; i++) {
        contact_t *c = &m->ct[i];
        if (!c->used) continue;
        if (alpha_for(c->rep.schema, age_min(report_when(&c->rep, c->rx_unix), now)) < 0) c->used = 0;
    }
    for (int i = 0; i < MAP_MAX_STATIONS; i++) {
        station_t *s = &m->st[i];
        if (!s->used || s->self) continue;
        const rschema_t *sc = report_schema_by_id(s->schema_id);
        if (sc && alpha_for(sc, age_min(s->pos_unix ? s->pos_unix : s->rx_unix, now)) < 0) s->used = 0;
    }
}

int map_ingest(map_model_t *m, const char *reporter_in, const uint8_t *data, size_t len, int64_t now) {
    report_t r;
    int rc = report_decode(&r, data, len);
    if (rc) return rc;
    char reporter[16];
    upcase_copy(reporter, sizeof reporter, reporter_in);
    double lat, lon;
    if (!reporter[0] || !report_location(&r, &lat, &lon)) return 0;
    int64_t when = report_when(&r, now);
    if (alpha_for(r.schema, age_min(when, now)) < 0) return 0;      /* already expired */

    if (r.schema->role == RR_STATION) {
        station_t *s = find_station(m, reporter, 1, now);
        if (!s) return 0;
        if (s->pos_unix && when < s->pos_unix && !s->self) return 0; /* older than what we have */
        s->lat = lat; s->lon = lon; s->radius_m = radius_of(&r); s->pos_unix = when; s->rx_unix = now;
        s->schema_id = r.schema->id;
        if (s->self || !strcmp(reporter, m->self)) { s->self = 1; map_set_self_position(m, lat, lon, s->radius_m, when); }
        return 1;
    }
    if (r.schema->role == RR_CONTACT) {
        for (int i = 0; i < MAP_MAX_CONTACTS; i++)            /* flood duplicate / re-send of the same report */
            if (m->ct[i].used && m->ct[i].raw_len == len && !strcmp(m->ct[i].reporter, reporter) && !memcmp(m->ct[i].raw, data, len))
                return 0;
        contact_t *slot = NULL;
        for (int i = 0; i < MAP_MAX_CONTACTS && !slot; i++) if (!m->ct[i].used) slot = &m->ct[i];
        if (!slot) {                                             /* full: replace the oldest */
            int64_t oldest = INT64_MAX;
            for (int i = 0; i < MAP_MAX_CONTACTS; i++) {
                int64_t w = report_when(&m->ct[i].rep, m->ct[i].rx_unix);
                if (w < oldest) { oldest = w; slot = &m->ct[i]; }
            }
        }
        memset(slot, 0, sizeof *slot);
        slot->used = 1; slot->id = ++m->next_id; slot->rep = r; slot->rx_unix = now;
        snprintf(slot->reporter, sizeof slot->reporter, "%s", reporter);
        slot->raw_len = (uint8_t)len; memcpy(slot->raw, data, len);
        return 1;
    }
    return 0;
}

/* ---- presentation ---- */

typedef struct { const char *color, *shape; int sev; } look_t;

static look_t contact_look(const report_t *r) {
    look_t l = { "#9E9E9E", "square", 0 };
    const rvalue_t *v;
    if (report_color_value(r, &v)) {
        if (v->color) l.color = v->color;
        if (v->shape) l.shape = v->shape;
        l.sev = v->severity;
    }
    return l;
}

/* latest tinting contact from this reporter, or NULL */
static const contact_t *tint_for(const map_model_t *m, const char *call, int64_t now) {
    const contact_t *best = NULL;
    int64_t best_t = 0;
    for (int i = 0; i < MAP_MAX_CONTACTS; i++) {
        const contact_t *c = &m->ct[i];
        if (!c->used || !c->rep.schema->tint_station || strcmp(c->reporter, call)) continue;
        int64_t w = report_when(&c->rep, c->rx_unix);
        if (alpha_for(c->rep.schema, age_min(w, now)) < 0) continue;
        if (!best || w >= best_t) { best = c; best_t = w; }
    }
    return best;
}

int map_snapshot(map_model_t *m, int64_t now, char *out, size_t cap) {
    if (!out || !cap) return 0;
    map_prune(m, now);
    size_t full = 0, o = 0;
    out[0] = 0;
    char line[420];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < (pass ? MAP_MAX_CONTACTS : MAP_MAX_STATIONS); i++) {
            int n;
            if (pass == 0) {
                const station_t *s = &m->st[i];
                if (!s->used) continue;
                const rschema_t *sc = report_schema_by_id(s->schema_id);
                if (!sc) continue;
                double age = age_min(s->pos_unix ? s->pos_unix : s->rx_unix, now);
                double alpha = s->self ? 1.0 : alpha_for(sc, age);
                const contact_t *t = tint_for(m, s->call, now);
                const char *color = sc->style_color ? sc->style_color : "#1E88E5";
                int sev = 0;
                if (t) { look_t l = contact_look(&t->rep); color = l.color; sev = l.sev; }
                n = snprintf(line, sizeof line, "S\tS:%s\t%s\t%.6f\t%.6f\t%.0f\t%s\t%s\t%.2f\t%.0f\t%d\t%s\t%d\n",
                             s->call, s->call, s->lat, s->lon, s->radius_m, color,
                             sc->style_shape ? sc->style_shape : "rectangle", alpha, age, sev, s->call, s->self);
            } else {
                const contact_t *c = &m->ct[i];
                if (!c->used) continue;
                double lat, lon; report_location(&c->rep, &lat, &lon);
                double age = age_min(report_when(&c->rep, c->rx_unix), now);
                double alpha = alpha_for(c->rep.schema, age);
                look_t l = contact_look(&c->rep);
                char label[160];
                report_summary(&c->rep, label, sizeof label);
                n = snprintf(line, sizeof line, "C\tC:%u\t%s\t%.6f\t%.6f\t0\t%s\t%s\t%.2f\t%.0f\t%d\t%s\t0\n",
                             c->id, c->reporter, lat, lon, l.color, l.shape, alpha, age, l.sev, label);
            }
            if (n < 0) continue;
            if (n >= (int)sizeof line) n = (int)sizeof line - 1;
            if (o + (size_t)n < cap) { memcpy(out + o, line, (size_t)n); o += (size_t)n; out[o] = 0; }
            full += (size_t)n;
        }
    }
    return (int)full;
}

int map_detail(map_model_t *m, const char *key, int64_t now, char *out, size_t cap) {
    if (!key || !out || !cap) return 0;
    out[0] = 0;
    double lat = 0, lon = 0, age = 0;
    size_t o = 0;
    if (!strncmp(key, "S:", 2)) {
        const station_t *s = find_station(m, key + 2, 0, now);
        if (!s) return 0;
        lat = s->lat; lon = s->lon; age = age_min(s->pos_unix ? s->pos_unix : s->rx_unix, now);
        char g[24] = "";
        geo_format(lat, lon, GEO_FMT_MAIDENHEAD, g, sizeof g);
        o = (size_t)snprintf(out, cap, "%s%s\nPosition: %.5f %.5f (%s)\n", s->call, s->self ? " (you)" : "", lat, lon, g);
        if (s->radius_m > 0 && o < cap) o += (size_t)snprintf(out + o, cap - o, "Accuracy: about %.0f m\n", s->radius_m);
        const contact_t *t = tint_for(m, s->call, now);
        if (t && o < cap) {
            char sum[160]; report_summary(&t->rep, sum, sizeof sum);
            o += (size_t)snprintf(out + o, cap - o, "Latest report: %s\n", sum);
        }
    } else if (!strncmp(key, "C:", 2)) {
        unsigned id = (unsigned)strtoul(key + 2, NULL, 10);
        const contact_t *c = NULL;
        for (int i = 0; i < MAP_MAX_CONTACTS; i++) if (m->ct[i].used && m->ct[i].id == id) c = &m->ct[i];
        if (!c) return 0;
        report_location(&c->rep, &lat, &lon);
        age = age_min(report_when(&c->rep, c->rx_unix), now);
        o = (size_t)snprintf(out, cap, "Reported by %s\n", c->reporter);
        if (o < cap) o += (size_t)report_detail(&c->rep, out + o, cap - o);
    } else return 0;
    if (o < cap) o += (size_t)snprintf(out + o, cap - o, "Age: %.0f min\n", age);
    if (m->has_self_pos && o < cap) {
        double d = geo_distance_m(m->self_lat, m->self_lon, lat, lon), b = geo_bearing_deg(m->self_lat, m->self_lon, lat, lon);
        if (d >= 1000) o += (size_t)snprintf(out + o, cap - o, "From you: %.1f km, bearing %03.0f\n", d / 1000.0, b);
        else o += (size_t)snprintf(out + o, cap - o, "From you: %.0f m, bearing %03.0f\n", d, b);
    }
    if (o >= cap) { o = cap - 1; out[o] = 0; }
    return (int)o;
}

int map_station_count(const map_model_t *m) { int n = 0; for (int i = 0; i < MAP_MAX_STATIONS; i++) n += m->st[i].used; return n; }
int map_contact_count(const map_model_t *m) { int n = 0; for (int i = 0; i < MAP_MAX_CONTACTS; i++) n += m->ct[i].used; return n; }
