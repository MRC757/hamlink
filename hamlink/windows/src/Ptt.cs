// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Ports;

namespace HamLink;

/// <summary>
/// Keys the radio through the DigiRig's USB serial port (RTS or DTR). In "VOX" mode this does nothing
/// and the radio's own VOX must trigger. Note: Windows serial drivers may pulse DTR/RTS for a few ms
/// when a port is opened, which can blip the transmitter - open the port with the radio's antenna
/// connected to a dummy load or the radio off, or accept a very brief key-up at app start.
/// </summary>
internal sealed class Ptt : IDisposable
{
    readonly string mode;
    readonly string portName;
    SerialPort? port;

    public Ptt(string mode, string portName) { this.mode = mode; this.portName = portName; }

    public bool UsesSerial => mode == "RTS" || mode == "DTR";
    public bool Ready => !UsesSerial || port != null;

    /// <summary>Returns null on success, otherwise a message for the status line.</summary>
    public string? Open()
    {
        if (!UsesSerial) return "PTT: radio VOX (no serial line)";
        if (port != null) return null;
        string name = portName;
        if (string.IsNullOrEmpty(name))
        {
            var ports = SerialPort.GetPortNames();
            if (ports.Length == 1) name = ports[0];
            else return ports.Length == 0 ? "PTT: no COM port found (plug in the DigiRig)"
                                          : "PTT: choose the DigiRig COM port in Settings";
        }
        try
        {
            var p = new SerialPort(name, 9600, Parity.None, 8, StopBits.One)
            {
                Handshake = Handshake.None,
                RtsEnable = false,
                DtrEnable = false,
            };
            p.Open();
            p.RtsEnable = false;
            p.DtrEnable = false;
            port = p;
            return null;
        }
        catch (Exception e)
        {
            return $"PTT: cannot open {name}: {e.Message}";
        }
    }

    public void Key(bool on)
    {
        var p = port;
        if (p == null) return;
        try
        {
            if (mode == "RTS") p.RtsEnable = on;
            else if (mode == "DTR") p.DtrEnable = on;
        }
        catch
        {
            // adapter unplugged mid-use: drop the port so Open() is retried
            try { p.Dispose(); } catch { }
            port = null;
        }
    }

    public void Dispose()
    {
        Key(false);
        try { port?.Dispose(); } catch { }
        port = null;
    }
}
