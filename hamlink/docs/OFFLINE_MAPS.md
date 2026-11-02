# Offline maps

HamLink draws raster map tiles. It can get them from three places, tried in this order:

1. **An offline map file** (Map setup > *Open map file...*): a raster **MBTiles** file covering your area. Works with no
   internet at all. Zoomed in past the file's deepest level, HamLink enlarges its last tiles (up to 5 levels) instead of
   going blank.
2. **Tiles saved from the tile server**: every tile you have viewed is kept on disk (300 MB by default) and used forever.
3. **The tile server** (Map setup > *Tile server URL*), when *Download map tiles when needed* is on. The default is
   openstreetmap.org. Any `https://` server works, and plain `http://` is accepted for a server on the same computer or
   local network (`localhost`, `10.x`, `172.16-31.x`, `192.168.x`, `*.local`, ...), e.g. a laptop at a command post.

**Do not bulk-download openstreetmap.org tiles** (panning over a whole county to fill the cache, or any download tool).
Its [tile usage policy](https://operations.osmfoundation.org/policies/tiles/) forbids it, and HamLink and
`tools/make_mbtiles.py` both refuse to do it. Build a map file from one of the sources below instead.

## Making a map file: `tools/make_mbtiles.py`

The script downloads one area (a bounding box) at a range of zoom levels from a tile server you are allowed to
bulk-download from, and writes an MBTiles file. It needs only Python 3, is polite (a delay between requests, an
identifying User-Agent, backs off when the server asks), and resumes if interrupted.

```
python tools/make_mbtiles.py --preset usgs-topo --bbox -76.75,36.60,-76.45,36.85 --zoom 8-15 \
       --name "Suffolk topo" --out suffolk-topo.mbtiles
```

`--bbox` is *west,south,east,north* in decimal degrees (find it by right-clicking corners on any online map). The script
prints the tile count, size and time and asks before starting. Then copy the file to the PC or phone and open it in
Map setup. On Android the file is copied into HamLink's own storage, so you can delete the downloaded copy afterwards.

How big? At 37 degrees N one zoom-15 tile is about 1 km across, and each zoom level has 4 times the tiles of the one
before:

| Area | Zoom | Tiles | Size (about) |
|---|---|---|---|
| A town, 20 x 15 km | 8-15 | ~400 | 10 MB |
| A county, 30 x 30 km | 8-16 | ~4,500 | 110 MB |
| A state the size of Virginia | 6-14 | ~40,000 | 1 GB |

Tip: a whole state at zoom 12 plus your own county at zoom 16 in a second run is much smaller than everything at 16.
Running the script twice with the same `--out` adds the second area to the same file.

## Source 1: USGS The National Map (United States)

`--preset usgs-topo` (topographic map: contours, trails, land cover, names) or `--preset usgs-imagery-topo` (aerial
photos with labels), up to zoom 16. These are US Geological Survey services; US government data is generally in the
public domain, but check the USGS's current terms for your use, and keep the default `--delay`. Tested: a Suffolk, VA sample (zoom 10-14) downloaded and displayed correctly.

## Source 2: your own OpenStreetMap tiles (anywhere)

Render raster tiles yourself from OpenStreetMap data, then run `make_mbtiles.py` against your own server:

1. Get vector map data for your region. [Protomaps](https://docs.protomaps.com/basemaps/downloads) publishes a free daily
   world map; cut out your area with its `pmtiles` tool, for example
   `pmtiles extract https://build.protomaps.com/<date>.pmtiles region.pmtiles --bbox=-76.75,36.60,-76.45,36.85 --maxzoom=15`.
   (Or use an OpenMapTiles `.mbtiles` vector file from MapTiler.)
2. Serve it as raster images with [TileServer GL](https://github.com/maptiler/tileserver-gl), which draws vector data
   with a map style and serves PNG tiles at `http://localhost:8080/styles/<style>/{z}/{x}/{y}.png`.
3. Build the map file from it:
   `python tools/make_mbtiles.py --url "http://localhost:8080/styles/<style>/{z}/{x}/{y}.png" --bbox ... --zoom 0-16
   --delay 0 --attribution "Protomaps (c) OpenStreetMap" --out region.mbtiles`

Or skip step 3 and point HamLink straight at that server (Map setup > Tile server URL) from any PC or phone on the same
network. Steps 1-2 depend on those projects' current versions; they have not been tested with HamLink here.
OpenStreetMap data is under the [ODbL](https://www.openstreetmap.org/copyright): keep the attribution.

## Not usable

* **Vector MBTiles / PMTiles files directly** (format `pbf`): HamLink only draws raster images. Render them to raster
  (Source 2) first. HamLink says so if you open one.
* **Google, Bing, Esri** map or satellite tiles: their terms do not allow offline copies.
