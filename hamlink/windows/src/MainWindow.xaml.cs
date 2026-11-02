// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace HamLink;

/// <summary>One line in the message log. Status text updates live as ACKs arrive.</summary>
public sealed class LogItem : INotifyPropertyChanged
{
    internal LogItem(LogKind kind, string text, int msgId, short[]? clip)
    {
        Kind = kind; Text = text; MsgId = msgId; Clip = clip;
    }

    internal LogKind Kind { get; }
    public string Text { get; }
    public int MsgId { get; }
    public short[]? Clip { get; }
    string? status;

    public string? Status
    {
        get => status;
        set { status = value; Raise(nameof(Display)); Raise(nameof(Brush)); }
    }

    public string Display
    {
        get
        {
            string prefix = Kind switch
            {
                LogKind.TxText or LogKind.TxVoice => "→ ",
                LogKind.RxText or LogKind.RxVoice => "← ",
                LogKind.Event => "· ",
                _ => "! ",
            };
            return prefix + Text + (string.IsNullOrEmpty(status) ? "" : $"   [{status}]");
        }
    }

    public Brush Brush =>
        Kind == LogKind.Error || (status?.StartsWith("FAILED") ?? false) ? Brushes.Firebrick
        : Kind == LogKind.Event ? Brushes.Gray
        : Brushes.Black;

    public Visibility PlayVisibility => Clip != null ? Visibility.Visible : Visibility.Collapsed;

    public event PropertyChangedEventHandler? PropertyChanged;
    void Raise(string n) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
}

public partial class MainWindow : Window
{
    readonly Settings settings = Settings.Load();
    readonly Engine engine;
    readonly ObservableCollection<LogItem> log = new();
    readonly Dictionary<int, LogItem> byId = new();
    readonly VoiceRecorder recorder = new();
    bool recording;
    bool loadingUi = true;

    public MainWindow()
    {
        InitializeComponent();
        engine = new Engine(settings);
        LogList.ItemsSource = log;
        DestBox.Text = settings.Dest;
        RelayBox.IsChecked = settings.Relay;
        SyncModemButtons();
        loadingUi = false;
        Positions.Apply(settings);
        RefreshHeader();
        SetRunning(false);

        engine.Log += (kind, text, id, clip) => Dispatcher.BeginInvoke(() => AddLog(kind, text, id, clip));
        engine.StatusChanged += (id, text) => Dispatcher.BeginInvoke(() =>
        {
            if (byId.TryGetValue(id, out var it)) it.Status = text;
        });
        engine.PhaseChanged += (phase, level) => Dispatcher.BeginInvoke(() => ShowPhase(phase, level));
        engine.Mode += text => Dispatcher.BeginInvoke(() => ModeText.Text = text);
        engine.ManualState += (on, stage, pending) => Dispatcher.BeginInvoke(() =>
        {
            ManualPanel.Visibility = on ? Visibility.Visible : Visibility.Collapsed;
            ManualText.Text = stage.Length > 0 ? stage : $"Manual PTT: {pending} frame(s) waiting. Press Transmit, then key the radio when told.";
            ManualText.Foreground = stage.Length > 0 ? System.Windows.Media.Brushes.Firebrick : System.Windows.Media.Brushes.Black;
            Ui.Enable(ManualGoBtn, stage.Length == 0, "A transmission is already in progress. Press Cancel to stop it.");
            ManualGoBtn.Content = $"Transmit ({settings.ManualCountdown} s countdown)";
        });
        engine.Info += (route, ptt) => Dispatcher.BeginInvoke(() => { RouteText.Text = route; PttText.Text = ptt; });

        if (!settings.CallsignValid) Loaded += (_, _) => OnSettings(this, new RoutedEventArgs());
    }

    MapPanel? mapPanel;

    void OnViewChanged(object sender, RoutedEventArgs e)
    {
        if (loadingUi) return; // Checked fires while the window is still being built
        if (ViewMapBtn.IsChecked == true)
        {
            if (mapPanel == null) { mapPanel = new MapPanel(settings, engine); MapHost.Child = mapPanel; }
            MapHost.Visibility = Visibility.Visible;
            mapPanel.Refresh();
        }
        else MapHost.Visibility = Visibility.Collapsed;
    }

    void RefreshHeader() => CallText.Text = settings.CallsignValid ? settings.Callsign : "set your callsign in Settings";

    void AddLog(LogKind kind, string text, int id, short[]? clip)
    {
        var item = new LogItem(kind, text, id, clip);
        log.Add(item);
        if (id >= 0) byId[id] = item;
        while (log.Count > 400) { var old = log[0]; log.RemoveAt(0); if (old.MsgId >= 0) byId.Remove(old.MsgId); }
        LogList.ScrollIntoView(item);
    }

    void ShowPhase(Phase phase, float level)
    {
        (string label, Brush color) = phase switch
        {
            Phase.Idle => ("LISTENING", Brushes.SeaGreen),
            Phase.Busy => ("CHANNEL BUSY", Brushes.DarkOrange),
            Phase.Tx => ("TRANSMITTING", Brushes.Firebrick),
            _ => ("OFF", Brushes.Gray),
        };
        PhaseText.Text = label;
        PhaseBadge.Background = color;
        LevelBar.Value = Math.Clamp(level * 20f, 0f, 1f);
        if (phase == Phase.Off && engine.IsRunning == false) SetRunning(false);
    }

    void SetRunning(bool on)
    {
        StartBtn.Content = on ? "Stop" : "Start";
        const string startFirst = "Press Start first: the radio is not running.";
        Ui.Enable(SendBtn, on, startFirst);
        Ui.Enable(TalkBtn, on, startFirst);
        Ui.Enable(TestBtn, on, startFirst);
        // settings take effect at Start; avoid half-applied changes
        Ui.Enable(SettingsBtn, !on, "Press Stop to change settings. They take effect at the next Start.");
        RelayBox.IsEnabled = true;
        mapPanel?.Refresh();
        if (!on) { PhaseText.Text = "OFF"; PhaseBadge.Background = Brushes.Gray; LevelBar.Value = 0; }
    }

    // ------------------------------------------------------------------ buttons
    void OnStartStop(object sender, RoutedEventArgs e)
    {
        if (engine.IsRunning)
        {
            engine.Stop();
            SetRunning(false);
            return;
        }
        if (!settings.CallsignValid) { OnSettings(sender, e); if (!settings.CallsignValid) return; }
        settings.Dest = DestBox.Text; settings.Relay = RelayBox.IsChecked == true; settings.Save();
        engine.Start();
        SetRunning(true);
    }

    void OnSettings(object sender, RoutedEventArgs e)
    {
        var dlg = new SettingsWindow(settings) { Owner = this };
        if (dlg.ShowDialog() == true)
        {
            settings.Save();
            RefreshHeader();
            loadingUi = true; // the modem may have changed in Settings: show it without re-applying it
            SyncModemButtons();
            loadingUi = false;
        }
    }

    void SyncModemButtons()
    {
        ModemAfskBtn.IsChecked = settings.Modem is not (1 or 2 or 3 or 4);
        ModemOfdmBtn.IsChecked = settings.Modem == 1;
        ModemMt63Btn.IsChecked = settings.Modem == 2;
        ModemPsk2400Btn.IsChecked = settings.Modem == 3;
        ModemPsk3600Btn.IsChecked = settings.Modem == 4;
        ModemPanel.Visibility = settings.Transport == "tnc" ? Visibility.Collapsed : Visibility.Visible;
    }

    void OnModemChanged(object sender, RoutedEventArgs e)
    {
        if (loadingUi) return;
        settings.Modem = ModemPsk3600Btn.IsChecked == true ? 4 : ModemPsk2400Btn.IsChecked == true ? 3
                       : ModemMt63Btn.IsChecked == true ? 2 : ModemOfdmBtn.IsChecked == true ? 1 : 0;
        settings.Save();
        engine.SetModem(settings.Modem); // takes effect at once while running; otherwise at the next Start
    }

    HelpWindow? help;
    /// <summary>Help is not modal: it can stay open beside the main window while you work.</summary>
    void OnHelp(object sender, RoutedEventArgs e)
    {
        if (help != null) { help.Activate(); return; }
        help = new HelpWindow { Owner = this };
        help.Closed += (_, _) => help = null;
        help.Show();
    }

    void OnManualGo(object sender, RoutedEventArgs e) => engine.ManualGo();
    void OnManualCancel(object sender, RoutedEventArgs e) => engine.ManualCancel();

    CalibrationWindow? cal;

    void OnTest(object sender, RoutedEventArgs e)
    {
        if (settings.Transport == "tnc")
        {
            AddLog(LogKind.Event, "PTT test and calibration only apply to the audio link, not KISS TNC mode", -1, null);
            return;
        }
        if (cal != null) { cal.Activate(); return; }
        cal = new CalibrationWindow(settings, engine) { Owner = this };
        cal.Closed += (_, _) => cal = null;
        cal.Show();
    }

    void OnRelayChanged(object sender, RoutedEventArgs e)
    {
        if (loadingUi) return;
        settings.Relay = RelayBox.IsChecked == true;
        settings.Save();
        engine.SetRelay(settings.Relay);
    }

    void OnSendText(object sender, RoutedEventArgs e)
    {
        string text = MsgBox.Text.Trim(), dst = DestBox.Text.Trim().ToUpperInvariant();
        if (text.Length == 0 || dst.Length == 0) return;
        settings.Dest = dst;
        engine.SendText(dst, text);
        MsgBox.Clear();
    }

    void OnMsgKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && SendBtn.IsEnabled) { OnSendText(sender, e); e.Handled = true; }
    }

    void OnPlayClip(object sender, RoutedEventArgs e)
    {
        if (((Button)sender).Tag is LogItem { Clip: { } pcm })
        {
            try { VoicePlayer.Play(pcm, Devices.Find(Devices.Outputs(), settings.VoiceOutput, preferUsb: false) is int d && d >= 0 ? d : -1); }
            catch (Exception ex) { AddLog(LogKind.Error, $"playback failed: {ex.Message}", -1, null); }
        }
    }

    // ------------------------------------------------------------------ hold-to-record voice
    void OnTalkDown(object sender, MouseButtonEventArgs e)
    {
        if (recording || !TalkBtn.IsEnabled) return;
        try
        {
            int dev = Devices.Find(Devices.Inputs(), settings.VoiceInput, preferUsb: false);
            recorder.Start(dev >= 0 ? dev : -1);
            recording = true;
            TalkBtn.Content = "Recording… release to send";
            TalkBtn.CaptureMouse();
        }
        catch (Exception ex) { AddLog(LogKind.Error, $"cannot record: {ex.Message}", -1, null); }
    }

    void OnTalkUp(object sender, MouseButtonEventArgs e) => FinishRecording();
    void OnTalkLost(object sender, MouseEventArgs e) => FinishRecording();

    void FinishRecording()
    {
        if (!recording) return;
        recording = false;
        TalkBtn.ReleaseMouseCapture();
        TalkBtn.Content = "Hold to record voice";
        var pcm = recorder.Stop();
        string dst = DestBox.Text.Trim().ToUpperInvariant();
        if (pcm.Length >= 4000 && dst.Length > 0) { settings.Dest = dst; engine.SendVoice(dst, pcm); }
        else if (pcm.Length < 4000) AddLog(LogKind.Event, "recording too short, not sent", -1, null);
    }

    void OnClosing(object? sender, CancelEventArgs e)
    {
        settings.Dest = DestBox.Text;
        settings.Save();
        engine.Stop(); // unkeys PTT
        Positions.Stop();
    }
}
