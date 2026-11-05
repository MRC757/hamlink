#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the HamLink icon (an antenna mast sending radio waves) as a PNG, from the same shapes as the Android launcher
icon (android/app/src/main/res/drawable/ic_launcher_foreground.xml, 108 x 108 units on #1D4E89).

    python tools/make_icon.py fastlane/metadata/android/en-US/images/icon.png      # 512 x 512, for F-Droid
    python tools/make_icon.py icon.png --size 256

Needs Pillow (pip install pillow).
"""
import argparse
import math

from PIL import Image, ImageDraw

BG = (0x1D, 0x4E, 0x89)
WHITE = (255, 255, 255)
TIP = (0xFF, 0xC9, 0x4A)


def arc_centre(x, y1, y2, r, left):
    """Centre of a circle of radius r through (x, y1) and (x, y2), on the side away from the bulge."""
    half = abs(y2 - y1) / 2
    d = math.sqrt(r * r - half * half)
    return (x + d if left else x - d), (y1 + y2) / 2


def draw(size):
    ss = 4                                    # supersample, then scale down: smooth edges
    k = size * ss / 108.0
    img = Image.new("RGBA", (size * ss, size * ss), (0, 0, 0, 0))
    g = ImageDraw.Draw(img)
    g.rounded_rectangle([0, 0, size * ss - 1, size * ss - 1], radius=int(size * ss * 0.18), fill=BG)

    def line(x1, y1, x2, y2, w):
        g.line([x1 * k, y1 * k, x2 * k, y2 * k], fill=WHITE, width=int(w * k))
        for x, y in ((x1, y1), (x2, y2)):     # round caps
            g.ellipse([(x - w / 2) * k, (y - w / 2) * k, (x + w / 2) * k, (y + w / 2) * k], fill=WHITE)

    def wave(x, y1, y2, r, w, left):
        cx, cy = arc_centre(x, y1, y2, r, left)
        a = math.degrees(math.atan2(abs(y2 - y1) / 2, abs(cx - x)))
        start, end = (180 - a, 180 + a) if left else (-a, a)
        R = r + w / 2                         # Pillow strokes inward from the box: centre the stroke on radius r
        g.arc([(cx - R) * k, (cy - R) * k, (cx + R) * k, (cy + R) * k], start, end, fill=WHITE, width=int(w * k))
        for yy in (y1, y2):
            g.ellipse([(x - w / 2) * k, (yy - w / 2) * k, (x + w / 2) * k, (yy + w / 2) * k], fill=WHITE)

    # the icon fills more of a store tile than of a launcher's safe zone: draw the 108-unit design about 1.4x larger
    # around its centre (54, 52)
    zoom = 1.4

    def z(v, c):
        return c + (v - c) * zoom

    def L(x1, y1, x2, y2, w):
        line(z(x1, 54), z(y1, 52), z(x2, 54), z(y2, 52), w * zoom)

    def W(x, y1, y2, r, w, left):
        wave(z(x, 54), z(y1, 52), z(y2, 52), r * zoom, w * zoom, left)

    L(54, 48, 54, 78, 4)                      # mast
    L(44, 78, 64, 78, 4)                      # base
    W(45, 35, 53, 13, 3.5, True)              # inner waves
    W(63, 35, 53, 13, 3.5, False)
    W(37, 28, 60, 23, 3.5, True)              # outer waves
    W(71, 28, 60, 23, 3.5, False)
    cx, cy, r = z(54, 54) * k, z(44, 52) * k, 5 * zoom * k   # tip
    g.ellipse([cx - r, cy - r, cx + r, cy + r], fill=TIP)
    return img.resize((size, size), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=512)
    a = ap.parse_args()
    draw(a.size).save(a.out)
    print(f"wrote {a.out} ({a.size} x {a.size})")


if __name__ == "__main__":
    main()
