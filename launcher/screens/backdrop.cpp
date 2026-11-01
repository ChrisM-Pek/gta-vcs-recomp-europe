// Behind everything: the sky and its glows, and on Home Vice City at dusk - a striped sun going down behind the towers
// of the beach, over a sea that moves (the app icon's motif, launcher/make_icon.py), all drawn with the renderer's own
// primitives. Alive while Home shows and the game isn't running: the sun's stripes slide toward the horizon, clouds
// come, drift and go, far-off jets draw trails that spread and fade, windows go on and off, the sea's lines roll in
// and the sun's reflection shimmers - at 30 frames a second (15 when the window isn't focused), slow enough to cost
// almost nothing; standing still with reduce motion on. Every other screen draws once and sleeps. (A later option: art
// from the player's own disc in this slot - docs/LAUNCHER.md.)
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

        // the towers of the beach: art deco - setbacks, parapets, a spire
        struct Tower
        {
            float x;      // left edge, a fraction of the width
            float width;  // fractions of the scene's size
            float height;
            int style;    // 0 flat, 1 stepped, 2 a parapet, 3 spire
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
            case 2: // a parapet, narrower, and a slab on it
                block(S, body, c);
                block(S, {body.x + bw * 0.08f, body.y - bh * 0.035f, bw * 0.84f, bh * 0.035f + px(1)}, c);
                block(S, {body.x + bw * 0.38f, body.y - bh * 0.09f, bw * 0.24f, bh * 0.055f + px(1)}, c);
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

        // a flight's own random number for one of its traits: the flight and the trait mixed apart first, so neighbouring
        // flights (and a flight's traits) don't come out alike
        float trait(int flight, uint32_t which)
        {
            return hash(uint32_t(flight) * 0x9E3779B9u ^ (which * 0x85EBCA6Bu + 0x27D4EB2Fu));
        }

        // how much of a trail to keep where it crosses behind the words on the left or the title bar (they stay easy to
        // read; a jet coming down from the top seems to come out from under the bar)
        float behindWords(const Scene &S, float x, float y)
        {
            const float inside = (1.0f - smooth(S.w * 0.50f, S.w * 0.58f, x)) * smooth(S.h * 0.17f, S.h * 0.23f, y) *
                                 (1.0f - smooth(S.h * 0.56f, S.h * 0.63f, y));
            const float bar = px(theme::kTitleBar);
            return (1.0f - 0.7f * inside) * (0.15f + 0.85f * smooth(bar * 0.8f, bar * 1.5f, y));
        }

        // jets crossing high up, far away: a speck drawing a trail that starts thin and bright behind it, then spreads,
        // drifts and fades. A flight may set off in each 24 s slot, at a random moment in it (some slots stay empty).
        // Each flies a line of its own: through a random point of the upper sky at up to 24 degrees either way, so it
        // comes in from the left or right edge or down from the top and leaves by another edge or behind the skyline.
        // Slots pair up flying opposite ways (in random order); each flight also has its own pace, distance (a far one
        // is smaller, fainter, slower), how long its trail lasts and how patchy it is - all from its number, so nothing
        // is stored. Usually one to three trails are in the sky
        void jets(const Scene &S)
        {
            constexpr float kSlot = 24;
            const float dark = theme::darkness;
            const int last = int(std::floor(S.t / kSlot));
            for (int k = last - 6; k <= last; ++k)
            {
                if (trait(k, 0) < 0.15f)
                    continue; // a quiet spell
                const float start = float(k) * kSlot + trait(k, 1) * 20.0f;
                const float far = trait(k, 2); // 0 near .. 1 far
                // its line: a point it passes and an angle; it enters and leaves where that line meets the edges of the
                // window (a little outside them) or goes down behind the skyline
                const int pair = k >= 0 ? k / 2 : (k - 1) / 2;
                const bool rightward = (trait(pair, 5) < 0.5f) != (k - pair * 2 == 1);
                const float angle = (trait(k, 6) * 2.0f - 1.0f) * 24.0f * 3.14159265f / 180.0f;
                const float dx = std::cos(angle) * (rightward ? 1.0f : -1.0f), dy = std::sin(angle);
                const float px0 = S.w * (0.10f + 0.80f * trait(k, 7)), py0 = S.h * (0.08f + 0.24f * trait(k, 12));
                float lo = -1e9f, hi = 1e9f;
                auto slab = [&](float from, float d, float a, float b) {
                    if (std::fabs(d) < 1e-6f)
                        return;
                    const float t0 = (a - from) / d, t1 = (b - from) / d;
                    lo = std::max(lo, std::min(t0, t1));
                    hi = std::min(hi, std::max(t0, t1));
                };
                slab(px0, dx, -0.05f * S.w, 1.05f * S.w);
                slab(py0, dy, -0.04f * S.h, S.horizon - S.s * 0.05f);
                const float ax = px0 + dx * lo, ay = py0 + dy * lo, pathLen = hi - lo;
                const float speed = S.w * 1.1f / ((30.0f + 22.0f * trait(k, 3)) * (0.9f + 0.3f * far)); // pixels a second
                const float cross = pathLen / speed;                                                     // seconds
                const float fade = 22.0f + 18.0f * trait(k, 4); // how long its trail lasts
                const float since = S.t - start;
                if (since < 0 || since > cross + fade || pathLen <= 0)
                    continue;
                const float size = 1.25f - 0.5f * far, strength = (0.85f - 0.35f * dark) * (1.0f - 0.35f * far);
                const float patchy = 0.6f * trait(k, 8), patchFreq = 8.0f + 14.0f * trait(k, 9), patchPhase = 6.28f * trait(k, 10);
                const float jet = std::min(since / cross, 1.0f);

                // the trail: soft dots along the line, wider, fainter and lower the longer ago the jet passed
                float u = 0;
                while (u < jet)
                {
                    const float age = since - u * cross; // seconds since the jet was here
                    const float life = 1.0f - smooth(fade * 0.3f, fade, age);
                    const float width = (px(1.4f) + age * px(0.45f)) * size;
                    const float step = std::max(px(2.5f), width * 0.55f);
                    if (age > 0.35f && life > 0.01f) // it forms a little behind the jet
                    {
                        const float x = ax + dx * pathLen * u, y = ay + dy * pathLen * u + age * px(0.12f);
                        const float patch = 1.0f - patchy * (0.5f + 0.5f * std::sin(u * patchFreq + patchPhase));
                        const float a = strength * life * patch * behindWords(S, x, y) * smooth(0.35f, 1.2f, age) *
                                        std::min(1.0f, step / width * 1.6f);
                        shadow({x - width * 0.5f, y - width * 0.5f, width, width}, withAlpha(theme::kContrail, a), width * 0.35f, width * 0.5f);
                    }
                    u += step / pathLen;
                }
                // the jet itself, a speck catching the sun, its strobe blinking at night
                if (since < cross)
                {
                    const float x = ax + dx * pathLen * jet, y = ay + dy * pathLen * jet;
                    circle(x, y, px(1.1f) * size, withAlpha(theme::kContrail, 0.9f));
                    if (dark > 0.01f && std::fmod(S.t + trait(k, 11), 1.3f) < 0.08f)
                    {
                        shadow({x - px(3), y - px(3), px(6), px(6)}, withAlpha(theme::kText, 0.5f * dark), px(1.5f), px(3));
                        circle(x, y, px(1.0f) * size, withAlpha(theme::kText, dark));
                    }
                }
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
            const float from = std::round(cy - radius * 0.05f), span = S.horizon - from;
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

        // streaks across the sun, coming and going: one appears every 10 s or so at a random place around the sun,
        // fades in, drifts right for 35-55 s and fades out, so three to five are in the sky at any time and the sky never
        // runs out of them. Each has its own height, length, thickness, pace and maybe a thinner one riding above it
        // (traits from its number, as the jets'). Dark with a lit edge at night, white by day; faded out over the words
        void clouds(const Scene &S, float cx, float radius)
        {
            constexpr float kSlot = 10, kLongest = 55;
            const float dark = theme::darkness;
            const int last = int(std::floor(S.t / kSlot));
            for (int k = last - int(kLongest / kSlot) - 1; k <= last; ++k)
            {
                const float start = float(k) * kSlot + trait(k, 20) * kSlot;
                const float life = 35.0f + 20.0f * trait(k, 21);
                const float since = S.t - start;
                if (since < 0 || since > life)
                    continue;
                const float shown = smooth(0.0f, 6.0f, since) * (1.0f - smooth(life - 9.0f, life, since));
                const float len = S.s * (0.16f + 0.24f * trait(k, 22));
                const float thick = std::max(S.s * (0.0045f + 0.0035f * trait(k, 23)), px(2.5f));
                const float y = S.h * (0.36f + 0.25f * trait(k, 24));
                const float x = cx - radius * 1.5f + radius * 2.3f * trait(k, 25) - len * 0.5f + since * px(3.0f + 5.0f * trait(k, 26));
                const float a = shown * smooth(S.w * 0.50f, S.w * 0.64f, x + len * 0.5f);
                if (a <= 0.01f)
                    continue;
                // a crisp streak, maybe a thinner one riding above it, in a faint haze
                shadow({x - len * 0.1f, y - thick, len * 1.2f, thick * 3}, withAlpha(theme::kCloud, 0.25f * a), thick * 1.2f, thick);
                rect({x, y, len, thick}, withAlpha(theme::kCloud, 0.92f * a), thick * 0.5f);
                if (trait(k, 27) < 0.6f)
                {
                    const float rx = x + len * (0.1f + 0.3f * trait(k, 28)), rw = len * (0.3f + 0.25f * trait(k, 29));
                    rect({rx, y - thick * 1.7f, rw, thick * 0.7f}, withAlpha(theme::kCloud, 0.85f * a), thick * 0.35f);
                }
                if (dark > 0.01f)
                    rect({x + len * 0.08f, y + thick - px(1), len * 0.84f, px(1)}, withAlpha(theme::kPinkHi, 0.30f * dark * a), px(0.5f));
            }
        }

        // a building's windows: each goes on or off now and then, on its own schedule. `scale` shrinks the grid (the far
        // row), `strength` dims it
        void windows(const Scene &S, Rect body, uint32_t building, float scale, float strength)
        {
            const float ww = px(2) * scale, wh = px(2.6f) * scale, gx = px(5) * scale, gy = px(6.5f) * scale;
            const float top = px(8) * scale;
            const int cols = int((body.w - px(6) * scale) / gx), rows = int((body.h - top - px(4) * scale) / gy);
            if (cols <= 0 || rows <= 0)
                return;
            const float x0 = body.x + (body.w - float(cols - 1) * gx - ww) * 0.5f;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                {
                    const uint32_t id = building * 977 + uint32_t(r) * 31 + uint32_t(c);
                    const float epoch = std::floor(S.t / 50.0f + hash(id * 3) * 50.0f);
                    if (hash(id * 5 + uint32_t(epoch) * 7919) > 0.3f)
                        continue;
                    rect({x0 + float(c) * gx, body.y + top + float(r) * gy, ww, wh},
                         withAlpha(theme::kLights, strength * (0.45f + 0.4f * theme::darkness) * (0.6f + 0.4f * hash(id * 9))));
                }
        }

        // the towers; once on the sky, once mirrored in the sea (lights off, sunk toward the sea's colour by `faded`:
        // opaque, so where pieces overlap nothing shows darker)
        void skyline(const Scene &S, bool lights, float faded = 0)
        {
            const Color near = mix(theme::kSkyline, theme::kSea, faded);
            // far towers, lower and paler in the haze, their windows smaller and dimmer
            const Color far = mix(mix(theme::kSkyline, theme::kHorizon, 0.55f), theme::kSea, faded);
            float x = S.w * 0.30f;
            for (uint32_t i = 0; x < S.w; ++i)
            {
                const float bw = S.s * (0.022f + 0.03f * hash(i * 13 + 1));
                const float bh = S.s * (0.016f + 0.045f * hash(i * 13 + 2));
                const Rect body{x, S.horizon - bh, bw + px(1), bh + px(1)};
                block(S, body, far);
                if (lights)
                    windows(S, body, 5000 + i, 0.7f, 0.55f);
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
                if (lights)
                    windows(S, body, b, 1.0f, 1.0f);
            }
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

            // the towers upside down, faint, fading out with depth
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
        S.t = ui::state.motion > 0 ? float(std::fmod(ui::state.wall + shell.options.clock, 3600.0)) : 20.0f;

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
        jets(S); // in front of the sun; behind the cloud streaks and the towers
        clouds(S, cx, radius);
        skyline(S, true);
        sea(S, cx, radius);
        popOpacity();

        // the next frame of the scene; never while the game runs (it wants the GPU) or with reduce motion on
        if (alive && ui::state.motion > 0)
            ui::wakeAt(ui::state.wall + (platform::window.focused ? 1.0 / 30.0 : 1.0 / 15.0));
    }
}
