// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.Manifest
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
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
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.SideEffect
import androidx.core.view.WindowCompat

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Hub.theme = Settings(this).theme
        setContent {
            val dark = when (Hub.theme) { THEME_DARK -> true; THEME_LIGHT -> false; else -> isSystemInDarkTheme() }
            // the app draws behind the status and navigation bars: their icons must contrast with our background
            SideEffect {
                WindowCompat.getInsetsController(window, window.decorView).apply {
                    isAppearanceLightStatusBars = !dark
                    isAppearanceLightNavigationBars = !dark
                }
            }
            MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                Surface(Modifier.fillMaxSize()) { App() }
            }
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun App() {
    val ctx = LocalContext.current
    val settings = remember { Settings(ctx) }
    var showSettings by remember { mutableStateOf(!settings.callsignValid()) }
    var showCal by remember { mutableStateOf(false) }
    var showHelp by remember { mutableStateOf(false) }
    var relay by remember { mutableStateOf(settings.relay) }
    var modemSel by remember { mutableStateOf(settings.modem) }
    var dest by remember { mutableStateOf(settings.dest) }
    var text by remember { mutableStateOf("") }
    val recorder = remember { VoiceRecorder(ctx) }
    var recording by remember { mutableStateOf(false) }
    var tab by remember { mutableStateOf(0) } // 0 = messages, 1 = map

    val perms = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        val ok = if (settings.transport == "tnc") Build.VERSION.SDK_INT < 31 || granted[Manifest.permission.BLUETOOTH_CONNECT] == true
                 else granted[Manifest.permission.RECORD_AUDIO] == true
        // location is optional: if refused the radio still starts, and Map setup offers it again
        if (granted[Manifest.permission.ACCESS_FINE_LOCATION] == true || granted[Manifest.permission.ACCESS_COARSE_LOCATION] == true)
            Positions.apply(ctx)
        if (ok) RadioService.start(ctx)
    }
    // Bluetooth permission on its own, so the settings dialog can list paired radios before Start.
    val btPerm = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { }
    fun startRadio() {
        if (!settings.callsignValid()) { showSettings = true; return }
        if (settings.transport == "tnc" && settings.btAddress.isBlank()) { showSettings = true; return }
        val need = mutableListOf<String>()
        if (settings.transport == "tnc") { if (Build.VERSION.SDK_INT >= 31) need += Manifest.permission.BLUETOOTH_CONNECT }
        else need += Manifest.permission.RECORD_AUDIO
        if (Build.VERSION.SDK_INT >= 33) need += Manifest.permission.POST_NOTIFICATIONS
        // Phone GPS is the default position source: ask together with the microphone, so the map and
        // "Send my position" work without a trip to Map setup.
        if (settings.positionMode == "gps" && !GpsProvider.permitted(ctx))
            need += listOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION)
        if (need.isEmpty()) RadioService.start(ctx) else perms.launch(need.toTypedArray())
    }

    val on = Hub.phase != Hub.Phase.OFF
    // While the on-screen keyboard is up only the message list and the compose box are shown, so the text being
    // typed and the latest messages are not squeezed out by the header (the keyboard takes about half the screen).
    val typing = WindowInsets.isImeVisible && tab == 0
    // Android 15 draws apps edge to edge: keep clear of the status bar, navigation bar and keyboard.
    Column(Modifier.fillMaxSize().safeDrawingPadding().padding(12.dp)) {
        if (typing) {
            val (label, color) = when (Hub.phase) {
                Hub.Phase.OFF -> "OFF" to Color.Gray
                Hub.Phase.IDLE -> "LISTENING" to Color(0xFF2E7D32)
                Hub.Phase.BUSY -> "CHANNEL BUSY" to Color(0xFFEF6C00)
                Hub.Phase.TX -> "TRANSMITTING" to Color(0xFFC62828)
            }
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(bottom = 4.dp)) {
                Text(settings.callsign, style = MaterialTheme.typography.titleSmall)
                Box(Modifier.width(8.dp))
                Text(label, color = Color.White, style = MaterialTheme.typography.labelMedium,
                    modifier = Modifier.background(color).padding(horizontal = 6.dp, vertical = 2.dp))
                if (on && Hub.manualMode && Hub.manualPending > 0) {
                    Box(Modifier.width(8.dp))
                    Text("${Hub.manualPending} waiting to transmit", style = MaterialTheme.typography.labelMedium)
                }
            }
        } else {
        // ---- header: callsign and buttons on one row, status lines below at full width, so long status text can
        // never push the buttons off screen
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Text(settings.callsign.ifEmpty { "set callsign" }, style = MaterialTheme.typography.titleLarge,
                maxLines = 1, modifier = Modifier.weight(1f))
            // tighter than the default padding so Help, Settings, Test and Start fit on a phone beside the callsign
            val pad = PaddingValues(horizontal = 12.dp, vertical = 8.dp)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                OutlinedButton(onClick = { showHelp = true }, contentPadding = PaddingValues(0.dp), modifier = Modifier.width(44.dp)) { Text("?") }
                OutlinedButton(onClick = { showSettings = true }, contentPadding = pad) { Text("Settings") }
                OutlinedButton(onClick = {
                    if (settings.transport == "tnc")
                        Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "PTT test and calibration only apply to the audio link, not Bluetooth TNC mode"))
                    else showCal = true
                }, contentPadding = pad) { Text("Test") }
                Button(onClick = { if (on) RadioService.stop(ctx) else startRadio() }, contentPadding = pad) { Text(if (on) "Stop" else "Start") }
            }
        }
        if (Hub.audioRoute.isNotEmpty()) Text(Hub.audioRoute, style = MaterialTheme.typography.bodySmall)
        if (Hub.pttInfo.isNotEmpty()) Text(Hub.pttInfo, style = MaterialTheme.typography.bodySmall)
        if (Hub.modeSummary.isNotEmpty()) Text(Hub.modeSummary, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(vertical = 8.dp)) {
            val (label, color) = when (Hub.phase) {
                Hub.Phase.OFF -> "OFF" to Color.Gray
                Hub.Phase.IDLE -> "LISTENING" to Color(0xFF2E7D32)
                Hub.Phase.BUSY -> "CHANNEL BUSY" to Color(0xFFEF6C00)
                Hub.Phase.TX -> "TRANSMITTING" to Color(0xFFC62828)
            }
            Text(label, color = Color.White, modifier = Modifier.background(color).padding(horizontal = 8.dp, vertical = 4.dp))
            Box(Modifier.width(12.dp))
            LinearProgressIndicator(progress = { (Hub.level * 20f).coerceIn(0f, 1f) }, modifier = Modifier.weight(1f))
            Box(Modifier.width(12.dp))
            Text("Relay"); Box(Modifier.width(4.dp))
            Switch(checked = relay, onCheckedChange = {
                relay = it; settings.relay = it; RadioService.engine?.setRelay(it)
            })
        }

        // ---- live modem switch (audio link only): handy for A/B tests without restarting
        if (settings.transport != "tnc") {
            // five modems do not fit a phone-width row: scroll sideways
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.horizontalScroll(rememberScrollState())) {
                Text("Modem:")
                Native.MODEM_LABELS.forEachIndexed { m, label ->
                    FilterChip(selected = modemSel == m, label = { Text(label) }, onClick = {
                        modemSel = m; settings.modem = m; RadioService.engine?.setModem(m)
                    })
                }
                Text("(all stations must match)", style = MaterialTheme.typography.bodySmall)
            }
        }

        // ---- manual PTT: you key the radio when the countdown says so
        if (on && Hub.manualMode && tab == 0) { // the Map tab has its own Transmit row under the map
            Surface(color = MaterialTheme.colorScheme.secondaryContainer, modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
                Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    if (Hub.manualStage.isNotEmpty()) {
                        Text(Hub.manualStage, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.error)
                    } else {
                        Text("Manual PTT: ${Hub.manualPending} frame(s) waiting. Tap Transmit, then key the radio when told.")
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(enabled = Hub.manualStage.isEmpty() && !Hub.testing, onClick = { RadioService.engine?.manualGo() }) {
                            Text("Transmit (${settings.manualCountdown} s countdown)")
                        }
                        OutlinedButton(onClick = { RadioService.engine?.manualCancel() }) { Text("Cancel") }
                    }
                }
            }
        }

        // ---- Messages / Map switch
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(bottom = 4.dp)) {
            FilterChip(selected = tab == 0, onClick = { tab = 0 }, label = { Text("Messages") })
            FilterChip(selected = tab == 1, onClick = { tab = 1 }, label = { Text("Map") })
        }
        } // !typing
        if (tab == 1) {
            Box(Modifier.weight(1f).fillMaxWidth()) { MapScreen(settings) }
        } else {
        // ---- log
        val listState = rememberLazyListState()
        LaunchedEffect(Hub.log.size) { if (Hub.log.isNotEmpty()) listState.animateScrollToItem(Hub.log.size - 1) }
        LazyColumn(state = listState, modifier = Modifier.weight(1f).fillMaxWidth()) {
            items(Hub.log) { item ->
                val status = if (item.msgId >= 0) Hub.status[item.msgId] else null
                val prefix = when (item.kind) {
                    Hub.Kind.TX_TEXT, Hub.Kind.TX_VOICE -> "→ "
                    Hub.Kind.RX_TEXT, Hub.Kind.RX_VOICE -> "← "
                    Hub.Kind.EVENT -> "· "
                    Hub.Kind.ERROR -> "! "
                }
                Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(vertical = 2.dp)) {
                    Text(prefix + item.text + (status?.let { "  [$it]" } ?: ""),
                        color = if (item.kind == Hub.Kind.ERROR || status?.startsWith("FAILED") == true) MaterialTheme.colorScheme.error
                        else if (item.kind == Hub.Kind.EVENT) Color.Gray else Color.Unspecified,
                        modifier = Modifier.weight(1f))
                    item.clip?.let { pcm -> TextButton(onClick = { VoicePlayer.play(ctx, pcm) }) { Text("Play") } }
                }
            }
        }

        // ---- compose
        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(dest, { dest = it.uppercase().take(8); settings.dest = dest }, label = { Text("To") },
                singleLine = true, modifier = Modifier.width(110.dp))
            Box(Modifier.width(8.dp))
            // grows to 4 lines as you type; the keyboard's Send key sends
            OutlinedTextField(text, { text = it }, label = { Text("Message") }, modifier = Modifier.weight(1f),
                minLines = 1, maxLines = 4,
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences, imeAction = ImeAction.Send),
                keyboardActions = KeyboardActions(onSend = {
                    if (on && text.isNotBlank() && dest.isNotBlank()) { RadioService.engine?.sendText(dest, text.trim()); text = "" }
                }))
        }
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(enabled = on && text.isNotBlank() && dest.isNotBlank(), modifier = Modifier.weight(1f), onClick = {
                RadioService.engine?.sendText(dest, text.trim()); text = ""
            }) { Text("Send text") }
            Box(
                Modifier.weight(1f)
                    .background(if (recording) Color(0xFFC62828) else MaterialTheme.colorScheme.primary)
                    .padding(vertical = 12.dp)
                    .pointerInput(on, dest) {
                        if (!on) return@pointerInput
                        detectTapGestures(onPress = {
                            recording = true
                            recorder.start()
                            tryAwaitRelease()
                            val pcm = recorder.stop()
                            recording = false
                            if (pcm.size > 8000 / 2 && dest.isNotBlank()) RadioService.engine?.sendVoice(dest, pcm)
                        })
                    },
                contentAlignment = Alignment.Center
            ) { Text(if (recording) "Recording… release to send" else "Hold to record voice", color = if (recording) Color.White else MaterialTheme.colorScheme.onPrimary) }
        }
        } // tab == 0
    }

    if (showSettings) SettingsDialog(settings,
        requestBt = { if (Build.VERSION.SDK_INT >= 31) btPerm.launch(Manifest.permission.BLUETOOTH_CONNECT) },
        onDone = {
            showSettings = false
            if (on) {   // settings are read at engine start: apply them now instead of after a manual Stop > Start
                RadioService.restartEngine(ctx)
                Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "Settings saved: radio restarted with the new settings"))
            }
        })
    if (showCal) CalibrationDialog(settings, onClose = { showCal = false })
    if (showHelp) HelpDialog(onClose = { showHelp = false })
}

@Composable
private fun SettingsDialog(s: Settings, requestBt: () -> Unit, onDone: () -> Unit) {
    val ctx = LocalContext.current
    var call by remember { mutableStateOf(s.callsign) }
    var ttl by remember { mutableStateOf(s.ttl.toFloat()) }
    var pre by remember { mutableStateOf(s.preambleMs.toFloat()) }
    var tx by remember { mutableStateOf(s.txLevel) }
    var busy by remember { mutableStateOf(s.busyThreshold) }
    var line by remember { mutableStateOf(s.pttLine) }
    var usb by remember { mutableStateOf(s.useUsbAudio) }
    var auto by remember { mutableStateOf(s.autoPlayVoice) }
    var manualSecs by remember { mutableStateOf(s.manualCountdown.toFloat()) }
    var sense by remember { mutableStateOf(s.carrierSenseMode) }
    var repair by remember { mutableStateOf(s.bitRepair) }
    var transport by remember { mutableStateOf(s.transport) }
    var modem by remember { mutableStateOf(s.modem) }
    var fec by remember { mutableStateOf(s.afskFec) }
    var tncBaud by remember { mutableStateOf(s.tncBaud) }
    var btAddress by remember { mutableStateOf(s.btAddress) }
    var btName by remember { mutableStateOf(s.btName) }
    val tnc = transport == "tnc"
    val hasBt = BtTnc.hasPermission(ctx)
    val paired = BtTnc.bonded(ctx)

    AlertDialog(
        onDismissRequest = {},
        title = { Text("Settings") },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                OutlinedTextField(call, { call = it.uppercase().take(8) }, label = { Text("Your callsign (required)") }, singleLine = true)

                // applies (and is saved) as soon as a chip is tapped, so the choice can be seen before Save
                Text("Appearance")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    listOf(THEME_SYSTEM to "Phone setting", THEME_LIGHT to "Light", THEME_DARK to "Dark").forEach { (t, label) ->
                        FilterChip(selected = Hub.theme == t, onClick = { s.theme = t; Hub.theme = t }, label = { Text(label) })
                    }
                }

                Text("Radio link")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    FilterChip(selected = !tnc, onClick = { transport = "audio" }, label = { Text("Audio + PTT") })
                    FilterChip(selected = tnc, onClick = { transport = "tnc" }, label = { Text("Bluetooth TNC") })
                }
                if (tnc) {
                    Text("The radio's own TNC does the modulation, keying and carrier sense. Pair it in Android's " +
                        "Bluetooth settings and enable Settings > General Settings > KISS TNC on the radio first.",
                        style = MaterialTheme.typography.bodySmall)
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("TNC speed:", modifier = Modifier.padding(top = 8.dp))
                        FilterChip(selected = tncBaud == 1200, onClick = { tncBaud = 1200 }, label = { Text("1200") })
                        FilterChip(selected = tncBaud == 9600, onClick = { tncBaud = 9600 }, label = { Text("9600") })
                    }
                    if (!hasBt) {
                        OutlinedButton(onClick = requestBt) { Text("Allow Bluetooth access") }
                    } else if (paired.isEmpty()) {
                        Text("No paired Bluetooth devices found. Pair the radio first, then reopen Settings.", color = MaterialTheme.colorScheme.error)
                    } else {
                        Text("Radio:")
                        paired.forEach { (name, addr) ->
                            FilterChip(selected = btAddress == addr, onClick = { btAddress = addr; btName = name },
                                label = { Text("$name  ($addr)") })
                        }
                    }
                } else {
                    Text("Modem (every station on the channel must use the same one)")
                    for (rowModems in listOf(listOf(0, 1, 2), listOf(3, 4))) {
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            rowModems.forEach { m ->
                                FilterChip(selected = modem == m, onClick = { modem = m }, label = { Text(Native.MODEM_LABELS[m]) })
                            }
                        }
                    }
                    Text(Native.modemNote(modem), style = MaterialTheme.typography.bodySmall)
                    if (modem == Native.MODEM_AFSK) {
                        Text("AFSK error correction (FX.25)")
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            listOf(0 to "Off", 16 to "16", 32 to "32", 64 to "64").forEach { (bytes, label) ->
                                FilterChip(selected = fec == bytes, onClick = { fec = bytes }, label = { Text(label) })
                            }
                        }
                        Text(if (fec == 0) "Off: plain AX.25-style frames." else
                            "FX.25 with $fec check bytes repairs up to ${fec / 2} damaged bytes per frame (16 is the default; more " +
                            "means longer transmissions). Stations without FX.25 still read frames that arrive intact.",
                            style = MaterialTheme.typography.bodySmall)
                    }
                }

                Text("Max relay hops for my messages: ${ttl.toInt()}")
                Slider(ttl, { ttl = it }, valueRange = 0f..3f, steps = 2)
                if (!tnc) {
                    Text("PTT lead-in: ${pre.toInt()} ms")
                    Slider(pre, { pre = it }, valueRange = 200f..1500f)
                    Text("TX audio level: %.2f  (use Test to calibrate)".format(tx))
                    Slider(tx, { tx = it }, valueRange = 0.02f..1f)
                    Text("Busy threshold (RMS): %.4f".format(busy))
                    Slider(busy, { busy = it }, valueRange = 0.0005f..0.05f)
                    Text("PTT (how the radio is keyed)")
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(selected = line == "RTS", onClick = { line = "RTS" }, label = { Text("RTS") })
                        FilterChip(selected = line == "DTR", onClick = { line = "DTR" }, label = { Text("DTR") })
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(selected = line == "VOX", onClick = { line = "VOX" }, label = { Text("None / radio VOX") })
                        FilterChip(selected = line == "MANUAL", onClick = { line = "MANUAL" }, label = { Text("Manual (I press PTT)") })
                    }
                    if (line == "MANUAL") {
                        Text("Manual PTT: tap Transmit, key the radio during the countdown (${manualSecs.toInt()} s), hold PTT while " +
                            "the audio plays, release when told. ACK waits are stretched to 45 s.", style = MaterialTheme.typography.bodySmall)
                        Slider(manualSecs, { manualSecs = it }, valueRange = 2f..10f, steps = 7)
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) { Switch(usb, { usb = it }); Box(Modifier.width(8.dp)); Text("Use USB audio interface (off = phone speaker + mic)") }
                    Text("Carrier sense (wait while the channel is in use)")
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(selected = sense == Native.CS_DATA_OR_LEVEL, onClick = { sense = Native.CS_DATA_OR_LEVEL }, label = { Text("Data or loud audio") })
                        FilterChip(selected = sense == Native.CS_DATA, onClick = { sense = Native.CS_DATA }, label = { Text("Data only") })
                        FilterChip(selected = sense == Native.CS_OFF, onClick = { sense = Native.CS_OFF }, label = { Text("Off") })
                    }
                    Text(when (sense) {
                        Native.CS_DATA -> "Waits only while packet data is heard. Room noise and voice are ignored: use this for acoustic-coupling tests."
                        Native.CS_OFF -> "Never waits. Only for tests: you may transmit over other stations."
                        else -> "Waits while packet data is heard or the audio is louder than the busy threshold (voice on the channel). Default."
                    }, style = MaterialTheme.typography.bodySmall)
                    Row(verticalAlignment = Alignment.CenterVertically) { Switch(repair, { repair = it }); Box(Modifier.width(8.dp)); Text("Repair single-bit errors in received frames") }
                }
                Row(verticalAlignment = Alignment.CenterVertically) { Switch(auto, { auto = it }); Box(Modifier.width(8.dp)); Text("Auto-play received voice") }
                Text("If the radio is running, Save restarts it with the new settings.", style = MaterialTheme.typography.bodySmall)
            }
        },
        confirmButton = {
            TextButton(onClick = {
                s.callsign = call; s.ttl = ttl.toInt(); s.preambleMs = pre.toInt(); s.txLevel = tx
                s.busyThreshold = busy; s.pttLine = line; s.useUsbAudio = usb; s.autoPlayVoice = auto
                s.manualCountdown = manualSecs.toInt(); s.carrierSenseMode = sense; s.bitRepair = repair
                s.transport = transport; s.modem = modem; s.afskFec = fec; s.tncBaud = tncBaud; s.btAddress = btAddress; s.btName = btName
                if (s.callsignValid()) onDone()
            }) { Text("Save") }
        },
    )
}
