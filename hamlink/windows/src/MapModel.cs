// SPDX-License-Identifier: GPL-3.0-or-later
using System.Globalization;

namespace HamLink;

/// <summary>One icon on the map, parsed from a line of the core's snapshot text (format: core/mapmodel.h).</summary>
internal sealed record MapItem(
    char Type,           // 'S' station, 'C' contact (situation report)
    string Key,
    string Name,
    double Lat,
    double Lon,
    double RadiusM,      // uncertainty ring for grid-square positions, 0 = exact
    string Color,        // "#RRGGBB", chosen by the report schema
    string Shape,        // rectangle | square | diamond | quatrefoil
    double Alpha,        // ageing: 1.0 fresh, fades toward 0.25 before the report expires
    int AgeMin,
    int Severity,        // draw order: higher on top
    string Label,
    bool Self)
{
    public static List<MapItem> Parse(string text)
    {
        var list = new List<MapItem>();
        foreach (var line in text.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            var f = line.Split('\t');
            if (f.Length < 13 || f[0].Length == 0) continue;
            try
            {
                var c = CultureInfo.InvariantCulture;
                list.Add(new MapItem(f[0][0], f[1], f[2], double.Parse(f[3], c), double.Parse(f[4], c), double.Parse(f[5], c),
                    f[6], f[7], double.Parse(f[8], c), (int)double.Parse(f[9], c), int.Parse(f[10], c), f[11], f[12] == "1"));
            }
            catch (FormatException) { /* one malformed line never takes the whole map down */ }
        }
        return list;
    }
}
