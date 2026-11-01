// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbManager
import androidx.core.content.ContextCompat
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber

/**
 * Keys the radio through a USB serial adapter's RTS or DTR line (DigiRig Mobile exposes a
 * CP210x serial port for this). "VOX" mode does nothing here: the radio's own VOX must trigger.
 */
class Ptt(private val ctx: Context, private val line: String) {
    private var port: UsbSerialPort? = null
    private var permissionAsked = false

    val usesSerial get() = line == "RTS" || line == "DTR"
    val ready get() = !usesSerial || port != null

    /** Returns null on success, otherwise a human-readable reason. Safe to call repeatedly. */
    fun open(): String? {
        if (!usesSerial) return "PTT: radio VOX (no serial line)"
        if (port != null) return null
        val usb = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
        val driver = UsbSerialProber.getDefaultProber().findAllDrivers(usb).firstOrNull()
            ?: return "PTT: no USB serial adapter found (plug in the DigiRig)"
        if (!usb.hasPermission(driver.device)) {
            if (!permissionAsked) {
                permissionAsked = true
                val action = "org.hamlink.app.USB_PERMISSION"
                val receiver = object : BroadcastReceiver() {
                    override fun onReceive(c: Context, i: Intent) { permissionAsked = false; ctx.unregisterReceiver(this) }
                }
                ContextCompat.registerReceiver(ctx, receiver, IntentFilter(action), ContextCompat.RECEIVER_NOT_EXPORTED)
                val pi = PendingIntent.getBroadcast(
                    ctx, 0, Intent(action).setPackage(ctx.packageName), PendingIntent.FLAG_MUTABLE
                )
                usb.requestPermission(driver.device, pi)
            }
            return "PTT: waiting for USB permission - tap Allow"
        }
        val conn = usb.openDevice(driver.device) ?: return "PTT: could not open USB device"
        return try {
            val p = driver.ports[0]
            p.open(conn)
            p.setParameters(9600, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            p.setRTS(false)
            p.setDTR(false)
            port = p
            null
        } catch (e: Exception) {
            "PTT: ${e.message}"
        }
    }

    fun key(on: Boolean) {
        val p = port ?: return
        try {
            if (line == "RTS") p.setRTS(on) else if (line == "DTR") p.setDTR(on)
        } catch (_: Exception) {
            port = null // adapter unplugged; reopen on next loop
        }
    }

    fun close() {
        key(false)
        try { port?.close() } catch (_: Exception) {}
        port = null
    }
}
