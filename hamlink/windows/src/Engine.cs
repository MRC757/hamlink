// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.Concurrent;
using System.Text;
using NAudio.Wave;

namespace HamLink;

internal enum Phase { Off, Idle, Busy, Tx }
internal enum LogKind { RxText, TxText, RxVoice, TxVoice, Event, Error }

/// <summary>Receive meter snapshot, all in dBFS (a full-scale sine reads 0; floor -120).</summary>
internal readonly record struct MeterData(float RmsDb, float PeakDb, float Tone1200Db, float Tone2200Db, int Clipped);

/// <summary>
/// The radio engine. One thread owns the native context, capture queue, PTT and transmit.
/// Half duplex: while a burst is on the air the receive audio is discarded.
/// All events are raised on the engine thread; the UI must marshal to its dispatcher.
/// </summary>
internal sealed class Engine
{
    const int Fs = 48000;

    readonly Settings s;
    readonly ConcurrentQueue<Action> cmds = new();
    readonly ConcurrentQueue<short[]> rxq = new();
    readonly SemaphoreSlim rxSignal = new(0);
    Thread? thread;
    volatile bool running;
    volatile bool txActive;

    public event Action<LogKind, string, int, short[]?>? Log;   // kind, text, msgId (-1 none), voice clip
    public event Action<int, string>? StatusChanged;            // msgId, text
    public event Action<Phase, float>? PhaseChanged;
    public event Action<string, string>? Info;                  // audio route, PTT status
    public event Action<MeterData>? Meter;                      // ~10 Hz
    public event Action<bool, int>? TestState;                  // on air?, seconds left
    public event Action<string>? Mode;                          // one-line summary of link, audio, PTT, modem
    public event Action<IReadOnlyList<MapItem>>? MapUpdated;    // ~every 2 s while running
    public event Action<bool, string, int>? ManualState;        // manual mode?, what the operator should do now, frames waiting

    public Engine(Settings settings) { s = settings; }

    public bool IsRunning => running;
    static long Now() => Environment.TickCount64;

    public void Start()
    {
        if (running) return;
        running = true;
        thread = new Thread(Loop) { Name = "hamlink-engine", IsBackground = true };
        thread.Start();
    }

    public void Stop()
    {
        running = false;
        rxSignal.Release();
        thread?.Join(8000);
        thread = null;
    }

    // ------------------------------------------------------------------ manual PTT and live modem switch (UI -> engine)
    long manualArmAt;          // when the countdown ends and the burst is sent; 0 = not armed
    long stageClearAt;
    string manualStage = "";

    /// <summary>MANUAL PTT: start the countdown; when it ends the next due burst is played and the operator holds PTT.</summary>
    public void ManualGo() { if (Interlocked.Read(ref manualArmAt) == 0) Interlocked.Exchange(ref manualArmAt, Now() + s.ManualCountdown * 1000L); }
    public void ManualCancel() { Interlocked.Exchange(ref manualArmAt, 0); SetStage(""); }

    /// <summary>Switch modem while running (every station on the channel must use the same one). Audio link only.</summary>
    public void SetModem(int modem) => cmds.Enqueue(() =>
    {
        var r = radio; if (r == null || s.Transport == "tnc") return;
        int rc = r.SetModem(modem);
        if (rc != 0 && modem == Radio.ModemOfdm) Log?.Invoke(LogKind.Error, "OFDM modem could not start; using AFSK 1200", -1, null);
        else Log?.Invoke(LogKind.Event, "Modem: " + (modem == Radio.ModemOfdm ? "OFDM (COFDMTV)" : "AFSK 1200 baud"), -1, null);
        Mode?.Invoke(Summary(rc != 0 ? Radio.ModemAfsk : modem));
    });

    string audioKind = "";
    string Summary(int modem)
    {
        string ptt = s.PttMode switch { "MANUAL" => "manual PTT (you key the radio)", "VOX" => "no PTT line (transmit immediately)", _ => $"PTT via {s.PttMode}" };
        return $"{audioKind} · {ptt} · {(modem == Radio.ModemOfdm ? "OFDM" : "AFSK 1200")} · {(s.EffectiveCarrierSense switch { 0 => "carrier sense OFF", 1 => "carrier sense: data only", _ => "carrier sense on" })}";
    }

    volatile int pendingCache; // updated on the engine thread; the native context must not be touched from the UI thread

    void SetStage(string text)
    {
        manualStage = text;
        ManualState?.Invoke(s.PttMode == "MANUAL", text, pendingCache);
    }

    // ------------------------------------------------------------------ PTT test (UI -> engine)
    volatile int testRequest = -1;          // requested tone mode, -1 = none
    volatile bool testStopFlag;
    volatile float testAmp = 0.3f;

    /// <summary>Start a PTT test: keys the radio and sends a steady tone (or silence for ToneOff), at most 15 s.</summary>
    public void StartTest(int mode, float amp)
    {
        testAmp = Math.Clamp(amp, 0.02f, 1f);
        testStopFlag = false;
        testRequest = mode;
    }
    public void StopTest() { testStopFlag = true; testRequest = -1; }
    /// <summary>Live level change while a test tone is on the air.</summary>
    public void SetTestLevel(float amp) => testAmp = Math.Clamp(amp, 0.02f, 1f);

    // ------------------------------------------------------------------ UI -> engine
    Radio? radio; // touched only on the engine thread

    public void SetRelay(bool on) => cmds.Enqueue(() => radio?.SetRelay(on));

    public void SendText(string dst, string text) => cmds.Enqueue(() =>
    {
        var r = radio; if (r == null) return;
        var bytes = Encoding.UTF8.GetBytes(text);
        Queued(r.Send(dst, Radio.TypeText, bytes, dst != "CQ", Now()), LogKind.TxText, $"to {dst}: {text}", null);
    });

    public void SendVoice(string dst, short[] pcm8k) => cmds.Enqueue(() =>
    {
        var r = radio; if (r == null) return;
        var clip = r.VoiceEncode(pcm8k);
        if (clip == null) { Log?.Invoke(LogKind.Error, "voice encode failed", -1, null); return; }
        Queued(r.Send(dst, Radio.TypeVoice, clip, dst != "CQ", Now()), LogKind.TxVoice,
            $"voice to {dst} ({pcm8k.Length / 8000.0:0.0} s, {clip.Length} bytes)", pcm8k);
    });

    // ------------------------------------------------------------------ reports and map (UI -> engine)
    /// <summary>Build and send a report of any schema (see the schemas directory). done gets null on success or a readable error.</summary>
    public void SendReport(string schema, string kv, string dst, Action<string?> done) => cmds.Enqueue(() =>
    {
        var r = radio;
        if (r == null) { done("Start the radio first."); return; }
        int id = r.ReportSend(dst, schema, kv, dst != "CQ", Now(), out string err);
        if (id < 0) { done(err); return; }
        Log?.Invoke(LogKind.TxText, $"{schema} report sent to {dst}", id, null);
        StatusChanged?.Invoke(id, "queued");
        done(null);
    });

    /// <summary>Transmit our current position (whatever source is selected) as a position report.</summary>
    public void SendPosition() => cmds.Enqueue(() => SendPositionNow());

    void SendPositionNow()
    {
        var r = radio; if (r == null) return;
        var f = Positions.Current;
        if (f == null) { Log?.Invoke(LogKind.Error, "no position to send yet", -1, null); return; }
        string kv = string.Create(System.Globalization.CultureInfo.InvariantCulture,
            $"loc={f.Lat:0.000000},{f.Lon:0.000000}\ntime={DateTimeOffset.UtcNow.ToUnixTimeSeconds()}\nsource={f.Source}\n");
        int id = r.ReportSend("CQ", "POS", kv, false, Now(), out string err);
        if (id < 0) Log?.Invoke(LogKind.Error, "position not sent: " + err, -1, null);
        else { Log?.Invoke(LogKind.TxText, $"position sent ({f.Source})", id, null); StatusChanged?.Invoke(id, "queued"); }
    }

    /// <summary>Fetch the details text for a map icon (callback runs on the engine thread).</summary>
    public void RequestDetail(string key, Action<string?> done) => cmds.Enqueue(() => done(radio?.MapDetail(key, Now())));

    public void ClearMap() => cmds.Enqueue(() => { radio?.MapClear(); MapUpdated?.Invoke(Array.Empty<MapItem>()); });

    long nextMapAt, nextClockAt, beaconDueAt;
    Fix? lastSelf;

    /// <summary>Called every loop turn on the engine thread (audio and TNC modes): clock, serial GPS, own position, map, beacon.</summary>
    void Housekeeping(Radio r, long t)
    {
        if (t >= nextClockAt) // the PC's clock is the time base for report timestamps and icon ageing
        {
            nextClockAt = t + 300_000;
            r.SetClock(DateTimeOffset.UtcNow.ToUnixTimeSeconds(), t);
        }
        while (Positions.NmeaBytes.TryDequeue(out var nmea))
            if (r.NmeaFeed(nmea, out double la, out double lo)) Positions.Publish(new Fix(la, lo, 0, "GPS"), null);
        var f = Positions.Current;
        if (!ReferenceEquals(f, lastSelf)) { lastSelf = f; if (f != null) r.MapSetSelf(f.Lat, f.Lon, f.RadiusM, t); }
        if (t >= nextMapAt)
        {
            nextMapAt = t + 2000;
            MapUpdated?.Invoke(MapItem.Parse(r.MapSnapshot(t)));
        }
        int minutes = s.BeaconMin;
        if (minutes > 0 && f != null)
        {
            if (beaconDueAt == 0) beaconDueAt = t + 15_000;
            if (t >= beaconDueAt) { beaconDueAt = t + minutes * 60_000L; SendPositionNow(); }
        }
        else beaconDueAt = 0;
    }

    public const int RepeatIntervalMs = 30_000;

    /// <summary>Change the report repeat count while running.</summary>
    public void SetReportRepeat(int extra) => cmds.Enqueue(() => radio?.SetReportRepeat(extra, RepeatIntervalMs));

    void AfterStart(Radio r)
    {
        r.SetReportRepeat(s.ReportRepeat, RepeatIntervalMs);
        nextMapAt = nextClockAt = beaconDueAt = 0;
        lastSelf = null;
        Positions.Apply(s);
    }

    void Queued(int id, LogKind kind, string text, short[]? clip)
    {
        if (id < 0)
        {
            string why = id switch { -2 => "send queue full, wait for pending messages", -3 => "message too long", _ => $"error {id}" };
            Log?.Invoke(LogKind.Error, $"not sent: {why}", -1, null);
        }
        else
        {
            Log?.Invoke(kind, text, id, clip);
            StatusChanged?.Invoke(id, "queued");
        }
    }

    // ------------------------------------------------------------------ main loop
    void Loop()
    {
        Radio r;
        bool tnc = s.Transport == "tnc";
        try { r = new Radio(s.Callsign, s.Relay, s.Ttl, tnc ? 300 : s.PreambleMs); }
        catch (Exception e)
        {
            Log?.Invoke(LogKind.Error, $"cannot start: {e.Message}", -1, null);
            running = false;
            PhaseChanged?.Invoke(Phase.Off, 0);
            return;
        }
        radio = r;
        r.Message += OnMessage;
        r.Status += (id, st) => StatusChanged?.Invoke(id, st switch { 1 => "delivered", 2 => "sent", _ => "FAILED (no ACK)" });
        r.Event += t => Log?.Invoke(LogKind.Event, t, -1, null);

        if (!tnc && s.Modem == Radio.ModemOfdm && r.SetModem(Radio.ModemOfdm) != 0)
            Log?.Invoke(LogKind.Error, "OFDM modem could not start; using AFSK 1200", -1, null);
        r.SetLink(tnc ? Radio.LinkTnc : Radio.LinkAudio, s.TncBaud);
        AfterStart(r);
        if (tnc) { LoopTnc(r); return; }
        Log?.Invoke(LogKind.Event, "Modem: " + (s.Modem == Radio.ModemOfdm ? "OFDM (COFDMTV)" : "AFSK 1200 baud"), -1, null);
        bool manual = s.PttMode == "MANUAL";
        r.SetManualPtt(manual);
        float busyThr = s.BusyThreshold; // the carrier-sense mode decides whether level counts
        r.SetCarrierSense(s.EffectiveCarrierSense);
        r.SetBitRepair(s.BitRepair);
        int lastLeft = -1;

        var ptt = new Ptt(s.PttMode, s.PttPort);
        var ins = Devices.Inputs();
        var outs = Devices.Outputs();
        int inDev = Devices.Find(ins, s.RadioInput, preferUsb: true);
        int outDev = Devices.Find(outs, s.RadioOutput, preferUsb: true);
        string route;
        if (inDev == -2 || outDev == -2) route = "Audio: radio device NOT FOUND - check Settings";
        else if (inDev >= 0 && outDev >= 0) route = $"Audio: in \"{ins[inDev]}\"  out \"{outs[outDev]}\"";
        else if (s.RadioInput == Devices.DefaultName || s.RadioOutput == Devices.DefaultName)
            route = "Audio: Windows default speakers + microphone (acoustic coupling)";
        else route = "Audio: Windows default devices (no USB audio found) - set the DigiRig in Settings";
        string pttInfo = ptt.UsesSerial ? "PTT: opening..." : manual ? "PTT: manual (you key the radio)" : "PTT: none (transmit immediately)";
        Info?.Invoke(route, pttInfo);
        audioKind = inDev < 0 || outDev < 0 ? "Windows default speakers + mic (acoustic)"
            : Devices.LooksUsb(ins[inDev]) && Devices.LooksUsb(outs[outDev]) ? "USB audio interface"
            : "PC sound devices (no USB interface)";
        Mode?.Invoke(Summary(s.Modem));
        ManualState?.Invoke(manual, "", 0);

        WaveInEvent? cap = null;
        try
        {
            if (inDev == -2 || outDev == -2) throw new InvalidOperationException("audio device not found");
            cap = new WaveInEvent
            {
                DeviceNumber = inDev,
                WaveFormat = new WaveFormat(Fs, 16, 1),
                BufferMilliseconds = 20,
                NumberOfBuffers = 6,
            };
            cap.DataAvailable += (_, e) =>
            {
                if (txActive) return;
                int n = e.BytesRecorded / 2;
                if (n <= 0) return;
                var a = new short[n];
                Buffer.BlockCopy(e.Buffer, 0, a, 0, n * 2);
                rxq.Enqueue(a);
                rxSignal.Release();
            };
            cap.RecordingStopped += (_, e) =>
            {
                if (e.Exception != null) Log?.Invoke(LogKind.Error, $"audio capture stopped: {e.Exception.Message}", -1, null);
            };
            cap.StartRecording();
            PhaseChanged?.Invoke(Phase.Idle, 0);

            int tick = 0;
            float level = 0;
            var meterBuf = new float[5];
            while (running)
            {
                while (cmds.TryDequeue(out var c))
                {
                    try { c(); } catch (Exception ex) { Log?.Invoke(LogKind.Error, ex.Message, -1, null); }
                }
                Housekeeping(r, Now());

                if (!ptt.Ready && tick % 50 == 0)
                {
                    string? m = ptt.Open();
                    Info?.Invoke(route, m ?? $"PTT: {s.PttMode} on {(string.IsNullOrEmpty(s.PttPort) ? "auto COM port" : s.PttPort)} ready");
                }

                rxSignal.Wait(20);
                bool any = false;
                while (rxq.TryDequeue(out var chunk))
                {
                    level = r.Process(chunk, chunk.Length, Now(), busyThr);
                    any = true;
                }
                while (rxSignal.CurrentCount > 0) rxSignal.Wait(0);
                if (!any) level = r.Process(Array.Empty<short>(), 0, Now(), busyThr); // run timers

                if (tick % 5 == 0)
                {
                    PhaseChanged?.Invoke(r.ChannelBusy ? Phase.Busy : Phase.Idle, level);
                    r.GetMeter(meterBuf);
                    Meter?.Invoke(new MeterData(meterBuf[0], meterBuf[1], meterBuf[2], meterBuf[3], (int)meterBuf[4]));
                }

                int tm = testRequest;
                if (tm >= 0)
                {
                    testRequest = -1;
                    if (!ptt.Ready)
                        Log?.Invoke(LogKind.Error, "PTT test not started: PTT line is not ready (see PTT status)", -1, null);
                    else if (r.ChannelBusy)
                        Log?.Invoke(LogKind.Error, "PTT test not started: channel is busy, someone is transmitting", -1, null);
                    else
                        RunTest(r, ptt, tm, outDev);
                    tick++;
                    continue;
                }

                // Never pull a burst off the queue if we cannot key the radio; ARQ would have to recover it.
                if (manual)
                {
                    // Nothing is taken off the queue until the operator presses Transmit and the countdown ends.
                    long now = Now();
                    if (tick % 5 == 0) { pendingCache = r.PendingTx(); ManualState?.Invoke(true, manualStage, pendingCache); }
                    long arm = Interlocked.Read(ref manualArmAt);
                    if (arm != 0)
                    {
                        if (now >= arm)
                        {
                            Interlocked.Exchange(ref manualArmAt, 0);
                            var burst = r.PollTx(now, s.TxLevel);
                            if (burst == null)
                            {
                                SetStage("");
                                Log?.Invoke(LogKind.Event, "Nothing was ready to send (queue empty, channel busy, or waiting on a timer)", -1, null);
                            }
                            else
                            {
                                SetStage("TRANSMITTING: keep holding the radio's PTT");
                                Transmit(ptt, burst, outDev);
                                SetStage("Release the radio's PTT now");
                                stageClearAt = Now() + 2500;
                            }
                            lastLeft = -1;
                        }
                        else
                        {
                            int left = (int)((arm - now + 999) / 1000);
                            if (left != lastLeft) { lastLeft = left; SetStage($"Key the radio's PTT now and HOLD it. Audio starts in {left} s"); }
                        }
                    }
                    if (stageClearAt != 0 && now >= stageClearAt) { stageClearAt = 0; SetStage(""); }
                }
                else if (ptt.Ready)
                {
                    var burst = r.PollTx(Now(), s.TxLevel);
                    if (burst != null) Transmit(ptt, burst, outDev);
                }
                tick++;
            }
        }
        catch (Exception e)
        {
            App.LogError("engine", e);
            Log?.Invoke(LogKind.Error, $"engine stopped: {e.Message} (details in {App.LogPath})", -1, null);
        }
        finally
        {
            running = false;
            try { cap?.StopRecording(); } catch { }
            try { cap?.Dispose(); } catch { }
            ptt.Dispose(); // also unkeys
            r.Dispose();
            radio = null;
            while (rxq.TryDequeue(out _)) { }
            PhaseChanged?.Invoke(Phase.Off, 0);
        }
    }

    /// <summary>
    /// Serial/Bluetooth KISS-TNC mode: no audio and no PTT line. The radio's own modem and carrier sense do the
    /// RF work; we exchange KISS frames over a COM port. The core paces output by estimated airtime.
    /// </summary>
    void LoopTnc(Radio r)
    {
        var link = new TncLink(s.TncPort);
        string route = $"Link: KISS TNC on {(string.IsNullOrEmpty(s.TncPort) ? "auto COM port" : s.TncPort)} ({s.TncBaud} baud)";
        Info?.Invoke(route, link.Status);
        Mode?.Invoke($"KISS TNC · {s.TncBaud} baud · radio does keying and carrier sense");
        ManualState?.Invoke(false, "", 0);
        PhaseChanged?.Invoke(Phase.Idle, 0);
        Log?.Invoke(LogKind.Event, "KISS TNC mode: the radio does the modulation, keying and carrier sense", -1, null);
        string last = "";
        try
        {
            while (running)
            {
                while (cmds.TryDequeue(out var c))
                {
                    try { c(); } catch (Exception ex) { Log?.Invoke(LogKind.Error, ex.Message, -1, null); }
                }
                long t = Now();
                Housekeeping(r, t);
                link.Service(t);
                if (link.Status != last) { last = link.Status; Info?.Invoke(route, last); }
                bool got = false;
                while (link.TryRead(out var b)) { r.TncRx(b, b.Length, t); got = true; }
                if (!got) r.TncRx(Array.Empty<byte>(), 0, t); // run the ACK/retry/ID timers
                if (link.Connected)
                {
                    var o = r.TncPollTx(t);
                    if (o != null && !link.Write(o))
                        Log?.Invoke(LogKind.Error, "write to the radio failed; ARQ will retry", -1, null);
                }
                Thread.Sleep(20);
            }
        }
        catch (Exception e)
        {
            App.LogError("engine", e);
            Log?.Invoke(LogKind.Error, $"engine stopped: {e.Message} (details in {App.LogPath})", -1, null);
        }
        finally
        {
            running = false;
            link.Dispose();
            r.Dispose();
            radio = null;
            PhaseChanged?.Invoke(Phase.Off, 0);
        }
    }

    void OnMessage(string src, string dst, int type, byte[] data)
    {
        if (type == Radio.TypeText)
        {
            Log?.Invoke(LogKind.RxText, $"{src}: {Encoding.UTF8.GetString(data)}", -1, null);
        }
        else if (type == Radio.TypeReport)
        {
            // The core already put it on the map; the log gets the one-line summary. Unknown future types are ignored here.
            string? sum = Radio.ReportSummary(data);
            if (sum != null) Log?.Invoke(LogKind.RxText, $"{src} [report] {sum}", -1, null);
        }
        else if (type == Radio.TypeVoice)
        {
            var pcm = radio?.VoiceDecode(data);
            if (pcm == null) { Log?.Invoke(LogKind.Error, $"voice clip from {src} could not be decoded", -1, null); return; }
            Log?.Invoke(LogKind.RxVoice, $"voice from {src} ({pcm.Length / 8000.0:0.0} s)", -1, pcm);
            if (s.AutoPlayVoice && !s.Relay)
            {
                try { VoicePlayer.Play(pcm, VoiceOutputDevice()); }
                catch (Exception ex) { Log?.Invoke(LogKind.Error, $"voice playback failed: {ex.Message}", -1, null); }
            }
        }
    }

    /// <summary>Output device index for received voice clips: the chosen one, else Windows default (-1).</summary>
    int VoiceOutputDevice()
    {
        int d = Devices.Find(Devices.Outputs(), s.VoiceOutput, preferUsb: false);
        return d >= 0 ? d : -1;
    }

    /// <summary>Key PTT, play the pre-modulated burst, wait for it to drain, unkey (always).</summary>
    void Transmit(Ptt ptt, short[] burst, int outDev)
    {
        PhaseChanged?.Invoke(Phase.Tx, 0);
        txActive = true;
        double seconds = burst.Length / (double)Fs;
        var fmt = new WaveFormat(Fs, 16, 1);
        var prov = new BufferedWaveProvider(fmt)
        {
            BufferDuration = TimeSpan.FromSeconds(seconds + 2),
            ReadFully = false,
        };
        var bytes = new byte[burst.Length * 2];
        Buffer.BlockCopy(burst, 0, bytes, 0, bytes.Length);
        prov.AddSamples(bytes, 0, bytes.Length);

        using var done = new ManualResetEventSlim(false);
        WaveOutEvent? wo = null;
        try
        {
            wo = new WaveOutEvent { DeviceNumber = outDev, DesiredLatency = 100 };
            wo.PlaybackStopped += (_, _) => done.Set();
            wo.Init(prov);
            ptt.Key(true);
            wo.Play();
            // With ReadFully=false the provider returns 0 bytes when drained, which ends playback and
            // raises PlaybackStopped. If that never fires, fall back to the burst duration plus a margin.
            if (!done.Wait(TimeSpan.FromSeconds(seconds + 1.5)))
                Log?.Invoke(LogKind.Event, "playback-complete event not seen; keyed by timer", -1, null);
            Thread.Sleep(200); // let the final flags leave the USB codec and radio before dropping PTT
        }
        catch (Exception e)
        {
            Log?.Invoke(LogKind.Error, $"transmit failed: {e.Message}", -1, null);
        }
        finally
        {
            ptt.Key(false);
            try { wo?.Stop(); } catch { }
            try { wo?.Dispose(); } catch { }
            Thread.Sleep(150);
            while (rxq.TryDequeue(out _)) { } // drop anything captured while keyed
            txActive = false;
        }
    }

    /// <summary>
    /// PTT test / calibration tone: streams a tone with a live-adjustable level, auto-stops after 15 s,
    /// always unkeys, then asks the mesh to queue a station ID (a test is a transmission).
    /// </summary>
    void RunTest(Radio r, Ptt ptt, int mode, int outDev)
    {
        const int MaxMs = 15000;
        string label = mode switch { Radio.ToneOff => "PTT only", Radio.Tone1200 => "1200 Hz", Radio.Tone2200 => "2200 Hz", _ => "1200/2200 Hz alternating" };
        Log?.Invoke(LogKind.Event, $"TX test started: {label}", -1, null);
        PhaseChanged?.Invoke(Phase.Tx, 0);
        TestState?.Invoke(true, MaxMs / 1000);
        txActive = true;
        long t0 = Now();

        if (s.PttMode == "MANUAL")
        {
            for (int i = s.ManualCountdown; i >= 1; i--)
            {
                if (!running || testStopFlag) break;
                SetStage($"Key the radio's PTT now and HOLD it. Test tone starts in {i} s");
                Thread.Sleep(1000);
            }
            SetStage("TEST TONE ON AIR: keep holding the radio's PTT");
        }
        var prov = new BufferedWaveProvider(new WaveFormat(Fs, 16, 1))
        {
            BufferDuration = TimeSpan.FromSeconds(1),
            ReadFully = true, // play silence rather than ending if we are ever late with data
        };
        const int Chunk = 960; // 20 ms
        var samples = new short[Chunk];
        var bytes = new byte[Chunk * 2];
        WaveOutEvent? wo = null;
        try
        {
            wo = new WaveOutEvent { DeviceNumber = outDev, DesiredLatency = 100 };
            wo.Init(prov);
            r.ToneStart(mode);
            // pre-fill ~100 ms so the device starts with data
            for (int i = 0; i < 5; i++) Feed(r, prov, samples, bytes, Chunk);
            ptt.Key(true);
            wo.Play();

            int lastLeft = -1;
            while (running && !testStopFlag && Now() - t0 < MaxMs)
            {
                // keep roughly 100-140 ms queued: short enough that the level slider is heard quickly
                while (prov.BufferedDuration.TotalMilliseconds < 120) Feed(r, prov, samples, bytes, Chunk);
                Thread.Sleep(10);
                int left = (int)((MaxMs - (Now() - t0)) / 1000) + 1;
                if (left != lastLeft) { lastLeft = left; TestState?.Invoke(true, left); }
            }
            // fade out (about 5 ms per full scale), let the device drain, then a short margin
            int guard = 0;
            while (r.ToneFill(samples, Chunk, 0f) > 0f && guard++ < 20) { Buf(samples, bytes, Chunk); prov.AddSamples(bytes, 0, bytes.Length); }
            Array.Clear(samples); Buf(samples, bytes, Chunk); prov.AddSamples(bytes, 0, bytes.Length);
            long drainBy = Now() + 2000;
            while (prov.BufferedBytes > 0 && Now() < drainBy) Thread.Sleep(10);
            Thread.Sleep(250);
        }
        catch (Exception e)
        {
            Log?.Invoke(LogKind.Error, $"TX test failed: {e.Message}", -1, null);
        }
        finally
        {
            ptt.Key(false);
            try { wo?.Stop(); } catch { }
            try { wo?.Dispose(); } catch { }
            Thread.Sleep(150);
            while (rxq.TryDequeue(out _)) { }
            txActive = false;
            TestState?.Invoke(false, 0);
        }
        if (s.PttMode == "MANUAL") { SetStage("Release the radio's PTT now"); stageClearAt = Now() + 2500; }
        r.NoteExternalTx(Now(), 300);
        Log?.Invoke(LogKind.Event, $"TX test ended after {(Now() - t0) / 1000.0:0.0} s (station ID will follow)", -1, null);
    }

    void Feed(Radio r, BufferedWaveProvider prov, short[] samples, byte[] bytes, int n)
    {
        r.ToneFill(samples, n, testAmp);
        Buf(samples, bytes, n);
        prov.AddSamples(bytes, 0, bytes.Length);
    }

    static void Buf(short[] samples, byte[] bytes, int n) => Buffer.BlockCopy(samples, 0, bytes, 0, n * 2);
}
