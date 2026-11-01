// SPDX-License-Identifier: GPL-3.0-or-later
using System.Collections.Concurrent;
using System.IO.Ports;

namespace HamLink;

/// <summary>
/// KISS byte stream to a TNC over a serial port. A radio paired over Bluetooth (BTECH UV-PRO / Vero VR-N76 with
/// KISS TNC enabled, or a Bluetooth KISS TNC) appears in Windows as a pair of virtual COM ports: choose the
/// OUTGOING one. Opening it makes Windows connect, which can take several seconds, so it happens on a
/// background thread and is retried with backoff (these radios power-save their classic Bluetooth).
/// DTR and RTS are deliberately left low so a wrongly chosen DigiRig port can never key the transmitter.
/// </summary>
internal sealed class TncLink : IDisposable
{
    readonly string portName;
    readonly ConcurrentQueue<byte[]> rx = new();
    SerialPort? port;
    volatile bool closed, connecting, up;
    long nextAttemptAt;
    long backoffMs = 2000;

    public TncLink(string portName) { this.portName = portName; }

    public bool Connected => up;
    public string Status { get; private set; } = "TNC: not connected";

    /// <summary>Call regularly from the engine loop; opens/reopens the port in the background when due.</summary>
    public void Service(long nowMs)
    {
        if (closed || up || connecting || nowMs < nextAttemptAt) return;
        string name = portName;
        if (string.IsNullOrWhiteSpace(name))
        {
            var ports = SerialPort.GetPortNames();
            if (ports.Length != 1) { Status = ports.Length == 0 ? "TNC: no COM port found (pair the radio / plug in the TNC)" : "TNC: choose the TNC's COM port in Settings"; return; }
            name = ports[0];
        }
        connecting = true;
        Status = $"TNC: opening {name}...";
        new Thread(() => Open(name)) { IsBackground = true, Name = "hamlink-tnc-open" }.Start();
    }

    void Open(string name)
    {
        try
        {
            var p = new SerialPort(name, 9600, Parity.None, 8, StopBits.One)
            {
                Handshake = Handshake.None,
                DtrEnable = false,
                RtsEnable = false,
                ReadTimeout = 200,
                WriteTimeout = 3000,
            };
            p.Open();
            if (closed) { p.Dispose(); return; }
            port = p;
            up = true;
            backoffMs = 2000;
            Status = $"TNC: connected on {name}";
            new Thread(() => ReadLoop(p)) { IsBackground = true, Name = "hamlink-tnc-read" }.Start();
        }
        catch (Exception e)
        {
            Status = $"TNC: cannot open {name}: {e.Message} (retrying; is KISS TNC enabled on the radio?)";
            nextAttemptAt = Environment.TickCount64 + backoffMs;
            backoffMs = Math.Min(backoffMs * 2, 30000);
        }
        finally { connecting = false; }
    }

    void ReadLoop(SerialPort p)
    {
        var buf = new byte[1024];
        try
        {
            while (!closed && p.IsOpen)
            {
                try
                {
                    int n = p.Read(buf, 0, buf.Length);
                    if (n > 0) { var c = new byte[n]; Array.Copy(buf, c, n); rx.Enqueue(c); }
                }
                catch (TimeoutException) { /* idle */ }
            }
        }
        catch (Exception) { /* port removed or link dropped */ }
        Drop("TNC: link lost (reconnecting)");
    }

    void Drop(string why)
    {
        up = false;
        try { port?.Dispose(); } catch { }
        port = null;
        if (!closed) { Status = why; nextAttemptAt = Environment.TickCount64 + 2000; }
    }

    public bool TryRead(out byte[] bytes) => rx.TryDequeue(out bytes!);

    /// <summary>Writes to the radio. Returns false (and starts reconnecting) if the link is down.</summary>
    public bool Write(byte[] data)
    {
        var p = port;
        if (p == null || !up) return false;
        try { p.Write(data, 0, data.Length); return true; }
        catch (Exception) { Drop("TNC: write failed (reconnecting)"); return false; }
    }

    public void Dispose()
    {
        closed = true;
        up = false;
        try { port?.Dispose(); } catch { }
        port = null;
        while (rx.TryDequeue(out _)) { }
    }
}
