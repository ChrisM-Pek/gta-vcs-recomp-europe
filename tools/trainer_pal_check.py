#!/usr/bin/env python3
"""Check PAL trainer actions in an already running, isolated test game.

Start a hidden test with its own VCS_DATA_DIR and PS2X_CONTROL_PORT first; this
changes the player and creates a vehicle, so never point it at a real save.
"""
import argparse
import time

import rig


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=47620)
    args = parser.parse_args()
    game = rig.Game(None, "", args.port)
    state = game.cmd("trainer state")
    assert "ready=1" in state, state
    ped = game.u32(0x4E4C10)
    assert ped, "PAL player missing"
    print("ready:", state, flush=True)
    print("spawn:", game.cmd("trainer spawn 171"), flush=True)
    car = game.u32(ped + 0x480)
    assert car and game.u32(ped + 0x8C4) == 0x37, "player did not enter vehicle"
    game.cmd("pokef 0x%x 400" % (car + 0x278))
    before = game.peekf(car + 0x278, 1)[0]
    reply = game.cmd("trainer repair")
    after = game.peekf(car + 0x278, 1)[0]
    assert before == 400 and after == 1000, (before, after, reply)
    print("repair: %.0f -> %.0f (%s)" % (before, after, reply), flush=True)
    game.cmd("trainer toggle vehicle 1")
    game.cmd("pokef 0x%x 350" % (car + 0x278))
    time.sleep(1)
    health = game.peekf(car + 0x278, 1)[0]
    game.cmd("trainer toggle vehicle 0")
    assert health == 1000, health
    print("vehicle invincibility:", health, flush=True)


if __name__ == "__main__":
    main()
