/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Geodesy helpers shared by both apps: Maidenhead grid squares, MGRS/UTM, Web Mercator map math,
 * distance/bearing, and parsing/formatting of typed coordinates. All strings are plain ASCII. */
#ifndef HAMLINK_GEO_H
#define HAMLINK_GEO_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Maidenhead locator ---- */
/* chars = 2, 4, 6, 8 or 10. out needs chars+1 bytes. Returns chars written, 0 on bad input. */
int geo_maidenhead_encode(double lat, double lon, int chars, char *out);
/* Decodes 2/4/6/8/10-character locators (any case). Gives the cell centre and its half-extent in degrees.
 * Returns the number of characters, or 0 if invalid. */
int geo_maidenhead_decode(const char *grid, double *lat, double *lon, double *half_lat, double *half_lon);

/* ---- UTM / MGRS (WGS84). Valid for latitudes -80 .. 84. ---- */
/* digits = 2 * precision digits per axis: 0..10 total digits (even), e.g. 10 = 1 m, 8 = 10 m. */
int geo_mgrs_encode(double lat, double lon, int digits, char *out, size_t cap);
/* Accepts "18S UJ 23478 06483" with or without spaces. Returns 1 and the centre of the referenced square,
 * with the square's half-size in metres. */
int geo_mgrs_decode(const char *text, double *lat, double *lon, double *half_m);

/* ---- Web Mercator (EPSG:3857) in 256 px tiles ---- */
void geo_to_world(double lat, double lon, int zoom, double *px, double *py);
void geo_from_world(double px, double py, int zoom, double *lat, double *lon);

/* ---- great circle ---- */
double geo_distance_m(double lat1, double lon1, double lat2, double lon2);
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/* ---- typed coordinates ---- */
#define GEO_FMT_DECIMAL 0   /* 38.88950 -77.03530 */
#define GEO_FMT_DMS 1       /* 38d 53' 22.2" N  77d 02' 07.1" W */
#define GEO_FMT_MAIDENHEAD 2 /* FM18lv55 */
#define GEO_FMT_MGRS 3      /* 18S UJ 23478 06483 */
int geo_format(double lat, double lon, int fmt, char *out, size_t cap);

#define GEO_KIND_LATLON 0
#define GEO_KIND_MAIDENHEAD 1
#define GEO_KIND_MGRS 2
/* Parses decimal degrees, degrees-minutes(-seconds) with hemisphere letters, a Maidenhead locator (4+ chars)
 * or an MGRS reference. Returns 1 on success. radius_m is the uncertainty of the typed value (0 for exact
 * coordinates, the half-diagonal of the square for grids). Non-ASCII separators (e.g. the degree sign) are fine. */
int geo_parse(const char *text, double *lat, double *lon, double *radius_m, int *kind);

#ifdef __cplusplus
}
#endif
#endif
