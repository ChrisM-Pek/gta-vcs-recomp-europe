#!/usr/bin/env python3
"""Draws the launcher's icon (launcher.ico): a striped hot-pink sun over the sea, on black. Standard library only.

    python launcher/make_icon.py        writes launcher/launcher.ico (16, 24, 32, 48, 64, 256 px)

Drawn here rather than kept as artwork, so the repository holds the source of every byte.
"""
import math
import os
import struct
import zlib


def pixel(x, y, size):
    """RGBA of the icon at (x, y) in [0, 1) - supersampled by the caller."""
    r = 0.18  # corner radius of the rounded square
    cx, cy = min(max(x, r), 1 - r), min(max(y, r), 1 - r)
    if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
        return (0, 0, 0, 0)
    # sky: black at the top to a deep pink at the horizon; sea below it
    horizon = 0.70
    if y < horizon:
        t = y / horizon
        sky = (0.04 + 0.20 * t, 0.04 + 0.02 * t, 0.045 + 0.08 * t)
        colour = sky
        # the sun: a disc, light pink at the top to hot pink at the bottom, cut by stripes that thicken toward the horizon
        sx, sy, sr = 0.5, 0.52, 0.30
        if (x - sx) ** 2 + (y - sy) ** 2 < sr * sr:
            v = (y - (sy - sr)) / (2 * sr)
            stripe = False
            if y > sy - 0.02:
                band = (y - (sy - 0.02)) / (horizon - sy + 0.02)
                period = 0.075
                phase = ((y - (sy - 0.02)) % period) / period
                stripe = phase > 1 - (0.15 + 0.45 * band)
            if not stripe:
                colour = (1.0, 0.60 - 0.42 * v, 0.80 - 0.26 * v)
    else:
        t = (y - horizon) / (1 - horizon)
        colour = (0.07 - 0.03 * t, 0.035 - 0.01 * t, 0.055 - 0.02 * t)
        # the sun's reflection: short pink lines
        if abs(x - 0.5) < 0.24 - 0.10 * t - 0.06 * (int((y - horizon) * 40) % 4 == 2) and int((y - horizon) * 40) % 2 == 0:
            colour = (1.0, 0.24, 0.58)
    return tuple(int(max(0, min(1, c)) * 255) for c in colour) + (255,)


def render(size):
    n = 4  # 4x4 supersampling
    rows = []
    for py in range(size):
        row = bytearray([0])  # PNG filter: none
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(n):
                for sx in range(n):
                    p = pixel((px + (sx + 0.5) / n) / size, (py + (sy + 0.5) / n) / size, size)
                    for i in range(3):
                        acc[i] += p[i] * p[3]
                    acc[3] += p[3]
            a = acc[3] / (n * n)
            rgb = [int(acc[i] / acc[3]) if acc[3] else 0 for i in range(3)]
            row += bytes(rgb + [int(a)])
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    sizes = [16, 24, 32, 48, 64, 256]
    images = [render(s) for s in sizes]
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


if __name__ == "__main__":
    main()
