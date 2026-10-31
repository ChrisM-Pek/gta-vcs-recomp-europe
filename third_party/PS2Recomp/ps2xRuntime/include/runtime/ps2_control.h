// [gta-recomp] Hidden test runs and remote control.
//
// PS2X_HIDDEN=1 runs the game with its window hidden (not on the desktop or taskbar, never focused), ignores the
// physical keyboard / mouse / gamepad and mutes the sound output unless PS2X_AUDIO=1 - so automated tests neither
// disturb the person at the machine nor pick up their input. PS2X_INPUT=injected ignores physical input with the
// window shown.
//
// PS2X_CONTROL=1 (implied by PS2X_HIDDEN) listens on 127.0.0.1:PS2X_CONTROL_PORT (default 47615) for one-line text
// commands, answered with one line ("ok ..." / "error ..."):
//   ping                      -> ok
//   screenshot <path.png>     -> ok <w>x<h>     the presented picture incl. the perf overlay, rendered off-screen
//   key <name> down|up        -> ok             hold / release a key (names: tools/gamectl.py --help)
//   press <name> [ms=150]     -> ok             press and release; answers after the release
//   perf                      -> ok <summary>   the perf overlay's numbers for the last second
//   quit                      -> ok             stop the game
// tools/gamectl.py is the client.
#pragma once

#include <functional>
#include <string>

namespace ps2control
{
    bool hiddenMode();        // PS2X_HIDDEN
    bool physicalInputOn();   // false when hidden, PS2X_INPUT=injected or the game's input is blocked
    // Main thread: an in-game UI (the settings menu) has the keyboard / mouse / gamepad; the game sees none of it.
    void setGameInputBlocked(bool blocked);
    bool gameInputBlocked();  // [gta-recomp] the pad then reads no keys at all, injected ones included

    // Keyboard state for the game and the debug keys: physical (when allowed) or injected through the control channel.
    // `key` is a platform::Key (platform/platform.h).
    bool keyDown(int key);
    bool keyPressed(int key); // main thread, once per frame (edges of physical or injected presses)

    // Main thread. start() after the window exists; pump() once per frame after drawing - it runs the queued
    // commands; `capture` renders the current picture into a PNG and returns "<w>x<h>" or "" on failure.
    void start();

    // [gta-recomp] More commands, from the game layer (src/debug/rig.cpp). Register before start(). The handler
    // runs on the main thread with the rest of the line; it answers through `reply` ("ok ..." / "error ..."),
    // at once or later from any thread (e.g. after the game thread did the work). One answer per command.
    using Reply = std::function<void(const std::string &answer)>;
    using CommandHandler = std::function<void(const std::string &args, Reply reply)>;
    void addCommand(const std::string &verb, CommandHandler handler);
    // [gta-recomp] Main thread: the player's screenshot key (game layer). The next pump() - this host frame's, after
    // drawing - takes the window as shown (its pixel size, menus and overlays included) for `path`; `done` gets
    // "<w>x<h>" or "" once the file is written, on another thread (the PNG is written there: no hitch in the game).
    using Done = std::function<void(const std::string &size)>;
    void requestScreenshot(const std::string &path, Done done);
    // `capture(path)`: the control channel's screenshot at its size (PS2X_CAPTURE_SIZE, at least 960x672), written
    // before it returns. `shoot(path, done)`: requestScreenshot's.
    void pump(const std::function<std::string(const std::string &path)> &capture,
              const std::function<void(const std::string &path, Done done)> &shoot, const std::function<void()> &quit);
    void stop();
}
