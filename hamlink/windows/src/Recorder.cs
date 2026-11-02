// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using NAudio.Wave;

namespace HamLink;

/// <summary>Writes received radio audio to WAV files for troubleshooting and offline replay (tools/replay_wav.py).
/// 48 kHz mono 16-bit, about 350 MB per hour; a new file every 30 minutes; the folder is kept under 1 GB by deleting the
/// oldest files. Audio is not captured while we transmit, so files skip those moments.</summary>
internal sealed class Recorder : IDisposable
{
    const long MaxFolderBytes = 1L << 30;
    static readonly TimeSpan FileLength = TimeSpan.FromMinutes(30);

    public static string Folder { get; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HamLink", "recordings");

    readonly int rate;
    WaveFileWriter? writer;
    DateTime started;

    public Recorder(int sampleRate) { rate = sampleRate; }

    /// <summary>Path of the file being written, or null.</summary>
    public string? CurrentFile { get; private set; }

    /// <summary>Append samples; opens a new file at the start and every 30 minutes. Returns the path of a newly opened file.</summary>
    public string? Write(short[] pcm, int n)
    {
        string? opened = null;
        if (writer == null || DateTime.UtcNow - started > FileLength)
        {
            writer?.Dispose();
            Directory.CreateDirectory(Folder);
            Prune();
            CurrentFile = Path.Combine(Folder, $"rx-{DateTime.Now:yyyyMMdd-HHmmss}.wav");
            writer = new WaveFileWriter(CurrentFile, new WaveFormat(rate, 16, 1));
            started = DateTime.UtcNow;
            opened = CurrentFile;
        }
        writer.WriteSamples(pcm, 0, n);
        return opened;
    }

    static void Prune()
    {
        var files = new DirectoryInfo(Folder).GetFiles("rx-*.wav").OrderBy(f => f.CreationTimeUtc).ToList();
        long total = files.Sum(f => f.Length);
        foreach (var f in files)
        {
            if (total <= MaxFolderBytes) break;
            try { total -= f.Length; f.Delete(); } catch (IOException) { }
        }
    }

    public void Dispose()
    {
        writer?.Dispose();
        writer = null;
    }
}
