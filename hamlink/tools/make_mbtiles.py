#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an offline map pack (raster MBTiles) for HamLink from a tile server you are ALLOWED to bulk-download from.

Good sources: your own tile server (e.g. tileserver-gl rendering an OpenStreetMap extract; see docs/OFFLINE_MAPS.md)
and USGS The National Map (US government, see --preset usgs-topo). NOT openstreetmap.org: its tile policy forbids
bulk downloading, so this script refuses it.

    python tools/make_mbtiles.py --preset usgs-topo --bbox -76.75,36.60,-76.45,36.85 --zoom 8-15 --out suffolk-topo.mbtiles
    python tools/make_mbtiles.py --url "http://localhost:8080/styles/basic/{z}/{x}/{y}.png" \\
           --bbox -76.75,36.60,-76.45,36.85 --zoom 0-16 --attribution "(c) OpenStreetMap contributors" --out suffolk.mbtiles

The bounding box is west,south,east,north in decimal degrees. Interrupted runs resume: tiles already in the file are
skipped. Uses only the Python standard library.
"""
import argparse
import math
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PRESETS = {
    # US Geological Survey, The National Map: topographic base map (contours, trails, place names), US only.
    "usgs-topo": ("https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}",
                  "USGS The National Map", 16),
    # USGS imagery with topographic labels, US only.
    "usgs-imagery-topo": ("https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryTopo/MapServer/tile/{z}/{y}/{x}",
                          "USGS The National Map: orthoimagery", 16),
}
FORBIDDEN_HOSTS = ("openstreetmap.org", "openstreetmap.fr", "openstreetmap.de")  # bulk download not allowed
UA = "HamLink-make_mbtiles/1 (+https://github.com/MRC757/hamlink; offline map pack builder)"


def tile_range(bbox, z):
    """Inclusive x and y tile ranges covering bbox (west, south, east, north) at zoom z (Web Mercator, XYZ)."""
    w, s, e, n = bbox
    def tx(lon): return int((lon + 180.0) / 360.0 * (1 << z))
    def ty(lat):
        lat = max(-85.05112878, min(85.05112878, lat))
        r = math.radians(lat)
        return int((1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * (1 << z))
    hi = (1 << z) - 1
    return (max(0, tx(w)), min(hi, tx(e))), (max(0, ty(n)), min(hi, ty(s)))


def count(bbox, zooms):
    return sum((x1 - x0 + 1) * (y1 - y0 + 1) for z in zooms for (x0, x1), (y0, y1) in [tile_range(bbox, z)])


def open_db(path):
    db = sqlite3.connect(path)
    db.executescript("""
        CREATE TABLE IF NOT EXISTS metadata (name TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS tiles (zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, tile_data BLOB);
        CREATE UNIQUE INDEX IF NOT EXISTS tile_index ON tiles (zoom_level, tile_column, tile_row);
    """)
    return db


def fetch(url, retries=4):
    for attempt in range(retries):
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None                                   # no tile here (e.g. outside the server's coverage)
            if e.code in (403, 429) or e.code >= 500:
                wait = int(e.headers.get("Retry-After", "0") or 0) or 10 * (attempt + 1)
                if e.code == 403 and attempt == retries - 1:
                    sys.exit(f"the server refuses us (403) at {url}; stop and check its terms of use")
                print(f"  server says {e.code}; waiting {wait} s", file=sys.stderr)
                time.sleep(wait)
            else:
                raise
        except (urllib.error.URLError, TimeoutError) as e:
            print(f"  {e}; retrying", file=sys.stderr)
            time.sleep(5 * (attempt + 1))
    return None


def image_format(data):
    if data[:8] == b"\x89PNG\r\n\x1a\n": return "png"
    if data[:3] == b"\xff\xd8\xff": return "jpg"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP": return "webp"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", help="tile URL template with {z} {x} {y}")
    ap.add_argument("--preset", choices=sorted(PRESETS), help="a known public source instead of --url")
    ap.add_argument("--bbox", required=True, help="west,south,east,north in degrees, e.g. -76.75,36.60,-76.45,36.85")
    ap.add_argument("--zoom", default="0-15", help="zoom range, e.g. 8-15 (default 0-15)")
    ap.add_argument("--out", required=True, help="output .mbtiles file (resumed if it exists)")
    ap.add_argument("--name", help="name shown in HamLink's Map setup")
    ap.add_argument("--attribution", help="credit line shown on the map (required by most sources)")
    ap.add_argument("--delay", type=float, default=0.25, help="seconds between requests (default 0.25: be polite)")
    ap.add_argument("--max-tiles", type=int, default=100_000, help="refuse bigger jobs (default 100000)")
    ap.add_argument("--yes", action="store_true", help="do not ask before starting")
    a = ap.parse_args()

    if a.preset:
        url, attribution, top = PRESETS[a.preset]
        attribution = a.attribution or attribution
    elif a.url:
        url, attribution, top = a.url, a.attribution or "", 22
    else:
        ap.error("give --url or --preset")
    if not all(k in url for k in ("{z}", "{x}", "{y}")):
        ap.error("the URL needs {z}, {x} and {y}")
    host = urllib.parse.urlparse(url.replace("{z}", "0").replace("{x}", "0").replace("{y}", "0")).hostname or ""
    if any(host == h or host.endswith("." + h) for h in FORBIDDEN_HOSTS):
        sys.exit(f"{host}: OpenStreetMap's tile policy forbids bulk downloading. Render your own tiles instead "
                 "(docs/OFFLINE_MAPS.md) or use another source.")
    try:
        bbox = [float(v) for v in a.bbox.split(",")]
        assert len(bbox) == 4 and bbox[0] < bbox[2] and bbox[1] < bbox[3]
    except (ValueError, AssertionError):
        ap.error("--bbox must be west,south,east,north with west < east and south < north")
    z0, _, z1 = a.zoom.partition("-")
    zooms = range(int(z0), int(z1 or z0) + 1)
    if zooms.stop - 1 > top:
        print(f"note: this source stops at zoom {top}; HamLink enlarges the deepest tiles when you zoom further", file=sys.stderr)
        zooms = range(zooms.start, top + 1)

    total = count(bbox, zooms)
    est_mb = total * 25 / 1024                                     # ~25 kB per raster tile on average
    print(f"{total} tiles, zoom {zooms.start}-{zooms.stop - 1}, roughly {est_mb:.0f} MB, "
          f"about {total * (a.delay + 0.15) / 60:.0f} min at --delay {a.delay}")
    if total > a.max_tiles:
        sys.exit(f"more than --max-tiles {a.max_tiles}; shrink the area or zoom range, or raise --max-tiles if the source allows it")
    if not a.yes and input("start? [y/N] ").strip().lower() != "y":
        return

    db = open_db(a.out)
    have = {(z, x, r) for z, x, r in db.execute("SELECT zoom_level, tile_column, tile_row FROM tiles")}
    fmt, done, got, t0 = None, 0, 0, time.time()
    for z in zooms:
        (x0, x1), (y0, y1) = tile_range(bbox, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                done += 1
                row = (1 << z) - 1 - y                            # MBTiles rows count from the bottom (TMS)
                if (z, x, row) in have:
                    continue
                data = fetch(url.replace("{z}", str(z)).replace("{x}", str(x)).replace("{y}", str(y)))
                if data:
                    f = image_format(data)
                    if f is None:
                        sys.exit(f"zoom {z} tile {x},{y} is not an image (vector or error page?); HamLink needs png/jpg/webp")
                    fmt = fmt or f
                    db.execute("INSERT OR REPLACE INTO tiles VALUES (?,?,?,?)", (z, x, row, sqlite3.Binary(data)))
                    got += 1
                if done % 200 == 0:
                    db.commit()
                    print(f"  {done}/{total} ({got} new) {time.time() - t0:.0f} s", flush=True)
                time.sleep(a.delay)
        db.commit()

    fmt = fmt or (db.execute("SELECT value FROM metadata WHERE name='format'").fetchone() or ["png"])[0]
    w, s, e, n = bbox
    old = dict(db.execute("SELECT name, value FROM metadata"))           # a resumed run keeps the name it was given
    attribution = a.attribution or old.get("attribution") or attribution
    meta = {"name": a.name or old.get("name") or a.out.rsplit("/", 1)[-1].rsplit("\\", 1)[-1].removesuffix(".mbtiles"),
            "format": fmt, "type": "baselayer", "version": "1",
            "bounds": f"{w},{s},{e},{n}", "center": f"{(w + e) / 2},{(s + n) / 2},{zooms.start}",
            "minzoom": str(zooms.start), "maxzoom": str(zooms.stop - 1),
            "attribution": attribution, "description": f"Made with HamLink tools/make_mbtiles.py from {host}"}
    db.executemany("INSERT OR REPLACE INTO metadata VALUES (?,?)", meta.items())
    db.commit()
    n_tiles = db.execute("SELECT COUNT(*) FROM tiles").fetchone()[0]
    db.execute("VACUUM")
    db.close()
    print(f"wrote {a.out}: {n_tiles} tiles ({fmt}). In HamLink: Map setup > Open map file...")


if __name__ == "__main__":
    main()
