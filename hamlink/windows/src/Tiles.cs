// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.Concurrent;
using System.IO;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace HamLink;

/// <summary>
/// Where map pictures come from. The map view only knows this interface, so an offline pack (MBTiles/PMTiles)
/// or another provider can be added later as another implementation without touching drawing code.
/// </summary>
internal interface ITileSource
{
    string Attribution { get; }
    int MaxZoom { get; }
    /// <summary>Raised on the UI thread when a requested tile has arrived and the map should redraw.</summary>
    event Action? Changed;
    /// <summary>The tile if it is already in memory, else null and a load is started. Never blocks.</summary>
    ImageSource? Tile(int z, int x, int y);
}

/// <summary>Which tile server URLs are allowed: https anywhere, plain http only to this computer or the local network
/// (a tile server at a command post), never across the internet.</summary>
internal static class TileUrls
{
    /// <summary>Null if the template is usable, else a one-line reason for Map setup.</summary>
    public static string? Problem(string template)
    {
        if (!template.Contains("{z}") || !template.Contains("{x}") || !template.Contains("{y}"))
            return "The URL needs {z}, {x} and {y} where the tile numbers go.";
        if (!Uri.TryCreate(template.Replace("{z}", "0").Replace("{x}", "0").Replace("{y}", "0"), UriKind.Absolute, out var u))
            return "This is not a web address.";
        if (u.Scheme == Uri.UriSchemeHttps) return null;
        if (u.Scheme == Uri.UriSchemeHttp)
            return IsLocal(u.Host) ? null : "Plain http:// is only allowed to this computer or the local network; use https://.";
        return "The URL must start with https:// (or http:// for a server on the local network).";
    }

    /// <summary>localhost, a private / link-local IP address, or a local-only name (no dot, .local, .lan, .home.arpa).</summary>
    public static bool IsLocal(string host)
    {
        host = host.Trim('[', ']').ToLowerInvariant();
        if (System.Net.IPAddress.TryParse(host, out var ip))
        {
            if (System.Net.IPAddress.IsLoopback(ip)) return true;
            if (ip.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6)
                return ip.IsIPv6LinkLocal || ip.IsIPv6UniqueLocal;
            byte[] b = ip.GetAddressBytes();
            return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
        }
        return host == "localhost" || !host.Contains('.') || host.EndsWith(".local") || host.EndsWith(".lan") || host.EndsWith(".home.arpa");
    }
}

/// <summary>
/// Slippy-map raster tiles (default: openstreetmap.org) cached on disk under %LOCALAPPDATA%\HamLink\tiles.
/// Follows the OSM tile usage policy (operations.osmfoundation.org/policies/tiles): tiles are fetched only when the
/// visible map needs them (no bulk download, no prefetch), at most 2 connections, an identifying User-Agent with the app
/// version and project URL, HTTP cache headers honoured (max-age or Expires, ETag revalidation, minimum 1 h), all downloads
/// paused after 429/403/5xx (Retry-After honoured), attribution shown. Tiles already on disk work with no network.
/// </summary>
internal sealed class HttpTileSource : ITileSource
{
    static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(10) };
    static readonly string AppVersion =
        typeof(HttpTileSource).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "0";
    /// <summary>After "too many requests", "blocked" or a server error, no tile is fetched until this time (all tiles,
    /// not just the one that failed), honouring Retry-After. Shared by every tile source.</summary>
    static long pauseUntil;

    readonly Settings s;
    readonly string dir;
    readonly object memLock = new();
    readonly Dictionary<long, ImageSource> mem = new();
    readonly LinkedList<long> lru = new();
    readonly ConcurrentDictionary<long, byte> inflight = new();
    readonly ConcurrentDictionary<long, long> wantedAt = new();
    readonly ConcurrentDictionary<long, long> failUntil = new();
    readonly SemaphoreSlim net = new(2); // OSM policy: at most 2 connections
    int notifyPending;

    public string Attribution => s.TileAttribution;
    public int MaxZoom => 19;
    public event Action? Changed;

    public HttpTileSource(Settings settings)
    {
        s = settings;
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HamLink", "tiles");
        dir = Path.Combine(root, Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(s.TileUrl)))[..12]);
        Directory.CreateDirectory(dir);
        _ = Task.Run(Prune);
    }

    static long Key(int z, int x, int y) => ((long)z << 56) | ((long)x << 28) | (uint)y;

    public ImageSource? Tile(int z, int x, int y)
    {
        int n = 1 << z;
        if (z < 0 || z > MaxZoom || y < 0 || y >= n) return null;
        int xw = ((x % n) + n) % n;
        long k = Key(z, xw, y);
        wantedAt[k] = Environment.TickCount64;
        lock (memLock)
        {
            if (mem.TryGetValue(k, out var img))
            {
                lru.Remove(k); lru.AddFirst(k);
                return img;
            }
        }
        if (inflight.TryAdd(k, 0)) _ = Task.Run(() => Load(z, xw, y, k));
        return null;
    }

    string FilePath(int z, int x, int y) => Path.Combine(dir, z.ToString(), x.ToString(), y + ".tile");

    async Task Load(int z, int x, int y, long k)
    {
        try
        {
            string path = FilePath(z, x, y);
            ImageSource? img = null;
            var (etag, expires) = ReadMeta(path);
            if (File.Exists(path))
            {
                img = Decode(await File.ReadAllBytesAsync(path));
                try { File.SetLastWriteTimeUtc(path, DateTime.UtcNow); } catch { } // LRU for pruning
            }
            bool fresh = img != null && expires > DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            if (!fresh && s.TileOnline && StillWanted(k) && Environment.TickCount64 >= failUntil.GetValueOrDefault(k) &&
                Environment.TickCount64 >= Interlocked.Read(ref pauseUntil))
            {
                var got = await Fetch(z, x, y, path, etag, img != null);
                if (got == null) failUntil[k] = Environment.TickCount64 + 60_000;
                else img = got;
            }
            if (img != null)
            {
                lock (memLock)
                {
                    mem[k] = img; lru.Remove(k); lru.AddFirst(k);
                    while (lru.Count > 128) { mem.Remove(lru.Last!.Value); lru.RemoveLast(); }
                }
                Notify();
            }
        }
        catch
        {
            // a bad tile or a dropped connection must never crash the radio app
        }
        finally
        {
            inflight.TryRemove(k, out _);
        }
    }

    /// <summary>A tile queued while the user kept panning is dropped if the map has not asked for it for 15 s.</summary>
    bool StillWanted(long k) => Environment.TickCount64 - wantedAt.GetValueOrDefault(k) < 15_000;

    void Notify()
    {
        if (Interlocked.Exchange(ref notifyPending, 1) == 1) return; // coalesce a burst of arrivals into one redraw
        System.Windows.Application.Current?.Dispatcher.BeginInvoke(() =>
        {
            Interlocked.Exchange(ref notifyPending, 0);
            Changed?.Invoke();
        }, System.Windows.Threading.DispatcherPriority.Background);
    }

    async Task<ImageSource?> Fetch(int z, int x, int y, string path, string? etag, bool haveFile)
    {
        string url = s.TileUrl.Replace("{z}", z.ToString()).Replace("{x}", x.ToString()).Replace("{y}", y.ToString());
        if (TileUrls.Problem(s.TileUrl) != null) return null;
        await net.WaitAsync();
        try
        {
            if (Environment.TickCount64 < Interlocked.Read(ref pauseUntil)) return null; // paused while we waited our turn
            using var req = new HttpRequestMessage(HttpMethod.Get, url);
            req.Headers.TryAddWithoutValidation("User-Agent", UserAgent(s));
            if (haveFile && etag != null) req.Headers.TryAddWithoutValidation("If-None-Match", etag);
            using var resp = await Http.SendAsync(req);
            long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            long ttl = CacheMs(resp, now);
            int code = (int)resp.StatusCode;
            if (code == 304)
            {
                WriteMeta(path, etag, now + ttl);
                return Decode(await File.ReadAllBytesAsync(path));
            }
            if (code == 429 || code == 403 || code >= 500)
            {
                // The server asks us to slow down (429), refuses us (403) or is in trouble: pause every tile download.
                TimeSpan wait = resp.Headers.RetryAfter?.Delta
                    ?? (resp.Headers.RetryAfter?.Date is { } d ? d - DateTimeOffset.UtcNow : (TimeSpan?)null)
                    ?? TimeSpan.FromMinutes(code == 403 ? 30 : 2);
                long until = Environment.TickCount64 + (long)Math.Clamp(wait.TotalMilliseconds, 30_000, 3_600_000);
                Interlocked.Exchange(ref pauseUntil, until);
                return null;
            }
            if (!resp.IsSuccessStatusCode) return null;
            byte[] bytes = await resp.Content.ReadAsByteArrayAsync();
            var img = Decode(bytes);
            if (img == null) return null;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            string tmp = path + ".part";
            await File.WriteAllBytesAsync(tmp, bytes);
            File.Move(tmp, path, overwrite: true);
            WriteMeta(path, resp.Headers.ETag?.Tag, now + ttl);
            return img;
        }
        finally
        {
            net.Release();
        }
    }

    /// <summary>Identifies the app, its version and who runs it, as the OSM tile policy asks; never a browser's agent.</summary>
    internal static string UserAgent(Settings s)
    {
        string contact = string.IsNullOrWhiteSpace(s.TileContact) ? s.Callsign : s.TileContact;
        return $"HamLink/{AppVersion} (amateur radio messaging app; +https://github.com/MRC757/hamlink; contact: {contact})";
    }

    /// <summary>How long a tile stays fresh: Cache-Control max-age, else the Expires header, else 7 days; at least 1 hour.</summary>
    static long CacheMs(HttpResponseMessage resp, long nowMs)
    {
        TimeSpan? age = resp.Headers.CacheControl?.MaxAge;
        if (age == null && resp.Content.Headers.Expires is { } exp) age = exp - DateTimeOffset.FromUnixTimeMilliseconds(nowMs);
        return (long)Math.Max((age ?? TimeSpan.FromDays(7)).TotalSeconds, 3600) * 1000;
    }

    internal static ImageSource? Decode(byte[] bytes)
    {
        try
        {
            var bi = new BitmapImage();
            using var ms = new MemoryStream(bytes);
            bi.BeginInit();
            bi.CacheOption = BitmapCacheOption.OnLoad;
            bi.StreamSource = ms;
            bi.EndInit();
            bi.Freeze(); // frozen images may be created on a worker thread and drawn on the UI thread
            return bi;
        }
        catch { return null; }
    }

    static (string? etag, long expires) ReadMeta(string path)
    {
        try
        {
            string m = path + ".meta";
            if (!File.Exists(m)) return (null, 0);
            var lines = File.ReadAllLines(m);
            return (lines.Length > 0 && lines[0].Length > 0 ? lines[0] : null, lines.Length > 1 && long.TryParse(lines[1], out var e) ? e : 0);
        }
        catch { return (null, 0); }
    }

    static void WriteMeta(string path, string? etag, long expires)
    {
        try { File.WriteAllText(path + ".meta", (etag ?? "") + "\n" + expires + "\n"); } catch { }
    }

    /// <summary>Keep the cache under the configured size by deleting the least recently used tiles.</summary>
    void Prune()
    {
        try
        {
            long limit = s.TileCacheMb * 1024L * 1024L;
            var files = new DirectoryInfo(dir).EnumerateFiles("*.tile", SearchOption.AllDirectories).ToList();
            long total = files.Sum(f => f.Length);
            if (total <= limit) return;
            foreach (var f in files.OrderBy(f => f.LastWriteTimeUtc))
            {
                if (total <= limit * 8 / 10) break;
                total -= f.Length;
                f.Delete();
                try { File.Delete(f.FullName + ".meta"); } catch { }
            }
        }
        catch { }
    }
}
