#!/usr/bin/env python3
"""HD 2D texture check (docs/NATIVE_RENDERER.md "HD textures for the 2D layer", src/game/vcs_hudtex.cpp): the HUD,
texts, radar and menus drawn from a sharper texture than the disc's, found by PCSX2's name for a texture.

    python tools/hudtex_check.py [OUT] [--pack DIR] [--port N]

Needs no pack of its own: it makes one. From the default card (build/rig/saves/fortbaxter, else .../user):
  dump  a run with VCS_HUDTEX_DUMP=OUT/dump: the PS2 textures the 2D layer draws, written under the names a
        replacement pack uses ("<TEX0Hash>-<CLUTHash>-<bits>.png"), with a screenshot of the HUD and the pause menu
  pack  a synthetic pack from the dumps: every image 2x by nearest sampling, its colours replaced by magenta, alpha
        kept (OUT/pack/textures/TEST/replacements/) - unmistakable in a picture, so "was it drawn from the pack?" is a
        pixel count
  hd    a run with VCS_HUDTEX=1 VCS_HUDTEX_DIR=OUT/pack VCS_HUDTEX_LOG=1, the same two screenshots and the log's
        [hudtex] lines
Checks: the dump holds the HUD's textures (the font atlas 256x256 among them); the log names every 2D texture and each
dump has a replacement at twice its PS2 size; the magenta of the synthetic pack shows on the HUD and fills the pause
menu's text; and the game's own HUD pictures do not have it. OUT/results.json holds the numbers, the pictures are kept
next to it. --pack DIR checks a real pack instead (then the pictures are only compared: nothing is tinted).
Exit code 0 = pass.
"""
import argparse
import json
import os
import re
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = re.compile(r"^([0-9a-f]+)(?:-([0-9a-f]+))?-([0-9a-f]{8})$")
GAME_LINE = re.compile(r"\[hudtex\]\s+(\d+)x(\d+) psm (\d+): (\S+)")     # the game side (src/game/vcs_hudtex.cpp)
RENDERER_LINE = re.compile(r"\[hudtex\]\s+(\S+) psm (\d+) (\d+)x(\d+) (.*)")  # the renderer (gs_hw.cpp)


def read_png_rgba(path):
    data = open(path, "rb").read()
    pos, idat, width, height, colour = 8, b"", 0, 0, 2
    while pos < len(data):
        length, = struct.unpack(">I", data[pos:pos + 4])
        kind, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, _, colour = struct.unpack(">IIBB", chunk[:10])
        elif kind == b"IDAT":
            idat += chunk
    raw = zlib.decompress(idat)
    bpp = 4 if colour == 6 else 3
    stride = width * bpp
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b, c = prev[x], (prev[x - bpp] if x >= bpp else 0)
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line) if bpp == 4 else b"".join(line[k:k + 3] + b"\xff" for k in range(0, stride, 3)))
        prev = line
    return width, height, rows


def write_png_rgba(path, width, height, rows):
    raw = bytearray()
    for row in rows:
        raw += b"\x00" + row

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


def magenta(width, height, rows):
    """The same picture 2x by nearest sampling, its colours magenta, its alpha kept: a replacement nothing else is."""
    out = []
    for row in rows:
        wide = b"".join((b"\xff\x00\xff" + row[k + 3:k + 4]) * 2 for k in range(0, len(row), 4))
        out.append(wide)
        out.append(wide)
    return width * 2, height * 2, out


def wait_playable(g, timeout=180.0):
    """The player exists and game frames advance (the save is loading or loaded)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not g.alive():
            raise RuntimeError("the game exited while loading")
        try:
            f0 = int(g.cmd("frames"))
            g.player()
            time.sleep(1.0)
            if int(g.cmd("frames")) > f0 + 10:
                return
        except Exception:
            time.sleep(1.0)
    raise RuntimeError("not in game after %.0f s" % timeout)


def content_pixels(path):
    """Not black in the top right (the HUD's bars / clock / money live there): the loading screen is black but its
    text, so this says the world is up (and with it the HUD). The synthetic pack tints those, so this counts any
    colour, not the money's green."""
    width, height, rows = read_png_rgba(path)
    count = 0
    for y in range(int(height * 0.15), int(height * 0.35)):
        row = rows[y]
        for x in range(int(width * 0.75), int(width * 0.99)):
            r, g, b = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            if r > 24 or g > 24 or b > 24:
                count += 1
    return count


def wait_hud(g, shots, timeout=90.0):
    """Until the world (and its HUD) is up after the save's load, a picture at shots/hud.png."""
    t0 = time.time()
    path = os.path.join(shots, "hud.png")
    while time.time() - t0 < timeout:
        g.cmd("screenshot " + path)
        if content_pixels(path) > 300:
            return path
        time.sleep(2.0)
    return path


def session(out, tag, env, port):
    """A run from the default card: the HUD and the pause menu's pictures and log."""
    shots = os.path.join(out, tag)
    os.makedirs(shots, exist_ok=True)
    g = rig.Game.start(save=rig.default_save(), name="hudtex_" + tag, env=env, port=port)
    try:
        wait_playable(g)
        wait_hud(g, shots)
        g.cmd("press Escape")
        time.sleep(3)
        g.cmd("screenshot " + os.path.join(shots, "menu.png"))
        time.sleep(1)
        g.cmd("press Escape")
        time.sleep(1)
    finally:
        g.quit()
    lines = [line for line in open(g.log_path, errors="replace") if "[hudtex]" in line]
    return shots, lines


def magenta_pixels(path):
    """How much of a picture is the synthetic pack's magenta: red and blue alike, no green. The HUD draws it at the
    texture's alpha and over a dark picture, so it is dark (32,0,32) as often as bright - the ratio is what says
    "magenta", not the level."""
    width, height, rows = read_png_rgba(path)
    count = 0
    for row in rows:
        for k in range(0, len(row), 4):
            r, g, b = row[k], row[k + 1], row[k + 2]
            if g < 16 and r > 24 and b > 24 and abs(r - b) <= 8 + r // 8:
                count += 1
    return count


def difference(a, b):
    """Mean absolute difference of two pictures of the same size (0..255), and the fraction of pixels that differ"""
    wa, ha, ra = read_png_rgba(a)
    wb, hb, rb = read_png_rgba(b)
    if (wa, ha) != (wb, hb):
        return None, None
    total = changed = 0
    for y in range(ha):
        rowa, rowb = ra[y], rb[y]
        for k in range(0, len(rowa), 4):
            d = abs(rowa[k] - rowb[k]) + abs(rowa[k + 1] - rowb[k + 1]) + abs(rowa[k + 2] - rowb[k + 2])
            total += d
            changed += 1 if d > 24 else 0
    pixels = wa * ha
    return total / (pixels * 3.0), changed / float(pixels)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build", "hudtex_check"))
    ap.add_argument("--pack", help="a real PCSX2 texture pack (folder with the replacements) instead of a synthetic one")
    ap.add_argument("--port", type=int, default=47650)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    base = {"VCS_ASPECT": "169", "VCS_HUD": "2", "PS2X_HIDDEN_WINDOW": "1920x1080", "PS2X_CAPTURE_SIZE": "1920x1080"}
    checks, dump, log = {}, [], []

    try:
        # 1. the PS2 textures the 2D layer draws, and the pictures to compare with
        shots, _ = session(out, "ps2", dict(base, VCS_HUDTEX="0", VCS_HUDTEX_DUMP=os.path.join(out, "dump")), a.port)
        dump = sorted(f for f in os.listdir(os.path.join(out, "dump")) if NAME.match(f[:-4])) if os.path.isdir(os.path.join(out, "dump")) else []
        sizes = {}
        for name in dump:
            sizes[name] = read_png_rgba(os.path.join(out, "dump", name))[:2]
        checks["dump"] = {"ok": len(dump) >= 8 and any(sizes[n] == (256, 256) for n in dump),
                          "files": len(dump), "font_atlas": [n for n in dump if sizes[n] == (256, 256)][:2]}

        # 2. the pack: a real one, else the dumps 2x as magenta
        pack = os.path.abspath(a.pack) if a.pack else os.path.join(out, "pack", "textures", "TEST", "replacements")
        synthetic = not a.pack
        if synthetic:
            os.makedirs(pack, exist_ok=True)
            for name in dump:
                width, height, rows = read_png_rgba(os.path.join(out, "dump", name))
                w, h, tinted = magenta(width, height, rows)
                write_png_rgba(os.path.join(pack, name), w, h, tinted)
        pack_files = 0
        if os.path.isdir(pack):
            pack_files = len([f for f in os.listdir(pack) if f.endswith(".png")]) if synthetic else \
                len([f for _, _, files in os.walk(pack) for f in files if f.endswith(".png")])
        checks["pack"] = {"ok": bool(dump), "synthetic": synthetic, "folder": pack, "files": pack_files}

        # 3. the same session with the pack
        shots_hd, log = session(out, "pack", dict(base, VCS_HUDTEX="1", VCS_HUDTEX_LOG="1", VCS_HUDTEX_DIR=pack), a.port)

        # 4. what the log says: every 2D texture named, the replacements at twice the PS2 size
        asked, replaced, scales = {}, [], []
        for line in log:
            m = GAME_LINE.search(line)   # the game side: "<ps2 size> psm <psm>: <name> <file> -> <size>"
            if m:
                w, h, psm, name = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
                asked[name] = (w, h)
                size = re.search(r"->\s+(\d+)x(\d+)", line)
                if size:
                    replaced.append(name)
                    scales.append((int(size.group(1)) / float(w), int(size.group(2)) / float(h)))
            m = RENDERER_LINE.search(line)  # the renderer: "<name> psm <psm> <ps2 size> <outcome>"
            if m and not m.group(5).startswith("no "):
                replaced.append(m.group(1))
        replaced = sorted(set(replaced))
        font = [n for n, (w, h) in asked.items() if (w, h) == (256, 256)]
        checks["log"] = {"ok": len(asked) >= 8 and bool(replaced), "asked": len(asked), "replaced": len(replaced),
                         "font_atlas_asked": font[:2],
                         "same_scale": all(abs(sx - sy) < 0.02 for sx, sy in scales) if scales else False}

        # 5. the picture: the synthetic pack's magenta on the HUD and over the menu's text, none in the PS2 pictures
        hd_hud = magenta_pixels(os.path.join(shots_hd, "hud.png"))
        hd_menu = magenta_pixels(os.path.join(shots_hd, "menu.png"))
        ps2_hud = magenta_pixels(os.path.join(shots, "hud.png"))
        diff_hud, changed_hud = difference(os.path.join(shots, "hud.png"), os.path.join(shots_hd, "hud.png"))
        diff_menu, changed_menu = difference(os.path.join(shots, "menu.png"), os.path.join(shots_hd, "menu.png"))
        checks["picture"] = {
            "ok": (hd_hud >= 1000 and hd_menu >= 20000 and ps2_hud < 200) if synthetic else
                  ((changed_hud or 0) > 0.001 and (changed_menu or 0) > 0.005),
            "synthetic": synthetic,
            "magenta_hud": hd_hud, "magenta_menu": hd_menu, "magenta_ps2_hud": ps2_hud,
            "hud_changed": changed_hud, "menu_changed": changed_menu,
            "hud_difference": diff_hud, "menu_difference": diff_menu}
    except Exception as error:  # a run failed: the check reports it rather than a traceback
        checks["run"] = {"ok": False, "error": str(error)}

    result = {"ok": all(c.get("ok") for c in checks.values()), "checks": checks,
              "pictures": ["ps2/hud.png", "ps2/menu.png", "pack/hud.png", "pack/menu.png"]}
    with open(os.path.join(out, "results.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    for name, check in checks.items():
        print("%-8s %s  %s" % (name, "ok  " if check.get("ok") else "FAIL", {k: v for k, v in check.items() if k != "ok"}))
    print("%s: %s (pictures and results in %s)" % ("pass" if result["ok"] else "FAIL", "HD 2D textures", out))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
