# SPDX-License-Identifier: GPL-3.0-or-later
"""Geo core checked against independent reference implementations (maidenhead, mgrs, pyproj)."""
import ctypes as C
import math
import numpy as np
import pytest
from geolib import *
import maidenhead as mh
import mgrs as mgrslib
from pyproj import Geod

RNG = np.random.default_rng(42)


def exact_maidenhead(lat, lon, chars):
    """Ground truth with exact rational arithmetic (the maidenhead package slips on floating point at cell edges)."""
    from fractions import Fraction as F
    x, y = F(lon) + 180, F(lat) + 90
    counts, wx, wy, out = [18, 10, 24, 10, 24], F(20), F(10), ""
    for level in range(chars // 2):
        if level:
            wx, wy = wx / counts[level], wy / counts[level]
        ix, iy = int(x // wx), int(y // wy)
        x, y = x - ix * wx, y - iy * wy
        out += (chr(ord("A" if level == 0 else "a") + ix) + chr(ord("A" if level == 0 else "a") + iy)) if level % 2 == 0 else f"{ix}{iy}"
    return out


def test_maidenhead_matches_exact_arithmetic():
    for _ in range(3000):
        la, lo = RNG.uniform(-89.9, 89.9), RNG.uniform(-179.9, 179.9)
        for chars in (4, 6, 8, 10):
            assert grid(la, lo, chars) == exact_maidenhead(la, lo, chars), (la, lo, chars)


def test_maidenhead_known_locators():
    assert grid(41.714775, -72.727260, 6) == "FN31pr"          # ARRL headquarters
    assert grid(38.8895, -77.0353, 6) == "FM18lv"
    lat, lon, rad, kind = parse("FN31pr")
    assert kind == 1 and abs(lat - 41.7292) < 0.001 and abs(lon + 72.7083) < 0.001
    assert 3000 < rad < 6000                                   # 6-char square: half-diagonal a few km
    assert parse("fn31PR")[:2] == parse("FN31pr")[:2]          # case insensitive


def test_maidenhead_decode_centres_match_reference():
    for g in ("FN31", "FN31pr", "JN58td", "QF22ne", "FM18lv55", "IO91wm"):
        la, lo, rad, kind = parse(g)
        rl, ro = mh.to_location(g, center=True)
        assert abs(la - rl) < 1e-6 and abs(lo - ro) < 1e-6, g
    for chars in (4, 6, 8, 10):
        la, lo = RNG.uniform(-80, 80), RNG.uniform(-170, 170)
        g = grid(la, lo, chars)
        assert grid(*parse(g)[:2], chars) == g                 # decode -> encode is stable


def test_mgrs_matches_reference_library():
    m = mgrslib.MGRS()
    pts = [(RNG.uniform(-79.9, 83.9), RNG.uniform(-179.9, 179.9)) for _ in range(3000)]
    pts += [(60.39, 5.32), (78.22, 15.65), (78.22, 25.0), (78.22, 35.0), (-33.87, 151.21), (38.8895, -77.0353),
            (0.0, 0.0), (-0.0001, 179.99), (51.5, -0.12), (64.1, 10.0)]   # Norway, Svalbard, equator, hemispheres
    bad = []
    for la, lo in pts:
        ours = mgrs(la, lo, 10).replace(" ", "")
        ref = m.toMGRS(la, lo, MGRSPrecision=5)
        if ours != ref: bad.append((la, lo, ours, ref))
    assert len(bad) <= 3, bad[:5]                              # only truncation ties at square edges may differ


def test_mgrs_round_trip_and_known_point():
    assert mgrs(38.8895, -77.0353).replace(" ", "") == mgrslib.MGRS().toMGRS(38.8895, -77.0353, MGRSPrecision=5)
    for _ in range(500):
        la, lo = RNG.uniform(-79.9, 83.9), RNG.uniform(-179.9, 179.9)
        s = mgrs(la, lo, 10)
        la2, lo2, rad, kind = parse(s)
        assert kind == 2
        assert G.hl_geo_distance_m(la, lo, la2, lo2) < 2.0, s     # centre of a 1 m square is within ~0.7 m
    for digits, size in ((8, 10), (6, 100), (4, 1000)):
        s = mgrs(38.8895, -77.0353, digits)
        la, lo, rad, kind = parse(s)
        assert abs(rad - size / 2 * 1.41421356) < 1e-6 * size
        assert G.hl_geo_distance_m(38.8895, -77.0353, la, lo) <= rad + 1.0
    assert parse("18SUJ2347806483")[:2] == parse("18S UJ 23478 06483")[:2]


def test_web_mercator_known_values_and_round_trip():
    assert to_world(0, 0, 1) == (256.0, 256.0)
    x, y = to_world(52.52, 13.405, 10)                          # Berlin
    assert (int(x // 256), int(y // 256)) == (550, 335)
    for z in (0, 5, 12, 18):
        la, lo = RNG.uniform(-84, 84), RNG.uniform(-179, 179)
        la2, lo2 = from_world(*to_world(la, lo, z), z)
        assert abs(la - la2) < 1e-9 and abs(lo - lo2) < 1e-9
    assert to_world(89.9, 0, 3)[1] == to_world(85.0511287798, 0, 3)[1]   # clamped at the Mercator limit


def test_distance_and_bearing():
    g = Geod(ellps="WGS84")
    for _ in range(100):
        a, b = (RNG.uniform(-70, 70), RNG.uniform(-170, 170)), (RNG.uniform(-70, 70), RNG.uniform(-170, 170))
        _, _, d = g.inv(a[1], a[0], b[1], b[0])
        assert abs(G.hl_geo_distance_m(*a, *b) - d) / d < 0.006           # sphere vs ellipsoid
    assert abs(G.hl_geo_bearing_deg(0, 0, 0, 10) - 90) < 1e-9
    assert abs(G.hl_geo_bearing_deg(0, 0, 10, 0) - 0) < 1e-9


@pytest.mark.parametrize("text,expect", [
    ("38.8895 -77.0353", (38.8895, -77.0353)),
    ("38.8895, -77.0353", (38.8895, -77.0353)),
    ("38.8895N 77.0353W", (38.8895, -77.0353)),
    ("N38.8895 W77.0353", (38.8895, -77.0353)),
    ("77.0353W 38.8895N", (38.8895, -77.0353)),                         # longitude first is fine with letters
    ("38 53 22.2 N 77 2 7.1 W", (38.8895, -77.035306)),
    ("38°53'22.2\"N 77°02'07.1\"W", (38.8895, -77.035306)),    # degree sign, quotes
    ("38 53.37 N, 77 02.118 W", (38.8895, -77.0353)),
    ("-33.8688 151.2093", (-33.8688, 151.2093)),
    ("33.8688S 151.2093E", (-33.8688, 151.2093)),
    ("38 53 22.2 -77 2 7.1", (38.8895, -77.035306)),                    # DMS with signs, no letters
    ("  0 0  ", (0.0, 0.0)),
])
def test_typed_coordinates_parse(text, expect):
    r = parse(text)
    assert r and abs(r[0] - expect[0]) < 1e-5 and abs(r[1] - expect[1]) < 1e-5 and r[3] == 0 and r[2] == 0.0


@pytest.mark.parametrize("text", ["", "hello", "95 10", "10 200", "38.8 N 77.0 N", "38 75 10 N 77 0 W", "FM18zz",
                                  "38.8895", "1 2 3", "99X QQ 123 456", "N 38.8", "38.8 N 77.0 W 1"])
def test_bad_input_is_rejected(text):
    assert parse(text) is None


def test_formatting():
    assert fmt(38.8895, -77.0353, 0) == "38.88950 -77.03530"
    assert fmt(38.8895, -77.0353, 1) == "38d 53' 22.2\" N  077d 02' 07.1\" W"
    assert fmt(-33.8688, 151.2093, 1) == "33d 52' 07.7\" S  151d 12' 33.5\" E"
    assert fmt(38.8895, -77.0353, 2) == "FM18lv" + grid(38.8895, -77.0353, 8)[6:]
    assert fmt(38.8895, -77.0353, 3).startswith("18S UJ ")
    # carries over correctly at rounding boundaries
    assert fmt(10.999999, 0.0, 1).startswith("11d 00' 00.0\" N")


def test_nmea_parser():
    import ctypes as C
    from test_api import A, N
    A.hl_nmea_feed.argtypes = [C.c_void_p, C.c_char_p, C.c_int, C.POINTER(C.c_double)]
    def sentence(body):
        cs = 0
        for ch in body: cs ^= ord(ch)
        return f"${body}*{cs:02X}\r\n"
    rmc = sentence("GPRMC,123519,A,3853.3700,N,07702.1180,W,010.0,084.4,230394,003.1,W")
    gga = sentence("GPGGA,123519,3853.3700,N,07702.1180,W,1,08,0.9,545.4,M,46.9,M,,")
    n = N("W1AAA")
    out = (C.c_double * 6)()
    data = (gga + rmc).encode()
    got = []
    for i in range(0, len(data), 7):                              # awkward chunking
        chunk = data[i:i + 7]
        if A.hl_nmea_feed(n.c, chunk, len(chunk), out): got.append(list(out))
    assert len(got) == 2
    lat, lon, spd, course, hdop, sats = got[-1]
    assert abs(lat - 38.8895) < 1e-5 and abs(lon + 77.0353) < 1e-4
    assert abs(spd - 18.52) < 0.01 and abs(course - 84.4) < 1e-9 and abs(hdop - 0.9) < 1e-9 and sats == 8
    for bad in (rmc[:-4] + ("0" if rmc[-4] != "0" else "1") + rmc[-3:], sentence("GPRMC,123519,V,3853.37,N,07702.118,W,0,0,230394,,"),
                sentence("GPGGA,123519,3853.37,N,07702.118,W,0,00,99.9,,M,,M,,"), sentence("GPRMC,1,A,9999.00,N,07702.118,W,0,0,1,,")):
        b = bad.encode()
        assert not A.hl_nmea_feed(n.c, b, len(b), out), bad
    southwest = sentence("GNRMC,010203,A,3351.6000,S,15112.5000,E,0.0,0.0,010120,,").encode()   # GN talker, south/east
    assert A.hl_nmea_feed(n.c, southwest, len(southwest), out) and out[0] < 0 and out[1] > 0
