// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Ports;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace HamLink;

/// <summary>Position source, beacon and map-tile settings. Returns true from ShowDialog() when saved.</summary>
internal sealed class MapSetupWindow : Window
{
    static readonly (string Value, string Text)[] Modes =
    {
        ("typed", "Typed lat/lon (decimal, DMS or MGRS)"),
        ("grid", "Maidenhead grid square"),
        ("windows", "Windows location service (needs a GPS receiver for real accuracy)"),
        ("serial", "Serial NMEA GPS on a COM port"),
    };
    static readonly (int Min, string Text)[] Beacons = { (0, "Off"), (5, "Every 5 min"), (10, "Every 10 min"), (15, "Every 15 min"), (30, "Every 30 min") };

    readonly Settings s;
    readonly ComboBox mode = new();
    readonly TextBox typed = new();
    readonly TextBlock preview = new() { TextWrapping = TextWrapping.Wrap, Foreground = Brushes.DimGray };
    readonly ComboBox gpsPort = new() { IsEditable = true };
    readonly ComboBox gpsBaud = new();
    readonly ComboBox beacon = new();
    readonly ComboBox repeat = new();
    readonly CheckBox online = new() { Content = "Download map tiles when needed (needs internet)" };
    readonly TextBox url = new() { TextWrapping = TextWrapping.Wrap, AcceptsReturn = false };
    readonly TextBox contact = new();
    readonly TextBox attribution = new();
    readonly StackPanel typedBox = new(), serialBox = new();

    public MapSetupWindow(Settings settings)
    {
        s = settings;
        Title = "Position and map";
        Width = 560;
        SizeToContent = SizeToContent.Height;
        ResizeMode = ResizeMode.NoResize;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        foreach (var m in Modes) mode.Items.Add(m.Text);
        mode.SelectedIndex = Math.Max(0, Array.FindIndex(Modes, m => m.Value == s.PositionMode));
        typed.Text = s.TypedPosition;
        foreach (var p in SerialPort.GetPortNames()) gpsPort.Items.Add(p);
        gpsPort.Text = s.GpsPort;
        foreach (var b in new[] { 4800, 9600, 19200, 38400, 115200 }) gpsBaud.Items.Add(b.ToString());
        gpsBaud.SelectedItem = s.GpsBaud.ToString();
        foreach (var b in Beacons) beacon.Items.Add(b.Text);
        beacon.SelectedIndex = Math.Max(0, Array.FindIndex(Beacons, b => b.Min == s.BeaconMin));
        foreach (var t in new[] { "Off", "1 extra copy", "2 extra copies" }) repeat.Items.Add(t);
        repeat.SelectedIndex = Math.Clamp(s.ReportRepeat, 0, 2);
        online.IsChecked = s.TileOnline;
        url.Text = s.TileUrl;
        contact.Text = s.TileContact;
        attribution.Text = s.TileAttribution;

        var root = new StackPanel { Margin = new Thickness(14) };
        root.Children.Add(Caption("Where am I? (used for the map and for \"Send my position\")"));
        root.Children.Add(mode);
        typedBox.Children.Add(Caption("Position"));
        typedBox.Children.Add(typed);
        typedBox.Children.Add(preview);
        root.Children.Add(typedBox);
        serialBox.Children.Add(Caption("GPS port and speed (the radio must be running to read it)"));
        serialBox.Children.Add(gpsPort);
        serialBox.Children.Add(gpsBaud);
        root.Children.Add(serialBox);

        root.Children.Add(Caption("Automatic position beacon"));
        root.Children.Add(beacon);
        root.Children.Add(Note("Every beacon is a radio transmission on a shared simplex channel. Off by default; keep it infrequent."));

        root.Children.Add(Caption("Repeat my reports"));
        root.Children.Add(repeat);
        root.Children.Add(Note("Position and SALUTE reports sent to everyone are not acknowledged, so one lost transmission loses the report. " +
            "Extra copies go out about 30 s apart; other stations show the report once. Each copy is a transmission."));

        root.Children.Add(Caption("Map data"));
        root.Children.Add(online);
        root.Children.Add(Note("Tiles you have viewed are saved on this PC and work offline later. Pan and zoom over your operating area while you " +
            "still have internet. OpenStreetMap's server does not allow bulk downloading. For an air-gapped PC, view the area on another " +
            "PC and copy the folder %LOCALAPPDATA%\\HamLink\\tiles across."));
        root.Children.Add(Caption("Tile server URL"));
        root.Children.Add(url);
        root.Children.Add(Caption("Contact for tile server (email), optional"));
        root.Children.Add(contact);
        root.Children.Add(Caption("Attribution shown on the map"));
        root.Children.Add(attribution);

        var cancel = new Button { Content = "Cancel", Padding = new Thickness(14, 5, 14, 5), IsCancel = true, Margin = new Thickness(0, 0, 8, 0) };
        var save = new Button { Content = "Save", Padding = new Thickness(18, 5, 18, 5), IsDefault = true };
        save.Click += (_, _) => Save();
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 12, 0, 0) };
        buttons.Children.Add(cancel); buttons.Children.Add(save);
        root.Children.Add(buttons);
        Content = new ScrollViewer { Content = root, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        MaxHeight = 800;

        mode.SelectionChanged += (_, _) => { UpdateSections(); UpdatePreview(); };
        typed.TextChanged += (_, _) => UpdatePreview();
        UpdateSections();
        UpdatePreview();
    }

    static TextBlock Caption(string t) => new() { Text = t, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 10, 0, 2), TextWrapping = TextWrapping.Wrap };
    static TextBlock Note(string t) => new() { Text = t, Foreground = Brushes.DimGray, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 0) };

    string CurrentMode => Modes[Math.Max(0, mode.SelectedIndex)].Value;

    void UpdateSections()
    {
        typedBox.Visibility = CurrentMode is "typed" or "grid" ? Visibility.Visible : Visibility.Collapsed;
        serialBox.Visibility = CurrentMode == "serial" ? Visibility.Visible : Visibility.Collapsed;
    }

    void UpdatePreview()
    {
        if (CurrentMode is not ("typed" or "grid")) return;
        bool grid = CurrentMode == "grid";
        if (string.IsNullOrWhiteSpace(typed.Text))
        {
            preview.Text = grid ? "A locator such as FN31pr" : "38.8895, -77.0353   or   38d53m22sN 77d02m07sW   or an MGRS reference";
            preview.Foreground = Brushes.DimGray;
            return;
        }
        var f = Positions.ParseTyped(typed.Text, grid);
        if (f == null) { preview.Text = grid ? "Not a grid square" : "Cannot read this position"; preview.Foreground = Brushes.Firebrick; return; }
        preview.Foreground = Brushes.DimGray;
        preview.Text = $"= {f.Lat:0.00000}, {f.Lon:0.00000}  {Geo.Format(f.Lat, f.Lon, Geo.Maidenhead)}" +
                       (f.RadiusM > 0 ? $"   (accurate to about {f.RadiusM / 1000:0.#} km; receivers draw this as a ring)" : "");
    }

    void Save()
    {
        s.PositionMode = CurrentMode;
        s.TypedPosition = typed.Text.Trim();
        s.GpsPort = gpsPort.Text.Trim();
        s.GpsBaud = int.TryParse(gpsBaud.SelectedItem as string, out int b) ? b : 4800;
        s.BeaconMin = Beacons[Math.Max(0, beacon.SelectedIndex)].Min;
        s.ReportRepeat = Math.Max(0, repeat.SelectedIndex);
        s.TileOnline = online.IsChecked == true;
        s.TileUrl = url.Text.Trim();
        s.TileContact = contact.Text.Trim();
        s.TileAttribution = attribution.Text.Trim();
        s.Save();
        Positions.Apply(s);
        DialogResult = true;
        Close();
    }
}
