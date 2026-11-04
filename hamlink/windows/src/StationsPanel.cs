// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.Specialized;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace HamLink;

/// <summary>
/// The Stations page (docs/STATIONS_PAGE.md): every station heard or messaged on the left, the selected station's
/// conversation on the right. The main window's compose row stays visible under it and is addressed to the selected
/// station. Reads <see cref="MessageStore"/> and the map's station data; UI thread only.
/// </summary>
internal sealed class StationsPanel : Grid
{
    public const string CqEntry = "CQ";

    /// <summary>Callsigns of the stations in the list, most recently heard first (the report form's "To" choices).</summary>
    public static IReadOnlyList<string> HeardCalls { get; private set; } = Array.Empty<string>();

    readonly Settings settings;
    readonly Action<short[]> play;
    readonly TextBox search = new() { Padding = new Thickness(3), Margin = new Thickness(0, 0, 0, 6) };
    readonly CheckBox showHeard = new()
    {
        Content = "Show overheard", IsChecked = true, Margin = new Thickness(0, 0, 0, 6),
        ToolTip = "Monitor mode: messages between other stations",
    };
    readonly ListBox list = new() { HorizontalContentAlignment = HorizontalAlignment.Stretch };
    readonly TextBlock title = new() { FontSize = 18, FontWeight = FontWeights.SemiBold };
    readonly TextBlock subtitle = new() { Foreground = Brushes.Gray, TextWrapping = TextWrapping.Wrap };
    readonly Button showOnMap = new() { Content = "Show on map", Padding = new Thickness(12, 3, 12, 3), Visibility = Visibility.Collapsed };
    readonly StackPanel conversation = new();
    readonly ScrollViewer convScroll;
    readonly Dictionary<string, long> lastRead;
    readonly string readPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HamLink", "stations-read.json");
    IReadOnlyList<MapItem> mapItems = Array.Empty<MapItem>();
    string? selected;
    bool rebuilding;
    static Radio? decoder; // Codec2 for playing stored clips, independent of the running radio

    /// <summary>The selected station changed: the main window addresses the compose row to it ("CQ" for the CQ entry).</summary>
    public event Action<string>? StationSelected;
    /// <summary>"Show on map" for this callsign.</summary>
    public event Action<string>? ShowOnMap;
    /// <summary>The total unread count changed (for the page button).</summary>
    public event Action<int>? UnreadChanged;

    public StationsPanel(Settings settings, Action<short[]> play)
    {
        this.settings = settings;
        this.play = play;
        lastRead = LoadRead();
        Background = Brushes.White;
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(230) });
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var left = new DockPanel { Margin = new Thickness(0, 0, 10, 0) };
        var searchLabel = new TextBlock { Text = "Find a callsign", FontSize = 11, Foreground = Brushes.Gray };
        DockPanel.SetDock(searchLabel, Dock.Top);
        DockPanel.SetDock(search, Dock.Top);
        DockPanel.SetDock(showHeard, Dock.Top);
        left.Children.Add(searchLabel);
        left.Children.Add(search);
        left.Children.Add(showHeard);
        left.Children.Add(list);
        SetColumn(left, 0);
        Children.Add(left);

        var right = new DockPanel();
        var head = new DockPanel { Margin = new Thickness(0, 0, 0, 6) };
        DockPanel.SetDock(showOnMap, Dock.Right);
        showOnMap.VerticalAlignment = VerticalAlignment.Top;
        head.Children.Add(showOnMap);
        var titles = new StackPanel();
        titles.Children.Add(title);
        titles.Children.Add(subtitle);
        head.Children.Add(titles);
        DockPanel.SetDock(head, Dock.Top);
        right.Children.Add(head);
        convScroll = new ScrollViewer { Content = conversation, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
        right.Children.Add(new Border { BorderBrush = Brushes.LightGray, BorderThickness = new Thickness(1), Child = convScroll });
        SetColumn(right, 1);
        Children.Add(right);

        search.TextChanged += (_, _) => RebuildList();
        showHeard.Click += (_, _) => { RebuildList(); if (selected != null) ShowConversation(selected); };
        list.SelectionChanged += (_, _) =>
        {
            if (rebuilding || list.SelectedItem is not ListBoxItem { Tag: string call }) return;
            Select(call);
        };
        showOnMap.Click += (_, _) => { if (selected != null && selected != CqEntry) ShowOnMap?.Invoke(selected); };
        MessageStore.All.CollectionChanged += OnStoreChanged;
        Positions.Changed += () => Dispatcher.BeginInvoke(RebuildList);

        RebuildList();
        Select(CqEntry);
    }

    /// <summary>New station data from the map snapshot (about every 2 s while running).</summary>
    public void SetMapItems(IReadOnlyList<MapItem> items)
    {
        mapItems = items;
        RebuildList();
        if (selected != null) ShowHeader(selected);
    }

    /// <summary>Opens a station's conversation (also used by the map's "Messages" link).</summary>
    public void Select(string call)
    {
        selected = call;
        MarkRead(call);
        ShowHeader(call);
        ShowConversation(call);
        RebuildList();
        StationSelected?.Invoke(call);
    }

    /// <summary>The page became visible: the open conversation counts as read.</summary>
    public void Activated()
    {
        if (selected == null) return;
        MarkRead(selected);
        RebuildList();
        StationSelected?.Invoke(selected);
    }

    public IReadOnlyList<MapItem> MapItems => mapItems;

    public bool IsShown { get; set; }

    // ---------------------------------------------------------------- station list
    sealed record Row(string Call, long LastT, int Unread, string Detail, Brush Dot);

    /// <summary>Overheard traffic is shown while monitor mode is on and "Show overheard" is ticked; station IDs always
    /// count for "last heard".</summary>
    bool Shown(MsgRecord m) => m.Dir != MsgDir.Heard || m.Kind == "id" || (settings.Monitor && showHeard.IsChecked == true);

    List<Row> Rows()
    {
        var byCall = new Dictionary<string, (long last, int unread)>(StringComparer.OrdinalIgnoreCase);
        foreach (var m in MessageStore.All)
        {
            if (!Shown(m)) continue;
            foreach (var call in m.Parties)
            {
                byCall.TryGetValue(call, out var v);
                long read = lastRead.TryGetValue(call, out var rt) ? rt : 0;
                bool unread = m.Dir == MsgDir.In && m.T > read && !(IsShown && call == selected);
                byCall[call] = (Math.Max(v.last, m.T), v.unread + (unread ? 1 : 0));
            }
        }
        long now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var stations = mapItems.Where(i => i.Type == 'S' && !i.Self).ToDictionary(i => i.Name, StringComparer.OrdinalIgnoreCase);
        foreach (var s in stations.Values)
        {
            long heard = now - (long)s.AgeMin * 60000;
            byCall.TryGetValue(s.Name, out var v);
            byCall[s.Name] = (Math.Max(v.last, heard), v.unread);
        }
        var rows = new List<Row>();
        foreach (var (call, v) in byCall)
        {
            if (call == CqEntry) continue;
            stations.TryGetValue(call, out var st);
            rows.Add(new Row(call, v.last, v.unread, Detail(call, v.last, st), Dot(st)));
        }
        rows.Sort((a, b) => b.LastT.CompareTo(a.LastT));
        byCall.TryGetValue(CqEntry, out var cq);
        rows.Insert(0, new Row(CqEntry, cq.last, cq.unread, "All stations: messages you sent to CQ", Brushes.SteelBlue));
        return rows;
    }

    string Detail(string call, long last, MapItem? st)
    {
        var parts = new List<string> { last > 0 ? Ago(last) : "not heard" };
        if (st != null && Positions.Current is { } me)
        {
            double d = Geo.DistanceM(me.Lat, me.Lon, st.Lat, st.Lon);
            parts.Add($"{(d >= 1000 ? $"{d / 1000:0.0} km" : $"{d:0} m")} {Compass(Geo.BearingDeg(me.Lat, me.Lon, st.Lat, st.Lon))}");
        }
        return string.Join(" · ", parts);
    }

    static Brush Dot(MapItem? st)
    {
        if (st == null) return Brushes.LightGray;
        try { return (Brush)new BrushConverter().ConvertFromString(st.Color)!; }
        catch (FormatException) { return Brushes.LightGray; }
    }

    void RebuildList()
    {
        rebuilding = true;
        try
        {
            string q = search.Text.Trim().ToUpperInvariant();
            var rows = Rows();
            HeardCalls = rows.Where(r => r.Call != CqEntry).Select(r => r.Call).ToList();
            UnreadChanged?.Invoke(rows.Sum(r => r.Unread));
            showHeard.Visibility = settings.Monitor ? Visibility.Visible : Visibility.Collapsed;
            list.Items.Clear();
            foreach (var r in rows)
            {
                if (q.Length > 0 && r.Call != CqEntry && !r.Call.Contains(q)) continue;
                var item = new ListBoxItem { Tag = r.Call, Content = RowView(r), Padding = new Thickness(2, 4, 2, 4) };
                list.Items.Add(item);
                if (r.Call == selected) item.IsSelected = true;
            }
        }
        finally { rebuilding = false; }
    }

    static UIElement RowView(Row r)
    {
        var g = new DockPanel();
        var dot = new System.Windows.Shapes.Ellipse { Width = 10, Height = 10, Fill = r.Dot, Margin = new Thickness(0, 4, 8, 0), VerticalAlignment = VerticalAlignment.Top };
        DockPanel.SetDock(dot, Dock.Left);
        g.Children.Add(dot);
        if (r.Unread > 0)
        {
            var badge = new Border
            {
                Background = Brushes.Firebrick, CornerRadius = new CornerRadius(8), Padding = new Thickness(6, 0, 6, 0),
                VerticalAlignment = VerticalAlignment.Top,
                Child = new TextBlock { Text = r.Unread.ToString(), Foreground = Brushes.White, FontWeight = FontWeights.SemiBold },
            };
            DockPanel.SetDock(badge, Dock.Right);
            g.Children.Add(badge);
        }
        var text = new StackPanel();
        text.Children.Add(new TextBlock { Text = r.Call == CqEntry ? "All stations (CQ)" : r.Call, FontWeight = FontWeights.SemiBold });
        text.Children.Add(new TextBlock { Text = r.Detail, Foreground = Brushes.Gray, FontSize = 11, TextWrapping = TextWrapping.Wrap });
        g.Children.Add(text);
        return g;
    }

    // ---------------------------------------------------------------- one station
    void ShowHeader(string call)
    {
        if (call == CqEntry)
        {
            title.Text = "All stations (CQ)";
            subtitle.Text = "Messages, voice clips, positions and reports you sent to everyone. Received CQ messages are listed under their sender.";
            showOnMap.Visibility = Visibility.Collapsed;
            return;
        }
        title.Text = call;
        var st = mapItems.FirstOrDefault(i => i.Type == 'S' && !i.Self && string.Equals(i.Name, call, StringComparison.OrdinalIgnoreCase));
        var row = Rows().FirstOrDefault(r => r.Call == call);
        subtitle.Text = (row?.Detail ?? "") + (st == null ? " · no position yet" : "");
        showOnMap.Visibility = st != null ? Visibility.Visible : Visibility.Collapsed;
    }

    void ShowConversation(string call)
    {
        conversation.Children.Clear();
        var msgs = MessageStore.All.Where(m => m.Kind != "id" && Shown(m) && m.Parties.Contains(call, StringComparer.OrdinalIgnoreCase)).ToList();
        if (msgs.Count == 0)
            conversation.Children.Add(new TextBlock { Text = "No messages yet. Type below to send one.", Foreground = Brushes.Gray, Margin = new Thickness(10) });
        string? lastDay = null;
        foreach (var m in msgs)
        {
            var t = DateTimeOffset.FromUnixTimeMilliseconds(m.T).LocalDateTime;
            string day = t.ToString("dddd d MMMM");
            if (day != lastDay)
            {
                conversation.Children.Add(new TextBlock { Text = day, HorizontalAlignment = HorizontalAlignment.Center, Foreground = Brushes.Gray, FontSize = 11, Margin = new Thickness(0, 8, 0, 2) });
                lastDay = day;
            }
            conversation.Children.Add(Bubble(m));
        }
        convScroll.ScrollToEnd();
    }

    UIElement Bubble(MsgRecord m)
    {
        bool mine = m.Dir == MsgDir.Out;
        var body = new StackPanel();
        string who = m.Dir switch
        {
            MsgDir.Out => m.To == CqEntry ? "to everyone" : $"to {m.To}",
            MsgDir.Heard => $"{m.From} → {m.To} (overheard)",
            _ => m.To == CqEntry ? $"{m.From} to everyone" : m.From,
        };
        var meta = new TextBlock { FontSize = 11, Foreground = Brushes.DimGray };
        void SetMeta() => meta.Text = $"{DateTimeOffset.FromUnixTimeMilliseconds(m.T).LocalDateTime:HH:mm}  {who}" +
                                      (m.Via != null ? $"  · via {m.Via}" : "") +
                                      (m.Dir != MsgDir.In && m.Status != null ? $"  [{m.Status}]" : "");
        SetMeta();
        if (m.Dir != MsgDir.In) m.PropertyChanged += (_, _) => Dispatcher.BeginInvoke(SetMeta);
        body.Children.Add(meta);
        string text = m.Kind == "report" ? "[report] " + m.Text : m.Text;
        var line = new DockPanel();
        if (m.Kind == "voice" && m.Clip != null)
        {
            var playBtn = new Button { Content = "Play", Padding = new Thickness(10, 1, 10, 1), Margin = new Thickness(8, 0, 0, 0) };
            playBtn.Click += (_, _) => PlayClip(m.Clip);
            DockPanel.SetDock(playBtn, Dock.Right);
            line.Children.Add(playBtn);
        }
        line.Children.Add(new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, Foreground = m.Dir == MsgDir.Heard ? Brushes.Gray : Brushes.Black });
        body.Children.Add(line);
        return new Border
        {
            Child = body,
            Background = m.Dir == MsgDir.Heard ? Brushes.WhiteSmoke : mine ? new SolidColorBrush(Color.FromRgb(0xE3, 0xF0, 0xFF)) : new SolidColorBrush(Color.FromRgb(0xF1, 0xF1, 0xF1)),
            CornerRadius = new CornerRadius(6),
            Padding = new Thickness(8, 4, 8, 6),
            Margin = mine ? new Thickness(60, 3, 8, 3) : new Thickness(8, 3, 60, 3),
            HorizontalAlignment = mine ? HorizontalAlignment.Right : HorizontalAlignment.Left,
        };
    }

    void PlayClip(string b64)
    {
        try
        {
            decoder ??= new Radio("N0CALL", false, 1, 300);
            var pcm = decoder.VoiceDecode(Convert.FromBase64String(b64));
            if (pcm != null) play(pcm);
        }
        catch (Exception ex) { MessageBox.Show(Window.GetWindow(this), $"Cannot play the clip: {ex.Message}", "HamLink"); }
    }

    void OnStoreChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.Action == NotifyCollectionChangedAction.Add && selected != null && e.NewItems != null)
        {
            foreach (MsgRecord m in e.NewItems)
                if (m.Kind != "id" && Shown(m) && m.Parties.Contains(selected, StringComparer.OrdinalIgnoreCase))
                {
                    if (conversation.Children.Count == 1 && conversation.Children[0] is TextBlock) conversation.Children.Clear();
                    conversation.Children.Add(Bubble(m));
                    convScroll.ScrollToEnd();
                    if (IsShown) MarkRead(selected);
                }
        }
        else if (selected != null) ShowConversation(selected); // cleared or reloaded
        RebuildList();
    }

    // ---------------------------------------------------------------- unread state
    void MarkRead(string call)
    {
        lastRead[call] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(readPath)!);
            File.WriteAllText(readPath, JsonSerializer.Serialize(lastRead));
        }
        catch (Exception ex) { App.LogError("stations read state", ex); }
    }

    Dictionary<string, long> LoadRead()
    {
        try
        {
            if (File.Exists(readPath))
                return JsonSerializer.Deserialize<Dictionary<string, long>>(File.ReadAllText(readPath)) is { } d
                    ? new Dictionary<string, long>(d, StringComparer.OrdinalIgnoreCase) : new(StringComparer.OrdinalIgnoreCase);
        }
        catch (Exception ex) { App.LogError("stations read state", ex); }
        return new(StringComparer.OrdinalIgnoreCase);
    }

    // ---------------------------------------------------------------- formatting
    static string Ago(long t)
    {
        var d = TimeSpan.FromMilliseconds(Math.Max(0, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - t));
        return d.TotalMinutes < 1 ? "just now" : d.TotalHours < 1 ? $"{(int)d.TotalMinutes} min ago"
             : d.TotalDays < 1 ? $"{(int)d.TotalHours} h ago" : $"{(int)d.TotalDays} d ago";
    }

    static string Compass(double deg) => new[] { "N", "NE", "E", "SE", "S", "SW", "W", "NW" }[(int)Math.Round(((deg % 360) + 360) % 360 / 45.0) % 8];
}
