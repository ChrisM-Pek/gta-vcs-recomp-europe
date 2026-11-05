// VCS display settings applied to the game: Hor+ widescreen through the camera's view window (see vcs_display.cpp).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs
{
    // r = display aspect / (4:3): 1 = the original view, 4/3 = 16:9 Hor+. Any thread; applies at the next view-window set.
    void setAspectWidening(float r);
    float aspectWidening();

    // "Car reflections" (Graphics): how the reflection map (FBP 420, 128 wide) reads the frame. 0 Original (the GS's four
    // bilinear taps per map texel), 1 Smooth (a filter over each texel's whole footprint), 2 Auto (Smooth above 1x
    // internal resolution, Original at 1x). docs/game-internals.md 6g. Any thread, any time.
    void setReflectionFilter(int mode);

    // Game thread, before CCamera::IsSphereVisible (FUN_003816F8) runs on this camera: widens its side planes to the
    // Hor+ view. Called by the display hooks' wrapper, and by any other wrapper of that function.
    void prepareSphereTest(uint8_t *rdram, uint32_t camera);

    // At start-up (applyVcs): wraps the view-window setter and the sphere test.
    void installDisplayHooks(PS2Runtime &runtime);

    // Game thread, once per game frame (testing: VCS_WIDESCREEN forces the game's own preference).
    void applyDisplay(uint8_t *rdram);
}
