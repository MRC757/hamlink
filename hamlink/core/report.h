/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Generic, schema-driven report codec (position, SALUTE, and anything added under schemas/).
 *
 * A report is a schema id plus one value per field. On the air it is bit-packed:
 *   [schema_id u8][schema_version u8][MSB-first bit stream, zero padded to a byte]
 * Nothing in this file knows what a "SALUTE" is; all knowledge lives in the generated tables. */
#ifndef HAMLINK_REPORT_H
#define HAMLINK_REPORT_H
#include <stddef.h>
#include <stdint.h>
#include "schemas_gen.h"

#define REPORT_MAX_FIELDS 32
#define REPORT_TEXT_MAX 63
#define REPORT_MAX_BYTES 94                 /* fits one class-0 mesh fragment */
#define REPORT_EPOCH_UNIX 1577836800LL      /* 2020-01-01T00:00:00Z; time fields are minutes since this */

enum { REP_OK = 0, REP_ERR_ARG = -1, REP_ERR_SCHEMA = -2, REP_ERR_FIELD = -3, REP_ERR_VALUE = -4,
       REP_ERR_SHORT = -5, REP_ERR_VERSION = -6, REP_ERR_SIZE = -7 };

typedef struct {
    uint8_t present;                        /* optional fields: 0 = absent. Required fields are always present on decode */
    uint32_t u;                             /* enum code, uint value, flags mask, or time (minutes since epoch) */
    double lat, lon;                        /* latlon fields */
    char text[REPORT_TEXT_MAX + 1];
} rvalue_slot_t;

typedef struct {
    const rschema_t *schema;
    rvalue_slot_t v[REPORT_MAX_FIELDS];
} report_t;

const rschema_t *report_schema_by_id(int id);
const rschema_t *report_schema_by_name(const char *name);
int report_field_index(const rschema_t *s, const char *key);
const rvalue_t *report_enum_value(const rfield_t *f, uint32_t code);   /* NULL if unknown code */

void report_init(report_t *r, const rschema_t *s);

/* Set fields from "key=value" lines. Values: enum by name (case-insensitive) or code; uint decimal; text as typed
 * (lower case folded to upper, unsupported characters rejected); latlon in any geo_parse format; time as unix seconds
 * or "now" (needs now_unix); flags as comma separated names. Returns REP_OK or an error and writes a message to err. */
int report_from_kv(report_t *r, const char *kv, int64_t now_unix, char *err, size_t errcap);
int report_to_kv(const report_t *r, char *out, size_t cap);

int report_validate(const report_t *r, char *err, size_t errcap);     /* required fields set, values in range */
int report_encode(const report_t *r, uint8_t *out, size_t cap);        /* bytes written or negative REP_ERR_* */
int report_decode(report_t *r, const uint8_t *data, size_t len);       /* REP_OK or negative; r->schema set on success */
int report_peek_schema(const uint8_t *data, size_t len);                /* schema id or -1 */

/* One line, e.g. "Hostile: 3 vehicles, moving". Uses the schema's map.label template or the first fields. */
int report_summary(const report_t *r, char *out, size_t cap);
/* Multi line "Label: value" text for a details view. */
int report_detail(const report_t *r, char *out, size_t cap);

/* Convenience for the map model. Returns 1 and the position if the schema has a location field that is present. */
int report_location(const report_t *r, double *lat, double *lon);
int report_time_unix(const report_t *r, int64_t *t);                   /* 1 if the schema has a present time field */
int report_color_value(const report_t *r, const rvalue_t **v);         /* enum value selected by map.colorField, or NULL */

#endif
