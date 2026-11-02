// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;
using NAudio.CoreAudioApi;
using NAudio.CoreAudioApi.Interfaces;
using NAudio.Wave;

namespace HamLink;

/// <summary>
/// Receive audio from the radio interface as 48 kHz mono 16-bit blocks. Prefers WASAPI in RAW mode, which asks Windows
/// to skip its audio effects (noise suppression, "Voice clarity", automatic gain) the way Android's UNPROCESSED source
/// does; those effects treat data signals as noise. Devices or drivers that refuse raw mode fall back to plain WASAPI,
/// then to WinMM. Hardware noise cancelling inside a USB cable cannot be bypassed by any of these.
/// </summary>
internal abstract class RadioCapture : IDisposable
{
    /// <summary>A block of samples, on a capture thread.</summary>
    public event Action<short[]>? Samples;
    /// <summary>Capture stopped because of an error, on a capture thread.</summary>
    public event Action<Exception>? Failed;
    /// <summary>One line for the log: which path is in use.</summary>
    public abstract string Description { get; }
    public abstract void Start();
    public abstract void Dispose();
    protected void Emit(short[] a) => Samples?.Invoke(a);
    protected void Fail(Exception e) => Failed?.Invoke(e);

    /// <summary>Opens the device named as in WinMM (<paramref name="mmeName"/>, null = Windows default) at
    /// <paramref name="fs"/> Hz mono; <paramref name="mmeIndex"/> is the WinMM fallback.</summary>
    public static RadioCapture Open(string? mmeName, int mmeIndex, int fs)
    {
        try
        {
            if (WasapiRadioCapture.FindDevice(mmeName) is { } dev) return new WasapiRadioCapture(dev, fs);
        }
        catch (Exception e) { App.LogError("WASAPI capture, using WinMM instead", e); }
        return new MmeRadioCapture(mmeIndex, fs);
    }
}

internal sealed class MmeRadioCapture : RadioCapture
{
    readonly WaveInEvent w;
    public override string Description => "Audio input: standard Windows path (turn off Audio enhancements for the radio interface)";

    public MmeRadioCapture(int device, int fs)
    {
        w = new WaveInEvent { DeviceNumber = device, WaveFormat = new WaveFormat(fs, 16, 1), BufferMilliseconds = 20, NumberOfBuffers = 6 };
        w.DataAvailable += (_, e) =>
        {
            int n = e.BytesRecorded / 2;
            if (n <= 0) return;
            var a = new short[n];
            Buffer.BlockCopy(e.Buffer, 0, a, 0, n * 2);
            Emit(a);
        };
        w.RecordingStopped += (_, e) => { if (e.Exception != null) Fail(e.Exception); };
    }

    public override void Start() => w.StartRecording();
    public override void Dispose() { try { w.StopRecording(); } catch { } w.Dispose(); }
}

internal sealed class WasapiRadioCapture : RadioCapture
{
    // IAudioClient2, declared here with its full vtable: the 12 IAudioClient methods are skipped with _VtblGap.
    [ComImport, Guid("726778CD-F60A-4eda-82DE-E47610CD78AA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioClient2Raw
    {
        void _VtblGap1_12();
        [PreserveSig] int IsOffloadCapable(int category, out int offloadCapable);
        [PreserveSig] int SetClientProperties(ref ClientProperties properties);
    }

    [StructLayout(LayoutKind.Sequential)]
    struct ClientProperties { public uint cbSize; public int bIsOffload; public int eCategory; public int options; }
    const int AudioCategoryOther = 0, StreamOptionRaw = 1;

    readonly MMDevice dev;
    readonly AudioClient client;
    readonly bool raw;
    readonly Thread thread;
    volatile bool running;

    public override string Description => raw
        ? $"Audio input: raw (Windows audio effects bypassed) from \"{dev.FriendlyName}\""
        : $"Audio input: \"{dev.FriendlyName}\" does not offer raw audio; turn off its Audio enhancements in Windows";

    /// <summary>The active capture endpoint whose name WinMM shortened to <paramref name="mmeName"/> (31 characters).</summary>
    public static MMDevice? FindDevice(string? mmeName)
    {
        var en = new MMDeviceEnumerator();
        if (mmeName == null) return en.GetDefaultAudioEndpoint(DataFlow.Capture, Role.Console);
        var all = en.EnumerateAudioEndPoints(DataFlow.Capture, DeviceState.Active).ToList();
        return all.FirstOrDefault(d => d.FriendlyName.Equals(mmeName, StringComparison.OrdinalIgnoreCase))
            ?? all.FirstOrDefault(d => d.FriendlyName.StartsWith(mmeName, StringComparison.OrdinalIgnoreCase));
    }

    public WasapiRadioCapture(MMDevice device, int fs)
    {
        dev = device;
        client = dev.AudioClient;
        // Raw mode must be set before Initialize. NAudio keeps the COM object in a private field.
        var com = typeof(AudioClient).GetField("audioClientInterface", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)
            ?.GetValue(client);
        if (com is IAudioClient2Raw c2)
        {
            var p = new ClientProperties { cbSize = (uint)Marshal.SizeOf<ClientProperties>(), eCategory = AudioCategoryOther, options = StreamOptionRaw };
            raw = c2.SetClientProperties(ref p) == 0;
        }
        // Shared mode; Windows converts the device's own format (often 48 kHz stereo float) to 48 kHz mono 16-bit for us.
        client.Initialize(AudioClientShareMode.Shared, AudioClientStreamFlags.AutoConvertPcm | AudioClientStreamFlags.SrcDefaultQuality,
            2_000_000 /* 200 ms buffer, in 100 ns units */, 0, new WaveFormat(fs, 16, 1), Guid.Empty);
        thread = new Thread(Loop) { IsBackground = true, Name = "hamlink-capture", Priority = ThreadPriority.AboveNormal };
    }

    public override void Start() { running = true; client.Start(); thread.Start(); }

    void Loop()
    {
        try
        {
            var cap = client.AudioCaptureClient;
            while (running)
            {
                Thread.Sleep(10);
                int packet;
                while (running && (packet = cap.GetNextPacketSize()) > 0)
                {
                    IntPtr buf = cap.GetBuffer(out int frames, out AudioClientBufferFlags flags);
                    var a = new short[frames];
                    if ((flags & AudioClientBufferFlags.Silent) == 0) Marshal.Copy(buf, a, 0, frames);
                    cap.ReleaseBuffer(frames);
                    if (frames > 0) Emit(a);
                }
            }
        }
        catch (Exception e) { if (running) Fail(e); }
    }

    public override void Dispose()
    {
        running = false;
        try { thread.Join(500); } catch { }
        try { client.Stop(); } catch { }
        client.Dispose();
        dev.Dispose();
    }
}
