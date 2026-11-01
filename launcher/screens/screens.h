// The launcher's window: a title bar, then one of three modes - Setup (a guided sequence until everything is ready),
// Home (Play) and Task (a build or another long tool running) - with Saves, Settings and Help as sheets over them,
// dialogs and toasts on top. docs/LAUNCHER.md, "The player's flow".
#pragma once

#include "core/math.h"

#include <string>

namespace launcher
{
    class App;
}

namespace screens
{
    // what --screenshot shows: "" (what's next), setup:folder|disc|tools|game, home, task, sheet:saves|settings|help|
    // folder, dialog:restore|quit|cancel, gallery; demo: running|count|error|done|playing|update
    struct Options
    {
        std::string screen, demo;
        bool screenshot = false; // no window: a fixed size, nothing outside the window
        double clock = 0;        // where Home's moving scene starts, seconds (--clock: a picture of a given moment)
    };

    void init(launcher::App &app, const Options &options);
    void frame(launcher::App &app);           // the whole window, every frame
    void show(launcher::App &app, const std::string &screen); // as --screen (and --then)
    void requestQuit(launcher::App &app);     // the close button, Alt+F4: asks first while a task runs
    bool closing();                           // leave the frame loop
    void runPending();                        // file dialogs asked for during the frame: between frames only
    core::Color clearColor();
}
