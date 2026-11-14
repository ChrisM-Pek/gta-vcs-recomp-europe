#!/usr/bin/env python3
"""macOS disk images: the release's .dmg (tools/release.py) and opening one (tools/update.py). macOS only.

The release's image opens in Finder as one window: VCSLauncher.app on the left, a link to /Applications on the right,
large icons, no toolbar or sidebar - drag the one onto the other. Finder reads that layout from the image's .DS_Store,
written here (ds_store below) because the tools are standard library only (no dmgbuild / ds_store packages). The
volume carries the launcher's icon (.VolumeIcon.icns + the volume's custom-icon flag, SetFile). Made with hdiutil:
a read-write image from the folder, the icon flag set on it mounted, then converted to ULMO (LZMA; macOS 10.15 and
later, the launcher needs 13).

    python3 tools/dmg.py show IMAGE.dmg|FOLDER   the layout a .DS_Store holds (a check; Finder's own files too)
"""
import contextlib
import io
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HDIUTIL = "/usr/bin/hdiutil"

# The window (points, Finder's screen coordinates from the top left) and where the two icons sit (their centres)
WINDOW = (200, 120, 640, 400)
ICON_SIZE = 128
APP_POS = (160, 180)
APPLICATIONS_POS = (480, 180)


# ------------------------------------------------------------------------------------------------------------------
# .DS_Store: a buddy-allocated file ("Bud1") holding one B-tree ("DSDB") of records (file name, 4-byte code, type,
# value) sorted by name (case-insensitive) then code. Everything fits one leaf node here. Layout as the ds_store package
# documents it (and Finder's own files show): a 36-byte header (1, "Bud1", the root block's offset and size twice, 16
# zero bytes), block offsets counted from byte 4; block addresses are offset | log2(size).

def _record(name, code, kind, value):
    out = struct.pack(">I", len(name.encode("utf-16-be")) // 2) + name.encode("utf-16-be") + code + kind
    if kind == b"blob":
        return out + struct.pack(">I", len(value)) + value
    if kind in (b"long", b"shor"):
        return out + struct.pack(">I", value)
    if kind == b"bool":
        return out + struct.pack(">B", 1 if value else 0)
    if kind == b"type":
        return out + value
    if kind == b"ustr":
        data = value.encode("utf-16-be")
        return out + struct.pack(">I", len(data) // 2) + data
    raise ValueError("record type " + repr(kind))


class _Buddy:
    """The allocator: the whole 2^31 space free, split in halves down to the size asked (32 bytes at least)."""

    def __init__(self):
        self.free = {w: [] for w in range(32)}
        self.free[31] = [0]

    def alloc(self, size):
        w = max(5, (size - 1).bit_length())
        k = next(k for k in range(w, 32) if self.free[k])
        offset = self.free[k].pop(0)
        while k > w:
            k -= 1
            self.free[k].append(offset + (1 << k))
            self.free[k].sort()
        return offset, w


def ds_store(records):
    """The bytes of a .DS_Store holding `records`: (name, code, type, value) - codes and types as 4-byte bytes."""
    records = sorted(records, key=lambda r: (r[0].lower(), r[1]))
    node = struct.pack(">II", 0, len(records)) + b"".join(_record(*r) for r in records)  # P = 0: a leaf
    if len(node) > 4096:
        raise ValueError(".DS_Store: more records than one node holds")
    buddy = _Buddy()
    buddy.alloc(32)  # the file header (offset 0)
    blocks = {}  # block number -> (offset, width)
    blocks[0] = buddy.alloc(2048)  # this allocator's own root block (its contents below)
    blocks[1] = buddy.alloc(20)  # DSDB: the tree's header
    blocks[2] = buddy.alloc(4096)  # the one leaf node
    dsdb = struct.pack(">IIIII", 2, 0, len(records), 1, 0x1000)  # root node, levels, records, nodes, page size

    addresses = [blocks[n][0] | blocks[n][1] for n in range(len(blocks))]
    root = struct.pack(">II", len(addresses), 0)
    root += b"".join(struct.pack(">I", a) for a in addresses + [0] * (-len(addresses) % 256))
    root += struct.pack(">I", 1) + struct.pack(">B", 4) + b"DSDB" + struct.pack(">I", 1)
    for w in range(32):
        root += struct.pack(">I", len(buddy.free[w])) + b"".join(struct.pack(">I", o) for o in buddy.free[w])
    if len(root) > 2048:
        raise ValueError(".DS_Store: the allocator's root block overflows")

    end = max(o + (1 << w) for o, w in blocks.values())
    data = bytearray(end + 4)
    root_offset, root_width = blocks[0]
    struct.pack_into(">I4sIII16s", data, 0, 1, b"Bud1", root_offset, 1 << root_width, root_offset, b"\0" * 16)
    for n, payload in ((0, root), (1, dsdb), (2, node)):
        data[blocks[n][0] + 4:blocks[n][0] + 4 + len(payload)] = payload
    return bytes(data)


def read_ds_store(data):
    """The records of a .DS_Store (one level or more): [(name, code, type, value)] - for `show` and the release's check."""
    def block(n):
        a = addresses[n]
        return data[(a & ~0x1f) + 4:(a & ~0x1f) + 4 + (1 << (a & 0x1f))]

    magic1, magic, offset, size, offset2 = struct.unpack_from(">I4sIII", data, 0)
    if magic1 != 1 or magic != b"Bud1" or offset != offset2:
        raise ValueError("not a .DS_Store")
    rb = data[offset + 4:offset + 4 + size]
    count = struct.unpack_from(">I", rb, 0)[0]
    addresses = list(struct.unpack_from(">%dI" % count, rb, 8))
    pos = 8 + 4 * (count + (-count % 256))
    toc = {}
    entries = struct.unpack_from(">I", rb, pos)[0]
    pos += 4
    for _ in range(entries):  # name length (1 byte), name, block number
        n = rb[pos]
        toc[rb[pos + 1:pos + 1 + n].decode()] = struct.unpack_from(">I", rb, pos + 1 + n)[0]
        pos += 1 + n + 4
    root_node = struct.unpack_from(">I", block(toc["DSDB"]), 0)[0]
    out = []

    def walk(n):
        b = block(n)
        p, c = struct.unpack_from(">II", b, 0)
        i = 8
        for _ in range(c):
            if p:
                walk(struct.unpack_from(">I", b, i)[0])
                i += 4
            ln = struct.unpack_from(">I", b, i)[0]
            name = b[i + 4:i + 4 + 2 * ln].decode("utf-16-be")
            i += 4 + 2 * ln
            code, kind = b[i:i + 4], b[i + 4:i + 8]
            i += 8
            if kind == b"blob":
                ln = struct.unpack_from(">I", b, i)[0]
                value = b[i + 4:i + 4 + ln]
                i += 4 + ln
            elif kind in (b"long", b"shor"):
                value = struct.unpack_from(">I", b, i)[0]
                i += 4
            elif kind == b"bool":
                value = b[i] != 0
                i += 1
            elif kind == b"type":
                value = b[i:i + 4]
                i += 4
            elif kind == b"ustr":
                ln = struct.unpack_from(">I", b, i)[0]
                value = b[i + 4:i + 4 + 2 * ln].decode("utf-16-be")
                i += 4 + 2 * ln
            elif kind in (b"comp", b"dutc"):
                value = struct.unpack_from(">Q", b, i)[0]
                i += 8
            else:
                raise ValueError("record type " + repr(kind))
            out.append((name, code, kind, value))
        if p:
            walk(p)

    walk(root_node)
    return out


def layout_records(app_name):
    """The release window: icon view, no bars, the app and Applications side by side."""
    x, y, w, h = WINDOW
    bwsp = {"WindowBounds": "{{%d, %d}, {%d, %d}}" % (x, y, w, h), "ShowSidebar": False, "ContainerShowSidebar": False,
            "SidebarWidth": 180, "ShowToolbar": False, "ShowPathbar": False, "ShowStatusBar": False,
            "ShowTabView": False, "PreviewPaneVisibility": False}
    icvp = {"viewOptionsVersion": 1, "backgroundType": 0, "backgroundColorRed": 1.0, "backgroundColorGreen": 1.0,
            "backgroundColorBlue": 1.0, "gridOffsetX": 0.0, "gridOffsetY": 0.0, "gridSpacing": 100.0,
            "arrangeBy": "none", "showIconPreview": True, "showItemInfo": False, "labelOnBottom": True,
            "textSize": 13.0, "iconSize": float(ICON_SIZE), "scrollPositionX": 0.0, "scrollPositionY": 0.0}

    def iloc(pos):
        return struct.pack(">II", *pos) + b"\xff" * 6 + b"\0\0"

    return [(".", b"bwsp", b"blob", plistlib.dumps(bwsp, fmt=plistlib.FMT_BINARY)),
            (".", b"icvp", b"blob", plistlib.dumps(icvp, fmt=plistlib.FMT_BINARY)),
            (".", b"vSrn", b"long", 1),
            (".", b"vstl", b"type", b"icnv"),
            (app_name, b"Iloc", b"blob", iloc(APP_POS)),
            ("Applications", b"Iloc", b"blob", iloc(APPLICATIONS_POS))]


# ------------------------------------------------------------------------------------------------------------------
# hdiutil

def hdiutil(*args, quiet=True):
    """hdiutil, tried again a few times: it fails now and then with 'Resource busy' (Spotlight, a previous detach -
    common on CI's Macs)."""
    for attempt in range(5):
        r = subprocess.run([HDIUTIL] + list(args) + (["-quiet"] if quiet and args[0] not in ("info",) else []),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode == 0:
            return r.stdout
        if attempt == 4 or "busy" not in r.stdout.lower() and "temporarily" not in r.stdout.lower():
            raise RuntimeError("hdiutil %s failed (%d): %s" % (args[0], r.returncode, r.stdout.strip()))
        time.sleep(2 * (attempt + 1))


@contextlib.contextmanager
def attached(image, writable=False):
    """The image mounted at a folder of its own, out of Finder's sight; detached afterwards (forced if it must be)."""
    mount = tempfile.mkdtemp(prefix="vcs-dmg-")
    hdiutil("attach", image, "-nobrowse", "-noautoopen", "-noverify", "-mountpoint", mount,
            "-readwrite" if writable else "-readonly")
    try:
        yield mount
    finally:
        try:
            hdiutil("detach", mount)
        except RuntimeError:
            hdiutil("detach", mount, "-force")
        shutil.rmtree(mount, ignore_errors=True)


def create(folder, out, volname, icon=None):
    """`folder` (its contents: the app, written already) as a compressed image with the release's window and, given an
    .icns, that icon on the volume. `folder` gets the Applications link and the .DS_Store."""
    apps = [n for n in os.listdir(folder) if n.endswith(".app")]
    if len(apps) != 1:
        raise ValueError("the image's folder must hold one .app: " + ", ".join(apps or ["none"]))
    link = os.path.join(folder, "Applications")
    if not os.path.islink(link):
        os.symlink("/Applications", link)
    with open(os.path.join(folder, ".DS_Store"), "wb") as f:
        f.write(ds_store(layout_records(apps[0])))
    if icon:
        shutil.copyfile(icon, os.path.join(folder, ".VolumeIcon.icns"))
    rw = out + ".rw.dmg"
    for f in (rw, out):
        if os.path.exists(f):
            os.remove(f)
    hdiutil("create", "-srcfolder", folder, "-volname", volname, "-fs", "HFS+", "-format", "UDRW", "-ov", rw)
    try:
        setfile = shutil.which("SetFile")  # the Command Line Tools' (deprecated, still there)
        if icon and not setfile:
            print("[dmg] no SetFile: the volume keeps the plain disk icon")
        elif icon:
            with attached(rw, writable=True) as mount:
                # the volume's custom-icon flag (its Finder info), which makes Finder use .VolumeIcon.icns
                r = subprocess.run([setfile, "-a", "C", mount], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True)
                if r.returncode != 0:
                    print("[dmg] no volume icon (SetFile: %s)" % r.stdout.strip())
        hdiutil("convert", rw, "-format", "ULMO", "-o", out)
    finally:
        if os.path.exists(rw):
            os.remove(rw)


def check(image, app_name):
    """The image as a player opens it: the app (signature intact), the Applications link, the layout. Problems, or []."""
    problems = []
    hdiutil("verify", image)
    with attached(image) as mount:
        app = os.path.join(mount, app_name)
        if not os.path.isfile(os.path.join(app, "Contents", "MacOS", "VCSLauncher")):
            problems.append("no %s in the image" % app_name)
        elif subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", app]).returncode != 0:
            problems.append("the app's signature doesn't verify inside the image")
        link = os.path.join(mount, "Applications")
        if not os.path.islink(link) or os.readlink(link) != "/Applications":
            problems.append("no Applications link in the image")
        try:
            with open(os.path.join(mount, ".DS_Store"), "rb") as f:
                names = {(r[0], r[1]) for r in read_ds_store(f.read())}
            for want in ((app_name, b"Iloc"), ("Applications", b"Iloc"), (".", b"icvp"), (".", b"bwsp")):
                if want not in names:
                    problems.append(".DS_Store has no %s %s" % (want[0], want[1].decode()))
        except (OSError, ValueError, KeyError, struct.error) as e:
            problems.append(".DS_Store unreadable: %s" % e)
    return problems


def find_app(folder, name="VCSLauncher.app"):
    found = [os.path.join(d, n) for d, ns, _ in os.walk(folder) for n in ns if n == name]
    return found[0] if found else ""


def main():
    if len(sys.argv) != 3 or sys.argv[1] != "show":
        print(__doc__)
        sys.exit(2)
    target = sys.argv[2]

    def show(path):
        with open(path, "rb") as f:
            for name, code, kind, value in read_ds_store(f.read()):
                if kind == b"blob" and value[:6] == b"bplist":
                    value = plistlib.load(io.BytesIO(value))
                elif code == b"Iloc":
                    value = struct.unpack(">II", value[:8])
                print("%-20s %s %s %r" % (name, code.decode(), kind.decode(), value))

    if os.path.isdir(target):
        show(os.path.join(target, ".DS_Store"))
    else:
        with attached(target) as mount:
            show(os.path.join(mount, ".DS_Store"))


if __name__ == "__main__":
    main()
