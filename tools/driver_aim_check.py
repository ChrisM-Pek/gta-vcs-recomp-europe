#!/usr/bin/env python3
"""Check driver mouse controls and the 25 fps cap in a hidden game with a copied save."""
import math
from pathlib import Path

import rig
from framerate_audit import BIKE, REGION, RUNWAY_SPOT
from perf_report import load, pct, summary_line


def check(g):
    g.wait_ingame()
    g.cmd("setting advanced.reset_all")
    g.cmd("setting display.fps 30")
    g.prime()
    x, y, heading = RUNWAY_SPOT
    g.teleport(x, y, -100.0, heading)
    g.wait_frames(90)
    s = rig.Scm
    for weapon in (19, 25):
        g.scm(s.op(s.GIVE_WEAPON_TO_CHAR, s.PLAYER, s.i(weapon), s.i(200)))
    g.spawn_car()
    g.wait_frames(45)
    ped = g.player()[0]
    car = g.u32(ped + 0x480)
    assert car and g.u32(car + 0x384) == ped

    def camera_front():
        camera = 0x6F44D0 + REGION
        cam = camera + 0x70 + 0x270 * g.peek(camera + 0x50, 1)[0]
        assert g.u32(cam) & 0xFFFF == 18
        return g.peekf(cam + 0x10, 3)

    def weapon():
        slot = g.peek(ped + 0x789, 1)[0]
        addr = ped + 0x578 + slot * 0x1C
        return g.u32(addr), g.u32(addr + 0xC)

    def aim():
        return {key: int(value) for key, value in (word.split("=") for word in g.cmd("aimstate").split())}

    def fire(expected):
        kind, ammo = weapon()
        assert kind == expected, (kind, expected)
        before = aim()["shots"]
        g.cmd("press mouseleft 1600")
        g.wait_frames(60)
        new_kind, after_ammo = weapon()
        shots = aim()["shots"] - before
        assert new_kind == expected and shots == ammo - after_ammo and shots > 0, (expected, shots, ammo, after_ammo)
        return shots

    g.cmd("setting mouse.free_aim 1")
    front = camera_front()
    g.cmd("key mouseright down")
    g.wait_frames(10)
    assert aim()["driver"] == 1 and g.cmd("padstate").startswith("buttons=0000 ")
    g.cmd("mouse 150 20")
    g.wait_frames(10)
    assert math.dist(front, camera_front()) > 0.15
    out = Path(rig.ROOT, "build", "run", "driver_aim_check.png").as_posix()
    g.cmd("screenshot " + out)
    smg = fire(25)
    g.cmd("press wheeldown 100")
    g.wait_frames(12)
    pistol = fire(19)
    g.cmd("press wheelup 100")
    g.wait_frames(12)
    assert weapon()[0] == 25
    before, forward = g.peekf(car + 0x30, 2), g.peekf(car + 0x10, 2)
    g.cmd("key w down")
    g.cmd("key d down")
    g.wait_frames(32)
    g.cmd("key d up")
    g.cmd("key w up")
    assert math.dist(before, g.peekf(car + 0x30, 2)) > 1.0 and math.dist(forward, g.peekf(car + 0x10, 2)) > 0.1
    assert aim()["driver"] == 1 and g.u32(ped + 0x8C4) == 0x37
    g.cmd("key mouseright up")
    g.wait_frames(10)
    assert aim()["driver"] == 0
    station = g.peek(car + 0x2BA, 1)
    g.cmd("press wheeldown 100")
    g.wait_frames(40)
    assert weapon()[0] == 19 and g.peek(car + 0x2BA, 1) == station
    g.cmd("setting mouse.free_aim 0")
    g.cmd("press wheelup 100")
    g.wait_frames(40)
    assert weapon()[0] == 25 and g.peek(car + 0x2BA, 1) == station
    g.cmd("key mouseright down")
    g.wait_frames(5)
    assert aim()["driver"] == 0 and g.cmd("padstate").startswith("buttons=0800 ")
    g.cmd("key mouseright up")

    print(f"Driver aim: {smg} SMG shots, {pistol} pistol shots; wheel works without tuning radio")

    z = g.player()[1][2]
    g.scm(s.op(0x0216, s.PLAYER, s.f(x), s.f(y), s.f(z)))  # WARP_CHAR_FROM_CAR_TO_COORD
    g.wait_frames(25)
    z = g.player()[1][2] + 0.5
    g.scm(s.op(s.REQUEST_MODEL, s.i(BIKE)) + s.op(s.LOAD_ALL_MODELS_NOW) +
          s.op(s.CREATE_CAR, s.i(BIKE), s.f(x + 4), s.f(y), s.f(z), s.LOCAL0) +
          s.op(s.WARP_CHAR_INTO_CAR, s.PLAYER, s.LOCAL0))
    g.wait_frames(35)
    bike = g.u32(ped + 0x480)
    assert bike and bike != car and g.u32(bike + 0x384) == ped, (hex(bike), hex(car), g.u32(ped + 0x8C4))
    before = weapon()[0]
    g.cmd("press wheeldown 100")
    g.wait_frames(15)
    assert weapon()[0] != before and weapon()[0] in (19, 25)
    print("Weapon wheel: passed on a motorcycle")

    assert g.cmd("setting display.fps 25") == "25"
    g.wait_frames(150)


if __name__ == "__main__":
    perf = Path(rig.ROOT, "build", "run", "driver_aim_check.csv")
    game = rig.Game.start(save=rig.default_save(), name="driver_aim_check", seconds=180, port=47693,
                          env={"VCS_FPS": "", "VCS_TIMING": "1", "PS2X_PERF_LOG": str(perf)})
    try:
        check(game)
    finally:
        game.quit()
    # The last 100 frames exclude any settling after changing the cap. Average FPS alone hid the 22/40/58 ms cadence.
    rows = load(perf, None, None)[-100:]
    _, fps, _, ms = summary_line(rows)
    p95 = pct(ms, 0.95)
    assert len(rows) == 100 and 24 <= fps <= 26 and p95 < 46, (len(rows), fps, p95)
    print(f"25 FPS cap: measured {fps:.2f} FPS, p95 {p95:.1f} ms")
