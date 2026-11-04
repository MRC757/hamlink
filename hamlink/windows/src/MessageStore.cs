// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace HamLink;

/// <summary>Direction of a stored message: received, sent, or overheard between two other stations (monitor mode).</summary>
public enum MsgDir { In, Out, Heard }

/// <summary>
/// One message to or from a station, kept for the Stations page (docs/STATIONS_PAGE.md). Created on the engine thread,
/// added to <see cref="MessageStore"/> on the UI thread. Only <see cref="Status"/> changes afterwards.
/// </summary>
public sealed class MsgRecord : INotifyPropertyChanged
{
    public long T { get; init; }                 // unix milliseconds
    public MsgDir Dir { get; init; }
    public string From { get; init; } = "";
    public string To { get; init; } = "";        // a callsign or "CQ"
    public string Kind { get; init; } = "text";  // text | voice | report
    public string Text { get; init; } = "";      // the text, the report summary, or a voice clip description
    public string? Clip { get; init; }           // voice: the Codec2 clip, base64 (decoded when played)
    public int Id { get; init; } = -1;           // message ID of a sent message (16-bit, reused over time)
    string? status;

    public string? Status
    {
        get => status;
        set { status = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Status))); }
    }

    /// <summary>The other party, the station this message is listed under. Messages we sent to CQ list under "CQ".</summary>
    [JsonIgnore] public string Station => Dir == MsgDir.Out ? To : From;

    public event PropertyChangedEventHandler? PropertyChanged;
}

/// <summary>
/// The message history: every message sent, received or overheard in the last <see cref="RetentionDays"/> days, saved to
/// %LOCALAPPDATA%\HamLink\messages.jsonl. The file is append-only while running (one record or status line per
/// change) and is rewritten without expired lines at start-up. UI thread only.
/// </summary>
public static class MessageStore
{
    public const int RetentionDays = 5;
    public static ObservableCollection<MsgRecord> All { get; } = new();

    static readonly string Dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HamLink");
    public static string FilePath { get; } = Path.Combine(Dir, "messages.jsonl");
    static readonly JsonSerializerOptions Json = new() { DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };

    sealed class StatusLine { public string S { get; set; } = ""; public int Id { get; set; } public long T { get; set; } }

    /// <summary>Reads the history, drops what has expired, and rewrites the file compactly. Never throws.</summary>
    public static void Load()
    {
        All.Clear();
        try
        {
            if (!File.Exists(FilePath)) return;
            long cutoff = DateTimeOffset.UtcNow.AddDays(-RetentionDays).ToUnixTimeMilliseconds();
            var list = new List<MsgRecord>();
            foreach (var line in File.ReadLines(FilePath))
            {
                if (string.IsNullOrWhiteSpace(line)) continue;
                try
                {
                    using var doc = JsonDocument.Parse(line);
                    if (doc.RootElement.TryGetProperty("S", out _))
                    {
                        var st = doc.RootElement.Deserialize<StatusLine>(Json);
                        if (st != null && Latest(list, st.Id) is { } target) target.Status = st.S;
                    }
                    else
                    {
                        var r = doc.RootElement.Deserialize<MsgRecord>(Json);
                        if (r != null && r.T >= cutoff) list.Add(r);
                    }
                }
                catch (JsonException) { /* one damaged line (power cut while writing) never loses the rest */ }
            }
            foreach (var r in list) All.Add(r);
            Rewrite();
        }
        catch (Exception e) { App.LogError("message history", e); }
    }

    /// <summary>Adds a message and appends it to the file.</summary>
    public static void Add(MsgRecord r)
    {
        All.Add(r);
        Append(JsonSerializer.Serialize(r, Json));
    }

    /// <summary>A status change ("queued", "sent", "delivered", "FAILED ...") for the newest sent message with this ID.</summary>
    public static void SetStatus(int id, string status)
    {
        var r = Latest(All, id);
        if (r == null || r.Status == status) return;
        r.Status = status;
        Append(JsonSerializer.Serialize(new StatusLine { S = status, Id = id, T = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() }, Json));
    }

    /// <summary>Deletes the whole history (Settings, "Clear message history").</summary>
    public static void Clear()
    {
        All.Clear();
        try { File.Delete(FilePath); } catch (Exception e) { App.LogError("message history", e); }
    }

    static MsgRecord? Latest(IList<MsgRecord> list, int id)
    {
        if (id < 0) return null;
        for (int i = list.Count - 1; i >= 0; i--)
            if (list[i].Dir == MsgDir.Out && list[i].Id == id) return list[i];
        return null;
    }

    static void Append(string line)
    {
        try
        {
            Directory.CreateDirectory(Dir);
            File.AppendAllText(FilePath, line + "\n");
        }
        catch (Exception e) { App.LogError("message history", e); }
    }

    static void Rewrite()
    {
        string tmp = FilePath + ".tmp";
        using (var w = new StreamWriter(tmp))
        {
            foreach (var r in All) w.WriteLine(JsonSerializer.Serialize(r, Json)); // each record carries its latest status
        }
        File.Move(tmp, FilePath, overwrite: true);
    }
}
