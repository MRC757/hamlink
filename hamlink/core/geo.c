/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "geo.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define DEG (PI / 180.0)

/* ====================================================================== Maidenhead */

/* Levels: field 18 x 20/10 deg, square 10 x 2/1 deg, subsquare 24 x 5'/2.5', extended 10 x 30"/15", sub-extended 24. */
static const int MH_COUNT[5] = {18, 10, 24, 10, 24};

static void mh_cell_size(int level, double *wx, double *wy) {
    double x = 20.0, y = 10.0;
    /* level 0 cell = 20 x 10 deg; each deeper cell is the parent divided by that level's count */
    for (int l = 1; l <= level; l++) { x /= MH_COUNT[l]; y /= MH_COUNT[l]; }
    *wx = x;
    *wy = y;
}

int geo_maidenhead_encode(double lat, double lon, int chars, char *out) {
    if (!out || chars < 2 || chars > 10 || (chars & 1) || !(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0))
        return 0;
    if (lat >= 90.0) lat = 90.0 - 1e-9;
    if (lon >= 180.0) lon = 180.0 - 1e-9;
    double x = lon + 180.0, y = lat + 90.0;
    for (int level = 0; level < chars / 2; level++) {
        double wx, wy;
        mh_cell_size(level, &wx, &wy);
        int ix = (int)floor(x / wx), iy = (int)floor(y / wy);
        if (ix >= MH_COUNT[level]) ix = MH_COUNT[level] - 1;
        if (iy >= MH_COUNT[level]) iy = MH_COUNT[level] - 1;
        if (ix < 0) ix = 0;
        if (iy < 0) iy = 0;
        if (level % 2 == 0) { /* letter pairs: A-R first, then a-x */
            out[2 * level] = (char)((level == 0 ? 'A' : 'a') + ix);
            out[2 * level + 1] = (char)((level == 0 ? 'A' : 'a') + iy);
        } else {
            out[2 * level] = (char)('0' + ix);
            out[2 * level + 1] = (char)('0' + iy);
        }
        x -= ix * wx;
        y -= iy * wy;
    }
    out[chars] = 0;
    return chars;
}

int geo_maidenhead_decode(const char *g, double *lat, double *lon, double *half_lat, double *half_lon) {
    if (!g) return 0;
    size_t n = strlen(g);
    if (n < 2 || n > 10 || (n & 1)) return 0;
    double x = -180.0, y = -90.0, wx = 0, wy = 0;
    for (size_t level = 0; level < n / 2; level++) {
        char a = g[2 * level], b = g[2 * level + 1];
        int ix, iy;
        if (level % 2 == 0) {
            ix = toupper((unsigned char)a) - 'A';
            iy = toupper((unsigned char)b) - 'A';
        } else {
            if (!isdigit((unsigned char)a) || !isdigit((unsigned char)b)) return 0;
            ix = a - '0';
            iy = b - '0';
        }
        if (ix < 0 || iy < 0 || ix >= MH_COUNT[level] || iy >= MH_COUNT[level]) return 0;
        mh_cell_size((int)level, &wx, &wy);
        x += ix * wx;
        y += iy * wy;
    }
    /* wx, wy are the size of the selected cell */
    if (lat) *lat = y + wy / 2.0;
    if (lon) *lon = x + wx / 2.0;
    if (half_lat) *half_lat = wy / 2.0;
    if (half_lon) *half_lon = wx / 2.0;
    return (int)n;
}

/* ====================================================================== UTM (Kruger series, WGS84) */

#define WGS_A 6378137.0
#define WGS_F (1.0 / 298.257223563)
#define K0 0.9996

typedef struct { double n, A, alpha[5], beta[5], delta[5]; } kruger_t;

static const kruger_t *kruger(void) {
    static kruger_t k;
    static int ready = 0;
    if (!ready) {
        double n = WGS_F / (2.0 - WGS_F), n2 = n * n, n3 = n2 * n, n4 = n3 * n;
        k.n = n;
        k.A = WGS_A / (1.0 + n) * (1.0 + n2 / 4.0 + n4 / 64.0);
        k.alpha[1] = n / 2 - 2 * n2 / 3 + 5 * n3 / 16 + 41 * n4 / 180;
        k.alpha[2] = 13 * n2 / 48 - 3 * n3 / 5 + 557 * n4 / 1440;
        k.alpha[3] = 61 * n3 / 240 - 103 * n4 / 140;
        k.alpha[4] = 49561 * n4 / 161280;
        k.beta[1] = n / 2 - 2 * n2 / 3 + 37 * n3 / 96 - n4 / 360;
        k.beta[2] = n2 / 48 + n3 / 15 - 437 * n4 / 1440;
        k.beta[3] = 17 * n3 / 480 - 37 * n4 / 840;
        k.beta[4] = 4397 * n4 / 161280;
        k.delta[1] = 2 * n - 2 * n2 / 3 - 2 * n3 + 116 * n4 / 45;
        k.delta[2] = 7 * n2 / 3 - 8 * n3 / 5 - 227 * n4 / 45;
        k.delta[3] = 56 * n3 / 15 - 136 * n4 / 35;
        k.delta[4] = 4279 * n4 / 630;
        ready = 1;
    }
    return &k;
}

static int utm_zone_for(double lat, double lon) {
    int zone = (int)floor((lon + 180.0) / 6.0) + 1;
    if (zone < 1) zone = 1;
    if (zone > 60) zone = 60;
    if (lat >= 56.0 && lat < 64.0 && lon >= 3.0 && lon < 12.0) zone = 32; /* Norway */
    if (lat >= 72.0 && lat < 84.0) {                                       /* Svalbard */
        if (lon >= 0.0 && lon < 9.0) zone = 31;
        else if (lon >= 9.0 && lon < 21.0) zone = 33;
        else if (lon >= 21.0 && lon < 33.0) zone = 35;
        else if (lon >= 33.0 && lon < 42.0) zone = 37;
    }
    return zone;
}

static double utm_cm(int zone) { return ((zone - 1) * 6 - 180 + 3) * DEG; }

static void utm_forward(double lat, double lon, int zone, double *E, double *N) {
    const kruger_t *k = kruger();
    double phi = lat * DEG, lam = lon * DEG - utm_cm(zone);
    while (lam > PI) lam -= 2 * PI;
    while (lam < -PI) lam += 2 * PI;
    double sphi = sin(phi), c = 2.0 * sqrt(k->n) / (1.0 + k->n);
    double t = sinh(atanh(sphi) - c * atanh(c * sphi));
    double xi_p = atan2(t, cos(lam)), eta_p = atanh(sin(lam) / sqrt(1.0 + t * t));
    double xi = xi_p, eta = eta_p;
    for (int j = 1; j <= 4; j++) {
        xi += k->alpha[j] * sin(2 * j * xi_p) * cosh(2 * j * eta_p);
        eta += k->alpha[j] * cos(2 * j * xi_p) * sinh(2 * j * eta_p);
    }
    *E = 500000.0 + K0 * k->A * eta;
    *N = K0 * k->A * xi + (lat < 0 ? 10000000.0 : 0.0);
}

static void utm_inverse(double E, double N, int zone, int south, double *lat, double *lon) {
    const kruger_t *k = kruger();
    double xi_p = (N - (south ? 10000000.0 : 0.0)) / (K0 * k->A), eta_p = (E - 500000.0) / (K0 * k->A);
    double xi0 = xi_p, eta0 = eta_p;
    for (int j = 1; j <= 4; j++) {
        xi0 -= k->beta[j] * sin(2 * j * xi_p) * cosh(2 * j * eta_p);
        eta0 -= k->beta[j] * cos(2 * j * xi_p) * sinh(2 * j * eta_p);
    }
    double chi = asin(sin(xi0) / cosh(eta0)), phi = chi;
    for (int j = 1; j <= 4; j++) phi += k->delta[j] * sin(2 * j * chi);
    double lam = utm_cm(zone) + atan2(sinh(eta0), cos(xi0));
    *lat = phi / DEG;
    *lon = lam / DEG;
}

/* ====================================================================== MGRS */

static const char BANDS[] = "CDEFGHJKLMNPQRSTUVWX";
static const char *COLSET[3] = {"ABCDEFGH", "JKLMNPQR", "STUVWXYZ"};
static const char ROWS[] = "ABCDEFGHJKLMNPQRSTUV";

int geo_mgrs_encode(double lat, double lon, int digits, char *out, size_t cap) {
    if (!out || cap < 24 || digits < 0 || digits > 10 || (digits & 1)) return 0;
    if (!(lat >= -80.0 && lat <= 84.0) || !(lon >= -180.0 && lon <= 180.0)) return 0;
    int zone = utm_zone_for(lat, lon);
    double E, N;
    utm_forward(lat, lon, zone, &E, &N);
    int bi = (int)floor((lat + 80.0) / 8.0);
    if (bi > 19) bi = 19;
    int e100 = (int)floor(E / 100000.0), n100 = (int)floor(N / 100000.0);
    if (e100 < 1 || e100 > 8) return 0;
    char col = COLSET[(zone - 1) % 3][e100 - 1];
    char row = ROWS[(n100 + (zone % 2 == 0 ? 5 : 0)) % 20];
    long ev = (long)floor(E) % 100000, nv = (long)floor(N) % 100000;
    int d = digits / 2;
    long div = 1;
    for (int i = d; i < 5; i++) div *= 10;
    ev /= div; /* MGRS truncates, it does not round */
    nv /= div;
    int w;
    if (d == 0) w = snprintf(out, cap, "%02d%c %c%c", zone, BANDS[bi], col, row);
    else w = snprintf(out, cap, "%02d%c %c%c %0*ld %0*ld", zone, BANDS[bi], col, row, d, ev, d, nv);
    return w > 0 ? w : 0;
}

int geo_mgrs_decode(const char *text, double *lat, double *lon, double *half_m) {
    if (!text) return 0;
    char s[40];
    size_t n = 0;
    for (const char *p = text; *p; p++) {
        if (*p == ' ' || *p == '\t') continue;
        if (n + 1 >= sizeof s) return 0;
        s[n++] = (char)toupper((unsigned char)*p);
    }
    s[n] = 0;
    size_t i = 0;
    int zone = 0, nd = 0;
    while (i < n && isdigit((unsigned char)s[i]) && nd < 2) { zone = zone * 10 + (s[i] - '0'); i++; nd++; }
    if (nd == 0 || zone < 1 || zone > 60 || i + 3 > n) return 0;
    const char *bp = strchr(BANDS, s[i]);
    if (!bp || !s[i]) return 0;
    int bi = (int)(bp - BANDS);
    i++;
    const char *cp = strchr(COLSET[(zone - 1) % 3], s[i]);
    const char *rp = strchr(ROWS, s[i + 1]);
    if (!cp || !rp || !s[i] || !s[i + 1]) return 0;
    int e100 = (int)(cp - COLSET[(zone - 1) % 3]) + 1;
    int rowi = ((int)(rp - ROWS) - (zone % 2 == 0 ? 5 : 0) + 20) % 20;
    i += 2;
    size_t nl = n - i;
    if (nl > 10 || (nl & 1)) return 0;
    for (size_t k = i; k < n; k++)
        if (!isdigit((unsigned char)s[k])) return 0;
    int d = (int)(nl / 2);
    double size = 1.0;
    for (int k = d; k < 5; k++) size *= 10.0;
    double ev = 0, nv = 0;
    for (int k = 0; k < d; k++) { ev = ev * 10 + (s[i + k] - '0'); nv = nv * 10 + (s[i + d + k] - '0'); }
    ev *= size;
    nv *= size;
    double E = e100 * 100000.0 + ev, Nrel = rowi * 100000.0 + nv;
    /* The row letters repeat every 2000 km: take the first northing at or above the bottom of the latitude band. */
    double band_lat = -80.0 + 8.0 * bi, nmin = 1e18;
    for (int k = -1; k <= 1; k++) {
        double e, nn;
        utm_forward(band_lat, (utm_cm(zone) / DEG) + 3.0 * k * 0.99, zone, &e, &nn);
        if (nn < nmin) nmin = nn;
    }
    double nband = floor(nmin / 100000.0) * 100000.0, N2M = 0;
    while (N2M + Nrel < nband) N2M += 2000000.0;
    double N = N2M + Nrel;
    utm_inverse(E + size / 2.0, N + size / 2.0, zone, bi < 10, lat, lon);
    if (half_m) *half_m = size / 2.0;
    return 1;
}

/* ====================================================================== Web Mercator */

#define MAX_LAT 85.0511287798

void geo_to_world(double lat, double lon, int zoom, double *px, double *py) {
    if (lat > MAX_LAT) lat = MAX_LAT;
    if (lat < -MAX_LAT) lat = -MAX_LAT;
    double size = 256.0 * pow(2.0, zoom), s = sin(lat * DEG);
    if (px) *px = (lon + 180.0) / 360.0 * size;
    if (py) *py = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * PI)) * size;
}

void geo_from_world(double px, double py, int zoom, double *lat, double *lon) {
    double size = 256.0 * pow(2.0, zoom);
    if (lon) *lon = px / size * 360.0 - 180.0;
    if (lat) {
        double y = 0.5 - py / size;
        *lat = 90.0 - 360.0 * atan(exp(-y * 2.0 * PI)) / PI;
    }
}

/* ====================================================================== great circle */

double geo_distance_m(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * DEG, p2 = lat2 * DEG, dp = (lat2 - lat1) * DEG, dl = (lon2 - lon1) * DEG;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 6371008.8 * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * DEG, p2 = lat2 * DEG, dl = (lon2 - lon1) * DEG;
    double y = sin(dl) * cos(p2), x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) / DEG;
    return b < 0 ? b + 360.0 : b;
}

/* ====================================================================== formatting */

static void dms(double v, char pos, char neg, char *out, size_t cap, int deg_width) {
    char h = v < 0 ? neg : pos;
    unsigned long t = (unsigned long)floor(fabs(v) * 36000.0 + 0.5); /* tenths of an arc second */
    snprintf(out, cap, "%0*lud %02lu' %02lu.%lu\" %c", deg_width, t / 36000, (t % 36000) / 600, (t % 600) / 10, t % 10, h);
}

int geo_format(double lat, double lon, int fmt, char *out, size_t cap) {
    if (!out || cap < 32) return 0;
    switch (fmt) {
    case GEO_FMT_DECIMAL: return snprintf(out, cap, "%.5f %.5f", lat, lon);
    case GEO_FMT_DMS: {
        char a[48], b[48];
        dms(lat, 'N', 'S', a, sizeof a, 2);
        dms(lon, 'E', 'W', b, sizeof b, 3);
        return snprintf(out, cap, "%s  %s", a, b);
    }
    case GEO_FMT_MAIDENHEAD: {
        char g[12];
        if (!geo_maidenhead_encode(lat, lon, 8, g)) return 0;
        return snprintf(out, cap, "%s", g);
    }
    case GEO_FMT_MGRS: return geo_mgrs_encode(lat, lon, 10, out, cap);
    }
    return 0;
}

/* ====================================================================== parsing typed coordinates */

typedef struct { int hem; double v; int neg; char h; } tok_t; /* hem=1: hemisphere letter h, else number v (neg: had a minus) */

static int parse_latlon(const char *s, double *lat, double *lon) {
    tok_t t[32];
    int nt = 0;
    for (const char *p = s; *p;) {
        unsigned char c = (unsigned char)*p;
        int neg = 0;
        const char *q = p;
        if ((c == '-' || c == '+') && (isdigit((unsigned char)p[1]) || p[1] == '.')) { neg = c == '-'; q = p + 1; }
        if (isdigit((unsigned char)*q) || (*q == '.' && isdigit((unsigned char)q[1]))) {
            char num[32];
            size_t k = 0;
            const char *r = q;
            int dots = 0;
            while ((isdigit((unsigned char)*r) || (*r == '.' && dots++ == 0)) && k + 1 < sizeof num) num[k++] = *r++;
            num[k] = 0;
            if (nt >= 32) return 0;
            t[nt].hem = 0; t[nt].v = atof(num); t[nt].neg = neg; t[nt].h = 0; nt++;
            p = r;
        } else if (strchr("NSEWnsew", c) && c) {
            if (nt >= 32) return 0;
            t[nt].hem = 1; t[nt].v = 0; t[nt].neg = 0; t[nt].h = (char)toupper(c); nt++;
            p++;
        } else {
            p++; /* separators: space, comma, quotes, degree sign bytes, d/m markers ... */
        }
    }
    int nh = 0, nn = 0;
    for (int i = 0; i < nt; i++) { if (t[i].hem) nh++; else nn++; }
    double val[2] = {0, 0};
    char hem[2] = {0, 0};
    if (nh == 0) {
        if (nn != 2 && nn != 4 && nn != 6) return 0;
        int per = nn / 2;
        for (int g = 0; g < 2; g++) {
            const tok_t *a = &t[g * per];
            double v = fabs(a[0].v);
            if (per >= 2) { if (a[1].v >= 60.0 || a[1].neg) return 0; v += a[1].v / 60.0; }
            if (per >= 3) { if (a[2].v >= 60.0 || a[2].neg) return 0; v += a[2].v / 3600.0; }
            val[g] = a[0].neg ? -v : v;
        }
        *lat = val[0];
        *lon = val[1];
    } else if (nh == 2) {
        int prefix = t[0].hem; /* "N 38.9 W 77.0" vs "38.9 N 77.0 W" */
        int seen = 0, cnt[2] = {0, 0}, idx[2][3];
        for (int i = 0; i < nt; i++) {
            if (t[i].hem) {
                if (seen >= 2) return 0;
                hem[seen++] = t[i].h;
                continue;
            }
            int gi = prefix ? seen - 1 : seen;
            if (gi < 0 || gi > 1 || cnt[gi] >= 3) return 0;
            idx[gi][cnt[gi]++] = i;
        }
        if (cnt[0] == 0 || cnt[1] == 0) return 0;
        for (int k = 0; k < 2; k++) {
            double v = fabs(t[idx[k][0]].v);
            if (cnt[k] >= 2) { double m = t[idx[k][1]].v; if (m >= 60.0) return 0; v += m / 60.0; }
            if (cnt[k] >= 3) { double sx = t[idx[k][2]].v; if (sx >= 60.0) return 0; v += sx / 3600.0; }
            val[k] = (hem[k] == 'S' || hem[k] == 'W') ? -v : v;
        }
        int a0 = (hem[0] == 'N' || hem[0] == 'S'), a1 = (hem[1] == 'N' || hem[1] == 'S');
        if (a0 == a1) return 0; /* two latitudes or two longitudes */
        *lat = a0 ? val[0] : val[1];
        *lon = a0 ? val[1] : val[0];
    } else {
        return 0;
    }
    return *lat >= -90.0 && *lat <= 90.0 && *lon >= -180.0 && *lon <= 180.0;
}

int geo_parse(const char *text, double *lat, double *lon, double *radius_m, int *kind) {
    if (!text || !lat || !lon) return 0;
    double la, lo, rad = 0.0;
    int k = GEO_KIND_LATLON;

    char compact[48];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof compact; p++)
        if (*p != ' ' && *p != '\t') compact[n++] = *p;
    compact[n] = 0;

    /* Maidenhead: 4, 6, 8 or 10 chars, letters first */
    double hl, ho;
    if ((n == 4 || n == 6 || n == 8 || n == 10) && isalpha((unsigned char)compact[0]) &&
        geo_maidenhead_decode(compact, &la, &lo, &hl, &ho)) {
        rad = geo_distance_m(la, lo, la + hl, lo + ho);
        k = GEO_KIND_MAIDENHEAD;
    } else {
        double hm;
        if (n >= 3 && isdigit((unsigned char)compact[0]) && geo_mgrs_decode(compact, &la, &lo, &hm)) {
            rad = hm * 1.41421356;
            k = GEO_KIND_MGRS;
        } else if (!parse_latlon(text, &la, &lo)) {
            return 0;
        }
    }
    *lat = la;
    *lon = lo;
    if (radius_m) *radius_m = rad;
    if (kind) *kind = k;
    return 1;
}
