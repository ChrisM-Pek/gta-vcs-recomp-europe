#!/usr/bin/env python3
"""Check the PAL trainer and radio wheel in a hidden game with an isolated card."""

import time

import gamectl
import rig

game = rig.Game.start(name='pal_upstream_smoke', port=47641,
                      env={'VCS_SKIP_MOVIES': '1', 'VCS_FPS': '60'})
try:
    start = time.time()
    while time.time() - start < 150:
        if not game.alive():
            raise RuntimeError('game exited')
        try:
            state = game.cmd('trainer state')
            if ('ready=1' in state and game.u32(0x48a27c) == 0 and game.u32(0x4e4c10)
                    and game.u32(0x6f4840) == 4):
                print('trainer:', state, flush=True)
                break
            gamectl.send('press Shift', port=game.port)
        except Exception:
            pass
        time.sleep(3)
    else:
        raise RuntimeError('game did not reach gameplay')
    ped = game.u32(0x4e4c10)
    assert ped
    print('player:', hex(ped), flush=True)
    print('spawn:', game.cmd('trainer spawn 171'), flush=True)
    car = game.u32(ped + 0x480)
    assert car
    time.sleep(3)
    station = lambda: game.peek(car + 0x2ba, 1)[0]
    before = station()
    for attempt in range(30):
        game.cmd('press wheeldown 100')
        time.sleep(2)
        if station() != before:
            break
    assert station() == (before + 1) % 10, (before, station())
    print('radio down:', before, '->', station(), flush=True)
    game.cmd('press wheelup 100')
    time.sleep(2)
    assert station() == before, (before, station())
    print('radio up:', station(), flush=True)
    print('frames:', game.cmd('frames'), flush=True)
finally:
    try:
        gamectl.send('quit', port=game.port)
    except Exception:
        pass
