// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

/** One icon on the map, parsed from a line of the core's snapshot text (format: core/mapmodel.h). */
data class MapItem(
    val type: Char,          // 'S' station, 'C' contact (situation report)
    val key: String,
    val name: String,
    val lat: Double,
    val lon: Double,
    val radiusM: Double,     // uncertainty ring for grid-square positions, 0 = exact
    val color: String,       // "#RRGGBB", chosen by the report schema
    val shape: String,       // rectangle | square | diamond | quatrefoil
    val alpha: Float,        // ageing: 1.0 fresh, fades toward 0.25 before the report expires
    val ageMin: Int,
    val severity: Int,       // draw order: higher on top
    val label: String,
    val self: Boolean,
) {
    companion object {
        fun parse(text: String): List<MapItem> {
            val out = ArrayList<MapItem>()
            for (line in text.lineSequence()) {
                val f = line.split('\t')
                if (f.size < 13 || f[0].isEmpty()) continue
                try {
                    out.add(MapItem(f[0][0], f[1], f[2], f[3].toDouble(), f[4].toDouble(), f[5].toDouble(), f[6], f[7],
                        f[8].toFloat(), f[9].toDouble().toInt(), f[10].toInt(), f[11], f[12] == "1"))
                } catch (_: NumberFormatException) {
                    // a malformed line never takes the whole map down
                }
            }
            return out
        }
    }
}
