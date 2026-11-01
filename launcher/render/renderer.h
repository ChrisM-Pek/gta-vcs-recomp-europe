// A graphics backend: turns the draw list and the glyph atlas into pixels. Five calls and a readback; every backend
// draws the same instances with the same shading (render/d3d11 on Windows, render/vulkan on Linux; both compile the
// one ui.hlsl).
#pragma once

#include "core/math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace render
{
    struct RendererStats
    {
        int instances = 0, drawCalls = 0;
        size_t uploadBytes = 0;
        std::string api;     // "Direct3D 11", "Vulkan 1.3"
        std::string adapter;
        bool warp = false, tearing = false;
    };
    extern RendererStats rendererStats;

    // window: the native handle to present to (an HWND; Linux: a platform::NativeSurface), or null to draw into an
    // offscreen texture of
    // width x height (tests: --screenshot, never a window on the desktop)
    bool rendererInit(void *window, int width, int height, std::string &error);
    void rendererShutdown();
    // blocks until the display can take a new frame (the swap chain's latency waitable); once per present
    void rendererWaitForFrame();
    void rendererRender(core::Color clear, int width, int height);
    void rendererPresent(bool vsync);
    // the last frame rendered, RGBA8 top-down (call after rendererRender)
    bool rendererReadback(std::vector<uint8_t> &rgba, int &width, int &height);
    // a debug switch: the device is dropped and recreated at the next present, as after a driver reset
    void rendererSimulateDeviceLoss();
}
