// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.Concurrent;
using System.IO;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Data.Sqlite;

namespace HamLink;

/// <summary>
/// An offline map pack: an MBTiles file (SQLite) of raster tiles, e.g. built with tools/make_mbtiles.py.
/// MBTiles stores rows bottom-up (TMS), so tile_row = 2^z - 1 - y. Read-only; safe to use from several threads.
/// </summary>
internal sealed class MapPack : IDisposable
{
    readonly SqliteConnection db;
    readonly object dbLock = new();
    public string FilePath { get; }
    public string Name { get; }
    public string Attribution { get; }
    public int MinZoom { get; }
    public int MaxZoom { get; }
    /// <summary>West, south, east, north in degrees, or null when the file does not say.</summary>
    public double[]? Bounds { get; }

    MapPack(string path, SqliteConnection db, Dictionary<string, string> meta, int minZ, int maxZ)
    {
        FilePath = path;
        this.db = db;
        Name = meta.GetValueOrDefault("name") is { Length: > 0 } n ? n : Path.GetFileNameWithoutExtension(path);
        // MBTiles attribution is often HTML (links); the map shows plain text
        Attribution = Regex.Replace(meta.GetValueOrDefault("attribution") ?? "", "<[^>]*>", "").Replace("&copy;", "©").Trim();
        MinZoom = minZ;
        MaxZoom = maxZ;
        var b = (meta.GetValueOrDefault("bounds") ?? "").Split(',');
        if (b.Length == 4 && b.All(v => double.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out _)))
            Bounds = b.Select(v => double.Parse(v, System.Globalization.CultureInfo.InvariantCulture)).ToArray();
    }

    /// <summary>Opens and checks a pack. Throws <see cref="InvalidDataException"/> with a readable message if it is not usable.</summary>
    public static MapPack Open(string path)
    {
        if (!File.Exists(path)) throw new InvalidDataException("The map file was not found: " + path);
        var db = new SqliteConnection(new SqliteConnectionStringBuilder { DataSource = path, Mode = SqliteOpenMode.ReadOnly, Pooling = false }.ToString());
        try
        {
            db.Open();
            var meta = new Dictionary<string, string>();
            using (var cmd = db.CreateCommand())
            {
                cmd.CommandText = "SELECT name, value FROM metadata";
                using var r = cmd.ExecuteReader();
                while (r.Read()) meta[r.GetString(0)] = r.IsDBNull(1) ? "" : r.GetString(1);
            }
            string fmt = (meta.GetValueOrDefault("format") ?? "png").ToLowerInvariant();
            if (fmt is "pbf" or "mvt")
                throw new InvalidDataException("This is a vector map (format pbf). HamLink needs raster tiles (png or jpg); see tools/make_mbtiles.py.");
            int minZ, maxZ;
            using (var cmd = db.CreateCommand())
            {
                cmd.CommandText = "SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles";
                using var r = cmd.ExecuteReader();
                if (!r.Read() || r.IsDBNull(0)) throw new InvalidDataException("The map file contains no tiles.");
                minZ = r.GetInt32(0); maxZ = r.GetInt32(1);
            }
            return new MapPack(path, db, meta, minZ, maxZ);
        }
        catch (SqliteException ex)
        {
            db.Dispose();
            throw new InvalidDataException("Not an MBTiles map file (" + ex.Message + ").");
        }
        catch
        {
            db.Dispose();
            throw;
        }
    }

    /// <summary>The tile's image bytes, or null if the pack does not have it.</summary>
    public byte[]? Read(int z, int x, int y)
    {
        if (z < MinZoom || z > MaxZoom) return null;
        lock (dbLock)
        {
            using var cmd = db.CreateCommand();
            cmd.CommandText = "SELECT tile_data FROM tiles WHERE zoom_level = $z AND tile_column = $x AND tile_row = $r";
            cmd.Parameters.AddWithValue("$z", z);
            cmd.Parameters.AddWithValue("$x", x);
            cmd.Parameters.AddWithValue("$r", (1 << z) - 1 - y);
            return cmd.ExecuteScalar() as byte[];
        }
    }

    /// <summary>One line for Map setup: name, zoom range, area, size.</summary>
    public string Describe()
    {
        string area = Bounds is { } b ? $", {b[1]:0.##}..{b[3]:0.##} N/S, {b[0]:0.##}..{b[2]:0.##} E/W" : "";
        double mb = new FileInfo(FilePath).Length / 1048576.0;
        return $"{Name}: zoom {MinZoom}-{MaxZoom}{area}, {mb:0} MB";
    }

    public void Dispose() { lock (dbLock) db.Dispose(); }
}

/// <summary>
/// Map pack first, online tiles (and their disk cache) for anything the pack lacks. Zoomed in beyond the pack's deepest
/// level, the pack's last tiles are enlarged (up to <see cref="MaxOverzoom"/> levels) rather than leaving the map blank.
/// </summary>
internal sealed class PackTileSource : ITileSource
{
    const int MaxOverzoom = 5;
    readonly MapPack pack;
    readonly ITileSource fallback;
    readonly object memLock = new();
    readonly Dictionary<long, ImageSource> mem = new();
    readonly LinkedList<long> lru = new();
    readonly ConcurrentDictionary<long, byte> inflight = new(), missing = new();
    int notifyPending;

    public PackTileSource(MapPack pack, ITileSource fallback)
    {
        this.pack = pack;
        this.fallback = fallback;
        fallback.Changed += () => Changed?.Invoke();
    }

    public string Attribution =>
        pack.Attribution.Length == 0 || pack.Attribution == fallback.Attribution ? fallback.Attribution
        : $"{pack.Attribution}   |   {fallback.Attribution}";
    public int MaxZoom => Math.Max(fallback.MaxZoom, pack.MaxZoom + MaxOverzoom);
    public event Action? Changed;

    static long Key(int z, int x, int y) => ((long)z << 56) | ((long)x << 28) | (uint)y;

    public ImageSource? Tile(int z, int x, int y)
    {
        int n = 1 << z;
        if (z < 0 || y < 0 || y >= n) return null;
        int xw = ((x % n) + n) % n;
        long k = Key(z, xw, y);
        lock (memLock)
            if (mem.TryGetValue(k, out var img)) { lru.Remove(k); lru.AddFirst(k); return img; }
        if (missing.ContainsKey(k)) return fallback.Tile(z, xw, y);
        if (inflight.TryAdd(k, 0)) _ = Task.Run(() => Load(z, xw, y, k));
        return null; // the pack answers in milliseconds; asking the network meanwhile would waste a download
    }

    void Load(int z, int x, int y, long k)
    {
        try
        {
            ImageSource? img = null;
            if (z <= pack.MaxZoom)
            {
                if (pack.Read(z, x, y) is { } bytes) img = HttpTileSource.Decode(bytes);
            }
            else if (z - pack.MaxZoom <= MaxOverzoom)
            {
                int dz = z - pack.MaxZoom;
                if (pack.Read(pack.MaxZoom, x >> dz, y >> dz) is { } bytes && HttpTileSource.Decode(bytes) is BitmapSource parent)
                    img = Enlarge(parent, dz, x & ((1 << dz) - 1), y & ((1 << dz) - 1));
            }
            if (img == null) { missing[k] = 0; Notify(); return; } // falls through to the online source
            lock (memLock)
            {
                mem[k] = img; lru.Remove(k); lru.AddFirst(k);
                while (lru.Count > 160) { mem.Remove(lru.Last!.Value); lru.RemoveLast(); }
            }
            Notify();
        }
        catch (Exception ex)
        {
            App.LogError("map pack tile", ex);
            missing[k] = 0;
        }
        finally
        {
            inflight.TryRemove(k, out _);
        }
    }

    /// <summary>The (sx, sy) part of a parent tile, 2^dz parts per side, scaled up to a whole tile.</summary>
    static ImageSource? Enlarge(BitmapSource parent, int dz, int sx, int sy)
    {
        int parts = 1 << dz;
        int w = parent.PixelWidth / parts, h = parent.PixelHeight / parts;
        if (w < 1 || h < 1) return null;
        // Scale a piece one source pixel larger on each side (where the parent has it), then trim that margin off:
        // smoothing at the piece's edges then blends with real neighbouring pixels, not with nothing, so no seams.
        int x0 = Math.Max(0, sx * w - 1), y0 = Math.Max(0, sy * h - 1);
        int x1 = Math.Min(parent.PixelWidth, sx * w + w + 1), y1 = Math.Min(parent.PixelHeight, sy * h + h + 1);
        double fx = (double)parent.PixelWidth / w, fy = (double)parent.PixelHeight / h;
        var big = new TransformedBitmap(new CroppedBitmap(parent, new Int32Rect(x0, y0, x1 - x0, y1 - y0)), new ScaleTransform(fx, fy));
        int left = (int)Math.Round((sx * w - x0) * fx), top = (int)Math.Round((sy * h - y0) * fy);
        var trimmed = new CroppedBitmap(big, new Int32Rect(left, top,
            Math.Min(parent.PixelWidth, big.PixelWidth - left), Math.Min(parent.PixelHeight, big.PixelHeight - top)));
        trimmed.Freeze();
        return trimmed;
    }

    void Notify()
    {
        if (Interlocked.Exchange(ref notifyPending, 1) == 1) return;
        Application.Current?.Dispatcher.BeginInvoke(() =>
        {
            Interlocked.Exchange(ref notifyPending, 0);
            Changed?.Invoke();
        }, System.Windows.Threading.DispatcherPriority.Background);
    }
}
