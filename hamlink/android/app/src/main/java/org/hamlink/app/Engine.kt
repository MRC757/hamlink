// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.os.SystemClock
import java.util.concurrent.LinkedBlockingQueue
import kotlin.math.max

/**
 * The radio engine. One thread owns everything: audio capture (48 kHz), the C demodulator and
 * mesh logic, PTT, and transmit. Half duplex: while a burst is on the air we are not listening.
 */
class Engine(private val ctx: Context) : MeshListener {
    private val settings = Settings(ctx)
    private val audio = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val cmds = LinkedBlockingQueue<() -> Unit>()
    @Volatile private var running = false
    private var thread: Thread? = null
    private var ptt: Ptt? = null
    private val fs = 48000
    private val chunk = 960 // 20 ms

    private fun now() = SystemClock.elapsedRealtime()

    fun start() {
        if (running) return
        running = true
        thread = Thread(::loop, "hamlink-engine").also { it.start() }
    }

    fun stop() {
        running = false
        thread?.join(4000)
        thread = null
    }

    fun post(c: () -> Unit) { cmds.add(c) }

    // ------------------------------------------------------------------ manual PTT and live modem switch (UI -> engine)
    @Volatile private var manualArmAt = 0L      // when the countdown ends and the burst is sent; 0 = not armed
    @Volatile private var stageClearAt = 0L

    /** MANUAL PTT: start the countdown; when it ends the next due burst is played and the operator holds PTT. */
    fun manualGo() { if (manualArmAt == 0L) manualArmAt = now() + settings.manualCountdown * 1000L }
    fun manualCancel() { manualArmAt = 0L; Hub.ui { Hub.manualStage = "" } }

    /** Switch modem while running (every station on the channel must use the same one). Audio link only. */
    fun setModem(modem: Int) = post {
        if (settings.transport == "tnc") return@post
        val rc = Native.setModem(modem)
        modemNow = if (rc != 0) Native.MODEM_AFSK else modem
        if (rc != 0) Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "${Native.modemName(modem)} could not start; using AFSK 1200"))
        else Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "Modem: " + Native.modemName(modem)))
        updateSummary(if (rc != 0) Native.MODEM_AFSK else modem)
    }

    private fun updateSummary(modem: Int) {
        val audio = if (settings.useUsbAudio && usb(true) != null) "USB audio interface" else "phone speaker + mic (acoustic)"
        val ptt = when (settings.pttLine) {
            "MANUAL" -> "manual PTT (you key the radio)"
            "VOX" -> "no PTT line (transmit immediately)"
            else -> "PTT via ${settings.pttLine}"
        }
        val sense = when (settings.carrierSenseMode) { Native.CS_OFF -> "carrier sense OFF"; Native.CS_DATA -> "carrier sense: data only"; else -> "carrier sense on" }
        val m = if (modem == Native.MODEM_AFSK) { if (settings.afskFec > 0) "AFSK 1200 + FX.25 (${settings.afskFec})" else "AFSK 1200" }
                else Native.MODEM_LABELS[modem]
        Hub.ui { Hub.modeSummary = "$audio · $ptt · $m · $sense" }
    }

    private fun setStage(text: String) { Hub.ui { Hub.manualStage = text } }

    // ------------------------------------------------------------------ PTT test (UI -> engine)
    @Volatile private var testRequest = -1      // requested Native.TONE_* mode, -1 = none
    @Volatile private var testStopFlag = false
    @Volatile private var testAmp = 0.3f

    /** Start a PTT test: keys the radio and sends a steady tone (or silence for TONE_OFF) for up to 15 s. */
    fun startTest(mode: Int, amp: Float) {
        testAmp = amp.coerceIn(0.02f, 1f)
        testStopFlag = false
        testRequest = mode
    }
    fun stopTest() { testStopFlag = true; testRequest = -1 }
    /** Live level change while a test tone is on the air. */
    fun setTestLevel(amp: Float) { testAmp = amp.coerceIn(0.02f, 1f) }

    // ------------------------------------------------------------------ UI -> engine
    fun setRelay(on: Boolean) = post { Native.setRelay(on) }

    fun sendText(dst: String, text: String) = post {
        val bytes = text.toByteArray(Charsets.UTF_8)
        val id = Native.send(dst, Native.MT_TEXT, bytes, dst != "CQ", now())
        queued(id, Hub.Kind.TX_TEXT, "to $dst: $text", null, record(MsgDir.OUT, settings.callsign, dst, "text", text, null, id))
    }

    fun sendVoice(dst: String, pcm8k: ShortArray) = post {
        val clip = Native.voiceEncode(pcm8k, pcm8k.size)
        if (clip == null) {
            Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "voice encode failed"))
            return@post
        }
        val id = Native.send(dst, Native.MT_VOICE, clip, dst != "CQ", now())
        val secs = pcm8k.size / 8000.0
        queued(id, Hub.Kind.TX_VOICE, "voice to $dst (%.1f s, %d bytes)".format(secs, clip.size), pcm8k,
            record(MsgDir.OUT, settings.callsign, dst, "voice", "voice clip (%.1f s)".format(secs), clip, id))
    }

    // ------------------------------------------------------------------ reports and map (UI -> engine)
    /** Build and send a report of any schema (see the schemas directory). [done] gets null on success or a readable error. */
    fun sendReport(schema: String, kv: String, dst: String = "CQ", done: (String?) -> Unit) = post {
        val r = Native.reportSend(dst, schema, kv, dst != "CQ", now())
        if (r.startsWith("OK:")) {
            val id = r.substring(3).toIntOrNull() ?: -1
            val rec = record(MsgDir.OUT, settings.callsign, dst, "report", "$schema report", null, id)
            Hub.ui { if (id >= 0) Hub.status[id] = "queued"; MessageStore.add(rec); MessageStore.setStatus(id, "queued") }
            Hub.addLog(Hub.LogItem(Hub.Kind.TX_TEXT, "$schema report sent to $dst", msgId = id))
            Hub.ui { done(null) }
        } else {
            Hub.ui { done(r.removePrefix("ERR:")) }
        }
    }

    /** Transmit our current position (whatever source is selected) as a position report. */
    fun sendPosition() = post { sendPositionNow() }

    private fun sendPositionNow() {
        val f = Positions.current
        if (f == null) { Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "no position to send yet")); return }
        val kv = "loc=%.6f,%.6f\ntime=%d\nsource=%s\n".format(java.util.Locale.US, f.lat, f.lon, System.currentTimeMillis() / 1000, f.source)
        val r = Native.reportSend("CQ", "POS", kv, false, now())
        if (r.startsWith("OK:")) {
            val id = r.substring(3).toIntOrNull() ?: -1
            val rec = record(MsgDir.OUT, settings.callsign, "CQ", "report", "position (${f.source})", null, id)
            Hub.ui { MessageStore.add(rec); MessageStore.setStatus(id, "queued") }
            Hub.addLog(Hub.LogItem(Hub.Kind.TX_TEXT, "position sent (${f.source})", msgId = id))
        } else Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "position not sent: " + r.removePrefix("ERR:")))
    }

    /** Fetch the details text for a map icon. */
    fun detail(key: String) = post {
        val d = Native.mapDetail(key, now())
        Hub.ui { if (Hub.selectedKey == key) Hub.selectedDetail = d }
    }

    fun clearMap() = post { Native.mapClear(); Hub.ui { Hub.mapItems = emptyList(); Hub.selectedKey = null; Hub.selectedDetail = null } }

    // Called every loop turn on the engine thread (both audio and Bluetooth-TNC modes).
    private var nextMapAt = 0L
    private val fecInfo = IntArray(3)
    private var fecOnlySeen = 0
    private var nextClockAt = 0L
    private var beaconDueAt = 0L
    private fun housekeeping(t: Long) {
        if (t >= nextClockAt) { // the phone's clock is the time base for report timestamps and icon ageing
            nextClockAt = t + 300_000
            Native.setClock(System.currentTimeMillis() / 1000, t)
        }
        if (t >= nextMapAt) {
            nextMapAt = t + 2000
            val items = MapItem.parse(Native.mapSnapshot(t))
            Hub.ui { Hub.mapItems = items }
            Native.getFecInfo(fecInfo)
            if (fecInfo[1] > fecOnlySeen) {
                Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "FX.25 error correction recovered ${fecInfo[1] - fecOnlySeen} frame(s) " +
                    "that arrived damaged (${fecInfo[2]} bad bytes fixed so far)"))
                fecOnlySeen = fecInfo[1]
            }
        }
        val minutes = settings.beaconMin
        if (minutes > 0 && Positions.current != null) {
            if (beaconDueAt == 0L) beaconDueAt = t + 15_000
            if (t >= beaconDueAt) { beaconDueAt = t + minutes * 60_000L; sendPositionNow() }
        } else beaconDueAt = 0L
    }

    fun setReportRepeat(extra: Int) = post { Native.setReportRepeat(extra, REPEAT_INTERVAL_MS) }

    private fun afterStart() {
        fecOnlySeen = 0
        Native.setReportRepeat(settings.reportRepeat, REPEAT_INTERVAL_MS)
        Native.setClock(System.currentTimeMillis() / 1000, now())
        Positions.current?.let { Native.mapSetSelf(it.lat, it.lon, it.radiusM, now()) }
        Positions.apply(ctx)
    }

    private fun record(dir: MsgDir, from: String, to: String, kind: String, text: String, clip: ByteArray?, id: Int = -1,
                       via: String? = null) =
        MsgRecord(System.currentTimeMillis(), dir, from, to, kind, text, clip, id, via = via)

    @Volatile private var modemNow = -1 // the selected modem while running (-1: TNC link)

    /** The modem that decoded the message being handled, when it is not the selected one (null otherwise). */
    private fun via(): String? {
        val m = Native.rxModem()
        return if (m >= 0 && modemNow >= 0 && m != modemNow) Native.modemShort(m) else null
    }

    private fun queued(id: Int, kind: Hub.Kind, text: String, clip: ShortArray?, rec: MsgRecord? = null) {
        if (id < 0) {
            val why = when (id) { -2 -> "send queue full, wait for pending messages"; -3 -> "message too long"; else -> "error $id" }
            Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "not sent: $why"))
        } else {
            Hub.ui { Hub.status[id] = "queued"; if (rec != null) { MessageStore.add(rec); MessageStore.setStatus(id, "queued") } }
            Hub.addLog(Hub.LogItem(kind, text, msgId = id, clip = clip))
        }
    }

    // ------------------------------------------------------------------ engine -> UI (MeshListener)
    override fun onMessage(src: String, dst: String, type: Int, data: ByteArray) {
        val via = via()
        val viaNote = via?.let { "  (via $it)" } ?: ""
        when (type) {
            Native.MT_TEXT -> {
                val text = String(data, Charsets.UTF_8)
                val rec = record(MsgDir.IN, src, dst, "text", text, null, via = via)
                Hub.ui { MessageStore.add(rec) }
                Hub.addLog(Hub.LogItem(Hub.Kind.RX_TEXT, "$src: $text$viaNote"))
            }
            Native.MT_REPORT -> {
                // The core already put it on the map; the log gets the one-line summary. Unknown future types: ignored here.
                Native.reportSummary(data)?.let {
                    val rec = record(MsgDir.IN, src, dst, "report", it, null, via = via)
                    Hub.ui { MessageStore.add(rec) }
                    Hub.addLog(Hub.LogItem(Hub.Kind.RX_TEXT, "$src [report] $it$viaNote"))
                }
            }
            Native.MT_VOICE -> {
                val pcm = Native.voiceDecode(data)
                if (pcm == null) {
                    Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "voice clip from $src could not be decoded"))
                } else {
                    val rec = record(MsgDir.IN, src, dst, "voice", "voice clip (%.1f s)".format(pcm.size / 8000.0), data, via = via)
                    Hub.ui { MessageStore.add(rec) }
                    Hub.addLog(Hub.LogItem(Hub.Kind.RX_VOICE, "voice from $src (%.1f s)$viaNote".format(pcm.size / 8000.0), clip = pcm))
                    if (settings.autoPlayVoice && !settings.relay) VoicePlayer.play(ctx, pcm)
                }
            }
        }
    }

    override fun onStatus(msgId: Int, status: Int) {
        val text = when (status) { 1 -> "delivered"; 2 -> "sent"; else -> "FAILED (no ACK)" }
        Hub.ui { Hub.status[msgId] = text; MessageStore.setStatus(msgId, text) }
    }

    override fun onMonitor(src: String, dst: String, type: Int, msgId: Int, data: ByteArray) {
        val via = via()
        val viaNote = via?.let { "  (via $it)" } ?: ""
        when (type) {
            Native.MT_TEXT -> {
                val text = String(data, Charsets.UTF_8)
                val rec = record(MsgDir.HEARD, src, dst, "text", text, null, msgId, via)
                Hub.ui { MessageStore.add(rec) }
                Hub.addLog(Hub.LogItem(Hub.Kind.HEARD, "$src → $dst: $text$viaNote"))
            }
            Native.MT_REPORT -> Native.reportSummary(data)?.let {
                val rec = record(MsgDir.HEARD, src, dst, "report", it, null, msgId, via)
                Hub.ui { MessageStore.add(rec) }
                Hub.addLog(Hub.LogItem(Hub.Kind.HEARD, "$src → $dst [report] $it$viaNote"))
            }
            Native.MT_VOICE -> {
                val pcm = Native.voiceDecode(data)
                val rec = record(MsgDir.HEARD, src, dst, "voice",
                    if (pcm == null) "voice clip" else "voice clip (%.1f s)".format(pcm.size / 8000.0), data, msgId, via)
                Hub.ui { MessageStore.add(rec) }
                Hub.addLog(Hub.LogItem(Hub.Kind.HEARD, "$src → $dst: voice clip$viaNote", clip = pcm))
            }
            Native.MT_ACK -> Hub.ui { MessageStore.setHeardStatus(dst, msgId, "delivered (ACK from $src)") } // dst sent it
            Native.MT_ID -> {
                val rec = record(MsgDir.HEARD, src, "CQ", "id", "station ID", null, via = via)
                Hub.ui { MessageStore.add(rec) }
            }
        }
    }

    override fun onEvent(text: String) {
        Hub.addLog(Hub.LogItem(if (text.startsWith("WARNING")) Hub.Kind.ERROR else Hub.Kind.EVENT, text)) // core warnings in red
    }

    // ------------------------------------------------------------------ audio plumbing
    private fun usb(inputs: Boolean): AudioDeviceInfo? {
        if (!settings.useUsbAudio) return null
        val flag = if (inputs) AudioManager.GET_DEVICES_INPUTS else AudioManager.GET_DEVICES_OUTPUTS
        return audio.getDevices(flag).firstOrNull {
            it.type == AudioDeviceInfo.TYPE_USB_DEVICE || it.type == AudioDeviceInfo.TYPE_USB_HEADSET
        }
    }

    private fun createRecord(): AudioRecord {
        // UNPROCESSED avoids Android's AGC / noise suppression / echo cancel, which mangle tones.
        val src = if (audio.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true")
            MediaRecorder.AudioSource.UNPROCESSED else MediaRecorder.AudioSource.VOICE_RECOGNITION
        val min = AudioRecord.getMinBufferSize(fs, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val rec = AudioRecord.Builder()
            .setAudioSource(src)
            .setAudioFormat(
                AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(fs).setChannelMask(AudioFormat.CHANNEL_IN_MONO).build()
            )
            .setBufferSizeInBytes(max(min, fs * 2))
            .build()
        usb(true)?.let { rec.setPreferredDevice(it) }
        return rec
    }

    private fun createTrack(bufferMs: Int = 500): AudioTrack {
        val min = AudioTrack.getMinBufferSize(fs, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build()
            )
            .setAudioFormat(
                AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(fs).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build()
            )
            .setBufferSizeInBytes(max(min, fs * 2 * bufferMs / 1000))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        usb(false)?.let { t.setPreferredDevice(it) }
        return t
    }

    private fun describeRoute() {
        val i = usb(true); val o = usb(false)
        Hub.ui {
            Hub.audioRoute = if (i != null && o != null) "Audio: USB ${i.productName}"
            else "Audio: phone mic/speaker (acoustic coupling; no USB audio found)"
        }
    }

    // ------------------------------------------------------------------ main loop
    private fun loop() {
        val cs = settings.callsign
        val tnc = settings.transport == "tnc"
        Native.start(cs, settings.relay, settings.ttl, if (tnc) 300 else settings.preambleMs,
            (System.nanoTime() xor cs.hashCode().toLong()).toInt(), this)
        var startModem = settings.modem
        if (!tnc && startModem != Native.MODEM_AFSK && Native.setModem(startModem) != 0) {
            Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "${Native.modemName(startModem)} could not start; using AFSK 1200"))
            startModem = Native.MODEM_AFSK
        }
        Native.setLink(if (tnc) Native.LINK_TNC else Native.LINK_AUDIO, settings.tncBaud)
        Native.setFec(settings.afskFec)
        Native.setMonitor(settings.monitor)
        if (!tnc) Native.setListenAll(settings.listenAll)
        modemNow = if (tnc) -1 else startModem
        Hub.ui { Hub.monitor = settings.monitor }
        afterStart()
        if (tnc) { loopTnc(); return }
        val manual = settings.pttLine == "MANUAL"
        Native.setManualPtt(manual)
        Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "Modem: " + Native.modemName(startModem)))
        updateSummary(startModem)
        Hub.ui { Hub.manualMode = manual; Hub.manualStage = ""; Hub.manualPending = 0 }
        val busyThr = settings.busyThreshold // the carrier-sense mode decides whether level counts
        Native.setCarrierSense(settings.carrierSenseMode)
        Native.setBitRepair(settings.bitRepair)
        var lastLeft = -1
        val p = Ptt(ctx, settings.pttLine).also { ptt = it }
        describeRoute()
        if (!p.usesSerial) Hub.ui { Hub.pttInfo = "PTT: radio VOX (no serial line)" }
        val rec = createRecord()
        rec.startRecording()
        val buf = ShortArray(chunk)
        val meterBuf = FloatArray(5)
        var tick = 0
        Hub.ui { Hub.phase = Hub.Phase.IDLE }
        try {
            while (running) {
                while (true) { (cmds.poll() ?: break).invoke() }
                housekeeping(now())
                if (!p.ready && tick % 100 == 0) { val msg = p.open(); Hub.ui { Hub.pttInfo = msg ?: "PTT: ${settings.pttLine} ready" } }
                if (tick % 250 == 0) describeRoute()

                val n = rec.read(buf, 0, chunk)
                if (n <= 0) { Thread.sleep(20); tick++; continue }
                val t = now()
                val level = Native.processAudio(buf, n, t, busyThr)
                if (tick % 5 == 0) {
                    val busy = Native.channelBusy()
                    Hub.ui { Hub.level = level; Hub.phase = if (busy) Hub.Phase.BUSY else Hub.Phase.IDLE }
                    Native.getMeter(meterBuf)
                    val m = Hub.Meter(meterBuf[0], meterBuf[1], meterBuf[2], meterBuf[3], meterBuf[4].toInt())
                    Hub.ui { Hub.meter = m }
                }
                val tm = testRequest
                if (tm >= 0) {
                    testRequest = -1
                    when {
                        !p.ready -> Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "PTT test not started: PTT line is not ready (see PTT status)"))
                        Native.channelBusy() -> Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "PTT test not started: channel is busy, someone is transmitting"))
                        else -> {
                            rec.stop()
                            runTest(p, tm)
                            rec.startRecording()
                        }
                    }
                    tick++
                    continue
                }
                if (manual) {
                    // Nothing is taken off the queue until the operator taps Transmit and the countdown ends.
                    if (tick % 5 == 0) { val pend = Native.pendingTx(); Hub.ui { Hub.manualPending = pend } }
                    val arm = manualArmAt
                    if (arm != 0L) {
                        if (t >= arm) {
                            manualArmAt = 0L
                            val burst = Native.pollTx(t, settings.txLevel)
                            if (burst == null) {
                                setStage("")
                                Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "Nothing was ready to send (queue empty, channel busy, or waiting on a timer)"))
                            } else {
                                setStage("TRANSMITTING: keep holding the radio's PTT")
                                rec.stop()
                                transmit(p, burst, manual = true)
                                rec.startRecording()
                                setStage("Release the radio's PTT now")
                                stageClearAt = now() + 2500
                            }
                            lastLeft = -1
                        } else {
                            val left = ((arm - t + 999) / 1000).toInt()
                            if (left != lastLeft) { lastLeft = left; setStage("Key the radio's PTT now and HOLD it. Audio starts in $left s") }
                        }
                    }
                    if (stageClearAt != 0L && t >= stageClearAt) { stageClearAt = 0L; setStage("") }
                } else {
                    // Never pull a burst off the queue if we cannot key the radio; ARQ would have to recover it.
                    val burst = if (p.ready) Native.pollTx(t, settings.txLevel) else null
                    if (burst != null) {
                        rec.stop()
                        transmit(p, burst)
                        rec.startRecording()
                    }
                }
                tick++
            }
        } finally {
            p.close()
            try { rec.stop() } catch (_: Exception) {}
            rec.release()
            Native.stop()
            Hub.ui { Hub.phase = Hub.Phase.OFF; Hub.level = 0f; Hub.manualMode = false; Hub.manualStage = "" }
        }
    }

    /** Key PTT, play the pre-modulated burst, wait for it to drain, unkey (always). */
    private fun transmit(p: Ptt, burst: ShortArray, manual: Boolean = false) {
        Hub.ui { Hub.phase = Hub.Phase.TX }
        val track = createTrack()
        var shownLeft = -1
        // Manual PTT: count down the transmission, so the operator knows when letting go is safe.
        fun countdown() {
            if (!manual) return
            val left = ((burst.size - track.playbackHeadPosition).coerceAtLeast(0) + fs - 1) / fs
            if (left != shownLeft) { shownLeft = left; setStage("TRANSMITTING: keep holding the radio's PTT ($left s left)") }
        }
        try {
            track.play()
            p.key(true)
            var off = 0
            val started = now()
            val chunk = fs / 4 // write in quarter seconds so the countdown can update while the audio plays
            while (off < burst.size && running && now() - started < 40_000) {
                val w = track.write(burst, off, minOf(chunk, burst.size - off))
                if (w < 0) break
                off += w
                countdown()
            }
            val deadline = now() + 3000
            while (track.playbackHeadPosition < burst.size && now() < deadline) { countdown(); Thread.sleep(10) }
            Thread.sleep(120) // let the final flags leave the radio before dropping PTT
        } finally {
            p.key(false)
            track.stop()
            track.release()
        }
    }

    /**
     * Bluetooth-TNC mode: no audio and no PTT line. The radio's own modem and CSMA do the RF work; we exchange
     * KISS frames over a Bluetooth SPP socket. The core paces output by estimated airtime.
     */
    private fun loopTnc() {
        val link = BtTnc(ctx, settings.btAddress)
        val name = settings.btName.ifBlank { settings.btAddress }
        Hub.ui { Hub.audioRoute = "Link: Bluetooth TNC $name (${settings.tncBaud} baud)"; Hub.phase = Hub.Phase.IDLE; Hub.level = 0f }
        Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "Bluetooth TNC mode: the radio does the modulation, keying and carrier sense"))
        Hub.ui { Hub.modeSummary = "Bluetooth KISS TNC · ${settings.tncBaud} baud · radio does keying and carrier sense"; Hub.manualMode = false }
        var lastStatus = ""
        var tick = 0
        try {
            while (running) {
                while (true) { (cmds.poll() ?: break).invoke() }
                val t = now()
                housekeeping(t)
                link.service(t)
                if (link.status != lastStatus) { lastStatus = link.status; val st = lastStatus; Hub.ui { Hub.pttInfo = st } }
                var got = false
                while (true) {
                    val b = link.read() ?: break
                    Native.tncRx(b, b.size, t)
                    got = true
                }
                if (!got) Native.tncRx(ByteArray(0), 0, t) // run the ACK/retry/ID timers
                if (link.connected) {
                    val out = Native.tncPollTx(t)
                    if (out != null && !link.write(out))
                        Hub.addLog(Hub.LogItem(Hub.Kind.ERROR, "write to the radio failed; ARQ will retry"))
                }
                tick++
                Thread.sleep(20)
            }
        } finally {
            link.close()
            Native.stop()
            Hub.ui { Hub.phase = Hub.Phase.OFF; Hub.level = 0f }
        }
    }

    /**
     * PTT test / calibration tone. Streams a tone with a live-adjustable level, auto-stops after 15 s,
     * always unkeys, then asks the mesh to queue a station ID (a test is a transmission).
     */
    private fun runTest(p: Ptt, mode: Int) {
        val maxMs = 15_000L
        val label = when (mode) { Native.TONE_OFF -> "PTT only"; Native.TONE_1200 -> "1200 Hz"; Native.TONE_2200 -> "2200 Hz"; else -> "1200/2200 Hz alternating" }
        Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "TX test started: $label"))
        Hub.ui { Hub.phase = Hub.Phase.TX; Hub.testing = true; Hub.testSecondsLeft = (maxMs / 1000).toInt() }
        if (settings.pttLine == "MANUAL") {
            for (i in settings.manualCountdown downTo 1) {
                if (!running || testStopFlag) break
                setStage("Key the radio's PTT now and HOLD it. Test tone starts in $i s")
                Thread.sleep(1000)
            }
            setStage("TEST TONE ON AIR: keep holding the radio's PTT")
        }
        val track = createTrack(bufferMs = 100)
        val out = ShortArray(chunk)
        val t0 = now()
        try {
            Native.toneStart(mode)
            track.play()
            p.key(true)
            var lastLeft = -1
            while (running && !testStopFlag && now() - t0 < maxMs) {
                Native.toneFill(out, chunk, testAmp)
                var off = 0
                while (off < chunk) { // blocking write paces the loop to real time
                    val w = track.write(out, off, chunk - off)
                    if (w < 0) break
                    off += w
                }
                val left = ((maxMs - (now() - t0)) / 1000).toInt() + 1
                if (left != lastLeft) { lastLeft = left; Hub.ui { Hub.testSecondsLeft = left } }
            }
            // fade out (about 5 ms per full scale), then a chunk of silence, then let the USB codec drain
            var guard = 0
            while (Native.toneFill(out, chunk, 0f) > 0f && guard++ < 20) track.write(out, 0, chunk)
            track.write(out, 0, chunk)
            Thread.sleep(250)
        } finally {
            p.key(false)
            try { track.stop() } catch (_: Exception) {}
            track.release()
            Hub.ui { Hub.testing = false }
        }
        if (settings.pttLine == "MANUAL") { setStage("Release the radio's PTT now"); stageClearAt = now() + 2500 }
        Native.noteExternalTx(now(), 300)
        Hub.addLog(Hub.LogItem(Hub.Kind.EVENT, "TX test ended after %.1f s (station ID will follow)".format((now() - t0) / 1000.0)))
    }

    companion object {
        const val REPEAT_INTERVAL_MS = 30_000
    }
}
