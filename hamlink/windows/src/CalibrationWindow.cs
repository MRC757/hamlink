// SPDX-License-Identifier: GPL-3.0-or-later
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace HamLink;

/// <summary>
/// PTT test + manual calibration. TX side: tone with a live level. RX side: meter with peak hold,
/// tone levels at 1200/2200 Hz, twist and a clipping warning. Code-only to keep the XAML surface small.
/// </summary>
internal sealed class CalibrationWindow : Window
{
    readonly Settings settings;
    readonly Engine engine;

    readonly RadioButton rbOff = new() { Content = "PTT only", GroupName = "m" };
    readonly RadioButton rb1200 = new() { Content = "1200 Hz", GroupName = "m" };
    readonly RadioButton rb2200 = new() { Content = "2200 Hz", GroupName = "m" };
    readonly RadioButton rbAlt = new() { Content = "Alternating", GroupName = "m", IsChecked = true };
    readonly Slider level = new() { Minimum = 0.02, Maximum = 1.0 };
    readonly TextBlock levelText = new();
    readonly Button txBtn = new() { Padding = new Thickness(16, 6, 16, 6) };
    readonly Button saveBtn = new() { Content = "Use as TX level", Padding = new Thickness(16, 6, 16, 6), Margin = new Thickness(8, 0, 0, 0) };
    readonly TextBlock onAir = new() { Foreground = Brushes.Firebrick, FontWeight = FontWeights.Bold };

    readonly ProgressBar peakBar = new() { Minimum = 0, Maximum = 1, Height = 12 };
    readonly ProgressBar bar1200 = new() { Minimum = 0, Maximum = 1, Height = 12 };
    readonly ProgressBar bar2200 = new() { Minimum = 0, Maximum = 1, Height = 12 };
    readonly TextBlock peakText = new(), text1200 = new(), text2200 = new(), twistText = new(), advice = new() { TextWrapping = TextWrapping.Wrap };

    float hold1200 = -120f, hold2200 = -120f, holdPeak = -120f;
    bool testing;
    int secondsLeft;

    static float Db(float x) => x <= 0f ? -120f : 20f * MathF.Log10(x);
    static string Fmt(float db) => db <= -119f ? "  -inf" : $"{db,6:0.0}";
    static double Bar(float db) => Math.Clamp((db + 60f) / 60f, 0f, 1f); // -60..0 dBFS

    public CalibrationWindow(Settings s, Engine e)
    {
        settings = s;
        engine = e;
        Title = "PTT test and calibration";
        Width = 540;
        Height = 780;
        MinHeight = 500;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        level.Value = Math.Clamp(s.TxLevel, 0.02f, 1f);

        var p = new StackPanel { Margin = new Thickness(14) };
        p.Children.Add(Heading("Transmit test"));
        p.Children.Add(Note("This keys your radio. Use low power or a dummy load, and only a frequency you may use. " +
                            "A test tone lasts at most 15 s, and your callsign is sent right after."));
        var modes = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 4) };
        foreach (var rb in new[] { rbOff, rb1200, rb2200, rbAlt }) { rb.Margin = new Thickness(0, 0, 14, 0); modes.Children.Add(rb); }
        p.Children.Add(modes);
        p.Children.Add(levelText);
        p.Children.Add(level);
        var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 8, 0, 4) };
        row.Children.Add(txBtn);
        row.Children.Add(saveBtn);
        p.Children.Add(row);
        p.Children.Add(onAir);

        p.Children.Add(Heading("Receive meter"));
        p.Children.Add(peakText); p.Children.Add(peakBar);
        p.Children.Add(text1200); p.Children.Add(bar1200);
        p.Children.Add(text2200); p.Children.Add(bar2200);
        p.Children.Add(twistText);
        p.Children.Add(advice);
        var reset = new Button { Content = "Reset holds", Padding = new Thickness(12, 3, 12, 3), HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 6, 0, 0) };
        reset.Click += (_, _) => { hold1200 = hold2200 = holdPeak = -120f; Refresh(null); };
        p.Children.Add(reset);

        p.Children.Add(Heading("Two-station procedure"));
        p.Children.Add(Note(
            "1. Station B opens this window and watches the meter.\n" +
            "2. Station A picks Alternating, sets a low level (about 0.05) and starts the tone.\n" +
            "3. A raises the level in steps of about 3 dB (x1.4), pausing 2 s on each. B notes the held 1200 and 2200 Hz readings.\n" +
            "4. Where B's reading stops rising, the radio's limiter is working. Set A about 6 dB (half) below that and press Use as TX level.\n" +
            "The knee may be soft on cheap radios; confirm with a real message exchange. Repeat if you change wide/narrow bandwidth."));
        Content = new ScrollViewer { Content = p, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };

        level.ValueChanged += (_, _) => { UpdateLevelText(); engine.SetTestLevel((float)level.Value); };
        txBtn.Click += (_, _) =>
        {
            if (testing) { engine.StopTest(); return; }
            if (!engine.IsRunning) { MessageBox.Show(this, "Press Start on the main window first: the radio engine must be running.", "HamLink"); return; }
            engine.StartTest(Mode(), (float)level.Value);
        };
        saveBtn.Click += (_, _) =>
        {
            settings.TxLevel = (float)level.Value; settings.Save();
            saveBtn.Content = "Saved";
        };
        level.ValueChanged += (_, _) => saveBtn.Content = "Use as TX level";

        engine.Meter += OnMeter;
        engine.TestState += OnTestState;
        Closed += (_, _) => { engine.Meter -= OnMeter; engine.TestState -= OnTestState; engine.StopTest(); };
        UpdateLevelText();
        UpdateTx();
        Refresh(null);
    }

    int Mode() => rbOff.IsChecked == true ? Radio.ToneOff : rb1200.IsChecked == true ? Radio.Tone1200
                : rb2200.IsChecked == true ? Radio.Tone2200 : Radio.ToneAlt;

    static TextBlock Heading(string t) => new() { Text = t, FontSize = 16, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 14, 0, 4) };
    static TextBlock Note(string t) => new() { Text = t, TextWrapping = TextWrapping.Wrap, Foreground = Brushes.DimGray };

    void UpdateLevelText() => levelText.Text = $"TX level {level.Value:0.000}  ({Db((float)level.Value):0.0} dBFS)";

    void UpdateTx()
    {
        txBtn.Content = testing ? $"STOP  ({secondsLeft} s left)" : "Transmit test";
        onAir.Text = testing ? "ON AIR" : "";
        foreach (var rb in new[] { rbOff, rb1200, rb2200, rbAlt }) rb.IsEnabled = !testing;
    }

    void OnTestState(bool on, int left) => Dispatcher.BeginInvoke(() => { testing = on; secondsLeft = left; UpdateTx(); });
    void OnMeter(MeterData m) => Dispatcher.BeginInvoke(() => Refresh(m));

    MeterData last = new(-120, -120, -120, -120, 0);

    void Refresh(MeterData? m)
    {
        if (m is { } v)
        {
            last = v;
            hold1200 = MathF.Max(hold1200, v.Tone1200Db);
            hold2200 = MathF.Max(hold2200, v.Tone2200Db);
            holdPeak = MathF.Max(holdPeak, v.PeakDb);
        }
        peakText.Text = $"Peak {Fmt(last.PeakDb)} dBFS    RMS {Fmt(last.RmsDb)} dBFS";
        peakBar.Value = Bar(last.PeakDb);
        text1200.Text = $"1200 Hz {Fmt(last.Tone1200Db)} dBFS    (hold {Fmt(hold1200)})";
        bar1200.Value = Bar(last.Tone1200Db);
        text2200.Text = $"2200 Hz {Fmt(last.Tone2200Db)} dBFS    (hold {Fmt(hold2200)})";
        bar2200.Value = Bar(last.Tone2200Db);
        twistText.Text = hold1200 > -70f && hold2200 > -70f
            ? $"Twist (2200 vs 1200, held): {hold2200 - hold1200:+0.0;-0.0} dB. Near 0 is ideal; 6 dB was tolerated in simulation."
            : "";
        (string msg, Brush color) = last.Clipped > 0
            ? ("INPUT CLIPPING: lower the radio volume or the DigiRig input gain.", Brushes.Firebrick)
            : holdPeak > -3f ? ("Very hot input. Reduce the radio volume.", Brushes.Black)
            : holdPeak > -120f && holdPeak < -40f ? ("Input is weak. Raise the radio volume.", Brushes.Black)
            : holdPeak >= -40f ? ("Input level OK (a peak of about -20 to -6 dBFS is a good target).", Brushes.Black)
            : ("No signal yet.", Brushes.Black);
        advice.Text = msg;
        advice.Foreground = color;
    }
}
