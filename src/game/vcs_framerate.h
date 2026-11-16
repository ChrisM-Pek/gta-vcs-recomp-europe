// VCS frame rate and game timing (docs/NATIVE_PORT_ROADMAP.md milestone 4, docs/game-internals.md 4).
#pragma once

class PS2Runtime;

namespace vcs
{
    void installFrameRateHooks(PS2Runtime &runtime);

    // Game timing: Original = paced by the emulated VBlank as on the PS2 (30, or 60 with a cap of 60 or more);
    // Independent = paced by the host clock to the frame-rate cap, game time from the host clock. Set before the game
    // runs (a switch while it runs would jump the game's clock).
    enum class Timing
    {
        Original = 0,
        Independent = 1,
    };
    void setTiming(Timing timing);
    Timing timing();

    // Frames per second: 25, 30, 60 ... 240 (the most, vcs_framerate.cpp kMaxFps); kCapRefresh = the monitor's refresh rate; kCapUnlimited = no limit
    // (independent timing). Any thread, takes effect at the next frame.
    constexpr int kCapRefresh = -1;
    constexpr int kCapUnlimited = 0;
    void setFrameCap(int fps);
    int frameCap();

    // The pre-independence switch: 60 = setFrameCap(60), else 30.
    void setFrameRate60(bool on);
    bool frameRate60();
}
