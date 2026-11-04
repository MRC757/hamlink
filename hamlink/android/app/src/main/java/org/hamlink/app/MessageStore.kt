// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.content.Context
import android.util.Base64
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONException
import org.json.JSONObject
import java.io.File

/** Direction of a stored message: received, sent, or overheard between two other stations (monitor mode). */
enum class MsgDir { IN, OUT, HEARD }

/**
 * One message to or from a station, kept for the Stations page (docs/STATIONS_PAGE.md). Created on the engine thread,
 * added to [MessageStore] on the main thread. Only [status] changes afterwards.
 */
class MsgRecord(
    val t: Long,                 // unix milliseconds
    val dir: MsgDir,
    val from: String,
    val to: String,              // a callsign or "CQ"
    val kind: String,            // text | voice | report
    val text: String,            // the text, the report summary, or a voice clip description
    val clip: ByteArray? = null, // voice: the Codec2 clip (decoded when played)
    val id: Int = -1,            // message ID of a sent message (16-bit, reused over time)
    status: String? = null,
) {
    var status by mutableStateOf(status)

    /** The other party, the station this message is listed under. Messages we sent to CQ list under "CQ". */
    val station: String get() = if (dir == MsgDir.OUT) to else from

    fun toJson(): String = JSONObject().apply {
        // same field names as the Windows app's messages.jsonl
        put("T", t); put("Dir", dir.ordinal); put("From", from); put("To", to); put("Kind", kind); put("Text", text)
        if (clip != null) put("Clip", Base64.encodeToString(clip, Base64.NO_WRAP))
        put("Id", id)
        status?.let { put("Status", it) }
    }.toString()

    companion object {
        fun fromJson(o: JSONObject) = MsgRecord(
            t = o.getLong("T"), dir = MsgDir.values()[o.optInt("Dir", 0).coerceIn(0, 2)],
            from = o.optString("From"), to = o.optString("To"), kind = o.optString("Kind", "text"),
            text = o.optString("Text"),
            clip = if (o.has("Clip")) Base64.decode(o.getString("Clip"), Base64.NO_WRAP) else null,
            id = o.optInt("Id", -1), status = if (o.has("Status")) o.getString("Status") else null,
        )
    }
}

/**
 * The message history: every message sent, received or overheard in the last [RETENTION_DAYS] days, saved to
 * messages.jsonl in the app's files folder. Append-only while running (one record or status line per change), rewritten
 * without expired lines at start-up. Main thread only: the engine goes through [Hub.ui].
 */
object MessageStore {
    const val RETENTION_DAYS = 5
    val all = mutableStateListOf<MsgRecord>()
    private var file: File? = null
    private var loaded = false

    /** Reads the history once per process, drops what has expired, and rewrites the file compactly. Never throws. */
    fun load(ctx: Context) {
        if (loaded) return
        loaded = true
        val f = File(ctx.filesDir, "messages.jsonl")
        file = f
        all.clear()
        try {
            if (!f.exists()) return
            val cutoff = System.currentTimeMillis() - RETENTION_DAYS * 86_400_000L
            val list = ArrayList<MsgRecord>()
            f.forEachLine { line ->
                if (line.isBlank()) return@forEachLine
                try {
                    val o = JSONObject(line)
                    if (o.has("S")) latest(list, o.getInt("Id"))?.status = o.getString("S")
                    else MsgRecord.fromJson(o).let { if (it.t >= cutoff) list.add(it) }
                } catch (_: JSONException) { /* one damaged line (power cut while writing) never loses the rest */ }
                catch (_: IllegalArgumentException) { }
            }
            all.addAll(list)
            rewrite()
        } catch (e: Exception) { Log.e("HamLink", "message history", e) }
    }

    /** Adds a message and appends it to the file. */
    fun add(r: MsgRecord) {
        all.add(r)
        append(r.toJson())
    }

    /** A status change ("queued", "sent", "delivered", "FAILED ...") for the newest sent message with this ID. */
    fun setStatus(id: Int, status: String) {
        val r = latest(all, id) ?: return
        if (r.status == status) return
        r.status = status
        append(JSONObject().apply { put("S", status); put("Id", id); put("T", System.currentTimeMillis()) }.toString())
    }

    /** Deletes the whole history (Settings, "Clear message history"). */
    fun clear() {
        all.clear()
        try { file?.delete() } catch (e: Exception) { Log.e("HamLink", "message history", e) }
    }

    private fun latest(list: List<MsgRecord>, id: Int): MsgRecord? {
        if (id < 0) return null
        for (i in list.indices.reversed()) if (list[i].dir == MsgDir.OUT && list[i].id == id) return list[i]
        return null
    }

    private fun append(line: String) {
        val f = file ?: return
        try { f.appendText(line + "\n") } catch (e: Exception) { Log.e("HamLink", "message history", e) }
    }

    private fun rewrite() {
        val f = file ?: return
        val tmp = File(f.parentFile, f.name + ".tmp")
        tmp.bufferedWriter().use { w -> for (r in all) { w.write(r.toJson()); w.newLine() } } // each record carries its latest status
        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }
}
