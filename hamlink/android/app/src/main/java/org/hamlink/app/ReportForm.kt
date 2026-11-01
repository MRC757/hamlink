// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
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

    /** Types that appear on the map as situation reports (the position report has its own button). */
    val forms: List<SchemaSpec> get() = all.filter { it.role == "contact" }

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

/** Dialog that builds and sends one report of any schema in [Schemas.forms]. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ReportDialog(onClose: () -> Unit) {
    val forms = Schemas.forms
    if (forms.isEmpty()) { onClose(); return }
    var schema by remember { mutableStateOf(forms[0]) }
    val values = remember(schema) {
        mutableStateMapOf<String, String>().also { m ->
            for (f in schema.fields) when (f.kind) {
                "latlon" -> Hub.position?.let { m[f.key] = "%.5f,%.5f".format(it.lat, it.lon) }
                "time" -> m[f.key] = "0"
            }
        }
    }
    var error by remember { mutableStateOf<String?>(null) }
    var sending by remember { mutableStateOf(false) }

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
                    when (f.kind) {
                        "enum" -> {
                            Text(label, style = MaterialTheme.typography.labelLarge)
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
                            label = { Text("$label, 0 to ${f.max}") }, singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        )
                        "text" -> OutlinedTextField(
                            values[f.key] ?: "", { t -> values[f.key] = t.uppercase().take(f.max) },
                            label = { Text("$label, up to ${f.max} characters") }, singleLine = true,
                        )
                        "latlon" -> {
                            val t = values[f.key] ?: ""
                            OutlinedTextField(t, { values[f.key] = it }, label = { Text(label) }, singleLine = true,
                                placeholder = { Text("lat,lon  or  FN31pr  or  MGRS") })
                            val parsed = if (t.isBlank()) null else Native.geoParse(t)
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                Hub.position?.let { p ->
                                    OutlinedButton(onClick = { values[f.key] = "%.5f,%.5f".format(p.lat, p.lon) }) { Text("Use my position") }
                                }
                            }
                            if (t.isNotBlank()) {
                                if (parsed == null) Text("Cannot read this position", color = Color(0xFFC62828), style = MaterialTheme.typography.bodySmall)
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
                error?.let { Text(it, color = Color(0xFFC62828), modifier = Modifier.padding(top = 4.dp)) }
                Text("Sent to everyone in range (CQ). Callsign and time are added automatically.", style = MaterialTheme.typography.bodySmall)
            }
        },
        confirmButton = {
            TextButton(enabled = !sending, onClick = {
                val eng = RadioService.engine
                if (eng == null) { error = "Start the radio first (Messages tab, Start)."; return@TextButton }
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
}
