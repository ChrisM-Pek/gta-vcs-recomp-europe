// The mouse on the camera behind the player on foot (docs/CONTROLS.md, game-internals.md 12b "On foot: the mouse's
// camera"). A PC game's third-person camera in place of the game's own, for the keyboard and mouse:
//
//  - The mouse turns the camera itself, and the player goes where the camera looks: the mouse steers him.
//  - The camera follows where he travels. When a key sends him sideways or back at the camera, it comes round behind
//    him by itself (the vehicle camera's ease), at once - the gamepad's camera does, on its string. With the mouse
//    just moved it waits first (Keyboard > Walking camera return), as behind a vehicle.
//  - That turn of the camera's own does not turn him. The game walks him by the camera's heading, so a camera that
//    swings behind a sideways runner swings him too - the gamepad's tight circle, which a stick can steer against and
//    a key cannot - and one behind a player running at it would never arrive. Each movement key therefore keeps the
//    heading it was pressed at (forward is where the camera looked then) through the camera's own turns, and only
//    those: the mouse turns the keys' headings with the camera.
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
    // Hooks the follow-ped camera (FUN_0029DAB0), the player's control (FUN_00230640) and the first reader of the
    // camera's heading after the world's update (FUN_0036AFD0).
    void install(PS2Runtime &runtime);

    // Installed (not VCS_FOOTCAM=0): the mouse's turn on foot goes to mouse() instead of being written at the pad read.
    bool active();

    // Game thread, at every pad read: the keyboard / mouse is the device in use and Mouse camera is on. Off, the game
    // has its own camera at once, and the keys follow it.
    void enable(bool on);

    // Game thread, at the pad read: the mouse's turn since the last one, radians (left +, up +). The camera's next
    // update applies it.
    void mouse(float turn, float tilt);

    // Game thread, at the pad read: the movement keys held, as a direction - 0 none (or the gamepad's stick moves the
    // player), else 1..9 without 5: (x + 1) + (y + 1) * 3 + 1 for x, y in -1 / 0 / 1 (left / right, forward / back).
    void walkKeys(int direction);

    // Game thread, at the pad read: the right stick turns the camera (the camera keys; the gamepad's is enable(false)
    // already). The camera's next update is the game's own.
    void stickMoved();

    // Seconds after the mouse stops before the camera turns behind the player again; < 0 never.
    void setReturnDelay(float seconds);
}
