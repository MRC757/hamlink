// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Looper
import androidx.core.content.ContextCompat

/** A position and where it came from. [source] is the exact name of a choice in schemas/position.json. */
data class Fix(val lat: Double, val lon: Double, val radiusM: Double, val source: String, val timeMs: Long)

/** Anything that can say where we are. Add a new source (serial NMEA, a sensor...) by implementing this. */
interface PositionProvider {
    fun start(onFix: (Fix) -> Unit)
    fun stop()
}

/** Phone GPS. Updates arrive on the main thread. */
class GpsProvider(private val ctx: Context) : PositionProvider, LocationListener {
    private var cb: ((Fix) -> Unit)? = null
    private val lm get() = ctx.getSystemService(Context.LOCATION_SERVICE) as LocationManager

    companion object {
        fun permitted(ctx: Context) =
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
    }

    @SuppressLint("MissingPermission")
    override fun start(onFix: (Fix) -> Unit) {
        cb = onFix
        if (!permitted(ctx)) return
        try {
            lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)?.let { onLocationChanged(it) }
            lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 5000L, 5f, this, Looper.getMainLooper())
        } catch (_: SecurityException) {
        } catch (_: IllegalArgumentException) { // no GPS provider on this device
        }
    }

    override fun stop() {
        try { lm.removeUpdates(this) } catch (_: Exception) {}
        cb = null
    }

    override fun onLocationChanged(l: Location) {
        cb?.invoke(Fix(l.latitude, l.longitude, if (l.hasAccuracy()) l.accuracy.toDouble() else 0.0, "GPS", System.currentTimeMillis()))
    }
    override fun onProviderEnabled(provider: String) {}
    override fun onProviderDisabled(provider: String) {}
}

/** A position the operator typed: decimal degrees, DMS, MGRS, or a Maidenhead grid square. */
object TypedPosition {
    /** Null if the text cannot be read. The Fix source names the choice in the position report schema. */
    fun parse(text: String, wantGrid: Boolean): Fix? {
        val t = text.trim()
        if (t.isEmpty()) return null
        val r = Native.geoParse(t) ?: return null
        val kind = r[3].toInt()
        if (wantGrid != (kind == 1)) return null
        val source = when {
            kind != 1 -> "Typed coordinates"
            t.length >= 8 -> "Grid square (8 char)"
            t.length >= 6 -> "Grid square (6 char)"
            else -> "Grid square (4 char)"
        }
        return Fix(r[0], r[1], r[2], source, System.currentTimeMillis())
    }
}

/** Chooses the active source from Settings and publishes the current fix to the UI and to the radio engine. */
object Positions {
    @Volatile var current: Fix? = null
        private set
    private var gps: GpsProvider? = null

    /** Call at app start, after a settings change, and when the radio service starts. Safe to call repeatedly. */
    @Synchronized
    fun apply(ctx: Context) {
        val app = ctx.applicationContext
        val s = Settings(app)
        gps?.stop(); gps = null
        when (s.positionMode) {
            "gps" -> {
                if (!GpsProvider.permitted(app)) {
                    publish(null, "GPS: location permission needed")
                } else {
                    if (current?.source != "GPS") publish(null, "GPS: waiting for a fix (go outside or near a window)")
                    gps = GpsProvider(app).also { it.start { f -> publish(f, null) } }
                }
            }
            else -> {
                val f = TypedPosition.parse(s.typedPosition, s.positionMode == "grid")
                publish(f, if (f == null) (if (s.positionMode == "grid") "Grid square: type a locator such as FN31pr" else "Typed position: type coordinates") else null)
            }
        }
    }

    @Synchronized
    fun stop() { gps?.stop(); gps = null }

    private fun publish(f: Fix?, status: String?) {
        current = f
        Hub.ui {
            Hub.position = f
            Hub.positionStatus = status ?: f?.let { describe(it) } ?: "No position yet"
            if (f != null && !Hub.camPlaced) { Hub.camLat = f.lat; Hub.camLon = f.lon; Hub.camZoom = 12f; Hub.camPlaced = true }
        }
        if (f != null) RadioService.engine?.post { Native.mapSetSelf(f.lat, f.lon, f.radiusM, android.os.SystemClock.elapsedRealtime()) }
    }

    fun describe(f: Fix): String {
        val grid = Native.geoFormat(f.lat, f.lon, 2)
        val src = if (f.source == "GPS") "GPS" else if (f.source.startsWith("Grid")) "Grid square" else "Typed"
        val acc = if (f.radiusM >= 1000) " ±%.0f km".format(f.radiusM / 1000) else if (f.radiusM >= 1) " ±%.0f m".format(f.radiusM) else ""
        return "$src: %.5f, %.5f  $grid$acc".format(f.lat, f.lon)
    }
}
