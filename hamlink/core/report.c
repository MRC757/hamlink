/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "report.h"
#include "geo.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 64 symbols, 6 bits each. No '|' or newline: those are reserved by the text interfaces. */
static const char CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,-/:;@#+()=!?'\"%*&_<>[]$^~";
typedef char charset_is_64[(sizeof(CHARSET) - 1 == 64) ? 1 : -1];

static int char_code(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (!c) return -1;
    const char *p = strchr(CHARSET, c);
    return p ? (int)(p - CHARSET) : -1;
}

/* Small portable replacements: the Windows (mingw) build lacks gmtime_r, strtok_r and strcasecmp. */
static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
}
/* Next token of `*pos` up to one of `delims`; modifies the buffer. NULL at the end. */
static char *next_tok(char **pos, const char *delims) {
    char *s = *pos;
    if (!s) return NULL;
    while (*s && strchr(delims, *s)) s++;
    if (!*s) { *pos = NULL; return NULL; }
    char *e = s;
    while (*e && !strchr(delims, *e)) e++;
    if (*e) { *e = 0; *pos = e + 1; } else *pos = NULL;
    return s;
}
/* Civil date from days since 1970-01-01 (Howard Hinnant's algorithm). */
static void civil_from_days(int64_t z, int *day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
}

/* ---------------- schema lookup ---------------- */

const rschema_t *report_schema_by_id(int id) {
    for (int i = 0; i < RSCHEMA_COUNT; i++) if (RSCHEMAS[i]->id == id) return RSCHEMAS[i];
    return NULL;
}
const rschema_t *report_schema_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < RSCHEMA_COUNT; i++) if (!ci_cmp(RSCHEMAS[i]->name, name)) return RSCHEMAS[i];
    return NULL;
}
int report_field_index(const rschema_t *s, const char *key) {
    for (int i = 0; i < s->nfields; i++) if (!strcmp(s->fields[i].key, key)) return i;
    return -1;
}
const rvalue_t *report_enum_value(const rfield_t *f, uint32_t code) {
    for (int i = 0; i < f->nvalues; i++) if ((uint32_t)f->values[i].code == code) return &f->values[i];
    return NULL;
}

void report_init(report_t *r, const rschema_t *s) {
    memset(r, 0, sizeof *r);
    r->schema = s;
}

/* ---------------- bit streams ---------------- */

typedef struct { uint8_t *b; size_t cap; size_t nbits; int over; } bw_t;
static void bw_put(bw_t *w, uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) {
        size_t byte = w->nbits >> 3;
        if (byte >= w->cap) { w->over = 1; return; }
        if (!(w->nbits & 7)) w->b[byte] = 0;
        if ((v >> i) & 1) w->b[byte] |= (uint8_t)(0x80 >> (w->nbits & 7));
        w->nbits++;
    }
}
typedef struct { const uint8_t *b; size_t nbits; size_t pos; int over; } br_t;
static uint32_t br_get(br_t *r, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (r->pos >= r->nbits) { r->over = 1; return 0; }
        v = (v << 1) | ((r->b[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}

static uint32_t quant_lat(double lat) { double q = floor((lat + 90.0) / 180.0 * 16777215.0 + 0.5); return (uint32_t)q; }
static uint32_t quant_lon(double lon) { double q = floor((lon + 180.0) / 360.0 * 16777216.0 + 0.5); return ((uint32_t)q) & 0xFFFFFF; }
static double dequant_lat(uint32_t q) { return (double)q / 16777215.0 * 180.0 - 90.0; }
static double dequant_lon(uint32_t q) { return (double)q / 16777216.0 * 360.0 - 180.0; }


/* ---------------- validation ---------------- */

#define SETERR(...) do { if (err && errcap) snprintf(err, errcap, __VA_ARGS__); } while (0)

int report_validate(const report_t *r, char *err, size_t errcap) {
    if (!r || !r->schema) { SETERR("no schema"); return REP_ERR_ARG; }
    const rschema_t *s = r->schema;
    for (int i = 0; i < s->nfields; i++) {
        const rfield_t *f = &s->fields[i];
        const rvalue_slot_t *v = &r->v[i];
        if (!v->present) {
            if (!f->optional) { SETERR("%s is required", f->label); return REP_ERR_FIELD; }
            continue;
        }
        switch (f->kind) {
        case RK_ENUM:
            if (!report_enum_value(f, v->u)) { SETERR("%s: unknown choice %u", f->label, v->u); return REP_ERR_VALUE; }
            break;
        case RK_UINT:
            if (v->u > f->max) { SETERR("%s: must be at most %u", f->label, f->max); return REP_ERR_VALUE; }
            break;
        case RK_TEXT: {
            size_t n = strlen(v->text);
            if (n > f->maxlen) { SETERR("%s: at most %d characters", f->label, f->maxlen); return REP_ERR_VALUE; }
            for (size_t k = 0; k < n; k++)
                if (char_code(v->text[k]) < 0) { SETERR("%s: character not allowed", f->label); return REP_ERR_VALUE; }
            break; }
        case RK_LATLON:
            if (!(v->lat >= -90 && v->lat <= 90 && v->lon >= -180 && v->lon <= 180)) {
                SETERR("%s: position out of range", f->label); return REP_ERR_VALUE; }
            break;
        case RK_TIME:
            if (v->u >= (1u << 24)) { SETERR("%s: time out of range", f->label); return REP_ERR_VALUE; }
            break;
        case RK_FLAGS:
            if (f->nvalues < 32 && (v->u >> f->nvalues)) { SETERR("%s: unknown flag", f->label); return REP_ERR_VALUE; }
            break;
        }
    }
    return REP_OK;
}

/* ---------------- encode / decode ---------------- */

int report_encode(const report_t *r, uint8_t *out, size_t cap) {
    int rc = report_validate(r, NULL, 0);
    if (rc) return rc;
    if (!out || cap < 3) return REP_ERR_ARG;
    const rschema_t *s = r->schema;
    out[0] = s->id;
    out[1] = s->version;
    bw_t w = { out + 2, cap - 2, 0, 0 };
    for (int i = 0; i < s->nfields; i++) {
        const rfield_t *f = &s->fields[i];
        const rvalue_slot_t *v = &r->v[i];
        if (f->optional) { bw_put(&w, v->present ? 1 : 0, 1); if (!v->present) continue; }
        switch (f->kind) {
        case RK_ENUM: case RK_UINT: case RK_FLAGS: bw_put(&w, v->u, f->bits); break;
        case RK_TIME: bw_put(&w, v->u, 24); break;
        case RK_LATLON: bw_put(&w, quant_lat(v->lat), 24); bw_put(&w, quant_lon(v->lon), 24); break;
        case RK_TEXT: {
            size_t n = strlen(v->text);
            bw_put(&w, (uint32_t)n, f->bits);
            for (size_t k = 0; k < n; k++) bw_put(&w, (uint32_t)char_code(v->text[k]), 6);
            break; }
        }
    }
    size_t total = 2 + ((w.nbits + 7) >> 3);
    if (w.over || total > REPORT_MAX_BYTES) return REP_ERR_SIZE;
    return (int)total;
}

int report_peek_schema(const uint8_t *d, size_t len) { return (d && len >= 2) ? d[0] : -1; }

int report_decode(report_t *r, const uint8_t *d, size_t len) {
    if (!r || !d) return REP_ERR_ARG;
    if (len < 2) return REP_ERR_SHORT;
    const rschema_t *s = report_schema_by_id(d[0]);
    if (!s) return REP_ERR_SCHEMA;
    if (d[1] != s->version) return REP_ERR_VERSION;
    report_init(r, s);
    br_t rd = { d + 2, (len - 2) * 8, 0, 0 };
    for (int i = 0; i < s->nfields; i++) {
        const rfield_t *f = &s->fields[i];
        rvalue_slot_t *v = &r->v[i];
        v->present = 1;
        if (f->optional) { v->present = (uint8_t)br_get(&rd, 1); if (rd.over) return REP_ERR_SHORT; if (!v->present) continue; }
        switch (f->kind) {
        case RK_ENUM: case RK_UINT: case RK_FLAGS:
            v->u = br_get(&rd, f->bits);
            if (f->kind == RK_UINT && v->u > f->max) return REP_ERR_VALUE;
            break;
        case RK_TIME: v->u = br_get(&rd, 24); break;
        case RK_LATLON: { uint32_t a = br_get(&rd, 24), b = br_get(&rd, 24);
            v->lat = dequant_lat(a); v->lon = dequant_lon(b); break; }
        case RK_TEXT: {
            uint32_t n = br_get(&rd, f->bits);
            if (n > f->maxlen) return REP_ERR_VALUE;
            for (uint32_t k = 0; k < n; k++) v->text[k] = CHARSET[br_get(&rd, 6)];
            v->text[n] = 0;
            break; }
        }
        if (rd.over) return REP_ERR_SHORT;
    }
    size_t used = (rd.pos + 7) >> 3;
    if (used != len - 2) return REP_ERR_SIZE;           /* trailing bytes mean a different layout than we think */
    for (size_t b = rd.pos; b < used * 8; b++)          /* padding must be zero */
        if ((d[2 + (b >> 3)] >> (7 - (b & 7))) & 1) return REP_ERR_VALUE;
    if (report_validate(r, NULL, 0)) return REP_ERR_VALUE;
    return REP_OK;
}

/* ---------------- text rendering ---------------- */

static void fmt_time(uint32_t minutes, char *out, size_t cap) {  /* military date-time group, e.g. 311530Z */
    int64_t t = REPORT_EPOCH_UNIX + (int64_t)minutes * 60;
    int64_t days = t / 86400, secs = t % 86400;
    int day;
    civil_from_days(days, &day);
    snprintf(out, cap, "%02d%02d%02dZ", day, (int)(secs / 3600), (int)((secs % 3600) / 60));
}

static void field_text(const rfield_t *f, const rvalue_slot_t *v, char *out, size_t cap) {
    out[0] = 0;
    if (!v->present) return;
    switch (f->kind) {
    case RK_ENUM: { const rvalue_t *e = report_enum_value(f, v->u);
        if (e) snprintf(out, cap, "%s", e->name); else snprintf(out, cap, "?%u", v->u); break; }
    case RK_UINT: snprintf(out, cap, "%u", v->u); break;
    case RK_TEXT: snprintf(out, cap, "%s", v->text); break;
    case RK_LATLON: snprintf(out, cap, "%.5f %.5f", v->lat, v->lon); break;
    case RK_TIME: fmt_time(v->u, out, cap); break;
    case RK_FLAGS: {
        size_t o = 0;
        for (int i = 0; i < f->nvalues && o + 2 < cap; i++)
            if (v->u & (1u << f->values[i].code))
                o += (size_t)snprintf(out + o, cap - o, "%s%s", o ? ", " : "", f->values[i].name);
        break; }
    }
}

static void tidy(char *s) {            /* drop empty template pieces left behind by absent optional fields */
    char *w = s;
    for (char *p = s; *p; p++) {
        if (*p == ' ' && (w == s || w[-1] == ' ' || p[1] == ',' || p[1] == 0)) continue;
        if (*p == ',' && (w == s || w[-1] == ',' || w[-1] == ':')) continue;
        *w++ = *p;
    }
    while (w > s && (w[-1] == ',' || w[-1] == ' ' || w[-1] == ':')) w--;
    *w = 0;
}

int report_summary(const report_t *r, char *out, size_t cap) {
    if (!r || !r->schema || !out || !cap) return REP_ERR_ARG;
    const rschema_t *s = r->schema;
    out[0] = 0;
    size_t o = 0;
    if (s->label_tpl) {
        for (const char *p = s->label_tpl; *p && o + 1 < cap; ) {
            if (*p == '{') {
                const char *e = strchr(p, '}');
                if (e && e - p - 1 < 40) {
                    char key[41]; memcpy(key, p + 1, (size_t)(e - p - 1)); key[e - p - 1] = 0;
                    int i = report_field_index(s, key);
                    if (i >= 0) { char t[96]; field_text(&s->fields[i], &r->v[i], t, sizeof t);
                                  o += (size_t)snprintf(out + o, cap - o, "%s", t); if (o >= cap) o = cap - 1; }
                    p = e + 1;
                    continue;
                }
            }
            out[o++] = *p++;
            out[o] = 0;
        }
        out[o < cap ? o : cap - 1] = 0;
        tidy(out);
        if (out[0]) return (int)strlen(out);
    }
    snprintf(out, cap, "%s", s->title);
    return (int)strlen(out);
}

int report_detail(const report_t *r, char *out, size_t cap) {
    if (!r || !r->schema || !out || !cap) return REP_ERR_ARG;
    const rschema_t *s = r->schema;
    size_t o = (size_t)snprintf(out, cap, "%s\n", s->title);
    for (int i = 0; i < s->nfields && o < cap; i++) {
        if (!r->v[i].present) continue;
        char t[160];
        field_text(&s->fields[i], &r->v[i], t, sizeof t);
        if (s->fields[i].kind == RK_LATLON) {
            char g[24] = "";
            geo_format(r->v[i].lat, r->v[i].lon, GEO_FMT_MAIDENHEAD, g, sizeof g);
            size_t n = strlen(t);
            snprintf(t + n, sizeof t - n, "  (%s)", g);
        }
        o += (size_t)snprintf(out + o, cap - o, "%s: %s\n", s->fields[i].label, t);
    }
    if (o >= cap) { o = cap - 1; out[o] = 0; }
    return (int)o;
}

/* ---------------- accessors for the map ---------------- */

int report_location(const report_t *r, double *lat, double *lon) {
    if (!r || !r->schema || r->schema->loc_field < 0) return 0;
    const rvalue_slot_t *v = &r->v[r->schema->loc_field];
    if (!v->present) return 0;
    *lat = v->lat; *lon = v->lon;
    return 1;
}
int report_time_unix(const report_t *r, int64_t *t) {
    if (!r || !r->schema || r->schema->time_field < 0) return 0;
    const rvalue_slot_t *v = &r->v[r->schema->time_field];
    if (!v->present) return 0;
    *t = REPORT_EPOCH_UNIX + (int64_t)v->u * 60;
    return 1;
}
int report_color_value(const report_t *r, const rvalue_t **out) {
    *out = NULL;
    if (!r || !r->schema || r->schema->color_field < 0) return 0;
    const rfield_t *f = &r->schema->fields[r->schema->color_field];
    const rvalue_slot_t *v = &r->v[r->schema->color_field];
    if (!v->present) return 0;
    *out = report_enum_value(f, v->u);
    return *out != NULL;
}

/* ---------------- key=value interface ---------------- */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
#define FAIL(code, ...) do { if (err && errcap) snprintf(err, errcap, __VA_ARGS__); return (code); } while (0)

static int set_one(report_t *r, int i, char *val, int64_t now_unix, char *err, size_t errcap) {
    const rfield_t *f = &r->schema->fields[i];
    rvalue_slot_t *v = &r->v[i];
    if (!*val && f->optional) { v->present = 0; return REP_OK; }
    switch (f->kind) {
    case RK_ENUM: {
        char *end; long n = strtol(val, &end, 10);
        if (*val && !*end) { if (!report_enum_value(f, (uint32_t)n)) FAIL(REP_ERR_VALUE, "%s: unknown choice %s", f->label, val); v->u = (uint32_t)n; }
        else {
            int hit = 0;
            for (int k = 0; k < f->nvalues; k++) if (!ci_cmp(f->values[k].name, val)) { v->u = (uint32_t)f->values[k].code; hit = 1; }
            if (!hit) FAIL(REP_ERR_VALUE, "%s: unknown choice '%s'", f->label, val);
        }
        break; }
    case RK_UINT: {
        char *end; long n = strtol(val, &end, 10);
        if (!*val || *end || n < 0 || (unsigned long)n > f->max) FAIL(REP_ERR_VALUE, "%s: must be 0..%u", f->label, f->max);
        v->u = (uint32_t)n;
        break; }
    case RK_TEXT: {
        size_t n = strlen(val);
        if (n > f->maxlen) FAIL(REP_ERR_VALUE, "%s: at most %d characters", f->label, f->maxlen);
        for (size_t k = 0; k < n; k++) {
            int c = char_code(val[k]);
            if (c < 0) FAIL(REP_ERR_VALUE, "%s: character '%c' not allowed", f->label, val[k]);
            v->text[k] = CHARSET[c];
        }
        v->text[n] = 0;
        break; }
    case RK_LATLON: {
        double lat, lon, rad; int kind;
        if (!geo_parse(val, &lat, &lon, &rad, &kind)) FAIL(REP_ERR_VALUE, "%s: cannot read position '%s'", f->label, val);
        v->lat = lat; v->lon = lon;
        break; }
    case RK_TIME: {
        int64_t t;
        if (!ci_cmp(val, "now")) { if (now_unix <= 0) FAIL(REP_ERR_VALUE, "%s: clock not set", f->label); t = now_unix; }
        else { char *end; t = strtoll(val, &end, 10); if (!*val || *end) FAIL(REP_ERR_VALUE, "%s: expected unix seconds or 'now'", f->label); }
        int64_t m = (t - REPORT_EPOCH_UNIX) / 60;
        if (t < REPORT_EPOCH_UNIX || m >= (1 << 24)) FAIL(REP_ERR_VALUE, "%s: time out of range", f->label);
        v->u = (uint32_t)m;
        break; }
    case RK_FLAGS: {
        uint32_t mask = 0;
        char tmp[200]; snprintf(tmp, sizeof tmp, "%s", val);
        char *save = tmp;
        for (char *tok = next_tok(&save, ","); tok; tok = next_tok(&save, ",")) {
            tok = trim(tok);
            if (!*tok || !ci_cmp(tok, "none")) continue;
            int hit = 0;
            for (int k = 0; k < f->nvalues; k++) if (!ci_cmp(f->values[k].name, tok)) { mask |= 1u << f->values[k].code; hit = 1; }
            if (!hit) FAIL(REP_ERR_VALUE, "%s: unknown item '%s'", f->label, tok);
        }
        v->u = mask;
        break; }
    }
    v->present = 1;
    return REP_OK;
}

int report_from_kv(report_t *r, const char *kv, int64_t now_unix, char *err, size_t errcap) {
    if (!r || !r->schema || !kv) return REP_ERR_ARG;
    size_t kvlen = strlen(kv);
    char *copy = (char *)malloc(kvlen + 1);
    if (!copy) return REP_ERR_ARG;
    memcpy(copy, kv, kvlen + 1);
    int rc = REP_OK;
    char *save = copy;
    for (char *line = next_tok(&save, "\n"); line; line = next_tok(&save, "\n")) {
        line = trim(line);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) { rc = REP_ERR_FIELD; if (err && errcap) snprintf(err, errcap, "expected key=value: '%s'", line); break; }
        *eq = 0;
        char *key = trim(line), *val = trim(eq + 1);
        int i = report_field_index(r->schema, key);
        if (i < 0) { rc = REP_ERR_FIELD; if (err && errcap) snprintf(err, errcap, "unknown field '%s'", key); break; }
        rc = set_one(r, i, val, now_unix, err, errcap);
        if (rc) break;
    }
    free(copy);
    if (rc) return rc;
    return report_validate(r, err, errcap);
}

int report_to_kv(const report_t *r, char *out, size_t cap) {
    if (!r || !r->schema || !out || !cap) return REP_ERR_ARG;
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < r->schema->nfields; i++) {
        const rfield_t *f = &r->schema->fields[i];
        const rvalue_slot_t *v = &r->v[i];
        if (!v->present) continue;
        char t[160];
        if (f->kind == RK_TIME) snprintf(t, sizeof t, "%lld", (long long)(REPORT_EPOCH_UNIX + (int64_t)v->u * 60));
        else if (f->kind == RK_LATLON) snprintf(t, sizeof t, "%.6f,%.6f", v->lat, v->lon);
        else field_text(f, v, t, sizeof t);
        int n = snprintf(out + o, cap - o, "%s=%s\n", f->key, t);
        if (n < 0 || (size_t)n >= cap - o) return REP_ERR_SIZE;
        o += (size_t)n;
    }
    return (int)o;
}
