#!/usr/bin/env python3
"""Controls and player settings check (docs/CONTROLS.md, src/game/vcs_input.cpp): the bindings, checked in the pad
report they make and in the game, plus the settings registry's presets and reset, with menu screenshots.

    python tools/controls_check.py [OUT] [--port 47673]

Loads the Fort Baxter rig save (a copy, hidden), then: every default key binding (both slots, either Shift / Ctrl,
Keypad Enter, the mouse buttons) gives its PS2 button or stick direction, Alt+Enter is not Start; a rebound key moves
Vic and the old one no longer does; the walk key holds the stick at walking speed; the mouse turns the on-foot camera
by the amount asked (heading and pitch, inverted, off), right click locks on to a ped, the Driving keys (W / S / Space
accelerate, brake, handbrake in a car, and nothing else there), the mouse on the vehicle camera, on foot again after
leaving the car; free aim (src/game/vcs_aim.cpp: the right button enters the game's free aim, the mouse turns it, a shot
hits a man on the aim line and not one beside it, Vic strafes while aiming, the camera stays where the aim looked, fists
and free aim off still lock on, the gamepad's RB and right stick, the sniper scope turned 1:1 and zoomed by the wheel,
the wheel changing weapons; in a car, the game's drive-by free aim as in Jive Drive: the left stick with free aim off,
the mouse / right stick with it on, the height limits, a shot through the circle, leaving the car; on foot, the game's
forced free aim as in Phil's shooting range: the same, the gamepad's R1 firing, the right button keeping it, the side
and height limits, the script's end); a mouse wheel binding; a menu key
refuses a key a control has; the presets and Reset all settings; the screenshot key (F8) saves a PNG; screenshots of the settings menu's pages; Audio >
Music / Sound effects are the game's own levels (set here, shown in its pause menu, changed there, kept by a save
load) (src/game/vcs_volume.cpp). Results: OUT/summary.txt; screenshots OUT/*.png. Default
OUT: build/controls/<date-time>.
"""
import argparse
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# DualShock 2 bits (pressed), as "padstate" reports them
SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT = 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080
L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE = 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000
KEY_G, KEY_SHIFT, KEY_WHEEL_UP, KEY_F2 = 71, 340, 405, 291

# control-channel key name -> what the default bindings make of it: a button bit, or (axis, byte)
DEFAULTS = [
    ("w", ("ly", 0)), ("s", ("ly", 255)), ("a", ("lx", 0)), ("d", ("lx", 255)),
    ("i", ("ry", 0)), ("k", ("ry", 255)), ("j", ("rx", 0)), ("l", ("rx", 255)),
    ("shift", CROSS), ("rshift", CROSS), ("space", SQUARE), ("f", TRIANGLE), ("ctrl", CIRCLE), ("rctrl", CIRCLE),
    ("mouseleft", CIRCLE), ("q", L1), ("e", R1), ("mouseright", R1), ("1", L2), ("z", L2), ("3", R2), ("x", R2),
    ("c", L3), ("v", R3), ("backspace", SELECT),
    ("up", UP), ("down", DOWN), ("left", LEFT), ("right", RIGHT),
]
# Start opens the game's pause menu: checked last
START_KEYS = [("return", START), ("kpenter", START)]


class Check:
    def __init__(self, out, port):
        self.out = out
        self.port = port
        self.results = []
        self.g = None

    def ok(self, name, cond, detail=""):
        self.results.append((name, bool(cond), detail))
        print("%-4s %s %s" % ("ok" if cond else "FAIL", name, detail), flush=True)
        return cond

    def pad(self):
        """The last pad report: {'buttons': int, 'lx': .., 'ly': .., 'rx': .., 'ry': ..}, after a few game frames."""
        self.g.wait_frames(3)
        line = self.g.cmd("padstate")
        d = dict(re.findall(r"(\w+)=([0-9a-f]+)", line))
        return {k: int(v, 16) if k == "buttons" else int(v) for k, v in d.items()}

    def held(self, *keys):
        for k in keys:
            self.g.cmd("key %s down" % k)
        try:
            return self.pad()
        finally:
            for k in keys:
                self.g.cmd("key %s up" % k)

    def setting(self, key, value=None):
        r = self.g.cmd("setting %s%s" % (key, "" if value is None else " %s" % value))
        return r.split(" ", 1)[1] if r.startswith("ok ") else r

    def shot(self, name):
        self.g.cmd("screenshot " + os.path.join(self.out, name + ".png").replace("\\", "/"))

    def moved(self, key, seconds=1.5):
        """How far Vic moves while `key` is held."""
        before = self.g.player()[1]
        self.g.cmd("key %s down" % key)
        time.sleep(seconds)
        self.g.cmd("key %s up" % key)
        time.sleep(0.5)
        after = self.g.player()[1]
        return math.hypot(after[0] - before[0], after[1] - before[1])

    def keys(self, *seq):
        """Key presses, KEY[@seconds to wait after] (the game's menus take their time)"""
        for k in seq:
            key, _, wait = k.partition("@")
            self.g.cmd("press %s 150" % key)
            time.sleep(float(wait or 1.5))

    def sound(self):
        """soundstate: the game's music / effects levels (0..127)."""
        self.g.wait_frames(3)
        return dict(re.findall(r"(\w+)=([0-9/]+)", self.g.cmd("soundstate")))

    def sound_levels(self):
        """Audio > Music / Sound effects are the game's own levels (src/game/vcs_volume.cpp)."""
        g = self.g
        s = self.sound()
        self.ok("sound levels at the defaults", s.get("music") == "80" and s.get("effects") == "112", str(s))
        self.setting("audio.music_volume", 25)
        self.setting("audio.sfx_volume", 50)
        s = self.sound()
        prefs = g.peek(g.u32(0x48F3D0) + 0x38, 8).hex()  # the menu's prefs: effects, music (u32)
        self.ok("music / effects settings reach the game", s.get("music") == "32" and s.get("effects") == "64" and
                prefs == "4000000020000000", "%s prefs %s" % (s, prefs))
        # loading the card's save (it holds the defaults) keeps the player's levels: pause menu > Game > Load, slot 1
        self.keys("Return@3", "Right", "Right", "Shift@2", "Shift@3", "Down", "Shift@3", "Up", "Shift@25")
        s = self.sound()
        with open(g.log_path, encoding="utf-8", errors="replace") as f:
            loaded = "the game sets the music level 80 (ra=0x3add90): the player's 32" in f.read()
        self.ok("a save load keeps the player's levels", loaded and s.get("music") == "32" and s.get("effects") == "64",
                "%s, load %s" % (s, "seen" if loaded else "not seen"))
        # the pause menu's Audio page shows them (2 and 4 bars) and its slider changes the setting
        self.keys("Return@3", "Down@2", "Right@2")
        self.shot("game_audio_page")
        self.keys("Shift@2", "Left@2")
        s = self.sound()
        music = self.setting("audio.music_volume")
        self.shot("game_audio_music_down")
        self.ok("the pause menu's music slider changes the setting", s.get("music") == "16" and music == "13",
                "%s, setting %s" % (s, music))
        self.keys("f", "f@2")  # Triangle: back, close
        self.setting("audio.music_volume", 63)
        self.setting("audio.sfx_volume", 88)

    CAM = 0x6F44D0 + 0x70  # TheCamera's active CCam: +0 mode (4 on foot, 11 locked on, 18 vehicle), +0x78 pitch, +0x7C heading

    def cam(self):
        """(mode, pitch, heading) of the active camera."""
        pitch, heading = self.g.peekf(self.CAM + 0x78, 2)
        return self.g.u32(self.CAM), pitch, heading

    def mouse_moves(self, n, dx, dy):
        """Mouse motion in n small moves (as a real mouse sends it); (heading, pitch) turned once the camera settled."""
        _, p0, h0 = self.cam()
        for _ in range(n):
            self.g.cmd("mouse %d %d" % (dx, dy))
        self.g.wait_frames(20)
        _, p1, h1 = self.cam()
        return (h1 - h0 + math.pi) % (2 * math.pi) - math.pi, p1 - p0

    def mouse_and_driving(self):
        """The mouse camera on foot (heading written in the game, pitch through the stick), right-click lock-on, and the
        Driving keys (W / S / Space as Cross / Square / R1 while driving only), checked in the camera and the car."""
        g = self.g
        S = rig.Scm
        g.teleport(575.0, 175.0, -100.0, 0.0)  # a straight street (the camera starts at its lowest here)
        time.sleep(4.0)
        g.cmd("mouse 0 -150")
        g.wait_frames(30)
        mode = self.cam()[0]
        turn, up = self.mouse_moves(30, 10, -3)  # 400 counts a radian at 100%: -0.75 heading (right), +0.225 pitch (up)
        self.ok("mouse turns the camera on foot", mode == 4 and abs(turn + 0.75) < 0.03 and abs(up - 0.225) < 0.02,
                "mode %d heading %+.3f pitch %+.3f" % (mode, turn, up))
        # up / down at once as left / right: 120 counts up are +0.3 two frames later (through the stick's top speed and
        # the game's easing it was under 0.06) and stay there
        _, p0, _ = self.cam()
        g.cmd("mouse 0 -120")
        g.wait_frames(2)
        _, p1, _ = self.cam()
        g.wait_frames(20)
        _, p2, _ = self.cam()
        self.ok("mouse pitch at once", abs(p1 - p0 - 0.3) < 0.02 and abs(p2 - p1) < 0.02,
                "pitch %+.3f after 2 frames, %+.3f after 22" % (p1 - p0, p2 - p0))
        g.cmd("mouse 0 120")
        g.wait_frames(20)
        self.setting("mouse.invert_y", 1)
        _, up = self.mouse_moves(10, 0, -4)
        self.setting("mouse.invert_y", 0)
        self.ok("invert mouse up / down", abs(up + 0.1) < 0.01, "pitch %+.3f" % up)
        self.setting("mouse.camera", 0)
        turn, up = self.mouse_moves(10, 20, 5)
        self.setting("mouse.camera", 1)
        self.ok("mouse camera off", abs(turn) < 0.005 and abs(up) < 0.005, "heading %+.3f pitch %+.3f" % (turn, up))
        self.keys("Return@3")  # the game's pause menu: the camera stays
        _, _, h0 = self.cam()
        g.cmd("mouse 400 0")
        time.sleep(1.0)
        _, _, h1 = self.cam()
        self.keys("Return@3")
        self.ok("the mouse leaves the camera in the pause menu", abs(h1 - h0) < 0.001, "heading %.3f -> %.3f" % (h0, h1))

        # right click locks on to a ped next to Vic (the game's lock-on camera, mode 11), and the mouse does not break it
        _, (x, y, _), _ = g.player()
        g.scm(S.op(S.REQUEST_MODEL, S.i(55)) + S.op(S.LOAD_ALL_MODELS_NOW) +
              S.op(S.CREATE_CHAR, S.i(4), S.i(55), S.f(x + 2.0), S.f(y + 2.0), S.f(-100.0), S.LOCAL0))
        time.sleep(1.5)
        g.cmd("key mouseright down")
        time.sleep(1.0)
        locked = self.cam()[0]
        g.cmd("mouse 400 0")
        time.sleep(0.5)
        still = self.cam()[0]
        g.cmd("key mouseright up")
        time.sleep(1.0)
        self.ok("right click locks on", locked == 11 and still == 11 and self.cam()[0] == 4,
                "held %d, mouse moved %d, released %d" % (locked, still, self.cam()[0]))

        # driving: the Driving keys, and nothing else on those keys
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)
        g.spawn_car(ahead=0.0)
        time.sleep(3.0)
        ped = g.player()[0]
        car = g.u32(ped + 0x480)
        self.ok("in a car", car != 0 and g.u32(ped + 0x8C4) == 0x37 and self.cam()[0] == 18, "car 0x%x mode %d" % (car, self.cam()[0]))
        got = {k: self.held(k) for k in ("w", "s", "space", "a", "shift", "mouseright")}
        want = {"w": CROSS, "s": SQUARE, "space": R1, "a": 0, "shift": CROSS, "mouseright": R1}
        bad = ["%s -> %s" % (k, p) for k, p in got.items() if p["buttons"] != want[k] or p["ly"] != 128 or
               p["lx"] != (0 if k == "a" else 128)]
        self.ok("driving keys", not bad, "; ".join(bad))
        self.setting("keys.drive_lean_forward", KEY_G)
        p = self.held("g")
        self.setting("keys.drive_lean_forward", 0)
        self.ok("lean forward rebound", p["ly"] == 0 and p["buttons"] == 0, str(p))

        def forward():
            v, f = g.peekf(car + 0x140, 3), g.peekf(car + 0x10, 3)
            return sum(a * b for a, b in zip(v, f))
        g.cmd("key w down")
        time.sleep(2.0)
        fast = forward()
        g.cmd("key w up")
        g.cmd("key space down")
        time.sleep(1.6)
        stopped = forward()
        g.cmd("key space up")
        self.ok("W drives, Space is the handbrake", fast > 0.15 and abs(stopped) < 0.05, "%.3f then %.3f" % (fast, stopped))
        _, _, h0 = self.cam()
        for _ in range(20):  # the vehicle camera turns at the stick's speed: a steady movement over 0.6 s
            g.cmd("mouse 20 0")
            time.sleep(0.03)
        g.wait_frames(20)
        _, _, h1 = self.cam()
        self.ok("mouse turns the vehicle camera", abs(h1 - h0) > 0.1, "heading %.3f -> %.3f" % (h0, h1))

        # out of the car: the on-foot keys and the mouse look again
        g.cmd("key space down")
        time.sleep(1.0)
        g.cmd("key space up")
        g.cmd("press f 300")
        t0 = time.time()
        while time.time() - t0 < 8 and (g.u32(ped + 0x8C4) == 0x37 or self.cam()[0] != 4):
            time.sleep(0.25)
        time.sleep(3.0)  # the camera's move back behind Vic sets its heading itself meanwhile
        turn, _ = self.mouse_moves(30, 10, 0)
        p = self.held("w")
        self.ok("on foot again", self.cam()[0] == 4 and abs(turn + 0.75) < 0.03 and p["ly"] == 0 and p["buttons"] == 0,
                "mode %d heading %+.3f, W %s" % (self.cam()[0], turn, p))

    # ---- free aim (src/game/vcs_aim.cpp, game-internals.md 12c) ----
    PED_POOL, PED_SIZE = 0x487A94, 0xD20
    PED_TARGET, PED_HEALTH, PED_SLOT = 0x820, 0x4E4, 0x789

    def give(self, weapon, ammo=200):
        """The weapon to Vic, in his hand (GIVE_WEAPON_TO_CHAR selects it)."""
        self.g.scm(rig.Scm.op(rig.Scm.GIVE_WEAPON_TO_CHAR, rig.Scm.PLAYER, rig.Scm.i(weapon), rig.Scm.i(ammo)))
        time.sleep(1.0)

    def ped_at(self, x, y):
        """A civilian standing at (x, y) on the ground: its address."""
        S = rig.Scm
        ints, _ = self.g.scm_outputs(S.op(S.REQUEST_MODEL, S.i(55)) + S.op(S.LOAD_ALL_MODELS_NOW) +
                                     S.op(S.CREATE_CHAR, S.i(4), S.i(55), S.f(x), S.f(y), S.f(-100.0), S.LOCAL0), 1)
        return self.g.u32(self.g.u32(self.PED_POOL)) + (ints[0] >> 8) * self.PED_SIZE

    def ped_ahead(self, ahead, side=0.0):
        """A civilian `ahead` metres in front of Vic and `side` to his right."""
        ped = self.g.player()[0]
        (px, py), (fx, fy) = self.g.peekf(ped + 0x30, 2), self.g.peekf(ped + 0x10, 2)
        n = math.hypot(fx, fy) or 1.0
        fx, fy = fx / n, fy / n
        return self.ped_at(px + fx * ahead + fy * side, py + fy * ahead - fx * side)

    def aim(self):
        """(active, shots) from aimstate; the dummy's heading from Vic (radians)."""
        d = dict(re.findall(r"(\w+)=(\d+)", self.g.cmd("aimstate")))
        return int(d.get("active", 0)), int(d.get("shots", 0))

    def aim_heading(self):
        ped = self.g.player()[0]
        target = self.g.u32(ped + self.PED_TARGET)
        if not target:
            return None
        (tx, ty), (px, py) = self.g.peekf(target + 0x30, 2), self.g.peekf(ped + 0x30, 2)
        return math.atan2(ty - py, tx - px)

    def free_aim(self):
        """Free aim with the mouse and the gamepad (the game's own free aim, driven by the host), the scopes turned by
        the mouse, the wheel's weapons and zoom."""
        g = self.g
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)

        # fists keep the lock-on with free aim on, and with it off the pistol locks on as on the PS2 (a man 2.5 m ahead)
        for weapon, free in ((0, 1), (19, 0)):
            self.setting("mouse.free_aim", free)
            if weapon:
                self.give(weapon)
            else:
                for _ in range(10):  # R2 (3) until the fists are in his hand
                    if g.peek(g.player()[0] + self.PED_SLOT, 1)[0] == 0:
                        break
                    g.cmd("press 3 120")
                    time.sleep(0.6)
            self.ped_ahead(2.5)
            time.sleep(1.5)
            g.cmd("key mouseright down")
            time.sleep(1.0)
            target = g.u32(g.player()[0] + self.PED_TARGET)
            kind = g.u32(target + 0x48) & 0xE if target else 0
            self.ok("%s, free aim %s: lock-on" % ("fists" if not weapon else "pistol", "on" if free else "off"),
                    self.cam()[0] == 11 and kind == 6 and self.aim()[0] == 0, "mode %d target kind %d" % (self.cam()[0], kind))
            g.cmd("key mouseright up")
            time.sleep(1.5)

        self.setting("mouse.free_aim", 1)
        self.give(19)  # a pistol

        # right button: the game's free aim (camera 11 on the dummy object), R1 held and no L3 once in
        g.cmd("key mouseright down")
        time.sleep(1.0)
        active, shots0 = self.aim()
        target = g.u32(g.player()[0] + self.PED_TARGET)
        kind = g.u32(target + 0x48) & 0xE if target else 0
        p = self.pad()
        self.ok("free aim on the right button", active == 1 and self.cam()[0] == 11 and kind == 8 and p["buttons"] == R1,
                "active %d mode %d target kind %d buttons %04x" % (active, self.cam()[0], kind, p["buttons"]))

        # the mouse turns it: 400 counts a radian, the camera with it
        h0, c0 = self.aim_heading(), self.cam()[2]
        for _ in range(20):
            g.cmd("mouse 20 0")
        g.wait_frames(15)
        turn = (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi
        cturn = (self.cam()[2] - c0 + math.pi) % (2 * math.pi) - math.pi
        self.ok("the mouse turns the aim", abs(turn + 1.0) < 0.03 and abs(cturn + 1.0) < 0.15,
                "aim %+.3f camera %+.3f" % (turn, cturn))

        # shots go through the circle: a man on the aim line is hit, one 2 m beside it is not (the aim levelled first: it
        # starts where the camera looked, low here)
        ped = g.player()[0]
        (px, py, pz), (tx, ty, tz) = g.peekf(ped + 0x30, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
        g.cmd("mouse 0 %d" % round(math.atan2(tz - (pz + 0.6), math.hypot(tx - px, ty - py)) * 400))
        g.wait_frames(10)
        (px, py), (tx, ty) = g.peekf(ped + 0x30, 2), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 2)
        ux, uy = tx - px, ty - py
        n = math.hypot(ux, uy)
        ux, uy = ux / n, uy / n
        on = self.ped_at(px + ux * 9, py + uy * 9)
        off = self.ped_at(px + ux * 9 - uy * 2, py + uy * 9 + ux * 2)
        time.sleep(0.3)
        g.cmd("press mouseleft 120")
        time.sleep(0.8)
        hit, missed = g.peekf(on + self.PED_HEALTH, 1)[0], g.peekf(off + self.PED_HEALTH, 1)[0]
        shots = self.aim()[1] - shots0
        self.ok("the shot goes through the circle", hit < 100 and missed == 100 and shots >= 1,
                "on the line %.0f, beside it %.0f, %d shot(s) down the camera line" % (hit, missed, shots))

        # moving while aiming, as locked on with this gun (the pistol walks): the aim keeps its heading
        h0 = self.aim_heading()
        far = self.moved("d", 1.0)
        dh = (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi
        self.ok("moving while aiming", far > 1.0 and abs(dh) < 0.05 and self.aim()[0] == 1, "%.2f m, aim %+.3f" % (far, dh))

        # let go: the follow camera keeps looking where the aim did
        beta = self.cam()[2]
        g.cmd("key mouseright up")
        time.sleep(1.5)
        mode, _, after = self.cam()
        dh = (after - beta + math.pi) % (2 * math.pi) - math.pi
        self.ok("released: the camera stays", mode == 4 and abs(dh) < 0.05 and self.aim()[0] == 0,
                "mode %d heading %+.3f" % (mode, dh))

        self.setting("mouse.free_aim", 0)

        # the gamepad: Gamepad > Free aim, RB (R1) aims, the right stick turns it (2 radians a second at full tilt), the
        # left stick still moves Vic
        self.setting("pad.free_aim", 1)
        g.cmd("gamepad on")
        g.cmd("gamepad rb 1")
        time.sleep(1.0)
        on_pad = self.aim()[0]
        h0 = self.aim_heading()
        g.cmd("gamepad rx 1")
        time.sleep(0.5)
        g.cmd("gamepad rx 0")
        time.sleep(0.3)
        turn = (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi
        before = g.player()[1]
        g.cmd("gamepad lx 1")
        time.sleep(1.0)
        g.cmd("gamepad lx 0")
        after = g.player()[1]
        g.cmd("gamepad rb 0")
        time.sleep(1.0)
        g.cmd("gamepad off")
        self.setting("pad.free_aim", 0)
        far = math.hypot(after[0] - before[0], after[1] - before[1])
        self.ok("gamepad free aim", on_pad == 1 and -1.4 < turn < -0.6 and far > 1.0, "aim %+.3f, moved %.2f m" % (turn, far))

        # the sniper rifle's scope: the mouse turns it directly (no stick speed), the wheel zooms
        self.give(30, 20)
        g.cmd("key mouseright down")
        time.sleep(1.2)
        mode, _, b0 = self.cam()
        for _ in range(20):
            g.cmd("mouse 20 0")
        g.wait_frames(10)
        dh = (self.cam()[2] - b0 + math.pi) % (2 * math.pi) - math.pi
        fov0 = g.peekf(self.CAM + 0x134, 1)[0]
        g.cmd("press wheelup 50")
        time.sleep(0.2)
        g.cmd("press wheelup 50")
        time.sleep(1.5)
        fov1 = g.peekf(self.CAM + 0x134, 1)[0]
        self.shot("scope_zoomed")
        g.cmd("key mouseright up")
        time.sleep(1.0)
        self.ok("the mouse turns the scope, the wheel zooms", mode == 7 and abs(dh + 1.0) < 0.01 and fov1 < fov0 * 0.7,
                "mode %d heading %+.3f, FOV %.1f -> %.1f" % (mode, dh, fov0, fov1))

        # the wheel on foot: down the next weapon, up the previous one (one each notch)
        self.give(24, 500)  # an SMG next to the pistol and the rifle
        slots = [g.peek(g.player()[0] + self.PED_SLOT, 1)[0]]
        for k in ("wheeldown", "wheelup", "wheelup"):
            g.cmd("press %s 50" % k)
            time.sleep(0.6)
            slots.append(g.peek(g.player()[0] + self.PED_SLOT, 1)[0])
        self.ok("the wheel changes weapon", slots[1] != slots[0] and slots[2] == slots[0] and slots[3] not in (slots[0], slots[1]),
                "slots %s" % slots)

        # a gun running dry while free aiming (people around): the game puts the fists in his hand; with the button
        # still held there is no lock-on (no R1 for the game) until it is let go, and the wheel's next gun aims again
        self.setting("mouse.free_aim", 1)
        self.give(19)
        ped = g.player()[0]
        slot = g.peek(ped + self.PED_SLOT, 1)[0]
        g.cmd("poke 0x%x %s" % (ped + 0x578 + slot * 0x1C + 8, (5).to_bytes(4, "little").hex() * 2))  # 5 rounds
        self.ped_ahead(4.0, 1.5)
        self.ped_ahead(5.0, -1.5)
        time.sleep(1.0)
        g.cmd("key mouseright down")
        time.sleep(0.6)
        g.cmd("key mouseleft down")
        time.sleep(2.5)
        g.cmd("key mouseleft up")
        time.sleep(0.5)
        dry = g.peek(ped + self.PED_SLOT, 1)[0], self.aim()[0], self.pad()["buttons"], g.u32(ped + self.PED_TARGET), self.cam()[0]
        for _ in range(4):  # the wheel to the SMG, the button still held
            if g.peek(ped + self.PED_SLOT, 1)[0] == 5:
                break
            g.cmd("press wheeldown 50")
            time.sleep(0.5)
        time.sleep(0.6)
        again = g.peek(ped + self.PED_SLOT, 1)[0], self.aim()[0], self.cam()[0]
        g.cmd("key mouseright up")
        time.sleep(1.0)
        self.setting("mouse.free_aim", 0)
        self.ok("a gun running dry while aiming: no lock-on, the next gun aims",
                dry == (0, 0, 0, 0, 4) and again == (5, 1, 11),
                "dry: slot %d aim %d buttons %04x target 0x%x mode %d; wheel: slot %d aim %d mode %d" % (dry + again))

    def shot_under_circle(self, ped, name):
        """A man under the circle (on the camera's line through it, 8 m past Vic) and one 2 m beside him, the aim tilted
        onto the first one's middle, the left button pressed: (health of the one under it, of the one beside it, shots
        sent down the camera line)."""
        g = self.g
        cam = self.CAM
        (sx, sy, sz), (dx, dy, dz) = g.peekf(cam + 0x20, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
        (px, py, _) = g.peekf(ped + 0x30, 3)
        h = math.hypot(dx - sx, dy - sy)
        ux, uy = (dx - sx) / h, (dy - sy) / h
        t = math.hypot(px - sx, py - sy) + 8.0
        on_ped = self.ped_at(sx + ux * t, sy + uy * t)
        off_ped = self.ped_at(sx + ux * t - uy * 2.0, sy + uy * t + ux * 2.0)
        time.sleep(0.5)
        for _ in range(3):  # the camera's line through his middle: the aim tilted until it does
            (sx, sy, sz), (dx, dy, dz) = g.peekf(cam + 0x20, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
            (qx, qy, qz) = g.peekf(on_ped + 0x30, 3)
            along = math.hypot(qx - sx, qy - sy)
            ray = sz + (dz - sz) * along / math.hypot(dx - sx, dy - sy)
            g.cmd("mouse 0 %d" % round(-math.atan2(qz - ray, along) * 400))
            g.wait_frames(10)
        shots0 = self.aim()[1]
        g.cmd("press mouseleft 300")
        time.sleep(1.0)
        hit, missed = g.peekf(on_ped + self.PED_HEALTH, 1)[0], g.peekf(off_ped + self.PED_HEALTH, 1)[0]
        self.shot(name)
        return hit, missed, self.aim()[1] - shots0

    def car_free_aim(self):
        """Free aim in a car: the game's own free aim in a mission's drive-by mode (SET_PLAYER_DRIVEBY_MODE, as Jive
        Drive sets it: Vic in the passenger seat with an SMG), the left stick turning it with free aim off (the PS2), the
        mouse with it on (the left stick then not), the game's height limits, shots through the circle."""
        g = self.g
        S = rig.Scm
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)
        g.cmd("trainer toggle god 1")
        self.give(25, 500)
        g.autodrive(speed=0.0)  # the car stands, an AI driver at the wheel
        time.sleep(2.0)
        g.scm(S.op(0x0442, S.PLAYER, S.i(1)) + S.op(0x0481, S.PLAYER, S.i(1)))  # SET_CHAR / SET_PLAYER_DRIVEBY_MODE
        time.sleep(2.0)
        ped = g.player()[0]
        target = g.u32(ped + self.PED_TARGET)
        kind = g.u32(target + 0x48) & 0xE if target else 0

        def turned(h0):
            return (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi

        def mouse_turn():
            h0 = self.aim_heading()
            for _ in range(20):
                g.cmd("mouse 20 0")
            g.wait_frames(15)
            return turned(h0)

        def stick_turn():
            h0 = self.aim_heading()
            g.cmd("key d down")
            time.sleep(1.0)
            g.cmd("key d up")
            g.wait_frames(5)
            return turned(h0)

        # free aim off: the game's (the left stick turns it, the mouse does not)
        self.setting("mouse.free_aim", 0)
        g.wait_frames(5)
        off = self.aim()[0], mouse_turn(), stick_turn()
        g.cmd("gamepad on")
        g.cmd("gamepad rb 1")
        rb_off = self.pad()["buttons"]
        g.cmd("gamepad rb 0")
        g.cmd("gamepad off")
        self.ok("in a car, free aim off: the gamepad's R1 is R1", rb_off == R1, "buttons %04x" % rb_off)
        self.ok("in a car, free aim off: the game's drive-by aim (left stick)",
                self.cam()[0] == 11 and kind == 8 and off[0] == 0 and abs(off[1]) < 0.02 and abs(off[2]) > 0.2,
                "mode %d target kind %d, active %d, mouse %+.3f, left stick %+.3f" % ((self.cam()[0], kind) + off))

        # free aim on: the mouse turns it (400 counts a radian) and the camera with it, the left stick does not
        self.setting("mouse.free_aim", 1)
        g.wait_frames(5)
        c0 = self.cam()[2]
        on = self.aim()[0], mouse_turn()
        cturn = (self.cam()[2] - c0 + math.pi) % (2 * math.pi) - math.pi
        still = stick_turn()
        p = self.held("d")
        self.ok("in a car, free aim on: the mouse aims", on[0] == 1 and abs(on[1] + 1.0) < 0.05 and abs(cturn + 1.0) < 0.15 and
                abs(still) < 0.01 and p["lx"] in (127, 128),
                "active %d, mouse %+.3f, camera %+.3f, left stick %+.3f (lx %d)" % (on + (cturn, still, p["lx"])))
        # and the gamepad's right stick (2 radians a second at full tilt), whichever device's setting is on
        g.cmd("gamepad on")
        h0 = self.aim_heading()
        g.cmd("gamepad rx 1")
        time.sleep(0.5)
        g.cmd("gamepad rx 0")
        time.sleep(0.3)
        pad = turned(h0)
        self.ok("in a car: the right stick aims", -1.4 < pad < -0.6, "aim %+.3f" % pad)
        # the gamepad's R1 (RB) fires there, as Circle (the right thumb is on the stick)
        shots0 = self.aim()[1]
        g.cmd("gamepad rb 1")
        rb = self.pad()["buttons"]
        time.sleep(1.0)
        g.cmd("gamepad rb 0")
        time.sleep(0.3)
        fired = self.aim()[1] - shots0
        g.cmd("gamepad off")
        self.ok("in a car: the gamepad's R1 fires", rb == CIRCLE and fired >= 1, "buttons %04x, %d shot(s)" % (rb, fired))

        # up and down within the game's limits there: 20 degrees above level, 5 below
        def elevation():
            (px, py, pz), (tx, ty, tz) = g.peekf(ped + 0x30, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
            return math.degrees(math.atan2(tz - pz, math.hypot(tx - px, ty - py)))
        g.cmd("mouse 0 -400")
        g.wait_frames(10)
        up = elevation()
        g.cmd("mouse 0 %d" % round(math.radians(20.0) * 400))  # level again
        g.wait_frames(10)
        # (seen from Vic: the game pulls the dummy along the camera's line onto what it hits, a degree or so either way)
        self.ok("in a car: the aim's height in the game's limits", 18.0 < up < 22.0, "%.1f deg up" % up)

        hit, missed, shots = self.shot_under_circle(ped, "car_free_aim")
        self.ok("in a car: the shot goes through the circle", hit < 100 and missed == 100 and shots >= 1,
                "under the circle %.0f, beside it %.0f, %d shot(s) down the camera line" % (hit, missed, shots))

        # out of the car: free aim ends with the game's (the drive-by modes switched off keep it until then - the game's
        # own doing, the same with VCS_FREEAIM=0)
        g.scm(S.op(0x0481, S.PLAYER, S.i(0)) + S.op(0x0442, S.PLAYER, S.i(0)))
        g.cmd("press f 300")
        t0 = time.time()
        while time.time() - t0 < 8 and g.u32(ped + 0x480):
            time.sleep(0.25)
        time.sleep(1.5)
        ended = self.aim()[0], self.cam()[0], self.pad()["lx"]
        self.setting("mouse.free_aim", 0)
        g.cmd("trainer toggle god 0")
        self.ok("in a car: leaving it ends it", ended[0] == 0 and ended[1] == 4 and not g.u32(ped + 0x480),
                "active %d mode %d" % ended[:2])

    def forced_free_aim(self):
        """The forced free aim on foot (SET_PLAYER_FORCE_FREE_AIMING, as Phil's shooting range sets it: a pistol, Vic
        standing): the left stick turns it with free aim off (the PS2), the mouse with it on (the left stick then not),
        the right button pressed and let go keeps it (our own free aim's R1 + L3 used to end it), the gamepad's R1
        fires, the game's limits (110 degrees either side, 40 up), shots through the circle, the script's end ends it."""
        g = self.g
        S = rig.Scm
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)
        self.give(19)
        g.scm(S.op(0x04D6, S.PLAYER, S.i(1)))  # SET_PLAYER_FORCE_FREE_AIMING
        time.sleep(1.5)
        ped = g.player()[0]
        target = g.u32(ped + self.PED_TARGET)
        kind = g.u32(target + 0x48) & 0xE if target else 0
        forced = bool(g.u32(ped + 0xCCC) & 0x200000)

        def turned(h0):
            return (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi

        def mouse_turn(counts=400):
            h0 = self.aim_heading()
            for _ in range(20):
                g.cmd("mouse %d 0" % (counts // 20))
            g.wait_frames(15)
            return turned(h0)

        def stick_turn():
            h0 = self.aim_heading()
            g.cmd("key d down")
            time.sleep(0.5)
            g.cmd("key d up")
            g.wait_frames(5)
            return turned(h0)

        def aiming():
            target = g.u32(ped + self.PED_TARGET)
            return self.cam()[0] == 11 and target and g.u32(target + 0x48) & 0xE == 8

        # free aim off: the game's (the left stick turns it, the mouse does not)
        self.setting("mouse.free_aim", 0)
        g.wait_frames(5)
        off = self.aim()[0], mouse_turn(), stick_turn()
        self.ok("forced free aim, free aim off: the game's (left stick)",
                forced and self.cam()[0] == 11 and kind == 8 and off[0] == 0 and abs(off[1]) < 0.02 and abs(off[2]) > 0.2,
                "forced %d mode %d target kind %d, active %d, mouse %+.3f, left stick %+.3f" % ((forced, self.cam()[0], kind) + off))

        # free aim on: the mouse turns it (400 counts a radian) and the camera with it, the left stick does not
        self.setting("mouse.free_aim", 1)
        g.wait_frames(5)
        c0 = self.cam()[2]
        on = self.aim()[0], mouse_turn()
        cturn = (self.cam()[2] - c0 + math.pi) % (2 * math.pi) - math.pi
        still = stick_turn()
        p = self.held("d")
        self.ok("forced free aim: the mouse aims", on[0] == 1 and abs(on[1] + 1.0) < 0.05 and abs(cturn + 1.0) < 0.15 and
                abs(still) < 0.01 and p["lx"] in (127, 128),
                "active %d, mouse %+.3f, camera %+.3f, left stick %+.3f (lx %d)" % (on + (cturn, still, p["lx"])))

        # the right button held and let go: still the script's free aim (no R1 + L3 of ours), the mouse still aims
        g.cmd("key mouseright down")
        time.sleep(0.8)
        g.cmd("key mouseright up")
        time.sleep(1.0)
        after = self.aim()[0], aiming(), mouse_turn(-400)
        self.ok("forced free aim: the right button keeps it", after[0] == 1 and after[1] and abs(after[2] - 1.0) < 0.05,
                "active %d aiming %d, mouse %+.3f" % after)

        # the gamepad: the right stick aims, R1 (RB) fires as Circle (the right thumb is on the stick), as in the car
        g.cmd("gamepad on")
        h0 = self.aim_heading()
        g.cmd("gamepad rx 1")
        time.sleep(0.5)
        g.cmd("gamepad rx 0")
        time.sleep(0.3)
        pad = turned(h0)
        shots0 = self.aim()[1]
        g.cmd("gamepad rb 1")
        rb = self.pad()["buttons"]
        time.sleep(1.0)
        g.cmd("gamepad rb 0")
        time.sleep(0.5)
        fired = self.aim()[1] - shots0
        g.cmd("gamepad off")
        mouse_turn(round(pad * 400))  # back to where it was
        self.ok("forced free aim: the right stick aims, R1 fires", -1.4 < pad < -0.6 and rb == CIRCLE and fired >= 1 and aiming(),
                "aim %+.3f, buttons %04x, %d shot(s)" % (pad, rb, fired))

        # the game's limits on foot: 110 degrees either side of Vic's heading, 40 above level
        fx, fy = g.peekf(ped + 0x10, 2)
        heading = math.atan2(fy, fx)
        mouse_turn(1600)  # 4 radians to the right
        side = math.degrees((self.aim_heading() - heading + math.pi) % (2 * math.pi) - math.pi)
        mouse_turn(round(math.radians(side) * 400))  # back along his heading
        g.cmd("mouse 0 -800")
        g.wait_frames(10)
        (px, py, pz), (tx, ty, tz) = g.peekf(ped + 0x30, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
        up = math.degrees(math.atan2(tz - (pz + 0.6), math.hypot(tx - px, ty - py)))
        g.cmd("mouse 0 %d" % round(math.radians(up) * 400))  # level again
        g.wait_frames(10)
        # (seen from Vic: the game pulls the dummy along the camera's line onto what it hits, a few degrees either way)
        self.ok("forced free aim: the game's limits", -112.0 < side < -106.0 and 38.0 < up < 42.0,
                "%.1f deg to the side, %.1f deg up" % (side, up))

        hit, missed, shots = self.shot_under_circle(ped, "forced_free_aim")
        self.ok("forced free aim: the shot goes through the circle", hit < 100 and missed == 100 and shots >= 1,
                "under the circle %.0f, beside it %.0f, %d shot(s) down the camera line" % (hit, missed, shots))

        # the script ends it (the range's last SET_PLAYER_FORCE_FREE_AIMING 0)
        g.scm(S.op(0x04D6, S.PLAYER, S.i(0)))
        time.sleep(1.5)
        ended = self.aim()[0], self.cam()[0]
        self.setting("mouse.free_aim", 0)
        self.ok("forced free aim: the script's end ends it", ended == (0, 4), "active %d mode %d" % ended)

    def run(self):
        g = self.g = rig.Game.start(save=os.path.join(ROOT, "build", "rig", "saves", "fortbaxter"), name="controls_check",
                                    port=self.port, data_dir=os.path.join(ROOT, "build", "controls", "data"),
                                    env={"VCS_FPS": "30", "VCS_TIMING": "",  # timing unlocked: the presets set it
                                         "VCS_SCREENSHOT_DIR": os.path.join(self.out, "screenshot_key")})
        try:
            g.wait_ingame()
            # the player's own vcs.ini is read by hidden runs too (never written): start from the defaults
            self.ok("start from the defaults", self.setting("advanced.reset_all") == "pressed")
            g.prime()
            g.teleport(75.0, 175.0, -100.0)
            time.sleep(2)
            idle = self.pad()
            self.ok("idle report", idle.get("buttons") == 0 and all(idle.get(a) in (127, 128) for a in ("lx", "ly", "rx", "ry")), str(idle))

            # every default binding
            bad = []
            for key, want in DEFAULTS:
                p = self.held(key)
                if isinstance(want, int):
                    good = p["buttons"] == want
                else:
                    good = p[want[0]] == want[1] and p["buttons"] == 0
                if not good:
                    bad.append("%s -> %s" % (key, p))
            self.ok("default bindings (%d keys)" % len(DEFAULTS), not bad, "; ".join(bad))
            p = self.held("w", "d", "shift", "e")
            self.ok("keys together", p["ly"] == 0 and p["lx"] == 255 and p["buttons"] == CROSS | R1, str(p))

            # the game sees them: Vic runs forward
            far = self.moved("w")
            self.ok("W moves Vic", far > 1.0, "%.2f m" % far)

            # rebinding: forward on G, W does nothing then
            self.setting("keys.move_up", KEY_G)
            far_g = self.moved("g")
            far_w = self.moved("w")
            self.ok("rebound key moves Vic, the old one doesn't", far_g > 1.0 and far_w < 0.2, "G %.2f m, W %.2f m" % (far_g, far_w))
            self.setting("keys.move_up", 87)
            self.setting("keys.cross", KEY_G)
            p_g, p_shift = self.held("g"), self.held("shift")
            self.ok("Cross rebound", p_g["buttons"] == CROSS and p_shift["buttons"] == 0, "G %s, Shift %s" % (p_g, p_shift))
            self.setting("keys.cross", KEY_SHIFT)

            # walk: the stick at the walking speed while held
            self.setting("keys.walk", KEY_G)
            p = self.held("w", "g")
            want = round(128 - 0.45 * 128)
            self.ok("walk key", abs(p["ly"] - want) <= 1, "ly %d (want %d)" % (p["ly"], want))
            walk = self.setting("keys.walk", 0)
            self.ok("walk unbound", walk == "0", walk)

            self.mouse_and_driving()
            self.free_aim()
            self.car_free_aim()
            self.forced_free_aim()

            # a wheel notch as L1 (second slot); the menu key keeps F2 (the menu refuses keys a control has)
            self.setting("keys.l12", KEY_WHEEL_UP)
            p = self.held("wheelup")
            self.ok("wheel binding", p["buttons"] == L1, str(p))
            self.setting("keys.l12", 0)

            # settings: presets and reset
            self.setting("presets.enhanced")
            got = {k: self.setting(k) for k in ("display.aspect", "display.hud", "display.msaa", "display.timing")}
            self.ok("Enhanced preset", got == {"display.aspect": "0", "display.hud": "2", "display.msaa": "4", "display.timing": "1"}, str(got))
            self.setting("presets.authentic")
            got = {k: self.setting(k) for k in ("display.aspect", "display.hud", "display.msaa", "display.timing", "display.fps")}
            self.ok("Authentic preset", got == {"display.aspect": "43", "display.hud": "0", "display.msaa": "1", "display.timing": "0",
                                                "display.fps": "30"}, str(got))
            self.setting("keys.cross", KEY_G)
            self.setting("audio.volume", 40)
            self.setting("pad.vibration", 0)
            self.setting("advanced.reset_all")
            got = {k: self.setting(k) for k in ("keys.cross", "audio.volume", "pad.vibration", "controls.menu_key")}
            self.ok("Reset all settings", got == {"keys.cross": str(KEY_SHIFT), "audio.volume": "100", "pad.vibration": "100",
                                                  "controls.menu_key": str(KEY_F2)}, str(got))
            p = self.held("shift")
            self.ok("bindings after the reset", p["buttons"] == CROSS, str(p))

            # the screenshot key (F8): a PNG of the window in VCS_SCREENSHOT_DIR (written on a thread of its own: the log
            # line says when), the pad left alone
            shots = os.path.join(self.out, "screenshot_key")
            p = self.held("f8")
            saved = None
            for _ in range(50):
                with open(g.log_path, encoding="utf-8", errors="replace") as f:
                    saved = re.search(r"\[screenshot\] (.+\.png) \((\d+)x(\d+)", f.read())
                if saved:
                    break
                time.sleep(0.1)
            pngs = [n for n in os.listdir(shots) if n.endswith(".png")] if os.path.isdir(shots) else []
            self.ok("screenshot key", saved is not None and len(pngs) == 1 and p["buttons"] == 0 and
                    os.path.getsize(os.path.join(shots, pngs[0])) > 10000,
                    "%s, %s, pad %s" % (saved.group(0) if saved else "no [screenshot] line", pngs, p))

            # the settings menu's pages
            for page in ("Display", "Graphics", "Audio", "Game", "Keyboard", "Gamepad", "Advanced"):
                g.cmd("menu " + page)
                g.wait_frames(4)
                self.shot("menu_" + page.lower())
                if page == "Keyboard":  # its later sections
                    g.cmd("menu scroll 0.6")
                    g.wait_frames(4)
                    self.shot("menu_keyboard_lower")
            g.cmd("menu close")
            p = self.held("shift")
            self.ok("input back after the menu", p["buttons"] == CROSS, str(p))

            # vibration: an explosion next to Vic (the game shakes the pad; god mode so he lives)
            g.cmd("trainer toggle god 1")
            x, y, z = g.player()[1]
            g.scm(rig.Scm.op(0x013C, rig.Scm.f(x + 2.0), rig.Scm.f(y), rig.Scm.f(z), rig.Scm.i(0)))
            time.sleep(2.0)
            with open(g.log_path, encoding="utf-8", errors="replace") as f:
                log = f.read()
            m = re.search(r"\[input\] the game vibrates the pad \(small (\d+), large (\d+)\)", log)
            self.ok("the game's vibration reaches the host", m is not None, m.group(0) if m else "no vibration logged")
            self.ok("player bindings in use", "[input] player bindings" in log)
            # the sound levels with their save load here, after play in the city with god mode on: the load used to hang
            # on "Loading..." (the trainer cancelled a script the load had started in its slot, game-internals.md 11d)
            self.sound_levels()

            # Start last (it opens the pause menu); Alt+Enter is full screen, not Start
            p = self.held("alt", "return")
            self.ok("Alt+Enter is not Start", not p["buttons"] & START, str(p))
            for key, want in START_KEYS:
                p = self.held(key)
                self.ok("%s is Start" % key, p["buttons"] == want, str(p))
                time.sleep(1.0)
            self.ok("still running", g.alive())
        finally:
            g.quit()
        fails = [r for r in self.results if not r[1]]
        with open(os.path.join(self.out, "summary.txt"), "w", encoding="utf-8") as f:
            for name, good, detail in self.results:
                f.write("%-4s %s %s\n" % ("ok" if good else "FAIL", name, detail))
            f.write("%d checks, %d failed\n" % (len(self.results), len(fails)))
        print("%d checks, %d failed -> %s" % (len(self.results), len(fails), self.out))
        return not fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", type=int, default=47673)
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "controls", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    sys.exit(0 if Check(out, a.port).run() else 1)


if __name__ == "__main__":
    main()
