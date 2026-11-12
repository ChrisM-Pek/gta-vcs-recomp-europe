#!/usr/bin/env python3
"""HD texture packs for the 2D layer (the game's HUD, texts, radar and menus; docs/NATIVE_RENDERER.md "HD textures for
the 2D layer"). Optional: the game's own PS2 art is what it uses without one.

    python tools/mods.py status              what is installed, and whether the game uses it
    python tools/mods.py install ARCHIVE     a pack as it is published (.zip or .7z)
    python tools/mods.py remove              delete the installed textures

A pack is a set of PNG/DDS images named as PCSX2 names a texture, under `textures/<serial>/replacements/` in the
archive - the layout PCSX2's packs come in. install unpacks it into the build folder, next to vcs.exe (where the game
looks for `textures/`), and turns the game's Graphics > Image quality > **HD textures (HUD)** setting on, so the game
uses it at its next start. Nothing outside textures/ is written and nothing of the game is touched. remove deletes
that textures/ folder; the setting then simply finds no pack.

The launcher's Mods section runs this (docs/LAUNCHER.md); it is the same script by hand. On Windows a .7z is unpacked
by the system's tar (bsdtar); on Linux 7z, 7za, 7zr or bsdtar, whichever is installed - a .zip needs nothing.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

TAG = "[mods]"
REPLACEMENT = re.compile(r"^[0-9a-fA-F]+(?:-[0-9a-fA-F]+)?-[0-9a-fA-F]{8}$")
IMAGES = (".png", ".dds")


def target(build):
    return os.path.join(build, "textures")


def pack_folders(build):
    """The packs under textures/: each top-level folder (a serial) or the 'installed' one the tool makes."""
    root = target(build)
    if not os.path.isdir(root):
        return []
    return sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)))


def image_count(build):
    count = 0
    for _, _, files in os.walk(target(build)):
        count += sum(1 for f in files if f.lower().endswith(IMAGES))
    return count


def enabled(build):
    """The game's setting: display.hud_textures=1 in build/vcs.ini (0 or absent = the PS2's own art)."""
    ini = os.path.join(build, "vcs.ini")
    try:
        with open(ini, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.strip().startswith("display.hud_textures="):
                    return line.split("=", 1)[1].strip() == "1"
    except OSError:
        pass
    return False


def set_enabled(build, on):
    """One line of vcs.ini, as the game writes its settings: the rest of the file is kept as it is."""
    ini = os.path.join(build, "vcs.ini")
    lines = []
    found = False
    try:
        with open(ini, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        pass
    out = []
    for line in lines:
        if line.strip().startswith("display.hud_textures="):
            out.append("display.hud_textures=%d\n" % (1 if on else 0))
            found = True
        else:
            out.append(line)
    if not found:
        if out and not out[-1].endswith("\n"):
            out[-1] += "\n"
        out.append("display.hud_textures=%d\n" % (1 if on else 0))
    os.makedirs(build, exist_ok=True)
    with open(ini, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)


def safe_name(name):
    """An archive member's relative path, or None for one that would land outside the folder it is unpacked into."""
    name = name.replace("\\", "/")
    if not name or name.startswith("/") or re.match(r"^[a-zA-Z]:", name):
        return None
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if not parts or any(p == ".." for p in parts):
        return None
    return os.path.join(*parts)


def unzip(archive, dest):
    tc.stage("unpack", "Unpacking the pack")
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                relative = safe_name(info.filename)
                if relative is None:
                    tc.fail("the archive has a file outside its folder: " + info.filename, "not_a_pack")
                path = os.path.join(dest, relative)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with zf.open(info) as src, open(path, "wb") as out:
                    shutil.copyfileobj(src, out)
        return
    # not a zip: 7-Zip, or the system's bsdtar (Windows' tar handles .7z)
    last = ""
    for attempt in (["7z", "x", "-y", "-o" + dest, archive], ["7za", "x", "-y", "-o" + dest, archive],
                    ["7zr", "x", "-y", "-o" + dest, archive], ["bsdtar", "-xf", archive, "-C", dest],
                    ["tar", "-xf", archive, "-C", dest]):
        if shutil.which(attempt[0]) is None:
            continue
        result = subprocess.run(attempt, capture_output=True, text=True)
        if result.returncode == 0:
            return
        last = (result.stderr or result.stdout or "").strip()
    tc.fail("the archive can't be unpacked: %s%s" % (os.path.basename(archive),
                                                    " (" + last.splitlines()[-1] + ")" if last else ""), "archive_unsupported")


def look_for_pack(extract):
    """Where the pack is in the unpacked archive: its own textures/ tree (PCSX2's layout), or loose replacement images."""
    tree = os.path.join(extract, "textures")
    if os.path.isdir(tree):
        return "textures", tree
    loose = []
    for base, _, files in os.walk(extract):
        for f in files:
            if f.lower().endswith(IMAGES) and REPLACEMENT.match(os.path.splitext(f)[0]):
                loose.append(os.path.join(base, f))
    return ("loose", loose) if loose else (None, None)


def copy_into(source, out):
    """One file or a tree into `out`, counting what was written (a file already there is replaced)."""
    written = 0
    if os.path.isfile(source):
        os.makedirs(out, exist_ok=True)
        shutil.copyfile(source, os.path.join(out, os.path.basename(source)))
        return 1
    for base, _, files in os.walk(source):
        relative = os.path.relpath(base, source)
        folder = out if relative == "." else os.path.join(out, relative)
        os.makedirs(folder, exist_ok=True)
        for f in files:
            shutil.copyfile(os.path.join(base, f), os.path.join(folder, f))
            written += 1
    return written


def install(build, archive):
    archive = os.path.abspath(archive)
    if not os.path.isfile(archive):
        tc.fail("archive not found: " + archive, "not_a_pack")
    build = os.path.abspath(build)
    if not os.path.isfile(os.path.join(build, "vcs.exe")):
        tc.fail("no game in " + tc.show(build) + ": build the game first, then install the pack", "no_game")
    tc.stage("check", "Reading the pack")
    print("%s installing %s into %s" % (TAG, os.path.basename(archive), tc.show(target(build))))
    work = tempfile.mkdtemp(prefix="pack-", dir=build)
    try:
        extract = os.path.join(work, "unpacked")
        os.makedirs(extract)
        unzip(archive, extract)
        kind, found = look_for_pack(extract)
        if kind is None:
            tc.fail("that archive isn't a texture pack: no textures folder and no replacement images", "not_a_pack")
        tc.stage("install", "Installing the textures")
        if kind == "textures":
            written = copy_into(found, target(build))
        else:
            # loose images: PCSX2's replacement names in one folder of their own (the game scans textures/ at any depth)
            folder = os.path.join(target(build), "installed", "replacements")
            written = 0
            for image in found:
                written += copy_into(image, folder)
        set_enabled(build, True)
        total = image_count(build)
        print("%s %d files installed; %d in %s; HD textures (HUD) on for the next start" % (
            TAG, written, total, tc.show(target(build))))
        tc.event(result="ok", installed=written, total=total, folder=tc.show(target(build)).replace(os.sep, "/"),
                 packs=pack_folders(build), enabled=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def remove(build):
    build = os.path.abspath(build)
    root = target(build)
    count = image_count(build)
    if os.path.isdir(root):
        shutil.rmtree(root)
    print("%s removed %d textures (%s)" % (TAG, count, tc.show(root)))
    tc.event(result="ok", removed=count, folder=tc.show(root).replace(os.sep, "/"))


def status(build):
    build = os.path.abspath(build)
    count = image_count(build)
    packs = pack_folders(build)
    on = enabled(build)
    if count:
        print("%s %d textures installed (%s)%s" % (TAG, count, ", ".join(packs) or tc.show(target(build)),
                                                   "" if on else ": HD textures (HUD) is off"))
    else:
        print("%s no texture pack installed" % TAG)
    tc.event(result="ok", installed=count, packs=packs, enabled=on,
             folder=tc.show(target(build)).replace(os.sep, "/"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", help="the build folder (default: the one tools/build.py uses)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p = sub.add_parser("install")
    p.add_argument("archive")
    sub.add_parser("remove")
    a = ap.parse_args()
    build = a.build or tc.BUILD
    if a.cmd == "install":
        install(build, a.archive)
    elif a.cmd == "remove":
        remove(build)
    else:
        status(build)


if __name__ == "__main__":
    main()
