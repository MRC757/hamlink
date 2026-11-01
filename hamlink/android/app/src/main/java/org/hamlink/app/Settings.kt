// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.content.Context

class Settings(ctx: Context) {
    private val sp = ctx.getSharedPreferences("hamlink", Context.MODE_PRIVATE)

    var callsign: String
        get() = sp.getString("callsign", "") ?: ""
        set(v) = sp.edit().putString("callsign", v.trim().uppercase()).apply()
    var relay: Boolean
        get() = sp.getBoolean("relay", false)
        set(v) = sp.edit().putBoolean("relay", v).apply()
    /** Max relay hops a frame we originate may take (1 = one simplex relay). */
    var ttl: Int
        get() = sp.getInt("ttl", 1)
        set(v) = sp.edit().putInt("ttl", v.coerceIn(0, 3)).apply()
    /** Lead-in of flags after PTT, covers the radio's PTT-to-RF delay. */
    var preambleMs: Int
        get() = sp.getInt("preambleMs", 500)
        set(v) = sp.edit().putInt("preambleMs", v.coerceIn(200, 1500)).apply()
    /** Peak amplitude of the transmitted audio, 0..1. Calibrate against radio deviation! */
    var txLevel: Float
        get() = sp.getFloat("txLevel", 0.30f)
        set(v) = sp.edit().putFloat("txLevel", v.coerceIn(0.02f, 1f)).apply()
    /** RX RMS (0..1 of full scale) above which the channel counts as busy. */
    var busyThreshold: Float
        get() = sp.getFloat("busyThreshold", 0.004f)
        set(v) = sp.edit().putFloat("busyThreshold", v.coerceIn(0.0005f, 0.2f)).apply()
    /** "RTS" / "DTR" (serial line), "VOX" (no PTT line: transmit at once; the radio's own VOX or nothing at all)
     *  or "MANUAL" (you press the radio's PTT yourself when the app counts down). */
    var pttLine: String
        get() = sp.getString("pttLine", "RTS") ?: "RTS"
        set(v) = sp.edit().putString("pttLine", v).apply()
    var useUsbAudio: Boolean
        get() = sp.getBoolean("useUsbAudio", true)
        set(v) = sp.edit().putBoolean("useUsbAudio", v).apply()
    var autoPlayVoice: Boolean
        get() = sp.getBoolean("autoPlayVoice", true)
        set(v) = sp.edit().putBoolean("autoPlayVoice", v).apply()
    var dest: String
        get() = sp.getString("dest", "CQ") ?: "CQ"
        set(v) = sp.edit().putString("dest", v.trim().uppercase()).apply()

    /** 0 = AFSK 1200 baud, 1 = OFDM (COFDMTV). Every station on the channel must use the same one. */
    var modem: Int
        get() = sp.getInt("modem", 0)
        set(v) = sp.edit().putInt("modem", if (v == 1) 1 else 0).apply()
    /** "audio" = USB audio interface + PTT line, "tnc" = radio with built-in KISS TNC over Bluetooth. */
    var transport: String
        get() = sp.getString("transport", "audio") ?: "audio"
        set(v) = sp.edit().putString("transport", if (v == "tnc") "tnc" else "audio").apply()
    var btAddress: String
        get() = sp.getString("btAddress", "") ?: ""
        set(v) = sp.edit().putString("btAddress", v).apply()
    var btName: String
        get() = sp.getString("btName", "") ?: ""
        set(v) = sp.edit().putString("btName", v).apply()
    /** Speed of the radio's TNC (1200 or 9600); only used to estimate airtime for ACK timing. */
    var tncBaud: Int
        get() = sp.getInt("tncBaud", 1200)
        set(v) = sp.edit().putInt("tncBaud", if (v >= 9600) 9600 else 1200).apply()

    /** Seconds between tapping Transmit and the audio starting, in MANUAL PTT mode (time to key the radio). */
    var manualCountdown: Int
        get() = sp.getInt("manualCountdown", 3)
        set(v) = sp.edit().putInt("manualCountdown", v.coerceIn(2, 10)).apply()
    /** Hold off transmitting while the receive audio is loud. Turn off for acoustic tests where room noise keeps it "busy". */
    var carrierSense: Boolean
        get() = carrierSenseMode != Native.CS_OFF
        set(v) { carrierSenseMode = if (v) Native.CS_DATA_OR_LEVEL else Native.CS_OFF }
    /** 0 = off, 1 = only while data is detected (ignores room noise and voice: use for acoustic tests),
     *  2 = data or loud audio (default). Older versions stored an on/off switch, read here as 2 / 0. */
    var carrierSenseMode: Int
        get() = sp.getInt("carrierSenseMode", if (sp.getBoolean("carrierSense", true)) Native.CS_DATA_OR_LEVEL else Native.CS_OFF)
        set(v) = sp.edit().putInt("carrierSenseMode", v.coerceIn(0, 2)).apply()
    /** Repair AFSK frames with a single wrong bit (default on). */
    var bitRepair: Boolean
        get() = sp.getBoolean("bitRepair", true)
        set(v) = sp.edit().putBoolean("bitRepair", v).apply()

    // ---- position and map ----
    /** "gps" = phone GPS, "typed" = lat/lon (decimal, DMS or MGRS) typed in, "grid" = Maidenhead grid square typed in. */
    var positionMode: String
        get() = sp.getString("positionMode", "gps") ?: "gps"
        set(v) = sp.edit().putString("positionMode", if (v == "typed" || v == "grid") v else "gps").apply()
    var typedPosition: String
        get() = sp.getString("typedPosition", "") ?: ""
        set(v) = sp.edit().putString("typedPosition", v.trim()).apply()
    /** Minutes between automatic position beacons; 0 = off (default). Each beacon is a transmission. */
    var beaconMin: Int
        get() = sp.getInt("beaconMin", 0)
        set(v) = sp.edit().putInt("beaconMin", if (v in 1..120) v else 0).apply()
    /** Extra copies of each position/SALUTE report we broadcast, about 30 s apart (0 = off, the default). Each copy is a
     *  transmission; receivers show the report once. */
    var reportRepeat: Int
        get() = sp.getInt("reportRepeat", 0)
        set(v) = sp.edit().putInt("reportRepeat", v.coerceIn(0, 2)).apply()
    /** Slippy-map tile URL template. The default is OpenStreetMap's public server: see the usage policy in docs. */
    var tileUrl: String
        get() = sp.getString("tileUrl", DEFAULT_TILE_URL) ?: DEFAULT_TILE_URL
        set(v) = sp.edit().putString("tileUrl", v.trim().ifEmpty { DEFAULT_TILE_URL }).apply()
    var tileAttribution: String
        get() = sp.getString("tileAttribution", DEFAULT_ATTRIBUTION) ?: DEFAULT_ATTRIBUTION
        set(v) = sp.edit().putString("tileAttribution", v.trim().ifEmpty { DEFAULT_ATTRIBUTION }).apply()
    /** Contact (email or URL) sent in the User-Agent of tile requests; falls back to the callsign. */
    var tileContact: String
        get() = sp.getString("tileContact", "") ?: ""
        set(v) = sp.edit().putString("tileContact", v.trim()).apply()
    /** Off = use only tiles already on the device. */
    var tileOnline: Boolean
        get() = sp.getBoolean("tileOnline", true)
        set(v) = sp.edit().putBoolean("tileOnline", v).apply()
    var tileCacheMb: Int
        get() = sp.getInt("tileCacheMb", 300)
        set(v) = sp.edit().putInt("tileCacheMb", v.coerceIn(20, 4000)).apply()

    fun callsignValid(): Boolean =
        Regex("^[A-Z0-9/-]{3,8}$").matches(callsign) && callsign != "CQ"

    companion object {
        const val DEFAULT_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
        const val DEFAULT_ATTRIBUTION = "\u00A9 OpenStreetMap contributors"
    }
}
