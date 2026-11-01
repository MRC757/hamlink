// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.Concurrent;
using System.IO.Ports;

namespace HamLink;

/// <summary>A position and where it came from. Source is the exact name of a choice in schemas/position.json.</summary>
internal sealed record Fix(double Lat, double Lon, double RadiusM, string Source);

/// <summary>
/// Chooses the active position source from Settings and publishes the current fix. Sources: typed lat/lon,
/// Maidenhead grid square, Windows location service, or a serial NMEA GPS. To add another, add a case in Apply.
/// </summary>
internal static class Positions
{
    static readonly object gate = new();
    static IDisposable? provider;

    public static Fix? Current { get; private set; }
    public static string Status { get; private set; } = "No position yet";
    public static event Action? Changed;

    /// <summary>Raw bytes from a serial GPS; the engine thread feeds them to the core's NMEA parser.</summary>
    public static readonly ConcurrentQueue<byte[]> NmeaBytes = new();

    /// <summary>Parse typed text. Null if unreadable or if it is the wrong kind for the chosen mode.</summary>
    public static Fix? ParseTyped(string text, bool wantGrid)
    {
        string t = text.Trim();
        if (!Geo.Parse(t, out double lat, out double lon, out double radius, out int kind)) return null;
        if (wantGrid != (kind == 1)) return null;
        string source = kind != 1 ? "Typed coordinates" : t.Length >= 8 ? "Grid square (8 char)" : t.Length >= 6 ? "Grid square (6 char)" : "Grid square (4 char)";
        return new Fix(lat, lon, radius, source);
    }

    /// <summary>Call at start, after settings change, and before the radio starts. Safe to repeat.</summary>
    public static void Apply(Settings s)
    {
        lock (gate)
        {
            provider?.Dispose();
            provider = null;
            while (NmeaBytes.TryDequeue(out _)) { }
            switch (s.PositionMode)
            {
                case "windows":
                    Publish(null, "Windows location: starting...", reset: true);
                    provider = new WindowsLocationProvider(f => Publish(f, null), msg => Publish(null, msg));
                    break;
                case "serial":
                    Publish(null, $"Serial GPS {(string.IsNullOrEmpty(s.GpsPort) ? "(no port chosen)" : s.GpsPort)}: waiting for a fix. Start the radio to read it.", reset: true);
                    provider = new SerialGpsProvider(s.GpsPort, s.GpsBaud, msg => Publish(null, msg));
                    break;
                default:
                    bool grid = s.PositionMode == "grid";
                    var f = ParseTyped(s.TypedPosition, grid);
                    Publish(f, f == null ? (grid ? "Grid square: type a locator such as FN31pr" : "Typed position: type coordinates") : null, reset: true);
                    break;
            }
        }
    }

    public static void Stop() { lock (gate) { provider?.Dispose(); provider = null; } }

    /// <summary>
    /// Called by providers (any thread) and by the engine for serial NMEA fixes. A status-only call (f == null) keeps the
    /// last good fix. <paramref name="reset"/> is used only by Apply when the source changes.
    /// </summary>
    public static void Publish(Fix? f, string? status, bool reset = false)
    {
        if (reset) Current = f;
        else if (f != null) Current = f;
        Status = status ?? (Current != null ? Describe(Current) : "No position yet");
        Changed?.Invoke();
    }

    public static string Describe(Fix f)
    {
        string src = f.Source == "GPS" ? "GPS" : f.Source.StartsWith("Grid") ? "Grid square" : "Typed";
        string acc = f.RadiusM >= 1000 ? $" ±{f.RadiusM / 1000:0} km" : f.RadiusM >= 1 ? $" ±{f.RadiusM:0} m" : "";
        return $"{src}: {f.Lat:0.00000}, {f.Lon:0.00000}  {Geo.Format(f.Lat, f.Lon, Geo.Maidenhead)}{acc}";
    }
}

/// <summary>Windows location service (a GPS receiver exposed to Windows, or Wi-Fi/IP positioning).</summary>
internal sealed class WindowsLocationProvider : IDisposable
{
    Windows.Devices.Geolocation.Geolocator? geo;
    bool disposed;

    public WindowsLocationProvider(Action<Fix> onFix, Action<string> onStatus) => _ = StartAsync(onFix, onStatus);

    async Task StartAsync(Action<Fix> onFix, Action<string> onStatus)
    {
        try
        {
            var access = await Windows.Devices.Geolocation.Geolocator.RequestAccessAsync();
            if (disposed) return;
            if (access != Windows.Devices.Geolocation.GeolocationAccessStatus.Allowed)
            {
                onStatus("Windows location: access denied. Allow it in Settings > Privacy & security > Location, including for desktop apps.");
                return;
            }
            var g = new Windows.Devices.Geolocation.Geolocator
            {
                DesiredAccuracy = Windows.Devices.Geolocation.PositionAccuracy.High,
                MovementThreshold = 5,
                ReportInterval = 5000,
            };
            g.PositionChanged += (_, e) =>
            {
                if (disposed) return; // an abandoned provider must not keep overwriting Positions.Current
                var c = e.Position.Coordinate;
                double acc = c.Accuracy;
                // Wi-Fi and IP positioning is only good to hundreds of metres or km: not a GPS fix, so do not send it as one.
                if (acc > 150) onStatus($"Windows location is only approximate (±{acc:0} m). Use a GPS receiver, typed coordinates or a grid square.");
                else onFix(new Fix(c.Point.Position.Latitude, c.Point.Position.Longitude, acc, "GPS"));
            };
            if (disposed) return; // Dispose() ran while we were awaiting RequestAccessAsync
            geo = g;
            onStatus("Windows location: waiting for a fix");
        }
        catch (Exception ex)
        {
            onStatus("Windows location unavailable: " + ex.Message);
        }
    }

    public void Dispose() { disposed = true; geo = null; }
}

/// <summary>NMEA GPS on a COM port. Bytes are queued; the engine thread parses them with the core.</summary>
internal sealed class SerialGpsProvider : IDisposable
{
    SerialPort? port;

    public SerialGpsProvider(string name, int baud, Action<string> onStatus)
    {
        if (string.IsNullOrEmpty(name)) return;
        try
        {
            port = new SerialPort(name, baud) { ReadTimeout = 500 };
            port.DataReceived += (_, _) =>
            {
                try
                {
                    int n = port!.BytesToRead;
                    if (n <= 0) return;
                    var b = new byte[n];
                    int got = port.Read(b, 0, n);
                    if (got > 0) { if (got != n) Array.Resize(ref b, got); Positions.NmeaBytes.Enqueue(b); }
                }
                catch { /* unplugged mid-read: the status line stops updating */ }
            };
            port.Open();
        }
        catch (Exception ex)
        {
            onStatus($"Serial GPS {name}: {ex.Message}");
            port?.Dispose();
            port = null;
        }
    }

    public void Dispose()
    {
        try { port?.Close(); } catch { }
        port?.Dispose();
        port = null;
    }
}
