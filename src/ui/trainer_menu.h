// The trainer menu (docs/TRAINER.md): a list menu in the style of Red Dead Redemption 2's Native Trainer - a panel at
// the left of the screen with a header, the path to the current page, the rows (ON / OFF, < value >, > for a page)
// and a line of help for the selected row - over the game, plus short notifications of what was done. The trainer
// key (controls.trainer_key, F5) or Back + RB on a gamepad opens and closes it. Up / Down select, Left / Right
// change a value, Enter / A applies, Backspace / B goes back a page. While it is open the game gets no input.
// The actions are game/vcs_trainer.h's.
#pragma once

namespace ui
{
    // Main thread, once per host frame inside the Dear ImGui frame (the runtime's debug-UI draw callback).
    void drawTrainerMenu();
    bool trainerMenuOpen();
    bool trainerMenuBlocksInput(); // open, or its keys still held after closing
    // Before drawTrainerMenu, on a frame the screenshot key captures: the notices of earlier shots stay out of it.
    void hideScreenshotNotices();
}
