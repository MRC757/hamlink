// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace HamLink;

/// <summary>Web Mercator maths (256 px tiles). Same formulas as core/geo.c geo_to_world / geo_from_world.</summary>
internal static class Mercator
{
    const double MaxLat = 85.0511287798;
    public static double WorldSize(double zoom) => 256.0 * Math.Pow(2.0, zoom);
    public static double X(double lon, double zoom) => (lon + 180.0) / 360.0 * WorldSize(zoom);
    public static double Y(double lat, double zoom)
    {
        double r = Math.Clamp(lat, -MaxLat, MaxLat) * Math.PI / 180.0;
        return (1.0 - Math.Log(Math.Tan(r) + 1.0 / Math.Cos(r)) / Math.PI) / 2.0 * WorldSize(zoom);
    }
    public static double Lon(double x, double zoom) => x / WorldSize(zoom) * 360.0 - 180.0;
    public static double Lat(double y, double zoom) => Math.Atan(Math.Sinh(Math.PI * (1.0 - 2.0 * y / WorldSize(zoom)))) * 180.0 / Math.PI;
    public static double MetersPerPixel(double lat, double zoom) => 40075016.686 * Math.Cos(lat * Math.PI / 180.0) / WorldSize(zoom);
}

/// <summary>
/// Pan/zoom raster map with report icons. Drawing is separate from tile loading (<see cref="ITileSource"/>) and from what
/// the icons mean (<see cref="MapItem"/> comes from the core, colours and shapes from the report schemas).
/// Drag to pan, wheel to zoom, click an icon to select it.
/// </summary>
internal sealed class MapView : FrameworkElement
{
    ITileSource? source;
    IReadOnlyList<MapItem> items = Array.Empty<MapItem>();
    Fix? me;
    string? selected;
    Point dragStart, lastMouse;
    bool dragging, moved;

    public double CamLat { get; set; } = 39.0;
    public double CamLon { get; set; } = -98.0;
    public double CamZoom { get; set; } = 4.0;
    public bool CamPlaced { get; set; }
    public event Action<string?>? ItemSelected;
    /// <summary>Right-click (not a drag): report a location at this lat/lon.</summary>
    public event Action<double, double>? MapRightClicked;

    public MapView() { ClipToBounds = true; Focusable = true; }

    public ITileSource? Source
    {
        get => source;
        set
        {
            if (source != null) source.Changed -= InvalidateVisual;
            source = value;
            if (source != null) source.Changed += InvalidateVisual;
            InvalidateVisual();
        }
    }

    public void SetItems(IReadOnlyList<MapItem> list) { items = list; InvalidateVisual(); }
    public void SetMe(Fix? f) { me = f; InvalidateVisual(); }
    public void SetSelected(string? key) { selected = key; InvalidateVisual(); }
    public void Zoom(double delta) { CamZoom = Math.Clamp(CamZoom + delta, 2, source?.MaxZoom ?? 19); InvalidateVisual(); }
    public void CenterOn(double lat, double lon, double? zoom = null)
    {
        CamLat = lat; CamLon = lon;
        if (zoom != null) CamZoom = zoom.Value;
        CamPlaced = true;
        InvalidateVisual();
    }

    // ------------------------------------------------------------------ input
    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        Focus();
        dragStart = lastMouse = e.GetPosition(this);
        dragging = true; moved = false;
        CaptureMouse();
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (rightDragging)
        {
            var rp = e.GetPosition(this);
            if ((rp - rightDragStart).Length > 4) rightMoved = true;
        }
        if (!dragging) return;
        var p = e.GetPosition(this);
        if ((p - dragStart).Length > 4) moved = true;
        if (moved)
        {
            PanBy(p.X - lastMouse.X, p.Y - lastMouse.Y);
            lastMouse = p;
        }
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        if (!dragging) return;
        dragging = false;
        ReleaseMouseCapture();
        if (!moved) ItemSelected?.Invoke(HitTest(e.GetPosition(this)));
    }

    bool rightDragging, rightMoved;
    Point rightDragStart;

    protected override void OnMouseRightButtonDown(MouseButtonEventArgs e)
    {
        rightDragStart = e.GetPosition(this);
        rightDragging = true; rightMoved = false;
        CaptureMouse();
    }

    protected override void OnMouseRightButtonUp(MouseButtonEventArgs e)
    {
        if (!rightDragging) return;
        rightDragging = false;
        ReleaseMouseCapture();
        if (!rightMoved)
        {
            var (lat, lon) = LatLonAt(e.GetPosition(this));
            MapRightClicked?.Invoke(lat, lon);
        }
        e.Handled = true;
    }

    /// <summary>Inverse of <see cref="ScreenOf"/>: the lat/lon under a point in this control.</summary>
    public (double Lat, double Lon) LatLonAt(Point p)
    {
        double z = CamZoom;
        double wx = Mercator.X(CamLon, z) + (p.X - ActualWidth / 2);
        double wy = Mercator.Y(CamLat, z) + (p.Y - ActualHeight / 2);
        return (Mercator.Lat(Math.Clamp(wy, 0, Mercator.WorldSize(z)), z), Mercator.Lon(wx, z));
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        double z0 = CamZoom;
        double z1 = Math.Clamp(z0 + e.Delta / 120.0 * 0.5, 2, source?.MaxZoom ?? 19);
        if (z1 == z0) return;
        var p = e.GetPosition(this);
        double s = Math.Pow(2, z1 - z0);
        double cx = Mercator.X(CamLon, z0), cy = Mercator.Y(CamLat, z0);
        double px = cx + (p.X - ActualWidth / 2), py = cy + (p.Y - ActualHeight / 2); // world point under the cursor stays put
        double nx = px * s - (p.X - ActualWidth / 2), ny = py * s - (p.Y - ActualHeight / 2);
        SetCenterWorld(nx, ny, z1);
        e.Handled = true;
    }

    void PanBy(double dx, double dy)
    {
        double z = CamZoom;
        SetCenterWorld(Mercator.X(CamLon, z) - dx, Mercator.Y(CamLat, z) - dy, z);
    }

    void SetCenterWorld(double x, double y, double zoom)
    {
        CamZoom = zoom;
        CamLat = Mercator.Lat(Math.Clamp(y, 0, Mercator.WorldSize(zoom)), zoom);
        double lon = Mercator.Lon(x, zoom);
        if (lon > 180) lon -= 360;
        if (lon < -180) lon += 360;
        CamLon = lon;
        CamPlaced = true;
        InvalidateVisual();
    }

    Point ScreenOf(double lat, double lon)
    {
        double z = CamZoom;
        return new Point(Mercator.X(lon, z) - Mercator.X(CamLon, z) + ActualWidth / 2, Mercator.Y(lat, z) - Mercator.Y(CamLat, z) + ActualHeight / 2);
    }

    string? HitTest(Point tap)
    {
        string? best = null;
        double bestD = 30;
        foreach (var it in items)
        {
            double d = (ScreenOf(it.Lat, it.Lon) - tap).Length;
            if (d < bestD) { bestD = d; best = it.Key; }
        }
        return best;
    }

    // ------------------------------------------------------------------ drawing
    static Color ParseColor(string hex)
    {
        try { return (Color)ColorConverter.ConvertFromString(hex); } catch (FormatException) { return Colors.Gray; }
    }

    protected override void OnRender(DrawingContext dc)
    {
        double w = ActualWidth, h = ActualHeight;
        dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(0xE8, 0xE4, 0xDC)), null, new Rect(0, 0, w, h));
        if (w < 2 || h < 2) return;
        DrawTiles(dc, w, h);

        bool hasSelf = items.Any(i => i.Self);
        // stations first, then contacts; within each, higher severity later so it is drawn on top; selected last
        foreach (var it in items.OrderBy(i => i.Key == selected).ThenBy(i => i.Type != 'S').ThenBy(i => i.Severity))
            DrawItem(dc, it, ScreenOf(it.Lat, it.Lon), it.Key == selected);
        if (me != null && !hasSelf)
        {
            var p = ScreenOf(me.Lat, me.Lon);
            dc.DrawEllipse(Brushes.White, null, p, 9, 9);
            dc.DrawEllipse(new SolidColorBrush(Color.FromRgb(0x15, 0x65, 0xC0)), null, p, 6, 6);
        }
    }

    void DrawTiles(DrawingContext dc, double w, double h)
    {
        if (source == null) return;
        int z = Math.Min((int)Math.Floor(CamZoom), source.MaxZoom);
        double scale = Math.Pow(2, CamZoom - z), tilePx = 256 * scale;
        double cw = Mercator.X(CamLon, z), ch = Mercator.Y(CamLat, z);
        double left = cw - w / (2 * scale), top = ch - h / (2 * scale), right = cw + w / (2 * scale), bottom = ch + h / (2 * scale);
        int n = 1 << z;
        var placeholder = new SolidColorBrush(Color.FromRgb(0xDA, 0xD6, 0xCC));
        for (int ty = Math.Max(0, (int)Math.Floor(top / 256)); ty <= Math.Min(n - 1, (int)Math.Floor(bottom / 256)); ty++)
            for (int tx = (int)Math.Floor(left / 256); tx <= (int)Math.Floor(right / 256); tx++)
            {
                double sx = Math.Round((tx * 256 - left) * scale), sy = Math.Round((ty * 256 - top) * scale);
                var img = source.Tile(z, tx, ty);
                var rect = new Rect(sx, sy, tilePx + 1, tilePx + 1);
                if (img != null) dc.DrawImage(img, rect);
                else dc.DrawRectangle(placeholder, null, new Rect(sx, sy, tilePx - 1, tilePx - 1));
            }
    }

    void DrawItem(DrawingContext dc, MapItem it, Point p, bool isSelected)
    {
        var c = ParseColor(it.Color);
        double r = it.Type == 'S' ? 9 : 11;
        dc.PushOpacity(Math.Clamp(it.Alpha, 0.1, 1.0));
        if (it.RadiusM > 0)
        {
            double ring = it.RadiusM / Mercator.MetersPerPixel(it.Lat, CamZoom);
            if (ring > r)
            {
                dc.DrawEllipse(new SolidColorBrush(Color.FromArgb(26, c.R, c.G, c.B)), new Pen(new SolidColorBrush(Color.FromArgb(128, c.R, c.G, c.B)), 1.5), p, ring, ring);
            }
        }
        if (isSelected) dc.DrawEllipse(new SolidColorBrush(Color.FromArgb(217, 255, 255, 255)), null, p, r * 1.9, r * 1.9);
        Geometry shape = it.Shape switch
        {
            "diamond" => Diamond(p, r * 1.25),
            "square" => new RectangleGeometry(new Rect(p.X - r, p.Y - r, 2 * r, 2 * r)),
            "quatrefoil" => Quatrefoil(p, r),
            _ => new RectangleGeometry(new Rect(p.X - r * 1.5, p.Y - r * 0.9, r * 3, r * 1.8)), // rectangle
        };
        dc.DrawGeometry(new SolidColorBrush(c), new Pen(Brushes.Black, it.Self ? 3 : 1.5), shape);
        if (it.Self) dc.DrawGeometry(null, new Pen(Brushes.White, 1.2), shape);
        dc.Pop();

        if (CamZoom >= 9.5 || isSelected)
        {
            string text = it.Type == 'S' ? it.Name : it.Label;
            var ft = new FormattedText(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, new Typeface("Segoe UI"), 11,
                Brushes.Black, VisualTreeHelper.GetDpi(this).PixelsPerDip);
            var origin = new Point(p.X - ft.Width / 2, p.Y + r + 3);
            var outline = ft.BuildGeometry(origin);
            dc.DrawGeometry(null, new Pen(Brushes.White, 3), outline);
            dc.DrawGeometry(Brushes.Black, null, outline);
        }
    }

    static Geometry Diamond(Point p, double r)
    {
        var g = new StreamGeometry();
        using (var ctx = g.Open())
        {
            ctx.BeginFigure(new Point(p.X, p.Y - r), true, true);
            ctx.LineTo(new Point(p.X + r, p.Y), true, false);
            ctx.LineTo(new Point(p.X, p.Y + r), true, false);
            ctx.LineTo(new Point(p.X - r, p.Y), true, false);
        }
        g.Freeze();
        return g;
    }

    static Geometry Quatrefoil(Point p, double r)
    {
        double c = r * 0.62;
        var group = new GeometryGroup { FillRule = FillRule.Nonzero };
        foreach (var (dx, dy) in new[] { (-1.0, 0.0), (1.0, 0.0), (0.0, -1.0), (0.0, 1.0) })
            group.Children.Add(new EllipseGeometry(new Point(p.X + dx * c, p.Y + dy * c), c, c));
        return group;
    }
}
