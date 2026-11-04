// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace HamLink;

/// <summary>The Map view: map, zoom buttons, details of the selected icon, and the position/report buttons.</summary>
internal sealed class MapPanel : Grid
{
    readonly Settings settings;
    readonly Engine engine;
    readonly MapView map = new();
    readonly TextBlock detail = new() { TextWrapping = TextWrapping.Wrap };
    readonly Button openStation = new() { Content = "Messages", Padding = new Thickness(12, 3, 12, 3), Margin = new Thickness(0, 4, 0, 0),
                                          HorizontalAlignment = HorizontalAlignment.Left, Visibility = Visibility.Collapsed };

    /// <summary>"Messages" on a selected station: open its conversation on the Stations page.</summary>
    public event Action<string>? OpenStation;
    readonly TextBlock posText = new() { TextWrapping = TextWrapping.Wrap };
    readonly TextBlock attribution = new() { Background = new SolidColorBrush(Color.FromArgb(204, 255, 255, 255)), Padding = new Thickness(4, 0, 4, 0), FontSize = 10, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom };
    readonly Button sendPos = new() { Content = "Send my position", Padding = new Thickness(14, 5, 14, 5), Margin = new Thickness(0, 0, 8, 0) };
    readonly Button newReport = new() { Content = "New report...", Padding = new Thickness(14, 5, 14, 5), Margin = new Thickness(0, 0, 8, 0) };
    string? selectedKey;
    string tilesFor = "\0"; // the settings the current tile source was built for
    MapPack? pack;
    string packNote = "";

    public MapPanel(Settings settings, Engine engine)
    {
        this.settings = settings;
        this.engine = engine;
        Background = Brushes.White;
        RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        map.ItemSelected += OnSelect;
        map.MapRightClicked += (lat, lon) =>
        {
            if (!engine.IsRunning)
            {
                MessageBox.Show(Window.GetWindow(this), "Press Start first: reports can only be sent while the radio is running.", "HamLink");
                return;
            }
            new ReportWindow(engine, new Fix(lat, lon, 0, "Map")) { Owner = Window.GetWindow(this) }.ShowDialog();
        };

        var mapArea = new Grid();
        mapArea.Children.Add(map);
        var zoom = new StackPanel { HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(8) };
        zoom.Children.Add(MapButton("+", () => map.Zoom(1)));
        zoom.Children.Add(MapButton("−", () => map.Zoom(-1)));
        zoom.Children.Add(MapButton("◎", () => { if (Positions.Current is { } p) map.CenterOn(p.Lat, p.Lon, Math.Max(map.CamZoom, 12)); }));
        mapArea.Children.Add(zoom);
        mapArea.Children.Add(attribution);
        SetRow(mapArea, 0);
        Children.Add(mapArea);

        var bottom = new StackPanel { Margin = new Thickness(0, 8, 0, 0) };
        bottom.Children.Add(detail);
        openStation.Click += (_, _) => { if (selectedKey is { } k && k.StartsWith("S:")) OpenStation?.Invoke(k[2..]); };
        bottom.Children.Add(openStation);
        bottom.Children.Add(posText);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 6, 0, 0) };
        sendPos.Click += (_, _) => engine.SendPosition();
        newReport.Click += (_, _) => new ReportWindow(engine) { Owner = Window.GetWindow(this) }.ShowDialog();
        var setup = new Button { Content = "Map setup...", Padding = new Thickness(14, 5, 14, 5), Margin = new Thickness(0, 0, 8, 0) };
        setup.Click += (_, _) => { if (new MapSetupWindow(settings) { Owner = Window.GetWindow(this) }.ShowDialog() == true) { engine.SetReportRepeat(settings.ReportRepeat); Refresh(); } };
        var clear = new Button { Content = "Clear map", Padding = new Thickness(14, 5, 14, 5) };
        clear.Click += (_, _) => { engine.ClearMap(); selectedKey = null; detail.Text = ""; openStation.Visibility = Visibility.Collapsed; map.SetSelected(null); };
        buttons.Children.Add(sendPos); buttons.Children.Add(newReport); buttons.Children.Add(setup); buttons.Children.Add(clear);
        bottom.Children.Add(buttons);
        bottom.Children.Add(new TextBlock
        {
            Text = "Tip: right-click a point on the map to start a report there.",
            Foreground = Brushes.DimGray,
            FontSize = 11,
            Margin = new Thickness(0, 4, 0, 0),
        });
        SetRow(bottom, 1);
        Children.Add(bottom);

        Positions.Changed += () => Dispatcher.BeginInvoke(() => Refresh());
        engine.MapUpdated += items => Dispatcher.BeginInvoke(() =>
        {
            lastItems = items;
            map.SetItems(items);
            if (selectedKey != null) engine.RequestDetail(selectedKey, d => Dispatcher.BeginInvoke(() => { if (selectedKey != null) detail.Text = d ?? ""; }));
        });
        Refresh();
    }

    IReadOnlyList<MapItem> lastItems = Array.Empty<MapItem>();

    /// <summary>Selects and centres a station (the Stations page's "Show on map").</summary>
    public void ShowStation(string call, IReadOnlyList<MapItem> items)
    {
        if (lastItems.Count == 0 && items.Count > 0) { lastItems = items; map.SetItems(items); } // map just opened
        var it = lastItems.FirstOrDefault(i => i.Type == 'S' && string.Equals(i.Name, call, StringComparison.OrdinalIgnoreCase));
        if (it == null) return;
        map.CenterOn(it.Lat, it.Lon, Math.Max(map.CamZoom, 12));
        map.SetSelected(it.Key);
        OnSelect(it.Key);
    }

    static Button MapButton(string text, Action click)
    {
        var b = new Button { Content = text, Width = 34, Height = 30, Margin = new Thickness(0, 0, 0, 6), FontWeight = FontWeights.Bold };
        b.Click += (_, _) => click();
        return b;
    }

    /// <summary>Re-read settings and position after Map setup, a position change or engine start/stop.</summary>
    public void Refresh()
    {
        RebuildTilesIfChanged();
        attribution.Text = (map.Source?.Attribution ?? settings.TileAttribution) + packNote +
                           (settings.TileOnline ? "" : "   (offline: map pack and saved tiles only)");
        var fix = Positions.Current;
        map.SetMe(fix);
        posText.Text = Positions.Status;
        posText.Foreground = fix == null ? Brushes.DarkOrange : Brushes.SteelBlue;
        const string startFirst = "Press Start first: the radio is not running.";
        Ui.Enable(sendPos, engine.IsRunning && fix != null,
            !engine.IsRunning ? startFirst : "Set your position first: Map setup... > Where am I?");
        Ui.Enable(newReport, engine.IsRunning, startFirst);
        if (fix != null && !map.CamPlaced) map.CenterOn(fix.Lat, fix.Lon, 12);
    }

    /// <summary>Tile source = map pack (if set) in front of the tile server and its cache. Rebuilt when either changes.</summary>
    void RebuildTilesIfChanged()
    {
        string key = settings.TileUrl + "\n" + settings.MapPackPath;
        if (key == tilesFor) return;
        tilesFor = key;
        pack?.Dispose();
        pack = null;
        packNote = "";
        ITileSource src = new HttpTileSource(settings);
        if (settings.MapPackPath.Length > 0)
        {
            try { pack = MapPack.Open(settings.MapPackPath); src = new PackTileSource(pack, src); }
            catch (Exception ex) { packNote = "   (map pack not loaded: " + ex.Message + ")"; App.LogError("map pack", ex); }
        }
        map.Source = src;
    }

    void OnSelect(string? key)
    {
        selectedKey = key;
        map.SetSelected(key);
        detail.Text = "";
        bool station = key != null && key.StartsWith("S:") &&
                       !lastItems.Any(i => i.Key == key && i.Self); // not our own marker
        openStation.Visibility = station ? Visibility.Visible : Visibility.Collapsed;
        if (key != null) engine.RequestDetail(key, d => Dispatcher.BeginInvoke(() => { if (selectedKey == key) detail.Text = d ?? ""; }));
    }
}
