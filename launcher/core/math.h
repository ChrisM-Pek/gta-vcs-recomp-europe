// Small helpers every layer shares: rectangles and rect cutting, sRGB colours, frame-rate independent animation.
// Nothing here knows about the OS, the GPU or widgets.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace core
{
    constexpr float kPi = 3.14159265358979f;

    inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
    inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

    struct Rect
    {
        float x = 0, y = 0, w = 0, h = 0;

        float right() const { return x + w; }
        float bottom() const { return y + h; }
        float cx() const { return x + w * 0.5f; }
        float cy() const { return y + h * 0.5f; }
        bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
        Rect inset(float a) const { return {x + a, y + a, std::max(w - 2 * a, 0.0f), std::max(h - 2 * a, 0.0f)}; }
        Rect inset(float ax, float ay) const { return {x + ax, y + ay, std::max(w - 2 * ax, 0.0f), std::max(h - 2 * ay, 0.0f)}; }
        Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
        // grown (or shrunk) about its centre by a factor
        Rect scaled(float s) const { return {cx() - w * s * 0.5f, cy() - h * s * 0.5f, w * s, h * s}; }
    };

    inline Rect intersect(const Rect &a, const Rect &b)
    {
        const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
        const float x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
        return {x0, y0, std::max(x1 - x0, 0.0f), std::max(y1 - y0, 0.0f)};
    }

    // Rect cutting: layout by slicing pieces off a rectangle's edges. Each returns the piece and shrinks `r` by it.
    inline Rect cutLeft(Rect &r, float a)
    {
        a = std::min(a, r.w);
        Rect piece{r.x, r.y, a, r.h};
        r.x += a;
        r.w -= a;
        return piece;
    }
    inline Rect cutRight(Rect &r, float a)
    {
        a = std::min(a, r.w);
        r.w -= a;
        return {r.x + r.w, r.y, a, r.h};
    }
    inline Rect cutTop(Rect &r, float a)
    {
        a = std::min(a, r.h);
        Rect piece{r.x, r.y, r.w, a};
        r.y += a;
        r.h -= a;
        return piece;
    }
    inline Rect cutBottom(Rect &r, float a)
    {
        a = std::min(a, r.h);
        r.h -= a;
        return {r.x, r.y + r.h, r.w, a};
    }
    // a w x h rectangle centred in r
    inline Rect centred(const Rect &r, float w, float h) { return {r.cx() - w * 0.5f, r.cy() - h * 0.5f, w, h}; }

    // Colours are sRGB, 8 bits per channel, packed 0xAABBGGRR so the bytes in memory are R, G, B, A: what the GPU
    // reads as R8G8B8A8_UNORM. Written in code as rgb(0xRRGGBB).
    using Color = uint32_t;

    constexpr Color rgb(uint32_t hex, uint32_t alpha = 255)
    {
        return ((hex >> 16) & 0xFF) | (hex & 0xFF00) | ((hex & 0xFF) << 16) | (alpha << 24);
    }
    inline float alphaOf(Color c) { return float(c >> 24) / 255.0f; }
    inline Color withAlpha(Color c, float a)
    {
        return (c & 0x00FFFFFFu) | (uint32_t(clamp01(a) * 255.0f + 0.5f) << 24);
    }
    // multiplies the alpha (fading a colour that may already be translucent)
    inline Color fade(Color c, float a) { return withAlpha(c, alphaOf(c) * a); }
    inline Color mix(Color a, Color b, float t)
    {
        t = clamp01(t);
        Color out = 0;
        for (int i = 0; i < 4; ++i)
        {
            const float ca = float((a >> (i * 8)) & 0xFF), cb = float((b >> (i * 8)) & 0xFF);
            out |= uint32_t(ca + (cb - ca) * t + 0.5f) << (i * 8);
        }
        return out;
    }

    // ---- animation: by time, never by frame count, so motion is the same at 60 Hz or 240 Hz ------------------------

    // Exponential approach to target. `speed` in 1/s: after 1/speed seconds about 63% of the way is covered.
    inline float damp(float current, float target, float speed, float dt)
    {
        return target + (current - target) * std::exp(-speed * dt);
    }

    // A damped spring, for motion with a little overshoot and settle. Sub-stepped, so stable at any frame time.
    struct Spring
    {
        float value = 0, velocity = 0, target = 0;
        bool initialised = false;

        void snap(float v)
        {
            value = target = v;
            velocity = 0;
            initialised = true;
        }
        // stiffness ~ how fast; a damping ratio below 1 overshoots, 1 is critically damped. True while moving.
        bool update(float dt, float stiffness = 300.0f, float dampingRatio = 0.75f)
        {
            if (!initialised)
            {
                snap(target);
                return false;
            }
            const float damping = 2.0f * dampingRatio * std::sqrt(stiffness);
            float left = dt;
            while (left > 0)
            {
                const float h = std::min(left, 1.0f / 240.0f);
                const float accel = stiffness * (target - value) - damping * velocity;
                velocity += accel * h; // semi-implicit Euler
                value += velocity * h;
                left -= h;
            }
            if (std::fabs(target - value) < 0.0005f && std::fabs(velocity) < 0.005f)
            {
                value = target;
                velocity = 0;
                return false;
            }
            return true;
        }
    };

    inline float easeOutCubic(float t)
    {
        t = 1.0f - clamp01(t);
        return 1.0f - t * t * t;
    }
    inline float easeInOutCubic(float t)
    {
        t = clamp01(t);
        return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
    }
    // overshoots a little before settling (dialogs, pops)
    inline float easeOutBack(float t)
    {
        t = clamp01(t);
        const float c1 = 1.4f, c3 = c1 + 1;
        return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f);
    }
}
