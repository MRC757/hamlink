// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.SystemClock
import java.io.IOException
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import kotlin.math.min

/**
 * KISS byte stream to a radio's built-in TNC over classic Bluetooth, using the Serial Port Profile (SPP).
 * Works with radios that expose KISS over SPP, e.g. BTECH UV-PRO / Vero VR-N76 after enabling
 * Settings > General Settings > KISS TNC, and standalone Bluetooth KISS TNCs. The radio must be paired in
 * Android's Bluetooth settings first. The connection runs on background threads and retries with backoff,
 * because these radios power-save their classic Bluetooth between attempts and early attempts may time out.
 */
@SuppressLint("MissingPermission")
class BtTnc(private val ctx: Context, private val address: String) {
    companion object {
        private val SPP: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

        fun hasPermission(ctx: Context): Boolean =
            Build.VERSION.SDK_INT < 31 ||
                ctx.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

        private fun adapter(ctx: Context): BluetoothAdapter? =
            (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

        /** Paired devices as (name, address). Empty when Bluetooth is off or the permission is missing. */
        fun bonded(ctx: Context): List<Pair<String, String>> = try {
            if (!hasPermission(ctx)) emptyList()
            else adapter(ctx)?.bondedDevices.orEmpty().map { (it.name ?: "unnamed") to it.address }.sortedBy { it.first }
        } catch (_: SecurityException) { emptyList() }
    }

    @Volatile
    var connected = false
        private set
    /** One-line status for the UI. */
    @Volatile
    var status = "Bluetooth TNC: not connected"
        private set

    private val rx = LinkedBlockingQueue<ByteArray>()
    @Volatile private var socket: BluetoothSocket? = null
    @Volatile private var connecting = false
    @Volatile private var closed = false
    private var backoffMs = 2_000L
    private var nextAttemptAt = 0L

    /** Call regularly from the engine loop; starts a (re)connect in the background when due. */
    fun service(nowMs: Long) {
        if (closed || connected || connecting || nowMs < nextAttemptAt) return
        if (address.isBlank()) { status = "Bluetooth TNC: no radio selected (Settings)"; return }
        if (!hasPermission(ctx)) { status = "Bluetooth TNC: Bluetooth permission not granted"; return }
        connecting = true
        status = "Bluetooth TNC: connecting..."
        Thread({ connect() }, "hamlink-bt-connect").start()
    }

    private fun connect() {
        try {
            val ad = adapter(ctx) ?: throw IOException("this device has no Bluetooth")
            if (!ad.isEnabled) throw IOException("Bluetooth is off")
            ad.cancelDiscovery() // discovery slows and destabilises RFCOMM connects
            val s = ad.getRemoteDevice(address).createRfcommSocketToServiceRecord(SPP)
            try {
                s.connect() // blocks up to ~10 s
            } catch (e: IOException) {
                try { s.close() } catch (_: IOException) {}
                throw e
            }
            if (closed) { try { s.close() } catch (_: IOException) {}; return }
            socket = s
            connected = true
            backoffMs = 2_000L
            status = "Bluetooth TNC: connected"
            Thread({ readLoop(s) }, "hamlink-bt-read").start()
        } catch (e: Exception) { // IOException, SecurityException, IllegalArgumentException (bad address)
            status = "Bluetooth TNC: ${e.message ?: "connect failed"} (retrying; is KISS TNC enabled on the radio?)"
            nextAttemptAt = SystemClock.elapsedRealtime() + backoffMs
            backoffMs = min(backoffMs * 2, 30_000L)
        } finally {
            connecting = false
        }
    }

    private fun readLoop(s: BluetoothSocket) {
        val buf = ByteArray(1024)
        try {
            val input = s.inputStream
            while (!closed) {
                val n = input.read(buf)
                if (n < 0) break
                if (n > 0) rx.add(buf.copyOf(n))
            }
        } catch (_: IOException) {
        }
        dropLink("Bluetooth TNC: link lost (reconnecting)")
    }

    private fun dropLink(why: String) {
        connected = false
        try { socket?.close() } catch (_: IOException) {}
        socket = null
        if (!closed) {
            status = why
            nextAttemptAt = SystemClock.elapsedRealtime() + 2_000L
        }
    }

    /** Next chunk of bytes received from the radio, or null. */
    fun read(): ByteArray? = rx.poll()

    /** Writes to the radio. Returns false (and starts reconnecting) if the link is down. */
    fun write(bytes: ByteArray): Boolean {
        val s = socket ?: return false
        return try {
            s.outputStream.write(bytes)
            s.outputStream.flush()
            true
        } catch (_: IOException) {
            dropLink("Bluetooth TNC: write failed (reconnecting)")
            false
        }
    }

    fun close() {
        closed = true
        connected = false
        try { socket?.close() } catch (_: IOException) {}
        socket = null
        rx.clear()
    }
}
