// The mouse on the camera behind the player on foot (docs/CONTROLS.md, game-internals.md 12b "On foot: the camera free
// of the player"). As behind a vehicle (game/vcs_carcam.h): the mouse turns the camera itself and the camera stays
// where it is put while the mouse moves; some time after it stops (Keyboard > Walking camera return), while the player
// moves, it eases back behind him and the game's own camera carries on. Two things differ from the vehicle:
//
//  - The player's direction is his own. The game walks him relative to the camera's heading (TheCamera + 0x7E8), so a
//    camera that turns steers him. Here the movement keys take that heading when they are pressed, and while they stay
//    held the player's control is given the same heading again whatever the camera has done since: he keeps his
//    direction, A / D run straight lines, and the mouse only looks.
//  - Walls: with its stick at rest the game's camera turns away from a wall instead of being pulled in toward the
//    player, which refused a mouse's turn toward a building. While the mouse holds the camera it takes the game's own
//    path for a turned stick: the pull-in.
//
// All of it for the keyboard and mouse only: the gamepad has the game's camera and its camera-relative stick, at once
// when it is used. VCS_FOOTCAM=0 leaves the module out (the mouse then turns the camera at the pad read and steers the
// player, as before: game/vcs_input.cpp).
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
    // has the camera and the player's direction at once.
    void enable(bool on);

    // Game thread, at the pad read: the mouse's turn since the last one, radians (left +, up +). The camera's next
    // update applies it.
    void mouse(float turn, float tilt);

    // Game thread, at the pad read: the movement keys held, as a direction - 0 none (or the gamepad's stick moves the
    // player), else 1..9 without 5: (x + 1) + (y + 1) * 3 + 1 for x, y in -1 / 0 / 1. A change takes the camera's
    // heading afresh.
    void walkKeys(int direction);

    // Game thread, at the pad read: the right stick turns the camera (the camera keys; the gamepad's is enable(false)
    // already). The camera's next update leaves it to the game at once, and while the stick turns it the player
    // follows the camera as the game has it (the keys take its heading afresh every frame).
    void stickMoved();

    // Seconds after the mouse stops before the camera goes back behind the player; < 0 never.
    void setReturnDelay(float seconds);
}
