// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Ports;
using System.Windows;
using System.Windows.Controls;

namespace HamLink;

/// <summary>Code-only settings dialog. Returns true from ShowDialog() when saved.</summary>
internal sealed class SettingsWindow : Window
{
    const string Auto = "(auto: first USB audio device)";
    const string WinDefault = "(Windows default)";
    const string RadioDefault = "(Windows default: speakers + mic, for acoustic coupling)";
    const string AutoPort = "(auto: only COM port)";
    const string LinkAudioText = "Audio + PTT (DigiRig / sound card)";
    const string LinkTncText = "KISS TNC (paired Bluetooth radio or serial TNC)";
    const string ModemAfskText = "AFSK 1200 baud";
    const string ModemOfdmText = "OFDM (COFDMTV, as in Rattlegram)";
    const string ModemMt63Text = "MT63-2000: slow but robust";
    const string ModemPsk2400Text = "PSK 2400 (QPSK): about twice as fast as AFSK";
    const string ModemPsk3600Text = "PSK 3600 (8-PSK): fastest, needs a strong signal";

    readonly Settings s;
    readonly TextBox call = new() { MaxLength = 8, CharacterCasing = CharacterCasing.Upper };
    readonly ComboBox link = new();
    readonly ComboBox modem = new();
    readonly TextBlock modemHint = new() { TextWrapping = TextWrapping.Wrap, Foreground = System.Windows.Media.Brushes.DimGray };
    readonly ComboBox tncPort = new() { IsEditable = true };
    readonly ComboBox tncBaud = new();
    readonly ComboBox radioIn = new() { IsEditable = true };
    readonly ComboBox radioOut = new() { IsEditable = true };
    readonly ComboBox voiceIn = new() { IsEditable = true };
    readonly ComboBox voiceOut = new() { IsEditable = true };
    readonly ComboBox pttMode = new();
    readonly ComboBox pttPort = new() { IsEditable = true };
    readonly Slider ttl = new() { Minimum = 0, Maximum = 3, TickFrequency = 1, IsSnapToTickEnabled = true };
    readonly Slider pre = new() { Minimum = 200, Maximum = 1500 };
    readonly Slider tx = new() { Minimum = 0.02, Maximum = 1.0 };
    readonly Slider busy = new() { Minimum = 0.0005, Maximum = 0.05 };
    readonly CheckBox auto = new() { Content = "Auto-play received voice clips" };
    static readonly string[] SenseChoices =
    {
        "Off: never wait (tests only; you may transmit over other stations)",
        "Data only: wait while packet data is heard; ignore room noise and voice (acoustic tests)",
        "Data or loud audio: also wait while the audio is above the busy threshold (default)",
    };
    readonly ComboBox sense = new();
    readonly CheckBox repair = new() { Content = "Repair single-bit errors in received frames", Margin = new Thickness(0, 6, 0, 0) };
    readonly CheckBox record = new()
    {
        Content = "Record received radio audio to WAV files (for troubleshooting; about 350 MB per hour, oldest deleted above 1 GB)",
        Margin = new Thickness(0, 6, 0, 0),
        ToolTip = Recorder.Folder,
    };
    static readonly (int Bytes, string Text)[] FecChoices =
    {
        (0, "Off (plain AX.25-style frames)"),
        (16, "FX.25, 16 check bytes: fixes up to 8 bad bytes per frame (default)"),
        (32, "FX.25, 32 check bytes: fixes up to 16"),
        (64, "FX.25, 64 check bytes: fixes up to 32, longest transmissions"),
    };
    readonly ComboBox fec = new();
    readonly Slider manualSecs = new() { Minimum = 2, Maximum = 10, TickFrequency = 1, IsSnapToTickEnabled = true };
    readonly TextBlock manualLabel = new(), pttHint = new() { TextWrapping = TextWrapping.Wrap, Foreground = System.Windows.Media.Brushes.DimGray };
    readonly TextBlock ttlLabel = new(), preLabel = new(), txLabel = new(), busyLabel = new();
    readonly StackPanel audioSection = new(), tncSection = new();

    public SettingsWindow(Settings settings)
    {
        s = settings;
        Title = "HamLink settings";
        Width = 540;
        SizeToContent = SizeToContent.Height;
        ResizeMode = ResizeMode.NoResize;
        WindowStartupLocation = WindowStartupLocation.CenterOwner;

        var ins = Devices.Inputs();
        var outs = Devices.Outputs();
        var ports = SerialPort.GetPortNames().ToList();
        Fill(radioIn, ins, Auto, s.RadioInput == Devices.DefaultName ? RadioDefault : s.RadioInput);
        Fill(radioOut, outs, Auto, s.RadioOutput == Devices.DefaultName ? RadioDefault : s.RadioOutput);
        radioIn.Items.Insert(1, RadioDefault);
        radioOut.Items.Insert(1, RadioDefault);
        Fill(voiceIn, ins, WinDefault, s.VoiceInput);
        Fill(voiceOut, outs, WinDefault, s.VoiceOutput);
        Fill(pttPort, ports, AutoPort, s.PttPort);
        Fill(tncPort, ports, AutoPort, s.TncPort);
        foreach (var m in PttChoices) pttMode.Items.Add(m.Text);
        pttMode.SelectedIndex = Math.Max(0, Array.FindIndex(PttChoices, m => m.Value == s.PttMode));
        pttMode.SelectionChanged += (_, _) => UpdateSections();
        manualSecs.Value = s.ManualCountdown;
        manualSecs.ValueChanged += (_, _) => UpdateLabels();
        foreach (var t in SenseChoices) sense.Items.Add(t);
        sense.SelectedIndex = s.EffectiveCarrierSense;
        repair.IsChecked = s.BitRepair;
        record.IsChecked = s.RecordRx;
        foreach (var f in FecChoices) fec.Items.Add(f.Text);
        fec.SelectedIndex = Math.Max(0, Array.FindIndex(FecChoices, f => f.Bytes == s.AfskFec));
        link.Items.Add(LinkAudioText); link.Items.Add(LinkTncText);
        link.SelectedIndex = s.Transport == "tnc" ? 1 : 0;
        foreach (var t in new[] { ModemAfskText, ModemOfdmText, ModemMt63Text, ModemPsk2400Text, ModemPsk3600Text }) modem.Items.Add(t);
        modem.SelectedIndex = s.Modem is >= 0 and <= 4 ? s.Modem : 0; // list order = modem number
        tncBaud.Items.Add("1200"); tncBaud.Items.Add("9600");
        tncBaud.SelectedIndex = s.TncBaud >= 9600 ? 1 : 0;

        call.Text = s.Callsign;
        ttl.Value = s.Ttl; pre.Value = s.PreambleMs; tx.Value = s.TxLevel; busy.Value = s.BusyThreshold;
        auto.IsChecked = s.AutoPlayVoice;
        ttl.ValueChanged += (_, _) => UpdateLabels();
        pre.ValueChanged += (_, _) => UpdateLabels();
        tx.ValueChanged += (_, _) => UpdateLabels();
        busy.ValueChanged += (_, _) => UpdateLabels();
        link.SelectionChanged += (_, _) => UpdateSections();
        modem.SelectionChanged += (_, _) => UpdateSections();
        UpdateLabels();

        var panel = new StackPanel { Margin = new Thickness(14) };
        Row(panel, "Your callsign (required)", call);
        Row(panel, "Radio link", link);

        // --- audio link section
        Row(audioSection, "Modem (every station on the channel must use the same one)", modem);
        Row(audioSection, "AFSK error correction (stations without FX.25 still read error-free frames)", fec);
        audioSection.Children.Add(modemHint);
        Row(audioSection, "Radio audio input (DigiRig)", radioIn);
        Row(audioSection, "Radio audio output (DigiRig)", radioOut);
        Row(audioSection, "PTT (how the radio is keyed)", pttMode);
        audioSection.Children.Add(pttHint);
        Row(audioSection, "PTT COM port (DigiRig serial; RTS/DTR only)", pttPort);
        Row(audioSection, manualLabel, manualSecs);
        Row(audioSection, "Carrier sense (wait while the channel is in use)", sense);
        audioSection.Children.Add(repair);
        audioSection.Children.Add(record);
        Row(audioSection, preLabel, pre);
        Row(audioSection, txLabel, tx);
        Row(audioSection, busyLabel, busy);
        Row(audioSection, "Voice clips: your microphone (not the radio!)", voiceIn);
        Row(audioSection, "Voice clips: your speakers (not the radio!)", voiceOut);
        panel.Children.Add(audioSection);

        // --- TNC link section
        Row(tncSection, "TNC COM port (for a paired Bluetooth radio: the OUTGOING virtual port)", tncPort);
        Row(tncSection, "TNC speed (only used to estimate airtime for ACK timing)", tncBaud);
        tncSection.Children.Add(new TextBlock
        {
            Margin = new Thickness(0, 8, 0, 0),
            TextWrapping = TextWrapping.Wrap,
            Foreground = System.Windows.Media.Brushes.DimGray,
            Text = "The radio's own TNC does the modulation, keying and carrier sense, so there is no audio or PTT setup. " +
                   "Pair the radio in Windows Bluetooth settings and enable KISS TNC on the radio (BTECH UV-PRO: " +
                   "Settings > General Settings > KISS TNC). Test / Calibrate does not apply in this mode.",
        });
        panel.Children.Add(tncSection);

        Row(panel, ttlLabel, ttl);
        panel.Children.Add(auto);
        panel.Children.Add(new TextBlock
        {
            Margin = new Thickness(0, 10, 0, 0),
            Foreground = System.Windows.Media.Brushes.Gray,
            TextWrapping = TextWrapping.Wrap,
            Text = "Do not make the DigiRig the Windows default recording or playback device, or system sounds will be transmitted. " +
                   "Changes apply the next time you press Start.",
        });

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 12, 0, 0) };
        var ok = new Button { Content = "Save", Padding = new Thickness(18, 4, 18, 4), IsDefault = true, Margin = new Thickness(0, 0, 8, 0) };
        var cancel = new Button { Content = "Cancel", Padding = new Thickness(18, 4, 18, 4), IsCancel = true };
        ok.Click += (_, _) => Save();
        buttons.Children.Add(ok);
        buttons.Children.Add(cancel);
        panel.Children.Add(buttons);
        Content = new ScrollViewer { Content = panel, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MaxHeight = 800 };
        UpdateSections();
    }

    static readonly (string Value, string Text)[] PttChoices =
    {
        ("RTS", "Serial RTS (DigiRig)"),
        ("DTR", "Serial DTR"),
        ("VOX", "None / radio VOX (transmit immediately)"),
        ("MANUAL", "Manual (I press the radio's PTT)"),
    };

    string SelectedPtt => PttChoices[Math.Clamp(pttMode.SelectedIndex, 0, PttChoices.Length - 1)].Value;

    void UpdateSections()
    {
        bool manual = SelectedPtt == "MANUAL";
        manualSecs.Visibility = manualLabel.Visibility = manual ? Visibility.Visible : Visibility.Collapsed;
        pttHint.Text = manual
            ? "Manual: press Transmit, key the radio during the countdown, hold PTT while the audio plays, release when told. ACK waits are stretched to 45 s."
            : SelectedPtt == "VOX" ? "No PTT line: audio is played immediately (radio VOX, or an acoustic test with no radio)." : "";
        bool tnc = link.SelectedIndex == 1;
        audioSection.Visibility = tnc ? Visibility.Collapsed : Visibility.Visible;
        tncSection.Visibility = tnc ? Visibility.Visible : Visibility.Collapsed;
        modemHint.Text = modem.SelectedIndex switch
        {
            1 => "OFDM is not faster than AFSK (its lead-in symbols cost airtime), and on a normal radio link AFSK with FX.25 " +
                 "now reaches slightly weaker signals in simulation. It may cope better with echo and multipath. Lower the " +
                 "PTT lead-in to about 200 ms to save time.",
            2 => "MT63-2000 is slow but robust: about 7 times the airtime of AFSK (a short message takes ~10 s, every " +
                 "transmission adds ~5 s), but in simulation it decoded at -4 dB SNR, where AFSK needs about +6 dB. " +
                 "For weak, noisy or fading paths. Occupies 500-2500 Hz; fldigi can tune it in but shows the data as gibberish.",
            3 => "PSK 2400 (QPSK, 1200 baud at 1700 Hz): about twice the throughput of AFSK, and in simulation about as robust " +
                 "(reliable down to ~6 dB SNR). Each frame carries Reed-Solomon error correction. PSK 2400 and 3600 stations hear each other.",
            4 => "PSK 3600 (8-PSK): the fastest choice, about 2.4 times the throughput of AFSK, but it needs a strong, clean " +
                 "signal (in simulation, reliable only above ~10 dB SNR). Drop to PSK 2400 if messages need retries.",
            _ => "AFSK 1200: simple, and interoperable with packet-radio tools.",
        };
    }

    static void Fill(ComboBox box, List<string> items, string defaultLabel, string current)
    {
        box.Items.Add(defaultLabel);
        foreach (var i in items) box.Items.Add(i);
        box.Text = string.IsNullOrWhiteSpace(current) ? defaultLabel : current;
    }

    static string Pick(ComboBox box, string defaultLabel)
    {
        string t = (box.Text ?? "").Trim();
        return t == defaultLabel ? "" : t;
    }

    static void Row(StackPanel p, string label, Control c) => Row(p, new TextBlock { Text = label }, c);

    static void Row(StackPanel p, TextBlock label, Control c)
    {
        label.Margin = new Thickness(0, 8, 0, 2);
        p.Children.Add(label);
        p.Children.Add(c);
    }

    void UpdateLabels()
    {
        ttlLabel.Text = $"Max relay hops for my messages: {(int)ttl.Value}";
        preLabel.Text = $"PTT lead-in: {(int)pre.Value} ms";
        manualLabel.Text = $"Manual PTT countdown: {(int)manualSecs.Value} s";
        txLabel.Text = $"TX audio level: {tx.Value:0.00}   (use Test / Calibrate)";
        busyLabel.Text = $"Busy threshold (RMS): {busy.Value:0.0000}";
    }

    void Save()
    {
        s.Callsign = call.Text.Trim().ToUpperInvariant();
        if (!s.CallsignValid)
        {
            MessageBox.Show(this, "Enter your amateur callsign (3-8 letters, digits, / or -).", "HamLink");
            return;
        }
        s.Transport = link.SelectedIndex == 1 ? "tnc" : "audio";
        s.Modem = modem.SelectedIndex is >= 0 and <= 4 ? modem.SelectedIndex : 0;
        s.TncPort = Pick(tncPort, AutoPort);
        s.TncBaud = tncBaud.SelectedIndex == 1 ? 9600 : 1200;
        s.RadioInput = Pick(radioIn, Auto) == RadioDefault ? Devices.DefaultName : Pick(radioIn, Auto);
        s.RadioOutput = Pick(radioOut, Auto) == RadioDefault ? Devices.DefaultName : Pick(radioOut, Auto);
        s.VoiceInput = Pick(voiceIn, WinDefault);
        s.VoiceOutput = Pick(voiceOut, WinDefault);
        s.PttPort = Pick(pttPort, AutoPort);
        s.PttMode = SelectedPtt;
        s.ManualCountdown = (int)manualSecs.Value;
        s.CarrierSenseMode = Math.Max(0, sense.SelectedIndex);
        s.CarrierSense = s.CarrierSenseMode != 0;
        s.BitRepair = repair.IsChecked == true;
        s.RecordRx = record.IsChecked == true;
        s.AfskFec = FecChoices[Math.Max(0, fec.SelectedIndex)].Bytes;
        s.Ttl = (int)ttl.Value;
        s.PreambleMs = (int)pre.Value;
        s.TxLevel = (float)tx.Value;
        s.BusyThreshold = (float)busy.Value;
        s.AutoPlayVoice = auto.IsChecked == true;
        DialogResult = true;
    }
}
