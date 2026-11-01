// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using NAudio.Wave;

namespace HamLink;

/// <summary>WinMM (MME) device lookup. MME is used on purpose: Windows converts sample rate and
/// channel count for us, so a USB codec that only offers 44.1/48 kHz still works at 48 kHz mono.</summary>
internal static class Devices
{
    /// <summary>Setting value meaning "the Windows default recording/playback device".</summary>
    public const string DefaultName = "DEFAULT";

    public static List<string> Inputs()
    {
        var l = new List<string>();
        for (int i = 0; i < WaveInEvent.DeviceCount; i++) l.Add(WaveInEvent.GetCapabilities(i).ProductName);
        return l;
    }

    public static List<string> Outputs()
    {
        var l = new List<string>();
        for (int i = 0; i < WaveOut.DeviceCount; i++) l.Add(WaveOut.GetCapabilities(i).ProductName);
        return l;
    }

    /// <summary>Index of the named device (exact, then substring). If name is empty: the first device that
    /// looks like a USB codec when preferUsb, else -1 (Windows default / WAVE_MAPPER). Returns -2 if a
    /// requested name is not present.</summary>
    public static int Find(List<string> names, string name, bool preferUsb)
    {
        if (name == DefaultName) return -1; // explicit "use the Windows default device" (acoustic coupling)
        if (!string.IsNullOrWhiteSpace(name))
        {
            int i = names.FindIndex(n => n.Equals(name, StringComparison.OrdinalIgnoreCase));
            if (i < 0) i = names.FindIndex(n => n.Contains(name, StringComparison.OrdinalIgnoreCase)
                                             || name.Contains(n, StringComparison.OrdinalIgnoreCase));
            return i >= 0 ? i : -2;
        }
        if (!preferUsb) return -1;
        int u = names.FindIndex(LooksUsb);
        return u >= 0 ? u : -1;
    }

    /// <summary>True for device names that look like a USB radio interface (DigiRig and other USB codecs).</summary>
    public static bool LooksUsb(string name) =>
        name.Contains("USB", StringComparison.OrdinalIgnoreCase) || name.Contains("DigiRig", StringComparison.OrdinalIgnoreCase);
}

/// <summary>Records the PC's own microphone at 8 kHz (never the radio link) while held.</summary>
internal sealed class VoiceRecorder
{
    const int Rate = 8000;
    readonly int maxSamples;
    readonly object gate = new();
    short[] buf;
    int n;
    WaveInEvent? w;

    public VoiceRecorder(int maxSeconds = 20) { maxSamples = Rate * maxSeconds; buf = new short[maxSamples]; }

    public void Start(int device)
    {
        lock (gate) n = 0;
        w = new WaveInEvent { DeviceNumber = device, WaveFormat = new WaveFormat(Rate, 16, 1), BufferMilliseconds = 50 };
        w.DataAvailable += (_, e) =>
        {
            lock (gate)
            {
                int take = Math.Min(e.BytesRecorded / 2, maxSamples - n);
                if (take > 0) { Buffer.BlockCopy(e.Buffer, 0, buf, n * 2, take * 2); n += take; }
            }
        };
        w.StartRecording();
    }

    public short[] Stop()
    {
        var x = w;
        w = null;
        try { x?.StopRecording(); } catch { }
        Thread.Sleep(80); // let the last buffer arrive
        try { x?.Dispose(); } catch { }
        lock (gate) { var r = new short[n]; Array.Copy(buf, r, n); return r; }
    }
}

internal static class VoicePlayer
{
    /// <summary>Plays 8 kHz mono PCM on the given output device (-1 = Windows default). Fire and forget.</summary>
    public static void Play(short[] pcm, int device)
    {
        var bytes = new byte[pcm.Length * 2];
        Buffer.BlockCopy(pcm, 0, bytes, 0, bytes.Length);
        var src = new RawSourceWaveStream(new MemoryStream(bytes), new WaveFormat(8000, 16, 1));
        var wo = new WaveOutEvent { DeviceNumber = device };
        wo.PlaybackStopped += (_, _) => { wo.Dispose(); src.Dispose(); };
        wo.Init(src);
        wo.Play();
    }
}
