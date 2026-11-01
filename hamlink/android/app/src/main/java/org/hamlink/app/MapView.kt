// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import kotlin.math.PI
import kotlin.math.atan
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sinh
import kotlin.math.tan

/** Web Mercator maths (256 px tiles). Same formulas as core/geo.c geo_to_world / geo_from_world. */
object Mercator {
    private const val MAX_LAT = 85.0511287798
    fun worldSize(zoom: Double) = 256.0 * 2.0.pow(zoom)
    fun x(lon: Double, zoom: Double) = (lon + 180.0) / 360.0 * worldSize(zoom)
    fun y(lat: Double, zoom: Double): Double {
        val r = Math.toRadians(lat.coerceIn(-MAX_LAT, MAX_LAT))
        return (1.0 - ln(tan(r) + 1.0 / cos(r)) / PI) / 2.0 * worldSize(zoom)
    }
    fun lon(x: Double, zoom: Double) = x / worldSize(zoom) * 360.0 - 180.0
    fun lat(y: Double, zoom: Double): Double =
        Math.toDegrees(atan(sinh(PI * (1.0 - 2.0 * y / worldSize(zoom)))))
    fun metersPerPixel(lat: Double, zoom: Double) = 40075016.686 * cos(Math.toRadians(lat)) / worldSize(zoom)
}

private fun parseColor(hex: String): Color =
    try { Color(android.graphics.Color.parseColor(hex)) } catch (_: IllegalArgumentException) { Color.Gray }

/**
 * Pan/zoom raster map with report icons. Camera state lives in [Hub] so it survives tab switches.
 * Drawing is separate from tile loading ([TileSource]) and from what the icons mean ([MapItem] comes from the core).
 */
@Composable
fun MapView(source: TileSource, items: List<MapItem>, me: Fix?, onSelect: (String?) -> Unit, modifier: Modifier = Modifier) {
    var redraw by remember { mutableIntStateOf(0) }
    DisposableEffect(source) {
        source.onChange = { redraw++ }
        onDispose { source.onChange = null }
    }
    val density = LocalDensity.current.density
    val selected = Hub.selectedKey
    // The tap handler reads the latest list without being restarted every time a new snapshot arrives (every 2 s),
    // which would otherwise cancel a tap in progress.
    val currentItems by rememberUpdatedState(items)
    val currentOnSelect by rememberUpdatedState(onSelect)

    Canvas(
        modifier = modifier
            .fillMaxSize()
            .pointerInput(Unit) {
                detectTransformGestures { centroid, pan, zoomChange, _ ->
                    val z0 = Hub.camZoom.toDouble()
                    val z1 = (z0 + ln(zoomChange.toDouble()) / ln(2.0)).coerceIn(2.0, source.maxZoom.toDouble())
                    val s = 2.0.pow(z1 - z0)
                    val cx0 = Mercator.x(Hub.camLon, z0)
                    val cy0 = Mercator.y(Hub.camLat, z0)
                    // world point under the fingers stays under the fingers while zooming
                    val px = cx0 + (centroid.x - size.width / 2.0)
                    val py = cy0 + (centroid.y - size.height / 2.0)
                    val nx = px * s - (centroid.x - size.width / 2.0) - pan.x
                    val ny = py * s - (centroid.y - size.height / 2.0) - pan.y
                    val lat = Mercator.lat(ny.coerceIn(0.0, Mercator.worldSize(z1)), z1)
                    var lon = Mercator.lon(nx, z1)
                    if (lon > 180.0) lon -= 360.0
                    if (lon < -180.0) lon += 360.0
                    Hub.camLat = lat
                    Hub.camLon = lon
                    Hub.camZoom = z1.toFloat()
                }
            }
            .pointerInput(Unit) {
                detectTapGestures { tap ->
                    val z = Hub.camZoom.toDouble()
                    val cx = Mercator.x(Hub.camLon, z)
                    val cy = Mercator.y(Hub.camLat, z)
                    var best: MapItem? = null
                    var bestD = 32f * density
                    for (it in currentItems) {
                        val sx = (Mercator.x(it.lon, z) - cx + size.width / 2.0).toFloat()
                        val sy = (Mercator.y(it.lat, z) - cy + size.height / 2.0).toFloat()
                        val d = (Offset(sx, sy) - tap).getDistance()
                        if (d < bestD) { bestD = d; best = it }
                    }
                    currentOnSelect(best?.key)
                }
            }
    ) {
        @Suppress("UNUSED_VARIABLE") val tick = redraw // reading it makes tile arrivals redraw the map
        drawMap(source, items, me, selected, density)
    }
}

private fun DrawScope.drawMap(source: TileSource, items: List<MapItem>, me: Fix?, selected: String?, density: Float) {
    drawRect(Color(0xFFE8E4DC), size = size)
    val zoom = Hub.camZoom.toDouble()
    val z = min(floor(zoom).toInt(), source.maxZoom)
    val scale = 2.0.pow(zoom - z)
    val tilePx = 256.0 * scale
    val cw = Mercator.x(Hub.camLon, z.toDouble())
    val ch = Mercator.y(Hub.camLat, z.toDouble())
    val left = cw - size.width / (2.0 * scale)
    val top = ch - size.height / (2.0 * scale)
    val right = cw + size.width / (2.0 * scale)
    val bottom = ch + size.height / (2.0 * scale)
    val n = 1 shl z
    for (ty in max(0, floor(top / 256.0).toInt())..min(n - 1, floor(bottom / 256.0).toInt())) {
        for (tx in floor(left / 256.0).toInt()..floor(right / 256.0).toInt()) {
            val sx = ((tx * 256.0 - left) * scale).roundToInt()
            val sy = ((ty * 256.0 - top) * scale).roundToInt()
            val bmp = source.tile(z, tx, ty)
            if (bmp != null) {
                drawImage(bmp.asImageBitmap(), dstOffset = IntOffset(sx, sy), dstSize = IntSize(ceil(tilePx).toInt() + 1, ceil(tilePx).toInt() + 1))
            } else {
                drawRect(Color(0xFFDAD6CC), topLeft = Offset(sx.toFloat(), sy.toFloat()), size = Size(tilePx.toFloat() - 1f, tilePx.toFloat() - 1f))
            }
        }
    }

    val cx = Mercator.x(Hub.camLon, zoom)
    val cy = Mercator.y(Hub.camLat, zoom)
    fun at(lat: Double, lon: Double) = Offset((Mercator.x(lon, zoom) - cx + size.width / 2.0).toFloat(), (Mercator.y(lat, zoom) - cy + size.height / 2.0).toFloat())

    val hasSelf = items.any { it.self }
    // stations first, then contacts; within each, higher severity later so it is drawn on top; selected last
    val ordered = items.sortedWith(compareBy<MapItem>({ it.type != 'S' }, { it.severity })).sortedBy { it.key == selected }
    for (it in ordered) drawItem(it, at(it.lat, it.lon), zoom, density, it.key == selected)
    if (me != null && !hasSelf) {
        val p = at(me.lat, me.lon)
        drawCircle(Color.White, 9f * density, p)
        drawCircle(Color(0xFF1565C0), 6f * density, p)
    }
}

private fun DrawScope.drawItem(it: MapItem, p: Offset, zoom: Double, density: Float, selected: Boolean) {
    val color = parseColor(it.color)
    val r = (if (it.type == 'S') 9f else 11f) * density
    if (it.radiusM > 0) {
        val ring = (it.radiusM / Mercator.metersPerPixel(it.lat, zoom)).toFloat()
        if (ring > r) {
            drawCircle(color.copy(alpha = 0.10f * it.alpha), ring, p)
            drawCircle(color.copy(alpha = 0.5f * it.alpha), ring, p, style = Stroke(width = 1.5f * density))
        }
    }
    if (selected) drawCircle(Color.White, r * 1.9f, p, alpha = 0.85f)
    val path = Path()
    when (it.shape) {
        "diamond" -> { path.moveTo(p.x, p.y - r * 1.25f); path.lineTo(p.x + r * 1.25f, p.y); path.lineTo(p.x, p.y + r * 1.25f); path.lineTo(p.x - r * 1.25f, p.y); path.close() }
        "square" -> path.addRect(androidx.compose.ui.geometry.Rect(p.x - r, p.y - r, p.x + r, p.y + r))
        "quatrefoil" -> {
            val c = r * 0.62f
            for ((dx, dy) in listOf(-1f to 0f, 1f to 0f, 0f to -1f, 0f to 1f))
                path.addOval(androidx.compose.ui.geometry.Rect(p.x + dx * c - c, p.y + dy * c - c, p.x + dx * c + c, p.y + dy * c + c))
        }
        else -> path.addRect(androidx.compose.ui.geometry.Rect(p.x - r * 1.5f, p.y - r * 0.9f, p.x + r * 1.5f, p.y + r * 0.9f)) // rectangle
    }
    drawPath(path, color, alpha = it.alpha)
    drawPath(path, Color.Black, alpha = it.alpha, style = Stroke(width = (if (it.self) 3f else 1.5f) * density))
    if (it.self) drawPath(path, Color.White, alpha = 1f, style = Stroke(width = 1.2f * density))
    if (zoom >= 9.5 || selected) {
        val text = if (it.type == 'S') it.name else it.label
        drawIntoCanvas { c ->
            val paint = android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG).apply {
                textSize = 11f * density
                textAlign = android.graphics.Paint.Align.CENTER
                this.color = android.graphics.Color.BLACK
                alpha = (255 * max(it.alpha, 0.5f)).toInt()
            }
            val halo = android.graphics.Paint(paint).apply {
                style = android.graphics.Paint.Style.STROKE; strokeWidth = 3f * density; this.color = android.graphics.Color.WHITE
            }
            val y = p.y + r + 13f * density
            c.nativeCanvas.drawText(text, p.x, y, halo)
            c.nativeCanvas.drawText(text, p.x, y, paint)
        }
    }
}
