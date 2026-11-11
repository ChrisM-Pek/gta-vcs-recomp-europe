// The mouse on the camera behind the player on foot (docs/CONTROLS.md, game-internals.md 12b "On foot: the mouse's
// camera"). A PC game's third-person camera in place of the game's own, for the keyboard and mouse:
//
//  - The mouse turns the camera itself, and the player goes where the camera looks: the movement keys follow the
//    camera, as the game has them, so the mouse steers him.
//  - The game's camera hangs on a string behind the player: run sideways and it swings round after him, and with the
//    keys following the camera he runs a tight circle about it (the gamepad's way). Here the camera only turns by the
//    mouse while the mouse moves and for a while after it, and after a change of the movement keys (Keyboard >
//    Walking camera return): A / D step and strafe in a straight line. Then, while he moves, it turns behind him
//    slowly - a wide curve, not the circle - and not at all while he runs at the camera, as in the game.
//  - Walls: with its stick at rest the game's camera is not pulled in toward the player at a wall, and a mouse's turn
//    toward a building left it standing at the wall's edge. Here it takes the game's own path for a turned stick: the
//    pull-in.
//
// The gamepad has the game's camera, at once when it is used; so have L1 (the game puts the camera behind him), the
// camera keys and a shot from the hip. VCS_FOOTCAM=0 leaves the module out (the mouse then turns the game's camera at
// the pad read, as before: game/vcs_input.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::footcam
{
    // Hooks the follow-ped camera (FUN_0029DAB0).
    void install(PS2Runtime &runtime);

    // Installed (not VCS_FOOTCAM=0): the mouse's turn on foot goes to mouse() instead of being written at the pad read.
    bool active();

    // Game thread, at every pad read: the keyboard / mouse is the device in use and Mouse camera is on. Off, the game
    // has its own camera at once.
    void enable(bool on);

    // Game thread, at the pad read: the mouse's turn since the last one, radians (left +, up +). The camera's next
    // update applies it.
    void mouse(float turn, float tilt);

    // Game thread, at the pad read: the movement keys held, as a direction - 0 none (or the gamepad's stick moves the
    // player), else 1..9 without 5: (x + 1) + (y + 1) * 3 + 1 for x, y in -1 / 0 / 1. A change holds the camera still
    // for Walking camera return, as the mouse does.
    void walkKeys(int direction);

    // Game thread, at the pad read: the right stick turns the camera (the camera keys; the gamepad's is enable(false)
    // already). The camera's next update is the game's own.
    void stickMoved();

    // Seconds after the mouse stops, or the movement keys change, before the camera turns behind the player; < 0 never.
    void setReturnDelay(float seconds);
}
