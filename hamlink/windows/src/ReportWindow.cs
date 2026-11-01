// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace HamLink;

// ---------------------------------------------------------------------------------------------------------------
// Report types are described by the JSON files in the schemas directory, compiled into the core. This file turns that
// description into a form, so adding a report type needs no change here.
// ---------------------------------------------------------------------------------------------------------------

internal sealed record ValueSpec(int Code, string Name, string? Color);
internal sealed record FieldSpec(string Key, string Label, string Kind, bool Optional, int Max, IReadOnlyList<ValueSpec> Values);
internal sealed record SchemaSpec(string Name, string Title, string Role, IReadOnlyList<FieldSpec> Fields);

internal static class Schemas
{
    static List<SchemaSpec>? all;

    /// <summary>All report types the core knows (stateless native calls: safe from the UI thread).</summary>
    public static IReadOnlyList<SchemaSpec> All => all ??= Load();

    /// <summary>Types that appear on the map as situation reports (the position report has its own button).</summary>
    public static IEnumerable<SchemaSpec> Forms => All.Where(s => s.Role == "contact");

    static List<SchemaSpec> Load()
    {
        var list = new List<SchemaSpec>();
        for (int i = 0; i < NativeMethods.hl_schema_count(); i++)
        {
            string? json = Radio.SchemaJson(i);
            if (json == null) continue;
            using var doc = JsonDocument.Parse(json);
            var o = doc.RootElement;
            var fields = new List<FieldSpec>();
            foreach (var f in o.GetProperty("fields").EnumerateArray())
            {
                string kind = f.GetProperty("kind").GetString()!;
                var vals = new List<ValueSpec>();
                if (f.TryGetProperty("values", out var va))
                    foreach (var v in va.EnumerateArray())
                        vals.Add(new ValueSpec(v.GetProperty("code").GetInt32(), v.GetProperty("name").GetString()!,
                            v.TryGetProperty("color", out var c) ? c.GetString() : null));
                int max = kind switch
                {
                    "uint" => f.TryGetProperty("max", out var m) ? m.GetInt32() : (1 << f.GetProperty("bits").GetInt32()) - 1,
                    "text" => f.GetProperty("max").GetInt32(),
                    _ => 0,
                };
                fields.Add(new FieldSpec(f.GetProperty("key").GetString()!, f.GetProperty("label").GetString()!, kind,
                    f.TryGetProperty("optional", out var op) && op.GetBoolean(), max, vals));
            }
            string role = o.TryGetProperty("map", out var map) && map.TryGetProperty("role", out var r) ? r.GetString() ?? "none" : "none";
            list.Add(new SchemaSpec(o.GetProperty("name").GetString()!, o.GetProperty("title").GetString()!, role, fields));
        }
        return list;
    }
}

/// <summary>Dialog that builds and sends one report of any schema in <see cref="Schemas.Forms"/>.</summary>
internal sealed class ReportWindow : Window
{
    static readonly (int Min, string Text)[] Ago = { (0, "Just now"), (5, "5 min ago"), (15, "15 min ago"), (30, "30 min ago"), (60, "1 h ago"), (120, "2 h ago") };

    readonly Engine engine;
    readonly List<SchemaSpec> forms = Schemas.Forms.ToList();
    readonly StackPanel body = new();
    readonly TextBlock error = new() { Foreground = Brushes.Firebrick, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) };
    readonly Button send = new() { Content = "Send", Padding = new Thickness(18, 5, 18, 5), IsDefault = true };
    readonly Dictionary<string, Func<string>> readers = new(); // field key -> current value as key=value text ("" = unset)
    readonly Fix? at; // location the report is seeded with (e.g. a map right-click), if any
    SchemaSpec schema;

    /// <summary>Opens the report form. If <paramref name="at"/> is given (e.g. a map right-click), latlon fields
    /// start filled with that location instead of the operator's own position.</summary>
    public ReportWindow(Engine engine, Fix? at = null)
    {
        this.engine = engine;
        this.at = at;
        schema = forms[0];
        Title = "New report";
        Width = 520;
        SizeToContent = SizeToContent.Height;
        MaxHeight = 760;
        ResizeMode = ResizeMode.NoResize;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var cancel = new Button { Content = "Cancel", Padding = new Thickness(14, 5, 14, 5), IsCancel = true, Margin = new Thickness(0, 0, 8, 0) };
        send.Click += OnSend;
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 10, 0, 0) };
        buttons.Children.Add(cancel);
        buttons.Children.Add(send);

        var root = new StackPanel { Margin = new Thickness(14) };
        if (forms.Count > 1)
        {
            var pick = new ComboBox { Margin = new Thickness(0, 0, 0, 8) };
            foreach (var f in forms) pick.Items.Add(f.Title);
            pick.SelectedIndex = 0;
            pick.SelectionChanged += (_, _) => { schema = forms[Math.Max(0, pick.SelectedIndex)]; Build(); };
            root.Children.Add(pick);
        }
        root.Children.Add(new ScrollViewer { Content = body, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MaxHeight = 560 });
        root.Children.Add(error);
        root.Children.Add(new TextBlock { Text = "Sent to everyone in range (CQ). Callsign and time are added automatically.", Foreground = Brushes.DimGray, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0) });
        root.Children.Add(buttons);
        Content = root;
        Build();
    }

    static Brush? BrushOf(string? hex)
    {
        try { return hex == null ? null : new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex)); } catch (FormatException) { return null; }
    }

    static TextBlock Caption(string text) => new() { Text = text, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 8, 0, 2) };

    void Build()
    {
        body.Children.Clear();
        readers.Clear();
        error.Text = "";
        foreach (var f in schema.Fields)
        {
            string label = f.Label + (f.Optional ? " (optional)" : "");
            switch (f.Kind)
            {
                case "enum":
                {
                    body.Children.Add(Caption(label));
                    var combo = new ComboBox();
                    if (f.Optional) combo.Items.Add("(not set)");
                    foreach (var v in f.Values)
                        combo.Items.Add(new ComboBoxItem { Content = v.Name, Background = BrushOf(v.Color) ?? Brushes.Transparent, Tag = v.Name });
                    combo.SelectedIndex = f.Optional ? 0 : -1;
                    body.Children.Add(combo);
                    readers[f.Key] = () => combo.SelectedItem is ComboBoxItem ci ? (string)ci.Tag : "";
                    break;
                }
                case "flags":
                {
                    body.Children.Add(Caption(label));
                    var wrap = new WrapPanel();
                    var boxes = f.Values.Select(v => new CheckBox { Content = v.Name, Margin = new Thickness(0, 0, 14, 4) }).ToList();
                    foreach (var b in boxes) wrap.Children.Add(b);
                    body.Children.Add(wrap);
                    readers[f.Key] = () => string.Join(",", boxes.Where(b => b.IsChecked == true).Select(b => (string)b.Content));
                    break;
                }
                case "uint":
                {
                    body.Children.Add(Caption($"{label}, 0 to {f.Max}"));
                    var tb = new TextBox { MaxLength = 5 };
                    tb.PreviewTextInput += (_, e) => e.Handled = !e.Text.All(char.IsDigit);
                    body.Children.Add(tb);
                    readers[f.Key] = () => tb.Text.Trim();
                    break;
                }
                case "text":
                {
                    body.Children.Add(Caption($"{label}, up to {f.Max} characters"));
                    var tb = new TextBox { MaxLength = f.Max, CharacterCasing = CharacterCasing.Upper };
                    body.Children.Add(tb);
                    readers[f.Key] = () => tb.Text.Trim();
                    break;
                }
                case "latlon":
                {
                    body.Children.Add(Caption(label));
                    var tb = new TextBox();
                    var hint = new TextBlock { Foreground = Brushes.DimGray, TextWrapping = TextWrapping.Wrap };
                    var mine = new Button { Content = "Use my position", Padding = new Thickness(10, 2, 10, 2), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 4, 0, 0) };
                    var fix = at ?? Positions.Current;
                    if (fix != null) tb.Text = $"{fix.Lat:0.00000},{fix.Lon:0.00000}";
                    mine.IsEnabled = Positions.Current != null;
                    mine.Click += (_, _) => { if (Positions.Current is { } p) tb.Text = $"{p.Lat:0.00000},{p.Lon:0.00000}"; };
                    void Preview()
                    {
                        if (string.IsNullOrWhiteSpace(tb.Text)) { hint.Text = "lat,lon   or  FN31pr   or  MGRS"; hint.Foreground = Brushes.DimGray; return; }
                        if (Geo.Parse(tb.Text, out double la, out double lo, out _, out _))
                        { hint.Text = $"= {la:0.00000}, {lo:0.00000}  {Geo.Format(la, lo, Geo.Maidenhead)}"; hint.Foreground = Brushes.DimGray; }
                        else { hint.Text = "Cannot read this position"; hint.Foreground = Brushes.Firebrick; }
                    }
                    tb.TextChanged += (_, _) => Preview();
                    Preview();
                    body.Children.Add(tb); body.Children.Add(hint); body.Children.Add(mine);
                    readers[f.Key] = () => tb.Text.Trim();
                    break;
                }
                case "time":
                {
                    body.Children.Add(Caption(label));
                    var combo = new ComboBox();
                    foreach (var a in Ago) combo.Items.Add(a.Text);
                    combo.SelectedIndex = 0;
                    body.Children.Add(combo);
                    readers[f.Key] = () => $"@{Ago[Math.Max(0, combo.SelectedIndex)].Min}"; // resolved to unix seconds when sending
                    break;
                }
            }
        }
    }

    void OnSend(object sender, RoutedEventArgs e)
    {
        long nowS = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        var kv = new StringBuilder();
        foreach (var f in schema.Fields)
        {
            string v = readers[f.Key]();
            if (f.Kind == "time") kv.Append($"{f.Key}={nowS - long.Parse(v[1..]) * 60}\n");
            else if (v.Length > 0) kv.Append($"{f.Key}={v}\n");
            else if (f.Kind == "flags" && !f.Optional) kv.Append($"{f.Key}=none\n");
        }
        send.IsEnabled = false;
        engine.SendReport(schema.Name, kv.ToString(), "CQ", err => Dispatcher.BeginInvoke(() =>
        {
            send.IsEnabled = true;
            if (err == null) { DialogResult = true; Close(); }
            else error.Text = err;
        }));
    }
}
