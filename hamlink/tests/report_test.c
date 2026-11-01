/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone C tests for the report codec; built with ASan/UBSan by tests/test_report.py. */
#include "../core/report.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static report_t mk(const char *schema, const char *kv, int64_t now) {
    report_t r; char err[120] = "";
    const rschema_t *s = report_schema_by_name(schema);
    assert(s);
    report_init(&r, s);
    int rc = report_from_kv(&r, kv, now, err, sizeof err);
    if (rc) printf("kv error: %s\n", err);
    assert(rc == 0);
    return r;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const int64_t NOW = 1793000000; /* late Oct 2026 */
    CHECK(report_schema_by_id(1) && report_schema_by_id(2) && !report_schema_by_id(99));

    /* position */
    report_t p = mk("POS", "loc=FN31pr\ntime=now\nsource=GPS\nspeed=0\n", NOW);
    uint8_t buf[128];
    int n = report_encode(&p, buf, sizeof buf);
    printf("position bytes: %d\n", n);
    CHECK(n > 0 && n <= 16);
    report_t q;
    CHECK(report_decode(&q, buf, n) == REP_OK);
    double la, lo;
    CHECK(report_location(&q, &la, &lo));
    CHECK(fabs(la - p.v[report_field_index(p.schema, "loc")].lat) < 0.00002);
    CHECK(fabs(lo - p.v[report_field_index(p.schema, "loc")].lon) < 0.00003);
    int64_t t; CHECK(report_time_unix(&q, &t) && llabs(t - NOW) < 60);

    /* SALUTE round trip, lowercase text folded */
    report_t sa = mk("salute",
        "assess=Hostile\nsize_n=3\nsize_what=vehicles\nactivity=moving\nactivity_note=heading north\n"
        "loc=38.8895,-77.0353\nheading=N\nunit=2nd platoon\nobserved=now\nequip=light vehicles, comms gear\n", NOW);
    n = report_encode(&sa, buf, sizeof buf);
    printf("salute bytes: %d\n", n);
    CHECK(n > 0 && n <= REPORT_MAX_BYTES);
    CHECK(report_decode(&q, buf, n) == REP_OK);
    char sum[200], det[600], kv[600];
    report_summary(&q, sum, sizeof sum);
    printf("summary: %s\n", sum);
    CHECK(strstr(sum, "Hostile") && strstr(sum, "3"));
    report_detail(&q, det, sizeof det);
    CHECK(strstr(det, "HEADING NORTH"));
    CHECK(strstr(det, "260733Z"));              /* observed=now(1793000000) as a day-hour-minute group, checked against Python datetime */
    const rvalue_t *cv;
    CHECK(report_color_value(&q, &cv) && !strcmp(cv->color, "#E53935"));
    CHECK(report_to_kv(&q, kv, sizeof kv) > 0);
    report_t q2 = {0}; report_init(&q2, q.schema);
    CHECK(report_from_kv(&q2, kv, NOW, NULL, 0) == 0);
    uint8_t buf2[128];
    int n2 = report_encode(&q2, buf2, sizeof buf2);
    CHECK(n2 == n && !memcmp(buf, buf2, n));             /* kv round trip is byte-exact */

    /* optional fields absent -> still valid and shorter */
    report_t min = mk("salute", "assess=Unknown\nsize_n=1\nsize_what=personnel\nactivity=observing\nloc=FN31pr\nobserved=now\nequip=none\n", NOW);
    int nm = report_encode(&min, buf2, sizeof buf2);
    CHECK(nm > 0 && nm < n);
    CHECK(report_decode(&q2, buf2, nm) == REP_OK);
    report_summary(&q2, sum, sizeof sum);
    printf("minimal summary: %s\n", sum);
    CHECK(sum[0] && sum[strlen(sum) - 1] != ',' && !strstr(sum, ", ,"));

    /* error paths */
    char err[120];
    report_t e; report_init(&e, report_schema_by_name("salute"));
    CHECK(report_from_kv(&e, "bogus=1", NOW, err, sizeof err) == REP_ERR_FIELD);
    report_init(&e, report_schema_by_name("salute"));
    CHECK(report_from_kv(&e, "assess=Purple", NOW, err, sizeof err) == REP_ERR_VALUE && strstr(err, "Purple"));
    report_init(&e, report_schema_by_name("salute"));
    CHECK(report_from_kv(&e, "assess=Hostile", NOW, err, sizeof err) == REP_ERR_FIELD && strstr(err, "required"));
    report_init(&e, report_schema_by_name("salute"));
    CHECK(report_from_kv(&e, "unit=a|b", NOW, err, sizeof err) == REP_ERR_VALUE);
    CHECK(report_decode(&q, buf, 1) == REP_ERR_SHORT);
    uint8_t bad[8] = { 99, 1, 0 };
    CHECK(report_decode(&q, bad, 3) == REP_ERR_SCHEMA);
    memcpy(bad, buf, 8); bad[1] = 77;
    CHECK(report_decode(&q, bad, 8) == REP_ERR_VERSION);
    CHECK(report_decode(&q, buf, n - 1) != REP_OK);       /* truncated */
    uint8_t ext[128]; memcpy(ext, buf, n); ext[n] = 0;
    CHECK(report_decode(&q, ext, n + 1) == REP_ERR_SIZE);  /* trailing byte */

    /* position extremes survive quantisation */
    const char *pts[] = { "90,0", "-90,0", "0,179.9999", "0,-180", "45.5,-122.6" };
    for (int i = 0; i < 5; i++) {
        char k[80]; snprintf(k, sizeof k, "loc=%s\ntime=now\nsource=GPS\n", pts[i]);
        report_t r = mk("POS", k, NOW);
        n = report_encode(&r, buf, sizeof buf);
        CHECK(n > 0 && report_decode(&q, buf, n) == REP_OK);
        report_location(&q, &la, &lo);
        double want_la = r.v[0].lat;
        CHECK(fabs(la - want_la) < 2e-5);
    }

    /* fuzz: random bytes and mutated valid messages must never crash (ASan) */
    srand(12345);
    int ok = 0;
    for (int i = 0; i < 200000; i++) {
        uint8_t f[96]; size_t len = 2 + (size_t)(rand() % 60);
        if (i & 1) { memcpy(f, buf, 40); len = 40; for (int k = 0; k < 3; k++) f[rand() % len] ^= (uint8_t)(1 << (rand() % 8)); }
        else { for (size_t k = 0; k < len; k++) f[k] = (uint8_t)rand(); f[0] = (uint8_t)(1 + rand() % 2); f[1] = 1; }
        report_t d;
        if (report_decode(&d, f, len) == REP_OK) {
            ok++;
            char s[200]; report_summary(&d, s, sizeof s); report_detail(&d, det, sizeof det);
            uint8_t again[128];
            CHECK(report_encode(&d, again, sizeof again) == (int)len);   /* anything that decodes re-encodes identically */
            CHECK(!memcmp(again, f, len));
        }
    }
    printf("fuzz: %d decoded cleanly\n", ok);
    printf(fails ? "FAILED %d\n" : "ALL OK\n", fails);
    return fails != 0;
}
