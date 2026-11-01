#!/usr/bin/env python3
"""Draws the launcher's icon (launcher.ico): Home's backdrop in a rounded square - Vice City at dusk, a striped hot-pink
sun going down behind the art deco towers of the beach, a line of light on the horizon and the sun on the sea. Colours
are the dark palette's (launcher/ui/theme.h). Standard library only.

    python launcher/make_icon.py            writes launcher/launcher.ico (16, 20, 24, 32, 40, 48, 64, 256 px) and
                                            launcher/launcher.png (256 px: the Linux .desktop entry's icon)
    python launcher/make_icon.py --png DIR  also writes each size as DIR/icon_<size>.png (to look at)
    python launcher/make_icon.py --header OUT.h   only the X11 window icon (_NET_WM_ICON: 32, 64, 128 px as
                                            ARGB) as a C header - launcher/CMakeLists.txt runs it on Linux

Drawn here rather than kept as artwork, so the repository holds the source of every byte. Small sizes draw less (no
windows or stars below 128 px, no towers below 24) and up to 64 px put the scene's edges and the sun's stripes on whole
pixels so they stay sharp.
"""
import math
import os
import struct
import sys
import zlib


def rgb(v):
    return ((v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255)


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


# theme.h, kDarkPalette
NIGHT, SKY, HORIZON, SEA = rgb(0x0A0A0B), rgb(0x140A0F), rgb(0x3A0E24), rgb(0x0F080B)
PINK, PINK_HI, WHITE = rgb(0xFF2E8B), rgb(0xFF62A8), rgb(0xFFFFFF)
SKYLINE, LIGHTS = rgb(0x0B0709), rgb(0xFFB8D6)
SUN_TOP = mix(PINK_HI, WHITE, 0.22)  # as backdrop.cpp's sun
FAR = mix(SKYLINE, HORIZON, 0.55)     # the towers in the haze


def hash01(n):
    """backdrop.cpp's hash: the same 'random' numbers every time, in [0, 1)."""
    n &= 0xFFFFFFFF
    n ^= n >> 16
    n = (n * 0x7FEB352D) & 0xFFFFFFFF
    n ^= n >> 15
    n = (n * 0x846CA68B) & 0xFFFFFFFF
    n ^= n >> 16
    return (n & 0xFFFFFF) / 16777216


class Scene:
    """The icon's shapes for one size, in [0, 1) units; edges on whole pixels when the icon is small."""

    def __init__(self, size):
        self.size = size
        small = size <= 64
        q = (lambda v: round(v * size) / size) if small else (lambda v: v)
        self.radius = 0.19  # the rounded square's corners
        self.horizon = q(0.70)
        self.sun = (0.5, self.horizon - 0.15, 0.33)
        # the sun's stripes: gaps that widen toward the horizon (backdrop.cpp's yAt and gap)
        cx, cy, r = self.sun
        start, span = cy - r * 0.25, self.horizon - (cy - r * 0.25)
        self.gaps = []
        if small:  # whole rows of pixels, counted up from the horizon's line: one-pixel slices of sun between them
            rows = {16: [2, 4], 20: [2, 4], 24: [2, 4, 6], 32: [2, 3, 5, 7], 40: [2, 3, 5, 8],
                    48: [2, 3, 5, 6, 9], 64: [2, 3, 4, 6, 7, 10, 14]}[size]
            self.gaps = [(self.horizon - (k + 1) / size, self.horizon - k / size) for k in rows]
        else:
            bands = 5
            for k in range(bands):
                u0, u1 = k / bands, (k + 1) / bands
                gap = 0.2 + 0.5 * (u0 + u1) * 0.5
                y_at = lambda u: start + span * u * (1.35 - 0.35 * u)
                self.gaps.append((y_at(u0 + (u1 - u0) * (1 - gap)), y_at(u1)))
        # the towers in front, each (left, right, height, style): 0 flat, 1 stepped, 2 parapet, 3 crown and spire. The
        # tall one stands against the sun's left side, the low middle leaves its stripes clear
        h = self.horizon
        towers = [(0.07, 0.19, 0.15, 1), (0.185, 0.305, 0.31, 3), (0.30, 0.395, 0.12, 2),
                  (0.615, 0.725, 0.21, 1), (0.72, 0.835, 0.13, 2), (0.83, 0.94, 0.085, 0)]
        if size <= 20:  # too small for a skyline: the sun and the sea
            towers = []
        self.towers = []
        for left, right, height, style in towers:
            body = (q(left), q(h - height), q(right), h + 0.01)
            w, top = body[2] - body[0], body[1]
            parts = [body]
            if style == 1:
                parts += [(q(left + w * 0.14), q(top - height * 0.08), q(right - w * 0.14), top + 0.001),
                          (q(left + w * 0.30), q(top - height * 0.15), q(right - w * 0.30), top + 0.001)]
            elif style == 2:
                parts += [(q(left + w * 0.08), q(top - height * 0.04), q(right - w * 0.08), top + 0.001),
                          (q(left + w * 0.36), q(top - height * 0.10), q(right - w * 0.36), top + 0.001)]
            elif style == 3:
                spire = max(0.012, 1 / size)
                mid = (left + right) / 2
                parts += [(q(left + w * 0.18), q(top - height * 0.07), q(right - w * 0.18), top + 0.001),
                          (q(mid - spire / 2), q(top - height * 0.30), q(mid - spire / 2) + spire, top + 0.001)]
            self.towers.append((body, parts))
        # the far row in the haze: low, irregular, all along the horizon
        self.far = []
        x, i = 0.0, 0
        while x < 1 and size > 20:
            bw = 0.035 + 0.04 * hash01(i * 13 + 1)
            bh = (0.02 + 0.05 * hash01(i * 13 + 2)) * (0.3 + 0.7 * min(1.0, abs(x + bw / 2 - 0.5) / 0.22))
            self.far.append((x, h - bh, x + bw + 0.002, h + 0.01))
            x += bw
            i += 1
        # lit windows (large sizes): a grid on each tower, about a third on
        self.windows = []
        if size >= 128:
            ww, wh, gx, gy = 0.011, 0.015, 0.026, 0.034
            for b, ((x0, y0, x1, y1), _) in enumerate(self.towers):
                cols = int((x1 - x0 - 0.02) / gx)
                rows = int((h - y0 - 0.03) / gy)
                left = x0 + (x1 - x0 - (cols - 1) * gx - ww) / 2
                for row in range(rows):
                    for col in range(cols):
                        if hash01(b * 977 + row * 31 + col + 11) < 0.42:
                            wx, wy = left + col * gx, y0 + 0.03 + row * gy
                            a = 0.55 + 0.45 * hash01(b * 977 + row * 31 + col + 5003)
                            self.windows.append((wx, wy, wx + ww, wy + wh, a))
        # stars (large sizes): high in the sky, away from the sun
        self.stars = []
        if size >= 128:
            for i in range(28):
                sx, sy = 0.06 + 0.88 * hash01(i * 3), 0.05 + 0.32 * hash01(i * 3 + 1) ** 1.4
                if math.hypot(sx - cx, sy - cy) < r + 0.06:
                    continue
                self.stars.append((sx, sy, 0.003 + 0.0035 * hash01(i * 11), 0.3 + 0.5 * hash01(i * 3 + 2)))
        # the sun on the sea: short lines, narrowing and fading with depth
        lines = 2 if size <= 20 else 3 if size <= 32 else 5 if small else 6
        self.reflection = []
        for i in range(lines):
            if small:
                y, th, a = q(h + (0.045 + 0.07 * i) * (0.30 / (h * 0.43))), max(0.018, 1 / size), 1 - 0.14 * i
                width = q(r * (1.5 - 0.22 * i) / 2) * 2
            else:  # closer together near the horizon, thinner and fainter going out
                y, th, a = h + 0.035 + 0.03 * i + 0.006 * i * i, 0.012 - 0.0008 * i, 0.85 - 0.11 * i
                width = r * (1.55 - 0.17 * i)
            self.reflection.append((y, th, width, a))


def inside(rect, x, y):
    return rect[0] <= x < rect[2] and rect[1] <= y < rect[3]


def pixel(S, x, y):
    """RGBA of the icon at (x, y) in [0, 1), as floats; supersampled by the caller."""
    r = S.radius
    cx, cy = min(max(x, r), 1 - r), min(max(y, r), 1 - r)
    if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
        return None
    h = S.horizon
    sx, sy, sr = S.sun
    d = math.hypot(x - sx, y - sy)
    if y < h:
        # the sky: dark up high, the horizon's glow below, a pink haze around the sun
        t = y / h
        colour = mix(mix(SKY, NIGHT, t * 0.5), HORIZON, max(0.0, (t - 0.35) / 0.65) ** 1.4)
        glow = 0.22 * math.exp(-(d - sr) / (0.07 if S.size >= 128 else 0.04)) if d >= sr else 0
        colour = mix(colour, PINK, glow)
        for (px, py, pr, a) in S.stars:
            if (x - px) ** 2 + (y - py) ** 2 < pr * pr:
                colour = mix(colour, WHITE, a)
        if d < sr and not any(g0 <= y < g1 for g0, g1 in S.gaps):
            colour = mix(SUN_TOP, PINK, (y - (sy - sr)) / (h - (sy - sr)))
        for rect in S.far:
            if inside(rect, x, y):
                colour = FAR
        for body, parts in S.towers:
            if any(inside(p, x, y) for p in parts):
                colour = SKYLINE
        for (x0, y0, x1, y1, a) in S.windows:
            if x0 <= x < x1 and y0 <= y < y1:
                colour = mix(SKYLINE, LIGHTS, a)
        # the horizon's line of light, and its glow just above
        line = max(0.012, 1.0 / S.size)
        if y >= h - line:
            colour = mix(colour, PINK, 0.85)
        elif y > h - 0.05:
            colour = mix(colour, PINK, 0.25 * (1 - (h - y) / 0.05) ** 2)
    else:
        t = (y - h) / (1 - h)
        colour = mix(SEA, NIGHT, t)
        colour = mix(colour, PINK, 0.12 * math.exp(-abs(x - sx) / 0.12) * (1 - t))  # the sun's column on the water
        for (ly, th, width, a) in S.reflection:
            if ly <= y < ly + th and abs(x - sx) < width / 2:
                colour = mix(colour, mix(PINK_HI, PINK, 1 - a), a)
    return colour


def render(size, pixels_only=False):
    S = Scene(size)
    n = 8 if size <= 64 else 3  # supersampling per side
    rows = []
    for py in range(size):
        row = bytearray([0])  # PNG filter: none
        for px in range(size):
            acc = [0.0, 0.0, 0.0]
            cover = 0
            for sy in range(n):
                for sx in range(n):
                    c = pixel(S, (px + (sx + 0.5) / n) / size, (py + (sy + 0.5) / n) / size)
                    if c is not None:
                        for i in range(3):
                            acc[i] += c[i]
                        cover += 1
            rgb_ = [int(min(1, acc[i] / cover) * 255 + 0.5) if cover else 0 for i in range(3)]
            row += bytes(rgb_ + [int(cover * 255 / (n * n) + 0.5)])
        rows.append(bytes(row))
    raw = b"".join(rows)
    if pixels_only:
        return b"".join(r[1:] for r in rows)  # RGBA, no filter bytes

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def write_header(path):
    """_NET_WM_ICON's layout: width, height, then ARGB pixels, one per `unsigned long` (Xlib's 32-bit format), for
    each size."""
    words = []
    for s in (32, 64, 128):
        rgba = render(s, pixels_only=True)
        words += [s, s]
        for i in range(0, len(rgba), 4):
            r, g, b, a = rgba[i:i + 4]
            words.append(a << 24 | r << 16 | g << 8 | b)
    lines = ["// Generated by launcher/make_icon.py --header - do not edit.", "#pragma once",
             "static const unsigned long g_launcher_icon[] = {"]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def main():
    if "--header" in sys.argv:
        write_header(sys.argv[sys.argv.index("--header") + 1])
        return
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    images = [render(s) for s in sizes]
    if "--png" in sys.argv:
        folder = sys.argv[sys.argv.index("--png") + 1]
        os.makedirs(folder, exist_ok=True)
        for s, data in zip(sizes, images):
            with open(os.path.join(folder, "icon_%d.png" % s), "wb") as f:
                f.write(data)
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "launcher.ico")
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))
    print("wrote " + out)
    png = os.path.join(os.path.dirname(out), "launcher.png")
    with open(png, "wb") as f:
        f.write(images[-1])
    print("wrote " + png)


if __name__ == "__main__":
    main()
