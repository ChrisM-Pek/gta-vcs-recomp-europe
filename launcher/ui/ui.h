// Immediate-mode UI with retained animation state.
//
// Widgets are functions called every frame with their rectangle: they draw themselves and say what happened
// (clicked, changed). There is no widget tree to keep in step with the model. What must survive between frames -
// hover / press / focus animations, springs, a scroll position - lives in a table keyed by the widget's id and is
// dropped once the widget stops being drawn.
//
// Animations are time based and report `ui::state.animating` while they move, so the frame loop draws exactly as
// long as something is in motion; a widget that needs a frame later (a tooltip's delay, a clock) asks with wakeAt()
// and the loop sleeps until then.
//
// Input layers: an overlay (a sheet, a dialog) raises the layer; widgets of lower layers stop taking the mouse and the
// keyboard while it is up, from the frame after it appeared.
//
// Keyboard: Tab / Shift+Tab walk the focusable widgets in the order they were drawn last frame; the focused one shows
// a ring (when focus came from the keyboard) and takes Enter / Space.
#pragma once

#include "core/math.h"
#include "platform/platform.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ui
{
    using core::Color;
    using core::Rect;
    using Id = uint64_t;

    struct Widget
    {
        float hover = 0, press = 0, focus = 0; // 0..1
        float t = 0, t2 = 0;                   // widget-specific
        core::Spring spring, spring2;
        double since = 0;                      // widget-specific time
        int value = -1, previous = -1;         // widget-specific (a badge's status and the one it morphs from)
        uint64_t lastFrame = 0;
        bool fresh = true;                     // first frame it exists
    };

    struct State
    {
        float scale = 1;          // pixels per logical unit
        float dt = 0;             // seconds, clamped
        double time = 0;          // animation clock, seconds
        double wall = 0;          // wall clock, seconds (timers)
        uint64_t frame = 0;
        float motion = 1;         // 0: reduced motion, everything snaps

        Id hot = 0, hotNext = 0;  // under the mouse (last frame's decision, this frame's candidate)
        Id active = 0;            // being pressed
        Id focus = 0;
        bool focusVisible = false;
        std::vector<Id> focusOrder, focusOrderPrev;
        Id focusRequest = 0;

        int layer = 0;            // the layer widgets are being drawn in
        int topLayer = 0, topLayerNext = 0; // the highest layer that wants input
        int disabled = 0;         // > 0: drawn without input (a group fading out)

        bool animating = false;
        double wakeAt = 0;        // the earliest time something wants a frame (0: none)
        platform::Cursor cursor = platform::Cursor::Arrow;

        Id tooltipId = 0;
        double tooltipSince = 0;
        std::string tooltipText;
        Rect tooltipAnchor;

        std::unordered_map<Id, Widget> widgets;
        std::vector<Id> idStack;
    };
    extern State state;

    inline float px(float logical) { return logical * state.scale; }
    float hairline(); // one logical pixel, never thinner than a physical one
    Rect snap(Rect r); // edges on whole pixels, so hairlines stay crisp

    void beginFrame(float dt, double wall);
    void endFrame();

    Id id(std::string_view label);  // hashed with the id stack
    Id id(std::string_view label, int index);
    void pushId(std::string_view label);
    void pushId(int index);
    void popId();
    Widget &widget(Id id);

    // ---- animation ------------------------------------------------------------------------------------------------
    // approaches target exponentially; keeps frames coming until it arrives
    float animate(float &value, float target, float speed);
    bool spring(core::Spring &s, float target, float stiffness = 300.0f, float damping = 0.75f);
    void keepAnimating();          // something moves that the helpers don't know about
    void wakeAt(double wallTime);  // a frame then, at the latest

    // ---- interaction ----------------------------------------------------------------------------------------------
    struct Interaction
    {
        bool hovered = false, held = false, clicked = false, rightClicked = false;
        bool pressed = false; // went down this frame
        bool focused = false;
        Widget *w = nullptr;
    };
    // hit-tests r (in the current draw offset), handles press / release / focus, and animates hover, press and focus
    Interaction interact(Id id, Rect r, bool focusable = true, bool enabled = true);
    bool inputAllowed();         // this layer takes input this frame
    bool mouseOver(Rect r);      // r under the mouse, and the layer takes input
    void blockInput(Rect r);     // nothing beneath r is hovered (a card the mouse is over)
    void requestFocus(Id id);

    // a raised layer for an overlay: everything below stops taking input while it is up (blocking); a non-blocking
    // layer only says where its widgets stand (the title bar stays usable under a sheet, not under a dialog)
    void beginLayer(int layer, bool blocking = true);
    void endLayer();
    void pushDisabled();
    void popDisabled();

    void setCursor(platform::Cursor cursor);
    void tooltip(Id id, Rect anchor, std::string_view text); // shown after a moment of hovering
}
