#include "ui.h"

#include "render/draw.h"
#include "render/font.h"
#include "ui/theme.h"

#include <algorithm>

namespace ui
{
    State state;

    namespace
    {
        constexpr double kTooltipDelay = 0.55;
        std::vector<int> g_layerStack;

        uint64_t hash(std::string_view s, uint64_t seed)
        {
            uint64_t h = seed ^ 0xcbf29ce484222325ull;
            for (char c : s)
                h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
            return h;
        }

        uint64_t seed() { return state.idStack.empty() ? 0x9E3779B97F4A7C15ull : state.idStack.back(); }
    }

    float hairline() { return std::max(1.0f, std::floor(state.scale)); }

    Rect snap(Rect r)
    {
        const float x0 = std::floor(r.x + 0.5f), y0 = std::floor(r.y + 0.5f);
        const float x1 = std::floor(r.right() + 0.5f), y1 = std::floor(r.bottom() + 0.5f);
        return {x0, y0, x1 - x0, y1 - y0};
    }

    Id id(std::string_view label) { return hash(label, seed()); }

    Id id(std::string_view label, int index) { return hash(label, seed() + uint64_t(index) * 0x9E3779B1ull + 1); }

    void pushId(std::string_view label) { state.idStack.push_back(id(label)); }
    void pushId(int index) { state.idStack.push_back(id("#", index)); }
    void popId()
    {
        if (!state.idStack.empty())
            state.idStack.pop_back();
    }

    Widget &widget(Id id)
    {
        auto [it, inserted] = state.widgets.try_emplace(id);
        Widget &w = it->second;
        w.fresh = inserted || w.lastFrame + 1 < state.frame; // new, or back after not being drawn
        if (w.fresh && !inserted)
        {
            w = Widget();
            w.fresh = true;
        }
        w.lastFrame = state.frame;
        return w;
    }

    void beginFrame(float dt, double wall)
    {
        state.dt = dt;
        state.time += dt;
        state.wall = wall;
        state.frame++;
        state.motion = platform::window.reducedMotion ? 0.0f : 1.0f;
        state.hot = state.hotNext;
        state.hotNext = 0;
        state.topLayer = state.topLayerNext;
        state.topLayerNext = 0;
        state.layer = 0;
        state.disabled = 0;
        state.animating = false;
        state.wakeAt = 0;
        state.cursor = platform::Cursor::Arrow;
        state.idStack.clear();
        g_layerStack.clear();
        state.focusOrderPrev.swap(state.focusOrder);
        state.focusOrder.clear();

        const platform::Input &in = platform::input;
        if (in.pressed[platform::kMouseLeft])
            state.focusVisible = false;
        if (state.focusRequest)
        {
            state.focus = state.focusRequest;
            state.focusRequest = 0;
        }
        if (in.key(platform::kKeyTab) && !state.focusOrderPrev.empty())
        {
            const auto &order = state.focusOrderPrev;
            auto it = std::find(order.begin(), order.end(), state.focus);
            int index = it == order.end() ? (in.shift ? int(order.size()) : -1) : int(it - order.begin());
            index += in.shift ? -1 : 1;
            index = (index + int(order.size())) % int(order.size());
            state.focus = order[size_t(index)];
            state.focusVisible = true;
        }
        if (state.active && !in.down[platform::kMouseLeft] && !in.released[platform::kMouseLeft])
            state.active = 0; // let go somewhere we never saw
    }

    void endFrame()
    {
        // a tooltip whose widget is no longer hovered goes away
        if (state.tooltipId && state.tooltipId != state.hot)
        {
            state.tooltipId = 0;
            state.tooltipText.clear();
        }
        if (state.tooltipId && !state.tooltipText.empty())
        {
            if (state.wall - state.tooltipSince < kTooltipDelay)
                wakeAt(state.tooltipSince + kTooltipDelay);
            else
            {
                const render::Layer previous = render::setLayer(render::Layer::Overlay);
                render::pushClipAbsolute({0, 0, render::draw.width, render::draw.height});
                const float size = px(theme::kSmall);
                const float w = render::measure(render::kFontBody, size, state.tooltipText) + px(20);
                const float h = px(28);
                float x = state.tooltipAnchor.cx() - w * 0.5f;
                float y = state.tooltipAnchor.bottom() + px(8);
                if (y + h > render::draw.height - px(4))
                    y = state.tooltipAnchor.y - h - px(8);
                x = std::clamp(x, px(6), std::max(px(6), render::draw.width - w - px(6)));
                Widget &tw = widget(id("#tooltip"));
                if (tw.fresh)
                    tw.t = 0;
                const float t = animate(tw.t, 1, 22);
                render::pushOpacity(t);
                const Rect box = snap({x, y + (1 - t) * px(4), w, h});
                render::shadow(box.offset(0, px(4)), theme::shade(0.67f), px(10), px(8));
                render::rectEx(box, theme::kSurfaceTop, theme::kSurfaceTop, px(7), theme::ink(0.055f), hairline());
                render::drawText(render::kFontBody, size, box.x + px(10), box.y + px(19), theme::kText, state.tooltipText);
                render::popOpacity();
                render::popClip();
                render::setLayer(previous);
            }
        }
        platform::setCursor(state.cursor);
        // widgets not drawn for a while are forgotten
        if (state.frame % 120 == 0)
            for (auto it = state.widgets.begin(); it != state.widgets.end();)
                it = it->second.lastFrame + 600 < state.frame ? state.widgets.erase(it) : std::next(it);
    }

    float animate(float &value, float target, float speed)
    {
        if (state.motion <= 0 || std::fabs(value - target) < 0.002f)
        {
            value = target;
            return value;
        }
        value = core::damp(value, target, speed, state.dt);
        if (std::fabs(value - target) < 0.002f)
            value = target;
        else
            state.animating = true;
        return value;
    }

    bool spring(core::Spring &s, float target, float stiffness, float damping)
    {
        s.target = target;
        if (state.motion <= 0)
        {
            s.snap(target);
            return false;
        }
        const bool moving = s.update(state.dt, stiffness, damping);
        if (moving)
            state.animating = true;
        return moving;
    }

    void keepAnimating() { state.animating = true; }

    void wakeAt(double wallTime)
    {
        if (state.wakeAt == 0 || wallTime < state.wakeAt)
            state.wakeAt = wallTime;
    }

    bool inputAllowed() { return state.disabled == 0 && state.layer >= state.topLayer; }

    bool mouseOver(Rect r)
    {
        const platform::Input &in = platform::input;
        if (!inputAllowed() || !in.mouseInside)
            return false;
        r = r.offset(render::draw.dx, render::draw.dy);
        const Rect clip = render::currentClip().offset(render::draw.dx, render::draw.dy);
        return r.contains(in.mouseX, in.mouseY) && clip.contains(in.mouseX, in.mouseY);
    }

    void blockInput(Rect r)
    {
        if (mouseOver(r))
            state.hotNext = 1; // an id no widget has: nothing drawn before it is hovered
    }

    Interaction interact(Id wid, Rect r, bool focusable, bool enabled)
    {
        Interaction out;
        Widget &w = widget(wid);
        out.w = &w;
        const platform::Input &in = platform::input;
        const bool allowed = enabled && inputAllowed();
        const bool inside = allowed && mouseOver(r);
        if (inside)
            state.hotNext = wid;
        out.hovered = inside && state.hot == wid;
        if (focusable && allowed)
            state.focusOrder.push_back(wid);
        if (out.hovered && in.pressed[platform::kMouseLeft])
        {
            state.active = wid;
            state.focus = focusable ? wid : state.focus;
            out.pressed = true;
        }
        out.held = state.active == wid && in.down[platform::kMouseLeft] && allowed;
        if (state.active == wid && in.released[platform::kMouseLeft])
        {
            out.clicked = out.hovered;
            state.active = 0;
        }
        if (!allowed && state.active == wid)
            state.active = 0;
        out.rightClicked = out.hovered && in.pressed[platform::kMouseRight];
        out.focused = state.focus == wid && allowed;
        if (out.focused && state.focusVisible && (in.key(platform::kKeyEnter) || in.key(platform::kKeySpace)))
        {
            out.clicked = true;
            w.press = 1; // a flash of the pressed look
        }
        if (w.fresh)
            w.hover = w.press = w.focus = 0;
        animate(w.hover, out.hovered ? 1.0f : 0.0f, out.hovered ? 22.0f : 12.0f);
        animate(w.press, out.held ? 1.0f : 0.0f, out.held ? 40.0f : 14.0f);
        animate(w.focus, out.focused && state.focusVisible ? 1.0f : 0.0f, 20.0f);
        if (out.hovered)
            state.cursor = platform::Cursor::Hand;
        return out;
    }

    void requestFocus(Id id) { state.focusRequest = id; }

    void beginLayer(int layer, bool blocking)
    {
        g_layerStack.push_back(state.layer);
        state.layer = layer;
        if (blocking)
            state.topLayerNext = std::max(state.topLayerNext, layer);
    }

    void endLayer()
    {
        if (g_layerStack.empty())
            return;
        state.layer = g_layerStack.back();
        g_layerStack.pop_back();
    }

    void pushDisabled() { state.disabled++; }
    void popDisabled() { state.disabled = std::max(0, state.disabled - 1); }

    void setCursor(platform::Cursor cursor) { state.cursor = cursor; }

    void tooltip(Id wid, Rect anchor, std::string_view text)
    {
        if (state.hot != wid || text.empty() || !inputAllowed())
            return;
        if (state.tooltipId != wid)
        {
            state.tooltipId = wid;
            state.tooltipSince = state.wall;
        }
        state.tooltipText = std::string(text);
        state.tooltipAnchor = anchor.offset(render::draw.dx, render::draw.dy);
    }
}
