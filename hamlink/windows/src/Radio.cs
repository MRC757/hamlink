// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;

namespace HamLink;

/// <summary>
/// Managed wrapper over one native context. NOT thread safe: use only from the engine thread.
/// Events fire synchronously on that thread, from inside ProcessAudio/Send/PollTx.
/// </summary>
internal sealed class Radio : IDisposable
{
    public const int TypeText = 1;
    public const int TypeVoice = 2;
    public const int TypeReport = 5;
    public const int StatusFailed = 0, StatusDelivered = 1, StatusSent = 2;

    IntPtr ctx;
    // Keep the delegates referenced for the lifetime of the native context, or the GC will free them.
    readonly NativeMethods.MessageCb onMsg;
    readonly NativeMethods.StatusCb onStatus;
    readonly NativeMethods.EventCb onEvent;

    public event Action<string, string, int, byte[]>? Message;   // src, dst, type, data
    public event Action<int, int>? Status;                       // msgId, status
    public event Action<string>? Event;                          // log line

    public Radio(string callsign, bool relay, int ttl, int preambleMs)
    {
        int v = NativeMethods.hl_api_version();
        if (v != NativeMethods.ApiVersion)
            throw new InvalidOperationException($"hamlink_core.dll API version {v}, app expects {NativeMethods.ApiVersion}");

        onMsg = (_, src, dst, type, p, n) =>
        {
            var data = new byte[Math.Max(n, 0)];
            if (n > 0) Marshal.Copy(p, data, 0, n);
            Message?.Invoke(src, dst, type, data);
        };
        onStatus = (_, id, st) => Status?.Invoke(id, st);
        onEvent = (_, text) => Event?.Invoke(text);

        uint seed = (uint)(Environment.TickCount64 ^ callsign.GetHashCode());
        ctx = NativeMethods.hl_create(callsign, relay ? 1 : 0, ttl, preambleMs, seed, onMsg, onStatus, onEvent, IntPtr.Zero);
        if (ctx == IntPtr.Zero) throw new InvalidOperationException("hl_create failed");
    }

    public void SetRelay(bool on) => NativeMethods.hl_set_relay(ctx, on ? 1 : 0);

    /// <summary>Feed 48 kHz mono audio (n may be 0 just to run timers). Returns RMS level 0..1.</summary>
    public float Process(short[] pcm, int n, long nowMs, float busyThreshold) =>
        NativeMethods.hl_process_audio(ctx, pcm, n, nowMs, busyThreshold);

    /// <summary>Message id (0..65535) or negative: -1 bad arg, -2 queue busy, -3 too big.</summary>
    public int Send(string dst, int type, byte[] data, bool ackReq, long nowMs) =>
        NativeMethods.hl_send(ctx, dst, type, data, data.Length, ackReq ? 1 : 0, nowMs);

    /// <summary>One PTT burst of 48 kHz audio if the channel is clear and something is due; else null.</summary>
    public short[]? PollTx(long nowMs, float amp)
    {
        IntPtr p = NativeMethods.hl_poll_tx(ctx, nowMs, amp, out int n);
        if (p == IntPtr.Zero || n <= 0) return null;
        var burst = new short[n];
        Marshal.Copy(p, burst, 0, n); // the native buffer is only valid until the next call
        return burst;
    }

    // ---- PTT test / calibration ----
    public const int ToneOff = NativeMethods.ToneOff, Tone1200 = NativeMethods.Tone1200,
                     Tone2200 = NativeMethods.Tone2200, ToneAlt = NativeMethods.ToneAlt;

    public void ToneStart(int mode) => NativeMethods.hl_tone_start(ctx, mode);

    /// <summary>Fills buf[0..n) toward amplitude amp. Returns the amplitude reached (0 once faded out).</summary>
    public float ToneFill(short[] buf, int n, float amp) => NativeMethods.hl_tone_fill(ctx, buf, n, amp);

    /// <summary>Tell the mesh we transmitted outside it; queues a station ID.</summary>
    public void NoteExternalTx(long nowMs, int durationMs) => NativeMethods.hl_note_external_tx(ctx, nowMs, durationMs);

    /// <summary>{rms dBFS, peak dBFS, 1200 Hz dBFS, 2200 Hz dBFS, clipped samples in last 100 ms}</summary>
    public void GetMeter(float[] out5) => NativeMethods.hl_get_meter(ctx, out5);

    // ---- modem and link choice ----
    public const int ModemAfsk = NativeMethods.ModemAfsk, ModemOfdm = NativeMethods.ModemOfdm, ModemMt63 = NativeMethods.ModemMt63,
                     ModemPsk2400 = NativeMethods.ModemPsk2400, ModemPsk3600 = NativeMethods.ModemPsk3600;
    public const int LinkAudio = NativeMethods.LinkAudio, LinkTnc = NativeMethods.LinkTnc;

    /// <summary>0 on success, -1 if OFDM or MT63 could not start (AFSK stays selected). All stations must use the same modem.</summary>
    public int SetModem(int modem) => NativeMethods.hl_set_modem(ctx, modem);
    public void SetLink(int link, int tncBaud) => NativeMethods.hl_set_link(ctx, link, tncBaud);

    /// <summary>TNC link: bytes read from the KISS stream (n may be 0 just to run timers).</summary>
    public void TncRx(byte[] bytes, int n, long nowMs) => NativeMethods.hl_tnc_rx(ctx, bytes, n, nowMs);

    /// <summary>TNC link: KISS bytes to write to the radio now, or null. Paced by estimated airtime.</summary>
    public byte[]? TncPollTx(long nowMs)
    {
        IntPtr p = NativeMethods.hl_tnc_poll_tx(ctx, nowMs, out int n);
        if (p == IntPtr.Zero || n <= 0) return null;
        var o = new byte[n];
        Marshal.Copy(p, o, 0, n); // native buffer is only valid until the next call
        return o;
    }

    /// <summary>Frames queued for transmission (ACKs, relays, IDs, fragments), due or not.</summary>
    public int PendingTx() => NativeMethods.hl_pending_tx(ctx);

    /// <summary>A human keys the radio for every transmission: ACK/retry timers are stretched to 45 s.</summary>
    public void SetManualPtt(bool on) => NativeMethods.hl_set_manual_ptt(ctx, on ? 1 : 0);

    /// <summary>{rx frames, dups, tx frames, relayed, suppressed, retries, ofdm ok, ofdm failed}</summary>
    public uint[] GetStats() { var v = new uint[8]; NativeMethods.hl_get_stats(ctx, v); return v; }

    public byte[]? VoiceEncode(short[] pcm8k)
    {
        var buf = new byte[NativeMethods.hl_voice_max_clip_bytes()];
        int n = NativeMethods.hl_voice_encode(ctx, pcm8k, pcm8k.Length, buf, buf.Length);
        if (n <= 0) return null;
        Array.Resize(ref buf, n);
        return buf;
    }

    public short[]? VoiceDecode(byte[] clip)
    {
        var pcm = new short[(clip.Length / 4 + 1) * 320];
        int n = NativeMethods.hl_voice_decode(ctx, clip, clip.Length, pcm, pcm.Length);
        if (n <= 0) return null;
        Array.Resize(ref pcm, n);
        return pcm;
    }

    public void Dispose()
    {
        if (ctx != IntPtr.Zero) { NativeMethods.hl_destroy(ctx); ctx = IntPtr.Zero; }
    }

    // ---- reports, map, geo (API v5) ----
    static string Ascii(byte[] b, int n) => n <= 0 ? "" : System.Text.Encoding.ASCII.GetString(b, 0, Math.Min(n, b.Length));
    static string CString(byte[] b) { int n = Array.IndexOf(b, (byte)0); return System.Text.Encoding.ASCII.GetString(b, 0, n < 0 ? b.Length : n); }

    public void SetClock(long unixSeconds, long nowMs) => NativeMethods.hl_set_clock(ctx, unixSeconds, nowMs);

    /// <summary>Build and send a report from key=value lines. Returns the message id (>= 0) or negative; err has the reason.</summary>
    public int ReportSend(string dst, string schema, string kv, bool ackReq, long nowMs, out string err)
    {
        var buf = new byte[200];
        int rc = NativeMethods.hl_report_send(ctx, dst, schema, kv, ackReq ? 1 : 0, nowMs, buf, buf.Length);
        err = CString(buf);
        if (rc < 0 && err.Length == 0) err = rc == -2 ? "send queue full, wait for pending messages" : "could not send";
        return rc;
    }

    public void MapSetSelf(double lat, double lon, double radiusM, long nowMs) =>
        NativeMethods.hl_map_set_self_position(ctx, lat, lon, radiusM, nowMs);

    /// <summary>Tab separated item lines (format: core/mapmodel.h).</summary>
    public string MapSnapshot(long nowMs)
    {
        var buf = new byte[32768];
        int n = NativeMethods.hl_map_snapshot(ctx, nowMs, buf, buf.Length);
        return Ascii(buf, Math.Min(n, buf.Length - 1));
    }

    public string? MapDetail(string key, long nowMs)
    {
        var buf = new byte[2048];
        int n = NativeMethods.hl_map_detail(ctx, key, nowMs, buf, buf.Length);
        return n > 0 ? Ascii(buf, Math.Min(n, buf.Length - 1)) : null;
    }

    public void MapClear() => NativeMethods.hl_map_clear(ctx);

    /// <summary>Send each broadcast report we originate extraCopies more times (0 = off); receivers drop the copies.</summary>
    public const int CsOff = NativeMethods.CsOff, CsData = NativeMethods.CsData, CsDataOrLevel = NativeMethods.CsDataOrLevel;
    /// <summary>0 off, 1 only while data is detected, 2 data or loud audio (default).</summary>
    public void SetCarrierSense(int mode) => NativeMethods.hl_set_carrier_sense(ctx, mode);
    public void SetBitRepair(bool on) => NativeMethods.hl_set_bit_repair(ctx, on ? 1 : 0);
    /// <summary>FX.25 on transmitted AFSK frames: 0 off, or 16/32/64 check bytes.</summary>
    public void SetFec(int checkBytes) => NativeMethods.hl_set_fec(ctx, checkBytes);
    /// <summary>{FX.25 blocks decoded, frames only FEC recovered, bytes corrected}</summary>
    public uint[] GetFecInfo() { var v = new uint[3]; NativeMethods.hl_get_fec_info(ctx, v); return v; }
    /// <summary>The carrier-sense decision from the last Process call.</summary>
    public bool ChannelBusy => NativeMethods.hl_channel_busy(ctx) != 0;
    /// <summary>{data detected now, AFSK frames decoded, repaired, rejected ambiguous, rejected by structure check, busy}</summary>
    public uint[] GetRxInfo() { var v = new uint[6]; NativeMethods.hl_get_rx_info(ctx, v); return v; }

    public void SetReportRepeat(int extraCopies, int intervalMs) => NativeMethods.hl_set_report_repeat(ctx, extraCopies, intervalMs);

    public static string? ReportSummary(byte[] data)
    {
        var buf = new byte[256];
        int n = NativeMethods.hl_report_summary(data, data.Length, buf, buf.Length);
        return n > 0 ? Ascii(buf, Math.Min(n, buf.Length - 1)) : null;
    }

    /// <summary>Feed raw NMEA bytes from a serial GPS. Returns true with {lat, lon} when a valid fix completed.</summary>
    public bool NmeaFeed(byte[] bytes, out double lat, out double lon)
    {
        var o = new double[6];
        bool ok = NativeMethods.hl_nmea_feed(ctx, bytes, bytes.Length, o) != 0;
        lat = o[0]; lon = o[1];
        return ok;
    }

    public static string? SchemaJson(int index)
    {
        IntPtr p = NativeMethods.hl_schema_json(index);
        return p == IntPtr.Zero ? null : Marshal.PtrToStringAnsi(p);
    }
}

/// <summary>Stateless coordinate helpers from the core (any thread).</summary>
internal static class Geo
{
    public const int Decimal = 0, Dms = 1, Maidenhead = 2, Mgrs = 3;

    /// <summary>Decimal, DMS, Maidenhead or MGRS text. kind: 0 lat/lon, 1 grid square, 2 MGRS. radiusM = uncertainty.</summary>
    public static bool Parse(string text, out double lat, out double lon, out double radiusM, out int kind)
    {
        lat = lon = radiusM = 0; kind = 0;
        if (string.IsNullOrWhiteSpace(text)) return false;
        return NativeMethods.hl_geo_parse(text.Trim(), out lat, out lon, out radiusM, out kind) != 0;
    }

    public static string Format(double lat, double lon, int fmt)
    {
        var buf = new byte[64];
        int n = NativeMethods.hl_geo_format(lat, lon, fmt, buf, buf.Length);
        return n > 0 ? System.Text.Encoding.ASCII.GetString(buf, 0, Math.Min(n, buf.Length - 1)) : "";
    }
}
