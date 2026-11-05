// SPDX-License-Identifier: GPL-3.0-or-later
using System.Runtime.InteropServices;

namespace HamLink;

/// <summary>P/Invoke surface of hamlink_core.dll (core/hamlink_api.h). API version 10.</summary>
internal static class NativeMethods
{
    public const int ApiVersion = 11;
    const string Lib = "hamlink_core";

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void MessageCb(IntPtr user, [MarshalAs(UnmanagedType.LPStr)] string src,
        [MarshalAs(UnmanagedType.LPStr)] string dst, int type, IntPtr data, int len);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void StatusCb(IntPtr user, int msgId, int status);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void EventCb(IntPtr user, [MarshalAs(UnmanagedType.LPStr)] string text);

    // ---- monitor mode and listening on all modems (API v11) ----
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void MonitorCb(IntPtr user, [MarshalAs(UnmanagedType.LPStr)] string src,
        [MarshalAs(UnmanagedType.LPStr)] string dst, int type, int msgId, IntPtr data, int len);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_monitor(IntPtr ctx, int on);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_monitor_callback(IntPtr ctx, MonitorCb cb, IntPtr user);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_listen_all(IntPtr ctx, int on);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_rx_modem(IntPtr ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_station_id(IntPtr ctx, int on);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_api_version();

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr hl_create([MarshalAs(UnmanagedType.LPStr)] string callsign, int relay, int ttl,
        int preambleMs, uint seed, MessageCb onMsg, StatusCb onStatus, EventCb onEvent, IntPtr user);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_destroy(IntPtr ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_relay(IntPtr ctx, int on);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern float hl_process_audio(IntPtr ctx, short[] pcm, int n, long nowMs, float busyThreshold);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_send(IntPtr ctx, [MarshalAs(UnmanagedType.LPStr)] string dst, int type,
        byte[] data, int len, int ackReq, long nowMs);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr hl_poll_tx(IntPtr ctx, long nowMs, float amp, out int nSamples);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern long hl_next_wakeup(IntPtr ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_voice_encode(IntPtr ctx, short[] pcm8k, int n, byte[] outBuf, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_voice_decode(IntPtr ctx, byte[] clip, int len, short[] pcm8k, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_voice_max_clip_bytes();

    // ---- PTT test / calibration (API v2) ----
    public const int ToneOff = 0, Tone1200 = 1, Tone2200 = 2, ToneAlt = 3;

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_tone_start(IntPtr ctx, int mode);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern float hl_tone_fill(IntPtr ctx, short[] outBuf, int n, float amp);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_note_external_tx(IntPtr ctx, long nowMs, int durationMs);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_get_meter(IntPtr ctx, float[] out5);

    // ---- modem and link choice (API v3) ----
    public const int ModemAfsk = 0, ModemOfdm = 1, ModemMt63 = 2, ModemPsk2400 = 3, ModemPsk3600 = 4, LinkAudio = 0, LinkTnc = 1;

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_set_modem(IntPtr ctx, int modem);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_link(IntPtr ctx, int link, int tncBaud);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_tnc_rx(IntPtr ctx, byte[] bytes, int n, long nowMs);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr hl_tnc_poll_tx(IntPtr ctx, long nowMs, out int nBytes);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_get_stats(IntPtr ctx, uint[] out8);

    // ---- hand-operated PTT (API v4) ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_pending_tx(IntPtr ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_manual_ptt(IntPtr ctx, int on);

    // ---- reports, map, geo (API v5) ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_clock(IntPtr ctx, long unixSeconds, long nowMs);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_schema_count();

    /// <summary>Static NUL-terminated JSON owned by the DLL; read with Marshal.PtrToStringAnsi.</summary>
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern IntPtr hl_schema_json(int index);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_report_send(IntPtr ctx, [MarshalAs(UnmanagedType.LPStr)] string dst,
        [MarshalAs(UnmanagedType.LPStr)] string schema, [MarshalAs(UnmanagedType.LPStr)] string kv, int ackReq,
        long nowMs, byte[] err, int errCap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_report_summary(byte[] data, int len, byte[] outBuf, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_report_detail(byte[] data, int len, byte[] outBuf, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_map_set_self_position(IntPtr ctx, double lat, double lon, double radiusM, long nowMs);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_map_snapshot(IntPtr ctx, long nowMs, byte[] outBuf, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_map_detail(IntPtr ctx, [MarshalAs(UnmanagedType.LPStr)] string key, long nowMs, byte[] outBuf, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_map_clear(IntPtr ctx);

    /// <summary>Stateless: safe from any thread.</summary>
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_geo_parse([MarshalAs(UnmanagedType.LPUTF8Str)] string text, out double lat, out double lon,
        out double radiusM, out int kind);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_geo_format(double lat, double lon, int fmt, byte[] outBuf, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_nmea_feed(IntPtr ctx, byte[] bytes, int n, double[] out6);

    // ---- report repeats (API v6) ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_report_repeat(IntPtr ctx, int extraCopies, int intervalMs);

    // ---- receive DSP options (API v7) ----
    public const int CsOff = 0, CsData = 1, CsDataOrLevel = 2;

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_carrier_sense(IntPtr ctx, int mode);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_set_bit_repair(IntPtr ctx, int on);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_channel_busy(IntPtr ctx);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_get_rx_info(IntPtr ctx, uint[] out6);

    // ---- FX.25 error correction on the AFSK link (API v8) ----
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern int hl_set_fec(IntPtr ctx, int checkBytes);

    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
    public static extern void hl_get_fec_info(IntPtr ctx, uint[] out3);
}
