#!/usr/bin/env python3
"""Mods for the game - HD texture packs today, other kinds prepared for (docs/LAUNCHER.md "Mods").

    python tools/mods.py list [--build DIR]              the mods, their state, and the game's HD textures setting
    python tools/mods.py install ARCHIVE [--name N] [--url U]   a pack as it is published (.zip or .7z)
    python tools/mods.py enable ID | disable ID
    python tools/mods.py remove ID
    python tools/mods.py hd on|off                       the game's Graphics/Mods "HD textures" setting

A mod is a folder the game reads, with a manifest entry in `<build>/mods.json`:

    id         the folder's name (`textures/<id>` when on, `textures-off/<id>` when off). A texture pack takes the
               name PCSX2 gives its game folder (SLUS-21590); a pack without one gets its archive's name.
    kind       "textures" today: a PCSX2 texture-replacement tree (PNG/DDS images named as PCSX2 names a texture).
               The layout is `textures/<id>/<serial>/replacements/...` or `textures/<id>/...` - the game scans
               textures/ at any depth. Another kind is one case in `detect()` and in `enable()` when it comes.
    enabled    the folder is under `textures/` (the game reads it) and, for a texture mod, the game's HD textures
               setting is on.

Installing unpacks the archive into a temporary folder, works out what it is, moves it under `textures/<id>`, turns
the game's setting on, and records the name, kind, source and date in mods.json. Disabling moves the folder to
`textures-off/<id>` - nothing is copied or deleted, so both ways are instant. Nothing outside `build/` is touched.

The launcher's Mods sheet runs this (docs/LAUNCHER.md); it is the same script by hand. A .7z is unpacked by 7z, 7za,
7zr, bsdtar or Windows' tar, whichever is there; a .zip needs nothing.
"""
import argparse
import datetime
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
SERIAL = re.compile(r"^[A-Z]{4}-\d{5}$")
IMAGES = (".png", ".dds")
KINDS = ("textures",)


# ---- storage -----------------------------------------------------------------------------------------------------
def folders(build):
    """(textures, textures-off): where the game reads mods, and where the disabled ones wait."""
    return os.path.join(build, "textures"), os.path.join(build, "textures-off")


def registry_path(build):
    return os.path.join(build, "mods.json")


def load_registry(build):
    try:
        with open(registry_path(build), encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) and isinstance(data.get("mods"), list) else {"version": 1, "mods": []}
    except (OSError, ValueError):
        return {"version": 1, "mods": []}


def save_registry(build, registry):
    with open(registry_path(build), "w", encoding="utf-8", newline="\n") as f:
        json.dump(registry, f, indent=1, sort_keys=True)
        f.write("\n")


def entries(build):
    """{id: entry} from the registry, then the folders found that it does not know (a pack put there by hand)."""
    registry = load_registry(build)
    known = {m.get("id"): m for m in registry["mods"] if isinstance(m, dict) and m.get("id")}
    on, off = folders(build)
    found = []
    for root, enabled in ((on, True), (off, False)):
        if os.path.isdir(root):
            for name in sorted(os.listdir(root)):
                if os.path.isdir(os.path.join(root, name)):
                    found.append((name, enabled))
    mods = []
    for name, enabled in found:
        entry = dict(known.pop(name, {}))
        entry.update(id=name, enabled=enabled)
        entry.setdefault("kind", "textures")
        entry.setdefault("name", "HD textures (%s)" % name if entry["kind"] == "textures" else name)
        mods.append(entry)
    return mods, registry


def texture_count(folder):
    count = 0
    for _, _, files in os.walk(folder):
        count += sum(1 for f in files if f.lower().endswith(IMAGES))
    return count


def hd_setting(build):
    """The game's setting: display.hud_textures=1 in build/vcs.ini (0 or absent = the PS2's own art)."""
    try:
        with open(os.path.join(build, "vcs.ini"), encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.strip().startswith("display.hud_textures="):
                    return line.split("=", 1)[1].strip() == "1"
    except OSError:
        pass
    return False


def set_hd_setting(build, on):
    """One line of vcs.ini, as the game writes its settings: every other line is kept as it is."""
    ini = os.path.join(build, "vcs.ini")
    lines, found = [], False
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


# ---- unpacking ---------------------------------------------------------------------------------------------------
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
                    tc.fail("the archive has a file outside its folder: " + info.filename, "not_a_mod")
                path = os.path.join(dest, relative)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with zf.open(info) as src, open(path, "wb") as out:
                    shutil.copyfileobj(src, out)
        return
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


def detect(extract):
    """What kind of mod the unpacked archive is, and the folder to install: ("textures", folder), or (None, ...).

    "textures": a PCSX2 pack - a textures/ tree, or loose images named as PCSX2 names a texture. Another kind is one
    more case here (with its folder) and in enable() / disable() below."""
    tree = os.path.join(extract, "textures")
    if os.path.isdir(tree):
        return "textures", tree
    loose = []
    for base, _, files in os.walk(extract):
        for f in files:
            if f.lower().endswith(IMAGES) and REPLACEMENT.match(os.path.splitext(f)[0]):
                loose.append(os.path.join(base, f))
    if loose:
        # loose images: PCSX2's replacement names in a replacements/ folder of their own
        folder = os.path.join(extract, "__loose", "replacements")
        os.makedirs(folder)
        for image in loose:
            shutil.copyfile(image, os.path.join(folder, os.path.basename(image)))
        return "textures", os.path.join(extract, "__loose")
    return None, None


def plan(source, archive):
    """The mod's id and the folder to install: (id, root).

    A PCSX2 pack keeps its game folder: `textures/SLUS-21590/replacements/*.png` installs as `textures/SLUS-21590/`
    (the same shape PCSX2 itself uses, so `textures/` can be copied to PCSX2 as it is). Anything else - loose
    replacement images, several game folders in one archive - gets the archive's name as its folder, with the tree
    under it (the game scans textures/ at any depth)."""
    children = [c for c in sorted(os.listdir(source)) if os.path.isdir(os.path.join(source, c))]
    if len(children) == 1 and SERIAL.match(children[0]):
        return children[0], os.path.join(source, children[0])
    return sanitize(archive), source


def sanitize(name):
    stem = os.path.splitext(os.path.basename(name))[0]
    return re.sub(r"[^A-Za-z0-9._-]+", "-", stem).strip("-") or "pack"


def copy_into(source, out):
    """The tree's files into `out`, counting what was written."""
    written = 0
    for base, _, files in os.walk(source):
        relative = os.path.relpath(base, source)
        folder = out if relative == "." else os.path.join(out, relative)
        os.makedirs(folder, exist_ok=True)
        for f in files:
            shutil.copyfile(os.path.join(base, f), os.path.join(folder, f))
            written += 1
    return written


# ---- commands ----------------------------------------------------------------------------------------------------
def install(build, archive, name=None, url=None):
    archive = os.path.abspath(archive)
    if not os.path.isfile(archive):
        tc.fail("archive not found: " + archive, "not_a_mod")
    build = os.path.abspath(build)
    if not any(os.path.isfile(os.path.join(build, exe)) for exe in ("vcs.exe", "vcs")):
        tc.fail("no game in " + tc.show(build) + ": build the game first, then install the mod", "no_game")
    tc.stage("check", "Reading the pack")
    print("%s installing %s into %s" % (TAG, os.path.basename(archive), tc.show(folders(build)[0])))
    work = tempfile.mkdtemp(prefix="mod-", dir=build)
    try:
        extract = os.path.join(work, "unpacked")
        os.makedirs(extract)
        unzip(archive, extract)
        kind, source = detect(extract)
        if kind is None:
            tc.fail("that archive isn't a mod this launcher knows: no textures folder and no replacement images",
                    "not_a_mod")
        tc.stage("install", "Installing the mod")
        ident, root = plan(source, archive)
        on, off = folders(build)
        for place in (on, off):  # a reinstall replaces the same mod, wherever it was
            if os.path.isdir(os.path.join(place, ident)):
                shutil.rmtree(os.path.join(place, ident))
        os.makedirs(on, exist_ok=True)
        written = copy_into(root, os.path.join(on, ident))
        set_hd_setting(build, True)
        mods, registry = entries(build)
        entry = next((m for m in registry["mods"] if m.get("id") == ident), None) or {"id": ident}
        entry.update(name=name or entry.get("name") or ("HD textures (%s)" % ident), kind=kind,
                     source=os.path.basename(archive), installed=datetime.date.today().isoformat(),
                     files=written, textures=texture_count(os.path.join(on, ident)))
        if url:
            entry["url"] = url
        registry["mods"] = [m for m in registry["mods"] if m.get("id") != ident] + [entry]
        save_registry(build, registry)
        print("%s %d files installed as %s; HD textures on for the next start" % (TAG, written, ident))
        tc.event(result="ok", id=ident, installed=written, mods=entries(build)[0], hd=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def set_enabled(build, ident, on):
    build = os.path.abspath(build)
    on_dir, off_dir = folders(build)
    source = os.path.join(off_dir if on else on_dir, ident)
    if not os.path.isdir(source):
        tc.fail("no mod called " + ident, "no_mod")
    target = os.path.join(on_dir if on else off_dir, ident)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    if os.path.isdir(target):
        shutil.rmtree(target)
    shutil.move(source, target)
    if on:
        set_hd_setting(build, True)  # an enabled texture mod with the setting off would do nothing
    print("%s %s %s" % (TAG, ident, "enabled" if on else "disabled"))
    tc.event(result="ok", id=ident, enabled=on, mods=entries(build)[0], hd=hd_setting(build))


def remove(build, ident):
    build = os.path.abspath(build)
    removed = 0
    for root in folders(build):
        folder = os.path.join(root, ident)
        if os.path.isdir(folder):
            removed += texture_count(folder)
            shutil.rmtree(folder)
    mods, registry = entries(build)
    registry["mods"] = [m for m in registry["mods"] if m.get("id") != ident]
    save_registry(build, registry)
    print("%s %s removed (%d textures)" % (TAG, ident, removed))
    tc.event(result="ok", id=ident, removed=removed, mods=entries(build)[0], hd=hd_setting(build))


def set_hd(build, on):
    build = os.path.abspath(build)
    set_hd_setting(build, on)
    print("%s HD textures (HUD) %s" % (TAG, "on" if on else "off"))
    tc.event(result="ok", hd=on, mods=entries(build)[0])


def listing(build):
    build = os.path.abspath(build)
    mods, _ = entries(build)
    for mod in mods:
        folder = os.path.join(folders(build)[0 if mod["enabled"] else 1], mod["id"])
        mod["textures"] = texture_count(folder)
        mod["kind"] = mod.get("kind") or "textures"
        mod.setdefault("name", "HD textures (%s)" % mod["id"] if mod["kind"] == "textures" else mod["id"])
    hd = hd_setting(build)
    if mods:
        for mod in mods:
            print("%s %s: %s, %s, %d textures%s" % (TAG, mod["id"], mod["name"], mod["kind"], mod["textures"],
                                                    "" if mod["enabled"] else ", disabled"))
    else:
        print("%s no mod installed" % TAG)
    if mods and not hd:
        print("%s the game's HD textures (HUD) setting is off" % TAG)
    tc.event(result="ok", mods=mods, hd=hd, installed=sum(m["textures"] for m in mods),
             enabled=any(m["enabled"] for m in mods))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", help="the build folder (default: the one tools/build.py uses)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("status")  # the launcher's name for it (lists are the same)
    p = sub.add_parser("install")
    p.add_argument("archive")
    p.add_argument("--name", help="what to call the mod (default: its folder)")
    p.add_argument("--url", help="where it came from (shown by the launcher later)")
    for name in ("enable", "disable", "remove"):
        sub.add_parser(name).add_argument("id")
    p = sub.add_parser("hd")
    p.add_argument("state", choices=["on", "off"])
    a = ap.parse_args()
    build = a.build or tc.BUILD
    if a.cmd == "install":
        install(build, a.archive, a.name, a.url)
    elif a.cmd == "enable":
        set_enabled(build, a.id, True)
    elif a.cmd == "disable":
        set_enabled(build, a.id, False)
    elif a.cmd == "remove":
        remove(build, a.id)
    elif a.cmd == "hd":
        set_hd(build, a.state == "on")
    else:
        listing(build)


if __name__ == "__main__":
    main()
