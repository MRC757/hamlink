// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import org.json.JSONObject

// ---------------------------------------------------------------------------------------------------------------
// Report types are described by the JSON files in /schemas, compiled into the core. This file turns that
// description into a form, so adding a report type needs no change here.
// ---------------------------------------------------------------------------------------------------------------

class ValueSpec(val code: Int, val name: String, val color: String?)

class FieldSpec(
    val key: String, val label: String, val kind: String, val optional: Boolean, val max: Int, val values: List<ValueSpec>,
)

class SchemaSpec(val name: String, val title: String, val role: String, val fields: List<FieldSpec>)

object Schemas {
    /** All report types the core knows. Stateless native calls: safe from the UI thread. */
    val all: List<SchemaSpec> by lazy {
        (0 until Native.schemaCount()).mapNotNull { i -> Native.schemaJson(i)?.let { parse(JSONObject(it)) } }
    }

    /** Types offered in the New report dialog: all but station positions, which have their own button. */
    val forms: List<SchemaSpec> get() = all.filter { it.role != "station" }

    private fun parse(o: JSONObject): SchemaSpec {
        val fields = ArrayList<FieldSpec>()
        val fa = o.getJSONArray("fields")
        for (i in 0 until fa.length()) {
            val f = fa.getJSONObject(i)
            val kind = f.getString("kind")
            val vals = ArrayList<ValueSpec>()
            f.optJSONArray("values")?.let { va ->
                for (k in 0 until va.length()) {
                    val v = va.getJSONObject(k)
                    vals.add(ValueSpec(v.getInt("code"), v.getString("name"), v.optString("color").ifEmpty { null }))
                }
            }
            val max = when (kind) {
                "uint" -> if (f.has("max")) f.getInt("max") else (1 shl f.getInt("bits")) - 1
                "text" -> f.getInt("max")
                else -> 0
            }
            fields.add(FieldSpec(f.getString("key"), f.getString("label"), kind, f.optBoolean("optional", false), max, vals))
        }
        return SchemaSpec(o.getString("name"), o.getString("title"), o.optJSONObject("map")?.optString("role", "none") ?: "none", fields)
    }
}

private val AGO = listOf(0 to "Just now", 5 to "5 min ago", 15 to "15 min ago", 30 to "30 min ago", 60 to "1 h ago", 120 to "2 h ago")

private fun colorOf(hex: String?): Color? =
    try { hex?.let { Color(android.graphics.Color.parseColor(it)) } } catch (_: IllegalArgumentException) { null }

private fun fmt(lat: Double, lon: Double) = "%.5f,%.5f".format(java.util.Locale.US, lat, lon)

/** Enum fields with more choices than this are a drop-down list instead of a wall of chips. */
private const val CHIP_LIMIT = 8

/**
 * Dialog that builds and sends one report of any schema in [Schemas.forms].
 * [at] (from a long-press on the map) pre-fills the report's position; otherwise it starts at the operator's position.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ReportDialog(source: TileSource, at: Pair<Double, Double>? = null, onClose: () -> Unit) {
    val forms = Schemas.forms
    if (forms.isEmpty()) { onClose(); return }
    // a report about a map spot opens on the first form that has a position (SALUTE)
    var schema by remember { mutableStateOf(if (at != null) forms.firstOrNull { s -> s.fields.any { it.kind == "latlon" } } ?: forms[0] else forms[0]) }
    val values = remember(schema) {
        mutableStateMapOf<String, String>().also { m ->
            for (f in schema.fields) when (f.kind) {
                "latlon" -> (at ?: Hub.position?.let { it.lat to it.lon })?.let { (lat, lon) -> m[f.key] = fmt(lat, lon) }
                "time" -> m[f.key] = "0"
            }
        }
    }
    var error by remember { mutableStateOf<String?>(null) }
    var tried by remember(schema) { mutableStateOf(false) } // after a failed Send, missing fields are shown in red
    var sending by remember { mutableStateOf(false) }
    var picking by remember { mutableStateOf<String?>(null) } // key of the position field being picked on the map
    // any change to the form clears the last error, so it never describes an earlier state of the form
    LaunchedEffect(values.toMap()) { error = null }
    fun missing(f: FieldSpec) = !f.optional && f.kind in setOf("enum", "uint", "text", "latlon") && values[f.key].isNullOrBlank()
    val red = MaterialTheme.colorScheme.error

    AlertDialog(
        onDismissRequest = onClose,
        title = { Text(schema.title) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (forms.size > 1) FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    forms.forEach { s -> FilterChip(selected = s === schema, onClick = { schema = s; error = null }, label = { Text(s.name) }) }
                }
                for (f in schema.fields) {
                    val label = f.label + if (f.optional) " (optional)" else ""
                    val bad = tried && missing(f)
                    when (f.kind) {
                        "enum" -> if (f.values.size > CHIP_LIMIT) {
                            EnumDropdown(label, f.values.map { it.name }, values[f.key], f.optional, bad) { pick ->
                                if (pick == null) values.remove(f.key) else values[f.key] = pick
                            }
                        } else {
                            Text(label, style = MaterialTheme.typography.labelLarge, color = if (bad) red else Color.Unspecified)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                f.values.forEach { v ->
                                    val on = values[f.key] == v.name
                                    val c = colorOf(v.color)
                                    FilterChip(
                                        selected = on,
                                        onClick = { if (on && f.optional) values.remove(f.key) else values[f.key] = v.name },
                                        label = { Text(v.name) },
                                        colors = if (c != null) FilterChipDefaults.filterChipColors(selectedContainerColor = c.copy(alpha = 0.45f))
                                                 else FilterChipDefaults.filterChipColors(),
                                    )
                                }
                            }
                        }
                        "flags" -> {
                            Text(label, style = MaterialTheme.typography.labelLarge)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                val chosen = (values[f.key] ?: "").split(",").filter { it.isNotBlank() }.toSet()
                                f.values.forEach { v ->
                                    FilterChip(selected = v.name in chosen, label = { Text(v.name) }, onClick = {
                                        val next = if (v.name in chosen) chosen - v.name else chosen + v.name
                                        values[f.key] = f.values.map { it.name }.filter { it in next }.joinToString(",")
                                    })
                                }
                            }
                        }
                        "uint" -> OutlinedTextField(
                            values[f.key] ?: "", { t -> values[f.key] = t.filter { it.isDigit() }.take(5) },
                            label = { Text("$label, 0 to ${f.max}") }, singleLine = true, isError = bad,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        )
                        "text" -> OutlinedTextField(
                            values[f.key] ?: "", { t -> values[f.key] = t.uppercase().take(f.max) },
                            label = { Text("$label, up to ${f.max} characters") }, singleLine = true, isError = bad,
                        )
                        "latlon" -> {
                            val t = values[f.key] ?: ""
                            OutlinedTextField(t, { values[f.key] = it }, label = { Text(label) }, singleLine = true, isError = bad,
                                placeholder = { Text("lat,lon  or  FN31pr  or  MGRS") })
                            val parsed = if (t.isBlank()) null else Native.geoParse(t)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Button(onClick = { picking = f.key }) { Text("Pick on map") }
                                Hub.position?.let { p ->
                                    OutlinedButton(onClick = { values[f.key] = fmt(p.lat, p.lon) }) { Text("Use my position") }
                                }
                            }
                            if (t.isNotBlank()) {
                                if (parsed == null) Text("Cannot read this position", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                                else Text("= %.5f, %.5f  %s".format(parsed[0], parsed[1], Native.geoFormat(parsed[0], parsed[1], 2)),
                                    style = MaterialTheme.typography.bodySmall)
                            }
                        }
                        "time" -> {
                            Text(label, style = MaterialTheme.typography.labelLarge)
                            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                                AGO.forEach { (min, text) ->
                                    FilterChip(selected = values[f.key] == "$min", onClick = { values[f.key] = "$min" }, label = { Text(text) })
                                }
                            }
                        }
                    }
                }
                error?.let { Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(top = 4.dp)) }
                Text("Sent to everyone in range (CQ). Callsign and time are added automatically.", style = MaterialTheme.typography.bodySmall)
            }
        },
        confirmButton = {
            TextButton(enabled = !sending, onClick = {
                val eng = RadioService.engine
                if (eng == null) { error = "Start the radio first (Messages tab, Start)."; return@TextButton }
                val gaps = schema.fields.filter { missing(it) }
                if (gaps.isNotEmpty()) {
                    tried = true
                    error = "Still needed: " + gaps.joinToString("; ") { it.label }
                    return@TextButton
                }
                val nowS = System.currentTimeMillis() / 1000
                val kv = StringBuilder()
                for (f in schema.fields) {
                    val v = values[f.key]?.trim().orEmpty()
                    if (f.kind == "time") kv.append("${f.key}=${nowS - (v.toLongOrNull() ?: 0L) * 60}\n")
                    else if (v.isNotEmpty() || f.kind == "flags" && !f.optional) kv.append("${f.key}=${v.ifEmpty { "none" }}\n")
                }
                sending = true
                eng.sendReport(schema.name, kv.toString()) { err ->
                    sending = false
                    if (err == null) onClose() else error = err
                }
            }) { Text("Send") }
        },
        dismissButton = { TextButton(onClick = onClose) { Text("Cancel") } },
    )

    picking?.let { key ->
        val start = values[key]?.takeIf { it.isNotBlank() }?.let { Native.geoParse(it) }?.let { it[0] to it[1] }
        MapPickerDialog(source, start, onCancel = { picking = null }, onPick = { lat, lon ->
            values[key] = fmt(lat, lon)
            picking = null
        })
    }
}

/** Drop-down for an enum field with many choices (e.g. the SITREP state list). */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun EnumDropdown(label: String, choices: List<String>, value: String?, optional: Boolean, isError: Boolean,
                         onPick: (String?) -> Unit) {
    var open by remember { mutableStateOf(false) }
    ExposedDropdownMenuBox(expanded = open, onExpandedChange = { open = it }) {
        OutlinedTextField(
            value ?: "", {}, readOnly = true, singleLine = true, isError = isError,
            label = { Text(label) }, placeholder = { Text("Choose…") },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = open) },
            modifier = Modifier.menuAnchor(MenuAnchorType.PrimaryNotEditable).fillMaxWidth(),
        )
        ExposedDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            if (optional) DropdownMenuItem(text = { Text("(none)") }, onClick = { onPick(null); open = false })
            choices.forEach { c -> DropdownMenuItem(text = { Text(c) }, onClick = { onPick(c); open = false }) }
        }
    }
}

/**
 * Full-screen map for choosing a report's position: the spot under the crosshairs is the answer. Drag to move the map,
 * or tap a spot to bring it to the crosshairs. The main map's view is restored afterwards.
 */
@Composable
private fun MapPickerDialog(source: TileSource, start: Pair<Double, Double>?, onCancel: () -> Unit,
                            onPick: (Double, Double) -> Unit) {
    val saved = remember { Triple(Hub.camLat, Hub.camLon, Hub.camZoom) }
    remember {
        val p = start ?: Hub.position?.let { it.lat to it.lon }
        if (p != null) { Hub.camLat = p.first; Hub.camLon = p.second; if (Hub.camZoom < 13f) Hub.camZoom = 15f }
        0
    }
    DisposableEffect(Unit) { onDispose { Hub.camLat = saved.first; Hub.camLon = saved.second; Hub.camZoom = saved.third } }

    // decorFitsSystemWindows = false: the dialog window is laid out edge to edge, so safeDrawingPadding knows where the
    // status and navigation bars are (otherwise, on Android 15, the bottom of a full-screen dialog runs off the screen).
    Dialog(onDismissRequest = onCancel,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize()) {
            Column(Modifier.fillMaxSize().safeDrawingPadding()) {
                // the answer and the buttons are at the top, where nothing can cover them
                Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Move the map so the crosshairs are on the spot, or tap the spot.", style = MaterialTheme.typography.bodyMedium)
                    Text("%.5f, %.5f   %s".format(java.util.Locale.US, Hub.camLat, Hub.camLon, Native.geoFormat(Hub.camLat, Hub.camLon, 2)),
                        style = MaterialTheme.typography.titleMedium)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OutlinedButton(onClick = onCancel, modifier = Modifier.weight(1f)) { Text("Cancel") }
                        Button(onClick = { onPick(Hub.camLat, Hub.camLon) }, modifier = Modifier.weight(1f)) { Text("Use this spot") }
                    }
                }
                Box(Modifier.weight(1f).fillMaxWidth()) {
                    MapView(source, Hub.mapItems, Hub.position, onSelect = {}, onTapPoint = { lat, lon ->
                        Hub.camLat = lat; Hub.camLon = lon
                    })
                    Canvas(Modifier.size(56.dp).align(Alignment.Center)) {
                        val c = center; val r = size.minDimension / 2f; val w = 2.dp.toPx()
                        for ((col, sw) in listOf(Color.White to w * 2.5f, Color(0xFFC62828) to w)) {
                            drawLine(col, Offset(c.x - r, c.y), Offset(c.x - r * 0.25f, c.y), sw)
                            drawLine(col, Offset(c.x + r * 0.25f, c.y), Offset(c.x + r, c.y), sw)
                            drawLine(col, Offset(c.x, c.y - r), Offset(c.x, c.y - r * 0.25f), sw)
                            drawLine(col, Offset(c.x, c.y + r * 0.25f), Offset(c.x, c.y + r), sw)
                            drawCircle(col, r * 0.5f, c, style = Stroke(sw))
                        }
                    }
                    Column(Modifier.align(Alignment.TopEnd).padding(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        Button(onClick = { Hub.camZoom = (Hub.camZoom + 1f).coerceAtMost(source.maxZoom.toFloat()) }) { Text("+") }
                        Button(onClick = { Hub.camZoom = (Hub.camZoom - 1f).coerceAtLeast(2f) }) { Text("−") }
                    }
                    Text(source.attribution, style = MaterialTheme.typography.labelSmall, color = Color.Black, // on a white box in both themes
                        modifier = Modifier.align(Alignment.BottomEnd).background(Color(0xCCFFFFFF)).padding(horizontal = 4.dp))
                }
            }
        }
    }
}
