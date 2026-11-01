# SPDX-License-Identifier: GPL-3.0-or-later
"""ctypes access to the geo/nmea/report C code for tests (built into the hamlink_api shared library)."""
import ctypes as C
import os
import subprocess
from hamlink import BUILD

subprocess.check_call(["cmake", "--build", BUILD, "--target", "hamlink_api", "-j8"], stdout=subprocess.DEVNULL)
G = C.CDLL(os.path.join(BUILD, "core", "libhamlink_api.so"))

G.hl_geo_parse.argtypes = [C.c_char_p, C.POINTER(C.c_double), C.POINTER(C.c_double), C.POINTER(C.c_double), C.POINTER(C.c_int)]
G.hl_geo_format.argtypes = [C.c_double, C.c_double, C.c_int, C.c_char_p, C.c_int]
G.hl_geo_maidenhead.argtypes = [C.c_double, C.c_double, C.c_int, C.c_char_p, C.c_int]
G.hl_geo_mgrs.argtypes = [C.c_double, C.c_double, C.c_int, C.c_char_p, C.c_int]
G.hl_geo_to_world.argtypes = [C.c_double, C.c_double, C.c_int, C.POINTER(C.c_double), C.POINTER(C.c_double)]
G.hl_geo_from_world.argtypes = [C.c_double, C.c_double, C.c_int, C.POINTER(C.c_double), C.POINTER(C.c_double)]
G.hl_geo_distance_m.restype = C.c_double
G.hl_geo_distance_m.argtypes = [C.c_double] * 4
G.hl_geo_bearing_deg.restype = C.c_double
G.hl_geo_bearing_deg.argtypes = [C.c_double] * 4


def parse(text):
    la, lo, rad, kind = C.c_double(), C.c_double(), C.c_double(), C.c_int()
    ok = G.hl_geo_parse(text.encode(), C.byref(la), C.byref(lo), C.byref(rad), C.byref(kind))
    return (la.value, lo.value, rad.value, kind.value) if ok else None


def fmt(lat, lon, f):
    b = C.create_string_buffer(64)
    return b.value.decode() if G.hl_geo_format(lat, lon, f, b, 64) and b.value else (b.value.decode() or None)


def grid(lat, lon, chars):
    b = C.create_string_buffer(16)
    return b.value.decode() if G.hl_geo_maidenhead(lat, lon, chars, b, 16) else None


def mgrs(lat, lon, digits=10):
    b = C.create_string_buffer(40)
    return b.value.decode() if G.hl_geo_mgrs(lat, lon, digits, b, 40) else None


def to_world(lat, lon, z):
    x, y = C.c_double(), C.c_double()
    G.hl_geo_to_world(lat, lon, z, C.byref(x), C.byref(y))
    return x.value, y.value


def from_world(x, y, z):
    la, lo = C.c_double(), C.c_double()
    G.hl_geo_from_world(x, y, z, C.byref(la), C.byref(lo))
    return la.value, lo.value
