/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nmea.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void nmea_init(nmea_t *s) { memset(s, 0, sizeof *s); s->hdop = 99.9; }

/* ddmm.mmmm / dddmm.mmmm -> degrees; hemisphere letter gives the sign. Returns 0 on bad input. */
static int coord(const char *v, const char *h, int lon, double *out) {
    if (!v[0] || !h[0]) return 0;
    char *end;
    double x = strtod(v, &end);
    if (end == v || x < 0) return 0;
    double deg = floor(x / 100.0), min = x - deg * 100.0;
    if (min >= 60.0) return 0;
    double r = deg + min / 60.0;
    if (h[0] == 'S' || h[0] == 'W') r = -r;
    else if (h[0] != 'N' && h[0] != 'E') return 0;
    if (fabs(r) > (lon ? 180.0 : 90.0)) return 0;
    *out = r;
    return 1;
}

static int handle(nmea_t *s, char *line, nmea_fix_t *fix) {
    if (line[0] != '$') return 0;
    char *star = strchr(line, '*');
    if (star) {
        unsigned cs = 0;
        for (char *p = line + 1; p < star; p++) cs ^= (unsigned char)*p;
        if (cs != (unsigned)strtoul(star + 1, NULL, 16)) { s->bad_checksum++; return 0; }
        *star = 0;
    }
    char *f[20];
    int nf = 0;
    for (char *p = line + 1; nf < 20;) {
        f[nf++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    if (strlen(f[0]) != 5) return 0;
    const char *type = f[0] + 2; /* skip the two-letter talker (GP, GN, GL, ...) */
    double lat, lon;
    if (!strcmp(type, "GGA") && nf >= 9) {
        s->quality = atoi(f[6]);
        s->sats = atoi(f[7]);
        s->hdop = f[8][0] ? atof(f[8]) : 99.9;
        if (s->quality > 0 && coord(f[2], f[3], 0, &lat) && coord(f[4], f[5], 1, &lon)) {
            fix->lat = lat; fix->lon = lon; fix->speed_kmh = s->speed_kmh; fix->course_deg = s->course_deg;
            fix->hdop = s->hdop; fix->sats = s->sats;
            s->good++;
            return 1;
        }
    } else if (!strcmp(type, "RMC") && nf >= 9) {
        if (f[2][0] == 'A' && coord(f[3], f[4], 0, &lat) && coord(f[5], f[6], 1, &lon)) {
            s->speed_kmh = atof(f[7]) * 1.852;
            s->course_deg = f[8][0] ? atof(f[8]) : 0.0;
            fix->lat = lat; fix->lon = lon; fix->speed_kmh = s->speed_kmh; fix->course_deg = s->course_deg;
            fix->hdop = s->hdop; fix->sats = s->sats;
            s->good++;
            return 1;
        }
    }
    return 0;
}

int nmea_feed(nmea_t *s, const uint8_t *bytes, size_t n, nmea_fix_t *fix) {
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        char c = (char)bytes[i];
        if (c == '$') { s->n = 0; s->overflow = 0; }
        if (c == '\n' || c == '\r') {
            if (s->n > 5 && !s->overflow) {
                s->line[s->n] = 0;
                nmea_fix_t f;
                if (handle(s, s->line, &f)) { *fix = f; got = 1; }
            }
            s->n = 0;
            s->overflow = 0;
            continue;
        }
        if (s->n + 1 < sizeof s->line) s->line[s->n++] = c; else s->overflow = 1;
    }
    return got;
}
