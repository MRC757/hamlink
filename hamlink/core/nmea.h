/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal NMEA 0183 GPS parser: GGA (fix quality, HDOP, satellites) and RMC (position, speed, course).
 * Feed it the raw serial byte stream in any chunking; it reports a fix whenever one is complete and valid. */
#ifndef HAMLINK_NMEA_H
#define HAMLINK_NMEA_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char line[128];
    size_t n;
    int overflow;
    double speed_kmh, course_deg, hdop;
    int sats, quality;
    uint32_t good, bad_checksum;
} nmea_t;

typedef struct {
    double lat, lon, speed_kmh, course_deg, hdop;
    int sats;
} nmea_fix_t;

void nmea_init(nmea_t *s);
/* Returns 1 and fills *fix when the bytes completed a valid position sentence (the last one wins if several). */
int nmea_feed(nmea_t *s, const uint8_t *bytes, size_t n, nmea_fix_t *fix);

#endif
