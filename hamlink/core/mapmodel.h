/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Map model: which stations and contacts exist, how they look, how they age. No drawing, no I/O.
 * The UIs render the text snapshot, so changing colours, shapes or ageing rules never touches UI code:
 * colours and shapes come from the JSON files in the schemas directory, ageing from the schema's map.staleMin / map.expireMin. */
#ifndef HAMLINK_MAPMODEL_H
#define HAMLINK_MAPMODEL_H
#include <stddef.h>
#include <stdint.h>
#include "report.h"

#define MAP_MAX_STATIONS 64
#define MAP_MAX_CONTACTS 64
#define MAP_MIN_ALPHA 0.25

typedef struct map_model map_model_t;

map_model_t *map_create(void);
void map_destroy(map_model_t *m);
void map_clear(map_model_t *m);

void map_set_self(map_model_t *m, const char *callsign);
/* Own position (any source). radius_m = uncertainty (grid squares), now_unix = when it was determined. */
void map_set_self_position(map_model_t *m, double lat, double lon, double radius_m, int64_t now_unix);
int map_self_position(const map_model_t *m, double *lat, double *lon);

/* Feed a received (or own) MT_REPORT payload. Returns 1 if it changed the map, 0 if ignored (duplicate, no
 * location, expired), negative REP_ERR_* if it did not decode. Unknown schemas return REP_ERR_SCHEMA and are harmless. */
int map_ingest(map_model_t *m, const char *reporter, const uint8_t *data, size_t len, int64_t now_unix);

/* Drop expired entries. Called by map_snapshot as well; exposed for tests. */
void map_prune(map_model_t *m, int64_t now_unix);

/* One line per item, tab separated, '\n' terminated:
 *   S key call lat lon radius_m color shape alpha age_min sev label self
 *   C key reporter lat lon radius_m color shape alpha age_min sev label 0
 * key is stable while the item exists ("S:CALL", "C:<n>"). Returns the full length (may exceed cap; output is
 * truncated on a line boundary and always NUL terminated). */
int map_snapshot(map_model_t *m, int64_t now_unix, char *out, size_t cap);

/* Details for a snapshot key: report fields plus distance/bearing from our own position. 0 if unknown. */
int map_detail(map_model_t *m, const char *key, int64_t now_unix, char *out, size_t cap);

int map_station_count(const map_model_t *m);
int map_contact_count(const map_model_t *m);

#endif
