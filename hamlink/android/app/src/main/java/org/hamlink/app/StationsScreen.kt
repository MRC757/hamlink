// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.roundToInt

/** The entry that holds the messages this station sent to CQ. */
const val CQ_ENTRY = "CQ"

/** When each station's conversation was last opened (unread counts), saved in the app's preferences. */
object StationsRead {
    val lastRead = mutableStateMapOf<String, Long>()
    private var loaded = false

    fun load(ctx: Context) {
        if (loaded) return
        loaded = true
        val json = ctx.getSharedPreferences("hamlink", Context.MODE_PRIVATE).getString("stations_read", null) ?: return
        try {
            val o = JSONObject(json)
            for (k in o.keys()) lastRead[k] = o.getLong(k)
        } catch (_: Exception) { }
    }

    fun mark(ctx: Context, call: String) {
        lastRead[call] = System.currentTimeMillis()
        val o = JSONObject(); for ((k, v) in lastRead) o.put(k, v)
        ctx.getSharedPreferences("hamlink", Context.MODE_PRIVATE).edit().putString("stations_read", o.toString()).apply()
    }
}

/** The station(s) a message is listed under: the other party, both callsigns for overheard traffic, only the sender
 *  for a station ID. */
fun MsgRecord.parties(): List<String> = when {
    dir == MsgDir.OUT -> listOf(to)
    dir == MsgDir.HEARD && kind != "id" && to != CQ_ENTRY -> listOf(from, to)
    else -> listOf(from)
}

/** "Show overheard" on the Stations page. */
object StationsView { var showHeard by mutableStateOf(true) }

/** Overheard traffic is shown while monitor mode is on and "Show overheard" is ticked; station IDs always count for
 *  "last heard". */
fun MsgRecord.shown() = dir != MsgDir.HEARD || kind == "id" || (Hub.monitor && StationsView.showHeard)

data class StationRow(val call: String, val lastT: Long, val unread: Int, val detail: String, val color: Color)

/** Station list: everyone heard (map) or messaged (history), most recent first, the CQ entry on top. */
fun stationRows(open: String?): List<StationRow> {
    data class Acc(var last: Long = 0, var unread: Int = 0)
    val acc = HashMap<String, Acc>()
    for (m in MessageStore.all) if (m.shown()) for (call in m.parties()) {
        val a = acc.getOrPut(call) { Acc() }
        a.last = maxOf(a.last, m.t)
        val read = StationsRead.lastRead[call] ?: 0L
        if (m.dir == MsgDir.IN && m.t > read && call != open) a.unread++
    }
    val now = System.currentTimeMillis()
    val stations = Hub.mapItems.filter { it.type == 'S' && !it.self }.associateBy { it.name.uppercase() }
    for (s in stations.values) {
        val a = acc.getOrPut(s.name.uppercase()) { Acc() }
        a.last = maxOf(a.last, now - s.ageMin * 60_000L)
    }
    val rows = acc.filterKeys { it != CQ_ENTRY }.map { (call, a) ->
        val st = stations[call.uppercase()]
        StationRow(call, a.last, a.unread, stationDetail(a.last, st), st?.let { parseColor(it.color) } ?: Color.LightGray)
    }.sortedByDescending { it.lastT }.toMutableList()
    val cq = acc[CQ_ENTRY]
    rows.add(0, StationRow(CQ_ENTRY, cq?.last ?: 0, cq?.unread ?: 0, "All stations: messages you sent to CQ", Color(0xFF4682B4)))
    return rows
}

fun totalUnread(open: String?): Int = stationRows(open).sumOf { it.unread }

private fun stationDetail(last: Long, st: MapItem?): String {
    val parts = mutableListOf(if (last > 0) ago(last) else "not heard")
    val me = Hub.position
    if (st != null && me != null) {
        val d = distanceM(me.lat, me.lon, st.lat, st.lon)
        val dist = if (d >= 1000) "%.1f km".format(d / 1000) else "%.0f m".format(d)
        parts += "$dist ${compass(bearingDeg(me.lat, me.lon, st.lat, st.lon))}"
    }
    return parts.joinToString(" · ")
}

private fun parseColor(hex: String): Color = try { Color(android.graphics.Color.parseColor(hex)) } catch (_: Exception) { Color.LightGray }

private fun ago(t: Long): String {
    val min = (System.currentTimeMillis() - t).coerceAtLeast(0) / 60_000
    return when { min < 1 -> "just now"; min < 60 -> "$min min ago"; min < 1440 -> "${min / 60} h ago"; else -> "${min / 1440} d ago" }
}

private fun compass(deg: Double) = listOf("N", "NE", "E", "SE", "S", "SW", "W", "NW")[(((deg % 360 + 360) % 360) / 45.0).roundToInt() % 8]

// the same spherical formulas as core/geo.c
private fun distanceM(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
    val r = Math.PI / 180
    val dp = (lat2 - lat1) * r; val dl = (lon2 - lon1) * r
    val a = Math.sin(dp / 2) * Math.sin(dp / 2) + Math.cos(lat1 * r) * Math.cos(lat2 * r) * Math.sin(dl / 2) * Math.sin(dl / 2)
    return 6371008.8 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a))
}

private fun bearingDeg(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
    val r = Math.PI / 180
    val dl = (lon2 - lon1) * r
    val y = Math.sin(dl) * Math.cos(lat2 * r)
    val x = Math.cos(lat1 * r) * Math.sin(lat2 * r) - Math.sin(lat1 * r) * Math.cos(lat2 * r) * Math.cos(dl)
    val b = Math.atan2(y, x) / r
    return if (b < 0) b + 360 else b
}

/** The station list. Tapping a row opens that station. */
@Composable
fun StationList(modifier: Modifier, onOpen: (String) -> Unit) {
    var query by remember { mutableStateOf("") }
    // read the store and map so Compose recomposes when either changes
    val rows = stationRows(null).let { MessageStore.all.size; Hub.mapItems; StationsRead.lastRead.size; StationsView.showHeard; it }
        .filter { query.isBlank() || it.call == CQ_ENTRY || it.call.contains(query.trim().uppercase()) }
    Column(modifier) {
        OutlinedTextField(query, { query = it.uppercase().take(8) }, label = { Text("Find a callsign") }, singleLine = true,
            modifier = Modifier.fillMaxWidth())
        if (Hub.monitor) Row(verticalAlignment = Alignment.CenterVertically) {
            Switch(StationsView.showHeard, { StationsView.showHeard = it })
            Text("  Show overheard (messages between other stations)", style = MaterialTheme.typography.bodySmall)
        }
        LazyColumn(Modifier.weight(1f).fillMaxWidth()) {
            items(rows, key = { it.call }) { r ->
                Row(Modifier.fillMaxWidth().clickable { onOpen(r.call) }.padding(vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(12.dp).background(r.color, CircleShape))
                    Column(Modifier.weight(1f).padding(start = 12.dp)) {
                        Text(if (r.call == CQ_ENTRY) "All stations (CQ)" else r.call, fontWeight = FontWeight.SemiBold)
                        Text(r.detail, style = MaterialTheme.typography.bodySmall, color = Color.Gray)
                    }
                    if (r.unread > 0) Box(Modifier.background(MaterialTheme.colorScheme.error, RoundedCornerShape(10.dp)).padding(horizontal = 8.dp, vertical = 2.dp)) {
                        Text("${r.unread}", color = MaterialTheme.colorScheme.onError, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                    }
                }
                HorizontalDivider()
            }
        }
    }
}

/** One station's conversation. The message box below it (MainActivity) is addressed to this station. */
@Composable
fun StationConversation(call: String, modifier: Modifier, onBack: () -> Unit, onShowOnMap: (() -> Unit)?) {
    val ctx = LocalContext.current
    val msgs = MessageStore.all.filter { m -> m.kind != "id" && m.shown() && m.parties().any { it.equals(call, ignoreCase = true) } }
    val listState = rememberLazyListState()
    LaunchedEffect(call, msgs.size) {
        StationsRead.mark(ctx, call)
        if (msgs.isNotEmpty()) listState.scrollToItem(msgs.size - 1)
    }
    val dayFmt = remember { SimpleDateFormat("EEEE d MMMM", Locale.getDefault()) }
    val timeFmt = remember { SimpleDateFormat("HH:mm", Locale.getDefault()) }
    Column(modifier) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Stations") }
            Text(if (call == CQ_ENTRY) "All stations (CQ)" else call, style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.weight(1f))
            if (onShowOnMap != null) OutlinedButton(onClick = onShowOnMap) { Text("Map") }
        }
        if (msgs.isEmpty()) Text("No messages yet. Type below to send one.", color = Color.Gray, modifier = Modifier.padding(12.dp))
        LazyColumn(state = listState, modifier = Modifier.weight(1f).fillMaxWidth()) {
            items(msgs.size) { i ->
                val m = msgs[i]
                val day = dayFmt.format(Date(m.t))
                if (i == 0 || dayFmt.format(Date(msgs[i - 1].t)) != day)
                    Text(day, style = MaterialTheme.typography.bodySmall, color = Color.Gray,
                        modifier = Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 2.dp), textAlign = androidx.compose.ui.text.style.TextAlign.Center)
                Bubble(m, timeFmt.format(Date(m.t)), ctx)
            }
        }
    }
}

@Composable
private fun Bubble(m: MsgRecord, time: String, ctx: Context) {
    val mine = m.dir == MsgDir.OUT
    val who = when (m.dir) {
        MsgDir.OUT -> if (m.to == CQ_ENTRY) "to everyone" else "to ${m.to}"
        MsgDir.HEARD -> "${m.from} → ${m.to} (overheard)"
        MsgDir.IN -> if (m.to == CQ_ENTRY) "${m.from} to everyone" else m.from
    }
    val bg = when {
        m.dir == MsgDir.HEARD -> MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.5f)
        mine -> MaterialTheme.colorScheme.primaryContainer
        else -> MaterialTheme.colorScheme.surfaceVariant
    }
    Row(Modifier.fillMaxWidth().padding(vertical = 3.dp), horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start) {
        Column(Modifier.widthIn(max = 300.dp).background(bg, RoundedCornerShape(8.dp)).padding(horizontal = 10.dp, vertical = 6.dp)) {
            val status = m.status
            Text("$time  $who" + (m.via?.let { "  · via $it" } ?: "") + (if (m.dir != MsgDir.IN && status != null) "  [$status]" else ""),
                style = MaterialTheme.typography.bodySmall,
                color = if (status?.startsWith("FAILED") == true) MaterialTheme.colorScheme.error else Color.Gray)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(if (m.kind == "report") "[report] ${m.text}" else m.text, modifier = Modifier.weight(1f, fill = false),
                    color = if (m.dir == MsgDir.HEARD) Color.Gray else Color.Unspecified)
                val clip = m.clip
                if (m.kind == "voice" && clip != null) TextButton(onClick = {
                    Native.storedVoiceDecode(clip)?.let { VoicePlayer.play(ctx, it) }
                }) { Text("Play") }
            }
        }
    }
}
