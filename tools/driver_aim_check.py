#!/usr/bin/env python3
"""Check mouse free aim in the driver's seat, in a hidden game with a copied save."""
import math
from pathlib import Path

import rig
from framerate_audit import REGION, RUNWAY_SPOT


def check(g):
    g.wait_ingame()
    g.cmd("setting advanced.reset_all")
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
    g.cmd("setting mouse.free_aim 0")
    g.cmd("key mouseright down")
    g.wait_frames(5)
    assert aim()["driver"] == 0 and g.cmd("padstate").startswith("buttons=0800 ")
    print(f"Driver aim: {smg} SMG shots, {pistol} pistol shots; mouse, wheel, steering and opt-out passed")


if __name__ == "__main__":
    game = rig.Game.start(save=rig.default_save(), name="driver_aim_check", seconds=180, port=47693,
                          env={"VCS_FPS": "30"})
    try:
        check(game)
    finally:
        game.quit()
