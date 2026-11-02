// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.Manifest
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp

/** The Map tab: map, position sources, situation reports. */
@Composable
fun MapScreen(settings: Settings) {
    val ctx = LocalContext.current
    val source = remember(settings.tileUrl, settings.tileOnline) { HttpTileSource(ctx.applicationContext, settings) }
    var showReport by remember { mutableStateOf(false) }
    var reportAt by remember { mutableStateOf<Pair<Double, Double>?>(null) } // set by a long-press on the map
    var showSettings by remember { mutableStateOf(false) }
    val running = Hub.phase != Hub.Phase.OFF

    LaunchedEffect(Unit) { Positions.apply(ctx) }

    Column(Modifier.fillMaxSize()) {
        Box(Modifier.weight(1f).fillMaxWidth()) {
            MapView(source, Hub.mapItems, Hub.position, onSelect = { key ->
                Hub.selectedKey = key
                Hub.selectedDetail = null
                if (key != null) RadioService.engine?.detail(key)
            }, onLongPress = { lat, lon ->
                // like right-click on Windows: report about the spot under the finger
                if (running) { reportAt = lat to lon; showReport = true }
                else Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "Start the radio to send a report"))
            })
            // zoom and recentre buttons
            Column(Modifier.align(Alignment.TopEnd).padding(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                MapButton("+") { Hub.camZoom = (Hub.camZoom + 1f).coerceAtMost(source.maxZoom.toFloat()) }
                MapButton("−") { Hub.camZoom = (Hub.camZoom - 1f).coerceAtLeast(2f) }
                MapButton("◎") { Hub.position?.let { Hub.camLat = it.lat; Hub.camLon = it.lon; if (Hub.camZoom < 10f) Hub.camZoom = 12f } }
            }
            Text(source.attribution, style = MaterialTheme.typography.labelSmall,
                modifier = Modifier.align(Alignment.BottomEnd).background(Color(0xCCFFFFFF)).padding(horizontal = 4.dp))
            if (!settings.tileOnline)
                Text("Offline: showing saved map tiles only", style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier.align(Alignment.BottomStart).background(Color(0xCCFFFFFF)).padding(horizontal = 4.dp))
        }

        Column(Modifier.fillMaxWidth().padding(8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Hub.selectedDetail?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
            if (running) Text("Long-press the map to report on that spot", style = MaterialTheme.typography.labelSmall, color = Color.Gray)
            Text(Hub.positionStatus, style = MaterialTheme.typography.bodySmall,
                color = if (Hub.position == null) Color(0xFFEF6C00) else MaterialTheme.colorScheme.primary)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(enabled = running && Hub.position != null, modifier = Modifier.weight(1f),
                    onClick = { RadioService.engine?.sendPosition() }) { Text("Send my position") }
                Button(enabled = running, modifier = Modifier.weight(1f), onClick = { reportAt = null; showReport = true }) { Text("New report") }
                OutlinedButton(onClick = { showSettings = true }) { Text("Map setup") }
            }
        }
    }
    if (showReport) ReportDialog(source, at = reportAt, onClose = { showReport = false })
    if (showSettings) MapSetupDialog(settings, onClose = { showSettings = false })
}

@Composable
private fun MapButton(label: String, onClick: () -> Unit) {
    Button(onClick = onClick, contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp), modifier = Modifier.width(44.dp)) { Text(label) }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun MapSetupDialog(s: Settings, onClose: () -> Unit) {
    val ctx = LocalContext.current
    var mode by remember { mutableStateOf(s.positionMode) }
    var typed by remember { mutableStateOf(s.typedPosition) }
    var beacon by remember { mutableStateOf(s.beaconMin) }
    var repeat by remember { mutableStateOf(s.reportRepeat) }
    var online by remember { mutableStateOf(s.tileOnline) }
    var url by remember { mutableStateOf(s.tileUrl) }
    var contact by remember { mutableStateOf(s.tileContact) }
    var attribution by remember { mutableStateOf(s.tileAttribution) }
    val locPerm = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { Positions.apply(ctx) }

    val parsed = if (mode == "gps" || typed.isBlank()) null else TypedPosition.parse(typed, mode == "grid")

    AlertDialog(
        onDismissRequest = onClose,
        title = { Text("Position and map") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("Where am I? (used for the map and for \"Send my position\")", style = MaterialTheme.typography.labelLarge)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = mode == "gps", onClick = { mode = "gps" }, label = { Text("Phone GPS") })
                    FilterChip(selected = mode == "typed", onClick = { mode = "typed" }, label = { Text("Typed lat/lon") })
                    FilterChip(selected = mode == "grid", onClick = { mode = "grid" }, label = { Text("Grid square") })
                }
                when (mode) {
                    "gps" -> {
                        if (!GpsProvider.permitted(ctx))
                            OutlinedButton(onClick = {
                                locPerm.launch(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION))
                            }) { Text("Allow location access") }
                        else Text("GPS works with no internet. The first fix can take a minute outdoors.", style = MaterialTheme.typography.bodySmall)
                    }
                    else -> {
                        OutlinedTextField(typed, { typed = it }, singleLine = true,
                            label = { Text(if (mode == "grid") "Grid square, e.g. FN31pr" else "Lat, lon  (38.8895, -77.0353  or  38d53m22sN 77d02m07sW  or MGRS)") })
                        if (typed.isNotBlank()) {
                            if (parsed == null) Text(if (mode == "grid") "Not a grid square" else "Cannot read this position", color = Color(0xFFC62828))
                            else Text("= %.5f, %.5f  %s".format(parsed.lat, parsed.lon, Native.geoFormat(parsed.lat, parsed.lon, 2)) +
                                if (parsed.radiusM > 0) "  (accurate to about %.1f km)".format(parsed.radiusM / 1000) else "",
                                style = MaterialTheme.typography.bodySmall)
                        }
                        Text("A grid square is sent as the centre of the square; receivers draw a ring showing the uncertainty.",
                            style = MaterialTheme.typography.bodySmall)
                    }
                }

                Text("Automatic position beacon", style = MaterialTheme.typography.labelLarge)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(0 to "Off", 5 to "5 min", 10 to "10 min", 15 to "15 min", 30 to "30 min").forEach { (m, t) ->
                        FilterChip(selected = beacon == m, onClick = { beacon = m }, label = { Text(t) })
                    }
                }
                Text("Every beacon is a radio transmission on a shared simplex channel. Off by default; keep it infrequent.",
                    style = MaterialTheme.typography.bodySmall)

                Text("Repeat my reports", style = MaterialTheme.typography.labelLarge)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(0 to "Off", 1 to "1 extra copy", 2 to "2 extra copies").forEach { (n, t) ->
                        FilterChip(selected = repeat == n, onClick = { repeat = n }, label = { Text(t) })
                    }
                }
                Text("Position and SALUTE reports sent to everyone are not acknowledged, so one lost transmission loses the report. " +
                    "Extra copies go out about 30 s apart; other stations show the report once. Each copy is a transmission.",
                    style = MaterialTheme.typography.bodySmall)

                Text("Map data", style = MaterialTheme.typography.labelLarge)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Switch(online, { online = it }); Box(Modifier.width(8.dp)); Text("Download map tiles when needed (needs internet)")
                }
                Text("Tiles you have viewed are saved on the phone and work offline later. Pan and zoom over your operating area " +
                    "while you still have internet. OpenStreetMap's server does not allow bulk downloading.", style = MaterialTheme.typography.bodySmall)
                OutlinedTextField(url, { url = it }, label = { Text("Tile server URL") }, minLines = 2)
                OutlinedTextField(contact, { contact = it }, singleLine = true, label = { Text("Contact for tile server (email), optional") })
                OutlinedTextField(attribution, { attribution = it }, singleLine = true, label = { Text("Attribution shown on the map") })
            }
        },
        confirmButton = {
            TextButton(onClick = {
                s.positionMode = mode; s.typedPosition = typed; s.beaconMin = beacon; s.reportRepeat = repeat
                RadioService.engine?.setReportRepeat(repeat)
                s.tileOnline = online; s.tileUrl = url; s.tileContact = contact; s.tileAttribution = attribution
                Positions.apply(ctx)
                onClose()
            }) { Text("Save") }
        },
        dismissButton = { TextButton(onClick = onClose) { Text("Cancel") } },
    )
}
