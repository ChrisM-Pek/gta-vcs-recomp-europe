#include "draw.h"

#include <cstring>

namespace render
{
    DrawList draw;

    namespace
    {
        std::vector<Instance> &target() { return draw.layer == Layer::Overlay ? draw.overlay : draw.instances; }

        // a new instance with the current clip, offset and opacity applied; null when clipped away entirely
        Instance *add(Kind kind, Rect r, float grow)
        {
            r.x += draw.dx;
            r.y += draw.dy;
            if (r.right() + grow <= draw.clip[0] || r.bottom() + grow <= draw.clip[1] || r.x - grow >= draw.clip[2] ||
                r.y - grow >= draw.clip[3] || draw.opacity <= 0.0f)
                return nullptr;
            Instance &i = target().emplace_back();
            std::memset(&i, 0, sizeof(i));
            i.rect[0] = r.x;
            i.rect[1] = r.y;
            i.rect[2] = r.right();
            i.rect[3] = r.bottom();
            std::memcpy(i.clip, draw.clip, sizeof(i.clip));
            i.kind = kind;
            return &i;
        }

        Color faded(Color c) { return draw.opacity >= 1.0f ? c : core::fade(c, draw.opacity); }
    }

    void beginFrame(float width, float height)
    {
        draw.instances.clear();
        draw.overlay.clear();
        draw.layer = Layer::Base;
        draw.width = width;
        draw.height = height;
        draw.clip[0] = draw.clip[1] = 0;
        draw.clip[2] = width;
        draw.clip[3] = height;
        draw.opacity = 1;
        draw.dx = draw.dy = 0;
        draw.clipStack.clear();
        draw.opacityStack.clear();
        draw.offsetStack.clear();
    }

    void endFrame()
    {
        draw.instances.insert(draw.instances.end(), draw.overlay.begin(), draw.overlay.end());
        draw.overlay.clear();
    }

    Layer setLayer(Layer layer)
    {
        const Layer previous = draw.layer;
        draw.layer = layer;
        return previous;
    }

    void pushClip(Rect r)
    {
        draw.clipStack.push_back({draw.clip[0], draw.clip[1], draw.clip[2], draw.clip[3]});
        r.x += draw.dx;
        r.y += draw.dy;
        draw.clip[0] = std::max(draw.clip[0], r.x);
        draw.clip[1] = std::max(draw.clip[1], r.y);
        draw.clip[2] = std::min(draw.clip[2], r.right());
        draw.clip[3] = std::min(draw.clip[3], r.bottom());
    }

    void pushClipAbsolute(Rect r)
    {
        draw.clipStack.push_back({draw.clip[0], draw.clip[1], draw.clip[2], draw.clip[3]});
        draw.clip[0] = r.x;
        draw.clip[1] = r.y;
        draw.clip[2] = r.right();
        draw.clip[3] = r.bottom();
    }

    void popClip()
    {
        if (draw.clipStack.empty())
            return;
        const auto c = draw.clipStack.back();
        draw.clipStack.pop_back();
        std::memcpy(draw.clip, c.data(), sizeof(draw.clip));
    }

    Rect currentClip()
    {
        return {draw.clip[0] - draw.dx, draw.clip[1] - draw.dy, std::max(draw.clip[2] - draw.clip[0], 0.0f),
                std::max(draw.clip[3] - draw.clip[1], 0.0f)};
    }

    void pushOpacity(float opacity)
    {
        draw.opacityStack.push_back(draw.opacity);
        draw.opacity *= core::clamp01(opacity);
    }

    void popOpacity()
    {
        if (draw.opacityStack.empty())
            return;
        draw.opacity = draw.opacityStack.back();
        draw.opacityStack.pop_back();
    }

    void pushOffset(float dx, float dy)
    {
        draw.offsetStack.emplace_back(draw.dx, draw.dy);
        draw.dx += dx;
        draw.dy += dy;
    }

    void popOffset()
    {
        if (draw.offsetStack.empty())
            return;
        draw.dx = draw.offsetStack.back().first;
        draw.dy = draw.offsetStack.back().second;
        draw.offsetStack.pop_back();
    }

    void rectEx(Rect r, Color from, Color to, Corners radius, Color border, float borderWidth, Gradient direction)
    {
        if (r.w <= 0 || r.h <= 0)
            return;
        Instance *i = add(kRect, r, 1);
        if (!i)
            return;
        i->radii[0] = radius.tl;
        i->radii[1] = radius.tr;
        i->radii[2] = radius.br;
        i->radii[3] = radius.bl;
        i->color0 = faded(from);
        i->color1 = faded(to);
        i->border = faded(border);
        i->params[0] = borderWidth;
        i->params[3] = direction == Gradient::Horizontal ? 1.0f : 0.0f;
    }

    void rect(Rect r, Color fill, Corners radius) { rectEx(r, fill, fill, radius, 0, 0); }

    void gradient(Rect r, Color from, Color to, Corners radius, Gradient direction)
    {
        rectEx(r, from, to, radius, 0, 0, direction);
    }

    void outline(Rect r, Color border, float width, Corners radius) { rectEx(r, 0, 0, radius, border, width); }

    void shadow(Rect r, Color color, float sigma, float radius)
    {
        Instance *i = add(kShadow, r, 3 * sigma + 1);
        if (!i)
            return;
        i->radii[0] = i->radii[1] = i->radii[2] = i->radii[3] = radius;
        i->color0 = faded(color);
        i->params[1] = sigma;
    }

    void line(float x0, float y0, float x1, float y1, float thickness, Color color)
    {
        const float pad = thickness * 0.5f + 1;
        const Rect bounds{std::min(x0, x1) - pad, std::min(y0, y1) - pad, std::fabs(x1 - x0) + 2 * pad,
                          std::fabs(y1 - y0) + 2 * pad};
        Instance *i = add(kLine, bounds, 0);
        if (!i)
            return;
        i->uv[0] = x0 + draw.dx;
        i->uv[1] = y0 + draw.dy;
        i->uv[2] = x1 + draw.dx;
        i->uv[3] = y1 + draw.dy;
        i->color0 = faded(color);
        i->params[2] = thickness;
    }

    void arc(float cx, float cy, float radius, float thickness, float start, float sweep, Color color)
    {
        if (sweep <= 0)
            return;
        const float outer = radius + thickness * 0.5f + 1;
        Instance *i = add(kArc, {cx - outer, cy - outer, outer * 2, outer * 2}, 0);
        if (!i)
            return;
        i->uv[0] = start;
        i->uv[1] = std::min(sweep, 2 * core::kPi);
        i->uv[2] = radius;
        i->uv[3] = thickness;
        i->color0 = faded(color);
    }

    void quad(const float corners[8], Color color)
    {
        float x0 = corners[0], y0 = corners[1], x1 = corners[0], y1 = corners[1];
        for (int k = 1; k < 4; ++k)
        {
            x0 = std::min(x0, corners[k * 2]);
            x1 = std::max(x1, corners[k * 2]);
            y0 = std::min(y0, corners[k * 2 + 1]);
            y1 = std::max(y1, corners[k * 2 + 1]);
        }
        Instance *i = add(kQuad, {x0, y0, x1 - x0, y1 - y0}, 1);
        if (!i)
            return;
        float *slots[4] = {&i->uv[0], &i->uv[2], &i->radii[0], &i->radii[2]};
        for (int k = 0; k < 4; ++k)
        {
            slots[k][0] = corners[k * 2] + draw.dx;
            slots[k][1] = corners[k * 2 + 1] + draw.dy;
        }
        i->color0 = faded(color);
    }

    void circle(float cx, float cy, float radius, Color color)
    {
        rect({cx - radius, cy - radius, radius * 2, radius * 2}, color, radius);
    }

    void glyph(Rect r, const float uv[4], Color color, float blur)
    {
        Instance *i = add(kGlyph, r, blur > 0 ? blur + 1 : 0);
        if (!i)
            return;
        std::memcpy(i->uv, uv, sizeof(i->uv));
        i->color0 = faded(color);
        i->params[1] = std::max(blur, 0.0f);
    }
}
