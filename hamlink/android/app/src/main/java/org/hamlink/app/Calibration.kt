// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import kotlin.math.log10
import kotlin.math.max

private fun db(x: Float) = if (x <= 0f) -120f else 20f * log10(x)
private fun fmt(db: Float) = if (db <= -119f) "  -inf" else "%6.1f".format(db)
private fun bar(db: Float) = ((db + 60f) / 60f).coerceIn(0f, 1f) // -60..0 dBFS

/**
 * PTT test + manual calibration. TX side: tone with a live level. RX side: meter with peak hold,
 * tone levels at 1200/2200 Hz, twist and a clipping warning.
 */
@Composable
fun CalibrationDialog(settings: Settings, onClose: () -> Unit) {
    val engine = RadioService.engine
    val running = Hub.phase != Hub.Phase.OFF
    var mode by remember { mutableStateOf(Native.TONE_ALT) }
    var level by remember { mutableStateOf(settings.txLevel) }
    var hold1200 by remember { mutableStateOf(-120f) }
    var hold2200 by remember { mutableStateOf(-120f) }
    var holdPeak by remember { mutableStateOf(-120f) }
    var saved by remember { mutableStateOf(false) }

    val m = Hub.meter
    LaunchedEffect(m) {
        hold1200 = max(hold1200, m.t1200Db)
        hold2200 = max(hold2200, m.t2200Db)
        holdPeak = max(holdPeak, m.peakDb)
    }

    Dialog(onDismissRequest = { engine?.stopTest(); onClose() },
        properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(modifier = Modifier.fillMaxWidth().padding(8.dp), shape = MaterialTheme.shapes.large) {
            Column(Modifier.verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("PTT test and calibration", style = MaterialTheme.typography.titleLarge)
                if (!running) Text("Press Start on the main screen first: the radio engine must be running.", color = MaterialTheme.colorScheme.error)
                if (Hub.manualStage.isNotEmpty()) Text(Hub.manualStage, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.error)
                if (settings.pttLine == "MANUAL") Text("Manual PTT: after you tap Transmit test, key the radio during the countdown and hold it until told to release.",
                    style = MaterialTheme.typography.bodySmall)

                Text("Transmit test", style = MaterialTheme.typography.titleMedium)
                Text("This keys your radio. Use low power or a dummy load, only on a frequency you may use. " +
                    "A test tone lasts at most 15 s, and your callsign is sent right after.", style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    FilterChip(mode == Native.TONE_OFF, { mode = Native.TONE_OFF }, label = { Text("PTT only") })
                    FilterChip(mode == Native.TONE_1200, { mode = Native.TONE_1200 }, label = { Text("1200") })
                    FilterChip(mode == Native.TONE_2200, { mode = Native.TONE_2200 }, label = { Text("2200") })
                    FilterChip(mode == Native.TONE_ALT, { mode = Native.TONE_ALT }, label = { Text("Alt") })
                }
                Text("TX level %.3f  (%.1f dBFS)".format(level, db(level)))
                Slider(level, { level = it; saved = false; engine?.setTestLevel(it) }, valueRange = 0.02f..1f)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (Hub.testing) {
                        Button(onClick = { engine?.stopTest() }, modifier = Modifier.weight(1f)) {
                            Text("STOP  (${Hub.testSecondsLeft} s left)")
                        }
                    } else {
                        Button(enabled = running, onClick = { engine?.startTest(mode, level) }, modifier = Modifier.weight(1f)) {
                            Text("Transmit test")
                        }
                    }
                    OutlinedButton(onClick = { settings.txLevel = level; saved = true }, modifier = Modifier.weight(1f)) {
                        Text(if (saved) "Saved" else "Use as TX level")
                    }
                }
                if (Hub.testing) Text("ON AIR", color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.titleMedium)

                Text("Receive meter", style = MaterialTheme.typography.titleMedium)
                Text("Peak %s dBFS   RMS %s dBFS".format(fmt(m.peakDb), fmt(m.rmsDb)))
                LinearProgressIndicator(progress = { bar(m.peakDb) }, modifier = Modifier.fillMaxWidth())
                Text("1200 Hz %s dBFS   (hold %s)".format(fmt(m.t1200Db), fmt(hold1200)))
                LinearProgressIndicator(progress = { bar(m.t1200Db) }, modifier = Modifier.fillMaxWidth())
                Text("2200 Hz %s dBFS   (hold %s)".format(fmt(m.t2200Db), fmt(hold2200)))
                LinearProgressIndicator(progress = { bar(m.t2200Db) }, modifier = Modifier.fillMaxWidth())
                if (hold1200 > -70f && hold2200 > -70f)
                    Text("Twist (2200 vs 1200, held): %+.1f dB. Near 0 is ideal; 6 dB was tolerated in simulation.".format(hold2200 - hold1200))
                val advice = when {
                    m.clipped > 0 -> "INPUT CLIPPING: lower the radio volume or the DigiRig input gain."
                    holdPeak > -3f -> "Very hot input. Reduce the radio volume."
                    holdPeak > -120f && holdPeak < -40f -> "Input is weak. Raise the radio volume."
                    holdPeak >= -40f -> "Input level OK (a peak of about -20 to -6 dBFS is a good target)."
                    else -> "No signal yet."
                }
                Text(advice, color = if (m.clipped > 0) MaterialTheme.colorScheme.error else Color.Unspecified)
                OutlinedButton(onClick = { hold1200 = -120f; hold2200 = -120f; holdPeak = -120f }) { Text("Reset holds") }

                Text("Two-station procedure", style = MaterialTheme.typography.titleMedium)
                Text("1. Station B opens this screen and watches the meter.\n" +
                    "2. Station A picks Alt, sets a low level (about 0.05) and starts the tone.\n" +
                    "3. A raises the level in steps of about 3 dB (x1.4), pausing 2 s on each. B notes the held 1200 and 2200 Hz readings.\n" +
                    "4. Where B's reading stops rising, the radio's limiter is working. Set A about 6 dB (half) below that and tap Use as TX level.\n" +
                    "The knee may be soft on cheap radios; confirm with a real message exchange. Repeat if you change wide/narrow bandwidth.",
                    style = MaterialTheme.typography.bodySmall)

                Button(onClick = { engine?.stopTest(); onClose() }, modifier = Modifier.fillMaxWidth()) { Text("Close") }
            }
        }
    }
}
