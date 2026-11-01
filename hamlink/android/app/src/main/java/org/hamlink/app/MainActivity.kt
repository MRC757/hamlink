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
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
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

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { MaterialTheme { Surface(Modifier.fillMaxSize()) { App() } } }
    }
}

@Composable
private fun App() {
    val ctx = LocalContext.current
    val settings = remember { Settings(ctx) }
    var showSettings by remember { mutableStateOf(!settings.callsignValid()) }
    var showCal by remember { mutableStateOf(false) }
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
        if (need.isEmpty()) RadioService.start(ctx) else perms.launch(need.toTypedArray())
    }

    val on = Hub.phase != Hub.Phase.OFF
    Column(Modifier.fillMaxSize().padding(12.dp)) {
        // ---- header
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween,
            modifier = Modifier.fillMaxWidth()) {
            Column {
                Text(settings.callsign.ifEmpty { "set callsign" }, style = MaterialTheme.typography.titleLarge)
                Text(Hub.audioRoute, style = MaterialTheme.typography.bodySmall)
                Text(Hub.pttInfo, style = MaterialTheme.typography.bodySmall)
                if (Hub.modeSummary.isNotEmpty()) Text(Hub.modeSummary, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
            }
            Row {
                OutlinedButton(onClick = { showSettings = true }) { Text("Settings") }
                Box(Modifier.width(8.dp))
                OutlinedButton(onClick = {
                    if (settings.transport == "tnc")
                        Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "PTT test and calibration only apply to the audio link, not Bluetooth TNC mode"))
                    else showCal = true
                }) { Text("Test") }
                Box(Modifier.width(8.dp))
                Button(onClick = { if (on) RadioService.stop(ctx) else startRadio() }) { Text(if (on) "Stop" else "Start") }
            }
        }
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
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Modem:")
                FilterChip(selected = modemSel == 0, label = { Text("AFSK 1200") }, onClick = {
                    modemSel = 0; settings.modem = 0; RadioService.engine?.setModem(0)
                })
                FilterChip(selected = modemSel == 1, label = { Text("OFDM") }, onClick = {
                    modemSel = 1; settings.modem = 1; RadioService.engine?.setModem(1)
                })
                Text("(all stations must match)", style = MaterialTheme.typography.bodySmall)
            }
        }

        // ---- manual PTT: you key the radio when the countdown says so
        if (on && Hub.manualMode) {
            Surface(color = MaterialTheme.colorScheme.secondaryContainer, modifier = Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
                Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    if (Hub.manualStage.isNotEmpty()) {
                        Text(Hub.manualStage, style = MaterialTheme.typography.titleMedium, color = Color(0xFFC62828))
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
                        color = if (item.kind == Hub.Kind.ERROR || status?.startsWith("FAILED") == true) Color(0xFFC62828)
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
            OutlinedTextField(text, { text = it }, label = { Text("Message") }, modifier = Modifier.weight(1f))
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
            ) { Text(if (recording) "Recording… release to send" else "Hold to record voice", color = Color.White) }
        }
        } // tab == 0
    }

    if (showSettings) SettingsDialog(settings,
        requestBt = { if (Build.VERSION.SDK_INT >= 31) btPerm.launch(Manifest.permission.BLUETOOTH_CONNECT) },
        onDone = { showSettings = false })
    if (showCal) CalibrationDialog(settings, onClose = { showCal = false })
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
                        Text("No paired Bluetooth devices found. Pair the radio first, then reopen Settings.", color = Color(0xFFC62828))
                    } else {
                        Text("Radio:")
                        paired.forEach { (name, addr) ->
                            FilterChip(selected = btAddress == addr, onClick = { btAddress = addr; btName = name },
                                label = { Text("$name  ($addr)") })
                        }
                    }
                } else {
                    Text("Modem (every station on the channel must use the same one)")
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        FilterChip(selected = modem == 0, onClick = { modem = 0 }, label = { Text("AFSK 1200") })
                        FilterChip(selected = modem == 1, onClick = { modem = 1 }, label = { Text("OFDM") })
                    }
                    Text(if (modem == 1) "OFDM (COFDMTV, as in Rattlegram): decodes about 4-6 dB deeper into the noise than AFSK in simulation, " +
                        "but is not faster. Lower the PTT lead-in to about 200 ms to save airtime." else
                        "AFSK 1200: simple and interoperable with packet-radio tools.", style = MaterialTheme.typography.bodySmall)
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
                Text("Changes apply after Stop > Start.", style = MaterialTheme.typography.bodySmall)
            }
        },
        confirmButton = {
            TextButton(onClick = {
                s.callsign = call; s.ttl = ttl.toInt(); s.preambleMs = pre.toInt(); s.txLevel = tx
                s.busyThreshold = busy; s.pttLine = line; s.useUsbAudio = usb; s.autoPlayVoice = auto
                s.manualCountdown = manualSecs.toInt(); s.carrierSenseMode = sense; s.bitRepair = repair
                s.transport = transport; s.modem = modem; s.tncBaud = tncBaud; s.btAddress = btAddress; s.btName = btName
                if (s.callsignValid()) onDone()
            }) { Text("Save") }
        },
    )
}
