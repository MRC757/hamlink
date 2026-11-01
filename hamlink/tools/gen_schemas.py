#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate core/schemas_gen.[ch] from schemas/*.json.

The JSON files are the single source of truth for report types: the C codec, the map colours and both apps'
entry forms all derive from them. After editing a schema run:  python3 tools/gen_schemas.py
`--check` exits non-zero if the checked-in generated files are out of date (used by the test suite).
"""
import glob
import json
import math
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KINDS = {"enum": 0, "uint": 1, "text": 2, "latlon": 3, "time": 4, "flags": 5}
ROLES = {"none": 0, "station": 1, "contact": 2}
SHAPES = {"rectangle", "diamond", "square", "quatrefoil"}
MAX_PAYLOAD_BITS = (96 - 2) * 8      # one class-0 fragment minus schema id and version bytes
KEY_RE = re.compile(r"^[a-z][a-z0-9_]*$")
HEX_RE = re.compile(r"^#[0-9A-Fa-f]{6}$")


class SchemaError(Exception):
    pass


def bits_for(n):                      # bits needed to store values 0..n
    return max(1, math.ceil(math.log2(n + 1))) if n > 0 else 1


def ascii_only(s, what):
    if not isinstance(s, str) or not s.isascii() or any(c in s for c in "\n\r|"):
        raise SchemaError(f"{what}: must be plain ASCII without newlines or '|': {s!r}")
    return s


def check_schema(path, s):
    where = os.path.basename(path)
    def err(msg): raise SchemaError(f"{where}: {msg}")
    for k in ("schema", "version", "name", "title", "fields"):
        if k not in s: err(f"missing '{k}'")
    if not (isinstance(s["schema"], int) and 1 <= s["schema"] <= 255): err("'schema' must be 1..255")
    if not (isinstance(s["version"], int) and 1 <= s["version"] <= 255): err("'version' must be 1..255")
    ascii_only(s["name"], "name"); ascii_only(s["title"], "title")
    fields, keys, total_bits, texts = s["fields"], set(), 0, 0
    if not (1 <= len(fields) <= 32): err("1..32 fields required")
    for f in fields:
        key = f.get("key", "")
        if not KEY_RE.match(key): err(f"bad field key {key!r}")
        if key in keys: err(f"duplicate field key {key!r}")
        keys.add(key)
        ascii_only(f.get("label", ""), f"{key}.label")
        kind = f.get("kind")
        if kind not in KINDS: err(f"{key}: kind must be one of {sorted(KINDS)}")
        bits = 0
        if kind == "enum":
            vals = f.get("values") or err(f"{key}: enum needs 'values'")
            codes = [v["code"] for v in vals]
            if len(set(codes)) != len(codes) or min(codes) < 0 or max(codes) > 255: err(f"{key}: enum codes must be unique 0..255")
            for v in vals:
                ascii_only(v["name"], f"{key} value name")
                if "color" in v and not HEX_RE.match(v["color"]): err(f"{key}: bad colour {v['color']!r}")
                if "shape" in v and v["shape"] not in SHAPES: err(f"{key}: shape must be one of {sorted(SHAPES)}")
            bits = bits_for(max(codes))
        elif kind == "uint":
            b = f.get("bits")
            if not (isinstance(b, int) and 1 <= b <= 16): err(f"{key}: uint needs 'bits' 1..16")
            if "max" in f and not (0 <= f["max"] < (1 << b)): err(f"{key}: 'max' does not fit in {b} bits")
            bits = b
        elif kind == "text":
            m = f.get("max")
            if not (isinstance(m, int) and 1 <= m <= 63): err(f"{key}: text needs 'max' 1..63")
            bits = bits_for(m) + 6 * m            # worst case: length + 6 bits per character
            texts += 1
        elif kind == "latlon":
            bits = 48
        elif kind == "time":
            bits = 24
        elif kind == "flags":
            vals = f.get("values") or err(f"{key}: flags needs 'values'")
            if len(vals) > 32 or sorted(v["code"] for v in vals) != list(range(len(vals))): err(f"{key}: flag codes must be 0..n-1, at most 32")
            for v in vals: ascii_only(v["name"], f"{key} flag name")
            bits = len(vals)
        total_bits += bits + (1 if f.get("optional") else 0)
    if texts > 8: err("at most 8 text fields")
    if total_bits > MAX_PAYLOAD_BITS:
        err(f"worst-case size {total_bits} bits exceeds one fragment ({MAX_PAYLOAD_BITS} bits)")
    m = s.get("map", {})
    role = m.get("role", "none")
    if role not in ROLES: err(f"map.role must be one of {sorted(ROLES)}")
    bykey = {f["key"]: f for f in fields}
    if role != "none":
        if bykey.get(m.get("locField"), {}).get("kind") != "latlon": err("map.locField must name a latlon field")
        if m.get("timeField") and bykey.get(m["timeField"], {}).get("kind") != "time": err("map.timeField must name a time field")
    if "colorField" in m and bykey.get(m["colorField"], {}).get("kind") != "enum": err("map.colorField must name an enum field")
    for k in re.findall(r"\{(\w+)\}", m.get("label", "")):
        if k not in bykey: err(f"map.label uses unknown field {{{k}}}")
    if role == "station" and not ("style" in m and HEX_RE.match(m["style"].get("color", "")) and m["style"].get("shape") in SHAPES):
        err("a station role needs map.style {color, shape}")
    return total_bits


def cstr(s):
    return json.dumps(s) if s is not None else "NULL"


def generate():
    paths = sorted(glob.glob(os.path.join(ROOT, "schemas", "*.json")))
    schemas, ids = [], set()
    for p in paths:
        s = json.load(open(p))
        bits = check_schema(p, s)
        if s["schema"] in ids: raise SchemaError(f"duplicate schema id {s['schema']}")
        ids.add(s["schema"])
        schemas.append((s, bits))
    schemas.sort(key=lambda t: t[0]["schema"])

    h = ['/* SPDX-License-Identifier: GPL-3.0-or-later */', '/* GENERATED by tools/gen_schemas.py from the schemas directory. DO NOT EDIT. */',
         '#ifndef HAMLINK_SCHEMAS_GEN_H', '#define HAMLINK_SCHEMAS_GEN_H', '#include <stdint.h>', '',
         '#define RK_ENUM 0', '#define RK_UINT 1', '#define RK_TEXT 2', '#define RK_LATLON 3', '#define RK_TIME 4', '#define RK_FLAGS 5',
         '#define RR_NONE 0', '#define RR_STATION 1', '#define RR_CONTACT 2', '',
         'typedef struct { int code; const char *name; const char *color; const char *shape; int radius_m; int severity; } rvalue_t;',
         'typedef struct {', '    const char *key, *label;', '    uint8_t kind, optional, bits, maxlen;', '    uint16_t nvalues;', '    uint32_t max;',
         '    const rvalue_t *values;', '} rfield_t;',
         'typedef struct {', '    uint8_t id, version, role, nfields, tint_station;', '    int8_t loc_field, time_field, color_field;',
         '    uint16_t stale_min, expire_min;', '    const char *name, *title, *label_tpl, *style_color, *style_shape, *json;',
         '    const rfield_t *fields;', '} rschema_t;', '',
         'extern const rschema_t *const RSCHEMAS[];', 'extern const int RSCHEMA_COUNT;', '', '#endif', '']
    c = ['/* SPDX-License-Identifier: GPL-3.0-or-later */', '/* GENERATED by tools/gen_schemas.py from the schemas directory. DO NOT EDIT. */', '#include <stddef.h>', '#include "schemas_gen.h"', '']
    for s, wbits in schemas:
        n = s["name"]
        for f in s["fields"]:
            if f["kind"] in ("enum", "flags"):
                c.append(f"static const rvalue_t V_{n}_{f['key']}[] = {{")
                for v in f["values"]:
                    c.append(f"    {{{v['code']}, {cstr(v['name'])}, {cstr(v.get('color'))}, {cstr(v.get('shape'))}, "
                             f"{v.get('radius_m', 0)}, {v.get('severity', v['code'])}}},")
                c.append("};")
        c.append(f"static const rfield_t F_{n}[] = {{")
        for f in s["fields"]:
            kind = f["kind"]
            if kind == "enum": bits = bits_for(max(v["code"] for v in f["values"])); nv = len(f["values"]); mx = max(v["code"] for v in f["values"])
            elif kind == "uint": bits = f["bits"]; nv = 0; mx = f.get("max", (1 << f["bits"]) - 1)
            elif kind == "text": bits = bits_for(f["max"]); nv = 0; mx = f["max"]
            elif kind == "flags": bits = len(f["values"]); nv = len(f["values"]); mx = (1 << len(f["values"])) - 1
            else: bits = 0; nv = 0; mx = 0
            vals = f"V_{n}_{f['key']}" if kind in ("enum", "flags") else "0"
            maxlen = f["max"] if kind == "text" else 0
            c.append(f"    {{{cstr(f['key'])}, {cstr(f['label'])}, RK_{kind.upper()}, {1 if f.get('optional') else 0}, {bits}, {maxlen}, {nv}, {mx}u, {vals}}},")
        c.append("};")
        m = s.get("map", {})
        idx = {f["key"]: i for i, f in enumerate(s["fields"])}
        role = "RR_" + m.get("role", "none").upper()
        style = m.get("style", {})
        j = json.dumps(s, separators=(",", ":"))
        c.append(f"static const char J_{n}[] = {cstr(j)};")
        c.append(f"/* {s['title']}: worst case {(wbits + 7) // 8 + 2} bytes on the air */")
        c.append(f"static const rschema_t S_{n} = {{{s['schema']}, {s['version']}, {role}, {len(s['fields'])}, {1 if m.get('tintStation') else 0}, "
                 f"{idx.get(m.get('locField'), -1)}, {idx.get(m.get('timeField'), -1)}, {idx.get(m.get('colorField'), -1)}, "
                 f"{m.get('staleMin', 30)}, {m.get('expireMin', 240)}, {cstr(n)}, {cstr(s['title'])}, {cstr(m.get('label'))}, "
                 f"{cstr(style.get('color'))}, {cstr(style.get('shape'))}, J_{n}, F_{n}}};")
        c.append("")
    c.append("const rschema_t *const RSCHEMAS[] = {")
    for s, _ in schemas: c.append(f"    &S_{s['name']},")
    c.append("};")
    c.append(f"const int RSCHEMA_COUNT = {len(schemas)};")
    c.append("")
    return "\n".join(h), "\n".join(c)


if __name__ == "__main__":
    try:
        hdr, src = generate()
    except SchemaError as e:
        print("schema error:", e, file=sys.stderr)
        sys.exit(2)
    out = {os.path.join(ROOT, "core", "schemas_gen.h"): hdr, os.path.join(ROOT, "core", "schemas_gen.c"): src}
    if "--check" in sys.argv:
        stale = [p for p, t in out.items() if not os.path.exists(p) or open(p).read() != t]
        if stale:
            print("out of date (run tools/gen_schemas.py):", *stale)
            sys.exit(1)
        print("generated files are up to date")
    else:
        for p, t in out.items():
            open(p, "w").write(t)
        print("wrote", *[os.path.relpath(p, ROOT) for p in out])
