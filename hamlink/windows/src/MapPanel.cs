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
    readonly TextBlock posText = new() { TextWrapping = TextWrapping.Wrap };
    readonly TextBlock attribution = new() { Background = new SolidColorBrush(Color.FromArgb(204, 255, 255, 255)), Padding = new Thickness(4, 0, 4, 0), FontSize = 10, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom };
    readonly Button sendPos = new() { Content = "Send my position", Padding = new Thickness(14, 5, 14, 5), Margin = new Thickness(0, 0, 8, 0) };
    readonly Button newReport = new() { Content = "New report...", Padding = new Thickness(14, 5, 14, 5), Margin = new Thickness(0, 0, 8, 0) };
    string? selectedKey;
    HttpTileSource tiles;
    string tilesFor;

    public MapPanel(Settings settings, Engine engine)
    {
        this.settings = settings;
        this.engine = engine;
        Background = Brushes.White;
        RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        tilesFor = settings.TileUrl;
        tiles = new HttpTileSource(settings);
        map.Source = tiles;
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
        bottom.Children.Add(posText);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 6, 0, 0) };
        sendPos.Click += (_, _) => engine.SendPosition();
        newReport.Click += (_, _) => new ReportWindow(engine) { Owner = Window.GetWindow(this) }.ShowDialog();
        var setup = new Button { Content = "Map setup...", Padding = new Thickness(14, 5, 14, 5), Margin = new Thickness(0, 0, 8, 0) };
        setup.Click += (_, _) => { if (new MapSetupWindow(settings) { Owner = Window.GetWindow(this) }.ShowDialog() == true) { engine.SetReportRepeat(settings.ReportRepeat); Refresh(); } };
        var clear = new Button { Content = "Clear map", Padding = new Thickness(14, 5, 14, 5) };
        clear.Click += (_, _) => { engine.ClearMap(); selectedKey = null; detail.Text = ""; map.SetSelected(null); };
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
            map.SetItems(items);
            if (selectedKey != null) engine.RequestDetail(selectedKey, d => Dispatcher.BeginInvoke(() => { if (selectedKey != null) detail.Text = d ?? ""; }));
        });
        Refresh();
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
        if (tilesFor != settings.TileUrl) { tilesFor = settings.TileUrl; tiles = new HttpTileSource(settings); map.Source = tiles; }
        attribution.Text = settings.TileAttribution + (settings.TileOnline ? "" : "   (offline: saved tiles only)");
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

    void OnSelect(string? key)
    {
        selectedKey = key;
        map.SetSelected(key);
        detail.Text = "";
        if (key != null) engine.RequestDetail(key, d => Dispatcher.BeginInvoke(() => { if (selectedKey == key) detail.Text = d ?? ""; }));
    }
}
