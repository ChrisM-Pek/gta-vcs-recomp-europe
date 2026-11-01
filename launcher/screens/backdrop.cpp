// Behind everything: the sky and its glows, and on Home Vice City at dusk - a striped sun going down behind palms and
// the towers of the beach, over a sea that moves (the app icon's motif, launcher/make_icon.py), all drawn with the
// renderer's own primitives. Alive while Home shows and the game isn't running: the sun's stripes slide toward the
// horizon, clouds drift, the palms sway, windows go on and off, the sea's lines roll in and the sun's reflection
// shimmers - at 30 frames a second (15 when the window isn't focused), slow enough to cost almost nothing, and still
// with reduce motion on. Every other screen draws once and sleeps. (A later option: art from the player's own disc in
// this slot - docs/LAUNCHER.md.)
#include "shell.h"

#include "platform/platform.h"

#include <cmath>
#include <cstdint>

namespace screens
{
    using namespace render;
    using core::mix;
    using core::withAlpha;

    namespace
    {
        constexpr float kPi = 3.14159265f;

        // the same "random" numbers every frame: a hash of an index into [0, 1)
        float hash(uint32_t n)
        {
            n ^= n >> 16;
            n *= 0x7FEB352Du;
            n ^= n >> 15;
            n *= 0x846CA68Bu;
            n ^= n >> 16;
            return float(n & 0xFFFFFF) / 16777216.0f;
        }

        float smooth(float a, float b, float x)
        {
            const float t = core::clamp01((x - a) / (b - a));
            return t * t * (3 - 2 * t);
        }

        struct Scene
        {
            float w = 0, h = 0, s = 0; // size, and the smaller side (what the scene scales with)
            float horizon = 0;
            float t = 0;               // the ambient clock, seconds
            bool mirror = false;       // drawing the reflection: y flips about the horizon
            float y(float v) const { return mirror ? 2 * horizon - v : v; }
        };

        // a rectangle standing on the scene (mirrored in the sea when reflecting)
        void block(const Scene &S, Rect r, Color c, Corners radius = {})
        {
            if (S.mirror)
            {
                r.y = 2 * S.horizon - r.y - r.h;
                radius = Corners(radius.bl, radius.br, radius.tr, radius.tl);
            }
            rect(r, c, radius);
        }

        // a band along a line of points, as convex quads between neighbours: palm trunks and fronds
        void ribbon(const Scene &S, const float *xs, const float *ys, const float *ws, int n, Color c)
        {
            float nx[24], ny[24];
            for (int i = 0; i < n; ++i)
            {
                const int a = i > 0 ? i - 1 : i, b = i + 1 < n ? i + 1 : i;
                const float dx = xs[b] - xs[a], dy = ys[b] - ys[a];
                const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-4f);
                nx[i] = -dy / len;
                ny[i] = dx / len;
            }
            for (int i = 0; i + 1 < n; ++i)
            {
                const float w0 = std::max(ws[i], px(0.35f)) * 0.5f, w1 = std::max(ws[i + 1], px(0.35f)) * 0.5f;
                // each piece reaches a little into the next, or their antialiased edges leave a hairline between them
                float dx = xs[i + 1] - xs[i], dy = ys[i + 1] - ys[i];
                const float len = std::max(std::sqrt(dx * dx + dy * dy), 1e-4f);
                dx *= px(0.7f) / len;
                dy *= px(0.7f) / len;
                const float ax = xs[i] - (i > 0 ? dx : 0), ay = ys[i] - (i > 0 ? dy : 0);
                const float bx = xs[i + 1] + (i + 2 < n ? dx : 0), by = ys[i + 1] + (i + 2 < n ? dy : 0);
                const float q[8] = {ax + nx[i] * w0, S.y(ay + ny[i] * w0),
                                    bx + nx[i + 1] * w1, S.y(by + ny[i + 1] * w1),
                                    bx - nx[i + 1] * w1, S.y(by - ny[i + 1] * w1),
                                    ax - nx[i] * w0, S.y(ay - ny[i] * w0)};
                quad(q, c);
            }
        }

        // a palm: a curved trunk thinning upward and a crown of drooping, notched fronds that sway a little
        void palm(const Scene &S, float baseX, float height, float lean, float phase, Color c)
        {
            const float sway = std::sin(S.t * 0.55f + phase) * height * 0.012f;
            const float topX = baseX + lean + sway, topY = S.horizon - height;
            const float ctrlX = baseX + lean * 0.12f, ctrlY = S.horizon - height * 0.55f;
            constexpr int kTrunk = 16;
            float xs[24], ys[24], ws[24];
            for (int i = 0; i < kTrunk; ++i)
            {
                const float u = float(i) / float(kTrunk - 1), v = 1 - u;
                xs[i] = v * v * baseX + 2 * v * u * ctrlX + u * u * topX;
                ys[i] = v * v * (S.horizon + px(2)) + 2 * v * u * ctrlY + u * u * topY;
                ws[i] = height * (0.046f - 0.022f * u) * (i % 2 ? 1.0f : 1.07f); // the rings of the bark
            }
            ribbon(S, xs, ys, ws, kTrunk, c);

            // the fronds, left to right; angles in degrees, 0 = right, -90 = up (y points down)
            static const float kAngles[] = {172, -168, -140, -110, -84, -58, -30, -8, 14};
            constexpr int kSeg = 14;
            for (int k = 0; k < int(std::size(kAngles)); ++k)
            {
                const float a = (kAngles[k] + 5.0f * std::sin(S.t * 0.9f + phase + float(k) * 1.3f)) * kPi / 180.0f;
                const float len = height * (0.40f + 0.12f * hash(uint32_t(k) * 7 + uint32_t(phase * 100)));
                const float droop = 0.62f;
                for (int i = 0; i < kSeg; ++i)
                {
                    const float u = float(i) / float(kSeg - 1);
                    xs[i] = topX + std::cos(a) * len * u;
                    ys[i] = topY + std::sin(a) * len * u + droop * len * u * u;
                    // a leaf's outline: widest a third of the way, notched along its length (the leaflets)
                    const float shape = std::pow(std::max(std::sin(kPi * std::min(u * 1.12f, 1.0f)), 0.0f), 0.75f);
                    ws[i] = height * 0.072f * shape * (0.82f + 0.18f * std::fabs(std::sin(u * kPi * 8.5f)));
                }
                ribbon(S, xs, ys, ws, kSeg, c);
            }
            circle(topX, S.y(topY + height * 0.015f), height * 0.032f, c); // the coconuts where the fronds meet
        }

        // the towers of the beach: art deco - setbacks, rounded crowns, a spire
        struct Tower
        {
            float x;      // left edge, a fraction of the width
            float width;  // fractions of the scene's size
            float height;
            int style;    // 0 flat, 1 stepped, 2 rounded, 3 spire
        };
        constexpr Tower kTowers[] = {
            {0.425f, 0.050f, 0.095f, 1}, {0.468f, 0.036f, 0.158f, 3}, {0.500f, 0.058f, 0.118f, 2},
            {0.551f, 0.040f, 0.072f, 0}, {0.583f, 0.048f, 0.092f, 1}, {0.900f, 0.046f, 0.084f, 2},
            {0.938f, 0.052f, 0.128f, 1}, {0.982f, 0.044f, 0.070f, 0},
        };

        void tower(const Scene &S, const Tower &T, Color c, Rect &body)
        {
            const float bw = T.width * S.s, bh = T.height * S.s;
            body = {T.x * S.w, S.horizon - bh, bw, bh + px(1)};
            switch (T.style)
            {
            case 1: // two setbacks
                block(S, body, c);
                block(S, {body.x + bw * 0.14f, body.y - bh * 0.08f, bw * 0.72f, bh * 0.08f + px(1)}, c);
                block(S, {body.x + bw * 0.30f, body.y - bh * 0.15f, bw * 0.40f, bh * 0.07f + px(1)}, c);
                break;
            case 2: // a rounded crown
                block(S, body, c, Corners(bw * 0.5f, bw * 0.5f, 0, 0));
                break;
            case 3: // a crown and a spire
                block(S, body, c);
                block(S, {body.x + bw * 0.2f, body.y - bh * 0.06f, bw * 0.6f, bh * 0.06f + px(1)}, c);
                block(S, {body.x + bw * 0.5f - px(1), body.y - bh * 0.30f, px(2), bh * 0.25f}, c);
                break;
            default:
                block(S, body, c);
            }
        }

        void sky(const Scene &S, float hero)
        {
            gradient({0, 0, S.w, S.h}, theme::kSky, theme::kNight);
            // the glow along the horizon, Home only
            if (hero > 0.01f)
                gradient({0, S.horizon - S.h * 0.42f, S.w, S.h * 0.42f + px(1)}, withAlpha(theme::kHorizon, 0),
                         withAlpha(theme::kHorizon, hero));
            // glows: a pink one up and to the right, a softer one low on the left
            shadow({S.w * 0.62f, -S.h * 0.25f, S.w * 0.5f, S.h * 0.55f}, withAlpha(theme::kPink, 0.09f + 0.05f * hero), S.s * 0.22f, S.s * 0.2f);
            shadow({-S.w * 0.2f, S.h * 0.65f, S.w * 0.55f, S.h * 0.5f}, withAlpha(theme::kPinkHi, 0.06f), S.s * 0.25f, S.s * 0.2f);
        }

        // a few stars in the dark, each twinkling at its own pace
        void stars(const Scene &S)
        {
            const float dark = theme::darkness;
            if (dark < 0.01f)
                return;
            for (uint32_t i = 0; i < 60; ++i)
            {
                const float x = hash(i * 3) * S.w, y = std::pow(hash(i * 3 + 1), 1.6f) * S.horizon * 0.62f;
                const float twinkle = 0.55f + 0.45f * std::sin(S.t * (0.6f + hash(i * 5) * 1.4f) + hash(i * 7) * 6.28f);
                const float a = (0.25f + 0.55f * hash(i * 3 + 2)) * twinkle * dark;
                circle(x, y, px(0.55f + 0.7f * hash(i * 11)), withAlpha(theme::kText, a));
            }
        }

        void sun(const Scene &S, float cx, float cy, float radius)
        {
            // its glow over the horizon, two layers
            shadow({cx - radius * 1.3f, cy - radius * 1.2f, radius * 2.6f, radius * 2.4f}, withAlpha(theme::kPinkHi, 0.16f), radius * 0.6f, radius);
            shadow({cx - radius * 1.05f, cy - radius * 1.05f, radius * 2.1f, radius * 2.1f}, withAlpha(theme::kPink, 0.22f), radius * 0.18f, radius);

            // the disc, solid above its middle, then in slices sliding down into the horizon; the slices thin and the
            // gaps between them widen toward it
            const Rect disc{cx - radius, cy - radius, radius * 2, radius * 2};
            const Color top = mix(theme::kPinkHi, theme::kOnPink, 0.22f), bottom = theme::kPink;
            const float from = cy - radius * 0.05f, span = S.horizon - from;
            pushClip({0, 0, S.w, S.horizon});
            pushClip({0, disc.y, S.w, from - disc.y});
            gradient(disc, top, bottom, radius);
            popClip();
            constexpr int kBands = 6;
            const float slide = S.t * 0.10f - std::floor(S.t * 0.10f);
            auto yAt = [&](float u) { return from + span * u * (1.35f - 0.35f * u); };
            for (int k = -1; k < kBands; ++k)
            {
                const float u0 = (float(k) + slide) / kBands, u1 = (float(k) + 1 + slide) / kBands;
                const float gap = 0.10f + 0.55f * core::clamp01((u0 + u1) * 0.5f);
                const float y0 = yAt(std::max(u0, 0.0f)), y1 = yAt(std::min(u0 + (u1 - u0) * (1 - gap), 1.0f));
                if (y1 <= y0)
                    continue;
                pushClip({0, y0, S.w, y1 - y0});
                gradient(disc, top, bottom, radius);
                popClip();
            }
            popClip();
        }

        // wisps drifting right across the sun's side of the sky, fading in as they leave the words behind: dark with a lit
        // edge at night, white by day
        void clouds(const Scene &S)
        {
            struct Cloud
            {
                float y, len, thick, speed, offset;
            };
            static const Cloud kClouds[] = {
                {0.385f, 0.30f, 0.0065f, 5.0f, 0.15f}, {0.455f, 0.20f, 0.005f, 7.5f, 0.70f},
                {0.535f, 0.38f, 0.0075f, 4.0f, 0.42f}, {0.60f, 0.24f, 0.005f, 6.0f, 0.92f},
            };
            const float dark = theme::darkness;
            const float from = S.w * 0.46f;
            for (const Cloud &c : kClouds)
            {
                const float len = c.len * S.s, thick = std::max(c.thick * S.s, px(2.5f));
                const float travel = S.w - from + len;
                const float x = from - len + std::fmod(c.offset * travel + S.t * px(c.speed), travel);
                const float a = smooth(S.w * 0.50f, S.w * 0.64f, x + len * 0.5f);
                if (a <= 0.01f)
                    continue;
                const float y = c.y * S.h;
                // a crisp streak with a thinner one riding above it, in a faint haze
                shadow({x - len * 0.1f, y - thick, len * 1.2f, thick * 3}, withAlpha(theme::kCloud, 0.25f * a), thick * 1.2f, thick);
                rect({x, y, len, thick}, withAlpha(theme::kCloud, 0.92f * a), thick * 0.5f);
                rect({x + len * 0.22f, y - thick * 1.7f, len * 0.45f, thick * 0.7f}, withAlpha(theme::kCloud, 0.85f * a), thick * 0.35f);
                if (dark > 0.01f)
                    rect({x + len * 0.08f, y + thick - px(1), len * 0.84f, px(1)}, withAlpha(theme::kPinkHi, 0.30f * dark * a), px(0.5f));
            }
        }

        // the towers and palms; once on the sky, once mirrored in the sea (lights off, sunk toward the sea's colour by
        // `faded`: opaque, so where pieces overlap nothing shows darker)
        void skyline(const Scene &S, bool lights, float faded = 0)
        {
            const Color near = mix(theme::kSkyline, theme::kSea, faded);
            // far towers, lower and paler in the haze
            const Color far = mix(mix(theme::kSkyline, theme::kHorizon, 0.55f), theme::kSea, faded);
            float x = S.w * 0.30f;
            for (uint32_t i = 0; x < S.w; ++i)
            {
                const float bw = S.s * (0.022f + 0.03f * hash(i * 13 + 1));
                const float bh = S.s * (0.016f + 0.045f * hash(i * 13 + 2));
                block(S, {x, S.horizon - bh, bw + px(1), bh + px(1)}, far, hash(i * 13 + 3) < 0.25f ? Corners(bw * 0.4f, bw * 0.4f, 0, 0) : Corners());
                x += bw;
            }

            float tallest = 0;
            Rect beacon{};
            for (uint32_t b = 0; b < std::size(kTowers); ++b)
            {
                Rect body;
                tower(S, kTowers[b], near, body);
                if (body.h > tallest)
                {
                    tallest = body.h;
                    beacon = body;
                }
                if (!lights)
                    continue;
                // windows: each goes on or off now and then, on its own schedule
                const float ww = px(2), wh = px(2.6f), gx = px(5), gy = px(6.5f);
                const int cols = int((body.w - px(6)) / gx), rows = int((body.h - px(12)) / gy);
                const float x0 = body.x + (body.w - float(cols - 1) * gx - ww) * 0.5f;
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols; ++c)
                    {
                        const uint32_t id = b * 977 + uint32_t(r) * 31 + uint32_t(c);
                        const float epoch = std::floor(S.t / 50.0f + hash(id * 3) * 50.0f);
                        if (hash(id * 5 + uint32_t(epoch) * 7919) > 0.3f)
                            continue;
                        rect({x0 + float(c) * gx, body.y + px(8) + float(r) * gy, ww, wh},
                             withAlpha(theme::kLights, (0.45f + 0.4f * theme::darkness) * (0.6f + 0.4f * hash(id * 9))));
                    }
            }
            palm(S, S.w * 0.615f, S.s * 0.36f, S.s * 0.05f, 0.0f, near);
            palm(S, S.w * 0.868f, S.s * 0.28f, -S.s * 0.04f, 2.1f, near);
            // the tallest tower's light, blinking
            if (lights && std::fmod(S.t, 1.6f) < 0.18f)
            {
                const float bx = beacon.cx(), by = beacon.y - beacon.h * 0.30f - px(1);
                shadow({bx - px(4), by - px(4), px(8), px(8)}, withAlpha(theme::kPinkHi, 0.6f), px(2.5f), px(4));
                circle(bx, by, px(1.5f), theme::kPinkHi);
            }
        }

        void sea(Scene &S, float cx, float radius)
        {
            const float top = S.horizon, depth = S.h - top;
            gradient({0, top, S.w, depth}, theme::kSea, theme::kNight);
            pushClip({0, top, S.w, depth});

            // the towers and palms upside down, faint, fading out with depth
            const float reach = depth * 0.45f;
            S.mirror = true;
            pushClip({0, top, S.w, reach});
            skyline(S, false, 0.8f);
            popClip();
            S.mirror = false;
            gradient({0, top, S.w, reach + px(1)}, withAlpha(theme::kSea, 0), mix(theme::kSea, theme::kNight, 0.45f));

            // the sun's light on the water: a soft column and short lines that waver
            shadow({cx - radius * 0.6f, top, radius * 1.2f, depth * 0.75f}, withAlpha(theme::kPink, 0.10f), radius * 0.3f, radius * 0.5f);
            for (int i = 0; i < 9; ++i)
            {
                const float fi = float(i);
                const float ly = top + px(7) + fi * px(9) * (1 + fi * 0.35f);
                if (ly > S.h)
                    break;
                const float rw = radius * (1.45f - 0.14f * fi) * (0.82f + 0.18f * std::sin(S.t * 1.1f + fi * 1.7f));
                const float dx = std::sin(S.t * 0.7f + fi * 2.3f) * px(5);
                rect({cx - rw * 0.5f + dx, std::round(ly), rw, px(2.5f)},
                     withAlpha(mix(theme::kPinkHi, theme::kPink, fi / 9.0f), 0.34f - 0.032f * fi), px(1.25f));
            }

            // lines on the water rolling in: close together at the horizon, spreading as they come
            constexpr int kLines = 11;
            const float roll = S.t * 0.05f;
            for (int i = 0; i < kLines; ++i)
            {
                float u = (float(i) + roll * kLines) / kLines;
                u -= std::floor(u);
                const float ly = top + depth * u * u;
                const float a = (0.13f * (1 - u) + 0.03f) * smooth(0.0f, 0.06f, u);
                rect({0, ly, S.w, std::max(1.0f, px(1))}, withAlpha(theme::kPink, a));
            }
            popClip();

            // the horizon, a line of light
            shadow({0, top - px(2), S.w, px(4)}, withAlpha(theme::kPink, 0.35f), px(3), 0);
            rect({0, top, S.w, std::max(1.0f, px(1))}, withAlpha(theme::kPink, 0.6f));
        }
    }

    void drawBackdrop(Rect r, float hero, bool alive)
    {
        Scene S;
        S.w = r.w;
        S.h = r.h;
        S.s = std::min(r.w, r.h);
        S.horizon = std::round(r.h * 0.70f);
        // the ambient clock: real time (a still frame with reduce motion on)
        S.t = ui::state.motion > 0 ? float(std::fmod(ui::state.wall, 3600.0)) : 20.0f;

        sky(S, hero);
        if (hero <= 0.01f)
            return;
        const float e = core::easeOutCubic(hero);
        const float radius = S.s * 0.24f;
        const float cx = S.w * 0.73f;
        const float cy = S.horizon - radius * (0.15f + 0.45f * e); // it rises a little as Home appears
        pushOpacity(hero);
        stars(S);
        sun(S, cx, cy, radius);
        clouds(S);
        skyline(S, true);
        sea(S, cx, radius);
        popOpacity();

        // the next frame of the scene; never while the game runs (it wants the GPU) or with reduce motion on
        if (alive && ui::state.motion > 0)
            ui::wakeAt(ui::state.wall + (platform::window.focused ? 1.0 / 30.0 : 1.0 / 15.0));
    }
}
