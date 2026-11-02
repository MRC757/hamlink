# Extending HamLink

The design rule: **what a report contains, how it looks on the map and how it ages are data**, not code. UIs ask the core.
Adding or changing most things touches one place.

```
schemas/*.json  --(tools/gen_schemas.py)-->  core/schemas_gen.[ch]  -->  core/report.c   (encode, decode, text)
                                                                    -->  core/mapmodel.c (colours, shapes, ageing)
                                  embedded JSON  -->  hl_schema_json  -->  report entry forms in both apps
```

## Add a new report type (e.g. a 9-line MEDEVAC or a damage report)
1. Copy `schemas/salute.json` to `schemas/<name>.json`. Pick an unused `schema` id (1-255; 1 = POS, 2 = SALUTE,
   3 = SITREP) and a `name`.
2. Describe `fields`. Kinds: `enum` (choices with `code`, `name`, optional `color`, `shape`, `severity`, `radius_m`),
   `uint` (`bits` 1-16, optional `max`), `text` (`max` 1-63, 6-bit charset), `latlon`, `time` (24-bit minutes), `flags`
   (up to 32 named bits). Add `"optional": true` for fields that may be left out (costs 1 bit).
3. Add a `map` section if it should appear on the map: `role` `contact` (an icon at the report's own location) or `station`
   (a reporter's own position), `locField`, `timeField`, `colorField` (an enum whose values carry colours), `tintStation` (the reporter's
   station icon takes the `colorField` colour of its latest report of this type; works with role `none`, as the SITREP does),
   `staleMin`, `expireMin`, and a `label` template such as `"{assess}: {size_n} {size_what}"`.
4. Run `python3 tools/gen_schemas.py`. It validates everything (unique ids and keys, colours, field references, worst-case
   size must fit one frame) and refuses with a message otherwise. Commit the regenerated `core/schemas_gen.[ch]`.
5. Rebuild: the Android build compiles `core/`; for Windows run `windows/build-native.sh` to refresh `hamlink_core.dll`.
6. That is all for the UIs: any schema whose `map.role` is `contact` or `none` (not drawn on the map, like SITREP) gets a
   form in the **New report** dialog automatically
   (choices become chips/drop-downs, flags become check boxes, `latlon` accepts any format, `time` becomes "how long ago").
7. Add a test next to `tests/report_test.c` (round trip, size) and a case in `tests/test_reports_e2e.py`.

`pytest tests/test_report.py` fails if the checked-in generated files are stale.

**Changing an existing type:** bump `version`. Receivers refuse a report whose version differs from theirs (it is shown as
undecodable and ignored, never misread), so stations must update together. Do not reuse a schema id for something else.

**Unknown types are harmless:** a station that does not know a message type or schema id still relays it (mesh types 5-15 are
forwarded untouched) and its map ignores what it cannot decode.

## Change how things look or age
Edit colours, shapes, `severity` (draw order), `staleMin` and `expireMin` in the schema JSON and regenerate. The apps never
hard-code them. The only drawing code is the shape switch in `MapView.kt` / `MapView.cs` (`rectangle`, `square`, `diamond`,
`quatrefoil`); add a shape name there and use it in a schema.

## Add a tile source (e.g. an offline MBTiles/PMTiles pack)
Implement `TileSource` (Android, `Tiles.kt`) / `ITileSource` (Windows, `Tiles.cs`): `Tile(z, x, y)` returns a bitmap if
available now, else null and starts loading, then signals `onChange`/`Changed` when it arrives. Construct it in `MapScreen.kt`
/ `MapPanel.cs` instead of `HttpTileSource`. Nothing else knows where pictures come from. The existing source already
shows the pattern for caching and cancelling stale requests.

## Add a position source (e.g. an external GNSS over Bluetooth)
Android: implement `PositionProvider` (`Positions.kt`) and add a mode name in `Positions.apply`. Windows: add a case in
`Positions.Apply` (`Positions.cs`); a source that needs the native NMEA parser can queue bytes in `Positions.NmeaBytes` as the
serial provider does. Provide a position as `Fix(lat, lon, radiusM, source)` where `source` is the exact name of a choice in
the `source` field of `schemas/position.json`. A new kind of source (say "RTK GPS") gets a new choice there, with a `radius_m`.

## Add a coordinate format
`core/geo.c`: add parsing to `geo_parse` and formatting to `geo_format`, then a test in `tests/test_geo.py` against an
independent oracle (the existing tests use exact rational arithmetic, `pyproj` and the `mgrs` package).

## Add a message type to the radio protocol
`enum` in `core/mesh.h` (types up to 15 are relayed even by builds that do not know them) and `docs/PROTOCOL.md`. Callbacks
deliver `type` and the raw bytes; the UIs' `onMessage` decides what to do.

## Map of the code
| Concern | Core (C) | Android | Windows |
|---|---|---|---|
| Report codec, text | `report.c`, `schemas_gen.c` | JNI `reportSend/Summary` | `Radio.ReportSend` |
| Map model | `mapmodel.c` | `MapItem` (parses snapshot) | `MapItem` |
| Drawing | none | `MapView.kt` | `MapView.cs` |
| Tiles | none | `Tiles.kt` | `Tiles.cs` |
| Position sources | `geo.c`, `nmea.c` | `Positions.kt` | `Positions.cs` |
| Forms | schema JSON | `ReportForm.kt` | `ReportWindow.cs` |
| Screens | none | `MapScreen.kt` | `MapPanel.cs`, `MapSetupWindow.cs` |
