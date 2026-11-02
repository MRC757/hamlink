// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/** UI-visible state. Written from the engine thread via [ui], read by Compose. */
object Hub {
    enum class Phase { OFF, IDLE, BUSY, TX }
    enum class Kind { RX_TEXT, TX_TEXT, RX_VOICE, TX_VOICE, EVENT, ERROR }

    class LogItem(
        val kind: Kind,
        val text: String,
        val msgId: Int = -1,
        val clip: ShortArray? = null,
        val time: Long = System.currentTimeMillis(),
    )

    /** Settings.theme, mirrored here so a change in the Settings dialog recolours the app at once. */
    var theme by mutableStateOf(THEME_SYSTEM)
    var phase by mutableStateOf(Phase.OFF)
    var level by mutableStateOf(0f)
    var audioRoute by mutableStateOf("")
    var pttInfo by mutableStateOf("")
    class Meter(
        val rmsDb: Float = -120f, val peakDb: Float = -120f,
        val t1200Db: Float = -120f, val t2200Db: Float = -120f, val clipped: Int = 0,
    )

    var meter by mutableStateOf(Meter())
    /** True while a PTT test tone is on the air. */
    var testing by mutableStateOf(false)
    var testSecondsLeft by mutableStateOf(0)
    /** One-line description of the active link, audio route, PTT and modem. */
    var modeSummary by mutableStateOf("")
    /** MANUAL PTT: frames waiting, and what the operator should be doing right now ("" = idle). */
    var manualMode by mutableStateOf(false)
    var manualPending by mutableStateOf(0)
    var manualStage by mutableStateOf("")
    // ---- position and map ----
    var position by mutableStateOf<Fix?>(null)
    var positionStatus by mutableStateOf("No position yet")
    var mapItems by mutableStateOf<List<MapItem>>(emptyList())
    var selectedKey by mutableStateOf<String?>(null)
    var selectedDetail by mutableStateOf<String?>(null)
    var camLat by mutableStateOf(39.0)
    var camLon by mutableStateOf(-98.0)
    var camZoom by mutableStateOf(4f)
    var camPlaced by mutableStateOf(false)
    val log = mutableStateListOf<LogItem>()
    /** msgId -> "sending…" / "delivered" / "FAILED" / "sent" */
    val status = mutableStateMapOf<Int, String>()

    private val main = Handler(Looper.getMainLooper())
    fun ui(block: () -> Unit) { main.post(block) }

    fun addLog(item: LogItem) = ui {
        log.add(item)
        while (log.size > 300) log.removeAt(0)
    }
}
