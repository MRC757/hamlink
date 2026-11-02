// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.util.LruCache
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

/**
 * An offline map pack: an MBTiles file (SQLite) of raster tiles, e.g. built with tools/make_mbtiles.py.
 * MBTiles stores rows bottom-up (TMS), so tile_row = 2^z - 1 - y. Read-only.
 */
class MapPack private constructor(val path: String, private val db: SQLiteDatabase, meta: Map<String, String>,
                                  val minZoom: Int, val maxZoom: Int) {
    val name: String = meta["name"]?.takeIf { it.isNotBlank() } ?: File(path).nameWithoutExtension
    /** MBTiles attribution is often HTML (links); the map shows plain text. */
    val attribution: String = (meta["attribution"] ?: "").replace(Regex("<[^>]*>"), "").replace("&copy;", "©").trim()
    /** West, south, east, north in degrees, or null when the file does not say. */
    val bounds: DoubleArray? = meta["bounds"]?.split(",")?.mapNotNull { it.trim().toDoubleOrNull() }
        ?.takeIf { it.size == 4 }?.toDoubleArray()

    /** The tile's image bytes, or null if the pack does not have it. */
    @Synchronized
    fun read(z: Int, x: Int, y: Int): ByteArray? {
        if (z < minZoom || z > maxZoom) return null
        db.rawQuery("SELECT tile_data FROM tiles WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?",
            arrayOf("$z", "$x", "${(1 shl z) - 1 - y}")).use { c -> return if (c.moveToFirst()) c.getBlob(0) else null }
    }

    /** One line for Map setup: name, zoom range, area, size. */
    fun describe(): String {
        val area = bounds?.let { b -> ", %.2f..%.2f N/S, %.2f..%.2f E/W".format(java.util.Locale.US, b[1], b[3], b[0], b[2]) } ?: ""
        return "$name: zoom $minZoom-$maxZoom$area, ${File(path).length() / 1048576} MB"
    }

    @Synchronized fun close() = db.close()

    companion object {
        /** Opens and checks a pack. Throws [IllegalArgumentException] with a readable message if it is not usable. */
        fun open(path: String): MapPack {
            if (!File(path).exists()) throw IllegalArgumentException("The map file was not found")
            val db = try {
                SQLiteDatabase.openDatabase(path, null, SQLiteDatabase.OPEN_READONLY or SQLiteDatabase.NO_LOCALIZED_COLLATORS)
            } catch (e: Exception) { throw IllegalArgumentException("Not an MBTiles map file (${e.message})") }
            try {
                val meta = HashMap<String, String>()
                db.rawQuery("SELECT name, value FROM metadata", null).use { c -> while (c.moveToNext()) meta[c.getString(0)] = c.getString(1) ?: "" }
                val fmt = (meta["format"] ?: "png").lowercase()
                if (fmt == "pbf" || fmt == "mvt")
                    throw IllegalArgumentException("This is a vector map (format pbf). HamLink needs raster tiles (png or jpg); see tools/make_mbtiles.py.")
                db.rawQuery("SELECT MIN(zoom_level), MAX(zoom_level) FROM tiles", null).use { c ->
                    if (!c.moveToFirst() || c.isNull(0)) throw IllegalArgumentException("The map file contains no tiles.")
                    return MapPack(path, db, meta, c.getInt(0), c.getInt(1))
                }
            } catch (e: IllegalArgumentException) { db.close(); throw e
            } catch (e: Exception) { db.close(); throw IllegalArgumentException("Not an MBTiles map file (${e.message})") }
        }

        /**
         * Copies a pack chosen with the system file picker into the app's storage (SQLite needs a real file, and a
         * picked document may vanish later). Returns the new file's path; throws with a readable message on failure.
         */
        fun import(ctx: Context, uri: Uri): String {
            val dir = File(ctx.filesDir, "maps").also { it.mkdirs() }
            val dest = File(dir, "pack-${System.currentTimeMillis()}.mbtiles")
            try {
                ctx.contentResolver.openInputStream(uri)?.use { input -> dest.outputStream().use { input.copyTo(it, 1 shl 16) } }
                    ?: throw IllegalArgumentException("Cannot read the chosen file")
                open(dest.path).close()                       // refuse anything HamLink cannot use
                return dest.path
            } catch (e: Exception) {
                dest.delete()
                throw if (e is IllegalArgumentException) e else IllegalArgumentException("Copy failed: ${e.message}")
            }
        }

        /** Deletes imported packs other than [keep] (old imports, cancelled ones). */
        fun deleteOthers(ctx: Context, keep: String) {
            File(ctx.filesDir, "maps").listFiles()?.forEach { if (it.path != keep) it.delete() }
        }
    }
}

/**
 * Map pack first, online tiles (and their disk cache) for anything the pack lacks. Zoomed in beyond the pack's deepest
 * level, the pack's last tiles are enlarged (up to [MAX_OVERZOOM] levels) rather than leaving the map blank.
 */
class PackTileSource(private val pack: MapPack, private val fallback: TileSource) : TileSource {
    override val attribution: String
        get() = if (pack.attribution.isEmpty() || pack.attribution == fallback.attribution) fallback.attribution
                else "${pack.attribution} | ${fallback.attribution}"
    override val maxZoom: Int get() = maxOf(fallback.maxZoom, pack.maxZoom + MAX_OVERZOOM)
    override var onChange: (() -> Unit)? = null
        set(v) { field = v; fallback.onChange = v }

    private val mem = object : LruCache<Long, Bitmap>(120) {}
    private val inflight = ConcurrentHashMap.newKeySet<Long>()
    private val missing = ConcurrentHashMap.newKeySet<Long>()
    private val pool = Executors.newSingleThreadExecutor()

    private fun key(z: Int, x: Int, y: Int): Long = (z.toLong() shl 56) or (x.toLong() shl 28) or y.toLong()

    override fun tile(z: Int, x: Int, y: Int): Bitmap? {
        val n = 1 shl z
        if (z < 0 || y < 0 || y >= n) return null
        val xw = ((x % n) + n) % n
        val k = key(z, xw, y)
        synchronized(mem) { mem.get(k) }?.let { return it }
        if (k in missing) return fallback.tile(z, xw, y)
        if (inflight.add(k)) pool.execute { load(z, xw, y, k) }
        return null // the pack answers in milliseconds; asking the network meanwhile would waste a download
    }

    private fun load(z: Int, x: Int, y: Int, k: Long) {
        try {
            val bmp: Bitmap? = if (z <= pack.maxZoom) {
                pack.read(z, x, y)?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
            } else if (z - pack.maxZoom <= MAX_OVERZOOM) {
                val dz = z - pack.maxZoom
                pack.read(pack.maxZoom, x shr dz, y shr dz)?.let { BitmapFactory.decodeByteArray(it, 0, it.size) }
                    ?.let { enlarge(it, dz, x and ((1 shl dz) - 1), y and ((1 shl dz) - 1)) }
            } else null
            if (bmp == null) missing.add(k) else synchronized(mem) { mem.put(k, bmp) }
            Hub.ui { onChange?.invoke() }
        } catch (_: Exception) {
            missing.add(k) // a damaged tile falls back to the online source; never crash the radio app
        } finally {
            inflight.remove(k)
        }
    }

    /** The (sx, sy) part of a parent tile, 2^dz parts per side, scaled up to a whole tile. */
    private fun enlarge(parent: Bitmap, dz: Int, sx: Int, sy: Int): Bitmap? {
        val parts = 1 shl dz
        val w = parent.width / parts; val h = parent.height / parts
        if (w < 1 || h < 1) return null
        val part = Bitmap.createBitmap(parent, sx * w, sy * h, w, h)
        return Bitmap.createScaledBitmap(part, parent.width, parent.height, true)
    }

    companion object { const val MAX_OVERZOOM = 5 }
}
