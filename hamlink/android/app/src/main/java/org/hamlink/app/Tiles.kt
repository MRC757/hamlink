// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.SystemClock
import android.util.LruCache
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors

/**
 * Where map pictures come from. The map view only knows this interface, so an offline pack (MBTiles/PMTiles)
 * or another provider can be added later as another implementation without touching drawing code.
 */
interface TileSource {
    val attribution: String
    val maxZoom: Int
    /** Called (on the UI thread) when a requested tile has arrived and the map should redraw. */
    var onChange: (() -> Unit)?
    /** The tile if it is already in memory, else null and a load is started. Never blocks. */
    fun tile(z: Int, x: Int, y: Int): Bitmap?
}

/**
 * Slippy-map raster tiles (default: openstreetmap.org) cached on disk.
 *
 * Behaviour follows the OSM tile usage policy (operations.osmfoundation.org/policies/tiles):
 * tiles are fetched only when the visible map needs them (no bulk download, no prefetch), at most 2 connections,
 * an identifying User-Agent with the app version and project URL, HTTP cache headers honoured (max-age or Expires, ETag
 * revalidation, min 1 h), all downloads paused after 429/403/5xx (Retry-After honoured), and attribution is shown.
 * Tiles already on disk are used forever when offline. Set a different URL template for your own/commercial tile
 * server if you need heavier use.
 */
class HttpTileSource(ctx: Context, private val settings: Settings) : TileSource {
    override val attribution: String get() = settings.tileAttribution
    override val maxZoom = 19
    override var onChange: (() -> Unit)? = null

    private val dir: File = File(ctx.filesDir, "tiles/" + hash(settings.tileUrl)).also { it.mkdirs() }
    private val mem = object : LruCache<Long, Bitmap>(96) {}
    private val inflight = ConcurrentHashMap.newKeySet<Long>()
    private val wantedAt = ConcurrentHashMap<Long, Long>()
    private val failUntil = ConcurrentHashMap<Long, Long>()
    private val pool = Executors.newFixedThreadPool(2) // OSM policy: at most 2 connections
    private val appVersion: String =
        try { ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: "0" } catch (e: Exception) { "0" }

    init { pool.execute { prune() } }

    private fun key(z: Int, x: Int, y: Int): Long = (z.toLong() shl 56) or (x.toLong() shl 28) or y.toLong()

    override fun tile(z: Int, x: Int, y: Int): Bitmap? {
        val n = 1 shl z
        if (z < 0 || z > maxZoom || y < 0 || y >= n) return null
        val xw = ((x % n) + n) % n
        val k = key(z, xw, y)
        wantedAt[k] = SystemClock.elapsedRealtime()
        synchronized(mem) { mem.get(k) }?.let { return it }
        if (inflight.add(k)) pool.execute { load(z, xw, y, k) }
        return null
    }

    private fun file(z: Int, x: Int, y: Int) = File(dir, "$z/$x/$y.tile")
    private fun metaFile(f: File) = File(f.path + ".meta")

    private fun load(z: Int, x: Int, y: Int, k: Long) {
        try {
            val f = file(z, x, y)
            var bmp: Bitmap? = if (f.exists()) BitmapFactory.decodeFile(f.path) else null
            val meta = readMeta(f)
            val fresh = bmp != null && meta.expiresAt > System.currentTimeMillis()
            if (bmp != null) { f.setLastModified(System.currentTimeMillis()) } // LRU for pruning
            if (!fresh && settings.tileOnline && stillWanted(k) && System.currentTimeMillis() >= (failUntil[k] ?: 0L)) {
                val got = fetch(z, x, y, f, meta)
                if (got == null) failUntil[k] = System.currentTimeMillis() + 60_000
                else bmp = got
            }
            if (bmp != null) {
                synchronized(mem) { mem.put(k, bmp) }
                Hub.ui { onChange?.invoke() }
            }
        } catch (_: Exception) {
            // a bad tile or a dropped connection must never crash the radio app
        } finally {
            inflight.remove(k)
        }
    }

    /** A tile queued while the user kept panning is dropped if the map has not asked for it for 15 s. */
    private fun stillWanted(k: Long) = SystemClock.elapsedRealtime() - (wantedAt[k] ?: 0L) < 15_000

    private class Meta(val etag: String?, val expiresAt: Long)

    private fun readMeta(f: File): Meta {
        val m = metaFile(f)
        if (!m.exists()) return Meta(null, 0)
        val lines = m.readLines()
        return Meta(lines.getOrNull(0)?.ifEmpty { null }, lines.getOrNull(1)?.toLongOrNull() ?: 0L)
    }

    private fun fetch(z: Int, x: Int, y: Int, f: File, meta: Meta): Bitmap? {
        val url = settings.tileUrl.replace("{z}", "$z").replace("{x}", "$x").replace("{y}", "$y")
        if (!url.startsWith("https://")) return null
        if (System.currentTimeMillis() < pauseUntil) return null
        val c = URL(url).openConnection() as HttpURLConnection
        try {
            c.connectTimeout = 8000
            c.readTimeout = 10000
            c.setRequestProperty("User-Agent", userAgent())
            if (f.exists() && meta.etag != null) c.setRequestProperty("If-None-Match", meta.etag)
            val code = c.responseCode
            val now = System.currentTimeMillis()
            if (code == 304) {
                writeMeta(f, meta.etag, now + maxAgeMs(c))
                return BitmapFactory.decodeFile(f.path)
            }
            if (code == 429 || code == 403 || code >= 500) {
                // slow down (429), refused (403) or server trouble: pause every tile download, honouring Retry-After
                val wait = c.getHeaderField("Retry-After")?.trim()?.toLongOrNull()?.times(1000L)
                    ?: if (code == 403) 30 * 60_000L else 2 * 60_000L
                pauseUntil = now + wait.coerceIn(30_000L, 3_600_000L)
                return null
            }
            if (code != 200) return null
            val bytes = c.inputStream.use { it.readBytes() }
            val bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return null
            f.parentFile?.mkdirs()
            val tmp = File(f.path + ".part")
            tmp.writeBytes(bytes)
            if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
            writeMeta(f, c.getHeaderField("ETag"), now + maxAgeMs(c))
            return bmp
        } finally {
            c.disconnect()
        }
    }

    private fun maxAgeMs(c: HttpURLConnection): Long {
        val cc = c.getHeaderField("Cache-Control") ?: ""
        val secs = Regex("max-age=(\\d+)").find(cc)?.groupValues?.get(1)?.toLongOrNull()
            ?: c.expiration.takeIf { it > 0 }?.let { (it - System.currentTimeMillis()) / 1000 } // Expires header
            ?: (7 * 24 * 3600L)
        return maxOf(secs, 3600L) * 1000L
    }

    private fun writeMeta(f: File, etag: String?, expiresAt: Long) {
        metaFile(f).writeText((etag ?: "") + "\n" + expiresAt + "\n")
    }

    /** Identifies the app, its version and who runs it, as the OSM tile policy asks. */
    private fun userAgent(): String {
        val contact = settings.tileContact.ifBlank { settings.callsign }
        return "HamLink/$appVersion (amateur radio messaging app; +https://github.com/MRC757/hamlink; contact: $contact)"
    }

    /** Keep the cache under the configured size by deleting the least recently used tiles. */
    private fun prune() {
        val limit = settings.tileCacheMb * 1024L * 1024L
        val files = dir.walkTopDown().filter { it.isFile && it.name.endsWith(".tile") }.toList()
        var total = files.sumOf { it.length() }
        if (total <= limit) return
        for (f in files.sortedBy { it.lastModified() }) {
            if (total <= limit * 8 / 10) break
            total -= f.length()
            f.delete()
            metaFile(f).delete()
        }
    }

    companion object {
        /** After "too many requests", "blocked" or a server error, no tile is fetched until then (honours Retry-After). */
        @Volatile private var pauseUntil = 0L

        private fun hash(s: String): String =
            MessageDigest.getInstance("SHA-1").digest(s.toByteArray()).take(6).joinToString("") { "%02x".format(it) }
    }
}
