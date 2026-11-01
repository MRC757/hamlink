// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

/** Callbacks from the C core. Always invoked on the engine thread. */
interface MeshListener {
    fun onMessage(src: String, dst: String, type: Int, data: ByteArray)
    /** status: 0 = failed, 1 = delivered (ACKed), 2 = sent (broadcast, no ACK expected) */
    fun onStatus(msgId: Int, status: Int)
    fun onEvent(text: String)
}

/** JNI surface. NOT thread safe: call everything from the engine thread only. */
object Native {
    const val MT_TEXT = 1
    const val MT_VOICE = 2

    init { System.loadLibrary("hamlink_jni") }

    external fun start(callsign: String, relay: Boolean, ttl: Int, preambleMs: Int, seed: Int, listener: MeshListener)
    external fun stop()
    external fun setRelay(on: Boolean)

    // ---- modem and link choice ----
    const val MODEM_AFSK = 0    // 1200 baud Bell 202 AFSK (default)
    const val MODEM_OFDM = 1    // COFDMTV (Rattlegram family): about 4-6 dB more sensitive in simulation
    const val LINK_AUDIO = 0    // sound card/USB audio + PTT
    const val LINK_TNC = 1      // radio with a built-in KISS TNC over Bluetooth SPP
    /** Returns 0, or -1 if OFDM could not start (AFSK stays selected). All stations must use the same modem. */
    external fun setModem(modem: Int): Int
    external fun setLink(link: Int, tncBaud: Int)
    /** True when a human keys the radio for every transmission: stretches ACK/retry timers to 45 s. */
    external fun setManualPtt(on: Boolean)
    /** Frames queued for transmission (ACKs, relays, IDs, fragments), due or not. */
    external fun pendingTx(): Int
    /** TNC link: feed bytes read from the KISS stream (n may be 0 just to run timers). */
    external fun tncRx(bytes: ByteArray, n: Int, nowMs: Long)
    /** TNC link: KISS bytes to write to the radio now, or null. */
    external fun tncPollTx(nowMs: Long): ByteArray?
    /** out8 = {rx frames, dups, tx frames, relayed, suppressed, retries, ofdm ok, ofdm failed} */
    external fun getStats(out8: IntArray)

    /** Feed 48 kHz mono PCM. Returns smoothed RMS level (0..1). Also runs ACK/retry/ID timers. */
    external fun processAudio(pcm: ShortArray, n: Int, nowMs: Long, busyThreshold: Float): Float

    /** Returns message id (0..65535) or a negative error (-2 queue busy, -3 too big). */
    external fun send(dst: String, type: Int, data: ByteArray, ackReq: Boolean, nowMs: Long): Int

    /** If the channel is clear and frames are due: modulated 48 kHz audio for one PTT burst, else null. */
    external fun pollTx(nowMs: Long, amp: Float): ShortArray?
    external fun nextWakeup(): Long

    // ---- PTT test / calibration ----
    const val TONE_OFF = 0      // PTT only, silence
    const val TONE_1200 = 1
    const val TONE_2200 = 2
    const val TONE_ALT = 3      // 1200 Hz for 1 s, then 2200 Hz for 1 s, repeating
    external fun toneStart(mode: Int)
    /** Fills out[0 until n] (48 kHz) toward amplitude amp. Returns the amplitude reached (0 once faded out). */
    external fun toneFill(out: ShortArray, n: Int, amp: Float): Float
    /** Call after a transmission made outside the mesh so the station still IDs. */
    external fun noteExternalTx(nowMs: Long, durationMs: Int)
    /** out5 = {rms dBFS, peak dBFS, 1200 Hz dBFS, 2200 Hz dBFS, clipped samples in last 100 ms} */
    external fun getMeter(out5: FloatArray)

    // ---- reports, map, geo (API v5). Everything except geoParse/geoFormat must run on the engine thread. ----
    const val MT_REPORT = 5
    external fun setClock(unixSeconds: Long, nowMs: Long)
    external fun schemaCount(): Int
    /** JSON describing a report type (fields, choices, colours); the entry forms are built from it. */
    external fun schemaJson(index: Int): String?
    /** "OK:<msgId>" or "ERR:<reason>". [kv] is key=value lines (see docs/MAPS_AND_REPORTS.md). */
    external fun reportSend(dst: String, schema: String, kv: String, ackReq: Boolean, nowMs: Long): String
    external fun reportSummary(data: ByteArray): String?
    /** Our own position from any source (GPS, typed, grid square); radiusM = uncertainty. Does not transmit. */
    external fun mapSetSelf(lat: Double, lon: Double, radiusM: Double, nowMs: Long)
    /** Tab separated item lines, format documented in core/mapmodel.h. */
    external fun mapSnapshot(nowMs: Long): String
    external fun mapDetail(key: String, nowMs: Long): String?
    external fun mapClear()
    /** Send each broadcast report we originate [extra] more times (0 = off), first about [intervalMs] later plus 0-10 s.
     *  Receivers drop the identical copies. Addressed reports are not repeated (ARQ covers them). */
    external fun setReportRepeat(extra: Int, intervalMs: Int)
    /** Stateless, any thread. Typed lat/lon, DMS, Maidenhead or MGRS -> {lat, lon, radiusM, kind(0 latlon,1 grid,2 mgrs)}. */
    external fun geoParse(text: String): DoubleArray?
    /** fmt: 0 decimal, 1 DMS, 2 Maidenhead, 3 MGRS. */
    external fun geoFormat(lat: Double, lon: Double, fmt: Int): String

    // ---- receive DSP options (API v7) ----
    const val CS_OFF = 0             // never hold off
    const val CS_DATA = 1            // hold off only while data is detected (ignores room noise and voice)
    const val CS_DATA_OR_LEVEL = 2   // data detected, or audio above the busy threshold (default)
    external fun setCarrierSense(mode: Int)
    /** Single-bit repair of AFSK frames that fail their CRC (default on). */
    external fun setBitRepair(on: Boolean)
    /** The carrier-sense decision from the last processAudio call. */
    external fun channelBusy(): Boolean
    /** out6 = {data detected now, AFSK frames decoded, repaired, rejected ambiguous, rejected by structure check, busy} */
    external fun getRxInfo(out6: IntArray)

    external fun voiceEncode(pcm8k: ShortArray, n: Int): ByteArray?
    external fun voiceDecode(clip: ByteArray): ShortArray?
}
