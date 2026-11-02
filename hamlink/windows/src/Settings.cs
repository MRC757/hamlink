// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace HamLink;

internal sealed class Settings
{
    public string Callsign { get; set; } = "";
    public bool Relay { get; set; }
    /// <summary>Max relay hops for frames we originate (1 = through one simplex relay).</summary>
    public int Ttl { get; set; } = 1;
    /// <summary>Flags sent after PTT, to cover the radio's PTT-to-RF delay.</summary>
    public int PreambleMs { get; set; } = 500;
    /// <summary>Peak amplitude of transmitted audio, 0..1. Calibrate against radio deviation.</summary>
    public float TxLevel { get; set; } = 0.30f;
    /// <summary>RX RMS (0..1) above which the channel counts as busy.</summary>
    public float BusyThreshold { get; set; } = 0.004f;
    /// <summary>"RTS" / "DTR" (serial line), "VOX" (no PTT line: transmit at once; radio VOX or nothing at all) or
    /// "MANUAL" (you press the radio's PTT yourself when the app counts down).</summary>
    public string PttMode { get; set; } = "RTS";
    public string PttPort { get; set; } = "";
    /// <summary>Radio audio devices by name. Empty = first device whose name contains "USB".</summary>
    public string RadioInput { get; set; } = "";
    public string RadioOutput { get; set; } = "";
    /// <summary>PC mic / speakers for voice clips. Empty = Windows default. Never the radio interface.</summary>
    public string VoiceInput { get; set; } = "";
    public string VoiceOutput { get; set; } = "";
    public bool AutoPlayVoice { get; set; } = true;
    /// <summary>Seconds between pressing Transmit and the audio starting, in MANUAL PTT mode.</summary>
    public int ManualCountdown { get; set; } = 3;
    /// <summary>Hold off transmitting while the receive audio is loud. Turn off for acoustic tests where room noise keeps it busy.</summary>
    public bool CarrierSense { get; set; } = true;
    /// <summary>0 = off, 1 = only while data is detected (ignores room noise and voice: acoustic tests), 2 = data or loud
    /// audio (default). -1 = not set yet: taken from the older CarrierSense on/off value.</summary>
    public int CarrierSenseMode { get; set; } = -1;
    /// <summary>Repair AFSK frames with a single wrong bit (default on).</summary>
    public bool BitRepair { get; set; } = true;
    /// <summary>Save received radio audio to WAV files (Recorder.Folder) for troubleshooting and offline replay.</summary>
    public bool RecordRx { get; set; }
    /// <summary>FX.25 error correction on transmitted AFSK frames: 0 off, or 16/32/64 Reed-Solomon check bytes. Receiving is always on.</summary>
    public int AfskFec { get; set; } = 16;
    public int EffectiveCarrierSense => CarrierSenseMode is >= 0 and <= 2 ? CarrierSenseMode : (CarrierSense ? 2 : 0);
    /// <summary>0 = AFSK 1200 baud, 1 = OFDM (COFDMTV), 2 = MT63-2000, 3 = PSK 2400, 4 = PSK 3600. Every station on the
    /// channel must use the same one (PSK 2400 and 3600 stations hear each other).</summary>
    public int Modem { get; set; }
    /// <summary>"audio" = USB audio interface + PTT line; "tnc" = radio/TNC with KISS over a serial or paired-Bluetooth COM port.</summary>
    public string Transport { get; set; } = "audio";
    /// <summary>COM port of the KISS TNC. For a paired Bluetooth radio this is the OUTGOING virtual COM port.</summary>
    public string TncPort { get; set; } = "";
    /// <summary>Speed of the TNC (1200 or 9600); only used to estimate airtime for ACK timing.</summary>
    public int TncBaud { get; set; } = 1200;
    public string Dest { get; set; } = "CQ";

    // ---- position and map ----
    /// <summary>"typed" = lat/lon (decimal, DMS or MGRS) typed in; "grid" = Maidenhead grid square; "windows" = Windows
    /// location service (needs a GPS receiver for real accuracy); "serial" = NMEA GPS on a COM port.</summary>
    public string PositionMode { get; set; } = "typed";
    public string TypedPosition { get; set; } = "";
    public string GpsPort { get; set; } = "";
    public int GpsBaud { get; set; } = 4800;
    /// <summary>Minutes between automatic position beacons; 0 = off. Each beacon is a transmission.</summary>
    public int BeaconMin { get; set; }
    /// <summary>Extra copies of each position/SALUTE report we broadcast, about 30 s apart (0 = off, the default).</summary>
    public int ReportRepeat { get; set; }
    public const string DefaultTileUrl = "https://tile.openstreetmap.org/{z}/{x}/{y}.png";
    public const string DefaultAttribution = "\u00A9 OpenStreetMap contributors";
    public string TileUrl { get; set; } = DefaultTileUrl;
    public string TileAttribution { get; set; } = DefaultAttribution;
    /// <summary>Contact (email) sent in the tile request User-Agent; falls back to the callsign.</summary>
    public string TileContact { get; set; } = "";
    /// <summary>Off = use only tiles already on disk (air-gapped use).</summary>
    public bool TileOnline { get; set; } = true;
    public int TileCacheMb { get; set; } = 300;

    public bool CallsignValid => Regex.IsMatch(Callsign, "^[A-Z0-9/-]{3,8}$") && Callsign != "CQ";

    static string PathFile
    {
        get
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "HamLink");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "settings.json");
        }
    }

    public static Settings Load()
    {
        try
        {
            if (File.Exists(PathFile))
                return JsonSerializer.Deserialize<Settings>(File.ReadAllText(PathFile)) ?? new Settings();
        }
        catch { /* corrupt file: start from defaults */ }
        return new Settings();
    }

    public void Save()
    {
        Callsign = Callsign.Trim().ToUpperInvariant();
        Dest = Dest.Trim().ToUpperInvariant();
        Ttl = Math.Clamp(Ttl, 0, 3);
        Modem = Modem is >= 0 and <= 4 ? Modem : 0;
        AfskFec = AfskFec is 0 or 16 or 32 or 64 ? AfskFec : 16;
        ManualCountdown = Math.Clamp(ManualCountdown, 2, 10);
        Transport = Transport == "tnc" ? "tnc" : "audio";
        TncBaud = TncBaud >= 9600 ? 9600 : 1200;
        PreambleMs = Math.Clamp(PreambleMs, 200, 1500);
        TxLevel = Math.Clamp(TxLevel, 0.02f, 1f);
        BusyThreshold = Math.Clamp(BusyThreshold, 0.0005f, 0.2f);
        PositionMode = PositionMode is "grid" or "windows" or "serial" ? PositionMode : "typed";
        BeaconMin = BeaconMin is >= 1 and <= 120 ? BeaconMin : 0;
        GpsBaud = GpsBaud is 4800 or 9600 or 19200 or 38400 or 115200 ? GpsBaud : 4800;
        if (string.IsNullOrWhiteSpace(TileUrl)) TileUrl = DefaultTileUrl;
        if (string.IsNullOrWhiteSpace(TileAttribution)) TileAttribution = DefaultAttribution;
        TileCacheMb = Math.Clamp(TileCacheMb, 20, 4000);
        ReportRepeat = Math.Clamp(ReportRepeat, 0, 2);
        try { File.WriteAllText(PathFile, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true })); }
        catch { /* read-only profile: settings just won't persist */ }
    }
}
